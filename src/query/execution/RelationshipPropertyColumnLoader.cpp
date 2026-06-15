#include "graph/query/execution/RelationshipPropertyColumnLoader.hpp"

#include <utility>

#include "graph/query/execution/PropertyColumnLoadKernel.hpp"

namespace graph::query::execution {

	RelationshipPropertyColumnLoader::RelationshipPropertyColumnLoader(std::shared_ptr<storage::DataManager> dm,
	                                                                 concurrent::ThreadPool *threadPool)
		: dm_(std::move(dm)), threadPool_(threadPool) {}

	std::unordered_map<std::string, std::vector<std::optional<PropertyValue>>>
	RelationshipPropertyColumnLoader::loadColumns(const std::vector<Edge> &edges,
	                                             const std::vector<uint8_t> &selected,
	                                             const std::vector<std::string> &requiredProperties) const {
		return PropertyColumnLoadKernel::load(
				edges.size(),
				selected,
				requiredProperties,
				dm_,
				threadPool_,
				{.extract = "relationship_count.extract_property_columns",
				 .loadPropertyEntities = "relationship_count.load_property_entities"},
				[&](size_t row) {
					const auto &edge = edges[row];
					return PropertyColumnLoadKernel::RowView{
							.active = edge.getId() != 0 && edge.isActive(),
							.storageType = edge.getPropertyStorageType(),
							.propertyEntityId = edge.getPropertyEntityId(),
							.inlineProperties = &edge.getProperties()};
				},
				[&](size_t row) { return dm_->getEdgeProperties(edges[row].getId()); });
	}

	std::unordered_map<std::string, std::vector<std::optional<PropertyValue>>>
	RelationshipPropertyColumnLoader::loadColumns(const RelationshipMetadataBatch &metadataBatch,
	                                             const std::vector<uint8_t> &selected,
	                                             const std::vector<std::string> &requiredProperties) const {
		return PropertyColumnLoadKernel::load(
				metadataBatch.size(),
				selected,
				requiredProperties,
				dm_,
				threadPool_,
				{.extract = "relationship_count.extract_property_columns",
				 .loadPropertyEntities = "relationship_count.load_property_entities"},
				[&](size_t row) {
					return PropertyColumnLoadKernel::RowView{
							.active = metadataBatch.isValid(row) && metadataBatch.active[row] != 0,
							.storageType = metadataBatch.propertyStorageTypes[row],
							.propertyEntityId = metadataBatch.propertyEntityIds[row],
							.inlineProperties = nullptr};
				},
				[&](size_t row) { return dm_->getEdgeProperties(metadataBatch.edgeIds[row]); });
	}

} // namespace graph::query::execution
