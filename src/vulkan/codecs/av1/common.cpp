#include "internal.h"

namespace vkvv {

    namespace {

        bool valid_reference_slot(const AV1ReferenceSlot& entry) {
            return entry.surface_id != VA_INVALID_ID && entry.slot >= 0 && static_cast<uint32_t>(entry.slot) < max_av1_dpb_slots;
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

        av1_set_reference_slot(session, reference_index, surface_id, surface_slot->slot, surface_slot->info);
        return av1_reference_slot_for_index(session, reference_index);
    }

    void av1_set_reference_slot(AV1VideoSession* session, uint32_t reference_index, VASurfaceID surface_id, int slot, const StdVideoDecodeAV1ReferenceInfo& info) {
        if (session == nullptr || reference_index >= max_av1_reference_slots || surface_id == VA_INVALID_ID || slot < 0) {
            return;
        }
        AV1ReferenceSlot& entry = session->reference_slots[reference_index];
        entry.surface_id        = surface_id;
        entry.slot              = slot;
        entry.info              = info;
    }

    void av1_set_surface_slot(AV1VideoSession* session, VASurfaceID surface_id, int slot, const StdVideoDecodeAV1ReferenceInfo& info) {
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
                return;
            }
        }
        session->surface_slots.push_back({
            .surface_id = surface_id,
            .slot       = slot,
            .info       = info,
        });
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

    int av1_select_current_setup_slot(AV1VideoSession* session, VASurfaceID target_surface_id, const bool used_slots[max_av1_dpb_slots], bool current_is_reference) {
        (void)current_is_reference;
        return av1_select_target_dpb_slot(session, target_surface_id, used_slots);
    }

    VkImageLayout av1_target_layout(bool has_setup_slot) {
        return has_setup_slot ? VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR : VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR;
    }

    VkAccessFlags2 av1_target_access(bool has_setup_slot) {
        return has_setup_slot ? (VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR | VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR) : VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR;
    }

    bool av1_decode_needs_export_refresh(const VkvvAV1DecodeInput* input) {
        if (input == nullptr || input->pic == nullptr) {
            return true;
        }
        return input->pic->pic_info_fields.bits.show_frame != 0 || input->header.show_existing_frame || input->header.show_frame || input->header.showable_frame;
    }

    void av1_update_reference_slots_from_refresh(AV1VideoSession* session, const VkvvAV1DecodeInput* input, VASurfaceID target_surface_id, int target_slot,
                                                 const StdVideoDecodeAV1ReferenceInfo& info) {
        if (session == nullptr || input == nullptr || target_surface_id == VA_INVALID_ID || target_slot < 0) {
            return;
        }
        if (input->header.refresh_frame_flags == 0) {
            return;
        }
        av1_set_surface_slot(session, target_surface_id, target_slot, info);
        for (uint32_t i = 0; i < max_av1_reference_slots; i++) {
            if ((input->header.refresh_frame_flags & (1U << i)) != 0) {
                av1_set_reference_slot(session, i, target_surface_id, target_slot, info);
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
