/**
 * @file Property.cpp
 * @author Nexepic
 * @date 2025/4/25
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

#include "graph/core/Property.hpp"
#include "graph/storage/IDAllocator.hpp"
#include "graph/utils/Serializer.hpp"

namespace graph {

	Property::Property(const int64_t id, const int64_t entityId, const uint32_t entityType) {
		metadata.id = id;
		metadata.entityId = entityId;
		metadata.entityType = entityType;
		metadata.isActive = true;
	}

	bool Property::hasPropertyValue(const std::string &key) const { return values.contains(key); }

	const std::unordered_map<std::string, PropertyValue> &Property::getPropertyValues() const { return values; }

	void Property::serialize(std::ostream &os) const {
		// Write metadata fields individually
		utils::Serializer::writePOD(os, metadata.id);
		utils::Serializer::writePOD(os, metadata.entityId);
		utils::Serializer::writePOD(os, metadata.entityType);
		utils::Serializer::writePOD(os, metadata.isActive);

		// Write property values
		utils::Serializer::writePOD(os, static_cast<uint32_t>(values.size()));
		for (const auto &[key, value]: values) {
			utils::Serializer::serialize(os, key);
			utils::Serializer::serialize<PropertyValue>(os, value);
		}
	}

	Property Property::deserialize(std::istream &is) {
		Property property;

		// Read metadata fields individually
		property.metadata.id = utils::Serializer::readPOD<int64_t>(is);
		property.metadata.entityId = utils::Serializer::readPOD<int64_t>(is);
		property.metadata.entityType = utils::Serializer::readPOD<uint32_t>(is);
		property.metadata.isActive = utils::Serializer::readPOD<bool>(is);

		// Read property values
		auto propertyCount = utils::Serializer::readPOD<uint32_t>(is);
		for (uint32_t i = 0; i < propertyCount; ++i) {
			std::string key = utils::Serializer::deserialize<std::string>(is);
			PropertyValue value = utils::Serializer::deserialize<PropertyValue>(is);
			property.values[key] = value;
		}

		return property;
	}

	Property Property::deserializeFromBuffer(const char *buf) {
		Property property;

		// memcpy for fixed metadata
		size_t off = 0;
		std::memcpy(&property.metadata.id, buf + off, sizeof(int64_t)); off += sizeof(int64_t);
		std::memcpy(&property.metadata.entityId, buf + off, sizeof(int64_t)); off += sizeof(int64_t);
		std::memcpy(&property.metadata.entityType, buf + off, sizeof(uint32_t)); off += sizeof(uint32_t);
		std::memcpy(&property.metadata.isActive, buf + off, sizeof(bool)); off += sizeof(bool);

		// istream for variable-length property key-value pairs
		struct membuf : std::streambuf {
			membuf(const char *p, size_t s) {
				char *mp = const_cast<char *>(p);
				setg(mp, mp, mp + s);
			}
		};
		membuf mb(buf + off, TOTAL_PROPERTY_SIZE - off);
		std::istream stream(&mb);

		auto propertyCount = utils::Serializer::readPOD<uint32_t>(stream);
		for (uint32_t i = 0; i < propertyCount; ++i) {
			std::string key = utils::Serializer::deserialize<std::string>(stream);
			PropertyValue value = utils::Serializer::deserialize<PropertyValue>(stream);
			property.values[key] = std::move(value);
		}

		return property;
	}

	size_t Property::getSerializedSize() const {
		// Calculate size of all metadata fields
		size_t size = 0;
		size += sizeof(metadata.id); // int64_t
		size += sizeof(metadata.entityId); // int64_t
		size += sizeof(metadata.entityType); // uint32_t
		size += sizeof(metadata.isActive); // bool

		// Size for property count
		size += sizeof(uint32_t); // For property count

		// Calculate size of all property values
		for (const auto &[key, value]: values) {
			// String key (length prefix + string content)
			size += sizeof(uint32_t); // String length prefix
			size += key.size(); // Key string content

			// Property value
			size += property_utils::getPropertyValueSize(value);
		}

		return size;
	}

} // namespace graph
