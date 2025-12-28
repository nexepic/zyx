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

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>
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
		NodeScanOperator(std::shared_ptr<storage::DataManager> dm, std::shared_ptr<indexes::IndexManager> im,
						 NodeScanConfig config) : dm_(std::move(dm)), im_(std::move(im)), config_(std::move(config)) {}

		void open() override {
			currentIdx_ = 0;
			candidateIds_.clear();

			// 1. Determine Candidate IDs
			switch (config_.type) {
				case ScanType::PROPERTY_SCAN:
					// Implicit Filtering: The Index returns ONLY IDs that match key=value.
					candidateIds_ = im_->findNodeIdsByProperty(config_.indexKey, config_.indexValue);
					break;

				case ScanType::LABEL_SCAN:
					// Implicit Filtering: The Index returns IDs with this label.
					candidateIds_ = im_->findNodeIdsByLabel(config_.label);
					break;

				case ScanType::FULL_SCAN:
				default:
					// No Filtering: Get all IDs.
					int64_t maxId = dm_->getIdAllocator()->getCurrentMaxNodeId();
					candidateIds_.reserve(maxId);
					for (int64_t i = 1; i <= maxId; ++i)
						candidateIds_.push_back(i);
					break;
			}
		}

		std::optional<RecordBatch> next() override {
			if (currentIdx_ >= candidateIds_.size())
				return std::nullopt;

			RecordBatch batch;
			batch.reserve(DEFAULT_BATCH_SIZE);

			while (batch.size() < DEFAULT_BATCH_SIZE && currentIdx_ < candidateIds_.size()) {
				int64_t id = candidateIds_[currentIdx_++];

				// 2. Load Header
				Node node = dm_->getNode(id);
				if (!node.isActive())
					continue;

				// 3. Label Double-Check
				// Necessary for FULL_SCAN mode, or if Index is slightly stale.
				if (!config_.label.empty() && node.getLabel() != config_.label) {
					continue;
				}

				// 4. Hydrate Properties
				// MANDATORY: Subsequent FilterOperators depend on this data.
				auto props = dm_->getNodeProperties(id);
				node.setProperties(std::move(props));

				Record r;
				r.setNode(config_.variable, std::move(node));
				batch.push_back(std::move(r));
			}

			if (batch.empty() && currentIdx_ >= candidateIds_.size())
				return std::nullopt;
			return batch;
		}

		void close() override { candidateIds_.clear(); }

		[[nodiscard]] std::vector<std::string> getOutputVariables() const override { return {config_.variable}; }

		[[nodiscard]] std::string toString() const override { return config_.toString(); }

	private:
		std::shared_ptr<storage::DataManager> dm_;
		std::shared_ptr<indexes::IndexManager> im_;
		NodeScanConfig config_;

		std::vector<int64_t> candidateIds_;
		size_t currentIdx_ = 0;
	};
} // namespace graph::query::execution::operators
