#include <fstream>
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include "graph/storage/relocatable/backend/RelocatableStorageBackend.hpp"
#include "graph/storage/relocatable/entity/CertifiedScanMembership.hpp"
#include "graph/storage/relocatable/property/HybridPropertyReadContext.hpp"
namespace graph::storage {
// Read-only census access; actual pinned descriptor and producer grant remain mandatory.
class HybridPropertyReadContextTestAccess {
public:
 template<class Visitor> static bool visit(const HybridPropertyReadContext &context,
     std::span<const EntityPropertyKey> owners,uint32_t token,Visitor &&visitor) {
  return context.visitColumnOwnersSorted(owners,token,std::forward<Visitor>(visitor),{});
 }
};
}
using namespace graph::storage;
int main(int argc,char **argv) {
 if(argc!=2)return 2;
 try {
  auto memory=std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U*1024U);
  auto control=std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U*1024U);
  uint32_t token=0;
  {
   RelocatableStorageBackendOptions options;options.data_path=argv[1];options.open_mode=OpenMode::OPEN_EXISTING_FILE;
   RelocatableStorageBackend backend(options);backend.open();
   {auto snapshot=backend.openLogicalGraphReadSnapshot();token=snapshot->findPropertyToken("score").value();}
   backend.close();
  }
  graph::utils::OwnedAllocationScope owner(memory);
  auto stream=std::make_shared<std::fstream>(argv[1],std::ios::in|std::ios::out|std::ios::binary);
  auto io=std::make_shared<StorageIO>(stream,INVALID_FILE_HANDLE,INVALID_FILE_HANDLE);
  auto metadata=RelocatableMetadataStore::open(io);
  auto proof=VerifiedScanRoot::verifyComplete(io,*metadata,memory,control);
  auto pin=metadata->openSnapshot();
  SlottedPagePool pages(io,*metadata,SharedPagePoolOpenMode::SPPOM_READ_ONLY);
  HybridPropertyReadContext context(io,pages,*metadata,pin);auto window=context.borrowQueryWindow();
  std::pmr::unordered_set<uint64_t> groupPages(memory.get()),columnPages(memory.get()),allPages(memory.get());
  std::pmr::unordered_map<uint64_t,size_t> columnsPerPage(memory.get());
  size_t examined=0,groups=0,columns=0,columnBytes=0,primitiveBytes=0;
  bool complete=CertifiedScanMembership::visit(proof,io,*metadata,pin,memory,SIZE_MAX,512,{},
   [&](std::span<const uint64_t> ids,size_t n,const ScanBatchReadGrant &grant) {
    examined+=n;
    grant.read(ids,[&] {
     for(size_t first=0;first<ids.size();) {
      const uint64_t group=1U+(ids[first]-1U)/64U;size_t end=first+1U;
      while(end<ids.size() && 1U+(ids[end]-1U)/64U==group)++end;
      std::array<EntityPropertyKey,64> keys{};
      for(size_t i=first;i<end;++i)keys[i-first]={EntityPropertyOwnerKind::EPOK_NODE,ids[i]};
      ++groups;
      (void)HybridPropertyReadContextTestAccess::visit(context,std::span(keys).first(end-first),token,
       [&](const auto &,const HybridPropertyColumnRecord &column,const SlottedSlotReference &groupRef,const SlottedSlotReference &columnRef) {
        ++columns;columnBytes+=column.wireSize();primitiveBytes+=size_t(std::popcount(column.presence & ~column.nulls))*8U;
        groupPages.insert(groupRef.page_id.value());columnPages.insert(columnRef.page_id.value());
        allPages.insert(groupRef.page_id.value());allPages.insert(columnRef.page_id.value());
        ++columnsPerPage[columnRef.page_id.value()];
       });
      first=end;
     }
    });return true;
   });
  if(!complete || examined!=225000 || columns==0)throw std::runtime_error("census coverage differs from medium oracle");
  size_t singleColumnPages=0,maxColumns=0;
  for(auto [id,n]:columnsPerPage){singleColumnPages+=n==1;maxColumns=(std::max)(maxColumns,n);}
  std::cout<<"{\"timing_benchmark\":false,\"examined\":"<<examined<<",\"groups\":"<<groups<<",\"typed_score_groups\":"<<columns
   <<",\"score_token\":"<<token<<",\"group_pages\":"<<groupPages.size()<<",\"score_pages\":"<<columnPages.size()
   <<",\"union_pages\":"<<allPages.size()<<",\"single_score_group_pages\":"<<singleColumnPages<<",\"max_score_groups_per_page\":"<<maxColumns
   <<",\"encoded_score_bytes\":"<<columnBytes<<",\"nonnull_primitive_bytes\":"<<primitiveBytes
   <<",\"score_page_payload_coverage_bytes\":"<<columnPages.size()*RSS_PAGE_SIZE<<",\"source_account_live_bytes\":"<<memory->used()<<"}\n";
 }catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
}
