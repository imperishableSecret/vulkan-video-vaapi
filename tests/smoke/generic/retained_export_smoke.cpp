#include "vulkan_runtime_internal.h"

#include <cstdio>
#include <string_view>

namespace {

constexpr unsigned int fourcc_nv12 = 0x3231564e;
constexpr unsigned int fourcc_p010 = 0x30313050;

bool check(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
    }
    return condition;
}

vkvv::RetainedExportBacking make_backing() {
    vkvv::RetainedExportBacking backing{};
    backing.resource.fd_stat_valid = true;
    backing.resource.fd_dev = 100;
    backing.resource.fd_ino = 200;
    backing.resource.va_fourcc = fourcc_nv12;
    backing.resource.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    backing.resource.extent = {1920, 1088};
    backing.resource.allocation_size = 1920 * 1088 * 3 / 2;
    backing.fd = vkvv::retained_export_fd_identity(backing.resource);
    return backing;
}

VkvvExternalSurfaceImport make_import() {
    VkvvExternalSurfaceImport import{};
    import.external = true;
    import.fd.valid = true;
    import.fd.dev = 100;
    import.fd.ino = 200;
    import.fourcc = fourcc_nv12;
    import.width = 1920;
    import.height = 1080;
    return import;
}

bool expect_match(
        const vkvv::RetainedExportBacking &backing,
        const VkvvExternalSurfaceImport &import,
        unsigned int fourcc,
        VkFormat format,
        VkExtent2D extent,
        vkvv::RetainedExportMatch expected,
        const char *message) {
    const vkvv::RetainedExportMatch actual =
        vkvv::retained_export_match_import(backing, import, fourcc, format, extent);
    if (actual != expected) {
        std::fprintf(stderr, "%s: expected %s got %s\n",
                     message,
                     vkvv::retained_export_match_reason(expected),
                     vkvv::retained_export_match_reason(actual));
        return false;
    }
    return true;
}

} // namespace

int main() {
    const vkvv::RetainedExportBacking backing = make_backing();
    VkvvExternalSurfaceImport import = make_import();

    bool ok = true;
    ok &= check(backing.fd.valid && backing.fd.dev == 100 && backing.fd.ino == 200,
                "retained fd identity was not copied from export resource");
    ok &= expect_match(backing, import, fourcc_nv12,
                       VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, {1920, 1088},
                       vkvv::RetainedExportMatch::Match,
                       "matching import");

    import.external = false;
    ok &= expect_match(backing, import, fourcc_nv12,
                       VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, {1920, 1088},
                       vkvv::RetainedExportMatch::MissingImport,
                       "missing external import");

    import = make_import();
    import.fd.ino = 201;
    ok &= expect_match(backing, import, fourcc_nv12,
                       VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, {1920, 1088},
                       vkvv::RetainedExportMatch::FdMismatch,
                       "wrong fd identity");

    import = make_import();
    ok &= expect_match(backing, import, fourcc_p010,
                       VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, {1920, 1088},
                       vkvv::RetainedExportMatch::FourccMismatch,
                       "wrong fourcc");

    ok &= expect_match(backing, import, fourcc_nv12,
                       VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16, {1920, 1088},
                       vkvv::RetainedExportMatch::FormatMismatch,
                       "wrong Vulkan format");

    ok &= expect_match(backing, import, fourcc_nv12,
                       VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, {3840, 2160},
                       vkvv::RetainedExportMatch::ExtentMismatch,
                       "undersized retained backing");

    ok &= check(std::string_view(vkvv::retained_export_match_reason(vkvv::RetainedExportMatch::Match)) == "match",
                "match reason text should be stable");
    return ok ? 0 : 1;
}
