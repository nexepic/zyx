/**
 * @file test_Database_EdgeCases.cpp
 * @brief Tests targeting uncovered branches in Database.cpp:
 *        - openIfExists catch(...) block (line 67)
 *        - setThreadPoolSize with VIM null (line 128-129)
 *        - close() lifecycle with the native logical backend
 */

#include <gtest/gtest.h>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <fstream>
#include "graph/core/Database.hpp"

namespace fs = std::filesystem;
using namespace graph;

class DatabaseEdgeCaseTest : public ::testing::Test {
protected:
	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testDbPath = fs::temp_directory_path() / ("test_db_edge_" + boost::uuids::to_string(uuid) + ".zyx");
		fs::remove_all(testDbPath);
	}

	void TearDown() override {
		std::error_code ec;
		fs::remove_all(testDbPath, ec);
		fs::remove(testDbPath.string() + "-wal", ec);
	}

	fs::path testDbPath;
};

// ============================================================================
// openIfExists: catch(...) block when open() throws
// Create a corrupted file to trigger an exception during open()
// ============================================================================

TEST_F(DatabaseEdgeCaseTest, OpenIfExistsExceptionInOpen) {
	// Create a corrupted database file
	{
		std::ofstream ofs(testDbPath.string(), std::ios::binary);
		ofs << "CORRUPTED DATA THAT IS NOT A VALID DATABASE";
	}
	ASSERT_TRUE(fs::exists(testDbPath));

	// openIfExists should catch the exception and return false
	auto db = std::make_unique<Database>(testDbPath.string(), storage::OpenMode::OPEN_EXISTING_FILE);
	EXPECT_TRUE(db->exists());

	bool result = db->openIfExists();
	EXPECT_FALSE(result);
	EXPECT_FALSE(db->isOpen());
}

// ============================================================================
// Native backend close before any logical transaction
// ============================================================================

TEST_F(DatabaseEdgeCaseTest, CloseBeforeLogicalTransaction) {
	auto db = std::make_unique<Database>(testDbPath.string());
	db->open();
	EXPECT_TRUE(db->isOpen());

	db->close();
	EXPECT_FALSE(db->isOpen());
}

TEST_F(DatabaseEdgeCaseTest, FlushIsNoopWhenClosedAndWorksBeforeWalInitialization) {
	auto db = std::make_unique<Database>(testDbPath.string());

	EXPECT_FALSE(db->isOpen());
	EXPECT_NO_THROW(db->flush());

	db->open();
	ASSERT_TRUE(db->isOpen());
	EXPECT_NO_THROW(db->flush());
}

// ============================================================================
// Native backend close after a logical transaction
// ============================================================================

TEST_F(DatabaseEdgeCaseTest, CloseAfterLogicalTransaction) {
	auto db = std::make_unique<Database>(testDbPath.string());
	db->open();

	{
		auto txn = db->beginTransaction();
		txn.commit();
	}

	db->close();
	EXPECT_FALSE(db->isOpen());
}

// ============================================================================
// setThreadPoolSize: with queryEngine initialized and VIM available
// ============================================================================

TEST_F(DatabaseEdgeCaseTest, SetThreadPoolSizeWithQueryEngine) {
	auto db = std::make_unique<Database>(testDbPath.string());
	db->open();

	// Initialize query engine
	auto engine = db->getQueryEngine();
	ASSERT_NE(engine, nullptr);

	// setThreadPoolSize should re-wire pool to all subsystems
	EXPECT_NO_THROW(db->setThreadPoolSize(2));
}

// ============================================================================
// setThreadPoolSize: without queryEngine (null check)
// ============================================================================

TEST_F(DatabaseEdgeCaseTest, SetThreadPoolSizeWithoutQueryEngine) {
	auto db = std::make_unique<Database>(testDbPath.string());
	db->open();

	// Don't initialize query engine
	// setThreadPoolSize should handle null queryEngine gracefully
	EXPECT_NO_THROW(db->setThreadPoolSize(2));
}

// ============================================================================
// setThreadPoolSize: without storage (null check)
// ============================================================================

TEST_F(DatabaseEdgeCaseTest, SetThreadPoolSizeAfterClose) {
	auto db = std::make_unique<Database>(testDbPath.string());
	db->open();
	db->close();

	// After close, storage->isOpen() is false but storage ptr still exists
	// setThreadPoolSize should still work (it doesn't check isOpen)
	EXPECT_NO_THROW(db->setThreadPoolSize(2));
}

// ============================================================================
// beginTransaction auto-opens closed database
// ============================================================================

TEST_F(DatabaseEdgeCaseTest, BeginTransactionAutoOpens) {
	auto db = std::make_unique<Database>(testDbPath.string());
	EXPECT_FALSE(db->isOpen());

	// beginTransaction should auto-open
	auto txn = db->beginTransaction();
	EXPECT_TRUE(db->isOpen());
	EXPECT_TRUE(txn.isActive());
	txn.commit();
}

// ============================================================================
// beginReadOnlyTransaction auto-opens closed database
// ============================================================================

TEST_F(DatabaseEdgeCaseTest, BeginReadOnlyTransactionAutoOpens) {
	auto db = std::make_unique<Database>(testDbPath.string());
	EXPECT_FALSE(db->isOpen());

	// beginReadOnlyTransaction should auto-open
	auto txn = db->beginReadOnlyTransaction();
	EXPECT_TRUE(db->isOpen());
	EXPECT_TRUE(txn.isReadOnly());
	txn.commit();
}

TEST_F(DatabaseEdgeCaseTest, BeginBulkTransactionAutoOpens) {
	auto db = std::make_unique<Database>(testDbPath.string());
	EXPECT_FALSE(db->isOpen());

	auto txn = db->beginBulkTransaction();
	EXPECT_TRUE(db->isOpen());
	EXPECT_TRUE(txn.isActive());
	txn.commit();
}

// ============================================================================
// getQueryEngine before open returns nullptr
// ============================================================================

TEST_F(DatabaseEdgeCaseTest, GetQueryEngineBeforeOpen) {
	auto db = std::make_unique<Database>(testDbPath.string());
	EXPECT_FALSE(db->isOpen());

	auto engine = db->getQueryEngine();
	EXPECT_EQ(engine, nullptr);
}

// ============================================================================
// getThreadPool before open returns nullptr
// ============================================================================

TEST_F(DatabaseEdgeCaseTest, GetThreadPoolBeforeOpen) {
	auto db = std::make_unique<Database>(testDbPath.string());
	EXPECT_FALSE(db->isOpen());

	auto tp = db->getThreadPool();
	EXPECT_EQ(tp, nullptr);
}

// ============================================================================
// hasActiveTransaction when transactionManager_ is null
// ============================================================================

TEST_F(DatabaseEdgeCaseTest, HasActiveTransactionBeforeAnyTransaction) {
	auto db = std::make_unique<Database>(testDbPath.string());
	// Before open
	EXPECT_FALSE(db->hasActiveTransaction());

	db->open();
	// After open but before any transaction
	EXPECT_FALSE(db->hasActiveTransaction());
}

// ============================================================================
// Multiple open() calls (idempotent)
// ============================================================================

TEST_F(DatabaseEdgeCaseTest, MultipleOpenCallsIdempotent) {
	auto db = std::make_unique<Database>(testDbPath.string());
	db->open();
	EXPECT_TRUE(db->isOpen());

	// Second open should be a no-op
	db->open();
	EXPECT_TRUE(db->isOpen());

	// Third open should be a no-op
	db->open();
	EXPECT_TRUE(db->isOpen());
}

// ============================================================================
// Multiple close() calls (idempotent)
// ============================================================================

TEST_F(DatabaseEdgeCaseTest, MultipleCloseCallsIdempotent) {
	auto db = std::make_unique<Database>(testDbPath.string());
	db->open();

	db->close();
	EXPECT_FALSE(db->isOpen());

	// Second close should be a no-op
	db->close();
	EXPECT_FALSE(db->isOpen());
}
