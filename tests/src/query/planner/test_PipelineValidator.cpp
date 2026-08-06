/**
 * @file test_PipelineValidator.cpp
 * @brief Unit tests for native logical PipelineValidator behavior.
 **/

#include <memory>
#include <stdexcept>

#include <gtest/gtest.h>

#include "graph/query/planner/PipelineValidator.hpp"
#include "graph/query/logical/operators/LogicalSingleRow.hpp"
#include "graph/query/logical/LogicalOperator.hpp"

TEST(PipelineValidatorTest, LogicalNonNullPassThrough) {
	auto op = std::make_unique<graph::query::logical::LogicalSingleRow>();
	auto *rawPtr = op.get();

	auto result = graph::query::PipelineValidator::ensureValidLogicalPipeline(
		std::move(op), "RETURN",
		graph::query::PipelineValidator::ValidationMode::ALLOW_EMPTY);

	EXPECT_EQ(result.get(), rawPtr);
}

TEST(PipelineValidatorTest, LogicalAllowEmptyInjectsSingleRow) {
	auto result = graph::query::PipelineValidator::ensureValidLogicalPipeline(
		nullptr, "RETURN",
		graph::query::PipelineValidator::ValidationMode::ALLOW_EMPTY);

	ASSERT_NE(result, nullptr);
	auto *singleRow = dynamic_cast<graph::query::logical::LogicalSingleRow *>(result.get());
	EXPECT_NE(singleRow, nullptr);
}

TEST(PipelineValidatorTest, LogicalRequirePrecedingThrows) {
	EXPECT_THROW(
		(void)graph::query::PipelineValidator::ensureValidLogicalPipeline(
			nullptr, "WITH",
			graph::query::PipelineValidator::ValidationMode::REQUIRE_PRECEDING),
		std::runtime_error);
}

TEST(PipelineValidatorTest, LogicalRequirePrecedingReportsTheClauseName) {
	try {
		(void)graph::query::PipelineValidator::ensureValidLogicalPipeline(
			nullptr, "REMOVE",
			graph::query::PipelineValidator::ValidationMode::REQUIRE_PRECEDING);
		FAIL() << "Expected runtime_error";
	} catch (const std::runtime_error &e) {
		std::string msg = e.what();
		EXPECT_TRUE(msg.find("REMOVE") != std::string::npos);
	}
}
