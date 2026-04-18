/**
 * @file test_TemporalSerialization.cpp
 * @author ZYX Contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 **/

#include <gtest/gtest.h>
#include <sstream>
#include "graph/utils/Serializer.hpp"
#include "graph/core/PropertyTypes.hpp"

using namespace graph;
using namespace graph::utils;

TEST(TemporalSerializationTest, RoundTrip_TemporalDate) {
	std::stringstream ss;
	PropertyValue original(TemporalDate::fromISO("2024-06-15"));
	Serializer::serialize<PropertyValue>(ss, original);
	auto deserialized = Serializer::deserialize<PropertyValue>(ss);
	EXPECT_EQ(original, deserialized);
	EXPECT_EQ(std::get<TemporalDate>(original.getVariant()).epochDays,
	          std::get<TemporalDate>(deserialized.getVariant()).epochDays);
}

TEST(TemporalSerializationTest, RoundTrip_TemporalDateTime) {
	std::stringstream ss;
	PropertyValue original(TemporalDateTime::fromISO("2024-06-15T10:30:00"));
	Serializer::serialize<PropertyValue>(ss, original);
	auto deserialized = Serializer::deserialize<PropertyValue>(ss);
	EXPECT_EQ(original, deserialized);
}

TEST(TemporalSerializationTest, RoundTrip_TemporalDuration) {
	std::stringstream ss;
	PropertyValue original(TemporalDuration::fromISO("P1Y2M3DT4H5M6S"));
	Serializer::serialize<PropertyValue>(ss, original);
	auto deserialized = Serializer::deserialize<PropertyValue>(ss);
	EXPECT_EQ(original, deserialized);
	auto &orig = std::get<TemporalDuration>(original.getVariant());
	auto &deser = std::get<TemporalDuration>(deserialized.getVariant());
	EXPECT_EQ(orig.months, deser.months);
	EXPECT_EQ(orig.days, deser.days);
	EXPECT_EQ(orig.nanos, deser.nanos);
}

TEST(TemporalSerializationTest, RoundTrip_Map) {
	std::stringstream ss;
	PropertyValue::MapType map;
	map["name"] = PropertyValue(std::string("Alice"));
	map["age"] = PropertyValue(int64_t(30));
	map["score"] = PropertyValue(95.5);
	PropertyValue original(std::move(map));
	Serializer::serialize<PropertyValue>(ss, original);
	auto deserialized = Serializer::deserialize<PropertyValue>(ss);
	EXPECT_EQ(original, deserialized);
}

TEST(TemporalSerializationTest, RoundTrip_MapWithNestedValues) {
	std::stringstream ss;
	PropertyValue::MapType inner;
	inner["key"] = PropertyValue(std::string("value"));
	PropertyValue::MapType outer;
	outer["nested"] = PropertyValue(std::move(inner));
	outer["flag"] = PropertyValue(true);
	PropertyValue original(std::move(outer));
	Serializer::serialize<PropertyValue>(ss, original);
	auto deserialized = Serializer::deserialize<PropertyValue>(ss);
	EXPECT_EQ(original, deserialized);
}

TEST(TemporalSerializationTest, GetSerializedSize_Temporal) {
	PropertyValue date(TemporalDate::fromISO("2024-06-15"));
	PropertyValue dateTime(TemporalDateTime::fromISO("2024-06-15T10:30:00"));
	PropertyValue duration(TemporalDuration::fromISO("P1Y2M3DT4H5M6S"));
	EXPECT_GT(getSerializedSize(date), 0u);
	EXPECT_GT(getSerializedSize(dateTime), 0u);
	EXPECT_GT(getSerializedSize(duration), 0u);
}

TEST(TemporalSerializationTest, GetSerializedSize_Map) {
	PropertyValue::MapType map;
	map["x"] = PropertyValue(int64_t(1));
	PropertyValue mapVal(std::move(map));
	EXPECT_GT(getSerializedSize(mapVal), 0u);
}
