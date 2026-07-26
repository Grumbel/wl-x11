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
/* Drag-and-drop: Wayland seat + text/uri XDND bridge to host X11       */
/* ------------------------------------------------------------------- */

void dnd_set_xdnd_aware(struct wlx_server *server, xcb_window_t w) {
	if (!server->xcb || w == XCB_WINDOW_NONE ||
			server->atom_xdnd_aware == XCB_ATOM_NONE ||
			server->clipboard_window == XCB_WINDOW_NONE) {
		return;
	}
	uint32_t version = 5;
	xcb_change_property(server->xcb, XCB_PROP_MODE_REPLACE, w,
		server->atom_xdnd_aware, XCB_ATOM_ATOM, 32, 1, &version);
	/* Proxy XDND messages to our clipboard helper window: the real X
	 * windows are owned by the wlroots X11 backend connection, so
	 * ClientMessages would not reach our auxiliary XCB fd without this. */
	xcb_atom_t proxy = intern_atom(server->xcb, "XdndProxy");
	uint32_t proxy_win = server->clipboard_window;
	xcb_change_property(server->xcb, XCB_PROP_MODE_REPLACE, w,
		proxy, XCB_ATOM_WINDOW, 32, 1, &proxy_win);
	/* Also mark the proxy itself XdndAware. */
	xcb_change_property(server->xcb, XCB_PROP_MODE_REPLACE,
		server->clipboard_window, server->atom_xdnd_aware,
		XCB_ATOM_ATOM, 32, 1, &version);
	xcb_flush(server->xcb);
}

bool dnd_window_is_ours(struct wlx_server *server, xcb_window_t w) {
	if (w == XCB_WINDOW_NONE || w == server->clipboard_window) {
		return true;
	}
	struct wlx_window *win;
	wl_list_for_each(win, &server->windows, link) {
		if (w == win->xwin || w == win->content_xwin) {
			return true;
		}
		for (int i = 0; i < win->related_count; i++) {
			if (win->related[i] == w) {
				return true;
			}
		}
	}
	return false;
}

int dnd_get_aware_version(struct wlx_server *server, xcb_window_t w) {
	if (!server->xcb || w == XCB_WINDOW_NONE) {
		return -1;
	}
	xcb_get_property_cookie_t cookie = xcb_get_property(server->xcb, 0, w,
		server->atom_xdnd_aware, XCB_ATOM_ATOM, 0, 1);
	xcb_get_property_reply_t *reply =
		xcb_get_property_reply(server->xcb, cookie, NULL);
	if (!reply || reply->type == XCB_ATOM_NONE || reply->value_len < 1) {
		free(reply);
		return -1;
	}
	uint32_t *vals = xcb_get_property_value(reply);
	int ver = (int)vals[0];
	free(reply);
	return ver;
}

/* Walk from the window under the pointer up toward root; return the
 * first XdndAware ancestor (or the window itself). */
xcb_window_t dnd_find_aware_target(struct wlx_server *server,
		int16_t root_x, int16_t root_y, int *version_out) {
	xcb_query_pointer_cookie_t pq =
		xcb_query_pointer(server->xcb, server->xcb_root);
	xcb_query_pointer_reply_t *pr =
		xcb_query_pointer_reply(server->xcb, pq, NULL);
	if (!pr) {
		return XCB_WINDOW_NONE;
	}
	xcb_window_t child = pr->child;
	free(pr);
	if (child == XCB_WINDOW_NONE) {
		return XCB_WINDOW_NONE;
	}

	/* Descend into the deepest child under the pointer for a better
	 * starting point, then walk back up looking for XdndAware. */
	xcb_window_t cur = child;
	for (int depth = 0; depth < 16; depth++) {
		xcb_translate_coordinates_cookie_t tc =
			xcb_translate_coordinates(server->xcb, server->xcb_root, cur,
				root_x, root_y);
		xcb_translate_coordinates_reply_t *tr =
			xcb_translate_coordinates_reply(server->xcb, tc, NULL);
		if (!tr || tr->child == XCB_WINDOW_NONE) {
			free(tr);
			break;
		}
		cur = tr->child;
		free(tr);
	}

	while (cur != XCB_WINDOW_NONE && cur != server->xcb_root) {
		if (dnd_window_is_ours(server, cur)) {
			/* Never XDND to ourselves — Wayland handles that path. */
			return XCB_WINDOW_NONE;
		}
		int ver = dnd_get_aware_version(server, cur);
		if (ver >= 0) {
			if (version_out) {
				*version_out = ver > 5 ? 5 : ver;
			}
			return cur;
		}
		xcb_query_tree_cookie_t cookie = xcb_query_tree(server->xcb, cur);
		xcb_query_tree_reply_t *reply =
			xcb_query_tree_reply(server->xcb, cookie, NULL);
		if (!reply) {
			break;
		}
		xcb_window_t parent = reply->parent;
		free(reply);
		cur = parent;
	}
	return XCB_WINDOW_NONE;
}

void dnd_send_client_message(struct wlx_server *server,
		xcb_window_t target, xcb_atom_t type,
		uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3, uint32_t d4) {
	xcb_client_message_event_t ev = {0};
	ev.response_type = XCB_CLIENT_MESSAGE;
	ev.window = target;
	ev.type = type;
	ev.format = 32;
	ev.data.data32[0] = d0;
	ev.data.data32[1] = d1;
	ev.data.data32[2] = d2;
	ev.data.data32[3] = d3;
	ev.data.data32[4] = d4;
	xcb_send_event(server->xcb, 0, target, XCB_EVENT_MASK_NO_EVENT,
		(const char *)&ev);
	xcb_flush(server->xcb);
}

void dnd_out_leave(struct wlx_server *server) {
	if (server->dnd_out_target != XCB_WINDOW_NONE) {
		dnd_send_client_message(server, server->dnd_out_target,
			server->atom_xdnd_leave, server->clipboard_window, 0, 0, 0, 0);
		server->dnd_out_target = XCB_WINDOW_NONE;
		server->dnd_out_accepted = false;
	}
}

void dnd_out_finish(struct wlx_server *server) {
	dnd_out_leave(server);
	server->dnd_out_active = false;
	server->dnd_out_drag = NULL;
	free(server->dnd_out_text);
	server->dnd_out_text = NULL;
	server->dnd_out_len = 0;
	server->dnd_out_is_uri = false;
	if (server->xcb && server->clipboard_window != XCB_WINDOW_NONE) {
		/* Release XdndSelection if we held it. */
		xcb_get_selection_owner_cookie_t cookie =
			xcb_get_selection_owner(server->xcb, server->atom_xdnd_selection);
		xcb_get_selection_owner_reply_t *reply =
			xcb_get_selection_owner_reply(server->xcb, cookie, NULL);
		if (reply && reply->owner == server->clipboard_window) {
			xcb_set_selection_owner(server->xcb, XCB_WINDOW_NONE,
				server->atom_xdnd_selection, XCB_CURRENT_TIME);
			xcb_flush(server->xcb);
		}
		free(reply);
	}
}

void dnd_out_update_position(struct wlx_server *server) {
	if (!server->dnd_out_active || !server->xcb || !server->dnd_out_text) {
		return;
	}
	int16_t px, py;
	if (!query_root_pointer_position(server, &px, &py)) {
		return;
	}

	/* Prefer Wayland surface under cursor: seat already handles that. */
	double sx, sy;
	if (surface_at_cursor(server, &sx, &sy)) {
		dnd_out_leave(server);
		return;
	}

	int version = 5;
	xcb_window_t target = dnd_find_aware_target(server, px, py, &version);
	if (target == XCB_WINDOW_NONE) {
		dnd_out_leave(server);
		return;
	}

	if (target != server->dnd_out_target) {
		dnd_out_leave(server);
		server->dnd_out_target = target;
		server->dnd_out_version = version;
		server->dnd_out_accepted = false;
		/* XdndEnter: data[1] = version << 24; types in [2..4] */
		xcb_atom_t typ = server->dnd_out_is_uri
			? server->atom_text_uri_list : server->atom_utf8_string;
		dnd_send_client_message(server, target, server->atom_xdnd_enter,
			server->clipboard_window, ((uint32_t)version) << 24,
			typ, XCB_ATOM_NONE, XCB_ATOM_NONE);
	}

	uint32_t pos = ((uint32_t)(uint16_t)px << 16) | (uint32_t)(uint16_t)py;
	dnd_send_client_message(server, target, server->atom_xdnd_position,
		server->clipboard_window, 0, pos, XCB_CURRENT_TIME,
		server->atom_xdnd_action_copy);
}

void dnd_out_on_button_release(struct wlx_server *server) {
	if (!server->dnd_out_active) {
		return;
	}
	if (server->dnd_out_target != XCB_WINDOW_NONE && server->dnd_out_accepted) {
		dnd_send_client_message(server, server->dnd_out_target,
			server->atom_xdnd_drop, server->clipboard_window, 0,
			XCB_CURRENT_TIME, 0, 0);
		/* Keep selection data until XdndFinished. */
	} else {
		dnd_out_finish(server);
	}
}

/* Serve XdndSelection / CLIPBOARD-style requests during an outbound drag. */
bool dnd_handle_selection_request(struct wlx_server *server,
		xcb_selection_request_event_t *req) {
	if (req->selection != server->atom_xdnd_selection ||
			!server->dnd_out_active || !server->dnd_out_text) {
		return false;
	}
	xcb_selection_notify_event_t notify = {
		.response_type = XCB_SELECTION_NOTIFY,
		.time = req->time,
		.requestor = req->requestor,
		.selection = req->selection,
		.target = req->target,
		.property = XCB_ATOM_NONE,
	};
	if (req->target == server->atom_targets) {
		xcb_atom_t targets[] = {
			server->atom_targets,
			server->atom_utf8_string,
			server->atom_text_uri_list,
			server->atom_string,
			server->atom_text,
		};
		xcb_change_property(server->xcb, XCB_PROP_MODE_REPLACE, req->requestor,
			req->property, XCB_ATOM_ATOM, 32,
			sizeof(targets) / sizeof(targets[0]), targets);
		notify.property = req->property;
	} else if (req->target == server->atom_utf8_string ||
			req->target == server->atom_text_uri_list ||
			req->target == server->atom_string ||
			req->target == server->atom_text) {
		xcb_change_property(server->xcb, XCB_PROP_MODE_REPLACE, req->requestor,
			req->property, req->target, 8,
			(uint32_t)server->dnd_out_len, server->dnd_out_text);
		notify.property = req->property;
	}
	xcb_send_event(server->xcb, 0, req->requestor, XCB_EVENT_MASK_NO_EVENT,
		(const char *)&notify);
	xcb_flush(server->xcb);
	return true;
}

void dnd_handle_client_message(struct wlx_server *server,
		xcb_client_message_event_t *ev) {
	if (ev->format != 32) {
		return;
	}
	xcb_atom_t type = ev->type;

	/* --- outbound status / finished from target --- */
	if (type == server->atom_xdnd_status && server->dnd_out_active) {
		/* data[0]=target window, data[1] bit0 = accept */
		server->dnd_out_accepted = (ev->data.data32[1] & 1) != 0;
		return;
	}
	if (type == server->atom_xdnd_finished && server->dnd_out_active) {
		dnd_out_finish(server);
		return;
	}

	/* --- inbound from X11 source --- */
	if (type == server->atom_xdnd_enter) {
		server->dnd_in_active = true;
		server->dnd_in_source = ev->data.data32[0];
		server->dnd_in_version = (int)((ev->data.data32[1] >> 24) & 0xff);
		if (server->dnd_in_version > 5) {
			server->dnd_in_version = 5;
		}
		server->dnd_in_our_window = ev->window;
		wlr_log(WLR_INFO, "XDND: enter from 0x%x onto 0x%x (ver %d)",
			server->dnd_in_source, server->dnd_in_our_window,
			server->dnd_in_version);
		return;
	}
	if (type == server->atom_xdnd_position && server->dnd_in_active) {
		/* Always accept copy for text; reply XdndStatus. */
		uint32_t flags = 1; /* accept */
		dnd_send_client_message(server, server->dnd_in_source,
			server->atom_xdnd_status, ev->window, flags, 0, 0,
			server->atom_xdnd_action_copy);
		return;
	}
	if (type == server->atom_xdnd_leave) {
		server->dnd_in_active = false;
		server->dnd_in_source = XCB_WINDOW_NONE;
		return;
	}
	if (type == server->atom_xdnd_drop && server->dnd_in_active) {
		/* Request text (prefer uri-list then utf8). */
		xcb_atom_t target = server->atom_utf8_string;
		xcb_delete_property(server->xcb, server->clipboard_window,
			server->atom_wlx_dnd);
		xcb_convert_selection(server->xcb, server->clipboard_window,
			server->atom_xdnd_selection, target, server->atom_wlx_dnd,
			XCB_CURRENT_TIME);
		xcb_flush(server->xcb);
		wlr_log(WLR_INFO, "XDND: drop — converting selection to text");
		return;
	}
}

void dnd_handle_selection_notify(struct wlx_server *server,
		xcb_selection_notify_event_t *ev) {
	if (ev->property == XCB_ATOM_NONE ||
			ev->requestor != server->clipboard_window ||
			ev->selection != server->atom_xdnd_selection) {
		return;
	}
	xcb_get_property_cookie_t cookie = xcb_get_property(server->xcb, 1,
		server->clipboard_window, server->atom_wlx_dnd,
		XCB_GET_PROPERTY_TYPE_ANY, 0, WLX_CLIPBOARD_MAX / 4);
	xcb_get_property_reply_t *reply =
		xcb_get_property_reply(server->xcb, cookie, NULL);
	if (!reply || reply->type == XCB_ATOM_NONE ||
			xcb_get_property_value_length(reply) <= 0) {
		free(reply);
		if (server->dnd_in_source != XCB_WINDOW_NONE) {
			dnd_send_client_message(server, server->dnd_in_source,
				server->atom_xdnd_finished, server->dnd_in_our_window,
				0, 0, 0, 0);
		}
		server->dnd_in_active = false;
		return;
	}
	int len = xcb_get_property_value_length(reply);
	char *text = malloc((size_t)len + 1);
	if (text) {
		memcpy(text, xcb_get_property_value(reply), (size_t)len);
		text[len] = '\0';
		/* Offer as Wayland selection so the user can paste; full surface
		 * drop injection needs a grab serial we don't have from X11. */
		clipboard_offer_x11_text_to_wayland(server, text, (size_t)len);
		wlr_log(WLR_INFO, "XDND: imported %d bytes into Wayland selection", len);
	}
	free(reply);
	if (server->dnd_in_source != XCB_WINDOW_NONE) {
		dnd_send_client_message(server, server->dnd_in_source,
			server->atom_xdnd_finished, server->dnd_in_our_window,
			1, server->atom_xdnd_action_copy, 0, 0);
	}
	server->dnd_in_active = false;
	server->dnd_in_source = XCB_WINDOW_NONE;
}

/* Prefetch text from a Wayland drag source for XDND export. */
void dnd_out_begin_from_source(struct wlx_server *server,
		struct wlr_data_source *source) {
	if (!source || !server->xcb) {
		return;
	}
	const char *uri_mime = NULL;
	const char *text_mime = NULL;
	char **p;
	wl_array_for_each(p, &source->mime_types) {
		if (strcmp(*p, "text/uri-list") == 0) {
			uri_mime = *p;
		} else if (!text_mime && (strcmp(*p, "text/plain;charset=utf-8") == 0 ||
				strcmp(*p, "text/plain") == 0 ||
				strncmp(*p, "text/", 5) == 0)) {
			text_mime = *p;
		}
	}
	const char *mime = uri_mime ? uri_mime : text_mime;
	if (!mime) {
		wlr_log(WLR_INFO, "DnD: source has no text/uri MIME — no XDND export");
		return;
	}
	server->dnd_out_is_uri = (uri_mime != NULL);

	/* Synchronous-ish read via pipe + short poll of the event loop is
	 * awkward; reuse the clipboard async reader, then claim XdndSelection
	 * when done. For drag we need data sooner: do a blocking read with a
	 * temporary display dispatch. */
	int fds[2];
	if (pipe(fds) < 0) {
		return;
	}
	wlr_data_source_send(source, mime, fds[1]);
	close(fds[1]);

	char *buf = NULL;
	size_t len = 0, cap = 0;
	char tmp[4096];
	/* Pump the display so the client can write the pipe. */
	for (int spins = 0; spins < 100; spins++) {
		wl_display_flush_clients(server->wl_display);
		struct pollfd pfd = { .fd = fds[0], .events = POLLIN };
		int pr = poll(&pfd, 1, 5);
		if (pr < 0) {
			break;
		}
		if (pr == 0) {
			/* Also drain other events so the client makes progress. */
			wl_event_loop_dispatch(server->loop, 0);
			continue;
		}
		ssize_t n = read(fds[0], tmp, sizeof(tmp));
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				continue;
			}
			break;
		}
		if (n == 0) {
			break;
		}
		if (len + (size_t)n > WLX_CLIPBOARD_MAX) {
			free(buf);
			close(fds[0]);
			return;
		}
		if (len + (size_t)n + 1 > cap) {
			size_t ncap = cap ? cap * 2 : 4096;
			while (ncap < len + (size_t)n + 1) {
				ncap *= 2;
			}
			char *nbuf = realloc(buf, ncap);
			if (!nbuf) {
				free(buf);
				close(fds[0]);
				return;
			}
			buf = nbuf;
			cap = ncap;
		}
		memcpy(buf + len, tmp, (size_t)n);
		len += (size_t)n;
		buf[len] = '\0';
	}
	close(fds[0]);
	if (!buf || len == 0) {
		free(buf);
		return;
	}

	free(server->dnd_out_text);
	server->dnd_out_text = buf;
	server->dnd_out_len = len;
	server->dnd_out_active = true;
	server->dnd_out_target = XCB_WINDOW_NONE;
	server->dnd_out_accepted = false;

	xcb_set_selection_owner(server->xcb, server->clipboard_window,
		server->atom_xdnd_selection, XCB_CURRENT_TIME);
	xcb_flush(server->xcb);
	wlr_log(WLR_INFO, "DnD: prepared XDND export (%zu bytes, uri=%d)",
		len, server->dnd_out_is_uri);
	dnd_out_update_position(server);
}

void server_request_start_drag(struct wl_listener *listener, void *data) {
	struct wlx_server *server =
		wl_container_of(listener, server, request_start_drag);
	struct wlr_seat_request_start_drag_event *event = data;

	if (wlr_seat_validate_pointer_grab_serial(server->seat, event->origin,
			event->serial)) {
		wlr_seat_start_pointer_drag(server->seat, event->drag, event->serial);
	} else {
		wlr_data_source_destroy(event->drag->source);
	}
}

void drag_destroy(struct wl_listener *listener, void *data);

struct wlx_drag {
	struct wlx_server *server;
	struct wl_listener destroy;
};

void drag_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct wlx_drag *d = wl_container_of(listener, d, destroy);
	struct wlx_server *server = d->server;
	server->dnd_out_drag = NULL;
	/* If XdndDrop was sent and we are waiting for XdndFinished, keep
	 * selection data; otherwise tear the outbound drag down now. */
	if (!(server->dnd_out_active && server->dnd_out_target != XCB_WINDOW_NONE &&
			server->dnd_out_accepted)) {
		dnd_out_finish(server);
	}
	wl_list_remove(&d->destroy.link);
	free(d);
}

void server_start_drag(struct wl_listener *listener, void *data) {
	struct wlx_server *server =
		wl_container_of(listener, server, start_drag);
	struct wlr_drag *drag = data;

	struct wlx_drag *d = calloc(1, sizeof(*d));
	if (d) {
		d->server = server;
		d->destroy.notify = drag_destroy;
		wl_signal_add(&drag->events.destroy, &d->destroy);
	}
	server->dnd_out_drag = drag;

	if (drag->source) {
		dnd_out_begin_from_source(server, drag->source);
	}
	wlr_log(WLR_INFO, "DnD: Wayland drag started");
}

void dnd_atoms_init(struct wlx_server *server) {
	server->atom_xdnd_aware = intern_atom(server->xcb, "XdndAware");
	server->atom_xdnd_enter = intern_atom(server->xcb, "XdndEnter");
	server->atom_xdnd_position = intern_atom(server->xcb, "XdndPosition");
	server->atom_xdnd_status = intern_atom(server->xcb, "XdndStatus");
	server->atom_xdnd_leave = intern_atom(server->xcb, "XdndLeave");
	server->atom_xdnd_drop = intern_atom(server->xcb, "XdndDrop");
	server->atom_xdnd_finished = intern_atom(server->xcb, "XdndFinished");
	server->atom_xdnd_selection = intern_atom(server->xcb, "XdndSelection");
	server->atom_xdnd_type_list = intern_atom(server->xcb, "XdndTypeList");
	server->atom_xdnd_action_copy = intern_atom(server->xcb, "XdndActionCopy");
	server->atom_text_uri_list = intern_atom(server->xcb, "text/uri-list");
	server->atom_wlx_dnd = intern_atom(server->xcb, "_WLX_DND");
}
