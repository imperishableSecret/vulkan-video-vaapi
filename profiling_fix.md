# VKVV Profiling Fix

Status: implemented in phases.

## Model

The driver profiling model is now trace-first:

- `VKVV_TRACE=1` enables raw `nvidia-vulkan-vaapi: trace seq=... event=...` records.
- `VKVV_TRACE=deep`, `VKVV_TRACE=2`, or `VKVV_TRACE=verbose` keeps the existing deeper diagnostic trace mode.
- `VKVV_LOG=1` remains human debug logging.
- `VKVV_PERF` driver aggregation has been removed.

The driver no longer owns profiler aggregates. It emits raw events, and `tools/profile_trace_log.py` performs all aggregation from those records.

## Implemented Phases

### Phase 1: Accurate Drop-Like Aggregates

The profiler now exposes explicit driver-visible drop categories instead of hiding them behind stream-local state:

- `driver_stale_drops`
- `predecode_stale_drops`
- `export_seed_stale_drops`
- `nondisplay_refresh_skips`
- `stale_visible_nondisplay`
- `export_copy_publish_skips`

These counters are counted globally before stream identity resolution. Events with `stream=0` or otherwise incomplete identity are still visible in top-level totals. Stream and codec aggregates include the same categories when identity is available.

The profiler still does not pretend to report YouTube's browser/player dropped-frame counter. It reports `browser_dropped_frames_observed=false` unless an explicit browser-side counter is added later.

### Phase 2: Streaming Input

The profiler accepts both files and stdin:

```bash
tools/profile_trace_log.py chrome.log --json
VKVV_TRACE=1 google-chrome ... 2>&1 | tools/profile_trace_log.py - --json
VKVV_TRACE=1 google-chrome ... 2>&1 | tools/profile_trace_log.py - --live
```

`--live` prints rolling summaries to stderr while preserving final text or JSON output on stdout.

### Phase 3: Driver Perf Removal

The legacy in-driver aggregation path was removed:

- `vkvv_perf_enabled()` and `VKVV_PERF` handling
- `VkvvPerfCounters` / codec perf counters
- perf high-water helpers
- decode submitted/completed perf counter updates
- fence/export/session/retained perf counter updates
- `vkvv_vulkan_flush_perf_summary()`
- perf summary smoke tests and telemetry perf env

Raw trace events remain for the metrics the profiler needs, including fence waits, export-copy metrics, image/session memory, retained export removal, and stale/drop-like events.

### Phase 4: Trace Contract

The profiler no longer consumes legacy `perf`, `perf-stream`, or `perf-sample` lines. Aggregates come from raw trace records, plus separately parsed `device-lost` and Chrome VAAPI error lines.

## File Writing And Pipe Limits

The driver has no explicit trace file-size cap. It writes trace lines to stderr with sequence numbers. Real limits are outside the driver: disk space, wrapper log rotation, terminal capture behavior, pipe buffering, and profiler read speed.

Loss or truncation is detectable through `seq_missing` in profiler output.

## Validation

Validated successfully with:

```bash
python3 -m py_compile tools/profile_trace_log.py tests/smoke/generic/trace_log_profiler_smoke.py
meson test -C build smoke-trace-log-profiler smoke-telemetry-patterns smoke-telemetry-off smoke-telemetry-on smoke-telemetry-log-on smoke-telemetry-deep --print-errorlogs
make all
graphify update .
```
