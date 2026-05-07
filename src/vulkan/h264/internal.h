#ifndef VKVV_VULKAN_H264_INTERNAL_H
#define VKVV_VULKAN_H264_INTERNAL_H

#include "../../vulkan_runtime_internal.h"

namespace vkvv {

struct H264StdParameters {
    StdVideoH264SequenceParameterSet sps{};
    StdVideoH264SequenceParameterSetVui vui{};
    StdVideoH264HrdParameters vui_hrd{};
    StdVideoH264ScalingLists sps_scaling{};
    StdVideoH264ScalingLists pps_scaling{};
    StdVideoH264PictureParameterSet pps{};
};

bool h264_picture_is_invalid(const VAPictureH264 &picture);
int allocate_dpb_slot(VkvvContext *vctx, const bool used_slots[max_h264_dpb_slots]);
void fill_reference_info(const VAPictureH264 &picture, uint16_t frame_num, StdVideoDecodeH264ReferenceInfo *info);
bool bitstream_has_idr(const VkvvH264DecodeInput *input);
void build_h264_std_parameters(H264VideoSession *session, VAProfile profile, const VkvvH264DecodeInput *input, H264StdParameters *std_params);
bool create_h264_session_parameters(VulkanRuntime *runtime, H264VideoSession *session, const H264StdParameters *std_params, VkVideoSessionParametersKHR *parameters, char *reason, size_t reason_size);
bool create_empty_h264_session_parameters(VulkanRuntime *runtime, H264VideoSession *session, VkVideoSessionParametersKHR *parameters, char *reason, size_t reason_size);

} // namespace vkvv

#endif
