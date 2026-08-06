/**
 * @file StateRecordStore.cpp
 * @brief Slotted-page State placement and State Directory publication.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/metadata/StateRecordStore.hpp"

#include <limits>
#include <optional>
#include <stdexcept>
#include <vector>

#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"

namespace graph::storage {

	namespace {

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		void validateOptions(const StateRecordStoreOptions &options) {
			const uint8_t invalid = static_cast<uint8_t>(!options.first_slot.isValid()) |
									static_cast<uint8_t>(!options.last_slot.isValid()) |
									static_cast<uint8_t>(options.first_slot > options.last_slot) |
									static_cast<uint8_t>(options.page_owner_id == 0U);
			if (invalid != 0U) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable State record store requires physical slots and a nonzero page owner");
			}
		}

		[[nodiscard]] LogSequenceNumber nextCheckpointLsn(RelocatableMetadataStore &metadata) {
			const MetadataSnapshotPin snapshot = metadata.openSnapshot();
			if (snapshot.superblock().checkpoint_lsn.value() == std::numeric_limits<uint64_t>::max()) {
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
					 "relocatable State record store cannot advance the checkpoint LSN");
			}
			return LogSequenceNumber(snapshot.superblock().checkpoint_lsn.value() + 1U);
		}

		[[nodiscard]] uint32_t minimumInsertBytes(size_t recordBytes) {
			const size_t maximum = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES - RSS_SLOTTED_PAGE_HEADER_BYTES -
								   RSS_SLOTTED_PAGE_SLOT_BYTES;
			if (recordBytes > maximum) {
				fail(FormatError::FERR_INVALID_STATE,
					 "relocatable State record exceeds one slotted-page record; use a LargeValue for its value");
			}
			return static_cast<uint32_t>(recordBytes + RSS_SLOTTED_PAGE_SLOT_BYTES);
		}

		[[nodiscard]] StateLocation toLocation(uint64_t stateId, const SlottedSlotReference &slot) {
			return {stateId, slot.page_id, slot.slot_id, slot.generation};
		}

	} // namespace

	StateRecordStore::StateRecordStore(SlottedPagePool &pages, RelocatableMetadataStore &metadata) :
		pages_(pages), metadata_(metadata) {
		if (!pages_.usesMetadataStore(metadata_)) {
			throw std::invalid_argument("relocatable State record store requires one metadata store");
		}
	}

	StateRecord StateRecordStore::read(const MetadataSnapshotPin &snapshot, uint64_t stateId) const {
		const StateLocation location = metadata_.findStateLocation(snapshot, stateId);
		const SlottedPage page = pages_.read(snapshot, location.page_id);
		const StateRecord record =
				StateRecordCodec::decode(page.read({location.page_id, location.slot_id, location.slot_generation}));
		if (record.state_id != stateId) {
			fail(FormatError::FERR_INVALID_STATE,
				 "relocatable State Directory location resolves to another State record");
		}
		return record;
	}

	StateLocation StateRecordStore::ensure(const StateRecord &record, const StateRecordStoreOptions &options) {
		validateOptions(options);
		const std::vector<std::byte> encoded = StateRecordCodec::encode(record);
		const uint32_t requiredBytes = minimumInsertBytes(encoded.size());
		std::vector<SegmentId> pools;
		{
			const MetadataSnapshotPin snapshot = metadata_.openSnapshot();
			if (metadata_.findStateLocationOptional(snapshot, record.state_id).has_value()) {
				const StateRecord existing = read(snapshot, record.state_id);
				if (existing != record) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable State record conflicts with an already published State ID");
				}
				return metadata_.findStateLocation(snapshot, record.state_id);
			}
			for (const CatalogEntry &pool: metadata_.findCurrentStatePagePools(snapshot, options.page_owner_id)) {
				pools.push_back(pool.segment_id);
			}
		}

		if (!pools.empty()) {
			const std::optional<SlottedPagePlacement> placement = pages_.findBestFitPage(pools, requiredBytes);
			if (placement.has_value()) {
				MetadataSnapshotPin snapshot = metadata_.openSnapshot();
				SlottedPage page = pages_.read(snapshot, placement->page_id);
				const SlottedSlotReference slot = page.insert(encoded);
				snapshot.release();
				static_cast<void>(pages_.replacePage(page, nextCheckpointLsn(metadata_)));
				const StateLocation location = toLocation(record.state_id, slot);
				static_cast<void>(
						metadata_.upsertStateLocations(std::span(&location, 1), {}, nextCheckpointLsn(metadata_)));
				return location;
			}
		}

		const PageId pageId = metadata_.allocateLogicalPageId(nextCheckpointLsn(metadata_));
		SegmentId segmentId;
		{
			const MetadataSnapshotPin snapshot = metadata_.openSnapshot();
			if (snapshot.superblock().next_logical_segment_id == 0U) {
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
					 "relocatable State record store cannot allocate another logical Segment ID");
			}
			segmentId = SegmentId(snapshot.superblock().next_logical_segment_id);
		}
		const LogSequenceNumber allocationLsn = nextCheckpointLsn(metadata_);
		SlottedPagePoolAllocationRequest request;
		request.segment_id = segmentId;
		request.pool_kind = SegmentKind::SGK_STATE_PAGE_POOL;
		request.pool_owner_id = options.page_owner_id;
		request.generation = Generation(1);
		request.page_id = pageId;
		request.page_owner_id = options.page_owner_id;
		request.page_owner_kind = PageOwnerKind::POK_STATE;
		PageOwnerHeader owner;
		owner.page_id = pageId;
		owner.owner_id = options.page_owner_id;
		owner.kind = PageOwnerKind::POK_STATE;
		owner.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
		owner.generation = request.generation;
		owner.committed_lsn = allocationLsn;
		SlottedPage page = SlottedPage::create(owner);
		const SlottedSlotReference slot = page.insert(encoded);
		const std::optional<SlottedPagePoolAllocation> allocation =
				pages_.allocatePage(request, page, options.first_slot, options.last_slot, allocationLsn);
		if (!allocation.has_value()) {
			fail(FormatError::FERR_INVALID_STATE,
				 "relocatable State record store has no free physical destination slot");
		}
		const StateLocation location = toLocation(record.state_id, slot);
		static_cast<void>(metadata_.upsertStateLocations(std::span(&location, 1), {}, nextCheckpointLsn(metadata_)));
		return location;
	}

} // namespace graph::storage
