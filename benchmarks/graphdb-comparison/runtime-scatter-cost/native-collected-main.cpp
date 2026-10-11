// Native-storage execution probe; fixture construction and validation are untimed.
#include "graph/storage/relocatable/backend/RelocatableStorageBackend.hpp"
#include "graph/query/execution/LogicalPlanReadRuntime.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/logical/operators/LogicalTraversal.hpp"
#include "graph/query/logical/operators/LogicalProject.hpp"
#include "graph/query/logical/operators/LogicalAggregate.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/debug/PerfTrace.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <numeric>
#if defined(__APPLE__) || defined(__linux__)
#include <sys/resource.h>
#endif
using namespace graph;
using namespace graph::storage;
using namespace graph::query;
using namespace graph::query::execution;
int main(int argc, char **argv) {
 try {
  if(argc!=7) throw std::invalid_argument("usage: private-db owners degree payload fields project|project2|aggregate");
  const std::filesystem::path path(argv[1]);
  const size_t owners=std::stoull(argv[2]), degree=std::stoull(argv[3]), payload=std::stoull(argv[4]), fields=std::stoull(argv[5]);
  const bool twoHop=std::string_view(argv[6])=="project2";
  const bool aggregate=std::string_view(argv[6])=="aggregate";
  if(!aggregate && !twoHop && std::string_view(argv[6])!="project") throw std::invalid_argument("mode");
  if(!owners || owners>1024 || !degree || degree>32 || !fields || fields>4 || payload>8192 || std::filesystem::exists(path)) throw std::invalid_argument("bounded dimensions and new private file required");
  struct Cleanup {std::filesystem::path path;~Cleanup(){std::error_code e;std::filesystem::remove(path,e);}} cleanup{path};
  RelocatableStorageBackendOptions options;options.data_path=path;options.open_mode=OpenMode::OPEN_CREATE_NEW_FILE;
  auto backend=RelocatableStorageBackend::initializeNew(options);backend->open();
  std::vector<std::string> expected(owners);
  std::vector<LogicalGraphNodeCreateRequest> requests(owners);
  for(size_t i=0;i<owners;++i) {
   uint64_t random=i+1;std::string text(payload,' ');
   for(char &c:text){random^=random<<13;random^=random>>7;random^=random<<17;c=char('!'+random%90);}
   expected[i]=text;requests[i].properties.push_back({"rank",PropertyValue(int64_t(i))});
   for(size_t field=0;field<fields;++field)requests[i].properties.push_back({"p"+std::to_string(field),PropertyValue(text)});
  }
  std::vector<uint64_t> ids;
  // Bound ordinary publication payload independently of benchmark read dimensions.
  const size_t batchSize=std::max<size_t>(1,std::min<size_t>(64,262144/(fields*(payload+64)+128)));
  for(size_t i=0;i<owners;i+=batchSize) {
   std::vector<LogicalGraphNodeCreateRequest> batch;
   for(size_t j=i;j<std::min(owners,i+batchSize);++j)batch.push_back(std::move(requests[j]));
   auto created=backend->createLogicalNodes(batch);ids.insert(ids.end(),created.begin(),created.end());
  }
  if(ids.size()!=owners)throw std::runtime_error("node cardinality");
  std::vector<LogicalGraphEdgeCreateRequest> edges;
  for(size_t i=0;i<owners;++i)for(size_t j=0;j<degree;++j)edges.push_back({ids[i],ids[(i+1)%owners],"KNOWS",{}});
  for(size_t i=0;i<edges.size();i+=128) {
   std::vector<LogicalGraphEdgeCreateRequest> batch(edges.begin()+i,edges.begin()+std::min(edges.size(),i+128));
   if(backend->createLogicalEdges(batch).size()!=batch.size())throw std::runtime_error("edge cardinality");
  }
  requests.clear();edges.clear();
  const size_t multiplicity=twoHop?degree*degree:degree;
  std::unique_ptr<logical::LogicalOperator> traversal=std::make_unique<logical::LogicalTraversal>(std::make_unique<logical::LogicalNodeScan>("a"),"a","r","b","KNOWS","out");
  if(twoHop)traversal=std::make_unique<logical::LogicalTraversal>(std::move(traversal),"b","s","c","KNOWS","out");
  const std::string target=twoHop?"c":"b";
  std::unique_ptr<logical::LogicalOperator> plan;
  if(aggregate) {
   auto rank=std::make_shared<expressions::VariableReferenceExpression>("b","rank");
   plan=std::make_unique<logical::LogicalAggregate>(std::move(traversal),std::vector<std::shared_ptr<expressions::Expression>>{},std::vector<logical::LogicalAggItem>{{"sum",rank,"total",false},{"count",nullptr,"rows",false}});
  } else {
   std::vector<logical::LogicalProjectItem> items;
   for(size_t field=0;field<fields;++field)items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>(target,"p"+std::to_string(field)),"p"+std::to_string(field));
   plan=std::make_unique<logical::LogicalProject>(std::move(traversal),std::move(items));
  }
  LogicalPlanReadRuntimeBounds bounds{.maximum_node_records=owners+1,.maximum_node_page_records=std::min<size_t>(256,owners),.maximum_metadata_pages_per_node_page=4096,.maximum_edges_per_node=degree+1,.maximum_edge_records=owners*degree+1,.maximum_adjacency_page_records=degree+1,.maximum_property_entries=16,.maximum_rows=owners*multiplicity+1,.maximum_distinct_rows=owners*multiplicity+1};
  LogicalPlanReadRuntime runtime(*backend);
  const auto execute=[&](bool validate) {
   size_t rows=0,bytes=0;std::vector<size_t> counts(owners);
   const auto result=runtime.executeToQueryResult(*plan,bounds);
   for(const auto &row:result.getRows()) {
    ++rows;
    if(aggregate) {
     const auto integer=[&](const char *key){return std::get<int64_t>(row.at(key).asPrimitive().getVariant());};
     if(integer("total")!=int64_t(degree*owners*(owners-1)/2) || integer("rows")!=int64_t(owners*degree))throw std::runtime_error("aggregate mismatch");
    } else {
     const auto &first=std::get<std::string>(row.at("p0").asPrimitive().getVariant());
     if(validate){auto it=std::find(expected.begin(),expected.end(),first);if(it==expected.end())throw std::runtime_error("unknown payload");++counts[size_t(it-expected.begin())];}
     for(size_t field=0;field<fields;++field) {
      const auto &value=std::get<std::string>(row.at("p"+std::to_string(field)).asPrimitive().getVariant());
      if(validate && value!=first)throw std::runtime_error("column mismatch");bytes+=value.size();
     }
    }
   }
   if(rows!=(aggregate?1:owners*multiplicity) || (!aggregate && bytes!=owners*multiplicity*fields*payload))throw std::runtime_error("result cardinality");
   if(validate && !aggregate && std::ranges::any_of(counts,[&](size_t n){return n!=multiplicity;}))throw std::runtime_error("bag mismatch");
  };
  debug::PerfTrace::setEnabled(false);debug::PerfTrace::setValueCollectionEnabled(false);
  execute(true);
  for(size_t iteration=0;iteration<13;++iteration) {
   const auto start=std::chrono::steady_clock::now();
   for(size_t repeat=0;repeat<4;++repeat)execute(false);
   const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()/4;
   if(iteration>=3)std::cout<<"{\"event\":\"timing\",\"milliseconds\":"<<ms<<"}\n";
  }
  debug::PerfTrace::reset();debug::PerfTrace::setValueCollectionEnabled(true);execute(true);
  const auto trace=debug::PerfTrace::snapshotAndReset();
  std::cout<<"{\"event\":\"diagnostic\",\"file_bytes\":"<<std::filesystem::file_size(path);
  for(const auto &[name,stats]:trace)std::cout<<",\""<<name<<"\":"<<stats.totalValue;
  size_t rss=0;
#if defined(__APPLE__) || defined(__linux__)
  struct rusage usage{};
  if(getrusage(RUSAGE_SELF,&usage)==0)rss=size_t(usage.ru_maxrss);
#if defined(__linux__)
  rss*=1024;
#endif
#endif
  std::cout<<",\"process_peak_rss_bytes\":"<<rss<<"}\n";
 }catch(const std::exception &error){std::cerr<<error.what()<<'\n';return 1;}
}
