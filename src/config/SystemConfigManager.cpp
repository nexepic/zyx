/**
 * @file SystemConfigManager.cpp
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

#include "graph/config/SystemConfigManager.hpp"
#include <algorithm>
#include <string>
#include "graph/log/Log.hpp"
#include "graph/storage/state/SystemStateKeys.hpp"

namespace graph::config {

	using namespace storage::state;

	SystemConfigManager::SystemConfigManager(std::shared_ptr<SystemStateManager> systemStateManager,
											 const std::shared_ptr<storage::FileStorage> &storage) :
		storage_(storage), systemStateManager_(std::move(systemStateManager)) {}

	void SystemConfigManager::loadAndApplyAll() const {
		applyLogLevel();
		applyCompaction();
	}

	void SystemConfigManager::onStateUpdated([[maybe_unused]] const State &oldState, const State &newState) {
		// Check if the updated state is the System Configuration
		if (newState.getKey() == keys::SYS_CONFIG) {
			log::Log::info("System configuration change detected. Reloading...");

			// Reload settings
			loadAndApplyAll();
		}
	}

	void SystemConfigManager::applyLogLevel() const {
		// 1. Read from State Manager
		// Key: "sys.config", Field: "log.level", Default: "INFO"
		auto logLevel = systemStateManager_->get<std::string>(keys::SYS_CONFIG, keys::Config::LOG_LEVEL, "INFO");

		// 2. Normalize string (toupper)
		std::string upperLevel = logLevel;
		std::ranges::transform(upperLevel, upperLevel.begin(), ::toupper);

		// 3. Apply to Log Subsystem
		if (upperLevel == "DEBUG") {
			log::Log::setDebug(true);
			// Optional: Log that configuration was loaded
			log::Log::debug("Configuration loaded: Log level set to DEBUG");
		} else {
			log::Log::setDebug(false);
		}
	}

	void SystemConfigManager::applyCompaction() const {
		// 1. Resolve FileStorage
		auto storage = storage_.lock();
		if (!storage) {
			return; // Storage might be destroying or not initialized yet
		}

		// 2. Read config
		// Key: "sys.config", Field: "storage.compaction.enabled", Default: true
		bool enabled = systemStateManager_->get<bool>(keys::SYS_CONFIG, keys::Config::STORAGE_COMPACTION_ENABLED,
													  false // Default to enabled for safety
		);

		// 3. Apply to Storage
		bool currentStatus = storage->isCompactionEnabled();
		if (currentStatus != enabled) {
			storage->setCompactionEnabled(enabled);
			log::Log::info("Configuration applied: Storage Compaction is now {}", enabled ? "ENABLED" : "DISABLED");
		}
	}

	size_t SystemConfigManager::getThreadPoolSize() const {
		auto val = systemStateManager_->get<int64_t>(keys::SYS_CONFIG, keys::Config::THREAD_POOL_SIZE, 1);
		return val < 0 ? 1 : static_cast<size_t>(val);
	}

	bool SystemConfigManager::isSlowLogEnabled() const {
		return systemStateManager_->get<bool>(keys::SYS_CONFIG, keys::Config::SLOW_LOG_ENABLED, false);
	}

	int64_t SystemConfigManager::getSlowLogThresholdMs() const {
		return systemStateManager_->get<int64_t>(keys::SYS_CONFIG, keys::Config::SLOW_LOG_THRESHOLD_MS, 1000);
	}

	bool SystemConfigManager::isStatsEnabled() const {
		return systemStateManager_->get<bool>(keys::SYS_CONFIG, keys::Config::STATS_ENABLED, true);
	}

} // namespace graph::config
