/**
 * @file test_CypherToPlanVisitor_Branches.cpp
 * @brief Focused branch tests for CypherToPlanVisitor control paths.
 */

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>

#include "query/parser/cypher/CypherToPlanVisitor.hpp"
#include "graph/query/logical/LogicalOperator.hpp"
#include "graph/query/planner/QueryPlanner.hpp"
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/indexes/IndexManager.hpp"

namespace fs = std::filesystem;

class CypherToPlanVisitorBranchesTest : public ::testing::Test {
protected:
	fs::path testFilePath;
	std::shared_ptr<graph::storage::FileStorage> storage;
	std::shared_ptr<graph::storage::DataManager> dataManager;
	std::shared_ptr<graph::query::indexes::IndexManager> indexManager;
	std::shared_ptr<graph::query::QueryPlanner> planner;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_cypher_visitor_" + boost::uuids::to_string(uuid) + ".db");

		storage = std::make_shared<graph::storage::FileStorage>(
			testFilePath.string(), 4096, graph::storage::OpenMode::OPEN_CREATE_NEW_FILE);
		storage->open();
		dataManager = storage->getDataManager();
		indexManager = std::make_shared<graph::query::indexes::IndexManager>(storage);
		planner = std::make_shared<graph::query::QueryPlanner>(dataManager, indexManager);
	}

	void TearDown() override {
		if (storage) {
			storage->close();
		}
		if (fs::exists(testFilePath)) {
			fs::remove(testFilePath);
		}
	}
};

TEST_F(CypherToPlanVisitorBranchesTest, GetPlanAndLogicalPlanReturnNullWhenRootIsMissing) {
	graph::parser::cypher::CypherToPlanVisitor visitor(planner);

	auto logical = visitor.getLogicalPlan();
	EXPECT_EQ(logical, nullptr);

	auto physical = visitor.getPlan();
	EXPECT_EQ(physical, nullptr);
}

TEST_F(CypherToPlanVisitorBranchesTest, VisitTxnBeginBuildsTransactionControlPlan) {
	graph::parser::cypher::CypherToPlanVisitor visitor(planner);

	visitor.visitTxnBegin(nullptr);
	auto logical = visitor.getLogicalPlan();
	ASSERT_NE(logical, nullptr);
	EXPECT_EQ(logical->getType(), graph::query::logical::LogicalOpType::LOP_TRANSACTION_CONTROL);
	EXPECT_NE(logical->toString().find("BEGIN"), std::string::npos);
}

TEST_F(CypherToPlanVisitorBranchesTest, VisitTxnCommitBuildsTransactionControlPlan) {
	graph::parser::cypher::CypherToPlanVisitor visitor(planner);

	visitor.visitTxnCommit(nullptr);
	auto logical = visitor.getLogicalPlan();
	ASSERT_NE(logical, nullptr);
	EXPECT_EQ(logical->getType(), graph::query::logical::LogicalOpType::LOP_TRANSACTION_CONTROL);
	EXPECT_NE(logical->toString().find("COMMIT"), std::string::npos);
}

TEST_F(CypherToPlanVisitorBranchesTest, VisitTxnRollbackBuildsTransactionControlPlan) {
	graph::parser::cypher::CypherToPlanVisitor visitor(planner);

	visitor.visitTxnRollback(nullptr);
	auto logical = visitor.getLogicalPlan();
	ASSERT_NE(logical, nullptr);
	EXPECT_EQ(logical->getType(), graph::query::logical::LogicalOpType::LOP_TRANSACTION_CONTROL);
	EXPECT_NE(logical->toString().find("ROLLBACK"), std::string::npos);
}
