#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "graph/query/execution/NodeBatchLoader.hpp"
#include "graph/query/execution/NodeCandidateSource.hpp"
#include "graph/query/execution/PhysicalOperator.hpp"
#include "graph/query/execution/RelationshipAdjacencyCursor.hpp"
#include "graph/query/execution/RelationshipPropertyColumnLoader.hpp"
#include "graph/query/execution/VectorizedPredicate.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/indexes/IndexManager.hpp"

namespace graph::query::execution::operators {

	class RelationshipCountFastPathOperator : public PhysicalOperator {
	public:
		RelationshipCountFastPathOperator(std::shared_ptr<storage::DataManager> dm,
		                                  std::shared_ptr<indexes::IndexManager> im,
		                                  NodeScanConfig seedConfig,
		                                  NodeScanRequirements seedRequirements,
		                                  std::vector<RelationshipExpandConfig> hops,
		                                  std::string outputAlias);
		RelationshipCountFastPathOperator(std::shared_ptr<storage::DataManager> dm,
		                                  std::shared_ptr<indexes::IndexManager> im,
		                                  NodeScanConfig seedConfig,
		                                  NodeScanRequirements seedRequirements,
		                                  std::vector<VectorizedPropertyPredicate> seedPredicates,
		                                  std::vector<RelationshipExpandConfig> hops,
		                                  DirectRelationshipCountConfig directCount,
		                                  std::string outputAlias);
		RelationshipCountFastPathOperator(std::shared_ptr<storage::DataManager> dm,
		                                  std::shared_ptr<indexes::IndexManager> im,
		                                  NodeScanConfig seedConfig,
		                                  NodeScanRequirements seedRequirements,
		                                  std::vector<VectorizedPropertyPredicate> seedPredicates,
		                                  std::vector<RelationshipExpandConfig> hops,
		                                  std::string outputAlias);

		void open() override;
		std::optional<RecordBatch> next() override;
		void close() override;

		[[nodiscard]] std::vector<std::string> getOutputVariables() const override { return {outputAlias_}; }
		[[nodiscard]] std::string toString() const override;

	private:
		[[nodiscard]] std::vector<int64_t> collectSeedIds() const;
		[[nodiscard]] int64_t countExpandedPaths(const std::vector<int64_t> &seedIds) const;
		[[nodiscard]] int64_t countDirectRelationships() const;
		[[nodiscard]] std::optional<int64_t> countDirectRelationshipsFromIndexes(int64_t edgeTypeId) const;
		[[nodiscard]] bool edgeMatchesPropertyColumns(
				const std::unordered_map<std::string, std::vector<std::optional<PropertyValue>>> &columns,
				size_t row) const;

		std::shared_ptr<storage::DataManager> dm_;
		std::shared_ptr<indexes::IndexManager> im_;
		NodeScanConfig seedConfig_;
		NodeScanRequirements seedRequirements_;
		std::vector<VectorizedPropertyPredicate> seedPredicates_;
		std::vector<RelationshipExpandConfig> hops_;
		DirectRelationshipCountConfig directCount_;
		std::string outputAlias_;
		bool emitted_ = false;
	};

} // namespace graph::query::execution::operators
