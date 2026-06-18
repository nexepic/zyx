#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "graph/core/PropertyTypes.hpp"
#include "graph/query/execution/NodeBatchLoader.hpp"
#include "graph/query/execution/NodeCandidateSource.hpp"
#include "graph/query/execution/NodeScanRequirements.hpp"
#include "graph/query/execution/PhysicalOperator.hpp"
#include "graph/query/execution/ScanConfigs.hpp"
#include "graph/query/execution/VectorizedPredicate.hpp"
#include "graph/storage/indexes/IndexManager.hpp"

namespace graph::query::execution::operators {

struct NodeProjectionScanItem {
	std::string property;
	std::string alias;
};

class NodeProjectionScanOperator : public PhysicalOperator {
public:
	NodeProjectionScanOperator(std::shared_ptr<storage::DataManager> dm,
	                           std::shared_ptr<indexes::IndexManager> im,
	                           NodeScanConfig config,
	                           NodeScanRequirements requirements,
	                           std::vector<VectorizedPropertyPredicate> predicates,
	                           std::vector<NodeProjectionScanItem> projections,
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
	NodeScanConfig config_;
	NodeScanRequirements requirements_;
	std::vector<VectorizedPropertyPredicate> predicates_;
	std::vector<NodeProjectionScanItem> projections_;
	std::optional<size_t> limit_;
	std::vector<ExplainAttribute> explainAttributes_;

	NodeCandidateSet candidateSet_;
	size_t currentIdx_ = 0;
	size_t emittedRows_ = 0;

	[[nodiscard]] size_t remainingLimit() const;
	[[nodiscard]] size_t nextBatchSize() const;
	[[nodiscard]] Record makeRecord(const NodeColumnBatch &batch, size_t row) const;
};

} // namespace graph::query::execution::operators
