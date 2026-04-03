/**
 * @file test_ConstraintManager_EdgeAndLifecycle.cpp
 * @brief Focused lifecycle and edge-entity branch tests for ConstraintManager.
 */

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "graph/core/Database.hpp"
#include "graph/storage/constraints/ConstraintManager.hpp"
#include "graph/storage/indexes/IndexManager.hpp"

namespace fs = std::filesystem;

using graph::Edge;
using graph::Node;
using graph::PropertyValue;
using graph::query::indexes::IndexManager;
using graph::storage::DataManager;
using graph::storage::constraints::ConstraintManager;

class ConstraintManagerEdgeAndLifecycleTest : public ::testing::Test {
protected:
	fs::path dbPath;
	std::unique_ptr<graph::Database> db;
	std::shared_ptr<DataManager> dataManager;
	std::shared_ptr<IndexManager> indexManager;
	std::shared_ptr<ConstraintManager> manager;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		dbPath = fs::temp_directory_path() / ("test_constraint_edge_lifecycle_" + boost::uuids::to_string(uuid) + ".db");
		if (fs::exists(dbPath)) {
			fs::remove(dbPath);
		}

		db = std::make_unique<graph::Database>(dbPath.string());
		db->open();
		dataManager = db->getStorage()->getDataManager();
		indexManager = db->getQueryEngine()->getIndexManager();
		manager = std::make_shared<ConstraintManager>(db->getStorage(), indexManager);
		manager->initialize();
	}

	void TearDown() override {
		if (db) {
			db->close();
		}
		if (fs::exists(dbPath)) {
			fs::remove(dbPath);
		}
	}

	int64_t addNodeWithLabel(const std::string &label) {
		Node node(0, dataManager->getOrCreateLabelId(label));
		dataManager->addNode(node);
		return node.getId();
	}
};

TEST_F(ConstraintManagerEdgeAndLifecycleTest, PersistStateAndDeleteValidatorsAreReachable) {
	Node node(1, dataManager->getOrCreateLabelId("Person"));
	Edge edge(1, 1, 2, dataManager->getOrCreateLabelId("REL"));

	EXPECT_NO_THROW(manager->persistState());
	EXPECT_NO_THROW(manager->validateNodeDelete(node));
	EXPECT_NO_THROW(manager->validateEdgeDelete(edge));
}

TEST_F(ConstraintManagerEdgeAndLifecycleTest, DropConstraintHandlesBothNonEmptyAndEmptyBuckets) {
	ASSERT_NO_THROW(manager->createConstraint("nn_a", "node", "not_null", "Person", {"a"}, ""));
	ASSERT_NO_THROW(manager->createConstraint("nn_b", "node", "not_null", "Person", {"b"}, ""));

	EXPECT_TRUE(manager->dropConstraint("nn_a"));
	auto afterFirstDrop = manager->listConstraints();
	ASSERT_EQ(afterFirstDrop.size(), 1UL);
	EXPECT_EQ(afterFirstDrop[0].name, "nn_b");

	EXPECT_TRUE(manager->dropConstraint("nn_b"));
	EXPECT_FALSE(manager->dropConstraint("nn_b"));
}

TEST_F(ConstraintManagerEdgeAndLifecycleTest, EdgeConstraintValidationScansAndFiltersExistingEdges) {
	const int64_t src = addNodeWithLabel("Person");
	const int64_t dst = addNodeWithLabel("Person");

	const int64_t relLabel = dataManager->getOrCreateLabelId("REL");
	const int64_t otherLabel = dataManager->getOrCreateLabelId("OTHER_REL");

	Edge validEdge(0, src, dst, relLabel);
	validEdge.addProperty("score", PropertyValue(int64_t(42)));
	dataManager->addEdge(validEdge);

	Edge differentLabelEdge(0, src, dst, otherLabel);
	differentLabelEdge.addProperty("score", PropertyValue(int64_t(7)));
	dataManager->addEdge(differentLabelEdge);

	Edge inactiveEdge(0, src, dst, relLabel);
	inactiveEdge.addProperty("score", PropertyValue(int64_t(9)));
	dataManager->addEdge(inactiveEdge);
	dataManager->deleteEdge(inactiveEdge);

	EXPECT_NO_THROW(manager->createConstraint(
		"edge_score_not_null", "edge", "not_null", "REL", {"score"}, ""));
}

TEST_F(ConstraintManagerEdgeAndLifecycleTest, ExistingPropertyIndexesAreReusedForUniqueAndNodeKey) {
	ASSERT_TRUE(indexManager->createIndex("idx_person_email", "node", "Person", "email"));
	ASSERT_NO_THROW(manager->createConstraint(
		"uq_person_email", "node", "unique", "Person", {"email"}, ""));

	ASSERT_TRUE(indexManager->createIndex("idx_person_first", "node", "Person", "first"));
	ASSERT_TRUE(indexManager->createIndex("idx_person_last", "node", "Person", "last"));
	ASSERT_NO_THROW(manager->createConstraint(
		"nk_person_name", "node", "node_key", "Person", {"first", "last"}, ""));
}
