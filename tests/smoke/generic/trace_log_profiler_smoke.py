#!/usr/bin/env python3

import json
import subprocess
import sys
import tempfile
from pathlib import Path


FIXTURE = """\
noise
nvidia-vulkan-vaapi: trace seq=1 event=domain-note driver=2 stream=1 codec=0x8 width=3840 height=2160 rt=0x1 fourcc=0x30313050 surface=7
nvidia-vulkan-vaapi: trace seq=2 event=pending-submit slot=0 use=decode pending=1 operation=VP9 decode surface=7 driver=2 stream=1 codec=0x8 refresh_export=1 decoded=0 content_gen=0 shadow_mem=0x1 shadow_gen=0 predecode=0 upload_mem=1024
nvidia-vulkan-vaapi: trace seq=4 event=pending-complete-after use=decode operation=VP9 decode surface=7 status=0 refresh_export=1 decoded=1 content_gen=1 shadow_mem=0x1 shadow_gen=1 predecode=0 exported=1
nvidia-vulkan-vaapi: trace seq=5 event=fence-wait slot=0 use=decode operation=VP9 decode timeout_ns=18446744073709551615 status=0 wait_ns=2000
nvidia-vulkan-vaapi: trace seq=6 event=surface-resource-create surface=7 driver=2 stream=1 surface_codec=0x8 key_codec=0x8 resource_codec=0x8 extent=3840x2160 exportable=1 decode_mem=4096 shadow_mem=0x1 imported=0 import_fd_stat=0 import_fd_dev=0 import_fd_ino=0
nvidia-vulkan-vaapi: trace seq=7 event=export-image-create format=1 fourcc=0x30313050 extent=3840x2160 export_mem=8192 planes=2
nvidia-vulkan-vaapi: trace seq=8 event=video-session-memory bytes=16384 binds=2
nvidia-vulkan-vaapi: trace seq=9 event=va-export-return driver=2 surface=7 status=0 stream=1 codec=0x8 decoded=1
nvidia-vulkan-vaapi: trace seq=10 event=export-copy-metrics surface=7 driver=2 stream=1 codec=0x8 owner_copy=1 predecode_targets=1 copy_targets=2 copy_bytes=12288 wait_ns=3000
nvidia-vulkan-vaapi: trace seq=11 event=export-copy-done surface=7 driver=2 stream=1 codec=0x8 content_gen=1 shadow_mem=0x1 shadow_gen=1 seeded_targets=1 predecode=0 seeded=1
nvidia-vulkan-vaapi: trace seq=12 event=predecode-export-stale-drop surface=7 driver=2 stream=1 codec=0x8
nvidia-vulkan-vaapi: trace seq=13 event=export-seed-stale-drop surface=0 driver=0 stream=0 codec=0x0
nvidia-vulkan-vaapi: trace seq=14 event=export-refresh-skip-nondisplay surface=7 driver=2 stream=1 codec=0x8
nvidia-vulkan-vaapi: trace seq=15 event=nondisplay-export-guard surface=7 driver=2 stream=1 codec=0x8 content_gen=2 shadow_gen=1 refresh_export=0 exported=1 shadow_exported=1 predecode_before=1 seeded_before=1 seed_source_before=6 action=current-refresh-unpinned attempted_seed=0 attempted_copy=1 present_pinned=0 presentable=0 present_gen=0
nvidia-vulkan-vaapi: trace seq=16 event=export-copy-proof codec=0x8 surface=7 source_surface=7 target_surface=7 source_content_gen=2 target_content_gen_before=1 target_content_gen_after=2 source_shadow_gen=1 target_shadow_gen_before=1 target_shadow_gen_after=2 copy_reason=nondisplay-current-refresh refresh_export=0
nvidia-vulkan-vaapi: trace seq=17 event=nondisplay-export-current-refresh codec=0x8 surface=7 stream=1 driver=2 content_gen=2 old_shadow_gen=1 new_shadow_gen=2 exported=1 shadow_exported=1 predecode_before=1 seeded_before=1 predecode_after=0 seeded_after=0 attempted_seed=0 attempted_copy=1 display_published=0 present_pinned=0 presentable=0 copy_status=success
nvidia-vulkan-vaapi: trace seq=18 event=export-present-state action=nondisplay-current-refresh-unpinned surface=7 codec=0x8 stream=1 fd_dev=11 fd_ino=22 content_gen=2 shadow_gen=2 present_gen=0 presentable=0 present_pinned=0 published_visible=0 predecode=0 seeded=0 placeholder=0 refresh_export=0 display_visible=0 present_source=none client_visible_shadow=0
nvidia-vulkan-vaapi: trace seq=19 event=nondisplay-export-post-check codec=0x8 surface=7 refresh_export=0 content_gen=2 shadow_gen=2 shadow_stale=0 exported=1 shadow_exported=1 predecode=0 seeded=0 present_pinned=0 presentable=0 present_gen=0 action=current-refresh-unpinned
nvidia-vulkan-vaapi: trace seq=20 event=private-decode-shadow-copy-done surface=7 driver=2 stream=1 codec=0x8 content_gen=3 present_gen=2 present_shadow_gen=2 private_shadow_gen_before=0 private_shadow_gen_after=3 decode_shadow_gen=3 copy_reason=nondisplay-private-refresh copy_bytes=8192 wait_ns=5000
nvidia-vulkan-vaapi: trace seq=21 event=nondisplay-private-shadow-refresh surface=7 codec=0x8 stream=1 driver=2 content_gen=3 present_gen=2 present_shadow_gen=2 private_shadow_gen=3 decode_shadow_gen=3 refresh_export=0 exported=1 shadow_exported=1 present_pinned=1 presentable=1 client_visible_shadow_mutated=0 copy_status=success
nvidia-vulkan-vaapi: trace seq=22 event=export-copy-publish-skip surface=7 driver=2 stream=1 codec=0x8
nvidia-vulkan-vaapi: trace seq=23 event=fence-wait slot=0 use=decode operation=VP9 decode timeout_ns=18446744073709551615 status=-4 wait_ns=4000
nvidia-vulkan-vaapi: trace seq=24 event=va-end-finish driver=2 target=7 status=1 decoded=0 pending=0
nvidia-vulkan-vaapi: trace seq=25 event=av1-frame-enter driver=2 ctx_stream=2 target=8 current_frame=2 order_hint=2 frame_type=1 show=0 hdr_existing=0 hdr_show=0 hdr_showable=1 refresh_export=0 refresh=0x01 primary_ref=0 depth=8 fourcc=0x3231564e bitstream=64 header=0 tiles=1
nvidia-vulkan-vaapi: trace seq=26 event=av1-decode-plan driver=2 ctx_stream=2 target=8 surface_stream=2 surface_codec=0x4 frame_type=1 show=0 hdr_existing=0 hdr_show=0 hdr_showable=1 refresh_export=0 current_frame=2 order_hint=2 primary_ref=0 refresh=0x01 depth=8 fourcc=0x3231564e refs=0 current_ref=1 setup=1 target_slot=1 used_mask=0x001 content_gen=0 shadow_gen=0 exported=0 last_display_gen=0 predecode=0
nvidia-vulkan-vaapi: trace seq=27 event=pending-submit slot=1 use=decode pending=1 operation=AV1 decode surface=8 driver=2 stream=2 codec=0x4 refresh_export=0 decoded=0 content_gen=0 shadow_mem=0x2 shadow_gen=0 predecode=0 upload_mem=2048
nvidia-vulkan-vaapi: trace seq=28 event=pending-complete-after use=decode operation=AV1 decode surface=8 status=0 refresh_export=0 decoded=1 content_gen=1 shadow_mem=0x2 shadow_gen=0 predecode=0 exported=0
nvidia-vulkan-vaapi: trace seq=29 event=av1-submit driver=2 ctx_stream=2 target=8 slot=1 refresh=0x01 refresh_export=0 hdr_existing=0 hdr_show=0 hdr_showable=1 depth=8 fourcc=0x3231564e refs=0 bytes=64 upload_mem=2048 session_mem=4096
nvidia-vulkan-vaapi: trace seq=30 event=av1-show-existing driver=2 ctx_stream=2 target=9 source=8 map_idx=1 slot=1 display_frame_id=0 target_gen=1 source_gen=1 refresh_export=1
nvidia-vulkan-vaapi: trace seq=31 event=av1-tile-submit-map scope=frame frame_seq=1 driver=2 stream=2 surface=8 codec=0x4 tile_count=1 tile_source=va-slice suspicious=1 ranges_inside_bitstream=1 ranges_overlap=0
nvidia-vulkan-vaapi: trace seq=32 event=av1-dpb-map-before-submit scope=frame frame_seq=1 driver=2 stream=2 surface=8 target_dpb_slot=1 references_valid=1 reference_count=0 codec=0x4
nvidia-vulkan-vaapi: trace seq=33 event=av1-dpb-map-after-submit scope=frame frame_seq=1 driver=2 stream=2 surface=8 target_dpb_slot=1 references_valid=1 reference_count=0 codec=0x4
nvidia-vulkan-vaapi: trace seq=34 event=av1-dpb-map-after-refresh scope=frame frame_seq=1 driver=2 stream=2 surface=8 target_dpb_slot=1 references_valid=1 reference_count=0 codec=0x4
nvidia-vulkan-vaapi: trace seq=35 event=av1-visible-frame-audit frame_seq=1 surface=8 stream=2 codec=0x4 order_hint=2 frame_type=1 show_frame=1 show_existing_frame=0 refresh_frame_flags=0x01 content_generation=1 tile_source=va-slice tile_count=1 tile_ranges_valid=1 tile_sum_size=64 setup_slot=1 target_dpb_slot=1 references_valid=1 reference_count=0 decode_crc_valid=1 decode_crc=0x1 published_path=exported-shadow published_crc_valid=1 published_crc=0x1 published_matches_decode=1 published_matches_previous_visible=0 output_published=1 failure_stage=none failure_reason=none
nvidia-vulkan-vaapi: trace seq=36 event=export-copy-proof codec=0x8 surface=7 source_surface=7 target_surface=7 source_content_gen=1 target_content_gen_before=0 target_content_gen_after=1 source_shadow_gen=0 target_shadow_gen_before=0 target_shadow_gen_after=1 copy_reason=visible-refresh refresh_export=1
nvidia-vulkan-vaapi: trace seq=37 event=export-present-state action=visible-present-pin surface=7 codec=0x8 stream=1 fd_dev=11 fd_ino=22 content_gen=1 shadow_gen=1 present_gen=1 presentable=1 present_pinned=1 published_visible=1 predecode=0 seeded=0 placeholder=0 refresh_export=1 display_visible=1 present_source=visible-refresh client_visible_shadow=1
nvidia-vulkan-vaapi: trace seq=38 event=visible-output-proof codec=0x8 surface=7 content_gen=1 order_hint_or_frame_num=1 published_path=exported-shadow published_gen=1 previous_visible_surface=4294967295 previous_visible_gen=0 published_matches_previous=0
nvidia-vulkan-vaapi: trace seq=39 event=export-seed-register codec=0x8 stream=1 source_surface=7 source_content_gen=1 source_shadow_gen=1 visible=1 refresh_export=1 published=1
nvidia-vulkan-vaapi: trace seq=40 event=export-present-state action=nondisplay-private-shadow-refresh surface=7 codec=0x8 stream=1 fd_dev=11 fd_ino=22 content_gen=3 present_shadow_gen=2 private_shadow_gen=3 decode_shadow_gen=3 present_gen=2 presentable=1 present_pinned=1 published_visible=1 decode_shadow_private_active=1 predecode=0 seeded=0 placeholder=0 refresh_export=0 display_visible=0 present_source=visible-refresh mutation_action=none client_visible_shadow_mutated=0 client_visible_shadow=1 private_only=0
nvidia-vulkan-vaapi: trace seq=41 event=decode-shadow-coherence-check surface=7 driver=2 stream=1 codec=0x8 content_gen=3 refresh_export=0 display_visible=0 present_pinned=1 present_shadow_gen=2 private_shadow_gen=3 decode_shadow_gen=3 coherent=1 action=private-shadow-refresh
nvidia-vulkan-vaapi: trace seq=42 event=decode-shadow-coherence-check surface=7 driver=2 stream=1 codec=0x8 content_gen=1 refresh_export=1 display_visible=1 present_pinned=1 present_shadow_gen=1 private_shadow_gen=0 decode_shadow_gen=1 coherent=1 action=visible-refresh
nvidia-vulkan-vaapi: trace seq=43 event=predecode-quarantine-enter surface=7 driver=2 stream=1 codec=0x8 fd_dev=11 fd_ino=22 content_gen=0 presentable=0 published_visible=0 predecode_exported=1 predecode_quarantined=1
nvidia-vulkan-vaapi: trace seq=44 event=predecode-quarantine-exit surface=7 driver=2 stream=1 codec=0x8 fd_dev=11 fd_ino=22 content_gen=1 present_gen=1 release_done=1 predecode_quarantined=0
nvidia-vulkan-vaapi: trace seq=45 event=export-visible-acquire surface=7 driver=2 stream=1 codec=0x8 fd_dev=11 fd_ino=22 acquired_generation=0 acquire_required=0 acquire_done=1 acquire_mode=implicit-sync-only src_queue_family=4294967295 dst_queue_family=4294967295
nvidia-vulkan-vaapi: trace seq=46 event=export-visible-release surface=7 driver=2 stream=1 codec=0x8 fd_dev=11 fd_ino=22 content_gen=1 present_gen=0 old_layout=7 new_layout=1 src_queue_family=4294967295 dst_queue_family=4294967295 release_required=0 release_done=1 release_mode=implicit-sync-only
nvidia-vulkan-vaapi: trace seq=47 event=decode-pixel-proof surface=7 codec=0x8 stream=1 content_gen=1 order_hint_or_frame_num=1 decode_crc_valid=1 decode_crc=0xabc sample_bytes=4096
nvidia-vulkan-vaapi: trace seq=48 event=present-pixel-proof surface=7 codec=0x8 stream=1 content_gen=1 present_gen=1 present_shadow_crc_valid=1 present_shadow_crc=0xabc previous_present_crc=0x0 matches_decode=1 matches_previous=0 sample_bytes=4096
nvidia-vulkan-vaapi: trace seq=49 event=private-shadow-pixel-proof surface=7 codec=0x8 stream=1 content_gen=3 decode_crc_valid=1 decode_crc=0xdef private_shadow_crc_valid=1 private_shadow_crc=0xdef matches_decode=1 decode_sample_bytes=4096 private_sample_bytes=4096
nvidia-vulkan-vaapi: trace seq=50 event=visible-publish-gate surface=7 codec=0x8 stream=1 content_gen=1 display_visible=1 copy_done=1 present_shadow_gen=1 external_release_ok=1 pixel_match_ok=1 pixel_proof_required=1 publish_ok=1
nvidia-vulkan-vaapi: trace seq=51 event=visible-publish-blocked surface=7 codec=0x8 stream=1 content_gen=2 display_visible=1 copy_done=1 present_shadow_gen=2 external_release_ok=1 pixel_match_ok=0 pixel_proof_required=1 reason=pixel-mismatch
nvidia-vulkan-vaapi: trace seq=52 event=exported-fd-freshness-check surface=7 driver=2 stream=1 codec=0x8 fd_dev=11 fd_ino=22 content_gen=2 fd_content_gen=2 last_written_content_gen=2 may_be_sampled_by_client=1 detached_from_surface=0 refresh_export=0 display_visible=0 action=copied-to-export-fd
nvidia-vulkan-vaapi: trace seq=53 event=nondisplay-exported-fd-refresh surface=7 driver=2 stream=1 codec=0x8 content_gen=2 fd_content_gen_before=1 fd_content_gen_after=2 refresh_export=0 display_published=0 may_be_sampled_by_client=1 present_gen=1
nvidia-vulkan-vaapi: trace seq=54 event=predecode-export-policy surface=7 codec=0x8 stream=1 content_gen=0 pending_decode=0 policy=stream-local-last-visible action=stream-local-seed source_surface=6 source_present_gen=1 source_external_release_ok=1
nvidia-vulkan-vaapi: trace seq=55 event=predecode-export-policy surface=7 codec=0x8 stream=1 content_gen=0 pending_decode=0 policy=stream-local-last-visible action=neutral-placeholder source_surface=4294967295 source_present_gen=0 source_external_release_ok=0
nvidia-vulkan-vaapi: trace seq=56 event=export-validity-gate surface=7 driver=2 stream=1 codec=0x8 profile=0 width=3840 height=2160 fourcc=0x30313050 content_gen=0 decoded=0 pending_decode=0 refresh_export=0 display_visible=0 fd_already_exported=0 fd_dev=0 fd_ino=0 fd_content_gen=0 may_be_sampled_by_client=0 valid_decoded_pixels_available=0 valid_seed_available=0 placeholder_available=1 retained_candidate_available=0 decision=fail reason=no-valid-decoded-or-seed-pixels returned_fd=0 status=18
nvidia-vulkan-vaapi: trace seq=57 event=generic-export-summary surface=7 stream=1 codec=0x8 width=3840 height=2160 fourcc=0x30313050 content_gen=0 fd_content_gen=0 returned_fd=0 decision=fail pixel_source=placeholder pixel_proof_valid=0 is_black=1 is_zero=0 pending_decode=0 valid_seed_available=0 quarantine_outcome=pending external_release_mode=none status=18 may_be_sampled_by_client=0
nvidia-vulkan-vaapi: trace seq=58 event=predecode-quarantine-outcome surface=7 fd_dev=0 fd_ino=0 stream=1 codec=0x8 age_ms=0 content_gen=0 fd_content_gen=0 decoded=0 had_va_begin=0 had_decode_submit=0 had_visible_decode=0 may_be_sampled_by_client=0 outcome=export-failed
nvidia-vulkan-vaapi: trace seq=59 event=export-seed-candidate target_surface=7 candidate_surface=6 same_driver=1 same_stream=1 same_codec=1 same_fourcc=1 same_visible_extent=1 same_coded_extent=1 same_sequence_generation=0 same_session_generation=1 candidate_present_gen=1 candidate_fd_content_gen=1 candidate_external_release_ok=1 candidate_pixel_proof_valid=0 candidate_is_black=0 candidate_is_zero=0 candidate_valid=0 reject_reason=no-pixel-proof
nvidia-vulkan-vaapi: trace seq=60 event=export-validity-gate surface=7 driver=2 stream=1 codec=0x8 profile=0 width=3840 height=2160 fourcc=0x30313050 content_gen=1 decoded=1 pending_decode=0 refresh_export=1 display_visible=1 fd_already_exported=1 fd_dev=11 fd_ino=22 fd_content_gen=1 may_be_sampled_by_client=1 valid_decoded_pixels_available=1 valid_seed_available=0 placeholder_available=0 retained_candidate_available=0 decision=return-decoded reason=success returned_fd=1 status=0
nvidia-vulkan-vaapi: trace seq=61 event=returned-fd-pixel-proof surface=7 fd_dev=11 fd_ino=22 stream=1 codec=0x8 content_gen=1 fd_content_gen=1 pixel_source=decoded returned_crc=0xabc black_crc=0x10 zero_crc=0x0 is_black=0 is_zero=0 pixel_proof_valid=1 may_be_sampled_by_client=1 returned_fd=1 sample_bytes=8192 proof_enabled=1
nvidia-vulkan-vaapi: trace seq=62 event=generic-export-summary surface=7 stream=1 codec=0x8 width=3840 height=2160 fourcc=0x30313050 content_gen=1 fd_content_gen=1 returned_fd=1 decision=return-decoded pixel_source=decoded pixel_proof_valid=1 is_black=0 is_zero=0 pending_decode=0 valid_seed_available=0 quarantine_outcome=none external_release_mode=implicit-sync-only status=0 may_be_sampled_by_client=1
nvidia-vulkan-vaapi: trace seq=63 event=external-sync-proof surface=7 fd_dev=11 fd_ino=22 stream=1 codec=0x8 content_gen=1 copy_done=1 fence_waited=1 release_mode=implicit-sync-only release_required=0 release_done=1 acquire_required=0 acquire_done=0 old_layout=7 new_layout=1 src_queue_family=4294967295 dst_queue_family=4294967295 sync_fd=-1 semaphore_exported=0 present_crc_after_release=0xabc
nvidia-vulkan-vaapi: trace seq=64 event=decode-pixel-proof surface=7 codec=0x8 stream=1 content_gen=2 order_hint_or_frame_num=2 decode_crc_valid=1 decode_crc=0x10 black_crc=0x10 zero_crc=0x0 is_black=1 is_zero=0 pixel_proof_valid=0 sample_bytes=4096
[1:2:0512/000000.000000:ERROR:media/gpu/vaapi/vaapi_wrapper.cc:3552] vaEndPicture failed, VA error: operation failed
nvidia-vulkan-vaapi: device-lost call=vkWaitForFences operation=AV1 decode result=-4 decode_submitted=1 decode_completed=0
"""


def check(condition: bool, message: str) -> None:
    if not condition:
        print(message, file=sys.stderr)
        raise SystemExit(1)


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: trace_log_profiler_smoke.py <source-root>", file=sys.stderr)
        return 2

    root = Path(sys.argv[1])
    script = root / "tools" / "profile_trace_log.py"
    with tempfile.TemporaryDirectory() as tmp:
        log = Path(tmp) / "trace.log"
        log.write_text(FIXTURE, encoding="utf-8")
        result = subprocess.run([sys.executable, str(script), "--json", str(log)], text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
        text_result = subprocess.run([sys.executable, str(script), str(log)], text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
        stdin_result = subprocess.run([sys.executable, str(script), "--json", "-"], input=FIXTURE, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
        live_result = subprocess.run(
            [sys.executable, str(script), "--live", "--live-interval-lines", "5", "-"],
            input=FIXTURE,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
    if result.returncode != 0:
        print(result.stdout, file=sys.stderr)
        print(result.stderr, file=sys.stderr)
        return result.returncode
    if text_result.returncode != 0:
        print(text_result.stdout, file=sys.stderr)
        print(text_result.stderr, file=sys.stderr)
        return text_result.returncode
    if stdin_result.returncode != 0:
        print(stdin_result.stdout, file=sys.stderr)
        print(stdin_result.stderr, file=sys.stderr)
        return stdin_result.returncode
    if live_result.returncode != 0:
        print(live_result.stdout, file=sys.stderr)
        print(live_result.stderr, file=sys.stderr)
        return live_result.returncode

    data = json.loads(result.stdout)
    stdin_data = json.loads(stdin_result.stdout)
    totals = data["totals"]
    check(data["trace_records"] == 63, "trace record count mismatch")
    check(stdin_data["path"] == "-" and stdin_data["trace_records"] == data["trace_records"], "stdin trace profile mismatch")
    check(data["trace_sequence"]["missing"] == 1, "trace sequence gap mismatch")
    check(totals["streams"] == 2, "stream count mismatch")
    check(totals["decode_submitted"] == 2 and totals["decode_completed"] == 2 and totals["decode_failed"] == 1, "decode aggregate mismatch")
    check(totals["export_copy_targets"] == 2, "copy target aggregate mismatch")
    check(totals["export_copy_bytes"] == 12288 and totals["export_copy_wait_ns"] == 3000, "copy metric aggregate mismatch")
    check(totals["fence_waits"] == 2 and totals["fence_wait_ns"] == 6000, "fence metric aggregate mismatch")
    check(totals["decode_image_high_water"] == 4096, "decode image high-water mismatch")
    check(totals["export_image_high_water"] == 8192, "export image high-water mismatch")
    check(totals["video_session_high_water"] == 16384, "video session high-water mismatch")
    check(totals["driver_stale_drops"] == 2, "driver stale drop aggregate mismatch")
    check(totals["predecode_stale_drops"] == 1, "predecode stale drop aggregate mismatch")
    check(totals["export_seed_stale_drops"] == 1, "export seed stale drop aggregate mismatch")
    check(totals["nondisplay_refresh_skips"] == 1, "nondisplay refresh skip aggregate mismatch")
    check(totals["stale_visible_nondisplay"] == 0, "stale visible nondisplay aggregate mismatch")
    check(totals["nondisplay_shadow_seeds"] == 0, "nondisplay shadow seed aggregate mismatch")
    check(totals["nondisplay_current_refreshes"] == 1, "nondisplay current refresh aggregate mismatch")
    check(totals["nondisplay_private_shadow_refreshes"] == 1, "nondisplay private refresh aggregate mismatch")
    check(totals["private_decode_shadow_copies"] == 1, "private decode shadow copy aggregate mismatch")
    check(totals["private_decode_shadow_copy_bytes"] == 8192, "private decode shadow copy bytes mismatch")
    check(totals["private_decode_shadow_copy_wait_ns"] == 5000, "private decode shadow copy wait mismatch")
    check(totals["decode_shadow_coherence_checks"] == 2, "decode shadow coherence aggregate mismatch")
    check(totals["decode_shadow_incoherent_checks"] == 0, "decode shadow incoherent aggregate mismatch")
    check(totals["present_state_traces"] == 3, "present state trace aggregate mismatch")
    check(totals["visible_present_pins"] == 1, "visible present pin aggregate mismatch")
    check(totals["predecode_quarantine_enters"] == 1, "predecode quarantine enter aggregate mismatch")
    check(totals["predecode_quarantine_exits"] == 1, "predecode quarantine exit aggregate mismatch")
    check(totals["predecode_quarantine_destroys"] == 0, "predecode quarantine destroy aggregate mismatch")
    check(totals["export_visible_releases"] == 1, "export visible release aggregate mismatch")
    check(totals["export_visible_acquires"] == 1, "export visible acquire aggregate mismatch")
    check(totals["export_visible_release_missing"] == 0, "export visible release missing aggregate mismatch")
    check(totals["exported_fd_freshness_checks"] == 1 and totals["exported_fd_refreshes"] == 1, "exported fd freshness aggregate mismatch")
    check(totals["invalid_stale_exported_fds"] == 0, "invalid stale exported fd aggregate mismatch")
    check(totals["nondisplay_exported_fd_refreshes"] == 1, "nondisplay exported fd refresh aggregate mismatch")
    check(totals["predecode_export_policy_events"] == 2, "predecode export policy aggregate mismatch")
    check(totals["predecode_stream_local_seeds"] == 1 and totals["predecode_neutral_placeholders"] == 1, "predecode policy action aggregate mismatch")
    check(totals["export_validity_gates"] == 2 and totals["generic_export_summaries"] == 2, "generic export telemetry aggregate mismatch")
    check(totals["generic_export_placeholder_black_sampled"] == 0, "generic export placeholder aggregate mismatch")
    check(totals["generic_export_seed_invalid"] == 0 and totals["generic_export_decoded_invalid"] == 0, "generic export invalid proof aggregate mismatch")
    check(totals["returned_fd_pixel_proofs"] == 1 and totals["returned_fd_placeholder_black"] == 0, "returned fd pixel proof aggregate mismatch")
    check(totals["predecode_quarantine_placeholder_returns"] == 0 and totals["predecode_quarantine_timeouts"] == 0, "quarantine outcome aggregate mismatch")
    check(totals["external_sync_proofs"] == 1 and totals["external_sync_implicit"] == 1, "external sync aggregate mismatch")
    check(totals["decode_pixel_proofs"] == 2 and totals["present_pixel_proofs"] == 1 and totals["decode_pixel_black"] == 1, "visible pixel proof aggregate mismatch")
    check(totals["present_pixel_mismatches"] == 0, "present pixel mismatch aggregate mismatch")
    check(totals["private_shadow_pixel_proofs"] == 1 and totals["private_shadow_pixel_mismatches"] == 0, "private pixel proof aggregate mismatch")
    check(totals["pixel_proof_unavailable"] == 0, "pixel proof unavailable aggregate mismatch")
    check(totals["visible_publish_gates"] == 1 and totals["visible_publish_successes"] == 1, "visible publish gate aggregate mismatch")
    check(totals["visible_publish_blocks"] == 1 and totals["visible_publish_pixel_blocks"] == 1, "visible publish block aggregate mismatch")
    check(totals["nondisplay_present_pinned_skips"] == 0, "nondisplay present-pinned skip aggregate mismatch")
    check(totals["invalid_nondisplay_stale_export_shadows"] == 0, "invalid nondisplay stale shadow aggregate mismatch")
    check(totals["invalid_presentable_undecoded_surfaces"] == 0, "invalid undecoded presentable aggregate mismatch")
    check(totals["invalid_nondisplay_present_mutations"] == 0, "invalid nondisplay present mutation aggregate mismatch")
    check(totals["invalid_present_generations"] == 0, "invalid present generation aggregate mismatch")
    check(totals["invalid_visible_without_present_pins"] == 0, "invalid visible without present pin aggregate mismatch")
    check(totals["invalid_stale_private_decode_shadows"] == 0, "invalid stale private decode shadow aggregate mismatch")
    check(totals["invalid_visible_present_states"] == 0, "invalid visible present state aggregate mismatch")
    check(totals["invalid_thumbnail_predecode_seeds"] == 0, "invalid thumbnail predecode seed aggregate mismatch")
    check(totals["export_copy_publish_skips"] == 1, "export copy publish skip aggregate mismatch")
    check(totals["av1_tile_submit_maps"] == 1 and totals["av1_tile_suspicious"] == 1, "AV1 tile aggregate mismatch")
    check(totals["av1_dpb_maps"] == 3, "AV1 DPB aggregate mismatch")
    check(totals["av1_visible_audits"] == 1 and totals["av1_publish_failures"] == 0, "AV1 audit aggregate mismatch")
    check(data["events"].get("av1-submit") == 1, "AV1 submit event count mismatch")
    check(data["events"].get("av1-show-existing") == 1, "AV1 show-existing event count mismatch")
    check(data["browser_dropped_frames_observed"] is False, "browser dropped-frame observation mismatch")
    check(totals["device_lost"] == 1, "device-lost aggregate mismatch")
    check(totals["va_end_failed"] == 1, "VA end failure aggregate mismatch")
    check(totals["chrome_vaapi_errors"] == 1, "chrome error aggregate mismatch")
    codec = data["codecs"]["vp9/0x8"]
    check(codec["driver_stale_drops"] == 1 and codec["predecode_stale_drops"] == 1, "codec stale drop aggregate mismatch")
    check(
        codec["nondisplay_refresh_skips"] == 1
        and codec["stale_visible_nondisplay"] == 0
        and codec["nondisplay_shadow_seeds"] == 0
        and codec["nondisplay_current_refreshes"] == 1,
        "codec nondisplay aggregate mismatch",
    )
    check(
        codec["nondisplay_private_shadow_refreshes"] == 1
        and codec["private_decode_shadow_copies"] == 1
        and codec["decode_shadow_coherence_checks"] == 2
        and codec["decode_shadow_incoherent_checks"] == 0,
        "codec private decode shadow aggregate mismatch",
    )
    check(codec["visible_present_pins"] == 1 and codec["nondisplay_present_pinned_skips"] == 0, "codec present aggregate mismatch")
    check(codec["predecode_quarantine_enters"] == 1 and codec["predecode_quarantine_exits"] == 1, "codec predecode quarantine aggregate mismatch")
    check(codec["export_visible_releases"] == 1 and codec["export_visible_acquires"] == 1, "codec visible handoff aggregate mismatch")
    check(codec["exported_fd_freshness_checks"] == 1 and codec["nondisplay_exported_fd_refreshes"] == 1, "codec exported fd freshness mismatch")
    check(codec["predecode_stream_local_seeds"] == 1 and codec["predecode_neutral_placeholders"] == 1, "codec predecode policy mismatch")
    check(
        codec["export_validity_gates"] == 2
        and codec["generic_export_placeholder_black_sampled"] == 0
        and codec["generic_export_seed_invalid"] == 0
        and codec["generic_export_decoded_invalid"] == 0,
        "codec generic export aggregate mismatch",
    )
    check(codec["decode_pixel_proofs"] == 2 and codec["present_pixel_proofs"] == 1 and codec["private_shadow_pixel_proofs"] == 1, "codec pixel proof aggregate mismatch")
    check(codec["visible_publish_gates"] == 1 and codec["visible_publish_blocks"] == 1, "codec visible publish gate aggregate mismatch")
    stream = data["streams"][0]
    check(stream["codec"] == "vp9/0x8", "stream codec mismatch")
    check(stream["width"] == 3840 and stream["height"] == 2160, "stream size mismatch")
    check(stream["stale_drops"] == 1 and stream["predecode_stale_drops"] == 1, "stream stale drop mismatch")
    check(stream["decode_failed"] == 1 and stream["va_end_failed"] == 1, "stream failure mismatch")
    check(
        stream["nondisplay_refresh_skips"] == 1
        and stream["stale_visible_nondisplay"] == 0
        and stream["nondisplay_shadow_seeds"] == 0
        and stream["nondisplay_current_refreshes"] == 1,
        "stream nondisplay mismatch",
    )
    check(
        stream["nondisplay_private_shadow_refreshes"] == 1
        and stream["private_decode_shadow_copies"] == 1
        and stream["private_decode_shadow_copy_bytes"] == 8192
        and stream["private_decode_shadow_copy_wait_ns"] == 5000
        and stream["decode_shadow_coherence_checks"] == 2
        and stream["decode_shadow_incoherent_checks"] == 0,
        "stream private decode shadow mismatch",
    )
    check(stream["present_state_traces"] == 3 and stream["visible_present_pins"] == 1, "stream present state mismatch")
    check(stream["predecode_quarantine_enters"] == 1 and stream["predecode_quarantine_exits"] == 1, "stream predecode quarantine mismatch")
    check(stream["export_visible_releases"] == 1 and stream["export_visible_acquires"] == 1, "stream visible handoff mismatch")
    check(stream["exported_fd_freshness_checks"] == 1 and stream["exported_fd_refreshes"] == 1, "stream exported fd freshness mismatch")
    check(stream["nondisplay_exported_fd_refreshes"] == 1, "stream nondisplay exported fd refresh mismatch")
    check(stream["predecode_stream_local_seeds"] == 1 and stream["predecode_neutral_placeholders"] == 1, "stream predecode policy mismatch")
    check(
        stream["export_validity_gates"] == 2
        and stream["generic_export_summaries"] == 2
        and stream["returned_fd_placeholder_black"] == 0
        and stream["predecode_quarantine_placeholder_returns"] == 0,
        "stream generic export mismatch",
    )
    check(stream["decode_pixel_proofs"] == 2 and stream["present_pixel_proofs"] == 1 and stream["private_shadow_pixel_proofs"] == 1, "stream pixel proof mismatch")
    check(stream["visible_publish_gates"] == 1 and stream["visible_publish_successes"] == 1 and stream["visible_publish_blocks"] == 1, "stream publish gate mismatch")
    check(stream["nondisplay_present_pinned_skips"] == 0, "stream nondisplay present-pinned skip mismatch")
    check(stream["export_copy_publish_skips"] == 1, "stream publish skip mismatch")
    av1_stream = data["streams"][1]
    check(av1_stream["codec"] == "av1/0x4", "AV1 stream codec mismatch")
    check(av1_stream["refresh_requested"] == 0 and av1_stream["stale_visible_nondisplay"] == 0, "hidden showable AV1 stream refreshed visible export state")
    check(av1_stream["av1_tile_submit_maps"] == 1 and av1_stream["av1_dpb_maps"] == 3 and av1_stream["av1_visible_audits"] == 1, "AV1 stream telemetry mismatch")
    check("driver_stale_drops=2" in text_result.stdout, "text stale drop aggregate missing")
    check("nondisplay_shadow_seeds=0" in text_result.stdout, "text nondisplay seed aggregate missing")
    check("nondisplay_current_refreshes=1" in text_result.stdout, "text nondisplay current refresh aggregate missing")
    check("nondisplay_private_shadow_refreshes=1" in text_result.stdout, "text nondisplay private refresh aggregate missing")
    check("private_decode_shadow_copies=1" in text_result.stdout, "text private decode shadow copy aggregate missing")
    check("decode_shadow_coherence_checks=2" in text_result.stdout, "text decode shadow coherence aggregate missing")
    check("decode_shadow_incoherent_checks=0" in text_result.stdout, "text decode shadow incoherent aggregate missing")
    check("visible_present_pins=1" in text_result.stdout, "text visible present pin aggregate missing")
    check("predecode_quarantine_enters=1" in text_result.stdout, "text predecode quarantine enter aggregate missing")
    check("predecode_quarantine_exits=1" in text_result.stdout, "text predecode quarantine exit aggregate missing")
    check("export_visible_releases=1" in text_result.stdout, "text export visible release aggregate missing")
    check("export_visible_acquires=1" in text_result.stdout, "text export visible acquire aggregate missing")
    check("exported_fd_freshness_checks=1" in text_result.stdout, "text exported fd freshness aggregate missing")
    check("nondisplay_exported_fd_refreshes=1" in text_result.stdout, "text nondisplay exported fd refresh aggregate missing")
    check("predecode_stream_local_seeds=1" in text_result.stdout, "text predecode stream-local seed aggregate missing")
    check("export_validity_gates=2" in text_result.stdout, "text export validity gate aggregate missing")
    check("generic_export_placeholder_black_sampled=0" in text_result.stdout, "text generic export placeholder aggregate missing")
    check("generic_export_seed_invalid=0" in text_result.stdout, "text generic export seed invalid aggregate missing")
    check("generic_export_decoded_invalid=0" in text_result.stdout, "text generic export decoded invalid aggregate missing")
    check("returned_fd_placeholder_black=0" in text_result.stdout, "text returned fd placeholder aggregate missing")
    check("predecode_quarantine_placeholder_returns=0" in text_result.stdout, "text quarantine placeholder aggregate missing")
    check("external_sync_implicit=1" in text_result.stdout, "text external sync mode aggregate missing")
    check("decode_pixel_proofs=2" in text_result.stdout, "text decode pixel proof aggregate missing")
    check("decode_pixel_black=1" in text_result.stdout, "text decode black aggregate missing")
    check("present_pixel_proofs=1" in text_result.stdout, "text present pixel proof aggregate missing")
    check("private_shadow_pixel_proofs=1" in text_result.stdout, "text private pixel proof aggregate missing")
    check("visible_publish_gates=1" in text_result.stdout, "text visible publish gate aggregate missing")
    check("visible_publish_blocks=1" in text_result.stdout, "text visible publish block aggregate missing")
    check("nondisplay_present_pinned_skips=0" in text_result.stdout, "text nondisplay present-pinned skip aggregate missing")
    check("invalid_nondisplay_stale_export_shadows=0" in text_result.stdout, "text invalid nondisplay stale shadow aggregate missing")
    check("invalid_visible_without_present_pins=0" in text_result.stdout, "text invalid visible without present pin missing")
    check("invalid_stale_private_decode_shadows=0" in text_result.stdout, "text invalid stale private decode shadow missing")
    check("invalid_visible_present_states=0" in text_result.stdout, "text invalid visible present state missing")
    check("invalid_thumbnail_predecode_seeds=0" in text_result.stdout, "text invalid thumbnail predecode seed missing")
    check("av1_visible_audits=1" in text_result.stdout, "text AV1 audit aggregate missing")
    check("browser_dropped_frames_observed=0" in text_result.stdout, "text browser dropped-frame warning missing")
    check("live-summary path=-" in live_result.stderr, "live summary missing")
    check("trace-profile path=-" in live_result.stdout, "live final summary missing")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
