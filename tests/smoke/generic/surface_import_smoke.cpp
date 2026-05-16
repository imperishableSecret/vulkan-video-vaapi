#include "va/surface_import.h"

#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

namespace {

    constexpr unsigned int fourcc_nv12 = 0x3231564e;
    constexpr unsigned int fourcc_p010 = 0x30313050;

    bool                   check(bool condition, const char* message) {
        if (!condition) {
            std::fprintf(stderr, "%s\n", message);
        }
        return condition;
    }

    void set_integer_attrib(VASurfaceAttrib* attrib, VASurfaceAttribType type, int value) {
        *attrib               = {};
        attrib->type          = type;
        attrib->value.type    = VAGenericValueTypeInteger;
        attrib->value.value.i = value;
    }

    void set_pointer_attrib(VASurfaceAttrib* attrib, VASurfaceAttribType type, void* value) {
        *attrib               = {};
        attrib->type          = type;
        attrib->value.type    = VAGenericValueTypePointer;
        attrib->value.value.p = value;
    }

    bool fd_identity_matches(const VkvvFdIdentity& identity, int fd) {
        struct stat st{};
        if (fstat(fd, &st) != 0) {
            return false;
        }
        return identity.valid && identity.dev == static_cast<uint64_t>(st.st_dev) && identity.ino == static_cast<uint64_t>(st.st_ino);
    }

    bool check_default_import() {
        const VkvvExternalSurfaceImport import = vkvv_surface_import_from_attribs(nullptr, 0, 0);
        bool                            ok     = true;
        ok &= check(import.memory_type == VA_SURFACE_ATTRIB_MEM_TYPE_VA, "default import should use VA-owned memory");
        ok &= check(!import.external, "default import should not be external");
        ok &= check(!import.fd.valid, "default import should not have fd identity");
        ok &= check(!vkvv_surface_import_has_external_descriptor(nullptr, 0), "default import should not report an external descriptor");
        return ok;
    }

    bool check_external_buffers_import(int fd) {
        uintptr_t                      buffers[1] = {static_cast<uintptr_t>(fd)};
        VASurfaceAttribExternalBuffers descriptor{};
        descriptor.pixel_format = fourcc_nv12;
        descriptor.width        = 1280;
        descriptor.height       = 720;
        descriptor.data_size    = 1280 * 720 * 3 / 2;
        descriptor.num_planes   = 2;
        descriptor.pitches[0]   = 1280;
        descriptor.pitches[1]   = 1280;
        descriptor.offsets[1]   = 1280 * 720;
        descriptor.num_buffers  = 1;
        descriptor.buffers      = buffers;

        VASurfaceAttrib attribs[2]{};
        set_integer_attrib(&attribs[0], VASurfaceAttribMemoryType, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME);
        set_pointer_attrib(&attribs[1], VASurfaceAttribExternalBufferDescriptor, &descriptor);

        const VkvvExternalSurfaceImport import = vkvv_surface_import_from_attribs(attribs, 2, 0);
        bool                            ok     = true;
        ok &= check(vkvv_surface_import_memory_type(attribs, 2) == VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME, "external buffer import memory type mismatch");
        ok &= check(vkvv_surface_import_has_external_descriptor(attribs, 2), "external buffer descriptor not detected");
        ok &= check(import.external, "external buffer import should be marked external");
        ok &= check(import.fourcc == fourcc_nv12, "external buffer import fourcc mismatch");
        ok &= check(import.width == 1280 && import.height == 720, "external buffer import size mismatch");
        ok &= check(fd_identity_matches(import.fd, fd), "external buffer fd identity mismatch");
        return ok;
    }

    bool check_drm_prime2_import_index(int fd) {
        VADRMPRIMESurfaceDescriptor descriptors[2]{};
        descriptors[1].fourcc        = fourcc_p010;
        descriptors[1].width         = 3840;
        descriptors[1].height        = 2160;
        descriptors[1].num_objects   = 1;
        descriptors[1].objects[0].fd = fd;

        VASurfaceAttrib attribs[2]{};
        set_integer_attrib(&attribs[0], VASurfaceAttribMemoryType, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2);
        set_pointer_attrib(&attribs[1], VASurfaceAttribExternalBufferDescriptor, descriptors);

        const VkvvExternalSurfaceImport import = vkvv_surface_import_from_attribs(attribs, 2, 1);
        bool                            ok     = true;
        ok &= check(import.external, "DRM_PRIME_2 import should be marked external");
        ok &= check(import.memory_type == VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, "DRM_PRIME_2 memory type mismatch");
        ok &= check(import.fourcc == fourcc_p010, "DRM_PRIME_2 fourcc mismatch");
        ok &= check(import.width == 3840 && import.height == 2160, "DRM_PRIME_2 indexed descriptor size mismatch");
        ok &= check(fd_identity_matches(import.fd, fd), "DRM_PRIME_2 fd identity mismatch");
        return ok;
    }

} // namespace

int main() {
    int fds[2] = {-1, -1};
    if (pipe(fds) != 0) {
        std::perror("pipe");
        return 1;
    }

    bool ok = true;
    ok &= check_default_import();
    ok &= check_external_buffers_import(fds[0]);
    ok &= check_drm_prime2_import_index(fds[0]);
    ok &= check(vkvv_fd_identity_from_fd(-1).valid == false, "negative fd should not produce identity");

    close(fds[0]);
    close(fds[1]);
    return ok ? 0 : 1;
}
