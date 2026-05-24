#include <algorithm>
#include <chrono>
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
        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        dbPath = (fs::temp_directory_path() / ("zyx_driver_abi_graph_values_" + std::to_string(now) + "_" +
                                              std::to_string(std::rand())))
                     .string();
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

TEST_F(DriverAbiGraphValuesTest, DirectCreateNodeReturnsIdAndPersistsProperties) {
    zyx_driver_params_t *params = nullptr;
    zyx_driver_result_t *result = nullptr;
    struct Cleanup {
        zyx_driver_params_t *&params;
        zyx_driver_result_t *&result;
        zyx_driver_error_t *&error;
        ~Cleanup() {
            zyx_driver_result_free(result);
            zyx_driver_params_free(params, &error);
        }
    } cleanup{params, result, error};

    ASSERT_EQ(zyx_driver_params_create(&params, &error), ZYX_DRIVER_OK);
    ASSERT_NE(params, nullptr);
    ASSERT_EQ(error, nullptr);
    ASSERT_EQ(zyx_driver_params_set_string(params, "name", "Ada", &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_params_set_int64(params, "age", 37, &error), ZYX_DRIVER_OK);

    int64_t nodeId = 0;
    ASSERT_EQ(zyx_driver_db_create_node(db, "DirectPerson", params, &nodeId, &error), ZYX_DRIVER_OK);
    EXPECT_GT(nodeId, 0);
    ASSERT_EQ(error, nullptr);

    ASSERT_EQ(zyx_driver_db_execute(db, "MATCH (n:DirectPerson) RETURN n.name, n.age", nullptr, &result, &error),
              ZYX_DRIVER_OK);
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(zyx_driver_result_next(result, &error), ZYX_DRIVER_ROW);

    const char *name = nullptr;
    int64_t age = 0;
    EXPECT_EQ(zyx_driver_result_get_string(result, 0, &name, &error), ZYX_DRIVER_OK);
    ASSERT_NE(name, nullptr);
    EXPECT_STREQ(name, "Ada");
    EXPECT_EQ(zyx_driver_result_get_int64(result, 1, &age, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(age, 37);
    EXPECT_EQ(error, nullptr);
}

TEST_F(DriverAbiGraphValuesTest, DirectCreateEdgeReturnsIdAndPersistsRelationship) {
    zyx_driver_params_t *params = nullptr;
    zyx_driver_result_t *result = nullptr;
    struct Cleanup {
        zyx_driver_params_t *&params;
        zyx_driver_result_t *&result;
        zyx_driver_error_t *&error;
        ~Cleanup() {
            zyx_driver_result_free(result);
            zyx_driver_params_free(params, &error);
        }
    } cleanup{params, result, error};

    int64_t sourceId = 0;
    int64_t targetId = 0;
    ASSERT_EQ(zyx_driver_db_create_node(db, "DirectEndpoint", nullptr, &sourceId, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_db_create_node(db, "DirectEndpoint", nullptr, &targetId, &error), ZYX_DRIVER_OK);
    ASSERT_GT(sourceId, 0);
    ASSERT_GT(targetId, 0);

    ASSERT_EQ(zyx_driver_params_create(&params, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_params_set_int64(params, "since", 2026, &error), ZYX_DRIVER_OK);
    int64_t edgeId = 0;
    ASSERT_EQ(zyx_driver_db_create_edge(db, sourceId, targetId, "DIRECT_REL", params, &edgeId, &error), ZYX_DRIVER_OK);
    EXPECT_GT(edgeId, 0);
    ASSERT_EQ(error, nullptr);

    ASSERT_EQ(zyx_driver_db_execute(db, "MATCH ()-[e:DIRECT_REL]->() RETURN e", nullptr, &result, &error),
              ZYX_DRIVER_OK);
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(zyx_driver_result_next(result, &error), ZYX_DRIVER_ROW);

    int64_t queriedEdgeId = 0;
    int64_t queriedSince = 0;
    EXPECT_EQ(zyx_driver_result_get_edge_id(result, 0, &queriedEdgeId, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(queriedEdgeId, edgeId);
    zyx_driver_result_free(result);
    result = nullptr;

    ASSERT_EQ(zyx_driver_db_execute(db, "MATCH ()-[e:DIRECT_REL]->() RETURN e.since", nullptr, &result, &error),
              ZYX_DRIVER_OK);
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(zyx_driver_result_next(result, &error), ZYX_DRIVER_ROW);
    EXPECT_EQ(zyx_driver_result_get_int64(result, 0, &queriedSince, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(queriedSince, 2026);
    EXPECT_EQ(error, nullptr);
}

TEST_F(DriverAbiGraphValuesTest, DirectCreateNodeWithLabelsPersistsAllLabels) {
    zyx_driver_params_t *params = nullptr;
    zyx_driver_result_t *result = nullptr;
    struct Cleanup {
        zyx_driver_params_t *&params;
        zyx_driver_result_t *&result;
        zyx_driver_error_t *&error;
        ~Cleanup() {
            zyx_driver_result_free(result);
            zyx_driver_params_free(params, &error);
        }
    } cleanup{params, result, error};

    ASSERT_EQ(zyx_driver_params_create(&params, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_params_set_string(params, "name", "Multi", &error), ZYX_DRIVER_OK);
    const char *labels[] = {"DirectMulti", "DirectEmployee"};
    int64_t nodeId = 0;
    ASSERT_EQ(zyx_driver_db_create_node_with_labels(db, labels, 2, params, &nodeId, &error), ZYX_DRIVER_OK);
    EXPECT_GT(nodeId, 0);
    ASSERT_EQ(error, nullptr);

    ASSERT_EQ(zyx_driver_db_execute(db, "MATCH (n:DirectMulti:DirectEmployee) RETURN n", nullptr, &result, &error),
              ZYX_DRIVER_OK);
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(zyx_driver_result_next(result, &error), ZYX_DRIVER_ROW);

    uint32_t labelCount = 0;
    ASSERT_EQ(zyx_driver_result_get_node_label_count(result, 0, &labelCount, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(labelCount, 2u);
    std::vector<std::string> returnedLabels;
    for (uint32_t i = 0; i < labelCount; ++i) {
        const char *label = nullptr;
        ASSERT_EQ(zyx_driver_result_get_node_label(result, 0, i, &label, &error), ZYX_DRIVER_OK);
        ASSERT_NE(label, nullptr);
        returnedLabels.emplace_back(label);
    }
    EXPECT_NE(std::find(returnedLabels.begin(), returnedLabels.end(), "DirectMulti"), returnedLabels.end());
    EXPECT_NE(std::find(returnedLabels.begin(), returnedLabels.end(), "DirectEmployee"), returnedLabels.end());
    EXPECT_EQ(error, nullptr);
}

TEST_F(DriverAbiGraphValuesTest, DirectCreateGraphValidatesArguments) {
    int64_t id = 0;

    EXPECT_EQ(zyx_driver_db_create_node(nullptr, "DirectInvalid", nullptr, &id, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(zyx_driver_error_code(error), ZYX_DRIVER_INVALID_ARGUMENT);
    zyx_driver_error_free(error);
    error = nullptr;

    EXPECT_EQ(zyx_driver_db_create_node(db, nullptr, nullptr, &id, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(zyx_driver_error_code(error), ZYX_DRIVER_INVALID_ARGUMENT);
    zyx_driver_error_free(error);
    error = nullptr;

    EXPECT_EQ(zyx_driver_db_create_node(db, "DirectInvalid", nullptr, nullptr, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(zyx_driver_error_code(error), ZYX_DRIVER_INVALID_ARGUMENT);
    zyx_driver_error_free(error);
    error = nullptr;

    const char *labels[] = {"DirectInvalid"};
    EXPECT_EQ(zyx_driver_db_create_node_with_labels(nullptr, labels, 1, nullptr, &id, &error),
              ZYX_DRIVER_INVALID_ARGUMENT);
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(zyx_driver_error_code(error), ZYX_DRIVER_INVALID_ARGUMENT);
    zyx_driver_error_free(error);
    error = nullptr;

    EXPECT_EQ(zyx_driver_db_create_node_with_labels(db, nullptr, 1, nullptr, &id, &error),
              ZYX_DRIVER_INVALID_ARGUMENT);
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(zyx_driver_error_code(error), ZYX_DRIVER_INVALID_ARGUMENT);
    zyx_driver_error_free(error);
    error = nullptr;

    EXPECT_EQ(zyx_driver_db_create_node_with_labels(db, labels, 0, nullptr, &id, &error),
              ZYX_DRIVER_INVALID_ARGUMENT);
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(zyx_driver_error_code(error), ZYX_DRIVER_INVALID_ARGUMENT);
    zyx_driver_error_free(error);
    error = nullptr;

    EXPECT_EQ(zyx_driver_db_create_node_with_labels(db, labels, 1, nullptr, nullptr, &error),
              ZYX_DRIVER_INVALID_ARGUMENT);
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(zyx_driver_error_code(error), ZYX_DRIVER_INVALID_ARGUMENT);
    zyx_driver_error_free(error);
    error = nullptr;

    const char *labelsWithNull[] = {"DirectInvalid", nullptr};
    EXPECT_EQ(zyx_driver_db_create_node_with_labels(db, labelsWithNull, 2, nullptr, &id, &error),
              ZYX_DRIVER_INVALID_ARGUMENT);
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(zyx_driver_error_code(error), ZYX_DRIVER_INVALID_ARGUMENT);
    EXPECT_NE(std::string(zyx_driver_error_message(error)).find("labels"), std::string::npos);
    zyx_driver_error_free(error);
    error = nullptr;

    EXPECT_EQ(zyx_driver_db_create_edge(nullptr, 1, 2, "DIRECT_INVALID", nullptr, &id, &error),
              ZYX_DRIVER_INVALID_ARGUMENT);
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(zyx_driver_error_code(error), ZYX_DRIVER_INVALID_ARGUMENT);
    zyx_driver_error_free(error);
    error = nullptr;

    EXPECT_EQ(zyx_driver_db_create_edge(db, 1, 2, nullptr, nullptr, &id, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(zyx_driver_error_code(error), ZYX_DRIVER_INVALID_ARGUMENT);
    zyx_driver_error_free(error);
    error = nullptr;

    EXPECT_EQ(zyx_driver_db_create_edge(db, 1, 2, "DIRECT_INVALID", nullptr, nullptr, &error),
              ZYX_DRIVER_INVALID_ARGUMENT);
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(zyx_driver_error_code(error), ZYX_DRIVER_INVALID_ARGUMENT);
    zyx_driver_error_free(error);
    error = nullptr;
}

TEST_F(DriverAbiGraphValuesTest, DirectCreateGraphAcceptsNullProperties) {
    int64_t sourceId = 0;
    int64_t targetId = 0;
    int64_t edgeId = 0;

    ASSERT_EQ(zyx_driver_db_create_node(db, "DirectEmpty", nullptr, &sourceId, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_db_create_node(db, "DirectEmpty", nullptr, &targetId, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_db_create_edge(db, sourceId, targetId, "DIRECT_EMPTY", nullptr, &edgeId, &error),
              ZYX_DRIVER_OK);
    EXPECT_GT(sourceId, 0);
    EXPECT_GT(targetId, 0);
    EXPECT_GT(edgeId, 0);
    EXPECT_EQ(error, nullptr);
}

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

    const char *label = nullptr;
    EXPECT_EQ(zyx_driver_result_get_node_label(result, 0, labelCount, &label, &error), ZYX_DRIVER_OUT_OF_RANGE);
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(zyx_driver_error_code(error), ZYX_DRIVER_OUT_OF_RANGE);
    EXPECT_NE(std::string(zyx_driver_error_message(error)).find("label"), std::string::npos);
    zyx_driver_error_free(error);
    error = nullptr;

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

TEST_F(DriverAbiGraphValuesTest, EntityPropertiesJsonEscapesGraphStrings) {
    zyx_driver_result_t *result = nullptr;

    ASSERT_EQ(zyx_driver_db_execute(db,
                                    R"ZYX(CREATE (a:JsonEsc {text:'Quote:" Slash:\\ Line\nTab\t'})-[e:ESCAPES {text:'Edge:" Path:\\ Ctrl\b'}]->(b:JsonEsc {name:'target'}))ZYX",
                                    nullptr, &result, &error),
              ZYX_DRIVER_OK);
    zyx_driver_result_free(result);
    result = nullptr;

    ASSERT_EQ(zyx_driver_db_execute(db, "MATCH (n:JsonEsc)-[e:ESCAPES]->() RETURN n, e", nullptr, &result, &error),
              ZYX_DRIVER_OK);
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(error, nullptr);
    ASSERT_EQ(zyx_driver_result_next(result, &error), ZYX_DRIVER_ROW);
    ASSERT_EQ(error, nullptr);

    const char *nodePropertiesJson = nullptr;
    ASSERT_EQ(zyx_driver_result_get_entity_properties_json(result, 0, &nodePropertiesJson, &error), ZYX_DRIVER_OK);
    ASSERT_NE(nodePropertiesJson, nullptr);
    const std::string nodeJson(nodePropertiesJson);
    EXPECT_NE(nodeJson.find(R"(Quote:\")"), std::string::npos);
    EXPECT_NE(nodeJson.find(R"(Slash:\\)"), std::string::npos);
    EXPECT_NE(nodeJson.find("Line\\nTab\\t"), std::string::npos);

    const char *edgePropertiesJson = nullptr;
    ASSERT_EQ(zyx_driver_result_get_entity_properties_json(result, 1, &edgePropertiesJson, &error), ZYX_DRIVER_OK);
    ASSERT_NE(edgePropertiesJson, nullptr);
    const std::string edgeJson(edgePropertiesJson);
    EXPECT_NE(edgeJson.find(R"(Edge:\")"), std::string::npos);
    EXPECT_NE(edgeJson.find(R"(Path:\\)"), std::string::npos);
    EXPECT_NE(edgeJson.find("Ctrl\\b"), std::string::npos);
    EXPECT_EQ(error, nullptr);

    zyx_driver_result_free(result);
}


TEST_F(DriverAbiGraphValuesTest, EntityPropertiesJsonIncludesListParams) {
    zyx_driver_params_t *params = nullptr;
    zyx_driver_result_t *result = nullptr;
    struct Cleanup {
        zyx_driver_params_t *&params;
        zyx_driver_result_t *&result;
        zyx_driver_error_t *&error;
        ~Cleanup() {
            zyx_driver_result_free(result);
            result = nullptr;
            zyx_driver_params_free(params, &error);
            params = nullptr;
        }
    } cleanup{params, result, error};

    ASSERT_EQ(zyx_driver_params_create(&params, &error), ZYX_DRIVER_OK);
    const char *tags[] = {"graph", "abi"};
    const float embedding[] = {0.5F, 1.25F};
    ASSERT_EQ(zyx_driver_params_set_string_list(params, "tags", tags, 2, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_params_set_float_list(params, "embedding", embedding, 2, &error), ZYX_DRIVER_OK);

    int64_t nodeId = 0;
    ASSERT_EQ(zyx_driver_db_create_node(db, "JsonList", params, &nodeId, &error), ZYX_DRIVER_OK);
    ASSERT_GT(nodeId, 0);

    ASSERT_EQ(zyx_driver_db_execute(db, "MATCH (n:JsonList) RETURN n", nullptr, &result, &error), ZYX_DRIVER_OK);
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(zyx_driver_result_next(result, &error), ZYX_DRIVER_ROW);

    const char *propertiesJson = nullptr;
    ASSERT_EQ(zyx_driver_result_get_entity_properties_json(result, 0, &propertiesJson, &error), ZYX_DRIVER_OK);
    ASSERT_NE(propertiesJson, nullptr);
    const std::string json(propertiesJson);
    EXPECT_NE(json.find("\"tags\":[\"graph\",\"abi\"]"), std::string::npos);
    EXPECT_NE(json.find("\"embedding\":["), std::string::npos);
    EXPECT_NE(json.find("0.5"), std::string::npos);
    EXPECT_NE(json.find("1.25"), std::string::npos);
    EXPECT_EQ(error, nullptr);
}

TEST_F(DriverAbiGraphValuesTest, EntityPropertiesJsonIncludesScalarParamsAndControlEscapes) {
    zyx_driver_params_t *params = nullptr;
    zyx_driver_result_t *result = nullptr;
    struct Cleanup {
        zyx_driver_params_t *&params;
        zyx_driver_result_t *&result;
        zyx_driver_error_t *&error;
        ~Cleanup() {
            zyx_driver_result_free(result);
            result = nullptr;
            zyx_driver_params_free(params, &error);
            params = nullptr;
        }
    } cleanup{params, result, error};

    ASSERT_EQ(zyx_driver_params_create(&params, &error), ZYX_DRIVER_OK);
    ASSERT_NE(params, nullptr);
    ASSERT_EQ(zyx_driver_params_set_null(params, "missing", &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_params_set_bool(params, "ok", true, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_params_set_bool(params, "nope", false, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_params_set_double(params, "score", 98.25, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_params_set_int64(params, "count", 7, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_params_set_string(params, "ctrl", "form\ffeed\rcarriage\x01unit", &error), ZYX_DRIVER_OK);

    ASSERT_EQ(zyx_driver_db_execute(db,
                                    "CREATE (n:JsonScalar {missing:$missing, ok:$ok, nope:$nope, score:$score, count:$count, ctrl:$ctrl})",
                                    params, &result, &error),
              ZYX_DRIVER_OK);
    zyx_driver_result_free(result);
    result = nullptr;

    ASSERT_EQ(zyx_driver_db_execute(db, "MATCH (n:JsonScalar) RETURN n", nullptr, &result, &error), ZYX_DRIVER_OK);
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(zyx_driver_result_next(result, &error), ZYX_DRIVER_ROW);

    const char *propertiesJson = nullptr;
    ASSERT_EQ(zyx_driver_result_get_entity_properties_json(result, 0, &propertiesJson, &error), ZYX_DRIVER_OK);
    ASSERT_NE(propertiesJson, nullptr);
    const std::string json(propertiesJson);
    EXPECT_NE(json.find("\"missing\":null"), std::string::npos);
    EXPECT_NE(json.find("\"ok\":true"), std::string::npos);
    EXPECT_NE(json.find("\"nope\":false"), std::string::npos);
    EXPECT_NE(json.find("\"score\":"), std::string::npos);
    EXPECT_NE(json.find("98.25"), std::string::npos);
    EXPECT_NE(json.find("\"count\":7"), std::string::npos);
    EXPECT_NE(json.find("\\f"), std::string::npos);
    EXPECT_NE(json.find("\\r"), std::string::npos);
    EXPECT_NE(json.find("\\u0001"), std::string::npos);
    EXPECT_EQ(error, nullptr);
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
    EXPECT_EQ(zyx_driver_result_get_edge_target_id(result, 9, &intValue, nullptr), ZYX_DRIVER_OUT_OF_RANGE);
    EXPECT_EQ(zyx_driver_result_get_edge_type(result, 2, &stringValue, nullptr), ZYX_DRIVER_TYPE_MISMATCH);
    EXPECT_EQ(zyx_driver_result_get_edge_source_id(result, 2, &intValue, nullptr), ZYX_DRIVER_TYPE_MISMATCH);
    EXPECT_EQ(zyx_driver_result_get_edge_target_id(result, 2, &intValue, nullptr), ZYX_DRIVER_TYPE_MISMATCH);
    EXPECT_EQ(zyx_driver_result_get_entity_properties_json(result, 9, &stringValue, nullptr), ZYX_DRIVER_OUT_OF_RANGE);
    EXPECT_EQ(zyx_driver_result_get_entity_properties_json(result, 2, &stringValue, nullptr), ZYX_DRIVER_TYPE_MISMATCH);

    EXPECT_EQ(zyx_driver_result_get_string(result, 0, &stringValue, &error), ZYX_DRIVER_TYPE_MISMATCH);
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(zyx_driver_error_code(error), ZYX_DRIVER_TYPE_MISMATCH);
    EXPECT_NE(std::string(zyx_driver_error_message(error)).find("node"), std::string::npos);
    zyx_driver_error_free(error);
    error = nullptr;
    EXPECT_EQ(zyx_driver_result_get_string(result, 1, &stringValue, &error), ZYX_DRIVER_TYPE_MISMATCH);
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(zyx_driver_error_code(error), ZYX_DRIVER_TYPE_MISMATCH);
    EXPECT_NE(std::string(zyx_driver_error_message(error)).find("edge"), std::string::npos);
    zyx_driver_error_free(error);
    error = nullptr;

    EXPECT_EQ(zyx_driver_result_get_node_id(result, 0, nullptr, nullptr), ZYX_DRIVER_INVALID_ARGUMENT);
    EXPECT_EQ(zyx_driver_result_get_node_label_count(result, 0, nullptr, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(zyx_driver_error_code(error), ZYX_DRIVER_INVALID_ARGUMENT);
    zyx_driver_error_free(error);
    error = nullptr;
    EXPECT_EQ(zyx_driver_result_get_node_label(result, 0, 0, nullptr, nullptr), ZYX_DRIVER_INVALID_ARGUMENT);
    EXPECT_EQ(zyx_driver_result_get_edge_id(result, 1, nullptr, nullptr), ZYX_DRIVER_INVALID_ARGUMENT);
    EXPECT_EQ(zyx_driver_result_get_edge_source_id(result, 1, nullptr, nullptr), ZYX_DRIVER_INVALID_ARGUMENT);
    EXPECT_EQ(zyx_driver_result_get_edge_target_id(result, 1, nullptr, nullptr), ZYX_DRIVER_INVALID_ARGUMENT);
    EXPECT_EQ(zyx_driver_result_get_edge_type(result, 1, nullptr, nullptr), ZYX_DRIVER_INVALID_ARGUMENT);
    EXPECT_EQ(zyx_driver_result_get_entity_properties_json(result, 0, nullptr, nullptr), ZYX_DRIVER_INVALID_ARGUMENT);

    zyx_driver_result_free(result);
}
