/**
 * @file test_RelocatableStorageBackendSchema_Durability.cpp
 * @brief Schema DDL durability and recovery contracts for the native backend.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "storage/relocatable/backend/RelocatableStorageBackendSchemaTestFixture.hpp"

#include <atomic>
#include <limits>
#ifndef _WIN32
#include <cerrno>
#endif

#include "graph/storage/relocatable/format/SuperblockFile.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

	[[nodiscard]] LargeValueReachabilityReclaimOptions largeValueReclaimOptions() {
		LargeValueReachabilityReclaimOptions options;
		options.maximum_manifest_locations = 16U;
		options.maximum_manifest_metadata_pages = 16U;
		options.maximum_manifest_nodes = 16U;
		options.maximum_owner_locations = 16U;
		options.maximum_owner_metadata_pages = 16U;
		options.maximum_data_pages = 16U;
		options.maximum_retirements = 32U;
		options.maximum_page_locations = 128U;
		options.maximum_pools = 16U;
		options.first_index_slot = PhysicalSlotId(1U);
		options.last_index_slot = PhysicalSlotId(std::numeric_limits<uint64_t>::max());
		return options;
	}

} // namespace

TEST_F(RelocatableStorageBackendSchemaTest,
	   LargeValueReclamationFinalPublicationSyncFailureRequiresReopenAndCanResume) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::string payload(8U * 1024U, 'x');
	const std::vector<uint64_t> nodes =
			backend.createLogicalNodes({{{"Document"}, {{"body", graph::PropertyValue(payload)}}}});
	ASSERT_EQ(nodes.size(), 1U);
	backend.deleteLogicalNodes({{nodes.front(), false, 0U}});

	std::atomic<bool> inactiveSuperblockWritten = false;
	std::atomic<uint32_t> syncCalls = 0U;
	{
		StorageIO::ScopedNativeWriteHookForTesting observeInactiveSuperblock(
				[&inactiveSuperblockWritten](const file_handle_t fd, const void *bytes, const size_t byteCount,
											 const pwrite_off_t offset) {
					if (byteCount == RSS_PAGE_SIZE &&
						(offset == SuperblockFile::slotOffset(0U) || offset == SuperblockFile::slotOffset(1U))) {
						inactiveSuperblockWritten.store(true, std::memory_order_relaxed);
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		StorageIO::ScopedNativeSyncHookForTesting failFinalPublicationFence(
				[&inactiveSuperblockWritten, &syncCalls](const file_handle_t fd) {
					syncCalls.fetch_add(1U, std::memory_order_relaxed);
					if (inactiveSuperblockWritten.load(std::memory_order_relaxed)) {
#ifdef _WIN32
						SetLastError(ERROR_WRITE_FAULT);
#else
						errno = EIO;
#endif
						return -1;
					}
					return portable_fsync(fd);
				});
		EXPECT_THROW(static_cast<void>(backend.reclaimUnreachableLargeValues(largeValueReclaimOptions())),
					 std::runtime_error);
	}
	EXPECT_TRUE(inactiveSuperblockWritten.load(std::memory_order_relaxed));
	EXPECT_GE(syncCalls.load(std::memory_order_relaxed), 2U);
	EXPECT_THROW(static_cast<void>(backend.reclaimUnreachableLargeValues(largeValueReclaimOptions())),
				 std::logic_error);
	EXPECT_THROW(static_cast<void>(backend.createLogicalNodes({{{"BlockedUntilReopen"}, {}}})), std::logic_error);
	backend.close();

	RelocatableStorageBackend reopened(options());
	reopened.open();
	EXPECT_NO_THROW(static_cast<void>(reopened.reclaimUnreachableLargeValues(largeValueReclaimOptions())));
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendSchemaTest,
	   ScalarIndexDropFinalPublicationSyncFailureRequiresReopenAndExposesOnlyACompleteSchemaGeneration) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::vector<uint64_t> nodes =
			backend.createLogicalNodes({{{"Person"}, {{"score", graph::PropertyValue(int64_t{17})}}}});
	ASSERT_EQ(nodes.size(), 1U);
	const LogicalGraphIndexDefinition index = backend.createLogicalNodeIndex({"person_score", "Person", {"score"}});
	ASSERT_EQ(
			readIndexEntries(index.index_id),
			(std::vector<BTreePageEntry>{
					{IndexKeyCodec::encodeEntryKey(graph::PropertyValue(int64_t{17}), nodes.front()), nodes.front()}}));

	std::atomic<bool> inactiveSuperblockWritten = false;
	std::atomic<uint32_t> syncCalls = 0U;
	{
		StorageIO::ScopedNativeWriteHookForTesting observeInactiveSuperblock(
				[&inactiveSuperblockWritten](const file_handle_t fd, const void *bytes, const size_t byteCount,
											 const pwrite_off_t offset) {
					if (byteCount == RSS_PAGE_SIZE &&
						(offset == SuperblockFile::slotOffset(0U) || offset == SuperblockFile::slotOffset(1U))) {
						inactiveSuperblockWritten.store(true, std::memory_order_relaxed);
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		StorageIO::ScopedNativeSyncHookForTesting failFinalPublicationFence(
				[&inactiveSuperblockWritten, &syncCalls](const file_handle_t fd) {
					syncCalls.fetch_add(1U, std::memory_order_relaxed);
					if (inactiveSuperblockWritten.load(std::memory_order_relaxed)) {
#ifdef _WIN32
						SetLastError(ERROR_WRITE_FAULT);
#else
						errno = EIO;
#endif
						return -1;
					}
					return portable_fsync(fd);
				});
		EXPECT_THROW(static_cast<void>(backend.dropLogicalIndexByName(index.name)), std::runtime_error);
	}
	EXPECT_TRUE(inactiveSuperblockWritten.load(std::memory_order_relaxed));
	EXPECT_GE(syncCalls.load(std::memory_order_relaxed), 2U);
	EXPECT_THROW(static_cast<void>(backend.createLogicalNodes({{{"BlockedUntilReopen"}, {}}})), std::logic_error);
	EXPECT_THROW(static_cast<void>(backend.dropLogicalIndexByName(index.name)), std::logic_error);
	backend.close();

	RelocatableStorageBackend reopened(options());
	reopened.open();
	const std::vector<LogicalGraphIndexDefinition> indexes = reopened.listLogicalIndexes(8U);
	ASSERT_LE(indexes.size(), 1U);
	if (indexes.empty()) {
		EXPECT_FALSE(reopened.dropLogicalIndexByName(index.name));
	} else {
		EXPECT_EQ(indexes.front(), index);
		EXPECT_EQ(readIndexEntries(index.index_id),
				  (std::vector<BTreePageEntry>{
						  {IndexKeyCodec::encodeEntryKey(graph::PropertyValue(int64_t{17}), nodes.front()),
						   nodes.front()}}));
		EXPECT_TRUE(reopened.dropLogicalIndexByName(index.name));
		EXPECT_TRUE(reopened.listLogicalIndexes(8U).empty());
	}
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendSchemaTest,
	   UniqueConstraintDropFinalPublicationSyncFailureRequiresReopenAndExposesOnlyACompleteSchemaGeneration) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes(
			{{{"User"}, {{"email", graph::PropertyValue(std::string("ada@example.test"))}}}});
	ASSERT_EQ(nodes.size(), 1U);
	const LogicalGraphConstraintDefinition constraint =
			backend.createLogicalConstraint({"user_email_unique",
											 LogicalGraphConstraintEntityKind::LGCEK_NODE,
											 LogicalGraphConstraintKind::LGCK_UNIQUE,
											 "User",
											 {"email"},
											 LogicalGraphConstraintPropertyType::LGCPT_NONE});
	ASSERT_FALSE(readIndexEntries(constraint.constraint_id).empty());

	std::atomic<bool> inactiveSuperblockWritten = false;
	std::atomic<uint32_t> syncCalls = 0U;
	{
		StorageIO::ScopedNativeWriteHookForTesting observeInactiveSuperblock(
				[&inactiveSuperblockWritten](const file_handle_t fd, const void *bytes, const size_t byteCount,
											 const pwrite_off_t offset) {
					if (byteCount == RSS_PAGE_SIZE &&
						(offset == SuperblockFile::slotOffset(0U) || offset == SuperblockFile::slotOffset(1U))) {
						inactiveSuperblockWritten.store(true, std::memory_order_relaxed);
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		StorageIO::ScopedNativeSyncHookForTesting failFinalPublicationFence(
				[&inactiveSuperblockWritten, &syncCalls](const file_handle_t fd) {
					syncCalls.fetch_add(1U, std::memory_order_relaxed);
					if (inactiveSuperblockWritten.load(std::memory_order_relaxed)) {
#ifdef _WIN32
						SetLastError(ERROR_WRITE_FAULT);
#else
						errno = EIO;
#endif
						return -1;
					}
					return portable_fsync(fd);
				});
		EXPECT_THROW(static_cast<void>(backend.dropLogicalConstraintByName(constraint.name)), std::runtime_error);
	}
	EXPECT_TRUE(inactiveSuperblockWritten.load(std::memory_order_relaxed));
	EXPECT_GE(syncCalls.load(std::memory_order_relaxed), 2U);
	EXPECT_THROW(static_cast<void>(backend.createLogicalNodes({{{"BlockedUntilReopen"}, {}}})), std::logic_error);
	EXPECT_THROW(static_cast<void>(backend.dropLogicalConstraintByName(constraint.name)), std::logic_error);
	backend.close();

	RelocatableStorageBackend reopened(options());
	reopened.open();
	const std::vector<LogicalGraphConstraintDefinition> constraints = reopened.listLogicalConstraints(8U);
	ASSERT_LE(constraints.size(), 1U);
	if (constraints.empty()) {
		EXPECT_FALSE(reopened.dropLogicalConstraintByName(constraint.name));
	} else {
		EXPECT_EQ(constraints.front(), constraint);
		EXPECT_FALSE(readIndexEntries(constraint.constraint_id).empty());
		EXPECT_TRUE(reopened.dropLogicalConstraintByName(constraint.name));
		EXPECT_TRUE(reopened.listLogicalConstraints(8U).empty());
	}
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendSchemaTest,
	   VectorIndexDropMaintenanceFinalPublicationSyncFailureRequiresReopenAndCanRetryPhysicalRetirement) {
	auto vectorValue = [](double first, double second) {
		return graph::PropertyValue(
				std::vector<graph::PropertyValue>{graph::PropertyValue(first), graph::PropertyValue(second)});
	};
	RelocatableStorageBackend backend(options());
	backend.open();
	static_cast<void>(backend.createLogicalNodes({{{"Person"}, {{"embedding", vectorValue(1.0, -2.0)}}}}));
	const LogicalGraphIndexDefinition index =
			backend.createLogicalNodeVectorIndex({"person_embedding", "Person", "embedding", 2U, "l2"});
	ASSERT_EQ(backend.listLogicalIndexes(8U), std::vector<LogicalGraphIndexDefinition>{index});
	ASSERT_TRUE(backend.dropLogicalIndexByName(index.name));
	EXPECT_TRUE(backend.listLogicalIndexes(8U).empty());

	std::atomic<bool> inactiveSuperblockWritten = false;
	std::atomic<uint32_t> syncCalls = 0U;
	{
		StorageIO::ScopedNativeWriteHookForTesting observeInactiveSuperblock(
				[&inactiveSuperblockWritten](const file_handle_t fd, const void *bytes, const size_t byteCount,
											 const pwrite_off_t offset) {
					if (byteCount == RSS_PAGE_SIZE &&
						(offset == SuperblockFile::slotOffset(0U) || offset == SuperblockFile::slotOffset(1U))) {
						inactiveSuperblockWritten.store(true, std::memory_order_relaxed);
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		StorageIO::ScopedNativeSyncHookForTesting failFinalPublicationFence(
				[&inactiveSuperblockWritten, &syncCalls](const file_handle_t fd) {
					syncCalls.fetch_add(1U, std::memory_order_relaxed);
					if (inactiveSuperblockWritten.load(std::memory_order_relaxed)) {
#ifdef _WIN32
						SetLastError(ERROR_WRITE_FAULT);
#else
						errno = EIO;
#endif
						return -1;
					}
					return portable_fsync(fd);
				});
		EXPECT_THROW(static_cast<void>(backend.reclaimObsoleteVectorIndexState(vectorStateReclaimOptions())),
					 std::runtime_error);
	}
	EXPECT_TRUE(inactiveSuperblockWritten.load(std::memory_order_relaxed));
	EXPECT_GE(syncCalls.load(std::memory_order_relaxed), 2U);
	EXPECT_THROW(static_cast<void>(backend.reclaimObsoleteVectorIndexState(vectorStateReclaimOptions())),
				 std::logic_error);
	EXPECT_THROW(static_cast<void>(backend.createLogicalNodes({{{"BlockedUntilReopen"}, {}}})), std::logic_error);
	backend.close();

	RelocatableStorageBackend reopened(options());
	reopened.open();
	EXPECT_TRUE(reopened.listLogicalIndexes(8U).empty());
	// DROP visibility is already durable. The reclaimer may resume at either
	// side of its first physical publication, but the retry converges to one
	// fully retired dependency graph without resurrecting the schema entry.
	EXPECT_NO_THROW(static_cast<void>(reopened.reclaimObsoleteVectorIndexState(vectorStateReclaimOptions())));
	EXPECT_TRUE(reopened.listLogicalIndexes(8U).empty());
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendSchemaTest,
	   VectorIndexDropMaintenanceSecondPhaseFinalSyncFailureRequiresReopenAndResumesFromTheManifestRetirement) {
	auto vectorValue = [](double first, double second) {
		return graph::PropertyValue(
				std::vector<graph::PropertyValue>{graph::PropertyValue(first), graph::PropertyValue(second)});
	};
	RelocatableStorageBackend backend(options());
	backend.open();
	static_cast<void>(backend.createLogicalNodes({{{"Person"}, {{"embedding", vectorValue(1.0, -2.0)}}}}));
	const LogicalGraphIndexDefinition index =
			backend.createLogicalNodeVectorIndex({"person_embedding", "Person", "embedding", 2U, "l2"});
	ASSERT_TRUE(backend.dropLogicalIndexByName(index.name));

	std::atomic<uint32_t> inactiveSuperblockWrites = 0U;
	std::atomic<uint32_t> syncCalls = 0U;
	{
		StorageIO::ScopedNativeWriteHookForTesting observeInactiveSuperblock(
				[&inactiveSuperblockWrites](const file_handle_t fd, const void *bytes, const size_t byteCount,
											const pwrite_off_t offset) {
					if (byteCount == RSS_PAGE_SIZE &&
						(offset == SuperblockFile::slotOffset(0U) || offset == SuperblockFile::slotOffset(1U))) {
						inactiveSuperblockWrites.fetch_add(1U, std::memory_order_relaxed);
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		StorageIO::ScopedNativeSyncHookForTesting failSecondRetirementPublicationFence(
				[&inactiveSuperblockWrites, &syncCalls](const file_handle_t fd) {
					syncCalls.fetch_add(1U, std::memory_order_relaxed);
					if (inactiveSuperblockWrites.load(std::memory_order_relaxed) >= 2U) {
#ifdef _WIN32
						SetLastError(ERROR_WRITE_FAULT);
#else
						errno = EIO;
#endif
						return -1;
					}
					return portable_fsync(fd);
				});
		EXPECT_THROW(static_cast<void>(backend.reclaimObsoleteVectorIndexState(vectorStateReclaimOptions())),
					 std::runtime_error);
	}
	EXPECT_GE(inactiveSuperblockWrites.load(std::memory_order_relaxed), 2U);
	EXPECT_GE(syncCalls.load(std::memory_order_relaxed), 4U);
	EXPECT_THROW(static_cast<void>(backend.reclaimObsoleteVectorIndexState(vectorStateReclaimOptions())),
				 std::logic_error);
	backend.close();

	RelocatableStorageBackend reopened(options());
	reopened.open();
	EXPECT_TRUE(reopened.listLogicalIndexes(8U).empty());
	const VectorIndexStateReclaimReport report = reopened.reclaimObsoleteVectorIndexState(vectorStateReclaimOptions());
	EXPECT_EQ(report.manifests.dropped_manifests_retired, 0U);
	// The second phase may have reached durable publication before its final
	// sync reported failure, so retrying it is intentionally idempotent.
	EXPECT_TRUE(reopened.listLogicalIndexes(8U).empty());
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}
