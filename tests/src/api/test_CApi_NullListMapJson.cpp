/**
 * @file test_CApi_NullListMapJson.cpp
 * @brief Branch coverage tests for CApi.cpp value_to_json null shared_ptr paths
 *        and list access out-of-bounds branches.
 */

#include <gtest/gtest.h>
#include <memory>
#include <sstream>
#include <string>
#include "zyx/value.hpp"

void value_to_json(std::ostringstream &oss, const zyx::Value &v);

TEST(CApiNullListMapJsonTest, ValueToJsonNullListPtr) {
	std::ostringstream oss;
	std::shared_ptr<zyx::ValueList> nullList;
	value_to_json(oss, zyx::Value(nullList));
	EXPECT_EQ(oss.str(), "null");
}

TEST(CApiNullListMapJsonTest, ValueToJsonNullMapPtr) {
	std::ostringstream oss;
	std::shared_ptr<zyx::ValueMap> nullMap;
	value_to_json(oss, zyx::Value(nullMap));
	EXPECT_EQ(oss.str(), "null");
}

TEST(CApiNullListMapJsonTest, ValueToJsonNonEmptyListPtr) {
	auto list = std::make_shared<zyx::ValueList>();
	list->elements.push_back(int64_t(42));
	list->elements.push_back(std::string("hello"));
	list->elements.push_back(std::monostate{});

	std::ostringstream oss;
	value_to_json(oss, zyx::Value(list));
	std::string json = oss.str();
	EXPECT_NE(json.find("42"), std::string::npos);
	EXPECT_NE(json.find("\"hello\""), std::string::npos);
	EXPECT_NE(json.find("null"), std::string::npos);
}

TEST(CApiNullListMapJsonTest, ValueToJsonNonEmptyMapPtr) {
	auto map = std::make_shared<zyx::ValueMap>();
	map->entries["key1"] = int64_t(99);
	map->entries["key2"] = std::string("val");
	map->entries["key3"] = true;
	map->entries["key4"] = 3.14;

	std::ostringstream oss;
	value_to_json(oss, zyx::Value(map));
	std::string json = oss.str();
	EXPECT_NE(json.find("99"), std::string::npos);
	EXPECT_NE(json.find("\"val\""), std::string::npos);
	EXPECT_NE(json.find("true"), std::string::npos);
	EXPECT_NE(json.find("3.14"), std::string::npos);
}

TEST(CApiNullListMapJsonTest, ValueToJsonNestedListInMap) {
	auto innerList = std::make_shared<zyx::ValueList>();
	innerList->elements.push_back(int64_t(1));
	innerList->elements.push_back(int64_t(2));

	auto map = std::make_shared<zyx::ValueMap>();
	map->entries["nested"] = innerList;

	std::ostringstream oss;
	value_to_json(oss, zyx::Value(map));
	std::string json = oss.str();
	EXPECT_NE(json.find("[1,2]"), std::string::npos);
}

TEST(CApiNullListMapJsonTest, ValueToJsonNestedMapInList) {
	auto innerMap = std::make_shared<zyx::ValueMap>();
	innerMap->entries["a"] = int64_t(10);

	auto list = std::make_shared<zyx::ValueList>();
	list->elements.push_back(innerMap);

	std::ostringstream oss;
	value_to_json(oss, zyx::Value(list));
	std::string json = oss.str();
	EXPECT_NE(json.find("\"a\":10"), std::string::npos);
}

TEST(CApiNullListMapJsonTest, ValueToJsonBoolFalse) {
	std::ostringstream oss;
	value_to_json(oss, zyx::Value(false));
	EXPECT_EQ(oss.str(), "false");
}

TEST(CApiNullListMapJsonTest, ValueToJsonEdgePtr) {
	auto edge = std::make_shared<zyx::Edge>();
	std::ostringstream oss;
	value_to_json(oss, zyx::Value(edge));
	EXPECT_NE(oss.str().find("<ComplexObject>"), std::string::npos);
}
