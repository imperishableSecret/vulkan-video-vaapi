#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
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

    bool initialize_and_terminate_once(void) {
        int fd = open_render_node();
        if (fd < 0) {
            return false;
        }

        VADisplay display = vaGetDisplayDRM(fd);
        if (!vaDisplayIsValid(display)) {
            std::fprintf(stderr, "first vaGetDisplayDRM returned an invalid display\n");
            close(fd);
            return false;
        }

        int major = 0;
        int minor = 0;
        if (!check_va(vaInitialize(display, &major, &minor), "first vaInitialize")) {
            close(fd);
            return false;
        }

        const char* vendor = vaQueryVendorString(display);
        std::printf("first display: VA-API %d.%d vendor=%s\n", major, minor, vendor != nullptr ? vendor : "(null)");

        bool ok = check_va(vaTerminate(display), "first vaTerminate");
        close(fd);
        return ok;
    }

    bool initialize_and_export_second_display(void) {
        int fd = open_render_node();
        if (fd < 0) {
            return false;
        }

        VADisplay display = vaGetDisplayDRM(fd);
        if (!vaDisplayIsValid(display)) {
            std::fprintf(stderr, "second vaGetDisplayDRM returned an invalid display\n");
            close(fd);
            return false;
        }

        int major = 0;
        int minor = 0;
        if (!check_va(vaInitialize(display, &major, &minor), "second vaInitialize")) {
            close(fd);
            return false;
        }

        const char* vendor = vaQueryVendorString(display);
        std::printf("second display: VA-API %d.%d vendor=%s\n", major, minor, vendor != nullptr ? vendor : "(null)");

        VAConfigAttrib rt_format{};
        rt_format.type = VAConfigAttribRTFormat;
        if (!check_va(vaGetConfigAttributes(display, VAProfileH264High, VAEntrypointVLD, &rt_format, 1), "vaGetConfigAttributes")) {
            vaTerminate(display);
            close(fd);
            return false;
        }
        if ((rt_format.value & VA_RT_FORMAT_YUV420) == 0) {
            std::fprintf(stderr, "H.264 High does not advertise VA_RT_FORMAT_YUV420: 0x%x\n", rt_format.value);
            vaTerminate(display);
            close(fd);
            return false;
        }

        VAConfigID     config = VA_INVALID_ID;
        VAConfigAttrib config_attrib{};
        config_attrib.type  = VAConfigAttribRTFormat;
        config_attrib.value = VA_RT_FORMAT_YUV420;
        if (!check_va(vaCreateConfig(display, VAProfileH264High, VAEntrypointVLD, &config_attrib, 1, &config), "vaCreateConfig")) {
            vaTerminate(display);
            close(fd);
            return false;
        }

        VASurfaceID surface = VA_INVALID_ID;
        if (!check_va(vaCreateSurfaces(display, VA_RT_FORMAT_YUV420, smoke_width, smoke_height, &surface, 1, nullptr, 0), "vaCreateSurfaces")) {
            vaDestroyConfig(display, config);
            vaTerminate(display);
            close(fd);
            return false;
        }

        VAContextID context = VA_INVALID_ID;
        if (!check_va(vaCreateContext(display, config, smoke_width, smoke_height, VA_PROGRESSIVE, &surface, 1, &context), "vaCreateContext")) {
            vaDestroySurfaces(display, &surface, 1);
            vaDestroyConfig(display, config);
            vaTerminate(display);
            close(fd);
            return false;
        }

        VADRMPRIMESurfaceDescriptor descriptor{};
        if (!check_va(vaExportSurfaceHandle(display, surface, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS, &descriptor),
                      "vaExportSurfaceHandle")) {
            vaDestroyContext(display, context);
            vaDestroySurfaces(display, &surface, 1);
            vaDestroyConfig(display, config);
            vaTerminate(display);
            close(fd);
            return false;
        }

        std::printf("exported surface: objects=%u layers=%u fd=%d size=%u\n", descriptor.num_objects, descriptor.num_layers,
                    descriptor.num_objects > 0 ? descriptor.objects[0].fd : -1, descriptor.num_objects > 0 ? descriptor.objects[0].size : 0);
        close_descriptor_fds(&descriptor);

        bool ok = true;
        ok      = check_va(vaDestroyContext(display, context), "vaDestroyContext") && ok;
        ok      = check_va(vaDestroySurfaces(display, &surface, 1), "vaDestroySurfaces") && ok;
        ok      = check_va(vaDestroyConfig(display, config), "vaDestroyConfig") && ok;
        ok      = check_va(vaTerminate(display), "second vaTerminate") && ok;
        close(fd);
        return ok;
    }

} // namespace

int main(void) {
    if (!initialize_and_terminate_once()) {
        return 1;
    }
    if (!initialize_and_export_second_display()) {
        return 1;
    }
    std::printf("VA display lifetime smoke passed\n");
    return 0;
}
