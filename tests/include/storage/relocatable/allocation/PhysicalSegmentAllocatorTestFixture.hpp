/**
 * @file PhysicalSegmentAllocatorTestFixture.hpp
 * @brief Allocation, integrity, and publication-boundary tests for relocatable physical slots.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <array>
#include <atomic>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <source_location>
#include <vector>

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/PwriteHelper.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"

using namespace graph::storage;

namespace {

	void expectFormatError(const std::function<void()> &operation, FormatError expected,
						   const std::source_location location = std::source_location::current()) {
		try {
			operation();
			ADD_FAILURE_AT(location.file_name(), static_cast<int>(location.line())) << "Expected FormatException";
		} catch (const FormatException &error) {
			if (error.error() != expected) {
				ADD_FAILURE_AT(location.file_name(), static_cast<int>(location.line()))
						<< "Unexpected FormatError: " << error.what();
			}
		}
	}

	Superblock superblock(uint64_t generation, uint64_t checkpointLsn) {
		Superblock result;
		result.database_id = DatabaseId(0x1020304050607080ULL);
		result.generation = Generation(generation);
		result.checkpoint_lsn = LogSequenceNumber(checkpointLsn);
		return result;
	}

	// SegmentId 1 is the bootstrap pool and the following ID belongs to the
	// bootstrap emergency reserve. Test ordinals begin at one after both IDs.
	SegmentId testSegmentId(uint64_t ordinal) { return SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID + ordinal - 1U); }

	PhysicalSegmentAllocationRequest request(uint64_t segmentId, std::span<const std::byte> payload = {}) {
		PhysicalSegmentAllocationRequest result;
		result.segment_id = testSegmentId(segmentId);
		result.kind = SegmentKind::SGK_ENTITY_PAGE_POOL;
		result.owner_id = 99;
		result.generation = Generation(1);
		result.payload.assign(payload.begin(), payload.end());
		return result;
	}

	MetadataPublishOptions withBootstrapFailpoint(BootstrapPublishFailpoint failpoint) {
		MetadataPublishOptions options;
		options.bootstrap_failpoint = failpoint;
		return options;
	}

	using NativeWriteHookFactoryForTesting = std::function<StorageIO::NativeWriteHookForTesting(const Superblock &)>;

	std::vector<CatalogEntry> catalogEntriesThatExhaustOneNormalMetadataPool() {
		std::vector<CatalogEntry> entries;
		entries.reserve(RSS_PAGE_POOL_DATA_FRAME_COUNT - 1U);
		for (uint64_t ordinal = 0U; ordinal < RSS_PAGE_POOL_DATA_FRAME_COUNT - 1U; ++ordinal) {
			CatalogEntry entry;
			entry.segment_id = SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID + ordinal * RSS_CATALOG_PAGE_MAX_ENTRIES);
			entry.physical_slot = PhysicalSlotId(1000U + ordinal);
			// These records stress Catalog capacity without physical bodies;
            // they must not claim the identity of materialized shared pools.
            entry.kind = SegmentKind::SGK_CODEBOOK_EXTENT;
			entry.owner_id = 99U;
			entry.generation = Generation(1U);
			entry.state = SegmentState::SGS_SEALED;
			entry.committed_lsn = LogSequenceNumber(2U);
			entries.push_back(entry);
		}
		return entries;
	}

} // namespace

class RelocatablePhysicalSegmentAllocatorTest : public ::testing::Test {
protected:
	std::filesystem::path dataPath;
	std::shared_ptr<std::fstream> dataFile;
	std::shared_ptr<StorageIO> io;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		dataPath =
				std::filesystem::temp_directory_path() / ("relocatable_physical_segment_" + to_string(uuid) + ".dat");
		dataFile = std::make_shared<std::fstream>(dataPath,
												  std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(dataFile->is_open());
		io = std::make_shared<StorageIO>(dataFile, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	}

	void TearDown() override {
		io.reset();
		if (dataFile && dataFile->is_open()) {
			dataFile->close();
		}
		dataFile.reset();
		std::error_code error;
		std::filesystem::remove(dataPath, error);
	}
};
