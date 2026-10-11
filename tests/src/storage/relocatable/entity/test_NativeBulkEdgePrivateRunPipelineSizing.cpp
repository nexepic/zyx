/**
 * @file test_NativeBulkEdgePrivateRunPipelineSizing.cpp
 * @brief Candidate sizing tests for bounded private run pipelines.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#include <limits>

#include <gtest/gtest.h>

#include "graph/storage/relocatable/entity/NativeBulkEdgePrivateRunPipeline.hpp"

namespace graph::storage {

	TEST(NativeBulkEdgePrivateRunPipelineTest, CandidateSizingRetainsEachReductionLevel) {
		EXPECT_EQ(NativeBulkEdgePrivateRunPipeline::maximumCandidateSegments(0U, 2U), 0U);
		EXPECT_EQ(NativeBulkEdgePrivateRunPipeline::maximumCandidateSegments(1U, 2U), 3U);
		EXPECT_EQ(NativeBulkEdgePrivateRunPipeline::maximumCandidateSegments(2U, 2U), 12U);
		EXPECT_EQ(NativeBulkEdgePrivateRunPipeline::maximumCandidateSegments(5U, 2U), 60U);
		EXPECT_THROW(static_cast<void>(NativeBulkEdgePrivateRunPipeline::maximumCandidateSegments(1U, 1U)),
					 FormatException);
		EXPECT_THROW(static_cast<void>(NativeBulkEdgePrivateRunPipeline::maximumCandidateSegments(
							 std::numeric_limits<size_t>::max(), 2U)),
					 FormatException);
	}

} // namespace graph::storage
