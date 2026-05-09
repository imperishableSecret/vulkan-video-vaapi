#include "va/private.h"
#include "codecs/av1/av1.h"
#include "codecs/hevc/hevc.h"
#include "codecs/vp9/vp9.h"

#include <cstdio>
#include <cstring>
#include <va/va_dec_av1.h>
#include <va/va_dec_hevc.h>
#include <vector>
#include <vulkan/vulkan.h>

namespace {

    bool check(bool condition, const char* message) {
        if (!condition) {
            std::fprintf(stderr, "%s\n", message);
        }
        return condition;
    }

    bool ops_complete(const VkvvDecodeOps* ops) {
        return ops != nullptr && ops->name != nullptr && ops->state_create != nullptr && ops->state_destroy != nullptr && ops->session_create != nullptr &&
            ops->session_destroy != nullptr && ops->begin_picture != nullptr && ops->render_buffer != nullptr && ops->prepare_decode != nullptr && ops->ensure_session != nullptr &&
            ops->configure_session != nullptr && ops->decode != nullptr;
    }

    std::vector<uint8_t> make_vp9_keyframe() {
        return {
            0x82, 0x49, 0x83, 0x42, 0x00, 0x00, 0xf0, 0x00, 0xf6, 0x06, 0x38, 0x24, 0x1c, 0x18, 0x42, 0x00, 0x00, 0x20, 0x40, 0x00,
            0x22, 0x9b, 0xff, 0xff, 0xa5, 0x13, 0xfb, 0x82, 0x94, 0xde, 0x8f, 0x15, 0x4a, 0xb7, 0x6d, 0x27, 0xfd, 0x54, 0xfd, 0x19,
            0xed, 0x94, 0x52, 0xd3, 0x71, 0xd2, 0x6c, 0xc8, 0xfe, 0xa6, 0xf7, 0x65, 0x4c, 0xbc, 0x26, 0x0c, 0x80, 0x00,
        };
    }

    std::vector<uint8_t> make_vp9_profile2_keyframe() {
        return {
            0x92, 0x49, 0x83, 0x42, 0x00, 0x00, 0x78, 0x00, 0x7b, 0x03, 0x1c, 0x12, 0x0e, 0x0c, 0x29,
            0x00, 0x00, 0x10, 0x40, 0x00, 0x10, 0xbf, 0xff, 0xfd, 0x25, 0x10, 0xdf, 0x98, 0x00,
        };
    }

    std::vector<uint8_t> make_av1_keyframe() {
        return {
            0x12, 0x00, 0x0a, 0x0a, 0x00, 0x00, 0x00, 0x01, 0x9f, 0xf9, 0xb5, 0xf2, 0x00, 0x80, 0x32, 0x80, 0x01, 0x10, 0x00, 0xc0, 0x00, 0x10, 0x40, 0x00, 0x28,
            0xba, 0xe8, 0xc9, 0x67, 0x40, 0xbb, 0x06, 0x62, 0x12, 0x4e, 0xf8, 0x58, 0x4b, 0xa5, 0xfc, 0xee, 0xc6, 0x54, 0x25, 0x89, 0x00, 0xbf, 0x94, 0x0b, 0x7d,
            0xb1, 0xb7, 0x1a, 0x34, 0x30, 0x25, 0xac, 0xe5, 0x7b, 0xd8, 0x28, 0x5a, 0x8a, 0x10, 0x49, 0xdd, 0x3f, 0x74, 0x9b, 0x64, 0xf1, 0xeb, 0x11, 0x7f, 0x68,
            0xa2, 0x7b, 0xff, 0x9f, 0x51, 0xc8, 0x62, 0x7d, 0x9f, 0xe3, 0x03, 0xed, 0x4a, 0x75, 0xcd, 0xe6, 0x5d, 0x15, 0xab, 0xab, 0x91, 0xba, 0x9a, 0x13, 0xf3,
            0x7c, 0x88, 0xbf, 0xf1, 0x80, 0xb2, 0x31, 0x9e, 0x20, 0xbe, 0x85, 0xc5, 0xd7, 0xdc, 0x95, 0x4d, 0xaf, 0xbf, 0x3b, 0x9c, 0x53, 0xfb, 0x7b, 0xf7, 0x87,
            0xbc, 0x7c, 0xc8, 0xee, 0x69, 0xd8, 0x40, 0x0e, 0x61, 0xe2, 0xa0, 0x11, 0xd7, 0xcf, 0x94, 0xa8, 0x33, 0x86, 0xf8, 0x8d,
        };
    }

    bool check_vp9_parser(const VkvvDecodeOps* vp9, uint8_t profile, uint8_t bit_depth, const std::vector<uint8_t>& bitstream, const char* label) {
        void* state = vp9 != nullptr ? vp9->state_create() : nullptr;
        if (!check(state != nullptr, "VP9 codec state allocation failed")) {
            return false;
        }

        vp9->begin_picture(state);

        VADecPictureParameterBufferVP9 pic{};
        pic.frame_width                          = 16;
        pic.frame_height                         = 16;
        pic.pic_fields.bits.subsampling_x        = 1;
        pic.pic_fields.bits.subsampling_y        = 1;
        pic.pic_fields.bits.error_resilient_mode = 1;
        pic.profile                              = profile;
        pic.bit_depth                            = bit_depth;

        VASliceParameterBufferVP9 slice{};
        slice.slice_data_size = static_cast<uint32_t>(bitstream.size());

        VkvvBuffer pic_buffer{};
        pic_buffer.type         = VAPictureParameterBufferType;
        pic_buffer.size         = sizeof(pic);
        pic_buffer.num_elements = 1;
        pic_buffer.data         = &pic;

        VkvvBuffer slice_buffer{};
        slice_buffer.type         = VASliceParameterBufferType;
        slice_buffer.size         = sizeof(slice);
        slice_buffer.num_elements = 1;
        slice_buffer.data         = &slice;

        VkvvBuffer data_buffer{};
        data_buffer.type         = VASliceDataBufferType;
        data_buffer.size         = static_cast<unsigned int>(bitstream.size());
        data_buffer.num_elements = 1;
        data_buffer.data         = const_cast<uint8_t*>(bitstream.data());

        bool ok = check(vp9->render_buffer(state, &pic_buffer) == VA_STATUS_SUCCESS, "VP9 picture buffer ingestion failed");
        ok      = check(vp9->render_buffer(state, &slice_buffer) == VA_STATUS_SUCCESS, "VP9 slice buffer ingestion failed") && ok;
        ok      = check(vp9->render_buffer(state, &data_buffer) == VA_STATUS_SUCCESS, "VP9 slice data ingestion failed") && ok;

        unsigned int width       = 0;
        unsigned int height      = 0;
        char         reason[512] = {};
        ok                       = check(vp9->prepare_decode(state, &width, &height, reason, sizeof(reason)) == VA_STATUS_SUCCESS, "VP9 prepare_decode failed") && ok;
        if (!ok) {
            std::fprintf(stderr, "%s\n", reason);
        }
        ok = check(width == 16 && height == 16, "VP9 prepare_decode returned the wrong dimensions") && ok;

        VkvvVP9DecodeInput input{};
        ok                = check(vkvv_vp9_get_decode_input(state, &input) == VA_STATUS_SUCCESS, "VP9 decode input extraction failed") && ok;
        const bool parsed = input.header.valid && input.header.refresh_frame_flags == 0xff && input.header.frame_header_length_in_bytes > 0 &&
            input.header.first_partition_size > 0 && input.bitstream_size == bitstream.size();
        if (!parsed) {
            std::fprintf(stderr, "%s parsed header mismatch: valid=%u refresh=0x%02x profile=%u depth=%u q=%u header=%u first_partition=%u bytes=%zu\n", label, input.header.valid,
                         input.header.refresh_frame_flags, input.header.profile, input.header.bit_depth, input.header.base_q_idx, input.header.frame_header_length_in_bytes,
                         input.header.first_partition_size, input.bitstream_size);
        }
        ok = check(parsed, "VP9 parsed header did not expose expected keyframe values") && ok;
        ok = check(input.header.profile == profile && input.header.bit_depth == bit_depth, "VP9 parsed header returned the wrong profile/depth") && ok;

        vp9->state_destroy(state);
        return ok;
    }

    bool check_av1_parser(const VkvvDecodeOps* av1, uint8_t bit_depth_idx, uint8_t expected_bit_depth, unsigned int expected_fourcc, const char* label) {
        void* state = av1 != nullptr ? av1->state_create() : nullptr;
        if (!check(state != nullptr, "AV1 codec state allocation failed")) {
            return false;
        }

        av1->begin_picture(state);

        std::vector<uint8_t>           bitstream = make_av1_keyframe();

        VADecPictureParameterBufferAV1 pic{};
        pic.profile                                        = 0;
        pic.order_hint_bits_minus_1                        = 6;
        pic.bit_depth_idx                                  = bit_depth_idx;
        pic.matrix_coefficients                            = 1;
        pic.seq_info_fields.fields.enable_order_hint       = 1;
        pic.seq_info_fields.fields.enable_cdef             = 1;
        pic.seq_info_fields.fields.subsampling_x           = 1;
        pic.seq_info_fields.fields.subsampling_y           = 1;
        pic.current_frame                                  = 7;
        pic.frame_width_minus1                             = 15;
        pic.frame_height_minus1                            = 15;
        pic.ref_frame_map[0]                               = VA_INVALID_ID;
        pic.primary_ref_frame                              = STD_VIDEO_AV1_PRIMARY_REF_NONE;
        pic.tile_cols                                      = 1;
        pic.tile_rows                                      = 1;
        pic.context_update_tile_id                         = 0;
        pic.pic_info_fields.bits.frame_type                = STD_VIDEO_AV1_FRAME_TYPE_KEY;
        pic.pic_info_fields.bits.show_frame                = 1;
        pic.pic_info_fields.bits.showable_frame            = 1;
        pic.pic_info_fields.bits.error_resilient_mode      = 1;
        pic.pic_info_fields.bits.disable_cdf_update        = 1;
        pic.pic_info_fields.bits.uniform_tile_spacing_flag = 1;
        pic.superres_scale_denominator                     = 8;
        pic.interp_filter                                  = STD_VIDEO_AV1_INTERPOLATION_FILTER_SWITCHABLE;
        pic.base_qindex                                    = 32;

        VASliceParameterBufferAV1 tile{};
        tile.slice_data_size = static_cast<uint32_t>(bitstream.size());
        tile.tile_row        = 0;
        tile.tile_column     = 0;

        VkvvBuffer pic_buffer{};
        pic_buffer.type         = VAPictureParameterBufferType;
        pic_buffer.size         = sizeof(pic);
        pic_buffer.num_elements = 1;
        pic_buffer.data         = &pic;

        VkvvBuffer data_buffer{};
        data_buffer.type         = VASliceDataBufferType;
        data_buffer.size         = static_cast<unsigned int>(bitstream.size());
        data_buffer.num_elements = 1;
        data_buffer.data         = bitstream.data();

        VkvvBuffer tile_buffer{};
        tile_buffer.type         = VASliceParameterBufferType;
        tile_buffer.size         = sizeof(tile);
        tile_buffer.num_elements = 1;
        tile_buffer.data         = &tile;

        bool ok = check(av1->render_buffer(state, &pic_buffer) == VA_STATUS_SUCCESS, "AV1 picture buffer ingestion failed");
        ok      = check(av1->render_buffer(state, &data_buffer) == VA_STATUS_SUCCESS, "AV1 slice data ingestion failed") && ok;
        ok      = check(av1->render_buffer(state, &tile_buffer) == VA_STATUS_SUCCESS, "AV1 tile parameter ingestion failed") && ok;

        unsigned int width       = 0;
        unsigned int height      = 0;
        char         reason[512] = {};
        ok                       = check(av1->prepare_decode(state, &width, &height, reason, sizeof(reason)) == VA_STATUS_SUCCESS, "AV1 prepare_decode failed") && ok;
        if (!ok) {
            std::fprintf(stderr, "%s\n", reason);
        }
        ok = check(width == 16 && height == 16, "AV1 prepare_decode returned the wrong dimensions") && ok;

        VkvvAV1DecodeInput input{};
        ok                = check(vkvv_av1_get_decode_input(state, &input) == VA_STATUS_SUCCESS, "AV1 decode input extraction failed") && ok;
        const bool parsed = input.header.valid && input.header.frame_type == STD_VIDEO_AV1_FRAME_TYPE_KEY && input.header.refresh_frame_flags == 0xff && input.tile_count == 1 &&
            input.tiles[0].offset == 0 && input.tiles[0].size == bitstream.size() && input.bitstream_size == bitstream.size() && input.bit_depth == expected_bit_depth &&
            input.fourcc == expected_fourcc;
        if (!parsed) {
            std::fprintf(stderr, "%s parsed header mismatch: valid=%u frame=%u refresh=0x%02x tiles=%zu off0=%u size0=%u bytes=%zu depth=%u fourcc=0x%x\n", label,
                         input.header.valid, input.header.frame_type, input.header.refresh_frame_flags, input.tile_count, input.tile_count > 0 ? input.tiles[0].offset : 0,
                         input.tile_count > 0 ? input.tiles[0].size : 0, input.bitstream_size, input.bit_depth, input.fourcc);
        }
        ok = check(parsed, "AV1 parser did not expose expected keyframe values") && ok;

        av1->begin_picture(state);
        ok        = check(av1->render_buffer(state, &pic_buffer) == VA_STATUS_SUCCESS, "AV1 second picture buffer ingestion failed") && ok;
        ok        = check(av1->render_buffer(state, &data_buffer) == VA_STATUS_SUCCESS, "AV1 second slice data ingestion failed") && ok;
        ok        = check(av1->render_buffer(state, &tile_buffer) == VA_STATUS_SUCCESS, "AV1 second tile parameter ingestion failed") && ok;
        width     = 0;
        height    = 0;
        reason[0] = '\0';
        ok        = check(av1->prepare_decode(state, &width, &height, reason, sizeof(reason)) == VA_STATUS_SUCCESS, "AV1 second prepare_decode failed") && ok;
        if (!ok) {
            std::fprintf(stderr, "%s\n", reason);
        }
        VkvvAV1DecodeInput second_input{};
        ok = check(vkvv_av1_get_decode_input(state, &second_input) == VA_STATUS_SUCCESS, "AV1 second decode input extraction failed") && ok;
        ok = check(second_input.header.valid && second_input.header.refresh_frame_flags == 0xff && second_input.tile_count == 1 &&
                       second_input.bitstream_size == bitstream.size() && second_input.bit_depth == expected_bit_depth && second_input.fourcc == expected_fourcc,
                   "AV1 parser failed to reuse state for a second picture") &&
            ok;

        std::vector<uint8_t> multi_frame;
        multi_frame.reserve(bitstream.size() * 3);
        multi_frame.insert(multi_frame.end(), bitstream.begin(), bitstream.end());
        multi_frame.insert(multi_frame.end(), bitstream.begin(), bitstream.end());
        multi_frame.insert(multi_frame.end(), bitstream.begin(), bitstream.end());
        const uint32_t target_tile_offset = static_cast<uint32_t>(bitstream.size() + (bitstream.size() / 2));
        tile.slice_data_offset            = target_tile_offset;
        tile.slice_data_size              = 16;
        data_buffer.size                  = static_cast<unsigned int>(multi_frame.size());
        data_buffer.data                  = multi_frame.data();

        av1->begin_picture(state);
        ok        = check(av1->render_buffer(state, &pic_buffer) == VA_STATUS_SUCCESS, "AV1 multi-frame picture buffer ingestion failed") && ok;
        ok        = check(av1->render_buffer(state, &data_buffer) == VA_STATUS_SUCCESS, "AV1 multi-frame slice data ingestion failed") && ok;
        ok        = check(av1->render_buffer(state, &tile_buffer) == VA_STATUS_SUCCESS, "AV1 multi-frame tile parameter ingestion failed") && ok;
        width     = 0;
        height    = 0;
        reason[0] = '\0';
        ok        = check(av1->prepare_decode(state, &width, &height, reason, sizeof(reason)) == VA_STATUS_SUCCESS, "AV1 multi-frame prepare_decode failed") && ok;
        if (!ok) {
            std::fprintf(stderr, "%s\n", reason);
        }
        VkvvAV1DecodeInput multi_input{};
        ok = check(vkvv_av1_get_decode_input(state, &multi_input) == VA_STATUS_SUCCESS, "AV1 multi-frame decode input extraction failed") && ok;
        ok = check(multi_input.header.frame_header_offset == 0 && multi_input.tiles[0].offset < multi_input.bitstream_size &&
                       multi_input.tiles[0].size <= multi_input.bitstream_size - multi_input.tiles[0].offset && multi_input.bitstream_size < multi_frame.size() &&
                       multi_input.bit_depth == expected_bit_depth && multi_input.fourcc == expected_fourcc,
                   "AV1 parser did not normalize the selected frame window for a packed buffer") &&
            ok;

        av1->state_destroy(state);
        return ok;
    }

    bool check_hevc_parser(const VkvvDecodeOps* hevc, uint8_t bit_depth_minus8, const char* label) {
        void* state = hevc != nullptr ? hevc->state_create() : nullptr;
        if (!check(state != nullptr, "HEVC codec state allocation failed")) {
            return false;
        }

        hevc->begin_picture(state);

        VAPictureParameterBufferHEVC pic{};
        pic.CurrPic.picture_id                       = 7;
        pic.pic_width_in_luma_samples                = 128;
        pic.pic_height_in_luma_samples               = 72;
        pic.bit_depth_luma_minus8                    = bit_depth_minus8;
        pic.bit_depth_chroma_minus8                  = bit_depth_minus8;
        pic.pic_fields.bits.chroma_format_idc        = 1;
        pic.slice_parsing_fields.bits.RapPicFlag     = 1;
        pic.slice_parsing_fields.bits.IdrPicFlag     = 1;
        pic.slice_parsing_fields.bits.IntraPicFlag   = 1;
        pic.log2_min_luma_coding_block_size_minus3   = 0;
        pic.log2_diff_max_min_luma_coding_block_size = 3;
        pic.log2_min_transform_block_size_minus2     = 0;
        pic.log2_diff_max_min_transform_block_size   = 3;
        pic.log2_parallel_merge_level_minus2         = 0;
        for (VAPictureHEVC& ref : pic.ReferenceFrames) {
            ref.picture_id = VA_INVALID_ID;
            ref.flags      = VA_PICTURE_HEVC_INVALID;
        }

        VASliceParameterBufferHEVC first_slice{};
        first_slice.slice_data_offset                    = 3;
        first_slice.slice_data_size                      = 4;
        first_slice.LongSliceFlags.fields.LastSliceOfPic = 0;
        first_slice.LongSliceFlags.fields.slice_type     = STD_VIDEO_H265_SLICE_TYPE_I;

        VASliceParameterBufferHEVC second_slice{};
        second_slice.slice_data_offset                    = 10;
        second_slice.slice_data_size                      = 3;
        second_slice.LongSliceFlags.fields.LastSliceOfPic = 1;
        second_slice.LongSliceFlags.fields.slice_type     = STD_VIDEO_H265_SLICE_TYPE_I;

        VASliceParameterBufferHEVC slices[2] = {
            first_slice,
            second_slice,
        };

        std::vector<uint8_t> data = {
            0x00, 0x00, 0x01, 0x26, 0x01, 0xaa, 0xbb, 0x00, 0x00, 0x01, 0x02, 0x01, 0xcc,
        };

        VkvvBuffer pic_buffer{};
        pic_buffer.type         = VAPictureParameterBufferType;
        pic_buffer.size         = sizeof(pic);
        pic_buffer.num_elements = 1;
        pic_buffer.data         = &pic;

        VkvvBuffer slice_buffer{};
        slice_buffer.type         = VASliceParameterBufferType;
        slice_buffer.size         = sizeof(first_slice);
        slice_buffer.num_elements = 2;
        slice_buffer.data         = slices;

        VkvvBuffer data_buffer{};
        data_buffer.type         = VASliceDataBufferType;
        data_buffer.size         = static_cast<unsigned int>(data.size());
        data_buffer.num_elements = 1;
        data_buffer.data         = data.data();

        bool ok = check(hevc->render_buffer(state, &pic_buffer) == VA_STATUS_SUCCESS, "HEVC picture buffer ingestion failed");
        ok      = check(hevc->render_buffer(state, &slice_buffer) == VA_STATUS_SUCCESS, "HEVC slice buffer ingestion failed") && ok;
        ok      = check(hevc->render_buffer(state, &data_buffer) == VA_STATUS_SUCCESS, "HEVC slice data ingestion failed") && ok;

        unsigned int width       = 0;
        unsigned int height      = 0;
        char         reason[512] = {};
        ok                       = check(hevc->prepare_decode(state, &width, &height, reason, sizeof(reason)) == VA_STATUS_SUCCESS, "HEVC prepare_decode failed") && ok;
        if (!ok) {
            std::fprintf(stderr, "%s\n", reason);
        }
        ok = check(width == 128 && height == 72, "HEVC prepare_decode returned the wrong dimensions") && ok;

        VkvvHEVCDecodeInput input{};
        ok = check(vkvv_hevc_get_decode_input(state, &input) == VA_STATUS_SUCCESS, "HEVC decode input extraction failed") && ok;
        ok = check(input.pic != nullptr && input.pic->bit_depth_luma_minus8 == bit_depth_minus8 && input.pic->bit_depth_chroma_minus8 == bit_depth_minus8,
                   "HEVC parser did not preserve the expected bit depth") &&
            ok;
        ok = check(input.slice_count == 2, "HEVC parser returned the wrong slice count") && ok;
        ok = check(input.bitstream_size == 13 && input.slice_offsets[0] == 0 && input.slice_offsets[1] == 7, "HEVC parser returned wrong slice offsets or size") && ok;
        ok = check(input.bitstream[0] == 0x00 && input.bitstream[1] == 0x00 && input.bitstream[2] == 0x01 && input.bitstream[3] == 0x26,
                   "HEVC parser did not prefix the first slice with an Annex-B start code") &&
            ok;
        ok = check(input.bitstream[7] == 0x00 && input.bitstream[8] == 0x00 && input.bitstream[9] == 0x01 && input.bitstream[10] == 0x02,
                   "HEVC parser did not prefix the second slice with an Annex-B start code") &&
            ok;

        VASliceParameterBufferHEVC interleaved_second_slice = second_slice;
        interleaved_second_slice.slice_data_offset          = 2;
        interleaved_second_slice.slice_data_size            = 3;
        std::vector<uint8_t> first_slice_data               = {0x00, 0x00, 0x01, 0x26, 0x01, 0xaa, 0xbb};
        std::vector<uint8_t> second_slice_data              = {0x00, 0x00, 0x02, 0x01, 0xcc};

        VkvvBuffer           single_slice_buffer{};
        single_slice_buffer.type         = VASliceParameterBufferType;
        single_slice_buffer.size         = sizeof(first_slice);
        single_slice_buffer.num_elements = 1;
        single_slice_buffer.data         = &first_slice;

        VkvvBuffer single_data_buffer{};
        single_data_buffer.type         = VASliceDataBufferType;
        single_data_buffer.size         = static_cast<unsigned int>(first_slice_data.size());
        single_data_buffer.num_elements = 1;
        single_data_buffer.data         = first_slice_data.data();

        hevc->begin_picture(state);
        ok = check(hevc->render_buffer(state, &pic_buffer) == VA_STATUS_SUCCESS, "HEVC interleaved picture buffer ingestion failed") && ok;
        ok = check(hevc->render_buffer(state, &single_slice_buffer) == VA_STATUS_SUCCESS, "HEVC interleaved first slice parameter ingestion failed") && ok;
        ok = check(hevc->render_buffer(state, &single_data_buffer) == VA_STATUS_SUCCESS, "HEVC interleaved first slice data ingestion failed") && ok;

        single_slice_buffer.data = &interleaved_second_slice;
        single_data_buffer.size  = static_cast<unsigned int>(second_slice_data.size());
        single_data_buffer.data  = second_slice_data.data();
        ok                       = check(hevc->render_buffer(state, &single_slice_buffer) == VA_STATUS_SUCCESS, "HEVC interleaved second slice parameter ingestion failed") && ok;
        ok                       = check(hevc->render_buffer(state, &single_data_buffer) == VA_STATUS_SUCCESS, "HEVC interleaved second slice data ingestion failed") && ok;

        width     = 0;
        height    = 0;
        reason[0] = '\0';
        ok        = check(hevc->prepare_decode(state, &width, &height, reason, sizeof(reason)) == VA_STATUS_SUCCESS, "HEVC interleaved prepare_decode failed") && ok;
        if (!ok) {
            std::fprintf(stderr, "%s\n", reason);
        }
        VkvvHEVCDecodeInput interleaved_input{};
        ok = check(vkvv_hevc_get_decode_input(state, &interleaved_input) == VA_STATUS_SUCCESS, "HEVC interleaved decode input extraction failed") && ok;
        ok = check(interleaved_input.slice_count == 2 && interleaved_input.bitstream_size == 13 && interleaved_input.slice_offsets[0] == 0 &&
                       interleaved_input.slice_offsets[1] == 7 && interleaved_input.bitstream[3] == 0x26 && interleaved_input.bitstream[10] == 0x02,
                   "HEVC parser failed interleaved slice parameter/data ingestion") &&
            ok;

        hevc->state_destroy(state);
        std::printf("%s parser passed\n", label);
        return ok;
    }

} // namespace

int main(void) {
    bool                 ok = true;

    const VkvvDecodeOps* h264 = vkvv_decode_ops_for_profile_entrypoint(VAProfileH264High, VAEntrypointVLD);
    ok                        = check(ops_complete(h264), "H.264 decode ops are incomplete") && ok;
    if (h264 != nullptr) {
        ok = check(std::strcmp(h264->name, "h264") == 0, "H.264 decode ops used the wrong name") && ok;
    }

    void* state = h264 != nullptr ? h264->state_create() : nullptr;
    ok          = check(state != nullptr, "H.264 codec state allocation failed") && ok;
    if (state != nullptr) {
        h264->begin_picture(state);
        h264->state_destroy(state);
    }

    void* session = h264 != nullptr ? h264->session_create(nullptr) : nullptr;
    ok            = check(session != nullptr, "H.264 codec session allocation failed") && ok;
    if (session != nullptr) {
        h264->session_destroy(nullptr, session);
    }

    ok = check(vkvv_decode_ops_for_profile_entrypoint(VAProfileH264High, VAEntrypointEncSlice) == nullptr, "H.264 encode entrypoint must not resolve decode ops") && ok;
    const VkvvDecodeOps* hevc_main = vkvv_decode_ops_for_profile_entrypoint(VAProfileHEVCMain, VAEntrypointVLD);
    ok                             = check(ops_complete(hevc_main), "HEVC Main decode ops are incomplete") && ok;
    if (hevc_main != nullptr) {
        ok = check(std::strcmp(hevc_main->name, "hevc-main") == 0, "HEVC Main decode ops used the wrong name") && ok;
        ok = check_hevc_parser(hevc_main, 0, "HEVC Main") && ok;
    }
    const VkvvDecodeOps* hevc_main10 = vkvv_decode_ops_for_profile_entrypoint(VAProfileHEVCMain10, VAEntrypointVLD);
    ok                               = check(ops_complete(hevc_main10), "HEVC Main10 decode ops are incomplete") && ok;
    if (hevc_main10 != nullptr) {
        ok = check(std::strcmp(hevc_main10->name, "hevc-main10") == 0, "HEVC Main10 decode ops used the wrong name") && ok;
        ok = check_hevc_parser(hevc_main10, 2, "HEVC Main10") && ok;
    }
    const VkvvDecodeOps* vp9 = vkvv_decode_ops_for_profile_entrypoint(VAProfileVP9Profile0, VAEntrypointVLD);
    ok                       = check(ops_complete(vp9), "VP9 decode ops are incomplete") && ok;
    if (vp9 != nullptr) {
        ok = check(std::strcmp(vp9->name, "vp9") == 0, "VP9 decode ops used the wrong name") && ok;
        ok = check_vp9_parser(vp9, 0, 8, make_vp9_keyframe(), "VP9 Profile0") && ok;
    }
    const VkvvDecodeOps* vp9_profile2 = vkvv_decode_ops_for_profile_entrypoint(VAProfileVP9Profile2, VAEntrypointVLD);
    ok                                = check(ops_complete(vp9_profile2), "VP9 Profile2 decode ops are incomplete") && ok;
    if (vp9_profile2 != nullptr) {
        ok = check(std::strcmp(vp9_profile2->name, "vp9-profile2") == 0, "VP9 Profile2 decode ops used the wrong name") && ok;
        ok = check_vp9_parser(vp9_profile2, 2, 10, make_vp9_profile2_keyframe(), "VP9 Profile2") && ok;
    }
    const VkvvDecodeOps* av1 = vkvv_decode_ops_for_profile_entrypoint(VAProfileAV1Profile0, VAEntrypointVLD);
    ok                       = check(ops_complete(av1), "AV1 decode ops are incomplete") && ok;
    if (av1 != nullptr) {
        ok = check(std::strcmp(av1->name, "av1") == 0, "AV1 decode ops used the wrong name") && ok;
        ok = check_av1_parser(av1, 0, 8, VA_FOURCC_NV12, "AV1 NV12") && ok;
        ok = check_av1_parser(av1, 1, 10, VA_FOURCC_P010, "AV1 P010") && ok;
    }
    ok =
        check(vkvv_encode_ops_for_profile_entrypoint(VAProfileH264High, VAEntrypointEncSlice) == nullptr, "H.264 EncSlice should not have encode ops before encode is wired") && ok;
    ok =
        check(vkvv_encode_ops_for_profile_entrypoint(VAProfileH264High, VAEntrypointEncSliceLP) == nullptr, "H.264 EncSliceLP should not have encode ops before encode is wired") &&
        ok;
    ok =
        check(vkvv_encode_ops_for_profile_entrypoint(VAProfileH264High, VAEntrypointEncPicture) == nullptr, "H.264 EncPicture should not have encode ops before encode is wired") &&
        ok;

    VkvvDriver drv{};
    drv.caps.h264                = true;
    drv.caps.h265                = true;
    drv.caps.h265_10             = true;
    drv.caps.h265_12             = true;
    drv.caps.vp9                 = true;
    drv.caps.vp9_10              = true;
    drv.caps.vp9_12              = true;
    drv.caps.av1                 = true;
    drv.caps.av1_10              = true;
    drv.caps.h264_encode         = true;
    drv.caps.surface_export      = true;
    drv.caps.surface_export_nv12 = true;
    drv.caps.surface_export_p010 = true;
    vkvv_init_profile_capabilities(&drv);

    const VkvvProfileCapability* h264_decode = vkvv_profile_capability_for_entrypoint(&drv, VAProfileH264High, VAEntrypointVLD);
    ok = check(h264_decode != nullptr && h264_decode->advertise && h264_decode->direction == VKVV_CODEC_DIRECTION_DECODE, "H.264 VLD should be an advertised decode capability") &&
        ok;
    ok = check(h264_decode != nullptr && h264_decode->format_count == 1 && h264_decode->formats[0].fourcc == VA_FOURCC_NV12,
               "H.264 decode capability should expose one NV12 format variant") &&
        ok;

    const VkvvProfileCapability* vp9_decode = vkvv_profile_capability_for_entrypoint(&drv, VAProfileVP9Profile0, VAEntrypointVLD);
    ok = check(vp9_decode != nullptr && vp9_decode->advertise && vp9_decode->direction == VKVV_CODEC_DIRECTION_DECODE && vp9_decode->format_count == 1 &&
                   vp9_decode->formats[0].fourcc == VA_FOURCC_NV12,
               "VP9 Profile0 should be advertised with one NV12 format variant") &&
        ok;

    const VkvvProfileCapability* vp9_profile2_decode = vkvv_profile_capability_for_entrypoint(&drv, VAProfileVP9Profile2, VAEntrypointVLD);
    ok = check(vp9_profile2_decode != nullptr && vp9_profile2_decode->advertise && vp9_profile2_decode->direction == VKVV_CODEC_DIRECTION_DECODE &&
                   vp9_profile2_decode->format_count == 2 && vp9_profile2_decode->formats[0].fourcc == VA_FOURCC_P010 && vp9_profile2_decode->formats[0].advertise &&
                   vp9_profile2_decode->formats[1].fourcc == VA_FOURCC_P012 && !vp9_profile2_decode->formats[1].advertise,
               "VP9 Profile2 should advertise P010 while keeping P012 hidden") &&
        ok;

    const VkvvProfileCapability* vp9_profile2_record = vkvv_profile_capability_record(&drv, VAProfileVP9Profile2, VAEntrypointVLD, VKVV_CODEC_DIRECTION_DECODE);
    ok                                               = check(vp9_profile2_record == vp9_profile2_decode, "VP9 Profile2 advertised capability should match its full record") && ok;

    const VkvvProfileCapability* hevc_main_cap = vkvv_profile_capability_record(&drv, VAProfileHEVCMain, VAEntrypointVLD, VKVV_CODEC_DIRECTION_DECODE);
    ok = check(hevc_main_cap != nullptr && hevc_main_cap->hardware_supported && hevc_main_cap->runtime_wired && hevc_main_cap->parser_wired && hevc_main_cap->advertise &&
                   hevc_main_cap->formats[0].fourcc == VA_FOURCC_NV12,
               "HEVC Main capability should advertise NV12 after decode wiring lands") &&
        ok;

    const VkvvProfileCapability* hevc_main10_cap = vkvv_profile_capability_record(&drv, VAProfileHEVCMain10, VAEntrypointVLD, VKVV_CODEC_DIRECTION_DECODE);
    ok = check(hevc_main10_cap != nullptr && hevc_main10_cap->hardware_supported && hevc_main10_cap->runtime_wired && hevc_main10_cap->parser_wired && hevc_main10_cap->advertise &&
                   hevc_main10_cap->formats[0].fourcc == VA_FOURCC_P010,
               "HEVC Main10 capability should advertise P010 after decode wiring lands") &&
        ok;
    VAConfigAttrib hevc_main10_rt{};
    hevc_main10_rt.type = VAConfigAttribRTFormat;
    vkvv_fill_config_attribute(hevc_main10_cap, &hevc_main10_rt);
    ok = check((hevc_main10_rt.value & VA_RT_FORMAT_YUV420_10) != 0 && (hevc_main10_rt.value & VA_RT_FORMAT_YUV420) != 0,
               "HEVC Main10 config RTFormat should include P010 plus the YUV420 config alias") &&
        ok;
    ok = check(vkvv_select_rt_format(hevc_main10_cap, VA_RT_FORMAT_YUV420) == VA_RT_FORMAT_YUV420_10, "HEVC Main10 YUV420 config request should resolve to P010") && ok;
    ok = check(vkvv_profile_format_variant(hevc_main10_cap, VA_RT_FORMAT_YUV420, true) == nullptr, "HEVC Main10 should not expose an actual NV12 surface variant") && ok;

    const VkvvProfileCapability* av1_decode = vkvv_profile_capability_record(&drv, VAProfileAV1Profile0, VAEntrypointVLD, VKVV_CODEC_DIRECTION_DECODE);
    ok =
        check(av1_decode != nullptr && av1_decode->format_count == 2 && av1_decode->advertise && av1_decode->rt_format == (VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_YUV420_10) &&
                  vkvv_profile_format_variant(av1_decode, VA_RT_FORMAT_YUV420, true) != nullptr && vkvv_profile_format_variant(av1_decode, VA_RT_FORMAT_YUV420_10, true) != nullptr,
              "AV1 Profile0 should advertise NV12 and P010 when both paths are wired") &&
        ok;

    VAConfigAttrib av1_features{};
    av1_features.type = VAConfigAttribDecAV1Features;
    vkvv_fill_config_attribute(av1_decode, &av1_features);
    VAConfigAttribValDecAV1Features av1_feature_value{};
    av1_feature_value.value = av1_features.value;
    ok =
        check(av1_features.value != VA_ATTRIB_NOT_SUPPORTED && av1_feature_value.bits.lst_support == 0, "AV1 feature attribute should be present with large-scale tile disabled") &&
        ok;

    const VkvvProfileCapability* h264_encode = vkvv_profile_capability_record(&drv, VAProfileH264High, VAEntrypointEncSlice, VKVV_CODEC_DIRECTION_ENCODE);
    ok = check(h264_encode != nullptr && h264_encode->hardware_supported && !h264_encode->parser_wired && !h264_encode->runtime_wired && !h264_encode->surface_wired &&
                   !h264_encode->advertise,
               "H.264 encode descriptor should be present but inert") &&
        ok;
    ok = check(h264_encode != nullptr && h264_encode->vulkan_codec_operation == VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR,
               "H.264 encode descriptor should carry the Vulkan H.264 encode operation") &&
        ok;
    ok = check(h264_encode != nullptr && std::strcmp(h264_encode->required_extension, VK_KHR_VIDEO_ENCODE_H264_EXTENSION_NAME) == 0,
               "H.264 encode descriptor should carry the Vulkan H.264 encode extension name") &&
        ok;
    ok = check(vkvv_profile_capability_for_entrypoint(&drv, VAProfileH264High, VAEntrypointEncSlice) == nullptr, "H.264 encode entrypoint must not be advertised") && ok;

    if (!ok) {
        return 1;
    }

    std::printf("codec ops smoke passed\n");
    return 0;
}
