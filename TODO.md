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
