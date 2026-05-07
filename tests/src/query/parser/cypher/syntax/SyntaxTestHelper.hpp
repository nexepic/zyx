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
	// ANTLR4 objects are kept alive across validate() calls to avoid DFA cache
	// corruption.  The static DFA cache retains pointers into the ATN simulator
	// owned by each Lexer/Parser; destroying them leaves dangling pointers.
	inline static std::unique_ptr<antlr4::ANTLRInputStream> input_;
	inline static std::unique_ptr<CypherLexer> lexer_;
	inline static std::unique_ptr<antlr4::CommonTokenStream> tokens_;
	inline static std::unique_ptr<CypherParser> parser_;

	static void ensureParser() {
		if (!parser_) {
			input_ = std::make_unique<antlr4::ANTLRInputStream>("");
			lexer_ = std::make_unique<CypherLexer>(input_.get());
			tokens_ = std::make_unique<antlr4::CommonTokenStream>(lexer_.get());
			parser_ = std::make_unique<CypherParser>(tokens_.get());
		}
	}

	static bool validate(const std::string &query) {
		ensureParser();

		input_->load(query);
		lexer_->setInputStream(input_.get());
		tokens_->setTokenSource(lexer_.get());
		parser_->setTokenStream(tokens_.get());
		parser_->reset();

		lexer_->removeErrorListeners();
		parser_->removeErrorListeners();
		TestErrorListener lexerErrorListener;
		TestErrorListener parserErrorListener;
		lexer_->addErrorListener(&lexerErrorListener);
		parser_->addErrorListener(&parserErrorListener);

		parser_->cypher();

		return !lexerErrorListener.hasErrors() && !parserErrorListener.hasErrors();
	}
};
