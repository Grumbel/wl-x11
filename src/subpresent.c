/* SPDX-FileCopyrightText: Copyright (c) 2026 Ingo Ruhnke <grumbel@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Overflowing wl_subsurface → OR present-window.
 *
 * GTK menubar menus are often subsurfaces at negative offsets (not
 * xdg_popup). On a fullscreen compositor they paint outside the parent;
 * on wl-x11 the parent is a small X11 window, so overflow was lost.
 * Promote those surfaces to present-windows (same host path as xdg_popup).
 */

#include "server.h"

#include <stdlib.h>
#include <time.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/render/allocator.h>
#include <wlr/util/log.h>

struct wlx_subpresent {
	struct wlx_server *server;
	struct wlx_window *parent;
	struct wlr_surface *surface;
	struct wlr_x11_present_window *xpresent;
	int16_t root_x, root_y;
	int32_t root_w, root_h;
	bool root_box_valid;
	/* Subsurface offset relative to the toplevel surface (logical). */
	int sx, sy;

	struct wl_list link; /* server->subpresents */

	struct wl_listener surface_commit;
	struct wl_listener surface_destroy;
	struct wl_listener xpresent_frame;
	struct wl_listener xpresent_destroy;
};

static void subpresent_destroy(struct wlx_subpresent *sp);
static void subpresent_set_scene_enabled(struct wlx_subpresent *sp, bool on);

static struct wlx_subpresent *subpresent_find(struct wlx_server *server,
		struct wlr_surface *surface) {
	struct wlx_subpresent *sp;
	wl_list_for_each(sp, &server->subpresents, link) {
		if (sp->surface == surface) {
			return sp;
		}
	}
	return NULL;
}

/* Walk scene tree under node; enable/disable buffer nodes for surface. */
static void scene_set_surface_enabled(struct wlr_scene_node *node,
		struct wlr_surface *surface, bool enabled) {
	if (!node) {
		return;
	}
	if (node->type == WLR_SCENE_NODE_BUFFER) {
		struct wlr_scene_buffer *buf = wlr_scene_buffer_from_node(node);
		struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(buf);
		if (ss && ss->surface == surface) {
			wlr_scene_node_set_enabled(node, enabled);
		}
	}
	if (node->type == WLR_SCENE_NODE_TREE) {
		struct wlr_scene_tree *tree = wlr_scene_tree_from_node(node);
		struct wlr_scene_node *child;
		wl_list_for_each(child, &tree->children, link) {
			scene_set_surface_enabled(child, surface, enabled);
		}
	}
}

static void subpresent_set_scene_enabled(struct wlx_subpresent *sp, bool on) {
	if (!sp->parent || !sp->parent->scene_tree || !sp->surface) {
		return;
	}
	scene_set_surface_enabled(&sp->parent->scene_tree->node, sp->surface, on);
}

static void subpresent_handle_xpresent_frame(struct wl_listener *listener,
		void *data) {
	(void)data;
	struct wlx_subpresent *sp =
		wl_container_of(listener, sp, xpresent_frame);
	if (!sp->surface) {
		return;
	}
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_surface_send_frame_done(sp->surface, &now);
}

static void subpresent_handle_xpresent_destroy(struct wl_listener *listener,
		void *data) {
	(void)data;
	struct wlx_subpresent *sp =
		wl_container_of(listener, sp, xpresent_destroy);
	sp->xpresent = NULL;
	wl_list_remove(&sp->xpresent_frame.link);
	wl_list_init(&sp->xpresent_frame.link);
	wl_list_remove(&sp->xpresent_destroy.link);
	wl_list_init(&sp->xpresent_destroy.link);
	subpresent_set_scene_enabled(sp, true);
}

static void subpresent_attach_xpresent_listeners(struct wlx_subpresent *sp) {
	if (!sp->xpresent) {
		return;
	}
	if (wl_list_empty(&sp->xpresent_frame.link)) {
		sp->xpresent_frame.notify = subpresent_handle_xpresent_frame;
		wlr_x11_present_window_add_frame_listener(sp->xpresent,
			&sp->xpresent_frame);
	}
	if (wl_list_empty(&sp->xpresent_destroy.link)) {
		sp->xpresent_destroy.notify = subpresent_handle_xpresent_destroy;
		wlr_x11_present_window_add_destroy_listener(sp->xpresent,
			&sp->xpresent_destroy);
	}
}

static bool subpresent_compute_root(struct wlx_subpresent *sp,
		int16_t *root_x, int16_t *root_y, int32_t *width, int32_t *height) {
	struct wlx_window *parent = sp->parent;
	struct wlr_surface *surf = sp->surface;
	if (!parent || !surf || !sp->server) {
		return false;
	}
	int lw = surf->current.width;
	int lh = surf->current.height;
	if (lw <= 0 || lh <= 0) {
		return false;
	}

	int16_t ox = 0, oy = 0;
	if (!window_content_root_position(parent, &ox, &oy)) {
		return false;
	}

	int lx = sp->sx;
	int ly = sp->sy;
	/* SSD: content window shows window-geometry region of the surface. */
	if (!sp->server->prefer_csd && parent->toplevel) {
		struct wlr_box geo = parent->toplevel->base->current.geometry;
		if (geo.width > 0 && geo.height > 0) {
			lx -= geo.x;
			ly -= geo.y;
		}
	}

	*root_x = (int16_t)(ox + wlx_scale_size(sp->server, lx));
	*root_y = (int16_t)(oy + wlx_scale_size(sp->server, ly));
	*width = wlx_scale_size(sp->server, lw);
	*height = wlx_scale_size(sp->server, lh);
	if (*width < 1) {
		*width = 1;
	}
	if (*height < 1) {
		*height = 1;
	}
	return true;
}

static bool subpresent_render_and_present(struct wlx_subpresent *sp) {
	if (!sp->xpresent || !sp->surface || !sp->server) {
		return false;
	}
	struct wlx_server *server = sp->server;
	int32_t width = 0, height = 0;
	int16_t root_x = 0, root_y = 0;
	wlr_x11_present_window_get_geometry(sp->xpresent,
		&root_x, &root_y, &width, &height);
	if (width < 1 || height < 1) {
		return false;
	}

	struct wlr_surface *surf = sp->surface;
	/* Same rule as xdg_popup: keep scaled present size under --scale;
	 * only match window to buffer when scale is 1. */
	if (surf->buffer) {
		int bw = surf->buffer->base.width;
		int bh = surf->buffer->base.height;
		bool scale_1 = server->content_scale <= 0.0 ||
			server->content_scale == 1.0;
		if (scale_1 && bw > 0 && bh > 0 && (bw != width || bh != height)) {
			wlr_x11_present_window_configure(sp->xpresent,
				root_x, root_y, bw, bh);
			width = bw;
			height = bh;
			sp->root_w = bw;
			sp->root_h = bh;
		}
		if (bw == width && bh == height) {
			if (wlr_x11_present_window_present(sp->xpresent,
					&surf->buffer->base)) {
				return true;
			}
			wlr_log(WLR_DEBUG, "subpresent: client-buffer present failed "
				"(%dx%d) — compositing", bw, bh);
		}
	}

	const struct wlr_drm_format *fmt =
		wlr_x11_backend_get_buffer_format(server->backend);
	if (!fmt || !server->allocator || !server->renderer) {
		return false;
	}
	struct wlr_buffer *buf = wlr_allocator_create_buffer(
		server->allocator, width, height, fmt);
	if (!buf) {
		return false;
	}
	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(
		server->renderer, buf, NULL);
	if (!pass) {
		wlr_buffer_drop(buf);
		return false;
	}
	wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
		.box = { .x = 0, .y = 0, .width = width, .height = height },
		.color = { .r = 0, .g = 0, .b = 0, .a = 0 },
	});
	struct wlr_texture *tex = wlr_surface_get_texture(surf);
	if (tex) {
		wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options){
			.texture = tex,
			.dst_box = { .x = 0, .y = 0, .width = width, .height = height },
			.blend_mode = WLR_RENDER_BLEND_MODE_PREMULTIPLIED,
		});
	}
	if (!wlr_render_pass_submit(pass)) {
		wlr_buffer_drop(buf);
		return false;
	}
	bool ok = wlr_x11_present_window_present(sp->xpresent, buf);
	wlr_buffer_drop(buf);
	return ok;
}

static void subpresent_position_and_present(struct wlx_subpresent *sp) {
	if (!sp->surface || !sp->surface->mapped) {
		return;
	}
	int16_t root_x = 0, root_y = 0;
	int32_t width = 1, height = 1;
	if (!subpresent_compute_root(sp, &root_x, &root_y, &width, &height)) {
		return;
	}

	if (!sp->xpresent) {
		sp->xpresent = wlr_x11_present_window_create(sp->server->backend);
		if (!sp->xpresent) {
			wlr_log(WLR_ERROR, "subpresent: create present-window failed");
			return;
		}
		subpresent_attach_xpresent_listeners(sp);
		wlr_log(WLR_INFO, "subpresent: OR window for subsurface %dx%d "
			"at root (%d,%d) offset (%d,%d)",
			width, height, root_x, root_y, sp->sx, sp->sy);
	}

	wlr_x11_present_window_configure(sp->xpresent,
		root_x, root_y, width, height);
	wlr_x11_present_window_map(sp->xpresent);
	sp->root_x = root_x;
	sp->root_y = root_y;
	sp->root_w = width;
	sp->root_h = height;
	sp->root_box_valid = true;

	/* Parent scene must not paint the clipped copy. */
	subpresent_set_scene_enabled(sp, false);

	if (!subpresent_render_and_present(sp)) {
		wlr_log(WLR_DEBUG, "subpresent: present failed (%dx%d)", width, height);
	}
}

static void subpresent_handle_surface_commit(struct wl_listener *listener,
		void *data) {
	(void)data;
	struct wlx_subpresent *sp =
		wl_container_of(listener, sp, surface_commit);
	subpresent_position_and_present(sp);
}

static void subpresent_handle_surface_destroy(struct wl_listener *listener,
		void *data) {
	(void)data;
	struct wlx_subpresent *sp =
		wl_container_of(listener, sp, surface_destroy);
	subpresent_destroy(sp);
}

static void subpresent_destroy(struct wlx_subpresent *sp) {
	if (!sp) {
		return;
	}
	subpresent_set_scene_enabled(sp, true);
	if (sp->xpresent) {
		if (!wl_list_empty(&sp->xpresent_frame.link)) {
			wl_list_remove(&sp->xpresent_frame.link);
			wl_list_init(&sp->xpresent_frame.link);
		}
		if (!wl_list_empty(&sp->xpresent_destroy.link)) {
			wl_list_remove(&sp->xpresent_destroy.link);
			wl_list_init(&sp->xpresent_destroy.link);
		}
		wlr_x11_present_window_destroy(sp->xpresent);
		sp->xpresent = NULL;
	}
	if (!wl_list_empty(&sp->surface_commit.link)) {
		wl_list_remove(&sp->surface_commit.link);
	}
	if (!wl_list_empty(&sp->surface_destroy.link)) {
		wl_list_remove(&sp->surface_destroy.link);
	}
	wl_list_remove(&sp->link);
	free(sp);
}

static struct wlx_subpresent *subpresent_get_or_create(struct wlx_window *win,
		struct wlr_surface *surface, int sx, int sy) {
	struct wlx_subpresent *sp = subpresent_find(win->server, surface);
	if (sp) {
		sp->sx = sx;
		sp->sy = sy;
		sp->parent = win;
		return sp;
	}
	sp = calloc(1, sizeof(*sp));
	if (!sp) {
		return NULL;
	}
	sp->server = win->server;
	sp->parent = win;
	sp->surface = surface;
	sp->sx = sx;
	sp->sy = sy;
	wl_list_init(&sp->xpresent_frame.link);
	wl_list_init(&sp->xpresent_destroy.link);

	sp->surface_commit.notify = subpresent_handle_surface_commit;
	wl_signal_add(&surface->events.commit, &sp->surface_commit);
	sp->surface_destroy.notify = subpresent_handle_surface_destroy;
	wl_signal_add(&surface->events.destroy, &sp->surface_destroy);

	wl_list_insert(&win->server->subpresents, &sp->link);
	return sp;
}

struct subpresent_sync_ctx {
	struct wlx_window *win;
	struct wlr_surface *root;
	int root_w, root_h;
	/* Surfaces still overflowing this pass. */
	struct wlr_surface **keep;
	size_t n_keep, cap_keep;
};

static void subpresent_sync_iterator(struct wlr_surface *surface,
		int sx, int sy, void *data) {
	struct subpresent_sync_ctx *ctx = data;
	if (surface == ctx->root) {
		return;
	}
	if (!surface->mapped) {
		return;
	}
	int w = surface->current.width;
	int h = surface->current.height;
	if (w <= 0 || h <= 0) {
		return;
	}
	/* Overflows parent surface buffer bounds? */
	bool overflows = sx < 0 || sy < 0
		|| sx + w > ctx->root_w
		|| sy + h > ctx->root_h;
	if (!overflows) {
		return;
	}

	if (ctx->n_keep >= ctx->cap_keep) {
		size_t ncap = ctx->cap_keep ? ctx->cap_keep * 2 : 8;
		struct wlr_surface **n = realloc(ctx->keep,
			ncap * sizeof(*n));
		if (!n) {
			return;
		}
		ctx->keep = n;
		ctx->cap_keep = ncap;
	}
	ctx->keep[ctx->n_keep++] = surface;

	struct wlx_subpresent *sp = subpresent_get_or_create(ctx->win,
		surface, sx, sy);
	if (sp) {
		subpresent_position_and_present(sp);
	}
}

void wlx_window_sync_subpresents(struct wlx_window *win) {
	if (!win || !win->server || !win->toplevel || !win->toplevel->base) {
		return;
	}
	struct wlr_surface *root = win->toplevel->base->surface;
	if (!root || !win->output) {
		/* Unmapped: drop all subpresents for this window. */
		wlx_window_destroy_subpresents(win);
		return;
	}

	struct subpresent_sync_ctx ctx = {
		.win = win,
		.root = root,
		.root_w = root->current.width,
		.root_h = root->current.height,
	};
	wlr_surface_for_each_surface(root, subpresent_sync_iterator, &ctx);

	/* Demote subpresents for this parent that are no longer overflowing. */
	struct wlx_subpresent *sp, *tmp;
	wl_list_for_each_safe(sp, tmp, &win->server->subpresents, link) {
		if (sp->parent != win) {
			continue;
		}
		bool still = false;
		for (size_t i = 0; i < ctx.n_keep; i++) {
			if (ctx.keep[i] == sp->surface) {
				still = true;
				break;
			}
		}
		if (!still) {
			wlr_log(WLR_INFO, "subpresent: demote (no longer overflow)");
			subpresent_destroy(sp);
		}
	}
	free(ctx.keep);
}

void wlx_window_destroy_subpresents(struct wlx_window *win) {
	if (!win || !win->server) {
		return;
	}
	struct wlx_subpresent *sp, *tmp;
	wl_list_for_each_safe(sp, tmp, &win->server->subpresents, link) {
		if (sp->parent == win) {
			subpresent_destroy(sp);
		}
	}
}

struct wlx_subpresent *subpresent_at_root_pointer(struct wlx_server *server) {
	if (!server) {
		return NULL;
	}
	int16_t px = 0, py = 0;
	if (!query_root_pointer_position(server, &px, &py)) {
		return NULL;
	}
	/* Newest first (list is insert-front). */
	struct wlx_subpresent *sp;
	wl_list_for_each(sp, &server->subpresents, link) {
		if (!sp->root_box_valid || !sp->xpresent) {
			continue;
		}
		if (!wlr_x11_present_window_is_mapped(sp->xpresent)) {
			continue;
		}
		if (px >= sp->root_x && py >= sp->root_y
				&& px < sp->root_x + sp->root_w
				&& py < sp->root_y + sp->root_h) {
			return sp;
		}
	}
	return NULL;
}

struct wlr_surface *subpresent_surface_at(struct wlx_subpresent *sp,
		double *sx, double *sy) {
	if (!sp || !sp->surface || !sp->root_box_valid) {
		return NULL;
	}
	int16_t px = 0, py = 0;
	if (!query_root_pointer_position(sp->server, &px, &py)) {
		return NULL;
	}
	double pix_sx = (double)(px - sp->root_x);
	double pix_sy = (double)(py - sp->root_y);
	double lsx = pix_sx, lsy = pix_sy;
	wlx_pointer_to_surface(sp->server, &lsx, &lsy);
	if (sx) {
		*sx = lsx;
	}
	if (sy) {
		*sy = lsy;
	}
	return sp->surface;
}

struct wlx_window *subpresent_parent(struct wlx_subpresent *sp) {
	return sp ? sp->parent : NULL;
}

void wlx_window_reposition_subpresents(struct wlx_window *win) {
	if (!win || !win->server) {
		return;
	}
	struct wlx_subpresent *sp;
	wl_list_for_each(sp, &win->server->subpresents, link) {
		if (sp->parent != win || !sp->xpresent) {
			continue;
		}
		subpresent_position_and_present(sp);
	}
}
