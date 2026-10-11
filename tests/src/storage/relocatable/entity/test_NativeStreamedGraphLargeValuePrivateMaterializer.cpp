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

} // namespace graph::storage
