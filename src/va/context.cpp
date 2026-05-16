#include "va/private.h"
#include "telemetry.h"
#include "vulkan/runtime.h"

#include <chrono>
#include <new>
#include <vulkan/vulkan.h>

namespace {

    constexpr uint64_t va_call_gap_threshold_us = 50000;

    uint64_t monotonic_us() {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }

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

    struct VaCallSurfaceCounters {
        unsigned int context_total         = 0;
        unsigned int decode_contexts       = 0;
        unsigned int encode_contexts       = 0;
        unsigned int surface_total         = 0;
        unsigned int surface_decoded       = 0;
        unsigned int surface_pending       = 0;
        unsigned int surface_exported      = 0;
        unsigned int surface_predecode     = 0;
        unsigned int surface_imported      = 0;
        unsigned int surface_destroying    = 0;
        unsigned int active_surfaces       = 0;
        unsigned int active_decoded        = 0;
        unsigned int active_pending        = 0;
        unsigned int active_exported       = 0;
        unsigned int active_imported       = 0;
    };

    void collect_va_call_surface_counters(VkvvDriver* drv, uint64_t active_stream, unsigned int active_codec, VaCallSurfaceCounters* counters) {
        if (drv == NULL || counters == NULL) {
            return;
        }

        VkvvLockGuard object_lock(&drv->object_mutex);
        for (VkvvObject* object = drv->objects; object != NULL; object = object->next) {
            if (object->type == VKVV_OBJECT_CONTEXT) {
                auto* vctx = static_cast<VkvvContext*>(object->payload);
                if (vctx == NULL) {
                    continue;
                }
                std::lock_guard<std::mutex> context_lock(vctx->mutex);
                counters->context_total++;
                if (vctx->mode == VKVV_CONTEXT_MODE_DECODE) {
                    counters->decode_contexts++;
                } else if (vctx->mode == VKVV_CONTEXT_MODE_ENCODE) {
                    counters->encode_contexts++;
                }
                continue;
            }
            if (object->type != VKVV_OBJECT_SURFACE) {
                continue;
            }

            auto* surface = static_cast<VkvvSurface*>(object->payload);
            if (surface == NULL) {
                continue;
            }
            std::lock_guard<std::mutex> surface_lock(surface->mutex);
            const bool                  pending          = vkvv_surface_has_pending_work(surface);
            const bool                  exported         = vkvv_vulkan_surface_has_exported_backing(surface);
            const bool                  predecode        = vkvv_vulkan_surface_has_predecode_export(surface);
            const bool                  imported         = surface->import.external;
            const bool                  in_active_domain = active_stream != 0 && surface->stream_id == active_stream && surface->codec_operation == active_codec;

            counters->surface_total++;
            counters->surface_decoded += surface->decoded ? 1U : 0U;
            counters->surface_pending += pending ? 1U : 0U;
            counters->surface_exported += exported ? 1U : 0U;
            counters->surface_predecode += predecode ? 1U : 0U;
            counters->surface_imported += imported ? 1U : 0U;
            counters->surface_destroying += surface->destroying ? 1U : 0U;
            if (in_active_domain) {
                counters->active_surfaces++;
                counters->active_decoded += surface->decoded ? 1U : 0U;
                counters->active_pending += pending ? 1U : 0U;
                counters->active_exported += exported ? 1U : 0U;
                counters->active_imported += imported ? 1U : 0U;
            }
        }
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

void vkvv_trace_va_call_gap(VkvvDriver* drv, const char* call, VAContextID context, VASurfaceID surface) {
    if (drv == NULL || !vkvv_trace_enabled()) {
        return;
    }

    const uint64_t now_us       = monotonic_us();
    const char*    current_call = call != NULL ? call : "unknown";
    const char*    previous_call;
    VAContextID    previous_context;
    VASurfaceID    previous_surface;
    uint64_t       previous_us;
    uint64_t       gap_us;
    uint64_t       active_stream;
    unsigned int   active_codec;
    {
        VkvvLockGuard state_lock(&drv->state_mutex);
        previous_call    = drv->telemetry_last_va_call != NULL ? drv->telemetry_last_va_call : "none";
        previous_context = drv->telemetry_last_va_context;
        previous_surface = drv->telemetry_last_va_surface;
        previous_us      = drv->telemetry_last_va_call_us;
        gap_us           = previous_us != 0 && now_us >= previous_us ? now_us - previous_us : 0;
        active_stream    = drv->active_decode_stream_id;
        active_codec     = drv->active_decode_codec_operation;

        drv->telemetry_last_va_call     = current_call;
        drv->telemetry_last_va_call_us  = now_us;
        drv->telemetry_last_va_context  = context;
        drv->telemetry_last_va_surface  = surface;
    }

    if (previous_us == 0 || gap_us < va_call_gap_threshold_us) {
        return;
    }

    VaCallSurfaceCounters counters{};
    collect_va_call_surface_counters(drv, active_stream, active_codec, &counters);
    const size_t   retained_export_count = drv->vulkan != NULL ? vkvv_vulkan_retained_export_count(drv->vulkan) : 0;
    const uint64_t retained_export_bytes = drv->vulkan != NULL ? vkvv_vulkan_retained_export_memory_bytes(drv->vulkan) : 0;

    vkvv_trace("va-call-gap",
               "driver=%llu previous_call=%s current_call=%s gap_us=%llu previous_monotonic_us=%llu current_monotonic_us=%llu previous_context=%u current_context=%u "
               "previous_surface=%u current_surface=%u active_stream=%llu active_codec=0x%x context_total=%u decode_contexts=%u encode_contexts=%u surface_total=%u "
               "surface_decoded=%u surface_pending=%u surface_exported=%u surface_predecode_export=%u surface_import_external=%u surface_destroying=%u active_surfaces=%u "
               "active_decoded=%u active_pending=%u active_exported=%u active_import_external=%u retained_export_count=%zu retained_export_bytes=%llu",
               (unsigned long long)drv->driver_instance_id, previous_call, current_call, (unsigned long long)gap_us, (unsigned long long)previous_us, (unsigned long long)now_us,
               previous_context, context, previous_surface, surface, (unsigned long long)active_stream, active_codec, counters.context_total, counters.decode_contexts,
               counters.encode_contexts, counters.surface_total, counters.surface_decoded, counters.surface_pending, counters.surface_exported, counters.surface_predecode,
               counters.surface_imported, counters.surface_destroying, counters.active_surfaces, counters.active_decoded, counters.active_pending, counters.active_exported,
               counters.active_imported, retained_export_count, (unsigned long long)retained_export_bytes);
}

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
    vkvv_trace_va_call_gap(drv, "vaCreateContext", VA_INVALID_ID, VA_INVALID_SURFACE);
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
        if (num_render_targets == 0) {
            VkvvLockGuard state_lock(&drv->state_mutex);
            vkvv_driver_note_decode_domain_locked(drv, vctx, NULL);
        }
        for (int i = 0; i < num_render_targets; i++) {
            tag_surface_for_context(drv, vctx, render_targets != NULL ? render_targets[i] : VA_INVALID_ID);
        }
    }
    return VA_STATUS_SUCCESS;
}

VAStatus vkvvDestroyContext(VADriverContextP ctx, VAContextID context) {
    VkvvDriver* drv  = vkvv_driver_from_ctx(ctx);
    vkvv_trace_va_call_gap(drv, "vaDestroyContext", context, VA_INVALID_SURFACE);
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
    const uint64_t begin_call_us = monotonic_us();
    VkvvDriver* drv  = vkvv_driver_from_ctx(ctx);
    vkvv_trace_va_call_gap(drv, "vaBeginPicture", context, render_target);
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
    vkvv_vulkan_note_surface_decode_begin(surface);
    const uint64_t previous_begin_us = vctx->telemetry_last_begin_us;
    const uint64_t previous_end_us   = vctx->telemetry_last_end_submitted_us;
    const uint64_t delta_begin_us    = previous_begin_us != 0 && begin_call_us >= previous_begin_us ? begin_call_us - previous_begin_us : 0;
    const uint64_t delta_end_us      = previous_end_us != 0 && begin_call_us >= previous_end_us ? begin_call_us - previous_end_us : 0;
    vctx->telemetry_current_begin_us = begin_call_us;
    vctx->telemetry_last_begin_us    = begin_call_us;
    vctx->telemetry_last_render_us   = 0;
    vkvv_trace("va-begin-bound",
               "driver=%llu ctx=%u target=%u stream=%llu codec=0x%x decoded=%u predecode_export=%u begin_monotonic_us=%llu delta_since_last_begin_us=%llu "
               "delta_since_last_end_us=%llu previous_end_monotonic_us=%llu",
               (unsigned long long)drv->driver_instance_id, context, render_target, (unsigned long long)surface->stream_id, surface->codec_operation,
               surface->decoded ? 1U : 0U, vkvv_vulkan_surface_has_predecode_export(surface) ? 1U : 0U, (unsigned long long)begin_call_us,
               (unsigned long long)delta_begin_us, (unsigned long long)delta_end_us, (unsigned long long)previous_end_us);
    if (vctx->decode_ops != NULL) {
        vctx->decode_ops->begin_picture(vctx->decode_state);
    }
    return VA_STATUS_SUCCESS;
}

VAStatus vkvvRenderPicture(VADriverContextP ctx, VAContextID context, VABufferID* buffers, int num_buffers) {
    VkvvDriver* drv  = vkvv_driver_from_ctx(ctx);
    vkvv_trace_va_call_gap(drv, "vaRenderPicture", context, VA_INVALID_SURFACE);
    auto*       vctx = static_cast<VkvvContext*>(vkvv_object_get(drv, context, VKVV_OBJECT_CONTEXT));
    if (vctx == NULL) {
        vkvv_trace("va-render-context-missing", "driver=%llu ctx=%u buffers=%d", (unsigned long long)drv->driver_instance_id, context, num_buffers);
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    VkvvLockGuard context_lock(&vctx->mutex);
    const uint64_t render_call_us      = monotonic_us();
    const uint64_t delta_since_begin_us = vctx->telemetry_current_begin_us != 0 && render_call_us >= vctx->telemetry_current_begin_us ?
        render_call_us - vctx->telemetry_current_begin_us :
        0;
    const uint64_t delta_since_render_us = vctx->telemetry_last_render_us != 0 && render_call_us >= vctx->telemetry_last_render_us ?
        render_call_us - vctx->telemetry_last_render_us :
        0;
    vctx->telemetry_last_render_us = render_call_us;
    for (int i = 0; i < num_buffers; i++) {
        auto* buffer = static_cast<VkvvBuffer*>(vkvv_object_get(drv, buffers[i], VKVV_OBJECT_BUFFER));
        if (buffer == NULL) {
            vkvv_trace("va-render-buffer-missing", "driver=%llu ctx=%u stream=%llu codec=0x%x index=%d buffer=%u target=%u", (unsigned long long)drv->driver_instance_id, context,
                       (unsigned long long)vctx->stream_id, vctx->codec_operation, i, buffers[i], vctx->render_target);
            return VA_STATUS_ERROR_INVALID_BUFFER;
        }
        vkvv_trace("va-render-buffer",
                   "driver=%llu ctx=%u stream=%llu codec=0x%x index=%d buffer=%u type=%u size=%u elements=%u mapped=%u target=%u render_monotonic_us=%llu "
                   "delta_since_begin_us=%llu delta_since_previous_render_us=%llu",
                   (unsigned long long)drv->driver_instance_id, context, (unsigned long long)vctx->stream_id, vctx->codec_operation, i, buffers[i], buffer->type, buffer->size,
                   buffer->num_elements, buffer->mapped ? 1U : 0U, vctx->render_target, (unsigned long long)render_call_us, (unsigned long long)delta_since_begin_us,
                   (unsigned long long)delta_since_render_us);
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
        const uint64_t end_call_us = monotonic_us();
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
        const uint64_t begin_us               = vctx->telemetry_current_begin_us;
        const uint64_t delta_since_begin_us   = begin_us != 0 && end_call_us >= begin_us ? end_call_us - begin_us : 0;
        const uint64_t delta_since_render_us  = vctx->telemetry_last_render_us != 0 && end_call_us >= vctx->telemetry_last_render_us ?
             end_call_us - vctx->telemetry_last_render_us :
             0;
        const uint64_t previous_end_enter_us  = vctx->telemetry_last_end_enter_us;
        const uint64_t delta_since_last_end_enter_us = previous_end_enter_us != 0 && end_call_us >= previous_end_enter_us ? end_call_us - previous_end_enter_us : 0;
        vctx->telemetry_last_end_enter_us     = end_call_us;
        vkvv_trace("va-end-enter",
                   "driver=%llu ctx_stream=%llu ctx_codec=0x%x target=%u surface_stream=%llu surface_codec=0x%x decoded=%u pending=%u predecode_export=%u "
                   "end_enter_monotonic_us=%llu begin_monotonic_us=%llu delta_since_begin_us=%llu delta_since_last_render_us=%llu delta_since_last_end_enter_us=%llu",
                   (unsigned long long)drv->driver_instance_id, (unsigned long long)vctx->stream_id, vctx->codec_operation, vctx->render_target,
                   (unsigned long long)target->stream_id, target->codec_operation, target->decoded ? 1U : 0U, vkvv_surface_has_pending_work(target) ? 1U : 0U,
                   vkvv_vulkan_surface_has_predecode_export(target) ? 1U : 0U, (unsigned long long)end_call_us, (unsigned long long)begin_us,
                   (unsigned long long)delta_since_begin_us, (unsigned long long)delta_since_render_us, (unsigned long long)delta_since_last_end_enter_us);
        auto finish_surface = [drv, vctx, target, end_call_us, begin_us](VAStatus status) {
            const uint64_t finish_us = monotonic_us();
            vctx->telemetry_last_end_submitted_us = finish_us;
            vkvv_surface_complete_work(target, status);
            vctx->render_target = VA_INVALID_ID;
            vkvv_trace("va-end-finish",
                       "driver=%llu target=%u status=%d decoded=%u pending=%u finish_monotonic_us=%llu end_total_us=%llu begin_to_finish_us=%llu",
                       (unsigned long long)drv->driver_instance_id, target->id, status, target->decoded ? 1U : 0U, vkvv_surface_has_pending_work(target) ? 1U : 0U,
                       (unsigned long long)finish_us, (unsigned long long)(finish_us >= end_call_us ? finish_us - end_call_us : 0),
                       (unsigned long long)(begin_us != 0 && finish_us >= begin_us ? finish_us - begin_us : 0));
            return status;
        };

        unsigned int width       = 0;
        unsigned int height      = 0;
        char         reason[512] = {};
        const uint64_t prepare_start_us = monotonic_us();
        VAStatus     status      = vctx->decode_ops->prepare_decode(vctx->decode_state, &width, &height, reason, sizeof(reason));
        const uint64_t prepare_done_us = monotonic_us();
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

        const uint64_t configure_start_us = monotonic_us();
        status = vctx->decode_ops->configure_session(drv->vulkan, vctx->decode_session, target, vctx->decode_state, reason, sizeof(reason));
        const uint64_t configure_done_us = monotonic_us();
        if (reason[0] != '\0') {
            vkvv_log("%s", reason);
        }
        if (status != VA_STATUS_SUCCESS) {
            return finish_surface(status);
        }

        const uint64_t ensure_start_us = monotonic_us();
        status = vctx->decode_ops->ensure_session(drv->vulkan, vctx->decode_session, width, height, reason, sizeof(reason));
        const uint64_t ensure_done_us = monotonic_us();
        vkvv_log("%s", reason);
        if (status != VA_STATUS_SUCCESS) {
            return finish_surface(status);
        }

        const uint64_t decode_start_us = monotonic_us();
        status = vctx->decode_ops->decode(drv->vulkan, vctx->decode_session, drv, vctx, target, vctx->profile, vctx->decode_state, reason, sizeof(reason));
        const uint64_t decode_done_us = monotonic_us();
        vkvv_log("%s", reason);
        if (status != VA_STATUS_SUCCESS) {
            return finish_surface(status);
        }

        const bool pending_export_refresh = vkvv_vulkan_surface_has_pending_export_refresh_work(drv->vulkan, target);
        const bool externally_visible     = vkvv_vulkan_surface_has_exported_backing(target) || target->import.external;
        uint64_t visible_drain_start_us   = 0;
        uint64_t visible_drain_done_us    = 0;
        if (vkvv_surface_has_pending_work(target) && pending_export_refresh && externally_visible) {
            visible_drain_start_us = monotonic_us();
            vkvv_trace("va-end-visible-drain", "driver=%llu target=%u stream=%llu codec=0x%x exported=%u import_external=%u", (unsigned long long)drv->driver_instance_id,
                       target->id, (unsigned long long)target->stream_id, target->codec_operation, vkvv_vulkan_surface_has_exported_backing(target) ? 1U : 0U,
                       target->import.external ? 1U : 0U);
            status = vkvv_vulkan_complete_surface_work(drv->vulkan, target, VA_TIMEOUT_INFINITE, reason, sizeof(reason));
            visible_drain_done_us = monotonic_us();
            if (reason[0] != '\0') {
                vkvv_log("%s", reason);
            }
            if (status != VA_STATUS_SUCCESS) {
                return finish_surface(status);
            }
        }

        const uint64_t end_submitted_us = monotonic_us();
        vkvv_trace("va-end-timing",
                   "driver=%llu target=%u stream=%llu codec=0x%x status=%d begin_monotonic_us=%llu end_enter_monotonic_us=%llu end_submitted_monotonic_us=%llu "
                   "begin_to_end_enter_us=%llu begin_to_end_submitted_us=%llu end_total_us=%llu prepare_us=%llu configure_us=%llu ensure_us=%llu decode_submit_us=%llu "
                   "visible_drain_us=%llu pending=%u",
                   (unsigned long long)drv->driver_instance_id, target->id, (unsigned long long)target->stream_id, target->codec_operation, status,
                   (unsigned long long)begin_us, (unsigned long long)end_call_us, (unsigned long long)end_submitted_us,
                   (unsigned long long)(begin_us != 0 && end_call_us >= begin_us ? end_call_us - begin_us : 0),
                   (unsigned long long)(begin_us != 0 && end_submitted_us >= begin_us ? end_submitted_us - begin_us : 0),
                   (unsigned long long)(end_submitted_us >= end_call_us ? end_submitted_us - end_call_us : 0),
                   (unsigned long long)(prepare_done_us >= prepare_start_us ? prepare_done_us - prepare_start_us : 0),
                   (unsigned long long)(configure_done_us >= configure_start_us ? configure_done_us - configure_start_us : 0),
                   (unsigned long long)(ensure_done_us >= ensure_start_us ? ensure_done_us - ensure_start_us : 0),
                   (unsigned long long)(decode_done_us >= decode_start_us ? decode_done_us - decode_start_us : 0),
                   (unsigned long long)(visible_drain_done_us >= visible_drain_start_us ? visible_drain_done_us - visible_drain_start_us : 0),
                   vkvv_surface_has_pending_work(target) ? 1U : 0U);
        vctx->telemetry_last_end_submitted_us = end_submitted_us;
        vkvv_trace("va-end-submitted",
                   "driver=%llu target=%u stream=%llu codec=0x%x pending=%u predecode_export=%u end_submitted_monotonic_us=%llu begin_to_end_submitted_us=%llu "
                   "delta_since_end_enter_us=%llu",
                   (unsigned long long)drv->driver_instance_id, target->id, (unsigned long long)target->stream_id, target->codec_operation,
                   vkvv_surface_has_pending_work(target) ? 1U : 0U, vkvv_vulkan_surface_has_predecode_export(target) ? 1U : 0U,
                   (unsigned long long)end_submitted_us, (unsigned long long)(begin_us != 0 && end_submitted_us >= begin_us ? end_submitted_us - begin_us : 0),
                   (unsigned long long)(end_submitted_us >= end_call_us ? end_submitted_us - end_call_us : 0));
        vctx->render_target = VA_INVALID_ID;
        return VA_STATUS_SUCCESS;
    }

} // namespace

VAStatus vkvvEndPicture(VADriverContextP ctx, VAContextID context) {
    VkvvDriver* drv  = vkvv_driver_from_ctx(ctx);
    vkvv_trace_va_call_gap(drv, "vaEndPicture", context, VA_INVALID_SURFACE);
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
