/**
 * @file test_NativeGraphWritePlanner_FreshPoolReplacement.cpp
 * @brief COW replacement placement tests for full native Entity pools.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include <vector>

#include <gtest/gtest.h>

#include "graph/storage/relocatable/entity/NativeGraphWritePlanner.hpp"
#include "storage/relocatable/entity/NativeGraphWritePlannerTestFixture.hpp"

using namespace graph::storage;

namespace {

	NativeGraphWritePlannerOptions plannerOptions() {
		NativeGraphWritePlannerOptions options;
		options.first_slot = PhysicalSlotId(40U);
		options.last_slot = PhysicalSlotId(80U);
		options.entity_pool_owner_id = 901U;
		options.property_roots = {PhysicalSlotId(40U), PhysicalSlotId(80U), 902U};
		return options;
	}

	MetadataPublishOptions publishOptions(BootstrapPublishFailpoint failpoint) {
		MetadataPublishOptions options;
		options.bootstrap_failpoint = failpoint;
		return options;
	}

} // namespace

class NativeGraphWritePlannerFreshPoolReplacementTest
		: public graph::storage::test::NativeGraphWritePlannerTestFixture {};

TEST_F(NativeGraphWritePlannerFreshPoolReplacementTest, MovesStableNodePageIntoFreshPoolWhenItsCurrentPoolIsFull) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	const std::array<uint64_t, 0U> noLabels{};
	NativeGraphWriteTransaction create = planner.beginTransaction();
	const size_t nodeCount = static_cast<size_t>(RSS_PAGE_POOL_DATA_FRAME_COUNT) *
						 EntityPage::recordCapacity(EntityPageRecordKind::EPRK_NODE);
	std::vector<uint64_t> nodeIds;
	nodeIds.reserve(nodeCount);
	for (size_t ordinal = 0U; ordinal < nodeCount; ++ordinal) {
		nodeIds.push_back(create.createNode(noLabels));
	}
	ASSERT_TRUE(create.commit().committed());

	MetadataSnapshotPin before = metadata_->openSnapshot();
	ASSERT_EQ(entities_->currentPools(before).size(), 1U);
	const EntityPageLocation oldEntityLocation = metadata_->findNodePageLocation(before, nodeIds.front());
	const PageLocation oldPageLocation = metadata_->findPageLocation(before, oldEntityLocation.page_id);

	NativeGraphWriteTransaction update = planner.beginTransaction();
	const std::array<uint64_t, 1U> labels = {7U};
	update.replaceNodeLabels(nodeIds.front(), labels);
	ASSERT_TRUE(update.commit().committed());

	EXPECT_EQ(labels_->readNodeLabels(before, entities_->readNode(before, nodeIds.front())),
			  std::vector<uint64_t>{});
	MetadataSnapshotPin after = metadata_->openSnapshot();
	const EntityPageLocation newEntityLocation = metadata_->findNodePageLocation(after, nodeIds.front());
	const PageLocation newPageLocation = metadata_->findPageLocation(after, newEntityLocation.page_id);
	EXPECT_EQ(newEntityLocation.page_id, oldEntityLocation.page_id);
	EXPECT_NE(newPageLocation.segment_id, oldPageLocation.segment_id);
	EXPECT_EQ(labels_->readNodeLabels(after, entities_->readNode(after, nodeIds.front())),
			  std::vector<uint64_t>(labels.begin(), labels.end()));
	EXPECT_EQ(entities_->currentPools(after).size(), 2U);
	after.release();
	before.release();
}

TEST_F(NativeGraphWritePlannerFreshPoolReplacementTest,
		RecoversEitherWholeGenerationWhenFreshPoolReplacementPublicationIsInterrupted) {
	const auto assertRecovered = [&](BootstrapPublishFailpoint failpoint, bool expectReplacement) {
		NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
		const std::array<uint64_t, 0U> noLabels{};
		NativeGraphWriteTransaction create = planner.beginTransaction();
		const size_t nodeCount = static_cast<size_t>(RSS_PAGE_POOL_DATA_FRAME_COUNT) *
							 EntityPage::recordCapacity(EntityPageRecordKind::EPRK_NODE);
		std::vector<uint64_t> nodeIds;
		nodeIds.reserve(nodeCount);
		for (size_t ordinal = 0U; ordinal < nodeCount; ++ordinal) {
			nodeIds.push_back(create.createNode(noLabels));
		}
		ASSERT_TRUE(create.commit().committed());
		MetadataSnapshotPin beforeReplacement = metadata_->openSnapshot();
		const size_t priorPoolCount = entities_->currentPools(beforeReplacement).size();
		ASSERT_GE(priorPoolCount, 1U);
		beforeReplacement.release();

		NativeGraphWriteTransaction update = planner.beginTransaction();
		const std::array<uint64_t, 1U> replacementLabels = {7U};
		update.replaceNodeLabels(nodeIds.front(), replacementLabels);
		EXPECT_THROW(static_cast<void>(update.commit(publishOptions(failpoint))), BootstrapPublishInterrupted);
		EXPECT_EQ(update.state(), NativeGraphWriteTransactionState::NGWTS_INDETERMINATE);

		reopenNativeStores();
		MetadataSnapshotPin recovered = metadata_->openSnapshot();
		EXPECT_EQ(labels_->readNodeLabels(recovered, entities_->readNode(recovered, nodeIds.front())),
				  expectReplacement ? std::vector<uint64_t>{7U} : std::vector<uint64_t>{});
		EXPECT_EQ(entities_->currentPools(recovered).size(), priorPoolCount + (expectReplacement ? 1U : 0U));
		recovered.release();
	};

	assertRecovered(BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE, false);
	assertRecovered(BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE, false);
	assertRecovered(BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE, true);
}
