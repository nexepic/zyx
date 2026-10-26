#include "storage/relocatable/property/HybridPropertyOrdinaryConversionTestFixture.hpp"
#include "graph/storage/relocatable/property/HybridPropertyValueReader.hpp"

using namespace graph::storage;
using graph::PropertyValue;
using graph::OwnedPropertyValue;
using graph::query::execution::MaterializedByteBudget;
namespace {
class HybridPropertyInlineMutationTest : public graph::storage::test::HybridPropertyOrdinaryConversionTestFixture {
protected:
    NativeGraphWritePlannerOptions placement() {
        NativeGraphWritePlannerOptions options; options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(80);
        options.entity_pool_owner_id = 901; options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 903}; return options;
    }
    Superblock write(std::span<const NativeHybridPropertyColumnReplacement> cells,
        std::shared_ptr<std::pmr::memory_resource> memory, const MetadataPublishOptions &options = {}) {
        const auto before = metadata_->openSnapshot().superblock();
        NativeHybridPropertyColumnWrite request{before.database_id, before.generation, before.hybrid_property_descriptor_root,
            cells, std::move(memory), PhysicalSlotId(40), PhysicalSlotId(80), 903, nullptr, true};
        return metadata_->mutateHybridPropertyColumns(request, LogSequenceNumber(before.checkpoint_lsn.value() + 1), options);
    }
    void convert(EntityPropertyOwnerKind kind, const std::shared_ptr<MaterializedByteBudget> &memory) {
        auto conversion = request(kind, memory); conversion.conversion->publish_candidate = true; run(conversion);
    }
};
}
TEST_F(HybridPropertyInlineMutationTest, DisabledOrUnconvertedComplexWritesAreRejectedWithoutIo) {
    seed(NODE, 2, 1); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    const auto before = metadata_->openSnapshot().superblock(); const auto epoch = io_->dataMutationEpoch();
    const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, 1}, 1, OwnedPropertyValue(PropertyValue("abc"))}}};
    EXPECT_THROW((void)write(cells, memory), FormatException);
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch); EXPECT_EQ(memory->used(), 0U);
}
#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
TEST_F(HybridPropertyInlineMutationTest, ComplexReplacementDemotesOnlyItsColumnAndKeepsOldPinsAndReopen) {
    seed(NODE); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); ASSERT_NO_THROW(convert(NODE, memory));
    const auto old = metadata_->openSnapshot(); const auto original = properties_->readEntity(old, {NODE, 1});
    PropertyValue::MapType map{{"list", PropertyValue(std::vector<PropertyValue>{PropertyValue("nested"), PropertyValue(), PropertyValue(int64_t(7))})},
        {"bytes", PropertyValue(PropertyValue::Bytes{std::byte{0}, std::byte{255}})}};
    const PropertyValue replacement(map);
    const std::array<NativeHybridPropertyColumnReplacement, 2> cells{{
        {{NODE, 1}, 1, OwnedPropertyValue(replacement)}, {{NODE, 49}, 3, OwnedPropertyValue(PropertyValue("new field"))}}};
    ASSERT_NO_THROW((void)write(cells, memory)); EXPECT_EQ(memory->used(), 0U);
    const auto pin = metadata_->openSnapshot(); HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, pin);
    const auto report = HybridPropertyDirectoryIntegrity::verify(context, pin, memory);
    EXPECT_EQ(report.columns, 1U); EXPECT_EQ(report.extensions, 49U); EXPECT_EQ(report.memberships, 97U);
    EXPECT_EQ(properties_->readEntity(pin, {NODE, 1}).entries[0].value, replacement);
    EXPECT_EQ(properties_->readEntity(pin, {NODE, 2}), properties_->readEntity(old, {NODE, 2}));
    EXPECT_EQ(properties_->readEntity(old, {NODE, 1}), original);
    auto reopened = RelocatableMetadataStore::openReadOnly(io_); SlottedPagePool pages(io_, *reopened, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    PropertyCollectionRootStore roots(pages, *reopened);
    EXPECT_EQ(roots.readEntity(reopened->openSnapshot(), {NODE, 1}).entries[0].value, replacement);
    auto inverse = request(NODE, memory); inverse.conversion->publish_candidate = true;
    inverse.conversion->direction = NativeHybridPropertyConversionDirection::NHPCD_HYBRID_TO_ROW;
    ASSERT_NO_THROW(run(inverse));
    EXPECT_EQ(properties_->readEntity(metadata_->openSnapshot(), {NODE, 1}).entries[0].value, replacement);
    EXPECT_EQ(properties_->readEntity(pin, {NODE, 1}).entries[0].value, replacement);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyInlineMutationTest, InlineKindsRoundTripAndNoopUsesRecursiveRowEquality) {
    seed(EDGE, 2, 0); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); ASSERT_NO_THROW(convert(EDGE, memory));
    std::vector<PropertyValue> values{PropertyValue(std::string(900, 'x')), PropertyValue(PropertyValue::Bytes{std::byte{42}}),
        PropertyValue(graph::TemporalDate{123}), PropertyValue(graph::TemporalDateTime{456}), PropertyValue(graph::TemporalDuration{7, 8, 9}),
        PropertyValue(std::vector<PropertyValue>{PropertyValue("a"), PropertyValue(-0.0)})};
    for (const auto &value : values) {
        const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{EDGE, 1}, 1, OwnedPropertyValue(value)}}};
        ASSERT_NO_THROW((void)write(cells, memory)); const auto before = metadata_->openSnapshot().superblock(); const auto epoch = io_->dataMutationEpoch();
        EXPECT_EQ(properties_->readEntity(metadata_->openSnapshot(), {EDGE, 1}).entries[0].value, value);
        EXPECT_EQ(write(cells, memory), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch); EXPECT_EQ(memory->used(), 0U);
    }
    constexpr uint64_t nanBits = UINT64_C(0x7ff80000abc12345);
    const PropertyValue nested(std::vector<PropertyValue>{PropertyValue(std::bit_cast<double>(nanBits))});
    const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{EDGE, 1}, 1, OwnedPropertyValue(nested)}}};
    ASSERT_NO_THROW((void)write(cells, memory)); const auto before = metadata_->openSnapshot().superblock();
    EXPECT_GT(write(cells, memory).generation.value(), before.generation.value());
    const auto root = properties_->readEntity(metadata_->openSnapshot(), {EDGE, 1});
    const auto &list = std::get<std::vector<PropertyValue>>(root.entries[0].value.getVariant());
    EXPECT_EQ(std::bit_cast<uint64_t>(std::get<double>(list[0].getVariant())), nanBits);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyInlineMutationTest, NativeStagingOwnsPayloadAcrossMoveConflictAndRollback) {
    seed(NODE, 2, 0); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); ASSERT_NO_THROW(convert(NODE, memory));
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, placement());
    std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, 1}, 1, OwnedPropertyValue(PropertyValue(std::string(900, 'a')))}}};
    auto first = planner.beginTransaction(); auto second = planner.beginTransaction();
    first.stageHybridPropertyColumns(cells, memory, nullptr, true); second.stageHybridPropertyColumns(cells, memory, nullptr, true);
    EXPECT_GT(memory->used(), 1800U); cells[0].value = OwnedPropertyValue(PropertyValue("changed caller"));
    NativeGraphWriteTransaction moved(std::move(first)); ASSERT_TRUE(moved.commit().committed());
    EXPECT_FALSE(second.commit().committed()); EXPECT_EQ(memory->used(), 0U);
    EXPECT_EQ(properties_->readEntity(metadata_->openSnapshot(), {NODE, 1}).entries[0].value, PropertyValue(std::string(900, 'a')));
    auto tx = planner.beginTransaction(); tx.stageHybridPropertyColumns(cells, memory, nullptr, true);
    EXPECT_GT(memory->used(), 0U); tx.rollback(); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyInlineMutationTest, BeyondAccountAndHeapHeavyCellsRefuseBeforeIoAndRefundStaging) {
    seed(NODE, 2, 0); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); ASSERT_NO_THROW(convert(NODE, memory));
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, placement());
    const auto before = metadata_->openSnapshot().superblock(); const auto epoch = io_->dataMutationEpoch();
    std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, 1}, 1, OwnedPropertyValue(PropertyValue(std::string(1024 * 1024, 'a')))}}};
    EXPECT_THROW((void)write(cells, memory), graph::query::execution::MaterializedByteLimitExceeded);
    auto tx = planner.beginTransaction(); EXPECT_THROW(tx.stageHybridPropertyColumns(cells, memory, nullptr, true), graph::query::execution::MaterializedByteLimitExceeded);
    EXPECT_EQ(memory->used(), 0U); EXPECT_TRUE(tx.isActive());
    cells[0].value = OwnedPropertyValue(PropertyValue(std::vector<PropertyValue>(300, PropertyValue())));
    auto tiny = std::make_shared<MaterializedByteBudget>(4096);
    EXPECT_THROW(tx.stageHybridPropertyColumns(cells, tiny, nullptr, true), graph::query::execution::MaterializedByteLimitExceeded);
    EXPECT_EQ(tiny->used(), 0U); EXPECT_TRUE(tx.isActive());
    EXPECT_THROW((void)write(cells, tiny), graph::query::execution::MaterializedByteLimitExceeded);
    EXPECT_EQ(tiny->used(), 0U);
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    cells[0].value = OwnedPropertyValue(PropertyValue("retry")); tx.stageHybridPropertyColumns(cells, memory, nullptr, true);
    ASSERT_TRUE(tx.commit().committed()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyInlineMutationTest, ComplexWriteABRecoveryAndErasePreserveAtomicFields) {
    seed(NODE, 2, 0); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); ASSERT_NO_THROW(convert(NODE, memory));
    const auto old = metadata_->openSnapshot();
    std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, 1}, 1, OwnedPropertyValue(PropertyValue("atomic"))}}};
    for (const auto point : {BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE, BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE}) {
        MetadataPublishOptions options; options.bootstrap_failpoint = point;
        EXPECT_THROW((void)write(cells, memory, options), BootstrapPublishInterrupted);
        auto recovered = RelocatableMetadataStore::openReadOnly(io_); SlottedPagePool pages(io_, *recovered, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        PropertyCollectionRootStore roots(pages, *recovered); const auto pin = recovered->openSnapshot();
        if (point == BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE)
            EXPECT_EQ(roots.readEntity(pin, {NODE, 1}).entries[0].value, PropertyValue("atomic"));
        else EXPECT_FALSE(roots.readEntityOptional(pin, {NODE, 1}));
        EXPECT_FALSE(properties_->readEntityOptional(old, {NODE, 1})); EXPECT_EQ(memory->used(), 0U);
    }
    cells[0].erase = true; ASSERT_NO_THROW((void)write(cells, memory));
    EXPECT_FALSE(properties_->readEntityOptional(metadata_->openSnapshot(), {NODE, 1}));
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyInlineMutationTest, CancellationAfterPrivatePreparationRefundsComplexPayloadAndAllowsRetry) {
    seed(NODE); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); ASSERT_NO_THROW(convert(NODE, memory));
    struct Cancelled : std::runtime_error { Cancelled() : std::runtime_error("inline mutation cancelled") {} };
    struct Cancellation final : CancellationCheckpoint {
        bool prepared = false;
        void check() const override { if (prepared) throw Cancelled(); }
    } cancellation;
    const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, 1}, 1,
        OwnedPropertyValue(PropertyValue(std::vector<PropertyValue>{PropertyValue(std::string(900, 'x'))}))}}};
    const auto before = metadata_->openSnapshot().superblock(); const auto epoch = io_->dataMutationEpoch();
    NativeHybridPropertyColumnWrite mutation{before.database_id, before.generation, before.hybrid_property_descriptor_root,
        cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 903, &cancellation, true};
    {
        graph::debug::PerfTrace::ScopedObserver observer(&cancellation,
            [](void *owner, std::string_view key, int64_t, bool) noexcept {
                if (key == "native_graph.hybrid.private_map_images") static_cast<Cancellation *>(owner)->prepared = true;
            });
        EXPECT_THROW((void)metadata_->mutateHybridPropertyColumns(mutation,
            LogSequenceNumber(before.checkpoint_lsn.value() + 1)), Cancelled);
    }
    EXPECT_TRUE(cancellation.prepared); EXPECT_EQ(memory->used(), 0U);
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    mutation.cancellation = nullptr;
    ASSERT_NO_THROW((void)metadata_->mutateHybridPropertyColumns(mutation, LogSequenceNumber(before.checkpoint_lsn.value() + 1)));
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyInlineMutationTest, RepeatedSmallWritesReusePhysicalFramesWithoutGrowingPropertyPools) {
    seed(NODE, 2, 1); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); ASSERT_NO_THROW(convert(NODE, memory));
    auto old = metadata_->openSnapshot(); const auto original = properties_->readEntity(old, {NODE, 1});
    const auto countPools = [&] {
        size_t count = 0; metadata_->visitCatalogEntries(metadata_->openSnapshot(), [&](const CatalogEntry &pool) {
            if (pool.kind == SegmentKind::SGK_PROPERTY_PAGE_POOL && pool.owner_id == 903 && pool.state == SegmentState::SGS_SEALED) ++count;
        }); return count;
    };
    const auto count = countPools(); ASSERT_EQ(count, 1U);
    for (int64_t value = 100; value < 170; ++value) {
        const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, 1}, 1, OwnedPropertyValue(PropertyValue(value))}}};
        ASSERT_NO_THROW((void)write(cells, memory));
    }
    EXPECT_EQ(countPools(), count); EXPECT_EQ(properties_->readEntity(old, {NODE, 1}), original);
    EXPECT_EQ(properties_->readEntity(metadata_->openSnapshot(), {NODE, 1}).entries[0].value, PropertyValue(int64_t(169)));
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
#endif

#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
TEST_F(HybridPropertyInlineMutationTest, PackedShortTextColumnsUseTypedSinksAndRetainUpdateDemotionAndOldPins) {
 seed(NODE,64,1);
 auto options=placement();options.property_roots.page_owner_id=902;
 NativeGraphWritePlanner writer(*metadata_,*entities_,*labels_,*properties_,*btree_pages_,options);
 auto seedText=writer.beginTransaction();std::vector<PropertyValue> expected;
 for(uint64_t id=1;id<=64;++id) {
  auto value=id==5?PropertyValue{}:id==3?PropertyValue(""):id==7?PropertyValue(std::string("\xe4\xb8\xad",3)):
      id==9?PropertyValue("1234567"):PropertyValue(id%2?std::string("a\0b",3):std::string("CN"));
  expected.push_back(value);seedText.replaceNodeProperty({id,graph::Node::typeId,{{1U,value}}});
 }
 ASSERT_TRUE(seedText.commit().committed());auto rowPin=metadata_->openSnapshot();
 auto memory=std::make_shared<MaterializedByteBudget>(1024*1024);
 auto conversion=request(NODE,memory);conversion.conversion->publish_candidate=true;
 conversion.conversion->compact_column_memberships=true;ASSERT_NO_THROW(run(conversion));
 auto columnPin=metadata_->openSnapshot();
 HybridPropertyReadContext context(io_,*slotted_pages_,*metadata_,columnPin);
 auto report=HybridPropertyDirectoryIntegrity::verify(context,columnPin,memory);
 EXPECT_EQ(report.columns,1U);EXPECT_EQ(report.extensions,0U);
 std::vector<EntityPropertyKey> owners;for(uint64_t id=1;id<=64;++id)owners.push_back({NODE,id});
 const std::array<uint32_t,1> tokens{1};size_t owningCalls=0,textCalls=0,nullCalls=0;
 const std::function<void(size_t,size_t,std::optional<PropertyCollectionRootEntry>)> owning=[&](auto,auto,auto){++owningCalls;};
 const PropertyScalarReadSink scalars=[&](size_t row,size_t,graph::PropertyType type,uint64_t word){
  ++nullCalls;EXPECT_EQ(row,4U);EXPECT_EQ(type,graph::PropertyType::NULL_TYPE);EXPECT_EQ(word,0U);
 };
 const PropertyTextReadSink texts=[&](size_t row,size_t,std::string_view value){
  ++textCalls;EXPECT_EQ(std::string(value),std::get<std::string>(expected[row].getVariant()));
 };
 PropertyColumnBatchReadSinks batches;size_t runCalls=0;
 batches.scalars=[&](auto rows,size_t col,auto type,auto words){++runCalls;for(size_t i=0;i<rows.size();++i)scalars(rows[i],col,type,words[i]);};
 batches.texts=[&](auto rows,size_t col,auto values){++runCalls;for(size_t i=0;i<rows.size();++i)texts(rows[i],col,values[i]);};
 // Output reservation must be cumulative and precede each run, with the
 // exact original bytes for short text (including embedded NUL) and NULL.
 size_t runAdmissionCalls=0, admittedOutputBytes=0, expectedOutputBytes=0;
 for(const auto &value:expected) expectedOutputBytes += sizeof(uint32_t)+CanonicalPropertyValueCodec::HEADER_BYTES+
     (value.getType()==graph::PropertyType::NULL_TYPE?0U:std::get<std::string>(value.getVariant()).size());
 const SelectedPropertyReadAdmission outputAdmission=[&](size_t bytes){++runAdmissionCalls;admittedOutputBytes+=bytes;};
 const bool valuesEnabled=graph::debug::PerfTrace::areValuesEnabled();
 struct RestoreTrace {bool enabled;~RestoreTrace(){graph::debug::PerfTrace::setValueCollectionEnabled(enabled);}} restore{valuesEnabled};
 graph::debug::PerfTrace::setValueCollectionEnabled(true);graph::debug::PerfTrace::reset();
 auto cursor=properties_->openScanCursor(columnPin);
 ASSERT_NO_THROW(cursor.readEntityPropertiesOptionalSorted(columnPin,owners,tokens,{},true,&outputAdmission,&owning,&scalars,&texts,&batches));
 const auto trace=graph::debug::PerfTrace::snapshotAndReset();
 EXPECT_EQ(owningCalls,0U);EXPECT_EQ(textCalls,63U);EXPECT_EQ(nullCalls,1U);EXPECT_EQ(runCalls,3U);
 // The cursor also admits owner/reference scratch. Test exact output accounting
 // directly at the same reader boundary using the authenticated context.
 runAdmissionCalls=admittedOutputBytes=0;
 ASSERT_TRUE(HybridPropertyValueReader::readSelected(context,columnPin,owners,tokens,{}, {},nullptr,
     &outputAdmission,&owning,&scalars,&texts,&batches));
 EXPECT_EQ(runAdmissionCalls,3U);EXPECT_EQ(admittedOutputBytes,expectedOutputBytes);
 size_t deniedOutputs=0;
 auto deniedBatches=batches;
 deniedBatches.scalars=[&](auto,size_t,auto,auto){++deniedOutputs;};
 deniedBatches.texts=[&](auto,size_t,auto){++deniedOutputs;};
 const SelectedPropertyReadAdmission denied=[](size_t){throw graph::query::execution::MaterializedByteLimitExceeded();};
 EXPECT_THROW((void)HybridPropertyValueReader::readSelected(context,columnPin,owners,tokens,{}, {},nullptr,
     &denied,&owning,&scalars,&texts,&deniedBatches),graph::query::execution::MaterializedByteLimitExceeded);
 EXPECT_EQ(deniedOutputs,0U);
 ASSERT_TRUE(trace.contains("native_graph.hybrid.grouped_column_batches"));
 EXPECT_EQ(trace.at("native_graph.hybrid.grouped_column_batches").totalValue,1);
 EXPECT_EQ(trace.at("native_graph.hybrid.grouped_owner_route_checks").totalValue,64);
 EXPECT_FALSE(trace.contains("native_graph.hybrid.owner_route_checks"));
 // Admission can decline the new group scratch before any output; the original
 // per-owner path still succeeds under this narrower boundary allowance.
 owningCalls=textCalls=nullCalls=runCalls=0;
 const SelectedPropertyReadAdmission narrow=[](size_t bytes){if(bytes>1024U)throw graph::query::execution::MaterializedByteLimitExceeded();};
 // Raw metadata fixtures have no implicit read-cache owner. Establish the
 // intended separately accounted warm cache explicitly before narrowing only
 // transient admission; otherwise this would test cold page-copy capacity.
 auto cacheMemory=std::make_shared<MaterializedByteBudget>(1024U*1024U);
 std::unique_ptr<HybridPropertyReadContext> warmContext;
 {
  graph::utils::OwnedAllocationScope cache(cacheMemory);
  warmContext=std::make_unique<HybridPropertyReadContext>(io_,*slotted_pages_,*metadata_,columnPin);
  ASSERT_TRUE(HybridPropertyValueReader::readSelected(*warmContext,columnPin,owners,tokens,{}, {},nullptr,&narrow,&owning,&scalars,&texts));
 }
 owningCalls=textCalls=nullCalls=runCalls=0;
 auto tight=std::make_shared<MaterializedByteBudget>(4096U);graph::debug::PerfTrace::reset();
 ASSERT_NO_THROW({
  // Reuse the separately accounted authenticated read context; the active
  // transient allowance is smaller than the grouped route's prepaid scratch.
  graph::utils::OwnedAllocationScope transient(tight);
  EXPECT_TRUE(HybridPropertyValueReader::readSelected(*warmContext,columnPin,owners,tokens,{}, {},nullptr,&narrow,&owning,&scalars,&texts,&batches));
 });
 EXPECT_EQ(tight->used(),0U);warmContext.reset();EXPECT_EQ(cacheMemory->used(),0U);
 const auto declined=graph::debug::PerfTrace::snapshotAndReset();
 ASSERT_TRUE(declined.contains("native_graph.hybrid.grouped_column_declined_admission"));
 EXPECT_EQ(declined.at("native_graph.hybrid.grouped_column_declined_admission").totalValue,1);
 EXPECT_FALSE(declined.contains("native_graph.hybrid.grouped_column_batches"));
 EXPECT_EQ(owningCalls,0U);EXPECT_EQ(textCalls,63U);EXPECT_EQ(nullCalls,1U);
 struct GroupCancelled final:std::runtime_error {GroupCancelled():std::runtime_error("group supply cancelled") {}};
 struct CancelAfterAuthentication final:CancellationCheckpoint {
  bool requested=false;void check()const override {if(requested)throw GroupCancelled();}
 } cancellation;
 owningCalls=textCalls=nullCalls=runCalls=0;
 auto cancelledCursor=properties_->openScanCursor(columnPin,&cancellation);
 {
  graph::debug::PerfTrace::ScopedObserver observe(&cancellation,[](void *state,std::string_view key,int64_t,bool)noexcept{
   if(key=="native_graph.hybrid.grouped_owner_route_checks")static_cast<CancelAfterAuthentication *>(state)->requested=true;
  });
  EXPECT_THROW(cancelledCursor.readEntityPropertiesOptionalSorted(columnPin,owners,tokens,{},true,nullptr,&owning,&scalars,&texts,&batches),GroupCancelled);
 }
 EXPECT_TRUE(cancellation.requested);EXPECT_EQ(owningCalls+textCalls+nullCalls+runCalls,0U);
 // A destination callback failure after group production begins must propagate,
 // rather than falling back and replaying an already consumed prefix.
 size_t failingRuns=0;auto failing=batches;
 failing.texts=[&](auto,size_t,auto){++failingRuns;throw graph::query::execution::MaterializedByteLimitExceeded();};
 EXPECT_THROW(cursor.readEntityPropertiesOptionalSorted(columnPin,owners,tokens,{},true,nullptr,&owning,&scalars,&texts,&failing),
     graph::query::execution::MaterializedByteLimitExceeded);
 EXPECT_EQ(failingRuns,1U);
 for(uint64_t id=1;id<=64;++id) {
  EXPECT_EQ(properties_->readEntity(rowPin,{NODE,id}).entries.front().value,expected[id-1]);
  EXPECT_EQ(properties_->readEntity(columnPin,{NODE,id}).entries.front().value,expected[id-1]);
 }
 const std::array<NativeHybridPropertyColumnReplacement,1> changed{{{{NODE,1},1,OwnedPropertyValue(PropertyValue("changed"))}}};
 ASSERT_NO_THROW((void)write(changed,memory));auto changedPin=metadata_->openSnapshot();
 const auto generation=changedPin.superblock().generation;
 ASSERT_NO_THROW((void)write(changed,memory));EXPECT_EQ(metadata_->openSnapshot().superblock().generation,generation);
 const std::array<NativeHybridPropertyColumnReplacement,1> retry{{{{NODE,1},1,OwnedPropertyValue(PropertyValue("retry"))}}};
 for(auto point:{BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE,BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE}) {
  MetadataPublishOptions failure;failure.bootstrap_failpoint=point;
  EXPECT_THROW((void)write(retry,memory,failure),BootstrapPublishInterrupted);
  EXPECT_EQ(metadata_->openSnapshot().superblock().generation,generation);
 }
 const std::array<NativeHybridPropertyColumnReplacement,1> empty{{{{NODE,5},1,OwnedPropertyValue(PropertyValue(""))}}};
 ASSERT_NO_THROW((void)write(empty,memory));const auto emptyGeneration=metadata_->openSnapshot().superblock().generation;
 ASSERT_NO_THROW((void)write(empty,memory));EXPECT_EQ(metadata_->openSnapshot().superblock().generation,emptyGeneration);
 EXPECT_EQ(properties_->readEntity(columnPin,{NODE,5}).entries.front().value,PropertyValue{});
 expected[4]=PropertyValue("");
 const std::array<NativeHybridPropertyColumnReplacement,1> longer{{{{NODE,1},1,OwnedPropertyValue(PropertyValue("12345678"))}}};
 ASSERT_NO_THROW((void)write(longer,memory));auto current=metadata_->openSnapshot();
 HybridPropertyReadContext demoted(io_,*slotted_pages_,*metadata_,current);
 report=HybridPropertyDirectoryIntegrity::verify(demoted,current,memory);
 EXPECT_EQ(report.columns,0U);EXPECT_EQ(report.extensions,64U);
 EXPECT_EQ(properties_->readEntity(current,{NODE,1}).entries.front().value,PropertyValue("12345678"));
 EXPECT_EQ(properties_->readEntity(changedPin,{NODE,1}).entries.front().value,PropertyValue("changed"));
 EXPECT_EQ(properties_->readEntity(columnPin,{NODE,1}).entries.front().value,expected.front());
 for(uint64_t id=2;id<=64;++id)EXPECT_EQ(properties_->readEntity(current,{NODE,id}).entries.front().value,expected[id-1]);
 EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_,*metadata_,memory).scan());
 EXPECT_EQ(memory->used(),0U);
 current.release();changedPin.release();columnPin.release();rowPin.release();
 auto readOnly=RelocatableMetadataStore::openReadOnly(io_);
 SlottedPagePool reopenedPages(io_,*readOnly,SharedPagePoolOpenMode::SPPOM_READ_ONLY);
 PropertyCollectionRootStore reopenedProperties(reopenedPages,*readOnly);
 auto reopened=readOnly->openSnapshot();
 EXPECT_EQ(reopenedProperties.readEntity(reopened,{NODE,1}).entries.front().value,PropertyValue("12345678"));
}
#endif

#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
TEST_F(HybridPropertyInlineMutationTest, GroupedColumnsPreserveSparseMembersAndUnrelatedExtensionAuthority) {
 seed(NODE,64,2);auto options=placement();options.property_roots.page_owner_id=902;
 NativeGraphWritePlanner writer(*metadata_,*entities_,*labels_,*properties_,*btree_pages_,options);
 auto replace=writer.beginTransaction();
 for(uint64_t id=1;id<=64;++id) {
  PropertyCollectionRoot root{id,graph::Node::typeId,{}};
  if(id!=1)root.entries.push_back({1U,id==2?PropertyValue{}:PropertyValue("CN")});
  root.entries.push_back({2U,PropertyValue("long other field")});replace.replaceNodeProperty(std::move(root));
 }
 ASSERT_TRUE(replace.commit().committed());
 auto memory=std::make_shared<MaterializedByteBudget>(1024*1024);
 auto conversion=request(NODE,memory);conversion.conversion->publish_candidate=true;
 conversion.conversion->compact_column_memberships=true;ASSERT_NO_THROW(run(conversion));
 auto pin=metadata_->openSnapshot();std::vector<EntityPropertyKey> owners;
 for(uint64_t id=1;id<=64;++id)owners.push_back({NODE,id});
 size_t ownCalls=0,textCalls=0,nullCalls=0,runs=0;
 std::vector<std::optional<PropertyValue>> values(64);
 const std::function<void(size_t,size_t,std::optional<PropertyCollectionRootEntry>)> owning=[&](size_t row,size_t,auto value){
  ++ownCalls;values[row]=value?std::optional<PropertyValue>(std::move(value->value)):std::nullopt;
 };
 const PropertyScalarReadSink scalar=[&](size_t row,size_t,auto type,auto word){
  ++nullCalls;EXPECT_EQ(type,graph::PropertyType::NULL_TYPE);EXPECT_EQ(word,0U);values[row]=PropertyValue{};
 };
 const PropertyTextReadSink text=[&](size_t row,size_t,std::string_view value){++textCalls;values[row]=PropertyValue(std::string(value));};
 PropertyColumnBatchReadSinks batches;
 batches.scalars=[&](auto rows,size_t col,auto type,auto words){++runs;for(size_t i=0;i<rows.size();++i)scalar(rows[i],col,type,words[i]);};
 batches.texts=[&](auto rows,size_t col,auto textValues){++runs;for(size_t i=0;i<rows.size();++i)text(rows[i],col,textValues[i]);};
 const bool enabled=graph::debug::PerfTrace::areValuesEnabled();
 struct Restore {bool enabled;~Restore(){graph::debug::PerfTrace::setValueCollectionEnabled(enabled);}} restore{enabled};
 graph::debug::PerfTrace::setValueCollectionEnabled(true);graph::debug::PerfTrace::reset();
 auto cursor=properties_->openScanCursor(pin);const std::array<uint32_t,1> country{1},other{2};
 cursor.readEntityPropertiesOptionalSorted(pin,owners,country,{},true,nullptr,&owning,&scalar,&text,&batches);
 EXPECT_EQ(ownCalls,1U);EXPECT_EQ(nullCalls,1U);EXPECT_EQ(textCalls,62U);EXPECT_EQ(runs,2U);
 EXPECT_FALSE(values[0]);ASSERT_TRUE(values[1]);EXPECT_EQ(values[1]->getType(),graph::PropertyType::NULL_TYPE);
 for(size_t i=2;i<values.size();++i)EXPECT_EQ(values[i],PropertyValue("CN"));
 const auto grouped=graph::debug::PerfTrace::snapshotAndReset();
 ASSERT_TRUE(grouped.contains("native_graph.hybrid.grouped_column_batches"));
 EXPECT_EQ(grouped.at("native_graph.hybrid.grouped_column_batches").totalValue,1);
 EXPECT_FALSE(grouped.contains("native_graph.hybrid.owner_route_checks"));
 ownCalls=textCalls=nullCalls=runs=0;
 cursor.readEntityPropertiesOptionalSorted(pin,owners,other,{},true,nullptr,&owning,&scalar,&text,&batches);
 EXPECT_EQ(ownCalls,64U);EXPECT_EQ(textCalls,0U);EXPECT_EQ(nullCalls,0U);EXPECT_EQ(runs,0U);
 for(const auto &value:values)EXPECT_EQ(value,PropertyValue("long other field"));
 const auto fallback=graph::debug::PerfTrace::snapshotAndReset();
 EXPECT_FALSE(fallback.contains("native_graph.hybrid.grouped_column_batches"));
 // Raw fixtures may disable the existing cache, causing repeated checks.
 EXPECT_GE(fallback.at("native_graph.hybrid.owner_route_checks").totalValue,64);
 const std::array<NativeHybridPropertyColumnReplacement,1> longer{{{{NODE,3},1,OwnedPropertyValue(PropertyValue("12345678"))}}};
 ASSERT_NO_THROW((void)write(longer,memory));auto current=metadata_->openSnapshot();
 for(uint64_t id=1;id<=64;++id) {
  const auto preserved=properties_->readEntityPropertyOptional(current,{NODE,id},2);ASSERT_TRUE(preserved);
  EXPECT_EQ(preserved->value,PropertyValue("long other field"));
  if(id>2) {const auto old=properties_->readEntityPropertyOptional(pin,{NODE,id},1);ASSERT_TRUE(old);EXPECT_EQ(old->value,PropertyValue("CN"));}
 }
 const auto changed=properties_->readEntityPropertyOptional(current,{NODE,3},1);ASSERT_TRUE(changed);
 EXPECT_EQ(changed->value,PropertyValue("12345678"));
 EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_,*metadata_,memory).scan());
 EXPECT_EQ(memory->used(),0U);
}
#endif
