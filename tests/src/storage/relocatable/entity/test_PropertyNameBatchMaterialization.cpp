#include "storage/relocatable/schema/SystemCatalogTestFixture.hpp"
#include "graph/core/Edge.hpp"
#include "graph/query/execution/MaterializedByteBudget.hpp"
#include "graph/storage/relocatable/entity/PageBackedLogicalGraphReadAdapter.hpp"

namespace {
class BatchMaterializationNames final : public PageBackedGraphNameResolver {
public:
    enum class Mode { PBM_ALLOW, PBM_DENY, PBM_MATERIALIZED_DENY, PBM_CORRUPT };
    Mode mode = Mode::PBM_ALLOW;
    mutable size_t individual_calls = 0U;
    mutable std::vector<size_t> batches;
    std::string resolveNodeLabelToken(const MetadataSnapshotPin &, uint64_t) const override { return "label"; }
    std::string resolveRelationshipTypeToken(const MetadataSnapshotPin &, uint64_t) const override { return "type"; }
    std::string resolvePropertyKey(const MetadataSnapshotPin &, uint32_t token) const override {
        ++individual_calls; return "field-" + std::to_string(token);
    }
    void prefetchPropertyKeyNames(const MetadataSnapshotPin &, std::span<const uint32_t> tokens,
        const std::function<void(uint32_t, std::string_view)> &consume) const override {
        batches.push_back(tokens.size());
        if (mode == Mode::PBM_DENY) throw graph::query::execution::detail::ResourceAdmissionDenied();
        if (mode == Mode::PBM_MATERIALIZED_DENY) throw graph::query::execution::MaterializedByteLimitExceeded();
        if (mode == Mode::PBM_CORRUPT) throw FormatException(FormatError::FERR_INVALID_STATE, "invalid batch mapping");
        for (const auto token : tokens) consume(token, "field-" + std::to_string(token));
    }
};
class PropertyNameBatchMaterializationTest : public SystemCatalogTest {
protected:
    void verify(BatchMaterializationNames::Mode mode) {
        BTreePageStore trees(*slotted_pages_, *metadata_);
        NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, trees,
            systemCatalogFixturePlannerOptions());
        auto transaction = planner.beginTransaction();
        const auto node = transaction.createNode(std::array<uint64_t, 0U>{});
        const auto target = transaction.createNode(std::array<uint64_t, 0U>{});
        const auto edge = transaction.createEdge(node, target, 0U);
        std::vector<PropertyCollectionRootEntry> fields;
        for (uint32_t token = 1U; token <= 40U; ++token)
            fields.push_back({token, graph::PropertyValue(int64_t(token))});
        transaction.replaceNodeProperty({node, graph::Node::typeId, fields});
        transaction.replaceEdgeProperty({edge, graph::Edge::typeId, fields});
        ASSERT_TRUE(transaction.commit().committed());
        PageBackedGraphReadAdapter pages(*metadata_, *entities_, *labels_, *properties_, trees);
        BatchMaterializationNames names; names.mode = mode;
        PageBackedLogicalGraphReadAdapter logical(pages, names);
        auto snapshot = metadata_->openSnapshot(); NativeReadSession session(snapshot);
        if (mode == BatchMaterializationNames::Mode::PBM_CORRUPT) {
            EXPECT_THROW(static_cast<void>(logical.readNode(snapshot, node, &session)), FormatException);
            EXPECT_EQ(names.individual_calls, 0U); return;
        }
        const auto first = logical.readNode(snapshot, node, &session);
        const auto second = logical.readEdge(snapshot, edge, &session);
        ASSERT_EQ(first.properties.size(), fields.size()); ASSERT_EQ(second.properties.size(), fields.size());
        for (size_t i = 0U; i < fields.size(); ++i) {
            EXPECT_EQ(first.properties[i].key, "field-" + std::to_string(i + 1U));
            EXPECT_EQ(first.properties[i].value, fields[i].value);
            EXPECT_EQ(second.properties[i].key, first.properties[i].key);
            EXPECT_EQ(second.properties[i].value, fields[i].value);
        }
        ASSERT_FALSE(names.batches.empty());
        EXPECT_LE(*std::max_element(names.batches.begin(), names.batches.end()), 16U);
        if (mode == BatchMaterializationNames::Mode::PBM_ALLOW) {
            EXPECT_EQ(names.individual_calls, 0U);
            const auto count = names.batches.size();
            EXPECT_EQ(logical.readNode(snapshot, node, &session).properties.size(), fields.size());
            EXPECT_EQ(names.batches.size(), count); // Already-cached names are not prefetched again.
        } else EXPECT_GT(names.individual_calls, 0U);
    }
};
}
TEST_F(PropertyNameBatchMaterializationTest, BoundedBatchesFeedNodeAndEdgeMaterializationAndReuseExistingNames) {
    verify(BatchMaterializationNames::Mode::PBM_ALLOW);
}
TEST_F(PropertyNameBatchMaterializationTest, OptionalQuotaFailureFallsBackToIndividualAuthenticatedNames) {
    verify(BatchMaterializationNames::Mode::PBM_DENY);
}
TEST_F(PropertyNameBatchMaterializationTest, CorruptionIsPropagatedInsteadOfHiddenByIndividualFallback) {
    verify(BatchMaterializationNames::Mode::PBM_CORRUPT);
}
TEST_F(PropertyNameBatchMaterializationTest, StandaloneMaterializedQuotaAlsoFallsBackWithoutChangingValues) {
    verify(BatchMaterializationNames::Mode::PBM_MATERIALIZED_DENY);
}
