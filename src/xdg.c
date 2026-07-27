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
#include <time.h>

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
	wlr_log(WLR_DEBUG, "surface commit (mapped=%d, has_buffer=%d)",
		win->output != NULL,
		win->toplevel->base->surface->buffer != NULL);

	/* Drop stretch-hold once the client buffer roughly matches the host
	 * size (intermediate commits would otherwise stop the hold early). */
	if (win->hold_present && win->output && win->toplevel &&
			win->toplevel->base->surface) {
		struct wlr_surface *surf = win->toplevel->base->surface;
		int ow = wlx_unscale_size(win->server, win->output->width);
		int oh = wlx_unscale_size(win->server, win->output->height);
		int sw = surf->current.width;
		int sh = surf->current.height;
		if (!win->server->prefer_csd) {
			struct wlr_box geo = win->toplevel->base->current.geometry;
			if (geo.width > 0 && geo.height > 0) {
				sw = geo.width;
				sh = geo.height;
			}
		}
		int dw = sw > ow ? sw - ow : ow - sw;
		int dh = sh > oh ? sh - oh : oh - sh;
		if (dw <= 2 && dh <= 2) {
			win->hold_present = false;
			wlr_output_schedule_frame(win->output);
		}
	}

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

	/* Fit the X11 window to the client buffer. Host is always buffer-sized
	 * so CSD shadows are not clipped. Configure still uses window geometry
	 * when known. Grow even under size_from_wm when the buffer is larger
	 * than the host (dialogs / unmaximize). */
	if (win->output && win->toplevel) {
		int cw = 0, ch = 0;
		toplevel_preferred_size(win, &cw, &ch);
		struct wlr_box geo = win->toplevel->base->current.geometry;
		int conf_w = (geo.width > 0) ? geo.width : cw;
		int conf_h = (geo.height > 0) ? geo.height : ch;
		int out_w = wlx_scale_size(win->server, cw);
		int out_h = wlx_scale_size(win->server, ch);
		bool tiled = win->toplevel->current.maximized ||
			win->toplevel->pending.maximized ||
			win->toplevel->current.fullscreen ||
			win->toplevel->pending.fullscreen;
		bool grow = !tiled &&
			(out_w > win->output->width || out_h > win->output->height);
		bool fit = !win->size_from_wm &&
			(out_w != win->output->width || out_h != win->output->height);
		if (cw >= WLX_MIN_OUTPUT_SIZE && ch >= WLX_MIN_OUTPUT_SIZE &&
				(fit || grow)) {
			if (grow && win->size_from_wm) {
				if (out_w < win->output->width) {
					out_w = win->output->width;
				}
				if (out_h < win->output->height) {
					out_h = win->output->height;
				}
			}
			wlr_log(WLR_DEBUG, "fitting X11 window to client %dx%d "
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

	/* Host is buffer-sized; always force (0,0) so the full surface matches
	 * the X11 window. xdg scene defaults to (-geometry.x, -geometry.y),
	 * which shifts content and breaks pointer hit-testing. */
	if (win->scene_tree) {
		struct wlr_scene_node *child;
		wl_list_for_each(child, &win->scene_tree->children, link) {
			wlr_scene_node_set_position(child, 0, 0);
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
	wl_list_remove(&win->new_popup.link);
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

void window_new_popup(struct wl_listener *listener, void *data);

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

	win->new_popup.notify = window_new_popup;
	wl_signal_add(&toplevel->base->events.new_popup, &win->new_popup);

	wl_list_insert(&server->windows, &win->link);
}

/* ------------------------------------------------------------------- */
/* xdg_popup → override-redirect X11 window                            */
/* ------------------------------------------------------------------- */

static struct wlx_window *popup_find_toplevel_window(struct wlr_xdg_popup *popup) {
       struct wlr_xdg_surface *parent = wlr_xdg_surface_try_from_wlr_surface(popup->parent);
	while (parent) {
		if (parent->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL && parent->data) {
			return parent->data;
		}
		if (parent->role == WLR_XDG_SURFACE_ROLE_POPUP && parent->popup) {
                  parent = wlr_xdg_surface_try_from_wlr_surface(parent->popup->parent);
                  continue;
		}
		break;
	}
	return NULL;
}

static void popup_output_frame(struct wl_listener *listener, void *data) {
	struct wlx_popup *pop = wl_container_of(listener, pop, output_frame);
	(void)data;
	if (!pop->scene_output) {
		return;
	}
	wlr_scene_output_commit(pop->scene_output, NULL);
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(pop->scene_output, &now);
}

static void popup_output_destroy(struct wl_listener *listener, void *data) {
	struct wlx_popup *pop = wl_container_of(listener, pop, output_destroy);
	(void)data;
	wl_list_remove(&pop->output_frame.link);
	wl_list_remove(&pop->output_destroy.link);
	pop->output = NULL;
	pop->scene_output = NULL;
	pop->l_output = NULL;
}

static void popup_destroy_output(struct wlx_popup *pop) {
	if (!pop->output) {
		return;
	}
	wlr_output_destroy(pop->output);
	/* output_destroy listener clears fields */
}


struct wlx_popup *popup_at_root_pointer(struct wlx_server *server) {
	if (!server->xcb || xcb_connection_has_error(server->xcb)) {
		return NULL;
	}
	int16_t px = 0, py = 0;
	if (!query_root_pointer_position(server, &px, &py)) {
		return NULL;
	}
	struct wlx_popup *pop;
	wl_list_for_each(pop, &server->popups, link) {
		if (!pop->output) {
			continue;
		}
		xcb_window_t xwin = wlr_x11_output_get_window(pop->output);
		if (xwin == XCB_WINDOW_NONE) {
			continue;
		}
		int16_t ox = 0, oy = 0;
		if (!query_window_root_position(server, xwin, &ox, &oy)) {
			continue;
		}
		int w = pop->output->width;
		int h = pop->output->height;
		if (px >= ox && py >= oy && px < ox + w && py < oy + h) {
			return pop;
		}
	}
	return NULL;
}

static void popup_position_and_map(struct wlx_popup *pop) {
	struct wlx_window *parent = pop->parent;
	if (!parent || !parent->output || parent->xwin == XCB_WINDOW_NONE) {
		return;
	}

	struct wlr_xdg_popup *xdg = pop->xdg_popup;
	struct wlr_surface *surf = xdg->base->surface;
	int width = surf->current.width;
	int height = surf->current.height;
	if (width < 1) {
		width = 1;
	}
	if (height < 1) {
		height = 1;
	}

	int pw = wlx_scale_size(pop->server, width);
	int ph = wlx_scale_size(pop->server, height);

	if (!pop->output) {
		pop->output = wlr_x11_output_create_override_redirect(pop->server->backend);
		if (!pop->output) {
			wlr_log(WLR_ERROR, "failed to create OR output for popup");
			return;
		}
		/* Same as toplevel outputs: cursor buffer path asserts on a
		 * non-NULL allocator/renderer when the pointer enters this window. */
		wlr_output_init_render(pop->output, pop->server->allocator,
			pop->server->renderer);

		pop->output_frame.notify = popup_output_frame;
		wl_signal_add(&pop->output->events.frame, &pop->output_frame);
		pop->output_destroy.notify = popup_output_destroy;
		wl_signal_add(&pop->output->events.destroy, &pop->output_destroy);

		struct wlr_output_state state;
		wlr_output_state_init(&state);
		wlr_output_state_set_custom_mode(&state, pw, ph, 0);
		wlr_output_state_set_enabled(&state, true);
		if (!wlr_output_commit_state(pop->output, &state)) {
			wlr_log(WLR_ERROR, "failed to map popup OR output");
			wlr_output_state_finish(&state);
			popup_destroy_output(pop);
			return;
		}
		wlr_output_state_finish(&state);

		pop->l_output = wlr_output_layout_add_auto(
			pop->server->output_layout, pop->output);
		pop->scene_output = wlr_scene_output_create(
			pop->server->scene, pop->output);
		wlr_scene_output_layout_add_output(pop->server->scene_layout,
			pop->l_output, pop->scene_output);
	} else if (pop->output->width != pw || pop->output->height != ph) {
		struct wlr_output_state state;
		wlr_output_state_init(&state);
		wlr_output_state_set_custom_mode(&state, pw, ph, 0);
		wlr_output_commit_state(pop->output, &state);
		wlr_output_state_finish(&state);
	}

	/* popup->current.geometry is relative to the parent *surface* origin.
	 * Our host windows are buffer-sized with scene at (0,0), so that is
	 * also the X11 content-window origin. Using get_toplevel_coords(0,0)
	 * only returned the parent window-geometry inset (~26,23) — every
	 * menu appeared at the parent's top-left. */
	int lx = xdg->current.geometry.x;
	int ly = xdg->current.geometry.y;
	if (lx == 0 && ly == 0 &&
			(xdg->scheduled.geometry.x != 0 || xdg->scheduled.geometry.y != 0)) {
		/* First map before the committed geometry is updated. */
		lx = xdg->scheduled.geometry.x;
		ly = xdg->scheduled.geometry.y;
	}

	/* Anchor: immediate parent surface's X11 window (toplevel content or
	 * another popup's OR window for nested menus). */
	int16_t root_x = 0, root_y = 0;
	xcb_window_t parent_xwin = XCB_WINDOW_NONE;
	struct wlr_xdg_surface *parent_xdg =
		wlr_xdg_surface_try_from_wlr_surface(xdg->parent);
	if (parent_xdg && parent_xdg->role == WLR_XDG_SURFACE_ROLE_POPUP &&
			parent_xdg->popup && parent_xdg->data) {
		struct wlx_popup *parent_pop = parent_xdg->data;
		if (parent_pop->output) {
			parent_xwin = wlr_x11_output_get_window(parent_pop->output);
		}
	}
	if (parent_xwin == XCB_WINDOW_NONE) {
		parent_xwin = parent->content_xwin != XCB_WINDOW_NONE
			? parent->content_xwin : parent->xwin;
	}
	query_window_root_position(pop->server, parent_xwin, &root_x, &root_y);

	int px = (int)root_x + wlx_scale_size(pop->server, lx);
	int py = (int)root_y + wlx_scale_size(pop->server, ly);

	wlr_log(WLR_INFO, "xdg_popup place %dx%d at root (%d,%d) "
		"(parent root %d,%d + geometry %d,%d)",
		pw, ph, px, py, (int)root_x, (int)root_y, lx, ly);

	xcb_window_t xwin = wlr_x11_output_get_window(pop->output);
	xcb_connection_t *xconn = wlr_x11_backend_get_connection(pop->server->backend);
	if (xwin != XCB_WINDOW_NONE && xconn) {
		uint32_t vals[] = { (uint32_t)px, (uint32_t)py, (uint32_t)pw, (uint32_t)ph };
		xcb_configure_window(xconn, xwin,
			XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
			XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, vals);
		xcb_flush(xconn);
	}

	/* Place scene tree at this output's layout origin so the scene_output
	 * samples the popup content. The OR window is sized to the full
	 * buffer; force children to (0,0) so xdg's (-geometry) placement does
	 * not clip shadows/padding (same as CSD toplevels). */
	if (pop->l_output && pop->scene_tree) {
		wlr_scene_node_set_position(&pop->scene_tree->node,
			pop->l_output->x, pop->l_output->y);
		struct wlr_scene_node *child;
		wl_list_for_each(child, &pop->scene_tree->children, link) {
			wlr_scene_node_set_position(child, 0, 0);
		}
	}
	wlr_output_schedule_frame(pop->output);
}

static void popup_map(struct wl_listener *listener, void *data) {
	struct wlx_popup *pop = wl_container_of(listener, pop, map);
	(void)data;
	struct wlr_surface *surf = pop->xdg_popup->base->surface;
	wlr_log(WLR_INFO, "xdg_popup map → OR window (buffer %dx%d)",
		surf ? surf->current.width : 0,
		surf ? surf->current.height : 0);
	popup_position_and_map(pop);
}

static void popup_unmap(struct wl_listener *listener, void *data) {
	struct wlx_popup *pop = wl_container_of(listener, pop, unmap);
	(void)data;
	wlr_log(WLR_INFO, "xdg_popup unmap");
	popup_destroy_output(pop);
}

static void popup_commit(struct wl_listener *listener, void *data) {
	struct wlx_popup *pop = wl_container_of(listener, pop, commit);
	(void)data;

	/* First commit: wlroots has marked the surface initialized (role
	 * commit runs before this listener). Unconstrain schedules the
	 * configure the client needs before attaching a buffer. Calling
	 * unconstrain in new_popup / setup_popup trips
	 * assert(surface->initialized) inside schedule_configure. */
	if (pop->xdg_popup->base->initial_commit) {
		struct wlr_box box = {
			.x = -2000,
			.y = -2000,
			.width = 8000,
			.height = 8000,
		};
		wlr_xdg_popup_unconstrain_from_box(pop->xdg_popup, &box);
	}

	if (!pop->xdg_popup->base->surface->mapped) {
		return;
	}
	/* Keep size/position in sync when the client resizes the popup. */
	popup_position_and_map(pop);
}

static void popup_destroy(struct wl_listener *listener, void *data) {
	struct wlx_popup *pop = wl_container_of(listener, pop, destroy);
	(void)data;
	popup_destroy_output(pop);
	if (pop->scene_tree) {
		wlr_scene_node_destroy(&pop->scene_tree->node);
		pop->scene_tree = NULL;
	}
	wl_list_remove(&pop->link);
	wl_list_remove(&pop->map.link);
	wl_list_remove(&pop->unmap.link);
	wl_list_remove(&pop->destroy.link);
	wl_list_remove(&pop->commit.link);
	if (pop->new_popup.link.prev) {
		wl_list_remove(&pop->new_popup.link);
	}
	free(pop);
}

static void handle_new_xdg_popup(struct wl_listener *listener, void *data);

static void setup_popup(struct wlx_server *server, struct wlr_xdg_popup *xdg_popup,
		struct wlx_window *parent) {
	struct wlx_popup *pop = calloc(1, sizeof(*pop));
	if (!pop) {
		return;
	}
	pop->server = server;
	pop->parent = parent;
	pop->xdg_popup = xdg_popup;
	xdg_popup->base->data = pop;
	wl_list_insert(&server->popups, &pop->link);

	pop->scene_tree = wlr_scene_xdg_surface_create(&server->scene->tree,
		xdg_popup->base);
	if (!pop->scene_tree) {
		free(pop);
		return;
	}
	pop->scene_tree->node.data = pop;

	/* Unconstrain is deferred to popup_commit on initial_commit — the
	 * surface is not initialized yet when new_popup fires. */

	pop->map.notify = popup_map;
	wl_signal_add(&xdg_popup->base->surface->events.map, &pop->map);
	pop->unmap.notify = popup_unmap;
	wl_signal_add(&xdg_popup->base->surface->events.unmap, &pop->unmap);
	pop->destroy.notify = popup_destroy;
	wl_signal_add(&xdg_popup->events.destroy, &pop->destroy);
	pop->commit.notify = popup_commit;
	wl_signal_add(&xdg_popup->base->surface->events.commit, &pop->commit);

	pop->new_popup.notify = handle_new_xdg_popup;
	wl_signal_add(&xdg_popup->base->events.new_popup, &pop->new_popup);

	wlr_log(WLR_INFO, "new xdg_popup (OR window on map)");
}

static void handle_new_xdg_popup(struct wl_listener *listener, void *data) {
	/* Nested popup under an existing popup — still root under the same toplevel. */
	struct wlx_popup *from_pop = wl_container_of(listener, from_pop, new_popup);
	struct wlr_xdg_popup *xdg_popup = data;
	struct wlx_window *parent = from_pop->parent;
	if (!parent) {
		parent = popup_find_toplevel_window(xdg_popup);
	}
	if (!parent) {
		wlr_log(WLR_ERROR, "xdg_popup without parent toplevel");
		return;
	}
	setup_popup(parent->server, xdg_popup, parent);
}

void window_new_popup(struct wl_listener *listener, void *data) {
	struct wlx_window *win = wl_container_of(listener, win, new_popup);
	struct wlr_xdg_popup *xdg_popup = data;
	if (xdg_popup->base->data) {
		return; /* already set up via shell-level new_popup */
	}
	wlr_log(WLR_INFO, "xdg_popup from toplevel \"%s\" (app_id \"%s\")",
		win->toplevel && win->toplevel->title ? win->toplevel->title : "",
		win->toplevel && win->toplevel->app_id ? win->toplevel->app_id : "");
	setup_popup(win->server, xdg_popup, win);
}

/* Shell-level catch-all so popups are never missed if the parent surface
 * listener was not connected yet (or parent is an intermediate popup). */
void server_new_xdg_popup(struct wl_listener *listener, void *data) {
	struct wlx_server *server =
		wl_container_of(listener, server, new_xdg_popup);
	struct wlr_xdg_popup *xdg_popup = data;
	/* Log before any early-return so we never miss a popup that fails setup. */
	wlr_log(WLR_INFO, "shell new_popup (parent_surface=%p data=%p)",
		(void *)xdg_popup->parent, (void *)xdg_popup->base->data);
	if (xdg_popup->base->data) {
		return; /* already set up via parent new_popup */
	}
	struct wlx_window *parent = popup_find_toplevel_window(xdg_popup);
	if (!parent) {
		wlr_log(WLR_ERROR, "shell new_popup: no parent toplevel "
			"(parent_surface=%p) — popup will not get an OR window",
			(void *)xdg_popup->parent);
		return;
	}
	wlr_log(WLR_INFO, "xdg_popup (shell) parent app_id \"%s\"",
		parent->toplevel && parent->toplevel->app_id ?
			parent->toplevel->app_id : "");
	setup_popup(server, xdg_popup, parent);
}
