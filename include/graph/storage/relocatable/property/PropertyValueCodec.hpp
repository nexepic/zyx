/**
 * @file PropertyValueCodec.hpp
 * @brief Canonical cross-platform PropertyValue wire codec.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "graph/core/Property.hpp"
#include "graph/storage/relocatable/format/Format.hpp"

namespace graph::storage {

	// Values are self-delimiting, so collection codecs can consume one value at
	// a time without deriving boundaries from native C++ object layout.
	class CanonicalPropertyValueCodec final {
	public:
		static void append(std::vector<std::byte> &bytes, const PropertyValue &value);
		[[nodiscard]] static PropertyValue decodeNext(std::span<const std::byte> bytes, size_t &offset);
	};

} // namespace graph::storage
