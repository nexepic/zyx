/**
 * @file CypherParserImpl.cpp
 * @author Nexepic
 * @date 2025/12/9
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 **/

#include "CypherParserImpl.hpp"
#include <regex>
#include "CypherLexer.h"
#include "CypherParser.h"
#include "CypherToPlanVisitor.hpp"
#include "antlr4-runtime.h"
#include "graph/query/planner/PhysicalPlanConverter.hpp"

class ThrowingErrorListener final : public antlr4::BaseErrorListener {
public:
	void syntaxError(antlr4::Recognizer * /*recognizer*/, antlr4::Token * /*offendingSymbol*/, size_t line,
					 size_t charPositionInLine, const std::string &msg, std::exception_ptr /*e*/) override {

		std::string readableMsg = prettifyErrorMessage(msg);

		throw std::runtime_error("Syntax Error at line " + std::to_string(line) + ":" +
								 std::to_string(charPositionInLine) + " - " + readableMsg);
	}

private:
	/**
	 * @brief Transforms internal ANTLR messages into user-friendly strings.
	 */
	static std::string prettifyErrorMessage(std::string msg) {
		// 1. Replace <EOF> with "end of input"
		// Pattern: Literal <EOF>
		static const std::regex eofRegex(R"(<EOF>)");
		msg = std::regex_replace(msg, eofRegex, "end of input");

		// 2. Remove token prefixes (K_INDEX -> 'INDEX')
		// Pattern: Word boundary, K_ or L_ prefix, uppercase letters
		static const std::regex tokenRegex(R"(\b(K_|L_)([A-Z_]+)\b)");
		msg = std::regex_replace(msg, tokenRegex, "'$2'");

		// 3. Make sets readable (expecting {A, B} -> expecting one of: A, B)
		// Pattern: expecting { ... }
		static const std::regex setRegex(R"(expecting\s*\{([^}]+)\})");
		msg = std::regex_replace(msg, setRegex, "expecting one of: $1");

		// 4. Translate technical token names
		// ID -> identifier
		static const std::regex idRegex(R"(\bID\b)");
		msg = std::regex_replace(msg, idRegex, "identifier");

		// StringLiteral -> string
		static const std::regex stringRegex(R"(\bStringLiteral\b)");
		msg = std::regex_replace(msg, stringRegex, "string");

		return msg;
	}
};

namespace graph::parser::cypher {

	CypherParserImpl::CypherParserImpl(std::shared_ptr<query::QueryPlanner> planner) : planner_(std::move(planner)) {}

	std::unique_ptr<query::execution::PhysicalOperator> CypherParserImpl::parse(const std::string &query) const {
		auto plan = parseToLogical(query);
		if (!plan.root) {
			return nullptr;
		}

		// Convert logical plan to physical plan using managers from the planner
		auto dm = planner_->getDataManager();
		auto im = planner_->getIndexManager();
		auto cm = planner_->getConstraintManager();

		query::PhysicalPlanConverter converter(dm, im, cm);
		return converter.convert(plan.root.get());
	}

	query::QueryPlan CypherParserImpl::parseToLogical(const std::string &query) const {
		antlr4::ANTLRInputStream input(query);
		CypherLexer lexer(&input);
		antlr4::CommonTokenStream tokens(&lexer);
		CypherParser parser(&tokens);

		parser.removeErrorListeners();
		lexer.removeErrorListeners();

		ThrowingErrorListener errorListener;
		parser.addErrorListener(&errorListener);
		lexer.addErrorListener(&errorListener);

		CypherParser::CypherContext *tree = parser.cypher();

		CypherToPlanVisitor visitor(planner_);

		try {
			visitor.visit(tree);
		} catch (const std::exception &e) {
			throw std::runtime_error(std::string("Plan generation failed: ") + e.what());
		}

		return visitor.getQueryPlan();
	}
} // namespace graph::parser::cypher
