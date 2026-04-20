/**
 * @file test_CApi_TypeMatrix.cpp
 * @brief Type-matrix tests for C API result accessors.
 */

#include <filesystem>
#include <gtest/gtest.h>

#include <cstdlib>
#include <string>

#include "zyx/zyx_c_api.h"

namespace fs = std::filesystem;

class CApiTypeMatrixTest : public ::testing::Test {
protected:
	std::string dbPath;
	ZYXDB_T *db = nullptr;

	void SetUp() override {
		auto tempDir = fs::temp_directory_path();
		dbPath = (tempDir / ("c_api_type_matrix_" + std::to_string(std::rand()))).string();
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

TEST_F(CApiTypeMatrixTest, ResultGetTypeCoversNullBoolIntDoubleStringNodeEdgeList) {
	zyx_result_close(zyx_execute(db, "CREATE (a:TypeNode {id:1, s:'abc'})"));
	zyx_result_close(zyx_execute(db, "CREATE (b:TypeNode {id:2})"));
	zyx_result_close(zyx_execute(db, "MATCH (a:TypeNode {id:1}), (b:TypeNode {id:2}) CREATE (a)-[e:TYPE_EDGE {w:7}]->(b)"));

	ZYXResult_T *res = zyx_execute(
		db,
		"MATCH (a:TypeNode {id:1})-[e:TYPE_EDGE]->(b:TypeNode {id:2}) "
		"RETURN null AS n0, true AS b0, 7 AS i0, 2.5 AS d0, 'txt' AS s0, a AS na, e AS ea, labels(a) AS la"
	);
	ASSERT_NE(res, nullptr);
	ASSERT_TRUE(zyx_result_next(res));

	const int n0 = findColumn(res, "n0");
	const int b0 = findColumn(res, "b0");
	const int i0 = findColumn(res, "i0");
	const int d0 = findColumn(res, "d0");
	const int s0 = findColumn(res, "s0");
	const int na = findColumn(res, "na");
	const int ea = findColumn(res, "ea");
	const int la = findColumn(res, "la");

	ASSERT_GE(n0, 0);
	ASSERT_GE(b0, 0);
	ASSERT_GE(i0, 0);
	ASSERT_GE(d0, 0);
	ASSERT_GE(s0, 0);
	ASSERT_GE(na, 0);
	ASSERT_GE(ea, 0);
	ASSERT_GE(la, 0);

	EXPECT_EQ(zyx_result_get_type(res, n0), ZYX_NULL);
	EXPECT_EQ(zyx_result_get_type(res, b0), ZYX_BOOL);
	EXPECT_EQ(zyx_result_get_type(res, i0), ZYX_INT);
	EXPECT_EQ(zyx_result_get_type(res, d0), ZYX_DOUBLE);
	EXPECT_EQ(zyx_result_get_type(res, s0), ZYX_STRING);
	EXPECT_EQ(zyx_result_get_type(res, na), ZYX_NODE);
	EXPECT_EQ(zyx_result_get_type(res, ea), ZYX_EDGE);
	EXPECT_EQ(zyx_result_get_type(res, la), ZYX_LIST);

	EXPECT_TRUE(zyx_result_get_bool(res, b0));
	EXPECT_EQ(zyx_result_get_int(res, i0), 7);
	EXPECT_DOUBLE_EQ(zyx_result_get_double(res, d0), 2.5);
	EXPECT_STREQ(zyx_result_get_string(res, s0), "txt");

	ZYXNode node{};
	ZYXEdge edge{};
	EXPECT_TRUE(zyx_result_get_node(res, na, &node));
	EXPECT_TRUE(zyx_result_get_edge(res, ea, &edge));
	EXPECT_GT(node.id, 0);
	EXPECT_GT(edge.id, 0);

	EXPECT_GT(zyx_result_list_size(res, la), 0);
	const auto listType = zyx_result_list_get_type(res, la, 0);
	EXPECT_TRUE(listType == ZYX_STRING || listType == ZYX_DOUBLE);
	EXPECT_STRNE(zyx_result_list_get_string(res, la, 0), "");

	zyx_result_close(res);
}

TEST_F(CApiTypeMatrixTest, OpenIfExistsDirectoryPathReturnsNullAndSetsError) {
	zyx_close(db);
	db = nullptr;

	const auto dirPath = dbPath + "_dir";
	if (fs::exists(dirPath)) {
		fs::remove_all(dirPath);
	}
	ASSERT_TRUE(fs::create_directories(dirPath));

	ZYXDB_T *opened = zyx_open_if_exists(dirPath.c_str());
	EXPECT_EQ(opened, nullptr);
	EXPECT_STRNE(zyx_get_last_error(), "");

	fs::remove_all(dirPath);
}
