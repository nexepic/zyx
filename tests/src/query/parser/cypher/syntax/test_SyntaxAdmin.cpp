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

TEST_F(CypherSyntaxTest, ValidAdministrativeGrammarAlternatives) {
	// Keep one small syntax contract for each DDL form that is not reliably
	// reachable from the execution tests without changing persistent state.
	const std::vector<std::string> statements = {
			"SHOW INDEX",
			"DROP INDEX named_index",
			"CREATE INDEX FOR (n:User) ON (n.email)",
			"CREATE INDEX user_identity FOR (n:User) ON (n.id, n.tenant)",
			"CREATE VECTOR INDEX ON :Document(embedding)",
			"CREATE CONSTRAINT active_flag FOR (n:User) REQUIRE n.active IS :: BOOLEAN",
			"CREATE CONSTRAINT score_value FOR (n:User) REQUIRE n.score IS :: FLOAT",
			"CREATE CONSTRAINT display_name FOR (n:User) REQUIRE n.name IS :: STRING",
			"CREATE CONSTRAINT tag_list FOR (n:User) REQUIRE n.tags IS :: LIST",
			"CREATE CONSTRAINT metadata_map FOR (n:User) REQUIRE n.metadata IS :: MAP",
			"DROP CONSTRAINT named_constraint",
			"SHOW CONSTRAINT",
	};
	for (const std::string &statement: statements) {
		SCOPED_TRACE(statement);
		EXPECT_TRUE(validate(statement));
	}
}

TEST_F(CypherSyntaxTest, ValidTransactionControlForms) {
	// These statements are handled before logical-plan execution, so exercise the
	// grammar directly to retain both optional TRANSACTION forms as contracts.
	EXPECT_TRUE(validate("BEGIN"));
	EXPECT_TRUE(validate("BEGIN TRANSACTION"));
	EXPECT_TRUE(validate("COMMIT"));
	EXPECT_TRUE(validate("COMMIT TRANSACTION"));
	EXPECT_TRUE(validate("ROLLBACK"));
	EXPECT_TRUE(validate("ROLLBACK TRANSACTION"));
}

TEST_F(CypherSyntaxTest, RejectsIncompleteTransactionControlForms) {
	EXPECT_FALSE(validate("BEGIN WORK"));
	EXPECT_FALSE(validate("COMMIT WORK"));
	EXPECT_FALSE(validate("ROLLBACK WORK"));
	EXPECT_FALSE(validate("TRANSACTION"));
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
