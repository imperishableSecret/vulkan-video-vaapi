#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <drm_fourcc.h>
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

    bool descriptor_fds_are_open(const VADRMPRIMESurfaceDescriptor& descriptor, const char* label) {
        for (uint32_t i = 0; i < descriptor.num_objects; i++) {
            if (descriptor.objects[i].fd < 0 || fcntl(descriptor.objects[i].fd, F_GETFD) < 0) {
                std::fprintf(stderr, "%s exported fd %u is not valid after surface destroy\n", label, i);
                return false;
            }
        }
        return true;
    }

    bool validate_nv12_descriptor(const VADRMPRIMESurfaceDescriptor& descriptor, const char* label) {
        if (descriptor.fourcc != VA_FOURCC_NV12 || descriptor.width != smoke_width || descriptor.height != smoke_height || descriptor.num_objects != 1 ||
            descriptor.objects[0].fd < 0 || descriptor.objects[0].size == 0 || descriptor.num_layers != 2 || descriptor.layers[0].drm_format != DRM_FORMAT_R8 ||
            descriptor.layers[0].num_planes != 1 || descriptor.layers[0].object_index[0] != 0 || descriptor.layers[0].pitch[0] == 0 ||
            descriptor.layers[1].drm_format != DRM_FORMAT_GR88 || descriptor.layers[1].num_planes != 1 || descriptor.layers[1].object_index[0] != 0 ||
            descriptor.layers[1].pitch[0] == 0) {
            std::fprintf(stderr, "%s returned unexpected descriptor: fourcc=0x%x size=%ux%u objects=%u layers=%u fd=%d obj_size=%u y=0x%x/%u uv=0x%x/%u\n", label,
                         descriptor.fourcc, descriptor.width, descriptor.height, descriptor.num_objects, descriptor.num_layers,
                         descriptor.num_objects > 0 ? descriptor.objects[0].fd : -1, descriptor.num_objects > 0 ? descriptor.objects[0].size : 0,
                         descriptor.num_layers > 0 ? descriptor.layers[0].drm_format : 0, descriptor.num_layers > 0 ? descriptor.layers[0].pitch[0] : 0,
                         descriptor.num_layers > 1 ? descriptor.layers[1].drm_format : 0, descriptor.num_layers > 1 ? descriptor.layers[1].pitch[0] : 0);
            return false;
        }
        return true;
    }

    bool export_and_validate(VADisplay display, VASurfaceID surface, VADRMPRIMESurfaceDescriptor* descriptor, const char* label) {
        *descriptor = {};
        if (!check_va(vaExportSurfaceHandle(display, surface, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS, descriptor),
                      label)) {
            return false;
        }
        std::printf("%s: objects=%u layers=%u fd=%d size=%u\n", label, descriptor->num_objects, descriptor->num_layers,
                    descriptor->num_objects > 0 ? descriptor->objects[0].fd : -1, descriptor->num_objects > 0 ? descriptor->objects[0].size : 0);
        return validate_nv12_descriptor(*descriptor, label);
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

    bool        initialized = false;
    VAConfigID  config      = VA_INVALID_ID;
    VASurfaceID surface     = VA_INVALID_SURFACE;

    auto        cleanup = [&]() {
        if (surface != VA_INVALID_SURFACE) {
            vaDestroySurfaces(display, &surface, 1);
            surface = VA_INVALID_SURFACE;
        }
        if (config != VA_INVALID_ID) {
            vaDestroyConfig(display, config);
            config = VA_INVALID_ID;
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

    VAConfigAttrib config_attrib{};
    config_attrib.type  = VAConfigAttribRTFormat;
    config_attrib.value = VA_RT_FORMAT_YUV420;
    if (!check_va(vaCreateConfig(display, VAProfileH264High, VAEntrypointVLD, &config_attrib, 1, &config), "vaCreateConfig")) {
        cleanup();
        return 1;
    }

    if (!check_va(vaCreateSurfaces(display, VA_RT_FORMAT_YUV420, smoke_width, smoke_height, &surface, 1, nullptr, 0), "vaCreateSurfaces[0]")) {
        cleanup();
        return 1;
    }

    VADRMPRIMESurfaceDescriptor first_export{};
    VADRMPRIMESurfaceDescriptor second_export{};
    if (!export_and_validate(display, surface, &first_export, "pre-decode export 0") || !export_and_validate(display, surface, &second_export, "pre-decode export 1")) {
        close_descriptor_fds(&first_export);
        close_descriptor_fds(&second_export);
        cleanup();
        return 1;
    }

    if (!check_va(vaDestroySurfaces(display, &surface, 1), "vaDestroySurfaces[0]")) {
        close_descriptor_fds(&first_export);
        close_descriptor_fds(&second_export);
        cleanup();
        return 1;
    }
    surface = VA_INVALID_SURFACE;

    if (!descriptor_fds_are_open(first_export, "first export") || !descriptor_fds_are_open(second_export, "second export")) {
        close_descriptor_fds(&first_export);
        close_descriptor_fds(&second_export);
        cleanup();
        return 1;
    }
    close_descriptor_fds(&first_export);
    close_descriptor_fds(&second_export);

    if (!check_va(vaCreateSurfaces(display, VA_RT_FORMAT_YUV420, smoke_width, smoke_height, &surface, 1, nullptr, 0), "vaCreateSurfaces[1]")) {
        cleanup();
        return 1;
    }

    VADRMPRIMESurfaceDescriptor recreated_export{};
    if (!export_and_validate(display, surface, &recreated_export, "recreated surface export")) {
        close_descriptor_fds(&recreated_export);
        cleanup();
        return 1;
    }
    close_descriptor_fds(&recreated_export);

    cleanup();
    std::printf("VA export lifecycle smoke passed\n");
    return 0;
}
