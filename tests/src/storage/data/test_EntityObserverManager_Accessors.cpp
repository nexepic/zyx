/**
 * @file test_EntityObserverManager_Accessors.cpp
 * @brief Accessor-focused tests for EntityObserverManager.
 */

#include <gtest/gtest.h>

#include <memory>

#include "graph/storage/data/EntityObserverManager.hpp"

using namespace graph;
using namespace graph::storage;

TEST(EntityObserverManagerAccessorTest, SuppressionAndObserverAccessors) {
	EntityObserverManager manager;

	EXPECT_FALSE(manager.isSuppressed());
	manager.setSuppressNotifications(true);
	EXPECT_TRUE(manager.isSuppressed());
	manager.setSuppressNotifications(false);
	EXPECT_FALSE(manager.isSuppressed());

	auto observer = std::make_shared<IEntityObserver>();
	manager.registerObserver(observer);
	ASSERT_EQ(manager.getObservers().size(), 1UL);
	EXPECT_EQ(manager.getObservers().front(), observer);

	auto validator = std::make_shared<constraints::IEntityValidator>();
	manager.registerValidator(validator);
	ASSERT_EQ(manager.getValidators().size(), 1UL);
	EXPECT_EQ(manager.getValidators().front(), validator);
}
