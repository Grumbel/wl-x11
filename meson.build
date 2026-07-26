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

# Prefer an already-built wlroots from pkg-config (Nix package or
# `ninja -C subprojects/wlroots/build install`). Fall back to the in-tree
# subproject for plain meson builds that have not installed it.
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

wlroots = dependency(wlroots_name, required: false)
if wlroots.found()
  message('using installed wlroots: ' + wlroots_name)
else
  fs = import('fs')
  if not fs.is_dir('subprojects/wlroots')
    error('wlroots not found via pkg-config and subprojects/wlroots is missing')
  endif
  message('using vendored subprojects/wlroots')
  wlroots_proj = subproject(
    'wlroots',
    default_options: [
      'examples=false',
      'tests=false',
      'backends=x11',
      'session=disabled',
      'xwayland=disabled',
      'renderers=gles2',
      'allocators=gbm',
      'color-management=disabled',
    ],
  )
  wlroots = wlroots_proj.get_variable('wlroots')
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
