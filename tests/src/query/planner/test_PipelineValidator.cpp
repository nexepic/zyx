/**
 * @file test_PipelineValidator.cpp
 * @brief Unit tests for PipelineValidator — covers all validation modes and both pipeline types.
 **/

#include <gtest/gtest.h>
#include <memory>

#include "graph/query/planner/PipelineValidator.hpp"
#include "graph/query/planner/QueryPlanner.hpp"
#include "graph/query/execution/operators/SingleRowOperator.hpp"
#include "graph/query/logical/operators/LogicalSingleRow.hpp"
#include "graph/query/logical/LogicalOperator.hpp"

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>

#include "graph/storage/FileStorage.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/indexes/IndexManager.hpp"

namespace fs = std::filesystem;

class PipelineValidatorTest : public ::testing::Test {
protected:
	fs::path testFilePath;
	std::shared_ptr<graph::storage::FileStorage> storage;
	std::shared_ptr<graph::query::QueryPlanner> planner;

	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_pv_" + to_string(uuid) + ".zyx");

		storage = std::make_shared<graph::storage::FileStorage>(
			testFilePath.string(), 4096, graph::storage::OpenMode::OPEN_CREATE_NEW_FILE);
		storage->open();

		auto dm = storage->getDataManager();
		auto im = std::make_shared<graph::query::indexes::IndexManager>(storage);
		planner = std::make_shared<graph::query::QueryPlanner>(dm, im);
	}

	void TearDown() override {
		if (storage) storage->close();
		std::error_code ec;
		if (fs::exists(testFilePath)) fs::remove(testFilePath, ec);
	}
};

// ============================================================================
// Physical pipeline: ensureValidPipeline
// ============================================================================

TEST_F(PipelineValidatorTest, Physical_NonNullPassThrough) {
	auto op = graph::query::QueryPlanner::singleRowOp();
	auto *rawPtr = op.get();

	auto result = graph::query::PipelineValidator::ensureValidPipeline(
		std::move(op), planner, "RETURN",
		graph::query::PipelineValidator::ValidationMode::ALLOW_EMPTY);

	EXPECT_EQ(result.get(), rawPtr);
}

TEST_F(PipelineValidatorTest, Physical_AllowEmpty_InjectsSingleRow) {
	auto result = graph::query::PipelineValidator::ensureValidPipeline(
		nullptr, planner, "RETURN",
		graph::query::PipelineValidator::ValidationMode::ALLOW_EMPTY);

	ASSERT_NE(result, nullptr);
	auto *singleRow = dynamic_cast<graph::query::execution::operators::SingleRowOperator *>(result.get());
	EXPECT_NE(singleRow, nullptr);
}

TEST_F(PipelineValidatorTest, Physical_RequirePreceding_Throws) {
	EXPECT_THROW(
		(void)graph::query::PipelineValidator::ensureValidPipeline(
			nullptr, planner, "SET",
			graph::query::PipelineValidator::ValidationMode::REQUIRE_PRECEDING),
		std::runtime_error);
}

TEST_F(PipelineValidatorTest, Physical_RequirePreceding_ErrorMessage) {
	try {
		(void)graph::query::PipelineValidator::ensureValidPipeline(
			nullptr, planner, "DELETE",
			graph::query::PipelineValidator::ValidationMode::REQUIRE_PRECEDING);
		FAIL() << "Expected runtime_error";
	} catch (const std::runtime_error &e) {
		std::string msg = e.what();
		EXPECT_TRUE(msg.find("DELETE") != std::string::npos);
		EXPECT_TRUE(msg.find("MATCH or CREATE") != std::string::npos);
	}
}

// ============================================================================
// Logical pipeline: ensureValidLogicalPipeline
// ============================================================================

TEST_F(PipelineValidatorTest, Logical_NonNullPassThrough) {
	auto op = std::make_unique<graph::query::logical::LogicalSingleRow>();
	auto *rawPtr = op.get();

	auto result = graph::query::PipelineValidator::ensureValidLogicalPipeline(
		std::move(op), "RETURN",
		graph::query::PipelineValidator::ValidationMode::ALLOW_EMPTY);

	EXPECT_EQ(result.get(), rawPtr);
}

TEST_F(PipelineValidatorTest, Logical_AllowEmpty_InjectsSingleRow) {
	auto result = graph::query::PipelineValidator::ensureValidLogicalPipeline(
		nullptr, "RETURN",
		graph::query::PipelineValidator::ValidationMode::ALLOW_EMPTY);

	ASSERT_NE(result, nullptr);
	auto *singleRow = dynamic_cast<graph::query::logical::LogicalSingleRow *>(result.get());
	EXPECT_NE(singleRow, nullptr);
}

TEST_F(PipelineValidatorTest, Logical_RequirePreceding_Throws) {
	EXPECT_THROW(
		(void)graph::query::PipelineValidator::ensureValidLogicalPipeline(
			nullptr, "WITH",
			graph::query::PipelineValidator::ValidationMode::REQUIRE_PRECEDING),
		std::runtime_error);
}

TEST_F(PipelineValidatorTest, Logical_RequirePreceding_ErrorMessage) {
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
