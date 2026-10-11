/**
 * @file LargeValueReachabilityReclaimer.hpp
 * @brief Snapshot-safe reachability reclamation for native graph LargeValue records.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "graph/storage/relocatable/maintenance/LargeValueMaintenanceJournal.hpp"
#include "graph/storage/relocatable/property/LargeValueStore.hpp"

namespace graph::storage {

	class SlottedPagePool;
	class RelocatableMetadataStore;

	// Typed owners enumerate only references that are reachable from their own
	// durable namespace. The sink re-authenticates every reference against the
	// directory-routed Manifest before it can retain a LargeValue.
	class LargeValueOwnershipScanSink {
	public:
		virtual ~LargeValueOwnershipScanSink() = default;
		virtual void markReachable(const LargeValueRef &reference) = 0;
	};

	// Media ownership is intentionally registered per reclaimer instance instead
	// of through mutable global state. This keeps maintenance deterministic and
	// lets an unopened owner namespace remain fail-closed.
	class LargeValueOwnershipScanner {
	public:
		virtual ~LargeValueOwnershipScanner() = default;
		[[nodiscard]] virtual std::string_view mediaType() const noexcept = 0;
		// The cursor is a scanner-defined stable logical owner identity. It must
		// never be a physical page/slot because a later relocation can replace it.
		// An exhausted result proves that this owner namespace was fully examined
		// for the pinned generation; callers must not reclaim from a partial scan.
		struct Progress {
			uint64_t last_scanned_location_id = 0U;
			size_t locations_scanned = 0U;
			size_t metadata_pages_scanned = 0U;
			bool exhausted = false;
		};
		[[nodiscard]] virtual Progress scanReachableAfter(const MetadataSnapshotPin &snapshot, uint64_t afterLocationId,
														  size_t maximumLocations, size_t maximumMetadataPages,
														  LargeValueOwnershipScanSink &sink) const = 0;
	};

	using LargeValueOwnershipScannerPtr = std::shared_ptr<const LargeValueOwnershipScanner>;

	// The native Entity Property namespace is the first typed LargeValue owner.
	// Future owners (for example generic attachments) implement the interface
	// above and are supplied to the reclaimer constructor.
	class EntityPropertyLargeValueOwnershipScanner final : public LargeValueOwnershipScanner {
	public:
		EntityPropertyLargeValueOwnershipScanner(SlottedPagePool &propertyPages, RelocatableMetadataStore &metadata);

		[[nodiscard]] std::string_view mediaType() const noexcept override;
		[[nodiscard]] Progress scanReachableAfter(const MetadataSnapshotPin &snapshot, uint64_t afterLocationId,
												  size_t maximumLocations, size_t maximumMetadataPages,
												  LargeValueOwnershipScanSink &sink) const override;

	private:
		SlottedPagePool &property_pages_;
		RelocatableMetadataStore &metadata_;
	};

	// State records retain only the stable LargeValue identity. The scanner
	// re-resolves its Manifest in the pinned snapshot before retaining it, so a
	// stale or cross-namespace identifier cannot keep unrelated data alive.
	class StateLargeValueOwnershipScanner final : public LargeValueOwnershipScanner {
	public:
		StateLargeValueOwnershipScanner(SlottedPagePool &pages, LargeValueStore &values,
										RelocatableMetadataStore &metadata);

		[[nodiscard]] std::string_view mediaType() const noexcept override;
		[[nodiscard]] Progress scanReachableAfter(const MetadataSnapshotPin &snapshot, uint64_t afterLocationId,
												  size_t maximumLocations, size_t maximumMetadataPages,
												  LargeValueOwnershipScanSink &sink) const override;

	private:
		SlottedPagePool &pages_;
		LargeValueStore &values_;
		RelocatableMetadataStore &metadata_;
	};

	// Every bound applies before publication. A malformed directory therefore
	// cannot turn one maintenance pass into an unbounded metadata or page walk.
	struct LargeValueReachabilityReclaimOptions {
		// Directory routes and recursive Manifest nodes grow independently. Keeping
		// separate bounds prevents one valid large value from exhausting a bound
		// intended only for the number of logical values.
		size_t maximum_manifest_locations = 0U;
		// The Manifest Directory is a separate routed metadata tree. A small
		// candidate batch must still be allowed to cross the metadata pages needed
		// to determine its next stable logical cursor.
		size_t maximum_manifest_metadata_pages = 0U;
		size_t maximum_manifest_nodes = 0U;
		// All registered typed media scanners share this explicit owner-record
		// budget. Unknown media is retained without opening an owner namespace.
		size_t maximum_owner_locations = 0U;
		// Owner directories are independent metadata trees. This explicit I/O cap
		// prevents a sparse owner namespace from making a bounded maintenance pass
		// scan an unbounded number of routed metadata pages.
		size_t maximum_owner_metadata_pages = 0U;
		size_t maximum_data_pages = 0U;
		size_t maximum_retirements = 0U;
		// Shared data pools require one Page Directory pass to prove that a
		// candidate pool has no surviving logical pages. This independent cap
		// prevents malformed metadata from turning reclamation into an unbounded
		// traversal.
		size_t maximum_page_locations = 0U;
		size_t maximum_pools = 64U;
		// COW deletion from the persistent content index can require a fresh
		// Index-page pool. Maintenance receives this explicit allocator policy
		// instead of silently choosing a global physical range.
		PhysicalSlotId first_index_slot{};
		PhysicalSlotId last_index_slot{};
		// The durable journal is a State record, not an unmanaged sidecar. Its
		// dedicated owner, State ID, and physical range are explicit so normal
		// graph State records cannot accidentally share this control-plane pool.
		PhysicalSlotId first_journal_state_slot{};
		PhysicalSlotId last_journal_state_slot{};
		uint64_t journal_state_page_owner_id = 0U;
		uint64_t journal_state_id = 0U;
	};

	struct LargeValueReachabilityReclaimReport {
		Generation scanned_generation{};
		Generation published_generation{};
		uint64_t manifest_locations_scanned = 0U;
		uint64_t manifest_nodes_scanned = 0U;
		uint64_t owner_locations_scanned = 0U;
		uint64_t data_pages_scanned = 0U;
		uint64_t page_locations_scanned = 0U;
		uint64_t reachable_values = 0U;
		uint64_t retained_unregistered_values = 0U;
		uint64_t retired_values = 0U;
		uint64_t retired_manifest_pages = 0U;
		uint64_t retired_data_pages = 0U;
		uint64_t pools_scanned = 0U;
		uint64_t empty_pools_retired = 0U;
		uint64_t retired_pools_released = 0U;

		constexpr bool operator==(const LargeValueReachabilityReclaimReport &) const = default;
	};

	// One journal advancement only reads one pinned generation. It never retires
	// a value; the caller persists the returned evidence before a later reclaim
	// phase can use it. If a configured bound cannot advance a stable logical
	// cursor, the method fails closed instead of resuming from a physical page.
	struct LargeValueMaintenanceJournalAdvance {
		LargeValueMaintenanceJournal journal;
		uint64_t manifest_locations_scanned = 0U;
		uint64_t owner_locations_scanned = 0U;
		uint64_t owner_metadata_pages_scanned = 0U;
		bool completed_without_candidates = false;
	};

	class LargeValueReachabilityReclaimer final {
	public:
		LargeValueReachabilityReclaimer(LargeValueStore &values, SlottedPagePool &propertyPages,
										RelocatableMetadataStore &metadata,
										std::span<const LargeValueOwnershipScannerPtr> additionalScanners = {});

		[[nodiscard]] LargeValueReachabilityReclaimReport reclaim(const LargeValueReachabilityReclaimOptions &options,
																  LogSequenceNumber checkpointLsn,
																  const MetadataPublishOptions &publishOptions = {});
		[[nodiscard]] LargeValueMaintenanceJournalAdvance advanceJournal(
				const MetadataSnapshotPin &snapshot, const LargeValueMaintenanceJournal &journal,
				const LargeValueReachabilityReclaimOptions &options) const;
		// Publishes only the durable candidate set whose two built-in owner scans
		// completed in the journal's pinned generation. This intentionally never
		// falls back to a new full owner scan: stale evidence must be restarted by
		// the caller instead of being silently mixed with another generation.
		[[nodiscard]] LargeValueReachabilityReclaimReport reclaimJournal(
				MetadataSnapshotPin &snapshot, const LargeValueMaintenanceJournal &journal,
				const LargeValueReachabilityReclaimOptions &options, LogSequenceNumber checkpointLsn,
				const MetadataPublishOptions &publishOptions = {});

	private:
		LargeValueStore &values_;
		SlottedPagePool &property_pages_;
		RelocatableMetadataStore &metadata_;
		EntityPropertyLargeValueOwnershipScanner entity_property_scanner_;
		StateLargeValueOwnershipScanner state_scanner_;
		std::vector<LargeValueOwnershipScannerPtr> additional_scanners_;
		std::map<std::string, const LargeValueOwnershipScanner *, std::less<>> scanners_;
	};

} // namespace graph::storage
