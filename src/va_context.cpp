#include "va_private.h"
#include "h264.h"
#include "vulkan_runtime.h"

#include <new>

void vkvv_release_context_payload(VkvvDriver *drv, VkvvContext *vctx) {
    if (vctx == NULL) {
        return;
    }
    if (vkvv_profile_is_h264(vctx->profile)) {
        vkvv_h264_state_destroy(vctx->codec_state);
        vkvv_vulkan_h264_session_destroy(drv != NULL ? drv->vulkan : NULL, vctx->codec_session);
    }
    vctx->codec_state = NULL;
    vctx->codec_session = NULL;
}

VAStatus vkvvCreateContext(
        VADriverContextP ctx,
        VAConfigID config_id,
        int picture_width,
        int picture_height,
        int flag,
        VASurfaceID *render_targets,
        int num_render_targets,
        VAContextID *context) {
    (void) flag;
    (void) render_targets;
    (void) num_render_targets;

    VkvvDriver *drv = vkvv_driver_from_ctx(ctx);
    auto *config = static_cast<VkvvConfig *>(vkvv_object_get(drv, config_id, VKVV_OBJECT_CONFIG));
    if (config == NULL) {
        return VA_STATUS_ERROR_INVALID_CONFIG;
    }

    auto *vctx = new (std::nothrow) VkvvContext();
    if (vctx == NULL) {
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    vctx->config_id = config_id;
    vctx->profile = config->profile;
    vctx->entrypoint = config->entrypoint;
    vctx->width = (unsigned int) picture_width;
    vctx->height = (unsigned int) picture_height;
    vctx->render_target = VA_INVALID_ID;
    vctx->next_dpb_slot = 0;
    if (vkvv_profile_is_h264(vctx->profile)) {
        vctx->codec_state = vkvv_h264_state_create();
        vctx->codec_session = vkvv_vulkan_h264_session_create();
        if (vctx->codec_state == NULL || vctx->codec_session == NULL) {
            vkvv_release_context_payload(drv, vctx);
            delete vctx;
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
    }

    *context = vkvv_object_add(drv, VKVV_OBJECT_CONTEXT, vctx);
    if (*context == VA_INVALID_ID) {
        vkvv_release_context_payload(drv, vctx);
        delete vctx;
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    return VA_STATUS_SUCCESS;
}

VAStatus vkvvDestroyContext(VADriverContextP ctx, VAContextID context) {
    VkvvDriver *drv = vkvv_driver_from_ctx(ctx);
    auto *vctx = static_cast<VkvvContext *>(vkvv_object_get(drv, context, VKVV_OBJECT_CONTEXT));
    if (vctx == NULL) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    {
        VkvvLockGuard context_lock(&vctx->mutex);
        if (vctx->render_target != VA_INVALID_ID) {
            auto *surface = static_cast<VkvvSurface *>(vkvv_object_get(drv, vctx->render_target, VKVV_OBJECT_SURFACE));
            if (surface != NULL) {
                VkvvLockGuard surface_lock(&surface->mutex);
                if (vkvv_surface_has_pending_work(surface)) {
                    vkvv_surface_complete_work(surface, VA_STATUS_ERROR_OPERATION_FAILED);
                }
            }
        }
        vkvv_release_context_payload(drv, vctx);
    }
    return vkvv_object_remove(drv, context, VKVV_OBJECT_CONTEXT) ?
           VA_STATUS_SUCCESS : VA_STATUS_ERROR_INVALID_CONTEXT;
}

VAStatus vkvvBeginPicture(VADriverContextP ctx, VAContextID context, VASurfaceID render_target) {
    VkvvDriver *drv = vkvv_driver_from_ctx(ctx);
    auto *vctx = static_cast<VkvvContext *>(vkvv_object_get(drv, context, VKVV_OBJECT_CONTEXT));
    if (vctx == NULL) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    VkvvLockGuard context_lock(&vctx->mutex);
    if (vctx->render_target != VA_INVALID_ID) {
        auto *previous = static_cast<VkvvSurface *>(vkvv_object_get(drv, vctx->render_target, VKVV_OBJECT_SURFACE));
        if (previous != NULL) {
            VkvvLockGuard previous_lock(&previous->mutex);
            if (vkvv_surface_has_pending_work(previous)) {
                vkvv_surface_complete_work(previous, VA_STATUS_ERROR_OPERATION_FAILED);
            }
        }
    }
    auto *surface = static_cast<VkvvSurface *>(vkvv_object_get(drv, render_target, VKVV_OBJECT_SURFACE));
    if (surface == NULL) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    VkvvLockGuard surface_lock(&surface->mutex);
    if (surface->destroying) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    if (vkvv_surface_has_pending_work(surface)) {
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    vctx->render_target = render_target;
    vkvv_surface_begin_work(surface);
    if (vkvv_profile_is_h264(vctx->profile)) {
        vkvv_h264_begin_picture(vctx->codec_state);
    }
    return VA_STATUS_SUCCESS;
}

VAStatus vkvvRenderPicture(VADriverContextP ctx, VAContextID context, VABufferID *buffers, int num_buffers) {
    VkvvDriver *drv = vkvv_driver_from_ctx(ctx);
    auto *vctx = static_cast<VkvvContext *>(vkvv_object_get(drv, context, VKVV_OBJECT_CONTEXT));
    if (vctx == NULL) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    VkvvLockGuard context_lock(&vctx->mutex);
    for (int i = 0; i < num_buffers; i++) {
        auto *buffer = static_cast<VkvvBuffer *>(vkvv_object_get(drv, buffers[i], VKVV_OBJECT_BUFFER));
        if (buffer == NULL) {
            return VA_STATUS_ERROR_INVALID_BUFFER;
        }
        if (vkvv_profile_is_h264(vctx->profile)) {
            VAStatus status = vkvv_h264_render_buffer(vctx->codec_state, buffer);
            if (status != VA_STATUS_SUCCESS) {
                return status;
            }
        }
    }
    return VA_STATUS_SUCCESS;
}

VAStatus vkvvEndPicture(VADriverContextP ctx, VAContextID context) {
    VkvvDriver *drv = vkvv_driver_from_ctx(ctx);
    auto *vctx = static_cast<VkvvContext *>(vkvv_object_get(drv, context, VKVV_OBJECT_CONTEXT));
    if (vctx == NULL) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    VkvvLockGuard context_lock(&vctx->mutex);
    if (!vkvv_profile_is_h264(vctx->profile)) {
        return VA_STATUS_ERROR_UNIMPLEMENTED;
    }
    auto *target = static_cast<VkvvSurface *>(vkvv_object_get(drv, vctx->render_target, VKVV_OBJECT_SURFACE));
    if (target == NULL) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    VkvvLockGuard surface_lock(&target->mutex);
    if (target->destroying) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    auto finish_surface = [vctx, target](VAStatus status) {
        vkvv_surface_complete_work(target, status);
        vctx->render_target = VA_INVALID_ID;
        return status;
    };

    unsigned int width = 0;
    unsigned int height = 0;
    char reason[512] = {};
    VAStatus status = vkvv_h264_prepare_decode(vctx->codec_state, &width, &height, reason, sizeof(reason));
    vkvv_log("%s", reason);
    if (status != VA_STATUS_SUCCESS) {
        return finish_surface(status);
    }

    VkvvH264DecodeInput decode_input = {};
    status = vkvv_h264_get_decode_input(vctx->codec_state, &decode_input);
    if (status != VA_STATUS_SUCCESS) {
        return finish_surface(status);
    }

    if (drv->vulkan == NULL) {
        drv->vulkan = vkvv_get_or_create_vulkan_runtime(reason, sizeof(reason));
        vkvv_log("%s", reason);
        if (drv->vulkan == NULL) {
            return finish_surface(VA_STATUS_ERROR_OPERATION_FAILED);
        }
    }

    status = vkvv_vulkan_ensure_h264_session(drv->vulkan, vctx->codec_session, width, height, reason, sizeof(reason));
    vkvv_log("%s", reason);
    if (status != VA_STATUS_SUCCESS) {
        return finish_surface(status);
    }

    status = vkvv_vulkan_decode_h264(
        drv->vulkan, vctx->codec_session, drv, vctx, target,
        vctx->profile, &decode_input, reason, sizeof(reason));
    vkvv_log("%s", reason);
    if (status != VA_STATUS_SUCCESS) {
        return finish_surface(status);
    }

    status = vkvv_vulkan_refresh_surface_export(drv->vulkan, target, reason, sizeof(reason));
    if (reason[0] != '\0') {
        vkvv_log("%s", reason);
    }
    return finish_surface(status);
}
