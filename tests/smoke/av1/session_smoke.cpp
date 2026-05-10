#include "vulkan/codecs/av1/api.h"
#include "vulkan/codecs/av1/internal.h"

#include <cstdint>
#include <cstdio>
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

    bool configure_session_for_p010(void* runtime, void* session) {
        VADecPictureParameterBufferAV1 pic{};
        pic.profile = 0;

        VkvvAV1DecodeInput input{};
        input.pic       = &pic;
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

    bool check_av1_dpb_slots() {
        vkvv::AV1VideoSession session{};
        bool                  used_slots[vkvv::max_av1_dpb_slots] = {};

        const int             first_slot = vkvv::allocate_av1_dpb_slot(&session, used_slots);
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

    bool check_av1_target_slot_selection() {
        vkvv::AV1VideoSession session{};
        session.max_dpb_slots                    = vkvv::max_av1_dpb_slots;
        bool used_slots[vkvv::max_av1_dpb_slots] = {};
        for (bool& used : used_slots) {
            used = true;
        }

        const int display_only_full_slot = vkvv::av1_select_current_setup_slot(&session, 77, used_slots, false);
        if (!check(display_only_full_slot == -1, "display-only AV1 frame claimed a DPB setup slot")) {
            return false;
        }

        const int full_slot = vkvv::av1_select_current_setup_slot(&session, 77, used_slots, true);
        if (!check(full_slot == -1, "AV1 target selection claimed a DPB setup slot under full slot pressure")) {
            return false;
        }

        used_slots[4]               = false;
        const int display_only_slot = vkvv::av1_select_current_setup_slot(&session, 77, used_slots, false);
        if (!check(display_only_slot == 4, "display-only AV1 frame did not select an available scratch setup slot")) {
            return false;
        }

        used_slots[display_only_slot] = true;
        used_slots[5]                 = false;
        const int reference_slot      = vkvv::av1_select_current_setup_slot(&session, 77, used_slots, true);
        if (!check(reference_slot == 5, "reference AV1 frame did not select the available DPB setup slot")) {
            return false;
        }

        vkvv::av1_set_surface_slot(&session, 77, reference_slot, {});
        for (bool& used : used_slots) {
            used = false;
        }
        used_slots[reference_slot] = true;
        const int conflicting_slot = vkvv::av1_select_current_setup_slot(&session, 77, used_slots, true);
        return check(conflicting_slot != reference_slot, "AV1 target slot selection reused an already-used target slot");
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
        ok                          = check(vkvv::av1_decode_needs_export_refresh(&input), "showable AV1 hidden frame did not request export shadow refresh") && ok;

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
    bool ok = check_av1_dpb_slots();
    ok      = check_av1_refresh_retention() && ok;
    ok      = check_av1_surface_reconciliation() && ok;
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
