#include "internal.h"

namespace vkvv {

    int vp9_dpb_slot_for_reference_index(const VP9VideoSession* session, uint32_t reference_index) {
        if (session == nullptr || reference_index >= max_vp9_reference_slots) {
            return -1;
        }
        return session->reference_slots[reference_index].slot;
    }

    int vp9_dpb_slot_for_surface(const VP9VideoSession* session, VASurfaceID surface_id) {
        if (session == nullptr || surface_id == VA_INVALID_ID) {
            return -1;
        }
        for (const VP9ReferenceSlot& entry : session->reference_slots) {
            if (entry.surface_id == surface_id && entry.slot >= 0) {
                return entry.slot;
            }
        }
        return -1;
    }

    void vp9_set_reference_slot(VP9VideoSession* session, uint32_t reference_index, VASurfaceID surface_id, int slot) {
        if (session == nullptr || reference_index >= max_vp9_reference_slots) {
            return;
        }
        session->reference_slots[reference_index].surface_id = surface_id;
        session->reference_slots[reference_index].slot       = slot;
    }

    int allocate_vp9_dpb_slot(VP9VideoSession* session, const bool used_slots[max_vp9_dpb_slots]) {
        if (session == nullptr || used_slots == nullptr) {
            return -1;
        }
        for (uint32_t attempt = 0; attempt < max_vp9_dpb_slots; attempt++) {
            const uint32_t slot = (session->next_dpb_slot + attempt) % max_vp9_dpb_slots;
            if (!used_slots[slot]) {
                session->next_dpb_slot = (slot + 1) % max_vp9_dpb_slots;
                return static_cast<int>(slot);
            }
        }
        return -1;
    }

} // namespace vkvv
