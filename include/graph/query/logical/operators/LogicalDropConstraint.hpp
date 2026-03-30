/**
 * @file LogicalDropConstraint.hpp
 * @brief Logical operator for DROP CONSTRAINT statements.
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
 * @class LogicalDropConstraint
 * @brief Logical operator that drops an existing schema constraint by name.
 *
 * Leaf operator: produces no output variables and has no child.
 */
class LogicalDropConstraint : public LogicalOperator {
public:
	LogicalDropConstraint(std::string name, bool ifExists)
		: name_(std::move(name)), ifExists_(ifExists) {}

	[[nodiscard]] LogicalOpType getType() const override { return LogicalOpType::LOP_DROP_CONSTRAINT; }

	[[nodiscard]] std::vector<LogicalOperator *> getChildren() const override { return {}; }

	[[nodiscard]] std::vector<std::string> getOutputVariables() const override { return {}; }

	[[nodiscard]] std::unique_ptr<LogicalOperator> clone() const override {
		return std::make_unique<LogicalDropConstraint>(name_, ifExists_);
	}

	[[nodiscard]] std::string toString() const override {
		return "DropConstraint(" + name_ + ")";
	}

	void setChild(size_t, std::unique_ptr<LogicalOperator>) override {
		throw std::logic_error("Leaf node");
	}

	std::unique_ptr<LogicalOperator> detachChild(size_t) override {
		throw std::logic_error("Leaf node");
	}

	[[nodiscard]] const std::string &getName() const { return name_; }
	[[nodiscard]] bool isIfExists() const { return ifExists_; }

private:
	std::string name_;
	bool ifExists_;
};

} // namespace graph::query::logical
