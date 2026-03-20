/**
 * @file SetOperator.hpp
 * @author Nexepic
 * @date 2025/12/18
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

#include "../PhysicalOperator.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/expressions/ExpressionEvaluationHelper.hpp"
#include "graph/query/expressions/EvaluationContext.hpp"
#include "graph/query/expressions/ExpressionEvaluator.hpp"

namespace graph::query::execution::operators {

	enum class SetActionType { PROPERTY, LABEL, MAP_MERGE };

	struct SetItem {
		SetActionType type; // Distinguish Property vs Label
		std::string variable;
		std::string key; // Property Key OR Label Name
		std::shared_ptr<graph::query::expressions::Expression> expression; // Expression AST (ignored if type == LABEL)

		// Constructor for expression AST
		SetItem(SetActionType t, std::string var, std::string k, std::shared_ptr<graph::query::expressions::Expression> expr)
			: type(t), variable(std::move(var)), key(std::move(k)), expression(std::move(expr)) {}

		// Default constructor
		SetItem() = default;
	};

	class SetOperator : public PhysicalOperator {
	public:
		SetOperator(std::shared_ptr<storage::DataManager> dm, std::unique_ptr<PhysicalOperator> child,
					std::vector<SetItem> items) :
			dm_(std::move(dm)), child_(std::move(child)), items_(std::move(items)) {}

		void open() override {
			if (child_)
				child_->open();
		}

		std::optional<RecordBatch> next() override {
			auto batchOpt = child_->next();
			if (!batchOpt)
				return std::nullopt;

			RecordBatch outputBatch;
			outputBatch.reserve(batchOpt->size());

			for (auto record: *batchOpt) {
				for (const auto &item: items_) {

					// Skip label-only items (no expression evaluation needed)
					if (item.type == SetActionType::PROPERTY && item.expression) {
						// Evaluate the expression to get the value
						PropertyValue value = graph::query::expressions::ExpressionEvaluationHelper::evaluate(
						    item.expression.get(), record);

						// --- Node Logic ---
						if (auto nodeOpt = record.getNode(item.variable)) {
							Node node = *nodeOpt;
							int64_t id = node.getId();

							// Property Update (Read-Modify-Write)
							auto props = dm_->getNodeProperties(id);
							props[item.key] = value;
							dm_->addNodeProperties(id, props);
							node.setProperties(std::move(props));

							record.setNode(item.variable, node);
						}
						// --- Edge Logic ---
						else if (auto edgeOpt = record.getEdge(item.variable)) {
							Edge edge = *edgeOpt;
							int64_t id = edge.getId();

							auto props = dm_->getEdgeProperties(id);
							props[item.key] = value;
							dm_->addEdgeProperties(id, props);
							edge.setProperties(std::move(props));
							record.setEdge(item.variable, edge);
						}
					} else if (item.type == SetActionType::LABEL) {
						// --- Label Update ---
						if (auto nodeOpt = record.getNode(item.variable)) {
							Node node = *nodeOpt;
							[[maybe_unused]] int64_t id = node.getId();

							// Resolve string label to ID
							int64_t newLabelId = dm_->getOrCreateLabelId(item.key);
							node.setLabelId(newLabelId);
							dm_->updateNode(node); // Persist label ID change

							record.setNode(item.variable, node);
						}
					}
				}
				outputBatch.push_back(std::move(record));
			}

			return outputBatch;
		}

		void close() override {
			if (child_)
				child_->close();
		}

		[[nodiscard]] std::vector<std::string> getOutputVariables() const override {
			return child_->getOutputVariables();
		}

		[[nodiscard]] std::string toString() const override {
			return "Set(" + std::to_string(items_.size()) + " items)";
		}

		[[nodiscard]] std::vector<const PhysicalOperator *> getChildren() const override { return {child_.get()}; }

	private:
		std::shared_ptr<storage::DataManager> dm_;
		std::unique_ptr<PhysicalOperator> child_;
		std::vector<SetItem> items_;
	};
} // namespace graph::query::execution::operators