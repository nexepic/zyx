/**
 * @file test_ExistsExpression.cpp
 * @author ZYX Contributors
 * @date 2026
 *
 * @copyright Copyright (c) 2026
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

#include "query/expressions/ExpressionsTestFixture.hpp"


TEST_F(ExpressionsTest, ExistsExpression_ConstructorPatternOnly) {
	// Test ExistsExpression constructor with pattern only
	ExistsExpression expr("(n)-[:FRIENDS]->()");

	EXPECT_EQ(expr.getPattern(), "(n)-[:FRIENDS]->()");
	EXPECT_FALSE(expr.hasWhereClause());
	EXPECT_EQ(expr.getWhereExpression(), nullptr);
}

TEST_F(ExpressionsTest, ExistsExpression_ConstructorWithWhere) {
	// Test ExistsExpression constructor with pattern and WHERE clause
	auto whereExpr = std::make_unique<LiteralExpression>(bool(true));
	ExistsExpression expr("(n)-[:FRIENDS]->()", std::move(whereExpr));

	EXPECT_EQ(expr.getPattern(), "(n)-[:FRIENDS]->()");
	EXPECT_TRUE(expr.hasWhereClause());
	EXPECT_NE(expr.getWhereExpression(), nullptr);
}

TEST_F(ExpressionsTest, ExistsExpression_ToStringNoWhere) {
	// Test toString() without WHERE clause
	ExistsExpression expr("(n)-[:FRIENDS]->()");

	std::string str = expr.toString();
	EXPECT_EQ(str, "EXISTS((n)-[:FRIENDS]->())");
}

TEST_F(ExpressionsTest, ExistsExpression_ToStringWithWhere) {
	// Test toString() with WHERE clause
	auto whereExpr = std::make_unique<LiteralExpression>(bool(true));
	ExistsExpression expr("(n)-[:FRIENDS]->()", std::move(whereExpr));

	std::string str = expr.toString();
	EXPECT_TRUE(str.find("EXISTS(") != std::string::npos);
	EXPECT_TRUE(str.find("(n)-[:FRIENDS]->()") != std::string::npos);
	EXPECT_TRUE(str.find("WHERE") != std::string::npos);
}

TEST_F(ExpressionsTest, ExistsExpression_CloneNoWhere) {
	// Test clone() without WHERE clause
	ExistsExpression expr("(n)-[:FRIENDS]->()");
	auto cloned = expr.clone();

	ASSERT_NE(cloned, nullptr);
	auto* clonedExists = dynamic_cast<ExistsExpression*>(cloned.get());
	ASSERT_NE(clonedExists, nullptr);
	EXPECT_EQ(clonedExists->getPattern(), expr.getPattern());
	EXPECT_FALSE(clonedExists->hasWhereClause());
}

TEST_F(ExpressionsTest, ExistsExpression_CloneWithWhere) {
	// Test clone() with WHERE clause
	auto whereExpr = std::make_unique<LiteralExpression>(bool(true));
	ExistsExpression expr("(n)-[:FRIENDS]->()", std::move(whereExpr));
	auto cloned = expr.clone();

	ASSERT_NE(cloned, nullptr);
	auto* clonedExists = dynamic_cast<ExistsExpression*>(cloned.get());
	ASSERT_NE(clonedExists, nullptr);
	EXPECT_EQ(clonedExists->getPattern(), expr.getPattern());
	EXPECT_TRUE(clonedExists->hasWhereClause());
	EXPECT_NE(clonedExists->getWhereExpression(), nullptr);
}

TEST_F(ExpressionsTest, ExistsExpression_AcceptVisitor) {
	// Test accept() with ExpressionVisitor
	ExistsExpression expr("(n)-[:FRIENDS]->()");

	TestExpressionVisitor visitor;
	expr.accept(visitor);

	EXPECT_TRUE(visitor.visitedExists);
}

TEST_F(ExpressionsTest, ExistsExpression_AcceptConstVisitor) {
	// Test accept() with ConstExpressionVisitor
	const ExistsExpression expr("(n)-[:FRIENDS]->()");

	TestConstExpressionVisitor visitor;
	expr.accept(visitor);

	EXPECT_TRUE(visitor.visitedExists);
}

TEST_F(ExpressionsTest, ExistsExpression_NotImplemented_Throws) {
	// Test ExistsExpression throws not implemented exception
	ExistsExpression expr("(n)-[:KNOWS]->()");

	ExpressionEvaluator evaluator(*context_);
	EXPECT_THROW((void)expr.accept(evaluator), ExpressionEvaluationException);
}

TEST_F(ExpressionsTest, ExistsExpression_GetTargetVar_Empty) {
	// Pattern-only constructor: targetVar_ is empty
	ExistsExpression expr("(n)-[:FRIENDS]->()");
	EXPECT_EQ(expr.getTargetVar(), "");
}

TEST_F(ExpressionsTest, ExistsExpression_GetTargetVar_Structured) {
	// Structured constructor: targetVar_ is set explicitly
	ExistsExpression expr(
		"(n)-[:FRIENDS]->(m)",
		"n",
		"FRIENDS",
		"Person",
		PatternDirection::PAT_OUTGOING,
		nullptr,
		"m"
	);
	EXPECT_EQ(expr.getTargetVar(), "m");
}

TEST_F(ExpressionsTest, ExistsExpression_HasStructuredPattern_False) {
	// Pattern-only constructor: sourceVar_ is empty => hasStructuredPattern() == false
	ExistsExpression expr("(n)-[:FRIENDS]->()");
	EXPECT_FALSE(expr.hasStructuredPattern());
}

TEST_F(ExpressionsTest, ExistsExpression_HasStructuredPattern_True) {
	// Structured constructor: sourceVar_ is non-empty => hasStructuredPattern() == true
	ExistsExpression expr(
		"(n)-[:KNOWS]->(m)",
		"n",
		"KNOWS",
		"",
		PatternDirection::PAT_BOTH
	);
	EXPECT_TRUE(expr.hasStructuredPattern());
	EXPECT_EQ(expr.getSourceVar(), "n");
	EXPECT_EQ(expr.getRelType(), "KNOWS");
}

TEST_F(ExpressionsTest, ExistsExpression_CloneStructuredWithWhere) {
	auto whereExpr = std::make_unique<LiteralExpression>(bool(true));
	ExistsExpression expr(
		"(n)-[:FRIENDS]->(m)",
		"n",
		"FRIENDS",
		"Person",
		PatternDirection::PAT_OUTGOING,
		std::move(whereExpr),
		"m"
	);

	auto cloned = expr.clone();
	ASSERT_NE(cloned, nullptr);
	auto* clonedExists = dynamic_cast<ExistsExpression*>(cloned.get());
	ASSERT_NE(clonedExists, nullptr);
	EXPECT_EQ(clonedExists->getPattern(), "(n)-[:FRIENDS]->(m)");
	EXPECT_TRUE(clonedExists->hasStructuredPattern());
	EXPECT_EQ(clonedExists->getSourceVar(), "n");
	EXPECT_EQ(clonedExists->getRelType(), "FRIENDS");
	EXPECT_EQ(clonedExists->getTargetLabel(), "Person");
	EXPECT_EQ(clonedExists->getTargetVar(), "m");
	EXPECT_TRUE(clonedExists->hasWhereClause());
	EXPECT_NE(clonedExists->getWhereExpression(), nullptr);
}

TEST_F(ExpressionsTest, ExistsExpression_CloneStructuredWithoutWhere) {
	ExistsExpression expr(
		"(n)-[:KNOWS]->(m)",
		"n",
		"KNOWS",
		"",
		PatternDirection::PAT_BOTH,
		nullptr,
		"m"
	);

	auto cloned = expr.clone();
	ASSERT_NE(cloned, nullptr);
	auto* clonedExists = dynamic_cast<ExistsExpression*>(cloned.get());
	ASSERT_NE(clonedExists, nullptr);
	EXPECT_TRUE(clonedExists->hasStructuredPattern());
	EXPECT_FALSE(clonedExists->hasWhereClause());
	EXPECT_EQ(clonedExists->getTargetVar(), "m");
}
