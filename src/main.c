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


/* ------------------------------------------------------------------- */
/* Signal handling (clean shutdown)                                     */
/* ------------------------------------------------------------------- */

static struct wl_display *g_display_for_signal;

int handle_terminate_signal(int signal_number, void *data) {
	(void)signal_number;
	struct wl_display *display = data;
	wl_display_terminate(display);
	return 0;
}

int handle_sigchld(int signal_number, void *data) {
	(void)signal_number;
	(void)data;
	int status;
	pid_t pid;
	while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
		wlr_log(WLR_INFO, "reaped child process %d", pid);
	}
	return 0;
}

/* ------------------------------------------------------------------- */
/* `wl-x11 <command>`: launch a client alongside the compositor, and    */
/* shut down once no Wayland clients remain connected                  */
/* ------------------------------------------------------------------- */

struct wlx_client_track {
	struct wlx_server *server;
	struct wl_listener destroy;
};

void client_destroy_notify(struct wl_listener *listener, void *data) {
	(void)data;
	struct wlx_client_track *track = wl_container_of(listener, track, destroy);
	struct wlx_server *server = track->server;

	server->active_clients--;
	wlr_log(WLR_INFO, "wayland client disconnected (%d remaining)",
		server->active_clients);

	if (server->exit_when_clients_gone && server->have_seen_client &&
			server->active_clients <= 0) {
		wlr_log(WLR_INFO,
			"no more wayland clients connected, shutting down");
		wl_display_terminate(server->wl_display);
	}

	wl_list_remove(&track->destroy.link);
	free(track);
}

void client_created_notify(struct wl_listener *listener, void *data) {
	struct wlx_server *server = wl_container_of(listener, server, client_created);
	struct wl_client *client = data;

	server->active_clients++;
	server->have_seen_client = true;
	wlr_log(WLR_INFO, "wayland client connected (%d active)",
		server->active_clients);

	struct wlx_client_track *track = calloc(1, sizeof(*track));
	if (!track) {
		wlr_log(WLR_ERROR, "out of memory allocating client tracker");
		return;
	}
	track->server = server;
	track->destroy.notify = client_destroy_notify;
	wl_client_add_destroy_listener(client, &track->destroy);
}

/* ------------------------------------------------------------------- */
/* main                                                                  */
/* ------------------------------------------------------------------- */

int main(int argc, char **argv) {
	bool debug = false;
	/* Default is SSD (host WM decorations). --csd / --ssd set this
	 * explicitly; last one wins if both appear. */
	bool prefer_csd = false;
	bool decoration_flag_set = false;
	double content_scale = 1.0;
	char **command_argv = NULL;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--debug") == 0) {
			debug = true;
		} else if (strcmp(argv[i], "--csd") == 0) {
			prefer_csd = true;
			decoration_flag_set = true;
		} else if (strcmp(argv[i], "--ssd") == 0) {
			prefer_csd = false;
			decoration_flag_set = true;
		} else if (strcmp(argv[i], "--scale") == 0) {
			if (i + 1 >= argc) {
				fprintf(stderr, "wl-x11: --scale requires a number\n");
				return 1;
			}
			char *end = NULL;
			content_scale = strtod(argv[++i], &end);
			if (end == argv[i] || *end != '\0' || content_scale <= 0.0) {
				fprintf(stderr, "wl-x11: invalid --scale value '%s'\n", argv[i]);
				return 1;
			}
		} else if (strncmp(argv[i], "--scale=", 8) == 0) {
			char *end = NULL;
			content_scale = strtod(argv[i] + 8, &end);
			if (end == argv[i] + 8 || *end != '\0' || content_scale <= 0.0) {
				fprintf(stderr, "wl-x11: invalid --scale value '%s'\n", argv[i] + 8);
				return 1;
			}
		} else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
			printf("usage: wl-x11 [--debug] [--ssd|--csd] [--scale FACTOR] "
				"[command [args...]]\n"
				"  --debug             print verbose diagnostic logging "
				"(default: only errors)\n"
				"  --ssd               server-side decorations via the host "
				"WM (default):\n"
				"                      clients are asked not to draw CSD/"
				"shadows\n"
				"  --csd               client-side decorations: no host WM "
				"border/title;\n"
				"                      Wayland clients draw their own chrome\n"
				"  --scale FACTOR      brute-force scale all content pixels "
				"(e.g. 2, 0.5; default 1)\n"
				"  command [args...]   also launch this program with "
				"WAYLAND_DISPLAY set,\n"
				"                      and shut down once no Wayland "
				"clients remain connected\n");
			return 0;
		} else {
			/* First non-flag argument: everything from here on is the
			 * command to launch, not further wl-x11 flags. */
			command_argv = &argv[i];
			break;
		}
	}

	/* Diagnostic wlr_log(WLR_INFO, ...) calls are gated by this: at the
	 * default WLR_ERROR level they simply won't print, leaving only
	 * genuine errors (and whatever wlroots itself logs at WLR_ERROR) on
	 * stderr. --debug raises it back to WLR_INFO to see everything. */
	wlr_log_init(debug ? WLR_INFO : WLR_ERROR, NULL);

	const char *x11_display = getenv("DISPLAY");
	if (!x11_display) {
		fprintf(stderr,
			"wl-x11: $DISPLAY is not set. This compositor must be run "
			"inside an existing X11 session (e.g. from an xterm on your "
			"desktop, or via `xinit`).\n");
		return 1;
	}

	struct wlx_server server = {0};
	server.content_scale = content_scale;
	server.prefer_csd = prefer_csd;
	if (server.content_scale != 1.0) {
		wlr_log(WLR_INFO, "content scale factor %.3f (brute-force pixel scale)",
			server.content_scale);
		fprintf(stderr, "wl-x11: content scale %.3f\n", server.content_scale);
	}
	if (server.prefer_csd) {
		wlr_log(WLR_INFO, "prefer client-side decorations (--csd)");
		fprintf(stderr, "wl-x11: client-side decorations (no host WM border)\n");
	} else if (decoration_flag_set) {
		wlr_log(WLR_INFO, "prefer server-side decorations (--ssd)");
		fprintf(stderr, "wl-x11: server-side decorations (host WM frame)\n");
	}
	server.clip_read_fd = -1;
	server.wl_display = wl_display_create();
	struct wl_event_loop *loop = wl_display_get_event_loop(server.wl_display);
	server.loop = loop;

	server.backend = wlr_x11_backend_create(loop, x11_display);
	if (!server.backend) {
		fprintf(stderr, "wl-x11: failed to create X11 backend for DISPLAY=%s\n",
			x11_display);
		return 1;
	}

	server.renderer = wlr_renderer_autocreate(server.backend);
	if (!server.renderer) {
		fprintf(stderr, "wl-x11: failed to create renderer\n");
		return 1;
	}
	wlr_renderer_init_wl_display(server.renderer, server.wl_display);

	server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
	if (!server.allocator) {
		fprintf(stderr, "wl-x11: failed to create allocator\n");
		return 1;
	}

	wlr_compositor_create(server.wl_display, 5, server.renderer);
	wlr_subcompositor_create(server.wl_display);
	wlr_data_device_manager_create(server.wl_display);
	/* Optional Wayland protocol (zwp_primary_selection_v1): middle-click
	 * paste. Many clients still implement it; pure Wayland desktops may
	 * omit it, but X11 users expect PRIMARY. */
	wlr_primary_selection_v1_device_manager_create(server.wl_display);
	/* GTK4/libadwaita expect these; without them menus/sizing can misbehave. */
	wlr_viewporter_create(server.wl_display);
	wlr_fractional_scale_manager_v1_create(server.wl_display, 1);
	wlr_single_pixel_buffer_manager_v1_create(server.wl_display);
	wlr_presentation_create(server.wl_display, server.backend, 2);

	server.output_layout = wlr_output_layout_create(server.wl_display);
	server.scene = wlr_scene_create();
	server.scene_layout =
		wlr_scene_attach_output_layout(server.scene, server.output_layout);

	wl_list_init(&server.windows);
	wl_list_init(&server.popups);

	/* Version ≥ 5 for xdg_toplevel.wm_capabilities (GTK/libadwaita CSD). */
	server.xdg_shell = wlr_xdg_shell_create(server.wl_display, 5);
	server.new_xdg_toplevel.notify = server_new_xdg_toplevel;
	wl_signal_add(&server.xdg_shell->events.new_toplevel, &server.new_xdg_toplevel);
	server.new_xdg_popup.notify = server_new_xdg_popup;
	wl_signal_add(&server.xdg_shell->events.new_popup, &server.new_xdg_popup);

	/* xdg-decoration-unstable-v1 (v2). Default path forces SERVER_SIDE so
	 * clients must not paint CSD/shadows; --csd requests CLIENT_SIDE. */
	server.xdg_decoration_manager =
		wlr_xdg_decoration_manager_v1_create(server.wl_display, 2);
	server.new_toplevel_decoration.notify = server_new_toplevel_decoration;
	wl_signal_add(&server.xdg_decoration_manager->events.new_toplevel_decoration,
		&server.new_toplevel_decoration);

	/* KDE server-decoration: GTK3 (and some GTK4) still prefer this over
	 * xdg-decoration. Advertise a default mode matching --csd / SSD. */
	server.server_decoration_manager =
		wlr_server_decoration_manager_create(server.wl_display);
	if (server.server_decoration_manager) {
		wlr_server_decoration_manager_set_default_mode(
			server.server_decoration_manager,
			server.prefer_csd
				? WLR_SERVER_DECORATION_MANAGER_MODE_CLIENT
				: WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);
		wlr_log(WLR_INFO, "org_kde_kwin_server_decoration default=%s",
			server.prefer_csd ? "client" : "server");
	}

	server.seat = wlr_seat_create(server.wl_display, "seat0");
	server.cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(server.cursor, server.output_layout);

	/* Theme cursors for the default arrow; client buffers override this
	 * via request_set_cursor → wlr_cursor_set_surface (X11 backend turns
	 * that into a real X cursor on the output window). */
	server.cursor_mgr = wlr_xcursor_manager_create(NULL, 24);
	if (server.cursor_mgr) {
		wlr_xcursor_manager_load(server.cursor_mgr, 1);
		wlr_cursor_set_xcursor(server.cursor, server.cursor_mgr, "left_ptr");
	} else {
		wlr_log(WLR_ERROR, "failed to create xcursor manager; pointer may stay invisible until a client sets one");
	}

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

	server.request_set_cursor.notify = server_seat_request_cursor;
	wl_signal_add(&server.seat->events.request_set_cursor,
		&server.request_set_cursor);
	server.request_set_selection.notify = server_request_set_selection;
	wl_signal_add(&server.seat->events.request_set_selection,
		&server.request_set_selection);
	server.request_set_primary_selection.notify =
		server_request_set_primary_selection;
	wl_signal_add(&server.seat->events.request_set_primary_selection,
		&server.request_set_primary_selection);
	server.request_start_drag.notify = server_request_start_drag;
	wl_signal_add(&server.seat->events.request_start_drag,
		&server.request_start_drag);
	server.start_drag.notify = server_start_drag;
	wl_signal_add(&server.seat->events.start_drag, &server.start_drag);

	/* Auxiliary XCB connection, used only for setting WM_NAME/WM_CLASS. */
	server.xcb = xcb_connect(x11_display, NULL);
	if (!server.xcb || xcb_connection_has_error(server.xcb)) {
		wlr_log(WLR_ERROR,
			"failed to open auxiliary X11 connection; window titles/classes "
			"will not be synced");
	} else {
		const xcb_setup_t *setup = xcb_get_setup(server.xcb);
		xcb_screen_t *screen = xcb_setup_roots_iterator(setup).data;
		server.xcb_root = screen->root;
		server.atom_net_wm_name = intern_atom(server.xcb, "_NET_WM_NAME");
		server.atom_utf8_string = intern_atom(server.xcb, "UTF8_STRING");
		server.atom_net_wm_moveresize = intern_atom(server.xcb, "_NET_WM_MOVERESIZE");
		server.atom_net_wm_state = intern_atom(server.xcb, "_NET_WM_STATE");
		server.atom_net_wm_state_maximized_vert =
			intern_atom(server.xcb, "_NET_WM_STATE_MAXIMIZED_VERT");
		server.atom_net_wm_state_maximized_horz =
			intern_atom(server.xcb, "_NET_WM_STATE_MAXIMIZED_HORZ");
		server.atom_net_wm_state_fullscreen =
			intern_atom(server.xcb, "_NET_WM_STATE_FULLSCREEN");
		server.atom_net_wm_state_modal =
			intern_atom(server.xcb, "_NET_WM_STATE_MODAL");
		server.atom_wm_change_state = intern_atom(server.xcb, "WM_CHANGE_STATE");
		server.atom_wm_transient_for = intern_atom(server.xcb, "WM_TRANSIENT_FOR");
		server.atom_wm_normal_hints = intern_atom(server.xcb, "WM_NORMAL_HINTS");
		server.atom_wm_size_hints = intern_atom(server.xcb, "WM_SIZE_HINTS");
		server.atom_motif_wm_hints = intern_atom(server.xcb, "_MOTIF_WM_HINTS");
		server.atom_net_wm_window_type =
			intern_atom(server.xcb, "_NET_WM_WINDOW_TYPE");
		server.atom_net_wm_window_type_normal =
			intern_atom(server.xcb, "_NET_WM_WINDOW_TYPE_NORMAL");
		server.atom_net_wm_window_type_dialog =
			intern_atom(server.xcb, "_NET_WM_WINDOW_TYPE_DIALOG");

		wlr_log(WLR_INFO,
			"resolved atoms: _NET_WM_NAME=%u UTF8_STRING=%u "
			"_NET_WM_MOVERESIZE=%u _NET_WM_STATE=%u "
			"_NET_WM_STATE_MAXIMIZED_VERT=%u _NET_WM_STATE_MAXIMIZED_HORZ=%u "
			"_NET_WM_STATE_FULLSCREEN=%u WM_CHANGE_STATE=%u (0 = FAILED)",
			server.atom_net_wm_name, server.atom_utf8_string,
			server.atom_net_wm_moveresize, server.atom_net_wm_state,
			server.atom_net_wm_state_maximized_vert,
			server.atom_net_wm_state_maximized_horz,
			server.atom_net_wm_state_fullscreen, server.atom_wm_change_state);

		clipboard_init(&server, screen);
		dnd_atoms_init(&server);
		dnd_set_xdnd_aware(&server, server.clipboard_window);

		wl_event_loop_add_fd(loop, xcb_get_file_descriptor(server.xcb),
			WL_EVENT_READABLE, handle_xcb_readable, &server);
	}

	const char *socket = wl_display_add_socket_auto(server.wl_display);
	if (!socket) {
		fprintf(stderr, "wl-x11: failed to create Wayland socket\n");
		return 1;
	}

	if (!wlr_backend_start(server.backend)) {
		fprintf(stderr, "wl-x11: failed to start X11 backend\n");
		wl_display_destroy(server.wl_display);
		return 1;
	}

	/* Must exist before any client connects: foot and others refuse to
	 * start with zero wl_outputs. Per-toplevel outputs only appear on map. */
	if (!create_bootstrap_output(&server)) {
		fprintf(stderr, "wl-x11: warning: no bootstrap monitor; some clients "
			"(e.g. foot) may fail to start\n");
	}

	setenv("WAYLAND_DISPLAY", socket, true);

	server.client_created.notify = client_created_notify;
	wl_display_add_client_created_listener(server.wl_display, &server.client_created);

	g_display_for_signal = server.wl_display;
	server.sigint_source = wl_event_loop_add_signal(loop, SIGINT,
		handle_terminate_signal, server.wl_display);
	server.sigterm_source = wl_event_loop_add_signal(loop, SIGTERM,
		handle_terminate_signal, server.wl_display);
	wl_event_loop_add_signal(loop, SIGCHLD, handle_sigchld, NULL);

	server.launched_pid = -1;
	if (command_argv) {
		server.exit_when_clients_gone = true;
		pid_t pid = fork();
		if (pid == 0) {
			/* Child: WAYLAND_DISPLAY was set via setenv() above, so it's
			 * already inherited through environ; just exec. */
			execvp(command_argv[0], command_argv);
			fprintf(stderr, "wl-x11: failed to exec '%s': %s\n",
				command_argv[0], strerror(errno));
			_exit(127);
		} else if (pid < 0) {
			fprintf(stderr, "wl-x11: fork() failed: %s\n", strerror(errno));
			server.exit_when_clients_gone = false;
		} else {
			server.launched_pid = pid;
			wlr_log(WLR_INFO, "launched '%s' (pid %d) with WAYLAND_DISPLAY=%s",
				command_argv[0], pid, socket);
		}
	}

	fprintf(stderr,
		"wl-x11: running. WAYLAND_DISPLAY=%s (nested inside X11 DISPLAY=%s)\n",
		socket, x11_display);
	if (command_argv) {
		fprintf(stderr,
			"wl-x11: launched '%s'; will exit once no Wayland clients "
			"remain connected\n", command_argv[0]);
	} else {
		fprintf(stderr,
			"wl-x11: start clients with, e.g.:\n"
			"wl-x11:   WAYLAND_DISPLAY=%s weston-terminal\n"
			"wl-x11:   WAYLAND_DISPLAY=%s foot\n",
			socket, socket);
	}

	wl_display_run(server.wl_display);

	if (server.launched_pid > 0) {
		/* Best-effort: don't leave the launched app running (or its
		 * children orphaned) after the compositor exits. */
		kill(server.launched_pid, SIGTERM);
	}

	/* Tear down in the order wlroots expects: clients first (so
	 * per-window listeners are removed via xdg_toplevel_destroy), then
	 * our listeners on globals/backend/cursor/seat (those objects assert
	 * empty listener lists on destroy), then scene/backend, then display. */
	wl_display_destroy_clients(server.wl_display);

	wl_list_remove(&server.new_xdg_toplevel.link);
	wl_list_remove(&server.new_xdg_popup.link);
	wl_list_remove(&server.new_toplevel_decoration.link);
	wl_list_remove(&server.client_created.link);
	wl_list_remove(&server.new_input.link);
	wl_list_remove(&server.cursor_motion.link);
	wl_list_remove(&server.cursor_motion_absolute.link);
	wl_list_remove(&server.cursor_button.link);
	wl_list_remove(&server.cursor_axis.link);
	wl_list_remove(&server.cursor_frame.link);
	wl_list_remove(&server.request_set_cursor.link);
	wl_list_remove(&server.request_set_selection.link);
	wl_list_remove(&server.request_set_primary_selection.link);
	wl_list_remove(&server.request_start_drag.link);
	wl_list_remove(&server.start_drag.link);

	if (server.sigint_source) {
		wl_event_source_remove(server.sigint_source);
		server.sigint_source = NULL;
	}
	if (server.sigterm_source) {
		wl_event_source_remove(server.sigterm_source);
		server.sigterm_source = NULL;
	}
	if (server.deferred_release_source) {
		wl_event_source_remove(server.deferred_release_source);
		server.deferred_release_source = NULL;
	}

	wlr_scene_node_destroy(&server.scene->tree.node);
	wlr_cursor_destroy(server.cursor);
	server.cursor = NULL;
	if (server.cursor_mgr) {
		wlr_xcursor_manager_destroy(server.cursor_mgr);
		server.cursor_mgr = NULL;
	}

	if (server.xcb) {
		clipboard_finish(&server);
		xcb_disconnect(server.xcb);
		server.xcb = NULL;
	}

	wlr_allocator_destroy(server.allocator);
	server.allocator = NULL;
	wlr_renderer_destroy(server.renderer);
	server.renderer = NULL;
	wlr_backend_destroy(server.backend);
	server.backend = NULL;

	wl_display_destroy(server.wl_display);
	return 0;
}
