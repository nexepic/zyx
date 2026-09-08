/**
 * @file test_NativeStreamedGraphPublicationPlan.cpp
 * @brief Bounded streamed Edge publication-plan tests.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "graph/storage/relocatable/entity/NativeStreamedGraphCandidateLayout.hpp"
#include "graph/storage/relocatable/entity/NativeStreamedGraphPageRanges.hpp"
#include "graph/storage/relocatable/entity/NativeStreamedGraphPublicationPlan.hpp"
#include "graph/storage/relocatable/entity/NativeStreamedGraphPublicationShape.hpp"

using namespace graph::storage;

namespace {

	NativeBulkEdgeRecord record(uint64_t id, std::string type) {
		NativeBulkEdgeRecord value;
		value.edge_id = id;
		value.source_node_id = id;
		value.target_node_id = id + 100U;
		value.type = std::move(type);
		value.properties = {{"key" + std::to_string(id), graph::PropertyValue(int64_t(id))}};
		return value;
	}

	NativeBulkEdgeRecordSource source(std::vector<NativeBulkEdgeRecord> records) {
		return [records = std::move(records)]() {
			size_t ordinal = 0U;
			return [records, ordinal]() mutable -> std::optional<NativeBulkEdgeRecord> {
				return ordinal == records.size() ? std::nullopt
												 : std::optional<NativeBulkEdgeRecord>(records[ordinal++]);
			};
		};
	}

	NativeBulkEdgePublicationPreflightOptions options(size_t runBytes, size_t maximumCandidates) {
		return {64U, runBytes, 2U, maximumCandidates};
	}

} // namespace

TEST(NativeStreamedGraphPublicationPlanTest, AccountsForEveryBoundedMergeLevelBeforePublication) {
	std::vector<NativeBulkEdgeRecord> records;
	for (uint64_t id = 1U; id <= 8U; ++id) {
		records.push_back(record(id, "REL_" + std::to_string(id)));
	}
	const std::vector<NativeGraphLogicalIdLeaseRange> lease{{1U, static_cast<uint64_t>(records.size())}};
	const size_t runBytes = RSS_NATIVE_BULK_RUN_HEADER_BYTES + NativeBulkEdgeRecordCodec::encodedSize(records.front()) +
							RSS_NATIVE_BULK_RUN_RECORD_HEADER_BYTES + 1U;
	const NativeStreamedGraphPublicationPlan plan = NativeStreamedGraphPublicationPlan::derive(
			source(records), lease, options(runBytes, 512U), [](uint64_t) { return true; });
	EXPECT_EQ(plan.record_count, records.size());
	EXPECT_EQ(plan.property_root_count, records.size());
	EXPECT_EQ(plan.large_value_count, 0U);
	EXPECT_GT(plan.edge_entity_page_count, 0U);
	EXPECT_GT(plan.token_candidate_segments, 0U);
	EXPECT_GT(plan.token_resolver_candidate_segments, 0U);
	EXPECT_GT(plan.outgoing_adjacency_candidate_segments, 0U);
	EXPECT_GT(plan.incoming_adjacency_candidate_segments, 0U);
	EXPECT_EQ(plan.maximum_candidate_segments, 512U);
	EXPECT_GT(plan.merge_passes, 0U);
	EXPECT_GT(plan.source_run_count, 0U);
	EXPECT_GT(plan.maximumCandidateSegmentCount(), plan.token_candidate_segments);
}

TEST(NativeStreamedGraphPublicationPlanTest, RejectsAPlanWhoseIntermediateMergeCandidatesExceedTheBound) {
	std::vector<NativeBulkEdgeRecord> records;
	for (uint64_t id = 1U; id <= 8U; ++id) {
		records.push_back(record(id, "REL_" + std::to_string(id)));
	}
	const std::vector<NativeGraphLogicalIdLeaseRange> lease{{1U, static_cast<uint64_t>(records.size())}};
	const size_t runBytes = RSS_NATIVE_BULK_RUN_HEADER_BYTES + NativeBulkEdgeRecordCodec::encodedSize(records.front()) +
							RSS_NATIVE_BULK_RUN_RECORD_HEADER_BYTES + 1U;
	const NativeStreamedGraphPublicationPlan unconstrained = NativeStreamedGraphPublicationPlan::derive(
			source(records), lease, options(runBytes, 512U), [](uint64_t) { return true; });
	ASSERT_GT(unconstrained.maximumCandidateSegmentCount(), 1U);
	EXPECT_THROW(static_cast<void>(NativeStreamedGraphPublicationPlan::derive(
						 source(records), lease, options(runBytes, unconstrained.maximumCandidateSegmentCount() - 1U),
						 [](uint64_t) { return true; })),
				 FormatException);
}

TEST(NativeStreamedGraphPublicationPlanTest, AllowsAnUntypedPropertyFreeEdgeStreamWithoutTokenRuns) {
	NativeBulkEdgeRecord untyped = record(1U, {});
	untyped.properties.clear();
	const std::vector<NativeGraphLogicalIdLeaseRange> lease{{1U, 1U}};
	const size_t runBytes = RSS_NATIVE_BULK_RUN_HEADER_BYTES + NativeBulkEdgeRecordCodec::encodedSize(untyped) +
							RSS_NATIVE_BULK_RUN_RECORD_HEADER_BYTES + 1U;
	const NativeStreamedGraphPublicationPlan plan = NativeStreamedGraphPublicationPlan::derive(
			source({untyped}), lease, options(runBytes, 128U), [](uint64_t) { return true; });
	EXPECT_EQ(plan.token_entry_count, 0U);
	EXPECT_EQ(plan.property_root_count, 0U);
	EXPECT_EQ(plan.large_value_count, 0U);
	EXPECT_EQ(plan.token_candidate_segments, 0U);
	EXPECT_EQ(plan.token_resolver_candidate_segments, 0U);
	EXPECT_GT(plan.outgoing_adjacency_candidate_segments, 0U);
	EXPECT_GT(plan.incoming_adjacency_candidate_segments, 0U);
}

TEST(NativeStreamedGraphPublicationPlanTest, AppendsAnExactLargeValueCandidateFamilyAfterTheCoreFamilies) {
	NativeBulkEdgeRecord overflowing = record(1U, "REL");
	overflowing.properties = {{"body", graph::PropertyValue(graph::PropertyValue::Bytes(
											   RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, std::byte{0x5A}))}};
	const std::vector<NativeGraphLogicalIdLeaseRange> lease{{1U, 1U}};
	const size_t runBytes = RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES;
	const NativeStreamedGraphPublicationPlan plan = NativeStreamedGraphPublicationPlan::derive(
			source({overflowing}), lease, options(runBytes, 128U), [](uint64_t) { return true; });
	ASSERT_EQ(plan.large_value_count, 1U);
	const NativeStreamedGraphCandidateLayout layout = NativeStreamedGraphCandidateLayout::derive(plan, 3U, 3U);
	EXPECT_EQ(layout.large_value_content.count, 3U);
	EXPECT_EQ(layout.large_value_content.first_ordinal, plan.maximumCandidateSegmentCount());
	EXPECT_EQ(layout.large_value_reference_map.count, 3U);
	EXPECT_EQ(layout.large_value_reference_map.first_ordinal, plan.maximumCandidateSegmentCount() + 3U);
	EXPECT_EQ(layout.candidate_count, plan.maximumCandidateSegmentCount() + 6U);
	EXPECT_THROW(static_cast<void>(NativeStreamedGraphCandidateLayout::derive(plan)), FormatException);
	NativeStreamedGraphPublicationPlan constrained = plan;
	constrained.maximum_candidate_segments = plan.maximumCandidateSegmentCount() + 5U;
	EXPECT_THROW(static_cast<void>(NativeStreamedGraphCandidateLayout::derive(constrained, 3U, 3U)), FormatException);
}

TEST(NativeStreamedGraphPublicationPlanTest, FinalPublicationShapeKeepsOnlyScalarPageCardinalities) {
	NativeStreamedGraphPublicationPlan plan;
	plan.record_count = 9U;
	plan.edge_entity_page_count = 2U;
	plan.property_root_count = 7U;
	plan.large_value_count = 3U;
	NativeStreamedGraphLargeValuePlan largeValues;
	largeValues.property_pages = {4U, 7U};
	largeValues.reference_count = 3U;
	largeValues.value_count = 3U;
	largeValues.data_page_count = 11U;
	largeValues.manifest_page_count = 5U;

	const NativeStreamedGraphPublicationShape shape = NativeStreamedGraphPublicationShape::derive(plan, largeValues);
	EXPECT_EQ(shape.entity_page_count, 2U);
	EXPECT_EQ(shape.property_page_count, 4U);
	EXPECT_EQ(shape.property_root_count, 7U);
	EXPECT_EQ(shape.large_value_count, 3U);
	EXPECT_EQ(shape.large_value_data_page_count, 11U);
	EXPECT_EQ(shape.large_value_manifest_page_count, 5U);
	EXPECT_EQ(shape.total_public_page_count, 22U);
}

TEST(NativeStreamedGraphPublicationPlanTest, FinalPublicationShapeRejectsDivergentOverflowCardinality) {
	NativeStreamedGraphPublicationPlan plan;
	plan.record_count = 1U;
	plan.edge_entity_page_count = 1U;
	plan.large_value_count = 1U;
	NativeStreamedGraphLargeValuePlan largeValues;
	largeValues.reference_count = 1U;
	largeValues.value_count = 2U;

	EXPECT_THROW(static_cast<void>(NativeStreamedGraphPublicationShape::derive(plan, largeValues)), FormatException);
}

TEST(NativeStreamedGraphPublicationPlanTest, FinalPublicationShapeRejectsAnIncompleteOverflowPageFamily) {
	NativeStreamedGraphPublicationPlan plan;
	plan.record_count = 1U;
	plan.edge_entity_page_count = 1U;
	plan.large_value_count = 1U;
	NativeStreamedGraphLargeValuePlan largeValues;
	largeValues.property_pages = {1U, 1U};
	largeValues.reference_count = 1U;
	largeValues.value_count = 1U;
	largeValues.data_page_count = 1U;

	EXPECT_THROW(static_cast<void>(NativeStreamedGraphPublicationShape::derive(plan, largeValues)), FormatException);
}

TEST(NativeStreamedGraphPublicationPlanTest, FinalPublicationShapeRejectsInvalidCoreAndOverflowCounters) {
	NativeStreamedGraphPublicationPlan plan;
	plan.record_count = 1U;
	plan.edge_entity_page_count = 1U;
	NativeStreamedGraphLargeValuePlan largeValues;

	plan.record_count = 0U;
	EXPECT_THROW(static_cast<void>(NativeStreamedGraphPublicationShape::derive(plan, largeValues)), FormatException);
	plan.record_count = 1U;
	plan.edge_entity_page_count = 0U;
	EXPECT_THROW(static_cast<void>(NativeStreamedGraphPublicationShape::derive(plan, largeValues)), FormatException);
	plan.edge_entity_page_count = 1U;
	largeValues.property_pages.property_count = 2U;
	EXPECT_THROW(static_cast<void>(NativeStreamedGraphPublicationShape::derive(plan, largeValues)), FormatException);
	largeValues.property_pages.property_count = 0U;

	plan.large_value_count = 1U;
	largeValues.reference_count = 1U;
	largeValues.value_count = 1U;
	largeValues.property_pages = {0U, 1U};
	largeValues.data_page_count = 1U;
	largeValues.manifest_page_count = 1U;
	EXPECT_THROW(static_cast<void>(NativeStreamedGraphPublicationShape::derive(plan, largeValues)), FormatException);
	largeValues.property_pages = {1U, 1U};
	largeValues.data_page_count = 0U;
	largeValues.manifest_page_count = 1U;
	EXPECT_THROW(static_cast<void>(NativeStreamedGraphPublicationShape::derive(plan, largeValues)), FormatException);
	largeValues.data_page_count = 1U;
	largeValues.manifest_page_count = 0U;
	EXPECT_THROW(static_cast<void>(NativeStreamedGraphPublicationShape::derive(plan, largeValues)), FormatException);

	NativeStreamedGraphPublicationPlan tooManyProperties = plan;
	tooManyProperties.property_root_count = 2U;
	tooManyProperties.large_value_count = 0U;
	NativeStreamedGraphLargeValuePlan noLargeValues;
	EXPECT_THROW(static_cast<void>(NativeStreamedGraphPublicationShape::derive(tooManyProperties, noLargeValues)),
				 FormatException);
}

TEST(NativeStreamedGraphPublicationPlanTest, FinalPublicationShapeValidatesEachCardinalityContractIndependently) {
	const auto valid = [] {
		NativeStreamedGraphPublicationPlan plan;
		plan.record_count = 2U;
		plan.edge_entity_page_count = 1U;
		plan.property_root_count = 1U;
		plan.large_value_count = 1U;
		NativeStreamedGraphLargeValuePlan values;
		values.property_pages = {1U, 1U};
		values.reference_count = 1U;
		values.value_count = 1U;
		values.data_page_count = 1U;
		values.manifest_page_count = 1U;
		return std::pair{plan, values};
	};

	{
		auto [plan, values] = valid();
		values.reference_count = 0U;
		EXPECT_THROW(static_cast<void>(NativeStreamedGraphPublicationShape::derive(plan, values)), FormatException);
	}
	{
		auto [plan, values] = valid();
		values.value_count = 0U;
		EXPECT_THROW(static_cast<void>(NativeStreamedGraphPublicationShape::derive(plan, values)), FormatException);
	}
	{
		auto [plan, values] = valid();
		values.value_count = 2U;
		EXPECT_THROW(static_cast<void>(NativeStreamedGraphPublicationShape::derive(plan, values)), FormatException);
	}
	{
		auto [plan, values] = valid();
		values.property_pages.property_count = 0U;
		EXPECT_THROW(static_cast<void>(NativeStreamedGraphPublicationShape::derive(plan, values)), FormatException);
	}
	{
		auto [plan, values] = valid();
		values.property_pages.page_count = 0U;
		EXPECT_THROW(static_cast<void>(NativeStreamedGraphPublicationShape::derive(plan, values)), FormatException);
	}
	{
		auto [plan, values] = valid();
		values.data_page_count = 0U;
		EXPECT_THROW(static_cast<void>(NativeStreamedGraphPublicationShape::derive(plan, values)), FormatException);
	}
	{
		auto [plan, values] = valid();
		values.manifest_page_count = 0U;
		EXPECT_THROW(static_cast<void>(NativeStreamedGraphPublicationShape::derive(plan, values)), FormatException);
	}
}

TEST(NativeStreamedGraphPublicationPlanTest, FinalPublicationShapeRejectsPublicPageCounterOverflow) {
	NativeStreamedGraphPublicationPlan plan;
	plan.record_count = 1U;
	plan.edge_entity_page_count = std::numeric_limits<size_t>::max();
	NativeStreamedGraphLargeValuePlan largeValues;
	largeValues.property_pages.page_count = 1U;

	EXPECT_THROW(static_cast<void>(NativeStreamedGraphPublicationShape::derive(plan, largeValues)), FormatException);
}

TEST(NativeStreamedGraphPublicationPlanTest, PageRangesMapEveryTypedFamilyWithoutAPerPageTable) {
	NativeStreamedGraphPublicationShape shape;
	shape.entity_page_count = 2U;
	shape.property_page_count = 3U;
	shape.large_value_count = 2U;
	shape.large_value_data_page_count = 5U;
	shape.large_value_manifest_page_count = 2U;
	shape.total_public_page_count = 12U;

	const NativeStreamedGraphPageRanges ranges = NativeStreamedGraphPageRanges::derive(PageId(40U), shape);
	EXPECT_EQ(ranges.entityPageAt(0U), PageId(40U));
	EXPECT_EQ(ranges.entityPageAt(1U), PageId(41U));
	EXPECT_EQ(ranges.propertyPageAt(0U), PageId(42U));
	EXPECT_EQ(ranges.propertyPageAt(2U), PageId(44U));
	EXPECT_EQ(ranges.largeValuePageAt(0U), PageId(45U));
	EXPECT_EQ(ranges.largeValuePageAt(4U), PageId(49U));
	EXPECT_EQ(ranges.largeValuePageAt(5U), PageId(50U));
	EXPECT_EQ(ranges.largeValuePageAt(6U), PageId(51U));
	EXPECT_THROW(static_cast<void>(ranges.entityPageAt(2U)), FormatException);
	EXPECT_THROW(static_cast<void>(ranges.propertyPageAt(3U)), FormatException);
}

TEST(NativeStreamedGraphPublicationPlanTest, PageRangesRejectNoncanonicalOrOverflowingShape) {
	NativeStreamedGraphPublicationShape malformed;
	malformed.entity_page_count = 1U;
	malformed.total_public_page_count = 2U;
	EXPECT_THROW(static_cast<void>(NativeStreamedGraphPageRanges::derive(PageId(1U), malformed)), FormatException);

	NativeStreamedGraphPublicationShape overflowing;
	overflowing.entity_page_count = 1U;
	overflowing.total_public_page_count = 2U;
	overflowing.property_page_count = 1U;
	EXPECT_THROW(static_cast<void>(NativeStreamedGraphPageRanges::derive(PageId(std::numeric_limits<uint64_t>::max()),
																		 overflowing)),
				 FormatException);

	NativeStreamedGraphPublicationShape incompleteLargeValue;
	incompleteLargeValue.entity_page_count = 1U;
	incompleteLargeValue.large_value_count = 1U;
	incompleteLargeValue.total_public_page_count = 1U;
	EXPECT_THROW(static_cast<void>(NativeStreamedGraphPageRanges::derive(PageId(1U), incompleteLargeValue)),
				 FormatException);
}

TEST(NativeStreamedGraphPublicationPlanTest, RejectsInvalidMergeFanInBeforeReadingTheSource) {
	NativeBulkEdgeRecord value = record(1U, "REL");
	const std::vector<NativeGraphLogicalIdLeaseRange> lease{{1U, 1U}};
	const size_t runBytes = RSS_NATIVE_BULK_RUN_HEADER_BYTES + NativeBulkEdgeRecordCodec::encodedSize(value) +
							RSS_NATIVE_BULK_RUN_RECORD_HEADER_BYTES + 1U;
	NativeBulkEdgePublicationPreflightOptions invalid = options(runBytes, 128U);
	invalid.maximum_sort_runs = 0U;
	EXPECT_THROW(static_cast<void>(NativeStreamedGraphPublicationPlan::derive(source({value}), lease, invalid,
																			  [](uint64_t) { return true; })),
				 FormatException);
	invalid.maximum_sort_runs = 1U;
	EXPECT_THROW(static_cast<void>(NativeStreamedGraphPublicationPlan::derive(source({value}), lease, invalid,
																			  [](uint64_t) { return true; })),
				 FormatException);
}

TEST(NativeStreamedGraphPublicationPlanTest, RejectsCandidateCountArithmeticOverflow) {
	NativeStreamedGraphPublicationPlan plan;
	plan.token_candidate_segments = std::numeric_limits<size_t>::max();
	plan.token_resolver_candidate_segments = 1U;
	EXPECT_THROW(static_cast<void>(plan.maximumCandidateSegmentCount()), FormatException);
}

TEST(NativeStreamedGraphPublicationPlanTest, RejectsPageRangeShapesWithMissingRequiredFamilies) {
	NativeStreamedGraphPublicationShape shape;
	shape.entity_page_count = 1U;
	shape.total_public_page_count = 1U;

	NativeStreamedGraphPublicationShape dataWithoutLargeValue = shape;
	dataWithoutLargeValue.large_value_data_page_count = 1U;
	dataWithoutLargeValue.total_public_page_count = 2U;
	EXPECT_THROW(static_cast<void>(NativeStreamedGraphPageRanges::derive(PageId(1U), dataWithoutLargeValue)),
				 FormatException);

	NativeStreamedGraphPublicationShape manifestWithoutLargeValue = shape;
	manifestWithoutLargeValue.large_value_manifest_page_count = 1U;
	manifestWithoutLargeValue.total_public_page_count = 2U;
	EXPECT_THROW(static_cast<void>(NativeStreamedGraphPageRanges::derive(PageId(1U), manifestWithoutLargeValue)),
				 FormatException);

	NativeStreamedGraphPublicationShape incompleteLargeValue = shape;
	incompleteLargeValue.large_value_count = 1U;
	incompleteLargeValue.property_page_count = 1U;
	incompleteLargeValue.total_public_page_count = 2U;
	EXPECT_THROW(static_cast<void>(NativeStreamedGraphPageRanges::derive(PageId(1U), incompleteLargeValue)),
				 FormatException);

	NativeStreamedGraphPublicationShape missingLargeValueManifest = shape;
	missingLargeValueManifest.large_value_count = 1U;
	missingLargeValueManifest.property_page_count = 1U;
	missingLargeValueManifest.large_value_data_page_count = 1U;
	missingLargeValueManifest.total_public_page_count = 3U;
	EXPECT_THROW(static_cast<void>(NativeStreamedGraphPageRanges::derive(PageId(1U), missingLargeValueManifest)),
				 FormatException);

	NativeStreamedGraphPublicationShape noEntity = shape;
	noEntity.entity_page_count = 0U;
	noEntity.total_public_page_count = 0U;
	EXPECT_THROW(static_cast<void>(NativeStreamedGraphPageRanges::derive(PageId(1U), noEntity)), FormatException);
}

TEST(NativeStreamedGraphPublicationPlanTest, RejectsInvalidFirstPageAndChecksEmptyOptionalRanges) {
	NativeStreamedGraphPublicationShape shape;
	shape.entity_page_count = 1U;
	shape.total_public_page_count = 1U;
	EXPECT_THROW(static_cast<void>(NativeStreamedGraphPageRanges::derive(PageId{}, shape)), FormatException);

	const NativeStreamedGraphPageRanges ranges = NativeStreamedGraphPageRanges::derive(PageId(7U), shape);
	EXPECT_EQ(ranges.first_property_page, PageId(8U));
	EXPECT_FALSE(ranges.first_large_value_page.isValid());
	EXPECT_THROW(static_cast<void>(ranges.propertyPageAt(0U)), FormatException);
	EXPECT_THROW(static_cast<void>(ranges.largeValuePageAt(0U)), FormatException);
}

TEST(NativeStreamedGraphPublicationPlanTest, RejectsPageRangeArithmeticOverflowAndInvalidAccessState) {
	NativeStreamedGraphPublicationShape propertyDataOverflow;
	propertyDataOverflow.entity_page_count = 1U;
	propertyDataOverflow.property_page_count = std::numeric_limits<size_t>::max();
	propertyDataOverflow.large_value_data_page_count = 1U;
	propertyDataOverflow.large_value_count = 1U;
	propertyDataOverflow.large_value_manifest_page_count = 1U;
	propertyDataOverflow.total_public_page_count = std::numeric_limits<size_t>::max();
	EXPECT_THROW(static_cast<void>(NativeStreamedGraphPageRanges::derive(PageId(1U), propertyDataOverflow)),
				 FormatException);

	NativeStreamedGraphPublicationShape manifestOverflow;
	manifestOverflow.entity_page_count = 1U;
	manifestOverflow.property_page_count = std::numeric_limits<size_t>::max() - 1U;
	manifestOverflow.large_value_count = 1U;
	manifestOverflow.large_value_data_page_count = 1U;
	manifestOverflow.large_value_manifest_page_count = 1U;
	manifestOverflow.total_public_page_count = std::numeric_limits<size_t>::max();
	EXPECT_THROW(static_cast<void>(NativeStreamedGraphPageRanges::derive(PageId(1U), manifestOverflow)),
				 FormatException);

	NativeStreamedGraphPublicationShape entityOverflow;
	entityOverflow.entity_page_count = std::numeric_limits<size_t>::max();
	entityOverflow.property_page_count = 1U;
	entityOverflow.total_public_page_count = std::numeric_limits<size_t>::max();
	EXPECT_THROW(static_cast<void>(NativeStreamedGraphPageRanges::derive(PageId(1U), entityOverflow)), FormatException);

	NativeStreamedGraphPublicationShape pageIdOverflow;
	pageIdOverflow.entity_page_count = 2U;
	pageIdOverflow.total_public_page_count = 2U;
	EXPECT_THROW(static_cast<void>(NativeStreamedGraphPageRanges::derive(PageId(std::numeric_limits<uint64_t>::max()),
																		 pageIdOverflow)),
				 FormatException);

	NativeStreamedGraphPublicationShape zeroTotal = entityOverflow;
	zeroTotal.entity_page_count = 1U;
	zeroTotal.property_page_count = 0U;
	zeroTotal.total_public_page_count = 0U;
	EXPECT_THROW(static_cast<void>(NativeStreamedGraphPageRanges::derive(PageId(1U), zeroTotal)), FormatException);

	NativeStreamedGraphPageRanges uninitialized;
	EXPECT_THROW(static_cast<void>(uninitialized.entityPageAt(0U)), FormatException);
	EXPECT_THROW(static_cast<void>(uninitialized.propertyPageAt(0U)), FormatException);
	EXPECT_THROW(static_cast<void>(uninitialized.largeValuePageAt(0U)), FormatException);
}
