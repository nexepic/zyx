#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "graph/query/execution/NodeBatchLoader.hpp"
#include "graph/query/execution/NodeCandidateSource.hpp"
#include "graph/query/execution/PhysicalOperator.hpp"
#include "graph/query/execution/VectorizedPredicate.hpp"
#include "graph/storage/indexes/IndexManager.hpp"

namespace graph::query::execution::operators {

	class NodeDistinctCountScanOperator : public PhysicalOperator {
	public:
		NodeDistinctCountScanOperator(std::shared_ptr<storage::DataManager> dm,
										  std::shared_ptr<indexes::IndexManager> im, NodeScanConfig config,
										  NodeScanRequirements requirements,
										  std::vector<VectorizedPropertyPredicate> predicates,
										  std::string distinctProperty, std::string outputAlias,
										  std::vector<ExplainAttribute> explainAttributes = {});

		void open() override;
		std::optional<RecordBatch> next() override;
		void close() override;

		[[nodiscard]] std::vector<std::string> getOutputVariables() const override;
		[[nodiscard]] std::string toString() const override;
		[[nodiscard]] std::vector<ExplainAttribute> explainAttributes() const override { return explainAttributes_; }

	private:
		std::shared_ptr<storage::DataManager> dm_;
		std::shared_ptr<indexes::IndexManager> im_;
		NodeScanConfig config_;
		NodeScanRequirements requirements_;
		std::vector<VectorizedPropertyPredicate> predicates_;
		std::string distinctProperty_;
		std::string outputAlias_;
		std::vector<ExplainAttribute> explainAttributes_;
		NodeCandidateSet candidateSet_;
		bool emitted_ = false;
	};

} // namespace graph::query::execution::operators
