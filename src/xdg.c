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
	/* Cancel any pending host-close recreate; the client is going away. */
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
		/* Must run after the surface is initialized (this commit path).
		 * set_wm_capabilities schedules a configure itself. */
		wlr_xdg_toplevel_set_wm_capabilities(win->toplevel,
			WLR_XDG_TOPLEVEL_WM_CAPABILITIES_WINDOW_MENU |
			WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE |
			WLR_XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN |
			WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MINIMIZE);
		wlr_xdg_surface_schedule_configure(win->toplevel->base);
	}

	if (win->pending_decoration) {
		struct wlr_xdg_toplevel_decoration_v1 *decoration = win->pending_decoration;
		win->pending_decoration = NULL;
		enum wlr_xdg_toplevel_decoration_v1_mode mode = win->server->prefer_csd
			? WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE
			: WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
		wlr_xdg_toplevel_decoration_v1_set_mode(decoration, mode);
	}

	/* Track CSD margin (buffer − geometry) so host resizes configure the
	 * client with geometry size while the X11 window stays buffer-sized. */
	if (win->toplevel && win->toplevel->base->surface) {
		struct wlr_box geo = win->toplevel->base->current.geometry;
		struct wlr_surface *surf = win->toplevel->base->surface;
		if (geo.width > 0 && geo.height > 0 &&
				surf->current.width > 0 && surf->current.height > 0) {
			int mw = surf->current.width - geo.width;
			int mh = surf->current.height - geo.height;
			if (mw < 0) {
				mw = 0;
			}
			if (mh < 0) {
				mh = 0;
			}
			win->csd_margin_w = mw;
			win->csd_margin_h = mh;
		}
	}

	/* Fit the X11 window to the client until the WM/user resizes it.
	 * SSD: xdg geometry. CSD: full buffer (shadow/resize margins). */
	if (win->output && !win->size_from_wm && win->toplevel) {
		int cw = 0, ch = 0;
		toplevel_preferred_size(win, &cw, &ch);
		struct wlr_box geo = win->toplevel->base->current.geometry;
		int conf_w = (geo.width > 0) ? geo.width : cw;
		int conf_h = (geo.height > 0) ? geo.height : ch;
		int out_w = wlx_scale_size(win->server, cw);
		int out_h = wlx_scale_size(win->server, ch);
		if (cw >= WLX_MIN_OUTPUT_SIZE && ch >= WLX_MIN_OUTPUT_SIZE &&
				(out_w != win->output->width || out_h != win->output->height)) {
			wlr_log(WLR_INFO, "fitting X11 window to client %dx%d "
				"(configure %dx%d, margin %dx%d, scale %.2f, csd=%d)",
				cw, ch, conf_w, conf_h,
				win->csd_margin_w, win->csd_margin_h,
				win->server->content_scale, win->server->prefer_csd);
			resize_output_to(win, out_w, out_h);
			wlr_xdg_toplevel_set_size(win->toplevel, conf_w, conf_h);
			if (win->l_output) {
				wlr_scene_node_set_position(&win->scene_tree->node,
					win->l_output->x, win->l_output->y);
			}
		}
	}

	/* Re-apply pixel scale after the scene helper refreshed buffer dest sizes. */
	wlx_apply_content_scale(win);

	/* xdg min/max may have changed on this commit — keep host hints current. */
	if (win->xwin != XCB_WINDOW_NONE && win->output) {
		win_sync_size_hints(win);
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
	/* Default: server-side (host WM draws the border). With --csd: client
	 * side, and _MOTIF_WM_HINTS strips the host chrome.
	 *
	 * xdg-decoration can be created before the first surface commit;
	 * set_mode schedules a configure and must wait until initialized. */
	struct wlx_window *win = decoration->toplevel->base->data;
	if (win && !win->initial_configure_sent) {
		win->pending_decoration = decoration;
		return;
	}
	bool csd = win && win->server && win->server->prefer_csd;
	wlr_xdg_toplevel_decoration_v1_set_mode(decoration, csd
		? WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE
		: WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
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

	struct wlx_window *win = decoration->toplevel->base->data;
	bool csd = win && win->server && win->server->prefer_csd;
	wlr_log(WLR_INFO, "new xdg toplevel decoration -> %s mode",
		csd ? "client-side" : "server-side");

	if (win && !win->initial_configure_sent) {
		win->pending_decoration = decoration;
		return;
	}
	wlr_xdg_toplevel_decoration_v1_set_mode(decoration, csd
		? WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE
		: WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
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
