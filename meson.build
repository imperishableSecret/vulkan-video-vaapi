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
src_inc = include_directories('src')

vulkan_runtime_sources = files(
  'src/vulkan_runtime.cpp',
  'src/vulkan/common.cpp',
  'src/vulkan/command.cpp',
  'src/vulkan/export.cpp',
  'src/vulkan/session.cpp',
  'src/vulkan/surface_resource.cpp',
  'src/vulkan/upload.cpp',
)

h264_vulkan_sources = files(
  'src/vulkan/h264/common.cpp',
  'src/vulkan/h264/decode.cpp',
  'src/vulkan/h264/parameters.cpp',
  'src/vulkan/h264/session.cpp',
)

driver_sources = files(
  'src/driver.cpp',
  'src/va_buffers.cpp',
  'src/va_config.cpp',
  'src/va_context.cpp',
  'src/va_surfaces.cpp',
  'src/h264.cpp',
  'src/object.cpp',
  'src/vulkan_probe.cpp',
) + vulkan_runtime_sources + h264_vulkan_sources

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

session_smoke = executable(
  'vkvv-session-smoke',
  files(
    'tests/smoke/session_smoke.cpp',
    'src/object.cpp',
  ) + vulkan_runtime_sources + h264_vulkan_sources,
  include_directories: src_inc,
  dependencies: [libva_dep, libdrm_dep, vulkan_dep],
  install: false,
)

export_smoke = executable(
  'vkvv-export-smoke',
  files(
    'tests/smoke/export_smoke.cpp',
    'src/object.cpp',
  ) + vulkan_runtime_sources + h264_vulkan_sources,
  include_directories: src_inc,
  dependencies: [libva_dep, libdrm_dep, vulkan_dep],
  install: false,
)

lifetime_smoke = executable(
  'vkvv-lifetime-smoke',
  files('tests/smoke/lifetime_smoke.cpp'),
  dependencies: [libva_dep, libva_drm_dep],
  install: false,
)

multi_context_smoke = executable(
  'vkvv-multi-context-smoke',
  files('tests/smoke/multi_context_smoke.cpp'),
  dependencies: [libva_dep, libva_drm_dep],
  install: false,
)

driver_test_env = environment()
driver_test_env.set('LIBVA_DRIVER_NAME', 'nvidia_vulkan')
driver_test_env.set('LIBVA_DRIVERS_PATH', meson.current_build_dir())

test('smoke-session', session_smoke, is_parallel: false, timeout: 30)
test('smoke-export', export_smoke, is_parallel: false, timeout: 30)
test('smoke-lifetime', lifetime_smoke, env: driver_test_env, is_parallel: false, timeout: 30)
test('smoke-multi-context', multi_context_smoke, env: driver_test_env, is_parallel: false, timeout: 30)
