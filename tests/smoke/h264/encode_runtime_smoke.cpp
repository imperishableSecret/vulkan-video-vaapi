#include "va/driver.h"
#include "vulkan/runtime_internal.h"

#include <cstdlib>
#include <cstdio>

namespace {

    bool check(bool condition, const char* message) {
        if (!condition) {
            std::fprintf(stderr, "%s\n", message);
        }
        return condition;
    }

    bool create_runtime_with_gate(const char* gate_value, void** runtime_out, char* reason, size_t reason_size) {
        if (gate_value != nullptr) {
            setenv("VKVV_ENABLE_ENCODE", gate_value, 1);
        } else {
            unsetenv("VKVV_ENABLE_ENCODE");
        }
        *runtime_out = vkvv_vulkan_runtime_create(reason, reason_size);
        std::printf("%s\n", reason);
        return check(*runtime_out != nullptr, "runtime creation failed");
    }

} // namespace

int main(void) {
    unsetenv("VKVV_ASSUME_CAPS");

    bool  ok = true;
    char  reason[512]{};
    void* runtime = nullptr;

    ok = create_runtime_with_gate(nullptr, &runtime, reason, sizeof(reason)) && ok;
    if (runtime == nullptr) {
        return 1;
    }
    auto* gate_off_runtime = static_cast<vkvv::VulkanRuntime*>(runtime);
    ok                     = check(gate_off_runtime->enabled_encode_operations == 0 && gate_off_runtime->probed_encode_operations == 0,
                                   "encode operations must stay empty when the runtime gate is off") &&
        ok;
    ok = check(gate_off_runtime->encode_queue_family == vkvv::invalid_queue_family && gate_off_runtime->encode_queue == VK_NULL_HANDLE,
               "encode queue must stay unset when the runtime gate is off") &&
        ok;
    ok = check((gate_off_runtime->enabled_decode_operations & VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR) != 0, "decode operations changed while encode gate was off") && ok;
    vkvv_vulkan_runtime_destroy(runtime);

    VkvvVideoCaps caps{};
    const bool    probe_ok = vkvv_probe_vulkan_video(&caps);
    ok                     = check(probe_ok, "Vulkan Video probe failed before encode-gated runtime smoke") && ok;

    runtime = nullptr;
    ok      = create_runtime_with_gate("1", &runtime, reason, sizeof(reason)) && ok;
    if (runtime == nullptr) {
        return 1;
    }

    auto* gate_on_runtime = static_cast<vkvv::VulkanRuntime*>(runtime);
    ok = check((gate_on_runtime->enabled_decode_operations & VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR) != 0, "decode operations changed while encode gate was on") && ok;

    if (caps.h264_encode) {
        ok = check((gate_on_runtime->probed_encode_operations & VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR) != 0, "runtime did not probe H.264 encode with gate on") && ok;
        ok = check((gate_on_runtime->enabled_encode_operations & VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR) != 0, "runtime did not enable H.264 encode with gate on") && ok;
        ok = check(gate_on_runtime->encode_queue_family != vkvv::invalid_queue_family && gate_on_runtime->encode_queue != VK_NULL_HANDLE,
                   "runtime did not create an H.264 encode queue with gate on") &&
            ok;
        ok = check(gate_on_runtime->cmd_encode_video != nullptr, "vkCmdEncodeVideoKHR was not loaded") && ok;
        ok = check(gate_on_runtime->get_encoded_video_session_parameters != nullptr, "vkGetEncodedVideoSessionParametersKHR was not loaded") && ok;
        ok = check(gate_on_runtime->get_video_encode_quality_level_properties != nullptr, "vkGetPhysicalDeviceVideoEncodeQualityLevelPropertiesKHR was not loaded") && ok;
    } else {
        ok = check(gate_on_runtime->enabled_encode_operations == 0, "runtime enabled encode despite absent H.264 encode probe support") && ok;
    }

    vkvv_vulkan_runtime_destroy(runtime);
    unsetenv("VKVV_ENABLE_ENCODE");

    if (!ok) {
        return 1;
    }
    std::printf("H.264 encode runtime smoke passed\n");
    return 0;
}
