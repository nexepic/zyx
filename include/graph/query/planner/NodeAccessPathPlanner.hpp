#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "graph/core/Property.hpp"
#include "graph/query/execution/NodeScanRequirements.hpp"
#include "graph/query/execution/ScanConfigs.hpp"
#include "graph/query/execution/VectorizedPredicate.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/planner/AccessPathSummary.hpp"

namespace graph::query::indexes {
class IndexManager;
}

namespace graph::query::planner {

struct NodeAccessPathOptions {
	bool allowOpenRangeIndex = false;
};

enum class NodeAccessPathKind {
	NAP_FULL_SCAN,
	NAP_LABEL_SCAN,
	NAP_PROPERTY_INDEX,
	NAP_RANGE_INDEX,
	NAP_COMPOSITE_INDEX
};

struct NodeAccessPathEstimate {
	std::optional<int64_t> cardinality;
	double cost = 0.0;
	bool exactCardinality = false;
	std::string source;
};

struct NodeAccessPathCandidate {
	execution::NodeScanConfig config;
	NodeAccessPathKind kind = NodeAccessPathKind::NAP_FULL_SCAN;
	NodeAccessPathEstimate estimate;
	std::string reason;
	bool preferred = false;
	bool valid = true;
	bool directCandidateLookup = false;
	bool openRange = false;
};

struct NodeAccessPathDecision {
	NodeAccessPathCandidate selected;
	std::vector<NodeAccessPathCandidate> candidates;

	[[nodiscard]] const execution::NodeScanConfig &config() const { return selected.config; }
	[[nodiscard]] bool selectedSupportsDirectCandidateLookup() const {
		return selected.directCandidateLookup && selected.valid;
	}
	[[nodiscard]] bool selectedRequiresConservativeFallback() const {
		return !selected.valid || selected.openRange;
	}
	[[nodiscard]] std::optional<int64_t> selectedEstimatedCardinality() const {
		return selected.estimate.cardinality;
	}
	[[nodiscard]] double selectedEstimatedCost() const {
		return selected.estimate.cost;
	}
};

[[nodiscard]] const char *nodeAccessPathKindName(NodeAccessPathKind kind);

[[nodiscard]] AccessPathSummary summarizeNodeAccessPath(const NodeAccessPathCandidate &candidate);

[[nodiscard]] bool isNodeVariableReference(
		const std::shared_ptr<expressions::Expression> &expression,
		const std::string &variable);

[[nodiscard]] const expressions::VariableReferenceExpression *asNodePropertyAccess(
		const std::shared_ptr<expressions::Expression> &expression,
		const std::string &variable);

void addRequiredNodeProperty(execution::NodeScanRequirements &requirements,
                             const std::string &key);

[[nodiscard]] bool hasBoundValue(const PropertyValue &value);

[[nodiscard]] bool hasOpenRangeBounds(const execution::NodeScanConfig &config);

[[nodiscard]] bool isIndexCandidateSource(execution::ScanType type);

[[nodiscard]] bool hasValidNodeCandidateConfig(const execution::NodeScanConfig &config);

void fallbackToLabelOrFullScan(execution::NodeScanConfig &config);

[[nodiscard]] execution::NodeScanConfig chooseNodeAccessPathConfig(
		const logical::LogicalNodeScan &scan,
		const std::shared_ptr<indexes::IndexManager> &indexManager,
		NodeAccessPathOptions options = {});

[[nodiscard]] NodeAccessPathDecision chooseNodeAccessPathDecision(
		const logical::LogicalNodeScan &scan,
		const std::shared_ptr<indexes::IndexManager> &indexManager,
		NodeAccessPathOptions options = {});

[[nodiscard]] bool appendResidualNodePredicates(
		const logical::LogicalNodeScan &scan,
		const execution::NodeScanConfig &config,
		execution::NodeScanRequirements &requirements,
		std::vector<execution::VectorizedPropertyPredicate> &predicates);

} // namespace graph::query::planner
