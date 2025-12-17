/**
 * @file test_Repl.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/3/20
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

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
	ASSERT_EQ(res.nodeCount(), 1);
	EXPECT_EQ(res.getNodes()[0].getProperties().at("id").toString(), "1");
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
	EXPECT_EQ(resA.nodeCount(), 1);

	// Verify B exists
	auto resB = db->getQueryEngine()->execute("MATCH (n:B) RETURN n");
	EXPECT_EQ(resB.nodeCount(), 1);
}

// Test executing a script where statements span multiple lines
TEST_F(REPLTest, RunMultilineStatement) {
	std::string content = "CREATE (n:Multiline \n {val: 'test'});";
	createScript(content);

	repl->runScript(scriptPath);

	auto res = db->getQueryEngine()->execute("MATCH (n:Multiline) RETURN n");
	ASSERT_EQ(res.nodeCount(), 1);
	EXPECT_EQ(res.getNodes()[0].getProperties().at("val").toString(), "test");
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
	ASSERT_EQ(res.rowCount(), 1);
	EXPECT_EQ(res.getRows()[0].at("value").toString(), "true");
}
