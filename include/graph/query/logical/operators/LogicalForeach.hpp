/**
 * @file LogicalForeach.hpp
 * @brief Logical operator for FOREACH iterative write operations.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once

#include "graph/query/logical/LogicalOperator.hpp"
#include "graph/query/expressions/Expression.hpp"

#include <memory>
#include <string>
#include <vector>

namespace graph::query::logical {

class LogicalForeach : public LogicalOperator {
public:
	LogicalForeach(std::unique_ptr<LogicalOperator> child,
	               std::string iterVar,
	               std::shared_ptr<graph::query::expressions::Expression> listExpr,
	               std::unique_ptr<LogicalOperator> body)
		: child_(std::move(child)), iterVar_(std::move(iterVar)),
		  listExpr_(std::move(listExpr)), body_(std::move(body)) {}

	[[nodiscard]] LogicalOpType getType() const override { return LogicalOpType::LOP_FOREACH; }

	[[nodiscard]] std::vector<LogicalOperator *> getChildren() const override {
		std::vector<LogicalOperator *> children;
		if (child_) children.push_back(child_.get());
		if (body_) children.push_back(body_.get());
		return children;
	}

	[[nodiscard]] std::vector<std::string> getOutputVariables() const override {
		// FOREACH does not export any new variables
		return child_ ? child_->getOutputVariables() : std::vector<std::string>{};
	}

	[[nodiscard]] std::unique_ptr<LogicalOperator> clone() const override {
		return std::make_unique<LogicalForeach>(
			child_ ? child_->clone() : nullptr,
			iterVar_,
			listExpr_,
			body_ ? body_->clone() : nullptr);
	}

	[[nodiscard]] std::string toString() const override {
		return "Foreach(" + iterVar_ + ")";
	}

	void setChild(size_t index, std::unique_ptr<LogicalOperator> child) override {
		if (index == 0) child_ = std::move(child);
		else if (index == 1) body_ = std::move(child);
		else throw std::logic_error("LogicalForeach: invalid child index");
	}

	std::unique_ptr<LogicalOperator> detachChild(size_t index) override {
		if (index == 0) return std::move(child_);
		if (index == 1) return std::move(body_);
		throw std::logic_error("LogicalForeach: invalid child index");
	}

	[[nodiscard]] const std::string &getIterVar() const { return iterVar_; }
	[[nodiscard]] const std::shared_ptr<expressions::Expression> &getListExpr() const { return listExpr_; }
	[[nodiscard]] LogicalOperator *getBody() const { return body_.get(); }
	[[nodiscard]] LogicalOperator *getInput() const { return child_.get(); }

private:
	std::unique_ptr<LogicalOperator> child_;
	std::string iterVar_;
	std::shared_ptr<graph::query::expressions::Expression> listExpr_;
	std::unique_ptr<LogicalOperator> body_;
};

} // namespace graph::query::logical
