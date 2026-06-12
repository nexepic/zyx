#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "graph/core/Edge.hpp"
#include "graph/core/Node.hpp"
#include "graph/query/execution/RelationshipExpandConfig.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/traversal/RelationshipTraversal.hpp"

namespace graph::query::execution {

	using RelationshipExpandVisitor = std::function<bool(const RelationshipExpandRow &)>;

	class RelationshipAdjacencyCursor {
	public:
		explicit RelationshipAdjacencyCursor(std::shared_ptr<storage::DataManager> dm);

		[[nodiscard]] RelationshipExpandBatch expand(const std::vector<int64_t> &sourceIds,
		                                             const RelationshipExpandConfig &config,
		                                             const RelationshipExpandRequirements &requirements) const;
		[[nodiscard]] int64_t count(const std::vector<int64_t> &sourceIds,
		                            const RelationshipExpandConfig &config,
		                            const RelationshipExpandRequirements &requirements) const;
		[[nodiscard]] size_t forEach(const std::vector<int64_t> &sourceIds,
		                             const RelationshipExpandConfig &config,
		                             const RelationshipExpandRequirements &requirements,
		                             const RelationshipExpandVisitor &visitor) const;

	private:
		[[nodiscard]] bool acceptsTarget(int64_t targetId,
		                                 const RelationshipExpandConfig &config,
		                                 const RelationshipExpandRequirements &requirements) const;
		[[nodiscard]] std::optional<int64_t> acceptedTargetForEdge(
				const Edge &edge,
				int64_t sourceId,
				const RelationshipExpandConfig &config,
				const RelationshipExpandRequirements &requirements) const;
		[[nodiscard]] std::optional<int64_t> acceptedTargetForEdgeRef(
				const traversal::RelationshipEdgeRef &edgeRef,
				int64_t sourceId,
				const RelationshipExpandConfig &config,
				const RelationshipExpandRequirements &requirements) const;
		[[nodiscard]] bool matchesTargetLabels(const Node &node, const RelationshipExpandConfig &config) const;
		[[nodiscard]] int64_t targetForSource(const Edge &edge, int64_t sourceId, const std::string &direction) const;
		[[nodiscard]] int64_t targetForSource(const traversal::RelationshipEdgeRef &edgeRef,
		                                      int64_t sourceId,
		                                      const std::string &direction) const;
		[[nodiscard]] traversal::RelationshipTraversalOptions traversalOptions(
				const RelationshipExpandConfig &config,
				const RelationshipExpandRequirements &requirements) const;

		std::shared_ptr<storage::DataManager> dm_;
	};

} // namespace graph::query::execution
