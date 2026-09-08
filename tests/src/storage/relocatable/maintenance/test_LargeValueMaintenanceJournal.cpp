/**
 * @file test_LargeValueMaintenanceJournal.cpp
 * @brief Fixed-capacity LargeValue maintenance journal codec contracts.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include <gtest/gtest.h>

#include "graph/storage/relocatable/maintenance/LargeValueMaintenanceJournal.hpp"
#include "graph/utils/ChecksumUtils.hpp"

using namespace graph::storage;

namespace {

	constexpr size_t PHASE_OFFSET = 8U;
	constexpr size_t MANIFEST_SCAN_EXHAUSTED_OFFSET = 9U;
	constexpr size_t CANDIDATE_COUNT_OFFSET = 44U;
	constexpr size_t REACHABLE_COUNT_OFFSET = 48U;
	constexpr size_t PAYLOAD_CRC_OFFSET = 52U;
	constexpr size_t HEADER_CRC_OFFSET = 56U;
	constexpr size_t RESERVED_OFFSET = 60U;

	void expectFormatError(const std::function<void()> &operation, FormatError expected) {
		try {
			operation();
			FAIL() << "Expected FormatException";
		} catch (const FormatException &error) {
			EXPECT_EQ(error.error(), expected);
		}
	}

	LargeValueMaintenanceJournal journal(LargeValueMaintenanceJournalPhase phase,
												std::vector<LargeValueId> candidates = {},
												std::vector<LargeValueId> reachable = {}) {
		LargeValueMaintenanceJournal result;
		result.scan_generation = Generation(7U);
		result.phase = phase;
		result.manifest_scan_exhausted = phase != LargeValueMaintenanceJournalPhase::LVMJP_ENUMERATE_MANIFESTS;
		result.manifest_after_value_id = phase == LargeValueMaintenanceJournalPhase::LVMJP_ENUMERATE_MANIFESTS ? 0U : 19U;
		result.entity_property_after_directory_id =
				phase == LargeValueMaintenanceJournalPhase::LVMJP_ENUMERATE_MANIFESTS ? 0U : 23U;
		result.state_after_id = phase == LargeValueMaintenanceJournalPhase::LVMJP_ENUMERATE_MANIFESTS ? 0U : 29U;
		result.candidate_value_ids = std::move(candidates);
		result.reachable_value_ids = std::move(reachable);
		return result;
	}

	void rewriteHeaderCrc(std::vector<std::byte> &bytes) {
		LittleEndianCodec::writeU32(bytes, HEADER_CRC_OFFSET, 0U);
		const uint32_t headerCrc = graph::utils::calculateCrc(bytes.data(), RSS_LARGE_VALUE_MAINTENANCE_JOURNAL_HEADER_BYTES);
		LittleEndianCodec::writeU32(bytes, HEADER_CRC_OFFSET, headerCrc);
	}

	void rewritePayloadAndHeaderCrc(std::vector<std::byte> &bytes) {
		const std::span<const std::byte> payload =
				std::span(bytes).subspan(RSS_LARGE_VALUE_MAINTENANCE_JOURNAL_HEADER_BYTES);
		LittleEndianCodec::writeU32(bytes, PAYLOAD_CRC_OFFSET,
								 graph::utils::calculateCrc(payload.data(), payload.size()));
		rewriteHeaderCrc(bytes);
	}

} // namespace

TEST(LargeValueMaintenanceJournalCodecTest, RoundTripsEachValidDurablePhase) {
	const std::array<LargeValueMaintenanceJournal, 4U> journals{
			journal(LargeValueMaintenanceJournalPhase::LVMJP_ENUMERATE_MANIFESTS),
			journal(LargeValueMaintenanceJournalPhase::LVMJP_SCAN_ENTITY_PROPERTIES, {LargeValueId(3U), LargeValueId(5U)}),
			journal(LargeValueMaintenanceJournalPhase::LVMJP_SCAN_STATES, {LargeValueId(3U), LargeValueId(5U)},
					{LargeValueId(3U)}),
			journal(LargeValueMaintenanceJournalPhase::LVMJP_RECLAIM, {LargeValueId(3U), LargeValueId(5U)},
					{LargeValueId(3U), LargeValueId(5U)}),
	};

	for (const LargeValueMaintenanceJournal &expected: journals) {
		const std::vector<std::byte> encoded = LargeValueMaintenanceJournalCodec::encode(expected);
		EXPECT_EQ(encoded.size(), RSS_LARGE_VALUE_MAINTENANCE_JOURNAL_HEADER_BYTES +
									 RSS_LARGE_VALUE_MAINTENANCE_JOURNAL_MAX_IDS * sizeof(uint64_t) * 2U);
		EXPECT_EQ(LargeValueMaintenanceJournalCodec::decode(encoded), expected);
	}
}

TEST(LargeValueMaintenanceJournalCodecTest, RejectsInvalidInMemoryEvidenceBeforeItCanBePersisted) {
	LargeValueMaintenanceJournal invalidGeneration = journal(LargeValueMaintenanceJournalPhase::LVMJP_ENUMERATE_MANIFESTS);
	invalidGeneration.scan_generation = Generation{};
	expectFormatError([&] { static_cast<void>(LargeValueMaintenanceJournalCodec::encode(invalidGeneration)); },
					  FormatError::FERR_INVALID_HEADER);

	LargeValueMaintenanceJournal invalidPhase = journal(LargeValueMaintenanceJournalPhase::LVMJP_ENUMERATE_MANIFESTS);
	invalidPhase.phase = static_cast<LargeValueMaintenanceJournalPhase>(255U);
	expectFormatError([&] { static_cast<void>(LargeValueMaintenanceJournalCodec::encode(invalidPhase)); },
					  FormatError::FERR_INVALID_HEADER);

	LargeValueMaintenanceJournal duplicateCandidates =
			journal(LargeValueMaintenanceJournalPhase::LVMJP_SCAN_ENTITY_PROPERTIES, {LargeValueId(3U), LargeValueId(3U)});
	expectFormatError([&] { static_cast<void>(LargeValueMaintenanceJournalCodec::encode(duplicateCandidates)); },
					  FormatError::FERR_INVALID_HEADER);

	LargeValueMaintenanceJournal zeroCandidate =
			journal(LargeValueMaintenanceJournalPhase::LVMJP_SCAN_ENTITY_PROPERTIES, {LargeValueId{}});
	expectFormatError([&] { static_cast<void>(LargeValueMaintenanceJournalCodec::encode(zeroCandidate)); },
					  FormatError::FERR_INVALID_HEADER);

	LargeValueMaintenanceJournal nonCandidateReachable =
			journal(LargeValueMaintenanceJournalPhase::LVMJP_SCAN_STATES, {LargeValueId(3U)}, {LargeValueId(5U)});
	expectFormatError([&] { static_cast<void>(LargeValueMaintenanceJournalCodec::encode(nonCandidateReachable)); },
					  FormatError::FERR_INVALID_HEADER);

	LargeValueMaintenanceJournal manifestEvidence =
			journal(LargeValueMaintenanceJournalPhase::LVMJP_ENUMERATE_MANIFESTS, {LargeValueId(3U)});
	expectFormatError([&] { static_cast<void>(LargeValueMaintenanceJournalCodec::encode(manifestEvidence)); },
					  FormatError::FERR_INVALID_HEADER);

	LargeValueMaintenanceJournal ownerWithoutEvidence =
			journal(LargeValueMaintenanceJournalPhase::LVMJP_RECLAIM);
	expectFormatError([&] { static_cast<void>(LargeValueMaintenanceJournalCodec::encode(ownerWithoutEvidence)); },
					  FormatError::FERR_INVALID_HEADER);

	std::vector<LargeValueId> tooManyCandidates;
	tooManyCandidates.reserve(RSS_LARGE_VALUE_MAINTENANCE_JOURNAL_MAX_IDS + 1U);
	for (uint64_t value = 1U; value <= RSS_LARGE_VALUE_MAINTENANCE_JOURNAL_MAX_IDS + 1U; ++value) {
		tooManyCandidates.emplace_back(value);
	}
	LargeValueMaintenanceJournal excessiveCandidates =
			journal(LargeValueMaintenanceJournalPhase::LVMJP_SCAN_ENTITY_PROPERTIES, std::move(tooManyCandidates));
	expectFormatError([&] { static_cast<void>(LargeValueMaintenanceJournalCodec::encode(excessiveCandidates)); },
					  FormatError::FERR_INVALID_HEADER);

	LargeValueMaintenanceJournal manifestReachable = journal(LargeValueMaintenanceJournalPhase::LVMJP_ENUMERATE_MANIFESTS);
	manifestReachable.reachable_value_ids = {LargeValueId(3U)};
	expectFormatError([&] { static_cast<void>(LargeValueMaintenanceJournalCodec::encode(manifestReachable)); },
					  FormatError::FERR_INVALID_HEADER);

	LargeValueMaintenanceJournal manifestOwnerCursor =
			journal(LargeValueMaintenanceJournalPhase::LVMJP_ENUMERATE_MANIFESTS);
	manifestOwnerCursor.entity_property_after_directory_id = 1U;
	expectFormatError([&] { static_cast<void>(LargeValueMaintenanceJournalCodec::encode(manifestOwnerCursor)); },
					  FormatError::FERR_INVALID_HEADER);

	LargeValueMaintenanceJournal manifestStateCursor = journal(LargeValueMaintenanceJournalPhase::LVMJP_ENUMERATE_MANIFESTS);
	manifestStateCursor.state_after_id = 1U;
	expectFormatError([&] { static_cast<void>(LargeValueMaintenanceJournalCodec::encode(manifestStateCursor)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST(LargeValueMaintenanceJournalCodecTest, RejectsDamagedAndNoncanonicalDurableRecords) {
	const LargeValueMaintenanceJournal source =
			journal(LargeValueMaintenanceJournalPhase::LVMJP_SCAN_STATES, {LargeValueId(3U)}, {LargeValueId(3U)});
	const std::vector<std::byte> encoded = LargeValueMaintenanceJournalCodec::encode(source);

	expectFormatError([&] { static_cast<void>(LargeValueMaintenanceJournalCodec::decode({})); },
					  FormatError::FERR_BUFFER_TOO_SMALL);

	std::vector<std::byte> damagedHeader = encoded;
	damagedHeader[0] ^= std::byte{1U};
	expectFormatError([&] { static_cast<void>(LargeValueMaintenanceJournalCodec::decode(damagedHeader)); },
					  FormatError::FERR_CHECKSUM_MISMATCH);

	std::vector<std::byte> invalidFixedFields = encoded;
	invalidFixedFields[MANIFEST_SCAN_EXHAUSTED_OFFSET] = std::byte{2U};
	LittleEndianCodec::writeU32(invalidFixedFields, RESERVED_OFFSET, 1U);
	rewriteHeaderCrc(invalidFixedFields);
	expectFormatError([&] { static_cast<void>(LargeValueMaintenanceJournalCodec::decode(invalidFixedFields)); },
					  FormatError::FERR_INVALID_HEADER);

	std::vector<std::byte> invalidReservedPadding = encoded;
	invalidReservedPadding[10U] = std::byte{1U};
	invalidReservedPadding[11U] = std::byte{1U};
	rewriteHeaderCrc(invalidReservedPadding);
	expectFormatError([&] { static_cast<void>(LargeValueMaintenanceJournalCodec::decode(invalidReservedPadding)); },
					  FormatError::FERR_INVALID_HEADER);

	std::vector<std::byte> invalidPhase = encoded;
	invalidPhase[PHASE_OFFSET] = std::byte{255U};
	rewriteHeaderCrc(invalidPhase);
	expectFormatError([&] { static_cast<void>(LargeValueMaintenanceJournalCodec::decode(invalidPhase)); },
					  FormatError::FERR_INVALID_HEADER);

	std::vector<std::byte> excessiveEvidence = encoded;
	LittleEndianCodec::writeU32(excessiveEvidence, CANDIDATE_COUNT_OFFSET,
									static_cast<uint32_t>(RSS_LARGE_VALUE_MAINTENANCE_JOURNAL_MAX_IDS + 1U));
	LittleEndianCodec::writeU32(excessiveEvidence, REACHABLE_COUNT_OFFSET,
									static_cast<uint32_t>(RSS_LARGE_VALUE_MAINTENANCE_JOURNAL_MAX_IDS + 1U));
	rewriteHeaderCrc(excessiveEvidence);
	expectFormatError([&] { static_cast<void>(LargeValueMaintenanceJournalCodec::decode(excessiveEvidence)); },
					  FormatError::FERR_INVALID_HEADER);

	std::vector<std::byte> excessiveReachableEvidence = encoded;
	LittleEndianCodec::writeU32(excessiveReachableEvidence, REACHABLE_COUNT_OFFSET,
									static_cast<uint32_t>(RSS_LARGE_VALUE_MAINTENANCE_JOURNAL_MAX_IDS + 1U));
	rewriteHeaderCrc(excessiveReachableEvidence);
	expectFormatError([&] { static_cast<void>(LargeValueMaintenanceJournalCodec::decode(excessiveReachableEvidence)); },
					  FormatError::FERR_INVALID_HEADER);

	std::vector<std::byte> damagedPayload = encoded;
	damagedPayload[RSS_LARGE_VALUE_MAINTENANCE_JOURNAL_HEADER_BYTES] ^= std::byte{1U};
	expectFormatError([&] { static_cast<void>(LargeValueMaintenanceJournalCodec::decode(damagedPayload)); },
					  FormatError::FERR_CHECKSUM_MISMATCH);

	std::vector<std::byte> noncanonicalUnusedEvidence = encoded;
	const size_t firstUnusedCandidate = RSS_LARGE_VALUE_MAINTENANCE_JOURNAL_HEADER_BYTES + sizeof(uint64_t);
	noncanonicalUnusedEvidence[firstUnusedCandidate] = std::byte{1U};
	rewritePayloadAndHeaderCrc(noncanonicalUnusedEvidence);
	expectFormatError(
			[&] { static_cast<void>(LargeValueMaintenanceJournalCodec::decode(noncanonicalUnusedEvidence)); },
			FormatError::FERR_INVALID_HEADER);

	std::vector<std::byte> noncanonicalUnusedReachableEvidence = encoded;
	const size_t firstUnusedReachable = RSS_LARGE_VALUE_MAINTENANCE_JOURNAL_HEADER_BYTES +
			RSS_LARGE_VALUE_MAINTENANCE_JOURNAL_MAX_IDS * sizeof(uint64_t) + sizeof(uint64_t);
	noncanonicalUnusedReachableEvidence[firstUnusedReachable] = std::byte{1U};
	rewritePayloadAndHeaderCrc(noncanonicalUnusedReachableEvidence);
	expectFormatError(
			[&] { static_cast<void>(LargeValueMaintenanceJournalCodec::decode(noncanonicalUnusedReachableEvidence)); },
			FormatError::FERR_INVALID_HEADER);
}
