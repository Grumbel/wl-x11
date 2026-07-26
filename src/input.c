/* SPDX-FileCopyrightText: Copyright (c) 2026 Ingo Ruhnke <grumbel@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>

struct wlr_surface *surface_at_cursor(struct wlx_server *server,
		double *sx, double *sy) {
	struct wlr_scene_node *node = wlr_scene_node_at(&server->scene->tree.node,
		server->cursor->x, server->cursor->y, sx, sy);
	if (!node || node->type != WLR_SCENE_NODE_BUFFER) {
		return NULL;
	}
	struct wlr_scene_buffer *buffer = wlr_scene_buffer_from_node(node);
	struct wlr_scene_surface *scene_surface =
		wlr_scene_surface_try_from_buffer(buffer);
	if (!scene_surface) {
		return NULL;
	}
	return scene_surface->surface;
}

/* Returns true if enough time has passed since the last actual
 * xcb_configure_window() call that we should send another one now. */
bool drag_throttle_ready(struct wlx_server *server) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	long elapsed_ms = (now.tv_sec - server->drag_last_send_at.tv_sec) * 1000 +
		(now.tv_nsec - server->drag_last_send_at.tv_nsec) / 1000000;
	if (elapsed_ms < WLX_DRAG_THROTTLE_MS) {
		return false;
	}
	server->drag_last_send_at = now;
	return true;
}

#define WLX_MIN_WINDOW_SIZE 50

/* Called whenever we get any pointer motion at all while a drag is
 * active; throttled to avoid flooding xfwm4's asynchronous
 * ConfigureRequest handling with one call per raw motion event. Always
 * re-queries the real pointer position fresh (see
 * query_root_pointer_position()) rather than trusting any accumulated
 * state, so a single noisy sample can't corrupt anything beyond itself. */
void update_interactive_drag(struct wlx_server *server) {
	if (!server->move_win && !server->resize_win) {
		return;
	}
	if (!drag_throttle_ready(server)) {
		return;
	}

	int16_t px, py;
	if (!query_root_pointer_position(server, &px, &py)) {
		return;
	}

	if (server->move_win) {
		xcb_window_t target = configure_target_window(server->move_win);
		if (target == XCB_WINDOW_NONE) {
			return;
		}
		/* x/y are root coordinates of the outer (frame) origin — what
		 * ICCCM ConfigureRequest asks the WM to apply. */
		int x = px + server->move_offset_x - server->drag_correction_x;
		int y = py + server->move_offset_y - server->drag_correction_y;
		server->drag_last_requested_x = x;
		server->drag_last_requested_y = y;
		wlr_log(WLR_INFO, "[DIAG] pointer at (%d,%d) -> requesting window "
			"0x%x at (%d,%d) (correction (%d,%d))", px, py,
			target, x, y, server->drag_correction_x,
			server->drag_correction_y);
		uint32_t values[2] = { (uint32_t)x, (uint32_t)y };
		xcb_configure_window(server->xcb, target,
			XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, values);
		xcb_flush(server->xcb);
		return;
	}

	/* resize: position is outer/frame root coords; size is client size */
	xcb_window_t target = configure_target_window(server->resize_win);
	if (target == XCB_WINDOW_NONE) {
		return;
	}
	int dx = px - server->resize_start_pointer_x;
	int dy = py - server->resize_start_pointer_y;
	int x = server->resize_start_x;
	int y = server->resize_start_y;
	int w = server->resize_start_w;
	int h = server->resize_start_h;

	if (server->resize_edges & WLR_EDGE_LEFT) {
		x += dx;
		w -= dx;
	} else if (server->resize_edges & WLR_EDGE_RIGHT) {
		w += dx;
	}
	if (server->resize_edges & WLR_EDGE_TOP) {
		y += dy;
		h -= dy;
	} else if (server->resize_edges & WLR_EDGE_BOTTOM) {
		h += dy;
	}
	if (w < WLX_MIN_WINDOW_SIZE) {
		w = WLX_MIN_WINDOW_SIZE;
	}
	if (h < WLX_MIN_WINDOW_SIZE) {
		h = WLX_MIN_WINDOW_SIZE;
	}
	x -= server->drag_correction_x;
	y -= server->drag_correction_y;
	server->drag_last_requested_x = x;
	server->drag_last_requested_y = y;

	wlr_log(WLR_INFO, "[DIAG] pointer at (%d,%d) -> requesting window 0x%x "
		"at (%d,%d) %dx%d (correction (%d,%d))", px, py,
		target, x, y, w, h, server->drag_correction_x,
		server->drag_correction_y);
	uint32_t values[4] = {
		(uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h,
	};
	xcb_configure_window(server->xcb, target,
		XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
		XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, values);
	xcb_flush(server->xcb);
}

void reset_cursor_to_default(struct wlx_server *server) {
	if (server->cursor_mgr) {
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "left_ptr");
	}
}

void process_cursor_motion(struct wlx_server *server, uint32_t time_msec) {
	if (server->move_win || server->resize_win) {
		update_interactive_drag(server);
		return;
	}

	/* Outbound XDND tracks root position independently of Wayland hit-testing. */
	if (server->dnd_out_active) {
		dnd_out_update_position(server);
	}

	double sx = 0, sy = 0;
	struct wlr_surface *surface = surface_at_cursor(server, &sx, &sy);

	/* When the layout cursor is still on the parent output but the real
	 * X11 pointer is over a dialog, prefer the window under the pointer. */
	if (!surface) {
		struct wlx_window *under = window_at_root_pointer(server);
		if (under && under->toplevel &&
				pointer_coords_on_window(server, under, &sx, &sy)) {
			surface = under->toplevel->base->surface;
		}
	}

	if (surface) {
		wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
		wlr_seat_pointer_notify_motion(server->seat, time_msec, sx, sy);
	} else {
		wlr_seat_pointer_clear_focus(server->seat);
		reset_cursor_to_default(server);
	}
}

/* Client called wl_pointer.set_cursor. Honour it only when that client
 * currently has pointer focus (protocol rule). surface == NULL means
 * hide the cursor; otherwise the surface's buffer becomes the image via
 * the X11 backend's output-cursor path. */
void server_seat_request_cursor(struct wl_listener *listener, void *data) {
	struct wlx_server *server =
		wl_container_of(listener, server, request_set_cursor);
	struct wlr_seat_pointer_request_set_cursor_event *event = data;

	struct wlr_seat_client *focused =
		server->seat->pointer_state.focused_client;
	if (event->seat_client != focused) {
		return;
	}

	/* surface == NULL hides the cursor (protocol). Otherwise the X11
	 * backend installs the surface buffer as the window's X cursor. */
	wlr_cursor_set_surface(server->cursor, event->surface,
		event->hotspot_x, event->hotspot_y);
}

void server_cursor_motion(struct wl_listener *listener, void *data) {
	struct wlx_server *server = wl_container_of(listener, server, cursor_motion);
	struct wlr_pointer_motion_event *event = data;
	wlr_cursor_move(server->cursor, &event->pointer->base,
		event->delta_x, event->delta_y);
	process_cursor_motion(server, event->time_msec);
}

void server_cursor_motion_absolute(struct wl_listener *listener, void *data) {
	struct wlx_server *server =
		wl_container_of(listener, server, cursor_motion_absolute);
	struct wlr_pointer_motion_absolute_event *event = data;
	wlr_cursor_warp_absolute(server->cursor, &event->pointer->base,
		event->x, event->y);
	process_cursor_motion(server, event->time_msec);
}

struct wlx_window *window_from_surface(struct wlr_surface *surface) {
	struct wlr_xdg_surface *xdg_surface = wlr_xdg_surface_try_from_wlr_surface(surface);
	if (!xdg_surface || xdg_surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
		return NULL;
	}
	return xdg_surface->data;
}

void server_cursor_button(struct wl_listener *listener, void *data) {
	struct wlx_server *server = wl_container_of(listener, server, cursor_button);
	struct wlr_pointer_button_event *event = data;

	if ((uint32_t)event->state == (uint32_t)WLR_BUTTON_PRESSED) {
		/* Prefer the real X11 window under the pointer. Scene hit-testing
		 * alone activates the parent when the layout cursor has not yet
		 * moved onto the dialog's output slot. */
		struct wlx_window *win = window_at_root_pointer(server);
		double sx = 0, sy = 0;
		struct wlr_surface *surface = NULL;

		if (win && win->toplevel &&
				pointer_coords_on_window(server, win, &sx, &sy)) {
			surface = win->toplevel->base->surface;
			set_active_window(server, win);
			wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
		} else {
			surface = surface_at_cursor(server, &sx, &sy);
			if (surface) {
				struct wlx_window *from_scene = window_from_surface(surface);
				if (from_scene) {
					set_active_window(server, from_scene);
				}
				wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
			}
		}
	}

	wlr_seat_pointer_notify_button(server->seat, event->time_msec,
		event->button, event->state);

	if ((uint32_t)event->state != (uint32_t)WLR_BUTTON_PRESSED) {
		if (server->move_win) {
			wlr_log(WLR_INFO, "ending self-driven interactive move");
			server->move_win = NULL;
		}
		if (server->resize_win) {
			wlr_log(WLR_INFO, "ending self-driven interactive resize");
			server->resize_win = NULL;
		}
		dnd_out_on_button_release(server);
	}
}

void server_cursor_axis(struct wl_listener *listener, void *data) {
	struct wlx_server *server = wl_container_of(listener, server, cursor_axis);
	struct wlr_pointer_axis_event *event = data;
	wlr_seat_pointer_notify_axis(server->seat, event->time_msec,
		event->orientation, event->delta, event->delta_discrete,
		event->source, event->relative_direction);
}

void server_cursor_frame(struct wl_listener *listener, void *data) {
	struct wlx_server *server = wl_container_of(listener, server, cursor_frame);
	wlr_seat_pointer_notify_frame(server->seat);
}

void keyboard_modifiers(struct wl_listener *listener, void *data) {
	struct wlx_keyboard *kb = wl_container_of(listener, kb, modifiers);
	wlr_seat_set_keyboard(kb->server->seat, kb->wlr_keyboard);
	wlr_seat_keyboard_notify_modifiers(kb->server->seat, &kb->wlr_keyboard->modifiers);
}

void keyboard_key(struct wl_listener *listener, void *data) {
	struct wlx_keyboard *kb = wl_container_of(listener, kb, key);
	struct wlr_keyboard_key_event *event = data;
	wlr_seat_set_keyboard(kb->server->seat, kb->wlr_keyboard);
	wlr_seat_keyboard_notify_key(kb->server->seat, event->time_msec,
		event->keycode, event->state);
}

void keyboard_destroy(struct wl_listener *listener, void *data) {
	struct wlx_keyboard *kb = wl_container_of(listener, kb, destroy);
	wl_list_remove(&kb->modifiers.link);
	wl_list_remove(&kb->key.link);
	wl_list_remove(&kb->destroy.link);
	free(kb);
}

void server_new_input(struct wl_listener *listener, void *data) {
	struct wlx_server *server = wl_container_of(listener, server, new_input);
	struct wlr_input_device *device = data;

	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD: {
		struct wlr_keyboard *wlr_kb = wlr_keyboard_from_input_device(device);

		struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
		struct xkb_keymap *keymap =
			xkb_keymap_new_from_names(ctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
		wlr_keyboard_set_keymap(wlr_kb, keymap);
		xkb_keymap_unref(keymap);
		xkb_context_unref(ctx);
		wlr_keyboard_set_repeat_info(wlr_kb, 25, 600);

		struct wlx_keyboard *kb = calloc(1, sizeof(*kb));
		if (!kb) {
			wlr_log(WLR_ERROR, "out of memory allocating keyboard tracker");
			break;
		}
		kb->server = server;
		kb->wlr_keyboard = wlr_kb;
		kb->modifiers.notify = keyboard_modifiers;
		wl_signal_add(&wlr_kb->events.modifiers, &kb->modifiers);
		kb->key.notify = keyboard_key;
		wl_signal_add(&wlr_kb->events.key, &kb->key);
		kb->destroy.notify = keyboard_destroy;
		wl_signal_add(&device->events.destroy, &kb->destroy);

		wlr_seat_set_keyboard(server->seat, wlr_kb);
		server->have_keyboard = true;
		break;
	}
	case WLR_INPUT_DEVICE_POINTER:
		wlr_cursor_attach_input_device(server->cursor, device);
		server->have_pointer = true;
		break;
	default:
		break;
	}

	uint32_t caps = 0;
	if (server->have_pointer) {
		caps |= WL_SEAT_CAPABILITY_POINTER;
	}
	if (server->have_keyboard) {
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	}
	wlr_seat_set_capabilities(server->seat, caps);
}

/* ------------------------------------------------------------------- */
/* Text-only CLIPBOARD bridge (Wayland ↔ host X11)                      */
/* ------------------------------------------------------------------- */

