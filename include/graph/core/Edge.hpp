/**
 * @file Edge.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/2/26
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once
#include <ostream>
#include <string>
#include <unordered_map>
#include "PropertyTypes.hpp"
#include "Types.hpp"
#include "graph/core/Entity.hpp"
#include "graph/utils/Serializer.hpp"

namespace graph {

	class Edge : public EntityBase<Edge> {
	public:
		// Metadata struct to contain fixed edge data
		struct Metadata {
			int64_t id = 0;
			int64_t sourceNodeId = 0; // Source node of this edge
			int64_t targetNodeId = 0; // Target node of this edge
			int64_t nextOutEdgeId = 0; // Next outgoing edge from source
			int64_t prevOutEdgeId = 0; // Previous outgoing edge from source
			int64_t nextInEdgeId = 0; // Next incoming edge to target
			int64_t prevInEdgeId = 0; // Previous incoming edge to target
			int64_t propertyEntityId = 0; // Property entity ID (if any)
			uint32_t propertyStorageType = 0; // How properties are stored
			bool isActive = true;
			// Padding is implicit
		};

		static constexpr size_t TOTAL_EDGE_SIZE = 256;
		static constexpr size_t METADATA_SIZE = offsetof(Metadata, isActive) + sizeof(Metadata::isActive);
		static constexpr size_t LABEL_BUFFER_SIZE = TOTAL_EDGE_SIZE - METADATA_SIZE;
		static constexpr uint32_t typeId = toUnderlying(EntityType::Edge);

		[[nodiscard]] size_t getSerializedSize() const;

		static constexpr size_t getTotalSize() { return TOTAL_EDGE_SIZE; }

		Edge() = default;
		Edge(int64_t id, int64_t sourceId, int64_t targetId, const std::string &label);

		// Metadata access for CRTP base class
		[[nodiscard]] const Metadata &getMetadata() const { return metadata; }
		Metadata &getMutableMetadata() { return metadata; }

		[[nodiscard]] std::string getLabel() const;

		// Node relationship getters
		[[nodiscard]] int64_t getSourceNodeId() const { return metadata.sourceNodeId; }
		[[nodiscard]] int64_t getTargetNodeId() const { return metadata.targetNodeId; }

		// Node relationship setters
		void setSourceNodeId(int64_t sourceId) { metadata.sourceNodeId = sourceId; }
		void setTargetNodeId(int64_t targetId) { metadata.targetNodeId = targetId; }

		// Edge linking for traversal
		[[nodiscard]] int64_t getNextOutEdgeId() const { return metadata.nextOutEdgeId; }
		[[nodiscard]] int64_t getPrevOutEdgeId() const { return metadata.prevOutEdgeId; }
		[[nodiscard]] int64_t getNextInEdgeId() const { return metadata.nextInEdgeId; }
		[[nodiscard]] int64_t getPrevInEdgeId() const { return metadata.prevInEdgeId; }

		void setNextOutEdgeId(int64_t edgeId) { metadata.nextOutEdgeId = edgeId; }
		void setPrevOutEdgeId(int64_t edgeId) { metadata.prevOutEdgeId = edgeId; }
		void setNextInEdgeId(int64_t edgeId) { metadata.nextInEdgeId = edgeId; }
		void setPrevInEdgeId(int64_t edgeId) { metadata.prevInEdgeId = edgeId; }

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

		// Type management
		[[nodiscard]] std::string getType() const;
		void setType(const std::string &type);

		// Active state
		[[nodiscard]] bool isActive() const { return metadata.isActive; }
		void markInactive(bool active = false) { metadata.isActive = active; }

		// Serialization
		void serialize(std::ostream &os) const;
		static Edge deserialize(std::istream &is);

		void setLabel(const std::string &label);

	private:
		// Fixed-size metadata structure
		Metadata metadata;

		// Fixed-size buffer for label
		char labelBuffer[LABEL_BUFFER_SIZE] = {0};

		// Variable-sized structures (not included in the fixed-size structure)
		std::unordered_map<std::string, PropertyValue> properties;
	};

} // namespace graph
