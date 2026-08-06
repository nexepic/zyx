/**
 * @file PhysicalSegmentAllocator.cpp
 * @brief Durable fixed-slot allocation and checked reads for relocatable segments.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

#include "graph/utils/ChecksumUtils.hpp"

namespace graph::storage {

	namespace {

		[[noreturn]] void throwPhysicalAllocatorError(FormatError error, const char *message) {
			throw FormatException(error, message);
		}

		[[nodiscard]] bool isAllocatableKind(SegmentKind kind) {
			const uint16_t raw = static_cast<uint16_t>(kind);
			return raw >= static_cast<uint16_t>(SegmentKind::SGK_ENTITY_PAGE_POOL) &&
				   raw <= static_cast<uint16_t>(SegmentKind::SGK_LARGE_VALUE_MANIFEST_POOL) &&
				   kind != SegmentKind::SGK_METADATA_PAGE_POOL;
		}

		[[nodiscard]] uint32_t payloadCrc(std::span<const std::byte> payload) {
			const std::byte empty{};
			return utils::calculateCrc(payload.empty() ? &empty : payload.data(), payload.size());
		}

		void readCompleteSegment(const StorageIO &io, uint64_t segmentOffset, Generation generation,
								 std::span<std::byte> bytes, const char *message) {
			for (size_t pageOffset = 0U; pageOffset < bytes.size(); pageOffset += RSS_PAGE_SIZE) {
				const std::span<std::byte> page = bytes.subspan(pageOffset, RSS_PAGE_SIZE);
				if (io.readAt({segmentOffset + pageOffset, generation.value()}, page.data(), page.size(),
							  BufferPoolReadHint::BPRH_SEQUENTIAL) != page.size()) {
					throwPhysicalAllocatorError(FormatError::FERR_BUFFER_TOO_SMALL, message);
				}
			}
		}

		void writeCompleteSegment(StorageIO &io, uint64_t segmentOffset, Generation generation,
								  std::span<const std::byte> bytes) {
			for (size_t pageOffset = 0U; pageOffset < bytes.size(); pageOffset += RSS_PAGE_SIZE) {
				const std::span<const std::byte> page = bytes.subspan(pageOffset, RSS_PAGE_SIZE);
				io.writeAt({segmentOffset + pageOffset, generation.value()}, page.data(), page.size());
			}
		}

		[[nodiscard]] std::vector<std::byte> buildPreparedSegment(const CatalogEntry &entry,
																  const PhysicalSegmentAllocationRequest &request) {
			std::vector<std::byte> bytes(RSS_SEGMENT_SIZE);
			SegmentEnvelope envelope;
			envelope.header_bytes = request.header_bytes;
			envelope.kind = request.kind;
			envelope.state = SegmentState::SGS_SEALED;
			envelope.segment_id = entry.segment_id;
			envelope.owner_id = entry.owner_id;
			envelope.generation = entry.generation;
			envelope.committed_lsn = entry.committed_lsn;
			envelope.payload_bytes = static_cast<uint32_t>(request.payload.size());
			envelope.payload_crc = payloadCrc(request.payload);

			const auto encodedEnvelope = SegmentEnvelopeCodec::encode(envelope);
			std::copy(encodedEnvelope.begin(), encodedEnvelope.end(), bytes.begin());
			std::copy(request.header_extension.begin(), request.header_extension.end(),
					  bytes.begin() + RSS_ENVELOPE_BYTES);
			std::copy(request.payload.begin(), request.payload.end(), bytes.begin() + request.header_bytes);
			return bytes;
		}

	} // namespace

	PhysicalSegmentAllocator::PhysicalSegmentAllocator(std::shared_ptr<StorageIO> io,
													   RelocatableMetadataStore &metadata) :
		io_(std::move(io)), metadata_(metadata) {
		if (!io_ || !metadata_.usesDataStorage(io_)) {
			throw std::invalid_argument("relocatable physical segment allocator requires the metadata data StorageIO");
		}
		const MetadataSnapshotPin snapshot = metadata_.openSnapshot();
		data_region_offset_ = snapshot.superblock().data_region_offset;
	}

	std::optional<PhysicalSegmentAllocations>
	PhysicalSegmentAllocator::allocate(std::span<const PhysicalSegmentAllocationRequest> requests,
									   PhysicalSlotId firstSlot, PhysicalSlotId lastSlot,
									   LogSequenceNumber checkpointLsn, const MetadataPublishOptions &options) {
		validateRequests(requests);
		std::vector<CatalogSlotReservationRequest> catalogRequests;
		catalogRequests.reserve(requests.size());
		for (const PhysicalSegmentAllocationRequest &request: requests) {
			CatalogSlotReservationRequest catalogRequest;
			catalogRequest.segment_id = request.segment_id;
			catalogRequest.kind = request.kind;
			catalogRequest.owner_id = request.owner_id;
			catalogRequest.segment_generation = request.generation;
			catalogRequest.state = SegmentState::SGS_SEALED;
			catalogRequest.require_absent = true;
			catalogRequests.push_back(catalogRequest);
		}

		const CatalogSlotReservationPreparer preparer = [this, requests](std::span<const CatalogEntry> entries) {
			writePreparedSegments(entries, requests);
		};
		const std::optional<CatalogSlotReservations> reservations = metadata_.reserveFirstFreeAndUpsertCatalog(
				std::span<const CatalogSlotReservationRequest>(catalogRequests), firstSlot, lastSlot, checkpointLsn,
				preparer, options);
		if (!reservations.has_value()) {
			return std::nullopt;
		}
		return PhysicalSegmentAllocations{reservations->superblock, reservations->entries};
	}

	std::optional<PhysicalSegmentAllocation>
	PhysicalSegmentAllocator::allocate(PhysicalSegmentAllocationRequest request, PhysicalSlotId firstSlot,
									   PhysicalSlotId lastSlot, LogSequenceNumber checkpointLsn,
									   const MetadataPublishOptions &options) {
		const std::optional<PhysicalSegmentAllocations> allocations =
				allocate(std::span(&request, 1), firstSlot, lastSlot, checkpointLsn, options);
		if (!allocations.has_value()) {
			return std::nullopt;
		}
		return PhysicalSegmentAllocation{allocations->superblock, allocations->entries.front()};
	}

	std::optional<PhysicalSegmentAllocations>
	PhysicalSegmentAllocator::allocateExtentSegments(std::span<const PhysicalSegmentAllocationRequest> requests,
													 std::span<const ExtentLocation> extents, PhysicalSlotId firstSlot,
													 PhysicalSlotId lastSlot, LogSequenceNumber checkpointLsn,
													 const MetadataPublishOptions &options) {
		validateRequests(requests);
		if (extents.size() != requests.size()) {
			throwPhysicalAllocatorError(
					FormatError::FERR_INVALID_HEADER,
					"relocatable extent allocation requires one Extent Directory entry per segment");
		}
		std::vector<uint64_t> extentIds;
		std::vector<uint64_t> extentSegmentIds;
		extentIds.reserve(extents.size());
		extentSegmentIds.reserve(extents.size());
		for (const ExtentLocation &extent: extents) {
			const auto request = std::ranges::lower_bound(requests, extent.segment_id, {},
														  &PhysicalSegmentAllocationRequest::segment_id);
			const uint8_t invalid =
					static_cast<uint8_t>(!extent.extent_id.isValid()) |
					static_cast<uint8_t>(request == requests.end() || request->segment_id != extent.segment_id) |
					static_cast<uint8_t>(request != requests.end() && request->generation != extent.generation) |
					static_cast<uint8_t>(extent.first_page != RSS_PAGE_POOL_FIRST_DATA_FRAME) |
					static_cast<uint8_t>(extent.page_count != RSS_PAGE_POOL_DATA_FRAME_COUNT);
			if (invalid != 0U) {
				throwPhysicalAllocatorError(
						FormatError::FERR_INVALID_HEADER,
						"relocatable extent allocation has an invalid full-segment Extent Directory entry");
			}
			extentIds.push_back(extent.extent_id.value());
			extentSegmentIds.push_back(extent.segment_id.value());
		}
		std::ranges::sort(extentIds);
		std::ranges::sort(extentSegmentIds);
		if (std::ranges::adjacent_find(extentIds) != extentIds.end() ||
			std::ranges::adjacent_find(extentSegmentIds) != extentSegmentIds.end()) {
			throwPhysicalAllocatorError(FormatError::FERR_INVALID_HEADER,
										"relocatable extent allocation has duplicate logical Extent or Segment IDs");
		}

		std::vector<CatalogSlotReservationRequest> catalogRequests;
		catalogRequests.reserve(requests.size());
		for (const PhysicalSegmentAllocationRequest &request: requests) {
			catalogRequests.push_back({request.segment_id, request.kind, request.owner_id, request.generation,
									   SegmentState::SGS_SEALED, true});
		}
		const CatalogSlotReservationPreparer preparer = [this, requests](std::span<const CatalogEntry> entries) {
			writePreparedSegments(entries, requests);
		};
		const std::optional<CatalogSlotReservations> reservations =
				metadata_.reserveFirstFreeAndUpsertCatalogAndLocations(
						std::span<const CatalogSlotReservationRequest>(catalogRequests), {}, extents, firstSlot,
						lastSlot, checkpointLsn, preparer, options);
		if (!reservations.has_value()) {
			return std::nullopt;
		}
		return PhysicalSegmentAllocations{reservations->superblock, reservations->entries};
	}

	Superblock PhysicalSegmentAllocator::retire(SegmentId segmentId, Generation expectedGeneration,
												LogSequenceNumber checkpointLsn,
												const MetadataPublishOptions &options) {
		return metadata_.retireCatalogSegment(segmentId, expectedGeneration, checkpointLsn, options);
	}

	Superblock PhysicalSegmentAllocator::releaseRetired(SegmentId segmentId, Generation expectedGeneration,
														LogSequenceNumber checkpointLsn,
														const MetadataPublishOptions &options) {
		return metadata_.releaseRetiredCatalogSegment(segmentId, expectedGeneration, checkpointLsn, options);
	}

	std::optional<PhysicalSegmentRelocation>
	PhysicalSegmentAllocator::relocate(SegmentId segmentId, Generation expectedGeneration, PhysicalSlotId firstSlot,
									   PhysicalSlotId lastSlot, LogSequenceNumber checkpointLsn,
									   const MetadataPublishOptions &options) {
		const CatalogSlotRelocationPreparer preparer = [this](const CatalogEntry &source,
															  const CatalogEntry &destination) {
			copyPreparedRelocation(source, destination);
		};
		const std::optional<CatalogSlotRelocation> relocation = metadata_.relocateCatalogSegment(
				segmentId, expectedGeneration, firstSlot, lastSlot, checkpointLsn, preparer, options);
		if (!relocation.has_value()) {
			return std::nullopt;
		}
		return PhysicalSegmentRelocation{relocation->superblock, relocation->source, relocation->destination};
	}

	std::optional<PhysicalSegmentRelocation> PhysicalSegmentAllocator::relocateExtent(
			const ExtentLocation &expectedExtent, std::span<const std::byte> headerExtension,
			std::span<const std::byte> payload, PhysicalSlotId firstSlot, PhysicalSlotId lastSlot,
			LogSequenceNumber checkpointLsn, const MetadataPublishOptions &options) {
		const CatalogSlotRelocationPreparer preparer =
				[this, headerExtension, payload](const CatalogEntry &source, const CatalogEntry &destination) {
					writeRelocatedExtentSegment(source, destination, headerExtension, payload);
				};
		const std::optional<CatalogSlotRelocation> relocation = metadata_.relocateCatalogExtentSegment(
				expectedExtent, firstSlot, lastSlot, checkpointLsn, preparer, options);
		if (!relocation.has_value()) {
			return std::nullopt;
		}
		return PhysicalSegmentRelocation{relocation->superblock, relocation->source, relocation->destination};
	}

	Superblock PhysicalSegmentAllocator::releaseRelocatedSource(SegmentId segmentId, Generation expectedGeneration,
																LogSequenceNumber checkpointLsn,
																const MetadataPublishOptions &options) {
		return metadata_.releaseRelocatedCatalogSource(segmentId, expectedGeneration, checkpointLsn, options);
	}

	PhysicalSegmentContents PhysicalSegmentAllocator::read(const MetadataSnapshotPin &snapshot,
														   SegmentId segmentId) const {
		const CatalogEntry entry = metadata_.findCatalog(snapshot, segmentId);
		if (entry.state != SegmentState::SGS_SEALED) {
			throwPhysicalAllocatorError(FormatError::FERR_INVALID_STATE,
										"relocatable physical segment read requires a sealed Catalog generation");
		}
		if (!metadata_.isPhysicalSlotAllocated(snapshot, entry.physical_slot)) {
			throwPhysicalAllocatorError(FormatError::FERR_INVALID_STATE,
										"relocatable Catalog mapping references a free physical slot");
		}

		const uint64_t segmentOffset = checkedSlotOffset(data_region_offset_, entry.physical_slot);
		std::vector<std::byte> bytes(RSS_SEGMENT_SIZE);
		readCompleteSegment(*io_, segmentOffset, entry.generation, bytes, "relocatable physical segment is truncated");
		const SegmentEnvelope envelope =
				SegmentEnvelopeCodec::decode(std::span<const std::byte>(bytes.data(), RSS_ENVELOPE_BYTES));
		const uint8_t invalid = static_cast<uint8_t>(envelope.state != SegmentState::SGS_SEALED) |
								static_cast<uint8_t>(envelope.kind != entry.kind) |
								static_cast<uint8_t>(envelope.segment_id != entry.segment_id) |
								static_cast<uint8_t>(envelope.owner_id != entry.owner_id) |
								static_cast<uint8_t>(envelope.generation != entry.generation) |
								static_cast<uint8_t>(envelope.committed_lsn != entry.committed_lsn);
		if (invalid != 0U) {
			throwPhysicalAllocatorError(FormatError::FERR_INVALID_STATE,
										"relocatable physical segment envelope disagrees with its Catalog mapping");
		}

		const uint32_t headerExtensionBytes = envelope.header_bytes - RSS_ENVELOPE_BYTES;
		std::vector<std::byte> headerExtension(bytes.begin() + RSS_ENVELOPE_BYTES,
											   bytes.begin() + RSS_ENVELOPE_BYTES + headerExtensionBytes);
		std::vector<std::byte> payload(bytes.begin() + envelope.header_bytes,
									   bytes.begin() + envelope.header_bytes + envelope.payload_bytes);
		if (payloadCrc(payload) != envelope.payload_crc) {
			throwPhysicalAllocatorError(FormatError::FERR_CHECKSUM_MISMATCH,
										"relocatable physical segment payload checksum does not match");
		}
		return PhysicalSegmentContents{entry, envelope, std::move(headerExtension), std::move(payload)};
	}

	void PhysicalSegmentAllocator::validateRequests(std::span<const PhysicalSegmentAllocationRequest> requests) const {
		if (requests.empty()) {
			throwPhysicalAllocatorError(FormatError::FERR_INVALID_HEADER,
										"relocatable physical allocation requires at least one segment request");
		}
		SegmentId previous;
		for (const PhysicalSegmentAllocationRequest &request: requests) {
			if (!request.segment_id.isValid() || request.segment_id.value() < RSS_FIRST_ALLOCATABLE_SEGMENT_ID ||
				!request.generation.isValid() || request.header_bytes < RSS_ENVELOPE_BYTES ||
				request.header_bytes > RSS_MAX_HEADER_BYTES || request.header_bytes % alignof(uint64_t) != 0U ||
				request.header_extension.size() != request.header_bytes - RSS_ENVELOPE_BYTES ||
				request.payload.size() > RSS_SEGMENT_SIZE - request.header_bytes) {
				throwPhysicalAllocatorError(
						FormatError::FERR_INVALID_HEADER,
						"relocatable physical allocation request has invalid dimensions or identifiers");
			}
			if (!isAllocatableKind(request.kind)) {
				throwPhysicalAllocatorError(FormatError::FERR_INVALID_ENUM,
											"relocatable physical allocation request has an unsupported segment kind");
			}
			if (previous.isValid() && request.segment_id <= previous) {
				throwPhysicalAllocatorError(
						FormatError::FERR_INVALID_HEADER,
						"relocatable physical allocation requests need strictly increasing Segment IDs");
			}
			previous = request.segment_id;
		}
	}

	void
	PhysicalSegmentAllocator::writePreparedSegments(std::span<const CatalogEntry> entries,
													std::span<const PhysicalSegmentAllocationRequest> requests) const {
		for (size_t index = 0; index < entries.size(); ++index) {
			const CatalogEntry &entry = entries[index];
			const PhysicalSegmentAllocationRequest &request = requests[index];
			const uint64_t segmentOffset = checkedSlotOffset(data_region_offset_, entry.physical_slot);
			const std::vector<std::byte> bytes = buildPreparedSegment(entry, request);
			writeCompleteSegment(*io_, segmentOffset, entry.generation, bytes);
		}
	}

	void PhysicalSegmentAllocator::copyPreparedRelocation(const CatalogEntry &source,
														  const CatalogEntry &destination) const {
		const uint8_t invalid = static_cast<uint8_t>(source.state != SegmentState::SGS_SEALED) |
								static_cast<uint8_t>(destination.state != SegmentState::SGS_SEALED) |
								static_cast<uint8_t>(destination.segment_id != source.segment_id) |
								static_cast<uint8_t>(destination.kind != source.kind) |
								static_cast<uint8_t>(destination.owner_id != source.owner_id) |
								static_cast<uint8_t>(destination.physical_slot == source.physical_slot) |
								static_cast<uint8_t>(destination.generation.value() != source.generation.value() + 1U);
		if (invalid != 0U) {
			throwPhysicalAllocatorError(
					FormatError::FERR_INVALID_STATE,
					"relocatable physical relocation preparation has inconsistent Catalog generations");
		}

		std::vector<std::byte> bytes(RSS_SEGMENT_SIZE);
		const uint64_t sourceOffset = checkedSlotOffset(data_region_offset_, source.physical_slot);
		readCompleteSegment(*io_, sourceOffset, source.generation, bytes,
							"relocatable physical relocation source is truncated");
		const SegmentEnvelope sourceEnvelope =
				SegmentEnvelopeCodec::decode(std::span<const std::byte>(bytes.data(), RSS_ENVELOPE_BYTES));
		const uint8_t invalidEnvelope =
				static_cast<uint8_t>(sourceEnvelope.state != SegmentState::SGS_SEALED) |
				static_cast<uint8_t>(sourceEnvelope.kind != source.kind) |
				static_cast<uint8_t>(sourceEnvelope.segment_id != source.segment_id) |
				static_cast<uint8_t>(sourceEnvelope.owner_id != source.owner_id) |
				static_cast<uint8_t>(sourceEnvelope.generation != source.generation) |
				static_cast<uint8_t>(sourceEnvelope.committed_lsn != source.committed_lsn) |
				static_cast<uint8_t>(payloadCrc(std::span<const std::byte>(bytes.data() + sourceEnvelope.header_bytes,
																		   sourceEnvelope.payload_bytes)) !=
									 sourceEnvelope.payload_crc);
		if (invalidEnvelope != 0U) {
			throwPhysicalAllocatorError(FormatError::FERR_INVALID_STATE,
										"relocatable physical relocation source envelope or payload is invalid");
		}

		SegmentEnvelope destinationEnvelope = sourceEnvelope;
		destinationEnvelope.generation = destination.generation;
		destinationEnvelope.committed_lsn = destination.committed_lsn;
		const auto encodedEnvelope = SegmentEnvelopeCodec::encode(destinationEnvelope);
		std::copy(encodedEnvelope.begin(), encodedEnvelope.end(), bytes.begin());
		writeCompleteSegment(*io_, checkedSlotOffset(data_region_offset_, destination.physical_slot),
							 destination.generation, bytes);
	}

	void PhysicalSegmentAllocator::writeRelocatedExtentSegment(const CatalogEntry &source,
															   const CatalogEntry &destination,
															   std::span<const std::byte> headerExtension,
															   std::span<const std::byte> payload) const {
		const uint64_t headerBytes = RSS_ENVELOPE_BYTES + headerExtension.size();
		const uint8_t invalid = static_cast<uint8_t>(source.state != SegmentState::SGS_SEALED) |
								static_cast<uint8_t>(source.kind != SegmentKind::SGK_CODEBOOK_EXTENT) |
								static_cast<uint8_t>(destination.state != SegmentState::SGS_SEALED) |
								static_cast<uint8_t>(destination.segment_id != source.segment_id) |
								static_cast<uint8_t>(destination.kind != source.kind) |
								static_cast<uint8_t>(destination.owner_id != source.owner_id) |
								static_cast<uint8_t>(destination.physical_slot == source.physical_slot) |
								static_cast<uint8_t>(destination.generation.value() != source.generation.value() + 1U) |
								static_cast<uint8_t>(headerBytes > RSS_MAX_HEADER_BYTES) |
								static_cast<uint8_t>(headerBytes % alignof(uint64_t) != 0U) |
								static_cast<uint8_t>(payload.size() > RSS_SEGMENT_SIZE - headerBytes);
		if (invalid != 0U) {
			throwPhysicalAllocatorError(
					FormatError::FERR_INVALID_STATE,
					"relocatable extent relocation preparation has inconsistent state or dimensions");
		}
		const PhysicalSegmentAllocationRequest request{destination.segment_id,
													   destination.kind,
													   destination.owner_id,
													   destination.generation,
													   static_cast<uint16_t>(headerBytes),
													   headerExtension,
													   payload};
		writePreparedSegments(std::span(&destination, 1), std::span(&request, 1));
	}

	uint64_t PhysicalSegmentAllocator::checkedSlotOffset(uint64_t dataRegionOffset, PhysicalSlotId slot) {
		if (!slot.isValid()) {
			throwPhysicalAllocatorError(FormatError::FERR_INVALID_HEADER,
										"relocatable physical allocation has an invalid physical slot");
		}
		if (dataRegionOffset < RSS_FILE_BOOTSTRAP_BYTES || dataRegionOffset % RSS_SEGMENT_SIZE != 0U) {
			throwPhysicalAllocatorError(FormatError::FERR_INVALID_HEADER,
										"relocatable physical allocation has an invalid data-region offset");
		}
		const uint64_t available = std::numeric_limits<uint64_t>::max() - dataRegionOffset;
		const uint64_t segmentCapacity = available / RSS_SEGMENT_SIZE;
		const uint64_t slotIndex = slot.value() - 1U;
		if (segmentCapacity == 0U || slotIndex >= segmentCapacity) {
			throwPhysicalAllocatorError(FormatError::FERR_ARITHMETIC_OVERFLOW,
										"relocatable physical segment slot offset overflows");
		}
		return dataRegionOffset + slotIndex * RSS_SEGMENT_SIZE;
	}

} // namespace graph::storage
