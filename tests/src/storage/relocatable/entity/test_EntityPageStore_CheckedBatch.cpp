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
