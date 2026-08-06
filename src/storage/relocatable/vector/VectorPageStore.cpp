/**
 * @file VectorPageStore.cpp
 * @brief Durable raw/PQ VectorPage materialization through shared page pools.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/vector/VectorPageStore.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <set>

#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"

namespace graph::storage {

	namespace {

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		void validateRawMaterializationInput(uint64_t indexId, std::span<const RawVectorMaterializationRecord> records,
											 const VectorRawMaterializationOptions &options,
											 LogSequenceNumber checkpointLsn) {
			const uint8_t invalid = static_cast<uint8_t>(indexId == 0U) | static_cast<uint8_t>(records.empty()) |
									static_cast<uint8_t>(records.size() > options.maximum_records) |
									static_cast<uint8_t>(options.maximum_records == 0U) |
									static_cast<uint8_t>(options.maximum_pages == 0U) |
									static_cast<uint8_t>(!options.first_slot.isValid()) |
									static_cast<uint8_t>(!options.last_slot.isValid()) |
									static_cast<uint8_t>(options.first_slot > options.last_slot) |
									static_cast<uint8_t>(!checkpointLsn.isValid());
			if (invalid != 0U) {
				fail(FormatError::FERR_INVALID_HEADER, "relocatable raw Vector materialization requires bounded "
													   "records, pages, placement, and checkpoint LSN");
			}
			// ZYX_COV_EXCL_START: a test record over 4 GiB cannot be materialized within the bounded test environment.
			if (records.front().bytes.size() > std::numeric_limits<uint32_t>::max()) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable raw Vector materialization record stride exceeds the persisted width");
			}
			// ZYX_COV_EXCL_STOP
			const uint32_t recordBytes = static_cast<uint32_t>(records.front().bytes.size());
			if (records.front().bytes.size() < sizeof(uint64_t) ||
				VectorPageStore::rawSlotCapacity(recordBytes) == 0U) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable raw Vector materialization record cannot contain its stable Vector ID prefix");
			}
			uint64_t previousId = 0U;
			for (const RawVectorMaterializationRecord &record: records) {
				const uint8_t invalidRecord =
						static_cast<uint8_t>(record.vector_id == 0U) |
						static_cast<uint8_t>(record.bytes.size() != recordBytes) |
						static_cast<uint8_t>(record.bytes.size() < sizeof(uint64_t)) |
						static_cast<uint8_t>(record.vector_id <= previousId) |
						static_cast<uint8_t>(LittleEndianCodec::readU64(record.bytes, 0) != record.vector_id);
				if (invalidRecord != 0U) {
					fail(FormatError::FERR_INVALID_HEADER,
						 "relocatable raw Vector materialization records are not canonical "
						 "fixed-stride Vector ID-prefixed values");
				}
				previousId = record.vector_id;
			}
		}

		void validatePqMaterializationInput(uint64_t indexId, std::span<const PqVectorMaterializationRecord> records,
											const VectorPqMaterializationOptions &options,
											LogSequenceNumber checkpointLsn) {
			const uint8_t invalid = static_cast<uint8_t>(indexId == 0U) | static_cast<uint8_t>(records.empty()) |
									static_cast<uint8_t>(records.size() > options.maximum_records) |
									static_cast<uint8_t>(options.maximum_records == 0U) |
									static_cast<uint8_t>(options.maximum_pages == 0U) |
									static_cast<uint8_t>(!options.first_slot.isValid()) |
									static_cast<uint8_t>(!options.last_slot.isValid()) |
									static_cast<uint8_t>(options.first_slot > options.last_slot) |
									static_cast<uint8_t>(!options.codebook_generation.isValid()) |
									static_cast<uint8_t>(!checkpointLsn.isValid());
			if (invalid != 0U) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable PQ Vector materialization requires bounded records, "
					 "pages, placement, codebook, and checkpoint LSN");
			}
			if (records.front().codes.empty()) {
				fail(FormatError::FERR_INVALID_HEADER, "relocatable PQ Vector materialization code width is invalid or "
													   "exceeds the persisted record stride");
			}
			// ZYX_COV_EXCL_START: a single test code vector over the 32-bit durable width exceeds test memory limits.
			if (records.front().codes.size() > std::numeric_limits<uint32_t>::max() - sizeof(uint64_t)) {
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW, "relocatable PQ Vector materialization code width is "
															"invalid or exceeds the persisted record stride");
			}
			// ZYX_COV_EXCL_STOP
			const uint32_t recordBytes = static_cast<uint32_t>(sizeof(uint64_t) + records.front().codes.size());
			// rawSlotCapacity either returns a nonzero capacity or fails closed, so
			// callers cannot proceed with an unrepresentable page layout.
			static_cast<void>(VectorPageStore::rawSlotCapacity(recordBytes));
			uint64_t previousId = 0U;
			for (const PqVectorMaterializationRecord &record: records) {
				const uint8_t invalidRecord = static_cast<uint8_t>(record.vector_id == 0U) |
											  static_cast<uint8_t>(record.vector_id <= previousId) |
											  static_cast<uint8_t>(record.codes.size() != records.front().codes.size());
				if (invalidRecord != 0U) {
					fail(FormatError::FERR_INVALID_HEADER, "relocatable PQ Vector materialization records are not "
														   "canonical fixed-width Vector ID values");
				}
				previousId = record.vector_id;
			}
		}

		[[nodiscard]] std::vector<std::byte> pqRecord(const PqVectorMaterializationRecord &record) {
			// ZYX_COV_EXCL_START: this private encoder runs only after validatePqMaterializationInput validates every
			// record.
			if (record.vector_id == 0U || record.codes.empty() ||
				record.codes.size() > std::numeric_limits<size_t>::max() - sizeof(uint64_t)) {
				fail(record.codes.size() > std::numeric_limits<size_t>::max() - sizeof(uint64_t)
							 ? FormatError::FERR_ARITHMETIC_OVERFLOW
							 : FormatError::FERR_INVALID_HEADER,
					 "relocatable PQ Vector record has an invalid identity or code length");
			}
			// ZYX_COV_EXCL_STOP
			std::vector<std::byte> bytes(sizeof(uint64_t) + record.codes.size());
			LittleEndianCodec::writeU64(bytes, 0, record.vector_id);
			for (size_t index = 0; index < record.codes.size(); ++index) {
				bytes[sizeof(uint64_t) + index] = static_cast<std::byte>(record.codes[index]);
			}
			return bytes;
		}

	} // namespace

	VectorPageStore::VectorPageStore(std::shared_ptr<StorageIO> io, RelocatableMetadataStore &metadata) :
		VectorPageStore(std::move(io), metadata, SharedPagePoolOpenMode::SPPOM_RECONCILE) {}

	VectorPageStore::VectorPageStore(std::shared_ptr<StorageIO> io, RelocatableMetadataStore &metadata,
									 SharedPagePoolOpenMode openMode) :
		allocator_(std::move(io), metadata, openMode), metadata_(metadata) {}

	uint16_t VectorPageStore::rawSlotCapacity(uint32_t recordBytes) {
		if (recordBytes == 0U || recordBytes > RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "relocatable raw Vector materialization record stride exceeds one Vector page");
		}
		const uint64_t available = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES - RSS_VECTOR_PAGE_HEADER_BYTES;
		const uint64_t minimumStride = static_cast<uint64_t>(recordBytes) + sizeof(uint32_t);
		uint64_t capacity = std::min<uint64_t>(std::numeric_limits<uint16_t>::max(), available / minimumStride);
		while (capacity != 0U) {
			const uint64_t bitmapBytes = (capacity + 7U) / 8U;
			const uint64_t total =
					RSS_VECTOR_PAGE_HEADER_BYTES + bitmapBytes + capacity * sizeof(uint32_t) + capacity * recordBytes;
			if (total <= RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES) {
				return static_cast<uint16_t>(capacity);
			}
			--capacity;
		}
		fail(FormatError::FERR_INVALID_HEADER,
			 "relocatable raw Vector materialization record stride leaves no usable Vector slot");
	}

	std::optional<SharedPagePoolAllocation> VectorPageStore::allocate(SharedPagePoolAllocationRequest request,
																	  const VectorPage &page, PhysicalSlotId firstSlot,
																	  PhysicalSlotId lastSlot,
																	  LogSequenceNumber checkpointLsn,
																	  const MetadataPublishOptions &options) {
		const VectorPageHeader header = page.header();
		const uint8_t invalid =
				static_cast<uint8_t>(request.pool_kind != poolKind(header.kind)) |
				static_cast<uint8_t>(request.page_id != header.page_id) |
				static_cast<uint8_t>(request.page_owner_id != header.index_id) |
				static_cast<uint8_t>(request.page_owner_kind != (header.kind == VectorStorageKind::VSK_RAW
																		 ? PageOwnerKind::POK_VECTOR_RAW
																		 : PageOwnerKind::POK_VECTOR_PQ)) |
				static_cast<uint8_t>(request.generation != header.generation) |
				static_cast<uint8_t>(header.committed_lsn != checkpointLsn);
		if (invalid != 0U) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable vector page allocation does not match its durable raw/PQ identity");
		}
		const std::vector<std::byte> bytes = page.serialize();
		request.page_free_bytes = pageFreeBytes(page);
		request.payload = bytes;
		return allocator_.allocateNewPoolPage(request, firstSlot, lastSlot, checkpointLsn, options);
	}

	VectorRawMaterializationResult
	VectorPageStore::materializeRaw(uint64_t indexId, std::span<const RawVectorMaterializationRecord> records,
									const VectorRawMaterializationOptions &options, LogSequenceNumber checkpointLsn,
									const MetadataPublishOptions &publishOptions) {
		validateRawMaterializationInput(indexId, records, options, checkpointLsn);
		const uint32_t recordBytes = static_cast<uint32_t>(records.front().bytes.size());
		const uint16_t slotsPerPage = rawSlotCapacity(recordBytes);
		const size_t pageCount = (records.size() - 1U) / slotsPerPage + 1U;
		if (pageCount > options.maximum_pages) {
			fail(FormatError::FERR_INVALID_STATE,
				 "relocatable raw Vector materialization exceeded its configured Vector page bound");
		}
		const size_t poolCount = (pageCount - 1U) / RSS_PAGE_POOL_DATA_FRAME_COUNT + 1U;
		std::vector<VectorRawMaterializationPoolRequest> pools;
		pools.reserve(poolCount);
		for (size_t ordinal = 0; ordinal < poolCount; ++ordinal) {
			const size_t firstPage = ordinal * RSS_PAGE_POOL_DATA_FRAME_COUNT;
			pools.push_back({options.first_slot, options.last_slot,
							 std::min(pageCount - firstPage, static_cast<size_t>(RSS_PAGE_POOL_DATA_FRAME_COUNT))});
		}

		VectorRawMaterializationResult result;
		result.record_count = records.size();
		result.pages.resize(pageCount);
		const VectorRawMultiPoolMaterializationPreparer preparer =
				[this, indexId, records, recordBytes, slotsPerPage, checkpointLsn,
				 &result](std::vector<VectorRawMaterializationPoolBatch> &batches,
						  std::vector<VectorLocation> &locations) {
					size_t globalPageOrdinal = 0U;
					for (VectorRawMaterializationPoolBatch &batch: batches) {
						std::vector<SharedPagePoolPreparedAppend> prepared;
						prepared.reserve(batch.page_ids.size());
						for (size_t pageOrdinal = 0; pageOrdinal < batch.page_ids.size();
							 ++pageOrdinal, ++globalPageOrdinal) {
							const size_t firstRecord = globalPageOrdinal * slotsPerPage;
							const size_t recordCount = std::min<size_t>(slotsPerPage, records.size() - firstRecord);
							VectorPageHeader header;
							header.index_id = indexId;
							header.page_id = batch.page_ids[pageOrdinal];
							header.generation = batch.pool.generation;
							header.kind = VectorStorageKind::VSK_RAW;
							header.record_bytes = recordBytes;
							header.slot_capacity = slotsPerPage;
							header.bitmap_bytes =
									static_cast<uint16_t>((static_cast<uint32_t>(slotsPerPage) + 7U) / 8U);
							header.committed_lsn = checkpointLsn;
							const PageOwnerHeader owner{header.page_id,
														indexId,
														PageOwnerKind::POK_VECTOR_RAW,
														0,
														RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
														batch.pool.generation,
														checkpointLsn};
							VectorPage page = VectorPage::create(owner, header);
							for (size_t recordOrdinal = 0; recordOrdinal < recordCount; ++recordOrdinal) {
								const RawVectorMaterializationRecord &record = records[firstRecord + recordOrdinal];
								const uint16_t slot = page.insert(record.bytes);
								locations.push_back(
										{record.vector_id, header.page_id, slot, page.slotGeneration(slot)});
							}
							result.pages[globalPageOrdinal] = {header.page_id, static_cast<uint32_t>(recordCount)};
							prepared.push_back({owner, page.serialize(), pageFreeBytes(page)});
						}
						allocator_.prepareNewPoolPages(batch.pool, prepared, batch.locations, checkpointLsn);
					}
				};
		result.superblock = metadata_.materializeNewVectorRawPagesAcrossPools(pools, indexId, checkpointLsn, preparer,
																			  publishOptions);
		return result;
	}

	VectorPqMaterializationResult VectorPageStore::materializePq(uint64_t indexId,
																 std::span<const PqVectorMaterializationRecord> records,
																 const VectorPqMaterializationOptions &options,
																 LogSequenceNumber checkpointLsn,
																 const MetadataPublishOptions &publishOptions) {
		validatePqMaterializationInput(indexId, records, options, checkpointLsn);
		const uint32_t recordBytes = static_cast<uint32_t>(sizeof(uint64_t) + records.front().codes.size());
		const uint16_t slotsPerPage = rawSlotCapacity(recordBytes);
		const size_t pageCount = (records.size() - 1U) / slotsPerPage + 1U;
		if (pageCount > options.maximum_pages) {
			fail(FormatError::FERR_INVALID_STATE,
				 "relocatable PQ Vector materialization exceeded its configured Vector page bound");
		}
		const size_t poolCount = (pageCount - 1U) / RSS_PAGE_POOL_DATA_FRAME_COUNT + 1U;
		std::vector<VectorPqMaterializationPoolRequest> pools;
		pools.reserve(poolCount);
		for (size_t ordinal = 0; ordinal < poolCount; ++ordinal) {
			const size_t firstPage = ordinal * RSS_PAGE_POOL_DATA_FRAME_COUNT;
			pools.push_back({options.first_slot, options.last_slot,
							 std::min(pageCount - firstPage, static_cast<size_t>(RSS_PAGE_POOL_DATA_FRAME_COUNT))});
		}

		VectorPqMaterializationResult result;
		const VectorPqMultiPoolMaterializationPreparer preparer =
				[this, indexId, records, &options, checkpointLsn,
				 &result](std::vector<VectorPqMaterializationPoolBatch> &batches) {
					result = preparePqNewPoolPages(indexId, records, options, batches, checkpointLsn);
				};
		result.superblock = metadata_.materializeNewVectorPqPagesAcrossPools(pools, indexId, checkpointLsn, preparer,
																			 publishOptions);
		return result;
	}

	VectorPqMaterializationResult
	VectorPageStore::preparePqNewPoolPages(uint64_t indexId, std::span<const PqVectorMaterializationRecord> records,
										   const VectorPqMaterializationOptions &options,
										   std::span<VectorPqMaterializationPoolBatch> batches,
										   LogSequenceNumber checkpointLsn) const {
		validatePqMaterializationInput(indexId, records, options, checkpointLsn);
		const uint32_t recordBytes = static_cast<uint32_t>(sizeof(uint64_t) + records.front().codes.size());
		const uint16_t slotsPerPage = rawSlotCapacity(recordBytes);
		const size_t pageCount = (records.size() - 1U) / slotsPerPage + 1U;
		if (pageCount > options.maximum_pages) {
			fail(FormatError::FERR_INVALID_STATE,
				 "relocatable prepared PQ Vector pages exceed the configured Vector page bound");
		}
		const size_t expectedPoolCount = (pageCount - 1U) / RSS_PAGE_POOL_DATA_FRAME_COUNT + 1U;
		if (batches.size() != expectedPoolCount) {
			fail(FormatError::FERR_INVALID_STATE,
				 "relocatable prepared PQ Vector pages have a metadata batch with the wrong pool cardinality");
		}

		VectorPqMaterializationResult result;
		result.record_count = records.size();
		result.pages.resize(pageCount);
		result.mappings.reserve(records.size());
		std::set<uint64_t> pageIds;
		size_t globalPageOrdinal = 0U;
		for (size_t poolOrdinal = 0U; poolOrdinal < batches.size(); ++poolOrdinal) {
			VectorPqMaterializationPoolBatch &batch = batches[poolOrdinal];
			const size_t expectedPageCount =
					std::min(pageCount - globalPageOrdinal, static_cast<size_t>(RSS_PAGE_POOL_DATA_FRAME_COUNT));
			const uint8_t invalidPool = static_cast<uint8_t>(batch.pool.kind != SegmentKind::SGK_VECTOR_PQ_POOL) |
										static_cast<uint8_t>(batch.pool.owner_id != indexId) |
										static_cast<uint8_t>(batch.pool.generation != Generation(1U)) |
										static_cast<uint8_t>(batch.pool.state != SegmentState::SGS_SEALED) |
										static_cast<uint8_t>(!batch.pool.physical_slot.isValid()) |
										static_cast<uint8_t>(batch.pool.committed_lsn != checkpointLsn) |
										static_cast<uint8_t>(batch.page_ids.size() != expectedPageCount) |
										static_cast<uint8_t>(batch.locations.size() != expectedPageCount);
			if (invalidPool != 0U) {
				fail(FormatError::FERR_INVALID_STATE,
					 "relocatable prepared PQ Vector pages have an invalid metadata-owned pool or page shape");
			}

			std::vector<SharedPagePoolPreparedAppend> prepared;
			prepared.reserve(expectedPageCount);
			for (size_t pageOrdinal = 0U; pageOrdinal < expectedPageCount; ++pageOrdinal, ++globalPageOrdinal) {
				const PageId pageId = batch.page_ids[pageOrdinal];
				const PageLocation expectedLocation = batch.locations[pageOrdinal];
				const uint8_t invalidLocation =
						static_cast<uint8_t>(!pageId.isValid()) |
						static_cast<uint8_t>(expectedLocation.page_id != pageId) |
						static_cast<uint8_t>(expectedLocation.segment_id != batch.pool.segment_id) |
						static_cast<uint8_t>(expectedLocation.generation != batch.pool.generation) |
						static_cast<uint8_t>(expectedLocation.page_index != 0U) |
						static_cast<uint8_t>(expectedLocation.free_bytes != 0U) |
						static_cast<uint8_t>(!pageIds.insert(pageId.value()).second);
				if (invalidLocation != 0U) {
					fail(FormatError::FERR_INVALID_STATE, "relocatable prepared PQ Vector pages have an invalid "
														  "metadata-assigned Page Directory location");
				}
				const size_t firstRecord = globalPageOrdinal * slotsPerPage;
				const size_t recordCount = std::min<size_t>(slotsPerPage, records.size() - firstRecord);
				VectorPageHeader header;
				header.index_id = indexId;
				header.page_id = pageId;
				header.generation = batch.pool.generation;
				header.kind = VectorStorageKind::VSK_PQ;
				header.record_bytes = recordBytes;
				header.slot_capacity = slotsPerPage;
				header.bitmap_bytes = static_cast<uint16_t>((static_cast<uint32_t>(slotsPerPage) + 7U) / 8U);
				header.committed_lsn = checkpointLsn;
				header.codebook_generation = options.codebook_generation;
				const PageOwnerHeader owner{header.page_id,
											indexId,
											PageOwnerKind::POK_VECTOR_PQ,
											0U,
											RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
											batch.pool.generation,
											checkpointLsn};
				VectorPage page = VectorPage::create(owner, header);
				for (size_t recordOrdinal = 0U; recordOrdinal < recordCount; ++recordOrdinal) {
					const PqVectorMaterializationRecord &record = records[firstRecord + recordOrdinal];
					static_cast<void>(page.insert(pqRecord(record)));
					result.mappings.push_back({record.vector_id, header.page_id});
				}
				result.pages[globalPageOrdinal] = {header.page_id, static_cast<uint32_t>(recordCount)};
				prepared.push_back({owner, page.serialize(), pageFreeBytes(page)});
			}
			allocator_.prepareNewPoolPages(batch.pool, prepared, batch.locations, checkpointLsn);
		}
		return result;
	}

	void VectorPageStore::prepareDerivedPqRetirements(std::span<const ExistingPoolPageRetirementBatch> batches,
													  LogSequenceNumber checkpointLsn) const {
		if (batches.empty() || !checkpointLsn.isValid()) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "relocatable prepared PQ retirement requires batches and a checkpoint LSN");
		}
		std::set<uint64_t> pageIds;
		for (const ExistingPoolPageRetirementBatch &batch: batches) {
			const CatalogEntry &pool = batch.pool;
			const uint8_t invalidPool = static_cast<uint8_t>(pool.kind != SegmentKind::SGK_VECTOR_PQ_POOL) |
										static_cast<uint8_t>(pool.owner_id == 0U) |
										static_cast<uint8_t>((pool.owner_id & (UINT64_C(1) << 63U)) != 0U) |
										static_cast<uint8_t>(!pool.segment_id.isValid()) |
										static_cast<uint8_t>(!pool.physical_slot.isValid()) |
										static_cast<uint8_t>(!pool.generation.isValid()) |
										static_cast<uint8_t>(pool.state != SegmentState::SGS_SEALED) |
										static_cast<uint8_t>(batch.retirements.empty());
			if (invalidPool != 0U) {
				fail(FormatError::FERR_INVALID_STATE,
					 "relocatable prepared PQ retirement has an invalid metadata-owned page pool");
			}
			std::vector<SharedPagePoolPreparedRetirement> prepared;
			prepared.reserve(batch.retirements.size());
			for (const PageRetirement &retirement: batch.retirements) {
				const PageLocation &location = retirement.location;
				const uint8_t invalidRetirement =
						static_cast<uint8_t>(location.segment_id != pool.segment_id) |
						static_cast<uint8_t>(location.generation != pool.generation) |
						static_cast<uint8_t>(location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
						static_cast<uint8_t>(location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
						static_cast<uint8_t>(retirement.expected_owner_id != pool.owner_id) |
						static_cast<uint8_t>(retirement.expected_owner_kind != PageOwnerKind::POK_VECTOR_PQ) |
						static_cast<uint8_t>(!retirement.expected_owner_lsn.isValid());
				if (invalidRetirement != 0U || !pageIds.insert(location.page_id.value()).second) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable prepared PQ retirement has an invalid or duplicate source frame");
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

	SharedPagePoolAllocation VectorPageStore::append(SharedPagePoolAppendRequest request, const VectorPage &page,
													 LogSequenceNumber checkpointLsn,
													 const MetadataPublishOptions &options) {
		const VectorPageHeader header = page.header();
		const uint8_t invalid = static_cast<uint8_t>(request.page_id != header.page_id) |
								static_cast<uint8_t>(request.page_owner_id != header.index_id) |
								static_cast<uint8_t>(request.generation != header.generation) |
								static_cast<uint8_t>(header.committed_lsn != checkpointLsn);
		if (invalid != 0U) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable appended vector page does not match its durable identity");
		}
		request.page_owner_kind = header.kind == VectorStorageKind::VSK_RAW ? PageOwnerKind::POK_VECTOR_RAW
																			: PageOwnerKind::POK_VECTOR_PQ;
		request.page_free_bytes = pageFreeBytes(page);
		const std::vector<std::byte> bytes = page.serialize();
		request.payload = bytes;
		return allocator_.appendPage(request, checkpointLsn, options);
	}

	SharedPagePoolAllocation VectorPageStore::replace(const VectorPage &page, SegmentId segmentId,
													  LogSequenceNumber expectedOwnerLsn,
													  LogSequenceNumber checkpointLsn,
													  const MetadataPublishOptions &options) {
		const VectorPageHeader header = page.header();
		SharedPagePoolReplacementRequest request;
		request.segment_id = segmentId;
		request.generation = header.generation;
		request.page_id = header.page_id;
		request.expected_owner_lsn = expectedOwnerLsn;
		request.page_owner_id = header.index_id;
		request.page_owner_kind = header.kind == VectorStorageKind::VSK_RAW ? PageOwnerKind::POK_VECTOR_RAW
																			: PageOwnerKind::POK_VECTOR_PQ;
		request.page_free_bytes = pageFreeBytes(page);
		const std::vector<std::byte> bytes = page.serialize();
		request.payload = bytes;
		return allocator_.replacePage(request, checkpointLsn, options);
	}

	bool VectorPageStore::hasReplacementFrameCapacity(const CatalogEntry &pool, size_t requiredFrames) const {
		return pool.kind == SegmentKind::SGK_VECTOR_RAW_POOL && pool.state == SegmentState::SGS_SEALED &&
			   requiredFrames != 0U && requiredFrames <= allocator_.availableFrameCount(pool);
	}

	void VectorPageStore::prepareRawReplacement(const CatalogEntry &pool, const PageLocation &expected,
												LogSequenceNumber expectedOwnerLsn, const VectorPage &page,
												LogSequenceNumber checkpointLsn, PageLocation &replacement) const {
		const VectorPageHeader header = page.header();
		const std::vector<std::byte> payload = page.serialize();
		const uint8_t invalid = static_cast<uint8_t>(pool.kind != SegmentKind::SGK_VECTOR_RAW_POOL) |
								static_cast<uint8_t>(pool.state != SegmentState::SGS_SEALED) |
								static_cast<uint8_t>(expected.segment_id != pool.segment_id) |
								static_cast<uint8_t>(expected.generation != pool.generation) |
								static_cast<uint8_t>(header.kind != VectorStorageKind::VSK_RAW) |
								static_cast<uint8_t>(header.index_id != pool.owner_id) |
								static_cast<uint8_t>(header.page_id != expected.page_id) |
								static_cast<uint8_t>(header.generation != pool.generation) |
								static_cast<uint8_t>(header.committed_lsn != checkpointLsn) |
								static_cast<uint8_t>(!expectedOwnerLsn.isValid()) |
								static_cast<uint8_t>(!checkpointLsn.isValid());
		if (invalid != 0U) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable raw Vector COW preparation has an invalid pool, source, or page identity");
		}
		const PageOwnerHeader owner{header.page_id,
									header.index_id,
									PageOwnerKind::POK_VECTOR_RAW,
									0U,
									static_cast<uint32_t>(payload.size()),
									pool.generation,
									checkpointLsn};
		const SharedPagePoolPreparedReplacement prepared{owner, expectedOwnerLsn, payload, pageFreeBytes(page)};
		const std::array<PageLocation, 1> sources{{expected}};
		const std::array<SharedPagePoolPreparedReplacement, 1> replacements{{prepared}};
		allocator_.prepareMutationPages(pool, sources, replacements, {}, std::span(&replacement, 1), {}, checkpointLsn);
	}

	void VectorPageStore::prepareRawAppend(const CatalogEntry &pool, const VectorPage &page,
										   LogSequenceNumber checkpointLsn, PageLocation &appended) const {
		const VectorPageHeader header = page.header();
		const std::vector<std::byte> payload = page.serialize();
		const uint8_t invalid = static_cast<uint8_t>(pool.kind != SegmentKind::SGK_VECTOR_RAW_POOL) |
								static_cast<uint8_t>(pool.state != SegmentState::SGS_SEALED) |
								static_cast<uint8_t>(!appended.page_id.isValid()) |
								static_cast<uint8_t>(appended.segment_id != pool.segment_id) |
								static_cast<uint8_t>(appended.generation != pool.generation) |
								static_cast<uint8_t>(appended.page_index != 0U) |
								static_cast<uint8_t>(appended.free_bytes != 0U) |
								static_cast<uint8_t>(header.kind != VectorStorageKind::VSK_RAW) |
								static_cast<uint8_t>(header.index_id != pool.owner_id) |
								static_cast<uint8_t>(header.page_id != appended.page_id) |
								static_cast<uint8_t>(header.generation != pool.generation) |
								static_cast<uint8_t>(header.committed_lsn != checkpointLsn) |
								static_cast<uint8_t>(!checkpointLsn.isValid());
		if (invalid != 0U) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable raw Vector append preparation has an invalid pool, page identity, or append location");
		}
		const PageOwnerHeader owner{header.page_id,
									header.index_id,
									PageOwnerKind::POK_VECTOR_RAW,
									0U,
									static_cast<uint32_t>(payload.size()),
									pool.generation,
									checkpointLsn};
		const SharedPagePoolPreparedAppend prepared{owner, payload, pageFreeBytes(page)};
		const std::array<SharedPagePoolPreparedAppend, 1> appends{{prepared}};
		allocator_.prepareMutationPages(pool, {}, {}, appends, {}, std::span(&appended, 1), checkpointLsn);
	}

	void VectorPageStore::prepareRawNewPool(const CatalogEntry &pool, const VectorPage &page,
											LogSequenceNumber checkpointLsn, PageLocation &initial) const {
		const VectorPageHeader header = page.header();
		const std::vector<std::byte> payload = page.serialize();
		const uint8_t invalid =
				static_cast<uint8_t>(pool.kind != SegmentKind::SGK_VECTOR_RAW_POOL) |
				static_cast<uint8_t>(pool.owner_id == 0U) | static_cast<uint8_t>(!pool.segment_id.isValid()) |
				static_cast<uint8_t>(!pool.physical_slot.isValid()) |
				static_cast<uint8_t>(pool.generation != Generation(1U)) |
				static_cast<uint8_t>(pool.state != SegmentState::SGS_SEALED) |
				static_cast<uint8_t>(pool.committed_lsn != checkpointLsn) |
				static_cast<uint8_t>(!initial.page_id.isValid()) |
				static_cast<uint8_t>(initial.segment_id != pool.segment_id) |
				static_cast<uint8_t>(initial.generation != pool.generation) |
				static_cast<uint8_t>(initial.page_index != 0U) | static_cast<uint8_t>(initial.free_bytes != 0U) |
				static_cast<uint8_t>(header.kind != VectorStorageKind::VSK_RAW) |
				static_cast<uint8_t>(header.index_id != pool.owner_id) |
				static_cast<uint8_t>(header.page_id != initial.page_id) |
				static_cast<uint8_t>(header.generation != pool.generation) |
				static_cast<uint8_t>(header.committed_lsn != checkpointLsn) |
				static_cast<uint8_t>(!checkpointLsn.isValid());
		if (invalid != 0U) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable raw Vector new-pool preparation has an invalid pool or initial page identity");
		}
		const PageOwnerHeader owner{header.page_id,
									header.index_id,
									PageOwnerKind::POK_VECTOR_RAW,
									0U,
									static_cast<uint32_t>(payload.size()),
									pool.generation,
									checkpointLsn};
		const SharedPagePoolPreparedAppend prepared{owner, payload, pageFreeBytes(page)};
		const std::array<SharedPagePoolPreparedAppend, 1> pages{{prepared}};
		allocator_.prepareNewPoolPages(pool, pages, std::span(&initial, 1), checkpointLsn);
	}

	void VectorPageStore::prepareRawNewPoolPages(const CatalogEntry &pool, std::span<const VectorPage> pages,
												 std::span<PageLocation> locations,
												 LogSequenceNumber checkpointLsn) const {
		const uint8_t invalidPool =
				static_cast<uint8_t>(pool.kind != SegmentKind::SGK_VECTOR_RAW_POOL) |
				static_cast<uint8_t>(pool.owner_id == 0U) | static_cast<uint8_t>(!pool.segment_id.isValid()) |
				static_cast<uint8_t>(!pool.physical_slot.isValid()) |
				static_cast<uint8_t>(pool.generation != Generation(1U)) |
				static_cast<uint8_t>(pool.state != SegmentState::SGS_SEALED) |
				static_cast<uint8_t>(pool.committed_lsn != checkpointLsn) | static_cast<uint8_t>(pages.empty()) |
				static_cast<uint8_t>(pages.size() != locations.size()) |
				static_cast<uint8_t>(pages.size() > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
				static_cast<uint8_t>(!checkpointLsn.isValid());
		if (invalidPool != 0U) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable Raw Vector multi-page preparation has an invalid pool or page batch");
		}
		std::set<PageId> pageIds;
		std::vector<SharedPagePoolPreparedAppend> prepared;
		prepared.reserve(pages.size());
		for (size_t ordinal = 0U; ordinal < pages.size(); ++ordinal) {
			const VectorPageHeader header = pages[ordinal].header();
			const PageLocation &location = locations[ordinal];
			const std::vector<std::byte> payload = pages[ordinal].serialize();
			const uint8_t invalidPage = static_cast<uint8_t>(!location.page_id.isValid()) |
										static_cast<uint8_t>(location.segment_id != pool.segment_id) |
										static_cast<uint8_t>(location.generation != pool.generation) |
										static_cast<uint8_t>(location.page_index != 0U) |
										static_cast<uint8_t>(location.free_bytes != 0U) |
										static_cast<uint8_t>(!pageIds.insert(location.page_id).second) |
										static_cast<uint8_t>(header.kind != VectorStorageKind::VSK_RAW) |
										static_cast<uint8_t>(header.index_id != pool.owner_id) |
										static_cast<uint8_t>(header.page_id != location.page_id) |
										static_cast<uint8_t>(header.generation != pool.generation) |
										static_cast<uint8_t>(header.committed_lsn != checkpointLsn);
			if (invalidPage != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable Raw Vector multi-page preparation has an invalid page identity");
			}
			const PageOwnerHeader owner{header.page_id,
										header.index_id,
										PageOwnerKind::POK_VECTOR_RAW,
										0U,
										static_cast<uint32_t>(payload.size()),
										pool.generation,
										checkpointLsn};
			prepared.push_back({owner, payload, pageFreeBytes(pages[ordinal])});
		}
		allocator_.prepareNewPoolPages(pool, prepared, locations, checkpointLsn);
	}

	VectorPage VectorPageStore::read(const MetadataSnapshotPin &snapshot, PageId pageId, BufferPoolReadHint hint) const {
		const SharedPagePoolContents contents = allocator_.read(snapshot, pageId, hint);
		return VectorPage::open(contents.owner, contents.payload);
	}

	std::optional<DerivedVectorPqPage> VectorPageStore::inspectDerivedPqPage(const MetadataSnapshotPin &snapshot,
																			 PageId pageId) const {
		const SharedPagePoolContents contents = allocator_.read(snapshot, pageId);
		const uint8_t invalid = static_cast<uint8_t>(contents.pool.kind != SegmentKind::SGK_VECTOR_PQ_POOL) |
								static_cast<uint8_t>(contents.owner.kind != PageOwnerKind::POK_VECTOR_PQ) |
								static_cast<uint8_t>(contents.owner.page_id != pageId) |
								static_cast<uint8_t>(contents.owner.generation != contents.location.generation) |
								static_cast<uint8_t>(contents.owner.flags != 0U) |
								static_cast<uint8_t>(!contents.owner.committed_lsn.isValid());
		if (invalid != 0U) {
			fail(FormatError::FERR_INVALID_STATE,
				 "relocatable PQ page inspection found an invalid current pool or owner identity");
		}
		if (contents.pool.owner_id == 0U || (contents.pool.owner_id & (UINT64_C(1) << 63U)) != 0U ||
			contents.owner.owner_id != contents.pool.owner_id) {
			return std::nullopt;
		}
		// `VectorPage::open` repeats the owner-to-payload identity validation before
		// the page can be classified as derived.
		static_cast<void>(VectorPage::open(contents.owner, contents.payload));
		return DerivedVectorPqPage{contents.location, contents.pool.owner_id, contents.owner.committed_lsn};
	}

	Superblock VectorPageStore::retireDerivedPqPages(Generation expectedMetadataGeneration,
													 std::span<const DerivedVectorPqPage> pages,
													 LogSequenceNumber checkpointLsn,
													 const MetadataPublishOptions &publishOptions) {
		if (pages.empty()) {
			fail(FormatError::FERR_INVALID_HEADER, "relocatable PQ retirement requires at least one page");
		}
		std::vector<PageRetirement> retirements;
		retirements.reserve(pages.size());
		uint64_t previousPageId = 0U;
		for (const DerivedVectorPqPage &page: pages) {
			const uint8_t invalid = static_cast<uint8_t>(!page.location.page_id.isValid()) |
									static_cast<uint8_t>(page.location.page_id.value() <= previousPageId) |
									static_cast<uint8_t>(page.index_id == 0U) |
									static_cast<uint8_t>((page.index_id & (UINT64_C(1) << 63U)) != 0U) |
									static_cast<uint8_t>(!page.owner_lsn.isValid());
			if (invalid != 0U) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable PQ retirement requires canonical page and public Index identities");
			}
			retirements.push_back({page.location, page.index_id, PageOwnerKind::POK_VECTOR_PQ, page.owner_lsn});
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
														noReplacementLocations, noAppendLocations, checkpointLsn,
														prepared);
					}
				};
		return metadata_.retirePagesInExistingPools(expectedMetadataGeneration, {}, retirements, checkpointLsn,
													preparer, publishOptions);
	}

	Superblock VectorPageStore::retireRawPages(Generation expectedMetadataGeneration,
											   std::span<const RetiredRawVectorPage> pages,
											   std::span<const uint64_t> erasedVectorIds,
											   LogSequenceNumber checkpointLsn,
											   const MetadataPublishOptions &publishOptions) {
		if (pages.empty()) {
			fail(FormatError::FERR_INVALID_HEADER, "relocatable raw Vector retirement requires at least one page");
		}
		std::vector<PageRetirement> retirements;
		retirements.reserve(pages.size());
		uint64_t previousPageId = 0U;
		for (const RetiredRawVectorPage &page: pages) {
			const uint8_t invalid = static_cast<uint8_t>(!page.location.page_id.isValid()) |
									static_cast<uint8_t>(page.location.page_id.value() <= previousPageId) |
									static_cast<uint8_t>(page.index_id == 0U) |
									static_cast<uint8_t>((page.index_id & (UINT64_C(1) << 63U)) != 0U) |
									static_cast<uint8_t>(!page.owner_lsn.isValid());
			if (invalid != 0U) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable raw Vector retirement requires canonical page and public Index identities");
			}
			retirements.push_back({page.location, page.index_id, PageOwnerKind::POK_VECTOR_RAW, page.owner_lsn});
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
														noReplacementLocations, noAppendLocations, checkpointLsn,
														prepared);
					}
				};
		return metadata_.retirePagesInExistingPools(expectedMetadataGeneration, {}, retirements, checkpointLsn,
													preparer, publishOptions, erasedVectorIds);
	}

	Superblock VectorPageStore::retireEmptyRawPool(SegmentId segmentId, Generation expectedGeneration,
												   LogSequenceNumber checkpointLsn,
												   const MetadataPublishOptions &publishOptions) {
		MetadataSnapshotPin snapshot = metadata_.openSnapshot();
		try {
			const CatalogEntry pool = metadata_.findCatalog(snapshot, segmentId);
			const uint8_t invalid = static_cast<uint8_t>(pool.kind != SegmentKind::SGK_VECTOR_RAW_POOL) |
									static_cast<uint8_t>(pool.generation != expectedGeneration) |
									static_cast<uint8_t>(pool.state != SegmentState::SGS_SEALED) |
									static_cast<uint8_t>(!checkpointLsn.isValid());
			snapshot.release();
			if (invalid != 0U) {
				fail(FormatError::FERR_INVALID_STATE,
					 "relocatable Raw Vector pool retirement requires one current sealed Raw pool generation");
			}
			return allocator_.retireEmptyPool(segmentId, expectedGeneration, checkpointLsn, publishOptions);
		} catch (...) {
			snapshot.release();
			throw;
		}
	}

	Superblock VectorPageStore::releaseRetiredRawPool(SegmentId segmentId, Generation expectedGeneration,
													  LogSequenceNumber checkpointLsn,
													  const MetadataPublishOptions &publishOptions) {
		MetadataSnapshotPin snapshot = metadata_.openSnapshot();
		try {
			const CatalogEntry pool = metadata_.findCatalog(snapshot, segmentId);
			const uint8_t invalid = static_cast<uint8_t>(pool.kind != SegmentKind::SGK_VECTOR_RAW_POOL) |
									static_cast<uint8_t>(pool.generation != expectedGeneration) |
									static_cast<uint8_t>(pool.state != SegmentState::SGS_RETIRED) |
									static_cast<uint8_t>(!checkpointLsn.isValid());
			snapshot.release();
			if (invalid != 0U) {
				fail(FormatError::FERR_INVALID_STATE,
					 "relocatable Raw Vector pool release requires one retired Raw pool generation");
			}
			return allocator_.releaseRetiredEmptyPool(segmentId, expectedGeneration, checkpointLsn, publishOptions);
		} catch (...) {
			snapshot.release();
			throw;
		}
	}

	void VectorPageStore::reclaimRetiredFrames(SegmentId segmentId) const {
		allocator_.reclaimRetiredFrames(segmentId);
	}

	SharedPagePoolAllocation VectorPageStore::evacuate(const VectorPage &page, SegmentId destinationSegmentId,
													   LogSequenceNumber checkpointLsn,
													   const MetadataPublishOptions &options) {
		const VectorPageHeader header = page.header();
		if (!destinationSegmentId.isValid() || !checkpointLsn.isValid()) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable vector page evacuation requires a destination pool and checkpoint LSN");
		}
		MetadataSnapshotPin snapshot = metadata_.openSnapshot();
		try {
			const SharedPagePoolContents source = allocator_.read(snapshot, header.page_id);
			const CatalogEntry target = metadata_.findCatalog(snapshot, destinationSegmentId);
			const PageOwnerKind expectedOwnerKind = header.kind == VectorStorageKind::VSK_RAW
															? PageOwnerKind::POK_VECTOR_RAW
															: PageOwnerKind::POK_VECTOR_PQ;
			const uint8_t invalidSource = static_cast<uint8_t>(source.pool.kind != poolKind(header.kind)) |
										  static_cast<uint8_t>(source.owner.page_id != header.page_id) |
										  static_cast<uint8_t>(source.owner.owner_id != header.index_id) |
										  static_cast<uint8_t>(source.owner.kind != expectedOwnerKind) |
										  static_cast<uint8_t>(source.owner.generation != header.generation) |
										  static_cast<uint8_t>(source.owner.committed_lsn != header.committed_lsn) |
										  static_cast<uint8_t>(target.kind != poolKind(header.kind));
			if (invalidSource != 0U) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable vector page evacuation requires a current compatible typed page and pool");
			}
			PageOwnerHeader destinationOwner = source.owner;
			destinationOwner.generation = target.generation;
			destinationOwner.committed_lsn = checkpointLsn;
			const VectorPage relocated = page.rebindOwner(destinationOwner);
			const std::vector<std::byte> bytes = relocated.serialize();
			snapshot.release();

			SharedPagePoolEvacuationRequest request;
			request.source_segment_id = source.pool.segment_id;
			request.source_generation = source.pool.generation;
			request.destination_segment_id = destinationSegmentId;
			request.destination_generation = target.generation;
			request.page_id = header.page_id;
			request.expected_owner_lsn = source.owner.committed_lsn;
			request.page_owner_id = header.index_id;
			request.page_owner_kind = expectedOwnerKind;
			request.page_free_bytes = pageFreeBytes(relocated);
			request.payload = bytes;
			return allocator_.evacuatePage(request, checkpointLsn, options);
		} catch (...) {
			snapshot.release();
			throw;
		}
	}

	uint32_t VectorPageStore::pageFreeBytes(const VectorPage &page) {
		const VectorPageHeader header = page.header();
		return static_cast<uint32_t>(static_cast<uint32_t>(header.slot_capacity - header.live_count) *
									 header.record_bytes);
	}

	SegmentKind VectorPageStore::poolKind(VectorStorageKind kind) {
		if (kind == VectorStorageKind::VSK_RAW) {
			return SegmentKind::SGK_VECTOR_RAW_POOL;
		}
		if (kind == VectorStorageKind::VSK_PQ) {
			return SegmentKind::SGK_VECTOR_PQ_POOL;
		}
		throw FormatException(
				FormatError::FERR_INVALID_ENUM,
				"relocatable vector page has an unknown storage kind"); // ZYX_COV_EXCL_LINE: VectorPage validation
																		// permits only the two storage kinds before
																		// this private helper is reached.
	}

} // namespace graph::storage
