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

TEST(NativeBulkEdgePrivateRunSetTest, TraversesACompleteTwoLevelHierarchyInRunOrder) {
	PrivateCandidates candidates;
	const NativeBulkRunFragmentLocation first = writeSingleEntryRun(candidates, 0x10U);
	const NativeBulkRunFragmentLocation second = writeSingleEntryRun(candidates, 0x20U);

	const auto writeRunSet = [&](const NativeBulkEdgePrivateRunSetManifest &manifest) {
		const NativeGraphPrivateSegmentMaterialization identity = candidates.allocate();
		candidates.write(identity, NativeBulkEdgePrivateRunSetCodec::encode(manifest));
		return NativeBulkRunFragmentLocation{identity.segment_id, identity.generation, identity.owner_id};
	};
	NativeBulkEdgePrivateRunSetManifest firstLeaf;
	firstLeaf.runs = {first};
	firstLeaf.run_count = 1U;
	const NativeBulkRunFragmentLocation firstLeafRoot = writeRunSet(firstLeaf);
	NativeBulkEdgePrivateRunSetManifest secondLeaf;
	secondLeaf.runs = {second};
	secondLeaf.run_count = 1U;
	const NativeBulkRunFragmentLocation secondLeafRoot = writeRunSet(secondLeaf);

	NativeBulkEdgePrivateRunSetManifest parent;
	parent.level = 1U;
	parent.children = {firstLeafRoot, secondLeafRoot};
	parent.run_count = 2U;
	const NativeBulkRunFragmentLocation root = writeRunSet(parent);

	EXPECT_EQ(replay(nativeBulkEdgePrivateRunSetSource(candidates.identities, root, 2U, candidates.reader())),
			  (std::vector<uint8_t>{0x10U, 0x20U}));
}

TEST(NativeBulkEdgePrivateRunSetTest, RejectsTruncatedPayloadsAndInvalidManifestRoots) {
	const NativeBulkRunFragmentLocation run{SegmentId(101U), Generation(7U), 88U};
	NativeBulkEdgePrivateRunSetManifest manifest;
	manifest.runs = {run};
	manifest.run_count = 1U;
	const std::vector<std::byte> payload = NativeBulkEdgePrivateRunSetCodec::encode(manifest);
	const NativeBulkRunFragmentLocation root{SegmentId(100U), Generation(7U), 88U};

	EXPECT_THROW(static_cast<void>(NativeBulkEdgePrivateRunSetCodec::decode({}, root)), FormatException);
	EXPECT_THROW(static_cast<void>(NativeBulkEdgePrivateRunSetCodec::decode(payload, {})), FormatException);
	manifest.runs = {root};
	const std::vector<std::byte> selfReferential = NativeBulkEdgePrivateRunSetCodec::encode(manifest);
	EXPECT_THROW(static_cast<void>(NativeBulkEdgePrivateRunSetCodec::decode(selfReferential, root)), FormatException);

	// A persisted payload can have a valid checksum while referring to an
	// invalid private segment. Decode must still reject that reference.
	std::vector<std::byte> invalidReference = payload;
	LittleEndianCodec::writeU64(invalidReference, RSS_NATIVE_BULK_RUN_SET_HEADER_BYTES, 0U);
	constexpr size_t recordCrcOffset = sizeof(uint32_t) + 4U * sizeof(uint16_t) + sizeof(uint32_t) + sizeof(uint64_t) +
								  sizeof(uint64_t);
	LittleEndianCodec::writeU32(
			invalidReference, recordCrcOffset,
			graph::utils::calculateCrc(invalidReference.data() + RSS_NATIVE_BULK_RUN_SET_HEADER_BYTES,
									   invalidReference.size() - RSS_NATIVE_BULK_RUN_SET_HEADER_BYTES));
	EXPECT_THROW(static_cast<void>(NativeBulkEdgePrivateRunSetCodec::decode(invalidReference, root)), FormatException);
}

// Crossing the physical fan-out requires thousands of private segments. Keep
// this capacity boundary out of the normal correctness suite.
TEST(DISABLED_NativeBulkEdgePrivateRunSetScaleTest, BuildsAndReplaysExactAndOverFanoutHierarchies) {
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

TEST(NativeBulkEdgePrivateRunSetTest, RejectsMalformedHierarchyBeforeReadingUntrustedRunPayloads) {
	NativeGraphPrivateSegmentSet identities{SegmentId(100U), Generation(7U), 3U, 88U};
	const NativeBulkRunFragmentLocation root{identities[0U].segment_id, identities[0U].generation, identities[0U].owner_id};
	const NativeBulkRunFragmentLocation firstChild{
			identities[1U].segment_id, identities[1U].generation, identities[1U].owner_id};
	const NativeBulkRunFragmentLocation secondChild{
			identities[2U].segment_id, identities[2U].generation, identities[2U].owner_id};
	const NativeBulkRunFragmentLocation untrustedRun{SegmentId(200U), Generation(1U), 88U};

	NativeBulkEdgePrivateRunSetManifest child;
	child.runs = {untrustedRun};
	child.run_count = 1U;
	NativeBulkEdgePrivateRunSetManifest parent;
	parent.level = 1U;
	parent.children = {firstChild, secondChild};
	// The parent claims only one run although both valid children each expose
	// one. Traversal must reject this before it opens either referenced run.
	parent.run_count = 1U;
	std::vector<std::vector<std::byte>> payloads{
			NativeBulkEdgePrivateRunSetCodec::encode(parent), NativeBulkEdgePrivateRunSetCodec::encode(child),
			NativeBulkEdgePrivateRunSetCodec::encode(child)};
	const NativeGraphPrivateSegmentReader reader = [&identities, &payloads](
				const NativeGraphPrivateSegmentMaterialization &identity) {
		return payloads.at(identities.ordinalOf(identity));
	};

	// A root at level one cannot be traversed with a one-frame depth budget.
	NativeBulkEdgeRunCursor depthBounded = nativeBulkEdgePrivateRunSetSource(identities, root, 1U, reader)();
	EXPECT_THROW((void)depthBounded(), FormatException);

	NativeBulkEdgeRunCursor cursor = nativeBulkEdgePrivateRunSetSource(identities, root, 4U, reader)();
	EXPECT_TRUE(cursor().has_value());
	EXPECT_TRUE(cursor().has_value());
	EXPECT_THROW((void)cursor(), FormatException);

	constexpr size_t levelOffset = sizeof(uint32_t) + 3U * sizeof(uint16_t);
	LittleEndianCodec::writeU16(payloads[0U], levelOffset, 2U);
	NativeBulkEdgeRunCursor malformedLevel = nativeBulkEdgePrivateRunSetSource(identities, root, 4U, reader)();
	EXPECT_THROW((void)malformedLevel(), FormatException);
}

TEST(NativeBulkEdgePrivateRunSetTest, RejectsAFlatManifestThatExposesMoreRunsThanDeclared) {
	PrivateCandidates candidates;
	const NativeBulkRunFragmentLocation first = writeSingleEntryRun(candidates, 0x11U);
	const NativeBulkRunFragmentLocation second = writeSingleEntryRun(candidates, 0x22U);
	NativeBulkEdgePrivateRunSetManifest rootManifest;
	rootManifest.runs = {first, second};
	rootManifest.run_count = 1U;
	const NativeGraphPrivateSegmentMaterialization rootIdentity = candidates.allocate();
	const NativeBulkRunFragmentLocation root{rootIdentity.segment_id, rootIdentity.generation, rootIdentity.owner_id};
	candidates.write(rootIdentity, NativeBulkEdgePrivateRunSetCodec::encode(rootManifest));

	NativeBulkEdgeRunCursor cursor = nativeBulkEdgePrivateRunSetSource(candidates.identities, root, 2U,
			candidates.reader())();
	ASSERT_TRUE(cursor().has_value());
	EXPECT_THROW(static_cast<void>(cursor()), FormatException);
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

TEST(NativeBulkEdgePrivateRunSetTest, RejectsInvalidPublicBuilderAndSourceContracts) {
	PrivateCandidates candidates;
	const NativeBulkEdgePrivateCandidateAllocator allocate = [&] { return candidates.allocate(); };
	const NativeBulkEdgePrivateCandidateWriter write =
			[&](const NativeGraphPrivateSegmentMaterialization &identity, std::vector<std::byte> payload) {
				candidates.write(identity, std::move(payload));
			};
	EXPECT_THROW(NativeBulkEdgePrivateRunSetBuilder(0U, allocate, write), FormatException);
	EXPECT_THROW(NativeBulkEdgePrivateRunSetBuilder(1U, {}, write), FormatException);
	EXPECT_THROW(NativeBulkEdgePrivateRunSetBuilder(1U, allocate, {}), FormatException);

	NativeBulkEdgePrivateRunSetBuilder builder(1U, allocate, write);
	EXPECT_THROW((void)builder.finish(), FormatException);
	EXPECT_THROW(builder.append({}), FormatException);

	const NativeBulkRunFragmentLocation invalid{};
	EXPECT_THROW((void)nativeBulkEdgePrivateRunSetSource(candidates.identities, invalid, 1U, candidates.reader()),
			 FormatException);
	const NativeBulkRunFragmentLocation unknown{SegmentId(99U), Generation(1U), 88U};
	EXPECT_THROW((void)nativeBulkEdgePrivateRunSetSource(candidates.identities, unknown, 1U, candidates.reader()),
			 FormatException);
	EXPECT_THROW((void)nativeBulkEdgePrivateRunSetSingleRun(candidates.identities, invalid, candidates.reader()),
			 FormatException);
	EXPECT_THROW((void)nativeBulkEdgePrivateRunSetSingleRun(candidates.identities, unknown, candidates.reader()),
				 FormatException);
}

TEST(NativeBulkEdgePrivateRunSetTest, RejectsInvalidOrForeignAllocatorIdentitiesBeforeWriting) {
	PrivateCandidates candidates;
	const NativeBulkRunFragmentLocation run = writeSingleEntryRun(candidates, 0x10U);

	auto verifyRejectedBeforeWrite = [&](NativeGraphPrivateSegmentMaterialization identity) {
		size_t writeCount = 0U;
		NativeBulkEdgePrivateRunSetBuilder builder(
				1U, [identity] { return identity; },
				[&writeCount](const NativeGraphPrivateSegmentMaterialization &, std::vector<std::byte>) { ++writeCount; });
		builder.append(run);
		EXPECT_THROW((void)builder.finish(), FormatException);
		EXPECT_EQ(writeCount, 0U);
	};

	// Candidate callbacks are extension points. Reject malformed and cross-owner
	// identities before they can make a private payload externally observable.
	verifyRejectedBeforeWrite({SegmentId{}, Generation(7U), 88U});
	verifyRejectedBeforeWrite({SegmentId(999U), Generation(7U), 89U});
}

TEST(NativeBulkEdgePrivateRunSetTest, RejectsMixedOwnerRunRootsBeforeWritingCandidate) {
	PrivateCandidates candidates;
	const NativeBulkRunFragmentLocation localRun = writeSingleEntryRun(candidates, 0x10U);
	const NativeBulkRunFragmentLocation foreignRun{SegmentId(900U), Generation(7U), 89U};

	size_t writeCount = 0U;
	NativeBulkEdgePrivateRunSetBuilder builder(
			1U, [] { return NativeGraphPrivateSegmentMaterialization{SegmentId(800U), Generation(7U), 88U}; },
			[&writeCount](const NativeGraphPrivateSegmentMaterialization &, std::vector<std::byte>) { ++writeCount; });
	builder.append(localRun);
	builder.append(foreignRun);

	// Ownership must be checked before the builder asks its callback to publish a private candidate.
	EXPECT_THROW(static_cast<void>(builder.finish()), FormatException);
	EXPECT_EQ(writeCount, 0U);
}

TEST(NativeBulkEdgePrivateRunSetTest, EvaluatesEachSourceAndTerminalContractGuard) {
	PrivateCandidates candidates;
	const NativeBulkRunFragmentLocation root{SegmentId(100U), Generation(7U), 88U};
	const NativeBulkEdgePrivateCandidateAllocator readerBackedAllocator = [&] { return candidates.allocate(); };
	const NativeBulkEdgePrivateCandidateWriter readerBackedWriter =
			[&](const NativeGraphPrivateSegmentMaterialization &identity, std::vector<std::byte> payload) {
				candidates.write(identity, std::move(payload));
			};

	// Exercise each short-circuit operand independently so invalid caller input
	// is validated consistently instead of only testing the first predicate.
	EXPECT_THROW((void)nativeBulkEdgePrivateRunSetSource(candidates.identities, root, 0U, candidates.reader()),
				 FormatException);
	EXPECT_THROW((void)nativeBulkEdgePrivateRunSetSource(candidates.identities, root, 1U, {}), FormatException);
	EXPECT_THROW((void)nativeBulkEdgePrivateRunSetSingleRun(candidates.identities, root, {}), FormatException);

	EXPECT_THROW(NativeBulkEdgePrivateRunSetBuilder(1U, {}, readerBackedWriter), FormatException);
	EXPECT_THROW(NativeBulkEdgePrivateRunSetBuilder(1U, readerBackedAllocator, {}), FormatException);
}

TEST(NativeBulkEdgePrivateRunSetTest, RejectsEveryInvalidOutboundRunSetManifestShape) {
	const auto valid = [] {
		NativeBulkEdgePrivateRunSetManifest manifest;
		manifest.runs = {{SegmentId(9U), Generation(1U), 71U}};
		manifest.run_count = 1U;
		return manifest;
	};
	const auto reject = [&](const auto &mutate) {
		NativeBulkEdgePrivateRunSetManifest manifest = valid();
		mutate(manifest);
		EXPECT_THROW(static_cast<void>(NativeBulkEdgePrivateRunSetCodec::encode(manifest)), FormatException);
	};

	reject([](auto &manifest) { manifest.location = {SegmentId(10U), Generation(1U), 71U}; });
	reject([](auto &manifest) { manifest.runs.clear(); });
	reject([](auto &manifest) { manifest.run_count = 0U; });
	reject([](auto &manifest) { manifest.children = {{SegmentId(10U), Generation(1U), 71U}}; });
	reject([](auto &manifest) { manifest.runs.front().segment_id = {}; });
	reject([](auto &manifest) { manifest.runs.front().generation = {}; });
	reject([](auto &manifest) { manifest.runs.front().transaction_id = 0U; });
	reject([](auto &manifest) { manifest.runs.push_back({SegmentId(10U), Generation(1U), 72U}); });

	// A non-leaf manifest carries child run-set roots only, still under one
	// immutable private owner.
	NativeBulkEdgePrivateRunSetManifest internal;
	internal.level = 1U;
	internal.children = {{SegmentId(9U), Generation(1U), 71U}};
	internal.run_count = 1U;
	EXPECT_NO_THROW(static_cast<void>(NativeBulkEdgePrivateRunSetCodec::encode(internal)));
	internal.runs = {{SegmentId(10U), Generation(1U), 71U}};
	EXPECT_THROW(static_cast<void>(NativeBulkEdgePrivateRunSetCodec::encode(internal)), FormatException);
}

TEST(NativeBulkEdgePrivateRunSetTest, RejectsMissingReadersAndNonterminalSingleRunShapes) {
	PrivateCandidates candidates;
	const NativeBulkRunFragmentLocation first = writeSingleEntryRun(candidates, 0x10U);
	const NativeBulkRunFragmentLocation second = writeSingleEntryRun(candidates, 0x20U);
	NativeBulkEdgePrivateRunSetBuilder builder(
			4U, [&] { return candidates.allocate(); },
			[&](const NativeGraphPrivateSegmentMaterialization &identity, std::vector<std::byte> payload) {
				candidates.write(identity, std::move(payload));
			});
	builder.append(first);
	builder.append(second);
	const NativeBulkRunFragmentLocation root = builder.finish();

	EXPECT_THROW((void)nativeBulkEdgePrivateRunSetSource(candidates.identities, root, 4U, {}), FormatException);
	EXPECT_THROW((void)nativeBulkEdgePrivateRunSetSingleRun(candidates.identities, root, {}), FormatException);

	const size_t rootOrdinal = candidates.identities.ordinalOf({root.segment_id, root.generation, root.transaction_id});
	constexpr size_t runCountOffset = sizeof(uint32_t) + 4U * sizeof(uint16_t) + sizeof(uint32_t);
	constexpr size_t recordCrcOffset = runCountOffset + sizeof(uint64_t) + sizeof(uint64_t);
	LittleEndianCodec::writeU64(candidates.payloads[rootOrdinal], runCountOffset, 1U);
	LittleEndianCodec::writeU32(
			candidates.payloads[rootOrdinal], recordCrcOffset,
			graph::utils::calculateCrc(candidates.payloads[rootOrdinal].data() + RSS_NATIVE_BULK_RUN_SET_HEADER_BYTES,
										   candidates.payloads[rootOrdinal].size() - RSS_NATIVE_BULK_RUN_SET_HEADER_BYTES));
	EXPECT_THROW((void)nativeBulkEdgePrivateRunSetSingleRun(candidates.identities, root, candidates.reader()),
				 FormatException);
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
