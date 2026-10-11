/**
 * @file test_RelocatableProcessCrash_AdjacencyMaintenance.cpp
 * @brief Real process-crash recovery for ordinary adjacency maintenance.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 */

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/backend/RelocatableStorageBackend.hpp"
#include "graph/storage/relocatable/format/SuperblockFile.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "storage/relocatable/format/RelocatableProcessCrashSupport.hpp"

using namespace graph::storage;

namespace {

	constexpr std::string_view CRASH_CHILD_ADJACENCY_MAINTENANCE_BEFORE_PUBLICATION =
			"adjacency-maintenance-before-publication";
	constexpr std::string_view CRASH_CHILD_ADJACENCY_MAINTENANCE_AFTER_PUBLICATION =
			"adjacency-maintenance-after-publication";

	using graph::test::relocatable_crash::CrashChild;
	using graph::test::relocatable_crash::waitToBeKilled;
	using graph::test::relocatable_crash::writeFailureMarker;
	using graph::test::relocatable_crash::writeMarker;

	[[nodiscard]] RelocatableMaintenanceOptions maintenanceOptions() {
		RelocatableMaintenanceOptions options;
		options.loop.maximum_outstanding_tasks = 2U;
		options.loop.minimum_cycle_interval = std::chrono::milliseconds(1);
		return options;
	}

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

namespace graph::test {

	int runRelocatableAdjacencyMaintenanceCrashChild(std::string_view mode, const std::filesystem::path &databasePath,
																						 const std::filesystem::path &markerPath) {
		if (mode != CRASH_CHILD_ADJACENCY_MAINTENANCE_BEFORE_PUBLICATION &&
			mode != CRASH_CHILD_ADJACENCY_MAINTENANCE_AFTER_PUBLICATION) {
			return -1;
		}
		try {
			const bool afterPublication = mode == CRASH_CHILD_ADJACENCY_MAINTENANCE_AFTER_PUBLICATION;
			PhysicalPageRef priorCatalogRoot;
			{
				auto data = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
				if (!data->is_open()) return 108;
				auto io = std::make_shared<StorageIO>(data, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
				priorCatalogRoot = SuperblockFile(io).recover().catalog_root;
			}
			RelocatableStorageBackendOptions options;
			options.data_path = databasePath;
			options.open_mode = OpenMode::OPEN_EXISTING_FILE;
			options.maintenance = maintenanceOptions();
			std::atomic<bool> observedCandidate{false};
			StorageIO::ScopedAllThreadNativeWriteHookForTesting pauseAtMaintenanceCandidate(
					[afterPublication, priorCatalogRoot, &observedCandidate, &markerPath](file_handle_t fd, const void *bytes, size_t byteCount,
																						 pwrite_off_t offset) {
						if (!observedCandidate.load(std::memory_order_relaxed) && afterPublication &&
							byteCount == RSS_PAGE_SIZE &&
							(offset == SuperblockFile::slotOffset(0U) || offset == SuperblockFile::slotOffset(1U))) {
							try {
								const Superblock candidate = SuperblockCodec::decode(
										std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount));
								if (candidate.catalog_root != priorCatalogRoot) {
									observedCandidate.store(true, std::memory_order_relaxed);
									const pwrite_ssize_t written = portable_pwrite(fd, bytes, byteCount, offset);
									if (written != static_cast<pwrite_ssize_t>(byteCount)) return written;
									writeMarker(markerPath);
									waitToBeKilled();
								}
							} catch (const FormatException &) {
								// Only the replacement Catalog publication is this boundary.
							}
						}
						if (!afterPublication && byteCount == RSS_PAGE_SIZE &&
							(offset != SuperblockFile::slotOffset(0U) && offset != SuperblockFile::slotOffset(1U)) &&
							!observedCandidate.exchange(true, std::memory_order_relaxed)) {
							writeMarker(markerPath);
							waitToBeKilled();
						}
						return portable_pwrite(fd, bytes, byteCount, offset);
					});
			RelocatableStorageBackend backend(std::move(options));
			backend.open();
			for (size_t attempt = 0U; attempt < 10'000U && !observedCandidate.load(std::memory_order_relaxed);
				 ++attempt) {
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			return 102;
		} catch (const std::exception &error) {
			writeFailureMarker(markerPath, error.what());
			return 101;
		}
	}

} // namespace graph::test

class RelocatableProcessCrashAdjacencyMaintenanceTest : public ::testing::Test {
protected:
	std::filesystem::path databasePath;
	std::filesystem::path markerPath;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		databasePath = std::filesystem::temp_directory_path() /
				("relocatable_adjacency_maintenance_" + to_string(uuid) + ".db");
		markerPath = databasePath.string() + ".marker";
	}

	void TearDown() override {
		std::error_code error;
		std::filesystem::remove(databasePath, error);
		std::filesystem::remove(markerPath, error);
		std::filesystem::remove(markerPath.string() + ".error", error);
	}

	[[nodiscard]] CatalogEntry createDatabaseWithEmptyAdjacencyPool() const {
		RelocatableStorageBackendOptions options;
		options.data_path = databasePath;
		options.open_mode = OpenMode::OPEN_CREATE_NEW_FILE;
		auto created = RelocatableStorageBackend::initializeNew(std::move(options));
		if (created == nullptr) {
			throw std::runtime_error("failed to initialize ordinary adjacency maintenance crash database");
		}
		created->open();
		const std::vector<uint64_t> nodes =
				created->createLogicalNodes({{{"MaintenanceSource"}, {}}, {{"MaintenanceTarget"}, {}}});
		if (nodes.size() != 2U) {
			throw std::runtime_error("failed to create ordinary adjacency maintenance endpoints");
		}
		const std::vector<uint64_t> edges =
				created->createLogicalEdges({{nodes[0], nodes[1], "MAINTENANCE_EDGE", {}}});
		if (edges.size() != 1U) {
			throw std::runtime_error("failed to create ordinary adjacency maintenance Edge");
		}

		CatalogEntry adjacencyPool;
		{
			auto data = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
			if (!data->is_open()) {
				throw std::runtime_error("failed to inspect ordinary adjacency maintenance pool");
			}
			auto io = std::make_shared<StorageIO>(data, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
			auto metadata = RelocatableMetadataStore::open(io);
			MetadataSnapshotPin snapshot = metadata->openSnapshot();
			const std::optional<IndexRootLocation> root =
					metadata->findIndexRootOptional(snapshot, RSS_GLOBAL_ADJACENCY_INDEX_ID);
			if (!root.has_value()) {
				throw std::runtime_error("ordinary adjacency root unexpectedly absent before maintenance crash");
			}
			adjacencyPool = metadata->findCatalog(snapshot, metadata->findPageLocation(snapshot, root->root_page_id).segment_id);
			snapshot.release();
		}

		created->deleteLogicalEdges({{edges.front()}});
		created->close();
		return adjacencyPool;
	}
};

TEST_F(RelocatableProcessCrashAdjacencyMaintenanceTest,
		KillBeforeAdjacencyMaintenancePublicationRetainsSealedPoolAndAllowsReclaimRetry) {
	const CatalogEntry adjacencyPool = createDatabaseWithEmptyAdjacencyPool();
	ASSERT_EQ(adjacencyPool.owner_id, RSS_GLOBAL_ADJACENCY_INDEX_ID);
	CrashChild child(databasePath, markerPath, CRASH_CHILD_ADJACENCY_MAINTENANCE_BEFORE_PUBLICATION);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	{
		auto data = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
		ASSERT_TRUE(data->is_open());
		auto io = std::make_shared<StorageIO>(data, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		auto metadata = RelocatableMetadataStore::open(io);
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		EXPECT_FALSE(metadata->findIndexRootOptional(snapshot, RSS_GLOBAL_ADJACENCY_INDEX_ID).has_value());
		EXPECT_EQ(metadata->findCatalog(snapshot, adjacencyPool.segment_id).state, SegmentState::SGS_SEALED);
		EXPECT_TRUE(metadata->isPhysicalSlotAllocated(snapshot, adjacencyPool.physical_slot));
		snapshot.release();
	}

	RelocatableStorageBackendOptions reopenOptions;
	reopenOptions.data_path = databasePath;
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	reopenOptions.maintenance = maintenanceOptions();
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
			FAIL() << "ordinary adjacency maintenance reclaim retry failed: " << error.what();
		}
	}
	EXPECT_GE(reopened.maintenanceMetrics().global_adjacency_pools_released, 1U);
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();

	auto data = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(data->is_open());
	auto io = std::make_shared<StorageIO>(data, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_FALSE(metadata->isPhysicalSlotAllocated(snapshot, adjacencyPool.physical_slot));
	snapshot.release();
}

TEST_F(RelocatableProcessCrashAdjacencyMaintenanceTest,
		KillAfterAdjacencyMaintenancePublicationRetainsRetiredPoolAndAllowsReleaseRetry) {
	const CatalogEntry adjacencyPool = createDatabaseWithEmptyAdjacencyPool();
	ASSERT_EQ(adjacencyPool.owner_id, RSS_GLOBAL_ADJACENCY_INDEX_ID);
	CrashChild child(databasePath, markerPath, CRASH_CHILD_ADJACENCY_MAINTENANCE_AFTER_PUBLICATION);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	{
		auto data = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
		ASSERT_TRUE(data->is_open());
		auto io = std::make_shared<StorageIO>(data, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		auto metadata = RelocatableMetadataStore::open(io);
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		EXPECT_FALSE(metadata->findIndexRootOptional(snapshot, RSS_GLOBAL_ADJACENCY_INDEX_ID).has_value());
		EXPECT_EQ(metadata->findCatalog(snapshot, adjacencyPool.segment_id).state, SegmentState::SGS_RETIRED);
		EXPECT_TRUE(metadata->isPhysicalSlotAllocated(snapshot, adjacencyPool.physical_slot));
		snapshot.release();
	}

	RelocatableStorageBackendOptions reopenOptions;
	reopenOptions.data_path = databasePath;
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	reopenOptions.maintenance = maintenanceOptions();
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
			FAIL() << "ordinary adjacency maintenance release retry failed: " << error.what();
		}
	}
	EXPECT_GE(reopened.maintenanceMetrics().global_adjacency_pools_released, 1U);
	EXPECT_EQ(reopened.maintenanceMetrics().global_adjacency_pools_retired, 0U);
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();

	auto data = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(data->is_open());
	auto io = std::make_shared<StorageIO>(data, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_FALSE(metadata->isPhysicalSlotAllocated(snapshot, adjacencyPool.physical_slot));
	snapshot.release();
}
