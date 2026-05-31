#include <gtest/gtest.h>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>

#include "graph/core/Database.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/query/execution/RelationshipColumnarCountKernel.hpp"

namespace fs = std::filesystem;
using namespace graph;
using namespace graph::query::execution;

class RelationshipColumnarCountKernelTest : public ::testing::Test {
protected:
	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		testDbPath = fs::temp_directory_path() /
		             ("test_relationship_columnar_count_kernel_" + boost::uuids::to_string(uuid) + ".zyx");
		db = std::make_unique<Database>(testDbPath.string());
		db->open();
		dm = db->getStorage()->getDataManager();
		userLabel = dm->getOrCreateTokenId("User");
		followsType = dm->getOrCreateTokenId("FOLLOWS");
		likesType = dm->getOrCreateTokenId("LIKES");
		sourceId = addUser();
		targetId = addUser();
	}

	void TearDown() override {
		graph::debug::PerfTrace::reset();
		graph::debug::PerfTrace::setEnabled(false);
		dm.reset();
		if (db) {
			db->close();
		}
		db.reset();
		std::error_code ec;
		fs::remove_all(testDbPath, ec);
	}

	int64_t addUser() {
		Node node(0, userLabel);
		dm->addNode(node);
		return node.getId();
	}

	Edge addRelationship(int64_t typeId) {
		Edge edge(0, sourceId, targetId, typeId);
		dm->addEdge(edge);
		return dm->getEdge(edge.getId());
	}

	Edge addRelationshipWithProperties(
			int64_t typeId,
			const std::unordered_map<std::string, PropertyValue> &properties) {
		Edge edge(0, sourceId, targetId, typeId);
		dm->addEdge(edge);
		dm->addEdgeProperties(edge.getId(), properties);
		return dm->getEdge(edge.getId());
	}

	RelationshipColumnarCountRequest request(int64_t endId, int64_t typeId) const {
		RelationshipColumnarCountRequest countRequest;
		countRequest.beginId = 1;
		countRequest.endId = endId;
		countRequest.typeId = typeId;
		return countRequest;
	}

	fs::path testDbPath;
	std::unique_ptr<Database> db;
	std::shared_ptr<storage::DataManager> dm;
	int64_t userLabel = 0;
	int64_t followsType = 0;
	int64_t likesType = 0;
	int64_t sourceId = 0;
	int64_t targetId = 0;
};

TEST_F(RelationshipColumnarCountKernelTest, RejectsMissingUnsafeAndShortRanges) {
	RelationshipColumnarCountKernel missingStorage(nullptr);
	EXPECT_FALSE(missingStorage.count(request(128, followsType)).has_value());

	RelationshipColumnarCountKernel kernel(dm);
	EXPECT_FALSE(kernel.count(request(1, followsType)).has_value());

	for (int i = 0; i < 128; ++i) {
		addRelationship(followsType);
	}
	EXPECT_FALSE(kernel.count(request(128, followsType)).has_value());
}

TEST_F(RelationshipColumnarCountKernelTest, CountsTypeOnlyRelationshipsFromMetadata) {
	int64_t maxEdgeId = 0;
	for (int i = 0; i < 130; ++i) {
		const int64_t typeId = i < 120 ? followsType : likesType;
		maxEdgeId = addRelationship(typeId).getId();
	}
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());

	RelationshipColumnarCountKernel kernel(dm);
	auto follows = kernel.count(request(maxEdgeId, followsType));
	ASSERT_TRUE(follows.has_value());
	EXPECT_EQ(follows->count, 120);
	EXPECT_EQ(follows->propertyCandidates, 0U);
	EXPECT_EQ(follows->fallbackEdges, 0U);

	auto allTypes = kernel.count(request(maxEdgeId, 0));
	ASSERT_TRUE(allTypes.has_value());
	EXPECT_EQ(allTypes->count, 130);

	graph::debug::PerfTrace::setEnabled(true);
	graph::debug::PerfTrace::reset();
	auto cachedFollows = kernel.count(request(maxEdgeId, followsType));
	auto snapshot = graph::debug::PerfTrace::snapshotAndReset();
	ASSERT_TRUE(cachedFollows.has_value());
	EXPECT_EQ(cachedFollows->count, 120);
	EXPECT_TRUE(snapshot.contains("relationship_count.type_cache"));
	EXPECT_FALSE(snapshot.contains("relationship_count.load_edge_metadata"));

	maxEdgeId = addRelationship(followsType).getId();
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());
	graph::debug::PerfTrace::reset();
	auto refreshedFollows = kernel.count(request(maxEdgeId, followsType));
	snapshot = graph::debug::PerfTrace::snapshotAndReset();
	ASSERT_TRUE(refreshedFollows.has_value());
	EXPECT_EQ(refreshedFollows->count, 121);
	EXPECT_FALSE(snapshot.contains("relationship_count.type_cache"));
	EXPECT_TRUE(snapshot.contains("relationship_count.load_edge_metadata"));
}

TEST_F(RelationshipColumnarCountKernelTest, CountsPropertyPredicatesWithoutMaterializingRows) {
	int64_t maxEdgeId = 0;
	maxEdgeId = addRelationshipWithProperties(
		followsType,
		{{"weight", PropertyValue(int64_t{7})}, {"kind", PropertyValue("fast")}}).getId();
	maxEdgeId = addRelationshipWithProperties(followsType, {{"weight", PropertyValue(int64_t{7})}}).getId();
	maxEdgeId = addRelationshipWithProperties(followsType, {{"weight", PropertyValue(int64_t{3})}}).getId();
	const std::string largePayload(512, 'x');
	Edge blobBacked = addRelationshipWithProperties(
		followsType,
		{{"weight", PropertyValue(int64_t{7})}, {"payload", PropertyValue(largePayload)}});
	maxEdgeId = blobBacked.getId();
	ASSERT_EQ(blobBacked.getPropertyStorageType(), PropertyStorageType::BLOB_ENTITY);
	addRelationshipWithProperties(likesType, {{"weight", PropertyValue(int64_t{7})}});
	for (int i = 0; i < 125; ++i) {
		maxEdgeId = addRelationship(i % 2 == 0 ? followsType : likesType).getId();
	}
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());

	RelationshipColumnarCountKernel kernel(dm);
	auto weightRequest = request(maxEdgeId, followsType);
	weightRequest.propertyPredicates = {{"weight", PropertyValue(int64_t{7})}};

	graph::debug::PerfTrace::setEnabled(true);
	graph::debug::PerfTrace::reset();
	auto weightMatches = kernel.count(weightRequest);
	auto snapshot = graph::debug::PerfTrace::snapshotAndReset();

	ASSERT_TRUE(weightMatches.has_value());
	EXPECT_EQ(weightMatches->count, 3);
	EXPECT_EQ(weightMatches->propertyCandidates, 3U);
	EXPECT_EQ(weightMatches->fallbackEdges, 1U);
	EXPECT_TRUE(snapshot.contains("relationship_count.load_edge_metadata"));
	EXPECT_TRUE(snapshot.contains("relationship_count.property_predicate"));
	EXPECT_TRUE(snapshot.contains("relationship_count.property_fallback"));

	graph::debug::PerfTrace::reset();
	auto cachedWeightMatches = kernel.count(weightRequest);
	snapshot = graph::debug::PerfTrace::snapshotAndReset();
	ASSERT_TRUE(cachedWeightMatches.has_value());
	EXPECT_EQ(cachedWeightMatches->count, 3);
	EXPECT_TRUE(snapshot.contains("relationship_count.property_cache"));
	EXPECT_FALSE(snapshot.contains("relationship_count.load_edge_metadata"));

	auto compoundRequest = request(maxEdgeId, followsType);
	compoundRequest.propertyPredicates = {
		{"weight", PropertyValue(int64_t{7})},
		{"kind", PropertyValue("fast")}};
	auto compoundMatches = kernel.count(compoundRequest);
	ASSERT_TRUE(compoundMatches.has_value());
	EXPECT_EQ(compoundMatches->count, 1);

	auto noMatchRequest = request(maxEdgeId, followsType);
	noMatchRequest.propertyPredicates = {{"weight", PropertyValue(int64_t{99})}};
	auto noMatches = kernel.count(noMatchRequest);
	ASSERT_TRUE(noMatches.has_value());
	EXPECT_EQ(noMatches->count, 0);

	maxEdgeId = addRelationshipWithProperties(followsType, {{"weight", PropertyValue(int64_t{7})}}).getId();
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());

	weightRequest = request(maxEdgeId, followsType);
	weightRequest.propertyPredicates = {{"weight", PropertyValue(int64_t{7})}};
	graph::debug::PerfTrace::reset();
	auto refreshedWeightMatches = kernel.count(weightRequest);
	snapshot = graph::debug::PerfTrace::snapshotAndReset();
	ASSERT_TRUE(refreshedWeightMatches.has_value());
	EXPECT_EQ(refreshedWeightMatches->count, 4);
	EXPECT_FALSE(snapshot.contains("relationship_count.property_cache"));
	EXPECT_TRUE(snapshot.contains("relationship_count.load_edge_metadata"));
}
