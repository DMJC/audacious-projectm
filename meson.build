project('audacious-projectm', 'cpp',
  version: '0.3',
  default_options: ['cpp_std=c++17'])

aud = dependency('audacious', required: true)
gtk = dependency('gtk+-3.0', required: true)
epoxy_dep = dependency('epoxy', required: true)
projectm_core = dependency('projectM-4', required: true)
projectm_pl = dependency('projectM-4-playlist', required: true)
qt6_widgets = dependency('qt6', modules: ['Widgets', 'Gui', 'OpenGLWidgets'], required: true)

inc = include_directories('src')
plugindir = join_paths(get_option('libdir'), 'audacious', 'Visualization')

shared_module('projectm',
  files('src/plugin.cc'),
  include_directories: inc,
  dependencies: [aud, gtk, qt6_widgets, epoxy_dep, projectm_core, projectm_pl],
  install: true,
  install_dir: plugindir
)
