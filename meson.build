project(
  'nvidia-vulkan-vaapi',
  'cpp',
  version: '0.1.0',
  meson_version: '>=0.58.0',
  default_options: [
    'cpp_std=c++23',
    'warning_level=2',
  ],
)

cpp = meson.get_compiler('cpp')

libva_dep = dependency('libva')
libva_drm_dep = dependency('libva-drm')
libdrm_dep = dependency('libdrm')
vulkan_dep = dependency('vulkan')
exports_map = meson.current_source_dir() / 'src' / 'exports.map'

driver_sources = files(
  'src/driver.cpp',
  'src/va_buffers.cpp',
  'src/va_config.cpp',
  'src/va_context.cpp',
  'src/va_surfaces.cpp',
  'src/h264.cpp',
  'src/object.cpp',
  'src/vulkan_runtime.cpp',
  'src/vulkan_h264.cpp',
  'src/vulkan_probe.cpp',
)

shared_module(
  'nvidia_vulkan_drv_video',
  driver_sources,
  name_prefix: '',
  dependencies: [libva_dep, libdrm_dep, vulkan_dep],
  gnu_symbol_visibility: 'hidden',
  link_args: ['-Wl,--version-script=' + exports_map],
  link_depends: files('src/exports.map'),
  cpp_args: [
    '-D_GNU_SOURCE',
    '-DVKVV_VERSION="@0@"'.format(meson.project_version()),
    '-fno-gnu-unique',
    '-fvisibility-inlines-hidden',
  ],
  install: false,
)

executable(
  'vkvv-probe',
  files(
    'src/probe_main.cpp',
    'src/vulkan_probe.cpp',
  ),
  dependencies: [libva_dep, vulkan_dep],
  install: false,
)

executable(
  'vkvv-export-smoke',
  files(
    'src/export_smoke.cpp',
    'src/object.cpp',
    'src/vulkan_runtime.cpp',
    'src/vulkan_h264.cpp',
  ),
  dependencies: [libva_dep, libdrm_dep, vulkan_dep],
  install: false,
)

executable(
  'vkvv-lifetime-smoke',
  files('src/lifetime_smoke.cpp'),
  dependencies: [libva_dep, libva_drm_dep],
  install: false,
)
