#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>

namespace {

    constexpr unsigned int smoke_width  = 64;
    constexpr unsigned int smoke_height = 64;

    int                    open_render_node(void) {
        const char* override_path = std::getenv("VKVV_RENDER_NODE");
        if (override_path != nullptr && override_path[0] != '\0') {
            int fd = open(override_path, O_RDWR | O_CLOEXEC);
            if (fd < 0) {
                std::fprintf(stderr, "failed to open %s: %s\n", override_path, std::strerror(errno));
            }
            return fd;
        }

        for (int i = 128; i < 138; i++) {
            std::string path = "/dev/dri/renderD" + std::to_string(i);
            int         fd   = open(path.c_str(), O_RDWR | O_CLOEXEC);
            if (fd >= 0) {
                std::printf("using render node %s\n", path.c_str());
                return fd;
            }
        }

        std::fprintf(stderr, "failed to open a DRM render node under /dev/dri/renderD128..137\n");
        return -1;
    }

    bool check_va(VAStatus status, const char* operation) {
        if (status == VA_STATUS_SUCCESS) {
            return true;
        }
        std::fprintf(stderr, "%s failed: %s (%d)\n", operation, vaErrorStr(status), status);
        return false;
    }

    void close_descriptor_fds(VADRMPRIMESurfaceDescriptor* descriptor) {
        for (uint32_t i = 0; i < descriptor->num_objects; i++) {
            if (descriptor->objects[i].fd >= 0) {
                close(descriptor->objects[i].fd);
                descriptor->objects[i].fd = -1;
            }
        }
    }

    bool export_surface(VADisplay display, VASurfaceID surface, const char* label) {
        VADRMPRIMESurfaceDescriptor descriptor{};
        if (!check_va(vaExportSurfaceHandle(display, surface, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS, &descriptor),
                      label)) {
            return false;
        }

        std::printf("%s: objects=%u layers=%u fd=%d size=%u\n", label, descriptor.num_objects, descriptor.num_layers, descriptor.num_objects > 0 ? descriptor.objects[0].fd : -1,
                    descriptor.num_objects > 0 ? descriptor.objects[0].size : 0);
        close_descriptor_fds(&descriptor);
        return true;
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

    bool        initialized      = false;
    VAConfigID  config           = VA_INVALID_ID;
    VASurfaceID surfaces[2]      = {VA_INVALID_ID, VA_INVALID_ID};
    bool        surfaces_created = false;
    VAContextID contexts[2]      = {VA_INVALID_ID, VA_INVALID_ID};

    auto        cleanup = [&]() {
        for (int i = 1; i >= 0; i--) {
            if (contexts[i] != VA_INVALID_ID) {
                vaDestroyContext(display, contexts[i]);
                contexts[i] = VA_INVALID_ID;
            }
        }
        if (surfaces_created) {
            vaDestroySurfaces(display, surfaces, 2);
        }
        if (config != VA_INVALID_ID) {
            vaDestroyConfig(display, config);
        }
        if (initialized) {
            vaTerminate(display);
        }
        close(fd);
    };

    int major = 0;
    int minor = 0;
    if (!check_va(vaInitialize(display, &major, &minor), "vaInitialize")) {
        cleanup();
        return 1;
    }
    initialized = true;
    std::printf("display: VA-API %d.%d vendor=%s\n", major, minor, vaQueryVendorString(display));

    VAConfigAttrib config_attrib{};
    config_attrib.type  = VAConfigAttribRTFormat;
    config_attrib.value = VA_RT_FORMAT_YUV420;
    if (!check_va(vaCreateConfig(display, VAProfileH264High, VAEntrypointVLD, &config_attrib, 1, &config), "vaCreateConfig")) {
        cleanup();
        return 1;
    }

    if (!check_va(vaCreateSurfaces(display, VA_RT_FORMAT_YUV420, smoke_width, smoke_height, surfaces, 2, nullptr, 0), "vaCreateSurfaces")) {
        cleanup();
        return 1;
    }
    surfaces_created = true;

    if (!check_va(vaCreateContext(display, config, smoke_width, smoke_height, VA_PROGRESSIVE, &surfaces[0], 1, &contexts[0]), "vaCreateContext[0]") ||
        !check_va(vaCreateContext(display, config, smoke_width, smoke_height, VA_PROGRESSIVE, &surfaces[1], 1, &contexts[1]), "vaCreateContext[1]")) {
        cleanup();
        return 1;
    }

    bool        export_ok[2]      = {};
    std::thread export_threads[2] = {
        std::thread([&]() { export_ok[0] = export_surface(display, surfaces[0], "parallel export surface 0"); }),
        std::thread([&]() { export_ok[1] = export_surface(display, surfaces[1], "parallel export surface 1"); }),
    };
    export_threads[0].join();
    export_threads[1].join();
    if (!export_ok[0] || !export_ok[1]) {
        cleanup();
        return 1;
    }

    if (!check_va(vaDestroyContext(display, contexts[0]), "vaDestroyContext[0]")) {
        cleanup();
        return 1;
    }
    contexts[0] = VA_INVALID_ID;

    VASurfaceStatus status = VASurfaceRendering;
    if (!check_va(vaQuerySurfaceStatus(display, surfaces[1], &status), "vaQuerySurfaceStatus[1]") || status != VASurfaceReady) {
        std::fprintf(stderr, "surface 1 was not ready after destroying context 0: %d\n", status);
        cleanup();
        return 1;
    }

    if (!export_surface(display, surfaces[1], "export surface 1 after context 0 destroy")) {
        cleanup();
        return 1;
    }

    cleanup();
    std::printf("multi-context smoke passed\n");
    return 0;
}
