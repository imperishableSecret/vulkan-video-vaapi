#include "telemetry.h"

#include <cstdlib>
#include <cstring>
#include <iostream>

namespace {

    int expensive_argument_evaluations = 0;
    int expensive_string_evaluations   = 0;
    int deep_argument_evaluations      = 0;
    int success_reason_evaluations     = 0;

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

} // namespace

int main() {
    const bool expect_trace  = env_is_enabled("VKVV_EXPECT_TRACE");
    const bool expect_deep   = env_is_enabled("VKVV_EXPECT_TRACE_DEEP");
    const bool expect_log    = env_is_enabled("VKVV_EXPECT_LOG");
    const bool expect_reason = expect_trace || expect_log;
    char       reason[64]    = {};

    bool       ok = true;
    ok            = check(vkvv_trace_enabled() == expect_trace, "trace env cache mismatch") && ok;
    ok            = check(vkvv_trace_deep_enabled() == expect_deep, "deep trace env cache mismatch") && ok;
    ok            = check(vkvv_log_enabled() == expect_log, "log env cache mismatch") && ok;
    ok            = check(vkvv_success_reason_enabled() == expect_reason, "success reason env cache mismatch") && ok;

    VKVV_TRACE("telemetry-smoke", "value=%d", expensive_argument());
    VKVV_TRACE("telemetry-smoke-string", "records=%s", expensive_string_argument());
    VKVV_TRACE_DEEP("telemetry-smoke-deep", "value=%d", deep_argument());
    VKVV_SUCCESS_REASON(reason, sizeof(reason), "success=%d", success_reason_argument());

    const int expected_evaluations = expect_trace ? 1 : 0;
    ok                             = check(expensive_argument_evaluations == expected_evaluations, "trace macro evaluated disabled arguments") && ok;
    ok                             = check(expensive_string_evaluations == expected_evaluations, "trace macro evaluated disabled string arguments") && ok;
    ok                             = check(deep_argument_evaluations == (expect_deep ? 1 : 0), "deep trace macro evaluated disabled arguments") && ok;
    ok                             = check(success_reason_evaluations == (expect_reason ? 1 : 0), "success reason macro evaluated disabled arguments") && ok;
    ok                             = check((reason[0] != '\0') == expect_reason, "success reason text mismatch") && ok;
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
