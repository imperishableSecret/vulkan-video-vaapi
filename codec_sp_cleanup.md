# Codec-specific cleanup plan

This analysis treats "codex specific" as codec-specific cleanup. The goal is to move AV1-only export/publication mechanics that are now proven generally useful into generic export/runtime helpers, while keeping actual AV1 bitstream, DPB, tile, and order-hint semantics inside the AV1 codec module.

## Current issue

The recent AV1 transition and thumbnail fixes stabilized behavior, but some of the implementation now makes generic export code know too much about AV1:

- `SurfaceResource` stores AV1 publication fields directly: `av1_visible_output_trace_valid`, `av1_visible_show_frame`, `av1_visible_show_existing_frame`, `av1_visible_refresh_frame_flags`, `av1_frame_sequence`, `av1_order_hint`, tile/reference metadata, and AV1 fingerprints.
- `VulkanRuntime` stores `Av1VisiblePublishCadence`.
- `PendingWork` stores `Av1PendingDecodeTrace`.
- `src/vulkan/export.cpp` contains AV1-specific publication helpers: `trace_av1_visible_output_check()`, `trace_av1_publication_fingerprint()`, `record_av1_visible_publish_cadence()`, `visible_present_source()`, and AV1-only checks in `refresh_has_visible_display()`.
- `src/vulkan/export/state.cpp` exposes generic-sounding helpers that are actually AV1-gated: `surface_resource_requires_visible_publication()` and `av1_visible_export_requires_copy()`.
- `src/vulkan/export/shadow_image.cpp` has generic pixel-proof code that special-cases AV1 order hints for `order_hint_or_frame_num`.

This is now a maintainability risk: future VP9, H264, HEVC, or new codec work may need the same export/present lifecycle but would either duplicate AV1 logic or add more codec branches into generic export code.

## What should become generic

### 1. Visible-output metadata

Move the common visible-output contract out of AV1-named fields into a codec-neutral struct, for example:

```cpp
struct CodecVisibleOutputTrace {
    bool valid = false;
    bool visible_output = false;
    bool show_existing = false;
    uint64_t frame_sequence = 0;
    uint64_t display_order = 0;
    uint32_t frame_type = 0;
    uint32_t refresh_flags = 0;
    int32_t displayed_reference_index = -1;
    const char* tile_or_slice_source = "unknown";
};
```

AV1 should populate this from `show_frame`, `show_existing_frame`, `order_hint`, and `refresh_frame_flags`. Other codecs can populate it from frame number, POC, display flag, or just `content_generation` until they have richer metadata.

Generic consumers:

- `refresh_has_visible_display()`
- `visible_present_source()`
- visible publish/cadence tracing
- pixel proof labels that currently choose AV1 order hint vs content generation

AV1-only fields that should stay AV1-specific:

- raw AV1 `order_hint` interpretation
- `show_existing_frame` parser semantics
- `frame_to_show_map_idx`
- AV1 reference names, frame IDs, tile groups, segmentation, CDEF, restoration, and DPB map details

### 2. Publication/cadence state

Rename `Av1VisiblePublishCadence` to a generic `VisiblePublishCadence` and keep it on `VulkanRuntime` as a per-domain publication tracker.

The current fields are mostly codec-neutral:

- driver, stream, codec
- surface id
- frame sequence
- display order
- content generation
- fd identity
- fd content generation
- present generation
- pixel CRC

Only `order_hint` is AV1-specific. Replace it with `display_order` and let AV1 provide order hint as that value. This allows VP9/H264/HEVC to use frame number/POC/generation without teaching export code about each codec.

### 3. Visible publication policy helpers

`surface_resource_requires_visible_publication()` is currently AV1-only even though the name is generic. Split the policy into generic predicates:

- `surface_resource_has_visible_output_trace(resource)`
- `surface_resource_visible_output_expected(resource, refresh_export)`
- `surface_resource_requires_visible_publication(resource, refresh_export)`
- `surface_resource_visible_export_requires_copy(resource)`

The generic implementation should key off export state and the codec-neutral visible-output metadata. AV1 should no longer be hardcoded inside these helpers.

The important behavior to preserve:

- AV1 visible output must publish either exported shadow or imported output when `refresh_export=true`.
- Predecode/seed/neutral backing/retained/import-attached states still force a visible copy before publish.
- Non-display frames must not mutate the client-visible shadow.

### 4. Pending decode trace payload

`PendingWork` currently carries `Av1PendingDecodeTrace`. That makes the command queue generic type depend on AV1.

Replace it with a generic pending decode trace payload:

```cpp
struct PendingDecodeTrace {
    bool valid = false;
    CodecVisibleOutputTrace visible{};
    uint32_t reference_count = 0;
    uint32_t traced_reference_count = 0;
    // Optional codec-owned deep trace payload can remain AV1-only elsewhere.
};
```

Keep full AV1 parameter and reference detail in AV1 code, but pass generic visible/reference summary into `track_pending_decode()`. If AV1 still needs deep proof traces for references, keep an AV1-specific side helper invoked by AV1 code or behind a tagged optional payload, not as a required field on all pending work.

### 5. Pixel-proof and fingerprint helpers

The current pixel-proof helpers are generic in behavior but some field naming and trace labels are AV1-shaped.

Move toward generic helpers:

- `frame_identity_for_trace(resource)` returns `{frame_sequence, display_order, frame_type, tile_or_slice_source}`.
- `publish_metadata_fingerprint(resource)` hashes generic publication metadata.
- `trace_visible_publication_fingerprint()` emits codec-neutral fields and includes codec-specific extras only in deep trace.

Keep AV1-specific metadata fingerprint construction in AV1 or a codec callback. Generic export should not know that AV1's ordering field is called order hint.

## What should not be moved

Do not generalize these yet:

- AV1 tile parsing and tile-range validation.
- AV1 DPB/reference map traces.
- AV1 order-hint wrap classification.
- AV1 no-op candidate detection.
- AV1 picture/reference parameter traces.
- AV1 show-existing fast path internals.

Those are codec semantics, not generic export lifecycle logic.

## Implementation phases

### Phase 1: Introduce generic visible-output structs

- Add `CodecVisibleOutputTrace` and `VisiblePublishCadence` to `runtime_internal.h`.
- Add helpers to set/clear generic visible-output metadata.
- Keep AV1 field population in place initially, but also populate the generic struct.
- Update smokes to assert the generic fields exist without deleting AV1 fields yet.

Validation:

- `meson compile -C build`
- `smoke-telemetry-patterns`
- `smoke-export`
- `smoke-av1-session`

### Phase 2: Move export publication code to generic metadata

- Change `refresh_has_visible_display()`, `visible_present_source()`, `record_*_publish_cadence()`, and visible publish traces to read `CodecVisibleOutputTrace`.
- Rename AV1-specific export helpers:
  - `trace_av1_visible_output_check()` -> `trace_visible_output_check()`
  - `trace_av1_publication_fingerprint()` -> `trace_publication_fingerprint()`
  - `av1_visible_export_requires_copy()` -> `surface_resource_visible_export_requires_copy()`
- Keep trace event names stable if browser-log comparability matters, or add one transition alias.

Validation:

- Existing AV1 browser scenario: VP9 to AV1 transition, thumbnails, grey-frame check.
- Ensure no successful generation-zero export is sampleable.

### Phase 3: Replace AV1 pending payload in command queue

- Replace `Av1PendingDecodeTrace` in `PendingWork` with `PendingDecodeTrace`.
- Make `track_pending_decode()` accept generic pending metadata.
- Have AV1 fill the generic payload from its richer local data.
- Keep AV1 deep reference/pixel proof traces inside AV1/export proof code without making `PendingWork` AV1-owned.

Validation:

- `smoke-av1-session`
- Any smoke covering pending decode timing/proof traces.
- Browser stream transition, because this touches async decode completion metadata.

### Phase 4: Remove duplicated AV1 fields from `SurfaceResource`

- After generic consumers are migrated, remove or reduce:
  - `av1_visible_output_trace_valid`
  - `av1_visible_show_frame`
  - `av1_visible_show_existing_frame`
  - `av1_visible_refresh_frame_flags`
  - generic-use parts of `av1_frame_sequence`, `av1_order_hint`, and `av1_tile_source`
- Keep AV1-only audit fields only where AV1 traces still require them.
- Rename clear helpers from `clear_surface_av1_visible_output_trace()` to `clear_surface_visible_output_trace()`.

Validation:

- Build with telemetry enabled and disabled.
- `smoke-telemetry-patterns`
- `smoke-trace-log-profiler`
- `smoke-export smoke-av1-session smoke-retained-export`

### Phase 5: Update documentation and trace vocabulary

- Update telemetry pattern checks to require generic helper names.
- Keep AV1-specific trace events only in AV1 decode files.
- Keep generic export trace fields codec-neutral: `frame_sequence`, `display_order`, `visible_output`, `show_existing`, `published_path`, `fd_content_gen`.

Validation:

- Compare one browser log before/after:
  - generic export summary still explains failures
  - AV1 deep traces still identify AV1-specific decode problems
  - normal trace volume does not grow

## Acceptance criteria

- No generic export/runtime file needs to inspect `VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR` for visible publication behavior.
- `SurfaceResource` has one generic visible-output metadata struct rather than scattered AV1 visible-output fields.
- `PendingWork` is codec-neutral.
- AV1 code remains responsible for AV1 semantics and only exports generic publication metadata.
- Existing stable behavior remains intact: stream transition, thumbnails, grey frames, flashing, stop-motion, and looping regressions stay fixed.

