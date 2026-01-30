/**
 * @file test_SyntaxVector.cpp
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

TEST_F(CypherSyntaxTest, ValidVectorSearchCall) {
	// Standard Call
	EXPECT_TRUE(validate("CALL db.index.vector.queryNodes('idx', 10, [1.0, 0.5])"));

	// With Yield
	EXPECT_TRUE(validate("CALL db.index.vector.queryNodes('idx', 10, [1.0]) YIELD node, score"));

	// Test your specific query that failed before:
	EXPECT_TRUE(validate(
			"CALL db.index.vector.queryNodes('idx', 2, [1.0, 1.0]) YIELD node, score RETURN node.content, score"));
}

TEST_F(CypherSyntaxTest, ValidListLiterals) {
	// Empty list
	EXPECT_TRUE(validate("RETURN []"));
	// Float list
	EXPECT_TRUE(validate("RETURN [1.0, 2.5, -3.0]"));
	// Nested list (if grammar allows)
	EXPECT_TRUE(validate("RETURN [[1,2], [3,4]]"));
}

TEST_F(CypherSyntaxTest, ValidVectorTrainCall) { EXPECT_TRUE(validate("CALL db.index.vector.train('idx')")); }
