/**
 * @file test_HeterogeneousLists.cpp
 * @date 2025/1/4
 *
 * Unit tests for heterogeneous list support in PropertyValue
 */

#include <gtest/gtest.h>
#include "graph/core/PropertyTypes.hpp"

using namespace graph;

class HeterogeneousListTest : public ::testing::Test {
protected:
	void SetUp() override {}
	void TearDown() override {}
};

// Test homogeneous lists
TEST_F(HeterogeneousListTest, IntegerList) {
	std::vector<PropertyValue> list = {
		PropertyValue(int64_t(1)),
		PropertyValue(int64_t(2)),
		PropertyValue(int64_t(3))
	};
	PropertyValue listValue(list);

	ASSERT_EQ(listValue.getType(), PropertyType::LIST);
	const auto& retrievedList = listValue.getList();
	ASSERT_EQ(retrievedList.size(), 3);

	EXPECT_EQ(retrievedList[0].getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(retrievedList[0].getVariant()), 1);
	EXPECT_EQ(std::get<int64_t>(retrievedList[1].getVariant()), 2);
	EXPECT_EQ(std::get<int64_t>(retrievedList[2].getVariant()), 3);

	EXPECT_EQ(listValue.toString(), "[1, 2, 3]");
}

// Test homogeneous double list
TEST_F(HeterogeneousListTest, DoubleList) {
	std::vector<PropertyValue> list = {
		PropertyValue(1.5),
		PropertyValue(2.5),
		PropertyValue(3.5)
	};
	PropertyValue listValue(list);

	ASSERT_EQ(listValue.getType(), PropertyType::LIST);
	const auto& retrievedList = listValue.getList();
	ASSERT_EQ(retrievedList.size(), 3);

	EXPECT_EQ(retrievedList[0].getType(), PropertyType::DOUBLE);
	EXPECT_EQ(std::get<double>(retrievedList[0].getVariant()), 1.5);
	EXPECT_EQ(std::get<double>(retrievedList[1].getVariant()), 2.5);
	EXPECT_EQ(std::get<double>(retrievedList[2].getVariant()), 3.5);
}

// Test homogeneous string list
TEST_F(HeterogeneousListTest, StringList) {
	std::vector<PropertyValue> list = {
		PropertyValue(std::string("hello")),
		PropertyValue(std::string("world"))
	};
	PropertyValue listValue(list);

	ASSERT_EQ(listValue.getType(), PropertyType::LIST);
	const auto& retrievedList = listValue.getList();
	ASSERT_EQ(retrievedList.size(), 2);

	EXPECT_EQ(retrievedList[0].getType(), PropertyType::STRING);
	EXPECT_EQ(std::get<std::string>(retrievedList[0].getVariant()), "hello");
	EXPECT_EQ(std::get<std::string>(retrievedList[1].getVariant()), "world");
}

// Test heterogeneous lists
TEST_F(HeterogeneousListTest, MixedTypes) {
	std::vector<PropertyValue> list = {
		PropertyValue(int64_t(42)),
		PropertyValue(std::string("hello")),
		PropertyValue(true),
		PropertyValue(3.14)
	};
	PropertyValue listValue(list);

	ASSERT_EQ(listValue.getType(), PropertyType::LIST);
	const auto& retrievedList = listValue.getList();
	ASSERT_EQ(retrievedList.size(), 4);

	// Verify each element's type and value
	EXPECT_EQ(retrievedList[0].getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(retrievedList[0].getVariant()), 42);

	EXPECT_EQ(retrievedList[1].getType(), PropertyType::STRING);
	EXPECT_EQ(std::get<std::string>(retrievedList[1].getVariant()), "hello");

	EXPECT_EQ(retrievedList[2].getType(), PropertyType::BOOLEAN);
	EXPECT_EQ(std::get<bool>(retrievedList[2].getVariant()), true);

	EXPECT_EQ(retrievedList[3].getType(), PropertyType::DOUBLE);
	EXPECT_DOUBLE_EQ(std::get<double>(retrievedList[3].getVariant()), 3.14);
}

// Test list with null values
TEST_F(HeterogeneousListTest, ListWithNulls) {
	std::vector<PropertyValue> list = {
		PropertyValue(int64_t(1)),
		PropertyValue(), // null
		PropertyValue(std::string("test"))
	};
	PropertyValue listValue(list);

	ASSERT_EQ(listValue.getType(), PropertyType::LIST);
	const auto& retrievedList = listValue.getList();
	ASSERT_EQ(retrievedList.size(), 3);

	EXPECT_EQ(retrievedList[0].getType(), PropertyType::INTEGER);
	EXPECT_EQ(retrievedList[1].getType(), PropertyType::NULL_TYPE);
	EXPECT_EQ(retrievedList[2].getType(), PropertyType::STRING);
}

// Test empty list
TEST_F(HeterogeneousListTest, EmptyList) {
	std::vector<PropertyValue> list;
	PropertyValue listValue(list);

	ASSERT_EQ(listValue.getType(), PropertyType::LIST);
	const auto& retrievedList = listValue.getList();
	EXPECT_EQ(retrievedList.size(), 0);
	EXPECT_EQ(listValue.toString(), "[]");
}

// Test nested lists
TEST_F(HeterogeneousListTest, NestedLists) {
	std::vector<PropertyValue> inner1 = {
		PropertyValue(int64_t(1)),
		PropertyValue(int64_t(2))
	};
	std::vector<PropertyValue> inner2 = {
		PropertyValue(int64_t(3)),
		PropertyValue(int64_t(4))
	};

	std::vector<PropertyValue> outer = {
		PropertyValue(inner1),
		PropertyValue(inner2)
	};
	PropertyValue listValue(outer);

	ASSERT_EQ(listValue.getType(), PropertyType::LIST);
	const auto& outerList = listValue.getList();
	ASSERT_EQ(outerList.size(), 2);

	// Verify first inner list
	EXPECT_EQ(outerList[0].getType(), PropertyType::LIST);
	const auto& firstInner = outerList[0].getList();
	EXPECT_EQ(firstInner.size(), 2);
	EXPECT_EQ(std::get<int64_t>(firstInner[0].getVariant()), 1);
	EXPECT_EQ(std::get<int64_t>(firstInner[1].getVariant()), 2);

	// Verify second inner list
	EXPECT_EQ(outerList[1].getType(), PropertyType::LIST);
	const auto& secondInner = outerList[1].getList();
	EXPECT_EQ(secondInner.size(), 2);
	EXPECT_EQ(std::get<int64_t>(secondInner[0].getVariant()), 3);
	EXPECT_EQ(std::get<int64_t>(secondInner[1].getVariant()), 4);
}

// Test deeply nested lists
TEST_F(HeterogeneousListTest, DeeplyNestedLists) {
	// Create [[[1, 2], [3, 4]], [[5, 6], [7, 8]]]
	std::vector<PropertyValue> deepest1 = {
		PropertyValue(int64_t(1)),
		PropertyValue(int64_t(2))
	};
	std::vector<PropertyValue> deepest2 = {
		PropertyValue(int64_t(3)),
		PropertyValue(int64_t(4))
	};
	std::vector<PropertyValue> deepest3 = {
		PropertyValue(int64_t(5)),
		PropertyValue(int64_t(6))
	};
	std::vector<PropertyValue> deepest4 = {
		PropertyValue(int64_t(7)),
		PropertyValue(int64_t(8))
	};

	std::vector<PropertyValue> deeper1 = {
		PropertyValue(deepest1),
		PropertyValue(deepest2)
	};
	std::vector<PropertyValue> deeper2 = {
		PropertyValue(deepest3),
		PropertyValue(deepest4)
	};

	std::vector<PropertyValue> outer = {
		PropertyValue(deeper1),
		PropertyValue(deeper2)
	};
	PropertyValue listValue(outer);

	ASSERT_EQ(listValue.getType(), PropertyType::LIST);
	const auto& outerList = listValue.getList();
	ASSERT_EQ(outerList.size(), 2);

	const auto& deeper1List = outerList[0].getList();
	ASSERT_EQ(deeper1List.size(), 2);

	const auto& deepest1List = deeper1List[0].getList();
	EXPECT_EQ(deepest1List.size(), 2);
	EXPECT_EQ(std::get<int64_t>(deepest1List[0].getVariant()), 1);
	EXPECT_EQ(std::get<int64_t>(deepest1List[1].getVariant()), 2);
}

// Test list comparison
TEST_F(HeterogeneousListTest, ListComparison) {
	std::vector<PropertyValue> list1 = {
		PropertyValue(int64_t(1)),
		PropertyValue(int64_t(2))
	};
	std::vector<PropertyValue> list2 = {
		PropertyValue(int64_t(1)),
		PropertyValue(int64_t(2))
	};
	std::vector<PropertyValue> list3 = {
		PropertyValue(int64_t(1)),
		PropertyValue(int64_t(3))
	};

	PropertyValue val1(list1);
	PropertyValue val2(list2);
	PropertyValue val3(list3);

	EXPECT_EQ(val1, val2);
	EXPECT_NE(val1, val3);
}

// Test heterogeneous list equality
TEST_F(HeterogeneousListTest, HeterogeneousListEquality) {
	std::vector<PropertyValue> list1 = {
		PropertyValue(int64_t(42)),
		PropertyValue(std::string("hello")),
		PropertyValue(true)
	};
	std::vector<PropertyValue> list2 = {
		PropertyValue(int64_t(42)),
		PropertyValue(std::string("hello")),
		PropertyValue(true)
	};
	std::vector<PropertyValue> list3 = {
		PropertyValue(int64_t(42)),
		PropertyValue(std::string("hello")),
		PropertyValue(false)  // Different boolean
	};

	PropertyValue val1(list1);
	PropertyValue val2(list2);
	PropertyValue val3(list3);

	EXPECT_EQ(val1, val2);
	EXPECT_NE(val1, val3);
}

// Test list copying
TEST_F(HeterogeneousListTest, ListCopy) {
	std::vector<PropertyValue> original = {
		PropertyValue(int64_t(1)),
		PropertyValue(std::string("test")),
		PropertyValue(true)
	};
	PropertyValue originalVal(original);

	// Copy constructor
	PropertyValue copyVal(originalVal);

	EXPECT_EQ(originalVal, copyVal);
	const auto& copyList = copyVal.getList();
	EXPECT_EQ(copyList.size(), 3);
	EXPECT_EQ(std::get<int64_t>(copyList[0].getVariant()), 1);
	EXPECT_EQ(std::get<std::string>(copyList[1].getVariant()), "test");
	EXPECT_EQ(std::get<bool>(copyList[2].getVariant()), true);
}

// Test list type name
TEST_F(HeterogeneousListTest, ListTypeName) {
	std::vector<PropertyValue> list = {
		PropertyValue(int64_t(1)),
		PropertyValue(std::string("test"))
	};
	PropertyValue listValue(list);

	EXPECT_EQ(listValue.typeName(), "LIST");
}

// Test property_utils::getPropertyType with list
TEST_F(HeterogeneousListTest, GetPropertyTypeWithList) {
	std::vector<PropertyValue> list = {
		PropertyValue(int64_t(1)),
		PropertyValue(std::string("test"))
	};
	PropertyValue listValue(list);

	EXPECT_EQ(getPropertyType(listValue), PropertyType::LIST);
}

// Test property_utils::getPropertyValueSize with list
TEST_F(HeterogeneousListTest, GetPropertyValueSizeWithList) {
	std::vector<PropertyValue> list = {
		PropertyValue(int64_t(42)),
		PropertyValue(std::string("hello")),  // 5 chars + size_t overhead
		PropertyValue(true)
	};
	PropertyValue listValue(list);

	size_t size = property_utils::getPropertyValueSize(listValue);
	EXPECT_GT(size, 0);  // Should be non-zero
}

// Test list with all types
TEST_F(HeterogeneousListTest, ListWithAllTypes) {
	std::vector<PropertyValue> list = {
		PropertyValue(),                          // null
		PropertyValue(false),                     // boolean
		PropertyValue(int64_t(42)),               // integer
		PropertyValue(3.14),                      // double
		PropertyValue(std::string("string"))      // string
	};
	PropertyValue listValue(list);

	ASSERT_EQ(listValue.getType(), PropertyType::LIST);
	const auto& retrievedList = listValue.getList();
	ASSERT_EQ(retrievedList.size(), 5);

	EXPECT_EQ(retrievedList[0].getType(), PropertyType::NULL_TYPE);
	EXPECT_EQ(retrievedList[1].getType(), PropertyType::BOOLEAN);
	EXPECT_EQ(retrievedList[2].getType(), PropertyType::INTEGER);
	EXPECT_EQ(retrievedList[3].getType(), PropertyType::DOUBLE);
	EXPECT_EQ(retrievedList[4].getType(), PropertyType::STRING);
}
