#ifndef WLR_BACKEND_X11_H
#define WLR_BACKEND_X11_H

#include <stdbool.h>

#include <wayland-server-core.h>

#include <wlr/backend.h>
#include <wlr/types/wlr_output.h>

struct wlr_input_device;

/**
 * Creates a new X11 backend. This backend will be created with no outputs;
 * you must use wlr_x11_output_create() to add them.
 *
 * The `x11_display` argument is the name of the X Display socket. Set
 * to NULL for the default behaviour of XOpenDisplay().
 */
struct wlr_backend *wlr_x11_backend_create(struct wl_event_loop *loop,
	const char *x11_display);

/**
 * Adds a new output to this backend. You may remove outputs by destroying them.
 * Note that if called before initializing the backend, this will return NULL
 * and your outputs will be created during initialization (and given to you via
 * the new_output signal).
 */
struct wlr_output *wlr_x11_output_create(struct wlr_backend *backend);

/**
 * Check whether this backend is an X11 backend.
 */
bool wlr_backend_is_x11(const struct wlr_backend *backend);

/**
 * Check whether this input device is an X11 input device.
 */
bool wlr_input_device_is_x11(struct wlr_input_device *device);

/**
 * Check whether this output device is an X11 output device.
 */
bool wlr_output_is_x11(const struct wlr_output *output);

/**
 * Sets the title of a struct wlr_output which is an X11 window.
 */
void wlr_x11_output_set_title(struct wlr_output *output, const char *title);

/**
 * Listen for a host-WM close request (WM_DELETE_WINDOW) on an X11 output.
 *
 * Unlike the stock behaviour of destroying the output immediately, the X11
 * backend keeps the window alive and emits this signal so the compositor can
 * ask the client to close (e.g. via xdg_toplevel.close). Destroy the output
 * only when the client has actually unmapped/destroyed the surface.
 *
 * data passed to the listener is the struct wlr_output *.
 */
void wlr_x11_output_add_request_close_listener(struct wlr_output *output,
	struct wl_listener *listener);

#endif
