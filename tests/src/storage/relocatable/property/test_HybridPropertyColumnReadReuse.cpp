#include "storage/relocatable/property/HybridPropertyOrdinaryConversionTestFixture.hpp"
using namespace graph::storage;
using graph::PropertyValue;
using graph::query::execution::MaterializedByteBudget;
#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
namespace {
class HybridPropertyColumnReadReuseTest : public graph::storage::test::HybridPropertyOrdinaryConversionTestFixture {
protected:
    void convert(const std::shared_ptr<MaterializedByteBudget> &memory) {
        auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true;
        conversion.conversion->compact_column_memberships = true; run(conversion);
    }
};
}
TEST_F(HybridPropertyColumnReadReuseTest, SelectedOwnersDecodeOneColumnAndIntegrityStillReadsItsSource) {
    seed(NODE, 64, 2);
    auto workspace = std::make_shared<MaterializedByteBudget>(1024 * 1024); ASSERT_NO_THROW(convert(workspace));
    const auto pin = metadata_->openSnapshot();
    auto retained = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    struct Work { int64_t decodes = 0, hits = 0; } work;
    graph::debug::PerfTrace::ScopedObserver observer(&work, [](void *owner, std::string_view key, int64_t value, bool) noexcept {
        auto &w = *static_cast<Work *>(owner);
        if (key == "native_graph.hybrid.column_decodes") w.decodes += value;
        if (key == "native_graph.hybrid.column_cache_hits") w.hits += value;
    });
    {
        graph::utils::OwnedAllocationScope scope(retained);
        HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, pin);
        const auto read = [&](uint64_t owner) {
            bool delivered = false;
            EXPECT_EQ(context.visitField({NODE, owner}, 1, [&](const auto &cell) {
                delivered = true; EXPECT_EQ(cell.word, owner); EXPECT_FALSE(cell.is_null);
            }, [](const auto &) { ADD_FAILURE() << "primitive column routed through extension"; }),
                HybridPropertyReadContext::FieldRoute::HFR_FOUND);
            EXPECT_TRUE(delivered);
        };
        for (uint64_t owner = 1; owner <= 64; ++owner) read(owner);
        EXPECT_EQ(work.decodes, 1); EXPECT_EQ(work.hits, 63);
        {
            StorageIO::SequentialReadScope integrity(*io_); read(1); read(2);
        }
        EXPECT_EQ(work.decodes, 3); EXPECT_EQ(work.hits, 63);
        EXPECT_NO_THROW((void)HybridPropertyDirectoryIntegrity::verify(context, pin, workspace));
        EXPECT_GT(retained->used(), 0U); EXPECT_EQ(workspace->used(), 0U);
    }
    EXPECT_EQ(retained->used(), 0U);
}
TEST_F(HybridPropertyColumnReadReuseTest, ReentrantColumnVisitorOwnsItsSourceAcrossCacheEviction) {
    seed(NODE, 64, 10);
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); ASSERT_NO_THROW(convert(memory));
    const auto pin = metadata_->openSnapshot();
    graph::utils::OwnedAllocationScope scope(memory);
    HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, pin);
    const auto scalar = [](const auto &) {};
    const auto extension = [](const auto &) { ADD_FAILURE() << "expected a primitive column"; };
    bool visited = false;
    EXPECT_EQ(context.visitField({NODE, 1}, 1, scalar, extension, {}, {},
        [&](const auto &, const auto &column, const auto &, const auto &) {
            visited = true; const auto original = column;
            for (uint32_t token = 2; token <= 10; ++token)
                EXPECT_EQ(context.visitField({NODE, 1}, token, scalar, extension), HybridPropertyReadContext::FieldRoute::HFR_FOUND);
            EXPECT_EQ(column, original); EXPECT_EQ(column.identity.token, 1U);
        }), HybridPropertyReadContext::FieldRoute::HFR_FOUND);
    EXPECT_TRUE(visited);
}
TEST_F(HybridPropertyColumnReadReuseTest, OptionalCachePressureUsesUncachedReadsAndRefundsItsOriginalOwner) {
    seed(NODE, 64, 2);
    auto workspace = std::make_shared<MaterializedByteBudget>(1024 * 1024); ASSERT_NO_THROW(convert(workspace));
    const auto pin = metadata_->openSnapshot();
    auto retained = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    std::unique_ptr<HybridPropertyReadContext> context;
    {
        graph::utils::OwnedAllocationScope scope(retained);
        context = std::make_unique<HybridPropertyReadContext>(io_, *slotted_pages_, *metadata_, pin);
    }
    {
        graph::utils::OwnedHeapReservation pressure(retained, retained->available() - 128U);
        const auto charged = retained->used();
        for (uint64_t owner : {1U, 2U}) {
            bool delivered = false;
            EXPECT_EQ(context->visitField({NODE, owner}, 1, [&](const auto &cell) {
                delivered = true; EXPECT_EQ(cell.word, owner);
            }, [](const auto &) { ADD_FAILURE() << "expected column"; }), HybridPropertyReadContext::FieldRoute::HFR_FOUND);
            EXPECT_TRUE(delivered); EXPECT_EQ(retained->used(), charged);
        }
    }
    context.reset(); EXPECT_EQ(retained->used(), 0U);
}
TEST_F(HybridPropertyColumnReadReuseTest, WriteEpochReauthenticatesPinnedColumnAndCancellationPrecedesHits) {
    seed(NODE, 64, 2);
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); ASSERT_NO_THROW(convert(memory));
    const auto pin = metadata_->openSnapshot();
    struct Cancelled : std::runtime_error { Cancelled() : std::runtime_error("cancelled column read") {} };
    struct Cancellation final : CancellationCheckpoint {
        bool cancelled = false;
        void check() const override { if (cancelled) throw Cancelled(); }
    } cancellation;
    int64_t decodes = 0;
    graph::debug::PerfTrace::ScopedObserver observer(&decodes, [](void *owner, std::string_view key, int64_t value, bool) noexcept {
        if (key == "native_graph.hybrid.column_decodes") *static_cast<int64_t *>(owner) += value;
    });
    graph::utils::OwnedAllocationScope scope(memory);
    HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, pin, &cancellation);
    const auto read = [&] {
        return context.visitField({NODE, 1}, 1, [](const auto &cell) { EXPECT_EQ(cell.word, 1U); }, [](const auto &) { ADD_FAILURE(); });
    };
    ASSERT_EQ(read(), HybridPropertyReadContext::FieldRoute::HFR_FOUND);
    const auto before = metadata_->openSnapshot().superblock(); const auto epoch = io_->dataMutationEpoch();
    const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{
        {{NODE, 1}, 1, graph::OwnedPropertyValue(PropertyValue(int64_t(777)))}}};
    NativeHybridPropertyColumnWrite change{before.database_id, before.generation, before.hybrid_property_descriptor_root,
        cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 903, nullptr, true};
    ASSERT_NO_THROW((void)metadata_->mutateHybridPropertyColumns(change, LogSequenceNumber(before.checkpoint_lsn.value() + 1)));
    ASSERT_GT(io_->dataMutationEpoch(), epoch);
    const auto afterWrite = decodes;
    EXPECT_EQ(read(), HybridPropertyReadContext::FieldRoute::HFR_FOUND); EXPECT_EQ(decodes, afterWrite + 1);
    cancellation.cancelled = true;
    EXPECT_THROW((void)read(), Cancelled); EXPECT_EQ(decodes, afterWrite + 1);
    cancellation.cancelled = false;
    EXPECT_EQ(read(), HybridPropertyReadContext::FieldRoute::HFR_FOUND); EXPECT_EQ(decodes, afterWrite + 1);
}
#endif
