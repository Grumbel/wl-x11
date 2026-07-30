# SPDX-FileCopyrightText: Copyright (c) 2026 Ingo Ruhnke <grumbel@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
project(
  'wl-x11-resize-torture',
  'c',
  version: '0.1.0',
  meson_version: '>=0.59.0',
  default_options: [
    'c_std=c11',
    'warning_level=2',
    'werror=false',
  ],
)

wayland_client = dependency('wayland-client')
wayland_cursor = dependency('wayland-cursor', required: false)
wayland_protocols = dependency('wayland-protocols')
wayland_scanner = find_program('wayland-scanner')

protocols_dir = wayland_protocols.get_variable('pkgdatadir')
xdg_xml = protocols_dir / 'stable/xdg-shell/xdg-shell.xml'

xdg_client_header = custom_target(
  'xdg-shell-client-header',
  input: xdg_xml,
  output: 'xdg-shell-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)

xdg_client_code = custom_target(
  'xdg-shell-client-code',
  input: xdg_xml,
  output: 'xdg-shell-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

executable(
  'resize-torture',
  'main.c',
  xdg_client_header,
  xdg_client_code,
  dependencies: [wayland_client],
  install: true,
)
