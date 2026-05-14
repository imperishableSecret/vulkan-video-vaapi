#include "internal.h"
#include "telemetry.h"

#include <cstdio>

namespace vkvv {

    namespace {

        bool valid_reference_slot(const AV1ReferenceSlot& entry) {
            return entry.surface_id != VA_INVALID_ID && entry.slot >= 0 && static_cast<uint32_t>(entry.slot) < max_av1_dpb_slots;
        }

        void set_reference_metadata(AV1ReferenceSlot* entry, const AV1ReferenceMetadata* metadata) {
            if (entry == nullptr) {
                return;
            }
            if (metadata == nullptr) {
                entry->has_metadata = false;
                entry->metadata     = {};
                return;
            }
            entry->has_metadata = true;
            entry->metadata     = *metadata;
        }

        bool reference_identity_matches(const AV1ReferenceMetadata& metadata, const VkvvSurface* surface, const SurfaceResource* resource, const VkvvDriver* drv,
                                        const VkvvContext* vctx, const AV1VideoSession* session, const DecodeImageKey& current_decode_key, char* reason, size_t reason_size) {
            if (surface == nullptr || resource == nullptr || drv == nullptr || vctx == nullptr || session == nullptr) {
                std::snprintf(reason, reason_size, "missing AV1 reference identity state");
                return false;
            }
            if (metadata.surface_id != surface->id || resource->surface_id != surface->id) {
                std::snprintf(reason, reason_size, "AV1 reference surface identity mismatch: slot_surface=%u surface=%u resource_surface=%u", metadata.surface_id, surface->id,
                              resource->surface_id);
                return false;
            }
            if (metadata.driver_instance_id != drv->driver_instance_id || surface->driver_instance_id != drv->driver_instance_id ||
                resource->driver_instance_id != drv->driver_instance_id) {
                std::snprintf(reason, reason_size, "AV1 reference driver identity mismatch: surface=%u slot_driver=%llu surface_driver=%llu resource_driver=%llu current=%llu",
                              surface->id, static_cast<unsigned long long>(metadata.driver_instance_id), static_cast<unsigned long long>(surface->driver_instance_id),
                              static_cast<unsigned long long>(resource->driver_instance_id), static_cast<unsigned long long>(drv->driver_instance_id));
                return false;
            }
            if (metadata.stream_id != vctx->stream_id || surface->stream_id != vctx->stream_id || resource->stream_id != vctx->stream_id) {
                std::snprintf(reason, reason_size, "AV1 reference stream identity mismatch: surface=%u slot_stream=%llu surface_stream=%llu resource_stream=%llu current=%llu",
                              surface->id, static_cast<unsigned long long>(metadata.stream_id), static_cast<unsigned long long>(surface->stream_id),
                              static_cast<unsigned long long>(resource->stream_id), static_cast<unsigned long long>(vctx->stream_id));
                return false;
            }
            if (metadata.codec_operation != session->video.key.codec_operation || surface->codec_operation != session->video.key.codec_operation ||
                resource->codec_operation != session->video.key.codec_operation || vctx->codec_operation != session->video.key.codec_operation) {
                std::snprintf(reason, reason_size, "AV1 reference codec identity mismatch: surface=%u slot_codec=0x%x surface_codec=0x%x resource_codec=0x%x current=0x%x",
                              surface->id, metadata.codec_operation, surface->codec_operation, resource->codec_operation, session->video.key.codec_operation);
                return false;
            }
            if (metadata.content_generation == 0 || metadata.content_generation != resource->content_generation) {
                std::snprintf(reason, reason_size, "AV1 reference content generation mismatch: surface=%u slot_gen=%llu resource_gen=%llu", surface->id,
                              static_cast<unsigned long long>(metadata.content_generation), static_cast<unsigned long long>(resource->content_generation));
                return false;
            }
            if (metadata.va_rt_format != resource->va_rt_format || metadata.va_fourcc != resource->va_fourcc || metadata.bit_depth != session->bit_depth) {
                std::snprintf(reason, reason_size,
                              "AV1 reference format identity mismatch: surface=%u slot_rt=0x%x resource_rt=0x%x slot_fourcc=0x%x resource_fourcc=0x%x depth=%u/%u", surface->id,
                              metadata.va_rt_format, resource->va_rt_format, metadata.va_fourcc, resource->va_fourcc, metadata.bit_depth, session->bit_depth);
                return false;
            }
            if (!decode_image_key_matches(resource->decode_key, current_decode_key) || !decode_image_key_matches(metadata.decode_key, current_decode_key)) {
                std::snprintf(reason, reason_size,
                              "AV1 reference decode key mismatch: surface=%u slot_codec=0x%x resource_codec=0x%x current_codec=0x%x slot_fourcc=0x%x resource_fourcc=0x%x "
                              "current_fourcc=0x%x",
                              surface->id, metadata.decode_key.codec_operation, resource->decode_key.codec_operation, current_decode_key.codec_operation,
                              metadata.decode_key.va_fourcc, resource->decode_key.va_fourcc, current_decode_key.va_fourcc);
                return false;
            }
            if (metadata.coded_extent.width != resource->coded_extent.width || metadata.coded_extent.height != resource->coded_extent.height) {
                std::snprintf(reason, reason_size, "AV1 reference extent metadata mismatch: surface=%u slot=%ux%u resource=%ux%u", surface->id, metadata.coded_extent.width,
                              metadata.coded_extent.height, resource->coded_extent.width, resource->coded_extent.height);
                return false;
            }
            return true;
        }

    } // namespace

    int av1_dpb_slot_for_surface(const AV1VideoSession* session, VASurfaceID surface_id) {
        if (session == nullptr || surface_id == VA_INVALID_ID) {
            return -1;
        }
        const AV1ReferenceSlot* surface_slot = av1_surface_slot_for_surface(session, surface_id);
        if (surface_slot != nullptr) {
            return surface_slot->slot;
        }
        const AV1ReferenceSlot* reference_slot = av1_reference_slot_for_surface(session, surface_id);
        return reference_slot != nullptr ? reference_slot->slot : -1;
    }

    const AV1ReferenceSlot* av1_reference_slot_for_index(const AV1VideoSession* session, uint32_t reference_index) {
        if (session == nullptr || reference_index >= max_av1_reference_slots) {
            return nullptr;
        }
        const AV1ReferenceSlot& entry = session->reference_slots[reference_index];
        return valid_reference_slot(entry) ? &entry : nullptr;
    }

    const AV1ReferenceSlot* av1_reference_slot_for_surface(const AV1VideoSession* session, VASurfaceID surface_id) {
        if (session == nullptr || surface_id == VA_INVALID_ID) {
            return nullptr;
        }
        for (const AV1ReferenceSlot& entry : session->reference_slots) {
            if (entry.surface_id == surface_id && entry.slot >= 0) {
                return &entry;
            }
        }
        return nullptr;
    }

    const AV1ReferenceSlot* av1_surface_slot_for_surface(const AV1VideoSession* session, VASurfaceID surface_id) {
        if (session == nullptr || surface_id == VA_INVALID_ID) {
            return nullptr;
        }
        for (const AV1ReferenceSlot& entry : session->surface_slots) {
            if (entry.surface_id == surface_id && entry.slot >= 0) {
                return &entry;
            }
        }
        return nullptr;
    }

    const AV1ReferenceSlot* av1_reconcile_reference_slot(AV1VideoSession* session, uint32_t reference_index, VASurfaceID surface_id) {
        const AV1ReferenceSlot* reference_slot = av1_reference_slot_for_index(session, reference_index);
        if (reference_slot != nullptr && reference_slot->surface_id == surface_id) {
            return reference_slot;
        }

        const AV1ReferenceSlot* surface_slot = av1_surface_slot_for_surface(session, surface_id);
        if (surface_slot == nullptr) {
            return reference_slot != nullptr && reference_slot->surface_id == surface_id ? reference_slot : nullptr;
        }

        av1_set_reference_slot(session, reference_index, surface_id, surface_slot->slot, surface_slot->info, surface_slot->has_metadata ? &surface_slot->metadata : nullptr);
        return av1_reference_slot_for_index(session, reference_index);
    }

    void av1_set_reference_slot(AV1VideoSession* session, uint32_t reference_index, VASurfaceID surface_id, int slot, const StdVideoDecodeAV1ReferenceInfo& info,
                                const AV1ReferenceMetadata* metadata) {
        if (session == nullptr || reference_index >= max_av1_reference_slots || surface_id == VA_INVALID_ID || slot < 0) {
            return;
        }
        AV1ReferenceSlot& entry = session->reference_slots[reference_index];
        entry.surface_id        = surface_id;
        entry.slot              = slot;
        entry.info              = info;
        set_reference_metadata(&entry, metadata);
    }

    void av1_set_surface_slot(AV1VideoSession* session, VASurfaceID surface_id, int slot, const StdVideoDecodeAV1ReferenceInfo& info, const AV1ReferenceMetadata* metadata) {
        if (session == nullptr || surface_id == VA_INVALID_ID || slot < 0) {
            return;
        }
        for (AV1ReferenceSlot& entry : session->reference_slots) {
            if (entry.slot == slot && entry.surface_id != surface_id) {
                entry = {};
            }
        }
        for (AV1ReferenceSlot& entry : session->surface_slots) {
            if (entry.surface_id == surface_id || entry.slot == slot) {
                entry.surface_id = surface_id;
                entry.slot       = slot;
                entry.info       = info;
                set_reference_metadata(&entry, metadata);
                return;
            }
        }
        AV1ReferenceSlot entry{};
        entry.surface_id = surface_id;
        entry.slot       = slot;
        entry.info       = info;
        set_reference_metadata(&entry, metadata);
        session->surface_slots.push_back(entry);
    }

    void av1_clear_reference_slot(AV1VideoSession* session, int slot) {
        if (session == nullptr || slot < 0) {
            return;
        }
        for (AV1ReferenceSlot& entry : session->reference_slots) {
            if (entry.slot == slot) {
                entry = {};
            }
        }
        for (auto it = session->surface_slots.begin(); it != session->surface_slots.end();) {
            if (it->slot == slot) {
                it = session->surface_slots.erase(it);
            } else {
                ++it;
            }
        }
    }

    void av1_clear_surface_slot(AV1VideoSession* session, VASurfaceID surface_id) {
        if (session == nullptr || surface_id == VA_INVALID_ID) {
            return;
        }
        for (auto it = session->surface_slots.begin(); it != session->surface_slots.end();) {
            if (it->surface_id == surface_id) {
                it = session->surface_slots.erase(it);
            } else {
                ++it;
            }
        }
    }

    SurfaceResource* av1_retained_dpb_resource_for_slot(const AV1VideoSession* session, int slot) {
        if (session == nullptr || slot < 0 || static_cast<uint32_t>(slot) >= max_av1_dpb_slots) {
            return nullptr;
        }
        return session->retained_dpb_resources[slot];
    }

    void av1_release_retained_dpb_resource(VulkanRuntime* runtime, AV1VideoSession* session, int slot) {
        if (session == nullptr || slot < 0 || static_cast<uint32_t>(slot) >= max_av1_dpb_slots) {
            return;
        }
        SurfaceResource* resource             = session->retained_dpb_resources[slot];
        session->retained_dpb_resources[slot] = nullptr;
        destroy_surface_resource_raw(runtime, resource);
    }

    void av1_release_unreferenced_retained_dpb_resources(VulkanRuntime* runtime, AV1VideoSession* session) {
        if (session == nullptr) {
            return;
        }
        for (uint32_t slot = 0; slot < max_av1_dpb_slots; slot++) {
            if (session->retained_dpb_resources[slot] == nullptr) {
                continue;
            }
            bool referenced = false;
            for (const AV1ReferenceSlot& entry : session->reference_slots) {
                if (entry.slot == static_cast<int>(slot)) {
                    referenced = true;
                    break;
                }
            }
            if (!referenced) {
                for (const AV1ReferenceSlot& entry : session->surface_slots) {
                    if (entry.slot == static_cast<int>(slot)) {
                        referenced = true;
                        break;
                    }
                }
            }
            if (!referenced) {
                av1_release_retained_dpb_resource(runtime, session, static_cast<int>(slot));
            }
        }
    }

    bool av1_target_surface_needs_detach(const AV1VideoSession* session, const VkvvAV1DecodeInput* input, VASurfaceID target_surface_id) {
        if (session == nullptr || input == nullptr || input->pic == nullptr || target_surface_id == VA_INVALID_ID) {
            return false;
        }
        const AV1ReferenceSlot* surface_slot = av1_surface_slot_for_surface(session, target_surface_id);
        if (surface_slot == nullptr || surface_slot->slot < 0) {
            return false;
        }

        for (uint32_t i = 0; i < VK_MAX_VIDEO_AV1_REFERENCES_PER_FRAME_KHR; i++) {
            const uint8_t ref_index = input->pic->ref_frame_idx[i];
            if (ref_index >= max_av1_reference_slots) {
                continue;
            }
            if (input->pic->ref_frame_map[ref_index] == target_surface_id) {
                return true;
            }
            const AV1ReferenceSlot* ref_slot = av1_reference_slot_for_index(session, ref_index);
            if (ref_slot != nullptr && ref_slot->slot == surface_slot->slot) {
                return true;
            }
        }

        for (uint32_t i = 0; i < max_av1_reference_slots; i++) {
            if ((input->header.refresh_frame_flags & (1U << i)) != 0) {
                continue;
            }
            const AV1ReferenceSlot* ref_slot = av1_reference_slot_for_index(session, i);
            if (ref_slot != nullptr && ref_slot->slot == surface_slot->slot) {
                return true;
            }
        }
        return false;
    }

    void av1_detach_target_dpb_resource(VulkanRuntime* runtime, AV1VideoSession* session, VkvvSurface* target, VASurfaceID target_surface_id) {
        if (session == nullptr || target == nullptr || target->vulkan == nullptr) {
            return;
        }
        const AV1ReferenceSlot* surface_slot = av1_surface_slot_for_surface(session, target_surface_id);
        if (surface_slot == nullptr || surface_slot->slot < 0 || static_cast<uint32_t>(surface_slot->slot) >= max_av1_dpb_slots) {
            return;
        }

        const int slot     = surface_slot->slot;
        auto*     resource = static_cast<SurfaceResource*>(target->vulkan);
        if (session->retained_dpb_resources[slot] != nullptr && session->retained_dpb_resources[slot] != resource) {
            destroy_surface_resource_raw(runtime, session->retained_dpb_resources[slot]);
        }
        session->retained_dpb_resources[slot] = resource;
        target->vulkan                        = nullptr;
        target->decoded                       = false;
        target->work_state                    = VKVV_SURFACE_WORK_READY;
        target->sync_status                   = VA_STATUS_SUCCESS;
        VKVV_TRACE("av1-target-reference-detach", "surface=%u slot=%d content_gen=%llu retained_mem=%llu", target_surface_id, slot,
                   static_cast<unsigned long long>(resource->content_generation), static_cast<unsigned long long>(resource->allocation_size));
    }

    void av1_mark_retained_reference_slots(const AV1VideoSession* session, const VkvvAV1DecodeInput* input, bool used_slots[max_av1_dpb_slots]) {
        if (session == nullptr || used_slots == nullptr) {
            return;
        }
        const uint8_t refresh = input != nullptr ? input->header.refresh_frame_flags : 0;
        for (uint32_t i = 0; i < max_av1_reference_slots; i++) {
            if ((refresh & (1U << i)) != 0) {
                continue;
            }
            const AV1ReferenceSlot& entry = session->reference_slots[i];
            if (valid_reference_slot(entry)) {
                used_slots[entry.slot] = true;
            }
            if (input == nullptr || input->pic == nullptr) {
                continue;
            }
            const AV1ReferenceSlot* surface_slot = av1_surface_slot_for_surface(session, input->pic->ref_frame_map[i]);
            if (surface_slot != nullptr) {
                used_slots[surface_slot->slot] = true;
            }
        }
    }

    int av1_select_target_dpb_slot(AV1VideoSession* session, VASurfaceID target_surface_id, const bool used_slots[max_av1_dpb_slots]) {
        if (session == nullptr || target_surface_id == VA_INVALID_ID || used_slots == nullptr) {
            return -1;
        }

        uint32_t slot_count = session->max_dpb_slots;
        if (slot_count == 0) {
            return -1;
        }
        if (slot_count > max_av1_dpb_slots) {
            slot_count = max_av1_dpb_slots;
        }
        int target_slot = av1_dpb_slot_for_surface(session, target_surface_id);
        if (target_slot < 0 || target_slot >= static_cast<int>(slot_count) || used_slots[target_slot]) {
            target_slot = allocate_av1_dpb_slot(session, used_slots);
        }
        return target_slot;
    }

    int av1_reserved_scratch_dpb_slot(const AV1VideoSession* session) {
        if (session == nullptr) {
            return -1;
        }
        uint32_t slot_count = session->max_dpb_slots;
        if (slot_count == 0) {
            return -1;
        }
        if (slot_count > max_av1_dpb_slots) {
            slot_count = max_av1_dpb_slots;
        }
        if (slot_count <= max_av1_reference_slots) {
            return -1;
        }
        return static_cast<int>(slot_count - 1);
    }

    int av1_select_current_setup_slot(AV1VideoSession* session, VASurfaceID target_surface_id, const bool used_slots[max_av1_dpb_slots], bool current_is_reference) {
        if (!current_is_reference) {
            const int scratch_slot = av1_reserved_scratch_dpb_slot(session);
            if (scratch_slot >= 0 && used_slots != nullptr && !used_slots[scratch_slot]) {
                return scratch_slot;
            }
            return allocate_av1_dpb_slot(session, used_slots);
        }

        bool reference_used_slots[max_av1_dpb_slots]{};
        if (used_slots == nullptr) {
            return -1;
        }
        for (uint32_t i = 0; i < max_av1_dpb_slots; i++) {
            reference_used_slots[i] = used_slots[i];
        }
        const int scratch_slot = av1_reserved_scratch_dpb_slot(session);
        if (scratch_slot >= 0) {
            reference_used_slots[scratch_slot] = true;
        }
        return av1_select_target_dpb_slot(session, target_surface_id, reference_used_slots);
    }

    VkImageLayout av1_target_layout(bool has_setup_slot) {
        return has_setup_slot ? VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR : VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR;
    }

    VkAccessFlags2 av1_target_access(bool has_setup_slot) {
        return has_setup_slot ? (VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR | VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR) : VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR;
    }

    bool validate_av1_reference_slot(const AV1VideoSession* session, const AV1ReferenceSlot* slot, const VkvvSurface* surface, const SurfaceResource* resource,
                                     const VkvvDriver* drv, const VkvvContext* vctx, const DecodeImageKey& current_decode_key, char* reason, size_t reason_size) {
        if (session == nullptr || slot == nullptr || !valid_reference_slot(*slot)) {
            std::snprintf(reason, reason_size, "missing AV1 reference DPB slot metadata");
            return false;
        }
        if (surface == nullptr) {
            std::snprintf(reason, reason_size, "missing AV1 reference surface: slot_surface=%u slot=%d", slot->surface_id, slot->slot);
            return false;
        }
        const bool retained_resource = resource != nullptr && resource != surface->vulkan;
        if (((!surface->decoded || surface->vulkan == nullptr) && !retained_resource) || resource == nullptr) {
            std::snprintf(reason, reason_size, "AV1 reference surface %u is not decoded: decoded=%u vulkan=%u slot=%d", surface->id, surface->decoded ? 1U : 0U,
                          surface->vulkan != nullptr ? 1U : 0U, slot->slot);
            return false;
        }
        if (slot->surface_id != surface->id) {
            std::snprintf(reason, reason_size, "AV1 reference slot surface mismatch: slot_surface=%u actual_surface=%u slot=%d", slot->surface_id, surface->id, slot->slot);
            return false;
        }
        if (!slot->has_metadata) {
            std::snprintf(reason, reason_size, "AV1 reference surface %u is missing identity metadata: slot=%d", surface->id, slot->slot);
            return false;
        }
        return reference_identity_matches(slot->metadata, surface, resource, drv, vctx, session, current_decode_key, reason, reason_size);
    }

    const AV1ReferenceSlot* validate_av1_show_existing_reference(const AV1VideoSession* session, const VkvvAV1DecodeInput* input, const VkvvSurface* surface,
                                                                 const SurfaceResource* resource, const VkvvDriver* drv, const VkvvContext* vctx,
                                                                 const DecodeImageKey& current_decode_key, char* reason, size_t reason_size) {
        if (session == nullptr || input == nullptr || !input->header.show_existing_frame) {
            std::snprintf(reason, reason_size, "missing AV1 show-existing frame state");
            return nullptr;
        }
        if (input->header.frame_to_show_map_idx < 0 || input->header.frame_to_show_map_idx >= static_cast<int8_t>(max_av1_reference_slots)) {
            std::snprintf(reason, reason_size, "invalid AV1 show-existing reference index: map_idx=%d", input->header.frame_to_show_map_idx);
            return nullptr;
        }

        const auto* slot = av1_reference_slot_for_index(session, static_cast<uint32_t>(input->header.frame_to_show_map_idx));
        if (!validate_av1_reference_slot(session, slot, surface, resource, drv, vctx, current_decode_key, reason, reason_size)) {
            return nullptr;
        }
        if (!slot->metadata.showable) {
            std::snprintf(reason, reason_size, "AV1 show-existing reference is not showable: surface=%u map_idx=%d slot=%d", slot->surface_id, input->header.frame_to_show_map_idx,
                          slot->slot);
            return nullptr;
        }
        if (input->sequence.frame_id_numbers_present_flag && slot->metadata.frame_id != input->header.display_frame_id) {
            std::snprintf(reason, reason_size, "AV1 show-existing display frame id mismatch: surface=%u map_idx=%d expected=%u actual=%u", slot->surface_id,
                          input->header.frame_to_show_map_idx, slot->metadata.frame_id, input->header.display_frame_id);
            return nullptr;
        }
        return slot;
    }

    bool av1_decode_needs_export_refresh(const VkvvAV1DecodeInput* input) {
        if (input == nullptr || input->pic == nullptr) {
            return true;
        }
        return input->pic->pic_info_fields.bits.show_frame != 0 || input->header.show_existing_frame || input->header.show_frame;
    }

    void av1_update_reference_slots_from_refresh(AV1VideoSession* session, const VkvvAV1DecodeInput* input, VASurfaceID target_surface_id, int target_slot,
                                                 const StdVideoDecodeAV1ReferenceInfo& info, const AV1ReferenceMetadata* metadata) {
        if (session == nullptr || input == nullptr || target_surface_id == VA_INVALID_ID || target_slot < 0) {
            return;
        }
        if (input->header.refresh_frame_flags == 0) {
            return;
        }
        av1_set_surface_slot(session, target_surface_id, target_slot, info, metadata);
        for (uint32_t i = 0; i < max_av1_reference_slots; i++) {
            if ((input->header.refresh_frame_flags & (1U << i)) != 0) {
                av1_set_reference_slot(session, i, target_surface_id, target_slot, info, metadata);
            }
        }
    }

    int allocate_av1_dpb_slot(AV1VideoSession* session, const bool used_slots[max_av1_dpb_slots]) {
        if (session == nullptr || used_slots == nullptr) {
            return -1;
        }
        uint32_t slot_count = session->max_dpb_slots;
        if (slot_count == 0) {
            return -1;
        }
        if (slot_count > max_av1_dpb_slots) {
            slot_count = max_av1_dpb_slots;
        }
        for (uint32_t attempt = 0; attempt < slot_count; attempt++) {
            const uint32_t slot = (session->next_dpb_slot + attempt) % slot_count;
            if (!used_slots[slot]) {
                session->next_dpb_slot = (slot + 1) % slot_count;
                return static_cast<int>(slot);
            }
        }
        return -1;
    }

} // namespace vkvv
