/**
 * @file LogicalDelete.hpp
 * @brief Logical operator for DELETE statements.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once

#include "graph/query/logical/LogicalOperator.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace graph::query::logical {

/**
 * @class LogicalDelete
 * @brief Logical operator that deletes nodes and/or edges from the graph.
 *
 * When detach_ is true, all relationships attached to a deleted node are also removed.
 */
class LogicalDelete : public LogicalOperator {
public:
	LogicalDelete(std::vector<std::string> variables, bool detach, std::unique_ptr<LogicalOperator> child = nullptr)
		: variables_(std::move(variables)), detach_(detach), child_(std::move(child)) {}

	[[nodiscard]] LogicalOpType getType() const override { return LogicalOpType::LOP_DELETE; }

	[[nodiscard]] std::vector<LogicalOperator *> getChildren() const override {
		return child_ ? std::vector<LogicalOperator *>{child_.get()} : std::vector<LogicalOperator *>{};
	}

	[[nodiscard]] std::vector<std::string> getOutputVariables() const override {
		return child_ ? child_->getOutputVariables() : std::vector<std::string>{};
	}

	[[nodiscard]] std::unique_ptr<LogicalOperator> clone() const override {
		return std::make_unique<LogicalDelete>(variables_, detach_, child_ ? child_->clone() : nullptr);
	}

	[[nodiscard]] std::string toString() const override {
		return detach_ ? "DetachDelete" : "Delete";
	}

	void setChild(size_t index, std::unique_ptr<LogicalOperator> child) override {
		if (index != 0) throw std::logic_error("Invalid child index");
		child_ = std::move(child);
	}

	std::unique_ptr<LogicalOperator> detachChild(size_t index) override {
		if (index != 0) throw std::logic_error("Invalid child index");
		return std::move(child_);
	}

	[[nodiscard]] const std::vector<std::string> &getVariables() const { return variables_; }
	[[nodiscard]] bool isDetach() const { return detach_; }

private:
	std::vector<std::string> variables_;
	bool detach_;
	std::unique_ptr<LogicalOperator> child_;
};

} // namespace graph::query::logical
