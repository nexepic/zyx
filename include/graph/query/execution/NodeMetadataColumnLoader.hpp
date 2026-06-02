#pragma once

#include <memory>
#include <optional>
#include <array>
#include <functional>
#include <vector>

#include "graph/core/Node.hpp"
#include "graph/core/PropertyTypes.hpp"
#include "graph/storage/data/DataManager.hpp"

namespace graph::query::execution {

	struct NodeMetadataRow {
		int64_t nodeId = 0;
		int64_t firstOutEdgeId = 0;
		int64_t firstInEdgeId = 0;
		uint8_t active = 0;
		uint8_t labelCount = 0;
		std::array<int64_t, Node::MAX_LABELS> labelIds{};
		int64_t propertyEntityId = 0;
		PropertyStorageType propertyStorageType = PropertyStorageType::NONE;

		[[nodiscard]] bool isValid() const {
			return nodeId != 0;
		}

		[[nodiscard]] bool hasLabelId(int64_t labelId) const;
		[[nodiscard]] Node toNode() const;
	};

	struct NodeMetadataBatch {
		std::vector<int64_t> nodeIds;
		std::vector<int64_t> firstOutEdgeIds;
		std::vector<int64_t> firstInEdgeIds;
		std::vector<uint8_t> active;
		std::vector<uint8_t> labelCounts;
		std::vector<std::array<int64_t, Node::MAX_LABELS>> labelIds;
		std::vector<int64_t> propertyEntityIds;
		std::vector<PropertyStorageType> propertyStorageTypes;

		[[nodiscard]] size_t size() const {
			return nodeIds.size();
		}

		void reserve(size_t rowCount);
		void appendDefault();
		void setFromNode(size_t row, const Node &node);
		void setFromMetadataRow(size_t row, const NodeMetadataRow &metadata);

		[[nodiscard]] bool isValid(size_t row) const {
			return row < nodeIds.size() && nodeIds[row] != 0;
		}

		[[nodiscard]] bool hasLabelId(size_t row, int64_t labelId) const;
		[[nodiscard]] Node toNode(size_t row) const;
	};

	class NodeMetadataColumnLoader {
	public:
		using MetadataVisitor = std::function<bool(size_t row, const NodeMetadataRow &metadata)>;

		explicit NodeMetadataColumnLoader(std::shared_ptr<storage::DataManager> dm);

		[[nodiscard]] std::optional<NodeMetadataBatch> loadBatch(const std::vector<int64_t> &candidateIds,
		                                                         size_t begin,
		                                                         size_t end) const;

		[[nodiscard]] std::optional<std::vector<Node>> load(const std::vector<int64_t> &candidateIds,
		                                                    size_t begin,
		                                                    size_t end) const;

		[[nodiscard]] bool visitBatch(const std::vector<int64_t> &candidateIds,
		                              size_t begin,
		                              size_t end,
		                              const MetadataVisitor &visitor) const;

	private:
		[[nodiscard]] bool canLoad(const std::vector<int64_t> &candidateIds, size_t begin, size_t end) const;
		[[nodiscard]] bool visitBatchChecked(const std::vector<int64_t> &candidateIds,
		                                     size_t begin,
		                                     size_t end,
		                                     const MetadataVisitor &visitor) const;

		std::shared_ptr<storage::DataManager> dm_;
	};

} // namespace graph::query::execution
