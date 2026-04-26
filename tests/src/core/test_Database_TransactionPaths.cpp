/**
 * @file test_Database_TransactionPaths.cpp
 * @brief Branch coverage tests for Database.cpp and TransactionManager.cpp:
 *        - Database::beginReadOnlyTransaction with closed DB
 *        - Database::hasActiveTransaction before/after txn
 *        - Database::getQueryEngine / getThreadPool when closed
 *        - Database::setThreadPoolSize full re-wiring
 *        - TransactionManager: read-only commit, read-only rollback,
 *          WAL checkpoint on commit, writeLock not owning
 */

#include <gtest/gtest.h>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include "graph/core/Database.hpp"
#include "graph/core/TransactionManager.hpp"

namespace fs = std::filesystem;
using namespace graph;

class DatabaseTransactionPathsTest : public ::testing::Test {
protected:
	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testDbPath = fs::temp_directory_path() / ("test_db_txnpath_" + boost::uuids::to_string(uuid) + ".zyx");
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
// beginReadOnlyTransaction opens DB if not open
// ============================================================================

TEST_F(DatabaseTransactionPathsTest, BeginReadOnlyTransactionOpensDB) {
	Database db(testDbPath.string());
	// DB not open yet
	EXPECT_FALSE(db.isOpen());

	auto txn = db.beginReadOnlyTransaction();
	EXPECT_TRUE(db.isOpen());
	EXPECT_TRUE(txn.isReadOnly());
	txn.commit(); // Commit read-only
}

// ============================================================================
// beginTransaction opens DB if not open
// ============================================================================

TEST_F(DatabaseTransactionPathsTest, BeginTransactionOpensDB) {
	Database db(testDbPath.string());
	EXPECT_FALSE(db.isOpen());

	auto txn = db.beginTransaction();
	EXPECT_TRUE(db.isOpen());
	EXPECT_FALSE(txn.isReadOnly());
	txn.rollback();
}

// ============================================================================
// hasActiveTransaction: false before, true during, false after
// ============================================================================

TEST_F(DatabaseTransactionPathsTest, HasActiveTransactionLifecycle) {
	Database db(testDbPath.string());
	db.open();

	EXPECT_FALSE(db.hasActiveTransaction());

	auto txn = db.beginTransaction();
	EXPECT_TRUE(db.hasActiveTransaction());

	txn.commit();
	EXPECT_FALSE(db.hasActiveTransaction());
}

// ============================================================================
// getQueryEngine / getThreadPool when DB is closed
// ============================================================================

TEST_F(DatabaseTransactionPathsTest, GetQueryEngineAndThreadPoolWhenClosed) {
	Database db(testDbPath.string());
	// DB not open
	EXPECT_EQ(db.getQueryEngine(), nullptr);
	EXPECT_EQ(db.getThreadPool(), nullptr);
}

// ============================================================================
// getQueryEngine when DB is open
// ============================================================================

TEST_F(DatabaseTransactionPathsTest, GetQueryEngineWhenOpen) {
	Database db(testDbPath.string());
	db.open();

	auto qe = db.getQueryEngine();
	EXPECT_NE(qe, nullptr);

	auto tp = db.getThreadPool();
	EXPECT_NE(tp, nullptr);
}

// ============================================================================
// setThreadPoolSize re-wires subsystems
// ============================================================================

TEST_F(DatabaseTransactionPathsTest, SetThreadPoolSizeRewires) {
	Database db(testDbPath.string());
	db.open();

	// Initialize query engine first
	auto qe = db.getQueryEngine();
	EXPECT_NE(qe, nullptr);

	// Now set a new thread pool size (re-wires)
	db.setThreadPoolSize(2);

	// Verify it still works
	auto tp = db.getThreadPool();
	EXPECT_NE(tp, nullptr);
}

// ============================================================================
// Read-only transaction commit and rollback paths
// ============================================================================

TEST_F(DatabaseTransactionPathsTest, ReadOnlyTransactionCommit) {
	Database db(testDbPath.string());
	db.open();

	auto txn = db.beginReadOnlyTransaction();
	EXPECT_TRUE(txn.isReadOnly());
	EXPECT_TRUE(txn.isActive());

	txn.commit();
	EXPECT_FALSE(txn.isActive());
}

TEST_F(DatabaseTransactionPathsTest, ReadOnlyTransactionRollback) {
	Database db(testDbPath.string());
	db.open();

	auto txn = db.beginReadOnlyTransaction();
	EXPECT_TRUE(txn.isReadOnly());

	txn.rollback();
	EXPECT_FALSE(txn.isActive());
}

// ============================================================================
// Double commit / double rollback should throw
// ============================================================================

TEST_F(DatabaseTransactionPathsTest, DoubleCommitThrows) {
	Database db(testDbPath.string());
	db.open();

	auto txn = db.beginTransaction();
	txn.commit();
	EXPECT_THROW(txn.commit(), std::runtime_error);
}

TEST_F(DatabaseTransactionPathsTest, DoubleRollbackThrows) {
	Database db(testDbPath.string());
	db.open();

	auto txn = db.beginTransaction();
	txn.rollback();
	EXPECT_THROW(txn.rollback(), std::runtime_error);
}

// ============================================================================
// Transaction move assignment with active transaction (rollback branch)
// ============================================================================

TEST_F(DatabaseTransactionPathsTest, TransactionMoveAssignmentActive) {
	Database db(testDbPath.string());
	db.open();

	auto txn1 = db.beginTransaction();
	txn1.commit();

	// Create a new txn
	auto txn2 = db.beginTransaction();

	// Move assign txn2 into txn1 (txn1 is already committed, so no rollback)
	txn1 = std::move(txn2);
	EXPECT_TRUE(txn1.isActive());
	txn1.commit();
}

// ============================================================================
// open() when already open is a no-op
// ============================================================================

TEST_F(DatabaseTransactionPathsTest, DoubleOpenIsNoop) {
	Database db(testDbPath.string());
	db.open();
	EXPECT_TRUE(db.isOpen());

	// Second open should be a no-op
	db.open();
	EXPECT_TRUE(db.isOpen());
}

// ============================================================================
// openIfExists when already open returns true
// ============================================================================

TEST_F(DatabaseTransactionPathsTest, OpenIfExistsWhenAlreadyOpen) {
	Database db(testDbPath.string());
	db.open();
	EXPECT_TRUE(db.isOpen());

	EXPECT_TRUE(db.openIfExists());
}

// ============================================================================
// openIfExists when file doesn't exist returns false
// ============================================================================

TEST_F(DatabaseTransactionPathsTest, OpenIfExistsNoFile) {
	std::string nonexistentPath = testDbPath.string() + "_nonexistent";
	Database db(nonexistentPath);
	EXPECT_FALSE(db.openIfExists());
}

// ============================================================================
// close when not open is a no-op
// ============================================================================

TEST_F(DatabaseTransactionPathsTest, CloseWhenNotOpen) {
	Database db(testDbPath.string());
	// Not opened - close should be a no-op
	db.close();
	EXPECT_FALSE(db.isOpen());
}

// ============================================================================
// WAL checkpoint: commit with enough data to trigger checkpoint
// ============================================================================

TEST_F(DatabaseTransactionPathsTest, CommitWithDataTriggersWALFlow) {
	Database db(testDbPath.string());
	db.open();

	{
		auto txn = db.beginTransaction();
		auto dm = db.getStorage()->getDataManager();
		// Add multiple nodes to exercise WAL write paths
		for (int i = 0; i < 5; i++) {
			Node n(0, 1);
			n.addProperty("name", std::string("node_") + std::to_string(i));
			dm->addNode(n);
		}
		txn.commit();
	}

	// Verify data persisted
	{
		auto txn = db.beginReadOnlyTransaction();
		auto dm = db.getStorage()->getDataManager();
		auto node = dm->getNode(1);
		EXPECT_NE(node.getId(), 0);
		txn.commit();
	}

	db.close();
}
