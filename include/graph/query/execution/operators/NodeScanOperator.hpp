/**
 * @file NodeScanOperator.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/10
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include "../PhysicalOperator.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/query/indexes/IndexManager.hpp"
#include <memory>
#include <vector>
#include <string>

namespace graph::query::execution::operators {

	class NodeScanOperator : public PhysicalOperator {
	public:
		/**
		 * @brief Constructs a NodeScanOperator.
		 *
		 * @param dataManager Storage access.
		 * @param indexManager Index access.
		 * @param variableName The Cypher variable (e.g., "n").
		 * @param label The node label filter (optional).
		 * @param key The property key for index lookup (optional).
		 * @param value The property value for index lookup (optional).
		 */
		NodeScanOperator(std::shared_ptr<storage::DataManager> dataManager,
						 std::shared_ptr<indexes::IndexManager> indexManager,
						 std::string variableName,
						 std::string label = "",
						 std::string key = "",
						 PropertyValue value = PropertyValue());

		~NodeScanOperator() override = default;

		void open() override;
		std::optional<RecordBatch> next() override;
		void close() override;
		[[nodiscard]] std::vector<std::string> getOutputVariables() const override;

	private:
		// Dependencies
		std::shared_ptr<storage::DataManager> dataManager_;
		std::shared_ptr<indexes::IndexManager> indexManager_;

		// Configuration
		std::string variableName_;
		std::string label_;

		// Pushdown Predicates (for Property Index optimization)
		std::string key_;
		PropertyValue value_;

		// Execution State
		std::vector<int64_t> candidateIds_;
		size_t currentIndex_ = 0;
		bool useIndex_ = false;
		static constexpr size_t BATCH_SIZE = 1000;

		// Helper to load remaining IDs via Full Scan
		void prepareFullScan();
	};

} // namespace graph::query::execution::operators