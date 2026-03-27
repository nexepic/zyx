/**
 * @file ScanConfigs.hpp
 * @author Nexepic
 * @date 2025/12/12
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

#include <string>
#include <variant>
#include <vector>
#include "graph/core/Property.hpp"

namespace graph::query::execution {

	enum class ScanType {
		FULL_SCAN, // Iterate all segments
		LABEL_SCAN, // Use Label Index
		PROPERTY_SCAN // Use Property Index
	};

	struct NodeScanConfig {
		ScanType type = ScanType::FULL_SCAN;
		std::string variable;
		std::vector<std::string> labels;

		// For Property Index
		std::string indexKey;
		PropertyValue indexValue;

		// Backward-compat: get first label or empty string
		[[nodiscard]] std::string label() const { return labels.empty() ? "" : labels[0]; }

		// Visualization helper
		std::string toString() const {
			std::string labelStr;
			for (size_t i = 0; i < labels.size(); ++i) {
				if (i > 0) labelStr += ":";
				labelStr += labels[i];
			}
			if (type == ScanType::PROPERTY_SCAN)
				return "IndexScan(" + labelStr + ", " + indexKey + "=" + indexValue.toString() + ")";
			if (type == ScanType::LABEL_SCAN)
				return "LabelScan(" + labelStr + ")";
			return "FullScan";
		}
	};

} // namespace graph::query::execution
