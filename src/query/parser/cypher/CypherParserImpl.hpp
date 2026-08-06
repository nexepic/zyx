/**
 * @file CypherParserImpl.hpp
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

#pragma once

#include <memory>
#include <string>
#include "query/parser/common/IQueryParser.hpp"

// Forward declarations to avoid including ANTLR headers here
namespace antlr4 {
	class ANTLRInputStream;
	class CommonTokenStream;
	class BaseErrorListener;
} // namespace antlr4
class CypherLexer;
class CypherParser;

namespace graph::parser::cypher {

	/**
	 * @class CypherParserImpl
	 * @brief Concrete implementation of IQueryParser for the Cypher language.
	 *
	 * This class acts as a facade over the ANTLR generated parser.
	 * It orchestrates the parsing process:
	 * 1. Sets up the ANTLR input stream and lexer.
	 * 2. Invokes the parser rules.
	 * 3. Uses CypherToPlanVisitor to traverse the AST and build the logical plan.
	 *
	 * The ANTLR4 lexer and parser are kept alive across calls to avoid
	 * DFA cache corruption that occurs when the ATN simulator is destroyed
	 * while the static DFA cache still references its states.
	 */
	class CypherParserImpl : public IQueryParser {
	public:
		/**
		 * @brief Constructs a parser for the native logical execution pipeline.
		 */
		CypherParserImpl();

		~CypherParserImpl() override;

		[[nodiscard]] query::QueryPlan
		parseToLogical(const std::string &query) const override;

	private:
		// Persistent ANTLR4 objects — kept alive to preserve DFA cache integrity.
		// ANTLR4's static DFA cache retains pointers into the ATN simulator owned
		// by each Lexer/Parser instance.  Destroying and recreating these objects
		// leaves dangling pointers in the cache, causing SIGSEGV/SIGBUS on the
		// next parse.  Keeping them alive for the lifetime of CypherParserImpl
		// avoids this.
		// Mutable because parsing mutates internal ANTLR state but is logically const.
		mutable std::unique_ptr<antlr4::ANTLRInputStream> input_;
		mutable std::unique_ptr<CypherLexer> lexer_;
		mutable std::unique_ptr<antlr4::CommonTokenStream> tokens_;
		mutable std::unique_ptr<CypherParser> parser_;
		std::unique_ptr<antlr4::BaseErrorListener> errorListener_;
	};

} // namespace graph::parser::cypher
