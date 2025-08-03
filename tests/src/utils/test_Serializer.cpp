/**
 * @file test_Serializer.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/8/1
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include <gtest/gtest.h>
#include <limits>
#include <sstream>
#include <vector>
#include "graph/utils/Serializer.hpp"

namespace graph::utils::test {

	class SerializerTest : public ::testing::Test {
	protected:
		// Helper method for testing POD type roundtrip
		template<typename T>
		void testPODRoundTrip(const T &value) {
			std::stringstream stream;
			Serializer::writePOD(stream, value);
			T result = Serializer::readPOD<T>(stream);
			EXPECT_EQ(value, result);
		}

		// Helper method for testing serialization roundtrip
		template<typename T>
		void testSerializeRoundTrip(const T &value) {
			std::stringstream stream;
			Serializer::serialize(stream, value);
			T result = Serializer::deserialize<T>(stream);
			EXPECT_EQ(value, result);
		}
	};

	// Test POD serialization for various types
	TEST_F(SerializerTest, WritePODReadPOD_UnsignedLong) {
		testPODRoundTrip<unsigned long>(42UL);
		testPODRoundTrip<unsigned long>(0UL);
		testPODRoundTrip<unsigned long>(ULONG_MAX);
	}

	TEST_F(SerializerTest, WritePODReadPOD_UnsignedChar) {
		testPODRoundTrip<unsigned char>(42);
		testPODRoundTrip<unsigned char>(0);
		testPODRoundTrip<unsigned char>(255);
	}

	TEST_F(SerializerTest, WritePODReadPOD_Int) {
		testPODRoundTrip<int>(42);
		testPODRoundTrip<int>(0);
		testPODRoundTrip<int>(-42);
		testPODRoundTrip<int>(INT_MAX);
		testPODRoundTrip<int>(INT_MIN);
	}

	TEST_F(SerializerTest, WritePODReadPOD_UnsignedInt) {
		testPODRoundTrip<unsigned int>(42U);
		testPODRoundTrip<unsigned int>(0U);
		testPODRoundTrip<unsigned int>(UINT_MAX);
	}

	TEST_F(SerializerTest, WritePODReadPOD_UnsignedLongLong) {
		testPODRoundTrip<unsigned long long>(42ULL);
		testPODRoundTrip<unsigned long long>(0ULL);
		testPODRoundTrip<unsigned long long>(ULLONG_MAX);
	}

	TEST_F(SerializerTest, WritePODReadPOD_Bool) {
		testPODRoundTrip<bool>(true);
		testPODRoundTrip<bool>(false);
	}

	TEST_F(SerializerTest, WritePODReadPOD_Int64) {
		testPODRoundTrip<int64_t>(42LL);
		testPODRoundTrip<int64_t>(0LL);
		testPODRoundTrip<int64_t>(-42LL);
		testPODRoundTrip<int64_t>(INT64_MAX);
		testPODRoundTrip<int64_t>(INT64_MIN);
	}

	// Test buffer operations
	TEST_F(SerializerTest, WriteBufferReadBuffer) {
		const char buffer[] = "Hello, World!";
		const size_t size = sizeof(buffer) - 1; // Exclude null terminator

		std::stringstream stream;
		Serializer::writeBuffer(stream, buffer, size);

		char result[size];
		Serializer::readBuffer(stream, result, size);

		EXPECT_EQ(0, memcmp(buffer, result, size));
	}

	// Test string serialization
	TEST_F(SerializerTest, WriteStringReadString) {
		std::string testStr = "Hello, World!";
		std::stringstream stream;

		Serializer::writeString(stream, testStr);
		std::string result = Serializer::readString(stream);

		EXPECT_EQ(testStr, result);

		// Test empty string
		testStr = "";
		stream.str("");
		stream.clear();

		Serializer::writeString(stream, testStr);
		result = Serializer::readString(stream);

		EXPECT_EQ(testStr, result);

		// Test unicode string
		testStr = "こんにちは世界";
		stream.str("");
		stream.clear();

		Serializer::writeString(stream, testStr);
		result = Serializer::readString(stream);

		EXPECT_EQ(testStr, result);
	}

	// Test PropertyValue serialization
	TEST_F(SerializerTest, WritePropertyValueReadPropertyValue) {
		std::stringstream stream;

		// Test int64_t
		PropertyValue intValue = int64_t(42);
		Serializer::writePropertyValue(stream, intValue);
		PropertyValue result = Serializer::readPropertyValue(stream);
		EXPECT_EQ(intValue, result);

		// Test double
		stream.str("");
		stream.clear();
		PropertyValue doubleValue = 3.14159;
		Serializer::writePropertyValue(stream, doubleValue);
		result = Serializer::readPropertyValue(stream);
		EXPECT_EQ(doubleValue, result);

		// Test string
		stream.str("");
		stream.clear();
		PropertyValue stringValue = std::string("Hello, World!");
		Serializer::writePropertyValue(stream, stringValue);
		result = Serializer::readPropertyValue(stream);
		EXPECT_EQ(stringValue, result);

		// Test vector<double>
		stream.str("");
		stream.clear();
		PropertyValue vectorValue = std::vector<double>{1.1, 2.2, 3.3};
		Serializer::writePropertyValue(stream, vectorValue);
		result = Serializer::readPropertyValue(stream);
		EXPECT_EQ(vectorValue, result);

		// Test bool
		stream.str("");
		stream.clear();
		PropertyValue boolValue = true;
		Serializer::writePropertyValue(stream, boolValue);
		result = Serializer::readPropertyValue(stream);
		EXPECT_EQ(boolValue, result);
	}

	// Test generic serialization
	TEST_F(SerializerTest, SerializeDeserialize_String) {
		testSerializeRoundTrip<std::string>("Hello, World!");
		testSerializeRoundTrip<std::string>("");
		testSerializeRoundTrip<std::string>("こんにちは世界");
	}

	TEST_F(SerializerTest, SerializeDeserialize_VectorDouble) {
		testSerializeRoundTrip<std::vector<double>>({1.1, 2.2, 3.3});
		testSerializeRoundTrip<std::vector<double>>({});
		testSerializeRoundTrip<std::vector<double>>({0.0, -1.0, std::numeric_limits<double>::max()});
	}

	TEST_F(SerializerTest, SerializeDeserialize_BasicTypes) {
		testSerializeRoundTrip<double>(3.14159);
		testSerializeRoundTrip<bool>(true);
		testSerializeRoundTrip<int64_t>(42LL);
		testSerializeRoundTrip<int>(-100);
		testSerializeRoundTrip<float>(1.5f);
	}

	// Test deserializeVariant
	TEST_F(SerializerTest, DeserializeVariant) {
		std::stringstream stream;

		// Write int64_t
		Serializer::writePOD(stream, static_cast<uint8_t>(0));
		Serializer::writePOD(stream, int64_t(42));
		PropertyValue result = Serializer::deserializeVariant(stream);
		EXPECT_EQ(PropertyValue(int64_t(42)), result);

		// Write double
		stream.str("");
		stream.clear();
		Serializer::writePOD(stream, static_cast<uint8_t>(1));
		Serializer::writePOD(stream, 3.14159);
		result = Serializer::deserializeVariant(stream);
		EXPECT_EQ(PropertyValue(3.14159), result);

		// Write string
		stream.str("");
		stream.clear();
		Serializer::writePOD(stream, static_cast<uint8_t>(2));
		Serializer::writeString(stream, "Hello, World!");
		result = Serializer::deserializeVariant(stream);
		EXPECT_EQ(PropertyValue(std::string("Hello, World!")), result);

		// Write vector<double>
		stream.str("");
		stream.clear();
		Serializer::writePOD(stream, static_cast<uint8_t>(3));
		std::vector<double> vec = {1.1, 2.2, 3.3};
		Serializer::serialize(stream, vec);
		result = Serializer::deserializeVariant(stream);
		EXPECT_EQ(PropertyValue(vec), result);

		// Write bool
		stream.str("");
		stream.clear();
		Serializer::writePOD(stream, static_cast<uint8_t>(4));
		Serializer::writePOD(stream, true);
		result = Serializer::deserializeVariant(stream);
		EXPECT_EQ(PropertyValue(true), result);

		// Test invalid type
		stream.str("");
		stream.clear();
		Serializer::writePOD(stream, static_cast<uint8_t>(255));
		EXPECT_THROW(Serializer::deserializeVariant(stream), std::runtime_error);
	}

	// Test error conditions
	TEST_F(SerializerTest, ErrorConditions) {
		std::stringstream emptyStream;

		// Reading from an empty stream should set the fail bit
		Serializer::readPOD<int>(emptyStream);
		EXPECT_TRUE(emptyStream.fail());

		// Reading with insufficient data
		std::stringstream partialStream;
		partialStream.write("ab", 2); // Only 2 bytes, not enough for an int
		Serializer::readPOD<int>(partialStream);
		EXPECT_TRUE(partialStream.fail());
	}

	// Test for edge cases
	TEST_F(SerializerTest, EdgeCases) {
		// Test serializing empty vector
		std::vector<double> emptyVec;
		std::stringstream stream;
		Serializer::serialize(stream, emptyVec);
		auto result = Serializer::deserialize<std::vector<double>>(stream);
		EXPECT_TRUE(result.empty());

		// Test with large string
		std::string largeString(100000, 'a');
		stream.str("");
		stream.clear();
		Serializer::writeString(stream, largeString);
		std::string largeResult = Serializer::readString(stream);
		EXPECT_EQ(largeString, largeResult);
	}

	// Test for std::monostate
	TEST_F(SerializerTest, MonostateSerialize) {
		std::stringstream stream;
		std::monostate ms;
		Serializer::serialize(stream, ms);
		// Just verify we don't crash, as monostate has no content
	}

} // namespace graph::utils::test
