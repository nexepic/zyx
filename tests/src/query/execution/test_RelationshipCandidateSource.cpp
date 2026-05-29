#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <unordered_map>

#include "graph/core/Database.hpp"
#include "graph/query/execution/RelationshipCandidateSource.hpp"

namespace fs = std::filesystem;
using namespace graph;
using namespace graph::query::execution;

class RelationshipCandidateSourceTest : public ::testing::Test {
protected:
	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		dbPath = fs::temp_directory_path() / ("test_relationship_candidate_source_" + boost::uuids::to_string(uuid) + ".zyx");
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
		fs::remove(dbPath, ec);
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

TEST_F(RelationshipCandidateSourceTest, NullManagersAndMissingIndexesReturnUnavailable) {
	DirectRelationshipCountConfig config;
	config.edgeType = "FOLLOWS";
	config.edgeProperties = {{"weight", PropertyValue(int64_t{1})}};

	RelationshipCandidateSource nullSource(nullptr, nullptr);
	EXPECT_FALSE(nullSource.collect(config).available);

	const int64_t a = addUser();
	const int64_t b = addUser();
	addEdge(a, b, followsType, {{"weight", PropertyValue(int64_t{1})}});
	RelationshipCandidateSource source(dm, im);
	EXPECT_FALSE(source.collect(config).available);

	DirectRelationshipCountConfig unconstrained;
	EXPECT_FALSE(source.collect(unconstrained).available);
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
