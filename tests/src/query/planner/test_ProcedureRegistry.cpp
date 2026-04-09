/**
 * @file test_ProcedureRegistry.cpp
 * @author Nexepic
 * @date 2026/1/16
 *
 * @copyright Copyright (c) 2026 Nexepic
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 **/

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <vector>

// Your Headers
#include "graph/query/algorithm/GraphProjectionManager.hpp"
#include "graph/query/planner/ProcedureRegistry.hpp"
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/indexes/IndexManager.hpp"

// Operator Headers for RTTI checks
#include "graph/query/execution/operators/AlgoShortestPathOperator.hpp"
#include "graph/query/execution/operators/CreateIndexOperator.hpp"
#include "graph/query/execution/operators/DropIndexOperator.hpp"
#include "graph/query/execution/operators/GdsOperators.hpp"
#include "graph/query/execution/operators/ListConfigOperator.hpp"
#include "graph/query/execution/operators/SetConfigOperator.hpp"
#include "graph/query/execution/operators/TrainVectorIndexOperator.hpp"
#include "graph/query/execution/operators/VectorSearchOperator.hpp"

namespace fs = std::filesystem;
using namespace graph;
using namespace graph::query::planner;
using namespace graph::query::execution;

class ProcedureRegistryTest : public ::testing::Test {
protected:
	std::filesystem::path testFilePath;
	std::shared_ptr<graph::storage::FileStorage> storage;
	std::shared_ptr<graph::storage::DataManager> dataManager;
	std::shared_ptr<graph::query::indexes::IndexManager> indexManager;

	// Test Target: Created as unique_ptr to test isolation
	std::unique_ptr<ProcedureRegistry> registry;
	ProcedureContext ctx;

	void SetUp() override {
		// 1. Infrastructure
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_proc_" + to_string(uuid) + ".db");

		storage = std::make_shared<graph::storage::FileStorage>(testFilePath.string(), 4096,
																graph::storage::OpenMode::OPEN_CREATE_NEW_FILE);
		storage->open();
		dataManager = storage->getDataManager();
		indexManager = std::make_shared<graph::query::indexes::IndexManager>(storage);

		// 2. Context
		ctx.dataManager = dataManager;
		ctx.indexManager = indexManager;
		ctx.projectionManager = std::make_shared<graph::query::algorithm::GraphProjectionManager>();

		// 3. Registry
		// Now that the constructor is public, we can create a fresh instance.
		// This instance will run the constructor code in ProcedureRegistry.cpp
		// and register all built-in procedures.
		registry = std::make_unique<ProcedureRegistry>();
	}

	void TearDown() override {
		if (storage)
			storage->close();
		std::error_code ec;
		if (fs::exists(testFilePath))
			fs::remove(testFilePath, ec);
	}

	// Helper
	std::unique_ptr<PhysicalOperator> invoke(const std::string &name, const std::vector<PropertyValue> &args) const {
		// Use 'get' as per your API
		auto factory = registry->get(name);
		if (!factory)
			return nullptr;
		return factory(ctx, args);
	}
};

// ============================================================================
// TESTS
// ============================================================================

TEST_F(ProcedureRegistryTest, DbmsSetConfig_Success) {
	std::vector<PropertyValue> args = {PropertyValue("key"), PropertyValue("val")};
	auto op = invoke("dbms.setConfig", args);

	ASSERT_NE(op, nullptr);
	EXPECT_NE(dynamic_cast<operators::SetConfigOperator *>(op.get()), nullptr);
}

TEST_F(ProcedureRegistryTest, DbmsSetConfig_Fail_Args) {
	std::vector<PropertyValue> args = {PropertyValue("key_only")};
	EXPECT_THROW(invoke("dbms.setConfig", args), std::runtime_error);
}

TEST_F(ProcedureRegistryTest, DbmsListConfig_Success) {
	auto op = invoke("dbms.listConfig", {});
	ASSERT_NE(op, nullptr);
	EXPECT_NE(dynamic_cast<operators::ListConfigOperator *>(op.get()), nullptr);
}

TEST_F(ProcedureRegistryTest, DbmsGetConfig_Success) {
	std::vector<PropertyValue> args = {PropertyValue("key")};
	auto op = invoke("dbms.getConfig", args);

	ASSERT_NE(op, nullptr);
	EXPECT_NE(dynamic_cast<operators::ListConfigOperator *>(op.get()), nullptr);
}

TEST_F(ProcedureRegistryTest, DbmsGetConfig_Fail_Args) {
	EXPECT_THROW(invoke("dbms.getConfig", {}), std::runtime_error);
}

TEST_F(ProcedureRegistryTest, DbmsGetConfig_Fail_TooManyArgs) {
	std::vector<PropertyValue> args = {PropertyValue("key"), PropertyValue("extra")};
	EXPECT_THROW(invoke("dbms.getConfig", args), std::runtime_error);
}

TEST_F(ProcedureRegistryTest, DbmsSetConfig_Fail_TooManyArgs) {
	std::vector<PropertyValue> args = {PropertyValue("key"), PropertyValue("val"), PropertyValue("extra")};
	EXPECT_THROW(invoke("dbms.setConfig", args), std::runtime_error);
}

TEST_F(ProcedureRegistryTest, AlgoShortestPath_Success) {
	std::vector<PropertyValue> args = {PropertyValue("1"), PropertyValue("2")};
	auto op = invoke("algo.shortestPath", args);

	ASSERT_NE(op, nullptr);
	EXPECT_NE(dynamic_cast<operators::AlgoShortestPathOperator *>(op.get()), nullptr);
}

TEST_F(ProcedureRegistryTest, AlgoShortestPath_Fail_Args) {
	std::vector<PropertyValue> args = {PropertyValue("1")};
	EXPECT_THROW(invoke("algo.shortestPath", args), std::runtime_error);
}

TEST_F(ProcedureRegistryTest, AlgoShortestPath_Fail_Type) {
	const std::vector<PropertyValue> args = {PropertyValue("abc"), PropertyValue("2")};
	try {
		(void) invoke("algo.shortestPath", args);
		FAIL();
	} catch (const std::runtime_error &e) {
		EXPECT_STREQ(e.what(), "IDs must be numeric");
	}
}

TEST_F(ProcedureRegistryTest, DbmsCreateLabelIndex) {
	auto op = invoke("dbms.createLabelIndex", {});
	ASSERT_NE(op, nullptr);
	EXPECT_NE(dynamic_cast<operators::CreateIndexOperator *>(op.get()), nullptr);
}

TEST_F(ProcedureRegistryTest, DbmsDropLabelIndex) {
	auto op = invoke("dbms.dropLabelIndex", {});
	ASSERT_NE(op, nullptr);
	EXPECT_NE(dynamic_cast<operators::DropIndexOperator *>(op.get()), nullptr);
}

TEST_F(ProcedureRegistryTest, UnknownProcedure) {
	auto op = invoke("unknown", {});
	EXPECT_EQ(op, nullptr);
}

TEST_F(ProcedureRegistryTest, ManualRegistration) {
	// Test the registerProcedure API
	bool called = false;
	registry->registerProcedure("custom.proc", [&](const ProcedureContext &, const std::vector<PropertyValue> &) {
		called = true;
		// Return a dummy pointer or nullptr (test context doesn't strictly need a valid Op if we just verify logic)
		return nullptr;
	});

	auto factory = registry->get("custom.proc");
	ASSERT_TRUE(factory);
	(void) factory(ctx, {});
	EXPECT_TRUE(called);
}

TEST_F(ProcedureRegistryTest, VectorIndexQueryNodes_Success) {
	// Create a vector argument as a LIST of floats
	std::vector<PropertyValue> vec = {PropertyValue(1.0), PropertyValue(2.0), PropertyValue(3.0)};
	std::vector<PropertyValue> args = {
		PropertyValue("test_index"),
		PropertyValue("5"),
		PropertyValue(vec)
	};

	auto op = invoke("db.index.vector.queryNodes", args);

	ASSERT_NE(op, nullptr);
	EXPECT_NE(dynamic_cast<operators::VectorSearchOperator *>(op.get()), nullptr);
}

TEST_F(ProcedureRegistryTest, VectorIndexQueryNodes_IntegerVector_Success) {
	std::vector<PropertyValue> vec = {PropertyValue(int64_t(1)), PropertyValue(int64_t(2)), PropertyValue(int64_t(3))};
	std::vector<PropertyValue> args = {
		PropertyValue("test_index"),
		PropertyValue("5"),
		PropertyValue(vec)
	};

	auto op = invoke("db.index.vector.queryNodes", args);

	ASSERT_NE(op, nullptr);
	EXPECT_NE(dynamic_cast<operators::VectorSearchOperator *>(op.get()), nullptr);
}

TEST_F(ProcedureRegistryTest, VectorIndexQueryNodes_Fail_Args_TooFew) {
	// Test with too few arguments
	std::vector<PropertyValue> args = {PropertyValue("test_index"), PropertyValue("5")};
	EXPECT_THROW(invoke("db.index.vector.queryNodes", args), std::runtime_error);
}

TEST_F(ProcedureRegistryTest, VectorIndexQueryNodes_Fail_Args_TooMany) {
	// Test with too many arguments
	std::vector<PropertyValue> vec = {PropertyValue(1.0), PropertyValue(2.0)};
	std::vector<PropertyValue> args = {
		PropertyValue("test_index"),
		PropertyValue("5"),
		PropertyValue(vec),
		PropertyValue("extra")
	};
	EXPECT_THROW(invoke("db.index.vector.queryNodes", args), std::runtime_error);
}

TEST_F(ProcedureRegistryTest, VectorIndexQueryNodes_Fail_WrongType) {
	// Test with wrong type for queryVector (not a LIST)
	std::vector<PropertyValue> args = {
		PropertyValue("test_index"),
		PropertyValue("5"),
		PropertyValue("not_a_list")
	};
	EXPECT_THROW(invoke("db.index.vector.queryNodes", args), std::runtime_error);
}

TEST_F(ProcedureRegistryTest, VectorIndexQueryNodes_Fail_NonNumericVectorElement) {
	std::vector<PropertyValue> vec = {PropertyValue(1.0), PropertyValue("bad_value")};
	std::vector<PropertyValue> args = {
		PropertyValue("test_index"),
		PropertyValue("5"),
		PropertyValue(vec)
	};
	EXPECT_THROW(invoke("db.index.vector.queryNodes", args), std::runtime_error);
}

TEST_F(ProcedureRegistryTest, VectorIndexTrain_Success) {
	std::vector<PropertyValue> args = {PropertyValue("test_index")};
	auto op = invoke("db.index.vector.train", args);

	ASSERT_NE(op, nullptr);
	EXPECT_NE(dynamic_cast<operators::TrainVectorIndexOperator *>(op.get()), nullptr);
}

TEST_F(ProcedureRegistryTest, VectorIndexTrain_Fail_Args_TooFew) {
	// Test with too few arguments
	EXPECT_THROW(invoke("db.index.vector.train", {}), std::runtime_error);
}

TEST_F(ProcedureRegistryTest, VectorIndexTrain_Fail_Args_TooMany) {
	// Test with too many arguments
	std::vector<PropertyValue> args = {PropertyValue("test_index"), PropertyValue("extra")};
	EXPECT_THROW(invoke("db.index.vector.train", args), std::runtime_error);
}

// ============================================================================
// GDS Procedure Argument Validation Tests
// ============================================================================

TEST_F(ProcedureRegistryTest, GdsGraphProject_Success) {
	std::vector<PropertyValue> args = {PropertyValue("g"), PropertyValue("Person"), PropertyValue("KNOWS")};
	auto op = invoke("gds.graph.project", args);
	ASSERT_NE(op, nullptr);
	EXPECT_NE(dynamic_cast<operators::GdsGraphProjectOperator *>(op.get()), nullptr);
}

TEST_F(ProcedureRegistryTest, GdsGraphProject_WithWeightProperty) {
	std::vector<PropertyValue> args = {PropertyValue("g"), PropertyValue("City"), PropertyValue("ROAD"), PropertyValue("dist")};
	auto op = invoke("gds.graph.project", args);
	ASSERT_NE(op, nullptr);
}

TEST_F(ProcedureRegistryTest, GdsGraphProject_Fail_TooFewArgs) {
	std::vector<PropertyValue> args = {PropertyValue("g"), PropertyValue("Person")};
	EXPECT_THROW(invoke("gds.graph.project", args), std::runtime_error);
}

TEST_F(ProcedureRegistryTest, GdsGraphDrop_Success) {
	// First create a projection to have something to drop
	std::vector<PropertyValue> createArgs = {PropertyValue("toDrop"), PropertyValue("X"), PropertyValue("Y")};
	(void) invoke("gds.graph.project", createArgs);

	std::vector<PropertyValue> args = {PropertyValue("toDrop")};
	auto op = invoke("gds.graph.drop", args);
	ASSERT_NE(op, nullptr);
}

TEST_F(ProcedureRegistryTest, GdsGraphDrop_Fail_NoArgs) {
	EXPECT_THROW(invoke("gds.graph.drop", {}), std::runtime_error);
}

TEST_F(ProcedureRegistryTest, GdsDijkstra_Success) {
	std::vector<PropertyValue> args = {PropertyValue("g"), PropertyValue("1"), PropertyValue("2")};
	auto op = invoke("gds.shortestPath.dijkstra.stream", args);
	ASSERT_NE(op, nullptr);
}

TEST_F(ProcedureRegistryTest, GdsDijkstra_Fail_TooFewArgs) {
	std::vector<PropertyValue> args = {PropertyValue("g"), PropertyValue("1")};
	EXPECT_THROW(invoke("gds.shortestPath.dijkstra.stream", args), std::runtime_error);
}

TEST_F(ProcedureRegistryTest, GdsPageRank_Success) {
	std::vector<PropertyValue> args = {PropertyValue("g")};
	auto op = invoke("gds.pageRank.stream", args);
	ASSERT_NE(op, nullptr);
}

TEST_F(ProcedureRegistryTest, GdsPageRank_Fail_NoArgs) {
	EXPECT_THROW(invoke("gds.pageRank.stream", {}), std::runtime_error);
}

TEST_F(ProcedureRegistryTest, GdsWcc_Success) {
	std::vector<PropertyValue> args = {PropertyValue("g")};
	auto op = invoke("gds.wcc.stream", args);
	ASSERT_NE(op, nullptr);
}

TEST_F(ProcedureRegistryTest, GdsWcc_Fail_NoArgs) {
	EXPECT_THROW(invoke("gds.wcc.stream", {}), std::runtime_error);
}

TEST_F(ProcedureRegistryTest, GdsBetweenness_Success) {
	std::vector<PropertyValue> args = {PropertyValue("g")};
	auto op = invoke("gds.betweenness.stream", args);
	ASSERT_NE(op, nullptr);
}

TEST_F(ProcedureRegistryTest, GdsBetweenness_WithSampling) {
	std::vector<PropertyValue> args = {PropertyValue("g"), PropertyValue("5")};
	auto op = invoke("gds.betweenness.stream", args);
	ASSERT_NE(op, nullptr);
}

TEST_F(ProcedureRegistryTest, GdsBetweenness_Fail_NoArgs) {
	EXPECT_THROW(invoke("gds.betweenness.stream", {}), std::runtime_error);
}

TEST_F(ProcedureRegistryTest, GdsCloseness_Success) {
	std::vector<PropertyValue> args = {PropertyValue("g")};
	auto op = invoke("gds.closeness.stream", args);
	ASSERT_NE(op, nullptr);
}

TEST_F(ProcedureRegistryTest, GdsCloseness_Fail_NoArgs) {
	EXPECT_THROW(invoke("gds.closeness.stream", {}), std::runtime_error);
}
