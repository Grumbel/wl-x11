/*
 * wc-x11 - a minimal wlroots Wayland compositor that runs nested inside an
 * X11 session.
 *
 * Unlike Weston's or KWin's X11 backends, which present the whole nested
 * Wayland desktop inside a *single* X11 window, this compositor creates a
 * dedicated X11 window (via wlroots' X11 backend, which models each such
 * window as a "wlr_output") for every Wayland toplevel. The toplevel's
 * surface is scaled to exactly fill that output. The net effect is that
 * each Wayland application window shows up as its own ordinary top-level
 * X11 window, manageable, decoratable, and movable by whatever window
 * manager is running on the host X server -- it behaves like a native X11
 * window rather than a "VNC-into-a-desktop" view.
 *
 * Design notes / simplifications (see README.md for the full list):
 *
 *  - One wlr_output == one X11 window == one Wayland toplevel. Outputs are
 *    created on demand when a toplevel surface maps, and destroyed when it
 *    unmaps/is destroyed or the user closes the X11 window.
 *  - wlr_output_layout_add_auto() is used purely as an internal
 *    bookkeeping trick to give each output a non-overlapping region in the
 *    compositor's single shared scene graph. The X11 windows themselves are
 *    of course independently placed on the host desktop by the host WM;
 *    our internal layout has no relation to their on-screen position.
 *  - No client-side compositor cursor is drawn: because every surface is
 *    hosted inside a real X11 window, the host X server already renders a
 *    pointer image over it. We only track pointer position for hit
 *    testing / focus, via wlr_cursor.
 *  - Popups (menus, tooltips) are positioned relative to their parent
 *    toplevel by wlroots' scene-graph xdg-shell helper as usual, but since
 *    a toplevel's whole visible area *is* its X11 window, popups that
 *    extend past the toplevel's edges get clipped at the window boundary.
 *    This is an intrinsic trade-off of the 1:1 window mapping model versus
 *    Xwayland's rootless mode (which uses extra override-redirect X
 *    windows for popups).
 *  - Window title / WM_CLASS are best-effort synced onto the X11 window via
 *    a small auxiliary XCB connection (wlroots does not expose the raw
 *    xcb_window_t of backend-created outputs), by diffing the root
 *    window's child list before/after creating the output.
 *  - Clipboard is only shared among Wayland clients of this compositor; it
 *    is not bridged to the host X11 selection. Bridging would need an
 *    Xwayland-style clipboard proxy and is out of scope for "minimal".
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>

#include <wayland-server-core.h>

#include <wlr/backend.h>
#include <wlr/backend/x11.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/util/log.h>

#include <xkbcommon/xkbcommon.h>

#include <xcb/xcb.h>
#include <xcb/xcb_cursor.h>

#include <wlr/util/edges.h>

/* ------------------------------------------------------------------- */
/* Types                                                                 */
/* ------------------------------------------------------------------- */

struct wc_server {
	struct wl_display *wl_display;
	struct wl_event_loop *loop;
	struct wlr_backend *backend;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;

	struct wlr_scene *scene;
	struct wlr_scene_output_layout *scene_layout;
	struct wlr_output_layout *output_layout;

	struct wlr_xdg_shell *xdg_shell;
	struct wl_listener new_xdg_toplevel;

	struct wlr_xdg_decoration_manager_v1 *xdg_decoration_manager;
	struct wl_listener new_toplevel_decoration;

	struct wlr_seat *seat;
	struct wlr_cursor *cursor;
	bool have_keyboard;
	bool have_pointer;

	struct wl_listener new_input;
	struct wl_listener cursor_motion;
	struct wl_listener cursor_motion_absolute;
	struct wl_listener cursor_button;
	struct wl_listener cursor_axis;
	struct wl_listener cursor_frame;
	struct wl_listener request_set_selection;

	struct wl_list windows; /* wc_window::link */
	struct wc_window *focused_window;

	/* Auxiliary connection used only to set WM_NAME / WM_CLASS on the
	 * X11 windows that the wlroots X11 backend creates for us. */
	xcb_connection_t *xcb;
	xcb_window_t xcb_root;
	xcb_atom_t atom_net_wm_name;
	xcb_atom_t atom_utf8_string;
	xcb_atom_t atom_net_wm_moveresize;
	xcb_atom_t atom_net_wm_state;
	xcb_atom_t atom_net_wm_state_maximized_vert;
	xcb_atom_t atom_net_wm_state_maximized_horz;
	xcb_atom_t atom_net_wm_state_fullscreen;
	xcb_atom_t atom_wm_change_state;
	xcb_cursor_t default_cursor;

	struct wl_event_source *sigint_source;
	struct wl_event_source *sigterm_source;
};

struct wc_window {
	struct wc_server *server;
	struct wlr_xdg_toplevel *toplevel;
	struct wlr_scene_tree *scene_tree;

	struct wlr_output *output;               /* NULL when unmapped */
	struct wlr_output_layout_output *l_output;
	struct wlr_scene_output *scene_output;
	int last_output_width;
	int last_output_height;
	struct wl_event_source *resize_debounce_timer;
	int pending_resize_width;
	int pending_resize_height;

	xcb_window_t xwin;
#define WC_MAX_RELATED_WINDOWS 32
	xcb_window_t related[WC_MAX_RELATED_WINDOWS];
	int related_count;
	char last_title[256];
	char last_app_id[256];
	bool initial_configure_sent;
	struct wlr_xdg_toplevel_decoration_v1 *pending_decoration;

	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener destroy;
	struct wl_listener commit;
	struct wl_listener request_move;
	struct wl_listener request_resize;
	struct wl_listener request_maximize;
	struct wl_listener request_fullscreen;
	struct wl_listener request_minimize;

	struct wl_listener output_frame;
	struct wl_listener output_destroy;
	struct wl_listener output_commit;

	struct wl_list link;
};

struct wc_keyboard {
	struct wc_server *server;
	struct wlr_keyboard *wlr_keyboard;
	struct wl_listener modifiers;
	struct wl_listener key;
	struct wl_listener destroy;
};

/* ------------------------------------------------------------------- */
/* XCB helpers for best-effort title / class syncing                    */
/* ------------------------------------------------------------------- */

static xcb_atom_t intern_atom(xcb_connection_t *c, const char *name) {
	xcb_intern_atom_cookie_t cookie =
		xcb_intern_atom(c, 0, strlen(name), name);
	xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(c, cookie, NULL);
	xcb_atom_t atom = reply ? reply->atom : XCB_ATOM_NONE;
	free(reply);
	return atom;
}

static void query_root_children(xcb_connection_t *c, xcb_window_t root,
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
static void xcb_roundtrip(xcb_connection_t *c) {
	if (!c || xcb_connection_has_error(c)) {
		return;
	}
	free(xcb_get_input_focus_reply(c, xcb_get_input_focus(c), NULL));
}

static void xwin_set_title(struct wc_server *s, xcb_window_t w, const char *title) {
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

static void xwin_set_class(struct wc_server *s, xcb_window_t w, const char *app_id) {
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

/* wlroots' X11 backend creates its windows with an invisible cursor by
 * default -- it expects the nested Wayland compositor to render its own
 * pointer image (e.g. from client-provided cursor surfaces). We don't
 * implement that (out of scope for "minimal"), so instead we force a
 * plain, always-visible arrow cursor at the X11 level on every window we
 * create. This means cursor shape doesn't change contextually (no I-beam
 * over text fields, no resize handles, etc.) but the pointer is always
 * visible, which is the important part.
 *
 * We use xcb-cursor (the XCB equivalent of libXcursor) to load a real
 * themed cursor. An earlier version of this used the legacy X core-font
 * glyph cursor mechanism (xcb_open_font(..., "cursor")), which silently
 * does nothing on modern X servers/distros that no longer ship that
 * legacy bitmap font -- the cursor ID it produces doesn't actually exist,
 * so every later attempt to apply it is quietly rejected by the server.
 * xcb-cursor is the mechanism real window managers use and doesn't have
 * that problem. */
static xcb_cursor_t create_default_cursor(xcb_connection_t *c, xcb_screen_t *screen) {
	if (!c || xcb_connection_has_error(c) || !screen) {
		return 0;
	}

	xcb_cursor_context_t *ctx;
	if (xcb_cursor_context_new(c, screen, &ctx) < 0) {
		wlr_log(WLR_ERROR, "xcb_cursor_context_new failed; pointer will stay invisible");
		return 0;
	}

	/* Try a couple of common names; themes disagree on which one exists. */
	xcb_cursor_t cursor = xcb_cursor_load_cursor(ctx, "left_ptr");
	if (cursor == 0) {
		cursor = xcb_cursor_load_cursor(ctx, "default");
	}
	xcb_cursor_context_free(ctx);

	if (cursor == 0) {
		wlr_log(WLR_ERROR,
			"xcb_cursor_load_cursor could not find \"left_ptr\" or \"default\" "
			"in the current cursor theme; pointer will stay invisible");
	} else {
		wlr_log(WLR_INFO, "loaded default X11 cursor (xid 0x%x)", cursor);
	}
	return cursor;
}

static void xwin_set_default_cursor(struct wc_server *s, xcb_window_t w) {
	if (!s->xcb || xcb_connection_has_error(s->xcb) ||
			w == XCB_WINDOW_NONE || s->default_cursor == 0) {
		return;
	}
	uint32_t value = s->default_cursor;
	xcb_change_window_attributes(s->xcb, w, XCB_CW_CURSOR, &value);
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

enum {
	_NET_WM_MOVERESIZE_SIZE_TOPLEFT = 0,
	_NET_WM_MOVERESIZE_SIZE_TOP = 1,
	_NET_WM_MOVERESIZE_SIZE_TOPRIGHT = 2,
	_NET_WM_MOVERESIZE_SIZE_RIGHT = 3,
	_NET_WM_MOVERESIZE_SIZE_BOTTOMRIGHT = 4,
	_NET_WM_MOVERESIZE_SIZE_BOTTOM = 5,
	_NET_WM_MOVERESIZE_SIZE_BOTTOMLEFT = 6,
	_NET_WM_MOVERESIZE_SIZE_LEFT = 7,
	_NET_WM_MOVERESIZE_MOVE = 8,
};

enum {
	_NET_WM_STATE_REMOVE = 0,
	_NET_WM_STATE_ADD = 1,
};

static void send_root_client_message(struct wc_server *s, xcb_window_t window,
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

static void send_net_wm_state(struct wc_server *s, xcb_window_t w,
		uint32_t action, xcb_atom_t prop1, xcb_atom_t prop2) {
	send_root_client_message(s, w, s->atom_net_wm_state, action, prop1, prop2,
		1 /* source indication: normal application */, 0);
}

/* Real, host-server pointer position, decoupled from our internal
 * (purely bookkeeping) scene-layout cursor coordinates -- needed because
 * _NET_WM_MOVERESIZE wants root-window coordinates and our own wlr_cursor
 * doesn't live in that space. */
static bool query_root_pointer(struct wc_server *s, int16_t *root_x, int16_t *root_y) {
	if (!s->xcb || xcb_connection_has_error(s->xcb)) {
		return false;
	}
	xcb_query_pointer_cookie_t cookie = xcb_query_pointer(s->xcb, s->xcb_root);
	xcb_query_pointer_reply_t *reply = xcb_query_pointer_reply(s->xcb, cookie, NULL);
	if (!reply) {
		return false;
	}
	*root_x = reply->root_x;
	*root_y = reply->root_y;
	free(reply);
	return true;
}

static uint32_t edges_to_moveresize_direction(uint32_t edges) {
	bool top = edges & WLR_EDGE_TOP;
	bool bottom = edges & WLR_EDGE_BOTTOM;
	bool left = edges & WLR_EDGE_LEFT;
	bool right = edges & WLR_EDGE_RIGHT;

	if (top && left) return _NET_WM_MOVERESIZE_SIZE_TOPLEFT;
	if (top && right) return _NET_WM_MOVERESIZE_SIZE_TOPRIGHT;
	if (bottom && right) return _NET_WM_MOVERESIZE_SIZE_BOTTOMRIGHT;
	if (bottom && left) return _NET_WM_MOVERESIZE_SIZE_BOTTOMLEFT;
	if (top) return _NET_WM_MOVERESIZE_SIZE_TOP;
	if (bottom) return _NET_WM_MOVERESIZE_SIZE_BOTTOM;
	if (left) return _NET_WM_MOVERESIZE_SIZE_LEFT;
	if (right) return _NET_WM_MOVERESIZE_SIZE_RIGHT;
	return _NET_WM_MOVERESIZE_MOVE;
}

static void toplevel_request_move(struct wl_listener *listener, void *data) {
	(void)data;
	struct wc_window *win = wl_container_of(listener, win, request_move);
	int16_t rx, ry;
	if (win->xwin == XCB_WINDOW_NONE || !query_root_pointer(win->server, &rx, &ry)) {
		return;
	}
	wlr_log(WLR_INFO, "toplevel requested move -> delegating to host WM");
	send_root_client_message(win->server, win->xwin,
		win->server->atom_net_wm_moveresize,
		(uint32_t)rx, (uint32_t)ry, _NET_WM_MOVERESIZE_MOVE,
		1 /* button: assume left */, 1 /* source: application */);
}

static void toplevel_request_resize(struct wl_listener *listener, void *data) {
	struct wc_window *win = wl_container_of(listener, win, request_resize);
	struct wlr_xdg_toplevel_resize_event *event = data;
	int16_t rx, ry;
	if (win->xwin == XCB_WINDOW_NONE || !query_root_pointer(win->server, &rx, &ry)) {
		return;
	}
	uint32_t dir = edges_to_moveresize_direction(event->edges);
	wlr_log(WLR_INFO, "toplevel requested resize (edges 0x%x) -> delegating to host WM",
		event->edges);
	send_root_client_message(win->server, win->xwin,
		win->server->atom_net_wm_moveresize,
		(uint32_t)rx, (uint32_t)ry, dir, 1, 1);
}

static void toplevel_request_maximize(struct wl_listener *listener, void *data) {
	(void)data;
	struct wc_window *win = wl_container_of(listener, win, request_maximize);
	bool want = win->toplevel->requested.maximized;

	if (win->xwin != XCB_WINDOW_NONE) {
		wlr_log(WLR_INFO, "toplevel requested maximized=%d -> delegating to host WM", want);
		send_net_wm_state(win->server, win->xwin,
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

static void toplevel_request_fullscreen(struct wl_listener *listener, void *data) {
	(void)data;
	struct wc_window *win = wl_container_of(listener, win, request_fullscreen);
	bool want = win->toplevel->requested.fullscreen;

	if (win->xwin != XCB_WINDOW_NONE) {
		wlr_log(WLR_INFO, "toplevel requested fullscreen=%d -> delegating to host WM", want);
		send_net_wm_state(win->server, win->xwin,
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

static void toplevel_request_minimize(struct wl_listener *listener, void *data) {
	(void)data;
	struct wc_window *win = wl_container_of(listener, win, request_minimize);
	if (win->xwin == XCB_WINDOW_NONE) {
		return;
	}
	wlr_log(WLR_INFO, "toplevel requested minimize -> delegating to host WM");
	/* ICCCM WM_CHANGE_STATE with IconicState=3. xdg-shell's minimize
	 * request has no configure/ack round trip (unlike maximize/
	 * fullscreen), so there's nothing further to send the client. */
	send_root_client_message(win->server, win->xwin,
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

static struct wc_window *window_from_xwin(struct wc_server *server, xcb_window_t w) {
	struct wc_window *win;
	wl_list_for_each(win, &server->windows, link) {
		if (win->xwin == w) {
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

static void select_window_events(struct wc_server *server, xcb_window_t w) {
	if (!server->xcb || xcb_connection_has_error(server->xcb) || w == XCB_WINDOW_NONE) {
		return;
	}
	uint32_t mask = XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_FOCUS_CHANGE;
	xcb_change_window_attributes(server->xcb, w, XCB_CW_EVENT_MASK, &mask);
	xcb_flush(server->xcb);
}

/* Applies the default cursor and selects resize/focus events on the given
 * window AND every descendant of it (recursively), and records each one
 * against `win` so window_from_xwin() can find `win` regardless of which
 * window in the subtree an event (FocusIn, ConfigureNotify, ...) actually
 * arrives on. This is what makes both the cursor and the X11-focus ->
 * xdg_toplevel ACTIVATED wiring work even when the real interactive
 * window is a child of the one we found via the root-children diff. */
static void register_x11_window_subtree(struct wc_window *win, xcb_window_t w) {
	struct wc_server *server = win->server;

	xwin_set_default_cursor(server, w);
	select_window_events(server, w);
	wlr_log(WLR_INFO, "registered X11 window 0x%x for toplevel \"%s\"",
		w, win->toplevel && win->toplevel->title ? win->toplevel->title : "?");

	if (win->related_count < WC_MAX_RELATED_WINDOWS) {
		win->related[win->related_count++] = w;
	} else {
		wlr_log(WLR_ERROR, "window subtree exceeds WC_MAX_RELATED_WINDOWS, "
			"some descendants won't get focus/cursor handling");
	}

	xcb_window_t *children = NULL;
	int n = 0;
	query_root_children(server->xcb, w, &children, &n);
	for (int i = 0; i < n; i++) {
		register_x11_window_subtree(win, children[i]);
	}
	free(children);
}

/* wlroots' X11 backend apparently doesn't keep wlr_output->width/height in
 * sync with host-driven window resizes in the version this was tested
 * against (its own events.commit never reflects the new size). So instead
 * of trusting that, we watch ConfigureNotify ourselves via our auxiliary
 * XCB connection and force the wlr_output to the observed size via
 * wlr_output_commit_state().
 *
 * That commit almost certainly also makes the X11 backend re-assert the
 * window's size at the X11 level (XConfigureWindow), which some WMs/
 * clients respond to with a *different* size of their own (e.g. GTK's
 * CSD shadow margin appearing/disappearing), which we'd observe as
 * another ConfigureNotify, echo back again, and so on forever. So rather
 * than committing on every single ConfigureNotify, we debounce: each new
 * observed size resets a short timer, and we only actually commit once
 * things have been quiet for a bit. A tight external fight collapses
 * into at most one commit per quiet period instead of feeding itself. */
#define WC_RESIZE_DEBOUNCE_MS 40

static int resize_debounce_fired(void *data) {
	struct wc_window *win = data;
	int width = win->pending_resize_width;
	int height = win->pending_resize_height;

	if (!win->output || width <= 0 || height <= 0) {
		return 0;
	}
	if (width == win->last_output_width && height == win->last_output_height) {
		return 0;
	}

	wlr_log(WLR_INFO, "debounced X11 resize settling at %dx%d", width, height);

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_custom_mode(&state, width, height, 0);
	wlr_output_commit_state(win->output, &state);
	wlr_output_state_finish(&state);
	return 0;
}

static void apply_x11_resize(struct wc_window *win, int width, int height) {
	if (!win->output || width <= 0 || height <= 0) {
		return;
	}
	if (width == win->last_output_width && height == win->last_output_height) {
		return;
	}
	wlr_log(WLR_INFO, "observed X11 ConfigureNotify resize to %dx%d "
		"(debouncing %dms)", width, height, WC_RESIZE_DEBOUNCE_MS);

	win->pending_resize_width = width;
	win->pending_resize_height = height;
	if (win->resize_debounce_timer) {
		wl_event_source_timer_update(win->resize_debounce_timer,
			WC_RESIZE_DEBOUNCE_MS);
	}
}

/* Drive both the xdg_toplevel ACTIVATED state (which is what clients like
 * weston-terminal read to decide e.g. solid vs. hollow cursor block) and
 * wl_seat keyboard focus from the host WM's real X11 focus, rather than
 * from our own pointer-hover heuristics. This keeps "this window is
 * focused" meaning the same thing at the X11 level and the Wayland level. */
static void set_active_window(struct wc_server *server, struct wc_window *win) {
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

static int handle_xcb_readable(int fd, uint32_t mask, void *data) {
	(void)fd;
	(void)mask;
	struct wc_server *server = data;

	xcb_generic_event_t *event;
	while ((event = xcb_poll_for_event(server->xcb)) != NULL) {
		uint8_t type = event->response_type & ~0x80;
		if (type == XCB_CONFIGURE_NOTIFY) {
			xcb_configure_notify_event_t *cn = (xcb_configure_notify_event_t *)event;
			/* Deliberately NOT using window_from_xwin() here: that also
			 * matches descendant windows (CSD title bar buttons, shadow
			 * border strips, etc.), whose own internal layout resizes
			 * must not be mistaken for the toplevel itself resizing --
			 * doing so was feeding garbage sizes (e.g. a 20x24 button)
			 * into apply_x11_resize() and causing runaway growth. Only
			 * react when the event is for a window we know as an actual
			 * top-level. */
			struct wc_window *win;
			bool found = false;
			wl_list_for_each(win, &server->windows, link) {
				if (win->xwin == cn->window) {
					found = true;
					break;
				}
			}
			if (found) {
				apply_x11_resize(win, cn->width, cn->height);
			}
		} else if (type == XCB_FOCUS_IN) {
			xcb_focus_in_event_t *fi = (xcb_focus_in_event_t *)event;
			struct wc_window *win = window_from_xwin(server, fi->event);
			if (win) {
				wlr_log(WLR_INFO, "X11 FocusIn on window 0x%x", fi->event);
				set_active_window(server, win);
			}
		} else if (type == XCB_FOCUS_OUT) {
			xcb_focus_out_event_t *fo = (xcb_focus_out_event_t *)event;
			if (server->focused_window && server->focused_window->xwin == fo->event) {
				wlr_log(WLR_INFO, "X11 FocusOut on window 0x%x", fo->event);
				set_active_window(server, NULL);
			}
		}
		free(event);
	}
	return 0;
}

static void output_frame(struct wl_listener *listener, void *data) {
	struct wc_window *win = wl_container_of(listener, win, output_frame);
	if (!win->scene_output) {
		return;
	}
	wlr_scene_output_commit(win->scene_output, NULL);

	/* wlroots' X11 backend appears to keep resetting the window's cursor
	 * back to invisible on its own (a one-shot set right after window
	 * creation didn't stick), so keep re-asserting a normal cursor every
	 * frame. This is a cheap XChangeWindowAttributes call and is a
	 * brute-force fix regardless of exactly when/why the backend resets
	 * it. */
	xwin_set_default_cursor(win->server, win->xwin);

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(win->scene_output, &now);
}

static void output_commit(struct wl_listener *listener, void *data) {
	struct wc_window *win = wl_container_of(listener, win, output_commit);
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

static void output_destroy(struct wl_listener *listener, void *data) {
	struct wc_window *win = wl_container_of(listener, win, output_destroy);

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

	win->output = NULL;
	win->scene_output = NULL;
	win->l_output = NULL;
	win->xwin = XCB_WINDOW_NONE;
}

#define WC_DEFAULT_WIDTH 1024
#define WC_DEFAULT_HEIGHT 720

static void create_output_for_window(struct wc_window *win) {
	struct wc_server *server = win->server;

	wlr_log(WLR_INFO, "mapping toplevel \"%s\" (app_id \"%s\") to a new X11 window",
		win->toplevel->title ? win->toplevel->title : "(no title)",
		win->toplevel->app_id ? win->toplevel->app_id : "(no app_id)");

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

	/* wlr_x11_output_create() does NOT pre-populate output->modes the way
	 * DRM/KMS backends do -- X11 windows are arbitrarily resizable, so
	 * there is no fixed mode list. If we don't explicitly set a mode
	 * here, the output (and therefore the underlying X11 window) can end
	 * up committed at its zero-initialized 0x0 size, which is why no
	 * window would appear on screen. Always fall back to an explicit
	 * custom mode when there's nothing in the preferred-mode list. */
	struct wlr_output_mode *mode = wlr_output_preferred_mode(output);
	if (mode) {
		wlr_log(WLR_INFO, "using preferred output mode %dx%d",
			mode->width, mode->height);
		wlr_output_state_set_mode(&state, mode);
	} else {
		wlr_log(WLR_INFO, "no preferred mode reported; using custom mode %dx%d",
			WC_DEFAULT_WIDTH, WC_DEFAULT_HEIGHT);
		wlr_output_state_set_custom_mode(&state, WC_DEFAULT_WIDTH,
			WC_DEFAULT_HEIGHT, 0);
	}

	if (!wlr_output_commit_state(output, &state)) {
		wlr_log(WLR_ERROR, "failed to commit initial state for new X11 output");
	}
	wlr_output_state_finish(&state);

	wlr_log(WLR_INFO, "new X11 output committed at %dx%d",
		output->width, output->height);
	win->last_output_width = output->width;
	win->last_output_height = output->height;

	/* Kick off the first frame explicitly; some backends (X11 included)
	 * don't render anything until a frame is scheduled at least once. */
	wlr_output_schedule_frame(output);

	win->l_output = wlr_output_layout_add_auto(server->output_layout, output);
	win->scene_output = wlr_scene_output_create(server->scene, output);
	wlr_scene_output_layout_add_output(server->scene_layout, win->l_output,
		win->scene_output);

	/* Place this toplevel's scene subtree at the output's slot in our
	 * internal (otherwise meaningless) layout, so it renders to fill
	 * exactly that output/X11 window. */
	wlr_scene_node_set_position(&win->scene_tree->node,
		win->l_output->x, win->l_output->y);

	win->output_frame.notify = output_frame;
	wl_signal_add(&output->events.frame, &win->output_frame);
	win->output_destroy.notify = output_destroy;
	wl_signal_add(&output->events.destroy, &win->output_destroy);
	win->output_commit.notify = output_commit;
	wl_signal_add(&output->events.commit, &win->output_commit);

	int w = output->width > 0 ? output->width : WC_DEFAULT_WIDTH;
	int h = output->height > 0 ? output->height : WC_DEFAULT_HEIGHT;
	wlr_xdg_toplevel_set_size(win->toplevel, w, h);

	/* Best-effort: find the xcb_window_t the backend just created so we
	 * can set WM_NAME / WM_CLASS on it. */
	xcb_roundtrip(server->xcb);
	xcb_window_t *after = NULL;
	int after_n = 0;
	query_root_children(server->xcb, server->xcb_root, &after, &after_n);

	win->xwin = XCB_WINDOW_NONE;
	for (int i = 0; i < after_n; i++) {
		bool seen = false;
		for (int j = 0; j < before_n; j++) {
			if (after[i] == before[j]) {
				seen = true;
				break;
			}
		}
		if (!seen) {
			win->xwin = after[i];
			break;
		}
	}
	free(before);
	free(after);

	if (win->xwin != XCB_WINDOW_NONE) {
		wlr_log(WLR_INFO, "resolved backing X11 window id 0x%x for new toplevel",
			win->xwin);
		register_x11_window_subtree(win, win->xwin);
		xwin_set_title(server, win->xwin, win->toplevel->title);
		xwin_set_class(server, win->xwin, win->toplevel->app_id);
		snprintf(win->last_title, sizeof(win->last_title), "%s",
			win->toplevel->title ? win->toplevel->title : "");
		snprintf(win->last_app_id, sizeof(win->last_app_id), "%s",
			win->toplevel->app_id ? win->toplevel->app_id : "");
	} else {
		wlr_log(WLR_INFO, "could not resolve backing X11 window id "
			"(title/class won't be synced, window should still be visible)");
	}
}

/* ------------------------------------------------------------------- */
/* xdg-shell toplevel lifecycle                                         */
/* ------------------------------------------------------------------- */

static void xdg_toplevel_map(struct wl_listener *listener, void *data) {
	struct wc_window *win = wl_container_of(listener, win, map);
	wlr_log(WLR_INFO, "surface map event received");
	if (win->output) {
		return; /* already has a window (e.g. re-map) */
	}
	create_output_for_window(win);
}

static void xdg_toplevel_unmap(struct wl_listener *listener, void *data) {
	struct wc_window *win = wl_container_of(listener, win, unmap);
	if (win->output) {
		/* wlr_output_destroy() synchronously fires events.destroy,
		 * which runs output_destroy() above and clears win->output. */
		wlr_output_destroy(win->output);
	}
}

static void surface_commit(struct wl_listener *listener, void *data) {
	struct wc_window *win = wl_container_of(listener, win, commit);
	wlr_log(WLR_INFO, "surface commit (mapped=%d, has_buffer=%d)",
		win->output != NULL,
		win->toplevel->base->surface->buffer != NULL);

	/* The client cannot attach a buffer (and therefore cannot map) until
	 * it has received and ack'd at least one xdg_surface.configure. By
	 * the time this commit listener runs, wlroots' own xdg_surface
	 * commit handling (registered before ours) has already marked the
	 * surface "initialized", which is a precondition for scheduling a
	 * configure -- doing this any earlier (e.g. right after get_toplevel)
	 * trips an assertion inside wlroots. Send one with no explicit size,
	 * which per the xdg-shell protocol tells the client it may pick its
	 * own initial size; we resize it to match its new dedicated X11
	 * window right after it maps. */
	if (!win->initial_configure_sent) {
		win->initial_configure_sent = true;
		wlr_xdg_surface_schedule_configure(win->toplevel->base);
	}

	if (win->pending_decoration) {
		struct wlr_xdg_toplevel_decoration_v1 *decoration = win->pending_decoration;
		win->pending_decoration = NULL;
		wlr_xdg_toplevel_decoration_v1_set_mode(decoration,
			WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	}

	if (win->xwin == XCB_WINDOW_NONE || !win->toplevel) {
		return;
	}
	const char *title = win->toplevel->title ? win->toplevel->title : "";
	const char *app_id = win->toplevel->app_id ? win->toplevel->app_id : "";

	if (strncmp(title, win->last_title, sizeof(win->last_title)) != 0) {
		xwin_set_title(win->server, win->xwin, title);
		snprintf(win->last_title, sizeof(win->last_title), "%s", title);
	}
	if (strncmp(app_id, win->last_app_id, sizeof(win->last_app_id)) != 0) {
		xwin_set_class(win->server, win->xwin, app_id);
		snprintf(win->last_app_id, sizeof(win->last_app_id), "%s", app_id);
	}
}

static void xdg_toplevel_destroy(struct wl_listener *listener, void *data) {
	struct wc_window *win = wl_container_of(listener, win, destroy);

	if (win->output) {
		wlr_output_destroy(win->output);
	}
	if (win->server->focused_window == win) {
		win->server->focused_window = NULL;
	}
	if (win->resize_debounce_timer) {
		wl_event_source_remove(win->resize_debounce_timer);
	}

	wl_list_remove(&win->map.link);
	wl_list_remove(&win->unmap.link);
	wl_list_remove(&win->destroy.link);
	wl_list_remove(&win->commit.link);
	wl_list_remove(&win->request_move.link);
	wl_list_remove(&win->request_resize.link);
	wl_list_remove(&win->request_maximize.link);
	wl_list_remove(&win->request_fullscreen.link);
	wl_list_remove(&win->request_minimize.link);
	wl_list_remove(&win->link);
	free(win);
}

struct wc_decoration {
	struct wl_listener request_mode;
	struct wl_listener destroy;
};

static void decoration_request_mode(struct wl_listener *listener, void *data) {
	(void)listener;
	struct wlr_xdg_toplevel_decoration_v1 *decoration = data;
	/* Always force server-side: each toplevel is a real host-WM-managed
	 * X11 window, so client-drawn CSD is redundant chrome whose buttons
	 * only reach us via relayed X11 messages that not every WM honors
	 * the same way -- the host WM's own title bar works natively and
	 * bypasses us entirely.
	 *
	 * xdg-decoration objects can legitimately be created (and its mode
	 * requested) before the toplevel's surface has had its first commit
	 * -- version 1 of the protocol actually requires this. Calling
	 * set_mode() (which schedules a configure) before that first commit
	 * trips the same "surface->initialized" assertion inside wlroots
	 * that calling wlr_xdg_surface_schedule_configure() too early does
	 * (see surface_commit() below) -- so defer it the same way. */
	struct wc_window *win = decoration->toplevel->base->data;
	if (win && !win->initial_configure_sent) {
		win->pending_decoration = decoration;
		return;
	}
	wlr_xdg_toplevel_decoration_v1_set_mode(decoration,
		WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

static void decoration_destroy(struct wl_listener *listener, void *data) {
	struct wlr_xdg_toplevel_decoration_v1 *decoration = data;
	struct wc_decoration *deco = wl_container_of(listener, deco, destroy);

	if (decoration && decoration->toplevel) {
		struct wc_window *win = decoration->toplevel->base->data;
		if (win && win->pending_decoration == decoration) {
			win->pending_decoration = NULL;
		}
	}

	wl_list_remove(&deco->request_mode.link);
	wl_list_remove(&deco->destroy.link);
	free(deco);
}

static void server_new_toplevel_decoration(struct wl_listener *listener, void *data) {
	(void)listener;
	struct wlr_xdg_toplevel_decoration_v1 *decoration = data;

	struct wc_decoration *deco = calloc(1, sizeof(*deco));
	deco->request_mode.notify = decoration_request_mode;
	wl_signal_add(&decoration->events.request_mode, &deco->request_mode);
	deco->destroy.notify = decoration_destroy;
	wl_signal_add(&decoration->events.destroy, &deco->destroy);

	wlr_log(WLR_INFO, "new xdg toplevel decoration object -> forcing server-side mode");

	struct wc_window *win = decoration->toplevel->base->data;
	if (win && !win->initial_configure_sent) {
		win->pending_decoration = decoration;
		return;
	}
	wlr_xdg_toplevel_decoration_v1_set_mode(decoration,
		WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

static void server_new_xdg_toplevel(struct wl_listener *listener, void *data) {
	struct wc_server *server = wl_container_of(listener, server, new_xdg_toplevel);
	struct wlr_xdg_toplevel *toplevel = data;

	wlr_log(WLR_INFO, "new xdg_toplevel created (not yet mapped)");

	struct wc_window *win = calloc(1, sizeof(*win));
	win->server = server;
	win->toplevel = toplevel;
	win->xwin = XCB_WINDOW_NONE;
	win->resize_debounce_timer = wl_event_loop_add_timer(server->loop,
		resize_debounce_fired, win);

	win->scene_tree = wlr_scene_xdg_surface_create(&server->scene->tree,
		toplevel->base);
	win->scene_tree->node.data = win;
	toplevel->base->data = win;

	win->map.notify = xdg_toplevel_map;
	wl_signal_add(&toplevel->base->surface->events.map, &win->map);
	win->unmap.notify = xdg_toplevel_unmap;
	wl_signal_add(&toplevel->base->surface->events.unmap, &win->unmap);
	win->destroy.notify = xdg_toplevel_destroy;
	wl_signal_add(&toplevel->events.destroy, &win->destroy);
	win->commit.notify = surface_commit;
	wl_signal_add(&toplevel->base->surface->events.commit, &win->commit);

	win->request_move.notify = toplevel_request_move;
	wl_signal_add(&toplevel->events.request_move, &win->request_move);
	win->request_resize.notify = toplevel_request_resize;
	wl_signal_add(&toplevel->events.request_resize, &win->request_resize);
	win->request_maximize.notify = toplevel_request_maximize;
	wl_signal_add(&toplevel->events.request_maximize, &win->request_maximize);
	win->request_fullscreen.notify = toplevel_request_fullscreen;
	wl_signal_add(&toplevel->events.request_fullscreen, &win->request_fullscreen);
	win->request_minimize.notify = toplevel_request_minimize;
	wl_signal_add(&toplevel->events.request_minimize, &win->request_minimize);

	wl_list_insert(&server->windows, &win->link);
}

/* ------------------------------------------------------------------- */
/* Input                                                                 */
/* ------------------------------------------------------------------- */

static struct wlr_surface *surface_at_cursor(struct wc_server *server,
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

static void process_cursor_motion(struct wc_server *server, uint32_t time_msec) {
	double sx = 0, sy = 0;
	struct wlr_surface *surface = surface_at_cursor(server, &sx, &sy);
	if (surface) {
		wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
		wlr_seat_pointer_notify_motion(server->seat, time_msec, sx, sy);
	} else {
		wlr_seat_pointer_clear_focus(server->seat);
	}
}

static void server_cursor_motion(struct wl_listener *listener, void *data) {
	struct wc_server *server = wl_container_of(listener, server, cursor_motion);
	struct wlr_pointer_motion_event *event = data;
	wlr_cursor_move(server->cursor, &event->pointer->base,
		event->delta_x, event->delta_y);
	process_cursor_motion(server, event->time_msec);
}

static void server_cursor_motion_absolute(struct wl_listener *listener, void *data) {
	struct wc_server *server =
		wl_container_of(listener, server, cursor_motion_absolute);
	struct wlr_pointer_motion_absolute_event *event = data;
	wlr_cursor_warp_absolute(server->cursor, &event->pointer->base,
		event->x, event->y);
	process_cursor_motion(server, event->time_msec);
}

static struct wc_window *window_from_surface(struct wlr_surface *surface) {
	struct wlr_xdg_surface *xdg_surface = wlr_xdg_surface_try_from_wlr_surface(surface);
	if (!xdg_surface || xdg_surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
		return NULL;
	}
	return xdg_surface->data;
}

static void server_cursor_button(struct wl_listener *listener, void *data) {
	struct wc_server *server = wl_container_of(listener, server, cursor_button);
	struct wlr_pointer_button_event *event = data;

	wlr_seat_pointer_notify_button(server->seat, event->time_msec,
		event->button, event->state);

	if ((uint32_t)event->state != (uint32_t)WLR_BUTTON_PRESSED) {
		return;
	}

	double sx, sy;
	struct wlr_surface *surface = surface_at_cursor(server, &sx, &sy);
	if (!surface) {
		return;
	}

	/* Click-to-focus. In practice a click inside a window will usually
	 * also cause the host WM to give it real X11 focus, generating a
	 * FocusIn we'd handle anyway; this is a same-path fallback for that,
	 * routed through set_active_window() so it can't drift out of sync
	 * with the ACTIVATED/keyboard-focus state that drives. */
	struct wc_window *win = window_from_surface(surface);
	if (win) {
		set_active_window(server, win);
	}
}

static void server_cursor_axis(struct wl_listener *listener, void *data) {
	struct wc_server *server = wl_container_of(listener, server, cursor_axis);
	struct wlr_pointer_axis_event *event = data;
	wlr_seat_pointer_notify_axis(server->seat, event->time_msec,
		event->orientation, event->delta, event->delta_discrete,
		event->source, event->relative_direction);
}

static void server_cursor_frame(struct wl_listener *listener, void *data) {
	struct wc_server *server = wl_container_of(listener, server, cursor_frame);
	wlr_seat_pointer_notify_frame(server->seat);
}

static void keyboard_modifiers(struct wl_listener *listener, void *data) {
	struct wc_keyboard *kb = wl_container_of(listener, kb, modifiers);
	wlr_seat_set_keyboard(kb->server->seat, kb->wlr_keyboard);
	wlr_seat_keyboard_notify_modifiers(kb->server->seat, &kb->wlr_keyboard->modifiers);
}

static void keyboard_key(struct wl_listener *listener, void *data) {
	struct wc_keyboard *kb = wl_container_of(listener, kb, key);
	struct wlr_keyboard_key_event *event = data;
	wlr_seat_set_keyboard(kb->server->seat, kb->wlr_keyboard);
	wlr_seat_keyboard_notify_key(kb->server->seat, event->time_msec,
		event->keycode, event->state);
}

static void keyboard_destroy(struct wl_listener *listener, void *data) {
	struct wc_keyboard *kb = wl_container_of(listener, kb, destroy);
	wl_list_remove(&kb->modifiers.link);
	wl_list_remove(&kb->key.link);
	wl_list_remove(&kb->destroy.link);
	free(kb);
}

static void server_new_input(struct wl_listener *listener, void *data) {
	struct wc_server *server = wl_container_of(listener, server, new_input);
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

		struct wc_keyboard *kb = calloc(1, sizeof(*kb));
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

static void server_request_set_selection(struct wl_listener *listener, void *data) {
	struct wc_server *server =
		wl_container_of(listener, server, request_set_selection);
	struct wlr_seat_request_set_selection_event *event = data;
	wlr_seat_set_selection(server->seat, event->source, event->serial);
}

/* ------------------------------------------------------------------- */
/* Signal handling (clean shutdown)                                     */
/* ------------------------------------------------------------------- */

static struct wl_display *g_display_for_signal;

static int handle_terminate_signal(int signal_number, void *data) {
	(void)signal_number;
	struct wl_display *display = data;
	wl_display_terminate(display);
	return 0;
}

/* ------------------------------------------------------------------- */
/* main                                                                  */
/* ------------------------------------------------------------------- */

int main(int argc, char **argv) {
	(void)argc;
	(void)argv;

	wlr_log_init(WLR_INFO, NULL);

	const char *x11_display = getenv("DISPLAY");
	if (!x11_display) {
		fprintf(stderr,
			"wc-x11: $DISPLAY is not set. This compositor must be run "
			"inside an existing X11 session (e.g. from an xterm on your "
			"desktop, or via `xinit`).\n");
		return 1;
	}

	struct wc_server server = {0};
	server.wl_display = wl_display_create();
	struct wl_event_loop *loop = wl_display_get_event_loop(server.wl_display);
	server.loop = loop;

	server.backend = wlr_x11_backend_create(loop, x11_display);
	if (!server.backend) {
		fprintf(stderr, "wc-x11: failed to create X11 backend for DISPLAY=%s\n",
			x11_display);
		return 1;
	}

	server.renderer = wlr_renderer_autocreate(server.backend);
	if (!server.renderer) {
		fprintf(stderr, "wc-x11: failed to create renderer\n");
		return 1;
	}
	wlr_renderer_init_wl_display(server.renderer, server.wl_display);

	server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
	if (!server.allocator) {
		fprintf(stderr, "wc-x11: failed to create allocator\n");
		return 1;
	}

	wlr_compositor_create(server.wl_display, 5, server.renderer);
	wlr_subcompositor_create(server.wl_display);
	wlr_data_device_manager_create(server.wl_display);

	server.output_layout = wlr_output_layout_create(server.wl_display);
	server.scene = wlr_scene_create();
	server.scene_layout =
		wlr_scene_attach_output_layout(server.scene, server.output_layout);

	wl_list_init(&server.windows);

	server.xdg_shell = wlr_xdg_shell_create(server.wl_display, 3);
	server.new_xdg_toplevel.notify = server_new_xdg_toplevel;
	wl_signal_add(&server.xdg_shell->events.new_toplevel, &server.new_xdg_toplevel);

	server.xdg_decoration_manager = wlr_xdg_decoration_manager_v1_create(server.wl_display);
	server.new_toplevel_decoration.notify = server_new_toplevel_decoration;
	wl_signal_add(&server.xdg_decoration_manager->events.new_toplevel_decoration,
		&server.new_toplevel_decoration);

	server.seat = wlr_seat_create(server.wl_display, "seat0");
	server.cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(server.cursor, server.output_layout);

	server.new_input.notify = server_new_input;
	wl_signal_add(&server.backend->events.new_input, &server.new_input);

	server.cursor_motion.notify = server_cursor_motion;
	wl_signal_add(&server.cursor->events.motion, &server.cursor_motion);
	server.cursor_motion_absolute.notify = server_cursor_motion_absolute;
	wl_signal_add(&server.cursor->events.motion_absolute,
		&server.cursor_motion_absolute);
	server.cursor_button.notify = server_cursor_button;
	wl_signal_add(&server.cursor->events.button, &server.cursor_button);
	server.cursor_axis.notify = server_cursor_axis;
	wl_signal_add(&server.cursor->events.axis, &server.cursor_axis);
	server.cursor_frame.notify = server_cursor_frame;
	wl_signal_add(&server.cursor->events.frame, &server.cursor_frame);

	server.request_set_selection.notify = server_request_set_selection;
	wl_signal_add(&server.seat->events.request_set_selection,
		&server.request_set_selection);

	/* Auxiliary XCB connection, used only for setting WM_NAME/WM_CLASS. */
	server.xcb = xcb_connect(x11_display, NULL);
	if (!server.xcb || xcb_connection_has_error(server.xcb)) {
		wlr_log(WLR_ERROR,
			"failed to open auxiliary X11 connection; window titles/classes "
			"will not be synced");
	} else {
		const xcb_setup_t *setup = xcb_get_setup(server.xcb);
		xcb_screen_t *screen = xcb_setup_roots_iterator(setup).data;
		server.xcb_root = screen->root;
		server.atom_net_wm_name = intern_atom(server.xcb, "_NET_WM_NAME");
		server.atom_utf8_string = intern_atom(server.xcb, "UTF8_STRING");
		server.atom_net_wm_moveresize = intern_atom(server.xcb, "_NET_WM_MOVERESIZE");
		server.atom_net_wm_state = intern_atom(server.xcb, "_NET_WM_STATE");
		server.atom_net_wm_state_maximized_vert =
			intern_atom(server.xcb, "_NET_WM_STATE_MAXIMIZED_VERT");
		server.atom_net_wm_state_maximized_horz =
			intern_atom(server.xcb, "_NET_WM_STATE_MAXIMIZED_HORZ");
		server.atom_net_wm_state_fullscreen =
			intern_atom(server.xcb, "_NET_WM_STATE_FULLSCREEN");
		server.atom_wm_change_state = intern_atom(server.xcb, "WM_CHANGE_STATE");

		wlr_log(WLR_INFO,
			"resolved atoms: _NET_WM_NAME=%u UTF8_STRING=%u "
			"_NET_WM_MOVERESIZE=%u _NET_WM_STATE=%u "
			"_NET_WM_STATE_MAXIMIZED_VERT=%u _NET_WM_STATE_MAXIMIZED_HORZ=%u "
			"_NET_WM_STATE_FULLSCREEN=%u WM_CHANGE_STATE=%u (0 = FAILED)",
			server.atom_net_wm_name, server.atom_utf8_string,
			server.atom_net_wm_moveresize, server.atom_net_wm_state,
			server.atom_net_wm_state_maximized_vert,
			server.atom_net_wm_state_maximized_horz,
			server.atom_net_wm_state_fullscreen, server.atom_wm_change_state);
		server.default_cursor = create_default_cursor(server.xcb, screen);
		wl_event_loop_add_fd(loop, xcb_get_file_descriptor(server.xcb),
			WL_EVENT_READABLE, handle_xcb_readable, &server);
	}

	const char *socket = wl_display_add_socket_auto(server.wl_display);
	if (!socket) {
		fprintf(stderr, "wc-x11: failed to create Wayland socket\n");
		return 1;
	}

	if (!wlr_backend_start(server.backend)) {
		fprintf(stderr, "wc-x11: failed to start X11 backend\n");
		wl_display_destroy(server.wl_display);
		return 1;
	}

	setenv("WAYLAND_DISPLAY", socket, true);

	g_display_for_signal = server.wl_display;
	server.sigint_source = wl_event_loop_add_signal(loop, SIGINT,
		handle_terminate_signal, server.wl_display);
	server.sigterm_source = wl_event_loop_add_signal(loop, SIGTERM,
		handle_terminate_signal, server.wl_display);

	fprintf(stderr,
		"wc-x11: running. WAYLAND_DISPLAY=%s (nested inside X11 DISPLAY=%s)\n"
		"wc-x11: start clients with, e.g.:\n"
		"wc-x11:   WAYLAND_DISPLAY=%s weston-terminal\n"
		"wc-x11:   WAYLAND_DISPLAY=%s foot\n",
		socket, x11_display, socket, socket);

	wl_display_run(server.wl_display);

	wl_display_destroy_clients(server.wl_display);
	wlr_scene_node_destroy(&server.scene->tree.node);
	if (server.xcb) {
		xcb_disconnect(server.xcb);
	}
	wl_display_destroy(server.wl_display);
	return 0;
}
