/**
 * @file test_NativeBulkEdgePrivateTokenResolver.cpp
 * @brief Candidate-private Token resolver tests for streamed Edge publication.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "graph/storage/relocatable/entity/NativeBulkEdgePrivateTokenResolver.hpp"

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
	EXPECT_FALSE(resolver.find(NativeBulkEdgeTokenKind::NBETK_PROPERTY, "missing").has_value());
	EXPECT_EQ(candidates.fences, 1U);
	const NativeBulkEdgeMaterializerResolvers materializerResolvers = resolver.materializerResolvers();
	candidates.reads = 0U;
	for (size_t count = 0U; count != 4U; ++count) {
		EXPECT_EQ(materializerResolvers.relationship_type_token("KNOWS"), 9U);
	}
	EXPECT_EQ(candidates.reads, 1U);
	for (size_t count = 0U; count != 4U; ++count) {
		EXPECT_EQ(materializerResolvers.property_key_token("cost"), 3U);
	}
	EXPECT_EQ(candidates.reads, 2U);
	EXPECT_FALSE(materializerResolvers.property_key_token("unknown").has_value());
	EXPECT_EQ(candidates.reads, 3U);
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

TEST(NativeBulkEdgePrivateTokenResolverTest, TraversesPrivateInternalNodesAndFailsClosedOnPayloadCorruption) {
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
