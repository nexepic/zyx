/**
 * @file test_PropertyIndex_Composite.cpp
 * @brief Branch coverage tests for PropertyIndex composite index methods
 *        and findRange type promotion paths.
 **/

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>

#include "graph/core/Database.hpp"
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/indexes/EntityTypeIndexManager.hpp"
#include "graph/storage/indexes/IndexManager.hpp"
#include "graph/storage/indexes/PropertyIndex.hpp"
#include "graph/storage/state/SystemStateKeys.hpp"

using graph::PropertyValue;
using graph::PropertyType;

class PropertyIndexCompositeTest : public ::testing::Test {
protected:
	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = std::filesystem::temp_directory_path() /
					   ("test_pi_composite_" + boost::uuids::to_string(uuid) + ".dat");

		database = std::make_unique<graph::Database>(testFilePath.string());
		database->open();
		dataManager = database->getStorage()->getDataManager();
		propertyIndex = database->getQueryEngine()
							->getIndexManager()
							->getNodeIndexManager()
							->getPropertyIndex();
	}

	void TearDown() override {
		if (database)
			database->close();
		database.reset();
		std::error_code ec;
		if (std::filesystem::exists(testFilePath))
			std::filesystem::remove(testFilePath, ec);
	}

	std::filesystem::path testFilePath;
	std::unique_ptr<graph::Database> database;
	std::shared_ptr<graph::storage::DataManager> dataManager;
	std::shared_ptr<graph::query::indexes::PropertyIndex> propertyIndex;
};

// ============================================================================
// Composite Index Lifecycle (lines 529-559)
// ============================================================================

TEST_F(PropertyIndexCompositeTest, CreateCompositeIndex_Basic) {
	propertyIndex->createCompositeIndex({"name", "age"});
	EXPECT_TRUE(propertyIndex->hasCompositeIndex({"name", "age"}));
}

TEST_F(PropertyIndexCompositeTest, CreateCompositeIndex_Duplicate_Idempotent) {
	// Cover line 533: compositeKeyDefinitions_.contains(keyStr) → return
	propertyIndex->createCompositeIndex({"name", "age"});
	propertyIndex->createCompositeIndex({"name", "age"});
	EXPECT_TRUE(propertyIndex->hasCompositeIndex({"name", "age"}));
}

TEST_F(PropertyIndexCompositeTest, HasCompositeIndex_NonExistent) {
	EXPECT_FALSE(propertyIndex->hasCompositeIndex({"x", "y"}));
}

TEST_F(PropertyIndexCompositeTest, RemoveCompositeIndex_Existing) {
	propertyIndex->createCompositeIndex({"a", "b"});
	EXPECT_TRUE(propertyIndex->hasCompositeIndex({"a", "b"}));

	propertyIndex->removeCompositeIndex({"a", "b"});
	EXPECT_FALSE(propertyIndex->hasCompositeIndex({"a", "b"}));
}

TEST_F(PropertyIndexCompositeTest, RemoveCompositeIndex_NonExistent) {
	// Cover line 552: rootIt == compositeRootIds_.end() → skip clear
	propertyIndex->removeCompositeIndex({"x", "y"});
	EXPECT_FALSE(propertyIndex->hasCompositeIndex({"x", "y"}));
}

// ============================================================================
// Composite Entry CRUD (lines 566-606)
// ============================================================================

TEST_F(PropertyIndexCompositeTest, AddAndFindCompositeEntry) {
	propertyIndex->createCompositeIndex({"name", "age"});

	propertyIndex->addCompositeEntry(1,
		{"name", "age"},
		{PropertyValue(std::string("Alice")), PropertyValue(static_cast<int64_t>(30))});

	auto results = propertyIndex->findCompositeExact(
		{"name", "age"},
		{PropertyValue(std::string("Alice")), PropertyValue(static_cast<int64_t>(30))});
	ASSERT_EQ(results.size(), 1u);
	EXPECT_EQ(results[0], 1);
}

TEST_F(PropertyIndexCompositeTest, AddCompositeEntry_NoIndex_Noop) {
	// Cover line 573: rootIt == compositeRootIds_.end() → return
	propertyIndex->addCompositeEntry(1,
		{"x", "y"},
		{PropertyValue(static_cast<int64_t>(1)), PropertyValue(static_cast<int64_t>(2))});

	auto results = propertyIndex->findCompositeExact(
		{"x", "y"},
		{PropertyValue(static_cast<int64_t>(1)), PropertyValue(static_cast<int64_t>(2))});
	EXPECT_TRUE(results.empty());
}

TEST_F(PropertyIndexCompositeTest, RemoveCompositeEntry) {
	propertyIndex->createCompositeIndex({"name", "age"});

	propertyIndex->addCompositeEntry(1,
		{"name", "age"},
		{PropertyValue(std::string("Bob")), PropertyValue(static_cast<int64_t>(25))});

	// Verify entry exists
	auto res1 = propertyIndex->findCompositeExact(
		{"name", "age"},
		{PropertyValue(std::string("Bob")), PropertyValue(static_cast<int64_t>(25))});
	ASSERT_EQ(res1.size(), 1u);

	// Remove entry
	propertyIndex->removeCompositeEntry(1,
		{"name", "age"},
		{PropertyValue(std::string("Bob")), PropertyValue(static_cast<int64_t>(25))});

	auto res2 = propertyIndex->findCompositeExact(
		{"name", "age"},
		{PropertyValue(std::string("Bob")), PropertyValue(static_cast<int64_t>(25))});
	EXPECT_TRUE(res2.empty());
}

TEST_F(PropertyIndexCompositeTest, RemoveCompositeEntry_NoIndex_Noop) {
	// Cover line 587: rootIt == compositeRootIds_.end() → return
	propertyIndex->removeCompositeEntry(1,
		{"nonexistent", "keys"},
		{PropertyValue(static_cast<int64_t>(1)), PropertyValue(static_cast<int64_t>(2))});
	SUCCEED();
}

TEST_F(PropertyIndexCompositeTest, FindCompositeExact_NoIndex_ReturnsEmpty) {
	// Cover line 601: rootIt == compositeRootIds_.end() → return {}
	auto results = propertyIndex->findCompositeExact(
		{"no", "index"},
		{PropertyValue(static_cast<int64_t>(1)), PropertyValue(static_cast<int64_t>(2))});
	EXPECT_TRUE(results.empty());
}

// ============================================================================
// findCompositePrefix (lines 608-644)
// ============================================================================

TEST_F(PropertyIndexCompositeTest, FindCompositePrefix_Match) {
	propertyIndex->createCompositeIndex({"name", "age", "city"});

	propertyIndex->addCompositeEntry(1,
		{"name", "age", "city"},
		{PropertyValue(std::string("Alice")), PropertyValue(static_cast<int64_t>(30)),
		 PropertyValue(std::string("NYC"))});

	propertyIndex->addCompositeEntry(2,
		{"name", "age", "city"},
		{PropertyValue(std::string("Alice")), PropertyValue(static_cast<int64_t>(30)),
		 PropertyValue(std::string("LA"))});

	// Prefix search with first two keys
	auto results = propertyIndex->findCompositePrefix(
		{"name", "age"},
		{PropertyValue(std::string("Alice")), PropertyValue(static_cast<int64_t>(30))});
	EXPECT_GE(results.size(), 1u);
}

TEST_F(PropertyIndexCompositeTest, FindCompositePrefix_NoMatch) {
	propertyIndex->createCompositeIndex({"name", "age"});

	// Prefix keys don't match any composite index definition
	auto results = propertyIndex->findCompositePrefix(
		{"x"},
		{PropertyValue(static_cast<int64_t>(1))});
	EXPECT_TRUE(results.empty());
}

TEST_F(PropertyIndexCompositeTest, FindCompositePrefix_PrefixLongerThanDef) {
	// Cover line 615: keyDef.size() < prefixKeys.size() → continue
	propertyIndex->createCompositeIndex({"a", "b"});

	auto results = propertyIndex->findCompositePrefix(
		{"a", "b", "c"},
		{PropertyValue(static_cast<int64_t>(1)), PropertyValue(static_cast<int64_t>(2)),
		 PropertyValue(static_cast<int64_t>(3))});
	EXPECT_TRUE(results.empty());
}

TEST_F(PropertyIndexCompositeTest, FindCompositePrefix_PrefixMismatch) {
	// Cover line 620: keyDef[i] != prefixKeys[i] → isPrefix = false
	propertyIndex->createCompositeIndex({"name", "age"});

	auto results = propertyIndex->findCompositePrefix(
		{"city"},
		{PropertyValue(std::string("NYC"))});
	EXPECT_TRUE(results.empty());
}

TEST_F(PropertyIndexCompositeTest, FindCompositePrefix_NoCompositeIndexExists) {
	auto results = propertyIndex->findCompositePrefix(
		{"a"},
		{PropertyValue(static_cast<int64_t>(1))});
	EXPECT_TRUE(results.empty());
}

// ============================================================================
// Composite State Persistence (lines 646-684)
// ============================================================================

TEST_F(PropertyIndexCompositeTest, SaveAndReloadCompositeState) {
	propertyIndex->createCompositeIndex({"name", "age"});
	propertyIndex->addCompositeEntry(1,
		{"name", "age"},
		{PropertyValue(std::string("Alice")), PropertyValue(static_cast<int64_t>(30))});

	// Save state
	propertyIndex->saveCompositeState();
	propertyIndex->saveState();

	// Reload
	constexpr uint32_t indexType = graph::query::indexes::IndexTypes::NODE_PROPERTY_TYPE;
	const std::string stateKeyPrefix = graph::query::indexes::StateKeys::NODE_PROPERTY_PREFIX;

	auto reloaded = std::make_unique<graph::query::indexes::PropertyIndex>(
		dataManager, database->getStorage()->getSystemStateManager(),
		indexType, stateKeyPrefix);

	// Verify composite index was deserialized
	EXPECT_TRUE(reloaded->hasCompositeIndex({"name", "age"}));

	auto results = reloaded->findCompositeExact(
		{"name", "age"},
		{PropertyValue(std::string("Alice")), PropertyValue(static_cast<int64_t>(30))});
	ASSERT_EQ(results.size(), 1u);
	EXPECT_EQ(results[0], 1);
}

TEST_F(PropertyIndexCompositeTest, SerializeCompositeState_EmptyIndex) {
	// Cover lines 651/654: empty compositeRootIds_ and compositeKeyDefinitions_
	EXPECT_NO_THROW(propertyIndex->saveCompositeState());
}

TEST_F(PropertyIndexCompositeTest, DeserializeCompositeState_Empty) {
	// With no composite state saved, deserialization should produce empty maps
	constexpr uint32_t indexType = graph::query::indexes::IndexTypes::NODE_PROPERTY_TYPE;
	const std::string stateKeyPrefix = graph::query::indexes::StateKeys::NODE_PROPERTY_PREFIX;

	auto idx = std::make_unique<graph::query::indexes::PropertyIndex>(
		dataManager, database->getStorage()->getSystemStateManager(),
		indexType, stateKeyPrefix);

	EXPECT_FALSE(idx->hasCompositeIndex({"any", "keys"}));
}

// ============================================================================
// compositeKeyString and encodeCompositeKey (lines 509-527)
// ============================================================================

TEST_F(PropertyIndexCompositeTest, MultipleCompositeEntries) {
	propertyIndex->createCompositeIndex({"x", "y"});

	for (int64_t i = 1; i <= 5; ++i) {
		propertyIndex->addCompositeEntry(i,
			{"x", "y"},
			{PropertyValue(i), PropertyValue(i * 10)});
	}

	auto r3 = propertyIndex->findCompositeExact(
		{"x", "y"},
		{PropertyValue(static_cast<int64_t>(3)), PropertyValue(static_cast<int64_t>(30))});
	ASSERT_EQ(r3.size(), 1u);
	EXPECT_EQ(r3[0], 3);
}

TEST_F(PropertyIndexCompositeTest, SingleKeyComposite) {
	// Test composite index with a single key (degenerate case)
	propertyIndex->createCompositeIndex({"solo"});

	propertyIndex->addCompositeEntry(1, {"solo"}, {PropertyValue(std::string("val"))});

	auto results = propertyIndex->findCompositeExact(
		{"solo"}, {PropertyValue(std::string("val"))});
	ASSERT_EQ(results.size(), 1u);
}

// ============================================================================
// findRange type promotion (lines 376-396)
// ============================================================================

TEST_F(PropertyIndexCompositeTest, FindRange_IntToDoublePromotion) {
	// Cover line 382: targetType == DOUBLE && val.getType() == INTEGER
	propertyIndex->addProperty(1, "dbl_key", 1.5);
	propertyIndex->addProperty(2, "dbl_key", 2.5);
	propertyIndex->addProperty(3, "dbl_key", 3.5);

	// Query with INTEGER bounds on a DOUBLE-typed key → promotes int to double
	auto results = propertyIndex->findRange("dbl_key",
		PropertyValue(static_cast<int64_t>(2)),
		PropertyValue(static_cast<int64_t>(3)));
	EXPECT_GE(results.size(), 1u);
}

TEST_F(PropertyIndexCompositeTest, FindRange_DoubleToIntPromotion) {
	// Cover line 386-388: targetType == INTEGER && val.getType() == DOUBLE
	propertyIndex->addProperty(1, "int_key", static_cast<int64_t>(10));
	propertyIndex->addProperty(2, "int_key", static_cast<int64_t>(20));
	propertyIndex->addProperty(3, "int_key", static_cast<int64_t>(30));

	// Query with DOUBLE bounds on an INTEGER-typed key → ceil/floor promotion
	auto results = propertyIndex->findRange("int_key",
		PropertyValue(15.5), PropertyValue(25.5));
	// ceil(15.5) = 16, floor(25.5) = 25 → only 20 in range
	ASSERT_EQ(results.size(), 1u);
	EXPECT_EQ(results[0], 2);
}

TEST_F(PropertyIndexCompositeTest, FindRange_IncompatibleTypes) {
	// Cover line 392: incompatible types return false → empty result
	propertyIndex->addProperty(1, "str_key", std::string("hello"));

	// Query with INTEGER bounds on a STRING-typed key → incompatible
	auto results = propertyIndex->findRange("str_key",
		PropertyValue(static_cast<int64_t>(0)),
		PropertyValue(static_cast<int64_t>(100)));
	EXPECT_TRUE(results.empty());
}

TEST_F(PropertyIndexCompositeTest, FindRange_StringType) {
	// String type is supported for range queries
	propertyIndex->addProperty(1, "name", std::string("Alice"));
	propertyIndex->addProperty(2, "name", std::string("Bob"));
	propertyIndex->addProperty(3, "name", std::string("Charlie"));

	auto results = propertyIndex->findRange("name",
		PropertyValue(std::string("B")),
		PropertyValue(std::string("C")));
	EXPECT_GE(results.size(), 1u); // At least "Bob"
}
