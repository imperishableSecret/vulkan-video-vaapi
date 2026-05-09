#include "internal.h"

namespace vkvv {

    bool h264_picture_is_invalid(const VAPictureH264& picture) {
        return (picture.flags & VA_PICTURE_H264_INVALID) != 0 || picture.picture_id == VA_INVALID_ID;
    }

    int h264_dpb_slot_for_surface(const H264VideoSession* session, VASurfaceID surface_id) {
        if (session == nullptr || surface_id == VA_INVALID_ID) {
            return -1;
        }
        for (const H264SurfaceDpbSlot& entry : session->surface_slots) {
            if (entry.surface_id == surface_id) {
                return entry.slot;
            }
        }
        return -1;
    }

    void h264_set_dpb_slot_for_surface(H264VideoSession* session, VASurfaceID surface_id, int slot) {
        if (session == nullptr || surface_id == VA_INVALID_ID) {
            return;
        }
        for (H264SurfaceDpbSlot& entry : session->surface_slots) {
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

    int allocate_dpb_slot(H264VideoSession* session, const bool used_slots[max_h264_dpb_slots]) {
        if (session == nullptr) {
            return -1;
        }
        for (uint32_t attempt = 0; attempt < max_h264_dpb_slots; attempt++) {
            const uint32_t slot = (session->next_dpb_slot + attempt) % max_h264_dpb_slots;
            if (!used_slots[slot]) {
                session->next_dpb_slot = (slot + 1) % max_h264_dpb_slots;
                return static_cast<int>(slot);
            }
        }
        return -1;
    }

    void fill_reference_info(const VAPictureH264& picture, uint16_t frame_num, StdVideoDecodeH264ReferenceInfo* info) {
        *info                                                             = {};
        info->flags.top_field_flag                                        = (picture.flags & VA_PICTURE_H264_TOP_FIELD) != 0;
        info->flags.bottom_field_flag                                     = (picture.flags & VA_PICTURE_H264_BOTTOM_FIELD) != 0;
        info->flags.used_for_long_term_reference                          = (picture.flags & VA_PICTURE_H264_LONG_TERM_REFERENCE) != 0;
        info->FrameNum                                                    = frame_num;
        info->PicOrderCnt[STD_VIDEO_DECODE_H264_FIELD_ORDER_COUNT_TOP]    = picture.TopFieldOrderCnt;
        info->PicOrderCnt[STD_VIDEO_DECODE_H264_FIELD_ORDER_COUNT_BOTTOM] = picture.BottomFieldOrderCnt;
    }

    bool bitstream_has_idr(const VkvvH264DecodeInput* input) {
        for (uint32_t i = 0; i < input->slice_count; i++) {
            size_t offset = input->slice_offsets[i];
            if (offset + 3 < input->bitstream_size && input->bitstream[offset] == 0 && input->bitstream[offset + 1] == 0 && input->bitstream[offset + 2] == 1) {
                offset += 3;
            } else if (offset + 4 < input->bitstream_size && input->bitstream[offset] == 0 && input->bitstream[offset + 1] == 0 && input->bitstream[offset + 2] == 0 &&
                       input->bitstream[offset + 3] == 1) {
                offset += 4;
            }
            if (offset < input->bitstream_size && (input->bitstream[offset] & 0x1f) == 5) {
                return true;
            }
        }
        return false;
    }
} // namespace vkvv
