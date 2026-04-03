/**
 * @file test_CApi_InternalHelperBranches.cpp
 * @brief Direct helper coverage tests for CApi.cpp internal JSON helpers.
 */

#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "zyx/value.hpp"

std::string escape_json_string(const std::string &s);
std::string props_to_json(const std::unordered_map<std::string, zyx::Value> &props);
void value_to_json(std::ostringstream &oss, const zyx::Value &v);

TEST(CApiInternalHelperTest, EscapeJsonStringCoversAllControlCharacterBranches) {
	std::string raw;
	raw.push_back('"');
	raw.push_back('\\');
	raw.push_back('\b');
	raw.push_back('\f');
	raw.push_back('\n');
	raw.push_back('\r');
	raw.push_back('\t');
	raw.push_back('\x01');
	raw.push_back('A');

	const std::string escaped = escape_json_string(raw);

	EXPECT_NE(escaped.find("\\\""), std::string::npos);
	EXPECT_NE(escaped.find("\\\\"), std::string::npos);
	EXPECT_NE(escaped.find("\\b"), std::string::npos);
	EXPECT_NE(escaped.find("\\f"), std::string::npos);
	EXPECT_NE(escaped.find("\\n"), std::string::npos);
	EXPECT_NE(escaped.find("\\r"), std::string::npos);
	EXPECT_NE(escaped.find("\\t"), std::string::npos);
	EXPECT_NE(escaped.find("\\u0001"), std::string::npos);
	EXPECT_NE(escaped.find('A'), std::string::npos);
}

TEST(CApiInternalHelperTest, PropsToJsonCoversPrimitiveAndComplexBranches) {
	std::unordered_map<std::string, zyx::Value> props;
	props["n"] = std::monostate{};
	props["b"] = true;
	props["i"] = (int64_t) 42;
	props["d"] = 3.5;
	props["s"] = std::string("txt");
	props["complex"] = std::vector<float>{1.0F, 2.0F};

	const std::string json = props_to_json(props);

	EXPECT_NE(json.find("\"n\":null"), std::string::npos);
	EXPECT_NE(json.find("\"b\":true"), std::string::npos);
	EXPECT_NE(json.find("\"i\":42"), std::string::npos);
	EXPECT_NE(json.find("\"d\":3.5"), std::string::npos);
	EXPECT_NE(json.find("\"s\":\"txt\""), std::string::npos);
	EXPECT_NE(json.find("<ComplexObject>"), std::string::npos);
}

TEST(CApiInternalHelperTest, ValueToJsonCoversFloatStringAndComplexObjectBranches) {
	std::ostringstream floatOut;
	value_to_json(floatOut, zyx::Value(std::vector<float>{1.25F, 2.5F}));
	EXPECT_NE(floatOut.str().find("1.25"), std::string::npos);
	EXPECT_NE(floatOut.str().find("2.5"), std::string::npos);

	std::ostringstream strOut;
	value_to_json(strOut, zyx::Value(std::vector<std::string>{"a", "b\"c"}));
	EXPECT_NE(strOut.str().find("\"a\""), std::string::npos);
	EXPECT_NE(strOut.str().find("\\\""), std::string::npos);

	std::ostringstream complexOut;
	auto nodePtr = std::make_shared<zyx::Node>();
	value_to_json(complexOut, zyx::Value(nodePtr));
	EXPECT_NE(complexOut.str().find("<ComplexObject>"), std::string::npos);
}
