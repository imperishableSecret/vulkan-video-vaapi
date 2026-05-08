#ifndef VKVV_VA_PRIVATE_H
#define VKVV_VA_PRIVATE_H

#include "driver.h"

#include <cstddef>
#include <stdint.h>
#include <mutex>

// Lock order for driver-owned VA objects: driver state/object registry,
// context, surface, then Vulkan runtime command resources.
class VkvvLockGuard {
  public:
    explicit VkvvLockGuard(std::mutex *mutex) : mutex_(mutex) {
        if (mutex_ != NULL) {
            mutex_->lock();
        }
    }

    ~VkvvLockGuard() {
        unlock();
    }

    VkvvLockGuard(const VkvvLockGuard &) = delete;
    VkvvLockGuard &operator=(const VkvvLockGuard &) = delete;

    void unlock() {
        if (mutex_ != NULL) {
            mutex_->unlock();
            mutex_ = NULL;
        }
    }

  private:
    std::mutex *mutex_;
};

inline VkvvDriver *vkvv_driver_from_ctx(VADriverContextP ctx) {
    return ctx != NULL ? static_cast<VkvvDriver *>(ctx->pDriverData) : NULL;
}

inline bool vkvv_profile_is_h264(VAProfile profile) {
    return profile == VAProfileH264Main ||
           profile == VAProfileH264High ||
           profile == VAProfileH264ConstrainedBaseline;
}

void vkvv_log(const char *fmt, ...);
void *vkvv_get_or_create_vulkan_runtime(char *reason, size_t reason_size);
void vkvv_release_context_payload(VkvvDriver *drv, VkvvContext *vctx);
const VkvvDecodeOps *vkvv_decode_ops_for_profile_entrypoint(VAProfile profile, VAEntrypoint entrypoint);
const VkvvEncodeOps *vkvv_encode_ops_for_profile_entrypoint(VAProfile profile, VAEntrypoint entrypoint);
void vkvv_init_profile_capabilities(VkvvDriver *drv);
const VkvvProfileCapability *vkvv_profile_capability(const VkvvDriver *drv, VAProfile profile);
const VkvvProfileCapability *vkvv_profile_capability_for_entrypoint(const VkvvDriver *drv, VAProfile profile, VAEntrypoint entrypoint);
const VkvvProfileCapability *vkvv_profile_capability_record(const VkvvDriver *drv, VAProfile profile, VAEntrypoint entrypoint, VkvvCodecDirection direction);
bool vkvv_profile_supported(const VkvvDriver *drv, VAProfile profile);
unsigned int vkvv_surface_fourcc_for_format(unsigned int rt_format);
unsigned int vkvv_rt_format_bit_depth(unsigned int rt_format);
unsigned int vkvv_select_rt_format(const VkvvProfileCapability *cap, unsigned int requested);
unsigned int vkvv_select_driver_rt_format(const VkvvDriver *drv, unsigned int requested);
unsigned int vkvv_config_attribute_count(void);
void vkvv_fill_config_attribute(const VkvvProfileCapability *cap, VAConfigAttrib *attrib);
bool vkvv_fill_config_attribute_by_index(const VkvvProfileCapability *cap, unsigned int index, VAConfigAttrib *attrib);
unsigned int vkvv_query_image_formats(const VkvvDriver *drv, VAImageFormat *format_list, unsigned int max_formats);
void vkvv_surface_begin_work(VkvvSurface *surface);
void vkvv_surface_complete_work(VkvvSurface *surface, VAStatus status);
bool vkvv_surface_has_pending_work(const VkvvSurface *surface);

VAStatus vkvvQueryConfigProfiles(VADriverContextP ctx, VAProfile *profile_list, int *num_profiles);
VAStatus vkvvQueryConfigEntrypoints(VADriverContextP ctx, VAProfile profile, VAEntrypoint *entrypoint_list, int *num_entrypoints);
VAStatus vkvvGetConfigAttributes(VADriverContextP ctx, VAProfile profile, VAEntrypoint entrypoint, VAConfigAttrib *attrib_list, int num_attribs);
VAStatus vkvvCreateConfig(VADriverContextP ctx, VAProfile profile, VAEntrypoint entrypoint, VAConfigAttrib *attrib_list, int num_attribs, VAConfigID *config_id);
VAStatus vkvvDestroyConfig(VADriverContextP ctx, VAConfigID config_id);
VAStatus vkvvQueryConfigAttributes(VADriverContextP ctx, VAConfigID config_id, VAProfile *profile, VAEntrypoint *entrypoint, VAConfigAttrib *attrib_list, int *num_attribs);

VAStatus vkvvCreateSurfaces2(VADriverContextP ctx, unsigned int format, unsigned int width, unsigned int height, VASurfaceID *surfaces, unsigned int num_surfaces, VASurfaceAttrib *attrib_list, unsigned int num_attribs);
VAStatus vkvvCreateSurfaces(VADriverContextP ctx, int width, int height, int format, int num_surfaces, VASurfaceID *surfaces);
VAStatus vkvvDestroySurfaces(VADriverContextP ctx, VASurfaceID *surface_list, int num_surfaces);
VAStatus vkvvSyncSurface(VADriverContextP ctx, VASurfaceID render_target);
VAStatus vkvvQuerySurfaceStatus(VADriverContextP ctx, VASurfaceID render_target, VASurfaceStatus *status);
VAStatus vkvvQuerySurfaceError(VADriverContextP ctx, VASurfaceID render_target, VAStatus error_status, void **error_info);
VAStatus vkvvQueryImageFormats(VADriverContextP ctx, VAImageFormat *format_list, int *num_formats);
VAStatus vkvvQuerySurfaceAttributes(VADriverContextP ctx, VAConfigID config, VASurfaceAttrib *attrib_list, unsigned int *num_attribs);
VAStatus vkvvExportSurfaceHandle(VADriverContextP ctx, VASurfaceID surface_id, uint32_t mem_type, uint32_t flags, void *descriptor);
VAStatus vkvvSyncSurface2(VADriverContextP ctx, VASurfaceID surface, uint64_t timeout_ns);

VAStatus vkvvCreateBuffer(VADriverContextP ctx, VAContextID context, VABufferType type, unsigned int size, unsigned int num_elements, void *data, VABufferID *buf_id);
VAStatus vkvvBufferSetNumElements(VADriverContextP ctx, VABufferID buf_id, unsigned int num_elements);
VAStatus vkvvMapBuffer(VADriverContextP ctx, VABufferID buf_id, void **pbuf);
VAStatus vkvvUnmapBuffer(VADriverContextP ctx, VABufferID buf_id);
VAStatus vkvvDestroyBuffer(VADriverContextP ctx, VABufferID buffer_id);
VAStatus vkvvBufferInfo(VADriverContextP ctx, VABufferID buf_id, VABufferType *type, unsigned int *size, unsigned int *num_elements);
VAStatus vkvvMapBuffer2(VADriverContextP ctx, VABufferID buf_id, void **pbuf, uint32_t flags);

VAStatus vkvvCreateContext(VADriverContextP ctx, VAConfigID config_id, int picture_width, int picture_height, int flag, VASurfaceID *render_targets, int num_render_targets, VAContextID *context);
VAStatus vkvvDestroyContext(VADriverContextP ctx, VAContextID context);
VAStatus vkvvBeginPicture(VADriverContextP ctx, VAContextID context, VASurfaceID render_target);
VAStatus vkvvRenderPicture(VADriverContextP ctx, VAContextID context, VABufferID *buffers, int num_buffers);
VAStatus vkvvEndPicture(VADriverContextP ctx, VAContextID context);

#endif
