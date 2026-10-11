#pragma once
// Lab-only composition of existing native LargeValue objects. Not a graph backend.
#include "TypedPropertyBlock.hpp"
#include "PropertyBlockOwnerSlots.hpp"
#include "graph/storage/relocatable/property/LargeValueStore.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRootStore.hpp"
namespace zyx::benchmark {
using namespace graph::storage;
class NativeColumnImageStore final {
public:
 static constexpr uint32_t ROOT_MAGIC = 0x4350545aU;
 static constexpr const char *ROOT_MEDIA = "application/vnd.zyx.lab.property-block";
 static constexpr const char *COLUMN_MEDIA = "application/vnd.zyx.lab.property-column";
 class Saved final {
 public:
  const LargeValueRef &root() const { return root_; }
  const TypedPropertyBlock::VerifiedImage &proof() const { return proof_; }
  const std::vector<LargeValueRef> &columns() const { return columns_; }
  PropertyBlockOwnerSlots::Route route(uint64_t owner) const {if(!slots_)fail("legacy root has no owner slots");return slots_->route(owner);}
 private:
  friend class NativeColumnImageStore;
  Saved(std::shared_ptr<const uint8_t> store, LargeValueRef root, std::vector<LargeValueRef> columns,
        TypedPropertyBlock::VerifiedImage proof,std::optional<PropertyBlockOwnerSlots> slots={}) : store_(store), root_(std::move(root)), columns_(std::move(columns)), proof_(std::move(proof)),slots_(std::move(slots)) {}
  std::shared_ptr<const uint8_t> store_;
  LargeValueRef root_;
  std::vector<LargeValueRef> columns_;
  TypedPropertyBlock::VerifiedImage proof_;
  std::optional<PropertyBlockOwnerSlots> slots_;
 };
 explicit NativeColumnImageStore(LargeValueStore &native) : native_(native) {}
 NativeColumnImageStore(const NativeColumnImageStore &) = delete;
 NativeColumnImageStore &operator=(const NativeColumnImageStore &) = delete;
 // Each changed child and then the root use ordinary durable native writes.
 // This is NOT one atomic graph transaction; no graph owner route or retirement is installed.
 Saved write(const TypedPropertyBlock::VerifiedImage &proof, LargeValueWriteOptions options,
             uint64_t &nextLsn, const Saved *before = nullptr) {
  if (before && before->store_ != identity_) fail("foreign persisted property image");
  const auto &image = proof.image();
  std::vector<LargeValueRef> references;
  for (const auto &column : image.columns) {
   bool reused = false;
   if (before) {
    const auto &old = before->proof_.image().columns;
    const auto it = std::find(old.begin(), old.end(), column);
    if (it != old.end()) { references.push_back(before->columns_[it-old.begin()]); reused = true; }
   }
   if (!reused) {
    options.media_type = COLUMN_MEDIA; options.kind = LargeValueKind::LVK_BYTES;
    references.push_back(native_.write(*column, options, takeLsn(nextLsn)).reference);
   }
  }
  std::vector<std::byte> root(16);
  LittleEndianCodec::writeU32(root,0,ROOT_MAGIC); LittleEndianCodec::writeU16(root,4,1);
  LittleEndianCodec::writeU32(root,8,image.metadata.size()); LittleEndianCodec::writeU32(root,12,references.size());
  root.insert(root.end(),image.metadata.begin(),image.metadata.end());
  for (const auto &reference : references) {
   const auto wire = LargeValueRefCodec::encode(reference);
   const size_t offset = root.size(); root.resize(offset+4);
   LittleEndianCodec::writeU32(root,offset,wire.size()); root.insert(root.end(),wire.begin(),wire.end());
  }
  options.media_type = ROOT_MEDIA;
  auto reference = native_.write(root,options,takeLsn(nextLsn)).reference;
  return Saved(identity_,std::move(reference),std::move(references),proof);
 }
 // Lab-only unified native publication. Owner routing and retirement remain separate work.
 Saved writeAtomic(const TypedPropertyBlock::VerifiedImage &proof,const LargeValueWriteOptions &options,
                   uint64_t &nextLsn,RelocatableMetadataStore &metadata,PropertyCollectionRootStore &properties,
                   const Saved *before=nullptr,const MetadataPublishOptions &publishOptions={}) {
  if(!native_.usesMetadataStore(metadata) || !properties.usesMetadataStore(metadata) || properties.largeValueStore()!=&native_)
   fail("inconsistent native column publication stores");
  if(before && before->store_!=identity_) fail("foreign persisted property image");
  auto snapshot=metadata.openSnapshot();uint64_t nextId=snapshot.superblock().next_large_value_id;
  std::vector<NativeGraphLargeValueMutation> mutations;std::vector<LargeValueRef> references,reused;
  const auto plan=[&](std::span<const std::byte> bytes,const char *media) {
   if(nextId==0 || nextId==UINT64_MAX) fail("native column logical identity overflow");
   auto reference=native_.planReference(LargeValueId(nextId++),Generation(1),bytes,LargeValueKind::LVK_BYTES,
                                       media,options.compression,options.extent_class);
   mutations.push_back({reference,std::vector<std::byte>(bytes.begin(),bytes.end()),options.first_data_slot,
    options.last_data_slot,options.first_manifest_slot,options.last_manifest_slot,options.compression,
    options.extent_class,native_.plannedDataPageCount(bytes,options.compression,options.extent_class),
    native_.plannedManifestPageCount(bytes,options.compression,options.extent_class)});
   return reference;
  };
  const auto &image=proof.image();
  for(const auto &column:image.columns) {
   bool shared=false;
   if(before) {
    const auto &old=before->proof_.image().columns;const auto found=std::find(old.begin(),old.end(),column);
    if(found!=old.end()) {references.push_back(before->columns_[found-old.begin()]);reused.push_back(references.back());shared=true;}
   }
   if(!shared) references.push_back(plan(*column,COLUMN_MEDIA));
  }
  const uint64_t block=LittleEndianCodec::readU64(image.metadata,16);
  const uint32_t type=LittleEndianCodec::readU32(image.metadata,12);
  auto owners=TypedPropertyBlock::decode(image,block,type,{});
  auto slots=(before && before->slots_
   ? PropertyBlockOwnerSlots::decode(before->slots_->encode(),block,type)
   : PropertyBlockOwnerSlots(block,type)).reconcile(owners);
  std::vector<std::byte> root(16);LittleEndianCodec::writeU32(root,0,ROOT_MAGIC);LittleEndianCodec::writeU16(root,4,2);
  LittleEndianCodec::writeU32(root,8,image.metadata.size());LittleEndianCodec::writeU32(root,12,references.size());
  root.insert(root.end(),image.metadata.begin(),image.metadata.end());
  const auto slotBytes=slots.encode();root.insert(root.end(),slotBytes.begin(),slotBytes.end());
  for(const auto &ref:references) {const auto wire=LargeValueRefCodec::encode(ref);const size_t offset=root.size();
   root.resize(offset+4);LittleEndianCodec::writeU32(root,offset,wire.size());root.insert(root.end(),wire.begin(),wire.end());}
  const auto rootReference=plan(root,ROOT_MEDIA);
  auto content=native_.planNativeGraphContentIndexMutation(snapshot,mutations,options.first_manifest_slot,options.last_manifest_slot);
  NativeGraphDerivedStateMutation derived;derived.index_tree_mutations.push_back(content.mutation);
  std::sort(reused.begin(),reused.end(),[](const auto &a,const auto &b){return a.value_id.value()<b.value_id.value();});
  reused.erase(std::unique(reused.begin(),reused.end()),reused.end());derived.reused_large_values=std::move(reused);
  const auto lsn=takeLsn(nextLsn);
  static_cast<void>(metadata.mutateNativeGraphPagesWithLargeValues({}, {}, {}, {}, {}, mutations,derived,lsn,
   [&](auto,auto,auto,auto,auto,std::span<LargeValueMaterializationResult> materializations) {
    if(materializations.size()!=mutations.size()) fail("native column publication materialization count");
    for(size_t i=0;i<mutations.size();++i) properties.preparePlannedLargeValue(mutations[i],materializations[i],lsn);
   },[&](auto &batches) {if(batches.size()!=1) fail("native column content-index materialization count");
     content.preparer(batches.front(),lsn);}, {}, {}, {}, {}, {},publishOptions,{},snapshot.superblock().generation));
  return Saved(identity_,rootReference,std::move(references),proof,std::move(slots));
 }
 std::vector<PropertyCollectionRoot> readSelected(const MetadataSnapshotPin &snapshot, const LargeValueRef &root,
                                                uint64_t block, uint32_t type, std::span<const uint32_t> tokens) const {
  return readSelectedImpl(snapshot,root,block,type,tokens,false);
 }
 // Lab attribution entry: same authenticated manifest/chunk and typed column CRC checks,
 // without additionally computing whole-column SHA256. Full-read API above is unchanged.
 std::vector<PropertyCollectionRoot> readSelectedChunkChecked(const MetadataSnapshotPin &snapshot, const LargeValueRef &root,
                                                uint64_t block, uint32_t type, std::span<const uint32_t> tokens) const {
  return readSelectedImpl(snapshot,root,block,type,tokens,true);
 }
 PropertyCollectionRoot readOwner(const MetadataSnapshotPin &snapshot,const LargeValueRef &root,
                                  const PropertyBlockOwnerSlots::Route &route,std::span<const uint32_t> tokens) const {
  auto loaded=readRoot(snapshot,root,route.block,route.type);
  if(!loaded.slots)fail("legacy root has no owner slots");
  const auto owner=loaded.slots->resolve(route);
  auto rows=readSelected(snapshot,root,route.block,route.type,tokens);
  auto found=std::lower_bound(rows.begin(),rows.end(),owner,[](const auto &row,uint64_t id){return row.owner_entity_id<id;});
  if(found==rows.end() || found->owner_entity_id!=owner)fail("property owner route root mismatch");
  return *found;
 }
private:
 std::vector<PropertyCollectionRoot> readSelectedImpl(const MetadataSnapshotPin &snapshot,const LargeValueRef &root,
                                                uint64_t block,uint32_t type,std::span<const uint32_t> tokens,bool chunkChecked) const {
  uint32_t previous=0;
  for (auto token : tokens) { if(token<=previous) fail("selected tokens must be sorted/unique"); previous=token; }
  auto loaded = readRoot(snapshot,root,block,type);
  for (size_t column=0;column<loaded.references.size();++column) {
   const size_t rows=LittleEndianCodec::readU16(loaded.image.metadata,6);
   const uint32_t token=LittleEndianCodec::readU32(loaded.image.metadata,TypedPropertyBlock::HEADER+rows*8+column*TypedPropertyBlock::DESCRIPTOR);
   if (std::binary_search(tokens.begin(),tokens.end(),token))
    loaded.image.columns[column]=std::make_shared<const std::vector<std::byte>>(chunkChecked
     ? native_.readRange(snapshot,loaded.references[column],0,loaded.references[column].logical_bytes)
     : native_.read(snapshot,loaded.references[column]));
  }
  return TypedPropertyBlock::decode(loaded.image,block,type,tokens);
 }
public:
 Saved load(const MetadataSnapshotPin &snapshot,const LargeValueRef &root,uint64_t block,uint32_t type) const {
  auto loaded=readRoot(snapshot,root,block,type);
  for(size_t i=0;i<loaded.references.size();++i)
   loaded.image.columns[i]=std::make_shared<const std::vector<std::byte>>(native_.read(snapshot,loaded.references[i]));
  return Saved(identity_,root,std::move(loaded.references),TypedPropertyBlock::verify(loaded.image,block,type),std::move(loaded.slots));
 }
private:
 struct Loaded { TypedPropertyBlock::Image image; std::vector<LargeValueRef> references;std::optional<PropertyBlockOwnerSlots> slots; };
 Loaded readRoot(const MetadataSnapshotPin &snapshot,const LargeValueRef &reference,uint64_t block,uint32_t type) const {
  if(reference.kind!=LargeValueKind::LVK_BYTES || reference.media_type!=ROOT_MEDIA || reference.logical_bytes>2U*1024U*1024U)
   fail("invalid native property block reference");
  const auto bytes=native_.read(snapshot,reference);
  if(bytes.size()<16 || LittleEndianCodec::readU32(bytes,0)!=ROOT_MAGIC || (LittleEndianCodec::readU16(bytes,4)!=1 && LittleEndianCodec::readU16(bytes,4)!=2) || LittleEndianCodec::readU16(bytes,6)!=0)
   fail("invalid native property block root header");
  const size_t metadataBytes=LittleEndianCodec::readU32(bytes,8),count=LittleEndianCodec::readU32(bytes,12);
  if(count>4096 || metadataBytes>TypedPropertyBlock::HEADER+64*8+4096*TypedPropertyBlock::DESCRIPTOR || metadataBytes>bytes.size()-16)
   fail("invalid native property block root size");
  Loaded loaded;
  loaded.image.metadata.assign(bytes.begin()+16,bytes.begin()+16+metadataBytes);
  loaded.image.columns.resize(count);
  const auto owners=TypedPropertyBlock::decode(loaded.image,block,type,{});
  const size_t rows=LittleEndianCodec::readU16(loaded.image.metadata,6);
  size_t cursor=16+metadataBytes;
  if(LittleEndianCodec::readU16(bytes,4)==2) {
   if(bytes.size()-cursor<PropertyBlockOwnerSlots::WIRE_BYTES)fail("truncated property owner slots");
   loaded.slots=PropertyBlockOwnerSlots::decode(std::span(bytes).subspan(cursor,PropertyBlockOwnerSlots::WIRE_BYTES),block,type);
   loaded.slots->validateOwners(owners);
   cursor+=PropertyBlockOwnerSlots::WIRE_BYTES;
  }
  for(size_t i=0;i<count;++i) {
   if(bytes.size()-cursor<4) fail("truncated native column reference");
   const size_t size=LittleEndianCodec::readU32(bytes,cursor);cursor+=4;
   if(size>RSS_LARGE_VALUE_REF_HEADER_BYTES+RSS_LARGE_VALUE_MAX_MEDIA_TYPE_BYTES || size>bytes.size()-cursor)
    fail("invalid native column reference size");
   auto child=LargeValueRefCodec::decode(std::span(bytes).subspan(cursor,size));cursor+=size;
   const auto declared=LittleEndianCodec::readU64(loaded.image.metadata,TypedPropertyBlock::HEADER+rows*8+i*TypedPropertyBlock::DESCRIPTOR+24);
   if(child.logical_bytes!=declared || child.kind!=LargeValueKind::LVK_BYTES || child.media_type!=COLUMN_MEDIA)
    fail("native column reference disagrees with property metadata");
   loaded.references.push_back(std::move(child));
  }
  if(cursor!=bytes.size()) fail("native property root trailing bytes");
  return loaded;
 }
 static LogSequenceNumber takeLsn(uint64_t &next) {
  if(next==0 || next==UINT64_MAX) fail("native property LSN overflow");
  return LogSequenceNumber(next++);
 }
 [[noreturn]] static void fail(const char *message) { throw std::runtime_error(message); }
 std::shared_ptr<const uint8_t> identity_ = std::make_shared<const uint8_t>(0);
 LargeValueStore &native_;
};
}
