/**
 * @file test_FixedSizeSerializer.cpp
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
#include <sstream>
#include <string>
#include "graph/utils/FixedSizeSerializer.hpp"

namespace graph::utils::test {

	// Test class that implements serialize and static deserialize methods
	class TestSerializable {
	private:
		int value_;
		std::string name_;

	public:
		TestSerializable(int value = 0, const std::string &name = "") : value_(value), name_(name) {}

		bool operator==(const TestSerializable &other) const { return value_ == other.value_ && name_ == other.name_; }

		void serialize(std::ostream &os) const {
			// Write value
			os.write(reinterpret_cast<const char *>(&value_), sizeof(value_));

			// Write name length and string
			size_t length = name_.length();
			os.write(reinterpret_cast<const char *>(&length), sizeof(length));
			os.write(name_.data(), static_cast<std::streamsize>(length));
		}

		static TestSerializable deserialize(std::istream &is) {
			TestSerializable obj;

			// Read value
			is.read(reinterpret_cast<char *>(&obj.value_), sizeof(obj.value_));

			// Read name length and string
			size_t length;
			is.read(reinterpret_cast<char *>(&length), sizeof(length));

			obj.name_.resize(length);
			is.read(&obj.name_[0], static_cast<std::streamsize>(length));

			return obj;
		}
	};

	// Variable size test class that can create objects of different serialized sizes
	class VariableSizeSerializable {
	private:
		std::vector<char> data_;

	public:
		explicit VariableSizeSerializable(size_t size, char fillValue = 'A') : data_(size, fillValue) {}

		bool operator==(const VariableSizeSerializable &other) const { return data_ == other.data_; }

		void serialize(std::ostream &os) const {
			size_t size = data_.size();
			os.write(reinterpret_cast<const char *>(&size), sizeof(size));
			os.write(data_.data(), static_cast<std::streamsize>(size));
		}

		static VariableSizeSerializable deserialize(std::istream &is) {
			size_t size;
			is.read(reinterpret_cast<char *>(&size), sizeof(size));

			std::vector<char> data(size);
			is.read(data.data(), static_cast<std::streamsize>(size));

			VariableSizeSerializable obj(0);
			obj.data_ = std::move(data);
			return obj;
		}
	};

	class FixedSizeSerializerTest : public ::testing::Test {
	protected:
		std::stringstream stream;

		void SetUp() override {
			stream.str("");
			stream.clear();
		}

		// Helper to verify stream position is at expected position
		void verifyStreamPosition(std::streampos expected) { EXPECT_EQ(expected, stream.tellg()); }

		// Helper to calculate serialized size of an object
		template<typename T>
		size_t getSerializedSize(const T &obj) {
			std::stringstream tmpStream;
			obj.serialize(tmpStream);
			return static_cast<size_t>(tmpStream.tellp());
		}
	};

	TEST_F(FixedSizeSerializerTest, SerializeDeserialize_ExactSize) {
		TestSerializable obj(42, "test");
		size_t size = getSerializedSize(obj);

		// Serialize with exact size
		FixedSizeSerializer::serializeWithFixedSize(stream, obj, size);

		// Verify no padding was added
		EXPECT_EQ(size, static_cast<size_t>(stream.tellp()));

		// Reset stream position for reading
		stream.seekg(0);

		// Deserialize
		TestSerializable result = FixedSizeSerializer::deserializeWithFixedSize<TestSerializable>(stream, size);

		// Verify object was correctly deserialized
		EXPECT_EQ(obj, result);

		// Verify the stream position is at the end
		verifyStreamPosition(static_cast<std::streampos>(size));
	}

	TEST_F(FixedSizeSerializerTest, SerializeDeserialize_WithPadding) {
		TestSerializable obj(123, "hello");
		size_t actualSize = getSerializedSize(obj);
		size_t fixedSize = actualSize + 10; // Add 10 bytes of padding

		// Serialize with padding
		FixedSizeSerializer::serializeWithFixedSize(stream, obj, fixedSize);

		// Verify total size including padding
		EXPECT_EQ(fixedSize, static_cast<size_t>(stream.tellp()));

		// Reset stream position for reading
		stream.seekg(0);

		// Deserialize
		TestSerializable result = FixedSizeSerializer::deserializeWithFixedSize<TestSerializable>(stream, fixedSize);

		// Verify object was correctly deserialized
		EXPECT_EQ(obj, result);

		// Verify the stream position is at the end
		verifyStreamPosition(static_cast<std::streampos>(fixedSize));
	}

	TEST_F(FixedSizeSerializerTest, Serialize_ObjectTooLarge) {
		TestSerializable obj(999, "this is a longer string");
		size_t actualSize = getSerializedSize(obj);
		size_t fixedSize = actualSize - 5; // Set fixed size smaller than actual size

		// Serialization should throw an exception
		EXPECT_THROW(FixedSizeSerializer::serializeWithFixedSize(stream, obj, fixedSize), std::runtime_error);
	}

	TEST_F(FixedSizeSerializerTest, SerializeDeserialize_VariableSizes) {
		// Test with different sizes to ensure padding works correctly
		for (size_t objSize: {10, 50, 100}) {
			for (size_t extraPadding: {0, 1, 10, 100}) {
				stream.str("");
				stream.clear();

				size_t fixedSize = objSize + sizeof(size_t) + extraPadding;
				VariableSizeSerializable obj(objSize);

				// Serialize
				FixedSizeSerializer::serializeWithFixedSize(stream, obj, fixedSize);

				// Verify total size
				EXPECT_EQ(fixedSize, static_cast<size_t>(stream.tellp()));

				// Reset stream position
				stream.seekg(0);

				// Deserialize
				VariableSizeSerializable result =
						FixedSizeSerializer::deserializeWithFixedSize<VariableSizeSerializable>(stream, fixedSize);

				// Verify object
				EXPECT_EQ(obj, result);

				// Verify stream position
				verifyStreamPosition(static_cast<std::streampos>(fixedSize));
			}
		}
	}

	TEST_F(FixedSizeSerializerTest, Deserialize_SkipPadding) {
		TestSerializable obj(42, "test");
		size_t actualSize = getSerializedSize(obj);
		size_t fixedSize = actualSize * 2; // Double the size for padding

		// Serialize with padding
		FixedSizeSerializer::serializeWithFixedSize(stream, obj, fixedSize);

		// Add some data after the fixed size
		std::string trailer = "This should not be read";
		stream.write(trailer.data(), static_cast<std::streamsize>(trailer.size()));

		// Reset stream position
		stream.seekg(0);

		// Deserialize
		TestSerializable result = FixedSizeSerializer::deserializeWithFixedSize<TestSerializable>(stream, fixedSize);

		// Verify object
		EXPECT_EQ(obj, result);

		// Verify stream position is exactly after the fixed size
		verifyStreamPosition(static_cast<std::streampos>(fixedSize));

		// Verify the trailer data is still available
		std::string remainingData(trailer.size(), '\0');
		stream.read(&remainingData[0], static_cast<std::streamsize>(trailer.size()));
		EXPECT_EQ(trailer, remainingData);
	}

	TEST_F(FixedSizeSerializerTest, SerializeDeserialize_ZeroSize) {
		TestSerializable obj(0, "");
		size_t actualSize = getSerializedSize(obj);

		// Serialize with exact size
		FixedSizeSerializer::serializeWithFixedSize(stream, obj, actualSize);

		// Reset stream position
		stream.seekg(0);

		// Deserialize
		TestSerializable result = FixedSizeSerializer::deserializeWithFixedSize<TestSerializable>(stream, actualSize);

		// Verify object
		EXPECT_EQ(obj, result);
	}

	TEST_F(FixedSizeSerializerTest, SerializeDeserialize_EmptyObject) {
		VariableSizeSerializable obj(0);
		size_t actualSize = getSerializedSize(obj);
		size_t fixedSize = actualSize + 10;

		// Serialize with padding
		FixedSizeSerializer::serializeWithFixedSize(stream, obj, fixedSize);

		// Reset stream position
		stream.seekg(0);

		// Deserialize
		VariableSizeSerializable result =
				FixedSizeSerializer::deserializeWithFixedSize<VariableSizeSerializable>(stream, fixedSize);

		// Verify object
		EXPECT_EQ(obj, result);
	}

} // namespace graph::utils::test
