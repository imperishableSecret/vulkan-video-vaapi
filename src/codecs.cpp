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

const VkvvDecodeOps h264_decode_ops = {
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

const VkvvDecodeOps *vkvv_decode_ops_for_profile_entrypoint(VAProfile profile, VAEntrypoint entrypoint) {
    if (entrypoint == VAEntrypointVLD && vkvv_profile_is_h264(profile)) {
        return &h264_decode_ops;
    }
    return nullptr;
}

const VkvvEncodeOps *vkvv_encode_ops_for_profile_entrypoint(VAProfile profile, VAEntrypoint entrypoint) {
    (void) profile;
    (void) entrypoint;
    return nullptr;
}
