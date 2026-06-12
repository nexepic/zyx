#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "graph/core/PropertyTypes.hpp"
#include "graph/core/Types.hpp"
#include "graph/query/execution/NodeBatchLoader.hpp"
#include "graph/query/execution/NodeCandidateSource.hpp"
#include "graph/query/execution/PhysicalOperator.hpp"
#include "graph/query/execution/TypedValueKey.hpp"
#include "graph/query/execution/VectorizedPredicate.hpp"
#include "graph/storage/indexes/IndexManager.hpp"

namespace graph::query::execution::operators {

	struct NodeTopKProjection {
		std::string property;
		std::string alias;
	};

	class NodeTopKScanOperator : public PhysicalOperator {
	public:
		NodeTopKScanOperator(std::shared_ptr<storage::DataManager> dm, std::shared_ptr<indexes::IndexManager> im,
								 NodeScanConfig config, NodeScanRequirements requirements,
								 std::vector<VectorizedPropertyPredicate> predicates,
								 std::vector<NodeTopKProjection> projections, std::string sortProperty, bool ascending,
								 int64_t limit, std::vector<ExplainAttribute> explainAttributes = {});

		void open() override;
		std::optional<RecordBatch> next() override;
		void close() override;

		[[nodiscard]] std::vector<std::string> getOutputVariables() const override;
		[[nodiscard]] std::string toString() const override;
		[[nodiscard]] std::vector<ExplainAttribute> explainAttributes() const override { return explainAttributes_; }

	private:
		struct Row {
			int64_t nodeId = 0;
			int64_t propertyEntityId = 0;
			PropertyStorageType propertyStorageType = PropertyStorageType::NONE;
			bool metadataLoaded = false;
			PropertyValue sortKey;
			TypedOrderKey orderKey;
			std::vector<PropertyValue> values;
		};

		std::shared_ptr<storage::DataManager> dm_;
		std::shared_ptr<indexes::IndexManager> im_;
		NodeScanConfig config_;
		NodeScanRequirements requirements_;
		std::vector<VectorizedPropertyPredicate> predicates_;
		std::vector<NodeTopKProjection> projections_;
		std::string sortProperty_;
		bool ascending_ = true;
		size_t limit_ = 0;
		std::vector<ExplainAttribute> explainAttributes_;
		NodeCandidateSet candidateSet_;
		std::vector<Record> outputRows_;
		size_t currentOutputIndex_ = 0;
		bool built_ = false;

		void buildTopK();
		[[nodiscard]] bool tryBuildTopKFromMetadata(std::vector<Row> &heap) const;
		[[nodiscard]] NodeScanRequirements makeSelectionRequirements() const;
		void offerTopK(std::vector<Row> &heap, Row candidate) const;
		void loadProjectionValues(std::vector<Row> &rows) const;
		[[nodiscard]] bool comesBefore(const PropertyValue &left, const PropertyValue &right) const;
		[[nodiscard]] bool comesBefore(const TypedOrderKey &left, const TypedOrderKey &right) const;
		[[nodiscard]] Record makeRecord(const Row &row) const;
		[[nodiscard]] static size_t normalizeLimit(int64_t limit);
	};

} // namespace graph::query::execution::operators
