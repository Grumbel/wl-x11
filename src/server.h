/* SPDX-FileCopyrightText: Copyright (c) 2026 Ingo Ruhnke <grumbel@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef WLX_SERVER_H
#define WLX_SERVER_H

#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
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
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>

#include <xkbcommon/xkbcommon.h>
#include <xcb/xcb.h>
#include <xcb/xfixes.h>

#define WLX_CLIPBOARD_MAX (1024 * 1024)
#define WLX_MAX_RELATED_WINDOWS 32
#define WLX_DEFAULT_WIDTH 1024
#define WLX_DEFAULT_HEIGHT 720
#define WLX_MIN_OUTPUT_SIZE 32
#define WLX_MIN_WINDOW_SIZE 50
#define WLX_DRAG_THROTTLE_MS 16

/* EWMH _NET_WM_STATE client-message action codes */
enum {
	_NET_WM_STATE_REMOVE = 0,
	_NET_WM_STATE_ADD = 1,
};

struct wlx_window;
struct wlx_keyboard;

struct wlx_server {
	struct wl_display *wl_display;
	struct wl_event_loop *loop;
	struct wlr_backend *backend;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;

	/* For `wl-x11 <command>`: track connected Wayland clients so we can
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
	/* Always-on virtual monitor so clients that require ≥1 wl_output at
	 * connect (foot, etc.) can start before any toplevel is mapped. */
	struct wlr_output *bootstrap_output;
	struct wlr_scene_output *bootstrap_scene_output;

	struct wlr_xdg_shell *xdg_shell;
	struct wl_listener new_xdg_toplevel;
	struct wl_listener new_xdg_popup; /* shell-level: all popups */

	struct wlr_xdg_decoration_manager_v1 *xdg_decoration_manager;
	struct wl_listener new_toplevel_decoration;
	/* KDE org_kde_kwin_server_decoration — still used by GTK3 and some
	 * GTK4 builds; without a SERVER default they freely draw CSD. */
	struct wlr_server_decoration_manager *server_decoration_manager;

	struct wlr_seat *seat;
	struct wlr_cursor *cursor;
	/* Compositor-side pixel scale (brute-force). 1.0 = native.
	 * X11 window size = logical * content_scale; client still sees
	 * logical size; scene buffers are dest-scaled to fill. */
	double content_scale;
	/* When true: ask clients for CSD and strip host WM decorations
	 * (_MOTIF_WM_HINTS decorations=0). Default is SSD via the host WM. */
	bool prefer_csd;
	struct wlr_xcursor_manager *cursor_mgr;
	bool have_keyboard;
	bool have_pointer;

	struct wl_listener new_input;
	struct wl_listener cursor_motion;
	struct wl_listener cursor_motion_absolute;
	struct wl_listener cursor_button;
	struct wl_listener cursor_axis;
	struct wl_listener cursor_frame;
	struct wl_listener request_set_cursor;
	struct wl_listener request_set_selection;
	struct wl_listener request_set_primary_selection;
	struct wl_listener request_start_drag;
	struct wl_listener start_drag;

	struct wl_list windows;
	struct wl_list popups; /* wlx_popup.link */
	struct wl_list subpresents; /* wlx_subpresent.link */
	/* X pointer grab while present-window menus are mapped. */
	bool popup_pointer_grabbed;
	/* Grab was deferred until the opening button is released. */
	bool popup_pointer_grab_pending;
	/* XI2 master pointer device id for grabs; 0 = unknown. */
	uint16_t xi_master_pointer_id;
	struct wlx_window *focused_window;

	/* Deferred pointer button release: held briefly so the client can
	 * process the matching press, create an xdg_popup and take a grab
	 * before the release is delivered. Otherwise Qt treats the release
	 * as "click outside" and destroys the menu in the same frame. */

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
	struct wlx_window *move_win;
	int move_offset_x, move_offset_y; /* window pos - pointer pos at drag start */

	struct wlx_window *resize_win;
	uint32_t resize_edges;
	int resize_start_x, resize_start_y, resize_start_w, resize_start_h;
	int16_t resize_start_pointer_x, resize_start_pointer_y;

	/* Throttles how often we actually poll+configure -- see
	 * WLX_DRAG_THROTTLE_MS. */
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

	/* Auxiliary connection: WM_NAME/WM_CLASS sync + text clipboard bridge. */
	xcb_connection_t *xcb;
	xcb_window_t xcb_root;
	xcb_atom_t atom_net_wm_name;
	xcb_atom_t atom_utf8_string;
	xcb_atom_t atom_net_wm_moveresize;
	xcb_atom_t atom_net_wm_state;
	xcb_atom_t atom_net_wm_state_maximized_vert;
	xcb_atom_t atom_net_wm_state_maximized_horz;
	xcb_atom_t atom_net_wm_state_fullscreen;
	xcb_atom_t atom_net_wm_state_modal;
	xcb_atom_t atom_wm_change_state;
	xcb_atom_t atom_wm_transient_for;
	xcb_atom_t atom_wm_normal_hints;
	xcb_atom_t atom_wm_size_hints;
	xcb_atom_t atom_motif_wm_hints;
	xcb_atom_t atom_net_wm_window_type;
	xcb_atom_t atom_net_wm_window_type_normal;
	xcb_atom_t atom_net_wm_window_type_dialog;
	xcb_atom_t atom_clipboard;
	xcb_atom_t atom_primary;
	xcb_atom_t atom_targets;
	xcb_atom_t atom_string;
	xcb_atom_t atom_text;
	xcb_atom_t atom_text_html; /* MIME text/html for TARGETS */
	xcb_atom_t atom_wlx_clipboard; /* property used for ConvertSelection */
	xcb_atom_t atom_wlx_primary;

	/* Invisible window that owns/serves X11 CLIPBOARD and PRIMARY. */
	xcb_window_t clipboard_window;
	uint8_t xfixes_event_base;
	bool xfixes_ok;

	/* Text published onto X11 CLIPBOARD (Wayland → X11). */
	char *clip_out_text;
	size_t clip_out_len;
	bool clip_we_own_x11;

	/* Text offered as Wayland clipboard from X11 CLIPBOARD. */
	char *clip_in_text;
	size_t clip_in_len;
	bool clip_setting_from_x11; /* suppress re-export loop */

	/* Same for X11 PRIMARY ↔ wp_primary_selection. */
	char *pri_out_text;
	size_t pri_out_len;
	bool pri_we_own_x11;
	char *pri_in_text;
	size_t pri_in_len;
	bool pri_setting_from_x11;

	/* In-progress async read of a Wayland data source for export to X11. */
	struct wl_event_source *clip_read_source;
	int clip_read_fd;
	char *clip_read_buf;
	size_t clip_read_len;
	size_t clip_read_cap;
	bool clip_read_is_primary; /* export target: PRIMARY vs CLIPBOARD */

	/* ---- XDND (X11 drag-and-drop), text / text/uri-list only ---- */
	xcb_atom_t atom_xdnd_aware;
	xcb_atom_t atom_xdnd_enter;
	xcb_atom_t atom_xdnd_position;
	xcb_atom_t atom_xdnd_status;
	xcb_atom_t atom_xdnd_leave;
	xcb_atom_t atom_xdnd_drop;
	xcb_atom_t atom_xdnd_finished;
	xcb_atom_t atom_xdnd_selection;
	xcb_atom_t atom_xdnd_type_list;
	xcb_atom_t atom_xdnd_action_copy;
	xcb_atom_t atom_text_uri_list;
	xcb_atom_t atom_wlx_dnd; /* property for ConvertSelection during drop */

	/* Wayland → X11 outbound drag */
	bool dnd_out_active;
	xcb_window_t dnd_out_target; /* current XdndPosition target */
	int dnd_out_version;
	bool dnd_out_accepted;
	char *dnd_out_text;
	size_t dnd_out_len;
	bool dnd_out_is_uri; /* text/uri-list vs plain text */
	struct wlr_drag *dnd_out_drag; /* wlroots drag, if any */

	/* X11 → Wayland inbound drag */
	bool dnd_in_active;
	xcb_window_t dnd_in_source;
	int dnd_in_version;
	xcb_window_t dnd_in_our_window; /* which of our windows got XdndEnter */

	struct wl_event_source *sigint_source;
	struct wl_event_source *sigterm_source;
};

struct wlx_window {
	struct wlx_server *server;
	struct wlr_xdg_toplevel *toplevel;
	struct wlr_scene_tree *scene_tree;

	struct wlr_output *output;               /* NULL when unmapped */
	struct wlr_output_layout_output *l_output;
	struct wlr_scene_output *scene_output;
	int last_output_width;
	int last_output_height;
	/* CSD margin (buffer − window geometry) in logical px. Host window
	 * includes the margin; xdg configure size does not. */
	int csd_margin_w;
	int csd_margin_h;
	/* Once the host WM (or interactive resize) changes the X11 window
	 * size we stop auto-fitting the output to the client's geometry, so
	 * a user-enlarged window is not yanked back down on the next commit.
	 * Cleared when the client commits a size we did not request
	 * (client-driven resize). */
	bool size_from_wm;
	/* Last size we sent via wlr_xdg_toplevel_set_size (logical px). Used to
	 * distinguish client-driven resizes from "client acked our configure". */
	int last_client_conf_w;
	int last_client_conf_h;
	/* Skip scene Present after an output size change until the client
	 * commits a new buffer, so the previous frame stays on screen. */
	bool hold_present;

	xcb_window_t xwin;
	xcb_window_t related[WLX_MAX_RELATED_WINDOWS];
	int related_count;
	xcb_window_t content_xwin; /* the real wlroots-owned window, distinct
	                            * from xwin (the WM's decoration frame) */
	char last_title[256];
	char last_app_id[256];
	bool initial_configure_sent;
	struct wlr_xdg_toplevel_decoration_v1 *pending_decoration;
	/* When the host WM closes the X11 window, wlroots destroys the
	 * output. If the client is still mapped (e.g. gedit showing a
	 * "save changes?" dialog after xdg_toplevel.close), we schedule
	 * an idle recreate so the window stays visible. Cancelled on
	 * unmap/destroy. */
	struct wl_listener output_request_close;

	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener destroy;
	struct wl_listener commit;
	struct wl_listener request_move;
	struct wl_listener request_resize;
	struct wl_listener request_maximize;
	struct wl_listener request_fullscreen;
	struct wl_listener request_minimize;
	struct wl_listener new_popup;
	struct wl_listener new_subsurface;

	struct wl_listener output_frame;
	struct wl_listener output_destroy;
	struct wl_listener output_commit;
	struct wl_listener output_request_state;

	struct wl_list link;
};

/* xdg_popup. Painting may still be parent-scene (clipped) until Phase 3
 * switches to an OR present-window. When xpresent is non-NULL the popup
 * has its own X11 window and root_* caches its root-space box for hit-test. */
struct wlx_popup {
	struct wlx_server *server;
	struct wlx_window *parent;
	struct wlr_xdg_popup *xdg_popup;
	struct wl_list link; /* server->popups */
	struct wlr_scene_tree *scene_tree;

	/* Host X11 window for unclipped menus (NULL while parent-scene path). */
	struct wlr_x11_present_window *xpresent;
	/* Cached root-space geometry of the present-window (or of the
	 * parent-relative box when still parent-scene). Updated on map/
	 * configure/reposition. */
	int16_t root_x, root_y;
	int32_t root_w, root_h;
	bool root_box_valid;

	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener destroy;
	struct wl_listener commit;
	struct wl_listener xpresent_frame;
	struct wl_listener xpresent_destroy;
	struct wl_listener new_popup; /* nested popups */
};

struct wlx_keyboard {
	struct wlx_server *server;
	struct wlr_keyboard *wlr_keyboard;
	struct wl_listener modifiers;
	struct wl_listener key;
	struct wl_listener destroy;
};


/* x11_util.c */
xcb_atom_t intern_atom(xcb_connection_t *c, const char *name);
void query_root_children(xcb_connection_t *c, xcb_window_t root, xcb_window_t **out, int *n);
void xcb_roundtrip(xcb_connection_t *c);
void xwin_set_title(struct wlx_server *s, xcb_window_t w, const char *title);
void xwin_set_class(struct wlx_server *s, xcb_window_t w, const char *app_id);
void send_root_client_message(struct wlx_server *s, xcb_window_t window,
	xcb_atom_t type, uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3, uint32_t d4);
void send_net_wm_state(struct wlx_server *s, xcb_window_t w, uint32_t action,
	xcb_atom_t prop1, xcb_atom_t prop2);
xcb_window_t ewmh_target_window(struct wlx_window *win);
xcb_window_t configure_target_window(struct wlx_window *win);
xcb_window_t outer_position_window(struct wlx_window *win);
bool query_window_root_position(struct wlx_server *s, xcb_window_t w, int16_t *x, int16_t *y);
bool query_window_geometry(struct wlx_server *s, xcb_window_t w, int *width, int *height);
bool query_root_pointer_position(struct wlx_server *s, int16_t *x, int16_t *y);
/* Root-space origin of a toplevel's content window (wlroots-owned X id). */
bool window_content_root_position(struct wlx_window *win, int16_t *x, int16_t *y);
struct wlx_window *window_from_xwin(struct wlx_server *server, xcb_window_t w);
struct wlx_window *window_from_surface(struct wlr_surface *surface);
struct wlx_window *window_at_root_pointer(struct wlx_server *server);
struct wlx_popup *popup_at_root_pointer(struct wlx_server *server);
void wlx_dismiss_all_popups(struct wlx_server *server);
void wlx_popup_update_pointer_grab(struct wlx_server *server);

/* Overflowing wl_subsurface → present-window (GTK menubar). */
struct wlx_subpresent;
void wlx_window_sync_subpresents(struct wlx_window *win);
void wlx_window_destroy_subpresents(struct wlx_window *win);
void wlx_window_reposition_subpresents(struct wlx_window *win);
struct wlx_subpresent *subpresent_at_root_pointer(struct wlx_server *server);
struct wlr_surface *subpresent_surface_at(struct wlx_subpresent *sp,
		double *sx, double *sy);
struct wlx_window *subpresent_parent(struct wlx_subpresent *sp);
/* True if any mapped popup belongs to this toplevel. */
bool window_has_mapped_popup(struct wlx_server *server, struct wlx_window *win);
bool pointer_coords_on_window(struct wlx_server *server, struct wlx_window *win, double *sx, double *sy);
void select_window_events(struct wlx_server *server, xcb_window_t w);
void register_x11_window_subtree(struct wlx_window *win, xcb_window_t w);
xcb_window_t find_content_window(struct wlx_server *server, struct wlx_window *win);
void set_active_window(struct wlx_server *server, struct wlx_window *win);
void apply_transient_hints(struct wlx_window *win);
void xwin_set_transient_for(struct wlx_server *s, xcb_window_t w, xcb_window_t parent);
void xwin_set_window_type_dialog(struct wlx_server *s, xcb_window_t w, bool dialog);
void xwin_set_modal(struct wlx_server *s, xcb_window_t w, bool modal);
/* decorations=false → Motif decorations=0 (no host border/title). */
void xwin_set_motif_decorations(struct wlx_server *s, xcb_window_t w,
	bool decorations);
void xwin_set_size_hints(struct wlx_server *s, xcb_window_t w,
	int width, int height, int min_width, int min_height,
	int max_width, int max_height);
void win_sync_size_hints(struct wlx_window *win);
void toplevel_preferred_size(struct wlx_window *win, int *w_out, int *h_out);
/* set_size + record last_client_conf so surface_commit can detect client-driven resizes */
void wlx_toplevel_set_size(struct wlx_window *win, int width, int height);

/* move_resize.c */
void begin_interactive_move(struct wlx_window *win);
void begin_interactive_resize(struct wlx_window *win, uint32_t edges);
void update_interactive_drag(struct wlx_server *server);
bool drag_throttle_ready(struct wlx_server *server);

/* x11_events.c */
int handle_xcb_readable(int fd, uint32_t mask, void *data);

/* output.c */
void create_output_for_window(struct wlx_window *win);
/* Advertise a permanent wl_output before any client toplevel exists. */
bool create_bootstrap_output(struct wlx_server *server);
void resize_output_to(struct wlx_window *win, int w, int h);

/* xdg.c */
void server_new_xdg_toplevel(struct wl_listener *listener, void *data);
void server_new_xdg_popup(struct wl_listener *listener, void *data);
void server_new_toplevel_decoration(struct wl_listener *listener, void *data);

/* input.c */
void server_new_input(struct wl_listener *listener, void *data);
void server_cursor_motion(struct wl_listener *listener, void *data);
void server_cursor_motion_absolute(struct wl_listener *listener, void *data);
void server_cursor_button(struct wl_listener *listener, void *data);
void server_cursor_axis(struct wl_listener *listener, void *data);
void server_cursor_frame(struct wl_listener *listener, void *data);
void server_seat_request_cursor(struct wl_listener *listener, void *data);
struct wlr_surface *surface_at_cursor(struct wlx_server *server, double *sx, double *sy);
void process_cursor_motion(struct wlx_server *server, uint32_t time_msec);
struct wlr_surface *pointer_enter_surface_under_cursor(struct wlx_server *server);
void reset_cursor_to_default(struct wlx_server *server);

/* clipboard.c */
bool clipboard_init(struct wlx_server *server, xcb_screen_t *screen);
void clipboard_finish(struct wlx_server *server);
void clipboard_handle_selection_notify(struct wlx_server *server, xcb_selection_notify_event_t *ev);
void clipboard_handle_selection_request(struct wlx_server *server, xcb_selection_request_event_t *req);
void clipboard_handle_selection_clear(struct wlx_server *server, xcb_selection_clear_event_t *ev);
void clipboard_request_from_x11(struct wlx_server *server, bool primary);
void clipboard_offer_x11_text_to_wayland(struct wlx_server *server, char *text, size_t len);
void server_request_set_selection(struct wl_listener *listener, void *data);
void server_request_set_primary_selection(struct wl_listener *listener, void *data);

/* dnd.c */
void dnd_atoms_init(struct wlx_server *server);
void dnd_set_xdnd_aware(struct wlx_server *server, xcb_window_t w);
bool dnd_handle_selection_request(struct wlx_server *server, xcb_selection_request_event_t *req);
void dnd_handle_selection_notify(struct wlx_server *server, xcb_selection_notify_event_t *ev);
void dnd_handle_client_message(struct wlx_server *server, xcb_client_message_event_t *ev);
void dnd_out_update_position(struct wlx_server *server);
void dnd_out_on_button_release(struct wlx_server *server);
void server_request_start_drag(struct wl_listener *listener, void *data);
void server_start_drag(struct wl_listener *listener, void *data);


void toplevel_request_move(struct wl_listener *listener, void *data);
void toplevel_request_resize(struct wl_listener *listener, void *data);
void toplevel_request_maximize(struct wl_listener *listener, void *data);
void toplevel_request_fullscreen(struct wl_listener *listener, void *data);
void toplevel_request_minimize(struct wl_listener *listener, void *data);
void output_request_state(struct wl_listener *listener, void *data);
void output_frame(struct wl_listener *listener, void *data);
void output_commit(struct wl_listener *listener, void *data);
void output_destroy(struct wl_listener *listener, void *data);

int wlx_scale_size(struct wlx_server *server, int logical);
int wlx_unscale_size(struct wlx_server *server, int output_px);
void wlx_apply_content_scale(struct wlx_window *win);
void wlx_apply_popup_content_scale(struct wlx_popup *pop);
void wlx_pointer_to_surface(struct wlx_server *server, double *sx, double *sy);
/* Re-resolve pointer focus from the real X11 pointer (after popup map/unmap). */
void wlx_pointer_refresh_focus(struct wlx_server *server);
/* Re-place mapped popups after their parent X11 window moved. */
void wlx_reposition_popups_for_window(struct wlx_window *win);

#endif /* WLX_SERVER_H */

