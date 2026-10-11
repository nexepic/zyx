/**
 * @file test_PreparedQuery.cpp
 * @brief Public native QueryBuilder acceptance tests.
 *
 * Licensed under the Apache License, Version 2.0.
 */

#include <algorithm>
#include <type_traits>

#include <gtest/gtest.h>

#include "ApiTestFixture.hpp"

static_assert(!std::is_copy_constructible_v<zyx::PreparedQuery>);
static_assert(!std::is_copy_assignable_v<zyx::PreparedQuery>);
static_assert(std::is_move_constructible_v<zyx::PreparedQuery>);
static_assert(!std::is_copy_constructible_v<zyx::QueryBuilder>);
static_assert(!std::is_copy_assignable_v<zyx::QueryBuilder>);
static_assert(std::is_move_constructible_v<zyx::QueryBuilder>);

TEST_F(CppApiTest, PreparedQueryCreatesConnectedGraphWithTypedBoundProperties) {
	const std::string hostileValue = "Ada' }) CREATE (:Unexpected) //";
	zyx::PreparedQuery prepared = db->query()
			.createNode("source", "BuilderPerson", {{"name", hostileValue}, {"score", int64_t{7}}})
			.createNode("target", "BuilderPerson", {{"name", std::string("Bob")}})
			.createEdge("relationship", "BUILDER_KNOWS", "source", "target", {{"weight", int64_t{3}}})
			.returning({"source", "relationship", "target"})
			.build();

	ASSERT_FALSE(prepared.empty());
	const zyx::Result created = db->execute(prepared);
	ASSERT_TRUE(created.isSuccess()) << created.getError();
	ASSERT_TRUE(created.hasNext());
	created.next();
	const auto source = std::get<std::shared_ptr<zyx::Node>>(created.get("source"));
	const auto relationship = std::get<std::shared_ptr<zyx::Edge>>(created.get("relationship"));
	const auto target = std::get<std::shared_ptr<zyx::Node>>(created.get("target"));
	ASSERT_NE(source, nullptr);
	ASSERT_NE(relationship, nullptr);
	ASSERT_NE(target, nullptr);
	EXPECT_EQ(std::get<std::string>(source->properties.at("name")), hostileValue);
	EXPECT_EQ(std::get<int64_t>(relationship->properties.at("weight")), 3);
	EXPECT_EQ(std::get<std::string>(target->properties.at("name")), "Bob");
	EXPECT_FALSE(created.hasNext());

	const zyx::Result match = db->execute(
			db->query().match("node", "BuilderPerson", "name", hostileValue).returning({"node"}).build());
	ASSERT_TRUE(match.isSuccess()) << match.getError();
	ASSERT_TRUE(match.hasNext());
	match.next();
	const auto matched = std::get<std::shared_ptr<zyx::Node>>(match.get("node"));
	ASSERT_NE(matched, nullptr);
	EXPECT_EQ(matched->id, source->id);
	EXPECT_FALSE(match.hasNext());

	const zyx::Result unexpected = db->execute("MATCH (n:Unexpected) RETURN n");
	ASSERT_TRUE(unexpected.isSuccess()) << unexpected.getError();
	EXPECT_FALSE(unexpected.hasNext());
}

TEST_F(CppApiTest, PreparedQueryIsReusableAndReadsTransactionOverlay) {
	const int64_t node = db->createNode("PreparedRead", {{"score", int64_t{1}}});
	ASSERT_GT(node, 0);
	// SET needs an explicit source; its builder-state failure is raised before
	// execution and therefore cannot publish a partial mutation.
	EXPECT_THROW((void)db->query().setProperty("n", "score", int64_t{9}).build(), std::logic_error);

	zyx::PreparedQuery update = db->query()
			.match("n", "PreparedRead")
			.setProperty("n", "score", int64_t{9})
			.returning({"n"})
			.build();
	const zyx::Result first = db->execute(update);
	ASSERT_TRUE(first.isSuccess()) << first.getError();
	const zyx::Result second = db->execute(update);
	ASSERT_TRUE(second.isSuccess()) << second.getError();
	const zyx::Result updated = db->execute(db->query().match("n", "PreparedRead").returning({"n"}).build());
	ASSERT_TRUE(updated.isSuccess()) << updated.getError();
	ASSERT_TRUE(updated.hasNext());
	updated.next();
	EXPECT_EQ(std::get<int64_t>(std::get<std::shared_ptr<zyx::Node>>(updated.get("n"))->properties.at("score")), 9);

	const zyx::Result selfPath = db->execute(
			db->query().call("algo.shortestPath", std::vector<zyx::Value>{node, node, int64_t{1}}).build());
	ASSERT_TRUE(selfPath.isSuccess()) << selfPath.getError();
	EXPECT_TRUE(selfPath.hasNext());

	auto transaction = db->beginTransaction();
	zyx::PreparedQuery staged = transaction.query()
			.createNode("staged", "PreparedTransaction", {{"name", std::string("visible")}})
			.returning({"staged"})
			.build();
	const zyx::Result stagedResult = transaction.execute(staged);
	ASSERT_TRUE(stagedResult.isSuccess()) << stagedResult.getError();
	const zyx::Result hidden = db->execute(
			db->query().match("n", "PreparedTransaction").returning({"n"}).build());
	ASSERT_TRUE(hidden.isSuccess()) << hidden.getError();
	EXPECT_FALSE(hidden.hasNext());
	transaction.commit();

	const zyx::Result visible = db->execute(
			db->query().match("n", "PreparedTransaction", "name", std::string("visible")).returning({"n"}).build());
	ASSERT_TRUE(visible.isSuccess()) << visible.getError();
	EXPECT_TRUE(visible.hasNext());
}

TEST_F(CppApiTest, PreparedQuerySupportsCartesianMatchAndUnwindPagination) {
	static_cast<void>(db->createNode("PreparedCartesian", {{"name", std::string("first")}}));
	static_cast<void>(db->createNode("PreparedCartesian", {{"name", std::string("second")}}));

	const zyx::Result pairs = db->execute(
			db->query()
					.match("left", "PreparedCartesian", "name", std::string("first"))
					.match("right", "PreparedCartesian", "name", std::string("second"))
					.returning({"left", "right"})
					.build());
	ASSERT_TRUE(pairs.isSuccess()) << pairs.getError();
	ASSERT_TRUE(pairs.hasNext());
	pairs.next();
	EXPECT_NE(std::get<std::shared_ptr<zyx::Node>>(pairs.get("left"))->id,
			  std::get<std::shared_ptr<zyx::Node>>(pairs.get("right"))->id);
	EXPECT_FALSE(pairs.hasNext());

	const zyx::Result values = db->execute(
			db->query().unwind({int64_t{1}, int64_t{2}, int64_t{3}}, "value").returning({"value"}).skip(1).limit(1).build());
	ASSERT_TRUE(values.isSuccess()) << values.getError();
	ASSERT_TRUE(values.hasNext());
	values.next();
	EXPECT_EQ(std::get<int64_t>(values.get("value")), 2);
	EXPECT_FALSE(values.hasNext());
}

TEST_F(CppApiTest, PreparedQueryCoversUpdateDeleteAndIndexAdministration) {
	static_cast<void>(db->createNode("PreparedMutation", {{"keep", int64_t{1}}, {"remove", int64_t{2}}}));

	const zyx::Result updated = db->execute(
			db->query()
					.match("n", "PreparedMutation")
					.setProperty("n", "keep", int64_t{9})
					.setProperty("n", "active", true)
					.setProperty("n", "score", 4.5)
					.setProperty("n", "remove", zyx::Value{})
					.addLabel("n", "PreparedMutationApplied")
					.returning({"n"})
					.build());
	ASSERT_TRUE(updated.isSuccess()) << updated.getError();
	ASSERT_TRUE(updated.hasNext());
	updated.next();
	const auto node = std::get<std::shared_ptr<zyx::Node>>(updated.get("n"));
	ASSERT_NE(node, nullptr);
	EXPECT_EQ(std::get<int64_t>(node->properties.at("keep")), 9);
	EXPECT_EQ(std::get<bool>(node->properties.at("active")), true);
	EXPECT_DOUBLE_EQ(std::get<double>(node->properties.at("score")), 4.5);
	EXPECT_FALSE(node->properties.contains("remove"));
	EXPECT_NE(std::find(node->labels.begin(), node->labels.end(), "PreparedMutationApplied"), node->labels.end());

	const zyx::Result labelRemoved = db->execute(
			db->query().match("n", "PreparedMutationApplied").remove("n", "PreparedMutationApplied", true).returning({"n"}).build());
	ASSERT_TRUE(labelRemoved.isSuccess()) << labelRemoved.getError();
	ASSERT_TRUE(labelRemoved.hasNext());
	labelRemoved.next();
	const auto labelRemovedNode = std::get<std::shared_ptr<zyx::Node>>(labelRemoved.get("n"));
	ASSERT_NE(labelRemovedNode, nullptr);
	EXPECT_EQ(std::find(labelRemovedNode->labels.begin(), labelRemovedNode->labels.end(), "PreparedMutationApplied"),
			  labelRemovedNode->labels.end());

	const zyx::Result createdIndex = db->execute(db->query().createIndex(
			"PreparedMutationApplied", "keep", "prepared_mutation_keep").build());
	ASSERT_TRUE(createdIndex.isSuccess()) << createdIndex.getError();
	const zyx::Result indexes = db->execute(db->query().showIndexes().build());
	ASSERT_TRUE(indexes.isSuccess()) << indexes.getError();
	ASSERT_TRUE(indexes.hasNext());

	const zyx::Result droppedIndex = db->execute(db->query().dropIndex("prepared_mutation_keep").build());
	ASSERT_TRUE(droppedIndex.isSuccess()) << droppedIndex.getError();
	const zyx::Result deleted = db->execute(
			db->query().match("n", "PreparedMutation").deleteEntities({"n"}).build());
	ASSERT_TRUE(deleted.isSuccess()) << deleted.getError();
	const zyx::Result missing = db->execute(db->query().match("n", "PreparedMutation").returning({"n"}).build());
	ASSERT_TRUE(missing.isSuccess()) << missing.getError();
	EXPECT_FALSE(missing.hasNext());
}

TEST_F(CppApiTest, PreparedQueryRejectsIdentifierInjectionBeforeExecution) {
	EXPECT_THROW((void)db->query().createNode("valid", "Invalid;Label").build(), std::invalid_argument);
	EXPECT_THROW((void)db->query().match("invalid variable", "ValidLabel").build(), std::invalid_argument);
	EXPECT_THROW((void)db->query().match("1invalid", "ValidLabel").build(), std::invalid_argument);
	EXPECT_THROW((void)db->query().call("gds.graph.project; DROP").build(), std::invalid_argument);
	EXPECT_THROW((void)db->query().call("").build(), std::invalid_argument);
	EXPECT_THROW((void)db->query().call(".gds").build(), std::invalid_argument);
	EXPECT_THROW((void)db->query().call("gds.").build(), std::invalid_argument);
	EXPECT_THROW((void)db->query().call("gds..project").build(), std::invalid_argument);
	EXPECT_THROW((void)db->query().call(std::string(257U, 'a')).build(), std::invalid_argument);

	const zyx::Result noMutation = db->execute(db->query().match("n", "Invalid").returning({"n"}).build());
	ASSERT_TRUE(noMutation.isSuccess()) << noMutation.getError();
	EXPECT_FALSE(noMutation.hasNext());
}

TEST_F(CppApiTest, PreparedQueryBindsNestedValuesAndBytesWithoutFormattingCypher) {
	auto nestedList = std::make_shared<zyx::ValueList>();
	nestedList->elements = {int64_t{7}, std::string("nested"), zyx::Bytes{0U, 0xFFU}};
	auto nestedMap = std::make_shared<zyx::ValueMap>();
	nestedMap->entries.emplace("enabled", true);
	nestedMap->entries.emplace("items", nestedList);
	std::shared_ptr<zyx::ValueList> nullList;
	std::shared_ptr<zyx::ValueMap> nullMap;
	const zyx::Bytes bytes{0U, 1U, 0x7FU, 0x80U, 0xFFU};

	const zyx::Result result = db->execute(
			db->query()
					.createNode("node", "PreparedNested",
								{{"floats", std::vector<float>{1.25F, -2.5F}},
								 {"strings", std::vector<std::string>{"first", "second"}},
								 {"list", nestedList},
								 {"map", nestedMap},
								 {"nullList", nullList},
								 {"nullMap", nullMap},
								 {"bytes", bytes}})
					.returning({"node"})
					.build());
	ASSERT_TRUE(result.isSuccess()) << result.getError();
	ASSERT_TRUE(result.hasNext());
	result.next();
	const auto node = std::get<std::shared_ptr<zyx::Node>>(result.get("node"));
	ASSERT_NE(node, nullptr);
	EXPECT_EQ(std::get<zyx::Bytes>(node->properties.at("bytes")), bytes);

	const auto floats = std::get<std::vector<std::string>>(node->properties.at("floats"));
	ASSERT_EQ(floats.size(), 2U);
	EXPECT_NE(floats[0].find("1.25"), std::string::npos);
	EXPECT_NE(floats[1].find("-2.5"), std::string::npos);

	const auto strings = std::get<std::vector<std::string>>(node->properties.at("strings"));
	EXPECT_EQ(strings, (std::vector<std::string>{"first", "second"}));

	const auto list = std::get<std::vector<std::string>>(node->properties.at("list"));
	EXPECT_EQ(list, (std::vector<std::string>{"7", "nested", "0x00ff"}));

	const auto map = std::get<std::string>(node->properties.at("map"));
	EXPECT_NE(map.find("enabled"), std::string::npos);
	EXPECT_NE(map.find("nested"), std::string::npos);
	EXPECT_TRUE(std::holds_alternative<std::monostate>(node->properties.at("nullList")));
	EXPECT_TRUE(std::holds_alternative<std::monostate>(node->properties.at("nullMap")));
}

TEST_F(CppApiTest, PreparedQueryRejectsGraphEntityBoundValuesBeforeExecution) {
	auto node = std::make_shared<zyx::Node>();
	auto edge = std::make_shared<zyx::Edge>();
	EXPECT_THROW((void)db->query().createNode("created", "PreparedInvalidValue", {{"entity", node}}),
			 std::invalid_argument);
	EXPECT_THROW((void)db->query().createNode("created", "PreparedInvalidValue", {{"entity", edge}}),
			 std::invalid_argument);

	const zyx::Result absent = db->execute(
			db->query().match("node", "PreparedInvalidValue").returning({"node"}).build());
	ASSERT_TRUE(absent.isSuccess()) << absent.getError();
	EXPECT_FALSE(absent.hasNext());
}

TEST_F(CppApiTest, PreparedQueryMovedBuilderRejectsFurtherUseOfTheMovedFromObject) {
	auto builder = db->query();
	auto movedBuilder = std::move(builder);
	EXPECT_THROW((void)builder.build(), std::logic_error);
	EXPECT_THROW((void)builder.createNode("node", "PreparedMovedBuilder"), std::logic_error);

	const zyx::PreparedQuery empty = movedBuilder.build();
	EXPECT_TRUE(empty.empty());
}

TEST_F(CppApiTest, PreparedQueryMoveOperationsRetainTheLogicalPlanExactlyOnce) {
	zyx::PreparedQuery first = db->query().createNode("node", "PreparedMovePlan").returning({"node"}).build();
	zyx::PreparedQuery moved = std::move(first);
	EXPECT_TRUE(first.empty());
	EXPECT_FALSE(moved.empty());

	zyx::PreparedQuery assigned = db->query().build();
	assigned = std::move(moved);
	EXPECT_TRUE(moved.empty());
	EXPECT_FALSE(assigned.empty());
	const zyx::Result result = db->execute(assigned);
	ASSERT_TRUE(result.isSuccess()) << result.getError();
	ASSERT_TRUE(result.hasNext());

	auto sourceBuilder = db->query();
	auto destinationBuilder = db->query();
	destinationBuilder = std::move(sourceBuilder);
	EXPECT_THROW((void)sourceBuilder.build(), std::logic_error);
	EXPECT_TRUE(destinationBuilder.build().empty());
}

TEST_F(CppApiTest, PreparedQueryRejectsAnExcessiveBoundValueCountBeforePublishing) {
	std::unordered_map<std::string, zyx::Value> properties;
	properties.reserve(65'537U);
	for (size_t index = 0U; index <= 65'536U; ++index) {
		properties.emplace("value_" + std::to_string(index), int64_t{1});
	}
	EXPECT_THROW((void)db->query().createNode("node", "PreparedBoundLimit", properties), std::length_error);

	const zyx::Result absent =
			db->execute(db->query().match("node", "PreparedBoundLimit").returning({"node"}).build());
	ASSERT_TRUE(absent.isSuccess()) << absent.getError();
	EXPECT_FALSE(absent.hasNext());
}

TEST_F(CppApiTest, PreparedQueryPreservesMultiLabelFilterOrderAndAllBoundScalarTypes) {
	static_cast<void>(db->createNode(std::vector<std::string>{"PreparedMulti", "PreparedStaff"},
								 {{"rank", int64_t{1}}, {"active", true}, {"score", 1.5}}));
	static_cast<void>(db->createNode(std::vector<std::string>{"PreparedMulti", "PreparedStaff"},
								 {{"rank", int64_t{2}}, {"active", true}, {"score", 2.5}}));
	static_cast<void>(db->createNode("PreparedMulti", {{"rank", int64_t{3}}, {"active", false}, {"score", 3.5}}));

	const zyx::Result ordered = db->execute(
			db->query()
					.match("node", std::vector<std::string>{"PreparedMulti", "PreparedStaff"})
					.where("node", "active", true)
					.orderBy({{"node", "rank", false}})
					.returning({"node"})
					.build());
	ASSERT_TRUE(ordered.isSuccess()) << ordered.getError();
	ASSERT_TRUE(ordered.hasNext());
	ordered.next();
	const auto first = std::get<std::shared_ptr<zyx::Node>>(ordered.get("node"));
	ASSERT_NE(first, nullptr);
	EXPECT_EQ(std::get<int64_t>(first->properties.at("rank")), 2);
	EXPECT_EQ(std::get<bool>(first->properties.at("active")), true);
	EXPECT_DOUBLE_EQ(std::get<double>(first->properties.at("score")), 2.5);
	ASSERT_TRUE(ordered.hasNext());
	ordered.next();
	const auto second = std::get<std::shared_ptr<zyx::Node>>(ordered.get("node"));
	ASSERT_NE(second, nullptr);
	EXPECT_EQ(std::get<int64_t>(second->properties.at("rank")), 1);
	EXPECT_FALSE(ordered.hasNext());
}

TEST_F(CppApiTest, PreparedQueryHasStandaloneDdlResultAndErrorContracts) {
	const zyx::Result created = db->execute(
			db->query().createIndex("PreparedDdl", "rank", "prepared_ddl_rank").build());
	ASSERT_TRUE(created.isSuccess()) << created.getError();
	ASSERT_EQ(created.getColumnCount(), 1);
	EXPECT_EQ(created.getColumnName(0), "result");
	ASSERT_TRUE(created.hasNext());
	created.next();
	EXPECT_EQ(std::get<std::string>(created.get("result")), "Index created");
	EXPECT_FALSE(created.hasNext());

	const zyx::Result listed = db->execute(db->query().showIndexes().build());
	ASSERT_TRUE(listed.isSuccess()) << listed.getError();
	ASSERT_EQ(listed.getColumnCount(), 4);
	EXPECT_EQ(listed.getColumnName(0), "name");
	bool found = false;
	while (listed.hasNext()) {
		listed.next();
		found = found || std::get<std::string>(listed.get("name")) == "prepared_ddl_rank";
	}
	EXPECT_TRUE(found);

	const zyx::Result dropped = db->execute(db->query().dropIndex("PreparedDdl", "rank").build());
	ASSERT_TRUE(dropped.isSuccess()) << dropped.getError();
	ASSERT_TRUE(dropped.hasNext());
	dropped.next();
	EXPECT_EQ(std::get<std::string>(dropped.get("result")), "Index dropped");
	EXPECT_FALSE(dropped.hasNext());

	const zyx::Result absent = db->execute(db->query().dropIndex("prepared_ddl_rank").build());
	ASSERT_TRUE(absent.isSuccess()) << absent.getError();
	EXPECT_FALSE(absent.hasNext());

	const zyx::Result unsupported = db->execute(db->query().call("unknown.procedure").build());
	EXPECT_FALSE(unsupported.isSuccess());
	EXPECT_FALSE(unsupported.getError().empty());
}

TEST_F(CppApiTest, PreparedQueryRejectsInvalidStructuralStatesWithoutPublishingWork) {
	zyx::PreparedQuery empty = db->query().build();
	EXPECT_TRUE(empty.empty());
	const zyx::Result emptyResult = db->execute(empty);
	ASSERT_TRUE(emptyResult.isSuccess()) << emptyResult.getError();
	EXPECT_FALSE(emptyResult.hasNext());

	EXPECT_THROW((void)db->query().dropIndex("").build(), std::logic_error);
	EXPECT_THROW((void)db->query().unwind({int64_t{1}}, "value").deleteEntities({}).build(), std::logic_error);
	EXPECT_THROW((void)db->query().unwind({int64_t{1}}, "value").returning({}).build(), std::logic_error);
	EXPECT_THROW((void)db->query().unwind({int64_t{1}}, "value").orderBy({}).build(), std::logic_error);
	EXPECT_THROW((void)db->query().unwind({int64_t{1}}, "value").skip(-1).build(), std::logic_error);
	EXPECT_THROW((void)db->query().unwind({int64_t{1}}, "value").limit(-1).build(), std::logic_error);
	EXPECT_THROW((void)db->query().match("node", "PreparedInvalidState").showIndexes().build(), std::logic_error);
	EXPECT_THROW((void)db->query().match("node", "PreparedInvalidState").call("algo.shortestPath").build(),
			 std::logic_error);
	EXPECT_THROW((void)db->query().createNode("node", "PreparedMustNotPublish").showIndexes().build(),
			 std::logic_error);
	EXPECT_THROW((void)db->query().match("node", "PreparedInvalidState")
					 .createIndex("PreparedInvalidState", "value").build(),
			 std::logic_error);
	EXPECT_THROW((void)db->query().match("node", "PreparedInvalidState").dropIndex("prepared_invalid").build(),
			 std::logic_error);
	EXPECT_THROW((void)db->query().match("node", "PreparedInvalidState")
					 .dropIndex("PreparedInvalidState", "value").build(),
			 std::logic_error);

	const zyx::Result unchanged = db->execute(
			db->query().match("node", "PreparedMustNotPublish").returning({"node"}).build());
	ASSERT_TRUE(unchanged.isSuccess()) << unchanged.getError();
	EXPECT_FALSE(unchanged.hasNext());
}

TEST_F(CppApiTest, PreparedQuerySupportsDetachDelete) {
	const int64_t source = db->createNode("PreparedDetachSource");
	const int64_t target = db->createNode("PreparedDetachTarget");
	static_cast<void>(db->createEdge(source, target, "PREPARED_DETACH_EDGE"));

	const zyx::Result detached = db->execute(
			db->query().match("source", "PreparedDetachSource").deleteEntities({"source"}, true).build());
	ASSERT_TRUE(detached.isSuccess()) << detached.getError();

	const zyx::Result sourceMissing = db->execute(
			db->query().match("source", "PreparedDetachSource").returning({"source"}).build());
	ASSERT_TRUE(sourceMissing.isSuccess()) << sourceMissing.getError();
	EXPECT_FALSE(sourceMissing.hasNext());

	const zyx::Result edgeMissing = db->execute("MATCH ()-[edge:PREPARED_DETACH_EDGE]->() RETURN edge");
	ASSERT_TRUE(edgeMissing.isSuccess()) << edgeMissing.getError();
	EXPECT_FALSE(edgeMissing.hasNext());
}
