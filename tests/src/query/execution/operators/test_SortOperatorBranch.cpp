/**
 * @file test_SortOperatorBranch.cpp
 * @date 2026
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
#include <vector>

#include "graph/query/execution/operators/SortOperator.hpp"
#include "graph/query/execution/Record.hpp"
#include "graph/query/expressions/Expression.hpp"

using namespace graph;
using namespace graph::query::execution;
using namespace graph::query::execution::operators;

// Mock Operator for testing
class SortMockOperator : public PhysicalOperator {
public:
	std::vector<RecordBatch> batches;
	size_t current_index = 0;

	explicit SortMockOperator(std::vector<RecordBatch> data = {}) : batches(std::move(data)) {}

	void open() override { current_index = 0; }
	std::optional<RecordBatch> next() override {
		if (current_index >= batches.size()) {
			return std::nullopt;
		}
		return batches[current_index++];
	}
	void close() override {}
	[[nodiscard]] std::vector<std::string> getOutputVariables() const override { return {"x"}; }
	[[nodiscard]] std::string toString() const override { return "SortMock"; }
};

class SortOperatorBranchTest : public ::testing::Test {
protected:
	void SetUp() override {}
	void TearDown() override {}
};

/**
 * Test SortOperator open/close with null child
 * Covers: if (child_) false branch in open() and close()
 */
TEST_F(SortOperatorBranchTest, OpenCloseWithNullChild) {
	std::vector<SortItem> sortItems;
	auto op = std::make_unique<SortOperator>(nullptr, sortItems);
	EXPECT_NO_THROW(op->open());
	EXPECT_NO_THROW(op->close());
}

/**
 * Test SortOperator toString with null expression in sort item
 * Covers: item.expression null branch in toString()
 */
TEST_F(SortOperatorBranchTest, ToStringWithNullExpression) {
	SortItem item;
	item.expression = nullptr;
	item.ascending = true;

	std::vector<SortItem> sortItems = {item};
	auto op = std::make_unique<SortOperator>(nullptr, sortItems);

	std::string str = op->toString();
	EXPECT_TRUE(str.find("Sort(") != std::string::npos);
	EXPECT_TRUE(str.find("ASC") != std::string::npos);
}

/**
 * Test SortOperator toString with DESC sort item
 * Covers: item.ascending false branch in toString()
 */
TEST_F(SortOperatorBranchTest, ToStringWithDescending) {
	auto expr = std::make_shared<graph::query::expressions::VariableReferenceExpression>("x");
	SortItem item(expr, false); // descending

	std::vector<SortItem> sortItems = {item};
	auto op = std::make_unique<SortOperator>(nullptr, sortItems);

	std::string str = op->toString();
	EXPECT_TRUE(str.find("DESC") != std::string::npos);
}

/**
 * Test SortOperator toString with multiple sort items (comma separator)
 * Covers: i < sortItems_.size() - 1 true branch (comma insertion)
 */
TEST_F(SortOperatorBranchTest, ToStringWithMultipleSortItems) {
	auto expr1 = std::make_shared<graph::query::expressions::VariableReferenceExpression>("x");
	auto expr2 = std::make_shared<graph::query::expressions::VariableReferenceExpression>("y");
	SortItem item1(expr1, true);
	SortItem item2(expr2, false);

	std::vector<SortItem> sortItems = {item1, item2};
	auto op = std::make_unique<SortOperator>(nullptr, sortItems);

	std::string str = op->toString();
	EXPECT_TRUE(str.find(", ") != std::string::npos);
	EXPECT_TRUE(str.find("ASC") != std::string::npos);
	EXPECT_TRUE(str.find("DESC") != std::string::npos);
}

/**
 * Test SortOperator sorting in descending order
 * Covers: item.ascending false branch in performSort() (valA > valB)
 */
TEST_F(SortOperatorBranchTest, SortDescending) {
	Record r1, r2, r3;
	r1.setValue("x", PropertyValue(static_cast<int64_t>(1)));
	r2.setValue("x", PropertyValue(static_cast<int64_t>(3)));
	r3.setValue("x", PropertyValue(static_cast<int64_t>(2)));

	auto mock = new SortMockOperator({{r1, r2, r3}});

	auto expr = std::make_shared<graph::query::expressions::VariableReferenceExpression>("x");
	SortItem item(expr, false); // descending
	std::vector<SortItem> sortItems = {item};

	auto op = std::make_unique<SortOperator>(
		std::unique_ptr<PhysicalOperator>(mock), sortItems);

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 3UL);

	// Verify descending order: 3, 2, 1
	auto val0 = (*batch)[0].getValue("x");
	auto val1 = (*batch)[1].getValue("x");
	auto val2 = (*batch)[2].getValue("x");
	ASSERT_TRUE(val0.has_value());
	ASSERT_TRUE(val1.has_value());
	ASSERT_TRUE(val2.has_value());
	EXPECT_EQ(std::get<int64_t>(val0->getVariant()), 3);
	EXPECT_EQ(std::get<int64_t>(val1->getVariant()), 2);
	EXPECT_EQ(std::get<int64_t>(val2->getVariant()), 1);

	op->close();
}

/**
 * Test SortOperator with empty input
 * Covers: exhausted input returns nullopt
 */
TEST_F(SortOperatorBranchTest, EmptyInput) {
	auto mock = new SortMockOperator({});

	auto expr = std::make_shared<graph::query::expressions::VariableReferenceExpression>("x");
	SortItem item(expr, true);
	std::vector<SortItem> sortItems = {item};

	auto op = std::make_unique<SortOperator>(
		std::unique_ptr<PhysicalOperator>(mock), sortItems);

	op->open();
	auto batch = op->next();
	EXPECT_FALSE(batch.has_value());

	op->close();
}

/**
 * Test SortOperator ascending sort (confirms ASC path)
 * Covers: item.ascending true branch in performSort() (valA < valB)
 */
TEST_F(SortOperatorBranchTest, SortAscending) {
	Record r1, r2, r3;
	r1.setValue("x", PropertyValue(static_cast<int64_t>(3)));
	r2.setValue("x", PropertyValue(static_cast<int64_t>(1)));
	r3.setValue("x", PropertyValue(static_cast<int64_t>(2)));

	auto mock = new SortMockOperator({{r1, r2, r3}});

	auto expr = std::make_shared<graph::query::expressions::VariableReferenceExpression>("x");
	SortItem item(expr, true); // ascending
	std::vector<SortItem> sortItems = {item};

	auto op = std::make_unique<SortOperator>(
		std::unique_ptr<PhysicalOperator>(mock), sortItems);

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 3UL);

	// Verify ascending order: 1, 2, 3
	auto val0 = (*batch)[0].getValue("x");
	auto val1 = (*batch)[1].getValue("x");
	auto val2 = (*batch)[2].getValue("x");
	ASSERT_TRUE(val0.has_value());
	ASSERT_TRUE(val1.has_value());
	ASSERT_TRUE(val2.has_value());
	EXPECT_EQ(std::get<int64_t>(val0->getVariant()), 1);
	EXPECT_EQ(std::get<int64_t>(val1->getVariant()), 2);
	EXPECT_EQ(std::get<int64_t>(val2->getVariant()), 3);

	op->close();
}

/**
 * Test SortOperator with null expression in sort item during sort
 * Covers: item.expression null branch in performSort() (both values stay default)
 */
TEST_F(SortOperatorBranchTest, SortWithNullExpression) {
	Record r1, r2;
	r1.setValue("x", PropertyValue(static_cast<int64_t>(1)));
	r2.setValue("x", PropertyValue(static_cast<int64_t>(2)));

	auto mock = new SortMockOperator({{r1, r2}});

	SortItem item;
	item.expression = nullptr;
	item.ascending = true;
	std::vector<SortItem> sortItems = {item};

	auto op = std::make_unique<SortOperator>(
		std::unique_ptr<PhysicalOperator>(mock), sortItems);

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 2UL);

	op->close();
}

/**
 * Test SortOperator with equal values (falls through to next sort key)
 * Covers: valA == valB branch (continue to next sort key) and return false (strictly equal)
 */
TEST_F(SortOperatorBranchTest, SortWithEqualValues) {
	Record r1, r2;
	r1.setValue("x", PropertyValue(static_cast<int64_t>(1)));
	r1.setValue("y", PropertyValue(static_cast<int64_t>(2)));
	r2.setValue("x", PropertyValue(static_cast<int64_t>(1)));
	r2.setValue("y", PropertyValue(static_cast<int64_t>(1)));

	auto mock = new SortMockOperator({{r1, r2}});

	auto exprX = std::make_shared<graph::query::expressions::VariableReferenceExpression>("x");
	auto exprY = std::make_shared<graph::query::expressions::VariableReferenceExpression>("y");
	SortItem item1(exprX, true);
	SortItem item2(exprY, true);
	std::vector<SortItem> sortItems = {item1, item2};

	auto op = std::make_unique<SortOperator>(
		std::unique_ptr<PhysicalOperator>(mock), sortItems);

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 2UL);

	// Both have x=1, so sorted by y: y=1 first, y=2 second
	auto val0 = (*batch)[0].getValue("y");
	auto val1 = (*batch)[1].getValue("y");
	ASSERT_TRUE(val0.has_value());
	ASSERT_TRUE(val1.has_value());
	EXPECT_EQ(std::get<int64_t>(val0->getVariant()), 1);
	EXPECT_EQ(std::get<int64_t>(val1->getVariant()), 2);

	op->close();
}
