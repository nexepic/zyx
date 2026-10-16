// S0.4 mechanism attribution only; never substitutes for end-to-end acceptance.
#include "query/execution/PrimitiveColumnFilter.hpp"
#include "graph/storage/relocatable/backend/RelocatableStorageBackend.hpp"
#include "graph/query/execution/LogicalPlanReadRuntime.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/logical/operators/LogicalProject.hpp"
#include "graph/query/logical/operators/LogicalSort.hpp"
#include "graph/query/logical/operators/LogicalLimit.hpp"
#include "graph/query/expressions/Expression.hpp"
#include <array>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <numeric>
using namespace graph;
using namespace graph::storage;
using namespace graph::query;
using namespace graph::query::execution;
namespace mechanism=graph::query::execution::detail;
int main(int argc, char **argv) {
 try {
  if (argc != 2 || std::filesystem::exists(argv[1])) throw std::invalid_argument("new private database required");
  const std::filesystem::path path(argv[1]);
  struct Cleanup {std::filesystem::path path; ~Cleanup(){std::error_code e;std::filesystem::remove(path,e);}} cleanup{path};
  RelocatableStorageBackendOptions options;options.data_path=path;options.open_mode=OpenMode::OPEN_CREATE_NEW_FILE;
  auto backend=RelocatableStorageBackend::initializeNew(options);backend->open();
  constexpr size_t count=4096, keep=128;
  std::vector<uint64_t> ids;
  for(size_t begin=0;begin<count;begin+=64) {
   std::vector<LogicalGraphNodeCreateRequest> requests;
   for(size_t i=begin;i<begin+64;++i) {
    LogicalGraphNodeCreateRequest request;request.labels={"Probe"};
    request.properties={{"rank",PropertyValue(int64_t((i*7919)%count))}};
    requests.push_back(std::move(request));
   }
   auto created=backend->createLogicalNodes(requests);ids.insert(ids.end(),created.begin(),created.end());
  }
  auto snapshot=backend->openLogicalGraphReadSnapshot();
  auto source=snapshot->readNodePropertiesSorted(ids,"rank");
  const auto verifySource=[&](const auto &values) {
   if(values.size()!=count)throw std::runtime_error("storage cardinality");
   for(size_t i=0;i<count;++i)if(values[i]!=PropertyValue(int64_t((i*7919)%count)))throw std::runtime_error("storage value");
  };
  verifySource(source);
  std::vector<size_t> expected(count);std::iota(expected.begin(),expected.end(),0U);
  std::ranges::sort(expected,[&](size_t a,size_t b){return (a*7919)%count<(b*7919)%count;});
  const PropertyValue lower(int64_t{1000}),upper(int64_t{3000});
  const std::array<mechanism::ColumnPredicate,2> conditions{{{0,mechanism::ColumnPredicateOp::CPO_GREATER_EQUAL,&lower},{0,mechanism::ColumnPredicateOp::CPO_LESS,&upper}}};
  LogicalPlanReadRuntimeBounds bounds;bounds.maximum_node_page_records=256;bounds.maximum_metadata_pages_per_node_page=256;bounds.maximum_property_entries=64;bounds.maximum_node_records=count+1;bounds.maximum_rows=count+1;bounds.maximum_materialized_bytes=64U*1024U*1024U;
  LogicalPlanReadRuntime runtime(*backend);
  const auto plan=[&](bool topk) -> std::unique_ptr<logical::LogicalOperator> {
   auto rank=std::make_shared<expressions::VariableReferenceExpression>("n","rank");
   auto project=std::make_unique<logical::LogicalProject>(std::make_unique<logical::LogicalNodeScan>("n",std::vector<std::string>{"Probe"}),std::vector<logical::LogicalProjectItem>{{rank,"rank"}});
   std::unique_ptr<logical::LogicalOperator> root=std::make_unique<logical::LogicalSort>(std::move(project),std::vector<logical::LogicalSortItem>{{std::make_shared<expressions::VariableReferenceExpression>("rank"),true}});
   if(topk)root=std::make_unique<logical::LogicalLimit>(std::move(root),keep);
   return root;
  };
  const auto full=plan(false),top=plan(true);
  const auto emit=[](const char *name,int sample,double elapsed,size_t rows){std::cout<<"{\"mechanism\":\""<<name<<"\",\"sample\":"<<sample<<",\"milliseconds\":"<<elapsed<<",\"validated_rows\":"<<rows<<"}\n";};
  for(int iteration=-3;iteration<10;++iteration) {
   for(size_t step=0;step<5;++step) {
    const size_t variant=(step+iteration+3)%5;
    const auto start=std::chrono::steady_clock::now();size_t rows=0;
    if(variant==0) {
     auto values=snapshot->readNodePropertiesSorted(ids,"rank");verifySource(values);rows=values.size();
    } else if(variant==1 || variant==2) {
     SelectionVector selection(count);MaterializedByteBudget budget(64U*1024U*1024U);
     if(variant==1) {
      if(!mechanism::tryFilterPrimitiveColumn(source,conditions,selection,budget,[]{}))throw std::runtime_error("typed path declined");
     } else {
      for(size_t i=0;i<count;++i)for(const auto &predicate:conditions)if(!mechanism::columnPredicateMatches(source[i],predicate)){selection.reject(i);break;}
     }
     for(size_t i=0;i<count;++i){const auto rank=(i*7919)%count;if(selection.includes(i)!=(rank>=1000 && rank<3000))throw std::runtime_error("filter selection");}
     rows=selection.selectedCount();
    } else {
     auto result=runtime.executeToQueryResult(variant==3?*full:*top,bounds);
     const size_t wanted=variant==3?count:keep;
     if(result.rowCount()!=wanted)throw std::runtime_error("sort cardinality");
     for(size_t i=0;i<wanted;++i)if(result.getRows()[i].at("rank").asPrimitive()!=PropertyValue(int64_t((expected[i]*7919)%count)))throw std::runtime_error("sort value/order");
     rows=wanted;
    }
    const auto elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    if(iteration>=0)emit(std::array{"native_selected_storage","typed_filter_including_conversion","generic_filter_same_source","native_full_sort_all_output","native_topk_128_output"}[variant],iteration,elapsed,rows);
   }
  }
  snapshot.reset();backend->close();
 } catch(const std::exception &error){std::cerr<<error.what()<<'\n';return 1;}
}
