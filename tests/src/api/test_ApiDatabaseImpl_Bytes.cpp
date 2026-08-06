/**
 * @file test_ApiDatabaseImpl_Bytes.cpp
 * @brief Public C++ API binary Value round-trip tests.
 */

#include <cstddef>

#include <gtest/gtest.h>

#include "ApiTestFixture.hpp"

TEST_F(CppApiTest, BinaryValueRoundTripsThroughNativePropertyStorage) {
	const zyx::Bytes expected{0U, 1U, 0x7FU, 0x80U, 0xFFU};

	ASSERT_NO_THROW(db->createNode("BytesNode", {{"payload", expected}}));
	const auto result = db->execute("MATCH (n:BytesNode) RETURN n.payload");
	ASSERT_TRUE(result.isSuccess()) << result.getError();
	ASSERT_TRUE(result.hasNext());
	result.next();

	const zyx::Value value = result.get(0);
	ASSERT_TRUE(std::holds_alternative<zyx::Bytes>(value));
	EXPECT_EQ(std::get<zyx::Bytes>(value), expected);
}

TEST_F(CppApiTest, BinaryValueCanBeUsedInNestedPublicValues) {
	auto map = std::make_shared<zyx::ValueMap>();
	map->entries.emplace("payload", zyx::Bytes{0x00U, 0xFFU});

	ASSERT_NO_THROW(db->createNode("BytesNestedNode", {{"metadata", map}}));
	const auto result = db->execute("MATCH (n:BytesNestedNode) RETURN n.metadata");
	ASSERT_TRUE(result.isSuccess()) << result.getError();
	ASSERT_TRUE(result.hasNext());
	result.next();

	// Map values retain the established public API stringification policy, but
	// the binary payload must be represented safely and deterministically.
	const zyx::Value value = result.get(0);
	ASSERT_TRUE(std::holds_alternative<std::string>(value));
	EXPECT_NE(std::get<std::string>(value).find("0x00ff"), std::string::npos);
}
