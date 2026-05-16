# AV1/export cleanup plan

## Stable baseline to preserve

Current stable behavior:

- VP9/AV1 stream transition works.
- YouTube thumbnails are no longer grey/corrupt.
- Grey frame, flashing, stop-motion, and looping regressions are not present in the latest functional run.
- Small AV1 640x360 predecode sampleable exports are refused before returning a placeholder FD. Chrome logs this as `vaExportSurfaceHandle failed`, but the driver behavior is correct because returning that FD reintroduces grey thumbnails.

Do not "fix" the console noise by changing AV1 capability advertisement or by returning placeholder FDs as success. That would hide the symptom by weakening correct driver behavior.

Keep these core rules:

- `PredecodeBacking` may be returned only as non-presentable, unpublished, generation-zero backing with `may_be_sampled_by_client=0`.
- Sampleable exports must contain decoded pixels, pixel-proven seed pixels, or a valid transition hold.
- Placeholder pixels/predecode backing must upgrade after decode by copying decoded content into the exported FD, updating FD content generation, and exiting quarantine.
- Retained transition hold is valid only for previously visible, released, same-domain presentation content.

## Cleanup targets

### 1. Compile-gate and demote diagnostic VA negotiation telemetry

Current state:

- `src/va/config.cpp` has uncommitted trace-only changes for `config-query-profiles`, `config-query-entrypoints`, `config-get-attributes`, `config-create`, and `config-query-attributes`.
- `src/va/surfaces.cpp` has uncommitted trace-only changes for `surface-attrib-query`.
- `tests/smoke/generic/telemetry_patterns.py` currently expects those temporary events.

Why it is prototype residue:

- This telemetry was added only to prove that Chrome creates normal VA-memory 640x360 AV1 surfaces and immediately exports them before decode.
- That question is now answered by `/tmp/stream-shift61.log`.
- Keeping these events at normal trace level adds noise to every trace run and makes the telemetry smoke pin a temporary diagnostic surface.
- Keeping the logic permanently compiled into production builds adds avoidable runtime branches and trace argument construction in paths that are useful only while debugging driver/client negotiation.

Cleanup:

- Add an explicit build option, for example `-Dtrace_telemetry=true`, that compiles runtime diagnostic telemetry support into the driver.
- Default production-style builds should compile diagnostic trace support out so `VKVV_TRACE*` calls have no runtime effect and do not evaluate trace arguments.
- Keep the VA negotiation events available only when diagnostic telemetry is compiled in, and move high-volume negotiation events behind deep trace.
- Do not make the generic telemetry smoke require one-off negotiation events unless that smoke is running in a telemetry-enabled build.
- Keep correctness state and export behavior independent from telemetry compilation.

Validation:

- Re-run `smoke-telemetry-patterns` in a telemetry-enabled build.
- Re-run telemetry-off/on smoke coverage to prove disabled builds do not emit traces and enabled builds still honor runtime `VKVV_TRACE`.
- Re-run one browser transition/thumbnail trace and confirm existing `surface-create-request`, `surface-create-attrib`, `va-context-create`, `va-export-enter`, and `generic-export-summary` still provide enough evidence.

### 2. Remove the unsafe debug placeholder export path

Current state:

- `src/vulkan/export.cpp:235` exposes `VKVV_ALLOW_PLACEHOLDER_EXPORT`.
- `src/vulkan/export.cpp:1368` and `src/vulkan/export.cpp:1452` can return a placeholder through `debug_placeholder_export`.
- `src/vulkan/runtime_internal.h:58` and `src/vulkan/export/state.cpp:47` still carry `VkvvExportRole::DebugPlaceholder`.
- `tests/smoke/generic/telemetry_patterns.py` requires `debug-placeholder-export` and `VKVV_ALLOW_PLACEHOLDER_EXPORT`.

Why it is prototype residue:

- The entire stable fix depends on never returning unsafe placeholder pixels as a valid client-sampleable export.
- An environment-variable override that can re-enable that path is a footgun. It is useful during early diagnosis, but it should not remain in a correctness branch.

Cleanup:

- Delete `allow_placeholder_export()`.
- Delete `debug_placeholder_export` and the `debug-placeholder-export` trace branch.
- Delete `VkvvExportRole::DebugPlaceholder`.
- Remove telemetry-pattern requirements for `debug-placeholder-export` and `VKVV_ALLOW_PLACEHOLDER_EXPORT`.
- Make placeholder export refusal unconditional except for the explicit `PredecodeBacking` role.

Validation:

- Existing smokes must still prove that sampleable placeholder export is refused.
- Add or keep a smoke assertion that no successful export summary can report `decision=return-placeholder`.

### 3. Replace the size-based probe discriminator with an explicit predecode-export admission model

Current state:

- `src/vulkan/export.cpp` uses `predecode_no_seed_export_matches_compat_probe_policy()`.
- The rule is currently `surface <= 960x540` and `coded <= 960x544`.
- `src/vulkan/export.cpp:1366` rejects only no-seed predecode backing exports matching that size rule.
- `src/vulkan/export/shadow_image.cpp` has a similar `predecode_seed_target_matches_compat_probe_policy()` helper, currently used for telemetry.

Why it is prototype residue:

- The size threshold is doing real work, but it is still a Chrome/YouTube-shaped heuristic.
- It correctly separates the latest observed 640x360 thumbnail/probe failures from 3840x2160 stream bootstrap, but dimensions are not a VA/Vulkan correctness property.
- A future real low-resolution AV1 stream could look like a "probe" by size alone.

Cleanup direction:

- Introduce a named admission decision, for example:
  - `PredecodeBackingAdmission::ReturnQuarantinedBacking`
  - `PredecodeBackingAdmission::RejectUnsafeProbe`
  - `PredecodeBackingAdmission::RequireSeedOrDecodedPixels`
- Base the decision on explicit driver state, not hardcoded dimensions:
  - active decode domain
  - context lifecycle
  - whether the export belongs to a decode setup pool or an isolated probe
  - whether a valid seed or retained transition hold exists
  - whether the surface later receives `vaBeginPicture`/decode submit in the same domain
- If no non-size signal is concrete enough, keep the size rule temporarily but quarantine it in a single policy helper with a name that says it is a compatibility fallback, not a content-validity rule.

Required extra evidence before replacing it:

- Capture one stable 4K stream bootstrap and one 640x360 rejected probe with fields for:
  - context id
  - context size
  - context target count
  - surface creation order after `domain-note`
  - whether any surface in the same context reaches `vaBeginPicture`
  - surface destroy timing after failed export
- Do not replace the size rule until that evidence identifies a concrete non-size discriminator.

Validation:

- Real low-resolution playback must be tested if the size rule is removed.
- Existing scenarios must stay good: stream transition, thumbnails, grey-frame check.
- Trace invariant: no successful `PredecodeBacking` export may set `may_be_sampled_by_client=1`.

### 4. Stop using pixel proof as a production correctness dependency

Current state:

- `src/vulkan/export/shadow_image.cpp:223` requires `export_pixel_proof_enabled()` before a decoded source is considered safe for predecode seed copy.
- `predecode_seed_source_decoded_for_internal_copy()` therefore makes seed eligibility depend on debug/readback environment flags:
  - `VKVV_PIXEL_PROOF`
  - `VKVV_EXPORT_PIXEL_PROOF`
  - `VKVV_TRACE_PIXEL_PROOF`

Why it is prototype residue:

- Pixel proof is expensive diagnostic validation. It should verify invariants, not define production correctness.
- Production seed eligibility should come from structural state:
  - decoded content generation is nonzero
  - visible output was published
  - export shadow generation matches content generation
  - external release is satisfied
  - source is not private/nondisplay/predecode/quarantined/placeholder
  - same driver, stream, codec, format, fourcc, and coded extent

Cleanup:

- Split the current helper into:
  - `predecode_seed_source_structurally_valid()`
  - optional `predecode_seed_source_pixel_proof_valid()` used only when proof tracing is enabled.
- Use the structural helper for real seed admission.
- Keep pixel proof as debug-only telemetry/assertion that can reject in diagnostic builds if explicitly requested, but not as the default production gate.

Validation:

- Run smokes with pixel proof disabled and enabled.
- With proof disabled, seed flow should still work from structurally valid visible frames.
- With proof enabled, mismatched/black/zero proof should still be reported loudly.

### 5. Rename "placeholder" terminology where it now means "allocation-only backing"

Post-cleanup state:

- `VkvvExportPixelSource::Placeholder`
- `VkvvExportPresentSource::PredecodeBacking`
- `VkvvExportCopyReason::PredecodeBackingSeed`
- `ExportResource::neutral_backing`
- Trace strings like `allocation-only-backing`, `predecode-backing`, and `return-placeholder`.

Why it is prototype residue:

- Some of these names refer to an unsafe pixel source.
- Others now refer to a valid but non-presentable allocation lease.
- Mixing those meanings caused several wrong turns in this debugging cycle.

Cleanup:

- Keep `Placeholder` only for actual invalid placeholder pixels.
- Keep the valid allocation role/state named `PredecodeBacking`.
- Keep the initialized allocation-only state named `neutral_backing`, distinct from client-visible `Placeholder` pixels.
- Use `allocation-only-backing` where no pixels are valid for presentation.
- Keep old trace aliases only if needed for one release of tooling compatibility, then remove.

Validation:

- `generic-export-summary` should clearly separate:
  - `pixel_source=placeholder`
  - `export_role=predecode-backing`
  - `may_be_sampled_by_client=0`
- No trace should imply that a returned generation-zero backing is valid presentation content.

### 6. Consolidate the export gate into one role-aware decision function

Current state:

- `src/vulkan/export.cpp:1351-1424` computes export validity through many adjacent booleans:
  - `valid_decoded_pixels_available`
  - `valid_seed_available`
  - `valid_transition_hold_available`
  - `placeholder_available`
  - `predecode_backing_export`
  - `no_seed_predecode_backing`
  - `no_seed_predecode_probe_export`
  - `sampleable_placeholder_export`
  - `debug_placeholder_export`

Why it needs cleanup:

- The behavior is now stable, but the gate is difficult to audit.
- Adding one more condition here can easily reintroduce the old grey-thumbnail or stream-transition regression.

Cleanup:

- Create a small `ExportAdmissionDecision` struct:
  - `status`
  - `decision`
  - `role`
  - `reason`
  - `may_return_fd`
  - `may_be_sampled_by_client`
  - `fd_content_generation`
  - `requires_quarantine`
  - `requires_seed_upgrade`
- Move the role decision into one helper that returns this struct.
- Keep descriptor fill and fd export separate from admission.
- Make `trace_export_summary()` consume the decision struct instead of reconstructing the story from scattered booleans.

Validation:

- Unit/smoke coverage should assert the matrix:
  - decoded pixels -> success, sampleable, current content generation
  - pixel-proven seed -> success, sampleable, seed generation
  - transition hold -> success, sampleable, retained generation
  - predecode backing -> success only when admitted, not sampleable, generation zero
  - unsafe probe/sampleable placeholder -> fail, no FD

### 7. Tighten retained transition hold ownership

Current state:

- `export_resource_has_valid_retained_presentation()` and `export_resource_is_transition_hold_for_surface()` are the right direction.
- `TransitionHold` is intentionally sampleable because it represents previously visible, released content.

Cleanup:

- Keep the logic, but document it in code near the helper.
- Add one negative smoke case for each disqualifier that caused regressions:
  - predecode/quarantined retained backing
  - seeded retained backing
  - private nondisplay shadow
  - stale fd content generation
  - missing external release
- Ensure retained export pruning never keeps non-presentable `PredecodeBacking` as a transition hold.

Validation:

- `smoke-retained-export`.
- Browser stream transition after retained-pool churn.

### 8. Reduce telemetry volume after invariants are locked

Current state:

- The export path now emits many high-detail traces: gate state, proof state, seed candidate scan, quarantine lifecycle, fd lifetime, role lifecycle, AV1 display decisions.

Why it needs cleanup:

- The extra detail was necessary to get to root cause.
- Permanent trace should remain useful without making every browser run huge.

Cleanup:

- Keep invariant summaries:
  - `generic-export-summary`
  - `export-validity-gate`
  - `predecode-backing-export`
  - `predecode-backing-no-seed-probe-reject` or its renamed equivalent
  - `predecode-quarantine-enter/exit/outcome`
  - `export-role-lifecycle`
- Move high-cardinality scans and CRC details behind deep trace:
  - seed candidate lists
  - records strings
  - pixel CRCs
  - config/surface negotiation traces if kept
- Keep `telemetry_patterns.py` focused on required invariant fields, not one-off diagnostic events.

Validation:

- Compare one browser log before/after cleanup:
  - summary events still explain every export failure
  - log volume drops
  - no behavior change

## Suggested implementation phases

### Phase 0: Freeze and document the stable baseline

- Record the current commit and runtime flags used for the stable run.
- Preserve `/tmp/stream-shift60.log` and `/tmp/stream-shift61.log` as reference traces.
- Run:
  - `env CCACHE_DISABLE=1 meson compile -C build`
  - `env CCACHE_DISABLE=1 meson test -C build smoke-telemetry-patterns smoke-retained-export smoke-codec --print-errorlogs`
- Browser gate:
  - VP9 to AV1 transition
  - multiple thumbnail hovers
  - grey-frame/flash check

### Phase 1: Compile-gate diagnostic telemetry

- Add a Meson option for diagnostic trace telemetry.
- Make `VKVV_TRACE` and `VKVV_TRACE_DEEP` compile to no-ops when telemetry is disabled.
- Deep-gate the uncommitted `src/va/config.cpp` and `src/va/surfaces.cpp` traces.
- Update `tests/smoke/generic/telemetry_patterns.py`.
- Commit separately if behavior remains unchanged.

### Phase 2: Remove debug placeholder escape hatch

- Delete `VKVV_ALLOW_PLACEHOLDER_EXPORT` and `DebugPlaceholder`.
- Update smokes and telemetry patterns.
- Confirm no successful placeholder return path remains.

### Phase 3: Split structural validity from pixel proof

- Add structural seed-source validity.
- Make pixel proof diagnostic-only.
- Test with pixel proof disabled and enabled.

### Phase 4: Consolidate export admission

- Introduce `ExportAdmissionDecision`.
- Move the role matrix out of the inline boolean block.
- Keep existing behavior identical at first.

### Phase 5: Replace or quarantine the size-based probe rule

- Add only the telemetry needed to identify a non-size signal.
- If concrete signal exists, replace `predecode_no_seed_export_matches_compat_probe_policy()`.
- If no concrete signal exists, keep it as a named compatibility policy with tests and comments instead of pretending it is generic correctness.

### Phase 6: Rename placeholder/backing terminology

- Rename internal states and traces so unsafe placeholder pixels are not confused with valid non-presentable backing.
- Update smokes and trace parser expectations.

### Phase 7: Telemetry slimming

- Move any remaining high-volume debug traces to deep trace or diagnostic-only build blocks.
  - Seed candidate scans and pixel/CRC proof traces are deep-trace gated.
  - VA config/surface negotiation traces are deep-trace gated.
- Keep the invariant summary events.
- Re-run the browser scenarios and compare logs.

## Final acceptance gates

- Build passes.
- Focused smokes pass.
- `git diff --check` passes.
- Browser transition remains good.
- Thumbnails remain good.
- No grey, flashing, stop-motion, or looping regression.
- No successful `generic-export-summary` reports `decision=return-placeholder`.
- Any generation-zero successful export must have:
  - `export_role=predecode-backing`
  - `fd_content_gen=0`
  - `presentable=0`
  - `published_visible=0`
  - `may_be_sampled_by_client=0`
- Any sampleable successful export must be decoded, pixel-proven seed, or transition hold.
