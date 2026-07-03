#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <unordered_map>

#include "graph/core/Database.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/query/QueryContext.hpp"
#include "graph/query/execution/operators/NodeProjectionScanOperator.hpp"
#include "graph/query/execution/operators/RelationshipProjectionScanOperator.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/indexes/IndexManager.hpp"

namespace fs = std::filesystem;
using namespace graph;
using namespace graph::query::execution;
using namespace graph::query::execution::operators;

class ProjectionScanOperatorsTest : public ::testing::Test {
protected:
	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		dbPath = fs::temp_directory_path() /
		         ("test_projection_scan_operators_" + boost::uuids::to_string(uuid) + ".zyx");
		db = std::make_unique<Database>(dbPath.string());
		db->open();
		dm = db->getStorage()->getDataManager();
		im = db->getQueryEngine()->getIndexManager();
		userLabel = dm->getOrCreateTokenId("User");
		postLabel = dm->getOrCreateTokenId("Post");
		followsType = dm->getOrCreateTokenId("FOLLOWS");
	}

	void TearDown() override {
		debug::PerfTrace::setEnabled(false);
		debug::PerfTrace::reset();
		if (db) {
			db->close();
		}
		db.reset();
		std::error_code ec;
		fs::remove_all(dbPath, ec);
	}

	int64_t addNode(int64_t labelId, const std::unordered_map<std::string, PropertyValue> &properties) {
		Node node(0, labelId);
		dm->addNode(node);
		if (!properties.empty()) {
			dm->addNodeProperties(node.getId(), properties);
		}
		return node.getId();
	}

	int64_t addFollowsReturningId(int64_t source, int64_t target, int64_t weight) {
		Edge edge(0, source, target, followsType);
		dm->addEdge(edge);
		dm->addEdgeProperties(edge.getId(), {{"weight", PropertyValue(weight)}});
		return edge.getId();
	}

	void addFollows(int64_t source, int64_t target, int64_t weight) {
		(void) addFollowsReturningId(source, target, weight);
	}

	static PropertyValue valueAt(const Record &record, const std::string &alias) {
		auto value = record.getValue(alias);
		EXPECT_TRUE(value.has_value());
		return value.value_or(PropertyValue{});
	}

	fs::path dbPath;
	std::unique_ptr<Database> db;
	std::shared_ptr<storage::DataManager> dm;
	std::shared_ptr<query::indexes::IndexManager> im;
	int64_t userLabel = 0;
	int64_t postLabel = 0;
	int64_t followsType = 0;
};

TEST_F(ProjectionScanOperatorsTest, NodeProjectionScanReturnsBoundedScalarRows) {
	for (int64_t i = 0; i < 5; ++i) {
		addNode(userLabel, {{"id", PropertyValue("user-" + std::to_string(i))}, {"score", PropertyValue(i)}});
	}
	addNode(postLabel, {{"id", PropertyValue("post")}, {"score", PropertyValue(int64_t{99})}});
	db->getStorage()->flush();

	NodeScanConfig config;
	config.type = ScanType::FULL_SCAN;
	config.variable = "u";
	config.labels = {"User"};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	requirements.requiredProperties = {"id", "score"};

	NodeProjectionScanOperator op(dm, im, config, requirements, {}, {{"id", "id"}, {"score", "score"}}, 2);
	op.open();
	auto batch = op.next();

	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 2U);
	EXPECT_EQ(valueAt((*batch)[0], "id"), PropertyValue("user-0"));
	EXPECT_EQ(valueAt((*batch)[1], "score"), PropertyValue(int64_t{1}));
	EXPECT_FALSE((*batch)[0].getNode("u").has_value());
	EXPECT_FALSE(op.next().has_value());
}

TEST_F(ProjectionScanOperatorsTest, NodeProjectionScanHonorsSmallerRuntimeLimitHint) {
	for (int64_t i = 0; i < 3; ++i) {
		addNode(userLabel, {{"id", PropertyValue("user-" + std::to_string(i))}});
	}
	db->getStorage()->flush();

	NodeScanConfig config;
	config.type = ScanType::FULL_SCAN;
	config.variable = "u";
	config.labels = {"User"};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;

	NodeProjectionScanOperator op(dm, im, config, requirements, {}, {{"id", "id"}, {"missing", "missing"}}, 10);
	op.setOutputLimitHint(1);
	op.open();
	auto batch = op.next();

	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 1U);
	EXPECT_EQ(valueAt((*batch)[0], "id"), PropertyValue("user-0"));
	EXPECT_EQ(valueAt((*batch)[0], "missing").getType(), PropertyType::NULL_TYPE);
	EXPECT_FALSE(op.next().has_value());
	EXPECT_EQ(op.getOutputVariables(), (std::vector<std::string>{"id", "missing"}));
	EXPECT_NE(op.toString().find("LIMIT 1"), std::string::npos);
	op.close();
	EXPECT_FALSE(op.next().has_value());
}

TEST_F(ProjectionScanOperatorsTest, NodeProjectionScanUsesRuntimeLimitWhenConstructedWithoutLimit) {
	for (int64_t i = 0; i < 4; ++i) {
		addNode(userLabel, {{"id", PropertyValue("user-" + std::to_string(i))}});
	}
	db->getStorage()->flush();

	NodeScanConfig config;
	config.type = ScanType::FULL_SCAN;
	config.variable = "u";
	config.labels = {"User"};
	NodeScanRequirements requirements;

	NodeProjectionScanOperator op(dm, im, config, requirements, {}, {{"id", "id"}});
	EXPECT_EQ(op.toString(), "NodeProjectionScan(u)");
	op.setOutputLimitHint(2);
	op.setOutputLimitHint(3);
	debug::PerfTrace::reset();
	debug::PerfTrace::setEnabled(true);
	op.open();
	auto batch = op.next();
	debug::PerfTrace::setEnabled(false);
	const auto profile = debug::PerfTrace::snapshotAndReset();

	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 2U);
	EXPECT_EQ(valueAt((*batch)[0], "id"), PropertyValue("user-0"));
	EXPECT_EQ(valueAt((*batch)[1], "id"), PropertyValue("user-1"));
	EXPECT_FALSE(op.next().has_value());
	ASSERT_TRUE(profile.contains("node_projection_scan"));
	EXPECT_EQ(profile.at("node_projection_scan").calls, 1U);
}

TEST_F(ProjectionScanOperatorsTest, NodeProjectionScanReturnsEmptyForEmptyCandidateSet) {
	NodeScanConfig config;
	config.type = ScanType::FULL_SCAN;
	config.variable = "u";
	config.labels = {"User"};

	NodeProjectionScanOperator op(dm, im, config, {}, {}, {{"id", "id"}}, 10);
	op.open();
	EXPECT_FALSE(op.next().has_value());
}

TEST_F(ProjectionScanOperatorsTest, NodeProjectionScanProfilesEmptyPredicateResult) {
	for (int64_t i = 0; i < 3; ++i) {
		addNode(userLabel, {{"score", PropertyValue(i)}});
	}
	db->getStorage()->flush();

	VectorizedPropertyPredicate rejectAll;
	rejectAll.propertyKey = "score";
	rejectAll.op = VectorPredicateOp::VPO_GT;
	rejectAll.value = PropertyValue(int64_t{99});

	NodeScanConfig config;
	config.type = ScanType::FULL_SCAN;
	config.variable = "u";
	config.labels = {"User"};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;

	NodeProjectionScanOperator op(dm, im, config, requirements, {rejectAll}, {{"score", "score"}}, 10);
	debug::PerfTrace::setEnabled(true);
	debug::PerfTrace::reset();
	op.open();
	EXPECT_FALSE(op.next().has_value());
	const auto profile = debug::PerfTrace::snapshotAndReset();
	debug::PerfTrace::setEnabled(false);

	ASSERT_TRUE(profile.contains("node_projection_scan"));
	EXPECT_EQ(profile.at("node_projection_scan").calls, 1U);
}

TEST_F(ProjectionScanOperatorsTest, NodeProjectionScanEmitsNullForRowsMissingProjectedProperty) {
	addNode(userLabel, {{"id", PropertyValue("user-0")}});
	addNode(userLabel, {});
	db->getStorage()->flush();

	NodeScanConfig config;
	config.type = ScanType::FULL_SCAN;
	config.variable = "u";
	config.labels = {"User"};
	NodeScanRequirements requirements;
	requirements.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	requirements.requiredProperties = {"id"};

	NodeProjectionScanOperator op(dm, im, config, requirements, {}, {{"id", "id"}}, 10);
	op.open();
	auto batch = op.next();

	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 2U);
	EXPECT_EQ(valueAt((*batch)[0], "id"), PropertyValue("user-0"));
	EXPECT_EQ(valueAt((*batch)[1], "id").getType(), PropertyType::NULL_TYPE);
}

TEST_F(ProjectionScanOperatorsTest, RelationshipProjectionScanFiltersAndProjectsScalars) {
	const int64_t a = addNode(userLabel, {{"id", PropertyValue("a")}});
	const int64_t b = addNode(userLabel, {{"id", PropertyValue("b")}});
	const int64_t c = addNode(postLabel, {{"id", PropertyValue("c")}});
	addFollows(a, b, 1);
	addFollows(b, a, 2);
	addFollows(a, c, 1);
	db->getStorage()->flush();

	VectorizedPropertyPredicate weightIsOne;
	weightIsOne.propertyKey = "weight";
	weightIsOne.op = VectorPredicateOp::VPO_EQ;
	weightIsOne.value = PropertyValue(int64_t{1});

	DirectRelationshipCountConfig config;
	config.enabled = true;
	config.edgeType = "FOLLOWS";
	config.direction = "out";
	config.edgePredicates = {weightIsOne};

	RelationshipProjectionScanOperator op(
		dm, im, config, "v", {"User"},
		{{RelationshipProjectionSource::RPS_EDGE, "weight", "weight"},
		 {RelationshipProjectionSource::RPS_TARGET_NODE, "id", "id"}},
		10);
	op.open();
	auto batch = op.next();

	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 1U);
	EXPECT_EQ(valueAt((*batch)[0], "weight"), PropertyValue(int64_t{1}));
	EXPECT_EQ(valueAt((*batch)[0], "id"), PropertyValue("b"));
	EXPECT_FALSE((*batch)[0].getEdge("r").has_value());
	EXPECT_FALSE(op.next().has_value());
}

TEST_F(ProjectionScanOperatorsTest, RelationshipProjectionScanProjectsTargetOnlyWithoutLimit) {
	const int64_t a = addNode(userLabel, {{"id", PropertyValue("a")}});
	const int64_t b = addNode(userLabel, {{"id", PropertyValue("b")}});
	const int64_t c = addNode(userLabel, {{"id", PropertyValue("c")}});
	addFollows(a, b, 1);
	addFollows(a, c, 2);
	db->getStorage()->flush();

	DirectRelationshipCountConfig config;
	config.enabled = true;
	config.edgeType = "FOLLOWS";
	config.direction = "out";

	RelationshipProjectionScanOperator op(
		dm, im, config, "v", {"User"},
		{{RelationshipProjectionSource::RPS_TARGET_NODE, "id", "id"}});
	EXPECT_EQ(op.toString(), "RelationshipProjectionScan(FOLLOWS)");
	op.open();
	auto batch = op.next();

	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 2U);
	EXPECT_EQ(valueAt((*batch)[0], "id"), PropertyValue("b"));
	EXPECT_EQ(valueAt((*batch)[1], "id"), PropertyValue("c"));
	EXPECT_FALSE(op.next().has_value());
}

TEST_F(ProjectionScanOperatorsTest, RelationshipProjectionScanRecordsTraceWhenUnboundedScanExhausts) {
	const int64_t a = addNode(userLabel, {{"id", PropertyValue("a")}});
	const int64_t b = addNode(userLabel, {{"id", PropertyValue("b")}});
	addFollows(a, b, 4);
	db->getStorage()->flush();

	DirectRelationshipCountConfig config;
	config.enabled = true;
	config.edgeType = "FOLLOWS";
	config.direction = "out";

	RelationshipProjectionScanOperator op(
		dm, im, config, "v", {},
		{{RelationshipProjectionSource::RPS_EDGE, "weight", "weight"}});

	debug::PerfTrace::reset();
	debug::PerfTrace::setEnabled(true);
	op.open();
	auto batch = op.next();
	auto exhausted = op.next();
	debug::PerfTrace::setEnabled(false);
	const auto profile = debug::PerfTrace::snapshotAndReset();

	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 1U);
	EXPECT_FALSE(exhausted.has_value());
	ASSERT_TRUE(profile.contains("relationship_projection_scan"));
	EXPECT_EQ(profile.at("relationship_projection_scan").calls, 2U);
}

TEST_F(ProjectionScanOperatorsTest, RelationshipProjectionScanUsesIndexedCandidatesAndIncomingDirection) {
	const int64_t a = addNode(userLabel, {{"id", PropertyValue("a")}});
	const int64_t b = addNode(userLabel, {{"id", PropertyValue("b")}});
	const int64_t c = addNode(userLabel, {{"id", PropertyValue("c")}});
	addFollows(a, b, 7);
	addFollows(c, b, 8);
	db->getStorage()->flush();
	ASSERT_TRUE(im->createIndex("projection_edge_weight", "edge", "FOLLOWS", "weight"));

	VectorizedPropertyPredicate weightIsSeven;
	weightIsSeven.propertyKey = "weight";
	weightIsSeven.op = VectorPredicateOp::VPO_EQ;
	weightIsSeven.value = PropertyValue(int64_t{7});

	DirectRelationshipCountConfig config;
	config.enabled = true;
	config.edgeType = "FOLLOWS";
	config.direction = "in";
	config.edgePredicates = {weightIsSeven};
	config.candidateSource.type = DirectRelationshipCandidateSourceType::DRCS_PROPERTY_INDEX;
	config.candidateSource.propertyKeys = {"weight"};

	RelationshipProjectionScanOperator op(
		dm, im, config, "v", {"User"},
		{{RelationshipProjectionSource::RPS_TARGET_NODE, "id", "id"},
		 {RelationshipProjectionSource::RPS_EDGE, "weight", "weight"}},
		10);
	op.open();
	auto batch = op.next();

	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 1U);
	EXPECT_EQ(valueAt((*batch)[0], "id"), PropertyValue("a"));
	EXPECT_EQ(valueAt((*batch)[0], "weight"), PropertyValue(int64_t{7}));
	EXPECT_FALSE(op.next().has_value());
}

TEST_F(ProjectionScanOperatorsTest, RelationshipProjectionScanUsesUnboundedIndexedCandidates) {
	const int64_t a = addNode(userLabel, {{"id", PropertyValue("a")}});
	const int64_t b = addNode(userLabel, {{"id", PropertyValue("b")}});
	const int64_t c = addNode(userLabel, {{"id", PropertyValue("c")}});
	addFollows(a, b, 5);
	addFollows(a, c, 5);
	db->getStorage()->flush();
	ASSERT_TRUE(im->createIndex("projection_edge_weight_unbounded", "edge", "FOLLOWS", "weight"));

	VectorizedPropertyPredicate weightIsFive;
	weightIsFive.propertyKey = "weight";
	weightIsFive.op = VectorPredicateOp::VPO_EQ;
	weightIsFive.value = PropertyValue(int64_t{5});

	DirectRelationshipCountConfig config;
	config.enabled = true;
	config.edgeType = "FOLLOWS";
	config.direction = "out";
	config.edgePredicates = {weightIsFive};
	config.candidateSource.type = DirectRelationshipCandidateSourceType::DRCS_PROPERTY_INDEX;
	config.candidateSource.propertyKeys = {"weight"};

	RelationshipProjectionScanOperator op(
		dm, im, config, "v", {"User"},
		{{RelationshipProjectionSource::RPS_EDGE, "weight", "weight"},
		 {RelationshipProjectionSource::RPS_TARGET_NODE, "id", "id"}});
	op.open();
	auto batch = op.next();

	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 2U);
	EXPECT_EQ(valueAt((*batch)[0], "weight"), PropertyValue(int64_t{5}));
	EXPECT_EQ(valueAt((*batch)[1], "weight"), PropertyValue(int64_t{5}));
	EXPECT_FALSE(op.next().has_value());
}

TEST_F(ProjectionScanOperatorsTest, RelationshipProjectionScanFiltersTypeAfterPropertyCandidateSource) {
	const int64_t a = addNode(userLabel, {{"id", PropertyValue("a")}});
	const int64_t b = addNode(userLabel, {{"id", PropertyValue("b")}});
	const int64_t c = addNode(userLabel, {{"id", PropertyValue("c")}});
	addFollows(a, b, 5);
	const int64_t likesType = dm->getOrCreateTokenId("LIKES");
	Edge likes(0, a, c, likesType);
	dm->addEdge(likes);
	dm->addEdgeProperties(likes.getId(), {{"weight", PropertyValue(int64_t{5})}});
	db->getStorage()->flush();
	ASSERT_TRUE(im->createIndex("projection_edge_weight_mixed_type", "edge", "", "weight"));

	VectorizedPropertyPredicate weightIsFive;
	weightIsFive.propertyKey = "weight";
	weightIsFive.op = VectorPredicateOp::VPO_EQ;
	weightIsFive.value = PropertyValue(int64_t{5});

	DirectRelationshipCountConfig config;
	config.enabled = true;
	config.edgeType = "FOLLOWS";
	config.direction = "out";
	config.edgePredicates = {weightIsFive};
	config.candidateSource.type = DirectRelationshipCandidateSourceType::DRCS_PROPERTY_INDEX;
	config.candidateSource.propertyKeys = {"weight"};

	RelationshipProjectionScanOperator op(
		dm, im, config, "v", {"User"},
		{{RelationshipProjectionSource::RPS_EDGE, "weight", "weight"},
		 {RelationshipProjectionSource::RPS_TARGET_NODE, "id", "id"}});
	op.open();
	auto batch = op.next();

	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 1U);
	EXPECT_EQ(valueAt((*batch)[0], "id"), PropertyValue("b"));
	EXPECT_EQ(valueAt((*batch)[0], "weight"), PropertyValue(int64_t{5}));
	EXPECT_FALSE(op.next().has_value());
}

TEST_F(ProjectionScanOperatorsTest, RelationshipProjectionScanReturnsNullForMissingProjectedProperties) {
	const int64_t a = addNode(userLabel, {{"id", PropertyValue("a")}});
	const int64_t b = addNode(userLabel, {});
	addFollows(a, b, 1);
	db->getStorage()->flush();

	DirectRelationshipCountConfig config;
	config.enabled = true;
	config.edgeType = "FOLLOWS";
	config.direction = "out";

	RelationshipProjectionScanOperator op(
		dm, im, config, "v", {},
		{{RelationshipProjectionSource::RPS_EDGE, "missingEdge", "missingEdge"},
		 {RelationshipProjectionSource::RPS_TARGET_NODE, "missingNode", "missingNode"}},
		10);
	op.open();
	auto batch = op.next();

	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 1U);
	EXPECT_EQ(valueAt((*batch)[0], "missingEdge").getType(), PropertyType::NULL_TYPE);
	EXPECT_EQ(valueAt((*batch)[0], "missingNode").getType(), PropertyType::NULL_TYPE);
}

TEST_F(ProjectionScanOperatorsTest, RelationshipProjectionScanSkipsInactiveEdges) {
	const int64_t a = addNode(userLabel, {{"id", PropertyValue("a")}});
	const int64_t b = addNode(userLabel, {{"id", PropertyValue("b")}});
	const int64_t c = addNode(userLabel, {{"id", PropertyValue("c")}});
	const int64_t deletedId = addFollowsReturningId(a, b, 11);
	addFollows(a, c, 12);

	Edge deleted = dm->getEdge(deletedId);
	dm->deleteEdge(deleted);
	db->getStorage()->flush();

	DirectRelationshipCountConfig config;
	config.enabled = true;
	config.edgeType = "FOLLOWS";
	config.direction = "out";

	RelationshipProjectionScanOperator op(
		dm, im, config, "v", {"User"},
		{{RelationshipProjectionSource::RPS_EDGE, "weight", "weight"},
		 {RelationshipProjectionSource::RPS_TARGET_NODE, "id", "id"}},
		10);
	op.open();
	auto batch = op.next();

	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 1U);
	EXPECT_EQ(valueAt((*batch)[0], "weight"), PropertyValue(int64_t{12}));
	EXPECT_EQ(valueAt((*batch)[0], "id"), PropertyValue("c"));
	EXPECT_FALSE(op.next().has_value());
}

TEST_F(ProjectionScanOperatorsTest, RelationshipProjectionScanScansAllTypesWhenTypeIsOmittedAndProfiles) {
	const int64_t a = addNode(userLabel, {{"id", PropertyValue("a")}});
	const int64_t b = addNode(userLabel, {{"id", PropertyValue("b")}});
	addFollows(a, b, 3);
	db->getStorage()->flush();

	DirectRelationshipCountConfig config;
	config.enabled = true;
	config.direction = "out";

	RelationshipProjectionScanOperator op(
		dm, im, config, "v", {},
		{{RelationshipProjectionSource::RPS_EDGE, "weight", "weight"}});
	op.setOutputLimitHint(1);
	op.setOutputLimitHint(2);

	debug::PerfTrace::reset();
	debug::PerfTrace::setEnabled(true);
	op.open();
	auto batch = op.next();
	auto exhausted = op.next();
	debug::PerfTrace::setEnabled(false);
	const auto profile = debug::PerfTrace::snapshotAndReset();

	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 1U);
	EXPECT_EQ(valueAt((*batch)[0], "weight"), PropertyValue(int64_t{3}));
	EXPECT_FALSE(exhausted.has_value());
	ASSERT_TRUE(profile.contains("relationship_projection_scan"));
	EXPECT_EQ(profile.at("relationship_projection_scan").calls, 1U);
}

TEST_F(ProjectionScanOperatorsTest, RelationshipProjectionScanConvertsEqualityPropertiesAndHonorsLimitHint) {
	const int64_t a = addNode(userLabel, {{"id", PropertyValue("a")}});
	const int64_t b = addNode(userLabel, {{"id", PropertyValue("b")}});
	addFollows(a, b, 2);
	addFollows(b, a, 2);
	db->getStorage()->flush();

	DirectRelationshipCountConfig config;
	config.enabled = true;
	config.edgeType = "FOLLOWS";
	config.direction = "out";
	config.edgeProperties = {{"weight", PropertyValue(int64_t{2})}};

	RelationshipProjectionScanOperator op(
		dm, im, config, "v", {},
		{{RelationshipProjectionSource::RPS_EDGE, "weight", "weight"}},
		10);
	op.setOutputLimitHint(1);
	op.open();
	auto batch = op.next();

	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 1U);
	EXPECT_EQ(valueAt((*batch)[0], "weight"), PropertyValue(int64_t{2}));
	EXPECT_FALSE(op.next().has_value());
	EXPECT_EQ(op.getOutputVariables(), (std::vector<std::string>{"weight"}));
	EXPECT_NE(op.toString().find("FOLLOWS"), std::string::npos);
	EXPECT_NE(op.toString().find("LIMIT 1"), std::string::npos);
}

TEST_F(ProjectionScanOperatorsTest, RelationshipProjectionScanRejectsUnknownTypeOrTargetLabel) {
	const int64_t a = addNode(userLabel, {{"id", PropertyValue("a")}});
	const int64_t b = addNode(userLabel, {{"id", PropertyValue("b")}});
	addFollows(a, b, 1);
	db->getStorage()->flush();

	DirectRelationshipCountConfig missingType;
	missingType.enabled = true;
	missingType.edgeType = "UNKNOWN_REL_TYPE";
	missingType.direction = "out";
	RelationshipProjectionScanOperator unknownTypeOp(
		dm, im, missingType, "v", {},
		{{RelationshipProjectionSource::RPS_EDGE, "weight", "weight"}},
		10);
	unknownTypeOp.open();
	EXPECT_FALSE(unknownTypeOp.next().has_value());

	DirectRelationshipCountConfig config;
	config.enabled = true;
	config.edgeType = "FOLLOWS";
	config.direction = "out";
	RelationshipProjectionScanOperator unknownLabelOp(
		dm, im, config, "v", {"MissingLabel"},
		{{RelationshipProjectionSource::RPS_EDGE, "weight", "weight"}},
		10);
	unknownLabelOp.open();
	EXPECT_FALSE(unknownLabelOp.next().has_value());
}

TEST_F(ProjectionScanOperatorsTest, RelationshipProjectionScanSkipsInvalidAndInactiveTargets) {
	const int64_t a = addNode(userLabel, {{"id", PropertyValue("a")}});
	const int64_t deletedTarget = addNode(userLabel, {{"id", PropertyValue("deleted")}});
	const int64_t liveTarget = addNode(userLabel, {{"id", PropertyValue("live")}});

	Edge zeroTarget(0, a, 0, followsType);
	dm->addEdge(zeroTarget);
	dm->addEdgeProperties(zeroTarget.getId(), {{"weight", PropertyValue(int64_t{1})}});
	Edge deletedEdge(0, a, deletedTarget, followsType);
	dm->addEdge(deletedEdge);
	dm->addEdgeProperties(deletedEdge.getId(), {{"weight", PropertyValue(int64_t{2})}});
	addFollows(a, liveTarget, 3);

	Node deletedNode = dm->getNode(deletedTarget);
	dm->deleteNode(deletedNode);
	db->getStorage()->flush();

	DirectRelationshipCountConfig config;
	config.enabled = true;
	config.edgeType = "FOLLOWS";
	config.direction = "out";

	query::QueryContext context;
	RelationshipProjectionScanOperator op(
		dm, im, config, "v", {"User"},
		{{RelationshipProjectionSource::RPS_TARGET_NODE, "id", "id"},
		 {RelationshipProjectionSource::RPS_EDGE, "weight", "weight"}},
		10);
	op.setQueryContext(&context);
	op.open();
	auto batch = op.next();

	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 1U);
	EXPECT_EQ(valueAt((*batch)[0], "id"), PropertyValue("live"));
	EXPECT_EQ(valueAt((*batch)[0], "weight"), PropertyValue(int64_t{3}));
	EXPECT_FALSE(op.next().has_value());
}

TEST_F(ProjectionScanOperatorsTest, RelationshipProjectionScanSkipsMissingAndDeletedTargetNodes) {
	const int64_t source = addNode(userLabel, {{"id", PropertyValue("source")}});
	const int64_t deletedTarget = addNode(userLabel, {{"id", PropertyValue("deleted")}});
	const int64_t liveTarget = addNode(userLabel, {{"id", PropertyValue("live")}});

	Edge missingTargetEdge(0, source, 999999, followsType);
	dm->addEdge(missingTargetEdge);
	dm->addEdgeProperties(missingTargetEdge.getId(), {{"weight", PropertyValue(int64_t{1})}});

	addFollows(source, deletedTarget, 2);
	addFollows(source, liveTarget, 3);

	Node deletedNode = dm->getNode(deletedTarget);
	dm->deleteNode(deletedNode);
	db->getStorage()->flush();

	DirectRelationshipCountConfig config;
	config.enabled = true;
	config.edgeType = "FOLLOWS";
	config.direction = "out";

	RelationshipProjectionScanOperator op(
		dm, im, config, "v", {"User"},
		{{RelationshipProjectionSource::RPS_TARGET_NODE, "id", "id"},
		 {RelationshipProjectionSource::RPS_EDGE, "weight", "weight"}},
		10);
	op.open();
	auto batch = op.next();

	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 1U);
	EXPECT_EQ(valueAt((*batch)[0], "id"), PropertyValue("live"));
	EXPECT_EQ(valueAt((*batch)[0], "weight"), PropertyValue(int64_t{3}));
	EXPECT_FALSE(op.next().has_value());
}
