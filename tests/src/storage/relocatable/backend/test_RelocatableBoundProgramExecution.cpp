#include "storage/relocatable/backend/RelocatableStorageBackendTestFixture.hpp"
#include "graph/query/api/QueryEngine.hpp"
#include "graph/debug/PerfTrace.hpp"
using namespace relocatable_storage_backend_test;

namespace {
struct TraceValues {
	bool previous = graph::debug::PerfTrace::areValuesEnabled();
	TraceValues() { graph::debug::PerfTrace::reset(); graph::debug::PerfTrace::setValueCollectionEnabled(true); }
	~TraceValues() { graph::debug::PerfTrace::setValueCollectionEnabled(previous); graph::debug::PerfTrace::reset(); }
};
}

TEST_F(RelocatableStorageBackendTest, BoundNativeAggregatePublishesValidatedEnvelopeAndKeepsSelectionSemantics) {
	materializeNativeDatabase(); graph::Database database(data_path_.string()); database.open();
	auto engine = database.getQueryEngine();
	engine->execute("CREATE (:Binding {id:1,rank:5}),(:Binding {id:2,rank:8}),(:Binding {id:3,rank:NULL})");
	TraceValues trace;
	const auto result = engine->execute("MATCH (n:Binding) RETURN sum(n.rank) AS total,avg(n.rank) AS mean,min(n.rank) AS low,count(n.rank) AS count");
	ASSERT_EQ(result.rowCount(), 1U); const auto &row = result.getRows().front();
	EXPECT_EQ(row.at("total").toString(), "13"); EXPECT_DOUBLE_EQ(std::get<double>(std::get<PropertyValue>(row.at("mean").getVariant()).getVariant()), 6.5);
	EXPECT_EQ(row.at("low").toString(), "5"); EXPECT_EQ(row.at("count").toString(), "2");
	const auto values = graph::debug::PerfTrace::snapshotAndReset();
	ASSERT_TRUE(values.contains("logical_plan.batch_envelope.published"));
	EXPECT_GT(values.at("logical_plan.batch_envelope.published").totalValue, 0);
	const auto filtered = engine->execute("MATCH (n:Binding) WHERE n.id = 2 RETURN sum(n.rank) AS total");
	ASSERT_EQ(filtered.rowCount(), 1U); EXPECT_EQ(filtered.getRows().front().at("total").toString(), "8");
}

TEST_F(RelocatableStorageBackendTest, BoundNativeInputsPreserveProjectedPropertiesLocalScopesAndLazyBranches) {
	materializeNativeDatabase(); graph::Database database(data_path_.string()); database.open();
	auto engine = database.getQueryEngine(); engine->execute("CREATE (:Binding {id:1,rank:5})");
	const auto projection = engine->execute("MATCH (n:Binding) WITH n.rank RETURN n.rank AS value");
	ASSERT_EQ(projection.rowCount(), 1U); EXPECT_EQ(projection.getRows().front().at("value").toString(), "5");
	const auto shadow = engine->execute("MATCH (n:Binding) WITH n,7 AS x RETURN [x IN [1,2] | x+n.rank] AS values");
	ASSERT_EQ(shadow.rowCount(), 1U); const auto &list = shadow.getRows().front().at("values").asPrimitive().getList();
	ASSERT_EQ(list.size(), 2U); EXPECT_EQ(list[0].toString(), "6"); EXPECT_EQ(list[1].toString(), "7");
	const auto correlated = engine->execute("MATCH (n:Binding) CALL { WITH n RETURN n.rank+1 AS inner } RETURN n.rank AS outer,inner");
	ASSERT_EQ(correlated.rowCount(), 1U); EXPECT_EQ(correlated.getRows().front().at("outer").toString(), "5");
	EXPECT_EQ(correlated.getRows().front().at("inner").toString(), "6");
	const auto lazy = engine->execute("RETURN CASE WHEN false THEN $absent ELSE 9 END AS value,false AND $absent AS conjunction,true OR $absent AS disjunction");
	ASSERT_EQ(lazy.rowCount(), 1U); EXPECT_EQ(lazy.getRows().front().at("value").toString(), "9");
	EXPECT_FALSE(std::get<bool>(lazy.getRows().front().at("conjunction").asPrimitive().getVariant()));
	EXPECT_TRUE(std::get<bool>(lazy.getRows().front().at("disjunction").asPrimitive().getVariant()));
}

TEST_F(RelocatableStorageBackendTest, LogicalCacheInvalidatesAcrossOtherEngineSchemaPublicationAndReopen) {
	materializeNativeDatabase(); graph::Database database(data_path_.string()); database.open();
	auto first = database.getQueryEngine(); auto second = std::make_shared<graph::query::QueryEngine>(std::dynamic_pointer_cast<graph::storage::LogicalGraphReadBackend>(database.getStorageBackend()));
	first->buildPlan("MATCH (n:Binding) RETURN n.rank"); first->buildPlan("RETURN 2");
	ASSERT_EQ(first->getPlanCache().size(), 2U);
	second->execute("CREATE INDEX binding_rank FOR (n:Binding) ON (n.rank)");
	first->buildPlan("MATCH (n:Binding) RETURN n.rank"); EXPECT_EQ(first->getPlanCache().size(), 1U);
	second->execute("DROP INDEX binding_rank");
	first->buildPlan("MATCH (n:Binding) RETURN n.rank"); EXPECT_EQ(first->getPlanCache().size(), 1U);
	first->resetEphemeralState(); EXPECT_EQ(first->getPlanCache().size(), 0U);
	database.close(); database.open();
	EXPECT_EQ(database.getQueryEngine()->execute("RETURN 3 AS value").getRows().front().at("value").toString(), "3");
}

TEST_F(RelocatableStorageBackendTest, CachedLogicalPlansRebindParameterTypesAndReturnIndependentOwnedValues) {
	materializeNativeDatabase(); graph::Database database(data_path_.string()); database.open();
	auto engine = database.getQueryEngine(); graph::query::QueryContext context;
	context.parameters.emplace("value", PropertyValue(int64_t(3)));
	const auto integer = engine->execute("RETURN $value AS value", context);
	EXPECT_EQ(integer.getRows().front().at("value").toString(), "3");
	context.parameters["value"] = PropertyValue(std::string(8192U, 'a'));
	const auto text = engine->execute("RETURN $value AS value", context);
	EXPECT_GT(engine->getPlanCache().hits(), 0U);
	context.parameters["value"] = PropertyValue("replacement"); database.close();
	EXPECT_EQ(std::get<std::string>(text.getRows().front().at("value").asPrimitive().getVariant()), std::string(8192U, 'a'));
}
