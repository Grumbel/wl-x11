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
```

It prints the `WAYLAND_DISPLAY` it created a socket on. Point Wayland
clients at it:

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
one-window-per-toplevel model. Notable things it does **not** do:

- **Popups get clipped at the window edge.** A dropdown/menu that would
  visually extend past its parent toplevel's bounds is clipped, because
  the toplevel's entire visible area *is* the X11 window — there's no
  extra canvas to draw into (unlike Xwayland rootless mode, which gives
  popups their own override-redirect X windows).
- **Text-only host X11 clipboard + PRIMARY bridge.** `text/plain` is
  mirrored between the Wayland seat selection and host `CLIPBOARD`, and
  between `zwp_primary_selection_v1` and host `PRIMARY` (middle-click).
  Images and other MIME types are not bridged; large INCR transfers are
  ignored. PRIMARY is optional on pure Wayland; many toolkits still use it.
- **Drag-and-drop.** Wayland↔Wayland DnD works via the seat. Bridging to
  host X11 uses XDND for `text/plain` and `text/uri-list` only. Drops from
  X11 onto a nested window are imported into the Wayland selection (paste)
  rather than injected as a surface-local drop (no grab serial from X11).
- **Cursor via the X11 backend output-cursor path.** Client
  `wl_pointer.set_cursor` surfaces are forwarded with
  `wlr_cursor_set_surface`; the X11 backend turns them into real X cursors
  on each output window. Outside client surfaces a theme `left_ptr` is
  shown. There is no separate compositor-drawn cursor overlay.
- **Window title/class syncing.** The vendored X11 backend exposes
  `wlr_x11_output_get_window()` so the compositor can set `WM_NAME` /
  `WM_CLASS` / ICCCM hints on the real host window before map. A side-channel
  XCB connection is still used for property writes and event monitoring.
- **Keyboard focus follows pointer**, click-to-focus on button press; no
  alt-tab / window switching is implemented (that's the host WM's job for
  the X11 windows anyway).
- No layer-shell, no server-side decorations, no xdg-decoration
  negotiation — clients that insist on drawing client-side decorations
  will show them nested inside the (also decorated) host X11 window. Most
  modern toolkits (GTK4, Qt) default to no CSD when the compositor doesn't
  advertise `xdg-decoration` support in the way they expect.

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
