#ifndef VKVV_TELEMETRY_H
#define VKVV_TELEMETRY_H

#include <stdint.h>

bool vkvv_log_enabled(void);
bool vkvv_trace_enabled(void);
void vkvv_trace(const char* event, const char* fmt, ...);
void vkvv_trace_emit(const char* event, const char* fmt, ...);

// Use VKVV_TRACE in hot paths so trace arguments are not evaluated unless tracing
// is enabled. vkvv_trace() remains for cold/compatibility paths where arguments
// are already cheap or explicitly pre-gated.
#define VKVV_TRACE(event, fmt, ...)                                                                                                                                                \
    do {                                                                                                                                                                           \
        if (vkvv_trace_enabled()) {                                                                                                                                                \
            vkvv_trace_emit((event), (fmt)__VA_OPT__(, ) __VA_ARGS__);                                                                                                             \
        }                                                                                                                                                                          \
    } while (false)

template <typename Handle>
inline unsigned long long vkvv_trace_handle(Handle handle) {
#if defined(VK_USE_64_BIT_PTR_DEFINES) && VK_USE_64_BIT_PTR_DEFINES
    return static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(handle));
#else
    return static_cast<unsigned long long>(handle);
#endif
}

#endif
