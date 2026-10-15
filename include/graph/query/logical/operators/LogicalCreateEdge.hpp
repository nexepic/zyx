/**
 * @file LogicalCreateEdge.hpp
 * @brief Logical operator for CREATE edge statements.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once
#include "graph/query/expressions/OwnedExpressionMap.hpp"
#include "graph/core/OwnedPropertyMap.hpp"
#include "graph/core/OwnedPropertyPredicates.hpp"
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
 * @class LogicalCreateEdge
 * @brief Logical operator that creates a new edge between two nodes.
 *
 * Typically has a child operator that provides the source and target node bindings.
 */
class LogicalCreateEdge : public LogicalOperator {
public:
	LogicalCreateEdge(
		utils::OwnedStdString variable,
		utils::OwnedStdString type,
		graph::OwnedPropertyMap properties,
		utils::OwnedStdString sourceVar,
		utils::OwnedStdString targetVar,
		std::unique_ptr<LogicalOperator> child = nullptr,
		utils::OwnedStdString direction = "out")
		: variable_(std::move(variable))
		, type_(std::move(type))
		, properties_(std::move(properties))
		, sourceVar_(std::move(sourceVar))
		, targetVar_(std::move(targetVar))
		, direction_(std::move(direction))
		, child_(std::move(child)) {}

	LogicalCreateEdge(
		utils::OwnedStdString variable,
		utils::OwnedStdString type,
		graph::OwnedPropertyMap properties,
		graph::query::expressions::OwnedExpressionMap propertyExprs,
		utils::OwnedStdString sourceVar,
		utils::OwnedStdString targetVar,
		std::unique_ptr<LogicalOperator> child = nullptr,
		utils::OwnedStdString direction = "out")
		: variable_(std::move(variable))
		, type_(std::move(type))
		, properties_(std::move(properties))
		, propertyExprs_(std::move(propertyExprs))
		, sourceVar_(std::move(sourceVar))
		, targetVar_(std::move(targetVar))
		, direction_(std::move(direction))
		, child_(std::move(child)) {}

	[[nodiscard]] LogicalOpType getType() const override { return LogicalOpType::LOP_CREATE_EDGE; }

	[[nodiscard]] std::vector<LogicalOperator *> getChildren() const override {
		return child_ ? std::vector<LogicalOperator *>{child_.get()} : std::vector<LogicalOperator *>{};
	}

	[[nodiscard]] std::vector<std::string> getOutputVariables() const override {
		std::vector<std::string> out;
		if (child_) {
			out = child_->getOutputVariables();
		}
		if (!variable_.empty()) {
			out.push_back(variable_);
		}
		return out;
	}

    [[nodiscard]] utils::OwnedStringVector getOwnedOutputVariables() const override {
		utils::OwnedStringVector out;
		if (child_) {
			out = child_->getOwnedOutputVariables();
		}
		if (!variable_.empty()) {
			out.push_back(variable_);
		}
		return out;
	}

	[[nodiscard]] std::unique_ptr<LogicalOperator> clone() const override {
		auto exprsCopy = graph::query::expressions::cloneExpressionMap(propertyExprs_);
		return std::make_unique<LogicalCreateEdge>(
			variable_,
			type_,
			properties_,
			std::move(exprsCopy),
			sourceVar_,
			targetVar_,
			child_ ? child_->clone() : nullptr,
			direction_);
	}

	[[nodiscard]] std::string toString() const override {
		return "CreateEdge(" + variable_ + ":" + type_ + ")";
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
	[[nodiscard]] const std::string &getEdgeType() const { return type_; }
	[[nodiscard]] const std::unordered_map<std::string, graph::PropertyValue> &getProperties() const { return properties_; }
	[[nodiscard]] const std::unordered_map<std::string, std::shared_ptr<graph::query::expressions::Expression>> &
	getPropertyExprs() const {
		return propertyExprs_;
	}
	[[nodiscard]] const std::string &getSourceVar() const { return sourceVar_; }
	[[nodiscard]] const std::string &getTargetVar() const { return targetVar_; }
	[[nodiscard]] const std::string &getDirection() const { return direction_; }

private:
	utils::OwnedStdString variable_;
	utils::OwnedStdString type_;
	graph::OwnedPropertyMap properties_;
	graph::query::expressions::OwnedExpressionMap propertyExprs_;
	utils::OwnedStdString sourceVar_;
	utils::OwnedStdString targetVar_;
	utils::OwnedStdString direction_;
	std::unique_ptr<LogicalOperator> child_;
};

} // namespace graph::query::logical
