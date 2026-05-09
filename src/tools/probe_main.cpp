#include "va/driver.h"

#include <cstdio>

int main(void) {
    VkvvVideoCaps caps;
    const bool    ok = vkvv_probe_vulkan_video(&caps);

    std::puts(caps.summary);
    std::printf("h264=%d\n", caps.h264);
    std::printf("h265=%d\n", caps.h265);
    std::printf("h265_10=%d\n", caps.h265_10);
    std::printf("h265_12=%d\n", caps.h265_12);
    std::printf("vp9=%d\n", caps.vp9);
    std::printf("vp9_10=%d\n", caps.vp9_10);
    std::printf("vp9_12=%d\n", caps.vp9_12);
    std::printf("av1=%d\n", caps.av1);
    std::printf("av1_10=%d\n", caps.av1_10);
    std::printf("surface_export=%d\n", caps.surface_export);
    std::printf("surface_export_nv12=%d\n", caps.surface_export_nv12);
    std::printf("surface_export_p010=%d\n", caps.surface_export_p010);
    std::printf("surface_export_p012=%d\n", caps.surface_export_p012);

    return ok ? 0 : 1;
}
