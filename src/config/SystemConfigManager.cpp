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
#include "graph/storage/state/SystemStateKeys.hpp"
#include "graph/log/Log.hpp"
#include <algorithm>
#include <string>

namespace graph::config {

	using namespace storage::state;

	SystemConfigManager::SystemConfigManager(std::shared_ptr<SystemStateManager> systemStateManager)
		: systemStateManager_(std::move(systemStateManager)) {}

	void SystemConfigManager::loadAndApplyAll() const {
		applyLogLevel();
	}

	void SystemConfigManager::onStateUpdated([[maybe_unused]] const State& oldState, const State& newState) {
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
		std::string logLevel = systemStateManager_->get<std::string>(
			keys::SYS_CONFIG,
			keys::Config::LOG_LEVEL,
			"INFO"
		);

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

} // namespace graph::config