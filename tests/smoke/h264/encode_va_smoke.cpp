#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_enc_h264.h>
#include <vector>

namespace {

    int open_render_node(void) {
        const char* override_path = std::getenv("VKVV_RENDER_NODE");
        if (override_path != nullptr && override_path[0] != '\0') {
            int fd = open(override_path, O_RDWR | O_CLOEXEC);
            if (fd < 0) {
                std::fprintf(stderr, "failed to open %s: %s\n", override_path, std::strerror(errno));
            }
            return fd;
        }

        for (int i = 128; i < 138; i++) {
            std::string path = "/dev/dri/renderD" + std::to_string(i);
            int         fd   = open(path.c_str(), O_RDWR | O_CLOEXEC);
            if (fd >= 0) {
                std::printf("using render node %s\n", path.c_str());
                return fd;
            }
        }

        std::fprintf(stderr, "failed to open a DRM render node under /dev/dri/renderD128..137\n");
        return -1;
    }

    bool check(bool condition, const char* message) {
        if (!condition) {
            std::fprintf(stderr, "%s\n", message);
        }
        return condition;
    }

    bool check_va(VAStatus status, const char* operation) {
        if (status == VA_STATUS_SUCCESS) {
            return true;
        }
        std::fprintf(stderr, "%s returned %s (%d)\n", operation, vaErrorStr(status), status);
        return false;
    }

    bool expect_va(VAStatus status, VAStatus expected, const char* operation) {
        if (status == expected) {
            return true;
        }
        std::fprintf(stderr, "%s returned %s (%d), expected %s (%d)\n", operation, vaErrorStr(status), status, vaErrorStr(expected), expected);
        return false;
    }

    bool entrypoint_present(const VAEntrypoint* entrypoints, int count, VAEntrypoint expected) {
        for (int i = 0; i < count; i++) {
            if (entrypoints[i] == expected) {
                return true;
            }
        }
        return false;
    }

    void fill_nv12(VAImage* image, void* mapped) {
        if (image == nullptr || mapped == nullptr) {
            return;
        }
        auto* bytes = static_cast<uint8_t*>(mapped);
        std::memset(bytes, 0x10, image->offsets[1]);
        std::memset(bytes + image->offsets[1], 0x80, image->data_size - image->offsets[1]);
    }

    VAEncSequenceParameterBufferH264 make_sequence(void) {
        VAEncSequenceParameterBufferH264 sequence{};
        sequence.seq_parameter_set_id                              = 0;
        sequence.level_idc                                         = 41;
        sequence.intra_period                                      = 30;
        sequence.intra_idr_period                                  = 30;
        sequence.ip_period                                         = 1;
        sequence.bits_per_second                                   = 4'000'000;
        sequence.max_num_ref_frames                                = 1;
        sequence.picture_width_in_mbs                              = 4;
        sequence.picture_height_in_mbs                             = 4;
        sequence.seq_fields.bits.chroma_format_idc                 = 1;
        sequence.seq_fields.bits.frame_mbs_only_flag               = 1;
        sequence.seq_fields.bits.direct_8x8_inference_flag         = 1;
        sequence.seq_fields.bits.log2_max_frame_num_minus4         = 4;
        sequence.seq_fields.bits.pic_order_cnt_type                = 0;
        sequence.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 = 4;
        sequence.vui_parameters_present_flag                       = 1;
        sequence.vui_fields.bits.timing_info_present_flag          = 1;
        sequence.vui_fields.bits.fixed_frame_rate_flag             = 1;
        sequence.num_units_in_tick                                 = 1;
        sequence.time_scale                                        = 60;
        return sequence;
    }

    VAPictureH264 invalid_picture(void) {
        VAPictureH264 picture{};
        picture.picture_id = VA_INVALID_ID;
        picture.flags      = VA_PICTURE_H264_INVALID;
        return picture;
    }

    VAPictureH264 reference_picture(VASurfaceID surface, uint16_t frame_idx, int32_t poc) {
        VAPictureH264 picture{};
        picture.picture_id       = surface;
        picture.frame_idx        = frame_idx;
        picture.TopFieldOrderCnt = poc;
        picture.flags            = VA_PICTURE_H264_SHORT_TERM_REFERENCE;
        return picture;
    }

    VAEncPictureParameterBufferH264 make_picture(VASurfaceID reconstructed_surface, VABufferID coded_buffer, bool idr, uint16_t frame_num, int32_t poc,
                                                 VASurfaceID reference_surface = VA_INVALID_ID, uint16_t reference_frame_idx = 0, int32_t reference_poc = 0) {
        VAEncPictureParameterBufferH264 picture{};
        picture.CurrPic.picture_id                                     = reconstructed_surface;
        picture.CurrPic.frame_idx                                      = frame_num;
        picture.CurrPic.TopFieldOrderCnt                               = poc;
        picture.coded_buf                                              = coded_buffer;
        picture.pic_parameter_set_id                                   = 0;
        picture.seq_parameter_set_id                                   = 0;
        picture.frame_num                                              = frame_num;
        picture.pic_init_qp                                            = 26;
        picture.pic_fields.bits.idr_pic_flag                           = idr ? 1 : 0;
        picture.pic_fields.bits.reference_pic_flag                     = 1;
        picture.pic_fields.bits.deblocking_filter_control_present_flag = 1;
        for (VAPictureH264& ref : picture.ReferenceFrames) {
            ref = invalid_picture();
        }
        if (reference_surface != VA_INVALID_ID) {
            picture.ReferenceFrames[0] = reference_picture(reference_surface, reference_frame_idx, reference_poc);
        }
        return picture;
    }

    VAEncSliceParameterBufferH264 make_slice(uint8_t slice_type, VASurfaceID reference_surface = VA_INVALID_ID, uint16_t reference_frame_idx = 0, int32_t reference_poc = 0) {
        VAEncSliceParameterBufferH264 slice{};
        slice.macroblock_address            = 0;
        slice.num_macroblocks               = 16;
        slice.macroblock_info               = VA_INVALID_ID;
        slice.slice_type                    = slice_type;
        slice.pic_parameter_set_id          = 0;
        slice.idr_pic_id                    = 0;
        slice.disable_deblocking_filter_idc = 0;
        for (VAPictureH264& ref : slice.RefPicList0) {
            ref = invalid_picture();
        }
        for (VAPictureH264& ref : slice.RefPicList1) {
            ref = invalid_picture();
        }
        if (reference_surface != VA_INVALID_ID) {
            slice.num_ref_idx_active_override_flag = 1;
            slice.num_ref_idx_l0_active_minus1     = 0;
            slice.RefPicList0[0]                   = reference_picture(reference_surface, reference_frame_idx, reference_poc);
        }
        return slice;
    }

} // namespace

int main(void) {
    int fd = open_render_node();
    if (fd < 0) {
        return 1;
    }

    VADisplay display = vaGetDisplayDRM(fd);
    if (!vaDisplayIsValid(display)) {
        std::fprintf(stderr, "vaGetDisplayDRM returned an invalid display\n");
        close(fd);
        return 1;
    }

    int  major = 0;
    int  minor = 0;
    bool ok    = check_va(vaInitialize(display, &major, &minor), "vaInitialize");
    if (!ok) {
        close(fd);
        return 1;
    }

    VAEntrypoint entrypoints[4]   = {};
    int          entrypoint_count = 0;
    ok                            = check_va(vaQueryConfigEntrypoints(display, VAProfileH264High, entrypoints, &entrypoint_count), "vaQueryConfigEntrypoints(H264High)") && ok;
    ok                            = check(entrypoint_present(entrypoints, entrypoint_count, VAEntrypointVLD), "H.264 VLD entrypoint disappeared") && ok;
    ok                            = check(entrypoint_present(entrypoints, entrypoint_count, VAEntrypointEncSlice), "H.264 EncSlice entrypoint was not advertised") && ok;
    ok                            = check(!entrypoint_present(entrypoints, entrypoint_count, VAEntrypointEncSliceLP), "H.264 EncSliceLP should stay hidden") && ok;
    ok                            = check(!entrypoint_present(entrypoints, entrypoint_count, VAEntrypointEncPicture), "H.264 EncPicture should stay hidden") && ok;

    VAConfigAttrib rt_attrib{};
    rt_attrib.type = VAConfigAttribRTFormat;
    ok             = check_va(vaGetConfigAttributes(display, VAProfileH264High, VAEntrypointEncSlice, &rt_attrib, 1), "vaGetConfigAttributes(H264 EncSlice)") && ok;
    ok             = check((rt_attrib.value & VA_RT_FORMAT_YUV420) != 0, "H.264 EncSlice did not expose YUV420 RTFormat") && ok;

    VAConfigAttrib encode_attribs[6]{};
    encode_attribs[0].type = VAConfigAttribRateControl;
    encode_attribs[1].type = VAConfigAttribEncMaxRefFrames;
    encode_attribs[2].type = VAConfigAttribEncMaxSlices;
    encode_attribs[3].type = VAConfigAttribEncSliceStructure;
    encode_attribs[4].type = VAConfigAttribEncQualityRange;
    encode_attribs[5].type = VAConfigAttribEncPackedHeaders;
    ok                     = check_va(vaGetConfigAttributes(display, VAProfileH264High, VAEntrypointEncSlice, encode_attribs, 6), "vaGetConfigAttributes(H264 encode attrs)") && ok;
    ok                     = check(encode_attribs[0].value == VA_RC_CQP, "H.264 EncSlice should expose CQP only") && ok;
    ok                     = check((encode_attribs[1].value & 0xffffu) >= 1, "H.264 EncSlice should expose at least one L0 reference") && ok;
    ok                     = check(encode_attribs[2].value == 1, "H.264 EncSlice should expose one slice") && ok;
    ok                     = check((encode_attribs[3].value & VA_ENC_SLICE_STRUCTURE_ARBITRARY_MACROBLOCKS) != 0, "H.264 EncSlice should expose macroblock slice structure") && ok;
    ok                     = check(encode_attribs[4].value == 1, "H.264 EncSlice should expose one quality level") && ok;
    ok                     = check(encode_attribs[5].value == VA_ENC_PACKED_HEADER_NONE, "H.264 EncSlice should not expose packed headers yet") && ok;

    VAConfigAttrib cbr_attribs[2]{};
    cbr_attribs[0].type   = VAConfigAttribRTFormat;
    cbr_attribs[0].value  = VA_RT_FORMAT_YUV420;
    cbr_attribs[1].type   = VAConfigAttribRateControl;
    cbr_attribs[1].value  = VA_RC_CBR;
    VAConfigID bad_config = VA_INVALID_ID;
    ok                    = expect_va(vaCreateConfig(display, VAProfileH264High, VAEntrypointEncSlice, cbr_attribs, 2, &bad_config), VA_STATUS_ERROR_ATTR_NOT_SUPPORTED,
                                      "vaCreateConfig(H264 EncSlice CBR)") &&
        ok;

    VAConfigAttrib create_attribs[2]{};
    create_attribs[0].type  = VAConfigAttribRTFormat;
    create_attribs[0].value = VA_RT_FORMAT_YUV420;
    create_attribs[1].type  = VAConfigAttribRateControl;
    create_attribs[1].value = VA_RC_CQP;
    VAConfigID config       = VA_INVALID_ID;
    ok                      = check_va(vaCreateConfig(display, VAProfileH264High, VAEntrypointEncSlice, create_attribs, 2, &config), "vaCreateConfig(H264 EncSlice CQP)") && ok;

    VASurfaceID surfaces[3] = {VA_INVALID_SURFACE, VA_INVALID_SURFACE, VA_INVALID_SURFACE};
    VAContextID context     = VA_INVALID_ID;
    VAImage     image{};
    VABufferID  sequence_buffer   = VA_INVALID_ID;
    VABufferID  picture_buffer    = VA_INVALID_ID;
    VABufferID  slice_buffer      = VA_INVALID_ID;
    VABufferID  coded_buffer      = VA_INVALID_ID;
    VABufferID  p_picture_buffer  = VA_INVALID_ID;
    VABufferID  p_slice_buffer    = VA_INVALID_ID;
    VABufferID  p2_picture_buffer = VA_INVALID_ID;
    VABufferID  p2_slice_buffer   = VA_INVALID_ID;

    if (ok) {
        ok = check_va(vaCreateSurfaces(display, VA_RT_FORMAT_YUV420, 64, 64, surfaces, 3, nullptr, 0), "vaCreateSurfaces(H264 encode)") && ok;
        ok = check_va(vaCreateContext(display, config, 64, 64, 0, surfaces, 3, &context), "vaCreateContext(H264 encode)") && ok;
    }

    if (ok) {
        VAImageFormat image_format{};
        image_format.fourcc         = VA_FOURCC_NV12;
        image_format.byte_order     = VA_LSB_FIRST;
        image_format.bits_per_pixel = 12;
        ok                          = check_va(vaCreateImage(display, &image_format, 64, 64, &image), "vaCreateImage(NV12)") && ok;

        void* mapped = nullptr;
        ok           = check_va(vaMapBuffer(display, image.buf, &mapped), "vaMapBuffer(image)") && ok;
        fill_nv12(&image, mapped);
        ok = check_va(vaUnmapBuffer(display, image.buf), "vaUnmapBuffer(image)") && ok;
        ok = check_va(vaPutImage(display, surfaces[0], image.image_id, 0, 0, 64, 64, 0, 0, 64, 64), "vaPutImage(encode input)") && ok;
    }

    if (ok) {
        ok = check_va(vaCreateBuffer(display, context, VAEncCodedBufferType, 1024 * 1024, 1, nullptr, &coded_buffer), "vaCreateBuffer(coded)") && ok;
        VAEncSequenceParameterBufferH264 sequence = make_sequence();
        VAEncPictureParameterBufferH264  picture  = make_picture(surfaces[1], coded_buffer, true, 0, 0);
        VAEncSliceParameterBufferH264    slice    = make_slice(2);

        ok = check_va(vaCreateBuffer(display, context, VAEncSequenceParameterBufferType, sizeof(sequence), 1, &sequence, &sequence_buffer), "vaCreateBuffer(sequence)") && ok;
        ok = check_va(vaCreateBuffer(display, context, VAEncPictureParameterBufferType, sizeof(picture), 1, &picture, &picture_buffer), "vaCreateBuffer(picture)") && ok;
        ok = check_va(vaCreateBuffer(display, context, VAEncSliceParameterBufferType, sizeof(slice), 1, &slice, &slice_buffer), "vaCreateBuffer(slice)") && ok;
    }

    if (ok) {
        VABufferID render_buffers[] = {sequence_buffer, picture_buffer, slice_buffer};
        ok                          = check_va(vaBeginPicture(display, context, surfaces[0]), "vaBeginPicture(H264 encode)") && ok;
        ok                          = check_va(vaRenderPicture(display, context, render_buffers, 3), "vaRenderPicture(H264 encode)") && ok;
        ok                          = check_va(vaEndPicture(display, context), "vaEndPicture(H264 encode)") && ok;
        ok                          = check_va(vaSyncBuffer(display, coded_buffer, UINT64_MAX), "vaSyncBuffer(coded)") && ok;

        void* mapped_coded = nullptr;
        ok                 = check_va(vaMapBuffer(display, coded_buffer, &mapped_coded), "vaMapBuffer(coded)") && ok;
        auto* segment      = static_cast<VACodedBufferSegment*>(mapped_coded);
        ok                 = check(segment != nullptr && segment->size > 0 && segment->buf != nullptr, "encoded coded segment is empty") && ok;
        if (segment != nullptr && segment->buf != nullptr) {
            const auto* bytes    = static_cast<const uint8_t*>(segment->buf);
            bool        non_zero = false;
            for (unsigned int i = 0; i < segment->size; i++) {
                non_zero = non_zero || bytes[i] != 0;
            }
            ok = check(non_zero, "encoded coded segment was all zeroes") && ok;
        }
        ok = check_va(vaUnmapBuffer(display, coded_buffer), "vaUnmapBuffer(coded)") && ok;
    }

    if (ok) {
        ok                                        = check_va(vaPutImage(display, surfaces[0], image.image_id, 0, 0, 64, 64, 0, 0, 64, 64), "vaPutImage(P encode input)") && ok;
        VAEncPictureParameterBufferH264 p_picture = make_picture(surfaces[2], coded_buffer, false, 1, 2, surfaces[1]);
        VAEncSliceParameterBufferH264   p_slice   = make_slice(0, surfaces[1]);

        ok = check_va(vaCreateBuffer(display, context, VAEncPictureParameterBufferType, sizeof(p_picture), 1, &p_picture, &p_picture_buffer), "vaCreateBuffer(P picture)") && ok;
        ok = check_va(vaCreateBuffer(display, context, VAEncSliceParameterBufferType, sizeof(p_slice), 1, &p_slice, &p_slice_buffer), "vaCreateBuffer(P slice)") && ok;

        VABufferID render_buffers[] = {sequence_buffer, p_picture_buffer, p_slice_buffer};
        ok                          = check_va(vaBeginPicture(display, context, surfaces[0]), "vaBeginPicture(H264 P encode)") && ok;
        ok                          = check_va(vaRenderPicture(display, context, render_buffers, 3), "vaRenderPicture(H264 P encode)") && ok;
        ok                          = check_va(vaEndPicture(display, context), "vaEndPicture(H264 P encode)") && ok;
        ok                          = check_va(vaSyncBuffer(display, coded_buffer, UINT64_MAX), "vaSyncBuffer(reused P coded)") && ok;

        void* mapped_coded = nullptr;
        ok                 = check_va(vaMapBuffer(display, coded_buffer, &mapped_coded), "vaMapBuffer(reused P coded)") && ok;
        auto* segment      = static_cast<VACodedBufferSegment*>(mapped_coded);
        ok                 = check(segment != nullptr && segment->size > 0 && segment->buf != nullptr, "reused P encoded coded segment is empty") && ok;
        ok                 = check_va(vaUnmapBuffer(display, coded_buffer), "vaUnmapBuffer(reused P coded)") && ok;
    }

    if (ok) {
        ok                                         = check_va(vaPutImage(display, surfaces[0], image.image_id, 0, 0, 64, 64, 0, 0, 64, 64), "vaPutImage(P2 encode input)") && ok;
        VAEncPictureParameterBufferH264 p2_picture = make_picture(surfaces[1], coded_buffer, false, 2, 4, surfaces[2], 1, 2);
        VAEncSliceParameterBufferH264   p2_slice   = make_slice(0, surfaces[2], 1, 2);

        ok =
            check_va(vaCreateBuffer(display, context, VAEncPictureParameterBufferType, sizeof(p2_picture), 1, &p2_picture, &p2_picture_buffer), "vaCreateBuffer(P2 picture)") && ok;
        ok = check_va(vaCreateBuffer(display, context, VAEncSliceParameterBufferType, sizeof(p2_slice), 1, &p2_slice, &p2_slice_buffer), "vaCreateBuffer(P2 slice)") && ok;

        VABufferID render_buffers[] = {sequence_buffer, p2_picture_buffer, p2_slice_buffer};
        ok                          = check_va(vaBeginPicture(display, context, surfaces[0]), "vaBeginPicture(H264 P2 encode)") && ok;
        ok                          = check_va(vaRenderPicture(display, context, render_buffers, 3), "vaRenderPicture(H264 P2 encode)") && ok;
        ok                          = check_va(vaEndPicture(display, context), "vaEndPicture(H264 P2 encode)") && ok;
        ok                          = check_va(vaDestroyContext(display, context), "vaDestroyContext(before repeated coded sync)") && ok;
        context                     = VA_INVALID_ID;
        ok                          = check_va(vaSyncBuffer(display, coded_buffer, UINT64_MAX), "vaSyncBuffer(reused P2 coded)") && ok;

        void* mapped_coded = nullptr;
        ok                 = check_va(vaMapBuffer(display, coded_buffer, &mapped_coded), "vaMapBuffer(reused P2 coded)") && ok;
        auto* segment      = static_cast<VACodedBufferSegment*>(mapped_coded);
        ok                 = check(segment != nullptr && segment->size > 0 && segment->buf != nullptr, "reused P2 encoded coded segment is empty") && ok;
        ok                 = check_va(vaUnmapBuffer(display, coded_buffer), "vaUnmapBuffer(reused P2 coded)") && ok;
    }

    if (sequence_buffer != VA_INVALID_ID) {
        ok = check_va(vaDestroyBuffer(display, sequence_buffer), "vaDestroyBuffer(sequence)") && ok;
    }
    if (picture_buffer != VA_INVALID_ID) {
        ok = check_va(vaDestroyBuffer(display, picture_buffer), "vaDestroyBuffer(picture)") && ok;
    }
    if (slice_buffer != VA_INVALID_ID) {
        ok = check_va(vaDestroyBuffer(display, slice_buffer), "vaDestroyBuffer(slice)") && ok;
    }
    if (coded_buffer != VA_INVALID_ID) {
        ok = check_va(vaDestroyBuffer(display, coded_buffer), "vaDestroyBuffer(coded)") && ok;
    }
    if (p_picture_buffer != VA_INVALID_ID) {
        ok = check_va(vaDestroyBuffer(display, p_picture_buffer), "vaDestroyBuffer(P picture)") && ok;
    }
    if (p_slice_buffer != VA_INVALID_ID) {
        ok = check_va(vaDestroyBuffer(display, p_slice_buffer), "vaDestroyBuffer(P slice)") && ok;
    }
    if (p2_picture_buffer != VA_INVALID_ID) {
        ok = check_va(vaDestroyBuffer(display, p2_picture_buffer), "vaDestroyBuffer(P2 picture)") && ok;
    }
    if (p2_slice_buffer != VA_INVALID_ID) {
        ok = check_va(vaDestroyBuffer(display, p2_slice_buffer), "vaDestroyBuffer(P2 slice)") && ok;
    }
    if (image.image_id != VA_INVALID_ID) {
        ok = check_va(vaDestroyImage(display, image.image_id), "vaDestroyImage") && ok;
    }
    if (context != VA_INVALID_ID) {
        ok = check_va(vaDestroyContext(display, context), "vaDestroyContext") && ok;
    }
    if (surfaces[0] != VA_INVALID_SURFACE || surfaces[1] != VA_INVALID_SURFACE || surfaces[2] != VA_INVALID_SURFACE) {
        ok = check_va(vaDestroySurfaces(display, surfaces, 3), "vaDestroySurfaces") && ok;
    }
    if (config != VA_INVALID_ID) {
        ok = check_va(vaDestroyConfig(display, config), "vaDestroyConfig") && ok;
    }
    ok = check_va(vaTerminate(display), "vaTerminate") && ok;
    close(fd);

    if (!ok) {
        return 1;
    }
    std::printf("H.264 VA encode smoke passed\n");
    return 0;
}
