project('uquick', 'c',
  version: '0.1.0',
  license: 'GPL-3.0-or-later',
  meson_version: '>= 0.62',
  default_options: [
    'warning_level=2',
    'c_std=gnu11',
    'buildtype=release',
    'optimization=3',
    'b_lto=true',
  ])

cc = meson.get_compiler('c')
gtk = dependency('gtk4', version: '>= 4.12')
gio_unix = dependency('gio-unix-2.0', version: '>= 2.72')
math = cc.find_library('m', required: false)

uquick_sources = files(
  'src/main.c',
  'src/core.c',
  'src/calc.c',
  'src/launch.c',
  'src/ui.c',
)

executable('uquick', uquick_sources,
  dependencies: [gtk, gio_unix, math],
  install: true,
  c_args: ['-ffunction-sections', '-fdata-sections', '-Wno-unused-parameter'],
  link_args: ['-Wl,--gc-sections', '-Wl,-O1', '-Wl,--as-needed'])

subdir('data')
