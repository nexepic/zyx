/**
 * @file Node.h
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/2/26
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include "PropertyValue.h"
#include "Types.h"

namespace graph {

	class alignas(128) Node {
	public:
		Node() = default;
		Node(int64_t id, std::string label);

		void addOutEdge(uint64_t edgeId);
		void addInEdge(uint64_t edgeId);

		[[nodiscard]] const std::vector<uint64_t>& getOutEdges() const;
		[[nodiscard]] const std::vector<uint64_t>& getInEdges() const;

		// Property methods
		void addProperty(const std::string &key, const PropertyValue &value);
		[[nodiscard]] bool hasProperty(const std::string &key) const;
		[[nodiscard]] PropertyValue getProperty(const std::string &key) const;
		void removeProperty(const std::string &key);
		[[nodiscard]] const std::unordered_map<std::string, PropertyValue>& getProperties() const;
		[[nodiscard]] size_t getTotalPropertySize() const;
		void clearProperties() { properties.clear(); }

		// Property entity
		void setPropertyEntityId(int64_t propertyId, PropertyStorageType storageType = PropertyStorageType::NONE);
		int64_t getPropertyEntityId() const;
		PropertyStorageType getPropertyStorageType() const;
		bool hasPropertyEntity() const;

		[[nodiscard]] const std::string& getLabel() const;
		[[nodiscard]] int64_t getId() const;

		// Check if this node has a temporary ID
		[[nodiscard]] bool hasTemporaryId() const;

		// Set permanent ID (replaces temporary ID)
		void setPermanentId(int64_t permanentId);

		void serialize(std::ostream& os) const;
		static Node deserialize(std::istream& is);

		static constexpr uint32_t typeId = 0;

		void setId(int64_t id) { this->id = id; }

		[[nodiscard]] bool isActive() const { return isActive_; }
		void markInactive(bool active = false) { isActive_ = active; }

		void setInEdges(const std::vector<uint64_t>& edges) {
			inEdges = edges;
		}

		void setOutEdges(const std::vector<uint64_t>& edges) {
			outEdges = edges;
		}

	private:
		int64_t id{};
		std::string label;

        std::unordered_map<std::string, PropertyValue> properties;
		PropertyStorageType propertyStorageType = PropertyStorageType::NONE;
		int64_t propertyEntityId = 0;

		std::vector<uint64_t> inEdges;
		std::vector<uint64_t> outEdges;

		static constexpr size_t MAX_TOTAL_PROPERTY_SIZE = 512 * 1024; // 512KB total property limit per node

		bool isActive_ = true;
	};

} // namespace graph