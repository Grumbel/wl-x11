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

# wlroots' pkg-config file name is versioned (e.g. wlroots-0.18.pc,
# wlroots-0.19.pc, ...) and the suffix changes with every wlroots major
# release, so hardcoding a version here goes stale quickly. Instead, ask
# pkg-config what's actually installed and use that name verbatim.
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
message('using wlroots pkg-config name: ' + wlroots_name)
wlroots = dependency(wlroots_name, required: true)

executable(
  'wl-x11',
  'src/main.c',
  dependencies: [wayland_server, wlroots, xkbcommon, pixman, xcb],
  install: true,
)
