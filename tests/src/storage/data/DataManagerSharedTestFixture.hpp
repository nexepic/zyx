/**
 * @file DataManagerSharedTestFixture.hpp
 * @date 2026/4/15
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

#pragma once

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>
#include "graph/core/Blob.hpp"
#include "graph/core/Database.hpp"
#include "graph/core/Edge.hpp"
#include "graph/core/Index.hpp"
#include "graph/core/Node.hpp"
#include "graph/core/Property.hpp"
#include "graph/core/State.hpp"
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/data/EntityChangeType.hpp"
#include "graph/storage/indexes/EntityTypeIndexManager.hpp"
#include "graph/storage/indexes/IEntityObserver.hpp"
#include "graph/storage/indexes/IndexManager.hpp"

using namespace graph;
using namespace graph::storage;

// Test observer to track notifications from DataManager (shared variant)
class SharedTestEntityObserver final : public IEntityObserver {
public:
	void onNodeAdded(const Node &node) override { addedNodes.push_back(node); }

	void onNodeUpdated(const Node &oldNode, const Node &newNode) override {
		updatedNodes.emplace_back(oldNode, newNode);
	}

	void onNodeDeleted(const Node &node) override { deletedNodes.push_back(node); }

	void onEdgeAdded(const Edge &edge) override { addedEdges.push_back(edge); }

	void onEdgeUpdated(const Edge &oldEdge, const Edge &newEdge) override {
		updatedEdges.emplace_back(oldEdge, newEdge);
	}

	void onEdgeDeleted(const Edge &edge) override { deletedEdges.push_back(edge); }

	std::vector<Node> addedNodes;
	std::vector<std::pair<Node, Node>> updatedNodes;
	std::vector<Node> deletedNodes;
	std::vector<Edge> addedEdges;
	std::vector<std::pair<Edge, Edge>> updatedEdges;
	std::vector<Edge> deletedEdges;

	void reset() {
		addedNodes.clear();
		updatedNodes.clear();
		deletedNodes.clear();
		addedEdges.clear();
		updatedEdges.clear();
		deletedEdges.clear();
	}
};

/**
 * Shared test fixture that opens one Database per test suite
 * instead of per test case, reducing overhead for tests that
 * don't require isolated DB state.
 */
class DataManagerSharedTest : public ::testing::Test {
protected:
	static void SetUpTestSuite() {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath_ = std::filesystem::temp_directory_path() / ("test_dm_shared_" + to_string(uuid) + ".dat");

		database_ = std::make_unique<graph::Database>(testFilePath_.string());
		database_->open();
		fileStorage_ = database_->getStorage();
		dataManager_ = fileStorage_->getDataManager();

		observer_ = std::make_shared<SharedTestEntityObserver>();
		dataManager_->registerObserver(observer_);
	}

	static void TearDownTestSuite() {
		observer_.reset();
		dataManager_.reset();
		fileStorage_.reset();
		if (database_) {
			database_->close();
			database_.reset();
		}
		std::error_code ec;
		if (std::filesystem::exists(testFilePath_)) {
			std::filesystem::remove(testFilePath_, ec);
		}
	}

	void SetUp() override {
		observer_->reset();
		dataManager_->clearCache();
	}

	void TearDown() override {
		// Don't flush — tests that need persistence do their own flush.
		// Just clear dirty state to keep the DB consistent for the next test.
		dataManager_->clearCache();
	}

	// Helper to simulate "Save" (Flush to disk and clear dirty state)
	static void simulateSave() {
		fileStorage_->flush();
	}

	// Helper methods to create test entities.
	static Node createTestNode(const std::shared_ptr<DataManager> &dm, const std::string &label = "TestNode") {
		Node node;
		node.setLabelId(dm->getOrCreateTokenId(label));
		return node;
	}

	static Edge createTestEdge(const std::shared_ptr<DataManager> &dm, int64_t sourceId, int64_t targetId,
							   const std::string &label = "TestEdge") {
		Edge edge;
		edge.setSourceNodeId(sourceId);
		edge.setTargetNodeId(targetId);
		edge.setTypeId(dm->getOrCreateTokenId(label));
		return edge;
	}

	static Property createTestProperty(int64_t entityId, uint32_t entityType,
									   const std::unordered_map<std::string, PropertyValue> &properties) {
		Property property;
		property.getMutableMetadata().entityId = entityId;
		property.getMutableMetadata().entityType = entityType;
		property.setProperties(properties);
		property.getMutableMetadata().isActive = true;
		return property;
	}

	static Blob createTestBlob(const std::string &data) {
		Blob blob;
		blob.setData(data);
		blob.getMutableMetadata().isActive = true;
		return blob;
	}

	static Index createTestIndex(Index::NodeType nodeType, uint32_t indexType) {
		Index index(0, nodeType, indexType);
		index.getMutableMetadata().isActive = true;
		return index;
	}

	static State createTestState(const std::string &key) {
		State state;
		state.setKey(key);
		return state;
	}

	// Static members shared across all tests in the suite
	static std::filesystem::path testFilePath_;
	static std::unique_ptr<Database> database_;
	static std::shared_ptr<FileStorage> fileStorage_;
	static std::shared_ptr<DataManager> dataManager_;
	static std::shared_ptr<SharedTestEntityObserver> observer_;

	// Convenience accessors (match the per-test fixture's member names)
	std::shared_ptr<DataManager> &dataManager = dataManager_;
	std::shared_ptr<FileStorage> &fileStorage = fileStorage_;
	std::shared_ptr<SharedTestEntityObserver> &observer = observer_;
};

// Static member definitions — must be in a single translation unit.
// Each test file that includes this header needs to define these once.
// We use inline to allow multiple TU inclusion (C++17).
inline std::filesystem::path DataManagerSharedTest::testFilePath_;
inline std::unique_ptr<Database> DataManagerSharedTest::database_;
inline std::shared_ptr<FileStorage> DataManagerSharedTest::fileStorage_;
inline std::shared_ptr<DataManager> DataManagerSharedTest::dataManager_;
inline std::shared_ptr<SharedTestEntityObserver> DataManagerSharedTest::observer_;
