/**
 * @file LogicalPropertyPredicate.hpp
 * @brief Format-independent property predicate for logical graph reads.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#pragma once

#include <optional>
#include <string>

#include "graph/core/PropertyTypes.hpp"

namespace graph::query::execution {

	// The LPP_ prefix keeps the enumerators safe from platform macros.
	enum class LogicalPropertyPredicateOp {
		LPP_EQ,
		LPP_NE,
		LPP_LT,
		LPP_LE,
		LPP_GT,
		LPP_GE,
		LPP_RANGE_CLOSED,
	};

	struct LogicalPropertyPredicate {
		std::string property_key;
		LogicalPropertyPredicateOp op = LogicalPropertyPredicateOp::LPP_EQ;
		PropertyValue value;
		std::optional<PropertyValue> upper_value;
	};

	[[nodiscard]] bool evaluateLogicalPropertyPredicate(const std::optional<PropertyValue> &actual,
															 const LogicalPropertyPredicate &predicate);

} // namespace graph::query::execution
