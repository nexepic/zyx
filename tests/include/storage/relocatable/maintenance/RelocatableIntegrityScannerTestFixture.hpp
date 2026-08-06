/**
 * @file RelocatableIntegrityScannerTestFixture.hpp
 * @brief Shared temporary-file fixture for relocatable integrity scanner tests.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <filesystem>
#include <fstream>
#include <memory>
#include <span>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"

namespace graph::storage::test {

	inline Superblock integrityScannerInitialSuperblock() {
		Superblock result;
		result.database_id = DatabaseId(0x1020304050607080ULL);
		result.generation = Generation(1U);
		result.checkpoint_lsn = LogSequenceNumber(1U);
		return result;
	}

	inline uint64_t integrityScannerPagePayloadOffset(RelocatableMetadataStore &metadata, PageId pageId) {
		const MetadataSnapshotPin snapshot = metadata.openSnapshot();
		const PageLocation location = metadata.findPageLocation(snapshot, pageId);
		const CatalogEntry pool = metadata.findCatalog(snapshot, location.segment_id);
		return PhysicalSegmentAllocator::checkedSlotOffset(snapshot.superblock().data_region_offset, pool.physical_slot) +
			   static_cast<uint64_t>(location.page_index) * RSS_PAGE_SIZE + RSS_PAGE_OWNER_HEADER_BYTES;
	}

	inline void overwriteIntegrityScannerPagePayload(const std::shared_ptr<StorageIO> &io, RelocatableMetadataStore &metadata,
																 PageId pageId, std::span<const std::byte> bytes) {
		io->writeAt(integrityScannerPagePayloadOffset(metadata, pageId), bytes.data(), bytes.size());
	}

	inline uint64_t integrityScannerExtentHeaderOffset(RelocatableMetadataStore &metadata, SegmentId segmentId) {
		const MetadataSnapshotPin snapshot = metadata.openSnapshot();
		const CatalogEntry entry = metadata.findCatalog(snapshot, segmentId);
		return PhysicalSegmentAllocator::checkedSlotOffset(snapshot.superblock().data_region_offset, entry.physical_slot) +
			   RSS_ENVELOPE_BYTES;
	}

	inline LogSequenceNumber integrityScannerNextCheckpointLsn(RelocatableMetadataStore &metadata) {
		MetadataSnapshotPin snapshot = metadata.openSnapshot();
		const LogSequenceNumber result(snapshot.superblock().checkpoint_lsn.value() + 1U);
		snapshot.release();
		return result;
	}

	class RelocatableIntegrityScannerTestFixture : public ::testing::Test {
	protected:
		std::filesystem::path path;
		std::shared_ptr<std::fstream> file;
		std::shared_ptr<StorageIO> io;

		void SetUp() override {
			const auto uuid = boost::uuids::random_generator()();
			path = std::filesystem::temp_directory_path() / ("relocatable_integrity_scanner_" + to_string(uuid));
			file = std::make_shared<std::fstream>(path, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
			ASSERT_TRUE(file->is_open());
			io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		}

		void TearDown() override {
			io.reset();
			if (file && file->is_open()) {
				file->close();
			}
			std::error_code error;
			std::filesystem::remove(path, error);
		}
	};

} // namespace graph::storage::test
