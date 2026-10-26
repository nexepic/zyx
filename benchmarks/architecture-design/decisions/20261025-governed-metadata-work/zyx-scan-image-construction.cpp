#include <iostream>
#include "graph/storage/relocatable/format/ScanBuildingImageBuilder.hpp"
int main(int argc,char **argv) {
 if(argc!=3)return 2;
 try {
  using namespace graph::storage;
  auto source=std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U*1024U);
  auto work=std::make_shared<graph::query::execution::MaterializedByteBudget>(7U*1024U*1024U);
  std::cerr<<"stage=clone_and_membership\n";
  auto builder=ScanBuildingImageBuilder::cloneWithPrimaryMembership(argv[1],argv[2],source,UINT64_C(2)*1024U*1024U*1024U,989);
  std::cerr<<"stage=fields\n";
  auto fields=builder->constructFields(nullptr,work);
  std::cerr<<"examined_groups="<<fields.examined_groups<<" converted_groups="<<fields.converted_groups<<" row_fallback_groups="<<fields.row_fallback_groups<<"\n";
  for(size_t step=0;step<100000;++step){
   auto retired=builder->releaseRetiredPoolOnce(work);
   if(retired.blocked) throw std::runtime_error("retired source pool remains pinned");
   if(retired.released){std::cerr<<"step="<<step<<" retired_pool_released\n";continue;}
   auto before=builder->buildingRoot();
   auto reclaimed=builder->reclaimMetadataOnce(work);
   if(reclaimed.superblock!=before){std::cerr<<"step="<<step<<" metadata_reclaimed\n";continue;}
   if(builder->compactDataPoolOnce(work)){std::cerr<<"step="<<step<<" data_compacted\n";continue;}
   if(builder->compactMetadataPoolOnce(work)){std::cerr<<"step="<<step<<" metadata_compacted\n";continue;}
   static_cast<void>(builder->truncateReleasedTailOnce(work));
   std::cerr<<"stage=complete\n";
   auto ready=builder->complete(work);
   std::cout<<"{\"generation\":"<<ready.generation.value()<<",\"source_account_used\":"<<source->used()<<",\"maintenance_account_used\":"<<work->used()<<"}\n";
   return 0;
  }
  throw std::runtime_error("construction step limit reached");
 }catch(const std::exception&e){std::cerr<<"construction_error="<<e.what()<<"\n";return 1;}
}
