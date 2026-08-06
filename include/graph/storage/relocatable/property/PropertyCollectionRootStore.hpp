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
#include <optional>
#include <span>
#include <vector>

#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/metadata/LocationDirectory.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRoot.hpp"

namespace graph::storage {
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

		// Canonical Stage 5 lookup. The Entity Property Directory is keyed by the
		// owning logical entity, so no Property-ID reservation or entity-record
		// reference is needed on this path.
		[[nodiscard]] PropertyCollectionRoot readEntity(const MetadataSnapshotPin &snapshot,
														const EntityPropertyKey &key) const;
		// Optional point lookup keeps normal Buffer Pool admission while avoiding a
		// separate directory probe before decoding a present owner.
		[[nodiscard]] std::optional<PropertyCollectionRoot> readEntityOptional(const MetadataSnapshotPin &snapshot,
																			   const EntityPropertyKey &key) const;
		// A logical Entity/Property scan reuses the currently decoded slotted page
		// while admitting its owned frame as sequential. Point callers keep using
		// readEntity(), whose normal admission preserves shared-page locality.
		[[nodiscard]] PropertyCollectionRootScanCursor openScanCursor(const MetadataSnapshotPin &snapshot) const;
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
		[[nodiscard]] PropertyCollectionRoot decodeEntityRoot(const MetadataSnapshotPin &snapshot,
															  const EntityPropertyKey &key,
															  const EntityPropertyLocation &location,
															  const SlottedPage &page) const;
		[[nodiscard]] EntityPropertyRootMutationPlan planEntityRootReplacementWithLargeValueId(
				const MetadataSnapshotPin &snapshot, const PropertyCollectionRoot &root,
				const PropertyCollectionRootStoreOptions &options, LargeValueId nextLargeValueId) const;
		SlottedPagePool &pages_;
		LargeValueStore *large_values_ = nullptr;
		RelocatableMetadataStore &metadata_;
	};

	// This cursor is snapshot-local and deliberately retains at most one decoded
	// 4 KiB Property page. Consecutive owners sharing that page do not reread it,
	// while moving to another page releases the transient Buffer Pool frame.
	class PropertyCollectionRootScanCursor final {
	public:
		PropertyCollectionRootScanCursor(const PropertyCollectionRootScanCursor &) = delete;
		PropertyCollectionRootScanCursor &operator=(const PropertyCollectionRootScanCursor &) = delete;
		PropertyCollectionRootScanCursor(PropertyCollectionRootScanCursor &&other) noexcept;
		PropertyCollectionRootScanCursor &operator=(PropertyCollectionRootScanCursor &&other) noexcept;

		[[nodiscard]] PropertyCollectionRoot readEntity(const MetadataSnapshotPin &snapshot,
														const EntityPropertyKey &key);
		[[nodiscard]] std::optional<PropertyCollectionRoot> readEntityOptional(const MetadataSnapshotPin &snapshot,
																			   const EntityPropertyKey &key);

	private:
		friend class PropertyCollectionRootStore;
		PropertyCollectionRootScanCursor(const PropertyCollectionRootStore &store, Generation generation) :
			store_(&store), generation_(generation) {}

		[[nodiscard]] PropertyCollectionRoot readLocation(const MetadataSnapshotPin &snapshot,
														  const EntityPropertyKey &key,
														  const EntityPropertyLocation &location);
		void validateSnapshot(const MetadataSnapshotPin &snapshot) const;

		const PropertyCollectionRootStore *store_ = nullptr;
		Generation generation_{};
		PageId current_page_id_{};
		std::optional<SlottedPage> current_page_;
	};

} // namespace graph::storage
