#include <gtest/gtest.h>

#include "query/execution/BoundPropertyBindings.hpp"

namespace graph::query::execution::detail {
namespace {
class BindingSnapshot final : public storage::LogicalGraphReadSnapshot {
public:
	std::optional<storage::LogicalGraphSchemaIdentity> identity{storage::LogicalGraphSchemaIdentity{1U, 2U, {}, 0U, 0U}};
	bool tokens_supported = true;
	mutable size_t resolutions = 0U;
	std::function<std::optional<uint32_t>(std::string_view)> resolve = [](auto key) {
		return key == "rank" ? std::optional<uint32_t>(7U) : std::nullopt;
	};
	std::optional<storage::LogicalGraphSchemaIdentity> bindingSchemaIdentity() const override { return identity; }
	bool supportsPropertyTokenResolution() const override { return tokens_supported; }
	std::optional<uint32_t> findPropertyToken(std::string_view key) const override { ++resolutions; return resolve(key); }
	storage::LogicalGraphNode readNode(uint64_t) const override { throw std::logic_error("unexpected data read"); }
	storage::LogicalGraphEdge readEdge(uint64_t) const override { throw std::logic_error("unexpected data read"); }
	void visitNodes(size_t, const std::function<bool(const storage::LogicalGraphNode &)> &) const override {}
	void visitEdges(size_t, const std::function<bool(const storage::LogicalGraphEdge &)> &) const override {}
	storage::LogicalGraphNodePage scanNodesAfter(uint64_t, size_t, size_t) const override { return {}; }
	storage::LogicalGraphAdjacencyPage scanAdjacentEdges(uint64_t, storage::LogicalGraphReadDirection,
		std::optional<storage::LogicalGraphAdjacencyContinuation>, size_t) const override { return {}; }
	void visitAdjacentEdges(uint64_t, storage::LogicalGraphReadDirection, size_t,
		const std::function<bool(const storage::LogicalGraphEdge &)> &) const override {}
};

class BindingAllocator final : public std::pmr::memory_resource {
public:
	size_t calls = 0U;
	size_t fail_at = (std::numeric_limits<size_t>::max)();
	size_t live_bytes = 0U;
private:
	void *do_allocate(size_t bytes, size_t alignment) override {
		if (calls++ == fail_at) throw std::bad_alloc();
		void *value = std::pmr::new_delete_resource()->allocate(bytes, alignment);
		live_bytes += bytes;
		return value;
	}
	void do_deallocate(void *value, size_t bytes, size_t alignment) override {
		std::pmr::new_delete_resource()->deallocate(value, bytes, alignment);
		live_bytes -= bytes;
	}
	bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return &other == this; }
};

TEST(BoundPropertyBindingsTest, RepeatedRequestsShareDescriptorsButDistinctSlotsRemainDistinct) {
	MaterializedByteBudget budget(65536U);
	BoundSlotRegistry slots(2U, 1U, 32U, budget);
	const auto first = slots.declare(slots.root(), "n"), second = slots.declare(slots.root(), "m");
	slots.seal();
	BindingSnapshot snapshot;
	const std::array<BoundPropertyRequest, 3U> requests{{{first, "rank"}, {first, "rank"}, {second, "rank"}}};
	BoundPropertyBindings bindings(slots, snapshot, requests, 3U, 32U, budget);
	EXPECT_EQ(bindings.size(), 2U);
	EXPECT_EQ(bindings.requestCount(), 3U);
	EXPECT_EQ(bindings.descriptor(0U), bindings.descriptor(1U));
	EXPECT_NE(bindings.descriptor(0U), bindings.descriptor(2U));
	EXPECT_EQ(snapshot.resolutions, 2U);
	const auto view = bindings.view(bindings.descriptor(0U), snapshot);
	EXPECT_EQ(view.source, first);
	EXPECT_EQ(view.key, "rank");
	EXPECT_EQ(view.token, 7U);
}

TEST(BoundPropertyBindingsTest, OwnedKeysSurviveInputDestructionAndNoTokenPreservesNameBasedAccess) {
	MaterializedByteBudget budget(65536U);
	BoundSlotRegistry slots(1U, 1U, 32U, budget);
	const auto slot = slots.declare(slots.root(), "n");
	slots.seal();
	BindingSnapshot snapshot;
	std::string key(512U, 'k');
	const std::array<BoundPropertyRequest, 1U> requests{{{slot, key}}};
	BoundPropertyBindings bindings(slots, snapshot, requests, 1U, 512U, budget);
	key.clear();
	key.shrink_to_fit();
	const auto view = bindings.view(bindings.descriptor(0U), snapshot);
	EXPECT_EQ(view.key, std::string(512U, 'k'));
	EXPECT_FALSE(view.token); // Includes staged names with no durable token.
}

TEST(BoundPropertyBindingsTest, UnsupportedTokenCapabilityDoesNotCallResolverOrPretendAbsence) {
	MaterializedByteBudget budget(65536U);
	BoundSlotRegistry slots(1U, 1U, 32U, budget);
	const auto slot = slots.declare(slots.root(), "n");
	slots.seal();
	BindingSnapshot snapshot;
	snapshot.tokens_supported = false;
	snapshot.resolve = [](auto) -> std::optional<uint32_t> { throw std::logic_error("must not resolve"); };
	const std::array<BoundPropertyRequest, 1U> requests{{{slot, "rank"}}};
	BoundPropertyBindings bindings(slots, snapshot, requests, 1U, 32U, budget);
	EXPECT_EQ(snapshot.resolutions, 0U);
	EXPECT_FALSE(bindings.view(bindings.descriptor(0U), snapshot).token);
	snapshot.tokens_supported = true;
	EXPECT_THROW(bindings.validateSnapshot(snapshot), BoundPropertyBindingConflict);
}

TEST(BoundPropertyBindingsTest, ForeignProgramsInvalidOrdinalsAndReusedOwnerAddressesRejectDescriptors) {
	MaterializedByteBudget budget(65536U);
	BoundSlotRegistry slots(1U, 1U, 32U, budget);
	const auto slot = slots.declare(slots.root(), "n");
	slots.seal();
	BindingSnapshot snapshot;
	const std::array<BoundPropertyRequest, 1U> requests{{{slot, "rank"}}};
	BoundPropertyBindings first(slots, snapshot, requests, 1U, 32U, budget);
	BoundPropertyBindings second(slots, snapshot, requests, 1U, 32U, budget);
	EXPECT_THROW(static_cast<void>(second.view(first.descriptor(0U), snapshot)), std::invalid_argument);
	auto invalid = first.descriptor(0U);
	invalid.index = 99U;
	EXPECT_THROW(static_cast<void>(first.view(invalid, snapshot)), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(first.descriptor(99U)), std::out_of_range);
	std::optional<BoundPropertyBindings> reused;
	reused.emplace(slots, snapshot, requests, 1U, 32U, budget);
	const auto stale = reused->descriptor(0U);
	reused.reset();
	reused.emplace(slots, snapshot, requests, 1U, 32U, budget);
	EXPECT_THROW(static_cast<void>(reused->view(stale, snapshot)), std::invalid_argument);
}

TEST(BoundPropertyBindingsTest, ChangedDatabaseFormatCatalogViewAndRevisionRequireRebinding) {
	MaterializedByteBudget budget(65536U);
	BoundSlotRegistry slots(1U, 1U, 32U, budget);
	const auto slot = slots.declare(slots.root(), "n");
	slots.seal();
	BindingSnapshot snapshot;
	snapshot.identity->view_token = 10U;
	const auto original = snapshot.identity;
	const std::array<BoundPropertyRequest, 1U> requests{{{slot, "rank"}}};
	BoundPropertyBindings bindings(slots, snapshot, requests, 1U, 32U, budget);
	const auto reject = [&] { EXPECT_THROW(static_cast<void>(bindings.view(bindings.descriptor(0U), snapshot)), BoundPropertyBindingConflict); snapshot.identity = original; };
	++snapshot.identity->database_token; reject();
	++snapshot.identity->format_token; reject();
	for (auto index = 0U; index < original->catalog_tokens.size(); ++index) {
		++snapshot.identity->catalog_tokens[index]; reject();
	}
	++snapshot.identity->view_token; reject();
	++snapshot.identity->logical_revision; reject();
	EXPECT_NO_THROW(bindings.validateSnapshot(snapshot));
}

TEST(BoundPropertyBindingsTest, ResolverEpochChangeZeroTokenAndSourceFailureCannotPublishBindings) {
	MaterializedByteBudget budget(65536U);
	BoundSlotRegistry slots(1U, 1U, 32U, budget);
	const auto slot = slots.declare(slots.root(), "n");
	slots.seal();
	BindingSnapshot snapshot;
	const size_t before = budget.used();
	const std::array<BoundPropertyRequest, 1U> requests{{{slot, "rank"}}};
	snapshot.resolve = [&](auto) { ++snapshot.identity->catalog_tokens[0U]; return std::optional<uint32_t>(7U); };
	EXPECT_THROW((BoundPropertyBindings(slots, snapshot, requests, 1U, 32U, budget)), BoundPropertyBindingConflict);
	EXPECT_EQ(budget.used(), before);
	snapshot.resolve = [](auto) { return std::optional<uint32_t>(0U); };
	EXPECT_THROW((BoundPropertyBindings(slots, snapshot, requests, 1U, 32U, budget)), std::logic_error);
	EXPECT_EQ(budget.used(), before);
	snapshot.resolve = [](auto) -> std::optional<uint32_t> { throw std::runtime_error("source failure"); };
	EXPECT_THROW((BoundPropertyBindings(slots, snapshot, requests, 1U, 32U, budget)), std::runtime_error);
	EXPECT_EQ(budget.used(), before);
}

TEST(BoundPropertyBindingsTest, InvalidRequestsAreRejectedBeforeAnyTokenResolution) {
	MaterializedByteBudget budget(65536U);
	BoundSlotRegistry slots(1U, 1U, 32U, budget), foreign(1U, 1U, 32U, budget);
	const auto slot = slots.declare(slots.root(), "n"), other = foreign.declare(foreign.root(), "n");
	slots.seal(); foreign.seal();
	BindingSnapshot snapshot;
	const std::array<BoundPropertyRequest, 2U> invalid{{{slot, "rank"}, {other, "rank"}}};
	EXPECT_THROW((BoundPropertyBindings(slots, snapshot, invalid, 2U, 32U, budget)), std::invalid_argument);
	EXPECT_EQ(snapshot.resolutions, 0U);
	const std::array<BoundPropertyRequest, 1U> empty{{{slot, ""}}};
	EXPECT_THROW((BoundPropertyBindings(slots, snapshot, empty, 1U, 32U, budget)), std::invalid_argument);
	const std::array<BoundPropertyRequest, 1U> valid{{{slot, "rank"}}};
	EXPECT_THROW((BoundPropertyBindings(slots, snapshot, valid, 1U, 3U, budget)), std::length_error);
	EXPECT_THROW((BoundPropertyBindings(slots, snapshot, valid, 0U, 32U, budget)), std::invalid_argument);
	EXPECT_THROW((BoundPropertyBindings(slots, snapshot, valid, 1U, 32U, budget, nullptr)), std::invalid_argument);
	snapshot.identity.reset();
	EXPECT_THROW((BoundPropertyBindings(slots, snapshot, valid, 1U, 32U, budget)), std::logic_error);
}

TEST(BoundPropertyBindingsTest, UnsealedLayoutsAndInvalidSourceIdentitiesAreNotOptimizationFallback) {
	MaterializedByteBudget budget(65536U);
	BoundSlotRegistry slots(1U, 1U, 32U, budget);
	const auto slot = slots.declare(slots.root(), "n");
	BindingSnapshot snapshot;
	const std::array<BoundPropertyRequest, 1U> requests{{{slot, "rank"}}};
	EXPECT_THROW((BoundPropertyBindings(slots, snapshot, requests, 1U, 32U, budget)), std::invalid_argument);
	slots.seal();
	snapshot.identity = storage::LogicalGraphSchemaIdentity{};
	EXPECT_THROW((BoundPropertyBindings(slots, snapshot, requests, 1U, 32U, budget)), std::logic_error);
	EXPECT_EQ(snapshot.resolutions, 0U);
}

TEST(BoundPropertyBindingsTest, EveryAllocationFailureRefundsTheTableAndRetryPreservesOtherOwners) {
	MaterializedByteBudget budget(1024U * 1024U);
	BoundSlotRegistry slots(1U, 1U, 32U, budget);
	const auto slot = slots.declare(slots.root(), "n");
	slots.seal();
	BindingSnapshot snapshot;
	const std::string wide(256U, 'k');
	const std::array<BoundPropertyRequest, 3U> requests{{{slot, "rank"}, {slot, wide}, {slot, "rank"}}};
	size_t allocationCount = 0U;
	const size_t before = budget.used();
	{
		BindingAllocator allocator;
		{ BoundPropertyBindings table(slots, snapshot, requests, 3U, 1024U, budget, &allocator); }
		allocationCount = allocator.calls;
		EXPECT_EQ(allocator.live_bytes, 0U);
	}
	ASSERT_GT(allocationCount, 0U);
	for (size_t rejected = 0U; rejected < allocationCount; ++rejected) {
		BindingAllocator allocator;
		allocator.fail_at = rejected;
		EXPECT_THROW((BoundPropertyBindings(slots, snapshot, requests, 3U, 1024U, budget, &allocator)), std::bad_alloc);
		EXPECT_EQ(allocator.live_bytes, 0U);
		EXPECT_EQ(budget.used(), before);
		allocator.fail_at = (std::numeric_limits<size_t>::max)();
		{ BoundPropertyBindings retry(slots, snapshot, requests, 3U, 1024U, budget, &allocator); EXPECT_EQ(retry.size(), 2U); }
		EXPECT_EQ(budget.used(), before);
	}
}

TEST(BoundPropertyBindingsTest, SharedBudgetPressureDoesNotBecomeTokenAbsence) {
	MaterializedByteBudget budget(4096U);
	BoundSlotRegistry slots(1U, 1U, 32U, budget);
	const auto slot = slots.declare(slots.root(), "n");
	slots.seal();
	MaterializedByteReservation other(&budget);
	other.set(budget.available());
	BindingSnapshot snapshot;
	const auto before = budget.used();
	const std::array<BoundPropertyRequest, 1U> requests{{{slot, "rank"}}};
	EXPECT_THROW((BoundPropertyBindings(slots, snapshot, requests, 1U, 32U, budget)), MaterializedByteLimitExceeded);
	EXPECT_EQ(budget.used(), before);
	other.reset();
	EXPECT_NO_THROW((BoundPropertyBindings(slots, snapshot, requests, 1U, 32U, budget)));
}

TEST(BoundPropertyBindingsTest, CancellationAtEveryConstructionCheckpointPublishesNothingAndRefundsCapacity) {
	MaterializedByteBudget budget(65536U);
	BoundSlotRegistry slots(1U, 1U, 32U, budget);
	const auto slot = slots.declare(slots.root(), "n");
	slots.seal();
	BindingSnapshot snapshot;
	const std::array<BoundPropertyRequest, 2U> requests{{{slot, "rank"}, {slot, "missing"}}};
	size_t checkpoints = 0U;
	const auto before = budget.used();
	{ BoundPropertyBindings table(slots, snapshot, requests, 2U, 32U, budget,
		std::pmr::get_default_resource(), [&] { ++checkpoints; }); }
	for (size_t rejected = 1U; rejected <= checkpoints; ++rejected) {
		size_t calls = 0U;
		EXPECT_THROW((BoundPropertyBindings(slots, snapshot, requests, 2U, 32U, budget,
			std::pmr::get_default_resource(), [&] { if (++calls == rejected) throw std::runtime_error("cancel binding"); })), std::runtime_error);
		EXPECT_EQ(budget.used(), before);
	}
}
} // namespace
} // namespace graph::query::execution::detail
