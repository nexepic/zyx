#include <gtest/gtest.h>

#define main zyx_compare_benchmark_main
#include "../../../apps/compare_benchmark/main.cpp"
#undef main

class CompareBenchmarkTest : public ::testing::Test {
protected:
    void TearDown() override {
        graph::debug::PerfTrace::reset();
        graph::debug::PerfTrace::setEnabled(false);
    }
};

TEST_F(CompareBenchmarkTest, MeasureOperationDoesNotEmitEventsWhenValidationFails) {
    Options options;
    options.scale = "smoke";
    options.profile = std::string(kProfileScan);
    options.emitProfile = true;

    testing::internal::CaptureStdout();
    EXPECT_THROW(
            (void) measureOperation(
                    options, "bad_workload", 0,
                    []() {
                        graph::debug::PerfTrace::addDuration("parse", 1000);
                        return int64_t{-1};
                    },
                    [](const int64_t value) {
                        requireNonNegative("bad_workload", value);
                    }),
            std::runtime_error);
    const std::string output = testing::internal::GetCapturedStdout();

    EXPECT_TRUE(output.empty());
    EXPECT_FALSE(graph::debug::PerfTrace::isEnabled());
    EXPECT_TRUE(graph::debug::PerfTrace::snapshotAndReset().empty());
}
