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
#include <cctype>
#include <random>
#include <stdexcept>

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

[[nodiscard]] uint8_t hexDigit(char value) {
	if (value >= '0' && value <= '9') {
		return static_cast<uint8_t>(value - '0');
	}
	const char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
	if (lower >= 'a' && lower <= 'f') {
		return static_cast<uint8_t>(lower - 'a' + 10);
	}
	throw std::invalid_argument("bytes() expects an even-length hexadecimal string");
}

static PropertyValue bytesImpl(const std::vector<PropertyValue> &args,
								[[maybe_unused]] const EvaluationContext &context) {
	if (args.empty() || EvaluationContext::isNull(args.front())) {
		return PropertyValue();
	}
	if (args.front().getType() == PropertyType::BYTES) {
		return PropertyValue(PropertyValue::Bytes(args.front().getBytes()));
	}
	if (args.front().getType() == PropertyType::STRING) {
		std::string hex = std::get<std::string>(args.front().getVariant());
		if (hex.size() >= 2U && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) {
			hex.erase(0U, 2U);
		}
		if ((hex.size() & 1U) != 0U) {
			throw std::invalid_argument("bytes() expects an even-length hexadecimal string");
		}
		PropertyValue::Bytes result;
		result.reserve(hex.size() / 2U);
		for (size_t index = 0U; index < hex.size(); index += 2U) {
			result.push_back(static_cast<std::byte>((hexDigit(hex[index]) << 4U) | hexDigit(hex[index + 1U])));
		}
		return PropertyValue(std::move(result));
	}
	if (args.front().getType() == PropertyType::LIST) {
		PropertyValue::Bytes result;
		const auto &values = args.front().getList();
		result.reserve(values.size());
		for (const PropertyValue &value: values) {
			if (value.getType() != PropertyType::INTEGER) {
				throw std::invalid_argument("bytes() list elements must be integers in [0, 255]");
			}
			const int64_t number = std::get<int64_t>(value.getVariant());
			if (number < 0 || number > 255) {
				throw std::invalid_argument("bytes() list elements must be integers in [0, 255]");
			}
			result.push_back(static_cast<std::byte>(number));
		}
		return PropertyValue(std::move(result));
	}
	throw std::invalid_argument("bytes() expects bytes, a hexadecimal string, or a byte list");
}

void registerUtilityFunctions(FunctionRegistry& registry) {
	registry.registerFunction(makeFn("timestamp", 0, 0, &timestampImpl));
	registry.registerFunction(makeFn("randomUUID", 0, 0, &randomUUIDImpl));
	registry.registerFunction(makeFn("bytes", 1, 1, &bytesImpl));
}

} // namespace graph::query::expressions
