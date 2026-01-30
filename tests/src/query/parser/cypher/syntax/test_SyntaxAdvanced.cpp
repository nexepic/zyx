/**
 * @file test_SyntaxAdvanced.cpp
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

TEST_F(CypherSyntaxTest, ValidPagination) {
	EXPECT_TRUE(validate("MATCH (n) RETURN n LIMIT 10"));
	EXPECT_TRUE(validate("MATCH (n) RETURN n SKIP 5 LIMIT 10"));
	EXPECT_FALSE(validate("MATCH (n) RETURN n LIMIT 10 SKIP 5")); // Wrong Order
}

TEST_F(CypherSyntaxTest, ValidOrderBy) { EXPECT_TRUE(validate("MATCH (n) RETURN n ORDER BY n.age DESC, n.name ASC")); }

TEST_F(CypherSyntaxTest, ValidVarLengthPath) {
	EXPECT_TRUE(validate("MATCH (a)-[*]->(b) RETURN b"));
	EXPECT_TRUE(validate("MATCH (a)-[*1..5]->(b) RETURN b"));
}

TEST_F(CypherSyntaxTest, ValidCartesianProduct) {
	EXPECT_TRUE(validate("MATCH (a), (b) RETURN a, b"));
	EXPECT_TRUE(validate("MATCH (a) MATCH (b) RETURN a, b"));
}

TEST_F(CypherSyntaxTest, ValidUnwind) {
	EXPECT_TRUE(validate("UNWIND [1, 2, 3] AS x RETURN x"));
	EXPECT_TRUE(validate("UNWIND [1, 'two', true] AS x RETURN x"));
	EXPECT_FALSE(validate("UNWIND [1, 2 AS x")); // Malformed
}
