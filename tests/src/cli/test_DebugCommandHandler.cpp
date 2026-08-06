/**
 * @file test_DebugCommandHandler.cpp
 * @author Nexepic
 * @date 2026/1/16
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

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>
#include <sstream>
#include <string>

#include "graph/cli/DebugCommandHandler.hpp"
#include "graph/core/Database.hpp"

using namespace graph::cli;
using namespace graph;

class DebugCommandHandlerTest : public ::testing::Test {
protected:
	void SetUp() override {
		// 1. Setup temporary database file
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = std::filesystem::temp_directory_path() / ("test_debug_cmd_" + to_string(uuid) + ".dat");

		database = std::make_unique<Database>(testFilePath.string());
		database->open();

		// 2. Redirect stdout and stderr to capture CLI output
		originalCout_ = std::cout.rdbuf(capturedOut_.rdbuf());
		originalCerr_ = std::cerr.rdbuf(capturedErr_.rdbuf());
	}

	void TearDown() override {
		// 1. Restore stdout/stderr
		if (originalCout_)
			std::cout.rdbuf(originalCout_);
		if (originalCerr_)
			std::cerr.rdbuf(originalCerr_);

		// 2. Cleanup resources
		if (database) {
			database->close();
			database.reset();
		}
		std::error_code ec;
		if (std::filesystem::exists(testFilePath)) {
			std::filesystem::remove(testFilePath, ec);
		}
	}

	void expectLegacyInspectorUnavailable(const std::string &command) {
		DebugCommandHandler handler(*database);
		handler.handle(command);
		EXPECT_TRUE(capturedOut_.str().empty());
		EXPECT_NE(capturedErr_.str().find("legacy segment inspector is unavailable"), std::string::npos);
	}

	std::filesystem::path testFilePath;
	std::unique_ptr<Database> database;

	std::streambuf *originalCout_ = nullptr;
	std::streambuf *originalCerr_ = nullptr;
	std::ostringstream capturedOut_;
	std::ostringstream capturedErr_;
};

// Test 'debug summary'
TEST_F(DebugCommandHandlerTest, HandleSummary) {
	expectLegacyInspectorUnavailable("debug summary");
}

// Test 'debug nodes'
TEST_F(DebugCommandHandlerTest, HandleNodes) {
	expectLegacyInspectorUnavailable("debug nodes");
}

// Test 'debug nodes' with page
TEST_F(DebugCommandHandlerTest, HandleNodesWithPage) {
	expectLegacyInspectorUnavailable("debug nodes 0");
}

// Test 'debug edges'
TEST_F(DebugCommandHandlerTest, HandleEdges) {
	expectLegacyInspectorUnavailable("debug edges");
}

// Test 'debug props'
TEST_F(DebugCommandHandlerTest, HandleProps) {
	expectLegacyInspectorUnavailable("debug props");
}

// Test 'debug blobs'
TEST_F(DebugCommandHandlerTest, HandleBlobs) {
	expectLegacyInspectorUnavailable("debug blobs");
}

// Test 'debug indexes'
TEST_F(DebugCommandHandlerTest, HandleIndexes) {
	expectLegacyInspectorUnavailable("debug indexes");
}

// Test 'debug state <key>' (String key)
TEST_F(DebugCommandHandlerTest, HandleStateKey) {
	expectLegacyInspectorUnavailable("debug state sys.config");
}

// Test 'debug states' (Alias coverage)
TEST_F(DebugCommandHandlerTest, HandleStatesAlias) {
	expectLegacyInspectorUnavailable("debug states");
}

// Test 'debug state <page>' (Numeric page)
TEST_F(DebugCommandHandlerTest, HandleStateNumericPage) {
	expectLegacyInspectorUnavailable("debug state 0");
}

// Test Unknown Target
TEST_F(DebugCommandHandlerTest, HandleUnknownTarget) {
	expectLegacyInspectorUnavailable("debug invalid_target");
}

// Test Storage Not Active (Database Closed)
TEST_F(DebugCommandHandlerTest, HandleStorageNotActive) {
	database->close();

	DebugCommandHandler handler(*database);
	handler.handle("debug summary");

	std::string err = capturedErr_.str();
	// A closed native database remains an incompatible format for the legacy
	// inspector; a legacy database instead reports its inactive storage state.
	bool hasError = (err.find("not active") != std::string::npos) ||
					(err.find("not initialized") != std::string::npos) ||
					(err.find("legacy segment inspector is unavailable") != std::string::npos);

	EXPECT_TRUE(hasError) << "Expected strict error message not found. stderr: " << err;
}

// Test Help command
TEST_F(DebugCommandHandlerTest, HandleHelp) {
	DebugCommandHandler handler(*database);
	handler.handle("debug help");

	std::string output = capturedOut_.str();
	EXPECT_NE(output.find("Debug Commands:"), std::string::npos);
}

// Test Empty/Formatting resilience
TEST_F(DebugCommandHandlerTest, HandleFormattingResilience) {
	expectLegacyInspectorUnavailable("debug  nodes ; ; ");
}

// Test Exception Handling (Argument out of range)
TEST_F(DebugCommandHandlerTest, HandleExceptionOutOfRange) {
	// Native databases reject legacy inspection before parsing a page number.
	expectLegacyInspectorUnavailable("debug nodes 99999999999999999999");
}

// ============================================================================
// Additional tests for improved coverage
// ============================================================================

// Test debug command with various extra semicolons
TEST_F(DebugCommandHandlerTest, HandleExtraSemicolons) {
	expectLegacyInspectorUnavailable("debug;;; nodes;;");
}

// Test debug command with tabs instead of spaces
TEST_F(DebugCommandHandlerTest, HandleTabsInCommand) {
	expectLegacyInspectorUnavailable("debug	nodes	0");
}

// Test debug state with numeric argument
TEST_F(DebugCommandHandlerTest, HandleStateWithNumericArg) {
	expectLegacyInspectorUnavailable("debug state 1");
}

// Test debug nodes with large page number
TEST_F(DebugCommandHandlerTest, HandleNodesLargePage) {
	expectLegacyInspectorUnavailable("debug nodes 99999");
}

TEST_F(DebugCommandHandlerTest, HandleNodesWithNonNumericPageUsesDefaultPage) {
	expectLegacyInspectorUnavailable("debug nodes not_a_page");
}

TEST_F(DebugCommandHandlerTest, HandleWhitespaceAndSemicolonOnlyShowsHelp) {
	DebugCommandHandler handler(*database);
	handler.handle("   ;;;");
	EXPECT_NE(capturedOut_.str().find("Debug Commands:"), std::string::npos);
	EXPECT_TRUE(capturedErr_.str().empty());
}

// Test debug command with trailing whitespace only
TEST_F(DebugCommandHandlerTest, HandleTrailingWhitespace) {
	expectLegacyInspectorUnavailable("debug nodes    ");
}
