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

// --- TemporalDate: error branches ---

TEST(TemporalDateTest, FromISOInvalidFormat) {
	EXPECT_THROW(TemporalDate::fromISO("not-a-date"), std::runtime_error);
	EXPECT_THROW(TemporalDate::fromISO("2024"), std::runtime_error);
	EXPECT_THROW(TemporalDate::fromISO(""), std::runtime_error);
}

TEST(TemporalDateTest, FromISOInvalidMonth) {
	EXPECT_THROW(TemporalDate::fromISO("2024-00-15"), std::runtime_error);
	EXPECT_THROW(TemporalDate::fromISO("2024-13-15"), std::runtime_error);
}

TEST(TemporalDateTest, FromISOInvalidDay) {
	EXPECT_THROW(TemporalDate::fromISO("2024-01-00"), std::runtime_error);
	EXPECT_THROW(TemporalDate::fromISO("2024-01-32"), std::runtime_error);
}

TEST(TemporalDateTest, FromISONonLeapYearFeb29) {
	EXPECT_THROW(TemporalDate::fromISO("2023-02-29"), std::runtime_error);
}

TEST(TemporalDateTest, LeapYearCenturyRules) {
	// 1900 is NOT a leap year (divisible by 100 but not 400)
	EXPECT_THROW(TemporalDate::fromISO("1900-02-29"), std::runtime_error);
	// 2000 IS a leap year (divisible by 400)
	auto d = TemporalDate::fromISO("2000-02-29");
	EXPECT_EQ(d.year(), 2000);
	EXPECT_EQ(d.month(), 2);
	EXPECT_EQ(d.day(), 29);
}

// --- TemporalDateTime: negative epochMillis (pre-epoch dates) ---

TEST(TemporalDateTimeTest, PreEpochDateTime) {
	// 1969-12-31T23:59:59 is 1 second before epoch
	auto dt = TemporalDateTime::fromComponents(1969, 12, 31, 23, 59, 59);
	EXPECT_EQ(dt.year(), 1969);
	EXPECT_EQ(dt.month(), 12);
	EXPECT_EQ(dt.day(), 31);
	EXPECT_EQ(dt.hour(), 23);
	EXPECT_EQ(dt.minute(), 59);
	EXPECT_EQ(dt.second(), 59);
	EXPECT_EQ(dt.toISO(), "1969-12-31T23:59:59");
}

TEST(TemporalDateTimeTest, PreEpochDateTimeMidnight) {
	// Exactly on a day boundary before epoch: epochMillis is negative and divisible by 86400000
	auto dt = TemporalDateTime::fromComponents(1969, 12, 31, 0, 0, 0);
	EXPECT_EQ(dt.year(), 1969);
	EXPECT_EQ(dt.month(), 12);
	EXPECT_EQ(dt.day(), 31);
	EXPECT_EQ(dt.hour(), 0);
	EXPECT_EQ(dt.minute(), 0);
	EXPECT_EQ(dt.second(), 0);
	EXPECT_EQ(dt.toISO(), "1969-12-31T00:00:00");
}

TEST(TemporalDateTimeTest, PreEpochDateTimeEarlier) {
	auto dt = TemporalDateTime::fromComponents(1960, 6, 15, 10, 30, 45);
	EXPECT_EQ(dt.year(), 1960);
	EXPECT_EQ(dt.month(), 6);
	EXPECT_EQ(dt.day(), 15);
	EXPECT_EQ(dt.hour(), 10);
	EXPECT_EQ(dt.minute(), 30);
	EXPECT_EQ(dt.second(), 45);
	EXPECT_EQ(dt.toISO(), "1960-06-15T10:30:45");
}

// --- TemporalDateTime::fromISO: date-only (matched==3) and invalid ---

TEST(TemporalDateTimeTest, FromISODateOnly) {
	// Only date part, no time: matched==3 so h/mi/sec stay 0
	auto dt = TemporalDateTime::fromISO("2024-03-15");
	EXPECT_EQ(dt.year(), 2024);
	EXPECT_EQ(dt.month(), 3);
	EXPECT_EQ(dt.day(), 15);
	EXPECT_EQ(dt.hour(), 0);
	EXPECT_EQ(dt.minute(), 0);
	EXPECT_EQ(dt.second(), 0);
}

TEST(TemporalDateTimeTest, FromISOInvalidFormat) {
	EXPECT_THROW(TemporalDateTime::fromISO("not-a-datetime"), std::runtime_error);
	EXPECT_THROW(TemporalDateTime::fromISO(""), std::runtime_error);
	EXPECT_THROW(TemporalDateTime::fromISO("2024"), std::runtime_error);
}

// --- TemporalDuration::fromISO parsing branches ---

TEST(TemporalDurationTest, FromISOOnlyDays) {
	auto dur = TemporalDuration::fromISO("P5D");
	EXPECT_EQ(dur.months, 0);
	EXPECT_EQ(dur.days, 5);
	EXPECT_EQ(dur.nanos, 0);
}

TEST(TemporalDurationTest, FromISOOnlyTime) {
	auto dur = TemporalDuration::fromISO("PT2H30M");
	EXPECT_EQ(dur.months, 0);
	EXPECT_EQ(dur.days, 0);
	int64_t expectedNanos = (2LL * 3600 + 30 * 60) * 1'000'000'000LL;
	EXPECT_EQ(dur.nanos, expectedNanos);
}

TEST(TemporalDurationTest, FromISOOnlyYears) {
	auto dur = TemporalDuration::fromISO("P3Y");
	EXPECT_EQ(dur.months, 36);
	EXPECT_EQ(dur.days, 0);
	EXPECT_EQ(dur.nanos, 0);
}

TEST(TemporalDurationTest, FromISOWeeks) {
	auto dur = TemporalDuration::fromISO("P2W");
	EXPECT_EQ(dur.months, 0);
	EXPECT_EQ(dur.days, 14);
	EXPECT_EQ(dur.nanos, 0);
}

TEST(TemporalDurationTest, FromISOMixedWithoutSomeComponents) {
	// Years and time, no months/days
	auto dur = TemporalDuration::fromISO("P1YT5M");
	EXPECT_EQ(dur.months, 12);
	EXPECT_EQ(dur.days, 0);
	int64_t expectedNanos = 5LL * 60 * 1'000'000'000LL;
	EXPECT_EQ(dur.nanos, expectedNanos);
}

TEST(TemporalDurationTest, FromISOInvalidMissingP) {
	EXPECT_THROW(TemporalDuration::fromISO("1Y2M"), std::runtime_error);
	EXPECT_THROW(TemporalDuration::fromISO(""), std::runtime_error);
}

TEST(TemporalDurationTest, FromISOInvalidChars) {
	EXPECT_THROW(TemporalDuration::fromISO("P1X"), std::runtime_error);
	EXPECT_THROW(TemporalDuration::fromISO("PT1X"), std::runtime_error);
}

TEST(TemporalDurationTest, FromISOInvalidEmptyNumber) {
	// A letter immediately after P or T with no number
	EXPECT_THROW(TemporalDuration::fromISO("PD"), std::runtime_error);
}

// --- TemporalDuration::toISO branches ---

TEST(TemporalDurationTest, ToISOOnlyMonths) {
	auto dur = TemporalDuration::fromComponents(0, 5, 0, 0, 0, 0, 0);
	EXPECT_EQ(dur.toISO(), "P5M");
}

TEST(TemporalDurationTest, ToISOOnlyDays) {
	auto dur = TemporalDuration::fromComponents(0, 0, 0, 10, 0, 0, 0);
	EXPECT_EQ(dur.toISO(), "P10D");
}

TEST(TemporalDurationTest, ToISOOnlyNanos) {
	// Only nanoseconds set, less than 1 second
	auto dur = TemporalDuration::fromComponents(0, 0, 0, 0, 0, 0, 0, 500'000'000);
	EXPECT_EQ(dur.toISO(), "PT0S");
}

TEST(TemporalDurationTest, ToISOZeroDuration) {
	TemporalDuration dur;
	dur.months = 0;
	dur.days = 0;
	dur.nanos = 0;
	EXPECT_EQ(dur.toISO(), "P0D");
}

TEST(TemporalDurationTest, ToISOWithRemainderNanos) {
	// 500 million nanos = 0.5 seconds; totalSecs=0, remainNanos=500000000
	// Because remainNanos != 0, the T section is emitted with s=0
	TemporalDuration dur;
	dur.months = 0;
	dur.days = 0;
	dur.nanos = 500'000'000;
	EXPECT_EQ(dur.toISO(), "PT0S");
}

TEST(TemporalDurationTest, ToISOYearsAndMonths) {
	auto dur = TemporalDuration::fromComponents(2, 6, 0, 0, 0, 0, 0);
	EXPECT_EQ(dur.toISO(), "P2Y6M");
}
