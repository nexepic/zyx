/**
 * @file test_ApiValueConversion.cpp
 * @author Nexepic
 * @date 2026/1/5
 *
 * @copyright Copyright (c) 2026 Nexepic
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

#include <unordered_map>
#include <variant>
#include <vector>

#include "ApiTestFixture.hpp"

// ============================================================================
// Internal Conversion Verification (ResultValue -> Value)
// ============================================================================

TEST_F(CppApiTest, InternalConversionCheck) {
	db->createNode("N", {{"id", (int64_t) 1}});
	db->createNode("M", {{"id", (int64_t) 2}});
	(void) db->execute("MATCH (n:N), (m:M) CREATE (n)-[r:REL {w: 10}]->(m)");
	db->save();

	auto res = db->execute("MATCH (n)-[r]->(m) RETURN n, r");
	ASSERT_TRUE(res.hasNext());
	res.next();

	auto nVal = res.get("n");
	ASSERT_TRUE(std::holds_alternative<std::shared_ptr<zyx::Node>>(nVal)) << "Result 'n' should be Node";

	auto rVal = res.get("r");
	ASSERT_TRUE(std::holds_alternative<std::shared_ptr<zyx::Edge>>(rVal))
			<< "Result 'r' should be Edge - Check toPublicValue logic!";
}

// ============================================================================
// Null Values and Vector Properties
// ============================================================================

TEST_F(CppApiTest, HandleNullPropertyValues) {
	db->createNode("NullTest", {{"name", "Test"}});
	db->save();

	auto res = db->execute("MATCH (n:NullTest) RETURN n.optionalProp");
	ASSERT_TRUE(res.hasNext());
	res.next();

	auto val = res.get("n.optionalProp");
	EXPECT_TRUE(std::holds_alternative<std::monostate>(val));
}

TEST_F(CppApiTest, HandleVectorProperties) {
	std::vector<std::string> vecProps = {"1.1", "2.2", "3.3"};
	db->createNode("VectorTest", {{"embeddings", vecProps}});
	db->save();

	auto res = db->execute("MATCH (n:VectorTest) RETURN n.embeddings");
	ASSERT_TRUE(res.hasNext());
	res.next();

	auto val = res.get("n.embeddings");
	ASSERT_TRUE(std::holds_alternative<std::vector<std::string>>(val));

	auto vec = std::get<std::vector<std::string>>(val);
	ASSERT_EQ(vec.size(), 3u);
	EXPECT_TRUE(vec[0] == "1.100000" || vec[0] == "1.1");
	EXPECT_TRUE(vec[1] == "2.200000" || vec[1] == "2.2");
	EXPECT_TRUE(vec[2] == "3.300000" || vec[2] == "3.3");
}

TEST_F(CppApiTest, VectorPropertyWithIntegerStrings) {
	std::vector<std::string> intVec = {"42", "100", "-7"};
	db->createNode("IntVecNode", {{"data", intVec}});
	db->save();

	auto res = db->execute("MATCH (n:IntVecNode) RETURN n.data");
	ASSERT_TRUE(res.hasNext());
	res.next();

	auto val = res.get("n.data");
	ASSERT_TRUE(std::holds_alternative<std::vector<std::string>>(val));
}

TEST_F(CppApiTest, VectorPropertyWithPlainStrings) {
	std::vector<std::string> strVec = {"hello", "world", "abc"};
	db->createNode("StrVecNode", {{"tags", strVec}});
	db->save();

	auto res = db->execute("MATCH (n:StrVecNode) RETURN n.tags");
	ASSERT_TRUE(res.hasNext());
	res.next();

	auto val = res.get("n.tags");
	ASSERT_TRUE(std::holds_alternative<std::vector<std::string>>(val));
	auto vec = std::get<std::vector<std::string>>(val);
	ASSERT_EQ(vec.size(), 3u);
}

TEST_F(CppApiTest, VectorPropertyWithMixedTypes) {
	std::vector<std::string> mixedVec = {"42", "3.14", "hello"};
	db->createNode("MixedVecNode", {{"mixed", mixedVec}});
	db->save();

	auto res = db->execute("MATCH (n:MixedVecNode) RETURN n.mixed");
	ASSERT_TRUE(res.hasNext());
	res.next();

	auto val = res.get("n.mixed");
	ASSERT_TRUE(std::holds_alternative<std::vector<std::string>>(val));
}

TEST_F(CppApiTest, CreateNodeWithMonostateProperty) {
	std::unordered_map<std::string, zyx::Value> props;
	props["name"] = "test";
	props["nullprop"] = std::monostate{};

	db->createNode("MonoTest", props);
	db->save();

	auto res = db->execute("MATCH (n:MonoTest) RETURN n.name");
	ASSERT_TRUE(res.hasNext());
	res.next();
	auto val = res.get("n.name");
	EXPECT_TRUE(std::holds_alternative<std::string>(val));
}

TEST_F(CppApiTest, VectorPropertyWithDoubleStrings) {
	std::vector<std::string> doubleVec = {"3.14", "2.718", "1.618"};
	db->createNode("DoubleVecNode", {{"data", doubleVec}});
	db->save();

	auto res = db->execute("MATCH (n:DoubleVecNode) RETURN n.data");
	ASSERT_TRUE(res.hasNext());
	res.next();

	auto val = res.get("n.data");
	ASSERT_TRUE(std::holds_alternative<std::vector<std::string>>(val));
}

TEST_F(CppApiTest, VectorPropertyWithPartialIntParse) {
	std::vector<std::string> partialVec = {"42abc", "not_a_number"};
	db->createNode("PartialVecNode", {{"data", partialVec}});
	db->save();

	auto res = db->execute("MATCH (n:PartialVecNode) RETURN n.data");
	ASSERT_TRUE(res.hasNext());
	res.next();

	auto val = res.get("n.data");
	ASSERT_TRUE(std::holds_alternative<std::vector<std::string>>(val));
}

TEST_F(CppApiTest, VectorPropertyWithPartialDoubleParse) {
	std::vector<std::string> partialDoubleVec = {"3.14xyz"};
	db->createNode("PartialDoubleNode", {{"data", partialDoubleVec}});
	db->save();

	auto res = db->execute("MATCH (n:PartialDoubleNode) RETURN n.data");
	ASSERT_TRUE(res.hasNext());
	res.next();

	auto val = res.get("n.data");
	ASSERT_TRUE(std::holds_alternative<std::vector<std::string>>(val));
}

TEST_F(CppApiTest, CreateNodeWithNodePtrProperty) {
	auto nodePtr = std::make_shared<zyx::Node>();
	nodePtr->id = 1;
	nodePtr->label = "Inner";

	std::unordered_map<std::string, zyx::Value> props;
	props["name"] = "test";
	props["ref"] = nodePtr;

	db->createNode("RefHolder", props);
	db->save();

	auto res = db->execute("MATCH (n:RefHolder) RETURN n.name");
	ASSERT_TRUE(res.hasNext());
	res.next();
	auto val = res.get("n.name");
	EXPECT_TRUE(std::holds_alternative<std::string>(val));
	EXPECT_EQ(std::get<std::string>(val), "test");
}

TEST_F(CppApiTest, CreateNodeWithEdgePtrProperty) {
	auto edgePtr = std::make_shared<zyx::Edge>();
	edgePtr->id = 1;
	edgePtr->type = "InnerEdge";

	std::unordered_map<std::string, zyx::Value> props;
	props["name"] = "test";
	props["ref"] = edgePtr;

	db->createNode("EdgeRefHolder", props);
	db->save();

	auto res = db->execute("MATCH (n:EdgeRefHolder) RETURN n.name");
	ASSERT_TRUE(res.hasNext());
	res.next();
	auto val = res.get("n.name");
	EXPECT_TRUE(std::holds_alternative<std::string>(val));
	EXPECT_EQ(std::get<std::string>(val), "test");
}

TEST_F(CppApiTest, VectorPropertyWithStodException) {
	std::vector<std::string> badVec = {"not_a_number_at_all", "!!!"};
	db->createNode("BadVecNode", {{"data", badVec}});
	db->save();

	auto res = db->execute("MATCH (n:BadVecNode) RETURN n.data");
	ASSERT_TRUE(res.hasNext());
	res.next();

	auto val = res.get("n.data");
	ASSERT_TRUE(std::holds_alternative<std::vector<std::string>>(val));
	auto vec = std::get<std::vector<std::string>>(val);
	ASSERT_EQ(vec.size(), 2u);
}
