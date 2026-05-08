#include "va_private.h"

#include <cstdio>
#include <cstring>

namespace {

bool check(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
    }
    return condition;
}

bool ops_complete(const VkvvDecodeOps *ops) {
    return ops != nullptr &&
           ops->name != nullptr &&
           ops->state_create != nullptr &&
           ops->state_destroy != nullptr &&
           ops->session_create != nullptr &&
           ops->session_destroy != nullptr &&
           ops->begin_picture != nullptr &&
           ops->render_buffer != nullptr &&
           ops->prepare_decode != nullptr &&
           ops->ensure_session != nullptr &&
           ops->decode != nullptr;
}

} // namespace

int main(void) {
    bool ok = true;

    const VkvvDecodeOps *h264 = vkvv_decode_ops_for_profile_entrypoint(VAProfileH264High, VAEntrypointVLD);
    ok = check(ops_complete(h264), "H.264 decode ops are incomplete") && ok;
    if (h264 != nullptr) {
        ok = check(std::strcmp(h264->name, "h264") == 0, "H.264 decode ops used the wrong name") && ok;
    }

    void *state = h264 != nullptr ? h264->state_create() : nullptr;
    ok = check(state != nullptr, "H.264 codec state allocation failed") && ok;
    if (state != nullptr) {
        h264->begin_picture(state);
        h264->state_destroy(state);
    }

    void *session = h264 != nullptr ? h264->session_create() : nullptr;
    ok = check(session != nullptr, "H.264 codec session allocation failed") && ok;
    if (session != nullptr) {
        h264->session_destroy(nullptr, session);
    }

    ok = check(vkvv_decode_ops_for_profile_entrypoint(VAProfileH264High, VAEntrypointEncSlice) == nullptr,
               "H.264 encode entrypoint must not resolve decode ops") && ok;
    ok = check(vkvv_decode_ops_for_profile_entrypoint(VAProfileHEVCMain, VAEntrypointVLD) == nullptr,
               "HEVC should not have decode ops before its decoder is wired") && ok;
    ok = check(vkvv_decode_ops_for_profile_entrypoint(VAProfileVP9Profile0, VAEntrypointVLD) == nullptr,
               "VP9 should not have decode ops before its decoder is wired") && ok;
    ok = check(vkvv_decode_ops_for_profile_entrypoint(VAProfileAV1Profile0, VAEntrypointVLD) == nullptr,
               "AV1 should not have decode ops before its decoder is wired") && ok;
    ok = check(vkvv_encode_ops_for_profile_entrypoint(VAProfileH264High, VAEntrypointEncSlice) == nullptr,
               "H.264 EncSlice should not have encode ops before encode is wired") && ok;
    ok = check(vkvv_encode_ops_for_profile_entrypoint(VAProfileH264High, VAEntrypointEncSliceLP) == nullptr,
               "H.264 EncSliceLP should not have encode ops before encode is wired") && ok;
    ok = check(vkvv_encode_ops_for_profile_entrypoint(VAProfileH264High, VAEntrypointEncPicture) == nullptr,
               "H.264 EncPicture should not have encode ops before encode is wired") && ok;

    VkvvDriver drv{};
    drv.caps.h264 = true;
    drv.caps.h265 = true;
    drv.caps.h265_10 = true;
    drv.caps.vp9 = true;
    drv.caps.av1 = true;
    drv.caps.surface_export = true;
    vkvv_init_profile_capabilities(&drv);

    const VkvvProfileCapability *h264_decode = vkvv_profile_capability_for_entrypoint(
        &drv, VAProfileH264High, VAEntrypointVLD);
    ok = check(h264_decode != nullptr && h264_decode->advertise &&
                   h264_decode->direction == VKVV_CODEC_DIRECTION_DECODE,
               "H.264 VLD should be an advertised decode capability") && ok;

    const VkvvProfileCapability *hevc_hidden = vkvv_profile_capability_record(
        &drv, VAProfileHEVCMain, VAEntrypointVLD, VKVV_CODEC_DIRECTION_DECODE);
    ok = check(hevc_hidden != nullptr &&
                   hevc_hidden->hardware_supported &&
                   !hevc_hidden->runtime_wired &&
                   !hevc_hidden->advertise,
               "HEVC hardware capability should remain hidden until decode is wired") && ok;

    const VkvvProfileCapability *h264_encode = vkvv_profile_capability_record(
        &drv, VAProfileH264High, VAEntrypointEncSlice, VKVV_CODEC_DIRECTION_ENCODE);
    ok = check(h264_encode != nullptr &&
                   h264_encode->hardware_supported &&
                   !h264_encode->parser_wired &&
                   !h264_encode->runtime_wired &&
                   !h264_encode->surface_wired &&
                   !h264_encode->advertise,
               "H.264 encode descriptor should be present but inert") && ok;
    ok = check(vkvv_profile_capability_for_entrypoint(
                   &drv, VAProfileH264High, VAEntrypointEncSlice) == nullptr,
               "H.264 encode entrypoint must not be advertised") && ok;

    if (!ok) {
        return 1;
    }

    std::printf("codec ops smoke passed\n");
    return 0;
}
