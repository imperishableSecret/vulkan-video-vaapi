#!/usr/bin/env python3

from pathlib import Path
import re
import sys


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def main() -> int:
    root = Path(sys.argv[1])
    vulkan_root = root / "src" / "vulkan"
    meson_text = (root / "meson.build").read_text(encoding="utf-8")
    telemetry_header = (root / "src" / "telemetry.h").read_text(encoding="utf-8")
    telemetry_text = (root / "src" / "telemetry.cpp").read_text(encoding="utf-8")

    if "trace_telemetry" not in meson_text:
        fail("Meson trace telemetry compile option is missing")
    if "VKVV_ENABLE_TRACE_TELEMETRY=0" not in meson_text or "VKVV_ENABLE_TRACE_TELEMETRY=1" not in meson_text:
        fail("trace telemetry option must define the compile-time telemetry gate")
    if "VKVV_ENABLE_TRACE_TELEMETRY" not in telemetry_header or "inline constexpr bool vkvv_trace_compiled" not in telemetry_header:
        fail("telemetry header is missing the compile-time trace gate")
    if "#if VKVV_ENABLE_TRACE_TELEMETRY" not in telemetry_header or "#if VKVV_ENABLE_TRACE_TELEMETRY" not in telemetry_text:
        fail("trace telemetry must compile out both macros and runtime trace emitters")

    direct_trace = []
    for path in (root / "src").rglob("*.cpp"):
        if path.name == "telemetry.cpp":
            continue
        text = path.read_text(encoding="utf-8")
        for number, line in enumerate(text.splitlines(), start=1):
            if re.search(r"\bvkvv_trace\(", line):
                direct_trace.append(f"{path.relative_to(root)}:{number}")
    if direct_trace:
        fail("direct vkvv_trace() remains in hot driver sources:\n" + "\n".join(direct_trace))

    shadow = root / "src" / "vulkan" / "export" / "shadow_image.cpp"
    lines = shadow.read_text(encoding="utf-8").splitlines()
    for index, line in enumerate(lines):
        if "export_seed_records_string(" not in line:
            continue
        if line.lstrip().startswith("std::string export_seed_records_string("):
            continue
        window = "\n".join(lines[max(0, index - 4) : index + 1])
        if "vkvv_trace_deep_enabled()" not in window:
            fail(f"export_seed_records_string() is not deep-trace gated at {shadow.relative_to(root)}:{index + 1}")

    export_cpp = root / "src" / "vulkan" / "export.cpp"
    export_text = export_cpp.read_text(encoding="utf-8")
    if "decision.predecode_backing_export = decision.exact_predecode_pool_backing;" not in export_text:
        fail("exact-predecode pool exports must use the explicit predecode backing path")
    if "seed_predecode_export_from_last_good(runtime, resource, reason, reason_size)" not in export_text:
        fail("active predecode backing must try stream-local seed before returning allocation-only backing")
    if "fresh_unreturned_seed" not in export_text:
        fail("freshly seeded pre-return export shadows must count as valid seed pixels")
    if '"predecode-backing-no-seed-return"' not in export_text:
        fail("unseeded predecode backing returns need explicit role telemetry")
    if "predecode_no_seed_export_matches_compat_probe_policy" not in export_text or "Compatibility fallback" not in export_text:
        fail("probe-sized no-seed predecode exports must be quarantined in an explicit compatibility policy")
    if '"predecode-backing-no-seed-probe-reject"' not in export_text:
        fail("probe-sized no-seed predecode export rejection needs explicit telemetry")
    if '"predecode-backing-no-seed-defer"' in export_text:
        fail("unseeded predecode backing must not be blanket-deferred without a role discriminator")
    if "decision.sampleable_placeholder_export  = export_request_readable && decision.placeholder_available && !decision.predecode_backing_export;" not in export_text:
        fail("sampleable placeholder rejection must only exempt explicit predecode backing")
    if re.search(r"sampleable_placeholder_export\s*=\s*export_request_readable\s*&&\s*placeholder_available\s*;", export_text):
        fail("sampleable placeholder rejection still includes predecode backing")
    if "struct ExportAdmissionDecision" not in export_text or "decide_export_admission" not in export_text:
        fail("export admission role matrix must be centralized in an ExportAdmissionDecision helper")
    if "const VkvvExportRole returned_export_role = admission.role;" not in export_text:
        fail("returned export role must come from the centralized admission decision")
    shadow_text_for_lock = "\n".join(lines)
    if "target->content_generation        = source->content_generation;" not in shadow_text_for_lock:
        fail("predecode seed targets must inherit the source content generation after pixel-proofed copy")
    if "mark_export_visible_release(source, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL)" not in shadow_text_for_lock:
        fail("predecode seed targets must be externally released after the seed copy")
    if "exit_predecode_quarantine(nullptr, target, export_visible_release_satisfied(target))" not in shadow_text_for_lock:
        fail("predecode seed targets must leave quarantine once valid seed pixels are released")
    copy_wait_marker = 'const bool waited = wait_for_command_fence(runtime, std::numeric_limits<uint64_t>::max(), reason, reason_size, "surface export copy");'
    seed_proof_marker = 'trace_seed_pixel_proof(runtime, source, target, "success");'
    copy_wait_index = shadow_text_for_lock.find(copy_wait_marker)
    seed_proof_index = shadow_text_for_lock.find(seed_proof_marker)
    if copy_wait_index < 0 or seed_proof_index < 0:
        fail("surface export copy seed-proof lock-order guard could not find expected markers")
    if "command_lock.unlock();" not in shadow_text_for_lock[copy_wait_index:seed_proof_index]:
        fail("surface export copy can call seed pixel proof while command_mutex is still locked")

    av1_decode = root / "src" / "vulkan" / "codecs" / "av1" / "decode.cpp"
    av1_text = av1_decode.read_text(encoding="utf-8")
    va_context_text = (root / "src" / "va" / "context.cpp").read_text(encoding="utf-8")
    command_text = (root / "src" / "vulkan" / "command.cpp").read_text(encoding="utf-8")
    for event in (
        '"va-begin-bound"',
        '"va-render-buffer"',
        '"va-end-enter"',
        '"va-end-timing"',
        '"va-end-submitted"',
        '"va-call-gap"',
    ):
        if event not in va_context_text:
            fail(f"VA boundary timing telemetry is missing event {event}")
    for field in (
        "begin_monotonic_us=",
        "delta_since_last_begin_us=",
        "delta_since_last_end_us=",
        "render_monotonic_us=",
        "delta_since_previous_render_us=",
        "end_enter_monotonic_us=",
        "delta_since_last_render_us=",
        "end_submitted_monotonic_us=",
        "begin_to_end_enter_us=",
        "begin_to_end_submitted_us=",
        "end_total_us=",
        "prepare_us=",
        "configure_us=",
        "ensure_us=",
        "decode_submit_us=",
        "visible_drain_us=",
        "previous_call=",
        "current_call=",
        "gap_us=",
        "previous_monotonic_us=",
        "current_monotonic_us=",
        "active_stream=",
        "active_codec=",
        "surface_total=",
        "surface_decoded=",
        "surface_pending=",
        "surface_exported=",
        "surface_predecode_export=",
        "surface_import_external=",
        "active_surfaces=",
        "active_pending=",
        "retained_export_count=",
        "retained_export_bytes=",
    ):
        if field not in va_context_text:
            fail(f"VA boundary timing telemetry is missing field {field}")
    for field in (
        "pending_submit_monotonic_us=",
        "complete_call_monotonic_us=",
        "complete_wait_done_monotonic_us=",
        "pending_age_us=",
        "complete_wait_us=",
        "refresh_start_monotonic_us=",
        "complete_done_monotonic_us=",
        "pending_total_us=",
        "refresh_us=",
        "post_wait_us=",
    ):
        if field not in command_text:
            fail(f"pending-work timing telemetry is missing field {field}")
    if '"av1-display-decision"' not in av1_text:
        fail("AV1 display decision trace is missing")
    for field in (
        "show_frame=",
        "show_existing_frame=",
        "showable_frame=",
        "refresh_frame_flags=",
        "frame_to_show_map_idx=",
        "content_gen=",
        "shadow_gen=",
        "refresh_export=",
        "exported=",
        "shadow_exported=",
        "predecode=",
        "seeded=",
        "last_display_surface=",
        "last_display_gen=",
        "display_action=",
    ):
        if field not in av1_text:
            fail(f"AV1 display decision trace is missing field {field}")
    for event in (
        '"av1-tile-submit-map"',
        '"av1-tile-source-compare"',
        '"av1-dpb-map-before-submit"',
        '"av1-dpb-map-after-submit"',
        '"av1-dpb-map-after-refresh"',
        '"av1-decode-fingerprint"',
        '"av1-order-hint-state"',
        '"av1-picture-params"',
        '"av1-reference-params"',
    ):
        if event not in av1_text:
            fail(f"AV1 decode telemetry trace is missing event {event}")
    for field in (
        "frame_seq=",
        "tile_source=",
        "selection_reason=",
        "parser_used=",
        "parser_status=",
        "sum_final_tile_sizes=",
        "ranges_inside_bitstream=",
        "ranges_overlap=",
        "va_offset=",
        "parsed_offset=",
        "target_dpb_slot=",
        "setup_slot=",
        "references_valid=",
        "decode_combined_crc=",
        "order_hint_enabled=",
        "order_hint_bits=",
        "order_hint_modulus=",
        "previous_visible_order_hint=",
        "previous_visible_frame_seq=",
        "previous_visible_surface=",
        "visible_output=",
        "order_decreased=",
        "same_order_hint=",
        "wrap_candidate=",
        "keyframe_reset=",
        "classification=",
        "bitstream_hash=",
        "tile_hash=",
        "base_q_idx=",
        "skip_mode_present=",
        "segmentation_feature_data_hash=",
        "loop_filter_delta_enabled=",
        "cdef_enabled=",
        "restoration_enabled=",
        "reference_ref_idx=",
        "reference_frame_type=",
        "reference_showable=",
    ):
        if field not in av1_text:
            fail(f"AV1 decode telemetry trace is missing field {field}")

    av1_parser_text = (root / "src" / "codecs" / "av1" / "av1.cpp").read_text(encoding="utf-8")
    for toggle in (
        "VKVV_AV1_TILE_SOURCE",
        "VKVV_AV1_DISABLE_TILE_GROUP_SPLIT",
        "VKVV_AV1_DISABLE_SHOW_EXISTING_FASTPATH",
        "VKVV_AV1_TRACE_DPB",
        "VKVV_AV1_TRACE_TILES",
        "VKVV_AV1_FINGERPRINT_LEVEL",
    ):
        if toggle not in av1_parser_text and toggle not in av1_text:
            fail(f"AV1 telemetry toggle is missing: {toggle}")

    export_text = (root / "src" / "vulkan" / "export.cpp").read_text(encoding="utf-8")
    shadow_text = (root / "src" / "vulkan" / "export" / "shadow_image.cpp").read_text(encoding="utf-8")
    export_state_text = (root / "src" / "vulkan" / "export" / "state.cpp").read_text(encoding="utf-8")
    export_retained_text = (root / "src" / "vulkan" / "export" / "retained.cpp").read_text(encoding="utf-8")
    resource_text = (root / "src" / "vulkan" / "resources" / "surface.cpp").read_text(encoding="utf-8")
    runtime_internal_text = (vulkan_root / "runtime_internal.h").read_text(encoding="utf-8")
    export_combined_text = export_text + "\n" + shadow_text + "\n" + export_state_text + "\n" + export_retained_text + "\n" + resource_text
    for token in (
        "struct CodecVisibleOutputTrace",
        "bool        visible_output",
        "uint64_t    display_order",
        "const char* tile_or_slice_source",
        "CodecVisibleOutputTrace       visible_output_trace{}",
        "struct VisiblePublishCadence",
        "uint64_t                      display_order",
        "VisiblePublishCadence              visible_publish_cadence{}",
        "set_surface_visible_output_trace",
        "clear_surface_visible_output_trace",
        "surface_resource_has_visible_output_trace",
        "surface_resource_visible_output_expected",
        "surface_resource_visible_export_requires_copy",
    ):
        if token not in runtime_internal_text:
            fail(f"generic visible-output runtime metadata is missing token: {token}")
    for token in (
        "struct PendingDecodeTrace",
        "CodecVisibleOutputTrace visible{}",
        "uint32_t              reference_count",
        "PendingDecodeTrace          decode_trace{}",
        "const PendingDecodeTrace* decode_trace",
    ):
        if token not in runtime_internal_text:
            fail(f"generic pending decode metadata is missing token: {token}")
    pending_work_start = runtime_internal_text.find("struct PendingWork")
    pending_work_end = runtime_internal_text.find("struct CommandSlot", pending_work_start)
    pending_work_text = runtime_internal_text[pending_work_start:pending_work_end]
    if "Av1PendingDecodeTrace" in pending_work_text:
        fail("PendingWork must carry generic PendingDecodeTrace instead of AV1 trace directly")
    if "resource->visible_output_trace = trace;" not in export_state_text:
        fail("generic visible-output trace setter must update SurfaceResource metadata")
    if "clear_surface_visible_output_trace(resource);" not in export_combined_text and "clear_surface_visible_output_trace(resource);" not in av1_text:
        fail("visible-output lifecycle must clear generic visible-output metadata")
    for token in (
        "av1_visible_output_trace_valid",
        "av1_visible_show_frame",
        "av1_visible_show_existing_frame",
        "av1_visible_refresh_frame_flags",
        "av1_visible_frame_to_show_map_idx",
        "clear_surface_av1_visible_output_trace",
    ):
        if token in runtime_internal_text or token in export_combined_text or token in av1_text:
            fail(f"duplicated AV1 visible-output state remains: {token}")
    if "CodecVisibleOutputTrace visible{};" not in av1_text or "set_surface_visible_output_trace(resource, visible);" not in av1_text:
        fail("AV1 decode must populate generic visible-output metadata")
    for token in (
        "trace_visible_output_check(",
        "trace_publication_fingerprint(",
        "record_visible_publish_cadence(",
        "resource->visible_output_trace.visible_output",
        "runtime->visible_publish_cadence",
    ):
        if token not in export_text and token not in export_state_text:
            fail(f"visible publication must use generic metadata helper: {token}")
    for token in (
        "trace_av1_visible_output_check",
        "trace_av1_publication_fingerprint",
        "record_av1_visible_publish_cadence",
        "av1_visible_export_requires_copy",
    ):
        if token in export_combined_text or token in runtime_internal_text:
            fail(f"old AV1-specific export publication helper remains: {token}")
    if "predecode_seed_source_structurally_valid" not in shadow_text:
        fail("predecode seed admission must have a structural source-validity helper")
    if "return export_pixel_proof_enabled() && source != nullptr" in shadow_text:
        fail("predecode seed source admission still depends on pixel proof being enabled")
    if "predecode_seed_target_matches_compat_probe_policy(target) || !predecode_seed_source_decoded_for_internal_copy(source)" not in shadow_text:
        fail("compatibility-probe predecode targets must stay on the placeholder path even when a structural source exists")
    for forbidden in (
        "VKVV_ALLOW_PLACEHOLDER_EXPORT",
        "DebugPlaceholder",
        '"debug-placeholder-export"',
        "decision=return-placeholder returned_fd=1",
    ):
        if forbidden in export_combined_text:
            fail(f"unsafe debug placeholder export path remains: {forbidden}")
    if '"visible-output-check"' not in export_text:
        fail("visible output check trace is missing")
    for event in (
        '"visible-output-published"',
        '"visible-output-not-published"',
        '"visible-frame-cadence"',
        '"visible-publish-fingerprint"',
        '"visible-frame-identity"',
        '"visible-frame-audit"',
        '"import-output-copy-enter"',
        '"import-output-copy-done"',
        '"import-output-release-barrier"',
        '"import-present-mark"',
        '"import-output-copy-failed"',
    ):
        if event not in export_text:
            fail(f"visible output publication trace is missing event {event}")
    for event in (
        '"av1-visible-output-check"',
        '"av1-visible-output-published"',
        '"av1-visible-output-not-published"',
        '"av1-visible-frame-cadence"',
        '"av1-publish-fingerprint"',
        '"av1-visible-frame-identity"',
        '"av1-visible-frame-audit"',
    ):
        if event in export_text:
            fail(f"export publication trace must use codec-neutral event name, not {event}")
    for event in (
        '"nondisplay-export-guard"',
        '"nondisplay-export-current-refresh"',
        '"nondisplay-present-pinned-skip"',
        '"nondisplay-private-shadow-refresh"',
        '"nondisplay-export-post-check"',
        '"decode-shadow-coherence-check"',
        '"export-present-state"',
        '"invalid-nondisplay-export-mutation"',
        '"invalid-stale-private-decode-shadow"',
        '"invalid-presentable-undecoded-surface"',
        '"invalid-nondisplay-present-mutation"',
        '"invalid-present-generation"',
        '"invalid-visible-present-state"',
        '"invalid-visible-without-present-pin"',
        '"internal-import-shadow-publish-blocked"',
        '"import-output-resource-create"',
        '"import-output-resource-failed"',
        '"import-output-resource-skip"',
        '"import-output-copy-mark"',
        '"direct-import-output-gate"',
        '"predecode-seed-policy"',
        '"predecode-export-policy"',
        '"predecode-backing-gate"',
        '"predecode-backing-no-seed-return"',
        '"predecode-backing-no-seed-probe-reject"',
        '"predecode-seed-release"',
        '"predecode-backing-export"',
        '"transition-hold-export"',
        '"transition-hold-attach"',
        '"export-request-flags"',
        '"export-validity-gate"',
        '"export-drain-attempt"',
        '"export-seed-candidate-scan"',
        '"export-seed-candidate"',
        '"seed-source-pixel-proof"',
        '"seed-target-pixel-proof"',
        '"returned-fd-pixel-proof"',
        '"export-fd-lifetime"',
        '"export-role-lifecycle"',
        '"predecode-quarantine-outcome"',
        '"generic-export-summary"',
        '"external-sync-proof"',
        '"predecode-quarantine-enter"',
        '"predecode-quarantine-exit"',
        '"export-resource-destroy"',
        '"export-visible-release"',
        '"export-visible-acquire"',
        '"exported-fd-freshness-check"',
        '"invalid-stale-exported-fd"',
        '"nondisplay-exported-fd-refresh"',
        '"decode-pixel-proof"',
        '"present-pixel-proof"',
        '"private-shadow-pixel-proof"',
        '"pending-decode-pixel-proof"',
        '"av1-decode-pixel-proof"',
        '"av1-reference-pixel-proof"',
        '"av1-noop-candidate"',
        '"pixel-proof-unavailable"',
        '"visible-publish-gate"',
        '"visible-publish-blocked"',
        '"private-decode-shadow-create"',
        '"private-decode-shadow-copy-enter"',
        '"private-decode-shadow-copy-done"',
        '"export-late-present-shadow-preserved"',
        '"retained-role-mismatch-drop"',
        '"retained-present-shadow-attach"',
        '"export-copy-proof"',
        '"visible-output-proof"',
        '"export-seed-register"',
    ):
        if event not in export_combined_text:
            fail(f"export regression telemetry trace is missing event {event}")
    for event in (
        '"export-seed-candidate-scan"',
        '"export-seed-candidate"',
        '"seed-source-pixel-proof"',
        '"seed-target-pixel-proof"',
        '"returned-fd-pixel-proof"',
        '"decode-pixel-proof"',
        '"present-pixel-proof"',
        '"private-shadow-pixel-proof"',
        '"pending-decode-pixel-proof"',
        '"av1-decode-pixel-proof"',
        '"av1-reference-pixel-proof"',
        '"av1-noop-candidate"',
    ):
        if f"VKVV_TRACE_DEEP({event}" not in export_combined_text:
            fail(f"high-volume diagnostic trace must be deep-trace gated: {event}")
    for field in (
        "frame_sequence=",
        "display_order=",
        "visible_output=",
        "show_existing=",
        "refresh_flags=",
        "displayed_reference_index=",
        "tile_or_slice_source=",
        "refresh_export=",
        "content_gen=",
        "shadow_mem=",
        "shadow_gen=",
        "shadow_ok=",
        "shadow_published=",
        "import_external=",
        "import_present_generation=",
        "direct_import_ok=",
        "import_published=",
        "import_fd_dev=",
        "import_fd_ino=",
        "decode_image=",
        "import_image=",
        "copy_done=",
        "copy_status=",
        "attempted_copy=",
        "attempted_seed=",
        "display_published=",
        "layout_released=",
        "queue_family_released=",
        "exported=",
        "shadow_exported=",
        "result=",
        "published_path=",
        "published_combined_crc=",
        "published_matches_decode=",
        "published_matches_previous_visible=",
        "output_published=",
        "failure_stage=",
        "failure_reason=",
    ):
        if field not in export_text:
            fail(f"AV1 visible output check trace is missing field {field}")
    for field in (
        "attempted_seed=",
        "attempted_copy=",
        "attempted_private_copy=",
        "copy_reason=",
        "presentable=",
        "present_pinned=",
        "published_visible=",
        "present_gen=",
        "present_source=",
        "predecode_quarantined=",
        "predecode_generation=",
        "release_done=",
        "release_required=",
        "release_mode=",
        "acquire_required=",
        "acquire_done=",
        "acquire_mode=",
        "external_release_done=",
        "external_release_mode=",
        "fd_exported=",
        "fd_content_gen=",
        "may_be_sampled_by_client=",
        "export_role=",
        "detached_from_surface=",
        "last_written_content_gen=",
        "decode_crc=",
        "present_shadow_crc=",
        "private_shadow_crc=",
        "matches_decode=",
        "matches_previous=",
        "proof_source=",
        "pixel_matches_decode=",
        "pixel_matches_previous_visible=",
        "metadata_matches_decode=",
        "metadata_matches_previous_visible=",
        "decode_pixel_crc=",
        "present_pixel_crc=",
        "matches_previous_decode_pixel=",
        "matches_any_reference=",
        "matching_reference_index=",
        "matching_reference_surface=",
        "matching_reference_slot=",
        "target_pixel_crc=",
        "reference_index=",
        "reference_surface=",
        "reference_slot=",
        "reference_name=",
        "reference_ref_idx=",
        "reference_order_hint=",
        "reference_frame_type=",
        "reference_frame_id=",
        "reference_showable=",
        "reference_displayed=",
        "submitted_reference_content_gen=",
        "current_reference_content_gen=",
        "ref_crc_valid=",
        "ref_pixel_crc=",
        "matches_target=",
        "references_traced=",
        "publish_monotonic_us=",
        "delta_visible_us=",
        "fd_dev=",
        "fd_ino=",
        "previous_visible_valid=",
        "previous_visible_frame_sequence=",
        "previous_visible_display_order=",
        "previous_visible_content_gen=",
        "previous_visible_fd_dev=",
        "previous_visible_fd_ino=",
        "previous_visible_fd_content_gen=",
        "previous_visible_present_gen=",
        "previous_visible_pixel_crc=",
        "visible_frame_sequence_gap=",
        "visible_display_order_delta=",
        "same_fd_as_previous=",
        "same_surface_as_previous=",
        "published-pixel-mismatch",
        "published-metadata-fingerprint-mismatch",
        "sample_bytes=",
        "bitstream_hash=",
        "tile_hash=",
        "tile_sum_size=",
        "base_q_idx=",
        "skip_mode_present=",
        "segmentation_feature_data_hash=",
        "loop_filter_delta_enabled=",
        "cdef_enabled=",
        "restoration_enabled=",
        "matching_reference_name=",
        "matching_reference_ref_idx=",
        "external_release_ok=",
        "pixel_match_ok=",
        "pixel_proof_required=",
        "publish_ok=",
        "mutation_action=",
        "client_visible_shadow_mutated=0",
        "neutral_backing=",
        "present_shadow_gen=",
        "private_shadow_gen=",
        "decode_shadow_gen=",
        "coherent=",
        "decode_shadow_private_active=",
        "private_shadow_gen_before=",
        "private_shadow_gen_after=",
        "private_only=1",
        "nondisplay-private-refresh",
        "display_visible=",
        "mutation_action=skipped-client-shadow",
        "predecode-backing-seed",
        "visible-refresh",
        "visible-present-pin",
        "nondisplay-current-refresh",
        "nondisplay-current-refresh-unpinned",
        "private-shadow-refresh",
        "nondisplay-present-pinned-skip",
        "allocation-only-backing",
        "source-not-decoded",
        "source-not-published-seed",
        "source-domain-incomplete",
        "source-proof-stale",
        "source-proof-unavailable",
        "source-proof-invalid",
        "stream-local-seed",
        "failed-no-valid-source",
        "source-not-copyable",
        "seed-proof-failed",
        "delay-if-pending",
        "drained-and-exported",
        "shadow_published_would_be_internal=1",
        "block-private-shadow",
        "copy_target=1",
        "missing-vulkan-import-support",
        "incomplete-import",
        "exact-surface-predecode",
        "exact-surface-backing",
        "exact_surface=1",
        "export_flags=",
        "access_flags=",
        "readable=",
        "writable=",
        "read_only=",
        "read_write=",
        "separate_layers=",
        "composed_layers=",
        "sampleable_export=",
        "sampleable-placeholder-not-presentable",
        "decision=",
        "returned_fd=",
        "pixel_source=",
        "valid_decoded_pixels_available=",
        "valid_seed_available=",
        "placeholder_available=",
        "retained_candidate_available=",
        "pending_decode_found=",
        "drain_attempted=",
        "can_return_decoded_after_drain=",
        "candidate_count=",
        "valid_candidate_count=",
        "candidate_valid=",
        "reject_reason=",
        "source_crc=",
        "target_crc_after_copy=",
        "target_sample_bytes=",
        "target-does-not-match-source",
        "returned_crc=",
        "black_crc=",
        "zero_crc=",
        "is_black=",
        "is_zero=",
        "pixel_proof_valid=",
        "source_decode_proof_gen=",
        "source_decode_proof_valid=",
        "source_decode_content_valid=",
        "event_action=",
        "generation_at_action=",
        "age_ms=",
        "had_va_begin=",
        "had_decode_submit=",
        "had_visible_decode=",
        "outcome=",
        "present_crc_after_release=",
        "decoded=0",
        "sync_fd=",
        "semaphore_exported=",
        "fence_waited=",
        "import-output",
        "previous_visible_surface=",
        "published_matches_previous=",
        "source_content_gen=",
        "target_content_gen_before=",
        "target_content_gen_after=",
        "source_shadow_gen=",
        "target_shadow_gen_before=",
        "target_shadow_gen_after=",
        "refresh_start_monotonic_us=",
        "refresh_total_us=",
        "drain_us=",
        "copy_us=",
        "pixel_proof_us=",
        "publish_us=",
    ):
        if field not in export_combined_text:
            fail(f"export regression telemetry trace is missing field {field}")

    for toggle in (
        "VKVV_AV1_TRACE_PUBLICATION",
        "VKVV_AV1_DISABLE_IMPORTED_OUTPUT",
        "VKVV_AV1_FORCE_EXPORTED_SHADOW",
        "VKVV_PIXEL_PROOF",
        "VKVV_EXPORT_PIXEL_PROOF",
        "VKVV_TRACE_PIXEL_PROOF",
        "VKVV_TRACE_EXPORT_VALIDITY",
        "VKVV_TRACE_FD_LIFETIME",
        "VKVV_EXPORT_SYNC_MODE",
    ):
        if toggle not in export_text and toggle not in export_state_text and toggle not in shadow_text and toggle not in telemetry_text:
            fail(f"AV1 publication telemetry toggle is missing: {toggle}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
