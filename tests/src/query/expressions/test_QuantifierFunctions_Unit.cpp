/**
 * @file test_QuantifierFunctions_Unit.cpp
 * @date 2025/3/9
 */

#include <gtest/gtest.h>
#include "graph/query/expressions/FunctionRegistry.hpp"
#include "graph/query/expressions/EvaluationContext.hpp"
#include "graph/query/execution/Record.hpp"
#include "graph/core/PropertyTypes.hpp"

using namespace graph::query::expressions;
using graph::PropertyValue;
using graph::query::execution::Record;

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

// Test that functions are registered
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

// TODO: Add more tests for actual evaluation logic
// This requires custom argument parsing which will be implemented later

// Test function signatures
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

// Test that standard evaluation throws error
TEST_F(QuantifierFunctionsTest, AllFunction_StandardEvaluateThrows) {
    const ScalarFunction* func = registry->lookupScalarFunction("all");
    ASSERT_NE(func, nullptr);

    std::vector<PropertyValue> args;
    args.push_back(PropertyValue(static_cast<int64_t>(1)));

    EXPECT_THROW({
        func->evaluate(args, *context_);
    }, std::runtime_error);
}

TEST_F(QuantifierFunctionsTest, AnyFunction_StandardEvaluateThrows) {
    const ScalarFunction* func = registry->lookupScalarFunction("any");
    ASSERT_NE(func, nullptr);

    std::vector<PropertyValue> args;
    args.push_back(PropertyValue(static_cast<int64_t>(1)));

    EXPECT_THROW({
        func->evaluate(args, *context_);
    }, std::runtime_error);
}

TEST_F(QuantifierFunctionsTest, NoneFunction_StandardEvaluateThrows) {
    const ScalarFunction* func = registry->lookupScalarFunction("none");
    ASSERT_NE(func, nullptr);

    std::vector<PropertyValue> args;
    args.push_back(PropertyValue(static_cast<int64_t>(1)));

    EXPECT_THROW({
        func->evaluate(args, *context_);
    }, std::runtime_error);
}

TEST_F(QuantifierFunctionsTest, SingleFunction_StandardEvaluateThrows) {
    const ScalarFunction* func = registry->lookupScalarFunction("single");
    ASSERT_NE(func, nullptr);

    std::vector<PropertyValue> args;
    args.push_back(PropertyValue(static_cast<int64_t>(1)));

    EXPECT_THROW({
        func->evaluate(args, *context_);
    }, std::runtime_error);
}

// Test error message contains helpful information
TEST_F(QuantifierFunctionsTest, AllFunction_ErrorMessageHelpful) {
    const ScalarFunction* func = registry->lookupScalarFunction("all");
    ASSERT_NE(func, nullptr);

    std::vector<PropertyValue> args;
    args.push_back(PropertyValue(static_cast<int64_t>(1)));

    try {
        func->evaluate(args, *context_);
        FAIL() << "Expected std::runtime_error";
    } catch (const std::runtime_error& e) {
        std::string msg = e.what();
        EXPECT_TRUE(msg.find("all()") != std::string::npos);
        EXPECT_TRUE(msg.find("special Cypher syntax") != std::string::npos);
        EXPECT_TRUE(msg.find("variable IN list WHERE") != std::string::npos);
    }
}
