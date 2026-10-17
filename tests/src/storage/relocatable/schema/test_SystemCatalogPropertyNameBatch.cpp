/**
 * @brief Bounded property-name batches retain scalar authentication and ownership.
 */
#include "storage/relocatable/schema/SystemCatalogTestFixture.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/query/execution/MaterializedByteBudget.hpp"

namespace {
class SystemCatalogPropertyNameBatchTest : public SystemCatalogTest {
protected:
    std::unique_ptr<BTreePageStore> indexes;
    std::unique_ptr<SystemCatalog> catalog;
    std::unique_ptr<NativeGraphWritePlanner> planner;
    void SetUp() override {
        SystemCatalogTest::SetUp();
        indexes = std::make_unique<BTreePageStore>(*slotted_pages_, *metadata_);
        catalog = std::make_unique<SystemCatalog>(*indexes, *metadata_);
        planner = std::make_unique<NativeGraphWritePlanner>(*metadata_, *entities_, *labels_, *properties_,
            *indexes, systemCatalogFixturePlannerOptions());
    }
    void TearDown() override {
        planner.reset(); catalog.reset(); indexes.reset(); SystemCatalogTest::TearDown();
    }
    std::vector<uint32_t> populate(size_t count, const std::string &prefix = "batch-field-") {
        auto transaction = planner->beginTransaction(); auto session = catalog->beginWrite(transaction);
        std::vector<uint32_t> tokens;
        for (size_t i = 0U; i < count; ++i) tokens.push_back(session.ensurePropertyToken(prefix + std::to_string(i)));
        session.finish();
        if (!transaction.commit().committed()) throw std::runtime_error("fixture publication failed");
        return tokens;
    }
};
struct TraceValues {
    bool enabled = graph::debug::PerfTrace::isEnabled();
    bool values = graph::debug::PerfTrace::areValuesEnabled();
    TraceValues() { graph::debug::PerfTrace::setEnabled(false); graph::debug::PerfTrace::setValueCollectionEnabled(true); graph::debug::PerfTrace::reset(); }
    ~TraceValues() { graph::debug::PerfTrace::reset(); graph::debug::PerfTrace::setEnabled(enabled); graph::debug::PerfTrace::setValueCollectionEnabled(values); }
};
}

TEST_F(SystemCatalogPropertyNameBatchTest, ConsecutiveNamesPreserveOrderAndAvoidRepeatedPageDecoding) {
    const auto tokens = populate(96U); auto snapshot = metadata_->openSnapshot(); TraceValues trace;
    for (const auto token : tokens) ASSERT_TRUE(catalog->findPropertyName(snapshot, token));
    const auto scalar = graph::debug::PerfTrace::snapshotAndReset();
    const auto names = catalog->findPropertyNamesSorted(snapshot, tokens);
    const auto batch = graph::debug::PerfTrace::snapshotAndReset();
    ASSERT_EQ(names.size(), tokens.size());
    for (size_t i = 0U; i < names.size(); ++i) {
        ASSERT_TRUE(names[i]); EXPECT_EQ(*names[i], "batch-field-" + std::to_string(i));
    }
    EXPECT_EQ(batch.at("native_btree.read.range_cursor_root_descent").totalValue, 1);
    EXPECT_LT(batch.at("native_btree.read.packed_page_decodes").totalValue,
        scalar.at("native_btree.read.packed_page_decodes").totalValue / 2);
}

TEST_F(SystemCatalogPropertyNameBatchTest, SparseTokensStartSeparateCursorsInsteadOfScanningTheGap) {
    const auto tokens = populate(128U); auto snapshot = metadata_->openSnapshot(); TraceValues trace;
    const std::array selected{tokens.front(), tokens.back()};
    const auto names = catalog->findPropertyNamesSorted(snapshot, selected);
    const auto values = graph::debug::PerfTrace::snapshotAndReset();
    ASSERT_TRUE(names.front()); ASSERT_TRUE(names.back());
    EXPECT_EQ(*names.front(), "batch-field-0"); EXPECT_EQ(*names.back(), "batch-field-127");
    EXPECT_EQ(values.at("native_btree.read.range_cursor_root_descent").totalValue, 2);
}

TEST_F(SystemCatalogPropertyNameBatchTest, MissingNamesAndMaximumTokenRemainAbsent) {
    const auto tokens = populate(2U); auto snapshot = metadata_->openSnapshot();
    const std::array selected{tokens.front(), uint32_t(1000U), std::numeric_limits<uint32_t>::max()};
    const auto names = catalog->findPropertyNamesSorted(snapshot, selected);
    ASSERT_EQ(names.size(), selected.size()); ASSERT_TRUE(names[0]);
    EXPECT_FALSE(names[1]); EXPECT_FALSE(names[2]);
    EXPECT_TRUE(catalog->findPropertyNamesSorted(snapshot, {}).empty());
    for (const auto &selectedTokens : {std::vector<uint32_t>{0U}, std::vector<uint32_t>{2U, 1U}, std::vector<uint32_t>{1U, 1U}})
        EXPECT_THROW(static_cast<void>(catalog->findPropertyNamesSorted(snapshot, selectedTokens)), FormatException);
}

TEST_F(SystemCatalogPropertyNameBatchTest, LastReverseMappingStillChecksItsSuccessorForAmbiguity) {
    const auto tokens = populate(8U); auto transaction = planner->beginTransaction();
    transaction.stageScalarIndexUpsert(*indexes, RSS_SYSTEM_CATALOG_INDEX_ID,
        SystemCatalogTokenKeyCodec::propertyTokenKey(tokens.back(), "other-name"), tokens.back());
    ASSERT_TRUE(transaction.commit().committed()); auto snapshot = metadata_->openSnapshot();
    EXPECT_THROW(static_cast<void>(catalog->findPropertyNamesSorted(snapshot, tokens)), FormatException);
}

TEST_F(SystemCatalogPropertyNameBatchTest, ForwardMismatchFailsWhileThePinnedOldSnapshotStillReadsCorrectly) {
    const auto tokens = populate(4U); auto before = metadata_->openSnapshot();
    auto transaction = planner->beginTransaction();
    transaction.stageScalarIndexUpsert(*indexes, RSS_SYSTEM_CATALOG_INDEX_ID,
        SystemCatalogTokenKeyCodec::propertyNameKey("batch-field-1"), tokens.back());
    ASSERT_TRUE(transaction.commit().committed()); auto current = metadata_->openSnapshot();
    EXPECT_THROW(static_cast<void>(catalog->findPropertyNamesSorted(current, tokens)), FormatException);
    const auto old = catalog->findPropertyNamesSorted(before, tokens);
    ASSERT_TRUE(old[1]); EXPECT_EQ(*old[1], "batch-field-1");
}

TEST_F(SystemCatalogPropertyNameBatchTest, ReverseValueMismatchIsNotHiddenByItsValidForwardRecord) {
    const auto tokens = populate(4U); auto transaction = planner->beginTransaction();
    transaction.stageScalarIndexUpsert(*indexes, RSS_SYSTEM_CATALOG_INDEX_ID,
        SystemCatalogTokenKeyCodec::propertyTokenKey(tokens[1], "batch-field-1"), tokens.back());
    ASSERT_TRUE(transaction.commit().committed()); auto snapshot = metadata_->openSnapshot();
    EXPECT_THROW(static_cast<void>(catalog->findPropertyNamesSorted(snapshot, tokens)), FormatException);
}

TEST_F(SystemCatalogPropertyNameBatchTest, MissingBootstrapDoesNotTurnPresentReverseRecordsIntoNames) {
    const auto tokens = populate(4U); auto transaction = planner->beginTransaction();
    transaction.stageScalarIndexErase(*indexes, RSS_SYSTEM_CATALOG_INDEX_ID, SystemCatalogTokenKeyCodec::bootstrapKey());
    ASSERT_TRUE(transaction.commit().committed()); auto snapshot = metadata_->openSnapshot();
    EXPECT_THROW(static_cast<void>(catalog->findPropertyNamesSorted(snapshot, tokens)), FormatException);
}

TEST_F(SystemCatalogPropertyNameBatchTest, OwnedNamesRetainTheirAuthorityAndQuotaFailureDoesNotWrite) {
    const std::string prefix(512U, 'n'); const auto tokens = populate(4U, prefix);
    auto snapshot = metadata_->openSnapshot(); const auto generation = snapshot.superblock().generation;
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(2U * 1024U * 1024U);
    std::weak_ptr<std::pmr::memory_resource> authority = memory;
    std::optional<graph::utils::OwnedVector<std::optional<graph::utils::OwnedStdString>>> names;
    {
        graph::utils::OwnedAllocationScope scope(memory); names.emplace(catalog->findPropertyNamesSorted(snapshot, tokens));
    }
    ASSERT_TRUE((*names)[0]); EXPECT_GE(memory->used(), prefix.size() * tokens.size());
    bool denied = false;
    try {
        auto small = std::make_shared<graph::query::execution::MaterializedByteBudget>(1U);
        graph::utils::OwnedAllocationScope scope(small);
        static_cast<void>(catalog->findPropertyNamesSorted(snapshot, tokens));
    } catch (const graph::query::execution::MaterializedByteLimitExceeded &) { denied = true; }
    EXPECT_TRUE(denied); EXPECT_EQ(metadata_->openSnapshot().superblock().generation, generation);
    snapshot.release(); memory.reset(); ASSERT_FALSE(authority.expired());
    EXPECT_EQ(*(*names)[0], prefix + "0"); names.reset(); EXPECT_TRUE(authority.expired());
}
