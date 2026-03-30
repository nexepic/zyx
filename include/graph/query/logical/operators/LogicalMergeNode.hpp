/**
 * @file LogicalMergeNode.hpp
 * @brief Logical operator for MERGE node statements.
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
 * @struct MergeSetAction
 * @brief Represents a single SET action within ON CREATE or ON MATCH clause.
 */
struct MergeSetAction {
	std::string variable;
	std::string key;
	std::shared_ptr<graph::query::expressions::Expression> expression;
};

/**
 * @class LogicalMergeNode
 * @brief Logical operator that matches or creates a node, then optionally runs
 *        ON CREATE / ON MATCH SET actions.
 */
class LogicalMergeNode : public LogicalOperator {
public:
	LogicalMergeNode(
		std::string variable,
		std::vector<std::string> labels,
		std::unordered_map<std::string, graph::PropertyValue> matchProps,
		std::vector<MergeSetAction> onCreateItems,
		std::vector<MergeSetAction> onMatchItems,
		std::unique_ptr<LogicalOperator> child = nullptr)
		: variable_(std::move(variable))
		, labels_(std::move(labels))
		, matchProps_(std::move(matchProps))
		, onCreateItems_(std::move(onCreateItems))
		, onMatchItems_(std::move(onMatchItems))
		, child_(std::move(child)) {}

	[[nodiscard]] LogicalOpType getType() const override { return LogicalOpType::LOP_MERGE_NODE; }

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
		auto cloneActions = [](const std::vector<MergeSetAction> &src) {
			std::vector<MergeSetAction> dst;
			dst.reserve(src.size());
			for (const auto &a : src) {
				dst.push_back({a.variable, a.key, a.expression ? a.expression->clone() : nullptr});
			}
			return dst;
		};
		return std::make_unique<LogicalMergeNode>(
			variable_,
			labels_,
			matchProps_,
			cloneActions(onCreateItems_),
			cloneActions(onMatchItems_),
			child_ ? child_->clone() : nullptr);
	}

	[[nodiscard]] std::string toString() const override { return "MergeNode(" + variable_ + ")"; }

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
	[[nodiscard]] const std::unordered_map<std::string, graph::PropertyValue> &getMatchProps() const { return matchProps_; }
	[[nodiscard]] const std::vector<MergeSetAction> &getOnCreateItems() const { return onCreateItems_; }
	[[nodiscard]] const std::vector<MergeSetAction> &getOnMatchItems() const { return onMatchItems_; }

private:
	std::string variable_;
	std::vector<std::string> labels_;
	std::unordered_map<std::string, graph::PropertyValue> matchProps_;
	std::vector<MergeSetAction> onCreateItems_;
	std::vector<MergeSetAction> onMatchItems_;
	std::unique_ptr<LogicalOperator> child_;
};

} // namespace graph::query::logical
