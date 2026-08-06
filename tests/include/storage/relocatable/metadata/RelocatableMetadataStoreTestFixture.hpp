/**
 * @file RelocatableMetadataStoreTestFixture.hpp
 * @brief Shared fixture and helpers for relocatable metadata-store tests.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/StorageHeaders.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"

using namespace graph::storage;

namespace {

	[[maybe_unused]] void expectFormatError(const std::function<void()> &operation, FormatError expected) {
		try {
			operation();
			FAIL() << "Expected FormatException";
		} catch (const FormatException &error) {
			EXPECT_EQ(error.error(), expected);
		}
	}

	Superblock superblock(uint64_t generation, uint64_t checkpointLsn) {
		Superblock result;
		result.database_id = DatabaseId(0x1020304050607080ULL);
		result.generation = Generation(generation);
		result.checkpoint_lsn = LogSequenceNumber(checkpointLsn);
		return result;
	}

	CatalogEntry entry(uint64_t physicalSlot, uint64_t generation, uint64_t lsn) {
		CatalogEntry result;
		// SegmentId 1 is the bootstrap metadata pool and the following identity
		// is held by the bootstrap emergency reserve. Typed segments begin after
		// both durable system objects in the clean single-file format.
		result.segment_id = SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID);
		result.physical_slot = PhysicalSlotId(physicalSlot);
		result.kind = SegmentKind::SGK_ENTITY_PAGE_POOL;
		result.owner_id = 19;
		result.generation = Generation(generation);
		result.state = SegmentState::SGS_SEALED;
		result.committed_lsn = LogSequenceNumber(lsn);
		return result;
	}

	CatalogSlotReservationRequest reservationRequest(uint64_t segmentId, uint64_t generation) {
		CatalogSlotReservationRequest result;
		result.segment_id = SegmentId(segmentId);
		result.kind = SegmentKind::SGK_ENTITY_PAGE_POOL;
		result.owner_id = 19;
		result.segment_generation = Generation(generation);
		result.state = SegmentState::SGS_SEALED;
		return result;
	}

	[[maybe_unused]] std::vector<CatalogSlotReservationRequest>
	reservationRequests(uint64_t firstSegmentId, uint64_t count) {
		std::vector<CatalogSlotReservationRequest> result;
		result.reserve(count);
		for (uint64_t offset = 0; offset < count; ++offset) {
			result.push_back(reservationRequest(firstSegmentId + offset, 1));
		}
		return result;
	}

	std::vector<CatalogEntry> catalogEntriesForLeafCount(uint64_t leafCount) {
		std::vector<CatalogEntry> result;
		result.reserve(leafCount);
		for (uint64_t ordinal = 0; ordinal < leafCount; ++ordinal) {
			CatalogEntry next = entry(1000U + ordinal, 1, 2);
			next.segment_id = SegmentId(ordinal * RSS_CATALOG_PAGE_MAX_ENTRIES + RSS_FIRST_ALLOCATABLE_SEGMENT_ID);
			result.push_back(next);
		}
		return result;
	}

	std::vector<CatalogEntry> entriesThatRequireAnotherMetadataPool() { return catalogEntriesForLeafCount(30U); }

	// Pin each historical root while replacing a large Catalog. This forces the
	// COW writer to append fresh metadata frames instead of recycling the prior
	// generation, leaving at least one sealed pool reclaimable once callers
	// release the returned snapshots.
	[[maybe_unused]] std::vector<MetadataSnapshotPin>
	createReclaimableMetadataPool(RelocatableMetadataStore &store) {
		const std::vector<CatalogEntry> original = entriesThatRequireAnotherMetadataPool();
		static_cast<void>(store.upsertCatalog(original, LogSequenceNumber(2)));

		std::vector<MetadataSnapshotPin> retained;
		retained.emplace_back(store.openSnapshot());
		static_cast<void>(store.setPhysicalSlotAllocated(PhysicalSlotId(40), true, LogSequenceNumber(3)));
		retained.emplace_back(store.openSnapshot());

		for (uint64_t checkpoint = 4U; checkpoint <= 6U; ++checkpoint) {
			std::vector<CatalogEntry> replacement = original;
			for (size_t index = 0; index < replacement.size(); ++index) {
				replacement[index].physical_slot = PhysicalSlotId(2000U + checkpoint * 100U + index);
				replacement[index].generation = Generation(2);
				replacement[index].committed_lsn = LogSequenceNumber(checkpoint);
			}
			static_cast<void>(store.upsertCatalog(replacement, LogSequenceNumber(checkpoint)));
			if (checkpoint != 6U) {
				retained.emplace_back(store.openSnapshot());
			}
		}
		return retained;
	}

	[[maybe_unused]] MetadataPublishOptions withBootstrapFailpoint(BootstrapPublishFailpoint failpoint) {
		MetadataPublishOptions options;
		options.bootstrap_failpoint = failpoint;
		return options;
	}

} // namespace

class RelocatableMetadataStoreTest : public ::testing::Test {
protected:
	std::filesystem::path testFilePath;
	std::shared_ptr<std::fstream> file;
	std::shared_ptr<StorageIO> io;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		testFilePath = std::filesystem::temp_directory_path() / ("relocatable_metadata_store_" + to_string(uuid) + ".dat");
		file = std::make_shared<std::fstream>(testFilePath,
										  std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(file->is_open());
		io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	}

	void TearDown() override {
		io.reset();
		if (file && file->is_open()) {
			file->close();
		}
		file.reset();
		std::error_code error;
		std::filesystem::remove(testFilePath, error);
	}
};
