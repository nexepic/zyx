#include "query/execution/JointPropertyWindow.hpp"
#include "query/execution/ConservativeNumericBlock.hpp"
#include "query/execution/DirectPropertyColumns.hpp"
#include "query/execution/StreamingPredicateCount.hpp"
#include "query/execution/LogicalPlanDirectEdgeRuntimeTestFixture.hpp"

namespace graph::query::execution {
using namespace direct_edge_test;
using detail::ColumnPredicate;
using detail::ColumnPredicateOp;
using detail::StreamingPredicateCount;

TEST(StreamingPredicateCountTest, SparseMixedRunsMatchScalarOracleForEveryComparison) {
    auto graph = directGraph(); DirectEdgeSnapshot snapshot(graph);
    const std::vector<PropertyValue> values{PropertyValue(-2), PropertyValue(7), PropertyValue(-0.0),
        PropertyValue(std::numeric_limits<double>::quiet_NaN()), PropertyValue(true), PropertyValue("text"), PropertyValue{}};
    for (auto op : {ColumnPredicateOp::CPO_EQUAL, ColumnPredicateOp::CPO_NOT_EQUAL,
        ColumnPredicateOp::CPO_LESS, ColumnPredicateOp::CPO_GREATER, ColumnPredicateOp::CPO_LESS_EQUAL,
        ColumnPredicateOp::CPO_GREATER_EQUAL, ColumnPredicateOp::CPO_NON_NULL}) {
        for (const auto &expected : values) {
            MaterializedByteBudget budget(4096);
            const ColumnPredicate predicate{0, op, &expected};
            {
                StreamingPredicateCount count(snapshot, 16, std::span(&predicate, 1), budget);
                EXPECT_EQ(budget.used(), 16U); // one seen byte, no value/type/state arrays
                size_t oracle = 0;
                for (size_t i = 0; i < values.size(); ++i) {
                    const size_t row = 2 * i;
                    const auto &v = values[i];
                    if (detail::columnPredicateMatches(v, predicate)) ++oracle;
                    if (v.getType() == PropertyType::STRING) {
                        const std::string_view text = std::get<std::string>(v.getVariant());
                        count.texts(std::span(&row, 1), 0, std::span(&text, 1), [] {});
                    } else {
                        uint64_t bits = 0;
                        if (v.getType() == PropertyType::INTEGER) bits = std::bit_cast<uint64_t>(std::get<int64_t>(v.getVariant()));
                        if (v.getType() == PropertyType::DOUBLE) bits = std::bit_cast<uint64_t>(std::get<double>(v.getVariant()));
                        if (v.getType() == PropertyType::BOOLEAN) bits = std::get<bool>(v.getVariant());
                        count.scalars(std::span(&row, 1), 0, v.getType(), std::span(&bits, 1), [] {});
                    }
                }
                EXPECT_EQ(count.finish([] {}), oracle);
                EXPECT_THROW(count.finish([] {}), std::invalid_argument);
            }
            EXPECT_EQ(budget.used(), 0U);
        }
    }
}

TEST(StreamingPredicateCountTest, OwningFallbackNullAndMissingAreConsumedWithoutRetention) {
    auto graph = directGraph(); DirectEdgeSnapshot snapshot(graph); MaterializedByteBudget budget(128);
    const PropertyValue expected(1);
    const ColumnPredicate predicate{0, ColumnPredicateOp::CPO_GREATER_EQUAL, &expected};
    StreamingPredicateCount count(snapshot, 4, std::span(&predicate, 1), budget);
    count.value(3, 0, PropertyValue(2), [] {});
    count.value(1, 0, PropertyValue{}, [] {});
    count.value(0, 0, std::nullopt, [] {});
    EXPECT_EQ(count.finish([] {}), 1U);
    EXPECT_EQ(budget.used(), 4U);
}

TEST(StreamingPredicateCountTest, InvalidCellsAndCallbackFailuresPoisonPrivateResult) {
    auto graph = directGraph(); DirectEdgeSnapshot snapshot(graph);
    const ColumnPredicate predicate{0, ColumnPredicateOp::CPO_NON_NULL, nullptr};
    for (int mode = 0; mode < 6; ++mode) {
        MaterializedByteBudget budget(128);
        {
            StreamingPredicateCount count(snapshot, 3, std::span(&predicate, 1), budget);
            count.value(1, 0, PropertyValue(1), [] {});
            const std::array<size_t, 2> rows{0, mode == 0 ? 1U : mode == 1 ? 3U : 2U};
            const std::array<uint64_t, 2> words{1, mode == 2 ? 2U : 1U};
            size_t checks = 0;
            EXPECT_THROW(count.scalars(rows, mode == 3 ? 1U : 0U, PropertyType::BOOLEAN, words, [&] {
                if (mode == 4 && ++checks == 2) throw std::invalid_argument("cancelled");
                if (mode == 5) count.value(0, 0, PropertyValue(1), [] {});
            }), std::invalid_argument);
            EXPECT_THROW(count.finish([] {}), std::invalid_argument);
        }
        EXPECT_EQ(budget.used(), 0U);
    }
    MaterializedByteBudget tiny(2);
    EXPECT_THROW((StreamingPredicateCount(snapshot, 3, std::span(&predicate, 1), tiny)), MaterializedByteLimitExceeded);
    EXPECT_EQ(tiny.used(), 0U);
}
TEST(StreamingPredicateCountTest, SnapshotChangesAndSwallowedReentryCannotPublishCounts) {
    auto graph = directGraph();
    class MutableIdentity final : public DirectEdgeSnapshot {
    public:
        using DirectEdgeSnapshot::DirectEdgeSnapshot;
        uint64_t revision = 1;
        std::optional<storage::LogicalGraphMutationFence> mutationFence() const override {
            storage::LogicalGraphMutationFence fence; fence.backend_token = 1;
            fence.format_token = 1; fence.logical_revision = revision; return fence;
        }
    } snapshot(graph);
    MaterializedByteBudget budget(128);
    const ColumnPredicate predicate{0, ColumnPredicateOp::CPO_NON_NULL, nullptr};
    StreamingPredicateCount first(snapshot, 2, std::span(&predicate, 1), budget);
    first.value(0, 0, PropertyValue(1), [] {});
    ++snapshot.revision;
    EXPECT_THROW(first.finish([] {}), std::invalid_argument);
    StreamingPredicateCount reentered(snapshot, 2, std::span(&predicate, 1), budget);
    EXPECT_THROW(reentered.value(0, 0, PropertyValue(1), [&] {
        try { reentered.value(1, 0, PropertyValue(2), [] {}); }
        catch (const std::invalid_argument &) {}
    }), std::invalid_argument);
    EXPECT_THROW(reentered.finish([] {}), std::invalid_argument);
}

TEST(StreamingPredicateCountTest, EliminatesRetainedNumericColumnsWithIdenticalRunsAndCount) {
    auto graph = directGraph(); DirectEdgeSnapshot snapshot(graph);
    MaterializedByteBudget legacyBudget(65536), streamBudget(65536);
    const PropertyValue expected(31);
    const ColumnPredicate predicate{0, ColumnPredicateOp::CPO_GREATER_EQUAL, &expected};
    detail::DirectPropertyColumns legacy(snapshot, 512, 1, legacyBudget);
    StreamingPredicateCount stream(snapshot, 512, std::span(&predicate, 1), streamBudget);
    std::array<size_t, 64> rows; std::array<uint64_t, 64> values;
    for (size_t start = 0; start < 512; start += 64) {
        for (size_t i = 0; i < 64; ++i) { rows[i] = start + i; values[i] = i; }
        legacy.appendScalarRun(rows, 0, PropertyType::INTEGER, values, [] {});
        stream.scalars(rows, 0, PropertyType::INTEGER, values, [] {});
    }
    legacy.seal(); SelectionVector selection(512);
    ASSERT_TRUE(legacy.tryFilterPrimitive(predicate, selection, [] {}));
    EXPECT_EQ(stream.finish([] {}), selection.selectedCount());
    EXPECT_EQ(selection.selectedCount(), 264U);
    EXPECT_EQ(streamBudget.used(), 512U);
    EXPECT_GE(legacyBudget.used(), 512U * (sizeof(int64_t) + sizeof(PropertyType) + 2U));
    std::cout << "STREAM_COUNT_WORK rows=512 callbacks=8 count=264 legacy_retained_bytes="
        << legacyBudget.used() << " stream_retained_bytes=" << streamBudget.used() << '\n';
}

TEST(StreamingPredicateCountTest, ConservativeBoundsNeverRejectARealMatch) {
    using detail::ConservativeNumericBlock;
    for (const auto &values : std::vector<std::vector<PropertyValue>>{
        {PropertyValue(-5), PropertyValue(0), PropertyValue(9)},
        {PropertyValue(-2.5), PropertyValue(-0.0), PropertyValue(9.0)}}) {
        std::vector<uint64_t> words;
        for (const auto &value : values) {
            if (value.getType() == PropertyType::INTEGER) words.push_back(std::bit_cast<uint64_t>(std::get<int64_t>(value.getVariant())));
            else words.push_back(std::bit_cast<uint64_t>(std::get<double>(value.getVariant())));
        }
        const auto block = ConservativeNumericBlock::inspect(values.front().getType(), words, [] {});
        ASSERT_TRUE(block.valid);
        for (auto operation : {ColumnPredicateOp::CPO_EQUAL, ColumnPredicateOp::CPO_NOT_EQUAL,
            ColumnPredicateOp::CPO_LESS, ColumnPredicateOp::CPO_GREATER,
            ColumnPredicateOp::CPO_LESS_EQUAL, ColumnPredicateOp::CPO_GREATER_EQUAL, ColumnPredicateOp::CPO_NON_NULL}) {
            for (const auto &expected : values) {
                ColumnPredicate predicate{0, operation, &expected};
                if (block.cannotMatch(predicate))
                    for (const auto &value : values) EXPECT_FALSE(detail::columnPredicateMatches(value, predicate));
            }
        }
        PropertyValue mixed = values.front().getType() == PropertyType::INTEGER ? PropertyValue(1.0) : PropertyValue(1);
        EXPECT_FALSE(block.cannotMatch({0, ColumnPredicateOp::CPO_EQUAL, &mixed}));
    }
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    const std::array<uint64_t, 2> words{std::bit_cast<uint64_t>(0.0), std::bit_cast<uint64_t>(nan)};
    EXPECT_FALSE(ConservativeNumericBlock::inspect(PropertyType::DOUBLE, words, [] {}).valid);
    EXPECT_FALSE(ConservativeNumericBlock::inspect(PropertyType::BOOLEAN, words, [] {}).valid);
    EXPECT_THROW((void)ConservativeNumericBlock::inspect(PropertyType::DOUBLE, words, [] { throw std::runtime_error("cancel"); }), std::runtime_error);
}

TEST(StreamingPredicateCountTest, NonNullConjunctionPreservesSparseMixedAndOwningNullSemantics) {
    auto graph = directGraph(); DirectEdgeSnapshot snapshot(graph); MaterializedByteBudget budget(4096);
    const PropertyValue expected(3);
    const std::array<ColumnPredicate, 2> predicates{{{0, ColumnPredicateOp::CPO_NON_NULL, nullptr},
        {0, ColumnPredicateOp::CPO_GREATER_EQUAL, &expected}}};
    const std::vector<PropertyValue> values{PropertyValue(7), PropertyValue(0.5), PropertyValue(),
        PropertyValue(true), PropertyValue("mixed"), PropertyValue(std::vector<PropertyValue>{PropertyValue(1)})};
    const std::array<size_t, 6> rows{9, 0, 7, 2, 5, 1};
    StreamingPredicateCount count(snapshot, 12, predicates, budget);
    size_t expectedCount = 0, nonnulls = 0;
    for (size_t i = 0; i < values.size(); ++i) {
        if (values[i].getType() != PropertyType::NULL_TYPE) ++nonnulls;
        if (detail::columnPredicateMatches(values[i], predicates[0]) && detail::columnPredicateMatches(values[i], predicates[1])) ++expectedCount;
        count.value(rows[i], 0, values[i], [] {});
    }
    count.value(10, 0, std::nullopt, [] {});
    EXPECT_EQ(count.finish([] {}), expectedCount);
    EXPECT_EQ(nonnulls, 5U); // Five tautological NON_NULL dispatches removed by the new loop.
}

namespace {
class JointSnapshot final : public DirectEdgeSnapshot, public storage::LogicalGraphJointPropertyReadProvider {
public:
    explicit JointSnapshot(DirectEdgeGraph &graph) : DirectEdgeSnapshot(graph) {}
    bool hasImmutableReadIdentity() const noexcept override { return true; }
    std::optional<storage::LogicalGraphMutationFence> mutationFence() const override {
        storage::LogicalGraphMutationFence fence; fence.backend_token = 1; fence.format_token = 1;
        fence.logical_revision = revision; return fence;
    }
    mutable size_t calls = 0; mutable uint64_t revision = 1; mutable const storage::JointNodePropertyReadSinks *active = nullptr;
    int mode = 0;
    bool tryReadJointNodePropertyWindow(std::span<const uint64_t> ids, std::span<const std::string_view>,
        std::span<const std::string_view> keys, const storage::JointNodePropertyReadSinks &sinks,
        const std::function<void(size_t)> &admit) const override {
        ++calls; if (mode == 1) return false;
        if (mode == 9) { try { admit(32); } catch (...) { return false; } }
        if (mode == 8) { sinks.nonnull(0, 0, true); return true; }
        admit(32); active = &sinks;
        struct Release { const storage::JointNodePropertyReadSinks *&p; ~Release() { p = nullptr; } } release{active};
        if (mode == 2) { sinks.nonnull(0, 0, true); return false; }
        if (mode == 3) { sinks.nonnull(0, 0, true); sinks.nonnull(0, 0, true); }
        if (mode == 4) { sinks.nonnull(0, 0, true); return true; }
        if (mode == 5) { sinks.nonnull(0, 0, true); throw std::runtime_error("source failure"); }
        if (mode == 6) ++revision;
        if (mode == 7) { try { sinks.nonnull(0, keys.size(), true); } catch (const std::invalid_argument &) {} }
        for (size_t c = 0; c < keys.size(); ++c)
            for (size_t i = 0; i < ids.size(); ++i) sinks.nonnull(i, c, i != 1);
        const std::array<size_t, 2> rows{0, 2};
        const std::array<uint64_t, 2> words{std::bit_cast<uint64_t>(int64_t(ids[0])), std::bit_cast<uint64_t>(int64_t(ids[2]))};
        sinks.runs.scalars(rows, 0, PropertyType::INTEGER, words);
        sinks.values(1, 0, PropertyValue(std::vector<PropertyValue>{PropertyValue(7)}));
        const std::array<size_t, 2> textRows{0, 2};
        const std::array<std::string_view, 2> text{"first", "last"};
        sinks.runs.texts(textRows, 1, text);
        sinks.values(1, 1, PropertyValue());
        return true;
    }
};
}
TEST(JointPropertyWindowTest, SynchronousMixedValuesAndPresenceUseIndependentOrdinalsAndBudget) {
    auto graph = directGraph(); JointSnapshot snapshot(graph); MaterializedByteBudget budget(65536);
    const std::array<uint64_t, 3> ids{2, 9, 42};
    const std::array<std::string_view, 2> values{"number", "text"}, presence{"large", "nullable"};
    {
        detail::JointPropertyWindow window(snapshot, ids.size(), values.size(), presence.size(), budget);
        size_t admitted = 0;
        ASSERT_TRUE(window.read(ids, values, presence, [&](size_t n) { admitted += n; }, [] {}));
        EXPECT_EQ(snapshot.calls, 1U); EXPECT_EQ(admitted, 32U);
        EXPECT_EQ(std::get<int64_t>(window.values().cell(0, window.values().rowId(0)).data), 2);
        EXPECT_EQ(std::get<int64_t>(window.values().cell(0, window.values().rowId(2)).data), 42);
        EXPECT_EQ(std::get<std::string_view>(window.values().cell(1, window.values().rowId(2)).data), "last");
        EXPECT_EQ(window.values().cell(1, window.values().rowId(1)).state, detail::ColumnCellState::CCS_NULL);
        EXPECT_EQ(*std::get<const PropertyValue *>(window.values().cell(0, window.values().rowId(1)).data),
            PropertyValue(std::vector<PropertyValue>{PropertyValue(7)}));
        for (size_t c = 0; c < 2; ++c) { EXPECT_TRUE(window.nonnull(0, c)); EXPECT_FALSE(window.nonnull(1, c)); EXPECT_TRUE(window.nonnull(2, c)); }
        EXPECT_THROW((void)window.read(ids, values, presence, [](size_t) {}, [] {}), std::invalid_argument);
    }
    EXPECT_EQ(budget.used(), 0U);
}
TEST(JointPropertyWindowTest, DeclineAndProducerErrorsNeverExposeOrReplayPartialWindow) {
    auto graph = directGraph(); JointSnapshot snapshot(graph);
    const std::array<uint64_t, 3> ids{2, 9, 42}; const std::array<std::string_view, 2> values{"v", "t"}, presence{"p", "q"};
    for (int mode = 1; mode <= 8; ++mode) {
        snapshot.mode = mode; snapshot.revision = 1; snapshot.calls = 0; MaterializedByteBudget budget(65536);
        {
            detail::JointPropertyWindow window(snapshot, 3, 2, 2, budget);
            if (mode == 1) EXPECT_FALSE(window.read(ids, values, presence, [](size_t) {}, [] {}));
            else EXPECT_THROW((void)window.read(ids, values, presence, [](size_t) {}, [] {}), std::exception);
            EXPECT_EQ(snapshot.calls, 1U);
            EXPECT_THROW((void)window.values(), std::invalid_argument);
            EXPECT_THROW((void)window.nonnull(0, 0), std::invalid_argument);
        }
        EXPECT_EQ(budget.used(), 0U);
    }
}
TEST(JointPropertyWindowTest, PreflightBudgetCancellationAndReentryRemainPrivate) {
    auto graph = directGraph(); JointSnapshot snapshot(graph);
    const std::array<uint64_t, 3> ids{2, 9, 42}, duplicate{2, 2, 42};
    const std::array<std::string_view, 2> values{"v", "t"}, presence{"p", "q"};
    { MaterializedByteBudget tiny(1); EXPECT_THROW((detail::JointPropertyWindow(snapshot, 3, 2, 2, tiny)), MaterializedByteLimitExceeded); EXPECT_EQ(tiny.used(), 0U); }
    { MaterializedByteBudget budget(65536); detail::JointPropertyWindow window(snapshot, 3, 2, 2, budget);
      EXPECT_THROW((void)window.read(duplicate, values, presence, [](size_t) {}, [] {}), std::invalid_argument); EXPECT_EQ(snapshot.calls, 0U); }
    { MaterializedByteBudget budget(65536); detail::JointPropertyWindow window(snapshot, 3, 2, 2, budget);
      EXPECT_THROW((void)window.read(ids, values, presence, [](size_t) { throw MaterializedByteLimitExceeded(); }, [] {}), MaterializedByteLimitExceeded); EXPECT_EQ(snapshot.calls, 1U); }
    snapshot.calls = 0;
    { MaterializedByteBudget budget(65536); detail::JointPropertyWindow window(snapshot, 3, 2, 2, budget); bool nested = false;
      EXPECT_THROW((void)window.read(ids, values, presence, [](size_t) {}, [&] {
          if (snapshot.active && !nested) { nested = true; try { snapshot.active->nonnull(2, 1, true); } catch (const std::invalid_argument &) {} }
      }), std::invalid_argument);
      EXPECT_TRUE(nested); EXPECT_EQ(snapshot.calls, 1U); EXPECT_THROW((void)window.values(), std::invalid_argument); }
}

TEST(JointPropertyWindowTest, MissingProviderDeclinesAndSnapshotFencesRemainIndependent) {
    auto graph = directGraph();
    class ImmutableLegacy final : public DirectEdgeSnapshot {
    public:
        using DirectEdgeSnapshot::DirectEdgeSnapshot;
        bool hasImmutableReadIdentity() const noexcept override { return true; }
    } legacy(graph);
    const std::array<uint64_t, 3> ids{2, 9, 42};
    const std::array<std::string_view, 2> values{"v", "t"}, presence{"p", "q"};
    { MaterializedByteBudget budget(65536); detail::JointPropertyWindow window(legacy, 3, 2, 2, budget);
      EXPECT_FALSE(window.read(ids, values, presence, [](size_t) {}, [] {}));
      EXPECT_THROW((void)window.values(), std::invalid_argument); }
    JointSnapshot old(graph), current(graph); current.revision = 2;
    MaterializedByteBudget budget(65536);
    detail::JointPropertyWindow oldWindow(old, 3, 2, 2, budget), newWindow(current, 3, 2, 2, budget);
    ASSERT_TRUE(oldWindow.read(ids, values, presence, [](size_t) {}, [] {}));
    ASSERT_TRUE(newWindow.read(ids, values, presence, [](size_t) {}, [] {}));
    ++current.revision;
    EXPECT_TRUE(oldWindow.nonnull(0, 0));
    EXPECT_THROW((void)newWindow.nonnull(0, 0), std::invalid_argument);
    EXPECT_THROW((void)newWindow.values(), std::invalid_argument);
}

TEST(JointPropertyWindowTest, ProducerCannotMaskAdmissionFailureOrTriggerFallback) {
    auto graph = directGraph(); JointSnapshot snapshot(graph); snapshot.mode = 9;
    const std::array<uint64_t, 3> ids{2, 9, 42}; const std::array<std::string_view, 2> values{"v", "t"}, presence{"p", "q"};
    MaterializedByteBudget budget(65536);
    detail::JointPropertyWindow window(snapshot, 3, 2, 2, budget);
    EXPECT_THROW((void)window.read(ids, values, presence, [](size_t) { throw MaterializedByteLimitExceeded(); }, [] {}), MaterializedByteLimitExceeded);
    EXPECT_EQ(snapshot.calls, 1U);
    EXPECT_THROW((void)window.values(), std::invalid_argument);
}

TEST(JointPropertyWindowTest, ScopedSourceUsesSameContractAndDoesNotRequireSnapshotProvider) {
    auto graph = directGraph();
    class ImmutableLegacy final : public DirectEdgeSnapshot {
    public: using DirectEdgeSnapshot::DirectEdgeSnapshot;
        bool hasImmutableReadIdentity() const noexcept override { return true; }
    } legacy(graph);
    MaterializedByteBudget budget(65536); size_t calls = 0;
    const std::array<uint64_t, 3> ids{2, 9, 42};
    const std::array<std::string_view, 2> values{"v", "t"}, presence{"p", "q"};
    const storage::JointNodePropertyReadSource source = [&](auto requested, auto, auto presenceKeys,
            const auto &sinks, const auto &admission) {
        ++calls; admission(32);
        for (size_t c = 0; c < presenceKeys.size(); ++c)
            for (size_t i = 0; i < requested.size(); ++i) sinks.nonnull(i, c, i != 1);
        const std::array<size_t, 3> rows{0, 1, 2};
        const std::array<uint64_t, 3> words{std::bit_cast<uint64_t>(int64_t(requested[0])),
            std::bit_cast<uint64_t>(int64_t(requested[1])), std::bit_cast<uint64_t>(int64_t(requested[2]))};
        sinks.runs.scalars(rows, 0, PropertyType::INTEGER, words);
        return true;
    };
    detail::JointPropertyWindow window(legacy, 3, 2, 2, budget);
    ASSERT_TRUE(window.readFromSource(ids, values, presence, [](size_t) {}, source, [] {}));
    EXPECT_EQ(calls, 1U); EXPECT_TRUE(window.nonnull(0, 0)); EXPECT_FALSE(window.nonnull(1, 0));
    EXPECT_EQ(std::get<int64_t>(window.values().cell(0, window.values().rowId(2)).data), 42);
}

}
