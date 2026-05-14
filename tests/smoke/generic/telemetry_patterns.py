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

    direct_trace = []
    for path in vulkan_root.rglob("*.cpp"):
        text = path.read_text(encoding="utf-8")
        for number, line in enumerate(text.splitlines(), start=1):
            if re.search(r"\bvkvv_trace\(", line):
                direct_trace.append(f"{path.relative_to(root)}:{number}")
    if direct_trace:
        fail("direct vkvv_trace() remains in hot Vulkan sources:\n" + "\n".join(direct_trace))

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

    av1_decode = root / "src" / "vulkan" / "codecs" / "av1" / "decode.cpp"
    av1_text = av1_decode.read_text(encoding="utf-8")
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
    export_combined_text = export_text + "\n" + shadow_text + "\n" + export_state_text + "\n" + export_retained_text + "\n" + resource_text
    if '"av1-visible-output-check"' not in export_text:
        fail("AV1 visible output check trace is missing")
    for event in (
        '"av1-visible-output-published"',
        '"av1-visible-output-not-published"',
        '"av1-publish-fingerprint"',
        '"av1-visible-frame-identity"',
        '"av1-visible-frame-audit"',
        '"import-output-copy-enter"',
        '"import-output-copy-done"',
        '"import-output-release-barrier"',
        '"import-present-mark"',
        '"import-output-copy-failed"',
    ):
        if event not in export_text:
            fail(f"AV1 visible output publication trace is missing event {event}")
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
        '"invalid-thumbnail-predecode-seed"',
        '"predecode-seed-policy"',
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
    for field in (
        "show_frame=",
        "show_existing_frame=",
        "refresh_frame_flags=",
        "frame_to_show_map_idx=",
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
        "mutation_action=",
        "client_visible_shadow_mutated=0",
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
        "predecode-placeholder-seed",
        "visible-refresh",
        "visible-present-pin",
        "nondisplay-current-refresh",
        "nondisplay-current-refresh-unpinned",
        "private-shadow-refresh",
        "nondisplay-present-pinned-skip",
        "keep-placeholder",
        "reason=content-gen-zero",
        "decoded=0",
        "import-output",
        "previous_visible_surface=",
        "published_matches_previous=",
        "source_content_gen=",
        "target_content_gen_before=",
        "target_content_gen_after=",
        "source_shadow_gen=",
        "target_shadow_gen_before=",
        "target_shadow_gen_after=",
    ):
        if field not in export_combined_text:
            fail(f"export regression telemetry trace is missing field {field}")

    for toggle in (
        "VKVV_AV1_TRACE_PUBLICATION",
        "VKVV_AV1_DISABLE_IMPORTED_OUTPUT",
        "VKVV_AV1_FORCE_EXPORTED_SHADOW",
    ):
        if toggle not in export_text and toggle not in export_state_text and toggle not in shadow_text:
            fail(f"AV1 publication telemetry toggle is missing: {toggle}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
