/**
 * @file test_RelocatableStorageBackend.cpp
 * @brief Integration tests for the format-safe relocatable logical read backend.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/
#include "storage/relocatable/backend/RelocatableStorageBackendTestFixture.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <span>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace relocatable_storage_backend_test;

namespace {

	template<typename Predicate>
	bool waitUntil(Predicate predicate) {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (std::chrono::steady_clock::now() < deadline) {
			if (predicate()) {
				return true;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return predicate();
	}

} // namespace

TEST_F(RelocatableStorageBackendTest, CypherWriteOnlyCreateUsesTheLogicalMutationBatch) {
	materializeNativeDatabase();

	zyx::Database database(data_path_.string());
	database.open();
	const zyx::Result create = database.execute(
			"CREATE (a:CypherOnline {name: 'Ada'})-[r:CONNECTS {since: 2026}]->(b:CypherOnline {name: 'Bob'}) "
			"RETURN a, r, b");
	ASSERT_TRUE(create.isSuccess()) << create.getError();
	ASSERT_EQ(create.getColumnCount(), 3);
	ASSERT_TRUE(create.hasNext());
	create.next();
	const auto createdSource = std::get<std::shared_ptr<zyx::Node>>(create.get("a"));
	const auto createdEdge = std::get<std::shared_ptr<zyx::Edge>>(create.get("r"));
	const auto createdTarget = std::get<std::shared_ptr<zyx::Node>>(create.get("b"));
	ASSERT_NE(createdSource, nullptr);
	ASSERT_NE(createdEdge, nullptr);
	ASSERT_NE(createdTarget, nullptr);
	EXPECT_EQ(std::get<std::string>(createdSource->properties.at("name")), "Ada");
	EXPECT_EQ(std::get<int64_t>(createdEdge->properties.at("since")), 2026);
	EXPECT_EQ(std::get<std::string>(createdTarget->properties.at("name")), "Bob");

	const zyx::Result parameterized = database.execute("CREATE (n:CypherParam {score: $score}) RETURN n.score AS score",
													   {{"score", int64_t(17)}});
	ASSERT_TRUE(parameterized.isSuccess()) << parameterized.getError();
	ASSERT_TRUE(parameterized.hasNext());
	parameterized.next();
	EXPECT_EQ(std::get<int64_t>(parameterized.get("score")), 17);

	const zyx::Result query = database.execute("MATCH (a:CypherOnline)-[r:CONNECTS]->(b:CypherOnline) RETURN a.name AS "
											   "source, r.since AS since, b.name AS target");
	ASSERT_TRUE(query.isSuccess()) << query.getError();
	ASSERT_TRUE(query.hasNext());
	query.next();
	EXPECT_EQ(std::get<std::string>(query.get("source")), "Ada");
	EXPECT_EQ(std::get<int64_t>(query.get("since")), 2026);
	EXPECT_EQ(std::get<std::string>(query.get("target")), "Bob");
	database.close();
}

TEST_F(RelocatableStorageBackendTest, SequentialAutocommitCreateDeleteReusesCowPoolCapacity) {
	materializeNativeDatabase();

	zyx::Database database(data_path_.string());
	database.open();
	for (int64_t id = 0; id < 25; ++id) {
		const zyx::Result create = database.execute("CREATE (:SequentialCow {id: " + std::to_string(id) + "})");
		ASSERT_TRUE(create.isSuccess()) << "create #" << id << ": " << create.getError();
	}
	for (int64_t id = 0; id < 25; ++id) {
		const zyx::Result erase =
				database.execute("MATCH (node:SequentialCow {id: " + std::to_string(id) + "}) DELETE node");
		ASSERT_TRUE(erase.isSuccess()) << "delete #" << id << ": " << erase.getError();
	}
	const zyx::Result remaining = database.execute("MATCH (node:SequentialCow) RETURN count(node) AS count");
	ASSERT_TRUE(remaining.isSuccess()) << remaining.getError();
	ASSERT_TRUE(remaining.hasNext());
	remaining.next();
	EXPECT_EQ(std::get<int64_t>(remaining.get("count")), 0);
	database.close();
}

TEST_F(RelocatableStorageBackendTest, RepeatedOrdinaryAdjacencyChurnRetiresEmptyRootsAcrossReopen) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableMaintenanceOptions maintenance;
	maintenance.loop.maximum_outstanding_tasks = 2U;
	maintenance.loop.minimum_cycle_interval = std::chrono::milliseconds(1);
	options.maintenance = maintenance;
	auto backend = std::make_unique<RelocatableStorageBackend>(options);
	backend->open();
	const std::vector<uint64_t> nodes =
			backend->createLogicalNodes({{{"AdjacencyChurnSource"}, {}}, {{"AdjacencyChurnTarget"}, {}}});
	ASSERT_EQ(nodes.size(), 2U);

	const auto verifyReleasedPool = [&](PhysicalSlotId releasedSlot) {
		auto data = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
		ASSERT_TRUE(data->is_open());
		auto io = std::make_shared<StorageIO>(data, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		auto metadata = RelocatableMetadataStore::open(io);
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		EXPECT_FALSE(metadata->findIndexRootOptional(snapshot, RSS_GLOBAL_ADJACENCY_INDEX_ID).has_value());
		EXPECT_FALSE(metadata->isPhysicalSlotAllocated(snapshot, releasedSlot));
		snapshot.release();
		metadata.reset();
		io.reset();
		data->close();
	};

	std::optional<PhysicalSlotId> previousReleasedSlot;
	for (size_t round = 0U; round < 8U; ++round) {
		std::vector<uint64_t> edges;
		edges.reserve(16U);
		for (size_t ordinal = 0U; ordinal < 16U; ++ordinal) {
			const std::vector<uint64_t> created =
					backend->createLogicalEdges({{nodes[0], nodes[1], "ADJACENCY_CHURN", {}}});
			ASSERT_EQ(created.size(), 1U);
			edges.push_back(created.front());
		}
		PhysicalSlotId currentPoolSlot;
		{
			auto data = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
			ASSERT_TRUE(data->is_open());
			auto io = std::make_shared<StorageIO>(data, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
			auto metadata = RelocatableMetadataStore::open(io);
			MetadataSnapshotPin metadataSnapshot = metadata->openSnapshot();
			const std::optional<IndexRootLocation> root =
					metadata->findIndexRootOptional(metadataSnapshot, RSS_GLOBAL_ADJACENCY_INDEX_ID);
			ASSERT_TRUE(root.has_value());
			const PageLocation location = metadata->findPageLocation(metadataSnapshot, root->root_page_id);
			const CatalogEntry pool = metadata->findCatalog(metadataSnapshot, location.segment_id);
			EXPECT_EQ(pool.owner_id, RSS_GLOBAL_ADJACENCY_INDEX_ID);
			EXPECT_TRUE(metadata->isPhysicalSlotAllocated(metadataSnapshot, pool.physical_slot));
			currentPoolSlot = pool.physical_slot;
			if (previousReleasedSlot.has_value()) {
				EXPECT_EQ(currentPoolSlot, *previousReleasedSlot);
			}
			metadataSnapshot.release();
			metadata.reset();
			io.reset();
			data->close();
		}

		std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend->openLogicalGraphReadSnapshot();
		std::optional<LogicalGraphAdjacencyContinuation> continuation;
		size_t seen = 0U;
		do {
			const LogicalGraphAdjacencyPage page =
					snapshot->scanAdjacentEdges(nodes[0], LogicalGraphReadDirection::LGRD_OUTGOING, continuation, 7U);
			seen += page.edges.size();
			continuation = page.continuation;
		} while (continuation.has_value());
		EXPECT_EQ(seen, edges.size());
		snapshot.reset();

		const uint64_t releasedBefore = backend->maintenanceMetrics().global_adjacency_pools_released;
		for (const uint64_t edgeId: edges) {
			backend->deleteLogicalEdges({{edgeId}});
		}
		ASSERT_TRUE(waitUntil([&] {
			const RelocatableMaintenanceMetrics metrics = backend->maintenanceMetrics();
			return metrics.global_adjacency_pools_released > releasedBefore ||
				   metrics.loop.state == RelocatableMaintenanceLoopState::VMLS_FAULTED;
		}));
		if (const std::exception_ptr failure = backend->maintenanceFailure(); failure) {
			try {
				std::rethrow_exception(failure);
			} catch (const std::exception &error) {
				FAIL() << "global adjacency pool churn reclamation failed: " << error.what();
			}
		}
		EXPECT_NO_THROW(static_cast<void>(backend->verifyIntegrity()));
		backend->close();
		verifyReleasedPool(currentPoolSlot);
		previousReleasedSlot = currentPoolSlot;

		backend = std::make_unique<RelocatableStorageBackend>(options);
		backend->open();
		snapshot = backend->openLogicalGraphReadSnapshot();
		EXPECT_TRUE(snapshot->scanAdjacentEdges(nodes[0], LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 1U)
							.edges.empty());
		EXPECT_TRUE(snapshot->scanAdjacentEdges(nodes[1], LogicalGraphReadDirection::LGRD_INCOMING, std::nullopt, 1U)
							.edges.empty());
	}

	EXPECT_NO_THROW(static_cast<void>(backend->verifyIntegrity()));
	backend->close();
}

TEST_F(RelocatableStorageBackendTest, OrdinaryAdjacencyPoolReclamationDefersSlotReuseUntilTheOldSnapshotDrains) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableMaintenanceOptions maintenance;
	maintenance.loop.maximum_outstanding_tasks = 2U;
	maintenance.loop.minimum_cycle_interval = std::chrono::milliseconds(1);
	options.maintenance = maintenance;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> nodes =
			backend.createLogicalNodes({{{"PoolReclaimSource"}, {}}, {{"PoolReclaimTarget"}, {}}});
	ASSERT_EQ(nodes.size(), 2U);
	const std::vector<uint64_t> edges = backend.createLogicalEdges({{nodes[0], nodes[1], "POOL_RECLAIM", {}}});
	ASSERT_EQ(edges.size(), 1U);
	CatalogEntry originalAdjacencyPool;
	{
		auto data = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
		ASSERT_TRUE(data->is_open());
		auto io = std::make_shared<StorageIO>(data, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		auto metadata = RelocatableMetadataStore::open(io);
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const std::optional<IndexRootLocation> root =
				metadata->findIndexRootOptional(snapshot, RSS_GLOBAL_ADJACENCY_INDEX_ID);
		ASSERT_TRUE(root.has_value());
		const PageLocation rootLocation = metadata->findPageLocation(snapshot, root->root_page_id);
		originalAdjacencyPool = metadata->findCatalog(snapshot, rootLocation.segment_id);
		EXPECT_EQ(originalAdjacencyPool.owner_id, RSS_GLOBAL_ADJACENCY_INDEX_ID);
		snapshot.release();
		metadata.reset();
		io.reset();
		data->close();
	}

	// Keep the final-delete maintenance task queued until the old graph root is
	// pinned. Its pool may retire, but its physical slot must not release yet.
	backend.pauseMaintenance();
	std::unique_ptr<LogicalGraphReadSnapshot> beforeDelete = backend.openLogicalGraphReadSnapshot();
	backend.deleteLogicalEdges({{edges.front()}});
	const uint64_t completedBeforeReclaim = backend.maintenanceMetrics().loop.completed_tasks;
	backend.resumeMaintenance();
	ASSERT_TRUE(waitUntil([&] {
		const RelocatableMaintenanceMetrics metrics = backend.maintenanceMetrics();
		return metrics.global_adjacency_pools_retired >= 1U ||
			   metrics.loop.state == RelocatableMaintenanceLoopState::VMLS_FAULTED;
	}));
	if (const std::exception_ptr failure = backend.maintenanceFailure(); failure) {
		try {
			std::rethrow_exception(failure);
		} catch (const std::exception &error) {
			FAIL() << "global adjacency pool reclamation failed: " << error.what();
		}
	}
	EXPECT_EQ(backend.maintenanceMetrics().loop.reclaimed_physical_bytes, 0U);
	EXPECT_EQ(backend.maintenanceMetrics().global_adjacency_pools_released, 0U);
	EXPECT_EQ(beforeDelete->scanAdjacentEdges(nodes[0], LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 1U)
					  .edges.size(),
			  1U);

	beforeDelete.reset();
	ASSERT_TRUE(waitUntil([&] {
		const RelocatableMaintenanceLoopMetrics metrics = backend.maintenanceMetrics().loop;
		return metrics.completed_tasks > completedBeforeReclaim ||
			   metrics.state == RelocatableMaintenanceLoopState::VMLS_FAULTED;
	}));
	if (const std::exception_ptr failure = backend.maintenanceFailure(); failure) {
		try {
			std::rethrow_exception(failure);
		} catch (const std::exception &error) {
			FAIL() << "global adjacency pool release failed: " << error.what();
		}
	}
	EXPECT_GE(backend.maintenanceMetrics().loop.reclaimed_physical_bytes, RSS_SEGMENT_SIZE);
	EXPECT_GE(backend.maintenanceMetrics().global_adjacency_pools_released, 1U);
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();

	auto data = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(data->is_open());
	auto io = std::make_shared<StorageIO>(data, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_FALSE(metadata->findIndexRootOptional(snapshot, RSS_GLOBAL_ADJACENCY_INDEX_ID).has_value());
	EXPECT_FALSE(metadata->isPhysicalSlotAllocated(snapshot, originalAdjacencyPool.physical_slot));
	for (const CatalogEntry &pool: metadata->findCurrentIndexPagePools(snapshot)) {
		EXPECT_NE(pool.owner_id, RSS_GLOBAL_ADJACENCY_INDEX_ID);
	}
	snapshot.release();
	metadata.reset();
	io.reset();
	data->close();

	RelocatableStorageBackendOptions reopenedOptions = backendOptions();
	reopenedOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend reopened(std::move(reopenedOptions));
	reopened.open();
	const std::vector<uint64_t> retriedEdges =
			reopened.createLogicalEdges({{nodes[0], nodes[1], "POOL_RECLAIM_RETRY", {}}});
	ASSERT_EQ(retriedEdges.size(), 1U);
	std::unique_ptr<LogicalGraphReadSnapshot> snapshotAfterReopen = reopened.openLogicalGraphReadSnapshot();
	const LogicalGraphAdjacencyPage retriedPage = snapshotAfterReopen->scanAdjacentEdges(
			nodes[0], LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 1U);
	ASSERT_EQ(retriedPage.edges.size(), 1U);
	EXPECT_EQ(retriedPage.edges.front().edge_id, retriedEdges.front());
	snapshotAfterReopen.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();

	data = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(data->is_open());
	io = std::make_shared<StorageIO>(data, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	metadata = RelocatableMetadataStore::open(io);
	snapshot = metadata->openSnapshot();
	const std::optional<IndexRootLocation> replacementRoot =
			metadata->findIndexRootOptional(snapshot, RSS_GLOBAL_ADJACENCY_INDEX_ID);
	ASSERT_TRUE(replacementRoot.has_value());
	const PageLocation replacementLocation = metadata->findPageLocation(snapshot, replacementRoot->root_page_id);
	const CatalogEntry replacementAdjacencyPool = metadata->findCatalog(snapshot, replacementLocation.segment_id);
	EXPECT_EQ(replacementAdjacencyPool.owner_id, RSS_GLOBAL_ADJACENCY_INDEX_ID);
	EXPECT_EQ(replacementAdjacencyPool.physical_slot, originalAdjacencyPool.physical_slot);
	snapshot.release();
	metadata.reset();
	io.reset();
	data->close();
}

TEST_F(RelocatableStorageBackendTest, OrdinaryAdjacencyPoolMaintenanceShortCandidateWriteRetainsPoolUntilReopen) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableMaintenanceOptions maintenance;
	maintenance.loop.maximum_outstanding_tasks = 2U;
	maintenance.loop.minimum_cycle_interval = std::chrono::milliseconds(1);
	options.maintenance = maintenance;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> nodes =
			backend.createLogicalNodes({{{"PoolFaultSource"}, {}}, {{"PoolFaultTarget"}, {}}});
	ASSERT_EQ(nodes.size(), 2U);
	const std::vector<uint64_t> edges = backend.createLogicalEdges({{nodes[0], nodes[1], "POOL_FAULT", {}}});
	ASSERT_EQ(edges.size(), 1U);

	CatalogEntry adjacencyPool;
	{
		auto data = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
		ASSERT_TRUE(data->is_open());
		auto io = std::make_shared<StorageIO>(data, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		auto metadata = RelocatableMetadataStore::open(io);
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const std::optional<IndexRootLocation> root =
				metadata->findIndexRootOptional(snapshot, RSS_GLOBAL_ADJACENCY_INDEX_ID);
		ASSERT_TRUE(root.has_value());
		adjacencyPool =
				metadata->findCatalog(snapshot, metadata->findPageLocation(snapshot, root->root_page_id).segment_id);
		snapshot.release();
	}

	backend.pauseMaintenance();
	backend.deleteLogicalEdges({{edges.front()}});
	std::atomic<bool> injected{false};
	{
		StorageIO::ScopedAllThreadNativeWriteHookForTesting shortRetirementCandidate(
				[&injected](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (byteCount == RSS_PAGE_SIZE &&
						(offset != SuperblockFile::slotOffset(0U) && offset != SuperblockFile::slotOffset(1U)) &&
						!injected.exchange(true, std::memory_order_relaxed)) {
						return portable_pwrite(fd, bytes, 16U, offset);
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		backend.resumeMaintenance();
		ASSERT_TRUE(waitUntil([&] {
			return backend.maintenanceMetrics().loop.state == RelocatableMaintenanceLoopState::VMLS_FAULTED;
		}));
	}
	EXPECT_TRUE(injected.load(std::memory_order_relaxed));
	EXPECT_NE(backend.maintenanceFailure(), nullptr);
	backend.close();

	{
		auto data = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
		ASSERT_TRUE(data->is_open());
		auto io = std::make_shared<StorageIO>(data, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		auto metadata = RelocatableMetadataStore::open(io);
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		EXPECT_FALSE(metadata->findIndexRootOptional(snapshot, RSS_GLOBAL_ADJACENCY_INDEX_ID).has_value());
		EXPECT_EQ(metadata->findCatalog(snapshot, adjacencyPool.segment_id).state, SegmentState::SGS_SEALED);
		EXPECT_TRUE(metadata->isPhysicalSlotAllocated(snapshot, adjacencyPool.physical_slot));
		snapshot.release();
	}

	RelocatableStorageBackendOptions reopenOptions = backendOptions();
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	reopenOptions.maintenance = maintenance;
	RelocatableStorageBackend reopened(std::move(reopenOptions));
	reopened.open();
	ASSERT_TRUE(waitUntil([&] {
		const RelocatableMaintenanceMetrics metrics = reopened.maintenanceMetrics();
		return metrics.global_adjacency_pools_released >= 1U ||
			   metrics.loop.state == RelocatableMaintenanceLoopState::VMLS_FAULTED;
	}));
	if (const std::exception_ptr failure = reopened.maintenanceFailure(); failure) {
		try {
			std::rethrow_exception(failure);
		} catch (const std::exception &error) {
			FAIL() << "global adjacency pool recovery failed: " << error.what();
		}
	}
	EXPECT_GE(reopened.maintenanceMetrics().global_adjacency_pools_released, 1U);
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();

	auto data = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(data->is_open());
	auto io = std::make_shared<StorageIO>(data, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_FALSE(metadata->isPhysicalSlotAllocated(snapshot, adjacencyPool.physical_slot));
	snapshot.release();
}

TEST_F(RelocatableStorageBackendTest, OrdinaryAdjacencyPoolMaintenanceCandidateDiskFullRetainsPoolUntilReopen) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableMaintenanceOptions maintenance;
	maintenance.loop.maximum_outstanding_tasks = 2U;
	maintenance.loop.minimum_cycle_interval = std::chrono::milliseconds(1);
	options.maintenance = maintenance;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> nodes =
			backend.createLogicalNodes({{{"PoolDiskFullSource"}, {}}, {{"PoolDiskFullTarget"}, {}}});
	ASSERT_EQ(nodes.size(), 2U);
	const std::vector<uint64_t> edges = backend.createLogicalEdges({{nodes[0], nodes[1], "POOL_DISK_FULL", {}}});
	ASSERT_EQ(edges.size(), 1U);

	CatalogEntry adjacencyPool;
	{
		auto data = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
		ASSERT_TRUE(data->is_open());
		auto io = std::make_shared<StorageIO>(data, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		auto metadata = RelocatableMetadataStore::open(io);
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const std::optional<IndexRootLocation> root =
				metadata->findIndexRootOptional(snapshot, RSS_GLOBAL_ADJACENCY_INDEX_ID);
		ASSERT_TRUE(root.has_value());
		adjacencyPool =
				metadata->findCatalog(snapshot, metadata->findPageLocation(snapshot, root->root_page_id).segment_id);
		snapshot.release();
	}

	backend.pauseMaintenance();
	backend.deleteLogicalEdges({{edges.front()}});
	std::atomic<bool> injected{false};
	{
		StorageIO::ScopedAllThreadNativeWriteHookForTesting failRetirementCandidate(
				[&injected](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (byteCount == RSS_PAGE_SIZE &&
						(offset != SuperblockFile::slotOffset(0U) && offset != SuperblockFile::slotOffset(1U)) &&
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
		backend.resumeMaintenance();
		ASSERT_TRUE(waitUntil([&] {
			return backend.maintenanceMetrics().loop.state == RelocatableMaintenanceLoopState::VMLS_FAULTED;
		}));
	}
	EXPECT_TRUE(injected.load(std::memory_order_relaxed));
	EXPECT_NE(backend.maintenanceFailure(), nullptr);
	backend.close();

	{
		auto data = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
		ASSERT_TRUE(data->is_open());
		auto io = std::make_shared<StorageIO>(data, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		auto metadata = RelocatableMetadataStore::open(io);
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		EXPECT_FALSE(metadata->findIndexRootOptional(snapshot, RSS_GLOBAL_ADJACENCY_INDEX_ID).has_value());
		EXPECT_EQ(metadata->findCatalog(snapshot, adjacencyPool.segment_id).state, SegmentState::SGS_SEALED);
		EXPECT_TRUE(metadata->isPhysicalSlotAllocated(snapshot, adjacencyPool.physical_slot));
		snapshot.release();
	}

	RelocatableStorageBackendOptions reopenOptions = backendOptions();
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	reopenOptions.maintenance = maintenance;
	RelocatableStorageBackend reopened(std::move(reopenOptions));
	reopened.open();
	ASSERT_TRUE(waitUntil([&] {
		const RelocatableMaintenanceMetrics metrics = reopened.maintenanceMetrics();
		return metrics.global_adjacency_pools_released >= 1U ||
			   metrics.loop.state == RelocatableMaintenanceLoopState::VMLS_FAULTED;
	}));
	if (const std::exception_ptr failure = reopened.maintenanceFailure(); failure) {
		try {
			std::rethrow_exception(failure);
		} catch (const std::exception &error) {
			FAIL() << "global adjacency pool disk-full recovery failed: " << error.what();
		}
	}
	EXPECT_GE(reopened.maintenanceMetrics().global_adjacency_pools_released, 1U);
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();

	auto data = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(data->is_open());
	auto io = std::make_shared<StorageIO>(data, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_FALSE(metadata->isPhysicalSlotAllocated(snapshot, adjacencyPool.physical_slot));
	snapshot.release();
}

TEST_F(RelocatableStorageBackendTest, OrdinaryAdjacencyPoolMaintenanceFinalSyncFailureResumesReleaseAfterReopen) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableMaintenanceOptions maintenance;
	maintenance.loop.maximum_outstanding_tasks = 2U;
	maintenance.loop.minimum_cycle_interval = std::chrono::milliseconds(1);
	options.maintenance = maintenance;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> nodes =
			backend.createLogicalNodes({{{"PoolFinalSyncSource"}, {}}, {{"PoolFinalSyncTarget"}, {}}});
	ASSERT_EQ(nodes.size(), 2U);
	const std::vector<uint64_t> edges = backend.createLogicalEdges({{nodes[0], nodes[1], "POOL_FINAL_SYNC", {}}});
	ASSERT_EQ(edges.size(), 1U);

	CatalogEntry adjacencyPool;
	{
		auto data = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
		ASSERT_TRUE(data->is_open());
		auto io = std::make_shared<StorageIO>(data, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		auto metadata = RelocatableMetadataStore::open(io);
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const std::optional<IndexRootLocation> root =
				metadata->findIndexRootOptional(snapshot, RSS_GLOBAL_ADJACENCY_INDEX_ID);
		ASSERT_TRUE(root.has_value());
		adjacencyPool =
				metadata->findCatalog(snapshot, metadata->findPageLocation(snapshot, root->root_page_id).segment_id);
		snapshot.release();
	}

	backend.pauseMaintenance();
	backend.deleteLogicalEdges({{edges.front()}});
	PhysicalPageRef preRetirementCatalogRoot;
	{
		auto data = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
		ASSERT_TRUE(data->is_open());
		auto io = std::make_shared<StorageIO>(data, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		preRetirementCatalogRoot = SuperblockFile(io).recover().catalog_root;
	}
	std::atomic<bool> retirementSuperblockWritten{false};
	std::atomic<bool> failedFinalSync{false};
	{
		StorageIO::ScopedAllThreadNativeWriteHookForTesting observeRetirementPublication(
				[preRetirementCatalogRoot, &retirementSuperblockWritten](file_handle_t fd, const void *bytes,
																		 size_t byteCount, pwrite_off_t offset) {
					if (!retirementSuperblockWritten.load(std::memory_order_relaxed) && byteCount == RSS_PAGE_SIZE &&
						(offset == SuperblockFile::slotOffset(0U) || offset == SuperblockFile::slotOffset(1U))) {
						try {
							const Superblock candidate = SuperblockCodec::decode(
									std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount));
							if (candidate.catalog_root != preRetirementCatalogRoot) {
								retirementSuperblockWritten.store(true, std::memory_order_relaxed);
							}
						} catch (const FormatException &) {
							// Only a valid replacement Superblock arms the final-fence fault.
						}
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		StorageIO::ScopedAllThreadNativeSyncHookForTesting failRetirementFinalSync(
				[&retirementSuperblockWritten, &failedFinalSync](file_handle_t fd) {
					if (retirementSuperblockWritten.load(std::memory_order_relaxed) &&
						!failedFinalSync.exchange(true, std::memory_order_relaxed)) {
#ifdef _WIN32
						SetLastError(ERROR_WRITE_FAULT);
#else
						errno = EIO;
#endif
						return -1;
					}
					return portable_fsync(fd);
				});
		backend.resumeMaintenance();
		ASSERT_TRUE(waitUntil([&] {
			return backend.maintenanceMetrics().loop.state == RelocatableMaintenanceLoopState::VMLS_FAULTED;
		}));
	}
	EXPECT_TRUE(retirementSuperblockWritten.load(std::memory_order_relaxed));
	EXPECT_TRUE(failedFinalSync.load(std::memory_order_relaxed));
	EXPECT_NE(backend.maintenanceFailure(), nullptr);
	backend.close();

	{
		auto data = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
		ASSERT_TRUE(data->is_open());
		auto io = std::make_shared<StorageIO>(data, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		auto metadata = RelocatableMetadataStore::open(io);
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		EXPECT_FALSE(metadata->findIndexRootOptional(snapshot, RSS_GLOBAL_ADJACENCY_INDEX_ID).has_value());
		EXPECT_EQ(metadata->findCatalog(snapshot, adjacencyPool.segment_id).state, SegmentState::SGS_RETIRED);
		EXPECT_TRUE(metadata->isPhysicalSlotAllocated(snapshot, adjacencyPool.physical_slot));
		snapshot.release();
	}

	RelocatableStorageBackendOptions reopenOptions = backendOptions();
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	reopenOptions.maintenance = maintenance;
	RelocatableStorageBackend reopened(std::move(reopenOptions));
	reopened.open();
	ASSERT_TRUE(waitUntil([&] {
		const RelocatableMaintenanceMetrics metrics = reopened.maintenanceMetrics();
		return metrics.global_adjacency_pools_released >= 1U ||
			   metrics.loop.state == RelocatableMaintenanceLoopState::VMLS_FAULTED;
	}));
	if (const std::exception_ptr failure = reopened.maintenanceFailure(); failure) {
		try {
			std::rethrow_exception(failure);
		} catch (const std::exception &error) {
			FAIL() << "global adjacency pool final-sync recovery failed: " << error.what();
		}
	}
	EXPECT_EQ(reopened.maintenanceMetrics().global_adjacency_pools_retired, 0U);
	EXPECT_GE(reopened.maintenanceMetrics().global_adjacency_pools_released, 1U);
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();

	auto data = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(data->is_open());
	auto io = std::make_shared<StorageIO>(data, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_FALSE(metadata->isPhysicalSlotAllocated(snapshot, adjacencyPool.physical_slot));
	snapshot.release();
}

TEST_F(RelocatableStorageBackendTest, CypherCreateSetRemoveFoldsCreatedEdgeStateIntoOneNativePublication) {
	materializeNativeDatabase();

	zyx::Database database(data_path_.string());
	database.open();
	zyx::Result create = database.execute("CREATE (a:CreatedEdgeFold {seed: 3})-"
										  "[r:CREATED_EDGE_FOLD {temporary: true}]->(b:CreatedEdgeFold) "
										  "SET r.weight = a.seed * 2 REMOVE r.temporary "
										  "RETURN r.weight AS weight, r.temporary AS temporary");
	ASSERT_TRUE(create.isSuccess()) << create.getError();
	ASSERT_TRUE(create.hasNext());
	create.next();
	EXPECT_EQ(std::get<int64_t>(create.get("weight")), 6);
	EXPECT_TRUE(std::holds_alternative<std::monostate>(create.get("temporary")));
	EXPECT_FALSE(create.hasNext());
	database.close();

	zyx::Database reopened(data_path_.string());
	reopened.open();
	zyx::Result visible = reopened.execute("MATCH (:CreatedEdgeFold)-[r:CREATED_EDGE_FOLD]->(:CreatedEdgeFold) "
										   "RETURN r.weight AS weight, r.temporary AS temporary");
	ASSERT_TRUE(visible.isSuccess()) << visible.getError();
	ASSERT_TRUE(visible.hasNext());
	visible.next();
	EXPECT_EQ(std::get<int64_t>(visible.get("weight")), 6);
	EXPECT_TRUE(std::holds_alternative<std::monostate>(visible.get("temporary")));
	EXPECT_FALSE(visible.hasNext());
	EXPECT_NO_THROW(reopened.close());
}

TEST_F(RelocatableStorageBackendTest, CypherSetNullRemovesACreatedEdgePropertyDurably) {
	materializeNativeDatabase();

	zyx::Database database(data_path_.string());
	database.open();
	zyx::Result create =
			database.execute("CREATE (:CreatedEdgeNull)-[r:CREATED_EDGE_NULL {temporary: true}]->(:CreatedEdgeNull) "
							 "SET r.temporary = null RETURN r.temporary AS temporary");
	ASSERT_TRUE(create.isSuccess()) << create.getError();
	ASSERT_TRUE(create.hasNext());
	create.next();
	EXPECT_TRUE(std::holds_alternative<std::monostate>(create.get("temporary")));
	EXPECT_FALSE(create.hasNext());
	database.close();

	zyx::Database reopened(data_path_.string());
	reopened.open();
	zyx::Result visible = reopened.execute(
			"MATCH (:CreatedEdgeNull)-[r:CREATED_EDGE_NULL]->(:CreatedEdgeNull) RETURN r.temporary AS temporary");
	ASSERT_TRUE(visible.isSuccess()) << visible.getError();
	ASSERT_TRUE(visible.hasNext());
	visible.next();
	EXPECT_TRUE(std::holds_alternative<std::monostate>(visible.get("temporary")));
	EXPECT_FALSE(visible.hasNext());
	EXPECT_NO_THROW(reopened.close());
}

TEST_F(RelocatableStorageBackendTest, CypherMergePostMutationsPublishCreatedAndMatchedLabelsDurably) {
	materializeNativeDatabase();

	zyx::Database database(data_path_.string());
	database.open();
	zyx::Result created = database.execute("MERGE (n:MergeLabelPerson {name: 'Ada'}) "
										   "SET n:MergeCreatedMarker, n += {legacy: 1} RETURN n");
	ASSERT_TRUE(created.isSuccess()) << created.getError();
	ASSERT_TRUE(created.hasNext());
	created.next();
	const auto createdNode = std::get<std::shared_ptr<zyx::Node>>(created.get("n"));
	ASSERT_NE(createdNode, nullptr);
	EXPECT_NE(std::ranges::find(createdNode->labels, "MergeLabelPerson"), createdNode->labels.end());
	EXPECT_NE(std::ranges::find(createdNode->labels, "MergeCreatedMarker"), createdNode->labels.end());
	EXPECT_EQ(std::get<int64_t>(createdNode->properties.at("legacy")), 1);
	EXPECT_FALSE(created.hasNext());

	zyx::Result matched = database.execute("MERGE (n:MergeLabelPerson {name: 'Ada'}) "
										   "SET n:MergeActive, n += {legacy: null, score: 4} "
										   "REMOVE n:MergeCreatedMarker RETURN n");
	ASSERT_TRUE(matched.isSuccess()) << matched.getError();
	ASSERT_TRUE(matched.hasNext());
	matched.next();
	const auto matchedNode = std::get<std::shared_ptr<zyx::Node>>(matched.get("n"));
	ASSERT_NE(matchedNode, nullptr);
	EXPECT_NE(std::ranges::find(matchedNode->labels, "MergeLabelPerson"), matchedNode->labels.end());
	EXPECT_NE(std::ranges::find(matchedNode->labels, "MergeActive"), matchedNode->labels.end());
	EXPECT_EQ(std::ranges::find(matchedNode->labels, "MergeCreatedMarker"), matchedNode->labels.end());
	EXPECT_FALSE(matchedNode->properties.contains("legacy"));
	EXPECT_EQ(std::get<int64_t>(matchedNode->properties.at("score")), 4);
	EXPECT_FALSE(matched.hasNext());
	database.close();

	zyx::Database reopened(data_path_.string());
	reopened.open();
	zyx::Result durable = reopened.execute("MATCH (n:MergeLabelPerson:MergeActive {name: 'Ada'}) "
										   "RETURN n.legacy AS legacy, n.score AS score");
	ASSERT_TRUE(durable.isSuccess()) << durable.getError();
	ASSERT_TRUE(durable.hasNext());
	durable.next();
	EXPECT_TRUE(std::holds_alternative<std::monostate>(durable.get("legacy")));
	EXPECT_EQ(std::get<int64_t>(durable.get("score")), 4);
	EXPECT_FALSE(durable.hasNext());
	const zyx::Result retired = reopened.execute("MATCH (n:MergeCreatedMarker) RETURN n");
	ASSERT_TRUE(retired.isSuccess()) << retired.getError();
	EXPECT_FALSE(retired.hasNext());
	EXPECT_NO_THROW(reopened.close());
}

TEST_F(RelocatableStorageBackendTest, CypherMergeResultFailureDoesNotPublishMatchedLabelMutation) {
	materializeNativeDatabase();

	zyx::Database database(data_path_.string());
	database.open();
	const zyx::Result seed = database.execute("CREATE (:MergeFailureSeed {name: 'Ada'})");
	ASSERT_TRUE(seed.isSuccess()) << seed.getError();

	// MERGE must preflight its result expressions before publishing the staged
	// label change. A failed RETURN cannot leave a partially visible mutation.
	const zyx::Result failed = database.execute("MERGE (n:MergeFailureSeed {name: 'Ada'}) SET n:MergeFailurePublished "
												"RETURN 1 / 0 AS invalidResult");
	EXPECT_FALSE(failed.isSuccess());
	EXPECT_FALSE(failed.getError().empty());

	const zyx::Result original = database.execute("MATCH (n:MergeFailureSeed {name: 'Ada'}) RETURN n");
	ASSERT_TRUE(original.isSuccess()) << original.getError();
	ASSERT_TRUE(original.hasNext());
	original.next();
	const auto node = std::get<std::shared_ptr<zyx::Node>>(original.get("n"));
	ASSERT_NE(node, nullptr);
	EXPECT_NE(std::ranges::find(node->labels, "MergeFailureSeed"), node->labels.end());
	EXPECT_EQ(std::ranges::find(node->labels, "MergeFailurePublished"), node->labels.end());
	EXPECT_FALSE(original.hasNext());
	database.close();
}

TEST_F(RelocatableStorageBackendTest, CypherDeleteUsesTheReadSnapshotFenceAndOneMutationBatch) {
	materializeNativeDatabase();

	zyx::Database database(data_path_.string());
	database.open();
	const int64_t source = database.createNode("DeleteSource", {{"name", std::string("Ada")}});
	const int64_t target = database.createNode("DeleteTarget", {{"name", std::string("Bob")}});
	const int64_t edge = database.createEdge(source, target, "DELETE_LINK", {{"weight", int64_t(1)}});
	ASSERT_GT(edge, 0);

	zyx::Result deleteSourceAndEdge = database.execute(
			"MATCH (a:DeleteSource)-[r:DELETE_LINK]->(b) DELETE a, r RETURN a.name AS name, r.weight AS weight");
	ASSERT_TRUE(deleteSourceAndEdge.isSuccess()) << deleteSourceAndEdge.getError();
	ASSERT_TRUE(deleteSourceAndEdge.hasNext());
	deleteSourceAndEdge.next();
	EXPECT_EQ(std::get<std::string>(deleteSourceAndEdge.get("name")), "Ada");
	EXPECT_EQ(std::get<int64_t>(deleteSourceAndEdge.get("weight")), 1);
	zyx::Result sourceCheck = database.execute("MATCH (n:DeleteSource) RETURN n");
	ASSERT_TRUE(sourceCheck.isSuccess()) << sourceCheck.getError();
	EXPECT_FALSE(sourceCheck.hasNext());
	zyx::Result targetCheck = database.execute("MATCH (n:DeleteTarget) RETURN n");
	ASSERT_TRUE(targetCheck.isSuccess()) << targetCheck.getError();
	ASSERT_TRUE(targetCheck.hasNext());
	targetCheck.next();
	EXPECT_EQ(std::get<std::shared_ptr<zyx::Node>>(targetCheck.get("n"))->id, target);

	const zyx::Result deleteTarget = database.execute("MATCH (n:DeleteTarget) DELETE n");
	ASSERT_TRUE(deleteTarget.isSuccess()) << deleteTarget.getError();
	targetCheck = database.execute("MATCH (n:DeleteTarget) RETURN n");
	ASSERT_TRUE(targetCheck.isSuccess()) << targetCheck.getError();
	EXPECT_FALSE(targetCheck.hasNext());

	const int64_t detachSource = database.createNode("DetachSource", {});
	const int64_t detachTarget = database.createNode("DetachTarget", {});
	ASSERT_GT(database.createEdge(detachSource, detachTarget, "DETACH_LINK", {}), 0);
	const zyx::Result detachDelete = database.execute("MATCH (n:DetachSource) DETACH DELETE n");
	ASSERT_TRUE(detachDelete.isSuccess()) << detachDelete.getError();
	zyx::Result detachSourceCheck = database.execute("MATCH (n:DetachSource) RETURN n");
	ASSERT_TRUE(detachSourceCheck.isSuccess()) << detachSourceCheck.getError();
	EXPECT_FALSE(detachSourceCheck.hasNext());
	zyx::Result detachTargetCheck = database.execute("MATCH (n:DetachTarget) RETURN n");
	ASSERT_TRUE(detachTargetCheck.isSuccess()) << detachTargetCheck.getError();
	ASSERT_TRUE(detachTargetCheck.hasNext());
	database.close();
}

TEST_F(RelocatableStorageBackendTest, CypherSetUsesTheReadSnapshotFenceAndCompletePropertyReplacement) {
	materializeNativeDatabase();

	zyx::Database database(data_path_.string());
	database.open();
	const int64_t source = database.createNode("SetSource", {{"score", int64_t(1)}});
	const int64_t target = database.createNode("SetTarget", {});
	ASSERT_GT(database.createEdge(source, target, "SET_LINK", {{"weight", int64_t(1)}}), 0);

	zyx::Result setNode = database.execute("MATCH (n:SetSource) SET n.score = $score RETURN n.score AS score",
										   {{"score", int64_t(7)}});
	ASSERT_TRUE(setNode.isSuccess()) << setNode.getError();
	ASSERT_TRUE(setNode.hasNext());
	setNode.next();
	EXPECT_EQ(std::get<int64_t>(setNode.get("score")), 7);
	const zyx::Result copyNodeProperty = database.execute("MATCH (n:SetSource) SET n.copied = n.score");
	ASSERT_TRUE(copyNodeProperty.isSuccess()) << copyNodeProperty.getError();
	const zyx::Result mergeNodeProperties = database.execute("MATCH (n:SetSource) SET n += {merged: 9, copied: null}");
	ASSERT_TRUE(mergeNodeProperties.isSuccess()) << mergeNodeProperties.getError();
	const zyx::Result setEdge = database.execute("MATCH (a:SetSource)-[r:SET_LINK]->(b) SET r.weight = 2");
	ASSERT_TRUE(setEdge.isSuccess()) << setEdge.getError();
	const zyx::Result setLabel = database.execute("MATCH (n:SetSource) SET n:SetApplied");
	ASSERT_TRUE(setLabel.isSuccess()) << setLabel.getError();

	zyx::Result nodeCheck = database.execute(
			"MATCH (n:SetSource:SetApplied) RETURN n.score AS score, n.copied AS copied, n.merged AS merged");
	ASSERT_TRUE(nodeCheck.isSuccess()) << nodeCheck.getError();
	ASSERT_TRUE(nodeCheck.hasNext());
	nodeCheck.next();
	EXPECT_EQ(std::get<int64_t>(nodeCheck.get("score")), 7);
	EXPECT_TRUE(std::holds_alternative<std::monostate>(nodeCheck.get("copied")));
	EXPECT_EQ(std::get<int64_t>(nodeCheck.get("merged")), 9);
	zyx::Result edgeCheck = database.execute("MATCH (a:SetSource)-[r:SET_LINK]->(b) RETURN r.weight AS weight");
	ASSERT_TRUE(edgeCheck.isSuccess()) << edgeCheck.getError();
	ASSERT_TRUE(edgeCheck.hasNext());
	edgeCheck.next();
	EXPECT_EQ(std::get<int64_t>(edgeCheck.get("weight")), 2);
	database.close();
}

TEST_F(RelocatableStorageBackendTest, CypherMultiRowPropertyWritesCoalesceSharedNativePropertyPages) {
	materializeNativeDatabase();

	zyx::Database database(data_path_.string());
	database.open();
	ASSERT_GT(database.createNode("MultiRowProperty", {{"score", int64_t(1)}, {"drop", int64_t(10)}}), 0);
	ASSERT_GT(database.createNode("MultiRowProperty", {{"score", int64_t(2)}, {"drop", int64_t(20)}}), 0);

	zyx::Result set = database.execute("MATCH (n:MultiRowProperty) SET n.score = $score RETURN n.score AS score",
									   {{"score", int64_t(9)}});
	ASSERT_TRUE(set.isSuccess()) << set.getError();
	size_t setRows = 0U;
	while (set.hasNext()) {
		set.next();
		EXPECT_EQ(std::get<int64_t>(set.get("score")), 9);
		++setRows;
	}
	EXPECT_EQ(setRows, 2U);

	zyx::Result remove =
			database.execute("MATCH (n:MultiRowProperty) REMOVE n.drop RETURN n.score AS score, n.drop AS drop");
	ASSERT_TRUE(remove.isSuccess()) << remove.getError();
	size_t removeRows = 0U;
	while (remove.hasNext()) {
		remove.next();
		EXPECT_EQ(std::get<int64_t>(remove.get("score")), 9);
		EXPECT_TRUE(std::holds_alternative<std::monostate>(remove.get("drop")));
		++removeRows;
	}
	EXPECT_EQ(removeRows, 2U);
	database.close();
}

TEST_F(RelocatableStorageBackendTest, CypherRemoveUsesTheReadSnapshotFenceAndCompletePropertyReplacement) {
	materializeNativeDatabase();

	zyx::Database database(data_path_.string());
	database.open();
	const int64_t source = database.createNode(std::vector<std::string>{"RemoveSource", "KeepLabel"},
											   {{"keep", int64_t(1)}, {"drop", int64_t(2)}});
	const int64_t target = database.createNode("RemoveTarget", {});
	ASSERT_GT(database.createEdge(source, target, "REMOVE_LINK", {{"keep", int64_t(3)}, {"drop", int64_t(4)}}), 0);

	zyx::Result removeNodeProperty =
			database.execute("MATCH (n:RemoveSource) REMOVE n.drop RETURN n.keep AS keep, n.drop AS drop");
	ASSERT_TRUE(removeNodeProperty.isSuccess()) << removeNodeProperty.getError();
	ASSERT_TRUE(removeNodeProperty.hasNext());
	removeNodeProperty.next();
	EXPECT_EQ(std::get<int64_t>(removeNodeProperty.get("keep")), 1);
	EXPECT_TRUE(std::holds_alternative<std::monostate>(removeNodeProperty.get("drop")));
	const zyx::Result removeEdgeProperty =
			database.execute("MATCH (a:RemoveSource)-[r:REMOVE_LINK]->(b) REMOVE r.drop");
	ASSERT_TRUE(removeEdgeProperty.isSuccess()) << removeEdgeProperty.getError();
	const zyx::Result removeNodeLabel = database.execute("MATCH (n:RemoveSource) REMOVE n:RemoveSource");
	ASSERT_TRUE(removeNodeLabel.isSuccess()) << removeNodeLabel.getError();

	zyx::Result nodeCheck = database.execute("MATCH (n:KeepLabel) RETURN n.keep AS keep, n.drop AS drop");
	ASSERT_TRUE(nodeCheck.isSuccess()) << nodeCheck.getError();
	ASSERT_TRUE(nodeCheck.hasNext());
	nodeCheck.next();
	EXPECT_EQ(std::get<int64_t>(nodeCheck.get("keep")), 1);
	EXPECT_TRUE(std::holds_alternative<std::monostate>(nodeCheck.get("drop")));
	zyx::Result edgeCheck =
			database.execute("MATCH (a:KeepLabel)-[r:REMOVE_LINK]->(b) RETURN r.keep AS keep, r.drop AS drop");
	ASSERT_TRUE(edgeCheck.isSuccess()) << edgeCheck.getError();
	ASSERT_TRUE(edgeCheck.hasNext());
	edgeCheck.next();
	EXPECT_EQ(std::get<int64_t>(edgeCheck.get("keep")), 3);
	EXPECT_TRUE(std::holds_alternative<std::monostate>(edgeCheck.get("drop")));
	zyx::Result removedLabelCheck = database.execute("MATCH (n:RemoveSource) RETURN n");
	ASSERT_TRUE(removedLabelCheck.isSuccess()) << removedLabelCheck.getError();
	EXPECT_FALSE(removedLabelCheck.hasNext());
	database.close();
}

TEST_F(RelocatableStorageBackendTest, CypherMatchCreatePublishesAllRowsInOneLogicalBatch) {
	materializeNativeDatabase();

	zyx::Database database(data_path_.string());
	database.open();
	ASSERT_GT(database.createNode("MatchCreateSource", {{"rank", int64_t(3)}}), 0);
	ASSERT_GT(database.createNode("MatchCreateSource", {{"rank", int64_t(5)}}), 0);

	zyx::Result create = database.execute(
			"MATCH (source:MatchCreateSource) "
			"CREATE (source)-[edge:MATCH_CREATED {rank: source.rank}]->(copy:MatchCreateTarget {copied: source.rank}) "
			"RETURN source.rank AS sourceRank, edge.rank AS edgeRank, copy.copied AS copiedRank");
	ASSERT_TRUE(create.isSuccess()) << create.getError();
	std::vector<int64_t> returned;
	while (create.hasNext()) {
		create.next();
		const int64_t sourceRank = std::get<int64_t>(create.get("sourceRank"));
		EXPECT_EQ(std::get<int64_t>(create.get("edgeRank")), sourceRank);
		EXPECT_EQ(std::get<int64_t>(create.get("copiedRank")), sourceRank);
		returned.push_back(sourceRank);
	}
	std::ranges::sort(returned);
	EXPECT_EQ(returned, (std::vector<int64_t>{3, 5}));

	zyx::Result check =
			database.execute("MATCH (source:MatchCreateSource)-[edge:MATCH_CREATED]->(copy:MatchCreateTarget) "
							 "RETURN source.rank AS sourceRank, edge.rank AS edgeRank, copy.copied AS copiedRank");
	ASSERT_TRUE(check.isSuccess()) << check.getError();
	std::vector<int64_t> observed;
	while (check.hasNext()) {
		check.next();
		const auto &sourceRankValue = check.get("sourceRank");
		const auto &edgeRankValue = check.get("edgeRank");
		const auto &copiedRankValue = check.get("copiedRank");
		ASSERT_TRUE(std::holds_alternative<int64_t>(sourceRankValue));
		ASSERT_TRUE(std::holds_alternative<int64_t>(edgeRankValue));
		ASSERT_TRUE(std::holds_alternative<int64_t>(copiedRankValue));
		const int64_t sourceRank = std::get<int64_t>(sourceRankValue);
		observed.push_back(sourceRank);
		EXPECT_EQ(std::get<int64_t>(edgeRankValue), sourceRank);
		EXPECT_EQ(std::get<int64_t>(copiedRankValue), sourceRank);
	}
	std::ranges::sort(observed);
	EXPECT_EQ(observed, (std::vector<int64_t>{3, 5}));
	database.close();
}

TEST_F(RelocatableStorageBackendTest, DriverAbiDirectCreatesUseTheLogicalWriteAdapter) {
	materializeNativeDatabase();

	zyx_driver_db_t *database = nullptr;
	zyx_driver_error_t *error = nullptr;
	ASSERT_EQ(zyx_driver_db_open(data_path_.string().c_str(), &database, &error), ZYX_DRIVER_OK);
	ASSERT_NE(database, nullptr);
	ASSERT_EQ(error, nullptr);

	zyx_driver_params_t *properties = nullptr;
	ASSERT_EQ(zyx_driver_params_create(&properties, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_params_set_int64(properties, "score", 7, &error), ZYX_DRIVER_OK);
	int64_t source = 0;
	ASSERT_EQ(zyx_driver_db_create_node(database, "DriverOnline", properties, &source, &error), ZYX_DRIVER_OK);
	ASSERT_GT(source, 0);
	zyx_driver_params_free(properties, &error);
	ASSERT_EQ(error, nullptr);
	properties = nullptr;

	int64_t target = 0;
	ASSERT_EQ(zyx_driver_db_create_node(database, "DriverOnline", nullptr, &target, &error), ZYX_DRIVER_OK);
	int64_t edge = 0;
	ASSERT_EQ(zyx_driver_db_create_edge(database, source, target, "DRIVER_LINK", nullptr, &edge, &error),
			  ZYX_DRIVER_OK);
	ASSERT_GT(edge, 0);

	zyx_driver_result_t *result = nullptr;
	ASSERT_EQ(zyx_driver_db_execute(database, "MATCH (n:DriverOnline) RETURN n", nullptr, &result, &error),
			  ZYX_DRIVER_OK);
	ASSERT_NE(result, nullptr);
	ASSERT_EQ(zyx_driver_result_next(result, &error), ZYX_DRIVER_ROW);
	int64_t returned = 0;
	EXPECT_EQ(zyx_driver_result_get_node_id(result, 0U, &returned, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(returned, source);
	zyx_driver_result_free(result);
	zyx_driver_db_close(database, &error);
	EXPECT_EQ(error, nullptr);
}

TEST_F(RelocatableStorageBackendTest, ReadOnlyNativeLogicalWriteSurfaceRejectsEveryNonEmptyMutation) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions writerOptions = backendOptions();
	writerOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend writer(std::move(writerOptions));
	writer.open();
	const std::vector<uint64_t> stableNodes = writer.createLogicalNodes({{{"Stable"}, {}}});
	ASSERT_EQ(stableNodes.size(), 1U);
	writer.close();

	RelocatableStorageBackendOptions readOnlyOptions = backendOptions();
	readOnlyOptions.open_mode = OpenMode::OPEN_READ_ONLY_EXISTING_FILE;
	RelocatableStorageBackend readOnly(std::move(readOnlyOptions));
	readOnly.open();
	RelocatableReadSnapshot snapshot = readOnly.openSnapshot();
	EXPECT_TRUE(readOnly.containsLogicalNode(snapshot, stableNodes.front()));
	EXPECT_NO_THROW(readOnly.beginLogicalGraphTransaction(true)->rollback());
	EXPECT_THROW(static_cast<void>(readOnly.beginLogicalGraphTransaction(false)), std::logic_error);

	const LogicalGraphNodeCreateRequest nodeRequest{{"ReadOnly"}, {}};
	const LogicalGraphEdgeCreateRequest edgeRequest{stableNodes.front(), stableNodes.front(), "READ_ONLY", {}};
	const LogicalGraphPropertyReplaceRequest replacement{stableNodes.front(),
														 {{"value", graph::PropertyValue(int64_t{1})}}};
	const LogicalGraphEdgeDeleteRequest edgeDelete{1U};
	const LogicalGraphNodeDeleteRequest nodeDelete{stableNodes.front(), false, 0U};
	LogicalGraphMutationBatch batch;
	batch.node_creates.push_back(nodeRequest);

	EXPECT_THROW(static_cast<void>(readOnly.reserveLogicalGraphCreationIds(1U, 0U)), std::logic_error);
	EXPECT_THROW(static_cast<void>(readOnly.createLogicalNodes({nodeRequest})), std::logic_error);
	EXPECT_THROW(static_cast<void>(readOnly.createLogicalEdges({edgeRequest})), std::logic_error);
	EXPECT_THROW(readOnly.replaceLogicalNodeProperties({replacement}), std::logic_error);
	EXPECT_THROW(readOnly.replaceLogicalEdgeProperties({replacement}), std::logic_error);
	EXPECT_THROW(readOnly.deleteLogicalEdges({edgeDelete}), std::logic_error);
	EXPECT_THROW(readOnly.deleteLogicalNodes({nodeDelete}), std::logic_error);
	EXPECT_THROW(static_cast<void>(readOnly.executeLogicalGraphMutationBatch(batch)), std::logic_error);

	// Empty requests do not need the write executor and are explicit no-ops.
	EXPECT_TRUE(readOnly.createLogicalNodes({}).empty());
	EXPECT_TRUE(readOnly.createLogicalEdges({}).empty());
	EXPECT_NO_THROW(readOnly.replaceLogicalNodeProperties({}));
	EXPECT_NO_THROW(readOnly.replaceLogicalEdgeProperties({}));
	EXPECT_NO_THROW(readOnly.deleteLogicalEdges({}));
	EXPECT_NO_THROW(readOnly.deleteLogicalNodes({}));
	EXPECT_NO_THROW(static_cast<void>(readOnly.executeLogicalGraphMutationBatch({})));
	snapshot.release();
	readOnly.close();

	RelocatableStorageBackendOptions reopenOptions = backendOptions();
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend reopened(std::move(reopenOptions));
	reopened.open();
	RelocatableReadSnapshot reopenedSnapshot = reopened.openSnapshot();
	EXPECT_TRUE(reopened.containsLogicalNode(reopenedSnapshot, stableNodes.front()));
	reopenedSnapshot.release();
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest, ReadOnlyNativeAdministrativeAndMaintenanceWritesFailClosed) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions writerOptions = backendOptions();
	writerOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend writer(std::move(writerOptions));
	writer.open();
	static_cast<void>(writer.createLogicalNodes(
			{{{"ReadOnly"}, {{"value", graph::PropertyValue(int64_t{1})}}}}));
	static_cast<void>(writer.createLogicalNodeIndex({"read_only_scalar", "ReadOnly", {"value"}}));
	writer.close();

	RelocatableStorageBackendOptions readOnlyOptions = backendOptions();
	readOnlyOptions.open_mode = OpenMode::OPEN_READ_ONLY_EXISTING_FILE;
	RelocatableStorageBackend readOnly(std::move(readOnlyOptions));
	readOnly.open();

	const LogicalGraphNodeIndexCreateRequest scalarIndex{"read_only_scalar", "ReadOnly", {"value"}};
	const std::array<LogicalGraphNodeIndexCreateRequest, 1U> scalarIndexes{scalarIndex};
	const LogicalGraphNodeVectorIndexCreateRequest vectorIndex{"read_only_vector", "ReadOnly", "embedding", 2U,
																							 "l2"};
	const LogicalGraphConstraintCreateRequest constraint{"read_only_unique", LogicalGraphConstraintEntityKind::LGCEK_NODE,
																				 LogicalGraphConstraintKind::LGCK_UNIQUE, "ReadOnly", {"value"},
																				 LogicalGraphConstraintPropertyType::LGCPT_NONE};

	EXPECT_THROW(static_cast<void>(readOnly.reserveNativeBulkEdgeSessionIds(1U)), std::logic_error);
	EXPECT_GT(readOnly.allocateNativeBulkScratchOwner(), 0U);
	EXPECT_THROW(static_cast<void>(readOnly.createNativeBulkRunStore()), std::logic_error);
	EXPECT_GT(readOnly.nextNativeBulkScratchCheckpointLsn().value(), 0U);
	EXPECT_THROW(readOnly.refreshNativeMetadataAfterExternalPublication(), std::logic_error);
	EXPECT_THROW(static_cast<void>(readOnly.prepareLogicalVectorTraining({})), std::runtime_error);
	EXPECT_THROW(readOnly.setConfiguration("storage.compaction.enabled", graph::PropertyValue(false)), std::runtime_error);
	EXPECT_THROW(static_cast<void>(readOnly.createLogicalNodeIndex(scalarIndex)), std::runtime_error);
	EXPECT_THROW(static_cast<void>(readOnly.createLogicalNodeIndexes(scalarIndexes)), std::runtime_error);
	EXPECT_THROW(static_cast<void>(readOnly.createLogicalNodeVectorIndex(vectorIndex)), std::runtime_error);
	EXPECT_THROW(static_cast<void>(readOnly.dropLogicalIndexByName("read_only_scalar")), std::runtime_error);
	EXPECT_THROW(static_cast<void>(readOnly.dropLogicalNodeIndexBySchema("ReadOnly", "value")), std::runtime_error);
	EXPECT_THROW(static_cast<void>(readOnly.createLogicalConstraint(constraint)), std::runtime_error);
	EXPECT_THROW(static_cast<void>(readOnly.dropLogicalConstraintByName("read_only_unique")), std::runtime_error);
	EXPECT_THROW(static_cast<void>(readOnly.reclaimObsoleteScalarIndexTrees({})), std::runtime_error);
	EXPECT_THROW(static_cast<void>(readOnly.reclaimObsoleteVectorIndexState({})), std::runtime_error);
	EXPECT_THROW(static_cast<void>(readOnly.reclaimUnreachableLargeValues({})), std::runtime_error);

	RelocatableReadSnapshot snapshot = readOnly.openSnapshot();
	const std::vector<LogicalGraphIndexDefinition> indexes = readOnly.listLogicalIndexes(8U);
	ASSERT_EQ(indexes.size(), 1U);
	EXPECT_EQ(indexes.front().name, "read_only_scalar");
	EXPECT_TRUE(readOnly.listLogicalConstraints(8U).empty());
	snapshot.release();
	readOnly.close();
}

TEST_F(RelocatableStorageBackendTest, NativeBulkIdentityReservationsValidateBoundsBeforeLeasingIds) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();

	const uint64_t maximum = static_cast<uint64_t>(backend.maximumNativeBulkEdgeTransactionRecords());
	ASSERT_GT(maximum, 0U);
	EXPECT_THROW(static_cast<void>(backend.reserveLogicalGraphCreationIds(0U, 0U)), std::logic_error);
	EXPECT_THROW(static_cast<void>(backend.reserveNativeBulkEdgeSessionIds(0U)), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(backend.reserveNativeBulkEdgeSessionIds(maximum + 1U)), std::invalid_argument);

	const NativeGraphLogicalIdReservation reservation = backend.reserveNativeBulkEdgeSessionIds(1U);
	ASSERT_TRUE(reservation.node_ranges.empty());
	ASSERT_EQ(reservation.edge_ranges.size(), 1U);
	EXPECT_EQ(reservation.edge_ranges.front().count, 1U);
	EXPECT_GT(reservation.edge_ranges.front().first_id, 0U);
	backend.abandonLogicalGraphCreationIds(reservation);

	// Reservation rejection and abandonment only affect the durable ID lease;
	// they must not disturb the opened backend or the next scratch checkpoint.
	EXPECT_GT(backend.allocateNativeBulkScratchOwner(), 0U);
	EXPECT_GT(backend.nextNativeBulkScratchCheckpointLsn().value(), 0U);
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, NativeSchemaWriteSurfaceRejectsEachMalformedBoundedRequest) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();

	// Exercise each independently validated field.  These are admission checks,
	// so none of the malformed requests may allocate a catalog token or publish a
	// candidate generation.
	EXPECT_THROW(static_cast<void>(backend.createLogicalNodeIndexes({})), std::invalid_argument);
	const auto expectInvalidIndex = [&](LogicalGraphNodeIndexCreateRequest request) {
		const std::array<LogicalGraphNodeIndexCreateRequest, 1U> requests{std::move(request)};
		EXPECT_THROW(static_cast<void>(backend.createLogicalNodeIndexes(requests)), std::invalid_argument);
	};
	const std::string oversizedName(RSS_INDEX_DEFINITION_MAX_NAME_BYTES + 1U, 'x');
	const std::string oversizedProperty(RSS_INDEX_DEFINITION_MAX_PROPERTY_NAME_BYTES + 1U, 'x');
	const std::string oversizedLabel(RSS_TOKEN_DEFINITION_MAX_NAME_BYTES + 1U, 'x');
	// An omitted name is a public request to derive one. The derived name still
	// has to fit the durable schema field before any schema publication.
	const std::string defaultNameOverflowLabel(RSS_TOKEN_DEFINITION_MAX_NAME_BYTES - 16U, 'x');
	expectInvalidIndex({oversizedName, "Person", {"name"}});
	expectInvalidIndex({"person_name", oversizedLabel, {"name"}});
	expectInvalidIndex({"", defaultNameOverflowLabel, {"name"}});
	expectInvalidIndex({"person_name", "Person", {oversizedProperty}});
	expectInvalidIndex({"person_name", "", {"name"}});
	expectInvalidIndex({"person_name", "Person", {}});
	expectInvalidIndex({"person_name", "Person", {""}});
	expectInvalidIndex({"person_name", "Person", {"name", "name"}});
	const std::array<LogicalGraphNodeIndexCreateRequest, 2U> duplicateIndexes{
		LogicalGraphNodeIndexCreateRequest{"same", "Person", {"name"}},
		LogicalGraphNodeIndexCreateRequest{"same", "Person", {"email"}}};
	EXPECT_THROW(static_cast<void>(backend.createLogicalNodeIndexes(duplicateIndexes)), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(backend.createLogicalNodeVectorIndex({"vector", "", "embedding", 2U, "l2"})),
			 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(backend.createLogicalNodeVectorIndex({"vector", "Person", "", 2U, "l2"})),
			 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(backend.createLogicalNodeVectorIndex({oversizedName, "Person", "embedding", 2U, "l2"})),
			 FormatException);
	EXPECT_THROW(static_cast<void>(
				 backend.createLogicalNodeVectorIndex({"", defaultNameOverflowLabel, "embedding", 2U, "l2"})),
			 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(backend.createLogicalNodeVectorIndex({"vector", "Person", oversizedProperty, 2U, "l2"})),
			 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(backend.createLogicalNodeVectorIndex({"vector", oversizedLabel, "embedding", 2U, "l2"})),
			 FormatException);
	EXPECT_THROW(static_cast<void>(backend.createLogicalNodeVectorIndex({"vector", "Person", "embedding", 0U, "l2"})),
			 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(backend.createLogicalNodeVectorIndex({"vector", "Person", "embedding", 2U, "unknown"})),
			 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(backend.createLogicalConstraint({"", LogicalGraphConstraintEntityKind::LGCEK_NODE,
			 LogicalGraphConstraintKind::LGCK_UNIQUE, "Person", {"name"}, LogicalGraphConstraintPropertyType::LGCPT_NONE})),
			 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(backend.createLogicalConstraint({"constraint", LogicalGraphConstraintEntityKind::LGCEK_NODE,
			 LogicalGraphConstraintKind::LGCK_UNIQUE, "", {"name"}, LogicalGraphConstraintPropertyType::LGCPT_NONE})),
			 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(backend.createLogicalConstraint({"constraint", LogicalGraphConstraintEntityKind::LGCEK_NODE,
			 LogicalGraphConstraintKind::LGCK_UNIQUE, "Person", {}, LogicalGraphConstraintPropertyType::LGCPT_NONE})),
			 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(backend.createLogicalConstraint({"constraint", LogicalGraphConstraintEntityKind::LGCEK_NODE,
			 LogicalGraphConstraintKind::LGCK_UNIQUE, "Person", {""}, LogicalGraphConstraintPropertyType::LGCPT_NONE})),
			 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(backend.createLogicalConstraint({oversizedName, LogicalGraphConstraintEntityKind::LGCEK_NODE,
			 LogicalGraphConstraintKind::LGCK_UNIQUE, "Person", {"name"}, LogicalGraphConstraintPropertyType::LGCPT_NONE})),
			 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(backend.createLogicalConstraint({"constraint", LogicalGraphConstraintEntityKind::LGCEK_NODE,
			 LogicalGraphConstraintKind::LGCK_UNIQUE, oversizedLabel, {"name"}, LogicalGraphConstraintPropertyType::LGCPT_NONE})),
			 FormatException);
	EXPECT_THROW(static_cast<void>(backend.createLogicalConstraint({"constraint", LogicalGraphConstraintEntityKind::LGCEK_NODE,
			 LogicalGraphConstraintKind::LGCK_UNIQUE, "Person", {oversizedProperty}, LogicalGraphConstraintPropertyType::LGCPT_NONE})),
			 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(backend.dropLogicalIndexByName("")), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(backend.dropLogicalNodeIndexBySchema("", "name")), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(backend.dropLogicalNodeIndexBySchema("Person", "")), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(backend.dropLogicalConstraintByName("")), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(backend.listLogicalIndexes(0U)), FormatException);
	EXPECT_THROW(static_cast<void>(backend.listLogicalConstraints(0U)), FormatException);
	EXPECT_TRUE(backend.listLogicalIndexes(16U).empty());
	EXPECT_TRUE(backend.listLogicalConstraints(16U).empty());
	backend.close();
}
