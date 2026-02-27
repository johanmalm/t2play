project(
  't2play',
  'c',
  version: '0.0.0',
  default_options: [
    'c_std=c11',
    'warning_level=2',
  ],
)

add_project_arguments(
  [
    '-D_POSIX_C_SOURCE=200809L',
    '-Wno-unused-parameter',
    '-Wshadow',
    '-DLABWC_VERSION="0.0.0"',
  ],
  language: 'c',
)

cyaml = dependency('libcyaml')
cairo = dependency('cairo')
pango = dependency('pango')
pangocairo = dependency('pangocairo')
glib = dependency('glib-2.0')
wayland_client = dependency('wayland-client')
wayland_cursor = dependency('wayland-cursor')
wayland_protos = dependency('wayland-protocols', version: '>=1.24')
wayland_scanner_dep = dependency('wayland-scanner', native: true)
wlroots = dependency('wlroots-0.19')

sources = files(
  'main.c',
  'pool.c',
  'util.c',

  'plugin-taskbar.c',
)

wl_protocol_dir = wayland_protos.get_variable('pkgdatadir')
wayland_scanner = find_program(
  wayland_scanner_dep.get_variable('wayland_scanner'),
  native: true,
)

protocols = [
  wl_protocol_dir / 'stable/tablet/tablet-v2.xml',
  wl_protocol_dir / 'stable/xdg-shell/xdg-shell.xml',
  wl_protocol_dir / 'staging/cursor-shape/cursor-shape-v1.xml',
  'p/wlr-layer-shell-unstable-v1.xml',
  'p/wlr-foreign-toplevel-management-unstable-v1.xml',
]

foreach xml : protocols
  sources += custom_target(
    xml.underscorify() + '_c',
    input: xml,
    output: '@BASENAME@-protocol.c',
    command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
  )
  sources += custom_target(
    xml.underscorify() + '_client_h',
    input: xml,
    output: '@BASENAME@-client-protocol.h',
    command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
  )
endforeach

executable(
  meson.project_name(),
  sources,
  dependencies: [
    cairo,
    cyaml,
    pango,
    pangocairo,
    glib,
    wayland_client,
    wayland_cursor,
    wlroots,
  ],
)
