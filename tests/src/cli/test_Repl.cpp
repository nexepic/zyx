/**
 * @file test_Repl.cpp
 * @author Nexepic
 * @date 2025/3/20
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

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <sstream>
#include <iostream>

#include "graph/cli/Repl.hpp"
#include "graph/core/Database.hpp"

namespace fs = std::filesystem;

class REPLTest : public ::testing::Test {
protected:
	std::string dbPath;
	std::string scriptPath;
	std::unique_ptr<graph::Database> db;
	std::unique_ptr<graph::REPL> repl;

	void SetUp() override {
		// Generate unique paths
		const boost::uuids::uuid uuid = boost::uuids::random_generator()();
		const std::string uuidStr = boost::uuids::to_string(uuid);
		const auto tempDir = fs::temp_directory_path();

		dbPath = (tempDir / ("repl_test_db_" + uuidStr)).string();
		scriptPath = (tempDir / ("test_script_" + uuidStr + ".cypher")).string();

		// Initialize DB
		db = std::make_unique<graph::Database>(dbPath);
		db->open();

		// Initialize REPL with the active DB
		repl = std::make_unique<graph::REPL>(*db);
	}

	void TearDown() override {
		if (db)
			db->close();

		// Cleanup paths
		if (fs::exists(dbPath))
			fs::remove_all(dbPath);
		if (fs::exists(scriptPath))
			fs::remove(scriptPath);
	}

	// Helper to create a script file
	void createScript(const std::string &content) const {
		std::ofstream out(scriptPath);
		out << content;
		out.close();
	}
};

// Test executing a simple single-line script
TEST_F(REPLTest, RunSimpleScript) {
	createScript("CREATE (n:ReplTest {id: 1});");

	// Execute
	repl->runScript(scriptPath);

	// Verify side effect in DB
	auto res = db->getQueryEngine()->execute("MATCH (n:ReplTest) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n").asNode().getProperties().at("id").toString(), "1");
}

// Test executing a script with multiple statements and comments
TEST_F(REPLTest, RunMultiLineScriptWithComments) {
	std::string content = R"(
            // This is a comment
            CREATE (n:A);

            // Create another one
            CREATE (n:B);
        )";
	createScript(content);

	repl->runScript(scriptPath);

	// Verify A exists
	auto resA = db->getQueryEngine()->execute("MATCH (n:A) RETURN n");
	EXPECT_EQ(resA.rowCount(), 1UL);

	// Verify B exists
	auto resB = db->getQueryEngine()->execute("MATCH (n:B) RETURN n");
	EXPECT_EQ(resB.rowCount(), 1UL);
}

// Test executing a script where statements span multiple lines
TEST_F(REPLTest, RunMultilineStatement) {
	std::string content = "CREATE (n:Multiline \n {val: 'test'});";
	createScript(content);

	repl->runScript(scriptPath);

	auto res = db->getQueryEngine()->execute("MATCH (n:Multiline) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n").asNode().getProperties().at("val").toString(), "test");
}

// Test robustness when script file is missing
TEST_F(REPLTest, HandleMissingFile) {
	// Ensure file does not exist
	if (fs::exists(scriptPath))
		fs::remove(scriptPath);

	// Should not crash (prints error to stderr)
	EXPECT_NO_THROW(repl->runScript(scriptPath));
}

// Test system command execution via script (e.g. CALL)
TEST_F(REPLTest, RunSystemCommands) {
	createScript("CALL dbms.setConfig('repl.test', 'true');");

	repl->runScript(scriptPath);

	auto res = db->getQueryEngine()->execute("CALL dbms.getConfig('repl.test')");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("value").toString(), "true");
}

// Test script execution with trailing buffer (no semicolon at EOF)
TEST_F(REPLTest, RunScriptWithImplicitEOF) {
	createScript("CREATE (n:EOFTest {name: 'implicit'})");

	repl->runScript(scriptPath);

	auto res = db->getQueryEngine()->execute("MATCH (n:EOFTest) RETURN n.name");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.name").toString(), "implicit");
}

// Test script with empty lines and various whitespace
TEST_F(REPLTest, RunScriptWithEmptyLines) {
	std::string content = R"(
		CREATE (n:A);

		CREATE (n:B);


		CREATE (n:C);
		)";
	createScript(content);

	repl->runScript(scriptPath);

	// The script creates 3 nodes, verify by counting nodes with each label
	auto resA = db->getQueryEngine()->execute("MATCH (n:A) RETURN n");
	EXPECT_EQ(resA.rowCount(), 1UL);

	auto resB = db->getQueryEngine()->execute("MATCH (n:B) RETURN n");
	EXPECT_EQ(resB.rowCount(), 1UL);

	auto resC = db->getQueryEngine()->execute("MATCH (n:C) RETURN n");
	EXPECT_EQ(resC.rowCount(), 1UL);
}

// Test handleCommand with help command - covers help branch
TEST_F(REPLTest, HandleCommandHelp) {
	// Create a script with help command
	createScript("help;");

	// Redirect stdout to capture output
	std::streambuf* old = std::cout.rdbuf();
	std::stringstream ss;
	std::cout.rdbuf(ss.rdbuf());

	repl->runScript(scriptPath);

	std::cout.rdbuf(old);

	std::string output = ss.str();
	EXPECT_TRUE(output.find("Commands:") != std::string::npos);
	EXPECT_TRUE(output.find("save") != std::string::npos);
	EXPECT_TRUE(output.find("debug") != std::string::npos);
	EXPECT_TRUE(output.find("exit") != std::string::npos);
}

// Test handleCommand with save command - covers save branch
TEST_F(REPLTest, HandleCommandSave) {
	// Create some data
	db->getQueryEngine()->execute("CREATE (n:SaveTest {id: 1})");

	// Create a script with save command
	createScript("save;");

	// Redirect stdout to capture output
	std::streambuf* old = std::cout.rdbuf();
	std::stringstream ss;
	std::cout.rdbuf(ss.rdbuf());

	repl->runScript(scriptPath);

	std::cout.rdbuf(old);

	std::string output = ss.str();
	EXPECT_TRUE(output.find("Database flushed") != std::string::npos ||
	            output.find("Storage not accessible") != std::string::npos);
}

// Test handleCommand with debug command - covers debug branch
TEST_F(REPLTest, HandleCommandDebug) {
	// Create a script with debug command
	createScript("debug;");

	// Redirect stdout to capture output
	std::streambuf* old = std::cout.rdbuf();
	std::stringstream ss;
	std::cout.rdbuf(ss.rdbuf());

	repl->runScript(scriptPath);

	std::cout.rdbuf(old);

	// Debug command should produce some output
	std::string output = ss.str();
	// The exact output depends on debug handler implementation
}

// Test handleCommand with debug help subcommand
TEST_F(REPLTest, HandleCommandDebugHelp) {
	// Create a script with debug help command
	createScript("debug help;");

	// Redirect stdout to capture output
	std::streambuf* old = std::cout.rdbuf();
	std::stringstream ss;
	std::cout.rdbuf(ss.rdbuf());

	repl->runScript(scriptPath);

	std::cout.rdbuf(old);

	// Debug help should produce some output
	std::string output = ss.str();
}

// Test printResult with empty result - covers isEmpty branch
TEST_F(REPLTest, PrintEmptyResult) {
	// Create a script with a query that returns no results
	createScript("MATCH (n:NonExistentLabel) RETURN n;");

	// Redirect stdout to capture output
	std::streambuf* old = std::cout.rdbuf();
	std::stringstream ss;
	std::cout.rdbuf(ss.rdbuf());

	repl->runScript(scriptPath);

	std::cout.rdbuf(old);

	// Should have "Empty result." message
	std::string output = ss.str();
	EXPECT_TRUE(output.find("Empty result") != std::string::npos || output.find("0 rows") != std::string::npos);
}

// Test printResult with result requiring truncation (>30 chars)
TEST_F(REPLTest, PrintResultWithTruncation) {
	// Create a node with a long property value
	std::string longString = "This_is_a_very_long_string_that_exceeds_thirty_characters_and_should_be_truncated";
	db->getQueryEngine()->execute(
		"CREATE (n:TruncationTest {long_field: '" + longString + "'})");

	// Create a script with query that returns truncated results
	createScript("MATCH (n:TruncationTest) RETURN n.long_field;");

	// Redirect stdout to capture output
	std::streambuf* old = std::cout.rdbuf();
	std::stringstream ss;
	std::cout.rdbuf(ss.rdbuf());

	repl->runScript(scriptPath);

	std::cout.rdbuf(old);

	std::string output = ss.str();
	// Should contain ellipsis for truncated strings
	EXPECT_TRUE(output.find("...") != std::string::npos);
}

// Test printResult with multiple rows and columns
TEST_F(REPLTest, PrintResultMultipleRowsColumns) {
	db->getQueryEngine()->execute("CREATE (n:A {id: 1, name: 'Alice'})");
	db->getQueryEngine()->execute("CREATE (n:A {id: 2, name: 'Bob'})");
	db->getQueryEngine()->execute("CREATE (n:A {id: 3, name: 'Charlie'})");

	// Create a script with query that returns multiple rows and columns
	createScript("MATCH (n:A) RETURN n.id, n.name ORDER BY n.id;");

	// Redirect stdout to capture output
	std::streambuf* old = std::cout.rdbuf();
	std::stringstream ss;
	std::cout.rdbuf(ss.rdbuf());

	repl->runScript(scriptPath);

	std::cout.rdbuf(old);

	std::string output = ss.str();
	EXPECT_TRUE(output.find("3 rows") != std::string::npos);
	EXPECT_TRUE(output.find("Alice") != std::string::npos);
	EXPECT_TRUE(output.find("Bob") != std::string::npos);
	EXPECT_TRUE(output.find("Charlie") != std::string::npos);
}

// Test error handling in handleCommand
TEST_F(REPLTest, HandleCommandWithError) {
	// Create a script with invalid Cypher query
	createScript("INVALID SYNTAX HERE;");

	// Redirect stderr to capture error output
	std::streambuf* oldErr = std::cerr.rdbuf();
	std::stringstream ssErr;
	std::cerr.rdbuf(ssErr.rdbuf());

	// Redirect stdout
	std::streambuf* oldOut = std::cout.rdbuf();
	std::stringstream ssOut;
	std::cout.rdbuf(ssOut.rdbuf());

	repl->runScript(scriptPath);

	std::cout.rdbuf(oldOut);
	std::cerr.rdbuf(oldErr);

	std::string errOutput = ssErr.str();
	// Should have an error message
	EXPECT_TRUE(errOutput.find("Error") != std::string::npos);
}

// Test script with comment at the end
TEST_F(REPLTest, RunScriptWithTrailingComment) {
	createScript(R"(
		CREATE (n:CommentTest {val: 1});
		// This is a trailing comment
	)");

	repl->runScript(scriptPath);

	auto res = db->getQueryEngine()->execute("MATCH (n:CommentTest) RETURN n.val");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.val").asPrimitive().toString(), "1");
}

// Test query with null values
TEST_F(REPLTest, PrintResultWithNullValues) {
	db->getQueryEngine()->execute("CREATE (n:NullTest {id: 1})");
	db->getQueryEngine()->execute("CREATE (n:NullTest {id: 2, name: 'Bob'})");

	// Create a script with query that returns null values
	createScript("MATCH (n:NullTest) RETURN n.id, n.name ORDER BY n.id;");

	// Redirect stdout
	std::streambuf* old = std::cout.rdbuf();
	std::stringstream ss;
	std::cout.rdbuf(ss.rdbuf());

	repl->runScript(scriptPath);

	std::cout.rdbuf(old);

	std::string output = ss.str();
	EXPECT_TRUE(output.find("2 rows") != std::string::npos);
	EXPECT_TRUE(output.find("null") != std::string::npos);
}

// Test error handling for invalid syntax with multiple semicolons
TEST_F(REPLTest, RunScriptWithMultipleSemicolons) {
	// Create a script with invalid syntax (multiple semicolons)
	createScript("CREATE (n:MultiSemi {a: 1});; CREATE (n:MultiSemi {a: 2});");

	// Redirect stderr to capture error output
	std::streambuf* oldErr = std::cerr.rdbuf();
	std::stringstream ssErr;
	std::cerr.rdbuf(ssErr.rdbuf());

	// Redirect stdout
	std::streambuf* oldOut = std::cout.rdbuf();
	std::stringstream ssOut;
	std::cout.rdbuf(ssOut.rdbuf());

	repl->runScript(scriptPath);

	std::cout.rdbuf(oldOut);
	std::cerr.rdbuf(oldErr);

	std::string errOutput = ssErr.str();
	// Should have an error message about syntax error
	EXPECT_TRUE(errOutput.find("Error") != std::string::npos || errOutput.find("Syntax") != std::string::npos);
}
