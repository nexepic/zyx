#include <gtest/gtest.h>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <algorithm>
#include <filesystem>
#include <memory>
#include <unordered_map>

#include "graph/core/Database.hpp"
#include "graph/query/planner/RelationshipAccessPathPlanner.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/indexes/IndexManager.hpp"

using namespace graph;
using namespace graph::query::execution;
using namespace graph::query::planner;

namespace fs = std::filesystem;

namespace {

class RelationshipAccessPathPlannerTest : public ::testing::Test {
protected:
	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		dbPath = fs::temp_directory_path() /
		         ("test_relationship_access_path_planner_" + boost::uuids::to_string(uuid) + ".zyx");
		db = std::make_unique<Database>(dbPath.string());
		db->open();
		dm = db->getStorage()->getDataManager();
		im = db->getQueryEngine()->getIndexManager();
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
		fs::remove_all(dbPath, ec);
	}

	int64_t addUser() {
		Node node(0, userLabel);
		dm->addNode(node);
		return node.getId();
	}

	void addRelationship(int64_t source,
	                     int64_t target,
	                     int64_t typeId,
	                     const std::unordered_map<std::string, PropertyValue> &properties = {}) {
		Edge edge(0, source, target, typeId);
		dm->addEdge(edge);
		if (!properties.empty()) {
			dm->addEdgeProperties(edge.getId(), properties);
		}
	}

	fs::path dbPath;
	std::unique_ptr<Database> db;
	std::shared_ptr<storage::DataManager> dm;
	std::shared_ptr<query::indexes::IndexManager> im;
	int64_t userLabel = 0;
	int64_t followsType = 0;
	int64_t likesType = 0;
};

} // namespace

TEST_F(RelationshipAccessPathPlannerTest, FallsBackToColumnarScanWhenNoIndexCanSeedCandidates) {
	DirectRelationshipCountConfig config;
	config.enabled = true;
	config.edgeType = "FOLLOWS";
	config.edgeProperties = {{"weight", PropertyValue(int64_t{1})}};

	auto decision = chooseRelationshipAccessPathDecision(config, im);

	ASSERT_FALSE(decision.candidates.empty());
	EXPECT_EQ(decision.selected.kind, RelationshipAccessPathKind::RAK_COLUMNAR_SCAN);
	EXPECT_FALSE(decision.selectedSupportsDirectCandidateLookup());
	EXPECT_FALSE(decision.selectedEstimatedCardinality().has_value());
}

TEST_F(RelationshipAccessPathPlannerTest, DisabledConfigDoesNotPlanIndexCandidates) {
	const int64_t a = addUser();
	const int64_t b = addUser();
	addRelationship(a, b, followsType, {{"weight", PropertyValue(int64_t{1})}});
	ASSERT_TRUE(im->createIndex("edge_disabled_type_access_path_idx", "edge", "", ""));
	ASSERT_TRUE(im->createIndex("edge_disabled_weight_access_path_idx", "edge", "", "weight"));

	DirectRelationshipCountConfig config;
	config.enabled = false;
	config.edgeType = "FOLLOWS";
	config.edgeProperties = {{"weight", PropertyValue(int64_t{1})}};
	auto decision = chooseRelationshipAccessPathDecision(config, im);

	ASSERT_EQ(decision.candidates.size(), 1U);
	EXPECT_EQ(decision.selected.kind, RelationshipAccessPathKind::RAK_COLUMNAR_SCAN);
	EXPECT_FALSE(decision.selected.valid);
	EXPECT_FALSE(decision.selectedSupportsDirectCandidateLookup());
}

TEST_F(RelationshipAccessPathPlannerTest, TypeOnlyCountPrefersExactTypeIndexWhenAvailable) {
	const int64_t a = addUser();
	const int64_t b = addUser();
	addRelationship(a, b, followsType);
	addRelationship(b, a, followsType);
	addRelationship(a, a, likesType);
	ASSERT_TRUE(im->createIndex("edge_type_access_path_idx", "edge", "", ""));
	im->resetStats();

	DirectRelationshipCountConfig config;
	config.enabled = true;
	config.edgeType = "FOLLOWS";
	auto decision = chooseRelationshipAccessPathDecision(config, im);

	EXPECT_EQ(decision.selected.kind, RelationshipAccessPathKind::RAK_TYPE_INDEX);
	EXPECT_EQ(decision.selected.reason, "type_index");
	EXPECT_TRUE(decision.selectedSupportsDirectCandidateLookup());
	ASSERT_TRUE(decision.selected.estimate.cardinality.has_value());
	EXPECT_EQ(*decision.selected.estimate.cardinality, 2);
	auto typeCandidate = std::find_if(decision.candidates.begin(), decision.candidates.end(), [](const auto &candidate) {
		return candidate.kind == RelationshipAccessPathKind::RAK_TYPE_INDEX;
	});
	ASSERT_NE(typeCandidate, decision.candidates.end());
	ASSERT_TRUE(typeCandidate->estimate.cardinality.has_value());
	EXPECT_EQ(*typeCandidate->estimate.cardinality, 2);
	EXPECT_TRUE(typeCandidate->estimate.exactCardinality);
	EXPECT_EQ(typeCandidate->estimate.source, "index_count");
	EXPECT_EQ(im->lookups(), 0u);
	EXPECT_EQ(im->indexHits(), 0u);
}

TEST_F(RelationshipAccessPathPlannerTest, NonEqualityPredicatesStayOnColumnarScan) {
	const int64_t a = addUser();
	const int64_t b = addUser();
	addRelationship(a, b, followsType, {{"weight", PropertyValue(int64_t{1})}});
	ASSERT_TRUE(im->createIndex("edge_gt_weight_access_path_idx", "edge", "", "weight"));

	VectorizedPropertyPredicate predicate;
	predicate.propertyKey = "weight";
	predicate.op = VectorPredicateOp::VPO_GT;
	predicate.value = PropertyValue(int64_t{0});
	DirectRelationshipCountConfig config;
	config.enabled = true;
	config.edgePredicates = {predicate};
	auto decision = chooseRelationshipAccessPathDecision(config, im);

	EXPECT_EQ(decision.selected.kind, RelationshipAccessPathKind::RAK_COLUMNAR_SCAN);
	EXPECT_FALSE(decision.selectedSupportsDirectCandidateLookup());
}

TEST_F(RelationshipAccessPathPlannerTest, PropertyIndexChoosesMostSelectiveEqualityPredicate) {
	const int64_t a = addUser();
	const int64_t b = addUser();
	for (int64_t id = 0; id < 8; ++id) {
		addRelationship(a, b, followsType, {
			{"weight", PropertyValue(int64_t{1})},
			{"rank", PropertyValue(id == 3 ? int64_t{7} : int64_t{8})}
		});
	}
	ASSERT_TRUE(im->createIndex("edge_weight_access_path_idx", "edge", "", "weight"));
	ASSERT_TRUE(im->createIndex("edge_rank_access_path_idx", "edge", "", "rank"));
	im->resetStats();

	DirectRelationshipCountConfig config;
	config.enabled = true;
	config.edgeProperties = {
		{"weight", PropertyValue(int64_t{1})},
		{"rank", PropertyValue(int64_t{7})},
	};
	auto decision = chooseRelationshipAccessPathDecision(config, im);

	EXPECT_EQ(decision.selected.kind, RelationshipAccessPathKind::RAK_PROPERTY_INDEX);
	ASSERT_EQ(decision.selected.propertyKeysSatisfied.size(), 1U);
	EXPECT_EQ(decision.selected.propertyKeysSatisfied[0], "rank");
	const auto source = relationshipCandidateSourceForAccessPath(decision.selected);
	EXPECT_EQ(source.type, DirectRelationshipCandidateSourceType::DRCS_PROPERTY_INDEX);
	EXPECT_EQ(source.propertyKeys, (std::vector<std::string>{"rank"}));
	ASSERT_TRUE(decision.selectedEstimatedCardinality().has_value());
	EXPECT_EQ(*decision.selectedEstimatedCardinality(), 1);
	EXPECT_EQ(im->lookups(), 0u);
	EXPECT_EQ(im->indexHits(), 0u);
}

TEST_F(RelationshipAccessPathPlannerTest, EqualPropertyEstimatesUseStableCandidateOrdering) {
	const int64_t a = addUser();
	const int64_t b = addUser();
	addRelationship(a, b, followsType, {
		{"left", PropertyValue(int64_t{1})},
		{"right", PropertyValue(int64_t{1})}
	});
	ASSERT_TRUE(im->createIndex("edge_left_access_path_idx", "edge", "", "left"));
	ASSERT_TRUE(im->createIndex("edge_right_access_path_idx", "edge", "", "right"));

	DirectRelationshipCountConfig config;
	config.enabled = true;
	config.edgeProperties = {
		{"left", PropertyValue(int64_t{1})},
		{"right", PropertyValue(int64_t{1})},
	};
	auto decision = chooseRelationshipAccessPathDecision(config, im);

	EXPECT_EQ(decision.selected.kind, RelationshipAccessPathKind::RAK_PROPERTY_INDEX);
	ASSERT_EQ(decision.selected.propertyKeysSatisfied.size(), 1U);
	EXPECT_EQ(decision.selected.propertyKeysSatisfied[0], "left");
}

TEST_F(RelationshipAccessPathPlannerTest, TypeAndPropertyIndexesExposeIntersectionCandidate) {
	const int64_t a = addUser();
	const int64_t b = addUser();
	addRelationship(a, b, followsType, {{"weight", PropertyValue(int64_t{1})}});
	addRelationship(b, a, followsType, {{"weight", PropertyValue(int64_t{2})}});
	addRelationship(a, a, likesType, {{"weight", PropertyValue(int64_t{1})}});
	ASSERT_TRUE(im->createIndex("edge_type_intersection_access_path_idx", "edge", "", ""));
	ASSERT_TRUE(im->createIndex("edge_weight_intersection_access_path_idx", "edge", "", "weight"));

	DirectRelationshipCountConfig config;
	config.enabled = true;
	config.edgeType = "FOLLOWS";
	config.edgeProperties = {{"weight", PropertyValue(int64_t{1})}};
	auto decision = chooseRelationshipAccessPathDecision(config, im);

	EXPECT_EQ(decision.selected.kind, RelationshipAccessPathKind::RAK_TYPE_PROPERTY_INTERSECTION);
	EXPECT_TRUE(decision.selectedSupportsDirectCandidateLookup());
	EXPECT_TRUE(decision.selected.typeSatisfied);
	ASSERT_EQ(decision.selected.propertyKeysSatisfied.size(), 1U);
	EXPECT_EQ(decision.selected.propertyKeysSatisfied[0], "weight");
	ASSERT_TRUE(decision.selectedEstimatedCardinality().has_value());
	EXPECT_EQ(*decision.selectedEstimatedCardinality(), 2);
	EXPECT_FALSE(decision.selected.estimate.exactCardinality);
	const auto source = relationshipCandidateSourceForAccessPath(decision.selected);
	EXPECT_EQ(source.type, DirectRelationshipCandidateSourceType::DRCS_TYPE_PROPERTY_INTERSECTION);
	EXPECT_EQ(source.propertyKeys, (std::vector<std::string>{"weight"}));

	const auto summary = summarizeRelationshipAccessPath(decision.selected);
	EXPECT_EQ(summary.kind, "type_property_intersection");
	EXPECT_EQ(summary.reason, "type_property_intersection");
	EXPECT_EQ(summary.estimateSource, "index_count_upper_bound");
}

TEST(RelationshipAccessPathKindNameTest, CoversAllRelationshipAccessPathKinds) {
	EXPECT_STREQ(relationshipAccessPathKindName(RelationshipAccessPathKind::RAK_COLUMNAR_SCAN), "columnar_scan");
	EXPECT_STREQ(relationshipAccessPathKindName(RelationshipAccessPathKind::RAK_TYPE_INDEX), "type_index");
	EXPECT_STREQ(relationshipAccessPathKindName(RelationshipAccessPathKind::RAK_PROPERTY_INDEX), "property_index");
	EXPECT_STREQ(relationshipAccessPathKindName(RelationshipAccessPathKind::RAK_TYPE_PROPERTY_INTERSECTION),
	             "type_property_intersection");
	EXPECT_STREQ(relationshipAccessPathKindName(static_cast<RelationshipAccessPathKind>(999)), "unknown");
}

TEST(RelationshipAccessPathCandidateSourceTest, MapsSyntheticCandidateSourceBranches) {
	RelationshipAccessPathCandidate typeCandidate;
	typeCandidate.kind = RelationshipAccessPathKind::RAK_TYPE_INDEX;
	typeCandidate.valid = true;
	typeCandidate.directCandidateLookup = true;
	auto typeSource = relationshipCandidateSourceForAccessPath(typeCandidate);
	EXPECT_EQ(typeSource.type, DirectRelationshipCandidateSourceType::DRCS_TYPE_INDEX);
	EXPECT_TRUE(typeSource.propertyKeys.empty());

	RelationshipAccessPathCandidate invalidPropertyCandidate;
	invalidPropertyCandidate.kind = RelationshipAccessPathKind::RAK_PROPERTY_INDEX;
	invalidPropertyCandidate.valid = false;
	invalidPropertyCandidate.directCandidateLookup = true;
	invalidPropertyCandidate.propertyKeysSatisfied = {"weight"};
	auto invalidSource = relationshipCandidateSourceForAccessPath(invalidPropertyCandidate);
	EXPECT_EQ(invalidSource.type, DirectRelationshipCandidateSourceType::DRCS_AUTO);
	EXPECT_TRUE(invalidSource.propertyKeys.empty());

	RelationshipAccessPathCandidate columnarCandidate;
	columnarCandidate.kind = RelationshipAccessPathKind::RAK_COLUMNAR_SCAN;
	columnarCandidate.valid = true;
	columnarCandidate.directCandidateLookup = true;
	columnarCandidate.propertyKeysSatisfied = {"weight"};
	auto columnarSource = relationshipCandidateSourceForAccessPath(columnarCandidate);
	EXPECT_EQ(columnarSource.type, DirectRelationshipCandidateSourceType::DRCS_AUTO);
	EXPECT_TRUE(columnarSource.propertyKeys.empty());

	RelationshipAccessPathCandidate unknownCandidate;
	unknownCandidate.kind = static_cast<RelationshipAccessPathKind>(999);
	unknownCandidate.valid = true;
	unknownCandidate.directCandidateLookup = true;
	auto unknownSource = relationshipCandidateSourceForAccessPath(unknownCandidate);
	EXPECT_EQ(unknownSource.type, DirectRelationshipCandidateSourceType::DRCS_AUTO);
	EXPECT_TRUE(unknownSource.propertyKeys.empty());
}

TEST(RelationshipAccessPathCandidateSourceTest, DecisionAccessorsExposeCostAndCardinality) {
	RelationshipAccessPathDecision decision;
	decision.selected.estimate.cost = 9.75;
	decision.selected.estimate.cardinality = int64_t{42};

	EXPECT_EQ(decision.selectedEstimatedCost(), 9.75);
	ASSERT_TRUE(decision.selectedEstimatedCardinality().has_value());
	EXPECT_EQ(*decision.selectedEstimatedCardinality(), 42);
}
