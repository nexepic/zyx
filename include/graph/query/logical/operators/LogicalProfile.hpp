/**
 * @file LogicalProfile.hpp
 * @brief Logical operator for PROFILE — wraps an inner plan to execute with timing.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once

#include "graph/query/logical/LogicalOperator.hpp"
#include <memory>
#include <stdexcept>
#include <string>

namespace graph::query::logical {

class LogicalProfile : public LogicalOperator {
public:
	explicit LogicalProfile(std::unique_ptr<LogicalOperator> innerPlan)
		: innerPlan_(std::move(innerPlan)) {}

	[[nodiscard]] LogicalOpType getType() const override { return LogicalOpType::LOP_PROFILE; }

	[[nodiscard]] std::vector<LogicalOperator *> getChildren() const override {
		if (innerPlan_) return {innerPlan_.get()};
		return {};
	}

	[[nodiscard]] std::vector<std::string> getOutputVariables() const override {
		return {};
	}

	[[nodiscard]] std::unique_ptr<LogicalOperator> clone() const override {
		return std::make_unique<LogicalProfile>(innerPlan_ ? innerPlan_->clone() : nullptr);
	}

	[[nodiscard]] std::string toString() const override { return "Profile"; }

	void setChild(size_t /*index*/, std::unique_ptr<LogicalOperator> child) override {
		innerPlan_ = std::move(child);
	}

	std::unique_ptr<LogicalOperator> detachChild(size_t /*index*/) override {
		return std::move(innerPlan_);
	}

	[[nodiscard]] const LogicalOperator *getInnerPlan() const { return innerPlan_.get(); }

private:
	std::unique_ptr<LogicalOperator> innerPlan_;
};

} // namespace graph::query::logical
