/**
 * @file test_InitialBTreeBuilder.cpp
 * @brief In-memory initial B+Tree layout tests.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

#include "graph/storage/relocatable/index/InitialBTreeBuilder.hpp"
#include "graph/query/execution/PrepaidMemoryResource.hpp"
#include "graph/utils/OwnedPmrAllocator.hpp"

using namespace graph::storage;

namespace {

	std::vector<std::byte> keyFor(uint64_t value) {
		std::vector<std::byte> key(sizeof(value));
		for (size_t index = 0U; index < key.size(); ++index) {
			const size_t shift = (key.size() - 1U - index) * 8U;
			key[index] = static_cast<std::byte>((value >> shift) & 0xFFU);
		}
		return key;
	}

	std::vector<BTreePageEntry> entries(size_t count) {
		std::vector<BTreePageEntry> result;
		result.reserve(count);
		for (size_t ordinal = 0U; ordinal < count; ++ordinal) {
			result.push_back({keyFor(ordinal + 1U), static_cast<uint64_t>(ordinal + 1U)});
		}
		return result;
	}

	std::vector<BTreePageEntry> entriesRequiringMultipleLeaves() {
		constexpr size_t packedEntryCapacity = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES - RSS_SLOTTED_PAGE_HEADER_BYTES -
											   2U * RSS_SLOTTED_PAGE_SLOT_BYTES - RSS_BTREE_PAGE_HEADER_BYTES;
		std::vector<BTreePageEntry> result;
		for (size_t ordinal = 1U;; ++ordinal) {
			result.push_back({keyFor(ordinal), static_cast<uint64_t>(ordinal)});
			if (PackedBTreePageCodec::encode(BTreeNodeKind::BPN_LEAF, result).size() > packedEntryCapacity) {
				return result;
			}
			if (ordinal == std::numeric_limits<size_t>::max()) {
				throw std::runtime_error("B+Tree leaf capacity cannot be exceeded safely");
			}
		}
	}

	std::vector<PageId> pageIdsFor(const InitialBTreePlan &plan) {
		std::vector<PageId> result;
		result.reserve(plan.nodes.size());
		for (size_t ordinal = 0U; ordinal < plan.nodes.size(); ++ordinal) {
			result.emplace_back(1000U + ordinal);
		}
		return result;
	}

	std::vector<Generation> generationsFor(const InitialBTreePlan &plan) {
		return std::vector<Generation>(plan.nodes.size(), Generation(1U));
	}

} // namespace

TEST(InitialBTreeBuilderTest, PlansAndMaterializesMultiLeafTreeWithStableSiblingLinks) {
	const std::vector<BTreePageEntry> source = entriesRequiringMultipleLeaves();
	const InitialBTreePlan plan = InitialBTreeBuilder::plan(71U, source);
	ASSERT_GT(plan.leaf_count, 1U);
	ASSERT_LT(plan.root_ordinal, plan.nodes.size());
	std::vector<PageId> pageIds;
	std::vector<Generation> generations;
	pageIds.reserve(plan.nodes.size());
	generations.reserve(plan.nodes.size());
	for (size_t ordinal = 0U; ordinal < plan.nodes.size(); ++ordinal) {
		pageIds.emplace_back(1000U + ordinal);
		generations.emplace_back(Generation(1U));
	}

	const std::vector<BTreePage> pages =
			InitialBTreeBuilder::materialize(71U, plan, pageIds, generations, LogSequenceNumber(9U));
	ASSERT_EQ(pages.size(), plan.nodes.size());
	size_t entryCount = 0U;
	for (size_t ordinal = 0U; ordinal < plan.leaf_count; ++ordinal) {
		const BTreePageHeader header = pages[ordinal].header();
		EXPECT_EQ(header.kind, BTreeNodeKind::BPN_LEAF);
		EXPECT_EQ(header.level, 0U);
		EXPECT_EQ(header.left_sibling, ordinal == 0U ? PageId{} : pageIds[ordinal - 1U]);
		EXPECT_EQ(header.right_sibling, ordinal + 1U == plan.leaf_count ? PageId{} : pageIds[ordinal + 1U]);
		entryCount += pages[ordinal].entries().size();
	}
	EXPECT_EQ(entryCount, source.size());
	const BTreePageHeader root = pages[plan.root_ordinal].header();
	EXPECT_EQ(root.page_id, pageIds[plan.root_ordinal]);
	EXPECT_EQ(root.level, plan.nodes[plan.root_ordinal].level);
	EXPECT_EQ(pages[plan.root_ordinal].slottedPage().ownerCommittedLsn(), LogSequenceNumber(9U));
}

TEST(InitialBTreeBuilderTest, MaterializesKeyOnlyLeavesWithoutEncodingLeafValues) {
	std::vector<BTreePageEntry> source = entries(3U);
	for (BTreePageEntry &entry: source) {
		entry.value_or_child = 0U;
	}
	const InitialBTreePlan plan = InitialBTreeBuilder::plan(71U, source, BTreeLeafValueLayout::BLVL_EMPTY);
	EXPECT_EQ(plan.leaf_value_layout, BTreeLeafValueLayout::BLVL_EMPTY);
	const std::vector<BTreePage> pages =
			InitialBTreeBuilder::materialize(71U, plan, pageIdsFor(plan), generationsFor(plan), LogSequenceNumber(9U));
	ASSERT_EQ(pages.size(), plan.nodes.size());
	for (size_t ordinal = 0U; ordinal < plan.leaf_count; ++ordinal) {
		EXPECT_EQ(pages[ordinal].header().leaf_value_layout, BTreeLeafValueLayout::BLVL_EMPTY);
		EXPECT_EQ(pages[ordinal].entries(), plan.nodes[ordinal].leaf_entries);
	}

	EXPECT_THROW(static_cast<void>(InitialBTreeBuilder::plan(71U, entries(1U), BTreeLeafValueLayout::BLVL_EMPTY)),
				 FormatException);
	InitialBTreePlan malformed = plan;
	malformed.leaf_value_layout = static_cast<BTreeLeafValueLayout>(99U);
	EXPECT_THROW(static_cast<void>(InitialBTreeBuilder::materialize(71U, malformed, pageIdsFor(malformed),
																	generationsFor(malformed), LogSequenceNumber(9U))),
				 FormatException);
}

TEST(InitialBTreeBuilderTest, RejectsUnorderedOrEmptyInputBeforeMaterialization) {
	EXPECT_THROW(static_cast<void>(InitialBTreeBuilder::plan(0U, entries(1U))), FormatException);
	const std::vector<BTreePageEntry> empty;
	EXPECT_THROW(static_cast<void>(InitialBTreeBuilder::plan(71U, empty)), FormatException);
	std::vector<BTreePageEntry> unordered = entries(2U);
	std::swap(unordered[0], unordered[1]);
	EXPECT_THROW(static_cast<void>(InitialBTreeBuilder::plan(71U, unordered)), FormatException);
	const std::vector<BTreePageEntry> emptyKey = {{std::vector<std::byte>{}, 1U}};
	EXPECT_THROW(static_cast<void>(InitialBTreeBuilder::plan(71U, emptyKey)), FormatException);
	const std::vector<BTreePageEntry> zeroValue = {{keyFor(1U), 0U}};
	EXPECT_THROW(static_cast<void>(InitialBTreeBuilder::plan(71U, zeroValue)), FormatException);
}

TEST(InitialBTreeBuilderTest, PlansNarrowInternalLevelsWithoutInvalidatingChildKeys) {
	std::vector<BTreePageEntry> source;
	source.reserve(34U);
	for (uint8_t ordinal = 0U; ordinal < 34U; ++ordinal) {
		source.push_back({std::vector<std::byte>(2400U, static_cast<std::byte>('a' + ordinal)), ordinal + 1U});
	}

	const InitialBTreePlan plan = InitialBTreeBuilder::plan(73U, source);
	EXPECT_GT(plan.nodes.size(), source.size());
	for (const InitialBTreePlanNode &node: plan.nodes) {
		EXPECT_FALSE(node.first_key.empty());
	}
}

TEST(InitialBTreeBuilderTest, RejectsAValidlyOrderedEntryThatCannotFitInAnyLeaf) {
	const std::vector<BTreePageEntry> oversized = {
			{std::vector<std::byte>(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, std::byte{'x'}), 1U}};
	EXPECT_THROW(static_cast<void>(InitialBTreeBuilder::plan(71U, oversized)), FormatException);
}

TEST(InitialBTreeBuilderTest, RejectsInvalidMaterializationInputsBeforeWritingPages) {
	const InitialBTreePlan plan = InitialBTreeBuilder::plan(71U, entries(1U));
	const std::vector<PageId> ids = pageIdsFor(plan);
	const std::vector<Generation> generations = generationsFor(plan);
	EXPECT_THROW(static_cast<void>(InitialBTreeBuilder::materialize(0U, plan, ids, generations, LogSequenceNumber(1U))),
				 FormatException);
	EXPECT_THROW(static_cast<void>(InitialBTreeBuilder::materialize(71U, plan, {}, generations, LogSequenceNumber(1U))),
				 FormatException);
	EXPECT_THROW(static_cast<void>(InitialBTreeBuilder::materialize(71U, plan, ids, generations, LogSequenceNumber{})),
				 FormatException);

	std::vector<PageId> invalidIds = ids;
	invalidIds.front() = {};
	EXPECT_THROW(static_cast<void>(
						 InitialBTreeBuilder::materialize(71U, plan, invalidIds, generations, LogSequenceNumber(1U))),
				 FormatException);
	std::vector<Generation> invalidGenerations = generations;
	invalidGenerations.front() = {};
	EXPECT_THROW(static_cast<void>(
						 InitialBTreeBuilder::materialize(71U, plan, ids, invalidGenerations, LogSequenceNumber(1U))),
				 FormatException);
}

TEST(InitialBTreeBuilderTest, RejectsEveryPubliclyConstructibleMalformedPlanNode) {
	InitialBTreePlan malformedLeaf = InitialBTreeBuilder::plan(71U, entries(1U));
	std::vector<PageId> leafIds = pageIdsFor(malformedLeaf);
	std::vector<Generation> leafGenerations = generationsFor(malformedLeaf);
	malformedLeaf.nodes.front().level = 1U;
	EXPECT_THROW(static_cast<void>(InitialBTreeBuilder::materialize(71U, malformedLeaf, leafIds, leafGenerations,
																	LogSequenceNumber(1U))),
				 FormatException);

	InitialBTreePlan emptyLeaf = InitialBTreeBuilder::plan(71U, entries(1U));
	leafIds = pageIdsFor(emptyLeaf);
	leafGenerations = generationsFor(emptyLeaf);
	emptyLeaf.nodes.front().leaf_entries.clear();
	EXPECT_THROW(static_cast<void>(InitialBTreeBuilder::materialize(71U, emptyLeaf, leafIds, leafGenerations,
																	LogSequenceNumber(1U))),
				 FormatException);

	InitialBTreePlan inconsistentLeafKey = InitialBTreeBuilder::plan(71U, entries(1U));
	leafIds = pageIdsFor(inconsistentLeafKey);
	leafGenerations = generationsFor(inconsistentLeafKey);
	inconsistentLeafKey.nodes.front().first_key = keyFor(2U);
	EXPECT_THROW(static_cast<void>(InitialBTreeBuilder::materialize(71U, inconsistentLeafKey, leafIds, leafGenerations,
																	LogSequenceNumber(1U))),
				 FormatException);

	const InitialBTreePlan source = InitialBTreeBuilder::plan(71U, entriesRequiringMultipleLeaves());
	ASSERT_LT(source.leaf_count, source.nodes.size());
	ASSERT_GE(source.nodes[source.root_ordinal].children.size(), 2U);
	auto expectMalformedInternal = [&](InitialBTreePlan candidate) {
		const std::vector<PageId> ids = pageIdsFor(candidate);
		const std::vector<Generation> generations = generationsFor(candidate);
		EXPECT_THROW(static_cast<void>(
							 InitialBTreeBuilder::materialize(71U, candidate, ids, generations, LogSequenceNumber(1U))),
					 FormatException);
	};

	InitialBTreePlan zeroLevel = source;
	zeroLevel.nodes[zeroLevel.root_ordinal].level = 0U;
	expectMalformedInternal(std::move(zeroLevel));
	InitialBTreePlan inconsistentInternalKey = source;
	inconsistentInternalKey.nodes[inconsistentInternalKey.root_ordinal].first_key = keyFor(0U);
	expectMalformedInternal(std::move(inconsistentInternalKey));
	InitialBTreePlan noChildren = source;
	noChildren.nodes[noChildren.root_ordinal].children.clear();
	expectMalformedInternal(std::move(noChildren));
	InitialBTreePlan invalidLeftmost = source;
	invalidLeftmost.nodes[invalidLeftmost.root_ordinal].children.front() = invalidLeftmost.root_ordinal;
	expectMalformedInternal(std::move(invalidLeftmost));
	InitialBTreePlan invalidRightChild = source;
	const size_t rightChild = invalidRightChild.nodes[invalidRightChild.root_ordinal].children[1U];
	invalidRightChild.nodes[rightChild].first_key.clear();
	expectMalformedInternal(std::move(invalidRightChild));
	InitialBTreePlan outOfRangeRightChild = source;
	outOfRangeRightChild.nodes[outOfRangeRightChild.root_ordinal].children[1U] = outOfRangeRightChild.root_ordinal;
	expectMalformedInternal(std::move(outOfRangeRightChild));
}

TEST(InitialBTreeBuilderTest, SizePlanningMatchesCanonicalPackedBoundariesAndMaterializesAllValues) {
	constexpr size_t capacity = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES - RSS_SLOTTED_PAGE_HEADER_BYTES -
			2U * RSS_SLOTTED_PAGE_SLOT_BYTES - RSS_BTREE_PAGE_HEADER_BYTES;
	for (const auto layout: {BTreeLeafValueLayout::BLVL_U64, BTreeLeafValueLayout::BLVL_EMPTY}) {
		std::vector<BTreePageEntry> source;
		for (size_t i = 0U; i < 1800U; ++i) {
			auto key = keyFor(i + 1U);
			key.resize(8U + (i % 37U) * 13U, static_cast<std::byte>(i % 251U));
			source.push_back({std::move(key), layout == BTreeLeafValueLayout::BLVL_EMPTY ? 0U :
					(i % 3U == 0U ? std::numeric_limits<uint64_t>::max() : i + 1U)});
		}
		const auto plan = InitialBTreeBuilder::plan(83U, source, layout);
		const auto pages = InitialBTreeBuilder::materialize(83U, plan, pageIdsFor(plan), generationsFor(plan), LogSequenceNumber(11U));
		size_t consumed = 0U;
		for (size_t leaf = 0U; leaf < plan.leaf_count; ++leaf) {
			const auto actual = pages[leaf].entries();
			ASSERT_FALSE(actual.empty());
			ASSERT_LE(consumed + actual.size(), source.size());
			EXPECT_TRUE(std::equal(actual.begin(), actual.end(), source.begin() + consumed));
			EXPECT_LE(PackedBTreePageCodec::encode(BTreeNodeKind::BPN_LEAF, actual, layout).size(), capacity);
			consumed += actual.size();
			if (consumed < source.size()) {
				auto overfull = actual;
				overfull.push_back(source[consumed]);
				EXPECT_GT(PackedBTreePageCodec::encode(BTreeNodeKind::BPN_LEAF, overfull, layout).size(), capacity);
			}
		}
		EXPECT_EQ(consumed, source.size());
	}
}

TEST(InitialBTreeBuilderTest, GrowingPlanNodesPreservesLeafBackingUnderTightWriteCredit) {
 using namespace graph::query::execution::detail;
 constexpr size_t maximum = 2U * 1024U * 1024U;
 auto process = std::make_shared<ProcessResourceBudget>(ResourceLimits::uniform(maximum));
 auto database = std::make_shared<DatabaseResourceBudget>(process, ResourceLimits::uniform(maximum));
 auto account = std::make_shared<ResourceAccount>(database, ResourceClass::RCK_WRITE, maximum);
 auto memory = std::make_shared<PrepaidMemoryResource>(account, maximum);
 {
  graph::utils::OwnedAllocationScope scope(memory);
  InitialBTreePlan plan;
  InitialBTreePlanNode leaf;
  leaf.leaf_entries = entries(2048U);
  plan.nodes.push_back(std::move(leaf));
  const auto *backing = plan.nodes.front().leaf_entries.data();
  const auto retained = memory->used();
  ASSERT_GT(retained, 65536U);
  PrepaidMemoryResource::ExternalReservation pressure(memory, memory->capacity() - retained - 65536U);
  const auto beforeGrowth = memory->used();
  EXPECT_NO_THROW({for (size_t ordinal = 0U; ordinal < 128U; ++ordinal) plan.nodes.emplace_back();});
  EXPECT_GE(memory->used() - beforeGrowth, 64U * sizeof(InitialBTreePlanNode));
  ASSERT_EQ(plan.nodes.size(), 129U);
  EXPECT_EQ(plan.nodes.front().leaf_entries.data(), backing);
  ASSERT_EQ(plan.nodes.front().leaf_entries.size(), 2048U);
  for (size_t ordinal = 0U; ordinal < 2048U; ++ordinal) {
   EXPECT_EQ(plan.nodes.front().leaf_entries[ordinal].value_or_child, ordinal + 1U);
   EXPECT_EQ(plan.nodes.front().leaf_entries[ordinal].key, keyFor(ordinal + 1U));
  }
 }
 EXPECT_EQ(memory->used(), 0U);
}

TEST(InitialBTreeBuilderTest, StreamingPlanAvoidsCompleteInputOverlapAndCompactsEveryLeaf) {
 using namespace graph::query::execution::detail;
 constexpr size_t count = 20000U;
 const size_t maximum = count * (sizeof(BTreePageEntry) + sizeof(uint64_t) +
     graph::utils::detail::standardContainerProxyBytes()) + 512U * 1024U;
 auto process = std::make_shared<ProcessResourceBudget>(ResourceLimits::uniform(maximum));
 auto database = std::make_shared<DatabaseResourceBudget>(process, ResourceLimits::uniform(maximum));
 auto account = std::make_shared<ResourceAccount>(database, ResourceClass::RCK_WRITE, maximum);
 auto memory = std::make_shared<PrepaidMemoryResource>(account, maximum);
 {
  graph::utils::OwnedAllocationScope scope(memory);
  {
   graph::utils::OwnedStdVector<BTreePageEntry> gathered;
   gathered.reserve(count);
   for (size_t index=0U;index<count;++index) gathered.push_back({keyFor(index+1U),index+1U});
   const auto retained=memory->used();
   EXPECT_THROW(static_cast<void>(InitialBTreeBuilder::plan(71U,gathered)),ResourceAdmissionDenied);
   EXPECT_EQ(memory->used(),retained);
  }
  size_t ordinal=0U;
  const auto plan=InitialBTreeBuilder::planFromCursor(71U,[&]() -> std::optional<BTreePageEntry> {
   if(ordinal==count)return std::nullopt;
   ++ordinal;return BTreePageEntry{keyFor(ordinal),ordinal};
  });
  ASSERT_EQ(ordinal,count);
  size_t seen=0U;
  for(size_t leaf=0U;leaf<plan.leaf_count;++leaf) {
   EXPECT_EQ(plan.nodes[leaf].leaf_entries.capacity(),plan.nodes[leaf].leaf_entries.size());
   for(const auto &entry:plan.nodes[leaf].leaf_entries) {
    ++seen;EXPECT_EQ(entry.key,keyFor(seen));EXPECT_EQ(entry.value_or_child,seen);
   }
  }
  EXPECT_EQ(seen,count);
  EXPECT_LT(memory->used(),maximum);
 }
 EXPECT_EQ(memory->used(),0U);
}

TEST(InitialBTreeBuilderTest, StreamingSourceFailureAndInvalidInputsReleaseAllPrivatePlanBytes) {
 using namespace graph::query::execution::detail;
 constexpr size_t maximum=1024U*1024U;
 auto process=std::make_shared<ProcessResourceBudget>(ResourceLimits::uniform(maximum));
 auto database=std::make_shared<DatabaseResourceBudget>(process,ResourceLimits::uniform(maximum));
 auto account=std::make_shared<ResourceAccount>(database,ResourceClass::RCK_WRITE,maximum);
 auto memory=std::make_shared<PrepaidMemoryResource>(account,maximum);
 {
  graph::utils::OwnedAllocationScope scope(memory);
  size_t calls=0U;
  EXPECT_THROW(static_cast<void>(InitialBTreeBuilder::planFromCursor(0U,[&]() -> std::optional<BTreePageEntry>{++calls;return std::nullopt;})),FormatException);
  EXPECT_EQ(calls,0U);
  EXPECT_THROW(static_cast<void>(InitialBTreeBuilder::planFromCursor(71U,[]() -> std::optional<BTreePageEntry>{return std::nullopt;})),FormatException);
  EXPECT_EQ(memory->used(),0U);
  EXPECT_THROW(static_cast<void>(InitialBTreeBuilder::planFromCursor(71U,[&]() -> std::optional<BTreePageEntry> {
   if(++calls==1000U)throw std::runtime_error("source rejected");
   return BTreePageEntry{keyFor(calls),calls};
  })),std::runtime_error);
  EXPECT_EQ(memory->used(),0U);
  calls=0U;
  EXPECT_THROW(static_cast<void>(InitialBTreeBuilder::planFromCursor(71U,[&]() -> std::optional<BTreePageEntry> {
   if(calls++==2U)return std::nullopt;
   return BTreePageEntry{keyFor(2U-calls),1U};
  })),FormatException);
 }
 EXPECT_EQ(memory->used(),0U);
}
