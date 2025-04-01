/**
 * @file PropertyStorage.h
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/3/27
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once
#include "DataManager.h"

#include <fstream>
#include <string>
#include <unordered_map>
#include "graph/core/PropertyValue.h"
#include "graph/storage/StorageHeaders.h"
#include "graph/utils/Serializer.h"

namespace graph::property_storage {

	class PropertyStorage {
	public:
		// Serialize properties to output stream
		static void serialize(std::ostream& os, const std::unordered_map<std::string, PropertyValue>& properties);

		// Deserialize properties from input stream
		static std::unordered_map<std::string, PropertyValue> deserialize(std::istream& is);

		// Store properties in a property segment
		static PropertyReference storeProperties(
		    const std::shared_ptr<std::fstream>& file,
		    uint64_t entityId,
		    uint8_t entityType,
		    const std::unordered_map<std::string, PropertyValue>& properties,
		    uint64_t propertySegmentHead,
		    storage::DataManager& dataManager);

		// Load properties from a property segment
		static std::unordered_map<std::string, PropertyValue> loadProperties(const std::shared_ptr<std::ifstream> &file,
		    const PropertyReference& reference);

		// Allocate a new property segment
		static uint64_t allocatePropertySegment(const std::shared_ptr<std::fstream> &file,
		    uint32_t capacity = storage::PROPERTY_SEGMENT_SIZE);

	private:
		// Calculate the serialized size of properties
		static uint32_t calculateSerializedSize(
		    const std::unordered_map<std::string, PropertyValue>& properties);

		// Find available space in a property segment
		static std::pair<uint64_t, uint32_t> findAvailableSpace(const std::shared_ptr<std::fstream> &file,
		    uint64_t segmentHead,
		    uint32_t requiredSize);

		std::unique_ptr<storage::DataManager> dataManager;
	};

} // namespace graph::property_storage