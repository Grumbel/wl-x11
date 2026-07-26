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

### With Nix

```sh
nix build .
./result/bin/wl-x11
```

or, for a dev shell:

```sh
nix develop
meson setup build
ninja -C build
```

### Without Nix

You need: `meson`, `ninja`, `pkg-config`, `wlroots` (0.18 or 0.19 dev
headers), `wayland-server`, `wayland-protocols`, `libxkbcommon`, `pixman`,
`libxcb`.

```sh
meson setup build
ninja -C build
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
- **No host X11 clipboard bridge.** Copy/paste works between Wayland
  clients of this compositor, but is not synced with X11 applications
  running directly on the host. That would require an Xwayland-style
  clipboard proxy.
- **Cursor via the X11 backend output-cursor path.** Client
  `wl_pointer.set_cursor` surfaces are forwarded with
  `wlr_cursor_set_surface`; the X11 backend turns them into real X cursors
  on each output window. Outside client surfaces a theme `left_ptr` is
  shown. There is no separate compositor-drawn cursor overlay.
- **Best-effort window title/class syncing.** wlroots doesn't expose the
  raw `xcb_window_t` for backend-created outputs, so `WM_NAME`/`WM_CLASS`
  are set via a small side-channel XCB connection that diffs the root
  window's children right after each output is created. This is
  effectively always correct in practice but is not a hard guarantee
  under heavy concurrent window creation.
- **Keyboard focus follows pointer**, click-to-focus on button press; no
  alt-tab / window switching is implemented (that's the host WM's job for
  the X11 windows anyway).
- No layer-shell, no server-side decorations, no xdg-decoration
  negotiation — clients that insist on drawing client-side decorations
  will show them nested inside the (also decorated) host X11 window. Most
  modern toolkits (GTK4, Qt) default to no CSD when the compositor doesn't
  advertise `xdg-decoration` support in the way they expect.

## API version note

This targets **wlroots 0.18.x** headers (also tries 0.19 in
`meson.build`). If your distro ships a different major version, a handful
of calls (`wlr_output_event_commit::state`, `wlr_scene_surface_try_from_buffer`,
the `wlr_seat_pointer_notify_axis` argument list, etc.) may have slightly
different names/signatures — check the corresponding `wlr/...h` header and
adjust `src/main.c` accordingly; the overall architecture does not change.

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
