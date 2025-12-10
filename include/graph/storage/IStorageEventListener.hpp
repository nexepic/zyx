/**
 * @file IStorageEventListener.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/10
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
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