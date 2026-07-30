<!--
SPDX-FileCopyrightText: Copyright (c) 2026 Ingo Ruhnke <grumbel@gmail.com>
SPDX-License-Identifier: GPL-3.0-or-later
-->

# resize-torture

Raw **xdg-shell** client that walks through the size behaviors that keep
breaking nested X11 compositors (especially wl-x11). Logs every
configure, ack, and buffer attach on stderr.

## Phases

| Phase | What it does |
|-------|----------------|
| `map` | First buffer 400×300 |
| `grow` | Attach 640×480 (client-driven grow) |
| `shrink` | Attach 320×200 (client-driven shrink) |
| `oscillate` | Alternate 400×300 ↔ 900×700 every `--period-ms` (A↔B thrash) |
| `restable` | Attach 500×350 so unmaximize does not restore a 64×64 leftover |
| `follow` | Obey compositor configures |
| `maximize` | `xdg_toplevel.set_maximized` |
| `undersize_while_max` | Half-size buffers while maximized (letterbox bait) |
| `unmaximize` | `unset_maximized` (host should restore ~restable size) |
| `done` | Exit (or restart with `--loop`) |

Floating “undersize” is intentional omitted: under preferred-fit the host
would shrink with the buffer, which is not the letterbox failure mode.

## Build / run

```bash
# Flake output (separate from the compositor derivation)
nix build .#resize-torture
nix run .#resize-torture -- --loop

# Under wl-x11
wl-x11 &
WAYLAND_DISPLAY=wayland-N ./result/bin/resize-torture
```

Local meson:

```bash
cd tools/resize-torture
meson setup build && ninja -C build
./build/resize-torture --period-ms 40
```

Compare stderr with compositor `size:` logs (`docs/SIZE.md`).
