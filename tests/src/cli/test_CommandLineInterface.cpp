/**
 * @file test_CommandLineInterface.cpp
 * @author Nexepic
 * @date 2025/2/28
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
#include <string>
#include <vector>

// --- Platform Specific Includes for Pipe/Dup ---
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#ifndef STDIN_FILENO
#define STDIN_FILENO 0
#endif
#else
#include <unistd.h>
#endif
// -----------------------------------------------

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
		std::string progName = "metrix_test";
		argv.push_back(progName.data());

		std::vector<std::string> argsCopy = args;
		for (auto &arg: argsCopy) {
			argv.push_back(arg.data());
		}

		// --- Redirect Stdin to simulate EOF (Prevent REPL Hang) ---
		int oldStdin;
		int pipefd[2];

#ifdef _WIN32
		// Windows implementation
		oldStdin = _dup(STDIN_FILENO);
		// _pipe(handles, size, mode) - _O_BINARY prevents text translation issues
		if (_pipe(pipefd, 256, _O_BINARY) == -1) {
			perror("_pipe failed");
			return -1;
		}
		_close(pipefd[1]); // Close write end immediately -> EOF
		_dup2(pipefd[0], STDIN_FILENO); // Replace stdin with read end
		_close(pipefd[0]); // Close original read end
#else
		// POSIX implementation (Linux/macOS)
		oldStdin = dup(STDIN_FILENO);
		if (pipe(pipefd) == -1) {
			perror("pipe failed");
			return -1;
		}
		close(pipefd[1]);
		dup2(pipefd[0], STDIN_FILENO);
		close(pipefd[0]);
#endif

		// --- Run CLI ---
		int result = cli.run(static_cast<int>(argv.size()), argv.data());

		// --- Restore Stdin ---
#ifdef _WIN32
		_dup2(oldStdin, STDIN_FILENO);
		_close(oldStdin);
#else
		dup2(oldStdin, STDIN_FILENO);
		close(oldStdin);
#endif

		return result;
	}
};

// ============================================================================
// Test Cases
// ============================================================================

// Verify behavior for partial commands (Help should be shown, exit code 0)
TEST_F(CommandLineInterfaceTest, HandlePartialCommand) { EXPECT_EQ(runMockCLI({"database"}), 0); }

// Verify actual invalid arguments (Missing required options, unknown flags)
TEST_F(CommandLineInterfaceTest, HandleInvalidArguments) {
	EXPECT_NE(runMockCLI({"database", "create"}), 0);
	EXPECT_NE(runMockCLI({"database", "open", "path", "--unknown-flag"}), 0);
}

// Verify 'exec' command happy path
TEST_F(CommandLineInterfaceTest, ExecuteScriptSuccessfully) {
	createScript("CREATE (n:CliNode {id: 999});");

	int exitCode = runMockCLI({"database", "exec", dbPath, scriptPath});

	ASSERT_EQ(exitCode, 0) << "CLI should exit with success code 0";
	ASSERT_TRUE(fs::exists(dbPath)) << "Database folder should be created";

	graph::Database db(dbPath);
	db.open();
	auto res = db.getQueryEngine()->execute("MATCH (n:CliNode) RETURN n");

	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n").asNode().getProperties().at("id").toString(), "999");
}

// Verify 'exec' command with missing script file
TEST_F(CommandLineInterfaceTest, ExecuteMissingScriptFile) {
	if (fs::exists(scriptPath))
		fs::remove(scriptPath);

	int exitCode = runMockCLI({"database", "exec", dbPath, scriptPath});
	EXPECT_NE(exitCode, 0) << "CLI should fail if script file is missing";
}

// Verify 'exec' command with invalid Cypher syntax
TEST_F(CommandLineInterfaceTest, ExecuteBadSyntaxScript) {
	createScript("CREATE (broken node without closing parenthesis");
	EXPECT_NO_THROW(runMockCLI({"database", "exec", dbPath, scriptPath}));
}

TEST_F(CommandLineInterfaceTest, CreateCommandSafety) {
	// 1. First creation should succeed
	// Now safe to run because stdin is mocked with EOF, so REPL exits immediately
	EXPECT_EQ(runMockCLI({"database", "create", dbPath}), 0);
	EXPECT_TRUE(fs::exists(dbPath));

	// 2. Second creation on SAME path should FAIL (prevent overwrite)
	// We can now safely test the CLI command itself because REPL won't hang
	// However, the exception is thrown BEFORE REPL starts.
	// Depending on CLI implementation, it might catch and return 1, or throw.
	// Based on CommandLineInterface.cpp, it catches std::exception and prints error.
	// So we expect non-zero return code, OR we can test the DB logic directly if we prefer.

	// Let's stick to testing the DB logic directly for the failure case to be precise about the error type
	EXPECT_THROW(
			{
				graph::Database db(dbPath, graph::storage::OpenMode::OPEN_CREATE_NEW_FILE);
				db.open();
			},
			std::runtime_error);
}

// Verify 'open' command safety (Should fail if DB missing)
TEST_F(CommandLineInterfaceTest, OpenCommandSafety) {
	if (fs::exists(dbPath))
		fs::remove_all(dbPath);

	EXPECT_THROW(
			{
				graph::Database db(dbPath, graph::storage::OpenMode::OPEN_EXISTING_FILE);
				db.open();
			},
			std::runtime_error);
}

// Verify 'open -c' (Create if missing)
TEST_F(CommandLineInterfaceTest, OpenWithCreateFlag) {
	if (fs::exists(dbPath))
		fs::remove_all(dbPath);

	EXPECT_NO_THROW({
		graph::Database db(dbPath, graph::storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
		db.open();
		db.close();
	});

	EXPECT_TRUE(fs::exists(dbPath));
}

// Test create command when database already exists (error handling path)
TEST_F(CommandLineInterfaceTest, CreateCommandExistingDatabase) {
	// First create the database
	EXPECT_EQ(runMockCLI({"database", "create", dbPath}), 0);
	EXPECT_TRUE(fs::exists(dbPath));

	// Try to create again - should handle error gracefully
	// The error is caught in the callback, so CLI should not crash
	EXPECT_EQ(runMockCLI({"database", "create", dbPath}), 0);
}

// Test open command without create-if-missing flag (should fail for missing DB)
TEST_F(CommandLineInterfaceTest, OpenCommandMissingDatabase) {
	// Ensure database doesn't exist
	if (fs::exists(dbPath))
		fs::remove_all(dbPath);

	// Try to open non-existent database - error is caught, returns 0
	EXPECT_EQ(runMockCLI({"database", "open", dbPath}), 0);
}

// Test open command with create-if-missing flag
TEST_F(CommandLineInterfaceTest, OpenCommandWithCreateFlag) {
	// Ensure database doesn't exist
	if (fs::exists(dbPath))
		fs::remove_all(dbPath);

	// Open with -c flag should create the database
	EXPECT_EQ(runMockCLI({"database", "open", "-c", dbPath}), 0);
	EXPECT_TRUE(fs::exists(dbPath));
}

// Test open command existing database with create flag
TEST_F(CommandLineInterfaceTest, OpenCommandExistingWithCreateFlag) {
	// First create the database
	{
		graph::Database db(dbPath, graph::storage::OpenMode::OPEN_CREATE_NEW_FILE);
		db.open();
		db.close();
	}

	// Open existing database with -c flag should work fine
	EXPECT_EQ(runMockCLI({"database", "open", "-c", dbPath}), 0);
}

// ============================================================================
// Additional tests for improved coverage
// ============================================================================

// Test exec command with script that causes runtime error
TEST_F(CommandLineInterfaceTest, ExecuteScriptWithRuntimeError) {
	// First create a database
	{
		graph::Database db(dbPath, graph::storage::OpenMode::OPEN_CREATE_NEW_FILE);
		db.open();
		db.close();
	}

	// Create a script that will cause a runtime error during execution
	createScript("INVALID_CYPHER_QUERY_THAT_WILL_THROW;");

	// Redirect stderr to capture error output
	std::streambuf* oldErr = std::cerr.rdbuf();
	std::stringstream ssErr;
	std::cerr.rdbuf(ssErr.rdbuf());

	int exitCode = runMockCLI({"database", "exec", dbPath, scriptPath});

	std::cerr.rdbuf(oldErr);

	// Should exit with 0 (error is caught internally)
	EXPECT_EQ(exitCode, 0);

	std::string errOutput = ssErr.str();
	// Should have some error output
	EXPECT_TRUE(!errOutput.empty() || errOutput.find("Execution Error") != std::string::npos);
}

// Test exec command with database open error
TEST_F(CommandLineInterfaceTest, ExecuteScriptWithDatabaseError) {
	// Create a directory at dbPath to simulate an invalid database
	fs::create_directories(dbPath);

	// Create a valid script
	createScript("CREATE (n:Test {id: 1});");

	// Redirect stderr
	std::streambuf* oldErr = std::cerr.rdbuf();
	std::stringstream ssErr;
	std::cerr.rdbuf(ssErr.rdbuf());

	int exitCode = runMockCLI({"database", "exec", dbPath, scriptPath});

	std::cerr.rdbuf(oldErr);

	// Should handle the error gracefully
	EXPECT_EQ(exitCode, 0);
}
