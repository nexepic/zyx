/**
 * @file SystemConfigManager.hpp
 * @author Nexepic
 * @date 2025/12/15
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

#include <functional>
#include <memory>

#include "graph/storage/FileStorage.hpp"
#include "graph/storage/state/SystemStateManager.hpp"

namespace graph::config {

	/**
	 * @class SystemConfigManager
	 * @brief Responsible for reading configurations from SystemStateManager
	 *        and applying them to the runtime environment (e.g., Logger, Memory Manager).
	 *
	 * This class is Read-Only regarding the StateManager. It does not handle
	 * writing configurations (which is handled by QueryExecutor or other modules).
	 */
	class SystemConfigManager : public IEntityObserver {
	public:
		SystemConfigManager(std::shared_ptr<storage::state::SystemStateManager> systemStateManager,
							const std::shared_ptr<storage::FileStorage> &storage);
		~SystemConfigManager() override = default;

		/**
		 * @brief Reads all known configurations from the persistent state
		 *        and applies them to the current running instance.
		 *
		 * This should be called:
		 * 1. On Database startup (open).
		 * 2. Whenever a configuration change is committed (if dynamic reload is supported).
		 */
		void loadAndApplyAll() const;

		void onStateUpdated(const State &oldState, const State &newState) override;

		/**
		 * @brief Reads the configured thread pool size from state.
		 * @return Thread pool size (0 = auto, 1 = single-threaded, >1 = N threads)
		 */
		[[nodiscard]] size_t getThreadPoolSize() const;

		[[nodiscard]] bool isSlowLogEnabled() const;
		[[nodiscard]] int64_t getSlowLogThresholdMs() const;
		[[nodiscard]] bool isStatsEnabled() const;

	private:
		std::weak_ptr<storage::FileStorage> storage_;
		std::shared_ptr<storage::state::SystemStateManager> systemStateManager_;

		// --- Specific Applicators ---
		void applyLogLevel() const;
		void applyCompaction() const;
	};

} // namespace graph::config
