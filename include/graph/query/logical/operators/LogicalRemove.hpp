/**
 * @file LogicalRemove.hpp
 * @brief Logical operator for REMOVE statements.
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
 * @enum LogicalRemoveActionType
 * @brief Discriminates whether a REMOVE item targets a property or a label.
 *
 * Prefixed with LREM_ to avoid Windows macro conflicts.
 */
enum class LogicalRemoveActionType {
	LREM_PROPERTY, ///< REMOVE n.prop
	LREM_LABEL     ///< REMOVE n:Label
};

/**
 * @struct LogicalRemoveItem
 * @brief Describes a single item within a REMOVE clause.
 */
struct LogicalRemoveItem {
	LogicalRemoveActionType type;
	std::string variable;
	std::string key;
};

/**
 * @class LogicalRemove
 * @brief Logical operator that removes properties or labels from graph entities.
 */
class LogicalRemove : public LogicalOperator {
public:
	LogicalRemove(std::vector<LogicalRemoveItem> items, std::unique_ptr<LogicalOperator> child = nullptr)
		: items_(std::move(items)), child_(std::move(child)) {}

	[[nodiscard]] LogicalOpType getType() const override { return LogicalOpType::LOP_REMOVE; }

	[[nodiscard]] std::vector<LogicalOperator *> getChildren() const override {
		return child_ ? std::vector<LogicalOperator *>{child_.get()} : std::vector<LogicalOperator *>{};
	}

	[[nodiscard]] std::vector<std::string> getOutputVariables() const override {
		return child_ ? child_->getOutputVariables() : std::vector<std::string>{};
	}

	[[nodiscard]] std::unique_ptr<LogicalOperator> clone() const override {
		return std::make_unique<LogicalRemove>(items_, child_ ? child_->clone() : nullptr);
	}

	[[nodiscard]] std::string toString() const override { return "Remove"; }

	void setChild(size_t index, std::unique_ptr<LogicalOperator> child) override {
		if (index != 0) throw std::logic_error("Invalid child index");
		child_ = std::move(child);
	}

	std::unique_ptr<LogicalOperator> detachChild(size_t index) override {
		if (index != 0) throw std::logic_error("Invalid child index");
		return std::move(child_);
	}

	[[nodiscard]] const std::vector<LogicalRemoveItem> &getItems() const { return items_; }

private:
	std::vector<LogicalRemoveItem> items_;
	std::unique_ptr<LogicalOperator> child_;
};

} // namespace graph::query::logical
