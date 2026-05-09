#include "internal.h"

#include <algorithm>
#include <array>

namespace vkvv {

    namespace {

        struct RpsEntry {
            uint8_t slot = STD_VIDEO_H265_NO_REFERENCE_PICTURE;
            int32_t poc  = 0;
        };

        using RpsList = std::array<RpsEntry, STD_VIDEO_DECODE_H265_REF_PIC_SET_LIST_SIZE>;

        void append_unique_rps_entry(uint8_t slot, int32_t poc, RpsList& list, uint32_t* count) {
            if (count == nullptr || *count >= list.size()) {
                return;
            }
            for (uint32_t i = 0; i < *count; i++) {
                if (list[i].slot == slot) {
                    return;
                }
            }
            list[*count] = {
                .slot = slot,
                .poc  = poc,
            };
            (*count)++;
        }

        template <typename Before>
        void sort_rps_entries(RpsList& entries, uint32_t count, Before before) {
            for (uint32_t i = 1; i < count && i < entries.size(); i++) {
                const RpsEntry value = entries[i];
                uint32_t       pos   = i;
                while (pos > 0 && before(value, entries[pos - 1])) {
                    entries[pos] = entries[pos - 1];
                    pos--;
                }
                entries[pos] = value;
            }
        }

        void emit_rps_entries(const RpsList& entries, uint32_t count, uint8_t list[STD_VIDEO_DECODE_H265_REF_PIC_SET_LIST_SIZE]) {
            for (uint32_t i = 0; i < count && i < entries.size(); i++) {
                list[i] = entries[i].slot;
            }
        }

    } // namespace

    bool hevc_picture_is_invalid(const VAPictureHEVC& picture) {
        return (picture.flags & VA_PICTURE_HEVC_INVALID) != 0 || picture.picture_id == VA_INVALID_ID;
    }

    bool hevc_picture_is_current_reference(const VAPictureHEVC& picture) {
        return (picture.flags & (VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE | VA_PICTURE_HEVC_RPS_ST_CURR_AFTER | VA_PICTURE_HEVC_RPS_LT_CURR)) != 0;
    }

    int hevc_dpb_slot_for_surface(const HEVCVideoSession* session, VASurfaceID surface_id) {
        if (session == nullptr || surface_id == VA_INVALID_ID) {
            return -1;
        }
        for (const HEVCSurfaceDpbSlot& entry : session->surface_slots) {
            if (entry.surface_id == surface_id) {
                return entry.slot;
            }
        }
        return -1;
    }

    void hevc_set_dpb_slot_for_surface(HEVCVideoSession* session, VASurfaceID surface_id, int slot) {
        if (session == nullptr || surface_id == VA_INVALID_ID) {
            return;
        }
        for (HEVCSurfaceDpbSlot& entry : session->surface_slots) {
            if (entry.surface_id == surface_id) {
                entry.slot = slot;
                return;
            }
        }
        session->surface_slots.push_back({
            .surface_id = surface_id,
            .slot       = slot,
        });
    }

    int allocate_hevc_dpb_slot(HEVCVideoSession* session, const bool used_slots[max_hevc_dpb_slots]) {
        if (session == nullptr) {
            return -1;
        }
        for (uint32_t attempt = 0; attempt < max_hevc_dpb_slots; attempt++) {
            const uint32_t slot = (session->next_dpb_slot + attempt) % max_hevc_dpb_slots;
            if (!used_slots[slot]) {
                session->next_dpb_slot = (slot + 1) % max_hevc_dpb_slots;
                return static_cast<int>(slot);
            }
        }
        return -1;
    }

    void fill_hevc_reference_info(const VAPictureHEVC& picture, StdVideoDecodeH265ReferenceInfo* info) {
        *info                                    = {};
        info->flags.used_for_long_term_reference = (picture.flags & VA_PICTURE_HEVC_LONG_TERM_REFERENCE) != 0;
        info->flags.unused_for_reference         = !hevc_picture_is_current_reference(picture);
        info->PicOrderCntVal                     = picture.pic_order_cnt;
    }

    HEVCRpsCounts fill_hevc_picture_rps(const HEVCDpbReference* references, uint32_t reference_count, StdVideoDecodeH265PictureInfo* picture) {
        HEVCRpsCounts counts{};
        if (picture == nullptr) {
            return counts;
        }

        std::fill(std::begin(picture->RefPicSetStCurrBefore), std::end(picture->RefPicSetStCurrBefore), STD_VIDEO_H265_NO_REFERENCE_PICTURE);
        std::fill(std::begin(picture->RefPicSetStCurrAfter), std::end(picture->RefPicSetStCurrAfter), STD_VIDEO_H265_NO_REFERENCE_PICTURE);
        std::fill(std::begin(picture->RefPicSetLtCurr), std::end(picture->RefPicSetLtCurr), STD_VIDEO_H265_NO_REFERENCE_PICTURE);

        RpsList st_before_entries{};
        RpsList st_after_entries{};
        RpsList lt_entries{};

        if (references != nullptr) {
            for (uint32_t i = 0; i < reference_count; i++) {
                const HEVCDpbReference& ref = references[i];
                if ((ref.flags & VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE) != 0) {
                    append_unique_rps_entry(ref.slot, ref.pic_order_cnt, st_before_entries, &counts.st_curr_before);
                } else if ((ref.flags & VA_PICTURE_HEVC_RPS_ST_CURR_AFTER) != 0) {
                    append_unique_rps_entry(ref.slot, ref.pic_order_cnt, st_after_entries, &counts.st_curr_after);
                } else if ((ref.flags & VA_PICTURE_HEVC_RPS_LT_CURR) != 0) {
                    append_unique_rps_entry(ref.slot, ref.pic_order_cnt, lt_entries, &counts.lt_curr);
                }
            }
        }

        sort_rps_entries(st_before_entries, counts.st_curr_before, [](const RpsEntry& a, const RpsEntry& b) { return a.poc > b.poc; });
        sort_rps_entries(st_after_entries, counts.st_curr_after, [](const RpsEntry& a, const RpsEntry& b) { return a.poc < b.poc; });

        emit_rps_entries(st_before_entries, counts.st_curr_before, picture->RefPicSetStCurrBefore);
        emit_rps_entries(st_after_entries, counts.st_curr_after, picture->RefPicSetStCurrAfter);
        emit_rps_entries(lt_entries, counts.lt_curr, picture->RefPicSetLtCurr);

        return counts;
    }

} // namespace vkvv
