#include <gtest/gtest.h>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <memory>
#include <unordered_map>

#include "graph/core/Database.hpp"
#include "graph/query/execution/RelationshipCandidateSource.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/indexes/IndexManager.hpp"

using namespace graph;
using namespace graph::query::execution;

namespace fs = std::filesystem;

namespace {

class RelationshipCandidateSourceTest : public ::testing::Test {
protected:
	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		dbPath = fs::temp_directory_path() /
		         ("test_relationship_candidate_source_" + boost::uuids::to_string(uuid) + ".zyx");
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

	void addEdge(int64_t source,
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

TEST_F(RelationshipCandidateSourceTest, NullManagersAndMissingIndexesReturnUnavailable) {
	DirectRelationshipCountConfig config;
	config.edgeType = "FOLLOWS";
	config.edgeProperties = {{"weight", PropertyValue(int64_t{1})}};

	RelationshipCandidateSource nullSource(nullptr, nullptr);
	EXPECT_FALSE(nullSource.collect(config).available);
	RelationshipCandidateSource nullIndexSource(dm, nullptr);
	EXPECT_FALSE(nullIndexSource.collect(config).available);

	const int64_t a = addUser();
	const int64_t b = addUser();
	addEdge(a, b, followsType, {{"weight", PropertyValue(int64_t{1})}});
	RelationshipCandidateSource source(dm, im);
	EXPECT_FALSE(source.collect(config).available);

	DirectRelationshipCountConfig unconstrained;
	EXPECT_FALSE(source.collect(unconstrained).available);
}

TEST_F(RelationshipCandidateSourceTest, UnknownPlannedSourceTypeIsRejected) {
	DirectRelationshipCountConfig config;
	config.candidateSource.type = static_cast<DirectRelationshipCandidateSourceType>(127);

	RelationshipCandidateSource source(dm, im);
	const auto candidates = source.collect(config);

	EXPECT_FALSE(candidates.available);
	EXPECT_TRUE(candidates.ids.empty());
}

TEST_F(RelationshipCandidateSourceTest, TypeIndexProvidesActiveTypeSatisfiedCandidates) {
	const int64_t a = addUser();
	const int64_t b = addUser();
	addEdge(a, b, followsType);
	addEdge(b, a, followsType);
	addEdge(a, a, likesType);
	ASSERT_TRUE(im->createIndex("edge_type_candidates_idx", "edge", "", ""));

	DirectRelationshipCountConfig config;
	config.edgeType = "FOLLOWS";
	RelationshipCandidateSource source(dm, im);
	const auto candidates = source.collect(config);

	EXPECT_TRUE(candidates.available);
	EXPECT_TRUE(candidates.activeOnly);
	EXPECT_TRUE(candidates.typeSatisfied);
	EXPECT_EQ(candidates.ids.size(), 2U);
	EXPECT_TRUE(candidates.propertyKeysSatisfied.empty());
}

TEST_F(RelationshipCandidateSourceTest, PlannedTypeIndexSourceRequiresUsableTypeIndex) {
	const int64_t a = addUser();
	const int64_t b = addUser();
	addEdge(a, b, followsType);

	DirectRelationshipCountConfig config;
	config.edgeType = "FOLLOWS";
	config.candidateSource.type = DirectRelationshipCandidateSourceType::DRCS_TYPE_INDEX;
	RelationshipCandidateSource source(dm, im);
	EXPECT_FALSE(source.collect(config).available);

	ASSERT_TRUE(im->createIndex("edge_type_planned_candidates_idx", "edge", "", ""));
	const auto candidates = source.collect(config);
	EXPECT_TRUE(candidates.available);
	EXPECT_TRUE(candidates.activeOnly);
	EXPECT_TRUE(candidates.typeSatisfied);
	ASSERT_EQ(candidates.ids.size(), 1U);
}

TEST_F(RelationshipCandidateSourceTest, PropertyIndexChoosesIndexedPredicateWithoutTypeRequirement) {
	const int64_t a = addUser();
	const int64_t b = addUser();
	addEdge(a, b, followsType, {{"weight", PropertyValue(int64_t{1})}, {"rank", PropertyValue(int64_t{7})}});
	addEdge(b, a, likesType, {{"weight", PropertyValue(int64_t{1})}, {"rank", PropertyValue(int64_t{8})}});
	addEdge(a, a, followsType, {{"weight", PropertyValue(int64_t{2})}, {"rank", PropertyValue(int64_t{7})}});
	ASSERT_TRUE(im->createIndex("edge_weight_candidates_idx", "edge", "FOLLOWS", "weight"));
	ASSERT_TRUE(im->createIndex("edge_rank_candidates_idx", "edge", "FOLLOWS", "rank"));

	DirectRelationshipCountConfig config;
	config.edgeProperties = {
		{"weight", PropertyValue(int64_t{1})},
		{"rank", PropertyValue(int64_t{7})},
	};
	RelationshipCandidateSource source(dm, im);
	const auto candidates = source.collect(config);

	EXPECT_TRUE(candidates.available);
	EXPECT_TRUE(candidates.activeOnly);
	EXPECT_TRUE(candidates.typeSatisfied);
	EXPECT_EQ(candidates.ids.size(), 2U);
	ASSERT_EQ(candidates.propertyKeysSatisfied.size(), 1U);
	EXPECT_TRUE(candidates.propertyKeysSatisfied[0] == "weight" || candidates.propertyKeysSatisfied[0] == "rank");
}

TEST_F(RelationshipCandidateSourceTest, PropertyAndTypeIndexesIntersectCandidates) {
	const int64_t a = addUser();
	const int64_t b = addUser();
	addEdge(a, b, followsType, {{"weight", PropertyValue(int64_t{1})}});
	addEdge(b, a, followsType, {{"weight", PropertyValue(int64_t{2})}});
	addEdge(a, a, likesType, {{"weight", PropertyValue(int64_t{1})}});
	ASSERT_TRUE(im->createIndex("edge_type_intersect_idx", "edge", "", ""));
	ASSERT_TRUE(im->createIndex("edge_weight_intersect_idx", "edge", "FOLLOWS", "weight"));

	DirectRelationshipCountConfig config;
	config.edgeType = "FOLLOWS";
	config.edgeProperties = {{"weight", PropertyValue(int64_t{1})}};
	RelationshipCandidateSource source(dm, im);
	const auto candidates = source.collect(config);

	EXPECT_TRUE(candidates.available);
	EXPECT_TRUE(candidates.typeSatisfied);
	ASSERT_EQ(candidates.ids.size(), 1U);
	EXPECT_EQ(dm->getEdge(candidates.ids.front()).getTypeId(), followsType);
}

TEST_F(RelationshipCandidateSourceTest, AutoChoosesMostSelectiveIndexedEqualityPredicate) {
	const int64_t a = addUser();
	const int64_t b = addUser();
	addEdge(a, b, followsType, {{"weight", PropertyValue(int64_t{1})}, {"rank", PropertyValue(int64_t{7})}});
	addEdge(a, b, followsType, {{"weight", PropertyValue(int64_t{1})}, {"rank", PropertyValue(int64_t{8})}});
	ASSERT_TRUE(im->createIndex("edge_candidate_auto_weight_idx", "edge", "", "weight"));
	ASSERT_TRUE(im->createIndex("edge_candidate_auto_rank_idx", "edge", "", "rank"));

	DirectRelationshipCountConfig config;
	config.enabled = true;
	config.edgeProperties = {
		{"weight", PropertyValue(int64_t{1})},
		{"rank", PropertyValue(int64_t{7})},
	};

	const RelationshipCandidateSource source(dm, im);
	const auto candidates = source.collect(config);

	EXPECT_TRUE(candidates.available);
	EXPECT_EQ(candidates.ids.size(), 1U);
	EXPECT_EQ(candidates.propertyKeysSatisfied, (std::vector<std::string>{"rank"}));
	EXPECT_TRUE(candidates.typeSatisfied);
}

TEST_F(RelationshipCandidateSourceTest, PlannedPropertySourceUsesRequestedIndexedPredicate) {
	const int64_t a = addUser();
	const int64_t b = addUser();
	addEdge(a, b, followsType, {{"weight", PropertyValue(int64_t{1})}, {"rank", PropertyValue(int64_t{7})}});
	addEdge(a, b, followsType, {{"weight", PropertyValue(int64_t{1})}, {"rank", PropertyValue(int64_t{8})}});
	ASSERT_TRUE(im->createIndex("edge_candidate_planned_weight_idx", "edge", "", "weight"));
	ASSERT_TRUE(im->createIndex("edge_candidate_planned_rank_idx", "edge", "", "rank"));

	DirectRelationshipCountConfig config;
	config.enabled = true;
	config.edgeProperties = {
		{"weight", PropertyValue(int64_t{1})},
		{"rank", PropertyValue(int64_t{7})},
	};
	config.candidateSource.type = DirectRelationshipCandidateSourceType::DRCS_PROPERTY_INDEX;
	config.candidateSource.propertyKeys = {"weight"};

	const RelationshipCandidateSource source(dm, im);
	const auto candidates = source.collect(config);

	EXPECT_TRUE(candidates.available);
	EXPECT_EQ(candidates.ids.size(), 2U);
	EXPECT_EQ(candidates.propertyKeysSatisfied, (std::vector<std::string>{"weight"}));
	EXPECT_TRUE(candidates.typeSatisfied);
}

TEST_F(RelationshipCandidateSourceTest, PlannedPropertySourceRequiresIndexedRequestedKeys) {
	const int64_t a = addUser();
	const int64_t b = addUser();
	addEdge(a, b, followsType, {{"weight", PropertyValue(int64_t{1})}, {"rank", PropertyValue(int64_t{7})}});
	ASSERT_TRUE(im->createIndex("edge_candidate_required_weight_idx", "edge", "", "weight"));
	ASSERT_TRUE(im->createIndex("edge_candidate_required_rank_idx", "edge", "", "rank"));

	DirectRelationshipCountConfig config;
	config.enabled = true;
	config.edgeProperties = {{"weight", PropertyValue(int64_t{1})}, {"rank", PropertyValue(int64_t{99})}};
	config.candidateSource.type = DirectRelationshipCandidateSourceType::DRCS_PROPERTY_INDEX;
	config.candidateSource.propertyKeys = {};

	const RelationshipCandidateSource source(dm, im);
	EXPECT_FALSE(source.collect(config).available);

	config.candidateSource.propertyKeys = {"weight", "rank"};
	const auto disjoint = source.collect(config);
	EXPECT_TRUE(disjoint.available);
	EXPECT_TRUE(disjoint.ids.empty());
	EXPECT_EQ(disjoint.propertyKeysSatisfied, (std::vector<std::string>{"rank", "weight"}));
}

TEST_F(RelationshipCandidateSourceTest, PlannedPropertySourceIntersectsEqualSizedIndexedPredicates) {
	const int64_t a = addUser();
	const int64_t b = addUser();
	addEdge(a, b, followsType, {{"rank", PropertyValue(int64_t{1})}, {"bucket", PropertyValue(int64_t{9})}});
	addEdge(b, a, followsType, {{"rank", PropertyValue(int64_t{1})}, {"bucket", PropertyValue(int64_t{9})}});
	ASSERT_TRUE(im->createIndex("edge_candidate_equal_rank_idx", "edge", "", "rank"));
	ASSERT_TRUE(im->createIndex("edge_candidate_equal_bucket_idx", "edge", "", "bucket"));

	DirectRelationshipCountConfig config;
	config.enabled = true;
	config.edgeProperties = {
		{"rank", PropertyValue(int64_t{1})},
		{"bucket", PropertyValue(int64_t{9})},
	};
	config.candidateSource.type = DirectRelationshipCandidateSourceType::DRCS_PROPERTY_INDEX;
	config.candidateSource.propertyKeys = {"rank", "bucket"};

	const RelationshipCandidateSource source(dm, im);
	const auto candidates = source.collect(config);

	EXPECT_TRUE(candidates.available);
	EXPECT_EQ(candidates.ids.size(), 2U);
	EXPECT_EQ(candidates.propertyKeysSatisfied, (std::vector<std::string>{"bucket", "rank"}));
}

TEST_F(RelationshipCandidateSourceTest, PlannedTypePropertyIntersectionUsesBothIndexes) {
	const int64_t a = addUser();
	const int64_t b = addUser();
	addEdge(a, b, followsType, {{"weight", PropertyValue(int64_t{1})}});
	addEdge(a, b, likesType, {{"weight", PropertyValue(int64_t{1})}});
	addEdge(a, b, followsType, {{"weight", PropertyValue(int64_t{2})}});
	ASSERT_TRUE(im->createIndex("edge_candidate_intersection_type_idx", "edge", "", ""));
	ASSERT_TRUE(im->createIndex("edge_candidate_intersection_weight_idx", "edge", "", "weight"));

	DirectRelationshipCountConfig config;
	config.enabled = true;
	config.edgeType = "FOLLOWS";
	config.edgeProperties = {{"weight", PropertyValue(int64_t{1})}};
	config.candidateSource.type = DirectRelationshipCandidateSourceType::DRCS_TYPE_PROPERTY_INTERSECTION;
	config.candidateSource.propertyKeys = {"weight"};

	const RelationshipCandidateSource source(dm, im);
	const auto candidates = source.collect(config);

	EXPECT_TRUE(candidates.available);
	EXPECT_EQ(candidates.ids.size(), 1U);
	EXPECT_EQ(candidates.propertyKeysSatisfied, (std::vector<std::string>{"weight"}));
	EXPECT_TRUE(candidates.typeSatisfied);
}

TEST_F(RelationshipCandidateSourceTest, PlannedTypePropertyIntersectionRequiresBothCandidateSources) {
	const int64_t a = addUser();
	const int64_t b = addUser();
	addEdge(a, b, followsType, {{"weight", PropertyValue(int64_t{1})}});

	DirectRelationshipCountConfig config;
	config.enabled = true;
	config.edgeType = "FOLLOWS";
	config.edgeProperties = {{"weight", PropertyValue(int64_t{1})}};
	config.candidateSource.type = DirectRelationshipCandidateSourceType::DRCS_TYPE_PROPERTY_INTERSECTION;
	config.candidateSource.propertyKeys = {"weight"};

	const RelationshipCandidateSource source(dm, im);
	ASSERT_TRUE(im->createIndex("edge_candidate_intersection_missing_type_weight_idx", "edge", "", "weight"));
	EXPECT_FALSE(source.collect(config).available);
}

TEST_F(RelationshipCandidateSourceTest, PlannedTypePropertyIntersectionRequiresPropertyCandidate) {
	const int64_t a = addUser();
	const int64_t b = addUser();
	addEdge(a, b, followsType, {{"weight", PropertyValue(int64_t{1})}});
	ASSERT_TRUE(im->createIndex("edge_candidate_intersection_missing_property_type_idx", "edge", "", ""));

	DirectRelationshipCountConfig config;
	config.enabled = true;
	config.edgeType = "FOLLOWS";
	config.edgeProperties = {{"weight", PropertyValue(int64_t{1})}};
	config.candidateSource.type = DirectRelationshipCandidateSourceType::DRCS_TYPE_PROPERTY_INTERSECTION;
	config.candidateSource.propertyKeys = {"weight"};

	const RelationshipCandidateSource source(dm, im);
	EXPECT_FALSE(source.collect(config).available);
}

TEST_F(RelationshipCandidateSourceTest, AutoIntersectsWithSmallerTypeCandidateSet) {
	const int64_t a = addUser();
	const int64_t b = addUser();
	addEdge(a, b, followsType, {{"weight", PropertyValue(int64_t{1})}});
	for (int i = 0; i < 4; ++i) {
		addEdge(b, a, likesType, {{"weight", PropertyValue(int64_t{1})}});
	}
	ASSERT_TRUE(im->createIndex("edge_candidate_auto_small_type_idx", "edge", "", ""));
	ASSERT_TRUE(im->createIndex("edge_candidate_auto_many_weight_idx", "edge", "", "weight"));

	DirectRelationshipCountConfig config;
	config.enabled = true;
	config.edgeType = "FOLLOWS";
	config.edgeProperties = {{"weight", PropertyValue(int64_t{1})}};

	const RelationshipCandidateSource source(dm, im);
	const auto candidates = source.collect(config);

	EXPECT_TRUE(candidates.available);
	EXPECT_TRUE(candidates.typeSatisfied);
	ASSERT_EQ(candidates.ids.size(), 1U);
	EXPECT_EQ(dm->getEdge(candidates.ids.front()).getTypeId(), followsType);
	EXPECT_EQ(candidates.propertyKeysSatisfied, (std::vector<std::string>{"weight"}));
}

TEST_F(RelationshipCandidateSourceTest, AutoKeepsFirstIndexedPredicateWhenLaterCandidateIsNotSmaller) {
	const int64_t a = addUser();
	const int64_t b = addUser();
	addEdge(a, b, followsType, {{"rank", PropertyValue(int64_t{1})}, {"weight", PropertyValue(int64_t{9})}});
	addEdge(b, a, followsType, {{"rank", PropertyValue(int64_t{1})}, {"weight", PropertyValue(int64_t{9})}});
	ASSERT_TRUE(im->createIndex("edge_candidate_auto_keep_rank_idx", "edge", "", "rank"));
	ASSERT_TRUE(im->createIndex("edge_candidate_auto_keep_weight_idx", "edge", "", "weight"));

	DirectRelationshipCountConfig config;
	config.enabled = true;
	config.edgeProperties = {
		{"rank", PropertyValue(int64_t{1})},
		{"weight", PropertyValue(int64_t{9})},
	};

	const RelationshipCandidateSource source(dm, im);
	const auto candidates = source.collect(config);

	EXPECT_TRUE(candidates.available);
	EXPECT_EQ(candidates.ids.size(), 2U);
	EXPECT_EQ(candidates.propertyKeysSatisfied, (std::vector<std::string>{"rank"}));
}

TEST_F(RelationshipCandidateSourceTest, AutoSwitchesToLaterIndexedPredicateWhenItIsSmaller) {
	const int64_t a = addUser();
	const int64_t b = addUser();
	addEdge(a, b, followsType, {{"acommon", PropertyValue(int64_t{1})}, {"zrare", PropertyValue(int64_t{7})}});
	addEdge(a, b, followsType, {{"acommon", PropertyValue(int64_t{1})}, {"zrare", PropertyValue(int64_t{8})}});
	addEdge(b, a, followsType, {{"acommon", PropertyValue(int64_t{1})}, {"zrare", PropertyValue(int64_t{8})}});
	ASSERT_TRUE(im->createIndex("edge_candidate_auto_later_common_idx", "edge", "", "acommon"));
	ASSERT_TRUE(im->createIndex("edge_candidate_auto_later_rare_idx", "edge", "", "zrare"));

	DirectRelationshipCountConfig config;
	config.enabled = true;
	config.edgeProperties = {
		{"acommon", PropertyValue(int64_t{1})},
		{"zrare", PropertyValue(int64_t{7})},
	};

	const RelationshipCandidateSource source(dm, im);
	const auto candidates = source.collect(config);

	EXPECT_TRUE(candidates.available);
	EXPECT_EQ(candidates.ids.size(), 1U);
	EXPECT_EQ(candidates.propertyKeysSatisfied, (std::vector<std::string>{"zrare"}));
}

TEST_F(RelationshipCandidateSourceTest, PlannedMissingPropertySourceDoesNotSilentlyFallBackToAuto) {
	const int64_t a = addUser();
	const int64_t b = addUser();
	addEdge(a, b, followsType, {{"weight", PropertyValue(int64_t{1})}});
	ASSERT_TRUE(im->createIndex("edge_candidate_missing_weight_idx", "edge", "", "weight"));

	DirectRelationshipCountConfig config;
	config.enabled = true;
	config.edgeProperties = {{"weight", PropertyValue(int64_t{1})}};
	config.candidateSource.type = DirectRelationshipCandidateSourceType::DRCS_PROPERTY_INDEX;
	config.candidateSource.propertyKeys = {"rank"};

	const RelationshipCandidateSource source(dm, im);
	const auto candidates = source.collect(config);

	EXPECT_FALSE(candidates.available);
	EXPECT_TRUE(candidates.ids.empty());
}

TEST_F(RelationshipCandidateSourceTest, PlannedPropertySourceRejectsNonEqualityPredicate) {
	const int64_t a = addUser();
	const int64_t b = addUser();
	addEdge(a, b, followsType, {{"weight", PropertyValue(int64_t{7})}});
	ASSERT_TRUE(im->createIndex("edge_candidate_range_weight_idx", "edge", "", "weight"));

	VectorizedPropertyPredicate rangePredicate;
	rangePredicate.propertyKey = "weight";
	rangePredicate.op = VectorPredicateOp::VPO_GT;
	rangePredicate.value = PropertyValue(int64_t{3});

	DirectRelationshipCountConfig config;
	config.enabled = true;
	config.edgePredicates = {rangePredicate};
	config.candidateSource.type = DirectRelationshipCandidateSourceType::DRCS_PROPERTY_INDEX;
	config.candidateSource.propertyKeys = {"weight"};

	const RelationshipCandidateSource source(dm, im);
	const auto candidates = source.collect(config);

	EXPECT_FALSE(candidates.available);
	EXPECT_TRUE(candidates.ids.empty());
}

TEST_F(RelationshipCandidateSourceTest, AutoFallsBackToTypeIndexWhenPropertyPredicateIsNotIndexable) {
	const int64_t a = addUser();
	const int64_t b = addUser();
	addEdge(a, b, followsType, {{"weight", PropertyValue(int64_t{7})}});
	addEdge(b, a, likesType, {{"weight", PropertyValue(int64_t{9})}});
	ASSERT_TRUE(im->createIndex("edge_candidate_auto_range_type_idx", "edge", "", ""));
	ASSERT_TRUE(im->createIndex("edge_candidate_auto_range_weight_idx", "edge", "", "weight"));

	VectorizedPropertyPredicate rangePredicate;
	rangePredicate.propertyKey = "weight";
	rangePredicate.op = VectorPredicateOp::VPO_GE;
	rangePredicate.value = PropertyValue(int64_t{7});

	DirectRelationshipCountConfig config;
	config.enabled = true;
	config.edgeType = "FOLLOWS";
	config.edgePredicates = {rangePredicate};

	const RelationshipCandidateSource source(dm, im);
	const auto candidates = source.collect(config);

	EXPECT_TRUE(candidates.available);
	EXPECT_TRUE(candidates.activeOnly);
	EXPECT_TRUE(candidates.typeSatisfied);
	EXPECT_TRUE(candidates.propertyKeysSatisfied.empty());
	ASSERT_EQ(candidates.ids.size(), 1U);
	EXPECT_EQ(dm->getEdge(candidates.ids.front()).getTypeId(), followsType);
}
