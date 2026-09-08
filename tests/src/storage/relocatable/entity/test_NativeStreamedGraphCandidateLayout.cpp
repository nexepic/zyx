/**
 * @file test_NativeStreamedGraphCandidateLayout.cpp
 * @brief Immutable private-candidate layout tests for streamed Edge publication.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#include <gtest/gtest.h>

#include <limits>

#include "graph/storage/relocatable/entity/NativeStreamedGraphCandidateLayout.hpp"

using namespace graph::storage;

TEST(NativeStreamedGraphCandidateLayoutTest, PartitionsEveryCandidateFamilyWithoutOverlap) {
	NativeStreamedGraphPublicationPlan plan;
	plan.record_count = 7U;
	plan.source_run_count = 3U;
	plan.token_candidate_segments = 5U;
	plan.token_resolver_candidate_segments = 7U;
	plan.outgoing_adjacency_candidate_segments = 11U;
	plan.incoming_adjacency_candidate_segments = 13U;
	const NativeStreamedGraphCandidateLayout layout = NativeStreamedGraphCandidateLayout::derive(plan);
	EXPECT_EQ(layout.tokens.first_ordinal, 0U);
	EXPECT_EQ(layout.token_resolver.first_ordinal, 5U);
	EXPECT_EQ(layout.outgoing_adjacency.first_ordinal, 12U);
	EXPECT_EQ(layout.incoming_adjacency.first_ordinal, 23U);
	EXPECT_EQ(layout.candidate_count, 36U);
	for (size_t ordinal = 0U; ordinal < layout.candidate_count; ++ordinal) {
		const size_t memberships = static_cast<size_t>(layout.tokens.contains(ordinal)) +
								   static_cast<size_t>(layout.token_resolver.contains(ordinal)) +
								   static_cast<size_t>(layout.outgoing_adjacency.contains(ordinal)) +
								   static_cast<size_t>(layout.incoming_adjacency.contains(ordinal));
		EXPECT_EQ(memberships, 1U);
	}
}

TEST(NativeStreamedGraphCandidateLayoutTest, RejectsAnEmptyPlanAndAllowsAStreamWithoutTokenWork) {
	EXPECT_THROW(static_cast<void>(NativeStreamedGraphCandidateLayout::derive({})), FormatException);
	NativeStreamedGraphPublicationPlan noSourceRuns;
	noSourceRuns.record_count = 1U;
	noSourceRuns.outgoing_adjacency_candidate_segments = 1U;
	noSourceRuns.incoming_adjacency_candidate_segments = 1U;
	EXPECT_THROW(static_cast<void>(NativeStreamedGraphCandidateLayout::derive(noSourceRuns)), FormatException);
	NativeStreamedGraphPublicationPlan untyped;
	untyped.record_count = 1U;
	untyped.source_run_count = 1U;
	untyped.outgoing_adjacency_candidate_segments = 1U;
	untyped.incoming_adjacency_candidate_segments = 1U;
	const NativeStreamedGraphCandidateLayout layout = NativeStreamedGraphCandidateLayout::derive(untyped);
	EXPECT_EQ(layout.tokens.count, 0U);
	EXPECT_EQ(layout.token_resolver.count, 0U);
	EXPECT_EQ(layout.candidate_count, 2U);
}

TEST(NativeStreamedGraphCandidateLayoutTest, RejectsMalformedLargeValueFamilyCombinations) {
	NativeStreamedGraphPublicationPlan plan;
	plan.record_count = 1U;
	plan.source_run_count = 1U;
	plan.outgoing_adjacency_candidate_segments = 1U;
	plan.incoming_adjacency_candidate_segments = 1U;

	EXPECT_THROW(static_cast<void>(NativeStreamedGraphCandidateLayout::derive(plan, 1U, 0U)), FormatException);
	plan.large_value_count = 1U;
	EXPECT_THROW(static_cast<void>(NativeStreamedGraphCandidateLayout::derive(plan, 0U, 0U)), FormatException);
	EXPECT_THROW(static_cast<void>(NativeStreamedGraphCandidateLayout::derive(plan, 1U, 0U)), FormatException);
}

TEST(NativeStreamedGraphCandidateLayoutTest, RejectsCandidateOrdinalOverflowAndMismatchedBudget) {
	NativeStreamedGraphPublicationPlan overflowing;
	overflowing.record_count = 1U;
	overflowing.source_run_count = 1U;
	overflowing.token_candidate_segments = std::numeric_limits<size_t>::max();
	overflowing.token_resolver_candidate_segments = 1U;
	EXPECT_THROW(static_cast<void>(NativeStreamedGraphCandidateLayout::derive(overflowing)), FormatException);
	NativeStreamedGraphPublicationPlan largeValueOverflow;
	largeValueOverflow.record_count = 1U;
	largeValueOverflow.source_run_count = 1U;
	largeValueOverflow.outgoing_adjacency_candidate_segments = 1U;
	largeValueOverflow.incoming_adjacency_candidate_segments = 1U;
	largeValueOverflow.large_value_count = 1U;
	EXPECT_THROW(static_cast<void>(NativeStreamedGraphCandidateLayout::derive(
				largeValueOverflow, std::numeric_limits<size_t>::max(), 1U)),
				 FormatException);

	NativeStreamedGraphPublicationPlan bounded;
	bounded.record_count = 1U;
	bounded.source_run_count = 1U;
	bounded.outgoing_adjacency_candidate_segments = 1U;
	bounded.incoming_adjacency_candidate_segments = 1U;
	bounded.maximum_candidate_segments = 1U;
	EXPECT_THROW(static_cast<void>(NativeStreamedGraphCandidateLayout::derive(bounded)), FormatException);
	bounded.maximum_candidate_segments = 2U;
	EXPECT_EQ(NativeStreamedGraphCandidateLayout::derive(bounded).candidate_count, 2U);
}
