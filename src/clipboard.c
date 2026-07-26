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

struct wlx_text_source {
	struct wlr_data_source base;
	struct wlx_server *server;
};

void clipboard_clear_read(struct wlx_server *server) {
	if (server->clip_read_source) {
		wl_event_source_remove(server->clip_read_source);
		server->clip_read_source = NULL;
	}
	if (server->clip_read_fd >= 0) {
		close(server->clip_read_fd);
		server->clip_read_fd = -1;
	}
	free(server->clip_read_buf);
	server->clip_read_buf = NULL;
	server->clip_read_len = 0;
	server->clip_read_cap = 0;
}

void clipboard_set_x11_owner(struct wlx_server *server,
		const char *text, size_t len, bool primary) {
	if (!server->xcb || server->clipboard_window == XCB_WINDOW_NONE) {
		return;
	}
	char **out_text = primary ? &server->pri_out_text : &server->clip_out_text;
	size_t *out_len = primary ? &server->pri_out_len : &server->clip_out_len;
	bool *we_own = primary ? &server->pri_we_own_x11 : &server->clip_we_own_x11;
	xcb_atom_t sel = primary ? server->atom_primary : server->atom_clipboard;

	free(*out_text);
	*out_text = NULL;
	*out_len = 0;
	if (text && len > 0) {
		*out_text = malloc(len + 1);
		if (!*out_text) {
			return;
		}
		memcpy(*out_text, text, len);
		(*out_text)[len] = '\0';
		*out_len = len;
	}
	xcb_set_selection_owner(server->xcb, server->clipboard_window,
		sel, XCB_CURRENT_TIME);
	xcb_flush(server->xcb);
	xcb_get_selection_owner_cookie_t cookie =
		xcb_get_selection_owner(server->xcb, sel);
	xcb_get_selection_owner_reply_t *reply =
		xcb_get_selection_owner_reply(server->xcb, cookie, NULL);
	*we_own = reply && reply->owner == server->clipboard_window;
	free(reply);
	wlr_log(WLR_INFO, "clipboard: claimed X11 %s (%zu bytes, own=%d)",
		primary ? "PRIMARY" : "CLIPBOARD", *out_len, *we_own);
}

const char *clipboard_pick_mime(struct wlr_data_source *source) {
	static const char *prefs[] = {
		"text/plain;charset=utf-8",
		"text/plain",
		"TEXT",
		"STRING",
		"UTF8_STRING",
		NULL,
	};
	char **p;
	wl_array_for_each(p, &source->mime_types) {
		for (int i = 0; prefs[i]; i++) {
			if (strcmp(*p, prefs[i]) == 0) {
				return *p;
			}
		}
	}
	/* Any text/ MIME type */
	wl_array_for_each(p, &source->mime_types) {
		if (strncmp(*p, "text/", 5) == 0) {
			return *p;
		}
	}
	return NULL;
}

int clipboard_read_fd(int fd, uint32_t mask, void *data) {
	struct wlx_server *server = data;
	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
		/* Finished: publish whatever we collected. */
		if (server->clip_read_len > 0 && server->clip_read_buf) {
			clipboard_set_x11_owner(server, server->clip_read_buf,
				server->clip_read_len, server->clip_read_is_primary);
		}
		clipboard_clear_read(server);
		return 0;
	}
	if (!(mask & WL_EVENT_READABLE)) {
		return 0;
	}
	char tmp[4096];
	for (;;) {
		ssize_t n = read(fd, tmp, sizeof(tmp));
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				break;
			}
			clipboard_clear_read(server);
			return 0;
		}
		if (n == 0) {
			if (server->clip_read_len > 0 && server->clip_read_buf) {
				clipboard_set_x11_owner(server, server->clip_read_buf,
					server->clip_read_len, server->clip_read_is_primary);
			}
			clipboard_clear_read(server);
			return 0;
		}
		if (server->clip_read_len + (size_t)n > WLX_CLIPBOARD_MAX) {
			wlr_log(WLR_ERROR, "clipboard: Wayland selection exceeds %d bytes, ignoring",
				WLX_CLIPBOARD_MAX);
			clipboard_clear_read(server);
			return 0;
		}
		if (server->clip_read_len + (size_t)n + 1 > server->clip_read_cap) {
			size_t ncap = server->clip_read_cap ? server->clip_read_cap * 2 : 4096;
			while (ncap < server->clip_read_len + (size_t)n + 1) {
				ncap *= 2;
			}
			char *nbuf = realloc(server->clip_read_buf, ncap);
			if (!nbuf) {
				clipboard_clear_read(server);
				return 0;
			}
			server->clip_read_buf = nbuf;
			server->clip_read_cap = ncap;
		}
		memcpy(server->clip_read_buf + server->clip_read_len, tmp, (size_t)n);
		server->clip_read_len += (size_t)n;
		server->clip_read_buf[server->clip_read_len] = '\0';
	}
	return 0;
}

void clipboard_export_wayland_source(struct wlx_server *server,
		struct wlr_data_source *source) {
	if (!server->xcb || !source) {
		return;
	}
	const char *mime = clipboard_pick_mime(source);
	if (!mime) {
		wlr_log(WLR_INFO, "clipboard: Wayland source has no text MIME type");
		return;
	}
	clipboard_clear_read(server);
	server->clip_read_is_primary = false;

	int fds[2];
	if (pipe(fds) < 0) {
		return;
	}
	int flags = fcntl(fds[0], F_GETFL, 0);
	if (flags >= 0) {
		fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);
	}
	server->clip_read_fd = fds[0];
	server->clip_read_source = wl_event_loop_add_fd(server->loop, fds[0],
		WL_EVENT_READABLE | WL_EVENT_HANGUP, clipboard_read_fd, server);
	if (!server->clip_read_source) {
		close(fds[0]);
		close(fds[1]);
		server->clip_read_fd = -1;
		return;
	}
	wlr_data_source_send(source, mime, fds[1]);
	close(fds[1]);
}

const char *primary_pick_mime(struct wlr_primary_selection_source *source) {
	static const char *prefs[] = {
		"text/plain;charset=utf-8",
		"text/plain",
		"TEXT",
		"STRING",
		"UTF8_STRING",
		NULL,
	};
	char **p;
	wl_array_for_each(p, &source->mime_types) {
		for (int i = 0; prefs[i]; i++) {
			if (strcmp(*p, prefs[i]) == 0) {
				return *p;
			}
		}
	}
	wl_array_for_each(p, &source->mime_types) {
		if (strncmp(*p, "text/", 5) == 0) {
			return *p;
		}
	}
	return NULL;
}

void primary_export_wayland_source(struct wlx_server *server,
		struct wlr_primary_selection_source *source) {
	if (!server->xcb || !source) {
		return;
	}
	const char *mime = primary_pick_mime(source);
	if (!mime) {
		wlr_log(WLR_INFO, "primary: Wayland source has no text MIME type");
		return;
	}
	clipboard_clear_read(server);
	server->clip_read_is_primary = true;

	int fds[2];
	if (pipe(fds) < 0) {
		return;
	}
	int flags = fcntl(fds[0], F_GETFL, 0);
	if (flags >= 0) {
		fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);
	}
	server->clip_read_fd = fds[0];
	server->clip_read_source = wl_event_loop_add_fd(server->loop, fds[0],
		WL_EVENT_READABLE | WL_EVENT_HANGUP, clipboard_read_fd, server);
	if (!server->clip_read_source) {
		close(fds[0]);
		close(fds[1]);
		server->clip_read_fd = -1;
		return;
	}
	wlr_primary_selection_source_send(source, mime, fds[1]);
	close(fds[1]);
}

void text_source_send(struct wlr_data_source *base, const char *mime,
		int32_t fd) {
	(void)mime;
	struct wlx_text_source *ts = wl_container_of(base, ts, base);
	struct wlx_server *server = ts->server;
	if (server->clip_in_text && server->clip_in_len > 0) {
		const char *p = server->clip_in_text;
		size_t left = server->clip_in_len;
		while (left > 0) {
			ssize_t n = write(fd, p, left);
			if (n < 0) {
				if (errno == EINTR) {
					continue;
				}
				break;
			}
			p += n;
			left -= (size_t)n;
		}
	}
	close(fd);
}

void text_source_destroy(struct wlr_data_source *base) {
	struct wlx_text_source *ts = wl_container_of(base, ts, base);
	free(ts);
}

static const struct wlr_data_source_impl text_source_impl = {
	.send = text_source_send,
	.destroy = text_source_destroy,
};

struct wlx_pri_source {
	struct wlr_primary_selection_source base;
	struct wlx_server *server;
};

void pri_source_send(struct wlr_primary_selection_source *base,
		const char *mime, int fd) {
	(void)mime;
	struct wlx_pri_source *ps = wl_container_of(base, ps, base);
	struct wlx_server *server = ps->server;
	if (server->pri_in_text && server->pri_in_len > 0) {
		const char *p = server->pri_in_text;
		size_t left = server->pri_in_len;
		while (left > 0) {
			ssize_t n = write(fd, p, left);
			if (n < 0) {
				if (errno == EINTR) {
					continue;
				}
				break;
			}
			p += n;
			left -= (size_t)n;
		}
	}
	close(fd);
}

void pri_source_destroy(struct wlr_primary_selection_source *base) {
	struct wlx_pri_source *ps = wl_container_of(base, ps, base);
	free(ps);
}

static const struct wlr_primary_selection_source_impl pri_source_impl = {
	.send = pri_source_send,
	.destroy = pri_source_destroy,
};

void clipboard_offer_x11_text_to_wayland(struct wlx_server *server,
		char *text, size_t len) {
	free(server->clip_in_text);
	server->clip_in_text = text;
	server->clip_in_len = len;

	struct wlx_text_source *ts = calloc(1, sizeof(*ts));
	if (!ts) {
		return;
	}
	ts->server = server;
	wlr_data_source_init(&ts->base, &text_source_impl);

	const char *mimes[] = {
		"text/plain;charset=utf-8",
		"text/plain",
	};
	for (size_t i = 0; i < sizeof(mimes) / sizeof(mimes[0]); i++) {
		char **slot = wl_array_add(&ts->base.mime_types, sizeof(char *));
		if (!slot) {
			wlr_data_source_destroy(&ts->base);
			return;
		}
		*slot = strdup(mimes[i]);
		if (!*slot) {
			wlr_data_source_destroy(&ts->base);
			return;
		}
	}

	server->clip_setting_from_x11 = true;
	wlr_seat_set_selection(server->seat, &ts->base,
		wl_display_next_serial(server->wl_display));
	server->clip_setting_from_x11 = false;
	wlr_log(WLR_INFO, "clipboard: offered X11 CLIPBOARD to Wayland (%zu bytes)",
		len);
}

void primary_offer_x11_text_to_wayland(struct wlx_server *server,
		char *text, size_t len) {
	free(server->pri_in_text);
	server->pri_in_text = text;
	server->pri_in_len = len;

	struct wlx_pri_source *ps = calloc(1, sizeof(*ps));
	if (!ps) {
		return;
	}
	ps->server = server;
	wlr_primary_selection_source_init(&ps->base, &pri_source_impl);

	const char *mimes[] = {
		"text/plain;charset=utf-8",
		"text/plain",
	};
	for (size_t i = 0; i < sizeof(mimes) / sizeof(mimes[0]); i++) {
		char **slot = wl_array_add(&ps->base.mime_types, sizeof(char *));
		if (!slot) {
			wlr_primary_selection_source_destroy(&ps->base);
			return;
		}
		*slot = strdup(mimes[i]);
		if (!*slot) {
			wlr_primary_selection_source_destroy(&ps->base);
			return;
		}
	}

	server->pri_setting_from_x11 = true;
	wlr_seat_set_primary_selection(server->seat, &ps->base,
		wl_display_next_serial(server->wl_display));
	server->pri_setting_from_x11 = false;
	wlr_log(WLR_INFO, "primary: offered X11 PRIMARY to Wayland (%zu bytes)",
		len);
}

void clipboard_request_from_x11(struct wlx_server *server, bool primary) {
	if (!server->xcb || server->clipboard_window == XCB_WINDOW_NONE) {
		return;
	}
	if (primary ? server->pri_we_own_x11 : server->clip_we_own_x11) {
		return;
	}
	xcb_atom_t sel = primary ? server->atom_primary : server->atom_clipboard;
	xcb_atom_t prop = primary ? server->atom_wlx_primary : server->atom_wlx_clipboard;
	xcb_delete_property(server->xcb, server->clipboard_window, prop);
	xcb_convert_selection(server->xcb, server->clipboard_window,
		sel, server->atom_utf8_string, prop, XCB_CURRENT_TIME);
	xcb_flush(server->xcb);
}

void clipboard_handle_selection_notify(struct wlx_server *server,
		xcb_selection_notify_event_t *ev) {
	if (ev->property == XCB_ATOM_NONE ||
			ev->requestor != server->clipboard_window) {
		return;
	}
	bool primary = (ev->property == server->atom_wlx_primary) ||
		(ev->selection == server->atom_primary);
	xcb_atom_t prop = primary ? server->atom_wlx_primary : server->atom_wlx_clipboard;
	if (ev->property != prop && ev->property != server->atom_wlx_clipboard &&
			ev->property != server->atom_wlx_primary) {
		return;
	}
	xcb_get_property_cookie_t cookie = xcb_get_property(server->xcb, 0,
		server->clipboard_window, ev->property,
		XCB_GET_PROPERTY_TYPE_ANY, 0, WLX_CLIPBOARD_MAX / 4);
	xcb_get_property_reply_t *reply =
		xcb_get_property_reply(server->xcb, cookie, NULL);
	if (!reply || reply->type == XCB_ATOM_NONE || reply->value_len == 0) {
		free(reply);
		return;
	}
	/* Skip INCR (incremental) transfers — incomplete by design. */
	xcb_atom_t incr = intern_atom(server->xcb, "INCR");
	if (reply->type == incr) {
		wlr_log(WLR_INFO, "clipboard: ignoring INCR transfer from X11");
		free(reply);
		return;
	}
	int len = xcb_get_property_value_length(reply);
	if (len <= 0 || len > WLX_CLIPBOARD_MAX) {
		free(reply);
		return;
	}
	char *text = malloc((size_t)len + 1);
	if (!text) {
		free(reply);
		return;
	}
	memcpy(text, xcb_get_property_value(reply), (size_t)len);
	text[len] = '\0';
	free(reply);
	if (primary) {
		primary_offer_x11_text_to_wayland(server, text, (size_t)len);
	} else {
		clipboard_offer_x11_text_to_wayland(server, text, (size_t)len);
	}
}

void clipboard_handle_selection_request(struct wlx_server *server,
		xcb_selection_request_event_t *req) {
	xcb_selection_notify_event_t notify = {
		.response_type = XCB_SELECTION_NOTIFY,
		.pad0 = 0,
		.sequence = 0,
		.time = req->time,
		.requestor = req->requestor,
		.selection = req->selection,
		.target = req->target,
		.property = XCB_ATOM_NONE,
	};

	bool primary = (req->selection == server->atom_primary);
	bool clipboard = (req->selection == server->atom_clipboard);
	const char *out = primary ? server->pri_out_text : server->clip_out_text;
	size_t out_len = primary ? server->pri_out_len : server->clip_out_len;

	if ((!primary && !clipboard) || !out) {
		xcb_send_event(server->xcb, 0, req->requestor,
			XCB_EVENT_MASK_NO_EVENT, (const char *)&notify);
		xcb_flush(server->xcb);
		return;
	}

	if (req->target == server->atom_targets) {
		xcb_atom_t targets[] = {
			server->atom_targets,
			server->atom_utf8_string,
			server->atom_string,
			server->atom_text,
		};
		xcb_change_property(server->xcb, XCB_PROP_MODE_REPLACE, req->requestor,
			req->property, XCB_ATOM_ATOM, 32,
			sizeof(targets) / sizeof(targets[0]), targets);
		notify.property = req->property;
	} else if (req->target == server->atom_utf8_string ||
			req->target == server->atom_string ||
			req->target == server->atom_text) {
		xcb_atom_t type = (req->target == server->atom_string)
			? server->atom_string : server->atom_utf8_string;
		xcb_change_property(server->xcb, XCB_PROP_MODE_REPLACE, req->requestor,
			req->property, type, 8, (uint32_t)out_len, out);
		notify.property = req->property;
	}

	xcb_send_event(server->xcb, 0, req->requestor,
		XCB_EVENT_MASK_NO_EVENT, (const char *)&notify);
	xcb_flush(server->xcb);
}

void clipboard_handle_selection_clear(struct wlx_server *server,
		xcb_selection_clear_event_t *ev) {
	if (ev->selection == server->atom_clipboard) {
		server->clip_we_own_x11 = false;
		wlr_log(WLR_INFO, "clipboard: lost X11 CLIPBOARD ownership");
	} else if (ev->selection == server->atom_primary) {
		server->pri_we_own_x11 = false;
		wlr_log(WLR_INFO, "primary: lost X11 PRIMARY ownership");
	}
}

bool clipboard_init(struct wlx_server *server, xcb_screen_t *screen) {
	server->clip_read_fd = -1;
	server->atom_clipboard = intern_atom(server->xcb, "CLIPBOARD");
	server->atom_primary = intern_atom(server->xcb, "PRIMARY");
	server->atom_targets = intern_atom(server->xcb, "TARGETS");
	server->atom_string = intern_atom(server->xcb, "STRING");
	server->atom_text = intern_atom(server->xcb, "TEXT");
	server->atom_wlx_clipboard = intern_atom(server->xcb, "_WLX_CLIPBOARD");
	server->atom_wlx_primary = intern_atom(server->xcb, "_WLX_PRIMARY");

	server->clipboard_window = xcb_generate_id(server->xcb);
	uint32_t values[] = { XCB_EVENT_MASK_PROPERTY_CHANGE };
	xcb_create_window(server->xcb, XCB_COPY_FROM_PARENT, server->clipboard_window,
		screen->root, 0, 0, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_ONLY,
		screen->root_visual, XCB_CW_EVENT_MASK, values);

	const xcb_query_extension_reply_t *xfixes =
		xcb_get_extension_data(server->xcb, &xcb_xfixes_id);
	if (xfixes && xfixes->present) {
		server->xfixes_event_base = xfixes->first_event;
		xcb_xfixes_query_version(server->xcb, 5, 0);
		uint32_t mask =
			XCB_XFIXES_SELECTION_EVENT_MASK_SET_SELECTION_OWNER |
			XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_WINDOW_DESTROY |
			XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_CLIENT_CLOSE;
		xcb_xfixes_select_selection_input(server->xcb, server->clipboard_window,
			server->atom_clipboard, mask);
		xcb_xfixes_select_selection_input(server->xcb, server->clipboard_window,
			server->atom_primary, mask);
		server->xfixes_ok = true;
		wlr_log(WLR_INFO, "clipboard: XFixes monitoring CLIPBOARD+PRIMARY");
	} else {
		wlr_log(WLR_ERROR, "clipboard: XFixes unavailable; X11→Wayland paste disabled");
	}
	xcb_flush(server->xcb);

	/* Import whatever the host already has. */
	clipboard_request_from_x11(server, false);
	clipboard_request_from_x11(server, true);
	return true;
}

void clipboard_finish(struct wlx_server *server) {
	clipboard_clear_read(server);
	free(server->clip_out_text);
	server->clip_out_text = NULL;
	free(server->clip_in_text);
	server->clip_in_text = NULL;
	free(server->pri_out_text);
	server->pri_out_text = NULL;
	free(server->pri_in_text);
	server->pri_in_text = NULL;
	if (server->xcb && server->clipboard_window != XCB_WINDOW_NONE) {
		xcb_destroy_window(server->xcb, server->clipboard_window);
		server->clipboard_window = XCB_WINDOW_NONE;
	}
}

void server_request_set_selection(struct wl_listener *listener, void *data) {
	struct wlx_server *server =
		wl_container_of(listener, server, request_set_selection);
	struct wlr_seat_request_set_selection_event *event = data;
	wlr_seat_set_selection(server->seat, event->source, event->serial);

	/* Export text to host X11 CLIPBOARD (unless we just set this from X11). */
	if (!server->clip_setting_from_x11) {
		if (event->source) {
			clipboard_export_wayland_source(server, event->source);
		}
	}
}

void server_request_set_primary_selection(struct wl_listener *listener,
		void *data) {
	struct wlx_server *server =
		wl_container_of(listener, server, request_set_primary_selection);
	struct wlr_seat_request_set_primary_selection_event *event = data;
	wlr_seat_set_primary_selection(server->seat, event->source, event->serial);

	if (!server->pri_setting_from_x11 && event->source) {
		primary_export_wayland_source(server, event->source);
	}
}
