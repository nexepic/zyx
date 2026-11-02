#include "graph/storage/relocatable/backend/RelocatableStorageBackend.hpp"
#include "graph/storage/LogicalGraphTransactionOverlay.hpp"
#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>
using namespace graph; using namespace graph::storage;
int main(int argc,char** argv) {
 if(argc!=3) return 2;
 const int clients=std::stoi(argv[1]); const std::filesystem::path path=argv[2];
 if((clients!=2 && clients!=4)||std::filesystem::exists(path)) return 2;
 try {
  RelocatableStorageBackendOptions opts; opts.data_path=path; opts.open_mode=OpenMode::OPEN_CREATE_NEW_FILE;
  auto backend=RelocatableStorageBackend::initializeNew(std::move(opts)); backend->open();
  std::vector<LogicalGraphNodeCreateRequest> requests;
  for(int i=0;i<clients;++i) requests.push_back({{},{{"rank",PropertyValue(0)}}});
  const auto nodes=backend->createLogicalNodes(requests);
  if(nodes.size()!=static_cast<size_t>(clients)) throw std::runtime_error("seed failed");
  std::barrier gate(clients+1); std::atomic<unsigned> failures{0}, conflicts{0}, retries{0};
  std::vector<std::vector<double>> latencies(clients); std::vector<std::thread> workers;
  for(int c=0;c<clients;++c) workers.emplace_back([&,c] {
   gate.arrive_and_wait();
   try {
    for(int i=0;i<100;++i) {
     if(i%10==9) {
      const auto start=std::chrono::steady_clock::now();
      bool committed=false;
      for(int attempt=0;attempt<4 && !committed;++attempt){
       try {
        auto tx=backend->beginLogicalGraphTransaction(false); LogicalGraphMutationBatch batch;
        batch.property_replacements={{{LogicalGraphMutationEntityKind::LGMK_NODE,LogicalGraphMutationReferenceKind::LGRK_PERSISTED,nodes[c]},{{"rank",PropertyValue(i/10+1)}}}};
        static_cast<void>(tx->stageMutationBatch(std::move(batch))); static_cast<void>(tx->commit()); committed=true;
       }catch(const LogicalGraphMutationConflict&){++conflicts;if(attempt<3)++retries;}
      }
      if(!committed)throw std::runtime_error("conflict retry limit");
      latencies[c].push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
     } else {
      auto snapshot=backend->openLogicalGraphReadSnapshot();
      const auto node=snapshot->readNodeShaped(nodes[c],{.all_properties=true});
      if(node.properties.empty()) ++failures;
     }
    }
   } catch(const std::exception& e) { std::cerr << "client " << c << ": " << e.what() << '\n'; ++failures; }
  });
  const auto begin=std::chrono::steady_clock::now(); gate.arrive_and_wait();
  for(auto& w:workers) w.join();
  const auto seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();
  auto snap=backend->openLogicalGraphReadSnapshot();
  for(int c=0;c<clients;++c) {
   const auto n=snap->readNodeShaped(nodes[c],{.all_properties=true});
   const auto it=std::ranges::find(n.properties,"rank",&LogicalGraphPropertyEntry::key);
   if(it==n.properties.end() || it->value!=PropertyValue(10) || latencies[c].size()!=10) ++failures;
  }
  snap.reset(); static_cast<void>(backend->verifyIntegrity()); backend->close();
  std::vector<double> all; for(auto& v:latencies) all.insert(all.end(),v.begin(),v.end());
  std::sort(all.begin(),all.end()); double sum=0;for(double x:all) sum+=x;
  std::cout << "clients=" << clients << " operations=" << clients*100 << " failures=" << failures.load()
   << " conflicts=" << conflicts.load() << " retries=" << retries.load() << " seconds=" << seconds << " throughput=" << clients*100/seconds << " commit_mean_ms=" << (all.empty()?0:sum/all.size())
   << " commit_p95_ms=" << (all.empty()?0:all[(all.size()*95+99)/100-1]) << '\n';
  return failures?1:0;
 } catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
