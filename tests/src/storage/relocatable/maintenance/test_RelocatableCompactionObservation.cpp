/**
 * @file test_RelocatableCompactionObservation.cpp
 * @brief Catalog lifecycle accounting tests for relocatable compaction observation.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <filesystem>
#include <fstream>
#include <memory>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"

using namespace graph::storage;

namespace {

	Superblock initialSuperblock() {
		Superblock result;
		result.database_id = DatabaseId(0x1020304050607080ULL);
		result.generation = Generation(1);
		result.checkpoint_lsn = LogSequenceNumber(1);
		return result;
	}

	PhysicalSegmentAllocationRequest allocationRequest() {
		PhysicalSegmentAllocationRequest result;
		result.segment_id = SegmentId(71);
		result.kind = SegmentKind::SGK_ENTITY_PAGE_POOL;
		result.owner_id = 17;
		result.generation = Generation(1);
		return result;
	}

} // namespace

class RelocatableCompactionObservationTest : public ::testing::Test {
protected:
	std::filesystem::path path;
	std::filesystem::path walPath;
	std::shared_ptr<std::fstream> file;
	std::shared_ptr<std::fstream> walFile;
	std::shared_ptr<StorageIO> io;
	std::shared_ptr<StorageIO> walIo;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		path = std::filesystem::temp_directory_path() / ("relocatable_compaction_observation_" + to_string(uuid));
		walPath = path.string() + ".foreign";
		file = std::make_shared<std::fstream>(path, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		walFile = std::make_shared<std::fstream>(walPath,
												 std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(file->is_open());
		ASSERT_TRUE(walFile->is_open());
		io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		walIo = std::make_shared<StorageIO>(walFile, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	}

	void TearDown() override {
		io.reset();
		walIo.reset();
		if (file && file->is_open()) {
			file->close();
		}
		if (walFile && walFile->is_open()) {
			walFile->close();
		}
		std::error_code error;
		std::filesystem::remove(path, error);
		std::filesystem::remove(walPath, error);
	}
};

TEST_F(RelocatableCompactionObservationTest, ReportsSealedRetiredObsoleteAndPinnedRelocationGenerations) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	const RelocatableCompactionObservation baseline = metadata->observeCompactionState();
	PhysicalSegmentAllocator physical(io, *metadata);
	ASSERT_TRUE(physical.allocate(allocationRequest(), PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2))
						.has_value());
	const RelocatableCompactionObservation sealed = metadata->observeCompactionState();
	ASSERT_GE(sealed.sealed_segments, 1U);
	const uint64_t metadataSealedSegments = sealed.sealed_segments - 1U;
	EXPECT_EQ(sealed.sealed_segments, metadataSealedSegments + 1U);
	EXPECT_EQ(sealed.retired_segments, baseline.retired_segments);
	EXPECT_EQ(sealed.obsolete_segments, baseline.obsolete_segments);
	EXPECT_EQ(sealed.pending_relocation_sources, 0U);

	MetadataSnapshotPin pinned = metadata->openSnapshot();
	ASSERT_TRUE(physical.relocate(SegmentId(71), Generation(1), PhysicalSlotId(41), PhysicalSlotId(41),
								  LogSequenceNumber(3))
						.has_value());
	const RelocatableCompactionObservation relocating = metadata->observeCompactionState();
	EXPECT_EQ(relocating.active_snapshot_count, 1U);
	EXPECT_EQ(relocating.sealed_segments, metadataSealedSegments + 1U);
	EXPECT_EQ(relocating.pending_relocation_sources, 1U);
	EXPECT_EQ(relocating.retainedGenerationCount(), 1U);
	pinned.release();
	static_cast<void>(physical.releaseRelocatedSource(SegmentId(71), Generation(2), LogSequenceNumber(4)));
	const RelocatableCompactionObservation releasedRelocation = metadata->observeCompactionState();
	EXPECT_EQ(releasedRelocation.pending_relocation_sources, 0U);
	EXPECT_EQ(releasedRelocation.retainedGenerationCount(), 0U);

	static_cast<void>(physical.retire(SegmentId(71), Generation(2), LogSequenceNumber(5)));
	const RelocatableCompactionObservation retired = metadata->observeCompactionState();
	EXPECT_EQ(retired.sealed_segments, metadataSealedSegments);
	EXPECT_EQ(retired.retired_segments, baseline.retired_segments + 1U);
	EXPECT_EQ(retired.retainedGenerationCount(), 1U);
	static_cast<void>(physical.releaseRetired(SegmentId(71), Generation(2), LogSequenceNumber(6)));
	const RelocatableCompactionObservation obsolete = metadata->observeCompactionState();
	EXPECT_EQ(obsolete.retired_segments, baseline.retired_segments);
	EXPECT_EQ(obsolete.obsolete_segments, baseline.obsolete_segments + 1U);
}
