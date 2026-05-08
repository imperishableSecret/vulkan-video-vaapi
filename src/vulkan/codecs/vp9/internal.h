#ifndef VKVV_VULKAN_VP9_INTERNAL_H
#define VKVV_VULKAN_VP9_INTERNAL_H

#include "vulkan_runtime_internal.h"
#include "codecs/vp9/vp9.h"

namespace vkvv {

inline constexpr uint32_t max_vp9_reference_slots = VKVV_VP9_REFERENCE_COUNT;
inline constexpr uint32_t max_vp9_active_references = VKVV_VP9_ACTIVE_REFERENCE_COUNT;
inline constexpr uint32_t max_vp9_dpb_slots = VKVV_VP9_REFERENCE_COUNT;

struct VP9ReferenceSlot {
    VASurfaceID surface_id = VA_INVALID_ID;
    int slot = -1;
};

struct VP9VideoSession {
    VideoSession video;
    UploadBuffer upload;
    VP9ReferenceSlot reference_slots[max_vp9_reference_slots]{};
    VkDeviceSize bitstream_offset_alignment = 1;
    VkDeviceSize bitstream_size_alignment = 1;
    StdVideoVP9Level max_level = STD_VIDEO_VP9_LEVEL_6_2;
    VkVideoDecodeCapabilityFlagsKHR decode_flags = 0;
    uint32_t next_dpb_slot = 0;
    uint32_t max_dpb_slots = 0;
    uint32_t max_active_reference_pictures = 0;
};

inline constexpr VideoProfileSpec vp9_profile0_spec{
    .operation = VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR,
    .bit_depth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR,
    .std_profile = STD_VIDEO_VP9_PROFILE_0,
};

void destroy_vp9_video_session(VulkanRuntime *runtime, VP9VideoSession *session);
VkImageUsageFlags vp9_surface_image_usage();
bool reset_vp9_session(
        VulkanRuntime *runtime,
        VP9VideoSession *session,
        char *reason,
        size_t reason_size);
int vp9_dpb_slot_for_reference_index(const VP9VideoSession *session, uint32_t reference_index);
int vp9_dpb_slot_for_surface(const VP9VideoSession *session, VASurfaceID surface_id);
void vp9_set_reference_slot(VP9VideoSession *session, uint32_t reference_index, VASurfaceID surface_id, int slot);
int allocate_vp9_dpb_slot(VP9VideoSession *session, const bool used_slots[max_vp9_dpb_slots]);

} // namespace vkvv

#endif
