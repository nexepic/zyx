#include "graph/storage/relocatable/backend/RelocatableStorageBackend.hpp"
#include "graph/debug/PerfTrace.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
using namespace graph;
using namespace graph::storage;
using Clock = std::chrono::steady_clock;

// Measure existing authoritative joint adjacency/primary reads. No duplicated
// covering format or skipped primary validation is introduced by this probe.
int main(int argc, char **argv) {
 if (argc != 5) throw std::invalid_argument("usage: topology-cost PRIVATE_DB chain|star|skew NODES EDGES");
 const std::filesystem::path path(argv[1]); const std::string shape(argv[2]);
 const size_t count = std::stoull(argv[3]), edgeCount = std::stoull(argv[4]);
 if (std::filesystem::exists(path) || count < 2 || count > 10000 || edgeCount > 100000 ||
     (shape != "chain" && shape != "star" && shape != "skew")) throw std::invalid_argument("bounded new fixture required");
 struct Cleanup { std::filesystem::path p; ~Cleanup() { std::error_code e; std::filesystem::remove(p,e); } } cleanup{path};
 RelocatableStorageBackendOptions options; options.data_path=path; options.open_mode=OpenMode::OPEN_CREATE_NEW_FILE;
 auto backend=RelocatableStorageBackend::initializeNew(options); backend->open();
 std::vector<uint64_t> nodes;
 for (size_t offset=0; offset<count; offset+=128) {
  const auto ids=backend->createLogicalNodes(std::vector<LogicalGraphNodeCreateRequest>(std::min<size_t>(128,count-offset)));
  nodes.insert(nodes.end(),ids.begin(),ids.end());
 }
 if (nodes.size()!=count) throw std::runtime_error("node creation mismatch");
 std::vector<std::vector<LogicalGraphEdgeTopology>> expected(count);
 for (size_t offset=0; offset<edgeCount; offset+=128) {
  std::vector<LogicalGraphEdgeCreateRequest> requests; std::vector<size_t> sources;
  for (size_t i=offset; i<std::min(edgeCount,offset+128); ++i) {
   const size_t source=shape=="star" ? 0 : shape=="chain" ? i%(count-1) : i%2==0 ? 0 : (i*97U)%count;
   const size_t target=shape=="chain" ? source+1 : (i*137U+1U)%count;
   requests.push_back({nodes[source],nodes[target],i%3==0 ? "RouteA" : "RouteB",{}}); sources.push_back(source);
  }
  const auto ids=backend->createLogicalEdges(requests);
  if(ids.size()!=requests.size()) throw std::runtime_error("edge creation mismatch");
  for(size_t i=0;i<ids.size();++i) expected[sources[i]].push_back({ids[i],requests[i].source_node_id,requests[i].target_node_id,requests[i].type});
 }
 const auto fileBytes=std::filesystem::file_size(path);
 for(int round=-3;round<=10;++round) {
  auto snapshot=backend->openLogicalGraphReadSnapshot();
  for(const char *pass:{"first","repeat"}) {
   const bool diagnostic=round==10;
   if(diagnostic) { debug::PerfTrace::reset(); debug::PerfTrace::setValueCollectionEnabled(true); }
   size_t checked=0;
   const auto start=Clock::now();
   for(size_t node=0;node<count;++node) {
    std::optional<LogicalGraphAdjacencyContinuation> continuation; size_t ordinal=0, pages=0;
    do {
     if (++pages > expected[node].size()/64U+2U) throw std::runtime_error("non-progressing topology continuation");
     const auto page=snapshot->scanAdjacentEdgeTopologies(nodes[node],LogicalGraphReadDirection::LGRD_OUTGOING,continuation,64);
     for(const auto &edge:page.edges) {
      if(ordinal>=expected[node].size() || edge!=expected[node][ordinal++]) throw std::runtime_error("complete topology mismatch");
      ++checked;
     }
     continuation=page.continuation;
    } while(continuation);
    if(ordinal!=expected[node].size()) throw std::runtime_error("missing topology");
   }
   const double ms=std::chrono::duration<double,std::milli>(Clock::now()-start).count();
   if(checked!=edgeCount) throw std::runtime_error("total edge mismatch");
   if(diagnostic) {
    const auto trace=debug::PerfTrace::snapshotAndReset(); debug::PerfTrace::setValueCollectionEnabled(false);
    for(const auto &[key,value]:trace) std::cout<<"{\"event\":\"counter\",\"pass\":\""<<pass<<"\",\"key\":\""<<key<<"\",\"value\":"<<value.totalValue<<"}\n";
   } else if(round>=0) std::cout<<"{\"event\":\"sample\",\"pass\":\""<<pass<<"\",\"round\":"<<round<<",\"milliseconds\":"<<ms<<",\"checked\":"<<checked<<"}\n";
  }
 }
 if(std::filesystem::file_size(path)!=fileBytes) throw std::runtime_error("read changed file size");
 std::cout<<"{\"event\":\"fixture\",\"shape\":\""<<shape<<"\",\"nodes\":"<<count<<",\"edges\":"<<edgeCount<<",\"file_bytes\":"<<fileBytes<<"}\n";
 backend->close();
}
