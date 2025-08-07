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
#include <graph/storage/IDAllocator.hpp>
#include "graph/storage/data/DataManager.hpp"

namespace graph::query::indexes {

	LabelIndex::LabelIndex(std::shared_ptr<storage::DataManager> dataManager, uint32_t indexType, std::string stateKey) :
		dataManager_(std::move(dataManager)),
		treeManager_(std::make_shared<IndexTreeManager>(dataManager_, indexType)),
		stateKey_(std::move(stateKey)) {
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

		// Remove the rootId from state registry using the specific key
		StateRegistry::getDataManager()->removeState(stateKey_);
	}

	void LabelIndex::saveState() {
		std::shared_lock lock(mutex_);
		if (rootId_ != 0) {
			std::unordered_map<std::string, PropertyValue> properties;
			properties["rootId"] = rootId_;
			StateRegistry::getDataManager()->addStateProperties(stateKey_, properties);
		}
	}

	void LabelIndex::loadRootId() {
		auto properties = StateRegistry::getDataManager()->getStateProperties(stateKey_);
		if (properties.contains("rootId") && std::holds_alternative<int64_t>(properties.at("rootId"))) {
			rootId_ = std::get<int64_t>(properties.at("rootId"));
		} else {
			rootId_ = 0;
		}
	}

	void LabelIndex::flush() {
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
