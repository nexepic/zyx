/**
 * @file test_NativeBulkEdgeAdjacencyRunBuilder.cpp
 * @brief Tests for bounded sorted adjacency runs from Edge scratch.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "graph/storage/relocatable/entity/NativeBulkEdgeAdjacencyRunBuilder.hpp"

namespace graph::storage {

	namespace {

		[[nodiscard]] NativeBulkEdgeRecord record(uint64_t edgeId, uint64_t source, uint64_t target) {
			return {edgeId, source, target, "", {}};
		}

		[[nodiscard]] NativeBulkEdgeRecordSource source(std::vector<NativeBulkEdgeRecord> records) {
			return [records = std::move(records)] {
				return [records, ordinal = size_t{0U}]() mutable -> std::optional<NativeBulkEdgeRecord> {
					if (ordinal == records.size()) {
						return std::nullopt;
					}
					return records[ordinal++];
				};
			};
		}

		[[nodiscard]] size_t runBytesForOneEntry() {
		const BTreePageEntry entry{AdjacencyKeyCodec::encode({GlobalAdjacencyDirection::GAD_OUT, 1U, 1U}), 1U};
			return RSS_NATIVE_BULK_RUN_HEADER_BYTES + RSS_NATIVE_BULK_RUN_RECORD_HEADER_BYTES +
				   BTreePageEntryCodec::encode(entry).size();
		}

	} // namespace

	TEST(NativeBulkEdgeAdjacencyRunBuilderTest, SortsEachDirectionAndFlushesAtTheConfiguredRunBound) {
		const NativeGraphLogicalIdLeaseRange lease{100U, 3U};
		const std::vector<NativeBulkEdgeRecord> records{
				record(100U, 9U, 4U), record(101U, 2U, 8U), record(102U, 5U, 3U)};
		std::vector<NativeBulkRunFragment> outgoing;
		NativeBulkEdgeAdjacencyRunBuilder::buildRuns(
				source(records), std::span(&lease, 1U), records.size(), runBytesForOneEntry(), records.size(),
				GlobalAdjacencyDirection::GAD_OUT,
				[&outgoing](NativeBulkRunFragment &&run) { outgoing.push_back(std::move(run)); });
		ASSERT_EQ(outgoing.size(), records.size());
		for (const NativeBulkRunFragment &run: outgoing) {
			ASSERT_EQ(run.entries.size(), 1U);
			const GlobalAdjacencyKey key = AdjacencyKeyCodec::decode(run.entries.front().key);
			EXPECT_EQ(key.direction, GlobalAdjacencyDirection::GAD_OUT);
			EXPECT_EQ(run.entries.front().value_or_child, 1U);
		}

		std::vector<NativeBulkRunFragment> incoming;
		NativeBulkEdgeAdjacencyRunBuilder::buildRuns(
				source(records), std::span(&lease, 1U), records.size(), runBytesForOneEntry() * 4U, records.size(),
				GlobalAdjacencyDirection::GAD_IN,
				[&incoming](NativeBulkRunFragment &&run) { incoming.push_back(std::move(run)); });
		ASSERT_EQ(incoming.size(), 1U);
		ASSERT_EQ(incoming.front().entries.size(), records.size());
		std::vector<GlobalAdjacencyKey> keys;
		for (const BTreePageEntry &entry: incoming.front().entries) {
			EXPECT_EQ(entry.value_or_child, 1U);
			keys.push_back(AdjacencyKeyCodec::decode(entry.key));
		}
		EXPECT_EQ(keys[0].node_id, 3U);
		EXPECT_EQ(keys[1].node_id, 4U);
		EXPECT_EQ(keys[2].node_id, 8U);
	}

	TEST(NativeBulkEdgeAdjacencyRunBuilderTest, RejectsUnleasedAndOversizedRunEntries) {
		const NativeGraphLogicalIdLeaseRange lease{100U, 1U};
		EXPECT_THROW(NativeBulkEdgeAdjacencyRunBuilder::buildRuns(
						 source({record(101U, 1U, 2U)}), std::span(&lease, 1U), 1U, 1024U, 1U,
						 GlobalAdjacencyDirection::GAD_OUT, [](NativeBulkRunFragment &&) {}),
				 FormatException);
		EXPECT_THROW(NativeBulkEdgeAdjacencyRunBuilder::buildRuns(
						 source({record(100U, 1U, 2U)}), std::span(&lease, 1U), 1U,
						 RSS_NATIVE_BULK_RUN_HEADER_BYTES + 1U, 1U, GlobalAdjacencyDirection::GAD_OUT,
						 [](NativeBulkRunFragment &&) {}),
				 FormatException);
	}

	TEST(NativeBulkEdgeAdjacencyRunBuilderTest, RejectsMalformedContractsBeforeOpeningTheSource) {
		const NativeGraphLogicalIdLeaseRange validLease{100U, 1U};
		const auto visitor = [](NativeBulkRunFragment &&) {};
		size_t sourceOpens = 0U;
		const NativeBulkEdgeRecordSource countedSource = [&sourceOpens] {
			++sourceOpens;
			return []() -> std::optional<NativeBulkEdgeRecord> { return std::nullopt; };
		};

		const auto expectInvalid = [&](const NativeBulkEdgeRecordSource &input,
									 std::span<const NativeGraphLogicalIdLeaseRange> leases, size_t maximumRecords,
									 size_t maximumRunBytes, size_t maximumFragments,
									 GlobalAdjacencyDirection direction,
									 const NativeBulkEdgeAdjacencyRunVisitor &runVisitor) {
			EXPECT_THROW(NativeBulkEdgeAdjacencyRunBuilder::buildRuns(
						 input, leases, maximumRecords, maximumRunBytes, maximumFragments, direction, runVisitor),
					 FormatException);
		};

		expectInvalid({}, std::span(&validLease, 1U), 1U, runBytesForOneEntry(), 1U,
					  GlobalAdjacencyDirection::GAD_OUT, visitor);
		const std::array<NativeGraphLogicalIdLeaseRange, 0U> noLeases{};
		expectInvalid(countedSource, noLeases, 1U, runBytesForOneEntry(), 1U, GlobalAdjacencyDirection::GAD_OUT,
					  visitor);
		const NativeGraphLogicalIdLeaseRange zeroFirst{0U, 1U};
		const NativeGraphLogicalIdLeaseRange zeroCount{100U, 0U};
		const NativeGraphLogicalIdLeaseRange overflowing{std::numeric_limits<uint64_t>::max(), 2U};
		expectInvalid(countedSource, std::span(&zeroFirst, 1U), 1U, runBytesForOneEntry(), 1U,
					  GlobalAdjacencyDirection::GAD_OUT, visitor);
		expectInvalid(countedSource, std::span(&zeroCount, 1U), 1U, runBytesForOneEntry(), 1U,
					  GlobalAdjacencyDirection::GAD_OUT, visitor);
		expectInvalid(countedSource, std::span(&overflowing, 1U), 1U, runBytesForOneEntry(), 1U,
					  GlobalAdjacencyDirection::GAD_OUT, visitor);
		const std::array overlapping{NativeGraphLogicalIdLeaseRange{100U, 2U}, NativeGraphLogicalIdLeaseRange{101U, 1U}};
		expectInvalid(countedSource, overlapping, 1U, runBytesForOneEntry(), 1U, GlobalAdjacencyDirection::GAD_OUT,
					  visitor);
		expectInvalid(countedSource, std::span(&validLease, 1U), 0U, runBytesForOneEntry(), 1U,
					  GlobalAdjacencyDirection::GAD_OUT, visitor);
		expectInvalid(countedSource, std::span(&validLease, 1U), 1U, RSS_NATIVE_BULK_RUN_HEADER_BYTES, 1U,
					  GlobalAdjacencyDirection::GAD_OUT, visitor);
		expectInvalid(countedSource, std::span(&validLease, 1U), 1U, runBytesForOneEntry(), 0U,
					  GlobalAdjacencyDirection::GAD_OUT, visitor);
		expectInvalid(countedSource, std::span(&validLease, 1U), std::numeric_limits<size_t>::max(),
					  runBytesForOneEntry(), 1U, GlobalAdjacencyDirection::GAD_OUT, visitor);
		expectInvalid(countedSource, std::span(&validLease, 1U), 1U, std::numeric_limits<size_t>::max(), 1U,
					  GlobalAdjacencyDirection::GAD_OUT, visitor);
		expectInvalid(countedSource, std::span(&validLease, 1U), 1U, runBytesForOneEntry(),
					  std::numeric_limits<size_t>::max(), GlobalAdjacencyDirection::GAD_OUT, visitor);
		expectInvalid(countedSource, std::span(&validLease, 1U), 1U, runBytesForOneEntry(), 1U,
					  static_cast<GlobalAdjacencyDirection>(255U), visitor);
		const NativeBulkEdgeAdjacencyRunVisitor noVisitor{};
		expectInvalid(countedSource, std::span(&validLease, 1U), 1U, runBytesForOneEntry(), 1U,
					  GlobalAdjacencyDirection::GAD_OUT, noVisitor);
		EXPECT_EQ(sourceOpens, 0U);

		// Once the shape is valid, an empty sealed source is a runtime failure;
		// it is intentionally distinct from the pre-open contract checks above.
		expectInvalid(countedSource, std::span(&validLease, 1U), 1U, runBytesForOneEntry(), 1U,
					  GlobalAdjacencyDirection::GAD_OUT, visitor);
		EXPECT_EQ(sourceOpens, 1U);

		const auto expectInvalidPair = [&](size_t maximumRecords, size_t maximumRunBytes, size_t maximumOutgoing,
									   size_t maximumIncoming) {
			EXPECT_THROW(NativeBulkEdgeAdjacencyRunBuilder::buildRunsPair(
						 countedSource, std::span(&validLease, 1U), maximumRecords, maximumRunBytes, maximumOutgoing,
						 maximumIncoming, visitor, visitor),
					 FormatException);
		};
		expectInvalidPair(std::numeric_limits<size_t>::max(), runBytesForOneEntry(), 1U, 1U);
		expectInvalidPair(1U, std::numeric_limits<size_t>::max(), 1U, 1U);
		expectInvalidPair(1U, runBytesForOneEntry(), std::numeric_limits<size_t>::max(), 1U);
		expectInvalidPair(1U, runBytesForOneEntry(), 1U, std::numeric_limits<size_t>::max());
		EXPECT_EQ(sourceOpens, 1U);
	}

	TEST(NativeBulkEdgeAdjacencyRunBuilderTest, EvaluatesEachPairedBuilderContractGuard) {
		const NativeGraphLogicalIdLeaseRange lease{100U, 1U};
		const auto visitor = [](NativeBulkRunFragment &&) {};
		const size_t runBytes = runBytesForOneEntry();
		const auto validSource = source({record(100U, 1U, 2U)});
		const auto expectInvalid = [&](const NativeBulkEdgeRecordSource &input, size_t maximumRecords,
									 size_t maximumRunBytes, size_t maximumOutgoing, size_t maximumIncoming,
									 const NativeBulkEdgeAdjacencyRunVisitor &outgoing,
									 const NativeBulkEdgeAdjacencyRunVisitor &incoming) {
			EXPECT_THROW(NativeBulkEdgeAdjacencyRunBuilder::buildRunsPair(
					input, std::span(&lease, 1U), maximumRecords, maximumRunBytes, maximumOutgoing, maximumIncoming,
					outgoing, incoming), FormatException);
		};

		// Keep every short-circuit operand independently observable.
		expectInvalid({}, 1U, runBytes, 1U, 1U, visitor, visitor);
		expectInvalid(validSource, 0U, runBytes, 1U, 1U, visitor, visitor);
		expectInvalid(validSource, 1U, RSS_NATIVE_BULK_RUN_HEADER_BYTES, 1U, 1U, visitor, visitor);
		expectInvalid(validSource, 1U, runBytes, 0U, 1U, visitor, visitor);
		expectInvalid(validSource, 1U, runBytes, 1U, 0U, visitor, visitor);
		expectInvalid(validSource, 1U, runBytes, 1U, 1U, {}, visitor);
		expectInvalid(validSource, 1U, runBytes, 1U, 1U, visitor, {});
	}

	TEST(NativeBulkEdgeAdjacencyRunBuilderTest, AdvancesAcrossSparseOrderedLeaseRanges) {
		const std::array leases{NativeGraphLogicalIdLeaseRange{100U, 2U}, NativeGraphLogicalIdLeaseRange{200U, 2U},
								NativeGraphLogicalIdLeaseRange{300U, 1U}};
		const std::vector<NativeBulkEdgeRecord> records{
				record(100U, 1U, 2U), record(101U, 2U, 3U), record(200U, 3U, 4U),
				record(201U, 4U, 5U), record(300U, 5U, 6U)};
		std::vector<NativeBulkRunFragment> fragments;
		NativeBulkEdgeAdjacencyRunBuilder::buildRuns(
				source(records), leases, records.size(), runBytesForOneEntry() * (records.size() + 1U), records.size(),
				GlobalAdjacencyDirection::GAD_OUT,
				[&fragments](NativeBulkRunFragment &&fragment) { fragments.push_back(std::move(fragment)); });
		ASSERT_EQ(fragments.size(), 1U);
		ASSERT_EQ(fragments.front().entries.size(), records.size());

		EXPECT_THROW(NativeBulkEdgeAdjacencyRunBuilder::buildRuns(
						 source({record(100U, 1U, 2U), record(150U, 2U, 3U)}), leases, 2U,
						 runBytesForOneEntry() * 3U, 2U, GlobalAdjacencyDirection::GAD_OUT,
						 [](NativeBulkRunFragment &&) {}),
				 FormatException);
	}

	TEST(NativeBulkEdgeAdjacencyRunBuilderTest, BuildsBothDirectionsFromOneSourcePass) {
		const std::array leases{NativeGraphLogicalIdLeaseRange{100U, 3U}};
		const std::vector<NativeBulkEdgeRecord> records{
				record(100U, 9U, 4U), record(101U, 2U, 8U), record(102U, 5U, 3U)};
		size_t sourceOpens = 0U;
		const NativeBulkEdgeRecordSource onePassSource = [&records, &sourceOpens] {
			++sourceOpens;
			return [&records, ordinal = size_t{0U}]() mutable -> std::optional<NativeBulkEdgeRecord> {
				return ordinal == records.size() ? std::nullopt : std::optional<NativeBulkEdgeRecord>(records[ordinal++]);
			};
		};
		std::vector<NativeBulkRunFragment> outgoing;
		std::vector<NativeBulkRunFragment> incoming;
		NativeBulkEdgeAdjacencyRunBuilder::buildRunsPair(
				onePassSource, leases, records.size(), runBytesForOneEntry() * 4U, records.size(), records.size(),
				[&outgoing](NativeBulkRunFragment &&fragment) { outgoing.push_back(std::move(fragment)); },
				[&incoming](NativeBulkRunFragment &&fragment) { incoming.push_back(std::move(fragment)); });
		EXPECT_EQ(sourceOpens, 1U);
		ASSERT_EQ(outgoing.size(), 1U);
		ASSERT_EQ(incoming.size(), 1U);
		EXPECT_EQ(outgoing.front().entries.size(), records.size());
		EXPECT_EQ(incoming.front().entries.size(), records.size());
	}

	// Crossing a physical segment boundary requires a capacity-sized input, so
	// keep this as an explicit scale gate rather than a regular unit test.
	TEST(DISABLED_NativeBulkEdgeAdjacencyRunBuilderScaleTest, SplitsOneLargeInMemorySortRunIntoSegmentBoundedFragments) {
		const BTreePageEntry sample{
				AdjacencyKeyCodec::encode({GlobalAdjacencyDirection::GAD_OUT, 1U, 1U}), 1U};
		const size_t entryBytes = RSS_NATIVE_BULK_RUN_RECORD_HEADER_BYTES + BTreePageEntryCodec::encode(sample).size();
		const size_t maximumFragmentPayload = RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES;
		const size_t recordsPerFragment =
				(maximumFragmentPayload - RSS_NATIVE_BULK_RUN_HEADER_BYTES) / entryBytes;
		ASSERT_GT(recordsPerFragment, 0U);

		std::vector<NativeBulkEdgeRecord> records;
		records.reserve(recordsPerFragment + 1U);
		for (size_t ordinal = 0U; ordinal <= recordsPerFragment; ++ordinal) {
			const uint64_t edgeId = 100U + ordinal;
			// The source is Edge-ID ordered while node IDs deliberately descend, so
			// the output must sort first and then split without losing key order.
			records.push_back(record(edgeId, recordsPerFragment - ordinal + 1U, 1U));
		}
		const NativeGraphLogicalIdLeaseRange lease{100U, static_cast<uint64_t>(records.size())};
		std::vector<NativeBulkRunFragment> fragments;
		NativeBulkEdgeAdjacencyRunBuilder::buildRuns(
				source(records), std::span(&lease, 1U), records.size(), maximumFragmentPayload * 2U, 2U,
				GlobalAdjacencyDirection::GAD_OUT,
				[&fragments](NativeBulkRunFragment &&fragment) { fragments.push_back(std::move(fragment)); });

		ASSERT_EQ(fragments.size(), 2U);
		std::vector<std::byte> previousKey;
		for (const NativeBulkRunFragment &fragment: fragments) {
			EXPECT_LE(NativeBulkRunFragmentCodec::encode(fragment).size(), maximumFragmentPayload);
			for (const BTreePageEntry &entry: fragment.entries) {
				EXPECT_EQ(entry.value_or_child, 1U);
				if (!previousKey.empty()) {
					EXPECT_LT(previousKey, entry.key);
				}
				previousKey = entry.key;
			}
		}
	}

	TEST(NativeBulkEdgeAdjacencyRunBuilderTest, RejectsAStreamThatExceedsTheCandidateFragmentBound) {
		const NativeGraphLogicalIdLeaseRange lease{100U, 2U};
		EXPECT_THROW(NativeBulkEdgeAdjacencyRunBuilder::buildRuns(
						 source({record(100U, 1U, 2U), record(101U, 3U, 4U)}), std::span(&lease, 1U), 2U,
						 runBytesForOneEntry(), 1U, GlobalAdjacencyDirection::GAD_OUT,
						 [](NativeBulkRunFragment &&) {}),
					 FormatException);
	}

	TEST(NativeBulkEdgeAdjacencyRunBuilderTest, RejectsNonCanonicalOrOverBoundSealedSources) {
		const NativeGraphLogicalIdLeaseRange lease{100U, 2U};
		const auto visitor = [](NativeBulkRunFragment &&) {};
		EXPECT_THROW(NativeBulkEdgeAdjacencyRunBuilder::buildRuns(
					 source({record(100U, 1U, 2U), record(100U, 3U, 4U)}), std::span(&lease, 1U), 2U,
					 runBytesForOneEntry() * 3U, 2U, GlobalAdjacencyDirection::GAD_OUT, visitor),
				 FormatException);
		EXPECT_THROW(NativeBulkEdgeAdjacencyRunBuilder::buildRuns(
					 source({record(100U, 1U, 2U), record(101U, 3U, 4U)}), std::span(&lease, 1U), 1U,
					 runBytesForOneEntry() * 3U, 2U, GlobalAdjacencyDirection::GAD_OUT, visitor),
				 FormatException);
	}

	TEST(NativeBulkEdgeAdjacencyRunBuilderTest, RejectsNonCanonicalOrOverBoundPairedSources) {
		const NativeGraphLogicalIdLeaseRange lease{100U, 2U};
		const auto visitor = [](NativeBulkRunFragment &&) {};
		const auto expectInvalid = [&](std::vector<NativeBulkEdgeRecord> records, size_t maximumRecords) {
			EXPECT_THROW(NativeBulkEdgeAdjacencyRunBuilder::buildRunsPair(
						 source(std::move(records)), std::span(&lease, 1U), maximumRecords, runBytesForOneEntry() * 3U,
						 2U, 2U, visitor, visitor),
					 FormatException);
		};

		// Pair construction has its own one-pass validation before it materializes
		// either direction, so cover each malformed sealed-stream shape directly.
		expectInvalid({record(0U, 1U, 2U)}, 1U);
		expectInvalid({record(100U, 1U, 2U), record(100U, 3U, 4U)}, 2U);
		expectInvalid({record(101U, 1U, 2U), record(100U, 3U, 4U)}, 2U);
		expectInvalid({record(100U, 1U, 2U), record(101U, 3U, 4U)}, 1U);
		expectInvalid({record(99U, 1U, 2U)}, 1U);
	}

	TEST(NativeBulkEdgeAdjacencyRunBuilderTest, BoundsAndValidatesPairedAdjacencyRuns) {
		const NativeGraphLogicalIdLeaseRange lease{100U, 2U};
		const std::vector<NativeBulkEdgeRecord> records{
				record(100U, 1U, 4U), record(101U, 2U, 3U)};
		std::vector<NativeBulkRunFragment> outgoing;
		std::vector<NativeBulkRunFragment> incoming;
		NativeBulkEdgeAdjacencyRunBuilder::buildRunsPair(
				source(records), std::span(&lease, 1U), records.size(), runBytesForOneEntry(), 2U, 2U,
				[&outgoing](NativeBulkRunFragment &&fragment) { outgoing.push_back(std::move(fragment)); },
				[&incoming](NativeBulkRunFragment &&fragment) { incoming.push_back(std::move(fragment)); });
		ASSERT_EQ(outgoing.size(), 2U);
		ASSERT_EQ(incoming.size(), 2U);

		const auto visitor = [](NativeBulkRunFragment &&) {};
		EXPECT_THROW(NativeBulkEdgeAdjacencyRunBuilder::buildRunsPair(
					 source(records), std::span(&lease, 1U), records.size(), runBytesForOneEntry(), 1U, 2U,
					 visitor, visitor),
				 FormatException);
		EXPECT_THROW(NativeBulkEdgeAdjacencyRunBuilder::buildRunsPair(
					 source({}), std::span(&lease, 1U), 1U, runBytesForOneEntry(), 1U, 1U, visitor, visitor),
				 FormatException);
		EXPECT_THROW(NativeBulkEdgeAdjacencyRunBuilder::buildRunsPair(
					 source({record(102U, 1U, 2U)}), std::span(&lease, 1U), 1U, runBytesForOneEntry(), 1U, 1U,
					 visitor, visitor),
				 FormatException);
		EXPECT_THROW(NativeBulkEdgeAdjacencyRunBuilder::buildRunsPair(
					 source({record(100U, 1U, 2U)}), std::span(&lease, 1U), 1U,
					 RSS_NATIVE_BULK_RUN_HEADER_BYTES + 1U, 1U, 1U, visitor, visitor),
				 FormatException);
	}

} // namespace graph::storage
