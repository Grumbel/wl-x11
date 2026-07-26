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
