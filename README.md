<!--
SPDX-FileCopyrightText: Copyright (c) 2026 Ingo Ruhnke <grumbel@gmail.com>
SPDX-License-Identifier: GPL-3.0-or-later
-->

# wl-x11

A minimal [wlroots](https://gitlab.freedesktop.org/wlroots/wlroots)-based
Wayland compositor that runs **nested under X11** — not as a full nested
desktop in one window, but as **one ordinary X11 top-level window per
Wayland `xdg_toplevel`**.

Host window managers decorate, move, resize, maximize, and task-switch
those windows like any other X client. Wayland apps keep talking Wayland;
wl-x11 is the bridge.

**Version:** see [`VERSION`](VERSION) (flake builds embed `VERSION+g<rev>`).

## How it works

wlroots’ X11 backend models host windows as `wlr_output`s. A typical nested
compositor creates one output for “the desktop”. wl-x11 creates a **new
output when an `xdg_toplevel` maps**, sizes that surface to the content
window, and destroys the output when the surface unmaps or the user closes
the X11 window.

Outputs are still placed non-overlapping in a shared `wlr_scene` for
bookkeeping only. On-screen placement is entirely the host WM’s job —
see [AGENTS.md](AGENTS.md) (no fake `ConfigureWindow` for placement).

Size negotiation (who owns the pixel size — client preferred vs host WM)
is documented in **[docs/SIZE.md](docs/SIZE.md)**.

## Quick start

### Nix

```sh
nix build .
./result/bin/wl-x11 --debug foot          # compositor + client
# or:
./result/bin/wl-x11
WAYLAND_DISPLAY=wayland-0 foot            # from another terminal
```

Dev shell (patched wlroots as `packages.wlroots`):

```sh
nix develop
meson setup build -Duse_system_wlroots=true
ninja -C build
./build/wl-x11 --debug
```

### Without Nix

Needs: `meson`, `ninja`, `pkg-config`, Wayland, xkbcommon, pixman, libdrm,
GLES2/EGL, X11/xcb (including xfixes, xinput), and the **vendored** tree
under `subprojects/wlroots` (or an equivalent patched package).

```sh
meson setup build
ninja -C build
./build/wl-x11
```

Meson uses `subprojects/wlroots` when present. Distro wlroots **without**
the project patches will not work correctly. Nix builds the same tree as
`packages.wlroots` and points the compositor at it with
`-Duse_system_wlroots=true`.

## Running

Must run inside an existing X11 session:

```sh
DISPLAY=:0 ./build/wl-x11 [--debug] [--csd|--ssd] [command [args…]]
```

Useful options:

| Flag | Meaning |
|------|---------|
| `--debug` | Verbose `WLR_INFO` size / X11 logs |
| `--csd` | Prefer client-side decorations |
| `--ssd` | Prefer server-side (host WM) decorations (default) |
| `--version` / `-V` | Print version and exit |
| `command …` | Launch with `WAYLAND_DISPLAY` set; compositor exits when no clients remain |

Each mapped toplevel should appear as its own decorated X11 window.

## Resize stress client

[`tools/resize-torture`](tools/resize-torture) is a raw xdg-shell client that
cycles grow, shrink, oscillation, maximize, and undersize-while-max. Flake
output is separate from the compositor:

```sh
nix run . -- --debug -- nix run .#resize-torture
# or: nix build .#resize-torture && WAYLAND_DISPLAY=… ./result/bin/resize-torture
```

Compare its stderr with compositor `size:` lines. Design notes:
[docs/SIZE.md](docs/SIZE.md).

## Features (0.1.0)

- One X11 content window per `xdg_toplevel` (ICCCM/EWMH-friendly)
- Host WM maximize / fullscreen mirrored into xdg state
- Content-driven and host-driven size policy (see `docs/SIZE.md`)
- Popups and overflowing subsurfaces via **rootless present-windows**
  (menus that leave the parent frame without an extra `wl_output`)
- Clipboard and primary selection bridge; basic drag-and-drop
- Optional CSD vs SSD preference
- Bootstrap virtual `wl_output` so clients that require a monitor at
  connect can start before the first toplevel maps

## Scope / known limitations

Intentionally small (tinywl-shaped), not a full desktop shell.

| Area | Status |
|------|--------|
| **Layer-shell** | Out of scope (no real desktop surface) |
| **DRM / seat session** | X11 backend only |
| **KDE server-decoration protocol** | Not advertised (GTK mode loops) |
| **Popups** | Present-window path; edge cases still worth testing on real GTK/Qt apps |
| **Placement** | Host WM only; do not set `USPosition` / `PPosition` |
| **Scaling** | Optional content scale; multi-monitor host scale is coarse |

Popups / GTK menubar subsurfaces that must leave the parent frame use
rootless override-redirect present-windows (`wlr_x11_present_window`). Input
is hit-tested in root coordinates. See [TODO.md](TODO.md) and [AGENTS.md](AGENTS.md).

## Project layout

| Path | Role |
|------|------|
| `src/` | Compositor |
| `subprojects/wlroots/` | **Required** patched wlroots (X11 backend) |
| `docs/SIZE.md` | Size negotiation design |
| `tools/resize-torture/` | Automated resize stress client |
| `AGENTS.md` | Contributor / agent rules |
| `TODO.md` | Roadmap and checklists |
| `VERSION` | Version string source of truth |

## License

GPL-3.0-or-later. See [LICENSES/](LICENSES/) and [REUSE.toml](REUSE.toml).
