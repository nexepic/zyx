/**
 * @file ScanConfigs.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/12
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <string>
#include <variant>
#include "graph/core/Property.hpp"

namespace graph::query::execution {

	enum class ScanType {
		FULL_SCAN,      // Iterate all segments
		LABEL_SCAN,     // Use Label Index
		PROPERTY_SCAN   // Use Property Index
	};

	struct NodeScanConfig {
		ScanType type = ScanType::FULL_SCAN;
		std::string variable;
		std::string label;

		// For Property Index
		std::string indexKey;
		PropertyValue indexValue;

		// Visualization helper
		std::string toString() const {
			if (type == ScanType::PROPERTY_SCAN)
				return "IndexScan(" + label + ", " + indexKey + "=" + indexValue.toString() + ")";
			if (type == ScanType::LABEL_SCAN)
				return "LabelScan(" + label + ")";
			return "FullScan";
		}
	};

}