#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>
#include <set>

#include "graph/concurrent/ThreadPool.hpp"
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

TEST_F(RelationshipAdjacencyCursorTest, NullManagerAndInvalidTargetLabelReturnNoRows) {
	const int64_t source = addNode();
	const int64_t target = addNode();
	(void)addEdge(source, target, "FOLLOWS");

	RelationshipExpandConfig config;
	config.direction = "out";
	config.edgeTypeId = followsType;
	config.targetLabelIds = {0};
	RelationshipExpandRequirements requirements;

	RelationshipAdjacencyCursor nullCursor(nullptr);
	EXPECT_TRUE(nullCursor.expand({source}, config, requirements).rows.empty());

	RelationshipAdjacencyCursor cursor(dm);
	EXPECT_TRUE(cursor.expand({source}, config, requirements).rows.empty());
}

TEST_F(RelationshipAdjacencyCursorTest, ExpandsBothDirectionsAndCanSkipTargetChecks) {
	const int64_t source = addNode();
	const int64_t inbound = addNode();
	const int64_t outbound = addNode();
	const int64_t inboundEdge = addEdge(inbound, source, "FOLLOWS");
	const int64_t outboundEdge = addEdge(source, outbound, "FOLLOWS");

	RelationshipExpandConfig config;
	config.direction = "both";
	config.edgeTypeId = followsType;
	RelationshipExpandRequirements requirements;
	requirements.needsTargetLabels = false;
	RelationshipAdjacencyCursor cursor(dm);

	const auto batch = cursor.expand({source}, config, requirements);

	ASSERT_EQ(batch.rows.size(), 2U);
	const auto firstTarget = batch.rows[0].edgeId == inboundEdge ? inbound : outbound;
	const auto secondTarget = batch.rows[1].edgeId == inboundEdge ? inbound : outbound;
	EXPECT_EQ(batch.rows[0].targetId, firstTarget);
	EXPECT_EQ(batch.rows[1].targetId, secondTarget);
	EXPECT_NE(batch.rows[0].edgeId, batch.rows[1].edgeId);
	EXPECT_TRUE(batch.rows[0].edgeId == inboundEdge || batch.rows[0].edgeId == outboundEdge);
	EXPECT_TRUE(batch.rows[1].edgeId == inboundEdge || batch.rows[1].edgeId == outboundEdge);
}

TEST_F(RelationshipAdjacencyCursorTest, CountAndStreamingUseCursorWithoutBuildingBatch) {
	const int64_t source = addNode();
	const int64_t first = addNode();
	const int64_t second = addNode();
	(void)addEdge(source, first, "FOLLOWS");
	(void)addEdge(source, second, "FOLLOWS");

	RelationshipExpandConfig config;
	config.direction = "out";
	config.edgeTypeId = followsType;
	config.targetLabelIds = {userLabel};
	RelationshipExpandRequirements requirements;
	RelationshipAdjacencyCursor cursor(dm);

	EXPECT_EQ(cursor.count({source}, config, requirements), 2);

	std::vector<int64_t> streamedTargets;
	const size_t visited = cursor.forEach({source}, config, requirements, [&](const RelationshipExpandRow &row) {
		streamedTargets.push_back(row.targetId);
		return streamedTargets.size() < 1;
	});

	EXPECT_EQ(visited, 1U);
	ASSERT_EQ(streamedTargets.size(), 1U);
	EXPECT_TRUE(streamedTargets[0] == first || streamedTargets[0] == second);
}

TEST_F(RelationshipAdjacencyCursorTest, CountOnlyCanSkipTargetLoadsWhenNoTargetPredicateIsRequired) {
	const int64_t source = addNode();
	const int64_t activeTarget = addNode();
	const int64_t deletedTarget = addNode();
	(void)addEdge(source, activeTarget, "FOLLOWS");
	(void)addEdge(source, deletedTarget, "FOLLOWS");

	Node target = dm->getNode(deletedTarget);
	dm->deleteNode(target);

	RelationshipExpandConfig config;
	config.direction = "out";
	config.edgeTypeId = followsType;
	RelationshipExpandRequirements requirements;
	requirements.needsTargetActiveCheck = false;
	requirements.needsTargetLabels = false;
	RelationshipAdjacencyCursor cursor(dm);

	// Node deletion cascades to its connected edge, so active relationship
	// filtering is enough for count-only traversal.
	EXPECT_EQ(cursor.count({source}, config, requirements), 1);
}

TEST_F(RelationshipAdjacencyCursorTest, WildcardTypeCanSkipRuntimeActiveChecks) {
	const int64_t source = addNode();
	const int64_t inbound = addNode();
	const int64_t outbound = addNode();
	const int64_t inboundEdge = addEdge(inbound, source, "FOLLOWS");
	const int64_t outboundEdge = addEdge(source, outbound, "LIKES");

	RelationshipExpandConfig config;
	config.direction = "both";
	config.edgeTypeId = 0;
	RelationshipExpandRequirements requirements;
	requirements.needsEdgeActiveCheck = false;
	requirements.needsTargetActiveCheck = false;
	requirements.needsTargetLabels = false;
	RelationshipAdjacencyCursor cursor(dm);

	const auto batch = cursor.expand({source}, config, requirements);

	ASSERT_EQ(batch.rows.size(), 2U);
	std::set<int64_t> edgeIds;
	std::set<int64_t> targetIds;
	for (const auto &row : batch.rows) {
		edgeIds.insert(row.edgeId);
		targetIds.insert(row.targetId);
		EXPECT_EQ(row.sourceId, source);
	}
	EXPECT_TRUE(edgeIds.contains(inboundEdge));
	EXPECT_TRUE(edgeIds.contains(outboundEdge));
	EXPECT_TRUE(targetIds.contains(inbound));
	EXPECT_TRUE(targetIds.contains(outbound));
}

TEST_F(RelationshipAdjacencyCursorTest, CountAndForEachHandleInvalidInputs) {
	const int64_t source = addNode();
	const int64_t target = addNode();
	(void)addEdge(source, target, "FOLLOWS");

	RelationshipExpandConfig config;
	config.direction = "out";
	config.edgeTypeId = followsType;
	RelationshipExpandRequirements requirements;
	RelationshipAdjacencyCursor cursor(dm);

	EXPECT_TRUE(cursor.expand({}, config, requirements).rows.empty());
	EXPECT_EQ(cursor.count({}, config, requirements), 0);
	EXPECT_EQ(cursor.forEach({source}, config, requirements, {}), 0U);

	RelationshipAdjacencyCursor nullCursor(nullptr);
	EXPECT_EQ(nullCursor.count({source}, config, requirements), 0);
	EXPECT_EQ(nullCursor.forEach({source}, config, requirements, [](const RelationshipExpandRow &) { return true; }), 0U);

	config.edgeTypeId = -1;
	EXPECT_EQ(cursor.count({source}, config, requirements), 0);
	EXPECT_EQ(cursor.forEach({source}, config, requirements, [](const RelationshipExpandRow &) { return true; }), 0U);
}

TEST_F(RelationshipAdjacencyCursorTest, TargetActiveCheckFiltersDeletedTargetsWhenEdgeActiveCheckIsSkipped) {
	const int64_t source = addNode();
	const int64_t activeTarget = addNode();
	const int64_t inactiveTarget = addNode();
	(void)addEdge(source, activeTarget, "FOLLOWS");
	(void)addEdge(source, inactiveTarget, "FOLLOWS");

	Node deleted = dm->getNode(inactiveTarget);
	dm->deleteNode(deleted);

	RelationshipExpandConfig config;
	config.direction = "out";
	config.edgeTypeId = followsType;
	RelationshipExpandRequirements requirements;
	requirements.needsEdgeActiveCheck = false;
	requirements.needsTargetActiveCheck = true;
	requirements.needsTargetLabels = false;
	RelationshipAdjacencyCursor cursor(dm);

	EXPECT_EQ(cursor.count({source}, config, requirements), 1);
	const auto batch = cursor.expand({source}, config, requirements);
	ASSERT_EQ(batch.rows.size(), 1U);
	EXPECT_EQ(batch.rows[0].targetId, activeTarget);
}

TEST_F(RelationshipAdjacencyCursorTest, ParallelEstimateSamplesLargeSourceFrontier) {
	std::vector<int64_t> sources;
	sources.reserve(16);
	for (size_t i = 0; i < 16; ++i) {
		const int64_t source = addNode();
		const int64_t target = addNode();
		(void)addEdge(source, target, "FOLLOWS");
		sources.push_back(source);
	}

	graph::concurrent::ThreadPool pool(2);
	RelationshipExpandConfig config;
	config.direction = "out";
	config.edgeTypeId = followsType;
	RelationshipExpandRequirements requirements;
	requirements.needsTargetLabels = false;
	RelationshipAdjacencyCursor cursor(dm, &pool);

	EXPECT_EQ(cursor.count(sources, config, requirements), static_cast<int64_t>(sources.size()));
	const auto batch = cursor.expand(sources, config, requirements);
	EXPECT_EQ(batch.rows.size(), sources.size());
}

TEST_F(RelationshipAdjacencyCursorTest, ForEachStopsBeforeScanningLaterSources) {
	const int64_t firstSource = addNode();
	const int64_t firstTarget = addNode();
	const int64_t secondSource = addNode();
	const int64_t secondTarget = addNode();
	(void)addEdge(firstSource, firstTarget, "FOLLOWS");
	(void)addEdge(secondSource, secondTarget, "FOLLOWS");

	RelationshipExpandConfig config;
	config.direction = "out";
	config.edgeTypeId = followsType;
	RelationshipExpandRequirements requirements;
	requirements.needsTargetLabels = false;
	RelationshipAdjacencyCursor cursor(dm);

	std::vector<int64_t> targets;
	const size_t emitted = cursor.forEach({firstSource, secondSource}, config, requirements,
	                                      [&](const RelationshipExpandRow &row) {
		                                      targets.push_back(row.targetId);
		                                      return false;
	                                      });

	EXPECT_EQ(emitted, 1U);
	ASSERT_EQ(targets.size(), 1U);
	EXPECT_EQ(targets[0], firstTarget);
}

TEST_F(RelationshipAdjacencyCursorTest, ForEachSkipsNonMatchingEdgesAndLabels) {
	const int64_t source = addNode();
	const int64_t matchingTarget = addNode();
	const int64_t wrongLabelTarget = addNode("Post");
	const int64_t wrongTypeTarget = addNode();
	(void)addEdge(source, wrongLabelTarget, "FOLLOWS");
	(void)addEdge(source, wrongTypeTarget, "LIKES");
	(void)addEdge(source, matchingTarget, "FOLLOWS");

	RelationshipExpandConfig config;
	config.direction = "out";
	config.edgeTypeId = followsType;
	config.targetLabelIds = {userLabel};
	RelationshipExpandRequirements requirements;
	RelationshipAdjacencyCursor cursor(dm);

	std::vector<int64_t> targets;
	const size_t emitted = cursor.forEach({source}, config, requirements, [&](const RelationshipExpandRow &row) {
		targets.push_back(row.targetId);
		return true;
	});

	EXPECT_EQ(emitted, 1U);
	ASSERT_EQ(targets.size(), 1U);
	EXPECT_EQ(targets[0], matchingTarget);
}
