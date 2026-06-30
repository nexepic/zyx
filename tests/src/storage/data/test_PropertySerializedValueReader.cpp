/**
 * @file test_PropertySerializedValueReader.cpp
 * @brief Unit tests for the typed serialized property reader kernel.
 */

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "graph/core/Property.hpp"
#include "graph/core/Types.hpp"
#include "graph/utils/Serializer.hpp"
#include "storage/data/PropertySerializedValueReader.hpp"

namespace {

	std::string serializeValue(const graph::PropertyValue &value) {
		std::ostringstream stream;
		graph::utils::Serializer::serialize<graph::PropertyValue>(stream, value);
		return stream.str();
	}

	template<typename T>
	std::string serializePod(const T &value) {
		std::ostringstream stream;
		graph::utils::Serializer::writePOD(stream, value);
		return stream.str();
	}

	std::array<char, graph::Property::TOTAL_PROPERTY_SIZE> serializePropertyRecord(graph::Property property) {
		std::ostringstream stream;
		property.serialize(stream);
		const std::string payload = stream.str();
		std::array<char, graph::Property::TOTAL_PROPERTY_SIZE> buffer{};
		EXPECT_LE(payload.size(), buffer.size());
		std::memcpy(buffer.data(), payload.data(), payload.size());
		return buffer;
	}

} // namespace

TEST(PropertySerializedValueReaderTest, FallbackReadsListAndMapValues) {
	const graph::PropertyValue listValue(std::vector<graph::PropertyValue>{
			graph::PropertyValue(int64_t{7}),
			graph::PropertyValue("seven"),
	});
	const std::string listPayload = serializeValue(listValue);
	const char *listCursor = listPayload.data();
	const char *listEnd = listCursor + listPayload.size();

	const auto decodedList = graph::storage::readSerializedPropertyValue(listCursor, listEnd);
	ASSERT_TRUE(decodedList.has_value());
	EXPECT_EQ(decodedList->getType(), graph::PropertyType::LIST);
	EXPECT_EQ(listCursor, listEnd);
	const auto &list = std::get<std::vector<graph::PropertyValue>>(decodedList->getVariant());
	ASSERT_EQ(list.size(), 2U);
	EXPECT_EQ(list[0], graph::PropertyValue(int64_t{7}));
	EXPECT_EQ(list[1], graph::PropertyValue("seven"));

	const graph::PropertyValue mapValue(graph::PropertyValue::MapType{
			{"answer", graph::PropertyValue(int64_t{42})},
			{"label", graph::PropertyValue("life")},
	});
	const std::string mapPayload = serializeValue(mapValue);
	const char *mapCursor = mapPayload.data();
	const char *mapEnd = mapCursor + mapPayload.size();

	const auto decodedMap = graph::storage::readSerializedPropertyValue(mapCursor, mapEnd);
	ASSERT_TRUE(decodedMap.has_value());
	EXPECT_EQ(decodedMap->getType(), graph::PropertyType::MAP);
	EXPECT_EQ(mapCursor, mapEnd);
	const auto &map = std::get<graph::PropertyValue::MapType>(decodedMap->getVariant());
	EXPECT_EQ(map.at("answer"), graph::PropertyValue(int64_t{42}));
	EXPECT_EQ(map.at("label"), graph::PropertyValue("life"));
}

TEST(PropertySerializedValueReaderTest, UnknownAndCompositeTagsFailWithoutAdvancingCursor) {
	for (const auto type: {graph::PropertyType::UNKNOWN, graph::PropertyType::COMPOSITE}) {
		const std::string payload = serializePod(type);
		const char *cursor = payload.data();
		const char *end = cursor + payload.size();

		const auto decoded = graph::storage::readSerializedPropertyValue(cursor, end);
		EXPECT_FALSE(decoded.has_value());
		EXPECT_EQ(cursor, payload.data());
	}
}

TEST(PropertySerializedValueReaderTest, ReadSelectedPropertyValuesReturnsRequestedSubsetAndSkipsOthers) {
	graph::Property property(101, 202, graph::toUnderlying(graph::EntityType::Node));
	property.setProperties({
			{"name", graph::PropertyValue("neo")},
			{"age", graph::PropertyValue(int64_t{29})},
			{"tags", graph::PropertyValue(std::vector<graph::PropertyValue>{graph::PropertyValue("skip")})},
	});
	const auto buffer = serializePropertyRecord(property);

	const std::unordered_set<std::string> requestedKeys{"name", "missing"};
	const auto values = graph::storage::readSelectedPropertyValues(buffer.data(), requestedKeys);
	ASSERT_TRUE(values.has_value());
	ASSERT_EQ(values->size(), 1U);
	EXPECT_EQ(values->at("name"), graph::PropertyValue("neo"));
	EXPECT_FALSE(values->contains("age"));
	EXPECT_FALSE(values->contains("tags"));
}

TEST(PropertySerializedValueReaderTest, InactiveOrIdlessPropertyRecordIsRejected) {
	graph::Property inactive(301, 302, graph::toUnderlying(graph::EntityType::Node));
	inactive.setProperties({{"name", graph::PropertyValue("inactive")}});
	inactive.markInactive();
	const auto inactiveBuffer = serializePropertyRecord(inactive);

	const std::unordered_set<std::string> requestedKeys{"name"};
	EXPECT_FALSE(graph::storage::readSelectedPropertyValues(inactiveBuffer.data(), requestedKeys).has_value());

	graph::Property idless(0, 303, graph::toUnderlying(graph::EntityType::Node));
	idless.setProperties({{"name", graph::PropertyValue("idless")}});
	const auto idlessBuffer = serializePropertyRecord(idless);
	EXPECT_FALSE(graph::storage::readSelectedPropertyValues(idlessBuffer.data(), requestedKeys).has_value());
}
