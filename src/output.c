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

void output_frame(struct wl_listener *listener, void *data) {
	struct wlx_window *win = wl_container_of(listener, win, output_frame);
	if (!win->scene_output) {
		return;
	}
	wlr_scene_output_commit(win->scene_output, NULL);

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(win->scene_output, &now);
}

void output_commit(struct wl_listener *listener, void *data) {
	struct wlx_window *win = wl_container_of(listener, win, output_commit);
	(void)data;

	wlr_log(WLR_INFO, "output commit event (current size %dx%d, last known %dx%d)",
		win->output->width, win->output->height,
		win->last_output_width, win->last_output_height);

	int w = win->output->width;
	int h = win->output->height;
	if (w <= 0 || h <= 0) {
		return;
	}
	if (w == win->last_output_width && h == win->last_output_height) {
		return;
	}
	win->last_output_width = w;
	win->last_output_height = h;

	wlr_log(WLR_INFO, "X11 window resized to %dx%d, propagating to toplevel", w, h);
	if (win->toplevel) {
		wlr_xdg_toplevel_set_size(win->toplevel, w, h);
	}
}

void output_destroy(struct wl_listener *listener, void *data) {
	struct wlx_window *win = wl_container_of(listener, win, output_destroy);

	/* This fires both when the host WM closes the X11 window (wlroots'
	 * X11 backend treats that like unplugging a monitor) and when we
	 * ourselves call wlr_output_destroy() below. Ask the client to close
	 * gracefully either way. */
	if (win->toplevel) {
		wlr_xdg_toplevel_send_close(win->toplevel);
	}

	wl_list_remove(&win->output_frame.link);
	wl_list_remove(&win->output_destroy.link);
	wl_list_remove(&win->output_commit.link);
	wl_list_remove(&win->output_request_state.link);

	win->output = NULL;
	win->scene_output = NULL;
	win->l_output = NULL;
	win->xwin = XCB_WINDOW_NONE;
	/* Drop all cached X11 identities so a later remap cannot match stale
	 * Xids (focus, EWMH targets, move/resize) or overflow related[]. */
	win->content_xwin = XCB_WINDOW_NONE;
	win->related_count = 0;
	memset(win->related, 0, sizeof(win->related));

	if (win->server->move_win == win) {
		win->server->move_win = NULL;
	}
	if (win->server->resize_win == win) {
		win->server->resize_win = NULL;
	}
	if (win->server->focused_window == win) {
		/* X11 window is gone; drop activated/keyboard focus. Avoid
		 * calling set_active_window here (it would touch toplevel state
		 * while the surface may already be tearing down). */
		win->server->focused_window = NULL;
	}
}

#define WLX_DEFAULT_WIDTH 1024
#define WLX_DEFAULT_HEIGHT 720
#define WLX_MIN_OUTPUT_SIZE 32

/* Preferred size: xdg window geometry first (excludes client-side shadow /
 * transparent padding that would otherwise show as black without RGBA
 * compositing), then surface buffer size, then the compositor default. */
void toplevel_preferred_size(struct wlx_window *win, int *w_out, int *h_out) {
	int w = 0, h = 0;
	if (win->toplevel && win->toplevel->base) {
		struct wlr_box geo = win->toplevel->base->current.geometry;
		w = geo.width;
		h = geo.height;
		if (w <= 0 || h <= 0) {
			struct wlr_surface *surf = win->toplevel->base->surface;
			if (surf) {
				w = surf->current.width;
				h = surf->current.height;
			}
		}
	}
	if (w < WLX_MIN_OUTPUT_SIZE) {
		w = WLX_DEFAULT_WIDTH;
	}
	if (h < WLX_MIN_OUTPUT_SIZE) {
		h = WLX_DEFAULT_HEIGHT;
	}
	*w_out = w;
	*h_out = h;
}

void xwin_set_transient_for(struct wlx_server *s, xcb_window_t w,
		xcb_window_t parent) {
	if (!s->xcb || w == XCB_WINDOW_NONE || parent == XCB_WINDOW_NONE ||
			s->atom_wm_transient_for == XCB_ATOM_NONE) {
		return;
	}
	xcb_change_property(s->xcb, XCB_PROP_MODE_REPLACE, w,
		s->atom_wm_transient_for, XCB_ATOM_WINDOW, 32, 1, &parent);
}

void xwin_set_window_type_dialog(struct wlx_server *s, xcb_window_t w,
		bool dialog) {
	if (!s->xcb || w == XCB_WINDOW_NONE ||
			s->atom_net_wm_window_type == XCB_ATOM_NONE) {
		return;
	}
	xcb_atom_t type = dialog
		? s->atom_net_wm_window_type_dialog
		: s->atom_net_wm_window_type_normal;
	if (type == XCB_ATOM_NONE) {
		return;
	}
	xcb_change_property(s->xcb, XCB_PROP_MODE_REPLACE, w,
		s->atom_net_wm_window_type, XCB_ATOM_ATOM, 32, 1, &type);
}

void xwin_set_modal(struct wlx_server *s, xcb_window_t w, bool modal) {
	if (!s->xcb || w == XCB_WINDOW_NONE ||
			s->atom_net_wm_state_modal == XCB_ATOM_NONE) {
		return;
	}
	if (modal) {
		xcb_change_property(s->xcb, XCB_PROP_MODE_REPLACE, w,
			s->atom_net_wm_state, XCB_ATOM_ATOM, 32, 1,
			&s->atom_net_wm_state_modal);
	}
}

/* Apply ICCCM/EWMH transient + dialog hints so the host WM places this
 * window above its parent instead of cascading it elsewhere on the desktop. */
void apply_transient_hints(struct wlx_window *win) {
	struct wlx_server *server = win->server;
	if (!win->toplevel || win->xwin == XCB_WINDOW_NONE) {
		return;
	}

	struct wlr_xdg_toplevel *parent_tl = win->toplevel->parent;
	struct wlx_window *parent_win = NULL;
	if (parent_tl && parent_tl->base) {
		parent_win = parent_tl->base->data;
	}

	bool is_transient = parent_win != NULL;
	xcb_window_t parent_x = XCB_WINDOW_NONE;
	if (parent_win) {
		parent_x = parent_win->content_xwin != XCB_WINDOW_NONE
			? parent_win->content_xwin : parent_win->xwin;
	}

	/* ICCCM: WM_TRANSIENT_FOR on the client window (and frame for WMs
	 * that read it from the frame). */
	if (is_transient && parent_x != XCB_WINDOW_NONE) {
		xwin_set_transient_for(server, win->xwin, parent_x);
		if (win->content_xwin != XCB_WINDOW_NONE &&
				win->content_xwin != win->xwin) {
			xwin_set_transient_for(server, win->content_xwin, parent_x);
		}
		xwin_set_window_type_dialog(server, win->xwin, true);
		xwin_set_window_type_dialog(server, win->content_xwin, true);
		/* Treat transients as modal for the host WM (wlroots 0.20 has no
		 * xdg_toplevel.modal on the state struct yet). */
		xwin_set_modal(server, win->xwin, true);
		xwin_set_modal(server, win->content_xwin, true);
		wlr_log(WLR_INFO, "transient hints: 0x%x transient for 0x%x",
			win->xwin, parent_x);

		/* Best-effort placement: center on parent if we know both
		 * positions. The WM may override; without this, some WMs still
		 * park new windows at a cascade origin when hints arrive late. */
		int16_t px = 0, py = 0;
		int pw = 0, ph = 0;
		int cw = win->output ? win->output->width : 0;
		int ch = win->output ? win->output->height : 0;
		if (cw > 0 && ch > 0 &&
				query_window_root_position(server, parent_win->xwin, &px, &py) &&
				query_window_geometry(server, parent_win->xwin, &pw, &ph)) {
			int nx = (int)px + (pw - cw) / 2;
			int ny = (int)py + (ph - ch) / 2;
			if (nx < 0) {
				nx = 0;
			}
			if (ny < 0) {
				ny = 0;
			}
			xcb_window_t cfg = win->content_xwin != XCB_WINDOW_NONE
				? win->content_xwin : win->xwin;
			uint32_t values[2] = { (uint32_t)nx, (uint32_t)ny };
			xcb_configure_window(server->xcb, cfg,
				XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, values);
		}
	} else {
		xwin_set_window_type_dialog(server, win->xwin, false);
		xwin_set_window_type_dialog(server, win->content_xwin, false);
	}
	if (server->xcb) {
		xcb_flush(server->xcb);
	}
}

/* Which of our toplevels is under the host pointer, by real X11 window
 * identity — not the scene-graph cursor position. The layout cursor can
 * lag or stay on the parent output when the pointer moves to a sibling
 * X11 window (dialog), which previously made click-to-focus activate the
 * parent and grey out the dialog. */
struct wlx_window *window_at_root_pointer(struct wlx_server *server) {
	if (!server->xcb || xcb_connection_has_error(server->xcb)) {
		return NULL;
	}
	xcb_query_pointer_cookie_t cookie =
		xcb_query_pointer(server->xcb, server->xcb_root);
	xcb_query_pointer_reply_t *reply =
		xcb_query_pointer_reply(server->xcb, cookie, NULL);
	if (!reply) {
		return NULL;
	}
	xcb_window_t w = reply->child;
	free(reply);
	if (w == XCB_WINDOW_NONE) {
		return NULL;
	}
	/* Descend to the deepest child under the pointer, then walk back up
	 * until we recognise one of our frame/content/related windows. */
	for (int depth = 0; depth < 16; depth++) {
		int16_t px = 0, py = 0;
		if (!query_root_pointer_position(server, &px, &py)) {
			break;
		}
		xcb_translate_coordinates_cookie_t tc =
			xcb_translate_coordinates(server->xcb, server->xcb_root, w, px, py);
		xcb_translate_coordinates_reply_t *tr =
			xcb_translate_coordinates_reply(server->xcb, tc, NULL);
		if (!tr || tr->child == XCB_WINDOW_NONE) {
			free(tr);
			break;
		}
		w = tr->child;
		free(tr);
	}
	while (w != XCB_WINDOW_NONE && w != server->xcb_root) {
		struct wlx_window *win = window_from_xwin(server, w);
		if (win) {
			return win;
		}
		xcb_query_tree_cookie_t q = xcb_query_tree(server->xcb, w);
		xcb_query_tree_reply_t *tr = xcb_query_tree_reply(server->xcb, q, NULL);
		if (!tr) {
			break;
		}
		w = tr->parent;
		free(tr);
	}
	return NULL;
}

/* Pointer coordinates relative to the toplevel surface, from the real
 * X11 pointer position (not the possibly-stale layout cursor). */
bool pointer_coords_on_window(struct wlx_server *server,
		struct wlx_window *win, double *sx, double *sy) {
	if (!win || !win->toplevel) {
		return false;
	}
	xcb_window_t target = win->content_xwin != XCB_WINDOW_NONE
		? win->content_xwin : win->xwin;
	if (target == XCB_WINDOW_NONE || !server->xcb) {
		return false;
	}
	int16_t px = 0, py = 0;
	if (!query_root_pointer_position(server, &px, &py)) {
		return false;
	}
	xcb_translate_coordinates_cookie_t tc =
		xcb_translate_coordinates(server->xcb, server->xcb_root, target, px, py);
	xcb_translate_coordinates_reply_t *tr =
		xcb_translate_coordinates_reply(server->xcb, tc, NULL);
	if (!tr) {
		return false;
	}
	*sx = (double)tr->dst_x;
	*sy = (double)tr->dst_y;
	free(tr);
	return true;
}

void resize_output_to(struct wlx_window *win, int w, int h) {
	if (!win->output || w < WLX_MIN_OUTPUT_SIZE || h < WLX_MIN_OUTPUT_SIZE) {
		return;
	}
	if (win->output->width == w && win->output->height == h) {
		return;
	}
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_custom_mode(&state, w, h, 0);
	if (!wlr_output_commit_state(win->output, &state)) {
		wlr_log(WLR_ERROR, "failed to resize X11 output to %dx%d", w, h);
	}
	wlr_output_state_finish(&state);
	/* output_commit may have already updated last_* and set_size; keep
	 * them consistent if commit did not emit (same-size no-op path). */
	win->last_output_width = win->output->width;
	win->last_output_height = win->output->height;
}

void create_output_for_window(struct wlx_window *win) {
	struct wlx_server *server = win->server;

	wlr_log(WLR_INFO, "mapping toplevel \"%s\" (app_id \"%s\") to a new X11 window",
		win->toplevel->title ? win->toplevel->title : "(no title)",
		win->toplevel->app_id ? win->toplevel->app_id : "(no app_id)");

	/* Fresh X11 identity state (also cleared in output_destroy; reset
	 * here so a remap never carries stale related[] entries). */
	win->xwin = XCB_WINDOW_NONE;
	win->content_xwin = XCB_WINDOW_NONE;
	win->related_count = 0;
	memset(win->related, 0, sizeof(win->related));
	win->size_from_wm = false;

	xcb_window_t *before = NULL;
	int before_n = 0;
	query_root_children(server->xcb, server->xcb_root, &before, &before_n);

	struct wlr_output *output = wlr_x11_output_create(server->backend);
	if (!output) {
		wlr_log(WLR_ERROR, "failed to create X11 output for new toplevel");
		free(before);
		return;
	}
	win->output = output;
	output->data = win;

	wlr_output_init_render(output, server->allocator, server->renderer);

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);

	/* Size the X11 window to the client's geometry when available so
	 * transient dialogs are not forced into the desktop default size. */
	int want_w = 0, want_h = 0;
	toplevel_preferred_size(win, &want_w, &want_h);

	struct wlr_output_mode *mode = wlr_output_preferred_mode(output);
	if (mode && want_w == WLX_DEFAULT_WIDTH && want_h == WLX_DEFAULT_HEIGHT) {
		wlr_log(WLR_INFO, "using preferred output mode %dx%d",
			mode->width, mode->height);
		wlr_output_state_set_mode(&state, mode);
	} else {
		wlr_log(WLR_INFO, "using custom mode %dx%d (client geometry)",
			want_w, want_h);
		wlr_output_state_set_custom_mode(&state, want_w, want_h, 0);
	}

	if (!wlr_output_commit_state(output, &state)) {
		wlr_log(WLR_ERROR, "failed to commit initial state for new X11 output");
	}
	wlr_output_state_finish(&state);

	wlr_log(WLR_INFO, "new X11 output committed at %dx%d",
		output->width, output->height);
	win->last_output_width = output->width;
	win->last_output_height = output->height;

	/* Resolve X window ID and apply transient position/hints *before*
	 * scheduling the first frame, so the window is not painted at 0,0
	 * and then jumped. */
	if (server->xcb) {
		xcb_flush(server->xcb);
	}
	xcb_roundtrip(server->xcb);
	xcb_window_t *after = NULL;
	int after_n = 0;
	query_root_children(server->xcb, server->xcb_root, &after, &after_n);

	win->xwin = XCB_WINDOW_NONE;
	xcb_window_t fallback = XCB_WINDOW_NONE;
	want_w = output->width > 0 ? output->width : WLX_DEFAULT_WIDTH;
	want_h = output->height > 0 ? output->height : WLX_DEFAULT_HEIGHT;
	for (int i = 0; i < after_n; i++) {
		bool seen = false;
		for (int j = 0; j < before_n; j++) {
			if (after[i] == before[j]) {
				seen = true;
				break;
			}
		}
		if (seen) {
			continue;
		}
		if (fallback == XCB_WINDOW_NONE) {
			fallback = after[i];
		}
		int gw = 0, gh = 0;
		if (query_window_geometry(server, after[i], &gw, &gh) &&
				gw == want_w && gh == want_h) {
			win->xwin = after[i];
			break;
		}
	}
	if (win->xwin == XCB_WINDOW_NONE) {
		win->xwin = fallback;
	}
	free(before);
	free(after);

	if (win->xwin != XCB_WINDOW_NONE) {
		wlr_log(WLR_INFO, "resolved backing X11 window id 0x%x for new toplevel",
			win->xwin);
		win->content_xwin = win->xwin;
		win->related_count = 0;
		memset(win->related, 0, sizeof(win->related));
		if (win->related_count < WLX_MAX_RELATED_WINDOWS) {
			win->related[win->related_count++] = win->xwin;
		}

		/* Hints + position before any frame (and before slow subtree walk). */
		apply_transient_hints(win);
		xwin_set_title(server, win->xwin, win->toplevel->title);
		xwin_set_class(server, win->xwin, win->toplevel->app_id);
		dnd_set_xdnd_aware(server, win->xwin);
		if (server->xcb) {
			xcb_flush(server->xcb);
		}

		register_x11_window_subtree(win, win->xwin);
		xcb_window_t found = find_content_window(server, win);
		if (found != XCB_WINDOW_NONE) {
			win->content_xwin = found;
			apply_transient_hints(win);
			xwin_set_title(server, win->content_xwin, win->toplevel->title);
			xwin_set_class(server, win->content_xwin, win->toplevel->app_id);
			dnd_set_xdnd_aware(server, win->content_xwin);
			if (server->xcb) {
				xcb_flush(server->xcb);
			}
		}

		snprintf(win->last_title, sizeof(win->last_title), "%s",
			win->toplevel->title ? win->toplevel->title : "");
		snprintf(win->last_app_id, sizeof(win->last_app_id), "%s",
			win->toplevel->app_id ? win->toplevel->app_id : "");
	} else {
		wlr_log(WLR_INFO, "could not resolve backing X11 window id "
			"(title/class won't be synced, window should still be visible)");
	}

	win->l_output = wlr_output_layout_add_auto(server->output_layout, output);
	win->scene_output = wlr_scene_output_create(server->scene, output);
	wlr_scene_output_layout_add_output(server->scene_layout, win->l_output,
		win->scene_output);
	wlr_scene_node_set_position(&win->scene_tree->node,
		win->l_output->x, win->l_output->y);

	win->output_frame.notify = output_frame;
	wl_signal_add(&output->events.frame, &win->output_frame);
	win->output_destroy.notify = output_destroy;
	wl_signal_add(&output->events.destroy, &win->output_destroy);
	win->output_commit.notify = output_commit;
	wl_signal_add(&output->events.commit, &win->output_commit);
	win->output_request_state.notify = output_request_state;
	wl_signal_add(&output->events.request_state, &win->output_request_state);

	wlr_xdg_toplevel_set_size(win->toplevel, output->width, output->height);

	/* First frame only after position/hints are applied. */
	wlr_output_schedule_frame(output);
}


/* ------------------------------------------------------------------- */
/* xdg-shell toplevel lifecycle                                         */
/* ------------------------------------------------------------------- */

