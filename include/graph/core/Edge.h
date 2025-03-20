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
#include "Node.h"
#include <string>
#include <unordered_map>
#include <ostream>
#include <istream>

namespace graph {

	class Edge {
	public:
		using PropertyValue = Node::PropertyValue;

		Edge() = default;
		Edge(uint64_t id, uint64_t from, uint64_t to, const std::string& label);

		void addProperty(const std::string& key, const PropertyValue& value);
		const PropertyValue& getProperty(const std::string& key) const;

		uint64_t getFrom() const;
		uint64_t getTo() const;
		uint64_t getId() const;
		const std::string& getLabel() const;

		void serialize(std::ostream& os) const;
		static Edge deserialize(std::istream& is);

		static constexpr uint8_t typeId = 1;

	private:
		uint64_t id;
		uint64_t fromNode;
		uint64_t toNode;
		std::string label;
		std::unordered_map<std::string, PropertyValue> properties;
	};

} // namespace graph