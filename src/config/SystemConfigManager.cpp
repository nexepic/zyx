/**
 * @file SystemConfigManager.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/15
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
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

} // namespace graph::config
