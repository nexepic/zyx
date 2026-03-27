/**
 * @file ConstraintMeta.hpp
 * @author Nexepic
 * @date 2026/3/27
 *
 * @copyright Copyright (c) 2026 Nexepic
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

#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace graph::storage::constraints {

struct ConstraintMetadata {
	std::string name;
	std::string entityType;     // "node" or "edge"
	std::string constraintType; // "unique", "not_null", "property_type", "node_key"
	std::string label;
	std::string properties;     // comma-separated for composite keys
	std::string options;        // e.g., "INTEGER" for property_type

	std::string toString() const {
		return entityType + "|" + constraintType + "|" + label + "|" + properties + "|" + options;
	}

	static ConstraintMetadata fromString(const std::string &name, const std::string &str) {
		std::stringstream ss(str);
		std::string segment;
		std::vector<std::string> parts;
		while (std::getline(ss, segment, '|')) {
			parts.push_back(segment);
		}
		// Handle trailing '|' which produces empty last element
		if (!str.empty() && str.back() == '|') {
			parts.push_back("");
		}

		if (parts.size() < 5) {
			std::cerr << "[Error] Corrupt Constraint Metadata: " << str
			          << " size: " << parts.size() << std::endl;
			return {name, "unknown", "unknown", "", "", ""};
		}
		return {name, parts[0], parts[1], parts[2], parts[3], parts[4]};
	}
};

} // namespace graph::storage::constraints
