#include <gtest/gtest.h>
#include <memory>
#include "query/execution/BoundSlotRegistry.hpp"

using graph::query::execution::MaterializedByteBudget;
using graph::query::execution::detail::BoundSlotRegistry;
using graph::query::execution::detail::BindingScopeVisibility;

TEST(BoundSlotRegistryTest, DeclarationAndLookupProduceStablePlanSlots) {
	MaterializedByteBudget budget(16384);
	BoundSlotRegistry slots(32, 16, 1024, budget);
	const auto user = slots.declare(slots.root(), "u");
	const auto age = slots.declare(slots.root(), "age");
	EXPECT_EQ(user.index, 0U);
	EXPECT_EQ(age.index, 1U);
	EXPECT_EQ(slots.resolve(slots.root(), "u"), user);
	EXPECT_EQ(slots.name(user), "u");
	EXPECT_EQ(slots.origin(age), slots.root());
	EXPECT_FALSE(slots.resolve(slots.root(), "missing"));
	EXPECT_THROW((void) slots.declare(slots.root(), "u"), std::invalid_argument);
	EXPECT_THROW((void) slots.declare(slots.root(), ""), std::invalid_argument);
	EXPECT_EQ(slots.slotCount(), 2U);
}

TEST(BoundSlotRegistryTest, LexicalShadowingDoesNotOverwriteOuterBinding) {
	MaterializedByteBudget budget(16384);
	BoundSlotRegistry slots(32, 16, 1024, budget);
	const auto outer = slots.declare(slots.root(), "u");
	const auto child = slots.createScope(slots.root(), BindingScopeVisibility::BSV_LEXICAL);
	EXPECT_EQ(slots.resolve(child, "u"), outer);
	const auto inner = slots.declare(child, "u");
	EXPECT_NE(inner, outer);
	EXPECT_EQ(slots.resolve(child, "u"), inner);
	EXPECT_EQ(slots.resolve(slots.root(), "u"), outer);
	EXPECT_EQ(slots.origin(inner), child);
}

TEST(BoundSlotRegistryTest, IsolatedSubqueryRequiresExplicitCorrelatedImport) {
	MaterializedByteBudget budget(16384);
	BoundSlotRegistry slots(32, 16, 1024, budget);
	const auto outer = slots.declare(slots.root(), "u");
	const auto child = slots.createScope(slots.root(), BindingScopeVisibility::BSV_ISOLATED);
	EXPECT_FALSE(slots.resolve(child, "u"));
	EXPECT_EQ(slots.importFromParent(child, "parent", "u"), outer);
	EXPECT_EQ(slots.resolve(child, "parent"), outer);
	EXPECT_EQ(slots.slotCount(), 1U);
	EXPECT_EQ(slots.bindingCount(), 2U);
	EXPECT_FALSE(slots.resolve(child, "u"));
	EXPECT_THROW((void) slots.importFromParent(child, "x", "unknown"), std::invalid_argument);
	EXPECT_THROW((void) slots.importFromParent(slots.root(), "x", "u"), std::invalid_argument);
}

TEST(BoundSlotRegistryTest, ImportsCannotJumpAcrossAnIsolatedScopeOrSeeSiblingLocals) {
	MaterializedByteBudget budget(16384);
	BoundSlotRegistry slots(32, 16, 1024, budget);
	const auto rootSlot = slots.declare(slots.root(), "u");
	const auto parent = slots.createScope(slots.root(), BindingScopeVisibility::BSV_ISOLATED);
	const auto child = slots.createScope(parent, BindingScopeVisibility::BSV_ISOLATED);
	EXPECT_THROW((void) slots.importFromParent(child, "u", "u"), std::invalid_argument);
	(void) slots.importFromParent(parent, "u", "u");
	EXPECT_EQ(slots.importFromParent(child, "u", "u"), rootSlot);
	const auto sibling = slots.createScope(slots.root(), BindingScopeVisibility::BSV_LEXICAL);
	(void) slots.declare(parent, "private");
	EXPECT_FALSE(slots.resolve(sibling, "private"));
}

TEST(BoundSlotRegistryTest, ForeignAndInvalidIdentifiersAreRejected) {
	MaterializedByteBudget budget(16384);
	BoundSlotRegistry first(32, 16, 1024, budget), second(32, 16, 1024, budget);
	const auto slot = first.declare(first.root(), "u");
	EXPECT_THROW((void) second.name(slot), std::invalid_argument);
	EXPECT_THROW((void) second.resolve(first.root(), "u"), std::invalid_argument);
	auto invalid = slot;
	invalid.index = 100;
	EXPECT_THROW((void) first.origin(invalid), std::invalid_argument);
	EXPECT_THROW((void) first.createScope(first.root(), static_cast<BindingScopeVisibility>(99)), std::invalid_argument);
}

TEST(BoundSlotRegistryTest, SealedLayoutRemainsReadableAndForbidsEveryMutation) {
	MaterializedByteBudget budget(16384);
	BoundSlotRegistry slots(32, 16, 1024, budget);
	const auto slot = slots.declare(slots.root(), "u");
	const auto child = slots.createScope(slots.root(), BindingScopeVisibility::BSV_ISOLATED);
	slots.seal();
	slots.seal();
	EXPECT_TRUE(slots.sealed());
	EXPECT_EQ(slots.resolve(slots.root(), "u"), slot);
	EXPECT_EQ(slots.name(slot), "u");
	EXPECT_THROW((void) slots.declare(child, "v"), std::logic_error);
	EXPECT_THROW((void) slots.importFromParent(child, "v", "u"), std::logic_error);
	EXPECT_THROW((void) slots.createScope(child, BindingScopeVisibility::BSV_LEXICAL), std::logic_error);
}

TEST(BoundSlotRegistryTest, BindingScopeAndNameLimitsAlsoCountImportedAliases) {
	MaterializedByteBudget budget(16384);
	BoundSlotRegistry slots(2, 2, 2, budget);
	(void) slots.declare(slots.root(), "u");
	const auto child = slots.createScope(slots.root(), BindingScopeVisibility::BSV_ISOLATED);
	EXPECT_THROW((void) slots.importFromParent(child, "too-long", "u"), std::runtime_error);
	(void) slots.importFromParent(child, "v", "u");
	EXPECT_THROW((void) slots.declare(child, "x"), std::runtime_error);
	EXPECT_THROW((void) slots.createScope(child, BindingScopeVisibility::BSV_LEXICAL), std::runtime_error);
	EXPECT_EQ(slots.bindingCount(), 2U);
	EXPECT_EQ(slots.slotCount(), 1U);
}

namespace {
class BindingFailureResource final : public std::pmr::memory_resource {
public:
	size_t remaining = (std::numeric_limits<size_t>::max)();
private:
	void *do_allocate(size_t bytes, size_t alignment) override {
		if (remaining == 0U) throw std::bad_alloc();
		--remaining;
		return std::pmr::new_delete_resource()->allocate(bytes, alignment);
	}
	void do_deallocate(void *pointer, size_t bytes, size_t alignment) override {
		std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
	}
	bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return this == &other; }
};
}

TEST(BoundSlotRegistryTest, AllocationFailureAtEachDeclarationStagePreservesBindingsAndAllowsRetry) {
	for (size_t allowed = 0; allowed < 6; ++allowed) {
		MaterializedByteBudget budget(65536);
		BindingFailureResource resource;
		BoundSlotRegistry slots(32, 16, 1024, budget, &resource);
		const auto existing = slots.declare(slots.root(), "u");
		const std::string name(128, 'x');
		resource.remaining = allowed;
		try { (void) slots.declare(slots.root(), name); }
		catch (const std::bad_alloc &) {
			EXPECT_EQ(slots.slotCount(), 1U);
			EXPECT_EQ(slots.bindingCount(), 1U);
			EXPECT_FALSE(slots.resolve(slots.root(), name));
			resource.remaining = (std::numeric_limits<size_t>::max)();
			EXPECT_EQ(slots.declare(slots.root(), name).index, 1U);
		}
		EXPECT_EQ(slots.resolve(slots.root(), "u"), existing);
		EXPECT_EQ(slots.slotCount(), 2U);
		EXPECT_EQ(slots.bindingCount(), 2U);
	}
}

TEST(BoundSlotRegistryTest, ScopeAndDescriptorGrowthPreserveIdsAndRefundAllOwnedMemory) {
	MaterializedByteBudget budget(1024 * 1024);
	{
		BoundSlotRegistry slots(512, 256, 65536, budget);
		const auto root = slots.declare(slots.root(), "u");
		for (size_t index = 0; index < 100; ++index) {
			const auto child = slots.createScope(slots.root(), BindingScopeVisibility::BSV_LEXICAL);
			const auto name = "long-variable-name-" + std::to_string(index);
			const auto slot = slots.declare(child, name);
			EXPECT_EQ(slots.name(slot), name);
			EXPECT_EQ(slots.resolve(child, "u"), root);
		}
		EXPECT_EQ(slots.name(root), "u");
		EXPECT_GT(budget.used(), 0U);
	}
	EXPECT_EQ(budget.used(), 0U);
}

TEST(BoundSlotRegistryTest, InvalidLimitsAndConstructorAllocationFailureDoNotLeakCharges) {
	MaterializedByteBudget budget(16384);
	EXPECT_THROW((BoundSlotRegistry(0, 1, 1, budget)), std::invalid_argument);
	EXPECT_THROW((BoundSlotRegistry(1, 0, 1, budget)), std::invalid_argument);
	EXPECT_THROW((BoundSlotRegistry(1, 1, 0, budget)), std::invalid_argument);
	EXPECT_THROW((BoundSlotRegistry(1, 1, 1, budget, nullptr)), std::invalid_argument);
	EXPECT_THROW((BoundSlotRegistry(1, 1, 1, budget, std::pmr::null_memory_resource())), std::bad_alloc);
	EXPECT_EQ(budget.used(), 0U);
}

TEST(BoundSlotRegistryTest, SharedBudgetLimitFailsBeforePublishingANameAndRetainsOtherOwners) {
	MaterializedByteBudget budget(1);
	EXPECT_THROW((BoundSlotRegistry(1, 1, 1, budget)), std::runtime_error);
	EXPECT_EQ(budget.used(), 0U);
	MaterializedByteBudget shared(16384);
	BoundSlotRegistry existing(16, 4, 128, shared);
	const auto original = existing.declare(existing.root(), "u");
	const auto charged = shared.used();
	{
		BoundSlotRegistry second(16, 4, 128, shared);
		(void) second.declare(second.root(), "v");
		EXPECT_GT(shared.used(), charged);
	}
	EXPECT_EQ(shared.used(), charged);
	EXPECT_EQ(existing.resolve(existing.root(), "u"), original);
}

TEST(BoundSlotRegistryTest, ReusingRegistryAddressDoesNotValidateStalePlanIdentifiers) {
	MaterializedByteBudget budget(16384);
	alignas(BoundSlotRegistry) std::byte storage[sizeof(BoundSlotRegistry)];
	auto *first = std::construct_at(reinterpret_cast<BoundSlotRegistry *>(storage), 4, 4, 64, budget);
	const auto oldScope = first->root();
	const auto oldSlot = first->declare(oldScope, "u");
	std::destroy_at(first);
	auto *second = std::construct_at(reinterpret_cast<BoundSlotRegistry *>(storage), 4, 4, 64, budget);
	(void) second->declare(second->root(), "v");
	EXPECT_THROW((void) second->name(oldSlot), std::invalid_argument);
	EXPECT_THROW((void) second->resolve(oldScope, "u"), std::invalid_argument);
	std::destroy_at(second);
	EXPECT_EQ(budget.used(), 0U);
}
