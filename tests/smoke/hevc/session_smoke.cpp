#include "vulkan/codecs/hevc/internal.h"

#include <cstdio>

namespace {

    bool check(bool condition, const char* message) {
        if (!condition) {
            std::fprintf(stderr, "%s\n", message);
        }
        return condition;
    }

    bool check_hevc_dpb_slots() {
        vkvv::HEVCVideoSession session{};
        bool                   used_slots[vkvv::max_hevc_dpb_slots] = {};

        const int              first_slot = vkvv::allocate_hevc_dpb_slot(&session, used_slots);
        if (!check(first_slot == 0, "first HEVC DPB slot allocation did not start at zero")) {
            return false;
        }
        vkvv::hevc_set_dpb_slot_for_surface(&session, 41, first_slot);
        if (!check(vkvv::hevc_dpb_slot_for_surface(&session, 41) == first_slot, "HEVC DPB slot lookup did not return the stored surface slot")) {
            return false;
        }

        used_slots[first_slot] = true;
        const int second_slot  = vkvv::allocate_hevc_dpb_slot(&session, used_slots);
        if (!check(second_slot == 1, "HEVC DPB allocation did not skip a used slot")) {
            return false;
        }
        vkvv::hevc_set_dpb_slot_for_surface(&session, 41, second_slot);
        return check(vkvv::hevc_dpb_slot_for_surface(&session, 41) == second_slot, "HEVC DPB slot update did not replace the old surface slot");
    }

    bool check_hevc_reference_info() {
        VAPictureHEVC                    pic{};
        StdVideoDecodeH265ReferenceInfo  info{};

        pic.flags = 0;
        vkvv::fill_hevc_reference_info(pic, &info);
        bool ok = check(info.flags.unused_for_reference == 1, "HEVC non-RPS picture was not marked unused for reference");

        pic.flags = VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE;
        vkvv::fill_hevc_reference_info(pic, &info);
        ok = check(info.flags.unused_for_reference == 0, "HEVC short-term RPS picture was marked unused") && ok;

        pic.flags = VA_PICTURE_HEVC_RPS_LT_CURR | VA_PICTURE_HEVC_LONG_TERM_REFERENCE;
        vkvv::fill_hevc_reference_info(pic, &info);
        ok = check(info.flags.unused_for_reference == 0 && info.flags.used_for_long_term_reference == 1, "HEVC long-term RPS picture flags were not preserved") && ok;

        return ok;
    }

    bool check_hevc_rps_ordering() {
        StdVideoDecodeH265PictureInfo picture{};
        vkvv::HEVCDpbReference        refs[] = {
            {.slot = 7, .pic_order_cnt = 4, .flags = VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE},
            {.slot = 2, .pic_order_cnt = 12, .flags = VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE},
            {.slot = 5, .pic_order_cnt = 20, .flags = VA_PICTURE_HEVC_RPS_ST_CURR_AFTER},
            {.slot = 3, .pic_order_cnt = 16, .flags = VA_PICTURE_HEVC_RPS_ST_CURR_AFTER},
            {.slot = 9, .pic_order_cnt = 1, .flags = VA_PICTURE_HEVC_RPS_LT_CURR | VA_PICTURE_HEVC_LONG_TERM_REFERENCE},
            {.slot = 11, .pic_order_cnt = 99, .flags = 0},
        };

        const vkvv::HEVCRpsCounts counts = vkvv::fill_hevc_picture_rps(refs, sizeof(refs) / sizeof(refs[0]), &picture);
        bool ok = check(counts.st_curr_before == 2 && counts.st_curr_after == 2 && counts.lt_curr == 1, "HEVC RPS counts were not derived from VA RPS flags only");

        ok = check(picture.RefPicSetStCurrBefore[0] == 2 && picture.RefPicSetStCurrBefore[1] == 7,
                   "HEVC RefPicSetStCurrBefore was not sorted by descending POC") &&
            ok;
        ok = check(picture.RefPicSetStCurrAfter[0] == 3 && picture.RefPicSetStCurrAfter[1] == 5, "HEVC RefPicSetStCurrAfter was not sorted by ascending POC") && ok;
        ok = check(picture.RefPicSetLtCurr[0] == 9, "HEVC RefPicSetLtCurr did not preserve the long-term entry") && ok;
        ok = check(picture.RefPicSetStCurrBefore[2] == STD_VIDEO_H265_NO_REFERENCE_PICTURE &&
                       picture.RefPicSetStCurrAfter[2] == STD_VIDEO_H265_NO_REFERENCE_PICTURE &&
                       picture.RefPicSetLtCurr[1] == STD_VIDEO_H265_NO_REFERENCE_PICTURE,
                   "HEVC RPS helper did not clear unused RPS entries") &&
            ok;

        StdVideoDecodeH265PictureInfo no_rps_picture{};
        vkvv::HEVCDpbReference        non_rps_ref = {.slot = 4, .pic_order_cnt = 50, .flags = 0};
        const vkvv::HEVCRpsCounts     no_rps_counts = vkvv::fill_hevc_picture_rps(&non_rps_ref, 1, &no_rps_picture);
        ok = check(no_rps_counts.st_curr_before == 0 && no_rps_counts.st_curr_after == 0 && no_rps_counts.lt_curr == 0,
                   "HEVC RPS helper treated a non-RPS picture as active") &&
            ok;
        ok = check(no_rps_picture.RefPicSetStCurrBefore[0] == STD_VIDEO_H265_NO_REFERENCE_PICTURE &&
                       no_rps_picture.RefPicSetStCurrAfter[0] == STD_VIDEO_H265_NO_REFERENCE_PICTURE &&
                       no_rps_picture.RefPicSetLtCurr[0] == STD_VIDEO_H265_NO_REFERENCE_PICTURE,
                   "HEVC RPS helper emitted references without VA RPS flags") &&
            ok;

        return ok;
    }

} // namespace

int main(void) {
    bool ok = check_hevc_dpb_slots();
    ok      = check_hevc_reference_info() && ok;
    ok      = check_hevc_rps_ordering() && ok;

    if (!ok) {
        return 1;
    }

    std::printf("HEVC RPS and DPB smoke passed\n");
    return 0;
}
