/**
 * @file ExplainOperator.hpp
 * @brief Physical operator for EXPLAIN — prints the logical plan without executing.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once

#include <sstream>
#include <string>
#include "../PhysicalOperator.hpp"
#include "graph/query/logical/LogicalOperator.hpp"
#include "graph/query/logical/LogicalPlanPrinter.hpp"

namespace graph::query::execution::operators {

	class ExplainOperator : public PhysicalOperator {
	public:
		explicit ExplainOperator(const logical::LogicalOperator *logicalPlan) {
			// Traverse the plan tree and collect rows at construction time
			if (logicalPlan) {
				collectPlanRows(logicalPlan, 0);
			}
		}

		void open() override { cursor_ = 0; }

		std::optional<RecordBatch> next() override {
			if (cursor_ > 0)
				return std::nullopt;
			cursor_++;

			RecordBatch batch;
			for (const auto &[op, details] : planRows_) {
				Record r;
				r.setValue("operator", PropertyValue(op));
				r.setValue("details", PropertyValue(details));
				batch.push_back(std::move(r));
			}

			if (batch.empty())
				return std::nullopt;
			return batch;
		}

		void close() override {}

		[[nodiscard]] std::vector<std::string> getOutputVariables() const override {
			return {"operator", "details"};
		}

		[[nodiscard]] std::string toString() const override { return "Explain"; }

	private:
		std::vector<std::pair<std::string, std::string>> planRows_;
		size_t cursor_ = 0;

		void collectPlanRows(const logical::LogicalOperator *node, int depth) {
			std::string indent;
			for (int i = 0; i < depth; ++i) indent += "  ";

			std::string opName = indent + logical::toString(node->getType());
			std::string details = node->toString();

			planRows_.emplace_back(std::move(opName), std::move(details));

			for (auto *child : node->getChildren()) {
				if (child) collectPlanRows(child, depth + 1);
			}
		}
	};

} // namespace graph::query::execution::operators
