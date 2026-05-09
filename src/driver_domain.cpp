#include "va_private.h"
#include "telemetry.h"

namespace {

bool decode_context_has_domain(const VkvvContext *vctx) {
    return vctx != NULL &&
           vctx->mode == VKVV_CONTEXT_MODE_DECODE &&
           vctx->stream_id != 0 &&
           vctx->codec_operation != 0 &&
           vctx->width != 0 &&
           vctx->height != 0;
}

void note_decode_domain_locked(VkvvDriver *drv, const VkvvContext *vctx, const VkvvSurface *surface) {
    if (drv == NULL || !decode_context_has_domain(vctx)) {
        return;
    }
    if (surface != NULL &&
        surface->driver_instance_id != 0 &&
        drv->driver_instance_id != 0 &&
        surface->driver_instance_id != drv->driver_instance_id) {
        return;
    }
    const uint64_t previous_stream = drv->active_decode_stream_id;
    const unsigned int previous_codec = drv->active_decode_codec_operation;
    const bool codec_transition =
        previous_stream != 0 &&
        (previous_stream != vctx->stream_id || previous_codec != vctx->codec_operation);

    drv->active_decode_stream_id = vctx->stream_id;
    drv->active_decode_codec_operation = vctx->codec_operation;
    drv->active_decode_width = surface != NULL && surface->width != 0 ? surface->width : vctx->width;
    drv->active_decode_height = surface != NULL && surface->height != 0 ? surface->height : vctx->height;
    drv->active_decode_rt_format = surface != NULL ? surface->rt_format : 0;
    drv->active_decode_fourcc = surface != NULL ? surface->fourcc : 0;
    vkvv_trace("domain-note",
               "driver=%llu stream=%llu codec=0x%x width=%u height=%u rt=0x%x fourcc=0x%x surface=%u",
               (unsigned long long) drv->driver_instance_id,
               (unsigned long long) drv->active_decode_stream_id,
               drv->active_decode_codec_operation,
               drv->active_decode_width,
               drv->active_decode_height,
               drv->active_decode_rt_format,
               drv->active_decode_fourcc,
               surface != NULL ? surface->id : VA_INVALID_ID);
    if (codec_transition) {
        vkvv_trace("domain-codec-transition",
                   "driver=%llu prev_stream=%llu prev_codec=0x%x new_stream=%llu new_codec=0x%x surface=%u width=%u height=%u rt=0x%x fourcc=0x%x",
                   (unsigned long long) drv->driver_instance_id,
                   (unsigned long long) previous_stream,
                   previous_codec,
                   (unsigned long long) drv->active_decode_stream_id,
                   drv->active_decode_codec_operation,
                   surface != NULL ? surface->id : VA_INVALID_ID,
                   drv->active_decode_width,
                   drv->active_decode_height,
                   drv->active_decode_rt_format,
                   drv->active_decode_fourcc);
    }
}

bool surface_matches_active_decode_domain_locked(const VkvvDriver *drv, const VkvvSurface *surface) {
    if (drv == NULL || surface == NULL) {
        return false;
    }
    if (surface->stream_id != 0 || surface->codec_operation != 0) {
        return false;
    }
    if (drv->active_decode_stream_id == 0 || drv->active_decode_codec_operation == 0) {
        return false;
    }
    if (surface->driver_instance_id != 0 &&
        drv->driver_instance_id != 0 &&
        surface->driver_instance_id != drv->driver_instance_id) {
        return false;
    }
    if (drv->active_decode_width == 0 || drv->active_decode_height == 0 ||
        surface->width != drv->active_decode_width ||
        surface->height != drv->active_decode_height) {
        return false;
    }
    if (drv->active_decode_rt_format != 0 && surface->rt_format != drv->active_decode_rt_format) {
        return false;
    }
    if (drv->active_decode_fourcc != 0 && surface->fourcc != drv->active_decode_fourcc) {
        return false;
    }
    return true;
}

} // namespace

void vkvv_driver_note_decode_domain_locked(VkvvDriver *drv, const VkvvContext *vctx, const VkvvSurface *surface) {
    note_decode_domain_locked(drv, vctx, surface);
}

void vkvv_driver_note_decode_domain(VkvvDriver *drv, const VkvvContext *vctx, const VkvvSurface *surface) {
    if (drv == NULL) {
        return;
    }
    VkvvLockGuard state_lock(&drv->state_mutex);
    vkvv_driver_note_decode_domain_locked(drv, vctx, surface);
}

bool vkvv_driver_apply_active_decode_domain_locked(VkvvDriver *drv, VkvvSurface *surface) {
    if (!surface_matches_active_decode_domain_locked(drv, surface)) {
        return false;
    }
    surface->stream_id = drv->active_decode_stream_id;
    surface->codec_operation = drv->active_decode_codec_operation;
    vkvv_trace("domain-apply",
               "driver=%llu surface=%u stream=%llu codec=0x%x width=%u height=%u",
               (unsigned long long) drv->driver_instance_id,
               surface->id,
               (unsigned long long) surface->stream_id,
               surface->codec_operation,
               surface->width,
               surface->height);
    return true;
}

bool vkvv_driver_apply_active_decode_domain(VkvvDriver *drv, VkvvSurface *surface) {
    if (drv == NULL) {
        return false;
    }
    VkvvLockGuard state_lock(&drv->state_mutex);
    return vkvv_driver_apply_active_decode_domain_locked(drv, surface);
}
