#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "graph/core/PropertyTypes.hpp"
#include "graph/query/execution/VectorizedPredicate.hpp"

namespace graph::query::execution {
	enum class RelationshipMaterializationMode {
		RMM_ID_ONLY,
		RMM_EDGE_AND_TARGET,
		RMM_FULL_RECORD
	};

	struct RelationshipExpandConfig {
		std::string sourceVar;
		std::string edgeVar;
		std::string targetVar;
		std::string edgeType;
		std::string direction = "out";
		std::vector<std::string> targetLabels;
		int64_t edgeTypeId = 0;
		std::vector<int64_t> targetLabelIds;
	};

	struct DirectRelationshipCountConfig {
		bool enabled = false;
		std::string edgeType;
		std::string direction = "out";
		std::unordered_map<std::string, PropertyValue> edgeProperties;
		std::vector<VectorizedPropertyPredicate> edgePredicates;
	};

	struct RelationshipExpandRequirements {
		RelationshipMaterializationMode materialization = RelationshipMaterializationMode::RMM_ID_ONLY;
		bool countOnly = true;
		bool needsEdgeActiveCheck = true;
		bool needsTargetActiveCheck = true;
		bool needsTargetLabels = true;
	};

	struct RelationshipExpandRow {
		int64_t sourceId = 0;
		int64_t edgeId = 0;
		int64_t targetId = 0;
	};

	struct RelationshipExpandBatch {
		std::vector<RelationshipExpandRow> rows;
		std::vector<uint8_t> selected;

		[[nodiscard]] size_t size() const { return rows.size(); }

		[[nodiscard]] size_t selectedCount() const {
			if (selected.empty()) {
				return rows.size();
			}
			size_t count = 0;
			for (const uint8_t value : selected) {
				if (value != 0) {
					++count;
				}
			}
			return count;
		}
	};

} // namespace graph::query::execution
