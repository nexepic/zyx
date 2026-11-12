#include <gtest/gtest.h>
#include <cstdlib>
#include <string>
#include "graph/query/execution/MaterializedByteBudget.hpp"

#ifdef ZYX_ENABLE_STORAGE_TEST_HOOKS
namespace {
class TraceEnvironment {
    bool present_ = std::getenv("ZYX_CI_BUDGET_TRACE") != nullptr;
    std::string previous_ = present_ ? std::getenv("ZYX_CI_BUDGET_TRACE") : "";
    static void set(const char *value) {
#ifdef _WIN32
        _putenv_s("ZYX_CI_BUDGET_TRACE", value ? value : "");
#else
        if (value) setenv("ZYX_CI_BUDGET_TRACE", value, 1);
        else unsetenv("ZYX_CI_BUDGET_TRACE");
#endif
    }
public:
    TraceEnvironment() { set("1"); }
    ~TraceEnvironment() { set(present_ ? previous_.c_str() : nullptr); }
};
}

namespace {
int probeCallsite() {
    using namespace graph::query::execution;
    TraceEnvironment environment;
    MaterializedByteBudget budget(8U);
    MaterializedByteReservation reservation(&budget);
    reservation.set(3U);
    testing::internal::CaptureStderr();
    bool rejected = false;
    const auto line = __LINE__ + 1U;
    try { reservation.set(9U); } catch (const MaterializedByteLimitExceeded &) { rejected = true; } catch (...) {}
    const auto output = testing::internal::GetCapturedStderr();
    if (!rejected || output.find("site_file=test_AdmissionCallsite.cpp site_line=" + std::to_string(line)) == std::string::npos ||
        reservation.bytes() != 3U || budget.used() != 3U) return 1;
    testing::internal::CaptureStderr();
    const auto checkLine = __LINE__ + 1U;
    const bool denied = reservation.wouldExceed(9U);
    const auto checked = testing::internal::GetCapturedStderr();
    if (!denied || checked.find("site_file=test_AdmissionCallsite.cpp site_line=" + std::to_string(checkLine)) == std::string::npos ||
        budget.used() != 3U) return 2;
    reservation.reset();
    return budget.used() == 0U ? 0 : 3;
}
}

TEST(AdmissionCallsiteTest, RejectsWithoutMutationAndReportsTheExternalReservationCall) {
    // A re-executed child preserves the process-wide production output cap,
    // even when the parent test runner has already exhausted its trace budget.
    const auto previous = GTEST_FLAG_GET(death_test_style);
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(std::exit(probeCallsite()), testing::ExitedWithCode(0), "");
    GTEST_FLAG_SET(death_test_style, previous);
}
#endif
