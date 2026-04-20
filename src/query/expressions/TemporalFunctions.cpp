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

namespace graph::query::expressions {

static PropertyValue dateImpl(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) {
	if (args.empty()) {
		return PropertyValue(TemporalDate::today());
	}
	const auto& arg = args[0];
	if (arg.getType() == PropertyType::STRING) {
		return PropertyValue(TemporalDate::fromISO(std::get<std::string>(arg.getVariant())));
	}
	if (arg.getType() == PropertyType::MAP) {
		const auto& map = arg.getMap();
		int year = 1970, month = 1, day = 1;
		auto it = map.find("year");
		if (it != map.end() && it->second.getType() == PropertyType::INTEGER)
			year = static_cast<int>(std::get<int64_t>(it->second.getVariant()));
		it = map.find("month");
		if (it != map.end() && it->second.getType() == PropertyType::INTEGER)
			month = static_cast<int>(std::get<int64_t>(it->second.getVariant()));
		it = map.find("day");
		if (it != map.end() && it->second.getType() == PropertyType::INTEGER)
			day = static_cast<int>(std::get<int64_t>(it->second.getVariant()));
		return PropertyValue(TemporalDate::fromYMD(year, month, day));
	}
	return PropertyValue();
}

static PropertyValue datetimeImpl(
	const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) {
	if (args.empty()) {
		return PropertyValue(TemporalDateTime::now());
	}
	const auto& arg = args[0];
	if (arg.getType() == PropertyType::STRING) {
		return PropertyValue(TemporalDateTime::fromISO(std::get<std::string>(arg.getVariant())));
	}
	if (arg.getType() == PropertyType::MAP) {
		const auto& map = arg.getMap();
		int y = 1970, mo = 1, d = 1, h = 0, mi = 0, s = 0, ms = 0;
		auto it = map.find("year");
		if (it != map.end() && it->second.getType() == PropertyType::INTEGER)
			y = static_cast<int>(std::get<int64_t>(it->second.getVariant()));
		it = map.find("month");
		if (it != map.end() && it->second.getType() == PropertyType::INTEGER)
			mo = static_cast<int>(std::get<int64_t>(it->second.getVariant()));
		it = map.find("day");
		if (it != map.end() && it->second.getType() == PropertyType::INTEGER)
			d = static_cast<int>(std::get<int64_t>(it->second.getVariant()));
		it = map.find("hour");
		if (it != map.end() && it->second.getType() == PropertyType::INTEGER)
			h = static_cast<int>(std::get<int64_t>(it->second.getVariant()));
		it = map.find("minute");
		if (it != map.end() && it->second.getType() == PropertyType::INTEGER)
			mi = static_cast<int>(std::get<int64_t>(it->second.getVariant()));
		it = map.find("second");
		if (it != map.end() && it->second.getType() == PropertyType::INTEGER)
			s = static_cast<int>(std::get<int64_t>(it->second.getVariant()));
		it = map.find("millisecond");
		if (it != map.end() && it->second.getType() == PropertyType::INTEGER)
			ms = static_cast<int>(std::get<int64_t>(it->second.getVariant()));
		return PropertyValue(TemporalDateTime::fromComponents(y, mo, d, h, mi, s, ms));
	}
	return PropertyValue();
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
		auto getInt = [&](const char* key) -> int64_t {
			auto it = map.find(key);
			if (it != map.end() && it->second.getType() == PropertyType::INTEGER)
				return std::get<int64_t>(it->second.getVariant());
			return 0;
		};
		return PropertyValue(TemporalDuration::fromComponents(
			getInt("years"), getInt("months"), getInt("weeks"),
			getInt("days"), getInt("hours"), getInt("minutes"),
			getInt("seconds"), getInt("nanoseconds")));
	}
	return PropertyValue();
}

void registerTemporalFunctions(FunctionRegistry& registry) {
	registry.registerFunction(makeFn("date", 0, 1, &dateImpl));
	registry.registerFunction(makeFn("datetime", 0, 1, &datetimeImpl));
	registry.registerFunction(makeFn("duration", 1, 1, &durationImpl));
}

} // namespace graph::query::expressions
