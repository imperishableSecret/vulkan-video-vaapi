#ifndef VKVV_VA_PRIVATE_H
#define VKVV_VA_PRIVATE_H

#include "va/driver.h"
#include "caps/caps.h"
#include "codecs/ops.h"

#include <cstddef>
#include <stdint.h>
#include <mutex>

// Lock order for driver-owned VA objects: driver state/object registry,
// context, surface, then Vulkan runtime command resources.
class VkvvLockGuard {
  public:
    explicit VkvvLockGuard(std::mutex* mutex) : mutex_(mutex) {
        if (mutex_ != NULL) {
            mutex_->lock();
        }
    }

    ~VkvvLockGuard() {
        unlock();
    }

    VkvvLockGuard(const VkvvLockGuard&)            = delete;
    VkvvLockGuard& operator=(const VkvvLockGuard&) = delete;

    void           unlock() {
        if (mutex_ != NULL) {
            mutex_->unlock();
            mutex_ = NULL;
        }
    }

  private:
    std::mutex* mutex_;
};

inline VkvvDriver* vkvv_driver_from_ctx(VADriverContextP ctx) {
    return ctx != NULL ? static_cast<VkvvDriver*>(ctx->pDriverData) : NULL;
}

inline bool vkvv_profile_is_h264(VAProfile profile) {
    return profile == VAProfileH264Main || profile == VAProfileH264High || profile == VAProfileH264ConstrainedBaseline;
}

inline bool vkvv_profile_is_vp9(VAProfile profile) {
    return profile == VAProfileVP9Profile0 || profile == VAProfileVP9Profile2;
}

inline bool vkvv_profile_is_hevc(VAProfile profile) {
    return profile == VAProfileHEVCMain || profile == VAProfileHEVCMain10 || profile == VAProfileHEVCMain12;
}

void     vkvv_log(const char* fmt, ...);
void*    vkvv_get_or_create_vulkan_runtime(char* reason, size_t reason_size);
void     vkvv_release_context_payload(VkvvDriver* drv, VkvvContext* vctx);
void     vkvv_surface_begin_work(VkvvSurface* surface);
void     vkvv_surface_complete_work(VkvvSurface* surface, VAStatus status);
bool     vkvv_surface_has_pending_work(const VkvvSurface* surface);
void     vkvv_driver_note_decode_domain_locked(VkvvDriver* drv, const VkvvContext* vctx, const VkvvSurface* surface);
void     vkvv_driver_note_decode_domain(VkvvDriver* drv, const VkvvContext* vctx, const VkvvSurface* surface);
bool     vkvv_driver_apply_active_decode_domain(VkvvDriver* drv, VkvvSurface* surface);
bool     vkvv_driver_apply_active_decode_domain_locked(VkvvDriver* drv, VkvvSurface* surface);
void     vkvv_release_buffer_payload(VkvvBuffer* buffer);
void     vkvv_coded_buffer_mark_pending(VkvvBuffer* buffer, uint64_t generation);
void     vkvv_coded_buffer_fail(VkvvBuffer* buffer, VAStatus status, uint64_t generation);
VAStatus vkvv_coded_buffer_store(VkvvBuffer* buffer, const void* data, size_t data_size, uint32_t status_flags, uint64_t generation);

VAStatus vkvvQueryConfigProfiles(VADriverContextP ctx, VAProfile* profile_list, int* num_profiles);
VAStatus vkvvQueryConfigEntrypoints(VADriverContextP ctx, VAProfile profile, VAEntrypoint* entrypoint_list, int* num_entrypoints);
VAStatus vkvvGetConfigAttributes(VADriverContextP ctx, VAProfile profile, VAEntrypoint entrypoint, VAConfigAttrib* attrib_list, int num_attribs);
VAStatus vkvvCreateConfig(VADriverContextP ctx, VAProfile profile, VAEntrypoint entrypoint, VAConfigAttrib* attrib_list, int num_attribs, VAConfigID* config_id);
VAStatus vkvvDestroyConfig(VADriverContextP ctx, VAConfigID config_id);
VAStatus vkvvQueryConfigAttributes(VADriverContextP ctx, VAConfigID config_id, VAProfile* profile, VAEntrypoint* entrypoint, VAConfigAttrib* attrib_list, int* num_attribs);

VAStatus vkvvCreateSurfaces2(VADriverContextP ctx, unsigned int format, unsigned int width, unsigned int height, VASurfaceID* surfaces, unsigned int num_surfaces,
                             VASurfaceAttrib* attrib_list, unsigned int num_attribs);
VAStatus vkvvCreateSurfaces(VADriverContextP ctx, int width, int height, int format, int num_surfaces, VASurfaceID* surfaces);
VAStatus vkvvDestroySurfaces(VADriverContextP ctx, VASurfaceID* surface_list, int num_surfaces);
VAStatus vkvvSyncSurface(VADriverContextP ctx, VASurfaceID render_target);
VAStatus vkvvQuerySurfaceStatus(VADriverContextP ctx, VASurfaceID render_target, VASurfaceStatus* status);
VAStatus vkvvQuerySurfaceError(VADriverContextP ctx, VASurfaceID render_target, VAStatus error_status, void** error_info);
VAStatus vkvvQueryImageFormats(VADriverContextP ctx, VAImageFormat* format_list, int* num_formats);
VAStatus vkvvCreateImage(VADriverContextP ctx, VAImageFormat* format, int width, int height, VAImage* image);
VAStatus vkvvDestroyImage(VADriverContextP ctx, VAImageID image);
VAStatus vkvvPutImage(VADriverContextP ctx, VASurfaceID surface, VAImageID image, int src_x, int src_y, unsigned int src_width, unsigned int src_height, int dest_x, int dest_y,
                      unsigned int dest_width, unsigned int dest_height);
VAStatus vkvvQuerySurfaceAttributes(VADriverContextP ctx, VAConfigID config, VASurfaceAttrib* attrib_list, unsigned int* num_attribs);
VAStatus vkvvExportSurfaceHandle(VADriverContextP ctx, VASurfaceID surface_id, uint32_t mem_type, uint32_t flags, void* descriptor);
VAStatus vkvvSyncSurface2(VADriverContextP ctx, VASurfaceID surface, uint64_t timeout_ns);

VAStatus vkvvCreateBuffer(VADriverContextP ctx, VAContextID context, VABufferType type, unsigned int size, unsigned int num_elements, void* data, VABufferID* buf_id);
VAStatus vkvvBufferSetNumElements(VADriverContextP ctx, VABufferID buf_id, unsigned int num_elements);
VAStatus vkvvMapBuffer(VADriverContextP ctx, VABufferID buf_id, void** pbuf);
VAStatus vkvvUnmapBuffer(VADriverContextP ctx, VABufferID buf_id);
VAStatus vkvvDestroyBuffer(VADriverContextP ctx, VABufferID buffer_id);
VAStatus vkvvBufferInfo(VADriverContextP ctx, VABufferID buf_id, VABufferType* type, unsigned int* size, unsigned int* num_elements);
VAStatus vkvvMapBuffer2(VADriverContextP ctx, VABufferID buf_id, void** pbuf, uint32_t flags);
VAStatus vkvvSyncBuffer(VADriverContextP ctx, VABufferID buf_id, uint64_t timeout_ns);

VAStatus vkvvCreateContext(VADriverContextP ctx, VAConfigID config_id, int picture_width, int picture_height, int flag, VASurfaceID* render_targets, int num_render_targets,
                           VAContextID* context);
VAStatus vkvvDestroyContext(VADriverContextP ctx, VAContextID context);
VAStatus vkvvBeginPicture(VADriverContextP ctx, VAContextID context, VASurfaceID render_target);
VAStatus vkvvRenderPicture(VADriverContextP ctx, VAContextID context, VABufferID* buffers, int num_buffers);
VAStatus vkvvEndPicture(VADriverContextP ctx, VAContextID context);

#endif
