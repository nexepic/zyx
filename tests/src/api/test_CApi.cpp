/**
 * @file test_CApi.cpp
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

#include <cstring>
#include <filesystem>
#include <gtest/gtest.h>
#include <string>
#include "metrix/metrix_c_api.h"

namespace fs = std::filesystem;

class CApiTest : public ::testing::Test {
protected:
	std::string dbPath;
	MetrixDB_T *db = nullptr;

	void SetUp() override {
		// Create a unique temporary path for the database
		auto tempDir = fs::temp_directory_path();
		dbPath = (tempDir / ("c_api_test_" + std::to_string(std::rand()))).string();
		if (fs::exists(dbPath))
			fs::remove_all(dbPath);
		// Standard open creates the DB
		db = metrix_open(dbPath.c_str());
	}

	void TearDown() override {
		if (db)
			metrix_close(db);
		if (fs::exists(dbPath))
			fs::remove_all(dbPath);
	}
};

TEST_F(CApiTest, ExecuteAndIterate) {
	metrix_result_close(metrix_execute(db, "CREATE (n:CNode {id: 1, name: 'C-Lang', pi: 3.14, stable: true})"));

	MetrixResult_T *res = metrix_execute(db, "MATCH (n:CNode) RETURN n.id, n.name, n.pi, n.stable");
	ASSERT_NE(res, nullptr);
	ASSERT_TRUE(metrix_result_next(res));

	// Map columns dynamically to be robust against sorting/parser changes
	int idxId = -1, idxName = -1, idxPi = -1, idxBool = -1;
	int cols = metrix_result_column_count(res);
	for (int i = 0; i < cols; i++) {
		std::string name = metrix_result_column_name(res, i);
		if (name.find("id") != std::string::npos)
			idxId = i;
		else if (name.find("name") != std::string::npos)
			idxName = i;
		else if (name.find("pi") != std::string::npos)
			idxPi = i;
		else if (name.find("stable") != std::string::npos)
			idxBool = i;
	}

	ASSERT_NE(idxId, -1);
	ASSERT_NE(idxName, -1);

	// Verify values
	EXPECT_EQ(metrix_result_get_int(res, idxId), 1);
	EXPECT_STREQ(metrix_result_get_string(res, idxName), "C-Lang");
	EXPECT_DOUBLE_EQ(metrix_result_get_double(res, idxPi), 3.14);
	EXPECT_EQ(metrix_result_get_bool(res, idxBool), true);

	metrix_result_close(res);
}

TEST_F(CApiTest, NodeObjectAccess) {
	metrix_result_close(metrix_execute(db, "CREATE (n:Obj {prop: 'val'})"));

	auto *res = metrix_execute(db, "MATCH (n:Obj) RETURN n");
	ASSERT_TRUE(metrix_result_next(res));

	EXPECT_EQ(metrix_result_get_type(res, 0), MX_NODE);

	MetrixNode node_struct;
	bool success = metrix_result_get_node(res, 0, &node_struct);
	EXPECT_TRUE(success);
	EXPECT_STREQ(node_struct.label, "Obj");

	const char *json = metrix_result_get_props_json(res, 0);
	std::string jsonStr(json);
	EXPECT_NE(jsonStr.find("prop"), std::string::npos);

	metrix_result_close(res);
}

TEST_F(CApiTest, EdgeObjectAccess) {
	metrix_result_close(metrix_execute(db, "CREATE (a:N {id:1})"));
	metrix_result_close(metrix_execute(db, "CREATE (b:N {id:2})"));

	// Match and Create edge
	metrix_result_close(metrix_execute(db, "MATCH (a:N {id:1}), (b:N {id:2}) CREATE (a)-[e:REL {w:10}]->(b)"));

	auto *res = metrix_execute(db, "MATCH ()-[e:REL]->() RETURN e");
	ASSERT_TRUE(metrix_result_next(res));

	EXPECT_EQ(metrix_result_get_type(res, 0), MX_EDGE);

	MetrixEdge edge_struct;
	bool success = metrix_result_get_edge(res, 0, &edge_struct);
	ASSERT_TRUE(success);
	EXPECT_STREQ(edge_struct.type, "REL");

	metrix_result_close(res);
}

TEST_F(CApiTest, ErrorHandling) {
	MetrixDB_T *bad_db = metrix_open(nullptr);
	EXPECT_EQ(bad_db, nullptr);
	MetrixResult_T *res = metrix_execute(nullptr, "MATCH (n) RETURN n");
	EXPECT_EQ(res, nullptr);
}

TEST_F(CApiTest, EmptyResult) {
	auto *res = metrix_execute(db, "MATCH (n:NonExistent) RETURN n");
	EXPECT_FALSE(metrix_result_next(res));
	metrix_result_close(res);
}

TEST_F(CApiTest, OutOfBoundsAccess) {
	metrix_result_close(metrix_execute(db, "CREATE (n:OneCol)"));
	auto *res = metrix_execute(db, "MATCH (n:OneCol) RETURN n");
	metrix_result_next(res);

	// Index 0 valid
	EXPECT_EQ(metrix_result_get_type(res, 0), MX_NODE);
	// Index 1 invalid
	EXPECT_EQ(metrix_result_get_type(res, 1), MX_NULL);

	metrix_result_close(res);
}

TEST_F(CApiTest, EdgeObjectAccess_Rigorous) {
	// 1. Setup
	metrix_result_close(metrix_execute(db, "CREATE (a:N {id:1})"));
	metrix_result_close(metrix_execute(db, "CREATE (b:N {id:2})"));

	// 2. Action: Match and Create
	// Using a complex query to ensure planner works correctly
	const char *createQuery = "MATCH (a:N {id:1}), (b:N {id:2}) CREATE (a)-[e:REL {w:10}]->(b)";
	MetrixResult_T *createRes = metrix_execute(db, createQuery);
	ASSERT_NE(createRes, nullptr);
	metrix_result_close(createRes);

	// 3. Verification
	auto *res = metrix_execute(db, "MATCH ()-[e:REL]->() RETURN e");
	ASSERT_NE(res, nullptr);

	// Rigorous Check 1: Existence
	ASSERT_TRUE(metrix_result_next(res)) << "No edge returned.";

	// Rigorous Check 2: Metadata
	EXPECT_EQ(metrix_result_get_type(res, 0), MX_EDGE);

	// Rigorous Check 3: Topology and Props
	MetrixEdge edge_struct;
	bool success = metrix_result_get_edge(res, 0, &edge_struct);
	ASSERT_TRUE(success);
	EXPECT_GT(edge_struct.id, 0);
	EXPECT_GT(edge_struct.source_id, 0);
	EXPECT_GT(edge_struct.target_id, 0);
	EXPECT_STREQ(edge_struct.type, "REL");

	// Check Property JSON string
	const char *jsonProps = metrix_result_get_props_json(res, 0);
	std::string jsonStr(jsonProps);
	EXPECT_NE(jsonStr.find("\"w\":10"), std::string::npos) << "Property w:10 missing in JSON";

	// Rigorous Check 4: Cardinality (Should be exactly 1)
	ASSERT_FALSE(metrix_result_next(res)) << "Expected exactly 1 edge, found more.";

	metrix_result_close(res);
}

// ============================================================================
// NEW TESTS FOR FULL COVERAGE
// ============================================================================

TEST_F(CApiTest, OpenIfExists) {
	// 1. Close the DB opened by SetUp to release the lock/handle
	metrix_close(db);
	db = nullptr;

	// 2. Test opening existing DB (path was created in SetUp)
	db = metrix_open_if_exists(dbPath.c_str());
	ASSERT_NE(db, nullptr) << "Failed to open existing DB with metrix_open_if_exists";
	metrix_close(db);
	db = nullptr;

	// 3. Test opening non-existent DB
	std::string badPath = dbPath + "_non_existent";
	if (fs::exists(badPath))
		fs::remove_all(badPath);

	db = metrix_open_if_exists(badPath.c_str());
	EXPECT_EQ(db, nullptr) << "metrix_open_if_exists should return null for missing DB";
}

TEST_F(CApiTest, NullHandleSafeguards) {
	// Test that passing nullptr to result accessors handles it gracefully
	MetrixResult_T *nullRes = nullptr;

	EXPECT_FALSE(metrix_result_next(nullRes));
	EXPECT_EQ(metrix_result_column_count(nullRes), 0);
	EXPECT_STREQ(metrix_result_column_name(nullRes, 0), "");

	EXPECT_EQ(metrix_result_get_type(nullRes, 0), MX_NULL);
	EXPECT_EQ(metrix_result_get_int(nullRes, 0), 0);
	EXPECT_DOUBLE_EQ(metrix_result_get_double(nullRes, 0), 0.0);
	EXPECT_FALSE(metrix_result_get_bool(nullRes, 0));
	EXPECT_STREQ(metrix_result_get_string(nullRes, 0), "");

	// Test safeguards for complex types
	MetrixNode node;
	EXPECT_FALSE(metrix_result_get_node(nullRes, 0, &node));

	MetrixEdge edge;
	EXPECT_FALSE(metrix_result_get_edge(nullRes, 0, &edge));

	EXPECT_STREQ(metrix_result_get_props_json(nullRes, 0), "{}");
	EXPECT_STREQ(metrix_result_get_error(nullRes), "Invalid Result Handle");
}

TEST_F(CApiTest, TypeMismatchSafeguards) {
	// Create a node with a property
	metrix_result_close(metrix_execute(db, "CREATE (n:Safe {val: 42})"));
	auto *res = metrix_execute(db, "MATCH (n:Safe) RETURN n.val");
	ASSERT_TRUE(metrix_result_next(res));

	// Valid access: Integer
	EXPECT_EQ(metrix_result_get_type(res, 0), MX_INT);
	EXPECT_EQ(metrix_result_get_int(res, 0), 42);

	// Invalid access: Trying to interpret Integer as Node
	MetrixNode node;
	EXPECT_FALSE(metrix_result_get_node(res, 0, &node)) << "Should not read Integer as Node";

	// Invalid access: Trying to interpret Integer as Edge
	MetrixEdge edge;
	EXPECT_FALSE(metrix_result_get_edge(res, 0, &edge)) << "Should not read Integer as Edge";

	metrix_result_close(res);
}

TEST_F(CApiTest, ResultStatusAndGlobalError) {
	// 1. Success Case
	auto *goodRes = metrix_execute(db, "RETURN 1");
	ASSERT_NE(goodRes, nullptr);
	EXPECT_TRUE(metrix_result_is_success(goodRes));
	metrix_result_close(goodRes);

	// 2. Failure Case (Triggered via exception in metrix_execute -> returns null)
	// Assuming the DB throws on invalid syntax
	auto *badRes = metrix_execute(db, "THIS IS INVALID CYPHER");
	if (badRes == nullptr) {
		// Check global error
		const char *err = metrix_get_last_error();
		EXPECT_NE(err, nullptr);
		// We expect a non-empty error message, though exact content depends on parser
		EXPECT_STRNE(err, "");
	} else {
		// Implementation might return a result object representing failure
		EXPECT_FALSE(metrix_result_is_success(badRes));
		const char *err = metrix_result_get_error(badRes);
		EXPECT_NE(err, nullptr);
		EXPECT_STRNE(err, "");
		metrix_result_close(badRes);
	}
}

TEST_F(CApiTest, UninitializedResultAccess) {
	// Manually create a result handle with internal null state
	// (Simulating a critical failure in allocation or logic)
	// Note: Since MetrixResult_T's constructor takes a value, we can't easily fake a null internal state
	// without hacking.
	// Instead, we verify that closing a nullptr is safe.
	metrix_result_close(nullptr);

	// Verify metrix_get_last_error is safe to call anytime
	const char *err = metrix_get_last_error();
	// It might be empty or hold previous error, just ensure no crash
	(void) err;
}

// Verify Large String Handling (Buffer Check)
TEST_F(CApiTest, LargeStringPayload) {
	std::string large(10000, 'x'); // 10KB string
	std::string query = "RETURN '" + large + "' AS s";

	auto *res = metrix_execute(db, query.c_str());
	ASSERT_TRUE(metrix_result_next(res));

	const char *val = metrix_result_get_string(res, 0);
	EXPECT_EQ(std::strlen(val), 10000UL); // Using UL to suppress sign-compare warning
	EXPECT_EQ(val[0], 'x');
	EXPECT_EQ(val[9999], 'x');

	metrix_result_close(res);
}

// ============================================================================
// Additional tests for improved coverage
// ============================================================================

TEST_F(CApiTest, GetDuration) {
	auto *res = metrix_execute(db, "RETURN 1 AS x");
	ASSERT_NE(res, nullptr);
	ASSERT_TRUE(metrix_result_next(res));

	double duration = metrix_result_get_duration(res);
	EXPECT_GE(duration, 0.0);

	metrix_result_close(res);
}

TEST_F(CApiTest, GetDurationWithNullResult) {
	double duration = metrix_result_get_duration(nullptr);
	EXPECT_EQ(duration, 0.0);
}

TEST_F(CApiTest, JSONEscapeSequences) {
	// Use Cypher's escape syntax for newlines and quotes
	// Cypher uses backslash escaping within strings
	metrix_result_close(metrix_execute(db, "CREATE (n:Special {text: 'Line1\\nLine2\\tTab\"Quote\\\\Slash'})"));

	auto *res = metrix_execute(db, "MATCH (n:Special) RETURN n");
	ASSERT_TRUE(metrix_result_next(res));

	const char *json = metrix_result_get_props_json(res, 0);
	std::string jsonStr(json);

	// After Cypher parsing, the string contains actual newlines/tabs
	// When serialized to JSON, these become escape sequences
	EXPECT_NE(jsonStr.find("\\n"), std::string::npos);
	EXPECT_NE(jsonStr.find("\\t"), std::string::npos);
	EXPECT_NE(jsonStr.find("\\\""), std::string::npos);
	EXPECT_NE(jsonStr.find("\\\\"), std::string::npos);

	metrix_result_close(res);
}

TEST_F(CApiTest, ResultIsSuccess) {
	auto *goodRes = metrix_execute(db, "RETURN 1");
	ASSERT_NE(goodRes, nullptr);
	EXPECT_TRUE(metrix_result_is_success(goodRes));
	metrix_result_close(goodRes);
}

TEST_F(CApiTest, ResultGetError) {
	// Test with valid result (no error)
	auto *goodRes = metrix_execute(db, "RETURN 1");
	ASSERT_NE(goodRes, nullptr);
	const char *err = metrix_result_get_error(goodRes);
	// Should be empty for successful result
	EXPECT_STREQ(err, "");
	metrix_result_close(goodRes);
}

TEST_F(CApiTest, GetPropsJsonForNonEntity) {
	// Test get_props_json with a non-entity type (should return {})
	auto *res = metrix_execute(db, "RETURN 42 AS num");
	ASSERT_TRUE(metrix_result_next(res));

	const char *json = metrix_result_get_props_json(res, 0);
	EXPECT_STREQ(json, "{}");

	metrix_result_close(res);
}

TEST_F(CApiTest, OpenIfExistsWithNonExistentPath) {
	// Test opening a non-existent database
	std::string nonExistentPath = "/tmp/non_existent_db_12345678";

	// Ensure path doesn't exist
	if (fs::exists(nonExistentPath))
		fs::remove_all(nonExistentPath);

	MetrixDB_T *db2 = metrix_open_if_exists(nonExistentPath.c_str());
	EXPECT_EQ(db2, nullptr);

	// Verify error message is set
	const char *err = metrix_get_last_error();
	EXPECT_NE(err, nullptr);
	EXPECT_STRNE(err, "");
}

TEST_F(CApiTest, CloseNullDatabase) {
	// Test closing a nullptr database - should not crash
	metrix_close(nullptr);
	// If we get here without crash, test passes
	SUCCEED();
}

TEST_F(CApiTest, CloseNullResult) {
	// Test closing a nullptr result - should not crash
	metrix_result_close(nullptr);
	// If we get here without crash, test passes
	SUCCEED();
}

TEST_F(CApiTest, GetColumnCountWithNullResult) {
	int count = metrix_result_column_count(nullptr);
	EXPECT_EQ(count, 0);
}

TEST_F(CApiTest, GetColumnNameWithNullResult) {
	const char *name = metrix_result_column_name(nullptr, 0);
	EXPECT_STREQ(name, "");
}

TEST_F(CApiTest, JSONControlCharacterEscape) {
	// Test that JSON escaping works for various special characters
	// Using Cypher string escape syntax
	metrix_result_close(metrix_execute(db, "CREATE (n:Ctrl {text: 'Quote:\\\" Slash:\\\\ Backspace:\\b'})"));

	auto *res = metrix_execute(db, "MATCH (n:Ctrl) RETURN n");
	ASSERT_TRUE(metrix_result_next(res));

	const char *json = metrix_result_get_props_json(res, 0);
	std::string jsonStr(json);

	// Verify escape sequences are in JSON output
	EXPECT_NE(jsonStr.find("\\\""), std::string::npos);
	EXPECT_NE(jsonStr.find("\\\\"), std::string::npos);
	EXPECT_NE(jsonStr.find("\\b"), std::string::npos);

	metrix_result_close(res);
}

// ============================================================================
// Additional tests for improved coverage
// ============================================================================

TEST_F(CApiTest, JSONMultiPropertyEscape) {
	// Test JSON escaping with multiple properties
	metrix_result_close(metrix_execute(db,
		"CREATE (n:Multi {prop1: 'Line1\\nLine2', prop2: 'Tab\\there', prop3: 'Quote\\''})"));

	auto *res = metrix_execute(db, "MATCH (n:Multi) RETURN n");
	ASSERT_TRUE(metrix_result_next(res));

	const char *json = metrix_result_get_props_json(res, 0);
	std::string jsonStr(json);

	// Verify multiple properties with escape sequences
	EXPECT_NE(jsonStr.find("\\n"), std::string::npos);
	EXPECT_NE(jsonStr.find("\\t"), std::string::npos);
	EXPECT_NE(jsonStr.find("\\'"), std::string::npos);

	metrix_result_close(res);
}

TEST_F(CApiTest, NodeWithComplexProperties) {
	// Test node with various property types for JSON serialization
	metrix_result_close(metrix_execute(db,
		"CREATE (n:Complex {str: 'test', num: 42, bool: true, nullProp: null})"));

	auto *res = metrix_execute(db, "MATCH (n:Complex) RETURN n");
	ASSERT_TRUE(metrix_result_next(res));

	const char *json = metrix_result_get_props_json(res, 0);
	std::string jsonStr(json);

	// Verify different types are serialized
	EXPECT_NE(jsonStr.find("str"), std::string::npos);
	EXPECT_NE(jsonStr.find("test"), std::string::npos);
	EXPECT_NE(jsonStr.find("42"), std::string::npos);
	EXPECT_NE(jsonStr.find("true"), std::string::npos);

	metrix_result_close(res);
}

TEST_F(CApiTest, EdgePropertiesJsonSerialization) {
	metrix_result_close(metrix_execute(db, "CREATE (a:A), (b:B)"));
	metrix_result_close(metrix_execute(db,
		"MATCH (a:A), (b:B) CREATE (a)-[e:LINK {weight: 10.5, active: true}]->(b)"));

	auto *res = metrix_execute(db, "MATCH ()-[e:LINK]->() RETURN e");
	ASSERT_TRUE(metrix_result_next(res));

	const char *json = metrix_result_get_props_json(res, 0);
	std::string jsonStr(json);

	// Verify edge properties are serialized
	EXPECT_NE(jsonStr.find("weight"), std::string::npos);
	EXPECT_NE(jsonStr.find("10.5"), std::string::npos);
	EXPECT_NE(jsonStr.find("active"), std::string::npos);
	EXPECT_NE(jsonStr.find("true"), std::string::npos);

	metrix_result_close(res);
}

TEST_F(CApiTest, ResultIsSuccessWithNull) {
	// Test is_success with null result
	bool success = metrix_result_is_success(nullptr);
	EXPECT_FALSE(success);
}

TEST_F(CApiTest, GetErrorWithSuccessResult) {
	// Test get_error with a successful query
	auto *res = metrix_execute(db, "RETURN 1");
	ASSERT_NE(res, nullptr);

	const char *err = metrix_result_get_error(res);
	// Successful result should have empty error message
	EXPECT_STREQ(err, "");

	metrix_result_close(res);
}

TEST_F(CApiTest, GetErrorWithNullResult) {
	// Test get_error with null result - already covered but let's be explicit
	const char *err = metrix_result_get_error(nullptr);
	EXPECT_STREQ(err, "Invalid Result Handle");
}

TEST_F(CApiTest, EmptyNodePropertiesJson) {
	// Test JSON serialization of node with no properties
	metrix_result_close(metrix_execute(db, "CREATE (n:Empty)"));

	auto *res = metrix_execute(db, "MATCH (n:Empty) RETURN n");
	ASSERT_TRUE(metrix_result_next(res));

	const char *json = metrix_result_get_props_json(res, 0);
	EXPECT_STREQ(json, "{}");

	metrix_result_close(res);
}

TEST_F(CApiTest, EmptyEdgePropertiesJson) {
	metrix_result_close(metrix_execute(db, "CREATE (a:A), (b:B)"));
	metrix_result_close(metrix_execute(db, "MATCH (a:A), (b:B) CREATE (a)-[e:LINK]->(b)"));

	auto *res = metrix_execute(db, "MATCH ()-[e:LINK]->() RETURN e");
	ASSERT_TRUE(metrix_result_next(res));

	const char *json = metrix_result_get_props_json(res, 0);
	EXPECT_STREQ(json, "{}");

	metrix_result_close(res);
}

TEST_F(CApiTest, GetStringFromNonString) {
	// Test getting string from non-string column
	auto *res = metrix_execute(db, "RETURN 42 AS num");
	ASSERT_TRUE(metrix_result_next(res));

	const char *str = metrix_result_get_string(res, 0);
	EXPECT_STREQ(str, "");

	metrix_result_close(res);
}

TEST_F(CApiTest, GetIntFromNonInt) {
	// Test getting int from non-int column
	auto *res = metrix_execute(db, "RETURN 'hello' AS str");
	ASSERT_TRUE(metrix_result_next(res));

	int64_t val = metrix_result_get_int(res, 0);
	EXPECT_EQ(val, 0);

	metrix_result_close(res);
}

TEST_F(CApiTest, GetDoubleFromNonDouble) {
	// Test getting double from non-double column
	auto *res = metrix_execute(db, "RETURN 'test' AS str");
	ASSERT_TRUE(metrix_result_next(res));

	double val = metrix_result_get_double(res, 0);
	EXPECT_EQ(val, 0.0);

	metrix_result_close(res);
}

TEST_F(CApiTest, GetBoolFromNonBool) {
	// Test getting bool from non-bool column
	auto *res = metrix_execute(db, "RETURN 'test' AS str");
	ASSERT_TRUE(metrix_result_next(res));

	bool val = metrix_result_get_bool(res, 0);
	EXPECT_FALSE(val);

	metrix_result_close(res);
}

TEST_F(CApiTest, GetNodeFromNonNode) {
	// Test getting node from non-node column
	auto *res = metrix_execute(db, "RETURN 42 AS num");
	ASSERT_TRUE(metrix_result_next(res));

	MetrixNode node;
	bool success = metrix_result_get_node(res, 0, &node);
	EXPECT_FALSE(success);

	metrix_result_close(res);
}

TEST_F(CApiTest, GetEdgeFromNonEdge) {
	// Test getting edge from non-edge column
	auto *res = metrix_execute(db, "RETURN 42 AS num");
	ASSERT_TRUE(metrix_result_next(res));

	MetrixEdge edge;
	bool success = metrix_result_get_edge(res, 0, &edge);
	EXPECT_FALSE(success);

	metrix_result_close(res);
}

TEST_F(CApiTest, OpenWithEmptyPath) {
	// Test opening with empty path
	MetrixDB_T *db2 = metrix_open("");
	EXPECT_EQ(db2, nullptr);
}

TEST_F(CApiTest, ResultIsSuccessValidatesImpl) {
	// Test is_success validates both res pointer and internal state
	auto *res = metrix_execute(db, "RETURN 1");
	ASSERT_NE(res, nullptr);

	// Result should be successful
	EXPECT_TRUE(metrix_result_is_success(res));

	metrix_result_close(res);
}
