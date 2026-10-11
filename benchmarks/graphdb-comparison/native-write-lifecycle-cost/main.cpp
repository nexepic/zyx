#include "graph/storage/relocatable/backend/RelocatableStorageBackend.hpp"
#include "graph/debug/PerfTrace.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>
using namespace graph;
using namespace graph::storage;
int main(int argc,char **argv) {
 if(argc!=7)throw std::invalid_argument("usage: lifecycle-cost PRIVATE_DB BATCH clustered|scattered COUNT PAYLOAD_BYTES create|delete");
 const std::filesystem::path path(argv[1]);const size_t batch=std::stoull(argv[2]),count=std::stoull(argv[4]),bytes=std::stoull(argv[5]);
 const std::string distribution(argv[3]),mode(argv[6]);
 if(std::filesystem::exists(path)||!batch||batch>count||count>8192||std::gcd(count,size_t{17})!=1||bytes>8192||(distribution!="clustered"&&distribution!="scattered")||(mode!="create"&&mode!="delete"))throw std::invalid_argument("invalid bounded fresh fixture");
 struct Cleanup{std::filesystem::path p;~Cleanup(){std::error_code e;std::filesystem::remove(p,e);}};
 for(int round=-3;round<=10;++round){
  Cleanup cleanup{path};RelocatableStorageBackendOptions options;options.data_path=path;options.open_mode=OpenMode::OPEN_CREATE_NEW_FILE;
  auto backend=RelocatableStorageBackend::initializeNew(options);backend->open();
  std::vector<LogicalGraphNodeCreateRequest> fixture;
  for(size_t i=0;i<count;++i)fixture.push_back({{"Document"},{{"ordinal",PropertyValue(static_cast<int64_t>(i))},{"body",PropertyValue(std::string(bytes,char('a'+i%26)))}}});
  std::vector<uint64_t> ids;
  if(mode=="delete")for(size_t i=0;i<count;i+=128){const size_t end=std::min(i+128,count);auto made=backend->createLogicalNodes({fixture.begin()+i,fixture.begin()+end});ids.insert(ids.end(),made.begin(),made.end());}
  std::vector<LogicalGraphNodeCreateRequest> creates;std::vector<LogicalGraphNodeDeleteRequest> deletes;std::vector<bool> removed(count,false);
  for(size_t i=0;i<batch;++i){const size_t ordinal=distribution=="clustered"?i:(i*17)%count;creates.push_back(fixture[ordinal]);if(mode=="delete"){deletes.push_back({ids.at(ordinal),false,0U});removed[ordinal]=true;}}
  auto old=backend->openLogicalGraphReadSnapshot();const bool diagnostic=round==10;
  if(diagnostic){debug::PerfTrace::reset();debug::PerfTrace::setEnabled(true);debug::PerfTrace::setValueCollectionEnabled(true);}
  const auto start=std::chrono::steady_clock::now();
  if(mode=="create")ids=backend->createLogicalNodes(creates);else backend->deleteLogicalNodes(deletes);
  const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
  if(diagnostic){const auto trace=debug::PerfTrace::snapshotAndReset();debug::PerfTrace::setEnabled(false);debug::PerfTrace::setValueCollectionEnabled(false);for(const auto &[key,value]:trace)std::cout<<"{\"event\":\"counter\",\"key\":\""<<key<<"\",\"value\":"<<value.totalValue<<",\"nanoseconds\":"<<value.totalNs<<"}\n";}
  auto now=backend->openLogicalGraphReadSnapshot();
  if(mode=="create"&&ids.size()!=batch)throw std::runtime_error("creation count mismatch");
  for(size_t i=0;i<ids.size();++i){
   if(mode=="create"){if(old->containsNode(ids[i])||!now->containsNode(ids[i]))throw std::runtime_error("creation visibility mismatch");}
   else if(!old->containsNode(ids[i])||now->containsNode(ids[i])==removed[i])throw std::runtime_error("deletion visibility mismatch");
   const auto &want=mode=="create"?creates[i]:fixture[i];const auto value=(mode=="delete"?old:now)->readNode(ids[i]);
   if(value.node_id!=ids[i]||value.labels!=want.labels||value.properties.size()!=want.properties.size())throw std::runtime_error("entity shape mismatch");
   for(const auto &entry:want.properties){const auto found=std::ranges::find(value.properties,entry.key,&LogicalGraphPropertyEntry::key);if(found==value.properties.end()||found->value!=entry.value)throw std::runtime_error("property mismatch");}
   if(mode=="delete"&&!removed[i]&&now->readNode(ids[i])!=value)throw std::runtime_error("surviving entity changed");
  }
  if(round>=0&&!diagnostic)std::cout<<"{\"event\":\"sample\",\"round\":"<<round<<",\"milliseconds\":"<<ms<<",\"checked\":"<<ids.size()<<"}\n";
  if(diagnostic)std::cout<<"{\"event\":\"fixture\",\"file_bytes\":"<<std::filesystem::file_size(path)<<"}\n";
  old.reset();now.reset();backend->close();
 }
}
