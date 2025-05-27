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
#include <graph/core/Types.h>

namespace graph::storage {

	using SegmentType = EntityType;

	constexpr uint32_t toUnderlying(SegmentType type) {
		return static_cast<uint32_t>(type);
	}

} // namespace graph::storage