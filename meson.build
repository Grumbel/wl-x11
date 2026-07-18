project(
  'wc-x11',
  'c',
  version: '0.1.0',
  default_options: [
    'c_std=gnu11',
    'warning_level=2',
    'werror=false',
  ],
)

add_project_arguments('-DWLR_USE_UNSTABLE', language: 'c')

wayland_server = dependency('wayland-server')
xkbcommon = dependency('xkbcommon')
pixman = dependency('pixman-1')
xcb = dependency('xcb')

# wlroots' pkg-config file name has historically been versioned
# (e.g. wlroots-0.18.pc). Try the versioned name first and fall back
# to the plain 'wlroots' name used by some distros/overlays.
wlroots = dependency('wlroots-0.18', required: false)
if not wlroots.found()
  wlroots = dependency('wlroots-0.19', required: false)
endif
if not wlroots.found()
  wlroots = dependency('wlroots', required: true)
endif

executable(
  'wc-x11',
  'src/main.c',
  dependencies: [wayland_server, wlroots, xkbcommon, pixman, xcb],
  install: true,
)
