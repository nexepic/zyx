/**
 * @file test_NativeBulkEdgePrivateRunMaterializer.cpp
 * @brief Private candidate persistence tests for streamed Edge-derived runs.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#include <optional>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "graph/storage/relocatable/entity/NativeBulkEdgePrivateRunMaterializer.hpp"

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
				return ordinal == runs.size()
							   ? std::nullopt
							   : std::optional<OrderedBTreeEntrySource>(orderedSource(std::move(runs[ordinal++])));
			};
		};
	}

	struct PrivateCandidates {
		NativeGraphPrivateSegmentSet identities{SegmentId(100U), Generation(7U), 512U, 88U};
		size_t next_ordinal = 0U;
		std::vector<std::vector<std::byte>> payloads;

		NativeGraphPrivateSegmentMaterialization allocate() {
			if (next_ordinal == identities.size()) {
				throw FormatException(FormatError::FERR_INVALID_STATE, "candidate test exhausted its identity range");
			}
			return identities[next_ordinal++];
		}

		void write(const NativeGraphPrivateSegmentMaterialization &identity, std::vector<std::byte> payload) {
			EXPECT_EQ(identities.ordinalOf(identity), payloads.size());
			payloads.push_back(std::move(payload));
		}

		[[nodiscard]] NativeBulkRunPayloadReader reader() const {
			return [this](const NativeBulkRunFragmentLocation &location) {
				return payloads.at(
						identities.ordinalOf({location.segment_id, location.generation, location.transaction_id}));
			};
		}
	};

	NativeBulkEdgePrivateRunMaterializationOptions options(size_t maximumEntries = 32U) {
		return {2U, RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES, maximumEntries, 4U,
				NativeBulkEdgeRunDuplicatePolicy::NBERDP_REJECT};
	}

} // namespace

TEST(NativeBulkEdgePrivateRunMaterializerTest, PersistsEachLogicalOutputAsOneRewindablePrivateManifest) {
	PrivateCandidates candidates;
	std::vector<NativeBulkRunFragmentLocation> roots;
	NativeBulkEdgePrivateRunMaterializer::materializeOneLevel(
			source({{entry(0x10U, 1U)}, {entry(0x20U, 2U)}, {entry(0x30U, 3U)}}), options(),
			[&] { return candidates.allocate(); },
			[&](const NativeGraphPrivateSegmentMaterialization &identity, std::vector<std::byte> payload) {
				candidates.write(identity, std::move(payload));
			},
			[&](NativeBulkRunFragmentLocation root) { roots.push_back(root); });

	ASSERT_EQ(roots.size(), 2U);
	ASSERT_EQ(candidates.payloads.size(), 4U);
	std::vector<uint8_t> keys;
	for (const NativeBulkRunFragmentLocation &root: roots) {
		OrderedBTreeEntryCursor cursor =
				NativeBulkRunStore::orderedSourceFromPayloadReader(root, 4U, candidates.reader())();
		while (const std::optional<BTreePageEntry> next = cursor()) {
			keys.push_back(std::to_integer<uint8_t>(next->key.front()));
		}
	}
	EXPECT_EQ(keys, (std::vector<uint8_t>{0x10U, 0x20U, 0x30U}));
}

TEST(NativeBulkEdgePrivateRunMaterializerTest, KeepsSplitFragmentsUnderOneManifestAndRejectsCandidateOwnerMismatch) {
	PrivateCandidates candidates;
	const BTreePageEntry first = entry(0x10U, 1U);
	const size_t oneEntryBytes = RSS_NATIVE_BULK_RUN_HEADER_BYTES + RSS_NATIVE_BULK_RUN_RECORD_HEADER_BYTES +
								 BTreePageEntryCodec::encode(first).size();
	auto splitOptions = options();
	splitOptions.maximum_fragment_bytes = oneEntryBytes;
	std::vector<NativeBulkRunFragmentLocation> roots;
	NativeBulkEdgePrivateRunMaterializer::materializeOneLevel(
			source({{first, entry(0x20U, 2U)}}), splitOptions, [&] { return candidates.allocate(); },
			[&](const NativeGraphPrivateSegmentMaterialization &identity, std::vector<std::byte> payload) {
				candidates.write(identity, std::move(payload));
			},
			[&](NativeBulkRunFragmentLocation root) { roots.push_back(root); });
	ASSERT_EQ(roots.size(), 1U);
	ASSERT_EQ(candidates.payloads.size(), 3U);
	OrderedBTreeEntryCursor cursor =
			NativeBulkRunStore::orderedSourceFromPayloadReader(roots.front(), 4U, candidates.reader())();
	ASSERT_TRUE(cursor().has_value());
	ASSERT_TRUE(cursor().has_value());
	EXPECT_FALSE(cursor().has_value());

	PrivateCandidates ownerMismatch;
	size_t allocationCount = 0U;
	EXPECT_THROW(NativeBulkEdgePrivateRunMaterializer::materializeOneLevel(
						 source({{entry(0x10U, 1U)}}), options(),
						 [&] {
							 NativeGraphPrivateSegmentMaterialization identity = ownerMismatch.allocate();
							 if (++allocationCount == 2U) {
								 identity.owner_id = 89U;
							 }
							 return identity;
						 },
						 [&](const NativeGraphPrivateSegmentMaterialization &identity, std::vector<std::byte> payload) {
							 ownerMismatch.write(identity, std::move(payload));
						 },
						 [](NativeBulkRunFragmentLocation) {}),
				 FormatException);
}

TEST(NativeBulkEdgePrivateRunMaterializerTest, CoalescesTokenDuplicatesOnlyWhenThePolicyAllowsIt) {
	PrivateCandidates candidates;
	std::vector<NativeBulkRunFragmentLocation> roots;
	auto tokenOptions = options();
	tokenOptions.duplicate_policy = NativeBulkEdgeRunDuplicatePolicy::NBERDP_KEEP_ONE;
	NativeBulkEdgePrivateRunMaterializer::materializeOneLevel(
			source({{entry(0x10U, 1U)}, {entry(0x10U, 2U), entry(0x20U, 3U)}}), tokenOptions,
			[&] { return candidates.allocate(); },
			[&](const NativeGraphPrivateSegmentMaterialization &identity, std::vector<std::byte> payload) {
				candidates.write(identity, std::move(payload));
			},
			[&](NativeBulkRunFragmentLocation root) { roots.push_back(root); });
	ASSERT_EQ(roots.size(), 1U);
	OrderedBTreeEntryCursor cursor =
			NativeBulkRunStore::orderedSourceFromPayloadReader(roots.front(), 4U, candidates.reader())();
	std::vector<uint8_t> keys;
	while (const std::optional<BTreePageEntry> next = cursor()) {
		keys.push_back(std::to_integer<uint8_t>(next->key.front()));
	}
	EXPECT_EQ(keys, (std::vector<uint8_t>{0x10U, 0x20U}));
}
