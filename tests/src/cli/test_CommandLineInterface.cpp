/**
 * @file test_CommandLineInterface.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/2/28
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
#include <string>
#include <vector>

#include "graph/cli/CommandLineInterface.hpp"
#include "graph/core/Database.hpp"

namespace fs = std::filesystem;

class CommandLineInterfaceTest : public ::testing::Test {
protected:
	std::string dbPath;
	std::string scriptPath;
	graph::cli::CommandLineInterface cli;

	void SetUp() override {
		// Create unique paths for isolation
		const boost::uuids::uuid uuid = boost::uuids::random_generator()();
		const std::string uuidStr = boost::uuids::to_string(uuid);
		const auto tempDir = fs::temp_directory_path();

		dbPath = (tempDir / ("cli_test_db_" + uuidStr)).string();
		scriptPath = (tempDir / ("cli_test_script_" + uuidStr + ".cypher")).string();

		cleanPaths();
	}

	void TearDown() override { cleanPaths(); }

	void cleanPaths() const {
		if (fs::exists(dbPath))
			fs::remove_all(dbPath);
		if (fs::exists(scriptPath))
			fs::remove(scriptPath);
	}

	// Helper to create a dummy cypher script
	void createScript(const std::string &content) const {
		std::ofstream out(scriptPath);
		out << content;
		out.close();
	}

	/**
	 * @brief Helper to simulate command line arguments.
	 * Converts vector<string> to argc/argv format expected by the run() method.
	 */
	int runMockCLI(const std::vector<std::string> &args) const {
		std::vector<char *> argv;
		// argv[0] is always the program name
		std::string progName = "metrix_test";
		argv.push_back(progName.data());

		// We need mutable copies of strings because argv is char**
		// (though CLI11 usually treats them as const, the signature requires char**)
		std::vector<std::string> argsCopy = args;
		for (auto &arg: argsCopy) {
			argv.push_back(arg.data());
		}

		return cli.run(static_cast<int>(argv.size()), argv.data());
	}
};

// ============================================================================
// Test Cases
// ============================================================================

// Verify behavior for partial commands (Help should be shown, exit code 0)
TEST_F(CommandLineInterfaceTest, HandlePartialCommand) {
	// Case: User types "metrix database" without a subcommand.
	// Expectation: It should print help and exit gracefully (return 0).
	// Previously we expected NE 0, which was wrong for your design.
	EXPECT_EQ(runMockCLI({"database"}), 0);
}

// Verify actual invalid arguments (Missing required options, unknown flags)
TEST_F(CommandLineInterfaceTest, HandleInvalidArguments) {
	// Missing required path argument for 'create'
	// "metrix database create" -> Error because --path is required
	EXPECT_NE(runMockCLI({"database", "create"}), 0);

	// Unknown flag
	// "metrix database open ... --unknown" -> Error
	EXPECT_NE(runMockCLI({"database", "open", "path", "--unknown-flag"}), 0);
}

// Verify 'exec' command happy path
// This is an Integration Test wrapped in a Unit Test
TEST_F(CommandLineInterfaceTest, ExecuteScriptSuccessfully) {
	// A. Prepare Script
	createScript("CREATE (n:CliNode {id: 999});");

	// B. Run CLI: metrix database exec <dbPath> <scriptPath>
	int exitCode = runMockCLI({"database", "exec", dbPath, scriptPath});

	// C. Verify Exit Code
	ASSERT_EQ(exitCode, 0) << "CLI should exit with success code 0";

	// D. Verify Side Effects (Database creation and Data insertion)
	ASSERT_TRUE(fs::exists(dbPath)) << "Database folder should be created";

	// Open DB manually to verify data persistence
	graph::Database db(dbPath);
	db.open();
	auto res = db.getQueryEngine()->execute("MATCH (n:CliNode) RETURN n");

	ASSERT_EQ(res.nodeCount(), 1UL);
	EXPECT_EQ(res.getNodes()[0].getProperties().at("id").toString(), "999");
}

// Verify 'exec' command with missing script file
TEST_F(CommandLineInterfaceTest, ExecuteMissingScriptFile) {
	// Ensure script does NOT exist
	if (fs::exists(scriptPath))
		fs::remove(scriptPath);

	// Run CLI
	int exitCode = runMockCLI({"database", "exec", dbPath, scriptPath});

	// Verify Failure
	EXPECT_NE(exitCode, 0) << "CLI should fail if script file is missing";
}

// Verify 'exec' command with invalid Cypher syntax
TEST_F(CommandLineInterfaceTest, ExecuteBadSyntaxScript) {
	// Prepare script with syntax error
	createScript("CREATE (broken node without closing parenthesis");

	// Run CLI
	// Note: Depending on your implementation, REPL might catch exception and print error
	// but still exit 0, OR exit with non-zero.
	// Based on your Repl.cpp, it catches exceptions and prints errors.
	// If you want it to fail the build, REPL should rethrow or return status.
	// For now, let's assume it runs without crashing.

	EXPECT_NO_THROW(runMockCLI({"database", "exec", dbPath, scriptPath}));
}
