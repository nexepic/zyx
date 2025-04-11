/**
 * @file PropertyStorage.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/3/27
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/storage/PropertyStorage.h"
#include <sstream>
#include "graph/utils/ChecksumUtils.h"

namespace graph::property_storage {

	void PropertyStorage::serialize(std::ostream &os,
									const std::unordered_map<std::string, PropertyValue> &properties) {
		// Write the number of properties
		utils::Serializer::writePOD(os, static_cast<uint32_t>(properties.size()));

		// Write each property key-value pair
		for (const auto &[key, value]: properties) {
			utils::Serializer::writeString(os, key);
			utils::Serializer::writePropertyValue(os, value);
		}
	}

	std::unordered_map<std::string, PropertyValue> PropertyStorage::deserialize(std::istream &is) {
		std::unordered_map<std::string, PropertyValue> properties;

		// Read the number of properties
		auto propertyCount = utils::Serializer::readPOD<uint32_t>(is);

		// Read each property key-value pair
		for (uint32_t i = 0; i < propertyCount; ++i) {
			std::string key = utils::Serializer::readString(is);
			PropertyValue value = utils::Serializer::readPropertyValue(is);
			properties[key] = value;
		}

		return properties;
	}

	PropertyReference PropertyStorage::storeProperties(const std::shared_ptr<std::fstream> &file, uint64_t entityId,
													   uint8_t entityType, uint64_t entityOffset,
													   const std::unordered_map<std::string, PropertyValue> &properties,
													   uint64_t propertySegmentHead, storage::DataManager &dataManager,
													   const PropertyReference &existingReference) {

		// If there are no properties, return an empty reference
		if (properties.empty()) {
			return PropertyReference{};
		}

		// Calculate the size of the serialized properties
		uint32_t dataSize = calculateSerializedSize(properties);

		// If the data size exceeds the inline property limit, store in a Blob
		if (dataSize > storage::MAX_SEGMENT_PROPERTY_SIZE) {
			// Serialize to a string stream buffer
			std::stringstream ss;
			serialize(ss, properties);
			std::string serializedData = ss.str();

			// Store the serialized data in a Blob
			uint64_t blobId = dataManager.storeBlob(serializedData, "text");

			// Return a Blob reference
			return PropertyReference{PropertyReference::StorageType::BLOB, blobId,
									 static_cast<uint32_t>(serializedData.size())};
		}

		// Check if we can reuse the existing property entry
		if (existingReference.type == PropertyReference::StorageType::SEGMENT) {
			// We can reuse the existing entry
			uint64_t entryPosition = existingReference.reference;

			// Seek to the entry position
			file->seekp(static_cast<std::streamoff>(entryPosition));

			// Write the property entry header
			storage::PropertyEntryHeader entryHeader;
			entryHeader.entity_id = entityId;
			entryHeader.entity_type = entityType;
			entryHeader.entity_offset = entityOffset; // Record entity offset
			entryHeader.data_size = dataSize;
			entryHeader.property_count = static_cast<uint32_t>(properties.size());

			file->write(reinterpret_cast<const char *>(&entryHeader), sizeof(entryHeader));

			// Serialize the properties
			std::stringstream ss;
			serialize(ss, properties);
			std::string serializedData = ss.str();
			file->write(serializedData.c_str(), static_cast<std::streamsize>(serializedData.size()));

			// Return the existing reference with potentially updated size
			return PropertyReference{PropertyReference::StorageType::SEGMENT, entryPosition, dataSize};
		}

		// Calculate the total size required for the property entry
		uint32_t totalSize = dataSize + sizeof(storage::PropertyEntryHeader);

		// Find available space in the property segment
		auto [segmentOffset, entryOffset] = findAvailableSpace(file, propertySegmentHead, totalSize);

		// Allocate a new property segment if needed
		if (segmentOffset == 0) {
			segmentOffset = allocatePropertySegment(file);
			entryOffset = 0;

			// Update the property segment head
			if (propertySegmentHead == 0) {
				// Update the FileHeader in DataManager
				dataManager.getFileHeaderRef().property_segment_head = segmentOffset;
			} else {
				// Find the last property segment and update the link
				uint64_t currentOffset = propertySegmentHead;
				storage::PropertySegmentHeader header;

				while (true) {
					file->seekg(static_cast<std::streamoff>(currentOffset));
					file->read(reinterpret_cast<char *>(&header), sizeof(header));

					if (header.next_segment_offset == 0) {
						// Update the next segment offset
						header.next_segment_offset = segmentOffset;
						file->seekp(static_cast<std::streamoff>(currentOffset));
						file->write(reinterpret_cast<const char *>(&header), sizeof(header));

						// Update the prev pointer of the new segment
						storage::PropertySegmentHeader newHeader;
						file->seekg(static_cast<std::streamoff>(segmentOffset));
						file->read(reinterpret_cast<char *>(&newHeader), sizeof(newHeader));
						newHeader.prev_segment_offset = currentOffset;
						file->seekp(static_cast<std::streamoff>(segmentOffset));
						file->write(reinterpret_cast<const char *>(&newHeader), sizeof(newHeader));
						break;
					}

					currentOffset = header.next_segment_offset;
				}
			}
		}

		// Calculate the position of the property entry
		uint64_t entryPosition = segmentOffset + sizeof(storage::PropertySegmentHeader) + entryOffset;
		file->seekp(static_cast<std::streamoff>(entryPosition));

		// Write the property entry header with entity offset
		storage::PropertyEntryHeader entryHeader;
		entryHeader.entity_id = entityId;
		entryHeader.entity_type = entityType;
		entryHeader.entity_offset = entityOffset; // Store entity offset
		entryHeader.data_size = dataSize;
		entryHeader.property_count = static_cast<uint32_t>(properties.size());

		file->write(reinterpret_cast<const char *>(&entryHeader), sizeof(entryHeader));

		// Serialize the properties
		std::stringstream ss;
		serialize(ss, properties);
		std::string serializedData = ss.str();
		file->write(serializedData.c_str(), static_cast<std::streamsize>(serializedData.size()));

		// Update the segment header
		storage::PropertySegmentHeader segmentHeader;
		file->seekg(static_cast<std::streamoff>(segmentOffset));
		file->read(reinterpret_cast<char *>(&segmentHeader), sizeof(segmentHeader));

		segmentHeader.used = entryOffset + totalSize;

		// Calculate the CRC of the segment header
		segmentHeader.segment_crc =
				utils::calculateCrc(&segmentHeader, offsetof(storage::PropertySegmentHeader, segment_crc));

		file->seekp(static_cast<std::streamoff>(segmentOffset));
		file->write(reinterpret_cast<const char *>(&segmentHeader), sizeof(segmentHeader));

		// Return a reference to the property entry
		return PropertyReference{PropertyReference::StorageType::SEGMENT, entryPosition, dataSize};
	}

	std::unordered_map<std::string, PropertyValue>
	PropertyStorage::loadProperties(const std::shared_ptr<std::ifstream> &file, const PropertyReference &reference) {

		// If there are no properties, return an empty map
		if (reference.type == PropertyReference::StorageType::NONE) {
			return {};
		}

		// Load properties based on the reference type
		if (reference.type == PropertyReference::StorageType::SEGMENT) {
			// Load from property segment
			file->seekg(static_cast<std::streamoff>(reference.reference));

			// Read property entry header
			storage::PropertyEntryHeader entryHeader;
			file->read(reinterpret_cast<char *>(&entryHeader), sizeof(entryHeader));

			// Read property data
			return deserialize(*file);
		}
		if (reference.type == PropertyReference::StorageType::BLOB) {
			// Load from Blob storage (integration with your existing Blob loading logic needed)
			// std::string blobData = blobStore->getBlob(reference.reference);
			// std::stringstream ss(blobData);
			// return deserialize(ss);

			// Note: Full implementation requires integration with Blob loading logic
			return {};
		}

		// Unknown reference type
		return {};
	}

	uint64_t PropertyStorage::allocatePropertySegment(const std::shared_ptr<std::fstream> &file, uint32_t capacity) {

		// Position to the end of the file
		file->seekp(0, std::ios::end);
		uint64_t segmentOffset = file->tellp();

		// Create a new property segment header
		storage::PropertySegmentHeader header;
		header.capacity = capacity;
		header.used = 0;
		header.next_segment_offset = 0;
		header.prev_segment_offset = 0;
		header.segment_crc = utils::calculateCrc(&header, offsetof(storage::PropertySegmentHeader, segment_crc));

		// Write the segment header
		file->write(reinterpret_cast<const char *>(&header), sizeof(header));

		// Allocate space
		std::vector<char> emptySpace(capacity, 0);
		file->write(emptySpace.data(), static_cast<std::streamsize>(capacity));

		return segmentOffset;
	}

	uint32_t
	PropertyStorage::calculateSerializedSize(const std::unordered_map<std::string, PropertyValue> &properties) {

		// Calculate the size of the serialized property set
		uint32_t size = sizeof(uint32_t); // Number of properties

		for (const auto &[key, value]: properties) {
			size += sizeof(uint32_t) + key.size(); // Key length + key content
			size += property_utils::getPropertyValueSize(value); // Value size
		}

		return size;
	}

	std::pair<uint64_t, uint32_t> PropertyStorage::findAvailableSpace(const std::shared_ptr<std::fstream> &file,
																	  uint64_t segmentHead, uint32_t requiredSize) {

		// If there are no segments, return 0 indicating a new segment needs to be allocated
		if (segmentHead == 0) {
			return {0, 0};
		}

		// Traverse all property segments to find enough space
		uint64_t currentOffset = segmentHead;

		while (currentOffset != 0) {
			// Read the segment header
			storage::PropertySegmentHeader header;
			file->seekg(static_cast<std::streamoff>(currentOffset));
			file->read(reinterpret_cast<char *>(&header), sizeof(header));

			// Check if there is enough space
			if (header.capacity - header.used >= requiredSize) {
				return {currentOffset, header.used};
			}

			// Move to the next segment
			currentOffset = header.next_segment_offset;
		}

		// No enough space found
		return {0, 0};
	}

} // namespace graph::property_storage
