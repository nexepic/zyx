/**
 * @file ForeachOperator.hpp
 * @brief Physical operator for FOREACH iterative write operations.
 *
 * For each input row, evaluates a list expression and executes the body
 * operator chain once per element. Output rows are unchanged from input.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once

#include "graph/query/execution/PhysicalOperator.hpp"
#include "graph/query/execution/operators/RecordInjectorOperator.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/expressions/ExpressionEvaluationHelper.hpp"

#include <memory>
#include <string>

namespace graph::query::execution::operators {

class ForeachOperator : public PhysicalOperator {
public:
	ForeachOperator(std::unique_ptr<PhysicalOperator> child,
	                std::string iterVar,
	                std::shared_ptr<graph::query::expressions::Expression> listExpr,
	                std::unique_ptr<PhysicalOperator> body,
	                RecordInjectorOperator *injector)
		: child_(std::move(child)), iterVar_(std::move(iterVar)),
		  listExpr_(std::move(listExpr)), body_(std::move(body)),
		  injector_(injector) {}

	void open() override {
		if (child_) child_->open();
	}

	std::optional<RecordBatch> next() override {
		RecordBatch inputBatch;

		if (child_) {
			auto batch = child_->next();
			if (!batch) return std::nullopt;
			inputBatch = std::move(*batch);
		} else {
			// Standalone FOREACH: generate a single empty seed record
			if (seeded_) return std::nullopt;
			seeded_ = true;
			inputBatch.emplace_back();
		}

		for (auto &record : inputBatch) {
			auto listVal = expressions::ExpressionEvaluationHelper::evaluate(
				listExpr_.get(), record, nullptr);

			if (listVal.getType() != graph::PropertyType::LIST) {
				continue;
			}

			const auto &list = listVal.getList();

			for (const auto &elem : list) {
				Record bodyRecord = record;
				bodyRecord.setValue(iterVar_, elem);

				if (body_ && injector_) {
					injector_->setRecord(bodyRecord);
					body_->open();
					while (auto bodyBatch = body_->next()) {
						// discard - body is for side effects only
					}
					body_->close();
				}
			}
		}

		return inputBatch;
	}

	void close() override {
		if (child_) child_->close();
	}

	[[nodiscard]] std::vector<std::string> getOutputVariables() const override {
		return child_ ? child_->getOutputVariables() : std::vector<std::string>{};
	}

	[[nodiscard]] std::string toString() const override {
		return "ForeachOperator(" + iterVar_ + ")";
	}

	[[nodiscard]] std::vector<const PhysicalOperator *> getChildren() const override {
		std::vector<const PhysicalOperator *> children;
		if (child_) children.push_back(child_.get());
		if (body_) children.push_back(body_.get());
		return children;
	}

private:
	std::unique_ptr<PhysicalOperator> child_;
	std::string iterVar_;
	std::shared_ptr<graph::query::expressions::Expression> listExpr_;
	std::unique_ptr<PhysicalOperator> body_;
	bool seeded_ = false;
	RecordInjectorOperator *injector_;
};

} // namespace graph::query::execution::operators
