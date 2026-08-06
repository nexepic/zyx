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
		// Implementations must stop before visiting more than `maximumLocations`
		// durable owner records, mark only references with this scanner's media
		// type, and return the exact number visited.
		[[nodiscard]] virtual size_t scanReachable(const MetadataSnapshotPin &snapshot, size_t maximumLocations,
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
		[[nodiscard]] size_t scanReachable(const MetadataSnapshotPin &snapshot, size_t maximumLocations,
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
		[[nodiscard]] size_t scanReachable(const MetadataSnapshotPin &snapshot, size_t maximumLocations,
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
		size_t maximum_manifest_nodes = 0U;
		// All registered typed media scanners share this explicit owner-record
		// budget. Unknown media is retained without opening an owner namespace.
		size_t maximum_owner_locations = 0U;
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

	class LargeValueReachabilityReclaimer final {
	public:
		LargeValueReachabilityReclaimer(LargeValueStore &values, SlottedPagePool &propertyPages,
										RelocatableMetadataStore &metadata,
										std::span<const LargeValueOwnershipScannerPtr> additionalScanners = {});

		[[nodiscard]] LargeValueReachabilityReclaimReport reclaim(const LargeValueReachabilityReclaimOptions &options,
																  LogSequenceNumber checkpointLsn,
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
