/**
 * @file test_NativeStreamedGraphLargeValueContentRunBuilder.cpp
 * @brief Bounded content-index run tests for streamed LargeValue publication.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#include "storage/relocatable/metadata/RelocatableMetadataStoreTestFixture.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <vector>

#include "graph/storage/relocatable/entity/NativeStreamedGraphLargeValueContentRunBuilder.hpp"
#include "graph/storage/relocatable/entity/NativeStreamedGraphLargeValueReferenceMapBuilder.hpp"

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

	} // namespace

	TEST_F(RelocatableMetadataStoreTest, StreamedLargeValueContentRunsAreSortedAndBounded) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
		LargeValueStore values(io, *metadata);
		const NativeStreamedGraphDeferredPublication publication = largeValuePublication();
		const NativeStreamedGraphLargeValuePlan plan =
				NativeStreamedGraphLargeValuePlanner::measure(publication, values);
		ASSERT_EQ(plan.value_count, 2U);

		std::vector<NativeBulkRunFragment> fragments;
		NativeStreamedGraphLargeValueContentRunBuilder::buildRuns(
				publication, values, LargeValueId(1U), plan, {}, 4096U, 4U,
				[&](NativeBulkRunFragment &&fragment) { fragments.push_back(std::move(fragment)); });

		ASSERT_EQ(fragments.size(), 1U);
		ASSERT_EQ(fragments.front().entries.size(), plan.value_count);
		EXPECT_LT(fragments.front().entries[0].key, fragments.front().entries[1].key);
		EXPECT_NE(fragments.front().entries[0].value_or_child, fragments.front().entries[1].value_or_child);
	}

	TEST_F(RelocatableMetadataStoreTest, StreamedLargeValueContentRunsDeriveABoundedPrivateCandidatePlan) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
		LargeValueStore values(io, *metadata);
		const NativeStreamedGraphDeferredPublication publication = largeValuePublication();
		const NativeStreamedGraphLargeValuePlan valuePlan =
				NativeStreamedGraphLargeValuePlanner::measure(publication, values);

		const NativeStreamedGraphLargeValueContentRunPlan runPlan =
				NativeStreamedGraphLargeValueContentRunBuilder::derivePlan(publication, values, LargeValueId(1U),
																		   valuePlan, {}, 256U, 2U);
		const NativeStreamedGraphLargeValueContentRunPlan countedPlan =
				NativeStreamedGraphLargeValueContentRunBuilder::derivePlanForValueCount(values, valuePlan.value_count,
																						256U, 2U);
		EXPECT_EQ(runPlan.initial_fragment_count, 2U);
		EXPECT_EQ(countedPlan.initial_fragment_count, runPlan.initial_fragment_count);
		EXPECT_EQ(countedPlan.candidate_segment_count, runPlan.candidate_segment_count);
		EXPECT_GE(runPlan.candidate_segment_count, runPlan.initial_fragment_count * 3U);
		EXPECT_THROW(static_cast<void>(NativeStreamedGraphLargeValueContentRunBuilder::derivePlan(
							 publication, values, LargeValueId(1U), valuePlan, {}, 256U, 1U)),
					 FormatException);
	}

	TEST_F(RelocatableMetadataStoreTest, StreamedLargeValueContentRunsRejectAnInsufficientFragmentBound) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
		LargeValueStore values(io, *metadata);
		const NativeStreamedGraphDeferredPublication publication = largeValuePublication();
		const NativeStreamedGraphLargeValuePlan plan =
				NativeStreamedGraphLargeValuePlanner::measure(publication, values);

		EXPECT_THROW(NativeStreamedGraphLargeValueContentRunBuilder::buildRuns(publication, values, LargeValueId(1U),
																			   plan, {}, 256U, 1U,
																			   [](NativeBulkRunFragment &&) {}),
					 FormatException);
	}

	TEST_F(RelocatableMetadataStoreTest, StreamedLargeValueReferenceMapGroupsEqualContentAndSortsByProvisionalId) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
		LargeValueStore values(io, *metadata);
		const std::vector<std::byte> duplicate{static_cast<std::byte>('d')};
		const std::vector<std::byte> distinct{static_cast<std::byte>('u')};
		const auto reference = [&](LargeValueId valueId, const std::vector<std::byte> &payload) {
			return values.planReference(valueId, Generation(1U), payload, LargeValueKind::LVK_BYTES,
											 RSS_ENTITY_PROPERTY_LARGE_VALUE_MEDIA_TYPE, {}, LargeValueExtentClass::LVEC_32K);
		};
		const LargeValueRef first = reference(LargeValueId(1U), duplicate);
		const LargeValueRef second = reference(LargeValueId(2U), distinct);
		const LargeValueRef third = reference(LargeValueId(3U), duplicate);
		auto content = std::make_shared<std::vector<BTreePageEntry>>(
				std::initializer_list<BTreePageEntry>{{values.contentIndexKey(first), 1U}, {values.contentIndexKey(second), 2U},
																		 {values.contentIndexKey(third), 3U}});
		std::ranges::sort(*content, {}, &BTreePageEntry::key);
		const OrderedBTreeEntrySource contentSource = [content] {
			return [content, ordinal = 0U]() mutable -> std::optional<BTreePageEntry> {
				if (ordinal == content->size()) {
					return std::nullopt;
				}
				return (*content)[ordinal++];
			};
		};

		std::vector<NativeBulkRunFragment> fragments;
		NativeStreamedGraphLargeValueReferenceMapBuilder::buildRuns(
				contentSource, values, LargeValueId(1U), 3U, 4096U, 1U,
				[&fragments](NativeBulkRunFragment &&fragment) { fragments.push_back(std::move(fragment)); });
		ASSERT_EQ(fragments.size(), 1U);
		ASSERT_EQ(fragments.front().entries.size(), 3U);
		EXPECT_EQ(fragments.front().entries[0].value_or_child, 1U);
		EXPECT_EQ(fragments.front().entries[1].value_or_child, 2U);
		EXPECT_EQ(fragments.front().entries[2].value_or_child, 1U);
		const NativeStreamedGraphLargeValueReferenceMapRunPlan plan =
				NativeStreamedGraphLargeValueReferenceMapBuilder::derivePlanForValueCount(3U, 4096U, 2U);
		EXPECT_EQ(plan.initial_fragment_count, 1U);
		EXPECT_GE(plan.candidate_segment_count, 3U);
	}

} // namespace graph::storage
