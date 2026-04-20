/**
 * @file UtilityFunctions.cpp
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
#include <chrono>
#include <random>

namespace graph::query::expressions {

static PropertyValue timestampImpl(
	[[maybe_unused]] const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) {
	auto now = std::chrono::system_clock::now();
	auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
		now.time_since_epoch()
	).count();
	return PropertyValue(static_cast<int64_t>(millis));
}

static PropertyValue randomUUIDImpl(
	[[maybe_unused]] const std::vector<PropertyValue>& args,
	[[maybe_unused]] const EvaluationContext& context
) {
	static thread_local std::mt19937 gen(std::random_device{}());
	std::uniform_int_distribution<uint32_t> dist(0, 15);
	std::uniform_int_distribution<uint32_t> dist2(8, 11);

	const char* hex = "0123456789abcdef";
	std::string uuid(36, '-');
	for (int i = 0; i < 36; i++) {
		if (i == 8 || i == 13 || i == 18 || i == 23) {
			continue;
		} else if (i == 14) {
			uuid[i] = '4';
		} else if (i == 19) {
			uuid[i] = hex[dist2(gen)];
		} else {
			uuid[i] = hex[dist(gen)];
		}
	}
	return PropertyValue(uuid);
}

void registerUtilityFunctions(FunctionRegistry& registry) {
	registry.registerFunction(makeFn("timestamp", 0, 0, &timestampImpl));
	registry.registerFunction(makeFn("randomUUID", 0, 0, &randomUUIDImpl));
}

} // namespace graph::query::expressions
