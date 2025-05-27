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

namespace graph {

	enum class EntityType : uint32_t { Node, Edge, Property, Blob };

	enum class PropertyStorageType : uint32_t { NONE, PROPERTY_ENTITY, BLOB_ENTITY };

	enum class Direction { INCOMING, OUTGOING, BOTH };

} // namespace graph
