/**
 * @file LogicalCreateNode.hpp
 * @brief Logical operator for CREATE node statements.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once
#include "graph/query/expressions/OwnedExpressionMap.hpp"
#include "graph/core/OwnedPropertyMap.hpp"
#include "graph/core/OwnedPropertyPredicates.hpp"
#include "graph/utils/OwnedStringVector.hpp"
#include "graph/utils/OwnedStdString.hpp"

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
		utils::OwnedStdString variable,
		utils::OwnedStringVector labels,
		graph::OwnedPropertyMap properties,
		graph::query::expressions::OwnedExpressionMap propertyExprs,
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

    [[nodiscard]] utils::OwnedStringVector getOwnedOutputVariables() const override {
		utils::OwnedStringVector out;
		if (child_) {
			out = child_->getOwnedOutputVariables();
		}
		out.push_back(variable_);
		return out;
	}

	[[nodiscard]] std::unique_ptr<LogicalOperator> clone() const override {
		auto exprsCopy = graph::query::expressions::cloneExpressionMap(propertyExprs_);
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
	utils::OwnedStdString variable_;
	utils::OwnedStringVector labels_;
	graph::OwnedPropertyMap properties_;
	graph::query::expressions::OwnedExpressionMap propertyExprs_;
	std::unique_ptr<LogicalOperator> child_;
};

} // namespace graph::query::logical
