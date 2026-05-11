#include "vulkan/runtime_internal.h"

#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <iostream>

namespace {

    bool check(bool condition, const char* message) {
        if (!condition) {
            std::cerr << message << "\n";
            return false;
        }
        return true;
    }

} // namespace

int main() {
    vkvv::VkvvPerfCounters counters{};
    bool                   ok = true;

    vkvv::perf_update_high_water(counters.upload_high_water, 64);
    vkvv::perf_update_high_water(counters.upload_high_water, 32);
    ok = check(counters.upload_high_water.load(std::memory_order_relaxed) == 64, "high-water counter regressed") && ok;

    auto* h264 = vkvv::perf_decode_codec_counters(&counters, VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR);
    auto* h265 = vkvv::perf_decode_codec_counters(&counters, VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR);
    auto* vp9  = vkvv::perf_decode_codec_counters(&counters, VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR);
    auto* av1  = vkvv::perf_decode_codec_counters(&counters, VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR);
    ok         = check(h264 == &counters.h264_decode, "H.264 perf counter routing failed") && ok;
    ok         = check(h265 == &counters.h265_decode, "HEVC perf counter routing failed") && ok;
    ok         = check(vp9 == &counters.vp9_decode, "VP9 perf counter routing failed") && ok;
    ok         = check(av1 == &counters.av1_decode, "AV1 perf counter routing failed") && ok;
    ok         = check(vkvv::perf_decode_codec_counters(&counters, 0) == nullptr, "unknown codec should not have perf counters") && ok;

    h264->submitted.fetch_add(1, std::memory_order_relaxed);
    h264->completed.fetch_add(1, std::memory_order_relaxed);
    ok = check(counters.h264_decode.submitted.load(std::memory_order_relaxed) == 1, "codec submitted counter failed") && ok;
    ok = check(counters.h264_decode.completed.load(std::memory_order_relaxed) == 1, "codec completed counter failed") && ok;

    ok = check(std::strcmp(vkvv::command_use_name(vkvv::CommandUse::Idle), "idle") == 0, "idle command-use label failed") && ok;
    ok = check(std::strcmp(vkvv::command_use_name(vkvv::CommandUse::Decode), "decode") == 0, "decode command-use label failed") && ok;
    ok = check(std::strcmp(vkvv::command_use_name(vkvv::CommandUse::Export), "export") == 0, "export command-use label failed") && ok;
    ok = check(std::strcmp(vkvv::command_use_name(vkvv::CommandUse::SessionReset), "session-reset") == 0, "session-reset command-use label failed") && ok;
    ok = check(std::strcmp(vkvv::command_use_name(vkvv::CommandUse::Encode), "encode") == 0, "encode command-use label failed") && ok;

    vkvv::CommandSlot slot{};
    ok = check(slot.submitted_use == vkvv::CommandUse::Idle, "command slot submitted-use should default idle") && ok;
    ok = check(slot.pending.use == vkvv::CommandUse::Idle, "command slot pending-use should default idle") && ok;
    ok = check(!vkvv::pending_work_has_payload(slot.pending), "empty command slot should not carry pending payload") && ok;

    VkvvSurface surface{};
    slot.pending.surface    = &surface;
    slot.pending.parameters = reinterpret_cast<VkVideoSessionParametersKHR>(static_cast<uintptr_t>(1));
    slot.pending.use        = vkvv::CommandUse::Decode;
    ok                      = check(vkvv::pending_work_has_payload(slot.pending), "pending-work payload helper missed active work") && ok;
    ok                      = check(!vkvv::pending_work_surface_is_destroying(slot.pending), "fresh pending surface should not be marked destroying") && ok;
    surface.destroying      = true;
    ok                      = check(vkvv::pending_work_surface_is_destroying(slot.pending), "pending-work helper missed destroying surface") && ok;
    vkvv::reset_pending_work(&slot.pending);
    ok = check(!vkvv::pending_work_has_payload(slot.pending) && slot.pending.use == vkvv::CommandUse::Idle && slot.pending.refresh_export,
               "pending-work reset did not restore defaults") &&
        ok;

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
