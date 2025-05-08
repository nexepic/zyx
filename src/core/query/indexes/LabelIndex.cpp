/**
 * @file LabelIndex.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/3/21
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/core/query/indexes/LabelIndex.h"
#include <algorithm>
#include <mutex>

namespace graph::query::indexes {

	LabelIndex::LabelIndex() = default;

	void LabelIndex::addNode(int64_t nodeId, const std::string &label) {
		std::unique_lock lock(mutex_);

		// Add to label -> nodes mapping
		labelToNodes_[label].push_back(nodeId);

		// Add to node -> labels mapping
		nodeToLabels_[nodeId].push_back(label);
	}

	void LabelIndex::removeNode(int64_t nodeId, const std::string &label) {
		std::unique_lock lock(mutex_);

		// Remove from label -> nodes mapping
		auto labelIt = labelToNodes_.find(label);
		if (labelIt != labelToNodes_.end()) {
			auto &nodes = labelIt->second;
			nodes.erase(std::remove(nodes.begin(), nodes.end(), nodeId), nodes.end());

			// Remove empty vectors
			if (nodes.empty()) {
				labelToNodes_.erase(labelIt);
			}
		}

		// Remove from node -> labels mapping
		auto nodeIt = nodeToLabels_.find(nodeId);
		if (nodeIt != nodeToLabels_.end()) {
			auto &labels = nodeIt->second;
			labels.erase(std::remove(labels.begin(), labels.end(), label), labels.end());

			// Remove empty vectors
			if (labels.empty()) {
				nodeToLabels_.erase(nodeIt);
			}
		}
	}

	std::vector<int64_t> LabelIndex::findNodes(const std::string &label) const {
		std::shared_lock lock(mutex_);

		auto it = labelToNodes_.find(label);
		if (it == labelToNodes_.end()) {
			return {};
		}

		return it->second;
	}

	bool LabelIndex::hasLabel(int64_t nodeId, const std::string &label) const {
		std::shared_lock lock(mutex_);

		auto it = nodeToLabels_.find(nodeId);
		if (it == nodeToLabels_.end()) {
			return false;
		}

		const auto &labels = it->second;
		return std::find(labels.begin(), labels.end(), label) != labels.end();
	}

	void LabelIndex::clear() {
		std::unique_lock lock(mutex_);
		labelToNodes_.clear();
		nodeToLabels_.clear();
	}

} // namespace graph::query::indexes
