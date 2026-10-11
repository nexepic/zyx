#include "query/execution/LogicalPlanDirectEdgeRuntimeTestFixture.hpp"
#include <chrono>
#include <iostream>
using namespace graph::query::execution;
using namespace graph::query::execution::direct_edge_test;
using graph::PropertyValue;
int main(int argc,char **argv) {
 try {
  if(argc!=3)throw std::runtime_error("usage: payload evaluations");
  const size_t bytes=std::stoull(argv[1]),count=std::stoull(argv[2]);
  if(bytes>65536 || !count || count>4096)throw std::runtime_error("fixture bounds");
  DirectEdgeGraph graph=directGraph();const std::string payload(bytes,'x');graph.nodes.at(1).properties={{"payload",PropertyValue(payload)}};
  DirectEdgeSnapshot snapshot(graph);graph::query::expressions::VariableReferenceExpression expression("n","payload");
  graph::query::QueryContext context;auto bounds=directBounds();bounds.maximum_materialized_bytes=64U*1024U*1024U;
  const auto evaluate=[&](LogicalPlanReadRow &row) {return LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(snapshot,expression,row,bounds,context);};
  {LogicalPlanReadRow row;row.values.emplace("n",graph::storage::EntityRef{1,false,0,0,0});auto result=evaluate(row);
   if(std::get<std::string>(std::get<PropertyValue>(result).getVariant())!=payload)throw std::runtime_error("point result mismatch");
   row.property_cache.clear();if(std::get<std::string>(std::get<PropertyValue>(result).getVariant())!=payload)throw std::runtime_error("escaped result lifetime");}
  for(size_t iteration=0;iteration<13;++iteration) {
   std::vector<LogicalPlanReadRow> rows(count);for(auto &row:rows)row.values.emplace("n",graph::storage::EntityRef{1,false,0,0,0});
   size_t total=0;const auto start=std::chrono::steady_clock::now();
   for(auto &row:rows) {const auto result=evaluate(row);total+=std::get<std::string>(std::get<PropertyValue>(result).getVariant()).size();}
   const auto elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
   if(total!=bytes*count)throw std::runtime_error("point cardinality");
   for(const auto &row:rows)if(std::get<std::string>(row.property_cache.at("n").at("payload").getVariant())!=payload)throw std::runtime_error("cache mismatch");
   if(iteration>=3)std::cout<<"{\"milliseconds\":"<<elapsed<<",\"checked\":"<<count<<"}\n";
  }
 }catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
}
