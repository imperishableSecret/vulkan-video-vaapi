# NVIDIA Vulkan Video VA-API Prototype

This is a clean VA-API driver prototype for NVIDIA GPUs using Vulkan Video
decode. The goal is modern browser decode first: expose a small, codec-aware
VA-API driver that translates VA decode requests into Vulkan Video and exports
browser-importable dma-bufs.

This is not a fork of the NVDEC-backed driver design. The public boundary stays
as libva's C ABI, while the implementation is organized as C++23 modules for VA
objects, codec parsing, Vulkan sessions, surface resources, and dma-buf export.

## Current State

The driver currently builds and initializes as:

```text
NVIDIA Vulkan Video VA-API prototype 0.1.0
```

Live state on the current development machine:

```text
Vulkan runtime codecs: h264,vp9,av1
Vulkan profile caps: h264, h265, h265_10, h265_12, vp9, vp9_10, vp9_12, av1, av1_10
Export caps: NV12 and P010
Advertised VA decode profiles: H.264, VP9 Profile0, VP9 Profile2, AV1 Profile0
```

`vainfo` currently advertises:

- `VAProfileH264ConstrainedBaseline : VAEntrypointVLD`
- `VAProfileH264Main : VAEntrypointVLD`
- `VAProfileH264High : VAEntrypointVLD`
- `VAProfileVP9Profile0 : VAEntrypointVLD`
- `VAProfileVP9Profile2 : VAEntrypointVLD`
- `VAProfileAV1Profile0 : VAEntrypointVLD`

Important limits:

- HEVC is probed by Vulkan but not advertised yet because the HEVC parser,
  session-parameter builder, and decode command path are not implemented.
- AV1 Profile0 is advertised for the current 8-bit NV12 path. AV1 10-bit/P010
  is still hidden until the AV1 format selection and browser validation are
  complete.
- VP9 Profile2 uses the P010 path. 12-bit/P012 stays hidden because P012 export
  is not wired.
- Encode entrypoints are deliberately not advertised. The tree has structural
  hooks for a later encode path, but no encode runtime implementation.

## What Works

- C++23 VA-API driver module exporting `__vaDriverInit_1_0`.
- Runtime Vulkan Video probing through `libvulkan`.
- Runtime codec selection for H.264, VP9, and AV1 decode.
- H.264 8-bit NV12 browser decode path.
- VP9 Profile0 8-bit NV12 browser decode path.
- VP9 Profile2 10-bit P010 browser decode path.
- AV1 Profile0 8-bit NV12 browser decode path.
- Single-FD, two-layer dma-buf export for NV12 and P010.
- Export shadow images for NVIDIA's optimal-only decode images.
- Retained export backing logic for Chrome stream switches and imported
  external surface pools.
- Non-blocking decode completion path with pending work drained on sync/export.
- Smoke tests for codec capability, sessions, export lifecycle, retained export
  backing, VA lifetime, sync, multi-context, and import handling.

## Layout

```text
src/
  caps/                 VA profile, format, and config capability records
  codecs/               VA buffer parsers and codec operation registry
  codecs/{h264,vp9,av1} Codec-owned VA decode state
  va/                   libva entrypoints, objects, configs, contexts, surfaces
  vulkan/               runtime, command submission, sessions, resources, export
  vulkan/codecs/*       Vulkan Video codec session/decode implementations
tests/smoke/            Focused smoke tests for driver and codec behavior
docs/codecs/            Codec bring-up and completeness plans
docs/plans/             Driver-level cleanup, profile, split, and encode plans
```

## Build

Required development packages include libva, libva-drm, libdrm, Vulkan headers
and loader, GStreamer codecparsers, Meson, Ninja, and a C++23 compiler.

Default optimized build:

```sh
make all
```

Equivalent Meson commands:

```sh
meson setup build -Doptimization=2 -Ddebug=false -Db_ndebug=true
meson compile -C build
```

Experimental local-machine build:

```sh
make experimental
```

The experimental target uses `-O3` and `-march=native`. It is useful for local
testing, but the default build avoids machine-specific code generation.

## Test

Run the smoke suite:

```sh
make test
```

The Vulkan and VA export tests need access to the real Vulkan/DRM devices. If a
sandbox blocks device access, failures commonly look like `vkCreateInstance
failed` or missing `/dev/dri/renderD*` nodes rather than source failures.

Run the hardware capability probe:

```sh
./build/vkvv-probe
```

Run `vainfo` against the in-tree driver:

```sh
LIBVA_DRIVER_NAME=nvidia_vulkan \
LIBVA_DRIVERS_PATH="$PWD/build" \
VKVV_LOG=1 \
LIBVA_MESSAGING_LEVEL=2 \
vainfo
```

## Install

Install into the user's local libva driver directory:

```sh
make install
```

Override the install destination when needed:

```sh
make install PREFIX=/usr LIBVA_DRIVER_DIR=/usr/lib/dri
```

For local browser testing without installing:

```sh
LIBVA_DRIVER_NAME=nvidia_vulkan \
LIBVA_DRIVERS_PATH="$PWD/build" \
VKVV_LOG=1 \
google-chrome-stable
```

## Development Commands

```sh
make format-fix   # clang-format all src/ and tests/ C++ files
make tidy         # run clang-tidy using the Meson compile database
make clean        # clean Meson build outputs
```

## Pending Work

Near-term driver work:

- Keep hardening retained export backing and transition-window behavior so
  Chrome stream switches stay deterministic without holding excessive VRAM.
- Lower and tune detached/retained export memory caps after more transition
  testing at 1080p, 4K, NV12, and P010.
- Expand browser transition telemetry only where it catches real stale-frame,
  grey-frame, or imported-backing regressions.
- Build a repeatable browser sample matrix for H.264, VP9 SDR, VP9 HDR/P010,
  AV1 SDR, resolution changes, and codec switches.

Codec work:

- Complete VP9 hardening: show-existing frames, superframes, invisible
  references, alt-ref behavior, resolution/profile transitions, and broader
  sample coverage.
- Complete AV1 hardening: 10-bit/P010 enablement, film-grain policy, more tile
  and reference edge cases, and long-session browser validation.
- Add HEVC after VP9 and AV1 are stable: start with HEVC Main/NV12, then
  HEVC Main10/P010. Keep HEVC Main12/P012, 4:2:2, 4:4:4, and SCC hidden.
- Revisit H.264 completeness: interlaced/field pictures, cropping, SPS/PPS edge
  cases, POC behavior, and conformance-stream coverage.

Deferred work:

- P012 export and 12-bit decode profiles.
- Non-4:2:0 profile families.
- Real VA-API encode support. Encode descriptors and mode hooks are present
  only to avoid future refactors; encode entrypoints must remain hidden until a
  real encode path exists.
