/**
 * @file test_NativeBulkEdgePrivateFamilyMaterializer.cpp
 * @brief Private bounded token and adjacency family materialization tests.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#include <array>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "graph/storage/relocatable/entity/NativeBulkEdgePrivateFamilyMaterializer.hpp"

using namespace graph::storage;

namespace {

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
				const auto &payload = payloads.at(identities.ordinalOf(identity));
				if (!payload.has_value()) {
					throw FormatException(FormatError::FERR_INVALID_STATE, "candidate reader encountered reservation");
				}
				return *payload;
			};
		}
	};

	NativeBulkEdgeRecordSource source(std::vector<NativeBulkEdgeRecord> records) {
		return [records = std::move(records)]() {
			size_t ordinal = 0U;
			return [records, ordinal]() mutable -> std::optional<NativeBulkEdgeRecord> {
				return ordinal == records.size() ? std::nullopt
												 : std::optional<NativeBulkEdgeRecord>(records[ordinal++]);
			};
		};
	}

	NativeBulkEdgePrivateFamilyMaterializationOptions options() {
		return {16U,
				64U,
				RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES,
				{2U, RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES, 64U, 4U, NativeBulkEdgeRunDuplicatePolicy::NBERDP_REJECT}};
	}

	std::vector<std::vector<std::byte>> replay(const PrivateCandidates &candidates,
											   NativeBulkRunFragmentLocation root) {
		std::vector<std::vector<std::byte>> keys;
		OrderedBTreeEntryCursor cursor = NativeBulkRunStore::orderedSourceFromPayloadReader(
				root, 4U, [&candidates](const NativeBulkRunFragmentLocation &location) {
					return candidates.reader()(candidates.identities[candidates.identities.ordinalOf(
							{location.segment_id, location.generation, location.transaction_id})]);
				})();
		while (const std::optional<BTreePageEntry> entry = cursor()) {
			keys.push_back(entry->key);
		}
		return keys;
	}

} // namespace

TEST(NativeBulkEdgePrivateFamilyMaterializerTest, MaterializesTokensAndBothAdjacencyDirectionsInDisjointRanges) {
	PrivateCandidates candidates;
	NativeBulkEdgeRecord first;
	first.edge_id = 100U;
	first.source_node_id = 1U;
	first.target_node_id = 2U;
	first.type = "REL";
	first.properties = {{"weight", graph::PropertyValue(int64_t(1))}};
	NativeBulkEdgeRecord second = first;
	second.edge_id = 101U;
	second.source_node_id = 3U;
	second.target_node_id = 1U;
	second.properties = {{"cost", graph::PropertyValue(int64_t(2))}};
	const NativeBulkEdgeRecordSource records = source({first, second});
	const NativeBulkEdgePrivateFamilyMaterializationOptions familyOptions = options();
	const std::array<NativeGraphLogicalIdLeaseRange, 1U> edgeLease{{{100U, 2U}}};
	const auto writer = [&](const NativeGraphPrivateSegmentMaterialization &identity, std::vector<std::byte> payload) {
		candidates.write(identity, std::move(payload));
	};

	const auto tokenRoot = NativeBulkEdgePrivateFamilyMaterializer::materializeTokens(
			records, candidates.identities, {0U, 96U}, familyOptions, candidates.reader(), writer, [] {});
	ASSERT_TRUE(tokenRoot.has_value());
	EXPECT_EQ(replay(candidates, *tokenRoot),
			  (std::vector<std::vector<std::byte>>{
					  NativeBulkEdgeTokenRunBuilder::encodeKey(NativeBulkEdgeTokenKind::NBETK_PROPERTY, "cost"),
					  NativeBulkEdgeTokenRunBuilder::encodeKey(NativeBulkEdgeTokenKind::NBETK_PROPERTY, "weight"),
					  NativeBulkEdgeTokenRunBuilder::encodeKey(NativeBulkEdgeTokenKind::NBETK_RELATIONSHIP_TYPE,
															   "REL")}));

	const NativeBulkEdgePrivateAdjacencyRoots adjacency = NativeBulkEdgePrivateFamilyMaterializer::materializeAdjacency(
			records, edgeLease, candidates.identities, {96U, 96U}, {192U, 96U}, familyOptions, candidates.reader(),
			writer, [] {});
	EXPECT_EQ(replay(candidates, adjacency.outgoing),
			  (std::vector<std::vector<std::byte>>{
					  AdjacencyKeyCodec::encode({GlobalAdjacencyDirection::GAD_OUT, 1U, 100U}),
					  AdjacencyKeyCodec::encode({GlobalAdjacencyDirection::GAD_OUT, 3U, 101U})}));
	EXPECT_EQ(replay(candidates, adjacency.incoming),
			  (std::vector<std::vector<std::byte>>{
					  AdjacencyKeyCodec::encode({GlobalAdjacencyDirection::GAD_IN, 1U, 101U}),
					  AdjacencyKeyCodec::encode({GlobalAdjacencyDirection::GAD_IN, 2U, 100U})}));
}

TEST(NativeBulkEdgePrivateFamilyMaterializerTest, AllowsAnEdgeStreamWithoutTokenCandidates) {
	PrivateCandidates candidates;
	NativeBulkEdgeRecord record;
	record.edge_id = 100U;
	record.source_node_id = 1U;
	record.target_node_id = 2U;
	const auto writer = [&](const NativeGraphPrivateSegmentMaterialization &identity, std::vector<std::byte> payload) {
		candidates.write(identity, std::move(payload));
	};
	EXPECT_FALSE(NativeBulkEdgePrivateFamilyMaterializer::materializeTokens(source({record}), candidates.identities,
													{0U, 0U}, options(), candidates.reader(),
													writer, [] {})
						 .has_value());

	NativeBulkEdgeRecord tokenized = record;
	tokenized.type = "REL";
	EXPECT_THROW(static_cast<void>(NativeBulkEdgePrivateFamilyMaterializer::materializeTokens(
					 source({tokenized}), candidates.identities, {0U, 0U}, options(), candidates.reader(), writer, [] {})),
				 FormatException);
}

TEST(NativeBulkEdgePrivateFamilyMaterializerTest, RejectsIncompleteScopedContractsBeforeWritingCandidates) {
	PrivateCandidates candidates;
	NativeBulkEdgeRecord record;
	record.edge_id = 100U;
	record.source_node_id = 1U;
	record.target_node_id = 2U;
	const NativeBulkEdgeRecordSource records = source({record});
	const std::array<NativeGraphLogicalIdLeaseRange, 1U> edgeLease{{{100U, 1U}}};
	const NativeGraphPrivateSegmentWriter writer = [&](const NativeGraphPrivateSegmentMaterialization &identity,
													   std::vector<std::byte> payload) {
		candidates.write(identity, std::move(payload));
	};
	const NativeGraphPrivateSegmentFence fence = [] {};
	const NativeGraphPrivateSegmentReader noReader;
	const NativeGraphPrivateSegmentWriter noWriter;
	const NativeGraphPrivateSegmentFence noFence;
	const auto expectFormatFailure = [](const auto &operation) {
		EXPECT_THROW(static_cast<void>(operation()), FormatException);
	};
	const NativeBulkEdgeRecordSource noSource;
	expectFormatFailure([&] {
		return NativeBulkEdgePrivateFamilyMaterializer::materializeTokens(
				noSource, candidates.identities, {0U, 1U}, options(), candidates.reader(), writer, fence);
	});
	NativeBulkEdgePrivateFamilyMaterializationOptions invalidOptions = options();
	invalidOptions.maximum_records = 0U;
	expectFormatFailure([&] {
		return NativeBulkEdgePrivateFamilyMaterializer::materializeTokens(
				records, candidates.identities, {0U, 1U}, invalidOptions, candidates.reader(), writer, fence);
	});
	invalidOptions = options();
	invalidOptions.maximum_records = std::numeric_limits<size_t>::max();
	expectFormatFailure([&] {
		return NativeBulkEdgePrivateFamilyMaterializer::materializeTokens(
				records, candidates.identities, {0U, 1U}, invalidOptions, candidates.reader(), writer, fence);
	});
	invalidOptions = options();
	invalidOptions.maximum_token_entries = 0U;
	expectFormatFailure([&] {
		return NativeBulkEdgePrivateFamilyMaterializer::materializeTokens(
				records, candidates.identities, {0U, 1U}, invalidOptions, candidates.reader(), writer, fence);
	});
	invalidOptions = options();
	invalidOptions.maximum_token_entries = std::numeric_limits<size_t>::max();
	expectFormatFailure([&] {
		return NativeBulkEdgePrivateFamilyMaterializer::materializeTokens(
				records, candidates.identities, {0U, 1U}, invalidOptions, candidates.reader(), writer, fence);
	});
	invalidOptions = options();
	invalidOptions.maximum_sort_run_bytes = RSS_NATIVE_BULK_RUN_HEADER_BYTES;
	expectFormatFailure([&] {
		return NativeBulkEdgePrivateFamilyMaterializer::materializeTokens(
				records, candidates.identities, {0U, 1U}, invalidOptions, candidates.reader(), writer, fence);
	});
	invalidOptions = options();
	invalidOptions.maximum_sort_run_bytes = std::numeric_limits<size_t>::max();
	expectFormatFailure([&] {
		return NativeBulkEdgePrivateFamilyMaterializer::materializeTokens(
				records, candidates.identities, {0U, 1U}, invalidOptions, candidates.reader(), writer, fence);
	});
	expectFormatFailure([&] {
		return NativeBulkEdgePrivateFamilyMaterializer::materializeTokens(records, candidates.identities, {0U, 1U},
																		  options(), noReader, writer, fence);
	});
	expectFormatFailure([&] {
		return NativeBulkEdgePrivateFamilyMaterializer::materializeTokens(
				records, candidates.identities, {0U, 1U}, options(), candidates.reader(), noWriter, fence);
	});
	expectFormatFailure([&] {
		return NativeBulkEdgePrivateFamilyMaterializer::materializeTokens(
				records, candidates.identities, {0U, 1U}, options(), candidates.reader(), writer, noFence);
	});
	const std::array<NativeGraphLogicalIdLeaseRange, 0U> noLease{};
	expectFormatFailure([&] {
		return NativeBulkEdgePrivateFamilyMaterializer::materializeAdjacency(records, noLease, candidates.identities,
																			 {0U, 1U}, {1U, 1U}, options(),
																			 candidates.reader(), writer, fence);
	});
	expectFormatFailure([&] {
		return NativeBulkEdgePrivateFamilyMaterializer::materializeAdjacency(records, edgeLease, candidates.identities,
																			 {0U, 0U}, {1U, 1U}, options(),
																			 candidates.reader(), writer, fence);
	});
	expectFormatFailure([&] {
		return NativeBulkEdgePrivateFamilyMaterializer::materializeAdjacency(records, edgeLease, candidates.identities,
																			 {0U, 1U}, {1U, 0U}, options(),
																			 candidates.reader(), writer, fence);
	});
	for (const auto &payload: candidates.payloads) {
		EXPECT_FALSE(payload.has_value());
	}
}
