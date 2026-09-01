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
#include <limits>
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
		LargeValueRef representative;
		representative.value_id = LargeValueId(1U);
		representative.kind = LargeValueKind::LVK_BYTES;
		representative.media_type = RSS_ENTITY_PROPERTY_LARGE_VALUE_MEDIA_TYPE;
		const size_t contentEntryBytes = RSS_NATIVE_BULK_RUN_HEADER_BYTES +
				RSS_NATIVE_BULK_RUN_RECORD_HEADER_BYTES +
				BTreePageEntryCodec::encodedSize({values.contentIndexKey(representative), 1U});

		EXPECT_THROW(NativeStreamedGraphLargeValueContentRunBuilder::buildRuns(publication, values, LargeValueId(1U),
																					   plan, {}, 256U, 1U,
																					   [](NativeBulkRunFragment &&) {}),
				 FormatException);
		NativeStreamedGraphLargeValuePlan emptyPlan = plan;
		emptyPlan.value_count = 0U;
		EXPECT_THROW(NativeStreamedGraphLargeValueContentRunBuilder::buildRuns(
						 publication, values, LargeValueId(1U), emptyPlan, {}, 4096U, 1U,
						 [](NativeBulkRunFragment &&) {}),
				 FormatException);
		EXPECT_THROW(NativeStreamedGraphLargeValueContentRunBuilder::buildRuns(
						 publication, values, LargeValueId(1U), plan, {}, RSS_NATIVE_BULK_RUN_HEADER_BYTES, 1U,
						 [](NativeBulkRunFragment &&) {}),
				 FormatException);
		EXPECT_THROW(NativeStreamedGraphLargeValueContentRunBuilder::buildRuns(
						 publication, values, LargeValueId(1U), plan, {}, RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES + 1U,
						 1U, [](NativeBulkRunFragment &&) {}),
				 FormatException);
		EXPECT_THROW(NativeStreamedGraphLargeValueContentRunBuilder::buildRuns(
						 publication, values, LargeValueId(1U), plan, {}, contentEntryBytes - 1U, 1U,
						 [](NativeBulkRunFragment &&) {}),
				 FormatException);
		EXPECT_THROW(static_cast<void>(NativeStreamedGraphLargeValueContentRunBuilder::derivePlanForValueCount(
					 values, 1U, contentEntryBytes - 1U, 2U)),
				 FormatException);
		EXPECT_THROW(static_cast<void>(NativeStreamedGraphLargeValueContentRunBuilder::derivePlanForValueCount(
					 values, 1U, RSS_NATIVE_BULK_RUN_HEADER_BYTES, 2U)),
				 FormatException);
		EXPECT_THROW(static_cast<void>(NativeStreamedGraphLargeValueContentRunBuilder::derivePlanForValueCount(
						 values, std::numeric_limits<size_t>::max(), 4096U, 2U)),
				 FormatException);
		EXPECT_THROW(static_cast<void>(NativeStreamedGraphLargeValueContentRunBuilder::derivePlanForValueCount(
						 values, 1U, 4096U, 1U)),
				 FormatException);
		const NativeStreamedGraphLargeValueContentRunPlan zeroCountPlan =
				NativeStreamedGraphLargeValueContentRunBuilder::derivePlanForValueCount(values, 0U, 4096U, 2U);
		EXPECT_EQ(zeroCountPlan.initial_fragment_count, 0U);
		EXPECT_EQ(zeroCountPlan.candidate_segment_count, 0U);
		const NativeStreamedGraphLargeValueContentRunPlan emptyMeasuredPlan =
				NativeStreamedGraphLargeValueContentRunBuilder::derivePlan(
						publication, values, LargeValueId(1U), emptyPlan, {}, 4096U, 2U);
		EXPECT_EQ(emptyMeasuredPlan.initial_fragment_count, 0U);
		EXPECT_EQ(emptyMeasuredPlan.candidate_segment_count, 0U);
		const NativeStreamedGraphLargeValueContentRunVisitor noVisitor;
		EXPECT_THROW(NativeStreamedGraphLargeValueContentRunBuilder::buildRuns(
					 publication, values, LargeValueId{}, plan, {}, 4096U, 1U,
					 [](NativeBulkRunFragment &&) {}),
				 FormatException);
		EXPECT_THROW(NativeStreamedGraphLargeValueContentRunBuilder::buildRuns(
					 publication, values, LargeValueId(1U), plan, {}, 4096U, 0U,
					 [](NativeBulkRunFragment &&) {}),
				 FormatException);
		EXPECT_THROW(NativeStreamedGraphLargeValueContentRunBuilder::buildRuns(
					 publication, values, LargeValueId(1U), plan, {}, 4096U, 1U, noVisitor),
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

	TEST_F(RelocatableMetadataStoreTest, StreamedLargeValueReferenceMapRejectsMalformedBoundedSources) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
		LargeValueStore values(io, *metadata);
		const std::vector<std::byte> firstPayload{static_cast<std::byte>('a')};
		const std::vector<std::byte> secondPayload{static_cast<std::byte>('b')};
		const LargeValueRef first = values.planReference(
				LargeValueId(1U), Generation(1U), firstPayload, LargeValueKind::LVK_BYTES,
				RSS_ENTITY_PROPERTY_LARGE_VALUE_MEDIA_TYPE, {}, LargeValueExtentClass::LVEC_32K);
		const LargeValueRef second = values.planReference(
				LargeValueId(2U), Generation(1U), secondPayload, LargeValueKind::LVK_BYTES,
				RSS_ENTITY_PROPERTY_LARGE_VALUE_MEDIA_TYPE, {}, LargeValueExtentClass::LVEC_32K);
		const LargeValueRef third = values.planReference(
				LargeValueId(3U), Generation(1U), firstPayload, LargeValueKind::LVK_BYTES,
				RSS_ENTITY_PROPERTY_LARGE_VALUE_MEDIA_TYPE, {}, LargeValueExtentClass::LVEC_32K);
		const BTreePageEntry firstEntry{values.contentIndexKey(first), 1U};
		const BTreePageEntry secondEntry{values.contentIndexKey(second), 2U};
		const BTreePageEntry thirdEntry{values.contentIndexKey(third), 3U};
		const size_t mappingEntryBytes = RSS_NATIVE_BULK_RUN_HEADER_BYTES +
				RSS_NATIVE_BULK_RUN_RECORD_HEADER_BYTES +
				BTreePageEntryCodec::encodedSize({std::vector<std::byte>(sizeof(uint64_t)), 1U});
		const auto sourceFor = [](std::vector<BTreePageEntry> entries) {
			return OrderedBTreeEntrySource{[entries = std::move(entries)] {
				return [entries, ordinal = size_t{0U}]() mutable -> std::optional<BTreePageEntry> {
					return ordinal == entries.size() ? std::nullopt
																		  : std::optional<BTreePageEntry>(entries[ordinal++]);
				};
			}};
		};
		const OrderedBTreeEntrySource validSource = sourceFor({firstEntry});
		const OrderedBTreeEntrySource noSource;
		const OrderedBTreeEntrySource noCursor = [] { return OrderedBTreeEntryCursor{}; };
		const NativeStreamedGraphLargeValueReferenceMapRunVisitor visitor = [](NativeBulkRunFragment &&) {};
		const NativeStreamedGraphLargeValueReferenceMapRunVisitor noVisitor;
		const auto expectRejected = [](const auto &operation) {
			EXPECT_THROW(static_cast<void>(operation()), FormatException);
		};

		expectRejected([&] {
			NativeStreamedGraphLargeValueReferenceMapBuilder::buildRuns(
					noSource, values, LargeValueId(1U), 1U, 4096U, 1U, visitor);
		});
		expectRejected([&] {
			NativeStreamedGraphLargeValueReferenceMapBuilder::buildRuns(
					validSource, values, LargeValueId{}, 1U, 4096U, 1U, visitor);
		});
		expectRejected([&] {
			NativeStreamedGraphLargeValueReferenceMapBuilder::buildRuns(
					validSource, values, LargeValueId(1U), 0U, 4096U, 1U, visitor);
		});
		expectRejected([&] {
			NativeStreamedGraphLargeValueReferenceMapBuilder::buildRuns(
					validSource, values, LargeValueId(1U), 1U, RSS_NATIVE_BULK_RUN_HEADER_BYTES, 1U, visitor);
		});
		expectRejected([&] {
			NativeStreamedGraphLargeValueReferenceMapBuilder::buildRuns(
					validSource, values, LargeValueId(1U), 1U, RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES + 1U, 1U,
					visitor);
		});
		expectRejected([&] {
			NativeStreamedGraphLargeValueReferenceMapBuilder::buildRuns(
					validSource, values, LargeValueId(1U), 1U, 4096U, 0U, visitor);
		});
		expectRejected([&] {
			NativeStreamedGraphLargeValueReferenceMapBuilder::buildRuns(
					validSource, values, LargeValueId(1U), 1U, 4096U, 1U, noVisitor);
		});
		expectRejected([&] {
			NativeStreamedGraphLargeValueReferenceMapBuilder::buildRuns(
					noCursor, values, LargeValueId(1U), 1U, 4096U, 1U, visitor);
		});

		BTreePageEntry zeroValue = firstEntry;
		zeroValue.value_or_child = 0U;
		expectRejected([&] {
			NativeStreamedGraphLargeValueReferenceMapBuilder::buildRuns(
					sourceFor({zeroValue}), values, LargeValueId(1U), 1U, 4096U, 1U, visitor);
		});
		BTreePageEntry mismatchedValue = firstEntry;
		mismatchedValue.value_or_child = 2U;
		expectRejected([&] {
			NativeStreamedGraphLargeValueReferenceMapBuilder::buildRuns(
					sourceFor({mismatchedValue}), values, LargeValueId(1U), 1U, 4096U, 1U, visitor);
		});
		expectRejected([&] {
			NativeStreamedGraphLargeValueReferenceMapBuilder::buildRuns(
					sourceFor({firstEntry}), values, LargeValueId(2U), 1U, 4096U, 1U, visitor);
		});
		expectRejected([&] {
			NativeStreamedGraphLargeValueReferenceMapBuilder::buildRuns(
					sourceFor({firstEntry}), values, LargeValueId(1U), 2U, 4096U, 1U, visitor);
		});
		expectRejected([&] {
			NativeStreamedGraphLargeValueReferenceMapBuilder::buildRuns(
					sourceFor({firstEntry, secondEntry}), values, LargeValueId(1U), 1U, 4096U, 1U, visitor);
		});
		expectRejected([&] {
			NativeStreamedGraphLargeValueReferenceMapBuilder::buildRuns(
					sourceFor({}), values, LargeValueId(1U), 1U, 4096U, 1U, visitor);
		});
		expectRejected([&] {
			NativeStreamedGraphLargeValueReferenceMapBuilder::buildRuns(
					sourceFor({firstEntry, firstEntry}), values, LargeValueId(1U), 2U, 4096U, 1U, visitor);
		});
		expectRejected([&] {
			NativeStreamedGraphLargeValueReferenceMapBuilder::buildRuns(
					sourceFor({thirdEntry}), values, LargeValueId(1U), 2U, 4096U, 1U, visitor);
		});
		expectRejected([&] {
			NativeStreamedGraphLargeValueReferenceMapBuilder::buildRuns(
					sourceFor({firstEntry}), values, LargeValueId(1U), 1U, mappingEntryBytes - 1U, 1U,
					visitor);
		});

		EXPECT_THROW(static_cast<void>(
						 NativeStreamedGraphLargeValueReferenceMapBuilder::derivePlanForValueCount(1U, 4096U, 1U)),
				 FormatException);
		EXPECT_THROW(static_cast<void>(NativeStreamedGraphLargeValueReferenceMapBuilder::derivePlanForValueCount(
						 1U, mappingEntryBytes - 1U, 2U)),
				 FormatException);
		EXPECT_THROW(static_cast<void>(NativeStreamedGraphLargeValueReferenceMapBuilder::derivePlanForValueCount(
						 std::numeric_limits<size_t>::max(), 4096U, 2U)),
				 FormatException);
	}

} // namespace graph::storage
