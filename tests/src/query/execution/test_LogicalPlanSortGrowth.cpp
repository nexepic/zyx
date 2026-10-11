/**
 * @file test_LogicalPlanSortGrowth.cpp
 * @brief Sorting owned map payloads across buffered row-capacity growth.
 */
#include <algorithm>
#include <numeric>
#include <string>
#include <vector>
#include <gtest/gtest.h>
#include "CypherParserImpl.hpp"
#include "query/execution/LogicalPlanUnwindRuntimeTestFixture.hpp"

using namespace graph;
using namespace graph::query;
using namespace graph::query::execution;
using namespace graph::query::execution::test;

TEST(LogicalPlanSortGrowthTest, PreservesStableNullOrderingAndOwnedPayloadsAcrossCapacityGrowth) {
 NoGraphBackend backend;
 LogicalPlanReadRuntime runtime(backend);
 graph::parser::cypher::CypherParserImpl parser;
 constexpr size_t count=1025U;
 std::vector<PropertyValue> items;
 std::vector<int64_t> ranks;
 std::vector<std::string> bodies;
 for(size_t position=0;position<count;++position) {
  const auto id=count-1U-position;
  const int64_t rank=id%19U==0U ? -1 : static_cast<int64_t>(id%11U);
  const std::string body=std::to_string(id)+"-"+std::string(1024U,static_cast<char>('a'+id%26U));
  ranks.push_back(rank);bodies.push_back(body);
  items.emplace_back(PropertyValue::MapType{{"rank",rank<0 ? PropertyValue() : PropertyValue(rank)},
                                           {"body",PropertyValue(body)}});
 }
 QueryContext context;
 context.parameters.emplace("items",PropertyValue(std::move(items)));
 auto limits=bounds();
 limits.maximum_rows=4096U;
 limits.maximum_node_records=1'000'000U;
 limits.maximum_materialized_bytes=64U*1024U*1024U;
 for(const bool ascending:{true,false}) {
  SCOPED_TRACE(ascending);
  auto plan=parser.parseToLogical(std::string("UNWIND $items AS item RETURN item.rank AS rank, item.body AS body ORDER BY rank ")+(ascending?"ASC":"DESC"));
  const auto result=runtime.executeToQueryResult(*plan.root,limits,context);
  std::vector<size_t> expected(count);
  std::iota(expected.begin(),expected.end(),0U);
  std::stable_sort(expected.begin(),expected.end(),[&](size_t left,size_t right) {
   if(ranks[left]<0 || ranks[right]<0) {
    if(ranks[left]<0 && ranks[right]<0)return false;
    return ascending ? ranks[right]<0 : ranks[left]<0;
   }
   return ascending ? ranks[left]<ranks[right] : ranks[left]>ranks[right];
  });
  ASSERT_EQ(result.rowCount(),count);
  for(size_t index=0;index<count;++index) {
   const auto source=expected[index];
   ASSERT_EQ(result.getRows()[index].at("rank").asPrimitive(),ranks[source]<0 ? PropertyValue() : PropertyValue(ranks[source]));
   ASSERT_EQ(result.getRows()[index].at("body").asPrimitive(),PropertyValue(bodies[source]));
  }
 }
}
