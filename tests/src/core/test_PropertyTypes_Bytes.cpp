/**
 * @file test_PropertyTypes_Bytes.cpp
 * @brief Binary PropertyValue construction, comparison, hashing, and sizing.
 */

#include <cstddef>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "graph/core/PropertyTypes.hpp"

using graph::PropertyType;
using graph::PropertyValue;

TEST(PropertyValueBytesTest, IdentifiesAndFormatsBinaryValuesWithoutTextConversion) {
	const PropertyValue value(PropertyValue::Bytes{std::byte{0x00}, std::byte{0x01}, std::byte{0xFE}, std::byte{0xFF}});

	EXPECT_EQ(value.getType(), PropertyType::BYTES);
	EXPECT_EQ(value.typeName(), "BYTES");
	EXPECT_EQ(value.toString(), "0x0001feff");
	EXPECT_EQ(value.getBytes(), (PropertyValue::Bytes{std::byte{0x00}, std::byte{0x01}, std::byte{0xFE}, std::byte{0xFF}}));
}

TEST(PropertyValueBytesTest, EmptyValuesRemainDistinctFromNullAndCompareByBytes) {
	const PropertyValue empty(PropertyValue::Bytes{});
	const PropertyValue one(PropertyValue::Bytes{std::byte{0x01}});
	const PropertyValue two(PropertyValue::Bytes{std::byte{0x02}});

	EXPECT_EQ(empty, PropertyValue(PropertyValue::Bytes{}));
	EXPECT_NE(empty, PropertyValue());
	EXPECT_LT(empty, one);
	EXPECT_LT(one, two);
	EXPECT_FALSE(two < one);
}

TEST(PropertyValueBytesTest, HashAndSizeIncludeBinaryContent) {
	const PropertyValue first(PropertyValue::Bytes{std::byte{0x10}, std::byte{0x20}});
	const PropertyValue same(PropertyValue::Bytes{std::byte{0x10}, std::byte{0x20}});
	const PropertyValue different(PropertyValue::Bytes{std::byte{0x10}, std::byte{0x21}});
	const graph::PropertyValueHash hash;

	EXPECT_EQ(hash(first), hash(same));
	EXPECT_NE(hash(first), hash(different));
	EXPECT_EQ(graph::property_utils::getPropertyValueSize(first), 2U + sizeof(size_t));
}

TEST(PropertyValueBytesTest, NestedBinaryValuesRemainTyped) {
	PropertyValue::MapType map;
	map.emplace("payload", PropertyValue(PropertyValue::Bytes{std::byte{0xA5}}));
	const PropertyValue value(std::vector<PropertyValue>{PropertyValue(std::move(map))});

	ASSERT_EQ(value.getType(), PropertyType::LIST);
	const auto &mapValue = value.getList().front();
	ASSERT_EQ(mapValue.getType(), PropertyType::MAP);
	EXPECT_EQ(mapValue.getMap().at("payload").getType(), PropertyType::BYTES);
}
