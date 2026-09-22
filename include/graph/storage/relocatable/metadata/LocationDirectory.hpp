/**
 * @file LocationDirectory.hpp
 * @brief Copy-on-write Page and Extent logical-location directories.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "graph/storage/CancellationCheckpoint.hpp"
#include "graph/storage/relocatable/metadata/CatalogRadix.hpp"

namespace graph::storage {

	constexpr uint32_t RSS_LOCATION_LEAF_MAGIC = 0x4C58595AU; // Little-endian bytes: ZYXL
	// Version 3 is the native Entity Map encoding. Version 2 could encode the
	// retired fixed-record EntityLocation bridge under the same directory types,
	// so it must never be accepted by a version-25 database.
	constexpr uint16_t RSS_LOCATION_LEAF_VERSION = 3;
	constexpr uint32_t RSS_LOCATION_LEAF_HEADER_BYTES = 64;
	constexpr uint32_t RSS_LOCATION_ENTRY_BYTES = 32;
	constexpr uint32_t RSS_LOCATION_LEAF_MAX_ENTRIES =
			(RSS_PAGE_SIZE - RSS_LOCATION_LEAF_HEADER_BYTES) / RSS_LOCATION_ENTRY_BYTES;
	static_assert(RSS_LOCATION_LEAF_HEADER_BYTES + RSS_LOCATION_LEAF_MAX_ENTRIES * RSS_LOCATION_ENTRY_BYTES ==
				  RSS_PAGE_SIZE);

	struct LocationLeafEntry {
		uint64_t logical_id = 0;
		SegmentId segment_id{};
		Generation generation{};
		uint16_t first_page = 0;
		uint16_t page_count = 0;
		// The current contiguous free-byte count is meaningful only for Page
		// entries. It is the durable source for page-selection indexes.
		uint32_t free_bytes = 0;

		constexpr bool operator==(const LocationLeafEntry &) const = default;
	};

	struct LocationLeafPage {
		MetadataDirectoryType directory_type = MetadataDirectoryType::MDT_PAGE;
		MetadataPageId page_id{};
		Generation generation{};
		LogSequenceNumber committed_lsn{};
		std::vector<LocationLeafEntry> entries;
	};

	class LocationLeafPageCodec {
	public:
		using Bytes = std::array<std::byte, RSS_PAGE_SIZE>;

		[[nodiscard]] static Bytes encode(const LocationLeafPage &page);
		[[nodiscard]] static LocationLeafPage decode(std::span<const std::byte> bytes);
	};

	struct PageLocation {
		PageId page_id{};
		SegmentId segment_id{};
		uint16_t page_index = 0;
		Generation generation{};
		uint32_t free_bytes = 0;

		constexpr bool operator==(const PageLocation &) const = default;
	};

	struct ExtentLocation {
		ExtentId extent_id{};
		SegmentId segment_id{};
		uint16_t first_page = 0;
		uint16_t page_count = 0;
		Generation generation{};

		constexpr bool operator==(const ExtentLocation &) const = default;
	};

	// The native Entity Map owns logical identity. It deliberately resolves an
	// Entity ID through a stable Page ID and a generation-protected slot, never
	// through a physical pool Segment ID. Page relocation therefore changes only
	// the Page Directory, not every Entity Map entry referring to that page.
	struct EntityPageLocation {
		uint64_t entity_id = 0;
		PageId page_id{};
		uint16_t slot_id = 0;
		uint32_t slot_generation = 0;

		constexpr bool operator==(const EntityPageLocation &) const = default;
	};

	// Property collections belong directly to a logical graph entity. The
	// encoded directory ID uses the top bit for Edge and the remaining positive
	// 63-bit range for the Entity ID. Keeping this conversion private to the
	// storage type prevents query code from depending on physical directory keys.
	// All enum members carry a prefix to remain safe on Windows builds.
	enum class EntityPropertyOwnerKind : uint8_t {
		EPOK_NODE = 1,
		EPOK_EDGE = 2,
	};

	struct EntityPropertyKey {
		EntityPropertyOwnerKind owner_kind = EntityPropertyOwnerKind::EPOK_NODE;
		uint64_t entity_id = 0;

		[[nodiscard]] constexpr bool isValid() const {
			return entity_id != 0U && entity_id <= ((UINT64_C(1) << 63U) - 1U) &&
				   (owner_kind == EntityPropertyOwnerKind::EPOK_NODE ||
					owner_kind == EntityPropertyOwnerKind::EPOK_EDGE);
		}
		[[nodiscard]] uint64_t directoryId() const;
		[[nodiscard]] static EntityPropertyKey fromDirectoryId(uint64_t directoryId);

		constexpr auto operator<=>(const EntityPropertyKey &) const = default;
	};

	struct EntityPropertyLocation {
		EntityPropertyKey key;
		PageId page_id{};
		uint16_t slot_id = 0;
		uint32_t slot_generation = 0;

		constexpr bool operator==(const EntityPropertyLocation &) const = default;
	};

	// State records use a separate logical namespace from graph Property roots.
	struct StateLocation {
		uint64_t state_id = 0;
		PageId page_id{};
		uint16_t slot_id = 0;
		uint32_t slot_generation = 0;

		constexpr bool operator==(const StateLocation &) const = default;
	};

	// Maps a stable Index ID to the logical Page ID of its current B+Tree root.
	// Root-page generation is carried explicitly so publication rejects a root
	// that no longer resolves to the same index-pool generation.
	struct IndexRootLocation {
		uint64_t index_id = 0;
		PageId root_page_id{};
		Generation root_generation{};

		constexpr bool operator==(const IndexRootLocation &) const = default;
	};

	// Maps the database-wide stable Vector ID to one reusable record slot in a
	// raw or PQ VectorPage. Vector records deliberately do not derive identity
	// from Node ID or Index ID, so relocation changes only this mapping.
	struct VectorLocation {
		uint64_t vector_id = 0;
		PageId page_id{};
		uint16_t slot_id = 0;
		uint32_t slot_generation = 0;

		constexpr bool operator==(const VectorLocation &) const = default;
	};

	// Maps an Index ID to the Page ID containing its immutable vector manifest.
	// The manifest page codec is intentionally separate from raw/PQ vector
	// frames; this directory only persists its stable logical reference.
	struct VectorManifestLocation {
		uint64_t index_id = 0;
		PageId manifest_page_id{};
		Generation generation{};

		constexpr bool operator==(const VectorManifestLocation &) const = default;
	};

	// Maps a durable LargeValue identity to its immutable manifest page. Its
	// generation is the manifest frame's physical pool generation, not the
	// LargeValue logical generation embedded in the immutable reference. The
	// manifest stores only logical data Page IDs, so data-page relocation changes
	// only the Page Directory while manifest relocation rebinds this route.
	struct LargeValueManifestLocation {
		LargeValueId value_id{};
		PageId manifest_page_id{};
		Generation generation{};

		constexpr bool operator==(const LargeValueManifestLocation &) const = default;
	};

	// A retired slot is keyed by its physical identity. The queue keeps the
	// logical Segment ownership and the publication generation that made the
	// old slot unreachable, so reclamation can release it only after snapshots
	// older than that generation have gone away.
	enum class RetireQueueDisposition : uint16_t {
		RQD_RELOCATED_SOURCE = 1,
		RQD_RETIRED_SEGMENT = 2,
	};

	struct RetireQueueEntry {
		PhysicalSlotId physical_slot{};
		SegmentId segment_id{};
		Generation retired_generation{};
		RetireQueueDisposition disposition = RetireQueueDisposition::RQD_RELOCATED_SOURCE;

		constexpr bool operator==(const RetireQueueEntry &) const = default;
	};

	class MetadataWriteBatch;
	class MetadataPageFile;
	class MetadataPageAllocator;
	class LocationDirectory;
	struct Superblock;

	// A forward-only logical range over one immutable directory snapshot. The
	// cursor retains only the current leaf and one radix route, so continuation
	// never restarts at the root and memory remains bounded by format depth.
	class LocationDirectoryRangeCursor final {
	public:
		LocationDirectoryRangeCursor(const LocationDirectoryRangeCursor &) = delete;
		LocationDirectoryRangeCursor &operator=(const LocationDirectoryRangeCursor &) = delete;
		LocationDirectoryRangeCursor(LocationDirectoryRangeCursor &&) noexcept;
		LocationDirectoryRangeCursor &operator=(LocationDirectoryRangeCursor &&) noexcept;
		~LocationDirectoryRangeCursor();

		[[nodiscard]] std::optional<LocationLeafEntry> next();
		// Skips directly to the first entry at or after `minimumLogicalId` without
		// rereading the root. Calls must be monotonic for an already-consumed cursor.
		[[nodiscard]] std::optional<LocationLeafEntry> nextAtOrAfter(uint64_t minimumLogicalId);
		[[nodiscard]] size_t metadataPagesRead() const noexcept;
		[[nodiscard]] bool exhausted() const noexcept;

	private:
		struct Impl;
		explicit LocationDirectoryRangeCursor(std::unique_ptr<Impl> implementation) noexcept;

		std::unique_ptr<Impl> implementation_;

		friend class LocationDirectory;
	};

	// Owns one bounded append-only path for a streamed logical-location
	// directory. It is deliberately narrow: only new contiguous IDs or explicitly
	// sparse monotonic Edge/Edge-Property IDs can be appended, which keeps
	// streamed publication memory bounded.
	class LocationDirectoryAppendSession final {
	public:
		LocationDirectoryAppendSession(LocationDirectoryAppendSession &&) noexcept;
		LocationDirectoryAppendSession &operator=(LocationDirectoryAppendSession &&) noexcept;
		~LocationDirectoryAppendSession();

		LocationDirectoryAppendSession(const LocationDirectoryAppendSession &) = delete;
		LocationDirectoryAppendSession &operator=(const LocationDirectoryAppendSession &) = delete;

		void append(const PageLocation &location);
		void append(const EntityPageLocation &location);
		void append(const EntityPropertyLocation &location);
		void append(const LargeValueManifestLocation &location);
		void finish();
		[[nodiscard]] bool isFinished() const noexcept;

	private:
		struct Impl;
		explicit LocationDirectoryAppendSession(std::unique_ptr<Impl> implementation) noexcept;

		std::unique_ptr<Impl> implementation_;

		friend class LocationDirectory;
	};

	class LocationDirectory final {
	public:
		// A streamed publisher assigns Page IDs monotonically before exposing the
		// corresponding pool frames.  This constrained append path keeps the Page
		// Directory COW state bounded by one leaf and one radix path.
		using PageLocationCursor = std::function<std::optional<PageLocation>()>;
		using PageLocationSink = std::function<void(const PageLocation &)>;
		using PageLocationProducer = std::function<void(const PageLocationSink &)>;
		[[nodiscard]] LocationDirectoryAppendSession beginAppendPageLocations(MetadataWriteBatch &batch,
																			  uint64_t firstPageId, uint64_t lastPageId,
																			  size_t expectedCount);
		void prepareAppendPageLocations(MetadataWriteBatch &batch, uint64_t firstPageId, uint64_t lastPageId,
										size_t expectedCount, const PageLocationCursor &cursor);
		void prepareAppendPageLocations(MetadataWriteBatch &batch, uint64_t firstPageId, uint64_t lastPageId,
										size_t expectedCount, const PageLocationProducer &producer);

		// A streamed Edge publication owns ordered, never-before-visible logical-ID
		// lease ranges. Interleaved writers may leave gaps between ranges, so this
		// path accepts strictly increasing IDs inside one declared lower/upper
		// bound. It retains only one directory leaf and one mutable radix path; it
		// is not a general replacement API, whose arbitrary key order requires a
		// wider COW working set.
		using EdgePageLocationCursor = std::function<std::optional<EntityPageLocation>()>;
		using EdgePageLocationSink = std::function<void(const EntityPageLocation &)>;
		// The push form lets a writer materialize one typed Entity page, persist it,
		// and immediately transfer its directory mappings into this bounded COW
		// builder. It avoids an import-sized mapping vector at the metadata boundary.
		using EdgePageLocationProducer = std::function<void(const EdgePageLocationSink &)>;
		[[nodiscard]] LocationDirectoryAppendSession beginAppendEdgePageLocations(MetadataWriteBatch &batch,
																				  uint64_t firstEdgeId,
																				  uint64_t lastEdgeId,
																				  size_t expectedCount);
		// Edge property roots are sparse because edges without Properties do not
		// consume a directory record. The session still keeps only one leaf and
		// one radix path, but accepts strictly increasing rather than contiguous
		// logical keys.
		[[nodiscard]] LocationDirectoryAppendSession beginAppendEdgePropertyLocations(MetadataWriteBatch &batch,
																					  uint64_t firstEdgeId,
																					  uint64_t lastEdgeId,
																					  size_t expectedCount);
		// New LargeValue IDs are allocated from the writer-fenced monotonic
		// counter. Appending their Manifest routes accepts strictly increasing IDs
		// because streamed content dedup may reserve a provisional range while
		// materializing only its canonical subset.
		[[nodiscard]] LocationDirectoryAppendSession beginAppendLargeValueManifestLocations(MetadataWriteBatch &batch,
																							LargeValueId firstValueId,
																							LargeValueId lastValueId,
																							size_t expectedCount);
		void prepareAppendEdgePageLocations(MetadataWriteBatch &batch, uint64_t firstEdgeId, uint64_t lastEdgeId,
											size_t expectedCount, const EdgePageLocationCursor &cursor);
		void prepareAppendEdgePageLocations(MetadataWriteBatch &batch, uint64_t firstEdgeId, uint64_t lastEdgeId,
											size_t expectedCount, const EdgePageLocationProducer &producer);
		LocationDirectory(MetadataPageFile &pages, MetadataPageAllocator &allocator);

		void prepareUpsertPages(MetadataWriteBatch &batch, std::span<const PageLocation> entries);
		void prepareReplacePages(MetadataWriteBatch &batch, std::span<const PageLocation> entries,
								 std::span<const uint64_t> erasedPageIds);
		void prepareUpsertExtents(MetadataWriteBatch &batch, std::span<const ExtentLocation> entries);
		void prepareReplaceExtents(MetadataWriteBatch &batch, std::span<const ExtentLocation> entries,
								   std::span<const uint64_t> erasedExtentIds);
		void prepareReplaceNodePageLocations(MetadataWriteBatch &batch, std::span<const EntityPageLocation> entries,
											 std::span<const uint64_t> erasedEntityIds);
		void prepareReplaceEdgePageLocations(MetadataWriteBatch &batch, std::span<const EntityPageLocation> entries,
											 std::span<const uint64_t> erasedEntityIds);
		void prepareReplaceEntityProperties(MetadataWriteBatch &batch, std::span<const EntityPropertyLocation> entries,
											std::span<const EntityPropertyKey> erasedKeys);
		void prepareReplaceStates(MetadataWriteBatch &batch, std::span<const StateLocation> entries,
								  std::span<const uint64_t> erasedStateIds);
		void prepareReplaceIndexRoots(MetadataWriteBatch &batch, std::span<const IndexRootLocation> entries,
									  std::span<const uint64_t> erasedIndexIds);
		void prepareReplaceVectorLocations(MetadataWriteBatch &batch, std::span<const VectorLocation> entries,
										   std::span<const uint64_t> erasedVectorIds);
		void prepareReplaceVectorManifests(MetadataWriteBatch &batch, std::span<const VectorManifestLocation> entries,
										   std::span<const uint64_t> erasedIndexIds);
		void prepareReplaceLargeValueManifests(MetadataWriteBatch &batch,
											   std::span<const LargeValueManifestLocation> entries,
											   std::span<const LargeValueId> erasedValueIds);
		void prepareReplaceRetireQueue(MetadataWriteBatch &batch, std::span<const RetireQueueEntry> entries,
									   std::span<const PhysicalSlotId> erasedSlots);
		// Candidate location roots stay physical while same-batch writers compose.
		// The publication gate logicalizes every typed non-bootstrap root afterward.
		void prepareLogicalRootChildren(MetadataWriteBatch &batch);

		[[nodiscard]] LocationDirectoryRangeCursor openRangeCursor(
				const Superblock &superblock, MetadataDirectoryType directoryType, uint64_t lowerInclusive = 1U,
				uint64_t upperInclusive = std::numeric_limits<uint64_t>::max(),
				const CancellationCheckpoint *cancellation = nullptr) const;

		[[nodiscard]] PageLocation findPage(const Superblock &superblock, PageId pageId) const;
		[[nodiscard]] ExtentLocation findExtent(const Superblock &superblock, ExtentId extentId) const;
		[[nodiscard]] std::optional<PageLocation> findPageOptional(const Superblock &superblock, PageId pageId) const;
		[[nodiscard]] std::optional<ExtentLocation> findExtentOptional(const Superblock &superblock,
																	   ExtentId extentId) const;
		[[nodiscard]] EntityPageLocation findNodePageLocation(const Superblock &superblock, uint64_t nodeId) const;
		[[nodiscard]] EntityPageLocation findEdgePageLocation(const Superblock &superblock, uint64_t edgeId) const;
		[[nodiscard]] std::optional<EntityPageLocation> findNodePageLocationOptional(const Superblock &superblock,
																					 uint64_t nodeId) const;
		[[nodiscard]] std::optional<EntityPageLocation> findEdgePageLocationOptional(const Superblock &superblock,
																					 uint64_t edgeId) const;
		// Resolves one strictly increasing Entity-ID batch through a single
		// directory range cursor. Results preserve input order and leave absent
		// logical IDs empty.
		void findEntityPageLocationsSorted(const Superblock &superblock, MetadataDirectoryType directoryType,
																std::span<const uint64_t> entityIds,
																std::span<std::optional<EntityPageLocation>> results,
																const CancellationCheckpoint *cancellation = nullptr) const;
		[[nodiscard]] EntityPropertyLocation findEntityProperty(const Superblock &superblock,
																const EntityPropertyKey &key) const;
		[[nodiscard]] std::optional<EntityPropertyLocation>
		findEntityPropertyOptional(const Superblock &superblock, const EntityPropertyKey &key) const;
		// Resolves one caller-bounded, strictly ordered key batch with a single
		// retained radix traversal. Results preserve input order and distinguish a
		// missing owner from malformed directory state.
		void findEntityPropertiesSorted(
				const Superblock &superblock, std::span<const EntityPropertyKey> keys,
				std::span<std::optional<EntityPropertyLocation>> results,
				const CancellationCheckpoint *cancellation = nullptr) const;
		[[nodiscard]] StateLocation findState(const Superblock &superblock, uint64_t stateId) const;
		[[nodiscard]] std::optional<StateLocation> findStateOptional(const Superblock &superblock,
																	 uint64_t stateId) const;
		[[nodiscard]] IndexRootLocation findIndexRoot(const Superblock &superblock, uint64_t indexId) const;
		[[nodiscard]] std::optional<IndexRootLocation> findIndexRootOptional(const Superblock &superblock,
																			 uint64_t indexId) const;
		[[nodiscard]] VectorLocation findVectorLocation(const Superblock &superblock, uint64_t vectorId) const;
		[[nodiscard]] std::optional<VectorLocation> findVectorLocationOptional(const Superblock &superblock,
																			   uint64_t vectorId) const;
		[[nodiscard]] VectorManifestLocation findVectorManifest(const Superblock &superblock, uint64_t indexId) const;
		[[nodiscard]] std::optional<VectorManifestLocation> findVectorManifestOptional(const Superblock &superblock,
																					   uint64_t indexId) const;
		[[nodiscard]] LargeValueManifestLocation findLargeValueManifest(const Superblock &superblock,
																		LargeValueId valueId) const;
		[[nodiscard]] std::optional<LargeValueManifestLocation>
		findLargeValueManifestOptional(const Superblock &superblock, LargeValueId valueId) const;
		[[nodiscard]] std::optional<RetireQueueEntry> findRetireQueueEntryOptional(const Superblock &superblock,
																				   PhysicalSlotId slot) const;
		// Proves that a planned contiguous entity-ID range has no published
		// location. Bulk creation uses this once per segment plan instead of doing
		// one root-to-leaf lookup for every new record.
		void requireNodeRangeAbsent(const Superblock &superblock, uint64_t firstNodeId, uint32_t count) const;
		void requireEdgeRangeAbsent(const Superblock &superblock, uint64_t firstEdgeId, uint32_t count) const;
		void visitPages(const Superblock &superblock, const std::function<void(const PageLocation &)> &visitor) const;
		[[nodiscard]] bool visitPagesUntil(const Superblock &superblock,
										   const std::function<bool(const PageLocation &)> &visitor) const;
		void visitNodePageLocations(const Superblock &superblock,
									const std::function<void(const EntityPageLocation &)> &visitor) const;
		void visitEdgePageLocations(const Superblock &superblock,
									const std::function<void(const EntityPageLocation &)> &visitor) const;
		[[nodiscard]] bool
		visitNodePageLocationsUntil(const Superblock &superblock,
									const std::function<bool(const EntityPageLocation &)> &visitor) const;
		[[nodiscard]] bool
		visitEdgePageLocationsUntil(const Superblock &superblock,
									const std::function<bool(const EntityPageLocation &)> &visitor) const;
		[[nodiscard]] bool visitNodePageLocationsAfter(const Superblock &superblock, uint64_t afterNodeId,
													   size_t maximumMetadataPages,
													   const std::function<bool(const EntityPageLocation &)> &visitor,
													   size_t *visitedMetadataPages = nullptr) const;
		void visitEntityProperties(const Superblock &superblock,
								   const std::function<void(const EntityPropertyLocation &)> &visitor) const;
		// The cursor is the stable directory identity, not a page or slot that a
		// relocation could replace while a maintenance pass is paused.
		[[nodiscard]] bool
		visitEntityPropertiesAfter(const Superblock &superblock, uint64_t afterDirectoryId, size_t maximumMetadataPages,
								   const std::function<bool(const EntityPropertyLocation &)> &visitor,
								   size_t *visitedMetadataPages = nullptr) const;
		void visitStates(const Superblock &superblock, const std::function<void(const StateLocation &)> &visitor) const;
		[[nodiscard]] bool visitStatesAfter(const Superblock &superblock, uint64_t afterStateId,
											size_t maximumMetadataPages,
											const std::function<bool(const StateLocation &)> &visitor,
											size_t *visitedMetadataPages = nullptr) const;
		void visitExtents(const Superblock &superblock,
						  const std::function<void(const ExtentLocation &)> &visitor) const;
		void visitIndexRoots(const Superblock &superblock,
							 const std::function<void(const IndexRootLocation &)> &visitor) const;
		void visitVectorLocations(const Superblock &superblock,
								  const std::function<void(const VectorLocation &)> &visitor) const;
		void visitVectorManifests(const Superblock &superblock,
								  const std::function<void(const VectorManifestLocation &)> &visitor) const;
		// Resumes Vector Manifest traversal after one logical Index ID. The caller
		// supplies a metadata-page budget so durable maintenance can rediscover
		// rebuild debt without an unbounded directory walk.
		[[nodiscard]] bool visitVectorManifestsAfter(const Superblock &superblock, uint64_t afterIndexId,
													 size_t maximumMetadataPages,
													 const std::function<bool(const VectorManifestLocation &)> &visitor,
													 size_t *visitedMetadataPages = nullptr) const;
		void visitLargeValueManifests(const Superblock &superblock,
									  const std::function<void(const LargeValueManifestLocation &)> &visitor) const;
		// Resumes by stable logical value ID so a maintenance cursor never relies on
		// a physical page location that relocation may replace.
		[[nodiscard]] bool
		visitLargeValueManifestsAfter(const Superblock &superblock, uint64_t afterValueId, size_t maximumMetadataPages,
									  const std::function<bool(const LargeValueManifestLocation &)> &visitor,
									  size_t *visitedMetadataPages = nullptr) const;
		void visitRetireQueue(const Superblock &superblock,
							  const std::function<void(const RetireQueueEntry &)> &visitor) const;

	private:
		using LocationEntrySink = std::function<void(const LocationLeafEntry &)>;
		using LocationEntryProducer = std::function<void(const LocationEntrySink &)>;
		void prepareAppendLocations(MetadataWriteBatch &batch, MetadataDirectoryType directoryType,
									uint64_t firstLogicalId, uint64_t lastLogicalId, size_t expectedCount,
									const LocationEntryProducer &producer);
		void prepareUpsert(MetadataWriteBatch &batch, MetadataDirectoryType directoryType,
						   std::span<const LocationLeafEntry> entries);
		void prepareReplace(MetadataWriteBatch &batch, MetadataDirectoryType directoryType,
							std::span<const LocationLeafEntry> entries, std::span<const uint64_t> erasedLogicalIds);
		[[nodiscard]] LocationLeafEntry find(const Superblock &superblock, MetadataDirectoryType directoryType,
											 uint64_t logicalId) const;
		[[nodiscard]] std::optional<LocationLeafEntry>
		findOptional(const Superblock &superblock, MetadataDirectoryType directoryType, uint64_t logicalId) const;
		void requireRangeAbsent(const Superblock &superblock, MetadataDirectoryType directoryType,
								uint64_t firstLogicalId, uint32_t count) const;
		void visit(const Superblock &superblock, MetadataDirectoryType directoryType,
				   const std::function<void(const LocationLeafEntry &)> &visitor) const;
		[[nodiscard]] bool visitUntil(const Superblock &superblock, MetadataDirectoryType directoryType,
									  const std::function<bool(const LocationLeafEntry &)> &visitor) const;
		[[nodiscard]] bool visitAfter(const Superblock &superblock, MetadataDirectoryType directoryType,
									  uint64_t afterLogicalId, size_t maximumMetadataPages,
									  const std::function<bool(const LocationLeafEntry &)> &visitor,
									  size_t *visitedMetadataPages) const;

		MetadataPageFile &pages_;
		MetadataPageAllocator &allocator_;
	};

} // namespace graph::storage
