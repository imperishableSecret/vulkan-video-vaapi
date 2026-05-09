#include "va/private.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>

namespace {

    void refresh_coded_segment(VkvvCodedBufferPayload* payload, uint32_t status_flags) {
        if (payload == NULL) {
            return;
        }
        payload->segment.size       = payload->bytes_used;
        payload->segment.bit_offset = 0;
        payload->segment.status     = status_flags;
        payload->segment.reserved   = 0;
        payload->segment.buf        = payload->storage.empty() ? NULL : payload->storage.data();
        payload->segment.next       = NULL;
        std::memset(payload->segment.va_reserved, 0, sizeof(payload->segment.va_reserved));
    }

} // namespace

void vkvv_coded_buffer_mark_pending(VkvvBuffer* buffer, uint64_t generation) {
    if (buffer == NULL || buffer->coded_payload == NULL) {
        return;
    }
    VkvvCodedBufferPayload* payload = buffer->coded_payload;
    payload->bytes_used             = 0;
    payload->generation             = generation;
    payload->sync_status            = VA_STATUS_SUCCESS;
    payload->ready                  = false;
    payload->pending                = true;
    refresh_coded_segment(payload, 0);
}

VAStatus vkvv_coded_buffer_store(VkvvBuffer* buffer, const void* data, size_t data_size, uint32_t status_flags, uint64_t generation) {
    if (buffer == NULL || buffer->buffer_class != VKVV_BUFFER_CLASS_ENCODE_CODED_OUTPUT || buffer->coded_payload == NULL) {
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    VkvvCodedBufferPayload* payload = buffer->coded_payload;
    if (data_size > payload->capacity) {
        return VA_STATUS_ERROR_NOT_ENOUGH_BUFFER;
    }
    if (data_size != 0 && data == NULL) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    if (data_size != 0) {
        std::memcpy(payload->storage.data(), data, data_size);
    }
    payload->bytes_used  = static_cast<unsigned int>(data_size);
    payload->generation  = generation;
    payload->sync_status = VA_STATUS_SUCCESS;
    payload->ready       = true;
    payload->pending     = false;
    refresh_coded_segment(payload, status_flags);
    return VA_STATUS_SUCCESS;
}

VAStatus vkvvCreateBuffer(VADriverContextP ctx, VAContextID context, VABufferType type, unsigned int size, unsigned int num_elements, void* data, VABufferID* buf_id) {
    VkvvDriver* drv = vkvv_driver_from_ctx(ctx);
    if (vkvv_object_get(drv, context, VKVV_OBJECT_CONTEXT) == NULL) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }

    if (num_elements != 0 && size > UINT32_MAX / num_elements) {
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    auto* buffer = new (std::nothrow) VkvvBuffer();
    if (buffer == NULL) {
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    buffer->type             = type;
    buffer->buffer_class     = type == VAEncCodedBufferType ? VKVV_BUFFER_CLASS_ENCODE_CODED_OUTPUT : VKVV_BUFFER_CLASS_PARAMETER;
    buffer->size             = size;
    buffer->num_elements     = num_elements;
    buffer->coded_payload    = NULL;
    const unsigned int total = size * num_elements;
    if (buffer->buffer_class == VKVV_BUFFER_CLASS_ENCODE_CODED_OUTPUT) {
        if (total == 0 || data != NULL) {
            delete buffer;
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        }
        buffer->coded_payload = new (std::nothrow) VkvvCodedBufferPayload();
        if (buffer->coded_payload == NULL) {
            delete buffer;
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
        try {
            buffer->coded_payload->storage.resize(total);
        } catch (const std::bad_alloc&) {
            vkvv_release_buffer_payload(buffer);
            delete buffer;
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
        buffer->coded_payload->capacity = total;
        refresh_coded_segment(buffer->coded_payload, 0);
    } else if (total != 0) {
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
        vkvv_release_buffer_payload(buffer);
        delete buffer;
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    return VA_STATUS_SUCCESS;
}

VAStatus vkvvBufferSetNumElements(VADriverContextP ctx, VABufferID buf_id, unsigned int num_elements) {
    VkvvDriver* drv    = vkvv_driver_from_ctx(ctx);
    auto*       buffer = static_cast<VkvvBuffer*>(vkvv_object_get(drv, buf_id, VKVV_OBJECT_BUFFER));
    if (buffer == NULL) {
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    buffer->num_elements = num_elements;
    return VA_STATUS_SUCCESS;
}

VAStatus vkvvMapBuffer(VADriverContextP ctx, VABufferID buf_id, void** pbuf) {
    VkvvDriver* drv    = vkvv_driver_from_ctx(ctx);
    auto*       buffer = static_cast<VkvvBuffer*>(vkvv_object_get(drv, buf_id, VKVV_OBJECT_BUFFER));
    if (buffer == NULL || pbuf == NULL) {
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    buffer->mapped = true;
    *pbuf          = buffer->coded_payload != NULL ? static_cast<void*>(&buffer->coded_payload->segment) : buffer->data;
    return VA_STATUS_SUCCESS;
}

VAStatus vkvvUnmapBuffer(VADriverContextP ctx, VABufferID buf_id) {
    VkvvDriver* drv    = vkvv_driver_from_ctx(ctx);
    auto*       buffer = static_cast<VkvvBuffer*>(vkvv_object_get(drv, buf_id, VKVV_OBJECT_BUFFER));
    if (buffer == NULL) {
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    buffer->mapped = false;
    return VA_STATUS_SUCCESS;
}

VAStatus vkvvDestroyBuffer(VADriverContextP ctx, VABufferID buffer_id) {
    VkvvDriver* drv    = vkvv_driver_from_ctx(ctx);
    auto*       buffer = static_cast<VkvvBuffer*>(vkvv_object_get(drv, buffer_id, VKVV_OBJECT_BUFFER));
    if (buffer == NULL) {
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    vkvv_release_buffer_payload(buffer);
    return vkvv_object_remove(drv, buffer_id, VKVV_OBJECT_BUFFER) ? VA_STATUS_SUCCESS : VA_STATUS_ERROR_INVALID_BUFFER;
}

VAStatus vkvvBufferInfo(VADriverContextP ctx, VABufferID buf_id, VABufferType* type, unsigned int* size, unsigned int* num_elements) {
    VkvvDriver* drv    = vkvv_driver_from_ctx(ctx);
    auto*       buffer = static_cast<VkvvBuffer*>(vkvv_object_get(drv, buf_id, VKVV_OBJECT_BUFFER));
    if (buffer == NULL) {
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    *type         = buffer->type;
    *size         = buffer->size;
    *num_elements = buffer->num_elements;
    return VA_STATUS_SUCCESS;
}

VAStatus vkvvMapBuffer2(VADriverContextP ctx, VABufferID buf_id, void** pbuf, uint32_t flags) {
    (void)flags;
    return vkvvMapBuffer(ctx, buf_id, pbuf);
}

VAStatus vkvvSyncBuffer(VADriverContextP ctx, VABufferID buf_id, uint64_t timeout_ns) {
    (void)timeout_ns;
    VkvvDriver* drv    = vkvv_driver_from_ctx(ctx);
    auto*       buffer = static_cast<VkvvBuffer*>(vkvv_object_get(drv, buf_id, VKVV_OBJECT_BUFFER));
    if (buffer == NULL) {
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    if (buffer->coded_payload == NULL) {
        return VA_STATUS_SUCCESS;
    }
    if (buffer->coded_payload->pending || !buffer->coded_payload->ready) {
        return VA_STATUS_ERROR_TIMEDOUT;
    }
    return buffer->coded_payload->sync_status;
}
