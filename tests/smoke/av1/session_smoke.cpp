#include "vulkan/codecs/av1/api.h"
#include "vulkan/codecs/av1/internal.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace {

    bool check(bool condition, const char* message) {
        if (!condition) {
            std::fprintf(stderr, "%s\n", message);
        }
        return condition;
    }

    bool ensure_session(void* runtime, void* session, unsigned int width, unsigned int height, unsigned int min_expected_width, unsigned int min_expected_height,
                        VkFormat expected_format, VkVideoComponentBitDepthFlagBitsKHR expected_depth, const char* label) {
        char     reason[512] = {};
        VAStatus status      = vkvv_vulkan_ensure_av1_session(runtime, session, width, height, reason, sizeof(reason));
        std::printf("%s\n", reason);
        if (!check(status == VA_STATUS_SUCCESS, "vkvv_vulkan_ensure_av1_session failed")) {
            return false;
        }

        const auto*               typed_session = static_cast<const vkvv::AV1VideoSession*>(session);
        const vkvv::VideoSession& video         = typed_session->video;
        if (!check(video.session != VK_NULL_HANDLE, "AV1 session handle was not created")) {
            return false;
        }
        if (!check(video.key.codec_operation == VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR, "AV1 session key did not record the codec operation")) {
            return false;
        }
        if (!check(video.key.codec_profile == STD_VIDEO_AV1_PROFILE_MAIN, "AV1 session key did not record the expected AV1 profile")) {
            return false;
        }
        if (!check(video.key.picture_format == expected_format && video.key.reference_picture_format == video.key.picture_format,
                   "AV1 session key did not record expected picture/reference formats")) {
            return false;
        }
        if (!check(video.key.luma_bit_depth == expected_depth && video.key.chroma_bit_depth == expected_depth, "AV1 session key did not record expected bit depth")) {
            return false;
        }
        if (!check(video.key.max_coded_extent.width >= min_expected_width && video.key.max_coded_extent.height >= min_expected_height,
                   "AV1 session extent is smaller than the requested stream")) {
            std::fprintf(stderr, "expected at least=%ux%u actual=%ux%u\n", min_expected_width, min_expected_height, video.key.max_coded_extent.width,
                         video.key.max_coded_extent.height);
            return false;
        }
        std::printf("%s profile=%u format=%d depth=0x%x\n", label, video.key.codec_profile, video.key.picture_format, video.key.luma_bit_depth);
        return check(video.memory_bytes > 0, "AV1 session memory accounting stayed at zero");
    }

    bool ensure_upload(vkvv::VulkanRuntime* runtime, vkvv::AV1VideoSession* session, const std::vector<uint8_t>& bytes) {
        char reason[512] = {};
        if (!check(vkvv::ensure_bitstream_upload_buffer(runtime, session->profile_spec, bytes.data(), bytes.size(), session->bitstream_size_alignment,
                                                        VK_BUFFER_USAGE_VIDEO_DECODE_SRC_BIT_KHR, &session->uploads[0], "AV1 smoke bitstream", reason, sizeof(reason)),
                   "ensure_bitstream_upload_buffer failed")) {
            std::fprintf(stderr, "%s\n", reason);
            return false;
        }
        return check(session->uploads[0].buffer != VK_NULL_HANDLE && session->uploads[0].memory != VK_NULL_HANDLE && session->uploads[0].size >= bytes.size() &&
                         session->uploads[0].capacity >= session->uploads[0].size,
                     "AV1 upload buffer was not populated correctly");
    }

    VkvvAV1SequenceHeader make_av1_sequence(uint8_t bit_depth, uint16_t max_width_minus_1 = 63, uint16_t max_height_minus_1 = 63) {
        VkvvAV1SequenceHeader sequence{};
        sequence.valid                          = true;
        sequence.seq_profile                    = 0;
        sequence.bit_depth                      = bit_depth;
        sequence.frame_width_bits_minus_1       = 15;
        sequence.frame_height_bits_minus_1      = 15;
        sequence.max_frame_width_minus_1        = max_width_minus_1;
        sequence.max_frame_height_minus_1       = max_height_minus_1;
        sequence.use_128x128_superblock         = true;
        sequence.enable_filter_intra            = true;
        sequence.enable_intra_edge_filter       = true;
        sequence.enable_interintra_compound     = true;
        sequence.enable_masked_compound         = true;
        sequence.enable_order_hint              = true;
        sequence.enable_dual_filter             = true;
        sequence.enable_jnt_comp                = true;
        sequence.enable_cdef                    = true;
        sequence.order_hint_bits_minus_1        = 6;
        sequence.seq_force_integer_mv           = STD_VIDEO_AV1_SELECT_INTEGER_MV;
        sequence.seq_force_screen_content_tools = STD_VIDEO_AV1_SELECT_SCREEN_CONTENT_TOOLS;
        sequence.subsampling_x                  = 1;
        sequence.subsampling_y                  = 1;
        sequence.color_primaries                = STD_VIDEO_AV1_COLOR_PRIMARIES_UNSPECIFIED;
        sequence.transfer_characteristics       = STD_VIDEO_AV1_TRANSFER_CHARACTERISTICS_UNSPECIFIED;
        sequence.matrix_coefficients            = STD_VIDEO_AV1_MATRIX_COEFFICIENTS_BT_709;
        sequence.chroma_sample_position         = STD_VIDEO_AV1_CHROMA_SAMPLE_POSITION_UNKNOWN;
        return sequence;
    }

    struct AV1StdInputFixture {
        VADecPictureParameterBufferAV1 pic{};
        VkvvAV1DecodeInput             input{};

        AV1StdInputFixture() {
            pic.profile                                             = 0;
            pic.frame_width_minus1                                  = 63;
            pic.frame_height_minus1                                 = 63;
            pic.tile_cols                                           = 1;
            pic.tile_rows                                           = 1;
            pic.primary_ref_frame                                   = STD_VIDEO_AV1_PRIMARY_REF_NONE;
            pic.pic_info_fields.bits.frame_type                     = STD_VIDEO_AV1_FRAME_TYPE_INTER;
            pic.pic_info_fields.bits.uniform_tile_spacing_flag      = 1;
            pic.seq_info_fields.fields.use_128x128_superblock       = 0;
            pic.loop_filter_info_fields.bits.mode_ref_delta_enabled = 1;
            for (VASurfaceID& surface : pic.ref_frame_map) {
                surface = VA_INVALID_ID;
            }

            input.pic                            = &pic;
            input.sequence                       = make_av1_sequence(8);
            input.bit_depth                      = 8;
            input.rt_format                      = VA_RT_FORMAT_YUV420;
            input.fourcc                         = VA_FOURCC_NV12;
            input.frame_width                    = 64;
            input.frame_height                   = 64;
            input.header.valid                   = true;
            input.header.frame_type              = STD_VIDEO_AV1_FRAME_TYPE_INTER;
            input.header.primary_ref_frame       = STD_VIDEO_AV1_PRIMARY_REF_NONE;
            input.header.tile_size_bytes_minus_1 = 3;
        }
    };

    bool check_loop_filter_ref_deltas(const StdVideoAV1LoopFilter& loop_filter, const int8_t expected[STD_VIDEO_AV1_TOTAL_REFS_PER_FRAME], const char* label) {
        for (uint32_t i = 0; i < STD_VIDEO_AV1_TOTAL_REFS_PER_FRAME; i++) {
            if (loop_filter.loop_filter_ref_deltas[i] != expected[i]) {
                std::fprintf(stderr, "%s ref_delta[%u] expected=%d actual=%d\n", label, i, expected[i], loop_filter.loop_filter_ref_deltas[i]);
                return false;
            }
        }
        return true;
    }

    bool check_loop_filter_mode_deltas(const StdVideoAV1LoopFilter& loop_filter, const int8_t expected[STD_VIDEO_AV1_LOOP_FILTER_ADJUSTMENTS], const char* label) {
        for (uint32_t i = 0; i < STD_VIDEO_AV1_LOOP_FILTER_ADJUSTMENTS; i++) {
            if (loop_filter.loop_filter_mode_deltas[i] != expected[i]) {
                std::fprintf(stderr, "%s mode_delta[%u] expected=%d actual=%d\n", label, i, expected[i], loop_filter.loop_filter_mode_deltas[i]);
                return false;
            }
        }
        return true;
    }

    bool configure_session_for_p010(void* runtime, void* session) {
        VADecPictureParameterBufferAV1 pic{};
        pic.profile = 0;

        VkvvAV1DecodeInput input{};
        input.pic       = &pic;
        input.sequence  = make_av1_sequence(10);
        input.bit_depth = 10;
        input.rt_format = VA_RT_FORMAT_YUV420_10;
        input.fourcc    = VA_FOURCC_P010;

        VkvvSurface target{};
        target.rt_format = VA_RT_FORMAT_YUV420_10;
        target.fourcc    = VA_FOURCC_P010;

        char     reason[512] = {};
        VAStatus status      = vkvv_vulkan_configure_av1_session(runtime, session, &target, &input, reason, sizeof(reason));
        std::printf("%s\n", reason);
        if (!check(status == VA_STATUS_SUCCESS, "AV1 P010 session retarget failed")) {
            return false;
        }

        const auto* typed_session = static_cast<const vkvv::AV1VideoSession*>(session);
        return check(typed_session->va_rt_format == VA_RT_FORMAT_YUV420_10 && typed_session->va_fourcc == VA_FOURCC_P010 && typed_session->bit_depth == 10,
                     "AV1 retarget did not switch the session metadata to P010") &&
            check(typed_session->video.session == VK_NULL_HANDLE && typed_session->uploads[0].buffer == VK_NULL_HANDLE, "AV1 retarget did not discard stale NV12 Vulkan resources");
    }

    bool check_av1_sequence_parameters() {
        VADecPictureParameterBufferAV1 pic{};
        pic.profile             = 0;
        pic.frame_width_minus1  = 15;
        pic.frame_height_minus1 = 15;

        VkvvAV1DecodeInput input{};
        input.pic       = &pic;
        input.sequence  = make_av1_sequence(8, 1919, 1079);
        input.bit_depth = 8;
        input.rt_format = VA_RT_FORMAT_YUV420;
        input.fourcc    = VA_FOURCC_NV12;

        input.sequence.frame_width_bits_minus_1           = 12;
        input.sequence.frame_height_bits_minus_1          = 13;
        input.sequence.enable_warped_motion               = true;
        input.sequence.enable_ref_frame_mvs               = true;
        input.sequence.enable_superres                    = true;
        input.sequence.enable_restoration                 = true;
        input.sequence.frame_id_numbers_present_flag      = true;
        input.sequence.delta_frame_id_length_minus_2      = 2;
        input.sequence.additional_frame_id_length_minus_1 = 3;
        input.sequence.color_description_present_flag     = true;
        input.sequence.color_range                        = true;
        input.sequence.separate_uv_delta_q                = true;
        input.sequence.color_primaries                    = STD_VIDEO_AV1_COLOR_PRIMARIES_BT_2020;
        input.sequence.transfer_characteristics           = STD_VIDEO_AV1_TRANSFER_CHARACTERISTICS_HLG;
        input.sequence.matrix_coefficients                = STD_VIDEO_AV1_MATRIX_COEFFICIENTS_BT_2020_NCL;
        input.sequence.chroma_sample_position             = STD_VIDEO_AV1_CHROMA_SAMPLE_POSITION_VERTICAL;
        input.sequence.timing_info_present_flag           = true;
        input.sequence.equal_picture_interval             = true;
        input.sequence.num_units_in_display_tick          = 1001;
        input.sequence.time_scale                         = 60000;
        input.sequence.num_ticks_per_picture_minus_1      = 1;

        vkvv::AV1SessionStdParameters params{};
        vkvv::build_av1_session_parameters(&input, &params);

        bool ok = true;
        ok      = check(params.sequence.flags.enable_warped_motion && params.sequence.flags.enable_ref_frame_mvs && params.sequence.flags.enable_superres &&
                            params.sequence.flags.enable_restoration,
                        "AV1 sequence tools were derived from current-frame flags instead of the sequence header") &&
            ok;
        ok = check(params.sequence.max_frame_width_minus_1 == 1919 && params.sequence.max_frame_height_minus_1 == 1079 && params.sequence.frame_width_bits_minus_1 == 12 &&
                       params.sequence.frame_height_bits_minus_1 == 13,
                   "AV1 sequence dimensions did not come from the sequence header") &&
            ok;
        ok = check(params.sequence.delta_frame_id_length_minus_2 == 2 && params.sequence.additional_frame_id_length_minus_1 == 3, "AV1 frame-id sequence fields were not copied") &&
            ok;
        ok = check(params.color.flags.color_description_present_flag && params.color.flags.color_range && params.color.flags.separate_uv_delta_q &&
                       params.color.color_primaries == STD_VIDEO_AV1_COLOR_PRIMARIES_BT_2020 &&
                       params.color.transfer_characteristics == STD_VIDEO_AV1_TRANSFER_CHARACTERISTICS_HLG &&
                       params.color.matrix_coefficients == STD_VIDEO_AV1_MATRIX_COEFFICIENTS_BT_2020_NCL &&
                       params.color.chroma_sample_position == STD_VIDEO_AV1_CHROMA_SAMPLE_POSITION_VERTICAL,
                   "AV1 color config did not come from the sequence header") &&
            ok;
        return check(params.sequence.pTimingInfo == &params.timing && params.timing.flags.equal_picture_interval && params.timing.num_units_in_display_tick == 1001 &&
                         params.timing.time_scale == 60000 && params.timing.num_ticks_per_picture_minus_1 == 1,
                     "AV1 timing info did not come from the sequence header") &&
            ok;
    }

    bool check_av1_sequence_key_reset() {
        vkvv::AV1VideoSession          session{};

        VADecPictureParameterBufferAV1 pic{};
        pic.profile = 0;

        VkvvSurface target{};
        target.rt_format = VA_RT_FORMAT_YUV420;
        target.fourcc    = VA_FOURCC_NV12;

        VkvvAV1DecodeInput input{};
        input.pic       = &pic;
        input.sequence  = make_av1_sequence(8, 63, 63);
        input.bit_depth = 8;
        input.rt_format = VA_RT_FORMAT_YUV420;
        input.fourcc    = VA_FOURCC_NV12;

        char     reason[512] = {};
        VAStatus status      = vkvv_vulkan_configure_av1_session(nullptr, &session, &target, &input, reason, sizeof(reason));
        if (!check(status == VA_STATUS_SUCCESS && session.has_sequence_key && session.sequence_key.max_frame_width_minus_1 == 63,
                   "AV1 sequence key was not recorded on first configure")) {
            std::fprintf(stderr, "%s\n", reason);
            return false;
        }

        StdVideoDecodeAV1ReferenceInfo info{};
        vkvv::av1_set_reference_slot(&session, 0, 101, 3, info);
        vkvv::av1_set_surface_slot(&session, 101, 3, info);
        session.next_dpb_slot = 4;
        session.max_dpb_slots = vkvv::max_av1_dpb_slots;

        input.sequence = make_av1_sequence(8, 1919, 1079);
        reason[0]      = '\0';
        status         = vkvv_vulkan_configure_av1_session(nullptr, &session, &target, &input, reason, sizeof(reason));
        if (!check(status == VA_STATUS_SUCCESS, "AV1 sequence change configure failed")) {
            std::fprintf(stderr, "%s\n", reason);
            return false;
        }

        return check(session.has_sequence_key && session.sequence_key.max_frame_width_minus_1 == 1919 && vkvv::av1_reference_slot_for_index(&session, 0) == nullptr &&
                         vkvv::av1_surface_slot_for_surface(&session, 101) == nullptr && session.max_dpb_slots == 0 && session.next_dpb_slot == 0,
                     "AV1 sequence change did not reset reference slots and DPB metadata");
    }

    bool check_av1_restoration_translation() {
        AV1StdInputFixture      fixture{};
        vkvv::AV1VideoSession   session{};
        vkvv::AV1PictureStdData std_data{};
        char                    reason[512] = {};

        fixture.pic.loop_restoration_fields.bits.yframe_restoration_type  = STD_VIDEO_AV1_FRAME_RESTORATION_TYPE_WIENER;
        fixture.pic.loop_restoration_fields.bits.cbframe_restoration_type = STD_VIDEO_AV1_FRAME_RESTORATION_TYPE_SGRPROJ;
        fixture.pic.loop_restoration_fields.bits.crframe_restoration_type = STD_VIDEO_AV1_FRAME_RESTORATION_TYPE_NONE;
        fixture.pic.loop_restoration_fields.bits.lr_unit_shift            = 0;
        fixture.pic.loop_restoration_fields.bits.lr_uv_shift              = 1;

        bool ok = check(vkvv::build_av1_picture_std_data(&session, &fixture.input, &std_data, reason, sizeof(reason)), "AV1 restoration std data build failed");
        if (!ok) {
            std::fprintf(stderr, "%s\n", reason);
            return false;
        }
        ok = check(std_data.restoration.LoopRestorationSize[0] == 1 && std_data.restoration.LoopRestorationSize[1] == 0 && std_data.restoration.LoopRestorationSize[2] == 0,
                   "AV1 loop restoration sizes were not translated to Vulkan size codes") &&
            ok;

        fixture.pic.loop_restoration_fields.bits.cbframe_restoration_type = STD_VIDEO_AV1_FRAME_RESTORATION_TYPE_NONE;
        fixture.pic.loop_restoration_fields.bits.lr_unit_shift            = 2;
        std_data                                                          = {};
        reason[0]                                                         = '\0';
        ok = check(vkvv::build_av1_picture_std_data(&session, &fixture.input, &std_data, reason, sizeof(reason)), "AV1 256 restoration std data build failed") && ok;
        ok = check(std_data.restoration.LoopRestorationSize[0] == 3, "AV1 256-pixel loop restoration size was not translated to code 3") && ok;

        fixture.pic.loop_restoration_fields.bits.lr_unit_shift = 3;
        std_data                                               = {};
        reason[0]                                              = '\0';
        ok = check(!vkvv::build_av1_picture_std_data(&session, &fixture.input, &std_data, reason, sizeof(reason)), "AV1 accepted an invalid loop restoration unit size") && ok;
        return check(std::strstr(reason, "invalid AV1 loop restoration size") != nullptr, "AV1 invalid restoration size did not explain the rejection") && ok;
    }

    bool check_av1_loop_filter_delta_translation() {
        AV1StdInputFixture      fixture{};
        vkvv::AV1VideoSession   session{};
        vkvv::AV1PictureStdData std_data{};
        char                    reason[512] = {};

        fixture.pic.primary_ref_frame = 0;
        for (uint32_t i = 0; i < STD_VIDEO_AV1_TOTAL_REFS_PER_FRAME; i++) {
            fixture.pic.ref_deltas[i] = 7;
        }
        fixture.pic.mode_deltas[0] = 7;
        fixture.pic.mode_deltas[1] = 7;

        bool ok = check(vkvv::build_av1_picture_std_data(&session, &fixture.input, &std_data, reason, sizeof(reason)), "AV1 loop-filter inherited std data build failed");
        if (!ok) {
            std::fprintf(stderr, "%s\n", reason);
            return false;
        }
        const int8_t default_ref_deltas[STD_VIDEO_AV1_TOTAL_REFS_PER_FRAME]     = {1, 0, 0, 0, -1, 0, -1, -1};
        const int8_t default_mode_deltas[STD_VIDEO_AV1_LOOP_FILTER_ADJUSTMENTS] = {0, 0};
        ok = check(std_data.loop_filter.update_ref_delta == 0 && std_data.loop_filter.update_mode_delta == 0, "AV1 inherited loop-filter deltas set update masks") && ok;
        ok = check_loop_filter_ref_deltas(std_data.loop_filter, default_ref_deltas, "AV1 inherited loop-filter") && ok;
        ok = check_loop_filter_mode_deltas(std_data.loop_filter, default_mode_deltas, "AV1 inherited loop-filter") && ok;

        fixture.pic.loop_filter_info_fields.bits.mode_ref_delta_update          = 1;
        const int8_t updated_ref_deltas[STD_VIDEO_AV1_TOTAL_REFS_PER_FRAME]     = {2, 3, 4, 5, 6, 7, 8, 9};
        const int8_t updated_mode_deltas[STD_VIDEO_AV1_LOOP_FILTER_ADJUSTMENTS] = {-1, 1};
        for (uint32_t i = 0; i < STD_VIDEO_AV1_TOTAL_REFS_PER_FRAME; i++) {
            fixture.pic.ref_deltas[i] = updated_ref_deltas[i];
        }
        for (uint32_t i = 0; i < STD_VIDEO_AV1_LOOP_FILTER_ADJUSTMENTS; i++) {
            fixture.pic.mode_deltas[i] = updated_mode_deltas[i];
        }
        std_data  = {};
        reason[0] = '\0';
        ok        = check(vkvv::build_av1_picture_std_data(&session, &fixture.input, &std_data, reason, sizeof(reason)), "AV1 loop-filter update std data build failed") && ok;
        ok        = check(std_data.loop_filter.update_ref_delta == 0xff && std_data.loop_filter.update_mode_delta == 0x03, "AV1 loop-filter update masks were incomplete") && ok;
        ok        = check_loop_filter_ref_deltas(std_data.loop_filter, updated_ref_deltas, "AV1 updated loop-filter") && ok;
        ok        = check_loop_filter_mode_deltas(std_data.loop_filter, updated_mode_deltas, "AV1 updated loop-filter") && ok;

        fixture.pic.loop_filter_info_fields.bits.mode_ref_delta_update = 0;
        for (uint32_t i = 0; i < STD_VIDEO_AV1_TOTAL_REFS_PER_FRAME; i++) {
            fixture.pic.ref_deltas[i] = 63;
        }
        fixture.pic.mode_deltas[0] = 63;
        fixture.pic.mode_deltas[1] = 63;
        std_data                   = {};
        reason[0]                  = '\0';
        ok = check(vkvv::build_av1_picture_std_data(&session, &fixture.input, &std_data, reason, sizeof(reason)), "AV1 loop-filter persisted std data build failed") && ok;
        ok = check(std_data.loop_filter.update_ref_delta == 0 && std_data.loop_filter.update_mode_delta == 0, "AV1 persisted loop-filter deltas set update masks") && ok;
        ok = check_loop_filter_ref_deltas(std_data.loop_filter, updated_ref_deltas, "AV1 persisted loop-filter") && ok;
        ok = check_loop_filter_mode_deltas(std_data.loop_filter, updated_mode_deltas, "AV1 persisted loop-filter") && ok;

        fixture.pic.primary_ref_frame = STD_VIDEO_AV1_PRIMARY_REF_NONE;
        std_data                      = {};
        reason[0]                     = '\0';
        ok = check(vkvv::build_av1_picture_std_data(&session, &fixture.input, &std_data, reason, sizeof(reason)), "AV1 loop-filter reset std data build failed") && ok;
        ok = check_loop_filter_ref_deltas(std_data.loop_filter, default_ref_deltas, "AV1 reset loop-filter") && ok;
        return check_loop_filter_mode_deltas(std_data.loop_filter, default_mode_deltas, "AV1 reset loop-filter") && ok;
    }

    bool check_av1_reference_info_sign_bias() {
        AV1StdInputFixture fixture{};
        fixture.input.header.ref_frame_sign_bias[1] = 1;
        fixture.input.header.ref_frame_sign_bias[7] = 1;

        const StdVideoDecodeAV1ReferenceInfo info          = vkvv::build_av1_current_reference_info(&fixture.input);
        const uint8_t                        expected_bias = (1U << 1U) | (1U << 7U);
        bool                                 ok            = check(info.RefFrameSignBias == expected_bias, "AV1 current reference info did not preserve full sign-bias mask");

        vkvv::AV1VideoSession                session{};
        fixture.input.header.refresh_frame_flags = 1U << 2U;
        vkvv::av1_update_reference_slots_from_refresh(&session, &fixture.input, 77, 4, info);
        const vkvv::AV1ReferenceSlot* slot = vkvv::av1_reference_slot_for_index(&session, 2);
        return check(slot != nullptr && slot->info.RefFrameSignBias == expected_bias, "AV1 refreshed reference slot did not store full sign-bias mask") && ok;
    }

    bool check_av1_switch_frame_translation() {
        AV1StdInputFixture      fixture{};
        vkvv::AV1VideoSession   session{};
        vkvv::AV1PictureStdData std_data{};
        char                    reason[512] = {};

        fixture.pic.pic_info_fields.bits.frame_type           = STD_VIDEO_AV1_FRAME_TYPE_SWITCH;
        fixture.pic.pic_info_fields.bits.error_resilient_mode = 1;
        fixture.pic.primary_ref_frame                         = STD_VIDEO_AV1_PRIMARY_REF_NONE;
        fixture.input.header.frame_type                       = STD_VIDEO_AV1_FRAME_TYPE_SWITCH;
        fixture.input.header.error_resilient_mode             = true;
        fixture.input.header.frame_size_override_flag         = true;
        fixture.input.header.refresh_frame_flags              = 0xff;

        bool ok = check(vkvv::validate_av1_switch_frame(&fixture.input, reason, sizeof(reason)), "valid AV1 switch frame was rejected");
        if (!ok) {
            std::fprintf(stderr, "%s\n", reason);
            return false;
        }
        ok = check(vkvv::build_av1_picture_std_data(&session, &fixture.input, &std_data, reason, sizeof(reason)), "valid AV1 switch frame std data build failed") && ok;
        ok = check(std_data.picture.frame_type == STD_VIDEO_AV1_FRAME_TYPE_INTER, "AV1 switch frame was not translated to effective inter frame type") && ok;

        const StdVideoDecodeAV1ReferenceInfo info = vkvv::build_av1_current_reference_info(&fixture.input);
        ok = check(info.frame_type == STD_VIDEO_AV1_FRAME_TYPE_INTER, "AV1 switch reference info was not translated to effective inter frame type") && ok;

        fixture.input.header.refresh_frame_flags = 0xfe;
        reason[0]                                = '\0';
        ok = check(!vkvv::validate_av1_switch_frame(&fixture.input, reason, sizeof(reason)), "AV1 switch frame accepted incomplete refresh flags") && ok;
        ok = check(std::strstr(reason, "refresh_frame_flags") != nullptr, "AV1 switch frame refresh rejection did not explain the failed constraint") && ok;

        fixture.input.header.refresh_frame_flags  = 0xff;
        fixture.input.header.error_resilient_mode = false;
        reason[0]                                 = '\0';
        ok = check(!vkvv::validate_av1_switch_frame(&fixture.input, reason, sizeof(reason)), "AV1 switch frame accepted missing error resilient mode") && ok;
        return check(std::strstr(reason, "error_resilient_mode") != nullptr, "AV1 switch frame resilient-mode rejection did not explain the failed constraint") && ok;
    }

    bool check_av1_dpb_slots() {
        vkvv::AV1VideoSession session{};
        session.max_dpb_slots                                     = vkvv::max_av1_dpb_slots;
        bool                  used_slots[vkvv::max_av1_dpb_slots] = {};

        vkvv::AV1VideoSession zero_dpb_session{};
        if (!check(vkvv::allocate_av1_dpb_slot(&zero_dpb_session, used_slots) == -1, "AV1 zero-DPB session claimed a slot")) {
            return false;
        }

        const int first_slot = vkvv::allocate_av1_dpb_slot(&session, used_slots);
        if (!check(first_slot == 0, "first AV1 DPB slot allocation did not start at zero")) {
            return false;
        }

        StdVideoDecodeAV1ReferenceInfo info{};
        info.OrderHint = 3;
        vkvv::av1_set_reference_slot(&session, 0, 41, first_slot, info);
        vkvv::av1_set_surface_slot(&session, 41, first_slot, info);
        if (!check(vkvv::av1_dpb_slot_for_surface(&session, 41) == first_slot, "AV1 surface lookup did not return the stored slot")) {
            return false;
        }
        const vkvv::AV1ReferenceSlot* stored = vkvv::av1_reference_slot_for_surface(&session, 41);
        if (!check(stored != nullptr && stored->info.OrderHint == 3, "AV1 reference info was not stored with the slot")) {
            return false;
        }

        used_slots[first_slot] = true;
        const int second_slot  = vkvv::allocate_av1_dpb_slot(&session, used_slots);
        if (!check(second_slot == 1, "AV1 DPB allocation did not skip a used slot")) {
            return false;
        }
        info.OrderHint = 7;
        vkvv::av1_set_reference_slot(&session, 0, 41, second_slot, info);
        vkvv::av1_set_surface_slot(&session, 41, second_slot, info);
        stored = vkvv::av1_reference_slot_for_surface(&session, 41);
        if (!check(stored != nullptr && stored->slot == second_slot && stored->info.OrderHint == 7, "AV1 reference slot update did not replace the old slot")) {
            return false;
        }

        for (bool& used : used_slots) {
            used = false;
        }
        for (uint32_t i = 0; i < vkvv::max_av1_reference_slots; i++) {
            used_slots[i] = true;
        }
        const int ninth_slot = vkvv::allocate_av1_dpb_slot(&session, used_slots);
        if (!check(ninth_slot == static_cast<int>(vkvv::max_av1_reference_slots), "AV1 DPB allocation did not allow a slot beyond the 8 VBI entries")) {
            return false;
        }
        info.OrderHint = 11;
        vkvv::av1_set_reference_slot(&session, 4, 42, ninth_slot, info);
        vkvv::av1_set_surface_slot(&session, 42, ninth_slot, info);
        stored = vkvv::av1_reference_slot_for_surface(&session, 42);
        if (!check(stored != nullptr && stored->slot == ninth_slot && stored->info.OrderHint == 11, "AV1 reference table did not store a DPB slot beyond the 8 VBI entries")) {
            return false;
        }

        vkvv::av1_clear_reference_slot(&session, second_slot);
        return check(vkvv::av1_reference_slot_for_surface(&session, 41) == nullptr, "AV1 reference slot clear did not remove stale mapping");
    }

    bool check_av1_refresh_retention() {
        vkvv::AV1VideoSession          session{};
        StdVideoDecodeAV1ReferenceInfo info{};
        info.OrderHint = 9;
        vkvv::av1_set_reference_slot(&session, 3, 378, 6, info);
        vkvv::av1_set_surface_slot(&session, 378, 6, info);

        VADecPictureParameterBufferAV1 pic{};
        for (VASurfaceID& surface : pic.ref_frame_map) {
            surface = VA_INVALID_ID;
        }
        pic.ref_frame_map[3] = 378;

        VkvvAV1DecodeInput input{};
        input.pic                        = &pic;
        input.header.refresh_frame_flags = 0x00;

        bool used_slots[vkvv::max_av1_dpb_slots] = {};
        vkvv::av1_mark_retained_reference_slots(&session, &input, used_slots);
        if (!check(used_slots[6], "AV1 refresh=0 frame did not retain an existing live reference slot")) {
            return false;
        }

        vkvv::av1_update_reference_slots_from_refresh(&session, &input, 999, 4, info);
        if (!check(vkvv::av1_reference_slot_for_index(&session, 3) != nullptr && vkvv::av1_reference_slot_for_index(&session, 3)->surface_id == 378,
                   "AV1 refresh=0 frame incorrectly updated an existing reference")) {
            return false;
        }
        if (!check(vkvv::av1_surface_slot_for_surface(&session, 999) == nullptr, "AV1 refresh=0 frame incorrectly persisted scratch surface history")) {
            return false;
        }

        input.header.refresh_frame_flags = 1U << 3;
        for (bool& used : used_slots) {
            used = false;
        }
        vkvv::av1_mark_retained_reference_slots(&session, &input, used_slots);
        if (!check(!used_slots[6], "AV1 refreshed-away surface still blocked DPB slot reuse")) {
            return false;
        }
        vkvv::av1_update_reference_slots_from_refresh(&session, &input, 999, 4, info);
        if (!check(vkvv::av1_reference_slot_for_index(&session, 3) != nullptr && vkvv::av1_reference_slot_for_index(&session, 3)->surface_id == 999 &&
                       vkvv::av1_reference_slot_for_index(&session, 3)->slot == 4,
                   "AV1 refreshed reference index was not updated to the current surface")) {
            return false;
        }

        vkvv::av1_set_reference_slot(&session, 2, 378, 6, info);
        vkvv::av1_set_reference_slot(&session, 3, 378, 6, info);
        vkvv::av1_set_surface_slot(&session, 378, 6, info);
        pic.ref_frame_map[2] = 378;
        for (bool& used : used_slots) {
            used = false;
        }
        vkvv::av1_mark_retained_reference_slots(&session, &input, used_slots);
        if (!check(used_slots[6], "AV1 surface retained by another VBI index was not protected")) {
            return false;
        }
        vkvv::av1_update_reference_slots_from_refresh(&session, &input, 999, 4, info);
        return check(vkvv::av1_reference_slot_for_index(&session, 2) != nullptr && vkvv::av1_reference_slot_for_index(&session, 2)->surface_id == 378 &&
                         vkvv::av1_reference_slot_for_index(&session, 3) != nullptr && vkvv::av1_reference_slot_for_index(&session, 3)->surface_id == 999,
                     "AV1 refresh update touched the wrong VBI reference index");
    }

    bool check_av1_surface_reconciliation() {
        vkvv::AV1VideoSession          session{};
        StdVideoDecodeAV1ReferenceInfo info{};
        info.OrderHint = 38;
        vkvv::av1_set_surface_slot(&session, 38, 7, info);

        VADecPictureParameterBufferAV1 pic{};
        for (VASurfaceID& surface : pic.ref_frame_map) {
            surface = VA_INVALID_ID;
        }
        pic.ref_frame_map[1] = 38;

        VkvvAV1DecodeInput input{};
        input.pic                        = &pic;
        input.header.refresh_frame_flags = 0x00;

        bool used_slots[vkvv::max_av1_dpb_slots] = {};
        vkvv::av1_mark_retained_reference_slots(&session, &input, used_slots);
        if (!check(used_slots[7], "AV1 surface history did not protect a retained ref_frame_map surface with missing VBI entry")) {
            return false;
        }

        const vkvv::AV1ReferenceSlot* slot = vkvv::av1_reconcile_reference_slot(&session, 1, 38);
        if (!check(slot != nullptr && slot->surface_id == 38 && slot->slot == 7 && slot->info.OrderHint == 38,
                   "AV1 reference reconciliation did not recover a missing VBI entry from surface history")) {
            return false;
        }

        slot = vkvv::av1_reference_slot_for_index(&session, 1);
        return check(slot != nullptr && slot->surface_id == 38 && slot->slot == 7, "AV1 reference reconciliation did not persist the recovered VBI entry");
    }

    vkvv::DecodeImageKey make_av1_decode_key(unsigned int fourcc = VA_FOURCC_NV12) {
        vkvv::DecodeImageKey key{};
        key.codec_operation          = VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR;
        key.codec_profile            = STD_VIDEO_AV1_PROFILE_MAIN;
        key.picture_format           = fourcc == VA_FOURCC_P010 ? VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16 : VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
        key.reference_picture_format = key.picture_format;
        key.va_rt_format             = fourcc == VA_FOURCC_P010 ? VA_RT_FORMAT_YUV420_10 : VA_RT_FORMAT_YUV420;
        key.va_fourcc                = fourcc;
        key.coded_extent             = {64, 64};
        key.usage                    = vkvv::av1_surface_image_usage();
        key.tiling                   = VK_IMAGE_TILING_OPTIMAL;
        key.chroma_subsampling       = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
        key.luma_bit_depth           = fourcc == VA_FOURCC_P010 ? VK_VIDEO_COMPONENT_BIT_DEPTH_10_BIT_KHR : VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
        key.chroma_bit_depth         = key.luma_bit_depth;
        return key;
    }

    bool check_av1_reference_identity_validation() {
        const vkvv::DecodeImageKey key = make_av1_decode_key();

        vkvv::AV1VideoSession      session{};
        session.bit_depth = 8;
        session.video.key = {
            .codec_operation          = key.codec_operation,
            .codec_profile            = key.codec_profile,
            .picture_format           = key.picture_format,
            .reference_picture_format = key.reference_picture_format,
            .max_coded_extent         = key.coded_extent,
            .image_usage              = key.usage,
            .image_create_flags       = key.create_flags,
            .image_tiling             = key.tiling,
            .chroma_subsampling       = key.chroma_subsampling,
            .luma_bit_depth           = key.luma_bit_depth,
            .chroma_bit_depth         = key.chroma_bit_depth,
        };

        vkvv::SurfaceResource resource{};
        resource.driver_instance_id = 17;
        resource.stream_id          = 23;
        resource.codec_operation    = key.codec_operation;
        resource.surface_id         = 55;
        resource.coded_extent       = key.coded_extent;
        resource.format             = key.picture_format;
        resource.va_rt_format       = key.va_rt_format;
        resource.va_fourcc          = key.va_fourcc;
        resource.decode_key         = key;
        resource.content_generation = 3;

        VkvvSurface surface{};
        surface.id                 = 55;
        surface.driver_instance_id = 17;
        surface.stream_id          = 23;
        surface.codec_operation    = key.codec_operation;
        surface.rt_format          = key.va_rt_format;
        surface.fourcc             = key.va_fourcc;
        surface.vulkan             = &resource;
        surface.decoded            = true;

        VkvvDriver drv{};
        drv.driver_instance_id = 17;

        VkvvContext vctx{};
        vctx.stream_id       = 23;
        vctx.codec_operation = key.codec_operation;

        vkvv::AV1ReferenceMetadata metadata{};
        metadata.driver_instance_id = drv.driver_instance_id;
        metadata.stream_id          = vctx.stream_id;
        metadata.codec_operation    = key.codec_operation;
        metadata.surface_id         = surface.id;
        metadata.content_generation = resource.content_generation;
        metadata.decode_key         = key;
        metadata.coded_extent       = resource.coded_extent;
        metadata.va_rt_format       = resource.va_rt_format;
        metadata.va_fourcc          = resource.va_fourcc;
        metadata.bit_depth          = session.bit_depth;
        metadata.showable           = true;

        StdVideoDecodeAV1ReferenceInfo info{};
        vkvv::av1_set_reference_slot(&session, 0, surface.id, 1, info, &metadata);
        const vkvv::AV1ReferenceSlot* slot = vkvv::av1_reference_slot_for_index(&session, 0);

        char                          reason[512] = {};
        bool                          ok =
            check(vkvv::validate_av1_reference_slot(&session, slot, &surface, &resource, &drv, &vctx, key, reason, sizeof(reason)), "valid AV1 reference identity was rejected");

        surface.stream_id = 24;
        reason[0]         = '\0';
        ok                = check(!vkvv::validate_av1_reference_slot(&session, slot, &surface, &resource, &drv, &vctx, key, reason, sizeof(reason)),
                                  "AV1 reference validation accepted a cross-stream surface") &&
            ok;
        surface.stream_id = vctx.stream_id;

        surface.codec_operation = VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR;
        reason[0]               = '\0';
        ok                      = check(!vkvv::validate_av1_reference_slot(&session, slot, &surface, &resource, &drv, &vctx, key, reason, sizeof(reason)),
                                        "AV1 reference validation accepted a wrong-codec surface") &&
            ok;
        surface.codec_operation = key.codec_operation;

        resource.content_generation = 4;
        reason[0]                   = '\0';
        ok                          = check(!vkvv::validate_av1_reference_slot(&session, slot, &surface, &resource, &drv, &vctx, key, reason, sizeof(reason)),
                                            "AV1 reference validation accepted stale content generation") &&
            ok;
        resource.content_generation = metadata.content_generation;

        resource.va_fourcc = VA_FOURCC_P010;
        reason[0]          = '\0';
        ok                 = check(!vkvv::validate_av1_reference_slot(&session, slot, &surface, &resource, &drv, &vctx, key, reason, sizeof(reason)),
                                   "AV1 reference validation accepted a wrong-format resource") &&
            ok;
        resource.va_fourcc = metadata.va_fourcc;

        vkvv::av1_set_reference_slot(&session, 1, surface.id, 2, info);
        const vkvv::AV1ReferenceSlot* bare_slot = vkvv::av1_reference_slot_for_index(&session, 1);
        reason[0]                               = '\0';
        return check(!vkvv::validate_av1_reference_slot(&session, bare_slot, &surface, &resource, &drv, &vctx, key, reason, sizeof(reason)),
                     "AV1 reference validation accepted a slot without identity metadata") &&
            ok;
    }

    bool check_av1_target_slot_selection() {
        vkvv::AV1VideoSession session{};
        session.max_dpb_slots                    = vkvv::max_av1_dpb_slots;
        bool used_slots[vkvv::max_av1_dpb_slots] = {};
        for (bool& used : used_slots) {
            used = true;
        }

        vkvv::AV1VideoSession zero_dpb_session{};
        const int             zero_dpb_slot = vkvv::av1_select_current_setup_slot(&zero_dpb_session, 77, used_slots, true);
        if (!check(zero_dpb_slot == -1, "zero-DPB AV1 session claimed a setup slot")) {
            return false;
        }

        const int full_slot = vkvv::av1_select_current_setup_slot(&session, 77, used_slots, true);
        if (!check(full_slot == -1, "AV1 target selection claimed a DPB setup slot under full slot pressure")) {
            return false;
        }

        used_slots[4]               = false;
        const int display_only_slot = vkvv::av1_select_current_setup_slot(&session, 77, used_slots, false);
        if (!check(display_only_slot == 4, "display-only AV1 frame did not claim a scratch DPB setup slot")) {
            return false;
        }
        if (!check(session.next_dpb_slot == 5, "display-only AV1 scratch setup did not advance DPB slot allocation state")) {
            return false;
        }
        if (!check(vkvv::av1_surface_slot_for_surface(&session, 77) == nullptr, "display-only AV1 scratch setup persisted surface history")) {
            return false;
        }

        const int setup_slot = vkvv::av1_select_current_setup_slot(&session, 77, used_slots, true);
        if (!check(setup_slot == 4, "reference AV1 frame did not claim an available DPB setup slot")) {
            return false;
        }

        vkvv::av1_set_surface_slot(&session, 77, setup_slot, {});
        for (bool& used : used_slots) {
            used = false;
        }
        used_slots[setup_slot]     = true;
        const int conflicting_slot = vkvv::av1_select_current_setup_slot(&session, 77, used_slots, true);
        return check(conflicting_slot != setup_slot, "AV1 target slot selection reused an already-used target slot");
    }

    bool check_av1_target_layout_selection() {
        bool ok = check(vkvv::av1_target_layout(false) == VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR, "AV1 frame without a setup slot did not use decode destination layout");
        ok      = check(vkvv::av1_target_access(false) == VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR, "AV1 frame without a setup slot did not use write-only decode access") && ok;
        ok      = check(vkvv::av1_target_layout(true) == VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR, "AV1 frame with a setup slot did not use DPB layout") && ok;
        ok      = check(vkvv::av1_target_access(true) == (VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR | VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR),
                        "AV1 frame with a setup slot did not use DPB read/write access") &&
            ok;
        return ok;
    }

    bool check_av1_export_refresh_decision() {
        VADecPictureParameterBufferAV1 pic{};
        VkvvAV1DecodeInput             input{};
        input.pic = &pic;

        bool ok = check(!vkvv::av1_decode_needs_export_refresh(&input), "pure hidden AV1 reference frame unexpectedly refreshed export shadow");

        input.header.showable_frame = true;
        ok                          = check(!vkvv::av1_decode_needs_export_refresh(&input), "showable AV1 hidden frame unexpectedly refreshed export shadow") && ok;

        input.header.showable_frame = false;
        input.header.show_frame     = true;
        ok                          = check(vkvv::av1_decode_needs_export_refresh(&input), "AV1 header show_frame did not request export shadow refresh") && ok;

        input.header.show_frame          = false;
        input.header.show_existing_frame = true;
        ok                               = check(vkvv::av1_decode_needs_export_refresh(&input), "AV1 show-existing frame did not request export shadow refresh") && ok;

        input.header.show_existing_frame    = false;
        pic.pic_info_fields.bits.show_frame = 1;
        return check(vkvv::av1_decode_needs_export_refresh(&input), "VA AV1 show_frame did not request export shadow refresh") && ok;
    }

    bool submit_empty_pending(vkvv::VulkanRuntime* runtime, VkvvSurface* surface, const char* operation) {
        char reason[512] = {};
        {
            std::lock_guard<std::mutex> command_lock(runtime->command_mutex);
            if (!check(vkvv::ensure_command_resources(runtime, reason, sizeof(reason)), "ensure_command_resources failed")) {
                std::fprintf(stderr, "%s\n", reason);
                return false;
            }

            VkResult result = vkResetFences(runtime->device, 1, &runtime->fence);
            if (!check(result == VK_SUCCESS, "vkResetFences failed for pending AV1 smoke")) {
                return false;
            }
            result = vkResetCommandBuffer(runtime->command_buffer, 0);
            if (!check(result == VK_SUCCESS, "vkResetCommandBuffer failed for pending AV1 smoke")) {
                return false;
            }

            VkCommandBufferBeginInfo begin_info{};
            begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            result           = vkBeginCommandBuffer(runtime->command_buffer, &begin_info);
            if (!check(result == VK_SUCCESS, "vkBeginCommandBuffer failed for pending AV1 smoke")) {
                return false;
            }
            result = vkEndCommandBuffer(runtime->command_buffer);
            if (!check(result == VK_SUCCESS, "vkEndCommandBuffer failed for pending AV1 smoke")) {
                return false;
            }
            if (!check(vkvv::submit_command_buffer(runtime, reason, sizeof(reason), operation), "submit_command_buffer failed for pending AV1 smoke")) {
                std::fprintf(stderr, "%s\n", reason);
                return false;
            }
            surface->work_state  = VKVV_SURFACE_WORK_RENDERING;
            surface->sync_status = VA_STATUS_ERROR_TIMEDOUT;
            vkvv::track_pending_decode(runtime, surface, VK_NULL_HANDLE, 0, true, operation);
        }
        return true;
    }

    bool check_pending_reference_completion(vkvv::VulkanRuntime* runtime) {
        VkvvSurface reference{};
        reference.id                 = 701;
        reference.driver_instance_id = 1;
        reference.stream_id          = 9;
        reference.codec_operation    = VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR;

        if (!submit_empty_pending(runtime, &reference, "AV1 pending reference smoke")) {
            return false;
        }

        char     reason[512] = {};
        VAStatus status      = vkvv::complete_pending_surface_work_if_needed(runtime, &reference, "AV1 reference", reason, sizeof(reason));
        if (!check(status == VA_STATUS_SUCCESS, "pending AV1 reference completion failed")) {
            std::fprintf(stderr, "%s\n", reason);
            return false;
        }
        return check(!vkvv::runtime_surface_has_pending_work(runtime, &reference) && reference.work_state == VKVV_SURFACE_WORK_READY &&
                         reference.sync_status == VA_STATUS_SUCCESS && reference.decoded,
                     "pending AV1 reference was not ready after helper completion");
    }

} // namespace

int main(void) {
    bool ok = check_av1_sequence_parameters();
    ok      = check_av1_sequence_key_reset() && ok;
    ok      = check_av1_restoration_translation() && ok;
    ok      = check_av1_loop_filter_delta_translation() && ok;
    ok      = check_av1_reference_info_sign_bias() && ok;
    ok      = check_av1_switch_frame_translation() && ok;
    ok      = check_av1_dpb_slots() && ok;
    ok      = check_av1_refresh_retention() && ok;
    ok      = check_av1_surface_reconciliation() && ok;
    ok      = check_av1_reference_identity_validation() && ok;
    ok      = check_av1_target_slot_selection() && ok;
    ok      = check_av1_target_layout_selection() && ok;
    ok      = check_av1_export_refresh_decision() && ok;

    char  reason[512] = {};
    void* runtime     = vkvv_vulkan_runtime_create(reason, sizeof(reason));
    std::printf("%s\n", reason);
    if (runtime == nullptr) {
        return 1;
    }
    auto* typed_runtime = static_cast<vkvv::VulkanRuntime*>(runtime);
    ok = check((typed_runtime->enabled_decode_operations & VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR) != 0, "runtime did not enable AV1 through codec-driven selection") && ok;
    ok = check_pending_reference_completion(typed_runtime) && ok;

    void* session = vkvv_vulkan_av1_session_create();
    if (session == nullptr) {
        vkvv_vulkan_runtime_destroy(runtime);
        return 1;
    }

    ok = ensure_session(runtime, session, 64, 64, 64, 64, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR, "AV1 Profile0") && ok;
    auto*                typed_session = static_cast<vkvv::AV1VideoSession*>(session);
    std::vector<uint8_t> first_upload(256, 0x11);
    ok                                         = ensure_upload(typed_runtime, typed_session, first_upload) && ok;
    const VkBuffer       first_upload_buffer   = typed_session->uploads[0].buffer;
    const VkDeviceMemory first_upload_memory   = typed_session->uploads[0].memory;
    const VkDeviceSize   first_upload_capacity = typed_session->uploads[0].capacity;

    std::vector<uint8_t> smaller_upload(128, 0x22);
    ok = ensure_upload(typed_runtime, typed_session, smaller_upload) && ok;
    ok = check(typed_session->uploads[0].buffer == first_upload_buffer && typed_session->uploads[0].memory == first_upload_memory &&
                   typed_session->uploads[0].capacity == first_upload_capacity,
               "smaller AV1 upload did not reuse the existing buffer") &&
        ok;

    ok = ensure_session(runtime, session, 640, 360, 640, 368, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR, "AV1 Profile0") && ok;
    const VkVideoSessionKHR grown_session = typed_session->video.session;
    ok = ensure_session(runtime, session, 320, 180, 640, 368, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR, "AV1 Profile0") && ok;
    ok = check(typed_session->video.session == grown_session, "AV1 session unexpectedly shrank or recreated") && ok;
    ok = configure_session_for_p010(runtime, session) && ok;
    ok = ensure_session(runtime, session, 64, 64, 64, 64, VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16, VK_VIDEO_COMPONENT_BIT_DEPTH_10_BIT_KHR,
                        "AV1 Profile0 P010 after retarget") &&
        ok;

    vkvv_vulkan_av1_session_destroy(runtime, session);

    VkvvConfig p010_config{};
    p010_config.rt_format = VA_RT_FORMAT_YUV420_10;
    p010_config.fourcc    = VA_FOURCC_P010;
    p010_config.bit_depth = 10;
    session               = vkvv_vulkan_av1_session_create_for_config(&p010_config);
    if (session == nullptr) {
        vkvv_vulkan_runtime_destroy(runtime);
        return 1;
    }
    ok = ensure_session(runtime, session, 64, 64, 64, 64, VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16, VK_VIDEO_COMPONENT_BIT_DEPTH_10_BIT_KHR, "AV1 Profile0 P010") && ok;
    typed_session = static_cast<vkvv::AV1VideoSession*>(session);
    ok            = check(typed_session->va_rt_format == VA_RT_FORMAT_YUV420_10 && typed_session->va_fourcc == VA_FOURCC_P010 && typed_session->bit_depth == 10,
                          "AV1 P010 session did not record VA format metadata") &&
        ok;
    vkvv_vulkan_av1_session_destroy(runtime, session);

    vkvv_vulkan_runtime_destroy(runtime);
    if (!ok) {
        return 1;
    }

    std::printf("AV1 session sizing smoke passed\n");
    return 0;
}
