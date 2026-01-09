/**
 * @file IndexMeta.hpp
 * @author Nexepic
 * @date 2025/12/18
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

#include <sstream>
#include <string>

namespace graph::query::indexes {
	struct IndexMetadata {
		std::string name;
		std::string entityType; // node/edge
		std::string indexType; // label/property
		std::string label;
		std::string property;

		std::string toString() const { return entityType + "|" + indexType + "|" + label + "|" + property; }

		static IndexMetadata fromString(const std::string &name, const std::string &str) {
			std::stringstream ss(str);
			std::string segment;
			std::vector<std::string> parts;
			while (std::getline(ss, segment, '|')) {
				parts.push_back(segment);
			}
			if (!str.empty() && str.back() == '|') {
				parts.push_back("");
			}

			if (parts.size() < 4) {
				// Log to stderr to see if this happens
				std::cerr << "[Error] Corrupt Metadata: " << str << " size: " << parts.size() << std::endl;
				return {name, "unknown", "unknown", "", ""};
			}
			return {name, parts[0], parts[1], parts[2], parts[3]};
		}
	};
} // namespace graph::query::indexes
