/**
 * @file test_RelocatableStorageBackend_TransactionAtomicity.cpp
 * @brief Statement-atomic staging contracts for native logical transactions.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "storage/relocatable/backend/RelocatableStorageBackendTestFixture.hpp"

#include "graph/storage/relocatable/allocation/SharedPagePool.hpp"
#include "graph/storage/relocatable/entity/NativeBulkEdgeRunStore.hpp"
#include "graph/storage/relocatable/format/Format.hpp"
#include "graph/storage/relocatable/format/SuperblockFile.hpp"
#include "graph/storage/relocatable/index/BTreePage.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <functional>
#include <future>
#include <span>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace graph::storage;
using namespace relocatable_storage_backend_test;


TEST_F(RelocatableStorageBackendTest, OrdinaryGraphMutationsPublishSnapshotPinnedCardinalityStatisticsAcrossReopen) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	EXPECT_EQ(backend.readLogicalGraphCardinalityStatistics(), std::nullopt);

	const std::vector<uint64_t> nodes = backend.createLogicalNodes({{{"Stats"}, {}}, {{"Stats"}, {}}});
	ASSERT_EQ(nodes.size(), 2U);
	EXPECT_EQ(backend.readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({2U, 0U}));

	const std::vector<uint64_t> edges = backend.createLogicalEdges({{nodes.front(), nodes.back(), "STATS_EDGE", {}}});
	ASSERT_EQ(edges.size(), 1U);
	EXPECT_EQ(backend.readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({2U, 1U}));

	backend.deleteLogicalNodes({{nodes.front(), true, 1U}});
	EXPECT_EQ(backend.readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({1U, 0U}));
	backend.deleteLogicalNodes({{nodes.back(), false, 0U}});
	EXPECT_EQ(backend.readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({0U, 0U}));
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();

	RelocatableStorageBackend reopened(backendOptions());
	reopened.open();
	EXPECT_EQ(reopened.readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({0U, 0U}));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest, FailedStatementDoesNotPoisonEarlierStagedLogicalTransactionState) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();

	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	LogicalGraphMutationBatch valid;
	valid.node_creates = {{{"Persists"}, {{"rank", PropertyValue(int64_t(7))}}}};
	const LogicalGraphMutationBatchResult staged = transaction->stageMutationBatch(std::move(valid));
	ASSERT_EQ(staged.created_node_ids.size(), 1U);

	// This statement obtains a separate provisional ID before its created-Node
	// reference is validated. Its rejection must leave the earlier statement
	// intact and the explicit transaction usable.
	LogicalGraphMutationBatch invalid;
	invalid.node_creates = {{{"Rejected"}, {}}};
	invalid.property_replacements = {
			{{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 1U},
			 {{"rank", PropertyValue(int64_t(9))}}}};
	EXPECT_THROW(static_cast<void>(transaction->stageMutationBatch(std::move(invalid))), std::invalid_argument);
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_ACTIVE);

	const LogicalGraphMutationBatchResult committed = transaction->commit();
	EXPECT_EQ(committed, staged);
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_COMMITTED);
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));

	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	const LogicalGraphNode persisted = snapshot->readNode(staged.created_node_ids.front());
	EXPECT_EQ(persisted.labels, (std::vector<std::string>{"Persists"}));
	EXPECT_EQ(persisted.properties, (std::vector<LogicalGraphPropertyEntry>{{"rank", PropertyValue(int64_t(7))}}));
	snapshot.reset();

	// The rejected statement had acquired a separate lease before validation.
	// Its ID remains a deliberate hole rather than being assigned to this later
	// successful write.
	const std::vector<uint64_t> afterFailure = backend.createLogicalNodes({{{"AfterRejectedStatement"}, {}}});
	ASSERT_EQ(afterFailure.size(), 1U);
	EXPECT_GT(afterFailure.front(), staged.created_node_ids.front() + 1U);
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, FailedPreviewedStatementRetainsOnlyItsLeaseForASameShapeRetry) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();

	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	LogicalGraphMutationBatch previewed;
	previewed.node_creates = {{{"PreviewSource"}, {}}, {{"PreviewTarget"}, {}}};
	previewed.edge_creates = {{
			{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 0U},
			{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 1U},
			"PREVIEW_LINK",
			{},
	}};
	const LogicalGraphMutationBatchResult preview = transaction->previewMutationBatch(previewed);
	ASSERT_EQ(preview.created_node_ids.size(), 2U);
	ASSERT_EQ(preview.created_edge_ids.size(), 1U);

	// The preview lease belongs to the transaction, but invalid statement content
	// must not become staged merely because it has the same creation shape.
	LogicalGraphMutationBatch invalid = previewed;
	invalid.property_replacements = {{
			{LogicalGraphMutationEntityKind::LGMK_EDGE, LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE, 1U},
			{{"weight", PropertyValue(int64_t(3))}},
	}};
	EXPECT_THROW(static_cast<void>(transaction->stageMutationBatch(std::move(invalid))), std::invalid_argument);
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_ACTIVE);

	// Correcting the statement reuses the exact preview IDs rather than silently
	// acquiring a second lease after the rejected attempt.
	previewed.property_replacements = {{
			{LogicalGraphMutationEntityKind::LGMK_EDGE, LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE, 0U},
			{{"weight", PropertyValue(int64_t(3))}},
	}};
	const LogicalGraphMutationBatchResult staged = transaction->stageMutationBatch(std::move(previewed));
	EXPECT_EQ(staged, preview);
	EXPECT_EQ(transaction->commit(), preview);
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));

	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	const LogicalGraphEdge edge = snapshot->readEdge(preview.created_edge_ids.front());
	EXPECT_EQ(edge.source_node_id, preview.created_node_ids.front());
	EXPECT_EQ(edge.target_node_id, preview.created_node_ids.back());
	EXPECT_EQ(edge.properties, (std::vector<LogicalGraphPropertyEntry>{{"weight", PropertyValue(int64_t(3))}}));
	snapshot.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, StreamedBulkEdgeStagingIsBoundedAndRollbackReleasesItsLease) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	options.maximum_bulk_edge_transaction_records = 8U;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes({{{"BulkSource"}, {}}, {{"BulkTarget"}, {}}});
	ASSERT_EQ(nodes.size(), 2U);

	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	auto *const bulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(transaction.get());
	ASSERT_NE(bulk, nullptr);
	const std::array<uint64_t, 2U> sources{nodes[0], nodes[0]};
	const std::array<uint64_t, 2U> targets{nodes[1], nodes[1]};
	const std::array<PropertyValue, 2U> weights{PropertyValue(int64_t(3)), PropertyValue(int64_t(5))};
	const std::array<LogicalGraphBulkEdgePropertyColumn, 1U> columns{{{"weight", weights}}};
	const LogicalGraphPersistedBulkEdgeAppendRequest request{"BULK_STAGED", sources, targets, columns};
	const std::vector<uint64_t> staged = bulk->appendPersistedBulkEdges(request);
	ASSERT_EQ(staged.size(), 2U);
	EXPECT_TRUE(bulk->hasStreamedBulkEdges());
	EXPECT_LT(staged.front(), staged.back());

	// Streamed edges must not be copied into the normal read-your-writes overlay.
	EXPECT_THROW(static_cast<void>(transaction->readSnapshot()), std::logic_error);
	LogicalGraphMutationBatch ordinary;
	ordinary.node_creates = {{{"MustNotMix"}, {}}};
	EXPECT_THROW(static_cast<void>(transaction->stageMutationBatch(std::move(ordinary))), std::logic_error);
	EXPECT_NO_THROW(transaction->rollback());
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_ROLLED_BACK);

	// Rollback abandons the in-memory lease capability. IDs remain durable holes,
	// so a later Edge can never publish one of the discarded staged identities.
	const std::vector<uint64_t> later = backend.createLogicalEdges({{nodes[0], nodes[1], "AFTER_BULK_ROLLBACK", {}}});
	ASSERT_EQ(later.size(), 1U);
	EXPECT_GT(later.front(), staged.back());
	// The active transaction retains its pinned metadata generation until it is
	// destroyed. Reopen performs the authoritative single-file scratch recovery
	// once that snapshot is released.
	transaction.reset();
	backend.close();
	RelocatableStorageBackend reopened(backendOptions());
	reopened.open();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest, ReadOnlyLogicalTransactionRejectsBulkAppendWithoutLeasingAnEdgeId) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes({{{"BulkSource"}, {}}, {{"BulkTarget"}, {}}});
	ASSERT_EQ(nodes.size(), 2U);

	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(true);
	auto *const bulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(transaction.get());
	ASSERT_NE(bulk, nullptr);
	const std::array<uint64_t, 1U> sources{nodes[0]};
	const std::array<uint64_t, 1U> targets{nodes[1]};

	// A rejected read-only append must happen before scratch allocation and durable
	// ID leasing, so a later writer retains the next canonical Edge identity.
	EXPECT_THROW(static_cast<void>(
				 bulk->appendPersistedBulkEdges({"BULK_READ_ONLY", sources, targets, {}})),
			 std::logic_error);
	EXPECT_FALSE(bulk->hasStreamedBulkEdges());
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_ACTIVE);
	EXPECT_NO_THROW(transaction->rollback());
	transaction.reset();

	const std::vector<uint64_t> edges = backend.createLogicalEdges({{nodes[0], nodes[1], "AFTER_READ_ONLY", {}}});
	ASSERT_EQ(edges.size(), 1U);
	EXPECT_EQ(edges.front(), 1U);
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, StreamedBulkEdgeStagingUsesBoundedIncrementalLeases) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	options.maximum_bulk_edge_transaction_records = 8U;
	options.bulk_edge_id_lease_block_records = 2U;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes({{{"BulkSource"}, {}}, {{"BulkTarget"}, {}}});
	ASSERT_EQ(nodes.size(), 2U);

	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	auto *const bulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(transaction.get());
	ASSERT_NE(bulk, nullptr);
	const std::array<uint64_t, 1U> sources{nodes[0]};
	const std::array<uint64_t, 1U> targets{nodes[1]};
	const LogicalGraphPersistedBulkEdgeAppendRequest request{"BULK_TINY", sources, targets, {}};

	std::vector<uint64_t> staged;
	staged.reserve(8U);
	for (size_t batch = 0U; batch < 8U; ++batch) {
		const std::vector<uint64_t> ids = bulk->appendPersistedBulkEdges(request);
		ASSERT_EQ(ids.size(), 1U);
		staged.push_back(ids.front());
	}
	EXPECT_TRUE(std::ranges::is_sorted(staged));
	EXPECT_EQ(std::adjacent_find(staged.begin(), staged.end(),
								 [](uint64_t left, uint64_t right) { return left + 1U != right; }),
			  staged.end());
	EXPECT_THROW(static_cast<void>(bulk->appendPersistedBulkEdges(request)), std::invalid_argument);

	transaction->rollback();
	transaction.reset();
	const std::vector<uint64_t> later = backend.createLogicalEdges({{nodes[0], nodes[1], "AFTER_TINY", {}}});
	ASSERT_EQ(later.size(), 1U);
	EXPECT_GT(later.front(), staged.back());
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, TinyStreamedBulkEdgeDoesNotBurnItsTransactionAdmissionLimit) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	options.maximum_bulk_edge_transaction_records = 1'000U;
	options.bulk_edge_id_lease_block_records = 2U;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes({{{"BulkSource"}, {}}, {{"BulkTarget"}, {}}});
	ASSERT_EQ(nodes.size(), 2U);

	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	auto *const bulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(transaction.get());
	ASSERT_NE(bulk, nullptr);
	const std::array<uint64_t, 1U> sources{nodes[0]};
	const std::array<uint64_t, 1U> targets{nodes[1]};
	const LogicalGraphPersistedBulkEdgeAppendRequest request{"BULK_TINY_LEASE", sources, targets, {}};
	const std::vector<uint64_t> staged = bulk->appendPersistedBulkEdges(request);
	ASSERT_EQ(staged.size(), 1U);
	transaction->rollback();
	transaction.reset();

	const std::vector<uint64_t> later = backend.createLogicalEdges({{nodes[0], nodes[1], "AFTER_TINY_LEASE", {}}});
	ASSERT_EQ(later.size(), 1U);
	EXPECT_EQ(later.front(), staged.front() + 2U);
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, StreamedBulkEdgeScratchRollbackDoesNotDependOnTailTruncation) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	options.maximum_bulk_edge_transaction_records = 2U;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes({{{"BulkSource"}, {}}, {{"BulkTarget"}, {}}});
	ASSERT_EQ(nodes.size(), 2U);

	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	auto *const bulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(transaction.get());
	ASSERT_NE(bulk, nullptr);
	const std::array<uint64_t, 2U> sources{nodes[0], nodes[0]};
	const std::array<uint64_t, 2U> targets{nodes[1], nodes[1]};
	// The second record flushes a scratch run. Allocator-managed scratch retirement
	// must remain independent from raw file-tail truncation.
	const std::string payload(RSS_SEGMENT_SIZE / 2U, 'x');
	const std::array<PropertyValue, 2U> values{PropertyValue(payload), PropertyValue(payload)};
	const std::array<LogicalGraphBulkEdgePropertyColumn, 1U> columns{{{"payload", values}}};
	const LogicalGraphPersistedBulkEdgeAppendRequest request{"BULK_SEALED_TAIL", sources, targets, columns};
	const std::vector<uint64_t> staged = bulk->appendPersistedBulkEdges(request);
	ASSERT_EQ(staged.size(), 2U);

	{
		StorageIO::ScopedNativeTruncateHookForTesting failTruncate([](file_handle_t, uint64_t) {
#ifdef _WIN32
			SetLastError(ERROR_DISK_FULL);
#else
			errno = ENOSPC;
#endif
			return -1;
		});
		EXPECT_NO_THROW(transaction->rollback());
	}
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_ROLLED_BACK);
	transaction.reset();

	const std::vector<uint64_t> later = backend.createLogicalEdges({{nodes[0], nodes[1], "AFTER_ROLLBACK", {}}});
	ASSERT_EQ(later.size(), 1U);
	EXPECT_GT(later.front(), staged.back());
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, StreamedBulkEdgeStagingDoesNotHoldTheGlobalPublicationGate) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	options.maximum_bulk_edge_transaction_records = 4U;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes({{{"BulkSource"}, {}}, {{"BulkTarget"}, {}}});
	ASSERT_EQ(nodes.size(), 2U);

	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	auto *const bulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(transaction.get());
	ASSERT_NE(bulk, nullptr);
	const std::array<uint64_t, 1U> sources{nodes[0]};
	const std::array<uint64_t, 1U> targets{nodes[1]};
	const LogicalGraphPersistedBulkEdgeAppendRequest request{"BULK_CONCURRENT", sources, targets, {}};
	ASSERT_EQ(bulk->appendPersistedBulkEdges(request).size(), 1U);

	auto competing = std::async(std::launch::async, [&backend] {
		return backend.createLogicalNodes({{{"ConcurrentDuringBulkStaging"}, {}}});
	});
	const std::future_status status = competing.wait_for(std::chrono::seconds(5));
	if (status != std::future_status::ready) {
		transaction->rollback();
	}
	ASSERT_EQ(status, std::future_status::ready);
	const std::vector<uint64_t> concurrent = competing.get();
	ASSERT_EQ(concurrent.size(), 1U);

	transaction->rollback();
	transaction.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, StreamedBulkEdgeWriterRefreshesItsCheckpointAcrossInterleavedPublications) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	options.maximum_bulk_edge_transaction_records = 4U;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes({{{"BulkSource"}, {}}, {{"BulkTarget"}, {}}});
	ASSERT_EQ(nodes.size(), 2U);

	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	auto *const bulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(transaction.get());
	ASSERT_NE(bulk, nullptr);
	const std::array<uint64_t, 1U> sources{nodes[0]};
	const std::array<uint64_t, 1U> targets{nodes[1]};
	const LogicalGraphPersistedBulkEdgeAppendRequest request{"BULK_INTERLEAVED", sources, targets, {}};
	const std::vector<uint64_t> first = bulk->appendPersistedBulkEdges(request);
	ASSERT_EQ(first.size(), 1U);

	const std::vector<uint64_t> concurrent = backend.createLogicalNodes({{{"PublishedBetweenBulkAppends"}, {}}});
	ASSERT_EQ(concurrent.size(), 1U);

	const std::vector<uint64_t> second = bulk->appendPersistedBulkEdges(request);
	ASSERT_EQ(second.size(), 1U);
	EXPECT_EQ(second.front(), first.front() + 1U);
	EXPECT_NO_THROW(transaction->rollback());
	transaction.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, StreamedBulkEdgeCommitPublishesAndSurvivesReopen) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes({{{"BulkSource"}, {}}, {{"BulkTarget"}, {}}});
	ASSERT_EQ(nodes.size(), 2U);
	EXPECT_EQ(backend.readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({2U, 0U}));
	std::unique_ptr<LogicalGraphReadSnapshot> beforeCommit = backend.openLogicalGraphReadSnapshot();

	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	auto *const bulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(transaction.get());
	ASSERT_NE(bulk, nullptr);
	const std::array<uint64_t, 1U> sources{nodes[0]};
	const std::array<uint64_t, 1U> targets{nodes[1]};
	const std::array<PropertyValue, 1U> weights{PropertyValue(int64_t(7))};
	const std::array<LogicalGraphBulkEdgePropertyColumn, 1U> columns{{{"weight", weights}}};
	const LogicalGraphPersistedBulkEdgeAppendRequest request{"BULK_COMMITTED", sources, targets, columns};
	const std::vector<uint64_t> staged = bulk->appendPersistedBulkEdges(request);
	ASSERT_EQ(staged.size(), 1U);

	EXPECT_NO_THROW(static_cast<void>(transaction->commit()));
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_COMMITTED);
	transaction.reset();
	EXPECT_THROW(static_cast<void>(beforeCommit->readEdge(staged.front())), FormatException);
	beforeCommit.reset();
	EXPECT_EQ(backend.readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({2U, 1U}));
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	const LogicalGraphEdge committed = snapshot->readEdge(staged.front());
	EXPECT_EQ(committed.source_node_id, nodes[0]);
	EXPECT_EQ(committed.target_node_id, nodes[1]);
	EXPECT_EQ(committed.type, "BULK_COMMITTED");
	EXPECT_EQ(committed.properties, (std::vector<LogicalGraphPropertyEntry>{{"weight", PropertyValue(int64_t(7))}}));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();

	RelocatableStorageBackend reopened(backendOptions());
	reopened.open();
	EXPECT_EQ(reopened.readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({2U, 1U}));
	snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(staged.front()), committed);
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest, StreamedBulkEdgeCommitPublishesInlinePropertiesAcrossPropertyPages) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	options.maximum_bulk_edge_transaction_records = 5U;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes({{{"BulkSource"}, {}}, {{"BulkTarget"}, {}}});
	ASSERT_EQ(nodes.size(), 2U);

	// A 1 KiB value keeps every root inline, but five roots cannot share one
	// 4 KiB Property page. This exercises the ordinary overflow-to-next-page
	// path independently from the LargeValue-root path.
	const std::string payload(1024U, 'p');
	const std::array<uint64_t, 5U> sources{nodes[0], nodes[0], nodes[0], nodes[0], nodes[0]};
	const std::array<uint64_t, 5U> targets{nodes[1], nodes[1], nodes[1], nodes[1], nodes[1]};
	const std::array<PropertyValue, 5U> payloads{PropertyValue(payload), PropertyValue(payload), PropertyValue(payload),
												 PropertyValue(payload), PropertyValue(payload)};
	const std::array<LogicalGraphBulkEdgePropertyColumn, 1U> columns{{{"payload", payloads}}};
	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	auto *const bulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(transaction.get());
	ASSERT_NE(bulk, nullptr);
	const std::vector<uint64_t> staged =
			bulk->appendPersistedBulkEdges({"BULK_PROPERTY_PAGES", sources, targets, columns});
	ASSERT_EQ(staged.size(), payloads.size());

	EXPECT_NO_THROW(static_cast<void>(transaction->commit()));
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_COMMITTED);
	transaction.reset();
	const auto assertProperties = [&](LogicalGraphReadSnapshot &snapshot) {
		for (const uint64_t edgeId: staged) {
			EXPECT_EQ(snapshot.readEdge(edgeId), LogicalGraphEdge(edgeId, nodes[0], nodes[1], "BULK_PROPERTY_PAGES",
																  {{"payload", PropertyValue(payload)}}));
		}
	};
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	assertProperties(*snapshot);
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();

	RelocatableStorageBackend reopened(backendOptions());
	reopened.open();
	snapshot = reopened.openLogicalGraphReadSnapshot();
	assertProperties(*snapshot);
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest, StreamedBulkEdgeShortPublicDataWriteRetainsOldGraphAndAllowsRetry) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes({{{"BulkSource"}, {}}, {{"BulkTarget"}, {}}});
	ASSERT_EQ(nodes.size(), 2U);

	const std::array<uint64_t, 1U> sources{nodes[0]};
	const std::array<uint64_t, 1U> targets{nodes[1]};
	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	auto *const bulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(transaction.get());
	ASSERT_NE(bulk, nullptr);
	const std::vector<uint64_t> rejected = bulk->appendPersistedBulkEdges({"BULK_SHORT_WRITE", sources, targets, {}});
	ASSERT_EQ(rejected.size(), 1U);

	std::atomic<bool> injected{false};
	{
		StorageIO::ScopedNativeWriteHookForTesting shortPublicEntityPage(
				[&injected](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (byteCount == RSS_PAGE_SIZE && !injected.load(std::memory_order_relaxed)) {
						try {
							const auto page =
									std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount);
							const PageOwnerHeader owner =
									PageOwnerHeaderCodec::decode(page.first(RSS_PAGE_OWNER_HEADER_BYTES));
							if (owner.kind == PageOwnerKind::POK_ENTITY) {
								injected.store(true, std::memory_order_relaxed);
								return portable_pwrite(fd, bytes, 16U, offset);
							}
						} catch (const std::exception &) {
							// Non-page writes are outside the public Entity-page failure point.
						}
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		EXPECT_THROW(static_cast<void>(transaction->commit()), std::system_error);
	}
	EXPECT_TRUE(injected.load(std::memory_order_relaxed));
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_ROLLED_BACK);
	transaction.reset();
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(rejected.front())), FormatException);
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));

	std::unique_ptr<LogicalGraphTransaction> retry = backend.beginLogicalGraphTransaction(false);
	auto *const retryBulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(retry.get());
	ASSERT_NE(retryBulk, nullptr);
	const std::vector<uint64_t> committed =
			retryBulk->appendPersistedBulkEdges({"BULK_SHORT_WRITE_RETRY", sources, targets, {}});
	ASSERT_EQ(committed.size(), 1U);
	EXPECT_NO_THROW(static_cast<void>(retry->commit()));
	retry.reset();
	snapshot = backend.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(committed.front()),
			  LogicalGraphEdge(committed.front(), nodes[0], nodes[1], "BULK_SHORT_WRITE_RETRY"));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, OrdinaryEdgeShortEntityWriteRetainsOldGraphAndAllowsRetryAfterReopen) {
	const NativeGraphFixture fixture = materializeNativeGraphFixture();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();

	std::atomic<bool> injected{false};
	{
		StorageIO::ScopedNativeWriteHookForTesting shortPublicEntityPage(
				[&injected](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (byteCount == RSS_PAGE_SIZE && !injected.load(std::memory_order_relaxed)) {
						try {
							const auto page = std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount);
							const PageOwnerHeader owner =
									PageOwnerHeaderCodec::decode(page.first(RSS_PAGE_OWNER_HEADER_BYTES));
							if (owner.kind == PageOwnerKind::POK_ENTITY) {
								injected.store(true, std::memory_order_relaxed);
								return portable_pwrite(fd, bytes, 16U, offset);
							}
						} catch (const std::exception &) {
							// Only an authenticated Entity page is this public fault point.
						}
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		EXPECT_THROW(static_cast<void>(backend.createLogicalEdges(
						{{fixture.person_node_id, fixture.unlabeled_node_id, "SHORT_ENTITY", {}}})),
					 std::system_error);
	}
	EXPECT_TRUE(injected.load(std::memory_order_relaxed));
	backend.close();

	RelocatableStorageBackendOptions reopenOptions = backendOptions();
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend reopened(std::move(reopenOptions));
	reopened.open();
	EXPECT_EQ(reopened.readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({2U, 2U}));
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = reopened.openLogicalGraphReadSnapshot();
	snapshot.reset();
	const std::vector<uint64_t> retried =
			reopened.createLogicalEdges({{fixture.person_node_id, fixture.unlabeled_node_id, "SHORT_ENTITY_RETRY", {}}});
	ASSERT_EQ(retried.size(), 1U);
	snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(retried.front()),
			  LogicalGraphEdge(retried.front(), fixture.person_node_id, fixture.unlabeled_node_id, "SHORT_ENTITY_RETRY"));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest, OrdinaryEdgeEntityDiskFullRetainsOldGraphAndAllowsRetryAfterReopen) {
	const NativeGraphFixture fixture = materializeNativeGraphFixture();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();

	std::atomic<bool> injected{false};
	{
		StorageIO::ScopedNativeWriteHookForTesting diskFullOnPublicEntityPage(
				[&injected](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (byteCount == RSS_PAGE_SIZE && !injected.load(std::memory_order_relaxed)) {
						try {
							const auto page = std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount);
							const PageOwnerHeader owner =
									PageOwnerHeaderCodec::decode(page.first(RSS_PAGE_OWNER_HEADER_BYTES));
							if (owner.kind == PageOwnerKind::POK_ENTITY) {
								injected.store(true, std::memory_order_relaxed);
#ifdef _WIN32
								SetLastError(ERROR_DISK_FULL);
#else
								errno = ENOSPC;
#endif
								return static_cast<pwrite_ssize_t>(-1);
							}
						} catch (const std::exception &) {
							// Only an authenticated Entity page is this public fault point.
						}
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		EXPECT_THROW(static_cast<void>(backend.createLogicalEdges(
						{{fixture.person_node_id, fixture.unlabeled_node_id, "DISK_FULL", {}}})),
					 std::system_error);
	}
	EXPECT_TRUE(injected.load(std::memory_order_relaxed));
	backend.close();

	RelocatableStorageBackendOptions reopenOptions = backendOptions();
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend reopened(std::move(reopenOptions));
	reopened.open();
	EXPECT_EQ(reopened.readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({2U, 2U}));
	const std::vector<uint64_t> retried =
			reopened.createLogicalEdges({{fixture.person_node_id, fixture.unlabeled_node_id, "DISK_FULL_RETRY", {}}});
	ASSERT_EQ(retried.size(), 1U);
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(retried.front()),
			  LogicalGraphEdge(retried.front(), fixture.person_node_id, fixture.unlabeled_node_id, "DISK_FULL_RETRY"));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest, OrdinaryEdgeShortAdjacencyPageRetainsOldGraphAndAllowsRetryAfterReopen) {
	const NativeGraphFixture fixture = materializeNativeGraphFixture();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();

	std::atomic<bool> injected{false};
	{
		StorageIO::ScopedNativeWriteHookForTesting shortAdjacencyPage(
				[&injected](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (byteCount == RSS_PAGE_SIZE && !injected.load(std::memory_order_relaxed)) {
						try {
							const auto page = std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount);
							const PageOwnerHeader owner =
									PageOwnerHeaderCodec::decode(page.first(RSS_PAGE_OWNER_HEADER_BYTES));
							if (owner.kind == PageOwnerKind::POK_INDEX) {
								const BTreePage tree =
										BTreePage::open(owner, page.subspan(RSS_PAGE_OWNER_HEADER_BYTES, owner.payload_bytes));
								if (tree.header().index_id == RSS_GLOBAL_ADJACENCY_INDEX_ID) {
									injected.store(true, std::memory_order_relaxed);
									return portable_pwrite(fd, bytes, 16U, offset);
								}
							}
						} catch (const std::exception &) {
							// Only an authenticated global adjacency page is this fault point.
						}
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		EXPECT_THROW(static_cast<void>(backend.createLogicalEdges(
						{{fixture.person_node_id, fixture.unlabeled_node_id, "SHORT_ADJACENCY", {}}})),
					 std::system_error);
	}
	EXPECT_TRUE(injected.load(std::memory_order_relaxed));
	backend.close();

	RelocatableStorageBackendOptions reopenOptions = backendOptions();
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend reopened(std::move(reopenOptions));
	reopened.open();
	EXPECT_EQ(reopened.readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({2U, 2U}));
	const std::vector<uint64_t> retried = reopened.createLogicalEdges(
			{{fixture.person_node_id, fixture.unlabeled_node_id, "SHORT_ADJACENCY_RETRY", {}}});
	ASSERT_EQ(retried.size(), 1U);
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(retried.front()), LogicalGraphEdge(retried.front(), fixture.person_node_id,
																	 fixture.unlabeled_node_id, "SHORT_ADJACENCY_RETRY"));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest, OrdinaryEdgeAdjacencyPageDiskFullRetainsOldGraphAndAllowsRetryAfterReopen) {
	const NativeGraphFixture fixture = materializeNativeGraphFixture();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();

	std::atomic<bool> injected{false};
	{
		StorageIO::ScopedNativeWriteHookForTesting diskFullOnAdjacencyPage(
				[&injected](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (byteCount == RSS_PAGE_SIZE && !injected.load(std::memory_order_relaxed)) {
						try {
							const auto page = std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount);
							const PageOwnerHeader owner =
									PageOwnerHeaderCodec::decode(page.first(RSS_PAGE_OWNER_HEADER_BYTES));
							if (owner.kind == PageOwnerKind::POK_INDEX) {
								const BTreePage tree =
										BTreePage::open(owner, page.subspan(RSS_PAGE_OWNER_HEADER_BYTES, owner.payload_bytes));
								if (tree.header().index_id == RSS_GLOBAL_ADJACENCY_INDEX_ID) {
									injected.store(true, std::memory_order_relaxed);
#ifdef _WIN32
									SetLastError(ERROR_DISK_FULL);
#else
									errno = ENOSPC;
#endif
									return static_cast<pwrite_ssize_t>(-1);
								}
							}
						} catch (const std::exception &) {
							// Only an authenticated global adjacency page is this fault point.
						}
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		EXPECT_THROW(static_cast<void>(backend.createLogicalEdges(
						{{fixture.person_node_id, fixture.unlabeled_node_id, "DISK_FULL_ADJACENCY", {}}})),
					 std::system_error);
	}
	EXPECT_TRUE(injected.load(std::memory_order_relaxed));
	backend.close();

	RelocatableStorageBackendOptions reopenOptions = backendOptions();
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend reopened(std::move(reopenOptions));
	reopened.open();
	EXPECT_EQ(reopened.readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({2U, 2U}));
	const std::vector<uint64_t> retried = reopened.createLogicalEdges(
			{{fixture.person_node_id, fixture.unlabeled_node_id, "DISK_FULL_ADJACENCY_RETRY", {}}});
	ASSERT_EQ(retried.size(), 1U);
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(retried.front()), LogicalGraphEdge(retried.front(), fixture.person_node_id,
																	 fixture.unlabeled_node_id, "DISK_FULL_ADJACENCY_RETRY"));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest,
		OrdinaryEdgeDeletionShortAdjacencyPageRetainsOldGraphAndAllowsRetryAfterReopen) {
	const NativeGraphFixture fixture = materializeNativeGraphFixture();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();

	std::atomic<bool> injected{false};
	{
		StorageIO::ScopedNativeWriteHookForTesting shortAdjacencyPage(
				[&injected](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (byteCount == RSS_PAGE_SIZE && !injected.load(std::memory_order_relaxed)) {
						try {
							const auto page = std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount);
							const PageOwnerHeader owner =
									PageOwnerHeaderCodec::decode(page.first(RSS_PAGE_OWNER_HEADER_BYTES));
							if (owner.kind == PageOwnerKind::POK_INDEX) {
								const BTreePage tree =
										BTreePage::open(owner, page.subspan(RSS_PAGE_OWNER_HEADER_BYTES, owner.payload_bytes));
								if (tree.header().index_id == RSS_GLOBAL_ADJACENCY_INDEX_ID) {
									injected.store(true, std::memory_order_relaxed);
									return portable_pwrite(fd, bytes, 16U, offset);
								}
							}
						} catch (const std::exception &) {
							// Only an authenticated global adjacency candidate is this fault point.
						}
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		EXPECT_THROW(backend.deleteLogicalEdges({{fixture.knows_edge_id}}), std::system_error);
	}
	EXPECT_TRUE(injected.load(std::memory_order_relaxed));
	backend.close();

	RelocatableStorageBackendOptions reopenOptions = backendOptions();
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend reopened(std::move(reopenOptions));
	reopened.open();
	EXPECT_EQ(reopened.readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({2U, 2U}));
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(fixture.knows_edge_id),
			  (LogicalGraphEdge{fixture.knows_edge_id, fixture.person_node_id, fixture.person_node_id, "KNOWS",
							{{"alpha", PropertyValue(int64_t(7))}}}));
	EXPECT_EQ(snapshot->readEdge(fixture.mentions_edge_id),
			  LogicalGraphEdge(fixture.mentions_edge_id, fixture.person_node_id, fixture.person_node_id, "MENTIONS"));
	snapshot.reset();
	reopened.deleteLogicalEdges({{fixture.knows_edge_id}});
	snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(fixture.knows_edge_id)), FormatException);
	EXPECT_EQ(snapshot->readEdge(fixture.mentions_edge_id),
			  LogicalGraphEdge(fixture.mentions_edge_id, fixture.person_node_id, fixture.person_node_id, "MENTIONS"));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest,
		OrdinaryEdgeDeletionAdjacencyPageDiskFullRetainsOldGraphAndAllowsRetryAfterReopen) {
	const NativeGraphFixture fixture = materializeNativeGraphFixture();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();

	std::atomic<bool> injected{false};
	{
		StorageIO::ScopedNativeWriteHookForTesting diskFullOnAdjacencyPage(
				[&injected](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (byteCount == RSS_PAGE_SIZE && !injected.load(std::memory_order_relaxed)) {
						try {
							const auto page = std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount);
							const PageOwnerHeader owner =
									PageOwnerHeaderCodec::decode(page.first(RSS_PAGE_OWNER_HEADER_BYTES));
							if (owner.kind == PageOwnerKind::POK_INDEX) {
								const BTreePage tree =
										BTreePage::open(owner, page.subspan(RSS_PAGE_OWNER_HEADER_BYTES, owner.payload_bytes));
								if (tree.header().index_id == RSS_GLOBAL_ADJACENCY_INDEX_ID) {
									injected.store(true, std::memory_order_relaxed);
#ifdef _WIN32
									SetLastError(ERROR_DISK_FULL);
#else
									errno = ENOSPC;
#endif
									return static_cast<pwrite_ssize_t>(-1);
								}
							}
						} catch (const std::exception &) {
							// Only an authenticated global adjacency candidate is this fault point.
						}
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		EXPECT_THROW(backend.deleteLogicalEdges({{fixture.knows_edge_id}}), std::system_error);
	}
	EXPECT_TRUE(injected.load(std::memory_order_relaxed));
	backend.close();

	RelocatableStorageBackendOptions reopenOptions = backendOptions();
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend reopened(std::move(reopenOptions));
	reopened.open();
	EXPECT_EQ(reopened.readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({2U, 2U}));
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(fixture.knows_edge_id),
			  (LogicalGraphEdge{fixture.knows_edge_id, fixture.person_node_id, fixture.person_node_id, "KNOWS",
							{{"alpha", PropertyValue(int64_t(7))}}}));
	EXPECT_EQ(snapshot->readEdge(fixture.mentions_edge_id),
			  LogicalGraphEdge(fixture.mentions_edge_id, fixture.person_node_id, fixture.person_node_id, "MENTIONS"));
	snapshot.reset();
	reopened.deleteLogicalEdges({{fixture.knows_edge_id}});
	snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(fixture.knows_edge_id)), FormatException);
	EXPECT_EQ(snapshot->readEdge(fixture.mentions_edge_id),
			  LogicalGraphEdge(fixture.mentions_edge_id, fixture.person_node_id, fixture.person_node_id, "MENTIONS"));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest, OrdinaryEdgeShortMetadataPageRetainsOldGraphAndAllowsRetryAfterReopen) {
	const NativeGraphFixture fixture = materializeNativeGraphFixture();
	uint64_t metadataPoolOffset = 0U;
	{
		auto data = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
		ASSERT_TRUE(data->is_open());
		auto dataIo = std::make_shared<StorageIO>(data, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		auto metadata = RelocatableMetadataStore::open(dataIo);
		const MetadataSnapshotPin snapshot = metadata->openSnapshot();
		metadataPoolOffset = snapshot.superblock().data_region_offset;
	}

	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	std::atomic<bool> injected{false};
	{
		StorageIO::ScopedNativeWriteHookForTesting shortMetadataPage(
				[metadataPoolOffset, &injected](file_handle_t fd, const void *bytes, size_t byteCount,
															pwrite_off_t offset) {
					const uint64_t absoluteOffset = static_cast<uint64_t>(offset);
				if (byteCount == RSS_PAGE_SIZE && absoluteOffset >= metadataPoolOffset + RSS_PAGE_SIZE &&
					absoluteOffset < metadataPoolOffset + RSS_SEGMENT_SIZE &&
					!injected.load(std::memory_order_relaxed)) {
					injected.store(true, std::memory_order_relaxed);
					return portable_pwrite(fd, bytes, 16U, offset);
				}
				return portable_pwrite(fd, bytes, byteCount, offset);
				});
		EXPECT_THROW(static_cast<void>(backend.createLogicalEdges(
						{{fixture.person_node_id, fixture.unlabeled_node_id, "SHORT_METADATA", {}}})),
					 std::system_error);
	}
	EXPECT_TRUE(injected.load(std::memory_order_relaxed));
	backend.close();

	RelocatableStorageBackendOptions reopenOptions = backendOptions();
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend reopened(std::move(reopenOptions));
	reopened.open();
	EXPECT_EQ(reopened.readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({2U, 2U}));
	const std::vector<uint64_t> retried =
			reopened.createLogicalEdges({{fixture.person_node_id, fixture.unlabeled_node_id, "SHORT_METADATA_RETRY", {}}});
	ASSERT_EQ(retried.size(), 1U);
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(retried.front()), LogicalGraphEdge(retried.front(), fixture.person_node_id,
																	 fixture.unlabeled_node_id, "SHORT_METADATA_RETRY"));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest, OrdinaryEdgeMetadataPageDiskFullRetainsOldGraphAndAllowsRetryAfterReopen) {
	const NativeGraphFixture fixture = materializeNativeGraphFixture();
	uint64_t metadataPoolOffset = 0U;
	{
		auto data = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
		ASSERT_TRUE(data->is_open());
		auto dataIo = std::make_shared<StorageIO>(data, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		auto metadata = RelocatableMetadataStore::open(dataIo);
		const MetadataSnapshotPin snapshot = metadata->openSnapshot();
		metadataPoolOffset = snapshot.superblock().data_region_offset;
	}

	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	std::atomic<bool> injected{false};
	{
		StorageIO::ScopedNativeWriteHookForTesting diskFullOnMetadataPage(
				[metadataPoolOffset, &injected](file_handle_t fd, const void *bytes, size_t byteCount,
																  pwrite_off_t offset) {
					const uint64_t absoluteOffset = static_cast<uint64_t>(offset);
					if (byteCount == RSS_PAGE_SIZE && absoluteOffset >= metadataPoolOffset + RSS_PAGE_SIZE &&
						absoluteOffset < metadataPoolOffset + RSS_SEGMENT_SIZE &&
						!injected.exchange(true, std::memory_order_relaxed)) {
#ifdef _WIN32
						SetLastError(ERROR_DISK_FULL);
#else
						errno = ENOSPC;
#endif
						return static_cast<pwrite_ssize_t>(-1);
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		EXPECT_THROW(static_cast<void>(backend.createLogicalEdges(
						{{fixture.person_node_id, fixture.unlabeled_node_id, "DISK_FULL_METADATA", {}}})),
					 std::system_error);
	}
	EXPECT_TRUE(injected.load(std::memory_order_relaxed));
	backend.close();

	RelocatableStorageBackendOptions reopenOptions = backendOptions();
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend reopened(std::move(reopenOptions));
	reopened.open();
	EXPECT_EQ(reopened.readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({2U, 2U}));
	const std::vector<uint64_t> retried = reopened.createLogicalEdges(
			{{fixture.person_node_id, fixture.unlabeled_node_id, "DISK_FULL_METADATA_RETRY", {}}});
	ASSERT_EQ(retried.size(), 1U);
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(retried.front()),
			  LogicalGraphEdge(retried.front(), fixture.person_node_id, fixture.unlabeled_node_id,
							   "DISK_FULL_METADATA_RETRY"));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest, OrdinaryEdgeFinalSyncFailureReopensAnOldOrNewCompleteGeneration) {
	const NativeGraphFixture fixture = materializeNativeGraphFixture();
	PhysicalPageRef baseEdgeDirectoryRoot;
	{
		auto data = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
		ASSERT_TRUE(data->is_open());
		auto dataIo = std::make_shared<StorageIO>(data, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		auto metadata = RelocatableMetadataStore::open(dataIo);
		const MetadataSnapshotPin snapshot = metadata->openSnapshot();
		baseEdgeDirectoryRoot = snapshot.superblock().edge_entity_directory_root;
	}
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();

	std::atomic<uint32_t> graphPublicationWrites{0U};
	std::atomic<uint32_t> syncCalls{0U};
	std::atomic<bool> failedFinalFence{false};
	{
		StorageIO::ScopedNativeWriteHookForTesting observeGraphPublication(
				[baseEdgeDirectoryRoot, &graphPublicationWrites](file_handle_t fd, const void *bytes, size_t byteCount,
																  pwrite_off_t offset) {
					if (byteCount == RSS_PAGE_SIZE &&
						(offset == SuperblockFile::slotOffset(0U) || offset == SuperblockFile::slotOffset(1U))) {
						try {
							const Superblock candidate = SuperblockCodec::decode(
									std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount));
							// The fixture already owns an Edge directory. Metadata-only ID
							// leases retain this root, so they are not the public graph fence.
							if (candidate.edge_entity_directory_root != baseEdgeDirectoryRoot) {
								graphPublicationWrites.fetch_add(1U, std::memory_order_relaxed);
							}
						} catch (const std::exception &) {
							// Invalid control pages are outside the publication boundary.
						}
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		StorageIO::ScopedNativeSyncHookForTesting failFinalPublicationFence(
				[&graphPublicationWrites, &syncCalls, &failedFinalFence](file_handle_t fd) {
					syncCalls.fetch_add(1U, std::memory_order_relaxed);
					if (graphPublicationWrites.load(std::memory_order_relaxed) >= 1U &&
						!failedFinalFence.exchange(true, std::memory_order_relaxed)) {
#ifdef _WIN32
						SetLastError(ERROR_WRITE_FAULT);
#else
						errno = EIO;
#endif
						return -1;
					}
					return portable_fsync(fd);
				});
		EXPECT_THROW(static_cast<void>(backend.createLogicalEdges(
						{{fixture.person_node_id, fixture.unlabeled_node_id, "FINAL_SYNC", {}}})),
					 std::runtime_error);
	}
	EXPECT_GE(graphPublicationWrites.load(std::memory_order_relaxed), 1U);
	EXPECT_TRUE(failedFinalFence.load(std::memory_order_relaxed));
	EXPECT_GE(syncCalls.load(std::memory_order_relaxed), 2U);
	EXPECT_THROW(static_cast<void>(backend.createLogicalNodes({{{"MustReopenFirst"}, {}}})), std::logic_error);
	backend.close();

	RelocatableStorageBackendOptions reopenOptions = backendOptions();
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend reopened(std::move(reopenOptions));
	reopened.open();
	std::optional<LogicalGraphCardinalityStatistics> statistics = reopened.readLogicalGraphCardinalityStatistics();
	ASSERT_TRUE(statistics.has_value());
	ASSERT_GE(statistics->edge_count, 2U);
	ASSERT_LE(statistics->edge_count, 3U);
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = reopened.openLogicalGraphReadSnapshot();
	const LogicalGraphAdjacencyPage outgoing = snapshot->scanAdjacentEdges(
			fixture.person_node_id, LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 16U);
	const auto findFinalSyncEdge = [&outgoing, &fixture]() {
		return std::ranges::find_if(outgoing.edges, [&fixture](const LogicalGraphEdge &edge) {
			return edge.target_node_id == fixture.unlabeled_node_id && edge.type == "FINAL_SYNC";
		});
	};
	if (findFinalSyncEdge() == outgoing.edges.end()) {
		snapshot.reset();
		const std::vector<uint64_t> retried =
				reopened.createLogicalEdges({{fixture.person_node_id, fixture.unlabeled_node_id, "FINAL_SYNC", {}}});
		ASSERT_EQ(retried.size(), 1U);
		snapshot = reopened.openLogicalGraphReadSnapshot();
		EXPECT_EQ(snapshot->readEdge(retried.front()),
				  LogicalGraphEdge(retried.front(), fixture.person_node_id, fixture.unlabeled_node_id, "FINAL_SYNC"));
	}
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest, OrdinaryEdgeDeletionFinalSyncFailureReopensAnOldOrNewCompleteGeneration) {
	const NativeGraphFixture fixture = materializeNativeGraphFixture();
	PhysicalPageRef baseEdgeDirectoryRoot;
	{
		auto data = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
		ASSERT_TRUE(data->is_open());
		auto dataIo = std::make_shared<StorageIO>(data, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		auto metadata = RelocatableMetadataStore::open(dataIo);
		const MetadataSnapshotPin snapshot = metadata->openSnapshot();
		baseEdgeDirectoryRoot = snapshot.superblock().edge_entity_directory_root;
	}
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();

	std::atomic<uint32_t> graphPublicationWrites{0U};
	std::atomic<uint32_t> syncCalls{0U};
	std::atomic<bool> failedFinalFence{false};
	{
		StorageIO::ScopedNativeWriteHookForTesting observeGraphPublication(
				[baseEdgeDirectoryRoot, &graphPublicationWrites](file_handle_t fd, const void *bytes, size_t byteCount,
																		  pwrite_off_t offset) {
					if (byteCount == RSS_PAGE_SIZE &&
						(offset == SuperblockFile::slotOffset(0U) || offset == SuperblockFile::slotOffset(1U))) {
						try {
							const Superblock candidate = SuperblockCodec::decode(
									std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount));
							if (candidate.edge_entity_directory_root != baseEdgeDirectoryRoot) {
								graphPublicationWrites.fetch_add(1U, std::memory_order_relaxed);
							}
						} catch (const std::exception &) {
							// Invalid control pages are outside the publication boundary.
						}
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		StorageIO::ScopedNativeSyncHookForTesting failFinalPublicationFence(
				[&graphPublicationWrites, &syncCalls, &failedFinalFence](file_handle_t fd) {
					syncCalls.fetch_add(1U, std::memory_order_relaxed);
					if (graphPublicationWrites.load(std::memory_order_relaxed) >= 1U &&
						!failedFinalFence.exchange(true, std::memory_order_relaxed)) {
#ifdef _WIN32
						SetLastError(ERROR_WRITE_FAULT);
#else
						errno = EIO;
#endif
						return -1;
					}
					return portable_fsync(fd);
				});
		EXPECT_THROW(backend.deleteLogicalEdges({{fixture.knows_edge_id}}), std::runtime_error);
	}
	EXPECT_GE(graphPublicationWrites.load(std::memory_order_relaxed), 1U);
	EXPECT_TRUE(failedFinalFence.load(std::memory_order_relaxed));
	EXPECT_GE(syncCalls.load(std::memory_order_relaxed), 2U);
	EXPECT_THROW(static_cast<void>(backend.createLogicalNodes({{{"MustReopenFirst"}, {}}})), std::logic_error);
	backend.close();

	RelocatableStorageBackendOptions reopenOptions = backendOptions();
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend reopened(std::move(reopenOptions));
	reopened.open();
	const std::optional<LogicalGraphCardinalityStatistics> statistics = reopened.readLogicalGraphCardinalityStatistics();
	ASSERT_TRUE(statistics.has_value());
	ASSERT_GE(statistics->edge_count, 1U);
	ASSERT_LE(statistics->edge_count, 2U);
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(fixture.mentions_edge_id),
			  LogicalGraphEdge(fixture.mentions_edge_id, fixture.person_node_id, fixture.person_node_id, "MENTIONS"));
	if (statistics->edge_count == 2U) {
		EXPECT_EQ(snapshot->readEdge(fixture.knows_edge_id),
				  (LogicalGraphEdge{fixture.knows_edge_id, fixture.person_node_id, fixture.person_node_id, "KNOWS",
									{{"alpha", PropertyValue(int64_t(7))}}}));
		snapshot.reset();
		reopened.deleteLogicalEdges({{fixture.knows_edge_id}});
		snapshot = reopened.openLogicalGraphReadSnapshot();
	}
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(fixture.knows_edge_id)), FormatException);
	EXPECT_EQ(snapshot->readEdge(fixture.mentions_edge_id),
			  LogicalGraphEdge(fixture.mentions_edge_id, fixture.person_node_id, fixture.person_node_id, "MENTIONS"));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest, DetachDeleteFinalSyncFailureReopensAnOldOrNewCompleteGeneration) {
	const NativeGraphFixture fixture = materializeNativeGraphFixture();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> retainedEdges = backend.createLogicalEdges(
			{{fixture.unlabeled_node_id, fixture.unlabeled_node_id, "RETAINED_AFTER_DETACH", {}}});
	ASSERT_EQ(retainedEdges.size(), 1U);
	PhysicalPageRef baseEdgeDirectoryRoot;
	{
		auto data = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
		ASSERT_TRUE(data->is_open());
		auto dataIo = std::make_shared<StorageIO>(data, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		auto metadata = RelocatableMetadataStore::open(dataIo);
		const MetadataSnapshotPin snapshot = metadata->openSnapshot();
		baseEdgeDirectoryRoot = snapshot.superblock().edge_entity_directory_root;
	}

	std::atomic<uint32_t> graphPublicationWrites{0U};
	std::atomic<uint32_t> syncCalls{0U};
	std::atomic<bool> failedFinalFence{false};
	{
		StorageIO::ScopedNativeWriteHookForTesting observeGraphPublication(
				[baseEdgeDirectoryRoot, &graphPublicationWrites](file_handle_t fd, const void *bytes, size_t byteCount,
																					  pwrite_off_t offset) {
					if (byteCount == RSS_PAGE_SIZE &&
						(offset == SuperblockFile::slotOffset(0U) || offset == SuperblockFile::slotOffset(1U))) {
						try {
							const Superblock candidate = SuperblockCodec::decode(
									std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount));
							if (candidate.edge_entity_directory_root != baseEdgeDirectoryRoot) {
								graphPublicationWrites.fetch_add(1U, std::memory_order_relaxed);
							}
						} catch (const std::exception &) {
							// Invalid control pages are outside the publication boundary.
						}
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		StorageIO::ScopedNativeSyncHookForTesting failFinalPublicationFence(
				[&graphPublicationWrites, &syncCalls, &failedFinalFence](file_handle_t fd) {
					syncCalls.fetch_add(1U, std::memory_order_relaxed);
					if (graphPublicationWrites.load(std::memory_order_relaxed) >= 1U &&
						!failedFinalFence.exchange(true, std::memory_order_relaxed)) {
#ifdef _WIN32
						SetLastError(ERROR_WRITE_FAULT);
#else
						errno = EIO;
#endif
						return -1;
					}
					return portable_fsync(fd);
				});
		EXPECT_THROW(backend.deleteLogicalNodes({{fixture.person_node_id, true, 2U}}), std::runtime_error);
	}
	EXPECT_GE(graphPublicationWrites.load(std::memory_order_relaxed), 1U);
	EXPECT_TRUE(failedFinalFence.load(std::memory_order_relaxed));
	EXPECT_GE(syncCalls.load(std::memory_order_relaxed), 2U);
	EXPECT_THROW(static_cast<void>(backend.createLogicalNodes({{{"MustReopenFirst"}, {}}})), std::logic_error);
	backend.close();

	RelocatableStorageBackendOptions reopenOptions = backendOptions();
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend reopened(std::move(reopenOptions));
	reopened.open();
	const std::optional<LogicalGraphCardinalityStatistics> statistics = reopened.readLogicalGraphCardinalityStatistics();
	ASSERT_TRUE(statistics.has_value());
	EXPECT_TRUE((*statistics == LogicalGraphCardinalityStatistics{2U, 3U} ||
				 *statistics == LogicalGraphCardinalityStatistics{1U, 1U}));
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(retainedEdges.front()),
			  LogicalGraphEdge(retainedEdges.front(), fixture.unlabeled_node_id, fixture.unlabeled_node_id,
								"RETAINED_AFTER_DETACH"));
	if (*statistics == LogicalGraphCardinalityStatistics{2U, 3U}) {
		EXPECT_EQ(snapshot->readNode(fixture.person_node_id).node_id, fixture.person_node_id);
		EXPECT_EQ(snapshot->readEdge(fixture.knows_edge_id),
				  (LogicalGraphEdge{fixture.knows_edge_id, fixture.person_node_id, fixture.person_node_id, "KNOWS",
												{{"alpha", PropertyValue(int64_t(7))}}}));
		EXPECT_EQ(snapshot->readEdge(fixture.mentions_edge_id),
				  LogicalGraphEdge(fixture.mentions_edge_id, fixture.person_node_id, fixture.person_node_id, "MENTIONS"));
		snapshot.reset();
		reopened.deleteLogicalNodes({{fixture.person_node_id, true, 2U}});
		snapshot = reopened.openLogicalGraphReadSnapshot();
	}
	EXPECT_THROW(static_cast<void>(snapshot->readNode(fixture.person_node_id)), FormatException);
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(fixture.knows_edge_id)), FormatException);
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(fixture.mentions_edge_id)), FormatException);
	EXPECT_EQ(snapshot->readEdge(retainedEdges.front()),
			  LogicalGraphEdge(retainedEdges.front(), fixture.unlabeled_node_id, fixture.unlabeled_node_id,
								"RETAINED_AFTER_DETACH"));
	snapshot.reset();
	EXPECT_EQ(reopened.readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({1U, 1U}));
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest, DetachDeletingACreatedNodeCancelsItsCreatedEdgesAtomically) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();

	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	const LogicalGraphMutationEntityReference firstCreatedNode{
			LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 0U};
	const LogicalGraphMutationEntityReference secondCreatedNode{
			LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 1U};
	LogicalGraphMutationBatch create;
	create.node_creates = {{{"Discarded"}, {}}, {{"Retained"}, {{"name", PropertyValue("Grace")}}}};
	create.edge_creates = {
			{firstCreatedNode, secondCreatedNode, "DISCARDED_OUT", {{"rank", PropertyValue(int64_t{1})}}},
			{secondCreatedNode, firstCreatedNode, "DISCARDED_IN", {{"rank", PropertyValue(int64_t{2})}}},
	};
	const LogicalGraphMutationBatchResult staged = transaction->stageMutationBatch(std::move(create));
	ASSERT_EQ(staged.created_node_ids.size(), 2U);
	ASSERT_EQ(staged.created_edge_ids.size(), 2U);

	// The public logical writer represents prior creates by their provisional
	// identity in later statements. These writes must be rebased and folded into
	// the same private final state before the subsequent DETACH DELETE cancels it.
	LogicalGraphMutationBatch update;
	update.property_replacements = {{
			{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED,
			 staged.created_node_ids.front()},
			{{"must_not_persist", PropertyValue(int64_t{3})}},
	}};
	update.node_label_replacements = {{
			{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED,
			 staged.created_node_ids.front()},
			{"Discarded", "Transient"},
	}};
	EXPECT_TRUE(transaction->stageMutationBatch(std::move(update)).created_node_ids.empty());

	// Deleting a staged Node is resolved against the transaction's final state.
	// The backend must cancel both incident staged Edges and all folded state for
	// that Node instead of materializing then deleting a partial graph.
	LogicalGraphMutationBatch erase;
	erase.node_deletes = {{staged.created_node_ids.front(), true, 8U}};
	EXPECT_TRUE(transaction->stageMutationBatch(std::move(erase)).created_node_ids.empty());
	const LogicalGraphMutationBatchResult committed = transaction->commit();
	EXPECT_EQ(committed.created_node_ids, (std::vector<uint64_t>{staged.created_node_ids.back()}));
	EXPECT_TRUE(committed.created_edge_ids.empty());
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();

	RelocatableStorageBackend reopened(backendOptions());
	reopened.open();
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_THROW(static_cast<void>(snapshot->readNode(staged.created_node_ids.front())), FormatException);
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(staged.created_edge_ids[0])), FormatException);
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(staged.created_edge_ids[1])), FormatException);
	const LogicalGraphNode retained = snapshot->readNode(staged.created_node_ids.back());
	EXPECT_EQ(retained.labels, (std::vector<std::string>{"Retained"}));
	EXPECT_EQ(retained.properties, (std::vector<LogicalGraphPropertyEntry>{{"name", PropertyValue("Grace")}}));
	EXPECT_TRUE(snapshot->scanAdjacentEdges(staged.created_node_ids.back(), LogicalGraphReadDirection::LGRD_OUTGOING,
											std::nullopt, 8U)
						.edges.empty());
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest, NonDetachDeletingACreatedNodeWithACreatedEdgeRollsBackTheWholeTransaction) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();

	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	const LogicalGraphMutationEntityReference source{LogicalGraphMutationEntityKind::LGMK_NODE,
													 LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 0U};
	const LogicalGraphMutationEntityReference target{LogicalGraphMutationEntityKind::LGMK_NODE,
													 LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 1U};
	LogicalGraphMutationBatch create;
	create.node_creates = {{{"MustRollbackSource"}, {}}, {{"MustRollbackTarget"}, {}}};
	create.edge_creates = {{source, target, "MUST_ROLLBACK", {}}};
	const LogicalGraphMutationBatchResult staged = transaction->stageMutationBatch(std::move(create));
	ASSERT_EQ(staged.created_node_ids.size(), 2U);
	ASSERT_EQ(staged.created_edge_ids.size(), 1U);

	// Deleting an in-transaction Node without DETACH may not silently cancel a
	// live in-transaction Edge. The rejection happens at final-state validation,
	// so commit must invalidate the complete transaction rather than expose a
	// partial graph or leave a reusable staged transaction behind.
	LogicalGraphMutationBatch erase;
	erase.node_deletes = {{staged.created_node_ids.front(), false, 0U}};
	EXPECT_TRUE(transaction->stageMutationBatch(std::move(erase)).created_node_ids.empty());
	EXPECT_THROW(static_cast<void>(transaction->commit()), std::invalid_argument);
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_ROLLED_BACK);
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();

	RelocatableStorageBackend reopened(backendOptions());
	reopened.open();
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = reopened.openLogicalGraphReadSnapshot();
	for (const uint64_t nodeId: staged.created_node_ids) {
		EXPECT_THROW(static_cast<void>(snapshot->readNode(nodeId)), FormatException);
	}
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(staged.created_edge_ids.front())), FormatException);
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest, LaterPersistedEntityReplacementsDefineTheTransactionFinalState) {
	const NativeGraphFixture fixture = materializeNativeGraphFixture();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();

	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	const LogicalGraphMutationEntityReference node{LogicalGraphMutationEntityKind::LGMK_NODE,
												   LogicalGraphMutationReferenceKind::LGRK_PERSISTED,
												   fixture.person_node_id};
	const LogicalGraphMutationEntityReference edge{LogicalGraphMutationEntityKind::LGMK_EDGE,
												   LogicalGraphMutationReferenceKind::LGRK_PERSISTED,
												   fixture.knows_edge_id};

	LogicalGraphMutationBatch first;
	first.property_replacements = {
			{node, {{"version", PropertyValue(int64_t{1})}}},
			{edge, {{"weight", PropertyValue(int64_t{1})}}},
	};
	first.node_label_replacements = {{node, {"First"}}};
	EXPECT_TRUE(transaction->stageMutationBatch(std::move(first)).created_node_ids.empty());

	// Each statement replaces a complete logical collection. Later statements
	// therefore define the final persisted entity state instead of forming an
	// invalid duplicate target in the single durable publication.
	LogicalGraphMutationBatch second;
	second.property_replacements = {
			{node, {{"version", PropertyValue(int64_t{2})}, {"state", PropertyValue("final")}}},
			{edge, {{"weight", PropertyValue(int64_t{2})}, {"kind", PropertyValue("final")}}},
	};
	second.node_label_replacements = {{node, {"Second", "Final"}}};
	EXPECT_TRUE(transaction->stageMutationBatch(std::move(second)).created_node_ids.empty());

	EXPECT_TRUE(transaction->commit().created_node_ids.empty());
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_COMMITTED);
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));

	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readNode(fixture.person_node_id).labels, (std::vector<std::string>{"Final", "Second"}));
	EXPECT_EQ(snapshot->readNode(fixture.person_node_id).properties,
			  (std::vector<LogicalGraphPropertyEntry>{{"state", PropertyValue("final")},
													  {"version", PropertyValue(int64_t{2})}}));
	EXPECT_EQ(snapshot->readEdge(fixture.knows_edge_id).properties,
			  (std::vector<LogicalGraphPropertyEntry>{{"kind", PropertyValue("final")},
													  {"weight", PropertyValue(int64_t{2})}}));
	snapshot.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, DuplicatePersistedEntityReplacementsWithinOneStatementAreRejected) {
	const NativeGraphFixture fixture = materializeNativeGraphFixture();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();

	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	const LogicalGraphMutationEntityReference node{LogicalGraphMutationEntityKind::LGMK_NODE,
												   LogicalGraphMutationReferenceKind::LGRK_PERSISTED,
												   fixture.person_node_id};
	LogicalGraphMutationBatch invalid;
	invalid.property_replacements = {
			{node, {{"first", PropertyValue(int64_t{1})}}},
			{node, {{"second", PropertyValue(int64_t{2})}}},
	};
	invalid.node_label_replacements = {{node, {"First"}}, {node, {"Second"}}};
	EXPECT_THROW(static_cast<void>(transaction->stageMutationBatch(std::move(invalid))), std::invalid_argument);
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_ACTIVE);

	LogicalGraphMutationBatch valid;
	valid.property_replacements = {{node, {{"accepted", PropertyValue(int64_t{3})}}}};
	valid.node_label_replacements = {{node, {"Accepted"}}};
	EXPECT_TRUE(transaction->stageMutationBatch(std::move(valid)).created_node_ids.empty());
	EXPECT_TRUE(transaction->commit().created_node_ids.empty());
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));

	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readNode(fixture.person_node_id).labels, (std::vector<std::string>{"Accepted"}));
	EXPECT_EQ(snapshot->readNode(fixture.person_node_id).properties,
			  (std::vector<LogicalGraphPropertyEntry>{{"accepted", PropertyValue(int64_t{3})}}));
	snapshot.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, DeletingAPersistedEntityElidesItsEarlierTransactionReplacements) {
	const NativeGraphFixture fixture = materializeNativeGraphFixture();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();

	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	const LogicalGraphMutationEntityReference node{LogicalGraphMutationEntityKind::LGMK_NODE,
												   LogicalGraphMutationReferenceKind::LGRK_PERSISTED,
												   fixture.person_node_id};
	const LogicalGraphMutationEntityReference edge{LogicalGraphMutationEntityKind::LGMK_EDGE,
												   LogicalGraphMutationReferenceKind::LGRK_PERSISTED,
												   fixture.knows_edge_id};
	LogicalGraphMutationBatch update;
	update.property_replacements = {
			{node, {{"discarded", PropertyValue(int64_t{1})}}},
			{edge, {{"discarded", PropertyValue(int64_t{2})}}},
	};
	update.node_label_replacements = {{node, {"Discarded"}}};
	EXPECT_TRUE(transaction->stageMutationBatch(std::move(update)).created_node_ids.empty());

	// The final state deletes the Edge and detaches the Node. The
	// preceding replacements are semantically dead and must not invalidate the
	// one canonical publication as update-on-deleted-entity requests.
	LogicalGraphMutationBatch erase;
	erase.edge_deletes = {{fixture.knows_edge_id}};
	erase.node_deletes = {{fixture.person_node_id, true, 8U}};
	EXPECT_TRUE(transaction->stageMutationBatch(std::move(erase)).created_node_ids.empty());
	EXPECT_TRUE(transaction->commit().created_node_ids.empty());
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_COMMITTED);
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));

	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	EXPECT_THROW(static_cast<void>(snapshot->readNode(fixture.person_node_id)), FormatException);
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(fixture.knows_edge_id)), FormatException);
	snapshot.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, ExplicitPersistedEdgeDeletionDoesNotConsumeDetachBudget) {
	const NativeGraphFixture fixture = materializeNativeGraphFixture();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();

	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	// The fixture owns two persisted self-loop Edges for this Node. Deleting one
	// first leaves one final-state incident Edge, so a detach bound of one must
	// succeed rather than count the already deleted Edge again.
	LogicalGraphMutationBatch erase;
	erase.edge_deletes = {{fixture.knows_edge_id}};
	erase.node_deletes = {{fixture.person_node_id, true, 1U}};
	EXPECT_TRUE(transaction->stageMutationBatch(std::move(erase)).created_node_ids.empty());
	EXPECT_TRUE(transaction->commit().created_node_ids.empty());
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_COMMITTED);
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));

	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	EXPECT_THROW(static_cast<void>(snapshot->readNode(fixture.person_node_id)), FormatException);
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(fixture.knows_edge_id)), FormatException);
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(fixture.mentions_edge_id)), FormatException);
	snapshot.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, ExplicitPersistedEdgeDeletionMakesNonDetachNodeDeletionValid) {
	const NativeGraphFixture fixture = materializeNativeGraphFixture();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();

	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	// A normal DELETE may remove its complete incident-edge set explicitly. The
	// Node validation must observe those staged adjacency removals, regardless of
	// their distinct request vectors in the final mutation batch.
	LogicalGraphMutationBatch erase;
	erase.edge_deletes = {{fixture.knows_edge_id}, {fixture.mentions_edge_id}};
	erase.node_deletes = {{fixture.person_node_id, false, 0U}};
	EXPECT_TRUE(transaction->stageMutationBatch(std::move(erase)).created_node_ids.empty());
	EXPECT_TRUE(transaction->commit().created_node_ids.empty());
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_COMMITTED);
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));

	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	EXPECT_THROW(static_cast<void>(snapshot->readNode(fixture.person_node_id)), FormatException);
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(fixture.knows_edge_id)), FormatException);
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(fixture.mentions_edge_id)), FormatException);
	snapshot.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, DetachDeletingAPersistedNodeCancelsLaterCreatedIncidentEdges) {
	const NativeGraphFixture fixture = materializeNativeGraphFixture();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();

	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	const LogicalGraphMutationEntityReference persistedSource{LogicalGraphMutationEntityKind::LGMK_NODE,
															  LogicalGraphMutationReferenceKind::LGRK_PERSISTED,
															  fixture.person_node_id};
	const LogicalGraphMutationEntityReference createdTarget{LogicalGraphMutationEntityKind::LGMK_NODE,
															LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 0U};
	LogicalGraphMutationBatch create;
	create.node_creates = {{{"Retained"}, {{"name", PropertyValue("Ada")}}}};
	create.edge_creates = {{persistedSource, createdTarget, "TRANSIENT", {{"rank", PropertyValue(int64_t{1})}}}};
	const LogicalGraphMutationBatchResult staged = transaction->stageMutationBatch(std::move(create));
	ASSERT_EQ(staged.created_node_ids.size(), 1U);
	ASSERT_EQ(staged.created_edge_ids.size(), 1U);

	LogicalGraphMutationBatch erase;
	erase.node_deletes = {{fixture.person_node_id, true, 8U}};
	EXPECT_TRUE(transaction->stageMutationBatch(std::move(erase)).created_node_ids.empty());
	const LogicalGraphMutationBatchResult committed = transaction->commit();
	EXPECT_EQ(committed.created_node_ids, staged.created_node_ids);
	EXPECT_TRUE(committed.created_edge_ids.empty());
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));

	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	EXPECT_THROW(static_cast<void>(snapshot->readNode(fixture.person_node_id)), FormatException);
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(staged.created_edge_ids.front())), FormatException);
	EXPECT_EQ(snapshot->readNode(staged.created_node_ids.front()).labels, (std::vector<std::string>{"Retained"}));
	snapshot.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, DetachDeletionBoundsLiveCreatedEdgesDuringFinalStateCanonicalization) {
	const NativeGraphFixture fixture = materializeNativeGraphFixture();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();

	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	const LogicalGraphMutationEntityReference persistedSource{LogicalGraphMutationEntityKind::LGMK_NODE,
															  LogicalGraphMutationReferenceKind::LGRK_PERSISTED,
															  fixture.person_node_id};
	const LogicalGraphMutationEntityReference createdTarget{LogicalGraphMutationEntityKind::LGMK_NODE,
															LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 0U};
	LogicalGraphMutationBatch create;
	create.node_creates = {{{"First"}, {}}, {{"Second"}, {}}};
	create.edge_creates = {
			{persistedSource, createdTarget, "FIRST", {}},
			{persistedSource,
			 {LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 1U},
			 "SECOND",
			 {}},
	};
	const LogicalGraphMutationBatchResult staged = transaction->stageMutationBatch(std::move(create));
	ASSERT_EQ(staged.created_node_ids.size(), 2U);
	ASSERT_EQ(staged.created_edge_ids.size(), 2U);

	LogicalGraphMutationBatch erase;
	erase.node_deletes = {{fixture.person_node_id, true, 1U}};
	EXPECT_TRUE(transaction->stageMutationBatch(std::move(erase)).created_node_ids.empty());
	EXPECT_THROW(static_cast<void>(transaction->commit()), std::invalid_argument);
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_ROLLED_BACK);
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));

	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	EXPECT_NO_THROW(static_cast<void>(snapshot->readNode(fixture.person_node_id)));
	for (const uint64_t nodeId: staged.created_node_ids) {
		EXPECT_THROW(static_cast<void>(snapshot->readNode(nodeId)), FormatException);
	}
	for (const uint64_t edgeId: staged.created_edge_ids) {
		EXPECT_THROW(static_cast<void>(snapshot->readEdge(edgeId)), FormatException);
	}
	snapshot.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, DetachDeletionSharesItsBoundBetweenPersistedAndCreatedIncidentEdges) {
	const NativeGraphFixture fixture = materializeNativeGraphFixture();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();

	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	const LogicalGraphMutationEntityReference persistedNode{LogicalGraphMutationEntityKind::LGMK_NODE,
															LogicalGraphMutationReferenceKind::LGRK_PERSISTED,
															fixture.person_node_id};
	const LogicalGraphMutationEntityReference createdNode{LogicalGraphMutationEntityKind::LGMK_NODE,
														  LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 0U};
	LogicalGraphMutationBatch create;
	create.node_creates = {{{"Survives"}, {}}};
	create.edge_creates = {{persistedNode, createdNode, "TRANSIENT", {}}};
	const LogicalGraphMutationBatchResult staged = transaction->stageMutationBatch(std::move(create));
	ASSERT_EQ(staged.created_node_ids.size(), 1U);
	ASSERT_EQ(staged.created_edge_ids.size(), 1U);

	// The fixture has two persisted self-loop Edges. The detach budget therefore
	// covers those two durable Edges plus this one provisional Edge exactly.
	LogicalGraphMutationBatch erase;
	erase.node_deletes = {{fixture.person_node_id, true, 3U}};
	EXPECT_TRUE(transaction->stageMutationBatch(std::move(erase)).created_node_ids.empty());
	const LogicalGraphMutationBatchResult committed = transaction->commit();
	EXPECT_EQ(committed.created_node_ids, staged.created_node_ids);
	EXPECT_TRUE(committed.created_edge_ids.empty());
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));

	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	EXPECT_THROW(static_cast<void>(snapshot->readNode(fixture.person_node_id)), FormatException);
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(fixture.knows_edge_id)), FormatException);
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(fixture.mentions_edge_id)), FormatException);
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(staged.created_edge_ids.front())), FormatException);
	EXPECT_NO_THROW(static_cast<void>(snapshot->readNode(staged.created_node_ids.front())));
	snapshot.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, DetachDeletingAPersistedNodeCountsCreatedSelfLoopOnlyOnce) {
	const NativeGraphFixture fixture = materializeNativeGraphFixture();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();

	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	const LogicalGraphMutationEntityReference persistedNode{LogicalGraphMutationEntityKind::LGMK_NODE,
															LogicalGraphMutationReferenceKind::LGRK_PERSISTED,
															fixture.person_node_id};
	LogicalGraphMutationBatch create;
	create.edge_creates = {{persistedNode, persistedNode, "TRANSIENT_SELF_LOOP", {}}};
	const LogicalGraphMutationBatchResult staged = transaction->stageMutationBatch(std::move(create));
	ASSERT_EQ(staged.created_edge_ids.size(), 1U);

	// Two fixture self-loops plus one newly created self-loop fit this exact
	// bound. A self-loop must consume one incident-Edge unit for its owner.
	LogicalGraphMutationBatch erase;
	erase.node_deletes = {{fixture.person_node_id, true, 3U}};
	EXPECT_TRUE(transaction->stageMutationBatch(std::move(erase)).created_node_ids.empty());
	const LogicalGraphMutationBatchResult committed = transaction->commit();
	EXPECT_TRUE(committed.created_edge_ids.empty());
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));

	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	EXPECT_THROW(static_cast<void>(snapshot->readNode(fixture.person_node_id)), FormatException);
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(staged.created_edge_ids.front())), FormatException);
	snapshot.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, ExplicitlyDeletedCreatedEdgeDoesNotConsumeDetachBudget) {
	const NativeGraphFixture fixture = materializeNativeGraphFixture();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();

	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	const LogicalGraphMutationEntityReference persistedNode{LogicalGraphMutationEntityKind::LGMK_NODE,
															LogicalGraphMutationReferenceKind::LGRK_PERSISTED,
															fixture.person_node_id};
	LogicalGraphMutationBatch create;
	create.edge_creates = {{persistedNode, persistedNode, "CANCELLED_SELF_LOOP", {}}};
	const LogicalGraphMutationBatchResult staged = transaction->stageMutationBatch(std::move(create));
	ASSERT_EQ(staged.created_edge_ids.size(), 1U);

	LogicalGraphMutationBatch erase;
	erase.edge_deletes = {{staged.created_edge_ids.front()}};
	// Only the fixture's two durable self-loops now consume this bound.
	erase.node_deletes = {{fixture.person_node_id, true, 2U}};
	EXPECT_TRUE(transaction->stageMutationBatch(std::move(erase)).created_node_ids.empty());
	const LogicalGraphMutationBatchResult committed = transaction->commit();
	EXPECT_TRUE(committed.created_edge_ids.empty());
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));

	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	EXPECT_THROW(static_cast<void>(snapshot->readNode(fixture.person_node_id)), FormatException);
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(staged.created_edge_ids.front())), FormatException);
	snapshot.reset();
	backend.close();
}
