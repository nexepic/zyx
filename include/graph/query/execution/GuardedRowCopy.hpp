#pragma once

#include <span>
#include <type_traits>
#include <utility>

#include "graph/query/execution/GuardedEntityCopy.hpp"
#include "graph/query/execution/LogicalPlanReadRuntime.hpp"

namespace graph::query::execution::detail {

template <typename Checkpoint>
LogicalPlanReadValue copyLogicalReadValueGuarded(const LogicalPlanReadValue &value, Checkpoint checkpoint) {
	checkpoint();
	return std::visit([&](const auto &entry) -> LogicalPlanReadValue {
			using Value = std::decay_t<decltype(entry)>;
			if constexpr (std::is_same_v<Value, PropertyValue>)
				return copyPropertyValueGuarded(entry, checkpoint);
			else if constexpr (std::is_same_v<Value, storage::EntityRef>)
				return entry;
			else
				return copyLogicalEntityGuarded(entry, checkpoint);
		}, value);
}

template <typename Checkpoint>
LogicalPlanReadPath copyLogicalReadPathGuarded(std::span<const storage::LogicalGraphNode> nodes,
		std::span<const storage::LogicalGraphEdge> edges, std::span<const storage::EntityRef> nodeRefs,
		std::span<const storage::EntityRef> edgeRefs, Checkpoint checkpoint) {
	checkpoint();
	LogicalPlanReadPath copy;
	copy.nodes.reserve(nodes.size());
	copy.edges.reserve(edges.size());
	copy.node_refs.reserve(nodeRefs.size());
	copy.edge_refs.reserve(edgeRefs.size());
	for (const auto &node : nodes) copy.nodes.push_back(copyLogicalEntityGuarded(node, checkpoint));
	for (const auto &edge : edges) copy.edges.push_back(copyLogicalEntityGuarded(edge, checkpoint));
	for (const auto &ref : nodeRefs) { checkpoint(); copy.node_refs.push_back(ref); }
	for (const auto &ref : edgeRefs) { checkpoint(); copy.edge_refs.push_back(ref); }
	return copy;
}

template <typename Checkpoint>
LogicalPlanReadPath copyLogicalReadPathGuarded(const LogicalPlanReadPath &path, Checkpoint checkpoint) {
	return copyLogicalReadPathGuarded(path.nodes, path.edges, path.node_refs, path.edge_refs, checkpoint);
}

// Build an independent row, preserving delayed references and cache completeness.
// The caller admits and retains the copy's memory for its entire lifetime.
template <typename Checkpoint>
LogicalPlanReadRow copyLogicalReadRowGuarded(const LogicalPlanReadRow &source, Checkpoint checkpoint) {
	checkpoint();
	LogicalPlanReadRow result;
	result.values.reserve(source.values.size());
	for (const auto &[key, value] : source.values) {
		checkpoint();
		auto copy = copyLogicalReadValueGuarded(value, checkpoint);
		result.values.emplace(copyPropertyStringGuarded(key, checkpoint), std::move(copy));
	}
	const auto copyCache = [&](const auto &cache, auto &destination) {
		destination.reserve(cache.size());
		for (const auto &[key, values] : cache) {
			checkpoint();
			auto copy = copyPropertyMapGuarded(values, checkpoint);
			destination.emplace(copyPropertyStringGuarded(key, checkpoint), std::move(copy));
		}
	};
	copyCache(source.property_cache, result.property_cache);
	copyCache(source.entity_metadata_cache, result.entity_metadata_cache);
	result.complete_entity_property_cache.reserve(source.complete_entity_property_cache.size());
	for (const auto &key : source.complete_entity_property_cache)
		result.complete_entity_property_cache.emplace(copyPropertyStringGuarded(key, checkpoint));
	result.path_cache.reserve(source.path_cache.size());
	for (const auto &[key, path] : source.path_cache) {
		checkpoint();
		auto copy = copyLogicalReadPathGuarded(path, checkpoint);
		result.path_cache.emplace(copyPropertyStringGuarded(key, checkpoint), std::move(copy));
	}
	checkpoint();
	return result;
}

}
