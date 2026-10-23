#include <gtest/gtest.h>
#include "storage/relocatable/property/HybridPropertyColdRowRoutingTestFixture.hpp"
#include "graph/core/Node.hpp"
#include "graph/storage/relocatable/property/HybridPropertyValueReader.hpp"
#include "graph/storage/relocatable/property/HybridPropertyPageImage.hpp"
#include "graph/storage/relocatable/property/HybridPropertyDirectoryIntegrity.hpp"
#include "graph/storage/relocatable/entity/NativeGraphWritePlanner.hpp"
#include "storage/relocatable/entity/NativeGraphWritePlannerTestFixture.hpp"
#include "storage/relocatable/property/HybridPropertyReadContextTestAccess.hpp"
#include "storage/relocatable/property/HybridPropertyDescriptorStoreTestAccess.hpp"

using namespace graph::storage;
using graph::PropertyValue;
using graph::query::execution::MaterializedByteBudget;
#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
// Actual authenticated native field/index pages with an in-memory candidate
// descriptor, as in the existing read-context fixture. This does NOT enable a
// production converter or claim cold-ROW publication/ownership acceptance.
using graph::storage::test::HybridPropertyColdRowRoutingTest;
TEST_F(HybridPropertyColdRowRoutingTest, PointValuesPresenceCountsAndFullCollectionsShareExactHotColdAuthority) {
    seed(); auto pin = metadata_->openSnapshot(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    graph::utils::OwnedAllocationScope scope(memory);
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    const auto read = [&](uint64_t id, uint32_t token) { return HybridPropertyValueReader::read(context, pin, {NODE, id}, token); };
    EXPECT_EQ(read(1, 1).value->value, PropertyValue(int64_t(100)));
    EXPECT_EQ(read(1, 2).value->value, PropertyValue("user-000000"));
    EXPECT_EQ(read(3, 1).value->value, PropertyValue());
    EXPECT_EQ(read(64, 2).value->value, PropertyValue("user-000063"));
    EXPECT_FALSE(read(64, 1).value); EXPECT_FALSE(read(49, 2).value); EXPECT_FALSE(read(1, 99).value);
    EXPECT_EQ(HybridPropertyValueReader::exists(context, pin, {NODE, 1}, 4), true);
    EXPECT_EQ(HybridPropertyValueReader::exists(context, pin, {NODE, 1}, 4, true), false);
    EXPECT_EQ(HybridPropertyValueReader::exists(context, pin, {NODE, 64}, 1), false);
    EXPECT_EQ(context.count({NODE, 1}), 3U); EXPECT_EQ(context.count({NODE, 2}), 1U);
    EXPECT_EQ(context.count({NODE, 49}), 0U); EXPECT_EQ(context.count({NODE, 64}), 1U);
    const auto full = HybridPropertyValueReader::readCollection(context, pin, {NODE, 1}, graph::Node::typeId);
    ASSERT_TRUE(full.routed); ASSERT_TRUE(full.value);
    EXPECT_EQ(full.value->entries, (std::vector<PropertyCollectionRootEntry>{{1, PropertyValue(int64_t(100))},
        {2, PropertyValue("user-000000")}, {4, PropertyValue()}}));
    const auto empty = HybridPropertyValueReader::readCollection(context, pin, {NODE, 49}, graph::Node::typeId);
    EXPECT_TRUE(empty.routed); EXPECT_FALSE(empty.value);
    EXPECT_THROW(context.groupRecord({NODE, 1}), FormatException);
    EXPECT_EQ(HybridPropertyDirectoryIntegrity::verify(context, pin, memory).memberships, 51U);
}
TEST_F(HybridPropertyColdRowRoutingTest, WholeRootLargeValuesKeepTheSamePointEnumerationAndFullReadBoundary) {
    seed(true); auto pin = metadata_->openSnapshot(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    graph::utils::OwnedAllocationScope scope(memory);
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    EXPECT_EQ(HybridPropertyValueReader::read(context, pin, {NODE, 1}, 2, values_.get()).value->value, PropertyValue("user-000000"));
    EXPECT_EQ(HybridPropertyValueReader::read(context, pin, {NODE, 1}, 4, values_.get()).value->value, PropertyValue(std::string(8193, 'x')));
    EXPECT_EQ(context.count({NODE, 1}), 3U);
    EXPECT_EQ(HybridPropertyValueReader::exists(context, pin, {NODE, 1}, 4, true, values_.get()), true);
    EXPECT_EQ(HybridPropertyValueReader::readCollection(context, pin, {NODE, 1}, graph::Node::typeId, values_.get()).value->entries.size(), 3U);
}
TEST_F(HybridPropertyColdRowRoutingTest, HotReadsDoNotTouchCorruptUnvisitedColdBlockAndColdReadsFailClosed) {
    bad_cold_crc_ = true; seed(); auto pin = metadata_->openSnapshot();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20); graph::utils::OwnedAllocationScope scope(memory);
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    EXPECT_EQ(HybridPropertyValueReader::read(context, pin, {NODE, 1}, 1).value->value, PropertyValue(int64_t(100)));
    EXPECT_FALSE(HybridPropertyValueReader::read(context, pin, {NODE, 64}, 1).value);
    EXPECT_THROW(HybridPropertyValueReader::read(context, pin, {NODE, 1}, 2), FormatException);
}
TEST_F(HybridPropertyColdRowRoutingTest, CompetingOrdinaryRowStillRejectsEvenAnOtherwiseValidHotRead) {
    competing_row_ = true; seed(); auto pin = metadata_->openSnapshot();
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    EXPECT_THROW(HybridPropertyValueReader::read(context, pin, {NODE, 1}, 1), FormatException);
}
TEST_F(HybridPropertyColdRowRoutingTest, CompetingExplicitSelectorCannotOverrideAGroupGlobalMissingHotCell) {
    competing_selector_ = true; seed(); auto pin = metadata_->openSnapshot();
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    EXPECT_THROW(HybridPropertyValueReader::read(context, pin, {NODE, 64}, 1), FormatException);
}
TEST_F(HybridPropertyColdRowRoutingTest, ColdEnumerationRejectsGloballyHotTokenEvenWhereThatOwnerHasNoHotMembership) {
    hot_in_cold_ = true; seed(); auto pin = metadata_->openSnapshot();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20); graph::utils::OwnedAllocationScope scope(memory);
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    EXPECT_FALSE(HybridPropertyValueReader::read(context, pin, {NODE, 64}, 1).value);
    EXPECT_THROW(context.count({NODE, 64}), FormatException);
    EXPECT_THROW(HybridPropertyValueReader::read(context, pin, {NODE, 64}, 2), FormatException);
}
TEST_F(HybridPropertyColdRowRoutingTest, ColdRepresentationHandoffRunsBeforeDecodeAndLeaseRestoresParentOnFailure) {
    seed(); auto pin = metadata_->openSnapshot(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    graph::utils::OwnedAllocationScope scope(memory);
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    std::optional<graph::utils::OwnedVector<std::byte>> old;
    old.emplace(graph::utils::OwnedPmrAllocator<std::byte>(memory)); old->resize(RSS_PAGE_SIZE);
    unsigned parentCalls = 0, childCalls = 0;
    const std::function<void()> parent = [&] { ++parentCalls; old.reset(); };
    auto parentLease = context.borrowColdRowRepresentation(&parent);
    EXPECT_EQ(HybridPropertyValueReader::read(context, pin, {NODE, 1}, 1).value->value, PropertyValue(int64_t(100)));
    EXPECT_EQ(parentCalls, 0U); ASSERT_TRUE(old);
    {
        const std::function<void()> child = [&] { ++childCalls; throw std::runtime_error("handoff refused"); };
        auto lease = context.borrowColdRowRepresentation(&child);
        EXPECT_THROW(HybridPropertyValueReader::read(context, pin, {NODE, 1}, 2), std::runtime_error);
        EXPECT_EQ(childCalls, 1U); EXPECT_TRUE(old);
    }
    EXPECT_EQ(HybridPropertyValueReader::read(context, pin, {NODE, 1}, 2).value->value, PropertyValue("user-000000"));
    EXPECT_EQ(parentCalls, 1U); EXPECT_FALSE(old);
}

TEST_F(HybridPropertyColdRowRoutingTest, GroupedHotRunsAuthenticateAllOwnersWithoutReadingAnyColdRoot) {
    bad_cold_crc_ = true; seed(); auto pin = metadata_->openSnapshot();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20); graph::utils::OwnedAllocationScope scope(memory);
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    std::array<EntityPropertyKey, 64> owners{};
    for (unsigned i = 0; i < owners.size(); ++i) owners[i] = {NODE, i + 1};
    const std::array<uint32_t, 1> tokens{1}; size_t scalarCount = 0, missing = 0, runs = 0;
    const std::function<void(size_t, size_t, std::optional<PropertyCollectionRootEntry>)> owning =
        [&](size_t row, size_t column, auto value) { EXPECT_GE(row, 48U); EXPECT_EQ(column, 0U); EXPECT_FALSE(value); ++missing; };
    const PropertyScalarReadSink scalar = [&](size_t row, size_t column, auto type, uint64_t word) {
        EXPECT_LT(row, 48U); EXPECT_EQ(column, 0U);
        if (row == 2) { EXPECT_EQ(type, graph::PropertyType::NULL_TYPE); EXPECT_EQ(word, 0U); }
        else { EXPECT_EQ(type, graph::PropertyType::INTEGER); EXPECT_EQ(word, row + 100U); }
        ++scalarCount;
    };
    const PropertyTextReadSink text = [](auto, auto, auto) { ADD_FAILURE() << "integer column emitted text"; };
    PropertyColumnBatchReadSinks batches;
    batches.scalars = [&](auto rows, size_t column, auto type, auto words) {
        ++runs; ASSERT_EQ(rows.size(), words.size());
        for (size_t i = 0; i < rows.size(); ++i) scalar(rows[i], column, type, words[i]);
    };
    batches.texts = [](auto, auto, auto) { ADD_FAILURE() << "integer column emitted text run"; };
    struct Work { int64_t cold = 0, routes = 0; } work;
    graph::debug::PerfTrace::ScopedObserver observer(&work, [](void *owner, std::string_view key, int64_t value, bool) noexcept {
        auto &w = *static_cast<Work *>(owner);
        if (key == "native_graph.hybrid.cold_block_decodes") w.cold += value;
        if (key == "native_graph.hybrid.grouped_owner_route_checks") w.routes += value;
    });
    EXPECT_TRUE(HybridPropertyValueReader::readSelected(context, pin, owners, tokens, {}, {}, nullptr,
        nullptr, &owning, &scalar, &text, &batches));
    EXPECT_EQ(scalarCount, 48U); EXPECT_EQ(missing, 16U); EXPECT_EQ(runs, 3U);
    EXPECT_EQ(work.cold, 0); EXPECT_EQ(work.routes, 64);
}
TEST_F(HybridPropertyColdRowRoutingTest, TypedColdTextBorrowAndMutationRefusalNeverMaterializeAnOwningValue) {
    seed(); auto pin = metadata_->openSnapshot();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20); graph::utils::OwnedAllocationScope scope(memory);
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    const std::array<EntityPropertyKey, 2> owners{{{NODE, 1}, {NODE, 64}}};
    const std::array<uint32_t, 1> tokens{2}; size_t texts = 0, mutations = 0;
    const std::function<void(size_t, size_t, std::optional<PropertyCollectionRootEntry>)> owning =
        [](auto, auto, auto) { ADD_FAILURE() << "cold primitive escaped through an owning fallback"; };
    const PropertyScalarReadSink scalar = [](auto, auto, auto, auto) { ADD_FAILURE() << "text emitted as scalar"; };
    const PropertyTextReadSink text = [&](size_t row, size_t column, std::string_view value) {
        EXPECT_EQ(column, 0U); EXPECT_EQ(value, row ? "user-000063" : "user-000000"); ++texts;
    };
    graph::property_decode::AllocationScope forbid(nullptr, [](void *, size_t) {
        throw std::logic_error("typed cold text must borrow its source bytes");
    });
    EXPECT_TRUE(HybridPropertyValueReader::readSelected(context, pin, owners, tokens, {}, {}, nullptr,
        nullptr, &owning, &scalar, &text));
    EXPECT_EQ(texts, 2U);
    EXPECT_THROW(context.visitField({NODE, 1}, 1, [](const auto &) {}, [](const auto &) {}, {}, {},
        [&](const auto &, const auto &, const auto &, const auto &) { ++mutations; }), FormatException);
    EXPECT_EQ(mutations, 0U);
}

TEST_F(HybridPropertyColdRowRoutingTest, ScalarFallbackUsesMetadataPresenceAndNeverReadsAnUnrequestedColdRoot) {
    bad_cold_crc_ = true; seed(); auto pin = metadata_->openSnapshot();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20); graph::utils::OwnedAllocationScope scope(memory);
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    const std::array<EntityPropertyKey, 2> owners{{{NODE, 1}, {NODE, 64}}};
    const std::array<uint32_t, 1> tokens{1}; size_t delivered = 0, missing = 0;
    const std::function<void(size_t, size_t, std::optional<PropertyCollectionRootEntry>)> owning =
        [&](size_t row, size_t column, auto value) { EXPECT_EQ(row, 1U); EXPECT_EQ(column, 0U); EXPECT_FALSE(value); ++missing; };
    const PropertyScalarReadSink scalar = [&](size_t row, size_t column, auto type, uint64_t word) {
        EXPECT_EQ(row, 0U); EXPECT_EQ(column, 0U); EXPECT_EQ(type, graph::PropertyType::INTEGER); EXPECT_EQ(word, 100U); ++delivered;
    };
    EXPECT_TRUE(HybridPropertyValueReader::readSelected(context, pin, owners, tokens, {}, {}, nullptr, nullptr, &owning, &scalar));
    EXPECT_EQ(delivered, 1U); EXPECT_EQ(missing, 1U);
    // Asking for exact counts really does access the cold directory, so its
    // corruption must then be discovered rather than inferred from a bitmap.
    std::array<std::optional<size_t>, 2> counts;
    EXPECT_THROW(HybridPropertyValueReader::readSelected(context, pin, owners, tokens, {}, counts,
        nullptr, nullptr, &owning, &scalar), FormatException);
}
TEST_F(HybridPropertyColdRowRoutingTest, ColdSelectedWindowDecodesSharedBlockOnceAndPreservesOrderedMissingCells) {
    seed(); auto pin = metadata_->openSnapshot();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    size_t textCount = 0, missing = 0, last = 0; bool first = true; int64_t decodes = 0, routes = 0;
    const auto epoch = io_->dataMutationEpoch();
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        std::array<EntityPropertyKey, 64> owners{};
        for (size_t i = 0; i < owners.size(); ++i) owners[i] = {NODE, i + 1};
        const auto ordered = [&](size_t row) { if (!first) EXPECT_GT(row, last); first = false; last = row; };
        const std::function<void(size_t, size_t, std::optional<PropertyCollectionRootEntry>)> owning =
            [&](size_t row, size_t column, auto value) {
                ordered(row); EXPECT_GE(row, 1U); EXPECT_LT(row, 48U); EXPECT_EQ(column, 0U); EXPECT_FALSE(value); ++missing;
            };
        const PropertyScalarReadSink scalar = [](auto, auto, auto, auto) { ADD_FAILURE() << "text emitted scalar"; };
        const PropertyTextReadSink text = [&](size_t row, size_t column, std::string_view value) {
            ordered(row); EXPECT_EQ(column, 0U); EXPECT_EQ(value, row ? "user-000063" : "user-000000"); ++textCount;
        };
        struct Work { int64_t &decodes, &routes; } work{decodes, routes};
        graph::debug::PerfTrace::ScopedObserver observer(&work, [](void *p, std::string_view key, int64_t value, bool) noexcept {
            auto &w = *static_cast<Work *>(p);
            if (key == "native_graph.hybrid.cold_block_decodes") w.decodes += value;
            if (key == "native_graph.hybrid.grouped_owner_route_checks") w.routes += value;
        });
        graph::property_decode::AllocationScope forbid(nullptr, [](void *, size_t) { throw std::logic_error("cold text must borrow"); });
        const std::array<uint32_t, 1> tokens{2};
        EXPECT_TRUE(HybridPropertyValueReader::readSelected(context, pin, owners, tokens, {}, {}, nullptr,
            nullptr, &owning, &scalar, &text));
    }
    EXPECT_EQ(textCount, 2U); EXPECT_EQ(missing, 47U); EXPECT_EQ(decodes, 1); EXPECT_EQ(routes, 64);
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
TEST_F(HybridPropertyColdRowRoutingTest, ColdSelectedWindowFailureNeverReplaysEarlierOutputAndRefundsItsBody) {
    seed(true); auto pin = metadata_->openSnapshot();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20); size_t callbacks = 0;
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        const std::array<EntityPropertyKey, 2> owners{{{NODE, 1}, {NODE, 64}}};
        const std::array<uint32_t, 1> tokens{2};
        const std::function<void(size_t, size_t, std::optional<PropertyCollectionRootEntry>)> owning =
            [](auto, auto, auto) { ADD_FAILURE() << "text must borrow"; };
        const PropertyScalarReadSink scalar = [](auto, auto, auto, auto) { ADD_FAILURE(); };
        const PropertyTextReadSink text = [&](size_t row, auto, auto) {
            ++callbacks; if (row == 1) throw graph::query::execution::MaterializedByteLimitExceeded();
        };
        EXPECT_THROW(HybridPropertyValueReader::readSelected(context, pin, owners, tokens, {}, {}, values_.get(),
            nullptr, &owning, &scalar, &text), graph::query::execution::MaterializedByteLimitExceeded);
    }
    EXPECT_EQ(callbacks, 2U); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdRowRoutingTest, ColdSelectedWindowPreflightsCompetingRoutesBeforeAnyOutput) {
    competing_row_ = true; seed(); auto pin = metadata_->openSnapshot();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20); size_t callbacks = 0;
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        const std::array<EntityPropertyKey, 2> owners{{{NODE, 1}, {NODE, 64}}}; const std::array<uint32_t, 1> tokens{2};
        const std::function<void(size_t, size_t, std::optional<PropertyCollectionRootEntry>)> owning = [&](auto, auto, auto) { ++callbacks; };
        const PropertyScalarReadSink scalar = [&](auto, auto, auto, auto) { ++callbacks; };
        const PropertyTextReadSink text = [&](auto, auto, auto) { ++callbacks; };
        EXPECT_THROW(HybridPropertyValueReader::readSelected(context, pin, owners, tokens, {}, {}, nullptr,
            nullptr, &owning, &scalar, &text), FormatException);
    }
    EXPECT_EQ(callbacks, 0U); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdRowRoutingTest, RawEdgeSelectedWindowsRespectSplitBlocksNullAndMissing) {
    owner_kind_ = EntityPropertyOwnerKind::EPOK_EDGE; raw_cold_ = split_cold_ = true; seed();
    auto pin = metadata_->openSnapshot(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    size_t texts = 0, nulls = 0, missing = 0; int64_t decodes = 0;
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        const std::array<EntityPropertyKey, 2> owners{{{owner_kind_, 1}, {owner_kind_, 64}}};
        const std::function<void(size_t, size_t, std::optional<PropertyCollectionRootEntry>)> owning =
            [&](size_t row, size_t col, auto value) { EXPECT_EQ(row, 1U); EXPECT_EQ(col, 0U); EXPECT_FALSE(value); ++missing; };
        const PropertyScalarReadSink scalar = [&](size_t row, size_t col, auto type, auto word) {
            EXPECT_EQ(row, 0U); EXPECT_EQ(col, 0U); EXPECT_EQ(type, graph::PropertyType::NULL_TYPE); EXPECT_EQ(word, 0U); ++nulls;
        };
        const PropertyTextReadSink text = [&](size_t row, size_t col, std::string_view value) {
            EXPECT_EQ(col, 0U); EXPECT_EQ(value, row ? "user-000063" : "user-000000"); ++texts;
        };
        graph::debug::PerfTrace::ScopedObserver observer(&decodes, [](void *p, std::string_view key, int64_t value, bool) noexcept {
            if (key == "native_graph.hybrid.cold_block_decodes") *static_cast<int64_t *>(p) += value;
        });
        for (uint32_t token : {2U, 4U}) {
            const std::array<uint32_t, 1> tokens{token};
            EXPECT_TRUE(HybridPropertyValueReader::readSelected(context, pin, owners, tokens, {}, {}, nullptr,
                nullptr, &owning, &scalar, &text));
        }
    }
    EXPECT_EQ(texts, 2U); EXPECT_EQ(nulls, 1U); EXPECT_EQ(missing, 1U); EXPECT_EQ(decodes, 4);
    EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdRowRoutingTest, MixedOwningProjectionAndExactCountsBorrowOneColdBody) {
    seed(); auto pin = metadata_->openSnapshot(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    int64_t decodes = 0;
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        const std::array<EntityPropertyKey, 2> owners{{{NODE, 1}, {NODE, 64}}}; const std::array<uint32_t, 3> tokens{1, 2, 4};
        std::array<std::optional<PropertyCollectionRootEntry>, 6> values;
        std::array<std::optional<size_t>, 2> counts;
        graph::debug::PerfTrace::ScopedObserver observer(&decodes, [](void *p, std::string_view key, int64_t value, bool) noexcept {
            if (key == "native_graph.hybrid.cold_block_decodes") *static_cast<int64_t *>(p) += value;
        });
        EXPECT_TRUE(HybridPropertyValueReader::readSelected(context, pin, owners, tokens, values, counts));
        ASSERT_TRUE(values[0]); EXPECT_EQ(values[0]->value, PropertyValue(int64_t(100)));
        ASSERT_TRUE(values[1]); EXPECT_EQ(values[1]->value, PropertyValue("user-000000"));
        ASSERT_TRUE(values[2]); EXPECT_EQ(values[2]->value, PropertyValue()); EXPECT_FALSE(values[3]);
        ASSERT_TRUE(values[4]); EXPECT_EQ(values[4]->value, PropertyValue("user-000063")); EXPECT_FALSE(values[5]);
        EXPECT_EQ(counts[0], 3U); EXPECT_EQ(counts[1], 1U);
    }
    EXPECT_EQ(decodes, 1); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdRowRoutingTest, CountOnlyWindowSharesActualColdDirectoriesWithoutValues) {
    seed(true); auto pin = metadata_->openSnapshot(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    int64_t decodes = 0;
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        const std::array<EntityPropertyKey, 2> owners{{{NODE, 1}, {NODE, 64}}}; std::array<std::optional<size_t>, 2> counts;
        graph::debug::PerfTrace::ScopedObserver observer(&decodes, [](void *p, std::string_view key, int64_t value, bool) noexcept {
            if (key == "native_graph.hybrid.cold_block_decodes") *static_cast<int64_t *>(p) += value;
        });
        EXPECT_TRUE(HybridPropertyValueReader::readSelected(context, pin, owners, {}, {}, counts, values_.get()));
        EXPECT_EQ(counts[0], 3U); EXPECT_EQ(counts[1], 1U);
    }
    EXPECT_EQ(decodes, 1); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdRowRoutingTest, MixedWindowQuotaFailureRestoresBorrowAndNeverReplaysOutput) {
    seed(); auto pin = metadata_->openSnapshot(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    int64_t decodes = 0; size_t calls = 0;
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        const std::array<EntityPropertyKey, 2> owners{{{NODE, 1}, {NODE, 64}}}; const std::array<uint32_t, 2> tokens{1, 2};
        const std::function<void(size_t, size_t, std::optional<PropertyCollectionRootEntry>)> sink =
            [&](size_t, size_t, auto) { if (++calls == 3) throw graph::query::execution::MaterializedByteLimitExceeded(); };
        graph::debug::PerfTrace::ScopedObserver observer(&decodes, [](void *p, std::string_view key, int64_t value, bool) noexcept {
            if (key == "native_graph.hybrid.cold_block_decodes") *static_cast<int64_t *>(p) += value;
        });
        EXPECT_THROW(HybridPropertyValueReader::readSelected(context, pin, owners, tokens, {}, {}, nullptr, nullptr, &sink),
            graph::query::execution::MaterializedByteLimitExceeded);
        EXPECT_EQ(calls, 3U); EXPECT_EQ(decodes, 1);
        // The failed window cannot leave a dangling borrowed block in context.
        EXPECT_EQ(HybridPropertyValueReader::read(context, pin, {NODE, 64}, 2).value->value, PropertyValue("user-000063"));
        EXPECT_EQ(decodes, 2);
    }
    EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdRowRoutingTest, OwningHotOnlyProjectionDoesNotOpenTheColdWindow) {
    bad_cold_crc_ = true; seed(); auto pin = metadata_->openSnapshot();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        const std::array<EntityPropertyKey, 2> owners{{{NODE, 1}, {NODE, 64}}}; const std::array<uint32_t, 1> tokens{1};
        std::array<std::optional<PropertyCollectionRootEntry>, 2> values;
        EXPECT_TRUE(HybridPropertyValueReader::readSelected(context, pin, owners, tokens, values));
        ASSERT_TRUE(values[0]); EXPECT_EQ(values[0]->value, PropertyValue(int64_t(100))); EXPECT_FALSE(values[1]);
    }
    EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdRowRoutingTest, CompleteCollectionCountAdmissionAndValuesShareOneDecodedColdBody) {
    seed(true); auto pin = metadata_->openSnapshot(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    int64_t decodes = 0; size_t admitted = 0;
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        graph::debug::PerfTrace::ScopedObserver observer(&decodes, [](void *p, std::string_view key, int64_t value, bool) noexcept {
            if (key == "native_graph.hybrid.cold_block_decodes") *static_cast<int64_t *>(p) += value;
        });
        const SelectedPropertyReadAdmission admission = [&](size_t bytes) { EXPECT_GT(bytes, 0U); ++admitted; };
        auto result = HybridPropertyValueReader::readCollection(context, pin, {NODE, 1}, graph::Node::typeId, values_.get(), &admission);
        EXPECT_TRUE(result.routed); ASSERT_TRUE(result.value); ASSERT_EQ(result.value->entries.size(), 3U);
        EXPECT_EQ(result.value->entries[0].key_token, 1U); EXPECT_EQ(result.value->entries[0].value, PropertyValue(int64_t(100)));
        EXPECT_EQ(result.value->entries[1].key_token, 2U); EXPECT_EQ(result.value->entries[1].value, PropertyValue("user-000000"));
        EXPECT_EQ(result.value->entries[2].key_token, 4U); EXPECT_EQ(result.value->entries[2].value, PropertyValue(std::string(8193, 'x')));
    }
    EXPECT_EQ(decodes, 1); EXPECT_EQ(admitted, 3U); EXPECT_EQ(memory->used(), 0U);
}
#endif
