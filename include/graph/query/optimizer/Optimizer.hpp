/**
 * @file Optimizer.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/12
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <memory>

#include "graph/query/execution/ScanConfigs.hpp"
#include "graph/query/indexes/IndexManager.hpp"
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
		[[nodiscard]] execution::NodeScanConfig optimizeNodeScan(
			const std::string& variable,
			const std::string& label,
			const std::string& key,
			const PropertyValue& value
		) const {
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