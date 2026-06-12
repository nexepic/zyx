#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "graph/query/execution/RelationshipExpandConfig.hpp"
#include "graph/query/planner/AccessPathSummary.hpp"

namespace graph::query::indexes {
class IndexManager;
}

namespace graph::query::planner {

enum class RelationshipAccessPathKind {
	RAK_COLUMNAR_SCAN,
	RAK_TYPE_INDEX,
	RAK_PROPERTY_INDEX,
	RAK_TYPE_PROPERTY_INTERSECTION
};

struct RelationshipAccessPathEstimate {
	std::optional<int64_t> cardinality;
	double cost = 0.0;
	bool exactCardinality = false;
	std::string source;
};

struct RelationshipAccessPathCandidate {
	execution::DirectRelationshipCountConfig config;
	RelationshipAccessPathKind kind = RelationshipAccessPathKind::RAK_COLUMNAR_SCAN;
	RelationshipAccessPathEstimate estimate;
	std::string reason;
	std::vector<std::string> propertyKeysSatisfied;
	bool valid = true;
	bool directCandidateLookup = false;
	bool typeSatisfied = false;
};

struct RelationshipAccessPathDecision {
	RelationshipAccessPathCandidate selected;
	std::vector<RelationshipAccessPathCandidate> candidates;

	[[nodiscard]] bool selectedSupportsDirectCandidateLookup() const {
		return selected.valid && selected.directCandidateLookup;
	}
	[[nodiscard]] std::optional<int64_t> selectedEstimatedCardinality() const {
		return selected.estimate.cardinality;
	}
	[[nodiscard]] double selectedEstimatedCost() const {
		return selected.estimate.cost;
	}
};

[[nodiscard]] const char *relationshipAccessPathKindName(RelationshipAccessPathKind kind);

[[nodiscard]] AccessPathSummary summarizeRelationshipAccessPath(
		const RelationshipAccessPathCandidate &candidate);

[[nodiscard]] execution::DirectRelationshipCandidateSourceConfig relationshipCandidateSourceForAccessPath(
		const RelationshipAccessPathCandidate &candidate);

[[nodiscard]] RelationshipAccessPathDecision chooseRelationshipAccessPathDecision(
		const execution::DirectRelationshipCountConfig &config,
		const std::shared_ptr<indexes::IndexManager> &indexManager);

} // namespace graph::query::planner
