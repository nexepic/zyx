/**
 * @file test_Operators.cpp
 * @date 2026/02/02
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

#include "graph/query/execution/operators/FilterOperator.hpp"
#include "graph/query/execution/operators/ProjectOperator.hpp"
#include "graph/query/execution/Record.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/core/Node.hpp"
#include "graph/core/Edge.hpp"

using namespace graph::query::execution;
using namespace graph::query::execution::operators;
using namespace graph;

// Test helper: creates a ProjectItem from a string expression text (mirrors old parseExpression)
inline ProjectItem makeProjectItem(const std::string& exprText, const std::string& alias) {
	using namespace graph::query::expressions;

	// Quoted string literal
	if (exprText.size() >= 2 &&
	    ((exprText.front() == '"' && exprText.back() == '"') ||
	     (exprText.front() == '\'' && exprText.back() == '\''))) {
		return ProjectItem(std::make_shared<LiteralExpression>(exprText.substr(1, exprText.size() - 2)), alias);
	}

	// Property access (n.prop)
	size_t dotPos = exprText.find('.');
	if (dotPos != std::string::npos && dotPos > 0 && std::isalpha(exprText[0])) {
		return ProjectItem(std::make_shared<VariableReferenceExpression>(
			exprText.substr(0, dotPos), exprText.substr(dotPos + 1)), alias);
	}

	// NULL
	{
		std::string upper = exprText;
		for (auto& c : upper) c = static_cast<char>(std::toupper(c));
		if (upper == "NULL") return ProjectItem(std::make_shared<LiteralExpression>(), alias);
		if (upper == "TRUE") return ProjectItem(std::make_shared<LiteralExpression>(true), alias);
		if (upper == "FALSE") return ProjectItem(std::make_shared<LiteralExpression>(false), alias);
	}

	// Integer literal
	try {
		size_t pos;
		int64_t intVal = std::stoll(exprText, &pos);
		if (pos == exprText.length())
			return ProjectItem(std::make_shared<LiteralExpression>(intVal), alias);
	} catch (...) {}

	// Double literal
	try {
		size_t pos;
		double dblVal = std::stod(exprText, &pos);
		if (pos == exprText.length())
			return ProjectItem(std::make_shared<LiteralExpression>(dblVal), alias);
	} catch (...) {}

	// Default: variable reference
	return ProjectItem(std::make_shared<VariableReferenceExpression>(exprText), alias);
}

// Mock Operator for testing
class MockOperator : public PhysicalOperator {
public:
	std::vector<RecordBatch> batches;
	size_t current_index = 0;
	bool throw_on_next = false;

	explicit MockOperator(std::vector<RecordBatch> data = {}) : batches(std::move(data)) {}

	void open() override {}
	std::optional<RecordBatch> next() override {
		if (throw_on_next) {
			throw std::runtime_error("Test exception");
		}
		if (current_index >= batches.size()) {
			return std::nullopt;
		}
		return batches[current_index++];
	}
	void close() override {}
	[[nodiscard]] std::vector<std::string> getOutputVariables() const override { return {"x"}; }
	[[nodiscard]] std::string toString() const override { return "Mock"; }
};

// ============================================================================
// FilterOperator Tests
// ============================================================================

class FilterOperatorTest : public ::testing::Test {
protected:
	void SetUp() override {}
	void TearDown() override {}
};

TEST_F(FilterOperatorTest, Filter_WithNullChild) {
	// Test with null child operator - should handle gracefully
	auto op = std::make_unique<FilterOperator>(nullptr, [](const Record &) { return true; }, "true");
	EXPECT_NO_THROW(op->open());
	EXPECT_NO_THROW(op->close());
	EXPECT_EQ(op->getOutputVariables().size(), 0UL);
	EXPECT_EQ(op->getChildren().size(), 0UL);
}

TEST_F(FilterOperatorTest, Filter_AllRecordsMatch) {
	// Create test records
	Record r1, r2, r3;
	r1.setValue("x", 1);
	r2.setValue("x", 2);
	r3.setValue("x", 3);

	MockOperator *mock = new MockOperator({{r1, r2, r3}});  // Single batch with 3 records
	auto op = std::unique_ptr<PhysicalOperator>(
		new FilterOperator(std::unique_ptr<PhysicalOperator>(mock), [](const Record &) { return true; }, "true"));

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 3UL);

	// Second call should return nullopt (exhausted)
	auto batch2 = op->next();
	EXPECT_FALSE(batch2.has_value());

	op->close();
}

TEST_F(FilterOperatorTest, Filter_NoRecordsMatch) {
	Record r1, r2;
	r1.setValue("x", 1);
	r2.setValue("x", 2);

	MockOperator *mock = new MockOperator({{r1, r2}});  // Single batch with 2 records
	auto op = std::unique_ptr<PhysicalOperator>(new FilterOperator(
		std::unique_ptr<PhysicalOperator>(mock), [](const Record &) { return false; }, "false"));

	op->open();
	auto batch = op->next();
	EXPECT_FALSE(batch.has_value()); // All filtered out, returns nullopt

	op->close();
}

TEST_F(FilterOperatorTest, Filter_PartialMatch) {
	Record r1, r2, r3;
	r1.setValue("x", 1);
	r2.setValue("x", 2);
	r3.setValue("x", 3);

	MockOperator *mock = new MockOperator({{r1, r2, r3}});  // Single batch with 3 records
	auto op = std::unique_ptr<PhysicalOperator>(new FilterOperator(
		std::unique_ptr<PhysicalOperator>(mock),
		[](const Record &rec) {
			auto val = rec.getValue("x");
			return val.has_value() && std::get_if<int64_t>(&val->getVariant()) && *std::get_if<int64_t>(&val->getVariant()) > 1;
		},
		"x > 1"));

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 2UL); // Only r2 and r3 match

	op->close();
}

TEST_F(FilterOperatorTest, Filter_EmptyBatchFromChild) {
	MockOperator *mock = new MockOperator({{Record(), Record()}});
	mock->batches.clear(); // Empty batches

	auto op = std::unique_ptr<PhysicalOperator>(
		new FilterOperator(std::unique_ptr<PhysicalOperator>(mock), [](const Record &) { return true; }, "true"));

	op->open();
	auto batch = op->next();
	EXPECT_FALSE(batch.has_value());

	op->close();
}

TEST_F(FilterOperatorTest, Filter_MultipleBatches) {
	// First batch: all filtered out
	// Second batch: some match
	Record r1, r2, r3, r4;
	r1.setValue("x", 1);
	r2.setValue("x", 2);
	r3.setValue("x", 3);
	r4.setValue("x", 4);

	MockOperator *mock = new MockOperator();
	// First batch returns 2 records but both will be filtered
	mock->batches.push_back({r1, r2});
	// Second batch returns 2 records, both match
	mock->batches.push_back({r3, r4});

	auto op = std::unique_ptr<PhysicalOperator>(new FilterOperator(
		std::unique_ptr<PhysicalOperator>(mock),
		[](const Record &rec) {
			auto val = rec.getValue("x");
			if (!val) return false;
			if (auto intVal = std::get_if<int64_t>(&val->getVariant())) {
				return *intVal >= 3;
			}
			return false;
		},
		"x >= 3"));

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 2UL); // r3 and r4

	op->close();
}

TEST_F(FilterOperatorTest, Filter_ToStringAndGetChildren) {
	MockOperator *mock = new MockOperator();
	auto op = std::unique_ptr<PhysicalOperator>(
		new FilterOperator(std::unique_ptr<PhysicalOperator>(mock), [](const Record &) { return true; }, "x > 5"));

	EXPECT_EQ(op->toString(), "Filter(x > 5)");
	EXPECT_EQ(op->getChildren().size(), 1UL);
	EXPECT_EQ(op->getChildren()[0], mock);

	op->close();
}

// ============================================================================
// ProjectOperator Tests
// ============================================================================

class ProjectOperatorTest : public ::testing::Test {
protected:
	void SetUp() override {}
	void TearDown() override {}
};

TEST_F(ProjectOperatorTest, Project_WithNullChild) {
	// Test with null child operator
	std::vector<ProjectItem> items = {{makeProjectItem("1", "num")}};
	auto op = std::make_unique<ProjectOperator>(nullptr, items);
	EXPECT_NO_THROW(op->open());
	EXPECT_NO_THROW(op->close());

	auto vars = op->getOutputVariables();
	EXPECT_EQ(vars.size(), 1UL);
	EXPECT_EQ(vars[0], "num");
}

TEST_F(ProjectOperatorTest,Project_DirectNodeLookup) {
	Record r1;
	Node n1(1, 100);
	n1.setProperties({{"name", std::string("Alice")}});
	r1.setNode("n", n1);

	MockOperator *mock = new MockOperator({{r1}});
	std::vector<ProjectItem> items = {{makeProjectItem("n", "node")}};
	auto op = std::unique_ptr<PhysicalOperator>(
		new ProjectOperator(std::unique_ptr<PhysicalOperator>(mock), items));

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 1UL);

	auto projectedNode = (*batch)[0].getNode("node");
	ASSERT_TRUE(projectedNode.has_value());
	EXPECT_EQ(projectedNode->getId(), 1);

	op->close();
}

TEST_F(ProjectOperatorTest, Project_DirectEdgeLookup) {
	Record r1;
	Edge e1(1, 100, 101, 200);
	e1.setProperties({{"weight", 1.5}});
	r1.setEdge("e", e1);

	MockOperator *mock = new MockOperator({{r1}});
	std::vector<ProjectItem> items = {{makeProjectItem("e", "edge")}};
	auto op = std::unique_ptr<PhysicalOperator>(
		new ProjectOperator(std::unique_ptr<PhysicalOperator>(mock), items));

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 1UL);

	auto projectedEdge = (*batch)[0].getEdge("edge");
	ASSERT_TRUE(projectedEdge.has_value());
	EXPECT_EQ(projectedEdge->getId(), 1);

	op->close();
}

TEST_F(ProjectOperatorTest, Project_DirectValueLookup) {
	Record r1;
	r1.setValue("x", 42);
	r1.setValue("y", std::string("test"));

	MockOperator *mock = new MockOperator({{r1}});
	std::vector<ProjectItem> items = {{makeProjectItem("x", "a")}, {makeProjectItem("y", "b")}};
	auto op = std::unique_ptr<PhysicalOperator>(
		new ProjectOperator(std::unique_ptr<PhysicalOperator>(mock), items));

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 1UL);

	auto valA = (*batch)[0].getValue("a");
	ASSERT_TRUE(valA.has_value());
	auto intPtr = std::get_if<int64_t>(&valA->getVariant());
	ASSERT_NE(intPtr, nullptr);
	EXPECT_EQ(*intPtr, 42);

	auto valB = (*batch)[0].getValue("b");
	ASSERT_TRUE(valB.has_value());
	auto strPtr = std::get_if<std::string>(&valB->getVariant());
	ASSERT_NE(strPtr, nullptr);
	EXPECT_EQ(*strPtr, "test");

	op->close();
}

TEST_F(ProjectOperatorTest, Project_PropertyAccessOnNode) {
	Record r1;
	Node n1(1, 100);
	n1.setProperties({{"name", std::string("Bob")}, {"age", 30}});
	r1.setNode("n", n1);

	MockOperator *mock = new MockOperator({{r1}});
	std::vector<ProjectItem> items = {{makeProjectItem("n.name", "name")}, {makeProjectItem("n.age", "age")}};
	auto op = std::unique_ptr<PhysicalOperator>(
		new ProjectOperator(std::unique_ptr<PhysicalOperator>(mock), items));

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 1UL);

	auto nameVal = (*batch)[0].getValue("name");
	ASSERT_TRUE(nameVal.has_value());
	auto namePtr = std::get_if<std::string>(&nameVal->getVariant());
	ASSERT_NE(namePtr, nullptr);
	EXPECT_EQ(*namePtr, "Bob");

	auto ageVal = (*batch)[0].getValue("age");
	ASSERT_TRUE(ageVal.has_value());
	auto agePtr = std::get_if<int64_t>(&ageVal->getVariant());
	ASSERT_NE(agePtr, nullptr);
	EXPECT_EQ(*agePtr, 30);

	op->close();
}

TEST_F(ProjectOperatorTest, Project_PropertyAccessOnEdge) {
	Record r1;
	Edge e1(1, 100, 101, 200);
	e1.setProperties({{"weight", 2.5}});
	r1.setEdge("e", e1);

	MockOperator *mock = new MockOperator({{r1}});
	std::vector<ProjectItem> items = {{makeProjectItem("e.weight", "weight")}};
	auto op = std::unique_ptr<PhysicalOperator>(
		new ProjectOperator(std::unique_ptr<PhysicalOperator>(mock), items));

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 1UL);

	auto weightVal = (*batch)[0].getValue("weight");
	ASSERT_TRUE(weightVal.has_value());
	auto weightPtr = std::get_if<double>(&weightVal->getVariant());
	ASSERT_NE(weightPtr, nullptr);
	EXPECT_DOUBLE_EQ(*weightPtr, 2.5);

	op->close();
}

TEST_F(ProjectOperatorTest, Project_MissingPropertyReturnsNull) {
	Record r1;
	Node n1(1, 100);
	n1.setProperties({{"name", std::string("Charlie")}});
	// 'age' property is missing
	r1.setNode("n", n1);

	MockOperator *mock = new MockOperator({{r1}});
	std::vector<ProjectItem> items = {{makeProjectItem("n.age", "age")}};
	auto op = std::unique_ptr<PhysicalOperator>(
		new ProjectOperator(std::unique_ptr<PhysicalOperator>(mock), items));

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 1UL);

	auto ageVal = (*batch)[0].getValue("age");
	ASSERT_TRUE(ageVal.has_value());
	// Missing property should return null (monostate)
	EXPECT_TRUE(std::holds_alternative<std::monostate>(ageVal->getVariant()));

	op->close();
}

TEST_F(ProjectOperatorTest, Project_LiteralInteger) {
	MockOperator *mock = new MockOperator({{Record()}});
	std::vector<ProjectItem> items = {{makeProjectItem("42", "answer")}};
	auto op = std::unique_ptr<PhysicalOperator>(
		new ProjectOperator(std::unique_ptr<PhysicalOperator>(mock), items));

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 1UL);

	auto val = (*batch)[0].getValue("answer");
	ASSERT_TRUE(val.has_value());
	auto intPtr = std::get_if<int64_t>(&val->getVariant());
	ASSERT_NE(intPtr, nullptr);
	EXPECT_EQ(*intPtr, 42);

	op->close();
}

TEST_F(ProjectOperatorTest, Project_LiteralDouble) {
	MockOperator *mock = new MockOperator({{Record()}});
	std::vector<ProjectItem> items = {{makeProjectItem("3.14", "pi")}};
	auto op = std::unique_ptr<PhysicalOperator>(
		new ProjectOperator(std::unique_ptr<PhysicalOperator>(mock), items));

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 1UL);

	auto val = (*batch)[0].getValue("pi");
	ASSERT_TRUE(val.has_value());
	auto doublePtr = std::get_if<double>(&val->getVariant());
	ASSERT_NE(doublePtr, nullptr);
	EXPECT_DOUBLE_EQ(*doublePtr, 3.14);

	op->close();
}

TEST_F(ProjectOperatorTest, Project_LiteralBoolean) {
	MockOperator *mock = new MockOperator({{Record()}});
	std::vector<ProjectItem> items = {{makeProjectItem("true", "t")}, {makeProjectItem("false", "f")}};
	auto op = std::unique_ptr<PhysicalOperator>(
		new ProjectOperator(std::unique_ptr<PhysicalOperator>(mock), items));

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 1UL);

	auto tVal = (*batch)[0].getValue("t");
	ASSERT_TRUE(tVal.has_value());
	EXPECT_TRUE(std::get_if<bool>(&tVal->getVariant()) && *std::get_if<bool>(&tVal->getVariant()));

	auto fVal = (*batch)[0].getValue("f");
	ASSERT_TRUE(fVal.has_value());
	EXPECT_FALSE(std::get_if<bool>(&fVal->getVariant()) && *std::get_if<bool>(&fVal->getVariant()));

	op->close();
}

TEST_F(ProjectOperatorTest, Project_LiteralString) {
	MockOperator *mock = new MockOperator({{Record()}});
	std::vector<ProjectItem> items = {{makeProjectItem("'hello'", "greeting")}, makeProjectItem(R"("world")", "place")};
	auto op = std::unique_ptr<PhysicalOperator>(
		new ProjectOperator(std::unique_ptr<PhysicalOperator>(mock), items));

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 1UL);

	auto greeting = (*batch)[0].getValue("greeting");
	ASSERT_TRUE(greeting.has_value());
	auto greetingPtr = std::get_if<std::string>(&greeting->getVariant());
	ASSERT_NE(greetingPtr, nullptr);
	EXPECT_EQ(*greetingPtr, "hello");

	auto place = (*batch)[0].getValue("place");
	ASSERT_TRUE(place.has_value());
	auto placePtr = std::get_if<std::string>(&place->getVariant());
	ASSERT_NE(placePtr, nullptr);
	EXPECT_EQ(*placePtr, "world");

	op->close();
}

TEST_F(ProjectOperatorTest, Project_LiteralNull) {
	MockOperator *mock = new MockOperator({{Record()}});
	std::vector<ProjectItem> items = {{makeProjectItem("NULL", "null_val")}, {makeProjectItem("null", "null_val2")}};
	auto op = std::unique_ptr<PhysicalOperator>(
		new ProjectOperator(std::unique_ptr<PhysicalOperator>(mock), items));

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 1UL);

	auto nullVal = (*batch)[0].getValue("null_val");
	ASSERT_TRUE(nullVal.has_value());
	EXPECT_TRUE(std::holds_alternative<std::monostate>(nullVal->getVariant()));

	auto nullVal2 = (*batch)[0].getValue("null_val2");
	ASSERT_TRUE(nullVal2.has_value());
	EXPECT_TRUE(std::holds_alternative<std::monostate>(nullVal2->getVariant()));

	op->close();
}

TEST_F(ProjectOperatorTest, Project_UnknownVariableReturnsNull) {
	Record r1;
	r1.setValue("x", 1);

	MockOperator *mock = new MockOperator({{r1}});
	std::vector<ProjectItem> items = {{makeProjectItem("unknown_var", "result")}};
	auto op = std::unique_ptr<PhysicalOperator>(
		new ProjectOperator(std::unique_ptr<PhysicalOperator>(mock), items));

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 1UL);

	auto val = (*batch)[0].getValue("result");
	ASSERT_TRUE(val.has_value());
	EXPECT_TRUE(std::holds_alternative<std::monostate>(val->getVariant()));

	op->close();
}

TEST_F(ProjectOperatorTest, Project_ToStringAndGetChildren) {
	MockOperator *mock = new MockOperator();
	std::vector<ProjectItem> items = {{makeProjectItem("n.name", "name")}, {makeProjectItem("n.age", "age")}};
	auto op = std::unique_ptr<PhysicalOperator>(
		new ProjectOperator(std::unique_ptr<PhysicalOperator>(mock), items));

	std::string str = op->toString();
	EXPECT_TRUE(str.find("Project(") == 0);
	EXPECT_TRUE(str.find("n.name") != std::string::npos);
	EXPECT_TRUE(str.find("AS name") != std::string::npos);

	EXPECT_EQ(op->getChildren().size(), 1UL);
	EXPECT_EQ(op->getChildren()[0], mock);

	op->close();
}

TEST_F(ProjectOperatorTest, Project_NegativeInteger) {
	MockOperator *mock = new MockOperator({{Record()}});
	std::vector<ProjectItem> items = {{makeProjectItem("-42", "neg")}};
	auto op = std::unique_ptr<PhysicalOperator>(
		new ProjectOperator(std::unique_ptr<PhysicalOperator>(mock), items));

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());

	auto val = (*batch)[0].getValue("neg");
	ASSERT_TRUE(val.has_value());
	auto intPtr = std::get_if<int64_t>(&val->getVariant());
	ASSERT_NE(intPtr, nullptr);
	EXPECT_EQ(*intPtr, -42);

	op->close();
}

TEST_F(ProjectOperatorTest, Project_NegativeDouble) {
	MockOperator *mock = new MockOperator({{Record()}});
	std::vector<ProjectItem> items = {{makeProjectItem("-3.14", "neg_pi")}};
	auto op = std::unique_ptr<PhysicalOperator>(
		new ProjectOperator(std::unique_ptr<PhysicalOperator>(mock), items));

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());

	auto val = (*batch)[0].getValue("neg_pi");
	ASSERT_TRUE(val.has_value());
	auto doublePtr = std::get_if<double>(&val->getVariant());
	ASSERT_NE(doublePtr, nullptr);
	EXPECT_DOUBLE_EQ(*doublePtr, -3.14);

	op->close();
}

TEST_F(ProjectOperatorTest, Project_CaseSensitivity) {
	MockOperator *mock = new MockOperator({{Record()}});
	std::vector<ProjectItem> items = {{makeProjectItem("TRUE", "t1")}, {makeProjectItem("true", "t2")}, {makeProjectItem("FALSE", "f1")}, {makeProjectItem("false", "f2")}};
	auto op = std::unique_ptr<PhysicalOperator>(
		new ProjectOperator(std::unique_ptr<PhysicalOperator>(mock), items));

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());

	auto t1 = (*batch)[0].getValue("t1");
	ASSERT_TRUE(t1.has_value());
	EXPECT_TRUE(std::get_if<bool>(&t1->getVariant()) && *std::get_if<bool>(&t1->getVariant()));

	auto t2 = (*batch)[0].getValue("t2");
	ASSERT_TRUE(t2.has_value());
	EXPECT_TRUE(std::get_if<bool>(&t2->getVariant()) && *std::get_if<bool>(&t2->getVariant()));

	auto f1 = (*batch)[0].getValue("f1");
	ASSERT_TRUE(f1.has_value());
	EXPECT_FALSE(std::get_if<bool>(&f1->getVariant()) && *std::get_if<bool>(&f1->getVariant()));

	auto f2 = (*batch)[0].getValue("f2");
	ASSERT_TRUE(f2.has_value());
	EXPECT_FALSE(std::get_if<bool>(&f2->getVariant()) && *std::get_if<bool>(&f2->getVariant()));

	op->close();
}

TEST_F(ProjectOperatorTest, Project_Zero) {
	MockOperator *mock = new MockOperator({{Record()}});
	std::vector<ProjectItem> items = {{makeProjectItem("0", "zero")}};
	auto op = std::unique_ptr<PhysicalOperator>(
		new ProjectOperator(std::unique_ptr<PhysicalOperator>(mock), items));

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());

	auto val = (*batch)[0].getValue("zero");
	ASSERT_TRUE(val.has_value());
	auto intPtr = std::get_if<int64_t>(&val->getVariant());
	ASSERT_NE(intPtr, nullptr);
	EXPECT_EQ(*intPtr, 0);

	op->close();
}

TEST_F(ProjectOperatorTest, Project_EmptyString) {
	MockOperator *mock = new MockOperator({{Record()}});
	std::vector<ProjectItem> items = {{makeProjectItem("''", "empty1")}, makeProjectItem(R"("")", "empty2")};
	auto op = std::unique_ptr<PhysicalOperator>(
		new ProjectOperator(std::unique_ptr<PhysicalOperator>(mock), items));

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());

	auto empty1 = (*batch)[0].getValue("empty1");
	ASSERT_TRUE(empty1.has_value());
	auto empty1Ptr = std::get_if<std::string>(&empty1->getVariant());
	ASSERT_NE(empty1Ptr, nullptr);
	EXPECT_EQ(*empty1Ptr, "");

	auto empty2 = (*batch)[0].getValue("empty2");
	ASSERT_TRUE(empty2.has_value());
	auto empty2Ptr = std::get_if<std::string>(&empty2->getVariant());
	ASSERT_NE(empty2Ptr, nullptr);
	EXPECT_EQ(*empty2Ptr, "");

	op->close();
}
