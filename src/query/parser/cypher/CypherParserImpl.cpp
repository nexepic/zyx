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
#include "CypherLexer.h"
#include "CypherParser.h"
#include "CypherToPlanVisitor.hpp"
#include "antlr4-runtime.h"

class ThrowingErrorListener : public antlr4::BaseErrorListener {
public:
	void syntaxError(antlr4::Recognizer * /*recognizer*/, antlr4::Token * /*offendingSymbol*/, size_t line,
					 size_t charPositionInLine, const std::string &msg, std::exception_ptr /*e*/) override {
		throw std::runtime_error("Syntax Error at line " + std::to_string(line) + ":" +
								 std::to_string(charPositionInLine) + " - " + msg);
	}
};

namespace graph::parser::cypher {

	CypherParserImpl::CypherParserImpl(std::shared_ptr<query::QueryPlanner> planner) : planner_(std::move(planner)) {}

	std::unique_ptr<query::execution::PhysicalOperator> CypherParserImpl::parse(const std::string &query) const {
		antlr4::ANTLRInputStream input(query);
		CypherLexer lexer(&input);
		antlr4::CommonTokenStream tokens(&lexer);
		CypherParser parser(&tokens);

		// Error Handling
		parser.removeErrorListeners();
		// Add console listener if you still want to see it, or just use the throwing one
		// parser.addErrorListener(&antlr4::ConsoleErrorListener::INSTANCE);

		ThrowingErrorListener errorListener;
		parser.addErrorListener(&errorListener);
		lexer.removeErrorListeners();
		lexer.addErrorListener(&errorListener);

		// 1. Parse
		CypherParser::CypherContext *tree = nullptr;
		try {
			tree = parser.cypher();
		} catch (const std::exception &e) {
			// Rethrow or handle parsing exception
			throw;
		}

		// 2. Visit
		CypherToPlanVisitor visitor(planner_);

		try {
			visitor.visit(tree);
		} catch (const std::exception &e) {
			// Contextualize visitor errors
			throw std::runtime_error(std::string("Plan generation failed: ") + e.what());
		}

		// 3. Extract result
		auto plan = visitor.getPlan();
		if (!plan) {
			// This happens if the query was empty or comment-only, or visitor didn't set rootOp
			// For CREATE INDEX, visitor SHOULD set rootOp.
			// If syntax was correct but no plan, it's an internal logic error.
			throw std::runtime_error("Query successfully parsed but generated no execution plan.");
		}

		return plan;
	}
} // namespace graph::parser::cypher
