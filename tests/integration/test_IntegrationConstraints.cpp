/**
 * @file test_IntegrationConstraints.cpp
 * @author Nexepic
 * @date 2026/3/27
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

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>

#include "graph/core/Database.hpp"
#include "graph/query/api/QueryResult.hpp"

namespace fs = std::filesystem;
using namespace graph;

class IntegrationConstraintTest : public ::testing::Test {
protected:
	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testDbPath = fs::temp_directory_path() / ("test_constraint_" + boost::uuids::to_string(uuid) + ".graph");
		if (fs::exists(testDbPath))
			fs::remove_all(testDbPath);
		db = std::make_unique<Database>(testDbPath.string());
		db->open();
	}

	void TearDown() override {
		if (db)
			db->close();
		db.reset();
		std::error_code ec;
		if (fs::exists(testDbPath))
			fs::remove_all(testDbPath, ec);
	}

	query::QueryResult execute(const std::string &query) const { return db->getQueryEngine()->execute(query); }

	std::filesystem::path testDbPath;
	std::unique_ptr<Database> db;
};

// ============================================================================
// SHOW CONSTRAINT (empty)
// ============================================================================

TEST_F(IntegrationConstraintTest, ShowConstraintsEmpty) {
	auto result = execute("SHOW CONSTRAINT");
	EXPECT_EQ(result.rowCount(), 0UL);
}

// ============================================================================
// NOT NULL Constraint
// ============================================================================

TEST_F(IntegrationConstraintTest, CreateNotNullConstraint) {
	auto result = execute("CREATE CONSTRAINT nn_name FOR (n:Person) REQUIRE n.name IS NOT NULL");
	EXPECT_EQ(result.rowCount(), 1UL);

	auto show = execute("SHOW CONSTRAINT");
	EXPECT_EQ(show.rowCount(), 1UL);
}

TEST_F(IntegrationConstraintTest, NotNullBlocksNullInsert) {
	(void) execute("CREATE CONSTRAINT nn_name FOR (n:Person) REQUIRE n.name IS NOT NULL");

	// Insert without the required property should fail
	EXPECT_THROW(execute("CREATE (n:Person {age: 30})"), std::runtime_error);
}

TEST_F(IntegrationConstraintTest, NotNullAllowsValidInsert) {
	(void) execute("CREATE CONSTRAINT nn_name FOR (n:Person) REQUIRE n.name IS NOT NULL");

	// Insert with the required property should succeed
	auto result = execute("CREATE (n:Person {name: 'Alice', age: 30})");
	EXPECT_EQ(result.rowCount(), 1UL);

	auto match = execute("MATCH (n:Person) RETURN n.name");
	EXPECT_EQ(match.rowCount(), 1UL);
}

TEST_F(IntegrationConstraintTest, NotNullBlocksPropertyRemoval) {
	(void) execute("CREATE CONSTRAINT nn_name FOR (n:Person) REQUIRE n.name IS NOT NULL");
	(void) execute("CREATE (n:Person {name: 'Alice', age: 30})");

	// Removing the constrained property should fail
	EXPECT_THROW(execute("MATCH (n:Person {name: 'Alice'}) REMOVE n.name"), std::runtime_error);
}

// ============================================================================
// UNIQUE Constraint
// ============================================================================

TEST_F(IntegrationConstraintTest, CreateUniqueConstraint) {
	auto result = execute("CREATE CONSTRAINT uniq_email FOR (n:User) REQUIRE n.email IS UNIQUE");
	EXPECT_EQ(result.rowCount(), 1UL);

	auto show = execute("SHOW CONSTRAINT");
	EXPECT_EQ(show.rowCount(), 1UL);
}

TEST_F(IntegrationConstraintTest, UniqueBlocksDuplicate) {
	(void) execute("CREATE CONSTRAINT uniq_email FOR (n:User) REQUIRE n.email IS UNIQUE");

	(void) execute("CREATE (n:User {email: 'alice@test.com'})");

	// Inserting a duplicate value should fail
	EXPECT_THROW(execute("CREATE (n:User {email: 'alice@test.com'})"), std::runtime_error);
}

TEST_F(IntegrationConstraintTest, UniqueAllowsDifferentValues) {
	(void) execute("CREATE CONSTRAINT uniq_email FOR (n:User) REQUIRE n.email IS UNIQUE");

	(void) execute("CREATE (n:User {email: 'alice@test.com'})");
	(void) execute("CREATE (n:User {email: 'bob@test.com'})");

	auto match = execute("MATCH (n:User) RETURN n.email");
	EXPECT_EQ(match.rowCount(), 2UL);
}

TEST_F(IntegrationConstraintTest, UniqueAllowsNullValues) {
	(void) execute("CREATE CONSTRAINT uniq_email FOR (n:User) REQUIRE n.email IS UNIQUE");

	// Two nodes without the property should be allowed (NULL is not a duplicate)
	(void) execute("CREATE (n:User {name: 'Alice'})");
	(void) execute("CREATE (n:User {name: 'Bob'})");

	auto match = execute("MATCH (n:User) RETURN n.name");
	EXPECT_EQ(match.rowCount(), 2UL);
}

// ============================================================================
// Property Type Constraint
// ============================================================================

TEST_F(IntegrationConstraintTest, CreatePropertyTypeConstraint) {
	auto result = execute("CREATE CONSTRAINT type_age FOR (n:Person) REQUIRE n.age IS ::INTEGER");
	EXPECT_EQ(result.rowCount(), 1UL);
}

TEST_F(IntegrationConstraintTest, PropertyTypeBlocksWrongType) {
	(void) execute("CREATE CONSTRAINT type_age FOR (n:Person) REQUIRE n.age IS ::INTEGER");

	// String value for an INTEGER constraint should fail
	EXPECT_THROW(execute("CREATE (n:Person {age: 'thirty'})"), std::runtime_error);
}

TEST_F(IntegrationConstraintTest, PropertyTypeAllowsCorrectType) {
	(void) execute("CREATE CONSTRAINT type_age FOR (n:Person) REQUIRE n.age IS ::INTEGER");

	auto result = execute("CREATE (n:Person {age: 30})");
	EXPECT_EQ(result.rowCount(), 1UL);
}

TEST_F(IntegrationConstraintTest, PropertyTypeAllowsNullValues) {
	(void) execute("CREATE CONSTRAINT type_age FOR (n:Person) REQUIRE n.age IS ::INTEGER");

	// Missing property is OK (type constraint only checks when present)
	auto result = execute("CREATE (n:Person {name: 'Alice'})");
	EXPECT_EQ(result.rowCount(), 1UL);
}

// ============================================================================
// NODE KEY Constraint
// ============================================================================

TEST_F(IntegrationConstraintTest, CreateNodeKeyConstraint) {
	auto result = execute("CREATE CONSTRAINT nk_person FOR (n:Person) REQUIRE (n.firstName, n.lastName) IS NODE KEY");
	EXPECT_EQ(result.rowCount(), 1UL);
}

TEST_F(IntegrationConstraintTest, NodeKeyBlocksMissingProperty) {
	(void) execute("CREATE CONSTRAINT nk_person FOR (n:Person) REQUIRE (n.firstName, n.lastName) IS NODE KEY");

	// Missing one of the key properties should fail
	EXPECT_THROW(execute("CREATE (n:Person {firstName: 'Alice'})"), std::runtime_error);
}

TEST_F(IntegrationConstraintTest, NodeKeyBlocksDuplicate) {
	(void) execute("CREATE CONSTRAINT nk_person FOR (n:Person) REQUIRE (n.firstName, n.lastName) IS NODE KEY");

	(void) execute("CREATE (n:Person {firstName: 'Alice', lastName: 'Smith'})");

	// Same composite key should fail
	EXPECT_THROW(execute("CREATE (n:Person {firstName: 'Alice', lastName: 'Smith'})"), std::runtime_error);
}

TEST_F(IntegrationConstraintTest, NodeKeyAllowsDifferentComposites) {
	(void) execute("CREATE CONSTRAINT nk_person FOR (n:Person) REQUIRE (n.firstName, n.lastName) IS NODE KEY");

	(void) execute("CREATE (n:Person {firstName: 'Alice', lastName: 'Smith'})");
	(void) execute("CREATE (n:Person {firstName: 'Alice', lastName: 'Jones'})");

	auto match = execute("MATCH (n:Person) RETURN n.firstName, n.lastName");
	EXPECT_EQ(match.rowCount(), 2UL);
}

// ============================================================================
// DROP CONSTRAINT
// ============================================================================

TEST_F(IntegrationConstraintTest, DropConstraintByName) {
	(void) execute("CREATE CONSTRAINT nn_name FOR (n:Person) REQUIRE n.name IS NOT NULL");
	auto show1 = execute("SHOW CONSTRAINT");
	EXPECT_EQ(show1.rowCount(), 1UL);

	auto result = execute("DROP CONSTRAINT nn_name");
	EXPECT_EQ(result.rowCount(), 1UL);

	auto show2 = execute("SHOW CONSTRAINT");
	EXPECT_EQ(show2.rowCount(), 0UL);
}

TEST_F(IntegrationConstraintTest, DropConstraintIfExistsNonExistent) {
	// Should not throw when constraint doesn't exist
	auto result = execute("DROP CONSTRAINT nonexistent IF EXISTS");
	EXPECT_EQ(result.rowCount(), 0UL);
}

TEST_F(IntegrationConstraintTest, DropConstraintNonExistentThrows) {
	EXPECT_THROW(execute("DROP CONSTRAINT nonexistent"), std::runtime_error);
}

TEST_F(IntegrationConstraintTest, DropConstraintRemovesEnforcement) {
	(void) execute("CREATE CONSTRAINT nn_name FOR (n:Person) REQUIRE n.name IS NOT NULL");

	// Should fail before drop
	EXPECT_THROW(execute("CREATE (n:Person {age: 30})"), std::runtime_error);

	(void) execute("DROP CONSTRAINT nn_name");

	// Should succeed after drop
	auto result = execute("CREATE (n:Person {age: 30})");
	EXPECT_EQ(result.rowCount(), 1UL);
}

// ============================================================================
// Duplicate Constraint Name
// ============================================================================

TEST_F(IntegrationConstraintTest, DuplicateConstraintNameThrows) {
	(void) execute("CREATE CONSTRAINT myconstraint FOR (n:Person) REQUIRE n.name IS NOT NULL");
	EXPECT_THROW(
		execute("CREATE CONSTRAINT myconstraint FOR (n:User) REQUIRE n.email IS UNIQUE"),
		std::runtime_error);
}

// ============================================================================
// Constraint on Existing Data
// ============================================================================

TEST_F(IntegrationConstraintTest, ConstraintOnExistingValidData) {
	(void) execute("CREATE (n:Person {name: 'Alice'})");
	(void) execute("CREATE (n:Person {name: 'Bob'})");

	// Should succeed - existing data satisfies NOT NULL
	auto result = execute("CREATE CONSTRAINT nn_name FOR (n:Person) REQUIRE n.name IS NOT NULL");
	EXPECT_EQ(result.rowCount(), 1UL);
}

TEST_F(IntegrationConstraintTest, ConstraintOnExistingViolatingData) {
	(void) execute("CREATE (n:Person {age: 30})"); // No 'name' property

	// Should fail - existing data violates NOT NULL
	EXPECT_THROW(
		execute("CREATE CONSTRAINT nn_name FOR (n:Person) REQUIRE n.name IS NOT NULL"),
		std::runtime_error);
}

TEST_F(IntegrationConstraintTest, UniqueConstraintOnExistingDuplicates) {
	(void) execute("CREATE (n:User {email: 'dup@test.com'})");
	(void) execute("CREATE (n:User {email: 'dup@test.com'})");

	// Should fail - existing data has duplicates
	EXPECT_THROW(
		execute("CREATE CONSTRAINT uniq_email FOR (n:User) REQUIRE n.email IS UNIQUE"),
		std::runtime_error);
}

// ============================================================================
// Multiple Constraints on Same Label
// ============================================================================

TEST_F(IntegrationConstraintTest, MultipleConstraintsSameLabel) {
	(void) execute("CREATE CONSTRAINT nn_name FOR (n:Person) REQUIRE n.name IS NOT NULL");
	(void) execute("CREATE CONSTRAINT type_age FOR (n:Person) REQUIRE n.age IS ::INTEGER");

	auto show = execute("SHOW CONSTRAINT");
	EXPECT_EQ(show.rowCount(), 2UL);

	// Both constraints must be satisfied
	EXPECT_THROW(execute("CREATE (n:Person {age: 30})"), std::runtime_error);        // missing name
	EXPECT_THROW(execute("CREATE (n:Person {name: 'Alice', age: 'old'})"), std::runtime_error); // wrong type

	// This should succeed
	auto result = execute("CREATE (n:Person {name: 'Alice', age: 30})");
	EXPECT_EQ(result.rowCount(), 1UL);
}

// ============================================================================
// Edge Constraints
// ============================================================================

TEST_F(IntegrationConstraintTest, EdgeNotNullConstraint) {
	auto result = execute("CREATE CONSTRAINT nn_since FOR ()-[r:KNOWS]-() REQUIRE r.since IS NOT NULL");
	EXPECT_EQ(result.rowCount(), 1UL);

	(void) execute("CREATE (a:Person {name: 'Alice'})");
	(void) execute("CREATE (b:Person {name: 'Bob'})");

	// Edge without 'since' should fail
	EXPECT_THROW(
		execute("MATCH (a:Person {name: 'Alice'}), (b:Person {name: 'Bob'}) CREATE (a)-[:KNOWS]->(b)"),
		std::runtime_error);
}

TEST_F(IntegrationConstraintTest, EdgeNotNullAllowsValid) {
	(void) execute("CREATE CONSTRAINT nn_since FOR ()-[r:KNOWS]-() REQUIRE r.since IS NOT NULL");

	(void) execute("CREATE (a:Person {name: 'Alice'})");
	(void) execute("CREATE (b:Person {name: 'Bob'})");

	auto result = execute("MATCH (a:Person {name: 'Alice'}), (b:Person {name: 'Bob'}) CREATE (a)-[:KNOWS {since: 2020}]->(b)");
	EXPECT_EQ(result.rowCount(), 1UL);
}

// ============================================================================
// Persistence
// ============================================================================

TEST_F(IntegrationConstraintTest, ConstraintPersistsAcrossRestart) {
	(void) execute("CREATE CONSTRAINT nn_name FOR (n:Person) REQUIRE n.name IS NOT NULL");

	// Close and reopen the database
	db->close();
	db = std::make_unique<Database>(testDbPath.string());
	db->open();

	// Constraint should still be enforced
	auto show = execute("SHOW CONSTRAINT");
	EXPECT_EQ(show.rowCount(), 1UL);

	// Should still block violations
	EXPECT_THROW(execute("CREATE (n:Person {age: 30})"), std::runtime_error);

	// Should still allow valid data
	auto result = execute("CREATE (n:Person {name: 'Alice'})");
	EXPECT_EQ(result.rowCount(), 1UL);
}
