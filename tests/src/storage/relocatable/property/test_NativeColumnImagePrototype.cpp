#include "storage/relocatable/metadata/RelocatableMetadataStoreTestFixture.hpp"
#include "benchmarks/graphdb-comparison/property-block-prototype/NativeColumnImageStore.hpp"
using zyx::benchmark::NativeColumnImageStore;
using zyx::benchmark::TypedPropertyBlock;
using graph::PropertyValue;
namespace {
LargeValueWriteOptions writeOptions(LargeValueCodec codec) {
 LargeValueWriteOptions o; o.first_data_slot=PhysicalSlotId(40);o.last_data_slot=PhysicalSlotId(140);
 o.first_manifest_slot=PhysicalSlotId(141);o.last_manifest_slot=PhysicalSlotId(170);
 o.compression.codec=codec;o.compression.allow_raw_fallback=true;return o;
}
std::vector<PropertyCollectionRoot> input() {
 return {{7U,1U,{{1U,PropertyValue(15)},{2U,PropertyValue(std::string(8192,'a'))}}},
         {23U,1U,{{1U,PropertyValue()},{2U,PropertyValue(std::string(8192,'b'))}}}};
}
}
TEST_F(RelocatableMetadataStoreTest, NativeColumnPrototypePersistsCompressedColumnsCowAndReopens) {
 LargeValueRef lastRoot;std::vector<PropertyCollectionRoot> expectedLast;
 auto metadata=RelocatableMetadataStore::initializeNew(io,superblock(1,1));
 {
  LargeValueStore values(io,*metadata);NativeColumnImageStore store(values);uint64_t lsn=2;
  for(auto codec:{LargeValueCodec::LVC_RAW,LargeValueCodec::LVC_LZ4,LargeValueCodec::LVC_ZSTD}) {
   auto source=input();source[0].entries[0].value=PropertyValue(static_cast<int64_t>(codec));
   source[0].entries[1].value=PropertyValue(std::string(8192,static_cast<char>('a'+static_cast<int>(codec))));
   source[1].entries[1].value=PropertyValue(std::string(8192,static_cast<char>('b'+static_cast<int>(codec))));
   auto proof=TypedPropertyBlock::verify(TypedPropertyBlock::encode(11U,source),11U,1U);
   auto before=store.write(proof,writeOptions(codec),lsn);auto oldPin=metadata->openSnapshot();
   EXPECT_EQ(before.columns()[1].codec,codec);
   const std::vector<uint32_t> all{1U,2U},narrow{1U};
   EXPECT_EQ(store.readSelected(oldPin,before.root(),11U,1U,all),source);
   auto expected=source;expected[0].entries[0].value=PropertyValue(97+static_cast<int>(codec));
   const auto changed=TypedPropertyBlock::replaceVerified(before.proof(),11U,1U,7U,expected[0].entries);
   auto after=store.write(changed,writeOptions(codec),lsn,&before);auto current=metadata->openSnapshot();
   ASSERT_EQ(after.columns().size(),2U);EXPECT_EQ(after.columns()[1],before.columns()[1]);
   EXPECT_NE(after.columns()[0],before.columns()[0]);
   EXPECT_EQ(store.readSelected(current,after.root(),11U,1U,all),expected);
   EXPECT_EQ(store.readSelectedChunkChecked(current,after.root(),11U,1U,all),expected);
   EXPECT_EQ(store.readSelectedChunkChecked(oldPin,before.root(),11U,1U,all),source);
   EXPECT_EQ(store.readSelected(oldPin,before.root(),11U,1U,all),source);
   auto projection=expected;for(auto &owner:projection)owner.entries.resize(1);
   EXPECT_EQ(store.readSelected(current,after.root(),11U,1U,narrow),projection);
   EXPECT_EQ(store.readSelectedChunkChecked(current,after.root(),11U,1U,narrow),projection);
   EXPECT_THROW(static_cast<void>(store.readSelectedChunkChecked(oldPin,after.root(),11U,1U,all)),FormatException);
   EXPECT_THROW(static_cast<void>(store.readSelected(oldPin,after.root(),11U,1U,all)),FormatException);
   EXPECT_THROW(static_cast<void>(store.readSelected(current,after.root(),12U,1U,all)),std::runtime_error);
   auto loaded=store.load(current,after.root(),11U,1U);
   EXPECT_EQ(TypedPropertyBlock::decode(loaded.proof().image(),11U,1U,all),expected);
   NativeColumnImageStore foreign(values);
   EXPECT_THROW(static_cast<void>(foreign.write(changed,writeOptions(codec),lsn,&before)),std::runtime_error);
   lastRoot=after.root();expectedLast=expected;
  }
 }
 metadata.reset();metadata=RelocatableMetadataStore::open(io);
 LargeValueStore values(io,*metadata);NativeColumnImageStore store(values);auto current=metadata->openSnapshot();
 const auto &expected=expectedLast;const std::vector<uint32_t> all{1U,2U};
 EXPECT_EQ(store.readSelected(current,lastRoot,11U,1U,all),expected);
 EXPECT_EQ(store.readSelectedChunkChecked(current,lastRoot,11U,1U,all),expected);
 current.release();
}
TEST_F(RelocatableMetadataStoreTest, NativeColumnPrototypeRejectsChecksumValidRootAndReferenceMismatch) {
 auto metadata=RelocatableMetadataStore::initializeNew(io,superblock(1,1));
 LargeValueStore values(io,*metadata);NativeColumnImageStore store(values);uint64_t lsn=2;
 const auto proof=TypedPropertyBlock::verify(TypedPropertyBlock::encode(11U,input()),11U,1U);
 const auto saved=store.write(proof,writeOptions(LargeValueCodec::LVC_RAW),lsn);
 auto snapshot=metadata->openSnapshot();const auto bytes=values.read(snapshot,saved.root());
 const std::vector<uint32_t> selected{1U},unsorted{2U,1U};
 EXPECT_THROW(static_cast<void>(store.readSelected(snapshot,saved.root(),11U,1U,unsorted)),std::runtime_error);
 for(int mode=0;mode<3;++mode) {
  auto bad=bytes;
  if(mode==0) LittleEndianCodec::writeU16(bad,4U,2U);
  if(mode==1) LittleEndianCodec::writeU32(bad,12U,3U);
  if(mode==2) {
   const size_t offset=16U+LittleEndianCodec::readU32(bad,8U);
   const size_t size=LittleEndianCodec::readU32(bad,offset);
   auto child=LargeValueRefCodec::decode(std::span(bad).subspan(offset+4U,size));
   ++child.logical_bytes;const auto wire=LargeValueRefCodec::encode(child);
   ASSERT_EQ(wire.size(),size);std::copy(wire.begin(),wire.end(),bad.begin()+offset+4U);
  }
  auto options=writeOptions(LargeValueCodec::LVC_RAW);options.media_type=NativeColumnImageStore::ROOT_MEDIA;
  const auto corrupt=values.write(bad,options,LogSequenceNumber(lsn++)).reference;
  auto current=metadata->openSnapshot();
  EXPECT_THROW(static_cast<void>(store.readSelected(current,corrupt,11U,1U,selected)),std::runtime_error);
  EXPECT_THROW(static_cast<void>(store.readSelectedChunkChecked(current,corrupt,11U,1U,selected)),std::runtime_error);
 }
 EXPECT_EQ(store.readSelected(snapshot,saved.root(),11U,1U,std::vector<uint32_t>{1U,2U}),input());
 snapshot.release();
}

TEST_F(RelocatableMetadataStoreTest, NativeColumnPrototypeAtomicPublicationPreservesOldSnapshotAndReopens) {
 auto metadata=RelocatableMetadataStore::initializeNew(io,superblock(1,1));
 LargeValueRef finalRoot;const std::vector<uint32_t> all{1,2};auto expected=input();
 {
  LargeValueStore values(io,*metadata);SlottedPagePool pages(io,*metadata);PropertyCollectionRootStore properties(pages,values,*metadata);
  NativeColumnImageStore store(values);uint64_t lsn=2;
  auto proof=TypedPropertyBlock::verify(TypedPropertyBlock::encode(11,input()),11,1);
  auto initial=store.writeAtomic(proof,writeOptions(LargeValueCodec::LVC_LZ4),lsn,*metadata,properties);
  auto old=metadata->openSnapshot();const auto generation=old.superblock().generation;
  expected[0].entries[0].value=PropertyValue(91);
  auto changed=TypedPropertyBlock::replaceVerified(initial.proof(),11,1,7,expected[0].entries);
  for(auto failpoint:{BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE,BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE}) {
   MetadataPublishOptions publish;publish.bootstrap_failpoint=failpoint;
   EXPECT_THROW(static_cast<void>(store.writeAtomic(changed,writeOptions(LargeValueCodec::LVC_LZ4),lsn,*metadata,properties,&initial,publish)),BootstrapPublishInterrupted);
   auto unchanged=metadata->openSnapshot();EXPECT_EQ(unchanged.superblock().generation,generation);
   EXPECT_EQ(store.readSelected(unchanged,initial.root(),11,1,all),input());
   auto recovered=RelocatableMetadataStore::open(io);LargeValueStore recoveredValues(io,*recovered);
   NativeColumnImageStore recoveredStore(recoveredValues);auto recoveredPin=recovered->openSnapshot();
   EXPECT_EQ(recoveredPin.superblock().generation,generation);
   EXPECT_EQ(recoveredStore.readSelected(recoveredPin,initial.root(),11,1,all),input());
  }
  io->resetStats();auto updated=store.writeAtomic(changed,writeOptions(LargeValueCodec::LVC_LZ4),lsn,*metadata,properties,&initial);
  auto current=metadata->openSnapshot();
  EXPECT_EQ(current.superblock().generation.value(),generation.value()+1);
  EXPECT_EQ(io->stats().data_sync_calls,1U);EXPECT_EQ(io->stats().superblock_sync_calls,1U);
  EXPECT_EQ(updated.columns()[1],initial.columns()[1]);
  EXPECT_EQ(store.readSelected(current,updated.root(),11,1,all),expected);
  EXPECT_EQ(store.readSelected(old,initial.root(),11,1,all),input());
  EXPECT_THROW(static_cast<void>(store.readSelected(old,updated.root(),11,1,all)),FormatException);
  finalRoot=updated.root();
 }
 metadata.reset();metadata=RelocatableMetadataStore::open(io);LargeValueStore values(io,*metadata);
 NativeColumnImageStore store(values);auto current=metadata->openSnapshot();
 EXPECT_EQ(store.readSelected(current,finalRoot,11,1,all),expected);
}
TEST_F(RelocatableMetadataStoreTest, NativeColumnPrototypePersistsOwnerDeletionAndSlotReuseWithSnapshotBinding) {
 auto metadata=RelocatableMetadataStore::initializeNew(io,superblock(1,1));
 LargeValueRef finalRoot;zyx::benchmark::PropertyBlockOwnerSlots::Route finalRoute{};
 auto expected=input();expected.erase(expected.begin());expected.push_back({99,1,{{1,PropertyValue(42)}}});
 {
  LargeValueStore values(io,*metadata);SlottedPagePool pages(io,*metadata);PropertyCollectionRootStore properties(pages,values,*metadata);
  NativeColumnImageStore store(values);uint64_t lsn=2;const std::vector<uint32_t> all{1,2};
  auto first=store.writeAtomic(TypedPropertyBlock::verify(TypedPropertyBlock::encode(11,input()),11,1),writeOptions(LargeValueCodec::LVC_LZ4),lsn,*metadata,properties);
  auto old=metadata->openSnapshot();const auto removed=first.route(7),sibling=first.route(23);
  auto next=store.writeAtomic(TypedPropertyBlock::verify(TypedPropertyBlock::encode(11,expected),11,1),writeOptions(LargeValueCodec::LVC_LZ4),lsn,*metadata,properties,&first);
  auto current=metadata->openSnapshot();auto reused=next.route(99);
  EXPECT_EQ(reused.slot,removed.slot);EXPECT_EQ(reused.generation,removed.generation+1);
  EXPECT_EQ(next.route(23),sibling);
  EXPECT_EQ(store.readOwner(old,first.root(),removed,all),input()[0]);
  EXPECT_THROW(static_cast<void>(store.readOwner(current,next.root(),removed,all)),std::runtime_error);
  EXPECT_EQ(store.readOwner(current,next.root(),sibling,all),expected[0]);
  EXPECT_EQ(store.readOwner(current,next.root(),reused,all),expected[1]);
  EXPECT_THROW(static_cast<void>(store.readOwner(old,next.root(),reused,all)),FormatException);
  auto loaded=store.load(current,next.root(),11,1);EXPECT_EQ(loaded.route(99),reused);
  finalRoot=next.root();finalRoute=reused;
 }
 metadata.reset();metadata=RelocatableMetadataStore::open(io);LargeValueStore values(io,*metadata);
 NativeColumnImageStore store(values);auto current=metadata->openSnapshot();
 EXPECT_EQ(store.readOwner(current,finalRoot,finalRoute,std::vector<uint32_t>{1,2}),expected[1]);
}
TEST_F(RelocatableMetadataStoreTest, NativeColumnPrototypeRejectsChecksumValidOwnerSlotBindingMismatch) {
 auto metadata=RelocatableMetadataStore::initializeNew(io,superblock(1,1));
 LargeValueStore values(io,*metadata);SlottedPagePool pages(io,*metadata);PropertyCollectionRootStore properties(pages,values,*metadata);
 NativeColumnImageStore store(values);uint64_t lsn=2;
 const auto proof=TypedPropertyBlock::verify(TypedPropertyBlock::encode(11,input()),11,1);
 const auto saved=store.writeAtomic(proof,writeOptions(LargeValueCodec::LVC_RAW),lsn,*metadata,properties);
 auto original=metadata->openSnapshot();const auto good=values.read(original,saved.root());
 const size_t offset=16+LittleEndianCodec::readU32(good,8);
 const auto route=saved.route(7);const std::vector<uint32_t> selected{1};
 for(int mode=0;mode<4;++mode) {
  auto bad=good;
  if(mode==0)LittleEndianCodec::writeU64(bad,offset+32,99);
  if(mode==1)LittleEndianCodec::writeU64(bad,offset+8,12);
  if(mode==2)LittleEndianCodec::writeU64(bad,offset+40,0);
  if(mode==3)LittleEndianCodec::writeU32(bad,offset+16,2);
  LittleEndianCodec::writeU32(bad,offset+24,0);
  LittleEndianCodec::writeU32(bad,offset+24,graph::utils::calculateCrc(bad.data()+offset,zyx::benchmark::PropertyBlockOwnerSlots::WIRE_BYTES));
  auto options=writeOptions(LargeValueCodec::LVC_RAW);options.media_type=NativeColumnImageStore::ROOT_MEDIA;
  const auto corrupted=values.write(bad,options,LogSequenceNumber(lsn++)).reference;
  auto current=metadata->openSnapshot();
  EXPECT_THROW(static_cast<void>(store.readSelected(current,corrupted,11,1,selected)),std::runtime_error);
  EXPECT_THROW(static_cast<void>(store.readSelectedChunkChecked(current,corrupted,11,1,selected)),std::runtime_error);
  EXPECT_THROW(static_cast<void>(store.readOwner(current,corrupted,route,selected)),std::runtime_error);
  EXPECT_THROW(static_cast<void>(store.load(current,corrupted,11,1)),std::runtime_error);
 }
 EXPECT_EQ(store.readOwner(original,saved.root(),route,selected).entries[0],input()[0].entries[0]);
 const auto wrongBlock=TypedPropertyBlock::verify(TypedPropertyBlock::encode(12,input()),12,1);
 auto before=metadata->openSnapshot();const auto generation=before.superblock().generation;
 EXPECT_THROW(static_cast<void>(store.writeAtomic(wrongBlock,writeOptions(LargeValueCodec::LVC_RAW),lsn,*metadata,properties,&saved)),std::runtime_error);
 auto after=metadata->openSnapshot();EXPECT_EQ(after.superblock().generation,generation);
}
TEST_F(RelocatableMetadataStoreTest, NativeColumnPrototypeLastOwnerDeletionRetainsSlotGenerationAcrossReopen) {
 auto metadata=RelocatableMetadataStore::initializeNew(io,superblock(1,1));LargeValueRef emptyRoot;
 zyx::benchmark::PropertyBlockOwnerSlots::Route originalRoute{};
 {
  LargeValueStore values(io,*metadata);SlottedPagePool pages(io,*metadata);PropertyCollectionRootStore properties(pages,values,*metadata);
  NativeColumnImageStore store(values);uint64_t lsn=2;
  const std::vector<PropertyCollectionRoot> single{{7,1,{{1,PropertyValue(15)}}}};
  const auto initial=store.writeAtomic(TypedPropertyBlock::verify(TypedPropertyBlock::encode(11,single),11,1),writeOptions(LargeValueCodec::LVC_RAW),lsn,*metadata,properties);
  auto old=metadata->openSnapshot();originalRoute=initial.route(7);
  const auto empty=store.writeAtomic(TypedPropertyBlock::verify(TypedPropertyBlock::encodeEmpty(11,1),11,1),writeOptions(LargeValueCodec::LVC_RAW),lsn,*metadata,properties,&initial);
  auto current=metadata->openSnapshot();const std::vector<uint32_t> tokens{1};
  EXPECT_TRUE(store.readSelected(current,empty.root(),11,1,tokens).empty());
  EXPECT_THROW(static_cast<void>(store.readOwner(current,empty.root(),originalRoute,tokens)),std::runtime_error);
  EXPECT_EQ(store.readOwner(old,initial.root(),originalRoute,tokens),single[0]);
  emptyRoot=empty.root();
 }
 metadata.reset();metadata=RelocatableMetadataStore::open(io);LargeValueStore values(io,*metadata);
 SlottedPagePool pages(io,*metadata);PropertyCollectionRootStore properties(pages,values,*metadata);NativeColumnImageStore store(values);
 auto current=metadata->openSnapshot();auto empty=store.load(current,emptyRoot,11,1);uint64_t lsn=current.superblock().checkpoint_lsn.value()+1;
 const std::vector<PropertyCollectionRoot> single{{99,1,{{1,PropertyValue(42)}}}};
 const auto inserted=store.writeAtomic(TypedPropertyBlock::verify(TypedPropertyBlock::encode(11,single),11,1),writeOptions(LargeValueCodec::LVC_RAW),lsn,*metadata,properties,&empty);
 auto next=metadata->openSnapshot();const auto route=inserted.route(99);
 EXPECT_EQ(route.slot,originalRoute.slot);EXPECT_EQ(route.generation,originalRoute.generation+1);
 EXPECT_THROW(static_cast<void>(store.readOwner(next,inserted.root(),originalRoute,std::vector<uint32_t>{1})),std::runtime_error);
 EXPECT_EQ(store.readOwner(next,inserted.root(),route,std::vector<uint32_t>{1}),single[0]);
}
