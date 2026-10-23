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

namespace {
PropertyValue ownBorrowedCell(const TypedPropertyBlock::BorrowedCell &cell) {
 if (!cell.present) throw std::logic_error("missing cell has no value");
 if (cell.primitive) {
  const auto &p = *cell.primitive;
  switch (p.type) {
   case graph::PropertyType::NULL_TYPE: return PropertyValue{};
   case graph::PropertyType::INTEGER: return PropertyValue(std::bit_cast<int64_t>(p.bits));
   case graph::PropertyType::DOUBLE: return PropertyValue(std::bit_cast<double>(p.bits));
   case graph::PropertyType::BOOLEAN: return PropertyValue(bool(p.bits));
   case graph::PropertyType::STRING: return PropertyValue(std::string(p.text));
   default: throw std::logic_error("unexpected borrowed primitive");
  }
 }
 size_t consumed = 0;
 auto result = CanonicalPropertyValueCodec::decodeNext(cell.complex, consumed);
 EXPECT_EQ(consumed, cell.complex.size());
 return result;
}
std::vector<PropertyCollectionRoot> borrowedRows(std::span<const TypedPropertyBlock::BorrowedCell> cells, uint32_t token) {
 std::vector<PropertyCollectionRoot> result;
 for (const auto &cell : cells) {
  PropertyCollectionRoot row{cell.owner, 1U, {}};
  if (cell.present) row.entries.push_back({token, ownBorrowedCell(cell)});
  result.push_back(std::move(row));
 }
 return result;
}
}

TEST_F(RelocatableMetadataStoreTest, NativeColumnBorrowedSupplyMatchesOwningAcrossCowPinsAndReopen) {
 auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1,1));
 LargeValueRef finalRoot;
 std::vector<PropertyCollectionRoot> source;
 for (size_t i = 0; i < 64; ++i) {
  PropertyCollectionRoot row{uint64_t(i * 3 + 1), 1U, {{1U, PropertyValue(int64_t(i)-32)}}};
  if (i % 5 == 0) row.entries.push_back({2U, PropertyValue{}});
  else if (i % 5 != 1) row.entries.push_back({2U, PropertyValue(std::string(i % 2 ? "a\0b" : "", i % 2 ? 3 : 0))});
  if (i % 3) row.entries.push_back({3U, PropertyValue(i % 2 == 0)});
  if(i==0)row.entries.push_back({4U,PropertyValue(std::vector<PropertyValue>{PropertyValue(7),PropertyValue("nested")})});
  else if(i==63)row.entries.push_back({4U,PropertyValue(std::string(9000,'v'))});
  else row.entries.push_back({4U,PropertyValue(int64_t(i))});
  source.push_back(std::move(row));
 }
 {
  LargeValueStore values(io,*metadata); NativeColumnImageStore store(values); uint64_t lsn=2;
  auto before = store.write(TypedPropertyBlock::verify(TypedPropertyBlock::encode(17U,source),17U,1U),
                            writeOptions(LargeValueCodec::LVC_RAW), lsn);
  auto old = metadata->openSnapshot();
  const auto read = [&](const MetadataSnapshotPin &pin, const LargeValueRef &root, uint32_t token) {
   std::vector<PropertyCollectionRoot> result;
   store.visitSelectedColumn(pin,root,17U,1U,token,[&](auto cells){result=borrowedRows(cells,token);});
   return result;
  };
  for (uint32_t token : {1U,2U,3U,4U,99U})
   EXPECT_EQ(read(old,before.root(),token),store.readSelected(old,before.root(),17U,1U,std::array{token}));
  auto changed = source; changed[1].entries.insert(changed[1].entries.begin()+1,{2U,PropertyValue(std::string(600,'x'))});
  auto proof = TypedPropertyBlock::replaceVerified(before.proof(),17U,1U,changed[1].owner_entity_id,changed[1].entries);
  auto after = store.write(proof,writeOptions(LargeValueCodec::LVC_RAW),lsn,&before);
  auto current = metadata->openSnapshot();
  EXPECT_EQ(read(old,before.root(),2U),store.readSelected(old,before.root(),17U,1U,std::array{2U}));
  EXPECT_EQ(read(current,after.root(),2U),store.readSelected(current,after.root(),17U,1U,std::array{2U}));
  EXPECT_THROW(static_cast<void>(read(old,after.root(),2U)),FormatException);
  size_t callbacks=0;
  EXPECT_THROW(store.visitSelectedColumn(current,after.root(),18U,1U,2U,[&](auto){++callbacks;}),std::runtime_error);
  EXPECT_EQ(callbacks,0U);
  finalRoot=after.root();
 }
 metadata.reset();metadata=RelocatableMetadataStore::open(io);
 LargeValueStore values(io,*metadata);NativeColumnImageStore store(values);auto reopened=metadata->openSnapshot();
 std::vector<PropertyCollectionRoot> observed;
 store.visitSelectedColumn(reopened,finalRoot,17U,1U,2U,[&](auto cells){observed=borrowedRows(cells,2U);});
 EXPECT_EQ(observed,store.readSelected(reopened,finalRoot,17U,1U,std::array{2U}));
}

TEST_F(RelocatableMetadataStoreTest, BorrowedColumnValidatesEntireSelectedPayloadBeforeAnyOutput) {
 std::vector<PropertyCollectionRoot> rows;
 for (uint64_t i=1;i<=64;++i) rows.push_back({i,1U,{{1U,PropertyValue(true)}}});
 auto image=TypedPropertyBlock::encode(9U,rows);
 auto payload=std::make_shared<std::vector<std::byte>>(*image.columns.front());
 payload->back()=std::byte{2};image.columns.front()=payload;
 const size_t descriptor=TypedPropertyBlock::HEADER+64U*8U;
 LittleEndianCodec::writeU32(image.metadata,descriptor+32U,graph::utils::calculateCrc(payload->data(),payload->size()));
 LittleEndianCodec::writeU32(image.metadata,24U,0U);
 LittleEndianCodec::writeU32(image.metadata,24U,graph::utils::calculateCrc(image.metadata.data(),image.metadata.size()));
 size_t callbacks=0;
 EXPECT_THROW(TypedPropertyBlock::visitSelectedColumn(image,9U,1U,1U,[&](auto){++callbacks;}),std::runtime_error);
 EXPECT_EQ(callbacks,0U);
 // An untouched corrupt payload is outside selected-column validation.
 TypedPropertyBlock::visitSelectedColumn(image,9U,1U,2U,[&](auto cells){
  ++callbacks;EXPECT_EQ(cells.size(),64U);for(const auto &cell:cells) EXPECT_FALSE(cell.present);
 });
 EXPECT_EQ(callbacks,1U);
 image=TypedPropertyBlock::encode(9U,rows);size_t checks=0;callbacks=0;
 EXPECT_THROW(TypedPropertyBlock::visitSelectedColumn(image,9U,1U,1U,[&](auto){++callbacks;},[&]{
  if(++checks==20)throw std::runtime_error("cancelled");
 }),std::runtime_error);
 EXPECT_EQ(callbacks,0U);
}

#include "storage/relocatable/property/NativeColumnSupplyTestSnapshot.hpp"
#include "query/execution/DirectPropertyColumns.hpp"
#include "graph/storage/relocatable/entity/EntityPageStore.hpp"
#include "graph/storage/relocatable/maintenance/NativeStorageAllocationCensus.hpp"
#include "graph/query/execution/LogicalPlanReadRuntime.hpp"
#include "graph/query/logical/operators/LogicalAggregate.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/expressions/Expression.hpp"

namespace {
void runNativeSupplyExperiment(const std::shared_ptr<StorageIO> &io, const Superblock &initial, bool sparse) {
 using namespace graph::query;
 using namespace graph::query::execution;
 auto metadata=RelocatableMetadataStore::initializeNew(io,initial);
 LargeValueStore values(io,*metadata);SlottedPagePool pages(io,*metadata);
 PropertyCollectionRootStore properties(pages,values,*metadata);EntityPageStore entities(io,*metadata);
 std::vector<PropertyCollectionRoot> inputRows;std::vector<uint64_t> ids;
 EntityPageBatchNewPool nodePool;
 nodePool.request={SegmentId(31U),901U,Generation(1U),PageId(101U),PhysicalSlotId(40U),PhysicalSlotId(40U)};
 nodePool.record_kind=EntityPageRecordKind::EPRK_NODE;
 for(size_t i=0;i<64;++i) {
  const uint64_t id=i*3+1;ids.push_back(id);nodePool.node_insertions.push_back({id,NodeEntityRecord{}});
  PropertyCollectionRoot row{id,graph::Node::typeId,{}};
  if(sparse && i%16U==1U)row.entries.push_back({1U,PropertyValue{}});
  else if(!sparse || i%16U!=0U)row.entries.push_back({1U,PropertyValue(std::string(i%8==0?"CN":"US"))});
  inputRows.push_back(std::move(row));
 }
 const PropertyCollectionRootStoreOptions options{PhysicalSlotId(41U),PhysicalSlotId(80U),902U};
 {
  auto pin=metadata->openSnapshot();const auto plans=properties.planEntityRootReplacements(pin,inputRows,options);
  const std::array pools{nodePool};
  static_cast<void>(entities.applyNativeGraphBatchTransaction(pin,{},{},pools,plans,properties,options,LogSequenceNumber(2U)));
 }
 const auto rowFileBytes=io->fileSize();
 auto censusMemory=std::make_shared<MaterializedByteBudget>(1024U*1024U);
 NativeStorageAllocationCensus census(io,*metadata,censusMemory);
 auto beforeColumns=metadata->openSnapshot();
 const auto rowCost=census.inspect(beforeColumns);
 ASSERT_TRUE(rowCost);ASSERT_TRUE(rowCost->fullyAttributed());beforeColumns.release();
 NativeColumnImageStore store(values);uint64_t lsn=3;
 const auto saved=store.write(TypedPropertyBlock::verify(TypedPropertyBlock::encode(19U,inputRows),19U,graph::Node::typeId),
                              writeOptions(LargeValueCodec::LVC_RAW),lsn);
 ::testing::Test::RecordProperty("row_fixture_file_bytes",std::to_string(rowFileBytes));
 ::testing::Test::RecordProperty("column_fixture_incremental_file_bytes",std::to_string(io->fileSize()-rowFileBytes));
 size_t columnLogicalBytes=saved.root().logical_bytes;
 for(const auto &column:saved.columns())columnLogicalBytes+=column.logical_bytes;
 ::testing::Test::RecordProperty("column_root_and_payload_logical_bytes",std::to_string(columnLogicalBytes));
 const bool valuesEnabled=graph::debug::PerfTrace::areValuesEnabled();
 struct RestoreTrace {bool enabled;~RestoreTrace(){graph::debug::PerfTrace::setValueCollectionEnabled(enabled);}} restore{valuesEnabled};
 graph::debug::PerfTrace::setValueCollectionEnabled(true);
 auto pin=metadata->openSnapshot();
 const auto combinedCost=census.inspect(pin);
 ASSERT_TRUE(combinedCost);ASSERT_TRUE(combinedCost->fullyAttributed());
 ::testing::Test::RecordProperty("row_fixture_steady_allocated_bytes",std::to_string(rowCost->steadyAllocatedBytes()));
 ::testing::Test::RecordProperty("combined_fixture_steady_allocated_bytes",std::to_string(combinedCost->steadyAllocatedBytes()));
 ::testing::Test::RecordProperty("combined_fixture_total_allocated_bytes",std::to_string(combinedCost->totalAllocatedBytes()));
 for(bool compact:{false,true}) {
  graph::debug::PerfTrace::reset();
  native_column_supply_test::Source source{pin,properties,store,saved.root(),19U,ids,compact};
  native_column_supply_test::Backend backend(source);LogicalPlanReadRuntime runtime(backend);
  LogicalPlanReadRuntimeBounds bounds;
  bounds.maximum_node_records=65;bounds.maximum_rows=65;bounds.maximum_node_page_records=64;
  bounds.maximum_metadata_pages_per_node_page=64;bounds.maximum_property_entries=16;bounds.maximum_distinct_rows=65;
  bounds.maximum_edges_per_node=1;bounds.maximum_edge_records=1;bounds.maximum_adjacency_page_records=1;
  for(bool grouped:{false,true}) {
   auto scan=std::make_unique<logical::LogicalNodeScan>("n");
   if(!grouped)scan->setRangePredicates({{"country",PropertyValue("CN"),PropertyValue("CN"),true,true}});
   std::vector<std::shared_ptr<expressions::Expression>> groups;
   if(grouped)groups.push_back(std::make_shared<expressions::VariableReferenceExpression>("n","country"));
   logical::LogicalAggregate plan(std::move(scan),std::move(groups),{{"count",nullptr,"rows"}},grouped?std::vector<std::string>{"country"}:std::vector<std::string>{});
   const auto result=runtime.executeToQueryResult(plan,bounds);
   ASSERT_EQ(result.rowCount(),grouped?(sparse?3U:2U):1U);
   if(!grouped)EXPECT_EQ(result.getRows().front().at("rows").asPrimitive(),PropertyValue(int64_t{sparse?4:8}));
   else {
    std::map<std::pair<int,std::string>,int64_t> actual;
    for(const auto &row:result.getRows()) {
     const auto &value=row.at("country").asPrimitive();
     const auto *text=std::get_if<std::string>(&value.getVariant());
     actual[{static_cast<int>(value.getType()),text?*text:""}]=std::get<int64_t>(row.at("rows").asPrimitive().getVariant());
    }
    std::map<std::pair<int,std::string>,int64_t> expected{
     {{static_cast<int>(graph::PropertyType::STRING),"CN"},sparse?4:8},
     {{static_cast<int>(graph::PropertyType::STRING),"US"},sparse?52:56}};
    if(sparse)expected[{static_cast<int>(graph::PropertyType::NULL_TYPE),""}]=8;
    EXPECT_EQ(actual,expected);
   }
  }
  const auto trace=graph::debug::PerfTrace::snapshotAndReset();
  const auto rootSelections=trace.find("native_graph.read.property_selected_column_decode");
  if(compact)EXPECT_EQ(rootSelections,trace.end());
  else {
   ASSERT_NE(rootSelections,trace.end());// Four missing roots per sparse batch require no payload selection.
   EXPECT_EQ(rootSelections->second.totalValue,sparse?120:128);
  }
  EXPECT_EQ(source.supplied_cells,128U);
  EXPECT_EQ(source.column_visits,compact?2U:0U);
  EXPECT_EQ(source.row_batches,compact?0U:2U);
  // The production gather boundary restores sparse, duplicate, unordered owners.
  MaterializedByteBudget budget(65536U);
  {
   native_column_supply_test::Snapshot snapshot(source);
   const std::array gatherIds{ids[63],ids[1],ids[0],ids[1],ids[8]};
   const std::array<std::string_view,1> keys{"country"};
   graph::query::execution::detail::DirectPropertyColumns gathered(snapshot,gatherIds.size(),1U,budget);
   gathered.readGather(gatherIds,keys,[](size_t){},[]{});
   EXPECT_EQ(std::get<std::string_view>(gathered.cell(0,gathered.rowId(0)).data),"US");
   if(sparse) {
    EXPECT_EQ(gathered.cell(0,gathered.rowId(1)).state,graph::query::execution::detail::ColumnCellState::CCS_NULL);
    EXPECT_EQ(gathered.cell(0,gathered.rowId(2)).state,graph::query::execution::detail::ColumnCellState::CCS_MISSING);
    EXPECT_EQ(gathered.cell(0,gathered.rowId(3)).state,graph::query::execution::detail::ColumnCellState::CCS_NULL);
   } else {
    EXPECT_EQ(std::get<std::string_view>(gathered.cell(0,gathered.rowId(1)).data),"US");
    EXPECT_EQ(std::get<std::string_view>(gathered.cell(0,gathered.rowId(2)).data),"CN");
    EXPECT_EQ(std::get<std::string_view>(gathered.cell(0,gathered.rowId(3)).data),"US");
   }
   EXPECT_EQ(std::get<std::string_view>(gathered.cell(0,gathered.rowId(4)).data),"CN");
  }
  EXPECT_EQ(budget.used(),0U);
 }
}
} // namespace

TEST_F(RelocatableMetadataStoreTest, NativeDenseRowAndColumnPagesFeedProductionFilterGroupAndGather) {
 runNativeSupplyExperiment(io,superblock(1,1),false);
}
TEST_F(RelocatableMetadataStoreTest, NativeSparseRowAndColumnPagesPreserveNullMissingFilterGroupAndGather) {
 runNativeSupplyExperiment(io,superblock(1,1),true);
}

TEST_F(RelocatableMetadataStoreTest, BorrowedColumnPreservesDoubleBitsAndMixedComplexFallback) {
 const std::array<uint64_t,4> bits{0U,UINT64_C(0x8000000000000000),UINT64_C(0x7ff8000000000042),UINT64_C(0xfff0000000000000)};
 std::vector<PropertyCollectionRoot> roots;
 for(size_t i=0;i<bits.size();++i)roots.push_back({i+1U,1U,{{1U,PropertyValue(std::bit_cast<double>(bits[i]))}}});
 auto image=TypedPropertyBlock::encode(11U,roots);
 TypedPropertyBlock::visitSelectedColumn(image,11U,1U,1U,[&](auto cells){
  ASSERT_EQ(cells.size(),bits.size());
  for(size_t i=0;i<cells.size();++i) {
   ASSERT_TRUE(cells[i].primitive);EXPECT_EQ(cells[i].primitive->type,graph::PropertyType::DOUBLE);
   EXPECT_EQ(cells[i].primitive->bits,bits[i]);
  }
 });
 roots={
  {1U,1U,{{1U,PropertyValue(std::vector<PropertyValue>{PropertyValue(7),PropertyValue("x")} )}}},
  {2U,1U,{{1U,PropertyValue(std::string(9000,'q'))}}},
  {3U,1U,{{1U,PropertyValue(false)}}},
  {4U,1U,{{1U,PropertyValue{}}}},
  {5U,1U,{}}};
 image=TypedPropertyBlock::encode(11U,roots);
 TypedPropertyBlock::visitSelectedColumn(image,11U,1U,1U,[&](auto cells){
  ASSERT_EQ(cells.size(),roots.size());
  EXPECT_FALSE(cells[0].primitive);EXPECT_FALSE(cells[0].complex.empty());
  ASSERT_TRUE(cells[1].primitive);EXPECT_EQ(cells[1].primitive->text,std::string(9000,'q'));
  EXPECT_EQ(borrowedRows(cells,1U),roots);
 });
}
