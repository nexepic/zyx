#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <variant>

#include "graph/core/Database.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/query/execution/VectorizedPredicate.hpp"
#include "graph/query/execution/operators/RelationshipCountScanOperator.hpp"
#include "graph/storage/SegmentIndexManager.hpp"
#include "graph/storage/StorageHeaders.hpp"

namespace fs = std::filesystem;
using namespace graph;
using namespace graph::query::execution;
using namespace graph::query::execution::operators;

namespace {
void writePropertyActiveFlag(const fs::path &dbPath, uint64_t segmentOffset, int64_t segmentStartId,
                             int64_t propertyId, bool active) {
	constexpr std::streamoff kIsActiveOffset =
		static_cast<std::streamoff>(sizeof(int64_t) + sizeof(int64_t) + sizeof(uint32_t));
	const std::streamoff slot = static_cast<std::streamoff>(propertyId - segmentStartId);
	const std::streamoff entityOffset = static_cast<std::streamoff>(segmentOffset + sizeof(storage::SegmentHeader)) +
		slot * static_cast<std::streamoff>(Property::getTotalSize());
	const std::streamoff flagOffset = entityOffset + kIsActiveOffset;

	std::fstream io(dbPath, std::ios::binary | std::ios::in | std::ios::out);
	if (!io.is_open()) {
		throw std::runtime_error("Failed to open database file for property flag update");
	}

	const char flag = active ? 1 : 0;
	io.seekp(flagOffset);
	io.write(&flag, sizeof(flag));
	io.flush();
}
} // namespace

class RelationshipCountScanOperatorTest : public ::testing::Test {
protected:
	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		testDbPath = fs::temp_directory_path() / ("test_relationship_count_scan_path_" + boost::uuids::to_string(uuid) + ".zyx");
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

	void addFollows(int64_t source, int64_t target, int64_t weight) {
		Edge edge(0, source, target, followsType);
		dm->addEdge(edge);
		dm->addEdgeProperties(edge.getId(), {{"weight", PropertyValue(weight)}});
	}

	void addFollowsWithProperties(int64_t source,
	                              int64_t target,
	                              const std::unordered_map<std::string, PropertyValue> &properties) {
		Edge edge(0, source, target, followsType);
		dm->addEdge(edge);
		dm->addEdgeProperties(edge.getId(), properties);
	}

	void addLikesWithProperties(int64_t source,
	                            int64_t target,
	                            const std::unordered_map<std::string, PropertyValue> &properties) {
		Edge edge(0, source, target, dm->getOrCreateTokenId("LIKES"));
		dm->addEdge(edge);
		dm->addEdgeProperties(edge.getId(), properties);
	}

	int64_t addFollowsAndReturnPropertyEntity(int64_t source, int64_t target, int64_t weight) {
		Edge edge(0, source, target, followsType);
		dm->addEdge(edge);
		dm->addEdgeProperties(edge.getId(), {{"weight", PropertyValue(weight)}});
		const Edge stored = dm->getEdge(edge.getId());
		return stored.getPropertyEntityId();
	}

	void addLikes(int64_t source, int64_t target) {
		Edge edge(0, source, target, dm->getOrCreateTokenId("LIKES"));
		dm->addEdge(edge);
	}

	static int64_t readCount(const std::optional<RecordBatch> &batch, const std::string &alias = "count") {
		EXPECT_TRUE(batch.has_value());
		if (!batch.has_value()) {
			return -1;
		}
		EXPECT_EQ(batch->size(), 1U);
		const auto value = batch->front().getValue(alias);
		EXPECT_TRUE(value.has_value());
		if (!value.has_value()) {
			return -1;
		}
		return std::get<int64_t>(value->getVariant());
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

TEST_F(RelationshipCountScanOperatorTest, CountsOneHopExpand) {
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

	RelationshipCountScanOperator op(dm, im, seedConfig, seedRequirements, {hop("u", "r", "v")}, "count");
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

TEST_F(RelationshipCountScanOperatorTest, CountsTwoHopPathMultiplicity) {
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

	RelationshipCountScanOperator op(dm, im, seedConfig, seedRequirements, hops, "count");
	op.open();
	const auto batch = op.next();
	op.close();

	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 1U);
	const auto value = batch->front().getValue("count");
	ASSERT_TRUE(value.has_value());
	EXPECT_EQ(value.value(), PropertyValue(int64_t{2}));
}

TEST_F(RelationshipCountScanOperatorTest, TwoHopCountSkipsRejectedIntermediateRows) {
	const int64_t source = addUser();
	const int64_t matchingMid = addUser();
	const int64_t wrongMid = addPost();
	const int64_t target = addUser();
	addFollows(source, matchingMid);
	addFollows(source, wrongMid);
	addFollows(matchingMid, target);
	addFollows(wrongMid, target);

	NodeScanConfig seedConfig;
	seedConfig.type = ScanType::FULL_SCAN;
	seedConfig.variable = "u";
	NodeScanRequirements seedRequirements;
	seedRequirements.materialization = NodeMaterializationMode::NSM_ID_ONLY;
	seedRequirements.countOnly = true;

	std::vector<RelationshipExpandConfig> hops = {hop("u", "r1", "m"), hop("m", "r2", "v")};
	RelationshipCountScanOperator op(dm, im, seedConfig, seedRequirements, hops, "count");
	op.open();
	EXPECT_EQ(readCount(op.next()), 1);
	op.close();
}

TEST_F(RelationshipCountScanOperatorTest, TwoHopCountStopsWhenFirstHopHasNoSelectedTargets) {
	const int64_t source = addUser();
	const int64_t wrongMid = addPost();
	const int64_t target = addUser();
	addFollows(source, wrongMid);
	addFollows(wrongMid, target);

	NodeScanConfig seedConfig;
	seedConfig.type = ScanType::FULL_SCAN;
	seedConfig.variable = "u";
	NodeScanRequirements seedRequirements;
	seedRequirements.materialization = NodeMaterializationMode::NSM_ID_ONLY;
	seedRequirements.countOnly = true;

	std::vector<RelationshipExpandConfig> hops = {hop("u", "r1", "m"), hop("m", "r2", "v")};
	RelationshipCountScanOperator op(dm, im, seedConfig, seedRequirements, hops, "count");
	op.open();
	EXPECT_EQ(readCount(op.next()), 0);
	op.close();
}

TEST_F(RelationshipCountScanOperatorTest, EmitsProfilePhases) {
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

	RelationshipCountScanOperator op(dm, im, seedConfig, seedRequirements, {hop("u", "r", "v")}, "count");
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


TEST_F(RelationshipCountScanOperatorTest, ResolvesHopTokensAtOpen) {
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

	RelationshipCountScanOperator op(dm, im, seedConfig, seedRequirements, {unresolvedHop}, "count");
	op.open();
	const auto batch = op.next();
	op.close();

	ASSERT_TRUE(batch.has_value());
	const auto value = batch->front().getValue("count");
	ASSERT_TRUE(value.has_value());
	EXPECT_EQ(value.value(), PropertyValue(int64_t{1}));
}

TEST_F(RelationshipCountScanOperatorTest, CountsFromSeedPropertyScan) {
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

	RelationshipCountScanOperator op(dm, im, seedConfig, seedRequirements, {seedPredicate}, {hop("u", "r", "v")}, "count");
	op.open();
	const auto batch = op.next();
	op.close();

	ASSERT_TRUE(batch.has_value());
	const auto value = batch->front().getValue("count");
	ASSERT_TRUE(value.has_value());
	EXPECT_EQ(value.value(), PropertyValue(int64_t{1}));
}

TEST_F(RelationshipCountScanOperatorTest, SeedPredicateFilteringSkipsUnselectedSeeds) {
	const int64_t matching = addUser();
	const int64_t mismatched = addUser();
	const int64_t target = addUser();
	dm->addNodeProperties(matching, {{"id", PropertyValue("match")}});
	dm->addNodeProperties(mismatched, {{"id", PropertyValue("skip")}});
	addFollows(matching, target);
	addFollows(mismatched, target);

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
	seedPredicate.value = PropertyValue("match");

	RelationshipCountScanOperator op(dm, im, seedConfig, seedRequirements, {seedPredicate}, {hop("u", "r", "v")}, "count");
	op.open();
	EXPECT_EQ(readCount(op.next()), 1);
	op.close();
}

TEST_F(RelationshipCountScanOperatorTest, NonIdOnlySeedRequirementsUseBatchFilteringWithoutPredicates) {
	const int64_t source = addUser();
	const int64_t other = addUser();
	const int64_t target = addUser();
	dm->addNodeProperties(source, {{"id", PropertyValue("match")}});
	dm->addNodeProperties(other, {{"id", PropertyValue("other")}});
	addFollows(source, target);
	addFollows(other, target);

	NodeScanConfig seedConfig;
	seedConfig.type = ScanType::FULL_SCAN;
	seedConfig.variable = "u";
	seedConfig.labels = {"User"};
	NodeScanRequirements seedRequirements;
	seedRequirements.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	seedRequirements.requiredProperties = {"id"};
	seedRequirements.countOnly = true;

	RelationshipCountScanOperator op(dm, im, seedConfig, seedRequirements, {}, {hop("u", "r", "v")}, "count");
	op.open();
	EXPECT_EQ(readCount(op.next()), 2);
	op.close();
}

TEST_F(RelationshipCountScanOperatorTest, SeedScanCanSkipActiveAndLabelChecksWhenPlannerMarksThemUnneeded) {
	const int64_t source = addUser();
	const int64_t target = addUser();
	addFollows(source, target);

	NodeScanConfig seedConfig;
	seedConfig.type = ScanType::FULL_SCAN;
	seedConfig.variable = "u";
	NodeScanRequirements seedRequirements;
	seedRequirements.materialization = NodeMaterializationMode::NSM_ID_ONLY;
	seedRequirements.countOnly = true;
	seedRequirements.needsActiveCheck = false;
	seedRequirements.needsLabels = false;

	RelationshipCountScanOperator op(dm, im, seedConfig, seedRequirements, {}, {hop("u", "r", "v")}, "count");
	op.open();
	EXPECT_EQ(readCount(op.next()), 1);
	op.close();
}

TEST_F(RelationshipCountScanOperatorTest, SeedLabelScanFallsBackToBatchFilteringWhenLabelsAreResidual) {
	const int64_t source = addUser();
	const int64_t target = addUser();
	addFollows(source, target);
	ASSERT_TRUE(im->createIndex("seed_label_residual_idx", "node", "", ""));

	NodeScanConfig seedConfig;
	seedConfig.type = ScanType::LABEL_SCAN;
	seedConfig.variable = "u";
	seedConfig.labels = {"User", "MissingResidualLabel"};
	NodeScanRequirements seedRequirements;
	seedRequirements.materialization = NodeMaterializationMode::NSM_ID_ONLY;
	seedRequirements.countOnly = true;
	seedRequirements.needsLabels = true;

	RelationshipCountScanOperator op(dm, im, seedConfig, seedRequirements, {}, {hop("u", "r", "v")}, "count");
	op.open();
	EXPECT_EQ(readCount(op.next()), 0);
	op.close();
}

TEST_F(RelationshipCountScanOperatorTest, CountsDirectRelationshipsWithPropertyFilter) {
	const int64_t source = addUser();
	const int64_t first = addUser();
	const int64_t second = addUser();
	const int64_t third = addUser();
	addFollows(source, first, 1);
	addFollows(source, second, 2);
	addFollows(second, third, 1);
	addLikes(source, third);

	NodeScanConfig seedConfig;
	NodeScanRequirements seedRequirements;
	DirectRelationshipCountConfig directCount;
	directCount.enabled = true;
	directCount.edgeType = "FOLLOWS";
	directCount.edgeProperties = {{"weight", PropertyValue(int64_t{1})}};
	debug::PerfTrace::setEnabled(true);
	debug::PerfTrace::reset();

	RelationshipCountScanOperator op(dm, im, seedConfig, seedRequirements, {}, {}, directCount, "count");
	op.open();
	const auto batch = op.next();
	op.close();

	ASSERT_TRUE(batch.has_value());
	const auto value = batch->front().getValue("count");
	ASSERT_TRUE(value.has_value());
	EXPECT_EQ(value.value(), PropertyValue(int64_t{2}));
	const auto snapshot = debug::PerfTrace::snapshotAndReset();
	EXPECT_TRUE(snapshot.contains("relationship_count.direct_scan"));
	EXPECT_TRUE(snapshot.contains("relationship_count.property_columns"));
	EXPECT_TRUE(snapshot.contains("relationship_count.extract_property_columns"));
}

TEST_F(RelationshipCountScanOperatorTest, CleanDirectRelationshipCountWithoutPropertiesUsesPlannedTypeIndex) {
	const int64_t source = addUser();
	const int64_t first = addUser();
	const int64_t second = addUser();
	for (int i = 0; i < 128; ++i) {
		addFollows(source, (i % 2 == 0) ? first : second);
	}
	addLikes(source, first);
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());
	ASSERT_TRUE(im->createIndex("edge_type_metadata_preferred_idx", "edge", "", ""));

	NodeScanConfig seedConfig;
	NodeScanRequirements seedRequirements;
	DirectRelationshipCountConfig directCount;
	directCount.enabled = true;
	directCount.edgeType = "FOLLOWS";
	directCount.candidateSource.type = DirectRelationshipCandidateSourceType::DRCS_TYPE_INDEX;
	debug::PerfTrace::setEnabled(true);
	debug::PerfTrace::reset();

	RelationshipCountScanOperator op(dm, im, seedConfig, seedRequirements, {}, {}, directCount, "count");
	op.open();
	const auto batch = op.next();
	op.close();

	ASSERT_TRUE(batch.has_value());
	const auto value = batch->front().getValue("count");
	ASSERT_TRUE(value.has_value());
	EXPECT_EQ(value.value(), PropertyValue(int64_t{128}));
	const auto snapshot = debug::PerfTrace::snapshotAndReset();
	EXPECT_TRUE(snapshot.contains("relationship_count.index_count"));
	EXPECT_FALSE(snapshot.contains("relationship_count.load_edge_metadata"));
	EXPECT_FALSE(snapshot.contains("relationship_count.index_candidates"));
	EXPECT_FALSE(snapshot.contains("relationship_count.property_predicate"));
	EXPECT_FALSE(snapshot.contains("relationship_count.property_columns"));
}

TEST_F(RelationshipCountScanOperatorTest, PlannedTypeIndexWithoutUsableIndexFallsBackToDirectScan) {
	const int64_t source = addUser();
	const int64_t target = addUser();
	addFollows(source, target);

	NodeScanConfig seedConfig;
	NodeScanRequirements seedRequirements;
	DirectRelationshipCountConfig directCount;
	directCount.enabled = true;
	directCount.edgeType = "FOLLOWS";
	directCount.candidateSource.type = DirectRelationshipCandidateSourceType::DRCS_TYPE_INDEX;
	debug::PerfTrace::setEnabled(true);
	debug::PerfTrace::reset();

	RelationshipCountScanOperator op(dm, im, seedConfig, seedRequirements, {}, {}, directCount, "count");
	op.open();
	EXPECT_EQ(readCount(op.next()), 1);
	op.close();

	const auto snapshot = debug::PerfTrace::snapshotAndReset();
	EXPECT_FALSE(snapshot.contains("relationship_count.index_count"));
	EXPECT_TRUE(snapshot.contains("relationship_count.direct_scan"));
}

TEST_F(RelationshipCountScanOperatorTest, CleanDirectRelationshipCountUsesMetadataColumns) {
	const int64_t source = addUser();
	const int64_t first = addUser();
	const int64_t second = addUser();
	for (int i = 0; i < 128; ++i) {
		addFollows(source, (i % 2 == 0) ? first : second, i == 0 ? 1 : 2);
	}
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());

	NodeScanConfig seedConfig;
	NodeScanRequirements seedRequirements;
	DirectRelationshipCountConfig directCount;
	directCount.enabled = true;
	directCount.edgeType = "FOLLOWS";
	directCount.edgeProperties = {{"weight", PropertyValue(int64_t{1})}};
	debug::PerfTrace::setEnabled(true);
	debug::PerfTrace::reset();

	RelationshipCountScanOperator op(dm, im, seedConfig, seedRequirements, {}, {}, directCount, "count");
	op.open();
	const auto batch = op.next();
	op.close();

	ASSERT_TRUE(batch.has_value());
	const auto value = batch->front().getValue("count");
	ASSERT_TRUE(value.has_value());
	EXPECT_EQ(value.value(), PropertyValue(int64_t{1}));
	const auto snapshot = debug::PerfTrace::snapshotAndReset();
	EXPECT_TRUE(snapshot.contains("relationship_count.load_edge_metadata"));
	EXPECT_TRUE(snapshot.contains("relationship_count.property_predicate"));
	EXPECT_FALSE(snapshot.contains("relationship_count.property_columns"));
}

TEST_F(RelationshipCountScanOperatorTest, CleanDirectRelationshipCountTreatsInactivePropertyEntityAsNoMatch) {
	const int64_t source = addUser();
	const int64_t target = addUser();
	int64_t inactivePropertyId = 0;
	for (int i = 0; i < 128; ++i) {
		const int64_t propertyId = addFollowsAndReturnPropertyEntity(source, target, i == 0 ? 1 : 2);
		if (i == 0) {
			inactivePropertyId = propertyId;
		}
	}
	ASSERT_NE(inactivePropertyId, 0);
	db->getStorage()->flush();

	const auto &segIndex = dm->getSegmentIndexManager()->getPropertySegmentIndex();
	auto segIt = std::find_if(segIndex.begin(), segIndex.end(), [inactivePropertyId](const auto &seg) {
		return inactivePropertyId >= seg.startId && inactivePropertyId <= seg.endId;
	});
	ASSERT_NE(segIt, segIndex.end());
	const uint64_t segmentOffset = segIt->segmentOffset;
	const int64_t segmentStartId = segIt->startId;

	db->close();
	db.reset();
	writePropertyActiveFlag(testDbPath, segmentOffset, segmentStartId, inactivePropertyId, false);
	db = std::make_unique<Database>(testDbPath.string());
	db->open();
	dm = db->getStorage()->getDataManager();
	im = db->getQueryEngine()->getIndexManager();

	NodeScanConfig seedConfig;
	NodeScanRequirements seedRequirements;
	DirectRelationshipCountConfig directCount;
	directCount.enabled = true;
	directCount.edgeType = "FOLLOWS";
	directCount.edgeProperties = {{"weight", PropertyValue(int64_t{1})}};

	RelationshipCountScanOperator op(dm, im, seedConfig, seedRequirements, {}, {}, directCount, "count");
	op.open();
	EXPECT_EQ(readCount(op.next()), 0);
	op.close();
}

TEST_F(RelationshipCountScanOperatorTest, DirectCountReturnsZeroForMissingManagerOrUnknownType) {
	NodeScanConfig seedConfig;
	NodeScanRequirements seedRequirements;
	DirectRelationshipCountConfig directCount;
	directCount.enabled = true;
	directCount.edgeType = "FOLLOWS";

	RelationshipCountScanOperator nullOp(nullptr, nullptr, seedConfig, seedRequirements, {}, {}, directCount, "count");
	nullOp.open();
	EXPECT_EQ(readCount(nullOp.next()), 0);

	const int64_t source = addUser();
	const int64_t target = addUser();
	addFollows(source, target);
	directCount.edgeType = "MISSING";
	RelationshipCountScanOperator missingType(dm, im, seedConfig, seedRequirements, {}, {}, directCount, "count");
	missingType.open();
	EXPECT_EQ(readCount(missingType.next()), 0);
}

TEST_F(RelationshipCountScanOperatorTest, UnknownHopTokensAndEmptyExpandInputsReturnZero) {
	const int64_t source = addUser();
	const int64_t target = addUser();
	addFollows(source, target);

	NodeScanConfig seedConfig;
	seedConfig.type = ScanType::FULL_SCAN;
	seedConfig.variable = "u";
	NodeScanRequirements seedRequirements;
	seedRequirements.materialization = NodeMaterializationMode::NSM_ID_ONLY;
	seedRequirements.countOnly = true;

	RelationshipExpandConfig missingType = hop("u", "r", "v");
	missingType.edgeType = "MISSING";
	missingType.edgeTypeId = 0;
	RelationshipCountScanOperator missingTypeOp(dm, im, seedConfig, seedRequirements, {missingType}, "count");
	missingTypeOp.open();
	EXPECT_EQ(readCount(missingTypeOp.next()), 0);

	RelationshipExpandConfig missingLabel = hop("u", "r", "v");
	missingLabel.targetLabels = {"MissingLabel"};
	missingLabel.targetLabelIds.clear();
	RelationshipCountScanOperator missingLabelOp(dm, im, seedConfig, seedRequirements, {missingLabel}, "count");
	missingLabelOp.open();
	EXPECT_EQ(readCount(missingLabelOp.next()), 0);

	NodeScanConfig emptySeed;
	emptySeed.type = ScanType::LABEL_SCAN;
	emptySeed.variable = "u";
	emptySeed.labels = {"MissingLabel"};
	RelationshipCountScanOperator emptySeedOp(dm, im, emptySeed, seedRequirements, {hop("u", "r", "v")}, "count");
	emptySeedOp.open();
	EXPECT_EQ(readCount(emptySeedOp.next()), 0);

	RelationshipCountScanOperator emptyHopsOp(dm, im, seedConfig, seedRequirements, {}, "count");
	emptyHopsOp.open();
	EXPECT_EQ(readCount(emptyHopsOp.next()), 0);
}

TEST_F(RelationshipCountScanOperatorTest, TwoHopMissingFirstHopStopsAfterEmptyFrontier) {
	const int64_t source = addUser();
	const int64_t middle = addUser();
	const int64_t target = addUser();
	addFollows(source, middle);
	addFollows(middle, target);

	NodeScanConfig seedConfig;
	seedConfig.type = ScanType::FULL_SCAN;
	seedConfig.variable = "u";
	NodeScanRequirements seedRequirements;
	seedRequirements.materialization = NodeMaterializationMode::NSM_ID_ONLY;
	seedRequirements.countOnly = true;

	RelationshipExpandConfig missingFirstHop = hop("u", "r1", "m");
	missingFirstHop.edgeType = "MISSING";
	missingFirstHop.edgeTypeId = 0;
	std::vector<RelationshipExpandConfig> hops = {missingFirstHop, hop("m", "r2", "v")};

	RelationshipCountScanOperator op(dm, im, seedConfig, seedRequirements, hops, "count");
	op.open();
	EXPECT_EQ(readCount(op.next()), 0);
	op.close();
}

TEST_F(RelationshipCountScanOperatorTest, DirectFallbackSkipsInactiveAndEmptyPropertyCandidates) {
	const int64_t source = addUser();
	const int64_t first = addUser();
	const int64_t second = addUser();
	Edge active(0, source, first, followsType);
	dm->addEdge(active);
	Edge inactive(0, source, second, followsType);
	dm->addEdge(inactive);
	dm->deleteEdge(inactive);

	NodeScanConfig seedConfig;
	NodeScanRequirements seedRequirements;
	DirectRelationshipCountConfig directCount;
	directCount.enabled = true;
	directCount.edgeType = "FOLLOWS";

	RelationshipCountScanOperator directOp(dm, im, seedConfig, seedRequirements, {}, {}, directCount, "count");
	directOp.open();
	EXPECT_EQ(readCount(directOp.next()), 1);

	directCount.edgeProperties = {{"weight", PropertyValue(int64_t{99})}};
	RelationshipCountScanOperator noCandidateOp(dm, im, seedConfig, seedRequirements, {}, {}, directCount, "count");
	noCandidateOp.open();
	EXPECT_EQ(readCount(noCandidateOp.next()), 0);
}

TEST_F(RelationshipCountScanOperatorTest, DirectCountFallsBackToScanWhenIndexManagerIsUnavailable) {
	const int64_t source = addUser();
	const int64_t first = addUser();
	const int64_t second = addUser();
	addFollowsWithProperties(source, first, {{"weight", PropertyValue(int64_t{1})}});
	addFollowsWithProperties(source, second, {{"weight", PropertyValue(int64_t{2})}});

	NodeScanConfig seedConfig;
	NodeScanRequirements seedRequirements;
	DirectRelationshipCountConfig directCount;
	directCount.enabled = true;
	directCount.edgeProperties = {{"weight", PropertyValue(int64_t{1})}};
	debug::PerfTrace::setEnabled(true);
	debug::PerfTrace::reset();

	RelationshipCountScanOperator op(dm, nullptr, seedConfig, seedRequirements, {}, {}, directCount, "count");
	op.open();
	EXPECT_EQ(readCount(op.next()), 1);
	op.close();

	const auto snapshot = debug::PerfTrace::snapshotAndReset();
	EXPECT_TRUE(snapshot.contains("relationship_count.direct_scan"));
	EXPECT_FALSE(snapshot.contains("relationship_count.index_candidates"));
}

TEST_F(RelationshipCountScanOperatorTest, DirectPropertyFallbackSkipsWhenTypeFilterRejectsEveryEdge) {
	const int64_t source = addUser();
	const int64_t target = addUser();
	addLikes(source, target);

	NodeScanConfig seedConfig;
	NodeScanRequirements seedRequirements;
	DirectRelationshipCountConfig directCount;
	directCount.enabled = true;
	directCount.edgeType = "FOLLOWS";
	directCount.edgeProperties = {{"weight", PropertyValue(int64_t{1})}};

	RelationshipCountScanOperator op(dm, im, seedConfig, seedRequirements, {}, {}, directCount, "count");
	op.open();
	EXPECT_EQ(readCount(op.next()), 0);
	op.close();
}

TEST_F(RelationshipCountScanOperatorTest, DirectPropertyFallbackRejectsMissingAndMismatchedColumns) {
	const int64_t source = addUser();
	const int64_t first = addUser();
	const int64_t second = addUser();
	addFollows(source, first, 5);
	addFollows(source, second, 7);

	NodeScanConfig seedConfig;
	NodeScanRequirements seedRequirements;
	DirectRelationshipCountConfig directCount;
	directCount.enabled = true;
	directCount.edgeType = "FOLLOWS";
	directCount.edgeProperties = {
		{"weight", PropertyValue(int64_t{5})},
		{"missing", PropertyValue(int64_t{1})},
	};

	RelationshipCountScanOperator missingPropertyOp(dm, im, seedConfig, seedRequirements, {}, {}, directCount, "count");
	missingPropertyOp.open();
	EXPECT_EQ(readCount(missingPropertyOp.next()), 0);

	directCount.edgeProperties = {{"weight", PropertyValue(int64_t{6})}};
	RelationshipCountScanOperator mismatchOp(dm, im, seedConfig, seedRequirements, {}, {}, directCount, "count");
	mismatchOp.open();
	EXPECT_EQ(readCount(mismatchOp.next()), 0);
}

TEST_F(RelationshipCountScanOperatorTest, DirectPropertyFallbackAppliesVectorComparisonPredicates) {
	const int64_t source = addUser();
	const int64_t first = addUser();
	const int64_t second = addUser();
	const int64_t third = addUser();
	addFollows(source, first, 2);
	addFollows(source, second, 5);
	addFollows(source, third, 8);

	VectorizedPropertyPredicate lower;
	lower.propertyKey = "weight";
	lower.op = VectorPredicateOp::VPO_GE;
	lower.value = PropertyValue(int64_t{5});
	VectorizedPropertyPredicate upper;
	upper.propertyKey = "weight";
	upper.op = VectorPredicateOp::VPO_LT;
	upper.value = PropertyValue(int64_t{8});

	NodeScanConfig seedConfig;
	NodeScanRequirements seedRequirements;
	DirectRelationshipCountConfig directCount;
	directCount.enabled = true;
	directCount.edgeType = "FOLLOWS";
	directCount.edgePredicates = {lower, upper};

	RelationshipCountScanOperator op(dm, im, seedConfig, seedRequirements, {}, {}, directCount, "count");
	op.open();
	EXPECT_EQ(readCount(op.next()), 1);
}

TEST_F(RelationshipCountScanOperatorTest, DirectCountUsesEdgeTypeIndexForTypeOnly) {
	const int64_t source = addUser();
	const int64_t first = addUser();
	const int64_t second = addUser();
	addFollows(source, first);
	addFollows(first, second);
	addLikes(source, second);
	ASSERT_TRUE(im->createIndex("edge_type_count_idx", "edge", "", ""));

	NodeScanConfig seedConfig;
	NodeScanRequirements seedRequirements;
	DirectRelationshipCountConfig directCount;
	directCount.enabled = true;
	directCount.edgeType = "FOLLOWS";
	debug::PerfTrace::setEnabled(true);
	debug::PerfTrace::reset();

	RelationshipCountScanOperator op(dm, im, seedConfig, seedRequirements, {}, {}, directCount, "count");
	op.open();
	EXPECT_EQ(readCount(op.next()), 2);
	op.close();

	const auto snapshot = debug::PerfTrace::snapshotAndReset();
	EXPECT_TRUE(snapshot.contains("relationship_count.index_candidates"));
	EXPECT_FALSE(snapshot.contains("relationship_count.load_edge_metadata"));
	EXPECT_FALSE(snapshot.contains("relationship_count.property_columns"));
}

TEST_F(RelationshipCountScanOperatorTest, DirectCountUsesEdgePropertyIndexAndVerifiesResidualPredicates) {
	const int64_t source = addUser();
	const int64_t first = addUser();
	const int64_t second = addUser();
	addFollowsWithProperties(source, first, {{"weight", PropertyValue(int64_t{1})}, {"status", PropertyValue("kept")}});
	addFollowsWithProperties(source, second, {{"weight", PropertyValue(int64_t{1})}, {"status", PropertyValue("skip")}});
	addFollowsWithProperties(first, second, {{"weight", PropertyValue(int64_t{2})}, {"status", PropertyValue("kept")}});
	addLikesWithProperties(source, second, {{"weight", PropertyValue(int64_t{1})}, {"status", PropertyValue("kept")}});
	ASSERT_TRUE(im->createIndex("edge_type_residual_idx", "edge", "", ""));
	ASSERT_TRUE(im->createIndex("edge_weight_residual_idx", "edge", "FOLLOWS", "weight"));

	NodeScanConfig seedConfig;
	NodeScanRequirements seedRequirements;
	DirectRelationshipCountConfig directCount;
	directCount.enabled = true;
	directCount.edgeType = "FOLLOWS";
	directCount.edgeProperties = {
		{"weight", PropertyValue(int64_t{1})},
		{"status", PropertyValue("kept")},
	};
	debug::PerfTrace::setEnabled(true);
	debug::PerfTrace::reset();

	RelationshipCountScanOperator op(dm, im, seedConfig, seedRequirements, {}, {}, directCount, "count");
	op.open();
	EXPECT_EQ(readCount(op.next()), 1);
	op.close();

	const auto snapshot = debug::PerfTrace::snapshotAndReset();
	EXPECT_TRUE(snapshot.contains("relationship_count.index_candidates"));
	EXPECT_TRUE(snapshot.contains("relationship_count.index_filter"));
	EXPECT_FALSE(snapshot.contains("relationship_count.load_edge_metadata"));
	EXPECT_FALSE(snapshot.contains("relationship_count.property_columns"));
}

TEST_F(RelationshipCountScanOperatorTest, DirectCountVerifiesVectorResidualPredicatesAfterIndexCandidate) {
	const int64_t source = addUser();
	const int64_t first = addUser();
	const int64_t second = addUser();
	addFollowsWithProperties(source, first, {{"weight", PropertyValue(int64_t{1})}});
	addFollowsWithProperties(source, second, {{"weight", PropertyValue(int64_t{1})}});
	ASSERT_TRUE(im->createIndex("edge_weight_vector_residual_idx", "edge", "FOLLOWS", "weight"));

	VectorizedPropertyPredicate indexedEquality;
	indexedEquality.propertyKey = "weight";
	indexedEquality.op = VectorPredicateOp::VPO_EQ;
	indexedEquality.value = PropertyValue(int64_t{1});
	VectorizedPropertyPredicate residualRange;
	residualRange.propertyKey = "weight";
	residualRange.op = VectorPredicateOp::VPO_GT;
	residualRange.value = PropertyValue(int64_t{1});

	NodeScanConfig seedConfig;
	NodeScanRequirements seedRequirements;
	DirectRelationshipCountConfig directCount;
	directCount.enabled = true;
	directCount.edgeType = "FOLLOWS";
	directCount.edgePredicates = {indexedEquality, residualRange};

	RelationshipCountScanOperator op(dm, im, seedConfig, seedRequirements, {}, {}, directCount, "count");
	op.open();
	EXPECT_EQ(readCount(op.next()), 0);
}

TEST_F(RelationshipCountScanOperatorTest, DirectCountTrustsCoveredVectorEqualityIndexCandidates) {
	const int64_t source = addUser();
	const int64_t first = addUser();
	const int64_t second = addUser();
	addFollowsWithProperties(source, first, {{"weight", PropertyValue(int64_t{1})}});
	addFollowsWithProperties(source, second, {{"weight", PropertyValue(int64_t{2})}});
	ASSERT_TRUE(im->createIndex("edge_weight_vector_eq_idx", "edge", "FOLLOWS", "weight"));

	VectorizedPropertyPredicate weightEquals;
	weightEquals.propertyKey = "weight";
	weightEquals.op = VectorPredicateOp::VPO_EQ;
	weightEquals.value = PropertyValue(int64_t{1});

	NodeScanConfig seedConfig;
	NodeScanRequirements seedRequirements;
	DirectRelationshipCountConfig directCount;
	directCount.enabled = true;
	directCount.edgeType = "FOLLOWS";
	directCount.edgePredicates = {weightEquals};

	RelationshipCountScanOperator op(dm, im, seedConfig, seedRequirements, {}, {}, directCount, "count");
	op.open();
	EXPECT_EQ(readCount(op.next()), 1);
}

TEST_F(RelationshipCountScanOperatorTest, PlannedPropertyIndexCanProvideExactGlobalCount) {
	const int64_t source = addUser();
	const int64_t first = addUser();
	const int64_t second = addUser();
	addFollowsWithProperties(source, first, {{"weight", PropertyValue(int64_t{1})}});
	addLikesWithProperties(source, second, {{"weight", PropertyValue(int64_t{1})}});
	addFollowsWithProperties(first, second, {{"weight", PropertyValue(int64_t{2})}});
	ASSERT_TRUE(im->createIndex("edge_weight_exact_global_count_idx", "edge", "", "weight"));

	NodeScanConfig seedConfig;
	NodeScanRequirements seedRequirements;
	DirectRelationshipCountConfig directCount;
	directCount.enabled = true;
	directCount.edgeProperties = {{"weight", PropertyValue(int64_t{1})}};
	directCount.candidateSource.type = DirectRelationshipCandidateSourceType::DRCS_PROPERTY_INDEX;
	directCount.candidateSource.propertyKeys = {"weight"};
	debug::PerfTrace::setEnabled(true);
	debug::PerfTrace::reset();

	RelationshipCountScanOperator op(dm, im, seedConfig, seedRequirements, {}, {}, directCount, "count");
	op.open();
	EXPECT_EQ(readCount(op.next()), 2);
	op.close();

	const auto snapshot = debug::PerfTrace::snapshotAndReset();
	EXPECT_TRUE(snapshot.contains("relationship_count.index_count"));
	EXPECT_FALSE(snapshot.contains("relationship_count.index_candidates"));
	EXPECT_FALSE(snapshot.contains("relationship_count.property_columns"));
}

TEST_F(RelationshipCountScanOperatorTest, PlannedPropertyIndexFallsBackForResidualRangePredicate) {
	const int64_t source = addUser();
	const int64_t first = addUser();
	const int64_t second = addUser();
	addFollowsWithProperties(source, first, {{"weight", PropertyValue(int64_t{1})}});
	addLikesWithProperties(source, second, {{"weight", PropertyValue(int64_t{3})}});
	ASSERT_TRUE(im->createIndex("edge_weight_range_fallback_idx", "edge", "", "weight"));

	VectorizedPropertyPredicate weightAtLeastTwo;
	weightAtLeastTwo.propertyKey = "weight";
	weightAtLeastTwo.op = VectorPredicateOp::VPO_GE;
	weightAtLeastTwo.value = PropertyValue(int64_t{2});

	NodeScanConfig seedConfig;
	NodeScanRequirements seedRequirements;
	DirectRelationshipCountConfig directCount;
	directCount.enabled = true;
	directCount.edgePredicates = {weightAtLeastTwo};
	directCount.candidateSource.type = DirectRelationshipCandidateSourceType::DRCS_PROPERTY_INDEX;
	directCount.candidateSource.propertyKeys = {"weight"};
	debug::PerfTrace::setEnabled(true);
	debug::PerfTrace::reset();

	RelationshipCountScanOperator op(dm, im, seedConfig, seedRequirements, {}, {}, directCount, "count");
	op.open();
	EXPECT_EQ(readCount(op.next()), 1);
	op.close();

	const auto snapshot = debug::PerfTrace::snapshotAndReset();
	EXPECT_FALSE(snapshot.contains("relationship_count.index_count"));
	EXPECT_FALSE(snapshot.contains("relationship_count.index_candidates"));
	EXPECT_TRUE(snapshot.contains("relationship_count.direct_scan"));
}

TEST_F(RelationshipCountScanOperatorTest, PlannedPropertyIndexWithTypeFilterFallsBackAndVerifiesType) {
	const int64_t source = addUser();
	const int64_t first = addUser();
	const int64_t second = addUser();
	addFollowsWithProperties(source, first, {{"weight", PropertyValue(int64_t{1})}});
	addLikesWithProperties(source, second, {{"weight", PropertyValue(int64_t{1})}});
	ASSERT_TRUE(im->createIndex("edge_weight_typed_property_fallback_idx", "edge", "", "weight"));

	NodeScanConfig seedConfig;
	NodeScanRequirements seedRequirements;
	DirectRelationshipCountConfig directCount;
	directCount.enabled = true;
	directCount.edgeType = "FOLLOWS";
	directCount.edgeProperties = {{"weight", PropertyValue(int64_t{1})}};
	directCount.candidateSource.type = DirectRelationshipCandidateSourceType::DRCS_PROPERTY_INDEX;
	directCount.candidateSource.propertyKeys = {"weight"};
	debug::PerfTrace::setEnabled(true);
	debug::PerfTrace::reset();

	RelationshipCountScanOperator op(dm, im, seedConfig, seedRequirements, {}, {}, directCount, "count");
	op.open();
	EXPECT_EQ(readCount(op.next()), 1);
	op.close();

	const auto snapshot = debug::PerfTrace::snapshotAndReset();
	EXPECT_FALSE(snapshot.contains("relationship_count.index_count"));
	EXPECT_TRUE(snapshot.contains("relationship_count.index_candidates"));
	EXPECT_TRUE(snapshot.contains("relationship_count.index_filter"));
}

TEST_F(RelationshipCountScanOperatorTest, PlannedPropertyIndexWithUnindexedKeyFallsBackToDirectScan) {
	const int64_t source = addUser();
	const int64_t target = addUser();
	addFollowsWithProperties(source, target, {{"weight", PropertyValue(int64_t{1})}});

	NodeScanConfig seedConfig;
	NodeScanRequirements seedRequirements;
	DirectRelationshipCountConfig directCount;
	directCount.enabled = true;
	directCount.edgeProperties = {{"weight", PropertyValue(int64_t{1})}};
	directCount.candidateSource.type = DirectRelationshipCandidateSourceType::DRCS_PROPERTY_INDEX;
	directCount.candidateSource.propertyKeys = {"weight"};
	debug::PerfTrace::setEnabled(true);
	debug::PerfTrace::reset();

	RelationshipCountScanOperator op(dm, im, seedConfig, seedRequirements, {}, {}, directCount, "count");
	op.open();
	EXPECT_EQ(readCount(op.next()), 1);
	op.close();

	const auto snapshot = debug::PerfTrace::snapshotAndReset();
	EXPECT_FALSE(snapshot.contains("relationship_count.index_count"));
	EXPECT_FALSE(snapshot.contains("relationship_count.index_candidates"));
	EXPECT_TRUE(snapshot.contains("relationship_count.direct_scan"));
}

TEST_F(RelationshipCountScanOperatorTest, PlannedPropertyIndexWithEmptyKeysFallsBackToDirectScan) {
	const int64_t source = addUser();
	const int64_t target = addUser();
	addFollowsWithProperties(source, target, {{"weight", PropertyValue(int64_t{1})}});
	ASSERT_TRUE(im->createIndex("edge_weight_empty_planned_keys_idx", "edge", "", "weight"));

	NodeScanConfig seedConfig;
	NodeScanRequirements seedRequirements;
	DirectRelationshipCountConfig directCount;
	directCount.enabled = true;
	directCount.edgeProperties = {{"weight", PropertyValue(int64_t{1})}};
	directCount.candidateSource.type = DirectRelationshipCandidateSourceType::DRCS_PROPERTY_INDEX;

	RelationshipCountScanOperator op(dm, im, seedConfig, seedRequirements, {}, {}, directCount, "count");
	op.open();
	EXPECT_EQ(readCount(op.next()), 1);
}

TEST_F(RelationshipCountScanOperatorTest, PlannedTypeIndexWithoutTypeFallsBackToDirectScan) {
	const int64_t source = addUser();
	const int64_t first = addUser();
	const int64_t second = addUser();
	addFollows(source, first);
	addLikes(source, second);
	ASSERT_TRUE(im->createIndex("edge_type_empty_type_idx", "edge", "", ""));

	NodeScanConfig seedConfig;
	NodeScanRequirements seedRequirements;
	DirectRelationshipCountConfig directCount;
	directCount.enabled = true;
	directCount.candidateSource.type = DirectRelationshipCandidateSourceType::DRCS_TYPE_INDEX;
	debug::PerfTrace::setEnabled(true);
	debug::PerfTrace::reset();

	RelationshipCountScanOperator op(dm, im, seedConfig, seedRequirements, {}, {}, directCount, "count");
	op.open();
	EXPECT_EQ(readCount(op.next()), 2);
	op.close();

	const auto snapshot = debug::PerfTrace::snapshotAndReset();
	EXPECT_FALSE(snapshot.contains("relationship_count.index_count"));
	EXPECT_TRUE(snapshot.contains("relationship_count.direct_scan"));
}

TEST_F(RelationshipCountScanOperatorTest, PlannedTypeIndexWithResidualPredicateUsesScanFallback) {
	const int64_t source = addUser();
	const int64_t first = addUser();
	const int64_t second = addUser();
	addFollowsWithProperties(source, first, {{"weight", PropertyValue(int64_t{1})}});
	addFollowsWithProperties(source, second, {{"weight", PropertyValue(int64_t{2})}});
	ASSERT_TRUE(im->createIndex("edge_type_with_residual_predicate_idx", "edge", "", ""));

	VectorizedPropertyPredicate weightEqualsOne;
	weightEqualsOne.propertyKey = "weight";
	weightEqualsOne.op = VectorPredicateOp::VPO_EQ;
	weightEqualsOne.value = PropertyValue(int64_t{1});

	NodeScanConfig seedConfig;
	NodeScanRequirements seedRequirements;
	DirectRelationshipCountConfig directCount;
	directCount.enabled = true;
	directCount.edgeType = "FOLLOWS";
	directCount.edgePredicates = {weightEqualsOne};
	directCount.candidateSource.type = DirectRelationshipCandidateSourceType::DRCS_TYPE_INDEX;

	RelationshipCountScanOperator op(dm, im, seedConfig, seedRequirements, {}, {}, directCount, "count");
	op.open();
	EXPECT_EQ(readCount(op.next()), 1);
}

TEST_F(RelationshipCountScanOperatorTest, DirectCountFiltersGlobalPropertyIndexCandidatesByType) {
	const int64_t source = addUser();
	const int64_t first = addUser();
	const int64_t second = addUser();
	addFollowsWithProperties(source, first, {{"weight", PropertyValue(int64_t{1})}});
	addLikesWithProperties(source, second, {{"weight", PropertyValue(int64_t{1})}});
	ASSERT_TRUE(im->createIndex("edge_weight_global_idx", "edge", "", "weight"));

	NodeScanConfig seedConfig;
	NodeScanRequirements seedRequirements;
	DirectRelationshipCountConfig directCount;
	directCount.enabled = true;
	directCount.edgeType = "FOLLOWS";
	directCount.edgeProperties = {{"weight", PropertyValue(int64_t{1})}};

	RelationshipCountScanOperator op(dm, im, seedConfig, seedRequirements, {}, {}, directCount, "count");
	op.open();
	EXPECT_EQ(readCount(op.next()), 1);
}

TEST_F(RelationshipCountScanOperatorTest, TypePropertyIntersectionCandidateNeedsNoResidualVerification) {
	const int64_t source = addUser();
	const int64_t first = addUser();
	const int64_t second = addUser();
	addFollowsWithProperties(source, first, {{"weight", PropertyValue(int64_t{1})}});
	addFollowsWithProperties(source, second, {{"weight", PropertyValue(int64_t{2})}});
	addLikesWithProperties(source, second, {{"weight", PropertyValue(int64_t{1})}});
	ASSERT_TRUE(im->createIndex("edge_type_property_intersection_type_idx", "edge", "", ""));
	ASSERT_TRUE(im->createIndex("edge_type_property_intersection_weight_idx", "edge", "", "weight"));

	NodeScanConfig seedConfig;
	NodeScanRequirements seedRequirements;
	DirectRelationshipCountConfig directCount;
	directCount.enabled = true;
	directCount.edgeType = "FOLLOWS";
	directCount.edgeProperties = {{"weight", PropertyValue(int64_t{1})}};
	directCount.candidateSource.type = DirectRelationshipCandidateSourceType::DRCS_TYPE_PROPERTY_INTERSECTION;
	directCount.candidateSource.propertyKeys = {"weight"};
	debug::PerfTrace::setEnabled(true);
	debug::PerfTrace::reset();

	RelationshipCountScanOperator op(dm, im, seedConfig, seedRequirements, {}, {}, directCount, "count");
	op.open();
	EXPECT_EQ(readCount(op.next()), 1);
	op.close();

	const auto snapshot = debug::PerfTrace::snapshotAndReset();
	EXPECT_TRUE(snapshot.contains("relationship_count.index_candidates"));
	EXPECT_FALSE(snapshot.contains("relationship_count.index_filter"));
	EXPECT_FALSE(snapshot.contains("relationship_count.direct_scan"));
}

TEST_F(RelationshipCountScanOperatorTest, DirectCountReturnsZeroForEmptyPropertyIndexCandidates) {
	const int64_t source = addUser();
	const int64_t target = addUser();
	addFollowsWithProperties(source, target, {{"weight", PropertyValue(int64_t{1})}});
	ASSERT_TRUE(im->createIndex("edge_weight_empty_candidates_idx", "edge", "FOLLOWS", "weight"));

	NodeScanConfig seedConfig;
	NodeScanRequirements seedRequirements;
	DirectRelationshipCountConfig directCount;
	directCount.enabled = true;
	directCount.edgeType = "FOLLOWS";
	directCount.edgeProperties = {{"weight", PropertyValue(int64_t{99})}};

	RelationshipCountScanOperator op(dm, im, seedConfig, seedRequirements, {}, {}, directCount, "count");
	op.open();
	EXPECT_EQ(readCount(op.next()), 0);
}

TEST_F(RelationshipCountScanOperatorTest, CleanDirectRelationshipCountFallsBackForBlobProperties) {
	const int64_t source = addUser();
	const int64_t target = addUser();
	const std::string payload(512, 'x');
	for (int i = 0; i < 128; ++i) {
		Edge edge(0, source, target, followsType);
		dm->addEdge(edge);
		dm->addEdgeProperties(edge.getId(), {{"payload", PropertyValue(payload)}, {"weight", PropertyValue(int64_t{i == 0 ? 7 : 9})}});
	}
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());

	NodeScanConfig seedConfig;
	NodeScanRequirements seedRequirements;
	DirectRelationshipCountConfig directCount;
	directCount.enabled = true;
	directCount.edgeType = "FOLLOWS";
	directCount.edgeProperties = {{"weight", PropertyValue(int64_t{7})}};

	RelationshipCountScanOperator op(dm, im, seedConfig, seedRequirements, {}, {}, directCount, "count");
	op.open();
	EXPECT_EQ(readCount(op.next()), 1);
}
