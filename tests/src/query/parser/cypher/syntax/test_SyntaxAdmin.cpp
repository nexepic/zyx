/**
 * @file test_SyntaxAdmin.cpp
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

TEST_F(CypherSyntaxTest, ValidIndexCommands) {
	// Standard
	EXPECT_TRUE(validate("CREATE INDEX ON :User(name)"));
	EXPECT_TRUE(validate("DROP INDEX ON :User(name)"));
	EXPECT_TRUE(validate("SHOW INDEXES"));
}

TEST_F(CypherSyntaxTest, ValidProcedures) {
	EXPECT_TRUE(validate("CALL dbms.listConfig()"));
	EXPECT_TRUE(validate("CALL dbms.setConfig('key', 'val')"));
}

// Vector Index Syntax Tests
TEST_F(CypherSyntaxTest, ValidVectorIndex) {
	// Basic Create
	EXPECT_TRUE(validate("CREATE VECTOR INDEX vec1 ON :Doc(emb)"));

	// With Options
	EXPECT_TRUE(validate("CREATE VECTOR INDEX vec2 ON :Doc(emb) OPTIONS {dimension: 128, metric: 'L2'}"));

	// With Mixed Case (Lexer is case insensitive)
	EXPECT_TRUE(validate("create vector index VEC ON :Label(prop) options {dim:4}"));
}

TEST_F(CypherSyntaxTest, InvalidVectorIndex) {
	// Missing ON
	EXPECT_FALSE(validate("CREATE VECTOR INDEX vec1 :Doc(emb)"));
	// Missing Property
	EXPECT_FALSE(validate("CREATE VECTOR INDEX vec1 ON :Doc"));
}
