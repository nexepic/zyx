/**
 * @file test_ScopedNodePropertyKey.cpp
 * @brief Tests for label-scoped node property index key encoding.
 */

#include <gtest/gtest.h>

#include "graph/storage/indexes/ScopedNodePropertyKey.hpp"

namespace {
	using graph::query::indexes::decodeScopedNodePropertyKey;
	using graph::query::indexes::makeScopedNodePropertyKey;
} // namespace

TEST(ScopedNodePropertyKeyTest, EncodesAndDecodesRoundTrip) {
	const auto key = makeScopedNodePropertyKey("User", "id");
	const auto decoded = decodeScopedNodePropertyKey(key);

	ASSERT_TRUE(decoded.has_value());
	EXPECT_EQ(decoded->first, "User");
	EXPECT_EQ(decoded->second, "id");
}

TEST(ScopedNodePropertyKeyTest, HandlesSeparatorTextInsideNames) {
	const auto key = makeScopedNodePropertyKey("Tenant__User", "profile__id");
	const auto decoded = decodeScopedNodePropertyKey(key);

	ASSERT_TRUE(decoded.has_value());
	EXPECT_EQ(decoded->first, "Tenant__User");
	EXPECT_EQ(decoded->second, "profile__id");
}

TEST(ScopedNodePropertyKeyTest, RejectsMalformedV2Keys) {
	EXPECT_FALSE(decodeScopedNodePropertyKey("__zyx_scoped_node_property_v2__").has_value());
	EXPECT_FALSE(decodeScopedNodePropertyKey("__zyx_scoped_node_property_v2__:id").has_value());
	EXPECT_FALSE(decodeScopedNodePropertyKey("__zyx_scoped_node_property_v2__4Userid").has_value());
	EXPECT_FALSE(decodeScopedNodePropertyKey("__zyx_scoped_node_property_v2__x:Userid").has_value());
	EXPECT_FALSE(decodeScopedNodePropertyKey("__zyx_scoped_node_property_v2__4x:Userid").has_value());
	EXPECT_FALSE(decodeScopedNodePropertyKey("__zyx_scoped_node_property_v2__0:id").has_value());
	EXPECT_FALSE(decodeScopedNodePropertyKey("__zyx_scoped_node_property_v2__4:User").has_value());
	EXPECT_FALSE(decodeScopedNodePropertyKey("__zyx_scoped_node_property_v2__99:Userid").has_value());
}

TEST(ScopedNodePropertyKeyTest, DecodesLegacyDevelopmentFormat) {
	const auto decoded = decodeScopedNodePropertyKey("__zyx_scoped_node_property__User__id");

	ASSERT_TRUE(decoded.has_value());
	EXPECT_EQ(decoded->first, "User");
	EXPECT_EQ(decoded->second, "id");
}

TEST(ScopedNodePropertyKeyTest, RejectsNonScopedAndMalformedLegacyKeys) {
	EXPECT_FALSE(decodeScopedNodePropertyKey("id").has_value());
	EXPECT_FALSE(decodeScopedNodePropertyKey("__zyx_scoped_node_property__").has_value());
	EXPECT_FALSE(decodeScopedNodePropertyKey("__zyx_scoped_node_property____id").has_value());
	EXPECT_FALSE(decodeScopedNodePropertyKey("__zyx_scoped_node_property__User__").has_value());
}
