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
#include <string>
#include <vector>
#include "graph/storage/indexes/IndexManager.hpp"

namespace graph::query::optimizer::rules {

	class IndexPushdownRule {
	public:
		explicit IndexPushdownRule(std::shared_ptr<indexes::IndexManager> indexManager) :
			indexManager_(std::move(indexManager)) {}

		/**
		 * @brief Decides the optimal scan configuration based on available indexes.
		 *
		 * @param variable The query variable (e.g., "n").
		 * @param label The label filter (e.g., "User").
		 * @param key The property key (e.g., "age").
		 * @param value The property value (e.g., 25).
		 * @return execution::NodeScanConfig The optimized configuration.
		 */
		[[nodiscard]] execution::NodeScanConfig apply(const std::string &variable, const std::vector<std::string> &labels,
													  const std::string &key, const PropertyValue &value) const {
			execution::NodeScanConfig config;
			config.variable = variable;
			config.labels = labels;

			// --- Optimization Logic (Moved from QueryPlanner) ---
			// Use first label for index pushdown (label indexes are per-label)
			std::string firstLabel = labels.empty() ? "" : labels[0];

			bool hasPropIndex = !key.empty() && indexManager_->hasPropertyIndex("node", key);
			bool hasLabelIndex = !firstLabel.empty() && indexManager_->hasLabelIndex("node");

			// Heuristic 1: Property Index is usually most selective (O(log N))
			if (hasPropIndex) {
				config.type = execution::ScanType::PROPERTY_SCAN;
				config.indexKey = key;
				config.indexValue = value;
			}
			// Heuristic 2: Label Index reduces search space (O(N_label))
			else if (hasLabelIndex) {
				config.type = execution::ScanType::LABEL_SCAN;
			}
			// Fallback: Full Scan (O(N_total))
			else {
				config.type = execution::ScanType::FULL_SCAN;
			}

			return config;
		}

	private:
		std::shared_ptr<indexes::IndexManager> indexManager_;
	};

} // namespace graph::query::optimizer::rules
