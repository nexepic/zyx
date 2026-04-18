/**
 * @file test_TemporalTypes.cpp
 * @author ZYX Contributors
 * @date 2026
 *
 * @copyright Copyright (c) 2026
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 **/

#include <gtest/gtest.h>
#include "graph/core/TemporalTypes.hpp"

using namespace graph;

// --- TemporalDate ---

TEST(TemporalDateTest, FromISOAndToISORoundTrip) {
	auto d = TemporalDate::fromISO("2024-01-15");
	EXPECT_EQ(d.toISO(), "2024-01-15");
}

TEST(TemporalDateTest, FromYMDAccessors) {
	auto d = TemporalDate::fromYMD(2024, 1, 15);
	EXPECT_EQ(d.year(), 2024);
	EXPECT_EQ(d.month(), 1);
	EXPECT_EQ(d.day(), 15);
}

TEST(TemporalDateTest, FromYMDMatchesFromISO) {
	auto fromYmd = TemporalDate::fromYMD(2024, 1, 15);
	auto fromIso = TemporalDate::fromISO("2024-01-15");
	EXPECT_EQ(fromYmd, fromIso);
}

TEST(TemporalDateTest, ComparisonOperators) {
	auto earlier = TemporalDate::fromISO("2024-01-01");
	auto later = TemporalDate::fromISO("2024-12-31");
	EXPECT_LT(earlier, later);
	EXPECT_GT(later, earlier);
	EXPECT_LE(earlier, later);
	EXPECT_GE(later, earlier);
	EXPECT_NE(earlier, later);
	EXPECT_EQ(earlier, earlier);
}

TEST(TemporalDateTest, LeapYearFeb29) {
	auto d = TemporalDate::fromISO("2024-02-29");
	EXPECT_EQ(d.year(), 2024);
	EXPECT_EQ(d.month(), 2);
	EXPECT_EQ(d.day(), 29);
	EXPECT_EQ(d.toISO(), "2024-02-29");
}

TEST(TemporalDateTest, EpochDate) {
	auto d = TemporalDate::fromISO("1970-01-01");
	EXPECT_EQ(d.epochDays, 0);
	EXPECT_EQ(d.toISO(), "1970-01-01");
}

TEST(TemporalDateTest, TodayReturnsValidDate) {
	auto d = TemporalDate::today();
	EXPECT_GE(d.year(), 2024);
	EXPECT_GE(d.month(), 1);
	EXPECT_LE(d.month(), 12);
	EXPECT_GE(d.day(), 1);
	EXPECT_LE(d.day(), 31);
}

// --- TemporalDateTime ---

TEST(TemporalDateTimeTest, FromISOAndToISORoundTrip) {
	auto dt = TemporalDateTime::fromISO("2024-01-15T13:45:30");
	EXPECT_EQ(dt.toISO(), "2024-01-15T13:45:30");
}

TEST(TemporalDateTimeTest, FromComponentsAccessors) {
	auto dt = TemporalDateTime::fromComponents(2024, 1, 15, 13, 45, 30);
	EXPECT_EQ(dt.year(), 2024);
	EXPECT_EQ(dt.month(), 1);
	EXPECT_EQ(dt.day(), 15);
	EXPECT_EQ(dt.hour(), 13);
	EXPECT_EQ(dt.minute(), 45);
	EXPECT_EQ(dt.second(), 30);
}

TEST(TemporalDateTimeTest, FromComponentsMatchesFromISO) {
	auto fromComp = TemporalDateTime::fromComponents(2024, 1, 15, 13, 45, 30);
	auto fromIso = TemporalDateTime::fromISO("2024-01-15T13:45:30");
	EXPECT_EQ(fromComp, fromIso);
}

TEST(TemporalDateTimeTest, ComparisonOperators) {
	auto earlier = TemporalDateTime::fromISO("2024-01-01T00:00:00");
	auto later = TemporalDateTime::fromISO("2024-12-31T23:59:59");
	EXPECT_LT(earlier, later);
	EXPECT_GT(later, earlier);
	EXPECT_NE(earlier, later);
	EXPECT_EQ(earlier, earlier);
}

TEST(TemporalDateTimeTest, NowReturnsValidDateTime) {
	auto dt = TemporalDateTime::now();
	EXPECT_GE(dt.year(), 2024);
	EXPECT_GE(dt.month(), 1);
	EXPECT_LE(dt.month(), 12);
	EXPECT_GE(dt.hour(), 0);
	EXPECT_LE(dt.hour(), 23);
}

TEST(TemporalDateTimeTest, MidnightTime) {
	auto dt = TemporalDateTime::fromComponents(2024, 6, 15, 0, 0, 0);
	EXPECT_EQ(dt.hour(), 0);
	EXPECT_EQ(dt.minute(), 0);
	EXPECT_EQ(dt.second(), 0);
	EXPECT_EQ(dt.toISO(), "2024-06-15T00:00:00");
}

// --- TemporalDuration ---

TEST(TemporalDurationTest, FromISOAndToISORoundTrip) {
	auto dur = TemporalDuration::fromISO("P1Y2M3DT4H5M6S");
	EXPECT_EQ(dur.toISO(), "P1Y2M3DT4H5M6S");
}

TEST(TemporalDurationTest, FromComponentsMonthsAndDays) {
	auto dur = TemporalDuration::fromComponents(1, 2, 0, 3, 0, 0, 0);
	EXPECT_EQ(dur.months, 1 * 12 + 2);
	EXPECT_EQ(dur.days, 3);
}

TEST(TemporalDurationTest, FromComponentsWithWeeks) {
	auto dur = TemporalDuration::fromComponents(0, 0, 2, 1, 0, 0, 0);
	EXPECT_EQ(dur.days, 2 * 7 + 1);
}

TEST(TemporalDurationTest, FromComponentsTimeToNanos) {
	auto dur = TemporalDuration::fromComponents(0, 0, 0, 0, 1, 30, 45);
	int64_t expectedNanos = (1LL * 3600 + 30 * 60 + 45) * 1'000'000'000LL;
	EXPECT_EQ(dur.nanos, expectedNanos);
}

TEST(TemporalDurationTest, ZeroDuration) {
	auto dur = TemporalDuration::fromISO("P0D");
	EXPECT_EQ(dur.months, 0);
	EXPECT_EQ(dur.days, 0);
	EXPECT_EQ(dur.nanos, 0);
}

TEST(TemporalDurationTest, ComparisonOperators) {
	auto shorter = TemporalDuration::fromISO("P1D");
	auto longer = TemporalDuration::fromISO("P1Y");
	EXPECT_LT(shorter, longer);
	EXPECT_GT(longer, shorter);
	EXPECT_LE(shorter, longer);
	EXPECT_GE(longer, shorter);
	EXPECT_EQ(shorter, shorter);
}

TEST(TemporalDurationTest, EqualityDefault) {
	auto a = TemporalDuration::fromComponents(1, 2, 0, 3, 4, 5, 6);
	auto b = TemporalDuration::fromComponents(1, 2, 0, 3, 4, 5, 6);
	EXPECT_EQ(a, b);
}
