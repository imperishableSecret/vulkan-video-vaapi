#include "va_private.h"
#include "h264.h"
#include "vulkan/h264/api.h"

namespace {

VAStatus h264_decode(
        void *runtime,
        void *session,
        VkvvDriver *drv,
        VkvvContext *vctx,
        VkvvSurface *target,
        VAProfile profile,
        void *state,
        char *reason,
        size_t reason_size) {
    VkvvH264DecodeInput input = {};
    VAStatus status = vkvv_h264_get_decode_input(state, &input);
    if (status != VA_STATUS_SUCCESS) {
        return status;
    }
    return vkvv_vulkan_decode_h264(runtime, session, drv, vctx, target, profile, &input, reason, reason_size);
}

const VkvvCodecOps h264_ops = {
    "h264",
    vkvv_h264_state_create,
    vkvv_h264_state_destroy,
    vkvv_vulkan_h264_session_create,
    vkvv_vulkan_h264_session_destroy,
    vkvv_h264_begin_picture,
    vkvv_h264_render_buffer,
    vkvv_h264_prepare_decode,
    vkvv_vulkan_ensure_h264_session,
    h264_decode,
};

} // namespace

const VkvvCodecOps *vkvv_codec_ops_for_profile(VAProfile profile) {
    if (vkvv_profile_is_h264(profile)) {
        return &h264_ops;
    }
    return nullptr;
}
