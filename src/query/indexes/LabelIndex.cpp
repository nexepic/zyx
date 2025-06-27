/**
 * @file LabelIndex.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/3/21
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/query/indexes/LabelIndex.hpp"
#include <algorithm>
#include <graph/core/StateRegistry.hpp>

#include "graph/storage/DataManager.hpp"

namespace graph::query::indexes {

	LabelIndex::LabelIndex(const std::shared_ptr<storage::DataManager> &dataManager) : dataManager_(dataManager),
		treeManager_(std::make_shared<IndexTreeManager>(dataManager, LABEL_INDEX_TYPE)) {
		initialize();
	}

	void LabelIndex::initialize() {
		std::unique_lock lock(mutex_);

		// Load the rootId from state
		loadRootId();
	}

	bool LabelIndex::isEmpty() const {
		std::shared_lock lock(mutex_);
		// The index is empty if no tree has been initialized (rootId_ is 0)
		return rootId_ == 0;
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
		// Clear the index and reset rootId
		clear();

		// Remove the rootId from state registry
		StateRegistry::getDataManager()->removeState(STATE_KEY_ROOT_ID);
	}

	void LabelIndex::saveState() {
		std::shared_lock lock(mutex_);

		// Save rootId to state registry
		if (rootId_ != 0) {
			if (rootId_ < 0) {
				rootId_ = dataManager_->getIdAllocator()->allocatePermanentId(rootId_, storage::Index::typeId, false);
			}

			// Create a property map with the single root ID
			std::unordered_map<std::string, PropertyValue> properties;
			properties["rootId"] = rootId_;

			// Store in state entity
			StateRegistry::getDataManager()->addStateProperties(STATE_KEY_ROOT_ID, properties);
		}
	}

	void LabelIndex::loadRootId() {
		// Load rootId from state registry, defaulting to 0 if not found
		auto properties = StateRegistry::getDataManager()->getStateProperties(STATE_KEY_ROOT_ID);

		// Look for the rootId property
		auto it = properties.find("rootId");
		if (it != properties.end() && std::holds_alternative<int64_t>(it->second)) {
			rootId_ = std::get<int64_t>(it->second);
		} else {
			rootId_ = 0;
		}
	}

	void LabelIndex::flush() {
		// std::unique_lock lock(mutex_);
		// Call the new saveState method instead of saveRootId
		saveState();
	}

	void LabelIndex::addNode(int64_t nodeId, const std::string &label) {
		std::unique_lock lock(mutex_);

		if (rootId_ == 0) {
			rootId_ = treeManager_->initialize();
		}

		// Use the tree manager to insert the label->nodeId mapping
		// The new root ID might change if tree restructuring occurs
		int64_t newRootId = treeManager_->insert(rootId_, label, nodeId);

		// If the root has changed, update the persisted value
		if (newRootId != rootId_) {
			rootId_ = newRootId;
		}
	}

	void LabelIndex::removeNode(int64_t nodeId, const std::string &label) {
		std::unique_lock lock(mutex_);

		if (rootId_ == 0) {
			return;
		}

		// The remove operation might modify the tree structure
		int64_t newRootId = treeManager_->remove(rootId_, label, nodeId);

		// If the root has changed, update the persisted value
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

	bool LabelIndex::hasLabel(int64_t nodeId, const std::string &label) const {
		std::shared_lock lock(mutex_);

		if (rootId_ == 0) {
			return false;
		}

		auto nodes = treeManager_->find(rootId_, label);
		return std::find(nodes.begin(), nodes.end(), nodeId) != nodes.end();
	}

} // namespace graph::query::indexes