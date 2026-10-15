#include <gtest/gtest.h>
#include <map>
#include "query/execution/ExternalQueryKeySet.hpp"

using namespace graph::query::execution;
using namespace graph::query::execution::detail;
namespace {
struct KeyScratchState {
    std::map<uint64_t, std::vector<std::byte>> blocks;
    size_t opened = 0U, writes = 0U, reads = 0U, finished = 0U;
    uint64_t next = 1U;
    bool readonly = false, corrupt = false, failRead = false, failWrite = false, failFinish = false;
};
class KeyScratch final : public graph::storage::LogicalQueryScratchSession {
public:
    KeyScratch(KeyScratchState &state, size_t maximum) : state_(state), maximum_(maximum) { ++state_.opened; }
    ~KeyScratch() override { state_.blocks.clear(); }
    uint64_t write(std::span<const std::byte> bytes) override {
        if (state_.failWrite) throw std::runtime_error("key write failed");
        if (state_.blocks.size() == maximum_) throw std::runtime_error("key disk exhausted");
        const auto id = state_.next++;
        state_.blocks.emplace(id, std::vector<std::byte>(bytes.begin(), bytes.end()));
        ++state_.writes; return id;
    }
    std::vector<std::byte> read(uint64_t id) override {
        if (state_.failRead) throw std::runtime_error("key read failed");
        ++state_.reads; auto bytes = state_.blocks.at(id);
        if (state_.corrupt) bytes.back() ^= std::byte{1};
        return bytes;
    }
    void release(uint64_t id) override { state_.blocks.erase(id); }
    void finish() override {
        if (state_.failFinish) throw std::runtime_error("key cleanup failed");
        state_.blocks.clear(); ++state_.finished;
    }
    size_t maximumFragmentBytes() const override {
        return graph::storage::RSS_SEGMENT_SIZE - graph::storage::RSS_ENVELOPE_BYTES;
    }
private:
    KeyScratchState &state_; size_t maximum_;
};
class KeyProvider final : public graph::storage::LogicalQueryScratchProvider {
public:
    mutable KeyScratchState state;
    std::unique_ptr<graph::storage::LogicalQueryScratchSession> openQueryScratchSession(size_t bytes, size_t fragments) const override {
        if (state.readonly) return {};
        return std::make_unique<KeyScratch>(state, (std::min)(fragments, bytes / graph::storage::RSS_SEGMENT_SIZE));
    }
};
std::pmr::string key(size_t ordinal, BudgetMemoryResource &memory) {
    std::pmr::string result("key-", &memory); result += std::to_string(ordinal);
    result.resize(512U, '\0'); return result;
}
}

TEST(ExternalQueryKeySetTest, ResidentKeysPreserveBinaryEqualityWithoutAcquiringFilePermission) {
    KeyProvider provider; MaterializedByteBudget budget(4U * 1024U * 1024U);
    {
        BudgetMemoryResource memory(budget);
        ExternalQueryKeySet keys(provider, budget, memory, 100U, 512U * 1024U, [] {}, [] {});
        for (size_t i = 0U; i < 100U; ++i) EXPECT_TRUE(keys.insert(key(i, memory)));
        for (size_t i = 0U; i < 100U; ++i) EXPECT_FALSE(keys.insert(key(i, memory)));
        EXPECT_EQ(keys.size(), 100U); EXPECT_FALSE(keys.spilled()); keys.finish(); keys.finish();
        EXPECT_EQ(provider.state.opened, 0U);
    }
    EXPECT_EQ(budget.used(), 0U);
}

TEST(ExternalQueryKeySetTest, PartitionedKeysProbeDiskAndReturnOnlyFirstOccurrencesAcrossBlocks) {
    KeyProvider provider; MaterializedByteBudget budget(4U * 1024U * 1024U);
    {
        BudgetMemoryResource memory(budget);
        ExternalQueryKeySet keys(provider, budget, memory, 30000U, 32768U, [] {}, [] {});
        for (size_t i = 0U; i < 3000U; ++i) ASSERT_TRUE(keys.insert(key(i, memory)));
        for (size_t i = 3000U; i > 0U; --i) ASSERT_FALSE(keys.insert(key(i - 1U, memory)));
        EXPECT_TRUE(keys.spilled()); EXPECT_EQ(keys.size(), 3000U);
        EXPECT_GT(provider.state.writes, 8U); EXPECT_GT(provider.state.reads, 0U);
        EXPECT_LE(budget.used(), 2U * 1024U * 1024U);
        keys.finish(); EXPECT_TRUE(provider.state.blocks.empty()); EXPECT_EQ(provider.state.finished, 1U);
    }
    EXPECT_EQ(budget.used(), 0U);
}

TEST(ExternalQueryKeySetTest, UniqueBoundAlsoAppliesAfterSpillAndDuplicatesRemainAllowedAtTheBound) {
    KeyProvider provider; MaterializedByteBudget budget(4U * 1024U * 1024U);
    {
        BudgetMemoryResource memory(budget);
        ExternalQueryKeySet keys(provider, budget, memory, 100U, 1U, [] {}, [] {});
        for (size_t i = 0U; i < 100U; ++i) ASSERT_TRUE(keys.insert(key(i, memory)));
        EXPECT_FALSE(keys.insert(key(0U, memory)));
        EXPECT_THROW((void)keys.insert(key(100U, memory)), std::runtime_error);
        EXPECT_THROW((void)keys.insert(key(0U, memory)), std::logic_error);
        keys.finish();
    }
    EXPECT_EQ(budget.used(), 0U); EXPECT_TRUE(provider.state.blocks.empty());
}

TEST(ExternalQueryKeySetTest, CancellationAndWorkspaceDenialPrecedeScratchOwnerPublication) {
    for (const bool cancel : {false, true}) {
        KeyProvider provider; MaterializedByteBudget budget(cancel ? 4U * 1024U * 1024U : 32768U);
        {
            BudgetMemoryResource memory(budget);
            ExternalQueryKeySet keys(provider, budget, memory, 100U, 1U, [] {}, [=] {
                if (cancel) throw std::runtime_error("cancel before key publication");
            });
            EXPECT_THROW((void)keys.insert(key(0U, memory)), std::runtime_error);
            EXPECT_EQ(provider.state.opened, 0U); EXPECT_EQ(provider.state.writes, 0U);
        }
        EXPECT_EQ(budget.used(), 0U);
    }
}

TEST(ExternalQueryKeySetTest, ReadonlyWriteReadAndCleanupFailuresReleaseEveryOwner) {
    for (size_t mode = 0U; mode < 4U; ++mode) {
        KeyProvider provider; provider.state.readonly = mode == 0U; provider.state.failWrite = mode == 1U;
        MaterializedByteBudget budget(4U * 1024U * 1024U);
        {
            BudgetMemoryResource memory(budget);
            ExternalQueryKeySet keys(provider, budget, memory, 100U, 1U, [] {}, [] {});
            if (mode < 2U) EXPECT_THROW((void)keys.insert(key(0U, memory)), std::runtime_error);
            else {
                ASSERT_TRUE(keys.insert(key(0U, memory)));
                if (mode == 2U) {
                    provider.state.failRead = true;
                    EXPECT_THROW((void)keys.insert(key(0U, memory)), std::runtime_error);
                } else {
                    provider.state.failFinish = true;
                    EXPECT_THROW(keys.finish(), std::runtime_error);
                }
            }
        }
        EXPECT_EQ(budget.used(), 0U); EXPECT_TRUE(provider.state.blocks.empty());
    }
}

TEST(ExternalQueryKeySetTest, MalformedPersistedKeyCannotBeAcceptedAsADuplicate) {
    KeyProvider provider; MaterializedByteBudget budget(4U * 1024U * 1024U);
    {
        BudgetMemoryResource memory(budget);
        ExternalQueryKeySet keys(provider, budget, memory, 100U, 1U, [] {}, [] {});
        ASSERT_TRUE(keys.insert(key(0U, memory)));
        // Corrupt the key length, retaining the original partition/Bloom route.
        auto &block = provider.state.blocks.begin()->second;
        for (size_t i = 16U; i < 20U; ++i) block[i] = std::byte{0xff};
        EXPECT_THROW((void)keys.insert(key(0U, memory)), std::invalid_argument);
        keys.finish();
    }
    EXPECT_EQ(budget.used(), 0U);
}

TEST(ExternalQueryKeySetTest, BloomCollisionStillRequiresAnExactKeyComparison) {
    KeyProvider provider; MaterializedByteBudget budget(4U * 1024U * 1024U);
    {
        BudgetMemoryResource memory(budget);
        ExternalQueryKeySet keys(provider, budget, memory, 100U, 1U, [] {}, [] {});
        const auto binary = [&](uint64_t number) {
            std::pmr::string bytes(8U, '\0', &memory);
            for (size_t i = 0U; i < 8U; ++i) bytes[i] = static_cast<char>((number >> (i * 8U)) & 255U);
            return bytes;
        };
        // These distinct binary keys share both Bloom bits and partition 4.
        ASSERT_TRUE(keys.insert(binary(54107U)));
        const auto reads = provider.state.reads;
        ASSERT_TRUE(keys.insert(binary(120710U)));
        EXPECT_GT(provider.state.reads, reads);
        EXPECT_FALSE(keys.insert(binary(54107U)));
        EXPECT_FALSE(keys.insert(binary(120710U)));
        EXPECT_EQ(keys.size(), 2U); keys.finish();
    }
    EXPECT_EQ(budget.used(), 0U);
}

TEST(ExternalQueryKeySetTest, SharedOutputPressureTriggersSpillBeforeTheLocalKeyThreshold) {
    KeyProvider provider; MaterializedByteBudget budget(4U * 1024U * 1024U);
    {
        BudgetMemoryResource memory(budget);
        ExternalQueryKeySet keys(provider, budget, memory, 100U, 512U * 1024U, [] {}, [] {});
        for (size_t i = 0U; i < 10U; ++i) ASSERT_TRUE(keys.insert(key(i, memory)));
        ASSERT_FALSE(keys.spilled());
        MaterializedByteReservation otherOperator(&budget);
        otherOperator.set(budget.available() - 1500U * 1024U);
        ASSERT_TRUE(keys.insert(key(10U, memory)));
        EXPECT_TRUE(keys.spilled()); EXPECT_GT(provider.state.writes, 0U);
        for (size_t i = 0U; i <= 10U; ++i) EXPECT_FALSE(keys.insert(key(i, memory)));
        keys.finish();
    }
    EXPECT_EQ(budget.used(), 0U);
}
