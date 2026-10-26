#include "RelocatableBTreePageStoreTestUtils.hpp"

using namespace relocatable_btree_test;

TEST_F(RelocatableBTreePageStoreTest, TransactionOverlayUsesEachEvacuatedChildGenerationAndRetainsOldPins) {
 auto metadata=RelocatableMetadataStore::initializeNew(io,superblock());
 SlottedPagePool pages(io,*metadata); BTreePageStore store(pages,*metadata); NativeBTreeWriteContext writes(io,*metadata,pages);
 const auto request=allocationRequest();
 ASSERT_TRUE(store.allocate(request,emptyPage(LogSequenceNumber(2)),PhysicalSlotId(40),PhysicalSlotId(40),LogSequenceNumber(2)));
 const IndexRootLocation root{17,request.page_id,request.generation};
 static_cast<void>(store.publishRoots(std::span(&root,1),{},LogSequenceNumber(3)));
 uint64_t lsn=4;
 for(uint16_t key:{100,102,104,106}) static_cast<void>(store.upsert(17,wideKey(key),key,LogSequenceNumber(lsn++)));
 auto destination=request; destination.segment_id=SegmentId(72); destination.pool_owner_id=78;
 destination.page_id=PageId(200); destination.generation=Generation(2);
 auto owner=pageOwner(LogSequenceNumber(lsn)); owner.page_id=destination.page_id; owner.generation=destination.generation;
 BTreePageHeader header; header.index_id=17; header.page_id=destination.page_id; header.generation=destination.generation;
 ASSERT_TRUE(store.allocate(destination,BTreePage::create(owner,header),PhysicalSlotId(41),PhysicalSlotId(41),LogSequenceNumber(lsn++)));
 {
  const auto snapshot=metadata->openSnapshot(); const auto top=metadata->findIndexRoot(snapshot,17);
  const auto page=store.read(snapshot,top.root_page_id); ASSERT_EQ(page.header().level,1U);
  const auto child=store.read(snapshot,PageId(page.entries().back().value_or_child));
  static_cast<void>(store.evacuate(child,destination.segment_id,LogSequenceNumber(lsn++)));
 }
 const auto old=metadata->openSnapshot(); BTreeIndex index(17,store,*metadata);
 EXPECT_EQ(index.find(old,wideKey(104)),std::optional<uint64_t>(104));
 auto transaction=writes.beginTransaction(); transaction.stageScalarIndexUpsert(store,17,wideKey(104),404);
 ASSERT_NO_THROW(ASSERT_TRUE(transaction.commit().committed()));
 const auto current=metadata->openSnapshot();
 EXPECT_EQ(index.find(current,wideKey(104)),std::optional<uint64_t>(404));
 EXPECT_EQ(index.find(old,wideKey(104)),std::optional<uint64_t>(104));
 EXPECT_EQ(index.find(current,wideKey(100)),std::optional<uint64_t>(100));
}
