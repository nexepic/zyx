/**
 * @file CreateNodeOperator.hpp
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

namespace graph::query::execution::operators {

	/**
	 * @class CreateNodeOperator
	 * @brief Operator that performs a write operation to create a node.
	 *        It acts as a source (if root) or a pass-through pipe.
	 */
	class CreateNodeOperator : public PhysicalOperator {
	public:
		CreateNodeOperator(std::shared_ptr<storage::DataManager> dm,
						   std::string variable,
						   std::string label,
						   std::unordered_map<std::string, PropertyValue> props)
			: dm_(std::move(dm)), variable_(std::move(variable)),
			  label_(std::move(label)), props_(std::move(props)) {}

		void open() override { executed_ = false; }

		std::optional<RecordBatch> next() override {
			if (executed_) return std::nullopt;

			// 1. Write to Storage (Side Effect)
			// ID 0 tells DataManager to allocate a new ID
			Node newNode(0, label_);
			dm_->addNode(newNode);

			if (!props_.empty()) {
				dm_->addNodeProperties(newNode.getId(), props_);
			}

			// 2. Return the created node as a Record
			Record record;
			record.setNode(variable_, newNode);

			RecordBatch batch;
			batch.push_back(std::move(record));

			executed_ = true;
			return batch;
		}

		void close() override {}

		[[nodiscard]] std::vector<std::string> getOutputVariables() const override {
			return {variable_};
		}

	private:
		std::shared_ptr<storage::DataManager> dm_;
		std::string variable_;
		std::string label_;
		std::unordered_map<std::string, PropertyValue> props_;
		bool executed_ = false;
	};
}