/**
 * @file PropertyCollectionRootStore.hpp
 * @brief Durable placement and lookup for relocatable Property collection roots.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <vector>
#include "graph/storage/PropertyScalarReadSink.hpp"

#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/metadata/Catalog.hpp"
#include "graph/storage/relocatable/metadata/LocationDirectory.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRoot.hpp"

namespace graph::storage {
	struct HybridPropertyScanContext;
	class LargeValueStore;
	class PropertyCollectionRootScanCursor;
	class PropertyCollectionRootStoreTestAccess;

	struct PropertyCollectionRootStoreOptions {
		PhysicalSlotId first_slot{};
		PhysicalSlotId last_slot{};
		// This stable namespace prevents root pages from sharing frames with a
		// future Property subtype that has different record semantics.
		uint64_t page_owner_id = 0;
		LargeValueCompressionOptions large_value_compression{};
		size_t maximum_large_value_dedup_candidates = RSS_LARGE_VALUE_DEFAULT_MAX_DEDUP_CANDIDATES;
		bool adaptive_row_chunks = false;
	};

	// All enum members carry a prefix to remain safe on Windows builds.
	enum class EntityPropertyRootMutationPlanKind : uint8_t {
		EPRMPK_NO_CHANGE = 1,
		EPRMPK_EXISTING_PAGE = 2,
		EPRMPK_NEW_PAGE_REQUIRED = 3,
	};

	// A snapshot-pinned owner-keyed Property mutation. Existing-page plans carry
	// the complete replacement payload but remain unreachable until the native
	// graph transaction prepares their frame and publishes its metadata batch.
	// New-pool plans retain only the canonical encoded root because physical
	// identity reservation belongs to a later compound allocator operation.
	struct EntityPropertyRootMutationPlan {
		EntityPropertyRootMutationPlanKind kind = EntityPropertyRootMutationPlanKind::EPRMPK_NO_CHANGE;
		EntityPropertyKey key;
		std::optional<NativeGraphPropertyPageReplacementRequest> page_replacement;
		std::vector<std::byte> replacement_payload;
		uint32_t replacement_free_bytes = 0U;
		std::vector<std::byte> encoded_root;
		// One page-local COW plan may pack several owner-keyed roots. Each matching
		// immutable value is materialized in the same generation as its directory
		// entry, so a reader can never observe a dangling LargeValue reference.
		std::vector<NativeGraphLargeValueMutation> large_values;
		// Snapshot-verified immutable references may be reused without creating a
		// new LargeValue frame. They remain separate from `large_values` so the
		// metadata writer can reserve logical IDs only for new values.
		std::vector<LargeValueRef> reused_large_values;
	};

	// Per-batch snapshot caches avoid reopening an immutable Property page while
	// independently planning many roots that target that same page.
	struct EntityPropertyBatchPageCache {
		std::map<PageId, PageLocation> locations;
		std::map<PageId, CatalogEntry> pools;
		std::map<PageId, std::optional<SlottedPage>> pages;
	};

	class PropertyCollectionRootStore final {
	public:
		PropertyCollectionRootStore(SlottedPagePool &pages, RelocatableMetadataStore &metadata);
		// Enables canonical LargeValue-backed Entity Property roots. The referenced
		// value remains logical and is resolved against the caller's snapshot.
		PropertyCollectionRootStore(SlottedPagePool &pages, LargeValueStore &largeValues,
									RelocatableMetadataStore &metadata);
		[[nodiscard]] bool usesMetadataStore(const RelocatableMetadataStore &metadata) const {
			return &metadata_ == &metadata;
		}
		[[nodiscard]] LargeValueStore *largeValueStore() const { return large_values_; }

        // Layout/liveness probe for explicit candidate transactions. A live
        // propertyless owner can be hybrid; a missing/ROW owner returns false.
        // A group can cover an absent owner. Probe that boundary before ROW
        // placement; a group hit alone grants no Entity/liveness write authority.
        [[nodiscard]] bool hasHybridPropertyGroup(const MetadataSnapshotPin &snapshot, EntityPropertyKey key,
            const CancellationCheckpoint *cancellation = nullptr,
            const SelectedPropertyReadAdmission *transientAdmission = nullptr) const;
        [[nodiscard]] bool isHybridPropertyOwner(const MetadataSnapshotPin &snapshot, EntityPropertyKey key,
            const CancellationCheckpoint *cancellation = nullptr, const SelectedPropertyReadAdmission *transientAdmission = nullptr) const;
		// Canonical Stage 5 lookup. The Entity Property Directory is keyed by the
		// owning logical entity, so no Property-ID reservation or entity-record
		// reference is needed on this path.
		[[nodiscard]] PropertyCollectionRoot readEntity(const MetadataSnapshotPin &snapshot,
														const EntityPropertyKey &key, const SelectedPropertyReadAdmission *admission = nullptr,
				const SelectedPropertyReadAdmission *slotsAdmission = nullptr) const;
		// Optional point lookup keeps normal Buffer Pool admission while avoiding a
		// separate directory probe before decoding a present owner.
		[[nodiscard]] std::optional<PropertyCollectionRoot> readEntityOptional(const MetadataSnapshotPin &snapshot,
																	   const EntityPropertyKey &key, const SelectedPropertyReadAdmission *admission = nullptr, const SelectedPropertyReadAdmission *slotsAdmission = nullptr) const;
		// Reads one Property value without materializing the owner's complete
		// collection. A missing owner and a missing token both return nullopt;
		// malformed directory, page, or payload state still fails closed.
		[[nodiscard]] std::optional<PropertyCollectionRootEntry>
		readEntityPropertyOptional(const MetadataSnapshotPin &snapshot, const EntityPropertyKey &key,
																							 uint32_t keyToken) const;
		[[nodiscard]] std::optional<bool> readEntityPropertyExists(const MetadataSnapshotPin &snapshot,
																							 const EntityPropertyKey &key, uint32_t keyToken, bool requireNonNull = false) const;
		// Resolves one selected Property for strictly directory-ordered owners in a
		// single metadata merge, then reads each distinct Property page once. Missing
		// owners and missing tokens remain nullopt; malformed locations fail closed.
		void readEntityPropertiesOptionalSorted(
				const MetadataSnapshotPin &snapshot, std::span<const EntityPropertyKey> keys, uint32_t keyToken,
				std::span<std::optional<PropertyCollectionRootEntry>> results,
				const CancellationCheckpoint *cancellation = nullptr) const;
		// Resolves several strictly increasing tokens for strictly directory-ordered
		// owners. Results are owner-major: result[owner * token_count + token].
		// One directory merge and one Property-page pin serve every requested column.
		void readEntityPropertiesOptionalSorted(
				const MetadataSnapshotPin &snapshot, std::span<const EntityPropertyKey> keys,
				std::span<const uint32_t> keyTokens,
				std::span<std::optional<PropertyCollectionRootEntry>> results,
				const CancellationCheckpoint *cancellation = nullptr) const;
		// Resolves selected values and root entry counts from one ordered owner
		// directory merge. Index candidate verification needs both facts from the
		// same authenticated root and must not perform two independent lookups.
		// transientBufferAdmission reports absolute owner-array and LargeValue read buffers.
		// Caller-owned output, page copies, and manifest metadata require separate budgets.
		void readEntityPropertiesAndCountsOptionalSorted(
				const MetadataSnapshotPin &snapshot, std::span<const EntityPropertyKey> keys,
				std::span<const uint32_t> keyTokens,
				std::span<std::optional<PropertyCollectionRootEntry>> results,
				std::span<std::optional<size_t>> counts,
				const CancellationCheckpoint *cancellation = nullptr,
				const SelectedPropertyReadAdmission *admission = nullptr,
				const SelectedPropertyReadAdmission *transientBufferAdmission = nullptr) const;
		// Reads only the authenticated root framing needed to enforce caller row
		// bounds. It never materializes the Property value collection.
		[[nodiscard]] std::optional<size_t> readEntityPropertyCount(const MetadataSnapshotPin &snapshot,
																												const EntityPropertyKey &key) const;
		// Enumerates authenticated field tokens without decoding values. Missing
		// property roots produce an empty list. Admissions cover output capacity
		// and transient LargeValue directory buffers separately.
		// Token admission reports absolute requested capacity and actual capacity;
		// transient admission reports its absolute retained buffers independently.
		[[nodiscard]] std::vector<uint32_t> readEntityPropertyTokens(
				const MetadataSnapshotPin &snapshot, const EntityPropertyKey &key,
				const SelectedPropertyReadAdmission &tokenAdmission,
				const SelectedPropertyReadAdmission &transientAdmission,
				const CancellationCheckpoint *cancellation = nullptr) const;
		// Resolves strictly ordered owners in one directory merge and reads each
		// distinct Property page at most once. Missing owners report zero, matching
		// the point-count contract; malformed locations still fail closed.
		void readEntityPropertyCountsSorted(const MetadataSnapshotPin &snapshot,
																std::span<const EntityPropertyKey> keys,
																std::span<std::optional<size_t>> results,
																const CancellationCheckpoint *cancellation = nullptr) const;
		// A logical Entity/Property scan reuses the currently decoded slotted page
		// while admitting its owned frame as sequential. Point callers keep using
		// readEntity(), whose normal admission preserves shared-page locality.
		[[nodiscard]] PropertyCollectionRootScanCursor openScanCursor(const MetadataSnapshotPin &snapshot) const;
		[[nodiscard]] PropertyCollectionRootScanCursor
		openScanCursor(const MetadataSnapshotPin &snapshot, const CancellationCheckpoint *cancellation) const;
		// Builds an owner-keyed mutation against exactly `snapshot`. It never
		// publishes metadata. Empty roots remove an existing root; a missing root
		// is a no-op. A nonempty root that cannot fit a current Property page
		// returns `EPRMPK_NEW_PAGE_REQUIRED` for the native graph writer to pack.
		[[nodiscard]] EntityPropertyRootMutationPlan
		planEntityRootReplacement(const MetadataSnapshotPin &snapshot, const PropertyCollectionRoot &root,
								  const PropertyCollectionRootStoreOptions &options) const;
		// Plans a deterministic batch from one snapshot. LargeValue IDs are
		// assigned only to roots that actually overflow and are consecutive from
		// the pinned Superblock high-water mark.
		[[nodiscard]] std::vector<EntityPropertyRootMutationPlan>
		planEntityRootReplacements(const MetadataSnapshotPin &snapshot, std::span<const PropertyCollectionRoot> roots,
								   const PropertyCollectionRootStoreOptions &options) const;
		// Makes a planned existing-page payload durable while the caller owns the
		// native graph metadata writer. It cannot publish a directory independently.
		void preparePlannedEntityRootReplacement(const EntityPropertyRootMutationPlan &plan,
												 NativeGraphPropertyPageReplacementMaterialization &materialization,
												 LogSequenceNumber checkpointLsn) const;
		// Materializes the immutable bytes for a planned LargeValue without
		// publishing its manifest directory. The caller must invoke this while the
		// native graph metadata writer owns the enclosing publication.
		void preparePlannedLargeValues(std::span<const NativeGraphLargeValueMutation> mutations,
				std::span<LargeValueMaterializationResult> materializations, LogSequenceNumber checkpointLsn) const;

		void preparePlannedLargeValue(const NativeGraphLargeValueMutation &mutation,
									  LargeValueMaterializationResult &materialization,
									  LogSequenceNumber checkpointLsn) const;
		// Materializes every page in one metadata-owned Property pool batch. Page
		// ordinals retain the exact page packing and key order validated by metadata.
		void preparePlannedEntityRootPageBatch(
				std::span<const EntityPropertyRootMutationPlan> plans,
				std::span<const std::vector<size_t>> pagePlanOrdinals,
				NativeGraphPropertyPageBatchMaterialization &materialization,
				LogSequenceNumber checkpointLsn) const;
	private:
		friend class PropertyCollectionRootScanCursor;
		friend class PropertyCollectionRootStoreTestAccess;
        // Returns false when the existing ROW merge must still run. Hybrid
        // output positions have already been filled and must be preserved.
        [[nodiscard]] bool readHybridSelectedBatch(const MetadataSnapshotPin &snapshot,
            std::span<const EntityPropertyKey> keys, std::span<const uint32_t> tokens,
            std::span<std::optional<PropertyCollectionRootEntry>> values,
            std::span<std::optional<size_t>> counts, const CancellationCheckpoint *cancellation,
            const SelectedPropertyReadAdmission *admission = nullptr,
            const std::function<void(size_t, size_t, std::optional<PropertyCollectionRootEntry>)> *sink = nullptr,
            const PropertyScalarReadSink *scalars = nullptr,
            const PropertyTextReadSink *texts = nullptr,
            const PropertyColumnBatchReadSinks *batches = nullptr,
            const std::function<void()> *beforeColdRead = nullptr) const;

		[[nodiscard]] PropertyCollectionRoot decodeEntityRoot(const MetadataSnapshotPin &snapshot,
															  const EntityPropertyKey &key,
															  const EntityPropertyLocation &location,
															  PageId pageId, PageOwnerKind ownerKind,
															  std::span<const std::byte> bytes, const SelectedPropertyReadAdmission *admission = nullptr, const SelectedPropertyReadAdmission *slotsAdmission = nullptr) const;
		[[nodiscard]] std::optional<PropertyCollectionRootEntry>
		decodeSelectedEntityProperty(const MetadataSnapshotPin &snapshot, const EntityPropertyKey &key,
															  const EntityPropertyLocation &location, PageId pageId,
															  PageOwnerKind ownerKind, std::span<const std::byte> bytes,
																																									 uint32_t keyToken) const;
		[[nodiscard]] bool decodeEntityPropertyExists(const MetadataSnapshotPin &snapshot, const EntityPropertyKey &key,
																																								 const EntityPropertyLocation &location, PageId pageId,
																																								 PageOwnerKind ownerKind, std::span<const std::byte> bytes,
																																												 uint32_t keyToken, bool requireNonNull = false) const;
		[[nodiscard]] std::vector<std::optional<PropertyCollectionRootEntry>>
		decodeSelectedEntityProperties(const MetadataSnapshotPin &snapshot, const EntityPropertyKey &key,
									  const EntityPropertyLocation &location, PageId pageId,
									  PageOwnerKind ownerKind, std::span<const std::byte> bytes,
																																														std::span<const uint32_t> keyTokens,
				const SelectedPropertyReadAdmission *admission = nullptr) const;
        [[nodiscard]] bool emitSelectedEntityProperties(const EntityPropertyKey &key,
            const EntityPropertyLocation &location, PageId pageId, PageOwnerKind ownerKind,
            std::span<const std::byte> bytes, std::span<const uint32_t> tokens, size_t ownerOrdinal,
            const std::function<void(size_t, size_t, std::optional<PropertyCollectionRootEntry>)> &sink,
            const PropertyScalarReadSink &scalars, const PropertyTextReadSink *texts,
            const SelectedPropertyReadAdmission *admission, const CancellationCheckpoint *cancellation) const;
		[[nodiscard]] std::pair<size_t, std::vector<std::optional<PropertyCollectionRootEntry>>>
		decodeSelectedEntityPropertiesAndCount(const MetadataSnapshotPin &snapshot, const EntityPropertyKey &key,
				const EntityPropertyLocation &location, PageId pageId, PageOwnerKind ownerKind,
				std::span<const std::byte> bytes, std::span<const uint32_t> keyTokens,
				const SelectedPropertyReadAdmission *admission,
				const SelectedPropertyReadAdmission *transientBufferAdmission) const;
		[[nodiscard]] size_t decodeEntityPropertyCount(const MetadataSnapshotPin &snapshot, const EntityPropertyKey &key,
																																																								const EntityPropertyLocation &location,
																																																														PageId pageId, PageOwnerKind ownerKind,
																																																														std::span<const std::byte> bytes,
				const SelectedPropertyReadAdmission *transientBufferAdmission = nullptr) const;
		[[nodiscard]] EntityPropertyRootMutationPlan planEntityRootReplacementWithLargeValueId(
				const MetadataSnapshotPin &snapshot, const PropertyCollectionRoot &root,
				const PropertyCollectionRootStoreOptions &options, LargeValueId nextLargeValueId,
				const std::map<EntityPropertyKey, EntityPropertyLocation> *existingLocations = nullptr,
				std::map<size_t, std::optional<SlottedPagePlacement>> *placementCache = nullptr,
				std::optional<std::vector<SegmentId>> *currentPropertyPools = nullptr,
				EntityPropertyBatchPageCache *pageCache = nullptr) const;
		SlottedPagePool &pages_;
		LargeValueStore *large_values_ = nullptr;
		RelocatableMetadataStore &metadata_;
	};

	// This cursor is snapshot-local and retains at most one decoded Property
	// page: an owned ROW image or a pool-charged selected-page pin. Consecutive
	// batches sharing that page reuse its authenticated slotted-page view.
	class PropertyCollectionRootScanCursor final {
	public:
		~PropertyCollectionRootScanCursor();
		PropertyCollectionRootScanCursor(const PropertyCollectionRootScanCursor &) = delete;
		PropertyCollectionRootScanCursor &operator=(const PropertyCollectionRootScanCursor &) = delete;
		PropertyCollectionRootScanCursor(PropertyCollectionRootScanCursor &&other) noexcept;
		PropertyCollectionRootScanCursor &operator=(PropertyCollectionRootScanCursor &&other) noexcept;

		[[nodiscard]] PropertyCollectionRoot readEntity(const MetadataSnapshotPin &snapshot,
														const EntityPropertyKey &key, const SelectedPropertyReadAdmission *admission = nullptr,
				const SelectedPropertyReadAdmission *slotsAdmission = nullptr);
		[[nodiscard]] std::optional<PropertyCollectionRoot> readEntityOptional(const MetadataSnapshotPin &snapshot,
																	   const EntityPropertyKey &key, const SelectedPropertyReadAdmission *admission = nullptr, const SelectedPropertyReadAdmission *slotsAdmission = nullptr);
		[[nodiscard]] std::optional<PropertyCollectionRootEntry>
		readEntityPropertyOptional(const MetadataSnapshotPin &snapshot, const EntityPropertyKey &key,
																																																											 uint32_t keyToken);
		[[nodiscard]] bool readEntityPropertyExists(const MetadataSnapshotPin &snapshot, const EntityPropertyKey &key,
																																																																													 uint32_t keyToken, bool requireNonNull = false);
			// Resolves strictly ordered owners through the retained Directory cursor and
			// decodes all requested tokens from each owner payload once. Results are
			// owner-major and remain nullopt for missing owners or tokens.
		void readEntityPropertiesOptionalSorted(
				const MetadataSnapshotPin &snapshot, std::span<const EntityPropertyKey> keys,
				std::span<const uint32_t> keyTokens,
				std::span<std::optional<PropertyCollectionRootEntry>> results,
				bool columnBatch = true, const SelectedPropertyReadAdmission *admission = nullptr,
				const std::function<void(size_t, size_t, std::optional<PropertyCollectionRootEntry>)> *sink = nullptr,
            const PropertyScalarReadSink *scalars = nullptr, const PropertyTextReadSink *texts = nullptr,
            const PropertyColumnBatchReadSinks *batches = nullptr);
		// Cursor-preserving counterpart used by indexed candidate streams. It keeps
		// the owner-directory route across adjacent batches while returning the
		// selected values and root counts required by the same validation pass.
		// transientBufferAdmission reports absolute owner-array and LargeValue read buffers.
		// Caller-owned output, page copies, and manifest metadata require separate budgets.
		void readEntityPropertiesAndCountsOptionalSorted(
				const MetadataSnapshotPin &snapshot, std::span<const EntityPropertyKey> keys,
				std::span<const uint32_t> keyTokens,
				std::span<std::optional<PropertyCollectionRootEntry>> results,
				std::span<std::optional<size_t>> counts,
				const SelectedPropertyReadAdmission *admission = nullptr,
				const SelectedPropertyReadAdmission *transientBufferAdmission = nullptr);
		// Count-only counterpart for batches whose selected values are already
		// cached. It reuses the same snapshot-bound Directory cursor without
		// decoding Property values a second time.
		void readEntityPropertyCountsSorted(const MetadataSnapshotPin &snapshot,
				std::span<const EntityPropertyKey> keys,
				std::span<std::optional<size_t>> results,
				const SelectedPropertyReadAdmission *transientBufferAdmission = nullptr);

	private:
		friend class PropertyCollectionRootStore;
		PropertyCollectionRootScanCursor(const PropertyCollectionRootStore &store, Generation generation,
									 const CancellationCheckpoint *cancellation);

		[[nodiscard]] PropertyCollectionRoot readLocation(const MetadataSnapshotPin &snapshot,
														  const EntityPropertyKey &key,
														  const EntityPropertyLocation &location, const SelectedPropertyReadAdmission *admission = nullptr, const SelectedPropertyReadAdmission *slotsAdmission = nullptr);
		[[nodiscard]] std::optional<PropertyCollectionRootEntry>
		readSelectedLocation(const MetadataSnapshotPin &snapshot, const EntityPropertyKey &key,
																	 const EntityPropertyLocation &location, uint32_t keyToken);
		[[nodiscard]] std::vector<std::optional<PropertyCollectionRootEntry>>
		readSelectedLocation(const MetadataSnapshotPin &snapshot, const EntityPropertyKey &key,
																	 const EntityPropertyLocation &location,
																							 std::span<const uint32_t> keyTokens,
				const SelectedPropertyReadAdmission *admission = nullptr);
		[[nodiscard]] bool readSelectedPresenceLocation(const MetadataSnapshotPin &snapshot,
																							 const EntityPropertyKey &key,
																							 const EntityPropertyLocation &location, uint32_t keyToken, bool requireNonNull = false);
		// Entity scans visit logical owners in directory-key order. Retain the
		// directory's root-to-leaf cursor and at most one look-ahead location.
		// Out-of-order callers fall back to point lookup without reusing ambiguous
		// continuation state.
		[[nodiscard]] std::optional<EntityPropertyLocation>
		findLocationOptional(const MetadataSnapshotPin &snapshot, const EntityPropertyKey &key);
		void accountDirectoryPages();
		void validateSnapshot(const MetadataSnapshotPin &snapshot, bool selectedPage = false);

		std::unique_ptr<HybridPropertyScanContext> hybrid_context_;
		const PropertyCollectionRootStore *store_ = nullptr;
		Generation generation_{};
		const CancellationCheckpoint *cancellation_ = nullptr;
		PageId current_page_id_{};
		std::optional<SlottedPage> current_page_;
		// Mutually exclusive with the owned ROW image; the pool charges its frame.
		std::optional<SlottedPagePinnedRead> selected_page_;
		uint64_t data_epoch_ = 0U;
		std::optional<LocationDirectoryRangeCursor> directory_cursor_;
		std::optional<EntityPropertyLocation> pending_directory_location_;
		size_t directory_metadata_pages_reported_ = 0U;
		uint64_t last_requested_directory_id_ = 0U;
		bool directory_stream_enabled_ = true;
	};

} // namespace graph::storage
