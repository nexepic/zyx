/**
 * @file Optimizer.hpp
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

#include "graph/query/execution/ScanConfigs.hpp"
#include "graph/storage/indexes/IndexManager.hpp"
#include "rules/IndexPushdownRule.hpp"

namespace graph::query::optimizer {

	class Optimizer {
	public:
		explicit Optimizer(std::shared_ptr<indexes::IndexManager> indexManager) {
			// Initialize rules
			indexPushdownRule_ = std::make_unique<rules::IndexPushdownRule>(indexManager);
		}

		/**
		 * @brief Resolves the best strategy for scanning nodes.
		 */
		[[nodiscard]] execution::NodeScanConfig optimizeNodeScan(const std::string &variable, const std::string &label,
																 const std::string &key,
																 const PropertyValue &value) const {
			// Delegate to the specific rule
			return indexPushdownRule_->apply(variable, label, key, value);
		}

		// Future methods:
		// optimizeJoin(...)
		// optimizeOrder(...)

	private:
		std::unique_ptr<rules::IndexPushdownRule> indexPushdownRule_;
	};

} // namespace graph::query::optimizer
