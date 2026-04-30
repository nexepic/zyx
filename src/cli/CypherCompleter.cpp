/**
 * @file CypherCompleter.cpp
 * @brief Tab-completion implementation for the Cypher REPL.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#include "graph/cli/CypherCompleter.hpp"
#include "graph/cli/Repl.hpp"
#include "graph/query/expressions/ExpressionUtils.hpp"
#include "graph/query/expressions/FunctionRegistry.hpp"
#include "graph/query/planner/ProcedureRegistry.hpp"
#include "CypherLexer.h"
#include "antlr4-runtime.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace graph::cli {

CypherCompleter::CypherCompleter() {
	std::set<std::string> tokens;

	// 1. Cypher keywords from ANTLR4 Lexer vocabulary
	// Use an empty input stream instead of nullptr to avoid undefined behavior
	// in ANTLR4's Lexer destructor (crashes on MSVC debug builds).
	antlr4::ANTLRInputStream emptyInput("");
	CypherLexer lexer(&emptyInput);
	const auto& vocab = lexer.getVocabulary();
	for (size_t i = 1; i <= vocab.getMaxTokenType(); ++i) {
		auto sym = vocab.getSymbolicName(i);
		if (sym.empty()) continue;
		// Only include keyword tokens (K_ prefix)
		if (sym.size() > 2 && sym[0] == 'K' && sym[1] == '_') {
			auto lit = vocab.getLiteralName(i);
			if (lit.size() >= 2) {
				// Strip surrounding quotes
				std::string keyword(lit.substr(1, lit.size() - 2));
				tokens.insert(keyword);
			}
		}
	}

	// 2. Scalar functions from FunctionRegistry
	for (const auto& name : query::expressions::FunctionRegistry::getInstance().getRegisteredFunctionNames()) {
		tokens.insert(name);
	}

	// 3. Aggregate functions from ExpressionUtils
	for (const auto& name : query::expressions::ExpressionUtils::getAggregateFunctionNames()) {
		tokens.insert(name);
	}

	// 4. Procedures from ProcedureRegistry
	for (const auto& name : query::planner::ProcedureRegistry::instance().getRegisteredNames()) {
		tokens.insert(name);
	}

	// 5. REPL commands
	for (const auto& name : graph::REPL::getCommandNames()) {
		tokens.insert(name);
	}

	allTokens_.assign(tokens.begin(), tokens.end());
}

void CypherCompleter::complete(const std::string& line, std::vector<std::string>& completions) const {
	// Find the start of the last token (after whitespace, '(', or ',')
	size_t prefixStart = line.size();
	for (size_t i = line.size(); i > 0; --i) {
		char c = line[i - 1];
		if (c == ' ' || c == '\t' || c == '(' || c == ',') {
			break;
		}
		prefixStart = i - 1;
	}

	std::string prefix = line.substr(prefixStart);
	if (prefix.empty()) return;

	std::string prefixUpper = prefix;
	std::transform(prefixUpper.begin(), prefixUpper.end(), prefixUpper.begin(),
	               [](unsigned char c) { return std::toupper(c); });

	std::string lineBefore = line.substr(0, prefixStart);

	for (const auto& token : allTokens_) {
		std::string tokenUpper = token;
		std::transform(tokenUpper.begin(), tokenUpper.end(), tokenUpper.begin(),
		               [](unsigned char c) { return std::toupper(c); });

		if (tokenUpper.size() >= prefixUpper.size() &&
		    tokenUpper.compare(0, prefixUpper.size(), prefixUpper) == 0) {
			completions.push_back(lineBefore + token);
		}
	}
}

} // namespace graph::cli
