/**
 * @file StateRecordStore.cpp
 * @brief Slotted-page State placement and State Directory publication.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/metadata/StateRecordStore.hpp"
#include "graph/storage/relocatable/maintenance/LargeValueMaintenanceJournal.hpp"

#include <stdexcept>
	#include <array>
	#include <limits>
	#include <optional>
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

	StateLocation StateRecordStore::upsert(const StateRecord &record, const StateRecordStoreOptions &options,
            const MetadataPublishOptions &publishOptions) {
		validateOptions(options);
        if (record.key == RSS_NATIVE_LARGE_VALUE_MAINTENANCE_JOURNAL_KEY)
            fail(FormatError::FERR_UNSUPPORTED_VERSION, "native maintenance evidence requires its private coordinator");
		const std::vector<std::byte> encoded = StateRecordCodec::encode(record);
		const uint32_t requiredBytes = minimumInsertBytes(encoded.size());
		std::vector<SegmentId> pools;
		std::optional<StateLocation> existingLocation;
		{
			MetadataSnapshotPin snapshot = metadata_.openSnapshot();
			if (metadata_.findStateLocationOptional(snapshot, record.state_id).has_value()) {
				existingLocation = metadata_.findStateLocation(snapshot, record.state_id);
				const StateRecord existingRecord = read(snapshot, record.state_id);
				if (existingRecord == record) {
					return *existingLocation;
				}
				const SlottedPage page = pages_.read(snapshot, existingLocation->page_id);
				const std::vector<std::byte> previous =
						page.read({existingLocation->page_id, existingLocation->slot_id, existingLocation->slot_generation});
				if (encoded.size() > previous.size()) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable State record replacement exceeds its stable slot capacity");
				}
				const PageLocation expectedPage = metadata_.findPageLocation(snapshot, existingLocation->page_id);
				snapshot.release();
				const LogSequenceNumber checkpointLsn = nextCheckpointLsn(metadata_);
				const StateLocation existing = *existingLocation;
				static_cast<void>(metadata_.replaceStatePageAndUpsertLocation(
						expectedPage, existing, checkpointLsn,
						[this, &page, &encoded, checkpointLsn, existing](const CatalogEntry &pool, const PageLocation &expected,
																								 PageLocation &replacement, StateLocation &) {
							SlottedPage replacementPage = page.clone();
							replacementPage.overwrite(
									{expected.page_id, existing.slot_id, existing.slot_generation}, encoded);
							const LogSequenceNumber expectedOwnerLsn = replacementPage.ownerCommittedLsn();
							PageOwnerHeader owner;
							owner.page_id = replacementPage.pageId();
							owner.owner_id = replacementPage.ownerId();
							owner.kind = replacementPage.ownerKind();
							owner.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
							owner.generation = pool.generation;
							owner.committed_lsn = checkpointLsn;
							replacementPage.rebindOwner(owner);
							const std::array expectedPages{expected};
							const std::array replacements{SharedPagePoolPreparedReplacement{
									owner, expectedOwnerLsn, replacementPage.serialize(),
									replacementPage.stats().contiguous_free_bytes}};
							const std::array<SharedPagePoolPreparedAppend, 0U> noAppends{};
							std::array<PageLocation, 1U> replacementPages{replacement};
							std::array<PageLocation, 0U> noAppendPages{};
							pages_.prepareMutationPages(pool, expectedPages, replacements, noAppends, replacementPages,
									noAppendPages, checkpointLsn);
							replacement = replacementPages.front();
						}, publishOptions));
				pages_.invalidateFreeSpaceIndex(expectedPage.segment_id);
				return *existingLocation;
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
				const PageLocation expectedPage = metadata_.findPageLocation(snapshot, placement->page_id);
				const SlottedSlotReference slot = page.insert(encoded);
				snapshot.release();
				const StateLocation location = toLocation(record.state_id, slot);
				const LogSequenceNumber checkpointLsn = nextCheckpointLsn(metadata_);
				static_cast<void>(metadata_.replaceStatePageAndUpsertLocation(
						expectedPage, location, checkpointLsn,
						[this, &page, checkpointLsn](const CatalogEntry &pool, const PageLocation &expected,
																					 PageLocation &replacement, StateLocation &) {
							const LogSequenceNumber expectedOwnerLsn = page.ownerCommittedLsn();
							PageOwnerHeader owner;
							owner.page_id = page.pageId();
							owner.owner_id = page.ownerId();
							owner.kind = page.ownerKind();
							owner.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
							owner.generation = pool.generation;
							owner.committed_lsn = checkpointLsn;
							page.rebindOwner(owner);
							const std::array expectedPages{expected};
							const std::array replacements{SharedPagePoolPreparedReplacement{
									owner, expectedOwnerLsn, page.serialize(), page.stats().contiguous_free_bytes}};
							const std::array<SharedPagePoolPreparedAppend, 0U> noAppends{};
							std::array<PageLocation, 1U> replacementPages{replacement};
							std::array<PageLocation, 0U> noAppendPages{};
							pages_.prepareMutationPages(pool, expectedPages, replacements, noAppends, replacementPages,
																	 noAppendPages, checkpointLsn);
							replacement = replacementPages.front();
						}, publishOptions));
				pages_.invalidateFreeSpaceIndex(expectedPage.segment_id);
				return location;
			}
		}

		const LogSequenceNumber allocationLsn = nextCheckpointLsn(metadata_);
		const std::optional<StatePagePoolMaterialization> materialized = metadata_.materializeStatePagePool(
				options.page_owner_id, record.state_id, options.first_slot, options.last_slot, allocationLsn,
				[this, &encoded, allocationLsn](const CatalogEntry &pool, PageId pageId, PageLocation &pageLocation,
																				 StateLocation &stateLocation) {
					PageOwnerHeader owner;
					owner.page_id = pageId;
					owner.owner_id = pool.owner_id;
					owner.kind = PageOwnerKind::POK_STATE;
					owner.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
					owner.generation = pool.generation;
					owner.committed_lsn = allocationLsn;
					SlottedPage page = SlottedPage::create(owner);
					const SlottedSlotReference slot = page.insert(encoded);
					const std::array prepared{SharedPagePoolPreparedAppend{
							owner, page.serialize(), page.stats().contiguous_free_bytes}};
					pageLocation = {pageId, pool.segment_id, 0U, pool.generation,
										page.stats().contiguous_free_bytes};
					stateLocation = {stateLocation.state_id, pageId, slot.slot_id, slot.generation};
					pages_.prepareNewPoolPages(pool, prepared, std::span(&pageLocation, 1U), allocationLsn);
				}, publishOptions);
		if (!materialized.has_value()) {
			fail(FormatError::FERR_INVALID_STATE,
				 "relocatable State record store has no free physical destination slot");
		}
		pages_.invalidateFreeSpaceIndex(materialized->pool.segment_id);
		return materialized->state;
	}

} // namespace graph::storage
