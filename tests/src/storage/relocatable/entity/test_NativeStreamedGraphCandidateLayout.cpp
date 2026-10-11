/**
 * @file test_NativeStreamedGraphCandidateLayout.cpp
 * @brief Immutable private-candidate layout tests for streamed Edge publication.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#include <gtest/gtest.h>

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
