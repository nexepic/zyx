#include "NativeColumnImageStore.hpp"
#include "graph/storage/PreadHelper.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#if defined(__APPLE__) || defined(__linux__)
#include <sys/resource.h>
#endif
using namespace graph::storage;
using graph::PropertyValue;
using zyx::benchmark::TypedPropertyBlock;
using zyx::benchmark::NativeColumnImageStore;
namespace {
using Clock=std::chrono::steady_clock;
struct File final {
 std::filesystem::path path;file_handle_t write=INVALID_FILE_HANDLE,read=INVALID_FILE_HANDLE;
 std::shared_ptr<std::fstream> stream;std::shared_ptr<StorageIO> io;
 static std::unique_ptr<File> create(const std::filesystem::path &path) {
  auto f=std::make_unique<File>();
  f->write=portable_create_rw_exclusive(path.string().c_str());
  if(f->write==INVALID_FILE_HANDLE) throw std::runtime_error("cannot exclusively create private file");
  f->path=path;f->stream=std::make_shared<std::fstream>(path,std::ios::in|std::ios::out|std::ios::binary);
  f->read=portable_open(path.string().c_str(),O_RDONLY);
  if(!f->stream->is_open() || f->read==INVALID_FILE_HANDLE) throw std::runtime_error("native private file handles unavailable");
  f->io=std::make_shared<StorageIO>(f->stream,f->write,f->read,StorageIOAccessMode::SIOAM_READ_WRITE);
  f->io->enableBufferPool(64U*1024U*1024U);return f;
 }
 ~File() {io.reset();stream.reset();portable_close_rw(write);portable_close(read);if(!path.empty()){std::error_code e;std::filesystem::remove(path,e);}}
};
LargeValueWriteOptions options() {
 LargeValueWriteOptions o;o.first_data_slot=PhysicalSlotId(40);o.last_data_slot=PhysicalSlotId(140);
 o.first_manifest_slot=PhysicalSlotId(141);o.last_manifest_slot=PhysicalSlotId(170);
 o.media_type=RSS_ENTITY_PROPERTY_LARGE_VALUE_MEDIA_TYPE;o.compression.codec=LargeValueCodec::LVC_LZ4;
 o.compression.allow_raw_fallback=true;o.extent_class=LargeValueExtentClass::LVEC_32K;return o;
}
PropertyCollectionRoot selectedOwner(LargeValueStore &store,const MetadataSnapshotPin &snapshot,const LargeValueRef &ref,
                                     uint64_t owner,std::span<const uint32_t> tokens) {
 auto manifest=store.manifest(snapshot,ref);LargeValueStore::RangeReadCache cache;
 auto header=store.readRangeFromManifestCached(snapshot,manifest,0,RSS_ENTITY_PROPERTY_LARGE_VALUE_PAYLOAD_HEADER_BYTES,cache);
 const size_t count=LittleEndianCodec::readU32(header,8);
 if(count>64) throw std::runtime_error("fixture directory limit");
 auto directory=store.readRangeFromManifestCached(snapshot,manifest,0,RSS_ENTITY_PROPERTY_LARGE_VALUE_PAYLOAD_HEADER_BYTES+count*8,cache);
 auto locations=EntityPropertyLargeValuePayloadCodec::decodeDirectory(directory);
 if(manifest.logical_bytes!=RSS_ENTITY_PROPERTY_LARGE_VALUE_PAYLOAD_HEADER_BYTES+LittleEndianCodec::readU32(directory,12)) throw std::runtime_error("fixture manifest mismatch");
 PropertyCollectionRoot root{owner,1U,{}};
 for(auto token:tokens) {
  auto it=std::lower_bound(locations.begin(),locations.end(),token,[](const auto &location,uint32_t key){return location.key_token<key;});
  if(it!=locations.end() && it->key_token==token) {
   auto bytes=store.readRangeFromManifestCached(snapshot,manifest,RSS_ENTITY_PROPERTY_LARGE_VALUE_PAYLOAD_HEADER_BYTES+it->offset,it->length,cache);
   root.entries.push_back(EntityPropertyLargeValuePayloadCodec::decodeEntry(bytes,token));
  }
 }
 return root;
}
void check(std::span<const PropertyCollectionRoot> result,std::span<const PropertyCollectionRoot> expected,std::span<const uint32_t> tokens) {
 if(result.size()!=expected.size()) throw std::runtime_error("native result cardinality");
 for(size_t i=0;i<result.size();++i) {
  auto root=expected[i];std::erase_if(root.entries,[&](const auto &entry){return !std::binary_search(tokens.begin(),tokens.end(),entry.key_token);});
  if(root!=result[i]) throw std::runtime_error("native property value mismatch");
 }
}
}
int main(int argc,char **argv) {
 try {
  if(argc!=7) throw std::runtime_error("usage: private_db row|row-full|column|column-range|column-atomic owners fields payload selected");
  const std::string version=argv[2];const size_t count=std::stoull(argv[3]),fields=std::stoull(argv[4]),payload=std::stoull(argv[5]),selected=std::stoull(argv[6]);
  if((version!="row" && version!="row-full" && version!="column" && version!="column-range" && version!="column-atomic") || !count || count>256 || !fields || fields>32 || !selected || selected>fields || payload>8192 || count*fields*payload>32U*1024U*1024U) throw std::runtime_error("native fixture bounds");
  auto file=File::create(argv[1]);Superblock initial;initial.database_id=DatabaseId(0x1020304050607080ULL);initial.generation=Generation(1);initial.checkpoint_lsn=LogSequenceNumber(1);
  auto metadata=RelocatableMetadataStore::initializeNew(file->io,initial);LargeValueStore values(file->io,*metadata);NativeColumnImageStore columns(values);SlottedPagePool pages(file->io,*metadata);
  PropertyCollectionRootStore properties(pages,values,*metadata);uint64_t lsn=2;
  std::vector<PropertyCollectionRoot> source;
  for(size_t row=0;row<count;++row) {
   PropertyCollectionRoot root{row*7+11,1U,{{1U,PropertyValue(static_cast<int64_t>(row))}}};
   for(uint32_t token=2;token<=fields;++token) {
    uint64_t state=(row+1)*1000003+token;std::string text(payload,' ');
    for(char &c:text){state^=state<<13;state^=state>>7;state^=state<<17;c=static_cast<char>('!'+state%90);}
    root.entries.push_back({token,PropertyValue(std::move(text))});
   }
   source.push_back(std::move(root));
  }
  std::vector<LargeValueRef> rowRefs;std::vector<NativeColumnImageStore::Saved> blocks;
  if(version=="row" || version=="row-full") for(const auto &root:source) rowRefs.push_back(values.write(EntityPropertyLargeValuePayloadCodec::encode(root.entries),options(),LogSequenceNumber(lsn++)).reference);
  else for(size_t i=0;i<count;i+=64) {
   auto image=TypedPropertyBlock::encode(i/64+1,std::span(source).subspan(i,(std::min<size_t>)(64,count-i)));
   auto proof=TypedPropertyBlock::verify(image,i/64+1,1U);
   blocks.push_back(version=="column-atomic"
    ? columns.writeAtomic(proof,options(),lsn,*metadata,properties)
    : columns.write(proof,options(),lsn));
  }
  auto before=metadata->openSnapshot();std::vector<uint32_t> tokens,all;
  for(uint32_t i=1;i<=fields;++i){all.push_back(i);if(i<=selected)tokens.push_back(i);}
  const auto read=[&](const MetadataSnapshotPin &snapshot,const std::vector<LargeValueRef> &refs,const std::vector<NativeColumnImageStore::Saved> &images,std::span<const uint32_t> requested) {
   std::vector<PropertyCollectionRoot> result;
   if(version=="row" || version=="row-full") for(size_t i=0;i<count;++i) {
    if(version=="row-full") {
     auto entries=EntityPropertyLargeValuePayloadCodec::decode(values.read(snapshot,refs[i]));
     std::erase_if(entries,[&](const auto &entry){return !std::binary_search(requested.begin(),requested.end(),entry.key_token);});
     result.push_back({source[i].owner_entity_id,1U,std::move(entries)});
    } else result.push_back(selectedOwner(values,snapshot,refs[i],source[i].owner_entity_id,requested));
   }
   else for(size_t i=0;i<images.size();++i) {
    auto rows=(version=="column-range" || version=="column-atomic")
     ? columns.readSelectedChunkChecked(snapshot,images[i].root(),i+1,1U,requested)
     : columns.readSelected(snapshot,images[i].root(),i+1,1U,requested);
    result.insert(result.end(),std::make_move_iterator(rows.begin()),std::make_move_iterator(rows.end()));
   }
   return result;
  };
  std::cout<<"{\"event\":\"fixture\",\"file_bytes\":"<<std::filesystem::file_size(file->path)<<"}\n";
  for(size_t iteration=0;iteration<13;++iteration) {
   file->io->resetStats();const auto start=Clock::now();auto result=read(before,rowRefs,blocks,tokens);
   const auto elapsed=std::chrono::duration<double,std::milli>(Clock::now()-start).count();const auto stats=file->io->stats();check(result,source,tokens);
   if(iteration>=3) std::cout<<"{\"event\":\"read\",\"milliseconds\":"<<elapsed<<",\"read_bytes\":"<<stats.read_bytes<<",\"checked\":"<<count<<"}\n";
  }
  const size_t target=count/2;const auto blockIndex=target/64;
  for(size_t iteration=0;iteration<13;++iteration) {
   auto expected=source;expected[target].entries[0].value=PropertyValue(-7001-static_cast<int64_t>(iteration));
   auto updatedRows=rowRefs;auto updatedBlocks=blocks;file->io->resetStats();const auto start=Clock::now();
   if(version=="row" || version=="row-full") updatedRows[target]=values.write(EntityPropertyLargeValuePayloadCodec::encode(expected[target].entries),options(),LogSequenceNumber(lsn++)).reference;
   else {
    const auto next=TypedPropertyBlock::replaceVerified(blocks[blockIndex].proof(),blockIndex+1,1U,expected[target].owner_entity_id,expected[target].entries);
    updatedBlocks[blockIndex]=version=="column-atomic"
     ? columns.writeAtomic(next,options(),lsn,*metadata,properties,&blocks[blockIndex])
     : columns.write(next,options(),lsn,&blocks[blockIndex]);
   }
   const auto elapsed=std::chrono::duration<double,std::milli>(Clock::now()-start).count();const auto stats=file->io->stats();auto current=metadata->openSnapshot();
   check(read(current,updatedRows,updatedBlocks,all),expected,all);check(read(before,rowRefs,blocks,all),source,all);
   if(iteration>=3) std::cout<<"{\"event\":\"update\",\"milliseconds\":"<<elapsed<<",\"write_bytes\":"<<stats.write_bytes<<",\"data_fences\":"<<stats.data_sync_calls<<",\"superblock_fences\":"<<stats.superblock_sync_calls<<",\"checked\":"<<count<<"}\n";
  }
  std::cout<<"{\"event\":\"final\",\"file_bytes\":"<<std::filesystem::file_size(file->path)<<"}\n";
#if defined(__APPLE__) || defined(__linux__)
  struct rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) == 0) {
   uint64_t peakBytes = static_cast<uint64_t>(usage.ru_maxrss);
#if defined(__linux__)
   peakBytes *= 1024U;
#endif
   std::cout << "{\"event\":\"peak_rss\",\"bytes\":" << peakBytes << "}\n";
  }
#endif
 } catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
}
