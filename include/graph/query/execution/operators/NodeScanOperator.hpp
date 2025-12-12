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
#include "../ScanConfigs.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/indexes/IndexManager.hpp"

namespace graph::query::execution::operators {

	/**
	 * @brief A pure source operator.
	 * It does NOT perform property filtering (except implicit filtering via Index lookup).
	 * It ALWAYS hydrates the node properties.
	 */
	class NodeScanOperator : public PhysicalOperator {
	public:
		NodeScanOperator(std::shared_ptr<storage::DataManager> dm,
						 std::shared_ptr<indexes::IndexManager> im,
						 NodeScanConfig config)
			: dm_(std::move(dm)), im_(std::move(im)), config_(std::move(config)) {}

		void open() override;
		std::optional<RecordBatch> next() override;
		void close() override { candidateIds_.clear(); }

		[[nodiscard]] std::vector<std::string> getOutputVariables() const override {
			return {config_.variable};
		}

		[[nodiscard]] std::string toString() const override {
			return config_.toString();
		}

	private:
		std::shared_ptr<storage::DataManager> dm_;
		std::shared_ptr<indexes::IndexManager> im_;
		NodeScanConfig config_;

		std::vector<int64_t> candidateIds_;
		size_t currentIdx_ = 0;
	};
}