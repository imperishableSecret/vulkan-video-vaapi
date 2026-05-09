#include "vulkan/codecs/h264/api.h"
#include "vulkan/codecs/h264/internal.h"

#include <cstdlib>
#include <cstdio>

namespace {

    bool check(bool condition, const char* message) {
        if (!condition) {
            std::fprintf(stderr, "%s\n", message);
        }
        return condition;
    }

    VkvvH264EncodeInput make_input(VAEncSequenceParameterBufferH264* sequence, VAEncPictureParameterBufferH264* picture, VAEncSliceParameterBufferH264* slice) {
        *sequence                                                   = {};
        sequence->seq_parameter_set_id                              = 0;
        sequence->level_idc                                         = STD_VIDEO_H264_LEVEL_IDC_4_1;
        sequence->intra_period                                      = 30;
        sequence->intra_idr_period                                  = 30;
        sequence->ip_period                                         = 1;
        sequence->bits_per_second                                   = 4'000'000;
        sequence->max_num_ref_frames                                = 1;
        sequence->picture_width_in_mbs                              = 4;
        sequence->picture_height_in_mbs                             = 4;
        sequence->seq_fields.bits.chroma_format_idc                 = 1;
        sequence->seq_fields.bits.frame_mbs_only_flag               = 1;
        sequence->seq_fields.bits.direct_8x8_inference_flag         = 1;
        sequence->seq_fields.bits.log2_max_frame_num_minus4         = 4;
        sequence->seq_fields.bits.pic_order_cnt_type                = 0;
        sequence->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 = 4;
        sequence->vui_parameters_present_flag                       = 1;
        sequence->vui_fields.bits.timing_info_present_flag          = 1;
        sequence->vui_fields.bits.fixed_frame_rate_flag             = 1;
        sequence->num_units_in_tick                                 = 1;
        sequence->time_scale                                        = 60;

        *picture                                                        = {};
        picture->coded_buf                                              = 11;
        picture->pic_parameter_set_id                                   = 0;
        picture->seq_parameter_set_id                                   = 0;
        picture->frame_num                                              = 0;
        picture->pic_init_qp                                            = 26;
        picture->CurrPic.picture_id                                     = 7;
        picture->CurrPic.frame_idx                                      = 0;
        picture->pic_fields.bits.idr_pic_flag                           = 1;
        picture->pic_fields.bits.reference_pic_flag                     = 1;
        picture->pic_fields.bits.deblocking_filter_control_present_flag = 1;

        *slice                               = {};
        slice->macroblock_address            = 0;
        slice->num_macroblocks               = 16;
        slice->macroblock_info               = VA_INVALID_ID;
        slice->slice_type                    = 2;
        slice->pic_parameter_set_id          = 0;
        slice->idr_pic_id                    = 0;
        slice->disable_deblocking_filter_idc = 0;

        VkvvH264EncodeInput input{};
        input.sequence              = sequence;
        input.picture               = picture;
        input.slices                = slice;
        input.slice_count           = 1;
        input.width                 = 64;
        input.height                = 64;
        input.input_surface         = 3;
        input.reconstructed_surface = 7;
        input.coded_buffer          = 11;
        input.frame_type            = VKVV_H264_ENCODE_FRAME_IDR;
        return input;
    }

} // namespace

int main(void) {
    setenv("VKVV_ENABLE_ENCODE", "1", 1);

    char  reason[512]{};
    void* runtime = vkvv_vulkan_runtime_create(reason, sizeof(reason));
    std::printf("%s\n", reason);
    if (runtime == nullptr) {
        unsetenv("VKVV_ENABLE_ENCODE");
        return 1;
    }

    auto* typed_runtime = static_cast<vkvv::VulkanRuntime*>(runtime);
    if ((typed_runtime->enabled_encode_operations & VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR) == 0) {
        std::printf("H.264 encode runtime unavailable, skipping encode session smoke\n");
        vkvv_vulkan_runtime_destroy(runtime);
        unsetenv("VKVV_ENABLE_ENCODE");
        return 0;
    }

    void* session_ptr = vkvv_vulkan_h264_encode_session_create();
    if (session_ptr == nullptr) {
        vkvv_vulkan_runtime_destroy(runtime);
        unsetenv("VKVV_ENABLE_ENCODE");
        return 1;
    }

    VAEncSequenceParameterBufferH264 sequence{};
    VAEncPictureParameterBufferH264  picture{};
    VAEncSliceParameterBufferH264    slice{};
    VkvvH264EncodeInput              input = make_input(&sequence, &picture, &slice);

    bool                             ok = true;
    ok = check(vkvv_vulkan_ensure_h264_encode_session(runtime, session_ptr, &input, reason, sizeof(reason)) == VA_STATUS_SUCCESS, "ensure H.264 encode session failed") && ok;
    if (!ok) {
        std::fprintf(stderr, "%s\n", reason);
    }
    auto* session = static_cast<vkvv::H264EncodeSession*>(session_ptr);
    ok            = check(session->video.session != VK_NULL_HANDLE && session->parameters != VK_NULL_HANDLE, "encode session did not allocate session parameters") && ok;
    ok = check(session->video.key.codec_operation == VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR && session->video.key.picture_format == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
               "encode session key was not populated") &&
        ok;
    ok = check(session->video.memory_bytes > 0 && session->max_dpb_slots >= 1 && session->max_active_reference_pictures >= 1, "encode session limits were not recorded") && ok;

    vkvv_vulkan_h264_encode_session_destroy(runtime, session_ptr);
    vkvv_vulkan_runtime_destroy(runtime);
    unsetenv("VKVV_ENABLE_ENCODE");

    if (!ok) {
        return 1;
    }
    std::printf("H.264 encode session smoke passed\n");
    return 0;
}
