#include "vulkan/codecs/vp9/api.h"
#include "vulkan/codecs/vp9/internal.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

    bool check(bool condition, const char* message) {
        if (!condition) {
            std::fprintf(stderr, "%s\n", message);
        }
        return condition;
    }

    bool ensure_session(void* runtime, void* session, unsigned int width, unsigned int height, unsigned int min_expected_width, unsigned int min_expected_height,
                        uint32_t expected_profile, VkVideoComponentBitDepthFlagsKHR expected_bit_depth, VkFormat expected_format, const char* label) {
        char     reason[512] = {};
        VAStatus status      = vkvv_vulkan_ensure_vp9_session(runtime, session, width, height, reason, sizeof(reason));
        std::printf("%s\n", reason);
        if (!check(status == VA_STATUS_SUCCESS, "vkvv_vulkan_ensure_vp9_session failed")) {
            return false;
        }

        const auto*               typed_session = static_cast<const vkvv::VP9VideoSession*>(session);
        const vkvv::VideoSession& video         = typed_session->video;
        if (!check(video.session != VK_NULL_HANDLE, "VP9 session handle was not created")) {
            return false;
        }
        if (!check(video.key.codec_operation == VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR, "VP9 session key did not record the codec operation")) {
            return false;
        }
        if (!check(video.key.codec_profile == expected_profile, "VP9 session key did not record the expected VP9 profile")) {
            return false;
        }
        if (!check(video.key.picture_format == expected_format && video.key.reference_picture_format == video.key.picture_format,
                   "VP9 session key did not record picture/reference formats")) {
            return false;
        }
        if (!check(video.key.luma_bit_depth == expected_bit_depth && video.key.chroma_bit_depth == expected_bit_depth, "VP9 session key did not record the expected bit depth")) {
            return false;
        }
        if (!check(video.key.max_coded_extent.width >= min_expected_width && video.key.max_coded_extent.height >= min_expected_height,
                   "VP9 session extent is smaller than the requested stream")) {
            std::fprintf(stderr, "expected at least=%ux%u actual=%ux%u\n", min_expected_width, min_expected_height, video.key.max_coded_extent.width,
                         video.key.max_coded_extent.height);
            return false;
        }
        std::printf("%s profile=%u format=%d depth=0x%x\n", label, video.key.codec_profile, video.key.picture_format, video.key.luma_bit_depth);
        return check(video.memory_bytes > 0, "VP9 session memory accounting stayed at zero");
    }

    bool ensure_upload(vkvv::VulkanRuntime* runtime, vkvv::VP9VideoSession* session, const std::vector<uint8_t>& bytes) {
        char reason[512] = {};
        if (!check(vkvv::ensure_bitstream_upload_buffer(runtime, session->profile_spec, bytes.data(), bytes.size(), session->bitstream_size_alignment,
                                                        VK_BUFFER_USAGE_VIDEO_DECODE_SRC_BIT_KHR, &session->upload, "VP9 smoke bitstream", reason, sizeof(reason)),
                   "ensure_bitstream_upload_buffer failed")) {
            std::fprintf(stderr, "%s\n", reason);
            return false;
        }
        return check(session->upload.buffer != VK_NULL_HANDLE && session->upload.memory != VK_NULL_HANDLE && session->upload.size >= bytes.size() &&
                         session->upload.capacity >= session->upload.size,
                     "VP9 upload buffer was not populated correctly");
    }

    bool check_vp9_dpb_slots() {
        vkvv::VP9VideoSession session{};
        bool                  used_slots[vkvv::max_vp9_dpb_slots] = {};

        const int             first_slot = vkvv::allocate_vp9_dpb_slot(&session, used_slots);
        if (!check(first_slot == 0, "first VP9 DPB slot allocation did not start at zero")) {
            return false;
        }
        vkvv::vp9_set_reference_slot(&session, 2, 41, first_slot);
        if (!check(vkvv::vp9_dpb_slot_for_reference_index(&session, 2) == first_slot, "VP9 reference-index lookup did not return the stored slot")) {
            return false;
        }
        if (!check(vkvv::vp9_dpb_slot_for_surface(&session, 41) == first_slot, "VP9 surface lookup did not return the stored slot")) {
            return false;
        }

        used_slots[first_slot] = true;
        const int second_slot  = vkvv::allocate_vp9_dpb_slot(&session, used_slots);
        if (!check(second_slot == 1, "VP9 DPB allocation did not skip a used slot")) {
            return false;
        }
        vkvv::vp9_set_reference_slot(&session, 2, 41, second_slot);
        return check(vkvv::vp9_dpb_slot_for_reference_index(&session, 2) == second_slot, "VP9 reference slot update did not replace the old slot");
    }

} // namespace

int main(void) {
    bool  ok = check_vp9_dpb_slots();

    char  reason[512] = {};
    void* runtime     = vkvv_vulkan_runtime_create(reason, sizeof(reason));
    std::printf("%s\n", reason);
    if (runtime == nullptr) {
        return 1;
    }
    auto* typed_runtime = static_cast<vkvv::VulkanRuntime*>(runtime);
    ok = check((typed_runtime->enabled_decode_operations & VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR) != 0, "runtime did not enable VP9 through codec-driven selection") && ok;
    ok = check(typed_runtime->video_decode_vp9, "runtime did not enable the VP9 decode feature") && ok;

    void* session = vkvv_vulkan_vp9_session_create();
    if (session == nullptr) {
        vkvv_vulkan_runtime_destroy(runtime);
        return 1;
    }

    ok =
        ensure_session(runtime, session, 64, 64, 64, 64, STD_VIDEO_VP9_PROFILE_0, VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, "VP9 Profile0") && ok;
    auto*                typed_session = static_cast<vkvv::VP9VideoSession*>(session);
    std::vector<uint8_t> first_upload(256, 0x11);
    ok                                         = ensure_upload(typed_runtime, typed_session, first_upload) && ok;
    const VkBuffer       first_upload_buffer   = typed_session->upload.buffer;
    const VkDeviceMemory first_upload_memory   = typed_session->upload.memory;
    const VkDeviceSize   first_upload_capacity = typed_session->upload.capacity;

    std::vector<uint8_t> smaller_upload(128, 0x22);
    ok = ensure_upload(typed_runtime, typed_session, smaller_upload) && ok;
    ok =
        check(typed_session->upload.buffer == first_upload_buffer && typed_session->upload.memory == first_upload_memory && typed_session->upload.capacity == first_upload_capacity,
              "smaller VP9 upload did not reuse the existing buffer") &&
        ok;

    ok =
        ensure_session(runtime, session, 640, 360, 640, 368, STD_VIDEO_VP9_PROFILE_0, VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, "VP9 Profile0") &&
        ok;
    const VkVideoSessionKHR grown_session = typed_session->video.session;
    ok =
        ensure_session(runtime, session, 320, 180, 640, 368, STD_VIDEO_VP9_PROFILE_0, VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, "VP9 Profile0") &&
        ok;
    ok = check(typed_session->video.session == grown_session, "VP9 session unexpectedly shrank or recreated") && ok;

    vkvv_vulkan_vp9_session_destroy(runtime, session);
    session = vkvv_vulkan_vp9_profile2_session_create();
    if (session == nullptr) {
        vkvv_vulkan_runtime_destroy(runtime);
        return 1;
    }
    typed_session = static_cast<vkvv::VP9VideoSession*>(session);
    ok = ensure_session(runtime, session, 64, 64, 64, 64, STD_VIDEO_VP9_PROFILE_2, VK_VIDEO_COMPONENT_BIT_DEPTH_10_BIT_KHR, VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16,
                        "VP9 Profile2") &&
        ok;
    std::vector<uint8_t> profile2_upload(256, 0x33);
    ok = ensure_upload(typed_runtime, typed_session, profile2_upload) && ok;
    vkvv_vulkan_vp9_session_destroy(runtime, session);
    vkvv_vulkan_runtime_destroy(runtime);
    if (!ok) {
        return 1;
    }

    std::printf("VP9 session sizing smoke passed\n");
    return 0;
}
