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
#include "zyx/zyx_c_api.h"

namespace fs = std::filesystem;

class CApiTest : public ::testing::Test {
protected:
	std::string dbPath;
	ZYXDB_T *db = nullptr;

	void SetUp() override {
		// Create a unique temporary path for the database
		auto tempDir = fs::temp_directory_path();
		dbPath = (tempDir / ("c_api_test_" + std::to_string(std::rand()))).string();
		if (fs::exists(dbPath))
			fs::remove_all(dbPath);
		// Standard open creates the DB
		db = zyx_open(dbPath.c_str());
	}

	void TearDown() override {
		if (db)
			zyx_close(db);
		std::error_code ec;
		if (fs::exists(dbPath))
			fs::remove_all(dbPath, ec);
	}
};

TEST_F(CApiTest, ExecuteAndIterate) {
	zyx_result_close(zyx_execute(db, "CREATE (n:CNode {id: 1, name: 'C-Lang', pi: 3.14, stable: true})"));

	ZYXResult_T *res = zyx_execute(db, "MATCH (n:CNode) RETURN n.id, n.name, n.pi, n.stable");
	ASSERT_NE(res, nullptr);
	ASSERT_TRUE(zyx_result_next(res));

	// Map columns dynamically to be robust against sorting/parser changes
	int idxId = -1, idxName = -1, idxPi = -1, idxBool = -1;
	int cols = zyx_result_column_count(res);
	for (int i = 0; i < cols; i++) {
		std::string name = zyx_result_column_name(res, i);
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
	EXPECT_EQ(zyx_result_get_int(res, idxId), 1);
	EXPECT_STREQ(zyx_result_get_string(res, idxName), "C-Lang");
	EXPECT_DOUBLE_EQ(zyx_result_get_double(res, idxPi), 3.14);
	EXPECT_EQ(zyx_result_get_bool(res, idxBool), true);

	zyx_result_close(res);
}

TEST_F(CApiTest, NodeObjectAccess) {
	zyx_result_close(zyx_execute(db, "CREATE (n:Obj {prop: 'val'})"));

	auto *res = zyx_execute(db, "MATCH (n:Obj) RETURN n");
	ASSERT_TRUE(zyx_result_next(res));

	EXPECT_EQ(zyx_result_get_type(res, 0), ZYX_NODE);

	ZYXNode node_struct;
	bool success = zyx_result_get_node(res, 0, &node_struct);
	EXPECT_TRUE(success);
	EXPECT_STREQ(node_struct.label, "Obj");

	const char *json = zyx_result_get_props_json(res, 0);
	std::string jsonStr(json);
	EXPECT_NE(jsonStr.find("prop"), std::string::npos);

	zyx_result_close(res);
}

TEST_F(CApiTest, EdgeObjectAccess) {
	zyx_result_close(zyx_execute(db, "CREATE (a:N {id:1})"));
	zyx_result_close(zyx_execute(db, "CREATE (b:N {id:2})"));

	// Match and Create edge
	zyx_result_close(zyx_execute(db, "MATCH (a:N {id:1}), (b:N {id:2}) CREATE (a)-[e:REL {w:10}]->(b)"));

	auto *res = zyx_execute(db, "MATCH ()-[e:REL]->() RETURN e");
	ASSERT_TRUE(zyx_result_next(res));

	EXPECT_EQ(zyx_result_get_type(res, 0), ZYX_EDGE);

	ZYXEdge edge_struct;
	bool success = zyx_result_get_edge(res, 0, &edge_struct);
	ASSERT_TRUE(success);
	EXPECT_STREQ(edge_struct.type, "REL");

	zyx_result_close(res);
}

TEST_F(CApiTest, ErrorHandling) {
	ZYXDB_T *bad_db = zyx_open(nullptr);
	EXPECT_EQ(bad_db, nullptr);
	ZYXResult_T *res = zyx_execute(nullptr, "MATCH (n) RETURN n");
	EXPECT_EQ(res, nullptr);
}

TEST_F(CApiTest, EmptyResult) {
	auto *res = zyx_execute(db, "MATCH (n:NonExistent) RETURN n");
	EXPECT_FALSE(zyx_result_next(res));
	zyx_result_close(res);
}

TEST_F(CApiTest, OutOfBoundsAccess) {
	zyx_result_close(zyx_execute(db, "CREATE (n:OneCol)"));
	auto *res = zyx_execute(db, "MATCH (n:OneCol) RETURN n");
	zyx_result_next(res);

	// Index 0 valid
	EXPECT_EQ(zyx_result_get_type(res, 0), ZYX_NODE);
	// Index 1 invalid
	EXPECT_EQ(zyx_result_get_type(res, 1), ZYX_NULL);

	zyx_result_close(res);
}

TEST_F(CApiTest, EdgeObjectAccess_Rigorous) {
	// 1. Setup
	zyx_result_close(zyx_execute(db, "CREATE (a:N {id:1})"));
	zyx_result_close(zyx_execute(db, "CREATE (b:N {id:2})"));

	// 2. Action: Match and Create
	// Using a complex query to ensure planner works correctly
	const char *createQuery = "MATCH (a:N {id:1}), (b:N {id:2}) CREATE (a)-[e:REL {w:10}]->(b)";
	ZYXResult_T *createRes = zyx_execute(db, createQuery);
	ASSERT_NE(createRes, nullptr);
	zyx_result_close(createRes);

	// 3. Verification
	auto *res = zyx_execute(db, "MATCH ()-[e:REL]->() RETURN e");
	ASSERT_NE(res, nullptr);

	// Rigorous Check 1: Existence
	ASSERT_TRUE(zyx_result_next(res)) << "No edge returned.";

	// Rigorous Check 2: Metadata
	EXPECT_EQ(zyx_result_get_type(res, 0), ZYX_EDGE);

	// Rigorous Check 3: Topology and Props
	ZYXEdge edge_struct;
	bool success = zyx_result_get_edge(res, 0, &edge_struct);
	ASSERT_TRUE(success);
	EXPECT_GT(edge_struct.id, 0);
	EXPECT_GT(edge_struct.source_id, 0);
	EXPECT_GT(edge_struct.target_id, 0);
	EXPECT_STREQ(edge_struct.type, "REL");

	// Check Property JSON string
	const char *jsonProps = zyx_result_get_props_json(res, 0);
	std::string jsonStr(jsonProps);
	EXPECT_NE(jsonStr.find("\"w\":10"), std::string::npos) << "Property w:10 missing in JSON";

	// Rigorous Check 4: Cardinality (Should be exactly 1)
	ASSERT_FALSE(zyx_result_next(res)) << "Expected exactly 1 edge, found more.";

	zyx_result_close(res);
}

// ============================================================================
// NEW TESTS FOR FULL COVERAGE
// ============================================================================

TEST_F(CApiTest, OpenIfExists) {
	// 1. Close the DB opened by SetUp to release the lock/handle
	zyx_close(db);
	db = nullptr;

	// 2. Test opening existing DB (path was created in SetUp)
	db = zyx_open_if_exists(dbPath.c_str());
	ASSERT_NE(db, nullptr) << "Failed to open existing DB with zyx_open_if_exists";
	zyx_close(db);
	db = nullptr;

	// 3. Test opening non-existent DB
	std::string badPath = dbPath + "_non_existent";
	if (fs::exists(badPath))
		fs::remove_all(badPath);

	db = zyx_open_if_exists(badPath.c_str());
	EXPECT_EQ(db, nullptr) << "zyx_open_if_exists should return null for missing DB";
}

TEST_F(CApiTest, NullHandleSafeguards) {
	// Test that passing nullptr to result accessors handles it gracefully
	ZYXResult_T *nullRes = nullptr;

	EXPECT_FALSE(zyx_result_next(nullRes));
	EXPECT_EQ(zyx_result_column_count(nullRes), 0);
	EXPECT_STREQ(zyx_result_column_name(nullRes, 0), "");

	EXPECT_EQ(zyx_result_get_type(nullRes, 0), ZYX_NULL);
	EXPECT_EQ(zyx_result_get_int(nullRes, 0), 0);
	EXPECT_DOUBLE_EQ(zyx_result_get_double(nullRes, 0), 0.0);
	EXPECT_FALSE(zyx_result_get_bool(nullRes, 0));
	EXPECT_STREQ(zyx_result_get_string(nullRes, 0), "");

	// Test safeguards for complex types
	ZYXNode node;
	EXPECT_FALSE(zyx_result_get_node(nullRes, 0, &node));

	ZYXEdge edge;
	EXPECT_FALSE(zyx_result_get_edge(nullRes, 0, &edge));

	EXPECT_STREQ(zyx_result_get_props_json(nullRes, 0), "{}");
	EXPECT_STREQ(zyx_result_get_error(nullRes), "Invalid Result Handle");
}

TEST_F(CApiTest, TypeMismatchSafeguards) {
	// Create a node with a property
	zyx_result_close(zyx_execute(db, "CREATE (n:Safe {val: 42})"));
	auto *res = zyx_execute(db, "MATCH (n:Safe) RETURN n.val");
	ASSERT_TRUE(zyx_result_next(res));

	// Valid access: Integer
	EXPECT_EQ(zyx_result_get_type(res, 0), ZYX_INT);
	EXPECT_EQ(zyx_result_get_int(res, 0), 42);

	// Invalid access: Trying to interpret Integer as Node
	ZYXNode node;
	EXPECT_FALSE(zyx_result_get_node(res, 0, &node)) << "Should not read Integer as Node";

	// Invalid access: Trying to interpret Integer as Edge
	ZYXEdge edge;
	EXPECT_FALSE(zyx_result_get_edge(res, 0, &edge)) << "Should not read Integer as Edge";

	zyx_result_close(res);
}

TEST_F(CApiTest, ResultStatusAndGlobalError) {
	// 1. Success Case
	auto *goodRes = zyx_execute(db, "RETURN 1");
	ASSERT_NE(goodRes, nullptr);
	EXPECT_TRUE(zyx_result_is_success(goodRes));
	zyx_result_close(goodRes);

	// 2. Failure Case (Triggered via exception in zyx_execute -> returns null)
	// Assuming the DB throws on invalid syntax
	auto *badRes = zyx_execute(db, "THIS IS INVALID CYPHER");
	if (badRes == nullptr) {
		// Check global error
		const char *err = zyx_get_last_error();
		EXPECT_NE(err, nullptr);
		// We expect a non-empty error message, though exact content depends on parser
		EXPECT_STRNE(err, "");
	} else {
		// Implementation might return a result object representing failure
		EXPECT_FALSE(zyx_result_is_success(badRes));
		const char *err = zyx_result_get_error(badRes);
		EXPECT_NE(err, nullptr);
		EXPECT_STRNE(err, "");
		zyx_result_close(badRes);
	}
}

TEST_F(CApiTest, UninitializedResultAccess) {
	// Manually create a result handle with internal null state
	// (Simulating a critical failure in allocation or logic)
	// Note: Since ZYXResult_T's constructor takes a value, we can't easily fake a null internal state
	// without hacking.
	// Instead, we verify that closing a nullptr is safe.
	zyx_result_close(nullptr);

	// Verify zyx_get_last_error is safe to call anytime
	const char *err = zyx_get_last_error();
	// It might be empty or hold previous error, just ensure no crash
	(void) err;
}

// Verify Large String Handling (Buffer Check)
TEST_F(CApiTest, LargeStringPayload) {
	std::string large(10000, 'x'); // 10KB string
	std::string query = "RETURN '" + large + "' AS s";

	auto *res = zyx_execute(db, query.c_str());
	ASSERT_TRUE(zyx_result_next(res));

	const char *val = zyx_result_get_string(res, 0);
	EXPECT_EQ(std::strlen(val), 10000UL); // Using UL to suppress sign-compare warning
	EXPECT_EQ(val[0], 'x');
	EXPECT_EQ(val[9999], 'x');

	zyx_result_close(res);
}

// ============================================================================
// Additional tests for improved coverage
// ============================================================================

TEST_F(CApiTest, GetDuration) {
	auto *res = zyx_execute(db, "RETURN 1 AS x");
	ASSERT_NE(res, nullptr);
	ASSERT_TRUE(zyx_result_next(res));

	double duration = zyx_result_get_duration(res);
	EXPECT_GE(duration, 0.0);

	zyx_result_close(res);
}

TEST_F(CApiTest, GetDurationWithNullResult) {
	double duration = zyx_result_get_duration(nullptr);
	EXPECT_EQ(duration, 0.0);
}

TEST_F(CApiTest, JSONEscapeSequences) {
	// Use Cypher's escape syntax for newlines and quotes
	// Cypher uses backslash escaping within strings
	zyx_result_close(zyx_execute(db, "CREATE (n:Special {text: 'Line1\\nLine2\\tTab\"Quote\\\\Slash'})"));

	auto *res = zyx_execute(db, "MATCH (n:Special) RETURN n");
	ASSERT_TRUE(zyx_result_next(res));

	const char *json = zyx_result_get_props_json(res, 0);
	std::string jsonStr(json);

	// After Cypher parsing, the string contains actual newlines/tabs
	// When serialized to JSON, these become escape sequences
	EXPECT_NE(jsonStr.find("\\n"), std::string::npos);
	EXPECT_NE(jsonStr.find("\\t"), std::string::npos);
	EXPECT_NE(jsonStr.find("\\\""), std::string::npos);
	EXPECT_NE(jsonStr.find("\\\\"), std::string::npos);

	zyx_result_close(res);
}

TEST_F(CApiTest, ResultIsSuccess) {
	auto *goodRes = zyx_execute(db, "RETURN 1");
	ASSERT_NE(goodRes, nullptr);
	EXPECT_TRUE(zyx_result_is_success(goodRes));
	zyx_result_close(goodRes);
}

TEST_F(CApiTest, ResultGetError) {
	// Test with valid result (no error)
	auto *goodRes = zyx_execute(db, "RETURN 1");
	ASSERT_NE(goodRes, nullptr);
	const char *err = zyx_result_get_error(goodRes);
	// Should be empty for successful result
	EXPECT_STREQ(err, "");
	zyx_result_close(goodRes);
}

TEST_F(CApiTest, GetPropsJsonForNonEntity) {
	// Test get_props_json with a non-entity type (should return {})
	auto *res = zyx_execute(db, "RETURN 42 AS num");
	ASSERT_TRUE(zyx_result_next(res));

	const char *json = zyx_result_get_props_json(res, 0);
	EXPECT_STREQ(json, "{}");

	zyx_result_close(res);
}

TEST_F(CApiTest, OpenIfExistsWithNonExistentPath) {
	// Test opening a non-existent database
	std::string nonExistentPath = "/tmp/non_existent_db_12345678";

	// Ensure path doesn't exist
	if (fs::exists(nonExistentPath))
		fs::remove_all(nonExistentPath);

	ZYXDB_T *db2 = zyx_open_if_exists(nonExistentPath.c_str());
	EXPECT_EQ(db2, nullptr);

	// Verify error message is set
	const char *err = zyx_get_last_error();
	EXPECT_NE(err, nullptr);
	EXPECT_STRNE(err, "");
}

TEST_F(CApiTest, CloseNullDatabase) {
	// Test closing a nullptr database - should not crash
	zyx_close(nullptr);
	// If we get here without crash, test passes
	SUCCEED();
}

TEST_F(CApiTest, CloseNullResult) {
	// Test closing a nullptr result - should not crash
	zyx_result_close(nullptr);
	// If we get here without crash, test passes
	SUCCEED();
}

TEST_F(CApiTest, GetColumnCountWithNullResult) {
	int count = zyx_result_column_count(nullptr);
	EXPECT_EQ(count, 0);
}

TEST_F(CApiTest, GetColumnNameWithNullResult) {
	const char *name = zyx_result_column_name(nullptr, 0);
	EXPECT_STREQ(name, "");
}

TEST_F(CApiTest, JSONControlCharacterEscape) {
	// Test that JSON escaping works for various special characters
	// Using Cypher string escape syntax
	zyx_result_close(zyx_execute(db, "CREATE (n:Ctrl {text: 'Quote:\\\" Slash:\\\\ Backspace:\\b'})"));

	auto *res = zyx_execute(db, "MATCH (n:Ctrl) RETURN n");
	ASSERT_TRUE(zyx_result_next(res));

	const char *json = zyx_result_get_props_json(res, 0);
	std::string jsonStr(json);

	// Verify escape sequences are in JSON output
	EXPECT_NE(jsonStr.find("\\\""), std::string::npos);
	EXPECT_NE(jsonStr.find("\\\\"), std::string::npos);
	EXPECT_NE(jsonStr.find("\\b"), std::string::npos);

	zyx_result_close(res);
}

// ============================================================================
// Additional tests for improved coverage
// ============================================================================

TEST_F(CApiTest, JSONMultiPropertyEscape) {
	// Test JSON escaping with multiple properties
	zyx_result_close(zyx_execute(db,
		"CREATE (n:Multi {prop1: 'Line1\\nLine2', prop2: 'Tab\\there', prop3: 'Quote\\''})"));

	auto *res = zyx_execute(db, "MATCH (n:Multi) RETURN n");
	ASSERT_TRUE(zyx_result_next(res));

	const char *json = zyx_result_get_props_json(res, 0);
	std::string jsonStr(json);

	// Verify multiple properties with escape sequences
	// After parser unescaping, \n and \t become real control chars,
	// which escape_json_string then re-escapes for JSON output
	EXPECT_NE(jsonStr.find("\\n"), std::string::npos);
	EXPECT_NE(jsonStr.find("\\t"), std::string::npos);
	// Single quote is stored as literal ' after unescaping, no JSON escaping needed
	EXPECT_NE(jsonStr.find("'"), std::string::npos);

	zyx_result_close(res);
}

TEST_F(CApiTest, NodeWithComplexProperties) {
	// Test node with various property types for JSON serialization
	zyx_result_close(zyx_execute(db,
		"CREATE (n:Complex {str: 'test', num: 42, bool: true, nullProp: null})"));

	auto *res = zyx_execute(db, "MATCH (n:Complex) RETURN n");
	ASSERT_TRUE(zyx_result_next(res));

	const char *json = zyx_result_get_props_json(res, 0);
	std::string jsonStr(json);

	// Verify different types are serialized
	EXPECT_NE(jsonStr.find("str"), std::string::npos);
	EXPECT_NE(jsonStr.find("test"), std::string::npos);
	EXPECT_NE(jsonStr.find("42"), std::string::npos);
	EXPECT_NE(jsonStr.find("true"), std::string::npos);

	zyx_result_close(res);
}

TEST_F(CApiTest, EdgePropertiesJsonSerialization) {
	zyx_result_close(zyx_execute(db, "CREATE (a:A), (b:B)"));
	zyx_result_close(zyx_execute(db,
		"MATCH (a:A), (b:B) CREATE (a)-[e:LINK {weight: 10.5, active: true}]->(b)"));

	auto *res = zyx_execute(db, "MATCH ()-[e:LINK]->() RETURN e");
	ASSERT_TRUE(zyx_result_next(res));

	const char *json = zyx_result_get_props_json(res, 0);
	std::string jsonStr(json);

	// Verify edge properties are serialized
	EXPECT_NE(jsonStr.find("weight"), std::string::npos);
	EXPECT_NE(jsonStr.find("10.5"), std::string::npos);
	EXPECT_NE(jsonStr.find("active"), std::string::npos);
	EXPECT_NE(jsonStr.find("true"), std::string::npos);

	zyx_result_close(res);
}

TEST_F(CApiTest, ResultIsSuccessWithNull) {
	// Test is_success with null result
	bool success = zyx_result_is_success(nullptr);
	EXPECT_FALSE(success);
}

TEST_F(CApiTest, GetErrorWithSuccessResult) {
	// Test get_error with a successful query
	auto *res = zyx_execute(db, "RETURN 1");
	ASSERT_NE(res, nullptr);

	const char *err = zyx_result_get_error(res);
	// Successful result should have empty error message
	EXPECT_STREQ(err, "");

	zyx_result_close(res);
}

TEST_F(CApiTest, GetErrorWithNullResult) {
	// Test get_error with null result - already covered but let's be explicit
	const char *err = zyx_result_get_error(nullptr);
	EXPECT_STREQ(err, "Invalid Result Handle");
}

TEST_F(CApiTest, EmptyNodePropertiesJson) {
	// Test JSON serialization of node with no properties
	zyx_result_close(zyx_execute(db, "CREATE (n:Empty)"));

	auto *res = zyx_execute(db, "MATCH (n:Empty) RETURN n");
	ASSERT_TRUE(zyx_result_next(res));

	const char *json = zyx_result_get_props_json(res, 0);
	EXPECT_STREQ(json, "{}");

	zyx_result_close(res);
}

TEST_F(CApiTest, EmptyEdgePropertiesJson) {
	zyx_result_close(zyx_execute(db, "CREATE (a:A), (b:B)"));
	zyx_result_close(zyx_execute(db, "MATCH (a:A), (b:B) CREATE (a)-[e:LINK]->(b)"));

	auto *res = zyx_execute(db, "MATCH ()-[e:LINK]->() RETURN e");
	ASSERT_TRUE(zyx_result_next(res));

	const char *json = zyx_result_get_props_json(res, 0);
	EXPECT_STREQ(json, "{}");

	zyx_result_close(res);
}

TEST_F(CApiTest, GetStringFromNonString) {
	// Test getting string from non-string column
	auto *res = zyx_execute(db, "RETURN 42 AS num");
	ASSERT_TRUE(zyx_result_next(res));

	const char *str = zyx_result_get_string(res, 0);
	EXPECT_STREQ(str, "");

	zyx_result_close(res);
}

TEST_F(CApiTest, GetIntFromNonInt) {
	// Test getting int from non-int column
	auto *res = zyx_execute(db, "RETURN 'hello' AS str");
	ASSERT_TRUE(zyx_result_next(res));

	int64_t val = zyx_result_get_int(res, 0);
	EXPECT_EQ(val, 0);

	zyx_result_close(res);
}

TEST_F(CApiTest, GetDoubleFromNonDouble) {
	// Test getting double from non-double column
	auto *res = zyx_execute(db, "RETURN 'test' AS str");
	ASSERT_TRUE(zyx_result_next(res));

	double val = zyx_result_get_double(res, 0);
	EXPECT_EQ(val, 0.0);

	zyx_result_close(res);
}

TEST_F(CApiTest, GetBoolFromNonBool) {
	// Test getting bool from non-bool column
	auto *res = zyx_execute(db, "RETURN 'test' AS str");
	ASSERT_TRUE(zyx_result_next(res));

	bool val = zyx_result_get_bool(res, 0);
	EXPECT_FALSE(val);

	zyx_result_close(res);
}

TEST_F(CApiTest, GetNodeFromNonNode) {
	// Test getting node from non-node column
	auto *res = zyx_execute(db, "RETURN 42 AS num");
	ASSERT_TRUE(zyx_result_next(res));

	ZYXNode node;
	bool success = zyx_result_get_node(res, 0, &node);
	EXPECT_FALSE(success);

	zyx_result_close(res);
}

TEST_F(CApiTest, GetEdgeFromNonEdge) {
	// Test getting edge from non-edge column
	auto *res = zyx_execute(db, "RETURN 42 AS num");
	ASSERT_TRUE(zyx_result_next(res));

	ZYXEdge edge;
	bool success = zyx_result_get_edge(res, 0, &edge);
	EXPECT_FALSE(success);

	zyx_result_close(res);
}

TEST_F(CApiTest, OpenWithEmptyPath) {
	// Test opening with empty path
	ZYXDB_T *db2 = zyx_open("");
	EXPECT_EQ(db2, nullptr);
}

TEST_F(CApiTest, ResultIsSuccessValidatesImpl) {
	// Test is_success validates both res pointer and internal state
	auto *res = zyx_execute(db, "RETURN 1");
	ASSERT_NE(res, nullptr);

	// Result should be successful
	EXPECT_TRUE(zyx_result_is_success(res));

	zyx_result_close(res);
}

TEST_F(CApiTest, OpenIfExistsWithNullPath) {
	ZYXDB_T *db2 = zyx_open_if_exists(nullptr);
	EXPECT_EQ(db2, nullptr);
	const char *err = zyx_get_last_error();
	EXPECT_NE(err, nullptr);
	EXPECT_STRNE(err, "");
}

TEST_F(CApiTest, ExecuteWithNullCypher) {
	ZYXResult_T *res = zyx_execute(db, nullptr);
	EXPECT_EQ(res, nullptr);
}

TEST_F(CApiTest, ExecuteWithNullDb) {
	ZYXResult_T *res = zyx_execute(nullptr, "RETURN 1");
	EXPECT_EQ(res, nullptr);
}

TEST_F(CApiTest, JSONFormFeedAndCarriageReturn) {
	// Test escape_json_string for \f and \r chars
	zyx_result_close(zyx_execute(db, "CREATE (n:Esc {text: 'abc\\fdef\\rghi'})"));
	auto *res = zyx_execute(db, "MATCH (n:Esc) RETURN n");
	ASSERT_TRUE(zyx_result_next(res));
	const char *json = zyx_result_get_props_json(res, 0);
	std::string jsonStr(json);
	EXPECT_NE(jsonStr.find("\\f"), std::string::npos);
	EXPECT_NE(jsonStr.find("\\r"), std::string::npos);
	zyx_result_close(res);
}

TEST_F(CApiTest, BooleanPropertyJsonSerialization) {
	zyx_result_close(zyx_execute(db, "CREATE (n:BoolNode {active: true, deleted: false})"));
	auto *res = zyx_execute(db, "MATCH (n:BoolNode) RETURN n");
	ASSERT_TRUE(zyx_result_next(res));
	const char *json = zyx_result_get_props_json(res, 0);
	std::string jsonStr(json);
	EXPECT_NE(jsonStr.find("true"), std::string::npos);
	EXPECT_NE(jsonStr.find("false"), std::string::npos);
	zyx_result_close(res);
}

TEST_F(CApiTest, DoublePropertyJsonSerialization) {
	zyx_result_close(zyx_execute(db, "CREATE (n:DblNode {pi: 3.14})"));
	auto *res = zyx_execute(db, "MATCH (n:DblNode) RETURN n");
	ASSERT_TRUE(zyx_result_next(res));
	const char *json = zyx_result_get_props_json(res, 0);
	std::string jsonStr(json);
	EXPECT_NE(jsonStr.find("3.14"), std::string::npos);
	zyx_result_close(res);
}

TEST_F(CApiTest, NullPropertyJsonSerialization) {
	zyx_result_close(zyx_execute(db, "CREATE (n:NullNode {val: null})"));
	auto *res = zyx_execute(db, "MATCH (n:NullNode) RETURN n");
	ASSERT_TRUE(zyx_result_next(res));
	const char *json = zyx_result_get_props_json(res, 0);
	std::string jsonStr(json);
	// Null properties may not be serialized or serialize as "null"
	// Either way, it shouldn't crash
	EXPECT_FALSE(jsonStr.empty());
	zyx_result_close(res);
}

TEST_F(CApiTest, GetNodeWithNullOutParam) {
	zyx_result_close(zyx_execute(db, "CREATE (n:N1)"));
	auto *res = zyx_execute(db, "MATCH (n:N1) RETURN n");
	ASSERT_TRUE(zyx_result_next(res));
	// null out_node
	EXPECT_FALSE(zyx_result_get_node(res, 0, nullptr));
	zyx_result_close(res);
}

TEST_F(CApiTest, GetEdgeWithNullOutParam) {
	zyx_result_close(zyx_execute(db, "CREATE (a:E1), (b:E2)"));
	zyx_result_close(zyx_execute(db, "MATCH (a:E1), (b:E2) CREATE (a)-[e:EREL]->(b)"));
	auto *res = zyx_execute(db, "MATCH ()-[e:EREL]->() RETURN e");
	ASSERT_TRUE(zyx_result_next(res));
	// null out_edge
	EXPECT_FALSE(zyx_result_get_edge(res, 0, nullptr));
	zyx_result_close(res);
}

TEST_F(CApiTest, NodeWithBoolPropertyFalse) {
	// Test props_to_json with false bool property
	zyx_result_close(zyx_execute(db, "CREATE (n:BoolFalse {active: false})"));

	auto *res = zyx_execute(db, "MATCH (n:BoolFalse) RETURN n");
	ASSERT_TRUE(zyx_result_next(res));

	const char *json = zyx_result_get_props_json(res, 0);
	std::string jsonStr(json);

	EXPECT_NE(jsonStr.find("false"), std::string::npos);

	zyx_result_close(res);
}

TEST_F(CApiTest, NodeWithDoubleProperty) {
	// Test props_to_json with double property
	zyx_result_close(zyx_execute(db, "CREATE (n:DblNode {pi: 3.14})"));

	auto *res = zyx_execute(db, "MATCH (n:DblNode) RETURN n");
	ASSERT_TRUE(zyx_result_next(res));

	const char *json = zyx_result_get_props_json(res, 0);
	std::string jsonStr(json);

	EXPECT_NE(jsonStr.find("3.14"), std::string::npos);

	zyx_result_close(res);
}

TEST_F(CApiTest, NodeWithNullProperty) {
	// Test props_to_json with null property
	zyx_result_close(zyx_execute(db, "CREATE (n:NullNode {val: null})"));

	auto *res = zyx_execute(db, "MATCH (n:NullNode) RETURN n");
	ASSERT_TRUE(zyx_result_next(res));

	const char *json = zyx_result_get_props_json(res, 0);
	std::string jsonStr(json);

	// Null properties might not be stored, but if they are, check for "null"
	// The important thing is no crash
	EXPECT_NE(json, nullptr);

	zyx_result_close(res);
}

TEST_F(CApiTest, MultipleResultRows) {
	// Test iteration over multiple result rows
	zyx_result_close(zyx_execute(db, "CREATE (n:Row {x: 1})"));
	zyx_result_close(zyx_execute(db, "CREATE (n:Row {x: 2})"));
	zyx_result_close(zyx_execute(db, "CREATE (n:Row {x: 3})"));

	auto *res = zyx_execute(db, "MATCH (n:Row) RETURN n.x");
	ASSERT_NE(res, nullptr);

	int count = 0;
	while (zyx_result_next(res)) {
		count++;
	}
	EXPECT_EQ(count, 3);

	// After exhausting, next should return false
	EXPECT_FALSE(zyx_result_next(res));

	zyx_result_close(res);
}

// ============================================================================
// Tests targeting specific uncovered branches
// ============================================================================

TEST_F(CApiTest, GetTypeBool) {
	// Cover the unexecuted bool instantiation of zyx_result_get_type visitor
	auto *res = zyx_execute(db, "RETURN true AS b");
	ASSERT_NE(res, nullptr);
	ASSERT_TRUE(zyx_result_next(res));

	EXPECT_EQ(zyx_result_get_type(res, 0), ZYX_BOOL);

	zyx_result_close(res);
}

TEST_F(CApiTest, GetTypeDouble) {
	// Cover the unexecuted double instantiation of zyx_result_get_type visitor
	auto *res = zyx_execute(db, "RETURN 3.14 AS d");
	ASSERT_NE(res, nullptr);
	ASSERT_TRUE(zyx_result_next(res));

	EXPECT_EQ(zyx_result_get_type(res, 0), ZYX_DOUBLE);

	zyx_result_close(res);
}

TEST_F(CApiTest, GetTypeString) {
	// Cover the unexecuted string instantiation of zyx_result_get_type visitor
	auto *res = zyx_execute(db, "RETURN 'hello' AS s");
	ASSERT_NE(res, nullptr);
	ASSERT_TRUE(zyx_result_next(res));

	EXPECT_EQ(zyx_result_get_type(res, 0), ZYX_STRING);

	zyx_result_close(res);
}

TEST_F(CApiTest, JSONUnicodeControlCharEscape) {
	// Cover the control character branch in escape_json_string (c <= '\x1f')
	// Use Cypher \u0001 escape to store a control character in a property
	zyx_result_close(zyx_execute(db, "CREATE (n:CtrlChar {text: 'abc\\u0001def'})"));

	auto *res = zyx_execute(db, "MATCH (n:CtrlChar) RETURN n");
	ASSERT_NE(res, nullptr);
	ASSERT_TRUE(zyx_result_next(res));

	const char *json = zyx_result_get_props_json(res, 0);
	std::string jsonStr(json);

	// The control character \x01 should be escaped as \u0001 in JSON output
	EXPECT_NE(jsonStr.find("\\u0001"), std::string::npos)
		<< "Control character should be escaped as \\u0001 in JSON. Got: " << jsonStr;

	zyx_result_close(res);
}

// ============================================================================
// Tests targeting vector<string> variant and remaining uncovered branches
// ============================================================================

TEST_F(CApiTest, GetTypeForListResult) {
	// labels(n) returns a vector<string> via the public API
	zyx_result_close(zyx_execute(db, "CREATE (n:LblTest {x: 1})"));
	auto *res = zyx_execute(db, "MATCH (n:LblTest) RETURN labels(n) AS lbls");
	ASSERT_NE(res, nullptr);
	ASSERT_TRUE(zyx_result_next(res));

	// The vector<string> variant should map to ZYX_LIST
	ZYXValueType t = zyx_result_get_type(res, 0);
	EXPECT_EQ(t, ZYX_LIST);

	zyx_result_close(res);
}

TEST_F(CApiTest, GetPropsJsonForListResult) {
	// Exercise the unexecuted vector<string> instantiation in get_props_json visitor
	zyx_result_close(zyx_execute(db, "CREATE (n:LblTest2 {x: 1})"));
	auto *res = zyx_execute(db, "MATCH (n:LblTest2) RETURN labels(n) AS lbls");
	ASSERT_NE(res, nullptr);
	ASSERT_TRUE(zyx_result_next(res));

	const char *json = zyx_result_get_props_json(res, 0);
	// Non-entity types return "{}"
	EXPECT_STREQ(json, "{}");

	zyx_result_close(res);
}

TEST_F(CApiTest, GetPropsJsonForBoolResult) {
	// Exercise the unexecuted bool instantiation in get_props_json visitor
	auto *res = zyx_execute(db, "RETURN true AS b");
	ASSERT_NE(res, nullptr);
	ASSERT_TRUE(zyx_result_next(res));

	const char *json = zyx_result_get_props_json(res, 0);
	EXPECT_STREQ(json, "{}");

	zyx_result_close(res);
}

TEST_F(CApiTest, GetPropsJsonForDoubleResult) {
	// Exercise the unexecuted double instantiation in get_props_json visitor
	auto *res = zyx_execute(db, "RETURN 3.14 AS d");
	ASSERT_NE(res, nullptr);
	ASSERT_TRUE(zyx_result_next(res));

	const char *json = zyx_result_get_props_json(res, 0);
	EXPECT_STREQ(json, "{}");

	zyx_result_close(res);
}

TEST_F(CApiTest, GetPropsJsonForStringResult) {
	// Exercise the unexecuted string instantiation in get_props_json visitor
	auto *res = zyx_execute(db, "RETURN 'hello' AS s");
	ASSERT_NE(res, nullptr);
	ASSERT_TRUE(zyx_result_next(res));

	const char *json = zyx_result_get_props_json(res, 0);
	EXPECT_STREQ(json, "{}");

	zyx_result_close(res);
}

TEST_F(CApiTest, GetPropsJsonForMonostateResult) {
	// Exercise the unexecuted monostate instantiation in get_props_json visitor
	auto *res = zyx_execute(db, "RETURN null AS n");
	ASSERT_NE(res, nullptr);
	ASSERT_TRUE(zyx_result_next(res));

	const char *json = zyx_result_get_props_json(res, 0);
	EXPECT_STREQ(json, "{}");

	zyx_result_close(res);
}

TEST_F(CApiTest, KeysFunctionProducesListValue) {
	// keys(n) returns vector<string> - exercise vector<string> variant paths
	zyx_result_close(zyx_execute(db, "CREATE (n:KeyTest {a: 1, b: 'two'})"));
	auto *res = zyx_execute(db, "MATCH (n:KeyTest) RETURN keys(n) AS k");
	ASSERT_NE(res, nullptr);
	ASSERT_TRUE(zyx_result_next(res));

	// Type should be ZYX_LIST (list type now supported in C API)
	EXPECT_EQ(zyx_result_get_type(res, 0), ZYX_LIST);

	// get_int, get_double, get_bool, get_string should return defaults
	EXPECT_EQ(zyx_result_get_int(res, 0), 0);
	EXPECT_DOUBLE_EQ(zyx_result_get_double(res, 0), 0.0);
	EXPECT_FALSE(zyx_result_get_bool(res, 0));
	EXPECT_STREQ(zyx_result_get_string(res, 0), "");

	// get_node and get_edge should return false
	ZYXNode node;
	EXPECT_FALSE(zyx_result_get_node(res, 0, &node));
	ZYXEdge edge;
	EXPECT_FALSE(zyx_result_get_edge(res, 0, &edge));

	// get_props_json should return "{}"
	EXPECT_STREQ(zyx_result_get_props_json(res, 0), "{}");

	zyx_result_close(res);
}

TEST_F(CApiTest, GetTypeNull) {
	// Exercise the monostate instantiation of get_type visitor
	auto *res = zyx_execute(db, "RETURN null AS n");
	ASSERT_NE(res, nullptr);
	ASSERT_TRUE(zyx_result_next(res));

	EXPECT_EQ(zyx_result_get_type(res, 0), ZYX_NULL);

	zyx_result_close(res);
}

TEST_F(CApiTest, PropsToJsonWithListPropertyInNode) {
	// If a node has a property value that is a vector<string>,
	// it should hit the ComplexObject branch in props_to_json
	// This can happen if we store a list property through Cypher
	// Cypher doesn't support list properties directly in CREATE, so
	// this may not be reachable. Let's at least verify no crash.
	zyx_result_close(zyx_execute(db, "CREATE (n:ListNode {name: 'test'})"));
	auto *res = zyx_execute(db, "MATCH (n:ListNode) RETURN n");
	ASSERT_NE(res, nullptr);
	ASSERT_TRUE(zyx_result_next(res));

	const char *json = zyx_result_get_props_json(res, 0);
	EXPECT_NE(json, nullptr);
	std::string jsonStr(json);
	EXPECT_NE(jsonStr.find("test"), std::string::npos);

	zyx_result_close(res);
}

TEST_F(CApiTest, GetBoolTrue) {
	// Exercise the get_bool path with true value
	auto *res = zyx_execute(db, "RETURN true AS b");
	ASSERT_NE(res, nullptr);
	ASSERT_TRUE(zyx_result_next(res));

	EXPECT_TRUE(zyx_result_get_bool(res, 0));

	zyx_result_close(res);
}

TEST_F(CApiTest, GetBoolFalse) {
	// Exercise the get_bool path with false value
	auto *res = zyx_execute(db, "RETURN false AS b");
	ASSERT_NE(res, nullptr);
	ASSERT_TRUE(zyx_result_next(res));

	EXPECT_FALSE(zyx_result_get_bool(res, 0));

	zyx_result_close(res);
}

TEST_F(CApiTest, GetDoubleValid) {
	// Exercise the get_double path with valid double
	auto *res = zyx_execute(db, "RETURN 2.718 AS e");
	ASSERT_NE(res, nullptr);
	ASSERT_TRUE(zyx_result_next(res));

	EXPECT_DOUBLE_EQ(zyx_result_get_double(res, 0), 2.718);

	zyx_result_close(res);
}

TEST_F(CApiTest, GetStringValid) {
	// Exercise the get_string path with valid string
	auto *res = zyx_execute(db, "RETURN 'world' AS w");
	ASSERT_NE(res, nullptr);
	ASSERT_TRUE(zyx_result_next(res));

	EXPECT_STREQ(zyx_result_get_string(res, 0), "world");

	zyx_result_close(res);
}
