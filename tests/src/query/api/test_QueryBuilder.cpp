/**
 * @file test_QueryBuilder.cpp
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

#include "graph/query/api/QueryBuilder.hpp"
#include "graph/query/planner/QueryPlanner.hpp"
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/indexes/IndexManager.hpp"

// Operator headers for dynamic_cast verification
#include "graph/query/execution/operators/CartesianProductOperator.hpp"
#include "graph/query/execution/operators/CreateEdgeOperator.hpp"
#include "graph/query/execution/operators/CreateIndexOperator.hpp"
#include "graph/query/execution/operators/CreateNodeOperator.hpp"
#include "graph/query/execution/operators/DeleteOperator.hpp"
#include "graph/query/execution/operators/DropIndexOperator.hpp"
#include "graph/query/execution/operators/FilterOperator.hpp"
#include "graph/query/execution/operators/LimitOperator.hpp"
#include "graph/query/execution/operators/ListConfigOperator.hpp"
#include "graph/query/execution/operators/ProjectOperator.hpp"
#include "graph/query/execution/operators/RemoveOperator.hpp"
#include "graph/query/execution/operators/SetOperator.hpp"
#include "graph/query/execution/operators/ShowIndexesOperator.hpp"
#include "graph/query/execution/operators/SortOperator.hpp"
#include "graph/query/execution/operators/UnwindOperator.hpp"

namespace fs = std::filesystem;
using namespace graph;
using namespace graph::query;
using namespace graph::query::execution;

class QueryBuilderTest : public ::testing::Test {
protected:
	std::filesystem::path testFilePath;
	std::shared_ptr<storage::FileStorage> storage;
	std::shared_ptr<storage::DataManager> dataManager;
	std::shared_ptr<indexes::IndexManager> indexManager;
	std::shared_ptr<QueryPlanner> planner;

	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_builder_" + to_string(uuid) + ".db");

		storage =
				std::make_shared<storage::FileStorage>(testFilePath.string(), 4096, storage::OpenMode::OPEN_CREATE_NEW_FILE);
		storage->open();
		dataManager = storage->getDataManager();
		indexManager = std::make_shared<indexes::IndexManager>(storage);
		planner = std::make_shared<QueryPlanner>(dataManager, indexManager);
	}

	void TearDown() override {
		if (storage)
			storage->close();
		if (fs::exists(testFilePath))
			fs::remove(testFilePath);
	}
};

// ============================================================================
// READ Operations
// ============================================================================

TEST_F(QueryBuilderTest, Match_Basic) {
	QueryBuilder qb(planner);
	auto plan = qb.match_("n", "Person").build();
	ASSERT_NE(plan, nullptr);
	// Should be a ScanOperator (or Filter over Scan depending on planner impl)
	// Assuming planner creates PhysicalOperator directly via createScanNode etc.
	// We just verify it's not null and has output.
	EXPECT_FALSE(plan->getOutputVariables().empty());
}

TEST_F(QueryBuilderTest, Match_Multiple_Cartesian) {
	QueryBuilder qb(planner);
	// MATCH (n), MATCH (m)
	auto plan = qb.match_("n").match_("m").build();
	ASSERT_NE(plan, nullptr);

	// Check if root is CartesianProduct
	auto cp = dynamic_cast<operators::CartesianProductOperator *>(plan.get());
	EXPECT_NE(cp, nullptr) << "Expected Cartesian Product for multiple matches";
}

TEST_F(QueryBuilderTest, Where_Success) {
	QueryBuilder qb(planner);
	auto plan = qb.match_("n").where_("n", "age", PropertyValue(30)).build();

	ASSERT_NE(plan, nullptr);
	EXPECT_NE(dynamic_cast<operators::FilterOperator *>(plan.get()), nullptr);
}

TEST_F(QueryBuilderTest, Where_Fail_NoMatch) {
	QueryBuilder qb(planner);
	// WHERE without MATCH should throw
	EXPECT_THROW({ qb.where_("n", "age", PropertyValue(10)); }, std::runtime_error);
}

// ============================================================================
// WRITE Operations (Data)
// ============================================================================

TEST_F(QueryBuilderTest, Create_Node) {
	QueryBuilder qb(planner);
	auto plan = qb.create_("n", "Person", {{"name", PropertyValue("Alice")}}).build();

	ASSERT_NE(plan, nullptr);
	EXPECT_NE(dynamic_cast<operators::CreateNodeOperator *>(plan.get()), nullptr);
}

TEST_F(QueryBuilderTest, Create_Node_Chained) {
	QueryBuilder qb(planner);
	// MATCH (n) CREATE (m)
	// This should chain CreateNode as a child of Match
	auto plan = qb.match_("n").create_("m", "User").build();

	ASSERT_NE(plan, nullptr);
	auto createOp = dynamic_cast<operators::CreateNodeOperator *>(plan.get());
	ASSERT_NE(createOp, nullptr);
	EXPECT_NE(createOp->getChildren()[0], nullptr); // Should have a child (the match)
}

TEST_F(QueryBuilderTest, Create_Edge) {
	QueryBuilder qb(planner);
	// CREATE (e:KNOWS {source:n, target:m})
	auto plan = qb.match_("n").match_("m").create_("e", "KNOWS", "n", "m").build();

	ASSERT_NE(plan, nullptr);
	EXPECT_NE(dynamic_cast<operators::CreateEdgeOperator *>(plan.get()), nullptr);
}

TEST_F(QueryBuilderTest, Delete_Success) {
	QueryBuilder qb(planner);
	auto plan = qb.match_("n").delete_({"n"}).build();

	ASSERT_NE(plan, nullptr);
	EXPECT_NE(dynamic_cast<operators::DeleteOperator *>(plan.get()), nullptr);
}

TEST_F(QueryBuilderTest, Delete_Fail_NoMatch) {
	QueryBuilder qb(planner);
	EXPECT_THROW({ qb.delete_({"n"}); }, std::runtime_error);
}

TEST_F(QueryBuilderTest, Set_Property) {
	QueryBuilder qb(planner);
	auto plan = qb.match_("n").set_("n", "age", PropertyValue(20)).build();

	ASSERT_NE(plan, nullptr);
	EXPECT_NE(dynamic_cast<operators::SetOperator *>(plan.get()), nullptr);
}

TEST_F(QueryBuilderTest, Set_Label) {
	QueryBuilder qb(planner);
	auto plan = qb.match_("n").setLabel_("n", "Admin").build();

	ASSERT_NE(plan, nullptr);
	EXPECT_NE(dynamic_cast<operators::SetOperator *>(plan.get()), nullptr);
}

TEST_F(QueryBuilderTest, Set_Fail_NoMatch) {
	QueryBuilder qb(planner);
	EXPECT_THROW({ qb.set_("n", "k", PropertyValue(1)); }, std::runtime_error);

	EXPECT_THROW({ qb.setLabel_("n", "L"); }, std::runtime_error);
}

TEST_F(QueryBuilderTest, Remove_Property_And_Label) {
	QueryBuilder qb(planner);

	// Remove Property
	auto planProp = qb.match_("n").remove_("n", "age", false).build();
	EXPECT_NE(dynamic_cast<operators::RemoveOperator *>(planProp.get()), nullptr);

	// Remove Label
	QueryBuilder qb2(planner);
	auto planLabel = qb2.match_("n").remove_("n", "OldLabel", true).build();
	EXPECT_NE(dynamic_cast<operators::RemoveOperator *>(planLabel.get()), nullptr);
}

TEST_F(QueryBuilderTest, Remove_Fail_NoMatch) {
	QueryBuilder qb(planner);
	EXPECT_THROW({ qb.remove_("n", "k"); }, std::runtime_error);
}

// ============================================================================
// Index Operations
// ============================================================================

TEST_F(QueryBuilderTest, CreateIndex) {
	QueryBuilder qb(planner);
	auto plan = qb.createIndex_("Label", "prop", "idx_name").build();
	EXPECT_NE(dynamic_cast<operators::CreateIndexOperator *>(plan.get()), nullptr);
}

TEST_F(QueryBuilderTest, DropIndex_ByName) {
	QueryBuilder qb(planner);
	auto plan = qb.dropIndex_("idx_name").build();
	EXPECT_NE(dynamic_cast<operators::DropIndexOperator *>(plan.get()), nullptr);
}

TEST_F(QueryBuilderTest, DropIndex_ByDefinition) {
	QueryBuilder qb(planner);
	auto plan = qb.dropIndex_("Label", "prop").build();
	EXPECT_NE(dynamic_cast<operators::DropIndexOperator *>(plan.get()), nullptr);
}

TEST_F(QueryBuilderTest, ShowIndexes) {
	QueryBuilder qb(planner);
	auto plan = qb.showIndexes_().build();
	EXPECT_NE(dynamic_cast<operators::ShowIndexesOperator *>(plan.get()), nullptr);
}

// ============================================================================
// Procedures
// ============================================================================

TEST_F(QueryBuilderTest, CallProcedure) {
	QueryBuilder qb(planner);
	// "dbms.listConfig" returns a ListConfigOperator
	auto plan = qb.call_("dbms.listConfig").build();

	ASSERT_NE(plan, nullptr);
	// Verify it returned the specific implementation operator
	EXPECT_NE(dynamic_cast<operators::ListConfigOperator*>(plan.get()), nullptr);
}

// ============================================================================
// Finalizers (Return, Skip, Limit, Order, Unwind)
// ============================================================================

TEST_F(QueryBuilderTest, Return) {
	QueryBuilder qb(planner);
	auto plan = qb.match_("n").return_({"n"}).build();
	EXPECT_NE(dynamic_cast<operators::ProjectOperator *>(plan.get()), nullptr);

	// Return on empty builder should do nothing (safe no-op)
	QueryBuilder empty(planner);
	auto noPlan = empty.return_({"n"}).build();
	EXPECT_EQ(noPlan, nullptr);
}

TEST_F(QueryBuilderTest, SkipLimit) {
	QueryBuilder qb(planner);
	auto plan = qb.match_("n").skip_(10).limit_(5).build();

	auto limitOp = dynamic_cast<operators::LimitOperator *>(plan.get());
	ASSERT_NE(limitOp, nullptr);
	EXPECT_NE(limitOp->getChildren()[0], nullptr);
}

TEST_F(QueryBuilderTest, SkipLimit_OnEmpty) {
	QueryBuilder qb(planner);
	// Should be no-op if no root
	qb.skip_(10);
	qb.limit_(5);
	EXPECT_EQ(qb.build(), nullptr);
}

TEST_F(QueryBuilderTest, OrderBy) {
	QueryBuilder qb(planner);
	std::vector<QueryBuilder::SortOrder> sortItems = {{"n", "age", true}};

	auto plan = qb.match_("n").orderBy_(sortItems).build();
	EXPECT_NE(dynamic_cast<operators::SortOperator *>(plan.get()), nullptr);

	// On Empty
	QueryBuilder empty(planner);
	empty.orderBy_(sortItems);
	EXPECT_EQ(empty.build(), nullptr);
}

TEST_F(QueryBuilderTest, Unwind) {
	QueryBuilder qb(planner);
	// UNWIND [1, 2, 3] AS x
	auto plan = qb.unwind({PropertyValue(1), PropertyValue(2)}, "x").build();
	EXPECT_NE(dynamic_cast<operators::UnwindOperator *>(plan.get()), nullptr);
}
