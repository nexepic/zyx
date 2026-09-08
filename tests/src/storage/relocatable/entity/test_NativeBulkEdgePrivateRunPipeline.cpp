/**
 * @file test_NativeBulkEdgePrivateRunPipeline.cpp
 * @brief Bounded private multi-pass streamed Edge run-pipeline tests.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#include <optional>
#include <limits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "graph/storage/relocatable/entity/NativeBulkEdgePrivateRunPipeline.hpp"

using namespace graph::storage;

namespace {

	BTreePageEntry entry(uint8_t key, uint64_t value) { return {{std::byte{key}}, value}; }

	OrderedBTreeEntrySource orderedSource(std::vector<BTreePageEntry> entries) {
		return [entries = std::move(entries)]() {
			size_t ordinal = 0U;
			return [entries, ordinal]() mutable -> std::optional<BTreePageEntry> {
				return ordinal == entries.size() ? std::nullopt : std::optional<BTreePageEntry>(entries[ordinal++]);
			};
		};
	}

	NativeBulkEdgeRunSource source(std::vector<std::vector<BTreePageEntry>> runs) {
		return [runs = std::move(runs)]() {
			size_t ordinal = 0U;
			return [runs, ordinal]() mutable -> std::optional<OrderedBTreeEntrySource> {
				return ordinal == runs.size() ? std::nullopt
										  : std::optional<OrderedBTreeEntrySource>(orderedSource(runs[ordinal++]));
			};
		};
	}

	struct PrivateCandidates {
		NativeGraphPrivateSegmentSet identities{SegmentId(100U), Generation(7U), 512U, 88U};
		std::vector<std::optional<std::vector<std::byte>>> payloads =
				std::vector<std::optional<std::vector<std::byte>>>(identities.size());

		void write(const NativeGraphPrivateSegmentMaterialization &identity, std::vector<std::byte> payload) {
			const size_t ordinal = identities.ordinalOf(identity);
			ASSERT_FALSE(payloads[ordinal].has_value());
			payloads[ordinal] = std::move(payload);
		}

		[[nodiscard]] NativeGraphPrivateSegmentReader reader() const {
			return [this](const NativeGraphPrivateSegmentMaterialization &identity) {
				const std::optional<std::vector<std::byte>> &payload = payloads.at(identities.ordinalOf(identity));
				if (!payload.has_value()) {
					throw FormatException(FormatError::FERR_INVALID_STATE, "candidate reader encountered reservation");
				}
				return *payload;
			};
		}
	};

	std::vector<uint8_t> replay(const NativeGraphPrivateSegmentSet &identities, NativeBulkRunFragmentLocation root,
								uint16_t maximumDepth, const NativeGraphPrivateSegmentReader &reader) {
		std::vector<uint8_t> result;
		OrderedBTreeEntryCursor cursor =
				NativeBulkRunStore::orderedSourceFromPayloadReader(root, maximumDepth,
						[&identities, &reader](const NativeBulkRunFragmentLocation &location) {
							return reader(identities[identities.ordinalOf(
									{location.segment_id, location.generation, location.transaction_id})]);
						})();
		while (const std::optional<BTreePageEntry> next = cursor()) {
			result.push_back(std::to_integer<uint8_t>(next->key.front()));
		}
		return result;
	}

} // namespace

TEST(NativeBulkEdgePrivateRunPipelineTest, ReducesMultiplePrivateLevelsInsideOneCandidateRange) {
	PrivateCandidates candidates;
	size_t fences = 0U;
	const NativeBulkEdgePrivateRunPipelineOptions options{
			2U, RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES, 16U, 4U, NativeBulkEdgeRunDuplicatePolicy::NBERDP_REJECT};
	const NativeBulkRunFragmentLocation root = NativeBulkEdgePrivateRunPipeline::materialize(
			candidates.identities, {0U, candidates.identities.size()},
			source({{entry(0x10U, 1U)}, {entry(0x20U, 2U)}, {entry(0x30U, 3U)}, {entry(0x40U, 4U)}}), options,
			candidates.reader(),
			[&](const NativeGraphPrivateSegmentMaterialization &identity, std::vector<std::byte> payload) {
				candidates.write(identity, std::move(payload));
			},
			[&] { ++fences; });

	EXPECT_EQ(replay(candidates.identities, root, options.maximum_manifest_depth, candidates.reader()),
			  (std::vector<uint8_t>{0x10U, 0x20U, 0x30U, 0x40U}));
	EXPECT_EQ(fences, 2U);
}

TEST(NativeBulkEdgePrivateRunPipelineTest, KeepsOneTokenDuplicateWhenRequested) {
	PrivateCandidates candidates;
	const NativeBulkEdgePrivateRunPipelineOptions options{
			2U, RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES, 16U, 4U, NativeBulkEdgeRunDuplicatePolicy::NBERDP_KEEP_ONE};
	const NativeBulkRunFragmentLocation root = NativeBulkEdgePrivateRunPipeline::materialize(
			candidates.identities, {0U, candidates.identities.size()},
			source({{entry(0x10U, 1U)}, {entry(0x10U, 2U), entry(0x20U, 3U)}}), options, candidates.reader(),
			[&](const NativeGraphPrivateSegmentMaterialization &identity, std::vector<std::byte> payload) {
				candidates.write(identity, std::move(payload));
			},
			[] {});

	EXPECT_EQ(replay(candidates.identities, root, options.maximum_manifest_depth, candidates.reader()),
			  (std::vector<uint8_t>{0x10U, 0x20U}));
}

TEST(NativeBulkEdgePrivateRunPipelineTest, RejectsInvalidFanInAndCandidateCountOverflow) {
	EXPECT_THROW(static_cast<void>(NativeBulkEdgePrivateRunPipeline::maximumCandidateSegments(1U, 1U)),
				 FormatException);
	EXPECT_EQ(NativeBulkEdgePrivateRunPipeline::maximumCandidateSegments(0U, 2U), 0U);
	EXPECT_THROW(static_cast<void>(NativeBulkEdgePrivateRunPipeline::maximumCandidateSegments(
				std::numeric_limits<size_t>::max(), 2U)),
				 FormatException);
	EXPECT_THROW(static_cast<void>(NativeBulkEdgePrivateRunPipeline::maximumCandidateSegments(
				std::numeric_limits<size_t>::max() / 3U, 2U)),
				 FormatException);
}

TEST(NativeBulkEdgePrivateRunPipelineTest, RejectsMalformedMaterializeCallbacksAndRanges) {
	PrivateCandidates candidates;
	const NativeBulkEdgePrivateRunPipelineOptions valid{
			2U, RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES, 16U, 4U, NativeBulkEdgeRunDuplicatePolicy::NBERDP_REJECT};
	const NativeBulkEdgeRunSource initial = source({{entry(0x10U, 1U)}});
	const NativeGraphPrivateSegmentWriter writer =
			[&](const NativeGraphPrivateSegmentMaterialization &identity, std::vector<std::byte> payload) {
				candidates.write(identity, std::move(payload));
			};
	const NativeGraphPrivateSegmentFence fence = [] {};

	EXPECT_THROW(static_cast<void>(NativeBulkEdgePrivateRunPipeline::materialize(
				candidates.identities, {0U, 1U}, {}, valid, candidates.reader(), writer, fence)),
				 FormatException);
	EXPECT_THROW(static_cast<void>(NativeBulkEdgePrivateRunPipeline::materialize(
				candidates.identities, {0U, 1U}, initial, valid, {}, writer, fence)),
				 FormatException);
	EXPECT_THROW(static_cast<void>(NativeBulkEdgePrivateRunPipeline::materialize(
				candidates.identities, {0U, 1U}, initial, valid, candidates.reader(), {}, fence)),
				 FormatException);
	EXPECT_THROW(static_cast<void>(NativeBulkEdgePrivateRunPipeline::materialize(
				candidates.identities, {0U, 1U}, initial, valid, candidates.reader(), writer, {})),
				 FormatException);
	EXPECT_THROW(static_cast<void>(NativeBulkEdgePrivateRunPipeline::materialize(
				candidates.identities, {1U, 0U}, initial, valid, candidates.reader(), writer, fence)),
				 FormatException);
	EXPECT_THROW(static_cast<void>(NativeBulkEdgePrivateRunPipeline::materialize(
				candidates.identities, {0U, candidates.identities.size() + 1U}, initial, valid,
				candidates.reader(), writer, fence)),
				 FormatException);
	EXPECT_THROW(static_cast<void>(NativeBulkEdgePrivateRunPipeline::materialize(
				candidates.identities, {candidates.identities.size() + 1U, 1U}, initial, valid,
				candidates.reader(), writer, fence)),
				 FormatException);

		NativeBulkEdgePrivateRunPipelineOptions invalid = valid;
		invalid.maximum_open_runs = 1U;
		EXPECT_THROW(static_cast<void>(NativeBulkEdgePrivateRunPipeline::materialize(
				candidates.identities, {0U, 1U}, initial, invalid, candidates.reader(), writer, fence)),
			 FormatException);
		invalid = valid;
		invalid.maximum_open_runs = std::numeric_limits<size_t>::max();
		EXPECT_THROW(static_cast<void>(NativeBulkEdgePrivateRunPipeline::materialize(
				candidates.identities, {0U, 1U}, initial, invalid, candidates.reader(), writer, fence)),
			 FormatException);
		invalid = valid;
	invalid.maximum_fragment_bytes = RSS_NATIVE_BULK_RUN_HEADER_BYTES;
	EXPECT_THROW(static_cast<void>(NativeBulkEdgePrivateRunPipeline::materialize(
				candidates.identities, {0U, 1U}, initial, invalid, candidates.reader(), writer, fence)),
				 FormatException);
	invalid = valid;
		invalid.maximum_entries = 0U;
	EXPECT_THROW(static_cast<void>(NativeBulkEdgePrivateRunPipeline::materialize(
				candidates.identities, {0U, 1U}, initial, invalid, candidates.reader(), writer, fence)),
			 FormatException);
		invalid = valid;
		invalid.maximum_entries = std::numeric_limits<size_t>::max();
		EXPECT_THROW(static_cast<void>(NativeBulkEdgePrivateRunPipeline::materialize(
				candidates.identities, {0U, 1U}, initial, invalid, candidates.reader(), writer, fence)),
			 FormatException);
	invalid = valid;
	invalid.maximum_manifest_depth = 0U;
	EXPECT_THROW(static_cast<void>(NativeBulkEdgePrivateRunPipeline::materialize(
				candidates.identities, {0U, 1U}, initial, invalid, candidates.reader(), writer, fence)),
				 FormatException);
		invalid = valid;
		invalid.maximum_fragment_bytes = RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES + 1U;
		EXPECT_THROW(static_cast<void>(NativeBulkEdgePrivateRunPipeline::materialize(
				candidates.identities, {0U, 1U}, initial, invalid, candidates.reader(), writer, fence)),
				 FormatException);
}

TEST(NativeBulkEdgePrivateRunPipelineTest, RejectsEmptyInitialFragmentStreams) {
	PrivateCandidates candidates;
	const NativeBulkEdgePrivateRunPipelineOptions options{
			2U, RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES, 16U, 4U, NativeBulkEdgeRunDuplicatePolicy::NBERDP_REJECT};
	const NativeGraphPrivateSegmentWriter writer =
			[&](const NativeGraphPrivateSegmentMaterialization &identity, std::vector<std::byte> payload) {
				candidates.write(identity, std::move(payload));
			};
	const NativeGraphPrivateSegmentFence fence = [] {};
	EXPECT_THROW(static_cast<void>(NativeBulkEdgePrivateRunPipeline::materializeInitialFragments(
				candidates.identities, {0U, 4U}, {}, options, candidates.reader(), writer, fence)),
				 FormatException);
	EXPECT_THROW(static_cast<void>(NativeBulkEdgePrivateRunPipeline::materializeInitialFragments(
				candidates.identities, {0U, 4U}, [](const auto &) {}, options, candidates.reader(), writer, fence)),
				 FormatException);
	EXPECT_THROW(static_cast<void>(NativeBulkEdgePrivateRunPipeline::materializeInitialFragments(
				candidates.identities, {0U, 4U},
				[](const auto &visit) { visit(NativeBulkRunFragment{}); }, options, candidates.reader(), writer, fence)),
				 FormatException);
}

TEST(NativeBulkEdgePrivateRunPipelineTest, RejectsMalformedPairedInitialFragmentStreams) {
	PrivateCandidates candidates;
	const NativeBulkEdgePrivateRunPipelineOptions options{
			2U, RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES, 16U, 4U, NativeBulkEdgeRunDuplicatePolicy::NBERDP_REJECT};
	const NativeGraphPrivateSegmentWriter writer =
			[&](const NativeGraphPrivateSegmentMaterialization &identity, std::vector<std::byte> payload) {
				candidates.write(identity, std::move(payload));
			};
	const NativeGraphPrivateSegmentFence fence = [] {};
	EXPECT_THROW(static_cast<void>(NativeBulkEdgePrivateRunPipeline::materializeInitialFragmentPair(
				candidates.identities, {0U, 2U}, {2U, 2U}, {}, options, options, candidates.reader(), writer, fence)),
				 FormatException);
	EXPECT_THROW(static_cast<void>(NativeBulkEdgePrivateRunPipeline::materializeInitialFragmentPair(
				candidates.identities, {0U, 3U}, {2U, 2U}, [](const auto &, const auto &) {}, options, options,
				candidates.reader(), writer, fence)),
				 FormatException);
	EXPECT_THROW(static_cast<void>(NativeBulkEdgePrivateRunPipeline::materializeInitialFragmentPair(
				candidates.identities, {0U, 2U}, {2U, 2U},
				[](const auto &first, const auto &) { first(NativeBulkRunFragment{}); }, options, options,
				candidates.reader(), writer, fence)),
				 FormatException);
	EXPECT_THROW(static_cast<void>(NativeBulkEdgePrivateRunPipeline::materializeInitialFragmentPair(
				candidates.identities, {0U, 2U}, {2U, 2U},
				[](const auto &, const auto &second) { second(NativeBulkRunFragment{}); }, options, options,
				candidates.reader(), writer, fence)),
				 FormatException);

	PrivateCandidates successful;
	const NativeGraphPrivateSegmentWriter successfulWriter =
			[&](const NativeGraphPrivateSegmentMaterialization &identity, std::vector<std::byte> payload) {
				successful.write(identity, std::move(payload));
			};
	const auto pairResult = NativeBulkEdgePrivateRunPipeline::materializeInitialFragmentPair(
			successful.identities, {0U, 8U}, {8U, 8U},
		[](const auto &first, const auto &second) {
				NativeBulkRunFragment firstFragment;
				firstFragment.entries.push_back(entry(0x10U, 1U));
				first(std::move(firstFragment));
				NativeBulkRunFragment secondFragment;
				secondFragment.entries.push_back(entry(0x20U, 2U));
				second(std::move(secondFragment));
		},
		options, options, successful.reader(), successfulWriter, fence);
	EXPECT_TRUE(pairResult.first.isValid());
	EXPECT_TRUE(pairResult.second.isValid());

	PrivateCandidates missingSecondCandidates;
	const NativeGraphPrivateSegmentWriter missingSecondWriter =
			[&](const NativeGraphPrivateSegmentMaterialization &identity, std::vector<std::byte> payload) {
				missingSecondCandidates.write(identity, std::move(payload));
			};
	EXPECT_THROW(static_cast<void>(NativeBulkEdgePrivateRunPipeline::materializeInitialFragmentPair(
				missingSecondCandidates.identities, {0U, 4U}, {4U, 4U},
				[](const auto &first, const auto &) {
					NativeBulkRunFragment fragment;
					fragment.entries.push_back(entry(0x10U, 1U));
					first(std::move(fragment));
				},
				options, options, missingSecondCandidates.reader(), missingSecondWriter, fence)),
				 FormatException);

	// A producer that emits more fragments than its immutable range must fail
	// before it can overwrite a previously materialized candidate.
	PrivateCandidates overflowCandidates;
	const NativeGraphPrivateSegmentWriter overflowWriter =
			[&](const NativeGraphPrivateSegmentMaterialization &identity, std::vector<std::byte> payload) {
				overflowCandidates.write(identity, std::move(payload));
			};
	EXPECT_THROW(static_cast<void>(NativeBulkEdgePrivateRunPipeline::materializeInitialFragments(
				overflowCandidates.identities, {0U, 1U},
				[](const auto &visit) {
					NativeBulkRunFragment first;
					first.entries.push_back(entry(0x10U, 1U));
					visit(std::move(first));
					NativeBulkRunFragment second;
					second.entries.push_back(entry(0x20U, 2U));
					visit(std::move(second));
				},
				options, overflowCandidates.reader(), overflowWriter, fence)),
				 FormatException);
}
