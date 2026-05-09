#include "vulkan/codecs/hevc/internal.h"
#include "vulkan/codecs/hevc/api.h"

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
        VAPictureHEVC                   pic{};
        StdVideoDecodeH265ReferenceInfo info{};

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

    bool check_hevc_session_profiles() {
        void* main_session = vkvv_vulkan_hevc_session_create();
        if (!check(main_session != nullptr, "HEVC Main session allocation failed")) {
            return false;
        }
        auto* main = static_cast<const vkvv::HEVCVideoSession*>(main_session);
        bool  ok   = check(main->va_profile == VAProfileHEVCMain && main->va_rt_format == VA_RT_FORMAT_YUV420 && main->va_fourcc == VA_FOURCC_NV12 && main->bit_depth == 8,
                           "HEVC Main session did not select NV12/8-bit metadata");
        ok         = check(main->profile_spec.bit_depth == VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR && main->profile_spec.std_profile == STD_VIDEO_H265_PROFILE_IDC_MAIN,
                           "HEVC Main session did not select the Vulkan Main profile spec") &&
            ok;
        vkvv_vulkan_hevc_session_destroy(nullptr, main_session);

        VkvvConfig main10_config{};
        main10_config.profile   = VAProfileHEVCMain10;
        main10_config.rt_format = VA_RT_FORMAT_YUV420_10;
        main10_config.fourcc    = VA_FOURCC_P010;
        main10_config.bit_depth = 10;

        void* main10_session = vkvv_vulkan_hevc_session_create_for_config(&main10_config);
        if (!check(main10_session != nullptr, "HEVC Main10 session allocation failed")) {
            return false;
        }
        auto* main10 = static_cast<const vkvv::HEVCVideoSession*>(main10_session);
        ok = check(main10->va_profile == VAProfileHEVCMain10 && main10->va_rt_format == VA_RT_FORMAT_YUV420_10 && main10->va_fourcc == VA_FOURCC_P010 && main10->bit_depth == 10,
                   "HEVC Main10 session did not select P010/10-bit metadata") &&
            ok;
        ok = check(main10->profile_spec.bit_depth == VK_VIDEO_COMPONENT_BIT_DEPTH_10_BIT_KHR && main10->profile_spec.std_profile == STD_VIDEO_H265_PROFILE_IDC_MAIN_10,
                   "HEVC Main10 session did not select the Vulkan Main10 profile spec") &&
            ok;
        vkvv_vulkan_hevc_session_destroy(nullptr, main10_session);
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

        ok = check(picture.RefPicSetStCurrBefore[0] == 2 && picture.RefPicSetStCurrBefore[1] == 7, "HEVC RefPicSetStCurrBefore was not sorted by descending POC") && ok;
        ok = check(picture.RefPicSetStCurrAfter[0] == 3 && picture.RefPicSetStCurrAfter[1] == 5, "HEVC RefPicSetStCurrAfter was not sorted by ascending POC") && ok;
        ok = check(picture.RefPicSetLtCurr[0] == 9, "HEVC RefPicSetLtCurr did not preserve the long-term entry") && ok;
        ok = check(picture.RefPicSetStCurrBefore[2] == STD_VIDEO_H265_NO_REFERENCE_PICTURE && picture.RefPicSetStCurrAfter[2] == STD_VIDEO_H265_NO_REFERENCE_PICTURE &&
                       picture.RefPicSetLtCurr[1] == STD_VIDEO_H265_NO_REFERENCE_PICTURE,
                   "HEVC RPS helper did not clear unused RPS entries") &&
            ok;

        StdVideoDecodeH265PictureInfo no_rps_picture{};
        vkvv::HEVCDpbReference        non_rps_ref   = {.slot = 4, .pic_order_cnt = 50, .flags = 0};
        const vkvv::HEVCRpsCounts     no_rps_counts = vkvv::fill_hevc_picture_rps(&non_rps_ref, 1, &no_rps_picture);
        ok =
            check(no_rps_counts.st_curr_before == 0 && no_rps_counts.st_curr_after == 0 && no_rps_counts.lt_curr == 0, "HEVC RPS helper treated a non-RPS picture as active") && ok;
        ok = check(no_rps_picture.RefPicSetStCurrBefore[0] == STD_VIDEO_H265_NO_REFERENCE_PICTURE &&
                       no_rps_picture.RefPicSetStCurrAfter[0] == STD_VIDEO_H265_NO_REFERENCE_PICTURE && no_rps_picture.RefPicSetLtCurr[0] == STD_VIDEO_H265_NO_REFERENCE_PICTURE,
                   "HEVC RPS helper emitted references without VA RPS flags") &&
            ok;

        return ok;
    }

    bool check_hevc_rps_capacity_and_uniqueness() {
        StdVideoDecodeH265PictureInfo picture{};
        vkvv::HEVCDpbReference        refs[] = {
            {.slot = 0, .pic_order_cnt = 0, .flags = VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE},  {.slot = 1, .pic_order_cnt = 10, .flags = VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE},
            {.slot = 1, .pic_order_cnt = 99, .flags = VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE}, {.slot = 2, .pic_order_cnt = 20, .flags = VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE},
            {.slot = 3, .pic_order_cnt = 30, .flags = VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE}, {.slot = 4, .pic_order_cnt = 40, .flags = VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE},
            {.slot = 5, .pic_order_cnt = 50, .flags = VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE}, {.slot = 6, .pic_order_cnt = 60, .flags = VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE},
            {.slot = 7, .pic_order_cnt = 70, .flags = VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE}, {.slot = 8, .pic_order_cnt = 80, .flags = VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE},
        };

        const vkvv::HEVCRpsCounts counts = vkvv::fill_hevc_picture_rps(refs, sizeof(refs) / sizeof(refs[0]), &picture);
        bool          ok = check(counts.st_curr_before == STD_VIDEO_DECODE_H265_REF_PIC_SET_LIST_SIZE, "HEVC RPS helper did not cap short-term references to the Vulkan list size");

        const uint8_t expected_slots[] = {7, 6, 5, 4, 3, 2, 1, 0};
        for (uint32_t i = 0; i < STD_VIDEO_DECODE_H265_REF_PIC_SET_LIST_SIZE; i++) {
            ok = check(picture.RefPicSetStCurrBefore[i] == expected_slots[i], "HEVC capped RPS entries were not unique and sorted") && ok;
        }

        return ok;
    }

} // namespace

int main(void) {
    bool ok = check_hevc_dpb_slots();
    ok      = check_hevc_reference_info() && ok;
    ok      = check_hevc_session_profiles() && ok;
    ok      = check_hevc_rps_ordering() && ok;
    ok      = check_hevc_rps_capacity_and_uniqueness() && ok;

    if (!ok) {
        return 1;
    }

    std::printf("HEVC RPS and DPB smoke passed\n");
    return 0;
}
