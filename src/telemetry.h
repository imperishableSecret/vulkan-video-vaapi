#ifndef VKVV_TELEMETRY_H
#define VKVV_TELEMETRY_H

#include <cstddef>
#include <cstdio>
#include <stdint.h>
#include <va/va.h>

bool        vkvv_log_enabled(void);
bool        vkvv_success_reason_enabled(void);
bool        vkvv_trace_enabled(void);
bool        vkvv_trace_deep_enabled(void);
void        vkvv_trace(const char* event, const char* fmt, ...);
void        vkvv_trace_emit(const char* event, const char* fmt, ...);

inline void vkvv_clear_reason(char* reason, std::size_t reason_size) {
    if (reason != nullptr && reason_size > 0) {
        reason[0] = '\0';
    }
}

inline const char* vkvv_va_status_name(VAStatus status) {
    switch (status) {
        case VA_STATUS_SUCCESS: return "success";
        case VA_STATUS_ERROR_OPERATION_FAILED: return "operation-failed";
        case VA_STATUS_ERROR_ALLOCATION_FAILED: return "allocation-failed";
        case VA_STATUS_ERROR_INVALID_CONFIG: return "invalid-config";
        case VA_STATUS_ERROR_INVALID_CONTEXT: return "invalid-context";
        case VA_STATUS_ERROR_INVALID_SURFACE: return "invalid-surface";
        case VA_STATUS_ERROR_INVALID_BUFFER: return "invalid-buffer";
        case VA_STATUS_ERROR_INVALID_PARAMETER: return "invalid-parameter";
        case VA_STATUS_ERROR_UNSUPPORTED_PROFILE: return "unsupported-profile";
        case VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT: return "unsupported-entrypoint";
        case VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT: return "unsupported-rt-format";
        case VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE: return "unsupported-memory-type";
        case VA_STATUS_ERROR_TIMEDOUT: return "timed-out";
        case VA_STATUS_ERROR_UNIMPLEMENTED: return "unimplemented";
        default: return "unknown";
    }
}

// Use VKVV_TRACE/VKVV_TRACE_DEEP in hot paths so trace arguments are not
// evaluated unless tracing is enabled. vkvv_trace() remains for cold paths where
// arguments are already cheap or explicitly pre-gated. Do not build strings,
// walk containers, or call syscalls for trace arguments outside one of these
// guards.
#define VKVV_TRACE(event, fmt, ...)                                                                                                                                                \
    do {                                                                                                                                                                           \
        if (vkvv_trace_enabled()) {                                                                                                                                                \
            vkvv_trace_emit((event), (fmt)__VA_OPT__(, ) __VA_ARGS__);                                                                                                             \
        }                                                                                                                                                                          \
    } while (false)

#define VKVV_TRACE_DEEP(event, fmt, ...)                                                                                                                                           \
    do {                                                                                                                                                                           \
        if (vkvv_trace_deep_enabled()) {                                                                                                                                           \
            vkvv_trace_emit((event), (fmt)__VA_OPT__(, ) __VA_ARGS__);                                                                                                             \
        }                                                                                                                                                                          \
    } while (false)

#define VKVV_SUCCESS_REASON(reason, reason_size, fmt, ...)                                                                                                                         \
    do {                                                                                                                                                                           \
        if (vkvv_success_reason_enabled()) {                                                                                                                                       \
            std::snprintf((reason), (reason_size), (fmt)__VA_OPT__(, ) __VA_ARGS__);                                                                                               \
        } else {                                                                                                                                                                   \
            vkvv_clear_reason((reason), (reason_size));                                                                                                                            \
        }                                                                                                                                                                          \
    } while (false)

#define VKVV_ERROR_REASON(reason, reason_size, status, fmt, ...)                                                                                                                   \
    do {                                                                                                                                                                           \
        const std::size_t vkvv_reason_size__ = (reason_size);                                                                                                                      \
        if ((reason) != nullptr && vkvv_reason_size__ > 0) {                                                                                                                       \
            std::snprintf((reason), vkvv_reason_size__, "%s: " fmt, vkvv_va_status_name((status)) __VA_OPT__(, ) __VA_ARGS__);                                                     \
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
