/**
 * @file Types.h
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/3/18
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once
#include <cstdint>

namespace graph {

	struct PropertyReference {
		enum class StorageType : uint8_t {
			NONE = 0,       // No property
			SEGMENT = 1,    // Property stored in property segment
			BLOB = 2        // Property stored in BLOB (large property)
		    };

		StorageType type = StorageType::NONE;
		uint64_t reference = 0;  // Segment offset or BLOB ID
		uint32_t size = 0;       // Property data size
	};

	enum class Direction {
		INCOMING,
		OUTGOING,
		BOTH
	};

} // namespace graph
