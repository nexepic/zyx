#pragma once
#include <type_traits>
#include "graph/query/execution/GuardedPropertyCopy.hpp"
#include "graph/storage/LogicalGraphRead.hpp"

namespace graph::query::execution::detail {
template<typename Entity, typename Checkpoint>
Entity copyLogicalEntityGuarded(const Entity &source, Checkpoint checkpoint) {
	static_assert(std::is_same_v<Entity, storage::LogicalGraphNode> ||
		std::is_same_v<Entity, storage::LogicalGraphEdge>);
	checkpoint();
	Entity result;
	if constexpr (std::is_same_v<Entity, storage::LogicalGraphNode>) {
		result.node_id = source.node_id;
		result.labels = copyPropertyStringsGuarded(source.labels, checkpoint);
	} else {
		result.edge_id = source.edge_id;
		result.source_node_id = source.source_node_id;
		result.target_node_id = source.target_node_id;
		result.type = copyPropertyStringGuarded(source.type, checkpoint);
	}
	result.properties.reserve(source.properties.size());
	for (const auto &entry : source.properties) {
		checkpoint();
		result.properties.push_back({copyPropertyStringGuarded(entry.key, checkpoint),
			copyPropertyValueGuarded(entry.value, checkpoint)});
	}
	checkpoint();
	return result;
}
} // namespace graph::query::execution::detail
