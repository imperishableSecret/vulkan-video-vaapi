#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <va/va.h>
#include <va/va_drm.h>

namespace {

constexpr unsigned int smoke_width = 64;
constexpr unsigned int smoke_height = 64;

int open_render_node(void) {
    const char *override_path = std::getenv("VKVV_RENDER_NODE");
    if (override_path != nullptr && override_path[0] != '\0') {
        int fd = open(override_path, O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            std::fprintf(stderr, "failed to open %s: %s\n", override_path, std::strerror(errno));
        }
        return fd;
    }

    for (int i = 128; i < 138; i++) {
        std::string path = "/dev/dri/renderD" + std::to_string(i);
        int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
        if (fd >= 0) {
            std::printf("using render node %s\n", path.c_str());
            return fd;
        }
    }

    std::fprintf(stderr, "failed to open a DRM render node under /dev/dri/renderD128..137\n");
    return -1;
}

bool expect_status(VAStatus actual, VAStatus expected, const char *operation) {
    if (actual == expected) {
        return true;
    }
    std::fprintf(stderr, "%s returned %s (%d), expected %s (%d)\n",
                 operation, vaErrorStr(actual), actual, vaErrorStr(expected), expected);
    return false;
}

bool check_va(VAStatus status, const char *operation) {
    return expect_status(status, VA_STATUS_SUCCESS, operation);
}

} // namespace

int main(void) {
    int fd = open_render_node();
    if (fd < 0) {
        return 1;
    }

    VADisplay display = vaGetDisplayDRM(fd);
    if (!vaDisplayIsValid(display)) {
        std::fprintf(stderr, "vaGetDisplayDRM returned an invalid display\n");
        close(fd);
        return 1;
    }

    int major = 0;
    int minor = 0;
    if (!check_va(vaInitialize(display, &major, &minor), "vaInitialize")) {
        close(fd);
        return 1;
    }

    VAConfigID config = VA_INVALID_ID;
    VAConfigAttrib config_attrib{};
    config_attrib.type = VAConfigAttribRTFormat;
    config_attrib.value = VA_RT_FORMAT_YUV420;
    bool ok = check_va(vaCreateConfig(display, VAProfileH264High, VAEntrypointVLD,
                                      &config_attrib, 1, &config),
                       "vaCreateConfig");

    VASurfaceID surface = VA_INVALID_SURFACE;
    if (ok) {
        ok = check_va(vaCreateSurfaces(display, VA_RT_FORMAT_YUV420, smoke_width, smoke_height,
                                       &surface, 1, nullptr, 0),
                      "vaCreateSurfaces");
    }

    VAContextID context = VA_INVALID_ID;
    if (ok) {
        ok = check_va(vaCreateContext(display, config, smoke_width, smoke_height, VA_PROGRESSIVE,
                                      &surface, 1, &context),
                      "vaCreateContext");
    }

    if (ok) {
        ok = expect_status(vaSyncSurface(display, VA_INVALID_SURFACE),
                           VA_STATUS_ERROR_INVALID_SURFACE,
                           "vaSyncSurface(invalid)") && ok;
        VASurfaceStatus surface_status = VASurfaceRendering;
        ok = expect_status(vaQuerySurfaceStatus(display, VA_INVALID_SURFACE, &surface_status),
                           VA_STATUS_ERROR_INVALID_SURFACE,
                           "vaQuerySurfaceStatus(invalid)") && ok;
        ok = check_va(vaQuerySurfaceStatus(display, surface, &surface_status),
                      "vaQuerySurfaceStatus(ready)") &&
             surface_status == VASurfaceReady && ok;
        if (surface_status != VASurfaceReady) {
            std::fprintf(stderr, "new surface status=%d, expected VASurfaceReady\n", surface_status);
        }
        ok = check_va(vaSyncSurface2(display, surface, 0), "vaSyncSurface2(ready, 0)") && ok;
    }

    if (ok) {
        ok = check_va(vaBeginPicture(display, context, surface), "vaBeginPicture") && ok;
        VASurfaceStatus surface_status = VASurfaceReady;
        ok = check_va(vaQuerySurfaceStatus(display, surface, &surface_status),
                      "vaQuerySurfaceStatus(rendering)") &&
             surface_status == VASurfaceRendering && ok;
        if (surface_status != VASurfaceRendering) {
            std::fprintf(stderr, "active surface status=%d, expected VASurfaceRendering\n", surface_status);
        }
        ok = expect_status(vaSyncSurface2(display, surface, 0),
                           VA_STATUS_ERROR_TIMEDOUT,
                           "vaSyncSurface2(rendering, 0)") && ok;
    }

    if (context != VA_INVALID_ID) {
        ok = check_va(vaDestroyContext(display, context), "vaDestroyContext") && ok;
        context = VA_INVALID_ID;
    }
    if (ok && surface != VA_INVALID_SURFACE) {
        VASurfaceStatus surface_status = VASurfaceRendering;
        ok = check_va(vaQuerySurfaceStatus(display, surface, &surface_status),
                      "vaQuerySurfaceStatus(after destroy context)") &&
             surface_status == VASurfaceReady && ok;
        if (surface_status != VASurfaceReady) {
            std::fprintf(stderr, "surface status after context destroy=%d, expected VASurfaceReady\n",
                         surface_status);
        }
    }

    if (surface != VA_INVALID_SURFACE) {
        ok = check_va(vaDestroySurfaces(display, &surface, 1), "vaDestroySurfaces") && ok;
    }
    if (config != VA_INVALID_ID) {
        ok = check_va(vaDestroyConfig(display, config), "vaDestroyConfig") && ok;
    }
    ok = check_va(vaTerminate(display), "vaTerminate") && ok;
    close(fd);

    if (!ok) {
        return 1;
    }
    std::printf("VA surface sync/status smoke passed\n");
    return 0;
}
