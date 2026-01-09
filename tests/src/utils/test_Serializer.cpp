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
} // namespace graph::utils::test
