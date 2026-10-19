#include <algorithm>
#include <array>
#include <vector>
#include "graph/storage/relocatable/index/FixedBTreeMap.hpp"
#include "graph/storage/relocatable/entity/NativeGraphWritePlanner.hpp"
#include "graph/storage/relocatable/index/InitialBTreeBuilder.hpp"
#include "storage/relocatable/entity/NativeGraphWritePlannerTestFixture.hpp"
#include "../../../../../benchmarks/architecture-design/property-subblock/PropertySubblockCandidate.hpp"

using namespace graph::storage;
namespace candidate = zyx::benchmark::subblock;
namespace {
using Directory = FixedBTreeMap<12,16>;
constexpr uint64_t DIRECTORY_INDEX = 0x48504243444952ULL; // lab generic index, not an allocated R3 root
Directory::Key key(uint64_t group, uint32_t token) {
    Directory::Key out{};
    for (unsigned i = 0; i < 8; ++i) out[i] = std::byte((group >> ((7-i)*8)) & 255);
    for (unsigned i = 0; i < 4; ++i) out[8+i] = std::byte((token >> ((3-i)*8)) & 255);
    return out;
}
BTreePageEntry entry(uint64_t group, uint32_t token, const candidate::Locator &locator) {
    auto bytes = Directory::encode(key(group, token), locator.encode());
    return {std::vector<std::byte>(bytes.begin(), bytes.end()), 0};
}
NativeGraphWritePlannerOptions options() {
    NativeGraphWritePlannerOptions out;
    out.first_slot = PhysicalSlotId(40); out.last_slot = PhysicalSlotId(80);
    out.entity_pool_owner_id = 901;
    out.property_roots = {PhysicalSlotId(40),PhysicalSlotId(80),902}; return out;
}
}
class PropertySubblockDirectoryTest : public graph::storage::test::NativeGraphWritePlannerTestFixture {
protected:
    void create(std::span<const BTreePageEntry> entries) {
        NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options());
        auto txn = planner.beginTransaction();
        auto plan = btree_pages_->planInitialTreeMutation(txn.baseSnapshot(), DIRECTORY_INDEX, entries, {},
            BTreeLeafValueLayout::BLVL_EMPTY);
        txn.stageScalarIndexCreate(*btree_pages_, std::move(plan));
        ASSERT_TRUE(txn.commit().committed());
    }
    void stage(NativeGraphWriteTransaction &txn, const BTreePageEntry &oldEntry, const BTreePageEntry &newEntry) {
        txn.stageScalarIndexErase(*btree_pages_,DIRECTORY_INDEX,oldEntry.key,BTreeLeafValueLayout::BLVL_EMPTY);
        txn.stageScalarIndexUpsert(*btree_pages_,DIRECTORY_INDEX,newEntry.key,0,BTreeLeafValueLayout::BLVL_EMPTY);
    }
};
TEST_F(PropertySubblockDirectoryTest, CodecKeepsOrderedBigEndianPrefixAndLittleEndianLocatorSuffix) {
    const auto encoded = entry(UINT64_C(0x0102030405060708),0x090a0b0c,{0x1122334455667788,0x1234,0x56789abc});
    ASSERT_EQ(encoded.key.size(),28U);
    for (size_t i=0;i<12;++i) EXPECT_EQ(encoded.key[i],std::byte(i+1));
    EXPECT_EQ(encoded.key[12],std::byte{0x88}); EXPECT_EQ(encoded.key[19],std::byte{0x11});
    EXPECT_EQ(encoded.key[20],std::byte{0x34}); EXPECT_EQ(encoded.key[21],std::byte{0x12});
    const auto decoded = Directory::decode(encoded);
    EXPECT_EQ(candidate::Locator::decode(decoded.value),(candidate::Locator{0x1122334455667788,0x1234,0x56789abc}));
    auto bad=encoded; bad.value_or_child=1;
    EXPECT_THROW(Directory::decode(bad),FormatException);
    bad=encoded; bad.key.pop_back(); EXPECT_THROW(Directory::decode(bad),FormatException);
    EXPECT_LT(key(255,UINT32_MAX),key(256,1));
    EXPECT_LT(key(256,255),key(256,256));
}
TEST_F(PropertySubblockDirectoryTest, NativeCompoundReplacementRetainsOldPinAndReopensExactLocator) {
    const auto original=entry(1,1,{101,3,7}); const auto changed=entry(1,1,{102,4,8});
    create(std::span(&original,1));
    Directory map(DIRECTORY_INDEX,*btree_pages_,*metadata_);
    auto old=metadata_->openSnapshot();
    const auto oldRoot=metadata_->findIndexRoot(old,DIRECTORY_INDEX);
    NativeGraphWritePlanner planner(*metadata_,*entities_,*labels_,*properties_,*btree_pages_,options());
    auto txn=planner.beginTransaction(); stage(txn,original,changed);
    ASSERT_TRUE(txn.commit().committed());
    auto fresh=metadata_->openSnapshot();
    ASSERT_TRUE(map.find(old,key(1,1))); ASSERT_TRUE(map.find(fresh,key(1,1)));
    EXPECT_EQ(candidate::Locator::decode(*map.find(old,key(1,1))),(candidate::Locator{101,3,7}));
    EXPECT_EQ(candidate::Locator::decode(*map.find(fresh,key(1,1))),(candidate::Locator{102,4,8}));
    EXPECT_FALSE(map.find(fresh,key(1,2)));
    EXPECT_EQ(metadata_->findIndexRoot(fresh,DIRECTORY_INDEX).root_page_id,oldRoot.root_page_id);
    old.release(); fresh.release(); reopenNativeStores();
    Directory reopened(DIRECTORY_INDEX,*btree_pages_,*metadata_); auto snapshot=metadata_->openSnapshot();
    EXPECT_EQ(candidate::Locator::decode(*reopened.find(snapshot,key(1,1))),(candidate::Locator{102,4,8}));
}
TEST_F(PropertySubblockDirectoryTest, PrefixDuplicationIsCorruptionEvenWhenNativeKeysAreUnique) {
    std::array entries{entry(1,1,{101,3,7}),entry(1,1,{102,4,8})};
    std::ranges::sort(entries,[](const auto &a,const auto &b){return a.key<b.key;});
    create(entries); Directory map(DIRECTORY_INDEX,*btree_pages_,*metadata_); auto pin=metadata_->openSnapshot();
    EXPECT_THROW(map.find(pin,key(1,1)),FormatException);
    EXPECT_FALSE(map.find(pin,key(1,2)));
}
TEST_F(PropertySubblockDirectoryTest, ExplicitLeafLayoutCannotChangeWithinOneTransaction) {
    const auto original=entry(1,1,{101,3,7}); create(std::span(&original,1));
    NativeGraphWritePlanner planner(*metadata_,*entities_,*labels_,*properties_,*btree_pages_,options());
    auto txn=planner.beginTransaction();
    EXPECT_THROW(txn.stageScalarIndexUpsert(*btree_pages_,DIRECTORY_INDEX,original.key,1,BTreeLeafValueLayout::BLVL_EMPTY),FormatException);
    EXPECT_THROW(txn.stageScalarIndexErase(*btree_pages_,DIRECTORY_INDEX,original.key,BTreeLeafValueLayout(2)),FormatException);
    txn.stageScalarIndexErase(*btree_pages_,DIRECTORY_INDEX,original.key,BTreeLeafValueLayout::BLVL_EMPTY);
    EXPECT_THROW(txn.stageScalarIndexUpsert(*btree_pages_,DIRECTORY_INDEX,original.key,1),FormatException);
    EXPECT_THROW(txn.stageScalarIndexErase(*btree_pages_,DIRECTORY_INDEX,original.key),FormatException);
    txn.rollback();
}
TEST_F(PropertySubblockDirectoryTest, SplitSharedTreeUsesTypedLayoutAndBoundedPointLookups) {
    std::vector<BTreePageEntry> entries;
    for(uint64_t group=1;group<=512;++group) entries.push_back(entry(group,1,{group+100,0,1}));
    create(entries); Directory map(DIRECTORY_INDEX,*btree_pages_,*metadata_);
    auto old=metadata_->openSnapshot();
    const auto root=metadata_->findIndexRoot(old,DIRECTORY_INDEX);
    EXPECT_EQ(btree_pages_->read(old,root.root_page_id).header().kind,BTreeNodeKind::BPN_INTERNAL);
    NativeGraphWritePlanner planner(*metadata_,*entities_,*labels_,*properties_,*btree_pages_,options());
    auto txn=planner.beginTransaction();
    for(uint64_t group : {1,255,256,512}) stage(txn,entry(group,1,{group+100,0,1}),entry(group,1,{group+200,1,2}));
    ASSERT_TRUE(txn.commit().committed()); auto fresh=metadata_->openSnapshot();
    for(uint64_t group : {1,255,256,512}) {
        ASSERT_TRUE(map.find(old,key(group,1))); ASSERT_TRUE(map.find(fresh,key(group,1)));
        EXPECT_EQ(candidate::Locator::decode(*map.find(old,key(group,1))).page,group+100);
        EXPECT_EQ(candidate::Locator::decode(*map.find(fresh,key(group,1))).page,group+200);
    }
    old.release(); fresh.release();
}
TEST_F(PropertySubblockDirectoryTest, AdmissionFailurePreventsEmissionAndAllowsCleanRetry) {
    const auto original=entry(1,1,{101,3,7}); create(std::span(&original,1));
    Directory map(DIRECTORY_INDEX,*btree_pages_,*metadata_); auto pin=metadata_->openSnapshot();
    size_t callbacks=0;
    // A single-leaf route has no separator allocation. Triggering this with
    // multiple leaves below exercises the existing cursor boundary admission.
    std::vector<BTreePageEntry> entries;
    pin.release();
    for(uint64_t group=1;group<=512;++group) entries.push_back(entry(group,1,{group+100,0,1}));
    NativeGraphWritePlanner planner(*metadata_,*entities_,*labels_,*properties_,*btree_pages_,options());
    auto txn=planner.beginTransaction();
    txn.stageScalarIndexErase(*btree_pages_,DIRECTORY_INDEX,original.key,BTreeLeafValueLayout::BLVL_EMPTY);
    for(const auto &e:entries) txn.stageScalarIndexUpsert(*btree_pages_,DIRECTORY_INDEX,e.key,0,BTreeLeafValueLayout::BLVL_EMPTY);
    ASSERT_TRUE(txn.commit().committed()); auto current=metadata_->openSnapshot();
    // Root descent itself does not allocate the retained boundary callback;
    // select the final record of a leaf so duplicate-prefix lookahead crosses it.
    BTreeIndex index(DIRECTORY_INDEX,*btree_pages_,*metadata_,BTreeLeafValueLayout::BLVL_EMPTY);
    const auto root=metadata_->findIndexRoot(current,DIRECTORY_INDEX);
    const auto parent=btree_pages_->read(current,root.root_page_id);
    const auto left=btree_pages_->read(current,parent.header().leftmost_child);
    const auto records=left.entries(); ASSERT_FALSE(records.empty());
    const auto target=Directory::decode(records.back()).key;
    EXPECT_THROW(map.find(current,target,[&](size_t bytes){++callbacks;if(bytes)throw std::length_error("test admission");}),std::length_error);
    EXPECT_GT(callbacks,0U); EXPECT_TRUE(map.find(current,target));
}
TEST_F(PropertySubblockDirectoryTest, ExhaustedPinnedPoolRebuildPreservesKeyOnlyLayoutAndAllOldGenerations) {
    auto currentEntry=entry(1,1,{101,0,1}); create(std::span(&currentEntry,1));
    Directory map(DIRECTORY_INDEX,*btree_pages_,*metadata_);
    auto original=metadata_->openSnapshot();
    const auto root=metadata_->findIndexRoot(original,DIRECTORY_INDEX);
    const auto originalPool=metadata_->findPageLocation(original,root.root_page_id).segment_id;
    std::vector<MetadataSnapshotPin> pins;
    NativeGraphWritePlanner planner(*metadata_,*entities_,*labels_,*properties_,*btree_pages_,options());
    for(uint64_t iteration=1;iteration<=36;++iteration) {
        const auto next=entry(1,1,{101+iteration,0,uint32_t(iteration+1)});
        auto txn=planner.beginTransaction(); stage(txn,currentEntry,next);
        ASSERT_TRUE(txn.commit().committed()); pins.push_back(metadata_->openSnapshot()); currentEntry=next;
    }
    const auto newRoot=metadata_->findIndexRoot(pins.back(),DIRECTORY_INDEX);
    EXPECT_NE(metadata_->findPageLocation(pins.back(),newRoot.root_page_id).segment_id,originalPool);
    EXPECT_EQ(candidate::Locator::decode(*map.find(original,key(1,1))).page,101U);
    for(size_t i=0;i<pins.size();++i)
        EXPECT_EQ(candidate::Locator::decode(*map.find(pins[i],key(1,1))).page,102U+i);
    for(auto &pin:pins) pin.release(); original.release();
    reopenNativeStores(); Directory reopened(DIRECTORY_INDEX,*btree_pages_,*metadata_);
    auto recovered=metadata_->openSnapshot(); EXPECT_EQ(candidate::Locator::decode(*reopened.find(recovered,key(1,1))).page,137U);
}
TEST_F(PropertySubblockDirectoryTest, WholeMapRetirementRemovesCurrentRootButRetainsPinnedRead) {
    const auto original=entry(1,1,{101,3,7}); create(std::span(&original,1));
    auto old=metadata_->openSnapshot(); Directory map(DIRECTORY_INDEX,*btree_pages_,*metadata_);
    NativeGraphWritePlanner planner(*metadata_,*entities_,*labels_,*properties_,*btree_pages_,options());
    auto txn=planner.beginTransaction(); txn.stageScalarIndexDrop(*btree_pages_,DIRECTORY_INDEX,32);
    ASSERT_TRUE(txn.commit().committed()); auto fresh=metadata_->openSnapshot();
    EXPECT_FALSE(metadata_->findIndexRootOptional(fresh,DIRECTORY_INDEX));
    EXPECT_FALSE(map.find(fresh,key(1,1))); EXPECT_TRUE(map.find(old,key(1,1)));
    old.release(); fresh.release(); reopenNativeStores();
    Directory reopened(DIRECTORY_INDEX,*btree_pages_,*metadata_); auto recovered=metadata_->openSnapshot();
    EXPECT_FALSE(reopened.find(recovered,key(1,1)));
}
TEST_F(PropertySubblockDirectoryTest, EveryBootstrapInterruptionRecoversExactlyOneCompletePrefixValue) {
    auto currentEntry=entry(1,1,{101,0,1}); create(std::span(&currentEntry,1));
    for(auto failpoint : {BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE,
        BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE,BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE}) {
        const auto next=entry(1,1,{102,0,2});
        {
            NativeGraphWritePlanner planner(*metadata_,*entities_,*labels_,*properties_,*btree_pages_,options());
            auto txn=planner.beginTransaction(); stage(txn,currentEntry,next);
            MetadataPublishOptions publish; publish.bootstrap_failpoint=failpoint;
            EXPECT_THROW(txn.commit(publish),BootstrapPublishInterrupted);
            EXPECT_EQ(txn.state(),NativeGraphWriteTransactionState::NGWTS_INDETERMINATE);
        }
        reopenNativeStores(); Directory map(DIRECTORY_INDEX,*btree_pages_,*metadata_);
        auto recovered=metadata_->openSnapshot();
        const auto expected=failpoint==BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE ? 102U : 101U;
        ASSERT_TRUE(map.find(recovered,key(1,1)));
        EXPECT_EQ(candidate::Locator::decode(*map.find(recovered,key(1,1))).page,expected);
        if(expected==102) currentEntry=next;
    }
}
TEST_F(PropertySubblockDirectoryTest, ExistingU64TreeCannotBeReinterpretedAsAKeyOnlyMap) {
    const auto record=entry(1,1,{101,0,1});
    NativeGraphWritePlanner planner(*metadata_,*entities_,*labels_,*properties_,*btree_pages_,options());
    {
        auto create=planner.beginTransaction();
        const BTreePageEntry u64{record.key,1};
        create.stageScalarIndexCreate(*btree_pages_,DIRECTORY_INDEX,std::span(&u64,1));
        ASSERT_TRUE(create.commit().committed());
    }
    auto old=metadata_->openSnapshot(); const auto generation=old.superblock().generation;
    Directory map(DIRECTORY_INDEX,*btree_pages_,*metadata_);
    EXPECT_THROW(map.find(old,key(1,1)),FormatException);
    auto txn=planner.beginTransaction();
    txn.stageScalarIndexErase(*btree_pages_,DIRECTORY_INDEX,record.key,BTreeLeafValueLayout::BLVL_EMPTY);
    EXPECT_THROW(txn.commit(),FormatException);
    auto fresh=metadata_->openSnapshot(); EXPECT_EQ(fresh.superblock().generation,generation);
    BTreeIndex u64(DIRECTORY_INDEX,*btree_pages_,*metadata_); EXPECT_EQ(u64.find(fresh,record.key),1U);
}
