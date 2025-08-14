/**
 * @file PropertyManager.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/7/24
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include "graph/core/PropertyTypes.hpp"
#include "graph/storage/data/BaseEntityManager.hpp"

namespace graph::storage {

	class DataManager;

	/**
	 * Manages property-related operations across all entity types
	 */
	class PropertyManager : public BaseEntityManager<Property> {
	public:
		PropertyManager(const std::shared_ptr<DataManager> &dataManager,
						std::shared_ptr<DeletionManager> deletionManager);

		// Serialization methods
		[[nodiscard]] static uint32_t
		calculateSerializedSize(const std::unordered_map<std::string, PropertyValue> &properties);
		static void serializeProperties(std::ostream &os,
										const std::unordered_map<std::string, PropertyValue> &properties);
		static std::unordered_map<std::string, PropertyValue> deserializeProperties(std::istream &is);

		// Property storage strategies
		template<typename EntityType>
		void storeProperties(EntityType &entity);

		template<typename EntityType>
		void cleanupExternalProperties(EntityType &entity);

		template<typename EntityType>
		void storePropertiesInPropertyEntity(EntityType &entity,
											 const std::unordered_map<std::string, PropertyValue> &properties);

		template<typename EntityType>
		void storePropertiesInBlob(EntityType &entity,
								   const std::unordered_map<std::string, PropertyValue> &properties);

		// Property access
		template<typename EntityType>
		std::unordered_map<std::string, PropertyValue> getEntityProperties(int64_t entityId);

		[[nodiscard]] std::unordered_map<std::string, PropertyValue> getPropertiesFromBlob(int64_t blobId) const;

		// Property operations
		template<typename EntityType>
		void addEntityProperties(int64_t entityId, const std::unordered_map<std::string, PropertyValue> &properties);

		template<typename EntityType>
		void removeEntityProperty(int64_t entityId, const std::string &key);

		template<typename EntityType>
		bool hasExternalProperty(const EntityType &entity, const std::string &key);

		template<typename EntityType>
		size_t calculateEntityTotalPropertySize(int64_t entityId);

	protected:
		int64_t doAllocateId() override;

		void doRemove(Property &property) override;
	};

} // namespace graph::storage
