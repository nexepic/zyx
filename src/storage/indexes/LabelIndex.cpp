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
#include "graph/storage/IDAllocator.hpp"
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
		enabled_ = systemStateManager_->get<bool>(configKey, storage::state::keys::Fields::ENABLED, true);
	}

	void LabelIndex::createIndex() {
		std::unique_lock lock(mutex_);
		enabled_ = true;
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

		// Explicitly set ENABLED = false instead of removing the key.
		// This overrides the "Default True" behavior on next restart.
		std::string configKey = stateKey_ + storage::state::keys::SUFFIX_CONFIG;
		systemStateManager_->set<bool>(configKey, storage::state::keys::Fields::ENABLED, false);

		// We can remove the Root ID part since data is cleared
		systemStateManager_->remove(stateKey_);
	}

	void LabelIndex::saveState() const {
		std::shared_lock lock(mutex_);

		// Save Root ID (if data exists)
		if (rootId_ != 0) {
			systemStateManager_->set<int64_t>(stateKey_, storage::state::keys::Fields::ROOT_ID, rootId_);
		}

		// Save Enabled Config
		// We always save the config to make the state explicit
		std::string configKey = stateKey_ + storage::state::keys::SUFFIX_CONFIG;
		systemStateManager_->set<bool>(configKey, storage::state::keys::Fields::ENABLED, enabled_);
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

	void LabelIndex::addNodesBatch(const std::unordered_map<std::string, std::vector<int64_t>> &nodesByLabel) {
		// Acquire lock ONCE for the entire batch operation
		std::unique_lock lock(mutex_);

		if (rootId_ == 0) {
			rootId_ = treeManager_->initialize();
		}

		// Iterate over each label group
		for (const auto &[label, entityIds] : nodesByLabel) {
			if (entityIds.empty()) continue;

			// Prepare data for IndexTreeManager
			// Key: Label (String), Value: Entity ID
			std::vector<std::pair<PropertyValue, int64_t>> batchEntries;
			batchEntries.reserve(entityIds.size());

			for (int64_t id : entityIds) {
				batchEntries.emplace_back(PropertyValue(label), id);
			}

			// Perform batch insertion into the B+ Tree
			// NOTE: Since LabelIndex uses a SINGLE tree where Key=Label,
			// all these entries share the same 'label' key but have different IDs.
			// The IndexTreeManager handles multi-value keys (duplicate keys) correctly.

			int64_t newRootId = treeManager_->insertBatch(rootId_, batchEntries);

			if (newRootId != rootId_) {
				rootId_ = newRootId;
			}
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
