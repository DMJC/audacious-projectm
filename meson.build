project('audacious-projectm', 'cpp',
  version: '0.2',
  default_options: ['cpp_std=c++17'])

aud = dependency('audacious', required: true)
gtk = dependency('gtk+-3.0', required: true)
epoxy_dep = dependency('epoxy', required: true)
projectm_core = dependency('projectM-4', required: true)
projectm_pl = dependency('projectM-4-playlist', required: true)
qt6_widgets = dependency('qt6', modules: ['Widgets', 'Gui', 'OpenGLWidgets'], required: true)

inc = include_directories('src')
plugindir = join_paths(get_option('libdir'), 'audacious', 'Visualization')

shared_module('projectm-gtk',
  files('src/plugin.cc'),
  include_directories: inc,
  dependencies: [aud, gtk, epoxy_dep, projectm_core, projectm_pl],
  install: true,
  install_dir: plugindir
)

shared_module('projectm-qt6',
  files('src/plugin_qt6.cc'),
  include_directories: inc,
  dependencies: [aud, qt6_widgets, epoxy_dep, projectm_core, projectm_pl],
  install: true,
  install_dir: plugindir
)
