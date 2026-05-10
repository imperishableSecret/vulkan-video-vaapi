#include "vulkan/codecs/h264/api.h"
#include "vulkan/codecs/h264/internal.h"

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

    template <typename Handle>
    Handle fake_handle(uintptr_t value) {
#if defined(VK_USE_64_BIT_PTR_DEFINES) && VK_USE_64_BIT_PTR_DEFINES
        return reinterpret_cast<Handle>(value);
#else
        return static_cast<Handle>(value);
#endif
    }

    bool ensure_session(void* runtime, void* session, unsigned int width, unsigned int height, unsigned int expected_width, unsigned int expected_height) {
        char     reason[512] = {};
        VAStatus status      = vkvv_vulkan_ensure_h264_session(runtime, session, width, height, reason, sizeof(reason));
        std::printf("%s\n", reason);
        if (!check(status == VA_STATUS_SUCCESS, "vkvv_vulkan_ensure_h264_session failed")) {
            return false;
        }

        const auto*               typed_session = static_cast<const vkvv::H264VideoSession*>(session);
        const vkvv::VideoSession& video         = typed_session->video;
        if (!check(video.session != VK_NULL_HANDLE, "H.264 session handle was not created")) {
            return false;
        }
        if (!check(video.key.codec_operation == VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR, "H.264 session key did not record the codec operation")) {
            return false;
        }
        if (!check(video.key.picture_format != VK_FORMAT_UNDEFINED && video.key.reference_picture_format == video.key.picture_format,
                   "H.264 session key did not record picture/reference formats")) {
            return false;
        }
        if (!check(video.key.image_usage != 0, "H.264 session key did not record decode image usage")) {
            return false;
        }
        if (!check(video.memory_bytes > 0, "H.264 session memory accounting stayed at zero")) {
            return false;
        }
        if (!check(video.key.max_coded_extent.width == expected_width && video.key.max_coded_extent.height == expected_height,
                   "H.264 session extent did not match the stream-sized policy")) {
            std::fprintf(stderr, "expected=%ux%u actual=%ux%u\n", expected_width, expected_height, video.key.max_coded_extent.width, video.key.max_coded_extent.height);
            return false;
        }
        return true;
    }

    bool ensure_upload(vkvv::VulkanRuntime* runtime, vkvv::H264VideoSession* session, const std::vector<uint8_t>& bytes) {
        char reason[512] = {};
        if (!check(vkvv::ensure_bitstream_upload_buffer(runtime, vkvv::h264_profile_spec, bytes.data(), bytes.size(), session->bitstream_size_alignment,
                                                        VK_BUFFER_USAGE_VIDEO_DECODE_SRC_BIT_KHR, &session->uploads[0], "H.264 smoke bitstream", reason, sizeof(reason)),
                   "ensure_bitstream_upload_buffer failed")) {
            std::fprintf(stderr, "%s\n", reason);
            return false;
        }
        if (!check(session->uploads[0].buffer != VK_NULL_HANDLE && session->uploads[0].memory != VK_NULL_HANDLE && session->uploads[0].size >= bytes.size() &&
                       session->uploads[0].capacity >= session->uploads[0].size && session->uploads[0].mapped != nullptr,
                   "upload buffer was not populated correctly")) {
            return false;
        }
        return true;
    }

    bool check_upload_contents(const vkvv::UploadBuffer& upload, const std::vector<uint8_t>& bytes, const char* label) {
        if (!check(upload.mapped != nullptr, "upload buffer is not persistently mapped")) {
            return false;
        }
        const auto* mapped = static_cast<const uint8_t*>(upload.mapped);
        if (!check(std::memcmp(mapped, bytes.data(), bytes.size()) == 0, label)) {
            return false;
        }
        for (size_t i = bytes.size(); i < static_cast<size_t>(upload.size); i++) {
            if (mapped[i] != 0) {
                std::fprintf(stderr, "%s padding byte %zu was not zero: 0x%02x\n", label, i, mapped[i]);
                return false;
            }
        }
        return true;
    }

    bool check_null_runtime_upload_destroy_clears_state() {
        vkvv::UploadBuffer upload{};
        upload.mapped           = reinterpret_cast<void*>(0x1);
        upload.size             = 128;
        upload.capacity         = 128;
        upload.allocation_size  = 256;
        upload.underused_frames = 7;
        vkvv::destroy_upload_buffer(nullptr, &upload);
        return check(upload.mapped == nullptr && upload.size == 0 && upload.capacity == 0 && upload.allocation_size == 0 && upload.underused_frames == 0,
                     "null-runtime upload destroy did not clear stale state");
    }

    bool check_predecode_record_validation() {
        vkvv::ExportResource resource{};
        resource.image              = fake_handle<VkImage>(0x1000);
        resource.memory             = fake_handle<VkDeviceMemory>(0x2000);
        resource.driver_instance_id = 7;
        resource.stream_id          = 8;
        resource.codec_operation    = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
        resource.owner_surface_id   = 9;
        resource.format             = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
        resource.va_fourcc          = VA_FOURCC_NV12;
        resource.extent             = {64, 64};
        resource.predecode_exported = true;

        const vkvv::PredecodeExportRecord record = vkvv::make_predecode_export_record(&resource);
        bool                              ok     = check(vkvv::predecode_export_record_still_valid(record), "fresh predecode export record should validate");
        ok &= check(vkvv::predecode_export_record_matches_resource(record, &resource), "fresh predecode export record should match its resource");

        resource.content_generation = 1;
        ok &= check(!vkvv::predecode_export_record_still_valid(record), "predecode record should reject generation drift");
        resource.content_generation = 0;
        resource.memory             = fake_handle<VkDeviceMemory>(0x3000);
        ok &= check(!vkvv::predecode_export_record_still_valid(record), "predecode record should reject memory replacement");
        return ok;
    }

    bool submit_empty_pending(vkvv::VulkanRuntime* runtime, VkvvSurface* surface, const char* operation, bool refresh_export = true) {
        char reason[512] = {};
        {
            std::lock_guard<std::mutex> command_lock(runtime->command_mutex);
            if (!check(vkvv::ensure_command_resources(runtime, reason, sizeof(reason)), "ensure_command_resources failed")) {
                std::fprintf(stderr, "%s\n", reason);
                return false;
            }

            VkResult result = vkResetFences(runtime->device, 1, &runtime->fence);
            if (!check(result == VK_SUCCESS, "vkResetFences failed for pending smoke")) {
                return false;
            }
            result = vkResetCommandBuffer(runtime->command_buffer, 0);
            if (!check(result == VK_SUCCESS, "vkResetCommandBuffer failed for pending smoke")) {
                return false;
            }

            VkCommandBufferBeginInfo begin_info{};
            begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            result           = vkBeginCommandBuffer(runtime->command_buffer, &begin_info);
            if (!check(result == VK_SUCCESS, "vkBeginCommandBuffer failed for pending smoke")) {
                return false;
            }
            result = vkEndCommandBuffer(runtime->command_buffer);
            if (!check(result == VK_SUCCESS, "vkEndCommandBuffer failed for pending smoke")) {
                return false;
            }
            if (!check(vkvv::submit_command_buffer(runtime, reason, sizeof(reason), operation), "submit_command_buffer failed for pending smoke")) {
                std::fprintf(stderr, "%s\n", reason);
                return false;
            }
            vkvv::track_pending_decode(runtime, surface, VK_NULL_HANDLE, 0, refresh_export, operation);
        }
        return true;
    }

    bool check_async_completion(vkvv::VulkanRuntime* runtime) {
        VkvvSurface surface{};
        surface.work_state  = VKVV_SURFACE_WORK_RENDERING;
        surface.sync_status = VA_STATUS_ERROR_TIMEDOUT;

        if (!submit_empty_pending(runtime, &surface, "async smoke")) {
            return false;
        }

        char     reason[512] = {};
        VAStatus status      = vkvv_vulkan_complete_surface_work(runtime, &surface, VA_TIMEOUT_INFINITE, reason, sizeof(reason));
        if (!check(status == VA_STATUS_SUCCESS, "async surface completion failed")) {
            std::fprintf(stderr, "%s\n", reason);
            return false;
        }
        return check(surface.work_state == VKVV_SURFACE_WORK_READY && surface.sync_status == VA_STATUS_SUCCESS && surface.decoded,
                     "async surface completion did not mark the surface ready");
    }

    bool check_two_pending_surfaces(vkvv::VulkanRuntime* runtime) {
        VkvvSurface first{};
        first.work_state  = VKVV_SURFACE_WORK_RENDERING;
        first.sync_status = VA_STATUS_ERROR_TIMEDOUT;
        VkvvSurface second{};
        second.work_state  = VKVV_SURFACE_WORK_RENDERING;
        second.sync_status = VA_STATUS_ERROR_TIMEDOUT;

        if (!submit_empty_pending(runtime, &first, "first pending smoke") || !submit_empty_pending(runtime, &second, "second pending smoke")) {
            return false;
        }
        if (!check(vkvv::runtime_pending_work_count(runtime) == 2 && vkvv::runtime_surface_has_pending_work(runtime, &first) &&
                       vkvv::runtime_surface_has_pending_work(runtime, &second),
                   "runtime did not keep two pending command slots")) {
            return false;
        }

        char     reason[512] = {};
        VAStatus status      = vkvv_vulkan_complete_surface_work(runtime, &first, VA_TIMEOUT_INFINITE, reason, sizeof(reason));
        if (!check(status == VA_STATUS_SUCCESS, "first pending surface completion failed")) {
            std::fprintf(stderr, "%s\n", reason);
            return false;
        }
        if (!check(vkvv::runtime_pending_work_count(runtime) == 1 && !vkvv::runtime_surface_has_pending_work(runtime, &first) &&
                       vkvv::runtime_surface_has_pending_work(runtime, &second) && second.work_state == VKVV_SURFACE_WORK_RENDERING,
                   "completing one surface drained or disturbed another pending slot")) {
            return false;
        }

        status = vkvv_vulkan_complete_surface_work(runtime, &second, VA_TIMEOUT_INFINITE, reason, sizeof(reason));
        if (!check(status == VA_STATUS_SUCCESS, "second pending surface completion failed")) {
            std::fprintf(stderr, "%s\n", reason);
            return false;
        }
        return check(vkvv::runtime_pending_work_count(runtime) == 0 && first.decoded && second.decoded && second.work_state == VKVV_SURFACE_WORK_READY,
                     "pending ring did not drain deterministically");
    }

    bool check_pending_export_refresh_tracking(vkvv::VulkanRuntime* runtime) {
        VkvvSurface export_refresh{};
        export_refresh.work_state  = VKVV_SURFACE_WORK_RENDERING;
        export_refresh.sync_status = VA_STATUS_ERROR_TIMEDOUT;
        VkvvSurface nondisplay{};
        nondisplay.work_state  = VKVV_SURFACE_WORK_RENDERING;
        nondisplay.sync_status = VA_STATUS_ERROR_TIMEDOUT;

        if (!submit_empty_pending(runtime, &export_refresh, "export-refresh pending smoke", true) ||
            !submit_empty_pending(runtime, &nondisplay, "nondisplay pending smoke", false)) {
            return false;
        }
        if (!check(vkvv_vulkan_surface_has_pending_export_refresh_work(runtime, &export_refresh), "export-refresh pending surface was not tracked")) {
            return false;
        }
        if (!check(!vkvv_vulkan_surface_has_pending_export_refresh_work(runtime, &nondisplay), "non-display pending surface was tracked for export refresh")) {
            return false;
        }

        char     reason[512] = {};
        VAStatus status      = vkvv_vulkan_complete_surface_work(runtime, &export_refresh, VA_TIMEOUT_INFINITE, reason, sizeof(reason));
        if (!check(status == VA_STATUS_SUCCESS, "export-refresh pending tracking completion failed")) {
            std::fprintf(stderr, "%s\n", reason);
            return false;
        }
        status = vkvv_vulkan_complete_surface_work(runtime, &nondisplay, VA_TIMEOUT_INFINITE, reason, sizeof(reason));
        if (!check(status == VA_STATUS_SUCCESS, "non-display pending tracking completion failed")) {
            std::fprintf(stderr, "%s\n", reason);
            return false;
        }
        return true;
    }

    bool submit_upload_pending(vkvv::VulkanRuntime* runtime, vkvv::H264VideoSession* session, VkvvSurface* surface, const std::vector<uint8_t>& bytes, const char* operation,
                               size_t* slot_index) {
        char reason[512] = {};
        {
            std::lock_guard<std::mutex> command_lock(runtime->command_mutex);
            if (!check(vkvv::ensure_command_resources(runtime, reason, sizeof(reason)), "ensure_command_resources failed for upload slot smoke")) {
                std::fprintf(stderr, "%s\n", reason);
                return false;
            }
            *slot_index = runtime->active_command_slot;
            if (!check(vkvv::ensure_bitstream_upload_buffer(runtime, vkvv::h264_profile_spec, bytes.data(), bytes.size(), session->bitstream_size_alignment,
                                                            VK_BUFFER_USAGE_VIDEO_DECODE_SRC_BIT_KHR, &session->uploads[*slot_index], operation, reason, sizeof(reason)),
                       "per-slot upload allocation failed")) {
                std::fprintf(stderr, "%s\n", reason);
                return false;
            }

            VkResult result = vkResetFences(runtime->device, 1, &runtime->fence);
            if (!check(result == VK_SUCCESS, "vkResetFences failed for upload slot smoke")) {
                return false;
            }
            result = vkResetCommandBuffer(runtime->command_buffer, 0);
            if (!check(result == VK_SUCCESS, "vkResetCommandBuffer failed for upload slot smoke")) {
                return false;
            }

            VkCommandBufferBeginInfo begin_info{};
            begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            result           = vkBeginCommandBuffer(runtime->command_buffer, &begin_info);
            if (!check(result == VK_SUCCESS, "vkBeginCommandBuffer failed for upload slot smoke")) {
                return false;
            }
            result = vkEndCommandBuffer(runtime->command_buffer);
            if (!check(result == VK_SUCCESS, "vkEndCommandBuffer failed for upload slot smoke")) {
                return false;
            }
            if (!check(vkvv::submit_command_buffer(runtime, reason, sizeof(reason), operation), "submit_command_buffer failed for upload slot smoke")) {
                std::fprintf(stderr, "%s\n", reason);
                return false;
            }
            vkvv::track_pending_decode(runtime, surface, VK_NULL_HANDLE, session->uploads[*slot_index].allocation_size, true, operation);
        }
        return true;
    }

    bool check_pending_upload_slots_are_independent(vkvv::VulkanRuntime* runtime, vkvv::H264VideoSession* session) {
        VkvvSurface first{};
        first.work_state  = VKVV_SURFACE_WORK_RENDERING;
        first.sync_status = VA_STATUS_ERROR_TIMEDOUT;
        VkvvSurface second{};
        second.work_state  = VKVV_SURFACE_WORK_RENDERING;
        second.sync_status = VA_STATUS_ERROR_TIMEDOUT;

        const std::vector<uint8_t> first_bytes(64, 0xa5);
        const std::vector<uint8_t> second_bytes(64, 0x5a);
        size_t                     first_slot  = 0;
        size_t                     second_slot = 0;
        if (!submit_upload_pending(runtime, session, &first, first_bytes, "first upload slot smoke", &first_slot) ||
            !submit_upload_pending(runtime, session, &second, second_bytes, "second upload slot smoke", &second_slot)) {
            return false;
        }
        if (!check(first_slot != second_slot, "pending uploads reused the same command slot")) {
            return false;
        }
        if (!check_upload_contents(session->uploads[first_slot], first_bytes, "first pending upload was overwritten") ||
            !check_upload_contents(session->uploads[second_slot], second_bytes, "second pending upload contents mismatch")) {
            return false;
        }

        char     reason[512] = {};
        VAStatus status      = vkvv_vulkan_complete_surface_work(runtime, &first, VA_TIMEOUT_INFINITE, reason, sizeof(reason));
        if (!check(status == VA_STATUS_SUCCESS, "first upload pending completion failed")) {
            std::fprintf(stderr, "%s\n", reason);
            return false;
        }
        status = vkvv_vulkan_complete_surface_work(runtime, &second, VA_TIMEOUT_INFINITE, reason, sizeof(reason));
        if (!check(status == VA_STATUS_SUCCESS, "second upload pending completion failed")) {
            std::fprintf(stderr, "%s\n", reason);
            return false;
        }
        return true;
    }

    bool check_full_pending_ring_backpressures(vkvv::VulkanRuntime* runtime) {
        std::vector<VkvvSurface> surfaces(vkvv::command_slot_count + 1);
        for (size_t i = 0; i < vkvv::command_slot_count; i++) {
            surfaces[i].id          = static_cast<VASurfaceID>(700 + i);
            surfaces[i].work_state  = VKVV_SURFACE_WORK_RENDERING;
            surfaces[i].sync_status = VA_STATUS_ERROR_TIMEDOUT;
            char operation[64]{};
            std::snprintf(operation, sizeof(operation), "full ring smoke %zu", i);
            if (!submit_empty_pending(runtime, &surfaces[i], operation)) {
                return false;
            }
        }
        if (!check(vkvv::runtime_pending_work_count(runtime) == vkvv::command_slot_count, "pending ring smoke did not fill all command slots")) {
            return false;
        }

        char     reason[512] = {};
        VAStatus status      = vkvv::ensure_command_slot_capacity(runtime, "full pending ring smoke", reason, sizeof(reason));
        if (!check(status == VA_STATUS_SUCCESS, "full pending ring backpressure failed")) {
            std::fprintf(stderr, "%s\n", reason);
            return false;
        }
        if (!check(vkvv::runtime_pending_work_count(runtime) == vkvv::command_slot_count - 1, "backpressure did not free exactly one command slot")) {
            return false;
        }

        size_t completed_count = 0;
        for (size_t i = 0; i < vkvv::command_slot_count; i++) {
            if (surfaces[i].decoded && surfaces[i].work_state == VKVV_SURFACE_WORK_READY && surfaces[i].sync_status == VA_STATUS_SUCCESS) {
                completed_count++;
            }
        }
        if (!check(completed_count == 1, "backpressure did not complete one pending surface")) {
            return false;
        }

        VkvvSurface& extra = surfaces.back();
        extra.id           = 800;
        extra.work_state   = VKVV_SURFACE_WORK_RENDERING;
        extra.sync_status  = VA_STATUS_ERROR_TIMEDOUT;
        if (!submit_empty_pending(runtime, &extra, "post-backpressure pending smoke")) {
            return false;
        }
        if (!check(vkvv::runtime_pending_work_count(runtime) == vkvv::command_slot_count, "post-backpressure submit did not reuse the freed command slot")) {
            return false;
        }

        status = vkvv::drain_pending_work_before_sync_command(runtime, reason, sizeof(reason));
        if (!check(status == VA_STATUS_SUCCESS, "full pending ring drain failed")) {
            std::fprintf(stderr, "%s\n", reason);
            return false;
        }
        return check(vkvv::runtime_pending_work_count(runtime) == 0, "full pending ring did not drain after backpressure smoke");
    }

    bool check_public_drain_completes_all_pending_work(vkvv::VulkanRuntime* runtime) {
        VkvvSurface first{};
        first.id          = 850;
        first.work_state  = VKVV_SURFACE_WORK_RENDERING;
        first.sync_status = VA_STATUS_ERROR_TIMEDOUT;
        VkvvSurface second{};
        second.id          = 851;
        second.work_state  = VKVV_SURFACE_WORK_RENDERING;
        second.sync_status = VA_STATUS_ERROR_TIMEDOUT;

        if (!submit_empty_pending(runtime, &first, "public drain first smoke") || !submit_empty_pending(runtime, &second, "public drain second smoke")) {
            return false;
        }
        char     reason[512] = {};
        VAStatus status      = vkvv_vulkan_drain_pending_work(runtime, reason, sizeof(reason));
        if (!check(status == VA_STATUS_SUCCESS, "public pending-work drain failed")) {
            std::fprintf(stderr, "%s\n", reason);
            return false;
        }
        return check(vkvv::runtime_pending_work_count(runtime) == 0 && first.work_state == VKVV_SURFACE_WORK_READY && second.work_state == VKVV_SURFACE_WORK_READY &&
                         first.sync_status == VA_STATUS_SUCCESS && second.sync_status == VA_STATUS_SUCCESS && first.decoded && second.decoded,
                     "public pending-work drain did not complete every pending slot");
    }

    bool check_runtime_destroy_drains_pending_work() {
        char  reason[512] = {};
        void* runtime_ptr = vkvv_vulkan_runtime_create(reason, sizeof(reason));
        std::printf("%s\n", reason);
        if (!check(runtime_ptr != nullptr, "runtime create failed for teardown drain smoke")) {
            return false;
        }
        auto*       runtime = static_cast<vkvv::VulkanRuntime*>(runtime_ptr);

        VkvvSurface surface{};
        surface.id          = 852;
        surface.work_state  = VKVV_SURFACE_WORK_RENDERING;
        surface.sync_status = VA_STATUS_ERROR_TIMEDOUT;
        if (!submit_empty_pending(runtime, &surface, "runtime teardown pending smoke")) {
            vkvv_vulkan_runtime_destroy(runtime_ptr);
            return false;
        }

        vkvv_vulkan_runtime_destroy(runtime_ptr);
        return check(surface.work_state == VKVV_SURFACE_WORK_READY && surface.sync_status == VA_STATUS_SUCCESS && surface.decoded,
                     "runtime teardown did not drain pending work before destroying command resources");
    }

    bool check_device_lost_fast_fail(vkvv::VulkanRuntime* runtime) {
        VkvvSurface surface{};
        surface.work_state  = VKVV_SURFACE_WORK_RENDERING;
        surface.sync_status = VA_STATUS_ERROR_TIMEDOUT;

        if (!submit_empty_pending(runtime, &surface, "device-lost smoke")) {
            return false;
        }

        char reason[512]     = {};
        runtime->device_lost = true;
        if (!check(!vkvv::ensure_command_resources(runtime, reason, sizeof(reason)), "device-lost runtime accepted new command resources")) {
            return false;
        }

        VAStatus status = vkvv_vulkan_complete_surface_work(runtime, &surface, VA_TIMEOUT_INFINITE, reason, sizeof(reason));
        if (!check(status == VA_STATUS_ERROR_OPERATION_FAILED, "device-lost pending work did not fail fast")) {
            std::fprintf(stderr, "%s\n", reason);
            return false;
        }
        return check(vkvv::runtime_pending_work_count(runtime) == 0 && runtime->pending_surface == nullptr && surface.work_state == VKVV_SURFACE_WORK_READY &&
                         surface.sync_status == VA_STATUS_ERROR_OPERATION_FAILED,
                     "device-lost pending work was not cleared deterministically");
    }

    bool check_h264_dpb_slots() {
        vkvv::H264VideoSession session{};
        bool                   used_slots[vkvv::max_h264_dpb_slots] = {};

        const int              first_slot = vkvv::allocate_dpb_slot(&session, used_slots);
        if (!check(first_slot == 0, "first H.264 DPB slot allocation did not start at zero")) {
            return false;
        }
        vkvv::h264_set_dpb_slot_for_surface(&session, 41, first_slot);
        if (!check(vkvv::h264_dpb_slot_for_surface(&session, 41) == first_slot, "H.264 DPB slot lookup did not return the stored surface slot")) {
            return false;
        }

        used_slots[first_slot] = true;
        const int second_slot  = vkvv::allocate_dpb_slot(&session, used_slots);
        if (!check(second_slot == 1, "H.264 DPB slot allocation did not skip a used slot")) {
            return false;
        }
        vkvv::h264_set_dpb_slot_for_surface(&session, 41, second_slot);
        return check(vkvv::h264_dpb_slot_for_surface(&session, 41) == second_slot, "H.264 DPB slot update did not replace the old surface slot");
    }

} // namespace

int main(void) {
    bool ok = check_h264_dpb_slots();
    ok      = check_null_runtime_upload_destroy_clears_state() && ok;
    ok      = check_predecode_record_validation() && ok;

    char  reason[512] = {};
    void* runtime     = vkvv_vulkan_runtime_create(reason, sizeof(reason));
    std::printf("%s\n", reason);
    if (runtime == nullptr) {
        return 1;
    }
    auto* typed_runtime = static_cast<vkvv::VulkanRuntime*>(runtime);
    ok                  = check(typed_runtime->decode_queue_family != vkvv::invalid_queue_family, "runtime did not select a decode queue family");
    ok = check((typed_runtime->enabled_decode_operations & VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR) != 0, "runtime did not enable H.264 through codec-driven selection") && ok;
    ok = check((typed_runtime->probed_decode_operations & VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR) != 0, "runtime did not record probed H.264 decode support") && ok;
    ok = check(typed_runtime->enabled_encode_operations == 0 && typed_runtime->probed_encode_operations == 0,
               "runtime should keep encode operation sets empty before encode probing is wired") &&
        ok;

    void* session = vkvv_vulkan_h264_session_create();
    if (session == nullptr) {
        vkvv_vulkan_runtime_destroy(runtime);
        return 1;
    }

    ok                                 = ensure_session(runtime, session, 64, 64, 256, 256) && ok;
    auto*                typed_session = static_cast<vkvv::H264VideoSession*>(session);
    std::vector<uint8_t> first_upload(256, 0x11);
    ok                                           = ensure_upload(typed_runtime, typed_session, first_upload) && ok;
    ok                                           = check_upload_contents(typed_session->uploads[0], first_upload, "first H.264 upload contents mismatch") && ok;
    const VkBuffer       first_upload_buffer     = typed_session->uploads[0].buffer;
    const VkDeviceMemory first_upload_memory     = typed_session->uploads[0].memory;
    void*                first_upload_mapping    = typed_session->uploads[0].mapped;
    const VkDeviceSize   first_upload_capacity   = typed_session->uploads[0].capacity;
    const VkDeviceSize   first_upload_allocation = typed_session->uploads[0].allocation_size;

    std::vector<uint8_t> smaller_upload(128, 0x22);
    ok = ensure_upload(typed_runtime, typed_session, smaller_upload) && ok;
    ok = check_upload_contents(typed_session->uploads[0], smaller_upload, "smaller H.264 upload contents mismatch") && ok;
    ok = check(typed_session->uploads[0].buffer == first_upload_buffer && typed_session->uploads[0].memory == first_upload_memory &&
                   typed_session->uploads[0].mapped == first_upload_mapping && typed_session->uploads[0].capacity == first_upload_capacity &&
                   typed_session->uploads[0].allocation_size == first_upload_allocation,
               "smaller H.264 upload did not reuse the existing buffer") &&
        ok;

    std::vector<uint8_t> larger_upload(static_cast<size_t>((first_upload_capacity * 8) + 1), 0x33);
    ok                                         = ensure_upload(typed_runtime, typed_session, larger_upload) && ok;
    ok                                         = check_upload_contents(typed_session->uploads[0], larger_upload, "larger H.264 upload contents mismatch") && ok;
    ok                                         = check(typed_session->uploads[0].capacity > first_upload_capacity, "larger H.264 upload did not grow the reusable buffer") && ok;
    const VkDeviceSize   grown_upload_capacity = typed_session->uploads[0].capacity;

    std::vector<uint8_t> tiny_upload(64, 0x44);
    for (uint32_t i = 0; i < 64; i++) {
        ok = ensure_upload(typed_runtime, typed_session, tiny_upload) && ok;
        ok = check_upload_contents(typed_session->uploads[0], tiny_upload, "tiny H.264 upload contents mismatch") && ok;
    }
    ok = check(typed_session->uploads[0].capacity < grown_upload_capacity && typed_session->uploads[0].capacity >= typed_session->uploads[0].size,
               "sustained tiny H.264 uploads did not shrink the overgrown upload buffer") &&
        ok;
    ok = check_async_completion(typed_runtime) && ok;
    ok = check_two_pending_surfaces(typed_runtime) && ok;
    ok = check_pending_export_refresh_tracking(typed_runtime) && ok;
    ok = check_pending_upload_slots_are_independent(typed_runtime, typed_session) && ok;
    ok = check_full_pending_ring_backpressures(typed_runtime) && ok;
    ok = check_public_drain_completes_all_pending_work(typed_runtime) && ok;
    ok = check_runtime_destroy_drains_pending_work() && ok;

    ok                                    = ensure_session(runtime, session, 640, 360, 640, 368) && ok;
    const VkVideoSessionKHR grown_session = typed_session->video.session;
    ok                                    = check(grown_session != VK_NULL_HANDLE, "grown H.264 session handle was not created") && ok;

    ok = ensure_session(runtime, session, 320, 180, 640, 368) && ok;
    ok = check(typed_session->video.session == grown_session, "H.264 session unexpectedly shrank or recreated") && ok;

    ok = check_device_lost_fast_fail(typed_runtime) && ok;

    vkvv_vulkan_h264_session_destroy(runtime, session);
    vkvv_vulkan_runtime_destroy(runtime);
    if (!ok) {
        return 1;
    }

    std::printf("H.264 session sizing smoke passed\n");
    return 0;
}
