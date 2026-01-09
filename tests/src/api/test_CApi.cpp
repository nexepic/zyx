/**
 * @file test_CApi.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2026/1/5
 *
 * @copyright Copyright (c) 2026 Nexepic
 *
 **/

#include <gtest/gtest.h>
#include <filesystem>
#include <string>
#include "metrix/metrix_c_api.h"

namespace fs = std::filesystem;

class CApiTest : public ::testing::Test {
protected:
    std::string dbPath;
    MetrixDB_T* db = nullptr;

    void SetUp() override {
        // Create a unique temporary path for the database
        auto tempDir = fs::temp_directory_path();
        dbPath = (tempDir / ("c_api_test_" + std::to_string(std::rand()))).string();
        if (fs::exists(dbPath)) fs::remove_all(dbPath);
        // Standard open creates the DB
        db = metrix_open(dbPath.c_str());
    }

    void TearDown() override {
        if (db) metrix_close(db);
        if (fs::exists(dbPath)) fs::remove_all(dbPath);
    }
};

TEST_F(CApiTest, ExecuteAndIterate) {
    metrix_result_close(metrix_execute(db, "CREATE (n:CNode {id: 1, name: 'C-Lang', pi: 3.14, stable: true})"));

    MetrixResult_T* res = metrix_execute(db, "MATCH (n:CNode) RETURN n.id, n.name, n.pi, n.stable");
    ASSERT_NE(res, nullptr);
    ASSERT_TRUE(metrix_result_next(res));

    // Map columns dynamically to be robust against sorting/parser changes
    int idxId = -1, idxName = -1, idxPi = -1, idxBool = -1;
    int cols = metrix_result_column_count(res);
    for(int i=0; i<cols; i++) {
        std::string name = metrix_result_column_name(res, i);
        if (name.find("id") != std::string::npos) idxId = i;
        else if (name.find("name") != std::string::npos) idxName = i;
        else if (name.find("pi") != std::string::npos) idxPi = i;
        else if (name.find("stable") != std::string::npos) idxBool = i;
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

    auto* res = metrix_execute(db, "MATCH (n:Obj) RETURN n");
    ASSERT_TRUE(metrix_result_next(res));

    EXPECT_EQ(metrix_result_get_type(res, 0), MX_NODE);

    MetrixNode node_struct;
    bool success = metrix_result_get_node(res, 0, &node_struct);
    EXPECT_TRUE(success);
    EXPECT_STREQ(node_struct.label, "Obj");

    const char* json = metrix_result_get_props_json(res, 0);
    std::string jsonStr(json);
    EXPECT_NE(jsonStr.find("prop"), std::string::npos);

    metrix_result_close(res);
}

TEST_F(CApiTest, EdgeObjectAccess) {
    metrix_result_close(metrix_execute(db, "CREATE (a:N {id:1})"));
    metrix_result_close(metrix_execute(db, "CREATE (b:N {id:2})"));

    // Match and Create edge
    metrix_result_close(metrix_execute(db, "MATCH (a:N {id:1}), (b:N {id:2}) CREATE (a)-[e:REL {w:10}]->(b)"));

    auto* res = metrix_execute(db, "MATCH ()-[e:REL]->() RETURN e");
    ASSERT_TRUE(metrix_result_next(res));

    EXPECT_EQ(metrix_result_get_type(res, 0), MX_EDGE);

    MetrixEdge edge_struct;
    bool success = metrix_result_get_edge(res, 0, &edge_struct);
    ASSERT_TRUE(success);
    EXPECT_STREQ(edge_struct.type, "REL");

    metrix_result_close(res);
}

TEST_F(CApiTest, ErrorHandling) {
    MetrixDB_T* bad_db = metrix_open(nullptr);
    EXPECT_EQ(bad_db, nullptr);
    MetrixResult_T* res = metrix_execute(nullptr, "MATCH (n) RETURN n");
    EXPECT_EQ(res, nullptr);
}

TEST_F(CApiTest, EmptyResult) {
    auto* res = metrix_execute(db, "MATCH (n:NonExistent) RETURN n");
    EXPECT_FALSE(metrix_result_next(res));
    metrix_result_close(res);
}

TEST_F(CApiTest, OutOfBoundsAccess) {
    metrix_result_close(metrix_execute(db, "CREATE (n:OneCol)"));
    auto* res = metrix_execute(db, "MATCH (n:OneCol) RETURN n");
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
	const char* createQuery = "MATCH (a:N {id:1}), (b:N {id:2}) CREATE (a)-[e:REL {w:10}]->(b)";
	MetrixResult_T* createRes = metrix_execute(db, createQuery);
	ASSERT_NE(createRes, nullptr);
	metrix_result_close(createRes);

	// 3. Verification
	auto* res = metrix_execute(db, "MATCH ()-[e:REL]->() RETURN e");
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
	const char* jsonProps = metrix_result_get_props_json(res, 0);
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
    if (fs::exists(badPath)) fs::remove_all(badPath);

    db = metrix_open_if_exists(badPath.c_str());
    EXPECT_EQ(db, nullptr) << "metrix_open_if_exists should return null for missing DB";
}

TEST_F(CApiTest, NullHandleSafeguards) {
    // Test that passing nullptr to result accessors handles it gracefully
    MetrixResult_T* nullRes = nullptr;

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
    auto* res = metrix_execute(db, "MATCH (n:Safe) RETURN n.val");
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
    auto* goodRes = metrix_execute(db, "RETURN 1");
    ASSERT_NE(goodRes, nullptr);
    EXPECT_TRUE(metrix_result_is_success(goodRes));
    metrix_result_close(goodRes);

    // 2. Failure Case (Triggered via exception in metrix_execute -> returns null)
    // Assuming the DB throws on invalid syntax
    auto* badRes = metrix_execute(db, "THIS IS INVALID CYPHER");
    if (badRes == nullptr) {
        // Check global error
        const char* err = metrix_get_last_error();
        EXPECT_NE(err, nullptr);
        // We expect a non-empty error message, though exact content depends on parser
        EXPECT_STRNE(err, "");
    } else {
        // Implementation might return a result object representing failure
        EXPECT_FALSE(metrix_result_is_success(badRes));
        const char* err = metrix_result_get_error(badRes);
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
	const char* err = metrix_get_last_error();
	// It might be empty or hold previous error, just ensure no crash
	(void)err;
}

// Verify Large String Handling (Buffer Check)
TEST_F(CApiTest, LargeStringPayload) {
	std::string large(10000, 'x'); // 10KB string
	std::string query = "RETURN '" + large + "' AS s";

	auto* res = metrix_execute(db, query.c_str());
	ASSERT_TRUE(metrix_result_next(res));

	const char* val = metrix_result_get_string(res, 0);
	EXPECT_EQ(std::strlen(val), 10000);
	EXPECT_EQ(val[0], 'x');
	EXPECT_EQ(val[9999], 'x');

	metrix_result_close(res);
}