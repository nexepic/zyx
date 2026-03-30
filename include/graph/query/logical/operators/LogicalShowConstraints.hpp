/**
 * @file LogicalShowConstraints.hpp
 * @brief Logical operator for SHOW CONSTRAINTS statements.
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
 * @class LogicalShowConstraints
 * @brief Logical operator that retrieves and returns all defined constraints.
 *
 * Leaf operator: has no child and produces no bindable output variables.
 */
class LogicalShowConstraints : public LogicalOperator {
public:
	LogicalShowConstraints() = default;

	[[nodiscard]] LogicalOpType getType() const override { return LogicalOpType::LOP_SHOW_CONSTRAINTS; }

	[[nodiscard]] std::vector<LogicalOperator *> getChildren() const override { return {}; }

	[[nodiscard]] std::vector<std::string> getOutputVariables() const override { return {}; }

	[[nodiscard]] std::unique_ptr<LogicalOperator> clone() const override {
		return std::make_unique<LogicalShowConstraints>();
	}

	[[nodiscard]] std::string toString() const override { return "ShowConstraints"; }

	void setChild(size_t, std::unique_ptr<LogicalOperator>) override {
		throw std::logic_error("Leaf node");
	}

	std::unique_ptr<LogicalOperator> detachChild(size_t) override {
		throw std::logic_error("Leaf node");
	}
};

} // namespace graph::query::logical
