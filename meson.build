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
xcb_xinput = dependency('xcb-xinput')

# -------------------------------------------------------------------
# wlroots: patched tree under subprojects/wlroots (MODE before MapWindow,
# size-only WM_NORMAL_HINTS, wlr_x11_output_get_window, etc.).
#
# Prefer the subproject when present. If it is absent (Nix builds wlroots
# as packages.wlroots and strips subprojects/ from the compositor src),
# fall back to pkg-config — which must be the same patched build.
# Force pkg-config with -Duse_system_wlroots=true (meson_options.txt).
# -------------------------------------------------------------------
fs = import('fs')
use_system = get_option('use_system_wlroots')
have_vendor = fs.is_dir('subprojects/wlroots')

if use_system or not have_vendor
  if use_system and have_vendor
    warning('use_system_wlroots=true: ignoring vendored subprojects/wlroots')
  endif
  if not have_vendor and not use_system
    message('subprojects/wlroots not present; using pkg-config wlroots '
      + '(expected for Nix builds that supply a patched package)')
  endif
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
  wlroots = dependency(wlroots_name, required: true)
  message('using pkg-config wlroots: ' + wlroots_name)
else
  message('using vendored subprojects/wlroots (required for patches)')
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
  'src/subpresent.c',
  'src/input.c',
  'src/clipboard.c',
  'src/dnd.c',
)

executable(
  'wl-x11',
  wlx_src,
  dependencies: [wayland_server, wlroots, xkbcommon, pixman, xcb, xcb_xfixes, xcb_xinput],
  install: true,
)
