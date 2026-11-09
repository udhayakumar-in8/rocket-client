project('rocket-client', 'c',
  version: '0.1.0',
  license: 'MIT',
  meson_version: '>= 0.62',
  default_options: ['warning_level=2', 'c_std=gnu11'])

app_id = 'io.github.udhayakumar_in8.RocketClient'

cc = meson.get_compiler('c')
add_project_arguments(cc.get_supported_arguments([
  '-Wno-unused-parameter',
  '-Wno-missing-field-initializers',
  '-Werror=implicit-function-declaration',
]), language: 'c')

deps = [
  dependency('gtk4', version: '>= 4.12'),
  dependency('libadwaita-1', version: '>= 1.5'),
  dependency('libsoup-3.0', version: '>= 3.0'),
  dependency('json-glib-1.0', version: '>= 1.6'),
]

conf = configuration_data()
conf.set_quoted('APP_ID', app_id)
conf.set_quoted('APP_VERSION', meson.project_version())
configure_file(output: 'config.h', configuration: conf)

executable('rocket-client',
  ['src/main.c', 'src/rc-client.c', 'src/rc-window.c'],
  dependencies: deps,
  include_directories: include_directories('.'),
  install: true)

install_data('data' / app_id + '.desktop',
  install_dir: get_option('datadir') / 'applications')
install_data('data' / app_id + '.svg',
  install_dir: get_option('datadir') / 'icons' / 'hicolor' / 'scalable' / 'apps')

test_client = executable('test-client',
  ['tests/test-client.c', 'src/rc-client.c'],
  dependencies: [dependency('libsoup-3.0'), dependency('json-glib-1.0')],
  include_directories: include_directories('.'))
test('client', test_client, env: ['no_proxy=*', 'NO_PROXY=*'])
