/**
 * @file LogicalCreateVectorIndex.hpp
 * @brief Logical operator for CREATE VECTOR INDEX statements.
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
 * @class LogicalCreateVectorIndex
 * @brief Logical operator that creates a vector (ANN) index on a node property.
 *
 * Leaf operator: produces no output variables and has no child.
 */
class LogicalCreateVectorIndex : public LogicalOperator {
public:
	LogicalCreateVectorIndex(
		std::string indexName,
		std::string label,
		std::string property,
		int dimension,
		std::string metric)
		: indexName_(std::move(indexName))
		, label_(std::move(label))
		, property_(std::move(property))
		, dimension_(dimension)
		, metric_(std::move(metric)) {}

	[[nodiscard]] LogicalOpType getType() const override { return LogicalOpType::LOP_CREATE_VECTOR_INDEX; }

	[[nodiscard]] std::vector<LogicalOperator *> getChildren() const override { return {}; }

	[[nodiscard]] std::vector<std::string> getOutputVariables() const override { return {}; }

	[[nodiscard]] std::unique_ptr<LogicalOperator> clone() const override {
		return std::make_unique<LogicalCreateVectorIndex>(indexName_, label_, property_, dimension_, metric_);
	}

	[[nodiscard]] std::string toString() const override {
		return "CreateVectorIndex(" + indexName_ + ")";
	}

	void setChild(size_t, std::unique_ptr<LogicalOperator>) override {
		throw std::logic_error("Leaf node");
	}

	std::unique_ptr<LogicalOperator> detachChild(size_t) override {
		throw std::logic_error("Leaf node");
	}

	[[nodiscard]] const std::string &getIndexName() const { return indexName_; }
	[[nodiscard]] const std::string &getLabel() const { return label_; }
	[[nodiscard]] const std::string &getProperty() const { return property_; }
	[[nodiscard]] int getDimension() const { return dimension_; }
	[[nodiscard]] const std::string &getMetric() const { return metric_; }

private:
	std::string indexName_;
	std::string label_;
	std::string property_;
	int dimension_;
	std::string metric_;
};

} // namespace graph::query::logical
