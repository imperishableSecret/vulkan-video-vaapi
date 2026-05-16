#include "vulkan/runtime_internal.h"

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

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
