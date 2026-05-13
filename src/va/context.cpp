#include "va/private.h"
#include "telemetry.h"
#include "vulkan/runtime.h"

#include <new>
#include <vulkan/vulkan.h>

namespace {

    unsigned int codec_operation_for_decode_profile(VAProfile profile) {
        if (vkvv_profile_is_h264(profile)) {
            return VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
        }
        if (vkvv_profile_is_hevc(profile)) {
            return VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR;
        }
        if (vkvv_profile_is_vp9(profile)) {
            return VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR;
        }
        if (profile == VAProfileAV1Profile0) {
            return VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR;
        }
        return 0;
    }

    void tag_surface_for_context(VkvvDriver* drv, VkvvContext* vctx, VASurfaceID surface_id) {
        if (drv == NULL || vctx == NULL || surface_id == VA_INVALID_ID) {
            return;
        }
        auto* surface = vkvv_surface_get_locked(drv, surface_id);
        if (surface == NULL) {
            return;
        }
        surface->stream_id       = vctx->stream_id;
        surface->codec_operation = vctx->codec_operation;
        vkvv_surface_unlock(surface);
    }

} // namespace

void vkvv_release_context_payload(VkvvDriver* drv, VkvvContext* vctx) {
    if (vctx == NULL) {
        return;
    }
    if (drv != NULL && drv->vulkan != NULL) {
        char     reason[512] = {};
        VAStatus status      = vkvv_vulkan_drain_pending_work(drv->vulkan, reason, sizeof(reason));
        if (reason[0] != '\0') {
            vkvv_log("%s", reason);
        }
        if (status != VA_STATUS_SUCCESS) {
            vkvv_log("pending Vulkan work drain failed before context release: %s", vaErrorStr(status));
        }
    }
    if (vctx->decode_ops != NULL) {
        vctx->decode_ops->state_destroy(vctx->decode_state);
        vctx->decode_ops->session_destroy(drv != NULL ? drv->vulkan : NULL, vctx->decode_session);
    }
    vctx->decode_ops     = NULL;
    vctx->decode_state   = NULL;
    vctx->decode_session = NULL;
    vctx->encode_ops     = NULL;
    vctx->encode_state   = NULL;
    vctx->encode_session = NULL;
}

VAStatus vkvvCreateContext(VADriverContextP ctx, VAConfigID config_id, int picture_width, int picture_height, int flag, VASurfaceID* render_targets, int num_render_targets,
                           VAContextID* context) {
    (void)flag;

    VkvvDriver* drv    = vkvv_driver_from_ctx(ctx);
    auto*       config = static_cast<VkvvConfig*>(vkvv_object_get(drv, config_id, VKVV_OBJECT_CONFIG));
    if (config == NULL) {
        return VA_STATUS_ERROR_INVALID_CONFIG;
    }

    auto* vctx = new (std::nothrow) VkvvContext();
    if (vctx == NULL) {
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    vctx->config_id       = config_id;
    vctx->profile         = config->profile;
    vctx->entrypoint      = config->entrypoint;
    vctx->mode            = config->mode;
    vctx->stream_id       = 0;
    vctx->codec_operation = 0;
    vctx->width           = (unsigned int)picture_width;
    vctx->height          = (unsigned int)picture_height;
    vctx->render_target   = VA_INVALID_ID;
    if (vctx->mode == VKVV_CONTEXT_MODE_DECODE) {
        vctx->decode_ops = vkvv_decode_ops_for_profile_entrypoint(vctx->profile, vctx->entrypoint);
        if (vctx->decode_ops == NULL) {
            delete vctx;
            return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
        }
        vctx->codec_operation = codec_operation_for_decode_profile(vctx->profile);
        {
            VkvvLockGuard state_lock(&drv->state_mutex);
            vctx->stream_id = drv->next_stream_id++;
            if (vctx->stream_id == 0) {
                vctx->stream_id = drv->next_stream_id++;
            }
        }
        vctx->decode_state   = vctx->decode_ops->state_create();
        vctx->decode_session = vctx->decode_ops->session_create(config);
        if (vctx->decode_state == NULL || vctx->decode_session == NULL) {
            vkvv_release_context_payload(drv, vctx);
            delete vctx;
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
    } else {
        vctx->encode_ops = vkvv_encode_ops_for_profile_entrypoint(vctx->profile, vctx->entrypoint);
        if (vctx->encode_ops == NULL) {
            delete vctx;
            return VA_STATUS_ERROR_UNIMPLEMENTED;
        }
    }

    *context = vkvv_object_add(drv, VKVV_OBJECT_CONTEXT, vctx);
    if (*context == VA_INVALID_ID) {
        vkvv_release_context_payload(drv, vctx);
        delete vctx;
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    vkvv_trace("va-context-create", "driver=%llu ctx=%u mode=%u profile=%d entrypoint=%d stream=%llu codec=0x%x size=%ux%u targets=%d", (unsigned long long)drv->driver_instance_id,
               *context, vctx->mode, vctx->profile, vctx->entrypoint, (unsigned long long)vctx->stream_id, vctx->codec_operation, vctx->width, vctx->height, num_render_targets);
    if (vctx->mode == VKVV_CONTEXT_MODE_DECODE) {
        for (int i = 0; i < num_render_targets; i++) {
            tag_surface_for_context(drv, vctx, render_targets != NULL ? render_targets[i] : VA_INVALID_ID);
        }
    }
    return VA_STATUS_SUCCESS;
}

VAStatus vkvvDestroyContext(VADriverContextP ctx, VAContextID context) {
    VkvvDriver* drv  = vkvv_driver_from_ctx(ctx);
    auto*       vctx = static_cast<VkvvContext*>(vkvv_object_get(drv, context, VKVV_OBJECT_CONTEXT));
    if (vctx == NULL) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    vkvv_trace("va-context-destroy", "driver=%llu ctx=%u stream=%llu codec=0x%x target=%u", (unsigned long long)drv->driver_instance_id, context,
               (unsigned long long)vctx->stream_id, vctx->codec_operation, vctx->render_target);
    {
        VkvvLockGuard state_lock(&drv->state_mutex);
        if (drv->active_decode_stream_id == vctx->stream_id && drv->active_decode_codec_operation == vctx->codec_operation) {
            drv->active_decode_stream_id       = 0;
            drv->active_decode_codec_operation = 0;
            drv->active_decode_width           = 0;
            drv->active_decode_height          = 0;
            drv->active_decode_rt_format       = 0;
            drv->active_decode_fourcc          = 0;
        }
    }
    {
        VkvvLockGuard context_lock(&vctx->mutex);
        if (vctx->render_target != VA_INVALID_ID) {
            auto* surface = static_cast<VkvvSurface*>(vkvv_object_get(drv, vctx->render_target, VKVV_OBJECT_SURFACE));
            if (surface != NULL) {
                VkvvLockGuard surface_lock(&surface->mutex);
                if (vkvv_surface_has_pending_work(surface)) {
                    vkvv_surface_complete_work(surface, VA_STATUS_ERROR_OPERATION_FAILED);
                }
            }
        }
        vkvv_release_context_payload(drv, vctx);
    }
    const bool removed = vkvv_object_remove(drv, context, VKVV_OBJECT_CONTEXT);
    return removed ? VA_STATUS_SUCCESS : VA_STATUS_ERROR_INVALID_CONTEXT;
}

VAStatus vkvvBeginPicture(VADriverContextP ctx, VAContextID context, VASurfaceID render_target) {
    VkvvDriver* drv  = vkvv_driver_from_ctx(ctx);
    auto*       vctx = static_cast<VkvvContext*>(vkvv_object_get(drv, context, VKVV_OBJECT_CONTEXT));
    if (vctx == NULL) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    {
        VkvvLockGuard state_lock(&drv->state_mutex);
        auto*         domain_surface = vkvv_surface_get_locked(drv, render_target);
        if (domain_surface != NULL) {
            const bool surface_codec_transition =
                domain_surface->stream_id != 0 && (domain_surface->stream_id != vctx->stream_id || domain_surface->codec_operation != vctx->codec_operation);
            vkvv_trace("va-begin-domain", "driver=%llu ctx=%u profile=%d target=%u ctx_stream=%llu ctx_codec=0x%x surface_stream=%llu surface_codec=0x%x decoded=%u pending=%u",
                       (unsigned long long)drv->driver_instance_id, context, vctx->profile, render_target, (unsigned long long)vctx->stream_id, vctx->codec_operation,
                       (unsigned long long)domain_surface->stream_id, domain_surface->codec_operation, domain_surface->decoded ? 1U : 0U,
                       vkvv_surface_has_pending_work(domain_surface) ? 1U : 0U);
            if (surface_codec_transition) {
                vkvv_trace("va-begin-surface-retag",
                           "driver=%llu ctx=%u target=%u old_stream=%llu old_codec=0x%x new_stream=%llu new_codec=0x%x decoded=%u pending=%u predecode_export=%u",
                           (unsigned long long)drv->driver_instance_id, context, render_target, (unsigned long long)domain_surface->stream_id, domain_surface->codec_operation,
                           (unsigned long long)vctx->stream_id, vctx->codec_operation, domain_surface->decoded ? 1U : 0U, vkvv_surface_has_pending_work(domain_surface) ? 1U : 0U,
                           vkvv_vulkan_surface_has_predecode_export(domain_surface) ? 1U : 0U);
            }
            if (!domain_surface->destroying) {
                vkvv_driver_note_decode_domain_locked(drv, vctx, domain_surface);
            }
            vkvv_surface_unlock(domain_surface);
        }
    }
    VkvvLockGuard context_lock(&vctx->mutex);
    if (vctx->mode == VKVV_CONTEXT_MODE_ENCODE) {
        return VA_STATUS_ERROR_UNIMPLEMENTED;
    }
    if (vctx->render_target != VA_INVALID_ID) {
        auto* previous = static_cast<VkvvSurface*>(vkvv_object_get(drv, vctx->render_target, VKVV_OBJECT_SURFACE));
        if (previous != NULL) {
            VkvvLockGuard previous_lock(&previous->mutex);
            if (vkvv_surface_has_pending_work(previous)) {
                vkvv_surface_complete_work(previous, VA_STATUS_ERROR_OPERATION_FAILED);
            }
        }
    }
    auto* surface = static_cast<VkvvSurface*>(vkvv_object_get(drv, render_target, VKVV_OBJECT_SURFACE));
    if (surface == NULL) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    VkvvLockGuard surface_lock(&surface->mutex);
    if (surface->destroying) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    if (vkvv_surface_has_pending_work(surface)) {
        vkvv_trace("va-begin-target-drain", "driver=%llu ctx=%u target=%u stream=%llu codec=0x%x", (unsigned long long)drv->driver_instance_id, context, surface->id,
                   (unsigned long long)surface->stream_id, surface->codec_operation);
        if (drv->vulkan == NULL) {
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        char     reason[512] = {};
        VAStatus status      = vkvv_vulkan_complete_surface_work(drv->vulkan, surface, VA_TIMEOUT_INFINITE, reason, sizeof(reason));
        if (reason[0] != '\0') {
            vkvv_log("%s", reason);
        }
        if (status != VA_STATUS_SUCCESS) {
            return status;
        }
        if (vkvv_surface_has_pending_work(surface)) {
            return VA_STATUS_ERROR_TIMEDOUT;
        }
    }
    surface->stream_id       = vctx->stream_id;
    surface->codec_operation = vctx->codec_operation;
    vctx->render_target      = render_target;
    vkvv_surface_begin_work(surface);
    vkvv_trace("va-begin-bound", "driver=%llu ctx=%u target=%u stream=%llu codec=0x%x decoded=%u predecode_export=%u", (unsigned long long)drv->driver_instance_id, context,
               render_target, (unsigned long long)surface->stream_id, surface->codec_operation, surface->decoded ? 1U : 0U,
               vkvv_vulkan_surface_has_predecode_export(surface) ? 1U : 0U);
    if (vctx->decode_ops != NULL) {
        vctx->decode_ops->begin_picture(vctx->decode_state);
    }
    return VA_STATUS_SUCCESS;
}

VAStatus vkvvRenderPicture(VADriverContextP ctx, VAContextID context, VABufferID* buffers, int num_buffers) {
    VkvvDriver* drv  = vkvv_driver_from_ctx(ctx);
    auto*       vctx = static_cast<VkvvContext*>(vkvv_object_get(drv, context, VKVV_OBJECT_CONTEXT));
    if (vctx == NULL) {
        vkvv_trace("va-render-context-missing", "driver=%llu ctx=%u buffers=%d", (unsigned long long)drv->driver_instance_id, context, num_buffers);
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    VkvvLockGuard context_lock(&vctx->mutex);
    for (int i = 0; i < num_buffers; i++) {
        auto* buffer = static_cast<VkvvBuffer*>(vkvv_object_get(drv, buffers[i], VKVV_OBJECT_BUFFER));
        if (buffer == NULL) {
            vkvv_trace("va-render-buffer-missing", "driver=%llu ctx=%u stream=%llu codec=0x%x index=%d buffer=%u target=%u", (unsigned long long)drv->driver_instance_id, context,
                       (unsigned long long)vctx->stream_id, vctx->codec_operation, i, buffers[i], vctx->render_target);
            return VA_STATUS_ERROR_INVALID_BUFFER;
        }
        vkvv_trace("va-render-buffer", "driver=%llu ctx=%u stream=%llu codec=0x%x index=%d buffer=%u type=%u size=%u elements=%u mapped=%u target=%u",
                   (unsigned long long)drv->driver_instance_id, context, (unsigned long long)vctx->stream_id, vctx->codec_operation, i, buffers[i], buffer->type, buffer->size,
                   buffer->num_elements, buffer->mapped ? 1U : 0U, vctx->render_target);
        if (vctx->mode == VKVV_CONTEXT_MODE_ENCODE) {
            vkvv_trace("va-render-buffer-status", "driver=%llu ctx=%u stream=%llu codec=0x%x index=%d buffer=%u type=%u status=%d", (unsigned long long)drv->driver_instance_id,
                       context, (unsigned long long)vctx->stream_id, vctx->codec_operation, i, buffers[i], buffer->type, VA_STATUS_ERROR_UNIMPLEMENTED);
            return VA_STATUS_ERROR_UNIMPLEMENTED;
        }
        if (vctx->decode_ops != NULL) {
            VAStatus status = vctx->decode_ops->render_buffer(vctx->decode_state, buffer);
            if (status != VA_STATUS_SUCCESS) {
                vkvv_trace("va-render-buffer-status", "driver=%llu ctx=%u stream=%llu codec=0x%x index=%d buffer=%u type=%u size=%u elements=%u status=%d target=%u",
                           (unsigned long long)drv->driver_instance_id, context, (unsigned long long)vctx->stream_id, vctx->codec_operation, i, buffers[i], buffer->type,
                           buffer->size, buffer->num_elements, status, vctx->render_target);
                return status;
            }
        }
    }
    return VA_STATUS_SUCCESS;
}

namespace {

    VAStatus end_encode_picture(VkvvDriver* drv, VkvvContext* vctx) {
        (void)drv;
        (void)vctx;
        return VA_STATUS_ERROR_UNIMPLEMENTED;
    }

    VAStatus end_decode_picture(VkvvDriver* drv, VkvvContext* vctx) {
        if (vctx->decode_ops == NULL) {
            return VA_STATUS_ERROR_UNIMPLEMENTED;
        }
        auto* target = static_cast<VkvvSurface*>(vkvv_object_get(drv, vctx->render_target, VKVV_OBJECT_SURFACE));
        if (target == NULL) {
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }
        VkvvLockGuard surface_lock(&target->mutex);
        if (target->destroying) {
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }
        vkvv_trace("va-end-enter", "driver=%llu ctx_stream=%llu ctx_codec=0x%x target=%u surface_stream=%llu surface_codec=0x%x decoded=%u pending=%u predecode_export=%u",
                   (unsigned long long)drv->driver_instance_id, (unsigned long long)vctx->stream_id, vctx->codec_operation, vctx->render_target,
                   (unsigned long long)target->stream_id, target->codec_operation, target->decoded ? 1U : 0U, vkvv_surface_has_pending_work(target) ? 1U : 0U,
                   vkvv_vulkan_surface_has_predecode_export(target) ? 1U : 0U);
        auto finish_surface = [drv, vctx, target](VAStatus status) {
            vkvv_surface_complete_work(target, status);
            vctx->render_target = VA_INVALID_ID;
            vkvv_trace("va-end-finish", "driver=%llu target=%u status=%d decoded=%u pending=%u", (unsigned long long)drv->driver_instance_id, target->id, status,
                       target->decoded ? 1U : 0U, vkvv_surface_has_pending_work(target) ? 1U : 0U);
            return status;
        };

        unsigned int width       = 0;
        unsigned int height      = 0;
        char         reason[512] = {};
        VAStatus     status      = vctx->decode_ops->prepare_decode(vctx->decode_state, &width, &height, reason, sizeof(reason));
        vkvv_log("%s", reason);
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

        status = vctx->decode_ops->configure_session(drv->vulkan, vctx->decode_session, target, vctx->decode_state, reason, sizeof(reason));
        if (reason[0] != '\0') {
            vkvv_log("%s", reason);
        }
        if (status != VA_STATUS_SUCCESS) {
            return finish_surface(status);
        }

        status = vctx->decode_ops->ensure_session(drv->vulkan, vctx->decode_session, width, height, reason, sizeof(reason));
        vkvv_log("%s", reason);
        if (status != VA_STATUS_SUCCESS) {
            return finish_surface(status);
        }

        status = vctx->decode_ops->decode(drv->vulkan, vctx->decode_session, drv, vctx, target, vctx->profile, vctx->decode_state, reason, sizeof(reason));
        vkvv_log("%s", reason);
        if (status != VA_STATUS_SUCCESS) {
            return finish_surface(status);
        }

        const bool pending_export_refresh = vkvv_vulkan_surface_has_pending_export_refresh_work(drv->vulkan, target);
        const bool externally_visible     = vkvv_vulkan_surface_has_exported_backing(target) || target->import.external;
        if (vkvv_surface_has_pending_work(target) && pending_export_refresh && externally_visible) {
            vkvv_trace("va-end-visible-drain", "driver=%llu target=%u stream=%llu codec=0x%x exported=%u import_external=%u", (unsigned long long)drv->driver_instance_id,
                       target->id, (unsigned long long)target->stream_id, target->codec_operation, vkvv_vulkan_surface_has_exported_backing(target) ? 1U : 0U,
                       target->import.external ? 1U : 0U);
            status = vkvv_vulkan_complete_surface_work(drv->vulkan, target, VA_TIMEOUT_INFINITE, reason, sizeof(reason));
            if (reason[0] != '\0') {
                vkvv_log("%s", reason);
            }
            if (status != VA_STATUS_SUCCESS) {
                return finish_surface(status);
            }
        }

        vkvv_trace("va-end-submitted", "driver=%llu target=%u stream=%llu codec=0x%x pending=%u predecode_export=%u", (unsigned long long)drv->driver_instance_id, target->id,
                   (unsigned long long)target->stream_id, target->codec_operation, vkvv_surface_has_pending_work(target) ? 1U : 0U,
                   vkvv_vulkan_surface_has_predecode_export(target) ? 1U : 0U);
        vctx->render_target = VA_INVALID_ID;
        return VA_STATUS_SUCCESS;
    }

} // namespace

VAStatus vkvvEndPicture(VADriverContextP ctx, VAContextID context) {
    VkvvDriver* drv  = vkvv_driver_from_ctx(ctx);
    auto*       vctx = static_cast<VkvvContext*>(vkvv_object_get(drv, context, VKVV_OBJECT_CONTEXT));
    if (vctx == NULL) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    VkvvLockGuard context_lock(&vctx->mutex);
    if (vctx->mode == VKVV_CONTEXT_MODE_ENCODE) {
        return end_encode_picture(drv, vctx);
    }
    return end_decode_picture(drv, vctx);
}
