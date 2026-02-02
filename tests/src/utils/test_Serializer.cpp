/**
 * @file test_Serializer.cpp
 * @author Nexepic
 * @date 2025/8/1
 *
 * @copyright Copyright (c) 2025 Nexepic
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
#include <limits>
#include <sstream>
#include <vector>
#include "graph/core/PropertyTypes.hpp"
#include "graph/utils/Serializer.hpp"

namespace graph::utils::test {

	class SerializerTest : public ::testing::Test {
	protected:
		// Helper to test a full serialize/deserialize round trip.
		template<typename T>
		void testRoundTrip(const T &value) {
			std::stringstream stream;
			Serializer::serialize(stream, value);
			T result = Serializer::deserialize<T>(stream);
			EXPECT_EQ(value, result);
		}
	};

	// Test serialization for all fundamental POD types used in the system.
	TEST_F(SerializerTest, RoundTrip_PODs) {
		testRoundTrip<bool>(true);
		testRoundTrip<bool>(false);
		testRoundTrip<int64_t>(42LL);
		testRoundTrip<int64_t>(-1234567890123LL);
		testRoundTrip<int64_t>(0);
		testRoundTrip<int64_t>(INT64_MAX);
		testRoundTrip<int64_t>(INT64_MIN);
		testRoundTrip<double>(3.14159);
		testRoundTrip<double>(0.0);
		testRoundTrip<double>(-2.718);
		testRoundTrip<uint64_t>(UINT64_MAX); // Used for string/vector sizes
	}

	// Test serialization for the specialized std::string type.
	TEST_F(SerializerTest, RoundTrip_String) {
		testRoundTrip<std::string>("Hello, World!");
		testRoundTrip<std::string>(""); // Empty string
		testRoundTrip<std::string>("你好世界"); // Unicode
		testRoundTrip<std::string>(std::string(1000, 'a')); // Long string
	}

	// Test the end-to-end serialization of the PropertyValue wrapper.
	TEST_F(SerializerTest, RoundTrip_PropertyValue) {
		std::stringstream stream;

		// Helper lambda for testing PropertyValue round trip
		auto testPropValue = [&](const PropertyValue &val) {
			stream.str(""); // Clear the stream
			stream.clear();
			Serializer::serialize<PropertyValue>(stream, val);
			PropertyValue result = Serializer::deserialize<PropertyValue>(stream);
			EXPECT_EQ(val, result);
		};

		// Test each type contained within PropertyValue
		testPropValue(PropertyValue(std::monostate{}));
		testPropValue(PropertyValue(true));
		testPropValue(PropertyValue(42));
		testPropValue(PropertyValue(3.14));
		testPropValue(PropertyValue("test string"));
	}

	// Test error conditions
	TEST_F(SerializerTest, ErrorOnInvalidTypeTag) {
		std::stringstream stream;
		// Write an invalid type tag that doesn't correspond to any known PropertyType.
		Serializer::serialize(stream, static_cast<PropertyType>(255));
		EXPECT_THROW(Serializer::deserialize<PropertyValue>(stream), std::runtime_error);
	}

	TEST_F(SerializerTest, ErrorOnInsufficientData) {
		std::stringstream stream;
		// Write only 4 bytes to the stream.
		stream.write("1234", 4);

		// Attempt to read an 8-byte int64_t.
		// The read operation itself won't throw.
		(void) Serializer::readPOD<int64_t>(stream);

		// Check gcount(). The number of bytes actually read must be less
		// than what we requested, which is a sign of an incomplete read.
		// The stream will also be in a failed state (either eof or fail).
		EXPECT_NE(static_cast<size_t>(stream.gcount()), sizeof(int64_t));
		EXPECT_TRUE(stream.fail() || stream.eof()); // A more robust check for read failure.
	}

	// Test PropertyValue with vector (LIST type)
	TEST_F(SerializerTest, PropertyValue_Vector) {
		std::vector<float> vec = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f};
		PropertyValue val(vec);

		std::stringstream stream;
		Serializer::serialize<PropertyValue>(stream, val);
		PropertyValue result = Serializer::deserialize<PropertyValue>(stream);

		EXPECT_EQ(val, result);

		// Verify the vector is correctly deserialized
		auto resultVec = result.getList();
		EXPECT_EQ(resultVec.size(), 5UL);
		EXPECT_FLOAT_EQ(resultVec[0], 1.0f);
		EXPECT_FLOAT_EQ(resultVec[4], 5.0f);
	}

	// Test PropertyValue with empty vector (covers line 116-119 and 148-150)
	TEST_F(SerializerTest, PropertyValue_EmptyVector) {
		std::vector<float> emptyVec;
		PropertyValue val(emptyVec);

		std::stringstream stream;
		Serializer::serialize<PropertyValue>(stream, val);
		PropertyValue result = Serializer::deserialize<PropertyValue>(stream);

		EXPECT_EQ(val, result);

		// Verify the vector is empty
		auto resultVec = result.getList();
		EXPECT_TRUE(resultVec.empty());
	}

	// Test PropertyValue getList() throws on non-list types (covers line 175-179)
	TEST_F(SerializerTest, PropertyValue_GetListNonListType) {
		// Test with bool
		PropertyValue boolVal(true);
		EXPECT_THROW(boolVal.getList(), std::runtime_error);

		// Test with int64_t
		PropertyValue intVal(42);
		EXPECT_THROW(intVal.getList(), std::runtime_error);

		// Test with double
		PropertyValue doubleVal(3.14);
		EXPECT_THROW(doubleVal.getList(), std::runtime_error);

		// Test with string
		PropertyValue stringVal("test");
		EXPECT_THROW(stringVal.getList(), std::runtime_error);

		// Test with null (std::monostate)
		PropertyValue nullVal;
		EXPECT_THROW(nullVal.getList(), std::runtime_error);
	}

	// Test various invalid type tags (covers line 153-154 default case)
	TEST_F(SerializerTest, ErrorOnVariousInvalidTypeTags) {
		auto testInvalidTag = [](uint8_t tagValue) {
			std::stringstream stream;
			stream.write(reinterpret_cast<const char*>(&tagValue), sizeof(tagValue));
			EXPECT_THROW(Serializer::deserialize<PropertyValue>(stream), std::runtime_error)
				<< "Should throw on invalid type tag: " << static_cast<int>(tagValue);
		};

		// Test type 0 (UNKNOWN/invalid)
		testInvalidTag(0);

		// Test type 7 (beyond defined types)
		testInvalidTag(7);

		// Test type 8 (way beyond defined types)
		testInvalidTag(8);

		// Test type 254 (one less than 255, also invalid)
		testInvalidTag(254);

		// Type 255 is already tested in ErrorOnInvalidTypeTag
	}

	// Test PropertyValue with large vector
	TEST_F(SerializerTest, PropertyValue_LargeVector) {
		std::vector<float> largeVec;
		for (int i = 0; i < 1000; ++i) {
			largeVec.push_back(static_cast<float>(i));
		}
		PropertyValue val(largeVec);

		std::stringstream stream;
		Serializer::serialize<PropertyValue>(stream, val);
		PropertyValue result = Serializer::deserialize<PropertyValue>(stream);

		EXPECT_EQ(val, result);

		// Verify the vector is correctly deserialized
		auto resultVec = result.getList();
		EXPECT_EQ(resultVec.size(), 1000UL);
		EXPECT_FLOAT_EQ(resultVec[0], 0.0f);
		EXPECT_FLOAT_EQ(resultVec[999], 999.0f);
	}

	// Test PropertyValue round-trip for all types
	TEST_F(SerializerTest, PropertyValue_RoundTripAllTypes) {
		std::stringstream stream;
		auto testPropValue = [&](const PropertyValue &val) {
			stream.str("");
			stream.clear();
			Serializer::serialize<PropertyValue>(stream, val);
			PropertyValue result = Serializer::deserialize<PropertyValue>(stream);
			EXPECT_EQ(val, result);
		};

		// Test all types
		testPropValue(PropertyValue(std::monostate{})); // NULL
		testPropValue(PropertyValue(true));
		testPropValue(PropertyValue(false));
		testPropValue(PropertyValue(INT64_MIN));
		testPropValue(PropertyValue(INT64_MAX));
		testPropValue(PropertyValue(0));
		testPropValue(PropertyValue(-1.234));
		testPropValue(PropertyValue(999.888));
		testPropValue(PropertyValue(""));
		testPropValue(PropertyValue("Unicode 你好 🌍"));
		testPropValue(PropertyValue(std::vector<float>{}));
		testPropValue(PropertyValue(std::vector<float>{1.0f, 2.0f, 3.0f}));
	}

	// Test getSerializedSize for all PropertyValue types
	TEST_F(SerializerTest, GetSerializedSize_AllTypes) {
		// NULL type (monostate)
		PropertyValue nullVal;
		size_t nullSize = getSerializedSize(nullVal);
		EXPECT_EQ(nullSize, sizeof(PropertyType)) << "NULL type should only have type tag size";

		// Boolean
		PropertyValue boolVal(true);
		size_t boolSize = getSerializedSize(boolVal);
		EXPECT_EQ(boolSize, sizeof(PropertyType) + sizeof(bool));

		// Integer
		PropertyValue intVal(42);
		size_t intSize = getSerializedSize(intVal);
		EXPECT_EQ(intSize, sizeof(PropertyType) + sizeof(int64_t));

		// Double
		PropertyValue doubleVal(3.14);
		size_t doubleSize = getSerializedSize(doubleVal);
		EXPECT_EQ(doubleSize, sizeof(PropertyType) + sizeof(double));

		// String
		PropertyValue stringVal("Hello");
		size_t stringSize = getSerializedSize(stringVal);
		EXPECT_EQ(stringSize, sizeof(PropertyType) + sizeof(uint32_t) + 5);

		// Empty string
		PropertyValue emptyStringVal("");
		size_t emptyStringSize = getSerializedSize(emptyStringVal);
		EXPECT_EQ(emptyStringSize, sizeof(PropertyType) + sizeof(uint32_t) + 0);

		// Vector (LIST)
		PropertyValue vectorVal(std::vector<float>{1.0f, 2.0f, 3.0f});
		size_t vectorSize = getSerializedSize(vectorVal);
		EXPECT_EQ(vectorSize, sizeof(PropertyType) + sizeof(uint32_t) + 3 * sizeof(float));

		// Empty vector
		PropertyValue emptyVectorVal(std::vector<float>{});
		size_t emptyVectorSize = getSerializedSize(emptyVectorVal);
		EXPECT_EQ(emptyVectorSize, sizeof(PropertyType) + sizeof(uint32_t) + 0);
	}

	// Test getSerializedSize with large values
	TEST_F(SerializerTest, GetSerializedSize_LargeValues) {
		// Long string
		std::string longStr(10000, 'A');
		PropertyValue longStringVal(longStr);
		size_t longStringSize = getSerializedSize(longStringVal);
		EXPECT_EQ(longStringSize, sizeof(PropertyType) + sizeof(uint32_t) + 10000);

		// Large vector
		std::vector<float> largeVec(1000);
		PropertyValue largeVectorVal(largeVec);
		size_t largeVectorSize = getSerializedSize(largeVectorVal);
		EXPECT_EQ(largeVectorSize, sizeof(PropertyType) + sizeof(uint32_t) + 1000 * sizeof(float));
	}

	// Test serialize/deserialize with multiple values in same stream
	TEST_F(SerializerTest, MultipleValuesInSameStream) {
		std::stringstream stream;

		// Write multiple values to the same stream
		Serializer::serialize(stream, static_cast<int64_t>(42));
		Serializer::serialize(stream, std::string("Hello"));
		Serializer::serialize(stream, static_cast<double>(3.14));

		// Read them back in order
		int64_t intVal = Serializer::deserialize<int64_t>(stream);
		EXPECT_EQ(intVal, 42);

		std::string stringVal = Serializer::deserialize<std::string>(stream);
		EXPECT_EQ(stringVal, "Hello");

		double doubleVal = Serializer::deserialize<double>(stream);
		EXPECT_DOUBLE_EQ(doubleVal, 3.14);
	}
} // namespace graph::utils::test
