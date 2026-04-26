/**
 * @file test_Database_EdgeCases.cpp
 * @brief Tests targeting uncovered branches in Database.cpp:
 *        - openIfExists catch(...) block (line 67)
 *        - setThreadPoolSize with VIM null (line 128-129)
 *        - close() with walManager_ null vs non-null
 *        - ensureWALForWrites when walManager_ already exists
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
// close() when walManager_ is null (database never had a transaction)
// ============================================================================

TEST_F(DatabaseEdgeCaseTest, CloseWithoutWAL) {
	auto db = std::make_unique<Database>(testDbPath.string());
	db->open();
	EXPECT_TRUE(db->isOpen());

	// Don't create any transactions, so walManager_ stays null
	db->close();
	EXPECT_FALSE(db->isOpen());
}

// ============================================================================
// close() when walManager_ is initialized (after transaction)
// ============================================================================

TEST_F(DatabaseEdgeCaseTest, CloseWithWALManager) {
	auto db = std::make_unique<Database>(testDbPath.string());
	db->open();

	// Create a transaction to initialize WAL
	{
		auto txn = db->beginTransaction();
		txn.commit();
	}

	// close() should call walManager_->close() (line 78-79)
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
// WAL recovery then write transaction (ensureWALForWrites early return)
// ============================================================================

TEST_F(DatabaseEdgeCaseTest, WALRecoveryThenWriteReusesWAL) {
	std::string walPath = testDbPath.string() + "-wal";

	// Phase 1: Create database with WAL
	{
		auto db1 = std::make_unique<Database>(testDbPath.string());
		db1->open();
		{
			auto txn = db1->beginTransaction();
			// auto-rollback
		}

		// Read WAL before destructor deletes it
		std::vector<uint8_t> walContent;
		{
			std::ifstream walIn(walPath, std::ios::binary);
			if (walIn.is_open()) {
				walContent.assign(std::istreambuf_iterator<char>(walIn),
								  std::istreambuf_iterator<char>());
			}
		}

		db1.reset();

		// Restore WAL to simulate crash
		if (!walContent.empty()) {
			std::ofstream walOut(walPath, std::ios::binary | std::ios::trunc);
			walOut.write(reinterpret_cast<const char *>(walContent.data()),
						 static_cast<std::streamsize>(walContent.size()));
		}
	}

	// Phase 2: Reopen with WAL file present
	if (fs::exists(walPath)) {
		auto db2 = std::make_unique<Database>(testDbPath.string());
		db2->open();

		// This should reuse the WAL from recovery (ensureWALForWrites early return)
		auto txn = db2->beginTransaction();
		EXPECT_TRUE(txn.isActive());
		txn.commit();

		db2->close();
	}
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
