/**
 * @file test_ApiParameterizedQueries.cpp
 * @brief Tests for parameterized queries with inline property patterns.
 *
 * Covers:
 * - CREATE (n:L {prop: $p}) with parameter values stored correctly
 * - MATCH (n:L {prop: $p}) with inline property parameter resolution
 * - Traversal with parameterized node/edge properties
 * - Multiple parameter types (string, int, double, bool)
 *
 * Licensed under the Apache License, Version 2.0
 **/

#include <gtest/gtest.h>
#include "ApiTestFixture.hpp"

// =============================================================================
// Parameterized CREATE — verify properties are stored
// =============================================================================

TEST_F(CppApiTest, CreateWithParamString_PropertyStored) {
	auto res = db->execute(
		"CREATE (n:Person {name: $name}) RETURN n.name AS name",
		{{"name", std::string("Alice")}}
	);
	ASSERT_TRUE(res.isSuccess());
	ASSERT_TRUE(res.hasNext());
	res.next();
	auto val = res.get("name");
	ASSERT_TRUE(std::holds_alternative<std::string>(val));
	EXPECT_EQ(std::get<std::string>(val), "Alice");
}

TEST_F(CppApiTest, CreateWithParamInt_PropertyStored) {
	auto res = db->execute(
		"CREATE (n:T {x: $x}) RETURN n.x AS x",
		{{"x", (int64_t)42}}
	);
	ASSERT_TRUE(res.isSuccess());
	ASSERT_TRUE(res.hasNext());
	res.next();
	auto val = res.get("x");
	ASSERT_TRUE(std::holds_alternative<int64_t>(val));
	EXPECT_EQ(std::get<int64_t>(val), 42);
}

TEST_F(CppApiTest, CreateWithParamDouble_PropertyStored) {
	auto res = db->execute(
		"CREATE (n:T {x: $x}) RETURN n.x AS x",
		{{"x", 3.14}}
	);
	ASSERT_TRUE(res.isSuccess());
	ASSERT_TRUE(res.hasNext());
	res.next();
	auto val = res.get("x");
	ASSERT_TRUE(std::holds_alternative<double>(val));
	EXPECT_DOUBLE_EQ(std::get<double>(val), 3.14);
}

TEST_F(CppApiTest, CreateWithParamBool_PropertyStored) {
	auto res = db->execute(
		"CREATE (n:T {active: $active}) RETURN n.active AS active",
		{{"active", true}}
	);
	ASSERT_TRUE(res.isSuccess());
	ASSERT_TRUE(res.hasNext());
	res.next();
	auto val = res.get("active");
	ASSERT_TRUE(std::holds_alternative<bool>(val));
	EXPECT_TRUE(std::get<bool>(val));
}

TEST_F(CppApiTest, CreateWithParam_ThenMatchVerify) {
	// CREATE with params, then MATCH with literal to verify stored value
	(void)db->execute(
		"CREATE (n:Person {name: $name, age: $age})",
		{{"name", std::string("Alice")}, {"age", (int64_t)30}}
	);

	auto res = db->execute("MATCH (n:Person {name: 'Alice'}) RETURN n.age AS age");
	ASSERT_TRUE(res.isSuccess());
	ASSERT_TRUE(res.hasNext());
	res.next();
	auto val = res.get("age");
	ASSERT_TRUE(std::holds_alternative<int64_t>(val));
	EXPECT_EQ(std::get<int64_t>(val), 30);
}

// =============================================================================
// Parameterized MATCH — inline property pattern with $param
// =============================================================================

TEST_F(CppApiTest, MatchInlineParam_String) {
	(void)db->execute("CREATE (n:Person {name: 'Alice', age: 30})");
	(void)db->execute("CREATE (n:Person {name: 'Bob', age: 25})");

	auto res = db->execute(
		"MATCH (n:Person {name: $name}) RETURN n.age AS age",
		{{"name", std::string("Alice")}}
	);
	ASSERT_TRUE(res.isSuccess());
	ASSERT_TRUE(res.hasNext());
	res.next();
	auto val = res.get("age");
	ASSERT_TRUE(std::holds_alternative<int64_t>(val));
	EXPECT_EQ(std::get<int64_t>(val), 30);
	EXPECT_FALSE(res.hasNext()); // Only Alice matches
}

TEST_F(CppApiTest, MatchInlineParam_Int) {
	(void)db->execute("CREATE (n:T {x: 42, label: 'first'})");
	(void)db->execute("CREATE (n:T {x: 99, label: 'second'})");

	auto res = db->execute(
		"MATCH (n:T {x: $val}) RETURN n.label AS label",
		{{"val", (int64_t)42}}
	);
	ASSERT_TRUE(res.isSuccess());
	ASSERT_TRUE(res.hasNext());
	res.next();
	auto val = res.get("label");
	ASSERT_TRUE(std::holds_alternative<std::string>(val));
	EXPECT_EQ(std::get<std::string>(val), "first");
	EXPECT_FALSE(res.hasNext());
}

TEST_F(CppApiTest, MatchInlineParam_Double) {
	(void)db->execute("CREATE (n:T {score: 9.5, name: 'good'})");
	(void)db->execute("CREATE (n:T {score: 3.0, name: 'bad'})");

	auto res = db->execute(
		"MATCH (n:T {score: $s}) RETURN n.name AS name",
		{{"s", 9.5}}
	);
	ASSERT_TRUE(res.isSuccess());
	ASSERT_TRUE(res.hasNext());
	res.next();
	auto val = res.get("name");
	ASSERT_TRUE(std::holds_alternative<std::string>(val));
	EXPECT_EQ(std::get<std::string>(val), "good");
	EXPECT_FALSE(res.hasNext());
}

TEST_F(CppApiTest, MatchInlineParam_Bool) {
	(void)db->execute("CREATE (n:T {active: true, name: 'on'})");
	(void)db->execute("CREATE (n:T {active: false, name: 'off'})");

	auto res = db->execute(
		"MATCH (n:T {active: $a}) RETURN n.name AS name",
		{{"a", true}}
	);
	ASSERT_TRUE(res.isSuccess());
	ASSERT_TRUE(res.hasNext());
	res.next();
	auto val = res.get("name");
	ASSERT_TRUE(std::holds_alternative<std::string>(val));
	EXPECT_EQ(std::get<std::string>(val), "on");
	EXPECT_FALSE(res.hasNext());
}

TEST_F(CppApiTest, MatchInlineParam_MultipleProperties) {
	(void)db->execute("CREATE (n:Person {name: 'Alice', age: 30})");
	(void)db->execute("CREATE (n:Person {name: 'Alice', age: 25})");
	(void)db->execute("CREATE (n:Person {name: 'Bob', age: 30})");

	auto res = db->execute(
		"MATCH (n:Person {name: $name, age: $age}) RETURN n.name AS name, n.age AS age",
		{{"name", std::string("Alice")}, {"age", (int64_t)30}}
	);
	ASSERT_TRUE(res.isSuccess());
	ASSERT_TRUE(res.hasNext());
	res.next();
	EXPECT_EQ(std::get<std::string>(res.get("name")), "Alice");
	EXPECT_EQ(std::get<int64_t>(res.get("age")), 30);
	EXPECT_FALSE(res.hasNext()); // Only one match
}

TEST_F(CppApiTest, MatchInlineParam_NoMatch) {
	(void)db->execute("CREATE (n:Person {name: 'Alice'})");

	auto res = db->execute(
		"MATCH (n:Person {name: $name}) RETURN n.name AS name",
		{{"name", std::string("NonExistent")}}
	);
	ASSERT_TRUE(res.isSuccess());
	EXPECT_FALSE(res.hasNext());
}

TEST_F(CppApiTest, MatchInlineParam_MixedLiteralAndParam) {
	(void)db->execute("CREATE (n:Person {name: 'Alice', city: 'NYC', age: 30})");
	(void)db->execute("CREATE (n:Person {name: 'Bob', city: 'NYC', age: 25})");

	// Mix literal 'NYC' with parameter $name
	auto res = db->execute(
		"MATCH (n:Person {city: 'NYC', name: $name}) RETURN n.age AS age",
		{{"name", std::string("Alice")}}
	);
	ASSERT_TRUE(res.isSuccess());
	ASSERT_TRUE(res.hasNext());
	res.next();
	EXPECT_EQ(std::get<int64_t>(res.get("age")), 30);
	EXPECT_FALSE(res.hasNext());
}

// =============================================================================
// Parameterized MATCH in traversals
// =============================================================================

TEST_F(CppApiTest, MatchTraversal_ParamOnSourceNode) {
	(void)db->execute("CREATE (a:Person {name: 'Alice'})-[:KNOWS]->(b:Person {name: 'Bob'})");
	(void)db->execute("CREATE (c:Person {name: 'Carol'})-[:KNOWS]->(d:Person {name: 'Dave'})");

	auto res = db->execute(
		"MATCH (a:Person {name: $name})-[:KNOWS]->(b:Person) RETURN b.name AS friend",
		{{"name", std::string("Alice")}}
	);
	ASSERT_TRUE(res.isSuccess());
	ASSERT_TRUE(res.hasNext());
	res.next();
	EXPECT_EQ(std::get<std::string>(res.get("friend")), "Bob");
	EXPECT_FALSE(res.hasNext());
}

TEST_F(CppApiTest, MatchTraversal_ParamOnTargetNode) {
	(void)db->execute("CREATE (a:Person {name: 'Alice'})-[:KNOWS]->(b:Person {name: 'Bob'})");
	(void)db->execute("CREATE (a:Person {name: 'Alice'})-[:KNOWS]->(c:Person {name: 'Carol'})");

	auto res = db->execute(
		"MATCH (a:Person)-[:KNOWS]->(b:Person {name: $target}) RETURN a.name AS person",
		{{"target", std::string("Bob")}}
	);
	ASSERT_TRUE(res.isSuccess());
	ASSERT_TRUE(res.hasNext());
	res.next();
	EXPECT_EQ(std::get<std::string>(res.get("person")), "Alice");
	EXPECT_FALSE(res.hasNext());
}

TEST_F(CppApiTest, MatchTraversal_ParamOnBothNodes) {
	(void)db->execute("CREATE (a:Person {name: 'Alice'})-[:KNOWS]->(b:Person {name: 'Bob'})");
	(void)db->execute("CREATE (a:Person {name: 'Alice'})-[:KNOWS]->(c:Person {name: 'Carol'})");
	(void)db->execute("CREATE (d:Person {name: 'Dave'})-[:KNOWS]->(e:Person {name: 'Bob'})");

	auto res = db->execute(
		"MATCH (a:Person {name: $src})-[:KNOWS]->(b:Person {name: $dst}) RETURN a.name AS src, b.name AS dst",
		{{"src", std::string("Alice")}, {"dst", std::string("Bob")}}
	);
	ASSERT_TRUE(res.isSuccess());
	ASSERT_TRUE(res.hasNext());
	res.next();
	EXPECT_EQ(std::get<std::string>(res.get("src")), "Alice");
	EXPECT_EQ(std::get<std::string>(res.get("dst")), "Bob");
	EXPECT_FALSE(res.hasNext());
}

// =============================================================================
// Combined CREATE + MATCH with parameters (end-to-end)
// =============================================================================

TEST_F(CppApiTest, CreateThenMatchWithParams_EndToEnd) {
	// Create with params
	(void)db->execute("CREATE (n:Person {name: $name, age: $age})",
		{{"name", std::string("Alice")}, {"age", (int64_t)30}});
	(void)db->execute("CREATE (n:Person {name: $name, age: $age})",
		{{"name", std::string("Bob")}, {"age", (int64_t)25}});

	// MATCH with inline params
	auto res = db->execute(
		"MATCH (n:Person {name: $name}) RETURN n.age AS age",
		{{"name", std::string("Bob")}}
	);
	ASSERT_TRUE(res.isSuccess());
	ASSERT_TRUE(res.hasNext());
	res.next();
	EXPECT_EQ(std::get<int64_t>(res.get("age")), 25);
	EXPECT_FALSE(res.hasNext());
}

TEST_F(CppApiTest, CreateAndMatchInTraversal_FullParamPipeline) {
	// Create graph with params
	(void)db->execute("CREATE (n:Person {name: $name})", {{"name", std::string("Alice")}});
	(void)db->execute("CREATE (n:Person {name: $name})", {{"name", std::string("Bob")}});
	(void)db->execute("CREATE (n:Movie {title: $title})", {{"title", std::string("Matrix")}});

	// Create edge using literal MATCH for cross-product join
	(void)db->execute(
		"MATCH (a:Person {name: 'Alice'}), (m:Movie {title: 'Matrix'}) "
		"CREATE (a)-[:RATED {score: 9.5}]->(m)"
	);

	// Query back with param on source node in traversal
	auto res = db->execute(
		"MATCH (p:Person {name: $name})-[r:RATED]->(m:Movie) "
		"RETURN m.title AS title, r.score AS score",
		{{"name", std::string("Alice")}}
	);
	ASSERT_TRUE(res.isSuccess());
	ASSERT_TRUE(res.hasNext());
	res.next();
	EXPECT_EQ(std::get<std::string>(res.get("title")), "Matrix");
	EXPECT_DOUBLE_EQ(std::get<double>(res.get("score")), 9.5);
	EXPECT_FALSE(res.hasNext());
}
