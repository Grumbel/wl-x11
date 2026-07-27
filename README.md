<!--
SPDX-FileCopyrightText: Copyright (c) 2026 Ingo Ruhnke <grumbel@gmail.com>
SPDX-License-Identifier: GPL-3.0-or-later
-->

# wl-x11

A minimal [wlroots](https://gitlab.freedesktop.org/wlroots/wlroots)-based
Wayland compositor that runs nested inside an X11 session — but, unlike
Weston's or KWin's X11 backends, it does **not** present the whole nested
Wayland desktop inside one X11 window. Instead it creates one ordinary
top-level X11 window per Wayland toplevel (`xdg_toplevel`), so Wayland
clients feel like native X11 windows: your host window manager decorates
them, moves/resizes them, lists them in the taskbar/alt-tab, etc.

## How it works

wlroots' X11 backend models "windows on the host X server" as
`wlr_output`s. Normally a compositor creates one such output at startup for
"the nested desktop". `wl-x11` instead creates a **new output on demand
every time a Wayland `xdg_toplevel` surface maps**, sizes that toplevel to
exactly fill the new output/window, and destroys the output again when the
toplevel unmaps, is destroyed, or the user closes the X11 window.

Internally, all these per-window outputs are still placed (non-overlapping,
via `wlr_output_layout_add_auto`) into a single shared `wlr_scene` graph,
purely as bookkeeping — each toplevel's scene subtree is pinned to exactly
its own output's region. There is no meaningful relationship between this
internal layout and where the X11 windows actually sit on your desktop;
that's entirely up to the host window manager.

## Building

**Patched wlroots is required.** This project ships a patched tree under
`subprojects/wlroots` (MODE before MapWindow, size-only `WM_NORMAL_HINTS`,
`wlr_x11_output_get_window()`, and related rootless fixes). A plain distro
wlroots will **not** include those changes.

Meson **prefers the vendored subproject when present**. If it is absent
(Nix builds the same tree as `packages.wlroots` and strips `subprojects/`
from the compositor source), meson falls back to pkg-config, which must
then be that patched package.

### With Nix

```sh
nix build .
./result/bin/wl-x11
```

or, for a dev shell:

```sh
nix develop
# Use the flake's patched wlroots package (subproject is not in the shell src):
meson setup build -Duse_system_wlroots=true
ninja -C build
```

The flake builds `subprojects/wlroots` as a separate derivation
(`packages.wlroots`) so compositor-only edits do not rebuild wlroots.

### Without Nix

You need: `meson`, `ninja`, `pkg-config`, `wayland-server`,
`wayland-protocols`, `libxkbcommon`, `pixman`, `libxcb`, `xcb-xfixes`,
plus the usual X11/GL deps wlroots wants for the X11 backend (see
`flake.nix` / the subproject’s meson files).

```sh
# Uses subprojects/wlroots automatically
meson setup build
ninja -C build
```

To force a system wlroots (not recommended unless you reapplied the
patches):

```sh
meson setup build -Duse_system_wlroots=true
```

## Running

Run it from inside an existing X11 session (a terminal on your desktop is
fine):

```sh
DISPLAY=:0 ./build/wl-x11
# optional: --csd / --ssd, --debug, and/or a command to launch:
# DISPLAY=:0 ./build/wl-x11 --debug foot
```

It prints the `WAYLAND_DISPLAY` it created a socket on. Point Wayland
clients at it (or pass the client as arguments so `WAYLAND_DISPLAY` is set
for that process only):

```sh
WAYLAND_DISPLAY=wayland-1 foot
WAYLAND_DISPLAY=wayland-1 weston-terminal
WAYLAND_DISPLAY=wayland-1 gtk4-demo
```

Each client window that appears should show up as its own regular,
decorated X11 window under your host window manager.

## Scope / known limitations

This is intentionally minimal, in the spirit of a "one file you can read
in one sitting" compositor (à la wlroots' `tinywl.c`), adapted to the
one-window-per-toplevel model.

### Popups

`xdg_popup` surfaces (menus, combo dropdowns, tooltips) are placed in the
**parent toplevel’s scene tree** and drawn in the same X11 window. Input and
grabs stay simple; the menu moves with the parent. Content that would extend
past the parent edge is **clipped**.

Override-redirect / extra `wlr_output` windows per popup were tried and
rejected (new `wl_output` globals break Qt; dual-window input is fragile).
A possible future approach is temporarily expanding the parent window while
a popup is open — more practical with `--csd` than with host SSD. See
[TODO.md](TODO.md).

### Decorations

By default the compositor prefers **server-side decorations** (host WM
chrome) via `xdg-decoration` and `org_kde_kwin_server_decoration`. Pass
`--csd` for client-side decorations (client-drawn title/shadows; host frame
suppressed where possible). `--ssd` is explicit for the default.

### Other limitations

- **Bootstrap virtual monitor.** A hidden `wl_output` is created at startup
  so clients that require ≥1 monitor at connect (e.g. foot) can start before
  any toplevel maps. It is not a real desktop surface.
- **Text-only host X11 clipboard + PRIMARY bridge.** `text/plain` on
  `CLIPBOARD` and `PRIMARY`. Images/other MIME and large `INCR` transfers
  are not bridged.
- **Drag-and-drop.** Wayland↔Wayland DnD works. Host XDND bridges
  `text/plain` and `text/uri-list` only; X11→Wayland drops become selection
  paste rather than a surface-local drop.
- **Cursor** via the X11 backend output-cursor path (`wl_pointer.set_cursor`
  → real X cursor per window); theme `left_ptr` otherwise.
- **Focus** tracks the host WM’s X11 focus (and pointer enter); no
  compositor-side alt-tab (that’s the host WM’s job).
- **No layer-shell.**

## API version note

The vendored tree is **wlroots 0.21.x-dev** (see
`subprojects/wlroots/meson.build`). The compositor is written against that
API. If you use `-Duse_system_wlroots=true` with another major version,
expect signature drift (`wlr_output` commit events, scene helpers, seat
axis notify, etc.) and missing local APIs such as
`wlr_x11_output_get_window()`.

## License

[GPL-3.0-or-later](https://spdx.org/licenses/GPL-3.0-or-later.html).
See [`LICENSES/GPL-3.0-or-later.txt`](LICENSES/GPL-3.0-or-later.txt) and
SPDX headers in each file. Compliance can be checked with
[REUSE](https://reuse.software/):

```sh
nix flake check        # runs `reuse lint` via checks.reuse
# or, in the dev shell:
reuse lint
```
