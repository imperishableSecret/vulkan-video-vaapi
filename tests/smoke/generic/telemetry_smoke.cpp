#include "telemetry.h"

#include <cstdlib>
#include <cstring>
#include <iostream>

namespace {

    int expensive_argument_evaluations = 0;

    int expensive_argument(void) {
        expensive_argument_evaluations++;
        return 42;
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
    const bool expect_trace = env_is_enabled("VKVV_EXPECT_TRACE");
    const bool expect_log   = env_is_enabled("VKVV_EXPECT_LOG");

    bool       ok = true;
    ok            = check(vkvv_trace_enabled() == expect_trace, "trace env cache mismatch") && ok;
    ok            = check(vkvv_log_enabled() == expect_log, "log env cache mismatch") && ok;

    VKVV_TRACE("telemetry-smoke", "value=%d", expensive_argument());

    const int expected_evaluations = expect_trace ? 1 : 0;
    ok                             = check(expensive_argument_evaluations == expected_evaluations, "trace macro evaluated disabled arguments") && ok;
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
