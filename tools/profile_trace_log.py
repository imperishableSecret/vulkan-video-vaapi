#!/usr/bin/env python3

import argparse
import json
import re
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, TextIO


TRACE_RE = re.compile(r"nvidia-vulkan-vaapi: trace seq=(?P<seq>\d+) event=(?P<event>\S+)(?: (?P<body>.*))?$")
NVIDIA_RE = re.compile(r"nvidia-vulkan-vaapi: (?P<kind>device-lost)(?: (?P<body>.*))?$")
CHROME_ERROR_RE = re.compile(r"\[[^\]]+:ERROR:(?P<source>[^\]]+)\] (?P<message>.*)$")
KEY_VALUE_RE = re.compile(r"(?P<key>[A-Za-z_][A-Za-z0-9_]*)=(?P<value>.*?)(?=\s+[A-Za-z_][A-Za-z0-9_]*=|$)")
BARE_SIZE_RE = re.compile(r"(?:^|\s)(?P<width>\d+)x(?P<height>\d+)(?:\s|$)")


CODECS = {
    0x1: "h264",
    0x2: "hevc",
    0x4: "av1",
    0x8: "vp9",
}


def parse_fields(body: str | None) -> dict[str, str]:
    fields: dict[str, str] = {}
    if not body:
        return fields
    for match in KEY_VALUE_RE.finditer(body):
        value = match.group("value").strip()
        if len(value) >= 2 and value[0] == '"' and value[-1] == '"':
            value = value[1:-1]
        fields[match.group("key")] = value
    size = BARE_SIZE_RE.search(body)
    if size and "width" not in fields and "height" not in fields:
        fields["width"] = size.group("width")
        fields["height"] = size.group("height")
    return fields


def parse_int(value: str | None) -> int | None:
    if value is None or value == "":
        return None
    try:
        return int(value, 0)
    except ValueError:
        return None


def mib(value: int) -> float:
    return value / (1024.0 * 1024.0)


def codec_name(codec: int) -> str:
    name = CODECS.get(codec, "unknown")
    return f"{name}/0x{codec:x}"


@dataclass(frozen=True, order=True)
class StreamKey:
    driver: int
    stream: int
    codec: int


@dataclass
class SurfaceInfo:
    driver: int = 0
    stream: int = 0
    codec: int = 0
    width: int = 0
    height: int = 0
    fourcc: int = 0


@dataclass
class StreamStats:
    key: StreamKey
    width: int = 0
    height: int = 0
    fourcc: int = 0
    surfaces: set[int] = field(default_factory=set)
    domain_notes: int = 0
    transitions_in: int = 0
    va_end_enter: int = 0
    va_end_submitted: int = 0
    va_end_finished: int = 0
    va_end_failed: int = 0
    decode_submitted: int = 0
    decode_completed: int = 0
    decode_failed: int = 0
    upload_bytes: int = 0
    upload_high: int = 0
    pending_high: int = 0
    refresh_requested: int = 0
    refresh_skipped: int = 0
    export_enter: int = 0
    export_return: int = 0
    export_failed: int = 0
    export_domain_applied: int = 0
    export_drains: int = 0
    export_copy_done: int = 0
    export_copy_metrics: int = 0
    export_copy_targets: int = 0
    export_copy_seed_targets: int = 0
    export_copy_bytes: int = 0
    export_copy_wait_ns: int = 0
    export_copy_publish_skips: int = 0
    retained_added: int = 0
    retained_pruned: int = 0
    retained_removed: int = 0
    stale_drops: int = 0
    predecode_stale_drops: int = 0
    export_seed_stale_drops: int = 0
    stale_visible_nondisplay: int = 0
    nondisplay_shadow_seeds: int = 0
    nondisplay_current_refreshes: int = 0
    nondisplay_private_shadow_refreshes: int = 0
    private_decode_shadow_copies: int = 0
    private_decode_shadow_copy_bytes: int = 0
    private_decode_shadow_copy_wait_ns: int = 0
    decode_shadow_coherence_checks: int = 0
    decode_shadow_incoherent_checks: int = 0
    present_state_traces: int = 0
    visible_present_pins: int = 0
    predecode_quarantine_enters: int = 0
    predecode_quarantine_exits: int = 0
    predecode_quarantine_destroys: int = 0
    export_visible_releases: int = 0
    export_visible_acquires: int = 0
    export_visible_release_missing: int = 0
    decode_pixel_proofs: int = 0
    present_pixel_proofs: int = 0
    present_pixel_mismatches: int = 0
    private_shadow_pixel_proofs: int = 0
    private_shadow_pixel_mismatches: int = 0
    pixel_proof_unavailable: int = 0
    nondisplay_present_pinned_skips: int = 0
    invalid_nondisplay_stale_export_shadows: int = 0
    invalid_presentable_undecoded_surfaces: int = 0
    invalid_nondisplay_present_mutations: int = 0
    invalid_present_generations: int = 0
    invalid_visible_without_present_pins: int = 0
    invalid_stale_private_decode_shadows: int = 0
    invalid_visible_present_states: int = 0
    invalid_thumbnail_predecode_seeds: int = 0
    av1_tile_submit_maps: int = 0
    av1_tile_suspicious: int = 0
    av1_dpb_maps: int = 0
    av1_visible_audits: int = 0
    av1_publish_failures: int = 0

    def remember_identity(self, fields: dict[str, str]) -> None:
        width = parse_int(fields.get("width"))
        height = parse_int(fields.get("height"))
        fourcc = parse_int(fields.get("fourcc"))
        if width:
            self.width = width
        if height:
            self.height = height
        if fourcc:
            self.fourcc = fourcc

    def add_surface(self, surface: int | None) -> None:
        if surface is not None and surface != 0 and surface != 0xFFFFFFFF:
            self.surfaces.add(surface)


class TraceProfile:
    def __init__(self) -> None:
        self.lines = 0
        self.trace_records = 0
        self.first_seq: int | None = None
        self.last_seq: int | None = None
        self.trace_sequence_gaps = 0
        self.sequence_first_by_pid: dict[int, int] = {}
        self.sequence_last_by_pid: dict[int, int] = {}
        self.sequence_gaps_by_pid: Counter[int] = Counter()
        self.event_counts: Counter[str] = Counter()
        self.chrome_errors: Counter[str] = Counter()
        self.nvidia_events: Counter[str] = Counter()
        self.device_lost_pids: set[int] = set()
        self.fence_polls = 0
        self.fence_waits = 0
        self.fence_wait_ns = 0
        self.backpressure_drains = 0
        self.reference_drains = 0
        self.sync_drains = 0
        self.export_copy_metrics = 0
        self.export_copy_targets = 0
        self.export_copy_bytes = 0
        self.export_copy_wait_ns = 0
        self.decode_image_high_water = 0
        self.export_image_high_water = 0
        self.video_session_high_water = 0
        self.retained_latest = 0
        self.retained_bytes_latest = 0
        self.retained_high = 0
        self.retained_bytes_high = 0
        self.retained_pruned = 0
        self.retained_removed = 0
        self.driver_stale_drops = 0
        self.predecode_stale_drops = 0
        self.export_seed_stale_drops = 0
        self.nondisplay_refresh_skips = 0
        self.stale_visible_nondisplay = 0
        self.nondisplay_shadow_seeds = 0
        self.nondisplay_current_refreshes = 0
        self.nondisplay_private_shadow_refreshes = 0
        self.private_decode_shadow_copies = 0
        self.private_decode_shadow_copy_bytes = 0
        self.private_decode_shadow_copy_wait_ns = 0
        self.decode_shadow_coherence_checks = 0
        self.decode_shadow_incoherent_checks = 0
        self.present_state_traces = 0
        self.visible_present_pins = 0
        self.predecode_quarantine_enters = 0
        self.predecode_quarantine_exits = 0
        self.predecode_quarantine_destroys = 0
        self.export_visible_releases = 0
        self.export_visible_acquires = 0
        self.export_visible_release_missing = 0
        self.decode_pixel_proofs = 0
        self.present_pixel_proofs = 0
        self.present_pixel_mismatches = 0
        self.private_shadow_pixel_proofs = 0
        self.private_shadow_pixel_mismatches = 0
        self.pixel_proof_unavailable = 0
        self.nondisplay_present_pinned_skips = 0
        self.invalid_nondisplay_stale_export_shadows = 0
        self.invalid_presentable_undecoded_surfaces = 0
        self.invalid_nondisplay_present_mutations = 0
        self.invalid_present_generations = 0
        self.invalid_visible_without_present_pins = 0
        self.invalid_stale_private_decode_shadows = 0
        self.invalid_visible_present_states = 0
        self.invalid_thumbnail_predecode_seeds = 0
        self.av1_tile_submit_maps = 0
        self.av1_tile_suspicious = 0
        self.av1_dpb_maps = 0
        self.av1_visible_audits = 0
        self.av1_publish_failures = 0
        self.export_copy_publish_skips = 0
        self.browser_dropped_frames_observed = False
        self.surfaces: dict[int, SurfaceInfo] = {}
        self.streams: dict[StreamKey, StreamStats] = {}

    def stream_for_fields(self, fields: dict[str, str], event: str = "") -> StreamStats | None:
        surface = parse_int(fields.get("surface") or fields.get("target") or fields.get("source_surface") or fields.get("owner"))
        driver = parse_int(fields.get("driver") or fields.get("seed_driver"))
        stream = parse_int(fields.get("stream") or fields.get("ctx_stream") or fields.get("new_stream"))
        codec = parse_int(fields.get("codec") or fields.get("ctx_codec") or fields.get("new_codec") or fields.get("surface_codec") or fields.get("resource_codec"))

        if event == "va-export-enter":
            stream = stream or parse_int(fields.get("surface_stream")) or parse_int(fields.get("active_stream"))
            codec = codec or parse_int(fields.get("surface_codec")) or parse_int(fields.get("active_codec"))

        if surface is not None and surface in self.surfaces:
            known = self.surfaces[surface]
            driver = driver or known.driver
            stream = stream or known.stream
            codec = codec or known.codec
            fields.setdefault("width", str(known.width))
            fields.setdefault("height", str(known.height))
            if known.fourcc:
                fields.setdefault("fourcc", f"0x{known.fourcc:x}")

        if not driver or not stream or not codec:
            return None

        key = StreamKey(driver=driver, stream=stream, codec=codec)
        stats = self.streams.get(key)
        if stats is None:
            stats = StreamStats(key=key)
            self.streams[key] = stats
        stats.remember_identity(fields)
        stats.add_surface(surface)
        return stats

    def remember_surface(self, fields: dict[str, str], event: str) -> None:
        surface = parse_int(fields.get("surface") or fields.get("target"))
        if surface is None or surface == 0 or surface == 0xFFFFFFFF:
            return
        info = self.surfaces.get(surface, SurfaceInfo())
        info.driver = parse_int(fields.get("driver")) or info.driver
        info.stream = parse_int(fields.get("stream") or fields.get("new_stream")) or info.stream
        info.codec = parse_int(fields.get("codec") or fields.get("surface_codec") or fields.get("resource_codec") or fields.get("new_codec")) or info.codec
        info.width = parse_int(fields.get("width")) or info.width
        info.height = parse_int(fields.get("height")) or info.height
        info.fourcc = parse_int(fields.get("fourcc")) or info.fourcc
        self.surfaces[surface] = info
        stream = self.stream_for_fields(fields, event)
        if stream is not None:
            stream.remember_identity(fields)

    def add_trace(self, seq: int, event: str, fields: dict[str, str]) -> None:
        self.trace_records += 1
        self.event_counts[event] += 1
        pid = parse_int(fields.get("pid")) or 0
        if pid not in self.sequence_first_by_pid:
            self.sequence_first_by_pid[pid] = seq
        last_for_pid = self.sequence_last_by_pid.get(pid)
        if last_for_pid is not None and seq > last_for_pid + 1:
            self.sequence_gaps_by_pid[pid] += seq - last_for_pid - 1
        if last_for_pid is None or seq > last_for_pid:
            self.sequence_last_by_pid[pid] = seq
        self.first_seq = min(self.sequence_first_by_pid.values()) if self.sequence_first_by_pid else None
        self.last_seq = max(self.sequence_last_by_pid.values()) if self.sequence_last_by_pid else None
        self.trace_sequence_gaps = sum(self.sequence_gaps_by_pid.values())

        self.remember_surface(fields, event)
        stream = self.stream_for_fields(fields, event)
        self.add_global_trace_metrics(event, fields, stream)

        if event == "domain-note" and stream is not None:
            stream.domain_notes += 1
        elif event == "domain-codec-transition" and stream is not None:
            stream.transitions_in += 1
        elif event == "va-end-enter" and stream is not None:
            stream.va_end_enter += 1
        elif event == "va-end-submitted" and stream is not None:
            stream.va_end_submitted += 1
            self.update_pending(stream, fields)
        elif event == "va-end-finish" and stream is not None:
            stream.va_end_finished += 1
            if (parse_int(fields.get("status")) or 0) != 0:
                stream.va_end_failed += 1
        elif event == "pending-submit" and stream is not None:
            stream.decode_submitted += 1
            upload = parse_int(fields.get("upload_mem")) or 0
            stream.upload_bytes += upload
            stream.upload_high = max(stream.upload_high, upload)
            if (parse_int(fields.get("refresh_export")) or 0) != 0:
                stream.refresh_requested += 1
            self.update_pending(stream, fields)
        elif event == "pending-complete-after" and stream is not None:
            if (parse_int(fields.get("status")) or 0) == 0:
                stream.decode_completed += 1
            else:
                stream.decode_failed += 1
        elif event == "pending-reference-drain" and stream is not None:
            stream.export_drains += 1
        elif event == "va-export-enter" and stream is not None:
            stream.export_enter += 1
        elif event == "va-export-domain" and stream is not None:
            if (parse_int(fields.get("applied")) or 0) != 0:
                stream.export_domain_applied += 1
        elif event == "va-export-drain" and stream is not None:
            stream.export_drains += 1
        elif event == "va-export-return" and stream is not None:
            stream.export_return += 1
            if (parse_int(fields.get("status")) or 0) != 0:
                stream.export_failed += 1
        elif event == "export-refresh-skip-nondisplay" and stream is not None:
            stream.refresh_skipped += 1
        elif event == "export-stale-visible-nondisplay" and stream is not None:
            stream.stale_visible_nondisplay += 1
        elif event == "export-nondisplay-shadow-seed" and stream is not None:
            stream.nondisplay_shadow_seeds += 1
        elif event == "nondisplay-export-current-refresh" and stream is not None:
            stream.nondisplay_current_refreshes += 1
        elif event == "nondisplay-private-shadow-refresh" and stream is not None:
            stream.nondisplay_private_shadow_refreshes += 1
        elif event == "private-decode-shadow-copy-done" and stream is not None:
            stream.private_decode_shadow_copies += 1
            stream.private_decode_shadow_copy_bytes += parse_int(fields.get("copy_bytes")) or 0
            stream.private_decode_shadow_copy_wait_ns += parse_int(fields.get("wait_ns")) or 0
        elif event == "decode-shadow-coherence-check" and stream is not None:
            stream.decode_shadow_coherence_checks += 1
            if (parse_int(fields.get("coherent")) or 0) == 0:
                stream.decode_shadow_incoherent_checks += 1
        elif event == "export-present-state" and stream is not None:
            stream.present_state_traces += 1
            if fields.get("action") == "visible-present-pin":
                stream.visible_present_pins += 1
        elif event == "predecode-quarantine-enter" and stream is not None:
            stream.predecode_quarantine_enters += 1
        elif event == "predecode-quarantine-exit" and stream is not None:
            stream.predecode_quarantine_exits += 1
        elif event == "export-resource-destroy" and stream is not None and (parse_int(fields.get("predecode_quarantined")) or 0) != 0:
            stream.predecode_quarantine_destroys += 1
        elif event == "export-visible-release" and stream is not None:
            stream.export_visible_releases += 1
            release_required = parse_int(fields.get("release_required")) or 0
            release_done = parse_int(fields.get("release_done")) or 0
            if release_required != 0 and release_done == 0:
                stream.export_visible_release_missing += 1
        elif event == "export-visible-acquire" and stream is not None:
            stream.export_visible_acquires += 1
        elif event == "decode-pixel-proof" and stream is not None:
            stream.decode_pixel_proofs += 1
        elif event == "present-pixel-proof" and stream is not None:
            stream.present_pixel_proofs += 1
            if (parse_int(fields.get("matches_decode")) or 0) == 0:
                stream.present_pixel_mismatches += 1
        elif event == "private-shadow-pixel-proof" and stream is not None:
            stream.private_shadow_pixel_proofs += 1
            if (parse_int(fields.get("matches_decode")) or 0) == 0:
                stream.private_shadow_pixel_mismatches += 1
        elif event == "pixel-proof-unavailable" and stream is not None:
            stream.pixel_proof_unavailable += 1
        elif event == "nondisplay-present-pinned-skip" and stream is not None:
            stream.nondisplay_present_pinned_skips += 1
        elif event == "invalid-nondisplay-stale-export-shadow" and stream is not None:
            stream.invalid_nondisplay_stale_export_shadows += 1
        elif event == "invalid-presentable-undecoded-surface" and stream is not None:
            stream.invalid_presentable_undecoded_surfaces += 1
        elif event == "invalid-nondisplay-present-mutation" and stream is not None:
            stream.invalid_nondisplay_present_mutations += 1
        elif event == "invalid-present-generation" and stream is not None:
            stream.invalid_present_generations += 1
        elif event == "invalid-visible-without-present-pin" and stream is not None:
            stream.invalid_visible_without_present_pins += 1
        elif event == "invalid-stale-private-decode-shadow" and stream is not None:
            stream.invalid_stale_private_decode_shadows += 1
        elif event == "invalid-visible-present-state" and stream is not None:
            stream.invalid_visible_present_states += 1
        elif event == "invalid-thumbnail-predecode-seed" and stream is not None:
            stream.invalid_thumbnail_predecode_seeds += 1
        elif event == "export-copy-done" and stream is not None:
            stream.export_copy_done += 1
            stream.export_copy_seed_targets += parse_int(fields.get("seeded_targets")) or 0
        elif event == "export-copy-metrics" and stream is not None:
            stream.export_copy_metrics += 1
            stream.export_copy_targets += parse_int(fields.get("copy_targets")) or 0
            stream.export_copy_bytes += parse_int(fields.get("copy_bytes")) or 0
            stream.export_copy_wait_ns += parse_int(fields.get("wait_ns")) or 0
        elif event == "export-copy-publish-skip" and stream is not None:
            stream.export_copy_publish_skips += 1
        elif event == "av1-tile-submit-map" and stream is not None and fields.get("scope") == "frame":
            stream.av1_tile_submit_maps += 1
            if (parse_int(fields.get("suspicious")) or 0) != 0:
                stream.av1_tile_suspicious += 1
        elif event in ("av1-dpb-map-before-submit", "av1-dpb-map-after-submit", "av1-dpb-map-after-refresh") and stream is not None and fields.get("scope") == "frame":
            stream.av1_dpb_maps += 1
        elif event == "av1-visible-frame-audit" and stream is not None:
            stream.av1_visible_audits += 1
            if fields.get("failure_stage") not in (None, "", "none"):
                stream.av1_publish_failures += 1
        elif event == "retained-export-add" and stream is not None:
            stream.retained_added += 1
        elif event == "retained-export-prune" and stream is not None:
            stream.retained_pruned += 1
        elif event == "retained-export-remove" and stream is not None:
            stream.retained_removed += 1
        elif event == "export-seed-stale-drop" and stream is not None:
            stream.stale_drops += 1
            stream.export_seed_stale_drops += 1
        elif event == "predecode-export-stale-drop" and stream is not None:
            stream.stale_drops += 1
            stream.predecode_stale_drops += 1

    def add_global_trace_metrics(self, event: str, fields: dict[str, str], stream: StreamStats | None) -> None:
        if "retained" in fields:
            retained = parse_int(fields.get("retained")) or 0
            retained_mem = parse_int(fields.get("retained_mem")) or 0
            self.retained_latest = retained
            self.retained_bytes_latest = retained_mem
            self.retained_high = max(self.retained_high, retained)
            self.retained_bytes_high = max(self.retained_bytes_high, retained_mem)

        if event == "fence-poll":
            self.fence_polls += 1
        elif event == "fence-wait":
            self.fence_waits += 1
            self.fence_wait_ns += parse_int(fields.get("wait_ns")) or 0
            if (parse_int(fields.get("status")) or 0) == -4:
                self.note_device_lost(fields)
        elif event == "pending-backpressure-drain":
            self.backpressure_drains += 1
        elif event == "pending-reference-drain":
            self.reference_drains += 1
        elif event == "pending-sync-drain":
            self.sync_drains += 1
        elif event == "surface-resource-create":
            self.decode_image_high_water = max(self.decode_image_high_water, parse_int(fields.get("decode_mem")) or 0)
        elif event == "export-image-create":
            self.export_image_high_water = max(self.export_image_high_water, parse_int(fields.get("export_mem")) or 0)
        elif event == "video-session-memory":
            self.video_session_high_water = max(self.video_session_high_water, parse_int(fields.get("bytes")) or 0)
        elif event == "export-copy-metrics":
            self.export_copy_metrics += 1
            self.export_copy_targets += parse_int(fields.get("copy_targets")) or 0
            self.export_copy_bytes += parse_int(fields.get("copy_bytes")) or 0
            self.export_copy_wait_ns += parse_int(fields.get("wait_ns")) or 0
        elif event == "retained-export-prune":
            self.retained_pruned += 1
        elif event == "retained-export-remove":
            self.retained_removed += 1
        elif event == "export-seed-stale-drop":
            self.export_seed_stale_drops += 1
            self.driver_stale_drops += 1
        elif event == "predecode-export-stale-drop":
            self.predecode_stale_drops += 1
            self.driver_stale_drops += 1
        elif event == "export-refresh-skip-nondisplay":
            self.nondisplay_refresh_skips += 1
        elif event == "export-stale-visible-nondisplay":
            self.stale_visible_nondisplay += 1
        elif event == "export-nondisplay-shadow-seed":
            self.nondisplay_shadow_seeds += 1
        elif event == "nondisplay-export-current-refresh":
            self.nondisplay_current_refreshes += 1
        elif event == "nondisplay-private-shadow-refresh":
            self.nondisplay_private_shadow_refreshes += 1
        elif event == "private-decode-shadow-copy-done":
            self.private_decode_shadow_copies += 1
            self.private_decode_shadow_copy_bytes += parse_int(fields.get("copy_bytes")) or 0
            self.private_decode_shadow_copy_wait_ns += parse_int(fields.get("wait_ns")) or 0
        elif event == "decode-shadow-coherence-check":
            self.decode_shadow_coherence_checks += 1
            if (parse_int(fields.get("coherent")) or 0) == 0:
                self.decode_shadow_incoherent_checks += 1
        elif event == "export-present-state":
            self.present_state_traces += 1
            if fields.get("action") == "visible-present-pin":
                self.visible_present_pins += 1
        elif event == "predecode-quarantine-enter":
            self.predecode_quarantine_enters += 1
        elif event == "predecode-quarantine-exit":
            self.predecode_quarantine_exits += 1
        elif event == "export-resource-destroy" and (parse_int(fields.get("predecode_quarantined")) or 0) != 0:
            self.predecode_quarantine_destroys += 1
        elif event == "export-visible-release":
            self.export_visible_releases += 1
            release_required = parse_int(fields.get("release_required")) or 0
            release_done = parse_int(fields.get("release_done")) or 0
            if release_required != 0 and release_done == 0:
                self.export_visible_release_missing += 1
        elif event == "export-visible-acquire":
            self.export_visible_acquires += 1
        elif event == "decode-pixel-proof":
            self.decode_pixel_proofs += 1
        elif event == "present-pixel-proof":
            self.present_pixel_proofs += 1
            if (parse_int(fields.get("matches_decode")) or 0) == 0:
                self.present_pixel_mismatches += 1
        elif event == "private-shadow-pixel-proof":
            self.private_shadow_pixel_proofs += 1
            if (parse_int(fields.get("matches_decode")) or 0) == 0:
                self.private_shadow_pixel_mismatches += 1
        elif event == "pixel-proof-unavailable":
            self.pixel_proof_unavailable += 1
        elif event == "nondisplay-present-pinned-skip":
            self.nondisplay_present_pinned_skips += 1
        elif event == "invalid-nondisplay-stale-export-shadow":
            self.invalid_nondisplay_stale_export_shadows += 1
        elif event == "invalid-presentable-undecoded-surface":
            self.invalid_presentable_undecoded_surfaces += 1
        elif event == "invalid-nondisplay-present-mutation":
            self.invalid_nondisplay_present_mutations += 1
        elif event == "invalid-present-generation":
            self.invalid_present_generations += 1
        elif event == "invalid-visible-without-present-pin":
            self.invalid_visible_without_present_pins += 1
        elif event == "invalid-stale-private-decode-shadow":
            self.invalid_stale_private_decode_shadows += 1
        elif event == "invalid-visible-present-state":
            self.invalid_visible_present_states += 1
        elif event == "invalid-thumbnail-predecode-seed":
            self.invalid_thumbnail_predecode_seeds += 1
        elif event == "export-copy-publish-skip":
            self.export_copy_publish_skips += 1
        elif event == "av1-tile-submit-map" and fields.get("scope") == "frame":
            self.av1_tile_submit_maps += 1
            if (parse_int(fields.get("suspicious")) or 0) != 0:
                self.av1_tile_suspicious += 1
        elif event in ("av1-dpb-map-before-submit", "av1-dpb-map-after-submit", "av1-dpb-map-after-refresh") and fields.get("scope") == "frame":
            self.av1_dpb_maps += 1
        elif event == "av1-visible-frame-audit":
            self.av1_visible_audits += 1
            if fields.get("failure_stage") not in (None, "", "none"):
                self.av1_publish_failures += 1
        elif event == "device-lost":
            self.note_device_lost(fields)

    @staticmethod
    def update_pending(stream: StreamStats, fields: dict[str, str]) -> None:
        pending = parse_int(fields.get("pending"))
        if pending is not None:
            stream.pending_high = max(stream.pending_high, pending)

    def add_nvidia_event(self, kind: str, fields: dict[str, str]) -> None:
        self.nvidia_events[kind] += 1
        if kind == "device-lost":
            self.note_device_lost(fields)

    def note_device_lost(self, fields: dict[str, str]) -> None:
        self.device_lost_pids.add(parse_int(fields.get("pid")) or 0)

    def add_chrome_error(self, source: str, message: str) -> None:
        if "vaapi" not in source and "vaapi" not in message.lower():
            return
        normalized = message
        if "vaEndPicture failed" in message:
            normalized = "vaEndPicture failed"
        elif "vaExportSurfaceHandle failed" in message:
            normalized = "vaExportSurfaceHandle failed"
        elif "failed Initialize()ing the frame pool" in message:
            normalized = "failed Initialize()ing the frame pool"
        elif "error decoding stream" in message:
            normalized = "error decoding stream"
        self.chrome_errors[normalized] += 1

    def totals(self) -> dict[str, int]:
        submitted = sum(stream.decode_submitted for stream in self.streams.values())
        completed = sum(stream.decode_completed for stream in self.streams.values())
        failed = sum(max(stream.decode_failed, stream.va_end_failed) for stream in self.streams.values())
        return {
            "streams": len(self.streams),
            "decode_submitted": submitted,
            "decode_completed": completed,
            "decode_failed": failed,
            "decode_inflight": submitted - completed,
            "va_end_submitted": sum(stream.va_end_submitted for stream in self.streams.values()),
            "va_end_failed": sum(stream.va_end_failed for stream in self.streams.values()),
            "export_return": sum(stream.export_return for stream in self.streams.values()),
            "export_failed": sum(stream.export_failed for stream in self.streams.values()),
            "export_copy_done": sum(stream.export_copy_done for stream in self.streams.values()),
            "export_copy_targets": self.export_copy_targets if self.export_copy_metrics else sum(stream.export_copy_seed_targets for stream in self.streams.values()),
            "export_copy_bytes": self.export_copy_bytes,
            "export_copy_wait_ns": self.export_copy_wait_ns,
            "fence_polls": self.fence_polls,
            "fence_waits": self.fence_waits,
            "fence_wait_ns": self.fence_wait_ns,
            "backpressure_drains": self.backpressure_drains,
            "reference_drains": self.reference_drains,
            "sync_drains": self.sync_drains,
            "upload_bytes": sum(stream.upload_bytes for stream in self.streams.values()),
            "decode_image_high_water": self.decode_image_high_water,
            "export_image_high_water": self.export_image_high_water,
            "video_session_high_water": self.video_session_high_water,
            "retained_latest": self.retained_latest,
            "retained_bytes_latest": self.retained_bytes_latest,
            "retained_high": self.retained_high,
            "retained_bytes_high": self.retained_bytes_high,
            "retained_pruned": self.retained_pruned,
            "retained_removed": self.retained_removed,
            "driver_stale_drops": self.driver_stale_drops,
            "predecode_stale_drops": self.predecode_stale_drops,
            "export_seed_stale_drops": self.export_seed_stale_drops,
            "nondisplay_refresh_skips": self.nondisplay_refresh_skips,
            "stale_visible_nondisplay": self.stale_visible_nondisplay,
            "nondisplay_shadow_seeds": self.nondisplay_shadow_seeds,
            "nondisplay_current_refreshes": self.nondisplay_current_refreshes,
            "nondisplay_private_shadow_refreshes": self.nondisplay_private_shadow_refreshes,
            "private_decode_shadow_copies": self.private_decode_shadow_copies,
            "private_decode_shadow_copy_bytes": self.private_decode_shadow_copy_bytes,
            "private_decode_shadow_copy_wait_ns": self.private_decode_shadow_copy_wait_ns,
            "decode_shadow_coherence_checks": self.decode_shadow_coherence_checks,
            "decode_shadow_incoherent_checks": self.decode_shadow_incoherent_checks,
            "present_state_traces": self.present_state_traces,
            "visible_present_pins": self.visible_present_pins,
            "predecode_quarantine_enters": self.predecode_quarantine_enters,
            "predecode_quarantine_exits": self.predecode_quarantine_exits,
            "predecode_quarantine_destroys": self.predecode_quarantine_destroys,
            "export_visible_releases": self.export_visible_releases,
            "export_visible_acquires": self.export_visible_acquires,
            "export_visible_release_missing": self.export_visible_release_missing,
            "decode_pixel_proofs": self.decode_pixel_proofs,
            "present_pixel_proofs": self.present_pixel_proofs,
            "present_pixel_mismatches": self.present_pixel_mismatches,
            "private_shadow_pixel_proofs": self.private_shadow_pixel_proofs,
            "private_shadow_pixel_mismatches": self.private_shadow_pixel_mismatches,
            "pixel_proof_unavailable": self.pixel_proof_unavailable,
            "nondisplay_present_pinned_skips": self.nondisplay_present_pinned_skips,
            "invalid_nondisplay_stale_export_shadows": self.invalid_nondisplay_stale_export_shadows,
            "invalid_presentable_undecoded_surfaces": self.invalid_presentable_undecoded_surfaces,
            "invalid_nondisplay_present_mutations": self.invalid_nondisplay_present_mutations,
            "invalid_present_generations": self.invalid_present_generations,
            "invalid_visible_without_present_pins": self.invalid_visible_without_present_pins,
            "invalid_stale_private_decode_shadows": self.invalid_stale_private_decode_shadows,
            "invalid_visible_present_states": self.invalid_visible_present_states,
            "invalid_thumbnail_predecode_seeds": self.invalid_thumbnail_predecode_seeds,
            "av1_tile_submit_maps": self.av1_tile_submit_maps,
            "av1_tile_suspicious": self.av1_tile_suspicious,
            "av1_dpb_maps": self.av1_dpb_maps,
            "av1_visible_audits": self.av1_visible_audits,
            "av1_publish_failures": self.av1_publish_failures,
            "export_copy_publish_skips": self.export_copy_publish_skips,
            "device_lost": len(self.device_lost_pids),
            "chrome_vaapi_errors": sum(self.chrome_errors.values()),
        }

    def codec_totals(self) -> dict[int, dict[str, int]]:
        codecs: dict[int, dict[str, int]] = defaultdict(lambda: defaultdict(int))
        for stream in self.streams.values():
            total = codecs[stream.key.codec]
            total["streams"] += 1
            total["decode_submitted"] += stream.decode_submitted
            total["decode_completed"] += stream.decode_completed
            total["decode_failed"] += max(stream.decode_failed, stream.va_end_failed)
            total["va_end_submitted"] += stream.va_end_submitted
            total["va_end_failed"] += stream.va_end_failed
            total["export_failed"] += stream.export_failed
            total["export_copy_bytes"] += stream.export_copy_bytes
            total["export_copy_wait_ns"] += stream.export_copy_wait_ns
            total["upload_bytes"] += stream.upload_bytes
            total["driver_stale_drops"] += stream.stale_drops
            total["predecode_stale_drops"] += stream.predecode_stale_drops
            total["export_seed_stale_drops"] += stream.export_seed_stale_drops
            total["nondisplay_refresh_skips"] += stream.refresh_skipped
            total["stale_visible_nondisplay"] += stream.stale_visible_nondisplay
            total["nondisplay_shadow_seeds"] += stream.nondisplay_shadow_seeds
            total["nondisplay_current_refreshes"] += stream.nondisplay_current_refreshes
            total["nondisplay_private_shadow_refreshes"] += stream.nondisplay_private_shadow_refreshes
            total["private_decode_shadow_copies"] += stream.private_decode_shadow_copies
            total["private_decode_shadow_copy_bytes"] += stream.private_decode_shadow_copy_bytes
            total["private_decode_shadow_copy_wait_ns"] += stream.private_decode_shadow_copy_wait_ns
            total["decode_shadow_coherence_checks"] += stream.decode_shadow_coherence_checks
            total["decode_shadow_incoherent_checks"] += stream.decode_shadow_incoherent_checks
            total["present_state_traces"] += stream.present_state_traces
            total["visible_present_pins"] += stream.visible_present_pins
            total["predecode_quarantine_enters"] += stream.predecode_quarantine_enters
            total["predecode_quarantine_exits"] += stream.predecode_quarantine_exits
            total["predecode_quarantine_destroys"] += stream.predecode_quarantine_destroys
            total["export_visible_releases"] += stream.export_visible_releases
            total["export_visible_acquires"] += stream.export_visible_acquires
            total["export_visible_release_missing"] += stream.export_visible_release_missing
            total["decode_pixel_proofs"] += stream.decode_pixel_proofs
            total["present_pixel_proofs"] += stream.present_pixel_proofs
            total["present_pixel_mismatches"] += stream.present_pixel_mismatches
            total["private_shadow_pixel_proofs"] += stream.private_shadow_pixel_proofs
            total["private_shadow_pixel_mismatches"] += stream.private_shadow_pixel_mismatches
            total["pixel_proof_unavailable"] += stream.pixel_proof_unavailable
            total["nondisplay_present_pinned_skips"] += stream.nondisplay_present_pinned_skips
            total["invalid_nondisplay_stale_export_shadows"] += stream.invalid_nondisplay_stale_export_shadows
            total["invalid_presentable_undecoded_surfaces"] += stream.invalid_presentable_undecoded_surfaces
            total["invalid_nondisplay_present_mutations"] += stream.invalid_nondisplay_present_mutations
            total["invalid_present_generations"] += stream.invalid_present_generations
            total["invalid_visible_without_present_pins"] += stream.invalid_visible_without_present_pins
            total["invalid_stale_private_decode_shadows"] += stream.invalid_stale_private_decode_shadows
            total["invalid_visible_present_states"] += stream.invalid_visible_present_states
            total["invalid_thumbnail_predecode_seeds"] += stream.invalid_thumbnail_predecode_seeds
            total["av1_tile_submit_maps"] += stream.av1_tile_submit_maps
            total["av1_tile_suspicious"] += stream.av1_tile_suspicious
            total["av1_dpb_maps"] += stream.av1_dpb_maps
            total["av1_visible_audits"] += stream.av1_visible_audits
            total["av1_publish_failures"] += stream.av1_publish_failures
            total["export_copy_publish_skips"] += stream.export_copy_publish_skips
        return codecs

    def to_json(self, source: str) -> dict[str, Any]:
        return {
            "path": source,
            "lines": self.lines,
            "trace_records": self.trace_records,
            "trace_sequence": {
                "first": self.first_seq,
                "last": self.last_seq,
                "missing": self.trace_sequence_gaps,
            },
            "trace_sequences": {
                str(pid): {
                    "first": self.sequence_first_by_pid[pid],
                    "last": self.sequence_last_by_pid.get(pid),
                    "missing": self.sequence_gaps_by_pid.get(pid, 0),
                }
                for pid in sorted(self.sequence_first_by_pid)
            },
            "totals": self.totals(),
            "browser_dropped_frames_observed": self.browser_dropped_frames_observed,
            "events": dict(sorted(self.event_counts.items())),
            "chrome_errors": dict(self.chrome_errors),
            "nvidia_events": dict(self.nvidia_events),
            "codecs": {codec_name(codec): dict(values) for codec, values in sorted(self.codec_totals().items())},
            "streams": [stream_to_json(stream) for stream in sorted(self.streams.values(), key=lambda item: item.key)],
        }


def stream_to_json(stream: StreamStats) -> dict[str, Any]:
    return {
        "driver": stream.key.driver,
        "stream": stream.key.stream,
        "codec": codec_name(stream.key.codec),
        "width": stream.width,
        "height": stream.height,
        "fourcc": f"0x{stream.fourcc:x}" if stream.fourcc else "0x0",
        "surfaces": len(stream.surfaces),
        "decode_submitted": stream.decode_submitted,
        "decode_completed": stream.decode_completed,
        "decode_failed": max(stream.decode_failed, stream.va_end_failed),
        "decode_inflight": stream.decode_submitted - stream.decode_completed,
        "va_end_submitted": stream.va_end_submitted,
        "va_end_failed": stream.va_end_failed,
        "export_return": stream.export_return,
        "export_failed": stream.export_failed,
        "export_copy_done": stream.export_copy_done,
        "export_copy_metrics": stream.export_copy_metrics,
        "export_copy_targets": stream.export_copy_targets if stream.export_copy_metrics else stream.export_copy_seed_targets,
        "export_copy_seed_targets": stream.export_copy_seed_targets,
        "export_copy_bytes": stream.export_copy_bytes,
        "export_copy_wait_ns": stream.export_copy_wait_ns,
        "pending_high": stream.pending_high,
        "upload_bytes": stream.upload_bytes,
        "upload_high": stream.upload_high,
        "refresh_requested": stream.refresh_requested,
        "refresh_skipped": stream.refresh_skipped,
        "retained_added": stream.retained_added,
        "retained_pruned": stream.retained_pruned,
        "retained_removed": stream.retained_removed,
        "stale_drops": stream.stale_drops,
        "predecode_stale_drops": stream.predecode_stale_drops,
        "export_seed_stale_drops": stream.export_seed_stale_drops,
        "nondisplay_refresh_skips": stream.refresh_skipped,
        "stale_visible_nondisplay": stream.stale_visible_nondisplay,
        "nondisplay_shadow_seeds": stream.nondisplay_shadow_seeds,
        "nondisplay_current_refreshes": stream.nondisplay_current_refreshes,
        "nondisplay_private_shadow_refreshes": stream.nondisplay_private_shadow_refreshes,
        "private_decode_shadow_copies": stream.private_decode_shadow_copies,
        "private_decode_shadow_copy_bytes": stream.private_decode_shadow_copy_bytes,
        "private_decode_shadow_copy_wait_ns": stream.private_decode_shadow_copy_wait_ns,
        "decode_shadow_coherence_checks": stream.decode_shadow_coherence_checks,
        "decode_shadow_incoherent_checks": stream.decode_shadow_incoherent_checks,
        "present_state_traces": stream.present_state_traces,
        "visible_present_pins": stream.visible_present_pins,
        "predecode_quarantine_enters": stream.predecode_quarantine_enters,
        "predecode_quarantine_exits": stream.predecode_quarantine_exits,
        "predecode_quarantine_destroys": stream.predecode_quarantine_destroys,
        "export_visible_releases": stream.export_visible_releases,
        "export_visible_acquires": stream.export_visible_acquires,
        "export_visible_release_missing": stream.export_visible_release_missing,
        "decode_pixel_proofs": stream.decode_pixel_proofs,
        "present_pixel_proofs": stream.present_pixel_proofs,
        "present_pixel_mismatches": stream.present_pixel_mismatches,
        "private_shadow_pixel_proofs": stream.private_shadow_pixel_proofs,
        "private_shadow_pixel_mismatches": stream.private_shadow_pixel_mismatches,
        "pixel_proof_unavailable": stream.pixel_proof_unavailable,
        "nondisplay_present_pinned_skips": stream.nondisplay_present_pinned_skips,
        "invalid_nondisplay_stale_export_shadows": stream.invalid_nondisplay_stale_export_shadows,
        "invalid_presentable_undecoded_surfaces": stream.invalid_presentable_undecoded_surfaces,
        "invalid_nondisplay_present_mutations": stream.invalid_nondisplay_present_mutations,
        "invalid_present_generations": stream.invalid_present_generations,
        "invalid_visible_without_present_pins": stream.invalid_visible_without_present_pins,
        "invalid_stale_private_decode_shadows": stream.invalid_stale_private_decode_shadows,
        "invalid_visible_present_states": stream.invalid_visible_present_states,
        "invalid_thumbnail_predecode_seeds": stream.invalid_thumbnail_predecode_seeds,
        "av1_tile_submit_maps": stream.av1_tile_submit_maps,
        "av1_tile_suspicious": stream.av1_tile_suspicious,
        "av1_dpb_maps": stream.av1_dpb_maps,
        "av1_visible_audits": stream.av1_visible_audits,
        "av1_publish_failures": stream.av1_publish_failures,
        "export_copy_publish_skips": stream.export_copy_publish_skips,
    }


def print_live_summary(source: str, profile: TraceProfile, output: TextIO = sys.stderr) -> None:
    totals = profile.totals()
    print(
        "live-summary "
        f"path={source} lines={profile.lines} trace_records={profile.trace_records} "
        f"submitted={totals['decode_submitted']} completed={totals['decode_completed']} "
        f"failed={totals['decode_failed']} device_lost={totals['device_lost']} "
        f"driver_stale_drops={totals['driver_stale_drops']} chrome_vaapi_errors={totals['chrome_vaapi_errors']}",
        file=output,
    )


def profile_lines(lines: TextIO, source: str = "-", live: bool = False, live_interval_lines: int = 1000) -> TraceProfile:
    profile = TraceProfile()
    interval = max(live_interval_lines, 1)
    for line in lines:
        profile.lines += 1
        line = line.rstrip("\n")
        trace = TRACE_RE.search(line)
        if trace:
            profile.add_trace(int(trace.group("seq")), trace.group("event"), parse_fields(trace.group("body")))
        else:
            nvidia = NVIDIA_RE.search(line)
            if nvidia:
                profile.add_nvidia_event(nvidia.group("kind"), parse_fields(nvidia.group("body")))
            else:
                chrome_error = CHROME_ERROR_RE.search(line)
                if chrome_error:
                    profile.add_chrome_error(chrome_error.group("source"), chrome_error.group("message"))
        if live and profile.lines % interval == 0:
            print_live_summary(source, profile)
    return profile


def profile_log(path: Path, live: bool = False, live_interval_lines: int = 1000) -> TraceProfile:
    with path.open("r", encoding="utf-8", errors="replace") as handle:
        return profile_lines(handle, str(path), live, live_interval_lines)


def print_text(source: str, profile: TraceProfile, top_events: int) -> None:
    totals = profile.totals()
    print(
        "trace-profile "
        f"path={source} lines={profile.lines} trace_records={profile.trace_records} "
        f"seq_first={profile.first_seq if profile.first_seq is not None else 0} "
        f"seq_last={profile.last_seq if profile.last_seq is not None else 0} "
        f"seq_missing={profile.trace_sequence_gaps}"
    )
    print(
        "summary "
        f"streams={totals['streams']} submitted={totals['decode_submitted']} "
        f"completed={totals['decode_completed']} failed={totals['decode_failed']} "
        f"inflight={totals['decode_inflight']} va_end_submitted={totals['va_end_submitted']} va_end_failed={totals['va_end_failed']} "
        f"export_return={totals['export_return']} export_failed={totals['export_failed']} "
        f"export_copy_done={totals['export_copy_done']} export_copy_targets={totals['export_copy_targets']} "
        f"export_copy_mb={mib(totals['export_copy_bytes']):.2f} export_copy_wait_ms={totals['export_copy_wait_ns'] / 1000000.0:.3f} "
        f"fence_polls={totals['fence_polls']} fence_waits={totals['fence_waits']} fence_wait_ms={totals['fence_wait_ns'] / 1000000.0:.3f} "
        f"backpressure={totals['backpressure_drains']} ref_drains={totals['reference_drains']} sync_drains={totals['sync_drains']} "
        f"upload_mb={mib(totals['upload_bytes']):.2f} decode_image_high_mb={mib(totals['decode_image_high_water']):.2f} "
        f"export_image_high_mb={mib(totals['export_image_high_water']):.2f} session_high_mb={mib(totals['video_session_high_water']):.2f} "
        f"retained_latest={totals['retained_latest']} retained_latest_mb={mib(totals['retained_bytes_latest']):.2f} "
        f"retained_high={totals['retained_high']} retained_high_mb={mib(totals['retained_bytes_high']):.2f} "
        f"retained_pruned={totals['retained_pruned']} retained_removed={totals['retained_removed']} "
        f"driver_stale_drops={totals['driver_stale_drops']} predecode_stale_drops={totals['predecode_stale_drops']} "
        f"export_seed_stale_drops={totals['export_seed_stale_drops']} nondisplay_refresh_skips={totals['nondisplay_refresh_skips']} "
        f"stale_visible_nondisplay={totals['stale_visible_nondisplay']} nondisplay_shadow_seeds={totals['nondisplay_shadow_seeds']} "
        f"nondisplay_current_refreshes={totals['nondisplay_current_refreshes']} "
        f"nondisplay_private_shadow_refreshes={totals['nondisplay_private_shadow_refreshes']} "
        f"private_decode_shadow_copies={totals['private_decode_shadow_copies']} "
        f"private_decode_shadow_copy_mb={mib(totals['private_decode_shadow_copy_bytes']):.2f} "
        f"private_decode_shadow_copy_wait_ms={totals['private_decode_shadow_copy_wait_ns'] / 1000000.0:.3f} "
        f"decode_shadow_coherence_checks={totals['decode_shadow_coherence_checks']} "
        f"decode_shadow_incoherent_checks={totals['decode_shadow_incoherent_checks']} "
        f"present_state_traces={totals['present_state_traces']} visible_present_pins={totals['visible_present_pins']} "
        f"predecode_quarantine_enters={totals['predecode_quarantine_enters']} "
        f"predecode_quarantine_exits={totals['predecode_quarantine_exits']} "
        f"predecode_quarantine_destroys={totals['predecode_quarantine_destroys']} "
        f"export_visible_releases={totals['export_visible_releases']} "
        f"export_visible_acquires={totals['export_visible_acquires']} "
        f"export_visible_release_missing={totals['export_visible_release_missing']} "
        f"decode_pixel_proofs={totals['decode_pixel_proofs']} present_pixel_proofs={totals['present_pixel_proofs']} "
        f"present_pixel_mismatches={totals['present_pixel_mismatches']} private_shadow_pixel_proofs={totals['private_shadow_pixel_proofs']} "
        f"private_shadow_pixel_mismatches={totals['private_shadow_pixel_mismatches']} pixel_proof_unavailable={totals['pixel_proof_unavailable']} "
        f"nondisplay_present_pinned_skips={totals['nondisplay_present_pinned_skips']} "
        f"invalid_nondisplay_stale_export_shadows={totals['invalid_nondisplay_stale_export_shadows']} "
        f"invalid_presentable_undecoded_surfaces={totals['invalid_presentable_undecoded_surfaces']} "
        f"invalid_nondisplay_present_mutations={totals['invalid_nondisplay_present_mutations']} "
        f"invalid_present_generations={totals['invalid_present_generations']} invalid_visible_without_present_pins={totals['invalid_visible_without_present_pins']} "
        f"invalid_stale_private_decode_shadows={totals['invalid_stale_private_decode_shadows']} "
        f"invalid_visible_present_states={totals['invalid_visible_present_states']} "
        f"invalid_thumbnail_predecode_seeds={totals['invalid_thumbnail_predecode_seeds']} "
        f"av1_tile_submit_maps={totals['av1_tile_submit_maps']} av1_tile_suspicious={totals['av1_tile_suspicious']} "
        f"av1_dpb_maps={totals['av1_dpb_maps']} av1_visible_audits={totals['av1_visible_audits']} av1_publish_failures={totals['av1_publish_failures']} "
        f"export_copy_publish_skips={totals['export_copy_publish_skips']} "
        f"browser_dropped_frames_observed={1 if profile.browser_dropped_frames_observed else 0} "
        f"device_lost={totals['device_lost']} chrome_vaapi_errors={totals['chrome_vaapi_errors']}"
    )
    for codec, values in sorted(profile.codec_totals().items()):
        print(
            "codec "
            f"codec={codec_name(codec)} streams={values['streams']} "
            f"submitted={values['decode_submitted']} completed={values['decode_completed']} "
            f"failed={values['decode_failed']} va_end_submitted={values['va_end_submitted']} va_end_failed={values['va_end_failed']} "
            f"export_failed={values['export_failed']} export_copy_mb={mib(values['export_copy_bytes']):.2f} "
            f"export_copy_wait_ms={values['export_copy_wait_ns'] / 1000000.0:.3f} upload_mb={mib(values['upload_bytes']):.2f} "
            f"driver_stale_drops={values['driver_stale_drops']} predecode_stale_drops={values['predecode_stale_drops']} "
            f"export_seed_stale_drops={values['export_seed_stale_drops']} nondisplay_refresh_skips={values['nondisplay_refresh_skips']} "
            f"stale_visible_nondisplay={values['stale_visible_nondisplay']} nondisplay_shadow_seeds={values['nondisplay_shadow_seeds']} "
            f"nondisplay_current_refreshes={values['nondisplay_current_refreshes']} "
            f"nondisplay_private_shadow_refreshes={values['nondisplay_private_shadow_refreshes']} "
            f"private_decode_shadow_copies={values['private_decode_shadow_copies']} "
            f"private_decode_shadow_copy_mb={mib(values['private_decode_shadow_copy_bytes']):.2f} "
            f"private_decode_shadow_copy_wait_ms={values['private_decode_shadow_copy_wait_ns'] / 1000000.0:.3f} "
            f"decode_shadow_coherence_checks={values['decode_shadow_coherence_checks']} "
            f"decode_shadow_incoherent_checks={values['decode_shadow_incoherent_checks']} "
            f"present_state_traces={values['present_state_traces']} visible_present_pins={values['visible_present_pins']} "
            f"predecode_quarantine_enters={values['predecode_quarantine_enters']} "
            f"predecode_quarantine_exits={values['predecode_quarantine_exits']} "
            f"predecode_quarantine_destroys={values['predecode_quarantine_destroys']} "
            f"export_visible_releases={values['export_visible_releases']} "
            f"export_visible_acquires={values['export_visible_acquires']} "
            f"export_visible_release_missing={values['export_visible_release_missing']} "
            f"decode_pixel_proofs={values['decode_pixel_proofs']} present_pixel_proofs={values['present_pixel_proofs']} "
            f"present_pixel_mismatches={values['present_pixel_mismatches']} private_shadow_pixel_proofs={values['private_shadow_pixel_proofs']} "
            f"private_shadow_pixel_mismatches={values['private_shadow_pixel_mismatches']} pixel_proof_unavailable={values['pixel_proof_unavailable']} "
            f"nondisplay_present_pinned_skips={values['nondisplay_present_pinned_skips']} "
            f"invalid_nondisplay_stale_export_shadows={values['invalid_nondisplay_stale_export_shadows']} "
            f"invalid_presentable_undecoded_surfaces={values['invalid_presentable_undecoded_surfaces']} "
            f"invalid_nondisplay_present_mutations={values['invalid_nondisplay_present_mutations']} "
            f"invalid_present_generations={values['invalid_present_generations']} invalid_visible_without_present_pins={values['invalid_visible_without_present_pins']} "
            f"invalid_stale_private_decode_shadows={values['invalid_stale_private_decode_shadows']} "
            f"invalid_visible_present_states={values['invalid_visible_present_states']} "
            f"invalid_thumbnail_predecode_seeds={values['invalid_thumbnail_predecode_seeds']} "
            f"av1_tile_submit_maps={values['av1_tile_submit_maps']} av1_tile_suspicious={values['av1_tile_suspicious']} "
            f"av1_dpb_maps={values['av1_dpb_maps']} av1_visible_audits={values['av1_visible_audits']} av1_publish_failures={values['av1_publish_failures']} "
            f"export_copy_publish_skips={values['export_copy_publish_skips']}"
        )
    for stream in sorted(profile.streams.values(), key=lambda item: item.key):
        copy_targets = stream.export_copy_targets if stream.export_copy_metrics else stream.export_copy_seed_targets
        print(
            "stream "
            f"driver={stream.key.driver} stream={stream.key.stream} codec={codec_name(stream.key.codec)} "
            f"size={stream.width}x{stream.height} fourcc=0x{stream.fourcc:x} surfaces={len(stream.surfaces)} "
            f"submitted={stream.decode_submitted} completed={stream.decode_completed} failed={max(stream.decode_failed, stream.va_end_failed)} "
            f"inflight={stream.decode_submitted - stream.decode_completed} va_end_submitted={stream.va_end_submitted} va_end_failed={stream.va_end_failed} "
            f"export_return={stream.export_return} export_failed={stream.export_failed} "
            f"copy_done={stream.export_copy_done} copy_targets={copy_targets} copy_mb={mib(stream.export_copy_bytes):.2f} "
            f"copy_wait_ms={stream.export_copy_wait_ns / 1000000.0:.3f} "
            f"pending_high={stream.pending_high} upload_mb={mib(stream.upload_bytes):.2f} "
            f"upload_high_mb={mib(stream.upload_high):.2f} "
            f"driver_stale_drops={stream.stale_drops} predecode_stale_drops={stream.predecode_stale_drops} "
            f"export_seed_stale_drops={stream.export_seed_stale_drops} nondisplay_refresh_skips={stream.refresh_skipped} "
            f"stale_visible_nondisplay={stream.stale_visible_nondisplay} nondisplay_shadow_seeds={stream.nondisplay_shadow_seeds} "
            f"nondisplay_current_refreshes={stream.nondisplay_current_refreshes} "
            f"nondisplay_private_shadow_refreshes={stream.nondisplay_private_shadow_refreshes} "
            f"private_decode_shadow_copies={stream.private_decode_shadow_copies} "
            f"private_decode_shadow_copy_mb={mib(stream.private_decode_shadow_copy_bytes):.2f} "
            f"private_decode_shadow_copy_wait_ms={stream.private_decode_shadow_copy_wait_ns / 1000000.0:.3f} "
            f"decode_shadow_coherence_checks={stream.decode_shadow_coherence_checks} "
            f"decode_shadow_incoherent_checks={stream.decode_shadow_incoherent_checks} "
            f"present_state_traces={stream.present_state_traces} visible_present_pins={stream.visible_present_pins} "
            f"predecode_quarantine_enters={stream.predecode_quarantine_enters} "
            f"predecode_quarantine_exits={stream.predecode_quarantine_exits} "
            f"predecode_quarantine_destroys={stream.predecode_quarantine_destroys} "
            f"export_visible_releases={stream.export_visible_releases} "
            f"export_visible_acquires={stream.export_visible_acquires} "
            f"export_visible_release_missing={stream.export_visible_release_missing} "
            f"decode_pixel_proofs={stream.decode_pixel_proofs} present_pixel_proofs={stream.present_pixel_proofs} "
            f"present_pixel_mismatches={stream.present_pixel_mismatches} private_shadow_pixel_proofs={stream.private_shadow_pixel_proofs} "
            f"private_shadow_pixel_mismatches={stream.private_shadow_pixel_mismatches} pixel_proof_unavailable={stream.pixel_proof_unavailable} "
            f"nondisplay_present_pinned_skips={stream.nondisplay_present_pinned_skips} "
            f"invalid_nondisplay_stale_export_shadows={stream.invalid_nondisplay_stale_export_shadows} "
            f"invalid_presentable_undecoded_surfaces={stream.invalid_presentable_undecoded_surfaces} "
            f"invalid_nondisplay_present_mutations={stream.invalid_nondisplay_present_mutations} "
            f"invalid_present_generations={stream.invalid_present_generations} invalid_visible_without_present_pins={stream.invalid_visible_without_present_pins} "
            f"invalid_stale_private_decode_shadows={stream.invalid_stale_private_decode_shadows} "
            f"invalid_visible_present_states={stream.invalid_visible_present_states} "
            f"invalid_thumbnail_predecode_seeds={stream.invalid_thumbnail_predecode_seeds} "
            f"av1_tile_submit_maps={stream.av1_tile_submit_maps} av1_tile_suspicious={stream.av1_tile_suspicious} "
            f"av1_dpb_maps={stream.av1_dpb_maps} av1_visible_audits={stream.av1_visible_audits} av1_publish_failures={stream.av1_publish_failures} "
            f"export_copy_publish_skips={stream.export_copy_publish_skips}"
        )
    for message, count in profile.chrome_errors.most_common():
        print(f"chrome-error count={count} message=\"{message}\"")
    for event, count in profile.event_counts.most_common(top_events):
        print(f"event count={count} name={event}")

def main() -> int:
    parser = argparse.ArgumentParser(description="Aggregate nvidia-vulkan-vaapi trace logs line by line.")
    parser.add_argument("log", help="Chrome/stderr log containing nvidia-vulkan-vaapi trace lines, or '-' for stdin")
    parser.add_argument("--json", action="store_true", help="print machine-readable JSON")
    parser.add_argument("--live", action="store_true", help="print rolling summaries to stderr while reading")
    parser.add_argument("--live-interval-lines", type=int, default=1000, help="input line interval between --live summaries")
    parser.add_argument("--top-events", type=int, default=20, help="number of event counters to print in text mode")
    args = parser.parse_args()

    if args.log == "-":
        source = "-"
        profile = profile_lines(sys.stdin, source, args.live, args.live_interval_lines)
    else:
        log_path = Path(args.log)
        if not log_path.is_file():
            print(f"not a file: {log_path}", file=sys.stderr)
            return 2
        source = str(log_path)
        profile = profile_log(log_path, args.live, args.live_interval_lines)

    if args.json:
        print(json.dumps(profile.to_json(source), indent=2, sort_keys=True))
    else:
        print_text(source, profile, args.top_events)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
