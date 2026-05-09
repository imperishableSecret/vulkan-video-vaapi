#include "va/driver.h"

#include <cstdio>

namespace {

    bool check(bool condition, const char* message) {
        if (!condition) {
            std::fprintf(stderr, "probe smoke failed: %s\n", message);
            return false;
        }
        return true;
    }

    bool limits_valid(const VkvvVideoProfileLimits& limits) {
        return limits.max_width >= limits.min_width && limits.max_height >= limits.min_height && limits.max_width != 0 && limits.max_height != 0;
    }

} // namespace

int main() {
    VkvvVideoCaps caps{};
    const bool    ok = vkvv_probe_vulkan_video(&caps);
    std::puts(caps.summary);
    if (!ok) {
        return 1;
    }

    bool valid = true;
    if (caps.h264) {
        valid = check(limits_valid(caps.h264_limits), "H.264 decode limits should be populated when decode is supported") && valid;
    }
    if (caps.h264_encode) {
        valid = check(limits_valid(caps.h264_encode_limits), "H.264 encode limits should be populated when encode is supported") && valid;
    }
    valid = check(caps.h264 || caps.h265 || caps.h265_10 || caps.h265_12 || caps.vp9 || caps.vp9_10 || caps.vp9_12 || caps.av1 || caps.av1_10 || caps.h264_encode,
                  "probe should report at least one decode or encode capability") &&
        valid;
    if (!valid) {
        return 1;
    }

    std::printf("probe smoke passed\n");
    return 0;
}
