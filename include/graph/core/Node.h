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
#include <vector>
#include <unordered_map>
#include "Types.h"

namespace graph {

	class Node {
	public:
		Node() = default;
		Node(uint64_t id, const std::string& label);

		void addOutEdge(uint64_t edgeId);
		void addInEdge(uint64_t edgeId);

		const std::vector<uint64_t>& getOutEdges() const;
		const std::vector<uint64_t>& getInEdges() const;

		void addProperties(const std::string& key, const PropertyValue& value);
		const std::unordered_map<std::string, PropertyValue>& getProperties() const;

		const std::string& getLabel() const;
		uint64_t getId() const;

		void serialize(std::ostream& os) const;
		static Node deserialize(std::istream& is);

		static constexpr uint8_t typeId = 0;

		void setId(uint64_t id) { this->id = id; }

	private:
		uint64_t id;
		std::string label;
		std::unordered_map<std::string, PropertyValue> properties;
		std::vector<uint64_t> inEdges;
		std::vector<uint64_t> outEdges;
	};

} // namespace graph