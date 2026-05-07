#include "driver.h"

#include <cstdio>

int main(void) {
    VkvvVideoCaps caps;
    const bool ok = vkvv_probe_vulkan_video(&caps);

    std::puts(caps.summary);
    std::printf("h264=%d\n", caps.h264);
    std::printf("h265=%d\n", caps.h265);
    std::printf("h265_10=%d\n", caps.h265_10);
    std::printf("vp9=%d\n", caps.vp9);
    std::printf("av1=%d\n", caps.av1);
    std::printf("surface_export=%d\n", caps.surface_export);

    return ok ? 0 : 1;
}
