#include "BuildingScanMembershipFixture.hpp"
#include "storage/relocatable/property/HybridPropertyReadContextTestAccess.hpp"
#include "graph/storage/relocatable/schema/SystemCatalog.hpp"
#include <set>

#if ZYX_EXPERIMENTAL_SCAN_PATH_FORMAT
namespace {
class BuildingScanFieldCohortTest : public BuildingScanMembershipTest {
protected:
    using Budget = graph::query::execution::MaterializedByteBudget;
    std::shared_ptr<Budget> source = std::make_shared<Budget>(1024U * 1024U);
    size_t count = 512, extra = 0;
    bool hole = false, heterogeneous = false;
    static NativeGraphWritePlannerOptions placement() {
        NativeGraphWritePlannerOptions options;options.first_slot=PhysicalSlotId(40);options.last_slot=PhysicalSlotId(511);
        options.property_roots={PhysicalSlotId(40),PhysicalSlotId(511),989};return options;
    }
    PropertyCollectionRoot row(uint64_t id) const {
        PropertyCollectionRoot result{id,graph::Node::typeId,{}};
        if(heterogeneous) {
            if((id-1)%64>=48)return result;
            for(uint32_t i=1;i<=10;++i) {
                const auto token=uint32_t((id-1)/64)*10+i;
                result.entries.push_back({token,graph::PropertyValue(int64_t(id*100+token))});
            }
            return result;
        }
        if(hole && id>=65 && id<=128)return result;
        using graph::PropertyValue;
        result.entries={{1,id%17==0?PropertyValue():PropertyValue(double(id))},
            {2,PropertyValue(int64_t(id%80))},{3,PropertyValue(id%2?"US":"DE")},
            {4,PropertyValue("user-"+std::to_string(100000+id))}};
        if(id%4==0)result.entries.push_back({5,PropertyValue(int64_t(id))});
        else if(id%4==1)result.entries.push_back({5,PropertyValue(std::string("a\0b",3))});
        else if(id%4==2)result.entries.push_back({5,PropertyValue()});
        for(size_t i=0;i<extra;++i)result.entries.push_back({uint32_t(6+i),PropertyValue(int64_t(id+i))});
        return result;
    }
    void seed(size_t nodes=512,bool gap=false,size_t wide=0) {
        count=nodes;hole=gap;extra=wide;initialize(true,true);
        LargeValueStore values(io,*metadata);PropertyCollectionRootStore properties(*pages,values,*metadata);
        BTreePageStore trees(*pages,*metadata);NativeGraphWritePlanner planner(*metadata,*entities,*labels,properties,trees,placement());
        auto tx=planner.beginTransaction();SystemCatalog catalog(trees,*metadata);auto schema=catalog.beginWrite(tx);
        uint32_t token=0;for(const auto *name:{"score","age","country","id","mixed"})ASSERT_EQ(schema.ensurePropertyToken(name),++token);
        for(size_t i=0;i<extra;++i)ASSERT_EQ(schema.ensurePropertyToken("wide"+std::to_string(i)),++token);
        for(size_t i=0;i<count;++i){const auto id=tx.createNode({});auto expected=row(id);if(!expected.entries.empty())tx.replaceNodeProperty(std::move(expected));}
        schema.setGraphCardinalityStatistics({uint64_t(count),0});schema.finish();ASSERT_TRUE(tx.commit().committed());
        static_cast<void>(ScanBuildingImageBuilder::populatePrimaryMembership(io,*metadata,source,PhysicalSlotId(40),PhysicalSlotId(511),989));
        ASSERT_EQ(source->used(),0U);
    }
    auto build(uint8_t width=8,const CancellationCheckpoint *cancel=nullptr) {
        return ScanBuildingImageBuilder::constructFieldsForBuilding(io,*metadata,source,
            PhysicalSlotId(40),PhysicalSlotId(511),989,cancel,{},width);
    }
    void verify(RelocatableMetadataStore &store,const MetadataSnapshotPin &pin,uint64_t changed=0,bool demoted=false) {
        graph::utils::OwnedAllocationScope allocation(source);
        SlottedPagePool readPages(io,store,SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        LargeValueStore values(io,store,SharedPagePoolOpenMode::SPPOM_READ_ONLY);PropertyCollectionRootStore roots(readPages,values,store);
        auto cursor=roots.openScanCursor(pin);
        for(uint64_t id=1;id<=count;++id){auto expected=row(id);if(id==changed)expected.entries[0].value=graph::PropertyValue(999.0);
            if(demoted && id<=17)expected.entries[0].value=graph::PropertyValue(false);
            auto actual=cursor.readEntityOptional(pin,{EntityPropertyOwnerKind::EPOK_NODE,id});
            if(expected.entries.empty())EXPECT_FALSE(actual);else {ASSERT_TRUE(actual);EXPECT_EQ(*actual,expected)<<id;}}
    }
    std::pair<size_t,size_t> coverage(RelocatableMetadataStore &store,const MetadataSnapshotPin &pin) {
        graph::utils::OwnedAllocationScope allocation(source);
        SlottedPagePool readPages(io,store,SharedPagePoolOpenMode::SPPOM_READ_ONLY);HybridPropertyReadContext context(io,readPages,store,pin);
        std::set<uint64_t> columns,all;
        for(uint64_t first=1;first<=count;first+=64){auto group=HybridPropertyReadContextTestAccess::group(context,{EntityPropertyOwnerKind::EPOK_NODE,first});
            EXPECT_TRUE(group);if(!group)continue;const auto matrix=HybridPropertyReadContextTestAccess::membership(context,*group);
            const auto ordinal=matrix.ordinal(1);EXPECT_TRUE(ordinal);if(!ordinal)continue;
            columns.insert(matrix.locators[*ordinal].page_id.value());all.insert(matrix.locators[*ordinal].page_id.value());
            all.insert(group->column_membership.page_id.value());}
        return {columns.size(),all.size()};
    }
    void stage(NativeGraphWriteTransaction &tx,const CancellationCheckpoint *cancel=nullptr) {
        tx.stageHybridPropertyConversion(EntityPropertyOwnerKind::EPOK_NODE,1,source,cancel,true,
            NativeHybridPropertyConversionDirection::NHPCD_ROW_TO_HYBRID,true,{},8,true);
    }
};
TEST_F(BuildingScanFieldCohortTest, PacksSameFieldAcrossGroupsAndPreservesTheOldRowPin) {
    seed();auto old=metadata->openSnapshot();const auto result=build();
    EXPECT_EQ(result.examined_groups,8U);EXPECT_EQ(result.converted_groups,8U);EXPECT_EQ(result.row_fallback_groups,0U);
    auto current=metadata->openSnapshot();verify(*metadata,old);verify(*metadata,current);
    const auto [columnPages,allPages]=coverage(*metadata,current);EXPECT_EQ(columnPages,2U);EXPECT_LE(allPages,5U);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io,*metadata,source).scan());
    auto reopened=RelocatableMetadataStore::openReadOnly(io);auto pin=reopened->openSnapshot();verify(*reopened,pin);
    EXPECT_EQ(coverage(*reopened,pin).first,2U);
}
TEST_F(BuildingScanFieldCohortTest, SharedPageMutationKeepsOtherGroupsAndOldValues) {
    seed();static_cast<void>(build());auto old=metadata->openSnapshot();const auto root=old.superblock();
    const std::array<NativeHybridPropertyColumnReplacement,1> cells{{{{EntityPropertyOwnerKind::EPOK_NODE,1},1,
        graph::OwnedPropertyValue(graph::PropertyValue(999.0))}}};
    NativeHybridPropertyColumnWrite write{root.database_id,root.generation,root.hybrid_property_descriptor_root,
        cells,source,PhysicalSlotId(40),PhysicalSlotId(511),989};write.field_lifecycle=true;
    static_cast<void>(metadata->mutateHybridPropertyColumns(write,LogSequenceNumber(root.checkpoint_lsn.value()+1)));
    auto current=metadata->openSnapshot();verify(*metadata,old);verify(*metadata,current,1);
    {
        graph::utils::OwnedAllocationScope allocation(source);
        SlottedPagePool readPages(io,*metadata,SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        HybridPropertyReadContext context(io,readPages,*metadata,current);
        const auto group=HybridPropertyReadContextTestAccess::group(context,{EntityPropertyOwnerKind::EPOK_NODE,1});
        ASSERT_TRUE(group);
        ASSERT_TRUE(HybridPropertyReadContextTestAccess::membership(context,*group).ordinal(1));
    }
    // Seventeen type changes take the original scalar column below its
    // eligibility bound. Demotion must erase only this group's shared slot.
    std::vector<NativeHybridPropertyColumnReplacement> replacements;
    for(uint64_t id=1;id<=17;++id)replacements.push_back({{EntityPropertyOwnerKind::EPOK_NODE,id},1,
        graph::OwnedPropertyValue(graph::PropertyValue(false))});
    const auto pointRoot=current.superblock();
    NativeHybridPropertyColumnWrite mixed{pointRoot.database_id,pointRoot.generation,pointRoot.hybrid_property_descriptor_root,
        replacements,source,PhysicalSlotId(40),PhysicalSlotId(511),989};mixed.field_lifecycle=true;
    static_cast<void>(metadata->mutateHybridPropertyColumns(mixed,LogSequenceNumber(pointRoot.checkpoint_lsn.value()+1)));
    auto changed=metadata->openSnapshot();verify(*metadata,old);verify(*metadata,current,1);verify(*metadata,changed,0,true);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io,*metadata,source).scan());
    {
        graph::utils::OwnedAllocationScope allocation(source);
        SlottedPagePool readPages(io,*metadata,SharedPagePoolOpenMode::SPPOM_READ_ONLY);HybridPropertyReadContext context(io,readPages,*metadata,changed);
        auto group=HybridPropertyReadContextTestAccess::group(context,{EntityPropertyOwnerKind::EPOK_NODE,1});ASSERT_TRUE(group);
        EXPECT_FALSE(HybridPropertyReadContextTestAccess::membership(context,*group).ordinal(1));
    }
    auto reopened=RelocatableMetadataStore::openReadOnly(io);auto pin=reopened->openSnapshot();verify(*reopened,pin,0,true);
}
TEST_F(BuildingScanFieldCohortTest, DeletingOneWholeGroupPreservesSharedNeighborsAndOldPin) {
    seed();static_cast<void>(build());auto old=metadata->openSnapshot();
    LargeValueStore values(io,*metadata,SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    PropertyCollectionRootStore properties(*pages,values,*metadata);BTreePageStore trees(*pages,*metadata);
    NativeGraphWritePlanner planner(*metadata,*entities,*labels,properties,trees,placement());
    {
        graph::utils::OwnedAllocationScope allocation(source);
        auto tx=planner.beginTransaction();
        // Use the native owner-deletion coordinator, as the logical batch
        // executor does. Raw ROW erase staging is intentionally unadapted.
        std::array<EntityPropertyKey,64> owners{};
        for(uint64_t id=1;id<=64;++id)owners[id-1]={EntityPropertyOwnerKind::EPOK_NODE,id};
        tx.stageHybridPropertyOwnerDeletion(owners,source);
        ASSERT_TRUE(tx.commit().committed());
    }
    const auto check=[&](RelocatableMetadataStore &store,const MetadataSnapshotPin &pin,bool deleted) {
        graph::utils::OwnedAllocationScope allocation(source);
        SlottedPagePool readPages(io,store,SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        LargeValueStore readValues(io,store,SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        PropertyCollectionRootStore roots(readPages,readValues,store);auto cursor=roots.openScanCursor(pin);
        for(uint64_t id=1;id<=count;++id) {
            const bool live=!deleted || id>64;
            EXPECT_EQ(bool(store.findNodePageLocationOptional(pin,id)),live)<<id;
            const auto actual=cursor.readEntityOptional(pin,{EntityPropertyOwnerKind::EPOK_NODE,id});
            if(!live)EXPECT_FALSE(actual)<<id;
            else {ASSERT_TRUE(actual)<<id;EXPECT_EQ(*actual,row(id))<<id;}
        }
    };
    auto current=metadata->openSnapshot();check(*metadata,old,false);check(*metadata,current,true);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io,*metadata,source).scan());
    auto reopened=RelocatableMetadataStore::openReadOnly(io);auto pin=reopened->openSnapshot();check(*reopened,pin,true);
}
TEST_F(BuildingScanFieldCohortTest, CancellationAfterBorrowingThePriorArenaPublishesNothing) {
    struct Cancel final:CancellationCheckpoint{bool stop=false;void check()const override{if(stop)throw std::runtime_error("stop cohort");}}cancel;
    seed();const auto before=metadata->openSnapshot().superblock();io->resetStats();
    {
        graph::debug::PerfTrace::ScopedObserver observer(&cancel,[](void *p,std::string_view key,int64_t value,bool)noexcept{
            if(key=="native_graph.hybrid.field_major.column_destination_reuses" && value)static_cast<Cancel*>(p)->stop=true;});
        EXPECT_THROW((void)build(8,&cancel),std::runtime_error);
    }
    EXPECT_TRUE(cancel.stop);EXPECT_EQ(metadata->openSnapshot().superblock(),before);EXPECT_EQ(io->stats().write_calls,0U);
    EXPECT_EQ(source->used(),0U);cancel.stop=false;EXPECT_EQ(build().converted_groups,8U);
}
TEST_F(BuildingScanFieldCohortTest, BudgetRefusalAfterArenaTransferRefundsAllPrivateState) {
    seed();const auto before=metadata->openSnapshot().superblock();io->resetStats();
    struct Injection{std::shared_ptr<Budget> budget;graph::utils::OwnedHeapReservation held;bool fired=false,failed=false;
        explicit Injection(std::shared_ptr<Budget> b):budget(b),held(b){}}injection(source);
    {
        graph::debug::PerfTrace::ScopedObserver observer(&injection,[](void *p,std::string_view key,int64_t value,bool)noexcept{
            auto &state=*static_cast<Injection*>(p);if(key!="native_graph.hybrid.field_major.column_destination_reuses"||!value||state.fired)return;
            state.fired=true;try{const auto available=state.budget->available();if(available>1024)state.held.set(available-1024);else state.failed=true;}catch(...){state.failed=true;}});
        EXPECT_THROW((void)build(),graph::query::execution::MaterializedByteLimitExceeded);
    }
    EXPECT_TRUE(injection.fired);EXPECT_FALSE(injection.failed);EXPECT_EQ(metadata->openSnapshot().superblock(),before);
    EXPECT_EQ(io->stats().write_calls,0U);EXPECT_EQ(source->used(),injection.held.bytes());injection.held.set(0U);EXPECT_EQ(source->used(),0U);
    EXPECT_EQ(build().converted_groups,8U);
}
TEST_F(BuildingScanFieldCohortTest, PublicationRecoverySelectsAllEightGroupsOrNone) {
    for(auto point:{BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE,BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE}){
        seed();const auto before=metadata->openSnapshot().superblock();
        LargeValueStore values(io,*metadata,SharedPagePoolOpenMode::SPPOM_READ_ONLY);PropertyCollectionRootStore properties(*pages,values,*metadata);BTreePageStore trees(*pages,*metadata);
        NativeGraphWritePlanner planner(*metadata,*entities,*labels,properties,trees,placement());auto tx=planner.beginTransaction();stage(tx);
        MetadataPublishOptions options;options.bootstrap_failpoint=point;EXPECT_THROW((void)tx.commit(options),BootstrapPublishInterrupted);
        auto recovered=RelocatableMetadataStore::openReadOnly(io);auto pin=recovered->openSnapshot();verify(*recovered,pin);
        const bool published=point==BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE;
        EXPECT_EQ(pin.superblock()==before,!published);
        graph::utils::OwnedAllocationScope allocation(source);SlottedPagePool readPages(io,*recovered,SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        HybridPropertyReadContext context(io,readPages,*recovered,pin);size_t groups=0;
        for(uint64_t id=1;id<=512;id+=64)groups+=bool(HybridPropertyReadContextTestAccess::group(context,{EntityPropertyOwnerKind::EPOK_NODE,id}));
        EXPECT_EQ(groups,published?8U:0U);
    }
}
TEST_F(BuildingScanFieldCohortTest, HolesAndPartialGroupsKeepExactValuesAndValidateWidth) {
    seed(511,true);const auto before=metadata->openSnapshot().superblock();
    EXPECT_THROW((void)build(0),std::invalid_argument);
    EXPECT_THROW((void)build(9),std::invalid_argument);
    EXPECT_EQ(metadata->openSnapshot().superblock(),before);
    const auto result=build();EXPECT_EQ(result.converted_groups,7U);auto current=metadata->openSnapshot();verify(*metadata,current);
}
TEST_F(BuildingScanFieldCohortTest, OverWideSourceRetainsItsRowRepresentationWithinTheSameBudget) {
    seed(64,false,70);const auto before=metadata->openSnapshot().superblock();io->resetStats();
    const auto result=build();EXPECT_EQ(result.converted_groups,0U);EXPECT_EQ(result.row_fallback_groups,1U);
    EXPECT_EQ(metadata->openSnapshot().superblock(),before);EXPECT_EQ(io->stats().write_calls,0U);
    auto pin=metadata->openSnapshot();verify(*metadata,pin);EXPECT_EQ(source->used(),0U);
}
TEST_F(BuildingScanFieldCohortTest, HeterogeneousCohortUsesBoundedSpillAndPreservesMissingFields) {
    // Eight ten-field schemas with 48 present owners each retain 3840 actual
    // memberships, below the original 4096 bound, but exceed 64 token lanes.
    heterogeneous=true;seed(512,false,75);auto old=metadata->openSnapshot();
    const auto result=build();EXPECT_EQ(result.converted_groups,8U);EXPECT_EQ(result.row_fallback_groups,0U);
    auto current=metadata->openSnapshot();verify(*metadata,old);verify(*metadata,current);
    {
        graph::utils::OwnedAllocationScope allocation(source);
        SlottedPagePool readPages(io,*metadata,SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        HybridPropertyReadContext context(io,readPages,*metadata,current);
        auto group=HybridPropertyReadContextTestAccess::group(context,{EntityPropertyOwnerKind::EPOK_NODE,385});
        ASSERT_TRUE(group);const auto matrix=HybridPropertyReadContextTestAccess::membership(context,*group);
        std::set<uint64_t> overflowPages;
        for(uint32_t token=65;token<=70;++token) {
            const auto ordinal=matrix.ordinal(token);ASSERT_TRUE(ordinal);
            overflowPages.insert(matrix.locators[*ordinal].page_id.value());
        }
        EXPECT_EQ(overflowPages.size(),1U); // Actual cross-token spill, not 80 dedicated lanes.
    }
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io,*metadata,source).scan());
    auto reopened=RelocatableMetadataStore::openReadOnly(io);auto pin=reopened->openSnapshot();verify(*reopened,pin);
}
TEST_F(BuildingScanFieldCohortTest, SharedPoolRetirementWaitsForOldPinThenReusesItsPhysicalSlot) {
    seed();static_cast<void>(build());auto old=metadata->openSnapshot();
    PageId scorePage;CatalogEntry originalPool;
    {
        graph::utils::OwnedAllocationScope allocation(source);
        SlottedPagePool readPages(io,*metadata,SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        HybridPropertyReadContext context(io,readPages,*metadata,old);
        const auto group=HybridPropertyReadContextTestAccess::group(context,{EntityPropertyOwnerKind::EPOK_NODE,1});
        ASSERT_TRUE(group);const auto matrix=HybridPropertyReadContextTestAccess::membership(context,*group);
        const auto ordinal=matrix.ordinal(1);ASSERT_TRUE(ordinal);scorePage=matrix.locators[*ordinal].page_id;
        originalPool=metadata->findCatalog(old,metadata->findPageLocation(old,scorePage).segment_id);
    }
    const auto before=old.superblock();
    NativeHybridPropertyPoolRelocation move{before.database_id,before.generation,
        before.hybrid_property_descriptor_root,originalPool.segment_id,PhysicalSlotId(5),PhysicalSlotId(39),source};
    static_cast<void>(metadata->relocateHybridPropertyFieldPool(move,LogSequenceNumber(before.checkpoint_lsn.value()+1)));
    auto moved=metadata->openSnapshot();verify(*metadata,old);verify(*metadata,moved);
    const auto movedLocation=metadata->findPageLocation(moved,scorePage);
    EXPECT_NE(movedLocation.segment_id,originalPool.segment_id);
    EXPECT_EQ(metadata->findPageLocation(old,scorePage).segment_id,originalPool.segment_id);
    const CatalogSegmentRetirementRequest retirement{originalPool.segment_id,originalPool.generation};
    const auto protectedRoot=moved.superblock();
    EXPECT_FALSE(metadata->tryReleaseRetiredCatalogSegments(std::span(&retirement,1),
        LogSequenceNumber(protectedRoot.checkpoint_lsn.value()+1)));
    EXPECT_EQ(metadata->openSnapshot().superblock(),protectedRoot);
    old.release();moved.release();
    ASSERT_TRUE(metadata->tryReleaseRetiredCatalogSegments(std::span(&retirement,1),
        LogSequenceNumber(protectedRoot.checkpoint_lsn.value()+1)));
    auto reusable=metadata->openSnapshot();
    EXPECT_FALSE(metadata->isPhysicalSlotAllocated(reusable,originalPool.physical_slot));
    const auto reuseRoot=reusable.superblock();
    NativeHybridPropertyPoolRelocation reuse{reuseRoot.database_id,reuseRoot.generation,
        reuseRoot.hybrid_property_descriptor_root,movedLocation.segment_id,
        originalPool.physical_slot,originalPool.physical_slot,source};
    static_cast<void>(metadata->relocateHybridPropertyFieldPool(reuse,LogSequenceNumber(reuseRoot.checkpoint_lsn.value()+1)));
    auto current=metadata->openSnapshot();
    const auto currentLocation=metadata->findPageLocation(current,scorePage);
    EXPECT_EQ(metadata->findCatalog(current,currentLocation.segment_id).physical_slot,originalPool.physical_slot);
    verify(*metadata,reusable);verify(*metadata,current);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io,*metadata,source).scan());
    auto reopened=RelocatableMetadataStore::openReadOnly(io);auto pin=reopened->openSnapshot();verify(*reopened,pin);
}
} // namespace
#endif
