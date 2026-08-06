/**
 * @file IQueryParser.hpp
 * @brief Interface that all DSL parsers (Cypher, etc.) must implement.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once

#include <string>
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
		 * @brief Parses a query into its format-independent logical plan.
		 */
		[[nodiscard]] virtual query::QueryPlan
		parseToLogical(const std::string &query) const = 0;
	};

} // namespace graph::parser
