#include "telemetry.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>

namespace {

    int expensive_argument_evaluations = 0;
    int expensive_string_evaluations   = 0;
    int deep_argument_evaluations      = 0;
    int success_reason_evaluations     = 0;
    int error_reason_evaluations       = 0;

    int expensive_argument(void) {
        expensive_argument_evaluations++;
        return 42;
    }

    const char* expensive_string_argument(void) {
        expensive_string_evaluations++;
        return "records";
    }

    int success_reason_argument(void) {
        success_reason_evaluations++;
        return 7;
    }

    int error_reason_argument(void) {
        error_reason_evaluations++;
        return 11;
    }

    int deep_argument(void) {
        deep_argument_evaluations++;
        return 99;
    }

    bool env_is_enabled(const char* name) {
        const char* value = std::getenv(name);
        return value != nullptr && std::strcmp(value, "0") != 0;
    }

    bool check(bool condition, const char* message) {
        if (!condition) {
            std::cerr << message << "\n";
            return false;
        }
        return true;
    }

    bool contains(const std::string& text, const char* needle) {
        return text.find(needle) != std::string::npos;
    }

} // namespace

int main() {
    const bool  expect_trace    = env_is_enabled("VKVV_EXPECT_TRACE");
    const bool  expect_deep     = env_is_enabled("VKVV_EXPECT_TRACE_DEEP");
    const bool  expect_log      = env_is_enabled("VKVV_EXPECT_LOG");
    const bool  expect_log_file = env_is_enabled("VKVV_EXPECT_LOG_FILE");
    const bool  expect_reason   = expect_trace || expect_log;
    const char* log_file        = std::getenv("VKVV_LOG_FILE");
    char        reason[64]      = {};

    if (expect_log_file && log_file != nullptr) {
        std::remove(log_file);
    }

    bool ok = true;
    ok      = check(vkvv_trace_enabled() == expect_trace, "trace env cache mismatch") && ok;
    ok      = check(vkvv_trace_deep_enabled() == expect_deep, "deep trace env cache mismatch") && ok;
    ok      = check(vkvv_log_enabled() == expect_log, "log env cache mismatch") && ok;
    ok      = check(vkvv_success_reason_enabled() == expect_reason, "success reason env cache mismatch") && ok;

    VKVV_TRACE("telemetry-smoke", "value=%d", expensive_argument());
    VKVV_TRACE("telemetry-smoke-string", "records=%s", expensive_string_argument());
    VKVV_TRACE_DEEP("telemetry-smoke-deep", "value=%d", deep_argument());
    vkvv_log("telemetry-log-smoke value=13");
    VKVV_SUCCESS_REASON(reason, sizeof(reason), "success=%d", success_reason_argument());
    VKVV_ERROR_REASON(nullptr, 0, VA_STATUS_ERROR_INVALID_BUFFER, "error=%d", error_reason_argument());

    char error_reason[64] = {};
    VKVV_ERROR_REASON(error_reason, sizeof(error_reason), VA_STATUS_ERROR_INVALID_BUFFER, "error=%d", error_reason_argument());

    const int expected_evaluations = expect_trace ? 1 : 0;
    ok                             = check(expensive_argument_evaluations == expected_evaluations, "trace macro evaluated disabled arguments") && ok;
    ok                             = check(expensive_string_evaluations == expected_evaluations, "trace macro evaluated disabled string arguments") && ok;
    ok                             = check(deep_argument_evaluations == (expect_deep ? 1 : 0), "deep trace macro evaluated disabled arguments") && ok;
    ok                             = check(success_reason_evaluations == (expect_reason ? 1 : 0), "success reason macro evaluated disabled arguments") && ok;
    ok                             = check((reason[0] != '\0') == expect_reason, "success reason text mismatch") && ok;
    ok                             = check(error_reason_evaluations == 1, "error reason macro evaluated disabled arguments") && ok;
    ok                             = check(std::strcmp(vkvv_va_status_name(VA_STATUS_ERROR_UNSUPPORTED_PROFILE), "unsupported-profile") == 0, "status name mismatch") && ok;
    ok                             = check(std::strcmp(error_reason, "invalid-buffer: error=11") == 0, "error reason text mismatch") && ok;
    if (expect_log_file) {
        ok = check(log_file != nullptr && log_file[0] != '\0', "expected VKVV_LOG_FILE path") && ok;
        std::ifstream input(log_file != nullptr ? log_file : "");
        std::string   contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        ok = check(contains(contents, "nvidia-vulkan-vaapi: trace seq=1 event=telemetry-smoke"), "trace record missing from VKVV_LOG_FILE") && ok;
        ok = check(contains(contents, "telemetry-log-smoke value=13"), "log record missing from VKVV_LOG_FILE") && ok;
    }
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
