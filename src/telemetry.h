#ifndef VKVV_TELEMETRY_H
#define VKVV_TELEMETRY_H

#include <stdint.h>

bool vkvv_trace_enabled(void);
void vkvv_trace(const char* event, const char* fmt, ...);

template <typename Handle>
inline unsigned long long vkvv_trace_handle(Handle handle) {
#if defined(VK_USE_64_BIT_PTR_DEFINES) && VK_USE_64_BIT_PTR_DEFINES
    return static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(handle));
#else
    return static_cast<unsigned long long>(handle);
#endif
}

#endif
