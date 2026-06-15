#include "graph/query/execution/NodePropertyColumnLoader.hpp"

#include <utility>

#include "graph/query/execution/PropertyColumnLoadKernel.hpp"

namespace graph::query::execution {

	NodePropertyColumnLoader::NodePropertyColumnLoader(std::shared_ptr<storage::DataManager> dm,
	                                                 concurrent::ThreadPool *threadPool)
		: dm_(std::move(dm)), threadPool_(threadPool) {}

	std::unordered_map<std::string, std::vector<std::optional<PropertyValue>>>
	NodePropertyColumnLoader::loadColumns(const std::vector<Node> &nodes,
	                                      const std::vector<uint8_t> &selected,
	                                      const std::vector<std::string> &requiredProperties) const {
		return PropertyColumnLoadKernel::load(
				nodes.size(),
				selected,
				requiredProperties,
				dm_,
				threadPool_,
				{.extract = "node_scan.extract_property_columns",
				 .loadPropertyEntities = "node_scan.load_property_entities"},
				[&](size_t row) {
					const auto &node = nodes[row];
					return PropertyColumnLoadKernel::RowView{
							.active = node.getId() != 0 && node.isActive(),
							.storageType = node.getPropertyStorageType(),
							.propertyEntityId = node.getPropertyEntityId(),
							.inlineProperties = &node.getProperties()};
				},
				[&](size_t row) { return dm_->getNodePropertiesDirect(nodes[row]); });
	}

	std::unordered_map<std::string, std::vector<std::optional<PropertyValue>>>
	NodePropertyColumnLoader::loadColumns(const NodeMetadataBatch &metadataBatch,
	                                      const std::vector<uint8_t> &selected,
	                                      const std::vector<std::string> &requiredProperties) const {
		return PropertyColumnLoadKernel::load(
				metadataBatch.size(),
				selected,
				requiredProperties,
				dm_,
				threadPool_,
				{.extract = "node_scan.extract_property_columns",
				 .loadPropertyEntities = "node_scan.load_property_entities"},
				[&](size_t row) {
					return PropertyColumnLoadKernel::RowView{
							.active = metadataBatch.isValid(row) && metadataBatch.active[row] != 0,
							.storageType = metadataBatch.propertyStorageTypes[row],
							.propertyEntityId = metadataBatch.propertyEntityIds[row],
							.inlineProperties = nullptr};
				},
				[&](size_t row) {
					Node node = metadataBatch.toNode(row);
					return dm_->getNodePropertiesDirect(node);
				});
	}

} // namespace graph::query::execution
