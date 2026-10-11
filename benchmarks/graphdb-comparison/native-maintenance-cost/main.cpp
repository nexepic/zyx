#include "graph/storage/relocatable/backend/RelocatableStorageBackend.hpp"
#include "graph/storage/relocatable/maintenance/LargeValueReachabilityReclaimer.hpp"
#include "graph/debug/PerfTrace.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <vector>
using namespace graph;
using namespace graph::storage;
int main(int argc,char **argv){
 if(argc!=7)throw std::invalid_argument("usage: maintenance-cost PRIVATE_DB DELETE_BATCH scattered COUNT PAYLOAD_BYTES reclaim");
 const std::filesystem::path path(argv[1]);const size_t batch=std::stoull(argv[2]),count=std::stoull(argv[4]),bytes=std::stoull(argv[5]);
 if(std::filesystem::exists(path)||!batch||batch>8||batch>count||!count||count>128||std::gcd(count,size_t{17})!=1||bytes<4096||bytes>8192||std::string(argv[3])!="scattered"||std::string(argv[6])!="reclaim")throw std::invalid_argument("bounded fresh maintenance fixture required");
 LargeValueReachabilityReclaimOptions reclaim;reclaim.maximum_manifest_locations=512;reclaim.maximum_manifest_metadata_pages=512;reclaim.maximum_manifest_nodes=512;reclaim.maximum_owner_locations=512;reclaim.maximum_owner_metadata_pages=512;reclaim.maximum_data_pages=1024;reclaim.maximum_retirements=31;reclaim.maximum_page_locations=4096;reclaim.maximum_pools=64;reclaim.first_index_slot=PhysicalSlotId(1);reclaim.last_index_slot=PhysicalSlotId(999);reclaim.first_journal_state_slot=PhysicalSlotId(1000);reclaim.last_journal_state_slot=PhysicalSlotId(1015);reclaim.journal_state_page_owner_id=0x4C564D4AU;reclaim.journal_state_id=0x4C564D4AU;
 struct Cleanup{std::filesystem::path p;~Cleanup(){std::error_code e;std::filesystem::remove(p,e);}};
 for(int round=-3;round<=10;++round){
  Cleanup cleanup{path};RelocatableStorageBackendOptions options;options.data_path=path;options.open_mode=OpenMode::OPEN_CREATE_NEW_FILE;auto backend=RelocatableStorageBackend::initializeNew(options);backend->open();
  std::vector<LogicalGraphNodeCreateRequest> requests;for(size_t i=0;i<count;++i)requests.push_back({{"Document"},{{"ordinal",PropertyValue(static_cast<int64_t>(i))},{"body",PropertyValue(std::string(bytes,char('a'+i%26)))}}});
  const auto ids=backend->createLogicalNodes(requests);if(ids.size()!=count)throw std::runtime_error("fixture count mismatch");
  auto old=backend->openLogicalGraphReadSnapshot();const auto oldValues=old->readNodesShapedSorted(ids,EntityReadShape{.include_labels=true,.all_properties=true});
  std::vector<bool> removed(count,false);std::vector<LogicalGraphNodeDeleteRequest> deletes;for(size_t i=0;i<batch;++i){const size_t ordinal=(i*17)%count;removed[ordinal]=true;deletes.push_back({ids[ordinal],false,0});}backend->deleteLogicalNodes(deletes);
  if(old->readNodesShapedSorted(ids,EntityReadShape{.include_labels=true,.all_properties=true})!=oldValues)throw std::runtime_error("old values changed before reclaim");old.reset();
  const bool diagnostic=round==10;if(diagnostic){debug::PerfTrace::reset();debug::PerfTrace::setEnabled(true);debug::PerfTrace::setValueCollectionEnabled(true);}
  const auto start=std::chrono::steady_clock::now();const auto report=backend->reclaimUnreachableLargeValues(reclaim);const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
  if(diagnostic){const auto trace=debug::PerfTrace::snapshotAndReset();debug::PerfTrace::setEnabled(false);debug::PerfTrace::setValueCollectionEnabled(false);for(const auto &[key,value]:trace)std::cout<<"{\"event\":\"counter\",\"key\":\""<<key<<"\",\"value\":"<<value.totalValue<<",\"nanoseconds\":"<<value.totalNs<<"}\n";}
  if(report.retired_values!=batch||!report.retired_data_pages||!report.retired_manifest_pages)throw std::runtime_error("reclamation count mismatch");
  auto now=backend->openLogicalGraphReadSnapshot();for(size_t i=0;i<count;++i){if(now->containsNode(ids[i])==removed[i])throw std::runtime_error("reclaim visibility mismatch");if(!removed[i]&&now->readNode(ids[i])!=oldValues[i])throw std::runtime_error("reclaim changed surviving values");}now.reset();
  if(backend->reclaimUnreachableLargeValues(reclaim).retired_values!=0)throw std::runtime_error("repeated reclaim not idempotent");static_cast<void>(backend->verifyIntegrity());
  if(round>=0&&!diagnostic)std::cout<<"{\"event\":\"sample\",\"round\":"<<round<<",\"milliseconds\":"<<ms<<",\"checked\":"<<count<<"}\n";
  if(diagnostic)std::cout<<"{\"event\":\"fixture\",\"file_bytes\":"<<std::filesystem::file_size(path)<<",\"retired_values\":"<<report.retired_values<<",\"retired_data_pages\":"<<report.retired_data_pages<<",\"retired_manifest_pages\":"<<report.retired_manifest_pages<<"}\n";
  backend->close();
 }
}
