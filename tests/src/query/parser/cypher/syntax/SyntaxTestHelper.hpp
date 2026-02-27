/**
 * @file SyntaxTestHelper.hpp
 * @author Nexepic
 * @date 2026/1/29
 *
 * @copyright Copyright (c) 2026 Nexepic
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

#include <gtest/gtest.h>
#include <string>
#include <vector>
#include "CypherLexer.h"
#include "CypherParser.h"
#include "antlr4-runtime.h"

class TestErrorListener : public antlr4::BaseErrorListener {
public:
	void syntaxError(antlr4::Recognizer * /*recognizer*/, antlr4::Token * /*offendingSymbol*/, size_t line,
					 size_t charPositionInLine, const std::string &msg, std::exception_ptr /*e*/) override {
		errorMessages.push_back("Line " + std::to_string(line) + ":" + std::to_string(charPositionInLine) + " " + msg);
	}
	bool hasErrors() const { return !errorMessages.empty(); }
	std::vector<std::string> errorMessages;
};

class CypherSyntaxTest : public ::testing::Test {
protected:
	static bool validate(const std::string &query) {
		antlr4::ANTLRInputStream input(query);
		CypherLexer lexer(&input);
		lexer.removeErrorListeners();
		TestErrorListener lexerErrorListener;
		lexer.addErrorListener(&lexerErrorListener);

		antlr4::CommonTokenStream tokens(&lexer);
		CypherParser parser(&tokens);
		parser.removeErrorListeners();
		TestErrorListener parserErrorListener;
		parser.addErrorListener(&parserErrorListener);

		parser.cypher();

		if (lexerErrorListener.hasErrors() || parserErrorListener.hasErrors()) {
			return false;
		}
		return true;
	}
};
