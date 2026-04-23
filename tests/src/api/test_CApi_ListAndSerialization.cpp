/**
 * @file test_CApi_ListAndSerialization.cpp
 * @brief Focused branch tests for C API JSON/list/error fallback paths.
 */

#include <filesystem>
#include <gtest/gtest.h>
#include <string>

#include "zyx/zyx_c_api.h"

namespace fs = std::filesystem;

class CApiListAndSerializationTest : public ::testing::Test {
protected:
	std::string dbPath;
	ZYXDB_T *db = nullptr;

	void SetUp() override {
		auto tempDir = fs::temp_directory_path();
		dbPath = (tempDir / ("c_api_list_json_" + std::to_string(std::rand()))).string();
		std::error_code ec;
		if (fs::exists(dbPath))
			fs::remove_all(dbPath, ec);
		std::string walPath = dbPath + "-wal";
		if (fs::exists(walPath))
			fs::remove(walPath, ec);
		db = zyx_open(dbPath.c_str());
		ASSERT_NE(db, nullptr);
	}

	void TearDown() override {
		if (db) {
			zyx_close(db);
			db = nullptr;
		}
		std::error_code ec;
		if (fs::exists(dbPath))
			fs::remove_all(dbPath, ec);
		std::string walPath = dbPath + "-wal";
		if (fs::exists(walPath))
			fs::remove(walPath, ec);
	}

	static int findColumn(ZYXResult_T *res, const char *needle) {
		const int cols = zyx_result_column_count(res);
		for (int i = 0; i < cols; ++i) {
			std::string name = zyx_result_column_name(res, i);
			if (name == needle || name.find(needle) != std::string::npos) {
				return i;
			}
		}
		return -1;
	}
};

TEST_F(CApiListAndSerializationTest, PropsJsonEscapesControlCharsAndUsesComplexPlaceholder) {
	ZYXParams_T *props = zyx_params_create();
	ASSERT_NE(props, nullptr);

	std::string ctrl;
	ctrl.push_back('"');
	ctrl.push_back('\\');
	ctrl.push_back('\b');
	ctrl.push_back('\f');
	ctrl.push_back('\n');
	ctrl.push_back('\r');
	ctrl.push_back('\t');
	ctrl.push_back('\x01');

	zyx_params_set_string(props, "text", ctrl.c_str());
	ASSERT_TRUE(zyx_create_node(db, "EscCtrl", props));
	zyx_params_close(props);

	zyx_result_close(zyx_execute(db, "CREATE (n:EscComplex {arr:[1,2,3]})"));

	ZYXResult_T *res = zyx_execute(db, "MATCH (n:EscCtrl) RETURN n");
	ASSERT_NE(res, nullptr);
	ASSERT_TRUE(zyx_result_next(res));
	std::string json = zyx_result_get_props_json(res, 0);
	EXPECT_FALSE(json.empty());
	EXPECT_NE(json.find("\\\\b"), std::string::npos);
	zyx_result_close(res);

	res = zyx_execute(db, "MATCH (n:EscComplex) RETURN n");
	ASSERT_NE(res, nullptr);
	ASSERT_TRUE(zyx_result_next(res));
	json = zyx_result_get_props_json(res, 0);
	EXPECT_NE(json.find("<ComplexObject>"), std::string::npos);
	zyx_result_close(res);
}

TEST_F(CApiListAndSerializationTest, ListAndMapAccessCoverFloatStringAndBoundsBranches) {
	ZYXResult_T *res = zyx_execute(db, "RETURN ['1.25', '2.5'] AS vf, ['true','false'] AS vs, 7 AS n");
	ASSERT_NE(res, nullptr);
	ASSERT_TRUE(zyx_result_next(res));

	const int vf = findColumn(res, "vf");
	const int vs = findColumn(res, "vs");
	const int n = findColumn(res, "n");
	ASSERT_GE(vf, 0);
	ASSERT_GE(vs, 0);
	ASSERT_GE(n, 0);

	EXPECT_EQ(zyx_result_list_size(res, vf), 2);
	EXPECT_EQ(zyx_result_list_size(res, vs), 2);
	EXPECT_EQ(zyx_result_list_size(res, n), 0);

	EXPECT_EQ(zyx_result_list_get_type(res, vf, 0), ZYX_STRING);
	EXPECT_EQ(zyx_result_list_get_type(res, vs, 1), ZYX_STRING);
	EXPECT_EQ(zyx_result_list_get_type(res, n, 0), ZYX_NULL);

	EXPECT_EQ(zyx_result_list_get_int(res, vf, 0), 0);
	EXPECT_DOUBLE_EQ(zyx_result_list_get_double(res, vf, 1), 0.0);
	EXPECT_FALSE(zyx_result_list_get_bool(res, vs, 1));
	EXPECT_TRUE(zyx_result_list_get_bool(res, vs, 0));
	EXPECT_STREQ(zyx_result_list_get_string(res, vs, 1), "false");
	EXPECT_STREQ(zyx_result_list_get_string(res, vf, 1), "2.5");

	EXPECT_EQ(zyx_result_list_get_int(res, vf, -1), 0);
	EXPECT_DOUBLE_EQ(zyx_result_list_get_double(res, vf, 99), 0.0);
	EXPECT_FALSE(zyx_result_list_get_bool(res, vs, 99));
	EXPECT_STREQ(zyx_result_list_get_string(res, vs, -1), "");

	std::string vfJson = zyx_result_get_map_json(res, vf);
	std::string vsJson = zyx_result_get_map_json(res, vs);
	EXPECT_NE(vfJson.find("\"1.25\""), std::string::npos);
	EXPECT_NE(vsJson.find("\"true\""), std::string::npos);

	zyx_result_close(res);
}

TEST_F(CApiListAndSerializationTest, AccessorsFallbackOnInvalidColumnIndex) {
	ZYXResult_T *res = zyx_execute(db, "RETURN 1 AS x");
	ASSERT_NE(res, nullptr);
	ASSERT_TRUE(zyx_result_next(res));

	const int bad = 999;
	ZYXNode node{};
	ZYXEdge edge{};

	EXPECT_EQ(zyx_result_get_type(res, bad), ZYX_NULL);
	EXPECT_EQ(zyx_result_get_int(res, bad), 0);
	EXPECT_DOUBLE_EQ(zyx_result_get_double(res, bad), 0.0);
	EXPECT_FALSE(zyx_result_get_bool(res, bad));
	EXPECT_STREQ(zyx_result_get_string(res, bad), "");
	EXPECT_FALSE(zyx_result_get_node(res, bad, &node));
	EXPECT_FALSE(zyx_result_get_edge(res, bad, &edge));
	EXPECT_STREQ(zyx_result_get_props_json(res, bad), "{}");
	EXPECT_EQ(zyx_result_list_size(res, bad), 0);
	EXPECT_EQ(zyx_result_list_get_type(res, bad, 0), ZYX_NULL);
	EXPECT_EQ(zyx_result_list_get_int(res, bad, 0), 0);
	EXPECT_DOUBLE_EQ(zyx_result_list_get_double(res, bad, 0), 0.0);
	EXPECT_FALSE(zyx_result_list_get_bool(res, bad, 0));
	EXPECT_STREQ(zyx_result_list_get_string(res, bad, 0), "");
	EXPECT_STREQ(zyx_result_get_map_json(res, bad), "null");

	zyx_result_close(res);
}
