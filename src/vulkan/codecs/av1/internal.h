#ifndef VKVV_VULKAN_AV1_INTERNAL_H
#define VKVV_VULKAN_AV1_INTERNAL_H

#include "vulkan_runtime_internal.h"
#include "codecs/av1/av1.h"

#include <array>
#include <vector>

namespace vkvv {

inline constexpr uint32_t max_av1_reference_slots = VKVV_AV1_REFERENCE_COUNT;
inline constexpr uint32_t max_av1_active_references = VKVV_AV1_ACTIVE_REFERENCE_COUNT;
inline constexpr uint32_t max_av1_dpb_slots = 10;

inline constexpr VideoProfileSpec av1_profile0_spec{
    .operation = VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR,
    .bit_depth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR,
    .std_profile = STD_VIDEO_AV1_PROFILE_MAIN,
};

struct AV1ReferenceSlot {
    VASurfaceID surface_id = VA_INVALID_ID;
    int slot = -1;
    StdVideoDecodeAV1ReferenceInfo info{};
};

struct AV1VideoSession {
    VAProfile va_profile = VAProfileAV1Profile0;
    unsigned int va_rt_format = VA_RT_FORMAT_YUV420;
    unsigned int va_fourcc = VA_FOURCC_NV12;
    uint8_t bitstream_profile = 0;
    uint8_t bit_depth = 8;
    VideoProfileSpec profile_spec = av1_profile0_spec;
    VideoSession video;
    UploadBuffer upload;
    AV1ReferenceSlot reference_slots[max_av1_reference_slots]{};
    std::vector<AV1ReferenceSlot> surface_slots;
    VkDeviceSize bitstream_offset_alignment = 1;
    VkDeviceSize bitstream_size_alignment = 1;
    StdVideoAV1Level max_level = STD_VIDEO_AV1_LEVEL_6_2;
    VkVideoDecodeCapabilityFlagsKHR decode_flags = 0;
    uint32_t next_dpb_slot = 0;
    uint32_t max_dpb_slots = 0;
    uint32_t max_active_reference_pictures = 0;
};

struct AV1SessionStdParameters {
    StdVideoAV1ColorConfig color{};
    StdVideoAV1SequenceHeader sequence{};
};

void destroy_av1_video_session(VulkanRuntime *runtime, AV1VideoSession *session);
VkImageUsageFlags av1_surface_image_usage();
bool reset_av1_session(
        VulkanRuntime *runtime,
        AV1VideoSession *session,
        VkVideoSessionParametersKHR parameters,
        char *reason,
        size_t reason_size);
int av1_dpb_slot_for_surface(const AV1VideoSession *session, VASurfaceID surface_id);
const AV1ReferenceSlot *av1_reference_slot_for_index(const AV1VideoSession *session, uint32_t reference_index);
const AV1ReferenceSlot *av1_reference_slot_for_surface(const AV1VideoSession *session, VASurfaceID surface_id);
const AV1ReferenceSlot *av1_surface_slot_for_surface(const AV1VideoSession *session, VASurfaceID surface_id);
const AV1ReferenceSlot *av1_reconcile_reference_slot(
        AV1VideoSession *session,
        uint32_t reference_index,
        VASurfaceID surface_id);
void av1_set_reference_slot(
        AV1VideoSession *session,
        uint32_t reference_index,
        VASurfaceID surface_id,
        int slot,
        const StdVideoDecodeAV1ReferenceInfo &info);
void av1_set_surface_slot(
        AV1VideoSession *session,
        VASurfaceID surface_id,
        int slot,
        const StdVideoDecodeAV1ReferenceInfo &info);
void av1_clear_reference_slot(AV1VideoSession *session, int slot);
void av1_mark_retained_reference_slots(
        const AV1VideoSession *session,
        const VkvvAV1DecodeInput *input,
        bool used_slots[max_av1_dpb_slots]);
int av1_select_target_dpb_slot(
        AV1VideoSession *session,
        VASurfaceID target_surface_id,
        const bool used_slots[max_av1_dpb_slots]);
void av1_update_reference_slots_from_refresh(
        AV1VideoSession *session,
        const VkvvAV1DecodeInput *input,
        VASurfaceID target_surface_id,
        int target_slot,
        const StdVideoDecodeAV1ReferenceInfo &info);
int allocate_av1_dpb_slot(AV1VideoSession *session, const bool used_slots[max_av1_dpb_slots]);
void build_av1_session_parameters(const VkvvAV1DecodeInput *input, AV1SessionStdParameters *std_params);
bool create_av1_session_parameters(
        VulkanRuntime *runtime,
        AV1VideoSession *session,
        const AV1SessionStdParameters *std_params,
        VkVideoSessionParametersKHR *parameters,
        char *reason,
        size_t reason_size);

} // namespace vkvv

#endif
