/**
 * @file test_NativeBulkEdgePrivateRunSet.cpp
 * @brief Bounded private run-set tests for streamed Edge materialization.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#include <optional>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "graph/storage/relocatable/entity/NativeBulkEdgePrivateRunSet.hpp"
#include "graph/utils/ChecksumUtils.hpp"

using namespace graph::storage;

namespace {

	BTreePageEntry entry(uint8_t key, uint64_t value) { return {{std::byte{key}}, value}; }

	struct PrivateCandidates {
		// The test intentionally crosses the physical run-set fan-out derived
		// from the current segment layout, so the private range must admit both
		// the source run manifests and the resulting run-set hierarchy.
		NativeGraphPrivateSegmentSet identities{SegmentId(100U), Generation(7U), 65536U, 88U};
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

		[[nodiscard]] NativeGraphPrivateSegmentReader reader() const {
			return [this](const NativeGraphPrivateSegmentMaterialization &identity) {
				return payloads.at(identities.ordinalOf(identity));
			};
		}
	};

	NativeBulkRunFragmentLocation writeSingleEntryRun(PrivateCandidates &candidates, uint8_t key) {
		const NativeBulkRunFragment fragment{{entry(key, key)}};
		const std::vector<std::byte> fragmentPayload = NativeBulkRunFragmentCodec::encode(fragment);
		const NativeGraphPrivateSegmentMaterialization fragmentIdentity = candidates.allocate();
		candidates.write(fragmentIdentity, fragmentPayload);

		NativeBulkRunManifest manifest;
		manifest.fragments = {{fragmentIdentity.segment_id, fragmentIdentity.generation, fragmentIdentity.owner_id}};
		manifest.entry_count = 1U;
		manifest.encoded_bytes = fragmentPayload.size();
		const NativeGraphPrivateSegmentMaterialization manifestIdentity = candidates.allocate();
		candidates.write(manifestIdentity, NativeBulkRunManifestCodec::encode(manifest));
		return {manifestIdentity.segment_id, manifestIdentity.generation, manifestIdentity.owner_id};
	}

	std::vector<uint8_t> replay(const NativeBulkEdgeRunSource &source) {
		std::vector<uint8_t> keys;
		NativeBulkEdgeRunCursor runs = source();
		while (const std::optional<OrderedBTreeEntrySource> run = runs()) {
			OrderedBTreeEntryCursor entries = (*run)();
			while (const std::optional<BTreePageEntry> next = entries()) {
				keys.push_back(std::to_integer<uint8_t>(next->key.front()));
			}
		}
		return keys;
	}

} // namespace

TEST(NativeBulkEdgePrivateRunSetTest, BuildsARewindableLogicalRunSourceInAppendOrder) {
	PrivateCandidates candidates;
	NativeBulkEdgePrivateRunSetBuilder builder(
			4U, [&] { return candidates.allocate(); },
			[&](const NativeGraphPrivateSegmentMaterialization &identity, std::vector<std::byte> payload) {
				candidates.write(identity, std::move(payload));
			});
	builder.append(writeSingleEntryRun(candidates, 0x10U));
	builder.append(writeSingleEntryRun(candidates, 0x20U));
	builder.append(writeSingleEntryRun(candidates, 0x30U));
	const NativeBulkRunFragmentLocation root = builder.finish();

	const NativeBulkEdgeRunSource source =
			nativeBulkEdgePrivateRunSetSource(candidates.identities, root, 4U, candidates.reader());
	EXPECT_EQ(replay(source), (std::vector<uint8_t>{0x10U, 0x20U, 0x30U}));
	EXPECT_EQ(replay(source), (std::vector<uint8_t>{0x10U, 0x20U, 0x30U}));
	EXPECT_THROW((void)nativeBulkEdgePrivateRunSetSingleRun(candidates.identities, root, candidates.reader()),
			 FormatException);
}

TEST(NativeBulkEdgePrivateRunSetTest, BuildsAndReplaysExactAndOverFanoutHierarchies) {
	auto buildAndReplay = [](size_t count) {
		PrivateCandidates candidates;
		NativeBulkEdgePrivateRunSetBuilder builder(
				4U, [&] { return candidates.allocate(); },
				[&](const NativeGraphPrivateSegmentMaterialization &identity, std::vector<std::byte> payload) {
					candidates.write(identity, std::move(payload));
				});
		const size_t fanout = (RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES - RSS_NATIVE_BULK_RUN_SET_HEADER_BYTES) /
							  RSS_NATIVE_BULK_RUN_SET_REFERENCE_BYTES;
		ASSERT_GE(fanout, 2U);
		ASSERT_TRUE(count == fanout || count == fanout + 1U);
		for (size_t ordinal = 0U; ordinal < count; ++ordinal) {
			builder.append(writeSingleEntryRun(candidates, static_cast<uint8_t>(ordinal)));
		}
		const NativeBulkRunFragmentLocation root = builder.finish();
		const NativeBulkEdgePrivateRunSetManifest manifest = NativeBulkEdgePrivateRunSetCodec::decode(
				candidates.reader()(candidates.identities[
						candidates.identities.ordinalOf({root.segment_id, root.generation, root.transaction_id})]),
				root);
		EXPECT_EQ(manifest.level, count == fanout ? 0U : 1U);

		std::vector<uint8_t> expected;
		for (size_t ordinal = 0U; ordinal < count; ++ordinal) {
			expected.push_back(static_cast<uint8_t>(ordinal));
		}
		EXPECT_EQ(replay(nativeBulkEdgePrivateRunSetSource(candidates.identities, root, 4U, candidates.reader())),
				  expected);
	};
	const size_t fanout = (RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES - RSS_NATIVE_BULK_RUN_SET_HEADER_BYTES) /
						  RSS_NATIVE_BULK_RUN_SET_REFERENCE_BYTES;
	buildAndReplay(fanout);
	buildAndReplay(fanout + 1U);
}

TEST(NativeBulkEdgePrivateRunSetTest, RejectsChecksumAndRunCountCorruption) {
	PrivateCandidates candidates;
	NativeBulkEdgePrivateRunSetBuilder builder(
			4U, [&] { return candidates.allocate(); },
			[&](const NativeGraphPrivateSegmentMaterialization &identity, std::vector<std::byte> payload) {
				candidates.write(identity, std::move(payload));
			});
	builder.append(writeSingleEntryRun(candidates, 0x10U));
	const NativeBulkRunFragmentLocation root = builder.finish();
	const size_t rootOrdinal = candidates.identities.ordinalOf({root.segment_id, root.generation, root.transaction_id});

	candidates.payloads[rootOrdinal].back() ^= std::byte{0x01U};
	EXPECT_THROW((void)replay(nativeBulkEdgePrivateRunSetSource(candidates.identities, root, 4U, candidates.reader())),
			 FormatException);
	candidates.payloads[rootOrdinal].back() ^= std::byte{0x01U};

	constexpr size_t runCountOffset = sizeof(uint32_t) + 4U * sizeof(uint16_t) + sizeof(uint32_t);
	constexpr size_t recordCrcOffset = runCountOffset + sizeof(uint64_t) + sizeof(uint64_t);
	LittleEndianCodec::writeU64(candidates.payloads[rootOrdinal], runCountOffset, 2U);
	LittleEndianCodec::writeU32(
			candidates.payloads[rootOrdinal], recordCrcOffset,
			graph::utils::calculateCrc(candidates.payloads[rootOrdinal].data() + RSS_NATIVE_BULK_RUN_SET_HEADER_BYTES,
											   candidates.payloads[rootOrdinal].size() - RSS_NATIVE_BULK_RUN_SET_HEADER_BYTES));
	EXPECT_THROW((void)replay(nativeBulkEdgePrivateRunSetSource(candidates.identities, root, 4U, candidates.reader())),
			 FormatException);
}

TEST(NativeBulkEdgePrivateRunSetTest, ReturnsTheOnlyTerminalRun) {
	PrivateCandidates candidates;
	const NativeBulkRunFragmentLocation expected = writeSingleEntryRun(candidates, 0x10U);
	NativeBulkEdgePrivateRunSetBuilder builder(
			4U, [&] { return candidates.allocate(); },
			[&](const NativeGraphPrivateSegmentMaterialization &identity, std::vector<std::byte> payload) {
				candidates.write(identity, std::move(payload));
			});
	builder.append(expected);
	const NativeBulkRunFragmentLocation root = builder.finish();
	EXPECT_EQ(nativeBulkEdgePrivateRunSetSingleRun(candidates.identities, root, candidates.reader()), expected);
}

TEST(NativeBulkEdgePrivateRunSetTest, SingleBitMutationIsRejectedOrRoundTripsWithoutNormalization) {
	NativeBulkEdgePrivateRunSetManifest manifest;
	manifest.runs = {{SegmentId(9U), Generation(1U), 71U}};
	manifest.run_count = 1U;
	const std::vector<std::byte> encoded = NativeBulkEdgePrivateRunSetCodec::encode(manifest);
	const NativeBulkRunFragmentLocation location{SegmentId(10U), Generation(1U), 71U};

	for (size_t byte = 0U; byte < encoded.size(); ++byte) {
		for (uint8_t bit = 0U; bit < 8U; ++bit) {
			SCOPED_TRACE(::testing::Message() << "byte=" << byte << ", bit=" << static_cast<unsigned>(bit));
			auto mutation = encoded;
			mutation[byte] ^= std::byte{static_cast<unsigned char>(1U << bit)};
			try {
				NativeBulkEdgePrivateRunSetManifest decoded =
						NativeBulkEdgePrivateRunSetCodec::decode(mutation, location);
				decoded.location = {};
				EXPECT_EQ(NativeBulkEdgePrivateRunSetCodec::encode(decoded), mutation);
			} catch (const FormatException &) {
				// Header fields without a checksum remain valid only when their
				// exact bytes are themselves a canonical v1 representation.
			}
		}
	}
}
