/**
 * @file Edge.h
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
#include "PropertyValue.h"
#include "Types.h"

#include <graph/utils/Serializer.h>

namespace graph {

	class Edge {
	public:
		Edge() = default;
		Edge(int64_t id, int64_t from, int64_t to, std::string label);

		// Property methods
		void addProperty(const std::string &key, const PropertyValue &value);
		[[nodiscard]] bool hasProperty(const std::string &key) const;
		[[nodiscard]] PropertyValue getProperty(const std::string &key) const;
		void removeProperty(const std::string &key);
		[[nodiscard]] const std::unordered_map<std::string, PropertyValue>& getProperties() const;
		[[nodiscard]] size_t getTotalPropertySize() const;
		void clearProperties() { properties.clear(); }

		void setPropertyReference(const PropertyReference& ref);
		[[nodiscard]] const PropertyReference& getPropertyReference() const;

		[[nodiscard]] int64_t getFromNodeId() const;
		[[nodiscard]] int64_t getToNodeId() const;
		[[nodiscard]] int64_t getId() const;
		[[nodiscard]] const std::string& getLabel() const;

		[[nodiscard]] bool hasTemporaryId() const;

		void setPermanentId(int64_t permanentId);

		void serialize(std::ostream& os) const;
		static Edge deserialize(std::istream& is);

		static constexpr uint8_t typeId = 1;

		void setId(uint64_t id) { this->id = id; }

		[[nodiscard]] bool isActive() const { return isActive_; }
		void markInactive(bool active = false) { isActive_ = active; }

	private:
		int64_t id{};
		int64_t fromNodeId{};
		int64_t toNodeId{};
		std::string label;

		PropertyReference propertyRef;

		std::unordered_map<std::string, PropertyValue> properties;

		static constexpr size_t MAX_TOTAL_PROPERTY_SIZE = 512 * 1024; // 512KB total property limit per edge

		bool isActive_ = true;
	};

} // namespace graph