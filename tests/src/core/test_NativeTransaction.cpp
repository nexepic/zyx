/**
 * @file test_NativeTransaction.cpp
 * @brief Public-behavior tests for the core native logical transaction owner.
 */

#include <chrono>
#include <atomic>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "graph/core/Database.hpp"
#include "graph/query/api/QueryEngine.hpp"
#include "graph/query/api/QueryResult.hpp"
#include "graph/storage/PwriteHelper.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/format/Format.hpp"

namespace graph {
namespace {

class NativeTransactionTest : public ::testing::Test {
protected:
	void SetUp() override {
		const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
		path_ = std::filesystem::temp_directory_path() / ("zyx_native_transaction_" + std::to_string(nonce) + ".zyx");
		std::error_code error;
		std::filesystem::remove(path_, error);
		database_ = std::make_unique<Database>(path_.string(), storage::OpenMode::OPEN_CREATE_NEW_FILE);
		database_->open();
		engine_ = database_->getQueryEngine();
		ASSERT_NE(engine_, nullptr);
	}

	void TearDown() override {
		if (database_) {
			database_->close();
		}
		std::error_code error;
		std::filesystem::remove(path_, error);
	}

	std::filesystem::path path_;
	std::unique_ptr<Database> database_;
	std::shared_ptr<query::QueryEngine> engine_;
};

TEST_F(NativeTransactionTest, WriteTransactionCommitsReadYourWritesAndRejectsReuse) {
	auto transaction = database_->beginTransaction();
	ASSERT_TRUE(transaction.isActive());
	EXPECT_FALSE(transaction.isReadOnly());
	EXPECT_TRUE(database_->hasActiveTransaction());

	EXPECT_NO_THROW(static_cast<void>(transaction.execute(*engine_, "CREATE (:CoreTxn {name: 'committed'})")));
	auto staged = transaction.execute(*engine_, "MATCH (n:CoreTxn) RETURN n.name");
	ASSERT_EQ(staged.rowCount(), 1U);

	transaction.commit();
	EXPECT_EQ(transaction.getState(), Transaction::TxnState::TXN_COMMITTED);
	EXPECT_FALSE(transaction.isActive());
	EXPECT_FALSE(database_->hasActiveTransaction());
	EXPECT_THROW(transaction.commit(), std::runtime_error);
	EXPECT_THROW(transaction.rollback(), std::runtime_error);
	EXPECT_THROW(static_cast<void>(transaction.execute(*engine_, "RETURN 1")), std::runtime_error);

	auto committed = engine_->execute("MATCH (n:CoreTxn) RETURN n.name");
	EXPECT_EQ(committed.rowCount(), 1U);
}

TEST_F(NativeTransactionTest, ActiveTransactionRollsBackOnDestruction) {
	{
		auto transaction = database_->beginTransaction();
		ASSERT_NO_THROW(static_cast<void>(transaction.execute(*engine_, "CREATE (:CoreTxn {name: 'rolled back'})")));
		EXPECT_TRUE(database_->hasActiveTransaction());
	}

	EXPECT_FALSE(database_->hasActiveTransaction());
	auto result = engine_->execute("MATCH (n:CoreTxn) RETURN n.name");
	EXPECT_EQ(result.rowCount(), 0U);
}

TEST_F(NativeTransactionTest, ExplicitCommitPublishesCompoundGraphStateAcrossReopen) {
	auto transaction = database_->beginTransaction();
	ASSERT_NO_THROW(static_cast<void>(transaction.execute(
			*engine_,
			"CREATE (source:CommitRecovered {name: 'source'})-[:COMMIT_LINK {weight: 11}]->"
			"(target:CommitRecovered {name: 'target'})")));
	EXPECT_EQ(transaction.execute(
				*engine_,
				"MATCH (source:CommitRecovered {name: 'source'})-[:COMMIT_LINK {weight: 11}]->"
				"(target:CommitRecovered {name: 'target'}) RETURN source")
				  .rowCount(),
			  1U);
	EXPECT_EQ(engine_->execute("MATCH (:CommitRecovered) RETURN 1").rowCount(), 0U);

	transaction.commit();
	EXPECT_EQ(engine_->execute(
				"MATCH (source:CommitRecovered {name: 'source'})-[:COMMIT_LINK {weight: 11}]->"
				"(target:CommitRecovered {name: 'target'}) RETURN source")
				  .rowCount(),
			  1U);

	database_->close();
	database_->open();
	engine_ = database_->getQueryEngine();
	ASSERT_NE(engine_, nullptr);
	EXPECT_EQ(engine_->execute(
				"MATCH (source:CommitRecovered {name: 'source'})-[:COMMIT_LINK {weight: 11}]->"
				"(target:CommitRecovered {name: 'target'}) RETURN source")
				  .rowCount(),
			  1U);
}

TEST_F(NativeTransactionTest, ExplicitCommitPersistsCompoundPropertyReplacementsAcrossReopen) {
	static_cast<void>(engine_->execute(
			"CREATE (source:UpdateRecoveredSource {version: 1, obsolete: 9})-[:UPDATE_LINK {weight: 3, temporary: true}]->"
			"(target:UpdateRecoveredTarget {version: 1})"));

	auto transaction = database_->beginTransaction();
	ASSERT_NO_THROW(static_cast<void>(transaction.execute(
			*engine_, "MATCH (source:UpdateRecoveredSource {version: 1}) SET source.version = 2")));
	ASSERT_NO_THROW(static_cast<void>(transaction.execute(
			*engine_,
			"MATCH (source:UpdateRecoveredSource)-[relationship:UPDATE_LINK {weight: 3}]->"
			"(target:UpdateRecoveredTarget) "
			"SET relationship.weight = 17")));
	ASSERT_NO_THROW(static_cast<void>(transaction.execute(
			*engine_,
			"MATCH (source:UpdateRecoveredSource)-[relationship:UPDATE_LINK]->(target:UpdateRecoveredTarget) "
			"REMOVE source.obsolete, relationship.temporary")));
	const auto insideNode =
			transaction.execute(*engine_, "MATCH (source:UpdateRecoveredSource) RETURN source.version AS version");
	ASSERT_EQ(insideNode.rowCount(), 1U);
	EXPECT_EQ(insideNode.getRows().front().at("version").asPrimitive(), PropertyValue(int64_t(2)));
	const auto insideEdge = transaction.execute(
			*engine_, "MATCH ()-[relationship:UPDATE_LINK]->() RETURN relationship.weight AS weight");
	ASSERT_EQ(insideEdge.rowCount(), 1U);
	EXPECT_EQ(insideEdge.getRows().front().at("weight").asPrimitive(), PropertyValue(int64_t(17)));
	EXPECT_EQ(transaction.execute(*engine_, "MATCH (:UpdateRecoveredSource {obsolete: 9}) RETURN 1").rowCount(), 0U);
	EXPECT_EQ(transaction.execute(*engine_, "MATCH ()-[:UPDATE_LINK {temporary: true}]->() RETURN 1").rowCount(), 0U);
	EXPECT_EQ(engine_->execute(
				"MATCH (source:UpdateRecoveredSource {version: 2})-[:UPDATE_LINK {weight: 17}]->"
				"(target:UpdateRecoveredTarget {version: 1}) RETURN source")
				  .rowCount(),
			  0U);

	transaction.commit();
	database_->close();
	database_->open();
	engine_ = database_->getQueryEngine();
	ASSERT_NE(engine_, nullptr);
	EXPECT_EQ(engine_->execute(
				"MATCH (source:UpdateRecoveredSource {version: 2})-[:UPDATE_LINK {weight: 17}]->"
				"(target:UpdateRecoveredTarget {version: 1}) RETURN source")
				  .rowCount(),
			  1U);
	EXPECT_EQ(engine_->execute("MATCH (:UpdateRecoveredSource {obsolete: 9}) RETURN 1").rowCount(), 0U);
	EXPECT_EQ(engine_->execute("MATCH ()-[:UPDATE_LINK {temporary: true}]->() RETURN 1").rowCount(), 0U);
}

TEST_F(NativeTransactionTest, ExplicitCommitPersistsNodeAndEdgeDeletionAcrossReopen) {
	static_cast<void>(engine_->execute(
			"CREATE (source:DeleteRecoveredSource {name: 'discard'})-[:DELETE_LINK {weight: 5}]->"
			"(target:DeleteRecoveredTarget {name: 'retain'})"));

	auto transaction = database_->beginTransaction();
	ASSERT_NO_THROW(static_cast<void>(transaction.execute(
			*engine_,
			"MATCH (source:DeleteRecoveredSource)-[relationship:DELETE_LINK]->(target:DeleteRecoveredTarget) "
			"DELETE relationship, source")));
	EXPECT_EQ(transaction.execute(*engine_, "MATCH (:DeleteRecoveredSource) RETURN 1").rowCount(), 0U);
	EXPECT_EQ(transaction.execute(*engine_, "MATCH ()-[:DELETE_LINK]->() RETURN 1").rowCount(), 0U);
	EXPECT_EQ(transaction.execute(*engine_, "MATCH (:DeleteRecoveredTarget {name: 'retain'}) RETURN 1").rowCount(), 1U);
	EXPECT_EQ(engine_->execute("MATCH (:DeleteRecoveredSource) RETURN 1").rowCount(), 1U);
	EXPECT_EQ(engine_->execute("MATCH ()-[:DELETE_LINK]->() RETURN 1").rowCount(), 1U);

	transaction.commit();
	database_->close();
	database_->open();
	engine_ = database_->getQueryEngine();
	ASSERT_NE(engine_, nullptr);
	EXPECT_EQ(engine_->execute("MATCH (:DeleteRecoveredSource) RETURN 1").rowCount(), 0U);
	EXPECT_EQ(engine_->execute("MATCH ()-[:DELETE_LINK]->() RETURN 1").rowCount(), 0U);
	EXPECT_EQ(engine_->execute("MATCH (:DeleteRecoveredTarget {name: 'retain'}) RETURN 1").rowCount(), 1U);
}

TEST_F(NativeTransactionTest, InterleavedTransactionsPersistOnlyTheFinalCompoundGraphStateAcrossReopen) {
	{
		auto transaction = database_->beginTransaction();
		ASSERT_NO_THROW(static_cast<void>(transaction.execute(
				*engine_,
				"CREATE (first:CompoundFirst {name: 'first'})-[:COMPOUND_KEEP {role: 'survives'}]->"
				"(middle:CompoundMiddle {name: 'middle'})")));
		ASSERT_NO_THROW(static_cast<void>(transaction.execute(
				*engine_,
				"MATCH (middle:CompoundMiddle) CREATE (middle)-[:COMPOUND_DROP {role: 'deleted'}]->"
				"(:CompoundDeleted {name: 'deleted'})")));
		transaction.commit();
	}
	{
		auto transaction = database_->beginTransaction();
		ASSERT_NO_THROW(static_cast<void>(transaction.execute(*engine_, "MATCH ()-[edge:COMPOUND_DROP]->() DELETE edge")));
		transaction.commit();
	}
	{
		auto transaction = database_->beginTransaction();
		ASSERT_NO_THROW(static_cast<void>(transaction.execute(*engine_, "MATCH (node:CompoundDeleted) DELETE node")));
		transaction.commit();
	}
	{
		auto transaction = database_->beginTransaction();
		ASSERT_NO_THROW(static_cast<void>(transaction.execute(*engine_, "CREATE (:CompoundRolledBack {name: 'discard'})")));
		transaction.rollback();
	}
	{
		auto transaction = database_->beginTransaction();
		ASSERT_NO_THROW(static_cast<void>(transaction.execute(*engine_, "CREATE (:CompoundFinalCommit {name: 'final'})")));
		transaction.commit();
	}

	database_->close();
	database_->open();
	engine_ = database_->getQueryEngine();
	ASSERT_NE(engine_, nullptr);
	EXPECT_EQ(engine_->execute(
				"MATCH (first:CompoundFirst {name: 'first'})-[:COMPOUND_KEEP {role: 'survives'}]->"
				"(middle:CompoundMiddle {name: 'middle'}) RETURN first")
				  .rowCount(),
			  1U);
	EXPECT_EQ(engine_->execute("MATCH (:CompoundDeleted) RETURN 1").rowCount(), 0U);
	EXPECT_EQ(engine_->execute("MATCH ()-[:COMPOUND_DROP]->() RETURN 1").rowCount(), 0U);
	EXPECT_EQ(engine_->execute("MATCH (:CompoundRolledBack) RETURN 1").rowCount(), 0U);
	EXPECT_EQ(engine_->execute("MATCH (:CompoundFinalCommit {name: 'final'}) RETURN 1").rowCount(), 1U);
}

TEST_F(NativeTransactionTest, ExplicitRollbackNeverPublishesCompoundGraphStateAcrossReopen) {
	static_cast<void>(engine_->execute("CREATE (:RollbackSeed {name: 'durable'})"));

	auto transaction = database_->beginTransaction();
	ASSERT_NO_THROW(static_cast<void>(transaction.execute(
			*engine_,
			"CREATE (source:RollbackDiscarded {name: 'source'})-[:ROLLBACK_LINK {weight: 7}]->"
			"(target:RollbackDiscarded {name: 'target'})")));
	EXPECT_EQ(transaction.execute(*engine_, "MATCH (:RollbackDiscarded) RETURN 1").rowCount(), 2U);
	EXPECT_EQ(engine_->execute("MATCH (:RollbackDiscarded) RETURN 1").rowCount(), 0U);

	transaction.rollback();
	EXPECT_EQ(transaction.getState(), Transaction::TxnState::TXN_ROLLED_BACK);
	EXPECT_EQ(engine_->execute("MATCH (:RollbackDiscarded) RETURN 1").rowCount(), 0U);
	EXPECT_EQ(engine_->execute("MATCH ()-[:ROLLBACK_LINK]->() RETURN 1").rowCount(), 0U);

	// A native rollback has no sidecar replay record. Reopen must retain only
	// the previously published generation, including after a compound mutation.
	database_->close();
	database_->open();
	engine_ = database_->getQueryEngine();
	ASSERT_NE(engine_, nullptr);
	EXPECT_EQ(engine_->execute("MATCH (:RollbackSeed) RETURN 1").rowCount(), 1U);
	EXPECT_EQ(engine_->execute("MATCH (:RollbackDiscarded) RETURN 1").rowCount(), 0U);
	EXPECT_EQ(engine_->execute("MATCH ()-[:ROLLBACK_LINK]->() RETURN 1").rowCount(), 0U);
}

TEST_F(NativeTransactionTest, CommitWriteFailureFinalizesTransactionWithoutPublishingAndAllowsReopenRetry) {
	auto transaction = database_->beginTransaction();
	ASSERT_NO_THROW(static_cast<void>(transaction.execute(*engine_, "CREATE (:CommitWriteFailure {value: 1})")));

	std::atomic<bool> injected = false;
	{
		// A short full-page write models a failed candidate-page publication. The
		// transaction must become terminal rather than retaining an ambiguous
		// overlay after the storage layer rejects that write.
		storage::StorageIO::ScopedNativeWriteHookForTesting failOneCandidate(
				[&injected](storage::file_handle_t fd, const void *bytes, size_t byteCount,
							storage::pwrite_off_t offset) {
					if (byteCount == storage::RSS_PAGE_SIZE && !injected.exchange(true, std::memory_order_relaxed)) {
						return storage::portable_pwrite(fd, bytes, 16U, offset);
					}
					return storage::portable_pwrite(fd, bytes, byteCount, offset);
				});
		EXPECT_THROW(transaction.commit(), std::runtime_error);
	}

	EXPECT_TRUE(injected.load(std::memory_order_relaxed));
	EXPECT_EQ(transaction.getState(), Transaction::TxnState::TXN_ROLLED_BACK);
	EXPECT_FALSE(transaction.isActive());
	EXPECT_FALSE(database_->hasActiveTransaction());
	EXPECT_THROW(transaction.commit(), std::runtime_error);
	EXPECT_THROW(transaction.rollback(), std::runtime_error);

	EXPECT_EQ(engine_->execute("MATCH (:CommitWriteFailure) RETURN 1").rowCount(), 0U);
	database_->close();
	database_->open();
	engine_ = database_->getQueryEngine();
	ASSERT_NE(engine_, nullptr);
	EXPECT_EQ(engine_->execute("MATCH (:CommitWriteFailure) RETURN 1").rowCount(), 0U);
	EXPECT_NO_THROW(static_cast<void>(engine_->execute("CREATE (:CommitWriteFailureRetry {value: 2})")));
	EXPECT_EQ(engine_->execute("MATCH (:CommitWriteFailureRetry) RETURN 1").rowCount(), 1U);
}

TEST_F(NativeTransactionTest, FailedMutationDoesNotPublishPartialStatementIntoTheTransactionOverlay) {
	auto transaction = database_->beginTransaction();
	ASSERT_NO_THROW(static_cast<void>(transaction.execute(*engine_, "CREATE (:CoreTxn {name: 'stable'})")));

	// The first loop iteration can stage a Node, but the second one fails while
	// evaluating its Property. The statement must not publish a partial batch.
	EXPECT_THROW(static_cast<void>(transaction.execute(
				 *engine_, "FOREACH (value IN [1, 0] | CREATE (:CoreTxn {name: 'partial', quotient: 1 / value}))")),
			 std::runtime_error);
	EXPECT_TRUE(transaction.isActive());

	auto staged = transaction.execute(*engine_, "MATCH (n:CoreTxn) RETURN n.name ORDER BY n.name");
	ASSERT_EQ(staged.rowCount(), 1U);
	EXPECT_EQ(staged.getRows().front().at("n.name").asPrimitive(), PropertyValue("stable"));

	// A failed statement does not poison an explicit transaction or discard its
	// previously staged work; the caller retains the normal commit decision.
	transaction.commit();
	auto committed = engine_->execute("MATCH (n:CoreTxn) RETURN n.name ORDER BY n.name");
	ASSERT_EQ(committed.rowCount(), 1U);
	EXPECT_EQ(committed.getRows().front().at("n.name").asPrimitive(), PropertyValue("stable"));
}

TEST_F(NativeTransactionTest, ForeachDeleteIsVisibleToItsTransactionAndSurvivesReopen) {
	static_cast<void>(engine_->execute("CREATE (:ForeachDelete {name: 'temporary'})"));

	auto transaction = database_->beginTransaction();
	EXPECT_NO_THROW(static_cast<void>(transaction.execute(
			*engine_, "MATCH (node:ForeachDelete) FOREACH (value IN [1] | DELETE node)")));

	// The write is immediately visible through the transaction-owned native
	// overlay, before its single durable publication at commit.
	EXPECT_EQ(transaction.execute(*engine_, "MATCH (node:ForeachDelete) RETURN node").rowCount(), 0U);
	EXPECT_EQ(engine_->execute("MATCH (node:ForeachDelete) RETURN node").rowCount(), 1U);

	transaction.commit();
	EXPECT_EQ(engine_->execute("MATCH (node:ForeachDelete) RETURN node").rowCount(), 0U);

	database_->close();
	database_->open();
	engine_ = database_->getQueryEngine();
	ASSERT_NE(engine_, nullptr);
	EXPECT_EQ(engine_->execute("MATCH (node:ForeachDelete) RETURN node").rowCount(), 0U);
}

TEST_F(NativeTransactionTest, ForeachDetachDeleteCancelsEdgesAttachedToCreatedNodes) {
	EXPECT_NO_THROW(static_cast<void>(engine_->execute(
			"FOREACH (value IN [1] | CREATE (source:ForeachDetach)-[relationship:LINK]->"
			"(target:ForeachDetach) DETACH DELETE source)")));

	EXPECT_EQ(engine_->execute("MATCH (node:ForeachDetach) RETURN node").rowCount(), 1U);
	EXPECT_EQ(engine_->execute("MATCH ()-[relationship:LINK]->() RETURN relationship").rowCount(), 0U);
}

TEST_F(NativeTransactionTest, ForeachDeleteCancelsAStagedRelationshipBeforeItsEndpoint) {
	EXPECT_NO_THROW(static_cast<void>(engine_->execute(
			"FOREACH (value IN [1] | CREATE (source:ForeachDeleteOrder)-[relationship:LINK]->"
			"(target:ForeachDeleteOrder) DELETE relationship, source)")));

	EXPECT_EQ(engine_->execute("MATCH (node:ForeachDeleteOrder) RETURN node").rowCount(), 1U);
	EXPECT_EQ(engine_->execute("MATCH ()-[relationship:LINK]->() RETURN relationship").rowCount(), 0U);
}

TEST_F(NativeTransactionTest, ForeachDeleteOfAConnectedCreatedNodeFailsAtomically) {
	EXPECT_THROW(static_cast<void>(engine_->execute(
			"FOREACH (value IN [1] | CREATE (source:ForeachDeleteAtomic)-[relationship:LINK]->"
			"(target:ForeachDeleteAtomic) DELETE source)")),
			 std::logic_error);

	EXPECT_EQ(engine_->execute("MATCH (node:ForeachDeleteAtomic) RETURN node").rowCount(), 0U);
	EXPECT_EQ(engine_->execute("MATCH ()-[relationship:LINK]->() RETURN relationship").rowCount(), 0U);
}

TEST_F(NativeTransactionTest, InTransactionsSubqueryPublishesEachConfiguredBatchDurably) {
	for (int value = 0; value < 5; ++value) {
		ASSERT_NO_THROW(static_cast<void>(engine_->execute("CREATE (:BatchSource {value: " + std::to_string(value) + "})")));
	}

	ASSERT_NO_THROW(static_cast<void>(engine_->execute(
			"MATCH (source:BatchSource) CALL { WITH source CREATE (:BatchResult {value: source.value}) } "
			"IN TRANSACTIONS OF 2 ROWS RETURN source.value AS value")));
	auto created = engine_->execute("MATCH (result:BatchResult) RETURN result.value AS value ORDER BY value");
	ASSERT_EQ(created.rowCount(), 5U);
	for (size_t index = 0U; index < created.rowCount(); ++index) {
		EXPECT_EQ(created.getRows()[index].at("value").asPrimitive(), PropertyValue(static_cast<int64_t>(index)));
	}

	database_->close();
	database_->open();
	engine_ = database_->getQueryEngine();
	ASSERT_NE(engine_, nullptr);
	EXPECT_EQ(engine_->execute("MATCH (:BatchResult) RETURN 1").rowCount(), 5U);
}

TEST_F(NativeTransactionTest, InTransactionsSubqueryIsRejectedBeforeMutatingAnExplicitTransaction) {
	static_cast<void>(engine_->execute("CREATE (:BatchTransactionSource {value: 1})"));
	auto transaction = database_->beginTransaction();
	EXPECT_THROW(static_cast<void>(transaction.execute(
				 *engine_, "MATCH (source:BatchTransactionSource) CALL { WITH source CREATE (:BatchTransactionResult) } "
						  "IN TRANSACTIONS RETURN source")),
			 std::runtime_error);
	EXPECT_EQ(transaction.execute(*engine_, "MATCH (:BatchTransactionResult) RETURN 1").rowCount(), 0U);
	transaction.rollback();
	EXPECT_EQ(engine_->execute("MATCH (:BatchTransactionResult) RETURN 1").rowCount(), 0U);
}

TEST_F(NativeTransactionTest, ExplicitTransactionRejectsSchemaPublicationWithoutChangingTheCatalog) {
	auto transaction = database_->beginTransaction();

	// Schema DDL is auto-commit only. Letting it enter a caller-owned graph
	// overlay would publish a durable Catalog generation outside that caller's
	// commit/rollback boundary.
	EXPECT_THROW(static_cast<void>(transaction.execute(
				 *engine_, "CREATE INDEX transaction_local_schema FOR (n:TxnSchema) ON (n.name)")),
			 std::runtime_error);
	EXPECT_TRUE(transaction.isActive());
	EXPECT_EQ(engine_->execute("SHOW INDEXES").rowCount(), 0U);

	transaction.rollback();
	EXPECT_EQ(engine_->execute("SHOW INDEXES").rowCount(), 0U);
}

TEST_F(NativeTransactionTest, InTransactionsSubqueryPreflightsOuterResultsBeforeAnyDurableBatch) {
	static_cast<void>(engine_->execute("CREATE (:BatchProjectionSource {value: 1})"));
	static_cast<void>(engine_->execute("CREATE (:BatchProjectionSource {value: 2})"));

	EXPECT_THROW(static_cast<void>(engine_->execute(
				 "MATCH (source:BatchProjectionSource) "
				 "CALL { WITH source CREATE (:BatchProjectionResult {value: source.value}) } "
				 "IN TRANSACTIONS OF 1 ROW RETURN 1 / 0 AS invalid")),
			 std::runtime_error);
	EXPECT_EQ(engine_->execute("MATCH (:BatchProjectionResult) RETURN 1").rowCount(), 0U);

	database_->close();
	database_->open();
	engine_ = database_->getQueryEngine();
	ASSERT_NE(engine_, nullptr);
	EXPECT_EQ(engine_->execute("MATCH (:BatchProjectionResult) RETURN 1").rowCount(), 0U);
}

TEST_F(NativeTransactionTest, FailedCreateResultPipelineDoesNotPublishIntoTheSingleFileBackend) {
	EXPECT_THROW(static_cast<void>(engine_->execute(
				 "CREATE (:AtomicCreateResult {value: 1}) RETURN 1 / 0 AS invalidResult")),
			 std::runtime_error);
	EXPECT_EQ(engine_->execute("MATCH (:AtomicCreateResult) RETURN 1").rowCount(), 0U);

	database_->close();
	database_->open();
	engine_ = database_->getQueryEngine();
	ASSERT_NE(engine_, nullptr);
	EXPECT_EQ(engine_->execute("MATCH (:AtomicCreateResult) RETURN 1").rowCount(), 0U);
}

TEST_F(NativeTransactionTest, TransactionLocalGdsProjectionsRemainUsableAfterFailedStatementsAndNeverLeak) {
	auto transaction = database_->beginTransaction();
	ASSERT_NO_THROW(static_cast<void>(transaction.execute(*engine_, "CREATE (:Projected {name: 'local'})")));

	// A failed create result pipeline must abandon the prepared catalog mutation.
	// Retrying the same name proves the preparation released its catalog writer
	// fence instead of leaving a hidden reservation in the transaction catalog.
	EXPECT_THROW(static_cast<void>(transaction.execute(
				 *engine_, "CALL gds.graph.project('local_projection', {}) YIELD name RETURN 1 / 0 AS invalidResult")),
			 std::runtime_error);
	EXPECT_EQ(transaction.execute(*engine_, "CALL gds.graph.exists('local_projection')").getRows().front().at("exists").asPrimitive(),
			  PropertyValue(false));

	const auto projected = transaction.execute(*engine_, "CALL gds.graph.project('local_projection', {})");
	ASSERT_EQ(projected.rowCount(), 1U);
	EXPECT_EQ(projected.getRows().front().at("name").asPrimitive(), PropertyValue("local_projection"));

	// A failed statement must not discard a projection that was already prepared
	// and published to this explicit transaction's private catalog.
	EXPECT_THROW(static_cast<void>(transaction.execute(*engine_,
				 "CALL gds.graph.drop('local_projection') YIELD name RETURN 1 / 0 AS invalidResult")),
			 std::runtime_error);
	EXPECT_EQ(transaction.execute(*engine_, "CALL gds.graph.exists('local_projection')").getRows().front().at("exists").asPrimitive(),
			  PropertyValue(true));
	EXPECT_THROW(static_cast<void>(transaction.execute(*engine_,
				 "CALL gds.graph.dropAll() YIELD droppedCount RETURN 1 / 0 AS invalidResult")),
			 std::runtime_error);
	EXPECT_EQ(transaction.execute(*engine_, "CALL gds.graph.exists('local_projection')").getRows().front().at("exists").asPrimitive(),
			  PropertyValue(true));
	EXPECT_EQ(transaction.execute(*engine_, "CALL gds.pageRank.stream('local_projection')").rowCount(), 1U);

	// A transaction-local derived object has no durable Database-owned catalog
	// representation, before or after the graph transaction commits.
	EXPECT_EQ(engine_->execute("CALL gds.graph.exists('local_projection')").getRows().front().at("exists").asPrimitive(),
			  PropertyValue(false));
	transaction.commit();
	EXPECT_EQ(engine_->execute("CALL gds.graph.exists('local_projection')").getRows().front().at("exists").asPrimitive(),
			  PropertyValue(false));

	auto rollbackTransaction = database_->beginTransaction();
	ASSERT_EQ(rollbackTransaction.execute(*engine_, "CALL gds.graph.project('rollback_projection', {})").rowCount(), 1U);
	EXPECT_EQ(rollbackTransaction.execute(*engine_, "CALL gds.graph.exists('rollback_projection')")
				  .getRows()
				  .front()
				  .at("exists")
				  .asPrimitive(),
			  PropertyValue(true));
	rollbackTransaction.rollback();
	EXPECT_EQ(engine_->execute("CALL gds.graph.exists('rollback_projection')").getRows().front().at("exists").asPrimitive(),
			  PropertyValue(false));
}

TEST_F(NativeTransactionTest, MoveOperationsTransferNativeTransactionOwnership) {
	auto original = database_->beginTransaction();
	const uint64_t originalId = original.getId();
	auto moved = std::move(original);
	EXPECT_EQ(moved.getId(), originalId);
	EXPECT_TRUE(moved.isActive());
	EXPECT_FALSE(original.isActive());

	auto readOnly = database_->beginReadOnlyTransaction();
	ASSERT_TRUE(readOnly.isActive());
	ASSERT_TRUE(readOnly.isReadOnly());
	moved = std::move(readOnly);
	EXPECT_TRUE(moved.isActive());
	EXPECT_TRUE(moved.isReadOnly());
	EXPECT_FALSE(readOnly.isActive());
	EXPECT_FALSE(database_->hasActiveTransaction());

	// Self move is explicitly a no-op and must preserve the usable transaction.
	Transaction* const self = &moved;
	moved = std::move(*self);
	EXPECT_TRUE(moved.isActive());
	moved.rollback();
	EXPECT_EQ(moved.getState(), Transaction::TxnState::TXN_ROLLED_BACK);
}

TEST_F(NativeTransactionTest, MovedWriteTransactionCanCommitFromAnotherThread) {
	auto transaction = database_->beginTransaction();
	ASSERT_TRUE(transaction.isActive());

	std::exception_ptr commitFailure;
	std::thread commitThread([moved = std::move(transaction), &commitFailure]() mutable {
		try {
			moved.commit();
		} catch (...) {
			commitFailure = std::current_exception();
		}
	});
	commitThread.join();

	EXPECT_EQ(commitFailure, nullptr);
	EXPECT_FALSE(database_->hasActiveTransaction());
	// The original thread must observe the completed publication before it
	// dispatches another query through the shared native backend.
	EXPECT_NO_THROW(static_cast<void>(engine_->execute("RETURN 1")));
}

TEST_F(NativeTransactionTest, ReadOnlyTransactionExecutesReadsAndRejectsWrites) {
	{
		auto writer = database_->beginTransaction();
		ASSERT_NO_THROW(static_cast<void>(writer.execute(*engine_, "CREATE (:CoreTxn {name: 'visible'})")));
		writer.commit();
	}

	auto transaction = database_->beginReadOnlyTransaction();
	ASSERT_TRUE(transaction.isReadOnly());
	auto read = transaction.execute(*engine_, "MATCH (n:CoreTxn) RETURN n.name");
	EXPECT_EQ(read.rowCount(), 1U);
	EXPECT_THROW(static_cast<void>(transaction.execute(*engine_, "CREATE (:CoreTxn {name: 'forbidden'})")),
			 std::runtime_error);
	transaction.rollback();
}

TEST_F(NativeTransactionTest, ReadOnlyTransactionPinsItsStartingGenerationAcrossLaterPublication) {
	static_cast<void>(engine_->execute("CREATE (:PinnedSnapshot {value: 'before'})"));

	auto reader = database_->beginReadOnlyTransaction();
	ASSERT_TRUE(reader.isReadOnly());
	EXPECT_EQ(reader.execute(*engine_, "MATCH (:PinnedSnapshot) RETURN 1").rowCount(), 1U);

	// Publication after the read-only transaction begins must not change its
	// pinned native generation. A fresh database-scoped read observes the new
	// generation independently.
	static_cast<void>(engine_->execute("CREATE (:PinnedSnapshot {value: 'after'})"));
	EXPECT_EQ(reader.execute(*engine_, "MATCH (:PinnedSnapshot) RETURN 1").rowCount(), 1U);
	EXPECT_EQ(engine_->execute("MATCH (:PinnedSnapshot) RETURN 1").rowCount(), 2U);

	reader.commit();
}

} // namespace
} // namespace graph
