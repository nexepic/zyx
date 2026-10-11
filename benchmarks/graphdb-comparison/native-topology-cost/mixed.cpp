#include "graph/storage/relocatable/backend/RelocatableStorageBackend.hpp"
#include "graph/debug/PerfTrace.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
using namespace graph;
using namespace graph::storage;
using Clock = std::chrono::steady_clock;
int main(int argc, char **argv) {
 if(argc != 5) throw std::invalid_argument("mixed-topology PRIVATE_DB chain|star|skew NODES EDGES");
 const std::filesystem::path path(argv[1]);const std::string shape(argv[2]);
 const size_t count=std::stoull(argv[3]), edgeCount=std::stoull(argv[4]);
 if(std::filesystem::exists(path)||count<2||count>10000||edgeCount<28||edgeCount>100000||(shape!="chain"&&shape!="star"&&shape!="skew")) throw std::invalid_argument("bounded new fixture required");
 struct Cleanup {std::filesystem::path path;~Cleanup(){std::error_code error;std::filesystem::remove(path,error);}} cleanup{path};
 RelocatableStorageBackendOptions options;options.data_path=path;options.open_mode=OpenMode::OPEN_CREATE_NEW_FILE;
 auto backend=RelocatableStorageBackend::initializeNew(options);backend->open();
 std::vector<uint64_t> nodes,originalEdges;
 for(size_t offset=0;offset<count;offset+=128) {
  const auto ids=backend->createLogicalNodes(std::vector<LogicalGraphNodeCreateRequest>(std::min<size_t>(128,count-offset)));
  nodes.insert(nodes.end(),ids.begin(),ids.end());
 }
 if(nodes.size()!=count)throw std::runtime_error("node fixture mismatch");
 std::vector<std::vector<LogicalGraphEdgeTopology>> expected(count);
 for(size_t offset=0;offset<edgeCount;offset+=128) {
  std::vector<LogicalGraphEdgeCreateRequest> requests;std::vector<size_t> sources;
  for(size_t i=offset;i<std::min(edgeCount,offset+128);++i) {
   const size_t source=shape=="star"?0:shape=="chain"?i%(count-1):i%2==0?0:(i*97U)%count;
   const size_t target=shape=="chain"?source+1:(i*137U+1U)%count;
   requests.push_back({nodes[source],nodes[target],i%3==0?"MixedRouteA":"MixedRouteB",{}});sources.push_back(source);
  }
  const auto ids=backend->createLogicalEdges(requests);if(ids.size()!=requests.size())throw std::runtime_error("edge fixture mismatch");
  originalEdges.insert(originalEdges.end(),ids.begin(),ids.end());
  for(size_t i=0;i<ids.size();++i)expected[sources[i]].push_back({ids[i],requests[i].source_node_id,requests[i].target_node_id,requests[i].type});
 }
 const auto readNode=[&](auto &snapshot,size_t node,const auto &truth) {
  std::vector<LogicalGraphEdgeTopology> result;std::optional<LogicalGraphAdjacencyContinuation> continuation;size_t pages=0;
  do {
   if(++pages>truth[node].size()/64U+2U)throw std::runtime_error("continuation did not progress");
   const auto page=snapshot->scanAdjacentEdgeTopologies(nodes[node],LogicalGraphReadDirection::LGRD_OUTGOING,continuation,64);
   result.insert(result.end(),page.edges.begin(),page.edges.end());continuation=page.continuation;
  }while(continuation);
  if(result!=truth[node])throw std::runtime_error("complete topology mismatch");
  return result;
 };
 const auto readAll=[&](auto &snapshot,const auto &truth){size_t checked=0;for(size_t n=0;n<count;++n)checked+=readNode(snapshot,n,truth).size();return checked;};
 const auto readTwoHop=[&](auto &snapshot,const auto &truth) {
  size_t checked=0,required=0;
  for(const auto &first:truth[0]) {
   const auto found=std::ranges::find(nodes,first.target_node_id);if(found==nodes.end())throw std::runtime_error("foreign target");
   const size_t target=static_cast<size_t>(found-nodes.begin());
   for(const auto &second:truth[target])if(second.edge_id!=first.edge_id)++required;
  }
  for(const auto &first:readNode(snapshot,0,truth)) {
   const size_t target=static_cast<size_t>(std::ranges::find(nodes,first.target_node_id)-nodes.begin());
   for(const auto &second:readNode(snapshot,target,truth))if(second.edge_id!=first.edge_id)++checked;
  }
  if(checked!=required)throw std::runtime_error("two-hop bag mismatch");return checked;
 };
 const auto counters=[](const char *stage,const char *pass) {
  const auto trace=debug::PerfTrace::snapshotAndReset();debug::PerfTrace::setValueCollectionEnabled(false);
  for(const auto &[key,value]:trace)std::cout<<"{\"event\":\"counter\",\"stage\":\""<<stage<<"\",\"pass\":\""<<pass<<"\",\"key\":\""<<key<<"\",\"value\":"<<value.totalValue<<"}\n";
 };
 const auto timeRead=[&](auto &snapshot,const auto &truth,const char *stage,const char *pass,int round) {
  const bool diagnostic=round==10;
  if(diagnostic){debug::PerfTrace::reset();debug::PerfTrace::setValueCollectionEnabled(true);}
  const auto start=Clock::now();const size_t checked=std::string_view(pass)=="two_hop"?readTwoHop(snapshot,truth):readAll(snapshot,truth);
  const double ms=std::chrono::duration<double,std::milli>(Clock::now()-start).count();
  if(diagnostic)counters(stage,pass);
  else if(round>=0)std::cout<<"{\"event\":\"read_sample\",\"stage\":\""<<stage<<"\",\"pass\":\""<<pass<<"\",\"round\":"<<round<<",\"checked\":"<<checked<<",\"milliseconds\":"<<ms<<"}\n";
 };
 const auto initialFile=std::filesystem::file_size(path);
 for(int round=-3;round<=10;++round){auto snapshot=backend->openLogicalGraphReadSnapshot();for(const char *pass:{"first","repeat","two_hop"})timeRead(snapshot,expected,"initial",pass,round);}
 if(std::filesystem::file_size(path)!=initialFile)throw std::runtime_error("read changed file size");
 for(int round=-3;round<=10;++round) {
  const size_t ordinal=static_cast<size_t>(round+3);auto old=backend->openLogicalGraphReadSnapshot();const auto prior=expected;
  readAll(old,prior);const uint64_t propertyEdge=originalEdges[14U+ordinal];const auto oldValue=old->readEdgeProperty(propertyEdge,"epoch");
  const auto timeWrite=[&](const char *operation,const auto &write) {
   const bool diagnostic=round==10;const auto before=std::filesystem::file_size(path);
   if(diagnostic){debug::PerfTrace::reset();debug::PerfTrace::setValueCollectionEnabled(true);}
   const auto start=Clock::now();write();const double ms=std::chrono::duration<double,std::milli>(Clock::now()-start).count();
   if(diagnostic)counters("mixed",operation);
   else if(round>=0)std::cout<<"{\"event\":\"write_sample\",\"operation\":\""<<operation<<"\",\"round\":"<<round<<",\"milliseconds\":"<<ms<<",\"file_before\":"<<before<<",\"file_after\":"<<std::filesystem::file_size(path)<<"}\n";
  };
  std::vector<uint64_t> created;const size_t source=ordinal%2==0?0:ordinal%count;const size_t target=(ordinal*71U+3U)%count;
  const std::vector<LogicalGraphEdgeCreateRequest> requests{{nodes[source],nodes[target],"MixedRouteC",{}}};
  timeWrite("create_edge",[&]{created=backend->createLogicalEdges(requests);});if(created.size()!=1)throw std::runtime_error("mixed create mismatch");
  expected[source].push_back({created[0],nodes[source],nodes[target],"MixedRouteC"});
  const uint64_t deleted=originalEdges[ordinal];timeWrite("delete_edge",[&]{backend->deleteLogicalEdges({LogicalGraphEdgeDeleteRequest{deleted}});});
  size_t erased=0;for(auto &edges:expected)erased+=std::erase_if(edges,[&](const auto &edge){return edge.edge_id==deleted;});if(erased!=1)throw std::runtime_error("expected delete mismatch");
  timeWrite("replace_edge_property",[&]{backend->replaceLogicalEdgeProperties({LogicalGraphPropertyReplaceRequest{propertyEdge,{{"epoch",PropertyValue(static_cast<int64_t>(ordinal))}}}});});
  readAll(old,prior);if(old->readEdgeProperty(propertyEdge,"epoch")!=oldValue)throw std::runtime_error("old property changed");
  auto current=backend->openLogicalGraphReadSnapshot();
  for(const char *pass:{"first","repeat","two_hop"})timeRead(current,expected,"mixed",pass,round);
  if(current->readEdgeProperty(propertyEdge,"epoch")!=PropertyValue(static_cast<int64_t>(ordinal)))throw std::runtime_error("new property mismatch");
 }
 const auto finalFile=std::filesystem::file_size(path);backend->close();backend->open();
 {auto reopened=backend->openLogicalGraphReadSnapshot();if(readAll(reopened,expected)!=edgeCount)throw std::runtime_error("reopened count mismatch");readTwoHop(reopened,expected);}
 backend->close();std::cout<<"{\"event\":\"fixture\",\"shape\":\""<<shape<<"\",\"nodes\":"<<count<<",\"edges\":"<<edgeCount<<",\"initial_file_bytes\":"<<initialFile<<",\"final_file_bytes\":"<<finalFile<<"}\n";
}
