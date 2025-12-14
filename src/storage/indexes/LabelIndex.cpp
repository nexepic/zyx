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

namespace graph::query::indexes {

	LabelIndex::LabelIndex(std::shared_ptr<storage::DataManager> dataManager, uint32_t indexType,
						   std::string stateKey) :
		dataManager_(std::move(dataManager)),
		treeManager_(std::make_shared<IndexTreeManager>(dataManager_, indexType, PropertyType::STRING)),
		stateKey_(std::move(stateKey)) {
		initialize();
	}

	void LabelIndex::initialize() {
		std::unique_lock lock(mutex_);
		loadRootId();

		// Load enabled state
		auto props = dataManager_->getStateProperties(STATE_ENABLED_KEY);
		if (props.contains("enabled")) {
			// Assuming PropertyValue has a boolean or int representation
			// Adjust based on your PropertyValue implementation
			if (const auto* val = std::get_if<int64_t>(&props["enabled"].getVariant())) {
				enabled_ = (*val != 0);
			}
		} else {
			// Fallback: if rootId > 0, it implies it was enabled before
			enabled_ = (rootId_ != 0);
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
		dataManager_->removeState(stateKey_);
		dataManager_->removeState(STATE_ENABLED_KEY);
	}

	void LabelIndex::saveState() const {
		std::shared_lock lock(mutex_);
		if (enabled_) {
			// Save root
			std::unordered_map<std::string, PropertyValue> props;
			props["rootId"] = rootId_;
			dataManager_->addStateProperties(stateKey_, props);

			// Save enabled flag
			std::unordered_map<std::string, PropertyValue> flagProps;
			flagProps["enabled"] = static_cast<int64_t>(1);
			dataManager_->addStateProperties(STATE_ENABLED_KEY, flagProps);
		}
	}

	void LabelIndex::loadRootId() {
		auto properties = dataManager_->getStateProperties(stateKey_);
		if (properties.contains("rootId")) {
			// Use the safe std::get_if pattern
			if (const auto *rootId_ptr = std::get_if<int64_t>(&properties["rootId"].getVariant())) {
				rootId_ = *rootId_ptr;
				return; // Exit after successful load
			}
		}
		// Default value if not found or type mismatch
		rootId_ = 0;
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
