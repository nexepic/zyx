#include <iostream>
#include <map>
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
  metadata->visitCatalogEntries(pin,[&](const auto&){++catalogs;});
  static_cast<void>(metadata->visitPageLocationsUntil(pin,[&](const graph::storage::PageLocation &page){++pages;++kinds[int(metadata->findCatalog(pin,page.segment_id).kind)];return true;}));
  std::cout<<"{\"file_bytes\":"<<io->fileSize()<<",\"catalogs\":"<<catalogs<<",\"page_locations\":"<<pages<<",\"pool_kinds\":{";
  bool comma=false;for(auto [kind,count]:kinds){if(comma)std::cout<<',';comma=true;std::cout<<'\"'<<kind<<"\":"<<count;}
  std::cout<<"},\"source_write_calls\":"<<io->stats().write_calls<<"}\n";
 }
 graph::storage::portable_close(fd);
}
