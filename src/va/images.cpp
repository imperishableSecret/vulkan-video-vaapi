#include "va/private.h"
#include "vulkan/runtime.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>

namespace {

    struct LockedSurface {
        explicit LockedSurface(VkvvSurface* surface) : surface(surface) {}
        ~LockedSurface() {
            if (surface != nullptr) {
                vkvv_surface_unlock(surface);
            }
        }

        LockedSurface(const LockedSurface&)            = delete;
        LockedSurface& operator=(const LockedSurface&) = delete;

        VkvvSurface*   surface = nullptr;
    };

    bool supported_image_fourcc(unsigned int fourcc) {
        return fourcc == VA_FOURCC_NV12 || fourcc == VA_FOURCC_P010 || fourcc == VA_FOURCC_P012;
    }

    unsigned int image_bit_depth(unsigned int fourcc) {
        switch (fourcc) {
            case VA_FOURCC_P010: return 10;
            case VA_FOURCC_P012: return 12;
            default: return 8;
        }
    }

    unsigned int image_sample_bytes(unsigned int fourcc) {
        return image_bit_depth(fourcc) > 8 ? 2 : 1;
    }

    bool fill_image_layout(VAImage* image, const VAImageFormat* format, int width, int height) {
        if (image == nullptr || format == nullptr || width <= 0 || height <= 0 || width > UINT16_MAX || height > UINT16_MAX) {
            return false;
        }
        if (!supported_image_fourcc(format->fourcc)) {
            return false;
        }

        const unsigned int sample_bytes = image_sample_bytes(format->fourcc);
        const unsigned int y_pitch      = static_cast<unsigned int>(width) * sample_bytes;
        const unsigned int uv_pitch     = y_pitch;
        const unsigned int uv_height    = (static_cast<unsigned int>(height) + 1u) / 2u;
        const uint32_t     y_size       = y_pitch * static_cast<unsigned int>(height);

        *image               = {};
        image->format        = *format;
        image->format.fourcc = format->fourcc;
        if (image->format.byte_order == 0) {
            image->format.byte_order = VA_LSB_FIRST;
        }
        if (image->format.bits_per_pixel == 0) {
            image->format.bits_per_pixel = image_bit_depth(format->fourcc) > 8 ? 24 : 12;
        }
        image->width      = static_cast<uint16_t>(width);
        image->height     = static_cast<uint16_t>(height);
        image->num_planes = 2;
        image->pitches[0] = y_pitch;
        image->pitches[1] = uv_pitch;
        image->offsets[0] = 0;
        image->offsets[1] = y_size;
        image->data_size  = y_size + uv_pitch * uv_height;
        return true;
    }

    VkvvBuffer* create_image_buffer_payload(uint32_t data_size) {
        auto* buffer = new (std::nothrow) VkvvBuffer();
        if (buffer == nullptr) {
            return nullptr;
        }
        buffer->type         = VAImageBufferType;
        buffer->buffer_class = VKVV_BUFFER_CLASS_PARAMETER;
        buffer->size         = data_size;
        buffer->num_elements = 1;
        buffer->data         = std::calloc(1, data_size);
        if (buffer->data == nullptr) {
            delete buffer;
            return nullptr;
        }
        return buffer;
    }

    bool full_frame_copy(const VAImage* image, const VkvvSurface* surface, int src_x, int src_y, unsigned int src_width, unsigned int src_height, int dest_x, int dest_y,
                         unsigned int dest_width, unsigned int dest_height) {
        return image != nullptr && surface != nullptr && src_x == 0 && src_y == 0 && dest_x == 0 && dest_y == 0 && src_width == image->width && src_height == image->height &&
            dest_width == surface->width && dest_height == surface->height && src_width == dest_width && src_height == dest_height;
    }

} // namespace

VAStatus vkvvCreateImage(VADriverContextP ctx, VAImageFormat* format, int width, int height, VAImage* image) {
    VkvvDriver* drv = vkvv_driver_from_ctx(ctx);
    if (drv == nullptr || format == nullptr || image == nullptr) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    VAImage va_image{};
    if (!fill_image_layout(&va_image, format, width, height)) {
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }

    auto* image_payload = new (std::nothrow) VkvvImage();
    if (image_payload == nullptr) {
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    auto* buffer_payload = create_image_buffer_payload(va_image.data_size);
    if (buffer_payload == nullptr) {
        delete image_payload;
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    const VABufferID buffer_id = vkvv_object_add(drv, VKVV_OBJECT_BUFFER, buffer_payload);
    if (buffer_id == VA_INVALID_ID) {
        vkvv_release_buffer_payload(buffer_payload);
        delete buffer_payload;
        delete image_payload;
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    va_image.buf             = buffer_id;
    image_payload->image     = va_image;
    image_payload->buffer_id = buffer_id;

    const VAImageID image_id = vkvv_object_add(drv, VKVV_OBJECT_IMAGE, image_payload);
    if (image_id == VA_INVALID_ID) {
        (void)vkvv_object_remove(drv, buffer_id, VKVV_OBJECT_BUFFER);
        delete image_payload;
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    image_payload->image.image_id = image_id;
    *image                        = image_payload->image;
    return VA_STATUS_SUCCESS;
}

VAStatus vkvvDestroyImage(VADriverContextP ctx, VAImageID image_id) {
    VkvvDriver* drv = vkvv_driver_from_ctx(ctx);
    if (drv == nullptr) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    auto* image = static_cast<VkvvImage*>(vkvv_object_get(drv, image_id, VKVV_OBJECT_IMAGE));
    if (image == nullptr) {
        return VA_STATUS_ERROR_INVALID_IMAGE;
    }

    const VABufferID buffer_id = image->buffer_id;
    if (!vkvv_object_remove(drv, image_id, VKVV_OBJECT_IMAGE)) {
        return VA_STATUS_ERROR_INVALID_IMAGE;
    }
    if (buffer_id != VA_INVALID_ID) {
        (void)vkvv_object_remove(drv, buffer_id, VKVV_OBJECT_BUFFER);
    }
    return VA_STATUS_SUCCESS;
}

VAStatus vkvvPutImage(VADriverContextP ctx, VASurfaceID surface_id, VAImageID image_id, int src_x, int src_y, unsigned int src_width, unsigned int src_height, int dest_x,
                      int dest_y, unsigned int dest_width, unsigned int dest_height) {
    VkvvDriver* drv = vkvv_driver_from_ctx(ctx);
    if (drv == nullptr) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    auto* image = static_cast<VkvvImage*>(vkvv_object_get(drv, image_id, VKVV_OBJECT_IMAGE));
    if (image == nullptr) {
        return VA_STATUS_ERROR_INVALID_IMAGE;
    }
    auto* buffer = static_cast<VkvvBuffer*>(vkvv_object_get(drv, image->buffer_id, VKVV_OBJECT_BUFFER));
    if (buffer == nullptr || buffer->data == nullptr || buffer->type != VAImageBufferType) {
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    if (buffer->size < image->image.data_size) {
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }

    auto* surface = vkvv_surface_get_locked(drv, surface_id);
    if (surface == nullptr) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    LockedSurface surface_lock(surface);
    if (surface->destroying) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    if (surface->work_state == VKVV_SURFACE_WORK_RENDERING) {
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    if (!full_frame_copy(&image->image, surface, src_x, src_y, src_width, src_height, dest_x, dest_y, dest_width, dest_height)) {
        return VA_STATUS_ERROR_UNIMPLEMENTED;
    }
    if (image->image.format.fourcc != surface->fourcc) {
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }
    if (drv->vulkan == nullptr) {
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    char     reason[512] = {};
    VAStatus status      = vkvv_vulkan_upload_encode_input_image(drv->vulkan, surface, image, buffer->data, buffer->size, reason, sizeof(reason));
    (void)reason;
    return status;
}
