/**
 * @file test_FunctionRegistry_ListFunctions.cpp
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

#include "query/expressions/FunctionRegistryTestFixture.hpp"

// ============================================================================
// List Function Tests
// ============================================================================

TEST_F(FunctionRegistryTest, CoalesceFunction_FirstNonNull) {
	const ScalarFunction* func = registry->lookupScalarFunction("coalesce");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue()); // NULL
	args.push_back(PropertyValue(int64_t(42)));
	args.push_back(PropertyValue(int64_t(99)));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 42);
}

TEST_F(FunctionRegistryTest, CoalesceFunction_SecondNonNull) {
	const ScalarFunction* func = registry->lookupScalarFunction("coalesce");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue()); // NULL
	args.push_back(PropertyValue()); // NULL
	args.push_back(PropertyValue(int64_t(99)));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 99);
}

TEST_F(FunctionRegistryTest, CoalesceFunction_AllNull) {
	const ScalarFunction* func = registry->lookupScalarFunction("coalesce");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue()); // NULL
	args.push_back(PropertyValue()); // NULL
	args.push_back(PropertyValue()); // NULL
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, CoalesceFunction_FirstNonNullImmediate) {
	const ScalarFunction* func = registry->lookupScalarFunction("coalesce");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("first")));
	args.push_back(PropertyValue(std::string("second")));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::STRING);
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "first");
}

TEST_F(FunctionRegistryTest, CoalesceFunction_EmptyArgs) {
	const ScalarFunction* func = registry->lookupScalarFunction("coalesce");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args; // Empty
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, SizeFunction_List) {
	const ScalarFunction* func = registry->lookupScalarFunction("size");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	std::vector<PropertyValue> list = {PropertyValue(1.0), PropertyValue(2.0), PropertyValue(3.0)};
	args.push_back(PropertyValue(list));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 3);
}

TEST_F(FunctionRegistryTest, SizeFunction_String) {
	const ScalarFunction* func = registry->lookupScalarFunction("size");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("hello")));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 5);
}

TEST_F(FunctionRegistryTest, SizeFunction_EmptyList) {
	const ScalarFunction* func = registry->lookupScalarFunction("size");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	std::vector<PropertyValue> list; // Empty
	args.push_back(PropertyValue(list));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 0);
}

TEST_F(FunctionRegistryTest, SizeFunction_EmptyString) {
	const ScalarFunction* func = registry->lookupScalarFunction("size");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("")));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 0);
}

TEST_F(FunctionRegistryTest, SizeFunction_Null) {
	const ScalarFunction* func = registry->lookupScalarFunction("size");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue()); // NULL
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, RangeFunction_Basic) {
	const ScalarFunction* func = registry->lookupScalarFunction("range");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(int64_t(0)));
	args.push_back(PropertyValue(int64_t(5)));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::LIST);
	const auto& list = std::get<std::vector<PropertyValue>>(result.getVariant());
	EXPECT_EQ(list.size(), 6UL);
	EXPECT_EQ(std::get<int64_t>(list[0].getVariant()), 0);
	EXPECT_EQ(std::get<int64_t>(list[1].getVariant()), 1);
	EXPECT_EQ(std::get<int64_t>(list[2].getVariant()), 2);
	EXPECT_EQ(std::get<int64_t>(list[3].getVariant()), 3);
	EXPECT_EQ(std::get<int64_t>(list[4].getVariant()), 4);
	EXPECT_EQ(std::get<int64_t>(list[5].getVariant()), 5);
}

TEST_F(FunctionRegistryTest, RangeFunction_WithPositiveStep) {
	const ScalarFunction* func = registry->lookupScalarFunction("range");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(int64_t(0)));
	args.push_back(PropertyValue(int64_t(10)));
	args.push_back(PropertyValue(int64_t(2)));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::LIST);
	const auto& list = std::get<std::vector<PropertyValue>>(result.getVariant());
	EXPECT_EQ(list.size(), 6UL);
	EXPECT_EQ(std::get<int64_t>(list[0].getVariant()), 0);
	EXPECT_EQ(std::get<int64_t>(list[1].getVariant()), 2);
	EXPECT_EQ(std::get<int64_t>(list[2].getVariant()), 4);
	EXPECT_EQ(std::get<int64_t>(list[3].getVariant()), 6);
	EXPECT_EQ(std::get<int64_t>(list[4].getVariant()), 8);
	EXPECT_EQ(std::get<int64_t>(list[5].getVariant()), 10);
}

TEST_F(FunctionRegistryTest, RangeFunction_WithNegativeStep) {
	const ScalarFunction* func = registry->lookupScalarFunction("range");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(int64_t(5)));
	args.push_back(PropertyValue(int64_t(0)));
	args.push_back(PropertyValue(int64_t(-1)));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::LIST);
	const auto& list = std::get<std::vector<PropertyValue>>(result.getVariant());
	EXPECT_EQ(list.size(), 6UL);
	EXPECT_EQ(std::get<int64_t>(list[0].getVariant()), 5);
	EXPECT_EQ(std::get<int64_t>(list[1].getVariant()), 4);
	EXPECT_EQ(std::get<int64_t>(list[2].getVariant()), 3);
	EXPECT_EQ(std::get<int64_t>(list[3].getVariant()), 2);
	EXPECT_EQ(std::get<int64_t>(list[4].getVariant()), 1);
	EXPECT_EQ(std::get<int64_t>(list[5].getVariant()), 0);
}

TEST_F(FunctionRegistryTest, RangeFunction_EmptyPositiveStep) {
	const ScalarFunction* func = registry->lookupScalarFunction("range");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(int64_t(5)));
	args.push_back(PropertyValue(int64_t(0))); // start >= end with positive default step
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::LIST);
	const auto& list = std::get<std::vector<PropertyValue>>(result.getVariant());
	EXPECT_EQ(list.size(), 0UL);
}

TEST_F(FunctionRegistryTest, RangeFunction_EmptyNegativeStep) {
	const ScalarFunction* func = registry->lookupScalarFunction("range");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(int64_t(0)));
	args.push_back(PropertyValue(int64_t(5)));
	args.push_back(PropertyValue(int64_t(-1))); // start < end with negative step
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::LIST);
	const auto& list = std::get<std::vector<PropertyValue>>(result.getVariant());
	EXPECT_EQ(list.size(), 0UL);
}

TEST_F(FunctionRegistryTest, RangeFunction_NullArgument) {
	const ScalarFunction* func = registry->lookupScalarFunction("range");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(int64_t(0)));
	args.push_back(PropertyValue()); // NULL end
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, RangeFunction_ZeroStep) {
	const ScalarFunction* func = registry->lookupScalarFunction("range");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(int64_t(0)));
	args.push_back(PropertyValue(int64_t(5)));
	args.push_back(PropertyValue(int64_t(0))); // step = 0

	EXPECT_THROW((void)func->evaluate(args, *context_), std::runtime_error);
}

TEST_F(FunctionRegistryTest, RangeFunction_NegativeStartPositiveEnd) {
	const ScalarFunction* func = registry->lookupScalarFunction("range");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(int64_t(-3)));
	args.push_back(PropertyValue(int64_t(2)));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::LIST);
	const auto& list = std::get<std::vector<PropertyValue>>(result.getVariant());
	EXPECT_EQ(list.size(), 6UL);
	EXPECT_EQ(std::get<int64_t>(list[0].getVariant()), -3);
	EXPECT_EQ(std::get<int64_t>(list[1].getVariant()), -2);
	EXPECT_EQ(std::get<int64_t>(list[2].getVariant()), -1);
	EXPECT_EQ(std::get<int64_t>(list[3].getVariant()), 0);
	EXPECT_EQ(std::get<int64_t>(list[4].getVariant()), 1);
	EXPECT_EQ(std::get<int64_t>(list[5].getVariant()), 2);
}

TEST_F(FunctionRegistryTest, RangeFunction_LargeStep) {
	const ScalarFunction* func = registry->lookupScalarFunction("range");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(int64_t(0)));
	args.push_back(PropertyValue(int64_t(100)));
	args.push_back(PropertyValue(int64_t(25)));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::LIST);
	const auto& list = std::get<std::vector<PropertyValue>>(result.getVariant());
	EXPECT_EQ(list.size(), 5UL);
	EXPECT_EQ(std::get<int64_t>(list[0].getVariant()), 0);
	EXPECT_EQ(std::get<int64_t>(list[1].getVariant()), 25);
	EXPECT_EQ(std::get<int64_t>(list[2].getVariant()), 50);
	EXPECT_EQ(std::get<int64_t>(list[3].getVariant()), 75);
	EXPECT_EQ(std::get<int64_t>(list[4].getVariant()), 100);
}

TEST_F(FunctionRegistryTest, RangeFunction_TooFewArgs) {
	const ScalarFunction* func = registry->lookupScalarFunction("range");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(int64_t(0)));
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, RangeFunction_TooManyArgs) {
	const ScalarFunction* func = registry->lookupScalarFunction("range");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(int64_t(0)));
	args.push_back(PropertyValue(int64_t(5)));
	args.push_back(PropertyValue(int64_t(1)));
	args.push_back(PropertyValue(int64_t(1)));
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, HeadFunction_NonEmptyList) {
	const ScalarFunction* func = registry->lookupScalarFunction("head");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	std::vector<PropertyValue> list = {PropertyValue(int64_t(10)), PropertyValue(int64_t(20)), PropertyValue(int64_t(30))};
	args.push_back(PropertyValue(list));
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 10);
}

TEST_F(FunctionRegistryTest, HeadFunction_EmptyList) {
	const ScalarFunction* func = registry->lookupScalarFunction("head");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	std::vector<PropertyValue> list;
	args.push_back(PropertyValue(list));
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, HeadFunction_NonList) {
	const ScalarFunction* func = registry->lookupScalarFunction("head");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(int64_t(42)));
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, HeadFunction_Null) {
	const ScalarFunction* func = registry->lookupScalarFunction("head");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue());
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, TailFunction_NonEmptyList) {
	const ScalarFunction* func = registry->lookupScalarFunction("tail");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	std::vector<PropertyValue> list = {PropertyValue(int64_t(10)), PropertyValue(int64_t(20)), PropertyValue(int64_t(30))};
	args.push_back(PropertyValue(list));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::LIST);
	const auto& rlist = std::get<std::vector<PropertyValue>>(result.getVariant());
	ASSERT_EQ(rlist.size(), 2UL);
	EXPECT_EQ(std::get<int64_t>(rlist[0].getVariant()), 20);
	EXPECT_EQ(std::get<int64_t>(rlist[1].getVariant()), 30);
}

TEST_F(FunctionRegistryTest, TailFunction_EmptyList) {
	const ScalarFunction* func = registry->lookupScalarFunction("tail");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	std::vector<PropertyValue> list;
	args.push_back(PropertyValue(list));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::LIST);
	const auto& rlist = std::get<std::vector<PropertyValue>>(result.getVariant());
	EXPECT_EQ(rlist.size(), 0UL);
}

TEST_F(FunctionRegistryTest, TailFunction_NonList) {
	const ScalarFunction* func = registry->lookupScalarFunction("tail");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(int64_t(42)));
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, TailFunction_Null) {
	const ScalarFunction* func = registry->lookupScalarFunction("tail");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue());
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, LastFunction_NonEmptyList) {
	const ScalarFunction* func = registry->lookupScalarFunction("last");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	std::vector<PropertyValue> list = {PropertyValue(int64_t(10)), PropertyValue(int64_t(20)), PropertyValue(int64_t(30))};
	args.push_back(PropertyValue(list));
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 30);
}

TEST_F(FunctionRegistryTest, LastFunction_EmptyList) {
	const ScalarFunction* func = registry->lookupScalarFunction("last");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	std::vector<PropertyValue> list;
	args.push_back(PropertyValue(list));
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, LastFunction_NonList) {
	const ScalarFunction* func = registry->lookupScalarFunction("last");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(int64_t(42)));
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, LastFunction_Null) {
	const ScalarFunction* func = registry->lookupScalarFunction("last");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue());
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, SizeFunction_NonStringNonList) {
	const ScalarFunction* func = registry->lookupScalarFunction("size");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(int64_t(42)));
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, SizeFunction_EmptyArgs) {
	const ScalarFunction* func = registry->lookupScalarFunction("size");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, HeadFunction_EmptyArgs) {
	const ScalarFunction* func = registry->lookupScalarFunction("head");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, TailFunction_EmptyArgs) {
	const ScalarFunction* func = registry->lookupScalarFunction("tail");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, LastFunction_EmptyArgs) {
	const ScalarFunction* func = registry->lookupScalarFunction("last");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, NodesFunction_NotList) {
	const ScalarFunction* func = registry->lookupScalarFunction("nodes");
	ASSERT_NE(func, nullptr);
	std::vector<PropertyValue> args{PropertyValue(std::string("not-a-list"))};
	EXPECT_EQ(func->evaluate(args, *context_).getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, NodesFunction_EmptyList) {
	const ScalarFunction* func = registry->lookupScalarFunction("nodes");
	std::vector<PropertyValue> args{PropertyValue(std::vector<PropertyValue>{})};
	auto result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::LIST);
	EXPECT_TRUE(std::get<std::vector<PropertyValue>>(result.getVariant()).empty());
}

TEST_F(FunctionRegistryTest, NodesFunction_WithNodeMaps) {
	const ScalarFunction* func = registry->lookupScalarFunction("nodes");
	PropertyValue::MapType nodeMap;
	nodeMap["_type"] = PropertyValue(std::string("node"));
	nodeMap["name"] = PropertyValue(std::string("Alice"));
	PropertyValue::MapType relMap;
	relMap["_type"] = PropertyValue(std::string("relationship"));
	std::vector<PropertyValue> pathList{PropertyValue(nodeMap), PropertyValue(relMap)};
	std::vector<PropertyValue> args{PropertyValue(pathList)};
	auto result = func->evaluate(args, *context_);
	EXPECT_EQ(std::get<std::vector<PropertyValue>>(result.getVariant()).size(), 1u);
}

TEST_F(FunctionRegistryTest, RelationshipsFunction_NotList) {
	const ScalarFunction* func = registry->lookupScalarFunction("relationships");
	ASSERT_NE(func, nullptr);
	std::vector<PropertyValue> args{PropertyValue(std::string("not-a-list"))};
	EXPECT_EQ(func->evaluate(args, *context_).getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, RelationshipsFunction_WithRelMaps) {
	const ScalarFunction* func = registry->lookupScalarFunction("relationships");
	PropertyValue::MapType nodeMap;
	nodeMap["_type"] = PropertyValue(std::string("node"));
	PropertyValue::MapType relMap;
	relMap["_type"] = PropertyValue(std::string("relationship"));
	std::vector<PropertyValue> pathList{PropertyValue(nodeMap), PropertyValue(relMap)};
	std::vector<PropertyValue> args{PropertyValue(pathList)};
	auto result = func->evaluate(args, *context_);
	EXPECT_EQ(std::get<std::vector<PropertyValue>>(result.getVariant()).size(), 1u);
}

TEST_F(FunctionRegistryTest, SizeFunction_NullArg) {
	const ScalarFunction* func = registry->lookupScalarFunction("size");
	ASSERT_NE(func, nullptr);
	std::vector<PropertyValue> args{PropertyValue()};
	EXPECT_EQ(func->evaluate(args, *context_).getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, SizeFunction_IntegerArg) {
	const ScalarFunction* func = registry->lookupScalarFunction("size");
	std::vector<PropertyValue> args{PropertyValue(static_cast<int64_t>(42))};
	EXPECT_EQ(func->evaluate(args, *context_).getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, RangeFunction_NegativeStep) {
	const ScalarFunction* func = registry->lookupScalarFunction("range");
	ASSERT_NE(func, nullptr);
	std::vector<PropertyValue> args{PropertyValue(static_cast<int64_t>(5)), PropertyValue(static_cast<int64_t>(1)), PropertyValue(static_cast<int64_t>(-1))};
	auto result = func->evaluate(args, *context_);
	auto& list = std::get<std::vector<PropertyValue>>(result.getVariant());
	EXPECT_EQ(list.size(), 5u); // 5,4,3,2,1
}

TEST_F(FunctionRegistryTest, NodesFunction_ListWithNonMapElements) {
	const ScalarFunction* func = registry->lookupScalarFunction("nodes");
	std::vector<PropertyValue> pathList{PropertyValue(std::string("not-a-map")), PropertyValue(static_cast<int64_t>(42))};
	std::vector<PropertyValue> args{PropertyValue(pathList)};
	auto result = func->evaluate(args, *context_);
	EXPECT_TRUE(std::get<std::vector<PropertyValue>>(result.getVariant()).empty());
}

TEST_F(FunctionRegistryTest, RelationshipsFunction_EmptyList) {
	const ScalarFunction* func = registry->lookupScalarFunction("relationships");
	std::vector<PropertyValue> args{PropertyValue(std::vector<PropertyValue>{})};
	auto result = func->evaluate(args, *context_);
	EXPECT_TRUE(std::get<std::vector<PropertyValue>>(result.getVariant()).empty());
}
