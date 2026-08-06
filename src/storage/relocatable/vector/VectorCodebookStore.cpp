/**
 * @file VectorCodebookStore.cpp
 * @brief Immutable inline and multi-extent Vector Codebook storage for relocatable.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/vector/VectorCodebookStore.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

#include "graph/utils/ChecksumUtils.hpp"

namespace graph::storage {

	namespace {

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		[[nodiscard]] uint32_t checksum(std::span<const std::byte> bytes) {
			const std::byte empty{};
			return utils::calculateCrc(bytes.empty() ? &empty : bytes.data(), bytes.size()); // ZYX_COV_EXCL_LINE: all
			// Codebook payloads and immutable extents are nonempty by their checked codecs; the sentinel keeps the
			// helper defensive for a future internal caller without requiring an unrepresentable empty Codebook object
			// in tests.
		}

		void validateFullSegmentExtent(const ExtentLocation &location) {
			const uint8_t invalid = static_cast<uint8_t>(!location.extent_id.isValid()) |
									static_cast<uint8_t>(!location.segment_id.isValid()) |
									static_cast<uint8_t>(!location.generation.isValid()) |
									static_cast<uint8_t>(location.first_page != RSS_PAGE_POOL_FIRST_DATA_FRAME) |
									static_cast<uint8_t>(location.page_count != RSS_PAGE_POOL_DATA_FRAME_COUNT);
			if (invalid != 0U) {
				fail(FormatError::FERR_INVALID_STATE,
					 "relocatable Vector Codebook Extent Directory entry is not one full immutable segment");
			}
		}

		[[nodiscard]] VectorCodebookHeader logicalHeader(const VectorCodebookWriteRequest &request,
														 std::span<const std::byte> payload) {
			VectorCodebookHeader header;
			header.codebook_id = request.codebook_id;
			header.generation = request.generation;
			header.element_type = request.element_type;
			header.dimensions = request.dimensions;
			header.subspace_count = request.subspace_count;
			header.centroid_count = request.centroid_count;
			switch (request.element_type) {
				case VectorCodebookElementType::VCET_FLOAT32:
					header.element_bytes = 4U;
					break;
				case VectorCodebookElementType::VCET_UINT8:
					header.element_bytes = 1U;
					break;
				default:
					fail(FormatError::FERR_INVALID_ENUM,
						 "relocatable Vector Codebook write uses an unknown element type");
			}
			if (payload.size() >
				std::numeric_limits<uint32_t>::max()) { // ZYX_COV_EXCL_LINE: exercising this requires a
				// contiguous payload larger than the on-disk uint32_t format limit; the guard remains before narrowing.
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
					 "relocatable Vector Codebook payload exceeds its persistent logical length");
			}
			header.payload_bytes = static_cast<uint32_t>(payload.size());
			header.payload_crc = checksum(payload);
			VectorCodebookHeaderCodec::validatePayload(header, payload);
			return header;
		}

		void validateWriteRequest(const VectorCodebookWriteRequest &request, size_t payloadBytes) {
			const size_t requiredExtents = VectorCodebookManifestCodec::requiredDataExtentCount(payloadBytes);
			const uint8_t invalid =
					static_cast<uint8_t>(!VectorCodebookExtentIdCodec::canEncodeCodebookId(request.codebook_id)) |
					static_cast<uint8_t>(!request.manifest_segment_id.isValid()) |
					static_cast<uint8_t>(!request.generation.isValid()) |
					static_cast<uint8_t>(request.data_extents.size() != requiredExtents);
			if (invalid != 0U) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable Vector Codebook write has invalid identities, LSN, or data-extent count");
			}
			std::set<uint64_t> extentIds;
			std::set<uint64_t> segmentIds;
			segmentIds.insert(request.manifest_segment_id.value());
			for (const VectorCodebookDataExtent &extent: request.data_extents) {
				const uint8_t invalidExtent =
						static_cast<uint8_t>(!extent.extent_id.isValid()) |
						static_cast<uint8_t>(!extent.segment_id.isValid()) |
						static_cast<uint8_t>(!VectorCodebookExtentIdCodec::isDataExtentForSegment(extent.extent_id,
																								  extent.segment_id)) |
						static_cast<uint8_t>(!extentIds.insert(extent.extent_id.value()).second) |
						static_cast<uint8_t>(!segmentIds.insert(extent.segment_id.value()).second);
				if (invalidExtent != 0U) {
					fail(FormatError::FERR_INVALID_HEADER,
						 "relocatable Vector Codebook write has a duplicate or invalid data Extent/Segment ID");
				}
			}
		}

		[[nodiscard]] VectorCodebookPreparedExtent prepareExtent(uint64_t codebookId, Generation codebookGeneration,
																 ExtentId extentId, SegmentId segmentId,
																 VectorCodebookExtentKind kind, uint32_t ordinal,
																 std::span<const std::byte> payload) {
			if (payload.size() > RSS_VECTOR_CODEBOOK_MAX_BLOCK_BYTES) { // ZYX_COV_EXCL_LINE: writeImmutable is the only
				// caller and deterministically splits every block at this exact limit before invoking this helper.
				fail(FormatError::FERR_INVALID_HEADER, "relocatable Vector Codebook block exceeds one extent");
			}
			VectorCodebookExtentHeader header;
			header.kind = kind;
			header.codebook_id = codebookId;
			header.codebook_generation = codebookGeneration;
			header.extent_id = extentId;
			header.ordinal = ordinal;
			header.block_bytes = static_cast<uint32_t>(payload.size());
			header.block_crc = checksum(payload);
			VectorCodebookPreparedExtent prepared;
			prepared.segment_id = segmentId;
			prepared.location = {extentId, segmentId, RSS_PAGE_POOL_FIRST_DATA_FRAME, RSS_PAGE_POOL_DATA_FRAME_COUNT,
								 codebookGeneration};
			prepared.header = VectorCodebookExtentHeaderCodec::encode(header);
			prepared.payload.assign(payload.begin(), payload.end());
			return prepared;
		}

		[[nodiscard]] bool hasExpectedSegment(const PhysicalSegmentContents &contents, const ExtentLocation &location,
											  uint64_t codebookId) {
			const uint8_t invalid =
					static_cast<uint8_t>(contents.entry.kind != SegmentKind::SGK_CODEBOOK_EXTENT) |
					static_cast<uint8_t>(contents.entry.owner_id != codebookId) |
					static_cast<uint8_t>(contents.entry.segment_id != location.segment_id) |
					static_cast<uint8_t>(contents.entry.generation != location.generation) |
					static_cast<uint8_t>(contents.envelope.generation != location.generation) |
					static_cast<uint8_t>(contents.header_extension.size() != RSS_VECTOR_CODEBOOK_EXTENT_HEADER_BYTES);
			return invalid == 0U;
		}

		[[nodiscard]] VectorCodebookExtentHeader readExtentHeader(const PhysicalSegmentContents &contents,
																  const ExtentLocation &location, uint64_t codebookId) {
			if (!hasExpectedSegment(contents, location, codebookId)) {
				fail(FormatError::FERR_INVALID_STATE,
					 "relocatable Vector Codebook extent disagrees with its Catalog or Extent Directory mapping");
			}
			const VectorCodebookExtentHeader header =
					VectorCodebookExtentHeaderCodec::decode(contents.header_extension);
			const uint8_t invalid = static_cast<uint8_t>(header.codebook_id != codebookId) |
									static_cast<uint8_t>(header.extent_id != location.extent_id);
			if (invalid != 0U) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable Vector Codebook extent header disagrees with its stable identity");
			}
			VectorCodebookExtentHeaderCodec::validateBlock(header, contents.payload);
			return header;
		}

	} // namespace

	VectorCodebookStore::VectorCodebookStore(PhysicalSegmentAllocator &segments, RelocatableMetadataStore &metadata,
											 size_t maximumCacheBytes, size_t maximumCacheEntries) :
		segments_(segments), metadata_(metadata), maximum_cache_bytes_(maximumCacheBytes),
		maximum_cache_entries_(maximumCacheEntries) {
		if ((maximum_cache_bytes_ == 0U) != (maximum_cache_entries_ == 0U)) {
			throw std::invalid_argument(
					"relocatable Vector Codebook cache byte and entry bounds must both be zero or both be positive");
		}
	}

	std::vector<CatalogSlotReservationRequest> VectorCodebookPreparedWrite::catalogReservations() const {
		std::vector<CatalogSlotReservationRequest> result;
		result.reserve(extents_.size());
		for (const VectorCodebookPreparedExtent &extent: extents_) {
			result.push_back({extent.segment_id, SegmentKind::SGK_CODEBOOK_EXTENT, codebook_id_, generation_,
							  SegmentState::SGS_SEALED, true});
		}
		return result;
	}

	VectorCodebookPreparedWrite VectorCodebookStore::prepareImmutable(const VectorCodebookWriteRequest &request,
																	  std::span<const std::byte> payload) {
		validateWriteRequest(request, payload.size());
		VectorCodebookPreparedWrite candidate;
		candidate.codebook_id_ = request.codebook_id;
		candidate.generation_ = request.generation;
		candidate.manifest_.header = logicalHeader(request, payload);
		candidate.extents_.reserve(request.data_extents.size() + 1U);
		if (request.data_extents.empty()) {
			candidate.manifest_.inline_payload.assign(payload.begin(), payload.end());
		} else {
			candidate.manifest_.entries.reserve(request.data_extents.size());
			size_t offset = 0U;
			for (size_t index = 0U; index < request.data_extents.size(); ++index) {
				const size_t blockBytes =
						std::min(payload.size() - offset, static_cast<size_t>(RSS_VECTOR_CODEBOOK_MAX_BLOCK_BYTES));
				const VectorCodebookDataExtent &extent = request.data_extents[index];
				VectorCodebookPreparedExtent block =
						prepareExtent(request.codebook_id, request.generation, extent.extent_id, extent.segment_id,
									  VectorCodebookExtentKind::VCEK_DATA, static_cast<uint32_t>(index),
									  payload.subspan(offset, blockBytes));
				const VectorCodebookExtentHeader blockHeader = VectorCodebookExtentHeaderCodec::decode(block.header);
				candidate.manifest_.entries.push_back(
						{extent.extent_id, blockHeader.block_bytes, blockHeader.block_crc});
				candidate.extents_.push_back(std::move(block));
				offset += blockBytes;
			}
		}

		const std::vector<std::byte> manifestBytes = VectorCodebookManifestCodec::encode(candidate.manifest_);
		candidate.extents_.push_back(prepareExtent(request.codebook_id, request.generation,
												   VectorCodebookExtentIdCodec::manifestExtentId(request.codebook_id),
												   request.manifest_segment_id, VectorCodebookExtentKind::VCEK_MANIFEST,
												   RSS_VECTOR_CODEBOOK_MANIFEST_ORDINAL, manifestBytes));
		std::ranges::sort(candidate.extents_, {}, &VectorCodebookPreparedExtent::segment_id);

		candidate.locations_.reserve(candidate.extents_.size());
		for (const VectorCodebookPreparedExtent &extent: candidate.extents_) {
			candidate.locations_.push_back(extent.location);
		}
		std::ranges::sort(candidate.locations_, {}, &ExtentLocation::extent_id);
		return candidate;
	}

	void VectorCodebookStore::writePreparedImmutable(const VectorCodebookPreparedWrite &candidate,
													 std::span<const CatalogEntry> entries) const {
		const uint8_t malformed =
				static_cast<uint8_t>(candidate.codebook_id_ == 0U) |
				static_cast<uint8_t>(!candidate.generation_.isValid()) |
				static_cast<uint8_t>(candidate.extents_.empty()) |
				static_cast<uint8_t>(entries.size() != candidate.extents_.size()) |
				static_cast<uint8_t>(candidate.locations_.size() != candidate.extents_.size()) |
				static_cast<uint8_t>(
						!std::ranges::is_sorted(candidate.extents_, {}, &VectorCodebookPreparedExtent::segment_id)) |
				static_cast<uint8_t>(!std::ranges::is_sorted(candidate.locations_, {}, &ExtentLocation::extent_id));
		if (malformed != 0U) {
			fail(FormatError::FERR_INVALID_STATE,
				 "relocatable prepared Vector Codebook has an invalid candidate or Catalog-entry cardinality");
		}

		std::vector<PhysicalSegmentAllocationRequest> allocations;
		allocations.reserve(candidate.extents_.size());
		LogSequenceNumber checkpointLsn;
		for (size_t index = 0U; index < candidate.extents_.size(); ++index) {
			const VectorCodebookPreparedExtent &extent = candidate.extents_[index];
			const CatalogEntry &entry = entries[index];
			const auto location = std::ranges::lower_bound(candidate.locations_, extent.location.extent_id, {},
														   &ExtentLocation::extent_id);
			const VectorCodebookExtentHeader header = VectorCodebookExtentHeaderCodec::decode(extent.header);
			const bool manifestExtent =
					extent.location.extent_id == VectorCodebookExtentIdCodec::manifestExtentId(candidate.codebook_id_);
			const uint8_t invalid =
					static_cast<uint8_t>(!entry.segment_id.isValid()) |
					static_cast<uint8_t>(!entry.physical_slot.isValid()) |
					static_cast<uint8_t>(entry.segment_id != extent.segment_id) |
					static_cast<uint8_t>(entry.kind != SegmentKind::SGK_CODEBOOK_EXTENT) |
					static_cast<uint8_t>(entry.owner_id != candidate.codebook_id_) |
					static_cast<uint8_t>(entry.generation != candidate.generation_) |
					static_cast<uint8_t>(entry.state != SegmentState::SGS_SEALED) |
					static_cast<uint8_t>(!entry.committed_lsn.isValid()) |
					static_cast<uint8_t>(index != 0U && entry.committed_lsn != checkpointLsn) |
					static_cast<uint8_t>(location == candidate.locations_.end() || *location != extent.location) |
					static_cast<uint8_t>(header.codebook_id != candidate.codebook_id_) |
					static_cast<uint8_t>(header.codebook_generation != candidate.generation_) |
					static_cast<uint8_t>(header.extent_id != extent.location.extent_id) |
					static_cast<uint8_t>(header.kind != (manifestExtent ? VectorCodebookExtentKind::VCEK_MANIFEST
																		: VectorCodebookExtentKind::VCEK_DATA)) |
					static_cast<uint8_t>(manifestExtent && header.ordinal != RSS_VECTOR_CODEBOOK_MANIFEST_ORDINAL) |
					static_cast<uint8_t>(!manifestExtent && !VectorCodebookExtentIdCodec::isDataExtentForSegment(
																	extent.location.extent_id, extent.segment_id));
			if (invalid != 0U) {
				fail(FormatError::FERR_INVALID_STATE,
					 "relocatable prepared Vector Codebook Catalog entry or typed extent disagrees with its candidate");
			}
			VectorCodebookExtentHeaderCodec::validateBlock(header, extent.payload);
			if (index == 0U) {
				checkpointLsn = entry.committed_lsn;
			}
			allocations.push_back({extent.segment_id, SegmentKind::SGK_CODEBOOK_EXTENT, candidate.codebook_id_,
								   candidate.generation_,
								   static_cast<uint16_t>(RSS_ENVELOPE_BYTES + RSS_VECTOR_CODEBOOK_EXTENT_HEADER_BYTES),
								   extent.header, extent.payload});
		}
		segments_.writePreparedSegments(entries, allocations);
	}

	Superblock VectorCodebookStore::writeImmutable(const VectorCodebookWriteRequest &request,
												   std::span<const std::byte> payload, PhysicalSlotId firstSlot,
												   PhysicalSlotId lastSlot, LogSequenceNumber checkpointLsn,
												   const MetadataPublishOptions &options) {
		if (!checkpointLsn.isValid()) {
			fail(FormatError::FERR_INVALID_HEADER, "relocatable Vector Codebook write requires a valid checkpoint LSN");
		}
		VectorCodebookPreparedWrite candidate = prepareImmutable(request, payload);
		std::vector<PhysicalSegmentAllocationRequest> allocations;
		allocations.reserve(candidate.extents_.size());
		for (VectorCodebookPreparedExtent &extent: candidate.extents_) {
			PhysicalSegmentAllocationRequest allocation;
			allocation.segment_id = extent.segment_id;
			allocation.kind = SegmentKind::SGK_CODEBOOK_EXTENT;
			allocation.owner_id = candidate.codebook_id_;
			allocation.generation = candidate.generation_;
			allocation.header_bytes =
					static_cast<uint16_t>(RSS_ENVELOPE_BYTES + RSS_VECTOR_CODEBOOK_EXTENT_HEADER_BYTES);
			allocation.header_extension.assign(extent.header.begin(), extent.header.end());
			allocation.payload = std::move(extent.payload);
			allocations.push_back(std::move(allocation));
		}
		const std::optional<PhysicalSegmentAllocations> result = segments_.allocateExtentSegments(
				allocations, candidate.locations_, firstSlot, lastSlot, checkpointLsn, options);
		if (!result.has_value()) {
			fail(FormatError::FERR_INVALID_STATE,
				 "relocatable Vector Codebook write could not reserve every required physical segment");
		}
		return result->superblock;
	}

	VectorCodebookManifest VectorCodebookStore::manifest(const MetadataSnapshotPin &snapshot,
														 uint64_t codebookId) const {
		if (!VectorCodebookExtentIdCodec::canEncodeCodebookId(codebookId)) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "relocatable Vector Codebook read requires an encodable logical Codebook ID");
		}
		const ExtentLocation location =
				metadata_.findExtentLocation(snapshot, VectorCodebookExtentIdCodec::manifestExtentId(codebookId));
		validateFullSegmentExtent(location);
		const PhysicalSegmentContents contents = segments_.read(snapshot, location.segment_id);
		const VectorCodebookExtentHeader extentHeader = readExtentHeader(contents, location, codebookId);
		VectorCodebookManifest immutableManifest = VectorCodebookManifestCodec::decode(contents.payload);
		if (immutableManifest.header.codebook_id != codebookId ||
			immutableManifest.header.generation != extentHeader.codebook_generation) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "relocatable Vector Codebook logical manifest disagrees with its root extent identity");
		}
		return immutableManifest;
	}

	VectorCodebook VectorCodebookStore::read(const MetadataSnapshotPin &snapshot, uint64_t codebookId) const {
		return readBounded(snapshot, codebookId, std::numeric_limits<uint32_t>::max(),
						   std::numeric_limits<uint32_t>::max());
	}

	VectorCodebook VectorCodebookStore::readBounded(const MetadataSnapshotPin &snapshot, uint64_t codebookId,
													uint64_t maximumPayloadBytes, size_t maximumDataExtents) const {
		return *readSharedBounded(snapshot, codebookId, maximumPayloadBytes, maximumDataExtents);
	}

	std::shared_ptr<const VectorCodebook> VectorCodebookStore::readSharedBounded(const MetadataSnapshotPin &snapshot,
																				 uint64_t codebookId,
																				 uint64_t maximumPayloadBytes,
																				 size_t maximumDataExtents) const {
		const VectorCodebookManifest immutableManifest = manifest(snapshot, codebookId);
		if (immutableManifest.header.payload_bytes > maximumPayloadBytes ||
			immutableManifest.entries.size() > maximumDataExtents) {
			fail(FormatError::FERR_INVALID_STATE,
				 "relocatable Vector Codebook exceeds its caller-provided payload or extent bound");
		}
		const CacheKey key{codebookId, immutableManifest.header.generation.value()};
		if (maximum_cache_entries_ != 0U) {
			std::lock_guard lock(cache_mutex_);
			const auto cached = cache_.find(key);
			if (cached != cache_.end()) {
				if (cached->second.codebook->header != immutableManifest.header) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable Vector Codebook cache identity disagrees with the snapshot manifest");
				}
				cache_recency_.splice(cache_recency_.begin(), cache_recency_, cached->second.recency);
				return cached->second.codebook;
			}
		}

		std::shared_ptr<const VectorCodebook> loaded;
		if (immutableManifest.isInline()) {
			loaded = std::make_shared<const VectorCodebook>(
					VectorCodebook{immutableManifest.header, immutableManifest.inline_payload});
		} else {
			std::vector<std::byte> payload;
			payload.reserve(immutableManifest.header.payload_bytes);
			for (size_t index = 0U; index < immutableManifest.entries.size(); ++index) {
				const std::vector<std::byte> block =
						readDataExtent(snapshot, immutableManifest, immutableManifest.entries[index], index);
				payload.insert(payload.end(), block.begin(), block.end());
			}
			VectorCodebookHeaderCodec::validatePayload(immutableManifest.header, payload);
			loaded = std::make_shared<const VectorCodebook>(
					VectorCodebook{immutableManifest.header, std::move(payload)});
		}

		const size_t payloadBytes = loaded->payload.size();
		if (maximum_cache_entries_ == 0U || payloadBytes > maximum_cache_bytes_) {
			return loaded;
		}
		std::lock_guard lock(cache_mutex_);
		const auto raced = cache_.find(key);
		if (raced != cache_.end()) {
			cache_recency_.splice(cache_recency_.begin(), cache_recency_, raced->second.recency);
			return raced->second.codebook;
		}
		if (cached_payload_bytes_ > maximum_cache_bytes_) { // ZYX_COV_EXCL_LINE: this private counter changes only
			// while this mutex is held and is updated by the exact payload delta for every insertion and eviction.
			fail(FormatError::FERR_INVALID_STATE, "relocatable Vector Codebook cache accounting is inconsistent");
		}
		while (!cache_recency_.empty() && (cache_.size() >= maximum_cache_entries_ ||
										   payloadBytes > maximum_cache_bytes_ - cached_payload_bytes_)) {
			const CacheKey victim = cache_recency_.back();
			const auto existing = cache_.find(victim);
			if (existing == cache_.end() ||
				existing->second.codebook->payload.size() > cached_payload_bytes_) { // ZYX_COV_EXCL_LINE:
				// the list and map are mutated together under this mutex and each entry contributes its exact payload
				// size.
				fail(FormatError::FERR_INVALID_STATE, "relocatable Vector Codebook cache accounting is inconsistent");
			}
			cached_payload_bytes_ -= existing->second.codebook->payload.size();
			cache_.erase(existing);
			cache_recency_.pop_back();
		}
		cache_recency_.push_front(key);
		cache_.emplace(key, CacheEntry{loaded, cache_recency_.begin()});
		cached_payload_bytes_ += payloadBytes;
		return loaded;
	}

	void VectorCodebookStore::validate(const MetadataSnapshotPin &snapshot, uint64_t codebookId,
									   uint64_t maximumPayloadBytes, size_t maximumDataExtents) const {
		const VectorCodebookManifest immutableManifest = manifest(snapshot, codebookId);
		if (immutableManifest.header.payload_bytes > maximumPayloadBytes ||
			immutableManifest.entries.size() > maximumDataExtents) {
			fail(FormatError::FERR_INVALID_STATE,
				 "relocatable Vector Codebook exceeds its caller-provided validation bound");
		}
		if (immutableManifest.isInline()) {
			return;
		}
		uint64_t totalBytes = 0U;
		uint32_t payloadCrc = 0U;
		for (size_t index = 0U; index < immutableManifest.entries.size(); ++index) {
			const std::vector<std::byte> block =
					readDataExtent(snapshot, immutableManifest, immutableManifest.entries[index], index);
			totalBytes += block.size();
			payloadCrc = graph::utils::updateCrc(payloadCrc, block.data(), block.size());
		}
		if (totalBytes != immutableManifest.header.payload_bytes ||
			payloadCrc != immutableManifest.header.payload_crc) {
			fail(FormatError::FERR_CHECKSUM_MISMATCH,
				 "relocatable Vector Codebook data extents do not match the logical payload checksum");
		}
	}

	std::vector<std::byte> VectorCodebookStore::readDataExtent(const MetadataSnapshotPin &snapshot,
															   const VectorCodebookManifest &immutableManifest,
															   const VectorCodebookManifestEntry &entry,
															   size_t ordinal) const {
		const ExtentLocation location = metadata_.findExtentLocation(snapshot, entry.extent_id);
		validateFullSegmentExtent(location);
		const PhysicalSegmentContents contents = segments_.read(snapshot, location.segment_id);
		const VectorCodebookExtentHeader extentHeader =
				readExtentHeader(contents, location, immutableManifest.header.codebook_id);
		const uint8_t invalid =
				static_cast<uint8_t>(extentHeader.kind != VectorCodebookExtentKind::VCEK_DATA) |
				static_cast<uint8_t>(!VectorCodebookExtentIdCodec::isDataExtentForSegment(extentHeader.extent_id,
																						  location.segment_id)) |
				static_cast<uint8_t>(extentHeader.codebook_generation != immutableManifest.header.generation) |
				static_cast<uint8_t>(extentHeader.ordinal != ordinal) |
				static_cast<uint8_t>(extentHeader.block_bytes != entry.block_bytes) |
				static_cast<uint8_t>(extentHeader.block_crc != entry.block_crc);
		if (invalid != 0U) {
			fail(FormatError::FERR_INVALID_STATE,
				 "relocatable Vector Codebook data extent disagrees with its immutable manifest entry");
		}
		return contents.payload;
	}

	std::optional<PhysicalSegmentRelocation>
	VectorCodebookStore::relocateExtent(uint64_t codebookId, ExtentId extentId, PhysicalSlotId firstSlot,
										PhysicalSlotId lastSlot, LogSequenceNumber checkpointLsn,
										const MetadataPublishOptions &options) {
		if (!VectorCodebookExtentIdCodec::canEncodeCodebookId(codebookId) || !extentId.isValid() ||
			checkpointLsn.isValid() == false) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "relocatable Vector Codebook relocation requires Codebook, extent, and checkpoint identities");
		}
		MetadataSnapshotPin snapshot = metadata_.openSnapshot();
		try {
			const VectorCodebookManifest immutableManifest = manifest(snapshot, codebookId);
			const ExtentLocation expected = metadata_.findExtentLocation(snapshot, extentId);
			validateFullSegmentExtent(expected);
			const PhysicalSegmentContents contents = segments_.read(snapshot, expected.segment_id);
			const VectorCodebookExtentHeader header = readExtentHeader(contents, expected, codebookId);
			if (extentId != VectorCodebookExtentIdCodec::manifestExtentId(codebookId)) {
				const auto entry =
						std::ranges::find(immutableManifest.entries, extentId, &VectorCodebookManifestEntry::extent_id);
				const size_t ordinal = static_cast<size_t>(entry - immutableManifest.entries.begin());
				const uint8_t invalid =
						static_cast<uint8_t>(entry == immutableManifest.entries.end()) |
						static_cast<uint8_t>(header.kind != VectorCodebookExtentKind::VCEK_DATA) |
						static_cast<uint8_t>(!VectorCodebookExtentIdCodec::isDataExtentForSegment(
								header.extent_id, expected.segment_id)) |
						static_cast<uint8_t>(header.codebook_generation != immutableManifest.header.generation) |
						static_cast<uint8_t>(entry != immutableManifest.entries.end() && header.ordinal != ordinal) |
						static_cast<uint8_t>(entry != immutableManifest.entries.end() &&
											 header.block_crc != entry->block_crc);
				if (invalid != 0U) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable Vector Codebook relocation target is not owned by the immutable manifest");
				}
			}
			const VectorCodebookExtentHeaderCodec::Bytes rebuiltHeader =
					VectorCodebookExtentHeaderCodec::encode(header);
			snapshot.release();
			return segments_.relocateExtent(expected, rebuiltHeader, contents.payload, firstSlot, lastSlot,
											checkpointLsn, options);
		} catch (...) {
			snapshot.release();
			throw;
		}
	}

} // namespace graph::storage
