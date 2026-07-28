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
#include <math.h>

#include <drm_fourcc.h>

#include <wlr/render/allocator.h>
#include <wlr/render/pass.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/render/drm_format_set.h>

static void destroy_present_windows_for_toplevel(struct wlx_window *win);

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
	/* Present-windows first so OR X11 windows cannot outlive the parent. */
	destroy_present_windows_for_toplevel(win);
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

	/* Fit the X11 window:
	 *  --csd: buffer-sized so client shadows stay visible
	 *  SSD:   geometry-sized so leftover client shadow margins are clipped
	 *
	 * size_from_wm: host WM chose the size — do not yank the window back
	 * when the client merely acks that configure. But if the client commits
	 * a *different* size than we last requested (client-driven resize:
	 * content changed, Qt adjusted itself, …), adopt it on the host. */
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
		bool size_mismatch = out_w != win->output->width ||
			out_h != win->output->height;
		/* Client-driven when the committed size is not what we last asked
		 * for. last_client_conf == 0 means we never requested a size yet
		 * (set_size(0,0) on map) — treat a mismatch as client-chosen. */
		bool client_driven = size_mismatch &&
			(win->last_client_conf_w <= 0 || win->last_client_conf_h <= 0 ||
			 conf_w != win->last_client_conf_w ||
			 conf_h != win->last_client_conf_h);
		bool grow = !tiled &&
			(out_w > win->output->width || out_h > win->output->height);
		bool fit = size_mismatch &&
			(!win->size_from_wm || client_driven);
		if (client_driven) {
			win->size_from_wm = false;
			wlr_log(WLR_DEBUG, "client-driven resize %dx%d → %dx%d "
				"(was conf %dx%d)",
				win->last_client_conf_w, win->last_client_conf_h,
				conf_w, conf_h, win->output->width, win->output->height);
		}
		if (cw >= WLX_MIN_OUTPUT_SIZE && ch >= WLX_MIN_OUTPUT_SIZE &&
				(fit || grow) && !tiled) {
			if (grow && win->size_from_wm && !client_driven) {
				/* Only expand for CSD margins while WM still owns size. */
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
			wlx_toplevel_set_size(win, conf_w, conf_h);
			if (win->l_output) {
				wlr_scene_node_set_position(&win->scene_tree->node,
					win->l_output->x, win->l_output->y);
			}
		}
	}

	/* Scene placement of the xdg surface within the output:
	 *  --csd: (0,0) so the full buffer (incl. shadow) fills the host window
	 *  SSD:   (-geometry.x, -geometry.y) so content aligns to the host and
	 *         client-drawn shadow pixels fall outside the output and clip.
	 * Skip popup scene trees (node.data is wlx_popup) — those keep their
	 * positioner geometry relative to the parent. */
	if (win->scene_tree) {
		int ox = 0, oy = 0;
		if (!win->server->prefer_csd && win->toplevel) {
			struct wlr_box geo = win->toplevel->base->current.geometry;
			if (geo.width > 0 && geo.height > 0) {
				ox = -geo.x;
				oy = -geo.y;
			}
		}
		struct wlr_scene_node *child;
		wl_list_for_each(child, &win->scene_tree->children, link) {
			/* Popups are reparented under this tree; do not overwrite their
			 * positioner coordinates with the toplevel buffer offset. */
			bool is_popup = false;
			struct wlx_popup *pp;
			wl_list_for_each(pp, &win->server->popups, link) {
				if (pp->scene_tree && &pp->scene_tree->node == child) {
					is_popup = true;
					break;
				}
			}
			if (is_popup) {
				continue;
			}
			wlr_scene_node_set_position(child, ox, oy);
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

	destroy_present_windows_for_toplevel(win);
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

/* Preferred xdg-decoration mode for this compositor instance.
 * Without --csd we always answer SERVER_SIDE so clients (GTK) must not
 * paint CSD chrome or in-buffer drop shadows. */
static enum wlr_xdg_toplevel_decoration_v1_mode
wlx_decoration_mode(struct wlx_server *server) {
	if (server && server->prefer_csd) {
		return WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE;
	}
	return WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
}

/* Apply mode now, or queue until the surface is initialized (set_mode
 * schedules a configure and asserts the surface is ready). */
static void
wlx_decoration_apply_mode(struct wlr_xdg_toplevel_decoration_v1 *decoration) {
	struct wlx_window *win = decoration->toplevel
		? decoration->toplevel->base->data : NULL;
	enum wlr_xdg_toplevel_decoration_v1_mode mode =
		wlx_decoration_mode(win ? win->server : NULL);

	if (win && !win->initial_configure_sent) {
		win->pending_decoration = decoration;
		wlr_log(WLR_DEBUG, "xdg-decoration: defer %s mode until configure",
			mode == WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE
				? "client-side" : "server-side");
		return;
	}
	wlr_log(WLR_INFO, "xdg-decoration: set %s mode",
		mode == WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE
			? "client-side" : "server-side");
	wlr_xdg_toplevel_decoration_v1_set_mode(decoration, mode);
}

void decoration_request_mode(struct wl_listener *listener, void *data) {
	(void)listener;
	/* Client preference is ignored: compositor policy wins (--csd or SSD). */
	wlx_decoration_apply_mode(data);
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
	if (deco) {
		deco->request_mode.notify = decoration_request_mode;
		wl_signal_add(&decoration->events.request_mode, &deco->request_mode);
		deco->destroy.notify = decoration_destroy;
		wl_signal_add(&decoration->events.destroy, &deco->destroy);
	} else {
		wlr_log(WLR_ERROR, "out of memory allocating decoration tracker");
		/* Still force a mode so the client is not left undecided. */
	}

	wlx_decoration_apply_mode(decoration);
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
/* xdg_popup — data model (present-window ready; parent-scene until P3) */
/* ------------------------------------------------------------------- */

static struct wlx_window *popup_find_toplevel_window(struct wlr_xdg_popup *popup) {
	struct wlr_xdg_surface *parent =
		wlr_xdg_surface_try_from_wlr_surface(popup->parent);
	while (parent) {
		if (parent->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL && parent->data) {
			return parent->data;
		}
		if (parent->role == WLR_XDG_SURFACE_ROLE_POPUP && parent->popup) {
			parent = wlr_xdg_surface_try_from_wlr_surface(
				parent->popup->parent);
			continue;
		}
		break;
	}
	return NULL;
}

/* Drop the OR present-window if any. Safe when still on parent-scene path. */
static void popup_destroy_xpresent(struct wlx_popup *pop) {
	if (!pop) {
		return;
	}
	if (pop->xpresent_frame.link.prev) {
		wl_list_remove(&pop->xpresent_frame.link);
		pop->xpresent_frame.link.prev = NULL;
		pop->xpresent_frame.link.next = NULL;
	}
	if (pop->xpresent_destroy.link.prev) {
		wl_list_remove(&pop->xpresent_destroy.link);
		pop->xpresent_destroy.link.prev = NULL;
		pop->xpresent_destroy.link.next = NULL;
	}
	if (pop->xpresent) {
		wlr_x11_present_window_destroy(pop->xpresent);
		pop->xpresent = NULL;
	}
	pop->root_box_valid = false;
}

static void popup_handle_xpresent_destroy(struct wl_listener *listener,
		void *data) {
	struct wlx_popup *pop =
		wl_container_of(listener, pop, xpresent_destroy);
	(void)data;
	if (pop->xpresent_frame.link.prev) {
		wl_list_remove(&pop->xpresent_frame.link);
		pop->xpresent_frame.link.prev = NULL;
		pop->xpresent_frame.link.next = NULL;
	}
	if (pop->xpresent_destroy.link.prev) {
		wl_list_remove(&pop->xpresent_destroy.link);
		pop->xpresent_destroy.link.prev = NULL;
		pop->xpresent_destroy.link.next = NULL;
	}
	pop->xpresent = NULL;
	pop->root_box_valid = false;
}

static void popup_send_frame_done_iterator(struct wlr_scene_buffer *scene_buffer,
		int lx, int ly, void *user_data) {
	(void)lx;
	(void)ly;
	struct timespec *now = user_data;
	struct wlr_scene_surface *scene_surface =
		wlr_scene_surface_try_from_buffer(scene_buffer);
	if (scene_surface) {
		wlr_scene_surface_send_frame_done(scene_surface, now);
	}
}

static void popup_handle_xpresent_frame(struct wl_listener *listener,
		void *data) {
	struct wlx_popup *pop =
		wl_container_of(listener, pop, xpresent_frame);
	(void)data;
	if (!pop->scene_tree) {
		return;
	}
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_node_for_each_buffer(&pop->scene_tree->node,
		popup_send_frame_done_iterator, &now);
}

/* Refresh root_x/y/w/h from present-window or parent-relative geometry. */
static void popup_update_root_box(struct wlx_popup *pop) {
	if (!pop || !pop->xdg_popup) {
		return;
	}
	pop->root_box_valid = false;

	if (pop->xpresent) {
		wlr_x11_present_window_get_geometry(pop->xpresent,
			&pop->root_x, &pop->root_y, &pop->root_w, &pop->root_h);
		pop->root_box_valid = (pop->root_w > 0 && pop->root_h > 0);
		return;
	}

	struct wlx_window *parent = pop->parent;
	if (!parent) {
		return;
	}
	int16_t ox = 0, oy = 0;
	if (!window_content_root_position(parent, &ox, &oy)) {
		return;
	}
	struct wlr_box geo = pop->xdg_popup->current.geometry;
	if (geo.width <= 0 || geo.height <= 0) {
		geo = pop->xdg_popup->scheduled.geometry;
	}
	struct wlx_server *server = pop->server;
	int surf_w = pop->xdg_popup->base->surface
		? pop->xdg_popup->base->surface->current.width : 0;
	int surf_h = pop->xdg_popup->base->surface
		? pop->xdg_popup->base->surface->current.height : 0;
	int gw = wlx_scale_size(server, geo.width > 0 ? geo.width : surf_w);
	int gh = wlx_scale_size(server, geo.height > 0 ? geo.height : surf_h);
	int gx = wlx_scale_size(server, geo.x);
	int gy = wlx_scale_size(server, geo.y);
	if (!server->prefer_csd && parent->toplevel) {
		struct wlr_box pgeo = parent->toplevel->base->current.geometry;
		if (pgeo.width > 0 && pgeo.height > 0) {
			gx = wlx_scale_size(server, geo.x - pgeo.x);
			gy = wlx_scale_size(server, geo.y - pgeo.y);
		}
	}
	if (gw <= 0 || gh <= 0) {
		return;
	}
	pop->root_x = (int16_t)(ox + gx);
	pop->root_y = (int16_t)(oy + gy);
	pop->root_w = gw;
	pop->root_h = gh;
	pop->root_box_valid = true;
}

struct wlx_popup *popup_at_root_pointer(struct wlx_server *server) {
	if (!server->xcb || xcb_connection_has_error(server->xcb)) {
		return NULL;
	}
	int16_t px = 0, py = 0;
	if (!query_root_pointer_position(server, &px, &py)) {
		return NULL;
	}
	/* Newest first (setup_popup inserts at head). */
	struct wlx_popup *pop;
	wl_list_for_each(pop, &server->popups, link) {
		if (!pop->xdg_popup || !pop->xdg_popup->base->surface ||
				!pop->xdg_popup->base->surface->mapped) {
			continue;
		}
		if (!pop->root_box_valid) {
			popup_update_root_box(pop);
		}
		if (!pop->root_box_valid) {
			continue;
		}
		if (px >= pop->root_x && py >= pop->root_y &&
				px < pop->root_x + pop->root_w &&
				py < pop->root_y + pop->root_h) {
			return pop;
		}
	}
	return NULL;
}

bool window_has_mapped_popup(struct wlx_server *server, struct wlx_window *win) {
	if (!server || !win) {
		return false;
	}
	struct wlx_popup *pop;
	wl_list_for_each(pop, &server->popups, link) {
		if (pop->parent == win && pop->xdg_popup &&
				pop->xdg_popup->base->surface->mapped) {
			return true;
		}
	}
	return false;
}

/* Present-window placement: OR X11 window in root space so menus are not
 * clipped by the parent. Scene tree is parked off-layout so parent
 * scene_outputs do not double-paint it. */
#define WLX_POPUP_SCENE_PARK_X (-100000)
#define WLX_POPUP_SCENE_PARK_Y (-100000)

struct popup_render_data {
	struct wlr_render_pass *pass;
	struct wlr_renderer *renderer;
	int origin_lx, origin_ly;
};

static void popup_render_iterator(struct wlr_scene_buffer *scene_buffer,
		int lx, int ly, void *user_data) {
	struct popup_render_data *d = user_data;
	if (!scene_buffer->buffer) {
		return;
	}
	struct wlr_texture *texture = wlr_texture_from_buffer(d->renderer,
		scene_buffer->buffer);
	if (!texture) {
		return;
	}
	int dst_w = scene_buffer->dst_width > 0
		? scene_buffer->dst_width : scene_buffer->buffer->width;
	int dst_h = scene_buffer->dst_height > 0
		? scene_buffer->dst_height : scene_buffer->buffer->height;
	struct wlr_box dst_box = {
		.x = lx - d->origin_lx,
		.y = ly - d->origin_ly,
		.width = dst_w,
		.height = dst_h,
	};
	float opacity = scene_buffer->opacity;
	wlr_render_pass_add_texture(d->pass, &(struct wlr_render_texture_options){
		.texture = texture,
		.src_box = scene_buffer->src_box,
		.dst_box = dst_box,
		.transform = wlr_output_transform_invert(scene_buffer->transform),
		.alpha = &opacity,
		.filter_mode = scene_buffer->filter_mode,
		.blend_mode = WLR_RENDER_BLEND_MODE_PREMULTIPLIED,
	});
	wlr_texture_destroy(texture);
}

static const struct wlr_drm_format *popup_pick_argb_format(
		struct wlx_server *server) {
	static const uint32_t caps[] = {
		WLR_BUFFER_CAP_DMABUF,
		WLR_BUFFER_CAP_SHM,
	};
	for (size_t i = 0; i < sizeof(caps) / sizeof(caps[0]); i++) {
		const struct wlr_drm_format_set *set =
			wlr_renderer_get_texture_formats(server->renderer, caps[i]);
		if (!set) {
			continue;
		}
		const struct wlr_drm_format *fmt =
			wlr_drm_format_set_get(set, DRM_FORMAT_ARGB8888);
		if (fmt) {
			return fmt;
		}
	}
	return NULL;
}

static bool popup_render_and_present(struct wlx_popup *pop) {
	if (!pop || !pop->xpresent || !pop->scene_tree || !pop->server) {
		return false;
	}
	struct wlx_server *server = pop->server;
	int32_t width = 0, height = 0;
	wlr_x11_present_window_get_geometry(pop->xpresent,
		NULL, NULL, &width, &height);
	if (width < 1 || height < 1) {
		return false;
	}

	/* Fast path: no content scale and a single client buffer of matching size. */
	struct wlr_surface *surf = pop->xdg_popup && pop->xdg_popup->base
		? pop->xdg_popup->base->surface : NULL;
	double scale = server->content_scale > 0.0 ? server->content_scale : 1.0;
	if (scale == 1.0 && surf && surf->buffer &&
			surf->buffer->base.width == width &&
			surf->buffer->base.height == height) {
		return wlr_x11_present_window_present(pop->xpresent,
			&surf->buffer->base);
	}

	const struct wlr_drm_format *fmt = popup_pick_argb_format(server);
	if (!fmt || !server->allocator || !server->renderer) {
		/* Fall back to client buffer even if size differs slightly. */
		if (surf && surf->buffer) {
			return wlr_x11_present_window_present(pop->xpresent,
				&surf->buffer->base);
		}
		return false;
	}

	struct wlr_buffer *buf = wlr_allocator_create_buffer(
		server->allocator, width, height, fmt);
	if (!buf) {
		if (surf && surf->buffer) {
			return wlr_x11_present_window_present(pop->xpresent,
				&surf->buffer->base);
		}
		return false;
	}

	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(
		server->renderer, buf, NULL);
	if (!pass) {
		wlr_buffer_drop(buf);
		return false;
	}

	/* Transparent clear — host compositor blends the OR window. */
	wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
		.box = { .x = 0, .y = 0, .width = width, .height = height },
		.color = { .r = 0, .g = 0, .b = 0, .a = 0 },
	});

	int origin_lx = 0, origin_ly = 0;
	wlr_scene_node_coords(&pop->scene_tree->node, &origin_lx, &origin_ly);
	struct popup_render_data rd = {
		.pass = pass,
		.renderer = server->renderer,
		.origin_lx = origin_lx,
		.origin_ly = origin_ly,
	};
	wlr_scene_node_for_each_buffer(&pop->scene_tree->node,
		popup_render_iterator, &rd);

	if (!wlr_render_pass_submit(pass)) {
		wlr_buffer_drop(buf);
		return false;
	}

	bool ok = wlr_x11_present_window_present(pop->xpresent, buf);
	wlr_buffer_drop(buf);
	return ok;
}

static void popup_attach_xpresent_listeners(struct wlx_popup *pop) {
	if (!pop->xpresent) {
		return;
	}
	if (!pop->xpresent_frame.link.prev) {
		pop->xpresent_frame.notify = popup_handle_xpresent_frame;
		wlr_x11_present_window_add_frame_listener(pop->xpresent,
			&pop->xpresent_frame);
	}
	if (!pop->xpresent_destroy.link.prev) {
		pop->xpresent_destroy.notify = popup_handle_xpresent_destroy;
		wlr_x11_present_window_add_destroy_listener(pop->xpresent,
			&pop->xpresent_destroy);
	}
}

static bool popup_compute_root_placement(struct wlx_popup *pop,
		int16_t *root_x, int16_t *root_y, int32_t *width, int32_t *height) {
	struct wlr_xdg_popup *xdg = pop->xdg_popup;
	if (!xdg || !pop->server) {
		return false;
	}

	int lx = xdg->current.geometry.x;
	int ly = xdg->current.geometry.y;
	int lw = xdg->current.geometry.width;
	int lh = xdg->current.geometry.height;
	if (lw <= 0 || lh <= 0) {
		lx = xdg->scheduled.geometry.x;
		ly = xdg->scheduled.geometry.y;
		lw = xdg->scheduled.geometry.width;
		lh = xdg->scheduled.geometry.height;
	}
	struct wlr_surface *surf = xdg->base->surface;
	if (lw <= 0 && surf) {
		lw = surf->current.width;
	}
	if (lh <= 0 && surf) {
		lh = surf->current.height;
	}
	if (lw <= 0 || lh <= 0) {
		return false;
	}

	struct wlx_server *server = pop->server;
	int gx = wlx_scale_size(server, lx);
	int gy = wlx_scale_size(server, ly);
	int gw = wlx_scale_size(server, lw);
	int gh = wlx_scale_size(server, lh);

	int16_t ox = 0, oy = 0;

	/* Nested popup: geometry is relative to the parent popup surface.
	 * Prefer that popup's cached root box so submenus track the menu. */
	struct wlr_surface *parent_surf = xdg->parent;
	struct wlr_xdg_surface *parent_xdg = parent_surf
		? wlr_xdg_surface_try_from_wlr_surface(parent_surf) : NULL;
	if (parent_xdg && parent_xdg->role == WLR_XDG_SURFACE_ROLE_POPUP &&
			parent_xdg->data) {
		struct wlx_popup *parent_pop = parent_xdg->data;
		if (!parent_pop->root_box_valid) {
			popup_update_root_box(parent_pop);
		}
		if (!parent_pop->root_box_valid) {
			return false;
		}
		ox = parent_pop->root_x;
		oy = parent_pop->root_y;
	} else {
		/* Direct child of a toplevel. */
		struct wlx_window *parent = pop->parent;
		if (!parent || !window_content_root_position(parent, &ox, &oy)) {
			return false;
		}
		/* SSD: parent content is window-geometry sized; popup geometry is
		 * surface-relative — subtract parent geometry origin. */
		if (!server->prefer_csd && parent->toplevel) {
			struct wlr_box pgeo = parent->toplevel->base->current.geometry;
			if (pgeo.width > 0 && pgeo.height > 0) {
				gx = wlx_scale_size(server, lx - pgeo.x);
				gy = wlx_scale_size(server, ly - pgeo.y);
			}
		}
	}

	*root_x = (int16_t)(ox + gx);
	*root_y = (int16_t)(oy + gy);
	*width = gw > 0 ? gw : 1;
	*height = gh > 0 ? gh : 1;
	return true;
}

static void popup_position_and_map(struct wlx_popup *pop) {
	struct wlx_window *parent = pop->parent;
	if (!parent || !pop->scene_tree || !pop->server) {
		return;
	}

	int16_t root_x = 0, root_y = 0;
	int32_t width = 1, height = 1;
	if (!popup_compute_root_placement(pop, &root_x, &root_y, &width, &height)) {
		wlr_log(WLR_DEBUG, "xdg_popup: cannot compute root placement yet");
		return;
	}

	/* Park scene off every output's layout region so parent scene_outputs
	 * do not composite the popup (we Present it ourselves). */
	wlr_scene_node_reparent(&pop->scene_tree->node, &pop->server->scene->tree);
	wlr_scene_node_set_position(&pop->scene_tree->node,
		WLX_POPUP_SCENE_PARK_X, WLX_POPUP_SCENE_PARK_Y);
	wlr_scene_node_set_enabled(&pop->scene_tree->node, true);

	struct wlr_scene_node *child;
	wl_list_for_each(child, &pop->scene_tree->children, link) {
		wlr_scene_node_set_position(child, 0, 0);
	}
	wlx_apply_popup_content_scale(pop);

	if (!pop->xpresent) {
		pop->xpresent = wlr_x11_present_window_create(pop->server->backend);
		if (!pop->xpresent) {
			wlr_log(WLR_ERROR,
				"xdg_popup: present_window create failed; "
				"falling back to parent-scene (clipped)");
			/* Restore parent-scene placement so the menu is at least visible. */
			if (parent->scene_tree) {
				wlr_scene_node_reparent(&pop->scene_tree->node,
					parent->scene_tree);
				double scale = pop->server->content_scale > 0.0
					? pop->server->content_scale : 1.0;
				struct wlr_xdg_popup *xdg = pop->xdg_popup;
				int lx = xdg->current.geometry.x;
				int ly = xdg->current.geometry.y;
				wlr_scene_node_set_position(&pop->scene_tree->node,
					(int)lround(lx * scale), (int)lround(ly * scale));
				if (parent->output) {
					wlr_output_schedule_frame(parent->output);
				}
			}
			popup_update_root_box(pop);
			return;
		}
		popup_attach_xpresent_listeners(pop);
	}

	wlr_x11_present_window_configure(pop->xpresent,
		root_x, root_y, width, height);
	wlr_x11_present_window_map(pop->xpresent);

	pop->root_x = root_x;
	pop->root_y = root_y;
	pop->root_w = width;
	pop->root_h = height;
	pop->root_box_valid = true;

	if (!popup_render_and_present(pop)) {
		wlr_log(WLR_DEBUG, "xdg_popup: present failed (%dx%d at %d,%d)",
			width, height, root_x, root_y);
	} else {
		wlr_log(WLR_INFO, "xdg_popup present-window %dx%d at root (%d,%d)",
			width, height, root_x, root_y);
	}
}


void wlx_reposition_popups_for_window(struct wlx_window *win) {
	if (!win || !win->server) {
		return;
	}
	/* Oldest first so parent menus update root boxes before nested
	 * submenus that depend on them (list is insert-at-head). */
	struct wlx_popup *pop;
	wl_list_for_each_reverse(pop, &win->server->popups, link) {
		if (pop->parent != win || !pop->xdg_popup ||
				!pop->xdg_popup->base->surface ||
				!pop->xdg_popup->base->surface->mapped) {
			continue;
		}
		int16_t rx = 0, ry = 0;
		int32_t rw = 1, rh = 1;
		if (!popup_compute_root_placement(pop, &rx, &ry, &rw, &rh)) {
			continue;
		}
		if (pop->xpresent) {
			wlr_x11_present_window_configure(pop->xpresent,
				rx, ry, rw, rh);
		}
		pop->root_x = rx;
		pop->root_y = ry;
		pop->root_w = rw;
		pop->root_h = rh;
		pop->root_box_valid = true;
	}
}

/* Tear down present-windows for every popup of this toplevel. Safe when
 * the client still owns the xdg_popup objects (unmap/destroy later). */
static void destroy_present_windows_for_toplevel(struct wlx_window *win) {
	if (!win || !win->server) {
		return;
	}
	struct wlx_popup *pop;
	wl_list_for_each(pop, &win->server->popups, link) {
		if (pop->parent == win) {
			popup_destroy_xpresent(pop);
			if (pop->scene_tree) {
				wlr_scene_node_set_enabled(&pop->scene_tree->node, false);
			}
		}
	}
}

static void popup_map(struct wl_listener *listener, void *data) {
	struct wlx_popup *pop = wl_container_of(listener, pop, map);
	(void)data;
	struct wlr_surface *surf = pop->xdg_popup->base->surface;
	wlr_log(WLR_INFO, "xdg_popup map → present-window (buffer %dx%d)",
		surf ? surf->current.width : 0,
		surf ? surf->current.height : 0);
	popup_position_and_map(pop);
	/* If the opening button is still held, do not enter yet (notify_enter
	 * resets seat buttons). Focus refresh runs on release. */
	if (!pop->server->seat ||
			pop->server->seat->pointer_state.button_count == 0) {
		wlx_pointer_refresh_focus(pop->server);
	}
}

static void popup_unmap(struct wl_listener *listener, void *data) {
	struct wlx_popup *pop = wl_container_of(listener, pop, unmap);
	(void)data;
	wlr_log(WLR_INFO, "xdg_popup unmap");
	if (pop->xdg_popup && pop->server->seat &&
			pop->server->seat->pointer_state.focused_surface ==
				pop->xdg_popup->base->surface) {
		wlr_seat_pointer_clear_focus(pop->server->seat);
	}
	popup_destroy_xpresent(pop);
	if (pop->scene_tree) {
		wlr_scene_node_set_enabled(&pop->scene_tree->node, false);
	}
	wlx_pointer_refresh_focus(pop->server);
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
	popup_destroy_xpresent(pop);
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

	wlr_log(WLR_INFO, "new xdg_popup (present-window on map)");
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
