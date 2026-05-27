#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>

#include "graph/core/Database.hpp"
#include "graph/query/execution/RelationshipAdjacencyCursor.hpp"
#include "graph/query/execution/RelationshipExpandConfig.hpp"

namespace fs = std::filesystem;
using namespace graph;
using namespace graph::query::execution;

TEST(RelationshipExpandConfigTest, BatchTracksSelectedRows) {
	RelationshipExpandBatch batch;
	batch.rows.push_back(RelationshipExpandRow{1, 10, 2});
	batch.rows.push_back(RelationshipExpandRow{1, 11, 3});
	batch.selected = {1, 0};

	EXPECT_EQ(batch.size(), 2U);
	EXPECT_EQ(batch.selectedCount(), 1U);
}

class RelationshipAdjacencyCursorTest : public ::testing::Test {
protected:
	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		testDbPath = fs::temp_directory_path() / ("test_relationship_adjacency_cursor_" + boost::uuids::to_string(uuid) + ".zyx");
		db = std::make_unique<Database>(testDbPath.string());
		db->open();
		dm = db->getStorage()->getDataManager();
		userLabel = dm->getOrCreateTokenId("User");
		followsType = dm->getOrCreateTokenId("FOLLOWS");
		likesType = dm->getOrCreateTokenId("LIKES");
	}

	void TearDown() override {
		if (db) {
			db->close();
		}
		db.reset();
		std::error_code ec;
		fs::remove(testDbPath, ec);
	}

	int64_t addNode(const std::string &label = "User") {
		Node node(0, dm->getOrCreateTokenId(label));
		dm->addNode(node);
		return node.getId();
	}

	int64_t addEdge(int64_t source, int64_t target, const std::string &type = "FOLLOWS") {
		Edge edge(0, source, target, dm->getOrCreateTokenId(type));
		dm->addEdge(edge);
		return edge.getId();
	}

	fs::path testDbPath;
	std::unique_ptr<Database> db;
	std::shared_ptr<storage::DataManager> dm;
	int64_t userLabel = 0;
	int64_t followsType = 0;
	int64_t likesType = 0;
};

TEST_F(RelationshipAdjacencyCursorTest, ExpandsOutgoingEdgesWithTypeAndTargetLabelFilters) {
	const int64_t source = addNode();
	const int64_t matching = addNode();
	const int64_t wrongLabel = addNode("Post");
	const int64_t wrongType = addNode();
	const int64_t firstEdge = addEdge(source, matching, "FOLLOWS");
	(void)addEdge(source, wrongLabel, "FOLLOWS");
	(void)addEdge(source, wrongType, "LIKES");

	RelationshipExpandConfig config;
	config.direction = "out";
	config.edgeTypeId = followsType;
	config.targetLabelIds = {userLabel};
	RelationshipExpandRequirements requirements;
	RelationshipAdjacencyCursor cursor(dm);

	const auto batch = cursor.expand({source}, config, requirements);

	ASSERT_EQ(batch.rows.size(), 1U);
	EXPECT_EQ(batch.rows[0].sourceId, source);
	EXPECT_EQ(batch.rows[0].targetId, matching);
	EXPECT_EQ(batch.rows[0].edgeId, firstEdge);
	EXPECT_EQ(batch.selectedCount(), 1U);
}

TEST_F(RelationshipAdjacencyCursorTest, ExpandsIncomingEdges) {
	const int64_t source = addNode();
	const int64_t origin = addNode();
	const int64_t activeEdge = addEdge(origin, source, "FOLLOWS");

	RelationshipExpandConfig config;
	config.direction = "in";
	config.edgeTypeId = followsType;
	config.targetLabelIds = {userLabel};
	RelationshipExpandRequirements requirements;
	RelationshipAdjacencyCursor cursor(dm);

	const auto batch = cursor.expand({source}, config, requirements);

	ASSERT_EQ(batch.rows.size(), 1U);
	EXPECT_EQ(batch.rows[0].sourceId, source);
	EXPECT_EQ(batch.rows[0].targetId, origin);
	EXPECT_EQ(batch.rows[0].edgeId, activeEdge);
}

TEST_F(RelationshipAdjacencyCursorTest, SkipsInactiveEdges) {
	const int64_t source = addNode();
	const int64_t activeTarget = addNode();
	const int64_t inactiveTarget = addNode();
	const int64_t activeEdge = addEdge(source, activeTarget, "FOLLOWS");
	const int64_t inactiveEdgeId = addEdge(source, inactiveTarget, "FOLLOWS");
	Edge inactiveEdge = dm->getEdge(inactiveEdgeId);
	dm->deleteEdge(inactiveEdge);

	RelationshipExpandConfig config;
	config.direction = "out";
	config.edgeTypeId = followsType;
	config.targetLabelIds = {userLabel};
	RelationshipExpandRequirements requirements;
	RelationshipAdjacencyCursor cursor(dm);

	const auto batch = cursor.expand({source}, config, requirements);

	ASSERT_EQ(batch.rows.size(), 1U);
	EXPECT_EQ(batch.rows[0].targetId, activeTarget);
	EXPECT_EQ(batch.rows[0].edgeId, activeEdge);
}

TEST_F(RelationshipAdjacencyCursorTest, MissingEdgeTypeReturnsNoRows) {
	const int64_t source = addNode();
	const int64_t target = addNode();
	(void)addEdge(source, target, "FOLLOWS");

	RelationshipExpandConfig config;
	config.direction = "out";
	config.edgeTypeId = -1;
	RelationshipExpandRequirements requirements;
	RelationshipAdjacencyCursor cursor(dm);

	const auto batch = cursor.expand({source}, config, requirements);

	EXPECT_TRUE(batch.rows.empty());
	EXPECT_EQ(batch.selectedCount(), 0U);
}
