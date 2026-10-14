#include <gtest/gtest.h>
#include <cmath>
#include <optional>

#include "query/execution/DynamicValueArena.hpp"
#include "query/execution/ResourceCoordinator.hpp"

namespace graph::query::execution::detail {
namespace {
constexpr DynamicValueLimits ampleLimits{1024U, 65536U, 1024U, 1024U, 32U};
class ArenaAllocator final : public std::pmr::memory_resource {
public:
	size_t calls = 0U, live = 0U;
	size_t fail_at = (std::numeric_limits<size_t>::max)();
private:
	void *do_allocate(size_t bytes, size_t alignment) override {
		if (calls++ == fail_at) throw std::bad_alloc();
		void *pointer = std::pmr::new_delete_resource()->allocate(bytes, alignment);
		live += bytes;
		return pointer;
	}
	void do_deallocate(void *pointer, size_t bytes, size_t alignment) override {
		std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
		live -= bytes;
	}
	bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return this == &other; }
};
PropertyValue nestedValue() {
	return PropertyValue(PropertyValue::MapType{{"text", PropertyValue(std::string(9000U, 'x'))},
		{"list", PropertyValue(std::vector<PropertyValue>{PropertyValue(int64_t{-7}), PropertyValue{},
			PropertyValue(TemporalDuration{-3, 4, -5}), PropertyValue(PropertyValue::Bytes{std::byte{0}, std::byte{255}})})}});
}

TEST(DynamicValueArenaTest, FixedScalarsPreserveTypesBitsAndTemporalComponents) {
	MaterializedByteBudget budget(65536U);
	DynamicValueArena arena(ampleLimits, budget);
	const std::vector<PropertyValue> inputs{PropertyValue{}, PropertyValue(true), PropertyValue(false),
		PropertyValue((std::numeric_limits<int64_t>::min)()), PropertyValue((std::numeric_limits<int64_t>::max)()),
		PropertyValue(-0.0), PropertyValue(std::numeric_limits<double>::infinity()),
		PropertyValue(TemporalDate{(std::numeric_limits<int32_t>::min)()}),
		PropertyValue(TemporalDateTime{(std::numeric_limits<int64_t>::max)()}),
		PropertyValue(TemporalDuration{(std::numeric_limits<int64_t>::min)(), 3, (std::numeric_limits<int64_t>::max)()})};
	for (const auto &input : inputs) {
		const auto id = arena.append(input, [] {});
		EXPECT_EQ(arena.type(id), input.getType());
		EXPECT_EQ(arena.scalar(id), input);
	}
	const double nan = std::bit_cast<double>(uint64_t{0x7ff8000000000001ULL});
	const auto nanId = arena.append(PropertyValue(nan), [] {});
	EXPECT_EQ(std::bit_cast<uint64_t>(std::get<double>(arena.scalar(nanId).getVariant())), std::bit_cast<uint64_t>(nan));
	EXPECT_TRUE(std::signbit(std::get<double>(arena.scalar(arena.valueId(5U)).getVariant())));
}

TEST(DynamicValueArenaTest, StringsAndBinaryOwnDifferentTypedPayloadsIncludingNullBytesAndEmptyValues) {
	MaterializedByteBudget budget(65536U);
	DynamicValueArena arena(ampleLimits, budget);
	{
		PropertyValue text(std::string("a\0b", 3U));
		EXPECT_EQ(arena.text(arena.append(text, [] {})), std::string_view("a\0b", 3U));
	}
	const auto textId = arena.valueId(0U);
	EXPECT_EQ(arena.text(textId), std::string_view("a\0b", 3U));
	EXPECT_THROW(static_cast<void>(arena.binary(textId)), std::logic_error);
	const auto bytes = arena.append(PropertyValue(PropertyValue::Bytes{std::byte{0}, std::byte{255}}), [] {});
	ASSERT_EQ(arena.binary(bytes).size(), 2U);
	EXPECT_EQ(arena.binary(bytes)[1], std::byte{255});
	EXPECT_THROW(static_cast<void>(arena.text(bytes)), std::logic_error);
	EXPECT_TRUE(arena.text(arena.append(PropertyValue(""), [] {})).empty());
	EXPECT_TRUE(arena.binary(arena.append(PropertyValue(PropertyValue::Bytes{}), [] {})).empty());
}

TEST(DynamicValueArenaTest, NestedMapsAndListsPreserveKeysOrderNullAndOwnedChildren) {
	MaterializedByteBudget budget(65536U);
	DynamicValueArena arena(ampleLimits, budget);
	{
		const auto source = nestedValue();
		const auto root = arena.append(source, [] {});
		EXPECT_EQ(arena.elementCount(root), 2U);
	}
	const auto root = arena.valueId(0U);
	for (size_t entry = 0U; entry < arena.elementCount(root); ++entry) {
		const auto item = arena.mapEntry(root, entry);
		if (item.key == "text") EXPECT_EQ(arena.text(item.value), std::string(9000U, 'x'));
		else {
			ASSERT_EQ(item.key, "list");
			ASSERT_EQ(arena.elementCount(item.value), 4U);
			EXPECT_EQ(arena.scalar(arena.child(item.value, 0U)), PropertyValue(-7));
			EXPECT_EQ(arena.state(arena.child(item.value, 1U)), ColumnCellState::CCS_NULL);
			EXPECT_EQ(arena.scalar(arena.child(item.value, 2U)), PropertyValue(TemporalDuration{-3, 4, -5}));
			EXPECT_EQ(arena.binary(arena.child(item.value, 3U))[1], std::byte{255});
			EXPECT_THROW(static_cast<void>(arena.child(item.value, 4U)), std::out_of_range);
		}
	}
	EXPECT_THROW(static_cast<void>(arena.mapEntry(root, 2U)), std::out_of_range);
	EXPECT_THROW(static_cast<void>(arena.child(root, 0U)), std::logic_error);
}

TEST(DynamicValueArenaTest, MissingAndNullRemainDistinctAndClearRetainsChargedCapacity) {
	MaterializedByteBudget budget(65536U);
	DynamicValueArena arena(ampleLimits, budget);
	const auto missing = arena.appendMissing();
	(void) arena.append(PropertyValue{}, [] {});
	EXPECT_THROW(static_cast<void>(arena.state(missing)), std::invalid_argument);
	EXPECT_EQ(arena.state(arena.valueId(0U)), ColumnCellState::CCS_MISSING);
	EXPECT_EQ(arena.state(arena.valueId(1U)), ColumnCellState::CCS_NULL);
	const size_t retained = arena.storageBytes();
	arena.clear();
	EXPECT_EQ(arena.size(), 0U);
	EXPECT_EQ(budget.used(), retained);
	EXPECT_THROW(static_cast<void>(arena.valueId(0U)), std::out_of_range);
	(void) arena.append(PropertyValue(7), [] {});
	EXPECT_EQ(arena.scalar(arena.valueId(0U)), PropertyValue(7));
	arena.release();
	EXPECT_EQ(budget.used(), 0U);
}

TEST(DynamicValueArenaTest, ForeignReusedAndOutOfRangeDescriptorsRejectBeforeAccess) {
	MaterializedByteBudget budget(65536U);
	std::optional<DynamicValueArena> first;
	first.emplace(ampleLimits, budget);
	const auto old = first->append(PropertyValue(1), [] {});
	const void *address = &*first;
	DynamicValueArena other(ampleLimits, budget);
	(void) other.append(PropertyValue(2), [] {});
	EXPECT_THROW(static_cast<void>(other.scalar(old)), std::invalid_argument);
	first.reset(); first.emplace(ampleLimits, budget);
	EXPECT_EQ(address, &*first);
	const auto current = first->append(PropertyValue(3), [] {});
	EXPECT_THROW(static_cast<void>(first->scalar(old)), std::invalid_argument);
	auto invalid = current;
	invalid.index = (std::numeric_limits<uint64_t>::max)();
	EXPECT_THROW(static_cast<void>(first->scalar(invalid)), std::out_of_range);
	first->release(); (void) first->append(PropertyValue(4), [] {});
	EXPECT_THROW(static_cast<void>(first->scalar(current)), std::invalid_argument);
}

TEST(DynamicValueArenaTest, PreflightEnforcesValuesBytesChildrenMapAndDepthBeforeAllocation) {
	const std::vector<std::pair<DynamicValueLimits, PropertyValue>> cases{
		{{1U, 8U, 2U, 2U, 8U}, PropertyValue(std::vector<PropertyValue>{PropertyValue(1)})},
		{{8U, 1U, 2U, 2U, 8U}, PropertyValue("too long")},
		{{8U, 8U, 1U, 2U, 8U}, PropertyValue(std::vector<PropertyValue>{PropertyValue(1), PropertyValue(2)})},
		{{8U, 8U, 2U, 0U, 8U}, PropertyValue(PropertyValue::MapType{{"k", PropertyValue(1)}})},
		{{8U, 8U, 2U, 2U, 1U}, PropertyValue(std::vector<PropertyValue>{PropertyValue(1)})}};
	for (const auto &[limits, value] : cases) {
		MaterializedByteBudget budget(65536U);
		ArenaAllocator upstream;
		DynamicValueArena arena(limits, budget, &upstream);
		EXPECT_THROW(static_cast<void>(arena.append(value, [] {})), std::length_error);
		EXPECT_EQ(upstream.calls, 0U);
		EXPECT_EQ(arena.size(), 0U);
		EXPECT_EQ(budget.used(), 0U);
	}
}

TEST(DynamicValueArenaTest, BudgetAdmissionPrecedesUpstreamAllocation) {
	MaterializedByteBudget budget(1U);
	ArenaAllocator upstream;
	DynamicValueArena arena(ampleLimits, budget, &upstream);
	EXPECT_THROW(static_cast<void>(arena.append(PropertyValue(1), [] {})), MaterializedByteLimitExceeded);
	EXPECT_EQ(upstream.calls, 0U);
	EXPECT_EQ(budget.used(), 0U);
}

TEST(DynamicValueArenaTest, EveryUpstreamAllocationFailureRollsBackSizesAndRetainedCapacityIsCharged) {
	const auto source = nestedValue();
	MaterializedByteBudget budget(65536U);
	ArenaAllocator baseline;
	{ DynamicValueArena arena(ampleLimits, budget, &baseline); (void) arena.append(source, [] {}); }
	for (size_t failure = 0U; failure < baseline.calls; ++failure) {
		SCOPED_TRACE(failure);
		ArenaAllocator upstream;
		upstream.fail_at = failure;
		DynamicValueArena arena(ampleLimits, budget, &upstream);
		EXPECT_THROW(static_cast<void>(arena.append(source, [] {})), std::bad_alloc);
		EXPECT_EQ(arena.size(), 0U);
		EXPECT_EQ(budget.used(), arena.storageBytes());
		EXPECT_EQ(upstream.live, arena.storageBytes());
		upstream.fail_at = (std::numeric_limits<size_t>::max)();
		const auto root = arena.append(source, [] {});
		EXPECT_EQ(arena.elementCount(root), 2U);
		arena.release();
		EXPECT_EQ(upstream.live, 0U);
		EXPECT_EQ(budget.used(), 0U);
	}
}

TEST(DynamicValueArenaTest, EveryGuardFailurePreservesPublishedPrefixAndInvalidatesOldViews) {
	const auto source = nestedValue();
	MaterializedByteBudget budget(65536U);
	size_t checkpoints = 0U;
	{ DynamicValueArena arena(ampleLimits, budget); (void) arena.append(source, [&] { ++checkpoints; }); }
	for (size_t failure = 1U; failure <= checkpoints; ++failure) {
		SCOPED_TRACE(failure);
		DynamicValueArena arena(ampleLimits, budget);
		const auto previous = arena.append(PropertyValue(42), [] {});
		size_t current = 0U;
		EXPECT_THROW(static_cast<void>(arena.append(source, [&] {
			if (++current == failure) throw std::runtime_error("cancel arena append");
		})), std::runtime_error);
		EXPECT_EQ(arena.size(), 1U);
		EXPECT_THROW(static_cast<void>(arena.scalar(previous)), std::invalid_argument);
		EXPECT_EQ(arena.scalar(arena.valueId(0U)), PropertyValue(42));
		EXPECT_EQ(budget.used(), arena.storageBytes());
		arena.release();
		EXPECT_EQ(budget.used(), 0U);
	}
}

TEST(DynamicValueArenaTest, CoordinatedUpstreamEnforcesSharedProcessLimitAcrossIndependentLocalBudgets) {
	ProcessResourceBudget process(ResourceLimits::uniform(16384U));
	DatabaseResourceBudget database(process, ResourceLimits::uniform(16384U));
	ResourceAccount firstAccount(database, ResourceClass::RCK_QUERY, 10000U), secondAccount(database, ResourceClass::RCK_QUERY, 10000U);
	CoordinatedMemoryResource firstMemory(firstAccount), secondMemory(secondAccount);
	MaterializedByteBudget firstBudget(65536U), secondBudget(65536U);
	DynamicValueArena first(ampleLimits, firstBudget, &firstMemory), second(ampleLimits, secondBudget, &secondMemory);
	const auto source = nestedValue();
	(void) first.append(source, [] {});
	EXPECT_THROW(static_cast<void>(second.append(source, [] {})), ResourceAdmissionDenied);
	EXPECT_EQ(second.size(), 0U);
	EXPECT_EQ(firstAccount.used(), first.storageBytes());
	EXPECT_EQ(secondAccount.used(), second.storageBytes());
	EXPECT_EQ(process.usage().total, firstBudget.used() + secondBudget.used());
	first.release();
	const auto root = second.append(source, [] {});
	EXPECT_EQ(second.elementCount(root), 2U);
	second.release();
	EXPECT_EQ(process.usage().total, 0U);
	EXPECT_EQ(firstBudget.used(), 0U);
	EXPECT_EQ(secondBudget.used(), 0U);
}

TEST(DynamicValueArenaTest, EmptyContainersAndZeroByteQuotaAreValidButInvalidOwnersFailWithoutAllocation) {
	MaterializedByteBudget budget(65536U);
	const DynamicValueLimits emptyLimits{8U, 0U, 0U, 0U, 1U};
	DynamicValueArena arena(emptyLimits, budget);
	EXPECT_EQ(arena.elementCount(arena.append(PropertyValue(std::vector<PropertyValue>{}), [] {})), 0U);
	EXPECT_EQ(arena.elementCount(arena.append(PropertyValue(PropertyValue::MapType{}), [] {})), 0U);
	EXPECT_TRUE(arena.text(arena.append(PropertyValue(""), [] {})).empty());
	EXPECT_TRUE(arena.binary(arena.append(PropertyValue(PropertyValue::Bytes{}), [] {})).empty());
	arena.release();
	EXPECT_THROW((DynamicValueArena({0U, 0U, 0U, 0U, 1U}, budget)), std::invalid_argument);
	EXPECT_THROW((DynamicValueArena({1U, 0U, 0U, 0U, 0U}, budget)), std::invalid_argument);
	EXPECT_THROW((DynamicValueArena({1U, 0U, 0U, 0U, 257U}, budget)), std::invalid_argument);
	EXPECT_THROW((DynamicValueArena(ampleLimits, budget, nullptr)), std::invalid_argument);
	EXPECT_EQ(budget.used(), 0U);
}

TEST(DynamicValueArenaTest, ForestsPublishOrderedRootsIndependentlyOfNestedDescriptorCounts) {
	MaterializedByteBudget budget(65536U);
	DynamicValueArena arena(ampleLimits, budget);
	const std::vector<PropertyValue> cells{PropertyValue(std::vector<PropertyValue>{PropertyValue(1), PropertyValue(1)}), PropertyValue(9)};
	const auto forest = arena.appendMany(cells, [] {});
	EXPECT_EQ(arena.rootCount(), 2U);
	EXPECT_EQ(arena.size(), 4U);
	EXPECT_EQ(forest.count, 2U);
	EXPECT_EQ(arena.elementCount(arena.root(forest, 0U)), 2U);
	EXPECT_EQ(arena.scalar(arena.root(forest, 1U)), PropertyValue(9));
	EXPECT_EQ(arena.root(forest, 1U).index, 3U);
	EXPECT_NE(arena.child(arena.root(forest, 0U), 0U).index, arena.child(arena.root(forest, 0U), 1U).index);
	EXPECT_THROW(static_cast<void>(arena.root(forest, 2U)), std::out_of_range);
	const auto empty = arena.appendMany({}, [] {});
	EXPECT_EQ(empty.count, 0U);
	EXPECT_EQ(arena.rootCount(), 2U);
	EXPECT_THROW(static_cast<void>(arena.root(forest, 0U)), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(arena.root(empty, 0U)), std::out_of_range);
	EXPECT_EQ(arena.scalar(arena.rootId(1U)), PropertyValue(9));
}

TEST(DynamicValueArenaTest, ForestGuardFailuresNeverPublishHalfARowOrChangeThePreviouslyPublishedPrefix) {
	MaterializedByteBudget budget(65536U);
	const std::vector<PropertyValue> cells{PropertyValue(7), PropertyValue(std::string(9000U, 'x'))};
	size_t checkpoints = 0U;
	{ DynamicValueArena arena(ampleLimits, budget); (void) arena.appendMany(cells, [&] { ++checkpoints; }); }
	for (size_t failure = 1U; failure <= checkpoints; ++failure) {
		SCOPED_TRACE(failure);
		DynamicValueArena arena(ampleLimits, budget);
		(void) arena.append(PropertyValue(42), [] {});
		size_t current = 0U;
		EXPECT_THROW(static_cast<void>(arena.appendMany(cells, [&] {
			EXPECT_EQ(arena.size(), 1U);
			EXPECT_EQ(arena.rootCount(), 1U);
			EXPECT_EQ(arena.scalar(arena.rootId(0U)), PropertyValue(42));
			EXPECT_THROW(static_cast<void>(arena.rootId(1U)), std::out_of_range);
			EXPECT_THROW(static_cast<void>(arena.valueId(1U)), std::out_of_range);
			if (++current == failure) throw std::runtime_error("cancel forest append");
		})), std::runtime_error);
		EXPECT_EQ(arena.rootCount(), 1U);
		EXPECT_EQ(arena.size(), 1U);
		EXPECT_EQ(arena.scalar(arena.rootId(0U)), PropertyValue(42));
		EXPECT_EQ(budget.used(), arena.storageBytes());
		const auto retry = arena.appendMany(cells, [] {});
		EXPECT_EQ(arena.rootCount(), 3U);
		EXPECT_EQ(arena.scalar(arena.root(retry, 0U)), PropertyValue(7));
		EXPECT_EQ(arena.text(arena.root(retry, 1U)), std::string(9000U, 'x'));
	}
}

TEST(DynamicValueArenaTest, MissingRootAllocationFailurePreservesExistingPublishedForest) {
	MaterializedByteBudget budget(65536U);
	ArenaAllocator upstream;
	DynamicValueArena arena(ampleLimits, budget, &upstream);
	(void) arena.append(PropertyValue(42), [] {});
	upstream.fail_at = upstream.calls + 1U; // Value reserve succeeds; root-index reserve fails.
	EXPECT_THROW(static_cast<void>(arena.appendMissing()), std::bad_alloc);
	EXPECT_EQ(arena.size(), 1U);
	EXPECT_EQ(arena.rootCount(), 1U);
	EXPECT_EQ(arena.scalar(arena.rootId(0U)), PropertyValue(42));
	EXPECT_EQ(budget.used(), arena.storageBytes());
	upstream.fail_at = (std::numeric_limits<size_t>::max)();
	EXPECT_EQ(arena.state(arena.appendMissing()), ColumnCellState::CCS_MISSING);
	EXPECT_EQ(arena.rootCount(), 2U);
}
} // namespace
} // namespace graph::query::execution::detail
