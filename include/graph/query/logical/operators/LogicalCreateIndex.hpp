/**
 * @file LogicalCreateIndex.hpp
 * @brief Logical operator for CREATE INDEX statements.
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
 * @class LogicalCreateIndex
 * @brief Logical operator that creates a property index on a labeled entity.
 *
 * Leaf operator: produces no output variables and has no child.
 */
class LogicalCreateIndex : public LogicalOperator {
public:
	LogicalCreateIndex(std::string indexName, std::string label, std::string propertyKey)
		: indexName_(std::move(indexName))
		, label_(std::move(label))
		, propertyKey_(std::move(propertyKey)) {}

	[[nodiscard]] LogicalOpType getType() const override { return LogicalOpType::LOP_CREATE_INDEX; }

	[[nodiscard]] std::vector<LogicalOperator *> getChildren() const override { return {}; }

	[[nodiscard]] std::vector<std::string> getOutputVariables() const override { return {}; }

	[[nodiscard]] std::unique_ptr<LogicalOperator> clone() const override {
		return std::make_unique<LogicalCreateIndex>(indexName_, label_, propertyKey_);
	}

	[[nodiscard]] std::string toString() const override {
		return "CreateIndex(" + indexName_ + ")";
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
