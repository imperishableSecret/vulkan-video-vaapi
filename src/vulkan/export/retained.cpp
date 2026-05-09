#include "vulkan_runtime_internal.h"

namespace vkvv {

VkvvFdIdentity retained_export_fd_identity(const ExportResource &resource) {
    VkvvFdIdentity identity{};
    identity.valid = resource.fd_stat_valid;
    identity.dev = resource.fd_dev;
    identity.ino = resource.fd_ino;
    return identity;
}

RetainedExportMatch retained_export_match_import(
        const RetainedExportBacking &backing,
        const VkvvExternalSurfaceImport &import,
        unsigned int va_fourcc,
        VkFormat format,
        VkExtent2D coded_extent) {
    if (!import.external) {
        return RetainedExportMatch::MissingImport;
    }
    if (!import.fd.valid || !backing.fd.valid) {
        return RetainedExportMatch::MissingFd;
    }
    if (backing.fd.dev != import.fd.dev || backing.fd.ino != import.fd.ino) {
        return RetainedExportMatch::FdMismatch;
    }

    const ExportResource &resource = backing.resource;
    if (resource.va_fourcc != va_fourcc) {
        return RetainedExportMatch::FourccMismatch;
    }
    if (resource.format != format) {
        return RetainedExportMatch::FormatMismatch;
    }
    if (resource.extent.width < coded_extent.width ||
        resource.extent.height < coded_extent.height) {
        return RetainedExportMatch::ExtentMismatch;
    }
    return RetainedExportMatch::Match;
}

const char *retained_export_match_reason(RetainedExportMatch match) {
    switch (match) {
    case RetainedExportMatch::Match:
        return "match";
    case RetainedExportMatch::MissingImport:
        return "missing-import";
    case RetainedExportMatch::MissingFd:
        return "missing-fd";
    case RetainedExportMatch::FdMismatch:
        return "fd-mismatch";
    case RetainedExportMatch::FourccMismatch:
        return "fourcc-mismatch";
    case RetainedExportMatch::FormatMismatch:
        return "format-mismatch";
    case RetainedExportMatch::ExtentMismatch:
        return "extent-mismatch";
    }
    return "unknown";
}

} // namespace vkvv
