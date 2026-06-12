/**
 * @file PropertyManager.cpp
 * @author Nexepic
 * @date 2025/7/24
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 **/

#include "graph/storage/data/PropertyManager.hpp"
#include <algorithm>
#include <stdexcept>
#include <sstream>
#include <type_traits>
#include "graph/core/Blob.hpp"
#include "graph/core/Edge.hpp"
#include "graph/core/EntityPropertyTraits.hpp"
#include "graph/core/Index.hpp"
#include "graph/core/Node.hpp"
#include "graph/core/Property.hpp"
#include "graph/core/State.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/IDAllocator.hpp"
#include "graph/storage/data/BlobManager.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/utils/Serializer.hpp"

namespace graph::storage {
namespace {

	template<typename T>
	void appendPod(std::vector<char> &buffer, const T &value) {
		static_assert(std::is_trivial_v<T>, "appendPod expects a trivial value");
		const auto *bytes = reinterpret_cast<const char *>(&value);
		buffer.insert(buffer.end(), bytes, bytes + sizeof(T));
	}

	void appendString(std::vector<char> &buffer, const std::string &value) {
		appendPod(buffer, static_cast<uint32_t>(value.size()));
		buffer.insert(buffer.end(), value.begin(), value.end());
	}

	void appendPropertyValue(std::vector<char> &buffer, const PropertyValue &value) {
		std::visit(
				[&buffer](const auto &arg) {
					using ValueType = std::decay_t<decltype(arg)>;
					if constexpr (std::is_same_v<ValueType, std::monostate>) {
						appendPod(buffer, PropertyType::NULL_TYPE);
					} else if constexpr (std::is_same_v<ValueType, bool>) {
						appendPod(buffer, PropertyType::BOOLEAN);
						appendPod(buffer, arg);
					} else if constexpr (std::is_same_v<ValueType, int64_t>) {
						appendPod(buffer, PropertyType::INTEGER);
						appendPod(buffer, arg);
					} else if constexpr (std::is_same_v<ValueType, double>) {
						appendPod(buffer, PropertyType::DOUBLE);
						appendPod(buffer, arg);
					} else if constexpr (std::is_same_v<ValueType, std::string>) {
						appendPod(buffer, PropertyType::STRING);
						appendString(buffer, arg);
					} else if constexpr (std::is_same_v<ValueType, std::vector<PropertyValue>>) {
						appendPod(buffer, PropertyType::LIST);
						appendPod(buffer, static_cast<uint32_t>(arg.size()));
						for (const auto &element: arg) {
							appendPropertyValue(buffer, element);
						}
					} else if constexpr (std::is_same_v<ValueType, PropertyValue::MapType>) {
						appendPod(buffer, PropertyType::MAP);
						appendPod(buffer, static_cast<uint32_t>(arg.size()));
						for (const auto &[key, mapValue]: arg) {
							appendString(buffer, key);
							appendPropertyValue(buffer, mapValue);
						}
					} else if constexpr (std::is_same_v<ValueType, TemporalDate>) {
						appendPod(buffer, PropertyType::DATE);
						appendPod(buffer, arg.epochDays);
					} else if constexpr (std::is_same_v<ValueType, TemporalDateTime>) {
						appendPod(buffer, PropertyType::DATETIME);
						appendPod(buffer, arg.epochMillis);
					} else if constexpr (std::is_same_v<ValueType, TemporalDuration>) {
						appendPod(buffer, PropertyType::DURATION);
						appendPod(buffer, arg.months);
						appendPod(buffer, arg.days);
						appendPod(buffer, arg.nanos);
					}
				},
				value.getVariant());
	}

	std::vector<char> serializeColumnarPropertyPayload(
			const std::vector<BulkPropertyColumn> &columns,
			size_t row) {
		std::vector<char> payload;
		size_t reserveSize = sizeof(uint32_t);
		for (const auto &column: columns) {
			reserveSize += sizeof(uint32_t) + column.key.size() + sizeof(PropertyType) + sizeof(uint64_t);
		}
		payload.reserve((std::min)(reserveSize, Property::TOTAL_PROPERTY_SIZE - Property::METADATA_SIZE));
		appendPod(payload, static_cast<uint32_t>(columns.size()));
		for (const auto &column: columns) {
			appendString(payload, column.key);
			appendPropertyValue(payload, column.values[row]);
		}
		return payload;
	}

	std::unordered_map<std::string, PropertyValue> materializeColumnarPropertyRow(
			const std::vector<BulkPropertyColumn> &columns,
			size_t row) {
		std::unordered_map<std::string, PropertyValue> properties;
		properties.reserve(columns.size());
		for (const auto &column: columns) {
			properties.emplace(column.key, column.values[row]);
		}
		return properties;
	}

} // namespace

	PropertyManager::PropertyManager(DataManager* dataManager,
									 std::shared_ptr<DeletionManager> deletionManager) :
		BaseEntityManager(dataManager, std::move(deletionManager)) {}

	int64_t PropertyManager::doAllocateId() {
		return getDataManagerPtr()->getIdAllocator(EntityType::Property)->allocate();
	}

	void PropertyManager::doRemove(Property &property) {
		// Delegate the complex deletion logic to the DeletionManager.
		deletionManager_->deleteProperty(property);
	}

	uint32_t
	PropertyManager::calculateSerializedSize(const std::unordered_map<std::string, PropertyValue> &properties) {
		// Calculate the size of the serialized property set
		uint32_t size = sizeof(uint32_t); // Number of properties

		for (const auto &[key, value]: properties) {
			size += sizeof(uint32_t) + key.size(); // Key length + key content
			size += utils::getSerializedSize(value); // Value size
		}

		return size;
	}

	void PropertyManager::serializeProperties(std::ostream &os,
											 const std::unordered_map<std::string, PropertyValue> &properties) {
		// Write the number of properties
		utils::Serializer::writePOD(os, static_cast<uint32_t>(properties.size()));

		// Write each property key-value pair
		for (const auto &[key, value]: properties) {
			utils::Serializer::serialize(os, key);
			utils::Serializer::serialize<PropertyValue>(os, value);
		}
	}

	std::unordered_map<std::string, PropertyValue> PropertyManager::deserializeProperties(std::istream &is) {
		std::unordered_map<std::string, PropertyValue> properties;

		// Read the number of properties
		auto propertyCount = utils::Serializer::readPOD<uint32_t>(is);

		// Read each property key-value pair
		for (uint32_t i = 0; i < propertyCount; ++i) {
			std::string key = utils::Serializer::deserialize<std::string>(is);
			PropertyValue value = utils::Serializer::deserialize<PropertyValue>(is);
			properties[key] = value;
		}

		return properties;
	}

	template<typename EntityType>
	void PropertyManager::storeProperties(EntityType &entity) {
		// Only process entity types that support properties.
		if constexpr (!EntityPropertyTraits<EntityType>::supportsProperties) {
			return;
		}

		[[maybe_unused]] auto *dataManager = getDataManagerPtr();

		// Step 1: ALWAYS clean up any existing external properties first.
		// This ensures we start from a clean slate every time properties are modified.
		if constexpr (EntityPropertyTraits<EntityType>::supportsExternalProperties) {
			cleanupExternalProperties(entity);
			// After cleanup, immediately reset the reference on the entity.
			EntityPropertyTraits<EntityType>::setPropertyEntityId(entity, 0, PropertyStorageType::NONE);
		}

		auto properties = EntityPropertyTraits<EntityType>::getProperties(entity);

		// Step 2: If the new set of properties is empty, our job is done.
		if (properties.empty()) {
			return;
		}

		// Step 3: Decide the storage strategy based on size.
		if constexpr (EntityPropertyTraits<EntityType>::supportsExternalProperties) {
			uint32_t dataSize = calculateSerializedSize(properties);
			constexpr size_t PROPERTY_ENTITY_PAYLOAD_SIZE = Property::TOTAL_PROPERTY_SIZE - Property::METADATA_SIZE;

			if (dataSize > PROPERTY_ENTITY_PAYLOAD_SIZE) {
				// Data is large, delegate to the Blob storage worker.
				storePropertiesInBlob(entity, properties);
			} else {
				// Data fits in a Property entity, delegate to the Property storage worker.
				storePropertiesInPropertyEntity(entity, properties);
			}
		}
		// If external properties are not supported, they remain inline, and no further action is needed.
	}

	template<typename EntityType>
	std::vector<size_t> PropertyManager::storePropertiesBatch(std::vector<EntityType> &entities) {
		graph::debug::ScopedPerfTimer timer("property.store_batch.total");
		if constexpr (!EntityPropertyTraits<EntityType>::supportsProperties ||
					  !EntityPropertyTraits<EntityType>::supportsExternalProperties) {
			return {};
		} else {
			if (entities.empty()) {
				return {};
			}

			constexpr size_t PROPERTY_ENTITY_PAYLOAD_SIZE = Property::TOTAL_PROPERTY_SIZE - Property::METADATA_SIZE;
			std::vector<size_t> propertyEntityOwnerIndices;
			propertyEntityOwnerIndices.reserve(entities.size());
			std::vector<Property> propertyEntities;
			propertyEntities.reserve(entities.size());
			std::vector<size_t> changedEntityIndices;
			changedEntityIndices.reserve(entities.size());

			for (size_t index = 0; index < entities.size(); ++index) {
				auto &entity = entities[index];
				const bool hadExternalProperties = EntityPropertyTraits<EntityType>::hasPropertyEntity(entity);
				cleanupExternalProperties(entity);
				EntityPropertyTraits<EntityType>::setPropertyEntityId(entity, 0, PropertyStorageType::NONE);

				auto properties = EntityPropertyTraits<EntityType>::takeProperties(entity);
				if (properties.empty()) {
					if (hadExternalProperties) {
						changedEntityIndices.push_back(index);
					}
					continue;
				}

				const uint32_t dataSize = calculateSerializedSize(properties);
				if (dataSize > PROPERTY_ENTITY_PAYLOAD_SIZE) {
					graph::debug::ScopedPerfTimer blobTimer("property.store_batch.blobs");
					storePropertiesInBlob(entity, properties);
					changedEntityIndices.push_back(index);
					continue;
				}

				Property property;
				property.setProperties(std::move(properties));
				property.setEntityInfo(entity.getId(), EntityType::typeId);
				propertyEntityOwnerIndices.push_back(index);
				propertyEntities.push_back(std::move(property));
			}

			if (!propertyEntities.empty()) {
				graph::debug::ScopedPerfTimer entityTimer("property.store_batch.property_entities");
				getDataManagerPtr()->addPropertyEntities(propertyEntities);

				for (size_t i = 0; i < propertyEntityOwnerIndices.size(); ++i) {
					auto &owner = entities[propertyEntityOwnerIndices[i]];
					EntityPropertyTraits<EntityType>::setPropertyEntityId(
							owner, propertyEntities[i].getId(), PropertyStorageType::PROPERTY_ENTITY);
					EntityPropertyTraits<EntityType>::clearProperties(owner);
					changedEntityIndices.push_back(propertyEntityOwnerIndices[i]);
				}
			}

			return changedEntityIndices;
		}
	}

	template<typename EntityType>
	std::vector<size_t> PropertyManager::storePropertiesColumnarBatch(
			std::vector<EntityType> &entities,
			const std::vector<BulkPropertyColumn> &columns) {
		graph::debug::ScopedPerfTimer timer("property.store_columnar_batch.total");
		if constexpr (!EntityPropertyTraits<EntityType>::supportsProperties ||
					  !EntityPropertyTraits<EntityType>::supportsExternalProperties) {
			return {};
		} else {
			if (entities.empty() || columns.empty()) {
				return {};
			}

			for (const auto &column: columns) {
				if (column.values.size() != entities.size()) {
					throw std::invalid_argument("Columnar property batch requires all columns to match entity count");
				}
			}

			constexpr size_t PROPERTY_ENTITY_PAYLOAD_SIZE = Property::TOTAL_PROPERTY_SIZE - Property::METADATA_SIZE;
			std::vector<size_t> propertyEntityOwnerIndices;
			propertyEntityOwnerIndices.reserve(entities.size());
			std::vector<Property> propertyEntities;
			propertyEntities.reserve(entities.size());
			std::vector<size_t> changedEntityIndices;
			changedEntityIndices.reserve(entities.size());

			for (size_t row = 0; row < entities.size(); ++row) {
				auto &entity = entities[row];
				if (EntityPropertyTraits<EntityType>::hasPropertyEntity(entity)) {
					cleanupExternalProperties(entity);
					EntityPropertyTraits<EntityType>::setPropertyEntityId(entity, 0, PropertyStorageType::NONE);
				}

				auto payload = serializeColumnarPropertyPayload(columns, row);
				if (payload.size() > PROPERTY_ENTITY_PAYLOAD_SIZE) {
					graph::debug::ScopedPerfTimer blobTimer("property.store_columnar_batch.blobs");
					auto properties = materializeColumnarPropertyRow(columns, row);
					storePropertiesInBlob(entity, properties);
					changedEntityIndices.push_back(row);
					continue;
				}

				Property property(0, entity.getId(), EntityType::typeId);
				property.setSerializedPropertyPayload(std::move(payload));
				propertyEntityOwnerIndices.push_back(row);
				propertyEntities.push_back(std::move(property));
			}

			if (!propertyEntities.empty()) {
				graph::debug::ScopedPerfTimer entityTimer("property.store_columnar_batch.property_entities");
				getDataManagerPtr()->addPropertyEntities(propertyEntities);
				for (size_t i = 0; i < propertyEntityOwnerIndices.size(); ++i) {
					auto &owner = entities[propertyEntityOwnerIndices[i]];
					EntityPropertyTraits<EntityType>::setPropertyEntityId(
							owner, propertyEntities[i].getId(), PropertyStorageType::PROPERTY_ENTITY);
					EntityPropertyTraits<EntityType>::clearProperties(owner);
					changedEntityIndices.push_back(propertyEntityOwnerIndices[i]);
				}
			}

			return changedEntityIndices;
		}
	}

	template<typename EntityType>
	void PropertyManager::cleanupExternalProperties(EntityType &entity) {
		// Only process entity types that support external properties
		if constexpr (!EntityPropertyTraits<EntityType>::supportsExternalProperties) {
			return; // Do nothing for entity types that don't support external properties
		}

		auto *dataManager = getDataManagerPtr();

		if (!EntityPropertyTraits<EntityType>::hasPropertyEntity(entity)) {
			return;
		}

		int64_t propertyId = EntityPropertyTraits<EntityType>::getPropertyEntityId(entity);
		auto storageType = EntityPropertyTraits<EntityType>::getPropertyStorageType(entity);

		if (storageType == PropertyStorageType::BLOB_ENTITY) {
			// Delete the entire blob chain
			auto blobManager = dataManager->getBlobManager();
			blobManager->deleteBlobChain(propertyId);
		} else {
			// PROPERTY_ENTITY
			Property property = dataManager->getProperty(propertyId);
			dataManager->deleteProperty(property);
		}
	}

	template<typename EntityType>
	void
	PropertyManager::storePropertiesInPropertyEntity(EntityType &entity,
													 const std::unordered_map<std::string, PropertyValue> &properties) {
		// Only process entity types that support external properties.
		if constexpr (!EntityPropertyTraits<EntityType>::supportsExternalProperties) {
			return;
		}

		auto *dataManager = getDataManagerPtr();

		// This function's responsibility is simple: create a new Property entity.
		// It assumes any previous external storage has already been cleaned up by the caller (storeProperties).

		// Create and configure the new property entity.
		Property newProperty;
		newProperty.setProperties(properties);
		newProperty.setEntityInfo(entity.getId(), EntityType::typeId); // Set back-reference

		// Add the new entity to storage.
		dataManager->addPropertyEntity(newProperty);

		// Update the parent entity to reference the new property entity.
		EntityPropertyTraits<EntityType>::setPropertyEntityId(entity, newProperty.getId(),
															 PropertyStorageType::PROPERTY_ENTITY);

		// Clear the properties from the in-memory entity object, as they are now stored externally.
		EntityPropertyTraits<EntityType>::clearProperties(entity);
	}

	template<typename EntityType>
	void PropertyManager::storePropertiesInBlob(EntityType &entity,
											 const std::unordered_map<std::string, PropertyValue> &properties) {
		// Only process entity types that support external properties.
		if constexpr (!EntityPropertyTraits<EntityType>::supportsExternalProperties) {
			return;
		}

		auto *dataManager = getDataManagerPtr();
		auto blobManager = dataManager->getBlobManager();

		// This function's responsibility is simple: create a new Blob chain.
		// It assumes any previous external storage has already been cleaned up by the caller (storeProperties).

		// Serialize properties to a string.
		std::stringstream ss;
		serializeProperties(ss, properties);
		std::string serializedData = ss.str();

		// Create a new blob chain for the serialized properties.
		auto blobChain = blobManager->createBlobChain(entity.getId(), EntityType::typeId, serializedData);

		// Update the entity to reference the head of the new blob chain.
		EntityPropertyTraits<EntityType>::setPropertyEntityId(entity, blobChain.front().getId(),
														 PropertyStorageType::BLOB_ENTITY);

		// Clear the properties from the in-memory entity object, as they are now stored externally.
		EntityPropertyTraits<EntityType>::clearProperties(entity);
	}

	std::unordered_map<std::string, PropertyValue> PropertyManager::getPropertiesFromBlob(int64_t blobId) const {
		auto *dataManager = getDataManagerPtr();
		auto blobManager = dataManager->getBlobManager();

		try {
			// Read data from blob chain
			std::string serializedData = blobManager->readBlobChain(blobId);

			// Deserialize properties
			std::stringstream ss(serializedData);
			return deserializeProperties(ss);
		} catch (const std::exception &e) {
			// Return empty map on error to handle corrupted storage gracefully
			return {};
		}
	}

	template<typename EntityType>
	std::unordered_map<std::string, PropertyValue> PropertyManager::getEntityProperties(int64_t entityId) {
		// Default empty properties
		std::unordered_map<std::string, PropertyValue> allProperties;

		// Only process entity types that support properties
		if constexpr (!EntityPropertyTraits<EntityType>::supportsProperties) {
			return allProperties; // Return empty properties for unsupported entity types
		}

		auto *dataManager = getDataManagerPtr();

		// Get the entity
		auto entity = dataManager->getEntity<EntityType>(entityId);

		// If entity doesn't exist or is inactive, return empty map
		if (entity.getId() == 0 || !entity.isActive()) { // ZYX_COV_EXCL_LINE
			return allProperties;
		}

		// Start with inline properties
		allProperties = EntityPropertyTraits<EntityType>::getProperties(entity);

		// Check if the entity supports external properties and has them
		if constexpr (EntityPropertyTraits<EntityType>::supportsExternalProperties) {
			if (EntityPropertyTraits<EntityType>::hasPropertyEntity(entity)) {
				auto storageType = EntityPropertyTraits<EntityType>::getPropertyStorageType(entity);
				auto propertyEntityId = EntityPropertyTraits<EntityType>::getPropertyEntityId(entity);

				if (storageType == PropertyStorageType::PROPERTY_ENTITY) {
					// Load from Property entity
					Property property = dataManager->getProperty(propertyEntityId);

					// Merge properties
					const auto &externalProps = property.getPropertyValues();
					for (const auto &[key, value]: externalProps) {
						allProperties[key] = value;
					}

					// Add property to cache
					dataManager->addToCache(property);
				} else if (storageType == PropertyStorageType::BLOB_ENTITY) { // ZYX_COV_EXCL_LINE
					// Load from Blob chain
					auto blobProperties = getPropertiesFromBlob(propertyEntityId);

					// Merge properties
					for (const auto &[key, value]: blobProperties) {
						allProperties[key] = value;
					}
				}
			}
		}

		return allProperties;
	}

	template<typename EntityType>
	void PropertyManager::addEntityProperties(int64_t entityId,
											 const std::unordered_map<std::string, PropertyValue> &properties) {
		// Only process entity types that support properties
		if constexpr (!EntityPropertyTraits<EntityType>::supportsProperties) {
			throw std::runtime_error("Entity type does not support properties");
		}

		auto *dataManager = getDataManagerPtr();

		// Get the entity
		auto entity = dataManager->getEntity<EntityType>(entityId);

		// Check if entity exists and is active
		if (entity.getId() == 0 || !entity.isActive()) { // ZYX_COV_EXCL_LINE
			throw std::runtime_error("Cannot add properties to non-existent or inactive: " + std::to_string(entityId));
		}

		// Add properties to the entity
		for (const auto &[key, value]: properties) {
			EntityPropertyTraits<EntityType>::addProperty(entity, key, value);
		}

		// Store properties using the appropriate mechanism
		storeProperties<EntityType>(entity);

		// Update the entity
		dataManager->updateEntity(entity);
	}

	template<typename EntityType>
	void PropertyManager::removeEntityProperty(int64_t entityId, const std::string &key) {
		// This function is idempotent. If the property does not exist, it does nothing and returns successfully.

		// Only process entity types that support properties.
		if constexpr (!EntityPropertyTraits<EntityType>::supportsProperties) {
			// Silently return for unsupported types, as this could be a generic cleanup call.
			return;
		}

		auto *dataManager = getDataManagerPtr();

		// Get the entity.
		auto entity = dataManager->getEntity<EntityType>(entityId);

		// Check if the entity exists and is active.
		if (entity.getId() == 0 || !entity.isActive()) { // ZYX_COV_EXCL_LINE
			// Cannot remove property from a non-existent entity, but we don't throw to maintain idempotency.
			return;
		}

		bool propertyWasRemoved = false;

		// Attempt to remove from inline properties.
		if (EntityPropertyTraits<EntityType>::hasProperty(entity, key)) {
			EntityPropertyTraits<EntityType>::removeProperty(entity, key);
			propertyWasRemoved = true;
		}

		// If not found inline, check external storage if supported.
		if constexpr (EntityPropertyTraits<EntityType>::supportsExternalProperties) {
			if (!propertyWasRemoved && EntityPropertyTraits<EntityType>::hasPropertyEntity(entity)) {
				auto storageType = EntityPropertyTraits<EntityType>::getPropertyStorageType(entity);
				auto propertyEntityId = EntityPropertyTraits<EntityType>::getPropertyEntityId(entity);

				if (storageType == PropertyStorageType::PROPERTY_ENTITY) {
					Property property = dataManager->getProperty(propertyEntityId);
					if (property.hasPropertyValue(key)) {
						auto properties = property.getPropertyValues();
						properties.erase(key);
						property.setProperties(properties);
						propertyWasRemoved = true;

						// If no properties are left, delete the property entity itself.
						if (properties.empty()) {
							dataManager->deleteProperty(property);
							EntityPropertyTraits<EntityType>::setPropertyEntityId(entity, 0, PropertyStorageType::NONE);
						} else {
							dataManager->updatePropertyEntity(property);
						}
					}
				} else if (storageType == PropertyStorageType::BLOB_ENTITY) { // ZYX_COV_EXCL_LINE
					// Get properties from blob chain to modify them.
					auto properties = getPropertiesFromBlob(propertyEntityId);
					auto it = properties.find(key);
					if (it != properties.end()) {
						properties.erase(it);
						propertyWasRemoved = true;

						auto blobManager = dataManager->getBlobManager();

						// First, delete the old blob chain completely.
						blobManager->deleteBlobChain(propertyEntityId);

						if (properties.empty()) {
							// If no properties left, just clear the reference on the entity.
							EntityPropertyTraits<EntityType>::setPropertyEntityId(entity, 0, PropertyStorageType::NONE);
						} else {
							// Re-serialize the remaining properties into a new blob chain.
							std::stringstream ss;
							serializeProperties(ss, properties);
							auto newBlobChain =
									blobManager->createBlobChain(entity.getId(), EntityType::typeId, ss.str());
							EntityPropertyTraits<EntityType>::setPropertyEntityId(
									entity, newBlobChain.front().getId(), PropertyStorageType::BLOB_ENTITY);
						}
					}
				}
			}
		}

		if (propertyWasRemoved) {
			// If a property was changed, update the parent entity.
			dataManager->updateEntity(entity);
		}
	}

	template<typename EntityType>
	bool PropertyManager::hasExternalProperty(const EntityType &entity, const std::string &key) {
		// Only process entity types that support external properties
		if constexpr (!EntityPropertyTraits<EntityType>::supportsExternalProperties) {
			return false; // Entities without external property support never have external properties
		}

		auto *dataManager = getDataManagerPtr();

		if (!EntityPropertyTraits<EntityType>::hasPropertyEntity(entity)) {
			return false;
		}

		auto storageType = EntityPropertyTraits<EntityType>::getPropertyStorageType(entity);
		auto propertyEntityId = EntityPropertyTraits<EntityType>::getPropertyEntityId(entity);

		if (storageType == PropertyStorageType::PROPERTY_ENTITY) {
			Property property = dataManager->getProperty(propertyEntityId);
			return property.hasPropertyValue(key);
		} else if (storageType == PropertyStorageType::BLOB_ENTITY) { // ZYX_COV_EXCL_LINE
			auto blobManager = getDataManagerPtr()->getBlobManager();
			try {
				std::string serializedData = blobManager->readBlobChain(propertyEntityId);

				std::stringstream ss(serializedData);
				auto properties = deserializeProperties(ss);
				return properties.contains(key);
			} catch (...) {
				return false;
			}
		}

		return false;
	}

	template<typename EntityType>
	size_t PropertyManager::calculateEntityTotalPropertySize(int64_t entityId) {
		// Only process entity types that support properties
		if constexpr (!EntityPropertyTraits<EntityType>::supportsProperties) {
			return 0; // Entities without property support have no property size
		}

		auto *dataManager = getDataManagerPtr();

		auto entity = dataManager->getEntity<EntityType>(entityId);
		if (entity.getId() == 0 || !entity.isActive()) { // ZYX_COV_EXCL_LINE
			return 0;
		}

		// Start with inline properties size
		size_t totalSize = 0;
		for (const auto &[key, value]: EntityPropertyTraits<EntityType>::getProperties(entity)) { // ZYX_COV_EXCL_LINE
			totalSize += key.size();
			totalSize += property_utils::getPropertyValueSize(value);
		}

		// If we have external properties, we need to account for them too
		if constexpr (EntityPropertyTraits<EntityType>::supportsExternalProperties) {
			if (EntityPropertyTraits<EntityType>::hasPropertyEntity(entity)) {
				auto storageType = EntityPropertyTraits<EntityType>::getPropertyStorageType(entity);
				auto propertyEntityId = EntityPropertyTraits<EntityType>::getPropertyEntityId(entity);

				if (storageType == PropertyStorageType::PROPERTY_ENTITY) {
					// Get property entity and calculate its size
					Property property = dataManager->getProperty(propertyEntityId);
					if (property.getId() != 0 && property.isActive()) { // ZYX_COV_EXCL_LINE
						for (const auto &[key, value]: property.getPropertyValues()) {
							totalSize += key.size();
							totalSize += property_utils::getPropertyValueSize(value);
						}
					}
				} else if (storageType == PropertyStorageType::BLOB_ENTITY) { // ZYX_COV_EXCL_LINE
					// Get blob entity and calculate size from its deserialized properties
					auto blobManager = getDataManagerPtr()->getBlobManager();
					try {
						std::string serializedData = blobManager->readBlobChain(propertyEntityId);
						std::stringstream ss(serializedData);
						auto properties = deserializeProperties(ss);

						for (const auto &[key, value]: properties) {
							totalSize += key.size();
							totalSize += property_utils::getPropertyValueSize(value);
						}
					} catch (...) {
						// Ignore read errors for corrupted blob chains
					}
				}
			}
		}

		return totalSize;
	}

	// Template instantiations for the property methods
	// These ensure the templates are instantiated for all entity types

	// storeProperties instantiations
	template void PropertyManager::storeProperties<Node>(Node &entity);
	template void PropertyManager::storeProperties<Edge>(Edge &entity);

	// storePropertiesBatch instantiations
	template std::vector<size_t> PropertyManager::storePropertiesBatch<Node>(std::vector<Node> &entities);
	template std::vector<size_t> PropertyManager::storePropertiesBatch<Edge>(std::vector<Edge> &entities);

	// storePropertiesColumnarBatch instantiations
	template std::vector<size_t> PropertyManager::storePropertiesColumnarBatch<Node>(
			std::vector<Node> &entities, const std::vector<BulkPropertyColumn> &columns);
	template std::vector<size_t> PropertyManager::storePropertiesColumnarBatch<Edge>(
			std::vector<Edge> &entities, const std::vector<BulkPropertyColumn> &columns);

	// cleanupExternalProperties instantiations
	template void PropertyManager::cleanupExternalProperties<Node>(Node &entity);
	template void PropertyManager::cleanupExternalProperties<Edge>(Edge &entity);

	// storePropertiesInPropertyEntity instantiations
	template void
	PropertyManager::storePropertiesInPropertyEntity<Node>(Node &entity,
														   const std::unordered_map<std::string, PropertyValue> &);
	template void
	PropertyManager::storePropertiesInPropertyEntity<Edge>(Edge &entity,
														   const std::unordered_map<std::string, PropertyValue> &);

	// storePropertiesInBlob instantiations
	template void PropertyManager::storePropertiesInBlob<Node>(Node &entity,
															 const std::unordered_map<std::string, PropertyValue> &);
	template void PropertyManager::storePropertiesInBlob<Edge>(Edge &entity,
															 const std::unordered_map<std::string, PropertyValue> &);

	// getEntityProperties instantiations
	template std::unordered_map<std::string, PropertyValue>
	PropertyManager::getEntityProperties<Node>(int64_t entityId);
	template std::unordered_map<std::string, PropertyValue>
	PropertyManager::getEntityProperties<Edge>(int64_t entityId);

	// addEntityProperties instantiations
	template void PropertyManager::addEntityProperties<Node>(int64_t entityId,
															 const std::unordered_map<std::string, PropertyValue> &);
	template void PropertyManager::addEntityProperties<Edge>(int64_t entityId,
															 const std::unordered_map<std::string, PropertyValue> &);

	// removeEntityProperty instantiations
	template void PropertyManager::removeEntityProperty<Node>(int64_t entityId, const std::string &);
	template void PropertyManager::removeEntityProperty<Edge>(int64_t entityId, const std::string &);

	// hasExternalProperty instantiations
	template bool PropertyManager::hasExternalProperty<Node>(const Node &entity, const std::string &);
	template bool PropertyManager::hasExternalProperty<Edge>(const Edge &entity, const std::string &);

	// calculateEntityTotalPropertySize instantiations
	template size_t PropertyManager::calculateEntityTotalPropertySize<Node>(int64_t entityId);
	template size_t PropertyManager::calculateEntityTotalPropertySize<Edge>(int64_t entityId);

} // namespace graph::storage
