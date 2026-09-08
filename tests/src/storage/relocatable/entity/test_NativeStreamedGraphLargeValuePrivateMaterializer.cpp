/**
 * @file test_NativeStreamedGraphLargeValuePrivateMaterializer.cpp
 * @brief Private content-index run materialization tests.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#include "storage/relocatable/metadata/RelocatableMetadataStoreTestFixture.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "graph/storage/relocatable/entity/NativeStreamedGraphLargeValuePrivateMaterializer.hpp"

namespace graph::storage {

	namespace {

		[[nodiscard]] NativeStreamedGraphDeferredPublication largeValuePublication() {
			const NativeBulkEdgeRecordSource source = [] {
				return [ordinal = 0U]() mutable -> std::optional<NativeBulkEdgeRecord> {
					if (ordinal == 2U) {
						return std::nullopt;
					}
					const uint64_t edgeId = ++ordinal;
					return NativeBulkEdgeRecord{
							edgeId,
							1U,
							2U,
							"REL",
							{{"body", graph::PropertyValue(std::string(5000U, static_cast<char>('a' + ordinal)))}}};
				};
			};
			const std::array<NativeGraphLogicalIdLeaseRange, 1U> lease = {{NativeGraphLogicalIdLeaseRange{1U, 2U}}};
			return NativeStreamedGraphDeferredPublication::create(
					source, lease, {2U, 1U},
					{[](std::string_view) -> std::optional<uint64_t> { return 7U; },
					 [](std::string_view) -> std::optional<uint32_t> { return 9U; }});
		}

		struct PrivateCandidates {
			NativeGraphPrivateSegmentSet identities;
			std::vector<std::optional<std::vector<std::byte>>> payloads;

			explicit PrivateCandidates(size_t count) :
				identities(SegmentId(100U), Generation(7U), count, 88U), payloads(count) {}

			void write(const NativeGraphPrivateSegmentMaterialization &identity, std::vector<std::byte> payload) {
				const size_t ordinal = identities.ordinalOf(identity);
				ASSERT_FALSE(payloads[ordinal].has_value());
				payloads[ordinal] = std::move(payload);
			}

			[[nodiscard]] NativeGraphPrivateSegmentReader reader() const {
				return [this](const NativeGraphPrivateSegmentMaterialization &identity) {
					const std::optional<std::vector<std::byte>> &payload = payloads.at(identities.ordinalOf(identity));
					if (!payload.has_value()) {
						throw FormatException(FormatError::FERR_INVALID_STATE,
											  "candidate reader encountered an unwritten reservation");
					}
					return *payload;
				};
			}
		};

	} // namespace

	TEST_F(RelocatableMetadataStoreTest, StreamedLargeValuePrivateMaterializerPublishesOnlyPrivateSortedRun) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
		LargeValueStore values(io, *metadata);
		const NativeStreamedGraphDeferredPublication publication = largeValuePublication();
		const NativeStreamedGraphLargeValuePlan valuePlan =
				NativeStreamedGraphLargeValuePlanner::measure(publication, values);
		const NativeStreamedGraphLargeValueContentRunPlan contentPlan =
				NativeStreamedGraphLargeValueContentRunBuilder::derivePlan(publication, values, LargeValueId(1U),
																		   valuePlan, {}, 256U, 2U);
		PrivateCandidates candidates(contentPlan.candidate_segment_count);

		const NativeBulkRunFragmentLocation root = NativeStreamedGraphLargeValuePrivateMaterializer::materialize(
				publication, values, LargeValueId(1U), valuePlan, {}, contentPlan, candidates.identities,
				{0U, candidates.identities.size()}, {256U, 2U, valuePlan.value_count, 4U}, candidates.reader(),
				[&](const NativeGraphPrivateSegmentMaterialization &identity, std::vector<std::byte> payload) {
					candidates.write(identity, std::move(payload));
				},
				[] {});

		std::vector<uint64_t> valueIds;
		OrderedBTreeEntryCursor cursor = NativeBulkRunStore::orderedSourceFromPayloadReader(
				root, 4U, [&](const NativeBulkRunFragmentLocation &location) {
					return candidates.reader()(candidates.identities[candidates.identities.ordinalOf(
							{location.segment_id, location.generation, location.transaction_id})]);
				})();
		while (const std::optional<BTreePageEntry> entry = cursor()) {
			valueIds.push_back(entry->value_or_child);
		}
		std::ranges::sort(valueIds);
		EXPECT_EQ(valueIds, (std::vector<uint64_t>{1U, 2U}));
	}

	TEST_F(RelocatableMetadataStoreTest, StreamedLargeValuePrivateMaterializerBuildsAPrivateReferenceMapRun) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
		LargeValueStore values(io, *metadata);
		const NativeStreamedGraphDeferredPublication publication = largeValuePublication();
		const NativeStreamedGraphLargeValuePlan valuePlan =
				NativeStreamedGraphLargeValuePlanner::measure(publication, values);
		const NativeStreamedGraphLargeValueContentRunPlan contentPlan =
				NativeStreamedGraphLargeValueContentRunBuilder::derivePlan(publication, values, LargeValueId(1U),
																	   valuePlan, {}, 256U, 2U);
		PrivateCandidates contentCandidates(contentPlan.candidate_segment_count);
		const NativeStreamedGraphLargeValuePrivateMaterializationOptions options{
				256U, 2U, valuePlan.value_count, 4U};
		const NativeBulkRunFragmentLocation contentRoot =
				NativeStreamedGraphLargeValuePrivateMaterializer::materialize(
						publication, values, LargeValueId(1U), valuePlan, {}, contentPlan, contentCandidates.identities,
						{0U, contentCandidates.identities.size()}, options, contentCandidates.reader(),
						[&](const NativeGraphPrivateSegmentMaterialization &identity, std::vector<std::byte> payload) {
							contentCandidates.write(identity, std::move(payload));
						},
						[] {});

		const OrderedBTreeEntrySource contentEntries = NativeBulkRunStore::orderedSourceFromPayloadReader(
				contentRoot, 4U, [&contentCandidates](const NativeBulkRunFragmentLocation &location) {
					return contentCandidates.reader()(contentCandidates.identities[contentCandidates.identities.ordinalOf(
							{location.segment_id, location.generation, location.transaction_id})]);
				});
		const NativeStreamedGraphLargeValueReferenceMapRunPlan mapPlan =
				NativeStreamedGraphLargeValueReferenceMapBuilder::derivePlanForValueCount(
						valuePlan.reference_count, options.maximum_run_bytes, options.maximum_open_runs);
		PrivateCandidates mapCandidates(mapPlan.candidate_segment_count);
		const NativeBulkRunFragmentLocation mapRoot =
				NativeStreamedGraphLargeValuePrivateMaterializer::materializeReferenceMap(
						contentEntries, values, LargeValueId(1U), valuePlan.reference_count, mapPlan,
						mapCandidates.identities, {0U, mapCandidates.identities.size()}, options, mapCandidates.reader(),
						[&](const NativeGraphPrivateSegmentMaterialization &identity, std::vector<std::byte> payload) {
							mapCandidates.write(identity, std::move(payload));
						},
						[] {});

		OrderedBTreeEntryCursor cursor = NativeBulkRunStore::orderedSourceFromPayloadReader(
					mapRoot, 4U, [&mapCandidates](const NativeBulkRunFragmentLocation &location) {
						return mapCandidates.reader()(mapCandidates.identities[mapCandidates.identities.ordinalOf(
								{location.segment_id, location.generation, location.transaction_id})]);
					})();
		size_t referenceEntries = 0U;
		while (cursor().has_value()) {
			++referenceEntries;
		}
		EXPECT_EQ(referenceEntries, valuePlan.reference_count);
	}

	TEST_F(RelocatableMetadataStoreTest, StreamedLargeValuePrivateMaterializerRejectsIncompleteScopedContracts) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
		LargeValueStore values(io, *metadata);
		const NativeStreamedGraphDeferredPublication publication = largeValuePublication();
		const NativeStreamedGraphLargeValuePlan valuePlan =
				NativeStreamedGraphLargeValuePlanner::measure(publication, values);
		const NativeStreamedGraphLargeValueContentRunPlan contentPlan =
				NativeStreamedGraphLargeValueContentRunBuilder::derivePlan(publication, values, LargeValueId(1U),
																		   valuePlan, {}, 256U, 2U);
		PrivateCandidates candidates(contentPlan.candidate_segment_count);
		const NativeStreamedGraphLargeValuePrivateMaterializationOptions options{256U, 2U, valuePlan.value_count, 4U};
		const NativeGraphPrivateSegmentWriter writer = [](const NativeGraphPrivateSegmentMaterialization &,
														  std::vector<std::byte>) {};
		const NativeGraphPrivateSegmentFence fence = [] {};
		const NativeGraphPrivateSegmentReader noReader;
		const NativeGraphPrivateSegmentWriter noWriter;
		const NativeGraphPrivateSegmentFence noFence;
		const auto expectFormatFailure = [](const auto &operation) {
			EXPECT_THROW(static_cast<void>(operation()), FormatException);
		};

		expectFormatFailure([&] {
			return NativeStreamedGraphLargeValuePrivateMaterializer::materialize(
					publication, values, LargeValueId{}, valuePlan, {}, contentPlan, candidates.identities,
					{0U, candidates.identities.size()}, options, candidates.reader(), writer, fence);
		});
		NativeStreamedGraphLargeValuePlan emptyValuePlan = valuePlan;
		emptyValuePlan.value_count = 0U;
		expectFormatFailure([&] {
			return NativeStreamedGraphLargeValuePrivateMaterializer::materialize(
					publication, values, LargeValueId(1U), emptyValuePlan, {}, contentPlan, candidates.identities,
					{0U, candidates.identities.size()}, options, candidates.reader(), writer, fence);
		});
		NativeStreamedGraphLargeValueContentRunPlan emptyContentPlan = contentPlan;
		emptyContentPlan.initial_fragment_count = 0U;
		expectFormatFailure([&] {
			return NativeStreamedGraphLargeValuePrivateMaterializer::materialize(
					publication, values, LargeValueId(1U), valuePlan, {}, emptyContentPlan, candidates.identities,
					{0U, candidates.identities.size()}, options, candidates.reader(), writer, fence);
		});
		emptyContentPlan = contentPlan;
		emptyContentPlan.candidate_segment_count = 0U;
		expectFormatFailure([&] {
			return NativeStreamedGraphLargeValuePrivateMaterializer::materialize(
					publication, values, LargeValueId(1U), valuePlan, {}, emptyContentPlan, candidates.identities,
					{0U, 0U}, options, candidates.reader(), writer, fence);
		});
		expectFormatFailure([&] {
			return NativeStreamedGraphLargeValuePrivateMaterializer::materialize(
					publication, values, LargeValueId(1U), valuePlan, {}, contentPlan, candidates.identities,
					{0U, candidates.identities.size() - 1U}, options, candidates.reader(), writer, fence);
		});
		expectFormatFailure([&] {
			return NativeStreamedGraphLargeValuePrivateMaterializer::materialize(
					publication, values, LargeValueId(1U), valuePlan, {}, contentPlan, candidates.identities,
					{0U, candidates.identities.size()}, options, noReader, writer, fence);
		});
		expectFormatFailure([&] {
			return NativeStreamedGraphLargeValuePrivateMaterializer::materialize(
					publication, values, LargeValueId(1U), valuePlan, {}, contentPlan, candidates.identities,
					{0U, candidates.identities.size()}, options, candidates.reader(), noWriter, fence);
		});
		expectFormatFailure([&] {
			return NativeStreamedGraphLargeValuePrivateMaterializer::materialize(
					publication, values, LargeValueId(1U), valuePlan, {}, contentPlan, candidates.identities,
					{0U, candidates.identities.size()}, options, candidates.reader(), writer, noFence);
		});
		for (const int option: {0, 1, 2, 3}) {
			NativeStreamedGraphLargeValuePrivateMaterializationOptions invalid = options;
			switch (option) {
				case 0: invalid.maximum_run_bytes = RSS_NATIVE_BULK_RUN_HEADER_BYTES; break;
				case 1: invalid.maximum_open_runs = 1U; break;
				case 2: invalid.maximum_entries = 0U; break;
				default: invalid.maximum_manifest_depth = 0U; break;
			}
			expectFormatFailure([&] {
				return NativeStreamedGraphLargeValuePrivateMaterializer::materialize(
						publication, values, LargeValueId(1U), valuePlan, {}, contentPlan, candidates.identities,
						{0U, candidates.identities.size()}, invalid, candidates.reader(), writer, fence);
			});
		}

		const OrderedBTreeEntrySource noContentEntries;
		const NativeStreamedGraphLargeValueReferenceMapRunPlan mapPlan{1U, 1U};
		expectFormatFailure([&] {
			return NativeStreamedGraphLargeValuePrivateMaterializer::materializeReferenceMap(
					noContentEntries, values, LargeValueId(1U), 1U, mapPlan, candidates.identities, {0U, 1U}, options,
					candidates.reader(), writer, fence);
		});
		const OrderedBTreeEntrySource oneEntry = [] {
			return []() -> std::optional<BTreePageEntry> { return std::nullopt; };
		};
		expectFormatFailure([&] {
			return NativeStreamedGraphLargeValuePrivateMaterializer::materializeReferenceMap(
					oneEntry, values, LargeValueId{}, 1U, mapPlan, candidates.identities, {0U, 1U}, options,
					candidates.reader(), writer, fence);
		});
		expectFormatFailure([&] {
			return NativeStreamedGraphLargeValuePrivateMaterializer::materializeReferenceMap(
					oneEntry, values, LargeValueId(1U), 0U, mapPlan, candidates.identities, {0U, 1U}, options,
					candidates.reader(), writer, fence);
		});
		NativeStreamedGraphLargeValueReferenceMapRunPlan emptyMapPlan = mapPlan;
		emptyMapPlan.initial_fragment_count = 0U;
		expectFormatFailure([&] {
			return NativeStreamedGraphLargeValuePrivateMaterializer::materializeReferenceMap(
					oneEntry, values, LargeValueId(1U), 1U, emptyMapPlan, candidates.identities, {0U, 1U}, options,
					candidates.reader(), writer, fence);
		});
		emptyMapPlan = mapPlan;
		emptyMapPlan.candidate_segment_count = 0U;
		expectFormatFailure([&] {
			return NativeStreamedGraphLargeValuePrivateMaterializer::materializeReferenceMap(
					oneEntry, values, LargeValueId(1U), 1U, emptyMapPlan, candidates.identities, {0U, 0U}, options,
					candidates.reader(), writer, fence);
		});
		expectFormatFailure([&] {
			return NativeStreamedGraphLargeValuePrivateMaterializer::materializeReferenceMap(
				oneEntry, values, LargeValueId(1U), 1U, mapPlan, candidates.identities, {0U, 2U}, options,
				candidates.reader(), writer, fence);
		});
		for (const int option: {0, 1, 2, 3}) {
			NativeStreamedGraphLargeValuePrivateMaterializationOptions invalid = options;
			switch (option) {
				case 0: invalid.maximum_run_bytes = RSS_NATIVE_BULK_RUN_HEADER_BYTES; break;
				case 1: invalid.maximum_open_runs = 1U; break;
				case 2: invalid.maximum_entries = 0U; break;
				default: invalid.maximum_manifest_depth = 0U; break;
			}
				expectFormatFailure([&] {
				return NativeStreamedGraphLargeValuePrivateMaterializer::materializeReferenceMap(
						oneEntry, values, LargeValueId(1U), 1U, mapPlan, candidates.identities, {0U, 1U}, invalid,
						candidates.reader(), writer, fence);
			});
		}
		expectFormatFailure([&] {
			return NativeStreamedGraphLargeValuePrivateMaterializer::materializeReferenceMap(
					oneEntry, values, LargeValueId(1U), 1U, mapPlan, candidates.identities, {0U, 1U}, options, noReader,
					writer, fence);
		});
		expectFormatFailure([&] {
			return NativeStreamedGraphLargeValuePrivateMaterializer::materializeReferenceMap(
					oneEntry, values, LargeValueId(1U), 1U, mapPlan, candidates.identities, {0U, 1U}, options,
					candidates.reader(), noWriter, fence);
		});
		expectFormatFailure([&] {
			return NativeStreamedGraphLargeValuePrivateMaterializer::materializeReferenceMap(
					oneEntry, values, LargeValueId(1U), 1U, mapPlan, candidates.identities, {0U, 1U}, options,
					candidates.reader(), writer, noFence);
		});
		for (const auto &payload: candidates.payloads) {
			EXPECT_FALSE(payload.has_value());
		}
	}

} // namespace graph::storage
