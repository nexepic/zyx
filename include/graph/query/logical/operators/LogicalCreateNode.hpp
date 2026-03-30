/**
 * @file LogicalCreateNode.hpp
 * @brief Logical operator for CREATE node statements.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once

#include "graph/query/logical/LogicalOperator.hpp"
#include "graph/core/PropertyTypes.hpp"
#include "graph/query/expressions/Expression.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace graph::query::logical {

/**
 * @class LogicalCreateNode
 * @brief Logical operator that creates a new node with optional labels and properties.
 *
 * May have an optional child operator for pipeline mode (e.g., MATCH ... CREATE).
 */
class LogicalCreateNode : public LogicalOperator {
public:
	LogicalCreateNode(
		std::string variable,
		std::vector<std::string> labels,
		std::unordered_map<std::string, graph::PropertyValue> properties,
		std::unordered_map<std::string, std::shared_ptr<graph::query::expressions::Expression>> propertyExprs,
		std::unique_ptr<LogicalOperator> child = nullptr)
		: variable_(std::move(variable))
		, labels_(std::move(labels))
		, properties_(std::move(properties))
		, propertyExprs_(std::move(propertyExprs))
		, child_(std::move(child)) {}

	[[nodiscard]] LogicalOpType getType() const override { return LogicalOpType::LOP_CREATE_NODE; }

	[[nodiscard]] std::vector<LogicalOperator *> getChildren() const override {
		return child_ ? std::vector<LogicalOperator *>{child_.get()} : std::vector<LogicalOperator *>{};
	}

	[[nodiscard]] std::vector<std::string> getOutputVariables() const override {
		std::vector<std::string> out;
		if (child_) {
			out = child_->getOutputVariables();
		}
		out.push_back(variable_);
		return out;
	}

	[[nodiscard]] std::unique_ptr<LogicalOperator> clone() const override {
		std::unordered_map<std::string, std::shared_ptr<graph::query::expressions::Expression>> exprsCopy;
		for (const auto &[k, v] : propertyExprs_) {
			exprsCopy[k] = v ? v->clone() : nullptr;
		}
		return std::make_unique<LogicalCreateNode>(
			variable_,
			labels_,
			properties_,
			std::move(exprsCopy),
			child_ ? child_->clone() : nullptr);
	}

	[[nodiscard]] std::string toString() const override {
		return "CreateNode(" + variable_ + ")";
	}

	void setChild(size_t index, std::unique_ptr<LogicalOperator> child) override {
		if (index != 0) throw std::logic_error("Invalid child index");
		child_ = std::move(child);
	}

	std::unique_ptr<LogicalOperator> detachChild(size_t index) override {
		if (index != 0) throw std::logic_error("Invalid child index");
		return std::move(child_);
	}

	[[nodiscard]] const std::string &getVariable() const { return variable_; }
	[[nodiscard]] const std::vector<std::string> &getLabels() const { return labels_; }
	[[nodiscard]] const std::unordered_map<std::string, graph::PropertyValue> &getProperties() const { return properties_; }
	[[nodiscard]] const std::unordered_map<std::string, std::shared_ptr<graph::query::expressions::Expression>> &getPropertyExprs() const { return propertyExprs_; }

private:
	std::string variable_;
	std::vector<std::string> labels_;
	std::unordered_map<std::string, graph::PropertyValue> properties_;
	std::unordered_map<std::string, std::shared_ptr<graph::query::expressions::Expression>> propertyExprs_;
	std::unique_ptr<LogicalOperator> child_;
};

} // namespace graph::query::logical
