#ifndef VKVV_VULKAN_EXPORT_INTERNAL_H
#define VKVV_VULKAN_EXPORT_INTERNAL_H

#include "vulkan/runtime_internal.h"

#include <cstddef>
#include <cstdint>
#include <vulkan/vulkan.h>

namespace vkvv {

    struct ExportLayerInfo {
        VkImageAspectFlags aspect;
        uint32_t           drm_format;
        uint32_t           width_divisor;
        uint32_t           height_divisor;
    };

    struct ExportFormatInfo {
        unsigned int    va_fourcc;
        VkFormat        vk_format;
        const char*     name;
        uint32_t        layer_count;
        ExportLayerInfo layers[2];
    };

    const ExportFormatInfo* export_format_for_fourcc(unsigned int fourcc);
    const ExportFormatInfo* export_format_for_surface(const VkvvSurface* surface, const SurfaceResource* resource, char* reason, size_t reason_size);
    VkExtent3D              export_layer_extent(VkExtent2D coded_extent, const ExportLayerInfo& layer);

    VAStatus                validate_export_flags(uint32_t flags, char* reason, size_t reason_size);
    VAStatus fill_drm_prime_descriptor(const VkvvSurface* surface, const ExportFormatInfo* format, VkDeviceSize allocation_size, const VkSubresourceLayout* plane_layouts,
                                       uint32_t plane_count, uint64_t modifier, bool has_modifier, int fd, VADRMPRIMESurfaceDescriptor* descriptor, char* reason,
                                       size_t reason_size);

    bool     ensure_export_resource(VulkanRuntime* runtime, SurfaceResource* source, char* reason, size_t reason_size);
    bool     ensure_private_decode_shadow(VulkanRuntime* runtime, SurfaceResource* source, char* reason, size_t reason_size);
    bool     copy_decode_to_private_shadow(VulkanRuntime* runtime, SurfaceResource* source, char* reason, size_t reason_size);
    bool     attach_imported_export_resource_by_fd(VulkanRuntime* runtime, SurfaceResource* source);
    bool     ensure_export_only_surface_resource(VkvvSurface* surface, const ExportFormatInfo* format, VkExtent2D extent, char* reason, size_t reason_size);
    bool     copy_surface_to_export_resource(VulkanRuntime* runtime, SurfaceResource* source, uint32_t* seeded_predecode_exports, char* reason, size_t reason_size);
    bool     refresh_nondisplay_export_resource(VulkanRuntime* runtime, SurfaceResource* source, char* reason, size_t reason_size);
    bool     seed_predecode_export_from_last_good(VulkanRuntime* runtime, SurfaceResource* target, char* reason, size_t reason_size);
    void     enter_predecode_quarantine(const SurfaceResource* owner, ExportResource* resource);
    void     exit_predecode_quarantine(const SurfaceResource* owner, ExportResource* resource, bool release_done);
    void     mark_export_visible_acquire(const SurfaceResource* owner, ExportResource* resource);
    void     mark_export_visible_release(const SurfaceResource* owner, ExportResource* resource, VkImageLayout old_layout, VkImageLayout new_layout);
    VkvvPixelProofMode export_pixel_proof_mode();
    bool     export_pixel_proof_enabled();
    bool     export_seed_pixel_proof_required();
    bool     export_visible_pixel_proof_required();
    bool     trace_visible_pixel_proof(VulkanRuntime* runtime, SurfaceResource* source, char* reason, size_t reason_size);
    bool     trace_private_shadow_pixel_proof(VulkanRuntime* runtime, SurfaceResource* source, char* reason, size_t reason_size);
    bool     trace_returned_fd_pixel_proof(VulkanRuntime* runtime, const SurfaceResource* owner, ExportResource* resource, const VkvvFdIdentity& fd,
                                           VkvvExportPixelSource pixel_source, VkvvReturnedFdProof* proof, char* reason, size_t reason_size);
    void     trace_seed_pixel_proof(VulkanRuntime* runtime, SurfaceResource* source, ExportResource* target, const char* copy_status);

} // namespace vkvv

#endif
