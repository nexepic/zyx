/**
 * @file LargeValueMaintenanceJournal.cpp
 * @brief Fixed-capacity codec and State-backed storage for maintenance continuation state.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/maintenance/LargeValueMaintenanceJournal.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/maintenance/LargeValueReachabilityReclaimer.hpp"
#include "graph/storage/relocatable/property/HybridPropertyDescriptor.hpp"
#include "graph/utils/ChecksumUtils.hpp"

namespace graph::storage {

	namespace {

		constexpr size_t MAGIC_OFFSET = 0U;
		constexpr size_t VERSION_OFFSET = 4U;
		constexpr size_t HEADER_BYTES_OFFSET = 6U;
		constexpr size_t PHASE_OFFSET = 8U;
		constexpr size_t MANIFEST_SCAN_EXHAUSTED_OFFSET = 9U;
		constexpr size_t SCAN_GENERATION_OFFSET = 12U;
		constexpr size_t MANIFEST_CURSOR_OFFSET = 20U;
		constexpr size_t ENTITY_PROPERTY_CURSOR_OFFSET = 28U;
		constexpr size_t STATE_CURSOR_OFFSET = 36U;
		constexpr size_t CANDIDATE_COUNT_OFFSET = 44U;
		constexpr size_t REACHABLE_COUNT_OFFSET = 48U;
		constexpr size_t PAYLOAD_CRC_OFFSET = 52U;
		constexpr size_t HEADER_CRC_OFFSET = 56U;
		constexpr size_t RESERVED_OFFSET = 60U;
		constexpr size_t FIXED_PAYLOAD_BYTES = RSS_LARGE_VALUE_MAINTENANCE_JOURNAL_MAX_IDS * sizeof(uint64_t) * 2U;

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		[[nodiscard]] uint32_t crc(std::span<const std::byte> bytes) {
			return utils::calculateCrc(bytes.data(), bytes.size());
		}

		[[nodiscard]] bool isKnownPhase(LargeValueMaintenanceJournalPhase phase) {
			return phase == LargeValueMaintenanceJournalPhase::LVMJP_ENUMERATE_MANIFESTS ||
					phase == LargeValueMaintenanceJournalPhase::LVMJP_SCAN_ENTITY_PROPERTIES ||
					phase == LargeValueMaintenanceJournalPhase::LVMJP_SCAN_STATES ||
					phase == LargeValueMaintenanceJournalPhase::LVMJP_RECLAIM;
		}

		void validateIds(std::span<const LargeValueId> ids, const char *message) {
			if (ids.size() > RSS_LARGE_VALUE_MAINTENANCE_JOURNAL_MAX_IDS) {
				fail(FormatError::FERR_INVALID_HEADER, message);
			}
			uint64_t previous = 0U;
			for (const LargeValueId id: ids) {
				if (!id.isValid() || id.value() <= previous) {
					fail(FormatError::FERR_INVALID_HEADER, message);
				}
				previous = id.value();
			}
		}

		void validate(const LargeValueMaintenanceJournal &journal) {
			if (!journal.scan_generation.isValid() || !isKnownPhase(journal.phase)) {
				fail(FormatError::FERR_INVALID_HEADER, "LargeValue maintenance journal has an invalid generation or phase");
			}
			validateIds(journal.candidate_value_ids, "LargeValue maintenance journal has invalid candidate identities");
			validateIds(journal.reachable_value_ids, "LargeValue maintenance journal has invalid reachable identities");
			if (!std::ranges::includes(journal.candidate_value_ids, journal.reachable_value_ids, {},
															[](LargeValueId id) { return id.value(); },
															[](LargeValueId id) { return id.value(); })) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "LargeValue maintenance journal marks a non-candidate value as reachable");
			}
		const bool hasCandidates = !journal.candidate_value_ids.empty();
		const bool invalidPhase =
				journal.phase == LargeValueMaintenanceJournalPhase::LVMJP_ENUMERATE_MANIFESTS &&
				(hasCandidates || !journal.reachable_value_ids.empty() || journal.entity_property_after_directory_id != 0U ||
				 journal.state_after_id != 0U);
		if (invalidPhase) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "LargeValue maintenance manifest phase cannot carry owner progress or candidate evidence");
		}
		if (!hasCandidates && journal.phase != LargeValueMaintenanceJournalPhase::LVMJP_ENUMERATE_MANIFESTS) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "LargeValue maintenance owner or reclaim phase requires durable candidate evidence");
		}
	}

	void writeHeader(std::span<std::byte> bytes, const LargeValueMaintenanceJournal &journal, uint32_t payloadCrc,
						 uint32_t headerCrc) {
		LittleEndianCodec::writeU32(bytes, MAGIC_OFFSET, RSS_LARGE_VALUE_MAINTENANCE_JOURNAL_MAGIC);
		LittleEndianCodec::writeU16(bytes, VERSION_OFFSET, RSS_LARGE_VALUE_MAINTENANCE_JOURNAL_VERSION);
		LittleEndianCodec::writeU16(bytes, HEADER_BYTES_OFFSET, RSS_LARGE_VALUE_MAINTENANCE_JOURNAL_HEADER_BYTES);
		bytes[PHASE_OFFSET] = static_cast<std::byte>(journal.phase);
		bytes[MANIFEST_SCAN_EXHAUSTED_OFFSET] = journal.manifest_scan_exhausted ? std::byte{1U} : std::byte{};
		std::fill(bytes.begin() + MANIFEST_SCAN_EXHAUSTED_OFFSET + 1U, bytes.begin() + SCAN_GENERATION_OFFSET,
				  std::byte{});
		LittleEndianCodec::writeU64(bytes, SCAN_GENERATION_OFFSET, journal.scan_generation.value());
		LittleEndianCodec::writeU64(bytes, MANIFEST_CURSOR_OFFSET, journal.manifest_after_value_id);
		LittleEndianCodec::writeU64(bytes, ENTITY_PROPERTY_CURSOR_OFFSET, journal.entity_property_after_directory_id);
		LittleEndianCodec::writeU64(bytes, STATE_CURSOR_OFFSET, journal.state_after_id);
		LittleEndianCodec::writeU32(bytes, CANDIDATE_COUNT_OFFSET,
								 static_cast<uint32_t>(journal.candidate_value_ids.size()));
		LittleEndianCodec::writeU32(bytes, REACHABLE_COUNT_OFFSET,
								 static_cast<uint32_t>(journal.reachable_value_ids.size()));
		LittleEndianCodec::writeU32(bytes, PAYLOAD_CRC_OFFSET, payloadCrc);
		LittleEndianCodec::writeU32(bytes, HEADER_CRC_OFFSET, headerCrc);
		LittleEndianCodec::writeU32(bytes, RESERVED_OFFSET, 0U);
	}

	} // namespace

	std::vector<std::byte> LargeValueMaintenanceJournalCodec::encode(const LargeValueMaintenanceJournal &journal) {
		validate(journal);
		std::vector<std::byte> bytes(RSS_LARGE_VALUE_MAINTENANCE_JOURNAL_HEADER_BYTES + FIXED_PAYLOAD_BYTES);
		std::span<std::byte> payload = std::span(bytes).subspan(RSS_LARGE_VALUE_MAINTENANCE_JOURNAL_HEADER_BYTES);
		for (size_t index = 0U; index < journal.candidate_value_ids.size(); ++index) {
			LittleEndianCodec::writeU64(payload, index * sizeof(uint64_t), journal.candidate_value_ids[index].value());
		}
		const size_t reachableOffset = RSS_LARGE_VALUE_MAINTENANCE_JOURNAL_MAX_IDS * sizeof(uint64_t);
		for (size_t index = 0U; index < journal.reachable_value_ids.size(); ++index) {
			LittleEndianCodec::writeU64(payload, reachableOffset + index * sizeof(uint64_t),
									  journal.reachable_value_ids[index].value());
		}
		const uint32_t payloadCrc = crc(payload);
		writeHeader(bytes, journal, payloadCrc, 0U);
		writeHeader(bytes, journal, payloadCrc, crc(std::span(bytes).first(RSS_LARGE_VALUE_MAINTENANCE_JOURNAL_HEADER_BYTES)));
		return bytes;
	}

	LargeValueMaintenanceJournal LargeValueMaintenanceJournalCodec::decode(std::span<const std::byte> bytes) {
		if (bytes.size() != RSS_LARGE_VALUE_MAINTENANCE_JOURNAL_HEADER_BYTES + FIXED_PAYLOAD_BYTES) {
			fail(FormatError::FERR_BUFFER_TOO_SMALL, "LargeValue maintenance journal has an invalid fixed payload size");
		}
		std::array<std::byte, RSS_LARGE_VALUE_MAINTENANCE_JOURNAL_HEADER_BYTES> header{};
		std::copy_n(bytes.begin(), header.size(), header.begin());
		const uint32_t expectedHeaderCrc = LittleEndianCodec::readU32(header, HEADER_CRC_OFFSET);
		LittleEndianCodec::writeU32(header, HEADER_CRC_OFFSET, 0U);
		if (crc(header) != expectedHeaderCrc) {
			fail(FormatError::FERR_CHECKSUM_MISMATCH, "LargeValue maintenance journal header checksum does not match");
		}
		const uint8_t rawPhase = std::to_integer<uint8_t>(bytes[PHASE_OFFSET]);
		const uint8_t rawManifestScanExhausted = std::to_integer<uint8_t>(bytes[MANIFEST_SCAN_EXHAUSTED_OFFSET]);
		const uint8_t invalidHeader =
				static_cast<uint8_t>(LittleEndianCodec::readU32(bytes, MAGIC_OFFSET) != RSS_LARGE_VALUE_MAINTENANCE_JOURNAL_MAGIC) |
				static_cast<uint8_t>(LittleEndianCodec::readU16(bytes, VERSION_OFFSET) != RSS_LARGE_VALUE_MAINTENANCE_JOURNAL_VERSION) |
				static_cast<uint8_t>(LittleEndianCodec::readU16(bytes, HEADER_BYTES_OFFSET) !=
										 RSS_LARGE_VALUE_MAINTENANCE_JOURNAL_HEADER_BYTES) |
				static_cast<uint8_t>(rawManifestScanExhausted > 1U || bytes[10] != std::byte{} || bytes[11] != std::byte{}) |
				static_cast<uint8_t>(LittleEndianCodec::readU32(bytes, RESERVED_OFFSET) != 0U);
		if (invalidHeader != 0U) {
			fail(FormatError::FERR_INVALID_HEADER, "LargeValue maintenance journal has invalid fixed fields");
		}
		const uint32_t candidateCount = LittleEndianCodec::readU32(bytes, CANDIDATE_COUNT_OFFSET);
		const uint32_t reachableCount = LittleEndianCodec::readU32(bytes, REACHABLE_COUNT_OFFSET);
		if (candidateCount > RSS_LARGE_VALUE_MAINTENANCE_JOURNAL_MAX_IDS ||
			reachableCount > RSS_LARGE_VALUE_MAINTENANCE_JOURNAL_MAX_IDS) {
			fail(FormatError::FERR_INVALID_HEADER, "LargeValue maintenance journal exceeds its fixed evidence capacity");
		}
		const std::span<const std::byte> payload = bytes.subspan(RSS_LARGE_VALUE_MAINTENANCE_JOURNAL_HEADER_BYTES);
		if (crc(payload) != LittleEndianCodec::readU32(bytes, PAYLOAD_CRC_OFFSET)) {
			fail(FormatError::FERR_CHECKSUM_MISMATCH, "LargeValue maintenance journal payload checksum does not match");
		}

		LargeValueMaintenanceJournal journal;
		journal.scan_generation = Generation(LittleEndianCodec::readU64(bytes, SCAN_GENERATION_OFFSET));
		journal.phase = static_cast<LargeValueMaintenanceJournalPhase>(rawPhase);
		journal.manifest_scan_exhausted = rawManifestScanExhausted != 0U;
		journal.manifest_after_value_id = LittleEndianCodec::readU64(bytes, MANIFEST_CURSOR_OFFSET);
		journal.entity_property_after_directory_id = LittleEndianCodec::readU64(bytes, ENTITY_PROPERTY_CURSOR_OFFSET);
		journal.state_after_id = LittleEndianCodec::readU64(bytes, STATE_CURSOR_OFFSET);
		journal.candidate_value_ids.reserve(candidateCount);
		journal.reachable_value_ids.reserve(reachableCount);
		for (uint32_t index = 0U; index < candidateCount; ++index) {
			journal.candidate_value_ids.emplace_back(LittleEndianCodec::readU64(payload, index * sizeof(uint64_t)));
		}
		const size_t reachableOffset = RSS_LARGE_VALUE_MAINTENANCE_JOURNAL_MAX_IDS * sizeof(uint64_t);
		for (uint32_t index = 0U; index < reachableCount; ++index) {
			journal.reachable_value_ids.emplace_back(
					LittleEndianCodec::readU64(payload, reachableOffset + index * sizeof(uint64_t)));
		}
		const size_t candidateUnusedOffset = static_cast<size_t>(candidateCount) * sizeof(uint64_t);
		const size_t reachableUnusedOffset = reachableOffset + static_cast<size_t>(reachableCount) * sizeof(uint64_t);
		if (!std::ranges::all_of(payload.subspan(candidateUnusedOffset,
											   reachableOffset - candidateUnusedOffset),
							  [](std::byte value) { return value == std::byte{}; }) ||
			!std::ranges::all_of(payload.subspan(reachableUnusedOffset),
							  [](std::byte value) { return value == std::byte{}; })) {
			fail(FormatError::FERR_INVALID_HEADER, "LargeValue maintenance journal has noncanonical unused evidence bytes");
		}
		validate(journal);
		return journal;
	}

	LargeValueMaintenanceJournalStore::LargeValueMaintenanceJournalStore(
			StateRecordStore &states, RelocatableMetadataStore &metadata, LargeValueMaintenanceJournalStoreOptions options) :
		states_(states), metadata_(metadata), options_(options) {
		if (!states_.usesMetadataStore(metadata_) || options_.state_id == 0U) {
			throw std::invalid_argument("LargeValue maintenance journal requires its State store and a nonzero State ID");
		}
	}

	std::optional<LargeValueMaintenanceJournal>
	LargeValueMaintenanceJournalStore::load(const MetadataSnapshotPin &snapshot) const {
		if (!metadata_.findStateLocationOptional(snapshot, options_.state_id).has_value()) {
			return std::nullopt;
		}
		const StateRecord record = states_.read(snapshot, options_.state_id);
		if ((record.key != RSS_LARGE_VALUE_MAINTENANCE_JOURNAL_KEY && record.key != RSS_NATIVE_LARGE_VALUE_MAINTENANCE_JOURNAL_KEY) ||
			record.storage_kind != StateRecordStorageKind::SRS_INLINE || record.large_value_id.isValid()) {
			fail(FormatError::FERR_INVALID_STATE, "LargeValue maintenance State record has an invalid journal identity");
		}
        auto journal = LargeValueMaintenanceJournalCodec::decode(record.inline_bytes);
        if (record.key == RSS_LARGE_VALUE_MAINTENANCE_JOURNAL_KEY && snapshot.superblock().hybrid_property_descriptor_root.isValid()) {
            const auto descriptor = metadata_.pages_.readHybridPropertyDescriptor(
                snapshot.superblock().hybrid_property_descriptor_root, snapshot.superblock()).descriptor;
            if (std::ranges::any_of(descriptor.roots, [](const auto &root) { return root.page_id.isValid(); })) return {};
        }
        return journal;
	}

	StateLocation LargeValueMaintenanceJournalStore::save(const LargeValueMaintenanceJournal &journal,
            const MetadataPublishOptions &publishOptions) {
        if (auto native = metadata_.upsertNativeMaintenanceJournal(journal, options_, publishOptions)) return *native;
		StateRecord record;
		record.state_id = options_.state_id;
		record.key = std::string(RSS_LARGE_VALUE_MAINTENANCE_JOURNAL_KEY);
		record.storage_kind = StateRecordStorageKind::SRS_INLINE;
		record.inline_bytes = LargeValueMaintenanceJournalCodec::encode(journal);
		return states_.upsert(record, options_.state_record_options, publishOptions);
	}

	LargeValueMaintenanceJournalSave
	LargeValueMaintenanceJournalStore::saveAfterScan(const MetadataSnapshotPin &snapshot,
														 const LargeValueMaintenanceJournal &journal, const MetadataPublishOptions &publishOptions) {
		// This lookup proves that the supplied pin belongs to this metadata store
		// even when the journal has not yet been materialized.
		static_cast<void>(metadata_.findStateLocationOptional(snapshot, options_.state_id));
		if (journal.scan_generation != snapshot.superblock().generation) {
			fail(FormatError::FERR_INVALID_STATE,
				 "LargeValue maintenance journal cannot persist evidence from another metadata generation");
		}
		if (snapshot.superblock().generation.value() == std::numeric_limits<uint64_t>::max()) {
			fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
				 "LargeValue maintenance journal cannot advance the metadata generation");
		}
		LargeValueMaintenanceJournal persisted = journal;
		persisted.scan_generation = Generation(snapshot.superblock().generation.value() + 1U);
		return {persisted, save(persisted, publishOptions)};
	}

    LargeValueMaintenanceJournalSave LargeValueMaintenanceJournalStore::saveAfterScan(
        const MetadataSnapshotPin &snapshot, const LargeValueMaintenanceJournalAdvance &advance,
        const MetadataPublishOptions &publishOptions) {
        static_cast<void>(metadata_.findStateLocationOptional(snapshot, options_.state_id));
        if (advance.journal.scan_generation != snapshot.superblock().generation ||
            snapshot.superblock().generation.value() == (std::numeric_limits<uint64_t>::max)())
            fail(FormatError::FERR_INVALID_STATE, "journal advancement requires its source pin and next generation");
        auto persisted = advance.journal;
        persisted.scan_generation = Generation(snapshot.superblock().generation.value() + 1U);
        const auto evidence = advance.native_evidence_ ? &*advance.native_evidence_ : nullptr;
        if (auto native = metadata_.upsertNativeMaintenanceJournal(persisted, options_, publishOptions, evidence))
            return {std::move(persisted), *native};
        return saveAfterScan(snapshot, advance.journal, publishOptions);
    }

} // namespace graph::storage
