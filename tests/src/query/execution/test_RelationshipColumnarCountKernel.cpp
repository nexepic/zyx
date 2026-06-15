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

	Edge addRelationshipWithProperties(int64_t typeId,
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

TEST_F(RelationshipColumnarCountKernelTest, RejectsMissingAndShortRangesButCountsDirtyOverlay) {
	RelationshipColumnarCountKernel missingStorage(nullptr);
	EXPECT_FALSE(missingStorage.count(request(128, followsType)).has_value());

	RelationshipColumnarCountKernel kernel(dm);
	EXPECT_FALSE(kernel.count(request(1, followsType)).has_value());

	for (int i = 0; i < 128; ++i) {
		addRelationship(followsType);
	}
	auto dirtyCount = kernel.count(request(128, followsType));
	ASSERT_TRUE(dirtyCount.has_value());
	EXPECT_EQ(dirtyCount->count, 128);
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
	auto repeatedFollows = kernel.count(request(maxEdgeId, followsType));
	auto snapshot = graph::debug::PerfTrace::snapshotAndReset();
	ASSERT_TRUE(repeatedFollows.has_value());
	EXPECT_EQ(repeatedFollows->count, 120);
	EXPECT_TRUE(snapshot.contains("relationship_count.load_edge_metadata"));
	EXPECT_FALSE(snapshot.contains("relationship_count.type_cache"));

	maxEdgeId = addRelationship(followsType).getId();
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());
	graph::debug::PerfTrace::reset();
	auto refreshedFollows = kernel.count(request(maxEdgeId, followsType));
	snapshot = graph::debug::PerfTrace::snapshotAndReset();
	ASSERT_TRUE(refreshedFollows.has_value());
	EXPECT_EQ(refreshedFollows->count, 121);
	EXPECT_TRUE(snapshot.contains("relationship_count.load_edge_metadata"));
	EXPECT_FALSE(snapshot.contains("relationship_count.type_cache"));
}

TEST_F(RelationshipColumnarCountKernelTest, CountsPropertyPredicatesWithoutMaterializingRows) {
	int64_t maxEdgeId = 0;
	maxEdgeId = addRelationshipWithProperties(followsType,
											  {{"weight", PropertyValue(int64_t{7})}, {"kind", PropertyValue("fast")}})
						.getId();
	maxEdgeId = addRelationshipWithProperties(followsType, {{"weight", PropertyValue(int64_t{7})}}).getId();
	maxEdgeId = addRelationshipWithProperties(followsType, {{"weight", PropertyValue(int64_t{3})}}).getId();
	const std::string largePayload(512, 'x');
	Edge blobBacked = addRelationshipWithProperties(
			followsType, {{"weight", PropertyValue(int64_t{7})}, {"payload", PropertyValue(largePayload)}});
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
	auto repeatedWeightMatches = kernel.count(weightRequest);
	snapshot = graph::debug::PerfTrace::snapshotAndReset();
	ASSERT_TRUE(repeatedWeightMatches.has_value());
	EXPECT_EQ(repeatedWeightMatches->count, 3);
	EXPECT_TRUE(snapshot.contains("relationship_count.load_edge_metadata"));
	EXPECT_TRUE(snapshot.contains("relationship_count.property_predicate"));
	EXPECT_FALSE(snapshot.contains("relationship_count.property_cache"));

	auto compoundRequest = request(maxEdgeId, followsType);
	compoundRequest.propertyPredicates = {{"weight", PropertyValue(int64_t{7})}, {"kind", PropertyValue("fast")}};
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
	EXPECT_TRUE(snapshot.contains("relationship_count.load_edge_metadata"));
	EXPECT_FALSE(snapshot.contains("relationship_count.property_cache"));
}

TEST_F(RelationshipColumnarCountKernelTest, CountsScalarEqualityPredicatesWithTypedStorageComparisons) {
	const TemporalDate date{12345};
	const TemporalDateTime dateTime{987654321};
	const TemporalDuration duration{2, 3, 4000};
	const auto stored = addRelationshipWithProperties(followsType, {{"active", PropertyValue(true)},
																	{"score", PropertyValue(1.5)},
																	{"kind", PropertyValue("fast")},
																	{"date", PropertyValue(date)},
																	{"date_time", PropertyValue(dateTime)},
																	{"duration", PropertyValue(duration)}});
	addRelationshipWithProperties(followsType, {{"active", PropertyValue(false)},
												{"score", PropertyValue(2.5)},
												{"kind", PropertyValue("slow")},
												{"date", PropertyValue(TemporalDate{12346})},
												{"date_time", PropertyValue(TemporalDateTime{987654322})},
												{"duration", PropertyValue(TemporalDuration{2, 4, 4000})}});
	for (int i = 0; i < 128; ++i) {
		addRelationship(i % 2 == 0 ? followsType : likesType);
	}
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());

	RelationshipColumnarCountKernel kernel(dm);
	auto expectCount = [&](const std::string &key, const PropertyValue &value, int64_t expectedCount) {
		auto countRequest = request(stored.getId() + 129, followsType);
		countRequest.propertyPredicates = {{key, value}};
		const auto result = kernel.count(countRequest);
		ASSERT_TRUE(result.has_value());
		EXPECT_EQ(result->count, expectedCount) << key;
	};

	expectCount("active", PropertyValue(true), 1);
	expectCount("score", PropertyValue(1.5), 1);
	expectCount("kind", PropertyValue("fast"), 1);
	expectCount("date", PropertyValue(date), 1);
	expectCount("date_time", PropertyValue(dateTime), 1);
	expectCount("duration", PropertyValue(duration), 1);
	expectCount("score", PropertyValue(int64_t{1}), 0);
	expectCount("missing", PropertyValue("fast"), 0);
}

TEST_F(RelationshipColumnarCountKernelTest, CountsLargeTypedPropertyPredicatesWithRelationshipCandidates) {
	int64_t maxEdgeId = 0;
	int64_t expectedFollows = 0;
	int64_t expectedFollowsCandidates = 0;
	int64_t expectedAllTypes = 0;
	for (int i = 0; i < 1300; ++i) {
		const bool follows = i % 2 == 0;
		const bool matches = i % 7 == 0;
		maxEdgeId = addRelationshipWithProperties(
							follows ? followsType : likesType,
							{{"weight", PropertyValue(matches ? int64_t{7} : int64_t{3})}})
							.getId();
		if (follows) {
			++expectedFollowsCandidates;
		}
		if (matches) {
			++expectedAllTypes;
			if (follows) {
				++expectedFollows;
			}
		}
	}
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());

	RelationshipColumnarCountKernel kernel(dm);
	auto countRequest = request(maxEdgeId, followsType);
	countRequest.propertyPredicates = {{"weight", PropertyValue(int64_t{7})}};

	graph::debug::PerfTrace::setEnabled(true);
	graph::debug::PerfTrace::reset();
	auto followsMatches = kernel.count(countRequest);
	auto snapshot = graph::debug::PerfTrace::snapshotAndReset();
	ASSERT_TRUE(followsMatches.has_value());
	EXPECT_EQ(followsMatches->count, expectedFollows);
	EXPECT_EQ(followsMatches->propertyCandidates, static_cast<size_t>(expectedFollowsCandidates));
	EXPECT_TRUE(snapshot.contains("relationship_count.load_edge_metadata"));
	EXPECT_TRUE(snapshot.contains("relationship_count.property_predicate"));
	EXPECT_FALSE(snapshot.contains("relationship_count.property_owner_type_filter"));

	countRequest.typeId = 0;
	graph::debug::PerfTrace::reset();
	auto allTypeMatches = kernel.count(countRequest);
	snapshot = graph::debug::PerfTrace::snapshotAndReset();
	ASSERT_TRUE(allTypeMatches.has_value());
	EXPECT_EQ(allTypeMatches->count, expectedAllTypes);
	EXPECT_TRUE(snapshot.contains("relationship_count.property_predicate"));
	EXPECT_FALSE(snapshot.contains("relationship_count.property_owner_scan"));
	EXPECT_FALSE(snapshot.contains("relationship_count.property_owner_type_filter"));
	EXPECT_FALSE(snapshot.contains("relationship_count.load_edge_metadata"));
}

TEST_F(RelationshipColumnarCountKernelTest, TypedOwnerScanFiltersLargeFallbackCandidatesByRelationshipType) {
	int64_t maxEdgeId = 0;
	int64_t expectedFollows = 0;
	Edge firstMatchingFollows;
	for (int i = 0; i < 1300; ++i) {
		const bool follows = i % 2 == 0;
		const bool matches = i % 7 == 0;
		Edge edge = addRelationshipWithProperties(
				follows ? followsType : likesType,
				{{"weight", PropertyValue(matches ? int64_t{7} : int64_t{3})}});
		maxEdgeId = edge.getId();
		if (matches && follows) {
			++expectedFollows;
			if (firstMatchingFollows.getId() == 0) {
				firstMatchingFollows = edge;
			}
		}
	}

	ASSERT_NE(firstMatchingFollows.getId(), 0);
	firstMatchingFollows.setPropertyEntityId(
			firstMatchingFollows.getPropertyEntityId() + 1000000, PropertyStorageType::PROPERTY_ENTITY);
	dm->updateEdge(firstMatchingFollows);
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());

	RelationshipColumnarCountKernel kernel(dm);
	auto countRequest = request(maxEdgeId, followsType);
	countRequest.propertyPredicates = {{"weight", PropertyValue(int64_t{7})}};

	graph::debug::PerfTrace::setEnabled(true);
	graph::debug::PerfTrace::reset();
	auto followsMatches = kernel.count(countRequest);
	auto snapshot = graph::debug::PerfTrace::snapshotAndReset();

	ASSERT_TRUE(followsMatches.has_value());
	EXPECT_EQ(followsMatches->count, expectedFollows);
	EXPECT_TRUE(snapshot.contains("relationship_count.property_owner_scan"));
	EXPECT_TRUE(snapshot.contains("relationship_count.property_owner_type_filter"));
}

TEST_F(RelationshipColumnarCountKernelTest, PropertyPredicateRejectsInvalidOwnerRange) {
	auto edge = addRelationshipWithProperties(followsType, {{"weight", PropertyValue(int64_t{7})}});
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());

	RelationshipColumnarCountKernel kernel(dm);
	auto invalidRange = request(edge.getId(), 0);
	invalidRange.beginId = edge.getId() + 1;
	invalidRange.endId = edge.getId();
	invalidRange.propertyPredicates = {{"weight", PropertyValue(int64_t{7})}};

	EXPECT_FALSE(kernel.count(invalidRange).has_value());
}

TEST_F(RelationshipColumnarCountKernelTest, CountsAllTypesWithBlobFallbackAfterMetadataCandidateScan) {
	int64_t maxEdgeId = 0;
	maxEdgeId = addRelationshipWithProperties(followsType, {{"weight", PropertyValue(int64_t{7})}}).getId();
	const std::string matchingPayload(512, 'm');
	maxEdgeId = addRelationshipWithProperties(
			likesType, {{"weight", PropertyValue(int64_t{7})}, {"payload", PropertyValue(matchingPayload)}})
					.getId();
	const std::string nonMatchingPayload(512, 'n');
	maxEdgeId = addRelationshipWithProperties(
			likesType, {{"weight", PropertyValue(int64_t{3})}, {"payload", PropertyValue(nonMatchingPayload)}})
					.getId();
	for (int i = 0; i < 125; ++i) {
		maxEdgeId = addRelationship(i % 2 == 0 ? followsType : likesType).getId();
	}
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());

	RelationshipColumnarCountKernel kernel(dm);
	auto countRequest = request(maxEdgeId, 0);
	countRequest.propertyPredicates = {{"weight", PropertyValue(int64_t{7})}};

	const auto result = kernel.count(countRequest);
	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->count, 2);
	EXPECT_EQ(result->propertyCandidates, 1U);
	EXPECT_EQ(result->fallbackEdges, 2U);
}

TEST_F(RelationshipColumnarCountKernelTest, ReturnsNulloptWhenLargeDirtyPropertyOwnerScanIsUnsafe) {
	int64_t maxEdgeId = 0;
	for (int i = 0; i < 1024; ++i) {
		maxEdgeId = addRelationshipWithProperties(followsType, {{"weight", PropertyValue(int64_t{7})}}).getId();
	}
	ASSERT_TRUE(dm->hasUnsavedChanges());

	RelationshipColumnarCountKernel kernel(dm);
	auto dirtyRequest = request(maxEdgeId, 0);
	dirtyRequest.propertyPredicates = {{"weight", PropertyValue(int64_t{7})}};

	EXPECT_FALSE(kernel.count(dirtyRequest).has_value());
}

TEST_F(RelationshipColumnarCountKernelTest, CountsVectorComparisonPredicatesWithColumnarFallback) {
	int64_t maxEdgeId = 0;
	maxEdgeId = addRelationshipWithProperties(followsType, {{"weight", PropertyValue(int64_t{2})}}).getId();
	maxEdgeId = addRelationshipWithProperties(followsType, {{"weight", PropertyValue(int64_t{5})}}).getId();
	maxEdgeId = addRelationshipWithProperties(followsType, {{"weight", PropertyValue(int64_t{8})}}).getId();
	maxEdgeId = addRelationshipWithProperties(followsType, {{"weight", PropertyValue(int64_t{11})}}).getId();
	maxEdgeId = addRelationshipWithProperties(followsType, {{"created", PropertyValue(TemporalDate{20})}}).getId();
	maxEdgeId = addRelationshipWithProperties(followsType, {{"created", PropertyValue(TemporalDate{30})}}).getId();
	maxEdgeId =
			addRelationshipWithProperties(followsType, {{"elapsed", PropertyValue(TemporalDuration{0, 2, 0})}})
					.getId();
	addRelationshipWithProperties(likesType, {{"weight", PropertyValue(int64_t{5})}});
	for (int i = 0; i < 128; ++i) {
		maxEdgeId = addRelationship(i % 2 == 0 ? followsType : likesType).getId();
	}
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());

	VectorizedPropertyPredicate lower;
	lower.propertyKey = "weight";
	lower.op = VectorPredicateOp::VPO_GE;
	lower.value = PropertyValue(int64_t{5});
	VectorizedPropertyPredicate upper;
	upper.propertyKey = "weight";
	upper.op = VectorPredicateOp::VPO_LT;
	upper.value = PropertyValue(int64_t{10});

	auto countRequest = request(maxEdgeId, followsType);
	countRequest.vectorPredicates = {lower, upper};
	RelationshipColumnarCountKernel kernel(dm);
	const auto result = kernel.count(countRequest);

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->count, 2);
	EXPECT_EQ(result->propertyCandidates, 7U);

	VectorizedPropertyPredicate createdLower;
	createdLower.propertyKey = "created";
	createdLower.op = VectorPredicateOp::VPO_RANGE_CLOSED;
	createdLower.value = PropertyValue(TemporalDate{10});
	createdLower.upperValue = PropertyValue(TemporalDate{25});
	countRequest.vectorPredicates = {createdLower};
	const auto dateResult = kernel.count(countRequest);
	ASSERT_TRUE(dateResult.has_value());
	EXPECT_EQ(dateResult->count, 1);

	VectorizedPropertyPredicate durationLower;
	durationLower.propertyKey = "elapsed";
	durationLower.op = VectorPredicateOp::VPO_GE;
	durationLower.value = PropertyValue(TemporalDuration{0, 1, 0});
	countRequest.vectorPredicates = {durationLower};
	const auto durationResult = kernel.count(countRequest);
	ASSERT_TRUE(durationResult.has_value());
	EXPECT_EQ(durationResult->count, 1);
}

TEST_F(RelationshipColumnarCountKernelTest, CountsGroupedVectorPredicatesByKey) {
	int64_t maxEdgeId = 0;
	maxEdgeId = addRelationshipWithProperties(followsType,
											  {{"weight", PropertyValue(int64_t{5})}, {"kind", PropertyValue("b")}})
						.getId();
	maxEdgeId = addRelationshipWithProperties(followsType,
											  {{"weight", PropertyValue(int64_t{8})}, {"kind", PropertyValue("c")}})
						.getId();
	maxEdgeId = addRelationshipWithProperties(followsType,
											  {{"weight", PropertyValue(int64_t{11})}, {"kind", PropertyValue("d")}})
						.getId();
	maxEdgeId =
			addRelationshipWithProperties(followsType, {{"weight", PropertyValue("8")}, {"kind", PropertyValue("c")}})
					.getId();
	for (int i = 0; i < 128; ++i) {
		maxEdgeId = addRelationship(i % 2 == 0 ? followsType : likesType).getId();
	}
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());

	VectorizedPropertyPredicate lower;
	lower.propertyKey = "weight";
	lower.op = VectorPredicateOp::VPO_GE;
	lower.value = PropertyValue(int64_t{5});
	VectorizedPropertyPredicate upper;
	upper.propertyKey = "weight";
	upper.op = VectorPredicateOp::VPO_LE;
	upper.value = PropertyValue(int64_t{8});
	VectorizedPropertyPredicate missing;
	missing.propertyKey = "score";
	missing.op = VectorPredicateOp::VPO_EQ;
	missing.value = PropertyValue(int64_t{1});
	VectorizedPropertyPredicate kindLower;
	kindLower.propertyKey = "kind";
	kindLower.op = VectorPredicateOp::VPO_GE;
	kindLower.value = PropertyValue("b");
	VectorizedPropertyPredicate kindUpper;
	kindUpper.propertyKey = "kind";
	kindUpper.op = VectorPredicateOp::VPO_LE;
	kindUpper.value = PropertyValue("c");

	RelationshipColumnarCountKernel kernel(dm);
	auto countRequest = request(maxEdgeId, followsType);
	countRequest.vectorPredicates = {lower, upper};
	auto numericRange = kernel.count(countRequest);
	ASSERT_TRUE(numericRange.has_value());
	EXPECT_EQ(numericRange->count, 2);

	countRequest.vectorPredicates = {lower, upper, missing};
	auto missingKey = kernel.count(countRequest);
	ASSERT_TRUE(missingKey.has_value());
	EXPECT_EQ(missingKey->count, 0);

	countRequest.vectorPredicates = {kindLower, kindUpper};
	auto stringRange = kernel.count(countRequest);
	ASSERT_TRUE(stringRange.has_value());
	EXPECT_EQ(stringRange->count, 3);
	EXPECT_EQ(stringRange->propertyCandidates, 4U);
}

TEST_F(RelationshipColumnarCountKernelTest, CountsVectorEqualityPredicatesViaEqualityMap) {
	int64_t maxEdgeId = 0;
	maxEdgeId = addRelationshipWithProperties(followsType,
	                                         {{"weight", PropertyValue(int64_t{7})}, {"kind", PropertyValue("fast")}})
	                    .getId();
	maxEdgeId = addRelationshipWithProperties(followsType,
	                                         {{"weight", PropertyValue(int64_t{7})}, {"kind", PropertyValue("slow")}})
	                    .getId();
	for (int i = 0; i < 128; ++i) {
		maxEdgeId = addRelationship(i % 2 == 0 ? followsType : likesType).getId();
	}
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());

	VectorizedPropertyPredicate weight;
	weight.propertyKey = "weight";
	weight.op = VectorPredicateOp::VPO_EQ;
	weight.value = PropertyValue(int64_t{7});
	VectorizedPropertyPredicate kind;
	kind.propertyKey = "kind";
	kind.op = VectorPredicateOp::VPO_EQ;
	kind.value = PropertyValue("fast");

	RelationshipColumnarCountRequest countRequest = request(maxEdgeId, followsType);
	countRequest.vectorPredicates = {weight, kind};
	RelationshipColumnarCountKernel kernel(dm);
	const auto result = kernel.count(countRequest);

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->count, 1);
	EXPECT_EQ(result->propertyCandidates, 2U);
}
