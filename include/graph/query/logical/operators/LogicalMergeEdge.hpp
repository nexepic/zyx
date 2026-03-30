/**
 * @file LogicalMergeEdge.hpp
 * @brief Logical operator for MERGE edge statements.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once

#include "graph/query/logical/LogicalOperator.hpp"
#include "graph/core/PropertyTypes.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/logical/operators/LogicalMergeNode.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace graph::query::logical {

/**
 * @class LogicalMergeEdge
 * @brief Logical operator that matches or creates an edge, with optional
 *        ON CREATE / ON MATCH SET actions.
 *
 * This is a leaf operator: the source and target node variables are resolved
 * by an enclosing scan or prior plan stage.
 */
class LogicalMergeEdge : public LogicalOperator {
public:
	LogicalMergeEdge(
		std::string sourceVar,
		std::string edgeVar,
		std::string targetVar,
		std::string edgeLabel,
		std::string direction,
		std::unordered_map<std::string, graph::PropertyValue> matchProps,
		std::vector<MergeSetAction> onCreateActions,
		std::vector<MergeSetAction> onMatchActions,
		std::unique_ptr<LogicalOperator> child = nullptr)
		: child_(std::move(child))
		, sourceVar_(std::move(sourceVar))
		, edgeVar_(std::move(edgeVar))
		, targetVar_(std::move(targetVar))
		, edgeLabel_(std::move(edgeLabel))
		, direction_(std::move(direction))
		, matchProps_(std::move(matchProps))
		, onCreateActions_(std::move(onCreateActions))
		, onMatchActions_(std::move(onMatchActions)) {}

	[[nodiscard]] LogicalOpType getType() const override { return LogicalOpType::LOP_MERGE_EDGE; }

	[[nodiscard]] std::vector<LogicalOperator *> getChildren() const override {
		if (child_) return {child_.get()};
		return {};
	}

	[[nodiscard]] std::vector<std::string> getOutputVariables() const override {
		std::vector<std::string> out;
		if (child_) out = child_->getOutputVariables();
		if (!edgeVar_.empty()) out.push_back(edgeVar_);
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
		return std::make_unique<LogicalMergeEdge>(
			sourceVar_, edgeVar_, targetVar_, edgeLabel_, direction_,
			matchProps_,
			cloneActions(onCreateActions_),
			cloneActions(onMatchActions_),
			child_ ? child_->clone() : nullptr);
	}

	[[nodiscard]] std::string toString() const override {
		return "MergeEdge(" + edgeVar_ + ":" + edgeLabel_ + ")";
	}

	void setChild(size_t index, std::unique_ptr<LogicalOperator> child) override {
		if (index != 0) throw std::out_of_range("LogicalMergeEdge has at most one child");
		child_ = std::move(child);
	}

	std::unique_ptr<LogicalOperator> detachChild(size_t index) override {
		if (index != 0) throw std::out_of_range("LogicalMergeEdge has at most one child");
		return std::move(child_);
	}

	[[nodiscard]] const std::string &getSourceVar() const { return sourceVar_; }
	[[nodiscard]] const std::string &getEdgeVar() const { return edgeVar_; }
	[[nodiscard]] const std::string &getTargetVar() const { return targetVar_; }
	[[nodiscard]] const std::string &getEdgeLabel() const { return edgeLabel_; }
	[[nodiscard]] const std::string &getDirection() const { return direction_; }
	[[nodiscard]] const std::unordered_map<std::string, graph::PropertyValue> &getMatchProps() const { return matchProps_; }
	[[nodiscard]] const std::vector<MergeSetAction> &getOnCreateActions() const { return onCreateActions_; }
	[[nodiscard]] const std::vector<MergeSetAction> &getOnMatchActions() const { return onMatchActions_; }

private:
	std::unique_ptr<LogicalOperator> child_;
	std::string sourceVar_;
	std::string edgeVar_;
	std::string targetVar_;
	std::string edgeLabel_;
	std::string direction_;
	std::unordered_map<std::string, graph::PropertyValue> matchProps_;
	std::vector<MergeSetAction> onCreateActions_;
	std::vector<MergeSetAction> onMatchActions_;
};

} // namespace graph::query::logical
