/**
 * @file SegmentType.h
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/5/6
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once
#include <cstdint>

namespace graph::storage {

	enum class SegmentType : uint8_t { Node = 0, Edge = 1, Property = 2, Blob = 3 };

	constexpr uint8_t toUnderlying(SegmentType type) {
		return static_cast<uint8_t>(type);
	}

} // namespace graph::storage