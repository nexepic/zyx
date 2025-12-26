/**
 * @file SystemConfigManager.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/15
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

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

		void onStateUpdated(const State& oldState, const State& newState) override;

	private:
		std::weak_ptr<storage::FileStorage> storage_;
		std::shared_ptr<storage::state::SystemStateManager> systemStateManager_;

		// --- Specific Applicators ---
		void applyLogLevel() const;
		void applyCompaction() const;
		// void applyMemoryLimit(); // Future extension
	};

} // namespace graph::config