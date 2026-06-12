#include <gtest/gtest.h>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "graph/core/Database.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/query/execution/NodeColumnarPredicateCounter.hpp"

namespace fs = std::filesystem;

using namespace graph;
using namespace graph::query::execution;

namespace {

class NodeColumnarPredicateCounterTest : public ::testing::Test {
protected:
	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		dbPath_ = fs::temp_directory_path() /
		          ("test_node_columnar_predicate_counter_" + boost::uuids::to_string(uuid) + ".zyx");
		db_ = std::make_unique<Database>(dbPath_.string());
		db_->open();
		dm_ = db_->getStorage()->getDataManager();
		userLabel_ = dm_->getOrCreateTokenId("User");
		otherLabel_ = dm_->getOrCreateTokenId("Other");
	}

	void TearDown() override {
		graph::debug::PerfTrace::reset();
		graph::debug::PerfTrace::setEnabled(false);
		dm_.reset();
		if (db_) {
			db_->close();
		}
		db_.reset();
		std::error_code ec;
		fs::remove_all(dbPath_, ec);
	}

	std::vector<int64_t> addNodes(size_t count) {
		std::vector<int64_t> ids;
		ids.reserve(count);
		for (size_t i = 0; i < count; ++i) {
			Node node(0, (i % 2 == 0) ? userLabel_ : otherLabel_);
			dm_->addNode(node);
			dm_->addNodeProperties(node.getId(), {
				{"country", PropertyValue((i % 3 == 0) ? "CN" : "US")},
				{"age", PropertyValue(static_cast<int64_t>(20 + (i % 50)))},
				{"score", PropertyValue(static_cast<int64_t>(i * 10))}
			});
			ids.push_back(node.getId());
		}
		return ids;
	}

	std::vector<int64_t> addScoreOnlyNodes(size_t count) {
		std::vector<int64_t> ids;
		ids.reserve(count);
		for (size_t i = 0; i < count; ++i) {
			Node node(0, userLabel_);
			dm_->addNode(node);
			dm_->addNodeProperties(node.getId(), {{"score", PropertyValue(static_cast<int64_t>(i * 10))}});
			ids.push_back(node.getId());
		}
		return ids;
	}

	NodeScanRequirements countRequirements(bool needsLabels) const {
		NodeScanRequirements requirements;
		requirements.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
		requirements.requiredProperties = {"country", "age", "score"};
		requirements.needsLabels = needsLabels;
		requirements.needsActiveCheck = true;
		requirements.countOnly = true;
		return requirements;
	}

	NodeCandidateSet candidateSet(std::vector<int64_t> ids) const {
		NodeCandidateSet set;
		set.ids = std::move(ids);
		return set;
	}

	fs::path dbPath_;
	std::unique_ptr<Database> db_;
	std::shared_ptr<storage::DataManager> dm_;
	int64_t userLabel_ = 0;
	int64_t otherLabel_ = 0;
};

VectorizedPropertyPredicate predicate(std::string key, VectorPredicateOp op, PropertyValue value) {
	VectorizedPropertyPredicate predicate;
	predicate.variable = "n";
	predicate.propertyKey = std::move(key);
	predicate.op = op;
	predicate.value = std::move(value);
	return predicate;
}

} // namespace

TEST_F(NodeColumnarPredicateCounterTest, CountsRangePredicateWithoutMaterializingNodes) {
	auto ids = addNodes(160);
	db_->getStorage()->flush();
	dm_->clearCache();

	NodeColumnarPredicateCounter counter(dm_);
	NodeScanConfig config;
	const auto result = counter.count(
		ids,
		candidateSet(ids),
		config,
		countRequirements(false),
		{predicate("score", VectorPredicateOp::VPO_GE, PropertyValue(static_cast<int64_t>(900)))});

	EXPECT_TRUE(result.available);
	EXPECT_EQ(result.count, 70);
}

TEST_F(NodeColumnarPredicateCounterTest, FullScanOwnerShortcutRequiresCompleteCandidateSet) {
	auto ids = addNodes(160);
	db_->getStorage()->flush();
	dm_->clearCache();

	std::vector<int64_t> subset(ids.begin(), ids.begin() + 130);
	NodeColumnarPredicateCounter counter(dm_);
	NodeScanConfig config;
	const auto result = counter.count(
		subset,
		candidateSet(subset),
		config,
		countRequirements(false),
		{predicate("score", VectorPredicateOp::VPO_GE, PropertyValue(static_cast<int64_t>(0)))});

	EXPECT_TRUE(result.available);
	EXPECT_EQ(result.count, 130);
}

TEST_F(NodeColumnarPredicateCounterTest, FullScanUsesCompleteOwnerPropertyShortcutForScalarProperties) {
	auto ids = addScoreOnlyNodes(160);
	db_->getStorage()->flush();
	dm_->clearCache();

	graph::debug::PerfTrace::setEnabled(true);
	graph::debug::PerfTrace::reset();

	NodeColumnarPredicateCounter counter(dm_);
	NodeScanConfig config;
	const auto result = counter.count(
		ids,
		candidateSet(ids),
		config,
		countRequirements(false),
		{predicate("score", VectorPredicateOp::VPO_GE, PropertyValue(static_cast<int64_t>(1000)))});

	const auto snapshot = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setEnabled(false);
	EXPECT_TRUE(result.available);
	EXPECT_EQ(result.count, 60);
	EXPECT_TRUE(snapshot.contains("node_scan.predicate_count"));
}

TEST_F(NodeColumnarPredicateCounterTest, AppliesLabelAndMultiplePredicates) {
	auto ids = addNodes(160);
	db_->getStorage()->flush();
	dm_->clearCache();

	NodeScanConfig config;
	config.labels = {"User"};
	NodeColumnarPredicateCounter counter(dm_);
	const auto result = counter.count(
		ids,
		candidateSet(ids),
		config,
		countRequirements(true),
		{predicate("country", VectorPredicateOp::VPO_EQ, PropertyValue("CN")),
		 predicate("age", VectorPredicateOp::VPO_GE, PropertyValue(static_cast<int64_t>(30)))});

	EXPECT_TRUE(result.available);
	EXPECT_EQ(result.count, 20);
}


TEST_F(NodeColumnarPredicateCounterTest, SupportsAllStoragePredicateOperators) {
	auto ids = addNodes(160);
	db_->getStorage()->flush();
	dm_->clearCache();

	NodeColumnarPredicateCounter counter(dm_);
	NodeScanConfig config;
	const auto requirements = countRequirements(false);

	auto countWith = [&](std::vector<VectorizedPropertyPredicate> predicates) {
		return counter.count(ids, candidateSet(ids), config, requirements, predicates);
	};

	auto notUs = countWith({predicate("country", VectorPredicateOp::VPO_NE, PropertyValue("US"))});
	EXPECT_TRUE(notUs.available);
	EXPECT_EQ(notUs.count, 54);

	auto lessThan = countWith({predicate("score", VectorPredicateOp::VPO_LT, PropertyValue(static_cast<int64_t>(100)))});
	EXPECT_TRUE(lessThan.available);
	EXPECT_EQ(lessThan.count, 10);

	auto lessEqual = countWith({predicate("score", VectorPredicateOp::VPO_LE, PropertyValue(static_cast<int64_t>(100)))});
	EXPECT_TRUE(lessEqual.available);
	EXPECT_EQ(lessEqual.count, 11);

	auto greaterThan = countWith({predicate("score", VectorPredicateOp::VPO_GT, PropertyValue(static_cast<int64_t>(1500)))});
	EXPECT_TRUE(greaterThan.available);
	EXPECT_EQ(greaterThan.count, 9);

	VectorizedPropertyPredicate range = predicate("score", VectorPredicateOp::VPO_RANGE_CLOSED, PropertyValue(static_cast<int64_t>(300)));
	range.upperValue = PropertyValue(static_cast<int64_t>(390));
	auto rangeResult = countWith({range});
	EXPECT_TRUE(rangeResult.available);
	EXPECT_EQ(rangeResult.count, 10);
}

TEST_F(NodeColumnarPredicateCounterTest, SkipsInactiveRowsDuringColumnarCount) {
	auto ids = addNodes(160);
	Node deleted = dm_->getNode(ids[1]);
	dm_->deleteNode(deleted);
	db_->getStorage()->flush();
	dm_->clearCache();

	NodeColumnarPredicateCounter counter(dm_);
	NodeScanConfig config;
	const auto result = counter.count(
		ids,
		candidateSet(ids),
		config,
		countRequirements(false),
		{predicate("score", VectorPredicateOp::VPO_GE, PropertyValue(static_cast<int64_t>(0)))});

	EXPECT_TRUE(result.available);
	EXPECT_EQ(result.count, 159);
}


TEST_F(NodeColumnarPredicateCounterTest, FallsBackForBlobStoredProperties) {
	std::vector<int64_t> ids;
	ids.reserve(130);
	const std::string matchingPayload(5000, 'm');
	const std::string otherPayload(5000, 'x');
	for (size_t i = 0; i < 130; ++i) {
		Node node(0, userLabel_);
		dm_->addNode(node);
		const std::string &payload = (i == 0) ? matchingPayload : otherPayload;
		dm_->addNodeProperties(node.getId(), {{"payload", PropertyValue(payload)}});
		ids.push_back(node.getId());
	}
	db_->getStorage()->flush();
	dm_->clearCache();

	NodeColumnarPredicateCounter counter(dm_);
	NodeScanConfig config;
	const auto result = counter.count(
		ids,
		candidateSet(ids),
		config,
		countRequirements(false),
		{predicate("payload", VectorPredicateOp::VPO_EQ, PropertyValue(matchingPayload))});

	EXPECT_TRUE(result.available);
	EXPECT_EQ(result.count, 1);
}


TEST_F(NodeColumnarPredicateCounterTest, UnknownLabelMatchesNoRows) {
	auto ids = addNodes(160);
	db_->getStorage()->flush();
	dm_->clearCache();

	NodeScanConfig config;
	config.labels = {"DoesNotExist"};
	NodeColumnarPredicateCounter counter(dm_);
	const auto result = counter.count(
		ids,
		candidateSet(ids),
		config,
		countRequirements(true),
		{predicate("score", VectorPredicateOp::VPO_GE, PropertyValue(static_cast<int64_t>(0)))});

	EXPECT_TRUE(result.available);
	EXPECT_EQ(result.count, 0);
}

TEST_F(NodeColumnarPredicateCounterTest, HandlesRowsWithoutExternalProperties) {
	std::vector<int64_t> ids;
	ids.reserve(130);
	for (size_t i = 0; i < 130; ++i) {
		Node node(0, userLabel_);
		dm_->addNode(node);
		ids.push_back(node.getId());
	}
	Node propertyEntityWithoutId = dm_->getNode(ids[0]);
	propertyEntityWithoutId.setPropertyEntityId(0, PropertyStorageType::PROPERTY_ENTITY);
	dm_->updateNode(propertyEntityWithoutId);
	Node blobWithoutId = dm_->getNode(ids[1]);
	blobWithoutId.setPropertyEntityId(0, PropertyStorageType::BLOB_ENTITY);
	dm_->updateNode(blobWithoutId);
	db_->getStorage()->flush();
	dm_->clearCache();

	NodeColumnarPredicateCounter counter(dm_);
	NodeScanConfig config;
	const auto result = counter.count(
		ids,
		candidateSet(ids),
		config,
		countRequirements(false),
		{predicate("score", VectorPredicateOp::VPO_GE, PropertyValue(static_cast<int64_t>(0)))});

	EXPECT_TRUE(result.available);
	EXPECT_EQ(result.count, 0);
}

TEST_F(NodeColumnarPredicateCounterTest, FallsBackWhenPropertyEntityCannotBeBulkMatched) {
	auto ids = addNodes(160);
	Node corrupted = dm_->getNode(ids[0]);
	corrupted.setPropertyEntityId(corrupted.getPropertyEntityId() + 1000000, PropertyStorageType::PROPERTY_ENTITY);
	dm_->updateNode(corrupted);
	db_->getStorage()->flush();
	dm_->clearCache();

	NodeColumnarPredicateCounter counter(dm_);
	NodeScanConfig config;
	const auto result = counter.count(
		ids,
		candidateSet(ids),
		config,
		countRequirements(false),
		{predicate("score", VectorPredicateOp::VPO_GE, PropertyValue(static_cast<int64_t>(0)))});

	EXPECT_TRUE(result.available);
	EXPECT_EQ(result.count, 159);
}

TEST_F(NodeColumnarPredicateCounterTest, FallsBackFromChunkedPropertyEntityScanWhenBulkMatchIsSparse) {
	auto ids = addNodes(160);
	Node corrupted = dm_->getNode(ids[0]);
	corrupted.setPropertyEntityId(corrupted.getPropertyEntityId() + 1000000, PropertyStorageType::PROPERTY_ENTITY);
	dm_->updateNode(corrupted);
	db_->getStorage()->flush();
	dm_->clearCache();

	NodeScanConfig config;
	config.labels = {"User"};
	NodeColumnarPredicateCounter counter(dm_);
	const auto result = counter.count(
		ids,
		candidateSet(ids),
		config,
		countRequirements(true),
		{predicate("score", VectorPredicateOp::VPO_GE, PropertyValue(static_cast<int64_t>(0)))});

	EXPECT_TRUE(result.available);
	EXPECT_EQ(result.count, 79);
}

TEST_F(NodeColumnarPredicateCounterTest, FallsBackFromChunkedMetadataScanForBlobStoredProperties) {
	std::vector<int64_t> ids;
	ids.reserve(130);
	const std::string matchingPayload(5000, 'm');
	const std::string otherPayload(5000, 'x');
	for (size_t i = 0; i < 130; ++i) {
		Node node(0, userLabel_);
		dm_->addNode(node);
		const std::string &payload = (i == 0) ? matchingPayload : otherPayload;
		dm_->addNodeProperties(node.getId(), {{"payload", PropertyValue(payload)}});
		ids.push_back(node.getId());
	}
	db_->getStorage()->flush();
	dm_->clearCache();

	NodeScanConfig config;
	config.labels = {"User"};
	NodeColumnarPredicateCounter counter(dm_);
	const auto result = counter.count(
		ids,
		candidateSet(ids),
		config,
		countRequirements(true),
		{predicate("payload", VectorPredicateOp::VPO_EQ, PropertyValue(matchingPayload))});

	EXPECT_TRUE(result.available);
	EXPECT_EQ(result.count, 1);
}

TEST_F(NodeColumnarPredicateCounterTest, MissingPropertyReturnsZeroMatches) {
	auto ids = addNodes(160);
	db_->getStorage()->flush();
	dm_->clearCache();

	NodeColumnarPredicateCounter counter(dm_);
	NodeScanConfig config;
	const auto result = counter.count(
		ids,
		candidateSet(ids),
		config,
		countRequirements(false),
		{predicate("missing", VectorPredicateOp::VPO_EQ, PropertyValue(static_cast<int64_t>(1)))});

	EXPECT_TRUE(result.available);
	EXPECT_EQ(result.count, 0);
}

TEST_F(NodeColumnarPredicateCounterTest, ReportsUnavailableForUnsafeMetadataLoads) {
	NodeColumnarPredicateCounter nullCounter(nullptr);
	NodeScanConfig config;
	EXPECT_FALSE(nullCounter.count({1}, candidateSet({1}), config, countRequirements(false),
	                               {predicate("score", VectorPredicateOp::VPO_GE, PropertyValue(static_cast<int64_t>(0)))}).available);

	NodeColumnarPredicateCounter counter(dm_);
	EXPECT_FALSE(counter.count({}, candidateSet({}), config, countRequirements(false),
	                           {predicate("score", VectorPredicateOp::VPO_GE, PropertyValue(static_cast<int64_t>(0)))}).available);
	EXPECT_FALSE(counter.count({1}, candidateSet({1}), config, countRequirements(false), {}).available);

	auto ids = addNodes(160);
	const auto result = counter.count(
		ids,
		candidateSet(ids),
		config,
		countRequirements(false),
		{predicate("score", VectorPredicateOp::VPO_GE, PropertyValue(static_cast<int64_t>(900)))});

	EXPECT_FALSE(result.available);
	EXPECT_EQ(result.count, 0);
}
