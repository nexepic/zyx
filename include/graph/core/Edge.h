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
#include "Types.h"
#include <string>
#include <unordered_map>
#include <ostream>
#include <istream>

namespace graph {

	class Edge {
	public:
		Edge() = default;
		Edge(uint64_t id, uint64_t from, uint64_t to, const std::string& label);

		void addProperty(const std::string& key, const PropertyValue& value);
		const PropertyValue& getProperty(const std::string& key) const;

		uint64_t getFromNodeId() const;
		uint64_t getToNodeId() const;
		uint64_t getId() const;
		const std::string& getLabel() const;

		void serialize(std::ostream& os) const;
		static Edge deserialize(std::istream& is);

		static constexpr uint8_t typeId = 1;

		void setId(uint64_t id) { this->id = id; }

	private:
		uint64_t id;
		uint64_t fromNodeId;
		uint64_t toNodeId;
		std::string label;
		std::unordered_map<std::string, PropertyValue> properties;
	};

} // namespace graph