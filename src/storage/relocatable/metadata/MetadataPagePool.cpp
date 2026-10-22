/**
 * @file MetadataPagePool.cpp
 * @brief Checked fixed-slot page-pool allocation for isolated Catalog metadata.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/property/HybridPropertyDescriptor.hpp"
#include "graph/storage/relocatable/metadata/MetadataPagePool.hpp"

#include "graph/storage/relocatable/allocation/AllocatorDirectory.hpp"
#include "graph/storage/relocatable/format/SuperblockFile.hpp"
#include "graph/storage/relocatable/metadata/MetadataPageRouteDirectory.hpp"

#include <algorithm>
#include <array>
#include "graph/debug/PerfTrace.hpp"
#include <bit>
#include <functional>
#include <iterator>
#include <limits>
#include <optional>
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

		[[nodiscard]] MetadataDirectoryRootOrdinal rootOrdinal(MetadataDirectoryType directoryType) {
			switch (directoryType) {
				case MetadataDirectoryType::MDT_CATALOG:
					return MetadataDirectoryRootOrdinal::MDRO_CATALOG;
				case MetadataDirectoryType::MDT_ALLOCATOR:
					return MetadataDirectoryRootOrdinal::MDRO_ALLOCATOR;
				case MetadataDirectoryType::MDT_EXTENT:
					return MetadataDirectoryRootOrdinal::MDRO_EXTENT;
				case MetadataDirectoryType::MDT_NODE_ENTITY:
					return MetadataDirectoryRootOrdinal::MDRO_NODE_ENTITY;
				case MetadataDirectoryType::MDT_EDGE_ENTITY:
					return MetadataDirectoryRootOrdinal::MDRO_EDGE_ENTITY;
				case MetadataDirectoryType::MDT_ENTITY_PROPERTY:
					return MetadataDirectoryRootOrdinal::MDRO_ENTITY_PROPERTY;
				case MetadataDirectoryType::MDT_INDEX_ROOT:
					return MetadataDirectoryRootOrdinal::MDRO_INDEX_ROOT;
				case MetadataDirectoryType::MDT_VECTOR_LOCATION:
					return MetadataDirectoryRootOrdinal::MDRO_VECTOR_LOCATION;
				case MetadataDirectoryType::MDT_VECTOR_MANIFEST:
					return MetadataDirectoryRootOrdinal::MDRO_VECTOR_MANIFEST;
				case MetadataDirectoryType::MDT_LARGE_VALUE_MANIFEST:
					return MetadataDirectoryRootOrdinal::MDRO_LARGE_VALUE_MANIFEST;
				case MetadataDirectoryType::MDT_STATE:
					return MetadataDirectoryRootOrdinal::MDRO_STATE;
				case MetadataDirectoryType::MDT_RETIRE_QUEUE:
					return MetadataDirectoryRootOrdinal::MDRO_RETIRE_QUEUE;
				default: // reconcileMetadataPagePoolLocked calls rootOrdinal only for its fixed non-Page directory list.
					throwCatalogPoolError(FormatError::FERR_INVALID_ENUM,
										  "relocatable metadata pool has no typed root ordinal");
			}
		}

		[[nodiscard]] PhysicalPageRef metadataRootReference(MetadataPageFile &pages, const Superblock &superblock,
															MetadataDirectoryType directoryType,
															PhysicalPageRef physicalRoot) {
			if (!superblock.usesPageDirectoryBootstrap()) {
				return physicalRoot;
			}
			if (!superblock.page_directory_root.isValid()) {
				throwCatalogPoolError(FormatError::FERR_INVALID_HEADER,
									  "Page Directory bootstrap has no physical header reference");
			}
			const PageDirectoryBootstrap bootstrap = pages.readPageDirectoryBootstrap(superblock.page_directory_root);
			if (bootstrap.generation > superblock.generation || bootstrap.committed_lsn > superblock.checkpoint_lsn ||
				!bootstrap.metadata_page_directory_root.isValid()) {
				throwCatalogPoolError(FormatError::FERR_INVALID_HEADER,
									  "Page Directory bootstrap has invalid visibility or route branch");
			}
			if (directoryType == MetadataDirectoryType::MDT_PAGE) { // Page Directory routing is handled by reconcileMetadataPagePoolLocked before its non-Page root loop.
				return bootstrap.data_page_directory_root;
			}
			const MetadataPageId rootId =
					bootstrap.metadata_directory_roots[static_cast<size_t>(rootOrdinal(directoryType))];
			if (!rootId.isValid()) {
				return {};
			}
			return MetadataPageRouteDirectory(pages).find(superblock, bootstrap.metadata_page_directory_root, rootId);
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
					static_cast<uint8_t>((header.flags & ~MPPF_KNOWN_FLAGS) != 0U) |
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
			LittleEndianCodec::writeU32(bytes, RESERVED0_OFFSET, header.flags);
			LittleEndianCodec::writeU64(bytes, SEGMENT_ID_OFFSET, header.segment_id.value());
			LittleEndianCodec::writeU64(bytes, RESERVED2_OFFSET, header.control_sequence);
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
			MetadataPagePoolHeader active = header;
			active.control_sequence = 1U;
			MetadataPagePoolHeader inactive = header;
			inactive.control_sequence = 0U;
			const auto activeBytes = MetadataPagePoolHeaderCodec::encode(active);
			const auto inactiveBytes = MetadataPagePoolHeaderCodec::encode(inactive);
			std::copy(envelopeBytes.begin(), envelopeBytes.end(), page.begin());
			std::copy(activeBytes.begin(), activeBytes.end(), page.begin() + RSS_ENVELOPE_BYTES);
			std::copy(inactiveBytes.begin(), inactiveBytes.end(),
					  page.begin() + RSS_ENVELOPE_BYTES + RSS_METADATA_PAGE_POOL_HEADER_BYTES);
			return page;
		}

		struct ControlHeaderSelection {
			MetadataPagePoolHeader header;
			size_t offset = 0U;
		};

		[[nodiscard]] ControlHeaderSelection selectControlHeader(std::span<const std::byte, RSS_PAGE_SIZE> page) {
			std::optional<ControlHeaderSelection> selected;
			for (size_t record = 0U; record < RSS_METADATA_PAGE_POOL_CONTROL_RECORD_COUNT; ++record) {
				const size_t offset = RSS_ENVELOPE_BYTES + record * RSS_METADATA_PAGE_POOL_HEADER_BYTES;
				try {
					const MetadataPagePoolHeader header = MetadataPagePoolHeaderCodec::decode(
							page.subspan(offset, RSS_METADATA_PAGE_POOL_HEADER_BYTES));
					if (!selected.has_value() || header.control_sequence > selected->header.control_sequence) {
						selected = {header, offset};
					}
				} catch (const FormatException &) {
					// The complete peer remains authoritative after a torn inactive record.
				}
			}
			if (!selected.has_value()) {
				throwCatalogPoolError(FormatError::FERR_CHECKSUM_MISMATCH,
									  "relocatable metadata pool has no valid control record");
			}
			return *selected;
		}

		uint64_t rewriteInactiveControlRecord(StorageIO &io, uint64_t pageOffset, Generation generation,
										  const MetadataPagePoolHeader &replacement, const char *truncatedMessage) {
			const std::array<std::byte, RSS_PAGE_SIZE> page =
					readTypedControlPage(io, pageOffset, generation, truncatedMessage);
			const ControlHeaderSelection current = selectControlHeader(page);
			const uint8_t invalidIdentity =
					static_cast<uint8_t>(replacement.physical_slot != current.header.physical_slot) |
					static_cast<uint8_t>(replacement.segment_id != current.header.segment_id) |
					static_cast<uint8_t>(replacement.generation != current.header.generation) |
					static_cast<uint8_t>(replacement.committed_lsn != current.header.committed_lsn) |
					static_cast<uint8_t>(current.header.control_sequence == std::numeric_limits<uint64_t>::max());
			if (invalidIdentity != 0U) {
				throwCatalogPoolError(FormatError::FERR_INVALID_STATE,
									  "relocatable metadata pool control record has invalid replacement identity");
			}
			MetadataPagePoolHeader next = replacement;
			next.control_sequence = current.header.control_sequence + 1U;
			const auto bytes = MetadataPagePoolHeaderCodec::encode(next);
			const size_t targetOffset = current.offset == RSS_ENVELOPE_BYTES
													? RSS_ENVELOPE_BYTES + RSS_METADATA_PAGE_POOL_HEADER_BYTES
													: RSS_ENVELOPE_BYTES;
			io.writeControlRecord({pageOffset, generation.value()}, targetOffset, bytes);
			return next.control_sequence;
		}

		// Batch allocation already holds the last authenticated control header for
		// each pool. Reuse that identity instead of rereading the whole control
		// page; the normal path above remains the fail-closed fallback for callers
		// that do not have an authenticated in-memory header.
		uint64_t rewriteInactiveControlRecordKnown(StorageIO &io, uint64_t pageOffset, Generation generation,
										  const MetadataPagePoolHeader &current,
										  const MetadataPagePoolHeader &replacement) {
			validateHeader(current);
			validateHeader(replacement);
			const uint8_t invalidIdentity =
					static_cast<uint8_t>(replacement.physical_slot != current.physical_slot) |
					static_cast<uint8_t>(replacement.segment_id != current.segment_id) |
					static_cast<uint8_t>(replacement.generation != current.generation) |
					static_cast<uint8_t>(replacement.committed_lsn != current.committed_lsn) |
					static_cast<uint8_t>(current.control_sequence == std::numeric_limits<uint64_t>::max()) |
					static_cast<uint8_t>(generation != current.generation);
			if (invalidIdentity != 0U) {
				throwCatalogPoolError(FormatError::FERR_INVALID_STATE,
									  "relocatable metadata pool control record has invalid known identity");
			}
			MetadataPagePoolHeader next = replacement;
			next.control_sequence = current.control_sequence + 1U;
			const auto bytes = MetadataPagePoolHeaderCodec::encode(next);
			const size_t firstOffset = RSS_ENVELOPE_BYTES;
			const size_t secondOffset = RSS_ENVELOPE_BYTES + RSS_METADATA_PAGE_POOL_HEADER_BYTES;
			const size_t currentOffset = (current.control_sequence & 1U) != 0U ? firstOffset : secondOffset;
			const size_t targetOffset = currentOffset == firstOffset ? secondOffset : firstOffset;
			io.writeControlRecord({pageOffset, generation.value()}, targetOffset, bytes);
			return next.control_sequence;
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
		const uint32_t flags = LittleEndianCodec::readU32(bytes, RESERVED0_OFFSET);
		const uint8_t invalidReserved = static_cast<uint8_t>(LittleEndianCodec::readU16(bytes, HEADER_BYTES_OFFSET) !=
															 RSS_METADATA_PAGE_POOL_HEADER_BYTES) |
										static_cast<uint8_t>((flags & ~MPPF_KNOWN_FLAGS) != 0U);
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
		header.control_sequence = LittleEndianCodec::readU64(bytes, RESERVED2_OFFSET);
		header.flags = flags;
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
		SuperblockCodec::validate(superblock);
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
            rebuildPoolAvailabilityLocked();
		}
		if (openMode == MetadataPagePoolOpenMode::CPOM_RECOVER ||
			openMode == MetadataPagePoolOpenMode::CPOM_ROOT_RECOVER) {
			recover(superblock);
		}
	}

	MetadataPageId MetadataPagePoolAllocator::allocateMetadataPageId() {
		std::lock_guard lock(mutex_);
		requireHealthyLocked();
		if (page_id_exhausted_) {
			throwCatalogPoolError(FormatError::FERR_ARITHMETIC_OVERFLOW,
								  "relocatable Catalog Page ID space is exhausted");
		}
		const MetadataPageId pageId = next_page_id_;
		if (pageId.value() == std::numeric_limits<uint64_t>::max()) {
			page_id_exhausted_ = true;
		} else {
			next_page_id_ = MetadataPageId(pageId.value() + 1U);
		}
		return pageId;
	}

	MetadataPageAllocation MetadataPagePoolAllocator::allocatePage(Generation generation,
																   LogSequenceNumber checkpointLsn,
																   std::span<const PhysicalSlotId> excludedPoolSlots) {
		return allocatePage(generation, checkpointLsn, excludedPoolSlots, {});
	}

	MetadataPageAllocation
	MetadataPagePoolAllocator::allocatePage(Generation generation, LogSequenceNumber checkpointLsn,
											std::span<const PhysicalSlotId> excludedPoolSlots,
											std::span<const PhysicalSlotRange> excludedDataSlotRanges) {
		return allocatePage(generation, checkpointLsn, excludedPoolSlots, excludedDataSlotRanges, nullptr);
	}

	MetadataPageAllocation
	MetadataPagePoolAllocator::allocatePage(Generation generation, LogSequenceNumber checkpointLsn,
											std::span<const PhysicalSlotId> excludedPoolSlots,
											std::span<const PhysicalSlotRange> excludedDataSlotRanges,
											const MetadataSlotReservationLookup *reservationLookup) {
		std::lock_guard lock(mutex_);
		requireHealthyLocked();
        if (!active_batches_.contains(generation.value())) retirement_base_.reset();
		const uint8_t invalidGeneration = static_cast<uint8_t>(!generation.isValid()) |
										  static_cast<uint8_t>(generation.value() > RSS_MAX_PHYSICAL_PAGE_GENERATION) |
										  static_cast<uint8_t>(!checkpointLsn.isValid());
		if (invalidGeneration != 0U) {
			throwCatalogPoolError(FormatError::FERR_INVALID_HEADER,
								  "relocatable Catalog page allocation context is invalid");
		}
		const bool metadataReservationActive = reserved_metadata_frames_.has_value();
		if (metadataReservationActive && *reserved_metadata_frames_ == 0U) {
			throwCatalogPoolError(FormatError::FERR_INVALID_STATE,
							  "relocatable metadata frame reservation was exhausted");
		}
		const auto excluded = [this, &excludedPoolSlots, &excludedDataSlotRanges,
							   reservationLookup](PhysicalSlotId slot) {
			if (snapshot_protected_pool_slots_.contains(slot.value())) {
				return true;
			}
			return reservationLookup != nullptr
						   ? reservationLookup->isDataSlotReserved(slot)
						   : std::ranges::any_of(excludedPoolSlots, [slot](PhysicalSlotId candidate) {
								 return candidate == slot;
							 }) || std::ranges::any_of(excludedDataSlotRanges, [slot](PhysicalSlotRange range) {
								 return range.contains(slot);
							 });
		};
        auto pool = pools_.end();
        for (uint64_t slot = available_pool_head_; slot != 0U; slot = pools_.at(slot).available_next) {
            debug::PerfTrace::addValue("native_metadata.capacity.visited_pools", 1);
            if (!excluded(PhysicalSlotId(slot))) { pool = pools_.find(slot); break; }
        }
        std::optional<PhysicalSlotId> createdPoolSlot;
		if (pool == pools_.end()) {
			if (metadataReservationActive) {
				throwCatalogPoolError(FormatError::FERR_INVALID_STATE,
								  "relocatable metadata frame reservation cannot allocate a new pool");
			}
			try {
				PoolState &created = createPoolLocked(generation, checkpointLsn, excludedPoolSlots,
													  excludedDataSlotRanges, reservationLookup);
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
		allocations_since_reconciliation_ = std::min<uint32_t>(
				allocations_since_reconciliation_ + 1U, PUBLICATION_RECLAIM_ALLOCATION_BATCH_FRAMES);
		PoolState updatedPool{updated};
		writePoolLocked(updatedPool);
		pool->second.header = std::move(updatedPool.header);
        refreshPoolAvailabilityLocked(pool->second.header.physical_slot);
		if (metadataReservationActive) {
			--*reserved_metadata_frames_;
		}
		if (pool->second.header.allocated_frames == RSS_METADATA_PAGE_POOL_VALID_FRAME_MASK) {
			auto next = std::next(pool);
			allocation_cursor_slot_ =
					next == pools_.end() ? std::optional<uint64_t>{} : std::optional<uint64_t>{next->first};
		} else {
			allocation_cursor_slot_ = pool->first;
		}
		return MetadataPageAllocation{PhysicalPageRef{pool->second.header.physical_slot, frame, generation},
									  createdPoolSlot};
	}

	std::vector<MetadataPageAllocation> MetadataPagePoolAllocator::allocatePages(
			Generation generation, LogSequenceNumber checkpointLsn, size_t count,
			std::span<const PhysicalSlotId> excludedPoolSlots,
			std::span<const PhysicalSlotRange> excludedDataSlotRanges,
			const MetadataSlotReservationLookup *reservationLookup) {
		std::lock_guard lock(mutex_);
		requireHealthyLocked();
        if (!active_batches_.contains(generation.value())) retirement_base_.reset();
		const uint8_t invalid = static_cast<uint8_t>(!generation.isValid()) |
								static_cast<uint8_t>(generation.value() > RSS_MAX_PHYSICAL_PAGE_GENERATION) |
								static_cast<uint8_t>(!checkpointLsn.isValid()) | static_cast<uint8_t>(count == 0U);
		if (invalid != 0U) {
			throwCatalogPoolError(FormatError::FERR_INVALID_HEADER,
								  "relocatable Catalog page batch allocation context is invalid");
		}
		const bool metadataReservationActive = reserved_metadata_frames_.has_value();
		if (metadataReservationActive && count > *reserved_metadata_frames_) {
			throwCatalogPoolError(FormatError::FERR_INVALID_STATE,
								  "relocatable metadata frame reservation was exhausted");
		}
		const auto excluded = [this, &excludedPoolSlots, &excludedDataSlotRanges,
							   reservationLookup](PhysicalSlotId slot) {
			if (snapshot_protected_pool_slots_.contains(slot.value())) {
				return true;
			}
			return reservationLookup != nullptr
					   ? reservationLookup->isDataSlotReserved(slot)
					   : std::ranges::any_of(excludedPoolSlots, [slot](PhysicalSlotId candidate) {
							 return candidate == slot;
						 }) || std::ranges::any_of(excludedDataSlotRanges, [slot](PhysicalSlotRange range) {
							 return range.contains(slot);
						 });
		};

		std::vector<MetadataPageAllocation> allocations;
		allocations.reserve(count);
		std::map<uint64_t, MetadataPagePoolHeader> originalHeaders;
		std::vector<PhysicalSlotId> createdPoolSlots;
		std::vector<uint64_t> persistedSlots;
		const std::optional<uint64_t> originalCursor = allocation_cursor_slot_;
		bool reusableSlotSearchPending = true;
		try {
			while (allocations.size() < count) {
                auto pool = pools_.end();
                for (uint64_t slot = available_pool_head_; slot != 0U; slot = pools_.at(slot).available_next) {
                    debug::PerfTrace::addValue("native_metadata.capacity.visited_pools", 1);
                    if (!excluded(PhysicalSlotId(slot))) { pool = pools_.find(slot); break; }
                }

				std::optional<PhysicalSlotId> createdPoolSlot;
				if (pool == pools_.end()) {
					if (metadataReservationActive) {
						throwCatalogPoolError(FormatError::FERR_INVALID_STATE,
												  "relocatable metadata frame reservation cannot allocate a new pool");
					}
					try {
						PoolState &created = createPoolLocked(generation, checkpointLsn, excludedPoolSlots,
								excludedDataSlotRanges, reservationLookup, reusableSlotSearchPending);
						reusableSlotSearchPending = false;
						createdPoolSlot = created.header.physical_slot;
						createdPoolSlots.push_back(*createdPoolSlot);
						pool = pools_.find(created.header.physical_slot.value());
					} catch (const std::system_error &error) {
						if (!isNoSpaceError(error) || !emergency_reserve_.isValid() ||
							excluded(emergency_reserve_.physical_slot)) {
							throw;
						}
						pool = pools_.find(emergency_reserve_.physical_slot.value());
						if (pool == pools_.end() ||
							pool->second.header.allocated_frames == RSS_METADATA_PAGE_POOL_VALID_FRAME_MASK) {
							throwCatalogPoolError(
									FormatError::FERR_INVALID_STATE,
									"relocatable metadata emergency reserve has no free page frame");
						}
					}
				}

				originalHeaders.try_emplace(pool->first, pool->second.header);
				uint32_t freeFrames = RSS_METADATA_PAGE_POOL_VALID_FRAME_MASK &
									  ~pool->second.header.allocated_frames;
				bool reportsCreatedPool = createdPoolSlot.has_value();
				while (freeFrames != 0U && allocations.size() < count) {
					const uint8_t frame =
							static_cast<uint8_t>(std::countr_zero(freeFrames) + RSS_PAGE_POOL_FIRST_DATA_FRAME);
					pool->second.header.allocated_frames |= 1U << (frame - RSS_PAGE_POOL_FIRST_DATA_FRAME);
					allocations_since_reconciliation_ = std::min<uint32_t>(
							allocations_since_reconciliation_ + 1U, PUBLICATION_RECLAIM_ALLOCATION_BATCH_FRAMES);
					allocations.push_back(
							{PhysicalPageRef{pool->second.header.physical_slot, frame, generation},
							 reportsCreatedPool ? createdPoolSlot : std::optional<PhysicalSlotId>{}});
					reportsCreatedPool = false;
					freeFrames &= ~(1U << (frame - RSS_PAGE_POOL_FIRST_DATA_FRAME));
				}
                refreshPoolAvailabilityLocked(pool->second.header.physical_slot);
				if (pool->second.header.allocated_frames == RSS_METADATA_PAGE_POOL_VALID_FRAME_MASK) {
					auto next = std::next(pool);
					allocation_cursor_slot_ =
							next == pools_.end() ? std::optional<uint64_t>{} : std::optional<uint64_t>{next->first};
				} else {
					allocation_cursor_slot_ = pool->first;
				}
			}

			persistedSlots.reserve(originalHeaders.size());
			for (const auto &[slot, original]: originalHeaders) {
				PoolState &pool = pools_.at(slot);
				writePoolLockedKnownHeader(pool, original);
				persistedSlots.push_back(slot);
			}
			if (metadataReservationActive) {
				*reserved_metadata_frames_ -= count;
			}
			return allocations;
		} catch (...) {
			const std::exception_ptr failure = std::current_exception();
			bool rollbackSucceeded = true;
			for (auto iterator = persistedSlots.rbegin(); iterator != persistedSlots.rend(); ++iterator) {
				try {
					writePoolLocked(PoolState{originalHeaders.at(*iterator)});
				} catch (...) {
					rollbackSucceeded = false;
				}
			}
			for (const auto &[slot, original]: originalHeaders) {
				if (auto pool = pools_.find(slot); pool != pools_.end()) {
					pool->second.header = original;
                        refreshPoolAvailabilityLocked(PhysicalSlotId(slot));
				}
			}
			for (auto iterator = createdPoolSlots.rbegin(); iterator != createdPoolSlots.rend(); ++iterator) {
				try {
					eraseUnpublishedPoolLocked(*iterator);
				} catch (...) {
					rollbackSucceeded = false;
				}
			}
			allocation_cursor_slot_ = originalCursor;
			if (!rollbackSucceeded) {
				poisoned_ = true;
			}
			std::rethrow_exception(failure);
		}
	}

	void MetadataPagePoolAllocator::beginUnpublishedBatch(const Superblock &current) {
		SuperblockCodec::validate(current);
		std::lock_guard lock(mutex_);
		requireHealthyLocked();
		if (current.generation.value() == std::numeric_limits<uint64_t>::max()) {
			throwCatalogPoolError(FormatError::FERR_ARITHMETIC_OVERFLOW,
								  "relocatable metadata allocator cannot advance the maximum batch generation");
		}
		const uint64_t candidateGeneration = current.generation.value() + 1U;
		auto &contexts = active_batches_[candidateGeneration];
		auto context = std::ranges::find(contexts, current, &ActiveBatchContext::superblock);
		if (context == contexts.end()) {
			contexts.push_back(ActiveBatchContext{current});
			context = std::prev(contexts.end());
		}
		if (context->depth == std::numeric_limits<uint32_t>::max()) {
			throwCatalogPoolError(FormatError::FERR_ARITHMETIC_OVERFLOW,
								  "relocatable metadata allocator unpublished batch nesting overflows");
		}
		++context->depth;
	}

	void MetadataPagePoolAllocator::endUnpublishedBatch(const Superblock &current) noexcept {
		try {
			std::lock_guard lock(mutex_);
			if (current.generation.value() == std::numeric_limits<uint64_t>::max()) {
				return;
			}
			auto contexts = active_batches_.find(current.generation.value() + 1U);
			if (contexts == active_batches_.end()) {
				return;
			}
			auto context = std::ranges::find(contexts->second, current, &ActiveBatchContext::superblock);
			if (context == contexts->second.end() || context->depth == 0U) {
				return;
			}
			--context->depth;
			if (context->depth == 0U) {
				contexts->second.erase(context);
			}
			if (contexts->second.empty()) {
				active_batches_.erase(contexts);
			}
		} catch (...) {
			// The context is only an in-memory allocation hint. A failed lock must
			// not change durable reachability or make a slot reusable.
		}
	}

	void MetadataPagePoolAllocator::reserveMetadataFrames(
			size_t count, std::span<const PhysicalSlotId> excludedPoolSlots,
			std::span<const PhysicalSlotRange> excludedDataSlotRanges,
			const MetadataSlotReservationLookup *reservationLookup) {
		std::lock_guard lock(mutex_);
		requireHealthyLocked();
		if (count == 0U || reserved_metadata_frames_.has_value()) {
			throwCatalogPoolError(FormatError::FERR_INVALID_STATE,
							  "relocatable metadata frame reservation is empty or already active");
		}
		const auto isExcluded = [&](PhysicalSlotId slot) {
			if (snapshot_protected_pool_slots_.contains(slot.value()) ||
				(emergency_reserve_.isValid() && slot == emergency_reserve_.physical_slot)) {
				return true;
			}
			if (reservationLookup != nullptr) {
				return reservationLookup->isDataSlotReserved(slot);
			}
			return std::ranges::any_of(excludedPoolSlots, [slot](PhysicalSlotId candidate) {
					return candidate == slot;
				}) ||
				   std::ranges::any_of(excludedDataSlotRanges, [slot](PhysicalSlotRange range) {
					   return range.contains(slot);
				   });
		};
		uint64_t available = 0U;
		for (uint64_t slot = available_pool_head_; slot != 0U; slot = pools_.at(slot).available_next) {
            const auto &pool = pools_.at(slot);
            debug::PerfTrace::addValue("native_metadata.capacity.visited_pools", 1);
			const PhysicalSlotId physicalSlot(slot);
			if (!isExcluded(physicalSlot)) {
				available += RSS_PAGE_POOL_DATA_FRAME_COUNT - std::popcount(pool.header.allocated_frames);
				if (available >= count) {
					break;
				}
			}
		}
		if (available < count) {
			throwCatalogPoolError(FormatError::FERR_INVALID_STATE,
							  "relocatable metadata frame reservation lacks existing free frames");
		}
		reserved_metadata_frames_ = count;
	}

	void MetadataPagePoolAllocator::releaseMetadataFrames() noexcept {
		try {
			std::lock_guard lock(mutex_);
			reserved_metadata_frames_.reset();
		} catch (...) {
			// Reservation cleanup is best effort; durable page ownership remains
			// protected by the batch rollback path if locking fails.
		}
	}

	std::vector<PhysicalSlotId> MetadataPagePoolAllocator::ensureMetadataFrameCapacity(
			Generation generation, LogSequenceNumber checkpointLsn, size_t count,
			std::span<const PhysicalSlotId> excludedPoolSlots,
			std::span<const PhysicalSlotRange> excludedDataSlotRanges,
			const MetadataSlotReservationLookup *reservationLookup) {
		std::lock_guard lock(mutex_);
		requireHealthyLocked();
		if (!generation.isValid() || !checkpointLsn.isValid() || count == 0U || reserved_metadata_frames_.has_value()) {
			throwCatalogPoolError(FormatError::FERR_INVALID_STATE,
							  "relocatable metadata route capacity requires an active unreserved write batch");
		}
		const auto excluded = [&](PhysicalSlotId slot) {
			if (snapshot_protected_pool_slots_.contains(slot.value()) ||
				(emergency_reserve_.isValid() && slot == emergency_reserve_.physical_slot)) {
				return true;
			}
			return reservationLookup != nullptr
					   ? reservationLookup->isDataSlotReserved(slot)
					   : std::ranges::any_of(excludedPoolSlots, [slot](PhysicalSlotId candidate) {
							 return candidate == slot;
						 }) || std::ranges::any_of(excludedDataSlotRanges, [slot](PhysicalSlotRange range) {
							 return range.contains(slot);
						 });
		};
		size_t available = 0U;
		for (uint64_t slot = available_pool_head_; slot != 0U && available < count; slot = pools_.at(slot).available_next) {
            const auto &pool = pools_.at(slot);
            debug::PerfTrace::addValue("native_metadata.capacity.visited_pools", 1);
            if (!excluded(PhysicalSlotId(slot))) available += pool.available_frames;
        }
		std::vector<PhysicalSlotId> created;
		while (available < count) {
			PoolState &pool = createPoolLocked(generation, checkpointLsn, excludedPoolSlots,
											excludedDataSlotRanges, reservationLookup);
			created.push_back(pool.header.physical_slot);
			available += RSS_PAGE_POOL_DATA_FRAME_COUNT;
		}
		return created;
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
            if (pool->second.retired_frames[reference.frame_index - RSS_PAGE_POOL_FIRST_DATA_FRAME].generation != 0U)
                throwCatalogPoolError(FormatError::FERR_INVALID_STATE, "cannot abandon a snapshot-retired metadata frame");
			MetadataPagePoolHeader updated = pool->second.header;
			updated.allocated_frames &= ~bit;
			PoolState updatedPool{updated};
			writePoolLocked(updatedPool);
			pool->second.header = std::move(updatedPool.header);
        refreshPoolAvailabilityLocked(pool->second.header.physical_slot);
			allocation_cursor_slot_.reset();
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
		abandonUnpublishedBatch(poolSlots, dataSlots, {}, initialFileSize);
	}

	void MetadataPagePoolAllocator::abandonUnpublishedBatch(std::span<const PhysicalSlotId> poolSlots,
															std::span<const PhysicalSlotId> dataSlots,
															std::span<const PhysicalSlotRange> dataSlotRanges,
															uint64_t initialFileSize) noexcept {
		try {
			std::lock_guard lock(mutex_);
			requireHealthyLocked();
			abandonUnpublishedBatchLocked(poolSlots, dataSlots, dataSlotRanges, initialFileSize);
		} catch (...) {
			// The batch owns only unpublished state.  If its cleanup cannot prove a
			// safe tail boundary, fail closed rather than reuse any of those slots.
			std::lock_guard lock(mutex_);
			poisoned_ = true;
		}
	}

	void MetadataPagePoolAllocator::recover(const Superblock &superblock) {
		SuperblockCodec::validate(superblock);
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

	void MetadataPagePoolAllocator::rebaseToPublishedSuperblock(const Superblock &superblock) {
		SuperblockCodec::validate(superblock);
		std::lock_guard lock(mutex_);
		if (superblock.data_region_offset != data_region_offset_) {
			throwCatalogPoolError(FormatError::FERR_INVALID_STATE,
								  "relocatable metadata pool cannot rebase a different data-region layout");
		}
		emergency_reserve_ = superblock.metadata_emergency_reserve;
		discoverReachablePoolsLocked(superblock);
        rebuildPoolAvailabilityLocked();
		poisoned_ = false;
	}

	void MetadataPagePoolAllocator::validateReachability(const Superblock &superblock) {
		SuperblockCodec::validate(superblock);
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
        if (inserted) refreshPoolAvailabilityLocked(slot);
		if (!inserted &&
			(iterator->second.header.segment_id != segmentId || iterator->second.header.generation != generation ||
			 iterator->second.header.committed_lsn != committedLsn)) {
			throwCatalogPoolError(FormatError::FERR_INVALID_STATE,
								  "relocatable Catalog metadata-pool ownership conflicts with a reachable pool");
		}
        unlinkUnconfirmedOwnershipLocked(slot);
        refreshPoolAvailabilityLocked(slot);
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
			// Route-capacity preflight can create an empty private pool before the
			// Catalog COW allocates its first frame; its slot-valued ID is unbound.
			const bool isUnboundEmptyPool = pool != pools_.end() && pool->second.header.allocated_frames == 0U &&
														pool->second.header.segment_id == SegmentId(slot.value());
			if (pool == pools_.end() || pool->second.header.generation != generation ||
				pool->second.header.committed_lsn != committedLsn ||
				(pool->second.header.segment_id != segmentId && !isUnboundEmptyPool &&
					 pool->second.header.allocated_frames == 0U)) {
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
			writeTypedControlPage(*io_, slotOffset(slot), generation, makeControlPage(envelope, updated.header));
			pool->second.header = std::move(updated.header);
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
											 reserve.committed_lsn, MPPF_EMERGENCY_RESERVE_ORIGIN));
		emergency_reserve_ = reserve;
        refreshPoolAvailabilityLocked(reserve.physical_slot);
	}

	MetadataEmergencyReserveDescriptor MetadataPagePoolAllocator::rotateEmergencyReserve(SegmentId nextPoolToken,
																						 Generation generation,
																						 LogSequenceNumber committedLsn,
																						 PhysicalSlotId successorSlot) {
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
		const uint64_t slotCount = (fileSize - data_region_offset_) / RSS_SEGMENT_SIZE;
		const PhysicalSlotId appendSlot(slotCount + 1U);
		if (!successorSlot.isValid()) {
			successorSlot = appendSlot;
		}
		const bool reuseExistingTailSlot = successorSlot != appendSlot;
		const uint8_t invalidSuccessor = static_cast<uint8_t>(successorSlot != appendSlot &&
															  (slotCount == 0U || successorSlot.value() != slotCount));
		if (invalidSuccessor != 0U) {
			throwCatalogPoolError(
					FormatError::FERR_INVALID_STATE,
					"relocatable metadata emergency reserve successor is not an append or reusable tail slot");
		}
		MetadataEmergencyReserveDescriptor next;
		next.physical_slot = successorSlot;
		next.pool_token = nextPoolToken;
		next.generation = generation;
		next.committed_lsn = committedLsn;
		if (!next.isValid() || pools_.contains(successorSlot.value())) {
			throwCatalogPoolError(FormatError::FERR_INVALID_STATE,
								  "relocatable metadata emergency reserve successor identity is invalid or occupied");
		}
		static_cast<void>(createPoolAtLocked(successorSlot, next.pool_token, generation, committedLsn,
											 MPPF_EMERGENCY_RESERVE_ORIGIN, reuseExistingTailSlot));
		emergency_reserve_ = next;
        refreshPoolAvailabilityLocked(previous.physical_slot);
        refreshPoolAvailabilityLocked(next.physical_slot);
		return next;
	}

	void MetadataPagePoolAllocator::restoreEmergencyReserve(
			const MetadataEmergencyReserveDescriptor &previous,
			const MetadataEmergencyReserveDescriptor &expectedCurrent) noexcept {
		try {
			std::lock_guard lock(mutex_);
			if (previous.isValid() && expectedCurrent.isValid() && emergency_reserve_ == expectedCurrent) {
				emergency_reserve_ = previous;
                refreshPoolAvailabilityLocked(previous.physical_slot);
                refreshPoolAvailabilityLocked(expectedCurrent.physical_slot);
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
													   std::span<const Superblock> activeSnapshots, bool force) {
		SuperblockCodec::validate(current);
		std::vector<Superblock> protectedSuperblocks;
		protectedSuperblocks.reserve(activeSnapshots.size() + 1U);
		protectedSuperblocks.push_back(current);
		for (const Superblock &snapshot: activeSnapshots) {
			SuperblockCodec::validate(snapshot);
			if (snapshot.database_id != current.database_id || snapshot.generation > current.generation ||
				snapshot.checkpoint_lsn > current.checkpoint_lsn ||
				snapshot.data_region_offset != current.data_region_offset ||
                (snapshot.generation == current.generation && snapshot != current)) {
				throwCatalogPoolError(FormatError::FERR_INVALID_STATE,
									  "relocatable metadata snapshot is not visible from the current Superblock");
			}
			protectedSuperblocks.push_back(snapshot);
		}

		std::unique_lock lock(mutex_);
		try {
			requireHealthyLocked();
            if (!force && retirement_base_ && *retirement_base_ == current) {
                reclaimRetiredMetadataLocked(current, activeSnapshots);
                return;
            }

			debug::PerfTrace::addValue("native_metadata.reclaim.pending_allocations", allocations_since_reconciliation_);
            if (debug::PerfTrace::areValuesEnabled()) {
                uint64_t free = 0U;
                for (const auto &[slot, pool] : pools_) {
                    if ((emergency_reserve_.isValid() && slot == emergency_reserve_.physical_slot.value()) || snapshot_protected_pool_slots_.contains(slot)) continue;
                    free += RSS_PAGE_POOL_DATA_FRAME_COUNT - std::popcount(pool.header.allocated_frames);
                }
                debug::PerfTrace::addValue("native_metadata.reclaim.free_frames_before_gc", free);
                debug::PerfTrace::addValue("native_metadata.reclaim.forced", force);
            }
			// This runs after the new Superblock is durable. A later publication's
			// mandatory data fence persists these reusable-frame header updates.
			reconcileReachabilityLocked(protectedSuperblocks, false, true, false);
			allocations_since_reconciliation_ = 0U;
		} catch (...) {
			poisoned_ = true;
			throw;
		}
	}

	void MetadataPagePoolAllocator::reconcileReachabilityLocked(std::span<const Superblock> protectedSuperblocks,
																bool resetPageId, bool reclaimUnreachable,
																bool persistReclaimedFrames) {
		debug::ScopedPerfTimer timer("native_metadata.reclaim.full_reconciliation");
        debug::PerfTrace::addValue("native_metadata.reclaim.full_traversals", 1);
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
        for (auto &[slot, pool] : pools_) { static_cast<void>(slot); pool.current_reachable_frames = 0U; }
        bool markingCurrent = false;
		std::set<std::pair<uint64_t, uint8_t>> reachable;
		uint64_t maximumPageId = 0;
		MetadataPageFile pages(io_, protectedSuperblocks.front());
		std::set<uint64_t> *activeLogicalPageIds = nullptr;

		auto markLogicalPageId = [&](MetadataPageId pageId) {
			if (activeLogicalPageIds == nullptr || !pageId.isValid() ||
				!activeLogicalPageIds->emplace(pageId.value()).second) {
				throwCatalogPoolError(FormatError::FERR_INVALID_STATE,
									  "relocatable metadata tree contains an invalid or duplicate logical Page ID");
			}
		};

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
            if (markingCurrent) pool->second.current_reachable_frames |= bit;
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
			markLogicalPageId(page.page_id);
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
					markLogicalPageId(leaf.page_id);
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
					markLogicalPageId(leaf.page_id);
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
					markLogicalPageId(leaf.page_id);
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

		std::function<void(PhysicalPageRef, bool, uint16_t, uint64_t, const Superblock &,
						   std::set<std::pair<uint64_t, uint8_t>> &, std::set<std::pair<uint64_t, uint8_t>> &)>
				visitRouteBranch;
		visitRouteBranch = [&](PhysicalPageRef reference, bool isRoot, uint16_t expectedLevel, uint64_t ordinalBase,
							   const Superblock &superblock,
							   std::set<std::pair<uint64_t, uint8_t>> &bootstrapReferences,
							   std::set<std::pair<uint64_t, uint8_t>> &targetReferences) {
			markReference(reference, bootstrapReferences);
			const MetadataRadixPage page = pages.readMetadataRadix(reference);
			if ((isRoot && page.kind != CatalogRadixPageKind::CRPK_ROOT) ||
				(!isRoot && (page.kind != CatalogRadixPageKind::CRPK_INTERNAL || page.level != expectedLevel)) ||
				page.directory_type != MetadataDirectoryType::MDT_METADATA_PAGE_ROUTE ||
				page.generation > superblock.generation || page.committed_lsn > superblock.checkpoint_lsn) {
				throwCatalogPoolError(
						FormatError::FERR_INVALID_HEADER,
						"metadata PageId route branch has an invalid radix page during pool reconciliation");
			}
			const uint64_t factor = radixFactor(page.level);
			for (uint32_t index = 0U; index < RSS_CATALOG_RADIX_FANOUT; ++index) {
				const PhysicalPageRef child = page.children[index];
				if (!child.isValid()) {
					continue;
				}
				if (index != 0U && factor > (std::numeric_limits<uint64_t>::max() - ordinalBase) / index) {
					throwCatalogPoolError(FormatError::FERR_ARITHMETIC_OVERFLOW,
										  "metadata PageId route branch ordinal overflows during pool reconciliation");
				}
				const uint64_t ordinal = ordinalBase + static_cast<uint64_t>(index) * factor;
				if (page.level != 0U) {
					visitRouteBranch(child, false, static_cast<uint16_t>(page.level - 1U), ordinal, superblock,
									 bootstrapReferences, targetReferences);
					continue;
				}
				markReference(child, bootstrapReferences);
				const MetadataPageRouteLeafPage leaf = pages.readMetadataPageRouteLeaf(child);
				if (leaf.generation > superblock.generation || leaf.committed_lsn > superblock.checkpoint_lsn) {
					throwCatalogPoolError(FormatError::FERR_INVALID_HEADER,
										  "metadata PageId route branch has a future leaf during pool reconciliation");
				}
				for (const MetadataPageRoute &route: leaf.routes) {
					if (!route.page_id.isValid() || !route.physical_reference.isValid() ||
						route.physical_reference.generation > superblock.generation ||
						(route.page_id.value() - 1U) / RSS_METADATA_PAGE_ROUTE_LEAF_MAX_ENTRIES != ordinal) {
						throwCatalogPoolError(
								FormatError::FERR_INVALID_HEADER,
								"metadata PageId route branch has a misrouted target during pool reconciliation");
					}
					markReference(route.physical_reference, targetReferences);
				}
			}
		};

		for (const Superblock &superblock: protectedSuperblocks) {
            markingCurrent = &superblock == &protectedSuperblocks.front();
			pages.rebindSuperblock(superblock);
            if (superblock.hybrid_property_descriptor_root.isValid()) {
                std::set<std::pair<uint64_t, uint8_t>> descriptorReferences;
                markReference(superblock.hybrid_property_descriptor_root, descriptorReferences);
                static_cast<void>(pages.readHybridPropertyDescriptor(superblock.hybrid_property_descriptor_root, superblock));
            }
			std::set<uint64_t> logicalPageIds;
			activeLogicalPageIds = &logicalPageIds;
			const auto reconcileRoot = [&](MetadataDirectoryType directoryType, PhysicalPageRef physicalRoot) {
				const PhysicalPageRef root = metadataRootReference(pages, superblock, directoryType, physicalRoot);
				if (!root.isValid()) {
					return;
				}
				std::set<std::pair<uint64_t, uint8_t>> treeReferences;
				visitRadix(root, true, 0, 0, directoryType, superblock, treeReferences);
			};
			reconcileRoot(MetadataDirectoryType::MDT_CATALOG, superblock.catalog_root);
			reconcileRoot(MetadataDirectoryType::MDT_ALLOCATOR, superblock.allocator_root);
			if (superblock.page_directory_root.isValid() && !superblock.usesPageDirectoryBootstrap()) {
				std::set<std::pair<uint64_t, uint8_t>> treeReferences;
				visitRadix(superblock.page_directory_root, true, 0, 0, MetadataDirectoryType::MDT_PAGE, superblock,
						   treeReferences);
			}
			if (superblock.usesPageDirectoryBootstrap()) {
				if (!superblock.page_directory_root.isValid()) {
					throwCatalogPoolError(FormatError::FERR_INVALID_HEADER,
										  "Page Directory bootstrap flag has no physical header reference");
				}
				std::set<std::pair<uint64_t, uint8_t>> bootstrapReferences;
				std::set<std::pair<uint64_t, uint8_t>> targetReferences;
				markReference(superblock.page_directory_root, bootstrapReferences);
				const PageDirectoryBootstrap bootstrap =
						pages.readPageDirectoryBootstrap(superblock.page_directory_root);
				if (bootstrap.generation > superblock.generation ||
					bootstrap.committed_lsn > superblock.checkpoint_lsn ||
					!bootstrap.metadata_page_directory_root.isValid()) {
					throwCatalogPoolError(FormatError::FERR_INVALID_HEADER,
										  "Page Directory bootstrap has invalid visibility or route branch");
				}
				if (bootstrap.data_page_directory_root.isValid()) {
					std::set<std::pair<uint64_t, uint8_t>> dataReferences;
					visitRadix(bootstrap.data_page_directory_root, true, 0U, 0U, MetadataDirectoryType::MDT_PAGE,
							   superblock, dataReferences);
				}
				visitRouteBranch(bootstrap.metadata_page_directory_root, true, 0U, 0U, superblock, bootstrapReferences,
								 targetReferences);
			}
			reconcileRoot(MetadataDirectoryType::MDT_EXTENT, superblock.extent_directory_root);
			reconcileRoot(MetadataDirectoryType::MDT_NODE_ENTITY, superblock.node_entity_directory_root);
			reconcileRoot(MetadataDirectoryType::MDT_EDGE_ENTITY, superblock.edge_entity_directory_root);
			reconcileRoot(MetadataDirectoryType::MDT_ENTITY_PROPERTY, superblock.entity_property_directory_root);
			reconcileRoot(MetadataDirectoryType::MDT_INDEX_ROOT, superblock.index_root_directory_root);
			reconcileRoot(MetadataDirectoryType::MDT_VECTOR_LOCATION, superblock.vector_location_directory_root);
			reconcileRoot(MetadataDirectoryType::MDT_VECTOR_MANIFEST, superblock.vector_manifest_directory_root);
			reconcileRoot(MetadataDirectoryType::MDT_LARGE_VALUE_MANIFEST,
						  superblock.large_value_manifest_directory_root);
			reconcileRoot(MetadataDirectoryType::MDT_STATE, superblock.state_directory_root);
			reconcileRoot(MetadataDirectoryType::MDT_RETIRE_QUEUE, superblock.retire_queue_root);
		}

		bool changed = false;
		if (reclaimUnreachable) {
            resetMetadataRetirementLocked();
			if (protectedSuperblocks.size() == 1U) {
				snapshot_protected_pool_slots_.clear();
			}
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
					pool.header = std::move(updated.header);
					changed = true;
				}
                uint32_t retiredOnly = liveFrames & ~pool.current_reachable_frames;
                while (retiredOnly != 0U) {
                    const auto index = static_cast<uint8_t>(std::countr_zero(retiredOnly));
                    queueMetadataRetirementLocked(PhysicalSlotId(slot),
                        static_cast<uint8_t>(index + RSS_PAGE_POOL_FIRST_DATA_FRAME), protectedSuperblocks.front().generation);
                    retiredOnly &= retiredOnly - 1U;
                }
			}
            rebuildPoolAvailabilityLocked();
            retirement_base_ = protectedSuperblocks.front();
		}
		if (resetPageId) {
			if (maximumPageId == std::numeric_limits<uint64_t>::max()) {
				page_id_exhausted_ = true;
			} else {
				next_page_id_ = MetadataPageId(maximumPageId + 1U);
				page_id_exhausted_ = false;
			}
		}
		if (changed && persistReclaimedFrames) {
			io_->sync();
		}
		if (changed) {
			allocation_cursor_slot_.reset();
		}
	}

	void MetadataPagePoolAllocator::protectPoolUntilSnapshotsDrain(PhysicalSlotId slot) {
		std::lock_guard lock(mutex_);
		requireHealthyLocked();
		if (!slot.isValid() || !pools_.contains(slot.value())) {
			throwCatalogPoolError(FormatError::FERR_INVALID_STATE,
								  "relocatable metadata snapshot protection targets an unknown pool");
		}
		snapshot_protected_pool_slots_.emplace(slot.value());
        refreshPoolAvailabilityLocked(slot);
		allocation_cursor_slot_.reset();
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

	uint64_t MetadataPagePoolAllocator::allocatableFreePages() const {
		std::lock_guard lock(mutex_);
        return available_metadata_frames_;
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
			unlinkUnconfirmedOwnershipLocked(slot);
            unlinkPoolAvailabilityLocked(slot);
			pools_.erase(slot.value());
		}
		allocation_cursor_slot_.reset();
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

    utils::OwnedStdVector<PhysicalSlotId> MetadataPagePoolAllocator::unconfirmedOwnershipSlots() const {
        std::lock_guard lock(mutex_);
        utils::OwnedStdVector<PhysicalSlotId> result;
        result.reserve(unconfirmed_ownership_count_);
        uint64_t slot = unconfirmed_ownership_head_;
        while (slot != 0U) {
            if (result.size() >= unconfirmed_ownership_count_)
                throwCatalogPoolError(FormatError::FERR_INVALID_STATE, "metadata ownership work list contains a cycle");
            const auto &pool = pools_.at(slot);
            if (!pool.ownership_pending)
                throwCatalogPoolError(FormatError::FERR_INVALID_STATE, "metadata ownership work list has an invalid member");
            result.emplace_back(slot); slot = pool.ownership_next;
        }
        if (result.size() != unconfirmed_ownership_count_)
            throwCatalogPoolError(FormatError::FERR_INVALID_STATE, "metadata ownership work list count disagrees");
        return result;
    }

    void MetadataPagePoolAllocator::linkUnconfirmedOwnershipLocked(PhysicalSlotId slot) {
        auto &pool = pools_.at(slot.value());
        if (pool.ownership_pending) return;
        pool.ownership_previous = 0U; pool.ownership_next = unconfirmed_ownership_head_;
        if (unconfirmed_ownership_head_ != 0U)
            pools_.at(unconfirmed_ownership_head_).ownership_previous = slot.value();
        unconfirmed_ownership_head_ = slot.value(); pool.ownership_pending = true;
        ++unconfirmed_ownership_count_;
    }

    void MetadataPagePoolAllocator::unlinkUnconfirmedOwnershipLocked(PhysicalSlotId slot) {
        auto &pool = pools_.at(slot.value());
        if (!pool.ownership_pending) return;
        if (pool.ownership_previous != 0U)
            pools_.at(pool.ownership_previous).ownership_next = pool.ownership_next;
        else unconfirmed_ownership_head_ = pool.ownership_next;
        if (pool.ownership_next != 0U)
            pools_.at(pool.ownership_next).ownership_previous = pool.ownership_previous;
        pool.ownership_previous = 0U; pool.ownership_next = 0U; pool.ownership_pending = false;
        --unconfirmed_ownership_count_;
    }

#ifdef ZYX_ENABLE_STORAGE_TEST_HOOKS
	void MetadataPagePoolAllocator::eraseRegisteredPoolForTesting(PhysicalSlotId slot) {
		std::lock_guard lock(mutex_);
		unlinkUnconfirmedOwnershipLocked(slot);
        unlinkPoolAvailabilityLocked(slot);
        retirement_base_.reset();
		pools_.erase(slot.value());
		allocation_cursor_slot_.reset();
	}

	void MetadataPagePoolAllocator::replaceRegisteredPoolHeaderForTesting(PhysicalSlotId slot,
																		  MetadataPagePoolHeader header) {
		std::lock_guard lock(mutex_);
		pools_.at(slot.value()).header = header;
        refreshPoolAvailabilityLocked(slot);
        retirement_base_.reset();
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
		const MetadataPagePoolHeader header = selectControlHeader(page).header;
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
        unconfirmed_ownership_head_ = 0U;
        unconfirmed_ownership_count_ = 0U;
        retired_metadata_head_ = 0U; retired_metadata_tail_ = 0U; retired_metadata_count_ = 0U; retirement_base_.reset();
		allocation_cursor_slot_.reset();
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
			const MetadataPagePoolHeader header = selectControlHeader(controlPage).header;
			if (header.physical_slot != physicalSlot || header.segment_id != envelope.segment_id) {
				throwCatalogPoolError(FormatError::FERR_INVALID_HEADER,
									  "relocatable metadata pool header has a mismatched physical slot");
			}
			pools_.emplace(slot, PoolState{header});
		}
	}

	void MetadataPagePoolAllocator::discoverReachablePoolsLocked(const Superblock &superblock) {
		pools_.clear();
        unconfirmed_ownership_head_ = 0U;
        unconfirmed_ownership_count_ = 0U;
        retired_metadata_head_ = 0U; retired_metadata_tail_ = 0U; retired_metadata_count_ = 0U; retirement_base_.reset();
		allocation_cursor_slot_.reset();
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

		const auto visitRouteBranch = [&](auto &&self, PhysicalPageRef reference, bool root, uint16_t expectedLevel,
										  uint64_t ordinalBase) -> void {
			if (!reference.isValid() || reference.generation > superblock.generation) {
				throwCatalogPoolError(FormatError::FERR_INVALID_HEADER,
									  "metadata PageId route branch has an invalid physical reference");
			}
			if (!visitedPages.emplace(reference.physical_slot.value(), reference.frame_index).second) {
				throwCatalogPoolError(FormatError::FERR_INVALID_STATE,
									  "metadata PageId route branch aliases or cycles through bootstrap pages");
			}
			reachableSlots.insert(reference.physical_slot.value());
			const MetadataRadixPage page = pages.readMetadataRadix(reference);
			if (page.kind != (root ? CatalogRadixPageKind::CRPK_ROOT : CatalogRadixPageKind::CRPK_INTERNAL) ||
				page.directory_type != MetadataDirectoryType::MDT_METADATA_PAGE_ROUTE ||
				(!root && page.level != expectedLevel) || page.generation > superblock.generation ||
				page.committed_lsn > superblock.checkpoint_lsn) {
				throwCatalogPoolError(FormatError::FERR_INVALID_HEADER,
									  "metadata PageId route branch has an invalid radix page");
			}
			const uint64_t factor = radixFactor(page.level);
			for (uint32_t index = 0U; index < RSS_CATALOG_RADIX_FANOUT; ++index) {
				const PhysicalPageRef child = page.children[index];
				if (!child.isValid()) {
					continue;
				}
				if (child.generation > superblock.generation ||
					(index != 0U && factor > (std::numeric_limits<uint64_t>::max() - ordinalBase) / index)) {
					throwCatalogPoolError(FormatError::FERR_INVALID_HEADER,
										  "metadata PageId route branch has an invalid child reference");
				}
				const uint64_t ordinal = ordinalBase + static_cast<uint64_t>(index) * factor;
				if (page.level != 0U) {
					self(self, child, false, static_cast<uint16_t>(page.level - 1U), ordinal);
					continue;
				}
				if (!visitedPages.emplace(child.physical_slot.value(), child.frame_index).second) {
					throwCatalogPoolError(FormatError::FERR_INVALID_STATE,
										  "metadata PageId route branch duplicates a bootstrap leaf");
				}
				reachableSlots.insert(child.physical_slot.value());
				const MetadataPageRouteLeafPage leaf = pages.readMetadataPageRouteLeaf(child);
				if (leaf.generation > superblock.generation || leaf.committed_lsn > superblock.checkpoint_lsn) {
					throwCatalogPoolError(FormatError::FERR_INVALID_HEADER,
										  "metadata PageId route branch has a future leaf");
				}
				for (const MetadataPageRoute &route: leaf.routes) {
					if (!route.page_id.isValid() || !route.physical_reference.isValid() ||
						route.physical_reference.generation > superblock.generation ||
						(route.page_id.value() - 1U) / RSS_METADATA_PAGE_ROUTE_LEAF_MAX_ENTRIES != ordinal) {
						throwCatalogPoolError(FormatError::FERR_INVALID_HEADER,
											  "metadata PageId route branch has a misrouted target");
					}
					reachableSlots.insert(route.physical_reference.physical_slot.value());
				}
			}
		};

        if (superblock.hybrid_property_descriptor_root.isValid()) {
            const auto reference = superblock.hybrid_property_descriptor_root;
            if (!visitedPages.emplace(reference.physical_slot.value(), reference.frame_index).second)
                throwCatalogPoolError(FormatError::FERR_INVALID_STATE, "hybrid descriptor aliases another metadata frame");
            static_cast<void>(pages.readHybridPropertyDescriptor(reference, superblock));
            reachableSlots.insert(reference.physical_slot.value());
        }

		struct RootDescriptor {
			PhysicalPageRef reference;
			MetadataDirectoryType directory_type;
		};
		const std::array roots{
				RootDescriptor{superblock.catalog_root, MetadataDirectoryType::MDT_CATALOG},
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
		if (!superblock.usesPageDirectoryBootstrap() && superblock.page_directory_root.isValid()) {
			visitDirectory(visitDirectory, superblock.page_directory_root, MetadataDirectoryType::MDT_PAGE, true);
		}
		if (!superblock.usesPageDirectoryBootstrap()) {
			for (const RootDescriptor root: roots) {
				if (root.reference.isValid()) {
					visitDirectory(visitDirectory, root.reference, root.directory_type, true);
				}
			}
		}
		if (superblock.usesPageDirectoryBootstrap()) {
			if (!superblock.page_directory_root.isValid() ||
				!visitedPages
						 .emplace(superblock.page_directory_root.physical_slot.value(),
								  superblock.page_directory_root.frame_index)
						 .second) {
				throwCatalogPoolError(FormatError::FERR_INVALID_STATE,
									  "Page Directory bootstrap has an invalid or aliased header frame");
			}
			reachableSlots.insert(superblock.page_directory_root.physical_slot.value());
			const PageDirectoryBootstrap bootstrap = pages.readPageDirectoryBootstrap(superblock.page_directory_root);
			if (bootstrap.generation > superblock.generation || bootstrap.committed_lsn > superblock.checkpoint_lsn ||
				!bootstrap.metadata_page_directory_root.isValid()) {
				throwCatalogPoolError(FormatError::FERR_INVALID_HEADER,
									  "Page Directory bootstrap has invalid visibility or route branch");
			}
			if (bootstrap.data_page_directory_root.isValid()) {
				visitDirectory(visitDirectory, bootstrap.data_page_directory_root, MetadataDirectoryType::MDT_PAGE,
							   true);
			}
			visitRouteBranch(visitRouteBranch, bootstrap.metadata_page_directory_root, true, 0U, 0U);
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
			const MetadataPagePoolHeader header = selectControlHeader(controlPage).header;
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
			if (!inserted && (reserve.header.allocated_frames == 0U ||
							  (reserve.header.flags & MPPF_EMERGENCY_RESERVE_ORIGIN) == 0U)) {
				throwCatalogPoolError(FormatError::FERR_INVALID_STATE,
									  "relocatable metadata emergency reserve aliases a root-reachable pool");
			}
			// A reserve may contain published COW metadata after ENOSPC. Its immutable
			// origin bit distinguishes that state from a forged ordinary pool alias.
			static_cast<void>(iterator);
		}
	}

	MetadataPagePoolAllocator::PoolState &
	MetadataPagePoolAllocator::createPoolLocked(Generation generation, LogSequenceNumber checkpointLsn,
													std::span<const PhysicalSlotId> excludedPoolSlots,
													std::span<const PhysicalSlotRange> excludedDataSlotRanges,
													const MetadataSlotReservationLookup *reservationLookup,
													bool searchReusableSlot) {
		requireHealthyLocked();
		const uint64_t fileSize = io_->fileSize();
		const uint8_t invalidTail = static_cast<uint8_t>(fileSize < data_region_offset_) |
									static_cast<uint8_t>((fileSize - data_region_offset_) % RSS_SEGMENT_SIZE != 0U);
		if (invalidTail != 0U ||
			fileSize - data_region_offset_ > (std::numeric_limits<uint64_t>::max() - 1U) / RSS_SEGMENT_SIZE) {
			throwCatalogPoolError(FormatError::FERR_ARITHMETIC_OVERFLOW,
								  "relocatable metadata pool file cannot append another physical slot");
		}
		if (searchReusableSlot) {
			if (const std::optional<PhysicalSlotId> reusableSlot = findReusableFreeSlotLocked(
					generation, excludedPoolSlots, excludedDataSlotRanges, reservationLookup);
					reusableSlot.has_value()) {
				return createPoolAtLocked(*reusableSlot, SegmentId(reusableSlot->value()), generation, checkpointLsn, 0U,
										  true);
			}
		}
		uint64_t nextSlot = (fileSize - data_region_offset_) / RSS_SEGMENT_SIZE + 1U;
		const auto excludedPool = [&excludedPoolSlots, reservationLookup](uint64_t slot) {
			return reservationLookup != nullptr
						   ? reservationLookup->isDataSlotReserved(PhysicalSlotId(slot))
						   : std::ranges::any_of(excludedPoolSlots, [slot](PhysicalSlotId candidate) {
								 return candidate.value() == slot;
							 });
		};
		const auto excludedRangeAt = [&excludedDataSlotRanges,
									  reservationLookup](uint64_t slot) -> std::optional<PhysicalSlotRange> {
			if (reservationLookup != nullptr) {
				return reservationLookup->dataSlotRangeContaining(PhysicalSlotId(slot));
			}
			const auto range = std::ranges::find_if(excludedDataSlotRanges, [slot](PhysicalSlotRange candidate) {
				return candidate.contains(PhysicalSlotId(slot));
			});
			return range == excludedDataSlotRanges.end() ? std::nullopt : std::optional<PhysicalSlotRange>(*range);
		};
		while (excludedPool(nextSlot) || excludedRangeAt(nextSlot).has_value()) {
			if (nextSlot == std::numeric_limits<uint64_t>::max()) {
				throwCatalogPoolError(FormatError::FERR_ARITHMETIC_OVERFLOW,
									  "relocatable metadata pool cannot skip a reserved maximum physical slot");
			}
			// Data and metadata append into one physical file.  Claim a skipped data
			// slot as part of this unpublished batch so the next metadata pool still
			// starts at the physical file tail.  MetadataWriteBatch reclaims this
			// complete provisional suffix if its Superblock switch does not publish.
			const std::optional<PhysicalSlotRange> range = excludedRangeAt(nextSlot);
			const uint64_t skippedSlots = range.has_value() ? range->last.value() - nextSlot + 1U : 1U;
			if (skippedSlots > std::numeric_limits<uint64_t>::max() / RSS_SEGMENT_SIZE ||
				skippedSlots * RSS_SEGMENT_SIZE > std::numeric_limits<size_t>::max()) {
				throwCatalogPoolError(FormatError::FERR_ARITHMETIC_OVERFLOW,
									  "relocatable metadata pool reserved data range exceeds addressable space");
			}
			const uint64_t reservedOffset = slotOffset(PhysicalSlotId(nextSlot));
			const size_t reservedBytes = static_cast<size_t>(skippedSlots * RSS_SEGMENT_SIZE);
			if (io_->reserveAppendSpace(reservedBytes) != reservedOffset) {
				throwCatalogPoolError(FormatError::FERR_INVALID_STATE,
									  "relocatable metadata pool reserved an unexpected physical tail offset");
			}
			nextSlot += skippedSlots;
		}
		if (nextSlot == 0U || pools_.contains(nextSlot)) {
			throwCatalogPoolError(FormatError::FERR_ARITHMETIC_OVERFLOW,
								  "relocatable metadata pool slot space is exhausted");
		}
		return createPoolAtLocked(PhysicalSlotId(nextSlot), SegmentId(nextSlot), generation, checkpointLsn);
	}

	std::optional<PhysicalSlotId>
	MetadataPagePoolAllocator::findReusableFreeSlotLocked(Generation generation,
														  std::span<const PhysicalSlotId> excludedPoolSlots,
														  std::span<const PhysicalSlotRange> excludedDataSlotRanges,
														  const MetadataSlotReservationLookup *reservationLookup) {
		const auto contexts = active_batches_.find(generation.value());
		if (contexts == active_batches_.end()) {
			return std::nullopt;
		}
		// A test or stale caller can keep a predicted candidate alive after another
		// batch has changed the durable root. Only the recovered A/B generation may
		// authorize reuse of a slot already present in the physical file.
		const Superblock durable = SuperblockFile(io_).recover();
		const auto context = std::ranges::find(contexts->second, durable, &ActiveBatchContext::superblock);
		if (context == contexts->second.end()) {
			return std::nullopt;
		}
		const uint64_t fileSize = io_->fileSize();
		if (fileSize < data_region_offset_ || (fileSize - data_region_offset_) % RSS_SEGMENT_SIZE != 0U) {
			return std::nullopt;
		}
		const uint64_t tailSlotValue = (fileSize - data_region_offset_) / RSS_SEGMENT_SIZE;
		if (tailSlotValue == 0U) {
			return std::nullopt;
		}
		MetadataPageFile pages(io_, context->superblock);
		AllocatorDirectory allocator(pages, *this);
		PhysicalSlotId firstSlot(1U);
		const PhysicalSlotId lastSlot(tailSlotValue);
		while (firstSlot <= lastSlot) {
			const std::optional<PhysicalSlotId> freeSlot =
					allocator.findFirstFree(context->superblock, firstSlot, lastSlot);
			if (!freeSlot.has_value()) {
				return std::nullopt;
			}
			const bool excluded =
					reservationLookup != nullptr
							? reservationLookup->isDataSlotReserved(*freeSlot)
							: std::ranges::any_of(excludedPoolSlots, [freeSlot](PhysicalSlotId slot) {
								  return slot == *freeSlot;
							  }) || std::ranges::any_of(excludedDataSlotRanges, [freeSlot](PhysicalSlotRange range) {
								  return range.contains(*freeSlot);
							  });
			if (!excluded && !pools_.contains(freeSlot->value())) {
				return freeSlot;
			}
			if (freeSlot->value() == lastSlot.value()) {
				return std::nullopt;
			}
			firstSlot = PhysicalSlotId(freeSlot->value() + 1U);
		}
		return std::nullopt;
	}

	MetadataPagePoolAllocator::PoolState &
	MetadataPagePoolAllocator::createPoolAtLocked(PhysicalSlotId physicalSlot, SegmentId segmentId,
												  Generation generation, LogSequenceNumber checkpointLsn,
												  uint32_t flags, bool reuseExistingSlot) {
		requireHealthyLocked();
		// createPoolLocked and both reserve callers validate the identity and reserve an
		// unoccupied slot before entering this private final-write primitive.
		if (!physicalSlot.isValid() || !segmentId.isValid() || !generation.isValid() || !checkpointLsn.isValid() ||
			pools_.contains(physicalSlot.value())) {
			throwCatalogPoolError(FormatError::FERR_INVALID_STATE,
								  "relocatable metadata pool creation has an invalid or occupied physical slot");
		}
		const uint64_t offset = slotOffset(physicalSlot);
		const uint64_t fileSize = io_->fileSize();
		const bool appendAtTail = fileSize == offset;
		const bool overwriteReusableSlot = reuseExistingSlot &&
										   offset <= std::numeric_limits<uint64_t>::max() - RSS_SEGMENT_SIZE &&
										   fileSize >= offset + RSS_SEGMENT_SIZE;
		if (!appendAtTail && !overwriteReusableSlot) {
			throwCatalogPoolError(
					FormatError::FERR_INVALID_STATE,
					"relocatable metadata pool creation requires an append or durable free physical slot");
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
		header.flags = flags;
		if (appendAtTail && io_->reserveAppendSpace(RSS_SEGMENT_SIZE) != offset) {
			throwCatalogPoolError(FormatError::FERR_INVALID_STATE,
								  "relocatable metadata pool reserved an unexpected physical tail offset");
		}
		writeTypedControlPage(*io_, offset, generation, makeControlPage(envelope, header));
		// makeControlPage publishes record 1 as the active A record. Keep the
		// process-local header synchronized so batch updates can select the
		// inactive B record without rereading the control page.
		header.control_sequence = 1U;
		auto [iterator, inserted] = pools_.emplace(physicalSlot.value(), PoolState{header});
		static_cast<void>(inserted);
        linkUnconfirmedOwnershipLocked(physicalSlot);
        refreshPoolAvailabilityLocked(physicalSlot);
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
		unlinkUnconfirmedOwnershipLocked(PhysicalSlotId(pool->first));
        unlinkPoolAvailabilityLocked(PhysicalSlotId(pool->first));
		pools_.erase(pool);
		allocation_cursor_slot_.reset();
	}

	void MetadataPagePoolAllocator::abandonUnpublishedBatchLocked(std::span<const PhysicalSlotId> poolSlots,
																  std::span<const PhysicalSlotId> dataSlots,
																  std::span<const PhysicalSlotRange> dataSlotRanges,
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
		for (const PhysicalSlotRange range: dataSlotRanges) {
			if (!range.isValid() || range.first.value() <= initialSlotCount) {
				throwCatalogPoolError(FormatError::FERR_INVALID_STATE,
									  "relocatable metadata batch has an invalid provisional data-slot range");
			}
		}

		// Metadata writers are serialized.  Therefore every physical byte beyond
		// the size captured at batch construction belongs to this unpublished
		// batch, including sparse gaps introduced when a caller materializes a
		// later data slot.  Treat the complete suffix as provisional so cleanup
		// never leaves a stranded tail gap after a failed root switch.
		const uint64_t retainedSlotCount = initialSlotCount;
		const bool canTruncateTail = retainedSlotCount < currentSlotCount && io_->hasPwriteSupport();
		if (!dataSlotRanges.empty() && currentSlotCount > retainedSlotCount && !canTruncateTail) {
			throwCatalogPoolError(FormatError::FERR_INVALID_STATE,
								  "relocatable metadata batch cannot safely abandon a ranged provisional tail");
		}

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
			unlinkUnconfirmedOwnershipLocked(PhysicalSlotId(pool->first));
        unlinkPoolAvailabilityLocked(PhysicalSlotId(pool->first));
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
		allocation_cursor_slot_.reset();
	}

	void MetadataPagePoolAllocator::writePoolLocked(PoolState &pool) {
		pool.header.control_sequence = rewriteInactiveControlRecord(
				*io_, slotOffset(pool.header.physical_slot), pool.header.generation, pool.header,
				"relocatable metadata-pool control page is truncated");
	}

	void MetadataPagePoolAllocator::writePoolLocked(const PoolState &pool) {
		static_cast<void>(rewriteInactiveControlRecord(*io_, slotOffset(pool.header.physical_slot), pool.header.generation,
															pool.header, "relocatable metadata-pool control page is truncated"));
	}

	void MetadataPagePoolAllocator::writePoolLockedKnownHeader(PoolState &pool,
											 const MetadataPagePoolHeader &current) {
		pool.header.control_sequence = rewriteInactiveControlRecordKnown(
				*io_, slotOffset(pool.header.physical_slot), pool.header.generation, current, pool.header);
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
