/**
 * @file Node.hpp
 * @author Nexepic
 * @date 2025/2/26
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

#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include "PropertyTypes.hpp"
#include "Types.hpp"
#include "graph/core/Entity.hpp"

namespace graph {

	class Node : public EntityBase<Node> {
	public:
		// Metadata struct to contain fixed node data
		struct Metadata {
			int64_t id = 0;
			int64_t firstOutEdgeId = 0; // First outgoing edge
			int64_t firstInEdgeId = 0; // First incoming edge
			int64_t propertyEntityId = 0;
			uint32_t propertyStorageType = 0;
			bool isActive = true;
			// Padding is implicit
		};

		static constexpr size_t TOTAL_NODE_SIZE = 256;
		static constexpr size_t METADATA_SIZE = offsetof(Metadata, isActive) + sizeof(Metadata::isActive);
		static constexpr size_t LABEL_BUFFER_SIZE = TOTAL_NODE_SIZE - METADATA_SIZE;
		static constexpr uint32_t typeId = toUnderlying(EntityType::Node);

		[[nodiscard]] size_t getSerializedSize() const;

		static constexpr size_t getTotalSize() { return TOTAL_NODE_SIZE; }

		Node() = default;
		Node(int64_t id, const std::string &label);

		// Metadata access for CRTP base class
		[[nodiscard]] const Metadata &getMetadata() const { return metadata; }
		Metadata &getMutableMetadata() { return metadata; }

		// Edge relationship management
		void setFirstOutEdgeId(int64_t edgeId);
		void setFirstInEdgeId(int64_t edgeId);
		[[nodiscard]] int64_t getFirstOutEdgeId() const { return metadata.firstOutEdgeId; }
		[[nodiscard]] int64_t getFirstInEdgeId() const { return metadata.firstInEdgeId; }

		// Property methods
		void setProperties(std::unordered_map<std::string, PropertyValue> props);
		void addProperty(const std::string &key, const PropertyValue &value);
		[[nodiscard]] bool hasProperty(const std::string &key) const;
		[[nodiscard]] PropertyValue getProperty(const std::string &key) const;
		void removeProperty(const std::string &key);
		[[nodiscard]] const std::unordered_map<std::string, PropertyValue> &getProperties() const;
		[[nodiscard]] size_t getTotalPropertySize() const;
		void clearProperties() { properties.clear(); }

		// Property entity
		void setPropertyEntityId(int64_t propertyId, PropertyStorageType storageType = PropertyStorageType::NONE);
		[[nodiscard]] int64_t getPropertyEntityId() const { return metadata.propertyEntityId; }
		[[nodiscard]] PropertyStorageType getPropertyStorageType() const {
			return static_cast<PropertyStorageType>(metadata.propertyStorageType);
		}
		[[nodiscard]] bool hasPropertyEntity() const {
			return getPropertyStorageType() != PropertyStorageType::NONE && metadata.propertyEntityId != 0;
		}

		// Getters for metadata
		[[nodiscard]] std::string getLabel() const;
		[[nodiscard]] bool isActive() const { return metadata.isActive; }

		// Setters for metadata
		void setLabel(const std::string &label);
		void markInactive(bool active = false) { metadata.isActive = active; }

		// Serialization
		void serialize(std::ostream &os) const;
		static Node deserialize(std::istream &is);

	private:
		// Fixed-size metadata structure
		Metadata metadata;

		// Fixed-size buffer for label
		char labelBuffer[LABEL_BUFFER_SIZE] = {0};

		// Variable-sized structures (not included in the 128-byte alignment)
		std::unordered_map<std::string, PropertyValue> properties;
	};

} // namespace graph
