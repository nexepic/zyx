/**
 * @file test_PipelineValidator_DefaultMode.cpp
 * @brief Additional branch coverage for PipelineValidator default switch cases.
 */

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

class PipelineValidatorDefaultModeTest : public ::testing::Test {
protected:
	fs::path testFilePath;
	std::shared_ptr<graph::storage::FileStorage> storage;
	std::shared_ptr<graph::query::QueryPlanner> planner;

	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_pv_dm_" + to_string(uuid) + ".zyx");

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

// Cover the default case in the physical ensureValidPipeline switch
// by passing an invalid ValidationMode enum value
TEST_F(PipelineValidatorDefaultModeTest, Physical_InvalidModeThrows) {
	auto invalidMode = static_cast<graph::query::PipelineValidator::ValidationMode>(99);
	EXPECT_THROW(
		(void)graph::query::PipelineValidator::ensureValidPipeline(
			nullptr, planner, "TEST", invalidMode),
		std::runtime_error);
}

// Cover the default case in the logical ensureValidLogicalPipeline switch
TEST_F(PipelineValidatorDefaultModeTest, Logical_InvalidModeThrows) {
	auto invalidMode = static_cast<graph::query::PipelineValidator::ValidationMode>(99);
	EXPECT_THROW(
		(void)graph::query::PipelineValidator::ensureValidLogicalPipeline(
			nullptr, "TEST", invalidMode),
		std::runtime_error);
}

// Verify the error message contains the clause name for default mode
TEST_F(PipelineValidatorDefaultModeTest, Physical_InvalidModeErrorContainsClauseName) {
	auto invalidMode = static_cast<graph::query::PipelineValidator::ValidationMode>(42);
	try {
		(void)graph::query::PipelineValidator::ensureValidPipeline(
			nullptr, planner, "FOOBAR", invalidMode);
		FAIL() << "Expected runtime_error";
	} catch (const std::runtime_error &e) {
		std::string msg = e.what();
		EXPECT_TRUE(msg.find("FOOBAR") != std::string::npos);
	}
}

TEST_F(PipelineValidatorDefaultModeTest, Logical_InvalidModeErrorContainsClauseName) {
	auto invalidMode = static_cast<graph::query::PipelineValidator::ValidationMode>(42);
	try {
		(void)graph::query::PipelineValidator::ensureValidLogicalPipeline(
			nullptr, "BAZQUX", invalidMode);
		FAIL() << "Expected runtime_error";
	} catch (const std::runtime_error &e) {
		std::string msg = e.what();
		EXPECT_TRUE(msg.find("BAZQUX") != std::string::npos);
	}
}
