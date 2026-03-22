/**
 * @file test_EntityIntrospectionFunction.cpp
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

#include <gtest/gtest.h>
#include <filesystem>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include "graph/query/expressions/FunctionRegistry.hpp"
#include "graph/query/expressions/EvaluationContext.hpp"
#include "graph/query/execution/Record.hpp"
#include "graph/core/Node.hpp"
#include "graph/core/Edge.hpp"
#include "graph/core/Database.hpp"
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/data/DataManager.hpp"

using namespace graph;
using namespace graph::query::expressions;
using namespace graph::query::execution;

class EntityIntrospectionTest : public ::testing::Test {
protected:
	void SetUp() override {
		registry_ = &FunctionRegistry::getInstance();
	}

	FunctionRegistry* registry_;
};

// ============================================================================
// id() function tests
// ============================================================================

TEST_F(EntityIntrospectionTest, Id_Node) {
	Record record;
	Node node(42, 1);
	record.setNode("n", node);
	EvaluationContext context(record);

	const ScalarFunction* func = registry_->lookupScalarFunction("id");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("n")));
	PropertyValue result = func->evaluate(args, context);

	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 42);
}

TEST_F(EntityIntrospectionTest, Id_Edge) {
	Record record;
	Edge edge(99, 1, 2, 3);
	record.setEdge("r", edge);
	EvaluationContext context(record);

	const ScalarFunction* func = registry_->lookupScalarFunction("id");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("r")));
	PropertyValue result = func->evaluate(args, context);

	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 99);
}

TEST_F(EntityIntrospectionTest, Id_UnknownVariable) {
	Record record;
	EvaluationContext context(record);

	const ScalarFunction* func = registry_->lookupScalarFunction("id");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("unknown")));
	EXPECT_THROW((void)func->evaluate(args, context), ExpressionEvaluationException);
}

// ============================================================================
// labels() function tests
// ============================================================================

TEST_F(EntityIntrospectionTest, Labels_NodeWithLabelId_NoDataManager) {
	Record record;
	Node node(1, 5);
	record.setNode("n", node);
	EvaluationContext context(record);

	const ScalarFunction* func = registry_->lookupScalarFunction("labels");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("n")));
	PropertyValue result = func->evaluate(args, context);

	EXPECT_EQ(result.getType(), PropertyType::LIST);
	const auto& list = std::get<std::vector<PropertyValue>>(result.getVariant());
	ASSERT_EQ(list.size(), 1UL);
	EXPECT_EQ(std::get<int64_t>(list[0].getVariant()), 5);
}

TEST_F(EntityIntrospectionTest, Labels_NodeWithZeroLabelId) {
	Record record;
	Node node(1, 0);
	record.setNode("n", node);
	EvaluationContext context(record);

	const ScalarFunction* func = registry_->lookupScalarFunction("labels");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("n")));
	PropertyValue result = func->evaluate(args, context);

	EXPECT_EQ(result.getType(), PropertyType::LIST);
	const auto& list = std::get<std::vector<PropertyValue>>(result.getVariant());
	EXPECT_TRUE(list.empty());
}

TEST_F(EntityIntrospectionTest, Labels_NotANode) {
	Record record;
	Edge edge(1, 1, 2, 3);
	record.setEdge("r", edge);
	EvaluationContext context(record);

	const ScalarFunction* func = registry_->lookupScalarFunction("labels");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("r")));
	EXPECT_THROW((void)func->evaluate(args, context), ExpressionEvaluationException);
}

TEST_F(EntityIntrospectionTest, Labels_UnknownVariable) {
	Record record;
	EvaluationContext context(record);

	const ScalarFunction* func = registry_->lookupScalarFunction("labels");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("unknown")));
	EXPECT_THROW((void)func->evaluate(args, context), ExpressionEvaluationException);
}

// ============================================================================
// type() function tests
// ============================================================================

TEST_F(EntityIntrospectionTest, Type_Edge_NoDataManager) {
	Record record;
	Edge edge(1, 10, 20, 7);
	record.setEdge("r", edge);
	EvaluationContext context(record);

	const ScalarFunction* func = registry_->lookupScalarFunction("type");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("r")));
	PropertyValue result = func->evaluate(args, context);

	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 7);
}

TEST_F(EntityIntrospectionTest, Type_EdgeWithZeroLabel) {
	Record record;
	Edge edge(1, 10, 20, 0);
	record.setEdge("r", edge);
	EvaluationContext context(record);

	const ScalarFunction* func = registry_->lookupScalarFunction("type");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("r")));
	PropertyValue result = func->evaluate(args, context);

	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 0);
}

TEST_F(EntityIntrospectionTest, Type_NotAnEdge) {
	Record record;
	Node node(1, 1);
	record.setNode("n", node);
	EvaluationContext context(record);

	const ScalarFunction* func = registry_->lookupScalarFunction("type");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("n")));
	EXPECT_THROW((void)func->evaluate(args, context), ExpressionEvaluationException);
}

TEST_F(EntityIntrospectionTest, Type_UnknownVariable) {
	Record record;
	EvaluationContext context(record);

	const ScalarFunction* func = registry_->lookupScalarFunction("type");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("unknown")));
	EXPECT_THROW((void)func->evaluate(args, context), ExpressionEvaluationException);
}

// ============================================================================
// keys() function tests
// ============================================================================

TEST_F(EntityIntrospectionTest, Keys_NodeWithProperties) {
	Record record;
	Node node(1, 1);
	std::unordered_map<std::string, PropertyValue> props;
	props["name"] = PropertyValue(std::string("Alice"));
	props["age"] = PropertyValue(int64_t(30));
	node.setProperties(props);
	record.setNode("n", node);
	EvaluationContext context(record);

	const ScalarFunction* func = registry_->lookupScalarFunction("keys");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("n")));
	PropertyValue result = func->evaluate(args, context);

	EXPECT_EQ(result.getType(), PropertyType::LIST);
	const auto& list = std::get<std::vector<PropertyValue>>(result.getVariant());
	EXPECT_EQ(list.size(), 2UL);

	std::set<std::string> keySet;
	for (const auto& k : list) {
		keySet.insert(std::get<std::string>(k.getVariant()));
	}
	EXPECT_TRUE(keySet.count("name"));
	EXPECT_TRUE(keySet.count("age"));
}

TEST_F(EntityIntrospectionTest, Keys_EdgeWithProperties) {
	Record record;
	Edge edge(1, 10, 20, 3);
	std::unordered_map<std::string, PropertyValue> props;
	props["weight"] = PropertyValue(0.5);
	edge.setProperties(props);
	record.setEdge("r", edge);
	EvaluationContext context(record);

	const ScalarFunction* func = registry_->lookupScalarFunction("keys");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("r")));
	PropertyValue result = func->evaluate(args, context);

	EXPECT_EQ(result.getType(), PropertyType::LIST);
	const auto& list = std::get<std::vector<PropertyValue>>(result.getVariant());
	ASSERT_EQ(list.size(), 1UL);
	EXPECT_EQ(std::get<std::string>(list[0].getVariant()), "weight");
}

TEST_F(EntityIntrospectionTest, Keys_EmptyProperties) {
	Record record;
	Node node(1, 1);
	record.setNode("n", node);
	EvaluationContext context(record);

	const ScalarFunction* func = registry_->lookupScalarFunction("keys");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("n")));
	PropertyValue result = func->evaluate(args, context);

	EXPECT_EQ(result.getType(), PropertyType::LIST);
	const auto& list = std::get<std::vector<PropertyValue>>(result.getVariant());
	EXPECT_TRUE(list.empty());
}

TEST_F(EntityIntrospectionTest, Keys_UnknownVariable) {
	Record record;
	EvaluationContext context(record);

	const ScalarFunction* func = registry_->lookupScalarFunction("keys");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("unknown")));
	EXPECT_THROW((void)func->evaluate(args, context), ExpressionEvaluationException);
}

// ============================================================================
// properties() function tests
// ============================================================================

TEST_F(EntityIntrospectionTest, Properties_NodeWithProperties) {
	Record record;
	Node node(1, 1);
	std::unordered_map<std::string, PropertyValue> props;
	props["name"] = PropertyValue(std::string("Bob"));
	props["age"] = PropertyValue(int64_t(25));
	node.setProperties(props);
	record.setNode("n", node);
	EvaluationContext context(record);

	const ScalarFunction* func = registry_->lookupScalarFunction("properties");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("n")));
	PropertyValue result = func->evaluate(args, context);

	EXPECT_EQ(result.getType(), PropertyType::MAP);
	const auto& map = std::get<PropertyValue::MapType>(result.getVariant());
	ASSERT_EQ(map.size(), 2UL);
	EXPECT_EQ(std::get<std::string>(map.at("name").getVariant()), "Bob");
	EXPECT_EQ(std::get<int64_t>(map.at("age").getVariant()), 25);
}

TEST_F(EntityIntrospectionTest, Properties_EdgeWithProperties) {
	Record record;
	Edge edge(1, 10, 20, 3);
	std::unordered_map<std::string, PropertyValue> props;
	props["since"] = PropertyValue(int64_t(2020));
	edge.setProperties(props);
	record.setEdge("r", edge);
	EvaluationContext context(record);

	const ScalarFunction* func = registry_->lookupScalarFunction("properties");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("r")));
	PropertyValue result = func->evaluate(args, context);

	EXPECT_EQ(result.getType(), PropertyType::MAP);
	const auto& map = std::get<PropertyValue::MapType>(result.getVariant());
	ASSERT_EQ(map.size(), 1UL);
	EXPECT_EQ(std::get<int64_t>(map.at("since").getVariant()), 2020);
}

TEST_F(EntityIntrospectionTest, Properties_EmptyProperties) {
	Record record;
	Node node(1, 1);
	record.setNode("n", node);
	EvaluationContext context(record);

	const ScalarFunction* func = registry_->lookupScalarFunction("properties");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("n")));
	PropertyValue result = func->evaluate(args, context);

	EXPECT_EQ(result.getType(), PropertyType::MAP);
	const auto& map = std::get<PropertyValue::MapType>(result.getVariant());
	EXPECT_TRUE(map.empty());
}

TEST_F(EntityIntrospectionTest, Properties_UnknownVariable) {
	Record record;
	EvaluationContext context(record);

	const ScalarFunction* func = registry_->lookupScalarFunction("properties");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("unknown")));
	EXPECT_THROW((void)func->evaluate(args, context), ExpressionEvaluationException);
}

// ============================================================================
// Additional branch coverage tests
// ============================================================================

TEST_F(EntityIntrospectionTest, Labels_NodeWithZeroLabelId_NoDataManager) {
	// Cover: dm is null AND labelId == 0 -> empty labels list
	// (Already covered above, but ensure the dm==nullptr && labelId==0 path)
	Record record;
	Node node(5, 0);
	record.setNode("n", node);
	EvaluationContext context(record); // no DataManager

	const ScalarFunction* func = registry_->lookupScalarFunction("labels");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("n")));
	PropertyValue result = func->evaluate(args, context);

	EXPECT_EQ(result.getType(), PropertyType::LIST);
	const auto& list = std::get<std::vector<PropertyValue>>(result.getVariant());
	EXPECT_TRUE(list.empty());
}

TEST_F(EntityIntrospectionTest, Type_EdgeWithZeroLabel_NoDataManager) {
	// Cover: dm is null AND labelId == 0 -> returns labelId as integer (0)
	Record record;
	Edge edge(5, 10, 20, 0);
	record.setEdge("r", edge);
	EvaluationContext context(record); // no DataManager

	const ScalarFunction* func = registry_->lookupScalarFunction("type");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("r")));
	PropertyValue result = func->evaluate(args, context);

	// With no DataManager and labelId=0, should return PropertyValue(0)
	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 0);
}

TEST_F(EntityIntrospectionTest, Id_EdgeBeforeNode) {
	// Cover: id() where record has edge but not node for same variable name
	// The function checks getNode first, then getEdge.
	// If we only have an edge, it falls through the node check to edge check.
	Record record;
	Edge edge(77, 1, 2, 3);
	record.setEdge("e", edge);
	EvaluationContext context(record);

	const ScalarFunction* func = registry_->lookupScalarFunction("id");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("e")));
	PropertyValue result = func->evaluate(args, context);

	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 77);
}

TEST_F(EntityIntrospectionTest, Keys_NodeWithNoProperties) {
	// Cover: keys() on node with empty properties - the for loop iterates 0 times
	Record record;
	Node node(10, 1);
	record.setNode("n", node);
	EvaluationContext context(record);

	const ScalarFunction* func = registry_->lookupScalarFunction("keys");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("n")));
	PropertyValue result = func->evaluate(args, context);

	const auto& list = std::get<std::vector<PropertyValue>>(result.getVariant());
	EXPECT_TRUE(list.empty());
}

TEST_F(EntityIntrospectionTest, Properties_EdgeWithNoProperties) {
	// Cover: properties() on edge with empty properties - the for loop iterates 0 times
	Record record;
	Edge edge(10, 1, 2, 3);
	record.setEdge("r", edge);
	EvaluationContext context(record);

	const ScalarFunction* func = registry_->lookupScalarFunction("properties");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("r")));
	PropertyValue result = func->evaluate(args, context);

	const auto& map = std::get<PropertyValue::MapType>(result.getVariant());
	EXPECT_TRUE(map.empty());
}

TEST_F(EntityIntrospectionTest, Keys_EdgeWithNoProperties) {
	// Cover: keys() on edge with empty properties
	Record record;
	Edge edge(10, 1, 2, 3);
	record.setEdge("r", edge);
	EvaluationContext context(record);

	const ScalarFunction* func = registry_->lookupScalarFunction("keys");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("r")));
	PropertyValue result = func->evaluate(args, context);

	const auto& list = std::get<std::vector<PropertyValue>>(result.getVariant());
	EXPECT_TRUE(list.empty());
}

// ============================================================================
// Additional branch coverage: keys/properties with edge having properties
// ============================================================================

TEST_F(EntityIntrospectionTest, Keys_EdgeWithMultipleProperties) {
	// Cover: keys() on edge with multiple properties - the for loop runs multiple iterations
	Record record;
	Edge edge(20, 10, 20, 5);
	std::unordered_map<std::string, PropertyValue> props;
	props["weight"] = PropertyValue(1.5);
	props["since"] = PropertyValue(int64_t(2020));
	props["label"] = PropertyValue(std::string("friend"));
	edge.setProperties(props);
	record.setEdge("r", edge);
	EvaluationContext context(record);

	const ScalarFunction* func = registry_->lookupScalarFunction("keys");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("r")));
	PropertyValue result = func->evaluate(args, context);

	const auto& list = std::get<std::vector<PropertyValue>>(result.getVariant());
	EXPECT_EQ(list.size(), 3UL);

	std::set<std::string> keySet;
	for (const auto& k : list) {
		keySet.insert(std::get<std::string>(k.getVariant()));
	}
	EXPECT_TRUE(keySet.count("weight"));
	EXPECT_TRUE(keySet.count("since"));
	EXPECT_TRUE(keySet.count("label"));
}

TEST_F(EntityIntrospectionTest, Properties_EdgeWithMultipleProperties) {
	// Cover: properties() on edge with multiple properties - the for loop runs multiple iterations
	Record record;
	Edge edge(20, 10, 20, 5);
	std::unordered_map<std::string, PropertyValue> props;
	props["weight"] = PropertyValue(1.5);
	props["since"] = PropertyValue(int64_t(2020));
	edge.setProperties(props);
	record.setEdge("r", edge);
	EvaluationContext context(record);

	const ScalarFunction* func = registry_->lookupScalarFunction("properties");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("r")));
	PropertyValue result = func->evaluate(args, context);

	const auto& map = std::get<PropertyValue::MapType>(result.getVariant());
	EXPECT_EQ(map.size(), 2UL);
	EXPECT_EQ(std::get<double>(map.at("weight").getVariant()), 1.5);
	EXPECT_EQ(std::get<int64_t>(map.at("since").getVariant()), 2020);
}

TEST_F(EntityIntrospectionTest, Properties_NodeWithMultiplePropertyTypes) {
	// Cover: properties() on node with many different types
	Record record;
	Node node(1, 1);
	std::unordered_map<std::string, PropertyValue> props;
	props["name"] = PropertyValue(std::string("Alice"));
	props["age"] = PropertyValue(int64_t(30));
	props["score"] = PropertyValue(9.5);
	props["active"] = PropertyValue(true);
	node.setProperties(props);
	record.setNode("n", node);
	EvaluationContext context(record);

	const ScalarFunction* func = registry_->lookupScalarFunction("properties");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("n")));
	PropertyValue result = func->evaluate(args, context);

	const auto& map = std::get<PropertyValue::MapType>(result.getVariant());
	EXPECT_EQ(map.size(), 4UL);
	EXPECT_EQ(std::get<std::string>(map.at("name").getVariant()), "Alice");
	EXPECT_EQ(std::get<int64_t>(map.at("age").getVariant()), 30);
	EXPECT_TRUE(std::get<bool>(map.at("active").getVariant()));
}

TEST_F(EntityIntrospectionTest, Id_NodeNotEdge) {
	// Cover: id() where record has node and edge with DIFFERENT names
	// Tests that node check succeeds before edge check
	Record record;
	Node node(100, 1);
	Edge edge(200, 1, 2, 3);
	record.setNode("n", node);
	record.setEdge("r", edge);
	EvaluationContext context(record);

	const ScalarFunction* func = registry_->lookupScalarFunction("id");
	ASSERT_NE(func, nullptr);

	// Get id of node
	std::vector<PropertyValue> argsNode;
	argsNode.push_back(PropertyValue(std::string("n")));
	PropertyValue nodeResult = func->evaluate(argsNode, context);
	EXPECT_EQ(std::get<int64_t>(nodeResult.getVariant()), 100);

	// Get id of edge
	std::vector<PropertyValue> argsEdge;
	argsEdge.push_back(PropertyValue(std::string("r")));
	PropertyValue edgeResult = func->evaluate(argsEdge, context);
	EXPECT_EQ(std::get<int64_t>(edgeResult.getVariant()), 200);
}

// ============================================================================
// DataManager branch coverage: labels() and type() with DataManager
// ============================================================================

class EntityIntrospectionWithDataManagerTest : public ::testing::Test {
protected:
	void SetUp() override {
		registry_ = &FunctionRegistry::getInstance();

		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath_ = std::filesystem::temp_directory_path() /
			("test_entity_introspection_" + to_string(uuid) + ".dat");

		database_ = std::make_unique<graph::Database>(testFilePath_.string());
		database_->open();
		auto storage = database_->getStorage();
		dataManager_ = storage->getDataManager().get();
	}

	void TearDown() override {
		database_->close();
		database_.reset();
		if (std::filesystem::exists(testFilePath_)) {
			std::filesystem::remove(testFilePath_);
		}
	}

	FunctionRegistry* registry_;
	std::filesystem::path testFilePath_;
	std::unique_ptr<graph::Database> database_;
	graph::storage::DataManager* dataManager_ = nullptr;
};

TEST_F(EntityIntrospectionWithDataManagerTest, Labels_WithDataManager_NonZeroLabelId) {
	// Cover: labelsImpl branch where dm != nullptr AND labelId != 0
	// This resolves the label name via DataManager
	int64_t labelId = dataManager_->getOrCreateLabelId("Person");

	Record record;
	Node node(1, labelId);
	record.setNode("n", node);
	EvaluationContext context(record, dataManager_);

	const ScalarFunction* func = registry_->lookupScalarFunction("labels");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("n")));
	PropertyValue result = func->evaluate(args, context);

	EXPECT_EQ(result.getType(), PropertyType::LIST);
	const auto& list = std::get<std::vector<PropertyValue>>(result.getVariant());
	ASSERT_EQ(list.size(), 1UL);
	EXPECT_EQ(std::get<std::string>(list[0].getVariant()), "Person");
}

TEST_F(EntityIntrospectionWithDataManagerTest, Labels_WithDataManager_ZeroLabelId) {
	// Cover: labelsImpl branch where dm != nullptr AND labelId == 0
	// This falls through to the no-DataManager path with empty label list
	Record record;
	Node node(2, 0);
	record.setNode("n", node);
	EvaluationContext context(record, dataManager_);

	const ScalarFunction* func = registry_->lookupScalarFunction("labels");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("n")));
	PropertyValue result = func->evaluate(args, context);

	EXPECT_EQ(result.getType(), PropertyType::LIST);
	const auto& list = std::get<std::vector<PropertyValue>>(result.getVariant());
	EXPECT_TRUE(list.empty());
}

TEST_F(EntityIntrospectionWithDataManagerTest, Type_WithDataManager_NonZeroLabelId) {
	// Cover: typeImpl branch where dm != nullptr AND labelId != 0
	// This resolves the relationship type name via DataManager
	int64_t labelId = dataManager_->getOrCreateLabelId("KNOWS");

	Record record;
	Edge edge(1, 10, 20, labelId);
	record.setEdge("r", edge);
	EvaluationContext context(record, dataManager_);

	const ScalarFunction* func = registry_->lookupScalarFunction("type");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("r")));
	PropertyValue result = func->evaluate(args, context);

	EXPECT_EQ(result.getType(), PropertyType::STRING);
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "KNOWS");
}

TEST_F(EntityIntrospectionWithDataManagerTest, Type_WithDataManager_ZeroLabelId) {
	// Cover: typeImpl branch where dm != nullptr AND labelId == 0
	// Falls through to return labelId as integer (0)
	Record record;
	Edge edge(2, 10, 20, 0);
	record.setEdge("r", edge);
	EvaluationContext context(record, dataManager_);

	const ScalarFunction* func = registry_->lookupScalarFunction("type");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("r")));
	PropertyValue result = func->evaluate(args, context);

	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 0);
}
