/**
 * @file Types.hpp
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

	enum class EntityType : uint32_t { Node, Edge, Property, Blob };

	constexpr uint32_t toUnderlying(EntityType type) {
		return static_cast<uint32_t>(type);
	}

	enum class PropertyStorageType : uint32_t { NONE, PROPERTY_ENTITY, BLOB_ENTITY };

} // namespace graph
