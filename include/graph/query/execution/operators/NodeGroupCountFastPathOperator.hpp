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

	class NodeGroupCountFastPathOperator : public PhysicalOperator {
	public:
		NodeGroupCountFastPathOperator(std::shared_ptr<storage::DataManager> dm,
		                               std::shared_ptr<indexes::IndexManager> im,
		                               NodeScanConfig config,
		                               NodeScanRequirements requirements,
		                               std::vector<VectorizedPropertyPredicate> predicates,
		                               std::string groupProperty,
		                               std::string groupAlias,
		                               std::string outputAlias);

		void open() override;
		std::optional<RecordBatch> next() override;
		void close() override;

		[[nodiscard]] std::vector<std::string> getOutputVariables() const override;
		[[nodiscard]] std::string toString() const override;

	private:
		std::shared_ptr<storage::DataManager> dm_;
		std::shared_ptr<indexes::IndexManager> im_;
		NodeScanConfig config_;
		NodeScanRequirements requirements_;
		std::vector<VectorizedPropertyPredicate> predicates_;
		std::string groupProperty_;
		std::string groupAlias_;
		std::string outputAlias_;
		NodeCandidateSet candidateSet_;
		bool emitted_ = false;
	};

} // namespace graph::query::execution::operators
