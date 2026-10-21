#include "storage/relocatable/property/HybridPropertyOrdinaryConversionTestFixture.hpp"
#include "graph/storage/relocatable/maintenance/NativeStorageAllocationCensus.hpp"
#include <iostream>
using namespace graph::storage;
using graph::PropertyValue;
using graph::query::execution::MaterializedByteBudget;
#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
namespace {
enum class ShapeKind { SK_DENSE, SK_UNIQUE, SK_MIXED, SK_MULTILABEL, SK_LARGE, SK_WIDE, SK_INTERLEAVED };
struct Shape { const char *name; size_t owners, fields, columns; ShapeKind kind; };
class HybridPropertyShapeCostsTest : public graph::storage::test::HybridPropertyOrdinaryConversionTestFixture,
    public testing::WithParamInterface<Shape> {};
}
TEST_P(HybridPropertyShapeCostsTest, WholeAllocationAndEveryOwnerSurviveConversionAndReadOnlyReopen) {
    const auto shape = GetParam(); seed(NODE, shape.owners, 0);
    LargeValueStore values(io_, *metadata_); PropertyCollectionRootStore roots(*slotted_pages_, values, *metadata_);
    NativeGraphWritePlannerOptions placement; placement.first_slot = PhysicalSlotId(40); placement.last_slot = PhysicalSlotId(80);
    placement.entity_pool_owner_id = 901; placement.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 902};
    NativeGraphWritePlanner writer(*metadata_, *entities_, *labels_, roots, *btree_pages_, placement);
    auto seedTx = writer.beginTransaction(); std::vector<PropertyCollectionRoot> expected;
    for (size_t i = 0; i < shape.owners; ++i) {
        PropertyCollectionRoot root{i + 1U, graph::Node::typeId, {}};
        for (uint32_t token = 1; token <= shape.fields; ++token) {
            if (shape.kind == ShapeKind::SK_INTERLEAVED &&
                (i * (2U * token + 1U) + token * 17U) % 64U >= (token <= 8U ? 48U : 8U)) continue;
            auto actualToken = token; PropertyValue value(int64_t(i + token));
            if (shape.kind == ShapeKind::SK_UNIQUE) actualToken += uint32_t(i * shape.fields);
            if (shape.kind == ShapeKind::SK_MIXED) {
                if (token == 1 && i % 2U) value = PropertyValue(double(i));
                if (token == 2) value = PropertyValue(bool(i % 2U));
                if (token == 3) value = PropertyValue(std::string("text") + std::to_string(i % 3U));
                if (token == 4) value = PropertyValue();
            }
            if (shape.kind == ShapeKind::SK_LARGE) value = PropertyValue(std::string(8192, char('a' + i % 2U)));
            root.entries.push_back({actualToken, std::move(value)});
        }
        if (shape.kind == ShapeKind::SK_MULTILABEL) seedTx.replaceNodeLabels(i + 1U, std::array<uint64_t, 3>{1, 2, 3});
        seedTx.replaceNodeProperty(root); expected.push_back(std::move(root));
        if (shape.kind == ShapeKind::SK_LARGE && i == 1U) {
            // Publish both content identities before staging their remaining
            // owners, so this fixture tests shared existing values rather than
            // the ordinary ROW writer's same-batch deduplication policy.
            ASSERT_TRUE(seedTx.commit().committed()); seedTx = writer.beginTransaction();
        }
    }
    ASSERT_TRUE(seedTx.commit().committed()); auto old = metadata_->openSnapshot();
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    NativeStorageAllocationCensus census(io_, *metadata_, memory); const auto row = census.inspect(old);
    ASSERT_TRUE(row); ASSERT_TRUE(row->fullyAttributed()); const auto oldNode = entities_->readNode(old, 1);
    size_t oldManifests = 0; metadata_->visitLargeValueManifestLocations(old, [&](const auto &) { ++oldManifests; });
    if (shape.kind == ShapeKind::SK_LARGE) ASSERT_EQ(oldManifests, 2U);
    auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true;
    struct Work { int64_t owners = 0, classified = 0, proposals = 0, retained = 0, peak = 0; } work;
    {
        graph::debug::PerfTrace::ScopedObserver observe(&work, [](void *owner, std::string_view key, int64_t value, bool) noexcept {
            auto &w = *static_cast<Work *>(owner);
            if (key == "native_graph.hybrid.source_owners_decoded") w.owners += value;
            if (key == "native_graph.hybrid.source_classification_completed") w.classified += value;
            if (key == "native_graph.hybrid.large_field_proposals_prepared") w.proposals += value;
            if (key == "native_graph.hybrid.private_large_values") w.retained = value;
            if (key == "logical_plan.materialized_budget_peak_bytes") w.peak = (std::max)(w.peak, value);
        });
        try { run(conversion); }
        catch (...) {
            std::cerr << "NATIVE_SHAPE_REFUSAL name=" << shape.name << " decoded=" << work.owners << " classified=" << work.classified
                << " proposals=" << work.proposals << " peak=" << work.peak << '\n';
            throw;
        }
    }
    EXPECT_EQ(work.owners, shape.owners); EXPECT_EQ(work.classified, 1);
    EXPECT_EQ(work.proposals, shape.kind == ShapeKind::SK_LARGE ? 64 : 0);
    EXPECT_EQ(work.retained, shape.kind == ShapeKind::SK_LARGE ? 2 : 0);
    if (shape.kind == ShapeKind::SK_LARGE) EXPECT_GE(work.peak, 64 * 8192);
    auto current = metadata_->openSnapshot(); const auto epoch = io_->dataMutationEpoch();
    const auto candidate = census.inspect(current); ASSERT_TRUE(candidate); ASSERT_TRUE(candidate->fullyAttributed());
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    EXPECT_EQ(entities_->readNode(current, 1), oldNode);
    HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, current);
    const auto directory = HybridPropertyDirectoryIntegrity::verify(context, current, memory, &values);
    size_t memberships = 0, interleavedExtensions = 0;
    for (const auto &root : expected) for (const auto &entry : root.entries) {
        ++memberships; if (entry.key_token > 8U) ++interleavedExtensions;
    }
    EXPECT_EQ(directory.columns, shape.columns); EXPECT_EQ(directory.memberships, memberships);
    EXPECT_EQ(directory.extensions, shape.kind == ShapeKind::SK_INTERLEAVED ? interleavedExtensions : shape.owners * (shape.fields - shape.columns));
    for (const auto &root : expected) {
        EXPECT_EQ(roots.readEntity(old, {NODE, root.owner_entity_id}), root);
        EXPECT_EQ(roots.readEntity(current, {NODE, root.owner_entity_id}), root);
    }
    if (shape.kind == ShapeKind::SK_LARGE) {
        size_t manifests = 0; metadata_->visitLargeValueManifestLocations(current, [&](const auto &) { ++manifests; });
        EXPECT_EQ(manifests, oldManifests); EXPECT_EQ(manifests, 2U);
    }
    auto reopened = RelocatableMetadataStore::openReadOnly(io_);
    SlottedPagePool pages(io_, *reopened, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    LargeValueStore reopenedValues(io_, *reopened, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    PropertyCollectionRootStore reopenedRoots(pages, reopenedValues, *reopened); auto pin = reopened->openSnapshot();
    for (const auto &root : expected) EXPECT_EQ(reopenedRoots.readEntity(pin, {NODE, root.owner_entity_id}), root);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
    EXPECT_GE(candidate->totalAllocatedBytes(), candidate->steadyAllocatedBytes() + candidate->retired_catalog_bytes);
    const auto threshold = row->steadyAllocatedBytes() / 10U * 9U + row->steadyAllocatedBytes() % 10U * 9U / 10U;
    std::cout << "NATIVE_SHAPE_COST name=" << shape.name << " owners=" << shape.owners << " fields=" << shape.fields
        << " columns=" << directory.columns << " extensions=" << directory.extensions
        << " row=" << row->steadyAllocatedBytes() << " candidate=" << candidate->steadyAllocatedBytes()
        << " retired=" << candidate->retired_catalog_bytes << " allocated_peak=" << candidate->totalAllocatedBytes()
        << " file=" << candidate->file_bytes << " planning_peak=" << work.peak << " byte_gate=" << (candidate->steadyAllocatedBytes() <= threshold ? "admit" : "reject") << '\n';
}
INSTANTIATE_TEST_SUITE_P(FixedShapes, HybridPropertyShapeCostsTest, testing::Values(
    Shape{"occupied47", 47, 2, 0, ShapeKind::SK_DENSE}, Shape{"occupied48", 48, 2, 2, ShapeKind::SK_DENSE},
    Shape{"occupied64", 64, 2, 2, ShapeKind::SK_DENSE}, Shape{"unique_sparse", 64, 1, 0, ShapeKind::SK_UNIQUE},
    Shape{"mixed_null_string", 64, 4, 1, ShapeKind::SK_MIXED}, Shape{"multiple_labels", 64, 16, 16, ShapeKind::SK_MULTILABEL},
    Shape{"shared_large", 64, 1, 0, ShapeKind::SK_LARGE}, Shape{"wide4096", 64, 64, 64, ShapeKind::SK_WIDE},
    Shape{"interleaved_dense_sparse", 64, 16, 8, ShapeKind::SK_INTERLEAVED}),
    [](const testing::TestParamInfo<Shape> &info) { return info.param.name; });
#endif
#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
namespace {
class HybridPropertyMatureShapeCostsTest : public graph::storage::test::HybridPropertyOrdinaryConversionTestFixture {};
}
TEST_F(HybridPropertyMatureShapeCostsTest, EightBoundedGroupsKeepWholeDatabaseCostsAndEveryOwnerExact) {
    constexpr size_t groups = 8, fields = 64, owners = groups * 64;
    seed(NODE, owners, 0);
    NativeGraphWritePlannerOptions placement; placement.first_slot = PhysicalSlotId(40); placement.last_slot = PhysicalSlotId(80);
    placement.entity_pool_owner_id = 901; placement.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 902};
    NativeGraphWritePlanner writer(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, placement);
    const auto expected = [](uint64_t id) {
        PropertyCollectionRoot root{id, graph::Node::typeId, {}};
        for (uint32_t token = 1; token <= fields; ++token) root.entries.push_back({token, PropertyValue(int64_t(id + token))});
        return root;
    };
    for (size_t group = 0; group < groups; ++group) {
        auto tx = writer.beginTransaction();
        for (size_t offset = 1; offset <= 64; ++offset) tx.replaceNodeProperty(expected(group * 64 + offset));
        ASSERT_TRUE(tx.commit().committed());
    }
    auto old = metadata_->openSnapshot(); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    NativeStorageAllocationCensus census(io_, *metadata_, memory); const auto row = census.inspect(old);
    ASSERT_TRUE(row); ASSERT_TRUE(row->fullyAttributed());
    for (size_t group = 1; group <= groups; ++group) {
        SCOPED_TRACE(group);
        auto conversion = request(NODE, memory); conversion.conversion->group_id = group;
        conversion.conversion->publish_candidate = true;
        ASSERT_NO_THROW(run(conversion));
        EXPECT_EQ(memory->used(), 0U);
    }
    auto current = metadata_->openSnapshot(); const auto candidate = census.inspect(current);
    ASSERT_TRUE(candidate); ASSERT_TRUE(candidate->fullyAttributed());
    HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, current);
    const auto report = HybridPropertyDirectoryIntegrity::verify(context, current, memory);
    EXPECT_EQ(report.groups, groups); EXPECT_EQ(report.columns, groups * fields);
    EXPECT_EQ(report.memberships, owners * fields); EXPECT_EQ(report.extensions, 0U);
    for (uint64_t id = 1; id <= owners; ++id) {
        EXPECT_EQ(properties_->readEntity(old, {NODE, id}), expected(id));
        EXPECT_EQ(properties_->readEntity(current, {NODE, id}), expected(id));
    }
    auto reopened = RelocatableMetadataStore::openReadOnly(io_);
    SlottedPagePool pages(io_, *reopened, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    PropertyCollectionRootStore roots(pages, *reopened); auto pin = reopened->openSnapshot();
    for (uint64_t id = 1; id <= owners; ++id) EXPECT_EQ(roots.readEntity(pin, {NODE, id}), expected(id));
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
    const auto threshold = row->steadyAllocatedBytes() / 10U * 9U + row->steadyAllocatedBytes() % 10U * 9U / 10U;
    std::cout << "NATIVE_MATURE_SHAPE_COST groups=" << groups << " owners=" << owners << " fields=" << fields
        << " row=" << row->steadyAllocatedBytes() << " candidate=" << candidate->steadyAllocatedBytes()
        << " retired=" << candidate->retired_catalog_bytes << " allocated_peak=" << candidate->totalAllocatedBytes()
        << " file=" << candidate->file_bytes << " byte_gate=" << (candidate->steadyAllocatedBytes() <= threshold ? "admit" : "reject") << '\n';
}
#endif
