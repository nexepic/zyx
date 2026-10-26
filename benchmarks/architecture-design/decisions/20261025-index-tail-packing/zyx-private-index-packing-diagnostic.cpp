#include <iostream>
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/format/BuildingScanMaintenance.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/PreadHelper.hpp"
#include "graph/storage/PwriteHelper.hpp"
#include "graph/utils/OwnedHeapReservation.hpp"
namespace graph::storage { class BuildingScanMembershipTestAccess { public:
 static BuildingScanMaintenanceRequest maintenance(){return BuildingScanMaintenanceRequest{};}
}; }
int main(int argc,char **argv){
 if(argc!=2)return 2;
 using namespace graph::storage;
 try{
 auto fd=portable_open_rw(argv[1]); if(fd==INVALID_FILE_HANDLE)throw std::runtime_error("open diagnostic clone failed");
 auto io=std::make_shared<StorageIO>(nullptr,fd,fd,StorageIOAccessMode::SIOAM_READ_WRITE,UINT64_C(1024)*1024U*1024U);
 auto source=std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U*1024U);
 auto work=std::make_shared<graph::query::execution::MaterializedByteBudget>(7U*1024U*1024U);
 auto metadata=RelocatableMetadataStore::open(io,source);
 size_t packed=0,released=0,gc=0;
 for(size_t step=0;step<10000;++step){
  graph::utils::OwnedAllocationScope scope(work);
  Superblock root;std::optional<CatalogSegmentRetirementRequest> retirement;
  {auto pin=metadata->openSnapshot();root=pin.superblock();
   if(!(root.flags&RSS_SUPERBLOCK_FLAG_SCAN_BUILDING))throw std::runtime_error("diagnostic must remain BUILDING");
   metadata->visitRetireQueueEntries(pin,[&](const auto&e){if(retirement)return;auto pool=metadata->findCatalog(pin,e.segment_id);retirement=CatalogSegmentRetirementRequest{pool.segment_id,pool.generation};});}
  if(retirement){auto result=metadata->tryReleaseRetiredCatalogSegments(std::span(&*retirement,1),LogSequenceNumber(root.checkpoint_lsn.value()+1));if(!result)throw std::runtime_error("diagnostic retirement pinned");++released;continue;}
  auto request=BuildingScanMembershipTestAccess::maintenance();request.expected_database_id=root.database_id;request.expected_generation=root.generation;request.expected_descriptor=root.hybrid_property_descriptor_root;request.memory=work;
  if(metadata->reclaimOneBuildingScanMetadataPool(request,LogSequenceNumber(root.checkpoint_lsn.value()+1)).superblock!=root){++gc;continue;}
  bool moved=false;uint64_t upper=UINT64_MAX;
  for(;;){std::optional<CatalogEntry> selected;
   {auto pin=metadata->openSnapshot();metadata->visitCatalogEntries(pin,[&](const auto&e){if(e.kind==SegmentKind::SGK_INDEX_PAGE_POOL&&e.state==SegmentState::SGS_SEALED&&e.physical_slot.value()<upper&&e.physical_slot.value()>1&&metadata->findIndexRootOptional(pin,e.owner_id)&&(!selected||e.physical_slot>selected->physical_slot))selected=e;});}
   if(!selected)break;
   graph::utils::OwnedHeapReservation prepaid(work,1024U*1024U);auto block=std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U*1024U);
   NativeHybridPropertyPoolRelocation move{root.database_id,root.generation,root.hybrid_property_descriptor_root,selected->segment_id,PhysicalSlotId(1),PhysicalSlotId(selected->physical_slot.value()-1),block};
   auto result=metadata->relocateHybridPropertyIndexPool(move,LogSequenceNumber(root.checkpoint_lsn.value()+1));
   if(result!=root){++packed;moved=true;std::cerr<<"step="<<step<<" packed="<<packed<<" source_slot="<<selected->physical_slot.value()<<"\n";break;}
   upper=selected->physical_slot.value();
  }
  if(moved)continue;
  const auto tail=metadata->truncateReleasedTail();
  std::cout<<"packed="<<packed<<" released="<<released<<" metadata_gc="<<gc<<" file_bytes="<<io->fileSize()<<" flags="<<metadata->openSnapshot().superblock().flags<<"\n";
  static_cast<void>(tail);return 0;
 }
 throw std::runtime_error("diagnostic step bound reached");
 }catch(const std::exception&e){std::cerr<<"diagnostic_error="<<e.what()<<"\n";return 1;}
}
