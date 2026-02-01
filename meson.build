project('audacious-projectm-gtk', 'cpp',
  version: '0.1',
  default_options: ['cpp_std=c++17'])

cc = meson.get_compiler('cpp')

aud = dependency('audacious', required: true)
gtk = dependency('gtk+-3.0', required: true)
epoxy_dep = dependency('epoxy', required: true)
projectm_core = dependency('projectM-4', required: true)
projectm_pl   = dependency('projectM-4-playlist', required: true)

src = files(
  'src/plugin.cc',
)

inc = include_directories('src')

# Where Audacious loads visualization plugins varies by distro/multiarch.
# Common: /usr/lib/<triplet>/audacious/Visualization
# We'll install under libdir/audacious/Visualization which matches typical packaging.
plugindir = join_paths(get_option('libdir'), 'audacious', 'Visualization')

shared_module('projectm-gtk',
  src,
  include_directories: inc,
  dependencies: [aud, gtk, epoxy_dep, projectm_core, projectm_pl],
  install: true,
  install_dir: plugindir
)
