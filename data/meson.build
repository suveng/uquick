desktop_conf = configuration_data()
desktop_conf.set('bindir', get_option('prefix') / get_option('bindir'))

desktop_file = configure_file(
  input: 'io.github.suveng.uquick.desktop.in',
  output: 'io.github.suveng.uquick.desktop',
  configuration: desktop_conf,
)

autostart_file = configure_file(
  input: 'uquick-daemon.desktop.in',
  output: 'uquick-daemon.desktop',
  configuration: desktop_conf,
)

install_data(desktop_file,
  install_dir: get_option('datadir') / 'applications')

install_data('io.github.suveng.uquick.metainfo.xml',
  install_dir: get_option('datadir') / 'metainfo')

install_data('icons/hicolor/scalable/apps/io.github.suveng.uquick.svg',
  install_dir: get_option('datadir') / 'icons/hicolor/scalable/apps')

install_data(autostart_file,
  install_dir: get_option('sysconfdir') / 'xdg/autostart')
