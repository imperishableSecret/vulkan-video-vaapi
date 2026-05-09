#include "codecs/h264/encode.h"
#include "va/private.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <vector>

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

    VkvvBuffer buffer_for(VABufferType type, void* data, unsigned int size, unsigned int elements = 1) {
        VkvvBuffer buffer{};
        buffer.type         = type;
        buffer.buffer_class = VKVV_BUFFER_CLASS_PARAMETER;
        buffer.size         = size;
        buffer.num_elements = elements;
        buffer.data         = data;
        return buffer;
    }

    VAEncSequenceParameterBufferH264 sequence(unsigned int width_mbs = 40, unsigned int height_mbs = 30) {
        VAEncSequenceParameterBufferH264 seq{};
        seq.level_idc                                 = 40;
        seq.intra_period                              = 30;
        seq.intra_idr_period                          = 30;
        seq.ip_period                                 = 1;
        seq.bits_per_second                           = 4000000;
        seq.max_num_ref_frames                        = 1;
        seq.picture_width_in_mbs                      = static_cast<uint16_t>(width_mbs);
        seq.picture_height_in_mbs                     = static_cast<uint16_t>(height_mbs);
        seq.seq_fields.bits.chroma_format_idc         = 1;
        seq.seq_fields.bits.frame_mbs_only_flag       = 1;
        seq.seq_fields.bits.direct_8x8_inference_flag = 1;
        return seq;
    }

    VAEncPictureParameterBufferH264 picture(VABufferID coded_buffer, bool idr = true) {
        VAEncPictureParameterBufferH264 pic{};
        pic.CurrPic.picture_id                      = 77;
        pic.coded_buf                               = coded_buffer;
        pic.pic_init_qp                             = 26;
        pic.pic_fields.bits.idr_pic_flag            = idr ? 1 : 0;
        pic.pic_fields.bits.reference_pic_flag      = 1;
        pic.pic_fields.bits.transform_8x8_mode_flag = 1;
        return pic;
    }

    VAEncSliceParameterBufferH264 slice(uint8_t slice_type = 2) {
        VAEncSliceParameterBufferH264 out{};
        out.num_macroblocks      = 40 * 30;
        out.macroblock_info      = VA_INVALID_ID;
        out.slice_type           = slice_type;
        out.pic_parameter_set_id = 0;
        out.idr_pic_id           = 1;
        return out;
    }

    bool add_test_context(VkvvDriver* drv, VAContextID* context_id) {
        auto* vctx = new (std::nothrow) VkvvContext();
        if (vctx == nullptr) {
            return false;
        }
        *context_id = vkvv_object_add(drv, VKVV_OBJECT_CONTEXT, vctx);
        return *context_id != VA_INVALID_ID;
    }

    bool render_minimal_h264(const VkvvEncodeOps* ops, void* state, VABufferID coded_buffer, uint8_t slice_type = 2, bool idr = true) {
        VAEncSequenceParameterBufferH264 seq = sequence();
        VAEncPictureParameterBufferH264  pic = picture(coded_buffer, idr);
        VAEncSliceParameterBufferH264    slc = slice(slice_type);

        VkvvBuffer                       seq_buffer = buffer_for(VAEncSequenceParameterBufferType, &seq, sizeof(seq));
        VkvvBuffer                       pic_buffer = buffer_for(VAEncPictureParameterBufferType, &pic, sizeof(pic));
        VkvvBuffer                       slc_buffer = buffer_for(VAEncSliceParameterBufferType, &slc, sizeof(slc));
        return ops->render_buffer(state, &seq_buffer) == VA_STATUS_SUCCESS && ops->render_buffer(state, &pic_buffer) == VA_STATUS_SUCCESS &&
            ops->render_buffer(state, &slc_buffer) == VA_STATUS_SUCCESS;
    }

    bool render_rate_control(const VkvvEncodeOps* ops, void* state) {
        std::vector<uint8_t> misc(sizeof(VAEncMiscParameterBuffer) + sizeof(VAEncMiscParameterRateControl));
        auto*                header = reinterpret_cast<VAEncMiscParameterBuffer*>(misc.data());
        header->type                = VAEncMiscParameterTypeRateControl;
        auto* rc                    = reinterpret_cast<VAEncMiscParameterRateControl*>(header->data);
        rc->bits_per_second         = 7000000;
        rc->target_percentage       = 80;
        rc->initial_qp              = 24;
        rc->min_qp                  = 18;
        rc->max_qp                  = 38;

        VkvvBuffer misc_buffer = buffer_for(VAEncMiscParameterBufferType, misc.data(), static_cast<unsigned int>(misc.size()));
        return ops->render_buffer(state, &misc_buffer) == VA_STATUS_SUCCESS;
    }

} // namespace

int main(void) {
    bool            ok = true;

    VkvvDriver      drv{};
    VADriverContext ctx{};
    drv.next_id     = 1;
    ctx.pDriverData = &drv;

    VAContextID object_context = VA_INVALID_ID;
    ok                         = check(add_test_context(&drv, &object_context), "failed to add object context") && ok;

    VABufferID coded_buffer = VA_INVALID_ID;
    ok = check_va(vkvvCreateBuffer(&ctx, object_context, VAEncCodedBufferType, 4096, 1, nullptr, &coded_buffer), VA_STATUS_SUCCESS, "create coded buffer") && ok;

    VkvvContext vctx{};
    vctx.render_target = 42;

    const VkvvEncodeOps ops = {
        "h264-encode-smoke",      vkvv_h264_encode_state_create, vkvv_h264_encode_state_destroy, vkvv_h264_encode_begin_picture, vkvv_h264_encode_render_buffer,
        vkvv_h264_encode_prepare,
    };

    void* state = ops.state_create();
    ok          = check(state != nullptr, "H.264 encode state allocation failed") && ok;

    if (state != nullptr) {
        ops.begin_picture(state);
        ok = check(render_minimal_h264(&ops, state, coded_buffer), "minimal H.264 encode buffers were rejected") && ok;
        ok = check(render_rate_control(&ops, state), "H.264 rate-control misc buffer was rejected") && ok;

        unsigned int width       = 0;
        unsigned int height      = 0;
        VABufferID   coded       = VA_INVALID_ID;
        char         reason[256] = {};
        ok = check_va(ops.prepare_encode(state, &drv, &vctx, &width, &height, &coded, reason, sizeof(reason)), VA_STATUS_SUCCESS, "prepare minimal encode") && ok;
        std::printf("%s\n", reason);
        ok = check(width == 640 && height == 480 && coded == coded_buffer, "prepared H.264 encode dimensions/coded buffer were wrong") && ok;

        VkvvH264EncodeInput input{};
        ok = check_va(vkvv_h264_encode_get_input(state, &drv, &vctx, &input, reason, sizeof(reason)), VA_STATUS_SUCCESS, "get H.264 encode input") && ok;
        ok = check(input.frame_type == VKVV_H264_ENCODE_FRAME_IDR && input.has_rate_control && input.rate_control != nullptr && input.rate_control->bits_per_second == 7000000,
                   "H.264 encode input did not preserve frame type or rate control") &&
            ok;

        ops.begin_picture(state);
        VAEncPictureParameterBufferH264  pic_missing_coded = picture(VA_INVALID_ID);
        VAEncSequenceParameterBufferH264 seq               = sequence();
        VAEncSliceParameterBufferH264    slc               = slice();
        VkvvBuffer                       seq_buffer        = buffer_for(VAEncSequenceParameterBufferType, &seq, sizeof(seq));
        VkvvBuffer                       pic_buffer        = buffer_for(VAEncPictureParameterBufferType, &pic_missing_coded, sizeof(pic_missing_coded));
        VkvvBuffer                       slc_buffer        = buffer_for(VAEncSliceParameterBufferType, &slc, sizeof(slc));
        (void)ops.render_buffer(state, &seq_buffer);
        (void)ops.render_buffer(state, &pic_buffer);
        (void)ops.render_buffer(state, &slc_buffer);
        ok = check_va(ops.prepare_encode(state, &drv, &vctx, &width, &height, &coded, reason, sizeof(reason)), VA_STATUS_ERROR_INVALID_BUFFER, "missing coded buffer") && ok;

        ops.begin_picture(state);
        ok = check(render_minimal_h264(&ops, state, coded_buffer, 1, false), "B-slice H.264 encode buffers were rejected too early") && ok;
        ok = check_va(ops.prepare_encode(state, &drv, &vctx, &width, &height, &coded, reason, sizeof(reason)), VA_STATUS_ERROR_UNIMPLEMENTED, "B-slice rejection") && ok;

        ops.state_destroy(state);
    }

    if (coded_buffer != VA_INVALID_ID) {
        ok = check_va(vkvvDestroyBuffer(&ctx, coded_buffer), VA_STATUS_SUCCESS, "destroy coded buffer") && ok;
    }
    vkvv_object_clear(&drv);

    if (!ok) {
        return 1;
    }
    std::printf("H.264 encode parser smoke passed\n");
    return 0;
}
