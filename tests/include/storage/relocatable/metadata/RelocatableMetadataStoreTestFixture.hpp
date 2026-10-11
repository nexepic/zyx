/**
 * @file RelocatableMetadataStoreTestFixture.hpp
 * @brief Shared fixture and helpers for relocatable metadata-store tests.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"
#include "graph/storage/relocatable/allocation/SharedPagePool.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"

using namespace graph::storage;

namespace {

	[[maybe_unused]] void expectFormatError(const std::function<void()> &operation, FormatError expected) {
		try {
		operation();
		FAIL() << "Expected FormatException";
	} catch (const FormatException &error) {
		EXPECT_EQ(error.error(), expected) << error.what();
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

	// Establish a fully published v5 route branch with a sparse routed source
	// pool. Tests can then exercise page-local evacuation without relying on
	// the physical-root fallback.
	[[maybe_unused]] void prepareRoutedMetadataPoolForEvacuation(RelocatableMetadataStore &store) {
		std::vector<MetadataSnapshotPin> retained = createReclaimableMetadataPool(store);
		for (MetadataSnapshotPin &snapshot: retained) {
			snapshot.release();
		}
		static_cast<void>(store.migratePageDirectoryToBootstrap(LogSequenceNumber(7U)));
		CatalogEntry catalog = entry(2U, 1U, 8U);
		catalog.segment_id = SegmentId(200U);
		catalog.owner_id = 77U;
		static_cast<void>(store.upsertCatalog(std::span(&catalog, 1U), LogSequenceNumber(8U)));
		const ExtentLocation extent{ExtentId(1U), SegmentId(20U), RSS_PAGE_POOL_FIRST_DATA_FRAME, 1U,
									Generation(1U)};
		static_cast<void>(store.upsertLocations({}, std::span(&extent, 1U), LogSequenceNumber(9U)));
	}

	[[maybe_unused]] MetadataPublishOptions withBootstrapFailpoint(BootstrapPublishFailpoint failpoint) {
		MetadataPublishOptions options;
		options.bootstrap_failpoint = failpoint;
		return options;
	}

	// Low-level metadata tests use the publication coordinator directly.  Give
	// those tests real typed pool bytes so the coordinator's pre-publication
	// authentication exercises the same envelope and owner invariants as a
	// production Entity/Property writer.
	[[maybe_unused]] void initializeTestSharedPagePool(StorageIO &io, uint64_t dataRegionOffset,
														  const CatalogEntry &pool) {
		if (!isSharedPagePoolSegmentKind(pool.kind) || pool.state != SegmentState::SGS_SEALED ||
			!pool.segment_id.isValid() || !pool.physical_slot.isValid() || !pool.generation.isValid() ||
			!pool.committed_lsn.isValid()) {
			throw std::invalid_argument("test shared page-pool has an invalid Catalog identity");
		}
		SegmentEnvelope envelope;
		envelope.header_bytes = RSS_PAGE_SIZE;
		envelope.kind = pool.kind;
		envelope.state = pool.state;
		envelope.segment_id = pool.segment_id;
		envelope.owner_id = pool.owner_id;
		envelope.generation = pool.generation;
		envelope.committed_lsn = pool.committed_lsn;
		std::array<std::byte, RSS_PAGE_SIZE> page{};
		const auto envelopeBytes = SegmentEnvelopeCodec::encode(envelope);
		std::copy(envelopeBytes.begin(), envelopeBytes.end(), page.begin());
		SharedPagePoolHeader active{pool.segment_id, 0U, pool.generation, pool.committed_lsn, 1U};
		SharedPagePoolHeader inactive{pool.segment_id, 0U, pool.generation, pool.committed_lsn, 0U};
		const auto activeBytes = SharedPagePoolHeaderCodec::encode(active);
		const auto inactiveBytes = SharedPagePoolHeaderCodec::encode(inactive);
		std::copy(activeBytes.begin(), activeBytes.end(), page.begin() + RSS_ENVELOPE_BYTES);
		std::copy(inactiveBytes.begin(), inactiveBytes.end(),
				  page.begin() + RSS_ENVELOPE_BYTES + RSS_SHARED_PAGE_POOL_HEADER_BYTES);
		const uint64_t offset = PhysicalSegmentAllocator::checkedSlotOffset(dataRegionOffset, pool.physical_slot);
		io.writeAt({offset, pool.generation.value()}, page.data(), page.size());
	}

	[[maybe_unused]] void prepareTestSharedPageFrame(StorageIO &io, uint64_t dataRegionOffset, const CatalogEntry &pool,
									 PageId pageId, uint8_t frameIndex, uint64_t ownerId, PageOwnerKind ownerKind,
									 LogSequenceNumber ownerLsn) {
		if (!pageId.isValid() || frameIndex < RSS_PAGE_POOL_FIRST_DATA_FRAME ||
			frameIndex > RSS_PAGE_POOL_DATA_FRAME_COUNT || ownerId == 0U || ownerKind == PageOwnerKind::POK_NONE ||
			!ownerLsn.isValid()) {
			throw std::invalid_argument("test shared page frame has an invalid typed identity");
		}
		const uint64_t poolOffset = PhysicalSegmentAllocator::checkedSlotOffset(dataRegionOffset, pool.physical_slot);
		std::array<std::byte, RSS_PAGE_SIZE> control{};
		if (io.readAt({poolOffset, pool.generation.value()}, control.data(), control.size()) != control.size()) {
			throw std::runtime_error("test shared page-pool control page is truncated");
		}
		const SharedPagePoolHeader first = SharedPagePoolHeaderCodec::decode(
				std::span<const std::byte>(control).subspan(RSS_ENVELOPE_BYTES, RSS_SHARED_PAGE_POOL_HEADER_BYTES));
		const SharedPagePoolHeader second = SharedPagePoolHeaderCodec::decode(std::span<const std::byte>(control).subspan(
				RSS_ENVELOPE_BYTES + RSS_SHARED_PAGE_POOL_HEADER_BYTES, RSS_SHARED_PAGE_POOL_HEADER_BYTES));
		const bool firstIsActive = first.control_sequence >= second.control_sequence;
		SharedPagePoolHeader updated = firstIsActive ? first : second;
		if (updated.control_sequence == std::numeric_limits<uint64_t>::max()) {
			throw std::overflow_error("test shared page-pool control sequence overflow");
		}
		updated.allocated_frames |= 1U << (frameIndex - RSS_PAGE_POOL_FIRST_DATA_FRAME);
		++updated.control_sequence;
		const auto updatedBytes = SharedPagePoolHeaderCodec::encode(updated);
		const size_t inactiveOffset = firstIsActive ? RSS_ENVELOPE_BYTES + RSS_SHARED_PAGE_POOL_HEADER_BYTES
													 : RSS_ENVELOPE_BYTES;
		io.writeControlRecord({poolOffset, pool.generation.value()}, inactiveOffset, updatedBytes);

		PageOwnerHeader owner{pageId, ownerId, ownerKind, 0U, 0U, pool.generation, ownerLsn};
		std::array<std::byte, RSS_PAGE_SIZE> frame{};
		const auto ownerBytes = PageOwnerHeaderCodec::encode(owner);
		std::copy(ownerBytes.begin(), ownerBytes.end(), frame.begin());
		io.writeAt({poolOffset + static_cast<uint64_t>(frameIndex) * RSS_PAGE_SIZE, pool.generation.value()},
				   frame.data(), frame.size());
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
