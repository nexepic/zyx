/**
 * @file test_CypherCompleter.cpp
 * @brief Tests for CypherCompleter tab-completion.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#include <gtest/gtest.h>
#include "graph/cli/CypherCompleter.hpp"
#include <algorithm>

using namespace graph::cli;

class CypherCompleterTest : public ::testing::Test {
protected:
	CypherCompleter completer;

	std::vector<std::string> complete(const std::string& line) {
		std::vector<std::string> completions;
		completer.complete(line, completions);
		return completions;
	}

	bool containsCompletion(const std::vector<std::string>& completions, const std::string& expected) {
		return std::find(completions.begin(), completions.end(), expected) != completions.end();
	}
};

TEST_F(CypherCompleterTest, EmptyInput) {
	auto completions = complete("");
	EXPECT_TRUE(completions.empty());
}

TEST_F(CypherCompleterTest, KeywordCompletion) {
	auto completions = complete("MAT");
	EXPECT_TRUE(containsCompletion(completions, "MATCH"));
}

TEST_F(CypherCompleterTest, KeywordCompletionCaseInsensitive) {
	auto completions = complete("mat");
	EXPECT_TRUE(containsCompletion(completions, "MATCH"));
}

TEST_F(CypherCompleterTest, KeywordAfterSpace) {
	auto completions = complete("MATCH (n) RET");
	EXPECT_TRUE(containsCompletion(completions, "MATCH (n) RETURN"));
}

TEST_F(CypherCompleterTest, MultipleMatches) {
	// "DE" should match DELETE, DETACH, DESC, DESCENDING
	auto completions = complete("DE");
	EXPECT_GE(completions.size(), 2u);
	EXPECT_TRUE(containsCompletion(completions, "DELETE"));
	EXPECT_TRUE(containsCompletion(completions, "DETACH"));
}

TEST_F(CypherCompleterTest, NoMatch) {
	auto completions = complete("ZZZZZ");
	EXPECT_TRUE(completions.empty());
}

TEST_F(CypherCompleterTest, ReplCommands) {
	auto completions = complete("hel");
	EXPECT_TRUE(containsCompletion(completions, "help"));
}

TEST_F(CypherCompleterTest, AggregateFunctions) {
	auto completions = complete("cou");
	// Should find both COUNT (keyword) and count (aggregate function)
	bool found = false;
	for (const auto& c : completions) {
		std::string upper = c;
		std::transform(upper.begin(), upper.end(), upper.begin(),
		               [](unsigned char ch) { return std::toupper(ch); });
		if (upper == "COUNT") {
			found = true;
			break;
		}
	}
	EXPECT_TRUE(found);
}

TEST_F(CypherCompleterTest, ProcedureCompletion) {
	auto completions = complete("dbms.");
	// Should find dbms.* procedures
	EXPECT_FALSE(completions.empty());
}

TEST_F(CypherCompleterTest, AfterOpenParen) {
	auto completions = complete("RETURN count(DI");
	EXPECT_TRUE(containsCompletion(completions, "RETURN count(DISTINCT"));
}

TEST_F(CypherCompleterTest, AfterComma) {
	auto completions = complete("RETURN a,MAT");
	EXPECT_TRUE(containsCompletion(completions, "RETURN a,MATCH"));
}

TEST_F(CypherCompleterTest, CompletionsAreSorted) {
	auto completions = complete("A");
	// Verify the completions are in sorted order
	for (size_t i = 1; i < completions.size(); ++i) {
		EXPECT_LE(completions[i - 1], completions[i]);
	}
}
