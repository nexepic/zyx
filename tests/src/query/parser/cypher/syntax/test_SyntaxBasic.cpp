/**
 * @file test_SyntaxBasic.cpp
 * @author Nexepic
 * @date 2026/1/29
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

#include "SyntaxTestHelper.hpp"

// --- Read (MATCH) ---
TEST_F(CypherSyntaxTest, ValidMatchWhere) {
	EXPECT_TRUE(validate("MATCH (n) RETURN n"));
	EXPECT_TRUE(validate("MATCH (n) WHERE n.age > 10 RETURN n"));
	EXPECT_TRUE(validate("MATCH (n) WHERE n.name = 'Alice' RETURN n"));
	EXPECT_TRUE(validate("MATCH (n) WHERE n.age > 10 AND n.name = 'Bob' RETURN n"));
}

TEST_F(CypherSyntaxTest, ValidPatterns) {
	EXPECT_TRUE(validate("MATCH (a)-[r1]->(b)-[r2]->(c) RETURN a, c"));
	EXPECT_TRUE(validate("MATCH (a)<-[r]-(b) RETURN a"));
}

// --- Write (CREATE/DELETE/SET/MERGE) ---
TEST_F(CypherSyntaxTest, ValidCreate) {
	EXPECT_TRUE(validate("CREATE (n:User {name: 'Alice'})"));
	EXPECT_TRUE(validate("CREATE (a)-[:REL]->(b)"));
}

TEST_F(CypherSyntaxTest, ValidDelete) {
	EXPECT_TRUE(validate("MATCH (n) DELETE n"));
	EXPECT_TRUE(validate("MATCH (n) DETACH DELETE n"));
	EXPECT_FALSE(validate("MATCH (n) DELETE")); // Invalid
}

TEST_F(CypherSyntaxTest, ValidSet) {
	EXPECT_TRUE(validate("MATCH (n) SET n.age = 30"));
	EXPECT_TRUE(validate("MATCH (n) SET n:Active"));
}

TEST_F(CypherSyntaxTest, ValidMerge) {
	EXPECT_TRUE(validate("MERGE (n:User {name: 'Alice'})"));
	EXPECT_TRUE(validate("MERGE (n) ON CREATE SET n.c=1 ON MATCH SET n.u=1"));
	EXPECT_FALSE(validate("MERGE")); // Invalid
}

TEST_F(CypherSyntaxTest, ValidRemove) {
	EXPECT_TRUE(validate("MATCH (n) REMOVE n.prop"));
	EXPECT_TRUE(validate("MATCH (n) REMOVE n:Label"));
}
