/**
 * @file LabelIndex.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/3/21
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/storage/indexes/LabelIndex.hpp"
#include <algorithm>
#include <graph/storage/IDAllocator.hpp>
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/data/StateManager.hpp"
#include "graph/storage/state/SystemStateKeys.hpp"

namespace graph::query::indexes {

	LabelIndex::LabelIndex(std::shared_ptr<storage::DataManager> dataManager,
						   std::shared_ptr<storage::state::SystemStateManager> systemStateManager, uint32_t indexType,
						   std::string stateKey) :
		dataManager_(std::move(dataManager)), systemStateManager_(std::move(systemStateManager)),
		treeManager_(std::make_shared<IndexTreeManager>(dataManager_, indexType, PropertyType::STRING)),
		stateKey_(std::move(stateKey)) {
		initialize();
	}

	void LabelIndex::initialize() {
		std::unique_lock lock(mutex_);

		// 1. Load Root ID
		// Logic: "Get an int64 from the stateKey under field 'rootId'"
		rootId_ = systemStateManager_->get<int64_t>(stateKey_, storage::state::keys::Fields::ROOT_ID, 0);

		// 2. Load Enabled Config
		// Logic: "Construct config key, then get a bool under field 'enabled'"
		std::string configKey = stateKey_ + storage::state::keys::SUFFIX_CONFIG;
		enabled_ = systemStateManager_->get<bool>(configKey, storage::state::keys::Fields::ENABLED, false);

		// Fallback for backward compatibility
		if (!enabled_ && rootId_ != 0) {
			enabled_ = true;
		}
	}

	void LabelIndex::createIndex() {
		std::unique_lock lock(mutex_);
		enabled_ = true;
		// Ideally, persist immediately or wait for flush.
		// Setting flag in memory is enough for listIndexes() to work immediately.
	}

	bool LabelIndex::isEmpty() const {
		std::shared_lock lock(mutex_);
		// It's empty if it's not enabled.
		// If it IS enabled, it's considered "present" even if it contains no nodes (rootId_ == 0).
		return !enabled_;
	}

	void LabelIndex::clear() {
		std::unique_lock lock(mutex_);
		if (rootId_ != 0) {
			treeManager_->clear(rootId_);
			// After clearing, reset rootId to 0
			rootId_ = 0;
		}
	}

	void LabelIndex::drop() {
		clear();
		std::unique_lock lock(mutex_);
		enabled_ = false;

		// Explicitly remove the specific states used by this module
		systemStateManager_->remove(stateKey_);
		systemStateManager_->remove(stateKey_ + storage::state::keys::SUFFIX_CONFIG);
	}

	void LabelIndex::saveState() const {
		std::shared_lock lock(mutex_);
		if (enabled_) {
			// Save Root
			systemStateManager_->set<int64_t>(stateKey_, storage::state::keys::Fields::ROOT_ID, rootId_);

			// Save Enabled Config
			systemStateManager_->set<bool>(stateKey_ + storage::state::keys::SUFFIX_CONFIG,
										   storage::state::keys::Fields::ENABLED, true);
		}
	}

	void LabelIndex::flush() const {
		// std::unique_lock lock(mutex_);
		// Call the new saveState method instead of saveRootId
		saveState();
	}

	void LabelIndex::addNode(int64_t entityId, const std::string &label) {
		std::unique_lock lock(mutex_);
		if (rootId_ == 0) {
			rootId_ = treeManager_->initialize();
		}
		int64_t newRootId = treeManager_->insert(rootId_, label, entityId);
		if (newRootId != rootId_) {
			rootId_ = newRootId;
		}
	}

	void LabelIndex::removeNode(int64_t entityId, const std::string &label) {
		std::unique_lock lock(mutex_);
		if (rootId_ == 0) {
			return;
		}
		int64_t newRootId = treeManager_->remove(rootId_, label, entityId);
		if (newRootId != rootId_) {
			rootId_ = newRootId;
		}
	}

	std::vector<int64_t> LabelIndex::findNodes(const std::string &label) const {
		std::shared_lock lock(mutex_);

		if (rootId_ == 0) {
			return {};
		}

		return treeManager_->find(rootId_, label);
	}

	bool LabelIndex::hasLabel(int64_t entityId, const std::string &label) const {
		std::shared_lock lock(mutex_);

		if (rootId_ == 0) {
			return false;
		}

		auto nodes = treeManager_->find(rootId_, label);
		return std::ranges::find(nodes, entityId) != nodes.end();
	}

} // namespace graph::query::indexes
