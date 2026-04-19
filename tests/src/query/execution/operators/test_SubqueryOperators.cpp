/**
 * @file test_SubqueryOperators.cpp
 * @brief Unit tests for RecordInjectorOperator, ForeachOperator,
 *        CallSubqueryOperator, and LoadCsvOperator.
 */

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

#include "graph/query/execution/operators/RecordInjectorOperator.hpp"
#include "graph/query/execution/operators/ForeachOperator.hpp"
#include "graph/query/execution/operators/CallSubqueryOperator.hpp"
#include "graph/query/execution/operators/LoadCsvOperator.hpp"
#include "graph/query/execution/PhysicalOperator.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/expressions/ListLiteralExpression.hpp"

namespace fs = std::filesystem;
using namespace graph::query::execution;
using namespace graph::query::execution::operators;
using namespace graph::query::expressions;

// =============================================================================
// Helper: mock operator that emits configurable batches
// =============================================================================

class MockSourceOperator : public PhysicalOperator {
public:
	explicit MockSourceOperator(std::vector<RecordBatch> batches)
		: batches_(std::move(batches)) {}

	void open() override { index_ = 0; }

	std::optional<RecordBatch> next() override {
		if (index_ >= batches_.size()) return std::nullopt;
		return batches_[index_++];
	}

	void close() override {}

	[[nodiscard]] std::vector<std::string> getOutputVariables() const override { return vars_; }
	[[nodiscard]] std::string toString() const override { return "MockSource"; }
	[[nodiscard]] std::vector<const PhysicalOperator *> getChildren() const override { return {}; }

	void setOutputVars(std::vector<std::string> vars) { vars_ = std::move(vars); }

private:
	std::vector<RecordBatch> batches_;
	size_t index_ = 0;
	std::vector<std::string> vars_;
};

// =============================================================================
// RecordInjectorOperator
// =============================================================================

class RecordInjectorOperatorTest : public ::testing::Test {};

TEST_F(RecordInjectorOperatorTest, EmitsInjectedRecord) {
	RecordInjectorOperator injector;
	Record r;
	r.setValue("x", graph::PropertyValue(int64_t(42)));
	injector.setRecord(r);

	injector.open();
	auto batch = injector.next();
	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 1u);
	auto val = (*batch)[0].getValue("x");
	ASSERT_TRUE(val.has_value());
	EXPECT_EQ(std::get<int64_t>(val->getVariant()), 42);
}

TEST_F(RecordInjectorOperatorTest, EmitsOnlyOnce) {
	RecordInjectorOperator injector;
	injector.open();
	auto batch1 = injector.next();
	ASSERT_TRUE(batch1.has_value());
	auto batch2 = injector.next();
	EXPECT_FALSE(batch2.has_value());
}

TEST_F(RecordInjectorOperatorTest, ResetOnOpen) {
	RecordInjectorOperator injector;
	injector.open();
	injector.next(); // consume
	// Re-open should allow another emit
	injector.open();
	auto batch = injector.next();
	ASSERT_TRUE(batch.has_value());
}

TEST_F(RecordInjectorOperatorTest, CloseMarksEmitted) {
	RecordInjectorOperator injector;
	injector.open();
	injector.close();
	auto batch = injector.next();
	EXPECT_FALSE(batch.has_value());
}

TEST_F(RecordInjectorOperatorTest, OutputVariablesEmpty) {
	RecordInjectorOperator injector;
	EXPECT_TRUE(injector.getOutputVariables().empty());
}

TEST_F(RecordInjectorOperatorTest, ToStringReturnsName) {
	RecordInjectorOperator injector;
	EXPECT_EQ(injector.toString(), "RecordInjector");
}

TEST_F(RecordInjectorOperatorTest, GetChildrenEmpty) {
	RecordInjectorOperator injector;
	EXPECT_TRUE(injector.getChildren().empty());
}

// =============================================================================
// ForeachOperator
// =============================================================================

class ForeachOperatorTest : public ::testing::Test {};

TEST_F(ForeachOperatorTest, StandaloneWithListExpression) {
	// Create a list expression [1, 2, 3]
	std::vector<graph::PropertyValue> list;
	list.emplace_back(int64_t(1));
	list.emplace_back(int64_t(2));
	list.emplace_back(int64_t(3));
	auto listExpr = std::make_shared<ListLiteralExpression>(graph::PropertyValue(std::move(list)));

	// Body: just a RecordInjector (side effect simulation)
	auto injectorOwned = std::make_unique<RecordInjectorOperator>();
	auto *injector = injectorOwned.get();

	ForeachOperator fe(nullptr, "i", listExpr, std::move(injectorOwned), injector);
	fe.open();
	auto batch = fe.next();
	ASSERT_TRUE(batch.has_value());
	// Standalone produces 1 output record (the seed)
	EXPECT_EQ(batch->size(), 1u);

	// Second call returns nullopt (seeded already)
	auto batch2 = fe.next();
	EXPECT_FALSE(batch2.has_value());
}

TEST_F(ForeachOperatorTest, WithChildPipeline) {
	// Create input batch with 2 records
	RecordBatch inputBatch;
	Record r1, r2;
	inputBatch.push_back(std::move(r1));
	inputBatch.push_back(std::move(r2));

	auto source = std::make_unique<MockSourceOperator>(std::vector<RecordBatch>{inputBatch});

	std::vector<graph::PropertyValue> list;
	list.emplace_back(std::string("a"));
	auto listExpr = std::make_shared<ListLiteralExpression>(graph::PropertyValue(std::move(list)));
	auto injectorOwned = std::make_unique<RecordInjectorOperator>();
	auto *injector = injectorOwned.get();

	ForeachOperator fe(std::move(source), "x", listExpr, std::move(injectorOwned), injector);
	fe.open();
	auto batch = fe.next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 2u);

	// Child exhausted
	auto batch2 = fe.next();
	EXPECT_FALSE(batch2.has_value());
	fe.close();
}

TEST_F(ForeachOperatorTest, NonListExpressionSkipped) {
	// Expression evaluates to a non-list (string)
	auto strExpr = std::make_shared<LiteralExpression>(std::string("not a list"));

	auto injectorOwned = std::make_unique<RecordInjectorOperator>();
	auto *injector = injectorOwned.get();

	ForeachOperator fe(nullptr, "x", strExpr, std::move(injectorOwned), injector);
	fe.open();
	auto batch = fe.next();
	ASSERT_TRUE(batch.has_value());
	// Record passes through unchanged
	EXPECT_EQ(batch->size(), 1u);
}

TEST_F(ForeachOperatorTest, EmptyListNoIteration) {
	std::vector<graph::PropertyValue> emptyList;
	auto listExpr = std::make_shared<ListLiteralExpression>(graph::PropertyValue(std::move(emptyList)));

	auto injectorOwned = std::make_unique<RecordInjectorOperator>();
	auto *injector = injectorOwned.get();

	ForeachOperator fe(nullptr, "x", listExpr, std::move(injectorOwned), injector);
	fe.open();
	auto batch = fe.next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 1u);
}

TEST_F(ForeachOperatorTest, NullBodyOrInjector) {
	std::vector<graph::PropertyValue> list;
	list.emplace_back(int64_t(1));
	auto listExpr = std::make_shared<ListLiteralExpression>(graph::PropertyValue(std::move(list)));

	// body is null, injector is null
	ForeachOperator fe(nullptr, "x", listExpr, nullptr, nullptr);
	fe.open();
	auto batch = fe.next();
	ASSERT_TRUE(batch.has_value());
	// Iterates but body is null so no side effects
	EXPECT_EQ(batch->size(), 1u);
}

TEST_F(ForeachOperatorTest, OutputVariablesNoChild) {
	auto listExpr = std::make_shared<LiteralExpression>(std::string("x"));
	ForeachOperator fe(nullptr, "x", listExpr, nullptr, nullptr);
	EXPECT_TRUE(fe.getOutputVariables().empty());
}

TEST_F(ForeachOperatorTest, OutputVariablesWithChild) {
	auto source = std::make_unique<MockSourceOperator>(std::vector<RecordBatch>{});
	source->setOutputVars({"n"});
	auto listExpr = std::make_shared<LiteralExpression>(std::string("x"));
	ForeachOperator fe(std::move(source), "x", listExpr, nullptr, nullptr);
	auto vars = fe.getOutputVariables();
	ASSERT_EQ(vars.size(), 1u);
	EXPECT_EQ(vars[0], "n");
}

TEST_F(ForeachOperatorTest, ToStringReturnsName) {
	auto listExpr = std::make_shared<LiteralExpression>(std::string("x"));
	ForeachOperator fe(nullptr, "myVar", listExpr, nullptr, nullptr);
	EXPECT_EQ(fe.toString(), "ForeachOperator(myVar)");
}

TEST_F(ForeachOperatorTest, GetChildrenBothPresent) {
	auto source = std::make_unique<MockSourceOperator>(std::vector<RecordBatch>{});
	auto body = std::make_unique<RecordInjectorOperator>();
	auto listExpr = std::make_shared<LiteralExpression>(std::string("x"));
	ForeachOperator fe(std::move(source), "x", listExpr, std::move(body), nullptr);
	EXPECT_EQ(fe.getChildren().size(), 2u);
}

TEST_F(ForeachOperatorTest, GetChildrenNone) {
	auto listExpr = std::make_shared<LiteralExpression>(std::string("x"));
	ForeachOperator fe(nullptr, "x", listExpr, nullptr, nullptr);
	EXPECT_TRUE(fe.getChildren().empty());
}

// =============================================================================
// CallSubqueryOperator
// =============================================================================

class CallSubqueryOperatorTest : public ::testing::Test {};

TEST_F(CallSubqueryOperatorTest, StandaloneSubqueryEmitsResults) {
	// Create a subquery that returns one record
	RecordBatch subBatch;
	Record subRec;
	subRec.setValue("total", graph::PropertyValue(int64_t(100)));
	subBatch.push_back(std::move(subRec));
	auto subquery = std::make_unique<MockSourceOperator>(std::vector<RecordBatch>{subBatch});

	CallSubqueryOperator csq(nullptr, std::move(subquery), {}, {"total"}, nullptr);
	csq.open();
	auto batch = csq.next();
	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 1u);

	// Second call - seeded, should return nullopt
	auto batch2 = csq.next();
	EXPECT_FALSE(batch2.has_value());
	csq.close();
}

TEST_F(CallSubqueryOperatorTest, WithInputPipeline) {
	// Input: 2 records
	RecordBatch inputBatch;
	Record r1;
	r1.setValue("n", graph::PropertyValue(std::string("Alice")));
	inputBatch.push_back(std::move(r1));
	Record r2;
	r2.setValue("n", graph::PropertyValue(std::string("Bob")));
	inputBatch.push_back(std::move(r2));
	auto input = std::make_unique<MockSourceOperator>(std::vector<RecordBatch>{inputBatch});

	// Subquery: returns one record per invocation
	RecordBatch subBatch;
	Record subRec;
	subRec.setValue("count", graph::PropertyValue(int64_t(5)));
	subBatch.push_back(std::move(subRec));
	auto subquery = std::make_unique<MockSourceOperator>(std::vector<RecordBatch>{subBatch});

	auto injectorOwned = std::make_unique<RecordInjectorOperator>();
	auto *injector = injectorOwned.get();

	// Replace subquery with one that uses the injector
	CallSubqueryOperator csq(std::move(input), std::move(injectorOwned),
	                         {"n"}, {}, injector);
	csq.open();
	auto batch = csq.next();
	ASSERT_TRUE(batch.has_value());
	// Each input record should produce output (merged with subquery)
	EXPECT_GE(batch->size(), 1u);
	csq.close();
}

TEST_F(CallSubqueryOperatorTest, WriteOnlySubquery) {
	// Subquery produces no results (write-only), no returned vars
	auto subquery = std::make_unique<MockSourceOperator>(std::vector<RecordBatch>{});
	auto input = std::make_unique<MockSourceOperator>(std::vector<RecordBatch>{RecordBatch{Record{}}});

	CallSubqueryOperator csq(std::move(input), std::move(subquery), {}, {}, nullptr);
	csq.open();
	auto batch = csq.next();
	ASSERT_TRUE(batch.has_value());
	// Write-only: input passes through
	EXPECT_EQ(batch->size(), 1u);
	csq.close();
}

TEST_F(CallSubqueryOperatorTest, NullSubquery) {
	auto input = std::make_unique<MockSourceOperator>(std::vector<RecordBatch>{RecordBatch{Record{}}});
	CallSubqueryOperator csq(std::move(input), nullptr, {}, {"x"}, nullptr);
	csq.open();
	auto batch = csq.next();
	// Null subquery: no results
	EXPECT_FALSE(batch.has_value());
	csq.close();
}

TEST_F(CallSubqueryOperatorTest, OutputVariablesNoInput) {
	CallSubqueryOperator csq(nullptr, nullptr, {}, {"a", "b"}, nullptr);
	auto vars = csq.getOutputVariables();
	ASSERT_EQ(vars.size(), 2u);
	EXPECT_EQ(vars[0], "a");
	EXPECT_EQ(vars[1], "b");
}

TEST_F(CallSubqueryOperatorTest, OutputVariablesWithInput) {
	auto input = std::make_unique<MockSourceOperator>(std::vector<RecordBatch>{});
	input->setOutputVars({"n"});
	CallSubqueryOperator csq(std::move(input), nullptr, {}, {"total"}, nullptr);
	auto vars = csq.getOutputVariables();
	ASSERT_EQ(vars.size(), 2u);
	EXPECT_EQ(vars[0], "n");
	EXPECT_EQ(vars[1], "total");
}

TEST_F(CallSubqueryOperatorTest, ToStringBasic) {
	CallSubqueryOperator csq(nullptr, nullptr, {}, {}, nullptr);
	EXPECT_EQ(csq.toString(), "CallSubquery");
}

TEST_F(CallSubqueryOperatorTest, ToStringInTransactions) {
	CallSubqueryOperator csq(nullptr, nullptr, {}, {}, nullptr, true);
	EXPECT_EQ(csq.toString(), "CallSubquery(IN TRANSACTIONS)");
}

TEST_F(CallSubqueryOperatorTest, ToStringInTransactionsWithBatch) {
	CallSubqueryOperator csq(nullptr, nullptr, {}, {}, nullptr, true, 50);
	EXPECT_EQ(csq.toString(), "CallSubquery(IN TRANSACTIONS OF 50 ROWS)");
}

TEST_F(CallSubqueryOperatorTest, GetChildrenBoth) {
	auto input = std::make_unique<MockSourceOperator>(std::vector<RecordBatch>{});
	auto subquery = std::make_unique<MockSourceOperator>(std::vector<RecordBatch>{});
	CallSubqueryOperator csq(std::move(input), std::move(subquery), {}, {}, nullptr);
	EXPECT_EQ(csq.getChildren().size(), 2u);
}

TEST_F(CallSubqueryOperatorTest, GetChildrenNone) {
	CallSubqueryOperator csq(nullptr, nullptr, {}, {}, nullptr);
	EXPECT_TRUE(csq.getChildren().empty());
}

TEST_F(CallSubqueryOperatorTest, SubqueryWithImportedVarsAndInjector) {
	// Input record with a value
	RecordBatch inputBatch;
	Record inputRec;
	inputRec.setValue("n", graph::PropertyValue(std::string("Alice")));
	inputBatch.push_back(std::move(inputRec));
	auto input = std::make_unique<MockSourceOperator>(std::vector<RecordBatch>{inputBatch});

	// Subquery: RecordInjector as the subquery (emits injected record)
	auto injectorOwned = std::make_unique<RecordInjectorOperator>();
	auto *injector = injectorOwned.get();

	CallSubqueryOperator csq(std::move(input), std::move(injectorOwned),
	                         {"n"}, {}, injector);
	csq.open();
	auto batch = csq.next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_GE(batch->size(), 1u);
	csq.close();
}

// =============================================================================
// LoadCsvOperator
// =============================================================================

class LoadCsvOperatorTest : public ::testing::Test {
protected:
	std::vector<fs::path> tempFiles;

	std::string createTempFile(const std::string &content) {
		auto uuid = boost::uuids::random_generator()();
		auto path = fs::temp_directory_path() / ("test_loadcsv_" + boost::uuids::to_string(uuid) + ".csv");
		std::ofstream out(path);
		out << content;
		out.close();
		tempFiles.push_back(path);
		// Use std::filesystem to convert path to generic format (forward slashes)
		return path.generic_string();
	}

	void TearDown() override {
		std::error_code ec;
		for (auto &f : tempFiles) {
			if (fs::exists(f)) fs::remove(f, ec);
		}
	}
};

TEST_F(LoadCsvOperatorTest, WithHeadersReturnsMap) {
	auto csvPath = createTempFile("name,age\nAlice,30\nBob,25\n");
	auto urlExpr = std::make_shared<LiteralExpression>(std::string("file:///" + csvPath));

	LoadCsvOperator op(nullptr, urlExpr, "row", true, ",");
	op.open();
	auto batch = op.next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 2u);

	// First record should have map with name and age
	auto val = (*batch)[0].getValue("row");
	ASSERT_TRUE(val.has_value());
	EXPECT_EQ(val->getType(), graph::PropertyType::MAP);

	auto batch2 = op.next();
	EXPECT_FALSE(batch2.has_value());
	op.close();
}

TEST_F(LoadCsvOperatorTest, WithoutHeadersReturnsList) {
	auto csvPath = createTempFile("Alice,30\nBob,25\n");
	auto urlExpr = std::make_shared<LiteralExpression>(std::string("file:///" + csvPath));

	LoadCsvOperator op(nullptr, urlExpr, "row", false, ",");
	op.open();
	auto batch = op.next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 2u);

	auto val = (*batch)[0].getValue("row");
	ASSERT_TRUE(val.has_value());
	EXPECT_EQ(val->getType(), graph::PropertyType::LIST);
	op.close();
}

TEST_F(LoadCsvOperatorTest, EmptyFile) {
	auto csvPath = createTempFile("");
	auto urlExpr = std::make_shared<LiteralExpression>(std::string("file:///" + csvPath));

	LoadCsvOperator op(nullptr, urlExpr, "row", false, ",");
	op.open();
	auto batch = op.next();
	EXPECT_FALSE(batch.has_value());
	op.close();
}

TEST_F(LoadCsvOperatorTest, EmptyFileWithHeaders) {
	auto csvPath = createTempFile("");
	auto urlExpr = std::make_shared<LiteralExpression>(std::string("file:///" + csvPath));

	LoadCsvOperator op(nullptr, urlExpr, "row", true, ",");
	op.open();
	auto batch = op.next();
	EXPECT_FALSE(batch.has_value());
	op.close();
}

TEST_F(LoadCsvOperatorTest, NonStringUrlThrows) {
	auto urlExpr = std::make_shared<LiteralExpression>(int64_t(42));

	LoadCsvOperator op(nullptr, urlExpr, "row", false, ",");
	EXPECT_THROW(op.open(), std::runtime_error);
}

TEST_F(LoadCsvOperatorTest, CustomFieldTerminator) {
	auto csvPath = createTempFile("name\tage\nAlice\t30\n");
	auto urlExpr = std::make_shared<LiteralExpression>(std::string("file:///" + csvPath));

	LoadCsvOperator op(nullptr, urlExpr, "row", true, "\t");
	op.open();
	auto batch = op.next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 1u);
	op.close();
}

TEST_F(LoadCsvOperatorTest, OutputVariablesNoChild) {
	auto urlExpr = std::make_shared<LiteralExpression>(std::string("file:///test.csv"));
	LoadCsvOperator op(nullptr, urlExpr, "row", false, ",");
	auto vars = op.getOutputVariables();
	ASSERT_EQ(vars.size(), 1u);
	EXPECT_EQ(vars[0], "row");
}

TEST_F(LoadCsvOperatorTest, OutputVariablesWithChild) {
	auto source = std::make_unique<MockSourceOperator>(std::vector<RecordBatch>{});
	source->setOutputVars({"n"});
	auto urlExpr = std::make_shared<LiteralExpression>(std::string("file:///test.csv"));
	LoadCsvOperator op(std::move(source), urlExpr, "row", false, ",");
	auto vars = op.getOutputVariables();
	ASSERT_EQ(vars.size(), 2u);
	EXPECT_EQ(vars[0], "n");
	EXPECT_EQ(vars[1], "row");
}

TEST_F(LoadCsvOperatorTest, ToStringWithHeaders) {
	auto urlExpr = std::make_shared<LiteralExpression>(std::string("file:///test.csv"));
	LoadCsvOperator op(nullptr, urlExpr, "row", true, ",");
	EXPECT_EQ(op.toString(), "LoadCsv(row, WITH HEADERS)");
}

TEST_F(LoadCsvOperatorTest, ToStringWithoutHeaders) {
	auto urlExpr = std::make_shared<LiteralExpression>(std::string("file:///test.csv"));
	LoadCsvOperator op(nullptr, urlExpr, "row", false, ",");
	EXPECT_EQ(op.toString(), "LoadCsv(row)");
}

TEST_F(LoadCsvOperatorTest, GetChildrenWithChild) {
	auto source = std::make_unique<MockSourceOperator>(std::vector<RecordBatch>{});
	auto urlExpr = std::make_shared<LiteralExpression>(std::string("file:///test.csv"));
	LoadCsvOperator op(std::move(source), urlExpr, "row", false, ",");
	EXPECT_EQ(op.getChildren().size(), 1u);
}

TEST_F(LoadCsvOperatorTest, GetChildrenNoChild) {
	auto urlExpr = std::make_shared<LiteralExpression>(std::string("file:///test.csv"));
	LoadCsvOperator op(nullptr, urlExpr, "row", false, ",");
	EXPECT_TRUE(op.getChildren().empty());
}

TEST_F(LoadCsvOperatorTest, HeadersOnlyFile) {
	auto csvPath = createTempFile("name,age\n");
	auto urlExpr = std::make_shared<LiteralExpression>(std::string("file:///" + csvPath));

	LoadCsvOperator op(nullptr, urlExpr, "row", true, ",");
	op.open();
	auto batch = op.next();
	// Headers-only file: no data rows
	EXPECT_FALSE(batch.has_value());
	op.close();
}

TEST_F(LoadCsvOperatorTest, WithChildOperator) {
	auto csvPath = createTempFile("a,b\n1,2\n");
	auto urlExpr = std::make_shared<LiteralExpression>(std::string("file:///" + csvPath));

	auto source = std::make_unique<MockSourceOperator>(std::vector<RecordBatch>{});
	LoadCsvOperator op(std::move(source), urlExpr, "row", false, ",");
	op.open();
	auto batch = op.next();
	ASSERT_TRUE(batch.has_value());
	op.close();
}

TEST_F(LoadCsvOperatorTest, LargeCsvExceedsBatchSize) {
	// Create CSV with >1000 rows to trigger batch size limit
	std::string content;
	for (int i = 0; i < 1100; ++i) {
		content += std::to_string(i) + "," + std::to_string(i * 2) + "\n";
	}
	auto csvPath = createTempFile(content);
	auto urlExpr = std::make_shared<LiteralExpression>(std::string("file:///" + csvPath));

	LoadCsvOperator op(nullptr, urlExpr, "row", false, ",");
	op.open();

	auto batch1 = op.next();
	ASSERT_TRUE(batch1.has_value());
	EXPECT_EQ(batch1->size(), 1000u);

	auto batch2 = op.next();
	ASSERT_TRUE(batch2.has_value());
	EXPECT_EQ(batch2->size(), 100u);

	auto batch3 = op.next();
	EXPECT_FALSE(batch3.has_value());
	op.close();
}

TEST_F(LoadCsvOperatorTest, UrlWithoutFilePrefix) {
	auto csvPath = createTempFile("a,b\n1,2\n");
	// Direct path without file:/// prefix
	auto urlExpr = std::make_shared<LiteralExpression>(std::string(csvPath));

	LoadCsvOperator op(nullptr, urlExpr, "row", false, ",");
	op.open();
	auto batch = op.next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 2u);
	op.close();
}

TEST_F(LoadCsvOperatorTest, FieldsFewerThanHeaders) {
	// Header has 3 cols, data has 2 cols - tests i < fields.size() branch
	auto csvPath = createTempFile("name,age,city\nAlice,30\n");
	auto urlExpr = std::make_shared<LiteralExpression>(std::string("file:///" + csvPath));

	LoadCsvOperator op(nullptr, urlExpr, "row", true, ",");
	op.open();
	auto batch = op.next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 1u);
	// Map should only have 2 keys (name, age), not city
	auto val = (*batch)[0].getValue("row");
	ASSERT_TRUE(val.has_value());
	EXPECT_EQ(val->getType(), graph::PropertyType::MAP);
	const auto &map = val->getMap();
	EXPECT_EQ(map.size(), 2u);
	op.close();
}

TEST_F(CallSubqueryOperatorTest, ImportedVarMissingFromRecord) {
	// Input record without the imported var
	RecordBatch inputBatch;
	Record inputRec;
	// No "n" set in this record
	inputBatch.push_back(std::move(inputRec));
	auto input = std::make_unique<MockSourceOperator>(std::vector<RecordBatch>{inputBatch});

	auto injectorOwned = std::make_unique<RecordInjectorOperator>();
	auto *injector = injectorOwned.get();

	CallSubqueryOperator csq(std::move(input), std::move(injectorOwned),
	                         {"n"}, {}, injector);
	csq.open();
	auto batch = csq.next();
	// Should still work (getValue returns nullopt, var not injected)
	ASSERT_TRUE(batch.has_value());
	csq.close();
}

TEST_F(CallSubqueryOperatorTest, SubqueryWithReturnedVarsNoResults) {
	// Subquery returns no results but has returned vars declared
	auto subquery = std::make_unique<MockSourceOperator>(std::vector<RecordBatch>{});
	auto input = std::make_unique<MockSourceOperator>(std::vector<RecordBatch>{RecordBatch{Record{}}});

	CallSubqueryOperator csq(std::move(input), std::move(subquery), {}, {"count"}, nullptr);
	csq.open();
	auto batch = csq.next();
	// No results because subquery is empty and returnedVars is not empty
	EXPECT_FALSE(batch.has_value());
	csq.close();
}

TEST_F(CallSubqueryOperatorTest, BatchOverflowFromSubquery) {
	// Subquery returns >1000 records for a single input, testing batch size limit
	RecordBatch largeBatch;
	largeBatch.reserve(1100);
	for (int i = 0; i < 1100; ++i) {
		Record r;
		r.setValue("val", graph::PropertyValue(int64_t(i)));
		largeBatch.push_back(std::move(r));
	}
	auto subquery = std::make_unique<MockSourceOperator>(std::vector<RecordBatch>{largeBatch});

	auto input = std::make_unique<MockSourceOperator>(std::vector<RecordBatch>{RecordBatch{Record{}}});

	CallSubqueryOperator csq(std::move(input), std::move(subquery), {}, {"val"}, nullptr);
	csq.open();

	// First batch should be capped at DEFAULT_BATCH_SIZE (1000)
	auto batch1 = csq.next();
	ASSERT_TRUE(batch1.has_value());
	EXPECT_EQ(batch1->size(), 1000u);

	// Second batch should have the remaining 100
	auto batch2 = csq.next();
	ASSERT_TRUE(batch2.has_value());
	EXPECT_EQ(batch2->size(), 100u);

	// Third call exhausted
	auto batch3 = csq.next();
	EXPECT_FALSE(batch3.has_value());
	csq.close();
}
