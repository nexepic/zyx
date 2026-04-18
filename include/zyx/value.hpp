/**
 * @file value.hpp
 * @author Nexepic
 * @date 2025/12/16
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

#include <memory>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace zyx {

	// Forward declarations
	struct Node;
	struct Edge;
	struct ValueList;
	struct ValueMap;

	// The universal value type.
	// Uses shared_ptr for complex types to allow recursive definition and cheap copying.
	using Value = std::variant<std::monostate, // Null
							   bool, int64_t, double, std::string, std::shared_ptr<Node>, std::shared_ptr<Edge>,
							   std::vector<float>, // Compact numeric vector (e.g. embeddings)
							   std::vector<std::string>, // Generic list/string-list interop
							   std::shared_ptr<ValueList>, // Heterogeneous list
							   std::shared_ptr<ValueMap>   // Key-value map
							   >;

	// Public Node Representation
	struct Node {
		int64_t id;
		std::string label; // First label (backward compat)
		std::vector<std::string> labels; // All labels
		std::unordered_map<std::string, Value> properties;
	};

	// Public Edge Representation
	struct Edge {
		int64_t id;
		int64_t sourceId;
		int64_t targetId;
		std::string type; // Relationship Type
		std::unordered_map<std::string, Value> properties;
	};

	// Heterogeneous list of Values
	struct ValueList {
		std::vector<Value> elements;
	};

	// String-keyed map of Values
	struct ValueMap {
		std::unordered_map<std::string, Value> entries;
	};

} // namespace zyx
