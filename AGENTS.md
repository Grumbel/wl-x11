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


## Git commits (chat / agent sessions)

After **every** meaningful change made in a chat session, write a **detailed
git commit** (do not batch unrelated work into one vague message).

- Prefer one logical change per commit when practical.
- Subject line: imperative, specific (e.g. `Promote overflowing subsurfaces to present-windows`).
- Body should explain **why**, not only what: problem, approach, files touched,
  intentional non-goals, and any follow-up / known gaps.
- Reference design docs when relevant (`TODO.md` phases, AGENTS rules).
- If the user did not ask to run `git commit`, still **prepare** the full
  message in the reply so it can be committed immediately.


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
- `wlr_x11_present_window_*` API for rootless OR menus (Present/DRI3
  without `wlr_output` / seat devices)

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
| `src/xdg.c` | xdg_toplevel + xdg_popup (present-window menus) |
| `src/subpresent.c` | Overflowing wl_subsurface → present-window (GTK menubar) |
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

## Popups and overflowing subsurfaces

Two client paths produce menus that must leave the parent X11 window:

| Client path | Host path |
|-------------|-----------|
| `xdg_popup` (Qt menus, GTK context menus) | `wlx_popup` + present-window (`src/xdg.c`) |
| Overflowing `wl_subsurface` (GTK menubar dropdowns) | `wlx_subpresent` + present-window (`src/subpresent.c`) |

Both use **rootless override-redirect present-windows**
(`wlr_x11_present_window` in vendored wlroots), not a second `wlr_output`.

Rules:

1. **No** `wlr_output` / `wl_output` global / `new_input` device per menu.
2. Present-windows are OR root children with `_NET_WM_WINDOW_TYPE_MENU`.
3. For `xdg_popup`: scene trees parked off-layout; `popup_render_and_present`.
4. For overflowing subsurfaces: parent scene buffer node **disabled** while
   the OR window is up; re-enabled on demote/destroy.
5. Hit-test in **root X11 coordinates**: xdg_popup presents, then
   subpresents, then toplevels. Never use `wlr_output_layout` cursor coords
   for focus.
6. Do **not** call `wlr_seat_pointer_notify_enter` while `button_count > 0`
   if that would change the focused surface.
7. Parent move → `wlx_reposition_popups_for_window` (also repositions
   subpresents); parent unmap/destroy tears down child present-windows first.
8. Do **not** grow the parent X11 window to fit menus (host WM flicker /
   `size_from_wm` fights).

Fallback: if present-window create fails for an `xdg_popup`, parent-scene
placement (clipped) is used so the menu still appears.

`wlr_x11_output_create_override_redirect` remains for the bootstrap
`WLX-BOOT` monitor and CSD transient dialogs only — not for menus.

Retired approaches: parent-window expand; per-popup / shared OR `wlr_output`.
See [TODO.md](TODO.md) for history and remaining integration checks.

## When changing behavior

- Prefer ICCCM/EWMH-correct behavior over compositor-side hacks.
- Keep comments that explain *why* position is unspecified and why size is
  applied before map — those were hard-won.
- Never call `wlr_seat_pointer_notify_enter` while a button is held if it
  would change the focused surface — that resets seat buttons mid-click.
- Update [TODO.md](TODO.md) when finishing or retiring an item.
