#include "telemetry.h"

#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

    std::atomic<unsigned long long> trace_sequence{1};
    std::mutex                      trace_mutex;

    bool                            env_enabled(const char* name) {
        const char* value = std::getenv(name);
        return value != nullptr && std::strcmp(value, "0") != 0;
    }

    bool trace_deep_env_enabled(void) {
        const char* value = std::getenv("VKVV_TRACE");
        return value != nullptr && (std::strcmp(value, "2") == 0 || std::strcmp(value, "deep") == 0 || std::strcmp(value, "verbose") == 0);
    }

    std::string format_body(const char* fmt, va_list args) {
        if (fmt == nullptr || fmt[0] == '\0') {
            return {};
        }

        va_list measure_args;
        va_copy(measure_args, args);
        const int needed = std::vsnprintf(nullptr, 0, fmt, measure_args);
        va_end(measure_args);
        if (needed <= 0) {
            return {};
        }

        std::vector<char> buffer(static_cast<size_t>(needed) + 1);
        va_list           format_args;
        va_copy(format_args, args);
        std::vsnprintf(buffer.data(), buffer.size(), fmt, format_args);
        va_end(format_args);
        return std::string(buffer.data(), static_cast<size_t>(needed));
    }

    int telemetry_output_fd_locked(void) {
        static int  fd       = -2;
        static bool warned   = false;
        const char* log_path = std::getenv("VKVV_LOG_FILE");
        if (fd != -2) {
            return fd;
        }

        if (log_path == nullptr || log_path[0] == '\0' || std::strcmp(log_path, "0") == 0) {
            fd = STDERR_FILENO;
            return fd;
        }

        fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        if (fd < 0) {
            if (!warned) {
                warned = true;
                std::fprintf(stderr, "nvidia-vulkan-vaapi: failed to open VKVV_LOG_FILE=%s: %s\n", log_path, std::strerror(errno));
            }
            fd = STDERR_FILENO;
        }
        return fd;
    }

    void write_all_locked(int fd, const std::string& line) {
        const char* data = line.data();
        size_t      left = line.size();
        while (left > 0) {
            ssize_t written = write(fd, data, left);
            if (written < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            if (written == 0) {
                break;
            }
            data += written;
            left -= static_cast<size_t>(written);
        }
    }

    void emit_line(std::string line) {
        line.push_back('\n');
        std::lock_guard<std::mutex> lock(trace_mutex);
        write_all_locked(telemetry_output_fd_locked(), line);
    }

    void trace_vemit(const char* event, const char* fmt, va_list args) {
        const std::string           body = format_body(fmt, args);

        std::lock_guard<std::mutex> lock(trace_mutex);
        const unsigned long long    seq = trace_sequence.fetch_add(1, std::memory_order_relaxed);
        std::string                 line;
        line.reserve(96 + body.size());
        line += "nvidia-vulkan-vaapi: trace seq=";
        line += std::to_string(seq);
        line += " event=";
        line += event != nullptr ? event : "unknown";
        line += " pid=";
        line += std::to_string(static_cast<long long>(getpid()));
        if (!body.empty()) {
            line.push_back(' ');
            line += body;
        }
        line.push_back('\n');
        write_all_locked(telemetry_output_fd_locked(), line);
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
    static const bool enabled = env_enabled("VKVV_TRACE") || env_enabled("VKVV_TRACE_EXPORT_VALIDITY") || env_enabled("VKVV_TRACE_FD_LIFETIME") ||
        env_enabled("VKVV_TRACE_PIXEL_PROOF");
    return enabled;
}

bool vkvv_trace_deep_enabled(void) {
    static const bool enabled = trace_deep_env_enabled();
    return enabled;
}

void vkvv_log(const char* fmt, ...) {
    if (!vkvv_log_enabled()) {
        return;
    }

    va_list args;
    va_start(args, fmt);
    const std::string body = format_body(fmt, args);
    va_end(args);

    std::string line;
    line.reserve(24 + body.size());
    line += "nvidia-vulkan-vaapi: ";
    line += body;
    emit_line(std::move(line));
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
