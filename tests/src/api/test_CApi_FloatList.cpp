/**
 * @file test_CApi_FloatList.cpp
 * @brief Focused float-list branch coverage tests for C API list accessors.
 */

#include <cstdlib>
#include <filesystem>
#include <gtest/gtest.h>
#include <string>

#include "zyx/zyx_c_api.h"

namespace fs = std::filesystem;

class CApiFloatListTest : public ::testing::Test {
protected:
	std::string dbPath;
	ZYXDB_T *db = nullptr;

	void SetUp() override {
		dbPath = (fs::temp_directory_path() / ("c_api_float_list_" + std::to_string(std::rand()))).string();
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
			if (std::string(zyx_result_column_name(res, i)).find(needle) != std::string::npos) {
				return i;
			}
		}
		return -1;
	}
};

TEST_F(CApiFloatListTest, NumericLiteralListsUseStableStringListSemantics) {
	ZYXResult_T *res = zyx_execute(db, "RETURN [1.25, 2.5] AS vf");
	ASSERT_NE(res, nullptr);
	ASSERT_TRUE(zyx_result_next(res));

	const int vf = findColumn(res, "vf");
	ASSERT_GE(vf, 0);

	EXPECT_EQ(zyx_result_list_size(res, vf), 2);
	EXPECT_EQ(zyx_result_list_get_type(res, vf, 0), ZYX_STRING);
	EXPECT_EQ(zyx_result_list_get_int(res, vf, 1), 0);
	EXPECT_DOUBLE_EQ(zyx_result_list_get_double(res, vf, 1), 0.0);

	const std::string firstAsString = zyx_result_list_get_string(res, vf, 0);
	EXPECT_FALSE(firstAsString.empty());
	EXPECT_NE(firstAsString.find("1.25"), std::string::npos);

	zyx_result_close(res);
}
