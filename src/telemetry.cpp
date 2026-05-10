#include "telemetry.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace {

    std::atomic<unsigned long long> trace_sequence{1};
    std::mutex                      trace_mutex;

    bool                            env_enabled(const char* name) {
        const char* value = std::getenv(name);
        return value != nullptr && std::strcmp(value, "0") != 0;
    }

    void trace_vemit(const char* event, const char* fmt, va_list args) {
        const unsigned long long    seq = trace_sequence.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(trace_mutex);
        std::fprintf(stderr, "nvidia-vulkan-vaapi: trace seq=%llu event=%s", seq, event != nullptr ? event : "unknown");
        if (fmt != nullptr && fmt[0] != '\0') {
            std::fprintf(stderr, " ");
            std::vfprintf(stderr, fmt, args);
        }
        std::fprintf(stderr, "\n");
    }

} // namespace

bool vkvv_log_enabled(void) {
    static const bool enabled = env_enabled("VKVV_LOG");
    return enabled;
}

bool vkvv_success_reason_enabled(void) {
    return vkvv_log_enabled() || vkvv_trace_enabled();
}

bool vkvv_trace_enabled(void) {
    static const bool enabled = env_enabled("VKVV_TRACE");
    return enabled;
}

void vkvv_trace(const char* event, const char* fmt, ...) {
    if (!vkvv_trace_enabled()) {
        return;
    }

    va_list args;
    va_start(args, fmt);
    trace_vemit(event, fmt, args);
    va_end(args);
}

void vkvv_trace_emit(const char* event, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    trace_vemit(event, fmt, args);
    va_end(args);
}
