#include "va_private.h"
#include "codecs/ops.h"
#include "codecs/av1/av1.h"
#include "codecs/h264/h264.h"
#include "codecs/vp9/vp9.h"
#include "vulkan/codecs/av1/api.h"
#include "vulkan/codecs/h264/api.h"
#include "vulkan/codecs/vp9/api.h"

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

VAStatus vp9_decode(
        void *runtime,
        void *session,
        VkvvDriver *drv,
        VkvvContext *vctx,
        VkvvSurface *target,
        VAProfile profile,
        void *state,
        char *reason,
        size_t reason_size) {
    VkvvVP9DecodeInput input = {};
    VAStatus status = vkvv_vp9_get_decode_input(state, &input);
    if (status != VA_STATUS_SUCCESS) {
        return status;
    }
    return vkvv_vulkan_decode_vp9(runtime, session, drv, vctx, target, profile, &input, reason, reason_size);
}

const VkvvDecodeOps vp9_profile0_decode_ops = {
    "vp9",
    vkvv_vp9_state_create,
    vkvv_vp9_state_destroy,
    vkvv_vulkan_vp9_session_create,
    vkvv_vulkan_vp9_session_destroy,
    vkvv_vp9_begin_picture,
    vkvv_vp9_render_buffer,
    vkvv_vp9_prepare_decode,
    vkvv_vulkan_ensure_vp9_session,
    vp9_decode,
};

const VkvvDecodeOps vp9_profile2_decode_ops = {
    "vp9-profile2",
    vkvv_vp9_state_create,
    vkvv_vp9_state_destroy,
    vkvv_vulkan_vp9_profile2_session_create,
    vkvv_vulkan_vp9_session_destroy,
    vkvv_vp9_begin_picture,
    vkvv_vp9_render_buffer,
    vkvv_vp9_prepare_decode,
    vkvv_vulkan_ensure_vp9_session,
    vp9_decode,
};

VAStatus av1_decode(
        void *runtime,
        void *session,
        VkvvDriver *drv,
        VkvvContext *vctx,
        VkvvSurface *target,
        VAProfile profile,
        void *state,
        char *reason,
        size_t reason_size) {
    VkvvAV1DecodeInput input = {};
    VAStatus status = vkvv_av1_get_decode_input(state, &input);
    if (status != VA_STATUS_SUCCESS) {
        return status;
    }
    return vkvv_vulkan_decode_av1(runtime, session, drv, vctx, target, profile, &input, reason, reason_size);
}

const VkvvDecodeOps av1_profile0_decode_ops = {
    "av1",
    vkvv_av1_state_create,
    vkvv_av1_state_destroy,
    vkvv_vulkan_av1_session_create,
    vkvv_vulkan_av1_session_destroy,
    vkvv_av1_begin_picture,
    vkvv_av1_render_buffer,
    vkvv_av1_prepare_decode,
    vkvv_vulkan_ensure_av1_session,
    av1_decode,
};

struct DecodeRegistryEntry {
    VAEntrypoint entrypoint;
    bool (*profile_matches)(VAProfile profile);
    const VkvvDecodeOps *ops;
};

bool h264_profile_matches(VAProfile profile) {
    return vkvv_profile_is_h264(profile);
}

bool vp9_profile_matches(VAProfile profile) {
    return profile == VAProfileVP9Profile0;
}

bool vp9_profile2_matches(VAProfile profile) {
    return profile == VAProfileVP9Profile2;
}

bool av1_profile0_matches(VAProfile profile) {
    return profile == VAProfileAV1Profile0;
}

const DecodeRegistryEntry decode_registry[] = {
    {VAEntrypointVLD, h264_profile_matches, &h264_decode_ops},
    {VAEntrypointVLD, vp9_profile_matches, &vp9_profile0_decode_ops},
    {VAEntrypointVLD, vp9_profile2_matches, &vp9_profile2_decode_ops},
    {VAEntrypointVLD, av1_profile0_matches, &av1_profile0_decode_ops},
};

} // namespace

const VkvvDecodeOps *vkvv_decode_ops_for_profile_entrypoint(VAProfile profile, VAEntrypoint entrypoint) {
    for (const DecodeRegistryEntry &entry : decode_registry) {
        if (entry.entrypoint == entrypoint && entry.profile_matches != nullptr && entry.profile_matches(profile)) {
            return entry.ops;
        }
    }
    return nullptr;
}

const VkvvEncodeOps *vkvv_encode_ops_for_profile_entrypoint(VAProfile profile, VAEntrypoint entrypoint) {
    (void) profile;
    (void) entrypoint;
    return nullptr;
}
