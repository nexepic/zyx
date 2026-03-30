/**
 * @file LogicalSet.hpp
 * @brief Logical operator for SET statements.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once

#include "graph/query/logical/LogicalOperator.hpp"
#include "graph/query/expressions/Expression.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace graph::query::logical {

/**
 * @enum SetActionType
 * @brief Discriminates the kind of SET action being performed.
 *
 * Prefixed with LSET_ to avoid Windows macro conflicts.
 */
enum class SetActionType {
	LSET_PROPERTY,   ///< SET n.prop = expr
	LSET_LABEL,      ///< SET n:Label
	LSET_MAP_MERGE   ///< SET n += {map}
};

/**
 * @struct LogicalSetItem
 * @brief Describes a single assignment within a SET clause.
 */
struct LogicalSetItem {
	SetActionType type;
	std::string variable;
	std::string key;
	std::shared_ptr<graph::query::expressions::Expression> expression;
};

/**
 * @class LogicalSet
 * @brief Logical operator that applies one or more SET assignments to graph entities.
 */
class LogicalSet : public LogicalOperator {
public:
	LogicalSet(std::vector<LogicalSetItem> items, std::unique_ptr<LogicalOperator> child = nullptr)
		: items_(std::move(items)), child_(std::move(child)) {}

	[[nodiscard]] LogicalOpType getType() const override { return LogicalOpType::LOP_SET; }

	[[nodiscard]] std::vector<LogicalOperator *> getChildren() const override {
		return child_ ? std::vector<LogicalOperator *>{child_.get()} : std::vector<LogicalOperator *>{};
	}

	[[nodiscard]] std::vector<std::string> getOutputVariables() const override {
		return child_ ? child_->getOutputVariables() : std::vector<std::string>{};
	}

	[[nodiscard]] std::unique_ptr<LogicalOperator> clone() const override {
		std::vector<LogicalSetItem> itemsCopy;
		itemsCopy.reserve(items_.size());
		for (const auto &item : items_) {
			itemsCopy.push_back({item.type, item.variable, item.key,
				item.expression ? item.expression->clone() : nullptr});
		}
		return std::make_unique<LogicalSet>(std::move(itemsCopy), child_ ? child_->clone() : nullptr);
	}

	[[nodiscard]] std::string toString() const override { return "Set"; }

	void setChild(size_t index, std::unique_ptr<LogicalOperator> child) override {
		if (index != 0) throw std::logic_error("Invalid child index");
		child_ = std::move(child);
	}

	std::unique_ptr<LogicalOperator> detachChild(size_t index) override {
		if (index != 0) throw std::logic_error("Invalid child index");
		return std::move(child_);
	}

	[[nodiscard]] const std::vector<LogicalSetItem> &getItems() const { return items_; }

private:
	std::vector<LogicalSetItem> items_;
	std::unique_ptr<LogicalOperator> child_;
};

} // namespace graph::query::logical
