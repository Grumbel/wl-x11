/* SPDX-FileCopyrightText: Copyright (c) 2026 Ingo Ruhnke <grumbel@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>

int wlx_scale_size(struct wlx_server *server, int logical) {
	if (!server || server->content_scale <= 0.0) {
		return logical;
	}
	int v = (int)lround((double)logical * server->content_scale);
	return v < 1 ? 1 : v;
}

int wlx_unscale_size(struct wlx_server *server, int output_px) {
	if (!server || server->content_scale <= 0.0) {
		return output_px;
	}
	int v = (int)lround((double)output_px / server->content_scale);
	return v < 1 ? 1 : v;
}

/* Convert host/output pixel coords (or scaled scene dest coords) into
 * surface-local logical coordinates for wl_pointer events. */
void wlx_pointer_to_surface(struct wlx_server *server, double *sx, double *sy) {
	if (!server || !sx || !sy || server->content_scale <= 0.0) {
		return;
	}
	if (server->content_scale == 1.0) {
		return;
	}
	*sx /= server->content_scale;
	*sy /= server->content_scale;
}

/*
 * Brute-force pixel scale: enlarge/shrink every scene buffer's destination
 * rectangle and scale subsurface positions. Clients still allocate logical
 * buffers; the X11 window is logical*scale. Re-run after each surface commit
 * because wlr_scene_xdg_surface resets dest size on commit.
 */
static void scene_node_apply_scale(struct wlr_scene_node *node, double scale) {
	if (!node || !node->enabled) {
		return;
	}
	if (node->type == WLR_SCENE_NODE_BUFFER) {
		struct wlr_scene_buffer *sb = wlr_scene_buffer_from_node(node);
		struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(sb);
		if (ss && ss->surface) {
			int bw = ss->surface->current.width;
			int bh = ss->surface->current.height;
			if (bw > 0 && bh > 0) {
				wlr_scene_buffer_set_dest_size(sb,
					(int)lround(bw * scale), (int)lround(bh * scale));
			}
		}
		return;
	}
	if (node->type == WLR_SCENE_NODE_TREE) {
		struct wlr_scene_tree *tree = wlr_scene_tree_from_node(node);
		struct wlr_scene_node *child;
		wl_list_for_each(child, &tree->children, link) {
			int ox = child->x;
			int oy = child->y;
			if (ox != 0 || oy != 0) {
				wlr_scene_node_set_position(child,
					(int)lround(ox * scale), (int)lround(oy * scale));
			}
			scene_node_apply_scale(child, scale);
		}
	}
}

void wlx_apply_content_scale(struct wlx_window *win) {
	if (!win || !win->server || !win->scene_tree) {
		return;
	}
	double scale = win->server->content_scale;
	if (scale <= 0.0 || scale == 1.0) {
		return;
	}
	scene_node_apply_scale(&win->scene_tree->node, scale);
}

void output_frame(struct wl_listener *listener, void *data) {
	struct wlx_window *win = wl_container_of(listener, win, output_frame);
	if (!win->scene_output || !win->output) {
		return;
	}

	/* Always present at the client's native buffer size (no stretch). During
	 * hold_present the last buffer is letterboxed on a transparent clear so
	 * resize does not flash fully empty, without scaling drop-shadows. */
	wlr_scene_output_commit(win->scene_output, NULL);

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(win->scene_output, &now);
}

void output_commit(struct wl_listener *listener, void *data) {
	struct wlx_window *win = wl_container_of(listener, win, output_commit);
	(void)data;

	wlr_log(WLR_DEBUG, "output commit event (current size %dx%d, last known %dx%d)",
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
	/* Only hold the previous frame if we already had a painted size.
	 * Avoid blocking the first map frame. */
	bool had_frame = win->last_output_width > WLX_MIN_OUTPUT_SIZE &&
		win->last_output_height > WLX_MIN_OUTPUT_SIZE;
	win->last_output_width = w;
	win->last_output_height = h;
	if (had_frame) {
		/* Paint immediately with the last client buffer at native size
		 * (letterboxed). Stretching filled the hole but corrupted ARGB
		 * shadows; skipping paint left a fully transparent flash. */
		win->hold_present = true;
		if (win->scene_output) {
			wlr_scene_output_commit(win->scene_output, NULL);
		}
	} else {
		win->hold_present = false;
		wlr_output_schedule_frame(win->output);
	}

	wlr_log(WLR_DEBUG, "X11 window resized to %dx%d, propagating to toplevel", w, h);
	if (win->toplevel) {
		/* Client sees logical window geometry; host may be larger by CSD
		 * shadow margin. Maximized/fullscreen drop the shadow — never
		 * subtract a stale margin or the client undersizes the X window
		 * (empty right/bottom border). Host resize often races ahead of
		 * _NET_WM_STATE; also treat size_from_wm + zero margins as tiled. */
		int lw = wlx_unscale_size(win->server, w);
		int lh = wlx_unscale_size(win->server, h);
		bool tiled = win->toplevel->current.maximized ||
			win->toplevel->pending.maximized ||
			win->toplevel->current.fullscreen ||
			win->toplevel->pending.fullscreen;
		/* After maximize the property handler clears margins; a resize that
		 * already ran with old margins is fixed by that handler re-sending
		 * size. While margins are still non-zero, only skip subtract if the
		 * xdg state already knows we are tiled. */
		if (win->server->prefer_csd && !tiled &&
				(win->csd_margin_w > 0 || win->csd_margin_h > 0)) {
			lw -= win->csd_margin_w;
			lh -= win->csd_margin_h;
			if (lw < 1) {
				lw = 1;
			}
			if (lh < 1) {
				lh = 1;
			}
		}
		wlx_toplevel_set_size(win, lw, lh);
	}
	/* Keep host WM constraints aligned with the new size / xdg min-max. */
	win_sync_size_hints(win);
}

/* Host WM sent WM_DELETE_WINDOW. Ask the client to close; keep the X11
 * window (and wlr_output) until the client unmaps/destroys the surface. */
static void output_request_close(struct wl_listener *listener, void *data) {
	struct wlx_window *win = wl_container_of(listener, win, output_request_close);
	(void)data;
	if (win->toplevel) {
		wlr_log(WLR_INFO, "X11 WM_DELETE_WINDOW → xdg_toplevel.close");
		wlr_xdg_toplevel_send_close(win->toplevel);
	}
}

void output_destroy(struct wl_listener *listener, void *data) {
	struct wlx_window *win = wl_container_of(listener, win, output_destroy);
	(void)data;

	/* Reached when the client closed the surface (unmap path) or the
	 * backend is tearing down. Close requests from the host WM no longer
	 * destroy the output — see output_request_close. */

	wl_list_remove(&win->output_frame.link);
	wl_list_remove(&win->output_destroy.link);
	wl_list_remove(&win->output_request_close.link);
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

/* Preferred host window size (logical pixels before content_scale).
 *
 * --csd: use the full surface buffer so client-drawn shadows/padding are
 * visible (host is buffer-sized).
 *
 * SSD (default): prefer xdg window geometry so any leftover client shadow
 * margin is clipped by the X11 window; fall back to buffer if geometry
 * is unset. */
void toplevel_preferred_size(struct wlx_window *win, int *w_out, int *h_out) {
	int w = 0, h = 0;
	if (win->toplevel && win->toplevel->base) {
		struct wlr_box geo = win->toplevel->base->current.geometry;
		struct wlr_surface *surf = win->toplevel->base->surface;
		bool use_geo = win->server && !win->server->prefer_csd &&
			geo.width > 0 && geo.height > 0;
		if (use_geo) {
			w = geo.width;
			h = geo.height;
		} else if (surf && surf->current.width > 0 && surf->current.height > 0) {
			w = surf->current.width;
			h = surf->current.height;
		} else {
			w = geo.width;
			h = geo.height;
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

void wlx_toplevel_set_size(struct wlx_window *win, int width, int height) {
	if (!win || !win->toplevel) {
		return;
	}
	wlr_xdg_toplevel_set_size(win->toplevel, width, height);
	if (width > 0 && height > 0) {
		win->last_client_conf_w = width;
		win->last_client_conf_h = height;
	}
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

/* ICCCM WM_NORMAL_HINTS — size (+ optional min/max), no USPosition/PPosition.
 * Omitting position flags is the signal for the host WM to place the window
 * itself. Never ConfigureWindow(x,y) for ordinary top-levels. */
void xwin_set_size_hints(struct wlx_server *s, xcb_window_t w,
		int width, int height, int min_width, int min_height,
		int max_width, int max_height) {
	if (!s->xcb || w == XCB_WINDOW_NONE ||
			s->atom_wm_normal_hints == XCB_ATOM_NONE ||
			s->atom_wm_size_hints == XCB_ATOM_NONE ||
			width <= 0 || height <= 0) {
		return;
	}
	/* Layout matches xcb_size_hints_t / XSizeHints (18 × int32). */
	int32_t hints[18] = {0};
	/* PSize | PMinSize (bits 3 and 4). */
	hints[0] = (1 << 3) | (1 << 4);
	hints[3] = width;
	hints[4] = height;
	hints[5] = min_width > 0 ? min_width : 1;
	hints[6] = min_height > 0 ? min_height : 1;
	if (max_width > 0 && max_height > 0) {
		hints[0] |= (1 << 5); /* PMaxSize */
		hints[7] = max_width;
		hints[8] = max_height;
	}
	xcb_change_property(s->xcb, XCB_PROP_MODE_REPLACE, w,
		s->atom_wm_normal_hints, s->atom_wm_size_hints, 32, 18, hints);
}

void win_sync_size_hints(struct wlx_window *win) {
	if (!win || !win->server || win->xwin == XCB_WINDOW_NONE) {
		return;
	}
	/* Prefer last_* when set (pre-map preferred size, or after a commit).
	 * Fall back to the live output mode. */
	int width = win->last_output_width > 0 ? win->last_output_width :
		(win->output ? win->output->width : 0);
	int height = win->last_output_height > 0 ? win->last_output_height :
		(win->output ? win->output->height : 0);
	if (width <= 0 || height <= 0) {
		return;
	}

	int min_w = 1, min_h = 1, max_w = 0, max_h = 0;
	if (win->toplevel) {
		/* xdg sizes are logical; host window is scaled pixels. */
		int32_t tmin_w = win->toplevel->current.min_width;
		int32_t tmin_h = win->toplevel->current.min_height;
		int32_t tmax_w = win->toplevel->current.max_width;
		int32_t tmax_h = win->toplevel->current.max_height;
		if (tmin_w > 0) {
			min_w = wlx_scale_size(win->server, tmin_w);
		}
		if (tmin_h > 0) {
			min_h = wlx_scale_size(win->server, tmin_h);
		}
		/* xdg: 0 means unconstrained. */
		if (tmax_w > 0 && tmax_h > 0) {
			max_w = wlx_scale_size(win->server, tmax_w);
			max_h = wlx_scale_size(win->server, tmax_h);
		}
	}

	xwin_set_size_hints(win->server, win->xwin,
		width, height, min_w, min_h, max_w, max_h);
	if (win->content_xwin != XCB_WINDOW_NONE &&
			win->content_xwin != win->xwin) {
		xwin_set_size_hints(win->server, win->content_xwin,
			width, height, min_w, min_h, max_w, max_h);
	}
	if (win->server->xcb) {
		xcb_flush(win->server->xcb);
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

	/* Host is buffer-sized and the scene is forced to (0,0), so X11
	 * window-local coords already match surface-local coords. */
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
	/* output_commit may have already updated last_*, set_size, and
	 * WM_NORMAL_HINTS; keep last_* consistent if commit did not emit. */
	win->last_output_width = win->output->width;
	win->last_output_height = win->output->height;
	win_sync_size_hints(win);
	wlr_output_schedule_frame(win->output);
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

	/* With --csd, transient dialogs use override-redirect so the host WM
	 * cannot reparent/shrink the buffer (CSD shadows stay visible). Without
	 * --csd, dialogs stay managed and get normal host decorations. */
	bool is_transient = win->toplevel && win->toplevel->parent != NULL;
	bool use_or = is_transient && server->prefer_csd;
	struct wlr_output *output = use_or
		? wlr_x11_output_create_override_redirect(server->backend)
		: wlr_x11_output_create(server->backend);
	if (!output) {
		wlr_log(WLR_ERROR, "failed to create X11 output for new toplevel");
		return;
	}
	if (use_or) {
		wlr_log(WLR_INFO, "transient toplevel → override-redirect X11 window "
			"(--csd, avoid host frame clipping)");
	}
	win->output = output;
	output->data = win;

	wlr_output_init_render(output, server->allocator, server->renderer);

	/* Preferred size for mode+map (backend applies MODE before MapWindow). */
	int want_w = 0, want_h = 0;
	toplevel_preferred_size(win, &want_w, &want_h);
	{
		int logical_w = want_w, logical_h = want_h;
		want_w = wlx_scale_size(server, logical_w);
		want_h = wlx_scale_size(server, logical_h);
	}

	/* Stable window id from the backend — no root-child scan. Properties
	 * are set on this window before map so the host WM sees them on
	 * MapRequest. */
	win->xwin = wlr_x11_output_get_window(output);
	if (win->xwin != XCB_WINDOW_NONE) {
		wlr_log(WLR_INFO, "backing X11 window id 0x%x (unmapped)", win->xwin);
		win->content_xwin = win->xwin;
		win->related_count = 0;
		memset(win->related, 0, sizeof(win->related));
		if (win->related_count < WLX_MAX_RELATED_WINDOWS) {
			win->related[win->related_count++] = win->xwin;
		}

		apply_transient_hints(win);
		xwin_set_title(server, win->xwin, win->toplevel->title);
		xwin_set_class(server, win->xwin, win->toplevel->app_id);
		/* --csd: no host border (client draws chrome). Default: host SSD. */
		xwin_set_motif_decorations(server, win->xwin, !server->prefer_csd);
		/* Size hints before MapWindow (no position flags). last_* not set
		 * yet — pass preferred size via a temporary so win_sync works. */
		win->last_output_width = want_w > 0 ? want_w : output->width;
		win->last_output_height = want_h > 0 ? want_h : output->height;
		win_sync_size_hints(win);
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
			xwin_set_motif_decorations(server, win->content_xwin, !server->prefer_csd);
			win_sync_size_hints(win);
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
		wlr_log(WLR_ERROR, "wlr_x11_output_get_window returned none "
			"(title/class won't be synced)");
	}

	/* Size then map in one commit. The X11 backend applies MODE before
	 * MapWindow so the host WM sees the real client size on MapRequest
	 * (mouse/center placement). Mapping at the create-time 1x1 placeholder
	 * and resizing afterward would leave the frame mis-anchored. */
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	if (want_w > 0 && want_h > 0) {
		wlr_output_state_set_custom_mode(&state, want_w, want_h, 0);
	}
	wlr_output_state_set_enabled(&state, true);
	if (!wlr_output_commit_state(output, &state)) {
		wlr_log(WLR_ERROR, "failed to map new X11 output at %dx%d",
			want_w > 0 ? want_w : output->width,
			want_h > 0 ? want_h : output->height);
	}
	wlr_output_state_finish(&state);

	wlr_log(WLR_INFO, "new X11 output committed at %dx%d",
		output->width, output->height);
	win->last_output_width = output->width;
	win->last_output_height = output->height;

	/* OR dialogs need an explicit root position (no WM placement). */
	if (use_or && win->xwin != XCB_WINDOW_NONE && server->xcb) {
		struct wlx_window *parent_win = NULL;
		if (win->toplevel->parent && win->toplevel->parent->base) {
			parent_win = win->toplevel->parent->base->data;
		}
		int16_t px = 100, py = 100;
		if (parent_win) {
			xcb_window_t pw = parent_win->content_xwin != XCB_WINDOW_NONE
				? parent_win->content_xwin : parent_win->xwin;
			int16_t ox = 0, oy = 0;
			query_window_root_position(server, pw, &ox, &oy);
			int pw_w = parent_win->output ? parent_win->output->width : 800;
			int pw_h = parent_win->output ? parent_win->output->height : 600;
			px = ox + (int16_t)((pw_w - output->width) / 2);
			py = oy + (int16_t)((pw_h - output->height) / 2);
			if (px < 0) {
				px = 0;
			}
			if (py < 0) {
				py = 0;
			}
		}
		uint32_t vals[] = { (uint32_t)px, (uint32_t)py };
		xcb_configure_window(server->xcb, win->xwin,
			XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, vals);
		xcb_flush(server->xcb);
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

	win->output_request_close.notify = output_request_close;
	wlr_x11_output_add_request_close_listener(output, &win->output_request_close);
	win->output_commit.notify = output_commit;
	wl_signal_add(&output->events.commit, &win->output_commit);
	win->output_request_state.notify = output_request_state;
	wl_signal_add(&output->events.request_state, &win->output_request_state);

	/* Do not lock the client to the first buffer size (0×0 = client picks).
	 * Host still opens at preferred size; surface_commit grows it if needed. */
	wlx_toplevel_set_size(win, 0, 0);
	wlx_apply_content_scale(win);

	/* First frame only after position/hints are applied. */
	wlr_output_schedule_frame(output);
}
