/**
 * @file LargeValueMaintenanceJournal.hpp
 * @brief Durable, bounded continuation state for LargeValue maintenance.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "graph/storage/relocatable/metadata/StateRecordStore.hpp"
#include "graph/utils/OwnedStdVector.hpp"

namespace graph::storage {

	class RelocatableMetadataStore;

	constexpr uint32_t RSS_LARGE_VALUE_MAINTENANCE_JOURNAL_MAGIC = 0x4A4D564CU; // Little-endian bytes: LVMJ
	constexpr uint16_t RSS_LARGE_VALUE_MAINTENANCE_JOURNAL_VERSION = 1U;
	constexpr uint16_t RSS_LARGE_VALUE_MAINTENANCE_JOURNAL_HEADER_BYTES = 64U;
	// The fixed payload capacity keeps every StateRecord replacement in one
	// stable slotted-page slot. A journal must fail closed rather than growing
	// an unbounded candidate set between maintenance cycles.
	constexpr size_t RSS_LARGE_VALUE_MAINTENANCE_JOURNAL_MAX_IDS = 128U;
	constexpr std::string_view RSS_LARGE_VALUE_MAINTENANCE_JOURNAL_KEY = "native.large-value-maintenance.v1";
    // This equal-width State identity is reserved to the native coordinator.
    // The payload remains the existing version1 wire format.
    constexpr std::string_view RSS_NATIVE_LARGE_VALUE_MAINTENANCE_JOURNAL_KEY = "native.large-value-maintenance.v2";
    static_assert(RSS_NATIVE_LARGE_VALUE_MAINTENANCE_JOURNAL_KEY.size() == RSS_LARGE_VALUE_MAINTENANCE_JOURNAL_KEY.size());
    struct LargeValueMaintenanceJournalAdvance;


	enum class LargeValueMaintenanceJournalPhase : uint8_t {
		LVMJP_ENUMERATE_MANIFESTS = 1U,
		LVMJP_SCAN_ENTITY_PROPERTIES = 2U,
		LVMJP_SCAN_STATES = 3U,
		LVMJP_RECLAIM = 4U,
	};

	struct LargeValueMaintenanceJournal {
		Generation scan_generation{};
		LargeValueMaintenanceJournalPhase phase = LargeValueMaintenanceJournalPhase::LVMJP_ENUMERATE_MANIFESTS;
		bool manifest_scan_exhausted = false;
		uint64_t manifest_after_value_id = 0U;
		uint64_t entity_property_after_directory_id = 0U;
		uint64_t state_after_id = 0U;
		utils::OwnedStdVector<LargeValueId> candidate_value_ids;
		utils::OwnedStdVector<LargeValueId> reachable_value_ids;

		bool operator==(const LargeValueMaintenanceJournal &) const = default;
	};

    class NativeJournalProgressEvidence final {
        friend class LargeValueReachabilityReclaimer;
        friend class RelocatableMetadataStore;
        Superblock source_;
        LargeValueMaintenanceJournal result_;
        NativeJournalProgressEvidence(const Superblock &source, const LargeValueMaintenanceJournal &result)
            : source_(source), result_(result) {}
    public:
        NativeJournalProgressEvidence(const NativeJournalProgressEvidence &) = default;
        NativeJournalProgressEvidence(NativeJournalProgressEvidence &&) = default;
    };

	class LargeValueMaintenanceJournalCodec final {
	public:
		[[nodiscard]] static std::vector<std::byte> encode(const LargeValueMaintenanceJournal &journal);
		[[nodiscard]] static LargeValueMaintenanceJournal decode(std::span<const std::byte> bytes);
	};

	struct LargeValueMaintenanceJournalStoreOptions {
		StateRecordStoreOptions state_record_options;
		uint64_t state_id = 0U;
	};

	struct LargeValueMaintenanceJournalSave {
		LargeValueMaintenanceJournal journal;
		StateLocation location;

		bool operator==(const LargeValueMaintenanceJournalSave &) const = default;
	};

	// The State Directory makes journal identity independent of a relocated
	// State page. Journal payloads always occupy one fixed-capacity slot, so an
	// update is atomically published with its State route rather than relying on
	// an unmanaged sidecar or in-memory scheduler state.
	class LargeValueMaintenanceJournalStore final {
	public:
		LargeValueMaintenanceJournalStore(StateRecordStore &states, RelocatableMetadataStore &metadata,
												  LargeValueMaintenanceJournalStoreOptions options);

		[[nodiscard]] std::optional<LargeValueMaintenanceJournal> load(const MetadataSnapshotPin &snapshot) const;
		[[nodiscard]] StateLocation save(const LargeValueMaintenanceJournal &journal,
            const MetadataPublishOptions &publishOptions = {});
		// A State-record publication advances the Superblock generation once. This
		// helper records that known next generation before the atomic upsert, so a
		// resumed maintenance pass distinguishes its own durable progress from an
		// external graph/owner mutation.
		[[nodiscard]] LargeValueMaintenanceJournalSave saveAfterScan(const MetadataSnapshotPin &snapshot,
				const LargeValueMaintenanceJournal &journal, const MetadataPublishOptions &publishOptions = {});
        // Native owner progress requires the exact unmodified advancement issued
        // by the reclaimer. Plain journal values may only reset/start a scan.
        [[nodiscard]] LargeValueMaintenanceJournalSave saveAfterScan(const MetadataSnapshotPin &snapshot,
            const LargeValueMaintenanceJournalAdvance &advance, const MetadataPublishOptions &publishOptions = {});

	private:
		StateRecordStore &states_;
		RelocatableMetadataStore &metadata_;
		LargeValueMaintenanceJournalStoreOptions options_;
	};

} // namespace graph::storage
