/**
 * @file IStorageEventListener.hpp
 * @author Nexepic
 * @date 2025/12/10
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
	 * @interface IStorageEventListener
	 * @brief Implement this interface to receive notifications from FileStorage.
	 *
	 * This allows higher-level components (like IndexManager) to hook into
	 * low-level storage operations (like flush) without creating circular dependencies.
	 */
	class IStorageEventListener {
	public:
		virtual ~IStorageEventListener() = default;

		/**
		 * @brief Called immediately before FileStorage persists data to disk.
		 *
		 * Components should use this hook to serialize their in-memory state
		 * (e.g., flushing indexes, dirty caches) to ensure consistency.
		 */
		virtual void onStorageFlush() = 0;
	};

} // namespace graph::storage
