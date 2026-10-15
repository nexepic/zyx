#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include "graph/utils/StandardContainerHeapBytes.hpp"

namespace graph::utils::detail {

// The owning string object is accounted separately. A small external allocation
// can have capacity below sizeof(string), so capacity does not identify SSO.
// MSVC iterator-debug proxies and allocator alignment prefixes are heap bytes.
[[nodiscard]] inline size_t dynamicStringBufferBytes(const std::string &value) {
	const auto object = reinterpret_cast<std::uintptr_t>(&value);
	const auto data = reinterpret_cast<std::uintptr_t>(value.data());
	if (data >= object && data - object < sizeof(value)) return standardContainerProxyBytes();
	if (value.capacity() == std::numeric_limits<size_t>::max())
		throw std::overflow_error("string buffer capacity overflowed");
	return addStandardHeapBytes(standardContainerProxyBytes(),
        standardAllocatorRequestBytes(value.capacity() + 1U, alignof(char)));
}

} // namespace graph::utils::detail
