/**
 * @file TemporalTypes.hpp
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

#pragma once

#include <cstdint>
#include <string>

namespace graph {

/**
 * @struct TemporalDate
 * @brief Represents a date as days since 1970-01-01 (epoch).
 */
struct TemporalDate {
	int32_t epochDays = 0;

	static TemporalDate today();
	static TemporalDate fromISO(const std::string& s);
	static TemporalDate fromYMD(int year, int month, int day);

	[[nodiscard]] int year() const;
	[[nodiscard]] int month() const;
	[[nodiscard]] int day() const;
	[[nodiscard]] std::string toISO() const;

	auto operator<=>(const TemporalDate&) const = default;
	bool operator==(const TemporalDate&) const = default;
};

/**
 * @struct TemporalDateTime
 * @brief Represents a datetime as milliseconds since epoch.
 */
struct TemporalDateTime {
	int64_t epochMillis = 0;

	static TemporalDateTime now();
	static TemporalDateTime fromISO(const std::string& s);
	static TemporalDateTime fromComponents(int y, int mo, int d, int h, int mi, int s, int ms = 0);

	[[nodiscard]] int year() const;
	[[nodiscard]] int month() const;
	[[nodiscard]] int day() const;
	[[nodiscard]] int hour() const;
	[[nodiscard]] int minute() const;
	[[nodiscard]] int second() const;
	[[nodiscard]] std::string toISO() const;

	auto operator<=>(const TemporalDateTime&) const = default;
	bool operator==(const TemporalDateTime&) const = default;
};

/**
 * @struct TemporalDuration
 * @brief ISO 8601 duration with months, days, and nanoseconds components.
 */
struct TemporalDuration {
	int64_t months = 0;
	int64_t days = 0;
	int64_t nanos = 0;

	static TemporalDuration fromISO(const std::string& s);
	static TemporalDuration fromComponents(int64_t years, int64_t months, int64_t weeks,
	                                        int64_t days, int64_t hours, int64_t minutes,
	                                        int64_t seconds, int64_t nanoseconds = 0);
	[[nodiscard]] std::string toISO() const;

	bool operator==(const TemporalDuration& o) const = default;

	// Total-order approximation: 1 month = 30 days, compare by total days then nanos
	bool operator<(const TemporalDuration& o) const {
		int64_t totalDays = months * 30 + days;
		int64_t otherTotalDays = o.months * 30 + o.days;
		if (totalDays != otherTotalDays) return totalDays < otherTotalDays;
		return nanos < o.nanos;
	}
	bool operator>(const TemporalDuration& o) const { return o < *this; }
	bool operator<=(const TemporalDuration& o) const { return !(o < *this); }
	bool operator>=(const TemporalDuration& o) const { return !(*this < o); }
};

} // namespace graph
