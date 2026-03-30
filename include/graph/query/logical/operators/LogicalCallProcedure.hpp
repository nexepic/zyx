/**
 * @file LogicalCallProcedure.hpp
 * @brief Logical operator for CALL procedure statements.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once

#include "graph/query/logical/LogicalOperator.hpp"
#include "graph/core/PropertyTypes.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace graph::query::logical {

/**
 * @class LogicalCallProcedure
 * @brief Logical operator that invokes a named stored procedure with arguments.
 *
 * Leaf operator: produces no bindable output variables and has no child.
 */
class LogicalCallProcedure : public LogicalOperator {
public:
	LogicalCallProcedure(std::string procedureName, std::vector<graph::PropertyValue> args)
		: procedureName_(std::move(procedureName)), args_(std::move(args)) {}

	[[nodiscard]] LogicalOpType getType() const override { return LogicalOpType::LOP_CALL_PROCEDURE; }

	[[nodiscard]] std::vector<LogicalOperator *> getChildren() const override { return {}; }

	[[nodiscard]] std::vector<std::string> getOutputVariables() const override { return {}; }

	[[nodiscard]] std::unique_ptr<LogicalOperator> clone() const override {
		return std::make_unique<LogicalCallProcedure>(procedureName_, args_);
	}

	[[nodiscard]] std::string toString() const override {
		return "CallProcedure(" + procedureName_ + ")";
	}

	void setChild(size_t, std::unique_ptr<LogicalOperator>) override {
		throw std::logic_error("Leaf node");
	}

	std::unique_ptr<LogicalOperator> detachChild(size_t) override {
		throw std::logic_error("Leaf node");
	}

	[[nodiscard]] const std::string &getProcedureName() const { return procedureName_; }
	[[nodiscard]] const std::vector<graph::PropertyValue> &getArgs() const { return args_; }

private:
	std::string procedureName_;
	std::vector<graph::PropertyValue> args_;
};

} // namespace graph::query::logical
