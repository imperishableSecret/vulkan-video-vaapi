#include "va_private.h"
#include "codecs/vp9/vp9.h"

#include <cstdio>
#include <cstring>
#include <vector>

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

std::vector<uint8_t> make_vp9_keyframe() {
    return {
        0x82, 0x49, 0x83, 0x42, 0x00, 0x00, 0xf0, 0x00,
        0xf6, 0x06, 0x38, 0x24, 0x1c, 0x18, 0x42, 0x00,
        0x00, 0x20, 0x40, 0x00, 0x22, 0x9b, 0xff, 0xff,
        0xa5, 0x13, 0xfb, 0x82, 0x94, 0xde, 0x8f, 0x15,
        0x4a, 0xb7, 0x6d, 0x27, 0xfd, 0x54, 0xfd, 0x19,
        0xed, 0x94, 0x52, 0xd3, 0x71, 0xd2, 0x6c, 0xc8,
        0xfe, 0xa6, 0xf7, 0x65, 0x4c, 0xbc, 0x26, 0x0c,
        0x80, 0x00,
    };
}

std::vector<uint8_t> make_vp9_profile2_keyframe() {
    return {
        0x92, 0x49, 0x83, 0x42, 0x00, 0x00, 0x78, 0x00,
        0x7b, 0x03, 0x1c, 0x12, 0x0e, 0x0c, 0x29, 0x00,
        0x00, 0x10, 0x40, 0x00, 0x10, 0xbf, 0xff, 0xfd,
        0x25, 0x10, 0xdf, 0x98, 0x00,
    };
}

bool check_vp9_parser(
        const VkvvDecodeOps *vp9,
        uint8_t profile,
        uint8_t bit_depth,
        const std::vector<uint8_t> &bitstream,
        const char *label) {
    void *state = vp9 != nullptr ? vp9->state_create() : nullptr;
    if (!check(state != nullptr, "VP9 codec state allocation failed")) {
        return false;
    }

    vp9->begin_picture(state);

    VADecPictureParameterBufferVP9 pic{};
    pic.frame_width = 16;
    pic.frame_height = 16;
    pic.pic_fields.bits.subsampling_x = 1;
    pic.pic_fields.bits.subsampling_y = 1;
    pic.pic_fields.bits.error_resilient_mode = 1;
    pic.profile = profile;
    pic.bit_depth = bit_depth;

    VASliceParameterBufferVP9 slice{};
    slice.slice_data_size = static_cast<uint32_t>(bitstream.size());

    VkvvBuffer pic_buffer{};
    pic_buffer.type = VAPictureParameterBufferType;
    pic_buffer.size = sizeof(pic);
    pic_buffer.num_elements = 1;
    pic_buffer.data = &pic;

    VkvvBuffer slice_buffer{};
    slice_buffer.type = VASliceParameterBufferType;
    slice_buffer.size = sizeof(slice);
    slice_buffer.num_elements = 1;
    slice_buffer.data = &slice;

    VkvvBuffer data_buffer{};
    data_buffer.type = VASliceDataBufferType;
    data_buffer.size = static_cast<unsigned int>(bitstream.size());
    data_buffer.num_elements = 1;
    data_buffer.data = const_cast<uint8_t *>(bitstream.data());

    bool ok = check(vp9->render_buffer(state, &pic_buffer) == VA_STATUS_SUCCESS,
                    "VP9 picture buffer ingestion failed");
    ok = check(vp9->render_buffer(state, &slice_buffer) == VA_STATUS_SUCCESS,
               "VP9 slice buffer ingestion failed") && ok;
    ok = check(vp9->render_buffer(state, &data_buffer) == VA_STATUS_SUCCESS,
               "VP9 slice data ingestion failed") && ok;

    unsigned int width = 0;
    unsigned int height = 0;
    char reason[512] = {};
    ok = check(vp9->prepare_decode(state, &width, &height, reason, sizeof(reason)) == VA_STATUS_SUCCESS,
               "VP9 prepare_decode failed") && ok;
    if (!ok) {
        std::fprintf(stderr, "%s\n", reason);
    }
    ok = check(width == 16 && height == 16, "VP9 prepare_decode returned the wrong dimensions") && ok;

    VkvvVP9DecodeInput input{};
    ok = check(vkvv_vp9_get_decode_input(state, &input) == VA_STATUS_SUCCESS,
               "VP9 decode input extraction failed") && ok;
    const bool parsed =
        input.header.valid &&
        input.header.refresh_frame_flags == 0xff &&
        input.header.frame_header_length_in_bytes > 0 &&
        input.header.first_partition_size > 0 &&
        input.bitstream_size == bitstream.size();
    if (!parsed) {
        std::fprintf(stderr,
                     "%s parsed header mismatch: valid=%u refresh=0x%02x profile=%u depth=%u q=%u header=%u first_partition=%u bytes=%zu\n",
                     label,
                     input.header.valid,
                     input.header.refresh_frame_flags,
                     input.header.profile,
                     input.header.bit_depth,
                     input.header.base_q_idx,
                     input.header.frame_header_length_in_bytes,
                     input.header.first_partition_size,
                     input.bitstream_size);
    }
    ok = check(parsed, "VP9 parsed header did not expose expected keyframe values") && ok;
    ok = check(input.header.profile == profile && input.header.bit_depth == bit_depth,
               "VP9 parsed header returned the wrong profile/depth") && ok;

    vp9->state_destroy(state);
    return ok;
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
    const VkvvDecodeOps *vp9 = vkvv_decode_ops_for_profile_entrypoint(VAProfileVP9Profile0, VAEntrypointVLD);
    ok = check(ops_complete(vp9), "VP9 decode ops are incomplete") && ok;
    if (vp9 != nullptr) {
        ok = check(std::strcmp(vp9->name, "vp9") == 0, "VP9 decode ops used the wrong name") && ok;
        ok = check_vp9_parser(vp9, 0, 8, make_vp9_keyframe(), "VP9 Profile0") && ok;
    }
    const VkvvDecodeOps *vp9_profile2 = vkvv_decode_ops_for_profile_entrypoint(VAProfileVP9Profile2, VAEntrypointVLD);
    ok = check(ops_complete(vp9_profile2), "VP9 Profile2 decode ops are incomplete") && ok;
    if (vp9_profile2 != nullptr) {
        ok = check(std::strcmp(vp9_profile2->name, "vp9-profile2") == 0,
                   "VP9 Profile2 decode ops used the wrong name") && ok;
        ok = check_vp9_parser(vp9_profile2, 2, 10, make_vp9_profile2_keyframe(), "VP9 Profile2") && ok;
    }
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
    drv.caps.h265_12 = true;
    drv.caps.vp9 = true;
    drv.caps.vp9_10 = true;
    drv.caps.vp9_12 = true;
    drv.caps.av1 = true;
    drv.caps.av1_10 = true;
    drv.caps.surface_export = true;
    drv.caps.surface_export_nv12 = true;
    drv.caps.surface_export_p010 = true;
    vkvv_init_profile_capabilities(&drv);

    const VkvvProfileCapability *h264_decode = vkvv_profile_capability_for_entrypoint(
        &drv, VAProfileH264High, VAEntrypointVLD);
    ok = check(h264_decode != nullptr && h264_decode->advertise &&
                   h264_decode->direction == VKVV_CODEC_DIRECTION_DECODE,
               "H.264 VLD should be an advertised decode capability") && ok;
    ok = check(h264_decode != nullptr &&
                   h264_decode->format_count == 1 &&
                   h264_decode->formats[0].fourcc == VA_FOURCC_NV12,
               "H.264 decode capability should expose one NV12 format variant") && ok;

    const VkvvProfileCapability *vp9_decode = vkvv_profile_capability_for_entrypoint(
        &drv, VAProfileVP9Profile0, VAEntrypointVLD);
    ok = check(vp9_decode != nullptr && vp9_decode->advertise &&
                   vp9_decode->direction == VKVV_CODEC_DIRECTION_DECODE &&
                   vp9_decode->format_count == 1 &&
                   vp9_decode->formats[0].fourcc == VA_FOURCC_NV12,
               "VP9 Profile0 should be advertised with one NV12 format variant") && ok;

    const VkvvProfileCapability *vp9_profile2_decode = vkvv_profile_capability_for_entrypoint(
        &drv, VAProfileVP9Profile2, VAEntrypointVLD);
    ok = check(vp9_profile2_decode != nullptr && vp9_profile2_decode->advertise &&
                   vp9_profile2_decode->direction == VKVV_CODEC_DIRECTION_DECODE &&
                   vp9_profile2_decode->format_count == 2 &&
                   vp9_profile2_decode->formats[0].fourcc == VA_FOURCC_P010 &&
                   vp9_profile2_decode->formats[0].advertise &&
                   vp9_profile2_decode->formats[1].fourcc == VA_FOURCC_P012 &&
                   !vp9_profile2_decode->formats[1].advertise,
               "VP9 Profile2 should advertise P010 while keeping P012 hidden") && ok;

    const VkvvProfileCapability *vp9_profile2_record = vkvv_profile_capability_record(
        &drv, VAProfileVP9Profile2, VAEntrypointVLD, VKVV_CODEC_DIRECTION_DECODE);
    ok = check(vp9_profile2_record == vp9_profile2_decode,
               "VP9 Profile2 advertised capability should match its full record") && ok;

    const VkvvProfileCapability *hevc_hidden = vkvv_profile_capability_record(
        &drv, VAProfileHEVCMain, VAEntrypointVLD, VKVV_CODEC_DIRECTION_DECODE);
    ok = check(hevc_hidden != nullptr &&
                   hevc_hidden->hardware_supported &&
                   !hevc_hidden->runtime_wired &&
                   !hevc_hidden->advertise,
               "HEVC hardware capability should remain hidden until decode is wired") && ok;

    const VkvvProfileCapability *av1_hidden = vkvv_profile_capability_record(
        &drv, VAProfileAV1Profile0, VAEntrypointVLD, VKVV_CODEC_DIRECTION_DECODE);
    ok = check(av1_hidden != nullptr &&
                   av1_hidden->format_count == 2 &&
                   vkvv_profile_format_variant(av1_hidden, VA_RT_FORMAT_YUV420, false) != nullptr &&
                   vkvv_profile_format_variant(av1_hidden, VA_RT_FORMAT_YUV420_10, false) != nullptr &&
                   !av1_hidden->advertise,
               "AV1 Profile0 should carry hidden NV12 and P010 variants") && ok;

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
