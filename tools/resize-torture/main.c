/* SPDX-FileCopyrightText: Copyright (c) 2026 Ingo Ruhnke <grumbel@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * resize-torture — raw xdg-shell client that exercises size paths that
 * keep breaking nested X11 compositors (wl-x11 in particular).
 *
 * Phases (see phase_name[]):
 *   map → grow → shrink → oscillate → restable → follow →
 *   maximize → undersize_while_max → unmaximize → done
 *
 * Floating undersize is omitted: preferred-fit would shrink the host, so
 * it does not test letterboxing. Letterbox is only exercised while max.
 *
 * Logs every configure, ack, buffer attach, and phase change on stderr.
 *
 * Usage:
 *   WAYLAND_DISPLAY=… ./resize-torture
 *   ./resize-torture --loop          # repeat forever
 *   ./resize-torture --period-ms 30  # oscillation / step timing
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"

/* --- logging ----------------------------------------------------------- */

static int64_t now_msec(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int64_t t0;

static void log_msg(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	int64_t ms = now_msec() - t0;
	fprintf(stderr, "[%6lld.%03lld] ", (long long)(ms / 1000), (long long)(ms % 1000));
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

/* --- globals ----------------------------------------------------------- */

enum phase {
	PH_MAP = 0,
	PH_GROW,
	PH_SHRINK,
	PH_OSCILLATE,
	/* After oscillation the host may still be at the large size (guard
	 * blocked shrink). Restabilize to a known natural size before max. */
	PH_RESTABLE,
	PH_FOLLOW,
	PH_MAXIMIZE,
	PH_UNDERSIZE_MAX,
	PH_UNMAXIMIZE,
	PH_DONE,
	PH_COUNT,
};

static const char *phase_name(enum phase p) {
	switch (p) {
	case PH_MAP: return "map";
	case PH_GROW: return "grow";
	case PH_SHRINK: return "shrink";
	case PH_OSCILLATE: return "oscillate";
	case PH_RESTABLE: return "restable";
	case PH_FOLLOW: return "follow";
	case PH_MAXIMIZE: return "maximize";
	case PH_UNDERSIZE_MAX: return "undersize_while_max";
	case PH_UNMAXIMIZE: return "unmaximize";
	case PH_DONE: return "done";
	default: return "?";
	}
}

struct app {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct xdg_wm_base *wm_base;
	struct wl_surface *surface;
	struct xdg_surface *xdg_surface;
	struct xdg_toplevel *toplevel;

	int conf_w, conf_h; /* last configure size (0 = client picks) */
	uint32_t conf_serial;
	bool conf_pending;
	bool maximized;
	bool activated;

	enum phase phase;
	int phase_step;
	int64_t phase_start_ms;
	int64_t last_step_ms;

	int period_ms;
	bool loop;
	bool running;

	/* last attached buffer size */
	int buf_w, buf_h;
};

static struct app g;

/* Desired buffer size for the current phase (may differ from conf). */
static void desired_size(int *w, int *h) {
	int cw = g.conf_w > 0 ? g.conf_w : 400;
	int ch = g.conf_h > 0 ? g.conf_h : 300;

	switch (g.phase) {
	case PH_MAP:
		*w = 400;
		*h = 300;
		break;
	case PH_GROW:
		*w = 640;
		*h = 480;
		break;
	case PH_SHRINK:
		*w = 320;
		*h = 200;
		break;
	case PH_OSCILLATE:
		/* Rapid A↔B — the maximize-race pattern without state. */
		if ((g.phase_step % 2) == 0) {
			*w = 400;
			*h = 300;
		} else {
			*w = 900;
			*h = 700;
		}
		break;
	case PH_RESTABLE:
		/* Known size so unmaximize does not restore 64x64. */
		*w = 500;
		*h = 350;
		break;
	case PH_UNDERSIZE_MAX:
		/* Deliberately smaller than the maximized configure (letterbox).
		 * Only meaningful while host_authority holds the X window. */
		*w = cw > 80 ? cw / 2 : 40;
		*h = ch > 80 ? ch / 2 : 40;
		if (*w < 64) {
			*w = 64;
		}
		if (*h < 64) {
			*h = 64;
		}
		break;
	case PH_UNMAXIMIZE:
		/* Prefer restable size so a stale maximized conf does not re-attach
		 * a fullscreen buffer before the host ConfigureNotify arrives. */
		if (g.conf_w > 0 && g.conf_h > 0 && g.conf_w < 1200 && g.conf_h < 900) {
			*w = g.conf_w;
			*h = g.conf_h;
		} else {
			*w = 500;
			*h = 350;
		}
		break;
	case PH_FOLLOW:
	case PH_MAXIMIZE:
	case PH_DONE:
	default:
		/* Obey configure; if 0, modest natural size. */
		*w = g.conf_w > 0 ? g.conf_w : 500;
		*h = g.conf_h > 0 ? g.conf_h : 350;
		break;
	}
}

/* --- shm buffer -------------------------------------------------------- */

static struct wl_buffer *make_buffer(int width, int height, uint32_t color) {
	int stride = width * 4;
	int size = stride * height;
	char path[] = "/tmp/wl-x11-resize-torture-XXXXXX";
	int fd = mkstemp(path);
	if (fd < 0) {
		log_msg("mkstemp failed: %s", strerror(errno));
		return NULL;
	}
	unlink(path);
	if (ftruncate(fd, size) < 0) {
		log_msg("ftruncate failed: %s", strerror(errno));
		close(fd);
		return NULL;
	}
	void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		log_msg("mmap failed: %s", strerror(errno));
		close(fd);
		return NULL;
	}
	uint32_t *pix = data;
	for (int i = 0; i < width * height; i++) {
		pix[i] = color;
	}
	struct wl_shm_pool *pool = wl_shm_create_pool(g.shm, fd, size);
	close(fd);
	struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, width, height,
		stride, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	munmap(data, size);
	return buf;
}

static uint32_t phase_color(void) {
	/* Distinct solid colors per phase so letterboxing is visible. */
	switch (g.phase) {
	case PH_MAP: return 0xFF336699;
	case PH_GROW: return 0xFF339966;
	case PH_SHRINK: return 0xFF996633;
	case PH_OSCILLATE: return 0xFF993366;
	case PH_RESTABLE: return 0xFF669966;
	case PH_FOLLOW: return 0xFF669933;
	case PH_MAXIMIZE: return 0xFF333399;
	case PH_UNDERSIZE_MAX: return 0xFF993333;
	case PH_UNMAXIMIZE: return 0xFF339999;
	default: return 0xFF444444;
	}
}

static void attach_desired(void) {
	int w = 0, h = 0;
	desired_size(&w, &h);
	if (w < 1) {
		w = 1;
	}
	if (h < 1) {
		h = 1;
	}
	struct wl_buffer *buf = make_buffer(w, h, phase_color());
	if (!buf) {
		return;
	}
	wl_surface_attach(g.surface, buf, 0, 0);
	wl_surface_damage_buffer(g.surface, 0, 0, w, h);
	wl_surface_commit(g.surface);
	wl_buffer_destroy(buf);
	g.buf_w = w;
	g.buf_h = h;
	log_msg("attach buffer %dx%d (phase=%s step=%d conf=%dx%d max=%d act=%d)",
		w, h, phase_name(g.phase), g.phase_step,
		g.conf_w, g.conf_h, (int)g.maximized, (int)g.activated);
}

/* --- phase machine ----------------------------------------------------- */

static int phase_duration_ms(enum phase p) {
	switch (p) {
	case PH_MAP: return 800;
	case PH_GROW: return 800;
	case PH_SHRINK: return 800;
	case PH_OSCILLATE: return 1500; /* many flips at period_ms */
	case PH_RESTABLE: return 900;
	case PH_FOLLOW: return 600;
	case PH_MAXIMIZE: return 1000;
	case PH_UNDERSIZE_MAX: return 1500;
	case PH_UNMAXIMIZE: return 1200;
	case PH_DONE: return 500;
	default: return 500;
	}
}

static void enter_phase(enum phase p) {
	g.phase = p;
	g.phase_step = 0;
	g.phase_start_ms = now_msec();
	g.last_step_ms = g.phase_start_ms;
	log_msg("=== PHASE %s ===", phase_name(p));

	if (p == PH_MAXIMIZE) {
		log_msg("xdg_toplevel.set_maximized");
		xdg_toplevel_set_maximized(g.toplevel);
	} else if (p == PH_UNMAXIMIZE) {
		log_msg("xdg_toplevel.unset_maximized");
		xdg_toplevel_unset_maximized(g.toplevel);
	}

	/* First buffer for the phase (may be before configure). */
	attach_desired();
}

static void advance_phase(void) {
	enum phase next = (enum phase)((int)g.phase + 1);
	if (next >= PH_COUNT) {
		if (g.loop) {
			log_msg("loop: restarting from map");
			enter_phase(PH_MAP);
		} else {
			log_msg("done — exiting");
			g.running = false;
		}
		return;
	}
	enter_phase(next);
}

static void tick(void) {
	int64_t now = now_msec();
	int64_t elapsed = now - g.phase_start_ms;

	if (g.phase == PH_OSCILLATE) {
		if (now - g.last_step_ms >= g.period_ms) {
			g.phase_step++;
			g.last_step_ms = now;
			log_msg("oscillate step %d", g.phase_step);
			attach_desired();
		}
	} else if (g.phase == PH_UNDERSIZE_MAX) {
		/* Re-assert undersized buffer while maximized (letterbox bait). */
		if (now - g.last_step_ms >= g.period_ms * 2) {
			g.phase_step++;
			g.last_step_ms = now;
			attach_desired();
		}
	}

	if (elapsed >= phase_duration_ms(g.phase)) {
		advance_phase();
	}
}

/* --- wayland listeners ------------------------------------------------- */

static void xdg_wm_base_ping(void *data, struct xdg_wm_base *wm, uint32_t serial) {
	(void)data;
	xdg_wm_base_pong(wm, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
	.ping = xdg_wm_base_ping,
};

static void xdg_surface_configure(void *data, struct xdg_surface *xdg_surface,
		uint32_t serial) {
	(void)data;
	g.conf_serial = serial;
	g.conf_pending = true;
	log_msg("xdg_surface.configure serial=%u (pending conf %dx%d)",
		serial, g.conf_w, g.conf_h);
	xdg_surface_ack_configure(xdg_surface, serial);
	g.conf_pending = false;
	attach_desired();
}

static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel,
		int32_t width, int32_t height, struct wl_array *states) {
	(void)data;
	(void)toplevel;
	g.conf_w = width;
	g.conf_h = height;
	g.maximized = false;
	g.activated = false;
	char state_buf[128];
	size_t n = 0;
	state_buf[0] = '\0';
	uint32_t *s;
	wl_array_for_each(s, states) {
		const char *name = "?";
		switch (*s) {
		case XDG_TOPLEVEL_STATE_MAXIMIZED:
			g.maximized = true;
			name = "max";
			break;
		case XDG_TOPLEVEL_STATE_FULLSCREEN:
			name = "fs";
			break;
		case XDG_TOPLEVEL_STATE_RESIZING:
			name = "resizing";
			break;
		case XDG_TOPLEVEL_STATE_ACTIVATED:
			g.activated = true;
			name = "act";
			break;
		case XDG_TOPLEVEL_STATE_TILED_LEFT:
			name = "tile_l";
			break;
		case XDG_TOPLEVEL_STATE_TILED_RIGHT:
			name = "tile_r";
			break;
		case XDG_TOPLEVEL_STATE_TILED_TOP:
			name = "tile_t";
			break;
		case XDG_TOPLEVEL_STATE_TILED_BOTTOM:
			name = "tile_b";
			break;
		default:
			break;
		}
		n += (size_t)snprintf(state_buf + n, sizeof(state_buf) - n,
			"%s%s", n ? "," : "", name);
		if (n >= sizeof(state_buf)) {
			break;
		}
	}
	log_msg("xdg_toplevel.configure %dx%d states=[%s]",
		width, height, state_buf);
}

static void toplevel_close(void *data, struct xdg_toplevel *toplevel) {
	(void)data;
	(void)toplevel;
	log_msg("xdg_toplevel.close");
	g.running = false;
}

static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = toplevel_configure,
	.close = toplevel_close,
};

static void registry_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version) {
	(void)data;
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		g.compositor = wl_registry_bind(registry, name, &wl_compositor_interface,
			version < 4 ? version : 4);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		g.shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
		g.wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface,
			version < 2 ? version : 2);
		xdg_wm_base_add_listener(g.wm_base, &wm_base_listener, NULL);
	}
}

static void registry_global_remove(void *data, struct wl_registry *registry,
		uint32_t name) {
	(void)data;
	(void)registry;
	(void)name;
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

/* --- main -------------------------------------------------------------- */

static void usage(const char *argv0) {
	fprintf(stderr,
		"Usage: %s [--loop] [--period-ms N]\n"
		"  Raw xdg-shell client that cycles resize stress phases.\n"
		"  Logs configures, acks, and buffer attaches on stderr.\n",
		argv0);
}

int main(int argc, char **argv) {
	g.period_ms = 40;
	g.loop = false;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--loop") == 0) {
			g.loop = true;
		} else if (strcmp(argv[i], "--period-ms") == 0 && i + 1 < argc) {
			g.period_ms = atoi(argv[++i]);
			if (g.period_ms < 5) {
				g.period_ms = 5;
			}
		} else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
			usage(argv[0]);
			return 0;
		} else {
			usage(argv[0]);
			return 1;
		}
	}

	t0 = now_msec();
	log_msg("resize-torture start period_ms=%d loop=%d", g.period_ms, (int)g.loop);

	g.display = wl_display_connect(NULL);
	if (!g.display) {
		log_msg("wl_display_connect failed (WAYLAND_DISPLAY=%s)",
			getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "(unset)");
		return 1;
	}

	g.registry = wl_display_get_registry(g.display);
	wl_registry_add_listener(g.registry, &registry_listener, NULL);
	wl_display_roundtrip(g.display);

	if (!g.compositor || !g.shm || !g.wm_base) {
		log_msg("missing globals compositor=%p shm=%p xdg_wm_base=%p",
			(void *)g.compositor, (void *)g.shm, (void *)g.wm_base);
		return 1;
	}

	g.surface = wl_compositor_create_surface(g.compositor);
	g.xdg_surface = xdg_wm_base_get_xdg_surface(g.wm_base, g.surface);
	xdg_surface_add_listener(g.xdg_surface, &xdg_surface_listener, NULL);
	g.toplevel = xdg_surface_get_toplevel(g.xdg_surface);
	xdg_toplevel_add_listener(g.toplevel, &toplevel_listener, NULL);
	xdg_toplevel_set_title(g.toplevel, "wl-x11 resize-torture");
	xdg_toplevel_set_app_id(g.toplevel, "wl-x11.resize-torture");
	/* Hint a modest min size; no max. */
	xdg_toplevel_set_min_size(g.toplevel, 64, 64);

	wl_surface_commit(g.surface);
	log_msg("initial commit (waiting for first configure)");

	g.running = true;
	/* Wait for first configure before entering phases. */
	while (g.running && g.conf_serial == 0) {
		if (wl_display_dispatch(g.display) < 0) {
			log_msg("dispatch error before first configure");
			return 1;
		}
	}

	enter_phase(PH_MAP);

	while (g.running) {
		tick();
		/* Flush outbound, then wait briefly for events. */
		wl_display_flush(g.display);
		struct timeval tv = {
			.tv_sec = 0,
			.tv_usec = (g.period_ms > 0 ? g.period_ms : 20) * 1000 / 2,
		};
		int fd = wl_display_get_fd(g.display);
		fd_set rfds;
		FD_ZERO(&rfds);
		FD_SET(fd, &rfds);
		int r = select(fd + 1, &rfds, NULL, NULL, &tv);
		if (r > 0) {
			if (wl_display_dispatch(g.display) < 0) {
				log_msg("dispatch error: %s", strerror(errno));
				break;
			}
		} else if (r < 0 && errno != EINTR) {
			log_msg("select error: %s", strerror(errno));
			break;
		} else {
			/* timeout — still process any deferred events */
			if (wl_display_prepare_read(g.display) == 0) {
				wl_display_read_events(g.display);
				wl_display_dispatch_pending(g.display);
			} else {
				wl_display_dispatch_pending(g.display);
			}
		}
	}

	log_msg("cleanup");
	if (g.toplevel) {
		xdg_toplevel_destroy(g.toplevel);
	}
	if (g.xdg_surface) {
		xdg_surface_destroy(g.xdg_surface);
	}
	if (g.surface) {
		wl_surface_destroy(g.surface);
	}
	if (g.wm_base) {
		xdg_wm_base_destroy(g.wm_base);
	}
	if (g.compositor) {
		wl_compositor_destroy(g.compositor);
	}
	if (g.shm) {
		wl_shm_destroy(g.shm);
	}
	if (g.registry) {
		wl_registry_destroy(g.registry);
	}
	if (g.display) {
		wl_display_disconnect(g.display);
	}
	return 0;
}
