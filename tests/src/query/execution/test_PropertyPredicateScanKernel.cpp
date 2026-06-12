#include <gtest/gtest.h>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <memory>
#include <unordered_map>
#include <vector>

#include "graph/core/Database.hpp"
#include "graph/query/execution/PropertyPredicateScanKernel.hpp"

namespace fs = std::filesystem;

using namespace graph;
using namespace graph::query::execution;

namespace {

class PropertyPredicateScanKernelTest : public ::testing::Test {
protected:
	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		dbPath_ = fs::temp_directory_path() /
		          ("test_property_predicate_scan_kernel_" + boost::uuids::to_string(uuid) + ".zyx");
		db_ = std::make_unique<Database>(dbPath_.string());
		db_->open();
		dm_ = db_->getStorage()->getDataManager();
	}

	void TearDown() override {
		dm_.reset();
		if (db_) {
			db_->close();
		}
		db_.reset();
		std::error_code ec;
		fs::remove_all(dbPath_, ec);
	}

	int64_t addPropertyEntity(std::unordered_map<std::string, PropertyValue> values) {
		Property property;
		property.setEntityInfo(1, Node::typeId);
		property.setProperties(std::move(values));
		dm_->addPropertyEntity(property);
		return property.getId();
	}

	static VectorizedPropertyPredicate predicate(std::string key, VectorPredicateOp op, PropertyValue value) {
		VectorizedPropertyPredicate predicate;
		predicate.propertyKey = std::move(key);
		predicate.op = op;
		predicate.value = std::move(value);
		return predicate;
	}

	fs::path dbPath_;
	std::unique_ptr<Database> db_;
	std::shared_ptr<storage::DataManager> dm_;
};

} // namespace

TEST_F(PropertyPredicateScanKernelTest, CountsEqualityPredicatesWithoutMaterializingMaps) {
	const std::vector<int64_t> ids = {
			addPropertyEntity({{"country", PropertyValue("CN")}, {"score", PropertyValue(int64_t{10})}}),
			addPropertyEntity({{"country", PropertyValue("US")}, {"score", PropertyValue(int64_t{20})}}),
			addPropertyEntity({{"country", PropertyValue("CN")}, {"score", PropertyValue(int64_t{30})}}),
	};
	db_->getStorage()->flush();

	auto kernel = PropertyPredicateScanKernel::fromEqualityPredicates(
			dm_, {{"country", PropertyValue("CN")}});

	EXPECT_FALSE(kernel.empty());
	EXPECT_EQ(kernel.countPropertyEntities(ids), 2U);
	EXPECT_TRUE(kernel.matchesMap({{"country", PropertyValue("CN")}}));
	EXPECT_FALSE(kernel.matchesMap({{"country", PropertyValue("US")}}));
}

TEST_F(PropertyPredicateScanKernelTest, CountMatchesReportsLoadedPropertyEntities) {
	const std::vector<int64_t> ids = {
			addPropertyEntity({{"country", PropertyValue("CN")}, {"score", PropertyValue(int64_t{10})}}),
			addPropertyEntity({{"country", PropertyValue("US")}, {"score", PropertyValue(int64_t{20})}}),
	};
	db_->getStorage()->flush();

	auto equalityKernel = PropertyPredicateScanKernel::fromEqualityPredicates(
			dm_, {{"country", PropertyValue("CN")}});
	auto equalityCount = equalityKernel.countPropertyEntityMatches({ids[0], ids[1], ids[1] + 1000});
	EXPECT_EQ(equalityCount.loadedCount, 2U);
	EXPECT_EQ(equalityCount.matchedCount, 1U);

	PropertyPredicateScanKernel rangeKernel(
			dm_, {predicate("score", VectorPredicateOp::VPO_GE, PropertyValue(int64_t{20}))});
	auto rangeCount = rangeKernel.countPropertyEntityMatches({ids[0], ids[1], ids[1]});
	EXPECT_EQ(rangeCount.loadedCount, 3U);
	EXPECT_EQ(rangeCount.matchedCount, 2U);
}

TEST_F(PropertyPredicateScanKernelTest, CountsOwnerTypeMatchesWhenStorageCanProveOwnersActive) {
	Node first;
	Node second;
	dm_->addNode(first);
	dm_->addNode(second);
	dm_->addNodeProperties(first.getId(), {{"score", PropertyValue(95.0)}});
	dm_->addNodeProperties(second.getId(), {{"score", PropertyValue(12.0)}});

	Edge edge;
	edge.setSourceNodeId(first.getId());
	edge.setTargetNodeId(second.getId());
	edge.setTypeId(dm_->getOrCreateTokenId("KERNEL_OWNER_EDGE"));
	dm_->addEdge(edge);
	dm_->addEdgeProperties(edge.getId(), {{"score", PropertyValue(99.0)}});
	db_->getStorage()->flush();

	PropertyPredicateScanKernel kernel(
			dm_, {predicate("score", VectorPredicateOp::VPO_GE, PropertyValue(90.0))});
	auto nodeCount = kernel.countOwnerTypeMatches(EntityType::Node);
	ASSERT_TRUE(nodeCount.has_value());
	EXPECT_EQ(nodeCount->loadedCount, 2U);
	EXPECT_EQ(nodeCount->matchedCount, 1U);

	auto edgeCount = kernel.countOwnerTypeMatches(EntityType::Edge);
	ASSERT_TRUE(edgeCount.has_value());
	EXPECT_EQ(edgeCount->loadedCount, 1U);
	EXPECT_EQ(edgeCount->matchedCount, 1U);
}

TEST_F(PropertyPredicateScanKernelTest, CountsCompleteOwnerPropertiesAndRejectsUnsafeOwnerScans) {
	Node first;
	Node second;
	dm_->addNode(first);
	dm_->addNode(second);
	dm_->addNodeProperties(first.getId(), {{"score", PropertyValue(int64_t{95})}});
	dm_->addNodeProperties(second.getId(), {{"score", PropertyValue(int64_t{12})}});
	db_->getStorage()->flush();

	PropertyPredicateScanKernel kernel(
			dm_, {predicate("score", VectorPredicateOp::VPO_GE, PropertyValue(int64_t{90}))});
	auto allNodeProperties = kernel.countAllOwnerProperties(EntityType::Node);
	ASSERT_TRUE(allNodeProperties.has_value());
	EXPECT_EQ(allNodeProperties->loadedCount, 2U);
	EXPECT_EQ(allNodeProperties->matchedCount, 1U);

	storage::PropertyEntityOwnerPredicateScanOptions options;
	options.beginOwnerId = first.getId();
	options.endOwnerId = second.getId();
	auto matchingOwners = kernel.collectAllOwnerIds(EntityType::Node, options);
	ASSERT_TRUE(matchingOwners.has_value());
	EXPECT_EQ(*matchingOwners, (std::vector<int64_t>{first.getId()}));

	EXPECT_FALSE(kernel.countOwnerTypeMatches(EntityType::Blob).has_value());

	PropertyPredicateScanKernel missing(
			nullptr, {predicate("score", VectorPredicateOp::VPO_GE, PropertyValue(int64_t{90}))});
	EXPECT_FALSE(missing.countOwnerTypeMatches(EntityType::Node).has_value());
	EXPECT_FALSE(missing.countAllOwnerProperties(EntityType::Node).has_value());
	EXPECT_FALSE(missing.collectAllOwnerIds(EntityType::Node, options).has_value());
	EXPECT_EQ(missing.matchPropertyEntities({1}, {0}, 1).matchedCount, 0U);

	PropertyPredicateScanKernel empty(dm_, {});
	EXPECT_FALSE(empty.countOwnerTypeMatches(EntityType::Node).has_value());
	EXPECT_FALSE(empty.countAllOwnerProperties(EntityType::Node).has_value());
	EXPECT_FALSE(empty.collectAllOwnerIds(EntityType::Node, options).has_value());
	EXPECT_EQ(kernel.matchPropertyEntities({}, {}, 0).matchedCount, 0U);
}

TEST_F(PropertyPredicateScanKernelTest, MatchesRowAlignedRangePredicatesAndReportsLoadedRows) {
	const std::vector<int64_t> ids = {
			addPropertyEntity({{"score", PropertyValue(int64_t{10})}}),
			addPropertyEntity({{"score", PropertyValue(int64_t{20})}}),
			addPropertyEntity({{"score", PropertyValue(int64_t{30})}}),
	};
	db_->getStorage()->flush();

	PropertyPredicateScanKernel kernel(
			dm_, {predicate("score", VectorPredicateOp::VPO_GE, PropertyValue(int64_t{20}))});

	storage::PropertyEntityPredicateMatchOptions options;
	options.collectLoadedRows = true;
	options.collectMatchedRows = true;
	auto result = kernel.matchPropertyEntities(ids, {0, 1, 2}, 3, options);

	EXPECT_EQ(result.loadedCount, 3U);
	EXPECT_EQ(result.matchedCount, 2U);
	EXPECT_EQ(result.loadedRows, (std::vector<size_t>{0, 1, 2}));
	EXPECT_EQ(result.matchedRows, (std::vector<size_t>{1, 2}));
}

TEST_F(PropertyPredicateScanKernelTest, EmptyOrMissingStorageReturnsNoMatches) {
	PropertyPredicateScanKernel missing(nullptr, {predicate("score", VectorPredicateOp::VPO_EQ, PropertyValue(int64_t{1}))});
	EXPECT_EQ(missing.countPropertyEntities({1, 2}), 0U);

	PropertyPredicateScanKernel empty(dm_, {});
	EXPECT_TRUE(empty.empty());
	EXPECT_EQ(empty.countPropertyEntities({1, 2}), 0U);
	EXPECT_EQ(empty.matchPropertyEntities({1}, {0}, 1).matchedCount, 0U);
}
