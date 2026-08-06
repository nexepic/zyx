/**
 * @file MetadataPagePool.cpp
 * @brief Checked fixed-slot page-pool allocation for isolated Catalog metadata.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/metadata/MetadataPagePool.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <functional>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "graph/utils/ChecksumUtils.hpp"

namespace graph::storage {

	namespace {

		constexpr size_t MAGIC_OFFSET = 0;
		constexpr size_t FORMAT_VERSION_OFFSET = 4;
		constexpr size_t HEADER_BYTES_OFFSET = 6;
		constexpr size_t PHYSICAL_SLOT_OFFSET = 8;
		constexpr size_t ALLOCATED_FRAMES_OFFSET = 16;
		constexpr size_t ALLOCATED_COUNT_OFFSET = 20;
		constexpr size_t GENERATION_OFFSET = 24;
		constexpr size_t COMMITTED_LSN_OFFSET = 32;
		constexpr size_t HEADER_CRC_OFFSET = 40;
		constexpr size_t RESERVED0_OFFSET = 44;
		constexpr size_t SEGMENT_ID_OFFSET = 48;
		constexpr size_t RESERVED2_OFFSET = 56;
		constexpr uint64_t RSS_MAX_PHYSICAL_PAGE_GENERATION = std::numeric_limits<uint64_t>::max() >> 5U;
		static_assert(RSS_METADATA_PAGE_POOL_HEADER_BYTES + RSS_ENVELOPE_BYTES <= RSS_PAGE_SIZE);

		[[noreturn]] void throwCatalogPoolError(FormatError error, const char *message) {
			throw FormatException(error, message);
		}

		[[nodiscard]] uint32_t calculateCrc(std::span<const std::byte> bytes) {
			return utils::calculateCrc(bytes.data(), bytes.size());
		}

		void validateHeader(const MetadataPagePoolHeader &header) {
			const uint8_t invalid =
					static_cast<uint8_t>(!header.physical_slot.isValid()) |
					static_cast<uint8_t>(!header.segment_id.isValid()) |
					static_cast<uint8_t>(!header.generation.isValid()) |
					static_cast<uint8_t>(!header.committed_lsn.isValid()) |
					static_cast<uint8_t>(header.generation.value() > RSS_MAX_PHYSICAL_PAGE_GENERATION) |
					static_cast<uint8_t>((header.allocated_frames & ~RSS_METADATA_PAGE_POOL_VALID_FRAME_MASK) != 0U) |
					static_cast<uint8_t>(static_cast<uint32_t>(std::popcount(header.allocated_frames)) >
										 RSS_PAGE_POOL_DATA_FRAME_COUNT);
			if (invalid != 0U) {
				throwCatalogPoolError(FormatError::FERR_INVALID_HEADER,
									  "invalid relocatable metadata page-pool header");
			}
		}

		void writeHeader(std::span<std::byte> bytes, const MetadataPagePoolHeader &header, uint32_t headerCrc) {
			LittleEndianCodec::writeU32(bytes, MAGIC_OFFSET, RSS_METADATA_PAGE_POOL_MAGIC);
			LittleEndianCodec::writeU16(bytes, FORMAT_VERSION_OFFSET, RSS_METADATA_PAGE_POOL_VERSION);
			LittleEndianCodec::writeU16(bytes, HEADER_BYTES_OFFSET, RSS_METADATA_PAGE_POOL_HEADER_BYTES);
			LittleEndianCodec::writeU64(bytes, PHYSICAL_SLOT_OFFSET, header.physical_slot.value());
			LittleEndianCodec::writeU32(bytes, ALLOCATED_FRAMES_OFFSET, header.allocated_frames);
			LittleEndianCodec::writeU32(bytes, ALLOCATED_COUNT_OFFSET, std::popcount(header.allocated_frames));
			LittleEndianCodec::writeU64(bytes, GENERATION_OFFSET, header.generation.value());
			LittleEndianCodec::writeU64(bytes, COMMITTED_LSN_OFFSET, header.committed_lsn.value());
			LittleEndianCodec::writeU32(bytes, HEADER_CRC_OFFSET, headerCrc);
			LittleEndianCodec::writeU32(bytes, RESERVED0_OFFSET, 0);
			LittleEndianCodec::writeU64(bytes, SEGMENT_ID_OFFSET, header.segment_id.value());
			LittleEndianCodec::writeU64(bytes, RESERVED2_OFFSET, 0);
		}

		[[nodiscard]] uint64_t checkedSlotOffset(uint64_t dataRegionOffset, PhysicalSlotId slot) {
			const uint64_t index = slot.value() - 1U;
			if (index > (std::numeric_limits<uint64_t>::max() - dataRegionOffset) / RSS_SEGMENT_SIZE) {
				throwCatalogPoolError(FormatError::FERR_ARITHMETIC_OVERFLOW,
									  "relocatable metadata pool slot offset overflows");
			}
			return dataRegionOffset + index * RSS_SEGMENT_SIZE;
		}

		[[nodiscard]] std::array<std::byte, RSS_PAGE_SIZE> readRawControlPage(StorageIO &io, uint64_t offset,
																			  const char *message) {
			std::array<std::byte, RSS_PAGE_SIZE> bytes{};
			if (io.readAt(offset, bytes.data(), bytes.size()) != bytes.size()) {
				throwCatalogPoolError(FormatError::FERR_BUFFER_TOO_SMALL, message);
			}
			return bytes;
		}

		[[nodiscard]] std::array<std::byte, RSS_PAGE_SIZE>
		readTypedControlPage(const StorageIO &io, uint64_t offset, Generation generation, const char *message) {
			std::array<std::byte, RSS_PAGE_SIZE> bytes{};
			if (io.readAt({offset, generation.value()}, bytes.data(), bytes.size()) != bytes.size()) {
				throwCatalogPoolError(FormatError::FERR_BUFFER_TOO_SMALL, message);
			}
			return bytes;
		}

		void writeTypedControlPage(StorageIO &io, uint64_t offset, Generation generation,
								   const std::array<std::byte, RSS_PAGE_SIZE> &bytes) {
			io.writeAt({offset, generation.value()}, bytes.data(), bytes.size());
		}

		[[nodiscard]] std::array<std::byte, RSS_PAGE_SIZE> makeControlPage(const SegmentEnvelope &envelope,
																		   const MetadataPagePoolHeader &header) {
			std::array<std::byte, RSS_PAGE_SIZE> page{};
			const auto envelopeBytes = SegmentEnvelopeCodec::encode(envelope);
			const auto headerBytes = MetadataPagePoolHeaderCodec::encode(header);
			std::copy(envelopeBytes.begin(), envelopeBytes.end(), page.begin());
			std::copy(headerBytes.begin(), headerBytes.end(), page.begin() + RSS_ENVELOPE_BYTES);
			return page;
		}

		[[nodiscard]] uint64_t radixFactor(uint16_t level) {
			uint64_t result = 1;
			for (uint16_t index = 0; index < level; ++index) {
				result *= RSS_CATALOG_RADIX_FANOUT;
			}
			return result;
		}

		[[nodiscard]] bool isNoSpaceError(const std::system_error &error) {
			if (error.code() == std::make_error_code(std::errc::no_space_on_device)) {
				return true;
			}
#ifdef _WIN32
			return error.code().category() == std::system_category() && error.code().value() == ERROR_DISK_FULL;
#else
			return false;
#endif
		}

	} // namespace

	std::array<std::byte, RSS_METADATA_PAGE_POOL_HEADER_BYTES>
	MetadataPagePoolHeaderCodec::encode(const MetadataPagePoolHeader &header) {
		validateHeader(header);
		std::array<std::byte, RSS_METADATA_PAGE_POOL_HEADER_BYTES> bytes{};
		writeHeader(bytes, header, 0);
		writeHeader(bytes, header, calculateCrc(bytes));
		return bytes;
	}

	MetadataPagePoolHeader MetadataPagePoolHeaderCodec::decode(std::span<const std::byte> bytes) {
		if (bytes.size() < RSS_METADATA_PAGE_POOL_HEADER_BYTES) {
			throwCatalogPoolError(FormatError::FERR_BUFFER_TOO_SMALL,
								  "relocatable metadata page-pool header is truncated");
		}
		if (bytes.size() != RSS_METADATA_PAGE_POOL_HEADER_BYTES) {
			throwCatalogPoolError(FormatError::FERR_INVALID_HEADER,
								  "relocatable metadata page-pool header has an unexpected size");
		}
		std::array<std::byte, RSS_METADATA_PAGE_POOL_HEADER_BYTES> checksumBytes{};
		std::copy_n(bytes.begin(), checksumBytes.size(), checksumBytes.begin());
		const uint32_t expectedCrc = LittleEndianCodec::readU32(checksumBytes, HEADER_CRC_OFFSET);
		LittleEndianCodec::writeU32(checksumBytes, HEADER_CRC_OFFSET, 0);
		if (calculateCrc(checksumBytes) != expectedCrc) {
			throwCatalogPoolError(FormatError::FERR_CHECKSUM_MISMATCH,
								  "relocatable metadata page-pool header checksum does not match");
		}
		if (LittleEndianCodec::readU32(bytes, MAGIC_OFFSET) != RSS_METADATA_PAGE_POOL_MAGIC) {
			throwCatalogPoolError(FormatError::FERR_INVALID_MAGIC,
								  "relocatable metadata page-pool magic does not match");
		}
		if (LittleEndianCodec::readU16(bytes, FORMAT_VERSION_OFFSET) != RSS_METADATA_PAGE_POOL_VERSION) {
			throwCatalogPoolError(FormatError::FERR_UNSUPPORTED_VERSION,
								  "unsupported relocatable metadata page-pool version");
		}
		const uint8_t invalidReserved = static_cast<uint8_t>(LittleEndianCodec::readU16(bytes, HEADER_BYTES_OFFSET) !=
															 RSS_METADATA_PAGE_POOL_HEADER_BYTES) |
										static_cast<uint8_t>(LittleEndianCodec::readU32(bytes, RESERVED0_OFFSET) != 0) |
										static_cast<uint8_t>(LittleEndianCodec::readU64(bytes, RESERVED2_OFFSET) != 0);
		if (invalidReserved != 0U) {
			throwCatalogPoolError(FormatError::FERR_RESERVED_BITS,
								  "relocatable metadata page-pool header uses invalid dimensions or reserved bytes");
		}
		MetadataPagePoolHeader header;
		header.physical_slot = PhysicalSlotId(LittleEndianCodec::readU64(bytes, PHYSICAL_SLOT_OFFSET));
		header.segment_id = SegmentId(LittleEndianCodec::readU64(bytes, SEGMENT_ID_OFFSET));
		header.allocated_frames = LittleEndianCodec::readU32(bytes, ALLOCATED_FRAMES_OFFSET);
		const uint32_t allocatedCount = LittleEndianCodec::readU32(bytes, ALLOCATED_COUNT_OFFSET);
		header.generation = Generation(LittleEndianCodec::readU64(bytes, GENERATION_OFFSET));
		header.committed_lsn = LogSequenceNumber(LittleEndianCodec::readU64(bytes, COMMITTED_LSN_OFFSET));
		validateHeader(header);
		if (allocatedCount != static_cast<uint32_t>(std::popcount(header.allocated_frames))) {
			throwCatalogPoolError(FormatError::FERR_INVALID_HEADER,
								  "relocatable metadata page-pool allocation count does not match its bitmap");
		}
		return header;
	}

	MetadataPagePoolAllocator::MetadataPagePoolAllocator(std::shared_ptr<StorageIO> io, const Superblock &superblock,
														 MetadataPagePoolOpenMode openMode) : io_(std::move(io)) {
		if (!io_) {
			throw std::invalid_argument("relocatable MetadataPagePoolAllocator requires StorageIO");
		}
		static_cast<void>(SuperblockCodec::encode(superblock));
		const bool rootReachabilityMode = openMode == MetadataPagePoolOpenMode::CPOM_ROOT_RECOVER ||
										  openMode == MetadataPagePoolOpenMode::CPOM_ROOT_DISCOVER_ONLY;
		if (openMode != MetadataPagePoolOpenMode::CPOM_RECOVER &&
			openMode != MetadataPagePoolOpenMode::CPOM_DISCOVER_ONLY && !rootReachabilityMode) {
			throw std::invalid_argument("relocatable MetadataPagePoolAllocator open mode is invalid");
		}
		data_region_offset_ = superblock.data_region_offset;
		emergency_reserve_ = superblock.metadata_emergency_reserve;
		{
			std::lock_guard lock(mutex_);
			if (rootReachabilityMode) {
				discoverReachablePoolsLocked(superblock);
			} else {
				discoverPoolsLocked();
			}
		}
		if (openMode == MetadataPagePoolOpenMode::CPOM_RECOVER ||
			openMode == MetadataPagePoolOpenMode::CPOM_ROOT_RECOVER) {
			recover(superblock);
		}
	}

	PageId MetadataPagePoolAllocator::allocatePageId() {
		std::lock_guard lock(mutex_);
		requireHealthyLocked();
		if (page_id_exhausted_) {
			throwCatalogPoolError(FormatError::FERR_ARITHMETIC_OVERFLOW,
								  "relocatable Catalog Page ID space is exhausted");
		}
		const PageId pageId = next_page_id_;
		if (pageId.value() == std::numeric_limits<uint64_t>::max()) {
			page_id_exhausted_ = true;
		} else {
			next_page_id_ = PageId(pageId.value() + 1U);
		}
		return pageId;
	}

	MetadataPageAllocation MetadataPagePoolAllocator::allocatePage(Generation generation,
																   LogSequenceNumber checkpointLsn,
																   std::span<const PhysicalSlotId> excludedPoolSlots) {
		std::lock_guard lock(mutex_);
		requireHealthyLocked();
		const uint8_t invalidGeneration = static_cast<uint8_t>(!generation.isValid()) |
										  static_cast<uint8_t>(generation.value() > RSS_MAX_PHYSICAL_PAGE_GENERATION) |
										  static_cast<uint8_t>(!checkpointLsn.isValid());
		if (invalidGeneration != 0U) {
			throwCatalogPoolError(FormatError::FERR_INVALID_HEADER,
								  "relocatable Catalog page allocation context is invalid");
		}
		const auto excluded = [&excludedPoolSlots](PhysicalSlotId slot) {
			return std::ranges::any_of(excludedPoolSlots,
									   [slot](PhysicalSlotId candidate) { return candidate == slot; });
		};
		auto pool = pools_.begin();
		std::optional<PhysicalSlotId> createdPoolSlot;
		while (pool != pools_.end() &&
			   (excluded(pool->second.header.physical_slot) ||
				(emergency_reserve_.isValid() &&
				 pool->second.header.physical_slot == emergency_reserve_.physical_slot) ||
				pool->second.header.allocated_frames == RSS_METADATA_PAGE_POOL_VALID_FRAME_MASK)) {
			++pool;
		}
		if (pool == pools_.end()) {
			try {
				PoolState &created = createPoolLocked(generation, checkpointLsn, excludedPoolSlots);
				createdPoolSlot = created.header.physical_slot;
				pool = pools_.find(created.header.physical_slot.value());
			} catch (const std::system_error &error) {
				if (!isNoSpaceError(error) || !emergency_reserve_.isValid() ||
					excluded(emergency_reserve_.physical_slot)) {
					throw;
				}
				pool = pools_.find(emergency_reserve_.physical_slot.value());
				if (pool == pools_.end() ||
					pool->second.header.allocated_frames == RSS_METADATA_PAGE_POOL_VALID_FRAME_MASK) {
					throwCatalogPoolError(FormatError::FERR_INVALID_STATE,
										  "relocatable metadata emergency reserve has no free page frame");
				}
			}
		}
		const uint32_t freeFrames = RSS_METADATA_PAGE_POOL_VALID_FRAME_MASK & ~pool->second.header.allocated_frames;
		const uint8_t frame = static_cast<uint8_t>(std::countr_zero(freeFrames) + RSS_PAGE_POOL_FIRST_DATA_FRAME);
		MetadataPagePoolHeader updated = pool->second.header;
		updated.allocated_frames |= 1U << (frame - RSS_PAGE_POOL_FIRST_DATA_FRAME);
		PoolState updatedPool{updated};
		writePoolLocked(updatedPool);
		pool->second = std::move(updatedPool);
		return MetadataPageAllocation{PhysicalPageRef{pool->second.header.physical_slot, frame, generation},
									  createdPoolSlot};
	}

	PhysicalPageRef MetadataPagePoolAllocator::allocatePage(Generation generation) {
		return allocatePage(generation, LogSequenceNumber(1), {}).page;
	}

	void MetadataPagePoolAllocator::abandonPage(PhysicalPageRef reference) noexcept {
		try {
			std::lock_guard lock(mutex_);
			requireHealthyLocked();
			auto pool = pools_.find(reference.physical_slot.value());
			if (!reference.isValid() || pool == pools_.end()) {
				throwCatalogPoolError(FormatError::FERR_INVALID_STATE,
									  "relocatable Catalog page abandonment has an unknown physical location");
			}
			const uint32_t bit = 1U << (reference.frame_index - RSS_PAGE_POOL_FIRST_DATA_FRAME);
			if ((pool->second.header.allocated_frames & bit) == 0U) {
				throwCatalogPoolError(FormatError::FERR_INVALID_STATE,
									  "relocatable Catalog page abandonment targets an unallocated frame");
			}
			MetadataPagePoolHeader updated = pool->second.header;
			updated.allocated_frames &= ~bit;
			PoolState updatedPool{updated};
			writePoolLocked(updatedPool);
			pool->second = std::move(updatedPool);
		} catch (...) {
			// The frame remains allocated on any failure, so poison rather than risking reuse.
			std::lock_guard lock(mutex_);
			poisoned_ = true;
		}
	}

	void MetadataPagePoolAllocator::abandonUnpublishedPool(PhysicalSlotId slot) noexcept {
		try {
			std::lock_guard lock(mutex_);
			requireHealthyLocked();
			eraseUnpublishedPoolLocked(slot);
		} catch (...) {
			// A failed wipe leaves the slot unavailable until recovery verifies it.
			std::lock_guard lock(mutex_);
			poisoned_ = true;
		}
	}

	uint64_t MetadataPagePoolAllocator::physicalFileSize() const {
		std::lock_guard lock(mutex_);
		requireHealthyLocked();
		return io_->fileSize();
	}

	void MetadataPagePoolAllocator::abandonUnpublishedBatch(std::span<const PhysicalSlotId> poolSlots,
															std::span<const PhysicalSlotId> dataSlots,
															uint64_t initialFileSize) noexcept {
		try {
			std::lock_guard lock(mutex_);
			requireHealthyLocked();
			abandonUnpublishedBatchLocked(poolSlots, dataSlots, initialFileSize);
		} catch (...) {
			// The batch owns only unpublished state.  If its cleanup cannot prove a
			// safe tail boundary, fail closed rather than reuse any of those slots.
			std::lock_guard lock(mutex_);
			poisoned_ = true;
		}
	}

	void MetadataPagePoolAllocator::recover(const Superblock &superblock) {
		static_cast<void>(SuperblockCodec::encode(superblock));
		const std::array<Superblock, 1> protectedSuperblocks = {superblock};
		std::unique_lock lock(mutex_);
		try {
			validateRegisteredPoolsLocked();
			reconcileReachabilityLocked(protectedSuperblocks, true, true, true);
			poisoned_ = false;
		} catch (...) {
			poisoned_ = true;
			throw;
		}
	}

	void MetadataPagePoolAllocator::validateReachability(const Superblock &superblock) {
		static_cast<void>(SuperblockCodec::encode(superblock));
		const std::array<Superblock, 1> protectedSuperblocks = {superblock};
		std::unique_lock lock(mutex_);
		try {
			requireHealthyLocked();
			reconcileReachabilityLocked(protectedSuperblocks, false, false, false);
		} catch (...) {
			poisoned_ = true;
			throw;
		}
	}

	void MetadataPagePoolAllocator::registerCatalogPool(PhysicalSlotId slot, SegmentId segmentId, Generation generation,
														LogSequenceNumber committedLsn) {
		std::lock_guard lock(mutex_);
		requireHealthyLocked();
		if (!slot.isValid() || !segmentId.isValid() || !generation.isValid() || !committedLsn.isValid()) {
			throwCatalogPoolError(FormatError::FERR_INVALID_HEADER,
								  "relocatable Catalog metadata-pool ownership record is invalid");
		}
		const PoolState discovered = readPoolLocked(slot, segmentId, generation, committedLsn);
		const auto [iterator, inserted] = pools_.emplace(slot.value(), discovered);
		if (!inserted &&
			(iterator->second.header.segment_id != segmentId || iterator->second.header.generation != generation ||
			 iterator->second.header.committed_lsn != committedLsn)) {
			throwCatalogPoolError(FormatError::FERR_INVALID_STATE,
								  "relocatable Catalog metadata-pool ownership conflicts with a reachable pool");
		}
	}

	void MetadataPagePoolAllocator::bindCatalogPoolIdentity(PhysicalSlotId slot, SegmentId segmentId,
															Generation generation, LogSequenceNumber committedLsn) {
		std::lock_guard lock(mutex_);
		try {
			requireHealthyLocked();
			if (!slot.isValid() || !segmentId.isValid() || !generation.isValid() || !committedLsn.isValid()) {
				throwCatalogPoolError(FormatError::FERR_INVALID_HEADER,
									  "relocatable metadata-pool identity binding is invalid");
			}
			auto pool = pools_.find(slot.value());
			if (pool == pools_.end() || pool->second.header.generation != generation ||
				pool->second.header.committed_lsn != committedLsn ||
				(pool->second.header.segment_id != segmentId && pool->second.header.allocated_frames == 0U)) {
				throwCatalogPoolError(FormatError::FERR_INVALID_STATE,
									  "relocatable metadata-pool identity binding does not target a private pool");
			}
			PoolState updated{pool->second.header};
			updated.header.segment_id = segmentId;
			std::array<std::byte, RSS_PAGE_SIZE> controlPage{};
			static_cast<void>(
					readPoolLocked(slot, pool->second.header.segment_id, generation, committedLsn, &controlPage));
			std::array<std::byte, RSS_ENVELOPE_BYTES> envelopeBytes{};
			std::copy_n(controlPage.begin(), envelopeBytes.size(), envelopeBytes.begin());
			SegmentEnvelope envelope = SegmentEnvelopeCodec::decode(envelopeBytes);
			envelope.segment_id = segmentId;
			const auto encodedEnvelope = SegmentEnvelopeCodec::encode(envelope);
			const auto encodedHeader = MetadataPagePoolHeaderCodec::encode(updated.header);
			std::copy(encodedEnvelope.begin(), encodedEnvelope.end(), controlPage.begin());
			std::copy(encodedHeader.begin(), encodedHeader.end(), controlPage.begin() + RSS_ENVELOPE_BYTES);
			writeTypedControlPage(*io_, slotOffset(slot), generation, controlPage);
			pool->second = std::move(updated);
		} catch (...) {
			poisoned_ = true;
			throw;
		}
	}

	void MetadataPagePoolAllocator::createEmergencyReserve(const MetadataEmergencyReserveDescriptor &reserve) {
		std::lock_guard lock(mutex_);
		requireHealthyLocked();
		if (!reserve.isValid() || emergency_reserve_.isValid() || pools_.contains(reserve.physical_slot.value())) {
			throwCatalogPoolError(FormatError::FERR_INVALID_STATE,
								  "relocatable metadata emergency reserve is already present or invalid");
		}
		static_cast<void>(createPoolAtLocked(reserve.physical_slot, reserve.pool_token, reserve.generation,
											 reserve.committed_lsn));
		emergency_reserve_ = reserve;
	}

	MetadataEmergencyReserveDescriptor
	MetadataPagePoolAllocator::rotateEmergencyReserve(SegmentId nextPoolToken, Generation generation,
													  LogSequenceNumber committedLsn) {
		std::lock_guard lock(mutex_);
		requireHealthyLocked();
		const auto previous = emergency_reserve_;
		const auto previousPool = pools_.find(previous.physical_slot.value());
		if (!previous.isValid() || previousPool == pools_.end() ||
			previousPool->second.header.segment_id != previous.pool_token ||
			previousPool->second.header.generation != previous.generation ||
			previousPool->second.header.committed_lsn != previous.committed_lsn ||
			previousPool->second.header.allocated_frames == 0U || !nextPoolToken.isValid() ||
			nextPoolToken == previous.pool_token || !generation.isValid() || !committedLsn.isValid()) {
			throwCatalogPoolError(
					FormatError::FERR_INVALID_STATE,
					"relocatable metadata emergency reserve rotation has no consumed reserve or valid successor");
		}
		const uint64_t fileSize = io_->fileSize();
		const uint8_t invalidTail =
				static_cast<uint8_t>(fileSize < data_region_offset_) |
				static_cast<uint8_t>((fileSize - data_region_offset_) % RSS_SEGMENT_SIZE != 0U) |
				static_cast<uint8_t>(fileSize - data_region_offset_ >
									 (std::numeric_limits<uint64_t>::max() - 1U) / RSS_SEGMENT_SIZE);
		if (invalidTail != 0U) {
			throwCatalogPoolError(FormatError::FERR_ARITHMETIC_OVERFLOW,
								  "relocatable metadata emergency reserve cannot allocate a successor tail slot");
		}
		const PhysicalSlotId successor((fileSize - data_region_offset_) / RSS_SEGMENT_SIZE + 1U);
		MetadataEmergencyReserveDescriptor next;
		next.physical_slot = successor;
		next.pool_token = nextPoolToken;
		next.generation = generation;
		next.committed_lsn = committedLsn;
		if (!next.isValid() || pools_.contains(successor.value())) {
			throwCatalogPoolError(FormatError::FERR_INVALID_STATE,
								  "relocatable metadata emergency reserve successor identity is invalid or occupied");
		}
		static_cast<void>(createPoolAtLocked(successor, next.pool_token, generation, committedLsn));
		emergency_reserve_ = next;
		return next;
	}

	void MetadataPagePoolAllocator::restoreEmergencyReserve(
			const MetadataEmergencyReserveDescriptor &previous,
			const MetadataEmergencyReserveDescriptor &expectedCurrent) noexcept {
		try {
			std::lock_guard lock(mutex_);
			if (previous.isValid() && expectedCurrent.isValid() && emergency_reserve_ == expectedCurrent) {
				emergency_reserve_ = previous;
			}
		} catch (...) {
			// This only protects in-memory cleanup after a failed unpublished batch.
			// A failed lock cannot safely establish a writable allocator state.
		}
	}

	MetadataEmergencyReserveDescriptor MetadataPagePoolAllocator::emergencyReserve() const {
		std::lock_guard lock(mutex_);
		return emergency_reserve_;
	}

	bool MetadataPagePoolAllocator::emergencyReserveHasAllocatedFrames() const {
		std::lock_guard lock(mutex_);
		const auto pool = pools_.find(emergency_reserve_.physical_slot.value());
		return emergency_reserve_.isValid() && pool != pools_.end() && pool->second.header.allocated_frames != 0U;
	}

	bool MetadataPagePoolAllocator::isEmergencyReserveSlot(PhysicalSlotId slot) const {
		std::lock_guard lock(mutex_);
		return emergency_reserve_.isValid() && slot == emergency_reserve_.physical_slot;
	}

	void MetadataPagePoolAllocator::reclaimUnreachable(const Superblock &current,
													   std::span<const Superblock> activeSnapshots) {
		static_cast<void>(SuperblockCodec::encode(current));
		std::vector<Superblock> protectedSuperblocks;
		protectedSuperblocks.reserve(activeSnapshots.size() + 1U);
		protectedSuperblocks.push_back(current);
		for (const Superblock &snapshot: activeSnapshots) {
			static_cast<void>(SuperblockCodec::encode(snapshot));
			if (snapshot.database_id != current.database_id || snapshot.generation > current.generation ||
				snapshot.checkpoint_lsn > current.checkpoint_lsn ||
				snapshot.data_region_offset != current.data_region_offset) {
				throwCatalogPoolError(FormatError::FERR_INVALID_STATE,
									  "relocatable metadata snapshot is not visible from the current Superblock");
			}
			protectedSuperblocks.push_back(snapshot);
		}

		std::unique_lock lock(mutex_);
		try {
			requireHealthyLocked();
			// This runs after the new Superblock is durable. A later publication's
			// mandatory data fence persists these reusable-frame header updates.
			reconcileReachabilityLocked(protectedSuperblocks, false, true, false);
		} catch (...) {
			poisoned_ = true;
			throw;
		}
	}

	void MetadataPagePoolAllocator::reconcileReachabilityLocked(std::span<const Superblock> protectedSuperblocks,
																bool resetPageId, bool reclaimUnreachable,
																bool persistReclaimedFrames) {
		if (protectedSuperblocks.empty()) {
			throwCatalogPoolError(FormatError::FERR_INVALID_STATE,
								  "relocatable metadata reachability reconciliation requires a current Superblock");
		}
		for (const Superblock &superblock: protectedSuperblocks) {
			if (superblock.data_region_offset != data_region_offset_) {
				throwCatalogPoolError(FormatError::FERR_INVALID_STATE,
									  "relocatable metadata pool cannot reconcile a different data-region layout");
			}
		}
		std::set<std::pair<uint64_t, uint8_t>> reachable;
		uint64_t maximumPageId = 0;
		MetadataPageFile pages(io_, protectedSuperblocks.front());

		auto markReference = [&](PhysicalPageRef reference, std::set<std::pair<uint64_t, uint8_t>> &treeReferences) {
			if (!reference.isValid()) {
				throwCatalogPoolError(FormatError::FERR_INVALID_HEADER,
									  "relocatable metadata tree contains an invalid physical reference");
			}
			auto pool = pools_.find(reference.physical_slot.value());
			const uint32_t bit = 1U << (reference.frame_index - RSS_PAGE_POOL_FIRST_DATA_FRAME);
			if (pool == pools_.end() || (pool->second.header.allocated_frames & bit) == 0U ||
				!treeReferences.emplace(reference.physical_slot.value(), reference.frame_index).second) {
				throwCatalogPoolError(FormatError::FERR_INVALID_STATE,
									  "relocatable metadata tree references an unavailable or duplicate pool frame");
			}
			reachable.emplace(reference.physical_slot.value(), reference.frame_index);
		};

		auto allocatorLeafHasFreeSlot = [](const AllocatorLeafPage &leaf) {
			const uint32_t usedBytes = (leaf.slot_count + 7U) / 8U;
			for (uint32_t index = 0U; index < usedBytes; ++index) {
				uint8_t clearBits = static_cast<uint8_t>(~std::to_integer<uint8_t>(leaf.allocated_slots[index]));
				if (index + 1U == usedBytes && (leaf.slot_count % 8U) != 0U) {
					clearBits &= static_cast<uint8_t>((1U << (leaf.slot_count % 8U)) - 1U);
				}
				if (clearBits != 0U) {
					return true;
				}
			}
			return false;
		};
		auto allocatorSummaryBit = [](const MetadataRadixPage &page, uint32_t index) {
			return (std::to_integer<uint8_t>(page.allocator_free_children[index / 8U]) & (1U << (index % 8U))) != 0U;
		};

		std::function<bool(PhysicalPageRef, bool, uint16_t, uint64_t, MetadataDirectoryType, const Superblock &,
						   std::set<std::pair<uint64_t, uint8_t>> &)>
				visitRadix;
		visitRadix = [&](PhysicalPageRef reference, bool isRoot, uint16_t expectedLevel, uint64_t ordinalBase,
						 MetadataDirectoryType directoryType, const Superblock &superblock,
						 std::set<std::pair<uint64_t, uint8_t>> &treeReferences) -> bool {
			markReference(reference, treeReferences);
			const MetadataRadixPage page = pages.readMetadataRadix(reference);
			if ((isRoot && page.kind != CatalogRadixPageKind::CRPK_ROOT) ||
				(!isRoot && (page.kind != CatalogRadixPageKind::CRPK_INTERNAL || page.level != expectedLevel)) ||
				page.directory_type != directoryType || page.generation > superblock.generation ||
				page.committed_lsn > superblock.checkpoint_lsn) {
				throwCatalogPoolError(
						FormatError::FERR_INVALID_HEADER,
						"relocatable metadata directory has an invalid radix page during pool reconciliation");
			}
			maximumPageId = std::max(maximumPageId, page.page_id.value());
			bool subtreeHasFreeSlot = false;
			const uint64_t factor = radixFactor(page.level);
			for (uint32_t index = 0; index < RSS_CATALOG_RADIX_FANOUT; ++index) {
				const PhysicalPageRef child = page.children[index];
				if (!child.isValid()) {
					if (directoryType == MetadataDirectoryType::MDT_ALLOCATOR) {
						if (allocatorSummaryBit(page, index)) {
							throwCatalogPoolError(
									FormatError::FERR_INVALID_HEADER,
									"relocatable allocator radix summary marks an absent child as noncanonical");
						}
						subtreeHasFreeSlot = true;
					}
					continue;
				}
				if (index != 0U && factor > (std::numeric_limits<uint64_t>::max() - ordinalBase) / index) {
					throwCatalogPoolError(FormatError::FERR_ARITHMETIC_OVERFLOW,
										  "relocatable Catalog radix route exceeds the Segment-ID domain");
				}
				const uint64_t childBase = ordinalBase + static_cast<uint64_t>(index) * factor;
				if (page.level != 0U) {
					const bool childHasFreeSlot = visitRadix(child, false, static_cast<uint16_t>(page.level - 1U),
															 childBase, directoryType, superblock, treeReferences);
					if (directoryType == MetadataDirectoryType::MDT_ALLOCATOR) {
						if (allocatorSummaryBit(page, index) != childHasFreeSlot) {
							throwCatalogPoolError(
									FormatError::FERR_INVALID_HEADER,
									"relocatable allocator radix summary disagrees with its child subtree");
						}
						subtreeHasFreeSlot = subtreeHasFreeSlot || childHasFreeSlot;
					}
					continue;
				}
				markReference(child, treeReferences);
				if (directoryType == MetadataDirectoryType::MDT_CATALOG) {
					const CatalogLeafPage leaf = pages.readCatalogLeaf(child);
					if (leaf.entries.empty() || leaf.generation > superblock.generation ||
						leaf.committed_lsn > superblock.checkpoint_lsn) {
						throwCatalogPoolError(
								FormatError::FERR_INVALID_HEADER,
								"relocatable Catalog tree has an invalid leaf during pool reconciliation");
					}
					maximumPageId = std::max(maximumPageId, leaf.page_id.value());
					for (const CatalogEntry &entry: leaf.entries) {
						if ((entry.segment_id.value() - 1U) / RSS_CATALOG_PAGE_MAX_ENTRIES != childBase) {
							throwCatalogPoolError(FormatError::FERR_INVALID_HEADER,
												  "relocatable Catalog leaf is inconsistent with its radix route");
						}
					}
				} else if (directoryType == MetadataDirectoryType::MDT_ALLOCATOR) {
					const AllocatorLeafPage leaf = pages.readAllocatorLeaf(child);
					const uint8_t invalidLeaf =
							static_cast<uint8_t>(leaf.generation > superblock.generation) |
							static_cast<uint8_t>(leaf.committed_lsn > superblock.checkpoint_lsn) |
							static_cast<uint8_t>(childBase > (std::numeric_limits<uint64_t>::max() - 1U) /
																	 RSS_ALLOCATOR_PAGE_SLOT_CAPACITY) |
							static_cast<uint8_t>(leaf.first_slot.value() !=
												 childBase * RSS_ALLOCATOR_PAGE_SLOT_CAPACITY + 1U);
					if (invalidLeaf != 0U) {
						throwCatalogPoolError(FormatError::FERR_INVALID_HEADER,
											  "relocatable free-space leaf is inconsistent with its radix route");
					}
					const bool childHasFreeSlot = allocatorLeafHasFreeSlot(leaf);
					if (allocatorSummaryBit(page, index) != childHasFreeSlot) {
						throwCatalogPoolError(FormatError::FERR_INVALID_HEADER,
											  "relocatable allocator radix summary disagrees with its bitmap leaf");
					}
					subtreeHasFreeSlot = subtreeHasFreeSlot || childHasFreeSlot;
					maximumPageId = std::max(maximumPageId, leaf.page_id.value());
				} else {
					const LocationLeafPage leaf = pages.readLocationLeaf(child);
					const uint8_t invalidLeaf = static_cast<uint8_t>(leaf.directory_type != directoryType) |
												static_cast<uint8_t>(leaf.generation > superblock.generation) |
												static_cast<uint8_t>(leaf.committed_lsn > superblock.checkpoint_lsn);
					if (invalidLeaf != 0U) {
						throwCatalogPoolError(
								FormatError::FERR_INVALID_HEADER,
								"relocatable location directory has an invalid leaf during pool reconciliation");
					}
					for (const LocationLeafEntry &entry: leaf.entries) {
						if ((entry.logical_id - 1U) / RSS_LOCATION_LEAF_MAX_ENTRIES != childBase) {
							throwCatalogPoolError(FormatError::FERR_INVALID_HEADER,
												  "relocatable location leaf is inconsistent with its radix route");
						}
					}
					maximumPageId = std::max(maximumPageId, leaf.page_id.value());
				}
			}
			return subtreeHasFreeSlot;
		};

		for (const Superblock &superblock: protectedSuperblocks) {
			if (superblock.catalog_root.isValid()) {
				std::set<std::pair<uint64_t, uint8_t>> treeReferences;
				visitRadix(superblock.catalog_root, true, 0, 0, MetadataDirectoryType::MDT_CATALOG, superblock,
						   treeReferences);
			}
			if (superblock.allocator_root.isValid()) {
				std::set<std::pair<uint64_t, uint8_t>> treeReferences;
				visitRadix(superblock.allocator_root, true, 0, 0, MetadataDirectoryType::MDT_ALLOCATOR, superblock,
						   treeReferences);
			}
			if (superblock.page_directory_root.isValid()) {
				std::set<std::pair<uint64_t, uint8_t>> treeReferences;
				visitRadix(superblock.page_directory_root, true, 0, 0, MetadataDirectoryType::MDT_PAGE, superblock,
						   treeReferences);
			}
			if (superblock.extent_directory_root.isValid()) {
				std::set<std::pair<uint64_t, uint8_t>> treeReferences;
				visitRadix(superblock.extent_directory_root, true, 0, 0, MetadataDirectoryType::MDT_EXTENT, superblock,
						   treeReferences);
			}
			if (superblock.node_entity_directory_root.isValid()) {
				std::set<std::pair<uint64_t, uint8_t>> treeReferences;
				visitRadix(superblock.node_entity_directory_root, true, 0, 0, MetadataDirectoryType::MDT_NODE_ENTITY,
						   superblock, treeReferences);
			}
			if (superblock.edge_entity_directory_root.isValid()) {
				std::set<std::pair<uint64_t, uint8_t>> treeReferences;
				visitRadix(superblock.edge_entity_directory_root, true, 0, 0, MetadataDirectoryType::MDT_EDGE_ENTITY,
						   superblock, treeReferences);
			}
			if (superblock.entity_property_directory_root.isValid()) {
				std::set<std::pair<uint64_t, uint8_t>> treeReferences;
				visitRadix(superblock.entity_property_directory_root, true, 0, 0,
						   MetadataDirectoryType::MDT_ENTITY_PROPERTY, superblock, treeReferences);
			}
			if (superblock.index_root_directory_root.isValid()) {
				std::set<std::pair<uint64_t, uint8_t>> treeReferences;
				visitRadix(superblock.index_root_directory_root, true, 0, 0, MetadataDirectoryType::MDT_INDEX_ROOT,
						   superblock, treeReferences);
			}
			if (superblock.vector_location_directory_root.isValid()) {
				std::set<std::pair<uint64_t, uint8_t>> treeReferences;
				visitRadix(superblock.vector_location_directory_root, true, 0, 0,
						   MetadataDirectoryType::MDT_VECTOR_LOCATION, superblock, treeReferences);
			}
			if (superblock.vector_manifest_directory_root.isValid()) {
				std::set<std::pair<uint64_t, uint8_t>> treeReferences;
				visitRadix(superblock.vector_manifest_directory_root, true, 0, 0,
						   MetadataDirectoryType::MDT_VECTOR_MANIFEST, superblock, treeReferences);
			}
			if (superblock.large_value_manifest_directory_root.isValid()) {
				std::set<std::pair<uint64_t, uint8_t>> treeReferences;
				visitRadix(superblock.large_value_manifest_directory_root, true, 0, 0,
						   MetadataDirectoryType::MDT_LARGE_VALUE_MANIFEST, superblock, treeReferences);
			}
			if (superblock.state_directory_root.isValid()) {
				std::set<std::pair<uint64_t, uint8_t>> treeReferences;
				visitRadix(superblock.state_directory_root, true, 0, 0, MetadataDirectoryType::MDT_STATE, superblock,
						   treeReferences);
			}
			if (superblock.retire_queue_root.isValid()) {
				std::set<std::pair<uint64_t, uint8_t>> treeReferences;
				visitRadix(superblock.retire_queue_root, true, 0, 0, MetadataDirectoryType::MDT_RETIRE_QUEUE,
						   superblock, treeReferences);
			}
		}

		bool changed = false;
		if (reclaimUnreachable) {
			for (auto &[slot, pool]: pools_) {
				uint32_t liveFrames = 0;
				for (uint8_t frame = RSS_PAGE_POOL_FIRST_DATA_FRAME; frame <= RSS_PAGE_POOL_DATA_FRAME_COUNT; ++frame) {
					if (reachable.contains({slot, frame})) {
						liveFrames |= 1U << (frame - RSS_PAGE_POOL_FIRST_DATA_FRAME);
					}
				}
				if (liveFrames != pool.header.allocated_frames) {
					PoolState updated{pool.header};
					updated.header.allocated_frames = liveFrames;
					writePoolLocked(updated);
					pool = std::move(updated);
					changed = true;
				}
			}
		}
		if (resetPageId) {
			if (maximumPageId == std::numeric_limits<uint64_t>::max()) {
				page_id_exhausted_ = true;
			} else {
				next_page_id_ = PageId(maximumPageId + 1U);
				page_id_exhausted_ = false;
			}
		}
		if (changed && persistReclaimedFrames) {
			io_->sync();
		}
	}

	MetadataPagePoolStats MetadataPagePoolAllocator::stats() const {
		std::lock_guard lock(mutex_);
		MetadataPagePoolStats result;
		result.pools = pools_.size();
		result.next_page_id = next_page_id_;
		result.poisoned = poisoned_;
		for (const auto &[slot, pool]: pools_) {
			static_cast<void>(slot);
			result.allocated_pages += std::popcount(pool.header.allocated_frames);
		}
		result.free_pages = result.pools * RSS_PAGE_POOL_DATA_FRAME_COUNT - result.allocated_pages;
		return result;
	}

	bool MetadataPagePoolAllocator::isMetadataPoolSlot(PhysicalSlotId slot) const {
		std::lock_guard lock(mutex_);
		return slot.isValid() && pools_.contains(slot.value());
	}

	bool MetadataPagePoolAllocator::isMetadataPoolEmpty(PhysicalSlotId slot) const {
		std::lock_guard lock(mutex_);
		const auto pool = pools_.find(slot.value());
		return slot.isValid() && pool != pools_.end() && pool->second.header.allocated_frames == 0U;
	}

	MetadataPagePoolHeader MetadataPagePoolAllocator::poolHeader(PhysicalSlotId slot) const {
		std::lock_guard lock(mutex_);
		const auto pool = pools_.find(slot.value());
		if (!slot.isValid() || pool == pools_.end()) {
			throwCatalogPoolError(FormatError::FERR_INVALID_STATE,
								  "relocatable metadata-pool header lookup targets an unknown physical slot");
		}
		return pool->second.header;
	}

	void MetadataPagePoolAllocator::releaseEmptyCatalogPools(std::span<const PhysicalSlotId> slots) {
		std::lock_guard lock(mutex_);
		requireHealthyLocked();
		uint64_t previous = 0U;
		for (const PhysicalSlotId slot: slots) {
			const auto pool = pools_.find(slot.value());
			if (!slot.isValid() || slot.value() <= previous ||
				(emergency_reserve_.isValid() && slot == emergency_reserve_.physical_slot) || pool == pools_.end() ||
				pool->second.header.allocated_frames != 0U) {
				throwCatalogPoolError(FormatError::FERR_INVALID_STATE,
									  "relocatable metadata-pool release targets a nonempty or unknown slot");
			}
			previous = slot.value();
		}
		for (const PhysicalSlotId slot: slots) {
			pools_.erase(slot.value());
		}
	}

	std::vector<PhysicalSlotId> MetadataPagePoolAllocator::poolSlots() const {
		std::lock_guard lock(mutex_);
		std::vector<PhysicalSlotId> result;
		result.reserve(pools_.size());
		for (const auto &[slot, pool]: pools_) {
			static_cast<void>(pool);
			result.emplace_back(slot);
		}
		return result;
	}

#ifdef ZYX_ENABLE_STORAGE_TEST_HOOKS
	void MetadataPagePoolAllocator::eraseRegisteredPoolForTesting(PhysicalSlotId slot) {
		std::lock_guard lock(mutex_);
		pools_.erase(slot.value());
	}

	void MetadataPagePoolAllocator::replaceRegisteredPoolHeaderForTesting(PhysicalSlotId slot,
																		  MetadataPagePoolHeader header) {
		std::lock_guard lock(mutex_);
		pools_.at(slot.value()).header = header;
	}
#endif

	uint64_t MetadataPagePoolAllocator::slotOffset(PhysicalSlotId slot) const {
		return checkedSlotOffset(data_region_offset_, slot);
	}

	MetadataPagePoolAllocator::PoolState
	MetadataPagePoolAllocator::readPoolLocked(PhysicalSlotId slot, SegmentId expectedSegmentId,
											  Generation expectedGeneration, LogSequenceNumber expectedLsn,
											  std::array<std::byte, RSS_PAGE_SIZE> *controlPage) const {
		// ZYX_COV_EXCL_START: every private caller validates or decodes the complete durable identity before invoking
		// this final fail-closed guard; retain it so future callers cannot issue an unkeyed typed read.
		if (!slot.isValid() || !expectedSegmentId.isValid() || !expectedGeneration.isValid() ||
			!expectedLsn.isValid()) {
			throwCatalogPoolError(FormatError::FERR_INVALID_HEADER,
								  "relocatable metadata-pool reader received an invalid durable identity");
		}
		// ZYX_COV_EXCL_STOP
		std::array<std::byte, RSS_PAGE_SIZE> page = readTypedControlPage(
				*io_, slotOffset(slot), expectedGeneration, "relocatable metadata-pool control page is truncated");
		std::array<std::byte, RSS_ENVELOPE_BYTES> envelopeBytes{};
		std::copy_n(page.begin(), envelopeBytes.size(), envelopeBytes.begin());
		const SegmentEnvelope envelope = SegmentEnvelopeCodec::decode(envelopeBytes);
		const uint8_t invalidEnvelope =
				static_cast<uint8_t>(envelope.kind != SegmentKind::SGK_METADATA_PAGE_POOL) |
				static_cast<uint8_t>(envelope.state != SegmentState::SGS_SEALED) |
				static_cast<uint8_t>(envelope.owner_id != slot.value()) |
				static_cast<uint8_t>(envelope.header_bytes != RSS_PAGE_SIZE) |
				static_cast<uint8_t>(envelope.payload_bytes != RSS_PAGE_POOL_DATA_FRAME_COUNT * RSS_PAGE_SIZE) |
				static_cast<uint8_t>(envelope.generation != expectedGeneration) |
				static_cast<uint8_t>(envelope.committed_lsn != expectedLsn);
		if (invalidEnvelope != 0U) {
			throwCatalogPoolError(FormatError::FERR_INVALID_HEADER,
								  "relocatable metadata pool envelope does not match its durable ownership record");
		}
		std::array<std::byte, RSS_METADATA_PAGE_POOL_HEADER_BYTES> headerBytes{};
		std::copy_n(page.begin() + RSS_ENVELOPE_BYTES, headerBytes.size(), headerBytes.begin());
		const MetadataPagePoolHeader header = MetadataPagePoolHeaderCodec::decode(headerBytes);
		const uint8_t invalidHeader = static_cast<uint8_t>(header.physical_slot != slot) |
									  static_cast<uint8_t>(header.segment_id != envelope.segment_id) |
									  static_cast<uint8_t>(header.segment_id != expectedSegmentId) |
									  static_cast<uint8_t>(header.generation != envelope.generation) |
									  static_cast<uint8_t>(header.committed_lsn != envelope.committed_lsn);
		if (invalidHeader != 0U) {
			throwCatalogPoolError(FormatError::FERR_INVALID_HEADER,
								  "relocatable metadata-pool header does not match its segment envelope");
		}
		if (controlPage != nullptr) {
			*controlPage = std::move(page);
		}
		return PoolState{header};
	}

	void MetadataPagePoolAllocator::discoverPoolsLocked() {
		const uint64_t fileSize = io_->fileSize();
		const uint8_t invalidTail = static_cast<uint8_t>(fileSize < data_region_offset_) |
									static_cast<uint8_t>((fileSize - data_region_offset_) % RSS_SEGMENT_SIZE != 0U);
		if (invalidTail != 0U) {
			throwCatalogPoolError(FormatError::FERR_INVALID_HEADER,
								  "relocatable metadata pool file has an invalid physical-slot tail");
		}
		const uint64_t poolCount = (fileSize - data_region_offset_) / RSS_SEGMENT_SIZE;
		pools_.clear();
		for (uint64_t slot = 1; slot <= poolCount; ++slot) {
			const PhysicalSlotId physicalSlot(slot);
			const std::array<std::byte, RSS_PAGE_SIZE> controlPage = readRawControlPage(
					*io_, slotOffset(physicalSlot), "relocatable metadata pool control page is truncated");
			std::array<std::byte, RSS_ENVELOPE_BYTES> envelopeBytes{};
			std::copy_n(controlPage.begin(), envelopeBytes.size(), envelopeBytes.begin());
			const bool emptySlot =
					std::ranges::all_of(envelopeBytes, [](std::byte value) { return value == std::byte{}; });
			if (emptySlot) {
				continue;
			}
			if (LittleEndianCodec::readU32(envelopeBytes, 0) != RSS_SEGMENT_MAGIC) {
				throwCatalogPoolError(FormatError::FERR_INVALID_MAGIC,
									  "relocatable physical slot has an unrecognized non-empty envelope");
			}
			const SegmentEnvelope envelope = SegmentEnvelopeCodec::decode(envelopeBytes);
			if (envelope.kind != SegmentKind::SGK_METADATA_PAGE_POOL) {
				continue;
			}
			const uint8_t invalidEnvelope =
					static_cast<uint8_t>(envelope.state != SegmentState::SGS_SEALED) |
					static_cast<uint8_t>(envelope.owner_id != slot) |
					static_cast<uint8_t>(envelope.header_bytes != RSS_PAGE_SIZE) |
					static_cast<uint8_t>(envelope.payload_bytes != RSS_PAGE_POOL_DATA_FRAME_COUNT * RSS_PAGE_SIZE);
			if (invalidEnvelope != 0U) {
				throwCatalogPoolError(FormatError::FERR_INVALID_HEADER,
									  "relocatable metadata pool envelope does not describe a metadata page pool");
			}
			std::array<std::byte, RSS_METADATA_PAGE_POOL_HEADER_BYTES> headerBytes{};
			std::copy_n(controlPage.begin() + RSS_ENVELOPE_BYTES, headerBytes.size(), headerBytes.begin());
			MetadataPagePoolHeader header = MetadataPagePoolHeaderCodec::decode(headerBytes);
			if (header.physical_slot != physicalSlot || header.segment_id != envelope.segment_id) {
				throwCatalogPoolError(FormatError::FERR_INVALID_HEADER,
									  "relocatable metadata pool header has a mismatched physical slot");
			}
			pools_.emplace(slot, PoolState{header});
		}
	}

	void MetadataPagePoolAllocator::discoverReachablePoolsLocked(const Superblock &superblock) {
		pools_.clear();
		MetadataPageFile pages(io_, superblock);
		std::set<std::pair<uint64_t, uint8_t>> visitedPages;
		std::set<uint64_t> reachableSlots;

		const auto visitDirectory = [&](auto &&self, PhysicalPageRef reference, MetadataDirectoryType directoryType,
										bool root) -> void {
			if (!reference.isValid()) {
				throwCatalogPoolError(FormatError::FERR_INVALID_HEADER,
									  "relocatable metadata root has an invalid physical reference");
			}
			if (!visitedPages.emplace(reference.physical_slot.value(), reference.frame_index).second) {
				throwCatalogPoolError(FormatError::FERR_INVALID_STATE,
									  "relocatable metadata roots alias or cycle through one physical page");
			}
			reachableSlots.insert(reference.physical_slot.value());
			const MetadataRadixPage page = pages.readMetadataRadix(reference);
			const uint8_t invalid = static_cast<uint8_t>(page.kind != (root ? CatalogRadixPageKind::CRPK_ROOT
																			: CatalogRadixPageKind::CRPK_INTERNAL)) |
									static_cast<uint8_t>(page.directory_type != directoryType) |
									static_cast<uint8_t>(page.generation > superblock.generation) |
									static_cast<uint8_t>(page.committed_lsn > superblock.checkpoint_lsn);
			if (invalid != 0U) {
				throwCatalogPoolError(FormatError::FERR_INVALID_HEADER,
									  "relocatable metadata root has an invalid directory page");
			}
			for (const PhysicalPageRef child: page.children) {
				if (!child.isValid()) {
					continue;
				}
				reachableSlots.insert(child.physical_slot.value());
				if (page.level != 0U) {
					self(self, child, directoryType, false);
				}
			}
		};

		struct RootDescriptor {
			PhysicalPageRef reference;
			MetadataDirectoryType directory_type;
		};
		const std::array roots{
				RootDescriptor{superblock.catalog_root, MetadataDirectoryType::MDT_CATALOG},
				RootDescriptor{superblock.page_directory_root, MetadataDirectoryType::MDT_PAGE},
				RootDescriptor{superblock.extent_directory_root, MetadataDirectoryType::MDT_EXTENT},
				RootDescriptor{superblock.allocator_root, MetadataDirectoryType::MDT_ALLOCATOR},
				RootDescriptor{superblock.node_entity_directory_root, MetadataDirectoryType::MDT_NODE_ENTITY},
				RootDescriptor{superblock.edge_entity_directory_root, MetadataDirectoryType::MDT_EDGE_ENTITY},
				RootDescriptor{superblock.entity_property_directory_root, MetadataDirectoryType::MDT_ENTITY_PROPERTY},
				RootDescriptor{superblock.index_root_directory_root, MetadataDirectoryType::MDT_INDEX_ROOT},
				RootDescriptor{superblock.vector_location_directory_root, MetadataDirectoryType::MDT_VECTOR_LOCATION},
				RootDescriptor{superblock.vector_manifest_directory_root, MetadataDirectoryType::MDT_VECTOR_MANIFEST},
				RootDescriptor{superblock.large_value_manifest_directory_root,
							   MetadataDirectoryType::MDT_LARGE_VALUE_MANIFEST},
				RootDescriptor{superblock.state_directory_root, MetadataDirectoryType::MDT_STATE},
				RootDescriptor{superblock.retire_queue_root, MetadataDirectoryType::MDT_RETIRE_QUEUE},
		};
		for (const RootDescriptor root: roots) {
			if (root.reference.isValid()) {
				visitDirectory(visitDirectory, root.reference, root.directory_type, true);
			}
		}

		for (const uint64_t slotValue: reachableSlots) {
			const PhysicalSlotId physicalSlot(slotValue);
			const std::array<std::byte, RSS_PAGE_SIZE> controlPage = readRawControlPage(
					*io_, slotOffset(physicalSlot), "relocatable reachable metadata-pool control page is truncated");
			std::array<std::byte, RSS_ENVELOPE_BYTES> envelopeBytes{};
			std::copy_n(controlPage.begin(), envelopeBytes.size(), envelopeBytes.begin());
			const SegmentEnvelope envelope = SegmentEnvelopeCodec::decode(envelopeBytes);
			const uint8_t invalidEnvelope =
					static_cast<uint8_t>(envelope.kind != SegmentKind::SGK_METADATA_PAGE_POOL) |
					static_cast<uint8_t>(envelope.state != SegmentState::SGS_SEALED) |
					static_cast<uint8_t>(envelope.owner_id != slotValue) |
					static_cast<uint8_t>(envelope.header_bytes != RSS_PAGE_SIZE) |
					static_cast<uint8_t>(envelope.payload_bytes != RSS_PAGE_POOL_DATA_FRAME_COUNT * RSS_PAGE_SIZE);
			if (invalidEnvelope != 0U) {
				throwCatalogPoolError(FormatError::FERR_INVALID_HEADER,
									  "relocatable reachable metadata slot does not describe a metadata page pool");
			}
			std::array<std::byte, RSS_METADATA_PAGE_POOL_HEADER_BYTES> headerBytes{};
			std::copy_n(controlPage.begin() + RSS_ENVELOPE_BYTES, headerBytes.size(), headerBytes.begin());
			const MetadataPagePoolHeader header = MetadataPagePoolHeaderCodec::decode(headerBytes);
			if (header.physical_slot != physicalSlot || header.segment_id != envelope.segment_id) {
				throwCatalogPoolError(FormatError::FERR_INVALID_HEADER,
									  "relocatable reachable metadata-pool header has a mismatched physical slot");
			}
			pools_.emplace(slotValue, PoolState{header});
		}
		if (emergency_reserve_.isValid()) {
			const PoolState reserve = readPoolLocked(emergency_reserve_.physical_slot, emergency_reserve_.pool_token,
													 emergency_reserve_.generation, emergency_reserve_.committed_lsn);
			const auto [iterator, inserted] = pools_.emplace(emergency_reserve_.physical_slot.value(), reserve);
			if (!inserted) {
				// The reserve is bootstrap-only capacity for ENOSPC handling. It must
				// never overlap a visible metadata tree, even if a corrupted file makes
				// both ownership records look identical.
				static_cast<void>(iterator);
				throwCatalogPoolError(FormatError::FERR_INVALID_STATE,
									  "relocatable metadata emergency reserve aliases a root-reachable pool");
			}
		}
	}

	MetadataPagePoolAllocator::PoolState &
	MetadataPagePoolAllocator::createPoolLocked(Generation generation, LogSequenceNumber checkpointLsn,
												std::span<const PhysicalSlotId> excludedPoolSlots) {
		requireHealthyLocked();
		const uint64_t fileSize = io_->fileSize();
		const uint8_t invalidTail = static_cast<uint8_t>(fileSize < data_region_offset_) |
									static_cast<uint8_t>((fileSize - data_region_offset_) % RSS_SEGMENT_SIZE != 0U);
		if (invalidTail != 0U ||
			fileSize - data_region_offset_ > (std::numeric_limits<uint64_t>::max() - 1U) / RSS_SEGMENT_SIZE) {
			throwCatalogPoolError(FormatError::FERR_ARITHMETIC_OVERFLOW,
								  "relocatable metadata pool file cannot append another physical slot");
		}
		uint64_t nextSlot = (fileSize - data_region_offset_) / RSS_SEGMENT_SIZE + 1U;
		while (std::ranges::any_of(excludedPoolSlots,
								   [nextSlot](PhysicalSlotId slot) { return slot.value() == nextSlot; })) {
			if (nextSlot == std::numeric_limits<uint64_t>::max()) {
				throwCatalogPoolError(FormatError::FERR_ARITHMETIC_OVERFLOW,
									  "relocatable metadata pool cannot skip a reserved maximum physical slot");
			}
			// Data and metadata append into one physical file.  Claim a skipped data
			// slot as part of this unpublished batch so the next metadata pool still
			// starts at the physical file tail.  MetadataWriteBatch reclaims this
			// complete provisional suffix if its Superblock switch does not publish.
			const uint64_t reservedOffset = slotOffset(PhysicalSlotId(nextSlot));
			if (io_->reserveAppendSpace(RSS_SEGMENT_SIZE) != reservedOffset) {
				throwCatalogPoolError(FormatError::FERR_INVALID_STATE,
									  "relocatable metadata pool reserved an unexpected physical tail offset");
			}
			++nextSlot;
		}
		if (nextSlot == 0U || pools_.contains(nextSlot)) {
			throwCatalogPoolError(FormatError::FERR_ARITHMETIC_OVERFLOW,
								  "relocatable metadata pool slot space is exhausted");
		}
		return createPoolAtLocked(PhysicalSlotId(nextSlot), SegmentId(nextSlot), generation, checkpointLsn);
	}

	MetadataPagePoolAllocator::PoolState &
	MetadataPagePoolAllocator::createPoolAtLocked(PhysicalSlotId physicalSlot, SegmentId segmentId,
												  Generation generation, LogSequenceNumber checkpointLsn) {
		requireHealthyLocked();
		if (!physicalSlot.isValid() || !segmentId.isValid() || !generation.isValid() || !checkpointLsn.isValid() ||
			pools_.contains(physicalSlot.value())) {
			throwCatalogPoolError(FormatError::FERR_INVALID_STATE,
								  "relocatable metadata pool creation has an invalid or occupied physical slot");
		}
		const uint64_t offset = slotOffset(physicalSlot);
		if (io_->fileSize() != offset) {
			throwCatalogPoolError(FormatError::FERR_INVALID_STATE,
								  "relocatable metadata pool creation requires the next unused physical slot");
		}
		SegmentEnvelope envelope;
		envelope.header_bytes = RSS_PAGE_SIZE;
		envelope.kind = SegmentKind::SGK_METADATA_PAGE_POOL;
		envelope.state = SegmentState::SGS_SEALED;
		envelope.segment_id = segmentId;
		envelope.owner_id = physicalSlot.value();
		envelope.generation = generation;
		envelope.committed_lsn = checkpointLsn;
		envelope.payload_bytes = RSS_PAGE_POOL_DATA_FRAME_COUNT * RSS_PAGE_SIZE;
		MetadataPagePoolHeader header;
		header.physical_slot = physicalSlot;
		header.segment_id = envelope.segment_id;
		header.generation = generation;
		header.committed_lsn = checkpointLsn;
		if (io_->reserveAppendSpace(RSS_SEGMENT_SIZE) != offset) {
			throwCatalogPoolError(FormatError::FERR_INVALID_STATE,
								  "relocatable metadata pool reserved an unexpected physical tail offset");
		}
		writeTypedControlPage(*io_, offset, generation, makeControlPage(envelope, header));
		auto [iterator, inserted] = pools_.emplace(physicalSlot.value(), PoolState{header});
		static_cast<void>(inserted);
		return iterator->second;
	}

	void MetadataPagePoolAllocator::eraseUnpublishedPoolLocked(PhysicalSlotId slot) {
		auto pool = pools_.find(slot.value());
		if (!slot.isValid() || pool == pools_.end() || pool->second.header.allocated_frames != 0U) {
			throwCatalogPoolError(FormatError::FERR_INVALID_STATE,
								  "relocatable metadata pool cannot abandon a non-empty or unknown slot");
		}
		std::array<std::byte, RSS_PAGE_SIZE> zeros{};
		const uint64_t offset = slotOffset(slot);
		for (uint64_t written = 0; written < RSS_SEGMENT_SIZE; written += zeros.size()) {
			io_->writeAt(offset + written, zeros.data(), zeros.size());
		}
		io_->sync();
		// A failed COW batch may have appended this private pool before its
		// Superblock publication. Reclaim a tail pool immediately so repeated
		// failed publications neither leak file space nor skip reusable slots.
		const uint64_t expectedTail = offset + RSS_SEGMENT_SIZE;
		if (expectedTail < offset) {
			throwCatalogPoolError(FormatError::FERR_ARITHMETIC_OVERFLOW,
								  "relocatable metadata pool tail offset overflows");
		}
		if (io_->fileSize() == expectedTail && io_->hasPwriteSupport()) {
			try {
				io_->truncate(offset);
			} catch (const std::exception &) {
				throwCatalogPoolError(FormatError::FERR_INVALID_STATE,
									  "relocatable metadata pool cannot reclaim an unpublished tail slot");
			}
			io_->sync();
		}
		pools_.erase(pool);
	}

	void MetadataPagePoolAllocator::abandonUnpublishedBatchLocked(std::span<const PhysicalSlotId> poolSlots,
																  std::span<const PhysicalSlotId> dataSlots,
																  uint64_t initialFileSize) {
		const uint64_t currentFileSize = io_->fileSize();
		const uint8_t invalidSizes =
				static_cast<uint8_t>(initialFileSize < data_region_offset_) |
				static_cast<uint8_t>(currentFileSize < initialFileSize) |
				static_cast<uint8_t>((initialFileSize - data_region_offset_) % RSS_SEGMENT_SIZE != 0U) |
				static_cast<uint8_t>((currentFileSize - data_region_offset_) % RSS_SEGMENT_SIZE != 0U);
		if (invalidSizes != 0U) {
			throwCatalogPoolError(FormatError::FERR_INVALID_STATE,
								  "relocatable metadata batch has an invalid provisional physical-tail boundary");
		}
		const uint64_t initialSlotCount = (initialFileSize - data_region_offset_) / RSS_SEGMENT_SIZE;
		const uint64_t currentSlotCount = (currentFileSize - data_region_offset_) / RSS_SEGMENT_SIZE;
		const auto validateProvisionalSlot = [&](PhysicalSlotId slot) {
			if (!slot.isValid()) {
				throwCatalogPoolError(FormatError::FERR_INVALID_STATE,
									  "relocatable metadata batch has an invalid provisional physical slot");
			}
		};
		for (const PhysicalSlotId slot: poolSlots) {
			validateProvisionalSlot(slot);
		}
		for (const PhysicalSlotId slot: dataSlots) {
			validateProvisionalSlot(slot);
		}

		// Metadata writers are serialized.  Therefore every physical byte beyond
		// the size captured at batch construction belongs to this unpublished
		// batch, including sparse gaps introduced when a caller materializes a
		// later data slot.  Treat the complete suffix as provisional so cleanup
		// never leaves a stranded tail gap after a failed root switch.
		const uint64_t retainedSlotCount = initialSlotCount;
		const bool canTruncateTail = retainedSlotCount < currentSlotCount && io_->hasPwriteSupport();

		std::array<std::byte, RSS_PAGE_SIZE> zeros{};
		const auto zeroSlot = [&](PhysicalSlotId slot) {
			const uint64_t offset = slotOffset(slot);
			for (uint64_t written = 0U; written < RSS_SEGMENT_SIZE; written += zeros.size()) {
				io_->writeAt(offset + written, zeros.data(), zeros.size());
			}
		};
		for (const PhysicalSlotId slot: poolSlots) {
			const auto pool = pools_.find(slot.value());
			if (!slot.isValid() || pool == pools_.end() || pool->second.header.allocated_frames != 0U) {
				throwCatalogPoolError(FormatError::FERR_INVALID_STATE,
									  "relocatable metadata batch cannot abandon a non-empty or unknown pool");
			}
			if (!canTruncateTail || slot.value() <= retainedSlotCount) {
				zeroSlot(slot);
			}
			pools_.erase(pool);
		}
		for (const PhysicalSlotId slot: dataSlots) {
			// A reservation is logical until its writer actually materializes the
			// corresponding tail slot.  Cleanup must never extend the database just
			// to wipe a reservation that did not write any bytes.
			if (slot.isValid() && slot.value() > initialSlotCount && slot.value() <= currentSlotCount &&
				(!canTruncateTail || slot.value() <= retainedSlotCount)) {
				zeroSlot(slot);
			}
		}
		io_->sync();
		if (canTruncateTail) {
			const uint64_t truncateOffset =
					data_region_offset_ + retainedSlotCount * static_cast<uint64_t>(RSS_SEGMENT_SIZE);
			io_->truncate(truncateOffset);
			io_->sync();
		}
	}

	void MetadataPagePoolAllocator::writePoolLocked(const PoolState &pool) {
		std::array<std::byte, RSS_PAGE_SIZE> controlPage{};
		static_cast<void>(readPoolLocked(pool.header.physical_slot, pool.header.segment_id, pool.header.generation,
										 pool.header.committed_lsn, &controlPage));
		const auto bytes = MetadataPagePoolHeaderCodec::encode(pool.header);
		std::copy(bytes.begin(), bytes.end(), controlPage.begin() + RSS_ENVELOPE_BYTES);
		writeTypedControlPage(*io_, slotOffset(pool.header.physical_slot), pool.header.generation, controlPage);
	}

	void MetadataPagePoolAllocator::validateRegisteredPoolsLocked() const {
		for (const auto &[slot, pool]: pools_) {
			const PoolState durable = readPoolLocked(PhysicalSlotId(slot), pool.header.segment_id,
													 pool.header.generation, pool.header.committed_lsn);
			if (durable.header.allocated_frames != pool.header.allocated_frames) {
				throwCatalogPoolError(FormatError::FERR_INVALID_STATE,
									  "relocatable metadata-pool allocation bitmap diverges from durable state");
			}
		}
	}

	void MetadataPagePoolAllocator::requireHealthyLocked() const {
		if (poisoned_) {
			throwCatalogPoolError(FormatError::FERR_INVALID_STATE,
								  "relocatable metadata page-pool allocator is poisoned and requires recovery");
		}
	}

} // namespace graph::storage
