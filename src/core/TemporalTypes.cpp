/**
 * @file TemporalTypes.cpp
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

#include "graph/core/TemporalTypes.hpp"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <sstream>
#include <iomanip>

namespace graph {

// ============================================================================
// Julian Day Number helpers (portable date arithmetic without C++20 calendar)
// ============================================================================

namespace {

// Convert y/m/d to days since epoch (1970-01-01)
int32_t ymdToEpochDays(int y, int m, int d) {
	// Algorithm from Howard Hinnant's date library (public domain)
	y -= (m <= 2);
	int era = (y >= 0 ? y : y - 399) / 400;
	unsigned yoe = static_cast<unsigned>(y - era * 400);
	unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
	unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return static_cast<int32_t>(era * 146097 + static_cast<int>(doe) - 719468);
}

// Convert epoch days to y/m/d
void epochDaysToYMD(int32_t epochDays, int& y, int& m, int& d) {
	int z = epochDays + 719468;
	int era = (z >= 0 ? z : z - 146096) / 146097;
	unsigned doe = static_cast<unsigned>(z - era * 146097);
	unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	y = static_cast<int>(yoe) + era * 400;
	unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	unsigned mp = (5 * doy + 2) / 153;
	d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
	m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
	y += (m <= 2);
}

bool isLeapYear(int y) {
	return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

int daysInMonth(int y, int m) {
	static const int dims[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	if (m == 2 && isLeapYear(y)) return 29;
	return dims[m];
}

} // anonymous namespace

// ============================================================================
// TemporalDate
// ============================================================================

TemporalDate TemporalDate::today() {
	auto now = std::chrono::system_clock::now();
	auto daysSinceEpoch = std::chrono::duration_cast<std::chrono::hours>(
		now.time_since_epoch()).count() / 24;
	return TemporalDate{static_cast<int32_t>(daysSinceEpoch)};
}

TemporalDate TemporalDate::fromISO(const std::string& s) {
	int y = 0, m = 0, d = 0;
	if (std::sscanf(s.c_str(), "%d-%d-%d", &y, &m, &d) != 3) {
		throw std::runtime_error("Invalid date format: " + s + " (expected YYYY-MM-DD)");
	}
	if (m < 1 || m > 12 || d < 1 || d > daysInMonth(y, m)) {
		throw std::runtime_error("Invalid date values: " + s);
	}
	return fromYMD(y, m, d);
}

TemporalDate TemporalDate::fromYMD(int year, int month, int day) {
	return TemporalDate{ymdToEpochDays(year, month, day)};
}

int TemporalDate::year() const {
	int y, m, d;
	epochDaysToYMD(epochDays, y, m, d);
	return y;
}

int TemporalDate::month() const {
	int y, m, d;
	epochDaysToYMD(epochDays, y, m, d);
	return m;
}

int TemporalDate::day() const {
	int y, m, d;
	epochDaysToYMD(epochDays, y, m, d);
	return d;
}

std::string TemporalDate::toISO() const {
	int y, m, d;
	epochDaysToYMD(epochDays, y, m, d);
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", y, m, d);
	return buf;
}

// ============================================================================
// TemporalDateTime
// ============================================================================

TemporalDateTime TemporalDateTime::now() {
	auto n = std::chrono::system_clock::now();
	auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
		n.time_since_epoch()).count();
	return TemporalDateTime{ms};
}

TemporalDateTime TemporalDateTime::fromISO(const std::string& s) {
	int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
	// Try full datetime first
	int matched = std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &sec);
	if (matched < 3) {
		throw std::runtime_error("Invalid datetime format: " + s);
	}
	return fromComponents(y, mo, d, h, mi, sec, 0);
}

TemporalDateTime TemporalDateTime::fromComponents(int y, int mo, int d, int h, int mi, int s, int ms) {
	int32_t epochDays = ymdToEpochDays(y, mo, d);
	int64_t totalMs = static_cast<int64_t>(epochDays) * 86400LL * 1000LL +
	                  static_cast<int64_t>(h) * 3600000LL +
	                  static_cast<int64_t>(mi) * 60000LL +
	                  static_cast<int64_t>(s) * 1000LL +
	                  static_cast<int64_t>(ms);
	return TemporalDateTime{totalMs};
}

int TemporalDateTime::year() const {
	int32_t days = static_cast<int32_t>(epochMillis / (86400LL * 1000LL));
	if (epochMillis < 0 && (epochMillis % (86400LL * 1000LL)) != 0) days--;
	int y, m, d;
	epochDaysToYMD(days, y, m, d);
	return y;
}

int TemporalDateTime::month() const {
	int32_t days = static_cast<int32_t>(epochMillis / (86400LL * 1000LL));
	if (epochMillis < 0 && (epochMillis % (86400LL * 1000LL)) != 0) days--;
	int y, m, d;
	epochDaysToYMD(days, y, m, d);
	return m;
}

int TemporalDateTime::day() const {
	int32_t days = static_cast<int32_t>(epochMillis / (86400LL * 1000LL));
	if (epochMillis < 0 && (epochMillis % (86400LL * 1000LL)) != 0) days--;
	int y, m, d;
	epochDaysToYMD(days, y, m, d);
	return d;
}

int TemporalDateTime::hour() const {
	int64_t msOfDay = epochMillis % (86400LL * 1000LL);
	if (msOfDay < 0) msOfDay += 86400LL * 1000LL;
	return static_cast<int>(msOfDay / 3600000LL);
}

int TemporalDateTime::minute() const {
	int64_t msOfDay = epochMillis % (86400LL * 1000LL);
	if (msOfDay < 0) msOfDay += 86400LL * 1000LL;
	return static_cast<int>((msOfDay % 3600000LL) / 60000LL);
}

int TemporalDateTime::second() const {
	int64_t msOfDay = epochMillis % (86400LL * 1000LL);
	if (msOfDay < 0) msOfDay += 86400LL * 1000LL;
	return static_cast<int>((msOfDay % 60000LL) / 1000LL);
}

std::string TemporalDateTime::toISO() const {
	int32_t days = static_cast<int32_t>(epochMillis / (86400LL * 1000LL));
	if (epochMillis < 0 && (epochMillis % (86400LL * 1000LL)) != 0) days--;
	int y, m, d;
	epochDaysToYMD(days, y, m, d);
	char buf[64];
	std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d",
	              y, m, d, hour(), minute(), second());
	return buf;
}

// ============================================================================
// TemporalDuration
// ============================================================================

TemporalDuration TemporalDuration::fromISO(const std::string& s) {
	if (s.empty() || s[0] != 'P') {
		throw std::runtime_error("Invalid duration format: " + s + " (must start with P)");
	}

	int64_t years = 0, mos = 0, weeks = 0, ds = 0;
	int64_t hours = 0, mins = 0, secs = 0;
	bool inTime = false;
	size_t i = 1;
	std::string numBuf;

	while (i < s.size()) {
		char c = s[i];
		if (c == 'T') {
			inTime = true;
			i++;
			continue;
		}
		if (c >= '0' && c <= '9') {
			numBuf += c;
			i++;
			continue;
		}
		if (numBuf.empty()) {
			throw std::runtime_error("Invalid duration format: " + s);
		}
		int64_t num = std::stoll(numBuf);
		numBuf.clear();

		if (!inTime) {
			switch (c) {
				case 'Y': years = num; break;
				case 'M': mos = num; break;
				case 'W': weeks = num; break;
				case 'D': ds = num; break;
				default:
					throw std::runtime_error("Invalid duration component: " + std::string(1, c));
			}
		} else {
			switch (c) {
				case 'H': hours = num; break;
				case 'M': mins = num; break;
				case 'S': secs = num; break;
				default:
					throw std::runtime_error("Invalid duration time component: " + std::string(1, c));
			}
		}
		i++;
	}

	return fromComponents(years, mos, weeks, ds, hours, mins, secs, 0);
}

TemporalDuration TemporalDuration::fromComponents(int64_t years, int64_t mos, int64_t weeks,
                                                    int64_t ds, int64_t hours, int64_t minutes,
                                                    int64_t seconds, int64_t nanoseconds) {
	TemporalDuration dur;
	dur.months = years * 12 + mos;
	dur.days = weeks * 7 + ds;
	dur.nanos = hours * 3600LL * 1000000000LL +
	            minutes * 60LL * 1000000000LL +
	            seconds * 1000000000LL +
	            nanoseconds;
	return dur;
}

std::string TemporalDuration::toISO() const {
	std::ostringstream oss;
	oss << "P";

	int64_t y = months / 12;
	int64_t m = months % 12;
	if (y != 0) oss << y << "Y";
	if (m != 0) oss << m << "M";
	if (days != 0) oss << days << "D";

	int64_t totalSecs = nanos / 1000000000LL;
	int64_t remainNanos = nanos % 1000000000LL;
	int64_t h = totalSecs / 3600;
	int64_t mi = (totalSecs % 3600) / 60;
	int64_t s = totalSecs % 60;

	if (h != 0 || mi != 0 || s != 0 || remainNanos != 0) {
		oss << "T";
		if (h != 0) oss << h << "H";
		if (mi != 0) oss << mi << "M";
		if (s != 0 || remainNanos != 0) oss << s << "S";
	}

	std::string result = oss.str();
	if (result == "P") result = "P0D";
	return result;
}

} // namespace graph
