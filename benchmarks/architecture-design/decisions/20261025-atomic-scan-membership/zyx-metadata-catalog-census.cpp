#include <fstream>
#include <iostream>
#include <bit>
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"
int main(int argc, char **argv) {
 if(argc!=2)return 2;
 auto file=std::make_shared<std::fstream>(argv[1],std::ios::in|std::ios::binary);
 if(!file->is_open())return 3;
 auto io=std::make_shared<graph::storage::StorageIO>(file,graph::storage::INVALID_FILE_HANDLE,graph::storage::INVALID_FILE_HANDLE);
 auto metadata=graph::storage::RelocatableMetadataStore::openReadOnly(io);auto pin=metadata->openSnapshot();
 size_t pools=0,frames=0;
 metadata->visitCatalogEntries(pin,[&](const graph::storage::CatalogEntry &entry){
  using namespace graph::storage;
  if(entry.kind!=SegmentKind::SGK_METADATA_PAGE_POOL||entry.state==SegmentState::SGS_OBSOLETE)return;
  const auto offset=PhysicalSegmentAllocator::checkedSlotOffset(pin.superblock().data_region_offset,entry.physical_slot)+RSS_ENVELOPE_BYTES;
  std::optional<MetadataPagePoolHeader> selected;
  for(size_t record=0;record<2;++record){std::array<std::byte,RSS_METADATA_PAGE_POOL_HEADER_BYTES> bytes{};
   if(io->readAt(offset+record*bytes.size(),bytes.data(),bytes.size())!=bytes.size())throw std::runtime_error("short header");
   try{auto h=MetadataPagePoolHeaderCodec::decode(bytes);if(!selected||h.control_sequence>selected->control_sequence)selected=h;}catch(const FormatException&){}
  }
  if(!selected||selected->segment_id!=entry.segment_id||selected->generation!=entry.generation||selected->committed_lsn!=entry.committed_lsn)throw std::runtime_error("header ownership mismatch");
  ++pools;frames+=std::popcount(selected->allocated_frames);
 });
 std::cout<<"{\"catalog_metadata_pools\":"<<pools<<",\"allocated_metadata_frames\":"<<frames<<"}\n";
}
