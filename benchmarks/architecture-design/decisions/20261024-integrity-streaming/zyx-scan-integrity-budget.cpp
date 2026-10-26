#include <iostream>
#include <map>
#include "graph/query/execution/PrepaidMemoryResource.hpp"
#include "graph/storage/relocatable/maintenance/RelocatableIntegrityScanner.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/PreadHelper.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
int main(int argc,char **argv) {
 if(argc!=2)return 2;
 const auto fd=graph::storage::portable_open(argv[1],O_RDONLY);
 if(fd==graph::storage::INVALID_FILE_HANDLE)return 3;
 {
  auto io=std::make_shared<graph::storage::StorageIO>(nullptr,graph::storage::INVALID_FILE_HANDLE,fd,graph::storage::StorageIOAccessMode::SIOAM_READ_ONLY);
  auto metadata=graph::storage::RelocatableMetadataStore::openReadOnly(io);
  auto pin=metadata->openSnapshot();std::map<int,uint64_t> kinds;uint64_t pages=0,catalogs=0;
  using namespace graph::query::execution::detail;
  constexpr size_t maximum=8U*1024U*1024U;
  auto process=std::make_shared<ProcessResourceBudget>(ResourceLimits::uniform(maximum));
  auto database=std::make_shared<DatabaseResourceBudget>(process,ResourceLimits::uniform(maximum));
  auto account=std::make_shared<ResourceAccount>(database,ResourceClass::RCK_MAINTENANCE,maximum);
  auto workspace=std::make_shared<PrepaidMemoryResource>(account,maximum,std::pmr::new_delete_resource());
  pin.release();
  const auto report=graph::storage::RelocatableIntegrityScanner(io,*metadata,workspace).scan();
  std::cout<<"{\"file_bytes\":"<<io->fileSize()<<",\"page_locations\":"<<report.page_locations
      <<",\"workspace_peak_bytes\":"<<workspace->peakAdmitted()<<",\"workspace_retained_bytes\":"<<workspace->used()
      <<",\"source_write_calls\":"<<io->stats().write_calls<<"}\n";
 }
 graph::storage::portable_close(fd);
}
