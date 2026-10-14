#include <atomic>
#include <chrono>
#include <limits>
#include <thread>
#include "storage/relocatable/backend/RelocatableStorageBackendTestFixture.hpp"
#include "graph/query/api/QueryEngine.hpp"
#include "graph/query/QueryGuard.hpp"
#include "graph/query/logical/operators/LogicalProject.hpp"
#include "graph/query/logical/operators/LogicalSingleRow.hpp"
#include "graph/query/expressions/Expression.hpp"

using namespace relocatable_storage_backend_test;

TEST_F(RelocatableStorageBackendTest, BoundSemanticsPreserveThreeValuedLogicNaNAndCheckedArithmetic) {
	materializeNativeDatabase(); graph::Database database(data_path_.string()); database.open(); auto engine = database.getQueryEngine();
	const auto result = engine->execute("RETURN NULL+1 AS arithmetic,NULL=1 AS equality,false AND NULL AS conjunction,true OR NULL AS disjunction,NULL IN [1,NULL] AS membership");
	ASSERT_EQ(result.rowCount(), 1U); const auto &row = result.getRows().front();
	EXPECT_EQ(row.at("arithmetic").asPrimitive(), PropertyValue()); EXPECT_EQ(row.at("equality").asPrimitive(), PropertyValue());
	EXPECT_EQ(row.at("conjunction").asPrimitive(), PropertyValue(false)); EXPECT_EQ(row.at("disjunction").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(row.at("membership").asPrimitive(), PropertyValue());
	graph::query::QueryContext context; context.parameters.emplace("x", PropertyValue((std::numeric_limits<double>::quiet_NaN)()));
	const auto nan = engine->execute("RETURN $x=$x AS equal,$x<=$x AS lessEqual,$x>=$x AS greaterEqual", context);
	ASSERT_EQ(nan.rowCount(), 1U); EXPECT_EQ(nan.getRows().front().at("equal").asPrimitive(), PropertyValue(false));
	EXPECT_EQ(nan.getRows().front().at("lessEqual").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(nan.getRows().front().at("greaterEqual").asPrimitive(), PropertyValue(true));
	context.parameters["x"] = PropertyValue((std::numeric_limits<int64_t>::max)());
	EXPECT_THROW(static_cast<void>(engine->execute("RETURN $x+1 AS value", context)), std::overflow_error);
	EXPECT_THROW(static_cast<void>(engine->execute("RETURN 1/0 AS value")), std::runtime_error);
	EXPECT_EQ(engine->execute("RETURN 7 AS value").getRows().front().at("value").asPrimitive(), PropertyValue(7));
}

TEST_F(RelocatableStorageBackendTest, BoundLocalMapScopesDoNotReuseOuterEntityPropertyCaches) {
	materializeNativeDatabase(); graph::Database database(data_path_.string()); database.open(); auto engine = database.getQueryEngine();
	engine->execute("CREATE (:Binding {rank:5})");
	const auto result = engine->execute("MATCH (n:Binding) WITH n,n.rank AS outside RETURN outside,[n IN [{rank:1},{rank:2}] | n.rank] AS local,reduce(n=0,x IN [1,2] | n+x) AS reduced");
	ASSERT_EQ(result.rowCount(), 1U); const auto &row = result.getRows().front();
	EXPECT_EQ(row.at("outside").asPrimitive(), PropertyValue(5));
	EXPECT_EQ(row.at("local").asPrimitive(), PropertyValue(std::vector<PropertyValue>{PropertyValue(1), PropertyValue(2)}));
	EXPECT_EQ(row.at("reduced").asPrimitive(), PropertyValue(3));
}

TEST_F(RelocatableStorageBackendTest, BoundPathsKeepParallelEdgesSelfLoopsAndRelationshipUniqueness) {
	materializeNativeDatabase(); graph::Database database(data_path_.string()); database.open(); auto engine = database.getQueryEngine();
	engine->execute("CREATE (a:Binding {id:1}),(b:Binding {id:2}),(c:Binding {id:3}) CREATE (a)-[:STEP]->(b),(a)-[:STEP]->(b),(b)-[:STEP]->(c),(a)-[:STEP]->(a)");
	const auto paths = engine->execute("MATCH p=(a:Binding {id:1})-[:STEP*1..2]->(b:Binding) RETURN b.id AS id,length(p) AS hops ORDER BY hops,id");
	ASSERT_EQ(paths.rowCount(), 7U);
	const std::array<int64_t, 7U> ids{1,2,2,2,2,3,3}, hops{1,1,1,2,2,2,2};
	for (size_t index = 0U; index < ids.size(); ++index) {
		EXPECT_EQ(paths.getRows()[index].at("id").asPrimitive(), PropertyValue(ids[index]));
		EXPECT_EQ(paths.getRows()[index].at("hops").asPrimitive(), PropertyValue(hops[index]));
	}
	const auto counted = engine->execute("MATCH (a:Binding {id:1})-[:STEP*1..2]->(b:Binding) RETURN count(*) AS paths");
	ASSERT_EQ(counted.rowCount(), 1U); EXPECT_EQ(counted.getRows().front().at("paths").asPrimitive(), PropertyValue(7));
	const auto undirected = engine->execute("MATCH (a:Binding {id:1})-[r:STEP]-(b) RETURN count(r) AS edges");
	ASSERT_EQ(undirected.rowCount(), 1U); EXPECT_EQ(undirected.getRows().front().at("edges").asPrimitive(), PropertyValue(3));
}

TEST_F(RelocatableStorageBackendTest, BoundBlockingOperatorsRetainDistinctMultiplicityAndNullOrdering) {
	materializeNativeDatabase(); graph::Database database(data_path_.string()); database.open(); auto engine = database.getQueryEngine();
	const auto result = engine->execute("UNWIND [2,NULL,1,2,NULL] AS x RETURN DISTINCT x ORDER BY x");
	ASSERT_EQ(result.rowCount(), 3U); EXPECT_EQ(result.getRows()[0].at("x").asPrimitive(), PropertyValue(1));
	EXPECT_EQ(result.getRows()[1].at("x").asPrimitive(), PropertyValue(2)); EXPECT_EQ(result.getRows()[2].at("x").asPrimitive(), PropertyValue());
	const auto grouped = engine->execute("UNWIND [2,NULL,1,2,NULL] AS x RETURN x,count(*) AS count ORDER BY x");
	ASSERT_EQ(grouped.rowCount(), 3U);
	EXPECT_EQ(grouped.getRows()[0].at("count").asPrimitive(), PropertyValue(1));
	EXPECT_EQ(grouped.getRows()[1].at("count").asPrimitive(), PropertyValue(2));
	EXPECT_EQ(grouped.getRows()[2].at("count").asPrimitive(), PropertyValue(2));
	const auto top = engine->execute("UNWIND [2,NULL,1,2,NULL] AS x RETURN x ORDER BY x DESC LIMIT 2");
	ASSERT_EQ(top.rowCount(), 2U); EXPECT_EQ(top.getRows()[0].at("x").asPrimitive(), PropertyValue());
	EXPECT_EQ(top.getRows()[1].at("x").asPrimitive(), PropertyValue());
}

TEST_F(RelocatableStorageBackendTest, BoundCancellationAndLowBudgetsLeaveReadOnlySnapshotsAndStoredGraphIntact) {
	materializeNativeDatabase(); graph::Database database(data_path_.string()); database.open(); auto engine = database.getQueryEngine();
	engine->execute("CREATE (:Binding {rank:5})");
	auto backend = std::dynamic_pointer_cast<LogicalGraphReadBackend>(database.getStorageBackend());
	auto pinned = backend->openLogicalGraphReadSnapshot(); const auto fence = pinned->mutationFence();
	graph::query::QueryGuard cancelled(1, 0); std::this_thread::sleep_for(std::chrono::milliseconds(2));
	graph::query::QueryContext context; context.guard = &cancelled; context.execMode = graph::query::ExecMode::EM_READ_ONLY;
	EXPECT_THROW(static_cast<void>(engine->execute("MATCH (n:Binding) RETURN n.rank", context)), graph::query::QueryTimeoutException);
	graph::query::LogicalReadExecutionOptions options; options.bounds.maximum_materialized_bytes = 4096U;
	graph::query::QueryEngine limited(backend, options); graph::query::QueryContext large;
	large.execMode = graph::query::ExecMode::EM_READ_ONLY; large.parameters.emplace("payload", PropertyValue(std::string(8192U, 'x')));
	EXPECT_THROW(static_cast<void>(limited.execute("RETURN $payload AS payload", large)), std::runtime_error);
	EXPECT_EQ(pinned->mutationFence(), fence); pinned.reset();
	EXPECT_EQ(engine->execute("MATCH (n:Binding) RETURN n.rank AS rank").getRows().front().at("rank").asPrimitive(), PropertyValue(5));
	std::atomic<size_t> sampled{0U}; graph::query::QueryGuard resident(0, 1, [&] { return sampled.load(); });
	sampled = 2U * 1024U * 1024U; context.guard = &resident;
	EXPECT_THROW(static_cast<void>(engine->execute("MATCH (n:Binding) RETURN n", context)), graph::query::QueryMemoryExceededException);
	database.close(); database.open(); EXPECT_EQ(database.getQueryEngine()->execute("MATCH (n:Binding) RETURN count(*) AS count").getRows().front().at("count").asPrimitive(), PropertyValue(1));
}

TEST_F(RelocatableStorageBackendTest, BoundImportedEntityRefreshAndNestedResultOwnershipSurviveNewRootsAndClose) {
	materializeNativeDatabase(); graph::Database database(data_path_.string()); database.open(); auto engine = database.getQueryEngine();
	engine->execute("CREATE (:Binding {rank:5})"); auto backend = std::dynamic_pointer_cast<LogicalGraphReadBackend>(database.getStorageBackend());
	auto before = backend->openLogicalGraphReadSnapshot(); const auto node = before->scanNodesAfter(0U, 10U, 65536U).nodes.at(0U);
	graph::query::execution::LogicalPlanReadRow input; input.values.emplace("n", node); input.property_cache["n"].emplace("rank", PropertyValue(5));
	engine->execute("MATCH (n:Binding) SET n.rank=9"); auto current = backend->openLogicalGraphReadSnapshot();
	std::vector<graph::query::logical::LogicalProjectItem> items{{std::make_shared<graph::query::expressions::VariableReferenceExpression>("n", "rank"), "rank"}};
	graph::query::logical::LogicalProject project(std::make_unique<graph::query::logical::LogicalSingleRow>(), std::move(items));
	graph::query::execution::LogicalPlanReadRuntime runtime(*backend); auto bounds = graph::query::LogicalReadExecutionOptions{}.bounds;
	graph::query::QueryContext context; int64_t observed = 0;
	const auto stats = runtime.executeWithSnapshotAndInputRow(*current, project, bounds, input, [&](const auto &row) {
		observed = std::get<int64_t>(std::get<PropertyValue>(row.values.at("rank")).getVariant()); return true;
	}, context);
	EXPECT_EQ(stats.rows_emitted, 1U); EXPECT_EQ(observed, 9); EXPECT_EQ(before->readNodeProperty(node.node_id, "rank"), PropertyValue(5));
	context.parameters.emplace("nested", PropertyValue(PropertyValue::MapType{{"values", PropertyValue(std::vector<PropertyValue>{PropertyValue(std::string(8192U, 'z')),PropertyValue()})}}));
	const auto result = engine->execute("RETURN $nested AS nested", context); current.reset(); before.reset(); database.close();
	EXPECT_EQ(result.getRows().front().at("nested").asPrimitive(), context.parameters.at("nested"));
}

TEST_F(RelocatableStorageBackendTest, BoundLoopTrailsMayRevisitNodesButNeverReuseAnEdge) {
	materializeNativeDatabase(); graph::Database database(data_path_.string()); database.open(); auto engine = database.getQueryEngine();
	engine->execute("CREATE (a:Loop) CREATE (a)-[:LOOP]->(a),(a)-[:LOOP]->(a)");
	const auto paths = engine->execute("MATCH p=(a:Loop)-[:LOOP*1..3]->(b:Loop) RETURN length(p) AS hops ORDER BY hops");
	ASSERT_EQ(paths.rowCount(), 4U);
	for (size_t row = 0U; row < 4U; ++row) EXPECT_EQ(paths.getRows()[row].at("hops").asPrimitive(), PropertyValue(row < 2U ? 1 : 2));
	const auto count = engine->execute("MATCH (a:Loop)-[:LOOP*1..3]->(b:Loop) RETURN count(*) AS count");
	ASSERT_EQ(count.rowCount(), 1U); EXPECT_EQ(count.getRows().front().at("count").asPrimitive(), PropertyValue(4));
}
