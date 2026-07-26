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

void begin_interactive_move(struct wlx_window *win) {
	struct wlx_server *server = win->server;
	xcb_window_t outer = outer_position_window(win);
	xcb_window_t target = configure_target_window(win);
	int16_t wx, wy, px, py;
	if (outer == XCB_WINDOW_NONE || target == XCB_WINDOW_NONE ||
			!query_window_root_position(server, outer, &wx, &wy) ||
			!query_root_pointer_position(server, &px, &py)) {
		return;
	}
	server->move_win = win;
	/* Offset from pointer to the frame's root origin — the same space as
	 * ConfigureRequest x/y, so the first update is a no-op geometrically. */
	server->move_offset_x = wx - px;
	server->move_offset_y = wy - py;
	server->drag_last_send_at = (struct timespec){0};
	server->drag_correction_x = 0;
	server->drag_correction_y = 0;
	server->drag_last_requested_x = wx;
	server->drag_last_requested_y = wy;

	wlr_log(WLR_INFO, "starting self-driven interactive move of window "
		"0x%x (outer 0x%x, configure target 0x%x) at (%d,%d), pointer at "
		"(%d,%d), offset (%d,%d)", win->xwin, outer, target, wx, wy, px, py,
		server->move_offset_x, server->move_offset_y);
}

void begin_interactive_resize(struct wlx_window *win, uint32_t edges) {
	struct wlx_server *server = win->server;
	xcb_window_t outer = outer_position_window(win);
	xcb_window_t target = configure_target_window(win);
	/* Size must be the client (content) size: ConfigureRequest width/
	 * height are the client's dimensions, not the frame's. */
	xcb_window_t size_win = (win->content_xwin != XCB_WINDOW_NONE)
		? win->content_xwin : win->xwin;
	int16_t wx, wy, px, py;
	int w, h;
	if (outer == XCB_WINDOW_NONE || target == XCB_WINDOW_NONE ||
			size_win == XCB_WINDOW_NONE ||
			!query_window_root_position(server, outer, &wx, &wy) ||
			!query_window_geometry(server, size_win, &w, &h) ||
			!query_root_pointer_position(server, &px, &py)) {
		return;
	}
	server->resize_win = win;
	server->resize_edges = edges;
	server->resize_start_x = wx;
	server->resize_start_y = wy;
	server->resize_start_w = w;
	server->resize_start_h = h;
	server->resize_start_pointer_x = px;
	server->resize_start_pointer_y = py;
	server->drag_last_send_at = (struct timespec){0};
	server->drag_correction_x = 0;
	server->drag_correction_y = 0;
	server->drag_last_requested_x = wx;
	server->drag_last_requested_y = wy;

	wlr_log(WLR_INFO, "starting self-driven interactive resize of window "
		"0x%x (outer 0x%x, configure target 0x%x, edges 0x%x) at (%d,%d) "
		"%dx%d, pointer at (%d,%d)", win->xwin, outer, target, edges,
		wx, wy, w, h, px, py);
}

void toplevel_request_move(struct wl_listener *listener, void *data) {
	(void)data;
	struct wlx_window *win = wl_container_of(listener, win, request_move);
	wlr_log(WLR_INFO, "toplevel requested move");
	begin_interactive_move(win);
}

void toplevel_request_resize(struct wl_listener *listener, void *data) {
	struct wlx_window *win = wl_container_of(listener, win, request_resize);
	struct wlr_xdg_toplevel_resize_event *event = data;
	wlr_log(WLR_INFO, "toplevel requested resize (edges 0x%x)", event->edges);
	begin_interactive_resize(win, event->edges);
}

void toplevel_request_maximize(struct wl_listener *listener, void *data) {
	(void)data;
	struct wlx_window *win = wl_container_of(listener, win, request_maximize);
	bool want = win->toplevel->requested.maximized;
	xcb_window_t target = ewmh_target_window(win);

	if (target != XCB_WINDOW_NONE) {
		wlr_log(WLR_INFO, "toplevel requested maximized=%d -> delegating to "
			"host WM (target 0x%x)", want, target);
		send_net_wm_state(win->server, target,
			want ? _NET_WM_STATE_ADD : _NET_WM_STATE_REMOVE,
			win->server->atom_net_wm_state_maximized_vert,
			win->server->atom_net_wm_state_maximized_horz);
	}

	/* Some clients (e.g. gedit, restoring a remembered window state)
	 * request maximized state immediately after creating the toplevel,
	 * before ever committing a buffer. wlr_xdg_toplevel_set_maximized()
	 * schedules a configure internally, which trips the same
	 * "surface->initialized" assertion as calling
	 * wlr_xdg_surface_schedule_configure() too early does (see
	 * surface_commit()) -- and there's no X11 window yet at this point
	 * anyway (it's only created once the surface first maps), so there's
	 * nothing real for us to have maximized regardless. Just skip it;
	 * our own initial configure on first commit will still give the
	 * client a size, it just won't start pre-maximized. */
	if (!win->initial_configure_sent) {
		wlr_log(WLR_INFO,
			"ignoring early maximize request (surface not yet initialized)");
		return;
	}

	/* Per xdg-shell, we must always respond with a configure, even if
	 * nothing changes -- wlr_xdg_toplevel_set_maximized() does this for
	 * us. The actual size change (if any) arrives later via the host
	 * WM's real resize of the X11 window, same as any other resize. */
	wlr_xdg_toplevel_set_maximized(win->toplevel, want);
}

void toplevel_request_fullscreen(struct wl_listener *listener, void *data) {
	(void)data;
	struct wlx_window *win = wl_container_of(listener, win, request_fullscreen);
	bool want = win->toplevel->requested.fullscreen;
	xcb_window_t target = ewmh_target_window(win);

	if (target != XCB_WINDOW_NONE) {
		wlr_log(WLR_INFO, "toplevel requested fullscreen=%d -> delegating "
			"to host WM (target 0x%x)", want, target);
		send_net_wm_state(win->server, target,
			want ? _NET_WM_STATE_ADD : _NET_WM_STATE_REMOVE,
			win->server->atom_net_wm_state_fullscreen, 0);
	}

	/* Same early-request hazard as maximize above. */
	if (!win->initial_configure_sent) {
		wlr_log(WLR_INFO,
			"ignoring early fullscreen request (surface not yet initialized)");
		return;
	}

	wlr_xdg_toplevel_set_fullscreen(win->toplevel, want);
}

void toplevel_request_minimize(struct wl_listener *listener, void *data) {
	(void)data;
	struct wlx_window *win = wl_container_of(listener, win, request_minimize);
	xcb_window_t target = ewmh_target_window(win);
	if (target == XCB_WINDOW_NONE) {
		return;
	}
	wlr_log(WLR_INFO, "toplevel requested minimize -> delegating to host WM "
		"(target 0x%x)", target);
	/* ICCCM WM_CHANGE_STATE with IconicState=3. xdg-shell's minimize
	 * request has no configure/ack round trip (unlike maximize/
	 * fullscreen), so there's nothing further to send the client. */
	send_root_client_message(win->server, target,
		win->server->atom_wm_change_state, 3 /* IconicState */, 0, 0, 0, 0);
}

/* Many clients (particularly GL/EGL ones, which foot and weston-terminal
 * both are) render into a child window distinct from the top-level window
 * the WM manages -- the one we find via the root-children diff in
 * create_output_for_window() is only that top-level window. X11 resolves
 * both the displayed cursor and (more importantly) input focus from the
 * actual window under the pointer / holding focus, which may be such a
 * child, not the top-level frame. See register_x11_window_subtree() below,
 * which walks the whole subtree once we know the WM/X11 machinery
 * involved here uses child windows for these clients. */

/* ------------------------------------------------------------------- */
/* Output (== one X11 window) lifecycle                                 */
/* ------------------------------------------------------------------- */

