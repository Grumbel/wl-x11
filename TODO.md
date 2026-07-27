<!--
SPDX-FileCopyrightText: Copyright (c) 2026 Ingo Ruhnke <grumbel@gmail.com>
SPDX-License-Identifier: GPL-3.0-or-later
-->

# TODO

Follow-up work for wl-x11. Ordered roughly by impact. The project vendors a
patched wlroots (`subprojects/wlroots`); several items assume we can change
the X11 backend freely.

## Medium impact

### Clipboard / DnD scope

Current bridge is text-only (`text/plain` on CLIPBOARD and PRIMARY). Next
useful steps:

- Image / HTML MIME on CLIPBOARD
- Proper inbound XDND as a Wayland drag (needs a synthetic grab serial)
- `INCR` for large pastes

## Explicitly out of scope (for now)

- Layer-shell (not meaningful without a real desktop surface)
- Full server-side decoration negotiation beyond "always SSD so clients do
  not double-decorate"
- DRM / session backends

## Popups (`xdg_popup`)

### Current model (parent scene)

Popups are parented into the **parent toplevel’s scene tree** and drawn in the
same X11 window. This matches tinywl/sway and keeps input/grab simple:

- One pointer device, no extra `wl_output` when a menu opens
- Menu moves with the parent window
- Opening-click races are handled by the xdg_popup seat grab (wlroots may
  swallow release when focus is not on a popup surface)

**Trade-off:** content that extends past the parent window edge is **clipped**
by the X11 window.

### Tried and rejected: per-popup / shared OR stage

Mapping each popup (or a shared “popup stage”) to an override-redirect
`wlr_output` was explored and rolled back:

- A new `wl_output` mid-grab made Qt destroy menus immediately
- A pre-created shared OR stage avoided the global but introduced dual-window
  input (hover flaky, double-click to open, menus not tracking parent)
- Required compositor hacks (`swallow_next_pointer_release`, deferred
  release, manual root hit-testing) that fought `wlr_seat` (`notify_enter`
  resets buttons when focus changes)

Do **not** reintroduce OR-as-`wlr_output` for popups without a present path
that is not a full output (no `wl_output` global, no seat pointer device).

### Future: expand parent window while popup is open

To draw past the parent edge without a second X11 window, temporarily grow
the parent host window (and scene) to the **union of parent + popup bounds**,
then shrink on popup unmap.

| Decoration | Expand viability |
|-------------|------------------|
| **CSD (`--csd`)** | Promising. Host is already buffer-sized with ARGB; extra margin can be transparent “shadow” padding so the frame growth is largely invisible. |
| **SSD (default)** | Ugly. The host WM’s frame grows with the client → visible chrome jump. Prefer keep clipping under SSD, or only expand with `--csd`. |

Sketch:

1. On popup map: compute root-space union of parent content and popup geometry
2. Resize parent `wlr_output` / X11 window to that size; offset scene so
   existing content stays put (transparent padding on the overflow sides)
3. Place popup in parent scene at the usual geometry (now inside the larger
   window)
4. On popup unmap (last popup): restore previous size

Careful with `size_from_wm`, configure feedback loops, and interactive move
while expanded.

### Related leftovers (keep)

- **Bootstrap `WLX-BOOT` virtual monitor** — still required so clients like
  foot start with ≥1 `wl_output`. Not used for popups.
- **wlroots xdg_popup grab patches** (swallow outside release / ignore some
  failed presses) — still useful for Qt opening click with parent-scene
  popups; re-evaluate if upstream changes.
