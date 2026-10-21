#include "storage/relocatable/property/HybridPropertyOrdinaryConversionTestFixture.hpp"
using namespace graph::storage;
using graph::PropertyValue;
using graph::query::execution::MaterializedByteBudget;
namespace {
class HybridPropertyCohortAdmissionTest : public graph::storage::test::HybridPropertyOrdinaryConversionTestFixture {
protected:
    static NativeGraphWritePlannerOptions placement(bool native = false) {
        NativeGraphWritePlannerOptions result; result.first_slot = PhysicalSlotId(40); result.last_slot = PhysicalSlotId(80);
        result.entity_pool_owner_id = 901; result.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), native ? 903U : 902U};
        return result;
    }
    static PropertyCollectionRoot expected(uint64_t id, size_t fields = 64) {
        PropertyCollectionRoot result{id, graph::Node::typeId, {}};
        for (uint32_t token = 1; token <= fields; ++token) result.entries.push_back({token, PropertyValue(int64_t(id + token))});
        return result;
    }
    void prepare(size_t groups = 8, size_t fields = 64) {
        seed(NODE, groups * 64, 0);
        NativeGraphWritePlanner writer(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, placement());
        for (size_t group = 0; group < groups; ++group) {
            auto tx = writer.beginTransaction();
            for (size_t offset = 1; offset <= 64; ++offset) tx.replaceNodeProperty(expected(group * 64 + offset, fields));
            ASSERT_TRUE(tx.commit().committed());
        }
    }
    static NativeHybridPropertyAllocationAdmission admission() {
        NativeHybridPropertyAllocationAdmission result;
        result.maximum_admitted_peak_bytes = 32U * 1024U * 1024U;
        result.maximum_admitted_file_bytes = 32U * 1024U * 1024U; return result;
    }
    static void stage(NativeGraphWriteTransaction &tx, const std::shared_ptr<MaterializedByteBudget> &memory,
        uint8_t groups = 8, const CancellationCheckpoint *cancellation = nullptr) {
        tx.stageHybridPropertyConversion(NODE, 1, memory, cancellation, true,
            NativeHybridPropertyConversionDirection::NHPCD_ROW_TO_HYBRID, true, admission(), groups);
    }
};
}
TEST_F(HybridPropertyCohortAdmissionTest, DefaultFormatRefusesBeforeAnyConversionIO) {
#if !ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
    seed(NODE, 2, 1); const auto before = metadata_->openSnapshot().superblock(); const auto epoch = io_->dataMutationEpoch();
    NativeGraphWritePlanner writer(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, placement(true));
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); auto tx = writer.beginTransaction();
    EXPECT_THROW(stage(tx, memory), FormatException); tx.rollback();
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch); EXPECT_EQ(memory->used(), 0U);
#endif
}
#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
TEST_F(HybridPropertyCohortAdmissionTest, InvalidCohortEnvelopeRefusesBeforeIO) {
    seed(NODE, 2, 1); const auto before = metadata_->openSnapshot().superblock(); const auto epoch = io_->dataMutationEpoch();
    NativeGraphWritePlanner writer(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, placement(true));
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    for (uint8_t count : {uint8_t(0), uint8_t(9), uint8_t(255)}) {
        auto tx = writer.beginTransaction(); EXPECT_THROW(stage(tx, memory, count), FormatException); tx.rollback();
    }
    auto tx = writer.beginTransaction();
    EXPECT_THROW(tx.stageHybridPropertyConversion(NODE, 1, memory, nullptr, true,
        NativeHybridPropertyConversionDirection::NHPCD_ROW_TO_HYBRID, true, {}, 2), FormatException);
    EXPECT_THROW(tx.stageHybridPropertyConversion(NODE, 1, memory, nullptr, true,
        NativeHybridPropertyConversionDirection::NHPCD_ROW_TO_HYBRID, false, admission(), 2), FormatException);
    EXPECT_THROW(tx.stageHybridPropertyConversion(NODE, RSS_HYBRID_MAX_GROUP_ID, memory, nullptr, true,
        NativeHybridPropertyConversionDirection::NHPCD_ROW_TO_HYBRID, true, admission(), 2), FormatException);
    tx.rollback(); EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
    EXPECT_EQ(io_->dataMutationEpoch(), epoch); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyCohortAdmissionTest, NonColumnGroupNeverPublishesAnEarlierPreparedGroup) {
    prepare(2, 1);
    NativeGraphWritePlanner row(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, placement());
    auto changed = expected(65, 1); changed.entries.front().value = PropertyValue("mixed");
    auto change = row.beginTransaction(); change.replaceNodeProperty(changed); ASSERT_TRUE(change.commit().committed());
    const auto before = metadata_->openSnapshot().superblock();
    NativeGraphWritePlanner writer(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, placement(true));
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); auto tx = writer.beginTransaction(); stage(tx, memory, 2);
    EXPECT_THROW((void)tx.commit(), FormatException); tx.rollback();
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(memory->used(), 0U);
    for (uint64_t id = 1; id <= 128; ++id) {
        auto pin = metadata_->openSnapshot(); EXPECT_TRUE(metadata_->findEntityPropertyLocationOptional(pin, {NODE, id}));
        EXPECT_EQ(properties_->readEntity(pin, {NODE, id}), id == 65 ? changed : expected(id, 1));
    }
}
TEST_F(HybridPropertyCohortAdmissionTest, UnprofitableWholeCohortKeepsEveryRowAuthority) {
    prepare(2, 2); const auto old = metadata_->openSnapshot(); const auto before = old.superblock();
    NativeGraphWritePlanner writer(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, placement(true));
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); auto tx = writer.beginTransaction(); stage(tx, memory, 2);
    int64_t refused = 0;
    {
        graph::debug::PerfTrace::ScopedObserver observe(&refused, [](void *owner, std::string_view key, int64_t value, bool) noexcept {
            if (key == "native_graph.hybrid.allocation_admission_insufficient_saving") *static_cast<int64_t *>(owner) += value;
        });
        ASSERT_TRUE(tx.commit().committed());
    }
    ASSERT_EQ(refused, 1); EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
    for (uint64_t id = 1; id <= 128; ++id) {
        auto pin = metadata_->openSnapshot(); EXPECT_TRUE(metadata_->findEntityPropertyLocationOptional(pin, {NODE, id}));
        EXPECT_EQ(properties_->readEntity(pin, {NODE, id}), expected(id, 2));
        EXPECT_EQ(properties_->readEntity(old, {NODE, id}), expected(id, 2));
    }
    EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyCohortAdmissionTest, SharedGrantExhaustionKeepsAllRootsAndRefundsPreparation) {
    prepare(); const auto old = metadata_->openSnapshot(); const auto before = old.superblock();
    NativeGraphWritePlanner writer(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, placement(true));
    auto memory = std::make_shared<MaterializedByteBudget>(128 * 1024); auto tx = writer.beginTransaction(); stage(tx, memory);
    EXPECT_THROW((void)tx.commit(), graph::query::execution::MaterializedByteLimitExceeded); tx.rollback();
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(memory->used(), 0U);
    for (uint64_t id = 1; id <= 512; ++id) {
        auto pin = metadata_->openSnapshot(); EXPECT_TRUE(metadata_->findEntityPropertyLocationOptional(pin, {NODE, id}));
    }
    EXPECT_EQ(properties_->readEntity(old, {NODE, 1}), expected(1));
    EXPECT_EQ(properties_->readEntity(old, {NODE, 512}), expected(512));
}
TEST_F(HybridPropertyCohortAdmissionTest, CancellationAfterCompleteCostPreparationKeepsAllRowRoots) {
    struct Cancel final : CancellationCheckpoint { bool prepared = false; void check() const override {
        if (prepared) throw std::runtime_error("cohort cancelled before admission");
    }} cancelled;
    prepare(); const auto old = metadata_->openSnapshot(); const auto before = old.superblock();
    NativeGraphWritePlanner writer(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, placement(true));
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); auto tx = writer.beginTransaction(); stage(tx, memory, 8, &cancelled);
    {
        graph::debug::PerfTrace::ScopedObserver observe(&cancelled, [](void *owner, std::string_view key, int64_t value, bool) noexcept {
            if (key == "native_graph.hybrid.allocation_costs_prepared" && value) static_cast<Cancel *>(owner)->prepared = true;
        });
        EXPECT_THROW((void)tx.commit(), std::runtime_error);
    }
    ASSERT_TRUE(cancelled.prepared); tx.rollback(); EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
    for (uint64_t id = 1; id <= 512; ++id) {
        auto pin = metadata_->openSnapshot(); EXPECT_TRUE(metadata_->findEntityPropertyLocationOptional(pin, {NODE, id}));
    }
    EXPECT_EQ(properties_->readEntity(old, {NODE, 512}), expected(512)); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyCohortAdmissionTest, PartialAndCompleteInactiveSlotRecoverTheWholeCohortOrNone) {
    prepare(); const auto old = metadata_->openSnapshot(); const auto before = old.superblock();
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    for (const auto failpoint : {BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE,
                                BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE}) {
        NativeGraphWritePlanner writer(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, placement(true));
        auto tx = writer.beginTransaction(); stage(tx, memory);
        MetadataPublishOptions publication; publication.bootstrap_failpoint = failpoint;
        EXPECT_THROW((void)tx.commit(publication), BootstrapPublishInterrupted); tx.rollback();
        auto recovered = RelocatableMetadataStore::openReadOnly(io_);
        SlottedPagePool pages(io_, *recovered, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        PropertyCollectionRootStore roots(pages, *recovered); auto pin = recovered->openSnapshot();
        const bool installed = failpoint == BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE;
        EXPECT_EQ(pin.superblock().generation.value(), before.generation.value() + size_t(installed));
        EXPECT_EQ(pin.superblock().hybrid_property_descriptor_root.isValid(), installed);
        for (uint64_t id = 1; id <= 512; ++id)
            EXPECT_EQ(recovered->findEntityPropertyLocationOptional(pin, {NODE, id}).has_value(), !installed);
        for (uint64_t id : {1U, 64U, 65U, 448U, 449U, 512U}) {
            EXPECT_EQ(roots.readEntity(pin, {NODE, id}), expected(id));
            EXPECT_EQ(properties_->readEntity(old, {NODE, id}), expected(id));
        }
        EXPECT_EQ(memory->used(), 0U);
    }
}
#endif
