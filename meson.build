# SPDX-FileCopyrightText: Copyright (c) 2026 Ingo Ruhnke <grumbel@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later

project(
  'wl-x11',
  'c',
  version: '0.1.0',
  default_options: [
    'c_std=gnu11',
    'warning_level=2',
    'werror=false',
  ],
)

# wl_listener callbacks all take a `void *data` parameter dictated by
# wlroots' signal API, even though many of our handlers don't need it --
# that's expected, not a bug, so don't warn about it.
add_project_arguments('-Wno-unused-parameter', language: 'c')

add_project_arguments('-DWLR_USE_UNSTABLE', language: 'c')

wayland_server = dependency('wayland-server')
xkbcommon = dependency('xkbcommon')
pixman = dependency('pixman-1')
xcb = dependency('xcb')
xcb_xfixes = dependency('xcb-xfixes')

# Prefer vendored subprojects/wlroots so backend/x11 is editable in-tree.
# (force_fallback_for is a meson *CLI* option, not a project() keyword.)
# Explicit subproject() always wins when the directory exists.
fs = import('fs')
if fs.is_dir('subprojects/wlroots')
  wlroots_proj = subproject(
    'wlroots',
    default_options: [
      'examples=false',
      'tests=false',
      'backends=x11',
      'session=disabled',
    ],
  )
  wlroots = wlroots_proj.get_variable('wlroots')
  message('using vendored subprojects/wlroots')
else
  # Fall back to system wlroots-*.pc
  pkgconfig_bin = find_program('pkg-config')
  wlroots_name = 'wlroots'
  list_result = run_command(pkgconfig_bin, '--list-all', check: false)
  if list_result.returncode() == 0
    foreach line : list_result.stdout().strip().split('\n')
      fields = line.strip().split(' ')
      if fields.length() > 0 and fields[0].startswith('wlroots')
        wlroots_name = fields[0]
      endif
    endforeach
  endif
  message('using system wlroots pkg-config name: ' + wlroots_name)
  wlroots = dependency(wlroots_name, required: true)
endif

wlx_src = files(
  'src/main.c',
  'src/x11.c',
  'src/move_resize.c',
  'src/output.c',
  'src/xdg.c',
  'src/input.c',
  'src/clipboard.c',
  'src/dnd.c',
)

executable(
  'wl-x11',
  wlx_src,
  dependencies: [wayland_server, wlroots, xkbcommon, pixman, xcb, xcb_xfixes],
  install: true,
)
