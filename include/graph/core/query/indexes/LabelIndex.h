/**
 * @file LabelIndex.h
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/3/20
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <shared_mutex>

namespace graph::query::indexes {

	class LabelIndex {
	public:
		LabelIndex();

		// Add a node to the label index
		void addNode(uint64_t nodeId, const std::string& label);

		// Remove a node from the label index
		void removeNode(uint64_t nodeId, const std::string& label);

		// Find nodes with a specific label
		[[nodiscard]] std::vector<uint64_t> findNodes(const std::string& label) const;

		// Check if a node has a specific label
		[[nodiscard]] bool hasLabel(uint64_t nodeId, const std::string& label) const;

		// Clear the index
		void clear();

	private:
		// Maps label -> set of node IDs
		std::unordered_map<std::string, std::vector<uint64_t>> labelToNodes_;

		// Maps node ID -> set of labels
		std::unordered_map<uint64_t, std::vector<std::string>> nodeToLabels_;

		// Thread safety
		mutable std::shared_mutex mutex_;
	};

} // namespace graph::query::indexes