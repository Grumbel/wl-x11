<!--
SPDX-FileCopyrightText: Copyright (c) 2026 Ingo Ruhnke <grumbel@gmail.com>
SPDX-License-Identifier: GPL-3.0-or-later
-->

# Notes for agents and contributors

## Project shape

wl-x11 is a minimal wlroots-based Wayland compositor that runs nested under
X11. Unlike a single nested desktop window, it creates **one top-level X11
window per Wayland `xdg_toplevel`**, so the host window manager decorates,
places, and taskbar-lists them like normal apps.

Architecture summary is in [README.md](README.md). Planned and suggested work
is tracked in [TODO.md](TODO.md) — read that before starting larger changes.

## Vendored wlroots

`subprojects/wlroots` is **required and patched**. Meson builds it by
default; `-Duse_system_wlroots=true` is an explicit escape hatch only.
Important local changes:

- Create window at (0,0) with a **1×1 placeholder** mode; backend does **not**
  set `WM_NORMAL_HINTS` (compositor owns hints via `win_sync_size_hints`)
- In `output_commit`, apply `MODE` **before** `MapWindow` so MapRequest sees
  the real client size (needed for mouse/center placement)
- Public helpers `wlr_x11_output_get_window()` and
  `wlr_x11_backend_get_connection()` so the compositor does not scan the
  root window's children to find the backing X id

Do not replace the subproject with an unpatched system wlroots without
re-applying those fixes. Prefer extending the vendored tree when a change
belongs in the X11 backend. When upstreaming, these are the candidates for
a `wlr_x11_rootless`-style feature flag.

### What to open in `subprojects/wlroots`

Build is **X11-only** (`backends=x11`, `session=disabled`, `xwayland=disabled`,
`renderers=gles2`). Unused trees were removed so agents do not wander into
them.

**Relevant:**

| Path | Why |
|------|-----|
| `backend/x11/` | Nested window backend; **local patches live here** |
| `backend/backend.c`, `backend/multi/` | Backend plumbing (still linked) |
| Headers matching `#include <wlr/...>` in `src/` | Public API the compositor uses |
| `types/` for scene, seat, xdg_shell, output, data_device, compositor, cursor, primary_selection | Core types used by `src/` |
| `render/` (gles2 + shared allocator path) | Rendering |

**Ignore unless a task explicitly names them:**

- `backend/wayland/`, `backend/headless/` (always compiled by upstream layout, not used by wl-x11)
- Other `types/wlr_*.c` protocols (layer-shell, foreign-toplevel, screencopy, tablet, session-lock, …) — linked but never created by this compositor
- DRM, libinput, session, Xwayland, examples, tinywl, tests, docs — **deleted** from this fork

## Placement rules (do not regress)

For ordinary top-levels:

1. Create at (0,0); do **not** set `USPosition` / `PPosition`.
2. Set `WM_NORMAL_HINTS` with **PSize | PMinSize only** before map.
3. Commit preferred size, then map (backend applies size before MapWindow).
4. Never `ConfigureWindow(x,y)` to “help” placement — that forces global
   coordinates and often lands on the wrong monitor.

Transients still use `WM_TRANSIENT_FOR` / dialog type via
`apply_transient_hints`.

## Source layout

| Path | Role |
|------|------|
| `src/main.c` | Startup, globals, seat, decorations |
| `src/output.c` | Per-toplevel X11 output create/map/resize, size hints |
| `src/xdg.c` | xdg_toplevel + xdg_popup (parent-scene placement) |
| `src/x11.c` | Side-channel XCB (title, class, focus, properties) |
| `src/input.c` | Pointer/keyboard; coords from X11 root, not layout cursor |
| `src/move_resize.c` | Host configure during drag |
| `src/clipboard.c` | CLIPBOARD / PRIMARY text bridge |
| `src/dnd.c` | Limited XDND bridge |
| `src/server.h` | Shared types and declarations |

## Build

```sh
# Nix
nix develop
meson setup build && ninja -C build

# Or plain meson with vendored wlroots
meson setup build && ninja -C build
```

Run under an existing X11 session (`DISPLAY=:0 ./build/wl-x11`), then point
clients at the printed `WAYLAND_DISPLAY`.

## Popups

Keep popups in the **parent scene / same X11 window**. Do not reintroduce
per-popup or shared OR `wlr_output`s without a present path that is not a
full output (see [TODO.md](TODO.md)). Pointer hit-testing must use real X11
window geometry, not `wlr_output_layout` positions.

## When changing behavior

- Prefer ICCCM/EWMH-correct behavior over compositor-side hacks.
- Keep comments that explain *why* position is unspecified and why size is
  applied before map — those were hard-won.
- Never call `wlr_seat_pointer_notify_enter` while a button is held if it
  would change the focused surface — that resets seat buttons mid-click.
- Update [TODO.md](TODO.md) when finishing or retiring an item.
