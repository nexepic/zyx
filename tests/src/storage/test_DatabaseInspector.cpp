/**
 * @file test_DatabaseInspector.cpp
 * @author Nexepic
 * @date 2025/12/23
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
#include <gtest/gtest.h>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "graph/core/Database.hpp"
#include "graph/storage/DatabaseInspector.hpp"
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/indexes/IndexManager.hpp"
#include "graph/storage/state/SystemStateManager.hpp"

using namespace graph;
using namespace graph::storage;

class DatabaseInspectorTest : public ::testing::Test {
protected:
	std::atomic<bool> dummyDeletionFlag{false};

	void SetUp() override {
		// 1. Unique path
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = std::filesystem::temp_directory_path() / ("test_inspect_" + to_string(uuid) + ".dat");

		// 2. Open DB
		database = std::make_unique<Database>(testFilePath.string());
		database->open();

		// 3. Populate
		populateDiverseData();

		// 4. Flush to disk to ensure data is inspectable.
		// We accept that compaction might run, so our tests must scan for data.
		database->getStorage()->flush();
	}

	void TearDown() override {
		if (database) {
			database->close();
			database.reset();
		}
		if (std::filesystem::exists(testFilePath)) {
			std::filesystem::remove(testFilePath);
		}
	}

	void populateDiverseData() const {
		auto dm = database->getStorage()->getDataManager();
		auto sm = database->getStorage()->getSystemStateManager();

		// 1. Nodes: Add enough to fill at least one full segment and start a second one.
		// This ensures Page 0 is full and Page 1 is partially empty (good for testing UNUSED slots).
		for (uint32_t i = 0; i < NODES_PER_SEGMENT + 1; ++i) {
			Node n(0, "InspectNode");
			dm->addNode(n);
		}

		// 2. Properties
		dm->addNodeProperties(1, {{"status", std::string("active")}, {"score", static_cast<int64_t>(100)}});

		// 3. Blobs (Large string > 32 bytes)
		std::string longData(1000, 'X');
		dm->addNodeProperties(2, {{"payload", longData}});

		// 4. Edges
		Edge e1(0, 1, 2, "CONNECTS");
		dm->addEdge(e1);

		// 5. Indexes
		(void) database->getQueryEngine()->getIndexManager()->createIndex("idx_status", "node", "InspectNode",
																		  "status");

		// 6. States
		// Use direct add to ensure at least one State entity exists
		State s(0, "test.config.key", "some_data");
		dm->addStateEntity(s);
	}

	static std::string captureOutput(const std::function<void()> &func) {
		std::stringstream buffer;
		std::streambuf *old = std::cout.rdbuf(buffer.rdbuf());
		func();
		std::cout.rdbuf(old);
		return buffer.str();
	}

	/**
	 * @brief Scans through multiple pages to find the expected content.
	 *        This makes the test resilient to Compaction moving data between pages.
	 */
	static bool contentExistsInAnyPage(const std::function<void(uint32_t)> &inspectFunc, const std::string &target) {
		// Scan up to 5 pages. If data isn't in the first 5 pages of a small test DB, something is wrong.
		for (uint32_t page = 0; page < 5; ++page) {
			std::string output = captureOutput([&]() { inspectFunc(page); });

			// If we hit a non-existent page, stop scanning this chain
			if (output.find("does not exist") != std::string::npos) {
				return false;
			}

			// Found it!
			if (output.find(target) != std::string::npos) {
				return true;
			}
		}
		return false;
	}

	std::filesystem::path testFilePath;
	std::unique_ptr<Database> database;
};

// ============================================================================
// Test Cases
// ============================================================================

TEST_F(DatabaseInspectorTest, InspectSummary) {
	auto inspector = database->getStorage()->getInspector();
	std::string output = captureOutput([&]() { inspector->inspectSummary(); });

	EXPECT_NE(output.find("Database Summary"), std::string::npos);
	EXPECT_NE(output.find("Nodes"), std::string::npos);
}

TEST_F(DatabaseInspectorTest, InspectNodeSegments) {
	auto inspector = database->getStorage()->getInspector();

	// We look for the label "InspectNode" which we inserted many times.
	bool found = contentExistsInAnyPage([&](uint32_t p) { inspector->inspectNodeSegments(p); }, "InspectNode");
	EXPECT_TRUE(found) << "Failed to find 'InspectNode' in Node segments.";
}

TEST_F(DatabaseInspectorTest, InspectEdgeSegments) {
	auto inspector = database->getStorage()->getInspector();

	bool found = contentExistsInAnyPage([&](uint32_t p) { inspector->inspectEdgeSegments(p); }, "CONNECTS");
	EXPECT_TRUE(found) << "Failed to find 'CONNECTS' label in Edge segments.";
}

TEST_F(DatabaseInspectorTest, InspectPropertySegments) {
	auto inspector = database->getStorage()->getInspector();

	bool found = contentExistsInAnyPage([&](uint32_t p) { inspector->inspectPropertySegments(p); }, "status");
	EXPECT_TRUE(found) << "Failed to find property key 'status'.";
}

TEST_F(DatabaseInspectorTest, InspectBlobSegments) {
	auto inspector = database->getStorage()->getInspector();

	// Blobs don't have labels, but they should be marked ACTIVE
	bool found = contentExistsInAnyPage([&](uint32_t p) { inspector->inspectBlobSegments(p); }, "ACTIVE");
	EXPECT_TRUE(found) << "Failed to find any ACTIVE Blob segment.";
}

TEST_F(DatabaseInspectorTest, InspectIndexSegments) {
	auto inspector = database->getStorage()->getInspector();

	bool found = contentExistsInAnyPage([&](uint32_t p) { inspector->inspectIndexSegments(p); }, "ACTIVE");
	EXPECT_TRUE(found) << "Failed to find any ACTIVE Index node.";
}

TEST_F(DatabaseInspectorTest, InspectStateSegments) {
	auto inspector = database->getStorage()->getInspector();

	bool found = contentExistsInAnyPage([&](uint32_t p) { inspector->inspectStateSegments(p); }, "test.config.key");
	EXPECT_TRUE(found) << "Failed to find 'test.config.key' in State segments.";
}

TEST_F(DatabaseInspectorTest, PaginationOutOfBounds) {
	auto inspector = database->getStorage()->getInspector();

	// Node (already covered)
	std::string output = captureOutput([&]() { inspector->inspectNodeSegments(999); });
	EXPECT_NE(output.find("does not exist"), std::string::npos);

	// Edge
	output = captureOutput([&]() { inspector->inspectEdgeSegments(999); });
	EXPECT_NE(output.find("does not exist"), std::string::npos);

	// Property
	output = captureOutput([&]() { inspector->inspectPropertySegments(999); });
	EXPECT_NE(output.find("does not exist"), std::string::npos);

	// Blob
	output = captureOutput([&]() { inspector->inspectBlobSegments(999); });
	EXPECT_NE(output.find("does not exist"), std::string::npos);

	// Index
	output = captureOutput([&]() { inspector->inspectIndexSegments(999); });
	EXPECT_NE(output.find("does not exist"), std::string::npos);

	// State
	output = captureOutput([&]() { inspector->inspectStateSegments(999); });
	EXPECT_NE(output.find("does not exist"), std::string::npos);
}

TEST_F(DatabaseInspectorTest, ShowUnusedSlots) {
	auto inspector = database->getStorage()->getInspector();

	// We populated NODES_PER_SEGMENT + 1 nodes.
	// Page 0 is full. Page 1 has 1 node, so it MUST have unused slots (assuming capacity > 1).
	// We inspect Page 1 specifically.

	std::string output = captureOutput([&]() { inspector->inspectNodeSegments(1, true); });

	// We expect to find at least one "UNUSED" row
	EXPECT_NE(output.find("UNUSED"), std::string::npos)
			<< "Page 1 should contain UNUSED slots given the population strategy.";
}

TEST_F(DatabaseInspectorTest, InspectEmptyDatabase) {
	boost::uuids::uuid uuid = boost::uuids::random_generator()();
	auto emptyPath = std::filesystem::temp_directory_path() / ("empty_db_" + to_string(uuid) + ".dat");
	{
		Database db(emptyPath.string());
		db.open();
		// Flush empty DB to write header
		db.getStorage()->flush();

		auto inspector = db.getStorage()->getInspector();
		std::string output = captureOutput([&]() { inspector->inspectNodeSegments(0); });

		bool handled =
				(output.find("does not exist") != std::string::npos) || (output.find("Used: 0") != std::string::npos);
		EXPECT_TRUE(handled);
	}
	std::filesystem::remove(emptyPath);
}
