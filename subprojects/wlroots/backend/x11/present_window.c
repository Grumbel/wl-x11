#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include <xcb/present.h>
#include <xcb/xcb.h>
#include <xcb/xinput.h>

#include <wlr/interfaces/wlr_output.h>
#include <wlr/util/log.h>

#include "backend/x11.h"
#include "types/wlr_buffer.h"

struct wlr_x11_present_window *get_x11_present_window_from_window_id(
		struct wlr_x11_backend *x11, xcb_window_t window) {
	struct wlr_x11_present_window *win;
	wl_list_for_each(win, &x11->present_windows, link) {
		if (win->win == window) {
			return win;
		}
	}
	return NULL;
}

struct wlr_x11_present_window *wlr_x11_present_window_create(
		struct wlr_backend *backend) {
	if (!wlr_backend_is_x11(backend)) {
		return NULL;
	}
	struct wlr_x11_backend *x11 = get_x11_backend_from_backend(backend);
	if (!x11->started) {
		wlr_log(WLR_ERROR,
			"wlr_x11_present_window_create: backend not started");
		return NULL;
	}

	struct wlr_x11_present_window *win = calloc(1, sizeof(*win));
	if (!win) {
		return NULL;
	}
	win->x11 = x11;
	wl_list_init(&win->buffers);
	wl_signal_init(&win->events.destroy);
	wl_signal_init(&win->events.frame);

	/* Override-redirect root child: no WM frame, free root-space placement.
	 * ARGB visual + transparent back so host compositor can blend menus. */
	uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL |
		XCB_CW_BIT_GRAVITY | XCB_CW_WIN_GRAVITY | XCB_CW_BACKING_STORE |
		XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK | XCB_CW_COLORMAP |
		XCB_CW_CURSOR;
	uint32_t values[] = {
		0, /* back_pixel */
		0, /* border_pixel */
		XCB_GRAVITY_NORTH_WEST,
		XCB_GRAVITY_NORTH_WEST,
		XCB_BACKING_STORE_WHEN_MAPPED,
		1, /* override_redirect */
		XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY,
		x11->colormap,
		x11->transparent_cursor,
	};

	win->win = xcb_generate_id(x11->xcb);
	win->width = 1;
	win->height = 1;
	win->root_x = 0;
	win->root_y = 0;

	xcb_create_window(x11->xcb, x11->depth->depth, win->win,
		x11->screen->root, 0, 0, 1, 1, 0,
		XCB_WINDOW_CLASS_INPUT_OUTPUT, x11->visualid, mask, values);

	/* EWMH window type: popup menu — informational for host compositors. */
	if (x11->atoms.net_wm_window_type != XCB_ATOM_NONE &&
			x11->atoms.net_wm_window_type_popup_menu != XCB_ATOM_NONE) {
		xcb_atom_t types[] = {
			x11->atoms.net_wm_window_type_popup_menu,
			x11->atoms.net_wm_window_type_dropdown_menu,
			x11->atoms.net_wm_window_type_menu,
		};
		xcb_change_property(x11->xcb, XCB_PROP_MODE_REPLACE, win->win,
			x11->atoms.net_wm_window_type, XCB_ATOM_ATOM, 32,
			3, types);
	}

	/* No WM_PROTOCOLS / WM_DELETE_WINDOW: OR menus are not WM-managed.
	 * No WM_NORMAL_HINTS: compositor positions via ConfigureWindow. */

	uint32_t present_mask = XCB_PRESENT_EVENT_MASK_IDLE_NOTIFY |
		XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY;
	win->present_event_id = xcb_generate_id(x11->xcb);
	xcb_present_select_input(x11->xcb, win->present_event_id, win->win,
		present_mask);

	/* Select structure notify only — pointer routing stays on the
	 * compositor's single seat / root-query path (no per-window wlr_pointer). */
	xcb_flush(x11->xcb);

	wl_list_insert(&x11->present_windows, &win->link);
	return win;
}

void wlr_x11_present_window_configure(struct wlr_x11_present_window *win,
		int16_t root_x, int16_t root_y, int32_t width, int32_t height) {
	if (!win || !win->x11) {
		return;
	}
	if (width < 1) {
		width = 1;
	}
	if (height < 1) {
		height = 1;
	}

	struct wlr_x11_backend *x11 = win->x11;
	bool pos_changed = (win->root_x != root_x || win->root_y != root_y);
	bool size_changed = (win->width != width || win->height != height);

	if (!pos_changed && !size_changed) {
		return;
	}

	uint32_t value_mask = 0;
	uint32_t values[4];
	size_t n = 0;
	if (pos_changed) {
		value_mask |= XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y;
		values[n++] = (uint32_t)(int32_t)root_x;
		values[n++] = (uint32_t)(int32_t)root_y;
	}
	if (size_changed) {
		value_mask |= XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;
		values[n++] = (uint32_t)width;
		values[n++] = (uint32_t)height;
	}

	xcb_configure_window(x11->xcb, win->win, value_mask, values);
	xcb_flush(x11->xcb);

	win->root_x = root_x;
	win->root_y = root_y;
	win->width = width;
	win->height = height;
}

void wlr_x11_present_window_map(struct wlr_x11_present_window *win) {
	if (!win || !win->x11 || win->mapped) {
		return;
	}
	xcb_map_window(win->x11->xcb, win->win);
	/* Keep above siblings so nested menus stack correctly. */
	const uint32_t values[] = { XCB_STACK_MODE_ABOVE };
	xcb_configure_window(win->x11->xcb, win->win,
		XCB_CONFIG_WINDOW_STACK_MODE, values);
	xcb_flush(win->x11->xcb);
	win->mapped = true;
}

void wlr_x11_present_window_unmap(struct wlr_x11_present_window *win) {
	if (!win || !win->x11 || !win->mapped) {
		return;
	}
	xcb_unmap_window(win->x11->xcb, win->win);
	xcb_flush(win->x11->xcb);
	win->mapped = false;
}

bool wlr_x11_present_window_present(struct wlr_x11_present_window *win,
		struct wlr_buffer *buffer) {
	if (!win || !win->x11 || !buffer) {
		return false;
	}
	struct wlr_x11_backend *x11 = win->x11;

	if (buffer->width != win->width || buffer->height != win->height) {
		wlr_log(WLR_DEBUG,
			"present_window: buffer size %dx%d != window %dx%d",
			buffer->width, buffer->height, win->width, win->height);
		return false;
	}

	struct wlr_x11_buffer *x11_buffer = x11_buffer_get_or_create(
		x11, win->win, &win->buffers, buffer);
	if (!x11_buffer) {
		return false;
	}

	/* Always full present: menus are typically ARGB; partial Present leaves
	 * stale alpha pixels (same rationale as output_commit_buffer). */
	win->present_serial++;
	uint32_t serial = win->present_serial;
	uint64_t target_msc = win->last_msc ? win->last_msc + 1 : 0;
	xcb_present_pixmap(x11->xcb, win->win, x11_buffer->pixmap, serial,
		0, XCB_NONE, 0, 0, XCB_NONE, XCB_NONE, XCB_NONE, 0, target_msc,
		0, 0, 0, NULL);
	xcb_flush(x11->xcb);
	return true;
}

xcb_window_t wlr_x11_present_window_get_xcb(
		struct wlr_x11_present_window *win) {
	return win ? win->win : XCB_WINDOW_NONE;
}

void wlr_x11_present_window_get_geometry(struct wlr_x11_present_window *win,
		int16_t *root_x, int16_t *root_y, int32_t *width, int32_t *height) {
	if (root_x) {
		*root_x = win ? win->root_x : 0;
	}
	if (root_y) {
		*root_y = win ? win->root_y : 0;
	}
	if (width) {
		*width = win ? win->width : 0;
	}
	if (height) {
		*height = win ? win->height : 0;
	}
}

bool wlr_x11_present_window_is_mapped(struct wlr_x11_present_window *win) {
	return win && win->mapped;
}

void wlr_x11_present_window_destroy(struct wlr_x11_present_window *win) {
	if (!win) {
		return;
	}
	struct wlr_x11_backend *x11 = win->x11;

	wl_signal_emit_mutable(&win->events.destroy, win);

	struct wlr_x11_buffer *buffer, *tmp;
	wl_list_for_each_safe(buffer, tmp, &win->buffers, link) {
		x11_buffer_destroy(buffer);
	}

	if (x11 && x11->xcb) {
		xcb_present_select_input(x11->xcb, win->present_event_id, win->win, 0);
		xcb_destroy_window(x11->xcb, win->win);
		xcb_flush(x11->xcb);
	}

	wl_list_remove(&win->link);
	free(win);
}

void wlr_x11_present_window_add_frame_listener(
		struct wlr_x11_present_window *win, struct wl_listener *listener) {
	wl_signal_add(&win->events.frame, listener);
}

void wlr_x11_present_window_add_destroy_listener(
		struct wlr_x11_present_window *win, struct wl_listener *listener) {
	wl_signal_add(&win->events.destroy, listener);
}
