#pragma once

#include <memory>
#include <optional>
#include <array>
#include <functional>
#include <vector>

#include "graph/concurrent/ThreadPool.hpp"
#include "graph/core/Node.hpp"
#include "graph/core/PropertyTypes.hpp"
#include "graph/query/execution/NodeScanRequirements.hpp"
#include "graph/query/execution/ScanConfigs.hpp"
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

	struct NodeMetadataProjection {
		bool loadEdgeRefs = true;
		bool loadLabels = true;
	};

	struct NodePropertyCandidateRef {
		int64_t nodeId = 0;
		int64_t propertyEntityId = 0;
		PropertyStorageType propertyStorageType = PropertyStorageType::NONE;

		[[nodiscard]] Node toNode() const;
	};

	struct NodePropertyCountCandidates {
		std::vector<int64_t> propertyEntityIds;
		std::vector<int64_t> propertyNodeIds;
		std::vector<size_t> propertyRows;
		std::vector<NodePropertyCandidateRef> blobRefs;
		size_t acceptedRowCount = 0;

		[[nodiscard]] size_t propertyRowCount() const { return propertyEntityIds.size(); }
		void reserve(size_t rowCount);
	};

	struct NodePropertyCountCandidateOptions {
		bool collectFallbackRefs = true;
	};

	class NodeMetadataColumnLoader {
	public:
		using MetadataVisitor = std::function<bool(size_t row, const NodeMetadataRow &metadata)>;
		using MetadataPartitionInitializer = std::function<void(size_t partitionCount)>;
		using MetadataPartitionVisitor =
				std::function<bool(size_t partition, size_t row, const NodeMetadataRow &metadata)>;

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
		                              const MetadataVisitor &visitor,
		                              NodeMetadataProjection projection = {}) const;
		[[nodiscard]] bool visitBatchPartitioned(const std::vector<int64_t> &candidateIds,
		                                         size_t begin,
		                                         size_t end,
		                                         const MetadataPartitionInitializer &initializer,
		                                         const MetadataPartitionVisitor &visitor,
		                                         concurrent::ThreadPool *threadPool,
		                                         NodeMetadataProjection projection = {}) const;

		[[nodiscard]] std::optional<NodePropertyCountCandidates>
		collectPropertyCountCandidates(const std::vector<int64_t> &candidateIds,
		                               size_t begin,
		                               size_t end,
		                               const NodeScanConfig &config,
		                               const NodeScanRequirements &requirements,
		                               NodePropertyCountCandidateOptions options = {}) const;
		[[nodiscard]] std::optional<NodePropertyCountCandidates>
		collectPropertyCountCandidates(const std::vector<int64_t> &candidateIds,
		                               size_t begin,
		                               size_t end,
		                               const NodeScanConfig &config,
		                               const NodeScanRequirements &requirements,
		                               concurrent::ThreadPool *threadPool,
		                               NodePropertyCountCandidateOptions options = {}) const;
		[[nodiscard]] std::optional<NodePropertyCountCandidates>
		collectFullScanPropertyCountCandidates(const NodeScanConfig &config,
		                                       const NodeScanRequirements &requirements,
		                                       NodePropertyCountCandidateOptions options = {}) const;
		[[nodiscard]] std::optional<NodePropertyCountCandidates>
		collectFullScanPropertyCountCandidates(const NodeScanConfig &config,
		                                       const NodeScanRequirements &requirements,
		                                       concurrent::ThreadPool *threadPool,
		                                       NodePropertyCountCandidateOptions options = {}) const;

	private:
		[[nodiscard]] bool canLoad(const std::vector<int64_t> &candidateIds, size_t begin, size_t end) const;
		[[nodiscard]] bool visitBatchChecked(const std::vector<int64_t> &candidateIds,
		                                     size_t begin,
		                                     size_t end,
		                                     const MetadataVisitor &visitor,
		                                     NodeMetadataProjection projection) const;

		std::shared_ptr<storage::DataManager> dm_;
	};

} // namespace graph::query::execution
