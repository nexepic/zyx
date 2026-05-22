#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>

#include "zyx/zyx_driver_abi.h"

namespace fs = std::filesystem;

class DriverAbiGraphValuesTest : public ::testing::Test {
protected:
    std::string dbPath;
    zyx_driver_db_t *db = nullptr;
    zyx_driver_error_t *error = nullptr;

    void SetUp() override {
        dbPath = (fs::temp_directory_path() / ("zyx_driver_abi_graph_values_" + std::to_string(std::rand()))).string();
        cleanup();
        ASSERT_EQ(zyx_driver_db_open(dbPath.c_str(), &db, &error), ZYX_DRIVER_OK);
        ASSERT_NE(db, nullptr);
        ASSERT_EQ(error, nullptr);
    }

    void TearDown() override {
        if (db != nullptr) {
            EXPECT_EQ(zyx_driver_db_close(db, &error), ZYX_DRIVER_OK);
            db = nullptr;
        }
        zyx_driver_error_free(error);
        error = nullptr;
        cleanup();
    }

    void cleanup() {
        std::error_code ec;
        fs::remove_all(dbPath, ec);
        fs::remove(dbPath + "-wal", ec);
    }
};

TEST_F(DriverAbiGraphValuesTest, NodeExposesPluralLabelsAndPropertiesJson) {
    zyx_driver_result_t *result = nullptr;

    ASSERT_EQ(zyx_driver_db_execute(db, "CREATE (n:Person:Engineer {name:'Ada', age:37})", nullptr, &result, &error),
              ZYX_DRIVER_OK);
    zyx_driver_result_free(result);
    result = nullptr;

    ASSERT_EQ(zyx_driver_db_execute(db, "MATCH (n:Person:Engineer) RETURN n", nullptr, &result, &error), ZYX_DRIVER_OK);
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(error, nullptr);
    ASSERT_EQ(zyx_driver_result_next(result, &error), ZYX_DRIVER_ROW);
    ASSERT_EQ(error, nullptr);

    EXPECT_EQ(zyx_driver_result_value_type(result, 0), ZYX_DRIVER_VALUE_NODE);

    int64_t nodeId = 0;
    uint32_t labelCount = 0;
    const char *propertiesJson = nullptr;
    EXPECT_EQ(zyx_driver_result_get_node_id(result, 0, &nodeId, &error), ZYX_DRIVER_OK);
    EXPECT_GT(nodeId, 0);
    EXPECT_EQ(zyx_driver_result_get_node_label_count(result, 0, &labelCount, &error), ZYX_DRIVER_OK);
    EXPECT_GE(labelCount, 1u);

    std::vector<std::string> labels;
    for (uint32_t i = 0; i < labelCount; ++i) {
        const char *label = nullptr;
        ASSERT_EQ(zyx_driver_result_get_node_label(result, 0, i, &label, &error), ZYX_DRIVER_OK);
        ASSERT_NE(label, nullptr);
        labels.emplace_back(label);
    }
    EXPECT_NE(std::find(labels.begin(), labels.end(), "Person"), labels.end());
    EXPECT_NE(std::find(labels.begin(), labels.end(), "Engineer"), labels.end());

    EXPECT_EQ(zyx_driver_result_get_entity_properties_json(result, 0, &propertiesJson, &error), ZYX_DRIVER_OK);
    ASSERT_NE(propertiesJson, nullptr);
    EXPECT_NE(std::string(propertiesJson).find("\"name\":\"Ada\""), std::string::npos);
    EXPECT_EQ(error, nullptr);

    zyx_driver_result_free(result);
}

TEST_F(DriverAbiGraphValuesTest, EdgeExposesTypeAndTopology) {
    zyx_driver_result_t *result = nullptr;

    ASSERT_EQ(zyx_driver_db_execute(db,
                                    "CREATE (a:Person {name:'Ada'})-[e:KNOWS]->(b:Person {name:'Bob'})",
                                    nullptr, &result, &error),
              ZYX_DRIVER_OK);
    zyx_driver_result_free(result);
    result = nullptr;

    ASSERT_EQ(zyx_driver_db_execute(db, "MATCH (:Person {name:'Ada'})-[e:KNOWS]->(:Person {name:'Bob'}) RETURN e",
                                    nullptr, &result, &error),
              ZYX_DRIVER_OK);
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(error, nullptr);
    ASSERT_EQ(zyx_driver_result_next(result, &error), ZYX_DRIVER_ROW);
    ASSERT_EQ(error, nullptr);

    EXPECT_EQ(zyx_driver_result_value_type(result, 0), ZYX_DRIVER_VALUE_EDGE);

    int64_t edgeId = 0;
    int64_t sourceId = 0;
    int64_t targetId = 0;
    const char *edgeType = nullptr;
    EXPECT_EQ(zyx_driver_result_get_edge_id(result, 0, &edgeId, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(zyx_driver_result_get_edge_source_id(result, 0, &sourceId, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(zyx_driver_result_get_edge_target_id(result, 0, &targetId, &error), ZYX_DRIVER_OK);
    EXPECT_GT(edgeId, 0);
    EXPECT_GT(sourceId, 0);
    EXPECT_GT(targetId, 0);

    EXPECT_EQ(zyx_driver_result_get_edge_type(result, 0, &edgeType, &error), ZYX_DRIVER_OK);
    ASSERT_NE(edgeType, nullptr);
    EXPECT_STREQ(edgeType, "KNOWS");
    EXPECT_EQ(error, nullptr);

    zyx_driver_result_free(result);
}

TEST_F(DriverAbiGraphValuesTest, GraphGetterErrorsPreserveStatusWithoutErrorOut) {
    zyx_driver_result_t *result = nullptr;

    ASSERT_EQ(zyx_driver_db_execute(db,
                                    "CREATE (:Person {name:'Ada'})-[:KNOWS]->(:Person {name:'Bob'})",
                                    nullptr, &result, &error),
              ZYX_DRIVER_OK);
    zyx_driver_result_free(result);
    result = nullptr;

    ASSERT_EQ(zyx_driver_db_execute(db,
                                    "MATCH (n:Person {name:'Ada'})-[e:KNOWS]->() RETURN n, e, 42",
                                    nullptr, &result, &error),
              ZYX_DRIVER_OK);
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(error, nullptr);
    ASSERT_EQ(zyx_driver_result_next(result, &error), ZYX_DRIVER_ROW);
    ASSERT_EQ(error, nullptr);

    int64_t intValue = 0;
    uint32_t labelCount = 0;
    const char *stringValue = nullptr;

    EXPECT_EQ(zyx_driver_result_get_node_id(result, 9, &intValue, nullptr), ZYX_DRIVER_OUT_OF_RANGE);
    EXPECT_EQ(zyx_driver_result_get_node_label_count(result, 2, &labelCount, nullptr), ZYX_DRIVER_TYPE_MISMATCH);
    EXPECT_EQ(zyx_driver_result_get_node_label(result, 2, 0, &stringValue, nullptr), ZYX_DRIVER_TYPE_MISMATCH);
    EXPECT_EQ(zyx_driver_result_get_edge_id(result, 9, &intValue, nullptr), ZYX_DRIVER_OUT_OF_RANGE);
    EXPECT_EQ(zyx_driver_result_get_edge_type(result, 2, &stringValue, nullptr), ZYX_DRIVER_TYPE_MISMATCH);
    EXPECT_EQ(zyx_driver_result_get_edge_source_id(result, 2, &intValue, nullptr), ZYX_DRIVER_TYPE_MISMATCH);
    EXPECT_EQ(zyx_driver_result_get_entity_properties_json(result, 9, &stringValue, nullptr), ZYX_DRIVER_OUT_OF_RANGE);
    EXPECT_EQ(zyx_driver_result_get_entity_properties_json(result, 2, &stringValue, nullptr), ZYX_DRIVER_TYPE_MISMATCH);

    EXPECT_EQ(zyx_driver_result_get_node_id(result, 0, nullptr, nullptr), ZYX_DRIVER_INVALID_ARGUMENT);
    EXPECT_EQ(zyx_driver_result_get_node_label(result, 0, 0, nullptr, nullptr), ZYX_DRIVER_INVALID_ARGUMENT);
    EXPECT_EQ(zyx_driver_result_get_edge_id(result, 1, nullptr, nullptr), ZYX_DRIVER_INVALID_ARGUMENT);
    EXPECT_EQ(zyx_driver_result_get_edge_type(result, 1, nullptr, nullptr), ZYX_DRIVER_INVALID_ARGUMENT);
    EXPECT_EQ(zyx_driver_result_get_entity_properties_json(result, 0, nullptr, nullptr), ZYX_DRIVER_INVALID_ARGUMENT);

    zyx_driver_result_free(result);
}
