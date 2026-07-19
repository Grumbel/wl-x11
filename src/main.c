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
#include <wlr/util/log.h>

#include <xkbcommon/xkbcommon.h>

#include <xcb/xcb.h>

/* ------------------------------------------------------------------- */
/* Types                                                                 */
/* ------------------------------------------------------------------- */

struct wc_server {
	struct wl_display *wl_display;
	struct wlr_backend *backend;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;

	struct wlr_scene *scene;
	struct wlr_scene_output_layout *scene_layout;
	struct wlr_output_layout *output_layout;

	struct wlr_xdg_shell *xdg_shell;
	struct wl_listener new_xdg_toplevel;

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

	/* Auxiliary connection used only to set WM_NAME / WM_CLASS on the
	 * X11 windows that the wlroots X11 backend creates for us. */
	xcb_connection_t *xcb;
	xcb_window_t xcb_root;
	xcb_atom_t atom_net_wm_name;
	xcb_atom_t atom_utf8_string;

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

	xcb_window_t xwin;
	char last_title[256];
	char last_app_id[256];
	bool initial_configure_sent;

	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener destroy;
	struct wl_listener commit;

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

/* ------------------------------------------------------------------- */
/* Output (== one X11 window) lifecycle                                 */
/* ------------------------------------------------------------------- */

static void output_frame(struct wl_listener *listener, void *data) {
	struct wc_window *win = wl_container_of(listener, win, output_frame);
	if (!win->scene_output) {
		return;
	}
	wlr_scene_output_commit(win->scene_output, NULL);

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(win->scene_output, &now);
}

static void output_commit(struct wl_listener *listener, void *data) {
	struct wc_window *win = wl_container_of(listener, win, output_commit);
	struct wlr_output_event_commit *event = data;

	if (!(event->state->committed & WLR_OUTPUT_STATE_MODE)) {
		return;
	}

	int w = win->output->width;
	int h = win->output->height;
	if (w > 0 && h > 0 && win->toplevel) {
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

	wl_list_remove(&win->map.link);
	wl_list_remove(&win->unmap.link);
	wl_list_remove(&win->destroy.link);
	wl_list_remove(&win->commit.link);
	wl_list_remove(&win->link);
	free(win);
}

static void server_new_xdg_toplevel(struct wl_listener *listener, void *data) {
	struct wc_server *server = wl_container_of(listener, server, new_xdg_toplevel);
	struct wlr_xdg_toplevel *toplevel = data;

	wlr_log(WLR_INFO, "new xdg_toplevel created (not yet mapped)");

	struct wc_window *win = calloc(1, sizeof(*win));
	win->server = server;
	win->toplevel = toplevel;
	win->xwin = XCB_WINDOW_NONE;

	win->scene_tree = wlr_scene_xdg_surface_create(&server->scene->tree,
		toplevel->base);
	win->scene_tree->node.data = win;
	toplevel->base->data = win->scene_tree;

	win->map.notify = xdg_toplevel_map;
	wl_signal_add(&toplevel->base->surface->events.map, &win->map);
	win->unmap.notify = xdg_toplevel_unmap;
	wl_signal_add(&toplevel->base->surface->events.unmap, &win->unmap);
	win->destroy.notify = xdg_toplevel_destroy;
	wl_signal_add(&toplevel->events.destroy, &win->destroy);
	win->commit.notify = surface_commit;
	wl_signal_add(&toplevel->base->surface->events.commit, &win->commit);

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

static void server_cursor_button(struct wl_listener *listener, void *data) {
	struct wc_server *server = wl_container_of(listener, server, cursor_button);
	struct wlr_pointer_button_event *event = data;

	wlr_seat_pointer_notify_button(server->seat, event->time_msec,
		event->button, event->state);

	if (event->state != WLR_BUTTON_PRESSED) {
		return;
	}

	double sx, sy;
	struct wlr_surface *surface = surface_at_cursor(server, &sx, &sy);
	if (!surface) {
		return;
	}

	/* Click-to-focus for keyboard input. */
	struct wlr_keyboard *kb = wlr_seat_get_keyboard(server->seat);
	if (kb) {
		wlr_seat_keyboard_notify_enter(server->seat, surface,
			kb->keycodes, kb->num_keycodes, &kb->modifiers);
	} else {
		wlr_seat_keyboard_notify_enter(server->seat, surface, NULL, 0, NULL);
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
		server.xcb_root = xcb_setup_roots_iterator(setup).data->root;
		server.atom_net_wm_name = intern_atom(server.xcb, "_NET_WM_NAME");
		server.atom_utf8_string = intern_atom(server.xcb, "UTF8_STRING");
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
