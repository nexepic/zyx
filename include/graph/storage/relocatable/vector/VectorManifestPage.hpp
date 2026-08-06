/**
 * @file VectorManifestPage.hpp
 * @brief Immutable, checksummed vector-index manifest frame for relocatable.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "graph/storage/relocatable/vector/VectorCodebookExtent.hpp"
#include "graph/storage/relocatable/vector/VectorPage.hpp"

namespace graph::storage {

	constexpr uint32_t RSS_VECTOR_MANIFEST_MAGIC = 0x4D56595AU; // Little-endian bytes: ZYVM
	constexpr uint16_t RSS_VECTOR_MANIFEST_VERSION = 8;
	constexpr uint16_t RSS_VECTOR_MANIFEST_HEADER_BYTES = 224;
	// Page-record counts are bounded by one relocatable Vector page, so 16 bits are
	// sufficient and retain room for the durable reverse-mapping root.
	constexpr uint16_t RSS_VECTOR_MANIFEST_ENTRY_BYTES = 12;
	static_assert(RSS_VECTOR_MANIFEST_HEADER_BYTES <= RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES);
	constexpr uint16_t RSS_VECTOR_MANIFEST_MAX_ENTRIES =
			(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES - RSS_VECTOR_MANIFEST_HEADER_BYTES) /
			RSS_VECTOR_MANIFEST_ENTRY_BYTES;
	// Header evolution can leave a sub-entry reserved tail. It remains part of
	// the checksummed payload and the decoder requires every byte to be zero.
	constexpr size_t RSS_VECTOR_MANIFEST_TRAILING_PADDING_BYTES =
			RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES -
			(RSS_VECTOR_MANIFEST_HEADER_BYTES + RSS_VECTOR_MANIFEST_MAX_ENTRIES * RSS_VECTOR_MANIFEST_ENTRY_BYTES);
	static_assert(RSS_VECTOR_MANIFEST_TRAILING_PADDING_BYTES < RSS_VECTOR_MANIFEST_ENTRY_BYTES);

	struct VectorManifestEntry {
		PageId page_id{};
		VectorStorageKind storage_kind = VectorStorageKind::VSK_RAW;
		uint32_t record_count = 0;

		constexpr bool operator==(const VectorManifestEntry &) const = default;
	};

	// All enumerators use prefixes to remain safe in Windows builds.
	enum class VectorDistanceMetric : uint16_t {
		VDM_L2 = 1,
		VDM_COSINE = 2,
		VDM_INNER_PRODUCT = 3,
	};

	struct VectorManifest {
		uint64_t index_id = 0;
		PageId page_id{};
		Generation generation{};
		LogSequenceNumber committed_lsn{};
		uint32_t dimensions = 0;
		VectorDistanceMetric metric = VectorDistanceMetric::VDM_L2;
		// Both fields are zero for raw-only indexes. A populated pair identifies
		// the immutable codebook extent that this manifest generation requires.
		uint64_t codebook_id = 0;
		Generation codebook_generation{};
		uint64_t entry_point_vector_id = 0;
		// Derived DiskANN adjacency uses its own immutable VectorId-to-page
		// mapping tree, so a staged rebuild never overwrites a visible graph.
		Generation adjacency_generation{};
		uint64_t adjacency_mapping_index_id = 0;
		PageId adjacency_mapping_root_page_id{};
		Generation adjacency_mapping_root_generation{};
		// The Vector index owns an internal B+Tree with Node ID keys and
		// database-wide Vector ID values. Keeping its root identity in the
		// immutable manifest binds semantic lookup to this Vector generation.
		PageId node_mapping_root_page_id{};
		Generation node_mapping_root_generation{};
		// Candidate graph traversal starts with Vector IDs. This immutable,
		// metadata-owned B+Tree resolves those IDs back to logical Node IDs
		// without depending on movable Raw-page locations.
		uint64_t vector_node_mapping_index_id = 0;
		PageId vector_node_mapping_root_page_id{};
		Generation vector_node_mapping_root_generation{};
		// PQ records are an independently rebuildable acceleration cache. Their
		// mapping remains separate from raw Vector Location Directory entries so
		// exact semantic reads never depend on a derived record layout.
		// The mapping B+Tree has its own metadata-owned Index ID. Persisting that
		// identity prevents a damaged Manifest from binding a PQ cache to an
		// unrelated application index that happens to have the same root shape.
		uint64_t pq_mapping_index_id = 0;
		PageId pq_mapping_root_page_id{};
		Generation pq_mapping_root_generation{};
		// A bounded typed B+Tree records semantic mutations newer than the base
		// derived generation. Its entries either add an exact-refined candidate or
		// suppress a stale base candidate until maintenance folds the overlay.
		uint64_t delta_mapping_index_id = 0;
		PageId delta_mapping_root_page_id{};
		Generation delta_mapping_root_generation{};
		uint32_t delta_entry_count = 0U;
		// The current raw tail is an allocation hint, not an authoritative lookup
		// route. It lets online writers find the one mutable VectorPage in O(1)
		// through the Page Directory; raw Vector Location entries remain authoritative.
		PageId raw_write_tail_page_id{};
		std::vector<VectorManifestEntry> entries;

		constexpr bool operator==(const VectorManifest &) const = default;
	};

	class VectorManifestPageCodec final {
	public:
		[[nodiscard]] static std::vector<std::byte> encode(const VectorManifest &manifest);
		[[nodiscard]] static VectorManifest decode(std::span<const std::byte> bytes);
	};

	// Manifest payloads are immutable. A replacement keeps the logical Page ID
	// and switches its Page Directory location through COW, so a pinned snapshot
	// can always decode its former physical frame.
	class VectorManifestPage final {
	public:
		[[nodiscard]] static VectorManifestPage create(const PageOwnerHeader &owner, const VectorManifest &manifest);
		[[nodiscard]] static VectorManifestPage open(const PageOwnerHeader &owner, std::span<const std::byte> bytes);

		[[nodiscard]] const VectorManifest &manifest() const { return manifest_; }
		[[nodiscard]] const std::vector<std::byte> &serialize() const { return bytes_; }
		[[nodiscard]] VectorManifestPage rebindOwner(const PageOwnerHeader &owner) const;

	private:
		VectorManifestPage(std::vector<std::byte> bytes, VectorManifest manifest);

		std::vector<std::byte> bytes_;
		VectorManifest manifest_;
	};

} // namespace graph::storage
