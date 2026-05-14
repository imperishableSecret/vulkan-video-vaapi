#include "vulkan/runtime_internal.h"
#include "va/surface_import.h"

#include <cstdint>
#include <cstdio>
#include <string_view>

namespace vkvv {
    // This smoke links only the retained-export helpers. Its fake runtime never owns Vulkan handles,
    // so the default destructor is enough here without pulling in the full runtime resource module.
    VulkanRuntime::~VulkanRuntime() = default;
} // namespace vkvv

namespace {

    constexpr unsigned int                  fourcc_nv12       = 0x3231564e;
    constexpr unsigned int                  fourcc_p010       = 0x30313050;
    constexpr uint64_t                      modifier_linear   = 0;
    constexpr uint64_t                      modifier_mismatch = 1;
    constexpr uint64_t                      retained_driver   = 2;
    constexpr uint64_t                      retained_stream   = 1;
    constexpr VkVideoCodecOperationFlagsKHR codec_vp9         = VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR;
    constexpr VkVideoCodecOperationFlagsKHR codec_av1         = VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR;

    bool                                    check(bool condition, const char* message) {
        if (!condition) {
            std::fprintf(stderr, "%s\n", message);
        }
        return condition;
    }

    template <typename Handle>
    Handle fake_handle(std::uintptr_t value) {
#if defined(VK_USE_64_BIT_PTR_DEFINES) && VK_USE_64_BIT_PTR_DEFINES
        return reinterpret_cast<Handle>(value);
#else
        return static_cast<Handle>(value);
#endif
    }

    vkvv::RetainedExportBacking make_backing() {
        vkvv::RetainedExportBacking backing{};
        backing.resource.fd_stat_valid           = true;
        backing.resource.fd_dev                  = 100;
        backing.resource.fd_ino                  = 200;
        backing.resource.driver_instance_id      = retained_driver;
        backing.resource.stream_id               = retained_stream;
        backing.resource.codec_operation         = codec_vp9;
        backing.resource.va_fourcc               = fourcc_nv12;
        backing.resource.format                  = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
        backing.resource.extent                  = {1920, 1088};
        backing.resource.allocation_size         = 1920 * 1088 * 3 / 2;
        backing.resource.has_drm_format_modifier = true;
        backing.resource.drm_format_modifier     = modifier_linear;
        backing.fd                               = vkvv::retained_export_fd_identity(backing.resource);
        return backing;
    }

    VkvvExternalSurfaceImport make_import() {
        VkvvExternalSurfaceImport import{};
        import.external                = true;
        import.fd.valid                = true;
        import.fd.dev                  = 100;
        import.fd.ino                  = 200;
        import.fourcc                  = fourcc_nv12;
        import.width                   = 1920;
        import.height                  = 1080;
        import.has_drm_format_modifier = true;
        import.drm_format_modifier     = modifier_linear;
        return import;
    }

    vkvv::TransitionRetentionWindow make_decode_window() {
        vkvv::TransitionRetentionWindow window{};
        window.active             = true;
        window.driver_instance_id = retained_driver;
        window.stream_id          = retained_stream;
        window.codec_operation    = codec_vp9;
        window.format             = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
        window.va_fourcc          = fourcc_nv12;
        window.coded_extent       = {3840, 2160};
        return window;
    }

    vkvv::ExportResource make_window_seed(VkVideoCodecOperationFlagsKHR codec, uint64_t stream_id) {
        vkvv::ExportResource seed{};
        seed.driver_instance_id = retained_driver;
        seed.stream_id          = stream_id;
        seed.codec_operation    = codec;
        seed.owner_surface_id   = 3;
        seed.format             = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
        seed.va_fourcc          = fourcc_nv12;
        seed.extent             = {3840, 2160};
        seed.allocation_size    = 12517376ull;
        return seed;
    }

    bool expect_match(const vkvv::RetainedExportBacking& backing, const VkvvExternalSurfaceImport& import, uint64_t driver_instance_id, uint64_t stream_id,
                      VkVideoCodecOperationFlagsKHR codec_operation, unsigned int fourcc, VkFormat format, VkExtent2D extent, vkvv::RetainedExportMatch expected,
                      const char* message) {
        const vkvv::RetainedExportMatch actual = vkvv::retained_export_match_import(backing, import, driver_instance_id, stream_id, codec_operation, fourcc, format, extent);
        if (actual != expected) {
            std::fprintf(stderr, "%s: expected %s got %s\n", message, vkvv::retained_export_match_reason(expected), vkvv::retained_export_match_reason(actual));
            return false;
        }
        return true;
    }

    VkPhysicalDeviceMemoryProperties memory_properties_with_local_heap(VkDeviceSize size) {
        VkPhysicalDeviceMemoryProperties properties{};
        properties.memoryHeapCount      = 1;
        properties.memoryHeaps[0].size  = size;
        properties.memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
        return properties;
    }

    bool check_dynamic_budget() {
        constexpr VkDeviceSize mib = 1024ull * 1024ull;
        bool                   ok  = true;
        ok &= check(vkvv::retained_export_global_cap_bytes(memory_properties_with_local_heap(16ull * 1024ull * mib)) == 512ull * mib,
                    "16 GiB heap should cap retained exports at 512 MiB");
        ok &= check(vkvv::retained_export_global_cap_bytes(memory_properties_with_local_heap(2ull * 1024ull * mib)) == 128ull * mib,
                    "small heaps should keep the minimum 128 MiB retained export cap");

        const vkvv::RetainedExportBudget low_res = vkvv::retained_export_budget_from_expected(4, 4ull * mib, 512ull * mib);
        ok &= check(low_res.headroom_count == 2, "low-res retained budget should add two headroom backings");
        ok &= check(low_res.target_count == 6, "low-res retained budget target count mismatch");
        ok &= check(low_res.target_bytes == 6ull * mib, "low-res retained budget should scale from actual bytes");

        const vkvv::RetainedExportBudget high_res = vkvv::retained_export_budget_from_expected(24, 288ull * mib, 512ull * mib);
        ok &= check(high_res.headroom_count == 4, "high-res retained budget should cap headroom at four backings");
        ok &= check(high_res.target_count == 28, "high-res retained budget target count mismatch");
        ok &= check(high_res.target_bytes == 336ull * mib, "high-res retained budget target bytes mismatch");

        const vkvv::RetainedExportBudget capped = vkvv::retained_export_budget_from_expected(32, 512ull * mib, 256ull * mib);
        ok &= check(capped.target_bytes == 512ull * mib, "retained budget must not shrink below the observed transition window");

        constexpr VkDeviceSize           four_k_export_bytes = 12517376ull;
        const vkvv::RetainedExportBudget browser_switch      = vkvv::retained_export_budget_from_expected(20, 20 * four_k_export_bytes, 402456576ull);
        ok &= check(browser_switch.headroom_count == 4, "browser switch budget should keep four headroom backings");
        ok &= check(browser_switch.target_count == 24, "browser switch budget should retain the old decode pool");
        ok &= check(browser_switch.target_bytes == 24 * four_k_export_bytes, "browser switch budget should cover the retained 4K decode pool");

        constexpr VkDeviceSize           p010_four_k_export_bytes = 24969216ull;
        const vkvv::RetainedExportBudget p010_over_cap            = vkvv::retained_export_budget_from_expected(17, 17 * p010_four_k_export_bytes, 402456576ull);
        ok &= check(p010_over_cap.headroom_count == 4, "P010 browser switch budget should keep four count slots");
        ok &= check(p010_over_cap.target_count == 21, "P010 browser switch budget should track the observed pool count plus headroom");
        ok &= check(p010_over_cap.target_bytes == 17 * p010_four_k_export_bytes, "P010 budget must not prune already-observed 4K P010 backings");
        return ok;
    }

    bool check_p010_over_cap_window_stays_retained() {
        constexpr size_t                 retained_count     = 17;
        constexpr VkDeviceSize           backing_bytes      = 24969216ull;
        constexpr VkDeviceSize           retained_bytes     = retained_count * backing_bytes;
        constexpr VkDeviceSize           browser_global_cap = 402456576ull;

        const vkvv::RetainedExportBudget budget      = vkvv::retained_export_budget_from_expected(retained_count, retained_bytes, browser_global_cap);
        const bool                       would_prune = retained_count > budget.target_count || retained_bytes > budget.target_bytes;
        return check(!would_prune, "P010 transition window would still prune a matching retained backing");
    }

    bool check_window_policy() {
        bool                                  ok            = true;
        const vkvv::TransitionRetentionWindow decode_window = make_decode_window();

        const vkvv::ExportResource            same_stream_seed = make_window_seed(codec_vp9, 1);
        ok &= check(vkvv::retained_export_matches_window(same_stream_seed, decode_window), "same decode stream should match active retention window");
        ok &= check(vkvv::retained_export_seed_can_replace_window(decode_window, same_stream_seed), "same decode stream should refresh active retention window");

        const vkvv::ExportResource placeholder_seed = make_window_seed(0, 0);
        ok &= check(!vkvv::retained_export_matches_window(placeholder_seed, decode_window), "placeholder seed should not match active decode window");
        ok &= check(!vkvv::retained_export_seed_can_replace_window(decode_window, placeholder_seed), "placeholder seed should not replace active decode window");

        vkvv::ExportResource same_extent_different_codec = make_window_seed(codec_av1, 1);
        ok &= check(!vkvv::retained_export_matches_window(same_extent_different_codec, decode_window), "same extent with different codec should not match retention window");

        vkvv::ExportResource same_surface_different_extent = make_window_seed(codec_vp9, 1);
        same_surface_different_extent.extent               = {1920, 1080};
        ok &= check(!vkvv::retained_export_matches_window(same_surface_different_extent, decode_window), "same stream with different extent should not match retention window");

        vkvv::ExportResource av1_seed = make_window_seed(codec_av1, 2);
        av1_seed.driver_instance_id   = 3;
        ok &= check(vkvv::retained_export_seed_can_replace_window(decode_window, av1_seed), "new decode stream should be allowed to replace active retention window");
        return ok;
    }

    bool check_stats_and_accounting() {
        bool                ok = true;

        vkvv::VulkanRuntime runtime{};
        runtime.retained_export_count_limit              = 7;
        runtime.retained_export_memory_budget            = 42;
        runtime.transition_retention.active              = true;
        runtime.transition_retention.retained_count      = 2;
        runtime.transition_retention.retained_bytes      = 384;
        runtime.transition_retention.budget.target_count = 4;
        runtime.transition_retention.budget.target_bytes = 768;

        vkvv::RetainedExportBacking first  = make_backing();
        first.resource.allocation_size     = 128;
        vkvv::RetainedExportBacking second = make_backing();
        second.resource.fd_ino             = 201;
        second.resource.allocation_size    = 256;
        runtime.retained_exports.push_back(std::move(first));
        runtime.retained_exports.push_back(std::move(second));
        runtime.retained_export_memory_bytes = 384;

        const vkvv::RetainedExportStats stats = vkvv::runtime_retained_export_stats(&runtime);
        ok &= check(stats.count == 2, "retained stats count mismatch");
        ok &= check(stats.bytes == 384, "retained stats byte counter mismatch");
        ok &= check(stats.accounted_bytes == 384, "retained stats accounted bytes mismatch");
        ok &= check(stats.accounting_valid, "retained stats should report valid accounting");
        ok &= check(stats.count_limit == 7, "retained stats count limit mismatch");
        ok &= check(stats.memory_budget == 42, "retained stats memory budget mismatch");
        ok &= check(stats.transition_active, "retained stats transition active flag mismatch");
        ok &= check(stats.transition_retained_count == 2, "retained stats transition count mismatch");
        ok &= check(stats.transition_retained_bytes == 384, "retained stats transition bytes mismatch");
        ok &= check(stats.transition_target_count == 4, "retained stats transition target count mismatch");
        ok &= check(stats.transition_target_bytes == 768, "retained stats transition target bytes mismatch");
        ok &= check(vkvv::runtime_retained_export_accounted_bytes(&runtime) == 384, "retained accounted byte helper mismatch");
        ok &= check(vkvv::runtime_retained_export_memory_accounting_valid(&runtime), "retained accounting helper should pass");

        runtime.retained_export_memory_bytes = 383;
        ok &= check(!vkvv::runtime_retained_export_memory_accounting_valid(&runtime), "retained accounting helper should catch counter drift");
        const vkvv::RetainedExportStats drifted_stats = vkvv::runtime_retained_export_stats(&runtime);
        ok &= check(drifted_stats.accounted_bytes == 384, "drifted retained stats accounted bytes mismatch");
        ok &= check(!drifted_stats.accounting_valid, "drifted retained stats should report invalid accounting");
        return ok;
    }

    bool check_av1_nondisplay_export_state_policy() {
        bool                  ok = true;
        vkvv::SurfaceResource av1_resource{};
        av1_resource.codec_operation                        = codec_av1;
        av1_resource.content_generation                     = 7;
        av1_resource.export_resource.predecode_exported     = true;
        av1_resource.export_resource.predecode_seeded       = true;
        av1_resource.export_resource.black_placeholder      = true;
        av1_resource.export_resource.seed_source_surface_id = 42;
        av1_resource.export_resource.seed_source_generation = 6;

        ok &= check(vkvv::surface_resource_uses_av1_decode(&av1_resource), "AV1 export state helper did not recognize AV1 decode resources");
        ok &= check(vkvv::av1_non_display_export_refresh(&av1_resource, false), "AV1 non-display refresh did not select the no-seed policy");
        ok &= check(!vkvv::av1_non_display_export_refresh(&av1_resource, true), "AV1 visible refresh selected the non-display no-seed policy");

        vkvv::SurfaceResource vp9_resource = av1_resource;
        vp9_resource.codec_operation       = codec_vp9;
        ok &= check(!vkvv::surface_resource_uses_av1_decode(&vp9_resource), "non-AV1 export state helper matched AV1");
        ok &= check(!vkvv::av1_non_display_export_refresh(&vp9_resource, false), "non-AV1 non-display refresh selected the AV1 no-seed policy");

        vkvv::clear_predecode_export_state(&av1_resource.export_resource);
        ok &= check(!av1_resource.export_resource.predecode_exported && !av1_resource.export_resource.predecode_seeded && !av1_resource.export_resource.black_placeholder &&
                        av1_resource.export_resource.seed_source_surface_id == VA_INVALID_ID && av1_resource.export_resource.seed_source_generation == 0,
                    "AV1 non-display predecode state was not fully cleared");
        return ok;
    }

    bool check_av1_visible_export_copy_policy() {
        bool                  ok = true;
        vkvv::SurfaceResource av1_resource{};
        av1_resource.codec_operation                    = codec_av1;
        av1_resource.content_generation                 = 11;
        av1_resource.export_resource.content_generation = 11;

        ok &= check(!vkvv::surface_resource_export_shadow_stale(&av1_resource), "current AV1 shadow was marked stale");
        ok &= check(!vkvv::av1_visible_export_requires_copy(&av1_resource), "current AV1 shadow forced an unnecessary visible copy");

        av1_resource.export_resource.predecode_exported = true;
        ok &= check(vkvv::av1_visible_export_requires_copy(&av1_resource), "AV1 predecode export did not force visible copy");
        av1_resource.export_resource.predecode_exported = false;

        av1_resource.export_resource.predecode_seeded = true;
        ok &= check(vkvv::av1_visible_export_requires_copy(&av1_resource), "AV1 seeded predecode export did not force visible copy");
        av1_resource.export_resource.predecode_seeded = false;

        av1_resource.export_resource.black_placeholder = true;
        ok &= check(vkvv::av1_visible_export_requires_copy(&av1_resource), "AV1 placeholder export did not force visible copy");
        av1_resource.export_resource.black_placeholder = false;

        av1_resource.export_retained_attached = true;
        ok &= check(vkvv::av1_visible_export_requires_copy(&av1_resource), "AV1 retained export attach did not force visible copy");
        vkvv::clear_surface_export_attach_state(&av1_resource);

        av1_resource.export_import_attached = true;
        ok &= check(vkvv::av1_visible_export_requires_copy(&av1_resource), "AV1 imported export attach did not force visible copy");
        vkvv::clear_surface_export_attach_state(&av1_resource);

        av1_resource.export_resource.content_generation = 10;
        ok &= check(vkvv::surface_resource_export_shadow_stale(&av1_resource), "stale AV1 shadow generation was not detected");
        ok &= check(vkvv::av1_visible_export_requires_copy(&av1_resource), "stale AV1 shadow did not force visible copy");

        vkvv::SurfaceResource vp9_resource = av1_resource;
        vp9_resource.codec_operation       = codec_vp9;
        ok &= check(!vkvv::av1_visible_export_requires_copy(&vp9_resource), "non-AV1 stale shadow used the AV1 visible copy policy");
        return ok;
    }

    void mark_direct_import_current(vkvv::SurfaceResource* resource) {
        resource->import.external                = true;
        resource->import.fd.valid                = true;
        resource->import.fd.dev                  = 100;
        resource->import.fd.ino                  = 200;
        resource->direct_import_presentable      = true;
        resource->decode_image_is_imported_image = true;
        resource->import_present_barrier_done    = true;
        resource->import_fd_stat_valid           = true;
        resource->import_present_generation      = resource->content_generation;
        resource->import_fd_dev                  = resource->import.fd.dev;
        resource->import_fd_ino                  = resource->import.fd.ino;
        resource->import_driver_instance_id      = resource->driver_instance_id;
        resource->import_stream_id               = resource->stream_id;
        resource->import_codec_operation         = resource->codec_operation;
    }

    bool check_visible_output_publication_policy() {
        bool                  ok = true;
        vkvv::SurfaceResource resource{};
        resource.driver_instance_id = retained_driver;
        resource.stream_id          = retained_stream;
        resource.codec_operation    = codec_av1;
        resource.content_generation = 12;

        ok &= check(!vkvv::surface_resource_requires_visible_publication(&resource, true), "unexported AV1 resource unexpectedly required visible publication");
        resource.import.external = true;
        ok &= check(vkvv::surface_resource_requires_visible_publication(&resource, true), "external AV1 visible refresh did not require publication");
        ok &= check(!vkvv::surface_resource_requires_visible_publication(&resource, false), "AV1 non-display refresh required visible publication");
        resource.import.external = false;

        ok &= check(!vkvv::surface_resource_has_current_export_shadow(&resource), "empty shadow was treated as current");
        ok &= check(!vkvv::surface_resource_has_published_visible_output(&resource), "empty resource was treated as published");

        resource.export_resource.image              = fake_handle<VkImage>(1);
        resource.export_resource.memory             = fake_handle<VkDeviceMemory>(2);
        resource.export_resource.content_generation = 11;
        ok &= check(!vkvv::surface_resource_has_current_export_shadow(&resource), "stale shadow was treated as current");
        ok &= check(!vkvv::surface_resource_has_published_visible_output(&resource), "stale shadow was treated as published");

        resource.export_resource.content_generation = resource.content_generation;
        ok &= check(vkvv::surface_resource_has_current_export_shadow(&resource), "current shadow was not treated as current");
        ok &= check(!vkvv::surface_resource_has_exported_shadow_output(&resource), "private shadow was treated as exported output");
        ok &= check(!vkvv::surface_resource_has_published_visible_output(&resource), "private shadow was treated as published output");

        resource.exported                 = true;
        resource.export_resource.exported = true;
        ok &= check(vkvv::surface_resource_has_exported_shadow_output(&resource), "exported current shadow was not treated as published output");
        ok &= check(vkvv::surface_resource_has_published_visible_output(&resource), "exported current shadow did not satisfy visible publication");
        resource.exported                 = false;
        resource.export_resource.exported = false;

        resource.export_resource = {};
        mark_direct_import_current(&resource);
        ok &= check(vkvv::surface_resource_has_direct_import_output(&resource), "current direct import was not treated as published output");
        ok &= check(vkvv::surface_resource_has_published_visible_output(&resource), "current direct import did not satisfy visible publication");

        resource.import.fd.ino++;
        ok &= check(!vkvv::surface_resource_has_direct_import_output(&resource), "direct import identity drift was not rejected");
        resource.import.fd.ino = resource.import_fd_ino;

        resource.import_present_generation--;
        ok &= check(!vkvv::surface_resource_has_direct_import_output(&resource), "stale direct import generation was not rejected");
        resource.import_present_generation = resource.content_generation;

        vkvv::clear_surface_direct_import_present_state(&resource);
        ok &= check(!vkvv::surface_resource_has_direct_import_output(&resource), "cleared direct import state still published");

        resource.av1_visible_output_trace_valid    = true;
        resource.av1_visible_show_frame            = true;
        resource.av1_visible_show_existing_frame   = true;
        resource.av1_visible_refresh_frame_flags   = 0xff;
        resource.av1_visible_frame_to_show_map_idx = 3;
        vkvv::clear_surface_av1_visible_output_trace(&resource);
        ok &= check(!resource.av1_visible_output_trace_valid && !resource.av1_visible_show_frame && !resource.av1_visible_show_existing_frame &&
                        resource.av1_visible_refresh_frame_flags == 0 && resource.av1_visible_frame_to_show_map_idx == -1,
                    "AV1 visible output trace state was not fully cleared");

        vkvv::SurfaceResource vp9_resource = resource;
        vp9_resource.codec_operation       = codec_vp9;
        vp9_resource.import.external       = true;
        ok &= check(!vkvv::surface_resource_requires_visible_publication(&vp9_resource, true), "non-AV1 refresh used the AV1 publication invariant");
        return ok;
    }

    bool check_private_decode_shadow_state_policy() {
        bool                  ok = true;
        vkvv::SurfaceResource resource{};
        resource.content_generation                              = 5;
        resource.export_resource.image                           = fake_handle<VkImage>(1);
        resource.export_resource.memory                          = fake_handle<VkDeviceMemory>(2);
        resource.export_resource.content_generation              = 4;
        resource.export_resource.exported                        = true;
        resource.export_resource.present_pinned                  = true;
        resource.export_resource.decode_shadow_generation        = resource.content_generation;
        resource.export_resource.decode_shadow_private_active    = true;
        resource.private_decode_shadow.image                     = fake_handle<VkImage>(3);
        resource.private_decode_shadow.memory                    = fake_handle<VkDeviceMemory>(4);
        resource.private_decode_shadow.content_generation        = resource.content_generation;
        resource.private_decode_shadow.private_nondisplay_shadow = true;

        ok &= check(vkvv::client_present_shadow(&resource) == &resource.export_resource, "client present helper did not select exported shadow");
        ok &= check(vkvv::current_decode_shadow(&resource) == &resource.private_decode_shadow, "private decode shadow was not selected as decode-current");
        ok &= check(vkvv::surface_resource_export_shadow_stale(&resource), "stale client-present shadow was not detected");
        ok &= check(!vkvv::surface_resource_decode_shadow_stale(&resource), "current private decode shadow was treated as stale");
        ok &= check(!vkvv::surface_resource_has_current_export_shadow(&resource), "stale client-present shadow was treated as current export output");

        vkvv::clear_private_decode_shadow_state(&resource);
        resource.export_resource.content_generation = resource.content_generation;
        ok &= check(vkvv::current_decode_shadow(&resource) == &resource.export_resource, "present shadow was not selected after private decode state cleared");
        ok &= check(!resource.export_resource.decode_shadow_private_active, "private decode state did not clear active flag");
        return ok;
    }

} // namespace

int main() {
    const vkvv::RetainedExportBacking backing = make_backing();
    VkvvExternalSurfaceImport         import  = make_import();

    bool                              ok = true;
    ok &= check(backing.fd.valid && backing.fd.dev == 100 && backing.fd.ino == 200, "retained fd identity was not copied from export resource");
    const VkvvExternalImageIdentity retained_key = vkvv::retained_export_image_identity(backing.resource);
    ok &= check(vkvv_fd_identity_equal(retained_key.fd, backing.fd) && retained_key.fourcc == fourcc_nv12 && retained_key.width == 1920 && retained_key.height == 1088 &&
                    retained_key.has_drm_format_modifier && retained_key.drm_format_modifier == modifier_linear,
                "retained export image identity did not preserve key fields");
    ok &= expect_match(backing, import, retained_driver, retained_stream, codec_vp9, fourcc_nv12, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, {1920, 1088},
                       vkvv::RetainedExportMatch::Match, "matching import");

    ok &= expect_match(backing, import, retained_driver + 1, retained_stream, codec_vp9, fourcc_nv12, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, {1920, 1088},
                       vkvv::RetainedExportMatch::DriverMismatch, "same fd with wrong driver");
    ok &= expect_match(backing, import, retained_driver, retained_stream + 1, codec_vp9, fourcc_nv12, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, {1920, 1088},
                       vkvv::RetainedExportMatch::StreamMismatch, "same fd with wrong stream");
    ok &= expect_match(backing, import, retained_driver, retained_stream, codec_av1, fourcc_nv12, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, {1920, 1088},
                       vkvv::RetainedExportMatch::CodecMismatch, "same fd with wrong codec");

    import.external = false;
    ok &= expect_match(backing, import, retained_driver, retained_stream, codec_vp9, fourcc_nv12, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, {1920, 1088},
                       vkvv::RetainedExportMatch::MissingImport, "missing external import");

    import        = make_import();
    import.fd.ino = 201;
    ok &= expect_match(backing, import, retained_driver, retained_stream, codec_vp9, fourcc_nv12, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, {1920, 1088},
                       vkvv::RetainedExportMatch::FdMismatch, "wrong fd identity");

    import = make_import();
    ok &= expect_match(backing, import, retained_driver, retained_stream, codec_vp9, fourcc_p010, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, {1920, 1088},
                       vkvv::RetainedExportMatch::FourccMismatch, "wrong fourcc");

    import        = make_import();
    import.fourcc = fourcc_p010;
    ok &= expect_match(backing, import, retained_driver, retained_stream, codec_vp9, fourcc_nv12, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, {1920, 1088},
                       vkvv::RetainedExportMatch::FourccMismatch, "same fd with mismatched import fourcc");

    import                     = make_import();
    import.drm_format_modifier = modifier_mismatch;
    ok &= expect_match(backing, import, retained_driver, retained_stream, codec_vp9, fourcc_nv12, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, {1920, 1088},
                       vkvv::RetainedExportMatch::ModifierMismatch, "same fd with mismatched modifier");

    import                         = make_import();
    import.has_drm_format_modifier = false;
    ok &= expect_match(backing, import, retained_driver, retained_stream, codec_vp9, fourcc_nv12, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, {1920, 1088},
                       vkvv::RetainedExportMatch::ModifierMismatch, "same fd with missing modifier");

    import = make_import();
    ok &= expect_match(backing, import, retained_driver, retained_stream, codec_vp9, fourcc_nv12, VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16, {1920, 1088},
                       vkvv::RetainedExportMatch::FormatMismatch, "wrong Vulkan format");

    ok &= expect_match(backing, import, retained_driver, retained_stream, codec_vp9, fourcc_nv12, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, {3840, 2160},
                       vkvv::RetainedExportMatch::ExtentMismatch, "undersized retained backing");

    ok &= check(std::string_view(vkvv::retained_export_match_reason(vkvv::RetainedExportMatch::Match)) == "match", "match reason text should be stable");
    ok &= check_dynamic_budget();
    ok &= check_p010_over_cap_window_stays_retained();
    ok &= check_window_policy();
    ok &= check_stats_and_accounting();
    ok &= check_av1_nondisplay_export_state_policy();
    ok &= check_av1_visible_export_copy_policy();
    ok &= check_visible_output_publication_policy();
    ok &= check_private_decode_shadow_state_policy();
    return ok ? 0 : 1;
}
