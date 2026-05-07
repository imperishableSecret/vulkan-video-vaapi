#include "va_private.h"

#include <cstdio>
#include <cstring>

namespace {

bool check(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
    }
    return condition;
}

bool ops_complete(const VkvvCodecOps *ops) {
    return ops != nullptr &&
           ops->name != nullptr &&
           ops->state_create != nullptr &&
           ops->state_destroy != nullptr &&
           ops->session_create != nullptr &&
           ops->session_destroy != nullptr &&
           ops->begin_picture != nullptr &&
           ops->render_buffer != nullptr &&
           ops->prepare_decode != nullptr &&
           ops->ensure_session != nullptr &&
           ops->decode != nullptr;
}

} // namespace

int main(void) {
    bool ok = true;

    const VkvvCodecOps *h264 = vkvv_codec_ops_for_profile(VAProfileH264High);
    ok = check(ops_complete(h264), "H.264 codec ops are incomplete") && ok;
    if (h264 != nullptr) {
        ok = check(std::strcmp(h264->name, "h264") == 0, "H.264 codec ops used the wrong name") && ok;
    }

    void *state = h264 != nullptr ? h264->state_create() : nullptr;
    ok = check(state != nullptr, "H.264 codec state allocation failed") && ok;
    if (state != nullptr) {
        h264->begin_picture(state);
        h264->state_destroy(state);
    }

    void *session = h264 != nullptr ? h264->session_create() : nullptr;
    ok = check(session != nullptr, "H.264 codec session allocation failed") && ok;
    if (session != nullptr) {
        h264->session_destroy(nullptr, session);
    }

    ok = check(vkvv_codec_ops_for_profile(VAProfileHEVCMain) == nullptr,
               "HEVC should not have codec ops before its decoder is wired") && ok;
    ok = check(vkvv_codec_ops_for_profile(VAProfileVP9Profile0) == nullptr,
               "VP9 should not have codec ops before its decoder is wired") && ok;
    ok = check(vkvv_codec_ops_for_profile(VAProfileAV1Profile0) == nullptr,
               "AV1 should not have codec ops before its decoder is wired") && ok;

    if (!ok) {
        return 1;
    }

    std::printf("codec ops smoke passed\n");
    return 0;
}
