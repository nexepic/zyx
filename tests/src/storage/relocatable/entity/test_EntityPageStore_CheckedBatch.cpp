#include "storage/relocatable/entity/EntityPageStoreTestFixture.hpp"

TEST_F(RelocatableEntityPageStoreTest, CheckedMixedPageBatchKeepsFailedShadowPrivateAndPreservesOldSnapshot) {
 auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
 EntityPageStore store(io, *metadata);
 const auto original = node(11U,12U);
 ASSERT_TRUE(store.allocateNode(newPool(31U,901U,101U,40U),1001U,original,LogSequenceNumber(2U)).has_value());
 {
  const auto pin = metadata->openSnapshot();
  static_cast<void>(store.insertNode(pin,PageId(101U),1002U,original,LogSequenceNumber(3U)));
 }
 const auto before = metadata->openSnapshot();
 EntityPageBatchMutation mutation;
 mutation.page_id = PageId(101U);
 mutation.record_kind = EntityPageRecordKind::EPRK_NODE;
 mutation.node_updates = {{1001U,node(21U,22U)}};
 mutation.node_insertions = {{1003U,node(31U,32U)},{1004U,node(41U,42U)}};
 mutation.node_insertions.back().record.inline_label_count = 255U;
 mutation.erased_node_ids = {1002U};
 EXPECT_THROW(static_cast<void>(store.applyBatch(before,std::span(&mutation,1U),LogSequenceNumber(4U))),FormatException);
 {
  const auto afterFailure = metadata->openSnapshot();
  EXPECT_EQ(afterFailure.superblock(),before.superblock());
  EXPECT_EQ(store.readNode(afterFailure,1001U),original);
  EXPECT_EQ(store.readNode(afterFailure,1002U),original);
  EXPECT_FALSE(metadata->findNodePageLocationOptional(afterFailure,1003U).has_value());
 }
 mutation.node_insertions.pop_back();
 static_cast<void>(store.applyBatch(before,std::span(&mutation,1U),LogSequenceNumber(4U)));
 const auto current = metadata->openSnapshot();
 EXPECT_EQ(store.readNode(current,1001U),mutation.node_updates.front().record);
 EXPECT_EQ(store.readNode(current,1003U),mutation.node_insertions.front().record);
 EXPECT_FALSE(metadata->findNodePageLocationOptional(current,1002U).has_value());
 EXPECT_EQ(store.readNode(before,1001U),original);
 EXPECT_EQ(store.readNode(before,1002U),original);
 const auto inserted = metadata->findNodePageLocation(current,1003U);
 EXPECT_EQ(inserted.slot_id,2U); // insertion precedes erasure: no premature slot reuse
 EXPECT_EQ(inserted.slot_generation,1U);
}

TEST_F(RelocatableEntityPageStoreTest, CrossGenerationCowRebindsEntityHeadersForFreshAndExistingDestinations) {
 auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
 EntityPageStore store(io, *metadata);
 auto first = newPool(31U,901U,101U,40U); first.generation = Generation(7U);
 auto second = newPool(32U,901U,102U,41U); second.generation = Generation(7U);
 auto destination = newPool(33U,901U,103U,42U); destination.generation = Generation(3U);
 const auto original = node(0U,0U,7U), changed = node(0U,0U,11U);
 ASSERT_TRUE(store.allocateNode(first,1001U,original,LogSequenceNumber(2U)));
 ASSERT_TRUE(store.allocateNode(second,1002U,original,LogSequenceNumber(3U)));
 ASSERT_TRUE(store.allocateNode(destination,2001U,original,LogSequenceNumber(4U)));
 const auto old = metadata->openSnapshot();
 std::array<EntityPageBatchMutation,2> updates;
 updates[0].page_id=PageId(101U); updates[0].record_kind=EntityPageRecordKind::EPRK_NODE;
 updates[0].node_updates={{1001U,changed}};
 updates[0].replacement_new_pool=EntityPageFreshPoolDestination{
     SegmentId(34U),901U,Generation(1U),PhysicalSlotId(43U),PhysicalSlotId(43U)};
 updates[1].page_id=PageId(102U); updates[1].record_kind=EntityPageRecordKind::EPRK_NODE;
 updates[1].node_updates={{1002U,changed}};
 updates[1].replacement_pool=metadata->findCatalog(old,SegmentId(33U));
 static_cast<void>(store.applyBatch(old,updates,LogSequenceNumber(5U)));
 const auto current=metadata->openSnapshot();
 EXPECT_EQ(store.readNode(current,1001U),changed);
 EXPECT_EQ(store.readNode(current,1002U),changed);
 EXPECT_EQ(store.readNode(current,2001U),original);
 EXPECT_EQ(store.readNode(old,1001U),original);
 EXPECT_EQ(store.readNode(old,1002U),original);
}

TEST_F(RelocatableEntityPageStoreTest, CrossGenerationCowRebindsOverflowLabelsAndKeepsOldSlotsReadable) {
 auto metadata=RelocatableMetadataStore::initializeNew(io,initialSuperblock()); EntityPageStore store(io,*metadata);
 auto request=newPool(31U,901U,101U,40U); request.generation=Generation(7U);
 ASSERT_TRUE(store.allocateNode(request,1001U,node(0U,0U),LogSequenceNumber(2U)));
 const auto initial=metadata->openSnapshot(); const auto pool=metadata->findCatalog(initial,SegmentId(31U));
 const std::array<uint64_t,4> labels{7U,9U,11U,13U};
 const auto first=labelPagePayload(pool,PageId(102U),LogSequenceNumber(3U),std::span(labels).subspan(3U));
 const auto record=nodeWithOverflow(labels,first.overflow);
 EntityPageBatchMutation update; update.page_id=PageId(101U); update.record_kind=EntityPageRecordKind::EPRK_NODE;
 update.node_updates={{1001U,record}};
 EntityPageAuxiliaryMutationBatch append;
 append.appends.push_back({pool,PageId(102U),PageOwnerKind::POK_ENTITY_LABEL,first.payload,first.free_bytes});
 append.node_update_bindings.push_back({1001U,metadata->findNodePageLocation(initial,1001U),record});
 static_cast<void>(store.applyAuxiliaryEntityBatchTransaction(initial,std::span(&update,1U),{},{},append,LogSequenceNumber(3U)));
 const auto old=metadata->openSnapshot(); const auto source=store.readAuxiliaryPage(old,PageId(102U),PageOwnerKind::POK_ENTITY_LABEL);
 const std::array<uint64_t,1> changed{99U};
 const auto replacement=labelPagePayload(source.pool,PageId(102U),LogSequenceNumber(4U),changed);
 EntityPageAuxiliaryReplacement move;
 move.expected_pool=source.pool; move.expected_location=source.location; move.owner_kind=PageOwnerKind::POK_ENTITY_LABEL;
 move.expected_owner_lsn=source.owner.committed_lsn; move.payload=replacement.payload; move.free_bytes=replacement.free_bytes;
 move.replacement_new_pool=EntityPageFreshPoolDestination{SegmentId(32U),901U,Generation(1U),PhysicalSlotId(41U),PhysicalSlotId(41U)};
 EntityPageAuxiliaryMutationBatch batch; batch.replacements.push_back(std::move(move));
 batch.node_update_bindings.push_back({1001U,metadata->findNodePageLocation(old,1001U),record});
 static_cast<void>(store.applyAuxiliaryEntityBatchTransaction(old,std::span(&update,1U),{},{},batch,LogSequenceNumber(4U)));
 const auto current=metadata->openSnapshot(); const auto fresh=store.readAuxiliaryPage(current,PageId(102U),PageOwnerKind::POK_ENTITY_LABEL);
 const auto prior=store.readAuxiliaryPage(old,PageId(102U),PageOwnerKind::POK_ENTITY_LABEL);
 const SlottedSlotReference slot{PageId(102U),first.overflow.slot_id,first.overflow.slot_generation};
 EXPECT_EQ(EntityLabelSetCodec::decode(SlottedPage::open(fresh.owner,fresh.payload).read(slot)),(std::vector<uint64_t>{99U}));
 EXPECT_EQ(EntityLabelSetCodec::decode(SlottedPage::open(prior.owner,prior.payload).read(slot)),(std::vector<uint64_t>{13U}));
}
