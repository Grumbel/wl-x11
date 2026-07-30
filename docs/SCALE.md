<!--
SPDX-FileCopyrightText: Copyright (c) 2026 Ingo Ruhnke <grumbel@gmail.com>
SPDX-License-Identifier: GPL-3.0-or-later
-->

# Content scale (`--scale`)

wl-x11 can enlarge or shrink the **host X11 content window** relative to the
Wayland client’s logical buffer size via `--scale <factor>` (e.g. `1.5`).

Clients still allocate **logical** buffers. Host pixels ≈ `logical × scale`.

## Coordinate spaces

| Space | Used for |
|-------|----------|
| **Logical** | Wayland buffer size, xdg geometry, subsurface offsets, pointer surface-local coords |
| **Host pixels** | X11 content window size, present-window size/position, `wlr_output` mode |

Helpers: `wlx_scale_size()` / `wlx_unscale_size()` / `wlx_pointer_to_surface()`.

## Toplevel scene

`wlr_scene_xdg_surface` resets buffer dest sizes (and layout positions) to
**logical** values on surface commit. After that we must apply scale **once**:

| Call site | Positions × scale? | Why |
|-----------|-------------------|-----|
| Surface commit (`wlx_apply_content_scale_ex(win, true)`) | **Yes** | Positions are logical again |
| `output_frame` / mid-resize hold (`wlx_apply_content_scale`) | **No** (dest only) | Positions already scaled; multiplying again sends nodes flying |

Dest size is always `surface.current.width/height × scale` (from the surface,
not from the previous dest).

## Present-window popups / subpresents

Menus that leave the parent frame use rootless OR windows.

1. **Placement** (`popup_compute_root_placement` / `subpresent_compute_root`):
   - Position: parent content root + `scale(logical offset)`
   - Size: `scale(logical surface/buffer size)`
2. **Configure** the present window to that **host** size (never replace with
   the unscaled buffer size when `scale ≠ 1`).
3. **Present**:
   - If buffer size equals present size (typical when `scale == 1`): direct
     Present of the client buffer.
   - Else: composite (stretch logical texture into host-sized buffer) and
     Present that.

### Bugs this document is defending against

| Failure | Cause |
|---------|--------|
| Qt menu cropped / too small under `--scale` | Present window forced to logical buffer size after scaled placement |
| Gedit menu flies to corner / shrinks recursively | Scene node **positions** multiplied by scale on **every frame** |
| Nested submenu wrong place | Parent present root box must stay in host pixels; child offsets scaled once |

## Input

Hit-tests use host-pixel root boxes on present-windows. Convert to logical
surface coordinates with `wlx_pointer_to_surface` before notifying clients.

## Manual check

```sh
wl-x11 --scale 1.5 --debug
# Qt menu, GTK menubar (gedit): size matches UI scale, position under the
# parent item, no drift over time while the menu stays open.
```
