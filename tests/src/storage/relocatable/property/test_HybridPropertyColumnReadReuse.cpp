#include "storage/relocatable/property/HybridPropertyOrdinaryConversionTestFixture.hpp"
#include "graph/storage/relocatable/property/HybridPropertyValueReader.hpp"
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
    struct Work { int64_t decodes = 0, hits = 0, groups = 0, routes = 0; } work;
    graph::debug::PerfTrace::ScopedObserver observer(&work, [](void *owner, std::string_view key, int64_t value, bool) noexcept {
        auto &w = *static_cast<Work *>(owner);
        if (key == "native_graph.hybrid.column_decodes") w.decodes += value;
        if (key == "native_graph.hybrid.column_cache_hits") w.hits += value;
        if (key == "native_graph.hybrid.group_decodes") w.groups += value;
        if (key == "native_graph.hybrid.owner_route_checks") w.routes += value;
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
        EXPECT_EQ(work.groups, 1); EXPECT_EQ(work.routes, 64);
        {
            StorageIO::SequentialReadScope integrity(*io_); read(1); read(2);
        }
        EXPECT_EQ(work.decodes, 3); EXPECT_EQ(work.hits, 63);
        EXPECT_EQ(work.groups, 3); EXPECT_EQ(work.routes, 66);
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

TEST_F(HybridPropertyColumnReadReuseTest, FullEntityCursorRetainsOnlyItsChargedColumnWindowAcrossOwnersAndMoves) {
    seed(NODE, 64, 64);
    auto workspace = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    ASSERT_NO_THROW(convert(workspace));
    const auto pin = metadata_->openSnapshot();
    auto retained = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    struct Work { int64_t columns = 0, groups = 0, routes = 0, batches = 0, walks = 0; } work;
    graph::debug::PerfTrace::ScopedObserver observer(&work, [](void *owner, std::string_view key, int64_t value, bool) noexcept {
        auto &w = *static_cast<Work *>(owner);
        if (key == "native_graph.hybrid.column_decodes") w.columns += value;
        if (key == "native_graph.hybrid.group_decodes") w.groups += value;
        if (key == "native_graph.hybrid.owner_route_checks") w.routes += value;
        if (key == "native_graph.hybrid.full_column_batches") w.batches += value;
        if (key == "native_graph.hybrid.full_column_directory_walks") w.walks += value;
    });
    {
        graph::utils::OwnedAllocationScope scope(retained);
        auto cursor = properties_->openScanCursor(pin);
        size_t admitted = 0;
        const SelectedPropertyReadAdmission admission = [&](size_t bytes) { admitted += bytes; };
        const auto check = [&](auto &reader, uint64_t owner) {
            auto root = reader.readEntityOptional(pin, {NODE, owner}, &admission);
            ASSERT_TRUE(root); ASSERT_EQ(root->entries.size(), 64U);
            for (uint32_t token = 1; token <= 64; ++token) {
                EXPECT_EQ(root->entries[token - 1].key_token, token);
                EXPECT_EQ(root->entries[token - 1].value, PropertyValue(int64_t(owner - 1 + token)));
            }
        };
        check(cursor, 1);
        auto moved = std::move(cursor);
        for (uint64_t owner = 2; owner <= 32; ++owner) check(moved, owner);
        auto assigned = properties_->openScanCursor(pin);
        assigned = std::move(moved);
        for (uint64_t owner = 33; owner <= 64; ++owner) check(assigned, owner);
        EXPECT_EQ(work.columns, 64); EXPECT_EQ(work.groups, 1); EXPECT_EQ(work.routes, 64);
        EXPECT_EQ(work.batches, 64); EXPECT_EQ(work.walks, 1);
        EXPECT_GT(admitted, 0U); EXPECT_GT(retained->used(), 0U);
        RecordProperty("retained_reader_bytes", std::to_string(retained->used()));
        RecordProperty("decoded_columns", std::to_string(work.columns));
        RecordProperty("authenticated_owner_routes", std::to_string(work.routes));
    }
    EXPECT_EQ(retained->used(), 0U);
}

TEST_F(HybridPropertyColumnReadReuseTest, CachedScalarOwnsSelectedBitsAcrossReentrantEvictionAndResize) {
    seed(NODE, 64, 10);
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    ASSERT_NO_THROW(convert(memory));
    const auto pin = metadata_->openSnapshot();
    graph::utils::OwnedAllocationScope scope(memory);
    HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, pin);
    const auto extension = [](const auto &) { ADD_FAILURE() << "expected column"; };
    ASSERT_EQ(context.visitField({NODE, 1}, 1, [](const auto &) {}, extension), HybridPropertyReadContext::FieldRoute::HFR_FOUND);
    bool delivered = false;
    EXPECT_EQ(context.visitField({NODE, 1}, 1, [&](const auto &cell) {
        delivered = true; EXPECT_EQ(cell.word, 1U);
        for (uint32_t token = 2; token <= 10; ++token)
            EXPECT_EQ(context.visitField({NODE, 1}, token, [&](const auto &next) { EXPECT_EQ(next.word, token); }, extension),
                HybridPropertyReadContext::FieldRoute::HFR_FOUND);
        context.enableFullEntityWindow(10);
        EXPECT_EQ(cell.word, 1U); EXPECT_FALSE(cell.is_null);
        EXPECT_EQ(cell.kind, HybridPropertyScalarKind::HPSK_INT64);
    }, extension), HybridPropertyReadContext::FieldRoute::HFR_FOUND);
    EXPECT_TRUE(delivered);
}
TEST_F(HybridPropertyColumnReadReuseTest, FullColumnAdmissionOwnsCellsAcrossReentryAndRejectsBeforeSlots) {
    seed(NODE, 64, 10);
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    ASSERT_NO_THROW(convert(memory));
    const auto pin = metadata_->openSnapshot();
    struct Cancelled {};
    struct Cancellation final : CancellationCheckpoint {
        bool cancelled = false;
        void check() const override { if (cancelled) throw Cancelled{}; }
    } cancellation;
    {
        graph::utils::OwnedAllocationScope scope(memory);
        HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, pin, &cancellation);
        size_t admitted = 0, slotCalls = 0;
        const SelectedPropertyReadAdmission slots = [&](size_t) { ++slotCalls; };
        const SelectedPropertyReadAdmission admission = [&](size_t bytes) {
            EXPECT_GT(bytes, sizeof(uint32_t));
            if (++admitted == 1) {
                context.enableFullEntityWindow(64);
                const auto nested = HybridPropertyValueReader::readCollection(context, pin, {NODE, 64}, graph::Node::typeId);
                ASSERT_TRUE(nested.value);
                EXPECT_EQ(nested.value->entries[0].value, PropertyValue(int64_t(64)));
            }
            EXPECT_EQ(slotCalls, 0U);
        };
        const auto root = HybridPropertyValueReader::readCollection(context, pin, {NODE, 1}, graph::Node::typeId,
            nullptr, &admission, &slots);
        ASSERT_TRUE(root.routed); ASSERT_TRUE(root.value); ASSERT_EQ(root.value->entries.size(), 10U);
        EXPECT_EQ(admitted, 10U); EXPECT_EQ(slotCalls, 2U);
        for (uint32_t token = 1; token <= 10; ++token)
            EXPECT_EQ(root.value->entries[token - 1].value, PropertyValue(int64_t(token)));
        struct Refused {};
        slotCalls = 0;
        const SelectedPropertyReadAdmission refusal = [](size_t) { throw Refused{}; };
        EXPECT_THROW((void)HybridPropertyValueReader::readCollection(context, pin, {NODE, 1}, graph::Node::typeId,
            nullptr, &refusal, &slots), Refused);
        EXPECT_EQ(slotCalls, 0U);
        const SelectedPropertyReadAdmission cancel = [&](size_t) { cancellation.cancelled = true; };
        EXPECT_THROW((void)HybridPropertyValueReader::readCollection(context, pin, {NODE, 1}, graph::Node::typeId,
            nullptr, &cancel, &slots), Cancelled);
        EXPECT_EQ(slotCalls, 0U);
        cancellation.cancelled = false;
        // Physical audits must bypass both the batch path and retained columns.
        StorageIO::SequentialReadScope physical(*io_);
        const auto fresh = HybridPropertyValueReader::readCollection(context, pin, {NODE, 2}, graph::Node::typeId);
        ASSERT_TRUE(fresh.value);
        EXPECT_EQ(fresh.value->entries[9].value, PropertyValue(int64_t(11)));
    }
    EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColumnReadReuseTest, FullColumnCollectionPreservesNullBooleanAndDoubleBits) {
    NativeGraphWritePlannerOptions options;
    options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(80);
    options.entity_pool_owner_id = 901; options.logical_id_lease_block_size = 1;
    options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 902};
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options);
    auto transaction = planner.beginTransaction();
    transaction.reserveNodeIds(64);
    for (uint64_t id = 1; id <= 64; ++id) {
        ASSERT_EQ(transaction.createNodeWithReservedId(id, {}), id);
        transaction.replaceNodeProperty({id, graph::Node::typeId, {
            {1, PropertyValue(int64_t(id))}, {2, PropertyValue(bool(id % 2))},
            {3, PropertyValue(std::bit_cast<double>(UINT64_C(0x8000000000000000)))},
            {4, id % 2 ? PropertyValue{} : PropertyValue(int64_t(id))}}});
    }
    ASSERT_TRUE(transaction.commit().committed());
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    ASSERT_NO_THROW(convert(memory));
    const auto pin = metadata_->openSnapshot();
    graph::utils::OwnedAllocationScope scope(memory);
    auto cursor = properties_->openScanCursor(pin);
    for (uint64_t id = 1; id <= 64; ++id) {
        const auto root = cursor.readEntityOptional(pin, {NODE, id});
        ASSERT_TRUE(root); ASSERT_EQ(root->entries.size(), 4U);
        EXPECT_EQ(root->entries[0].value, PropertyValue(int64_t(id)));
        EXPECT_EQ(root->entries[1].value, PropertyValue(bool(id % 2)));
        EXPECT_EQ(root->entries[2].value.getType(), graph::PropertyType::DOUBLE);
        EXPECT_EQ(std::bit_cast<uint64_t>(std::get<double>(root->entries[2].value.getVariant())), UINT64_C(0x8000000000000000));
        EXPECT_EQ(root->entries[3].value, id % 2 ? PropertyValue{} : PropertyValue(int64_t(id)));
    }
}
#endif
