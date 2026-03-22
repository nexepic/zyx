/**
 * @file test_QuantifierFunctions.cpp
 * @date 2025/3/9
 *
 * @copyright Copyright (c) 2025
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
#include <vector>
#include "graph/query/expressions/FunctionRegistry.hpp"
#include "graph/query/expressions/EvaluationContext.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/expressions/ListLiteralExpression.hpp"
#include "graph/query/expressions/QuantifierFunctionExpression.hpp"
#include "graph/query/expressions/ExpressionEvaluator.hpp"
#include "graph/query/execution/Record.hpp"
#include "graph/core/PropertyTypes.hpp"

using namespace graph;
using namespace graph::query::expressions;
using namespace graph::query::execution;

// === Registration & Signature Tests ===

class QuantifierFunctionsTest : public ::testing::Test {
protected:
    void SetUp() override {
        registry = &FunctionRegistry::getInstance();

        // Create a basic record for context
        record_.setValue("x", PropertyValue(42));
        record_.setValue("name", PropertyValue(std::string("Alice")));
        context_ = std::make_unique<EvaluationContext>(record_);
    }

    FunctionRegistry* registry;
    Record record_;
    std::unique_ptr<EvaluationContext> context_;
};

TEST_F(QuantifierFunctionsTest, AllFunction_Registered) {
    const ScalarFunction* func = registry->lookupScalarFunction("all");
    ASSERT_NE(func, nullptr);
    auto sig = func->getSignature();
    EXPECT_EQ(sig.name, "all");
}

TEST_F(QuantifierFunctionsTest, AnyFunction_Registered) {
    const ScalarFunction* func = registry->lookupScalarFunction("any");
    ASSERT_NE(func, nullptr);
    auto sig = func->getSignature();
    EXPECT_EQ(sig.name, "any");
}

TEST_F(QuantifierFunctionsTest, NoneFunction_Registered) {
    const ScalarFunction* func = registry->lookupScalarFunction("none");
    ASSERT_NE(func, nullptr);
    auto sig = func->getSignature();
    EXPECT_EQ(sig.name, "none");
}

TEST_F(QuantifierFunctionsTest, SingleFunction_Registered) {
    const ScalarFunction* func = registry->lookupScalarFunction("single");
    ASSERT_NE(func, nullptr);
    auto sig = func->getSignature();
    EXPECT_EQ(sig.name, "single");
}

TEST_F(QuantifierFunctionsTest, AllFunction_Signature) {
    const ScalarFunction* func = registry->lookupScalarFunction("all");
    auto sig = func->getSignature();
    EXPECT_EQ(sig.name, "all");
    EXPECT_EQ(sig.minArgs, static_cast<size_t>(1));
    EXPECT_TRUE(sig.variadic);
}

TEST_F(QuantifierFunctionsTest, AnyFunction_Signature) {
    const ScalarFunction* func = registry->lookupScalarFunction("any");
    auto sig = func->getSignature();
    EXPECT_EQ(sig.name, "any");
    EXPECT_EQ(sig.minArgs, static_cast<size_t>(1));
    EXPECT_TRUE(sig.variadic);
}

TEST_F(QuantifierFunctionsTest, NoneFunction_Signature) {
    const ScalarFunction* func = registry->lookupScalarFunction("none");
    auto sig = func->getSignature();
    EXPECT_EQ(sig.name, "none");
    EXPECT_EQ(sig.minArgs, static_cast<size_t>(1));
    EXPECT_TRUE(sig.variadic);
}

TEST_F(QuantifierFunctionsTest, SingleFunction_Signature) {
    const ScalarFunction* func = registry->lookupScalarFunction("single");
    auto sig = func->getSignature();
    EXPECT_EQ(sig.name, "single");
    EXPECT_EQ(sig.minArgs, static_cast<size_t>(1));
    EXPECT_TRUE(sig.variadic);
}

TEST_F(QuantifierFunctionsTest, AllFunction_StandardEvaluateThrows) {
    const ScalarFunction* func = registry->lookupScalarFunction("all");
    ASSERT_NE(func, nullptr);

    std::vector<PropertyValue> args;
    args.push_back(PropertyValue(static_cast<int64_t>(1)));

    EXPECT_THROW({
        (void)func->evaluate(args, *context_);
    }, std::runtime_error);
}

TEST_F(QuantifierFunctionsTest, AnyFunction_StandardEvaluateThrows) {
    const ScalarFunction* func = registry->lookupScalarFunction("any");
    ASSERT_NE(func, nullptr);

    std::vector<PropertyValue> args;
    args.push_back(PropertyValue(static_cast<int64_t>(1)));

    EXPECT_THROW({
        (void)func->evaluate(args, *context_);
    }, std::runtime_error);
}

TEST_F(QuantifierFunctionsTest, NoneFunction_StandardEvaluateThrows) {
    const ScalarFunction* func = registry->lookupScalarFunction("none");
    ASSERT_NE(func, nullptr);

    std::vector<PropertyValue> args;
    args.push_back(PropertyValue(static_cast<int64_t>(1)));

    EXPECT_THROW({
        (void)func->evaluate(args, *context_);
    }, std::runtime_error);
}

TEST_F(QuantifierFunctionsTest, SingleFunction_StandardEvaluateThrows) {
    const ScalarFunction* func = registry->lookupScalarFunction("single");
    ASSERT_NE(func, nullptr);

    std::vector<PropertyValue> args;
    args.push_back(PropertyValue(static_cast<int64_t>(1)));

    EXPECT_THROW({
        (void)func->evaluate(args, *context_);
    }, std::runtime_error);
}

TEST_F(QuantifierFunctionsTest, AllFunction_ErrorMessageHelpful) {
    const ScalarFunction* func = registry->lookupScalarFunction("all");
    ASSERT_NE(func, nullptr);

    std::vector<PropertyValue> args;
    args.push_back(PropertyValue(static_cast<int64_t>(1)));

    try {
        (void)func->evaluate(args, *context_);
        FAIL() << "Expected std::runtime_error";
    } catch (const std::runtime_error& e) {
        std::string msg = e.what();
        EXPECT_TRUE(msg.find("all()") != std::string::npos);
        EXPECT_TRUE(msg.find("special Cypher syntax") != std::string::npos);
        EXPECT_TRUE(msg.find("variable IN list WHERE") != std::string::npos);
    }
}

// === Evaluation Tests ===

class QuantifierFunctionsEvaluationTest : public ::testing::Test {
protected:
	void SetUp() override {
		const auto& registry = FunctionRegistry::getInstance();
		ASSERT_NE(registry.lookupScalarFunction("all"), nullptr);
		ASSERT_NE(registry.lookupScalarFunction("any"), nullptr);
		ASSERT_NE(registry.lookupScalarFunction("none"), nullptr);
		ASSERT_NE(registry.lookupScalarFunction("single"), nullptr);

		record_ = Record();
		context_ = std::make_unique<EvaluationContext>(record_);
	}

	Record record_;
	std::unique_ptr<EvaluationContext> context_;
};

static std::unique_ptr<QuantifierFunctionExpression> createQuantifierExpr(
	const std::string& funcName,
	const std::string& varName,
	std::vector<PropertyValue> listValues,
	int64_t whereValue) {

	PropertyValue listValue(listValues);
	auto listExpr = std::make_unique<ListLiteralExpression>(listValue);

	auto varExpr = std::make_unique<VariableReferenceExpression>(varName);
	auto intExpr = std::make_unique<LiteralExpression>(whereValue);
	auto whereExpr = std::make_unique<BinaryOpExpression>(
		std::move(varExpr),
		BinaryOperatorType::BOP_GREATER,
		std::move(intExpr)
	);

	return std::make_unique<QuantifierFunctionExpression>(
		funcName, varName, std::move(listExpr), std::move(whereExpr)
	);
}

TEST_F(QuantifierFunctionsEvaluationTest, AllFunction_AllTrue) {
	auto quantExpr = createQuantifierExpr("all", "x",
		{PropertyValue(int64_t(1)), PropertyValue(int64_t(2)), PropertyValue(int64_t(3))},
		0);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(QuantifierFunctionsEvaluationTest, AllFunction_OneFalse) {
	auto quantExpr = createQuantifierExpr("all", "x",
		{PropertyValue(int64_t(1)), PropertyValue(int64_t(-1)), PropertyValue(int64_t(3))},
		0);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(QuantifierFunctionsEvaluationTest, AllFunction_EmptyList) {
	auto quantExpr = createQuantifierExpr("all", "x", std::vector<PropertyValue>{}, 0);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));  // Vacuous truth
}

TEST_F(QuantifierFunctionsEvaluationTest, AnyFunction_OneTrue) {
	auto quantExpr = createQuantifierExpr("any", "x",
		{PropertyValue(int64_t(-1)), PropertyValue(int64_t(2)), PropertyValue(int64_t(-3))},
		0);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(QuantifierFunctionsEvaluationTest, AnyFunction_AllFalse) {
	auto quantExpr = createQuantifierExpr("any", "x",
		{PropertyValue(int64_t(-1)), PropertyValue(int64_t(-2)), PropertyValue(int64_t(-3))},
		0);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(QuantifierFunctionsEvaluationTest, AnyFunction_EmptyList) {
	auto quantExpr = createQuantifierExpr("any", "x", std::vector<PropertyValue>{}, 0);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(QuantifierFunctionsEvaluationTest, NoneFunction_NoMatches) {
	auto quantExpr = createQuantifierExpr("none", "x",
		{PropertyValue(int64_t(-1)), PropertyValue(int64_t(-2)), PropertyValue(int64_t(-3))},
		0);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(QuantifierFunctionsEvaluationTest, NoneFunction_OneMatch) {
	auto quantExpr = createQuantifierExpr("none", "x",
		{PropertyValue(int64_t(-1)), PropertyValue(int64_t(2)), PropertyValue(int64_t(-3))},
		0);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(QuantifierFunctionsEvaluationTest, NoneFunction_EmptyList) {
	auto quantExpr = createQuantifierExpr("none", "x", std::vector<PropertyValue>{}, 0);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(QuantifierFunctionsEvaluationTest, SingleFunction_OneMatch) {
	auto quantExpr = createQuantifierExpr("single", "x",
		{PropertyValue(int64_t(-1)), PropertyValue(int64_t(2)), PropertyValue(int64_t(-3))},
		0);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(QuantifierFunctionsEvaluationTest, SingleFunction_ZeroMatches) {
	auto quantExpr = createQuantifierExpr("single", "x",
		{PropertyValue(int64_t(-1)), PropertyValue(int64_t(-2)), PropertyValue(int64_t(-3))},
		0);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(QuantifierFunctionsEvaluationTest, SingleFunction_TwoMatches) {
	auto quantExpr = createQuantifierExpr("single", "x",
		{PropertyValue(int64_t(1)), PropertyValue(int64_t(2)), PropertyValue(int64_t(-3))},
		0);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(QuantifierFunctionsEvaluationTest, SingleFunction_EmptyList) {
	auto quantExpr = createQuantifierExpr("single", "x", std::vector<PropertyValue>{}, 0);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(QuantifierFunctionsEvaluationTest, AllFunction_NumericWhereResult) {
	auto quantExpr = createQuantifierExpr("all", "x",
		{PropertyValue(int64_t(10)), PropertyValue(int64_t(20)), PropertyValue(int64_t(30))},
		15);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(QuantifierFunctionsEvaluationTest, AllFunction_StringComparison) {
	std::vector<PropertyValue> stringList = {
		PropertyValue(std::string("banana")),
		PropertyValue(std::string("banana")),
		PropertyValue(std::string("banana"))
	};
	PropertyValue listValue(stringList);
	auto listExpr = std::make_unique<ListLiteralExpression>(listValue);

	auto varExpr = std::make_unique<VariableReferenceExpression>("x");
	auto strExpr = std::make_unique<LiteralExpression>(std::string("banana"));
	auto whereExpr = std::make_unique<BinaryOpExpression>(
		std::move(varExpr),
		BinaryOperatorType::BOP_EQUAL,
		std::move(strExpr)
	);

	auto quantExpr = std::make_unique<QuantifierFunctionExpression>(
		"all", "x", std::move(listExpr), std::move(whereExpr)
	);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(QuantifierFunctionsEvaluationTest, AllFunction_WithNulls) {
	auto quantExpr = createQuantifierExpr("all", "x",
		{PropertyValue(int64_t(1)), PropertyValue(), PropertyValue(int64_t(3))},
		0);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(QuantifierFunctionsEvaluationTest, AnyFunction_AllNulls) {
	auto quantExpr = createQuantifierExpr("any", "x",
		{PropertyValue(), PropertyValue(), PropertyValue()},
		0);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(QuantifierFunctionsEvaluationTest, AnyFunction_BooleanWhere) {
	std::vector<PropertyValue> boolList = {
		PropertyValue(false),
		PropertyValue(true),
		PropertyValue(false)
	};
	PropertyValue listValue(boolList);
	auto listExpr = std::make_unique<ListLiteralExpression>(listValue);

	auto varExpr = std::make_unique<VariableReferenceExpression>("x");

	auto quantExpr = std::make_unique<QuantifierFunctionExpression>(
		"any", "x", std::move(listExpr), std::move(varExpr)
	);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(QuantifierFunctionsEvaluationTest, AllFunction_TypeCoercion) {
	std::vector<PropertyValue> intList = {
		PropertyValue(int64_t(1)),
		PropertyValue(int64_t(2)),
		PropertyValue(int64_t(3))
	};
	PropertyValue listValue(intList);
	auto listExpr = std::make_unique<ListLiteralExpression>(listValue);

	auto varExpr = std::make_unique<VariableReferenceExpression>("x");

	auto quantExpr = std::make_unique<QuantifierFunctionExpression>(
		"all", "x", std::move(listExpr), std::move(varExpr)
	);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(QuantifierFunctionsEvaluationTest, UnknownFunction_Throws) {
	std::vector<PropertyValue> list = {PropertyValue(int64_t(1))};
	PropertyValue listValue(list);
	auto listExpr = std::make_unique<ListLiteralExpression>(listValue);

	auto varExpr = std::make_unique<VariableReferenceExpression>("x");
	auto whereExpr = std::make_unique<BinaryOpExpression>(
		std::move(varExpr),
		BinaryOperatorType::BOP_GREATER,
		std::make_unique<LiteralExpression>(int64_t(0))
	);

	auto quantExpr = std::make_unique<QuantifierFunctionExpression>(
		"unknown_func", "x", std::move(listExpr), std::move(whereExpr)
	);

	ExpressionEvaluator evaluator(*context_);
	EXPECT_THROW((void)evaluator.visit(quantExpr.get()), ExpressionEvaluationException);
}

TEST_F(QuantifierFunctionsEvaluationTest, AllFunction_ShortCircuit) {
	std::vector<PropertyValue> boolList = {
		PropertyValue(true),
		PropertyValue(false),
		PropertyValue(true)
	};
	PropertyValue listValue(boolList);
	auto listExpr = std::make_unique<ListLiteralExpression>(listValue);

	auto varExpr = std::make_unique<VariableReferenceExpression>("x");

	auto quantExpr = std::make_unique<QuantifierFunctionExpression>(
		"all", "x", std::move(listExpr), std::move(varExpr)
	);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(QuantifierFunctionsEvaluationTest, SingleFunction_FindsExactlyOne) {
	std::vector<PropertyValue> boolList = {
		PropertyValue(false),
		PropertyValue(true),
		PropertyValue(true)
	};
	PropertyValue listValue(boolList);
	auto listExpr = std::make_unique<ListLiteralExpression>(listValue);

	auto varExpr = std::make_unique<VariableReferenceExpression>("x");

	auto quantExpr = std::make_unique<QuantifierFunctionExpression>(
		"single", "x", std::move(listExpr), std::move(varExpr)
	);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(QuantifierFunctionsEvaluationTest, AllFunction_NonListArgument_Throws) {
	auto nonListExpr = std::make_unique<LiteralExpression>(int64_t(42));
	auto varExpr = std::make_unique<VariableReferenceExpression>("x");

	auto quantExpr = std::make_unique<QuantifierFunctionExpression>(
		"all", "x", std::move(nonListExpr), std::move(varExpr)
	);

	ExpressionEvaluator evaluator(*context_);
	EXPECT_THROW((void)evaluator.visit(quantExpr.get()), ExpressionEvaluationException);
}

TEST_F(QuantifierFunctionsEvaluationTest, AllFunction_DoubleCoercion) {
	std::vector<PropertyValue> doubleList = {
		PropertyValue(1.5),
		PropertyValue(0.0),
		PropertyValue(2.5)
	};
	PropertyValue listValue(doubleList);
	auto listExpr = std::make_unique<ListLiteralExpression>(listValue);

	auto varExpr = std::make_unique<VariableReferenceExpression>("x");

	auto quantExpr = std::make_unique<QuantifierFunctionExpression>(
		"all", "x", std::move(listExpr), std::move(varExpr)
	);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(QuantifierFunctionsEvaluationTest, AnyFunction_DoubleCoercion) {
	std::vector<PropertyValue> doubleList = {
		PropertyValue(0.0),
		PropertyValue(0.0),
		PropertyValue(3.14)
	};
	PropertyValue listValue(doubleList);
	auto listExpr = std::make_unique<ListLiteralExpression>(listValue);

	auto varExpr = std::make_unique<VariableReferenceExpression>("x");

	auto quantExpr = std::make_unique<QuantifierFunctionExpression>(
		"any", "x", std::move(listExpr), std::move(varExpr)
	);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(QuantifierFunctionsEvaluationTest, AllFunction_StringTruthiness) {
	std::vector<PropertyValue> stringList = {
		PropertyValue(std::string("hello")),
		PropertyValue(std::string("world")),
		PropertyValue(std::string("!"))
	};
	PropertyValue listValue(stringList);
	auto listExpr = std::make_unique<ListLiteralExpression>(listValue);

	auto varExpr = std::make_unique<VariableReferenceExpression>("x");

	auto quantExpr = std::make_unique<QuantifierFunctionExpression>(
		"all", "x", std::move(listExpr), std::move(varExpr)
	);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(QuantifierFunctionsEvaluationTest, AllFunction_EmptyString_Falsy) {
	std::vector<PropertyValue> stringList = {
		PropertyValue(std::string("hello")),
		PropertyValue(std::string("")),
		PropertyValue(std::string("!"))
	};
	PropertyValue listValue(stringList);
	auto listExpr = std::make_unique<ListLiteralExpression>(listValue);

	auto varExpr = std::make_unique<VariableReferenceExpression>("x");

	auto quantExpr = std::make_unique<QuantifierFunctionExpression>(
		"all", "x", std::move(listExpr), std::move(varExpr)
	);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(QuantifierFunctionsEvaluationTest, AllFunction_ListTruthiness) {
	std::vector<PropertyValue> outerList = {
		PropertyValue(std::vector<PropertyValue>{PropertyValue(int64_t(1))}),
		PropertyValue(std::vector<PropertyValue>{PropertyValue(int64_t(2))}),
		PropertyValue(std::vector<PropertyValue>{PropertyValue(int64_t(3))})
	};
	PropertyValue listValue(outerList);
	auto listExpr = std::make_unique<ListLiteralExpression>(listValue);

	auto varExpr = std::make_unique<VariableReferenceExpression>("x");

	auto quantExpr = std::make_unique<QuantifierFunctionExpression>(
		"all", "x", std::move(listExpr), std::move(varExpr)
	);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(QuantifierFunctionsEvaluationTest, AllFunction_EmptyList_Falsy) {
	std::vector<PropertyValue> outerList = {
		PropertyValue(std::vector<PropertyValue>{PropertyValue(int64_t(1))}),
		PropertyValue(std::vector<PropertyValue>{}),
		PropertyValue(std::vector<PropertyValue>{PropertyValue(int64_t(3))})
	};
	PropertyValue listValue(outerList);
	auto listExpr = std::make_unique<ListLiteralExpression>(listValue);

	auto varExpr = std::make_unique<VariableReferenceExpression>("x");

	auto quantExpr = std::make_unique<QuantifierFunctionExpression>(
		"all", "x", std::move(listExpr), std::move(varExpr)
	);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(QuantifierFunctionsEvaluationTest, NoneFunction_DoubleCoercion) {
	std::vector<PropertyValue> doubleList = {
		PropertyValue(0.0),
		PropertyValue(0.0),
		PropertyValue(0.0)
	};
	PropertyValue listValue(doubleList);
	auto listExpr = std::make_unique<ListLiteralExpression>(listValue);

	auto varExpr = std::make_unique<VariableReferenceExpression>("x");

	auto quantExpr = std::make_unique<QuantifierFunctionExpression>(
		"none", "x", std::move(listExpr), std::move(varExpr)
	);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(QuantifierFunctionsEvaluationTest, NoneFunction_StringTruthiness) {
	std::vector<PropertyValue> stringList = {
		PropertyValue(std::string("")),
		PropertyValue(std::string("")),
		PropertyValue(std::string(""))
	};
	PropertyValue listValue(stringList);
	auto listExpr = std::make_unique<ListLiteralExpression>(listValue);

	auto varExpr = std::make_unique<VariableReferenceExpression>("x");

	auto quantExpr = std::make_unique<QuantifierFunctionExpression>(
		"none", "x", std::move(listExpr), std::move(varExpr)
	);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(QuantifierFunctionsEvaluationTest, SingleFunction_DoubleCoercion) {
	std::vector<PropertyValue> doubleList = {
		PropertyValue(0.0),
		PropertyValue(3.14),
		PropertyValue(0.0)
	};
	PropertyValue listValue(doubleList);
	auto listExpr = std::make_unique<ListLiteralExpression>(listValue);

	auto varExpr = std::make_unique<VariableReferenceExpression>("x");

	auto quantExpr = std::make_unique<QuantifierFunctionExpression>(
		"single", "x", std::move(listExpr), std::move(varExpr)
	);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(QuantifierFunctionsEvaluationTest, SingleFunction_StringTruthiness) {
	std::vector<PropertyValue> stringList = {
		PropertyValue(std::string("")),
		PropertyValue(std::string("hello")),
		PropertyValue(std::string(""))
	};
	PropertyValue listValue(stringList);
	auto listExpr = std::make_unique<ListLiteralExpression>(listValue);

	auto varExpr = std::make_unique<VariableReferenceExpression>("x");

	auto quantExpr = std::make_unique<QuantifierFunctionExpression>(
		"single", "x", std::move(listExpr), std::move(varExpr)
	);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(QuantifierFunctionsEvaluationTest, NoneFunction_NonListArgument_Throws) {
	auto nonListExpr = std::make_unique<LiteralExpression>(int64_t(42));
	auto varExpr = std::make_unique<VariableReferenceExpression>("x");

	auto quantExpr = std::make_unique<QuantifierFunctionExpression>(
		"none", "x", std::move(nonListExpr), std::move(varExpr)
	);

	ExpressionEvaluator evaluator(*context_);
	EXPECT_THROW((void)evaluator.visit(quantExpr.get()), ExpressionEvaluationException);
}

TEST_F(QuantifierFunctionsEvaluationTest, SingleFunction_NonListArgument_Throws) {
	auto nonListExpr = std::make_unique<LiteralExpression>(int64_t(42));
	auto varExpr = std::make_unique<VariableReferenceExpression>("x");

	auto quantExpr = std::make_unique<QuantifierFunctionExpression>(
		"single", "x", std::move(nonListExpr), std::move(varExpr)
	);

	ExpressionEvaluator evaluator(*context_);
	EXPECT_THROW((void)evaluator.visit(quantExpr.get()), ExpressionEvaluationException);
}

TEST_F(QuantifierFunctionsEvaluationTest, AnyFunction_NonListArgument_Throws) {
	auto nonListExpr = std::make_unique<LiteralExpression>(int64_t(42));
	auto varExpr = std::make_unique<VariableReferenceExpression>("x");

	auto quantExpr = std::make_unique<QuantifierFunctionExpression>(
		"any", "x", std::move(nonListExpr), std::move(varExpr)
	);

	ExpressionEvaluator evaluator(*context_);
	EXPECT_THROW((void)evaluator.visit(quantExpr.get()), ExpressionEvaluationException);
}

TEST_F(QuantifierFunctionsEvaluationTest, AnyFunction_StringCoercion) {
	std::vector<PropertyValue> stringList = {
		PropertyValue(std::string("")),
		PropertyValue(std::string("")),
		PropertyValue(std::string("hello"))
	};
	PropertyValue listValue(stringList);
	auto listExpr = std::make_unique<ListLiteralExpression>(listValue);

	auto varExpr = std::make_unique<VariableReferenceExpression>("x");

	auto quantExpr = std::make_unique<QuantifierFunctionExpression>(
		"any", "x", std::move(listExpr), std::move(varExpr)
	);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(QuantifierFunctionsEvaluationTest, AnyFunction_ListCoercion) {
	std::vector<PropertyValue> outerList = {
		PropertyValue(std::vector<PropertyValue>{}),
		PropertyValue(std::vector<PropertyValue>{}),
		PropertyValue(std::vector<PropertyValue>{PropertyValue(int64_t(1))})
	};
	PropertyValue listValue(outerList);
	auto listExpr = std::make_unique<ListLiteralExpression>(listValue);

	auto varExpr = std::make_unique<VariableReferenceExpression>("x");

	auto quantExpr = std::make_unique<QuantifierFunctionExpression>(
		"any", "x", std::move(listExpr), std::move(varExpr)
	);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(QuantifierFunctionsEvaluationTest, NoneFunction_StringCoercion) {
	std::vector<PropertyValue> stringList = {
		PropertyValue(std::string("")),
		PropertyValue(std::string(""))
	};
	PropertyValue listValue(stringList);
	auto listExpr = std::make_unique<ListLiteralExpression>(listValue);

	auto varExpr = std::make_unique<VariableReferenceExpression>("x");

	auto quantExpr = std::make_unique<QuantifierFunctionExpression>(
		"none", "x", std::move(listExpr), std::move(varExpr)
	);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(QuantifierFunctionsEvaluationTest, SingleFunction_ListCoercion) {
	std::vector<PropertyValue> outerList = {
		PropertyValue(std::vector<PropertyValue>{}),
		PropertyValue(std::vector<PropertyValue>{PropertyValue(int64_t(1))}),
		PropertyValue(std::vector<PropertyValue>{})
	};
	PropertyValue listValue(outerList);
	auto listExpr = std::make_unique<ListLiteralExpression>(listValue);

	auto varExpr = std::make_unique<VariableReferenceExpression>("x");

	auto quantExpr = std::make_unique<QuantifierFunctionExpression>(
		"single", "x", std::move(listExpr), std::move(varExpr)
	);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(QuantifierFunctionsEvaluationTest, NoneFunction_WithNulls) {
	auto quantExpr = createQuantifierExpr("none", "x",
		{PropertyValue(), PropertyValue(), PropertyValue()},
		0);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(QuantifierFunctionsEvaluationTest, SingleFunction_WithNulls) {
	auto quantExpr = createQuantifierExpr("single", "x",
		{PropertyValue(), PropertyValue(int64_t(1)), PropertyValue()},
		0);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(QuantifierFunctionsEvaluationTest, SingleFunction_MultipleMatches_ReturnsFalse) {
	auto quantExpr = createQuantifierExpr("single", "x",
		{PropertyValue(int64_t(1)), PropertyValue(int64_t(2)), PropertyValue(int64_t(3))},
		0);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));
}

// ============================================================================
// Branch Coverage Tests for NoneFunction.cpp
// ============================================================================

// Test NoneFunction with INTEGER truthiness (covers line 67-68 True branch)
// The where expression evaluates to an integer, exercising the INTEGER branch
// in NoneFunction::evaluatePredicate
TEST_F(QuantifierFunctionsEvaluationTest, NoneFunction_IntegerTruthiness_NonZero) {
	// Create a list of integers and use the variable directly as the where expression
	// The variable x will be set to each integer, and the where expression returns x itself
	// This exercises the INTEGER branch in evaluatePredicate
	std::vector<PropertyValue> intList = {
		PropertyValue(int64_t(1)),
		PropertyValue(int64_t(2)),
		PropertyValue(int64_t(3))
	};
	PropertyValue listValue(intList);
	auto listExpr = std::make_unique<ListLiteralExpression>(listValue);

	// Use variable reference as where expression - returns the integer itself
	auto varExpr = std::make_unique<VariableReferenceExpression>("x");

	auto quantExpr = std::make_unique<QuantifierFunctionExpression>(
		"none", "x", std::move(listExpr), std::move(varExpr)
	);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	// All integers are non-zero (truthy), so none() should return false
	EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(QuantifierFunctionsEvaluationTest, NoneFunction_IntegerTruthiness_AllZero) {
	// All zeros - none should return true since no element is truthy
	std::vector<PropertyValue> intList = {
		PropertyValue(int64_t(0)),
		PropertyValue(int64_t(0)),
		PropertyValue(int64_t(0))
	};
	PropertyValue listValue(intList);
	auto listExpr = std::make_unique<ListLiteralExpression>(listValue);

	auto varExpr = std::make_unique<VariableReferenceExpression>("x");

	auto quantExpr = std::make_unique<QuantifierFunctionExpression>(
		"none", "x", std::move(listExpr), std::move(varExpr)
	);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
}

// Test NoneFunction with LIST truthiness (covers line 74-76 True branch)
// The where expression evaluates to a list, exercising the LIST branch
// in NoneFunction::evaluatePredicate
TEST_F(QuantifierFunctionsEvaluationTest, NoneFunction_ListTruthiness_NonEmpty) {
	// Create a list of lists - the where expression returns the list element itself
	std::vector<PropertyValue> outerList = {
		PropertyValue(std::vector<PropertyValue>{PropertyValue(int64_t(1))}),
		PropertyValue(std::vector<PropertyValue>{PropertyValue(int64_t(2))})
	};
	PropertyValue listValue(outerList);
	auto listExpr = std::make_unique<ListLiteralExpression>(listValue);

	auto varExpr = std::make_unique<VariableReferenceExpression>("x");

	auto quantExpr = std::make_unique<QuantifierFunctionExpression>(
		"none", "x", std::move(listExpr), std::move(varExpr)
	);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	// Non-empty lists are truthy, so none() should return false
	EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(QuantifierFunctionsEvaluationTest, NoneFunction_ListTruthiness_AllEmpty) {
	// All empty lists - none should return true since empty lists are falsy
	std::vector<PropertyValue> outerList = {
		PropertyValue(std::vector<PropertyValue>{}),
		PropertyValue(std::vector<PropertyValue>{})
	};
	PropertyValue listValue(outerList);
	auto listExpr = std::make_unique<ListLiteralExpression>(listValue);

	auto varExpr = std::make_unique<VariableReferenceExpression>("x");

	auto quantExpr = std::make_unique<QuantifierFunctionExpression>(
		"none", "x", std::move(listExpr), std::move(varExpr)
	);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
}
