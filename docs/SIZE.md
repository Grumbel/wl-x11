<!--
SPDX-FileCopyrightText: Copyright (c) 2026 Ingo Ruhnke <grumbel@gmail.com>
SPDX-License-Identifier: GPL-3.0-or-later
-->

# Size negotiation (X11 host ↔ Wayland client)

This document describes how **wl-x11** decides the size of each nested
X11 window and the corresponding Wayland `xdg_toplevel`, and how that
maps to public X11 and Wayland rules.

It is the design record for the logic in:

| Layer | Paths |
|-------|--------|
| Vendored X11 backend | `subprojects/wlroots/backend/x11/output.c` |
| Compositor policy | `src/xdg.c` (surface commit), `src/x11.c` (`output_request_state`), `src/output.c` (`resize_output_to`, `output_commit`, size hints) |

Related notes: [AGENTS.md](../AGENTS.md) (placement rules), [TODO.md](../TODO.md)
(size negotiation checklist).

---

## 1. Roles in this architecture

wl-x11 is unusual: each Wayland toplevel is backed by its own **managed
X11 top-level window** (`wlr_output` on the X11 backend). Size therefore
has two independent authorities:

| Authority | Mechanism | Meaning |
|-----------|-----------|---------|
| **Host window manager** | X11 `ConfigureNotify` on the content window | User resize, maximize, tile, WM policy |
| **Wayland client** | Buffer / window geometry on commit; optional preferred size | Content that wants a particular size |

There is **no single “true” size** until policy picks one. The rest of
this document is that policy.

On a normal (non-nested) Wayland compositor, the compositor owns the
output size and pushes `xdg_toplevel.configure` to the client. On X11,
the **window manager** owns top-level geometry after map. wl-x11 sits in
between: it is an X11 **client** of the host WM and a Wayland
**compositor** to nested clients.

---

## 2. Public protocol facts

### 2.1 X11 — ConfigureWindow / ConfigureNotify (ICCCM)

From the [ICCCM](https://x.org/releases/X11R7.6/doc/xorg-docs/specs/ICCCM/icccm.html)
(and Open Group / X11 programming manuals summarizing the same rules):

1. Clients resize top-levels with **`ConfigureWindow`**.
2. Clients **must not assume** the request is applied as sent. They must
   select `StructureNotify` and track **`ConfigureNotify`**.
3. The window manager may **rewrite** size and position (decoration
   frame, constraints, placement policy).
4. **Real** `ConfigureNotify` coordinates are relative to the *parent*
   (often a reparented frame, not the root). **Synthetic**
   `ConfigureNotify` (ICCCM §4.1.5, used when geometry did not change)
   reports root-relative position; the `send_event` bit is set.
5. **`ConfigureNotify` carries no “who requested this” or “purpose”
   field.** Sequence number, size, and whether the event is synthetic
   are the only structural signals.

Implication for a nested compositor: **origin of a size change cannot
be read off the event.** Correlation must be done by the party that
issued `ConfigureWindow` (our X11 backend).

### 2.2 X11 — WM_NORMAL_HINTS (ICCCM size hints)

`WM_NORMAL_HINTS` (`WM_SIZE_HINTS`) is a **hint** to the window manager.
Relevant flags (ICCCM / Xlib `XSizeHints`):

| Flag | Role |
|------|------|
| `USPosition` / `PPosition` | Position was user- or program-specified |
| `USSize` / `PSize` | Size was user- or program-specified |
| `PMinSize` / `PMaxSize` | Minimum / maximum size |
| `PBaseSize`, `PResizeInc`, `PAspect`, `PWinGravity` | Further constraints |

ICCCM and common WM practice: program-specified position often fights
multi-monitor placement; many toolkits avoid asserting position so the
WM can place the window.

wl-x11 placement rules ([AGENTS.md](../AGENTS.md)):

- Create at `(0,0)`; **do not** set `USPosition` / `PPosition`.
- Before map, set **`PSize | PMinSize` only** (optional `PMaxSize` from
  xdg min/max). Compositor owns this via `win_sync_size_hints` —
  backend does not set hints itself.
- Never `ConfigureWindow(x,y)` “to help” placement.

That matches ICCCM’s model: size is suggested; **position is left to
the WM**.

### 2.3 X11 — EWMH state (orthogonal to ConfigureNotify)

[`_NET_WM_STATE`](https://specifications.freedesktop.org/wm-spec/latest/)
(maximized, fullscreen, …) is a **property**, not a field on
`ConfigureNotify`. Maximize often arrives as state **and** a configure.
wl-x11 mirrors state into `xdg_toplevel` (`set_maximized` /
`set_fullscreen`) on property notify and still accepts size via the
configure path.

### 2.4 Wayland — xdg-shell toplevel size

From the [xdg-shell protocol](https://wayland.app/protocols/xdg-shell):

1. The compositor sends **`xdg_toplevel.configure(width, height, states)`**
   then **`xdg_surface.configure(serial)`**.
2. Width/height are a **hint** in window-geometry coordinates. **Zero**
   means the client may choose its own dimensions.
3. The client must **`ack_configure(serial)`** before committing a buffer
   that applies that configure.
4. The client may still attach a buffer whose geometry differs; many
   compositors accept that, but the *intended* model is that the client
   follows non-zero configures, especially under maximized/fullscreen/
   resizing states.
5. Interactive move/resize on Wayland is initiated by the **client**
   (`xdg_toplevel.resize` / `move`) with a seat serial; the compositor
   then drives configures. In wl-x11, interactive chrome is the **host
   WM**; the Wayland client does not drive host decoration resize.

wlroots exposes compositor-driven size as `wlr_xdg_toplevel_set_size()`,
which schedules the configure sequence above.

---

## 3. End-to-end flows in wl-x11

```
                    ┌─────────────────────┐
   Wayland client   │  xdg_toplevel       │
                    │  commit / geometry  │
                    └─────────┬───────────┘
                              │ preferred size (grow path)
                              ▼
                    ┌─────────────────────┐
                    │  compositor policy  │
                    │  size_from_wm?      │
                    │  grow-only fit?     │
                    └─────────┬───────────┘
                              │ resize_output_to / set_size
                              ▼
                    ┌─────────────────────┐
                    │  wlr_output MODE    │
                    │  (X11 backend)      │
                    └─────────┬───────────┘
                              │ ConfigureWindow
                              ▼
                    ┌─────────────────────┐
   Host X11 WM      │  may rewrite size   │
                    └─────────┬───────────┘
                              │ ConfigureNotify
                              ▼
                    ┌─────────────────────┐
                    │  backend filter:    │
                    │  stale / self-ack / │
                    │  echo / external    │
                    └─────────┬───────────┘
                              │ request_state (external only)
                              ▼
                    ┌─────────────────────┐
                    │  size_from_wm = 1   │
                    │  commit MODE        │
                    │  set_size → client  │
                    └─────────────────────┘
```

### 3.1 Backend: pending self-configure

`struct wlr_x11_output` tracks:

| Field | Meaning |
|-------|---------|
| `win_width` / `win_height` | Last size applied in `output_set_custom_mode` |
| `last_configure_seq` | Sequence of last `ConfigureWindow` we issued |
| `pending_width` / `pending_height` | Size we **asked** for (outstanding) |
| `has_pending_configure` | Pending request not yet resolved |

On **`ConfigureWindow`** (`output_set_custom_mode`):

1. Record sequence → `last_configure_seq`.
2. Set `pending_*` and `has_pending_configure`.
3. Optimistically set `win_*` to the requested size (wlroots mode path).

On **`ConfigureNotify`** (`handle_x11_configure_notify`):

| Condition | Action |
|-----------|--------|
| `width` or `height` is 0 | Ignore |
| Sequence older than `last_configure_seq` | **Stale** — ignore (out-of-order) |
| Size equals `pending_*` | **Self-ack** — clear pending; **no** `request_state` |
| Size equals `win_*` | **Echo** — ignore |
| Otherwise | **External** (host resize / WM rewrite) — clear pending; emit `wlr_output_send_request_state` |

Public helpers (for compositor debug / policy):

- `wlr_x11_output_has_pending_configure()`
- `wlr_x11_output_get_pending_size()`

**Why this matches X11:** ICCCM requires clients to follow
`ConfigureNotify`, not their own request. Matching pending size is the
only reliable “this notify completes *our* ConfigureWindow” signal.
Everything else is treated as host-driven because the protocol does not
label sources.

**What we deliberately do not do:** interpret `send_event` as
“host vs us” for all cases; use ignore counters or pixel-Δ heuristics
in the backend; emit `request_state` for self-acks (that re-entered
compositor policy and caused loops).

### 3.2 Compositor: `output_request_state`

Anything that still reaches the compositor is treated as **host
geometry**:

1. Ignore non-mode or empty requests.
2. No-op if requested size already equals current output mode.
3. Set `size_from_wm = true`.
4. `wlr_output_commit_state` (applies MODE → may `ConfigureWindow` again
   with the external size, recording a new pending).
5. `output_commit` listener → `wlr_xdg_toplevel_set_size` so the Wayland
   client is told the host size.

There is **no** compositor-side `awaiting_configure_*` soft-ignore loop
anymore; that lived in the wrong layer and fought real host resizes.

### 3.3 Compositor: preferred size on surface commit (`src/xdg.c`)

On each xdg surface commit, compute a preferred pixel size from window
geometry (SSD) or buffer size (CSD), scale it, and compare to the
current `wlr_output` size.

| Condition | Action |
|-----------|--------|
| `size_from_wm` and preferred ≠ output | **Hold host size** — do not `resize_output_to`; log `size: hold host size` |
| Not tiled, not `size_from_wm`, preferred **larger** than output | **Grow** host: `resize_output_to` + `set_size` |
| Preferred **smaller** than output, not host-driven | **Ignore shrink** — client oscillation must not shrink the X window |

**Grow-only** is intentional. A previous “fit both directions” policy
plus `client_assert` (clearing `size_from_wm` whenever geometry
differed from `last_client_conf`) produced a tight loop when clients
alternated buffers (e.g. 557×471 ↔ 2560×1391) and exhausted the
swapchain.

Initial map still works: the backend creates a **1×1 placeholder**
mode; the first real preferred size is always a grow.

### 3.4 Compositor: `resize_output_to` / `output_commit`

- `resize_output_to` commits a custom MODE on the X11 output. The
  backend records pending self-configure. No compositor awaiting flags.
- When the output mode actually changes, `output_commit` updates
  `last_output_*`, may letterbox one frame (`hold_present`), and calls
  `wlx_toplevel_set_size` so the client tracks the host (with CSD margin
  subtraction when appropriate).

### 3.5 Size hints to the host WM

`win_sync_size_hints` writes `WM_NORMAL_HINTS` with **`PSize | PMinSize`**
(and `PMaxSize` when xdg max is set). Values are in host pixels (scaled).
This is a **hint**, consistent with ICCCM; the WM remains free to
override.

---

## 4. Policy summary

| Event | Who wins | Client notified? |
|-------|----------|------------------|
| First map / larger client preferred | Grow host to preferred | `set_size` after mode apply |
| Client preferred shrink | Host size unchanged | No host resize; optional future re-assert of existing configure |
| Host WM resize / maximize / tile | Host (`size_from_wm`) | `set_size` via `output_commit` |
| ConfigureNotify matching our pending | Self-ack; no policy change | Already sized for that request |
| ConfigureNotify other size | Host | `request_state` → commit → `set_size` |

**Invariant:** once `size_from_wm` is set, client preferred size must not
drive `resize_output_to` until host state restores otherwise (e.g.
unmaximize clears the flag in the `_NET_WM_STATE` handler).

---

## 5. Known limitations and open checks

Documented in [TODO.md](../TODO.md); repeated here for the size design:

1. **Manual verify** still required: one bounce max on map; interactive
   WM resize updates the client; maximize/tile while a fit is in flight;
   preferred shrink does not fight the host.
2. **Client may attach a smaller buffer** than the last non-zero
   configure. xdg-shell allows the compositor to suggest size; some
   clients still commit “natural” sizes. Under `size_from_wm`, the host
   window stays large; the scene letterboxes rather than stretching
   ARGB shadows. Re-asserting `set_size` when geometry lags the host is
   a possible follow-up if a specific client misbehaves.
3. **Sequence filtering** can drop late notifies if many
   `ConfigureWindow` requests raise `last_configure_seq`. Pending-size
   ack is the primary self-filter; stale-seq is a backstop for
   out-of-order events only.
4. **No X11 “purpose” flag** exists; any future improvement stays in the
   realm of better correlation (pending queue, tying `_NET_WM_STATE` to
   concurrent configures), not discovering a missing protocol field.

---

## 6. References (public)

| Source | Relevance |
|--------|-----------|
| [ICCCM](https://x.org/releases/X11R7.6/doc/xorg-docs/specs/ICCCM/icccm.html) §4.1.5 Configuring the Window | ConfigureWindow vs ConfigureNotify; synthetic events; do not assume request arguments stick |
| [ICCCM](https://x.org/releases/X11R7.6/doc/xorg-docs/specs/ICCCM/icccm.html) §4.1.2.3 WM_NORMAL_HINTS | Size/position hints; PSize / PMinSize / position flags |
| [EWMH / wm-spec](https://specifications.freedesktop.org/wm-spec/latest/) | `_NET_WM_STATE`, workarea; state is not ConfigureNotify |
| [xdg-shell](https://wayland.app/protocols/xdg-shell) | `xdg_toplevel.configure`, `ack_configure`, zero size = client-driven |
| XCB / Xlib size hints | Flag bits for `WM_SIZE_HINTS` (`PSize` = 1<<3, `PMinSize` = 1<<4, …) |

---

## 7. Changelog (design)

| Date | Change |
|------|--------|
| 2026-07 | Backend pending self-configure; drop compositor `awaiting_configure_*` |
| 2026-07 | Grow-only preferred fit; shrink is host-driven only |
| 2026-07 | This document added under `docs/SIZE.md` |
