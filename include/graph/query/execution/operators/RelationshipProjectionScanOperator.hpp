#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "graph/core/PropertyTypes.hpp"
#include "graph/query/execution/PhysicalOperator.hpp"
#include "graph/query/execution/RelationshipExpandConfig.hpp"
#include "graph/query/execution/RelationshipMetadataColumnLoader.hpp"
#include "graph/query/execution/VectorizedPredicate.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/indexes/IndexManager.hpp"

namespace graph::query::execution::operators {

enum class RelationshipProjectionSource {
	RPS_EDGE,
	RPS_TARGET_NODE
};

struct RelationshipProjectionScanItem {
	RelationshipProjectionSource source = RelationshipProjectionSource::RPS_EDGE;
	std::string property;
	std::string alias;
};

class RelationshipProjectionScanOperator : public PhysicalOperator {
public:
	RelationshipProjectionScanOperator(std::shared_ptr<storage::DataManager> dm,
	                                   std::shared_ptr<indexes::IndexManager> im,
	                                   DirectRelationshipCountConfig config,
	                                   std::string targetVariable,
	                                   std::vector<std::string> targetLabels,
	                                   std::vector<RelationshipProjectionScanItem> projections,
	                                   std::optional<size_t> limit = std::nullopt,
	                                   std::vector<ExplainAttribute> explainAttributes = {});

	void open() override;
	std::optional<RecordBatch> next() override;
	void close() override;
	void setOutputLimitHint(size_t limit) override;

	[[nodiscard]] std::vector<std::string> getOutputVariables() const override;
	[[nodiscard]] std::string toString() const override;
	[[nodiscard]] std::vector<ExplainAttribute> explainAttributes() const override { return explainAttributes_; }

private:
	std::shared_ptr<storage::DataManager> dm_;
	std::shared_ptr<indexes::IndexManager> im_;
	DirectRelationshipCountConfig config_;
	std::string targetVariable_;
	std::vector<std::string> targetLabels_;
	std::vector<RelationshipProjectionScanItem> projections_;
	std::optional<size_t> limit_;
	std::vector<ExplainAttribute> explainAttributes_;

	std::vector<int64_t> candidateIds_;
	bool hasCandidateIds_ = false;
	size_t candidateIndex_ = 0;
	int64_t nextEdgeId_ = 1;
	int64_t maxEdgeId_ = 0;
	int64_t edgeTypeId_ = 0;
	std::vector<int64_t> targetLabelIds_;
	size_t emittedRows_ = 0;

	[[nodiscard]] size_t remainingLimit() const;
	[[nodiscard]] size_t nextCandidateBatchSize() const;
	[[nodiscard]] size_t nextRangeBatchSize() const;
	[[nodiscard]] std::vector<std::string> requiredRelationshipProperties() const;
	[[nodiscard]] bool acceptsTargetNode(int64_t nodeId) const;
	[[nodiscard]] int64_t targetNodeId(const RelationshipMetadataBatch &metadata, size_t row) const;
	[[nodiscard]] std::optional<RelationshipMetadataBatch> loadNextMetadataBatch();
	[[nodiscard]] RelationshipMetadataBatch loadCandidateMetadata(size_t begin, size_t end) const;
	[[nodiscard]] RelationshipMetadataBatch loadRangeMetadata(int64_t beginId, int64_t endId) const;
	[[nodiscard]] Record makeRecord(
			const RelationshipMetadataBatch &metadata,
			size_t row,
			const std::unordered_map<std::string, std::vector<std::optional<PropertyValue>>> &relationshipColumns,
			const std::unordered_map<std::string, PropertyValue> &targetProperties) const;
};

} // namespace graph::query::execution::operators
