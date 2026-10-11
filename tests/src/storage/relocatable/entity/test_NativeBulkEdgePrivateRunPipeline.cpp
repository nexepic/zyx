/**
 * @file test_NativeBulkEdgePrivateRunPipeline.cpp
 * @brief Bounded private multi-pass streamed Edge run-pipeline tests.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#include <optional>
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
