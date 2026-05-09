project(
  'nvidia-vulkan-vaapi',
  'cpp',
  version: '0.1.0',
  meson_version: '>=0.58.0',
  default_options: [
    'cpp_std=c++23',
    'warning_level=2',
    'optimization=2',
    'debug=false',
    'b_ndebug=true',
  ],
)

cpp = meson.get_compiler('cpp')

libva_dep = dependency('libva')
libva_drm_dep = dependency('libva-drm')
libdrm_dep = dependency('libdrm')
vulkan_dep = dependency('vulkan')
threads_dep = dependency('threads')
gst_codecparsers_dep = dependency('gstreamer-codecparsers-1.0')
exports_map = meson.current_source_dir() / 'src' / 'exports.map'
src_inc = include_directories('src')

vulkan_runtime_sources = files(
  'src/telemetry.cpp',
  'src/va/surface_import.cpp',
  'src/vulkan/runtime.cpp',
  'src/vulkan/common.cpp',
  'src/vulkan/command.cpp',
  'src/vulkan/export.cpp',
  'src/vulkan/export/descriptor.cpp',
  'src/vulkan/export/formats.cpp',
  'src/vulkan/export/retained.cpp',
  'src/vulkan/export/shadow_image.cpp',
  'src/vulkan/formats.cpp',
  'src/vulkan/session.cpp',
  'src/vulkan/resources/surface.cpp',
  'src/vulkan/resources/upload.cpp',
  'src/vulkan/video_profile.cpp',
)

h264_vulkan_sources = files(
  'src/vulkan/codecs/h264/common.cpp',
  'src/vulkan/codecs/h264/decode.cpp',
  'src/vulkan/codecs/h264/parameters.cpp',
  'src/vulkan/codecs/h264/session.cpp',
)

hevc_vulkan_sources = files(
  'src/vulkan/codecs/hevc/common.cpp',
  'src/vulkan/codecs/hevc/decode.cpp',
  'src/vulkan/codecs/hevc/parameters.cpp',
  'src/vulkan/codecs/hevc/session.cpp',
)

vp9_vulkan_sources = files(
  'src/vulkan/codecs/vp9/common.cpp',
  'src/vulkan/codecs/vp9/decode.cpp',
  'src/vulkan/codecs/vp9/session.cpp',
)

av1_vulkan_sources = files(
  'src/vulkan/codecs/av1/common.cpp',
  'src/vulkan/codecs/av1/decode.cpp',
  'src/vulkan/codecs/av1/session.cpp',
)

driver_sources = files(
  'src/caps/records.cpp',
  'src/caps/formats.cpp',
  'src/codecs/registry.cpp',
  'src/va/driver.cpp',
  'src/va/domain.cpp',
  'src/va/buffers.cpp',
  'src/va/config.cpp',
  'src/va/context.cpp',
  'src/va/surfaces.cpp',
  'src/codecs/av1/av1.cpp',
  'src/codecs/h264/h264.cpp',
  'src/codecs/hevc/hevc.cpp',
  'src/codecs/vp9/vp9.cpp',
  'src/va/object.cpp',
  'src/vulkan/probe.cpp',
) + vulkan_runtime_sources + h264_vulkan_sources + hevc_vulkan_sources + vp9_vulkan_sources + av1_vulkan_sources

shared_module(
  'nvidia_vulkan_drv_video',
  driver_sources,
  name_prefix: '',
  include_directories: src_inc,
  dependencies: [libva_dep, libdrm_dep, vulkan_dep, threads_dep, gst_codecparsers_dep],
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
    'src/tools/probe_main.cpp',
    'src/vulkan/probe.cpp',
  ),
  include_directories: src_inc,
  dependencies: [libva_dep, vulkan_dep],
  install: false,
)

probe_smoke = executable(
  'vkvv-probe-smoke',
  files(
    'tests/smoke/generic/probe_smoke.cpp',
    'src/vulkan/probe.cpp',
  ),
  include_directories: src_inc,
  dependencies: [libva_dep, vulkan_dep],
  install: false,
)

codec_smoke = executable(
  'vkvv-codec-smoke',
  files(
    'tests/smoke/generic/codec_smoke.cpp',
    'src/caps/records.cpp',
    'src/caps/formats.cpp',
    'src/codecs/registry.cpp',
    'src/codecs/av1/av1.cpp',
    'src/codecs/h264/h264.cpp',
    'src/codecs/hevc/hevc.cpp',
    'src/codecs/vp9/vp9.cpp',
    'src/va/object.cpp',
  ) + vulkan_runtime_sources + h264_vulkan_sources + hevc_vulkan_sources + vp9_vulkan_sources + av1_vulkan_sources,
  include_directories: src_inc,
  dependencies: [libva_dep, libdrm_dep, vulkan_dep, threads_dep, gst_codecparsers_dep],
  install: false,
)

session_smoke = executable(
  'vkvv-session-smoke',
  files(
    'tests/smoke/h264/session_smoke.cpp',
    'src/va/object.cpp',
  ) + vulkan_runtime_sources + h264_vulkan_sources + vp9_vulkan_sources + av1_vulkan_sources,
  include_directories: src_inc,
  dependencies: [libva_dep, libdrm_dep, vulkan_dep, threads_dep],
  install: false,
)

hevc_session_smoke = executable(
  'vkvv-hevc-session-smoke',
  files(
    'tests/smoke/hevc/session_smoke.cpp',
    'src/va/object.cpp',
  ) + vulkan_runtime_sources + hevc_vulkan_sources,
  include_directories: src_inc,
  dependencies: [libva_dep, libdrm_dep, vulkan_dep, threads_dep],
  install: false,
)

vp9_session_smoke = executable(
  'vkvv-vp9-session-smoke',
  files(
    'tests/smoke/vp9/session_smoke.cpp',
    'src/va/object.cpp',
  ) + vulkan_runtime_sources + vp9_vulkan_sources,
  include_directories: src_inc,
  dependencies: [libva_dep, libdrm_dep, vulkan_dep, threads_dep],
  install: false,
)

av1_session_smoke = executable(
  'vkvv-av1-session-smoke',
  files(
    'tests/smoke/av1/session_smoke.cpp',
    'src/va/object.cpp',
  ) + vulkan_runtime_sources + av1_vulkan_sources,
  include_directories: src_inc,
  dependencies: [libva_dep, libdrm_dep, vulkan_dep, threads_dep],
  install: false,
)

export_smoke = executable(
  'vkvv-export-smoke',
  files(
    'tests/smoke/h264/export_smoke.cpp',
    'src/va/domain.cpp',
    'src/va/object.cpp',
  ) + vulkan_runtime_sources + h264_vulkan_sources + vp9_vulkan_sources + av1_vulkan_sources,
  include_directories: src_inc,
  dependencies: [libva_dep, libdrm_dep, vulkan_dep, threads_dep],
  install: false,
)

va_export_smoke = executable(
  'vkvv-va-export-smoke',
  files('tests/smoke/generic/va_export_smoke.cpp'),
  dependencies: [libva_dep, libva_drm_dep, libdrm_dep],
  install: false,
)

lifetime_smoke = executable(
  'vkvv-lifetime-smoke',
  files('tests/smoke/generic/lifetime_smoke.cpp'),
  dependencies: [libva_dep, libva_drm_dep, threads_dep],
  install: false,
)

multi_context_smoke = executable(
  'vkvv-multi-context-smoke',
  files('tests/smoke/generic/multi_context_smoke.cpp'),
  dependencies: [libva_dep, libva_drm_dep, threads_dep],
  install: false,
)

sync_smoke = executable(
  'vkvv-sync-smoke',
  files('tests/smoke/generic/sync_smoke.cpp'),
  dependencies: [libva_dep, libva_drm_dep, threads_dep],
  install: false,
)

capability_smoke = executable(
  'vkvv-capability-smoke',
  files('tests/smoke/generic/capability_smoke.cpp'),
  dependencies: [libva_dep, libva_drm_dep],
  install: false,
)

surface_import_smoke = executable(
  'vkvv-surface-import-smoke',
  files(
    'tests/smoke/generic/surface_import_smoke.cpp',
    'src/va/surface_import.cpp',
  ),
  include_directories: src_inc,
  dependencies: [libva_dep],
  install: false,
)

retained_export_smoke = executable(
  'vkvv-retained-export-smoke',
  files(
    'tests/smoke/generic/retained_export_smoke.cpp',
    'src/vulkan/export/retained.cpp',
  ),
  include_directories: src_inc,
  dependencies: [libva_dep, vulkan_dep],
  install: false,
)

driver_test_env = environment()
driver_test_env.set('LIBVA_DRIVER_NAME', 'nvidia_vulkan')
driver_test_env.set('LIBVA_DRIVERS_PATH', meson.current_build_dir())

test('smoke-session', session_smoke, is_parallel: false, timeout: 30)
test('smoke-hevc-session', hevc_session_smoke, is_parallel: false, timeout: 30)
test('smoke-vp9-session', vp9_session_smoke, is_parallel: false, timeout: 30)
test('smoke-av1-session', av1_session_smoke, is_parallel: false, timeout: 30)
test('smoke-probe', probe_smoke, is_parallel: false, timeout: 30)
test('smoke-codec', codec_smoke, is_parallel: false, timeout: 30)
test('smoke-export', export_smoke, is_parallel: false, timeout: 30)
test('smoke-va-export', va_export_smoke, env: driver_test_env, is_parallel: false, timeout: 30)
test('smoke-lifetime', lifetime_smoke, env: driver_test_env, is_parallel: false, timeout: 30)
test('smoke-multi-context', multi_context_smoke, env: driver_test_env, is_parallel: false, timeout: 30)
test('smoke-sync', sync_smoke, env: driver_test_env, is_parallel: false, timeout: 30)
test('smoke-capability', capability_smoke, env: driver_test_env, is_parallel: false, timeout: 30)
test('smoke-surface-import', surface_import_smoke, is_parallel: false, timeout: 30)
test('smoke-retained-export', retained_export_smoke, is_parallel: false, timeout: 30)
