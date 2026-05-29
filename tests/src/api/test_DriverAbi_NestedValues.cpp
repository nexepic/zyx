#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>
#include <algorithm>

#include <gtest/gtest.h>

#include "api/driver_abi/DriverAbiInternal.hpp"
#include "zyx/zyx_driver_abi.h"

namespace fs = std::filesystem;

namespace {

std::string uniqueDbPath() {
    static std::atomic<unsigned long long> counter{0};
    const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const auto sequence = counter.fetch_add(1, std::memory_order_relaxed);
    return (fs::temp_directory_path() / ("zyx_driver_abi_nested_values_" + std::to_string(now) + "_" +
                                        std::to_string(std::rand()) + "_" + std::to_string(sequence)))
        .string();
}

struct RegisteredResultOwner {
    zyx_driver_result_t result;

    RegisteredResultOwner() {
        registerResultHandle(&result);
    }

    ~RegisteredResultOwner() {
        unregisterResultHandle(&result);
    }
};

void expectLocalError(zyx_driver_status_t status,
                      zyx_driver_status_t expected,
                      zyx_driver_error_t *&error) {
    EXPECT_EQ(status, expected);
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(zyx_driver_error_code(error), expected);
    zyx_driver_error_free(error);
    error = nullptr;
}

} // namespace

class DriverAbiNestedValuesTest : public ::testing::Test {
protected:
    std::string dbPath;
    zyx_driver_db_t *db = nullptr;
    zyx_driver_error_t *error = nullptr;

    void SetUp() override {
        dbPath = uniqueDbPath();
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

    void expectError(zyx_driver_status_t status, zyx_driver_status_t expected) {
        EXPECT_EQ(status, expected);
        ASSERT_NE(error, nullptr);
        EXPECT_EQ(zyx_driver_error_code(error), expected);
        zyx_driver_error_free(error);
        error = nullptr;
    }
};


TEST_F(DriverAbiNestedValuesTest, ReadsNestedNodeEntityThroughValueRef) {
    zyx_driver_params_t *params = nullptr;
    zyx_driver_value_t *nodesValue = nullptr;
    zyx_driver_value_t *nodeValue = nullptr;
    zyx_driver_result_t *result = nullptr;

    ASSERT_EQ(zyx_driver_params_create(&params, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_list_create(&nodesValue, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_create(&nodeValue, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_set_string(nodeValue, "__zyx_driver_entity", "node", &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_set_int64(nodeValue, "id", 101, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_set_string(nodeValue, "label0", "NestedPerson", &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_set_string(nodeValue, "label1", "NestedEngineer", &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_set_string(nodeValue, "prop:name", "Ada", &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_set_int64(nodeValue, "prop:age", 37, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_list_append_value(nodesValue, nodeValue, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_params_set_value(params, "nodes", nodesValue, &error), ZYX_DRIVER_OK);

    ASSERT_EQ(zyx_driver_db_execute(db, "RETURN $nodes AS nodes", params, &result, &error), ZYX_DRIVER_OK)
        << (error != nullptr ? zyx_driver_error_message(error) : "");
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(zyx_driver_result_next(result, &error), ZYX_DRIVER_ROW);

    zyx_driver_value_ref_t root{};
    ASSERT_EQ(zyx_driver_result_get_value(result, 0, &root, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(zyx_driver_value_ref_type(&root), ZYX_DRIVER_VALUE_LIST);

    zyx_driver_value_ref_t node{};
    ASSERT_EQ(zyx_driver_value_ref_list_get(&root, 0, &node, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(zyx_driver_value_ref_type(&node), ZYX_DRIVER_VALUE_NODE);

    int64_t nodeId = 0;
    ASSERT_EQ(zyx_driver_value_ref_get_node_id(&node, &nodeId, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(nodeId, 101);

    uint32_t labelCount = 0;
    ASSERT_EQ(zyx_driver_value_ref_get_node_label_count(&node, &labelCount, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(labelCount, 2u);

    std::vector<std::string> labels;
    for (uint32_t i = 0; i < labelCount; ++i) {
        const char *label = nullptr;
        ASSERT_EQ(zyx_driver_value_ref_get_node_label(result, &node, i, &label, &error), ZYX_DRIVER_OK);
        ASSERT_NE(label, nullptr);
        labels.emplace_back(label);
    }
    EXPECT_NE(std::find(labels.begin(), labels.end(), "NestedPerson"), labels.end());
    EXPECT_NE(std::find(labels.begin(), labels.end(), "NestedEngineer"), labels.end());

    const char *propertiesJson = nullptr;
    ASSERT_EQ(zyx_driver_value_ref_get_entity_properties_json(nullptr, &node, &propertiesJson, &error), ZYX_DRIVER_OK);
    ASSERT_NE(propertiesJson, nullptr);
    const std::string json(propertiesJson);
    EXPECT_NE(json.find("\"name\":\"Ada\""), std::string::npos);
    EXPECT_NE(json.find("\"age\":37"), std::string::npos);

    const char *labelFromOwner = nullptr;
    ASSERT_EQ(zyx_driver_value_ref_get_node_label(nullptr, &node, 0, &labelFromOwner, &error), ZYX_DRIVER_OK);
    ASSERT_NE(labelFromOwner, nullptr);
    EXPECT_TRUE(std::string(labelFromOwner) == "NestedPerson" || std::string(labelFromOwner) == "NestedEngineer");

    const char *badLabel = nullptr;
    expectError(zyx_driver_value_ref_get_node_label(result, &node, labelCount, &badLabel, &error), ZYX_DRIVER_OUT_OF_RANGE);

    zyx_driver_result_free(result);
    zyx_driver_value_free(nodeValue, &error);
    zyx_driver_value_free(nodesValue, &error);
    zyx_driver_params_free(params, &error);
}

TEST_F(DriverAbiNestedValuesTest, SerializesNestedEntityPropertyCollectionsToJson) {
    zyx_driver_params_t *params = nullptr;
    zyx_driver_value_t *nodeValue = nullptr;
    zyx_driver_value_t *tagsValue = nullptr;
    zyx_driver_value_t *metaValue = nullptr;
    zyx_driver_result_t *result = nullptr;

    ASSERT_EQ(zyx_driver_params_create(&params, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_create(&nodeValue, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_set_string(nodeValue, "__zyx_driver_entity", "node", &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_set_int64(nodeValue, "id", 303, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_set_string(nodeValue, "label0", "NestedCollections", &error), ZYX_DRIVER_OK);

    ASSERT_EQ(zyx_driver_value_list_create(&tagsValue, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_list_append_string(tagsValue, "graph", &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_list_append_int64(tagsValue, 7, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_set_value(nodeValue, "prop:tags", tagsValue, &error), ZYX_DRIVER_OK);

    ASSERT_EQ(zyx_driver_value_map_create(&metaValue, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_set_bool(metaValue, "active", true, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_set_double(metaValue, "score", 9.5, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_set_value(nodeValue, "prop:meta", metaValue, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_params_set_value(params, "node", nodeValue, &error), ZYX_DRIVER_OK);

    ASSERT_EQ(zyx_driver_db_execute(db, "RETURN $node AS node", params, &result, &error), ZYX_DRIVER_OK)
        << (error != nullptr ? zyx_driver_error_message(error) : "");
    ASSERT_EQ(zyx_driver_result_next(result, &error), ZYX_DRIVER_ROW);

    zyx_driver_value_ref_t node{};
    ASSERT_EQ(zyx_driver_result_get_value(result, 0, &node, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_ref_type(&node), ZYX_DRIVER_VALUE_NODE);

    const char *propertiesJson = nullptr;
    ASSERT_EQ(zyx_driver_value_ref_get_entity_properties_json(result, &node, &propertiesJson, &error), ZYX_DRIVER_OK);
    ASSERT_NE(propertiesJson, nullptr);
    const std::string json(propertiesJson);
    EXPECT_NE(json.find("\"tags\":[\"graph\",7]"), std::string::npos);
    EXPECT_NE(json.find("\"meta\":{"), std::string::npos);
    EXPECT_NE(json.find("\"active\":true"), std::string::npos);
    EXPECT_NE(json.find("\"score\":9.5"), std::string::npos);

    zyx_driver_result_free(result);
    zyx_driver_value_free(metaValue, &error);
    zyx_driver_value_free(tagsValue, &error);
    zyx_driver_value_free(nodeValue, &error);
    zyx_driver_params_free(params, &error);
}

TEST_F(DriverAbiNestedValuesTest, ReadsNestedEdgeEntityThroughValueRef) {
    zyx_driver_params_t *params = nullptr;
    zyx_driver_value_t *relsValue = nullptr;
    zyx_driver_value_t *edgeValue = nullptr;
    zyx_driver_result_t *result = nullptr;

    ASSERT_EQ(zyx_driver_params_create(&params, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_list_create(&relsValue, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_create(&edgeValue, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_set_string(edgeValue, "__zyx_driver_entity", "edge", &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_set_int64(edgeValue, "id", 202, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_set_int64(edgeValue, "source", 301, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_set_int64(edgeValue, "target", 302, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_set_string(edgeValue, "type", "NESTED_REL", &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_set_int64(edgeValue, "prop:since", 2026, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_list_append_value(relsValue, edgeValue, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_params_set_value(params, "rels", relsValue, &error), ZYX_DRIVER_OK);

    ASSERT_EQ(zyx_driver_db_execute(db, "RETURN $rels AS rels", params, &result, &error), ZYX_DRIVER_OK)
        << (error != nullptr ? zyx_driver_error_message(error) : "");
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(zyx_driver_result_next(result, &error), ZYX_DRIVER_ROW);

    zyx_driver_value_ref_t root{};
    ASSERT_EQ(zyx_driver_result_get_value(result, 0, &root, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(zyx_driver_value_ref_type(&root), ZYX_DRIVER_VALUE_LIST);

    zyx_driver_value_ref_t edge{};
    ASSERT_EQ(zyx_driver_value_ref_list_get(&root, 0, &edge, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(zyx_driver_value_ref_type(&edge), ZYX_DRIVER_VALUE_EDGE);

    int64_t edgeId = 0;
    int64_t sourceId = 0;
    int64_t targetId = 0;
    ASSERT_EQ(zyx_driver_value_ref_get_edge_id(&edge, &edgeId, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_ref_get_edge_source_id(&edge, &sourceId, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_ref_get_edge_target_id(&edge, &targetId, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(edgeId, 202);
    EXPECT_EQ(sourceId, 301);
    EXPECT_EQ(targetId, 302);
    EXPECT_NE(sourceId, targetId);

    const char *edgeType = nullptr;
    ASSERT_EQ(zyx_driver_value_ref_get_edge_type(nullptr, &edge, &edgeType, &error), ZYX_DRIVER_OK);
    ASSERT_NE(edgeType, nullptr);
    EXPECT_STREQ(edgeType, "NESTED_REL");

    const char *propertiesJson = nullptr;
    ASSERT_EQ(zyx_driver_value_ref_get_entity_properties_json(nullptr, &edge, &propertiesJson, &error), ZYX_DRIVER_OK);
    ASSERT_NE(propertiesJson, nullptr);
    EXPECT_NE(std::string(propertiesJson).find("\"since\":2026"), std::string::npos);

    uint32_t labelCount = 0;
    expectError(zyx_driver_value_ref_get_node_label_count(&edge, &labelCount, &error), ZYX_DRIVER_TYPE_MISMATCH);
    expectError(zyx_driver_value_ref_get_edge_id(nullptr, &edgeId, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_ref_get_edge_type(result, &edge, nullptr, &error), ZYX_DRIVER_INVALID_ARGUMENT);

    expectError(zyx_driver_value_ref_get_node_id(&edge, nullptr, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_ref_get_node_label_count(&edge, nullptr, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_ref_get_node_label(result, &edge, 0, nullptr, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_ref_get_edge_id(&edge, nullptr, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_ref_get_edge_source_id(&edge, nullptr, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_ref_get_edge_target_id(&edge, nullptr, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_ref_get_entity_properties_json(result, &edge, nullptr, &error),
                ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_ref_list_count(&edge, nullptr, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_ref_map_count(&edge, nullptr, &error), ZYX_DRIVER_INVALID_ARGUMENT);

    zyx_driver_result_free(result);
    zyx_driver_value_free(edgeValue, &error);
    zyx_driver_value_free(relsValue, &error);
    zyx_driver_params_free(params, &error);
}

TEST_F(DriverAbiNestedValuesTest, RawVectorValueRefsExposeListAccessors) {
    RegisteredResultOwner owner;

    const size_t floatSlot = appendValueRefBuffer(&owner.result, std::vector<float>{1.25f, 2.5f});
    zyx_driver_value_ref_t floatRef = makeValueRef(&owner.result, floatSlot);

    uint32_t count = 0;
    ASSERT_EQ(zyx_driver_value_ref_list_count(&floatRef, &count, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(count, 2u);
    EXPECT_EQ(zyx_driver_value_ref_type(&floatRef), ZYX_DRIVER_VALUE_LIST);

    double numericValue = 0.0;
    ASSERT_EQ(zyx_driver_value_ref_list_get_double(&floatRef, 0, &numericValue, &error), ZYX_DRIVER_OK);
    EXPECT_DOUBLE_EQ(numericValue, 1.25);
    expectError(zyx_driver_value_ref_list_get_double(&floatRef, 9, &numericValue, &error), ZYX_DRIVER_OUT_OF_RANGE);

    zyx_driver_value_ref_t numericItem{};
    ASSERT_EQ(zyx_driver_value_ref_list_get(&floatRef, 1, &numericItem, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(zyx_driver_value_ref_type(&numericItem), ZYX_DRIVER_VALUE_DOUBLE);
    expectError(zyx_driver_value_ref_list_get(&floatRef, 9, &numericItem, &error), ZYX_DRIVER_OUT_OF_RANGE);

    const size_t stringSlot =
            appendValueRefBuffer(&owner.result, std::vector<std::string>{"alpha", "beta"});
    zyx_driver_value_ref_t stringRef = makeValueRef(&owner.result, stringSlot);
    ASSERT_EQ(zyx_driver_value_ref_list_count(&stringRef, &count, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(count, 2u);

    const char *text = nullptr;
    ASSERT_EQ(zyx_driver_value_ref_list_get_string(nullptr, &stringRef, 0, &text, &error), ZYX_DRIVER_OK);
    ASSERT_NE(text, nullptr);
    EXPECT_STREQ(text, "alpha");
    expectError(zyx_driver_value_ref_list_get_string(nullptr, &stringRef, 9, &text, &error),
                ZYX_DRIVER_OUT_OF_RANGE);

    zyx_driver_value_ref_t stringItem{};
    ASSERT_EQ(zyx_driver_value_ref_list_get(&stringRef, 1, &stringItem, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(zyx_driver_value_ref_type(&stringItem), ZYX_DRIVER_VALUE_STRING);
}

TEST_F(DriverAbiNestedValuesTest, MarkerMapResultValueTypeStaysMapWhileValueRefMaterializesEntity) {
    zyx_driver_params_t *params = nullptr;
    zyx_driver_value_t *nodeValue = nullptr;
    zyx_driver_result_t *result = nullptr;

    ASSERT_EQ(zyx_driver_params_create(&params, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_create(&nodeValue, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_set_string(nodeValue, "__zyx_driver_entity", "node", &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_set_int64(nodeValue, "id", 101, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_set_string(nodeValue, "label0", "NestedPerson", &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_set_string(nodeValue, "prop:name", "Ada", &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_params_set_value(params, "node", nodeValue, &error), ZYX_DRIVER_OK);

    ASSERT_EQ(zyx_driver_db_execute(db, "RETURN $node AS node", params, &result, &error), ZYX_DRIVER_OK)
        << (error != nullptr ? zyx_driver_error_message(error) : "");
    ASSERT_EQ(zyx_driver_result_next(result, &error), ZYX_DRIVER_ROW);

    EXPECT_EQ(zyx_driver_result_value_type(result, 0), ZYX_DRIVER_VALUE_MAP);

    int64_t nodeId = 0;
    expectError(zyx_driver_result_get_node_id(result, 0, &nodeId, &error), ZYX_DRIVER_TYPE_MISMATCH);

    zyx_driver_value_ref_t node{};
    ASSERT_EQ(zyx_driver_result_get_value(result, 0, &node, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(zyx_driver_value_ref_type(&node), ZYX_DRIVER_VALUE_NODE);
    ASSERT_EQ(zyx_driver_value_ref_get_node_id(&node, &nodeId, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(nodeId, 101);

    zyx_driver_result_free(result);
    zyx_driver_value_free(nodeValue, &error);
    zyx_driver_params_free(params, &error);
}


TEST_F(DriverAbiNestedValuesTest, EntityMapConversionUsesFallbacksForMalformedMarkers) {
    zyx_driver_params_t *params = nullptr;
    zyx_driver_value_t *nodeValue = nullptr;
    zyx_driver_value_t *edgeValue = nullptr;
    zyx_driver_result_t *result = nullptr;

    ASSERT_EQ(zyx_driver_params_create(&params, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_create(&nodeValue, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_set_string(nodeValue, "__zyx_driver_entity", "node", &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_set_string(nodeValue, "id", "not-int", &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_set_int64(nodeValue, "label0", 123, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_set_string(nodeValue, "prop:name", "Fallback", &error), ZYX_DRIVER_OK);

    ASSERT_EQ(zyx_driver_value_map_create(&edgeValue, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_set_string(edgeValue, "__zyx_driver_entity", "edge", &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_set_string(edgeValue, "source", "not-int", &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_set_bool(edgeValue, "target", true, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_set_int64(edgeValue, "type", 99, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_set_value(edgeValue, "prop:nested", nodeValue, &error), ZYX_DRIVER_OK);

    ASSERT_EQ(zyx_driver_params_set_value(params, "node", nodeValue, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_params_set_value(params, "edge", edgeValue, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_db_execute(db, "RETURN $node AS node, $edge AS edge", params, &result, &error),
              ZYX_DRIVER_OK)
        << (error != nullptr ? zyx_driver_error_message(error) : "");
    ASSERT_EQ(zyx_driver_result_next(result, &error), ZYX_DRIVER_ROW);

    zyx_driver_value_ref_t node{};
    zyx_driver_value_ref_t edge{};
    ASSERT_EQ(zyx_driver_result_get_value(result, 0, &node, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_result_get_value(result, 1, &edge, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(zyx_driver_value_ref_type(&node), ZYX_DRIVER_VALUE_NODE);
    EXPECT_EQ(zyx_driver_value_ref_type(&edge), ZYX_DRIVER_VALUE_EDGE);

    int64_t id = -1;
    ASSERT_EQ(zyx_driver_value_ref_get_node_id(&node, &id, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(id, 0);
    uint32_t labelCount = 1;
    ASSERT_EQ(zyx_driver_value_ref_get_node_label_count(&node, &labelCount, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(labelCount, 0u);

    int64_t source = -1;
    int64_t target = -1;
    ASSERT_EQ(zyx_driver_value_ref_get_edge_source_id(&edge, &source, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_ref_get_edge_target_id(&edge, &target, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(source, 0);
    EXPECT_EQ(target, 0);
    const char *edgeType = nullptr;
    ASSERT_EQ(zyx_driver_value_ref_get_edge_type(result, &edge, &edgeType, &error), ZYX_DRIVER_OK);
    ASSERT_NE(edgeType, nullptr);
    EXPECT_STREQ(edgeType, "");

    const char *propertiesJson = nullptr;
    ASSERT_EQ(zyx_driver_value_ref_get_entity_properties_json(result, &edge, &propertiesJson, &error), ZYX_DRIVER_OK);
    ASSERT_NE(propertiesJson, nullptr);
    const std::string json(propertiesJson);
    EXPECT_NE(json.find("\"nested\":null"), std::string::npos) << json;

    zyx_driver_result_free(result);
    zyx_driver_value_free(edgeValue, &error);
    zyx_driver_value_free(nodeValue, &error);
    zyx_driver_params_free(params, &error);
}

TEST_F(DriverAbiNestedValuesTest, ReadsNestedListByValuePath) {
    zyx_driver_params_t *params = nullptr;
    zyx_driver_value_t *rootValue = nullptr;
    zyx_driver_value_t *nestedValue = nullptr;
    zyx_driver_value_t *mapValue = nullptr;
    zyx_driver_result_t *result = nullptr;

    ASSERT_EQ(zyx_driver_params_create(&params, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_list_create(&rootValue, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_list_append_int64(rootValue, 1, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_list_create(&nestedValue, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_list_append_int64(nestedValue, 2, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_list_append_string(nestedValue, "two", &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_list_append_value(rootValue, nestedValue, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_create(&mapValue, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_set_string(mapValue, "name", "Ada", &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_set_bool(mapValue, "active", true, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_list_append_value(rootValue, mapValue, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_params_set_value(params, "value", rootValue, &error), ZYX_DRIVER_OK);

    ASSERT_EQ(zyx_driver_db_execute(db, "RETURN $value AS value", params, &result, &error), ZYX_DRIVER_OK)
        << (error != nullptr ? zyx_driver_error_message(error) : "");
    ASSERT_EQ(zyx_driver_result_next(result, &error), ZYX_DRIVER_ROW);

    zyx_driver_value_ref_t root{};
    ASSERT_EQ(zyx_driver_result_get_value(result, 0, &root, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(zyx_driver_value_ref_type(&root), ZYX_DRIVER_VALUE_LIST);

    uint32_t rootCount = 0;
    ASSERT_EQ(zyx_driver_value_ref_list_count(&root, &rootCount, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(rootCount, 3u);

    zyx_driver_value_ref_t nested{};
    ASSERT_EQ(zyx_driver_value_ref_list_get(&root, 1, &nested, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(zyx_driver_value_ref_type(&nested), ZYX_DRIVER_VALUE_LIST);

    int64_t intValue = 0;
    ASSERT_EQ(zyx_driver_value_ref_list_get_int64(&nested, 0, &intValue, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(intValue, 2);

    const char *stringValue = nullptr;
    ASSERT_EQ(zyx_driver_value_ref_list_get_string(result, &nested, 1, &stringValue, &error), ZYX_DRIVER_OK);
    ASSERT_NE(stringValue, nullptr);
    EXPECT_STREQ(stringValue, "two");

    zyx_driver_value_ref_t map{};
    ASSERT_EQ(zyx_driver_value_ref_list_get(&root, 2, &map, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(zyx_driver_value_ref_type(&map), ZYX_DRIVER_VALUE_MAP);

    bool active = false;
    ASSERT_EQ(zyx_driver_value_ref_map_get_bool(&map, "active", &active, &error), ZYX_DRIVER_OK);
    EXPECT_TRUE(active);

    zyx_driver_result_free(result);
    zyx_driver_value_free(mapValue, &error);
    zyx_driver_value_free(nestedValue, &error);
    zyx_driver_value_free(rootValue, &error);
    zyx_driver_params_free(params, &error);
}


TEST_F(DriverAbiNestedValuesTest, TraversesStringListParameterValueRef) {
    zyx_driver_params_t *params = nullptr;
    zyx_driver_result_t *result = nullptr;
    const char *names[] = {"Ada", "Grace"};

    ASSERT_EQ(zyx_driver_params_create(&params, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_params_set_string_list(params, "names", names, 2, &error), ZYX_DRIVER_OK);

    ASSERT_EQ(zyx_driver_db_execute(db, "RETURN $names", params, &result, &error), ZYX_DRIVER_OK)
        << (error != nullptr ? zyx_driver_error_message(error) : "");
    ASSERT_EQ(zyx_driver_result_next(result, &error), ZYX_DRIVER_ROW);

    zyx_driver_value_ref_t root{};
    ASSERT_EQ(zyx_driver_result_get_value(result, 0, &root, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(zyx_driver_value_ref_type(&root), ZYX_DRIVER_VALUE_LIST);

    uint32_t count = 0;
    ASSERT_EQ(zyx_driver_value_ref_list_count(&root, &count, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(count, 2u);

    const char *name = nullptr;
    ASSERT_EQ(zyx_driver_value_ref_list_get_string(result, &root, 0, &name, &error), ZYX_DRIVER_OK);
    ASSERT_NE(name, nullptr);
    EXPECT_STREQ(name, "Ada");
    ASSERT_EQ(zyx_driver_value_ref_list_get_string(result, &root, 1, &name, &error), ZYX_DRIVER_OK);
    ASSERT_NE(name, nullptr);
    EXPECT_STREQ(name, "Grace");

    zyx_driver_value_ref_t element{};
    ASSERT_EQ(zyx_driver_value_ref_list_get(&root, 1, &element, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(zyx_driver_value_ref_type(&element), ZYX_DRIVER_VALUE_STRING);

    const char *elementValue = nullptr;
    ASSERT_EQ(zyx_driver_value_ref_get_string(result, &element, &elementValue, &error), ZYX_DRIVER_OK);
    ASSERT_NE(elementValue, nullptr);
    EXPECT_STREQ(elementValue, "Grace");

    zyx_driver_result_free(result);
    zyx_driver_params_free(params, &error);
}

TEST_F(DriverAbiNestedValuesTest, TraversesFloatListParameterValueRef) {
    zyx_driver_params_t *params = nullptr;
    zyx_driver_result_t *result = nullptr;
    const float scores[] = {1.5F, 2.25F};

    ASSERT_EQ(zyx_driver_params_create(&params, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_params_set_float_list(params, "scores", scores, 2, &error), ZYX_DRIVER_OK);

    ASSERT_EQ(zyx_driver_db_execute(db, "RETURN $scores", params, &result, &error), ZYX_DRIVER_OK)
        << (error != nullptr ? zyx_driver_error_message(error) : "");
    ASSERT_EQ(zyx_driver_result_next(result, &error), ZYX_DRIVER_ROW);

    zyx_driver_value_ref_t root{};
    ASSERT_EQ(zyx_driver_result_get_value(result, 0, &root, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(zyx_driver_value_ref_type(&root), ZYX_DRIVER_VALUE_LIST);

    uint32_t count = 0;
    ASSERT_EQ(zyx_driver_value_ref_list_count(&root, &count, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(count, 2u);

    double score = 0.0;
    ASSERT_EQ(zyx_driver_value_ref_list_get_double(&root, 0, &score, &error), ZYX_DRIVER_OK);
    EXPECT_DOUBLE_EQ(score, 1.5);
    ASSERT_EQ(zyx_driver_value_ref_list_get_double(&root, 1, &score, &error), ZYX_DRIVER_OK);
    EXPECT_DOUBLE_EQ(score, 2.25);

    zyx_driver_value_ref_t element{};
    ASSERT_EQ(zyx_driver_value_ref_list_get(&root, 1, &element, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(zyx_driver_value_ref_type(&element), ZYX_DRIVER_VALUE_DOUBLE);

    double elementValue = 0.0;
    ASSERT_EQ(zyx_driver_value_ref_get_double(&element, &elementValue, &error), ZYX_DRIVER_OK);
    EXPECT_DOUBLE_EQ(elementValue, 2.25);

    zyx_driver_result_free(result);
    zyx_driver_params_free(params, &error);
}

TEST_F(DriverAbiNestedValuesTest, MapTraversalReportsMissingKeysAndTypeMismatch) {
    zyx_driver_params_t *params = nullptr;
    zyx_driver_value_t *rootValue = nullptr;
    zyx_driver_result_t *result = nullptr;

    ASSERT_EQ(zyx_driver_params_create(&params, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_create(&rootValue, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_set_string(rootValue, "name", "Ada", &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_set_int64(rootValue, "age", 37, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_params_set_value(params, "value", rootValue, &error), ZYX_DRIVER_OK);

    ASSERT_EQ(zyx_driver_db_execute(db, "RETURN $value AS value", params, &result, &error), ZYX_DRIVER_OK)
        << (error != nullptr ? zyx_driver_error_message(error) : "");
    ASSERT_EQ(zyx_driver_result_next(result, &error), ZYX_DRIVER_ROW);

    zyx_driver_value_ref_t root{};
    ASSERT_EQ(zyx_driver_result_get_value(result, 0, &root, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(zyx_driver_value_ref_type(&root), ZYX_DRIVER_VALUE_MAP);

    uint32_t count = 0;
    ASSERT_EQ(zyx_driver_value_ref_map_count(&root, &count, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(count, 2u);

    const char *key = nullptr;
    ASSERT_EQ(zyx_driver_value_ref_map_key(result, &root, 0, &key, &error), ZYX_DRIVER_OK);
    ASSERT_NE(key, nullptr);
    EXPECT_TRUE(std::string(key) == "name" || std::string(key) == "age");

    int64_t age = 0;
    ASSERT_EQ(zyx_driver_value_ref_map_get_int64(&root, "age", &age, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(age, 37);

    zyx_driver_value_ref_t name{};
    ASSERT_EQ(zyx_driver_value_ref_map_get(&root, "name", &name, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(zyx_driver_value_ref_type(&name), ZYX_DRIVER_VALUE_STRING);

    const char *nameValue = nullptr;
    ASSERT_EQ(zyx_driver_value_ref_get_string(result, &name, &nameValue, &error), ZYX_DRIVER_OK);
    ASSERT_NE(nameValue, nullptr);
    EXPECT_STREQ(nameValue, "Ada");

    expectError(zyx_driver_value_ref_map_get_int64(&root, "missing", &age, &error), ZYX_DRIVER_NOT_FOUND);
    expectError(zyx_driver_value_ref_map_get_int64(&root, "name", &age, &error), ZYX_DRIVER_TYPE_MISMATCH);
    expectError(zyx_driver_value_ref_map_key(result, &root, 2, &key, &error), ZYX_DRIVER_OUT_OF_RANGE);

    zyx_driver_result_free(result);
    zyx_driver_value_free(rootValue, &error);
    zyx_driver_params_free(params, &error);
}

TEST_F(DriverAbiNestedValuesTest, RejectsForgedValueRefsWithoutDereferencingCallerPointers) {
    zyx_driver_value_ref_t forged{1234, 5678, 1, 0};
    zyx_driver_value_ref_t missingCookie{1234, 0, 1, 0};
    zyx_driver_value_ref_t missingGeneration{1234, 5678, 0, 0};

    int64_t intValue = 0;
    expectError(zyx_driver_value_ref_get_int64(&forged, &intValue, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_ref_get_int64(&missingCookie, &intValue, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_ref_get_int64(&missingGeneration, &intValue, &error), ZYX_DRIVER_INVALID_ARGUMENT);

    zyx_driver_value_ref_t nested{};
    expectError(zyx_driver_value_ref_list_get(&forged, 0, &nested, &error), ZYX_DRIVER_INVALID_ARGUMENT);
}


TEST_F(DriverAbiNestedValuesTest, InvalidValueRefsFailBeforeNullResultStringOwners) {
    zyx_driver_value_ref_t empty{};
    zyx_driver_value_ref_t forged{1234, 5678, 1, 0};

    const char *stringValue = nullptr;
    const char *key = nullptr;
    zyx_driver_value_ref_t nested{};
    int64_t intValue = 0;
    uint32_t count = 0;

    expectError(zyx_driver_value_ref_get_string(nullptr, &empty, &stringValue, &error),
                ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_ref_list_get_string(nullptr, &empty, 0, &stringValue, &error),
                ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_ref_map_key(nullptr, &empty, 0, &key, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_ref_map_get_string(nullptr, &empty, "name", &stringValue, &error),
                ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_ref_map_get(&empty, "name", &nested, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_ref_get_node_label(nullptr, &empty, 0, &stringValue, &error),
                ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_ref_get_edge_type(nullptr, &empty, &stringValue, &error),
                ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_ref_get_entity_properties_json(nullptr, &empty, &stringValue, &error),
                ZYX_DRIVER_INVALID_ARGUMENT);

    EXPECT_EQ(zyx_driver_value_ref_get_string(nullptr, &forged, &stringValue, nullptr),
              ZYX_DRIVER_INVALID_ARGUMENT);
    EXPECT_EQ(zyx_driver_value_ref_list_get_int64(&forged, 0, &intValue, nullptr),
              ZYX_DRIVER_INVALID_ARGUMENT);
    EXPECT_EQ(zyx_driver_value_ref_map_count(&forged, &count, nullptr), ZYX_DRIVER_INVALID_ARGUMENT);
}

TEST_F(DriverAbiNestedValuesTest, ValueRefStringAccessorsUseOwnerWhenResultIsNull) {
    zyx_driver_params_t *params = nullptr;
    zyx_driver_value_t *mapValue = nullptr;
    zyx_driver_value_t *listValue = nullptr;
    zyx_driver_result_t *result = nullptr;

    ASSERT_EQ(zyx_driver_params_create(&params, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_create(&mapValue, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_set_string(mapValue, "name", "Ada", &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_list_create(&listValue, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_list_append_string(listValue, "graph", &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_set_value(mapValue, "tags", listValue, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_params_set_value(params, "value", mapValue, &error), ZYX_DRIVER_OK);

    ASSERT_EQ(zyx_driver_db_execute(db, "RETURN $value AS value", params, &result, &error), ZYX_DRIVER_OK)
        << (error != nullptr ? zyx_driver_error_message(error) : "");
    ASSERT_EQ(zyx_driver_result_next(result, &error), ZYX_DRIVER_ROW);

    zyx_driver_value_ref_t root{};
    ASSERT_EQ(zyx_driver_result_get_value(result, 0, &root, &error), ZYX_DRIVER_OK);

    const char *key = nullptr;
    ASSERT_EQ(zyx_driver_value_ref_map_key(nullptr, &root, 0, &key, &error), ZYX_DRIVER_OK);
    ASSERT_NE(key, nullptr);
    EXPECT_TRUE(std::string(key) == "name" || std::string(key) == "tags");

    const char *name = nullptr;
    ASSERT_EQ(zyx_driver_value_ref_map_get_string(nullptr, &root, "name", &name, &error), ZYX_DRIVER_OK);
    ASSERT_NE(name, nullptr);
    EXPECT_STREQ(name, "Ada");

    zyx_driver_value_ref_t tags{};
    ASSERT_EQ(zyx_driver_value_ref_map_get(&root, "tags", &tags, &error), ZYX_DRIVER_OK);
    const char *tag = nullptr;
    ASSERT_EQ(zyx_driver_value_ref_list_get_string(nullptr, &tags, 0, &tag, &error), ZYX_DRIVER_OK);
    ASSERT_NE(tag, nullptr);
    EXPECT_STREQ(tag, "graph");

    zyx_driver_value_ref_t nameRef{};
    ASSERT_EQ(zyx_driver_value_ref_map_get(&root, "name", &nameRef, &error), ZYX_DRIVER_OK);
    const char *nameFromRef = nullptr;
    ASSERT_EQ(zyx_driver_value_ref_get_string(nullptr, &nameRef, &nameFromRef, &error), ZYX_DRIVER_OK);
    ASSERT_NE(nameFromRef, nullptr);
    EXPECT_STREQ(nameFromRef, "Ada");

    zyx_driver_result_free(result);
    zyx_driver_value_free(listValue, &error);
    zyx_driver_value_free(mapValue, &error);
    zyx_driver_params_free(params, &error);
}

TEST_F(DriverAbiNestedValuesTest, RejectsInvalidSlotsAndFreedValueRefOwners) {
    zyx_driver_params_t *params = nullptr;
    zyx_driver_value_t *listValue = nullptr;
    zyx_driver_result_t *result = nullptr;

    ASSERT_EQ(zyx_driver_params_create(&params, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_list_create(&listValue, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_list_append_int64(listValue, 1, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_params_set_value(params, "value", listValue, &error), ZYX_DRIVER_OK);

    ASSERT_EQ(zyx_driver_db_execute(db, "RETURN $value AS value", params, &result, &error), ZYX_DRIVER_OK)
        << (error != nullptr ? zyx_driver_error_message(error) : "");
    ASSERT_EQ(zyx_driver_result_next(result, &error), ZYX_DRIVER_ROW);

    zyx_driver_value_ref_t ref{};
    ASSERT_EQ(zyx_driver_result_get_value(result, 0, &ref, &error), ZYX_DRIVER_OK);
    zyx_driver_value_ref_t invalidSlot = ref;
    invalidSlot.slot = std::numeric_limits<uint64_t>::max();

    int64_t intValue = 0;
    expectError(zyx_driver_value_ref_get_int64(&invalidSlot, &intValue, &error), ZYX_DRIVER_INVALID_ARGUMENT);

    zyx_driver_result_free(result);
    result = nullptr;
    EXPECT_EQ(zyx_driver_value_ref_type(&ref), ZYX_DRIVER_VALUE_NULL);
    expectError(zyx_driver_value_ref_list_get_int64(&ref, 0, &intValue, &error), ZYX_DRIVER_INVALID_ARGUMENT);

    zyx_driver_value_free(listValue, &error);
    zyx_driver_params_free(params, &error);
}

TEST_F(DriverAbiNestedValuesTest, RejectsValueRefsAfterCursorAdvance) {
    zyx_driver_params_t *params = nullptr;
    zyx_driver_value_t *firstValue = nullptr;
    zyx_driver_value_t *secondValue = nullptr;
    zyx_driver_result_t *result = nullptr;

    ASSERT_EQ(zyx_driver_params_create(&params, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_list_create(&firstValue, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_list_append_int64(firstValue, 1, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_list_create(&secondValue, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_list_append_int64(secondValue, 2, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_params_set_value(params, "first", firstValue, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_params_set_value(params, "second", secondValue, &error), ZYX_DRIVER_OK);

    ASSERT_EQ(zyx_driver_db_execute(db, "RETURN $first AS value UNION ALL RETURN $second AS value", params, &result, &error),
              ZYX_DRIVER_OK)
        << (error != nullptr ? zyx_driver_error_message(error) : "");
    ASSERT_EQ(zyx_driver_result_next(result, &error), ZYX_DRIVER_ROW);

    zyx_driver_value_ref_t stale{};
    ASSERT_EQ(zyx_driver_result_get_value(result, 0, &stale, &error), ZYX_DRIVER_OK);
    uint32_t count = 0;
    ASSERT_EQ(zyx_driver_value_ref_list_count(&stale, &count, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(count, 1u);

    ASSERT_EQ(zyx_driver_result_next(result, &error), ZYX_DRIVER_ROW);
    expectError(zyx_driver_value_ref_list_count(&stale, &count, &error), ZYX_DRIVER_INVALID_ARGUMENT);

    zyx_driver_result_free(result);
    zyx_driver_value_free(secondValue, &error);
    zyx_driver_value_free(firstValue, &error);
    zyx_driver_params_free(params, &error);
}

TEST_F(DriverAbiNestedValuesTest, TraversesDriverStringAndFloatListRefsFromProperties) {
    zyx_driver_params_t *params = nullptr;
    zyx_driver_result_t *result = nullptr;

    ASSERT_EQ(zyx_driver_params_create(&params, &error), ZYX_DRIVER_OK);
    const char *tags[] = {"graph", "abi"};
    const float embedding[] = {0.25F, 1.5F};
    ASSERT_EQ(zyx_driver_params_set_string_list(params, "tags", tags, 2, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_params_set_float_list(params, "embedding", embedding, 2, &error), ZYX_DRIVER_OK);

    ASSERT_EQ(zyx_driver_db_execute(db, "CREATE (n:NestedVector {tags:$tags, embedding:$embedding})", params, &result,
                                    &error),
              ZYX_DRIVER_OK)
        << (error != nullptr ? zyx_driver_error_message(error) : "");
    zyx_driver_result_free(result);
    result = nullptr;

    ASSERT_EQ(zyx_driver_db_execute(db, "MATCH (n:NestedVector) RETURN n.tags AS tags, n.embedding AS embedding",
                                    nullptr, &result, &error),
              ZYX_DRIVER_OK)
        << (error != nullptr ? zyx_driver_error_message(error) : "");
    ASSERT_EQ(zyx_driver_result_next(result, &error), ZYX_DRIVER_ROW);

    zyx_driver_value_ref_t tagsRef{};
    zyx_driver_value_ref_t embeddingRef{};
    ASSERT_EQ(zyx_driver_result_get_value(result, 0, &tagsRef, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_result_get_value(result, 1, &embeddingRef, &error), ZYX_DRIVER_OK);

    uint32_t count = 0;
    ASSERT_EQ(zyx_driver_value_ref_list_count(&tagsRef, &count, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(count, 2u);
    ASSERT_EQ(zyx_driver_value_ref_list_count(&embeddingRef, &count, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(count, 2u);

    const char *tag = nullptr;
    ASSERT_EQ(zyx_driver_value_ref_list_get_string(nullptr, &tagsRef, 1, &tag, &error), ZYX_DRIVER_OK);
    ASSERT_NE(tag, nullptr);
    EXPECT_STREQ(tag, "abi");

    zyx_driver_value_ref_t tagElement{};
    ASSERT_EQ(zyx_driver_value_ref_list_get(&tagsRef, 0, &tagElement, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(zyx_driver_value_ref_type(&tagElement), ZYX_DRIVER_VALUE_STRING);

    double score = 0.0;
    ASSERT_EQ(zyx_driver_value_ref_list_get_double(&embeddingRef, 1, &score, &error), ZYX_DRIVER_OK);
    EXPECT_DOUBLE_EQ(score, 1.5);

    zyx_driver_value_ref_t scoreElement{};
    ASSERT_EQ(zyx_driver_value_ref_list_get(&embeddingRef, 0, &scoreElement, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(zyx_driver_value_ref_type(&scoreElement), ZYX_DRIVER_VALUE_DOUBLE);
    ASSERT_EQ(zyx_driver_value_ref_get_double(&scoreElement, &score, &error), ZYX_DRIVER_OK);
    EXPECT_DOUBLE_EQ(score, 0.25);

    expectError(zyx_driver_value_ref_list_get_string(nullptr, &tagsRef, 2, &tag, &error), ZYX_DRIVER_OUT_OF_RANGE);
    expectError(zyx_driver_value_ref_list_get_double(&embeddingRef, 2, &score, &error), ZYX_DRIVER_OUT_OF_RANGE);

    zyx_driver_result_free(result);
    zyx_driver_params_free(params, &error);
}


TEST_F(DriverAbiNestedValuesTest, ValueRefScalarTraversalCoversNullAndEntityTypeNames) {
    zyx_driver_params_t *params = nullptr;
    zyx_driver_value_t *listValue = nullptr;
    zyx_driver_value_t *mapValue = nullptr;
    zyx_driver_value_t *nodeValue = nullptr;
    zyx_driver_value_t *edgeValue = nullptr;
    zyx_driver_result_t *result = nullptr;

    ASSERT_EQ(zyx_driver_params_create(&params, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_list_create(&listValue, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_list_append_null(listValue, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_create(&mapValue, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_set_null(mapValue, "missing", &error), ZYX_DRIVER_OK);

    ASSERT_EQ(zyx_driver_value_map_create(&nodeValue, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_set_string(nodeValue, "__zyx_driver_entity", "node", &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_set_int64(nodeValue, "id", 77, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_set_string(nodeValue, "label0", "EntityName", &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_set_value(mapValue, "node", nodeValue, &error), ZYX_DRIVER_OK);

    ASSERT_EQ(zyx_driver_value_map_create(&edgeValue, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_set_string(edgeValue, "__zyx_driver_entity", "edge", &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_set_int64(edgeValue, "id", 88, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_set_string(edgeValue, "type", "ENTITY_NAME", &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_list_append_value(listValue, edgeValue, &error), ZYX_DRIVER_OK);

    ASSERT_EQ(zyx_driver_params_set_value(params, "list", listValue, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_params_set_value(params, "map", mapValue, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_db_execute(db, "RETURN $list AS list, $map AS map", params, &result, &error),
              ZYX_DRIVER_OK)
        << (error != nullptr ? zyx_driver_error_message(error) : "");
    ASSERT_EQ(zyx_driver_result_next(result, &error), ZYX_DRIVER_ROW);

    zyx_driver_value_ref_t list{};
    zyx_driver_value_ref_t map{};
    ASSERT_EQ(zyx_driver_result_get_value(result, 0, &list, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_result_get_value(result, 1, &map, &error), ZYX_DRIVER_OK);

    int64_t intValue = 0;
    double doubleValue = 0.0;
    bool boolValue = false;
    const char *stringValue = nullptr;

    expectError(zyx_driver_value_ref_list_get_int64(&list, 0, &intValue, &error), ZYX_DRIVER_TYPE_MISMATCH);
    expectError(zyx_driver_value_ref_list_get_bool(&list, 0, &boolValue, &error), ZYX_DRIVER_TYPE_MISMATCH);
    expectError(zyx_driver_value_ref_list_get_double(&list, 0, &doubleValue, &error), ZYX_DRIVER_TYPE_MISMATCH);
    expectError(zyx_driver_value_ref_list_get_string(result, &list, 0, &stringValue, &error),
                ZYX_DRIVER_TYPE_MISMATCH);
    expectError(zyx_driver_value_ref_map_get_int64(&map, "missing", &intValue, &error),
                ZYX_DRIVER_TYPE_MISMATCH);
    expectError(zyx_driver_value_ref_map_get_bool(&map, "missing", &boolValue, &error),
                ZYX_DRIVER_TYPE_MISMATCH);
    expectError(zyx_driver_value_ref_map_get_double(&map, "missing", &doubleValue, &error),
                ZYX_DRIVER_TYPE_MISMATCH);
    expectError(zyx_driver_value_ref_map_get_string(result, &map, "missing", &stringValue, &error),
                ZYX_DRIVER_TYPE_MISMATCH);

    zyx_driver_value_ref_t node{};
    zyx_driver_value_ref_t edge{};
    ASSERT_EQ(zyx_driver_value_ref_map_get(&map, "node", &node, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_ref_list_get(&list, 1, &edge, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(zyx_driver_value_ref_type(&node), ZYX_DRIVER_VALUE_NODE);
    EXPECT_EQ(zyx_driver_value_ref_type(&edge), ZYX_DRIVER_VALUE_EDGE);

    zyx_driver_value_ref_t nodeEntity{};
    zyx_driver_value_ref_t edgeEntity{};
    ASSERT_EQ(zyx_driver_value_ref_map_get(&map, "node", &nodeEntity, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_ref_list_get(&list, 1, &edgeEntity, &error), ZYX_DRIVER_OK);
    expectError(zyx_driver_value_ref_get_string(result, &nodeEntity, &stringValue, &error),
                ZYX_DRIVER_TYPE_MISMATCH);
    expectError(zyx_driver_value_ref_get_string(result, &edgeEntity, &stringValue, &error),
                ZYX_DRIVER_TYPE_MISMATCH);
    uint32_t count = 0;
    ASSERT_EQ(zyx_driver_value_ref_map_count(&nodeEntity, &count, &error), ZYX_DRIVER_OK);
    EXPECT_GT(count, 0u);
    expectError(zyx_driver_value_ref_list_count(&edgeEntity, &count, &error), ZYX_DRIVER_TYPE_MISMATCH);

    zyx_driver_result_free(result);
    zyx_driver_value_free(edgeValue, &error);
    zyx_driver_value_free(nodeValue, &error);
    zyx_driver_value_free(mapValue, &error);
    zyx_driver_value_free(listValue, &error);
    zyx_driver_params_free(params, &error);
}

TEST_F(DriverAbiNestedValuesTest, ReportsTypeMismatchAndOutOfRangeForInvalidTraversal) {
    zyx_driver_params_t *params = nullptr;
    zyx_driver_value_t *listValue = nullptr;
    zyx_driver_value_t *mapValue = nullptr;
    zyx_driver_result_t *result = nullptr;

    ASSERT_EQ(zyx_driver_params_create(&params, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_list_create(&listValue, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_list_append_int64(listValue, 1, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_list_append_bool(listValue, true, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_list_append_double(listValue, 2.5, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_create(&mapValue, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_set_string(mapValue, "name", "Ada", &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_set_double(mapValue, "score", 9.5, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_value_map_set_bool(mapValue, "active", true, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_params_set_value(params, "list", listValue, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_params_set_value(params, "map", mapValue, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_params_set_int64(params, "scalar", 42, &error), ZYX_DRIVER_OK);

    ASSERT_EQ(zyx_driver_db_execute(db, "RETURN $list AS list, $map AS map, $scalar AS scalar", params, &result, &error),
              ZYX_DRIVER_OK)
        << (error != nullptr ? zyx_driver_error_message(error) : "");
    ASSERT_EQ(zyx_driver_result_next(result, &error), ZYX_DRIVER_ROW);

    zyx_driver_value_ref_t list{};
    zyx_driver_value_ref_t map{};
    zyx_driver_value_ref_t scalar{};
    ASSERT_EQ(zyx_driver_result_get_value(result, 0, &list, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_result_get_value(result, 1, &map, &error), ZYX_DRIVER_OK);
    ASSERT_EQ(zyx_driver_result_get_value(result, 2, &scalar, &error), ZYX_DRIVER_OK);

    uint32_t count = 0;
    zyx_driver_value_ref_t nested{};
    int64_t intValue = 0;
    bool boolValue = false;
    double doubleValue = 0.0;
    const char *stringValue = nullptr;
    const char *key = nullptr;

    expectError(zyx_driver_value_ref_get_int64(&scalar, nullptr, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_ref_get_double(&scalar, nullptr, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_ref_get_bool(&scalar, nullptr, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_ref_get_string(result, &scalar, nullptr, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_ref_list_get(&list, 0, nullptr, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_ref_map_get(&map, "name", nullptr, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_ref_map_key(result, &map, 0, nullptr, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_ref_map_get_int64(&map, "score", nullptr, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_ref_map_get_double(&map, "score", nullptr, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_ref_map_get_bool(&map, "active", nullptr, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_ref_map_get_string(result, &map, "name", nullptr, &error),
                ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_ref_list_get_int64(&list, 0, nullptr, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_ref_list_get_double(&list, 2, nullptr, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_ref_list_get_bool(&list, 1, nullptr, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_ref_list_get_string(result, &list, 0, nullptr, &error),
                ZYX_DRIVER_INVALID_ARGUMENT);

    ASSERT_EQ(zyx_driver_value_ref_get_int64(&scalar, &intValue, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(intValue, 42);
    ASSERT_EQ(zyx_driver_value_ref_list_get_bool(&list, 1, &boolValue, &error), ZYX_DRIVER_OK);
    EXPECT_TRUE(boolValue);
    ASSERT_EQ(zyx_driver_value_ref_list_get_double(&list, 2, &doubleValue, &error), ZYX_DRIVER_OK);
    EXPECT_DOUBLE_EQ(doubleValue, 2.5);
    ASSERT_EQ(zyx_driver_value_ref_map_get_double(&map, "score", &doubleValue, &error), ZYX_DRIVER_OK);
    EXPECT_DOUBLE_EQ(doubleValue, 9.5);
    ASSERT_EQ(zyx_driver_value_ref_map_get_bool(&map, "active", &boolValue, &error), ZYX_DRIVER_OK);
    EXPECT_TRUE(boolValue);

    expectError(zyx_driver_value_ref_list_count(&scalar, &count, &error), ZYX_DRIVER_TYPE_MISMATCH);
    expectError(zyx_driver_value_ref_map_count(&scalar, &count, &error), ZYX_DRIVER_TYPE_MISMATCH);
    expectError(zyx_driver_value_ref_list_get(&list, 3, &nested, &error), ZYX_DRIVER_OUT_OF_RANGE);
    expectError(zyx_driver_value_ref_list_get_bool(&list, 0, &boolValue, &error), ZYX_DRIVER_TYPE_MISMATCH);
    expectError(zyx_driver_value_ref_list_get_double(&list, 1, &doubleValue, &error), ZYX_DRIVER_TYPE_MISMATCH);
    expectError(zyx_driver_value_ref_map_get_int64(&map, nullptr, &intValue, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_ref_map_get_double(&map, "name", &doubleValue, &error), ZYX_DRIVER_TYPE_MISMATCH);
    expectError(zyx_driver_value_ref_get_bool(&scalar, &boolValue, &error), ZYX_DRIVER_TYPE_MISMATCH);
    expectError(zyx_driver_value_ref_get_double(&scalar, &doubleValue, &error), ZYX_DRIVER_TYPE_MISMATCH);

    EXPECT_EQ(zyx_driver_value_ref_type(nullptr), ZYX_DRIVER_VALUE_NULL);
    zyx_driver_value_ref_t empty{};
    EXPECT_EQ(zyx_driver_value_ref_type(&empty), ZYX_DRIVER_VALUE_NULL);

    expectError(zyx_driver_value_ref_get_int64(nullptr, &intValue, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_ref_get_int64(&empty, &intValue, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_ref_get_double(&empty, &doubleValue, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_ref_get_bool(&empty, &boolValue, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_ref_get_string(nullptr, &empty, &stringValue, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_ref_list_count(nullptr, &count, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_ref_list_count(&empty, &count, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_ref_map_count(nullptr, &count, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_ref_map_count(&empty, &count, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_ref_list_get(nullptr, 0, &nested, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_ref_list_get(&empty, 0, &nested, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_ref_list_get_double(&empty, 0, &doubleValue, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_ref_list_get_bool(&empty, 0, &boolValue, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_ref_list_get_string(nullptr, &empty, 0, &stringValue, &error),
                ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_ref_map_key(nullptr, &empty, 0, &key, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_ref_map_get(nullptr, "name", &nested, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_ref_map_get(&empty, "name", &nested, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_ref_map_get_int64(&empty, "name", &intValue, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_ref_map_get_double(&empty, "name", &doubleValue, &error),
                ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_ref_map_get_bool(&empty, "name", &boolValue, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_ref_map_get_string(nullptr, &empty, "name", &stringValue, &error),
                ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_ref_get_node_id(&empty, &intValue, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_ref_get_node_label_count(&empty, &count, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_ref_get_node_label(nullptr, &empty, 0, &stringValue, &error),
                ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_ref_get_edge_id(&empty, &intValue, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_ref_get_edge_source_id(&empty, &intValue, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_ref_get_edge_target_id(&empty, &intValue, &error), ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_ref_get_edge_type(nullptr, &empty, &stringValue, &error),
                ZYX_DRIVER_INVALID_ARGUMENT);
    expectError(zyx_driver_value_ref_get_entity_properties_json(nullptr, &empty, &stringValue, &error),
                ZYX_DRIVER_INVALID_ARGUMENT);

    zyx_driver_result_free(result);
    zyx_driver_value_free(mapValue, &error);
    zyx_driver_value_free(listValue, &error);
    zyx_driver_params_free(params, &error);
}

TEST(DriverAbiValueRefRawPathsTest, DirectOwnerTraversesCompactVectorRefs) {
    RegisteredResultOwner owner;
    zyx_driver_error_t *error = nullptr;

    const size_t slot = appendValueRefBuffer(&owner.result, std::vector<float>{1.25F, 2.5F});
    zyx_driver_value_ref_t ref = makeValueRef(&owner.result, slot);

    EXPECT_EQ(zyx_driver_value_ref_type(&ref), ZYX_DRIVER_VALUE_LIST);

    uint32_t count = 0;
    ASSERT_EQ(zyx_driver_value_ref_list_count(&ref, &count, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(count, 2u);

    double value = 0.0;
    ASSERT_EQ(zyx_driver_value_ref_list_get_double(&ref, 0, &value, &error), ZYX_DRIVER_OK);
    EXPECT_DOUBLE_EQ(value, 1.25);
    ASSERT_EQ(zyx_driver_value_ref_list_get_double(&ref, 1, &value, &error), ZYX_DRIVER_OK);
    EXPECT_DOUBLE_EQ(value, 2.5);

    zyx_driver_value_ref_t element{};
    ASSERT_EQ(zyx_driver_value_ref_list_get(&ref, 1, &element, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(zyx_driver_value_ref_type(&element), ZYX_DRIVER_VALUE_DOUBLE);
    ASSERT_EQ(zyx_driver_value_ref_get_double(&element, &value, &error), ZYX_DRIVER_OK);
    EXPECT_DOUBLE_EQ(value, 2.5);

    int64_t intValue = 0;
    const char *stringValue = nullptr;
    expectLocalError(zyx_driver_value_ref_list_get_int64(&ref, 0, &intValue, &error),
                     ZYX_DRIVER_TYPE_MISMATCH,
                     error);
    expectLocalError(zyx_driver_value_ref_list_get_string(nullptr, &ref, 0, &stringValue, &error),
                     ZYX_DRIVER_TYPE_MISMATCH,
                     error);
    expectLocalError(zyx_driver_value_ref_list_get_double(&ref, 2, &value, &error),
                     ZYX_DRIVER_OUT_OF_RANGE,
                     error);

    element = {};
    expectLocalError(zyx_driver_value_ref_list_get(&ref, 2, &element, &error),
                     ZYX_DRIVER_OUT_OF_RANGE,
                     error);
}

TEST(DriverAbiValueRefRawPathsTest, DirectOwnerTraversesStringVectorRefsAndRejectsStaleRefs) {
    RegisteredResultOwner owner;
    zyx_driver_error_t *error = nullptr;

    const size_t slot = appendValueRefBuffer(&owner.result, std::vector<std::string>{"Ada", "Grace"});
    zyx_driver_value_ref_t ref = makeValueRef(&owner.result, slot);

    EXPECT_EQ(zyx_driver_value_ref_type(&ref), ZYX_DRIVER_VALUE_LIST);

    uint32_t count = 0;
    ASSERT_EQ(zyx_driver_value_ref_list_count(&ref, &count, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(count, 2u);

    const char *name = nullptr;
    ASSERT_EQ(zyx_driver_value_ref_list_get_string(nullptr, &ref, 1, &name, &error), ZYX_DRIVER_OK);
    ASSERT_NE(name, nullptr);
    EXPECT_STREQ(name, "Grace");

    zyx_driver_value_ref_t element{};
    ASSERT_EQ(zyx_driver_value_ref_list_get(&ref, 0, &element, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(zyx_driver_value_ref_type(&element), ZYX_DRIVER_VALUE_STRING);
    ASSERT_EQ(zyx_driver_value_ref_get_string(nullptr, &element, &name, &error), ZYX_DRIVER_OK);
    ASSERT_NE(name, nullptr);
    EXPECT_STREQ(name, "Ada");

    expectLocalError(zyx_driver_value_ref_list_get_string(nullptr, &ref, 2, &name, &error),
                     ZYX_DRIVER_OUT_OF_RANGE,
                     error);

    zyx_driver_value_ref_t outOfRange{};
    expectLocalError(zyx_driver_value_ref_list_get(&ref, 2, &outOfRange, &error),
                     ZYX_DRIVER_OUT_OF_RANGE,
                     error);

    zyx_driver_value_ref_t badSlot = ref;
    badSlot.slot = std::numeric_limits<uint64_t>::max();
    expectLocalError(zyx_driver_value_ref_list_count(&badSlot, &count, &error),
                     ZYX_DRIVER_INVALID_ARGUMENT,
                     error);

    zyx_driver_value_ref_t stale = ref;
    ++stale.owner_cookie;
    expectLocalError(zyx_driver_value_ref_list_count(&stale, &count, &error),
                     ZYX_DRIVER_INVALID_ARGUMENT,
                     error);

    zyx_driver_value_ref_t released = ref;
    unregisterResultHandle(&owner.result);
    expectLocalError(zyx_driver_value_ref_list_count(&released, &count, &error),
                     ZYX_DRIVER_INVALID_ARGUMENT,
                     error);
}

TEST(DriverAbiValueRefRawPathsTest, EntityMarkerRefsExposeJsonAndTypedMismatches) {
    RegisteredResultOwner owner;
    zyx_driver_error_t *error = nullptr;

    auto nodeMap = std::make_shared<zyx::ValueMap>();
    nodeMap->entries["__zyx_driver_entity"] = std::string("node");
    nodeMap->entries["id"] = int64_t{77};
    nodeMap->entries["label0"] = std::string("RawNode");
    nodeMap->entries["prop:embedding"] = std::vector<float>{1.0F, 2.5F};
    nodeMap->entries["prop:nullList"] = std::shared_ptr<zyx::ValueList>{};
    nodeMap->entries["prop:nullMap"] = std::shared_ptr<zyx::ValueMap>{};

    zyx_driver_value_ref_t nodeRef = makeValueRef(&owner.result, appendValueRefBuffer(&owner.result, nodeMap));
    ASSERT_EQ(zyx_driver_value_ref_type(&nodeRef), ZYX_DRIVER_VALUE_NODE);

    int64_t id = 0;
    ASSERT_EQ(zyx_driver_value_ref_get_node_id(&nodeRef, &id, &error), ZYX_DRIVER_OK);
    EXPECT_EQ(id, 77);

    const char *json = nullptr;
    ASSERT_EQ(zyx_driver_value_ref_get_entity_properties_json(nullptr, &nodeRef, &json, &error), ZYX_DRIVER_OK);
    ASSERT_NE(json, nullptr);
    const std::string text(json);
    EXPECT_NE(text.find("\"embedding\":[1,2.5]"), std::string::npos);
    EXPECT_NE(text.find("\"nullList\":null"), std::string::npos);
    EXPECT_NE(text.find("\"nullMap\":null"), std::string::npos);

    expectLocalError(zyx_driver_value_ref_get_edge_id(&nodeRef, &id, &error),
                     ZYX_DRIVER_TYPE_MISMATCH,
                     error);
    expectLocalError(zyx_driver_value_ref_get_entity_properties_json(nullptr,
                                                                    nullptr,
                                                                    &json,
                                                                    &error),
                     ZYX_DRIVER_INVALID_ARGUMENT,
                     error);
}

TEST(DriverAbiValueRefRawPathsTest, EntityPropertyJsonSerializesDirectGraphRefsAndRejectsScalars) {
    RegisteredResultOwner owner;
    zyx_driver_error_t *error = nullptr;

    auto nestedList = std::make_shared<zyx::ValueList>();
    nestedList->elements.emplace_back(int64_t{3});
    nestedList->elements.emplace_back(std::string("item"));

    auto nestedMap = std::make_shared<zyx::ValueMap>();
    nestedMap->entries["enabled"] = true;
    nestedMap->entries["ratio"] = 2.5;

    auto node = std::make_shared<zyx::Node>();
    node->id = 91;
    node->label = "DirectNode";
    node->properties["null"] = std::monostate{};
    node->properties["flag"] = false;
    node->properties["count"] = int64_t{5};
    node->properties["name"] = std::string("Ada\nLovelace");
    node->properties["tags"] = std::vector<std::string>{"math", "engine"};
    node->properties["embedding"] = std::vector<float>{1.5F, 2.25F};
    node->properties["list"] = nestedList;
    node->properties["map"] = nestedMap;

    zyx_driver_value_ref_t nodeRef = makeValueRef(&owner.result, appendValueRefBuffer(&owner.result, node));
    const char *json = nullptr;
    ASSERT_EQ(zyx_driver_value_ref_get_entity_properties_json(&owner.result, &nodeRef, &json, &error),
              ZYX_DRIVER_OK);
    ASSERT_NE(json, nullptr);
    const std::string text(json);
    EXPECT_NE(text.find("\"null\":null"), std::string::npos);
    EXPECT_NE(text.find("\"flag\":false"), std::string::npos);
    EXPECT_NE(text.find("\"count\":5"), std::string::npos);
    EXPECT_NE(text.find("\"name\":\"Ada\\nLovelace\""), std::string::npos);
    EXPECT_NE(text.find("\"embedding\":[1.5,2.25]"), std::string::npos);
    EXPECT_NE(text.find("\"tags\":[\"math\",\"engine\"]"), std::string::npos);
    EXPECT_NE(text.find("\"list\":[3,\"item\"]"), std::string::npos);
    EXPECT_NE(text.find("\"map\":"), std::string::npos);

    auto edge = std::make_shared<zyx::Edge>();
    edge->id = 92;
    edge->sourceId = 1;
    edge->targetId = 2;
    edge->type = "KNOWS";
    edge->properties["weight"] = 0.75;
    zyx_driver_value_ref_t edgeRef = makeValueRef(&owner.result, appendValueRefBuffer(&owner.result, edge));
    ASSERT_EQ(zyx_driver_value_ref_get_entity_properties_json(&owner.result, &edgeRef, &json, &error),
              ZYX_DRIVER_OK);
    ASSERT_NE(json, nullptr);
    EXPECT_NE(std::string(json).find("\"weight\":0.75"), std::string::npos);

    zyx_driver_value_ref_t scalarRef = makeValueRef(&owner.result, appendValueRefBuffer(&owner.result, int64_t{7}));
    expectLocalError(zyx_driver_value_ref_get_entity_properties_json(&owner.result,
                                                                    &scalarRef,
                                                                    &json,
                                                                    &error),
                     ZYX_DRIVER_TYPE_MISMATCH,
                     error);

    uint32_t labelCount = 0;
    expectLocalError(zyx_driver_value_ref_get_node_label_count(&scalarRef, &labelCount, &error),
                     ZYX_DRIVER_TYPE_MISMATCH,
                     error);
    expectLocalError(zyx_driver_value_ref_get_node_label(&owner.result, &scalarRef, 0, &json, &error),
                     ZYX_DRIVER_TYPE_MISMATCH,
                     error);
    expectLocalError(zyx_driver_value_ref_get_edge_type(&owner.result, &scalarRef, &json, &error),
                     ZYX_DRIVER_TYPE_MISMATCH,
                     error);
}

TEST(DriverAbiValueRefRawPathsTest, MapRefErrorsCoverOwnerBackedAccessors) {
    RegisteredResultOwner owner;
    zyx_driver_error_t *error = nullptr;

    const size_t scalarSlot = appendValueRefBuffer(&owner.result, int64_t{9});
    zyx_driver_value_ref_t scalarRef = makeValueRef(&owner.result, scalarSlot);

    const char *key = nullptr;
    zyx_driver_value_ref_t nested{};
    expectLocalError(zyx_driver_value_ref_map_key(nullptr, &scalarRef, 0, &key, &error),
                     ZYX_DRIVER_TYPE_MISMATCH,
                     error);
    expectLocalError(zyx_driver_value_ref_map_get(&scalarRef, "missing", &nested, &error),
                     ZYX_DRIVER_TYPE_MISMATCH,
                     error);
    expectLocalError(zyx_driver_value_ref_list_get(&scalarRef, 0, &nested, &error),
                     ZYX_DRIVER_TYPE_MISMATCH,
                     error);
}

TEST(DriverAbiValueRefRawPathsTest, DirectOwnerRejectsIncompleteOwnerTokens) {
    RegisteredResultOwner owner;

    const auto originalOwnerId = owner.result.value_ref_owner_id;
    const auto originalCookie = owner.result.value_ref_cookie;
    const auto originalGeneration = owner.result.value_ref_generation;

    owner.result.value_ref_cookie = 0;
    EXPECT_EQ(makeValueRef(&owner.result, 0).owner_id, 0U);

    owner.result.value_ref_cookie = originalCookie;
    owner.result.value_ref_generation = 0;
    EXPECT_EQ(makeValueRef(&owner.result, 0).owner_id, 0U);

    owner.result.value_ref_owner_id = originalOwnerId;
    owner.result.value_ref_cookie = originalCookie;
    owner.result.value_ref_generation = originalGeneration;
}
