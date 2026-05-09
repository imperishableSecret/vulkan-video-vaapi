#include "va/private.h"
#include "codecs/ops.h"
#include "codecs/av1/av1.h"
#include "codecs/h264/h264.h"
#include "codecs/hevc/hevc.h"
#include "codecs/vp9/vp9.h"
#include "vulkan/codecs/av1/api.h"
#include "vulkan/codecs/h264/api.h"
#include "vulkan/codecs/hevc/api.h"
#include "vulkan/codecs/vp9/api.h"

namespace {

    void* h264_session_create(const VkvvConfig* config) {
        (void)config;
        return vkvv_vulkan_h264_session_create();
    }

    void* vp9_profile0_session_create(const VkvvConfig* config) {
        (void)config;
        return vkvv_vulkan_vp9_session_create();
    }

    void* vp9_profile2_session_create(const VkvvConfig* config) {
        (void)config;
        return vkvv_vulkan_vp9_profile2_session_create();
    }

    void* av1_session_create(const VkvvConfig* config) {
        return vkvv_vulkan_av1_session_create_for_config(config);
    }

    void* hevc_session_create(const VkvvConfig* config) {
        return vkvv_vulkan_hevc_session_create_for_config(config);
    }

    VAStatus noop_configure_session(void* runtime, void* session, const VkvvSurface* target, void* state, char* reason, size_t reason_size) {
        (void)runtime;
        (void)session;
        (void)target;
        (void)state;
        if (reason != nullptr && reason_size > 0) {
            reason[0] = '\0';
        }
        return VA_STATUS_SUCCESS;
    }

    VAStatus h264_decode(void* runtime, void* session, VkvvDriver* drv, VkvvContext* vctx, VkvvSurface* target, VAProfile profile, void* state, char* reason, size_t reason_size) {
        VkvvH264DecodeInput input  = {};
        VAStatus            status = vkvv_h264_get_decode_input(state, &input);
        if (status != VA_STATUS_SUCCESS) {
            return status;
        }
        return vkvv_vulkan_decode_h264(runtime, session, drv, vctx, target, profile, &input, reason, reason_size);
    }

    const VkvvDecodeOps h264_decode_ops = {
        "h264",
        vkvv_h264_state_create,
        vkvv_h264_state_destroy,
        h264_session_create,
        vkvv_vulkan_h264_session_destroy,
        vkvv_h264_begin_picture,
        vkvv_h264_render_buffer,
        vkvv_h264_prepare_decode,
        noop_configure_session,
        vkvv_vulkan_ensure_h264_session,
        h264_decode,
    };

    VAStatus hevc_decode(void* runtime, void* session, VkvvDriver* drv, VkvvContext* vctx, VkvvSurface* target, VAProfile profile, void* state, char* reason, size_t reason_size) {
        VkvvHEVCDecodeInput input  = {};
        VAStatus            status = vkvv_hevc_get_decode_input(state, &input);
        if (status != VA_STATUS_SUCCESS) {
            return status;
        }
        return vkvv_vulkan_decode_hevc(runtime, session, drv, vctx, target, profile, &input, reason, reason_size);
    }

    const VkvvDecodeOps hevc_main_decode_ops = {
        "hevc-main",
        vkvv_hevc_state_create,
        vkvv_hevc_state_destroy,
        hevc_session_create,
        vkvv_vulkan_hevc_session_destroy,
        vkvv_hevc_begin_picture,
        vkvv_hevc_render_buffer,
        vkvv_hevc_prepare_decode,
        noop_configure_session,
        vkvv_vulkan_ensure_hevc_session,
        hevc_decode,
    };

    const VkvvDecodeOps hevc_main10_decode_ops = {
        "hevc-main10",
        vkvv_hevc_state_create,
        vkvv_hevc_state_destroy,
        hevc_session_create,
        vkvv_vulkan_hevc_session_destroy,
        vkvv_hevc_begin_picture,
        vkvv_hevc_render_buffer,
        vkvv_hevc_prepare_decode,
        noop_configure_session,
        vkvv_vulkan_ensure_hevc_session,
        hevc_decode,
    };

    VAStatus vp9_decode(void* runtime, void* session, VkvvDriver* drv, VkvvContext* vctx, VkvvSurface* target, VAProfile profile, void* state, char* reason, size_t reason_size) {
        VkvvVP9DecodeInput input  = {};
        VAStatus           status = vkvv_vp9_get_decode_input(state, &input);
        if (status != VA_STATUS_SUCCESS) {
            return status;
        }
        return vkvv_vulkan_decode_vp9(runtime, session, drv, vctx, target, profile, &input, reason, reason_size);
    }

    const VkvvDecodeOps vp9_profile0_decode_ops = {
        "vp9",
        vkvv_vp9_state_create,
        vkvv_vp9_state_destroy,
        vp9_profile0_session_create,
        vkvv_vulkan_vp9_session_destroy,
        vkvv_vp9_begin_picture,
        vkvv_vp9_render_buffer,
        vkvv_vp9_prepare_decode,
        noop_configure_session,
        vkvv_vulkan_ensure_vp9_session,
        vp9_decode,
    };

    const VkvvDecodeOps vp9_profile2_decode_ops = {
        "vp9-profile2",
        vkvv_vp9_state_create,
        vkvv_vp9_state_destroy,
        vp9_profile2_session_create,
        vkvv_vulkan_vp9_session_destroy,
        vkvv_vp9_begin_picture,
        vkvv_vp9_render_buffer,
        vkvv_vp9_prepare_decode,
        noop_configure_session,
        vkvv_vulkan_ensure_vp9_session,
        vp9_decode,
    };

    VAStatus av1_configure_session(void* runtime, void* session, const VkvvSurface* target, void* state, char* reason, size_t reason_size) {
        VkvvAV1DecodeInput input  = {};
        VAStatus           status = vkvv_av1_get_decode_input(state, &input);
        if (status != VA_STATUS_SUCCESS) {
            return status;
        }
        return vkvv_vulkan_configure_av1_session(runtime, session, target, &input, reason, reason_size);
    }

    VAStatus av1_decode(void* runtime, void* session, VkvvDriver* drv, VkvvContext* vctx, VkvvSurface* target, VAProfile profile, void* state, char* reason, size_t reason_size) {
        VkvvAV1DecodeInput input  = {};
        VAStatus           status = vkvv_av1_get_decode_input(state, &input);
        if (status != VA_STATUS_SUCCESS) {
            return status;
        }
        return vkvv_vulkan_decode_av1(runtime, session, drv, vctx, target, profile, &input, reason, reason_size);
    }

    const VkvvDecodeOps av1_profile0_decode_ops = {
        "av1",
        vkvv_av1_state_create,
        vkvv_av1_state_destroy,
        av1_session_create,
        vkvv_vulkan_av1_session_destroy,
        vkvv_av1_begin_picture,
        vkvv_av1_render_buffer,
        vkvv_av1_prepare_decode,
        av1_configure_session,
        vkvv_vulkan_ensure_av1_session,
        av1_decode,
    };

    struct DecodeRegistryEntry {
        VAEntrypoint entrypoint;
        bool (*profile_matches)(VAProfile profile);
        const VkvvDecodeOps* ops;
    };

    bool h264_profile_matches(VAProfile profile) {
        return vkvv_profile_is_h264(profile);
    }

    bool vp9_profile_matches(VAProfile profile) {
        return profile == VAProfileVP9Profile0;
    }

    bool hevc_main_profile_matches(VAProfile profile) {
        return profile == VAProfileHEVCMain;
    }

    bool hevc_main10_profile_matches(VAProfile profile) {
        return profile == VAProfileHEVCMain10;
    }

    bool vp9_profile2_matches(VAProfile profile) {
        return profile == VAProfileVP9Profile2;
    }

    bool av1_profile0_matches(VAProfile profile) {
        return profile == VAProfileAV1Profile0;
    }

    const DecodeRegistryEntry decode_registry[] = {
        {VAEntrypointVLD, h264_profile_matches, &h264_decode_ops},
        {VAEntrypointVLD, hevc_main_profile_matches, &hevc_main_decode_ops},
        {VAEntrypointVLD, hevc_main10_profile_matches, &hevc_main10_decode_ops},
        {VAEntrypointVLD, vp9_profile_matches, &vp9_profile0_decode_ops},
        {VAEntrypointVLD, vp9_profile2_matches, &vp9_profile2_decode_ops},
        {VAEntrypointVLD, av1_profile0_matches, &av1_profile0_decode_ops},
    };

} // namespace

const VkvvDecodeOps* vkvv_decode_ops_for_profile_entrypoint(VAProfile profile, VAEntrypoint entrypoint) {
    for (const DecodeRegistryEntry& entry : decode_registry) {
        if (entry.entrypoint == entrypoint && entry.profile_matches != nullptr && entry.profile_matches(profile)) {
            return entry.ops;
        }
    }
    return nullptr;
}

const VkvvEncodeOps* vkvv_encode_ops_for_profile_entrypoint(VAProfile profile, VAEntrypoint entrypoint) {
    (void)profile;
    (void)entrypoint;
    return nullptr;
}
