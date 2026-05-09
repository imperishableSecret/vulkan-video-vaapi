#include "va/private.h"
#include "vulkan/codecs/h264/api.h"
#include "vulkan/runtime_internal.h"

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <new>

namespace {

    bool check(bool condition, const char* message) {
        if (!condition) {
            std::fprintf(stderr, "%s\n", message);
        }
        return condition;
    }

    bool check_va(VAStatus status, VAStatus expected, const char* label) {
        if (status != expected) {
            std::fprintf(stderr, "%s: expected %s got %s\n", label, vaErrorStr(expected), vaErrorStr(status));
            return false;
        }
        return true;
    }

    bool has_annexb_start_code(const unsigned char* bytes, unsigned int size) {
        if (bytes == nullptr || size < 3) {
            return false;
        }
        if (bytes[0] == 0x00 && bytes[1] == 0x00 && bytes[2] == 0x01) {
            return true;
        }
        return size >= 4 && bytes[0] == 0x00 && bytes[1] == 0x00 && bytes[2] == 0x00 && bytes[3] == 0x01;
    }

    bool check_coded_segment(const VACodedBufferSegment* segment) {
        if (!check(segment != nullptr && segment->size > 0 && segment->buf != nullptr, "encoded coded segment is empty")) {
            return false;
        }
        const auto* bytes    = static_cast<const unsigned char*>(segment->buf);
        bool        non_zero = false;
        for (unsigned int i = 0; i < segment->size; i++) {
            non_zero = non_zero || bytes[i] != 0;
        }
        return check(non_zero, "encoded coded segment was all zeroes") && check(has_annexb_start_code(bytes, segment->size), "encoded coded segment did not start with Annex-B");
    }

    VASurfaceID add_test_surface(VkvvDriver* drv, unsigned int width, unsigned int height, uint64_t stream_id) {
        auto* surface = new (std::nothrow) VkvvSurface();
        if (surface == nullptr) {
            return VA_INVALID_ID;
        }
        surface->driver_instance_id = drv->driver_instance_id;
        surface->stream_id          = stream_id;
        surface->rt_format          = VA_RT_FORMAT_YUV420;
        surface->width              = width;
        surface->height             = height;
        surface->fourcc             = VA_FOURCC_NV12;
        surface->work_state         = VKVV_SURFACE_WORK_READY;
        surface->sync_status        = VA_STATUS_SUCCESS;
        const VASurfaceID id        = vkvv_object_add(drv, VKVV_OBJECT_SURFACE, surface);
        if (id == VA_INVALID_ID) {
            delete surface;
            return VA_INVALID_ID;
        }
        surface->id = id;
        return id;
    }

    bool add_test_context(VkvvDriver* drv, VAContextID* context_id) {
        auto* context = new (std::nothrow) VkvvContext();
        if (context == nullptr) {
            return false;
        }
        *context_id = vkvv_object_add(drv, VKVV_OBJECT_CONTEXT, context);
        return *context_id != VA_INVALID_ID;
    }

    VkvvH264EncodeInput make_input(VAEncSequenceParameterBufferH264* sequence, VAEncPictureParameterBufferH264* picture, VAEncSliceParameterBufferH264* slice,
                                   VASurfaceID input_surface, VASurfaceID reconstructed_surface, VABufferID coded_buffer) {
        *sequence                                                   = {};
        sequence->seq_parameter_set_id                              = 0;
        sequence->level_idc                                         = STD_VIDEO_H264_LEVEL_IDC_4_1;
        sequence->intra_period                                      = 30;
        sequence->intra_idr_period                                  = 30;
        sequence->ip_period                                         = 1;
        sequence->bits_per_second                                   = 4'000'000;
        sequence->max_num_ref_frames                                = 1;
        sequence->picture_width_in_mbs                              = 4;
        sequence->picture_height_in_mbs                             = 4;
        sequence->seq_fields.bits.chroma_format_idc                 = 1;
        sequence->seq_fields.bits.frame_mbs_only_flag               = 1;
        sequence->seq_fields.bits.direct_8x8_inference_flag         = 1;
        sequence->seq_fields.bits.log2_max_frame_num_minus4         = 4;
        sequence->seq_fields.bits.pic_order_cnt_type                = 0;
        sequence->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 = 4;
        sequence->vui_parameters_present_flag                       = 1;
        sequence->vui_fields.bits.timing_info_present_flag          = 1;
        sequence->vui_fields.bits.fixed_frame_rate_flag             = 1;
        sequence->num_units_in_tick                                 = 1;
        sequence->time_scale                                        = 60;

        *picture                                                        = {};
        picture->CurrPic.picture_id                                     = reconstructed_surface;
        picture->CurrPic.frame_idx                                      = 0;
        picture->CurrPic.TopFieldOrderCnt                               = 0;
        picture->coded_buf                                              = coded_buffer;
        picture->pic_parameter_set_id                                   = 0;
        picture->seq_parameter_set_id                                   = 0;
        picture->frame_num                                              = 0;
        picture->pic_init_qp                                            = 26;
        picture->pic_fields.bits.idr_pic_flag                           = 1;
        picture->pic_fields.bits.reference_pic_flag                     = 1;
        picture->pic_fields.bits.deblocking_filter_control_present_flag = 1;

        *slice                               = {};
        slice->macroblock_address            = 0;
        slice->num_macroblocks               = 16;
        slice->macroblock_info               = VA_INVALID_ID;
        slice->slice_type                    = 2;
        slice->pic_parameter_set_id          = 0;
        slice->idr_pic_id                    = 0;
        slice->disable_deblocking_filter_idc = 0;

        VkvvH264EncodeInput input{};
        input.sequence              = sequence;
        input.picture               = picture;
        input.slices                = slice;
        input.slice_count           = 1;
        input.width                 = 64;
        input.height                = 64;
        input.input_surface         = input_surface;
        input.reconstructed_surface = reconstructed_surface;
        input.coded_buffer          = coded_buffer;
        input.frame_type            = VKVV_H264_ENCODE_FRAME_IDR;
        return input;
    }

} // namespace

int main(void) {
    setenv("VKVV_ENABLE_ENCODE", "1", 1);

    char  reason[1024]{};
    void* runtime_ptr = vkvv_vulkan_runtime_create(reason, sizeof(reason));
    std::printf("%s\n", reason);
    if (runtime_ptr == nullptr) {
        unsetenv("VKVV_ENABLE_ENCODE");
        return 1;
    }

    auto* runtime = static_cast<vkvv::VulkanRuntime*>(runtime_ptr);
    if ((runtime->enabled_encode_operations & VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR) == 0) {
        std::printf("H.264 encode runtime unavailable, skipping encode submit smoke\n");
        vkvv_vulkan_runtime_destroy(runtime_ptr);
        unsetenv("VKVV_ENABLE_ENCODE");
        return 0;
    }

    bool            ok = true;
    VkvvDriver      drv{};
    VADriverContext ctx{};
    drv.next_id            = 1;
    drv.driver_instance_id = 23;
    drv.vulkan             = runtime_ptr;
    ctx.pDriverData        = &drv;

    VAContextID object_context = VA_INVALID_ID;
    ok                         = check(add_test_context(&drv, &object_context), "failed to add object context") && ok;

    VAImageFormat format{};
    format.fourcc         = VA_FOURCC_NV12;
    format.byte_order     = VA_LSB_FIRST;
    format.bits_per_pixel = 12;

    VAImage image{};
    ok                 = check_va(vkvvCreateImage(&ctx, &format, 64, 64, &image), VA_STATUS_SUCCESS, "create NV12 image") && ok;
    void* mapped_image = nullptr;
    ok                 = check_va(vkvvMapBuffer(&ctx, image.buf, &mapped_image), VA_STATUS_SUCCESS, "map image buffer") && ok;
    if (mapped_image != nullptr) {
        std::memset(mapped_image, 0x10, image.offsets[1]);
        std::memset(static_cast<unsigned char*>(mapped_image) + image.offsets[1], 0x80, image.data_size - image.offsets[1]);
    }
    ok = check_va(vkvvUnmapBuffer(&ctx, image.buf), VA_STATUS_SUCCESS, "unmap image buffer") && ok;

    VkvvContext vctx{};
    vctx.stream_id       = 1;
    vctx.codec_operation = VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR;

    const VASurfaceID input_surface = add_test_surface(&drv, 64, 64, vctx.stream_id);
    const VASurfaceID recon_surface = add_test_surface(&drv, 64, 64, vctx.stream_id);
    ok                              = check(input_surface != VA_INVALID_ID && recon_surface != VA_INVALID_ID, "failed to add encode surfaces") && ok;
    vctx.render_target              = input_surface;
    ok                              = check_va(vkvvPutImage(&ctx, input_surface, image.image_id, 0, 0, 64, 64, 0, 0, 64, 64), VA_STATUS_SUCCESS, "upload encode input") && ok;

    VABufferID coded_buffer = VA_INVALID_ID;
    ok = check_va(vkvvCreateBuffer(&ctx, object_context, VAEncCodedBufferType, 1024 * 1024, 1, nullptr, &coded_buffer), VA_STATUS_SUCCESS, "create coded buffer") && ok;

    void* session = vkvv_vulkan_h264_encode_session_create();
    ok            = check(session != nullptr, "failed to create H.264 encode session") && ok;

    VAEncSequenceParameterBufferH264 sequence{};
    VAEncPictureParameterBufferH264  picture{};
    VAEncSliceParameterBufferH264    slice{};
    VkvvH264EncodeInput              input = make_input(&sequence, &picture, &slice, input_surface, recon_surface, coded_buffer);
    ok = check_va(vkvv_vulkan_ensure_h264_encode_session(runtime_ptr, session, &input, reason, sizeof(reason)), VA_STATUS_SUCCESS, "ensure encode session") && ok;
    if (!ok) {
        std::fprintf(stderr, "%s\n", reason);
    }
    ok = check_va(vkvv_vulkan_encode_h264(runtime_ptr, session, &drv, &vctx, &input, reason, sizeof(reason)), VA_STATUS_SUCCESS, "submit H.264 encode") && ok;
    if (!ok) {
        std::fprintf(stderr, "%s\n", reason);
    }

    ok                 = check_va(vkvvSyncBuffer(&ctx, coded_buffer, UINT64_MAX), VA_STATUS_SUCCESS, "sync coded buffer") && ok;
    void* mapped_coded = nullptr;
    ok                 = check_va(vkvvMapBuffer(&ctx, coded_buffer, &mapped_coded), VA_STATUS_SUCCESS, "map coded buffer") && ok;
    auto* segment      = static_cast<VACodedBufferSegment*>(mapped_coded);
    ok                 = check_coded_segment(segment) && ok;
    ok                 = check_va(vkvvUnmapBuffer(&ctx, coded_buffer), VA_STATUS_SUCCESS, "unmap coded buffer") && ok;

    picture.pic_fields.bits.idr_pic_flag = 0;
    slice.slice_type                     = 0;
    for (VAPictureH264& ref : picture.ReferenceFrames) {
        ref.picture_id = VA_INVALID_ID;
        ref.flags      = VA_PICTURE_H264_INVALID;
    }
    for (VAPictureH264& ref : slice.RefPicList0) {
        ref.picture_id = VA_INVALID_ID;
        ref.flags      = VA_PICTURE_H264_INVALID;
    }
    input.frame_type = VKVV_H264_ENCODE_FRAME_P;
    ok               = check_va(vkvv_vulkan_encode_h264(runtime_ptr, session, &drv, &vctx, &input, reason, sizeof(reason)), VA_STATUS_ERROR_INVALID_SURFACE,
                                "submit H.264 encode with missing P reference") &&
        ok;
    ok           = check_va(vkvvSyncBuffer(&ctx, coded_buffer, UINT64_MAX), VA_STATUS_ERROR_INVALID_SURFACE, "sync failed coded buffer") && ok;
    mapped_coded = nullptr;
    ok           = check_va(vkvvMapBuffer(&ctx, coded_buffer, &mapped_coded), VA_STATUS_SUCCESS, "map failed coded buffer") && ok;
    segment      = static_cast<VACodedBufferSegment*>(mapped_coded);
    ok           = check(segment != nullptr && segment->size == 0 && segment->buf != nullptr, "failed encode coded segment was not cleared") && ok;
    ok           = check_va(vkvvUnmapBuffer(&ctx, coded_buffer), VA_STATUS_SUCCESS, "unmap failed coded buffer") && ok;

    if (session != nullptr) {
        vkvv_vulkan_h264_encode_session_destroy(runtime_ptr, session);
    }
    if (coded_buffer != VA_INVALID_ID) {
        ok = check_va(vkvvDestroyBuffer(&ctx, coded_buffer), VA_STATUS_SUCCESS, "destroy coded buffer") && ok;
    }
    if (image.image_id != VA_INVALID_ID) {
        ok = check_va(vkvvDestroyImage(&ctx, image.image_id), VA_STATUS_SUCCESS, "destroy image") && ok;
    }
    if (input_surface != VA_INVALID_ID) {
        auto* surface = static_cast<VkvvSurface*>(vkvv_object_get(&drv, input_surface, VKVV_OBJECT_SURFACE));
        vkvv_vulkan_surface_destroy(runtime_ptr, surface);
    }
    if (recon_surface != VA_INVALID_ID) {
        auto* surface = static_cast<VkvvSurface*>(vkvv_object_get(&drv, recon_surface, VKVV_OBJECT_SURFACE));
        vkvv_vulkan_surface_destroy(runtime_ptr, surface);
    }
    vkvv_object_clear(&drv);
    vkvv_vulkan_runtime_destroy(runtime_ptr);
    unsetenv("VKVV_ENABLE_ENCODE");

    if (!ok) {
        return 1;
    }
    std::printf("H.264 encode submit smoke passed\n");
    return 0;
}
