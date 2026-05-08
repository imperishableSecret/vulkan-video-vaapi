#include "va_private.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>

VAStatus vkvvCreateBuffer(
        VADriverContextP ctx,
        VAContextID context,
        VABufferType type,
        unsigned int size,
        unsigned int num_elements,
        void *data,
        VABufferID *buf_id) {
    VkvvDriver *drv = vkvv_driver_from_ctx(ctx);
    if (vkvv_object_get(drv, context, VKVV_OBJECT_CONTEXT) == NULL) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }

    if (num_elements != 0 && size > UINT32_MAX / num_elements) {
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    auto *buffer = new (std::nothrow) VkvvBuffer();
    if (buffer == NULL) {
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    buffer->type = type;
    buffer->buffer_class = type == VAEncCodedBufferType ?
                           VKVV_BUFFER_CLASS_ENCODE_CODED_OUTPUT :
                           VKVV_BUFFER_CLASS_PARAMETER;
    buffer->size = size;
    buffer->num_elements = num_elements;
    buffer->coded_payload = NULL;
    const unsigned int total = size * num_elements;
    if (total != 0) {
        buffer->data = std::calloc(1, total);
        if (buffer->data == NULL) {
            delete buffer;
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
        if (data != NULL) {
            std::memcpy(buffer->data, data, total);
        }
    }

    *buf_id = vkvv_object_add(drv, VKVV_OBJECT_BUFFER, buffer);
    if (*buf_id == VA_INVALID_ID) {
        std::free(buffer->data);
        delete buffer;
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    return VA_STATUS_SUCCESS;
}

VAStatus vkvvBufferSetNumElements(VADriverContextP ctx, VABufferID buf_id, unsigned int num_elements) {
    VkvvDriver *drv = vkvv_driver_from_ctx(ctx);
    auto *buffer = static_cast<VkvvBuffer *>(vkvv_object_get(drv, buf_id, VKVV_OBJECT_BUFFER));
    if (buffer == NULL) {
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    buffer->num_elements = num_elements;
    return VA_STATUS_SUCCESS;
}

VAStatus vkvvMapBuffer(VADriverContextP ctx, VABufferID buf_id, void **pbuf) {
    VkvvDriver *drv = vkvv_driver_from_ctx(ctx);
    auto *buffer = static_cast<VkvvBuffer *>(vkvv_object_get(drv, buf_id, VKVV_OBJECT_BUFFER));
    if (buffer == NULL) {
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    buffer->mapped = true;
    *pbuf = buffer->data;
    return VA_STATUS_SUCCESS;
}

VAStatus vkvvUnmapBuffer(VADriverContextP ctx, VABufferID buf_id) {
    VkvvDriver *drv = vkvv_driver_from_ctx(ctx);
    auto *buffer = static_cast<VkvvBuffer *>(vkvv_object_get(drv, buf_id, VKVV_OBJECT_BUFFER));
    if (buffer == NULL) {
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    buffer->mapped = false;
    return VA_STATUS_SUCCESS;
}

VAStatus vkvvDestroyBuffer(VADriverContextP ctx, VABufferID buffer_id) {
    VkvvDriver *drv = vkvv_driver_from_ctx(ctx);
    auto *buffer = static_cast<VkvvBuffer *>(vkvv_object_get(drv, buffer_id, VKVV_OBJECT_BUFFER));
    if (buffer == NULL) {
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    std::free(buffer->data);
    buffer->data = NULL;
    buffer->coded_payload = NULL;
    return vkvv_object_remove(drv, buffer_id, VKVV_OBJECT_BUFFER) ?
           VA_STATUS_SUCCESS : VA_STATUS_ERROR_INVALID_BUFFER;
}

VAStatus vkvvBufferInfo(
        VADriverContextP ctx,
        VABufferID buf_id,
        VABufferType *type,
        unsigned int *size,
        unsigned int *num_elements) {
    VkvvDriver *drv = vkvv_driver_from_ctx(ctx);
    auto *buffer = static_cast<VkvvBuffer *>(vkvv_object_get(drv, buf_id, VKVV_OBJECT_BUFFER));
    if (buffer == NULL) {
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    *type = buffer->type;
    *size = buffer->size;
    *num_elements = buffer->num_elements;
    return VA_STATUS_SUCCESS;
}

VAStatus vkvvMapBuffer2(VADriverContextP ctx, VABufferID buf_id, void **pbuf, uint32_t flags) {
    (void) flags;
    return vkvvMapBuffer(ctx, buf_id, pbuf);
}
