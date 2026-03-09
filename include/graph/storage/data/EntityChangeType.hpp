/**
 * @file EntityChangeType.hpp
 * @author Nexepic
 * @date 2025/7/24
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 **/

#pragma once

namespace graph::storage {

	/**
	 * Represents the type of change made to an entity
	 */
	enum class EntityChangeType {
		CHANGE_ADDED, // Entity was newly added
		CHANGE_MODIFIED, // Entity was modified
		CHANGE_DELETED // Entity was deleted
	};

} // namespace graph::storage
