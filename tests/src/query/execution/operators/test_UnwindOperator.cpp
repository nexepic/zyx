/**
 * @file test_UnwindOperator.cpp
 * @date 2026/03/21
 *
 * @copyright Copyright (c) 2026
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

#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "graph/query/execution/operators/UnwindOperator.hpp"
#include "graph/query/execution/Record.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/expressions/ListLiteralExpression.hpp"
#include "graph/query/expressions/ExpressionEvaluationHelper.hpp"

using namespace graph::query::execution;
using namespace graph::query::execution::operators;
using namespace graph::query::expressions;
using namespace graph;

// ============================================================================
// Mock child operator for pipeline tests
// ============================================================================

class MockChildOperator : public PhysicalOperator {
public:
	std::vector<RecordBatch> batches;
	size_t currentIndex = 0;
	bool opened = false;
	bool closed = false;

	explicit MockChildOperator(std::vector<RecordBatch> data = {})
		: batches(std::move(data)) {}

	void open() override { opened = true; }

	std::optional<RecordBatch> next() override {
		if (currentIndex >= batches.size())
			return std::nullopt;
		return batches[currentIndex++];
	}

	void close() override { closed = true; }

	[[nodiscard]] std::vector<std::string> getOutputVariables() const override {
		return {"n"};
	}

	[[nodiscard]] std::string toString() const override { return "MockChild"; }
};

// ============================================================================
// Source Mode Tests (no child operator) - CASE A
// ============================================================================

class UnwindSourceModeTest : public ::testing::Test {};

// UNWIND literal list [1, 2, 3] AS x - basic functionality
TEST_F(UnwindSourceModeTest, LiteralList_BasicUnwind) {
	std::vector<PropertyValue> list = {PropertyValue(int64_t(1)),
	                                   PropertyValue(int64_t(2)),
	                                   PropertyValue(int64_t(3))};

	auto op = std::make_unique<UnwindOperator>(nullptr, "x", list);
	op->open();

	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 3UL);

	for (size_t i = 0; i < 3; ++i) {
		auto val = (*batch)[i].getValue("x");
		ASSERT_TRUE(val.has_value());
		auto intPtr = std::get_if<int64_t>(&val->getVariant());
		ASSERT_NE(intPtr, nullptr);
		EXPECT_EQ(*intPtr, static_cast<int64_t>(i + 1));
	}

	// Second call should return nullopt (exhausted)
	auto batch2 = op->next();
	EXPECT_FALSE(batch2.has_value());

	op->close();
}

// UNWIND [] AS x - empty list returns nullopt immediately
TEST_F(UnwindSourceModeTest, LiteralList_EmptyList) {
	std::vector<PropertyValue> emptyList;
	auto op = std::make_unique<UnwindOperator>(nullptr, "x", emptyList);
	op->open();

	auto batch = op->next();
	EXPECT_FALSE(batch.has_value());

	op->close();
}

// UNWIND with single element
TEST_F(UnwindSourceModeTest, LiteralList_SingleElement) {
	std::vector<PropertyValue> list = {PropertyValue(std::string("only"))};
	auto op = std::make_unique<UnwindOperator>(nullptr, "x", list);
	op->open();

	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 1UL);

	auto val = (*batch)[0].getValue("x");
	ASSERT_TRUE(val.has_value());
	auto strPtr = std::get_if<std::string>(&val->getVariant());
	ASSERT_NE(strPtr, nullptr);
	EXPECT_EQ(*strPtr, "only");

	EXPECT_FALSE(op->next().has_value());
	op->close();
}

// UNWIND with mixed types: [1, "hello", true, 3.14]
TEST_F(UnwindSourceModeTest, LiteralList_MixedTypes) {
	std::vector<PropertyValue> list = {
		PropertyValue(int64_t(1)),
		PropertyValue(std::string("hello")),
		PropertyValue(true),
		PropertyValue(3.14)
	};

	auto op = std::make_unique<UnwindOperator>(nullptr, "x", list);
	op->open();

	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 4UL);

	// Check int
	auto v0 = (*batch)[0].getValue("x");
	ASSERT_TRUE(v0.has_value());
	EXPECT_NE(std::get_if<int64_t>(&v0->getVariant()), nullptr);

	// Check string
	auto v1 = (*batch)[1].getValue("x");
	ASSERT_TRUE(v1.has_value());
	EXPECT_NE(std::get_if<std::string>(&v1->getVariant()), nullptr);

	// Check bool
	auto v2 = (*batch)[2].getValue("x");
	ASSERT_TRUE(v2.has_value());
	EXPECT_NE(std::get_if<bool>(&v2->getVariant()), nullptr);

	// Check double
	auto v3 = (*batch)[3].getValue("x");
	ASSERT_TRUE(v3.has_value());
	EXPECT_NE(std::get_if<double>(&v3->getVariant()), nullptr);

	op->close();
}

// UNWIND with nested lists: [[1,2], [3,4]] AS x
TEST_F(UnwindSourceModeTest, LiteralList_NestedLists) {
	std::vector<PropertyValue> innerList1 = {PropertyValue(int64_t(1)), PropertyValue(int64_t(2))};
	std::vector<PropertyValue> innerList2 = {PropertyValue(int64_t(3)), PropertyValue(int64_t(4))};

	std::vector<PropertyValue> outerList = {
		PropertyValue(innerList1),
		PropertyValue(innerList2)
	};

	auto op = std::make_unique<UnwindOperator>(nullptr, "x", outerList);
	op->open();

	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 2UL);

	// Each element should be a list
	auto v0 = (*batch)[0].getValue("x");
	ASSERT_TRUE(v0.has_value());
	EXPECT_EQ(v0->getType(), PropertyType::LIST);
	EXPECT_EQ(v0->getList().size(), 2UL);

	auto v1 = (*batch)[1].getValue("x");
	ASSERT_TRUE(v1.has_value());
	EXPECT_EQ(v1->getType(), PropertyType::LIST);
	EXPECT_EQ(v1->getList().size(), 2UL);

	op->close();
}

// UNWIND with null values in the list
TEST_F(UnwindSourceModeTest, LiteralList_NullValues) {
	std::vector<PropertyValue> list = {
		PropertyValue(int64_t(1)),
		PropertyValue(),  // null
		PropertyValue(int64_t(3))
	};

	auto op = std::make_unique<UnwindOperator>(nullptr, "x", list);
	op->open();

	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 3UL);

	// Second element should be null
	auto v1 = (*batch)[1].getValue("x");
	ASSERT_TRUE(v1.has_value());
	EXPECT_TRUE(std::holds_alternative<std::monostate>(v1->getVariant()));

	op->close();
}

// UNWIND with expression that evaluates to a list (source mode)
TEST_F(UnwindSourceModeTest, ExpressionList_EvaluatesToList) {
	std::vector<PropertyValue> innerList = {
		PropertyValue(int64_t(10)),
		PropertyValue(int64_t(20))
	};
	PropertyValue listVal(innerList);

	auto listExpr = std::make_shared<ListLiteralExpression>(listVal);

	auto op = std::make_unique<UnwindOperator>(nullptr, "x", listExpr);
	op->open();

	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 2UL);

	auto v0 = (*batch)[0].getValue("x");
	ASSERT_TRUE(v0.has_value());
	auto intPtr = std::get_if<int64_t>(&v0->getVariant());
	ASSERT_NE(intPtr, nullptr);
	EXPECT_EQ(*intPtr, 10);

	auto v1 = (*batch)[1].getValue("x");
	ASSERT_TRUE(v1.has_value());
	intPtr = std::get_if<int64_t>(&v1->getVariant());
	ASSERT_NE(intPtr, nullptr);
	EXPECT_EQ(*intPtr, 20);

	// Exhausted
	EXPECT_FALSE(op->next().has_value());

	op->close();
}

// UNWIND with expression that evaluates to non-list (source mode)
// When expression evaluates to non-list and list is empty, currentList_ stays empty
TEST_F(UnwindSourceModeTest, ExpressionList_EvaluatesToNonList) {
	// A LiteralExpression evaluates to a scalar, not a list
	auto scalarExpr = std::make_shared<LiteralExpression>(int64_t(42));

	auto op = std::make_unique<UnwindOperator>(nullptr, "x", scalarExpr);
	op->open();

	// The expression evaluates to a non-list, so currentList_ remains empty
	// and listIndex_ >= currentList_.size() returns nullopt
	auto batch = op->next();
	EXPECT_FALSE(batch.has_value());

	op->close();
}

// UNWIND with expression-based empty list (source mode)
TEST_F(UnwindSourceModeTest, ExpressionList_EmptyList) {
	std::vector<PropertyValue> emptyList;
	PropertyValue emptyListVal(emptyList);
	auto listExpr = std::make_shared<ListLiteralExpression>(emptyListVal);

	auto op = std::make_unique<UnwindOperator>(nullptr, "x", listExpr);
	op->open();

	auto batch = op->next();
	EXPECT_FALSE(batch.has_value());

	op->close();
}

// Verify open() resets state properly for re-use
TEST_F(UnwindSourceModeTest, OpenResetsState) {
	std::vector<PropertyValue> list = {PropertyValue(int64_t(1)), PropertyValue(int64_t(2))};
	auto op = std::make_unique<UnwindOperator>(nullptr, "x", list);

	// First run
	op->open();
	auto batch1 = op->next();
	ASSERT_TRUE(batch1.has_value());
	EXPECT_EQ(batch1->size(), 2UL);
	EXPECT_FALSE(op->next().has_value());

	// Re-open and run again
	op->open();
	auto batch2 = op->next();
	ASSERT_TRUE(batch2.has_value());
	EXPECT_EQ(batch2->size(), 2UL);
	EXPECT_FALSE(op->next().has_value());

	op->close();
}

// ============================================================================
// Pipeline Mode Tests (with child operator) - CASE B
// ============================================================================

class UnwindPipelineModeTest : public ::testing::Test {};

// MATCH (n) UNWIND [1,2] AS x - basic pipeline with literal list
TEST_F(UnwindPipelineModeTest, LiteralList_BasicPipeline) {
	Record r1;
	r1.setValue("n", std::string("Alice"));
	Record r2;
	r2.setValue("n", std::string("Bob"));

	auto mock = std::make_unique<MockChildOperator>(std::vector<RecordBatch>{{r1, r2}});

	std::vector<PropertyValue> list = {PropertyValue(int64_t(1)), PropertyValue(int64_t(2))};
	auto op = std::make_unique<UnwindOperator>(std::move(mock), "x", list);
	op->open();

	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	// 2 input records x 2 list items = 4 output records
	EXPECT_EQ(batch->size(), 4UL);

	// Check that original values are preserved
	auto n0 = (*batch)[0].getValue("n");
	ASSERT_TRUE(n0.has_value());
	EXPECT_EQ(*std::get_if<std::string>(&n0->getVariant()), "Alice");

	auto x0 = (*batch)[0].getValue("x");
	ASSERT_TRUE(x0.has_value());
	EXPECT_EQ(*std::get_if<int64_t>(&x0->getVariant()), 1);

	auto n2 = (*batch)[2].getValue("n");
	ASSERT_TRUE(n2.has_value());
	EXPECT_EQ(*std::get_if<std::string>(&n2->getVariant()), "Bob");

	auto x2 = (*batch)[2].getValue("x");
	ASSERT_TRUE(x2.has_value());
	EXPECT_EQ(*std::get_if<int64_t>(&x2->getVariant()), 1);

	// Should be exhausted after upstream is done
	auto batch2 = op->next();
	EXPECT_FALSE(batch2.has_value());

	op->close();
}

// Pipeline with empty child (no upstream records)
TEST_F(UnwindPipelineModeTest, LiteralList_EmptyChild) {
	auto mock = std::make_unique<MockChildOperator>(std::vector<RecordBatch>{});

	std::vector<PropertyValue> list = {PropertyValue(int64_t(1))};
	auto op = std::make_unique<UnwindOperator>(std::move(mock), "x", list);
	op->open();

	auto batch = op->next();
	EXPECT_FALSE(batch.has_value());

	op->close();
}

// Pipeline with expression-based list that evaluates to a list
TEST_F(UnwindPipelineModeTest, ExpressionList_EvaluatesToList) {
	Record r1;
	r1.setValue("n", std::string("Alice"));

	auto mock = std::make_unique<MockChildOperator>(std::vector<RecordBatch>{{r1}});

	std::vector<PropertyValue> innerList = {
		PropertyValue(int64_t(10)),
		PropertyValue(int64_t(20)),
		PropertyValue(int64_t(30))
	};
	PropertyValue listVal(innerList);
	auto listExpr = std::make_shared<ListLiteralExpression>(listVal);

	auto op = std::make_unique<UnwindOperator>(std::move(mock), "x", listExpr);
	op->open();

	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 3UL);

	// All should have the original "n" value
	for (size_t i = 0; i < 3; ++i) {
		auto n = (*batch)[i].getValue("n");
		ASSERT_TRUE(n.has_value());
		EXPECT_EQ(*std::get_if<std::string>(&n->getVariant()), "Alice");
	}

	// x values should be 10, 20, 30
	auto x0 = (*batch)[0].getValue("x");
	EXPECT_EQ(*std::get_if<int64_t>(&x0->getVariant()), 10);

	auto x2 = (*batch)[2].getValue("x");
	EXPECT_EQ(*std::get_if<int64_t>(&x2->getVariant()), 30);

	EXPECT_FALSE(op->next().has_value());
	op->close();
}

// Pipeline with expression that evaluates to non-list (single-element treatment)
TEST_F(UnwindPipelineModeTest, ExpressionList_EvaluatesToNonList) {
	Record r1;
	r1.setValue("n", std::string("Alice"));

	auto mock = std::make_unique<MockChildOperator>(std::vector<RecordBatch>{{r1}});

	// A scalar expression - should be treated as single-element list
	auto scalarExpr = std::make_shared<LiteralExpression>(int64_t(42));

	auto op = std::make_unique<UnwindOperator>(std::move(mock), "x", scalarExpr);
	op->open();

	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 1UL);

	// Should have both n and x
	auto n = (*batch)[0].getValue("n");
	ASSERT_TRUE(n.has_value());
	EXPECT_EQ(*std::get_if<std::string>(&n->getVariant()), "Alice");

	auto x = (*batch)[0].getValue("x");
	ASSERT_TRUE(x.has_value());
	// The scalar 42 is wrapped as a single-element list item
	auto intPtr = std::get_if<int64_t>(&x->getVariant());
	ASSERT_NE(intPtr, nullptr);
	EXPECT_EQ(*intPtr, 42);

	EXPECT_FALSE(op->next().has_value());
	op->close();
}

// Pipeline with multiple child batches
TEST_F(UnwindPipelineModeTest, LiteralList_MultipleBatches) {
	Record r1;
	r1.setValue("n", int64_t(1));
	Record r2;
	r2.setValue("n", int64_t(2));

	auto mock = std::make_unique<MockChildOperator>(std::vector<RecordBatch>{
		{r1},  // batch 1: single record
		{r2}   // batch 2: single record
	});

	std::vector<PropertyValue> list = {PropertyValue(std::string("a")), PropertyValue(std::string("b"))};
	auto op = std::make_unique<UnwindOperator>(std::move(mock), "x", list);
	op->open();

	// Collect all records from all batches
	size_t totalRecords = 0;
	while (auto batch = op->next()) {
		totalRecords += batch->size();
	}
	// r1 x [a, b] + r2 x [a, b] = 4 total records
	EXPECT_EQ(totalRecords, 4UL);

	op->close();
}

// Pipeline where upstream returns some records then exhausts
// Tests the branch: upstream exhausted but outputBatch is non-empty
TEST_F(UnwindPipelineModeTest, UpstreamExhausted_PartialBatch) {
	Record r1;
	r1.setValue("n", int64_t(1));

	auto mock = std::make_unique<MockChildOperator>(std::vector<RecordBatch>{{r1}});

	std::vector<PropertyValue> list = {PropertyValue(int64_t(100))};
	auto op = std::make_unique<UnwindOperator>(std::move(mock), "x", list);
	op->open();

	// First next(): processes r1 with list [100], then tries to get more from upstream
	// Upstream exhausted, but outputBatch has 1 record -> returns it
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 1UL);

	// Now truly exhausted
	auto batch2 = op->next();
	EXPECT_FALSE(batch2.has_value());

	op->close();
}

// Pipeline with expression, multiple input records in same batch
TEST_F(UnwindPipelineModeTest, ExpressionList_MultipleRecordsSameBatch) {
	Record r1;
	r1.setValue("n", std::string("Alice"));
	Record r2;
	r2.setValue("n", std::string("Bob"));

	auto mock = std::make_unique<MockChildOperator>(std::vector<RecordBatch>{{r1, r2}});

	std::vector<PropertyValue> innerList = {PropertyValue(int64_t(1)), PropertyValue(int64_t(2))};
	PropertyValue listVal(innerList);
	auto listExpr = std::make_shared<ListLiteralExpression>(listVal);

	auto op = std::make_unique<UnwindOperator>(std::move(mock), "x", listExpr);
	op->open();

	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	// 2 records x 2 list items = 4
	EXPECT_EQ(batch->size(), 4UL);

	// Verify needsEval_ triggers re-evaluation per input record
	// r1 expanded: x=1, x=2
	// r2 expanded: x=1, x=2
	auto x0 = (*batch)[0].getValue("x");
	EXPECT_EQ(*std::get_if<int64_t>(&x0->getVariant()), 1);
	auto x1 = (*batch)[1].getValue("x");
	EXPECT_EQ(*std::get_if<int64_t>(&x1->getVariant()), 2);
	auto x2 = (*batch)[2].getValue("x");
	EXPECT_EQ(*std::get_if<int64_t>(&x2->getVariant()), 1);
	auto x3 = (*batch)[3].getValue("x");
	EXPECT_EQ(*std::get_if<int64_t>(&x3->getVariant()), 2);

	EXPECT_FALSE(op->next().has_value());
	op->close();
}

// Pipeline with literal list, no expression, currentList_ initially empty branch
TEST_F(UnwindPipelineModeTest, LiteralList_CurrentListInitiallyEmpty) {
	Record r1;
	r1.setValue("n", int64_t(42));

	auto mock = std::make_unique<MockChildOperator>(std::vector<RecordBatch>{{r1}});

	// Use literal list constructor - listExpr_ is null, currentList_ starts empty
	std::vector<PropertyValue> list = {PropertyValue(int64_t(5)), PropertyValue(int64_t(6))};
	auto op = std::make_unique<UnwindOperator>(std::move(mock), "x", list);
	op->open();

	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 2UL);

	auto x0 = (*batch)[0].getValue("x");
	EXPECT_EQ(*std::get_if<int64_t>(&x0->getVariant()), 5);
	auto x1 = (*batch)[1].getValue("x");
	EXPECT_EQ(*std::get_if<int64_t>(&x1->getVariant()), 6);

	op->close();
}

// ============================================================================
// Metadata / Introspection Tests
// ============================================================================

class UnwindMetadataTest : public ::testing::Test {};

// toString() with literal list (no expression)
TEST_F(UnwindMetadataTest, ToString_LiteralList) {
	std::vector<PropertyValue> list = {PropertyValue(int64_t(1)), PropertyValue(int64_t(2))};
	auto op = std::make_unique<UnwindOperator>(nullptr, "x", list);

	std::string str = op->toString();
	EXPECT_EQ(str, "Unwind(x, size=2)");
}

// toString() with expression
TEST_F(UnwindMetadataTest, ToString_Expression) {
	auto listExpr = std::make_shared<LiteralExpression>(int64_t(1));
	auto op = std::make_unique<UnwindOperator>(nullptr, "x", listExpr);

	std::string str = op->toString();
	EXPECT_TRUE(str.find("Unwind(x, expr=") != std::string::npos);
}

// getOutputVariables() without child
TEST_F(UnwindMetadataTest, GetOutputVariables_NoChild) {
	std::vector<PropertyValue> list;
	auto op = std::make_unique<UnwindOperator>(nullptr, "items", list);

	auto vars = op->getOutputVariables();
	EXPECT_EQ(vars.size(), 1UL);
	EXPECT_EQ(vars[0], "items");
}

// getOutputVariables() with child
TEST_F(UnwindMetadataTest, GetOutputVariables_WithChild) {
	auto mock = std::make_unique<MockChildOperator>();
	std::vector<PropertyValue> list;
	auto op = std::make_unique<UnwindOperator>(std::move(mock), "x", list);

	auto vars = op->getOutputVariables();
	// MockChildOperator returns {"n"}, plus "x" from unwind
	EXPECT_EQ(vars.size(), 2UL);
	EXPECT_EQ(vars[0], "n");
	EXPECT_EQ(vars[1], "x");
}

// getChildren() without child
TEST_F(UnwindMetadataTest, GetChildren_NoChild) {
	std::vector<PropertyValue> list;
	auto op = std::make_unique<UnwindOperator>(nullptr, "x", list);

	EXPECT_TRUE(op->getChildren().empty());
}

// getChildren() with child
TEST_F(UnwindMetadataTest, GetChildren_WithChild) {
	auto mock = std::make_unique<MockChildOperator>();
	auto *rawMock = mock.get();
	std::vector<PropertyValue> list;
	auto op = std::make_unique<UnwindOperator>(std::move(mock), "x", list);

	auto children = op->getChildren();
	EXPECT_EQ(children.size(), 1UL);
	EXPECT_EQ(children[0], rawMock);
}

// open() and close() with child
TEST_F(UnwindMetadataTest, OpenClose_WithChild) {
	auto mock = std::make_unique<MockChildOperator>();
	auto *rawMock = mock.get();
	std::vector<PropertyValue> list;
	auto op = std::make_unique<UnwindOperator>(std::move(mock), "x", list);

	EXPECT_FALSE(rawMock->opened);
	EXPECT_FALSE(rawMock->closed);

	op->open();
	EXPECT_TRUE(rawMock->opened);

	op->close();
	EXPECT_TRUE(rawMock->closed);
}

// open() and close() without child (null child branches)
TEST_F(UnwindMetadataTest, OpenClose_NoChild) {
	std::vector<PropertyValue> list;
	auto op = std::make_unique<UnwindOperator>(nullptr, "x", list);

	// Should not crash
	EXPECT_NO_THROW(op->open());
	EXPECT_NO_THROW(op->close());
}
