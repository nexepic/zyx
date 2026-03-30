/**
 * @file LogicalCreateConstraint.hpp
 * @brief Logical operator for CREATE CONSTRAINT statements.
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
 * @class LogicalCreateConstraint
 * @brief Logical operator that creates a schema constraint (e.g., uniqueness, existence).
 *
 * Leaf operator: produces no output variables and has no child.
 */
class LogicalCreateConstraint : public LogicalOperator {
public:
	LogicalCreateConstraint(
		std::string name,
		std::string entityType,
		std::string constraintType,
		std::string label,
		std::vector<std::string> properties,
		std::string options)
		: name_(std::move(name))
		, entityType_(std::move(entityType))
		, constraintType_(std::move(constraintType))
		, label_(std::move(label))
		, properties_(std::move(properties))
		, options_(std::move(options)) {}

	[[nodiscard]] LogicalOpType getType() const override { return LogicalOpType::LOP_CREATE_CONSTRAINT; }

	[[nodiscard]] std::vector<LogicalOperator *> getChildren() const override { return {}; }

	[[nodiscard]] std::vector<std::string> getOutputVariables() const override { return {}; }

	[[nodiscard]] std::unique_ptr<LogicalOperator> clone() const override {
		return std::make_unique<LogicalCreateConstraint>(
			name_, entityType_, constraintType_, label_, properties_, options_);
	}

	[[nodiscard]] std::string toString() const override {
		return "CreateConstraint(" + name_ + ")";
	}

	void setChild(size_t, std::unique_ptr<LogicalOperator>) override {
		throw std::logic_error("Leaf node");
	}

	std::unique_ptr<LogicalOperator> detachChild(size_t) override {
		throw std::logic_error("Leaf node");
	}

	[[nodiscard]] const std::string &getName() const { return name_; }
	[[nodiscard]] const std::string &getEntityType() const { return entityType_; }
	[[nodiscard]] const std::string &getConstraintType() const { return constraintType_; }
	[[nodiscard]] const std::string &getLabel() const { return label_; }
	[[nodiscard]] const std::vector<std::string> &getProperties() const { return properties_; }
	[[nodiscard]] const std::string &getOptions() const { return options_; }

private:
	std::string name_;
	std::string entityType_;
	std::string constraintType_;
	std::string label_;
	std::vector<std::string> properties_;
	std::string options_;
};

} // namespace graph::query::logical
