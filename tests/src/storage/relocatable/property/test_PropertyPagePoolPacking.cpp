/**
 * @file test_PropertyPagePoolPacking.cpp
 * @brief Space and capacity invariants for native Property page-pool batching.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/core/Node.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/entity/EntityPageStore.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRootStore.hpp"

using namespace graph::storage;

namespace {

	Superblock initialSuperblock() {
		Superblock result;
		result.database_id = DatabaseId(0x8979323846264338ULL);
		result.generation = Generation(1U);
		result.checkpoint_lsn = LogSequenceNumber(1U);
		return result;
	}

	EntityPageNewPoolRequest nodePoolRequest() {
		return {.segment_id = SegmentId(31U),
				.pool_owner_id = 901U,
				.generation = Generation(1U),
				.page_id = PageId(101U),
				.first_slot = PhysicalSlotId(40U),
				.last_slot = PhysicalSlotId(40U)};
	}

	PropertyCollectionRootStoreOptions propertyOptions() {
		return {PhysicalSlotId(41U), PhysicalSlotId(48U), 902U};
	}

	PropertyCollectionRoot sizedRoot(uint64_t nodeId, uint32_t token, size_t encodedBytes, char fill) {
		PropertyCollectionRoot result{
				nodeId, graph::Node::typeId, {{token, graph::PropertyValue(std::string{})}}};
		const size_t baseBytes = EntityPropertyCollectionRootCodec::encode(result).size();
		if (encodedBytes <= baseBytes) {
			throw std::logic_error("Property pool-packing test requested an undersized root");
		}
		result.entries.front().value = graph::PropertyValue(std::string(encodedBytes - baseBytes, fill));
		if (EntityPropertyCollectionRootCodec::encode(result).size() != encodedBytes) {
			throw std::logic_error("Property pool-packing test failed to derive an exact root size");
		}
		return result;
	}

	EntityPageBatchNewPool nodePool(std::span<const uint64_t> nodeIds) {
		EntityPageBatchNewPool result;
		result.request = nodePoolRequest();
		result.record_kind = EntityPageRecordKind::EPRK_NODE;
		result.node_insertions.reserve(nodeIds.size());
		for (const uint64_t nodeId: nodeIds) {
			result.node_insertions.push_back({nodeId, NodeEntityRecord{}});
		}
		return result;
	}

} // namespace

class PropertyPagePoolPackingTest : public ::testing::Test {
protected:
	std::filesystem::path path_;
	std::shared_ptr<std::fstream> file_;
	std::shared_ptr<StorageIO> io_;
	std::unique_ptr<RelocatableMetadataStore> metadata_;
	std::unique_ptr<EntityPageStore> entities_;
	std::unique_ptr<SlottedPagePool> pages_;
	std::unique_ptr<PropertyCollectionRootStore> properties_;

	void SetUp() override {
		path_ = std::filesystem::temp_directory_path() /
				("property_pool_packing_" + to_string(boost::uuids::random_generator()()));
		file_ = std::make_shared<std::fstream>(
				path_, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(file_->is_open());
		io_ = std::make_shared<StorageIO>(file_, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		metadata_ = RelocatableMetadataStore::initializeNew(io_, initialSuperblock());
		entities_ = std::make_unique<EntityPageStore>(io_, *metadata_);
		pages_ = std::make_unique<SlottedPagePool>(io_, *metadata_);
		properties_ = std::make_unique<PropertyCollectionRootStore>(*pages_, *metadata_);
	}

	void TearDown() override {
		properties_.reset();
		pages_.reset();
		entities_.reset();
		metadata_.reset();
		io_.reset();
		if (file_ && file_->is_open()) {
			file_->close();
		}
		file_.reset();
		std::error_code error;
		std::filesystem::remove(path_, error);
	}

	EntityPageBatchResult publishInitial(std::span<const uint64_t> nodeIds,
										 std::span<const PropertyCollectionRoot> roots) {
		MetadataSnapshotPin snapshot = metadata_->openSnapshot();
		const std::vector<EntityPropertyRootMutationPlan> plans =
				properties_->planEntityRootReplacements(snapshot, roots, propertyOptions());
		const std::array pools{nodePool(nodeIds)};
		EntityPageBatchResult result = entities_->applyNativeGraphBatchTransaction(
				snapshot, {}, {}, pools, plans, *properties_, propertyOptions(), LogSequenceNumber(2U));
		snapshot.release();
		return result;
	}
};

TEST_F(PropertyPagePoolPackingTest, AppendsANewPageToAnExistingPoolBeforeAllocatingAnotherSegment) {
	const std::array<uint64_t, 2U> nodeIds{1001U, 1002U};
	const PropertyCollectionRoot first = sizedRoot(1001U, 3U, 3000U, 'a');
	static_cast<void>(publishInitial(nodeIds, std::span(&first, 1U)));

	MetadataSnapshotPin before = metadata_->openSnapshot();
	const std::vector<CatalogEntry> beforePools =
			metadata_->findCurrentPropertyPagePools(before, propertyOptions().page_owner_id);
	ASSERT_EQ(beforePools.size(), 1U);
	const EntityPropertyLocation firstLocation = metadata_->findEntityPropertyLocation(
			before, {EntityPropertyOwnerKind::EPOK_NODE, first.owner_entity_id});
	EXPECT_EQ(pages_->availableFrameCount(before, beforePools.front().segment_id),
			  RSS_PAGE_POOL_DATA_FRAME_COUNT - 1U);

	const PropertyCollectionRoot second = sizedRoot(1002U, 5U, 2000U, 'b');
	const std::array secondRoots{second};
	const std::vector<EntityPropertyRootMutationPlan> plans =
			properties_->planEntityRootReplacements(before, secondRoots, propertyOptions());
	ASSERT_EQ(plans.size(), 1U);
	ASSERT_EQ(plans.front().kind, EntityPropertyRootMutationPlanKind::EPRMPK_NEW_PAGE_REQUIRED);
	const EntityPageBatchResult published = entities_->applyNativeGraphBatchTransaction(
			before, {}, {}, {}, plans, *properties_, propertyOptions(), LogSequenceNumber(3U));
	before.release();
	ASSERT_EQ(published.properties.size(), 1U);

	MetadataSnapshotPin current = metadata_->openSnapshot();
	const std::vector<CatalogEntry> currentPools =
			metadata_->findCurrentPropertyPagePools(current, propertyOptions().page_owner_id);
	ASSERT_EQ(currentPools.size(), 1U);
	const EntityPropertyLocation secondLocation = metadata_->findEntityPropertyLocation(
			current, {EntityPropertyOwnerKind::EPOK_NODE, second.owner_entity_id});
	EXPECT_NE(firstLocation.page_id, secondLocation.page_id);
	EXPECT_EQ(metadata_->findPageLocation(current, firstLocation.page_id).segment_id,
			  metadata_->findPageLocation(current, secondLocation.page_id).segment_id);
	EXPECT_EQ(properties_->readEntity(current, secondLocation.key), second);
	current.release();
}

TEST_F(PropertyPagePoolPackingTest, FillsOneFreshPoolBeforeCreatingTheNextPool) {
	constexpr size_t rootCount = RSS_PAGE_POOL_DATA_FRAME_COUNT + 1U;
	std::vector<uint64_t> nodeIds;
	std::vector<PropertyCollectionRoot> roots;
	nodeIds.reserve(rootCount);
	roots.reserve(rootCount);
	for (size_t index = 0U; index < rootCount; ++index) {
		const uint64_t nodeId = 1001U + index;
		nodeIds.push_back(nodeId);
		roots.push_back(sizedRoot(nodeId, static_cast<uint32_t>(3U + 2U * index), 2000U,
									  static_cast<char>('a' + index % 26U)));
	}
	const EntityPageBatchResult published = publishInitial(nodeIds, roots);
	ASSERT_EQ(published.properties.size(), rootCount);

	MetadataSnapshotPin current = metadata_->openSnapshot();
	const std::vector<CatalogEntry> pools =
			metadata_->findCurrentPropertyPagePools(current, propertyOptions().page_owner_id);
	ASSERT_EQ(pools.size(), 2U);
	std::map<SegmentId, size_t> pagesByPool;
	std::set<PageId> pageIds;
	for (const PropertyCollectionRoot &root: roots) {
		const EntityPropertyKey key{EntityPropertyOwnerKind::EPOK_NODE, root.owner_entity_id};
		const EntityPropertyLocation location = metadata_->findEntityPropertyLocation(current, key);
		const PageLocation page = metadata_->findPageLocation(current, location.page_id);
		++pagesByPool[page.segment_id];
		pageIds.insert(page.page_id);
	}
	ASSERT_EQ(pageIds.size(), rootCount);
	ASSERT_EQ(pagesByPool.size(), 2U);
	std::vector<size_t> pageCounts;
	for (const auto &[segmentId, pageCount]: pagesByPool) {
		static_cast<void>(segmentId);
		pageCounts.push_back(pageCount);
	}
	std::ranges::sort(pageCounts);
	EXPECT_EQ(pageCounts, (std::vector<size_t>{1U, RSS_PAGE_POOL_DATA_FRAME_COUNT}));
	EXPECT_EQ(properties_->readEntity(
			current, {EntityPropertyOwnerKind::EPOK_NODE, roots.front().owner_entity_id}), roots.front());
	EXPECT_EQ(properties_->readEntity(
			current, {EntityPropertyOwnerKind::EPOK_NODE, roots.back().owner_entity_id}), roots.back());
	current.release();
}

TEST_F(PropertyPagePoolPackingTest, ReservesTheLastExistingFrameForCowBeforePlacingANewPage) {
	constexpr size_t initialRootCount = RSS_PAGE_POOL_DATA_FRAME_COUNT - 1U;
	constexpr size_t nodeCount = initialRootCount + 1U;
	std::vector<uint64_t> nodeIds;
	std::vector<PropertyCollectionRoot> initialRoots;
	nodeIds.reserve(nodeCount);
	initialRoots.reserve(initialRootCount);
	for (size_t index = 0U; index < nodeCount; ++index) {
		const uint64_t nodeId = 1001U + index;
		nodeIds.push_back(nodeId);
		if (index < initialRootCount) {
			initialRoots.push_back(sizedRoot(nodeId, static_cast<uint32_t>(3U + 2U * index), 2000U, 'a'));
		}
	}
	static_cast<void>(publishInitial(nodeIds, initialRoots));

	MetadataSnapshotPin before = metadata_->openSnapshot();
	const std::vector<CatalogEntry> beforePools =
			metadata_->findCurrentPropertyPagePools(before, propertyOptions().page_owner_id);
	ASSERT_EQ(beforePools.size(), 1U);
	ASSERT_EQ(pages_->availableFrameCount(before, beforePools.front().segment_id), 1U);
	PropertyCollectionRoot replacement = initialRoots.front();
	replacement.entries.front().value = graph::PropertyValue(std::string(1950U, 'r'));
	const PropertyCollectionRoot appended = sizedRoot(nodeIds.back(), 999U, 2000U, 'n');
	const std::array mutations{replacement, appended};
	const std::vector<EntityPropertyRootMutationPlan> plans =
			properties_->planEntityRootReplacements(before, mutations, propertyOptions());
	ASSERT_EQ(plans.size(), 2U);
	EXPECT_EQ(plans.front().kind, EntityPropertyRootMutationPlanKind::EPRMPK_EXISTING_PAGE);
	EXPECT_EQ(plans.back().kind, EntityPropertyRootMutationPlanKind::EPRMPK_NEW_PAGE_REQUIRED);
	static_cast<void>(entities_->applyNativeGraphBatchTransaction(
			before, {}, {}, {}, plans, *properties_, propertyOptions(), LogSequenceNumber(3U)));
	before.release();

	MetadataSnapshotPin current = metadata_->openSnapshot();
	const std::vector<CatalogEntry> currentPools =
			metadata_->findCurrentPropertyPagePools(current, propertyOptions().page_owner_id);
	ASSERT_EQ(currentPools.size(), 2U);
	const EntityPropertyLocation replacedLocation = metadata_->findEntityPropertyLocation(
			current, {EntityPropertyOwnerKind::EPOK_NODE, replacement.owner_entity_id});
	const EntityPropertyLocation appendedLocation = metadata_->findEntityPropertyLocation(
			current, {EntityPropertyOwnerKind::EPOK_NODE, appended.owner_entity_id});
	EXPECT_EQ(metadata_->findPageLocation(current, replacedLocation.page_id).segment_id,
			  beforePools.front().segment_id);
	EXPECT_NE(metadata_->findPageLocation(current, appendedLocation.page_id).segment_id,
			  beforePools.front().segment_id);
	EXPECT_EQ(properties_->readEntity(current, replacedLocation.key), replacement);
	EXPECT_EQ(properties_->readEntity(current, appendedLocation.key), appended);
	current.release();
}
