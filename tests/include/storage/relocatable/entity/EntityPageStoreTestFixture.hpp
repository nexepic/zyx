/**
 * @file EntityPageStoreTestFixture.hpp
 * @brief Shared fixture and helpers for durable Entity Page Store tests.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/core/Node.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"
#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/entity/EntityLabelSet.hpp"
#include "graph/storage/relocatable/entity/EntityLabelSetStore.hpp"
#include "graph/storage/relocatable/entity/EntityPageStore.hpp"
#include "graph/storage/relocatable/property/LargeValueStore.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRootStore.hpp"
#include "storage/relocatable/property/PropertyCollectionRootTestUtils.hpp"

using namespace graph::storage;

namespace {

	void expectFormatError(const std::function<void()> &operation, FormatError expected) {
		try {
			operation();
			FAIL() << "Expected FormatException";
		} catch (const FormatException &error) {
			EXPECT_EQ(error.error(), expected);
		}
	}

	Superblock initialSuperblock() {
		Superblock result;
		result.database_id = DatabaseId(0x1020304050607080ULL);
		result.generation = Generation(1U);
		result.checkpoint_lsn = LogSequenceNumber(1U);
		return result;
	}

	EntityPageNewPoolRequest newPool(uint64_t segmentId, uint64_t ownerId, uint64_t pageId, uint64_t firstSlot) {
		return {.segment_id = SegmentId(segmentId),
				.pool_owner_id = ownerId,
				.generation = Generation(1U),
				.page_id = PageId(pageId),
				.first_slot = PhysicalSlotId(firstSlot),
				.last_slot = PhysicalSlotId(firstSlot)};
	}

	NodeEntityRecord node(uint64_t /*outgoingRootPage*/, uint64_t /*incomingRootPage*/, uint64_t firstLabel = 7U) {
		NodeEntityRecord result;
		result.inline_label_ids = {firstLabel, firstLabel + 2U, firstLabel + 4U};
		result.inline_label_count = 3U;
		return result;
	}

	EdgeEntityRecord edge(uint64_t source, uint64_t target, uint64_t typeId) {
		return {.source_node_id = source,
				.target_node_id = target,
				.type_id = typeId};
	}

	[[maybe_unused]] std::vector<uint64_t> orderedLabels(uint64_t first, size_t count) {
		std::vector<uint64_t> result;
		result.reserve(count);
		for (size_t index = 0U; index < count; ++index) {
			result.push_back(first + static_cast<uint64_t>(index) * 2U);
		}
		return result;
	}

	struct LabelPagePayload {
		std::vector<std::byte> payload;
		uint32_t free_bytes = 0U;
		EntityLabelOverflowReference overflow;
	};

	[[maybe_unused]] LabelPagePayload labelPagePayload(const CatalogEntry &pool, PageId pageId,
													 LogSequenceNumber checkpointLsn,
													 std::span<const uint64_t> overflowLabels) {
		const PageOwnerHeader owner{
				pageId,			 pool.owner_id, PageOwnerKind::POK_ENTITY_LABEL, 0U, RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
				pool.generation, checkpointLsn};
		SlottedPage page = SlottedPage::create(owner);
		const SlottedSlotReference slot = page.insert(EntityLabelSetCodec::encode(overflowLabels));
		return {page.serialize(), page.stats().contiguous_free_bytes, {slot.page_id, slot.slot_id, slot.generation}};
	}

	[[maybe_unused]] NodeEntityRecord nodeWithOverflow(std::span<const uint64_t> labels,
													 const EntityLabelOverflowReference &overflow) {
		if (labels.size() <= RSS_ENTITY_INLINE_LABEL_CAPACITY) {
			throw std::invalid_argument("test Node must contain a LabelSet overflow");
		}
		NodeEntityRecord result;
		result.inline_label_count = static_cast<uint8_t>(RSS_ENTITY_INLINE_LABEL_CAPACITY);
		for (size_t index = 0U; index < RSS_ENTITY_INLINE_LABEL_CAPACITY; ++index) {
			result.inline_label_ids[index] = labels[index];
		}
		result.label_overflow = overflow;
		return result;
	}

	[[maybe_unused]] void overwritePageOwner(const std::shared_ptr<StorageIO> &io,
												  RelocatableMetadataStore &metadata, PageId pageId,
												  const PageOwnerHeader &owner) {
		MetadataSnapshotPin snapshot = metadata.openSnapshot();
		const PageLocation location = metadata.findPageLocation(snapshot, pageId);
		const CatalogEntry pool = metadata.findCatalog(snapshot, location.segment_id);
		const uint64_t offset = PhysicalSegmentAllocator::checkedSlotOffset(snapshot.superblock().data_region_offset,
																			pool.physical_slot) +
								static_cast<uint64_t>(location.page_index) * RSS_PAGE_SIZE;
		snapshot.release();
		const auto bytes = PageOwnerHeaderCodec::encode(owner);
		io->writeAt(offset, bytes.data(), bytes.size());
	}

	[[maybe_unused]] void corruptPagePayloadByte(const std::shared_ptr<StorageIO> &io,
													RelocatableMetadataStore &metadata, PageId pageId,
													size_t payloadOffset = RSS_ENTITY_PAGE_HEADER_BYTES) {
		if (payloadOffset >= RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES) {
			throw std::invalid_argument("test page payload corruption offset is outside the page payload");
		}
		MetadataSnapshotPin snapshot = metadata.openSnapshot();
		const PageLocation location = metadata.findPageLocation(snapshot, pageId);
		const CatalogEntry pool = metadata.findCatalog(snapshot, location.segment_id);
		const uint64_t offset = PhysicalSegmentAllocator::checkedSlotOffset(snapshot.superblock().data_region_offset,
																							pool.physical_slot) +
								static_cast<uint64_t>(location.page_index) * RSS_PAGE_SIZE + RSS_PAGE_OWNER_HEADER_BYTES +
								payloadOffset;
		snapshot.release();
		std::byte original{};
		if (io->readAt(offset, &original, sizeof(original)) != sizeof(original)) {
			throw std::runtime_error("test page payload corruption could not read the persisted byte");
		}
		const std::byte corrupted = original ^ std::byte{0x01U};
		io->writeAt(offset, &corrupted, sizeof(corrupted));
	}

} // namespace

class RelocatableEntityPageStoreTest : public ::testing::Test {
protected:
	std::filesystem::path path;
	std::shared_ptr<std::fstream> file;
	std::shared_ptr<StorageIO> io;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		path = std::filesystem::temp_directory_path() / ("relocatable_entity_page_store_" + to_string(uuid));
		file = std::make_shared<std::fstream>(path, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
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
		std::filesystem::remove(path, error);
	}
};
