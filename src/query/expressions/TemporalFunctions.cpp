/**
 * @file TemporalFunctions.cpp
 * @author ZYX Contributors
 * @date 2025
 *
 * @copyright Copyright (c) 2025
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

#include "BuiltinFunctions.hpp"
#include "graph/core/TemporalTypes.hpp"

#include <limits>
#include <stdexcept>

namespace graph::query::expressions {

namespace {

[[nodiscard]] int mapIntComponent(const PropertyValue::MapType &map, const char *key, int defaultValue) {
	const auto found = map.find(key);
	if (found == map.end()) {
		return defaultValue;
	}
	if (found->second.getType() != PropertyType::INTEGER) {
		throw std::invalid_argument(std::string("temporal map component '") + key + "' must be an integer");
	}
	const int64_t value = std::get<int64_t>(found->second.getVariant());
	if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max()) {
		throw std::out_of_range(std::string("temporal map component '") + key + "' is outside the supported range");
	}
	return static_cast<int>(value);
}

[[nodiscard]] int64_t mapInt64Component(const PropertyValue::MapType &map, const char *key, int64_t defaultValue) {
	const auto found = map.find(key);
	if (found == map.end()) {
		return defaultValue;
	}
	if (found->second.getType() != PropertyType::INTEGER) {
		throw std::invalid_argument(std::string("temporal map component '") + key + "' must be an integer");
	}
	return std::get<int64_t>(found->second.getVariant());
}

} // namespace

static PropertyValue dateImpl(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) {
	if (args.empty()) {
		return PropertyValue(TemporalDate::today());
	}
	const auto& arg = args[0];
	if (EvaluationContext::isNull(arg)) {
		return PropertyValue();
	}
	if (arg.getType() == PropertyType::STRING) {
		return PropertyValue(TemporalDate::fromISO(std::get<std::string>(arg.getVariant())));
	}
	if (arg.getType() == PropertyType::MAP) {
		const auto& map = arg.getMap();
		const int year = mapIntComponent(map, "year", 1970);
		const int month = mapIntComponent(map, "month", 1);
		const int day = mapIntComponent(map, "day", 1);
		return PropertyValue(TemporalDate::fromYMD(year, month, day));
	}
	throw std::invalid_argument("date() requires a string, map, or NULL argument");
}

static PropertyValue datetimeImpl(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) {
	if (args.empty()) {
		return PropertyValue(TemporalDateTime::now());
	}
	const auto& arg = args[0];
	if (EvaluationContext::isNull(arg)) {
		return PropertyValue();
	}
	if (arg.getType() == PropertyType::STRING) {
		return PropertyValue(TemporalDateTime::fromISO(std::get<std::string>(arg.getVariant())));
	}
	if (arg.getType() == PropertyType::MAP) {
		const auto& map = arg.getMap();
		const int y = mapIntComponent(map, "year", 1970);
		const int mo = mapIntComponent(map, "month", 1);
		const int d = mapIntComponent(map, "day", 1);
		const int h = mapIntComponent(map, "hour", 0);
		const int mi = mapIntComponent(map, "minute", 0);
		const int s = mapIntComponent(map, "second", 0);
		const int ms = mapIntComponent(map, "millisecond", 0);
		return PropertyValue(TemporalDateTime::fromComponents(y, mo, d, h, mi, s, ms));
	}
	throw std::invalid_argument("datetime() requires a string, map, or NULL argument");
}

static PropertyValue durationImpl(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) {
	if (args.empty() || EvaluationContext::isNull(args[0])) return PropertyValue();
	const auto& arg = args[0];
	if (arg.getType() == PropertyType::STRING) {
		return PropertyValue(TemporalDuration::fromISO(std::get<std::string>(arg.getVariant())));
	}
	if (arg.getType() == PropertyType::MAP) {
		const auto& map = arg.getMap();
		return PropertyValue(TemporalDuration::fromComponents(
			mapInt64Component(map, "years", 0), mapInt64Component(map, "months", 0),
			mapInt64Component(map, "weeks", 0), mapInt64Component(map, "days", 0),
			mapInt64Component(map, "hours", 0), mapInt64Component(map, "minutes", 0),
			mapInt64Component(map, "seconds", 0), mapInt64Component(map, "nanoseconds", 0)));
	}
	throw std::invalid_argument("duration() requires a string, map, or NULL argument");
}

void registerTemporalFunctions(FunctionRegistry& registry) {
	registry.registerFunction(makeFn("date", 0, 1, &dateImpl));
	registry.registerFunction(makeFn("datetime", 0, 1, &datetimeImpl));
	registry.registerFunction(makeFn("duration", 1, 1, &durationImpl));
}

} // namespace graph::query::expressions
