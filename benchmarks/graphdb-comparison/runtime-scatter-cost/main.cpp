// Release execution-layer attribution through the existing storage-neutral test backend.
#include "query/execution/LogicalPlanDirectEdgeRuntimeTestFixture.hpp"
#include <chrono>
#include <iostream>
using namespace graph::query::execution;
using namespace graph::query::execution::direct_edge_test;
using graph::PropertyValue;
int main(int argc,char **argv) {
 try {
  if(argc!=5)throw std::runtime_error("usage: owners degree payload diagnostics");
  const size_t owners=std::stoull(argv[1]),degree=std::stoull(argv[2]),payload=std::stoull(argv[3]);const bool diagnostics=std::stoi(argv[4])!=0;
  if(!owners || owners>128 || !degree || degree>32 || payload>8192)throw std::runtime_error("fixture limits");
  const std::string value(payload,'x');
  for(size_t iteration=0;iteration<(diagnostics?1U:13U);++iteration) {
   DirectEdgeGraph graph;
   for(uint64_t owner=1;owner<=owners;++owner)graph.nodes.emplace(owner,graph::storage::LogicalGraphNode{owner,{"Person"},{{"payload",PropertyValue(value)}}});
   uint64_t edge=1;for(uint64_t owner=1;owner<=owners;++owner)for(size_t repeat=0;repeat<degree;++repeat)
    graph.edges.emplace_back(edge++,owner,owner==owners?1:owner+1,"KNOWS",std::vector<graph::storage::LogicalGraphPropertyEntry>{});
   DirectEdgeBackend backend(graph);LogicalPlanReadRuntime runtime(backend);
   auto traversal=std::make_unique<graph::query::logical::LogicalTraversal>(std::make_unique<graph::query::logical::LogicalNodeScan>("a"),"a","r","b","KNOWS","out");
   std::vector<graph::query::logical::LogicalProjectItem> items;
   items.emplace_back(std::make_shared<graph::query::expressions::VariableReferenceExpression>("b","payload"),"value");
   graph::query::logical::LogicalProject project(std::move(traversal),std::move(items));
   auto bounds=directBounds();bounds.maximum_node_records=owners;bounds.maximum_rows=owners*degree+1;
   bounds.maximum_edge_records=owners*degree+1;bounds.maximum_edges_per_node=degree+1;bounds.maximum_materialized_bytes=64U*1024U*1024U;
   graph::debug::PerfTrace::reset();graph::debug::PerfTrace::setEnabled(diagnostics);
   if(!diagnostics) {
    size_t checked=0;
    static_cast<void>(runtime.execute(project,bounds,[&](const LogicalPlanReadRow &row){
     if(std::get<std::string>(std::get<PropertyValue>(row.values.at("value")).getVariant())!=value)throw std::runtime_error("incorrect projected payload");++checked;return true;
    }));
    if(checked!=owners*degree)throw std::runtime_error("validation cardinality");
   }
   size_t rows=0,bytes=0;const auto start=std::chrono::steady_clock::now();
   const size_t repetitions=diagnostics?1U:16U;
   for(size_t repeat=0;repeat<repetitions;++repeat)static_cast<void>(runtime.execute(project,bounds,[&](const LogicalPlanReadRow &row){
    const auto &output=std::get<PropertyValue>(row.values.at("value"));
    const auto &text=std::get<std::string>(output.getVariant());
    if(diagnostics && text!=value)throw std::runtime_error("incorrect projected payload");++rows;bytes+=text.size();return true;
   }));
   const auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
   if(rows!=owners*degree*repetitions || bytes!=rows*payload)throw std::runtime_error("incorrect bag cardinality");
   if(diagnostics) {
    const auto trace=graph::debug::PerfTrace::snapshotAndReset();
    std::cout<<"{\"event\":\"diagnostic\",\"rows\":"<<rows;
    for(const char *name:{"borrowed_values","copied_values","moved_values","copied_retained_bytes","moved_retained_bytes"}) {
     auto it=trace.find(std::string("logical_plan.prefetch.")+name);
     std::cout<<",\""<<name<<"\":"<<(it==trace.end()?0:it->second.totalValue);
    }
    std::cout<<"}\n";
   } else if(iteration>=3)std::cout<<"{\"event\":\"timing\",\"milliseconds\":"<<ms<<",\"checked_rows\":"<<rows/repetitions<<"}\n";
  }
 }catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
}
