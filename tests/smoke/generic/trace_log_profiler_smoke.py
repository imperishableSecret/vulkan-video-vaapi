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
nvidia-vulkan-vaapi: trace seq=15 event=export-stale-visible-nondisplay surface=7 driver=2 stream=1 codec=0x8
nvidia-vulkan-vaapi: trace seq=16 event=nondisplay-export-guard surface=7 driver=2 stream=1 codec=0x8 content_gen=2 shadow_gen=1 refresh_export=0 exported=1 shadow_exported=1 predecode_before=1 seeded_before=1 seed_source_before=6 action=cleared attempted_seed=0 attempted_copy=0
nvidia-vulkan-vaapi: trace seq=17 event=export-copy-publish-skip surface=7 driver=2 stream=1 codec=0x8
nvidia-vulkan-vaapi: trace seq=18 event=fence-wait slot=0 use=decode operation=VP9 decode timeout_ns=18446744073709551615 status=-4 wait_ns=4000
nvidia-vulkan-vaapi: trace seq=19 event=va-end-finish driver=2 target=7 status=1 decoded=0 pending=0
nvidia-vulkan-vaapi: trace seq=20 event=av1-frame-enter driver=2 ctx_stream=2 target=8 current_frame=2 order_hint=2 frame_type=1 show=0 hdr_existing=0 hdr_show=0 hdr_showable=1 refresh_export=0 refresh=0x01 primary_ref=0 depth=8 fourcc=0x3231564e bitstream=64 header=0 tiles=1
nvidia-vulkan-vaapi: trace seq=21 event=av1-decode-plan driver=2 ctx_stream=2 target=8 surface_stream=2 surface_codec=0x4 frame_type=1 show=0 hdr_existing=0 hdr_show=0 hdr_showable=1 refresh_export=0 current_frame=2 order_hint=2 primary_ref=0 refresh=0x01 depth=8 fourcc=0x3231564e refs=0 current_ref=1 setup=1 target_slot=1 used_mask=0x001 content_gen=0 shadow_gen=0 exported=0 last_display_gen=0 predecode=0
nvidia-vulkan-vaapi: trace seq=22 event=pending-submit slot=1 use=decode pending=1 operation=AV1 decode surface=8 driver=2 stream=2 codec=0x4 refresh_export=0 decoded=0 content_gen=0 shadow_mem=0x2 shadow_gen=0 predecode=0 upload_mem=2048
nvidia-vulkan-vaapi: trace seq=23 event=pending-complete-after use=decode operation=AV1 decode surface=8 status=0 refresh_export=0 decoded=1 content_gen=1 shadow_mem=0x2 shadow_gen=0 predecode=0 exported=0
nvidia-vulkan-vaapi: trace seq=24 event=av1-submit driver=2 ctx_stream=2 target=8 slot=1 refresh=0x01 refresh_export=0 hdr_existing=0 hdr_show=0 hdr_showable=1 depth=8 fourcc=0x3231564e refs=0 bytes=64 upload_mem=2048 session_mem=4096
nvidia-vulkan-vaapi: trace seq=25 event=av1-show-existing driver=2 ctx_stream=2 target=9 source=8 map_idx=1 slot=1 display_frame_id=0 target_gen=1 source_gen=1 refresh_export=1
nvidia-vulkan-vaapi: trace seq=26 event=av1-tile-submit-map scope=frame frame_seq=1 driver=2 stream=2 surface=8 codec=0x4 tile_count=1 tile_source=va-slice suspicious=1 ranges_inside_bitstream=1 ranges_overlap=0
nvidia-vulkan-vaapi: trace seq=27 event=av1-dpb-map-before-submit scope=frame frame_seq=1 driver=2 stream=2 surface=8 target_dpb_slot=1 references_valid=1 reference_count=0 codec=0x4
nvidia-vulkan-vaapi: trace seq=28 event=av1-dpb-map-after-submit scope=frame frame_seq=1 driver=2 stream=2 surface=8 target_dpb_slot=1 references_valid=1 reference_count=0 codec=0x4
nvidia-vulkan-vaapi: trace seq=29 event=av1-dpb-map-after-refresh scope=frame frame_seq=1 driver=2 stream=2 surface=8 target_dpb_slot=1 references_valid=1 reference_count=0 codec=0x4
nvidia-vulkan-vaapi: trace seq=30 event=av1-visible-frame-audit frame_seq=1 surface=8 stream=2 codec=0x4 order_hint=2 frame_type=1 show_frame=1 show_existing_frame=0 refresh_frame_flags=0x01 content_generation=1 tile_source=va-slice tile_count=1 tile_ranges_valid=1 tile_sum_size=64 setup_slot=1 target_dpb_slot=1 references_valid=1 reference_count=0 decode_crc_valid=1 decode_crc=0x1 published_path=exported-shadow published_crc_valid=1 published_crc=0x1 published_matches_decode=1 published_matches_previous_visible=0 output_published=1 failure_stage=none failure_reason=none
nvidia-vulkan-vaapi: trace seq=31 event=export-copy-proof codec=0x8 surface=7 source_surface=7 target_surface=7 source_content_gen=1 target_content_gen_before=0 target_content_gen_after=1 source_shadow_gen=0 target_shadow_gen_before=0 target_shadow_gen_after=1 copy_reason=visible-refresh refresh_export=1
nvidia-vulkan-vaapi: trace seq=32 event=visible-output-proof codec=0x8 surface=7 content_gen=1 order_hint_or_frame_num=1 published_path=exported-shadow published_gen=1 previous_visible_surface=4294967295 previous_visible_gen=0 published_matches_previous=0
nvidia-vulkan-vaapi: trace seq=33 event=export-seed-register codec=0x8 stream=1 source_surface=7 source_content_gen=1 source_shadow_gen=1 visible=1 refresh_export=1 published=1
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
    check(data["trace_records"] == 32, "trace record count mismatch")
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
    check(totals["stale_visible_nondisplay"] == 1, "stale visible nondisplay aggregate mismatch")
    check(totals["nondisplay_shadow_seeds"] == 0, "nondisplay shadow seed aggregate mismatch")
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
    check(codec["nondisplay_refresh_skips"] == 1 and codec["stale_visible_nondisplay"] == 1 and codec["nondisplay_shadow_seeds"] == 0, "codec nondisplay aggregate mismatch")
    stream = data["streams"][0]
    check(stream["codec"] == "vp9/0x8", "stream codec mismatch")
    check(stream["width"] == 3840 and stream["height"] == 2160, "stream size mismatch")
    check(stream["stale_drops"] == 1 and stream["predecode_stale_drops"] == 1, "stream stale drop mismatch")
    check(stream["decode_failed"] == 1 and stream["va_end_failed"] == 1, "stream failure mismatch")
    check(stream["nondisplay_refresh_skips"] == 1 and stream["stale_visible_nondisplay"] == 1 and stream["nondisplay_shadow_seeds"] == 0, "stream nondisplay mismatch")
    check(stream["export_copy_publish_skips"] == 1, "stream publish skip mismatch")
    av1_stream = data["streams"][1]
    check(av1_stream["codec"] == "av1/0x4", "AV1 stream codec mismatch")
    check(av1_stream["refresh_requested"] == 0 and av1_stream["stale_visible_nondisplay"] == 0, "hidden showable AV1 stream refreshed visible export state")
    check(av1_stream["av1_tile_submit_maps"] == 1 and av1_stream["av1_dpb_maps"] == 3 and av1_stream["av1_visible_audits"] == 1, "AV1 stream telemetry mismatch")
    check("driver_stale_drops=2" in text_result.stdout, "text stale drop aggregate missing")
    check("nondisplay_shadow_seeds=0" in text_result.stdout, "text nondisplay seed aggregate missing")
    check("av1_visible_audits=1" in text_result.stdout, "text AV1 audit aggregate missing")
    check("browser_dropped_frames_observed=0" in text_result.stdout, "text browser dropped-frame warning missing")
    check("live-summary path=-" in live_result.stderr, "live summary missing")
    check("trace-profile path=-" in live_result.stdout, "live final summary missing")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
