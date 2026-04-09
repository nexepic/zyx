/**
 * @file LogicalLoadCsv.hpp
 * @brief Logical operator for LOAD CSV statement.
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

class LogicalLoadCsv : public LogicalOperator {
public:
	LogicalLoadCsv(std::unique_ptr<LogicalOperator> child,
	               std::shared_ptr<graph::query::expressions::Expression> urlExpr,
	               std::string rowVariable,
	               bool withHeaders = false,
	               std::string fieldTerminator = ",")
		: child_(std::move(child)), urlExpr_(std::move(urlExpr)),
		  rowVariable_(std::move(rowVariable)), withHeaders_(withHeaders),
		  fieldTerminator_(std::move(fieldTerminator)) {}

	[[nodiscard]] LogicalOpType getType() const override { return LogicalOpType::LOP_LOAD_CSV; }

	[[nodiscard]] std::vector<LogicalOperator *> getChildren() const override {
		if (child_) return {child_.get()};
		return {};
	}

	[[nodiscard]] std::vector<std::string> getOutputVariables() const override {
		auto vars = child_ ? child_->getOutputVariables() : std::vector<std::string>{};
		vars.push_back(rowVariable_);
		return vars;
	}

	[[nodiscard]] std::unique_ptr<LogicalOperator> clone() const override {
		return std::make_unique<LogicalLoadCsv>(
			child_ ? child_->clone() : nullptr,
			urlExpr_, rowVariable_, withHeaders_, fieldTerminator_);
	}

	[[nodiscard]] std::string toString() const override {
		return "LoadCsv(" + rowVariable_ + (withHeaders_ ? ", WITH HEADERS" : "") + ")";
	}

	void setChild(size_t index, std::unique_ptr<LogicalOperator> child) override {
		if (index == 0) child_ = std::move(child);
		else throw std::logic_error("LogicalLoadCsv: invalid child index");
	}

	std::unique_ptr<LogicalOperator> detachChild(size_t index) override {
		if (index == 0) return std::move(child_);
		throw std::logic_error("LogicalLoadCsv: invalid child index");
	}

	[[nodiscard]] const std::shared_ptr<expressions::Expression> &getUrlExpr() const { return urlExpr_; }
	[[nodiscard]] const std::string &getRowVariable() const { return rowVariable_; }
	[[nodiscard]] bool isWithHeaders() const { return withHeaders_; }
	[[nodiscard]] const std::string &getFieldTerminator() const { return fieldTerminator_; }

private:
	std::unique_ptr<LogicalOperator> child_;
	std::shared_ptr<graph::query::expressions::Expression> urlExpr_;
	std::string rowVariable_;
	bool withHeaders_;
	std::string fieldTerminator_;
};

} // namespace graph::query::logical
