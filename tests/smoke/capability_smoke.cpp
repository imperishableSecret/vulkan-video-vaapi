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
#include <vector>

namespace {

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

bool profile_present(const std::vector<VAProfile> &profiles, int profile_count, VAProfile profile) {
    for (int i = 0; i < profile_count; i++) {
        if (profiles[i] == profile) {
            return true;
        }
    }
    return false;
}

bool find_integer_attrib(
        const std::vector<VASurfaceAttrib> &attribs,
        VASurfaceAttribType type,
        int *value) {
    for (const VASurfaceAttrib &attrib : attribs) {
        if (attrib.type == type && attrib.value.type == VAGenericValueTypeInteger) {
            *value = attrib.value.value.i;
            return true;
        }
    }
    return false;
}

bool check_h264_profile(VADisplay display, VAProfile profile) {
    VAEntrypoint entrypoints[4] = {};
    int entrypoint_count = 0;
    if (!check_va(vaQueryConfigEntrypoints(display, profile, entrypoints, &entrypoint_count),
                  "vaQueryConfigEntrypoints")) {
        return false;
    }
    if (entrypoint_count != 1 || entrypoints[0] != VAEntrypointVLD) {
        std::fprintf(stderr, "profile %d returned unexpected entrypoints: count=%d first=%d\n",
                     profile, entrypoint_count, entrypoints[0]);
        return false;
    }

    VAConfigAttrib attribs[4] = {};
    attribs[0].type = VAConfigAttribRTFormat;
    attribs[1].type = VAConfigAttribDecSliceMode;
    attribs[2].type = VAConfigAttribMaxPictureWidth;
    attribs[3].type = VAConfigAttribMaxPictureHeight;
    if (!check_va(vaGetConfigAttributes(display, profile, VAEntrypointVLD, attribs, 4),
                  "vaGetConfigAttributes")) {
        return false;
    }
    if ((attribs[0].value & VA_RT_FORMAT_YUV420) == 0 ||
        attribs[1].value != VA_DEC_SLICE_MODE_NORMAL ||
        attribs[2].value == 0 ||
        attribs[3].value == 0) {
        std::fprintf(stderr,
                     "profile %d attributes invalid: rt=0x%x slice=0x%x max=%ux%u\n",
                     profile, attribs[0].value, attribs[1].value,
                     attribs[2].value, attribs[3].value);
        return false;
    }

    VAConfigAttrib create_attrib{};
    create_attrib.type = VAConfigAttribRTFormat;
    create_attrib.value = VA_RT_FORMAT_YUV420;
    VAConfigID config = VA_INVALID_ID;
    if (!check_va(vaCreateConfig(display, profile, VAEntrypointVLD, &create_attrib, 1, &config),
                  "vaCreateConfig")) {
        return false;
    }

    unsigned int surface_attrib_count = 0;
    bool ok = check_va(vaQuerySurfaceAttributes(display, config, nullptr, &surface_attrib_count),
                       "vaQuerySurfaceAttributes(count)");
    std::vector<VASurfaceAttrib> surface_attribs(surface_attrib_count);
    if (ok) {
        ok = check_va(vaQuerySurfaceAttributes(display, config, surface_attribs.data(), &surface_attrib_count),
                      "vaQuerySurfaceAttributes(values)") && ok;
    }

    int min_width = 0;
    int min_height = 0;
    int max_width = 0;
    int max_height = 0;
    int pixel_format = 0;
    int memory_type = 0;
    ok = find_integer_attrib(surface_attribs, VASurfaceAttribMinWidth, &min_width) && ok;
    ok = find_integer_attrib(surface_attribs, VASurfaceAttribMinHeight, &min_height) && ok;
    ok = find_integer_attrib(surface_attribs, VASurfaceAttribMaxWidth, &max_width) && ok;
    ok = find_integer_attrib(surface_attribs, VASurfaceAttribMaxHeight, &max_height) && ok;
    ok = find_integer_attrib(surface_attribs, VASurfaceAttribPixelFormat, &pixel_format) && ok;
    ok = find_integer_attrib(surface_attribs, VASurfaceAttribMemoryType, &memory_type) && ok;
    if (!ok ||
        min_width <= 0 ||
        min_height <= 0 ||
        max_width < min_width ||
        max_height < min_height ||
        pixel_format != static_cast<int>(VA_FOURCC_NV12) ||
        (memory_type & VA_SURFACE_ATTRIB_MEM_TYPE_VA) == 0 ||
        (memory_type & VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2) == 0) {
        std::fprintf(stderr,
                     "surface attributes invalid: min=%dx%d max=%dx%d fourcc=0x%x memory=0x%x\n",
                     min_width, min_height, max_width, max_height, pixel_format, memory_type);
        ok = false;
    }

    ok = check_va(vaDestroyConfig(display, config), "vaDestroyConfig") && ok;
    return ok;
}

bool check_profile_not_advertised(VADisplay display, VAProfile profile, const char *name) {
    bool ok = true;

    VAEntrypoint entrypoints[4] = {};
    int entrypoint_count = 0;
    ok = expect_status(
             vaQueryConfigEntrypoints(display, profile, entrypoints, &entrypoint_count),
             VA_STATUS_ERROR_UNSUPPORTED_PROFILE,
             name) && ok;

    VAConfigAttrib attrib{};
    attrib.type = VAConfigAttribRTFormat;
    ok = expect_status(
             vaGetConfigAttributes(display, profile, VAEntrypointVLD, &attrib, 1),
             VA_STATUS_ERROR_UNSUPPORTED_PROFILE,
             name) && ok;

    VAConfigID config = VA_INVALID_ID;
    ok = expect_status(
             vaCreateConfig(display, profile, VAEntrypointVLD, nullptr, 0, &config),
             VA_STATUS_ERROR_UNSUPPORTED_PROFILE,
             name) && ok;

    return ok;
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

    bool ok = true;
    std::vector<VAProfile> profiles(static_cast<size_t>(vaMaxNumProfiles(display)));
    int profile_count = 0;
    ok = check_va(vaQueryConfigProfiles(display, profiles.data(), &profile_count),
                  "vaQueryConfigProfiles") && ok;
    if (ok && profile_count != 3) {
        std::fprintf(stderr, "expected exactly 3 usable profiles, got %d\n", profile_count);
        ok = false;
    }
    ok = profile_present(profiles, profile_count, VAProfileH264ConstrainedBaseline) && ok;
    ok = profile_present(profiles, profile_count, VAProfileH264Main) && ok;
    ok = profile_present(profiles, profile_count, VAProfileH264High) && ok;
    if (profile_present(profiles, profile_count, VAProfileHEVCMain) ||
        profile_present(profiles, profile_count, VAProfileVP9Profile0) ||
        profile_present(profiles, profile_count, VAProfileAV1Profile0)) {
        std::fprintf(stderr, "driver advertised a profile without wired decode/export\n");
        ok = false;
    }

    ok = check_h264_profile(display, VAProfileH264ConstrainedBaseline) && ok;
    ok = check_h264_profile(display, VAProfileH264Main) && ok;
    ok = check_h264_profile(display, VAProfileH264High) && ok;

    ok = check_profile_not_advertised(display, VAProfileHEVCMain, "HEVC Main advertising") && ok;
    ok = check_profile_not_advertised(display, VAProfileHEVCMain10, "HEVC Main10 advertising") && ok;
    ok = check_profile_not_advertised(display, VAProfileVP9Profile0, "VP9 Profile0 advertising") && ok;
    ok = check_profile_not_advertised(display, VAProfileAV1Profile0, "AV1 Profile0 advertising") && ok;

    VAImageFormat image_formats[4] = {};
    int image_format_count = 0;
    ok = check_va(vaQueryImageFormats(display, image_formats, &image_format_count),
                  "vaQueryImageFormats") && ok;
    if (image_format_count != 1 || image_formats[0].fourcc != VA_FOURCC_NV12) {
        std::fprintf(stderr, "expected only NV12 image format, got count=%d first=0x%x\n",
                     image_format_count, image_formats[0].fourcc);
        ok = false;
    }

    VASurfaceID p010_surface = VA_INVALID_SURFACE;
    ok = expect_status(
             vaCreateSurfaces(display, VA_RT_FORMAT_YUV420_10, 64, 64, &p010_surface, 1, nullptr, 0),
             VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT,
             "vaCreateSurfaces(P010)") && ok;

    ok = check_va(vaTerminate(display), "vaTerminate") && ok;
    close(fd);

    if (!ok) {
        return 1;
    }
    std::printf("VA capability/attribute smoke passed\n");
    return 0;
}
