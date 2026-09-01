/**
 * @file test_NativeBulkEdgePrivateTokenResolver.cpp
 * @brief Candidate-private Token resolver tests for streamed Edge publication.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "graph/storage/relocatable/entity/NativeBulkEdgePrivateTokenResolver.hpp"
#include "graph/utils/ChecksumUtils.hpp"

using namespace graph::storage;

namespace {

	struct PrivateCandidates {
		NativeGraphPrivateSegmentSet identities{SegmentId(100U), Generation(7U), 512U, 88U};
		std::vector<std::optional<std::vector<std::byte>>> payloads =
				std::vector<std::optional<std::vector<std::byte>>>(identities.size());
		mutable size_t reads = 0U;
		size_t fences = 0U;

		void write(const NativeGraphPrivateSegmentMaterialization &identity, std::vector<std::byte> payload) {
			const size_t ordinal = identities.ordinalOf(identity);
			ASSERT_FALSE(payloads[ordinal].has_value());
			payloads[ordinal] = std::move(payload);
		}

		[[nodiscard]] NativeGraphPrivateSegmentReader reader() const {
			return [this](const NativeGraphPrivateSegmentMaterialization &identity) {
				++reads;
				const auto &payload = payloads.at(identities.ordinalOf(identity));
				if (!payload.has_value()) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
									  "candidate reader encountered an unfilled reservation");
				}
				return *payload;
			};
		}
	};

	[[nodiscard]] NativeBulkEdgeCatalogTokenResolutionSource
	resolutionSource(std::vector<NativeBulkEdgeCatalogTokenResolution> resolutions) {
		return [resolutions = std::move(resolutions)] {
			return [resolutions, ordinal = size_t{0U}]() mutable
					-> std::optional<NativeBulkEdgeCatalogTokenResolution> {
				return ordinal == resolutions.size() ? std::nullopt
											  : std::optional<NativeBulkEdgeCatalogTokenResolution>(resolutions[ordinal++]);
			};
		};
	}

} // namespace

TEST(NativeBulkEdgePrivateTokenResolverTest, ResolvesNamesFromAPrivateImmutableTree) {
	PrivateCandidates candidates;
	const std::vector<NativeBulkEdgeCatalogTokenResolution> resolutions = {
			{NativeBulkEdgeTokenKind::NBETK_PROPERTY, "cost", 3U, true},
			{NativeBulkEdgeTokenKind::NBETK_PROPERTY, "name", 4U, false},
			{NativeBulkEdgeTokenKind::NBETK_RELATIONSHIP_TYPE, "KNOWS", 9U, true}};
	const size_t capacity = NativeBulkEdgePrivateTokenResolver::maximumCandidateSegments(resolutions.size());
	ASSERT_GT(capacity, 0U);
	const NativeBulkEdgePrivateTokenResolver resolver = NativeBulkEdgePrivateTokenResolver::materialize(
			candidates.identities, {0U, capacity}, resolutionSource(resolutions), resolutions.size(), candidates.reader(),
			[&](const NativeGraphPrivateSegmentMaterialization &identity, std::vector<std::byte> payload) {
				candidates.write(identity, std::move(payload));
			},
			[&] { ++candidates.fences; });

	EXPECT_FALSE(resolver.empty());
	EXPECT_LE(resolver.materializedSegmentCount(), capacity);
	EXPECT_EQ(resolver.maximumCandidateSegmentCount(), capacity);
	EXPECT_EQ(resolver.find(NativeBulkEdgeTokenKind::NBETK_PROPERTY, "cost"), 3U);
	EXPECT_EQ(resolver.find(NativeBulkEdgeTokenKind::NBETK_PROPERTY, "name"), 4U);
	EXPECT_EQ(resolver.find(NativeBulkEdgeTokenKind::NBETK_RELATIONSHIP_TYPE, "KNOWS"), 9U);
	EXPECT_FALSE(resolver.find(NativeBulkEdgeTokenKind::NBETK_PROPERTY, "a").has_value());
	EXPECT_FALSE(resolver.find(NativeBulkEdgeTokenKind::NBETK_PROPERTY, "missing").has_value());
	EXPECT_EQ(candidates.fences, 1U);
	const NativeBulkEdgeMaterializerResolvers materializerResolvers = resolver.materializerResolvers();
	candidates.reads = 0U;
	for (size_t count = 0U; count != 4U; ++count) {
		EXPECT_EQ(materializerResolvers.relationship_type_token("KNOWS"), 9U);
	}
	EXPECT_EQ(candidates.reads, 1U);
	EXPECT_FALSE(materializerResolvers.relationship_type_token("OTHER").has_value());
	EXPECT_EQ(candidates.reads, 2U);
	for (size_t count = 0U; count != 4U; ++count) {
		EXPECT_EQ(materializerResolvers.property_key_token("cost"), 3U);
	}
	EXPECT_EQ(candidates.reads, 3U);
	EXPECT_FALSE(materializerResolvers.property_key_token("unknown").has_value());
	EXPECT_EQ(candidates.reads, 4U);
}

TEST(NativeBulkEdgePrivateTokenResolverTest, RejectsUnorderedSources) {
	PrivateCandidates candidates;
	const std::vector<NativeBulkEdgeCatalogTokenResolution> unordered = {
			{NativeBulkEdgeTokenKind::NBETK_RELATIONSHIP_TYPE, "TYPE", 1U, true},
			{NativeBulkEdgeTokenKind::NBETK_PROPERTY, "property", 1U, true}};
	const size_t capacity = NativeBulkEdgePrivateTokenResolver::maximumCandidateSegments(unordered.size());
	EXPECT_THROW(static_cast<void>(NativeBulkEdgePrivateTokenResolver::materialize(
				 candidates.identities, {0U, capacity}, resolutionSource(unordered), unordered.size(), candidates.reader(),
				 [&](const NativeGraphPrivateSegmentMaterialization &identity, std::vector<std::byte> payload) {
					 candidates.write(identity, std::move(payload));
				 },
				 [] {})),
				 FormatException);
}

TEST(NativeBulkEdgePrivateTokenResolverTest,
	 RejectsIncompleteOrOverBoundSourcesWithoutPublishingCandidatePayloads) {
	PrivateCandidates candidates;
	const NativeBulkEdgePrivateCandidateRange validRange{0U, 1U};
	const NativeBulkEdgeCatalogTokenResolutionSource oneEntry = resolutionSource(
			{{NativeBulkEdgeTokenKind::NBETK_PROPERTY, "property", 1U, true}});
	const auto noWrite = [&](const NativeGraphPrivateSegmentMaterialization &, std::vector<std::byte>) {
		FAIL() << "a rejected resolver source must not materialize a candidate";
	};

	EXPECT_THROW(static_cast<void>(NativeBulkEdgePrivateTokenResolver::materialize(
				 candidates.identities, validRange, {}, 1U, candidates.reader(), noWrite, [] {})),
			 FormatException);
	EXPECT_THROW(static_cast<void>(NativeBulkEdgePrivateTokenResolver::materialize(
				 candidates.identities, validRange, oneEntry, std::numeric_limits<size_t>::max(), candidates.reader(), noWrite,
				 [] {})),
			 FormatException);
	EXPECT_THROW(static_cast<void>(NativeBulkEdgePrivateTokenResolver::materialize(
				 candidates.identities, validRange, oneEntry, 1U, {}, noWrite, [] {})),
			 FormatException);
	EXPECT_THROW(static_cast<void>(NativeBulkEdgePrivateTokenResolver::materialize(
				 candidates.identities, validRange, oneEntry, 1U, candidates.reader(), noWrite, {})),
			 FormatException);
	EXPECT_THROW(static_cast<void>(NativeBulkEdgePrivateTokenResolver::materialize(
				 candidates.identities, {candidates.identities.size(), 1U}, oneEntry, 1U, candidates.reader(), noWrite,
				 [] {})),
			 FormatException);

	const NativeBulkEdgeCatalogTokenResolutionSource unopened = [] {
		return NativeBulkEdgeCatalogTokenResolutionCursor{};
	};
	EXPECT_THROW(static_cast<void>(NativeBulkEdgePrivateTokenResolver::materialize(
				 candidates.identities, validRange, unopened, 1U, candidates.reader(), noWrite, [] {})),
			 FormatException);
	EXPECT_THROW(static_cast<void>(NativeBulkEdgePrivateTokenResolver::materialize(
				 candidates.identities, validRange,
				 resolutionSource({{NativeBulkEdgeTokenKind::NBETK_PROPERTY, "first", 1U, true},
											 {NativeBulkEdgeTokenKind::NBETK_PROPERTY, "second", 2U, true}}),
				 1U, candidates.reader(), noWrite, [] {})),
			 FormatException);
	EXPECT_THROW(static_cast<void>(NativeBulkEdgePrivateTokenResolver::materialize(
				 candidates.identities, validRange,
				 resolutionSource({{NativeBulkEdgeTokenKind::NBETK_PROPERTY, "property", 0U, true}}), 1U,
				 candidates.reader(), noWrite, [] {})),
			 FormatException);
	EXPECT_EQ(candidates.reads, 0U);
	EXPECT_EQ(candidates.fences, 0U);
	EXPECT_FALSE(candidates.payloads[0U].has_value());
}

TEST(NativeBulkEdgePrivateTokenResolverTest, KeepsAnEmptyBoundedSourcePrivateAndReturnsAnEmptyResolver) {
	PrivateCandidates candidates;
	const size_t capacity = NativeBulkEdgePrivateTokenResolver::maximumCandidateSegments(1U);
	const NativeBulkEdgePrivateTokenResolver resolver = NativeBulkEdgePrivateTokenResolver::materialize(
			candidates.identities, {0U, capacity}, resolutionSource({}), 1U, candidates.reader(),
			[&](const NativeGraphPrivateSegmentMaterialization &identity, std::vector<std::byte> payload) {
				candidates.write(identity, std::move(payload));
			},
			[&] { ++candidates.fences; });

	EXPECT_TRUE(resolver.empty());
	EXPECT_FALSE(resolver.find(NativeBulkEdgeTokenKind::NBETK_PROPERTY, "missing").has_value());
	EXPECT_EQ(candidates.reads, 0U);
	EXPECT_EQ(candidates.fences, 0U);
	EXPECT_FALSE(candidates.payloads[0U].has_value());
}

TEST(NativeBulkEdgePrivateTokenResolverTest, BuildsAndTraversesTheSmallestBoundedInternalTree) {
	PrivateCandidates candidates;
	std::vector<NativeBulkEdgeCatalogTokenResolution> resolutions;
	const auto nameFor = [](uint64_t ordinal) {
		std::string name = "p" + std::to_string(ordinal);
		name.insert(1U, 3U - (name.size() - 1U), '0');
		name.append(RSS_SYSTEM_CATALOG_NAME_MAX_BYTES - name.size(), 'x');
		return name;
	};
	// The bounded codec fits 126 maximal entries in one leaf.  One additional
	// entry is the smallest deterministic input that requires an internal node.
	for (uint64_t ordinal = 0U; ordinal != 127U; ++ordinal) {
		resolutions.push_back({NativeBulkEdgeTokenKind::NBETK_PROPERTY, nameFor(ordinal), ordinal + 1U, true});
	}
	const size_t capacity = NativeBulkEdgePrivateTokenResolver::maximumCandidateSegments(resolutions.size());
	ASSERT_EQ(capacity, 3U);
	const NativeBulkEdgePrivateTokenResolver resolver = NativeBulkEdgePrivateTokenResolver::materialize(
			candidates.identities, {0U, capacity}, resolutionSource(resolutions), resolutions.size(), candidates.reader(),
			[&](const NativeGraphPrivateSegmentMaterialization &identity, std::vector<std::byte> payload) {
				candidates.write(identity, std::move(payload));
			},
			[&] { ++candidates.fences; });

	EXPECT_EQ(resolver.materializedSegmentCount(), 3U);
	EXPECT_EQ(candidates.fences, 2U);
	EXPECT_EQ(resolver.find(NativeBulkEdgeTokenKind::NBETK_PROPERTY, nameFor(0U)), 1U);
	EXPECT_EQ(resolver.find(NativeBulkEdgeTokenKind::NBETK_PROPERTY, nameFor(63U)), 64U);
	EXPECT_EQ(resolver.find(NativeBulkEdgeTokenKind::NBETK_PROPERTY, nameFor(126U)), 127U);
	EXPECT_FALSE(resolver.find(NativeBulkEdgeTokenKind::NBETK_PROPERTY, "a").has_value());
	EXPECT_FALSE(resolver.find(NativeBulkEdgeTokenKind::NBETK_PROPERTY, "p999").has_value());
}

TEST(NativeBulkEdgePrivateTokenResolverTest, RejectsZeroBoundsAndInvalidCatalogResolutions) {
	PrivateCandidates candidates;
	const NativeBulkEdgePrivateCandidateRange range{0U, 1U};
	const NativeBulkEdgeCatalogTokenResolutionSource oneEntry = resolutionSource(
			{{NativeBulkEdgeTokenKind::NBETK_PROPERTY, "property", 1U, true}});
	const NativeGraphPrivateSegmentWriter noWriter;
	const auto writer = [](const NativeGraphPrivateSegmentMaterialization &, std::vector<std::byte>) {};

	EXPECT_EQ(NativeBulkEdgePrivateTokenResolver::maximumCandidateSegments(0U), 0U);
	const NativeBulkEdgePrivateTokenResolver empty = NativeBulkEdgePrivateTokenResolver::makeEmpty();
	EXPECT_TRUE(empty.empty());
	EXPECT_FALSE(empty.find(NativeBulkEdgeTokenKind::NBETK_PROPERTY, "property").has_value());
	EXPECT_THROW(static_cast<void>(NativeBulkEdgePrivateTokenResolver::materialize(
				 candidates.identities, range, oneEntry, 0U, candidates.reader(), writer, [] {})),
				 FormatException);
	EXPECT_THROW(static_cast<void>(NativeBulkEdgePrivateTokenResolver::materialize(
				 candidates.identities, {0U, 0U}, oneEntry, 1U, candidates.reader(), writer, [] {})),
				 FormatException);
	EXPECT_THROW(static_cast<void>(NativeBulkEdgePrivateTokenResolver::materialize(
				 candidates.identities, range, oneEntry, 1U, candidates.reader(), noWriter, [] {})),
				 FormatException);
	EXPECT_THROW(static_cast<void>(NativeBulkEdgePrivateTokenResolver::materialize(
				 candidates.identities, range,
				 resolutionSource({{static_cast<NativeBulkEdgeTokenKind>(255U), "property", 1U, true}}), 1U,
				 candidates.reader(), writer, [] {})),
				 FormatException);
	EXPECT_THROW(static_cast<void>(NativeBulkEdgePrivateTokenResolver::materialize(
				 candidates.identities, range,
				 resolutionSource({{NativeBulkEdgeTokenKind::NBETK_PROPERTY, "property",
								  std::numeric_limits<uint32_t>::max() + 1ULL, true}}),
				 1U, candidates.reader(), writer, [] {})),
				 FormatException);
}

// Building an internal resolver node requires a capacity-sized set of maximal
// token keys, so retain it as an explicit scale/corruption gate.
TEST(DISABLED_NativeBulkEdgePrivateTokenResolverScaleTest, TraversesPrivateInternalNodesAndFailsClosedOnPayloadCorruption) {
	PrivateCandidates candidates;
	std::vector<NativeBulkEdgeCatalogTokenResolution> resolutions;
	for (uint64_t ordinal = 0U; ordinal < 130U; ++ordinal) {
		std::string name = "p" + std::to_string(ordinal);
		name.insert(1U, 3U - (name.size() - 1U), '0');
		name.append(RSS_SYSTEM_CATALOG_NAME_MAX_BYTES - name.size(), 'x');
		resolutions.push_back({NativeBulkEdgeTokenKind::NBETK_PROPERTY, std::move(name), ordinal + 1U, true});
	}
	const size_t capacity = NativeBulkEdgePrivateTokenResolver::maximumCandidateSegments(resolutions.size());
	ASSERT_GE(capacity, 3U);
	const NativeBulkEdgePrivateTokenResolver resolver = NativeBulkEdgePrivateTokenResolver::materialize(
			candidates.identities, {0U, capacity}, resolutionSource(resolutions), resolutions.size(), candidates.reader(),
			[&](const NativeGraphPrivateSegmentMaterialization &identity, std::vector<std::byte> payload) {
				candidates.write(identity, std::move(payload));
			},
			[&] { ++candidates.fences; });

	EXPECT_GT(resolver.materializedSegmentCount(), 1U);
	EXPECT_EQ(resolver.find(NativeBulkEdgeTokenKind::NBETK_PROPERTY, resolutions.front().name), 1U);
	EXPECT_EQ(resolver.find(NativeBulkEdgeTokenKind::NBETK_PROPERTY, resolutions.back().name), resolutions.size());
	ASSERT_TRUE(candidates.payloads[0U].has_value());
	(*candidates.payloads[0U])[0U] = std::byte{0U};
	EXPECT_THROW(static_cast<void>(resolver.find(NativeBulkEdgeTokenKind::NBETK_PROPERTY, resolutions.front().name)),
				 FormatException);
}

TEST(NativeBulkEdgePrivateTokenResolverTest, RejectsEveryPersistedLeafHeaderAndRecordCorruption) {
	PrivateCandidates candidates;
	const std::vector<NativeBulkEdgeCatalogTokenResolution> resolutions = {
			{NativeBulkEdgeTokenKind::NBETK_PROPERTY, "property", 7U, true}};
	const size_t capacity = NativeBulkEdgePrivateTokenResolver::maximumCandidateSegments(resolutions.size());
	const NativeBulkEdgePrivateTokenResolver resolver = NativeBulkEdgePrivateTokenResolver::materialize(
			candidates.identities, {0U, capacity}, resolutionSource(resolutions), resolutions.size(), candidates.reader(),
			[&](const NativeGraphPrivateSegmentMaterialization &identity, std::vector<std::byte> payload) {
				candidates.write(identity, std::move(payload));
			},
			[&] { ++candidates.fences; });
	ASSERT_TRUE(candidates.payloads[0U].has_value());
	const std::vector<std::byte> original = *candidates.payloads[0U];
	const auto rewriteRecordChecksum = [](std::vector<std::byte> &payload) {
		const uint32_t checksum = graph::utils::calculateCrc(payload.data() + 64U, payload.size() - 64U);
		LittleEndianCodec::writeU32(payload, 24U, checksum);
	};
	const auto rejects = [&](const std::function<void(std::vector<std::byte> &)> &mutate) {
		*candidates.payloads[0U] = original;
		mutate(*candidates.payloads[0U]);
		EXPECT_THROW(static_cast<void>(resolver.find(NativeBulkEdgeTokenKind::NBETK_PROPERTY, "property")),
					 FormatException);
	};

	// Header fields are protected by their own shape rules. Record mutations
	// refresh the payload CRC so the parser exercises the exact lower-level
	// validation instead of stopping at checksum verification.
	rejects([](std::vector<std::byte> &payload) { payload[4U] = std::byte{0U}; });
	rejects([](std::vector<std::byte> &payload) { payload[6U] = std::byte{0U}; });
	rejects([](std::vector<std::byte> &payload) { payload[8U] = std::byte{0U}; });
	rejects([](std::vector<std::byte> &payload) { payload[10U] = std::byte{1U}; });
	rejects([](std::vector<std::byte> &payload) { payload.resize(63U); });
	rejects([&](std::vector<std::byte> &payload) {
		payload.resize(64U);
		const std::byte empty{};
		LittleEndianCodec::writeU32(payload, 24U, graph::utils::calculateCrc(&empty, 0U));
	});
	rejects([](std::vector<std::byte> &payload) { payload[12U] = std::byte{0U}; });
	rejects([](std::vector<std::byte> &payload) { payload[16U] = std::byte{0U}; });
	rejects([](std::vector<std::byte> &payload) { payload[28U] = std::byte{1U}; });
	rejects([](std::vector<std::byte> &payload) { payload[32U] = std::byte{1U}; });
	rejects([](std::vector<std::byte> &payload) { payload[24U] ^= std::byte{0xFFU}; });
	rejects([&](std::vector<std::byte> &payload) {
		payload[64U] = std::byte{0U};
		rewriteRecordChecksum(payload);
	});
	rejects([&](std::vector<std::byte> &payload) {
		payload[64U] = std::byte{1U};
		rewriteRecordChecksum(payload);
	});
	rejects([&](std::vector<std::byte> &payload) {
		for (size_t offset = 66U; offset != 74U; ++offset) {
			payload[offset] = std::byte{0U};
		}
		rewriteRecordChecksum(payload);
	});
	rejects([&](std::vector<std::byte> &payload) {
		LittleEndianCodec::writeU64(payload, 66U, std::numeric_limits<uint32_t>::max() + 1ULL);
		rewriteRecordChecksum(payload);
	});
	rejects([&](std::vector<std::byte> &payload) {
		payload[74U] = std::byte{0xFFU};
		rewriteRecordChecksum(payload);
	});
	rejects([&](std::vector<std::byte> &payload) {
		payload.push_back(std::byte{0U});
		rewriteRecordChecksum(payload);
	});
}

TEST(NativeBulkEdgePrivateTokenResolverTest, RejectsUnorderedPersistedLeafEntries) {
	PrivateCandidates candidates;
	const std::vector<NativeBulkEdgeCatalogTokenResolution> resolutions = {
			{NativeBulkEdgeTokenKind::NBETK_PROPERTY, "aa", 7U, true},
			{NativeBulkEdgeTokenKind::NBETK_PROPERTY, "bb", 8U, true}};
	const size_t capacity = NativeBulkEdgePrivateTokenResolver::maximumCandidateSegments(resolutions.size());
	const NativeBulkEdgePrivateTokenResolver resolver = NativeBulkEdgePrivateTokenResolver::materialize(
			candidates.identities, {0U, capacity}, resolutionSource(resolutions), resolutions.size(), candidates.reader(),
			[&](const NativeGraphPrivateSegmentMaterialization &identity, std::vector<std::byte> payload) {
				candidates.write(identity, std::move(payload));
			},
			[] {});
	ASSERT_TRUE(candidates.payloads[0U].has_value());
	std::vector<std::byte> &payload = *candidates.payloads[0U];
	// Both encoded keys have the same width; replacing the second with the
	// first produces a strictly non-increasing persisted leaf without resizing.
	for (size_t index = 0U; index != 3U; ++index) {
		payload[87U + index] = payload[74U + index];
	}
	const uint32_t checksum = graph::utils::calculateCrc(payload.data() + 64U, payload.size() - 64U);
	LittleEndianCodec::writeU32(payload, 24U, checksum);
	EXPECT_THROW(static_cast<void>(resolver.find(NativeBulkEdgeTokenKind::NBETK_PROPERTY, "bb")), FormatException);
}
