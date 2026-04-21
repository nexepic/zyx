/**
 * @file test_PhysicalPlanConverterBranches.cpp
 * @brief Focused branch tests for PhysicalPlanConverter.
 */

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "graph/query/execution/operators/AggregateOperator.hpp"
#include "graph/query/execution/operators/DeleteOperator.hpp"
#include "graph/query/execution/operators/RemoveOperator.hpp"
#include "graph/query/execution/operators/SetOperator.hpp"
#include "graph/query/execution/operators/TransactionControlOperator.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/logical/operators/LogicalAggregate.hpp"
#include "graph/query/logical/operators/LogicalDelete.hpp"
#include "graph/query/logical/operators/LogicalRemove.hpp"
#include "graph/query/logical/operators/LogicalSet.hpp"
#include "graph/query/logical/operators/LogicalSingleRow.hpp"
#include "graph/query/logical/operators/LogicalTransactionControl.hpp"
#include "graph/query/planner/PhysicalPlanConverter.hpp"
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/constraints/ConstraintManager.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/indexes/IndexManager.hpp"

namespace fs = std::filesystem;
using namespace graph::query;
using namespace graph::query::logical;
using namespace graph::query::expressions;
using namespace graph::query::execution::operators;

namespace {

class InvalidLogicalOperator final : public LogicalOperator {
public:
	[[nodiscard]] LogicalOpType getType() const override {
		return static_cast<LogicalOpType>(9999);
	}

	[[nodiscard]] std::vector<LogicalOperator *> getChildren() const override { return {}; }
	[[nodiscard]] std::vector<std::string> getOutputVariables() const override { return {}; }
	[[nodiscard]] std::string toString() const override { return "InvalidLogicalOperator"; }
	[[nodiscard]] std::unique_ptr<LogicalOperator> clone() const override {
		return std::make_unique<InvalidLogicalOperator>();
	}

	void setChild(size_t, std::unique_ptr<LogicalOperator>) override {
		throw std::logic_error("Leaf");
	}
	std::unique_ptr<LogicalOperator> detachChild(size_t) override {
		throw std::logic_error("Leaf");
	}
};

std::shared_ptr<Expression> varRef(const std::string &name) {
	return std::shared_ptr<Expression>(std::make_unique<VariableReferenceExpression>(name).release());
}

} // namespace

class PhysicalPlanConverterBranchesTest : public ::testing::Test {
protected:
	fs::path testFilePath;
	std::shared_ptr<graph::storage::FileStorage> storage;
	std::shared_ptr<graph::storage::DataManager> dataManager;
	std::shared_ptr<graph::query::indexes::IndexManager> indexManager;
	std::shared_ptr<graph::storage::constraints::ConstraintManager> constraintManager;
	std::unique_ptr<PhysicalPlanConverter> converter;

	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_phys_converter_" + to_string(uuid) + ".zyx");

		storage = std::make_shared<graph::storage::FileStorage>(
			testFilePath.string(), 4096, graph::storage::OpenMode::OPEN_CREATE_NEW_FILE);
		storage->open();
		dataManager = storage->getDataManager();

		indexManager = std::make_shared<graph::query::indexes::IndexManager>(storage);
		indexManager->initialize();

		constraintManager = std::make_shared<graph::storage::constraints::ConstraintManager>(storage, indexManager);
		constraintManager->initialize();

		converter = std::make_unique<PhysicalPlanConverter>(dataManager, indexManager, constraintManager);
	}

	void TearDown() override {
		if (storage) storage->close();
		std::error_code ec;
		if (fs::exists(testFilePath)) fs::remove(testFilePath, ec);
	}
};

TEST_F(PhysicalPlanConverterBranchesTest, ConvertRejectsNullLogicalOperator) {
	EXPECT_THROW((void)converter->convert(nullptr), std::invalid_argument);
}

TEST_F(PhysicalPlanConverterBranchesTest, ConvertRejectsUnsupportedLogicalType) {
	InvalidLogicalOperator invalid;
	EXPECT_THROW((void)converter->convert(&invalid), std::runtime_error);
}

TEST_F(PhysicalPlanConverterBranchesTest, ConvertTransactionControlCoversAllCommands) {
	LogicalTransactionControl begin(LogicalTxnCommand::LTXN_BEGIN);
	auto beginPhys = converter->convert(&begin);
	auto *beginOp = dynamic_cast<TransactionControlOperator *>(beginPhys.get());
	ASSERT_NE(beginOp, nullptr);
	EXPECT_EQ(beginOp->getCommand(), TransactionCommand::TXN_CTL_BEGIN);

	LogicalTransactionControl commit(LogicalTxnCommand::LTXN_COMMIT);
	auto commitPhys = converter->convert(&commit);
	auto *commitOp = dynamic_cast<TransactionControlOperator *>(commitPhys.get());
	ASSERT_NE(commitOp, nullptr);
	EXPECT_EQ(commitOp->getCommand(), TransactionCommand::TXN_CTL_COMMIT);

	LogicalTransactionControl rollback(LogicalTxnCommand::LTXN_ROLLBACK);
	auto rollbackPhys = converter->convert(&rollback);
	auto *rollbackOp = dynamic_cast<TransactionControlOperator *>(rollbackPhys.get());
	ASSERT_NE(rollbackOp, nullptr);
	EXPECT_EQ(rollbackOp->getCommand(), TransactionCommand::TXN_CTL_ROLLBACK);
}

TEST_F(PhysicalPlanConverterBranchesTest, ConvertTransactionControlRejectsUnknownCommand) {
	LogicalTransactionControl unknown(static_cast<LogicalTxnCommand>(777));
	EXPECT_THROW((void)converter->convert(&unknown), std::runtime_error);
}

TEST_F(PhysicalPlanConverterBranchesTest, ConvertAggregateCoversCollectAndAliasFallback) {
	auto child = std::make_unique<LogicalSingleRow>();
	std::vector<std::shared_ptr<Expression>> groupByExprs;
	groupByExprs.push_back(nullptr); // cover expr==nullptr alias fallback branch

	std::vector<LogicalAggItem> aggs;
	aggs.emplace_back("collect", varRef("n"), "items");

	LogicalAggregate agg(std::move(child), std::move(groupByExprs), std::move(aggs), {});
	auto phys = converter->convert(&agg);

	auto *aggOp = dynamic_cast<AggregateOperator *>(phys.get());
	ASSERT_NE(aggOp, nullptr);
}

TEST_F(PhysicalPlanConverterBranchesTest, ConvertAggregateRejectsUnknownFunctionName) {
	auto child = std::make_unique<LogicalSingleRow>();
	std::vector<std::shared_ptr<Expression>> groupByExprs;
	std::vector<LogicalAggItem> aggs;
	aggs.emplace_back("median", varRef("n"), "m");

	LogicalAggregate agg(std::move(child), std::move(groupByExprs), std::move(aggs), {});
	EXPECT_THROW((void)converter->convert(&agg), std::runtime_error);
}

TEST_F(PhysicalPlanConverterBranchesTest, ConvertSetCoversMapMergeAndDefaultAndNullChild) {
	std::vector<LogicalSetItem> items;
	items.push_back({logical::SetActionType::LSET_MAP_MERGE, "n", "", varRef("m")});
	items.push_back({static_cast<logical::SetActionType>(999), "n", "p", varRef("v")});

	LogicalSet lset(std::move(items), nullptr);
	auto phys = converter->convert(&lset);
	auto *setOp = dynamic_cast<SetOperator *>(phys.get());
	ASSERT_NE(setOp, nullptr);
}

TEST_F(PhysicalPlanConverterBranchesTest, ConvertDeleteHandlesNullChildPath) {
	LogicalDelete del({"n"}, false, nullptr);
	auto phys = converter->convert(&del);
	auto *delOp = dynamic_cast<DeleteOperator *>(phys.get());
	ASSERT_NE(delOp, nullptr);
}

TEST_F(PhysicalPlanConverterBranchesTest, ConvertRemoveCoversDefaultAndNullChild) {
	std::vector<LogicalRemoveItem> items;
	items.push_back({static_cast<LogicalRemoveActionType>(999), "n", "p"});

	LogicalRemove rem(std::move(items), nullptr);
	auto phys = converter->convert(&rem);
	auto *remOp = dynamic_cast<RemoveOperator *>(phys.get());
	ASSERT_NE(remOp, nullptr);
}
