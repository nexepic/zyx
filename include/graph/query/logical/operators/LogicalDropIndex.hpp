/**
 * @file LogicalDropIndex.hpp
 * @brief Logical operator for DROP INDEX statements.
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
 * @class LogicalDropIndex
 * @brief Logical operator that drops an existing property index.
 *
 * An index may be identified either by name alone, or by label + property key.
 * Leaf operator: produces no output variables and has no child.
 */
class LogicalDropIndex : public LogicalOperator {
public:
	LogicalDropIndex(std::string indexName, std::string label, std::string propertyKey)
		: indexName_(std::move(indexName))
		, label_(std::move(label))
		, propertyKey_(std::move(propertyKey)) {}

	[[nodiscard]] LogicalOpType getType() const override { return LogicalOpType::LOP_DROP_INDEX; }

	[[nodiscard]] std::vector<LogicalOperator *> getChildren() const override { return {}; }

	[[nodiscard]] std::vector<std::string> getOutputVariables() const override { return {}; }

	[[nodiscard]] std::unique_ptr<LogicalOperator> clone() const override {
		return std::make_unique<LogicalDropIndex>(indexName_, label_, propertyKey_);
	}

	[[nodiscard]] std::string toString() const override {
		return "DropIndex(" + (indexName_.empty() ? label_ + "." + propertyKey_ : indexName_) + ")";
	}

	void setChild(size_t, std::unique_ptr<LogicalOperator>) override {
		throw std::logic_error("Leaf node");
	}

	std::unique_ptr<LogicalOperator> detachChild(size_t) override {
		throw std::logic_error("Leaf node");
	}

	[[nodiscard]] const std::string &getIndexName() const { return indexName_; }
	[[nodiscard]] const std::string &getLabel() const { return label_; }
	[[nodiscard]] const std::string &getPropertyKey() const { return propertyKey_; }

private:
	std::string indexName_;
	std::string label_;
	std::string propertyKey_;
};

} // namespace graph::query::logical
