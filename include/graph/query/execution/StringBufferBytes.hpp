#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace graph::query::execution::detail {

// The owning string object is accounted separately. A small external allocation
// can have capacity below sizeof(string), so capacity does not identify SSO.
[[nodiscard]] inline size_t dynamicStringBufferBytes(const std::string &value) {
	const auto object = reinterpret_cast<std::uintptr_t>(&value);
	const auto data = reinterpret_cast<std::uintptr_t>(value.data());
	if (data >= object && data - object < sizeof(value)) return 0U;
	if (value.capacity() == std::numeric_limits<size_t>::max())
		throw std::overflow_error("string buffer capacity overflowed");
	return value.capacity() + 1U;
}

} // namespace graph::query::execution::detail
