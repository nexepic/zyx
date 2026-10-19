#include <gtest/gtest.h>
#include <array>
#include <thread>
#include <stdexcept>
#include "graph/debug/PerfTrace.hpp"
using graph::debug::PerfTrace;
namespace {
struct Counts {
    int64_t values = 0, durations = 0;
    static void receive(void *owner, std::string_view key, int64_t value, bool duration) noexcept {
        if (key != "local") return;
        auto &self = *static_cast<Counts *>(owner);
        (duration ? self.durations : self.values) += value;
    }
};
}
TEST(PerfTraceScopedObserverTest, NestedObserversAreInclusiveAndDoNotEnableGlobalCollection) {
    PerfTrace::setEnabled(false); PerfTrace::reset();
    Counts parent, child;
    {
        PerfTrace::ScopedObserver outer(&parent, Counts::receive);
        EXPECT_TRUE(PerfTrace::areValuesEnabled()); EXPECT_FALSE(PerfTrace::isEnabled());
        PerfTrace::addValue("local", 2);
        {
            PerfTrace::ScopedObserver inner(&child, Counts::receive);
            PerfTrace::addValue("local", 3); PerfTrace::addDuration("local", 7);
        }
        PerfTrace::addValue("local", 5);
    }
    EXPECT_EQ(parent.values, 10); EXPECT_EQ(child.values, 3);
    EXPECT_EQ(parent.durations, 7); EXPECT_EQ(child.durations, 7);
    EXPECT_FALSE(PerfTrace::areValuesEnabled()); EXPECT_FALSE(PerfTrace::isEnabled());
    EXPECT_TRUE(PerfTrace::snapshotAndReset().empty());
}
TEST(PerfTraceScopedObserverTest, OtherThreadsAndExistingGlobalValuesStaySeparate) {
    PerfTrace::setEnabled(true); PerfTrace::reset(); PerfTrace::addValue("local", 100);
    Counts local;
    {
        PerfTrace::ScopedObserver scope(&local, Counts::receive);
        std::thread other([] { PerfTrace::addValue("local", 1000); }); other.join();
        PerfTrace::addValue("local", 2);
    }
    EXPECT_EQ(local.values, 2);
    const auto global = PerfTrace::snapshotAndReset();
    EXPECT_EQ(global.at("local").totalValue, 1102);
    EXPECT_TRUE(PerfTrace::isEnabled()); PerfTrace::setEnabled(false);
}
TEST(PerfTraceScopedObserverTest, ExceptionRestoresObserverAndPagePinsRemainGloballyOwned) {
    PerfTrace::setEnabled(false); PerfTrace::reset(); Counts counts;
    EXPECT_THROW(([&] {
        PerfTrace::ScopedObserver scope(&counts, Counts::receive);
        PerfTrace::addValue("local", 1);
        PerfTrace::addPagePin(&counts, 4096, 1);
        throw std::runtime_error("stop");
    }()), std::runtime_error);
    EXPECT_EQ(counts.values, 1);
    EXPECT_FALSE(PerfTrace::areValuesEnabled());
    EXPECT_TRUE(PerfTrace::snapshotAndReset().empty());
}
