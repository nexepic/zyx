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

TEST(AdmissionCallsiteTest, RejectsWithoutMutationAndReportsTheExternalReservationCall) {
    using namespace graph::query::execution;
    TraceEnvironment environment;
    MaterializedByteBudget budget(8U);
    MaterializedByteReservation reservation(&budget);
    reservation.set(3U);
    testing::internal::CaptureStderr();
    const auto line = __LINE__ + 1U;
    EXPECT_THROW(reservation.set(9U), MaterializedByteLimitExceeded);
    const auto output = testing::internal::GetCapturedStderr();
    EXPECT_NE(output.find("site_file=test_AdmissionCallsite.cpp site_line=" + std::to_string(line)), std::string::npos);
    EXPECT_EQ(reservation.bytes(), 3U); EXPECT_EQ(budget.used(), 3U);
    testing::internal::CaptureStderr();
    const auto checkLine = __LINE__ + 1U;
    EXPECT_TRUE(reservation.wouldExceed(9U));
    const auto checked = testing::internal::GetCapturedStderr();
    EXPECT_NE(checked.find("site_file=test_AdmissionCallsite.cpp site_line=" + std::to_string(checkLine)), std::string::npos);
    EXPECT_EQ(budget.used(), 3U);
    reservation.reset(); EXPECT_EQ(budget.used(), 0U);
}
#endif
