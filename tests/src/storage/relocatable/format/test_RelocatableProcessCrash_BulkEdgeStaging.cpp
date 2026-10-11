/**
 * @file test_RelocatableProcessCrash_BulkEdgeStaging.cpp
 * @brief Real process-crash recovery for allocator-managed streamed Edge scratch.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#include <array>
#include <atomic>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/allocation/SharedPagePool.hpp"
#include "graph/storage/relocatable/adjacency/AdjacencyKeyCodec.hpp"
#include "graph/storage/relocatable/backend/RelocatableStorageBackend.hpp"
#include "graph/storage/relocatable/format/SuperblockFile.hpp"
#include "graph/storage/relocatable/metadata/CatalogPage.hpp"
#include "storage/relocatable/format/RelocatableProcessCrashSupport.hpp"

using namespace graph::storage;

namespace {

	constexpr std::string_view CRASH_CHILD_BULK_EDGE_SCRATCH = "bulk-edge-scratch";
	constexpr std::string_view CRASH_CHILD_BULK_EDGE_BEFORE_PUBLICATION = "bulk-edge-before-publication";
	constexpr std::string_view CRASH_CHILD_BULK_EDGE_PROPERTY_BEFORE_PUBLICATION =
			"bulk-edge-property-before-publication";
	constexpr std::string_view CRASH_CHILD_BULK_EDGE_LARGE_VALUE_DATA_BEFORE_PUBLICATION =
			"bulk-edge-large-value-data-before-publication";
	constexpr std::string_view CRASH_CHILD_BULK_EDGE_LARGE_VALUE_MANIFEST_BEFORE_PUBLICATION =
			"bulk-edge-large-value-manifest-before-publication";
	constexpr std::string_view CRASH_CHILD_BULK_EDGE_ADJACENCY_INDEX_BEFORE_PUBLICATION =
			"bulk-edge-adjacency-index-before-publication";
	constexpr std::string_view CRASH_CHILD_BULK_EDGE_CATALOG_METADATA_BEFORE_PUBLICATION =
			"bulk-edge-catalog-metadata-before-publication";
	constexpr std::string_view CRASH_CHILD_BULK_EDGE_AFTER_PUBLICATION = "bulk-edge-after-publication";
	constexpr std::string_view CRASH_CHILD_BULK_EDGE_SESSION_LEASE = "bulk-edge-session-lease";

	using graph::test::relocatable_crash::CrashChild;
	using graph::test::relocatable_crash::waitToBeKilled;
	using graph::test::relocatable_crash::writeFailureMarker;
	using graph::test::relocatable_crash::writeMarker;

} // namespace

namespace graph::test {

	int runRelocatableBulkEdgeStagingCrashChild(std::string_view mode, const std::filesystem::path &databasePath,
													 const std::filesystem::path &markerPath) {
		if (mode != CRASH_CHILD_BULK_EDGE_SCRATCH && mode != CRASH_CHILD_BULK_EDGE_BEFORE_PUBLICATION &&
			mode != CRASH_CHILD_BULK_EDGE_PROPERTY_BEFORE_PUBLICATION &&
			mode != CRASH_CHILD_BULK_EDGE_LARGE_VALUE_DATA_BEFORE_PUBLICATION &&
			mode != CRASH_CHILD_BULK_EDGE_LARGE_VALUE_MANIFEST_BEFORE_PUBLICATION &&
			mode != CRASH_CHILD_BULK_EDGE_ADJACENCY_INDEX_BEFORE_PUBLICATION &&
			mode != CRASH_CHILD_BULK_EDGE_CATALOG_METADATA_BEFORE_PUBLICATION &&
			mode != CRASH_CHILD_BULK_EDGE_AFTER_PUBLICATION && mode != CRASH_CHILD_BULK_EDGE_SESSION_LEASE) {
			return -1;
		}
		try {
			RelocatableStorageBackendOptions options;
			options.data_path = databasePath;
			options.open_mode = OpenMode::OPEN_EXISTING_FILE;
			RelocatableStorageBackend backend(std::move(options));
			backend.open();

			std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
			auto *const bulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(transaction.get());
			if (bulk == nullptr) {
				throw std::runtime_error("relocatable backend did not expose bounded bulk Edge staging");
			}
			const std::array<uint64_t, 1U> sources{1U};
			const std::array<uint64_t, 1U> targets{2U};
			const bool largeValuePublication = mode == CRASH_CHILD_BULK_EDGE_LARGE_VALUE_DATA_BEFORE_PUBLICATION ||
					mode == CRASH_CHILD_BULK_EDGE_LARGE_VALUE_MANIFEST_BEFORE_PUBLICATION;
			const std::string largeValuePayload(largeValuePublication ? RSS_SEGMENT_SIZE / 2U : 0U, 'x');
			const std::array<PropertyValue, 1U> values{
					PropertyValue(largeValuePublication ? largeValuePayload : "crash-property")};
			const std::array<LogicalGraphBulkEdgePropertyColumn, 1U> columns{{{"payload", values}}};
			const bool propertyPublication = mode == CRASH_CHILD_BULK_EDGE_PROPERTY_BEFORE_PUBLICATION;
			const bool adjacencyIndexPublication = mode == CRASH_CHILD_BULK_EDGE_ADJACENCY_INDEX_BEFORE_PUBLICATION;
			const bool catalogMetadataPublication = mode == CRASH_CHILD_BULK_EDGE_CATALOG_METADATA_BEFORE_PUBLICATION;
			const bool candidateWriteBeforeDeath =
					propertyPublication || largeValuePublication || adjacencyIndexPublication || catalogMetadataPublication;
			const PageOwnerKind candidateOwner = mode == CRASH_CHILD_BULK_EDGE_LARGE_VALUE_DATA_BEFORE_PUBLICATION
													 ? PageOwnerKind::POK_LARGE_VALUE_DATA
													 : mode == CRASH_CHILD_BULK_EDGE_LARGE_VALUE_MANIFEST_BEFORE_PUBLICATION
																 ? PageOwnerKind::POK_LARGE_VALUE_MANIFEST
																 : adjacencyIndexPublication ? PageOwnerKind::POK_INDEX
																								  : propertyPublication ? PageOwnerKind::POK_PROPERTY
																													  : PageOwnerKind::POK_ENTITY;
			static_cast<void>(bulk->appendPersistedBulkEdges(
					{"PRIVATE", sources, targets, candidateWriteBeforeDeath ? std::span<const LogicalGraphBulkEdgePropertyColumn>(columns) :
																std::span<const LogicalGraphBulkEdgePropertyColumn>{}}));
			if (mode == CRASH_CHILD_BULK_EDGE_SESSION_LEASE) {
				writeMarker(markerPath);
				waitToBeKilled();
			}
			if (mode == CRASH_CHILD_BULK_EDGE_BEFORE_PUBLICATION || candidateWriteBeforeDeath) {
				std::atomic<bool> observedPublicPage{false};
				StorageIO::ScopedNativeWriteHookForTesting pauseBeforeGraphPublication(
						[&observedPublicPage, &markerPath, candidateWriteBeforeDeath, candidateOwner, adjacencyIndexPublication,
								catalogMetadataPublication](file_handle_t fd, const void *bytes,
								size_t byteCount, pwrite_off_t offset) {
							if (byteCount == RSS_PAGE_SIZE && !observedPublicPage.load(std::memory_order_relaxed)) {
								try {
									const auto page =
											std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount);
									bool matchesCandidate = false;
									if (catalogMetadataPublication) {
										static_cast<void>(CatalogLeafPageCodec::decode(page));
										matchesCandidate = true;
									} else {
										const PageOwnerHeader owner =
												PageOwnerHeaderCodec::decode(page.first(RSS_PAGE_OWNER_HEADER_BYTES));
										matchesCandidate = owner.kind == candidateOwner &&
												(!adjacencyIndexPublication || owner.owner_id == RSS_GLOBAL_ADJACENCY_INDEX_ID);
									}
									if (matchesCandidate) {
										observedPublicPage.store(true, std::memory_order_relaxed);
										if (candidateWriteBeforeDeath) {
											const pwrite_ssize_t written = portable_pwrite(fd, bytes, byteCount, offset);
											if (written != static_cast<pwrite_ssize_t>(byteCount)) {
												return written;
											}
										}
										writeMarker(markerPath);
										waitToBeKilled();
									}
								} catch (const FormatException &) {
									// Private scratch and metadata writes cannot expose a graph route.
								}
							}
							return portable_pwrite(fd, bytes, byteCount, offset);
						});
				static_cast<void>(transaction->commit());
				return 102;
			}
			if (mode == CRASH_CHILD_BULK_EDGE_AFTER_PUBLICATION) {
				std::atomic<bool> observedPublication{false};
				StorageIO::ScopedNativeWriteHookForTesting pauseAfterGraphPublication(
						[&observedPublication, &markerPath](file_handle_t fd, const void *bytes, size_t byteCount,
																		pwrite_off_t offset) {
							if (byteCount == RSS_PAGE_SIZE &&
								(offset == SuperblockFile::slotOffset(0U) || offset == SuperblockFile::slotOffset(1U)) &&
								!observedPublication.load(std::memory_order_relaxed)) {
								try {
									const Superblock candidate = SuperblockCodec::decode(
											std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount));
									if (candidate.edge_entity_directory_root.isValid()) {
										observedPublication.store(true, std::memory_order_relaxed);
										const pwrite_ssize_t written = portable_pwrite(fd, bytes, byteCount, offset);
										if (written != static_cast<pwrite_ssize_t>(byteCount)) {
											return written;
										}
										writeMarker(markerPath);
										waitToBeKilled();
									}
								} catch (const FormatException &) {
									// Ignore non-Superblock control writes while locating the final route.
								}
							}
							return portable_pwrite(fd, bytes, byteCount, offset);
						});
				static_cast<void>(transaction->commit());
				return 102;
			}
			writeMarker(markerPath);
			waitToBeKilled();
		} catch (const std::exception &error) {
			writeFailureMarker(markerPath, error.what());
			return 101;
		}
	}

} // namespace graph::test

class RelocatableProcessCrashBulkEdgeStagingTest : public ::testing::Test {
protected:
	std::filesystem::path databasePath;
	std::filesystem::path markerPath;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		databasePath = std::filesystem::temp_directory_path() /
				("relocatable_bulk_edge_scratch_" + to_string(uuid) + ".db");
		markerPath = databasePath.string() + ".marker";
	}

	void TearDown() override {
		std::error_code error;
		std::filesystem::remove(databasePath, error);
		std::filesystem::remove(markerPath, error);
		std::filesystem::remove(markerPath.string() + ".error", error);
	}
};

TEST_F(RelocatableProcessCrashBulkEdgeStagingTest,
		KillAfterScratchWriteReclaimsCatalogedScratchOnWritableReopenWithoutExposingAnEdge) {
	RelocatableStorageBackendOptions createOptions;
	createOptions.data_path = databasePath;
	createOptions.open_mode = OpenMode::OPEN_CREATE_NEW_FILE;
	auto created = RelocatableStorageBackend::initializeNew(std::move(createOptions));
	ASSERT_NE(created, nullptr);
	created->open();
	const std::vector<uint64_t> nodes =
			created->createLogicalNodes({{{"CrashSource"}, {}}, {{"CrashTarget"}, {}}});
	ASSERT_EQ(nodes, (std::vector<uint64_t>{1U, 2U}));
	created->close();
	const uintmax_t initialBytes = std::filesystem::file_size(databasePath);

	CrashChild child(databasePath, markerPath, CRASH_CHILD_BULK_EDGE_SCRATCH);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	RelocatableStorageBackendOptions reopenOptions;
	reopenOptions.data_path = databasePath;
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend reopened(std::move(reopenOptions));
	ASSERT_NO_THROW(reopened.open());
	EXPECT_GE(std::filesystem::file_size(databasePath), initialBytes);
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(1U)), FormatException);
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	EXPECT_NO_THROW(reopened.close());
}

TEST_F(RelocatableProcessCrashBulkEdgeStagingTest,
		LiveCrossProcessBulkSessionRejectsAnotherSessionAndCrashReleasesTheLeaseForRetry) {
	RelocatableStorageBackendOptions createOptions;
	createOptions.data_path = databasePath;
	createOptions.open_mode = OpenMode::OPEN_CREATE_NEW_FILE;
	auto created = RelocatableStorageBackend::initializeNew(std::move(createOptions));
	ASSERT_NE(created, nullptr);
	created->open();
	ASSERT_EQ(created->createLogicalNodes({{{"CrashSource"}, {}}, {{"CrashTarget"}, {}}}),
			  (std::vector<uint64_t>{1U, 2U}));
	created->close();

	CrashChild child(databasePath, markerPath, CRASH_CHILD_BULK_EDGE_SESSION_LEASE);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();

	RelocatableStorageBackendOptions competingOptions;
	competingOptions.data_path = databasePath;
	competingOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend competing(std::move(competingOptions));
	ASSERT_NO_THROW(competing.open());
	std::unique_ptr<LogicalGraphTransaction> competingTransaction = competing.beginLogicalGraphTransaction(false);
	auto *const competingBulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(competingTransaction.get());
	ASSERT_NE(competingBulk, nullptr);
	const std::array<uint64_t, 1U> sources{1U};
	const std::array<uint64_t, 1U> targets{2U};
	EXPECT_THROW(static_cast<void>(competingBulk->appendPersistedBulkEdges({"COMPETING", sources, targets, {}})),
			 LogicalGraphMutationConflict);
	EXPECT_NO_THROW(competingTransaction->rollback());
	competingTransaction.reset();
	competing.close();

	child.terminate();

	RelocatableStorageBackendOptions retryOptions;
	retryOptions.data_path = databasePath;
	retryOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend retry(std::move(retryOptions));
	ASSERT_NO_THROW(retry.open());
	std::unique_ptr<LogicalGraphTransaction> retryTransaction = retry.beginLogicalGraphTransaction(false);
	auto *const retryBulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(retryTransaction.get());
	ASSERT_NE(retryBulk, nullptr);
	const std::vector<uint64_t> retryIds = retryBulk->appendPersistedBulkEdges({"RETRY", sources, targets, {}});
	ASSERT_EQ(retryIds.size(), 1U);
	EXPECT_NO_THROW(static_cast<void>(retryTransaction->commit()));
	retryTransaction.reset();
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = retry.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(retryIds.front()), LogicalGraphEdge(retryIds.front(), 1U, 2U, "RETRY", {}));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(retry.verifyIntegrity()));
	EXPECT_NO_THROW(retry.close());
}

TEST_F(RelocatableProcessCrashBulkEdgeStagingTest,
		ReadOnlyOpenLeavesOrphanScratchUntouchedWithoutExposingAnEdge) {
	RelocatableStorageBackendOptions createOptions;
	createOptions.data_path = databasePath;
	createOptions.open_mode = OpenMode::OPEN_CREATE_NEW_FILE;
	auto created = RelocatableStorageBackend::initializeNew(std::move(createOptions));
	ASSERT_NE(created, nullptr);
	created->open();
	const std::vector<uint64_t> nodes =
			created->createLogicalNodes({{{"CrashSource"}, {}}, {{"CrashTarget"}, {}}});
	ASSERT_EQ(nodes, (std::vector<uint64_t>{1U, 2U}));
	created->close();

	CrashChild child(databasePath, markerPath, CRASH_CHILD_BULK_EDGE_SCRATCH);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();
	const uintmax_t bytesBeforeReadOnlyOpen = std::filesystem::file_size(databasePath);
	EXPECT_FALSE(std::filesystem::exists(databasePath.string() + ".wal"));

	RelocatableStorageBackendOptions readOnlyOptions;
	readOnlyOptions.data_path = databasePath;
	readOnlyOptions.open_mode = OpenMode::OPEN_READ_ONLY_EXISTING_FILE;
	RelocatableStorageBackend readOnly(std::move(readOnlyOptions));
	ASSERT_NO_THROW(readOnly.open());
	EXPECT_TRUE(readOnly.isReadOnly());
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = readOnly.openLogicalGraphReadSnapshot();
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(1U)), FormatException);
	snapshot.reset();
	EXPECT_NO_THROW(readOnly.close());
	EXPECT_EQ(std::filesystem::file_size(databasePath), bytesBeforeReadOnlyOpen);
	EXPECT_FALSE(std::filesystem::exists(databasePath.string() + ".wal"));
}

TEST_F(RelocatableProcessCrashBulkEdgeStagingTest,
		KillAfterGraphPublicationBeforeFinalSyncRecoversTheCompleteNewGeneration) {
	RelocatableStorageBackendOptions createOptions;
	createOptions.data_path = databasePath;
	createOptions.open_mode = OpenMode::OPEN_CREATE_NEW_FILE;
	auto created = RelocatableStorageBackend::initializeNew(std::move(createOptions));
	ASSERT_NE(created, nullptr);
	created->open();
	const std::vector<uint64_t> nodes =
			created->createLogicalNodes({{{"CrashSource"}, {}}, {{"CrashTarget"}, {}}});
	ASSERT_EQ(nodes, (std::vector<uint64_t>{1U, 2U}));
	created->close();

	CrashChild child(databasePath, markerPath, CRASH_CHILD_BULK_EDGE_AFTER_PUBLICATION);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	RelocatableStorageBackendOptions reopenOptions;
	reopenOptions.data_path = databasePath;
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend reopened(std::move(reopenOptions));
	ASSERT_NO_THROW(reopened.open());
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(1U), LogicalGraphEdge(1U, nodes[0], nodes[1], "PRIVATE"));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	EXPECT_NO_THROW(reopened.close());
}

TEST_F(RelocatableProcessCrashBulkEdgeStagingTest,
		KillBeforeGraphPublicationRetainsTheCompleteOldGeneration) {
	RelocatableStorageBackendOptions createOptions;
	createOptions.data_path = databasePath;
	createOptions.open_mode = OpenMode::OPEN_CREATE_NEW_FILE;
	auto created = RelocatableStorageBackend::initializeNew(std::move(createOptions));
	ASSERT_NE(created, nullptr);
	created->open();
	const std::vector<uint64_t> nodes =
			created->createLogicalNodes({{{"CrashSource"}, {}}, {{"CrashTarget"}, {}}});
	ASSERT_EQ(nodes, (std::vector<uint64_t>{1U, 2U}));
	created->close();

	CrashChild child(databasePath, markerPath, CRASH_CHILD_BULK_EDGE_BEFORE_PUBLICATION);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	RelocatableStorageBackendOptions reopenOptions;
	reopenOptions.data_path = databasePath;
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend reopened(std::move(reopenOptions));
	ASSERT_NO_THROW(reopened.open());
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(1U)), FormatException);
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	EXPECT_NO_THROW(reopened.close());
}

TEST_F(RelocatableProcessCrashBulkEdgeStagingTest,
		KillAfterPropertyPageWriteBeforeGraphPublicationRetainsTheCompleteOldGeneration) {
	RelocatableStorageBackendOptions createOptions;
	createOptions.data_path = databasePath;
	createOptions.open_mode = OpenMode::OPEN_CREATE_NEW_FILE;
	auto created = RelocatableStorageBackend::initializeNew(std::move(createOptions));
	ASSERT_NE(created, nullptr);
	created->open();
	const std::vector<uint64_t> nodes =
			created->createLogicalNodes({{{"CrashSource"}, {}}, {{"CrashTarget"}, {}}});
	ASSERT_EQ(nodes, (std::vector<uint64_t>{1U, 2U}));
	created->close();

	CrashChild child(databasePath, markerPath, CRASH_CHILD_BULK_EDGE_PROPERTY_BEFORE_PUBLICATION);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	RelocatableStorageBackendOptions reopenOptions;
	reopenOptions.data_path = databasePath;
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend reopened(std::move(reopenOptions));
	ASSERT_NO_THROW(reopened.open());
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(1U)), FormatException);
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	EXPECT_NO_THROW(reopened.close());
}

TEST_F(RelocatableProcessCrashBulkEdgeStagingTest,
		KillAfterLargeValueDataPageWriteBeforeGraphPublicationRetainsTheCompleteOldGeneration) {
	RelocatableStorageBackendOptions createOptions;
	createOptions.data_path = databasePath;
	createOptions.open_mode = OpenMode::OPEN_CREATE_NEW_FILE;
	auto created = RelocatableStorageBackend::initializeNew(std::move(createOptions));
	ASSERT_NE(created, nullptr);
	created->open();
	const std::vector<uint64_t> nodes =
			created->createLogicalNodes({{{"CrashSource"}, {}}, {{"CrashTarget"}, {}}});
	ASSERT_EQ(nodes, (std::vector<uint64_t>{1U, 2U}));
	created->close();

	CrashChild child(databasePath, markerPath, CRASH_CHILD_BULK_EDGE_LARGE_VALUE_DATA_BEFORE_PUBLICATION);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	RelocatableStorageBackendOptions reopenOptions;
	reopenOptions.data_path = databasePath;
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend reopened(std::move(reopenOptions));
	ASSERT_NO_THROW(reopened.open());
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(1U)), FormatException);
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	EXPECT_NO_THROW(reopened.close());
}

TEST_F(RelocatableProcessCrashBulkEdgeStagingTest,
		KillAfterLargeValueManifestPageWriteBeforeGraphPublicationRetainsTheCompleteOldGeneration) {
	RelocatableStorageBackendOptions createOptions;
	createOptions.data_path = databasePath;
	createOptions.open_mode = OpenMode::OPEN_CREATE_NEW_FILE;
	auto created = RelocatableStorageBackend::initializeNew(std::move(createOptions));
	ASSERT_NE(created, nullptr);
	created->open();
	const std::vector<uint64_t> nodes =
			created->createLogicalNodes({{{"CrashSource"}, {}}, {{"CrashTarget"}, {}}});
	ASSERT_EQ(nodes, (std::vector<uint64_t>{1U, 2U}));
	created->close();

	CrashChild child(databasePath, markerPath, CRASH_CHILD_BULK_EDGE_LARGE_VALUE_MANIFEST_BEFORE_PUBLICATION);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	RelocatableStorageBackendOptions reopenOptions;
	reopenOptions.data_path = databasePath;
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend reopened(std::move(reopenOptions));
	ASSERT_NO_THROW(reopened.open());
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(1U)), FormatException);
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	EXPECT_NO_THROW(reopened.close());
}

TEST_F(RelocatableProcessCrashBulkEdgeStagingTest,
		KillAfterGlobalAdjacencyIndexPageWriteBeforeGraphPublicationRetainsTheCompleteOldGeneration) {
	RelocatableStorageBackendOptions createOptions;
	createOptions.data_path = databasePath;
	createOptions.open_mode = OpenMode::OPEN_CREATE_NEW_FILE;
	auto created = RelocatableStorageBackend::initializeNew(std::move(createOptions));
	ASSERT_NE(created, nullptr);
	created->open();
	const std::vector<uint64_t> nodes =
			created->createLogicalNodes({{{"CrashSource"}, {}}, {{"CrashTarget"}, {}}});
	ASSERT_EQ(nodes, (std::vector<uint64_t>{1U, 2U}));
	const std::vector<uint64_t> oldEdges = created->createLogicalEdges({{nodes[0], nodes[1], "EXISTING", {}}});
	ASSERT_EQ(oldEdges, (std::vector<uint64_t>{1U}));
	created->close();

	CrashChild child(databasePath, markerPath, CRASH_CHILD_BULK_EDGE_ADJACENCY_INDEX_BEFORE_PUBLICATION);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	RelocatableStorageBackendOptions reopenOptions;
	reopenOptions.data_path = databasePath;
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend reopened(std::move(reopenOptions));
	ASSERT_NO_THROW(reopened.open());
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(oldEdges.front()), LogicalGraphEdge(oldEdges.front(), nodes[0], nodes[1], "EXISTING"));
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(2U)), FormatException);
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	EXPECT_NO_THROW(reopened.close());
}

TEST_F(RelocatableProcessCrashBulkEdgeStagingTest,
		KillAfterCatalogMetadataPageWriteBeforeGraphPublicationRetainsTheCompleteOldGeneration) {
	RelocatableStorageBackendOptions createOptions;
	createOptions.data_path = databasePath;
	createOptions.open_mode = OpenMode::OPEN_CREATE_NEW_FILE;
	auto created = RelocatableStorageBackend::initializeNew(std::move(createOptions));
	ASSERT_NE(created, nullptr);
	created->open();
	const std::vector<uint64_t> nodes =
			created->createLogicalNodes({{{"CrashSource"}, {}}, {{"CrashTarget"}, {}}});
	ASSERT_EQ(nodes, (std::vector<uint64_t>{1U, 2U}));
	const std::vector<uint64_t> oldEdges = created->createLogicalEdges({{nodes[0], nodes[1], "EXISTING", {}}});
	ASSERT_EQ(oldEdges, (std::vector<uint64_t>{1U}));
	created->close();

	CrashChild child(databasePath, markerPath, CRASH_CHILD_BULK_EDGE_CATALOG_METADATA_BEFORE_PUBLICATION);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	RelocatableStorageBackendOptions reopenOptions;
	reopenOptions.data_path = databasePath;
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend reopened(std::move(reopenOptions));
	ASSERT_NO_THROW(reopened.open());
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(oldEdges.front()), LogicalGraphEdge(oldEdges.front(), nodes[0], nodes[1], "EXISTING"));
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(2U)), FormatException);
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	EXPECT_NO_THROW(reopened.close());
}
