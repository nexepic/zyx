/**
 * @file SharedPagePoolAllocator.cpp
 * @brief Atomic first-page allocation and checked reads for relocatable shared pools.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/allocation/SharedPagePoolAllocator.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"
#include "graph/debug/PerfTrace.hpp"

namespace graph::storage {

	namespace {

		[[noreturn]] void throwSharedPagePoolAllocatorError(FormatError error, const std::string &message) {
			throw FormatException(error, message);
		}

		[[nodiscard]] bool isSharedPoolKind(SegmentKind poolKind) {
			switch (poolKind) {
				case SegmentKind::SGK_ENTITY_PAGE_POOL:
				case SegmentKind::SGK_PROPERTY_PAGE_POOL:
				case SegmentKind::SGK_INDEX_PAGE_POOL:
				case SegmentKind::SGK_VECTOR_RAW_POOL:
				case SegmentKind::SGK_VECTOR_PQ_POOL:
				case SegmentKind::SGK_VECTOR_ADJACENCY_POOL:
				case SegmentKind::SGK_VECTOR_MANIFEST_POOL:
				case SegmentKind::SGK_STATE_PAGE_POOL:
				case SegmentKind::SGK_LARGE_VALUE_DATA_POOL:
				case SegmentKind::SGK_LARGE_VALUE_MANIFEST_POOL:
					return true;
				default:
					return false;
			}
		}

		[[nodiscard]] bool matchesPoolAndOwnerKind(SegmentKind poolKind, PageOwnerKind ownerKind) {
			switch (poolKind) {
				case SegmentKind::SGK_ENTITY_PAGE_POOL:
					return ownerKind == PageOwnerKind::POK_ENTITY || ownerKind == PageOwnerKind::POK_ENTITY_LABEL;
				case SegmentKind::SGK_PROPERTY_PAGE_POOL:
					return ownerKind == PageOwnerKind::POK_PROPERTY;
				case SegmentKind::SGK_INDEX_PAGE_POOL:
					return ownerKind == PageOwnerKind::POK_INDEX;
				case SegmentKind::SGK_VECTOR_RAW_POOL:
					return ownerKind == PageOwnerKind::POK_VECTOR_RAW;
				case SegmentKind::SGK_VECTOR_PQ_POOL:
					return ownerKind == PageOwnerKind::POK_VECTOR_PQ;
				case SegmentKind::SGK_VECTOR_ADJACENCY_POOL:
					return ownerKind == PageOwnerKind::POK_VECTOR_ADJACENCY;
				case SegmentKind::SGK_VECTOR_MANIFEST_POOL:
					return ownerKind == PageOwnerKind::POK_VECTOR_MANIFEST;
				case SegmentKind::SGK_STATE_PAGE_POOL:
					return ownerKind == PageOwnerKind::POK_STATE;
				case SegmentKind::SGK_LARGE_VALUE_DATA_POOL:
					return ownerKind == PageOwnerKind::POK_LARGE_VALUE_DATA;
				case SegmentKind::SGK_LARGE_VALUE_MANIFEST_POOL:
					return ownerKind == PageOwnerKind::POK_LARGE_VALUE_MANIFEST;
				default:
					return false;
			}
		}

		[[nodiscard]] bool isKnownReadHint(BufferPoolReadHint hint) {
			switch (hint) {
				case BufferPoolReadHint::BPRH_NORMAL:
				case BufferPoolReadHint::BPRH_SEQUENTIAL:
				case BufferPoolReadHint::BPRH_PREFETCH:
					return true;
				default:
					return false;
			}
		}

		[[nodiscard]] std::array<std::byte, RSS_PAGE_SIZE>
		readTypedPage(const StorageIO &io, uint64_t offset, Generation generation, const char *message,
					  BufferPoolReadHint hint = BufferPoolReadHint::BPRH_NORMAL) {
			std::array<std::byte, RSS_PAGE_SIZE> bytes{};
			if (io.readAt({offset, generation.value()}, bytes.data(), bytes.size(), hint) != bytes.size()) {
				throwSharedPagePoolAllocatorError(FormatError::FERR_BUFFER_TOO_SMALL, message);
			}
			return bytes;
		}

		void writeTypedPage(StorageIO &io, uint64_t offset, Generation generation,
							const std::array<std::byte, RSS_PAGE_SIZE> &bytes) {
			io.writeAt({offset, generation.value()}, bytes.data(), bytes.size());
		}

		[[nodiscard]] std::array<std::byte, RSS_PAGE_SIZE> makePoolControlPage(const SegmentEnvelope &envelope,
																			   const SharedPagePoolHeader &header) {
			std::array<std::byte, RSS_PAGE_SIZE> page{};
			const auto envelopeBytes = SegmentEnvelopeCodec::encode(envelope);
			SharedPagePoolHeader active = header;
			active.control_sequence = 1U;
			SharedPagePoolHeader inactive = header;
			inactive.control_sequence = 0U;
			const auto activeBytes = SharedPagePoolHeaderCodec::encode(active);
			const auto inactiveBytes = SharedPagePoolHeaderCodec::encode(inactive);
			std::copy(envelopeBytes.begin(), envelopeBytes.end(), page.begin());
			std::copy(activeBytes.begin(), activeBytes.end(), page.begin() + RSS_ENVELOPE_BYTES);
			std::copy(inactiveBytes.begin(), inactiveBytes.end(),
					  page.begin() + RSS_ENVELOPE_BYTES + RSS_SHARED_PAGE_POOL_HEADER_BYTES);
			return page;
		}

		struct PoolControlHeaderSelection {
			SharedPagePoolHeader header;
			size_t offset = 0U;
		};

		[[nodiscard]] PoolControlHeaderSelection
		selectPoolControlHeader(std::span<const std::byte, RSS_PAGE_SIZE> page) {
			std::optional<PoolControlHeaderSelection> selected;
			for (size_t record = 0U; record < RSS_SHARED_PAGE_POOL_CONTROL_RECORD_COUNT; ++record) {
				const size_t offset = RSS_ENVELOPE_BYTES + record * RSS_SHARED_PAGE_POOL_HEADER_BYTES;
				try {
					const SharedPagePoolHeader header =
							SharedPagePoolHeaderCodec::decode(page.subspan(offset, RSS_SHARED_PAGE_POOL_HEADER_BYTES));
					if (!selected.has_value() || header.control_sequence > selected->header.control_sequence) {
						selected = {header, offset};
					}
				} catch (const FormatException &) {
					// A torn inactive record is expected to fail its checksum.  The other
					// record remains the durable control state until publication succeeds.
				}
			}
			if (!selected.has_value()) {
				throwSharedPagePoolAllocatorError(FormatError::FERR_CHECKSUM_MISMATCH,
												  "relocatable shared page-pool has no valid control record");
			}
			return *selected;
		}

		void rewriteInactivePoolControlRecord(StorageIO &io, uint64_t pageOffset, Generation generation,
											  const SharedPagePoolHeader &replacement, const char *truncatedMessage) {
			std::array<std::byte, RSS_PAGE_SIZE> page = readTypedPage(io, pageOffset, generation, truncatedMessage);
			const PoolControlHeaderSelection current = selectPoolControlHeader(page);
			const uint8_t invalidIdentity =
					static_cast<uint8_t>(replacement.segment_id != current.header.segment_id) |
					static_cast<uint8_t>(replacement.generation != current.header.generation) |
					static_cast<uint8_t>(replacement.committed_lsn != current.header.committed_lsn) |
					static_cast<uint8_t>(current.header.control_sequence == std::numeric_limits<uint64_t>::max());
			if (invalidIdentity != 0U) {
				throwSharedPagePoolAllocatorError(
						FormatError::FERR_INVALID_STATE,
						"relocatable shared page-pool control record has invalid replacement identity");
			}
			SharedPagePoolHeader next = replacement;
			next.control_sequence = current.header.control_sequence + 1U;
			const auto bytes = SharedPagePoolHeaderCodec::encode(next);
			const size_t targetOffset = current.offset == RSS_ENVELOPE_BYTES
												? RSS_ENVELOPE_BYTES + RSS_SHARED_PAGE_POOL_HEADER_BYTES
												: RSS_ENVELOPE_BYTES;
			// Each record is independently checksummed.  Writing only the inactive
			// record makes a short write unable to overwrite the envelope or the
			// currently selected control record.
			io.writeControlRecord({pageOffset, generation.value()}, targetOffset, bytes);
		}

		[[nodiscard]] std::array<std::byte, RSS_PAGE_SIZE> makeOwnedPage(const PageOwnerHeader &owner,
																		 std::span<const std::byte> payload) {
			// ZYX_COV_EXCL_START: every private caller validates the same owner/payload pair before reaching this
			// final bounds guard; retaining it prevents a future caller from turning a violated invariant into
			// overflow.
			if (payload.size() != owner.payload_bytes || payload.size() > RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES) {
				throwSharedPagePoolAllocatorError(FormatError::FERR_INVALID_STATE,
												  "relocatable shared page payload disagrees with its owner header");
			}
			// ZYX_COV_EXCL_STOP
			std::array<std::byte, RSS_PAGE_SIZE> page{};
			const auto ownerBytes = PageOwnerHeaderCodec::encode(owner);
			std::copy(ownerBytes.begin(), ownerBytes.end(), page.begin());
			std::copy(payload.begin(), payload.end(), page.begin() + RSS_PAGE_OWNER_HEADER_BYTES);
			return page;
		}

		void rewriteTypedPageRange(StorageIO &io, uint64_t pageOffset, Generation generation, size_t byteOffset,
								   std::span<const std::byte> replacement, const char *truncatedMessage) {
			// ZYX_COV_EXCL_START: call sites use fixed codec arrays and compile-time offsets; this guard remains the
			// fail-closed boundary if a later codec adds a larger replacement.
			if (byteOffset > RSS_PAGE_SIZE || replacement.size() > RSS_PAGE_SIZE - byteOffset) {
				throwSharedPagePoolAllocatorError(FormatError::FERR_ARITHMETIC_OVERFLOW,
												  "relocatable shared page rewrite exceeds one physical page");
			}
			// ZYX_COV_EXCL_STOP
			std::array<std::byte, RSS_PAGE_SIZE> page = readTypedPage(io, pageOffset, generation, truncatedMessage);
			std::copy(replacement.begin(), replacement.end(), page.begin() + static_cast<std::ptrdiff_t>(byteOffset));
			writeTypedPage(io, pageOffset, generation, page);
		}

		void clearTypedSegment(StorageIO &io, uint64_t segmentOffset, Generation generation) {
			const std::array<std::byte, RSS_PAGE_SIZE> zeros{};
			for (uint64_t written = 0U; written < RSS_SEGMENT_SIZE; written += RSS_PAGE_SIZE) {
				writeTypedPage(io, segmentOffset + written, generation, zeros);
			}
		}

		void clearTypedSegmentThrough(StorageIO &io, uint64_t segmentOffset, Generation generation) {
			// A virgin pool is unreachable until its catalog publication. Clear its
			// bounded extent in one cache-bypassing range write, while the control
			// and owned frames below retain their individual durable fault boundaries.
			static const std::array<std::byte, RSS_SEGMENT_SIZE> zeros{};
			io.writePagesThrough({segmentOffset, generation.value()}, zeros.data(), zeros.size());
		}

	} // namespace

	SharedPagePoolAllocator::SharedPagePoolAllocator(std::shared_ptr<StorageIO> io, RelocatableMetadataStore &metadata,
													 SharedPagePoolOpenMode openMode) :
		io_(std::move(io)), metadata_(metadata) {
		if (!io_ || !metadata_.usesDataStorage(io_)) {
			throw std::invalid_argument("relocatable shared page-pool allocator requires the metadata data StorageIO");
		}
		{
			const MetadataSnapshotPin snapshot = metadata_.openSnapshot();
			data_region_offset_ = snapshot.superblock().data_region_offset;
		}
		if (openMode == SharedPagePoolOpenMode::SPPOM_RECONCILE) {
			reconcileCurrentPools();
		} else if (openMode != SharedPagePoolOpenMode::SPPOM_READ_ONLY &&
				   openMode != SharedPagePoolOpenMode::SPPOM_DEFER_RECONCILIATION) {
			throw std::invalid_argument("relocatable shared page-pool allocator has an unknown open mode");
		}
	}

	std::optional<SharedPagePoolAllocation>
	SharedPagePoolAllocator::allocateNewPoolPage(SharedPagePoolAllocationRequest request, PhysicalSlotId firstSlot,
												 PhysicalSlotId lastSlot, LogSequenceNumber checkpointLsn,
												 const MetadataPublishOptions &options) {
		validateRequest(request, checkpointLsn);
		CatalogSlotReservationRequest catalogRequest;
		catalogRequest.segment_id = request.segment_id;
		catalogRequest.kind = request.pool_kind;
		catalogRequest.owner_id = request.pool_owner_id;
		catalogRequest.segment_generation = request.generation;
		catalogRequest.state = SegmentState::SGS_SEALED;
		catalogRequest.require_absent = true;
		const PageLocation location{.page_id = request.page_id,
									.segment_id = request.segment_id,
									.page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME,
									.generation = request.generation,
									.free_bytes = request.page_free_bytes};
		const PageOwnerHeader owner{.page_id = request.page_id,
									.owner_id = request.page_owner_id,
									.kind = request.page_owner_kind,
									.payload_bytes = static_cast<uint32_t>(request.payload.size()),
									.generation = request.generation,
									.committed_lsn = checkpointLsn};
		const CatalogSlotReservationPreparer preparer = [this, owner, payload = std::move(request.payload)](
																std::span<const CatalogEntry> entries) {
			if (entries.size() != 1U) { // ZYX_COV_EXCL_LINE: this single-request facade always supplies one entry.
				throwSharedPagePoolAllocatorError(
						FormatError::FERR_INVALID_STATE,
						"relocatable shared page-pool preparation requires one Catalog reservation");
			}
			writePreparedPool(entries.front(), owner, payload);
		};
		const std::optional<CatalogSlotReservations> reservation =
				metadata_.reserveFirstFreeAndUpsertCatalogAndLocations(std::span(&catalogRequest, 1),
																	   std::span(&location, 1), {}, firstSlot, lastSlot,
																	   checkpointLsn, preparer, options);
		if (!reservation.has_value()) {
			return std::nullopt;
		}
		return SharedPagePoolAllocation{reservation->superblock, reservation->entries.front(), location, owner};
	}

	SharedPagePoolAllocation SharedPagePoolAllocator::appendPage(SharedPagePoolAppendRequest request,
																 LogSequenceNumber checkpointLsn,
																 const MetadataPublishOptions &options) {
		reconcilePool(request.segment_id);
		MetadataSnapshotPin snapshot = metadata_.openSnapshot();
		const CatalogEntry expectedPool = metadata_.findCatalog(snapshot, request.segment_id);
		validateAppendRequest(request, expectedPool, checkpointLsn);
		snapshot.release();
		const PageLocation location{.page_id = request.page_id,
									.segment_id = request.segment_id,
									.page_index = 0,
									.generation = request.generation,
									.free_bytes = request.page_free_bytes};
		const PageOwnerHeader owner{.page_id = request.page_id,
									.owner_id = request.page_owner_id,
									.kind = request.page_owner_kind,
									.payload_bytes = static_cast<uint32_t>(request.payload.size()),
									.generation = request.generation,
									.committed_lsn = checkpointLsn};
		PageLocation publishedLocation = location;
		uint8_t allocatedFrame = 0;
		const ExistingPoolPagePreparer preparer =
				[this, &allocatedFrame, owner, payload = std::move(request.payload)](const CatalogEntry &visiblePool,
																					 PageLocation &locationToPublish) {
					SharedPagePoolHeader header = readPoolHeader(visiblePool);
					const std::optional<uint8_t> frame = SharedPagePoolFrameMap::allocateFirstFree(header);
					if (!frame.has_value()) {
						throwSharedPagePoolAllocatorError(FormatError::FERR_INVALID_STATE,
														  "relocatable shared page-pool has no free data frame");
					}
					locationToPublish.page_index = *frame;
					allocatedFrame = *frame;
					writePreparedPage(visiblePool, header, owner, payload, *frame);
				};
		const Superblock published =
				metadata_.appendPageToExistingPool(expectedPool, publishedLocation, checkpointLsn, preparer, options);
		publishedLocation.page_index = allocatedFrame;
		return {published, expectedPool, publishedLocation, owner};
	}

	void SharedPagePoolAllocator::prepareAppendPages(const CatalogEntry &pool,
													 std::span<const SharedPagePoolPreparedAppend> pages,
													 std::span<PageLocation> locations,
													 LogSequenceNumber checkpointLsn) const {
		prepareAppendPagesUnderWrite(*io_, data_region_offset_, pool, pages, locations, checkpointLsn);
	}

	void SharedPagePoolAllocator::prepareAppendPagesUnderWrite(
			StorageIO &io, uint64_t dataRegionOffset, const CatalogEntry &pool,
			std::span<const SharedPagePoolPreparedAppend> pages, std::span<PageLocation> locations,
			LogSequenceNumber checkpointLsn) {
		// ZYX_COV_EXCL_START: BTreePageStore invokes this private helper only after validating a nonempty,
		// cardinality-matched batch and a valid publication LSN.
		if (pages.empty() || pages.size() != locations.size() || !checkpointLsn.isValid()) {
			throwSharedPagePoolAllocatorError(
					FormatError::FERR_INVALID_HEADER,
					"relocatable shared page batch append requires matching nonempty pages and locations");
		}
		// ZYX_COV_EXCL_STOP
		const uint8_t invalidPool = static_cast<uint8_t>(pool.state != SegmentState::SGS_SEALED) |
									static_cast<uint8_t>(!isSharedPoolKind(pool.kind));
		if (invalidPool != 0U) { // ZYX_COV_EXCL_LINE: the metadata writer admits only a current index pool.
			throwSharedPagePoolAllocatorError(
					FormatError::FERR_INVALID_STATE,
					"relocatable shared page batch append requires one current sealed shared pool");
		}
		SharedPagePoolHeader capacity = readPoolHeader(io, dataRegionOffset, pool);
		std::set<PageId> pageIds;
		for (size_t index = 0; index < pages.size(); ++index) {
			const SharedPagePoolPreparedAppend &prepared = pages[index];
			const PageLocation &location = locations[index];
			const uint8_t invalid = static_cast<uint8_t>(location.page_id != prepared.owner.page_id) |
									static_cast<uint8_t>(location.segment_id != pool.segment_id) |
									static_cast<uint8_t>(location.generation != pool.generation) |
									static_cast<uint8_t>(location.page_index != 0U) |
									static_cast<uint8_t>(prepared.owner.flags != 0U) |
									static_cast<uint8_t>(prepared.owner.generation != pool.generation) |
									static_cast<uint8_t>(prepared.owner.committed_lsn != checkpointLsn) |
									static_cast<uint8_t>(prepared.owner.payload_bytes != prepared.payload.size()) |
									static_cast<uint8_t>(prepared.free_bytes > prepared.owner.payload_bytes) |
									static_cast<uint8_t>(!matchesPoolAndOwnerKind(pool.kind, prepared.owner.kind));
			// ZYX_COV_EXCL_START: the typed B+Tree plan assigns each owner from one unique, validated Page ID.
			if (invalid != 0U || !pageIds.insert(prepared.owner.page_id).second) {
				throwSharedPagePoolAllocatorError(
						FormatError::FERR_INVALID_STATE,
						"relocatable shared page batch append has an invalid page owner or duplicate Page ID");
			}
			// ZYX_COV_EXCL_STOP
			static_cast<void>(PageOwnerHeaderCodec::encode(prepared.owner));
			if (!SharedPagePoolFrameMap::allocateFirstFree(capacity).has_value()) { // ZYX_COV_EXCL_LINE: the
				// coordinator's capacity rejection is exercised through the typed store; this helper only preserves it.
				throwSharedPagePoolAllocatorError(
						FormatError::FERR_INVALID_STATE,
						"relocatable shared page-pool has insufficient frames for an atomic batch append");
			}
		}

		SharedPagePoolHeader header = readPoolHeader(io, dataRegionOffset, pool);
		const uint64_t poolOffset = PhysicalSegmentAllocator::checkedSlotOffset(dataRegionOffset, pool.physical_slot);
		for (size_t index = 0; index < pages.size(); ++index) {
			const std::optional<uint8_t> frame = SharedPagePoolFrameMap::allocateFirstFree(header);
			if (!frame.has_value()) { // ZYX_COV_EXCL_LINE: the capacity preflight reserves one frame per page.
				throwSharedPagePoolAllocatorError(FormatError::FERR_INVALID_STATE,
												  "relocatable shared page batch append lost a preflighted free frame");
			}
			locations[index].page_index = *frame;
			locations[index].free_bytes = pages[index].free_bytes;
			const uint8_t invalid = static_cast<uint8_t>(!SharedPagePoolFrameMap::isAllocated(header, *frame)) |
									static_cast<uint8_t>(pages[index].payload.size() != pages[index].owner.payload_bytes);
			if (invalid != 0U) {
				throwSharedPagePoolAllocatorError(FormatError::FERR_INVALID_STATE,
												  "relocatable shared page batch append has inconsistent frame or payload metadata");
			}
			const uint64_t pageOffset = poolOffset + static_cast<uint64_t>(*frame) * RSS_PAGE_SIZE;
			writeTypedPage(io, pageOffset, pool.generation, makeOwnedPage(pages[index].owner, pages[index].payload));
			// Persist the owner frame before switching the inactive control record so
			// a failed write leaves only unreachable state for reconciliation.
			rewriteInactivePoolControlRecord(io, poolOffset, pool.generation, header,
											 "relocatable shared page-pool control page is truncated");
		}
	}

	void SharedPagePoolAllocator::prepareNewPoolPages(const CatalogEntry &pool,
													  std::span<const SharedPagePoolPreparedAppend> pages,
													  std::span<PageLocation> locations,
													  LogSequenceNumber checkpointLsn,
													  size_t preparedPrefixCount) const {
		prepareNewPoolPagesUnderWrite(*io_, data_region_offset_, pool, pages, locations, checkpointLsn,
									  preparedPrefixCount);
	}

	void SharedPagePoolAllocator::prepareNewPoolPagesUnderWrite(StorageIO &io, uint64_t dataRegionOffset,
																const CatalogEntry &pool,
																std::span<const SharedPagePoolPreparedAppend> pages,
																std::span<PageLocation> locations,
																LogSequenceNumber checkpointLsn,
																size_t preparedPrefixCount) {
		if (pages.empty() || pages.size() != locations.size() || pages.size() > RSS_PAGE_POOL_DATA_FRAME_COUNT ||
			preparedPrefixCount > RSS_PAGE_POOL_DATA_FRAME_COUNT - pages.size() ||
			!checkpointLsn.isValid()) { // ZYX_COV_EXCL_LINE: private callback receives a MetadataWriteBatch-validated
										// suffix.
			throwSharedPagePoolAllocatorError(
					FormatError::FERR_INVALID_HEADER,
					"relocatable new shared page pool requires a bounded nonempty suffix and a valid publication LSN");
		}
		const uint8_t invalidPool = static_cast<uint8_t>(pool.state != SegmentState::SGS_SEALED) |
									static_cast<uint8_t>(pool.committed_lsn != checkpointLsn) |
									static_cast<uint8_t>(!isSharedPoolKind(pool.kind));
		if (invalidPool != 0U) { // ZYX_COV_EXCL_LINE: the private new-pool callback receives the writer-validated pool.
			throwSharedPagePoolAllocatorError(
					FormatError::FERR_INVALID_STATE,
					"relocatable new shared page pool requires one sealed shared-pool Catalog entry");
		}

		SharedPagePoolHeader header;
		if (preparedPrefixCount == 0U) {
			header.segment_id = pool.segment_id;
			header.generation = pool.generation;
			header.committed_lsn = pool.committed_lsn;
		} else {
			// A later typed callback may extend only the exact durable prefix emitted
			// by earlier callbacks in this publication. Decoding the complete control
			// page authenticates its envelope, checksum, generation, and frame map.
			header = readPoolHeader(io, dataRegionOffset, pool);
			const uint32_t expectedPrefixMask =
					(UINT32_C(1) << static_cast<uint32_t>(preparedPrefixCount)) - UINT32_C(1);
			if (header.allocated_frames != expectedPrefixMask) {
				throwSharedPagePoolAllocatorError(
						FormatError::FERR_INVALID_STATE,
						"relocatable new shared page pool does not contain its declared prepared-frame prefix");
			}
		}
		std::set<PageId> pageIds;
		for (size_t index = 0; index < pages.size(); ++index) {
			const SharedPagePoolPreparedAppend &prepared = pages[index];
			PageLocation &location = locations[index];
			const uint8_t invalid = static_cast<uint8_t>(location.page_id != prepared.owner.page_id) |
									static_cast<uint8_t>(location.segment_id != pool.segment_id) |
									static_cast<uint8_t>(location.generation != pool.generation) |
									static_cast<uint8_t>(location.page_index != 0U) |
									static_cast<uint8_t>(prepared.owner.flags != 0U) |
									static_cast<uint8_t>(prepared.owner.generation != pool.generation) |
									static_cast<uint8_t>(prepared.owner.committed_lsn != checkpointLsn) |
									static_cast<uint8_t>(prepared.owner.payload_bytes != prepared.payload.size()) |
									static_cast<uint8_t>(prepared.free_bytes > prepared.owner.payload_bytes) |
									static_cast<uint8_t>(!matchesPoolAndOwnerKind(pool.kind, prepared.owner.kind));
			// ZYX_COV_EXCL_START: the enclosing Metadata batch allocates unique logical Page IDs and their owners
			// together.
			if (invalid != 0U || !pageIds.insert(prepared.owner.page_id).second) {
				throwSharedPagePoolAllocatorError(
						FormatError::FERR_INVALID_STATE,
						"relocatable new shared page pool has an invalid initial page owner or duplicate Page ID");
			}
			// ZYX_COV_EXCL_STOP
			static_cast<void>(PageOwnerHeaderCodec::encode(prepared.owner));
			const std::optional<uint8_t> frame = SharedPagePoolFrameMap::allocateFirstFree(header);
			if (!frame.has_value()) { // ZYX_COV_EXCL_LINE: the coordinator reserved one distinct frame per append.
				throwSharedPagePoolAllocatorError(
						FormatError::FERR_INVALID_STATE,
						"relocatable new shared page pool cannot allocate all initial frames");
			}
			location.page_index = *frame;
			location.free_bytes = prepared.free_bytes;
		}

		const uint64_t poolOffset = PhysicalSegmentAllocator::checkedSlotOffset(dataRegionOffset, pool.physical_slot);
		if (preparedPrefixCount == 0U) {
			SegmentEnvelope envelope;
			envelope.header_bytes = RSS_PAGE_SIZE;
			envelope.kind = pool.kind;
			envelope.state = SegmentState::SGS_SEALED;
			envelope.segment_id = pool.segment_id;
			envelope.owner_id = pool.owner_id;
			envelope.generation = pool.generation;
			envelope.committed_lsn = pool.committed_lsn;
			envelope.payload_bytes = RSS_PAGE_POOL_DATA_FRAME_COUNT * RSS_PAGE_SIZE;
			clearTypedSegmentThrough(io, poolOffset, pool.generation);
			writeTypedPage(io, poolOffset, pool.generation, makePoolControlPage(envelope, header));
			for (size_t index = 0U; index < pages.size(); ++index) {
				const uint64_t pageOffset =
						poolOffset + static_cast<uint64_t>(locations[index].page_index) * RSS_PAGE_SIZE;
				writeTypedPage(io, pageOffset, pool.generation,
							   makeOwnedPage(pages[index].owner, pages[index].payload));
			}
		} else {
			rewriteInactivePoolControlRecord(io, poolOffset, pool.generation, header,
											 "relocatable shared page-pool control page is truncated");
			for (size_t index = 0U; index < pages.size(); ++index) {
				const uint64_t pageOffset = poolOffset + static_cast<uint64_t>(locations[index].page_index) * RSS_PAGE_SIZE;
				writeTypedPage(io, pageOffset, pool.generation, makeOwnedPage(pages[index].owner, pages[index].payload));
			}
		}
	}

	void SharedPagePoolAllocator::prepareMutationPages(
			const CatalogEntry &pool, std::span<const PageLocation> expectedReplacements,
			std::span<const SharedPagePoolPreparedReplacement> replacements,
			std::span<const SharedPagePoolPreparedAppend> appends, std::span<PageLocation> replacementLocations,
			std::span<PageLocation> appendLocations, LogSequenceNumber checkpointLsn,
			std::span<const SharedPagePoolPreparedRetirement> retirements) const {
		// These counters are deliberately attached to the generic page mutation
		// boundary. They are cheap when value collection is disabled and expose
		// copy-on-write amplification without coupling the planner to one pool kind.
		if (debug::PerfTrace::areValuesEnabled()) {
			debug::PerfTrace::addValue("native_graph.write.cow.replacement_pages",
					static_cast<int64_t>(replacements.size()));
			debug::PerfTrace::addValue("native_graph.write.cow.append_pages", static_cast<int64_t>(appends.size()));
			debug::PerfTrace::addValue("native_graph.write.cow.retired_pages", static_cast<int64_t>(retirements.size()));
			uint64_t replacementBytes = 0U;
			for (const SharedPagePoolPreparedReplacement &replacement: replacements) {
				const uint64_t bytes = static_cast<uint64_t>(replacement.payload.size());
				replacementBytes = bytes > std::numeric_limits<uint64_t>::max() - replacementBytes
									 ? std::numeric_limits<uint64_t>::max()
									 : replacementBytes + bytes;
			}
			uint64_t appendBytes = 0U;
			for (const SharedPagePoolPreparedAppend &append: appends) {
				const uint64_t bytes = static_cast<uint64_t>(append.payload.size());
				appendBytes = bytes > std::numeric_limits<uint64_t>::max() - appendBytes
								? std::numeric_limits<uint64_t>::max()
								: appendBytes + bytes;
			}
			debug::PerfTrace::addValue("native_graph.write.cow.replacement_payload_bytes",
					static_cast<int64_t>(std::min<uint64_t>(replacementBytes, std::numeric_limits<int64_t>::max())));
			debug::PerfTrace::addValue("native_graph.write.cow.append_payload_bytes",
					static_cast<int64_t>(std::min<uint64_t>(appendBytes, std::numeric_limits<int64_t>::max())));
			if (pool.kind == SegmentKind::SGK_INDEX_PAGE_POOL) {
				debug::PerfTrace::addValue("native_graph.write.btree.replacement_pages",
						static_cast<int64_t>(replacements.size()));
				debug::PerfTrace::addValue("native_graph.write.btree.append_pages",
						static_cast<int64_t>(appends.size()));
				debug::PerfTrace::addValue("native_graph.write.btree.retired_pages",
						static_cast<int64_t>(retirements.size()));
			}
		}
		// ZYX_COV_EXCL_START: BTreePageStore constructs these parallel vectors from one immutable, validated mutation
		// plan.
		if ((expectedReplacements.empty() && appends.empty() && retirements.empty()) ||
			replacements.size() != expectedReplacements.size() ||
			replacementLocations.size() != expectedReplacements.size() || appendLocations.size() != appends.size() ||
			!checkpointLsn.isValid()) {
			throwSharedPagePoolAllocatorError(
					FormatError::FERR_INVALID_HEADER,
					"relocatable shared page mutation requires one matching replacement or appended page batch");
		}
		// ZYX_COV_EXCL_STOP
		const uint8_t invalidPool = static_cast<uint8_t>(pool.state != SegmentState::SGS_SEALED) |
									static_cast<uint8_t>(!isSharedPoolKind(pool.kind));
		if (invalidPool != 0U) { // ZYX_COV_EXCL_LINE: Metadata admits only one current shared pool per callback.
			throwSharedPagePoolAllocatorError(
					FormatError::FERR_INVALID_STATE,
					"relocatable shared page mutation requires one current sealed shared pool");
		}

		SharedPagePoolHeader header = readPoolHeader(pool);
		std::set<PageId> pageIds;
		std::vector<PageOwnerHeader> oldOwners;
		oldOwners.reserve(replacements.size());
		std::vector<PageOwnerHeader> retirementOwners;
		retirementOwners.reserve(retirements.size());
		const auto validatePreparedOwner = [&](const PageOwnerHeader &owner, std::span<const std::byte> payload,
											   uint32_t freeBytes, PageId expectedPageId) {
			const uint8_t invalid = static_cast<uint8_t>(owner.page_id != expectedPageId) |
									static_cast<uint8_t>(owner.flags != 0U) |
									static_cast<uint8_t>(owner.generation != pool.generation) |
									static_cast<uint8_t>(owner.committed_lsn != checkpointLsn) |
									static_cast<uint8_t>(owner.payload_bytes != payload.size()) |
									static_cast<uint8_t>(freeBytes > owner.payload_bytes) |
									static_cast<uint8_t>(!matchesPoolAndOwnerKind(pool.kind, owner.kind));
			// ZYX_COV_EXCL_START: replacement and append owner identities come from the same validated logical page
			// plan.
			if (invalid != 0U || !pageIds.insert(owner.page_id).second) {
				throwSharedPagePoolAllocatorError(
						FormatError::FERR_INVALID_STATE,
						"relocatable shared page mutation has an invalid owner, payload, or duplicate Page ID");
			}
			// ZYX_COV_EXCL_STOP
			static_cast<void>(PageOwnerHeaderCodec::encode(owner));
		};
		for (size_t index = 0; index < replacements.size(); ++index) {
			const PageLocation &expected = expectedReplacements[index];
			const SharedPagePoolPreparedReplacement &replacement = replacements[index];
			const uint8_t invalidLocation =
					static_cast<uint8_t>(expected.segment_id != pool.segment_id) |
					static_cast<uint8_t>(expected.generation != pool.generation) |
					static_cast<uint8_t>(expected.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
					static_cast<uint8_t>(expected.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
					static_cast<uint8_t>(!replacement.expected_owner_lsn.isValid()) |
					static_cast<uint8_t>(!SharedPagePoolFrameMap::isAllocated(header, expected.page_index));
			if (invalidLocation !=
				0U) { // ZYX_COV_EXCL_LINE: the private mutation plan reserves appended locations in its target pool.
				throwSharedPagePoolAllocatorError(
						FormatError::FERR_INVALID_STATE,
						"relocatable shared page mutation has an invalid replacement source location");
			}
			validatePreparedOwner(replacement.owner, replacement.payload, replacement.free_bytes, expected.page_id);
			const PageOwnerHeader oldOwner = readPageOwner(pool, expected.page_index);
			const uint8_t invalidOwner =
					static_cast<uint8_t>(oldOwner.page_id != expected.page_id) |
					static_cast<uint8_t>(oldOwner.owner_id != replacement.owner.owner_id) |
					static_cast<uint8_t>(oldOwner.kind != replacement.owner.kind) |
					static_cast<uint8_t>(oldOwner.generation != expected.generation) |
					static_cast<uint8_t>(oldOwner.committed_lsn != replacement.expected_owner_lsn) |
					static_cast<uint8_t>(oldOwner.flags != 0U) |
					static_cast<uint8_t>(!matchesPoolAndOwnerKind(pool.kind, oldOwner.kind));
			// ZYX_COV_EXCL_START: Metadata serializes this callback with the source-location validation that
			// produced the replacement plan.
			if (invalidOwner != 0U) {
				throwSharedPagePoolAllocatorError(
						FormatError::FERR_INVALID_STATE,
						"relocatable shared page mutation replacement source is stale or retired");
			}
			// ZYX_COV_EXCL_STOP
			oldOwners.push_back(oldOwner);
		}
		for (size_t index = 0; index < appends.size(); ++index) {
			const PageLocation &location = appendLocations[index];
			const uint8_t invalidLocation = static_cast<uint8_t>(location.segment_id != pool.segment_id) |
											static_cast<uint8_t>(location.generation != pool.generation) |
											static_cast<uint8_t>(location.page_index != 0U);
			// ZYX_COV_EXCL_START: the enclosing Metadata batch allocates append locations as the target pool's
			// unassigned frames.
			if (invalidLocation != 0U) {
				throwSharedPagePoolAllocatorError(
						FormatError::FERR_INVALID_STATE,
						"relocatable shared page mutation has an invalid appended-page location");
			}
			// ZYX_COV_EXCL_STOP
			validatePreparedOwner(appends[index].owner, appends[index].payload, appends[index].free_bytes,
								  location.page_id);
		}
		if (!retirements.empty()) {
			validateRetirementSources(pool, retirements);
			for (const SharedPagePoolPreparedRetirement &retirement: retirements) {
				if (!pageIds.insert(retirement.location.page_id).second) {
					throwSharedPagePoolAllocatorError(
							FormatError::FERR_INVALID_STATE,
							"relocatable shared page mutation has a duplicate retirement Page ID");
				}
				retirementOwners.push_back(readPageOwner(pool, retirement.location.page_index));
			}
		}

		SharedPagePoolHeader capacity = header;
		for (size_t index = 0; index < replacements.size() + appends.size(); ++index) {
			// ZYX_COV_EXCL_START: Metadata preflights one distinct target frame for every replacement and append.
			if (!SharedPagePoolFrameMap::allocateFirstFree(capacity).has_value()) {
				throwSharedPagePoolAllocatorError(
						FormatError::FERR_INVALID_STATE,
						"relocatable shared page-pool has insufficient frames for an atomic mutation (kind " +
								std::to_string(static_cast<uint32_t>(pool.kind)) + ", segment " +
								std::to_string(pool.segment_id.value()) + ", replacements " +
								std::to_string(replacements.size()) + ", appends " + std::to_string(appends.size()) +
								", owner " + std::to_string(pool.owner_id) + ")");
			}
			// ZYX_COV_EXCL_STOP
		}

		for (size_t index = 0; index < replacements.size(); ++index) {
			const std::optional<uint8_t> frame = SharedPagePoolFrameMap::allocateFirstFree(header);
			if (!frame.has_value()) { // ZYX_COV_EXCL_LINE: every replacement reserves a free target frame above.
				throwSharedPagePoolAllocatorError(FormatError::FERR_INVALID_STATE,
												  "relocatable shared page-pool lost a preflighted replacement frame");
			}
			replacementLocations[index] = expectedReplacements[index];
			replacementLocations[index].page_index = *frame;
			replacementLocations[index].free_bytes = replacements[index].free_bytes;
			writePreparedReplacement(pool, header, oldOwners[index], expectedReplacements[index].page_index,
									 replacements[index].owner, replacements[index].payload, *frame);
		}
		for (size_t index = 0; index < appends.size(); ++index) {
			const std::optional<uint8_t> frame = SharedPagePoolFrameMap::allocateFirstFree(header);
			if (!frame.has_value()) { // ZYX_COV_EXCL_LINE: every append reserves a free target frame above.
				throwSharedPagePoolAllocatorError(
						FormatError::FERR_INVALID_STATE,
						"relocatable shared page-pool lost a preflighted appended-page frame");
			}
			appendLocations[index].page_index = *frame;
			appendLocations[index].free_bytes = appends[index].free_bytes;
			writePreparedPage(pool, header, appends[index].owner, appends[index].payload, *frame);
		}
		// Page Directory publication is the sole reachability transition.  A
		// source owner stays immutable: rewriting it before the new directory root
		// is durable could corrupt the still-published generation on a torn write.
		static_cast<void>(retirementOwners);
	}

	void SharedPagePoolAllocator::validateRetirementSources(
			const CatalogEntry &pool, std::span<const SharedPagePoolPreparedRetirement> retirements) const {
		validateRetirementSourcesUnderWrite(*io_, data_region_offset_, pool, retirements);
	}

	void SharedPagePoolAllocator::validateRetirementSourcesUnderWrite(
			const StorageIO &io, uint64_t dataRegionOffset, const CatalogEntry &pool,
			std::span<const SharedPagePoolPreparedRetirement> retirements) {
		const uint8_t invalidPool = static_cast<uint8_t>(pool.state != SegmentState::SGS_SEALED) |
									static_cast<uint8_t>(!isSharedPoolKind(pool.kind));
		if (invalidPool != 0U) {
			throwSharedPagePoolAllocatorError(
					FormatError::FERR_INVALID_STATE,
					"relocatable retirement authentication requires a current sealed shared pool");
		}
		const SharedPagePoolHeader header = readPoolHeader(io, dataRegionOffset, pool);
		const auto readOwner = [&io, dataRegionOffset, &pool](uint8_t frameIndex) {
			const uint64_t poolOffset =
					PhysicalSegmentAllocator::checkedSlotOffset(dataRegionOffset, pool.physical_slot);
			const uint64_t pageOffset = poolOffset + static_cast<uint64_t>(frameIndex) * RSS_PAGE_SIZE;
			const std::array<std::byte, RSS_PAGE_SIZE> page =
					readTypedPage(io, pageOffset, pool.generation, "relocatable retirement owner header is truncated");
			std::array<std::byte, RSS_PAGE_OWNER_HEADER_BYTES> bytes{};
			std::copy_n(page.begin(), bytes.size(), bytes.begin());
			return PageOwnerHeaderCodec::decode(bytes);
		};
		std::set<PageId> pageIds;
		for (const SharedPagePoolPreparedRetirement &retirement: retirements) {
			const PageLocation &location = retirement.location;
			const uint8_t invalidLocation =
					static_cast<uint8_t>(location.segment_id != pool.segment_id) |
					static_cast<uint8_t>(location.generation != pool.generation) |
					static_cast<uint8_t>(location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
					static_cast<uint8_t>(location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
					static_cast<uint8_t>(retirement.expected_owner_id == 0U) |
					static_cast<uint8_t>(retirement.expected_owner_kind == PageOwnerKind::POK_NONE) |
					static_cast<uint8_t>(!retirement.expected_owner_lsn.isValid()) |
					static_cast<uint8_t>(!SharedPagePoolFrameMap::isAllocated(header, location.page_index));
			if (invalidLocation != 0U || !pageIds.insert(location.page_id).second) {
				throwSharedPagePoolAllocatorError(
						FormatError::FERR_INVALID_STATE,
						"relocatable retirement authentication found an invalid or duplicate source");
			}
			const PageOwnerHeader owner = readOwner(location.page_index);
			const uint8_t invalidOwner = static_cast<uint8_t>(owner.page_id != location.page_id) |
										 static_cast<uint8_t>(owner.owner_id != retirement.expected_owner_id) |
										 static_cast<uint8_t>(owner.kind != retirement.expected_owner_kind) |
										 static_cast<uint8_t>(owner.generation != location.generation) |
										 static_cast<uint8_t>(owner.committed_lsn != retirement.expected_owner_lsn) |
										 static_cast<uint8_t>(owner.flags != 0U) |
										 static_cast<uint8_t>(!matchesPoolAndOwnerKind(pool.kind, owner.kind));
			if (invalidOwner != 0U) {
				throwSharedPagePoolAllocatorError(
						FormatError::FERR_INVALID_STATE,
						"relocatable retirement authentication found a stale or foreign source");
			}
		}
	}

	void SharedPagePoolAllocator::validatePreparedPublicationsUnderWrite(
			const StorageIO &io, uint64_t dataRegionOffset, const CatalogEntry &pool,
			std::span<const SharedPagePoolPreparedPublication> publications) {
		debug::ScopedPerfTimer validationTimer("native_graph.commit.publication_validation");
		const uint8_t invalidPool = static_cast<uint8_t>(pool.state != SegmentState::SGS_SEALED) |
									static_cast<uint8_t>(!isSharedPoolKind(pool.kind));
		if (invalidPool != 0U || publications.empty()) {
			throwSharedPagePoolAllocatorError(
					FormatError::FERR_INVALID_STATE,
					"relocatable publication authentication requires a current shared pool and assigned frames");
		}
		SharedPagePoolHeader header;
		{
			debug::ScopedPerfTimer headerTimer("native_graph.commit.publication_validation.pool_header");
			header = readPoolHeader(io, dataRegionOffset, pool);
		}
		const uint64_t poolOffset = PhysicalSegmentAllocator::checkedSlotOffset(dataRegionOffset, pool.physical_slot);
		std::set<PageId> pageIds;
		std::set<uint8_t> frames;
		std::vector<const SharedPagePoolPreparedPublication *> ordered;
		ordered.reserve(publications.size());
		{
			debug::ScopedPerfTimer shapeTimer("native_graph.commit.publication_validation.shape");
			for (const SharedPagePoolPreparedPublication &publication: publications) {
				const PageLocation &location = publication.location;
				const uint8_t invalidLocation =
						static_cast<uint8_t>(location.segment_id != pool.segment_id) |
						static_cast<uint8_t>(location.generation != pool.generation) |
						static_cast<uint8_t>(location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
						static_cast<uint8_t>(location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
						static_cast<uint8_t>(publication.expected_owner_id == 0U) |
						static_cast<uint8_t>(publication.expected_owner_kind == PageOwnerKind::POK_NONE) |
						static_cast<uint8_t>(!publication.expected_owner_lsn.isValid()) |
						static_cast<uint8_t>(!SharedPagePoolFrameMap::isAllocated(header, location.page_index));
				if (invalidLocation != 0U || !pageIds.insert(location.page_id).second ||
					!frames.insert(location.page_index).second) {
					throwSharedPagePoolAllocatorError(
							FormatError::FERR_INVALID_STATE,
							"relocatable publication authentication found an invalid or duplicate assigned frame");
				}
				ordered.push_back(&publication);
			}
			std::ranges::sort(ordered, {}, [](const SharedPagePoolPreparedPublication *publication) {
				return publication->location.page_index;
			});
		}

		size_t readRuns = 0U;
		for (size_t first = 0U; first < ordered.size();) {
			size_t last = first + 1U;
			while (last < ordered.size() &&
				   ordered[last]->location.page_index == ordered[last - 1U]->location.page_index + 1U) {
				++last;
			}
			const uint8_t firstFrame = ordered[first]->location.page_index;
			const size_t pageCount = last - first;
			std::vector<std::byte> pages(pageCount * RSS_PAGE_SIZE);
			const uint64_t firstPageOffset = poolOffset + static_cast<uint64_t>(firstFrame) * RSS_PAGE_SIZE;
			{
				debug::ScopedPerfTimer readTimer("native_graph.commit.publication_validation.physical_read");
				if (io.readPages({firstPageOffset, pool.generation.value()}, pages.data(), pages.size(),
								 BufferPoolReadHint::BPRH_SEQUENTIAL) != pages.size()) {
					throwSharedPagePoolAllocatorError(FormatError::FERR_BUFFER_TOO_SMALL,
											  "relocatable publication owner header is truncated");
				}
			}
			++readRuns;
			{
				debug::ScopedPerfTimer ownerTimer("native_graph.commit.publication_validation.owner_decode");
				for (size_t index = first; index < last; ++index) {
					const SharedPagePoolPreparedPublication &publication = *ordered[index];
					const size_t pageOffset = (index - first) * RSS_PAGE_SIZE;
					const PageOwnerHeader owner = PageOwnerHeaderCodec::decode(
							std::span<const std::byte>(pages.data() + pageOffset, RSS_PAGE_OWNER_HEADER_BYTES));
					const PageLocation &location = publication.location;
					const uint8_t invalidOwner = static_cast<uint8_t>(owner.page_id != location.page_id) |
												 static_cast<uint8_t>(owner.owner_id != publication.expected_owner_id) |
												 static_cast<uint8_t>(owner.kind != publication.expected_owner_kind) |
												 static_cast<uint8_t>(owner.generation != location.generation) |
												 static_cast<uint8_t>(owner.committed_lsn != publication.expected_owner_lsn) |
												 static_cast<uint8_t>(owner.flags != 0U) |
												 static_cast<uint8_t>(!matchesPoolAndOwnerKind(pool.kind, owner.kind));
					if (invalidOwner != 0U) {
						throwSharedPagePoolAllocatorError(
								FormatError::FERR_INVALID_STATE,
								"relocatable publication authentication found a missing, stale, or foreign assigned frame");
					}
				}
			}
			first = last;
		}
		if (debug::PerfTrace::areValuesEnabled()) {
			debug::PerfTrace::addValue("native_graph.commit.publication_validation_frames",
					static_cast<int64_t>(publications.size()));
			debug::PerfTrace::addValue("native_graph.commit.publication_validation_read_runs",
					static_cast<int64_t>(readRuns));
		}
	}

	void SharedPagePoolAllocator::prepareCrossPoolReplacementPages(
			const CatalogEntry &destinationPool,
			std::span<const SharedPagePoolPreparedCrossPoolReplacement> replacements,
			std::span<PageLocation> replacementLocations, LogSequenceNumber checkpointLsn) const {
		if (replacements.empty() || replacements.size() != replacementLocations.size() || !checkpointLsn.isValid() ||
			destinationPool.state != SegmentState::SGS_SEALED || !isSharedPoolKind(destinationPool.kind)) {
			throwSharedPagePoolAllocatorError(
					FormatError::FERR_INVALID_HEADER,
					"relocatable cross-pool replacement requires a destination, matching pages, and a checkpoint LSN");
		}

		std::map<uint64_t, SharedPagePoolHeader> headers;
		std::map<uint64_t, CatalogEntry> pools;
		const auto headerFor = [this, &headers, &pools](const CatalogEntry &pool) -> SharedPagePoolHeader & {
			auto [poolIterator, insertedPool] = pools.try_emplace(pool.segment_id.value(), pool);
			// ZYX_COV_EXCL_START: the private planner builds every occurrence of one Segment ID from the same pinned
			// Catalog entry. This guard prevents future callers from mixing snapshot generations in one batch.
			if (!insertedPool &&
				(poolIterator->second.physical_slot != pool.physical_slot || poolIterator->second.kind != pool.kind ||
				 poolIterator->second.owner_id != pool.owner_id || poolIterator->second.generation != pool.generation ||
				 poolIterator->second.state != pool.state ||
				 poolIterator->second.committed_lsn != pool.committed_lsn)) {
				throwSharedPagePoolAllocatorError(
						FormatError::FERR_INVALID_STATE,
						"relocatable cross-pool replacement received conflicting Catalog identities");
			}
			// ZYX_COV_EXCL_STOP
			auto [headerIterator, insertedHeader] = headers.try_emplace(pool.segment_id.value());
			if (insertedHeader) {
				headerIterator->second = readPoolHeader(pool);
			}
			return headerIterator->second;
		};

		SharedPagePoolHeader &destinationHeader = headerFor(destinationPool);
		std::set<PageId> pageIds;
		for (const SharedPagePoolPreparedCrossPoolReplacement &replacement: replacements) {
			const CatalogEntry &sourcePool = replacement.source_pool;
			const PageLocation &source = replacement.source_location;
			const PageOwnerHeader &owner = replacement.owner;
			const uint8_t invalidSource =
					static_cast<uint8_t>(sourcePool.state != SegmentState::SGS_SEALED) |
					static_cast<uint8_t>(sourcePool.kind != destinationPool.kind) |
					static_cast<uint8_t>(sourcePool.owner_id != destinationPool.owner_id) |
					static_cast<uint8_t>(source.segment_id != sourcePool.segment_id) |
					static_cast<uint8_t>(source.generation != sourcePool.generation) |
					static_cast<uint8_t>(source.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
					static_cast<uint8_t>(source.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
					static_cast<uint8_t>(!replacement.expected_owner_lsn.isValid()) |
					static_cast<uint8_t>(owner.page_id != source.page_id) |
					static_cast<uint8_t>(owner.owner_id != destinationPool.owner_id) |
					static_cast<uint8_t>(owner.flags != 0U) |
					static_cast<uint8_t>(owner.generation != destinationPool.generation) |
					static_cast<uint8_t>(owner.committed_lsn != checkpointLsn) |
					static_cast<uint8_t>(owner.payload_bytes != replacement.payload.size()) |
					static_cast<uint8_t>(replacement.free_bytes > owner.payload_bytes) |
					static_cast<uint8_t>(!matchesPoolAndOwnerKind(destinationPool.kind, owner.kind)) |
					static_cast<uint8_t>(!pageIds.insert(source.page_id).second);
			if (invalidSource != 0U) {
				throwSharedPagePoolAllocatorError(FormatError::FERR_INVALID_STATE,
												  "relocatable cross-pool replacement has an invalid source, "
												  "destination owner, or duplicate Page ID");
			}
			const SharedPagePoolHeader &sourceHeader = headerFor(sourcePool);
			if (!SharedPagePoolFrameMap::isAllocated(sourceHeader, source.page_index)) {
				throwSharedPagePoolAllocatorError(
						FormatError::FERR_INVALID_STATE,
						"relocatable cross-pool replacement targets an unallocated source frame");
			}
			const PageOwnerHeader sourceOwner = readPageOwner(sourcePool, source.page_index);
			const uint8_t invalidOwner =
					static_cast<uint8_t>(sourceOwner.page_id != source.page_id) |
					static_cast<uint8_t>(sourceOwner.owner_id != owner.owner_id) |
					static_cast<uint8_t>(sourceOwner.kind != owner.kind) |
					static_cast<uint8_t>(sourceOwner.generation != source.generation) |
					static_cast<uint8_t>(sourceOwner.committed_lsn != replacement.expected_owner_lsn) |
					static_cast<uint8_t>(sourceOwner.flags != 0U) |
					static_cast<uint8_t>(!matchesPoolAndOwnerKind(sourcePool.kind, sourceOwner.kind));
			if (invalidOwner != 0U) {
				throwSharedPagePoolAllocatorError(
						FormatError::FERR_INVALID_STATE,
						"relocatable cross-pool replacement source owner is stale or retired");
			}
		}

		SharedPagePoolHeader capacity = destinationHeader;
		for (size_t index = 0U; index < replacements.size(); ++index) {
			if (!SharedPagePoolFrameMap::allocateFirstFree(capacity).has_value()) {
				throwSharedPagePoolAllocatorError(
						FormatError::FERR_INVALID_STATE,
						"relocatable shared page-pool has insufficient frames for cross-pool replacement");
			}
		}

		for (size_t index = 0U; index < replacements.size(); ++index) {
			const SharedPagePoolPreparedCrossPoolReplacement &replacement = replacements[index];
			SharedPagePoolHeader &sourceHeader = headerFor(replacement.source_pool);
			const PageOwnerHeader sourceOwner =
					readPageOwner(replacement.source_pool, replacement.source_location.page_index);
			const std::optional<uint8_t> frame = SharedPagePoolFrameMap::allocateFirstFree(destinationHeader);
			// ZYX_COV_EXCL_START: capacity was preflighted on this same local header and no callback can mutate it.
			if (!frame.has_value()) {
				throwSharedPagePoolAllocatorError(
						FormatError::FERR_INVALID_STATE,
						"relocatable cross-pool replacement lost a preflighted destination frame");
			}
			// ZYX_COV_EXCL_STOP
			replacementLocations[index] = {replacement.source_location.page_id, destinationPool.segment_id, *frame,
										   destinationPool.generation, replacement.free_bytes};
			if (replacement.source_pool.segment_id == destinationPool.segment_id) {
				writePreparedReplacement(destinationPool, destinationHeader, sourceOwner,
										 replacement.source_location.page_index, replacement.owner, replacement.payload,
										 *frame);
			} else {
				writePreparedCrossPoolEvacuation(replacement.source_pool, sourceHeader, sourceOwner,
												 replacement.source_location.page_index, destinationPool,
												 destinationHeader, replacement.owner, replacement.payload, *frame);
			}
		}
	}

	SharedPagePoolAllocation SharedPagePoolAllocator::replacePage(SharedPagePoolReplacementRequest request,
																  LogSequenceNumber checkpointLsn,
																  const MetadataPublishOptions &options) {
		reconcilePool(request.segment_id);
		MetadataSnapshotPin snapshot = metadata_.openSnapshot();
		const CatalogEntry expectedPool = metadata_.findCatalog(snapshot, request.segment_id);
		validateReplacementRequest(request, expectedPool, checkpointLsn);
		const PageLocation expectedLocation = metadata_.findPageLocation(snapshot, request.page_id);
		const uint8_t invalidExpectedLocation =
				static_cast<uint8_t>(expectedLocation.segment_id != request.segment_id) |
				static_cast<uint8_t>(expectedLocation.generation != request.generation);
		// ZYX_COV_EXCL_START: reconciliation has already proved the current Page Directory mapping before this facade
		// reads it.
		if (invalidExpectedLocation != 0U) {
			throwSharedPagePoolAllocatorError(
					FormatError::FERR_INVALID_STATE,
					"relocatable shared page replacement does not target the requested pool generation");
		}
		// ZYX_COV_EXCL_STOP
		snapshot.release();
		const PageOwnerHeader owner{.page_id = request.page_id,
									.owner_id = request.page_owner_id,
									.kind = request.page_owner_kind,
									.payload_bytes = static_cast<uint32_t>(request.payload.size()),
									.generation = request.generation,
									.committed_lsn = checkpointLsn};
		PageLocation replacement{.page_id = request.page_id,
								 .segment_id = request.segment_id,
								 .page_index = 0,
								 .generation = request.generation,
								 .free_bytes = request.page_free_bytes};
		uint8_t allocatedFrame = 0;
		const ExistingPoolPageReplacementPreparer preparer = [this, &allocatedFrame, owner,
															  expectedOwnerLsn = request.expected_owner_lsn,
															  payload = std::move(request.payload)](
																	 const CatalogEntry &visiblePool,
																	 const PageLocation &oldLocation,
																	 PageLocation &locationToPublish) {
			SharedPagePoolHeader header = readPoolHeader(visiblePool);
			// ZYX_COV_EXCL_START: the metadata replacement validates the current location and reconciled pool bitmap
			// before entering this private preparer.
			if (!SharedPagePoolFrameMap::isAllocated(header, oldLocation.page_index)) {
				throwSharedPagePoolAllocatorError(
						FormatError::FERR_INVALID_STATE,
						"relocatable shared page replacement targets an unallocated source frame");
			}
			// ZYX_COV_EXCL_STOP
			const PageOwnerHeader oldOwner = readPageOwner(visiblePool, oldLocation.page_index);
			const uint8_t invalidOldOwner =
					static_cast<uint8_t>(oldOwner.page_id != oldLocation.page_id) |
					static_cast<uint8_t>(oldOwner.generation != oldLocation.generation) |
					static_cast<uint8_t>(oldOwner.committed_lsn != expectedOwnerLsn) |
					static_cast<uint8_t>(oldOwner.flags != 0U) |
					static_cast<uint8_t>(!matchesPoolAndOwnerKind(visiblePool.kind, oldOwner.kind));
			if (invalidOldOwner != 0U) {
				throwSharedPagePoolAllocatorError(
						FormatError::FERR_INVALID_STATE,
						"relocatable shared page replacement source owner is stale or retired");
			}
			const std::optional<uint8_t> frame = SharedPagePoolFrameMap::allocateFirstFree(header);
			if (!frame.has_value()) {
				throwSharedPagePoolAllocatorError(FormatError::FERR_INVALID_STATE,
												  "relocatable shared page-pool has no free frame for a replacement");
			}
			locationToPublish.page_index = *frame;
			allocatedFrame = *frame;
			writePreparedReplacement(visiblePool, header, oldOwner, oldLocation.page_index, owner, payload, *frame);
		};
		const Superblock published = metadata_.replacePageInExistingPool(expectedPool, expectedLocation, replacement,
																		 checkpointLsn, preparer, options);
		replacement.page_index = allocatedFrame;
		return {published, expectedPool, replacement, owner};
	}

	SharedPagePoolAllocation SharedPagePoolAllocator::evacuatePage(SharedPagePoolEvacuationRequest request,
																   LogSequenceNumber checkpointLsn,
																   const MetadataPublishOptions &options) {
		return evacuatePageImpl(request, {}, {}, {}, {}, {}, {}, checkpointLsn, options);
	}

	SharedPagePoolAllocation
	SharedPagePoolAllocator::evacuateIndexPage(SharedPagePoolEvacuationRequest request,
											   const std::optional<IndexRootLocation> &expectedRoot,
											   const std::optional<IndexRootLocation> &replacementRoot,
											   LogSequenceNumber checkpointLsn, const MetadataPublishOptions &options) {
		if (expectedRoot.has_value() != replacementRoot.has_value()) {
			throwSharedPagePoolAllocatorError(
					FormatError::FERR_INVALID_HEADER,
					"relocatable typed index-page evacuation requires both current and replacement root mappings");
		}
		return evacuatePageImpl(request, expectedRoot, replacementRoot, {}, {}, {}, {}, checkpointLsn, options);
	}

	SharedPagePoolAllocation SharedPagePoolAllocator::evacuateVectorManifestPage(
			SharedPagePoolEvacuationRequest request, const VectorManifestLocation &expectedManifest,
			const VectorManifestLocation &replacementManifest, LogSequenceNumber checkpointLsn,
			const MetadataPublishOptions &options) {
		return evacuatePageImpl(request, {}, {}, expectedManifest, replacementManifest, {}, {}, checkpointLsn, options);
	}

	SharedPagePoolAllocation SharedPagePoolAllocator::evacuateLargeValueManifestPage(
			SharedPagePoolEvacuationRequest request, const LargeValueManifestLocation &expectedManifest,
			const LargeValueManifestLocation &replacementManifest, LogSequenceNumber checkpointLsn,
			const MetadataPublishOptions &options) {
		return evacuatePageImpl(request, {}, {}, {}, {}, expectedManifest, replacementManifest, checkpointLsn, options);
	}

	SharedPagePoolAllocation SharedPagePoolAllocator::evacuatePageImpl(
			SharedPagePoolEvacuationRequest request, const std::optional<IndexRootLocation> &expectedRoot,
			const std::optional<IndexRootLocation> &replacementRoot,
			const std::optional<VectorManifestLocation> &expectedManifest,
			const std::optional<VectorManifestLocation> &replacementManifest,
			const std::optional<LargeValueManifestLocation> &expectedLargeValueManifest,
			const std::optional<LargeValueManifestLocation> &replacementLargeValueManifest,
			LogSequenceNumber checkpointLsn, const MetadataPublishOptions &options) {
		reconcilePool(request.source_segment_id);
		reconcilePool(request.destination_segment_id);
		MetadataSnapshotPin snapshot = metadata_.openSnapshot();
		const CatalogEntry expectedSourcePool = metadata_.findCatalog(snapshot, request.source_segment_id);
		const CatalogEntry expectedDestinationPool = metadata_.findCatalog(snapshot, request.destination_segment_id);
		validateEvacuationRequest(request, expectedSourcePool, expectedDestinationPool, checkpointLsn);
		const PageLocation expectedSourceLocation = metadata_.findPageLocation(snapshot, request.page_id);
		const uint8_t invalidExpectedLocation =
				static_cast<uint8_t>(expectedSourceLocation.segment_id != request.source_segment_id) |
				static_cast<uint8_t>(expectedSourceLocation.generation != request.source_generation);
		if (invalidExpectedLocation != 0U) {
			throwSharedPagePoolAllocatorError(
					FormatError::FERR_INVALID_STATE,
					"relocatable shared page evacuation does not target the requested source pool");
		}
		snapshot.release();
		const PageOwnerHeader owner{.page_id = request.page_id,
									.owner_id = request.page_owner_id,
									.kind = request.page_owner_kind,
									.payload_bytes = static_cast<uint32_t>(request.payload.size()),
									.generation = request.destination_generation,
									.committed_lsn = checkpointLsn};
		PageLocation destination{.page_id = request.page_id,
								 .segment_id = request.destination_segment_id,
								 .page_index = 0,
								 .generation = request.destination_generation,
								 .free_bytes = request.page_free_bytes};
		uint8_t allocatedFrame = 0;
		const CrossPoolPageEvacuationPreparer preparer = [this, &allocatedFrame, owner,
														  expectedOwnerLsn = request.expected_owner_lsn,
														  payload = std::move(request.payload)](
																 const CatalogEntry &sourcePool,
																 const PageLocation &sourceLocation,
																 const CatalogEntry &destinationPool,
																 PageLocation &locationToPublish) {
			SharedPagePoolHeader sourceHeader = readPoolHeader(sourcePool);
			// ZYX_COV_EXCL_START: the metadata evacuation writer verifies this source location while holding
			// publication ownership.
			if (!SharedPagePoolFrameMap::isAllocated(sourceHeader, sourceLocation.page_index)) {
				throwSharedPagePoolAllocatorError(
						FormatError::FERR_INVALID_STATE,
						"relocatable shared page evacuation targets an unallocated source frame");
			}
			// ZYX_COV_EXCL_STOP
			const PageOwnerHeader sourceOwner = readPageOwner(sourcePool, sourceLocation.page_index);
			const uint8_t invalidSourceOwner =
					static_cast<uint8_t>(sourceOwner.page_id != sourceLocation.page_id) |
					static_cast<uint8_t>(sourceOwner.owner_id != owner.owner_id) |
					static_cast<uint8_t>(sourceOwner.kind != owner.kind) |
					static_cast<uint8_t>(sourceOwner.generation != sourceLocation.generation) |
					static_cast<uint8_t>(sourceOwner.committed_lsn != expectedOwnerLsn) |
					static_cast<uint8_t>(sourceOwner.flags != 0U) |
					static_cast<uint8_t>(!matchesPoolAndOwnerKind(sourcePool.kind, sourceOwner.kind));
			if (invalidSourceOwner != 0U) {
				throwSharedPagePoolAllocatorError(
						FormatError::FERR_INVALID_STATE,
						"relocatable shared page evacuation source owner is stale or retired");
			}
			SharedPagePoolHeader destinationHeader = readPoolHeader(destinationPool);
			const std::optional<uint8_t> frame = SharedPagePoolFrameMap::allocateFirstFree(destinationHeader);
			if (!frame.has_value()) {
				throwSharedPagePoolAllocatorError(FormatError::FERR_INVALID_STATE,
												  "relocatable shared page-pool has no free frame for evacuation");
			}
			locationToPublish.page_index = *frame;
			allocatedFrame = *frame;
			writePreparedCrossPoolEvacuation(sourcePool, sourceHeader, sourceOwner, sourceLocation.page_index,
											 destinationPool, destinationHeader, owner, payload, *frame);
		};
		const Superblock published =
				expectedRoot.has_value()
						? metadata_.evacuateIndexRootPageAcrossPools(
								  expectedSourcePool, expectedSourceLocation, expectedDestinationPool, destination,
								  *expectedRoot, *replacementRoot, checkpointLsn, preparer, options)
				: expectedManifest.has_value()
						? metadata_.evacuateVectorManifestPageAcrossPools(
								  expectedSourcePool, expectedSourceLocation, expectedDestinationPool, destination,
								  *expectedManifest, *replacementManifest, checkpointLsn, preparer, options)
				: expectedLargeValueManifest.has_value()
						? metadata_.evacuateLargeValueManifestPageAcrossPools(
								  expectedSourcePool, expectedSourceLocation, expectedDestinationPool, destination,
								  *expectedLargeValueManifest, *replacementLargeValueManifest, checkpointLsn, preparer,
								  options)
						: metadata_.evacuatePageAcrossPools(expectedSourcePool, expectedSourceLocation,
															expectedDestinationPool, destination, checkpointLsn,
															preparer, options);
		destination.page_index = allocatedFrame;
		return {published, expectedDestinationPool, destination, owner};
	}

	SharedPagePoolContents SharedPagePoolAllocator::read(const MetadataSnapshotPin &snapshot, PageId pageId,
														 BufferPoolReadHint hint) const {
		if (!isKnownReadHint(hint)) {
			throwSharedPagePoolAllocatorError(FormatError::FERR_INVALID_ENUM,
											  "relocatable shared page read has an unknown Buffer Pool hint");
		}
		if (debug::PerfTrace::areValuesEnabled()) {
			debug::PerfTrace::addValue("native_graph.read.shared_page_route_resolution", 1);
		}
		const PageLocation location = metadata_.findPageLocation(snapshot, pageId);
		const CatalogEntry pool = metadata_.findCatalog(snapshot, location.segment_id);
		return readKnownLocation(snapshot, pool, location, hint);
	}

	SharedPagePoolContents SharedPagePoolAllocator::readKnownLocation(
			const MetadataSnapshotPin &snapshot, const CatalogEntry &pool, const PageLocation &location,
			const BufferPoolReadHint hint) const {
		if (!isKnownReadHint(hint)) {
			throwSharedPagePoolAllocatorError(FormatError::FERR_INVALID_ENUM,
					"relocatable shared known-location read has an unknown Buffer Pool hint");
		}
		if (debug::PerfTrace::areValuesEnabled()) {
			debug::PerfTrace::addValue("native_graph.read.shared_page_known_location_copy", 1);
		}
		const uint8_t invalidLocation =
				static_cast<uint8_t>(!metadata_.ownsSnapshot(snapshot)) |
				static_cast<uint8_t>(!location.page_id.isValid()) |
				static_cast<uint8_t>(location.segment_id != pool.segment_id) |
				static_cast<uint8_t>(pool.state != SegmentState::SGS_SEALED) |
				static_cast<uint8_t>(pool.generation != location.generation) |
				static_cast<uint8_t>(!isSharedPoolKind(pool.kind)) |
				static_cast<uint8_t>(!metadata_.isPhysicalSlotAllocated(snapshot, pool.physical_slot));
		if (invalidLocation != 0U) {
			throwSharedPagePoolAllocatorError(
					FormatError::FERR_INVALID_STATE,
					"relocatable shared page location does not reference an allocated pool generation");
		}
		const uint64_t poolOffset =
				PhysicalSegmentAllocator::checkedSlotOffset(data_region_offset_, pool.physical_slot);
		const std::array<std::byte, RSS_PAGE_SIZE> poolPage =
				readTypedPage(*io_, poolOffset, pool.generation, "relocatable shared page-pool envelope is truncated");
		std::array<std::byte, RSS_ENVELOPE_BYTES> envelopeBytes{};
		std::copy_n(poolPage.begin(), envelopeBytes.size(), envelopeBytes.begin());
		const SegmentEnvelope envelope = SegmentEnvelopeCodec::decode(envelopeBytes);
		const uint8_t invalidEnvelope = static_cast<uint8_t>(envelope.state != SegmentState::SGS_SEALED) |
										static_cast<uint8_t>(envelope.header_bytes != RSS_PAGE_SIZE) |
										static_cast<uint8_t>(envelope.kind != pool.kind) |
										static_cast<uint8_t>(envelope.segment_id != pool.segment_id) |
										static_cast<uint8_t>(envelope.owner_id != pool.owner_id) |
										static_cast<uint8_t>(envelope.generation != pool.generation) |
										static_cast<uint8_t>(envelope.committed_lsn != pool.committed_lsn);
		if (invalidEnvelope != 0U) {
			throwSharedPagePoolAllocatorError(
					FormatError::FERR_INVALID_STATE,
					"relocatable shared page-pool envelope disagrees with its Catalog mapping: expected " +
							std::to_string(pool.segment_id.value()) + " at slot " +
							std::to_string(pool.physical_slot.value()) + ", found " +
							std::to_string(envelope.segment_id.value()) + " (kind " +
							std::to_string(static_cast<uint32_t>(envelope.kind)) + ")");
		}
		const SharedPagePoolHeader poolHeader = selectPoolControlHeader(poolPage).header;
		const uint8_t invalidPoolHeader =
				static_cast<uint8_t>(poolHeader.segment_id != pool.segment_id) |
				static_cast<uint8_t>(poolHeader.generation != pool.generation) |
				static_cast<uint8_t>(poolHeader.committed_lsn != pool.committed_lsn) |
				static_cast<uint8_t>(!SharedPagePoolFrameMap::isAllocated(poolHeader, location.page_index));
		if (invalidPoolHeader != 0U) {
			throwSharedPagePoolAllocatorError(
					FormatError::FERR_INVALID_STATE,
					"relocatable shared page-pool header disagrees with its Page Directory mapping");
		}
		const uint64_t pageOffset = poolOffset + static_cast<uint64_t>(location.page_index) * RSS_PAGE_SIZE;
		const std::array<std::byte, RSS_PAGE_SIZE> pageBytes = readTypedPage(
				*io_, pageOffset, location.generation, "relocatable shared page owner header is truncated", hint);
		std::array<std::byte, RSS_PAGE_OWNER_HEADER_BYTES> ownerBytes{};
		std::copy_n(pageBytes.begin(), ownerBytes.size(), ownerBytes.begin());
		const PageOwnerHeader owner = PageOwnerHeaderCodec::decode(ownerBytes);
		if (owner.page_id != location.page_id || owner.generation != location.generation ||
			!matchesPoolAndOwnerKind(pool.kind, owner.kind)) {
			throwSharedPagePoolAllocatorError(
					FormatError::FERR_INVALID_STATE,
					"relocatable shared page owner header disagrees with its location mapping");
		}
		std::vector<std::byte> payload(owner.payload_bytes);
		std::copy_n(pageBytes.begin() + RSS_PAGE_OWNER_HEADER_BYTES, payload.size(), payload.begin());
		return {pool, location, poolHeader, owner, std::move(payload)};
	}

	std::optional<SharedPagePoolPinnedView>
	SharedPagePoolAllocator::readPinned(const MetadataSnapshotPin &snapshot, const PageId pageId,
														  const BufferPoolReadHint hint) const {
		if (!isKnownReadHint(hint)) {
			throwSharedPagePoolAllocatorError(FormatError::FERR_INVALID_ENUM,
																 "relocatable shared pinned page read has an unknown Buffer Pool hint");
		}
		if (debug::PerfTrace::areValuesEnabled()) {
			debug::PerfTrace::addValue("native_graph.read.shared_page_route_resolution", 1);
		}
		const PageLocation location = metadata_.findPageLocation(snapshot, pageId);
		const CatalogEntry pool = metadata_.findCatalog(snapshot, location.segment_id);
		return readPinnedKnownLocation(snapshot, pool, location, hint);
	}

	std::optional<SharedPagePoolPinnedView> SharedPagePoolAllocator::readPinnedKnownLocation(
			const MetadataSnapshotPin &snapshot, const CatalogEntry &pool, const PageLocation &location,
			const BufferPoolReadHint hint) const {
		if (!isKnownReadHint(hint)) {
			throwSharedPagePoolAllocatorError(FormatError::FERR_INVALID_ENUM,
					"relocatable shared pinned known-location read has an unknown Buffer Pool hint");
		}
		if (debug::PerfTrace::areValuesEnabled()) {
			debug::PerfTrace::addValue("native_graph.read.shared_page_known_location_pin", 1);
		}
		const uint8_t invalidLocation =
				static_cast<uint8_t>(!metadata_.ownsSnapshot(snapshot)) |
				static_cast<uint8_t>(!location.page_id.isValid()) |
				static_cast<uint8_t>(location.segment_id != pool.segment_id) |
				static_cast<uint8_t>(pool.state != SegmentState::SGS_SEALED) |
				static_cast<uint8_t>(pool.generation != location.generation) |
				static_cast<uint8_t>(!isSharedPoolKind(pool.kind)) |
				static_cast<uint8_t>(!metadata_.isPhysicalSlotAllocated(snapshot, pool.physical_slot));
		if (invalidLocation != 0U) {
			throwSharedPagePoolAllocatorError(
					FormatError::FERR_INVALID_STATE,
					"relocatable shared pinned page location does not reference an allocated pool generation");
		}
		const uint64_t poolOffset =
				PhysicalSegmentAllocator::checkedSlotOffset(data_region_offset_, pool.physical_slot);
		const BufferPoolPageKey poolKey{poolOffset, pool.generation.value()};
		std::optional<BufferPool::Pin> poolPin = io_->pinTypedPage(poolKey);
		if (!poolPin.has_value()) {
			return std::nullopt;
		}
		if (poolPin->validBytes() != RSS_PAGE_SIZE) {
			throwSharedPagePoolAllocatorError(FormatError::FERR_BUFFER_TOO_SMALL,
														 "relocatable shared pinned page-pool envelope is truncated");
		}
		const std::span<const std::byte, RSS_PAGE_SIZE> poolBytes = poolPin->bytes();
		const SegmentEnvelope envelope = SegmentEnvelopeCodec::decode(poolBytes.first(RSS_ENVELOPE_BYTES));
		const uint8_t invalidEnvelope = static_cast<uint8_t>(envelope.state != SegmentState::SGS_SEALED) |
				static_cast<uint8_t>(envelope.header_bytes != RSS_PAGE_SIZE) |
				static_cast<uint8_t>(envelope.kind != pool.kind) |
				static_cast<uint8_t>(envelope.segment_id != pool.segment_id) |
				static_cast<uint8_t>(envelope.owner_id != pool.owner_id) |
				static_cast<uint8_t>(envelope.generation != pool.generation) |
				static_cast<uint8_t>(envelope.committed_lsn != pool.committed_lsn);
		if (invalidEnvelope != 0U) {
			throwSharedPagePoolAllocatorError(
					FormatError::FERR_INVALID_STATE,
					"relocatable shared pinned page-pool envelope disagrees with its Catalog mapping");
		}
		const SharedPagePoolHeader poolHeader = selectPoolControlHeader(poolBytes).header;
		const uint8_t invalidPoolHeader = static_cast<uint8_t>(poolHeader.segment_id != pool.segment_id) |
				static_cast<uint8_t>(poolHeader.generation != pool.generation) |
				static_cast<uint8_t>(poolHeader.committed_lsn != pool.committed_lsn) |
				static_cast<uint8_t>(!SharedPagePoolFrameMap::isAllocated(poolHeader, location.page_index));
		if (invalidPoolHeader != 0U) {
			throwSharedPagePoolAllocatorError(
					FormatError::FERR_INVALID_STATE,
					"relocatable shared pinned page-pool header disagrees with its Page Directory mapping");
		}
		poolPin.reset();

		const uint64_t pageOffset = poolOffset + static_cast<uint64_t>(location.page_index) * RSS_PAGE_SIZE;
		std::optional<BufferPool::Pin> pagePin =
				io_->pinTypedPage({pageOffset, location.generation.value()}, hint);
		if (!pagePin.has_value()) {
			return std::nullopt;
		}
		if (pagePin->validBytes() != RSS_PAGE_SIZE) {
			throwSharedPagePoolAllocatorError(FormatError::FERR_BUFFER_TOO_SMALL,
														 "relocatable shared pinned page owner header is truncated");
		}
		const std::span<const std::byte, RSS_PAGE_SIZE> pageBytes = pagePin->bytes();
		const PageOwnerHeader owner = PageOwnerHeaderCodec::decode(pageBytes.first(RSS_PAGE_OWNER_HEADER_BYTES));
		if (owner.page_id != location.page_id || owner.generation != location.generation ||
			!matchesPoolAndOwnerKind(pool.kind, owner.kind)) {
			throwSharedPagePoolAllocatorError(
					FormatError::FERR_INVALID_STATE,
					"relocatable shared pinned page owner header disagrees with its location mapping");
		}
		if (owner.payload_bytes > RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES) {
			throwSharedPagePoolAllocatorError(FormatError::FERR_INVALID_HEADER,
														 "relocatable shared pinned page owner has an oversized payload");
		}
		return SharedPagePoolPinnedView{pool, location, poolHeader, owner, std::move(*pagePin)};
	}

	void SharedPagePoolAllocator::prefetch(const MetadataSnapshotPin &snapshot, std::span<const PageId> pageIds,
										   BufferPoolReadHint admissionHint) const {
		if (!isKnownReadHint(admissionHint)) {
			throwSharedPagePoolAllocatorError(FormatError::FERR_INVALID_ENUM,
											  "relocatable shared page prefetch has an unknown Buffer Pool hint");
		}
		if (!snapshot.isValid() || pageIds.empty()) {
			throwSharedPagePoolAllocatorError(FormatError::FERR_INVALID_HEADER,
											  "relocatable shared page prefetch requires a snapshot and Page IDs");
		}
		std::vector<BufferPoolPageKey> keys;
		keys.reserve(pageIds.size());
		for (const PageId pageId: pageIds) {
			if (!pageId.isValid()) {
				throwSharedPagePoolAllocatorError(FormatError::FERR_INVALID_HEADER,
												  "relocatable shared page prefetch has an invalid Page ID");
			}
			const PageLocation location = metadata_.findPageLocation(snapshot, pageId);
			const CatalogEntry pool = metadata_.findCatalog(snapshot, location.segment_id);
			const uint8_t invalid =
					static_cast<uint8_t>(pool.state != SegmentState::SGS_SEALED) |
					static_cast<uint8_t>(pool.generation != location.generation) |
					static_cast<uint8_t>(!isSharedPoolKind(pool.kind)) |
					static_cast<uint8_t>(!metadata_.isPhysicalSlotAllocated(snapshot, pool.physical_slot));
			// ZYX_COV_EXCL_START: Page and Catalog lookups are from the same validated snapshot; malformed durable
			// routes fail in those codecs before this defensive cross-check.
			if (invalid != 0U) {
				throwSharedPagePoolAllocatorError(
						FormatError::FERR_INVALID_STATE,
						"relocatable shared page prefetch route is not a current allocated pool frame");
			}
			// ZYX_COV_EXCL_STOP
			const uint64_t poolOffset =
					PhysicalSegmentAllocator::checkedSlotOffset(data_region_offset_, pool.physical_slot);
			keys.push_back({poolOffset + static_cast<uint64_t>(location.page_index) * RSS_PAGE_SIZE,
							location.generation.value()});
		}
		io_->prefetchPages(keys, admissionHint);
	}

	uint8_t SharedPagePoolAllocator::availableFrameCount(const CatalogEntry &pool) const {
		return availableFrameCountUnderWrite(*io_, data_region_offset_, pool);
	}

	uint8_t SharedPagePoolAllocator::availableFrameCountUnderWrite(const StorageIO &io, uint64_t dataRegionOffset,
																   const CatalogEntry &pool) {
		const uint8_t invalidPool = static_cast<uint8_t>(pool.state != SegmentState::SGS_SEALED) |
									static_cast<uint8_t>(!isSharedPoolKind(pool.kind));
		if (invalidPool != 0U) {
			throwSharedPagePoolAllocatorError(
					FormatError::FERR_INVALID_STATE,
					"relocatable shared page-pool capacity query requires one current sealed pool");
		}
		const SharedPagePoolHeader header = readPoolHeader(io, dataRegionOffset, pool);
		return static_cast<uint8_t>(RSS_PAGE_POOL_DATA_FRAME_COUNT - std::popcount(header.allocated_frames));
	}

	void SharedPagePoolAllocator::reconcilePool(SegmentId segmentId) const {
		metadata_.reconcileExistingPool(
				segmentId, [this](const CatalogEntry &pool,
								  const std::function<std::optional<PageLocation>(PageId)> &findPageLocation,
								  const std::function<CatalogEntry(SegmentId)> &findCatalog,
								  const ExistingPoolFrameVisibility &isFrameVisibleToActiveSnapshot) {
					const uint32_t framesBefore = debug::PerfTrace::areValuesEnabled()
							? std::popcount(readPoolHeader(pool).allocated_frames)
							: 0U;
					const bool repaired = reconcilePoolFrames(pool, findPageLocation, findCatalog,
							isFrameVisibleToActiveSnapshot);
					if (debug::PerfTrace::areValuesEnabled()) {
						const uint32_t framesAfter = std::popcount(readPoolHeader(pool).allocated_frames);
						debug::PerfTrace::addValue("native_graph.write.recovery.pool_reconcile_calls", 1);
						debug::PerfTrace::addValue("native_graph.write.recovery.frames_scanned",
								static_cast<int64_t>(framesBefore));
						debug::PerfTrace::addValue("native_graph.write.recovery.frames_released",
								static_cast<int64_t>(framesBefore >= framesAfter ? framesBefore - framesAfter : 0U));
						if (repaired) {
							debug::PerfTrace::addValue("native_graph.write.recovery.syncs", 1);
						}
					}
					if (repaired) {
						// The repaired frame map must reach stable storage before a future
						// append can reuse a frame which was unreachable at recovery time.
						io_->sync();
					}
				});
	}

	void SharedPagePoolAllocator::reclaimRetiredFrames(SegmentId segmentId) const { reconcilePool(segmentId); }

	Superblock SharedPagePoolAllocator::retireEmptyPool(SegmentId segmentId, Generation expectedGeneration,
														LogSequenceNumber checkpointLsn,
														const MetadataPublishOptions &options) {
		reconcilePool(segmentId);
		return metadata_.retireCatalogSegment(segmentId, expectedGeneration, checkpointLsn, options);
	}

	Superblock SharedPagePoolAllocator::releaseRetiredEmptyPool(SegmentId segmentId, Generation expectedGeneration,
																LogSequenceNumber checkpointLsn,
																const MetadataPublishOptions &options) {
		return metadata_.releaseRetiredCatalogSegment(segmentId, expectedGeneration, checkpointLsn, options);
	}

	void SharedPagePoolAllocator::reconcileCurrentPools() const {
		metadata_.reconcileCurrentPools([this](const CatalogEntry &pool, std::span<const PageLocation> pageLocations) {
			if (!isSharedPoolKind(pool.kind)) {
				return;
			}
			const bool repaired = reconcilePoolFramesAtStartup(pool, pageLocations);
			if (debug::PerfTrace::areValuesEnabled()) {
				debug::PerfTrace::addValue("native_graph.write.recovery.startup_pool_reconcile_calls", 1);
				debug::PerfTrace::addValue("native_graph.write.recovery.startup_directory_pages",
						static_cast<int64_t>(pageLocations.size()));
				if (repaired) {
					debug::PerfTrace::addValue("native_graph.write.recovery.startup_syncs", 1);
				}
			}
			if (repaired) {
				// Persist each safe repair before a later corrupt pool can abort the
				// scan; this prevents a recovered frame from becoming ambiguous again.
				io_->sync();
			}
		});
	}

	void SharedPagePoolAllocator::validateRequest(const SharedPagePoolAllocationRequest &request,
												  LogSequenceNumber checkpointLsn) const {
		const uint8_t invalid =
				static_cast<uint8_t>(!request.segment_id.isValid()) |
				static_cast<uint8_t>(!request.generation.isValid()) | static_cast<uint8_t>(!request.page_id.isValid()) |
				static_cast<uint8_t>(!checkpointLsn.isValid()) |
				static_cast<uint8_t>(request.payload.size() > RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES) |
				static_cast<uint8_t>(request.page_free_bytes > request.payload.size()) |
				static_cast<uint8_t>(!matchesPoolAndOwnerKind(request.pool_kind, request.page_owner_kind));
		if (invalid != 0U) {
			throwSharedPagePoolAllocatorError(FormatError::FERR_INVALID_HEADER,
											  "relocatable shared page-pool allocation request is invalid");
		}
	}

	void SharedPagePoolAllocator::validateAppendRequest(const SharedPagePoolAppendRequest &request,
														const CatalogEntry &pool,
														LogSequenceNumber checkpointLsn) const {
		const uint8_t invalid = static_cast<uint8_t>(!request.segment_id.isValid()) |
								static_cast<uint8_t>(!request.generation.isValid()) |
								static_cast<uint8_t>(!request.page_id.isValid()) |
								static_cast<uint8_t>(!checkpointLsn.isValid()) |
								static_cast<uint8_t>(request.payload.size() > RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES) |
								static_cast<uint8_t>(request.page_free_bytes > request.payload.size()) |
								static_cast<uint8_t>(pool.state != SegmentState::SGS_SEALED) |
								static_cast<uint8_t>(pool.generation != request.generation) |
								static_cast<uint8_t>(!matchesPoolAndOwnerKind(pool.kind, request.page_owner_kind));
		if (invalid != 0U) {
			throwSharedPagePoolAllocatorError(FormatError::FERR_INVALID_HEADER,
											  "relocatable shared page-pool append request is invalid");
		}
	}

	void SharedPagePoolAllocator::validateReplacementRequest(const SharedPagePoolReplacementRequest &request,
															 const CatalogEntry &pool,
															 LogSequenceNumber checkpointLsn) const {
		const uint8_t invalid = static_cast<uint8_t>(!request.segment_id.isValid()) |
								static_cast<uint8_t>(!request.generation.isValid()) |
								static_cast<uint8_t>(!request.page_id.isValid()) |
								static_cast<uint8_t>(!request.expected_owner_lsn.isValid()) |
								static_cast<uint8_t>(!checkpointLsn.isValid()) |
								static_cast<uint8_t>(request.payload.size() > RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES) |
								static_cast<uint8_t>(request.page_free_bytes > request.payload.size()) |
								static_cast<uint8_t>(pool.state != SegmentState::SGS_SEALED) |
								static_cast<uint8_t>(pool.generation != request.generation) |
								static_cast<uint8_t>(!matchesPoolAndOwnerKind(pool.kind, request.page_owner_kind));
		if (invalid != 0U) {
			throwSharedPagePoolAllocatorError(FormatError::FERR_INVALID_HEADER,
											  "relocatable shared page-pool replacement request is invalid");
		}
	}

	void SharedPagePoolAllocator::validateEvacuationRequest(const SharedPagePoolEvacuationRequest &request,
															const CatalogEntry &sourcePool,
															const CatalogEntry &destinationPool,
															LogSequenceNumber checkpointLsn) const {
		const uint8_t invalid =
				static_cast<uint8_t>(!request.source_segment_id.isValid()) |
				static_cast<uint8_t>(!request.source_generation.isValid()) |
				static_cast<uint8_t>(!request.destination_segment_id.isValid()) |
				static_cast<uint8_t>(!request.destination_generation.isValid()) |
				static_cast<uint8_t>(request.source_segment_id == request.destination_segment_id) |
				static_cast<uint8_t>(!request.page_id.isValid()) |
				static_cast<uint8_t>(!request.expected_owner_lsn.isValid()) |
				static_cast<uint8_t>(!checkpointLsn.isValid()) |
				static_cast<uint8_t>(request.payload.size() > RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES) |
				static_cast<uint8_t>(request.page_free_bytes > request.payload.size()) |
				static_cast<uint8_t>(sourcePool.state != SegmentState::SGS_SEALED) |
				static_cast<uint8_t>(destinationPool.state != SegmentState::SGS_SEALED) |
				static_cast<uint8_t>(sourcePool.generation != request.source_generation) |
				static_cast<uint8_t>(destinationPool.generation != request.destination_generation) |
				static_cast<uint8_t>(sourcePool.kind != destinationPool.kind) |
				static_cast<uint8_t>(!matchesPoolAndOwnerKind(sourcePool.kind, request.page_owner_kind)) |
				static_cast<uint8_t>(!matchesPoolAndOwnerKind(destinationPool.kind, request.page_owner_kind));
		if (invalid != 0U) {
			throwSharedPagePoolAllocatorError(FormatError::FERR_INVALID_HEADER,
											  "relocatable shared page-pool evacuation request is invalid");
		}
	}

	SharedPagePoolHeader SharedPagePoolAllocator::readPoolHeader(const CatalogEntry &entry) const {
		return readPoolHeader(*io_, data_region_offset_, entry);
	}

	SharedPagePoolHeader SharedPagePoolAllocator::readPoolHeader(const StorageIO &io, uint64_t dataRegionOffset,
																 const CatalogEntry &entry) {
		const uint64_t offset = PhysicalSegmentAllocator::checkedSlotOffset(dataRegionOffset, entry.physical_slot);
		const std::array<std::byte, RSS_PAGE_SIZE> poolPage =
				readTypedPage(io, offset, entry.generation, "relocatable shared page-pool envelope is truncated");
		std::array<std::byte, RSS_ENVELOPE_BYTES> envelopeBytes{};
		std::copy_n(poolPage.begin(), envelopeBytes.size(), envelopeBytes.begin());
		const SegmentEnvelope envelope = SegmentEnvelopeCodec::decode(envelopeBytes);
		if (envelope.state != SegmentState::SGS_SEALED || envelope.header_bytes != RSS_PAGE_SIZE ||
			envelope.kind != entry.kind || envelope.segment_id != entry.segment_id ||
			envelope.owner_id != entry.owner_id || envelope.generation != entry.generation ||
			envelope.committed_lsn != entry.committed_lsn) {
			throwSharedPagePoolAllocatorError(
					FormatError::FERR_INVALID_STATE,
					"relocatable shared page-pool envelope disagrees with its Catalog mapping: expected " +
							std::to_string(entry.segment_id.value()) + " at slot " +
							std::to_string(entry.physical_slot.value()) + ", found " +
							std::to_string(envelope.segment_id.value()) + " (kind " +
							std::to_string(static_cast<uint32_t>(envelope.kind)) + ")");
		}
		const SharedPagePoolHeader header = selectPoolControlHeader(poolPage).header;
		if (header.segment_id != entry.segment_id || header.generation != entry.generation ||
			header.committed_lsn != entry.committed_lsn) {
			throwSharedPagePoolAllocatorError(FormatError::FERR_INVALID_STATE,
											  "relocatable shared page-pool header disagrees with its Catalog mapping");
		}
		return header;
	}

	PageOwnerHeader SharedPagePoolAllocator::readPageOwner(const CatalogEntry &entry, uint8_t frameIndex) const {
		const uint64_t poolOffset =
				PhysicalSegmentAllocator::checkedSlotOffset(data_region_offset_, entry.physical_slot);
		const uint64_t pageOffset = poolOffset + static_cast<uint64_t>(frameIndex) * RSS_PAGE_SIZE;
		const std::array<std::byte, RSS_PAGE_SIZE> page =
				readTypedPage(*io_, pageOffset, entry.generation, "relocatable shared page owner header is truncated");
		std::array<std::byte, RSS_PAGE_OWNER_HEADER_BYTES> bytes{};
		std::copy_n(page.begin(), bytes.size(), bytes.begin());
		return PageOwnerHeaderCodec::decode(bytes);
	}

	bool SharedPagePoolAllocator::reconcilePoolFrames(
			const CatalogEntry &entry, const std::function<std::optional<PageLocation>(PageId)> &findPageLocation,
			const std::function<CatalogEntry(SegmentId)> &findCatalog,
			const ExistingPoolFrameVisibility &isFrameVisibleToActiveSnapshot) const {
		// ZYX_COV_EXCL_START: both reconciliation entry points filter Catalog kinds before invoking this helper.
		if (!isSharedPoolKind(entry.kind)) {
			throwSharedPagePoolAllocatorError(
					FormatError::FERR_INVALID_STATE,
					"relocatable pool reconciliation requires a shared pool and Page Directory lookup");
		}
		// ZYX_COV_EXCL_STOP
		SharedPagePoolHeader header = readPoolHeader(entry);
		SharedPagePoolHeader reconciled = header;
		const uint64_t poolOffset =
				PhysicalSegmentAllocator::checkedSlotOffset(data_region_offset_, entry.physical_slot);
		struct FrameState {
			uint8_t frame_index = 0;
			PageOwnerHeader owner;
			std::optional<PageLocation> location;
		};
		std::vector<FrameState> frames;
		frames.reserve(RSS_PAGE_POOL_DATA_FRAME_COUNT);
		for (uint8_t frameIndex = RSS_PAGE_POOL_FIRST_DATA_FRAME; frameIndex <= RSS_PAGE_POOL_DATA_FRAME_COUNT;
			 ++frameIndex) {
			if (!SharedPagePoolFrameMap::isAllocated(header, frameIndex)) {
				continue;
			}
			const uint64_t pageOffset = poolOffset + static_cast<uint64_t>(frameIndex) * RSS_PAGE_SIZE;
			const std::array<std::byte, RSS_PAGE_SIZE> page = readTypedPage(
					*io_, pageOffset, entry.generation, "relocatable reconciled shared page owner header is truncated");
			std::array<std::byte, RSS_PAGE_OWNER_HEADER_BYTES> ownerBytes{};
			std::copy_n(page.begin(), ownerBytes.size(), ownerBytes.begin());
			const PageOwnerHeader owner = PageOwnerHeaderCodec::decode(ownerBytes);
			frames.push_back({frameIndex, owner, findPageLocation(owner.page_id)});
		}
		const auto isCompatibleLocation = [&entry](const PageLocation &location, const PageOwnerHeader &owner) {
			const uint8_t invalid = static_cast<uint8_t>(location.segment_id != entry.segment_id) |
									static_cast<uint8_t>(location.generation != entry.generation) |
									static_cast<uint8_t>(owner.generation != entry.generation) |
									static_cast<uint8_t>(!matchesPoolAndOwnerKind(entry.kind, owner.kind));
			return invalid == 0U;
		};
		const auto currentFrameIsCompatibleReplacement =
				[&frames, &header, &isCompatibleLocation](const PageLocation &location, PageId pageId) {
					const uint8_t invalidLocation =
							static_cast<uint8_t>(location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
							static_cast<uint8_t>(location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
							static_cast<uint8_t>(!SharedPagePoolFrameMap::isAllocated(header, location.page_index));
					if (invalidLocation != 0U) {
						return false;
					}
					const auto replacement = std::ranges::find(frames, location.page_index, &FrameState::frame_index);
					return replacement != frames.end() && replacement->owner.page_id == pageId &&
						   replacement->owner.flags == 0U && isCompatibleLocation(location, replacement->owner);
				};
		std::vector<uint8_t> reactivateFrames;
		const auto canReclaimRetiredFrame = [&entry, &isFrameVisibleToActiveSnapshot](const FrameState &frame) {
			return !isFrameVisibleToActiveSnapshot(frame.owner.page_id, entry.segment_id, frame.frame_index,
												   entry.generation);
		};
		for (const FrameState &frame: frames) {
			if (!frame.location.has_value()) {
				if (canReclaimRetiredFrame(frame)) {
					SharedPagePoolFrameMap::release(reconciled, frame.frame_index);
				}
				continue;
			}
			if (frame.location->segment_id != entry.segment_id) {
				const CatalogEntry remotePool = findCatalog(frame.location->segment_id);
				const uint8_t invalidRemote =
						static_cast<uint8_t>(!isSharedPoolKind(remotePool.kind)) |
						static_cast<uint8_t>(remotePool.kind != entry.kind) |
						static_cast<uint8_t>(remotePool.generation != frame.location->generation) |
						static_cast<uint8_t>(!matchesPoolAndOwnerKind(remotePool.kind, frame.owner.kind));
				if (invalidRemote != 0U) {
					throwSharedPagePoolAllocatorError(
							FormatError::FERR_INVALID_STATE,
							"relocatable shared page recovery found an invalid remote pool mapping");
				}
				// A remote current mapping is either a published cross-pool move whose
				// local source is retired, or an unpublished destination frame. The
				// Page Directory is the sole reachability authority in both cases.
				if (canReclaimRetiredFrame(frame)) {
					SharedPagePoolFrameMap::release(reconciled, frame.frame_index);
				}
				continue;
			}
			if (!isCompatibleLocation(*frame.location, frame.owner)) {
				throwSharedPagePoolAllocatorError(
						FormatError::FERR_INVALID_STATE,
						"relocatable shared page recovery found an incompatible Page Directory location");
			}
			if (frame.location->page_index == frame.frame_index) {
				if ((frame.owner.flags & RSS_PAGE_OWNER_FLAG_RETIRED) != 0U) {
					reactivateFrames.push_back(frame.frame_index);
				}
				continue;
			}
			if (!currentFrameIsCompatibleReplacement(*frame.location, frame.owner.page_id)) {
				throwSharedPagePoolAllocatorError(
						FormatError::FERR_INVALID_STATE,
						"relocatable shared page recovery found an invalid replacement frame");
			}
			// The current directory selects a different COW frame for this Page ID.
			// Keep the old immutable owner only while an older snapshot can still
			// resolve it; otherwise its allocation bit is safely reusable.
			if (canReclaimRetiredFrame(frame)) {
				SharedPagePoolFrameMap::release(reconciled, frame.frame_index);
			}
		}
		for (const uint8_t frameIndex: reactivateFrames) {
			const auto frame = std::ranges::find(frames, frameIndex, &FrameState::frame_index);
			if (frame == frames.end()) {
				throwSharedPagePoolAllocatorError(FormatError::FERR_INVALID_STATE,
												  "relocatable shared page recovery lost a reachable frame record");
			}
			PageOwnerHeader active = frame->owner;
			active.flags = 0U;
			const auto activeBytes = PageOwnerHeaderCodec::encode(active);
			const uint64_t pageOffset = poolOffset + static_cast<uint64_t>(frameIndex) * RSS_PAGE_SIZE;
			rewriteTypedPageRange(*io_, pageOffset, entry.generation, 0U, activeBytes,
								  "relocatable reactivated shared page is truncated");
		}
		if (reconciled.allocated_frames == header.allocated_frames) {
			return !reactivateFrames.empty();
		}
		rewriteInactivePoolControlRecord(*io_, poolOffset, entry.generation, reconciled,
										 "relocatable reconciled shared page-pool control page is truncated");
		return true;
	}

	bool SharedPagePoolAllocator::reconcilePoolFramesAtStartup(const CatalogEntry &entry,
															   std::span<const PageLocation> pageLocations) const {
		// ZYX_COV_EXCL_START: reconcileCurrentPools filters non-shared Catalog entries before invoking this
		// startup-only helper.
		if (!isSharedPoolKind(entry.kind)) {
			throwSharedPagePoolAllocatorError(FormatError::FERR_INVALID_STATE,
											  "relocatable startup pool reconciliation requires a shared pool");
		}
		// ZYX_COV_EXCL_STOP
		const SharedPagePoolHeader header = readPoolHeader(entry);
		SharedPagePoolHeader reconciled = header;
		std::array<std::optional<PageLocation>, RSS_PAGE_POOL_DATA_FRAME_COUNT + 1U> locationsByFrame;
		for (const PageLocation &location: pageLocations) {
			const uint8_t invalidLocation = static_cast<uint8_t>(location.segment_id != entry.segment_id) |
											static_cast<uint8_t>(location.generation != entry.generation) |
											static_cast<uint8_t>(location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
											static_cast<uint8_t>(location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
											static_cast<uint8_t>(locationsByFrame[location.page_index].has_value());
			if (invalidLocation != 0U) {
				throwSharedPagePoolAllocatorError(FormatError::FERR_INVALID_STATE,
												  "relocatable Page Directory does not map pool frames one-to-one");
			}
			locationsByFrame[location.page_index] = location;
		}
		for (uint8_t frameIndex = RSS_PAGE_POOL_FIRST_DATA_FRAME; frameIndex <= RSS_PAGE_POOL_DATA_FRAME_COUNT;
			 ++frameIndex) {
			if (locationsByFrame[frameIndex].has_value() && !SharedPagePoolFrameMap::isAllocated(header, frameIndex)) {
				throwSharedPagePoolAllocatorError(FormatError::FERR_INVALID_STATE,
												  "relocatable Page Directory references an unallocated pool frame");
			}
		}
		const uint64_t poolOffset =
				PhysicalSegmentAllocator::checkedSlotOffset(data_region_offset_, entry.physical_slot);
		const bool canReclaimRetired = metadata_.activeSnapshotCount() == 0U;
		std::vector<uint8_t> reactivateFrames;
		for (uint8_t frameIndex = RSS_PAGE_POOL_FIRST_DATA_FRAME; frameIndex <= RSS_PAGE_POOL_DATA_FRAME_COUNT;
			 ++frameIndex) {
			if (!SharedPagePoolFrameMap::isAllocated(header, frameIndex)) {
				continue;
			}
			const std::optional<PageLocation> &location = locationsByFrame[frameIndex];
			const PageOwnerHeader owner = readPageOwner(entry, frameIndex);
			if (location.has_value()) {
				const uint8_t invalidMappedOwner =
						static_cast<uint8_t>(owner.page_id != location->page_id) |
						static_cast<uint8_t>(owner.generation != entry.generation) |
						static_cast<uint8_t>(!matchesPoolAndOwnerKind(entry.kind, owner.kind));
				if (invalidMappedOwner != 0U) {
					throwSharedPagePoolAllocatorError(
							FormatError::FERR_INVALID_STATE,
							"relocatable shared page owner disagrees with the current Page Directory frame");
				}
				if ((owner.flags & RSS_PAGE_OWNER_FLAG_RETIRED) != 0U) {
					reactivateFrames.push_back(frameIndex);
				}
				continue;
			}
			if (canReclaimRetired) {
				SharedPagePoolFrameMap::release(reconciled, frameIndex);
			}
		}
		for (const uint8_t frameIndex: reactivateFrames) {
			const PageOwnerHeader retired = readPageOwner(entry, frameIndex);
			PageOwnerHeader active = retired;
			active.flags = 0;
			const auto activeBytes = PageOwnerHeaderCodec::encode(active);
			const uint64_t pageOffset = poolOffset + static_cast<uint64_t>(frameIndex) * RSS_PAGE_SIZE;
			rewriteTypedPageRange(*io_, pageOffset, entry.generation, 0U, activeBytes,
								  "relocatable startup-reactivated shared page is truncated");
		}
		if (reconciled.allocated_frames == header.allocated_frames) {
			return !reactivateFrames.empty();
		}
		rewriteInactivePoolControlRecord(*io_, poolOffset, entry.generation, reconciled,
										 "relocatable startup-reconciled shared page-pool control page is truncated");
		return true;
	}

	void SharedPagePoolAllocator::writePreparedPage(const CatalogEntry &entry, const SharedPagePoolHeader &poolHeader,
													const PageOwnerHeader &owner, std::span<const std::byte> payload,
													uint8_t frameIndex) const {
		const uint8_t invalid = static_cast<uint8_t>(!SharedPagePoolFrameMap::isAllocated(poolHeader, frameIndex)) |
								static_cast<uint8_t>(payload.size() != owner.payload_bytes);
		if (invalid != 0U) { // ZYX_COV_EXCL_LINE: private writers derive frame and payload from one validated request.
			throwSharedPagePoolAllocatorError(
					FormatError::FERR_INVALID_STATE,
					"relocatable shared page append has inconsistent frame or payload metadata");
		}
		const uint64_t offset = PhysicalSegmentAllocator::checkedSlotOffset(data_region_offset_, entry.physical_slot);
		const uint64_t pageOffset = offset + static_cast<uint64_t>(frameIndex) * RSS_PAGE_SIZE;
		writeTypedPage(*io_, pageOffset, entry.generation, makeOwnedPage(owner, payload));
		// The candidate must be complete before its allocation bit becomes durable.
		// Otherwise a failed page write leaves recovery unable to distinguish a torn
		// unpublished owner from corruption in a previously allocated frame.
		rewriteInactivePoolControlRecord(*io_, offset, entry.generation, poolHeader,
										 "relocatable shared page-pool control page is truncated");
	}

	void SharedPagePoolAllocator::writePreparedReplacement(const CatalogEntry &entry,
														   const SharedPagePoolHeader &poolHeader,
														   const PageOwnerHeader &oldOwner, uint8_t oldFrame,
														   const PageOwnerHeader &newOwner,
														   std::span<const std::byte> payload, uint8_t newFrame) const {
		const uint8_t invalid = static_cast<uint8_t>(oldFrame == newFrame) |
								static_cast<uint8_t>(!SharedPagePoolFrameMap::isAllocated(poolHeader, oldFrame)) |
								static_cast<uint8_t>(!SharedPagePoolFrameMap::isAllocated(poolHeader, newFrame)) |
								static_cast<uint8_t>(oldOwner.flags != 0U) |
								static_cast<uint8_t>(newOwner.flags != 0U) |
								static_cast<uint8_t>(oldOwner.page_id != newOwner.page_id) |
								static_cast<uint8_t>(payload.size() != newOwner.payload_bytes);
		if (invalid !=
			0U) { // ZYX_COV_EXCL_LINE: private replacement preparation derives every field from validated state.
			throwSharedPagePoolAllocatorError(
					FormatError::FERR_INVALID_STATE,
					"relocatable shared page replacement has inconsistent frame or owner metadata");
		}
		const uint64_t poolOffset =
				PhysicalSegmentAllocator::checkedSlotOffset(data_region_offset_, entry.physical_slot);
		const uint64_t newPageOffset = poolOffset + static_cast<uint64_t>(newFrame) * RSS_PAGE_SIZE;
		writeTypedPage(*io_, newPageOffset, entry.generation, makeOwnedPage(newOwner, payload));
		static_cast<void>(oldOwner);
		static_cast<void>(oldFrame);
		rewriteInactivePoolControlRecord(*io_, poolOffset, entry.generation, poolHeader,
										 "relocatable shared page-pool control page is truncated");
	}

	void SharedPagePoolAllocator::writePreparedCrossPoolEvacuation(
			const CatalogEntry &sourcePool, const SharedPagePoolHeader &sourceHeader,
			const PageOwnerHeader &sourceOwner, uint8_t sourceFrame, const CatalogEntry &destinationPool,
			const SharedPagePoolHeader &destinationHeader, const PageOwnerHeader &destinationOwner,
			std::span<const std::byte> payload, uint8_t destinationFrame) const {
		const uint8_t invalid =
				static_cast<uint8_t>(sourcePool.physical_slot == destinationPool.physical_slot) |
				static_cast<uint8_t>(!SharedPagePoolFrameMap::isAllocated(sourceHeader, sourceFrame)) |
				static_cast<uint8_t>(!SharedPagePoolFrameMap::isAllocated(destinationHeader, destinationFrame)) |
				static_cast<uint8_t>(sourceOwner.flags != 0U) | static_cast<uint8_t>(destinationOwner.flags != 0U) |
				static_cast<uint8_t>(sourceOwner.page_id != destinationOwner.page_id) |
				static_cast<uint8_t>(payload.size() != destinationOwner.payload_bytes);
		if (invalid != 0U) { // ZYX_COV_EXCL_LINE: the transaction validates both pools and derives both owners.
			throwSharedPagePoolAllocatorError(
					FormatError::FERR_INVALID_STATE,
					"relocatable cross-pool evacuation has inconsistent frame or owner metadata");
		}
		const uint64_t destinationOffset =
				PhysicalSegmentAllocator::checkedSlotOffset(data_region_offset_, destinationPool.physical_slot);
		const uint64_t destinationPageOffset =
				destinationOffset + static_cast<uint64_t>(destinationFrame) * RSS_PAGE_SIZE;
		writeTypedPage(*io_, destinationPageOffset, destinationPool.generation,
					   makeOwnedPage(destinationOwner, payload));
		static_cast<void>(sourcePool);
		static_cast<void>(sourceHeader);
		static_cast<void>(sourceOwner);
		static_cast<void>(sourceFrame);
		rewriteInactivePoolControlRecord(*io_, destinationOffset, destinationPool.generation, destinationHeader,
										 "relocatable destination page-pool control page is truncated");
	}

	void SharedPagePoolAllocator::writePreparedPool(const CatalogEntry &entry, const PageOwnerHeader &owner,
													std::span<const std::byte> payload) const {
		const uint8_t invalidEntry = static_cast<uint8_t>(entry.state != SegmentState::SGS_SEALED) |
									 static_cast<uint8_t>(!matchesPoolAndOwnerKind(entry.kind, owner.kind)) |
									 static_cast<uint8_t>(entry.generation != owner.generation) |
									 static_cast<uint8_t>(entry.committed_lsn != owner.committed_lsn) |
									 static_cast<uint8_t>(payload.size() != owner.payload_bytes);
		if (invalidEntry != 0U) { // ZYX_COV_EXCL_LINE: the enclosing transaction derives both values from one request.
			throwSharedPagePoolAllocatorError(FormatError::FERR_INVALID_STATE,
											  "relocatable shared page-pool preparation has inconsistent metadata");
		}
		const uint64_t poolOffset =
				PhysicalSegmentAllocator::checkedSlotOffset(data_region_offset_, entry.physical_slot);
		clearTypedSegment(*io_, poolOffset, entry.generation);
		SegmentEnvelope envelope;
		envelope.header_bytes = RSS_PAGE_SIZE;
		envelope.kind = entry.kind;
		envelope.state = SegmentState::SGS_SEALED;
		envelope.segment_id = entry.segment_id;
		envelope.owner_id = entry.owner_id;
		envelope.generation = entry.generation;
		envelope.committed_lsn = entry.committed_lsn;
		envelope.payload_bytes = RSS_PAGE_POOL_DATA_FRAME_COUNT * RSS_PAGE_SIZE;
		SharedPagePoolHeader poolHeader;
		poolHeader.segment_id = entry.segment_id;
		poolHeader.generation = entry.generation;
		poolHeader.committed_lsn = entry.committed_lsn;
		const std::optional<uint8_t> frame = SharedPagePoolFrameMap::allocateFirstFree(poolHeader);
		// ZYX_COV_EXCL_START: a zeroed pool always has its first data frame available.
		if (!frame.has_value() || *frame != RSS_PAGE_POOL_FIRST_DATA_FRAME) {
			throwSharedPagePoolAllocatorError(FormatError::FERR_INVALID_STATE,
											  "relocatable new shared page pool did not reserve its first data frame");
		}
		// ZYX_COV_EXCL_STOP
		writeTypedPage(*io_, poolOffset, entry.generation, makePoolControlPage(envelope, poolHeader));
		const uint64_t pageOffset = poolOffset + static_cast<uint64_t>(*frame) * RSS_PAGE_SIZE;
		writeTypedPage(*io_, pageOffset, entry.generation, makeOwnedPage(owner, payload));
	}

} // namespace graph::storage
