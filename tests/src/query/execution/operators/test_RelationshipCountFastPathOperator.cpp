#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>

#include "graph/core/Database.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/query/execution/VectorizedPredicate.hpp"
#include "graph/query/execution/operators/RelationshipCountFastPathOperator.hpp"

namespace fs = std::filesystem;
using namespace graph;
using namespace graph::query::execution;
using namespace graph::query::execution::operators;

class RelationshipCountFastPathOperatorTest : public ::testing::Test {
protected:
	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		testDbPath = fs::temp_directory_path() / ("test_relationship_count_fast_path_" + boost::uuids::to_string(uuid) + ".zyx");
		db = std::make_unique<Database>(testDbPath.string());
		db->open();
		dm = db->getStorage()->getDataManager();
		im = db->getQueryEngine()->getIndexManager();
		userLabel = dm->getOrCreateTokenId("User");
		followsType = dm->getOrCreateTokenId("FOLLOWS");
	}

	void TearDown() override {
		if (db) {
			db->close();
		}
		db.reset();
		std::error_code ec;
		fs::remove(testDbPath, ec);
		debug::PerfTrace::reset();
		debug::PerfTrace::setEnabled(false);
	}

	int64_t addUser() {
		Node node(0, userLabel);
		dm->addNode(node);
		return node.getId();
	}

	int64_t addPost() {
		Node node(0, dm->getOrCreateTokenId("Post"));
		dm->addNode(node);
		return node.getId();
	}

	void addFollows(int64_t source, int64_t target) {
		Edge edge(0, source, target, followsType);
		dm->addEdge(edge);
	}

	void addLikes(int64_t source, int64_t target) {
		Edge edge(0, source, target, dm->getOrCreateTokenId("LIKES"));
		dm->addEdge(edge);
	}

	RelationshipExpandConfig hop(std::string source, std::string edge, std::string target) const {
		RelationshipExpandConfig config;
		config.sourceVar = std::move(source);
		config.edgeVar = std::move(edge);
		config.targetVar = std::move(target);
		config.edgeType = "FOLLOWS";
		config.edgeTypeId = followsType;
		config.direction = "out";
		config.targetLabels = {"User"};
		config.targetLabelIds = {userLabel};
		return config;
	}

	fs::path testDbPath;
	std::unique_ptr<Database> db;
	std::shared_ptr<storage::DataManager> dm;
	std::shared_ptr<query::indexes::IndexManager> im;
	int64_t userLabel = 0;
	int64_t followsType = 0;
};

TEST_F(RelationshipCountFastPathOperatorTest, CountsOneHopExpand) {
	const int64_t source = addUser();
	const int64_t first = addUser();
	const int64_t second = addUser();
	addFollows(source, first);
	addFollows(source, second);

	NodeScanConfig seedConfig;
	seedConfig.type = ScanType::FULL_SCAN;
	seedConfig.variable = "u";
	NodeScanRequirements seedRequirements;
	seedRequirements.materialization = NodeMaterializationMode::NSM_ID_ONLY;
	seedRequirements.countOnly = true;

	RelationshipCountFastPathOperator op(dm, im, seedConfig, seedRequirements, {hop("u", "r", "v")}, "count");
	op.open();
	const auto batch = op.next();
	op.close();

	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 1U);
	const auto value = batch->front().getValue("count");
	ASSERT_TRUE(value.has_value());
	EXPECT_EQ(value.value(), PropertyValue(int64_t{2}));
	EXPECT_FALSE(op.next().has_value());
}

TEST_F(RelationshipCountFastPathOperatorTest, CountsTwoHopPathMultiplicity) {
	const int64_t source = addUser();
	const int64_t mid1 = addUser();
	const int64_t mid2 = addUser();
	const int64_t target = addUser();
	addFollows(source, mid1);
	addFollows(source, mid2);
	addFollows(mid1, target);
	addFollows(mid2, target);

	NodeScanConfig seedConfig;
	seedConfig.type = ScanType::FULL_SCAN;
	seedConfig.variable = "u";
	NodeScanRequirements seedRequirements;
	seedRequirements.materialization = NodeMaterializationMode::NSM_ID_ONLY;
	seedRequirements.countOnly = true;
	std::vector<RelationshipExpandConfig> hops = {hop("u", "r1", "m"), hop("m", "r2", "v")};

	RelationshipCountFastPathOperator op(dm, im, seedConfig, seedRequirements, hops, "count");
	op.open();
	const auto batch = op.next();
	op.close();

	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 1U);
	const auto value = batch->front().getValue("count");
	ASSERT_TRUE(value.has_value());
	EXPECT_EQ(value.value(), PropertyValue(int64_t{2}));
}

TEST_F(RelationshipCountFastPathOperatorTest, EmitsProfilePhases) {
	const int64_t source = addUser();
	const int64_t target = addUser();
	addFollows(source, target);
	NodeScanConfig seedConfig;
	seedConfig.type = ScanType::FULL_SCAN;
	seedConfig.variable = "u";
	NodeScanRequirements seedRequirements;
	seedRequirements.materialization = NodeMaterializationMode::NSM_ID_ONLY;
	seedRequirements.countOnly = true;
	debug::PerfTrace::setEnabled(true);
	debug::PerfTrace::reset();

	RelationshipCountFastPathOperator op(dm, im, seedConfig, seedRequirements, {hop("u", "r", "v")}, "count");
	op.open();
	(void)op.next();
	op.close();

	const auto snapshot = debug::PerfTrace::snapshotAndReset();
	EXPECT_TRUE(snapshot.contains("relationship_expand.seed_candidates"));
	EXPECT_TRUE(snapshot.contains("relationship_expand.seed_load"));
	EXPECT_TRUE(snapshot.contains("relationship_expand.edges"));
	EXPECT_TRUE(snapshot.contains("relationship_expand.target_check"));
	EXPECT_TRUE(snapshot.contains("relationship_expand.count"));
}


TEST_F(RelationshipCountFastPathOperatorTest, ResolvesHopTokensAtOpen) {
	const int64_t source = addUser();
	const int64_t matching = addUser();
	const int64_t wrongLabel = addPost();
	const int64_t wrongType = addUser();
	addFollows(source, matching);
	addFollows(source, wrongLabel);
	addLikes(source, wrongType);

	NodeScanConfig seedConfig;
	seedConfig.type = ScanType::FULL_SCAN;
	seedConfig.variable = "u";
	NodeScanRequirements seedRequirements;
	seedRequirements.materialization = NodeMaterializationMode::NSM_ID_ONLY;
	seedRequirements.countOnly = true;

	RelationshipExpandConfig unresolvedHop;
	unresolvedHop.sourceVar = "u";
	unresolvedHop.edgeVar = "r";
	unresolvedHop.targetVar = "v";
	unresolvedHop.edgeType = "FOLLOWS";
	unresolvedHop.direction = "out";
	unresolvedHop.targetLabels = {"User"};

	RelationshipCountFastPathOperator op(dm, im, seedConfig, seedRequirements, {unresolvedHop}, "count");
	op.open();
	const auto batch = op.next();
	op.close();

	ASSERT_TRUE(batch.has_value());
	const auto value = batch->front().getValue("count");
	ASSERT_TRUE(value.has_value());
	EXPECT_EQ(value.value(), PropertyValue(int64_t{1}));
}

TEST_F(RelationshipCountFastPathOperatorTest, CountsFromSeedPropertyScan) {
	const int64_t source = addUser();
	const int64_t other = addUser();
	const int64_t target = addUser();
	dm->addNodeProperties(source, {{"id", PropertyValue("u1")}});
	dm->addNodeProperties(other, {{"id", PropertyValue("u2")}});
	ASSERT_EQ(dm->getNodeProperties(source).at("id"), PropertyValue("u1"));
	ASSERT_EQ(dm->getNodeProperties(other).at("id"), PropertyValue("u2"));
	addFollows(source, target);
	addFollows(other, target);
	ASSERT_TRUE(im->createIndex("user_id_idx", "node", "User", "id"));

	NodeScanConfig seedConfig;
	seedConfig.type = ScanType::FULL_SCAN;
	seedConfig.variable = "u";
	seedConfig.labels = {"User"};
	NodeScanRequirements seedRequirements;
	seedRequirements.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	seedRequirements.requiredProperties = {"id"};
	seedRequirements.countOnly = true;
	VectorizedPropertyPredicate seedPredicate;
	seedPredicate.variable = "u";
	seedPredicate.propertyKey = "id";
	seedPredicate.op = VectorPredicateOp::VPO_EQ;
	seedPredicate.value = PropertyValue("u1");

	RelationshipCountFastPathOperator op(dm, im, seedConfig, seedRequirements, {seedPredicate}, {hop("u", "r", "v")}, "count");
	op.open();
	const auto batch = op.next();
	op.close();

	ASSERT_TRUE(batch.has_value());
	const auto value = batch->front().getValue("count");
	ASSERT_TRUE(value.has_value());
	EXPECT_EQ(value.value(), PropertyValue(int64_t{1}));
}
