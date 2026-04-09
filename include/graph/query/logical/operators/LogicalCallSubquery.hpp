/**
 * @file LogicalCallSubquery.hpp
 * @brief Logical operator for CALL { ... } inline subqueries.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once

#include "graph/query/logical/LogicalOperator.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace graph::query::logical {

class LogicalCallSubquery : public LogicalOperator {
public:
	LogicalCallSubquery(std::unique_ptr<LogicalOperator> input,
	                    std::unique_ptr<LogicalOperator> subquery,
	                    std::vector<std::string> importedVars,
	                    std::vector<std::string> returnedVars,
	                    bool inTransactions = false,
	                    int64_t batchSize = 0)
		: input_(std::move(input)), subquery_(std::move(subquery)),
		  importedVars_(std::move(importedVars)), returnedVars_(std::move(returnedVars)),
		  inTransactions_(inTransactions), batchSize_(batchSize) {}

	[[nodiscard]] LogicalOpType getType() const override { return LogicalOpType::LOP_CALL_SUBQUERY; }

	[[nodiscard]] std::vector<LogicalOperator *> getChildren() const override {
		std::vector<LogicalOperator *> children;
		if (input_) children.push_back(input_.get());
		if (subquery_) children.push_back(subquery_.get());
		return children;
	}

	[[nodiscard]] std::vector<std::string> getOutputVariables() const override {
		// Output = input vars + returned vars from subquery
		auto vars = input_ ? input_->getOutputVariables() : std::vector<std::string>{};
		vars.insert(vars.end(), returnedVars_.begin(), returnedVars_.end());
		return vars;
	}

	[[nodiscard]] std::unique_ptr<LogicalOperator> clone() const override {
		return std::make_unique<LogicalCallSubquery>(
			input_ ? input_->clone() : nullptr,
			subquery_ ? subquery_->clone() : nullptr,
			importedVars_, returnedVars_,
			inTransactions_, batchSize_);
	}

	[[nodiscard]] std::string toString() const override {
		std::string s = "CallSubquery";
		if (inTransactions_) {
			s += "(IN TRANSACTIONS";
			if (batchSize_ > 0) s += " OF " + std::to_string(batchSize_) + " ROWS";
			s += ")";
		}
		return s;
	}

	void setChild(size_t index, std::unique_ptr<LogicalOperator> child) override {
		if (index == 0) input_ = std::move(child);
		else if (index == 1) subquery_ = std::move(child);
		else throw std::logic_error("LogicalCallSubquery: invalid child index");
	}

	std::unique_ptr<LogicalOperator> detachChild(size_t index) override {
		if (index == 0) return std::move(input_);
		if (index == 1) return std::move(subquery_);
		throw std::logic_error("LogicalCallSubquery: invalid child index");
	}

	[[nodiscard]] LogicalOperator *getInput() const { return input_.get(); }
	[[nodiscard]] LogicalOperator *getSubquery() const { return subquery_.get(); }
	[[nodiscard]] const std::vector<std::string> &getImportedVars() const { return importedVars_; }
	[[nodiscard]] const std::vector<std::string> &getReturnedVars() const { return returnedVars_; }
	[[nodiscard]] bool isInTransactions() const { return inTransactions_; }
	[[nodiscard]] int64_t getBatchSize() const { return batchSize_; }

private:
	std::unique_ptr<LogicalOperator> input_;
	std::unique_ptr<LogicalOperator> subquery_;
	std::vector<std::string> importedVars_;
	std::vector<std::string> returnedVars_;
	bool inTransactions_;
	int64_t batchSize_;
};

} // namespace graph::query::logical
