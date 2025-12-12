/**
 * @file CypherParserImpl.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/9
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "CypherParserImpl.hpp"
#include "antlr4-runtime.h"
#include "CypherLexer.h"
#include "CypherParser.h"
#include "CypherToPlanVisitor.hpp"

namespace graph::parser::cypher {

	CypherParserImpl::CypherParserImpl(std::shared_ptr<query::QueryPlanner> planner)
		: planner_(std::move(planner)) {}

	std::unique_ptr<query::execution::PhysicalOperator> CypherParserImpl::parse(const std::string& query) const {
		// 1. ANTLR Boilerplate
		antlr4::ANTLRInputStream input(query);
		CypherLexer lexer(&input);
		antlr4::CommonTokenStream tokens(&lexer);
		CypherParser parser(&tokens);

		// Error handling
		parser.removeErrorListeners();
		parser.addErrorListener(&antlr4::ConsoleErrorListener::INSTANCE);

		auto tree = parser.query();

		// 2. Visit tree to build Operator Pipeline
		CypherToPlanVisitor visitor(planner_);
		visitor.visit(tree);

		// 3. Extract result
		auto plan = visitor.getPlan();
		if (!plan) {
			throw std::runtime_error("Failed to generate query plan.");
		}

		return plan;
	}
}