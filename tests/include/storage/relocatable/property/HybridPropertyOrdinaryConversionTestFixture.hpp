#pragma once
#include <gtest/gtest.h>
#include <bit>
#include <cstdlib>
#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/relocatable/index/BulkBTreeBuilder.hpp"
#include "graph/storage/CancellationCheckpoint.hpp"
#include "graph/storage/relocatable/entity/NativeGraphWritePlanner.hpp"
#include "graph/storage/relocatable/property/HybridPropertyGroupConversion.hpp"
#include "graph/storage/relocatable/property/HybridPropertyIndexIds.hpp"
#include "graph/storage/relocatable/property/HybridPropertyDirectoryIntegrity.hpp"
#include "graph/storage/relocatable/maintenance/RelocatableIntegrityScanner.hpp"
#include "graph/storage/relocatable/property/LargeValueStore.hpp"
#include "graph/query/execution/MaterializedByteBudget.hpp"
#include "storage/relocatable/entity/NativeGraphWritePlannerTestFixture.hpp"


namespace graph::storage::test {
class HybridPropertyOrdinaryConversionTestFixture : public graph::storage::test::NativeGraphWritePlannerTestFixture {
protected:
    using Request = NativeGraphHybridPropertyRootMutation;
    static constexpr auto NODE = EntityPropertyOwnerKind::EPOK_NODE;
    static constexpr auto EDGE = EntityPropertyOwnerKind::EPOK_EDGE;
    void seed(EntityPropertyOwnerKind kind, size_t owners = 49, size_t fields = 2) {
        NativeGraphWritePlannerOptions options;
        options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(80);
        options.entity_pool_owner_id = 901; options.logical_id_lease_block_size = 1;
        options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 902};
        NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options);
        auto transaction = planner.beginTransaction();
        const auto firstNode = metadata_->openSnapshot().superblock().next_logical_node_id;
        const auto firstEdge = metadata_->openSnapshot().superblock().next_logical_edge_id;
        transaction.reserveNodeIds(kind == NODE ? owners : 2);
        uint64_t source = 0, target = 0;
        if (kind == EDGE) {
            source = transaction.createNodeWithReservedId(firstNode, {});
            target = transaction.createNodeWithReservedId(firstNode + 1, {}); transaction.reserveEdgeIds(owners);
        }
        for (size_t i = 0; i < owners; ++i) {
            const auto id = kind == NODE ? transaction.createNodeWithReservedId(firstNode + i, {}) :
                transaction.createEdgeWithReservedId(firstEdge + i, source, target, 7);
            if (i == 48 && owners == 49) continue; // live owner without a Property root
            PropertyCollectionRoot root{id, kind == NODE ? graph::Node::typeId : graph::Edge::typeId, {}};
            for (size_t token = 1; token <= fields; ++token) root.entries.push_back({uint32_t(token), PropertyValue(int64_t(i + token))});
            if (kind == NODE) transaction.replaceNodeProperty(std::move(root));
            else transaction.replaceEdgeProperty(std::move(root));
        }
        ASSERT_TRUE(transaction.commit().committed());
    }
    Request request(EntityPropertyOwnerKind kind, std::shared_ptr<std::pmr::memory_resource> memory) {
        const auto current = metadata_->openSnapshot().superblock();
        Request result{current.database_id, current.hybrid_property_descriptor_root, NativeHybridPropertyRootMode::NHPRM_REPLACE,
            {}, NativeGraphHybridPropertyConversion{kind, 1, std::move(memory), nullptr, PhysicalSlotId(40), PhysicalSlotId(80), 903}};
        // Explicit runner-only matrix variant reuses the identical lifecycle
        // assertions; normal runs continue to exercise the legacy layout.
        const auto *compact = std::getenv("ZYX_TEST_COMPACT_COLUMN_MEMBERSHIPS");
        result.conversion->compact_column_memberships = compact && std::string_view(compact) == "1";
        return result;
    }
    void run(const Request &request, bool omitFence = false, std::optional<Generation> expected = {}) {
        const auto current = metadata_->openSnapshot().superblock();
        NativeGraphDerivedStateMutation derived;
        static_cast<void>(metadata_->mutateNativeGraphPagesWithLargeValues({}, {}, {}, {}, {}, {}, derived,
            LogSequenceNumber(current.checkpoint_lsn.value() + 1), [](auto, auto, auto, auto, auto, auto) {},
            [](auto &) {}, {}, {}, {}, {}, {}, {}, {},
            omitFence ? std::nullopt : std::optional(expected.value_or(current.generation)), {}, {}, {}, {}, {}, request));
    }
    struct PreparationStats { int64_t fields = -1, indexes = -1, pool_bytes = -1; };
    PreparationStats prepareWithStats(const Request &request) {
        PreparationStats stats;
        graph::debug::PerfTrace::ScopedObserver observer(&stats,
            [](void *owner, std::string_view key, int64_t value, bool) noexcept {
                auto &result = *static_cast<PreparationStats *>(owner);
                if (key == "native_graph.hybrid.private_field_frames") result.fields = value;
                if (key == "native_graph.hybrid.private_index_frames") result.indexes = value;
                if (key == "native_graph.hybrid.private_data_pool_bytes") result.pool_bytes = value;
            });
        expectPreparedRefusal(request); return stats;
    }
    void expectPreparedRefusal(const Request &request) {
        try { run(request); FAIL() << "private conversion was published"; }
        catch (const FormatException &error) {
            EXPECT_EQ(error.error(), FormatError::FERR_UNSUPPORTED_VERSION) << error.what();
            EXPECT_NE(std::string(error.what()).find("private field preparation completed"), std::string::npos) << error.what();
            EXPECT_NE(std::string(error.what()).find("directory plans prepared"), std::string::npos) << error.what();
            EXPECT_NE(std::string(error.what()).find("physical images and final roots prepared"), std::string::npos) << error.what();
        }
    }
};
} // namespace graph::storage::test
