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

void xdg_toplevel_map(struct wl_listener *listener, void *data) {
	struct wlx_window *win = wl_container_of(listener, win, map);
	wlr_log(WLR_INFO, "surface map event received");
	if (win->output) {
		return; /* already has a window (e.g. re-map) */
	}
	create_output_for_window(win);
	/* Dialogs and new windows should take keyboard focus immediately;
	 * host FocusIn often arrives a frame later (or not at all if the WM
	 * keeps focus on the parent). */
	if (win->output) {
		set_active_window(win->server, win);
	}
}

void xdg_toplevel_unmap(struct wl_listener *listener, void *data) {
	struct wlx_window *win = wl_container_of(listener, win, unmap);
	if (win->output) {
		/* wlr_output_destroy() synchronously fires events.destroy,
		 * which runs output_destroy() above and clears win->output. */
		wlr_output_destroy(win->output);
	}
}

void surface_commit(struct wl_listener *listener, void *data) {
	struct wlx_window *win = wl_container_of(listener, win, commit);
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

	/* Fit the X11 window to the client's geometry until the WM/user
	 * resizes it. Fixes transient dialogs that first mapped with a
	 * default size and only later committed their real size. */
	if (win->output && !win->size_from_wm && win->toplevel) {
		struct wlr_box geo = win->toplevel->base->current.geometry;
		/* Prefer xdg geometry (opaque window area). Buffer size can include
		 * transparent CSD shadow drawn as black without host alpha. */
		int cw = geo.width;
		int ch = geo.height;
		int out_w = wlx_scale_size(win->server, cw);
		int out_h = wlx_scale_size(win->server, ch);
		if (cw >= WLX_MIN_OUTPUT_SIZE && ch >= WLX_MIN_OUTPUT_SIZE &&
				(out_w != win->output->width || out_h != win->output->height)) {
			wlr_log(WLR_INFO, "fitting X11 window to client geometry %dx%d "
				"(output %dx%d at scale %.2f)",
				cw, ch, out_w, out_h, win->server->content_scale);
			resize_output_to(win, out_w, out_h);
			wlr_xdg_toplevel_set_size(win->toplevel, cw, ch);
			if (win->l_output) {
				wlr_scene_node_set_position(&win->scene_tree->node,
					win->l_output->x, win->l_output->y);
			}
		}
	}

	/* Re-apply pixel scale after the scene helper refreshed buffer dest sizes. */
	wlx_apply_content_scale(win);

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

void xdg_toplevel_destroy(struct wl_listener *listener, void *data) {
	struct wlx_window *win = wl_container_of(listener, win, destroy);

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

struct wlx_decoration {
	struct wl_listener request_mode;
	struct wl_listener destroy;
};

void decoration_request_mode(struct wl_listener *listener, void *data) {
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
	struct wlx_window *win = decoration->toplevel->base->data;
	if (win && !win->initial_configure_sent) {
		win->pending_decoration = decoration;
		return;
	}
	wlr_xdg_toplevel_decoration_v1_set_mode(decoration,
		WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

void decoration_destroy(struct wl_listener *listener, void *data) {
	struct wlr_xdg_toplevel_decoration_v1 *decoration = data;
	struct wlx_decoration *deco = wl_container_of(listener, deco, destroy);

	if (decoration && decoration->toplevel) {
		struct wlx_window *win = decoration->toplevel->base->data;
		if (win && win->pending_decoration == decoration) {
			win->pending_decoration = NULL;
		}
	}

	wl_list_remove(&deco->request_mode.link);
	wl_list_remove(&deco->destroy.link);
	free(deco);
}

void server_new_toplevel_decoration(struct wl_listener *listener, void *data) {
	(void)listener;
	struct wlr_xdg_toplevel_decoration_v1 *decoration = data;

	struct wlx_decoration *deco = calloc(1, sizeof(*deco));
	if (!deco) {
		wlr_log(WLR_ERROR, "out of memory allocating decoration tracker");
		return;
	}
	deco->request_mode.notify = decoration_request_mode;
	wl_signal_add(&decoration->events.request_mode, &deco->request_mode);
	deco->destroy.notify = decoration_destroy;
	wl_signal_add(&decoration->events.destroy, &deco->destroy);

	wlr_log(WLR_INFO, "new xdg toplevel decoration object -> forcing server-side mode");

	struct wlx_window *win = decoration->toplevel->base->data;
	if (win && !win->initial_configure_sent) {
		win->pending_decoration = decoration;
		return;
	}
	wlr_xdg_toplevel_decoration_v1_set_mode(decoration,
		WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

void server_new_xdg_toplevel(struct wl_listener *listener, void *data) {
	struct wlx_server *server = wl_container_of(listener, server, new_xdg_toplevel);
	struct wlr_xdg_toplevel *toplevel = data;

	wlr_log(WLR_INFO, "new xdg_toplevel created (not yet mapped)");

	struct wlx_window *win = calloc(1, sizeof(*win));
	if (!win) {
		wlr_log(WLR_ERROR, "out of memory allocating wlx_window");
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
