/**
 * @file IQueryParser.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/9
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <memory>
#include <string>
#include "graph/query/execution/PhysicalOperator.hpp"

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
		 *
		 * @param query The source query string.
		 * @return std::unique_ptr<graph::query::execution::PhysicalOperator> The root of the execution tree.
		 */
		[[nodiscard]] virtual std::unique_ptr<query::execution::PhysicalOperator>
		parse(const std::string &query) const = 0;
	};

} // namespace graph::parser
