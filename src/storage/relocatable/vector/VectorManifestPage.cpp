/**
 * @file VectorManifestPage.cpp
 * @brief Immutable, checksummed vector-index manifest frame for relocatable.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/vector/VectorManifestPage.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <set>

#include "graph/storage/relocatable/vector/VectorAdjacencyMapping.hpp"
#include "graph/storage/relocatable/vector/VectorDeltaMapping.hpp"
#include "graph/storage/relocatable/vector/VectorNodeMapping.hpp"
#include "graph/utils/ChecksumUtils.hpp"

namespace graph::storage {

	namespace {
		constexpr size_t MAGIC_OFFSET = 0;
		constexpr size_t VERSION_OFFSET = 4;
		constexpr size_t HEADER_BYTES_OFFSET = 6;
		constexpr size_t FLAGS_OFFSET = 8;
		constexpr size_t ENTRY_BYTES_OFFSET = 12;
		constexpr size_t ENTRY_COUNT_OFFSET = 14;
		constexpr size_t DIMENSIONS_OFFSET = 16;
		constexpr size_t METRIC_OFFSET = 20;
		constexpr size_t METRIC_RESERVED_OFFSET = 22;
		constexpr size_t INDEX_ID_OFFSET = 24;
		constexpr size_t PAGE_ID_OFFSET = 32;
		constexpr size_t GENERATION_OFFSET = 40;
		constexpr size_t LSN_OFFSET = 48;
		constexpr size_t CODEBOOK_ID_OFFSET = 56;
		constexpr size_t CODEBOOK_GENERATION_OFFSET = 64;
		constexpr size_t ENTRY_POINT_VECTOR_ID_OFFSET = 72;
		constexpr size_t ADJACENCY_GENERATION_OFFSET = 80;
		constexpr size_t ADJACENCY_MAPPING_INDEX_ID_OFFSET = 88;
		constexpr size_t ADJACENCY_MAPPING_ROOT_PAGE_ID_OFFSET = 96;
		constexpr size_t ADJACENCY_MAPPING_ROOT_GENERATION_OFFSET = 104;
		constexpr size_t NODE_MAPPING_ROOT_PAGE_ID_OFFSET = 112;
		constexpr size_t NODE_MAPPING_ROOT_GENERATION_OFFSET = 120;
		constexpr size_t PAYLOAD_CRC_OFFSET = 128;
		constexpr size_t HEADER_CRC_OFFSET = 132;
		constexpr size_t PQ_MAPPING_ROOT_PAGE_ID_OFFSET = 136;
		constexpr size_t PQ_MAPPING_ROOT_GENERATION_OFFSET = 144;
		constexpr size_t PQ_MAPPING_INDEX_ID_OFFSET = 152;
		constexpr size_t RAW_WRITE_TAIL_PAGE_ID_OFFSET = 160;
		constexpr size_t VECTOR_NODE_MAPPING_INDEX_ID_OFFSET = 168;
		constexpr size_t VECTOR_NODE_MAPPING_ROOT_PAGE_ID_OFFSET = 176;
		constexpr size_t VECTOR_NODE_MAPPING_ROOT_GENERATION_OFFSET = 184;
		constexpr size_t DELTA_MAPPING_INDEX_ID_OFFSET = 192;
		constexpr size_t DELTA_MAPPING_ROOT_PAGE_ID_OFFSET = 200;
		constexpr size_t DELTA_MAPPING_ROOT_GENERATION_OFFSET = 208;
		constexpr size_t DELTA_ENTRY_COUNT_OFFSET = 216;
		constexpr size_t DELTA_RESERVED_OFFSET = 220;
		constexpr size_t ENTRY_PAGE_ID_OFFSET = 0;
		constexpr size_t ENTRY_KIND_OFFSET = 8;
		constexpr size_t ENTRY_RECORD_COUNT_OFFSET = 10;

		[[noreturn]] void throwManifestError(FormatError error, const char *message) {
			throw FormatException(error, message);
		}

		[[nodiscard]] uint32_t checksum(std::span<const std::byte> bytes) {
			return utils::calculateCrc(bytes.data(), bytes.size());
		}

		[[nodiscard]] bool isStorageKind(VectorStorageKind kind) {
			return kind == VectorStorageKind::VSK_RAW || kind == VectorStorageKind::VSK_PQ;
		}

		[[nodiscard]] bool isMetric(VectorDistanceMetric metric) {
			return metric == VectorDistanceMetric::VDM_L2 || metric == VectorDistanceMetric::VDM_COSINE ||
				   metric == VectorDistanceMetric::VDM_INNER_PRODUCT;
		}

		void validateManifest(const VectorManifest &manifest) {
			const bool hasCodebook = manifest.codebook_id != 0U;
			const bool hasPqMapping = manifest.pq_mapping_index_id != 0U;
			const bool hasAdjacencyMapping = manifest.adjacency_mapping_index_id != 0U;
			const bool hasNodeMapping = manifest.node_mapping_root_page_id.isValid();
			const bool hasVectorNodeMapping = manifest.vector_node_mapping_index_id != 0U;
			const bool hasDeltaMapping = manifest.delta_mapping_index_id != 0U;
			const uint8_t invalid =
					static_cast<uint8_t>(manifest.index_id == 0U) | static_cast<uint8_t>(!manifest.page_id.isValid()) |
					static_cast<uint8_t>(!manifest.generation.isValid()) |
					static_cast<uint8_t>(!manifest.committed_lsn.isValid()) |
					static_cast<uint8_t>(manifest.dimensions == 0U) | static_cast<uint8_t>(!isMetric(manifest.metric)) |
					static_cast<uint8_t>(hasCodebook &&
										 !VectorCodebookExtentIdCodec::canEncodeCodebookId(manifest.codebook_id)) |
					static_cast<uint8_t>(hasCodebook != manifest.codebook_generation.isValid()) |
					static_cast<uint8_t>(hasNodeMapping != manifest.node_mapping_root_generation.isValid()) |
					static_cast<uint8_t>(hasNodeMapping != hasVectorNodeMapping) |
					static_cast<uint8_t>(hasVectorNodeMapping &&
										 !VectorNodeMapping::isMappingIndexId(manifest.vector_node_mapping_index_id)) |
					static_cast<uint8_t>(hasVectorNodeMapping !=
										 manifest.vector_node_mapping_root_page_id.isValid()) |
					static_cast<uint8_t>(hasVectorNodeMapping !=
										 manifest.vector_node_mapping_root_generation.isValid()) |
					static_cast<uint8_t>(hasPqMapping &&
										 !VectorPqMapping::isMappingIndexId(manifest.pq_mapping_index_id)) |
					static_cast<uint8_t>(hasPqMapping != manifest.pq_mapping_root_page_id.isValid()) |
					static_cast<uint8_t>(hasPqMapping != manifest.pq_mapping_root_generation.isValid()) |
					static_cast<uint8_t>(hasDeltaMapping &&
										 !VectorDeltaMapping::isMappingIndexId(manifest.delta_mapping_index_id)) |
					static_cast<uint8_t>(hasDeltaMapping != manifest.delta_mapping_root_page_id.isValid()) |
					static_cast<uint8_t>(hasDeltaMapping != manifest.delta_mapping_root_generation.isValid()) |
					static_cast<uint8_t>(hasDeltaMapping != (manifest.delta_entry_count != 0U)) |
					static_cast<uint8_t>(hasAdjacencyMapping &&
										 !VectorAdjacencyMapping::isMappingIndexId(
													 manifest.adjacency_mapping_index_id)) |
					static_cast<uint8_t>(hasAdjacencyMapping != manifest.adjacency_generation.isValid()) |
					static_cast<uint8_t>(hasAdjacencyMapping != manifest.adjacency_mapping_root_page_id.isValid()) |
					static_cast<uint8_t>(hasAdjacencyMapping != manifest.adjacency_mapping_root_generation.isValid()) |
					static_cast<uint8_t>(hasAdjacencyMapping && manifest.entry_point_vector_id == 0U) |
					static_cast<uint8_t>(std::ranges::any_of(manifest.entries, [](const VectorManifestEntry &entry) {
						return entry.record_count > std::numeric_limits<uint16_t>::max();
					})) |
					static_cast<uint8_t>(manifest.entries.size() > RSS_VECTOR_MANIFEST_MAX_ENTRIES);
			if (invalid != 0U) {
				throwManifestError(FormatError::FERR_INVALID_HEADER,
								   "relocatable vector manifest has invalid identity, dimensions, or entry count");
			}
			std::pair<uint16_t, uint64_t> previous{};
			bool hasPrevious = false;
			for (const VectorManifestEntry &entry: manifest.entries) {
				const std::pair<uint16_t, uint64_t> current{static_cast<uint16_t>(entry.storage_kind),
															entry.page_id.value()};
				const uint8_t invalidEntry = static_cast<uint8_t>(!entry.page_id.isValid()) |
											 static_cast<uint8_t>(!isStorageKind(entry.storage_kind)) |
											 static_cast<uint8_t>(entry.record_count == 0U) |
											 static_cast<uint8_t>(hasPrevious && current <= previous);
				if (invalidEntry != 0U) {
					throwManifestError(FormatError::FERR_INVALID_HEADER,
									   "relocatable vector manifest entries are invalid or not strictly ordered");
				}
				previous = current;
				hasPrevious = true;
			}
		}

		void validateOwner(const PageOwnerHeader &owner, const VectorManifest &manifest, bool allowRetired) {
			const uint8_t invalid = static_cast<uint8_t>(owner.page_id != manifest.page_id) |
									static_cast<uint8_t>(owner.owner_id != manifest.index_id) |
									static_cast<uint8_t>(owner.kind != PageOwnerKind::POK_VECTOR_MANIFEST) |
									static_cast<uint8_t>((owner.flags & ~RSS_PAGE_OWNER_KNOWN_FLAGS) != 0U) |
									static_cast<uint8_t>(!allowRetired && owner.flags != 0U) |
									static_cast<uint8_t>(owner.payload_bytes != RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES) |
									static_cast<uint8_t>(owner.generation != manifest.generation) |
									static_cast<uint8_t>(owner.committed_lsn != manifest.committed_lsn);
			if (invalid != 0U) {
				throwManifestError(FormatError::FERR_INVALID_STATE,
								   "relocatable vector manifest owner disagrees with immutable manifest identity");
			}
		}

	} // namespace

	std::vector<std::byte> VectorManifestPageCodec::encode(const VectorManifest &manifest) {
		validateManifest(manifest);
		std::vector<std::byte> bytes(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES);
		LittleEndianCodec::writeU32(bytes, MAGIC_OFFSET, RSS_VECTOR_MANIFEST_MAGIC);
		LittleEndianCodec::writeU16(bytes, VERSION_OFFSET, RSS_VECTOR_MANIFEST_VERSION);
		LittleEndianCodec::writeU16(bytes, HEADER_BYTES_OFFSET, RSS_VECTOR_MANIFEST_HEADER_BYTES);
		LittleEndianCodec::writeU32(bytes, FLAGS_OFFSET, 0);
		LittleEndianCodec::writeU16(bytes, ENTRY_BYTES_OFFSET, RSS_VECTOR_MANIFEST_ENTRY_BYTES);
		LittleEndianCodec::writeU16(bytes, ENTRY_COUNT_OFFSET, static_cast<uint16_t>(manifest.entries.size()));
		LittleEndianCodec::writeU32(bytes, DIMENSIONS_OFFSET, manifest.dimensions);
		LittleEndianCodec::writeU16(bytes, METRIC_OFFSET, static_cast<uint16_t>(manifest.metric));
		LittleEndianCodec::writeU16(bytes, METRIC_RESERVED_OFFSET, 0);
		LittleEndianCodec::writeU64(bytes, INDEX_ID_OFFSET, manifest.index_id);
		LittleEndianCodec::writeU64(bytes, PAGE_ID_OFFSET, manifest.page_id.value());
		LittleEndianCodec::writeU64(bytes, GENERATION_OFFSET, manifest.generation.value());
		LittleEndianCodec::writeU64(bytes, LSN_OFFSET, manifest.committed_lsn.value());
		LittleEndianCodec::writeU64(bytes, CODEBOOK_ID_OFFSET, manifest.codebook_id);
		LittleEndianCodec::writeU64(bytes, CODEBOOK_GENERATION_OFFSET, manifest.codebook_generation.value());
		LittleEndianCodec::writeU64(bytes, ENTRY_POINT_VECTOR_ID_OFFSET, manifest.entry_point_vector_id);
		LittleEndianCodec::writeU64(bytes, ADJACENCY_GENERATION_OFFSET, manifest.adjacency_generation.value());
		LittleEndianCodec::writeU64(bytes, ADJACENCY_MAPPING_INDEX_ID_OFFSET, manifest.adjacency_mapping_index_id);
		LittleEndianCodec::writeU64(bytes, ADJACENCY_MAPPING_ROOT_PAGE_ID_OFFSET,
									manifest.adjacency_mapping_root_page_id.value());
		LittleEndianCodec::writeU64(bytes, ADJACENCY_MAPPING_ROOT_GENERATION_OFFSET,
									manifest.adjacency_mapping_root_generation.value());
		LittleEndianCodec::writeU64(bytes, NODE_MAPPING_ROOT_PAGE_ID_OFFSET,
									manifest.node_mapping_root_page_id.value());
		LittleEndianCodec::writeU64(bytes, NODE_MAPPING_ROOT_GENERATION_OFFSET,
									manifest.node_mapping_root_generation.value());
		LittleEndianCodec::writeU64(bytes, PQ_MAPPING_ROOT_PAGE_ID_OFFSET, manifest.pq_mapping_root_page_id.value());
		LittleEndianCodec::writeU64(bytes, PQ_MAPPING_ROOT_GENERATION_OFFSET,
									manifest.pq_mapping_root_generation.value());
		LittleEndianCodec::writeU64(bytes, PQ_MAPPING_INDEX_ID_OFFSET, manifest.pq_mapping_index_id);
		LittleEndianCodec::writeU64(bytes, RAW_WRITE_TAIL_PAGE_ID_OFFSET, manifest.raw_write_tail_page_id.value());
		LittleEndianCodec::writeU64(bytes, VECTOR_NODE_MAPPING_INDEX_ID_OFFSET,
									manifest.vector_node_mapping_index_id);
		LittleEndianCodec::writeU64(bytes, VECTOR_NODE_MAPPING_ROOT_PAGE_ID_OFFSET,
									manifest.vector_node_mapping_root_page_id.value());
		LittleEndianCodec::writeU64(bytes, VECTOR_NODE_MAPPING_ROOT_GENERATION_OFFSET,
									manifest.vector_node_mapping_root_generation.value());
		LittleEndianCodec::writeU64(bytes, DELTA_MAPPING_INDEX_ID_OFFSET, manifest.delta_mapping_index_id);
		LittleEndianCodec::writeU64(bytes, DELTA_MAPPING_ROOT_PAGE_ID_OFFSET,
									manifest.delta_mapping_root_page_id.value());
		LittleEndianCodec::writeU64(bytes, DELTA_MAPPING_ROOT_GENERATION_OFFSET,
									manifest.delta_mapping_root_generation.value());
		LittleEndianCodec::writeU32(bytes, DELTA_ENTRY_COUNT_OFFSET, manifest.delta_entry_count);
		LittleEndianCodec::writeU32(bytes, DELTA_RESERVED_OFFSET, 0U);
		for (size_t index = 0; index < manifest.entries.size(); ++index) {
			const size_t offset = RSS_VECTOR_MANIFEST_HEADER_BYTES + index * RSS_VECTOR_MANIFEST_ENTRY_BYTES;
			const VectorManifestEntry &entry = manifest.entries[index];
			LittleEndianCodec::writeU64(bytes, offset + ENTRY_PAGE_ID_OFFSET, entry.page_id.value());
			LittleEndianCodec::writeU16(bytes, offset + ENTRY_KIND_OFFSET, static_cast<uint16_t>(entry.storage_kind));
			LittleEndianCodec::writeU16(bytes, offset + ENTRY_RECORD_COUNT_OFFSET,
									static_cast<uint16_t>(entry.record_count));
		}
		LittleEndianCodec::writeU32(bytes, PAYLOAD_CRC_OFFSET,
									checksum(std::span(bytes).subspan(RSS_VECTOR_MANIFEST_HEADER_BYTES)));
		LittleEndianCodec::writeU32(bytes, HEADER_CRC_OFFSET, 0);
		LittleEndianCodec::writeU32(bytes, HEADER_CRC_OFFSET,
									checksum(std::span(bytes).first(RSS_VECTOR_MANIFEST_HEADER_BYTES)));
		return bytes;
	}

	VectorManifest VectorManifestPageCodec::decode(std::span<const std::byte> bytes) {
		if (bytes.size() < RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES) {
			throwManifestError(FormatError::FERR_BUFFER_TOO_SMALL, "relocatable vector manifest page is truncated");
		}
		if (bytes.size() != RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES) {
			throwManifestError(FormatError::FERR_INVALID_HEADER, "relocatable vector manifest page has an invalid size");
		}
		std::array<std::byte, RSS_VECTOR_MANIFEST_HEADER_BYTES> header{};
		std::copy_n(bytes.begin(), header.size(), header.begin());
		const uint32_t expectedHeaderCrc = LittleEndianCodec::readU32(header, HEADER_CRC_OFFSET);
		LittleEndianCodec::writeU32(header, HEADER_CRC_OFFSET, 0);
		if (checksum(header) != expectedHeaderCrc) {
			throwManifestError(FormatError::FERR_CHECKSUM_MISMATCH,
							   "relocatable vector manifest header checksum does not match");
		}
		const uint16_t entryCount = LittleEndianCodec::readU16(bytes, ENTRY_COUNT_OFFSET);
		const uint8_t invalidStatic =
				static_cast<uint8_t>(LittleEndianCodec::readU32(bytes, MAGIC_OFFSET) != RSS_VECTOR_MANIFEST_MAGIC) |
				static_cast<uint8_t>(LittleEndianCodec::readU16(bytes, VERSION_OFFSET) !=
									 RSS_VECTOR_MANIFEST_VERSION) |
				static_cast<uint8_t>(LittleEndianCodec::readU16(bytes, HEADER_BYTES_OFFSET) !=
									 RSS_VECTOR_MANIFEST_HEADER_BYTES) |
				static_cast<uint8_t>(LittleEndianCodec::readU32(bytes, FLAGS_OFFSET) != 0U) |
				static_cast<uint8_t>(LittleEndianCodec::readU16(bytes, ENTRY_BYTES_OFFSET) !=
									 RSS_VECTOR_MANIFEST_ENTRY_BYTES) |
					static_cast<uint8_t>(entryCount > RSS_VECTOR_MANIFEST_MAX_ENTRIES) |
					static_cast<uint8_t>(LittleEndianCodec::readU16(bytes, METRIC_RESERVED_OFFSET) != 0U) |
					static_cast<uint8_t>(LittleEndianCodec::readU32(bytes, DELTA_RESERVED_OFFSET) != 0U);
		if (invalidStatic != 0U) {
			throwManifestError(FormatError::FERR_INVALID_HEADER, "relocatable vector manifest header is invalid");
		}
		if (checksum(bytes.subspan(RSS_VECTOR_MANIFEST_HEADER_BYTES)) !=
			LittleEndianCodec::readU32(bytes, PAYLOAD_CRC_OFFSET)) {
			throwManifestError(FormatError::FERR_CHECKSUM_MISMATCH,
							   "relocatable vector manifest payload checksum does not match");
		}
		VectorManifest manifest;
		manifest.index_id = LittleEndianCodec::readU64(bytes, INDEX_ID_OFFSET);
		manifest.page_id = PageId(LittleEndianCodec::readU64(bytes, PAGE_ID_OFFSET));
		manifest.generation = Generation(LittleEndianCodec::readU64(bytes, GENERATION_OFFSET));
		manifest.committed_lsn = LogSequenceNumber(LittleEndianCodec::readU64(bytes, LSN_OFFSET));
		manifest.dimensions = LittleEndianCodec::readU32(bytes, DIMENSIONS_OFFSET);
		manifest.metric = VectorDistanceMetric(LittleEndianCodec::readU16(bytes, METRIC_OFFSET));
		manifest.codebook_id = LittleEndianCodec::readU64(bytes, CODEBOOK_ID_OFFSET);
		manifest.codebook_generation = Generation(LittleEndianCodec::readU64(bytes, CODEBOOK_GENERATION_OFFSET));
		manifest.entry_point_vector_id = LittleEndianCodec::readU64(bytes, ENTRY_POINT_VECTOR_ID_OFFSET);
		manifest.adjacency_generation = Generation(LittleEndianCodec::readU64(bytes, ADJACENCY_GENERATION_OFFSET));
		manifest.adjacency_mapping_index_id = LittleEndianCodec::readU64(bytes, ADJACENCY_MAPPING_INDEX_ID_OFFSET);
		manifest.adjacency_mapping_root_page_id =
				PageId(LittleEndianCodec::readU64(bytes, ADJACENCY_MAPPING_ROOT_PAGE_ID_OFFSET));
		manifest.adjacency_mapping_root_generation =
				Generation(LittleEndianCodec::readU64(bytes, ADJACENCY_MAPPING_ROOT_GENERATION_OFFSET));
		manifest.node_mapping_root_page_id =
				PageId(LittleEndianCodec::readU64(bytes, NODE_MAPPING_ROOT_PAGE_ID_OFFSET));
		manifest.node_mapping_root_generation =
				Generation(LittleEndianCodec::readU64(bytes, NODE_MAPPING_ROOT_GENERATION_OFFSET));
		manifest.pq_mapping_root_page_id = PageId(LittleEndianCodec::readU64(bytes, PQ_MAPPING_ROOT_PAGE_ID_OFFSET));
		manifest.pq_mapping_root_generation =
				Generation(LittleEndianCodec::readU64(bytes, PQ_MAPPING_ROOT_GENERATION_OFFSET));
		manifest.pq_mapping_index_id = LittleEndianCodec::readU64(bytes, PQ_MAPPING_INDEX_ID_OFFSET);
		manifest.raw_write_tail_page_id = PageId(LittleEndianCodec::readU64(bytes, RAW_WRITE_TAIL_PAGE_ID_OFFSET));
		manifest.vector_node_mapping_index_id = LittleEndianCodec::readU64(bytes, VECTOR_NODE_MAPPING_INDEX_ID_OFFSET);
		manifest.vector_node_mapping_root_page_id =
				PageId(LittleEndianCodec::readU64(bytes, VECTOR_NODE_MAPPING_ROOT_PAGE_ID_OFFSET));
		manifest.vector_node_mapping_root_generation =
				Generation(LittleEndianCodec::readU64(bytes, VECTOR_NODE_MAPPING_ROOT_GENERATION_OFFSET));
		manifest.delta_mapping_index_id = LittleEndianCodec::readU64(bytes, DELTA_MAPPING_INDEX_ID_OFFSET);
		manifest.delta_mapping_root_page_id =
				PageId(LittleEndianCodec::readU64(bytes, DELTA_MAPPING_ROOT_PAGE_ID_OFFSET));
		manifest.delta_mapping_root_generation =
				Generation(LittleEndianCodec::readU64(bytes, DELTA_MAPPING_ROOT_GENERATION_OFFSET));
		manifest.delta_entry_count = LittleEndianCodec::readU32(bytes, DELTA_ENTRY_COUNT_OFFSET);
		manifest.entries.reserve(entryCount);
		for (uint16_t index = 0; index < entryCount; ++index) {
			const size_t offset = RSS_VECTOR_MANIFEST_HEADER_BYTES + index * RSS_VECTOR_MANIFEST_ENTRY_BYTES;
			manifest.entries.push_back(
					{PageId(LittleEndianCodec::readU64(bytes, offset + ENTRY_PAGE_ID_OFFSET)),
					 VectorStorageKind(LittleEndianCodec::readU16(bytes, offset + ENTRY_KIND_OFFSET)),
					 LittleEndianCodec::readU16(bytes, offset + ENTRY_RECORD_COUNT_OFFSET)});
		}
		const size_t usedBytes = RSS_VECTOR_MANIFEST_HEADER_BYTES +
								 static_cast<size_t>(entryCount) * RSS_VECTOR_MANIFEST_ENTRY_BYTES;
		if (!std::all_of(bytes.begin() + usedBytes, bytes.end(),
						 [](std::byte value) { return value == std::byte{}; })) {
			throwManifestError(FormatError::FERR_RESERVED_BITS,
							   "relocatable vector manifest has nonzero trailing reserved bytes");
		}
		validateManifest(manifest);
		return manifest;
	}

	VectorManifestPage::VectorManifestPage(std::vector<std::byte> bytes, VectorManifest manifest) :
		bytes_(std::move(bytes)), manifest_(std::move(manifest)) {}

	VectorManifestPage VectorManifestPage::create(const PageOwnerHeader &owner, const VectorManifest &manifest) {
		validateManifest(manifest);
		validateOwner(owner, manifest, false);
		return VectorManifestPage(VectorManifestPageCodec::encode(manifest), manifest);
	}

	VectorManifestPage VectorManifestPage::open(const PageOwnerHeader &owner, std::span<const std::byte> bytes) {
		VectorManifest manifest = VectorManifestPageCodec::decode(bytes);
		// A pinned snapshot can legitimately resolve a retired physical frame.
		validateOwner(owner, manifest, true);
		return VectorManifestPage(std::vector<std::byte>(bytes.begin(), bytes.end()), std::move(manifest));
	}

	VectorManifestPage VectorManifestPage::rebindOwner(const PageOwnerHeader &owner) const {
		VectorManifest rebound = manifest_;
		rebound.generation = owner.generation;
		rebound.committed_lsn = owner.committed_lsn;
		return create(owner, rebound);
	}

} // namespace graph::storage
