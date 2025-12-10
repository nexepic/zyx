/**
 * @file CypherParserImpl.hpp
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

// Interface definition
#include "parser/common/IQueryParser.hpp"

// Dependency for building the operator tree
#include "graph/query/QueryPlanner.hpp"

namespace graph::parser::cypher {

	/**
	 * @class CypherParserImpl
	 * @brief Concrete implementation of IQueryParser for the Cypher language.
	 *
	 * This class acts as a facade over the ANTLR generated parser.
	 * It orchestrates the parsing process:
	 * 1. Sets up the ANTLR input stream and lexer.
	 * 2. Invokes the parser rules.
	 * 3. Uses CypherToPlanVisitor (which uses QueryPlanner) to traverse the AST
	 *    and build the Physical Operator Tree.
	 */
	class CypherParserImpl : public IQueryParser {
	public:
		/**
		 * @brief Constructs the parser with a reference to the planner factory.
		 *
		 * @param planner The factory used to create physical operators (Scan, Filter, etc.).
		 */
		explicit CypherParserImpl(std::shared_ptr<query::QueryPlanner> planner);

		~CypherParserImpl() override = default;

		/**
		 * @brief Parses a raw Cypher query string into an executable pipeline.
		 *
		 * @param query The raw Cypher string (e.g., "MATCH (n:User) RETURN n").
		 * @return std::unique_ptr<query::execution::PhysicalOperator> The root of the executable operator tree.
		 * @throws std::runtime_error If syntax errors occur or plan generation fails.
		 */
		[[nodiscard]] std::unique_ptr<query::execution::PhysicalOperator> parse(const std::string& query) const override;

	private:
		std::shared_ptr<query::QueryPlanner> planner_;
	};

} // namespace graph::parser::cypher