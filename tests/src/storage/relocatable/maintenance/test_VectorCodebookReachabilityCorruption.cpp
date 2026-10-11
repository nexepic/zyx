/**
 * @file test_VectorCodebookReachabilityCorruption.cpp
 * @brief Persistent corruption tests for immutable Vector Codebook reclamation.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/vector/VectorCodebookExtent.hpp"
#include "graph/storage/relocatable/maintenance/VectorCodebookReachabilityReclaimer.hpp"
#include "graph/storage/relocatable/vector/VectorCodebookStore.hpp"
#include "graph/storage/relocatable/vector/VectorManifestStore.hpp"
#include "graph/utils/ChecksumUtils.hpp"

using namespace graph::storage;

namespace {

	Superblock initialSuperblock() {
		Superblock result;
		result.database_id = DatabaseId(0x1122334455667788ULL);
		result.generation = Generation(1U);
		result.checkpoint_lsn = LogSequenceNumber(1U);
		return result;
	}

} // namespace

class RelocatableVectorCodebookReachabilityCorruptionTest : public ::testing::Test {
protected:
	std::filesystem::path data_path_;
	std::filesystem::path wal_path_;
	std::shared_ptr<std::fstream> data_file_;
	std::shared_ptr<std::fstream> wal_file_;
	std::shared_ptr<StorageIO> data_io_;
	std::shared_ptr<StorageIO> wal_io_;
	std::unique_ptr<RelocatableMetadataStore> metadata_;
	std::unique_ptr<PhysicalSegmentAllocator> physical_;
	std::unique_ptr<VectorManifestStore> manifests_;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		data_path_ = std::filesystem::temp_directory_path() / ("relocatable_codebook_reclaim_corruption_" + to_string(uuid));
		wal_path_ = data_path_.string() + ".foreign";
		data_file_ = std::make_shared<std::fstream>(data_path_,
													std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		wal_file_ = std::make_shared<std::fstream>(wal_path_,
												   std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(data_file_->is_open());
		ASSERT_TRUE(wal_file_->is_open());
		data_io_ = std::make_shared<StorageIO>(data_file_, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		wal_io_ = std::make_shared<StorageIO>(wal_file_, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		metadata_ = RelocatableMetadataStore::initializeNew(data_io_, initialSuperblock());
		physical_ = std::make_unique<PhysicalSegmentAllocator>(data_io_, *metadata_);
		manifests_ = std::make_unique<VectorManifestStore>(data_io_, *metadata_);
	}

	void TearDown() override {
		manifests_.reset();
		physical_.reset();
		metadata_.reset();
		data_io_.reset();
		wal_io_.reset();
		if (data_file_ && data_file_->is_open()) {
			data_file_->close();
		}
		if (wal_file_ && wal_file_->is_open()) {
			wal_file_->close();
		}
		std::error_code error;
		std::filesystem::remove(data_path_, error);
		std::filesystem::remove(wal_path_, error);
	}

	[[nodiscard]] LogSequenceNumber nextCheckpointLsn() const {
		const MetadataSnapshotPin snapshot = metadata_->openSnapshot();
		return LogSequenceNumber(snapshot.superblock().checkpoint_lsn.value() + 1U);
	}

	[[nodiscard]] VectorCodebookReachabilityReclaimOptions options() const { return {8U, 16U, 64U, 64U, 16U}; }

	void writeInlineCodebook(uint64_t codebookId, SegmentId segmentId) {
		VectorCodebookStore codebooks(*physical_, *metadata_);
		VectorCodebookWriteRequest request;
		request.codebook_id = codebookId;
		request.manifest_segment_id = segmentId;
		request.generation = Generation(1U);
		request.element_type = VectorCodebookElementType::VCET_FLOAT32;
		request.dimensions = 2U;
		request.subspace_count = 1U;
		request.centroid_count = 3U;
		const std::vector<std::byte> payload(24U, std::byte{0x5A});
		static_cast<void>(codebooks.writeImmutable(request, payload, PhysicalSlotId(40U), PhysicalSlotId(95U),
												   nextCheckpointLsn()));
	}

	void allocateOrphanedDataExtent(uint64_t codebookId, SegmentId segmentId) {
		const std::vector<std::byte> payload(8U, std::byte{0xA5});
		const ExtentId extentId = VectorCodebookExtentIdCodec::dataExtentId(segmentId);
		VectorCodebookExtentHeader header;
		header.kind = VectorCodebookExtentKind::VCEK_DATA;
		header.codebook_id = codebookId;
		header.codebook_generation = Generation(1U);
		header.extent_id = extentId;
		header.ordinal = 0U;
		header.block_bytes = static_cast<uint32_t>(payload.size());
		header.block_crc = graph::utils::calculateCrc(payload.data(), payload.size());
		const VectorCodebookExtentHeaderCodec::Bytes encoded = VectorCodebookExtentHeaderCodec::encode(header);
		const PhysicalSegmentAllocationRequest request{
				segmentId,
				SegmentKind::SGK_CODEBOOK_EXTENT,
				codebookId,
				Generation(1U),
				static_cast<uint16_t>(RSS_ENVELOPE_BYTES + RSS_VECTOR_CODEBOOK_EXTENT_HEADER_BYTES),
				encoded,
				payload};
		const ExtentLocation location{extentId, SegmentId(segmentId), RSS_PAGE_POOL_FIRST_DATA_FRAME,
									  RSS_PAGE_POOL_DATA_FRAME_COUNT, Generation(1U)};
		ASSERT_TRUE(physical_
							->allocateExtentSegments(std::span(&request, 1U), std::span(&location, 1U),
													 PhysicalSlotId(40U), PhysicalSlotId(95U), nextCheckpointLsn())
							.has_value());
	}
};

TEST_F(RelocatableVectorCodebookReachabilityCorruptionTest, RejectsCodebookDataExtentWithoutACanonicalManifest) {
	allocateOrphanedDataExtent(17U, SegmentId(9701U));

	VectorCodebookReachabilityReclaimer reclaimer(*physical_, *manifests_, *metadata_);
	EXPECT_THROW(static_cast<void>(reclaimer.reclaim(options(), nextCheckpointLsn())), FormatException);
}

TEST_F(RelocatableVectorCodebookReachabilityCorruptionTest, RejectsUnownedExtentAlongsideAnInlineCodebook) {
	writeInlineCodebook(17U, SegmentId(9701U));
	allocateOrphanedDataExtent(17U, SegmentId(9702U));

	VectorCodebookReachabilityReclaimer reclaimer(*physical_, *manifests_, *metadata_);
	EXPECT_THROW(static_cast<void>(reclaimer.reclaim(options(), nextCheckpointLsn())), FormatException);
}

TEST_F(RelocatableVectorCodebookReachabilityCorruptionTest, RejectsAPartialExtentDirectorySpan) {
	writeInlineCodebook(17U, SegmentId(9701U));
	const MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	const ExtentLocation root =
			metadata_->findExtentLocation(snapshot, VectorCodebookExtentIdCodec::manifestExtentId(17U));
	const ExtentLocation partial{root.extent_id, root.segment_id, root.first_page,
								 static_cast<uint16_t>(root.page_count - 1U), root.generation};
	static_cast<void>(metadata_->upsertLocations({}, std::span(&partial, 1U), nextCheckpointLsn()));

	VectorCodebookReachabilityReclaimer reclaimer(*physical_, *manifests_, *metadata_);
	EXPECT_THROW(static_cast<void>(reclaimer.reclaim(options(), nextCheckpointLsn())), FormatException);
}

TEST_F(RelocatableVectorCodebookReachabilityCorruptionTest, RejectsAnExtentDirectorySpanOutsideTheDataFrameRange) {
	writeInlineCodebook(17U, SegmentId(9701U));
	const MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	const ExtentLocation root =
			metadata_->findExtentLocation(snapshot, VectorCodebookExtentIdCodec::manifestExtentId(17U));
	const ExtentLocation shifted{root.extent_id, root.segment_id, static_cast<uint16_t>(root.first_page + 1U),
								 static_cast<uint16_t>(root.page_count - 1U), root.generation};
	static_cast<void>(metadata_->upsertLocations({}, std::span(&shifted, 1U), nextCheckpointLsn()));

	VectorCodebookReachabilityReclaimer reclaimer(*physical_, *manifests_, *metadata_);
	EXPECT_THROW(static_cast<void>(reclaimer.reclaim(options(), nextCheckpointLsn())), FormatException);
}

TEST_F(RelocatableVectorCodebookReachabilityCorruptionTest, IgnoresAnExtentOwnedByAnotherStorageKind) {
	const PhysicalSegmentAllocationRequest request{SegmentId(9703U), SegmentKind::SGK_STATE_PAGE_POOL, 17U,
															 Generation(1U), RSS_ENVELOPE_BYTES, {}, {}};
	ASSERT_TRUE(physical_->allocate(request, PhysicalSlotId(40U), PhysicalSlotId(95U), nextCheckpointLsn()).has_value());
	const ExtentLocation unrelated{ExtentId(9703U), SegmentId(9703U), RSS_PAGE_POOL_FIRST_DATA_FRAME,
											 RSS_PAGE_POOL_DATA_FRAME_COUNT, Generation(1U)};
	static_cast<void>(metadata_->upsertLocations({}, std::span(&unrelated, 1U), nextCheckpointLsn()));

	VectorCodebookReachabilityReclaimer reclaimer(*physical_, *manifests_, *metadata_);
	EXPECT_NO_THROW(static_cast<void>(reclaimer.reclaim(options(), nextCheckpointLsn())));
}
