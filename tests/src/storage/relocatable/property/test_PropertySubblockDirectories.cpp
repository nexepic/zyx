#include <algorithm>
#include <array>
#include <vector>
#include "graph/debug/PerfTrace.hpp"
#include "graph/query/execution/MaterializedByteBudget.hpp"
#include "graph/utils/OwnedHeapReservation.hpp"
#include "graph/storage/relocatable/index/FixedBTreeMap.hpp"
#include "graph/storage/relocatable/property/HybridPropertyDirectory.hpp"
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
    size_t emitted = 0;
    EXPECT_THROW(map.visitAll(pin, 2, [&](const auto &) { ++emitted; return true; }), FormatException);
    EXPECT_EQ(emitted, 0U);
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
    size_t delivered = 0;
    EXPECT_THROW(map.visitPrefix(current, target, 1, [&](const auto &) { ++delivered; return true; },
        [](size_t bytes) { if (bytes) throw std::length_error("prefix boundary admission"); }), std::length_error);
    EXPECT_EQ(delivered, 0U);
    class CancelPrefix final : public CancellationCheckpoint {
    public: void check() const override { throw std::logic_error("cancel owner enumeration"); }
    } cancellation;
    EXPECT_THROW(map.visitPrefix(current, target, 1, [&](const auto &) { ++delivered; return true; },
        {}, &cancellation), std::logic_error);
    EXPECT_EQ(delivered, 0U);
    EXPECT_THROW(map.find(current, target, {}, &cancellation), std::logic_error);
    EXPECT_TRUE(map.find(current, target));
    EXPECT_TRUE(map.visitPrefix(current, target, 1, [&](const auto &) { ++delivered; return true; }));
    EXPECT_EQ(delivered, 1U);
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

TEST_F(PropertySubblockDirectoryTest, OwnerPrefixEnumerationIsBoundedAndDoesNotVisitAnotherOwner) {
    std::vector<BTreePageEntry> records;
    for (uint32_t token = 1; token <= 600; ++token) records.push_back(entry(1, token, {100 + token, 0, 1}));
    records.push_back(entry(2, 1, {900, 0, 1})); create(records);
    Directory map(DIRECTORY_INDEX, *btree_pages_, *metadata_); auto pin = metadata_->openSnapshot();
    const auto lower = key(1, 0); const auto prefix = std::span(lower).first(8);
    size_t visited = 0;
    EXPECT_FALSE(map.visitPrefix(pin, prefix, 3, [&](const auto &item) {
        EXPECT_EQ(item.key, key(1, uint32_t(++visited))); return true;
    }));
    EXPECT_EQ(visited, 3U);
    visited = 0;
    EXPECT_TRUE(map.visitPrefix(pin, prefix, 600, [&](const auto &item) {
        EXPECT_EQ(item.key, key(1, uint32_t(++visited))); return true;
    }));
    EXPECT_EQ(visited, 600U);
    visited = 0;
    EXPECT_FALSE(map.visitPrefix(pin, prefix, 600, [&](const auto &) { ++visited; return false; }));
    EXPECT_EQ(visited, 1U);
    const auto missing = key(3, 0);
    EXPECT_TRUE(map.visitPrefix(pin, std::span(missing).first(8), 1, [&](const auto &) { ADD_FAILURE(); return true; }));
    EXPECT_THROW(map.visitPrefix(pin, {}, 1, [](const auto &) { return true; }), FormatException);
    EXPECT_THROW(map.visitPrefix(pin, prefix, 0, [](const auto &) { return true; }), FormatException);
    EXPECT_THROW(map.visitPrefix(pin, prefix, (std::numeric_limits<size_t>::max)(), [](const auto &) { return true; }), FormatException);
}
TEST_F(PropertySubblockDirectoryTest, PrefixEnumerationRejectsDuplicateOwnershipAcrossLeavesBeforeDelivery) {
    using WideMap = FixedBTreeMap<12, 1800>;
    std::vector<BTreePageEntry> records;
    for (const auto [token, suffix] : {std::pair{1U, 1U}, std::pair{2U, 1U}, std::pair{2U, 2U}}) {
        WideMap::Value value{}; value[0] = std::byte(suffix);
        const auto bytes = WideMap::encode(key(1, token), value);
        records.push_back({std::vector<std::byte>(bytes.begin(), bytes.end()), 0});
    }
    create(records); auto pin = metadata_->openSnapshot();
    const auto root = btree_pages_->read(pin, metadata_->findIndexRoot(pin, DIRECTORY_INDEX).root_page_id);
    ASSERT_EQ(root.header().kind, BTreeNodeKind::BPN_INTERNAL);
    const auto left = btree_pages_->read(pin, root.header().leftmost_child);
    ASSERT_EQ(left.entries().size(), 2U);
    EXPECT_EQ(WideMap::decode(left.entries().back()).key, key(1, 2));
    WideMap map(DIRECTORY_INDEX, *btree_pages_, *metadata_); size_t delivered = 0;
    const auto target = key(1, 2);
    EXPECT_THROW(map.visitPrefix(pin, target, 1, [&](const auto &) { ++delivered; return true; }), FormatException);
    EXPECT_EQ(delivered, 0U);
}

TEST_F(PropertySubblockDirectoryTest, TypedOwnerEnumerationValidatesSourcesAndKeepsTokenOrder) {
    using Codec = HybridPropertyDirectoryCodec;
    const EntityPropertyKey owner{EntityPropertyOwnerKind::EPOK_NODE, 65};
    const EntityPropertyKey nextOwner{EntityPropertyOwnerKind::EPOK_NODE, 66};
    std::vector<BTreePageEntry> entries;
    for (const auto token : {1U, 256U, UINT32_MAX}) {
        const auto bytes = HybridPropertyOwnerMap::encode(Codec::ownerKey(owner, token),
            Codec::sourceValue(token == 256 ? HybridPropertySource::HPS_EXTENSION : HybridPropertySource::HPS_COLUMN));
        entries.push_back({std::vector<std::byte>(bytes.begin(), bytes.end()), 0});
    }
    const auto other = HybridPropertyOwnerMap::encode(Codec::ownerKey(nextOwner, 1),
        Codec::sourceValue(HybridPropertySource::HPS_COLUMN));
    entries.push_back({std::vector<std::byte>(other.begin(), other.end()), 0});
    create(entries);
    HybridPropertyOwnerMap map(DIRECTORY_INDEX, *btree_pages_, *metadata_);
    const auto pin = metadata_->openSnapshot();
    std::vector<uint32_t> tokens;
    EXPECT_TRUE(Codec::visitOwner(map, pin, owner, 3, [&](uint32_t token, HybridPropertySource source) {
        tokens.push_back(token);
        EXPECT_EQ(source, token == 256 ? HybridPropertySource::HPS_EXTENSION : HybridPropertySource::HPS_COLUMN);
        return true;
    }));
    EXPECT_EQ(tokens, (std::vector<uint32_t>{1, 256, UINT32_MAX}));
    size_t count = 0;
    EXPECT_FALSE(Codec::visitOwner(map, pin, owner, 2, [&](auto, auto) { ++count; return true; }));
    EXPECT_EQ(count, 2U);
    EXPECT_THROW(Codec::visitOwner(map, pin, {}, 3, [](auto, auto) { return true; }), FormatException);
}
TEST_F(PropertySubblockDirectoryTest, MalformedOwnerMembershipCannotReachTypedVisitor) {
    using Codec = HybridPropertyDirectoryCodec;
    const EntityPropertyKey owner{EntityPropertyOwnerKind::EPOK_EDGE, 1};
    auto value = Codec::sourceValue(HybridPropertySource::HPS_COLUMN);
    value[1] = std::byte{1};
    const auto bytes = HybridPropertyOwnerMap::encode(Codec::ownerKey(owner, 1), value);
    const BTreePageEntry malformed{std::vector<std::byte>(bytes.begin(), bytes.end()), 0};
    create(std::span(&malformed, 1));
    HybridPropertyOwnerMap map(DIRECTORY_INDEX, *btree_pages_, *metadata_);
    const auto pin = metadata_->openSnapshot();
    size_t visited = 0;
    EXPECT_THROW(Codec::visitOwner(map, pin, owner, 1, [&](auto, auto) { ++visited; return true; }), FormatException);
    EXPECT_EQ(visited, 0U);
}

TEST_F(PropertySubblockDirectoryTest, BoundDirectoryRejectsAnotherSnapshotDespiteStableLogicalRootAndEpoch) {
    const auto original = entry(1, 1, {101, 0, 1});
    create(std::span(&original, 1));
    const auto old = metadata_->openSnapshot();
    const auto root = metadata_->findIndexRoot(old, DIRECTORY_INDEX);
    const BTreeRootReadBinding binding{old.superblock().database_id, old.superblock().generation, root};
    Directory bound(DIRECTORY_INDEX, *btree_pages_, *metadata_, binding);
    EXPECT_EQ(*bound.find(old, key(1, 1)), Directory::decode(original).value);
    BTreeIndex boundIndex(DIRECTORY_INDEX, *btree_pages_, *metadata_, BTreeLeafValueLayout::BLVL_EMPTY, binding);
    const auto changed = entry(1, 1, {102, 0, 2});
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options());
    auto txn = planner.beginTransaction(); stage(txn, original, changed); ASSERT_TRUE(txn.commit().committed());
    const auto fresh = metadata_->openSnapshot();
    EXPECT_EQ(metadata_->findIndexRoot(fresh, DIRECTORY_INDEX), root);
    EXPECT_NE(fresh.superblock().generation, old.superblock().generation);
    EXPECT_THROW(bound.find(fresh, key(1, 1)), FormatException);
    size_t visited = 0;
    EXPECT_THROW(bound.visitPrefix(fresh, key(1, 1), 1, [&](const auto &) { ++visited; return true; }), FormatException);
    EXPECT_EQ(visited, 0U);
    // orderedSource clones the index; it must retain the same root binding.
    auto invalidSource = boundIndex.orderedSource(fresh, 1); auto invalidCursor = invalidSource();
    EXPECT_THROW(invalidCursor(), FormatException);
    auto validSource = boundIndex.orderedSource(old, 1); auto validCursor = validSource();
    ASSERT_TRUE(validCursor());
    EXPECT_EQ(*bound.find(old, key(1, 1)), Directory::decode(original).value);
    Directory current(DIRECTORY_INDEX, *btree_pages_, *metadata_,
        BTreeRootReadBinding{fresh.superblock().database_id, fresh.superblock().generation, root});
    EXPECT_EQ(*current.find(fresh, key(1, 1)), Directory::decode(changed).value);
}
TEST_F(PropertySubblockDirectoryTest, BoundDirectoryRejectsForeignDatabaseRootPageEpochAndAbsentTree) {
    const auto original = entry(1, 1, {101, 0, 1}); create(std::span(&original, 1));
    const auto pin = metadata_->openSnapshot();
    const BTreeRootReadBinding valid{pin.superblock().database_id, pin.superblock().generation,
        metadata_->findIndexRoot(pin, DIRECTORY_INDEX)};
    for (unsigned bad = 0; bad < 4; ++bad) {
        auto binding = valid;
        if (bad == 0) binding.database_id = DatabaseId(valid.database_id.value() + 1);
        if (bad == 1) binding.root.root_page_id = PageId(valid.root.root_page_id.value() + 1);
        if (bad == 2) binding.root.root_generation = Generation(valid.root.root_generation.value() + 1);
        if (bad == 3) binding.root.index_id = DIRECTORY_INDEX + 1;
        Directory bound(binding.root.index_id, *btree_pages_, *metadata_, binding);
        EXPECT_THROW(bound.find(pin, key(1, 1)), FormatException) << bad;
    }
    auto incomplete = valid; incomplete.root.root_page_id = {};
    EXPECT_THROW((Directory(DIRECTORY_INDEX, *btree_pages_, *metadata_, incomplete)), std::invalid_argument);
    incomplete = valid; incomplete.root.index_id++;
    EXPECT_THROW((Directory(DIRECTORY_INDEX, *btree_pages_, *metadata_, incomplete)), std::invalid_argument);
}

TEST_F(PropertySubblockDirectoryTest, WholeMapIntegrityCursorKeepsBoundsOrderAndAdmissionAcrossLeaves) {
    std::vector<BTreePageEntry> records;
    for (uint64_t i = 1; i <= 600; ++i) records.push_back(entry(i, 1, {100 + i, 1, 1}));
    create(records); const auto pin = metadata_->openSnapshot();
    Directory map(DIRECTORY_INDEX, *btree_pages_, *metadata_);
    size_t emitted = 0;
    EXPECT_FALSE(map.visitAll(pin, 1, [&](const auto &value) {
        ++emitted; EXPECT_EQ(value.key, key(1, 1)); return true;
    }));
    EXPECT_EQ(emitted, 1U); emitted = 0;
    EXPECT_TRUE(map.visitAll(pin, 600, [&](const auto &value) {
        ++emitted; EXPECT_EQ(value.key, key(emitted, 1)); return true;
    }));
    EXPECT_EQ(emitted, 600U); emitted = 0;
    EXPECT_THROW(map.visitAll(pin, 600, [&](const auto &) { ++emitted; return true; },
        [](size_t) { throw std::length_error("deny map cursor boundary"); }), std::length_error);
    EXPECT_EQ(emitted, 0U);
    EXPECT_THROW(map.visitAll(pin, 0, [](const auto &) { return true; }), FormatException);
    EXPECT_THROW(map.visitAll(pin, (std::numeric_limits<size_t>::max)(), [](const auto &) { return true; }), FormatException);
}

TEST_F(PropertySubblockDirectoryTest, FixedKeyStreamMatchesGenericAcrossLeavesAndRetriesBeforeEmission) {
    // Stay within the initial builder's bounded input while forcing multiple leaves.
    constexpr size_t count = 2000;
    std::vector<BTreePageEntry> records;
    for (uint64_t id = 1; id <= count; ++id) records.push_back(entry(id, 1, {100 + id, 3, 7}));
    create(records);
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(8U * 1024U * 1024U);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto pin = metadata_->openSnapshot();
        BTreeIndex index(DIRECTORY_INDEX, *btree_pages_, *metadata_, BTreeLeafValueLayout::BLVL_EMPTY);
        class Cancel final : public CancellationCheckpoint {
        public:
            bool armed = false;
            void check() const override { if (armed) throw std::logic_error("fixed stream cancelled"); }
        } cancel;
        const auto lower = key(25, 1);
        auto generic = index.openRangeCursor(pin, std::span<const std::byte>(lower), 5000);
        auto fixed = index.openFixedKeyRangeCursor(pin, std::span<const std::byte>(lower), 5000,
            Directory::Record{}.size(), BufferPoolReadHint::BPRH_NORMAL, false, &cancel);
        ASSERT_EQ(fixed.fixedKeyWidth(), Directory::Record{}.size());
        struct Refusal {};
        bool rejectBoundary = true; size_t refusals = 0, seen = 0;
        const std::function<void(size_t)> admission = [&](size_t bytes) {
            if (bytes && rejectBoundary) { rejectBoundary = false; throw Refusal{}; }
        };
        for (;;) {
            if (seen == 10) {
                cancel.armed = true;
                EXPECT_THROW(static_cast<void>(fixed.nextFixedKey(admission)), std::logic_error);
                cancel.armed = false;
            }
            std::optional<BTreeFixedKey> actual;
            try { actual = fixed.nextFixedKey(admission); }
            catch (const Refusal &) { ++refusals; continue; }
            const auto expected = generic.next();
            ASSERT_EQ(actual.has_value(), expected.has_value());
            if (!actual) break;
            ASSERT_EQ(actual->key().size(), expected->key.size());
            EXPECT_TRUE(std::equal(actual->key().begin(), actual->key().end(), expected->key.begin()));
            ++seen;
        }
        EXPECT_EQ(seen, count - 24U); EXPECT_EQ(refusals, 1U);
        auto bounded = index.openFixedKeyRangeCursor(pin, std::nullopt, 2, Directory::Record{}.size());
        ASSERT_NE(bounded.fixedKeyWidth(), 0U);
        ASSERT_TRUE(bounded.nextFixedKey()); ASSERT_TRUE(bounded.nextFixedKey());
        EXPECT_THROW(static_cast<void>(bounded.nextFixedKey()), FormatException);
        Directory map(DIRECTORY_INDEX, *btree_pages_, *metadata_); size_t mapped = 0;
        EXPECT_TRUE(map.visitAll(pin, 5000, [&](const auto &value) {
            ++mapped; EXPECT_EQ(value.key, key(mapped, 1)); return true;
        }));
        EXPECT_EQ(mapped, count);
    }
    EXPECT_EQ(memory->used(), 0U);
}

TEST_F(PropertySubblockDirectoryTest, FixedKeyOptionalConversionDeclinesWithoutRejectingReadableCursor) {
    const auto original = entry(1, 1, {101, 3, 7}); create(std::span(&original, 1));
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto pin = metadata_->openSnapshot();
        BTreeIndex index(DIRECTORY_INDEX, *btree_pages_, *metadata_, BTreeLeafValueLayout::BLVL_EMPTY);
        graph::utils::OwnedHeapReservation pressure(memory);
        struct Inject { decltype(memory) budget; graph::utils::OwnedHeapReservation *pressure; bool injected = false; } inject{memory, &pressure};
        graph::debug::PerfTrace::ScopedObserver observer(&inject,
            [](void *state, std::string_view name, int64_t, bool duration) noexcept {
                auto &value = *static_cast<Inject *>(state);
                if (!duration && name == "native_btree.read.fixed_key_cursor_preparation") {
                    try { value.pressure->set(value.budget->available()); value.injected = true; }
                    catch (...) { std::terminate(); }
                }
            });
        auto cursor = index.openFixedKeyRangeCursor(pin, std::nullopt, 2, Directory::Record{}.size());
        ASSERT_TRUE(inject.injected); EXPECT_EQ(cursor.fixedKeyWidth(), 0U);
        pressure.set(0);
        const auto actual = cursor.next(); ASSERT_TRUE(actual);
        EXPECT_EQ(Directory::decode(*actual).key, key(1, 1)); EXPECT_FALSE(cursor.next());
    }
    EXPECT_EQ(memory->used(), 0U);
}

TEST_F(PropertySubblockDirectoryTest, FixedKeyWholeMapRejectsDuplicateLogicalOwnershipAfterSeveralLeaves) {
    std::vector<BTreePageEntry> records;
    for (uint64_t id = 1; id <= 2000; ++id) records.push_back(entry(id, 1, {100 + id, 3, 7}));
    records.push_back(entry(1200, 1, {9999, 4, 8}));
    std::ranges::sort(records, [](const auto &a, const auto &b) { return a.key < b.key; });
    create(records);
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(8U * 1024U * 1024U);
    graph::utils::OwnedAllocationScope scope(memory);
    auto pin = metadata_->openSnapshot();
    Directory map(DIRECTORY_INDEX, *btree_pages_, *metadata_);
    size_t delivered = 0; int64_t fixedPages = 0;
    graph::debug::PerfTrace::ScopedObserver observer(&fixedPages,
        [](void *state, std::string_view name, int64_t value, bool duration) noexcept {
            if (!duration && name == "native_btree.read.fixed_key_page_decodes") *static_cast<int64_t *>(state) += value;
        });
    EXPECT_THROW(map.visitAll(pin, 3000, [&](const auto &value) {
        EXPECT_NE(value.key, key(1200, 1)); ++delivered; return true;
    }), FormatException);
    EXPECT_EQ(delivered, 1199U); EXPECT_GT(fixedPages, 0);
}

TEST_F(PropertySubblockDirectoryTest, FixedKeyPreparationPropagatesOwnerCheckpointExceptions) {
    const auto original = entry(1, 1, {101, 3, 7}); create(std::span(&original, 1));
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto pin = metadata_->openSnapshot();
        BTreeIndex index(DIRECTORY_INDEX, *btree_pages_, *metadata_, BTreeLeafValueLayout::BLVL_EMPTY);
        class OwnerCheck final : public CancellationCheckpoint {
        public:
            mutable bool armed = false;
            void check() const override {
                if (armed) { armed = false; throw graph::query::execution::MaterializedByteLimitExceeded(); }
            }
        } checkpoint;
        graph::debug::PerfTrace::ScopedObserver observer(&checkpoint,
            [](void *state, std::string_view name, int64_t, bool duration) noexcept {
                if (!duration && name == "native_btree.read.fixed_key_cursor_preparation")
                    static_cast<OwnerCheck *>(state)->armed = true;
            });
        EXPECT_THROW(static_cast<void>(index.openFixedKeyRangeCursor(pin, std::nullopt, 2,
            Directory::Record{}.size(), BufferPoolReadHint::BPRH_NORMAL, false, &checkpoint)),
            graph::query::execution::MaterializedByteLimitExceeded);
    }
    EXPECT_EQ(memory->used(), 0U);
}
