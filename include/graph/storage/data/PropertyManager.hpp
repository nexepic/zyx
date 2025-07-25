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
#include "graph/core/PropertyValue.hpp"

namespace graph::storage {

	class DataManager;

	/**
	 * Manages property-related operations across all entity types
	 */
	class PropertyManager {
	public:
		explicit PropertyManager(std::shared_ptr<DataManager> dataManager);

		// Serialization methods
		uint32_t calculateSerializedSize(const std::unordered_map<std::string, PropertyValue> &properties) const;
		void serializeProperties(std::ostream &os,
								 const std::unordered_map<std::string, PropertyValue> &properties) const;
		std::unordered_map<std::string, PropertyValue> deserializeProperties(std::istream &is) const;

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

		std::unordered_map<std::string, PropertyValue> getPropertiesFromBlob(int64_t blobId);

		// Property operations
		template<typename EntityType>
		void addEntityProperties(int64_t entityId, const std::unordered_map<std::string, PropertyValue> &properties);

		template<typename EntityType>
		void removeEntityProperty(int64_t entityId, const std::string &key);

		template<typename EntityType>
		bool hasExternalProperty(const EntityType &entity, const std::string &key);

		template<typename EntityType>
		size_t calculateEntityTotalPropertySize(int64_t entityId);

	private:
		std::weak_ptr<DataManager> dataManager_;
		static constexpr uint32_t PROPERTY_ENTITY_OVERHEAD = 100;
		static constexpr uint32_t MAX_SEGMENT_PROPERTY_SIZE = 4096; // Maximum size for property entity storage
	};

} // namespace graph::storage
