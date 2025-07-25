/**
 * @file EntityChangeType.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/7/24
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

namespace graph::storage {

	/**
	 * Represents the type of change made to an entity
	 */
	enum class EntityChangeType {
		ADDED,    // Entity was newly added
		MODIFIED, // Entity was modified
		DELETED   // Entity was deleted
	};

} // namespace graph::storage