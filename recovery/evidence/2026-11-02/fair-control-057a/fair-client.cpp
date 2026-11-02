#include "graph/storage/relocatable/backend/RelocatableStorageBackend.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <vector>
using namespace graph;using namespace graph::storage;
int main(int argc,char** argv){
 if(argc!=2||std::filesystem::exists(argv[1]))return 2;
 try{
  RelocatableStorageBackendOptions options;options.data_path=argv[1];options.open_mode=OpenMode::OPEN_CREATE_NEW_FILE;
  auto backend=RelocatableStorageBackend::initializeNew(std::move(options));backend->open();
  std::vector<LogicalGraphNodeCreateRequest> initial(64,{{},{{"value",PropertyValue(std::string(256,'x'))}}});
  auto ids=backend->createLogicalNodes(initial);if(ids.size()!=64)throw std::runtime_error("seed size");
  std::vector<uint64_t> deleted;std::vector<double> timings;auto start=std::chrono::steady_clock::now();
  for(int i=0;i<128;++i){
   auto before=std::chrono::steady_clock::now();deleted.push_back(ids[i%64]);backend->deleteLogicalNodes({{ids[i%64],false,0U}});
   timings.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-before).count());
   auto next=backend->createLogicalNodes({{{},{{"value",PropertyValue(std::string(256,'x'))}}}});
   if(next.size()!=1)throw std::runtime_error("create count");ids[i%64]=next[0];
  }
  double secs=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
  auto pin=backend->openLogicalGraphReadSnapshot();for(auto id:ids){auto n=pin->readNodeShaped(id,{.all_properties=true});if(n.properties.size()!=1||n.properties[0].value!=PropertyValue(std::string(256,'x')))throw std::runtime_error("bad final value");}for(auto id:deleted){bool rejected=false;try{static_cast<void>(pin->readNode(id));}catch(const FormatException&){rejected=true;}if(!rejected)throw std::runtime_error("deleted node remains visible");}pin.reset();
  static_cast<void>(backend->verifyIntegrity());backend->close();
  std::sort(timings.begin(),timings.end());double total=0;for(double x:timings)total+=x;
  std::cout<<"iterations=128 failures=0 seconds="<<secs<<" pairs_per_s="<<128/secs<<" delete_mean_ms="<<total/128<<" delete_p95_ms="<<timings[(128*95+99)/100-1]<<" file_bytes="<<std::filesystem::file_size(argv[1])<<'\n';
  return 0;
 }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
}
