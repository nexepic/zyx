/**
 * @file NodeScanOperator.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/10
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/query/execution/operators/NodeScanOperator.hpp"
#include <algorithm>

namespace graph::query::execution::operators {

	NodeScanOperator::NodeScanOperator(std::shared_ptr<storage::DataManager> dataManager,
									   std::shared_ptr<indexes::IndexManager> indexManager, std::string variableName,
									   std::string label, std::string key, PropertyValue value) :
		dataManager_(std::move(dataManager)), indexManager_(std::move(indexManager)),
		variableName_(std::move(variableName)), label_(std::move(label)), key_(std::move(key)),
		value_(std::move(value)) {}

	void NodeScanOperator::open() {
		currentIndex_ = 0;
		candidateIds_.clear();
		useIndex_ = false;

		// --- Strategy 1: Property Index (Most Specific) ---
		// If a key is provided and an index exists for it, this is usually the fastest path.
		if (!key_.empty() && indexManager_->hasPropertyIndex("node", key_)) {
			candidateIds_ = indexManager_->findNodeIdsByProperty(key_, value_);
			useIndex_ = true;
			return;
		}

		// --- Strategy 2: Label Index ---
		if (!label_.empty() && indexManager_->hasLabelIndex("node")) {
			candidateIds_ = indexManager_->findNodeIdsByLabel(label_);
			useIndex_ = true;
			return;
		}

		// --- Strategy 3: Full Scan (Fallback) ---
		prepareFullScan();
	}

	void NodeScanOperator::prepareFullScan() {
		useIndex_ = false;
		// Fetch max ID to determine range
		int64_t maxId = dataManager_->getIdAllocator()->getCurrentMaxNodeId();

		// Pre-allocate for performance (though in real DB we would use an iterator)
		candidateIds_.reserve(maxId);
		for (int64_t i = 1; i <= maxId; ++i) {
			candidateIds_.push_back(i);
		}
	}

	std::optional<RecordBatch> NodeScanOperator::next() {
		if (currentIndex_ >= candidateIds_.size()) {
			return std::nullopt;
		}

		RecordBatch batch;
		batch.reserve(BATCH_SIZE);

		while (batch.size() < BATCH_SIZE && currentIndex_ < candidateIds_.size()) {
			int64_t id = candidateIds_[currentIndex_++];

			// 1. Load Header (ID, Label, pointers) - Cheap
			Node node = dataManager_->getNode(id);

			if (!node.isActive())
				continue;

			// 2. Check Label
			if (!label_.empty() && node.getLabel() != label_) {
				continue;
			}

			// 3. Hydrate Properties (Load from disk) - Expensive
			// We must do this before checking properties or returning the node.
			auto props = dataManager_->getNodeProperties(id);

			// Fill the node object with the loaded properties
			node.setProperties(std::move(props));

			// 4. Check Property (Pushdown check)
			if (!key_.empty()) {
				// Now node.getProperties() is populated, so this works
				const auto &nodeProps = node.getProperties();
				auto it = nodeProps.find(key_);

				if (it == nodeProps.end() || it->second != value_) {
					continue;
				}
			}

			// 5. Add to Batch
			Record record;
			record.setNode(variableName_, node);
			batch.push_back(std::move(record));
		}

		if (batch.empty() && currentIndex_ >= candidateIds_.size()) {
			return std::nullopt;
		}

		return batch;
	}

	void NodeScanOperator::close() { candidateIds_.clear(); }

	std::vector<std::string> NodeScanOperator::getOutputVariables() const { return {variableName_}; }

} // namespace graph::query::execution::operators
