#include "va/private.h"

#include <cstdint>
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

} // namespace

int main(void) {
    bool            ok = true;

    VkvvDriver      drv{};
    VADriverContext ctx{};
    drv.next_id     = 1;
    ctx.pDriverData = &drv;

    auto* vctx = new (std::nothrow) VkvvContext();
    if (vctx == nullptr) {
        return 1;
    }
    const VAContextID context = vkvv_object_add(&drv, VKVV_OBJECT_CONTEXT, vctx);
    ok                        = check(context != VA_INVALID_ID, "failed to add test context") && ok;

    VABufferID invalid = VA_INVALID_ID;
    ok = check_va(vkvvCreateBuffer(&ctx, context, VAEncCodedBufferType, 0, 1, nullptr, &invalid), VA_STATUS_ERROR_INVALID_PARAMETER, "zero-capacity coded buffer") && ok;

    uint8_t    initial_byte = 0x33;
    VABufferID with_initial = VA_INVALID_ID;
    ok =
        check_va(vkvvCreateBuffer(&ctx, context, VAEncCodedBufferType, 64, 1, &initial_byte, &with_initial), VA_STATUS_ERROR_INVALID_PARAMETER, "pre-populated coded buffer") && ok;

    VABufferID coded_id = VA_INVALID_ID;
    ok                  = check_va(vkvvCreateBuffer(&ctx, context, VAEncCodedBufferType, 64, 1, nullptr, &coded_id), VA_STATUS_SUCCESS, "create coded buffer") && ok;

    auto* coded = static_cast<VkvvBuffer*>(vkvv_object_get(&drv, coded_id, VKVV_OBJECT_BUFFER));
    ok          = check(coded != nullptr && coded->buffer_class == VKVV_BUFFER_CLASS_ENCODE_CODED_OUTPUT, "coded buffer was not classified as encode output") && ok;
    ok          = check(coded != nullptr && coded->data == nullptr && coded->coded_payload != nullptr, "coded buffer did not use coded payload storage") && ok;
    ok          = check(coded != nullptr && coded->coded_payload != nullptr && coded->coded_payload->capacity == 64, "coded buffer capacity was not recorded") && ok;

    void* mapped  = nullptr;
    ok            = check_va(vkvvMapBuffer(&ctx, coded_id, &mapped), VA_STATUS_SUCCESS, "map empty coded buffer") && ok;
    auto* segment = static_cast<VACodedBufferSegment*>(mapped);
    ok            = check(segment != nullptr && segment == &coded->coded_payload->segment, "coded buffer did not map to VACodedBufferSegment") && ok;
    ok            = check(segment != nullptr && segment->size == 0 && segment->bit_offset == 0 && segment->status == 0 && segment->next == nullptr,
                          "empty coded segment metadata was not initialized") &&
        ok;
    ok = check(segment != nullptr && segment->buf != nullptr, "empty coded segment did not expose reserved storage") && ok;
    ok = check_va(vkvvUnmapBuffer(&ctx, coded_id), VA_STATUS_SUCCESS, "unmap empty coded buffer") && ok;
    ok = check_va(vkvvSyncBuffer(&ctx, coded_id, 0), VA_STATUS_SUCCESS, "sync ready empty coded buffer") && ok;

    const uint8_t payload[] = {0x00, 0x00, 0x01, 0x65, 0x88};
    ok      = check_va(vkvv_coded_buffer_store(coded, payload, sizeof(payload), VA_CODED_BUF_STATUS_SINGLE_NALU, 7), VA_STATUS_SUCCESS, "store coded payload") && ok;
    mapped  = nullptr;
    ok      = check_va(vkvvMapBuffer(&ctx, coded_id, &mapped), VA_STATUS_SUCCESS, "map filled coded buffer") && ok;
    segment = static_cast<VACodedBufferSegment*>(mapped);
    ok      = check(segment != nullptr && segment->size == sizeof(payload) && segment->status == VA_CODED_BUF_STATUS_SINGLE_NALU && segment->next == nullptr,
                    "filled coded segment metadata was not populated") &&
        ok;
    ok = check(segment != nullptr && segment->buf != nullptr && std::memcmp(segment->buf, payload, sizeof(payload)) == 0, "filled coded segment bytes were not copied") && ok;
    ok = check_va(vkvvUnmapBuffer(&ctx, coded_id), VA_STATUS_SUCCESS, "unmap filled coded buffer") && ok;
    ok = check_va(vkvvSyncBuffer(&ctx, coded_id, 0), VA_STATUS_SUCCESS, "sync ready filled coded buffer") && ok;

    vkvv_coded_buffer_mark_pending(coded, 8);
    ok = check(coded->coded_payload->pending && !coded->coded_payload->ready && coded->coded_payload->generation == 8, "pending coded buffer state was not recorded") && ok;
    ok = check_va(vkvvSyncBuffer(&ctx, coded_id, 0), VA_STATUS_ERROR_TIMEDOUT, "sync pending coded buffer") && ok;

    vkvv_coded_buffer_fail(coded, VA_STATUS_ERROR_OPERATION_FAILED, 9);
    ok      = check(!coded->coded_payload->pending && coded->coded_payload->ready && coded->coded_payload->generation == 9, "failed coded buffer state was not completed") && ok;
    ok      = check_va(vkvvSyncBuffer(&ctx, coded_id, 0), VA_STATUS_ERROR_OPERATION_FAILED, "sync failed coded buffer") && ok;
    mapped  = nullptr;
    ok      = check_va(vkvvMapBuffer(&ctx, coded_id, &mapped), VA_STATUS_SUCCESS, "map failed coded buffer") && ok;
    segment = static_cast<VACodedBufferSegment*>(mapped);
    ok      = check(segment != nullptr && segment->size == 0 && segment->buf != nullptr, "failed coded segment metadata was not cleared") && ok;
    ok      = check_va(vkvvUnmapBuffer(&ctx, coded_id), VA_STATUS_SUCCESS, "unmap failed coded buffer") && ok;

    uint8_t too_large[65]{};
    ok = check_va(vkvv_coded_buffer_store(coded, too_large, sizeof(too_large), 0, 9), VA_STATUS_ERROR_NOT_ENOUGH_BUFFER, "oversized coded payload") && ok;

    ok = check_va(vkvvDestroyBuffer(&ctx, coded_id), VA_STATUS_SUCCESS, "destroy coded buffer") && ok;
    ok = check(vkvv_object_get(&drv, coded_id, VKVV_OBJECT_BUFFER) == nullptr, "destroyed coded buffer stayed in object table") && ok;

    vkvv_object_clear(&drv);

    if (!ok) {
        return 1;
    }
    std::printf("coded buffer smoke passed\n");
    return 0;
}
