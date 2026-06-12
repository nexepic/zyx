#include "graph/storage/data/DataManager.hpp"

#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/data/EdgeManager.hpp"
#include "graph/storage/data/NodeManager.hpp"
#include "graph/storage/data/PropertyManager.hpp"

namespace graph::storage {
namespace {

	void validateColumnarInput(size_t count, const std::vector<BulkPropertyColumn> &columns) {
		std::unordered_set<std::string> keys;
		keys.reserve(columns.size());
		for (const auto &column: columns) {
			if (column.key.empty()) {
				throw std::invalid_argument("Columnar bulk property columns require non-empty keys");
			}
			if (column.values.size() != count) {
				throw std::invalid_argument("Columnar bulk property columns must match row count");
			}
			auto [_, inserted] = keys.insert(column.key);
			if (!inserted) {
				throw std::invalid_argument("Columnar bulk property columns require unique keys");
			}
		}
	}

	std::unordered_map<std::string, PropertyValue>
	materializePropertyRow(const std::vector<BulkPropertyColumn> &columns, size_t row) {
		std::unordered_map<std::string, PropertyValue> properties;
		properties.reserve(columns.size());
		for (const auto &column: columns) {
			properties.emplace(column.key, column.values[row]);
		}
		return properties;
	}

} // namespace

	std::vector<int64_t> DataManager::addNodesColumnar(
			int64_t labelId,
			size_t count,
			const std::vector<BulkPropertyColumn> &columns) const {
		guardReadOnly();
		validateColumnarInput(count, columns);
		if (count == 0) {
			return {};
		}

		std::vector<Node> nodes;
		nodes.reserve(count);
		for (size_t row = 0; row < count; ++row) {
			nodes.emplace_back(0, labelId);
		}

		{
			debug::ScopedPerfTimer timer("datamanager.add_nodes_columnar.validate");
			const auto &validators = observerManager_.getValidators();
			if (!validators.empty()) {
				for (size_t row = 0; row < count; ++row) {
					auto properties = materializePropertyRow(columns, row);
					Node validationNode = nodes[row];
					validationNode.setProperties(properties);
					for (const auto &validator: validators) {
						validator->validateNodeInsert(validationNode, properties);
					}
				}
			}
		}

		{
			debug::ScopedPerfTimer timer("datamanager.add_nodes_columnar.prepare_ids");
			nodeManager_->prepareAddBatch(nodes);
		}
		{
			debug::ScopedPerfTimer timer("datamanager.add_nodes_columnar.notify_indexes");
			observerManager_.notifyNodesAddedColumnar(nodes, columns);
		}
		{
			debug::ScopedPerfTimer timer("datamanager.add_nodes_columnar.store_properties");
			(void) propertyManager_->storePropertiesColumnarBatch(nodes, columns);
		}
		{
			debug::ScopedPerfTimer timer("datamanager.add_nodes_columnar.entity_write");
			nodeManager_->persistPreparedAddBatch(nodes);
		}
		{
			debug::ScopedPerfTimer timer("datamanager.add_nodes_columnar.txn_record");
			txnContext_.recordAdds(nodes);
		}

		std::vector<int64_t> ids;
		ids.reserve(nodes.size());
		for (const auto &node: nodes) {
			ids.push_back(node.getId());
		}
		return ids;
	}

	std::vector<int64_t> DataManager::addEdgesColumnar(
			int64_t typeId,
			const std::vector<int64_t> &sourceIds,
			const std::vector<int64_t> &targetIds,
			const std::vector<BulkPropertyColumn> &columns) const {
		guardReadOnly();
		if (sourceIds.size() != targetIds.size()) {
			throw std::invalid_argument("Columnar edge bulk insert requires matching source and target counts");
		}
		const size_t count = sourceIds.size();
		validateColumnarInput(count, columns);
		if (count == 0) {
			return {};
		}

		std::vector<Edge> edges;
		edges.reserve(count);
		for (size_t row = 0; row < count; ++row) {
			edges.emplace_back(0, sourceIds[row], targetIds[row], typeId);
		}

		{
			debug::ScopedPerfTimer timer("datamanager.add_edges_columnar.validate");
			const auto &validators = observerManager_.getValidators();
			if (!validators.empty()) {
				for (size_t row = 0; row < count; ++row) {
					auto properties = materializePropertyRow(columns, row);
					Edge validationEdge = edges[row];
					validationEdge.setProperties(properties);
					for (const auto &validator: validators) {
						validator->validateEdgeInsert(validationEdge, properties);
					}
				}
			}
		}

		traversal::RelationshipBatchLinkUpdates linkUpdates;
		{
			debug::ScopedPerfTimer timer("datamanager.add_edges_columnar.prepare_links");
			linkUpdates = edgeManager_->prepareAddBatch(edges);
		}
		{
			debug::ScopedPerfTimer timer("datamanager.add_edges_columnar.notify_indexes");
			observerManager_.notifyEdgesAddedColumnar(edges, columns);
		}
		{
			debug::ScopedPerfTimer timer("datamanager.add_edges_columnar.store_properties");
			(void) propertyManager_->storePropertiesColumnarBatch(edges, columns);
		}
		{
			debug::ScopedPerfTimer timer("datamanager.add_edges_columnar.entity_write");
			edgeManager_->persistPreparedAddBatch(edges, linkUpdates);
		}
		{
			debug::ScopedPerfTimer timer("datamanager.add_edges_columnar.txn_record");
			txnContext_.recordAdds(edges);
		}

		std::vector<int64_t> ids;
		ids.reserve(edges.size());
		for (const auto &edge: edges) {
			ids.push_back(edge.getId());
		}
		return ids;
	}

} // namespace graph::storage
