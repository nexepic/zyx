#include "graph/storage/relocatable/backend/RelocatableStorageBackend.hpp"
#include "graph/debug/PerfTrace.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
using namespace graph;
using namespace graph::storage;
using Clock=std::chrono::steady_clock;
int main(int argc,char **argv) {
 if(argc!=4 && argc!=6 && argc!=7) throw std::invalid_argument("usage: write-batch-cost PRIVATE_DB BATCH_SIZE clustered|scattered [NODE_COUNT PAYLOAD_BYTES [read-update]]");
 const std::filesystem::path path(argv[1]);const size_t batchSize=std::stoull(argv[2]);const std::string distribution(argv[3]);
 const size_t count=argc>=6?std::stoull(argv[4]):256U;
 const bool mixed=argc==7;
 if(mixed && std::string(argv[6])!="read-update")throw std::invalid_argument("unknown workload mode");
 const size_t payloadBytes=argc>=6?std::stoull(argv[5]):96U;
 if(!count || count>8192U || count%128U || std::gcd(count,size_t{17U})!=1U || payloadBytes>8192U || count*payloadBytes>64U*1024U*1024U) throw std::invalid_argument("bounded divisible fixture with unique scattered ordinals required");
 if(std::filesystem::exists(path)||!batchSize||batchSize>count||(distribution!="clustered"&&distribution!="scattered"))throw std::invalid_argument("new bounded fixture required");
 struct Cleanup{std::filesystem::path p;~Cleanup(){std::error_code e;std::filesystem::remove(p,e);}}cleanup{path};
 RelocatableStorageBackendOptions options;options.data_path=path;options.open_mode=OpenMode::OPEN_CREATE_NEW_FILE;
 auto backend=RelocatableStorageBackend::initializeNew(options);backend->open();
 std::vector<uint64_t> ids;std::vector<std::vector<LogicalGraphWriteProperty>> expected(count);
 for(size_t i=0;i<count;++i)expected[i]={{"p0",PropertyValue(static_cast<int64_t>(i))},{"p1",PropertyValue(std::string(payloadBytes,char('a'+i%26)))}};
 for(size_t offset=0;offset<count;offset+=128){
  std::vector<LogicalGraphNodeCreateRequest> requests;
  for(size_t i=offset;i<offset+128;++i)requests.push_back({{},expected[i]});
  const auto created=backend->createLogicalNodes(requests);if(created.size()!=128)throw std::runtime_error("fixture count mismatch");ids.insert(ids.end(),created.begin(),created.end());
 }
 for(int round=-3;round<=10;++round){
  std::vector<LogicalGraphPropertyReplaceRequest> requests;std::vector<size_t> ordinals;
  for(size_t i=0;i<batchSize;++i){const size_t ordinal=distribution=="clustered"?i:(i*17U)%count;ordinals.push_back(ordinal);requests.push_back({ids[ordinal],expected[ordinal]});requests.back().properties[0].value=PropertyValue(static_cast<int64_t>(10000+round));}
  auto old=backend->openLogicalGraphReadSnapshot();
  std::vector<uint64_t> readIds;for(const auto &request:requests)readIds.push_back(request.entity_id);
  std::ranges::sort(readIds);
  using ReadResults=decltype(old->readNodesShapedSorted(readIds,EntityReadShape{.all_properties=true}));
  std::optional<ReadResults> measuredRead;
  const bool diagnostic=round==10;
  if(diagnostic){debug::PerfTrace::reset();debug::PerfTrace::setEnabled(true);debug::PerfTrace::setValueCollectionEnabled(true);}
  const auto start=Clock::now();
  if(mixed)measuredRead=old->readNodesShapedSorted(readIds,EntityReadShape{.all_properties=true});
  const auto readEnd=Clock::now();backend->replaceLogicalNodeProperties(requests);
  const auto finish=Clock::now();
  const double ms=std::chrono::duration<double,std::milli>(finish-start).count();
  const double readMs=std::chrono::duration<double,std::milli>(readEnd-start).count();
  const double writeMs=std::chrono::duration<double,std::milli>(finish-readEnd).count();
  if(diagnostic){const auto trace=debug::PerfTrace::snapshotAndReset();debug::PerfTrace::setEnabled(false);debug::PerfTrace::setValueCollectionEnabled(false);for(const auto &[key,value]:trace)std::cout<<"{\"event\":\"counter\",\"key\":\""<<key<<"\",\"value\":"<<value.totalValue<<",\"nanoseconds\":"<<value.totalNs<<"}\n";}
  auto now=backend->openLogicalGraphReadSnapshot();
  // Exhaustively validate affected and unaffected fields and old visibility outside timing.
  const auto oldValues=old->readNodesShapedSorted(ids,EntityReadShape{.all_properties=true});
  const auto values=now->readNodesShapedSorted(ids,EntityReadShape{.all_properties=true});
  if(oldValues.size()!=count||values.size()!=count)throw std::runtime_error("result shape mismatch");
  for(size_t i=0;i<count;++i){
   for(const auto &entry:expected[i]){const auto found=std::ranges::find(oldValues[i].properties,entry.key,&LogicalGraphPropertyEntry::key);if(found==oldValues[i].properties.end()||found->value!=entry.value)throw std::runtime_error("old snapshot changed");}
  }
  if(measuredRead){if(measuredRead->size()!=readIds.size())throw std::runtime_error("mixed read count mismatch");
   for(size_t i=0;i<readIds.size();++i){const size_t ordinal=std::lower_bound(ids.begin(),ids.end(),readIds[i])-ids.begin();
    if((*measuredRead)[i].properties.size()!=expected[ordinal].size())throw std::runtime_error("mixed read field count mismatch");
    for(const auto &entry:expected[ordinal]){const auto found=std::ranges::find((*measuredRead)[i].properties,entry.key,&LogicalGraphPropertyEntry::key);if(found==(*measuredRead)[i].properties.end()||found->value!=entry.value)throw std::runtime_error("mixed read field value mismatch");}
   }
  }
  for(size_t i=0;i<requests.size();++i)expected[ordinals[i]]=requests[i].properties;
  for(size_t i=0;i<count;++i){if(values[i].properties.size()!=expected[i].size())throw std::runtime_error("field count mismatch");for(const auto &entry:expected[i]){const auto found=std::ranges::find(values[i].properties,entry.key,&LogicalGraphPropertyEntry::key);if(found==values[i].properties.end()||found->value!=entry.value)throw std::runtime_error("field value mismatch");}}
  if(round>=0&&!diagnostic)std::cout<<"{\"event\":\"sample\",\"round\":"<<round<<",\"milliseconds\":"<<ms<<",\"read_milliseconds\":"<<readMs<<",\"write_milliseconds\":"<<writeMs<<",\"checked\":"<<count<<"}\n";
 }
 std::cout<<"{\"event\":\"fixture\",\"file_bytes\":"<<std::filesystem::file_size(path)<<"}\n";backend->close();
}
