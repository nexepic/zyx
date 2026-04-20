/**
 * @file ExpressionsTestFixture.hpp
 * @author ZYX Contributors
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

#pragma once

#include <gtest/gtest.h>
#include "graph/query/expressions/ExpressionEvaluationHelper.hpp"
#include "graph/query/expressions/EvaluationContext.hpp"
#include "graph/query/expressions/ExpressionEvaluator.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/expressions/FunctionRegistry.hpp"
#include "graph/query/execution/Record.hpp"
#include "graph/core/Entity.hpp"
#include "graph/query/api/ResultValue.hpp"
#include "graph/query/expressions/IsNullExpression.hpp"
#include "graph/query/expressions/ListComprehensionExpression.hpp"
#include "graph/query/expressions/ListLiteralExpression.hpp"
#include "graph/query/expressions/ListSliceExpression.hpp"
#include "graph/query/expressions/QuantifierFunctionExpression.hpp"
#include "graph/query/expressions/ExistsExpression.hpp"
#include "graph/query/expressions/PatternComprehensionExpression.hpp"
#include "graph/query/expressions/ReduceExpression.hpp"
#include "graph/query/expressions/ParameterExpression.hpp"
#include "graph/query/expressions/ShortestPathExpression.hpp"
#include "graph/query/expressions/MapProjectionExpression.hpp"

using namespace graph;
using namespace graph::query::expressions;
using namespace graph::query::execution;

/**
 * @brief Test visitor that implements ConstExpressionVisitor interface.
 * Used to test the accept(ConstExpressionVisitor&) methods on all expression types.
 */
class TestConstExpressionVisitor : public ConstExpressionVisitor {
public:
	void visit(const LiteralExpression* expr [[maybe_unused]]) override { visitedLiteral = true; }
	void visit(const VariableReferenceExpression* expr [[maybe_unused]]) override { visitedVarRef = true; }
	void visit(const BinaryOpExpression* expr [[maybe_unused]]) override { visitedBinary = true; }
	void visit(const UnaryOpExpression* expr [[maybe_unused]]) override { visitedUnary = true; }
	void visit(const FunctionCallExpression* expr [[maybe_unused]]) override { visitedFunction = true; }
	void visit(const CaseExpression* expr [[maybe_unused]]) override { visitedCase = true; }
	void visit(const InExpression* expr [[maybe_unused]]) override { visitedIn = true; }
	void visit(const ListSliceExpression* expr [[maybe_unused]]) override { visitedListSlice = true; }
	void visit(const ListComprehensionExpression* expr [[maybe_unused]]) override { visitedListComprehension = true; }
	void visit(const ListLiteralExpression* expr [[maybe_unused]]) override { visitedListLiteral = true; }
	void visit(const IsNullExpression* expr [[maybe_unused]]) override { visitedIsNull = true; }
	void visit(const QuantifierFunctionExpression* expr [[maybe_unused]]) override { visitedQuantifierFunction = true; }
	void visit(const ExistsExpression* expr [[maybe_unused]]) override { visitedExists = true; }
	void visit(const PatternComprehensionExpression* expr [[maybe_unused]]) override { visitedPatternComprehension = true; }
	void visit(const ReduceExpression* expr [[maybe_unused]]) override {}
	void visit(const ParameterExpression* expr [[maybe_unused]]) override { visitedParameter = true; }
	void visit(const ShortestPathExpression* expr [[maybe_unused]]) override { visitedShortestPath = true; }
	void visit(const MapProjectionExpression* expr [[maybe_unused]]) override { visitedMapProjection = true; }

	bool visitedLiteral = false;
	bool visitedVarRef = false;
	bool visitedBinary = false;
	bool visitedUnary = false;
	bool visitedFunction = false;
	bool visitedCase = false;
	bool visitedIn = false;
	bool visitedListSlice = false;
	bool visitedListComprehension = false;
	bool visitedListLiteral = false;
	bool visitedIsNull = false;
	bool visitedQuantifierFunction = false;
	bool visitedExists = false;
	bool visitedPatternComprehension = false;
	bool visitedParameter = false;
	bool visitedShortestPath = false;
	bool visitedMapProjection = false;
};

/**
 * @brief Test visitor that implements ExpressionVisitor interface (non-const).
 * Used to test the accept(ExpressionVisitor&) methods on all expression types.
 */
class TestExpressionVisitor : public ExpressionVisitor {
public:
	void visit(LiteralExpression* expr [[maybe_unused]]) override { visitedLiteral = true; }
	void visit(VariableReferenceExpression* expr [[maybe_unused]]) override { visitedVarRef = true; }
	void visit(BinaryOpExpression* expr [[maybe_unused]]) override { visitedBinary = true; }
	void visit(UnaryOpExpression* expr [[maybe_unused]]) override { visitedUnary = true; }
	void visit(FunctionCallExpression* expr [[maybe_unused]]) override { visitedFunction = true; }
	void visit(CaseExpression* expr [[maybe_unused]]) override { visitedCase = true; }
	void visit(InExpression* expr [[maybe_unused]]) override { visitedIn = true; }
	void visit(ListSliceExpression* expr [[maybe_unused]]) override { visitedListSlice = true; }
	void visit(ListComprehensionExpression* expr [[maybe_unused]]) override { visitedListComprehension = true; }
	void visit(ListLiteralExpression* expr [[maybe_unused]]) override { visitedListLiteral = true; }
	void visit(IsNullExpression* expr [[maybe_unused]]) override { visitedIsNull = true; }
	void visit(QuantifierFunctionExpression* expr [[maybe_unused]]) override { visitedQuantifierFunction = true; }
	void visit(ExistsExpression* expr [[maybe_unused]]) override { visitedExists = true; }
	void visit(PatternComprehensionExpression* expr [[maybe_unused]]) override { visitedPatternComprehension = true; }
	void visit(ReduceExpression* expr [[maybe_unused]]) override {}
	void visit(ParameterExpression* expr [[maybe_unused]]) override { visitedParameter = true; }
	void visit(ShortestPathExpression* expr [[maybe_unused]]) override { visitedShortestPath = true; }
	void visit(MapProjectionExpression* expr [[maybe_unused]]) override { visitedMapProjection = true; }

	bool visitedLiteral = false;
	bool visitedVarRef = false;
	bool visitedBinary = false;
	bool visitedUnary = false;
	bool visitedFunction = false;
	bool visitedCase = false;
	bool visitedIn = false;
	bool visitedListSlice = false;
	bool visitedListComprehension = false;
	bool visitedListLiteral = false;
	bool visitedIsNull = false;
	bool visitedQuantifierFunction = false;
	bool visitedExists = false;
	bool visitedPatternComprehension = false;
	bool visitedParameter = false;
	bool visitedShortestPath = false;
	bool visitedMapProjection = false;
};

/**
 * @brief Simple mock ExpressionVisitor that counts visit calls.
 * Used to exercise non-const accept methods.
 */
class MockExpressionVisitor : public ExpressionVisitor {
public:
	int visitCount = 0;
	void visit(LiteralExpression*) override { visitCount++; }
	void visit(VariableReferenceExpression*) override { visitCount++; }
	void visit(BinaryOpExpression*) override { visitCount++; }
	void visit(UnaryOpExpression*) override { visitCount++; }
	void visit(FunctionCallExpression*) override { visitCount++; }
	void visit(CaseExpression*) override { visitCount++; }
	void visit(InExpression*) override { visitCount++; }
	void visit(ListSliceExpression*) override { visitCount++; }
	void visit(ListComprehensionExpression*) override { visitCount++; }
	void visit(ListLiteralExpression*) override { visitCount++; }
	void visit(IsNullExpression*) override { visitCount++; }
	void visit(QuantifierFunctionExpression*) override { visitCount++; }
	void visit(ExistsExpression*) override { visitCount++; }
	void visit(PatternComprehensionExpression*) override { visitCount++; }
	void visit(ReduceExpression*) override { visitCount++; }
	void visit(ParameterExpression*) override { visitCount++; }
	void visit(ShortestPathExpression*) override { visitCount++; }
	void visit(MapProjectionExpression*) override { visitCount++; }
};

class ExpressionsTest : public ::testing::Test {
protected:
	void SetUp() override {
		// Create a test record with some variables
		record_.setValue("x", PropertyValue(42));
		record_.setValue("y", PropertyValue(3.14));
		record_.setValue("name", PropertyValue(std::string("Alice")));
		record_.setValue("flag", PropertyValue(true));
		record_.setValue("nullVal", PropertyValue()); // NULL

		// Create a node with properties
		node_ = Node(1, 100);
		std::unordered_map<std::string, PropertyValue> props;
		props["age"] = PropertyValue(30);
		props["city"] = PropertyValue(std::string("NYC"));
		node_.setProperties(props);
		record_.setNode("n", node_);

		// Create an edge with properties
		edge_ = Edge(2, 1, 2, 101);
		std::unordered_map<std::string, PropertyValue> edgeProps;
		edgeProps["weight"] = PropertyValue(1.5);
		edge_.setProperties(edgeProps);
		record_.setEdge("r", edge_);

		context_ = std::make_unique<EvaluationContext>(record_);
	}

	Record record_;
	Node node_;
	Edge edge_;
	std::unique_ptr<EvaluationContext> context_;
};
