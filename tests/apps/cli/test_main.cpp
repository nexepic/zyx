/**
 * @file test_main.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/2/28
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include <gtest/gtest.h>
#include "graph/core/Database.hpp"

// Test database creation
TEST(DatabaseTest, CreateDatabase) {
    std::string testDbPath = "test_db_create.metrix";

    // Create the database
    graph::Database db(testDbPath);

    // Open the database to create the file
    db.open();

    // Check if it was created successfully
    EXPECT_TRUE(db.exists());

    // Cleanup
    db.close();
    remove(testDbPath.c_str());
}

// Test database opening
TEST(DatabaseTest, OpenDatabase) {
	std::string testDbPath = "test_db_open.metrix";
	graph::Database db(testDbPath);

	db.open();
	EXPECT_TRUE(db.isOpen());

	db.close();
	EXPECT_FALSE(db.isOpen());

	remove(testDbPath.c_str());
}

int main(int argc, char **argv) {
	::testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}