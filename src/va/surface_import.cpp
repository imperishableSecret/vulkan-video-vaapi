#include "va/surface_import.h"

#include <cstdint>
#include <drm/drm_fourcc.h>
#include <sys/stat.h>

namespace {

    const VASurfaceAttrib* find_surface_attrib(const VASurfaceAttrib* attrib_list, unsigned int num_attribs, VASurfaceAttribType type) {
        if (attrib_list == nullptr) {
            return nullptr;
        }
        for (unsigned int i = 0; i < num_attribs; i++) {
            if (attrib_list[i].type == type) {
                return &attrib_list[i];
            }
        }
        return nullptr;
    }

} // namespace

uint32_t vkvv_surface_import_memory_type(const VASurfaceAttrib* attrib_list, unsigned int num_attribs) {
    const VASurfaceAttrib* attrib = find_surface_attrib(attrib_list, num_attribs, VASurfaceAttribMemoryType);
    if (attrib == nullptr || attrib->value.type != VAGenericValueTypeInteger) {
        return VA_SURFACE_ATTRIB_MEM_TYPE_VA;
    }
    return static_cast<uint32_t>(attrib->value.value.i);
}

bool vkvv_surface_import_has_external_descriptor(const VASurfaceAttrib* attrib_list, unsigned int num_attribs) {
    const VASurfaceAttrib* attrib = find_surface_attrib(attrib_list, num_attribs, VASurfaceAttribExternalBufferDescriptor);
    return attrib != nullptr && attrib->value.type == VAGenericValueTypePointer && attrib->value.value.p != nullptr;
}

VkvvFdIdentity vkvv_fd_identity_from_fd(int fd) {
    VkvvFdIdentity out{};
    if (fd < 0) {
        return out;
    }
    struct stat st{};
    if (fstat(fd, &st) != 0) {
        return out;
    }
    out.valid = true;
    out.dev   = static_cast<uint64_t>(st.st_dev);
    out.ino   = static_cast<uint64_t>(st.st_ino);
    return out;
}

bool vkvv_fd_identity_equal(VkvvFdIdentity lhs, VkvvFdIdentity rhs) {
    return lhs.valid && rhs.valid && lhs.dev == rhs.dev && lhs.ino == rhs.ino;
}

VkvvExternalImageIdentity vkvv_external_image_identity_from_import(const VkvvExternalSurfaceImport& import) {
    VkvvExternalImageIdentity identity{};
    identity.fd                      = import.fd;
    identity.fourcc                  = import.fourcc;
    identity.width                   = import.width;
    identity.height                  = import.height;
    identity.has_drm_format_modifier = import.has_drm_format_modifier;
    identity.drm_format_modifier     = import.drm_format_modifier;
    return identity;
}

VkvvExternalSurfaceImport vkvv_surface_import_from_attribs(const VASurfaceAttrib* attrib_list, unsigned int num_attribs, unsigned int index) {
    VkvvExternalSurfaceImport info{};
    info.memory_type                = vkvv_surface_import_memory_type(attrib_list, num_attribs);
    const VASurfaceAttrib* external = find_surface_attrib(attrib_list, num_attribs, VASurfaceAttribExternalBufferDescriptor);
    if (external == nullptr || external->value.type != VAGenericValueTypePointer || external->value.value.p == nullptr) {
        return info;
    }

    info.external = true;
    int fd        = -1;
    if ((info.memory_type & VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_3) != 0) {
        const auto*                         descriptors = static_cast<const VADRMPRIME3SurfaceDescriptor*>(external->value.value.p);
        const VADRMPRIME3SurfaceDescriptor& descriptor  = descriptors[index];
        fd                                              = descriptor.num_objects > 0 ? descriptor.objects[0].fd : -1;
        info.fourcc                                     = descriptor.fourcc;
        info.width                                      = descriptor.width;
        info.height                                     = descriptor.height;
        info.has_drm_format_modifier                    = descriptor.num_objects > 0;
        info.drm_format_modifier                        = descriptor.num_objects > 0 ? descriptor.objects[0].drm_format_modifier : 0;
    } else if ((info.memory_type & VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2) != 0) {
        const auto*                        descriptors = static_cast<const VADRMPRIMESurfaceDescriptor*>(external->value.value.p);
        const VADRMPRIMESurfaceDescriptor& descriptor  = descriptors[index];
        fd                                             = descriptor.num_objects > 0 ? descriptor.objects[0].fd : -1;
        info.fourcc                                    = descriptor.fourcc;
        info.width                                     = descriptor.width;
        info.height                                    = descriptor.height;
        info.has_drm_format_modifier                   = descriptor.num_objects > 0;
        info.drm_format_modifier                       = descriptor.num_objects > 0 ? descriptor.objects[0].drm_format_modifier : 0;
    } else if ((info.memory_type & VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME) != 0) {
        const auto*                           descriptors  = static_cast<const VASurfaceAttribExternalBuffers*>(external->value.value.p);
        const VASurfaceAttribExternalBuffers& descriptor   = descriptors[index];
        const uintptr_t                       first_buffer = descriptor.buffers != nullptr && descriptor.num_buffers > 0 ? descriptor.buffers[0] : 0;
        fd                                                 = first_buffer <= static_cast<uintptr_t>(INT32_MAX) ? static_cast<int>(first_buffer) : -1;
        info.fourcc                                        = descriptor.pixel_format;
        info.width                                         = descriptor.width;
        info.height                                        = descriptor.height;
        if ((descriptor.flags & VA_SURFACE_EXTBUF_DESC_ENABLE_TILING) == 0) {
            info.has_drm_format_modifier = true;
            info.drm_format_modifier     = DRM_FORMAT_MOD_LINEAR;
        }
    }

    info.fd = vkvv_fd_identity_from_fd(fd);
    return info;
}
