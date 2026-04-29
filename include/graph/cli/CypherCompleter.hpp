/**
 * @file CypherCompleter.hpp
 * @brief Tab-completion for Cypher keywords, functions, procedures, and REPL commands.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once

#include <string>
#include <vector>

namespace graph::cli {

class CypherCompleter {
public:
	CypherCompleter();

	void complete(const std::string& line, std::vector<std::string>& completions) const;

private:
	std::vector<std::string> allTokens_; // sorted candidates
};

} // namespace graph::cli
