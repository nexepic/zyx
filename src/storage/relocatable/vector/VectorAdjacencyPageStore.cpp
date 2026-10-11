/**
 * @file VectorAdjacencyPageStore.cpp
 * @brief Staged immutable adjacency-page materialization for relocatable vectors.
 */

#include "graph/storage/relocatable/vector/VectorAdjacencyPageStore.hpp"

#include <array>
#include <limits>
#include <optional>
#include <set>

#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"

namespace graph::storage {

	namespace {

		struct StagedAdjacencyRecord {
			uint64_t vector_id = 0;
			std::vector<std::byte> bytes;
		};

		struct AdjacencyPagePlan {
			size_t first_record = 0;
			size_t record_count = 0;
		};

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		void checkCancellation(const CancellationCheckpoint *cancellation) {
			if (cancellation != nullptr) {
				cancellation->check();
			}
		}

		void validateInput(uint64_t indexId, std::span<const VectorAdjacencyMaterializationRecord> records,
						   const VectorAdjacencyMaterializationOptions &options, LogSequenceNumber checkpointLsn) {
			const uint8_t invalid = static_cast<uint8_t>(indexId == 0U) |
									static_cast<uint8_t>((indexId & RSS_DERIVED_ADJACENCY_OWNER_MASK) != 0U) |
									static_cast<uint8_t>(records.empty()) |
									static_cast<uint8_t>(records.size() > options.maximum_records) |
									static_cast<uint8_t>(options.maximum_records == 0U) |
									static_cast<uint8_t>(options.maximum_pages == 0U) |
									static_cast<uint8_t>(!options.first_slot.isValid()) |
									static_cast<uint8_t>(!options.last_slot.isValid()) |
									static_cast<uint8_t>(options.first_slot > options.last_slot) |
									static_cast<uint8_t>(!checkpointLsn.isValid());
			if (invalid != 0U) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable adjacency materialization requires bounded records, pages, placement, and checkpoint LSN");
			}
			uint64_t previousVectorId = 0U;
			for (const VectorAdjacencyMaterializationRecord &record: records) {
				if (record.vector_id == 0U || record.vector_id <= previousVectorId) {
					fail(FormatError::FERR_INVALID_HEADER,
						 "relocatable adjacency materialization records require strictly increasing Vector IDs");
				}
				previousVectorId = record.vector_id;
			}
		}

		[[nodiscard]] std::vector<StagedAdjacencyRecord>
		encodeRecords(std::span<const VectorAdjacencyMaterializationRecord> records) {
			std::vector<StagedAdjacencyRecord> encoded;
			encoded.reserve(records.size());
			const size_t maximumRecordBytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES - RSS_SLOTTED_PAGE_HEADER_BYTES -
											  RSS_SLOTTED_PAGE_SLOT_BYTES;
			for (const VectorAdjacencyMaterializationRecord &record: records) {
				std::vector<std::byte> bytes = VectorAdjacencyCodec::encode({record.vector_id, record.neighbors});
				if (bytes.size() > maximumRecordBytes) {
					fail(FormatError::FERR_INVALID_HEADER,
						 "relocatable adjacency record cannot fit in one slotted adjacency page");
				}
				encoded.push_back({record.vector_id, std::move(bytes)});
			}
			return encoded;
		}

		[[nodiscard]] std::vector<AdjacencyPagePlan> planPages(std::span<const StagedAdjacencyRecord> records,
															   size_t maximumPages) {
			std::vector<AdjacencyPagePlan> result;
			const size_t pagePayloadBytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES - RSS_SLOTTED_PAGE_HEADER_BYTES;
			size_t currentFirst = 0U;
			size_t currentBytes = 0U;
			for (size_t ordinal = 0; ordinal < records.size(); ++ordinal) {
				const StagedAdjacencyRecord &record = records[ordinal];
				const size_t required = RSS_SLOTTED_PAGE_SLOT_BYTES + record.bytes.size();
				if (currentBytes != 0U && required > pagePayloadBytes - currentBytes) {
					result.push_back({currentFirst, ordinal - currentFirst});
					currentFirst = ordinal;
					currentBytes = 0U;
				}
				currentBytes += required;
			}
			result.push_back({currentFirst, records.size() - currentFirst});
			if (result.size() > maximumPages) {
				fail(FormatError::FERR_INVALID_STATE,
					 "relocatable adjacency materialization exceeded its configured adjacency page bound");
			}
			return result;
		}

	} // namespace

	VectorAdjacencyPageStore::VectorAdjacencyPageStore(std::shared_ptr<StorageIO> io, RelocatableMetadataStore &metadata) :
		VectorAdjacencyPageStore(std::move(io), metadata, SharedPagePoolOpenMode::SPPOM_RECONCILE) {}

	VectorAdjacencyPageStore::VectorAdjacencyPageStore(std::shared_ptr<StorageIO> io,
													 RelocatableMetadataStore &metadata,
													 SharedPagePoolOpenMode openMode) :
		allocator_(std::move(io), metadata, openMode), metadata_(metadata) {}

	uint64_t VectorAdjacencyPageStore::derivedOwnerId(uint64_t indexId) {
		if (indexId == 0U || (indexId & RSS_DERIVED_ADJACENCY_OWNER_MASK) != 0U) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "relocatable derived adjacency pages require a nonzero public Index ID without reserved bits");
		}
		return indexId | RSS_DERIVED_ADJACENCY_OWNER_MASK;
	}

	std::vector<VectorAdjacencyMaterializationPoolRequest> VectorAdjacencyPageStore::planNewPoolRequests(
			uint64_t indexId, std::span<const VectorAdjacencyMaterializationRecord> records,
			const VectorAdjacencyMaterializationOptions &options, LogSequenceNumber checkpointLsn) {
		validateInput(indexId, records, options, checkpointLsn);
		const std::vector<StagedAdjacencyRecord> encoded = encodeRecords(records);
		const std::vector<AdjacencyPagePlan> plans = planPages(encoded, options.maximum_pages);
		const size_t poolCount = (plans.size() - 1U) / RSS_PAGE_POOL_DATA_FRAME_COUNT + 1U;
		std::vector<VectorAdjacencyMaterializationPoolRequest> pools;
		pools.reserve(poolCount);
		for (size_t ordinal = 0U; ordinal < poolCount; ++ordinal) {
			const size_t firstPage = ordinal * RSS_PAGE_POOL_DATA_FRAME_COUNT;
			pools.push_back({options.first_slot, options.last_slot,
							 std::min(plans.size() - firstPage, static_cast<size_t>(RSS_PAGE_POOL_DATA_FRAME_COUNT))});
		}
		return pools;
	}

	VectorAdjacencyMaterializationResult VectorAdjacencyPageStore::materialize(
			uint64_t indexId, std::span<const VectorAdjacencyMaterializationRecord> records,
			const VectorAdjacencyMaterializationOptions &options, LogSequenceNumber checkpointLsn,
			const MetadataPublishOptions &publishOptions) {
		const std::vector<VectorAdjacencyMaterializationPoolRequest> pools =
				planNewPoolRequests(indexId, records, options, checkpointLsn);

		VectorAdjacencyMaterializationResult result;
		const VectorAdjacencyMultiPoolMaterializationPreparer preparer =
				[this, indexId, &records, &options, checkpointLsn,
				 &result](std::vector<VectorAdjacencyMaterializationPoolBatch> &batches) {
					result = prepareNewPoolPages(indexId, records, options, batches, checkpointLsn);
				};
		result.superblock = metadata_.materializeNewVectorAdjacencyPagesAcrossPools(pools, indexId, checkpointLsn,
															preparer, publishOptions);
		return result;
	}

	VectorAdjacencyMaterializationResult VectorAdjacencyPageStore::prepareNewPoolPages(
			uint64_t indexId, std::span<const VectorAdjacencyMaterializationRecord> records,
			const VectorAdjacencyMaterializationOptions &options,
			std::span<VectorAdjacencyMaterializationPoolBatch> batches, LogSequenceNumber checkpointLsn,
			const CancellationCheckpoint *cancellation) const {
		validateInput(indexId, records, options, checkpointLsn);
		checkCancellation(cancellation);
		const uint64_t ownerId = derivedOwnerId(indexId);
		const std::vector<StagedAdjacencyRecord> encoded = encodeRecords(records);
		const std::vector<AdjacencyPagePlan> plans = planPages(encoded, options.maximum_pages);
		const size_t expectedPoolCount = (plans.size() - 1U) / RSS_PAGE_POOL_DATA_FRAME_COUNT + 1U;
		if (batches.size() != expectedPoolCount) {
			fail(FormatError::FERR_INVALID_STATE,
				 "relocatable prepared adjacency pages have a different pool count than their bounded layout");
		}

		VectorAdjacencyMaterializationResult result;
		result.record_count = records.size();
		result.pages.resize(plans.size());
		result.mappings.reserve(records.size());
		size_t globalPageOrdinal = 0U;
		for (VectorAdjacencyMaterializationPoolBatch &batch: batches) {
			checkCancellation(cancellation);
			const size_t expectedPageCount =
					std::min(plans.size() - globalPageOrdinal, static_cast<size_t>(RSS_PAGE_POOL_DATA_FRAME_COUNT));
			const uint8_t invalidPool =
					static_cast<uint8_t>(batch.pool.kind != SegmentKind::SGK_VECTOR_ADJACENCY_POOL) |
					static_cast<uint8_t>(batch.pool.owner_id != indexId) |
					static_cast<uint8_t>(batch.pool.state != SegmentState::SGS_SEALED) |
					static_cast<uint8_t>(!batch.pool.generation.isValid()) |
					static_cast<uint8_t>(batch.page_ids.size() != expectedPageCount) |
					static_cast<uint8_t>(batch.locations.size() != expectedPageCount);
			if (invalidPool != 0U) {
				fail(FormatError::FERR_INVALID_STATE,
					 "relocatable prepared adjacency pages have an invalid metadata-assigned pool shape");
			}

			std::vector<SharedPagePoolPreparedAppend> prepared;
			prepared.reserve(expectedPageCount);
			for (size_t pageOrdinal = 0U; pageOrdinal < expectedPageCount; ++pageOrdinal, ++globalPageOrdinal) {
				checkCancellation(cancellation);
				const AdjacencyPagePlan &plan = plans[globalPageOrdinal];
				const PageOwnerHeader owner{batch.page_ids[pageOrdinal], ownerId, PageOwnerKind::POK_VECTOR_ADJACENCY,
														0U, RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, batch.pool.generation, checkpointLsn};
				SlottedPage page = SlottedPage::create(owner);
				for (size_t recordOrdinal = 0U; recordOrdinal < plan.record_count; ++recordOrdinal) {
					checkCancellation(cancellation);
					const StagedAdjacencyRecord &record = encoded[plan.first_record + recordOrdinal];
					static_cast<void>(page.insert(record.bytes));
					result.mappings.push_back({record.vector_id, owner.page_id});
				}
				const SlottedPageStats stats = page.stats();
				result.pages[globalPageOrdinal] = {owner.page_id, static_cast<uint32_t>(plan.record_count)};
				prepared.push_back({owner, page.serialize(), stats.contiguous_free_bytes});
			}
			allocator_.prepareNewPoolPages(batch.pool, prepared, batch.locations, checkpointLsn);
		}
		return result;
	}

	void VectorAdjacencyPageStore::prepareDerivedRetirements(
			std::span<const ExistingPoolPageRetirementBatch> batches, LogSequenceNumber checkpointLsn,
			const CancellationCheckpoint *cancellation) const {
		if (batches.empty() || !checkpointLsn.isValid()) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "relocatable prepared adjacency retirement requires batches and a checkpoint LSN");
		}
		std::set<uint64_t> pageIds;
		for (const ExistingPoolPageRetirementBatch &batch: batches) {
			checkCancellation(cancellation);
			const CatalogEntry &pool = batch.pool;
			const uint8_t invalidPool =
					static_cast<uint8_t>(pool.kind != SegmentKind::SGK_VECTOR_ADJACENCY_POOL) |
					static_cast<uint8_t>(pool.owner_id == 0U) |
					static_cast<uint8_t>((pool.owner_id & RSS_DERIVED_ADJACENCY_OWNER_MASK) != 0U) |
					static_cast<uint8_t>(!pool.segment_id.isValid()) |
					static_cast<uint8_t>(!pool.physical_slot.isValid()) |
					static_cast<uint8_t>(!pool.generation.isValid()) |
					static_cast<uint8_t>(pool.state != SegmentState::SGS_SEALED) |
					static_cast<uint8_t>(batch.retirements.empty());
			if (invalidPool != 0U) {
				fail(FormatError::FERR_INVALID_STATE,
					 "relocatable prepared adjacency retirement has an invalid metadata-owned page pool");
			}
			std::vector<SharedPagePoolPreparedRetirement> prepared;
			prepared.reserve(batch.retirements.size());
			for (const PageRetirement &retirement: batch.retirements) {
				checkCancellation(cancellation);
				const PageLocation &location = retirement.location;
				const uint8_t invalidRetirement =
						static_cast<uint8_t>(location.segment_id != pool.segment_id) |
						static_cast<uint8_t>(location.generation != pool.generation) |
						static_cast<uint8_t>(location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
						static_cast<uint8_t>(location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
						static_cast<uint8_t>(retirement.expected_owner_id != derivedOwnerId(pool.owner_id)) |
						static_cast<uint8_t>(retirement.expected_owner_kind != PageOwnerKind::POK_VECTOR_ADJACENCY) |
						static_cast<uint8_t>(!retirement.expected_owner_lsn.isValid());
				if (invalidRetirement != 0U || !pageIds.insert(location.page_id.value()).second) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable prepared adjacency retirement has an invalid or duplicate source frame");
				}
				prepared.push_back({location, retirement.expected_owner_id, retirement.expected_owner_kind,
									retirement.expected_owner_lsn});
			}
			const std::array<PageLocation, 0U> noReplacements{};
			const std::array<SharedPagePoolPreparedReplacement, 0U> noReplacementPayloads{};
			const std::array<SharedPagePoolPreparedAppend, 0U> noAppends{};
			std::array<PageLocation, 0U> noReplacementLocations{};
			std::array<PageLocation, 0U> noAppendLocations{};
			allocator_.prepareMutationPages(pool, noReplacements, noReplacementPayloads, noAppends,
											noReplacementLocations, noAppendLocations, checkpointLsn, prepared);
		}
	}

	SlottedPage VectorAdjacencyPageStore::readPage(const MetadataSnapshotPin &snapshot, PageId pageId) const {
		const SharedPagePoolContents contents = allocator_.read(snapshot, pageId);
		const uint8_t invalid =
				static_cast<uint8_t>(contents.pool.kind != SegmentKind::SGK_VECTOR_ADJACENCY_POOL) |
				static_cast<uint8_t>(contents.pool.owner_id == 0U) |
				static_cast<uint8_t>(contents.owner.kind != PageOwnerKind::POK_VECTOR_ADJACENCY) |
				static_cast<uint8_t>(contents.owner.owner_id != derivedOwnerId(contents.pool.owner_id)) |
				static_cast<uint8_t>(contents.owner.generation != contents.location.generation) |
				static_cast<uint8_t>(contents.owner.committed_lsn > snapshot.superblock().checkpoint_lsn);
		if (invalid != 0U) {
			fail(FormatError::FERR_INVALID_STATE,
				 "relocatable derived adjacency page disagrees with its shared pool or snapshot identity");
		}
		return SlottedPage::open(contents.owner, contents.payload);
	}

	std::optional<DerivedVectorAdjacencyPage>
	VectorAdjacencyPageStore::inspectDerivedPage(const MetadataSnapshotPin &snapshot, PageId pageId) const {
		const SharedPagePoolContents contents = allocator_.read(snapshot, pageId);
		const uint8_t invalidPool = static_cast<uint8_t>(contents.pool.kind != SegmentKind::SGK_VECTOR_ADJACENCY_POOL) |
										static_cast<uint8_t>(contents.owner.kind != PageOwnerKind::POK_VECTOR_ADJACENCY) |
										static_cast<uint8_t>(contents.owner.page_id != pageId) |
										static_cast<uint8_t>(contents.owner.generation != contents.location.generation) |
										static_cast<uint8_t>(contents.owner.flags != 0U) |
										static_cast<uint8_t>(!contents.owner.committed_lsn.isValid());
		if (invalidPool != 0U) {
			fail(FormatError::FERR_INVALID_STATE,
				 "relocatable adjacency page inspection found an invalid current pool or owner identity");
		}
		// A reclaimer must retain an unfamiliar namespace, even if a future owner
		// family happens to use the adjacency pool kind.
		if ((contents.pool.owner_id & RSS_DERIVED_ADJACENCY_OWNER_MASK) != 0U) {
			return std::nullopt;
		}
		if (contents.owner.owner_id == contents.pool.owner_id) {
			return std::nullopt;
		}
		if (contents.owner.owner_id != derivedOwnerId(contents.pool.owner_id)) {
			return std::nullopt;
		}
		static_cast<void>(SlottedPage::open(contents.owner, contents.payload));
		return DerivedVectorAdjacencyPage{contents.location, contents.pool.owner_id, contents.owner.committed_lsn};
	}

	VectorAdjacencyRecord VectorAdjacencyPageStore::readRecord(const MetadataSnapshotPin &snapshot, PageId pageId,
															   uint64_t vectorId) const {
		if (vectorId == 0U) {
			fail(FormatError::FERR_INVALID_HEADER, "relocatable derived adjacency lookup requires a nonzero Vector ID");
		}
		const SlottedPage page = readPage(snapshot, pageId);
		uint64_t previousVectorId = 0U;
		std::optional<VectorAdjacencyRecord> result;
		for (const SlottedSlotReference &reference: page.liveSlots()) {
			const VectorAdjacencyRecord record = VectorAdjacencyCodec::decode(page.read(reference));
			if (record.vector_id <= previousVectorId) {
				fail(FormatError::FERR_INVALID_STATE,
					 "relocatable derived adjacency page records are not in canonical Vector ID order");
			}
			if (record.vector_id == vectorId) {
				result = record;
			}
			previousVectorId = record.vector_id;
		}
		if (result.has_value()) {
			return *result;
		}
		fail(FormatError::FERR_INVALID_STATE,
			 "relocatable adjacency mapping resolves to a page without its requested Vector record");
	}

	Superblock VectorAdjacencyPageStore::retireDerivedPages(
			Generation expectedMetadataGeneration, std::span<const DerivedVectorAdjacencyPage> pages,
			LogSequenceNumber checkpointLsn, const MetadataPublishOptions &publishOptions) {
		if (pages.empty()) {
			fail(FormatError::FERR_INVALID_HEADER, "relocatable derived adjacency retirement requires at least one page");
		}
		std::vector<PageRetirement> retirements;
		retirements.reserve(pages.size());
		uint64_t previousPageId = 0U;
		for (const DerivedVectorAdjacencyPage &page: pages) {
			const uint8_t invalid = static_cast<uint8_t>(!page.location.page_id.isValid()) |
									static_cast<uint8_t>(page.location.page_id.value() <= previousPageId) |
									static_cast<uint8_t>(page.index_id == 0U) |
									static_cast<uint8_t>((page.index_id & RSS_DERIVED_ADJACENCY_OWNER_MASK) != 0U) |
									static_cast<uint8_t>(!page.owner_lsn.isValid());
			if (invalid != 0U) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable derived adjacency retirement requires canonical page and owner identities");
			}
			retirements.push_back(
					{page.location, derivedOwnerId(page.index_id), PageOwnerKind::POK_VECTOR_ADJACENCY, page.owner_lsn});
			previousPageId = page.location.page_id.value();
		}
		const ExistingPoolPagesRetirementPreparer preparer =
				[this, checkpointLsn](std::span<const ExistingPoolPageRetirementBatch> batches) {
					for (const ExistingPoolPageRetirementBatch &batch: batches) {
						std::vector<SharedPagePoolPreparedRetirement> prepared;
						prepared.reserve(batch.retirements.size());
						for (const PageRetirement &retirement: batch.retirements) {
							prepared.push_back({retirement.location, retirement.expected_owner_id,
														retirement.expected_owner_kind, retirement.expected_owner_lsn});
						}
						const std::array<PageLocation, 0> noReplacements{};
						const std::array<SharedPagePoolPreparedReplacement, 0> noReplacementPayloads{};
						const std::array<SharedPagePoolPreparedAppend, 0> noAppends{};
						std::array<PageLocation, 0> noReplacementLocations{};
						std::array<PageLocation, 0> noAppendLocations{};
						allocator_.prepareMutationPages(batch.pool, noReplacements, noReplacementPayloads, noAppends,
															  noReplacementLocations, noAppendLocations, checkpointLsn, prepared);
					}
				};
		return metadata_.retirePagesInExistingPools(expectedMetadataGeneration, {}, retirements, checkpointLsn, preparer,
																	publishOptions);
	}

	void VectorAdjacencyPageStore::reclaimRetiredFrames(SegmentId segmentId) const {
		allocator_.reclaimRetiredFrames(segmentId);
	}

} // namespace graph::storage
