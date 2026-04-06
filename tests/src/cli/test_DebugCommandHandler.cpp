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
#include "graph/core/Node.hpp"
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/data/NodeManager.hpp"

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

	// Helper to populate DB with a node so debug commands display data
	void addDummyNode() const {
		auto dm = database->getStorage()->getDataManager();
		auto nm = dm->getNodeManager();

		Node node;
		node.setLabelId(dm->getOrCreateLabelId("TestNode"));
		nm->add(node);

		(void) dm->prepareFlushSnapshot();
		dm->commitFlushSnapshot();
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
	DebugCommandHandler handler(*database);
	handler.handle("debug summary");
	EXPECT_FALSE(capturedOut_.str().empty());
}

// Test 'debug nodes'
TEST_F(DebugCommandHandlerTest, HandleNodes) {
	addDummyNode();
	DebugCommandHandler handler(*database);
	handler.handle("debug nodes");
	EXPECT_FALSE(capturedOut_.str().empty());
}

// Test 'debug nodes' with page
TEST_F(DebugCommandHandlerTest, HandleNodesWithPage) {
	DebugCommandHandler handler(*database);
	handler.handle("debug nodes 0");
	EXPECT_FALSE(capturedOut_.str().empty());
}

// Test 'debug edges'
TEST_F(DebugCommandHandlerTest, HandleEdges) {
	DebugCommandHandler handler(*database);
	handler.handle("debug edges");
	EXPECT_FALSE(capturedOut_.str().empty());
}

// Test 'debug props'
TEST_F(DebugCommandHandlerTest, HandleProps) {
	DebugCommandHandler handler(*database);
	handler.handle("debug props");
	EXPECT_FALSE(capturedOut_.str().empty());
}

// Test 'debug blobs'
TEST_F(DebugCommandHandlerTest, HandleBlobs) {
	DebugCommandHandler handler(*database);
	handler.handle("debug blobs");
	EXPECT_FALSE(capturedOut_.str().empty());
}

// Test 'debug indexes'
TEST_F(DebugCommandHandlerTest, HandleIndexes) {
	DebugCommandHandler handler(*database);
	handler.handle("debug indexes");
	EXPECT_FALSE(capturedOut_.str().empty());
}

// Test 'debug state <key>' (String key)
TEST_F(DebugCommandHandlerTest, HandleStateKey) {
	DebugCommandHandler handler(*database);
	handler.handle("debug state sys.config");
	EXPECT_FALSE(capturedOut_.str().empty());
}

// Test 'debug states' (Alias coverage)
TEST_F(DebugCommandHandlerTest, HandleStatesAlias) {
	DebugCommandHandler handler(*database);
	handler.handle("debug states");
	EXPECT_FALSE(capturedOut_.str().empty());
}

// Test 'debug state <page>' (Numeric page)
TEST_F(DebugCommandHandlerTest, HandleStateNumericPage) {
	DebugCommandHandler handler(*database);
	// Passing "0" triggers the inspectStateSegments path instead of inspectStateData
	handler.handle("debug state 0");
	EXPECT_FALSE(capturedOut_.str().empty());
}

// Test Unknown Target
TEST_F(DebugCommandHandlerTest, HandleUnknownTarget) {
	DebugCommandHandler handler(*database);
	handler.handle("debug invalid_target");

	std::string err = capturedErr_.str();
	EXPECT_NE(err.find("Unknown debug target"), std::string::npos);
}

// Test Storage Not Active (Database Closed)
TEST_F(DebugCommandHandlerTest, HandleStorageNotActive) {
	database->close();

	DebugCommandHandler handler(*database);
	handler.handle("debug summary");

	std::string err = capturedErr_.str();
	// Expect error about storage not active OR inspector not initialized
	bool hasError = (err.find("not active") != std::string::npos) || (err.find("not initialized") != std::string::npos);

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
	DebugCommandHandler handler(*database);
	handler.handle("debug  nodes ; ; ");
	EXPECT_FALSE(capturedOut_.str().empty());
	EXPECT_TRUE(capturedErr_.str().empty());
}

// Test Exception Handling (Argument out of range)
TEST_F(DebugCommandHandlerTest, HandleExceptionOutOfRange) {
	DebugCommandHandler handler(*database);

	// Provide a number strictly containing digits but too large for std::stoi/int.
	// This passes the `all_of(isdigit)` check but throws std::out_of_range during conversion.
	// 99999999999999999999 is larger than UINT64_MAX, definitely larger than int.
	handler.handle("debug nodes 99999999999999999999");

	std::string err = capturedErr_.str();
	// Expect the catch block to print "Debug Error: ..."
	EXPECT_NE(err.find("Debug Error:"), std::string::npos);
}

// ============================================================================
// Additional tests for improved coverage
// ============================================================================

// Test debug command with various extra semicolons
TEST_F(DebugCommandHandlerTest, HandleExtraSemicolons) {
	DebugCommandHandler handler(*database);
	handler.handle("debug;;; nodes;;");
	EXPECT_FALSE(capturedOut_.str().empty());
}

// Test debug command with tabs instead of spaces
TEST_F(DebugCommandHandlerTest, HandleTabsInCommand) {
	DebugCommandHandler handler(*database);
	handler.handle("debug	nodes	0");
	EXPECT_FALSE(capturedOut_.str().empty());
}

// Test debug state with numeric argument
TEST_F(DebugCommandHandlerTest, HandleStateWithNumericArg) {
	DebugCommandHandler handler(*database);
	handler.handle("debug state 1");
	EXPECT_FALSE(capturedOut_.str().empty());
}

// Test debug nodes with large page number
TEST_F(DebugCommandHandlerTest, HandleNodesLargePage) {
	DebugCommandHandler handler(*database);
	handler.handle("debug nodes 99999");
	EXPECT_FALSE(capturedOut_.str().empty());
}

// Test debug command with trailing whitespace only
TEST_F(DebugCommandHandlerTest, HandleTrailingWhitespace) {
	DebugCommandHandler handler(*database);
	handler.handle("debug nodes    ");
	EXPECT_FALSE(capturedOut_.str().empty());
}
