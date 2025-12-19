/**
 * @file IndexMeta.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/18
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
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

		static IndexMetadata fromString(const std::string& name, const std::string& str) {
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
