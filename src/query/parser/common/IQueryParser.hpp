/**
 * @file IQueryParser.hpp
 * @brief Interface that all DSL parsers (Cypher, etc.) must implement.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once

#include <memory>
#include <string>
#include "graph/query/execution/PhysicalOperator.hpp"
#include "graph/query/QueryPlan.hpp"

namespace graph::parser {

	/**
	 * @interface IQueryParser
	 * @brief Interface that all DSL parsers (Cypher, etc.) must implement.
	 */
	class IQueryParser {
	public:
		virtual ~IQueryParser() = default;

		/**
		 * @brief Parses a raw query string into an executable Operator Tree.
		 */
		[[nodiscard]] virtual std::unique_ptr<query::execution::PhysicalOperator>
		parse(const std::string &query) const = 0;

		/**
		 * @brief Parses and optimizes a query, returning a QueryPlan (cacheable).
		 */
		[[nodiscard]] virtual query::QueryPlan
		parseToLogical(const std::string &query) const = 0;
	};

} // namespace graph::parser
