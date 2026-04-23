/**
 * @file test_Database.cpp
 * @brief Comprehensive unit tests for Database class
 * @date 2026/02/21
 */

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

#include "graph/core/Database.hpp"

namespace fs = std::filesystem;
using namespace graph;

/**
 * @class DatabaseTest
 * @brief Unit test fixture for Database class
 */
class DatabaseTest : public ::testing::Test {
protected:
	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testDbPath = fs::temp_directory_path() / ("test_db_" + boost::uuids::to_string(uuid) + ".zyx");
		if (fs::exists(testDbPath))
			fs::remove_all(testDbPath);
	}

	void TearDown() override {
		std::error_code ec;
		if (fs::exists(testDbPath))
			fs::remove_all(testDbPath, ec);
	}

	std::filesystem::path testDbPath;
};

// ============================================================================
// Constructor Tests
// ============================================================================

TEST_F(DatabaseTest, Constructor_WithValidPath) {
	// Test basic constructor with valid path
	auto db = std::make_unique<Database>(testDbPath.string());
	EXPECT_FALSE(db->isOpen());
}

TEST_F(DatabaseTest, Constructor_WithOpenMode) {
	// Test constructor with different open modes
	auto db1 = std::make_unique<Database>(testDbPath.string(), storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
	EXPECT_FALSE(db1->isOpen());

	auto db2 = std::make_unique<Database>(testDbPath.string(), storage::OpenMode::OPEN_EXISTING_FILE);
	EXPECT_FALSE(db2->isOpen());
}

TEST_F(DatabaseTest, Constructor_WithCacheSize) {
	// Test constructor with custom cache size
	auto db = std::make_unique<Database>(testDbPath.string(), storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE, 1024);
	EXPECT_FALSE(db->isOpen());
}

// ============================================================================
// Open Tests
// ============================================================================

TEST_F(DatabaseTest, Open_Success) {
	// Test successful open
	auto db = std::make_unique<Database>(testDbPath.string());
	EXPECT_FALSE(db->isOpen());

	db->open();
	EXPECT_TRUE(db->isOpen());
}

TEST_F(DatabaseTest, Open_AlreadyOpened) {
	// Test opening an already opened database (Line 38-40)
	auto db = std::make_unique<Database>(testDbPath.string());
	db->open();
	EXPECT_TRUE(db->isOpen());

	// Call open again - should just return without error
	db->open();
	EXPECT_TRUE(db->isOpen());
}

TEST_F(DatabaseTest, Open_MultipleTimes) {
	// Test opening database multiple times
	auto db = std::make_unique<Database>(testDbPath.string());

	for (int i = 0; i < 5; ++i) {
		db->open();
		EXPECT_TRUE(db->isOpen());
	}
}

// ============================================================================
// OpenIfExists Tests
// ============================================================================

TEST_F(DatabaseTest, OpenIfExists_FileDoesNotExist) {
	// Test openIfExists when file doesn't exist (Line 52-53)
	auto db = std::make_unique<Database>(testDbPath.string());
	EXPECT_FALSE(db->exists());

	bool result = db->openIfExists();
	EXPECT_FALSE(result);
	EXPECT_FALSE(db->isOpen());
}

TEST_F(DatabaseTest, OpenIfExists_FileExists) {
	// Test openIfExists when file exists
	// First create the database
	{
		auto db1 = std::make_unique<Database>(testDbPath.string());
		db1->open();
		// Create some data
		auto txn = db1->beginTransaction();
	}
	// Database is now closed and file exists

	// Now test openIfExists
	auto db2 = std::make_unique<Database>(testDbPath.string());
	EXPECT_TRUE(db2->exists());

	bool result = db2->openIfExists();
	EXPECT_TRUE(result);
	EXPECT_TRUE(db2->isOpen());
}

TEST_F(DatabaseTest, OpenIfExists_AlreadyOpened) {
	// Test openIfExists when database is already open (Line 50-51)
	auto db = std::make_unique<Database>(testDbPath.string());
	db->open();
	EXPECT_TRUE(db->isOpen());

	bool result = db->openIfExists();
	EXPECT_TRUE(result);  // Should return true since it's already open
	EXPECT_TRUE(db->isOpen());
}

TEST_F(DatabaseTest, OpenIfExists_MultipleCalls) {
	// Test multiple calls to openIfExists
	auto db = std::make_unique<Database>(testDbPath.string());

	// File doesn't exist yet
	EXPECT_FALSE(db->openIfExists());

	// Create database
	db->open();
	auto txn = db->beginTransaction();
	db->close();

	// File exists now
	bool result = db->openIfExists();
	EXPECT_TRUE(result);
	EXPECT_TRUE(db->isOpen());

	// Call again when already open
	result = db->openIfExists();
	EXPECT_TRUE(result);
	EXPECT_TRUE(db->isOpen());
}

// ============================================================================
// Close Tests
// ============================================================================

TEST_F(DatabaseTest, Close_Success) {
	// Test closing an open database
	auto db = std::make_unique<Database>(testDbPath.string());
	db->open();
	EXPECT_TRUE(db->isOpen());

	db->close();
	EXPECT_FALSE(db->isOpen());
}

TEST_F(DatabaseTest, Close_AlreadyClosed) {
	// Test closing an already closed database (Line 64-66)
	auto db = std::make_unique<Database>(testDbPath.string());
	db->open();
	db->close();
	EXPECT_FALSE(db->isOpen());

	// Close again - should be safe (no-op)
	db->close();
	EXPECT_FALSE(db->isOpen());
}

TEST_F(DatabaseTest, Close_MultipleTimes) {
	// Test closing database multiple times
	auto db = std::make_unique<Database>(testDbPath.string());
	db->open();

	for (int i = 0; i < 5; ++i) {
		db->close();
		EXPECT_FALSE(db->isOpen());
	}
}

TEST_F(DatabaseTest, Close_WithoutOpen) {
	// Test close without ever opening
	auto db = std::make_unique<Database>(testDbPath.string());
	EXPECT_FALSE(db->isOpen());

	db->close();
	EXPECT_FALSE(db->isOpen());
}

// ============================================================================
// IsOpen Tests
// ============================================================================

TEST_F(DatabaseTest, IsOpen_NotOpened) {
	// Test isOpen on unopened database
	auto db = std::make_unique<Database>(testDbPath.string());
	EXPECT_FALSE(db->isOpen());
}

TEST_F(DatabaseTest, IsOpen_AfterOpen) {
	// Test isOpen after opening
	auto db = std::make_unique<Database>(testDbPath.string());
	db->open();
	EXPECT_TRUE(db->isOpen());
}

TEST_F(DatabaseTest, IsOpen_AfterClose) {
	// Test isOpen after closing
	auto db = std::make_unique<Database>(testDbPath.string());
	db->open();
	db->close();
	EXPECT_FALSE(db->isOpen());
}

// ============================================================================
// Exists Tests
// ============================================================================

TEST_F(DatabaseTest, Exists_NotCreated) {
	// Test exists on non-existent database
	auto db = std::make_unique<Database>(testDbPath.string());
	EXPECT_FALSE(db->exists());
}

TEST_F(DatabaseTest, Exists_AfterCreation) {
	// Test exists after creating database
	auto db = std::make_unique<Database>(testDbPath.string());
	db->open();
	EXPECT_TRUE(db->exists());
}

TEST_F(DatabaseTest, Exists_AfterClose) {
	// Test exists persists after close
	auto db = std::make_unique<Database>(testDbPath.string());
	db->open();
	db->close();
	EXPECT_TRUE(db->exists());
}

// ============================================================================
// BeginTransaction Tests
// ============================================================================

TEST_F(DatabaseTest, BeginTransaction_AfterOpen) {
	// Test beginTransaction when database is open
	auto db = std::make_unique<Database>(testDbPath.string());
	db->open();

	auto txn = db->beginTransaction();
	EXPECT_TRUE(db->isOpen());
}

TEST_F(DatabaseTest, BeginTransaction_BeforeOpen) {
	// Test beginTransaction before open (Line 74-76: auto-open if not open)
	auto db = std::make_unique<Database>(testDbPath.string());
	EXPECT_FALSE(db->isOpen());

	auto txn = db->beginTransaction();
	// Database should be auto-opened
	EXPECT_TRUE(db->isOpen());
}

TEST_F(DatabaseTest, BeginTransaction_AfterClose) {
	// Test beginTransaction after close (should auto-open)
	auto db = std::make_unique<Database>(testDbPath.string());
	db->open();
	db->close();
	EXPECT_FALSE(db->isOpen());

	auto txn = db->beginTransaction();
	// Database should be auto-opened again
	EXPECT_TRUE(db->isOpen());
}

TEST_F(DatabaseTest, BeginTransaction_Multiple) {
	// Test multiple transactions sequentially
	auto db = std::make_unique<Database>(testDbPath.string());
	db->open();

	{
		auto txn1 = db->beginTransaction();
	}
	{
		auto txn2 = db->beginTransaction();
	}
	{
		auto txn3 = db->beginTransaction();
	}

	EXPECT_TRUE(db->isOpen());
}

// ============================================================================
// Lifecycle Tests
// ============================================================================

TEST_F(DatabaseTest, Lifecycle_CreateOpenClose) {
	// Test basic lifecycle: create -> open -> close
	auto db = std::make_unique<Database>(testDbPath.string());
	EXPECT_FALSE(db->isOpen());

	db->open();
	EXPECT_TRUE(db->isOpen());

	db->close();
	EXPECT_FALSE(db->isOpen());
}

TEST_F(DatabaseTest, Lifecycle_MultipleOpenClose) {
	// Test multiple open/close cycles
	auto db = std::make_unique<Database>(testDbPath.string());

	for (int i = 0; i < 3; ++i) {
		db->open();
		EXPECT_TRUE(db->isOpen());

		db->close();
		EXPECT_FALSE(db->isOpen());
	}
}

TEST_F(DatabaseTest, Destructor_ClosesDatabase) {
	// Test that destructor closes the database
	{
		auto db = std::make_unique<Database>(testDbPath.string());
		db->open();
		EXPECT_TRUE(db->isOpen());
		// Destructor called here
	}
	// Database should be closed now
}

TEST_F(DatabaseTest, Lifecycle_CreateReopenClose) {
	// Test creating, opening, and reopening database
	{
		auto db1 = std::make_unique<Database>(testDbPath.string());
		db1->open();
		auto txn = db1->beginTransaction();
	}
	// Database is closed

	{
		auto db2 = std::make_unique<Database>(testDbPath.string());
		db2->open();
		EXPECT_TRUE(db2->isOpen());
	}
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST_F(DatabaseTest, EdgeCase_EmptyPath) {
	// Test with empty path (should use current directory)
	auto db = std::make_unique<Database>("");
	// Constructor should not crash
	EXPECT_FALSE(db->isOpen());
}

TEST_F(DatabaseTest, EdgeCase_LongPath) {
	// Test with a long path
	std::string longPath = (fs::temp_directory_path() / ("a" + std::string(100, 'b'))).string();
	auto db = std::make_unique<Database>(longPath);
	EXPECT_FALSE(db->isOpen());
}

TEST_F(DatabaseTest, EdgeCase_ZeroCacheSize) {
	// Test with zero cache size
	auto db = std::make_unique<Database>(testDbPath.string(), storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE, 0);
	db->open();
	EXPECT_TRUE(db->isOpen());
}

TEST_F(DatabaseTest, EdgeCase_LargeCacheSize) {
	// Test with large cache size
	auto db = std::make_unique<Database>(testDbPath.string(), storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE, 1024 * 1024);
	db->open();
	EXPECT_TRUE(db->isOpen());
}

// ============================================================================
// QueryEngine Access Tests
// ============================================================================

TEST_F(DatabaseTest, GetQueryEngine_AfterOpen) {
	// Test getQueryEngine after open
	auto db = std::make_unique<Database>(testDbPath.string());
	db->open();

	auto engine = db->getQueryEngine();
	EXPECT_NE(engine, nullptr);
}

TEST_F(DatabaseTest, GetQueryEngine_BeforeOpen) {
	// Test getQueryEngine before open
	auto db = std::make_unique<Database>(testDbPath.string());
	EXPECT_FALSE(db->isOpen());

	// getQueryEngine should return nullptr (not throw)
	auto engine = db->getQueryEngine();
	EXPECT_EQ(engine, nullptr);
}

// ============================================================================
// OpenMode Tests
// ============================================================================

TEST_F(DatabaseTest, OpenMode_CreateOrOpen) {
	// Test CREATE_OR_OPEN_FILE mode
	auto db = std::make_unique<Database>(testDbPath.string(), storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
	db->open();
	EXPECT_TRUE(db->isOpen());

	// Should be able to write
	auto txn = db->beginTransaction();
	EXPECT_TRUE(db->isOpen());
}

TEST_F(DatabaseTest, OpenMode_OpenExisting) {
	// Test OPEN_EXISTING_FILE mode
	// First create database
	{
		auto db1 = std::make_unique<Database>(testDbPath.string(), storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
		db1->open();
	}
	// Now open in OPEN_EXISTING_FILE mode
	{
		auto db2 = std::make_unique<Database>(testDbPath.string(), storage::OpenMode::OPEN_EXISTING_FILE);
		db2->open();
		EXPECT_TRUE(db2->isOpen());
	}
}

// ============================================================================
// HasActiveTransaction Tests (Database.cpp line 109-111)
// ============================================================================

TEST_F(DatabaseTest, HasActiveTransaction_NoTransactionManager) {
	// Test hasActiveTransaction when transactionManager_ is null (before open)
	auto db = std::make_unique<Database>(testDbPath.string());
	EXPECT_FALSE(db->hasActiveTransaction());
}

TEST_F(DatabaseTest, HasActiveTransaction_NoActiveTransaction) {
	// Test hasActiveTransaction when no transaction is active
	auto db = std::make_unique<Database>(testDbPath.string());
	db->open();
	EXPECT_FALSE(db->hasActiveTransaction());
}

TEST_F(DatabaseTest, HasActiveTransaction_WithActiveTransaction) {
	// Test hasActiveTransaction returns true when transaction is active
	auto db = std::make_unique<Database>(testDbPath.string());
	db->open();
	auto txn = db->beginTransaction();
	EXPECT_TRUE(db->hasActiveTransaction());
}

// ============================================================================
// Close with WAL Manager Tests (Database.cpp lines 86-88)
// ============================================================================

TEST_F(DatabaseTest, Close_WithWALManager) {
	// Test close() when walManager_ is initialized (line 86-88)
	auto db = std::make_unique<Database>(testDbPath.string());
	db->open();
	// WAL manager is created during open()
	db->close();
	EXPECT_FALSE(db->isOpen());
	// Opening again should reinitialize WAL
	db->open();
	EXPECT_TRUE(db->isOpen());
}

// ============================================================================
// Open with WAL Recovery Tests (Database.cpp lines 53-57)
// ============================================================================

TEST_F(DatabaseTest, Open_WithWALRecovery) {
	// Test open path where WAL recovery might be needed
	// First open creates WAL
	{
		auto db1 = std::make_unique<Database>(testDbPath.string());
		db1->open();
		auto txn = db1->beginTransaction();
		// Don't commit - just let it close
	}
	// Re-open should trigger WAL recovery check (line 53)
	{
		auto db2 = std::make_unique<Database>(testDbPath.string());
		EXPECT_NO_THROW(db2->open());
		EXPECT_TRUE(db2->isOpen());
	}
}

// ============================================================================
// BeginTransaction flush path (Database.cpp lines 101-106)
// ============================================================================

TEST_F(DatabaseTest, BeginTransaction_FlushesBeforeStart) {
	// Test that beginTransaction flushes pending changes (line 101)
	auto db = std::make_unique<Database>(testDbPath.string());
	db->open();

	// Add data through the query engine
	auto engine = db->getQueryEngine();
	ASSERT_NE(engine, nullptr);
	engine->execute("CREATE (n:FlushTest {val: 1})");

	// beginTransaction should flush before creating transaction
	auto txn = db->beginTransaction();
	EXPECT_TRUE(db->isOpen());
}

// ============================================================================
// GetStorage Tests
// ============================================================================

TEST_F(DatabaseTest, GetStorage_AfterOpen) {
	auto db = std::make_unique<Database>(testDbPath.string());
	db->open();
	auto storage = db->getStorage();
	EXPECT_NE(storage, nullptr);
	EXPECT_TRUE(storage->isOpen());
}

TEST_F(DatabaseTest, GetStorage_BeforeOpen) {
	auto db = std::make_unique<Database>(testDbPath.string());
	auto storage = db->getStorage();
	// Storage object exists but file isn't open
	EXPECT_NE(storage, nullptr);
	EXPECT_FALSE(storage->isOpen());
}

// ============================================================================
// Thread pool management (Database.cpp lines 107-118, 171-175)
// ============================================================================

TEST_F(DatabaseTest, SetThreadPoolSize) {
	auto db = std::make_unique<Database>(testDbPath.string());
	db->open();

	// Initialize query engine first so all subsystems exist
	auto engine = db->getQueryEngine();
	ASSERT_NE(engine, nullptr);

	// Should not throw — wires thread pool to storage + query engine + VIM
	EXPECT_NO_THROW(db->setThreadPoolSize(4));
}

TEST_F(DatabaseTest, SetThreadPoolSize_WithoutQueryEngine) {
	auto db = std::make_unique<Database>(testDbPath.string());
	db->open();

	// Do NOT call getQueryEngine() — queryEngine is null
	// setThreadPoolSize should still work (the if(queryEngine) branch is false)
	EXPECT_NO_THROW(db->setThreadPoolSize(2));
}

TEST_F(DatabaseTest, GetThreadPool_BeforeOpen) {
	auto db = std::make_unique<Database>(testDbPath.string());
	EXPECT_FALSE(db->isOpen());

	auto tp = db->getThreadPool();
	EXPECT_EQ(tp, nullptr);
}

TEST_F(DatabaseTest, GetThreadPool_AfterOpen) {
	auto db = std::make_unique<Database>(testDbPath.string());
	db->open();

	auto tp = db->getThreadPool();
	EXPECT_NE(tp, nullptr);
}

// ============================================================================
// Close with WAL manager initialized via beginTransaction
// ============================================================================

TEST_F(DatabaseTest, Close_AfterBeginTransaction_WALInitialized) {
	// Ensure close() hits the walManager_->close() path (line 77)
	auto db = std::make_unique<Database>(testDbPath.string());
	db->open();

	{
		auto txn = db->beginTransaction();
		// walManager_ is now initialized
	}

	// close() should call walManager_->close()
	EXPECT_NO_THROW(db->close());
	EXPECT_FALSE(db->isOpen());
}

// ============================================================================
// WAL deferred initialization: ensureWALForWrites skips when WAL already
// exists from recovery
// ============================================================================

TEST_F(DatabaseTest, WALRecoveryThenWriteTransactionReusesExistingWAL) {
	std::string walPath = testDbPath.string() + "-wal";

	// Phase 1: Create a database with a WAL file left behind (simulate crash).
	// Since close() now deletes the WAL, we read the WAL content before the
	// destructor runs, then restore it to simulate an unclean shutdown.
	{
		auto db1 = std::make_unique<Database>(testDbPath.string());
		db1->open();
		{
			auto txn = db1->beginTransaction();
			// Don't commit — destructor auto-rolls back, but we want the WAL
			// to persist to simulate a crash.
		}

		// Read WAL content before db1 goes out of scope (destructor calls close
		// which deletes the WAL)
		std::vector<uint8_t> walContent;
		{
			std::ifstream walIn(walPath, std::ios::binary);
			if (walIn.is_open()) {
				walContent.assign(std::istreambuf_iterator<char>(walIn),
								  std::istreambuf_iterator<char>());
			}
		}

		// Let db1 destruct (this deletes the WAL)
		db1.reset();

		// Restore the WAL to simulate a process crash (no clean shutdown)
		if (!walContent.empty()) {
			std::ofstream walOut(walPath, std::ios::binary | std::ios::trunc);
			walOut.write(reinterpret_cast<const char *>(walContent.data()),
						 static_cast<std::streamsize>(walContent.size()));
		}
	}
	EXPECT_TRUE(fs::exists(walPath));

	// Phase 2: Reopen — ensureWALAndTransactionManager detects WAL, opens it for recovery.
	// Then beginTransaction calls ensureWALForWrites — should early-return since
	// walManager_ was already created during recovery.
	{
		auto db2 = std::make_unique<Database>(testDbPath.string());
		db2->open();

		// First write transaction: WAL already exists from recovery path
		auto txn = db2->beginTransaction();
		EXPECT_TRUE(txn.isActive());
		EXPECT_TRUE(fs::exists(walPath));
		txn.commit();

		db2->close();
	}
}

TEST_F(DatabaseTest, BeginReadOnlyTransaction_AutoOpensClosedDb) {
	auto db = std::make_unique<Database>(testDbPath.string());
	db->open();
	db->close();
	EXPECT_FALSE(db->isOpen());

	auto txn = db->beginReadOnlyTransaction();
	EXPECT_TRUE(db->isOpen());
	EXPECT_TRUE(txn.isReadOnly());
	txn.commit();
}

TEST_F(DatabaseTest, ReadOnlyTransactionDoesNotCreateWALFile) {
	std::string walPath = testDbPath.string() + "-wal";

	auto db = std::make_unique<Database>(testDbPath.string());
	db->open();
	EXPECT_FALSE(fs::exists(walPath));

	// Multiple read-only transactions should not create a WAL file
	for (int i = 0; i < 3; ++i) {
		auto roTxn = db->beginReadOnlyTransaction();
		EXPECT_TRUE(roTxn.isReadOnly());
		roTxn.commit();
	}

	EXPECT_FALSE(fs::exists(walPath));
	db->close();
}
