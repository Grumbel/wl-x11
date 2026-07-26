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
#include <errno.h>
#include <sys/wait.h>

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

	/* For `wc-x11 <command>`: track connected Wayland clients so we can
	 * shut down once none remain, rather than just watching the spawned
	 * process (which may fork/re-exec into a different process that
	 * actually holds the Wayland connection). See client_created_notify()
	 * and main(). */
	struct wl_listener client_created;
	int active_clients;
	bool have_seen_client;
	bool exit_when_clients_gone;
	pid_t launched_pid;

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

	/* Self-driven interactive move/resize state.
	 *
	 * We don't delegate this to the host WM via _NET_WM_MOVERESIZE: doing
	 * so put xfwm4's own interactive grab attempt in conflict with
	 * wlroots' pre-existing implicit grab (X11 automatically grants an
	 * implicit active grab to whichever client owns the window a button
	 * was pressed in, lasting until release -- since that's wlroots' own
	 * window, its connection holds it for the whole drag, and no other
	 * client, including xfwm4, can compete for the pointer meanwhile),
	 * producing an X bell and no actual move/resize happening. We also
	 * tried taking our own competing grab (fails for the same reason),
	 * and driving movement from accumulated per-event deltas computed
	 * from wlroots' own motion events (both relative and, after ruling
	 * that out, raw pre-clamp absolute device coordinates, and a
	 * possible multi-device mixup) -- all of which kept drifting/
	 * oscillating, because every one of those was delta-accumulation
	 * based: each update built on the previous computed position, so any
	 * single noisy or inconsistent sample corrupted every update after
	 * it, permanently.
	 *
	 * This is self-correcting instead: at drag start we query the real,
	 * ground-truth pointer position directly from the X server (via
	 * xcb_query_pointer on our own connection -- bypasses wlroots'
	 * cursor entirely, no clamping, no per-device normalization
	 * ambiguity) and compute a fixed offset between it and the window's
	 * real position. On every throttled update we re-query the real
	 * pointer position fresh and simply set window_position = pointer
	 * position + offset. There's no running accumulator for noise to
	 * corrupt -- a single bad sample only affects that one update, not
	 * everything after it. */
	struct wc_window *move_win;
	int move_offset_x, move_offset_y; /* window pos - pointer pos at drag start */

	struct wc_window *resize_win;
	uint32_t resize_edges;
	int resize_start_x, resize_start_y, resize_start_w, resize_start_h;
	int16_t resize_start_pointer_x, resize_start_pointer_y;

	/* Throttles how often we actually poll+configure -- see
	 * WC_DRAG_THROTTLE_MS. */
	struct timespec drag_last_send_at;

	/* Closed-loop correction: testing showed xfwm4 places the frame at a
	 * position systematically offset from what we request (consistently
	 * +4,+24 in one test -- lines up with the border+titlebar size, but
	 * we don't assume a fixed value since that'd be WM-theme-specific).
	 * We track the most recent (x,y) we asked for, and the moment we see
	 * a real ConfigureNotify for the dragged window (in
	 * handle_xcb_readable()) we learn correction = observed - requested
	 * and subtract it from future requests, so the discrepancy converges
	 * to zero after the first real round trip instead of staying as a
	 * constant, uncorrected jump for the whole drag. */
	int drag_last_requested_x, drag_last_requested_y;
	int drag_correction_x, drag_correction_y;

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

	xcb_window_t xwin;
#define WC_MAX_RELATED_WINDOWS 32
	xcb_window_t related[WC_MAX_RELATED_WINDOWS];
	int related_count;
	xcb_window_t content_xwin; /* the real wlroots-owned window, distinct
	                            * from xwin (the WM's decoration frame) */
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
	struct wl_listener output_request_state;

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

/* EWMH/ICCCM window-management messages must target the real client
 * window (content_xwin), not xfwm4's own decoration frame (xwin) -- see
 * find_content_window() for why. Falls back to xwin if we failed to
 * identify it, which will likely just be ignored by the WM but is
 * better than sending nowhere. */
static xcb_window_t ewmh_target_window(struct wc_window *win) {
	return win->content_xwin != XCB_WINDOW_NONE ? win->content_xwin : win->xwin;
}

/* Real root-relative position of a window's origin, robust regardless of
 * how deep it's nested (handles the WM's reparenting for us). */
static bool query_window_root_position(struct wc_server *s, xcb_window_t w,
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

static bool query_window_geometry(struct wc_server *s, xcb_window_t w,
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

/* xcb_get_geometry's x/y fields are defined as position relative to the
 * window's immediate parent -- exactly what `xwininfo` shows as "Relative
 * upper-left X/Y". For win->content_xwin, whose parent is win->xwin (the
 * WM's decoration frame), this directly gives the border+titlebar inset:
 * no need to infer it by learning from ConfigureNotify feedback. */
static bool query_window_relative_offset(struct wc_server *s, xcb_window_t w,
		int16_t *x, int16_t *y) {
	if (!s->xcb || xcb_connection_has_error(s->xcb) || w == XCB_WINDOW_NONE) {
		return false;
	}
	xcb_get_geometry_cookie_t cookie = xcb_get_geometry(s->xcb, w);
	xcb_get_geometry_reply_t *reply = xcb_get_geometry_reply(s->xcb, cookie, NULL);
	if (!reply) {
		return false;
	}
	*x = reply->x;
	*y = reply->y;
	free(reply);
	return true;
}

/* Real, ground-truth root-relative pointer position, queried directly
 * from the X server -- bypasses wlroots' own cursor tracking entirely
 * (no output-layout clamping, no per-device normalization, no risk of
 * mixing data from multiple input devices). See the comment on struct
 * wc_server's move_win field for why this matters. */
static bool query_root_pointer_position(struct wc_server *s, int16_t *x, int16_t *y) {
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

#define WC_DRAG_THROTTLE_MS 16

/* Window we actually xcb_configure during interactive move/resize.
 * Must be the real client window (content_xwin), not the WM frame:
 * ICCCM requires clients to configure their own window; the WM then
 * moves/resizes the frame. Configuring the frame is undefined and
 * breaks on many WMs. Falls back to xwin when content is unknown. */
static xcb_window_t configure_target_window(struct wc_window *win) {
	return ewmh_target_window(win);
}

static void begin_interactive_move(struct wc_window *win) {
	struct wc_server *server = win->server;
	xcb_window_t target = configure_target_window(win);
	int16_t wx, wy, px, py;
	if (target == XCB_WINDOW_NONE ||
			!query_window_root_position(server, target, &wx, &wy) ||
			!query_root_pointer_position(server, &px, &py)) {
		return;
	}
	server->move_win = win;
	server->move_offset_x = wx - px;
	server->move_offset_y = wy - py;
	server->drag_last_send_at = (struct timespec){0};

	/* When target is the content window parented under a frame, root
	 * position already reflects the final on-screen origin of the
	 * client area; no extra inset correction is needed. Only apply
	 * relative-offset correction when we are still configuring the
	 * frame itself (content unknown). */
	server->drag_correction_x = 0;
	server->drag_correction_y = 0;
	if (target == win->xwin && win->content_xwin != XCB_WINDOW_NONE &&
			win->content_xwin != win->xwin) {
		int16_t inset_x = 0, inset_y = 0;
		if (query_window_relative_offset(server, win->content_xwin,
				&inset_x, &inset_y)) {
			server->drag_correction_x = inset_x;
			server->drag_correction_y = inset_y;
		}
	}

	wlr_log(WLR_INFO, "starting self-driven interactive move of window "
		"0x%x (configure target 0x%x) at (%d,%d), pointer at (%d,%d), "
		"offset (%d,%d), correction (%d,%d)", win->xwin, target, wx, wy,
		px, py, server->move_offset_x, server->move_offset_y,
		server->drag_correction_x, server->drag_correction_y);
}

static void begin_interactive_resize(struct wc_window *win, uint32_t edges) {
	struct wc_server *server = win->server;
	xcb_window_t target = configure_target_window(win);
	int16_t wx, wy, px, py;
	int w, h;
	if (target == XCB_WINDOW_NONE ||
			!query_window_root_position(server, target, &wx, &wy) ||
			!query_window_geometry(server, target, &w, &h) ||
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
	if (target == win->xwin && win->content_xwin != XCB_WINDOW_NONE &&
			win->content_xwin != win->xwin) {
		int16_t inset_x = 0, inset_y = 0;
		if (query_window_relative_offset(server, win->content_xwin,
				&inset_x, &inset_y)) {
			server->drag_correction_x = inset_x;
			server->drag_correction_y = inset_y;
		}
	}

	wlr_log(WLR_INFO, "starting self-driven interactive resize of window "
		"0x%x (configure target 0x%x, edges 0x%x) at (%d,%d) %dx%d, "
		"pointer at (%d,%d), correction (%d,%d)", win->xwin, target,
		edges, wx, wy, w, h, px, py,
		server->drag_correction_x, server->drag_correction_y);
}

static void toplevel_request_move(struct wl_listener *listener, void *data) {
	(void)data;
	struct wc_window *win = wl_container_of(listener, win, request_move);
	wlr_log(WLR_INFO, "toplevel requested move");
	begin_interactive_move(win);
}

static void toplevel_request_resize(struct wl_listener *listener, void *data) {
	struct wc_window *win = wl_container_of(listener, win, request_resize);
	struct wlr_xdg_toplevel_resize_event *event = data;
	wlr_log(WLR_INFO, "toplevel requested resize (edges 0x%x)", event->edges);
	begin_interactive_resize(win, event->edges);
}

static void toplevel_request_maximize(struct wl_listener *listener, void *data) {
	(void)data;
	struct wc_window *win = wl_container_of(listener, win, request_maximize);
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

static void toplevel_request_fullscreen(struct wl_listener *listener, void *data) {
	(void)data;
	struct wc_window *win = wl_container_of(listener, win, request_fullscreen);
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

static void toplevel_request_minimize(struct wl_listener *listener, void *data) {
	(void)data;
	struct wc_window *win = wl_container_of(listener, win, request_minimize);
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
	/* FOCUS_CHANGE is the normal case (resize is otherwise handled via
	 * wlroots' own request_state signal, not by us watching
	 * ConfigureNotify). STRUCTURE_NOTIFY is temporarily back too, purely
	 * to observe (log only, no reaction) what xfwm4 actually does to a
	 * window's real geometry during an interactive move/resize drag, to
	 * get ground truth after three different drag-driving mechanisms
	 * all produced the same jump/wall symptom -- see the
	 * XCB_CONFIGURE_NOTIFY case in handle_xcb_readable(). */
	uint32_t mask = XCB_EVENT_MASK_FOCUS_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY;
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
static void register_x11_window_subtree(struct wc_window *win, xcb_window_t w) {
	struct wc_server *server = win->server;

	select_window_events(server, w);
	wlr_log(WLR_INFO, "registered X11 window 0x%x for toplevel \"%s\"",
		w, win->toplevel && win->toplevel->title ? win->toplevel->title : "?");

	if (win->related_count < WC_MAX_RELATED_WINDOWS) {
		win->related[win->related_count++] = w;
	} else {
		wlr_log(WLR_ERROR, "window subtree exceeds WC_MAX_RELATED_WINDOWS, "
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

/* win->xwin (found via our root-children diff) is xfwm4's own decoration
 * frame, not the window wlroots actually created and owns (the private
 * struct wlr_x11_output's `win` field, which the public API doesn't
 * expose). EWMH/ICCCM window-management messages (_NET_WM_STATE,
 * _NET_WM_MOVERESIZE, WM_CHANGE_STATE) must target that real client
 * window -- WMs track requests by the original client id, and have no
 * reason to handle messages addressed to their own frame, which isn't a
 * "client" in their bookkeeping at all. This heuristically identifies
 * that window among the registered subtree: it's overwhelmingly the
 * largest-area descendant, since decoration widgets (title bar strip,
 * borders, buttons) are comparatively tiny. */
static xcb_window_t find_content_window(struct wc_server *server, struct wc_window *win) {
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
		wlr_log(WLR_ERROR, "could not identify a content window under "
			"frame 0x%x; window-management requests (move/resize/"
			"maximize/minimize) will fall back to targeting the frame "
			"and likely be ignored by the WM", win->xwin);
	}
	return best;
}

/* Earlier versions of this file tried to detect X11-driven resizes
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
static void output_request_state(struct wl_listener *listener, void *data) {
	struct wc_window *win = wl_container_of(listener, win, output_request_state);
	const struct wlr_output_event_request_state *event = data;

	wlr_log(WLR_INFO, "output requested state (backend-detected resize) "
		"-> accepting");
	/* output_commit() (below) fires synchronously as part of this call
	 * and handles diffing the new size against what we last told the
	 * toplevel and forwarding it if different -- nothing further needed
	 * here. */
	wlr_output_commit_state(win->output, event->state);
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
		if (type == XCB_FOCUS_IN) {
			xcb_focus_in_event_t *fi = (xcb_focus_in_event_t *)event;
			struct wc_window *win = window_from_xwin(server, fi->event);
			if (win) {
				wlr_log(WLR_INFO, "X11 FocusIn on window 0x%x", fi->event);
				set_active_window(server, win);
			}
		} else if (type == XCB_FOCUS_OUT) {
			xcb_focus_out_event_t *fo = (xcb_focus_out_event_t *)event;
			/* Match the same way as FocusIn: focus may land on the
			 * content window or a decoration child, not only the frame.
			 * detail == Inferior means focus moved to a child of this
			 * window (still inside our toplevel subtree) — do not clear. */
			struct wc_window *win = window_from_xwin(server, fo->event);
			if (win && server->focused_window == win &&
					fo->detail != XCB_NOTIFY_DETAIL_INFERIOR) {
				wlr_log(WLR_INFO, "X11 FocusOut on window 0x%x (detail %u)",
					fo->event, fo->detail);
				set_active_window(server, NULL);
			}
		} else if (type == XCB_REPARENT_NOTIFY) {
			/* WM reparented our window into a decoration frame (or the
			 * reverse). Update frame vs. content identity and re-walk the
			 * subtree so related[] tracks decoration widgets too. */
			xcb_reparent_notify_event_t *rn =
				(xcb_reparent_notify_event_t *)event;
			struct wc_window *win = window_from_xwin(server, rn->window);
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
				xwin_set_default_cursor(server, win->content_xwin);
			}
		} else if (type == XCB_CONFIGURE_NOTIFY) {
			/* Learn the WM's systematic position discrepancy from real
			 * feedback (see the comment on struct wc_server's
			 * drag_correction_x field) rather than assuming a fixed
			 * value, since it's WM-theme-specific. Match against the
			 * same window we actually configure (content when known). */
			if (server->move_win || server->resize_win) {
				xcb_configure_notify_event_t *cn =
					(xcb_configure_notify_event_t *)event;
				struct wc_window *active =
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
						wlr_log(WLR_INFO, "[DIAG] real ConfigureNotify for "
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
	 * it. Only on the content window, not the frame -- xfwm4 needs to
	 * keep control of its own decoration cursors (resize arrows, etc). */
	xwin_set_default_cursor(win->server, win->content_xwin);

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

#define WC_DEFAULT_WIDTH 1024
#define WC_DEFAULT_HEIGHT 720

static void create_output_for_window(struct wc_window *win) {
	struct wc_server *server = win->server;

	wlr_log(WLR_INFO, "mapping toplevel \"%s\" (app_id \"%s\") to a new X11 window",
		win->toplevel->title ? win->toplevel->title : "(no title)",
		win->toplevel->app_id ? win->toplevel->app_id : "(no app_id)");

	/* Fresh X11 identity state (also cleared in output_destroy; reset
	 * here so a remap never carries stale related[] entries). */
	win->xwin = XCB_WINDOW_NONE;
	win->content_xwin = XCB_WINDOW_NONE;
	win->related_count = 0;
	memset(win->related, 0, sizeof(win->related));

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
	win->output_request_state.notify = output_request_state;
	wl_signal_add(&output->events.request_state, &win->output_request_state);

	int w = output->width > 0 ? output->width : WC_DEFAULT_WIDTH;
	int h = output->height > 0 ? output->height : WC_DEFAULT_HEIGHT;
	wlr_xdg_toplevel_set_size(win->toplevel, w, h);

	/* Best-effort: find the xcb_window_t the backend just created so we
	 * can set WM_NAME / WM_CLASS on it. Prefer a newly appeared root
	 * child whose size matches the output we just committed — reduces
	 * mis-attribution when several toplevels map concurrently. */
	xcb_roundtrip(server->xcb);
	xcb_window_t *after = NULL;
	int after_n = 0;
	query_root_children(server->xcb, server->xcb_root, &after, &after_n);

	win->xwin = XCB_WINDOW_NONE;
	xcb_window_t fallback = XCB_WINDOW_NONE;
	int want_w = output->width > 0 ? output->width : WC_DEFAULT_WIDTH;
	int want_h = output->height > 0 ? output->height : WC_DEFAULT_HEIGHT;
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
		register_x11_window_subtree(win, win->xwin);
		win->content_xwin = find_content_window(server, win);
		/* Before the WM reparents, the root child *is* the client
		 * window — treat it as content so EWMH/configure have a valid
		 * target. ReparentNotify will refresh this later. */
		if (win->content_xwin == XCB_WINDOW_NONE) {
			win->content_xwin = win->xwin;
		}
		xwin_set_default_cursor(server, win->content_xwin);
		/* Set on both: the frame (win->xwin), since that's what xfwm4
		 * appears to actually display, and the content window (the
		 * correct ICCCM target), for robustness across other WMs/tools
		 * that read WM_NAME from the real client window instead. */
		xwin_set_title(server, win->xwin, win->toplevel->title);
		xwin_set_class(server, win->xwin, win->toplevel->app_id);
		xwin_set_title(server, win->content_xwin, win->toplevel->title);
		xwin_set_class(server, win->content_xwin, win->toplevel->app_id);
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
		xwin_set_title(win->server, win->content_xwin, title);
		snprintf(win->last_title, sizeof(win->last_title), "%s", title);
	}
	if (strncmp(app_id, win->last_app_id, sizeof(win->last_app_id)) != 0) {
		xwin_set_class(win->server, win->xwin, app_id);
		xwin_set_class(win->server, win->content_xwin, app_id);
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
	if (win->server->move_win == win) {
		win->server->move_win = NULL;
	}
	if (win->server->resize_win == win) {
		win->server->resize_win = NULL;
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
	if (!deco) {
		wlr_log(WLR_ERROR, "out of memory allocating decoration tracker");
		return;
	}
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
	if (!win) {
		wlr_log(WLR_ERROR, "out of memory allocating wc_window");
		return;
	}
	win->server = server;
	win->toplevel = toplevel;
	win->xwin = XCB_WINDOW_NONE;

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

/* Returns true if enough time has passed since the last actual
 * xcb_configure_window() call that we should send another one now. */
static bool drag_throttle_ready(struct wc_server *server) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	long elapsed_ms = (now.tv_sec - server->drag_last_send_at.tv_sec) * 1000 +
		(now.tv_nsec - server->drag_last_send_at.tv_nsec) / 1000000;
	if (elapsed_ms < WC_DRAG_THROTTLE_MS) {
		return false;
	}
	server->drag_last_send_at = now;
	return true;
}

#define WC_MIN_WINDOW_SIZE 50

/* Called whenever we get any pointer motion at all while a drag is
 * active; throttled to avoid flooding xfwm4's asynchronous
 * ConfigureRequest handling with one call per raw motion event. Always
 * re-queries the real pointer position fresh (see
 * query_root_pointer_position()) rather than trusting any accumulated
 * state, so a single noisy sample can't corrupt anything beyond itself. */
static void update_interactive_drag(struct wc_server *server) {
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

	/* resize */
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
	if (w < WC_MIN_WINDOW_SIZE) {
		w = WC_MIN_WINDOW_SIZE;
	}
	if (h < WC_MIN_WINDOW_SIZE) {
		h = WC_MIN_WINDOW_SIZE;
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

static void process_cursor_motion(struct wc_server *server, uint32_t time_msec) {
	if (server->move_win || server->resize_win) {
		update_interactive_drag(server);
		return;
	}

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
		if (server->move_win) {
			wlr_log(WLR_INFO, "ending self-driven interactive move");
			server->move_win = NULL;
		}
		if (server->resize_win) {
			wlr_log(WLR_INFO, "ending self-driven interactive resize");
			server->resize_win = NULL;
		}
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

static int handle_sigchld(int signal_number, void *data) {
	(void)signal_number;
	(void)data;
	int status;
	pid_t pid;
	while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
		wlr_log(WLR_INFO, "reaped child process %d", pid);
	}
	return 0;
}

/* ------------------------------------------------------------------- */
/* `wc-x11 <command>`: launch a client alongside the compositor, and    */
/* shut down once no Wayland clients remain connected                  */
/* ------------------------------------------------------------------- */

struct wc_client_track {
	struct wc_server *server;
	struct wl_listener destroy;
};

static void client_destroy_notify(struct wl_listener *listener, void *data) {
	(void)data;
	struct wc_client_track *track = wl_container_of(listener, track, destroy);
	struct wc_server *server = track->server;

	server->active_clients--;
	wlr_log(WLR_INFO, "wayland client disconnected (%d remaining)",
		server->active_clients);

	if (server->exit_when_clients_gone && server->have_seen_client &&
			server->active_clients <= 0) {
		wlr_log(WLR_INFO,
			"no more wayland clients connected, shutting down");
		wl_display_terminate(server->wl_display);
	}

	wl_list_remove(&track->destroy.link);
	free(track);
}

static void client_created_notify(struct wl_listener *listener, void *data) {
	struct wc_server *server = wl_container_of(listener, server, client_created);
	struct wl_client *client = data;

	server->active_clients++;
	server->have_seen_client = true;
	wlr_log(WLR_INFO, "wayland client connected (%d active)",
		server->active_clients);

	struct wc_client_track *track = calloc(1, sizeof(*track));
	if (!track) {
		wlr_log(WLR_ERROR, "out of memory allocating client tracker");
		return;
	}
	track->server = server;
	track->destroy.notify = client_destroy_notify;
	wl_client_add_destroy_listener(client, &track->destroy);
}

/* ------------------------------------------------------------------- */
/* main                                                                  */
/* ------------------------------------------------------------------- */

int main(int argc, char **argv) {
	bool debug = false;
	char **command_argv = NULL;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--debug") == 0) {
			debug = true;
		} else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
			printf("usage: wc-x11 [--debug] [command [args...]]\n"
				"  --debug             print verbose diagnostic logging "
				"(default: only errors)\n"
				"  command [args...]   also launch this program with "
				"WAYLAND_DISPLAY set,\n"
				"                      and shut down once no Wayland "
				"clients remain connected\n");
			return 0;
		} else {
			/* First non-flag argument: everything from here on is the
			 * command to launch, not further wc-x11 flags. */
			command_argv = &argv[i];
			break;
		}
	}

	/* All of our own diagnostic wlr_log(WLR_INFO, ...) calls throughout
	 * this file are gated by this: at the default WLR_ERROR level they
	 * simply won't print, leaving only genuine errors (and whatever
	 * wlroots itself logs at WLR_ERROR) on stderr. --debug raises it
	 * back to WLR_INFO to see everything. */
	wlr_log_init(debug ? WLR_INFO : WLR_ERROR, NULL);

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

	server.client_created.notify = client_created_notify;
	wl_display_add_client_created_listener(server.wl_display, &server.client_created);

	g_display_for_signal = server.wl_display;
	server.sigint_source = wl_event_loop_add_signal(loop, SIGINT,
		handle_terminate_signal, server.wl_display);
	server.sigterm_source = wl_event_loop_add_signal(loop, SIGTERM,
		handle_terminate_signal, server.wl_display);
	wl_event_loop_add_signal(loop, SIGCHLD, handle_sigchld, NULL);

	server.launched_pid = -1;
	if (command_argv) {
		server.exit_when_clients_gone = true;
		pid_t pid = fork();
		if (pid == 0) {
			/* Child: WAYLAND_DISPLAY was set via setenv() above, so it's
			 * already inherited through environ; just exec. */
			execvp(command_argv[0], command_argv);
			fprintf(stderr, "wc-x11: failed to exec '%s': %s\n",
				command_argv[0], strerror(errno));
			_exit(127);
		} else if (pid < 0) {
			fprintf(stderr, "wc-x11: fork() failed: %s\n", strerror(errno));
			server.exit_when_clients_gone = false;
		} else {
			server.launched_pid = pid;
			wlr_log(WLR_INFO, "launched '%s' (pid %d) with WAYLAND_DISPLAY=%s",
				command_argv[0], pid, socket);
		}
	}

	fprintf(stderr,
		"wc-x11: running. WAYLAND_DISPLAY=%s (nested inside X11 DISPLAY=%s)\n",
		socket, x11_display);
	if (command_argv) {
		fprintf(stderr,
			"wc-x11: launched '%s'; will exit once no Wayland clients "
			"remain connected\n", command_argv[0]);
	} else {
		fprintf(stderr,
			"wc-x11: start clients with, e.g.:\n"
			"wc-x11:   WAYLAND_DISPLAY=%s weston-terminal\n"
			"wc-x11:   WAYLAND_DISPLAY=%s foot\n",
			socket, socket);
	}

	wl_display_run(server.wl_display);

	if (server.launched_pid > 0) {
		/* Best-effort: don't leave the launched app running (or its
		 * children orphaned) after the compositor exits. */
		kill(server.launched_pid, SIGTERM);
	}

	wl_display_destroy_clients(server.wl_display);
	wlr_scene_node_destroy(&server.scene->tree.node);
	if (server.xcb) {
		xcb_disconnect(server.xcb);
	}
	wl_display_destroy(server.wl_display);
	return 0;
}
