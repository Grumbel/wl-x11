<!--
SPDX-FileCopyrightText: Copyright (c) 2026 Ingo Ruhnke <grumbel@gmail.com>
SPDX-License-Identifier: GPL-3.0-or-later
-->

# TODO

Follow-up work for wl-x11. Ordered roughly by impact. The project vendors a
patched wlroots (`subprojects/wlroots`); several items assume we can change
the X11 backend freely.

## High impact

### Popups that are not clipped

`xdg_popup` content that would extend past the parent toplevel is clipped
because the toplevel's visible area *is* the host X11 window.

Options:

- **Override-redirect popup windows** — on popup map, create a second X11
  window (override-redirect, no decorations), position it in root coordinates
  relative to the parent X window. Closest to Xwayland rootless.
- **Grow the parent frame** temporarily when a popup would overflow; keep
  decorations on the original geometry via frame extents / input shape.
  Harder and more fragile with host WMs.

OR windows are the preferred path for menus and dropdowns.

## Medium impact

### Cursor and input polish

- Per-window cursor is already via the X11 backend; ensure leave/enter when
  the pointer crosses between two of *our* X windows does not briefly show
  the wrong cursor or drop focus.
- Keyboard focus: force-on-map is good for dialogs. Optionally respect host
  `FocusIn` / `FocusOut` more strictly so alt-tab away and back matches pure
  X apps.

### Clipboard / DnD scope

Current bridge is text-only (`text/plain` on CLIPBOARD and PRIMARY). Next
useful steps:

- Image / HTML MIME on CLIPBOARD
- Proper inbound XDND as a Wayland drag (needs a synthetic grab serial)
- `INCR` for large pastes

## Backend / packaging

### Defaults tuned for many small outputs

- Avoid relying on the backend pre-map size (1024×768) once the compositor
  always commits a real mode before map. A 1×1 or first preferred size is
  enough as a placeholder.
- Optional: skip the backend's own `WM_NORMAL_HINTS` when the compositor is
  about to set them, or document that the compositor owns hints.

### Build and packaging

- Document that the vendored subproject is **required** (patched), not
  optional, so a plain `pkg-config wlroots` does not silently drop MODE-before-
  map and size-hint fixes.
- Consider a compile flag (e.g. `wlr_x11_rootless`) around the window-id
  export and commit order so upstreaming later is cleaner.

## Explicitly out of scope (for now)

- Layer-shell (not meaningful without a real desktop surface)
- Full server-side decoration negotiation beyond "always SSD so clients do
  not double-decorate"
- DRM / session backends
