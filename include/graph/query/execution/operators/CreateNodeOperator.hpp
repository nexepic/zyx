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

		void setChild(std::unique_ptr<PhysicalOperator> child) {
			child_ = std::move(child);
		}

		void open() override { executed_ = false; }

		std::optional<RecordBatch> next() override {
			if (child_) {
				auto batchOpt = child_->next();
				if (!batchOpt) return std::nullopt;

				RecordBatch& inputBatch = *batchOpt;
				RecordBatch outputBatch;
				outputBatch.reserve(inputBatch.size());

				for (auto& record : inputBatch) {
					Node newNode = performCreate();

					Record newRecord = record;
					newRecord.setNode(variable_, newNode);
					outputBatch.push_back(std::move(newRecord));
				}
				return outputBatch;
			}

			if (executed_) return std::nullopt;

			Node newNode = performCreate();

			Record record;
			record.setNode(variable_, newNode);
			RecordBatch batch;
			batch.push_back(std::move(record));

			executed_ = true;
			return batch;
		}

		void close() override { if (child_) child_->close(); }

		[[nodiscard]] std::vector<std::string> getOutputVariables() const override {
			auto vars = child_ ? child_->getOutputVariables() : std::vector<std::string>{};
			vars.push_back(variable_);
			return vars;
		}

		[[nodiscard]] std::string toString() const override {
			return "CreateNode(var=" + variable_ + ", label=" + label_ + ")";
		}

		[[nodiscard]] std::vector<const PhysicalOperator*> getChildren() const override {
			if (child_) return {child_.get()};
			return {};
		}

	private:
		std::shared_ptr<storage::DataManager> dm_;
		std::unique_ptr<PhysicalOperator> child_;
		std::string variable_;
		std::string label_;
		std::unordered_map<std::string, PropertyValue> props_;
		bool executed_ = false;

		Node performCreate() const {
			Node newNode(0, label_);
			dm_->addNode(newNode);
			if (!props_.empty()) {
				dm_->addNodeProperties(newNode.getId(), props_);
			}
			newNode.setProperties(props_);
			return newNode;
		}
	};
}