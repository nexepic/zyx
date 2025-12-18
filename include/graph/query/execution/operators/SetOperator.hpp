/**
 * @file SetOperator.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/18
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include "../PhysicalOperator.hpp"
#include "graph/storage/data/DataManager.hpp"

namespace graph::query::execution::operators {

	struct SetItem {
		std::string variable;
		std::string key;
		PropertyValue value;
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
				// Apply all updates
				for (const auto &item: items_) {

					// --- Update Node ---
					if (auto nodeOpt = record.getNode(item.variable)) {
						Node node = *nodeOpt;
						int64_t id = node.getId();

						// Read-Modify-Write Pattern
						// 1. Fetch ALL existing properties to prevent overwriting
						auto props = dm_->getNodeProperties(id);

						// 2. Modify in memory
						props[item.key] = item.value;

						// 3. Write FULL map back to storage
						// This ensures PersistenceManager has the complete state
						dm_->addNodeProperties(id, props);

						// 4. Update the record object for downstream operators
						node.setProperties(std::move(props));
						record.setNode(item.variable, node);
					}

					// --- Update Edge ---
					else if (auto edgeOpt = record.getEdge(item.variable)) {
						Edge edge = *edgeOpt;
						int64_t id = edge.getId();

						// Read-Modify-Write for Edge
						auto props = dm_->getEdgeProperties(id);

						props[item.key] = item.value;

						dm_->addEdgeProperties(id, props);

						// Update record object (Assuming Edge has setProperties)
						edge.setProperties(std::move(props));
						record.setEdge(item.variable, edge);
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
