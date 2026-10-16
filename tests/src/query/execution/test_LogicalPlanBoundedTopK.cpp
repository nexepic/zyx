#include <algorithm>
#include <numeric>
#include <gtest/gtest.h>
#include "CypherParserImpl.hpp"
#include "query/execution/LogicalPlanUnwindRuntimeTestFixture.hpp"

using namespace graph;
using namespace graph::query;
using namespace graph::query::execution;
using namespace graph::query::execution::test;

TEST(LogicalPlanBoundedTopKTest, KeepsSmallResultWhenFullSortExceedsTheSameByteBudget) {
 NoGraphBackend backend; LogicalPlanReadRuntime runtime(backend);
 graph::parser::cypher::CypherParserImpl parser;
 std::vector<PropertyValue> values;
 for(int64_t i=20000;i>0;--i) values.emplace_back(i);
 QueryContext context; context.parameters.emplace("items",PropertyValue(std::move(values)));
 auto process=std::make_shared<graph::query::execution::detail::ProcessResourceBudget>(graph::query::execution::detail::ResourceLimits::uniform(32U*1024U*1024U));
 auto database=std::make_shared<graph::query::execution::detail::DatabaseResourceBudget>(process,graph::query::execution::detail::ResourceLimits::uniform(32U*1024U*1024U));
 context.resource_account=std::make_shared<graph::query::execution::detail::ResourceAccount>(database,graph::query::execution::detail::ResourceClass::RCK_QUERY,8U*1024U*1024U);
 auto limits=bounds();limits.maximum_rows=30000;limits.maximum_materialized_bytes=8U*1024U*1024U;
 auto full=parser.parseToLogical("UNWIND $items AS x RETURN x ORDER BY x");
 EXPECT_THROW(runtime.executeToQueryResult(*full.root,limits,context),std::runtime_error);
 EXPECT_EQ(context.resource_account->used(),0U);
 auto top=parser.parseToLogical("UNWIND $items AS x RETURN x ORDER BY x LIMIT 7");
 { const auto result=runtime.executeToQueryResult(*top.root,limits,context);
 ASSERT_EQ(result.rowCount(),7U);
 for(size_t i=0;i<7;++i) EXPECT_EQ(result.getRows()[i].at("x").asPrimitive(),PropertyValue(static_cast<int64_t>(i+1))); }
 EXPECT_EQ(context.resource_account->used(),0U);
}

TEST(LogicalPlanBoundedTopKTest, MatchesStableFullSortForNullsMixedTypesMultipleKeysAndOwnedPayloads) {
 NoGraphBackend backend;LogicalPlanReadRuntime runtime(backend);graph::parser::cypher::CypherParserImpl parser;
 std::vector<PropertyValue> values;
 for(int64_t i=0;i<1025;++i) {
  PropertyValue rank=i%17==0 ? PropertyValue() : (i%5==0 ? PropertyValue(std::string("s")) : PropertyValue(i%11));
  values.emplace_back(PropertyValue::MapType{{"id",i},{"rank",std::move(rank)},{"other",i%3},{"body",std::string(256,static_cast<char>('a'+i%26))}});
 }
 QueryContext context;context.parameters.emplace("items",PropertyValue(std::move(values)));
 auto limits=bounds();limits.maximum_rows=2048;limits.maximum_materialized_bytes=32U*1024U*1024U;
 for(const auto *direction:{"ASC","DESC"}) {
  const std::string query=std::string("UNWIND $items AS item RETURN item.id AS id,item.body AS body ORDER BY item.rank ")+direction+",item.other DESC";
  auto full=parser.parseToLogical(query),top=parser.parseToLogical(query+" LIMIT 17");
  const auto expected=runtime.executeToQueryResult(*full.root,limits,context);
  const auto actual=runtime.executeToQueryResult(*top.root,limits,context);
  ASSERT_EQ(actual.rowCount(),17U);
  for(size_t i=0;i<17;++i) for(const auto *column:{"id","body"})
   EXPECT_EQ(actual.getRows()[i].at(column).asPrimitive(),expected.getRows()[i].at(column).asPrimitive());
 }
}

TEST(LogicalPlanBoundedTopKTest, DoesNotPushLimitAcrossDistinctOrSkip) {
 NoGraphBackend backend;LogicalPlanReadRuntime runtime(backend);graph::parser::cypher::CypherParserImpl parser;
 QueryContext context;auto limits=bounds();
 for(const auto &[query,expected]:std::vector<std::pair<std::string,std::vector<int64_t>>>{
  {"UNWIND [3,3,2,2,1] AS x RETURN DISTINCT x ORDER BY x DESC LIMIT 2",{3,2}},
  {"UNWIND [5,4,3,2,1] AS x RETURN x ORDER BY x SKIP 2 LIMIT 3",{3,4,5}}}) {
  auto plan=parser.parseToLogical(query);const auto result=runtime.executeToQueryResult(*plan.root,limits,context);
  ASSERT_EQ(result.rowCount(),expected.size());
  for(size_t i=0;i<expected.size();++i)EXPECT_EQ(result.getRows()[i].at("x").asPrimitive(),PropertyValue(expected[i]));
 }
}

TEST(LogicalPlanBoundedTopKTest, HandlesEmptyAndSingletonInputWithoutRequiringUnavailableSortKeys) {
 NoGraphBackend backend;LogicalPlanReadRuntime runtime(backend);graph::parser::cypher::CypherParserImpl parser;
 QueryContext context;auto limits=bounds();
 auto empty=parser.parseToLogical("UNWIND [] AS x RETURN x ORDER BY x LIMIT 1");
 EXPECT_EQ(runtime.executeToQueryResult(*empty.root,limits,context).rowCount(),0U);
 auto single=parser.parseToLogical("UNWIND [1] AS x RETURN count(x) AS total ORDER BY x LIMIT 1");
 const auto result=runtime.executeToQueryResult(*single.root,limits,context);
 ASSERT_EQ(result.rowCount(),1U);EXPECT_EQ(result.getRows()[0].at("total").asPrimitive(),PropertyValue(int64_t{1}));
}

TEST(LogicalPlanBoundedTopKTest, EqualUnorderableMapKeysKeepInputOrderBeforeLaterKeys) {
 NoGraphBackend backend;LogicalPlanReadRuntime runtime(backend);graph::parser::cypher::CypherParserImpl parser;
 QueryContext context;auto limits=bounds();limits.maximum_rows=32;
 auto full=parser.parseToLogical("UNWIND [9,1,8,2,7,3] AS x RETURN x ORDER BY {key:x},x");
 auto top=parser.parseToLogical("UNWIND [9,1,8,2,7,3] AS x RETURN x ORDER BY {key:x},x LIMIT 3");
 const auto expected=runtime.executeToQueryResult(*full.root,limits,context);
 const auto actual=runtime.executeToQueryResult(*top.root,limits,context);
 ASSERT_EQ(actual.rowCount(),3U);
 for(size_t i=0;i<3;++i)EXPECT_EQ(actual.getRows()[i].at("x").asPrimitive(),expected.getRows()[i].at("x").asPrimitive());
}
