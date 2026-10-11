/**
 * @file test_NativeDatabaseLifecycle.cpp
 * @brief Native Database lifecycle contracts independent of the retired storage stack.
 */

#include <chrono>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>
#include <string>

#include "graph/core/Database.hpp"
#include "graph/query/api/QueryEngine.hpp"

namespace fs = std::filesystem;

namespace {

class NativeDatabaseLifecycleTest : public ::testing::Test {
protected:
	void SetUp() override {
		const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
		path_ = fs::temp_directory_path() / ("zyx_native_database_lifecycle_" + std::to_string(tick) + ".zyx");
		std::error_code error;
		fs::remove(path_, error);
	}

	void TearDown() override {
		std::error_code error;
		fs::remove(path_, error);
	}

	fs::path path_;
};

} // namespace

TEST_F(NativeDatabaseLifecycleTest, ClosedDatabaseAccessorsAndFlushFailSafe) {
	graph::Database database(path_.string());

	EXPECT_FALSE(database.isOpen());
	EXPECT_FALSE(database.hasActiveTransaction());
	EXPECT_EQ(database.getQueryEngine(), nullptr);
	EXPECT_EQ(database.getThreadPool(), nullptr);
	EXPECT_NO_THROW(database.flush());
	EXPECT_NO_THROW(database.close());
}

TEST_F(NativeDatabaseLifecycleTest, OpenCloseAndOpenIfExistsAreIdempotent) {
	graph::Database database(path_.string());
	EXPECT_FALSE(database.openIfExists());

	database.open();
	EXPECT_TRUE(database.isOpen());
	EXPECT_TRUE(database.openIfExists());
	EXPECT_NO_THROW(database.open());

	database.close();
	EXPECT_FALSE(database.isOpen());
	EXPECT_NO_THROW(database.close());

	// Reopening must reuse the already selected native backend rather than
	// losing the format-specific lifecycle state kept by that backend.
	database.open();
	EXPECT_TRUE(database.isOpen());
	EXPECT_NE(database.getLogicalGraphWriteBackend(), nullptr);
	database.flush();
}

TEST_F(NativeDatabaseLifecycleTest, OpenIfExistsRejectsAnInvalidNativeFile) {
	{
		std::ofstream corrupted(path_, std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(corrupted.is_open());
		corrupted << "not a ZYX native storage file";
	}

	graph::Database database(path_.string(), graph::storage::OpenMode::OPEN_EXISTING_FILE);
	EXPECT_TRUE(database.exists());
	EXPECT_FALSE(database.openIfExists());
	EXPECT_FALSE(database.isOpen());
}

TEST_F(NativeDatabaseLifecycleTest, TransactionsAutoOpenTheNativeBackend) {
	{
		graph::Database database(path_.string());
		auto transaction = database.beginTransaction();
		EXPECT_TRUE(database.isOpen());
		EXPECT_TRUE(database.hasActiveTransaction());
		transaction.rollback();
		EXPECT_FALSE(database.hasActiveTransaction());
	}

	{
		graph::Database database(path_.string());
		auto transaction = database.beginBulkTransaction();
		EXPECT_TRUE(database.isOpen());
		EXPECT_FALSE(transaction.isReadOnly());
		transaction.commit();
	}

	{
		graph::Database database(path_.string());
		auto transaction = database.beginReadOnlyTransaction();
		EXPECT_TRUE(database.isOpen());
		EXPECT_TRUE(transaction.isReadOnly());
		transaction.commit();
	}

	{
		graph::Database database(path_.string());
		database.open();
		auto transaction = database.beginTransaction();
		EXPECT_TRUE(transaction.isActive());
		transaction.commit();

		auto readOnly = database.beginReadOnlyTransaction();
		EXPECT_TRUE(readOnly.isReadOnly());
		readOnly.commit();
	}
}

TEST_F(NativeDatabaseLifecycleTest, NativeTransactionsNeverCreateTheRetiredSidecarWal) {
	const fs::path legacyWalPath = path_.string() + "-wal";
	graph::Database database(path_.string());
	database.open();
	EXPECT_FALSE(fs::exists(legacyWalPath));

	// Native durability is published through the single-file generation path.
	// Neither transaction mode may revive the legacy sidecar WAL contract.
	auto reader = database.beginReadOnlyTransaction();
	reader.commit();
	EXPECT_FALSE(fs::exists(legacyWalPath));

	auto writer = database.beginTransaction();
	writer.commit();
	EXPECT_FALSE(fs::exists(legacyWalPath));
}

TEST_F(NativeDatabaseLifecycleTest, CloseRejectsActiveTransactionWithoutInvalidatingItsOverlay) {
	graph::Database database(path_.string());
	database.open();
	auto engine = database.getQueryEngine();
	ASSERT_NE(engine, nullptr);
	auto transaction = database.beginTransaction();

	EXPECT_TRUE(transaction.execute(*engine, "CREATE (:CloseProtectedTransaction)").isEmpty());
	EXPECT_THROW(database.close(), std::logic_error);
	EXPECT_TRUE(database.isOpen());
	EXPECT_TRUE(transaction.isActive());
	EXPECT_TRUE(database.hasActiveTransaction());
	EXPECT_EQ(transaction.execute(*engine, "MATCH (:CloseProtectedTransaction) RETURN 1").rowCount(), 1U);

	transaction.commit();
	EXPECT_FALSE(database.hasActiveTransaction());
	EXPECT_NO_THROW(database.close());

	database.open();
	engine = database.getQueryEngine();
	ASSERT_NE(engine, nullptr);
	EXPECT_EQ(engine->execute("MATCH (:CloseProtectedTransaction) RETURN 1").rowCount(), 1U);
	database.close();
}

TEST_F(NativeDatabaseLifecycleTest, CloseRejectsReadOnlyTransactionWithoutDiscardingEphemeralState) {
	graph::Database database(path_.string());
	database.open();
	auto engine = database.getQueryEngine();
	ASSERT_NE(engine, nullptr);
	static_cast<void>(engine->execute("CREATE (:ReadOnlyCloseProtected)"));
	static_cast<void>(engine->execute("CALL gds.graph.project('close_rejection_projection', {})"));
	auto transaction = database.beginReadOnlyTransaction();
	EXPECT_FALSE(database.hasActiveTransaction());

	// Read-only transactions do not reserve the public write-conflict slot, but
	// they still own a snapshot and must prevent storage teardown.
	EXPECT_THROW(database.close(), std::logic_error);
	EXPECT_TRUE(database.isOpen());
	EXPECT_TRUE(transaction.isActive());
	EXPECT_EQ(transaction.execute(*engine, "MATCH (:ReadOnlyCloseProtected) RETURN 1").rowCount(), 1U);
	EXPECT_EQ(engine->execute("CALL gds.graph.exists('close_rejection_projection')")
				  .getRows()
				  .front()
				  .at("exists")
				  .asPrimitive(),
			  graph::PropertyValue(true));

	transaction.commit();
	EXPECT_NO_THROW(database.close());
	database.open();
	engine = database.getQueryEngine();
	ASSERT_NE(engine, nullptr);
	EXPECT_EQ(engine->execute("CALL gds.graph.exists('close_rejection_projection')")
				  .getRows()
				  .front()
				  .at("exists")
				  .asPrimitive(),
			  graph::PropertyValue(false));
	database.close();
}

TEST_F(NativeDatabaseLifecycleTest, ActiveTransactionKeepsNativeBackendAliveAfterDatabaseWrapperDestruction) {
	auto database = std::make_unique<graph::Database>(path_.string());
	database->open();
	auto transaction = std::make_unique<graph::Transaction>(database->beginReadOnlyTransaction());
	ASSERT_TRUE(transaction->isActive());

	// Database destruction cannot close a backend that still has this transaction's
	// pinned snapshot. The transaction must keep the backend alive long enough to
	// release that snapshot rather than retaining a dangling backend reference.
	database.reset();
	// Destruction runs the same rollback path without access to the destroyed
	// wrapper, so it verifies the backend anchor's member-destruction ordering.
	transaction.reset();

	graph::Database reopened(path_.string(), graph::storage::OpenMode::OPEN_EXISTING_FILE);
	EXPECT_NO_THROW(reopened.open());
	EXPECT_TRUE(reopened.isOpen());
	EXPECT_NO_THROW(reopened.close());
}

TEST_F(NativeDatabaseLifecycleTest, ActiveWriteTransactionCanFinishAfterDatabaseWrapperDestruction) {
	auto database = std::make_unique<graph::Database>(path_.string());
	database->open();
	auto transaction = std::make_unique<graph::Transaction>(database->beginTransaction());
	ASSERT_TRUE(transaction->isActive());
	EXPECT_TRUE(database->hasActiveTransaction());

	// A write transaction owns the same backend lifetime boundary even when it
	// has no QueryEngine work left to perform after the wrapper is destroyed.
	database.reset();
	EXPECT_NO_THROW(transaction->commit());
	EXPECT_FALSE(transaction->isActive());
	transaction.reset();

	graph::Database reopened(path_.string(), graph::storage::OpenMode::OPEN_EXISTING_FILE);
	EXPECT_NO_THROW(reopened.open());
	EXPECT_TRUE(reopened.isOpen());
	EXPECT_NO_THROW(reopened.close());
}

TEST_F(NativeDatabaseLifecycleTest, TransactionRejectsAQueryEngineFromAnotherDatabase) {
	const fs::path otherPath = path_.parent_path() / (path_.stem().string() + "_other.zyx");
	std::error_code error;
	fs::remove(otherPath, error);

	graph::Database database(path_.string());
	graph::Database otherDatabase(otherPath.string());
	database.open();
	otherDatabase.open();
	auto engine = database.getQueryEngine();
	auto otherEngine = otherDatabase.getQueryEngine();
	ASSERT_NE(engine, nullptr);
	ASSERT_NE(otherEngine, nullptr);
	auto transaction = database.beginTransaction();

	// A foreign engine has its own catalog and resource policy. It must never
	// execute against this transaction's overlay, even for a nominally read-only
	// statement, because that would combine two Database ownership domains.
	EXPECT_THROW(static_cast<void>(transaction.execute(*otherEngine, "RETURN 1")), std::invalid_argument);
	EXPECT_TRUE(transaction.isActive());
	EXPECT_TRUE(database.hasActiveTransaction());
	EXPECT_EQ(otherEngine->execute("MATCH (:CrossDatabaseTransaction) RETURN 1").rowCount(), 0U);

	EXPECT_NO_THROW(static_cast<void>(transaction.execute(*engine, "CREATE (:CrossDatabaseTransaction)")));
	transaction.commit();
	EXPECT_EQ(engine->execute("MATCH (:CrossDatabaseTransaction) RETURN 1").rowCount(), 1U);
	EXPECT_EQ(otherEngine->execute("MATCH (:CrossDatabaseTransaction) RETURN 1").rowCount(), 0U);

	database.close();
	otherDatabase.close();
	fs::remove(otherPath, error);
}

TEST_F(NativeDatabaseLifecycleTest, ThreadPoolConfigurationSurvivesEachLazyInitializationBoundary) {
	graph::Database database(path_.string());
	database.open();

	// Before lazy creation, retain the explicit setting without allocating workers.
	database.setThreadPoolSize(1U);
	auto pool = database.getThreadPool();
	ASSERT_NE(pool, nullptr);
	EXPECT_EQ(pool->getThreadCount(), 1U);

	// Replacing an existing pool must also work before the QueryEngine exists.
	database.setThreadPoolSize(2U);
	pool = database.getThreadPool();
	ASSERT_NE(pool, nullptr);
	EXPECT_EQ(pool->getThreadCount(), 2U);

	// Once the engine exists, replacement keeps both lazy services usable.
	ASSERT_NE(database.getQueryEngine(), nullptr);
	database.setThreadPoolSize(1U);
	EXPECT_EQ(database.getThreadPool()->getThreadCount(), 1U);
}

TEST_F(NativeDatabaseLifecycleTest, DefaultThreadPoolUsesThePortableAutoDetectionPolicy) {
	graph::Database database(path_.string());
	database.open();

	auto pool = database.getThreadPool();
	ASSERT_NE(pool, nullptr);
	EXPECT_EQ(pool->getHardwareInfo().requestedThreadCount, 0U);
	EXPECT_GE(pool->getHardwareInfo().resolvedThreadCount, 1U);
}
