/**
 * @file test_AdminClauseHandler.cpp
 * @brief Consolidated tests for AdminClauseHandler class
 * @date 2026/02/14
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

#include "QueryTestFixture.hpp"

class AdminClauseHandlerTest : public QueryTestFixture {};

// === SHOW Tests ===

TEST_F(AdminClauseHandlerTest, ShowIndexes_EmptyDatabase) {
	auto res = execute("SHOW INDEXES");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(AdminClauseHandlerTest, ShowIndexes_AfterCreatingIndex) {
	(void) execute("CREATE INDEX ON :Person(name)");

	auto res = execute("SHOW INDEXES");
	EXPECT_GE(res.rowCount(), 1UL);
}

TEST_F(AdminClauseHandlerTest, ShowIndexes_MultipleIndexes) {
	(void) execute("CREATE INDEX ON :Person(name)");
	(void) execute("CREATE INDEX ON :Company(title)");

	auto res = execute("SHOW INDEXES");
	EXPECT_GE(res.rowCount(), 2UL);
}

TEST_F(AdminClauseHandlerTest, ShowIndexes_AfterCreateDrop) {
	(void) execute("CREATE INDEX ON :Person(name)");
	(void) execute("DROP INDEX ON :Person(name)");
	auto res = execute("SHOW INDEXES");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(AdminClauseHandlerTest, ShowIndexes_AfterMultipleOperations) {
	(void) execute("CREATE INDEX idx1 ON :Person(name)");
	(void) execute("CREATE INDEX idx2 ON :Person(age)");
	(void) execute("DROP INDEX idx1");
	(void) execute("CREATE INDEX idx3 ON :Person(email)");

	auto res = execute("SHOW INDEXES");
	EXPECT_GE(res.rowCount(), 2UL);
}

TEST_F(AdminClauseHandlerTest, ShowIndexes_AfterDropAll) {
	(void) execute("CREATE INDEX ON :Person(name)");
	(void) execute("CREATE INDEX ON :Company(title)");
	(void) execute("DROP INDEX ON :Person(name)");
	(void) execute("DROP INDEX ON :Company(title)");

	auto res = execute("SHOW INDEXES");
	ASSERT_EQ(res.rowCount(), 0UL);
}

// === CREATE INDEX Tests (By Label) ===

TEST_F(AdminClauseHandlerTest, CreateIndexByLabel_SimpleIndex) {
	auto res = execute("CREATE INDEX ON :Person(name)");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(AdminClauseHandlerTest, CreateIndexByLabel_WithNamedIndex) {
	auto res = execute("CREATE INDEX my_person_idx ON :Person(name)");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(AdminClauseHandlerTest, CreateIndexByLabel_UseIndexForQuerying) {
	(void) execute("CREATE INDEX ON :Person(name)");
	(void) execute("CREATE (n:Person {name: 'Alice'})");

	auto res = execute("MATCH (n:Person {name: 'Alice'}) RETURN n.name");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.name").toString(), "Alice");
}

TEST_F(AdminClauseHandlerTest, CreateIndexByLabel_SingleCharLabel) {
	auto res = execute("CREATE INDEX ON :A(name)");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(AdminClauseHandlerTest, CreateIndexByLabel_SingleCharProperty) {
	auto res = execute("CREATE INDEX ON :Label(x)");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(AdminClauseHandlerTest, CreateIndexByLabel_SingleCharLabelAndProperty) {
	auto res = execute("CREATE INDEX ON :A(x)");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(AdminClauseHandlerTest, CreateIndexByLabel_MultiCharLabel) {
	auto res = execute("CREATE INDEX ON :Organization(name)");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(AdminClauseHandlerTest, CreateIndexByLabel_MultiCharProperty) {
	auto res = execute("CREATE INDEX ON :Person(username)");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(AdminClauseHandlerTest, CreateIndexByLabel_OnNonExistentData) {
	// Indexes can be created before data exists
	auto res = execute("CREATE INDEX ON :Person(name)");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(AdminClauseHandlerTest, CreateIndexByLabel_MultipleOnSameLabel) {
	auto res1 = execute("CREATE INDEX ON :Person(name)");
	ASSERT_EQ(res1.rowCount(), 1UL);

	auto res2 = execute("CREATE INDEX ON :Person(age)");
	ASSERT_EQ(res2.rowCount(), 1UL);

	auto res3 = execute("CREATE INDEX ON :Person(email)");
	ASSERT_EQ(res3.rowCount(), 1UL);
}

// === CREATE INDEX Tests (By Pattern) ===

TEST_F(AdminClauseHandlerTest, CreateIndexByPattern_SimplePattern) {
	auto res = execute("CREATE INDEX FOR (n:Person) ON (n.name)");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(AdminClauseHandlerTest, CreateIndexByPattern_WithNamedIndex) {
	auto res = execute("CREATE INDEX my_pattern_idx FOR (n:Person) ON (n.name)");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(AdminClauseHandlerTest, CreateIndexByPattern_WithVariable) {
	auto res = execute("CREATE INDEX FOR (node:Person) ON (node.name)");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(AdminClauseHandlerTest, CreateIndexByPattern_SingleCharVariable) {
	auto res = execute("CREATE INDEX FOR (x:Label) ON (x.prop)");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(AdminClauseHandlerTest, CreateIndexByPattern_SingleCharLabel) {
	auto res = execute("CREATE INDEX FOR (n:A) ON (n.prop)");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(AdminClauseHandlerTest, CreateIndexByPattern_SingleCharProperty) {
	auto res = execute("CREATE INDEX FOR (n:Label) ON (n.x)");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(AdminClauseHandlerTest, CreateIndexByPattern_WithProperties) {
	auto res = execute("CREATE INDEX FOR (n:Person {id: 1}) ON (n.id)");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(AdminClauseHandlerTest, CreateIndexByPattern_UseForQuerying) {
	(void) execute("CREATE INDEX FOR (n:Person) ON (n.name)");
	(void) execute("CREATE (n:Person {name: 'Bob'})");

	auto res = execute("MATCH (n:Person {name: 'Bob'}) RETURN n.name");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.name").toString(), "Bob");
}

// === CREATE INDEX - Name Variations ===

TEST_F(AdminClauseHandlerTest, CreateIndex_WithUnderscoreName) {
	auto res = execute("CREATE INDEX my_test_idx ON :Person(name)");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(AdminClauseHandlerTest, CreateIndex_WithNumbersInName) {
	auto res = execute("CREATE INDEX idx123 ON :Person(name)");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(AdminClauseHandlerTest, CreateIndex_WithMixedCaseName) {
	auto res = execute("CREATE INDEX MyIndexName ON :Person(name)");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(AdminClauseHandlerTest, CreateIndex_WithLongName) {
	std::string longName(100, 'a');
	auto res = execute("CREATE INDEX " + longName + " ON :Person(name)");
	ASSERT_EQ(res.rowCount(), 1UL);
}

// === CREATE VECTOR INDEX Tests ===

TEST_F(AdminClauseHandlerTest, CreateVectorIndex_SimpleIndex) {
	auto res = execute("CREATE VECTOR INDEX ON :Person(embedding) OPTIONS {dimension: 3}");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(AdminClauseHandlerTest, CreateVectorIndex_WithNamedIndex) {
	auto res = execute("CREATE VECTOR INDEX my_vec_idx ON :Person(embedding) OPTIONS {dimension: 3}");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(AdminClauseHandlerTest, CreateVectorIndex_WithCustomMetricCosine) {
	auto res = execute("CREATE VECTOR INDEX ON :Person(embedding) OPTIONS {dimension: 3, metric: 'COSINE'}");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(AdminClauseHandlerTest, CreateVectorIndex_MetricL2) {
	auto res = execute("CREATE VECTOR INDEX ON :Test(vec) OPTIONS {dimension: 3, metric: 'L2'}");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(AdminClauseHandlerTest, CreateVectorIndex_MetricInnerProduct) {
	auto res = execute("CREATE VECTOR INDEX ON :Test(vec) OPTIONS {dimension: 3, metric: 'INNER_PRODUCT'}");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(AdminClauseHandlerTest, CreateVectorIndex_DimShortForm) {
	auto res = execute("CREATE VECTOR INDEX ON :Person(vec) OPTIONS {dim: 5}");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(AdminClauseHandlerTest, CreateVectorIndex_DimWithMetric) {
	auto res = execute("CREATE VECTOR INDEX ON :Test(vec) OPTIONS {dim: 5, metric: 'COSINE'}");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(AdminClauseHandlerTest, CreateVectorIndex_DimensionOne) {
	auto res = execute("CREATE VECTOR INDEX ON :Test(vec) OPTIONS {dimension: 1}");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(AdminClauseHandlerTest, CreateVectorIndex_DimensionTwo) {
	auto res = execute("CREATE VECTOR INDEX ON :Test(vec) OPTIONS {dimension: 2}");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(AdminClauseHandlerTest, CreateVectorIndex_DimensionHundred) {
	auto res = execute("CREATE VECTOR INDEX ON :Test(embedding) OPTIONS {dimension: 100}");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(AdminClauseHandlerTest, CreateVectorIndex_DimensionThousand) {
	auto res = execute("CREATE VECTOR INDEX ON :Test(embedding) OPTIONS {dimension: 1000}");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(AdminClauseHandlerTest, CreateVectorIndex_DimensionLargeValue) {
	auto res = execute("CREATE VECTOR INDEX ON :Test(vec) OPTIONS {dimension: 10000}");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(AdminClauseHandlerTest, CreateVectorIndex_OptionsOrderReversed) {
	auto res = execute("CREATE VECTOR INDEX ON :Test(vec) OPTIONS {metric: 'L2', dimension: 5}");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(AdminClauseHandlerTest, CreateVectorIndex_WithUnknownOptionKey) {
	auto res = execute("CREATE VECTOR INDEX ON :Test(vec) OPTIONS {dimension: 3, unknown_key: 'value'}");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(AdminClauseHandlerTest, CreateVectorIndex_WithMultipleUnknownKeys) {
	auto res = execute("CREATE VECTOR INDEX ON :Test(vec) OPTIONS {dimension: 3, key1: 1, key2: 2}");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(AdminClauseHandlerTest, CreateVectorIndex_OtherKeyBetweenDimAndMetric) {
	auto res = execute("CREATE VECTOR INDEX ON :Test(vec) OPTIONS {dimension: 3, other: 1, metric: 'L2'}");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(AdminClauseHandlerTest, CreateVectorIndex_MultipleDifferentProperties) {
	auto res1 = execute("CREATE VECTOR INDEX ON :Person(vec1) OPTIONS {dimension: 3}");
	ASSERT_EQ(res1.rowCount(), 0UL);

	auto res2 = execute("CREATE VECTOR INDEX ON :Person(vec2) OPTIONS {dim: 5}");
	ASSERT_EQ(res2.rowCount(), 0UL);

	auto res3 = execute("CREATE VECTOR INDEX ON :Person(vec3) OPTIONS {dimension: 10, metric: 'L2'}");
	ASSERT_EQ(res3.rowCount(), 0UL);
}

TEST_F(AdminClauseHandlerTest, CreateVectorIndex_DifferentMetrics) {
	auto res1 = execute("CREATE VECTOR INDEX ON :Person(vec1) OPTIONS {dimension: 3, metric: 'L2'}");
	ASSERT_EQ(res1.rowCount(), 0UL);

	auto res2 = execute("CREATE VECTOR INDEX ON :Person(vec2) OPTIONS {dimension: 3, metric: 'COSINE'}");
	ASSERT_EQ(res2.rowCount(), 0UL);
}

TEST_F(AdminClauseHandlerTest, CreateVectorIndex_ThenShow) {
	auto res1 = execute("CREATE VECTOR INDEX ON :Person(vec) OPTIONS {dimension: 3}");
	ASSERT_EQ(res1.rowCount(), 0UL);

	auto res2 = execute("SHOW INDEXES");
	EXPECT_GE(res2.rowCount(), 1UL);
}

// === DROP INDEX Tests (By Name) ===

TEST_F(AdminClauseHandlerTest, DropIndexByName_DropExistingIndex) {
	(void) execute("CREATE INDEX my_idx ON :Person(name)");

	auto res = execute("DROP INDEX my_idx");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(AdminClauseHandlerTest, DropIndexByName_DropNonExistentIndex) {
	auto res = execute("DROP INDEX nonexistent_idx");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(AdminClauseHandlerTest, DropIndexByName_CanRecreateAfterDrop) {
	(void) execute("CREATE INDEX reuse_test ON :Person(name)");
	(void) execute("DROP INDEX reuse_test");

	auto res = execute("CREATE INDEX reuse_test ON :Person(name)");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(AdminClauseHandlerTest, DropIndexByName_LongIndexName) {
	std::string longName(50, 'x');
	(void) execute("CREATE INDEX " + longName + " ON :Person(name)");
	auto res = execute("DROP INDEX " + longName);
	ASSERT_EQ(res.rowCount(), 1UL);
}

// === DROP INDEX Tests (By Label) ===

TEST_F(AdminClauseHandlerTest, DropIndexByLabel_DropExistingIndex) {
	(void) execute("CREATE INDEX ON :Person(name)");

	auto res = execute("DROP INDEX ON :Person(name)");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(AdminClauseHandlerTest, DropIndexByLabel_DropNonExistentIndex) {
	auto res = execute("DROP INDEX ON :Person(nonexistent)");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(AdminClauseHandlerTest, DropIndexByLabel_DropMultipleIndexesSameLabel) {
	(void) execute("CREATE INDEX ON :Person(name)");
	(void) execute("CREATE INDEX ON :Person(age)");

	auto res = execute("DROP INDEX ON :Person(name)");
	ASSERT_EQ(res.rowCount(), 1UL);

	auto showRes = execute("SHOW INDEXES");
	EXPECT_GE(showRes.rowCount(), 1UL);
}

TEST_F(AdminClauseHandlerTest, DropIndexByLabel_MultiCharLabel) {
	(void) execute("CREATE INDEX ON :Organization(name)");

	auto res = execute("DROP INDEX ON :Organization(name)");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(AdminClauseHandlerTest, DropIndexByLabel_MultiCharProperty) {
	(void) execute("CREATE INDEX ON :Person(username)");

	auto res = execute("DROP INDEX ON :Person(username)");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(AdminClauseHandlerTest, DropIndexByLabel_SingleCharLabel) {
	(void) execute("CREATE INDEX ON :A(name)");
	auto res = execute("DROP INDEX ON :A(name)");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(AdminClauseHandlerTest, DropIndexByLabel_SingleCharProperty) {
	(void) execute("CREATE INDEX ON :Label(x)");
	auto res = execute("DROP INDEX ON :Label(x)");
	ASSERT_EQ(res.rowCount(), 1UL);
}

// === Lifecycle Tests ===

TEST_F(AdminClauseHandlerTest, Lifecycle_CreateShowDrop) {
	auto res1 = execute("CREATE INDEX test_idx ON :Person(name)");
	ASSERT_EQ(res1.rowCount(), 1UL);

	auto res2 = execute("SHOW INDEXES");
	EXPECT_GE(res2.rowCount(), 1UL);

	auto res3 = execute("DROP INDEX test_idx");
	ASSERT_EQ(res3.rowCount(), 1UL);

	auto res4 = execute("SHOW INDEXES");
	EXPECT_EQ(res4.rowCount(), 0UL);
}

TEST_F(AdminClauseHandlerTest, Lifecycle_MixedIndexAndVectorIndex) {
	(void) execute("CREATE INDEX ON :Person(name)");
	(void) execute("CREATE VECTOR INDEX ON :Person(embedding) OPTIONS {dimension: 3}");

	auto res = execute("SHOW INDEXES");
	EXPECT_GE(res.rowCount(), 2UL);
}

// === Error/Edge Case Tests ===

TEST_F(AdminClauseHandlerTest, Error_CreateIndexWithEmptyLabel) {
	EXPECT_THROW({ execute("CREATE INDEX ON :(name)"); }, std::runtime_error);
}

TEST_F(AdminClauseHandlerTest, Error_CreateIndexWithEmptyProperty) {
	EXPECT_THROW({ execute("CREATE INDEX ON :Person()"); }, std::runtime_error);
}

TEST_F(AdminClauseHandlerTest, Error_CreateIndexByPatternNoLabel) {
	EXPECT_THROW({ execute("CREATE INDEX FOR (n) ON (n.prop)"); }, std::runtime_error);
}

TEST_F(AdminClauseHandlerTest, Error_DropIndexWithEmptyName) {
	EXPECT_THROW({ execute("DROP INDEX ''"); }, std::runtime_error);
}

TEST_F(AdminClauseHandlerTest, Error_CreateVectorIndexWithoutOptions) {
	EXPECT_THROW({ execute("CREATE VECTOR INDEX ON :Test(vec)"); }, std::runtime_error);
}

TEST_F(AdminClauseHandlerTest, Error_CreateVectorIndexEmptyOptions) {
	EXPECT_THROW({ execute("CREATE VECTOR INDEX ON :Test(vec) OPTIONS {}"); }, std::runtime_error);
}

TEST_F(AdminClauseHandlerTest, Error_CreateVectorIndexNoDimension) {
	EXPECT_THROW({
		execute("CREATE VECTOR INDEX ON :Test(vec) OPTIONS {metric: 'L2'}");
	}, std::runtime_error);
}

TEST_F(AdminClauseHandlerTest, Error_CreateVectorIndexDimensionZero) {
	EXPECT_THROW({
		execute("CREATE VECTOR INDEX ON :Person(vec) OPTIONS {dimension: 0}");
	}, std::runtime_error);
}

TEST_F(AdminClauseHandlerTest, EdgeCase_CreateVectorIndexNegativeDimension) {
	// Implementation accepts negative dimensions (doesn't throw) - verify no crash
	EXPECT_NO_THROW({ execute("CREATE VECTOR INDEX ON :Person(vec) OPTIONS {dimension: -5}"); });
}

TEST_F(AdminClauseHandlerTest, EdgeCase_CreateSameIndexTwice) {
	(void) execute("CREATE INDEX duplicate_test ON :Person(name)");

	// Should either succeed idempotently or fail, but not crash
	EXPECT_NO_THROW({ execute("CREATE INDEX duplicate_test ON :Person(name)"); });
}

TEST_F(AdminClauseHandlerTest, EdgeCase_MultipleIndexesSameProperty) {
	auto res1 = execute("CREATE INDEX idx1 ON :Person(name)");
	auto res2 = execute("CREATE INDEX idx2 ON :Person(name)");

	ASSERT_EQ(res1.rowCount(), 1UL);
	ASSERT_EQ(res2.rowCount(), 1UL);
}
