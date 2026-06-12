#include "graph/storage/indexes/IEntityObserver.hpp"

#include <unordered_map>

namespace graph {
namespace {

	template<typename EntityType>
	std::vector<EntityType> materializeColumnarProperties(
			const std::vector<EntityType> &entities,
			const std::vector<storage::BulkPropertyColumn> &columns) {
		if (entities.empty() || columns.empty()) {
			return entities;
		}

		std::vector<EntityType> materialized = entities;
		for (size_t row = 0; row < materialized.size(); ++row) {
			std::unordered_map<std::string, PropertyValue> properties;
			properties.reserve(columns.size());
			for (const auto &column: columns) {
				if (row < column.values.size()) {
					properties.emplace(column.key, column.values[row]);
				}
			}
			materialized[row].setProperties(std::move(properties));
		}
		return materialized;
	}

} // namespace

	void IEntityObserver::onNodesAddedColumnar(
			const std::vector<Node> &nodes,
			const std::vector<storage::BulkPropertyColumn> &columns) {
		onNodesAdded(materializeColumnarProperties(nodes, columns));
	}

	void IEntityObserver::onEdgesAddedColumnar(
			const std::vector<Edge> &edges,
			const std::vector<storage::BulkPropertyColumn> &columns) {
		onEdgesAdded(materializeColumnarProperties(edges, columns));
	}

} // namespace graph
