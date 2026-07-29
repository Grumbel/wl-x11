/* SPDX-FileCopyrightText: Copyright (c) 2026 Ingo Ruhnke <grumbel@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <linux/input-event-codes.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>

xcb_atom_t intern_atom(xcb_connection_t *c, const char *name) {
	xcb_intern_atom_cookie_t cookie =
		xcb_intern_atom(c, 0, strlen(name), name);
	xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(c, cookie, NULL);
	xcb_atom_t atom = reply ? reply->atom : XCB_ATOM_NONE;
	free(reply);
	return atom;
}

void query_root_children(xcb_connection_t *c, xcb_window_t root,
		xcb_window_t **out, int *n) {
	*out = NULL;
	*n = 0;
	if (!c || xcb_connection_has_error(c)) {
		return;
	}
	xcb_query_tree_cookie_t cookie = xcb_query_tree(c, root);
	xcb_query_tree_reply_t *reply = xcb_query_tree_reply(c, cookie, NULL);
	if (!reply) {
		return;
	}
	int len = xcb_query_tree_children_length(reply);
	xcb_window_t *children = xcb_query_tree_children(reply);
	if (len > 0) {
		xcb_window_t *copy = malloc(sizeof(xcb_window_t) * (size_t)len);
		memcpy(copy, children, sizeof(xcb_window_t) * (size_t)len);
		*out = copy;
		*n = len;
	}
	free(reply);
}

/* Force a round trip on our auxiliary connection so that, best-effort, the
 * window the backend just created on its own connection is visible to us
 * too before we query the tree again. */
void xcb_roundtrip(xcb_connection_t *c) {
	if (!c || xcb_connection_has_error(c)) {
		return;
	}
	free(xcb_get_input_focus_reply(c, xcb_get_input_focus(c), NULL));
}

void xwin_set_title(struct wlx_server *s, xcb_window_t w, const char *title) {
	if (!s->xcb || xcb_connection_has_error(s->xcb) || w == XCB_WINDOW_NONE) {
		return;
	}
	if (!title) {
		title = "";
	}
	xcb_change_property(s->xcb, XCB_PROP_MODE_REPLACE, w,
		s->atom_net_wm_name, s->atom_utf8_string, 8, strlen(title), title);
	xcb_change_property(s->xcb, XCB_PROP_MODE_REPLACE, w,
		XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, strlen(title), title);
	xcb_flush(s->xcb);
}

void xwin_set_class(struct wlx_server *s, xcb_window_t w, const char *app_id) {
	if (!s->xcb || xcb_connection_has_error(s->xcb) || w == XCB_WINDOW_NONE) {
		return;
	}
	if (!app_id || app_id[0] == '\0') {
		app_id = "wayland";
	}
	size_t n = strlen(app_id);
	size_t total = (n + 1) * 2;
	char *joined = malloc(total);
	memcpy(joined, app_id, n + 1);
	memcpy(joined + n + 1, app_id, n + 1);
	xcb_change_property(s->xcb, XCB_PROP_MODE_REPLACE, w,
		XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8, total, joined);
	free(joined);
	xcb_flush(s->xcb);
}

/* Motif WM hints: control host decorations. decorations=false clears
 * the decorations field so xfwm4/others draw no title bar or border. */
void xwin_set_motif_decorations(struct wlx_server *s, xcb_window_t w,
		bool decorations) {
	if (!s->xcb || xcb_connection_has_error(s->xcb) || w == XCB_WINDOW_NONE ||
			s->atom_motif_wm_hints == XCB_ATOM_NONE) {
		return;
	}
	/* struct { flags, functions, decorations, input_mode, status } */
	uint32_t hints[5] = {0};
	hints[0] = 1u << 1; /* MWM_HINTS_DECORATIONS */
	hints[2] = decorations ? (1u << 0) /* MWM_DECOR_ALL */ : 0;
	xcb_change_property(s->xcb, XCB_PROP_MODE_REPLACE, w,
		s->atom_motif_wm_hints, s->atom_motif_wm_hints, 32, 5, hints);
	xcb_flush(s->xcb);
}

/* ------------------------------------------------------------------- */
/* CSD button plumbing: move/resize/maximize/minimize/fullscreen        */
/*                                                                      */
/* Each toplevel is a real, WM-managed X11 window, so rather than       */
/* reimplementing interactive move/resize or maximize geometry          */
/* ourselves, we delegate to the host window manager via the standard  */
/* EWMH (_NET_WM_MOVERESIZE, _NET_WM_STATE) and ICCCM (WM_CHANGE_STATE) */
/* client-message mechanisms -- the same ones GTK/Qt use themselves     */
/* when they draw their own title bar on a plain X11 session. Any      */
/* resulting geometry change comes back to us naturally through the    */
/* existing ConfigureNotify-based resize handling.                      */
/* ------------------------------------------------------------------- */

void send_root_client_message(struct wlx_server *s, xcb_window_t window,
		xcb_atom_t type, uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3,
		uint32_t d4) {
	if (!s->xcb || xcb_connection_has_error(s->xcb) || window == XCB_WINDOW_NONE) {
		wlr_log(WLR_ERROR, "send_root_client_message: skipped (no xcb connection "
			"or invalid target window)");
		return;
	}
	xcb_client_message_event_t ev = {0};
	ev.response_type = XCB_CLIENT_MESSAGE;
	ev.format = 32;
	ev.window = window;
	ev.type = type;
	ev.data.data32[0] = d0;
	ev.data.data32[1] = d1;
	ev.data.data32[2] = d2;
	ev.data.data32[3] = d3;
	ev.data.data32[4] = d4;

	uint32_t mask = XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
		XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY;
	xcb_void_cookie_t cookie = xcb_send_event_checked(s->xcb, 0, s->xcb_root,
		mask, (const char *)&ev);
	xcb_generic_error_t *err = xcb_request_check(s->xcb, cookie);
	if (err) {
		wlr_log(WLR_ERROR,
			"send_root_client_message: X error code %d sending atom %u to "
			"window 0x%x via root 0x%x", err->error_code, type, window,
			s->xcb_root);
		free(err);
	} else {
		wlr_log(WLR_INFO,
			"send_root_client_message: sent atom %u to window 0x%x "
			"(data: %u %u %u %u %u)", type, window, d0, d1, d2, d3, d4);
	}
}

void send_net_wm_state(struct wlx_server *s, xcb_window_t w,
		uint32_t action, xcb_atom_t prop1, xcb_atom_t prop2) {
	send_root_client_message(s, w, s->atom_net_wm_state, action, prop1, prop2,
		1 /* source indication: normal application */, 0);
}

/* EWMH/ICCCM window-management messages must target the real client
 * window (content_xwin), not xfwm4's own decoration frame (xwin) -- see
 * find_content_window() for why. Falls back to xwin if we failed to
 * identify it, which will likely just be ignored by the WM but is
 * better than sending nowhere. */
xcb_window_t ewmh_target_window(struct wlx_window *win) {
	return win->content_xwin != XCB_WINDOW_NONE ? win->content_xwin : win->xwin;
}

/* Real root-relative position of a window's origin, robust regardless of
 * how deep it's nested (handles the WM's reparenting for us). */
bool query_window_root_position(struct wlx_server *s, xcb_window_t w,
		int16_t *x, int16_t *y) {
	if (!s->xcb || xcb_connection_has_error(s->xcb) || w == XCB_WINDOW_NONE) {
		return false;
	}
	xcb_translate_coordinates_cookie_t cookie =
		xcb_translate_coordinates(s->xcb, w, s->xcb_root, 0, 0);
	xcb_translate_coordinates_reply_t *reply =
		xcb_translate_coordinates_reply(s->xcb, cookie, NULL);
	if (!reply) {
		return false;
	}
	*x = reply->dst_x;
	*y = reply->dst_y;
	free(reply);
	return true;
}

/* Prefer the wlroots-owned content window; fall back to decoration frame. */
bool window_content_root_position(struct wlx_window *win, int16_t *x, int16_t *y) {
	if (!win || !win->server || !x || !y) {
		return false;
	}
	xcb_window_t target = XCB_WINDOW_NONE;
	if (win->output) {
		target = wlr_x11_output_get_window(win->output);
	}
	if (target == XCB_WINDOW_NONE) {
		target = win->content_xwin != XCB_WINDOW_NONE
			? win->content_xwin : win->xwin;
	}
	if (target == XCB_WINDOW_NONE) {
		return false;
	}
	return query_window_root_position(win->server, target, x, y);
}

bool query_window_geometry(struct wlx_server *s, xcb_window_t w,
		int *width, int *height) {
	if (!s->xcb || xcb_connection_has_error(s->xcb) || w == XCB_WINDOW_NONE) {
		return false;
	}
	xcb_get_geometry_cookie_t cookie = xcb_get_geometry(s->xcb, w);
	xcb_get_geometry_reply_t *reply = xcb_get_geometry_reply(s->xcb, cookie, NULL);
	if (!reply) {
		return false;
	}
	*width = reply->width;
	*height = reply->height;
	free(reply);
	return true;
}

/* Real, ground-truth root-relative pointer position, queried directly
 * from the X server -- bypasses wlroots' own cursor tracking entirely
 * (no output-layout clamping, no per-device normalization, no risk of
 * mixing data from multiple input devices). See the comment on struct
 * wlx_server's move_win field for why this matters. */
bool query_root_pointer_position(struct wlx_server *s, int16_t *x, int16_t *y) {
	if (!s->xcb || xcb_connection_has_error(s->xcb)) {
		return false;
	}
	xcb_query_pointer_cookie_t cookie = xcb_query_pointer(s->xcb, s->xcb_root);
	xcb_query_pointer_reply_t *reply = xcb_query_pointer_reply(s->xcb, cookie, NULL);
	if (!reply) {
		return false;
	}
	*x = reply->root_x;
	*y = reply->root_y;
	free(reply);
	return true;
}

/* ICCCM ConfigureRequest target: the real client window, not the WM
 * frame. The WM intercepts the request and moves/resizes the frame.
 * Falls back to xwin when content is unknown. */
xcb_window_t configure_target_window(struct wlx_window *win) {
	return ewmh_target_window(win);
}

/* Visual outer top-left: the decoration frame when the WM has reparented
 * us, otherwise the client window itself. ConfigureRequest x/y are the
 * desired root position of this outer corner (ICCCM §4.1.5), so we must
 * measure the frame — not the content origin below the titlebar — or the
 * window jumps by exactly the titlebar/border inset on the first update. */
xcb_window_t outer_position_window(struct wlx_window *win) {
	if (win->xwin != XCB_WINDOW_NONE) {
		return win->xwin;
	}
	return win->content_xwin;
}

struct wlx_window *window_from_xwin(struct wlx_server *server, xcb_window_t w) {
	struct wlx_window *win;
	wl_list_for_each(win, &server->windows, link) {
		if (win->xwin == w || win->content_xwin == w) {
			return win;
		}
		for (int i = 0; i < win->related_count; i++) {
			if (win->related[i] == w) {
				return win;
			}
		}
	}
	return NULL;
}

/* Host WM changed _NET_WM_STATE (titlebar maximize/fullscreen, etc.).
 * Mirror into xdg_toplevel so the client matches the host chrome. */
static void win_handle_net_wm_state_notify(struct wlx_server *server,
		struct wlx_window *win) {
	if (!win || !win->toplevel || !server->xcb ||
			server->atom_net_wm_state == XCB_ATOM_NONE) {
		return;
	}
	if (!win->initial_configure_sent) {
		return;
	}

	/* Prefer the client window; some WMs only set the property there.
	 * Fall back to the frame if the client has no _NET_WM_STATE yet. */
	xcb_window_t targets[2] = {
		ewmh_target_window(win),
		win->xwin,
	};

	bool maximized = false;
	bool fullscreen = false;
	bool got = false;
	for (int t = 0; t < 2; t++) {
		xcb_window_t target = targets[t];
		if (target == XCB_WINDOW_NONE) {
			continue;
		}
		if (t == 1 && targets[0] == targets[1]) {
			break;
		}
		xcb_get_property_cookie_t cookie = xcb_get_property(server->xcb, 0,
			target, server->atom_net_wm_state, XCB_ATOM_ATOM, 0, 64);
		xcb_get_property_reply_t *reply =
			xcb_get_property_reply(server->xcb, cookie, NULL);
		if (!reply) {
			continue;
		}
		if (reply->type == XCB_ATOM_ATOM && reply->format == 32) {
			xcb_atom_t *atoms = xcb_get_property_value(reply);
			int n = (int)reply->value_len;
			bool vert = false, horz = false;
			fullscreen = false;
			for (int i = 0; i < n; i++) {
				if (atoms[i] == server->atom_net_wm_state_maximized_vert) {
					vert = true;
				} else if (atoms[i] == server->atom_net_wm_state_maximized_horz) {
					horz = true;
				} else if (atoms[i] == server->atom_net_wm_state_fullscreen) {
					fullscreen = true;
				}
			}
			/* EWMH: fully maximized only when both vert and horz are set. */
			maximized = vert && horz;
			got = true;
			free(reply);
			break;
		}
		free(reply);
	}
	if (!got) {
		return;
	}

	bool cur_max = win->toplevel->current.maximized ||
		win->toplevel->pending.maximized;
	bool cur_fs = win->toplevel->current.fullscreen ||
		win->toplevel->pending.fullscreen;

	if (maximized != cur_max) {
		wlr_log(WLR_INFO, "host _NET_WM_STATE maximized=%d → xdg_toplevel",
			maximized);
		/* CSD drop-shadow goes away when maximized; clear so a concurrent
		 * host resize does not subtract the old margin from the configure. */
		if (maximized) {
			win->size_from_wm = true;
					win->csd_margin_w = 0;
			win->csd_margin_h = 0;
		} else {
			/* Allow surface_commit to grow the host for restored CSD
			 * shadows (otherwise buffer is clipped at right/bottom). */
			win->size_from_wm = false;
				}
		wlr_xdg_toplevel_set_maximized(win->toplevel, maximized);
	}
	if (fullscreen != cur_fs) {
		wlr_log(WLR_INFO, "host _NET_WM_STATE fullscreen=%d → xdg_toplevel",
			fullscreen);
		if (fullscreen) {
			win->size_from_wm = true;
					win->csd_margin_w = 0;
			win->csd_margin_h = 0;
		} else {
			win->size_from_wm = false;
				}
		wlr_xdg_toplevel_set_fullscreen(win->toplevel, fullscreen);
	}

	/* Resize often arrives before this property notify. Re-send the current
	 * host size without CSD margins so the client fills the maximized
	 * window (avoids empty right/bottom border). */
	if ((maximized || fullscreen) && win->output && win->output->width > 0 &&
			win->output->height > 0) {
		int lw = wlx_unscale_size(server, win->output->width);
		int lh = wlx_unscale_size(server, win->output->height);
		wlx_toplevel_set_size(win, lw, lh);
	}
}

void select_window_events(struct wlx_server *server, xcb_window_t w) {
	if (!server->xcb || xcb_connection_has_error(server->xcb) || w == XCB_WINDOW_NONE) {
		return;
	}
	/* FOCUS_CHANGE: host focus → Wayland activated.
	 * STRUCTURE_NOTIFY: ReparentNotify / diagnostic ConfigureNotify.
	 * PROPERTY_CHANGE: host _NET_WM_STATE (maximize/fullscreen) → xdg. */
	uint32_t mask = XCB_EVENT_MASK_FOCUS_CHANGE |
		XCB_EVENT_MASK_STRUCTURE_NOTIFY |
		XCB_EVENT_MASK_PROPERTY_CHANGE;
	xcb_change_window_attributes(server->xcb, w, XCB_CW_EVENT_MASK, &mask);
	xcb_flush(server->xcb);
}

/* Selects focus-change events on the given window AND every descendant
 * of it (recursively) -- these are xfwm4's own decoration widgets
 * (title bar, borders, buttons), created when it reparents our window
 * into a frame, not anything wlroots created. We record each one against
 * `win` so window_from_xwin() can find `win` regardless of which window
 * in the subtree a FocusIn/FocusOut actually arrives on. The default
 * cursor is deliberately NOT applied here (only to the content window,
 * once identified -- see find_content_window() and its caller): forcing
 * it across the whole subtree overrode xfwm4's own edge/corner resize
 * cursors on its decoration windows, which we have no business touching.
 * Resize is also deliberately NOT handled here -- see
 * output_request_state(), which uses wlroots' own (correct) mechanism
 * instead of us watching ConfigureNotify ourselves. */
void register_x11_window_subtree(struct wlx_window *win, xcb_window_t w) {
	struct wlx_server *server = win->server;

	select_window_events(server, w);
	wlr_log(WLR_INFO, "registered X11 window 0x%x for toplevel \"%s\"",
		w, win->toplevel && win->toplevel->title ? win->toplevel->title : "?");

	if (win->related_count < WLX_MAX_RELATED_WINDOWS) {
		win->related[win->related_count++] = w;
	} else {
		wlr_log(WLR_ERROR, "window subtree exceeds WLX_MAX_RELATED_WINDOWS, "
			"some descendants won't get focus handling");
	}

	xcb_window_t *children = NULL;
	int n = 0;
	query_root_children(server->xcb, w, &children, &n);
	for (int i = 0; i < n; i++) {
		register_x11_window_subtree(win, children[i]);
	}
	free(children);
}

/* After the host WM reparents, win->xwin is often the decoration frame
 * while content_xwin is the real client window from
 * wlr_x11_output_get_window(). EWMH/ICCCM messages (_NET_WM_STATE,
 * _NET_WM_MOVERESIZE, WM_CHANGE_STATE) must target the client window —
 * WMs key requests by the original client id. Among the registered
 * subtree, the content window is usually the largest-area descendant
 * (decoration widgets are comparatively small). */
xcb_window_t find_content_window(struct wlx_server *server, struct wlx_window *win) {
	xcb_window_t best = XCB_WINDOW_NONE;
	uint32_t best_area = 0;

	for (int i = 0; i < win->related_count; i++) {
		xcb_window_t w = win->related[i];
		if (w == XCB_WINDOW_NONE || w == win->xwin) {
			continue;
		}
		xcb_get_geometry_cookie_t cookie = xcb_get_geometry(server->xcb, w);
		xcb_get_geometry_reply_t *reply =
			xcb_get_geometry_reply(server->xcb, cookie, NULL);
		if (!reply) {
			continue;
		}
		uint32_t area = (uint32_t)reply->width * (uint32_t)reply->height;
		if (area > best_area) {
			best_area = area;
			best = w;
		}
		free(reply);
	}

	if (best != XCB_WINDOW_NONE) {
		wlr_log(WLR_INFO, "identified 0x%x as the real content window "
			"(largest descendant, area=%u) under frame 0x%x",
			best, best_area, win->xwin);
	} else {
		/* Normal before the host WM reparents us into a decoration frame
		 * (related[] only has the client window). Callers fall back to
		 * xwin; ReparentNotify will re-run discovery once a frame exists. */
		wlr_log(WLR_INFO, "no distinct content window under 0x%x yet "
			"(likely not reparented); using client window itself",
			win->xwin);
	}
	return best;
}

/* Earlier versions tried to detect X11-driven resizes
 * ourselves, via our own auxiliary XCB connection watching
 * ConfigureNotify on what we assumed was "the" window. Reading wlroots'
 * actual X11 backend source (backend/x11/output.c) revealed why that was
 * wrong: wlr_x11_output_create() creates exactly one window
 * (output->win). Everything else we were seeing in the subtree under
 * win->xwin -- title bar strip, side/corner borders, buttons -- is
 * created by the *window manager* (xfwm4) when it reparents our window
 * into a decorated frame. win->xwin (found via our root-children diff)
 * is that frame; the real wlroots-owned content window is a *child* of
 * it, sized to the frame's content area (frame size minus border/
 * titlebar overhead).
 *
 * wlr_output_commit_state()'s custom-mode path resizes output->win (the
 * content window) directly. Feeding it the *frame's* observed size told
 * the small inner content window to become as big as the whole decorated
 * frame, overshooting by the border/titlebar overhead -- which made
 * xfwm4 grow the frame again to fit the oversized child, which we'd
 * observe and repeat: the runaway-growth loop.
 *
 * wlroots already has the correct mechanism for this, and we just
 * weren't using it: its own X11 event handling watches output->win (the
 * right window) and, on a real resize, calls
 * wlr_output_send_request_state(), which emits wlr_output's
 * events.request_state signal for the compositor to accept. Accepting a
 * state that already matches is a documented no-op in the backend
 * (output_set_custom_mode() checks `width == output->win_width` and
 * returns immediately without touching X11 at all if so), so this can't
 * feed back on itself. This replaces all of our own ConfigureNotify-based
 * resize detection. */
void output_request_state(struct wl_listener *listener, void *data) {
	struct wlx_window *win = wl_container_of(listener, win, output_request_state);
	const struct wlr_output_event_request_state *event = data;

	int req_w = 0, req_h = 0;
	bool have_mode = false;
	if (event && event->state &&
			(event->state->committed & WLR_OUTPUT_STATE_MODE)) {
		have_mode = true;
		if (event->state->mode) {
			req_w = event->state->mode->width;
			req_h = event->state->mode->height;
		} else {
			req_w = event->state->custom_mode.width;
			req_h = event->state->custom_mode.height;
		}
	}

	int cur_w = win->output ? win->output->width : 0;
	int cur_h = win->output ? win->output->height : 0;

	wlr_log(WLR_INFO,
		"size: request_state cur=%dx%d req=%dx%d have_mode=%d "
		"size_from_wm=%d→1 last_conf=%dx%d committed=0x%x",
		cur_w, cur_h, req_w, req_h, (int)have_mode,
		(int)win->size_from_wm,
		win->last_client_conf_w, win->last_client_conf_h,
		event && event->state ? event->state->committed : 0);

	/* Host WM or user resized the X11 window — stop auto-fitting to
	 * client geometry on subsequent commits. */
	win->size_from_wm = true;
	/* Sync maximized/fullscreen from X before applying the size so
	 * output_commit does not subtract CSD margins on a maximize resize. */
	win_handle_net_wm_state_notify(win->server, win);
	wlr_output_commit_state(win->output, event->state);
	wlr_log(WLR_INFO, "size: request_state after commit output=%dx%d",
		win->output ? win->output->width : 0,
		win->output ? win->output->height : 0);
	if (win->output) {
		wlr_output_schedule_frame(win->output);
	}
}

void set_active_window(struct wlx_server *server, struct wlx_window *win) {
	if (server->focused_window == win) {
		return;
	}

	if (server->focused_window && server->focused_window->toplevel) {
		wlr_xdg_toplevel_set_activated(server->focused_window->toplevel, false);
	}

	server->focused_window = win;

	if (win && win->toplevel) {
		wlr_xdg_toplevel_set_activated(win->toplevel, true);
		struct wlr_keyboard *kb = wlr_seat_get_keyboard(server->seat);
		wlr_seat_keyboard_notify_enter(server->seat, win->toplevel->base->surface,
			kb ? kb->keycodes : NULL, kb ? kb->num_keycodes : 0,
			kb ? &kb->modifiers : NULL);
	} else {
		wlr_seat_keyboard_notify_clear_focus(server->seat);
	}
}


static bool xcb_event_on_popup_present(struct wlx_server *server, xcb_window_t w) {
	struct wlx_popup *pop;
	wl_list_for_each(pop, &server->popups, link) {
		if (pop->xpresent &&
				wlr_x11_present_window_get_xcb(pop->xpresent) == w) {
			return true;
		}
	}
	return false;
}

/* Core pointer events delivered during XGrabPointer (owner_events=false).
 * Drive the seat the same way XI2 motion/button would. */
static void handle_core_pointer_during_grab(struct wlx_server *server,
		uint8_t type, xcb_generic_event_t *event) {
	if (type == XCB_MOTION_NOTIFY) {
		xcb_motion_notify_event_t *ev = (xcb_motion_notify_event_t *)event;
		process_cursor_motion(server, ev->time);
		/* Clients (Qt) apply hover/highlight on wl_pointer.frame, not on
		 * motion alone. XI2 path emits frame from the backend; core grab
		 * path must do the same. */
		wlr_seat_pointer_notify_frame(server->seat);
		return;
	}
	if (type == XCB_BUTTON_PRESS || type == XCB_BUTTON_RELEASE) {
		xcb_button_press_event_t *ev = (xcb_button_press_event_t *)event;
		uint32_t button = 0;
		switch (ev->detail) {
		case 1: button = BTN_LEFT; break;
		case 2: button = BTN_MIDDLE; break;
		case 3: button = BTN_RIGHT; break;
		default:
			return; /* wheel etc. — ignore for grab dismiss */
		}
		enum wlr_button_state state = (type == XCB_BUTTON_PRESS)
			? WLR_BUTTON_PRESSED : WLR_BUTTON_RELEASED;
		if (state == WLR_BUTTON_PRESSED) {
			pointer_enter_surface_under_cursor(server);
			wlr_seat_pointer_notify_button(server->seat, ev->time,
				button, state);
		} else {
			wlr_seat_pointer_notify_button(server->seat, ev->time,
				button, state);
			pointer_enter_surface_under_cursor(server);
			if (server->popup_pointer_grab_pending &&
					server->seat->pointer_state.button_count == 0) {
				wlx_popup_update_pointer_grab(server);
			}
		}
		wlx_log_seat_buttons(server, "core-grab", button, (uint32_t)state);
		wlr_seat_pointer_notify_frame(server->seat);
	}
}

int handle_xcb_readable(int fd, uint32_t mask, void *data) {
	(void)fd;
	(void)mask;
	struct wlx_server *server = data;

	xcb_generic_event_t *event;
	while ((event = xcb_poll_for_event(server->xcb)) != NULL) {
		uint8_t type = event->response_type & ~0x80;
		/* Core pointer path while XGrabPointer is active (menu open). */
		if (server->popup_pointer_grabbed &&
				(type == XCB_BUTTON_PRESS || type == XCB_BUTTON_RELEASE ||
				 type == XCB_MOTION_NOTIFY)) {
			handle_core_pointer_during_grab(server, type, event);
			free(event);
			continue;
		}
		if (type == XCB_FOCUS_IN) {
			xcb_focus_in_event_t *fi = (xcb_focus_in_event_t *)event;
			/* Ignore grab transitions and pointer-only notifies; keep
			 * keyboard activation aligned with real host WM focus. */
			if (fi->mode != XCB_NOTIFY_MODE_NORMAL &&
					fi->mode != XCB_NOTIFY_MODE_WHILE_GRABBED) {
				/* skip */
			} else if (fi->detail == XCB_NOTIFY_DETAIL_POINTER ||
					fi->detail == XCB_NOTIFY_DETAIL_POINTER_ROOT ||
					fi->detail == XCB_NOTIFY_DETAIL_INFERIOR) {
				/* skip: not a keyboard-focus gain on this window */
			} else {
				struct wlx_window *win = window_from_xwin(server, fi->event);
				if (win) {
					wlr_log(WLR_DEBUG, "X11 FocusIn on 0x%x (mode %u detail %u)",
						fi->event, fi->mode, fi->detail);
					set_active_window(server, win);
					/* If the pointer is already over this window, re-enter
					 * so the client can set_cursor for the focused surface. */
					double sx = 0, sy = 0;
					if (win->toplevel &&
							pointer_coords_on_window(server, win, &sx, &sy)) {
						wlx_pointer_to_surface(server, &sx, &sy);
						if (!server->prefer_csd) {
							struct wlr_box geo =
								win->toplevel->base->current.geometry;
							if (geo.width > 0 && geo.height > 0) {
								sx += geo.x;
								sy += geo.y;
							}
						}
						reset_cursor_to_default(server);
						wlr_seat_pointer_notify_enter(server->seat,
							win->toplevel->base->surface, sx, sy);
					}
				}
			}
		} else if (type == XCB_FOCUS_OUT) {
			xcb_focus_out_event_t *fo = (xcb_focus_out_event_t *)event;
			/* detail == Inferior: focus moved to a child (still ours).
			 * mode Grab/Ungrab: temporary; ignore.
			 * Also do not clear when focus is merely moving between two
			 * of our toplevels — the matching FocusIn will set the new
			 * active window. */
			if (fo->mode != XCB_NOTIFY_MODE_NORMAL &&
					fo->mode != XCB_NOTIFY_MODE_WHILE_GRABBED) {
				/* skip grab churn */
			} else if (fo->detail == XCB_NOTIFY_DETAIL_INFERIOR ||
					fo->detail == XCB_NOTIFY_DETAIL_POINTER ||
					fo->detail == XCB_NOTIFY_DETAIL_POINTER_ROOT) {
				/* skip */
			} else {
				struct wlx_window *win = window_from_xwin(server, fo->event);
				if (win && server->focused_window == win) {
					struct wlx_window *under = window_at_root_pointer(server);
					if (under && under != win) {
						wlr_log(WLR_DEBUG, "X11 FocusOut on 0x%x — focus moving to "
							"another of our windows, not clearing", fo->event);
					} else if (under == win) {
						/* Spurious focus churn inside the same toplevel. */
					} else if (popup_at_root_pointer(server) ||
							window_has_mapped_popup(server, win)) {
						/* Keep keyboard focus while a menu is open; dismiss
						 * is driven by the X pointer grab + seat, not FocusOut. */
						wlr_log(WLR_DEBUG, "X11 FocusOut on 0x%x — popup active, "
							"keeping keyboard focus", fo->event);
					} else {
						wlr_log(WLR_DEBUG, "X11 FocusOut on 0x%x (mode %u detail %u)",
							fo->event, fo->mode, fo->detail);
						set_active_window(server, NULL);
					}
				}
			}
		} else if (type == XCB_REPARENT_NOTIFY) {
			/* WM reparented our window into a decoration frame (or the
			 * reverse). Update frame vs. content identity and re-walk the
			 * subtree so related[] tracks decoration widgets too. */
			xcb_reparent_notify_event_t *rn =
				(xcb_reparent_notify_event_t *)event;
			struct wlx_window *win = window_from_xwin(server, rn->window);
			if (win && win->xwin != XCB_WINDOW_NONE) {
				wlr_log(WLR_INFO, "X11 ReparentNotify for 0x%x (parent 0x%x) "
					"on toplevel \"%s\" — refreshing window subtree",
					rn->window, rn->parent,
					win->toplevel && win->toplevel->title ?
						win->toplevel->title : "?");
				/* Client was reparented under a new frame: the event
				 * window is the client, parent is the WM frame. */
				if (rn->parent != XCB_WINDOW_NONE &&
						rn->parent != server->xcb_root &&
						(rn->window == win->xwin ||
						 rn->window == win->content_xwin)) {
					win->content_xwin = rn->window;
					win->xwin = rn->parent;
				}
				win->related_count = 0;
				memset(win->related, 0, sizeof(win->related));
				register_x11_window_subtree(win, win->xwin);
				xcb_window_t found = find_content_window(server, win);
				if (found != XCB_WINDOW_NONE) {
					win->content_xwin = found;
				} else if (win->content_xwin == XCB_WINDOW_NONE) {
					win->content_xwin = win->xwin;
				}
				dnd_set_xdnd_aware(server, win->xwin);
				dnd_set_xdnd_aware(server, win->content_xwin);
				apply_transient_hints(win);
				xwin_set_motif_decorations(server, win->xwin, !server->prefer_csd);
				if (win->content_xwin != XCB_WINDOW_NONE) {
					xwin_set_motif_decorations(server,
						win->content_xwin, !server->prefer_csd);
				}
				/* Host WM often ignores Motif and still wraps us in a
				 * frame, shrinking the client area (e.g. 531x203 → ~531x176
				 * for a titlebar). Present then draws into a smaller
				 * drawable and the bottom/right of the buffer is clipped.
				 * Re-assert the buffer size on the content window, and if
				 * the geometry still disagrees, shrink our output to the
				 * real drawable so we stop presenting off the edge. */
				if (win->output && win->content_xwin != XCB_WINDOW_NONE &&
						server->xcb) {
					int want_w = win->output->width;
					int want_h = win->output->height;
					if (want_w > 0 && want_h > 0) {
						uint32_t vals[] = { (uint32_t)want_w, (uint32_t)want_h };
						xcb_configure_window(server->xcb, win->content_xwin,
							XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
							vals);
						xcb_flush(server->xcb);
					}
					xcb_get_geometry_cookie_t gc =
						xcb_get_geometry(server->xcb, win->content_xwin);
					xcb_get_geometry_reply_t *gr =
						xcb_get_geometry_reply(server->xcb, gc, NULL);
					if (gr) {
						int cw = gr->width, ch = gr->height;
						free(gr);
						if (cw > 0 && ch > 0 &&
								(cw != win->output->width ||
								 ch != win->output->height)) {
							wlr_log(WLR_INFO,
								"content window 0x%x is %dx%d after reparent "
								"(output %dx%d) — syncing output to drawable",
								win->content_xwin, cw, ch,
								win->output->width, win->output->height);
							resize_output_to(win, cw, ch);
							if (win->toplevel) {
								wlx_toplevel_set_size(win,
									wlx_unscale_size(server, cw),
									wlx_unscale_size(server, ch));
							}
						}
					}
				}
			}
		} else if (type == XCB_CONFIGURE_NOTIFY) {
			xcb_configure_notify_event_t *cn_all =
				(xcb_configure_notify_event_t *)event;
			struct wlx_window *cfg_win =
				window_from_xwin(server, cn_all->window);
			if (cfg_win) {
				/* Popup stage is OR in root coords — follow parent moves. */
				wlx_reposition_popups_for_window(cfg_win);
			}
			/* Learn the WM's systematic position discrepancy from real
			 * feedback (see the comment on struct wlx_server's
			 * drag_correction_x field) rather than assuming a fixed
			 * value, since it's WM-theme-specific. Match against the
			 * same window we actually configure (content when known). */
			if (server->move_win || server->resize_win) {
				xcb_configure_notify_event_t *cn =
					(xcb_configure_notify_event_t *)event;
				struct wlx_window *active =
					server->move_win ? server->move_win : server->resize_win;
				xcb_window_t target = ewmh_target_window(active);
				if (cn->window == target || cn->window == active->xwin) {
					/* ConfigureNotify x/y are parent-relative. For the
					 * client window parented under a frame this is the
					 * border inset, not a root-relative error — only
					 * learn correction when the event is on the frame
					 * (parent is root) or when we have no separate
					 * content window. */
					if (cn->window == active->xwin ||
							active->content_xwin == XCB_WINDOW_NONE ||
							active->content_xwin == active->xwin) {
						int new_correction_x =
							cn->x - server->drag_last_requested_x;
						int new_correction_y =
							cn->y - server->drag_last_requested_y;
						wlr_log(WLR_DEBUG, "[DIAG] real ConfigureNotify for "
							"dragged window 0x%x: pos=(%d,%d) size=%dx%d "
							"border=%d (learned correction now (%d,%d), "
							"was (%d,%d))", cn->window, cn->x, cn->y,
							cn->width, cn->height, cn->border_width,
							new_correction_x, new_correction_y,
							server->drag_correction_x,
							server->drag_correction_y);
						server->drag_correction_x = new_correction_x;
						server->drag_correction_y = new_correction_y;
					}
				}
			}
		} else if (type == XCB_SELECTION_REQUEST) {
			xcb_selection_request_event_t *sreq =
				(xcb_selection_request_event_t *)event;
			if (!dnd_handle_selection_request(server, sreq)) {
				clipboard_handle_selection_request(server, sreq);
			}
		} else if (type == XCB_SELECTION_NOTIFY) {
			xcb_selection_notify_event_t *snotify =
				(xcb_selection_notify_event_t *)event;
			dnd_handle_selection_notify(server, snotify);
			clipboard_handle_selection_notify(server, snotify);
		} else if (type == XCB_SELECTION_CLEAR) {
			clipboard_handle_selection_clear(server,
				(xcb_selection_clear_event_t *)event);
		} else if (type == XCB_PROPERTY_NOTIFY) {
			xcb_property_notify_event_t *pn =
				(xcb_property_notify_event_t *)event;
			if (pn->atom == server->atom_net_wm_state) {
				struct wlx_window *win = window_from_xwin(server, pn->window);
				if (win) {
					win_handle_net_wm_state_notify(server, win);
				}
			}
		} else if (type == XCB_CLIENT_MESSAGE) {
			dnd_handle_client_message(server,
				(xcb_client_message_event_t *)event);
		} else if (server->xfixes_ok &&
				type == (uint8_t)(server->xfixes_event_base +
					XCB_XFIXES_SELECTION_NOTIFY)) {
			xcb_xfixes_selection_notify_event_t *xn =
				(xcb_xfixes_selection_notify_event_t *)event;
			if (xn->selection == server->atom_clipboard) {
				server->clip_we_own_x11 =
					(xn->owner == server->clipboard_window);
				if (!server->clip_we_own_x11 &&
						xn->owner != XCB_WINDOW_NONE) {
					clipboard_request_from_x11(server, false);
				}
			} else if (xn->selection == server->atom_primary) {
				server->pri_we_own_x11 =
					(xn->owner == server->clipboard_window);
				if (!server->pri_we_own_x11 &&
						xn->owner != XCB_WINDOW_NONE) {
					clipboard_request_from_x11(server, true);
				}
			}
		}
		free(event);
	}
	return 0;
}

