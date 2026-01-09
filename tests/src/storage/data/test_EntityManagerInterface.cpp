/**
 * @file test_EntityManagerInterface.cpp
 * @author Nexepic
 * @date 2026/1/8
 *
 * @copyright Copyright (c) 2026 Nexepic
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

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <string>
#include <vector>

#include "graph/storage/data/EntityManagerInterface.hpp"

// Define a simple dummy Entity for testing template instantiation
struct TestEntity {
	int64_t id = 0;
	std::string data;

	// Minimal interface expected by some graph components, though interface itself doesn't demand it
	int64_t getId() const { return id; }

	bool operator==(const TestEntity &other) const { return id == other.id && data == other.data; }
};

using namespace graph::storage;

// Mock implementation of the interface
template<typename T>
class MockEntityManager : public EntityManagerInterface<T> {
public:
	MOCK_METHOD(void, add, (T & entity), (override));
	MOCK_METHOD(void, update, (const T &entity), (override));
	MOCK_METHOD(void, remove, (T & entity), (override));
	MOCK_METHOD(T, get, (int64_t id), (override));

	MOCK_METHOD(std::vector<T>, getBatch, (const std::vector<int64_t> &ids), (override));
	MOCK_METHOD(std::vector<T>, getInRange, (int64_t startId, int64_t endId, size_t limit), (override));

	MOCK_METHOD(void, addToCache, (const T &entity), (override));
	MOCK_METHOD(void, clearCache, (), (override));
};

class EntityManagerInterfaceTest : public ::testing::Test {
protected:
	// We can use the interface pointer to test polymorphism
	std::unique_ptr<EntityManagerInterface<TestEntity>> manager;
	MockEntityManager<TestEntity> *mock; // Keep raw pointer for EXPECT_CALL

	void SetUp() override {
		auto m = std::make_unique<MockEntityManager<TestEntity>>();
		mock = m.get();
		manager = std::move(m);
	}
};

TEST_F(EntityManagerInterfaceTest, AddCallsThroughInterface) {
	TestEntity e{1, "test"};

	EXPECT_CALL(*mock, add(testing::_));

	manager->add(e);
}

TEST_F(EntityManagerInterfaceTest, UpdateCallsThroughInterface) {
	TestEntity e{1, "update"};

	EXPECT_CALL(*mock, update(testing::Eq(e)));

	manager->update(e);
}

TEST_F(EntityManagerInterfaceTest, RemoveCallsThroughInterface) {
	TestEntity e{1, "remove"};

	EXPECT_CALL(*mock, remove(testing::_));

	manager->remove(e);
}

TEST_F(EntityManagerInterfaceTest, GetReturnByType) {
	TestEntity expected{100, "found"};

	EXPECT_CALL(*mock, get(100)).WillOnce(testing::Return(expected));

	TestEntity result = manager->get(100);
	EXPECT_EQ(result, expected);
}

TEST_F(EntityManagerInterfaceTest, BatchOperations) {
	std::vector<int64_t> ids = {1, 2, 3};
	std::vector<TestEntity> ret = {{1, "a"}, {2, "b"}};

	EXPECT_CALL(*mock, getBatch(ids)).WillOnce(testing::Return(ret));

	auto result = manager->getBatch(ids);
	EXPECT_EQ(result.size(), 2u);
}

TEST_F(EntityManagerInterfaceTest, RangeOperations) {
	EXPECT_CALL(*mock, getInRange(10, 20, 5));
	manager->getInRange(10, 20, 5);
}

TEST_F(EntityManagerInterfaceTest, CacheOperations) {
	TestEntity e{5, "cache"};

	EXPECT_CALL(*mock, addToCache(testing::Eq(e)));
	EXPECT_CALL(*mock, clearCache());

	manager->addToCache(e);
	manager->clearCache();
}

// Test Template Instantiation for a different type (e.g., int) to ensure generic correctness
TEST(EntityManagerInterfaceGenericTest, InstantiatesWithPrimitive) {
	MockEntityManager<int> intManager;

	EXPECT_CALL(intManager, get(42)).WillOnce(testing::Return(123));

	int val = intManager.get(42);
	EXPECT_EQ(val, 123);
}
