/**
 * @file ExpressionEvaluatorTestFixture.hpp
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
#include "graph/query/expressions/ExpressionEvaluator.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/expressions/EvaluationContext.hpp"
#include "graph/query/expressions/FunctionRegistry.hpp"
#include "graph/query/expressions/IsNullExpression.hpp"
#include "graph/query/expressions/ListSliceExpression.hpp"
#include "graph/query/expressions/ListComprehensionExpression.hpp"
#include "graph/query/expressions/ListLiteralExpression.hpp"
#include "graph/query/expressions/ExistsExpression.hpp"
#include "graph/query/expressions/PatternComprehensionExpression.hpp"
#include "graph/query/expressions/QuantifierFunctionExpression.hpp"
#include "graph/query/expressions/ReduceExpression.hpp"
#include "graph/query/expressions/ParameterExpression.hpp"
#include "graph/query/expressions/ShortestPathExpression.hpp"
#include "graph/query/expressions/MapProjectionExpression.hpp"
#include "graph/query/execution/Record.hpp"
#include "graph/core/Node.hpp"
#include "graph/core/Edge.hpp"

using namespace graph;
using namespace graph::query::expressions;
using namespace graph::query::execution;

class ExpressionEvaluatorTest : public ::testing::Test {
protected:
	void SetUp() override {
		record_.setValue("x", PropertyValue(int64_t(42)));
		record_.setValue("y", PropertyValue(3.14));
		record_.setValue("name", PropertyValue(std::string("Alice")));
		record_.setValue("flag", PropertyValue(true));
		record_.setValue("nullVal", PropertyValue());

		Node node(1, 100);
		std::unordered_map<std::string, PropertyValue> props;
		props["age"] = PropertyValue(int64_t(30));
		props["city"] = PropertyValue(std::string("NYC"));
		node.setProperties(props);
		record_.setNode("n", node);

		Edge edge(2, 1, 2, 101);
		std::unordered_map<std::string, PropertyValue> edgeProps;
		edgeProps["weight"] = PropertyValue(1.5);
		edge.setProperties(edgeProps);
		record_.setEdge("r", edge);

		context_ = std::make_unique<EvaluationContext>(record_);
	}

	PropertyValue eval(const Expression* expr) {
		ExpressionEvaluator evaluator(*context_);
		return evaluator.evaluate(expr);
	}

	Record record_;
	std::unique_ptr<EvaluationContext> context_;
};
