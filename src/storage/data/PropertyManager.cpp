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
#include <sstream>
#include "graph/core/Blob.hpp"
#include "graph/core/Edge.hpp"
#include "graph/core/EntityPropertyTraits.hpp"
#include "graph/core/Index.hpp"
#include "graph/core/Node.hpp"
#include "graph/core/Property.hpp"
#include "graph/core/State.hpp"
#include "graph/storage/data/BlobManager.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/utils/Serializer.hpp"

namespace graph::storage {

	PropertyManager::PropertyManager(const std::shared_ptr<DataManager> &dataManager,
									 std::shared_ptr<DeletionManager> deletionManager) :
		BaseEntityManager(dataManager, std::move(deletionManager)) {}

	int64_t PropertyManager::doAllocateId() {
		const auto dataManager = dataManager_.lock();
		return dataManager->getIdAllocator()->allocateId(Property::typeId);
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
			size += property_utils::getPropertyValueSize(value); // Value size
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

		auto dataManager = dataManager_.lock();
		if (!dataManager)
			return;

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
	void PropertyManager::cleanupExternalProperties(EntityType &entity) {
		// Only process entity types that support external properties
		if constexpr (!EntityPropertyTraits<EntityType>::supportsExternalProperties) {
			return; // Do nothing for entity types that don't support external properties
		}

		auto dataManager = dataManager_.lock();
		if (!dataManager)
			return;

		if (!EntityPropertyTraits<EntityType>::hasPropertyEntity(entity)) {
			return;
		}

		int64_t propertyId = EntityPropertyTraits<EntityType>::getPropertyEntityId(entity);
		auto storageType = EntityPropertyTraits<EntityType>::getPropertyStorageType(entity);

		if (storageType == PropertyStorageType::BLOB_ENTITY) {
			// Delete the entire blob chain
			if (auto blobManager = dataManager->getBlobManager()) {
				blobManager->deleteBlobChain(propertyId);
			}
		} else if (storageType == PropertyStorageType::PROPERTY_ENTITY) {
			Property property = dataManager->getProperty(propertyId);
			if (property.getId() != 0 && property.isActive()) {
				dataManager->deleteProperty(property);
			}
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

		auto dataManager = dataManager_.lock();
		if (!dataManager)
			return;

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

		auto dataManager = dataManager_.lock();
		if (!dataManager)
			return;

		auto blobManager = dataManager->getBlobManager();
		if (!blobManager) {
			throw std::runtime_error("BlobManager not initialized");
		}

		// This function's responsibility is simple: create a new Blob chain.
		// It assumes any previous external storage has already been cleaned up by the caller (storeProperties).

		// Serialize properties to a string.
		std::stringstream ss;
		serializeProperties(ss, properties);
		std::string serializedData = ss.str();

		if (serializedData.empty()) {
			return; // Nothing to store.
		}

		try {
			// Create a new blob chain for the serialized properties.
			auto blobChain = blobManager->createBlobChain(entity.getId(), EntityType::typeId, serializedData);

			if (blobChain.empty()) {
				throw std::runtime_error("Failed to create blob chain");
			}

			// Update the entity to reference the head of the new blob chain.
			EntityPropertyTraits<EntityType>::setPropertyEntityId(entity, blobChain.front().getId(),
																  PropertyStorageType::BLOB_ENTITY);

			// Clear the properties from the in-memory entity object, as they are now stored externally.
			EntityPropertyTraits<EntityType>::clearProperties(entity);

		} catch (const std::runtime_error &e) {
			throw std::runtime_error(std::string("Failed to store properties in blob: ") + e.what());
		}
	}

	std::unordered_map<std::string, PropertyValue> PropertyManager::getPropertiesFromBlob(int64_t blobId) const {
		auto dataManager = dataManager_.lock();
		if (!dataManager)
			return {};

		try {
			// Read data from blob chain
			auto blobManager = dataManager->getBlobManager();
			if (!blobManager) {
				return {};
			}

			std::string serializedData = blobManager->readBlobChain(blobId);

			// Deserialize properties
			std::stringstream ss(serializedData);
			return deserializeProperties(ss);

		} catch (const std::exception &e) {
			// Log error but return empty map to avoid crashing
			std::cerr << "Error reading blob chain: " << e.what() << std::endl;
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

		auto dataManager = dataManager_.lock();
		if (!dataManager)
			return allProperties;

		// Get the entity
		auto entity = dataManager->getEntity<EntityType>(entityId);

		// If entity doesn't exist or is inactive, return empty map
		if (entity.getId() == 0 || !entity.isActive()) {
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
					if (property.getId() != 0 && property.isActive()) {
						// Verify ownership
						if (property.getEntityId() == entityId && property.getEntityType() == EntityType::typeId) {
							// Merge properties
							const auto &externalProps = property.getPropertyValues();
							for (const auto &[key, value]: externalProps) {
								allProperties[key] = value;
							}

							// Add property to cache
							dataManager->addToCache(property);
						}
					}
				} else if (storageType == PropertyStorageType::BLOB_ENTITY) {
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

		auto dataManager = dataManager_.lock();
		if (!dataManager)
			return;

		// Get the entity
		auto entity = dataManager->getEntity<EntityType>(entityId);

		// Check if entity exists and is active
		if (entity.getId() == 0 || !entity.isActive()) {
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

		auto dataManager = dataManager_.lock();
		if (!dataManager)
			return;

		// Get the entity.
		auto entity = dataManager->getEntity<EntityType>(entityId);

		// Check if the entity exists and is active.
		if (entity.getId() == 0 || !entity.isActive()) {
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
					if (property.getId() != 0 && property.isActive() && property.hasPropertyValue(key)) {
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
				} else if (storageType == PropertyStorageType::BLOB_ENTITY) {
					// Get properties from blob chain to modify them.
					auto properties = getPropertiesFromBlob(propertyEntityId);
					auto it = properties.find(key);
					if (it != properties.end()) {
						properties.erase(it);
						propertyWasRemoved = true;

						auto blobManager = dataManager->getBlobManager();
						if (!blobManager)
							return; // Should not happen

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
							if (!newBlobChain.empty()) {
								EntityPropertyTraits<EntityType>::setPropertyEntityId(
										entity, newBlobChain.front().getId(), PropertyStorageType::BLOB_ENTITY);
							} else {
								// Failed to create a new chain, clear the reference.
								EntityPropertyTraits<EntityType>::setPropertyEntityId(entity, 0,
																					  PropertyStorageType::NONE);
							}
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

		auto dataManager = dataManager_.lock();
		if (!dataManager)
			return false;

		if (!EntityPropertyTraits<EntityType>::hasPropertyEntity(entity)) {
			return false;
		}

		auto storageType = EntityPropertyTraits<EntityType>::getPropertyStorageType(entity);
		auto propertyEntityId = EntityPropertyTraits<EntityType>::getPropertyEntityId(entity);

		if (storageType == PropertyStorageType::PROPERTY_ENTITY) {
			Property property = dataManager->getProperty(propertyEntityId);
			if (property.getId() != 0 && property.isActive()) {
				return property.hasPropertyValue(key);
			}
		} else if (storageType == PropertyStorageType::BLOB_ENTITY) {
			if (auto blobManager = dataManager->getBlobManager()) {
				try {
					std::string serializedData = blobManager->readBlobChain(propertyEntityId);
					if (serializedData.empty())
						return false;

					std::stringstream ss(serializedData);
					auto properties = deserializeProperties(ss);
					return properties.contains(key);
				} catch (...) {
					return false;
				}
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

		auto dataManager = dataManager_.lock();
		if (!dataManager)
			return 0;

		auto entity = dataManager->getEntity<EntityType>(entityId);
		if (entity.getId() == 0 || !entity.isActive()) {
			return 0;
		}

		// Start with inline properties size
		size_t totalSize = 0;
		for (const auto &[key, value]: EntityPropertyTraits<EntityType>::getProperties(entity)) {
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
					if (property.getId() != 0 && property.isActive()) {
						for (const auto &[key, value]: property.getPropertyValues()) {
							totalSize += key.size();
							totalSize += property_utils::getPropertyValueSize(value);
						}
					}
				} else if (storageType == PropertyStorageType::BLOB_ENTITY) {
					// Get blob entity and calculate size from its deserialized properties
					if (auto blobManager = dataManager->getBlobManager()) {
						try {
							std::string serializedData = blobManager->readBlobChain(propertyEntityId);
							if (!serializedData.empty()) {
								std::stringstream ss(serializedData);
								auto properties = deserializeProperties(ss);

								for (const auto &[key, value]: properties) {
									totalSize += key.size();
									totalSize += property_utils::getPropertyValueSize(value);
								}
							}
						} catch (...) {
							// Ignore read errors
						}
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

	// cleanupExternalProperties instantiations
	template void PropertyManager::cleanupExternalProperties<Node>(Node &entity);
	template void PropertyManager::cleanupExternalProperties<Edge>(Edge &entity);
	template void PropertyManager::cleanupExternalProperties<Property>(Property &entity);
	template void PropertyManager::cleanupExternalProperties<Blob>(Blob &entity);
	template void PropertyManager::cleanupExternalProperties<Index>(Index &entity);
	template void PropertyManager::cleanupExternalProperties<State>(State &entity);

	// storePropertiesInPropertyEntity instantiations
	template void
	PropertyManager::storePropertiesInPropertyEntity<Node>(Node &entity,
														   const std::unordered_map<std::string, PropertyValue> &);
	template void
	PropertyManager::storePropertiesInPropertyEntity<Edge>(Edge &entity,
														   const std::unordered_map<std::string, PropertyValue> &);
	template void
	PropertyManager::storePropertiesInPropertyEntity<Property>(Property &entity,
															   const std::unordered_map<std::string, PropertyValue> &);
	template void
	PropertyManager::storePropertiesInPropertyEntity<Blob>(Blob &entity,
														   const std::unordered_map<std::string, PropertyValue> &);
	template void
	PropertyManager::storePropertiesInPropertyEntity<Index>(Index &entity,
															const std::unordered_map<std::string, PropertyValue> &);
	template void
	PropertyManager::storePropertiesInPropertyEntity<State>(State &entity,
															const std::unordered_map<std::string, PropertyValue> &);

	// storePropertiesInBlob instantiations
	template void PropertyManager::storePropertiesInBlob<Node>(Node &entity,
															   const std::unordered_map<std::string, PropertyValue> &);
	template void PropertyManager::storePropertiesInBlob<Edge>(Edge &entity,
															   const std::unordered_map<std::string, PropertyValue> &);
	template void
	PropertyManager::storePropertiesInBlob<Property>(Property &entity,
													 const std::unordered_map<std::string, PropertyValue> &);
	template void PropertyManager::storePropertiesInBlob<Blob>(Blob &entity,
															   const std::unordered_map<std::string, PropertyValue> &);
	template void PropertyManager::storePropertiesInBlob<Index>(Index &entity,
																const std::unordered_map<std::string, PropertyValue> &);
	template void PropertyManager::storePropertiesInBlob<State>(State &entity,
																const std::unordered_map<std::string, PropertyValue> &);

	// getEntityProperties instantiations
	template std::unordered_map<std::string, PropertyValue>
	PropertyManager::getEntityProperties<Node>(int64_t entityId);
	template std::unordered_map<std::string, PropertyValue>
	PropertyManager::getEntityProperties<Edge>(int64_t entityId);
	template std::unordered_map<std::string, PropertyValue>
	PropertyManager::getEntityProperties<Property>(int64_t entityId);
	template std::unordered_map<std::string, PropertyValue>
	PropertyManager::getEntityProperties<Blob>(int64_t entityId);
	template std::unordered_map<std::string, PropertyValue>
	PropertyManager::getEntityProperties<Index>(int64_t entityId);
	template std::unordered_map<std::string, PropertyValue>
	PropertyManager::getEntityProperties<State>(int64_t entityId);

	// addEntityProperties instantiations
	template void PropertyManager::addEntityProperties<Node>(int64_t entityId,
															 const std::unordered_map<std::string, PropertyValue> &);
	template void PropertyManager::addEntityProperties<Edge>(int64_t entityId,
															 const std::unordered_map<std::string, PropertyValue> &);
	template void
	PropertyManager::addEntityProperties<Property>(int64_t entityId,
												   const std::unordered_map<std::string, PropertyValue> &);
	template void PropertyManager::addEntityProperties<Blob>(int64_t entityId,
															 const std::unordered_map<std::string, PropertyValue> &);
	template void PropertyManager::addEntityProperties<Index>(int64_t entityId,
															  const std::unordered_map<std::string, PropertyValue> &);
	template void PropertyManager::addEntityProperties<State>(int64_t entityId,
															  const std::unordered_map<std::string, PropertyValue> &);

	// removeEntityProperty instantiations
	template void PropertyManager::removeEntityProperty<Node>(int64_t entityId, const std::string &);
	template void PropertyManager::removeEntityProperty<Edge>(int64_t entityId, const std::string &);
	template void PropertyManager::removeEntityProperty<Property>(int64_t entityId, const std::string &);
	template void PropertyManager::removeEntityProperty<Blob>(int64_t entityId, const std::string &);
	template void PropertyManager::removeEntityProperty<Index>(int64_t entityId, const std::string &);
	template void PropertyManager::removeEntityProperty<State>(int64_t entityId, const std::string &);

	// hasExternalProperty instantiations
	template bool PropertyManager::hasExternalProperty<Node>(const Node &entity, const std::string &);
	template bool PropertyManager::hasExternalProperty<Edge>(const Edge &entity, const std::string &);
	template bool PropertyManager::hasExternalProperty<Property>(const Property &entity, const std::string &);
	template bool PropertyManager::hasExternalProperty<Blob>(const Blob &entity, const std::string &);
	template bool PropertyManager::hasExternalProperty<Index>(const Index &entity, const std::string &);
	template bool PropertyManager::hasExternalProperty<State>(const State &entity, const std::string &);

	// calculateEntityTotalPropertySize instantiations
	template size_t PropertyManager::calculateEntityTotalPropertySize<Node>(int64_t entityId);
	template size_t PropertyManager::calculateEntityTotalPropertySize<Edge>(int64_t entityId);
	template size_t PropertyManager::calculateEntityTotalPropertySize<Property>(int64_t entityId);
	template size_t PropertyManager::calculateEntityTotalPropertySize<Blob>(int64_t entityId);
	template size_t PropertyManager::calculateEntityTotalPropertySize<Index>(int64_t entityId);
	template size_t PropertyManager::calculateEntityTotalPropertySize<State>(int64_t entityId);

} // namespace graph::storage
