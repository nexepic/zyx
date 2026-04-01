/**
 * @file IndexPushdownRule.hpp
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

#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/storage/indexes/IndexManager.hpp"

namespace graph::query::optimizer::rules {

	class IndexPushdownRule {
	public:
		explicit IndexPushdownRule(std::shared_ptr<indexes::IndexManager> indexManager) :
			indexManager_(std::move(indexManager)) {}

		/**
		 * @brief Decides the optimal scan configuration based on available indexes.
		 *
		 * Supports equality, range, and composite scan strategies.
		 */
		[[nodiscard]] execution::NodeScanConfig apply(
				const std::string &variable, const std::vector<std::string> &labels,
				const std::string &key, const PropertyValue &value,
				const std::vector<logical::RangePredicate> &rangePredicates = {},
				const std::optional<logical::CompositeEqualityPredicate> &compositeEquality = std::nullopt) const {
			execution::NodeScanConfig config;
			config.variable = variable;
			config.labels = labels;

			std::string firstLabel = labels.empty() ? "" : labels[0];

			bool hasPropIndex = !key.empty() && indexManager_->hasPropertyIndex("node", key);
			bool hasLabelIndex = !firstLabel.empty() && indexManager_->hasLabelIndex("node");

			// Priority 1: Composite index (most selective when matching 2+ fields)
			if (compositeEquality && compositeEquality->keys.size() >= 2 &&
				indexManager_->hasCompositeIndex("node", compositeEquality->keys)) {
				config.type = execution::ScanType::COMPOSITE_SCAN;
				config.compositeKeys = compositeEquality->keys;
				config.compositeValues = compositeEquality->values;
				return config;
			}

			// Priority 2: Property equality (O(log N))
			if (hasPropIndex) {
				config.type = execution::ScanType::PROPERTY_SCAN;
				config.indexKey = key;
				config.indexValue = value;
				return config;
			}

			// Priority 3: Range scan with index
			for (const auto &rp : rangePredicates) {
				if (indexManager_->hasPropertyIndex("node", rp.key)) {
					config.type = execution::ScanType::RANGE_SCAN;
					config.indexKey = rp.key;
					config.rangeMin = rp.minValue;
					config.rangeMax = rp.maxValue;
					config.minInclusive = rp.minInclusive;
					config.maxInclusive = rp.maxInclusive;
					return config;
				}
			}

			// Priority 4: Label scan
			if (hasLabelIndex) {
				config.type = execution::ScanType::LABEL_SCAN;
				return config;
			}

			// Fallback: Full Scan
			config.type = execution::ScanType::FULL_SCAN;
			return config;
		}

	private:
		std::shared_ptr<indexes::IndexManager> indexManager_;
	};

} // namespace graph::query::optimizer::rules
