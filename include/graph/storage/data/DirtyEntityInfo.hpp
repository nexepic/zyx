/**
 * @file DirtyEntityInfo.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/7/24
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <optional>
#include "EntityChangeType.hpp"

namespace graph::storage {

	/**
	 * Tracks information about changes to an entity
	 * @tparam T The entity type
	 */
	template<typename T>
	struct DirtyEntityInfo {
		EntityChangeType changeType;
		std::optional<T> backup;

		// Default constructor
		DirtyEntityInfo() : changeType(EntityChangeType::MODIFIED), backup(std::nullopt) {}

		// Constructor with entity
		DirtyEntityInfo(EntityChangeType type, const T& entity) : changeType(type) {
			backup.emplace(entity);
		}

		// Constructor with just type
		explicit DirtyEntityInfo(EntityChangeType type) : changeType(type), backup(std::nullopt) {}
	};

} // namespace graph::storage