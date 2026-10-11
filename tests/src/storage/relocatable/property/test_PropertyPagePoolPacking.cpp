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

	PropertyCollectionRootStoreOptions propertyOptions() { return {PhysicalSlotId(41U), PhysicalSlotId(48U), 902U}; }

	PropertyCollectionRoot sizedRoot(uint64_t nodeId, uint32_t token, size_t encodedBytes, char fill) {
		PropertyCollectionRoot result{nodeId, graph::Node::typeId, {{token, graph::PropertyValue(std::string{})}}};
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
		file_ = std::make_shared<std::fstream>(path_,
											   std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
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
	const EntityPropertyLocation firstLocation =
			metadata_->findEntityPropertyLocation(before, {EntityPropertyOwnerKind::EPOK_NODE, first.owner_entity_id});
	EXPECT_EQ(pages_->availableFrameCount(before, beforePools.front().segment_id), RSS_PAGE_POOL_DATA_FRAME_COUNT - 1U);

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
		roots.push_back(
				sizedRoot(nodeId, static_cast<uint32_t>(3U + 2U * index), 2000U, static_cast<char>('a' + index % 26U)));
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
	EXPECT_EQ(properties_->readEntity(current, {EntityPropertyOwnerKind::EPOK_NODE, roots.front().owner_entity_id}),
			  roots.front());
	EXPECT_EQ(properties_->readEntity(current, {EntityPropertyOwnerKind::EPOK_NODE, roots.back().owner_entity_id}),
			  roots.back());
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
	static_cast<void>(entities_->applyNativeGraphBatchTransaction(before, {}, {}, {}, plans, *properties_,
																  propertyOptions(), LogSequenceNumber(3U)));
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

TEST_F(PropertyPagePoolPackingTest, MovesPropertyCowToFreshPoolWhenPinnedSnapshotsExhaustSourceFrames) {
	const std::array<uint64_t, 1U> nodeIds{1001U};
	const PropertyCollectionRoot initial = sizedRoot(1001U, 3U, 2000U, 'a');
	static_cast<void>(publishInitial(nodeIds, std::span(&initial, 1U)));

	MetadataSnapshotPin oldSnapshot = metadata_->openSnapshot();
	const CatalogEntry sourcePool =
			metadata_->findCurrentPropertyPagePools(oldSnapshot, propertyOptions().page_owner_id).front();
	PropertyCollectionRoot replacement = initial;
	std::vector<MetadataSnapshotPin> retainedGenerations;
	retainedGenerations.reserve(RSS_PAGE_POOL_DATA_FRAME_COUNT);
	// Keep every historical routing generation alive. Each replacement source is
	// therefore still reachable and cannot be reused by the allocator.
	for (size_t iteration = 0U; iteration < RSS_PAGE_POOL_DATA_FRAME_COUNT; ++iteration) {
		replacement.entries.front().value =
				graph::PropertyValue(std::string(1950U, static_cast<char>('b' + iteration % 20U)));
		MetadataSnapshotPin current = metadata_->openSnapshot();
		const std::array roots{replacement};
		const std::vector<EntityPropertyRootMutationPlan> plans =
				properties_->planEntityRootReplacements(current, roots, propertyOptions());
		ASSERT_EQ(plans.size(), 1U);
		ASSERT_EQ(plans.front().kind, EntityPropertyRootMutationPlanKind::EPRMPK_EXISTING_PAGE);
		static_cast<void>(entities_->applyNativeGraphBatchTransaction(
				current, {}, {}, {}, plans, *properties_, propertyOptions(), LogSequenceNumber(3U + iteration)));
		retainedGenerations.push_back(std::move(current));
	}

	MetadataSnapshotPin current = metadata_->openSnapshot();
	const std::vector<CatalogEntry> pools =
			metadata_->findCurrentPropertyPagePools(current, propertyOptions().page_owner_id);
	ASSERT_EQ(pools.size(), 2U);
	const EntityPropertyKey key{EntityPropertyOwnerKind::EPOK_NODE, initial.owner_entity_id};
	const EntityPropertyLocation location = metadata_->findEntityPropertyLocation(current, key);
	EXPECT_NE(metadata_->findPageLocation(current, location.page_id).segment_id, sourcePool.segment_id);
	EXPECT_EQ(properties_->readEntity(oldSnapshot, key), initial);
	EXPECT_EQ(properties_->readEntity(current, key), replacement);
	current.release();

	oldSnapshot.release();
	for (MetadataSnapshotPin &snapshot: retainedGenerations) {
		snapshot.release();
	}
	pages_->reclaimRetiredFrames(sourcePool.segment_id);
	MetadataSnapshotPin reclaimed = metadata_->openSnapshot();
	EXPECT_EQ(pages_->availableFrameCount(reclaimed, sourcePool.segment_id), RSS_PAGE_POOL_DATA_FRAME_COUNT);
	reclaimed.release();

	properties_.reset();
	pages_.reset();
	entities_.reset();
	metadata_.reset();
	metadata_ = RelocatableMetadataStore::open(io_);
	entities_ = std::make_unique<EntityPageStore>(io_, *metadata_);
	pages_ = std::make_unique<SlottedPagePool>(io_, *metadata_);
	properties_ = std::make_unique<PropertyCollectionRootStore>(*pages_, *metadata_);
	MetadataSnapshotPin reopened = metadata_->openSnapshot();
	EXPECT_EQ(properties_->readEntity(reopened, key), replacement);
	EXPECT_EQ(metadata_->findCurrentPropertyPagePools(reopened, propertyOptions().page_owner_id).size(), 2U);
	reopened.release();
}

TEST_F(PropertyPagePoolPackingTest, PacksConcurrentPinnedPropertyCowFallbacksIntoOneFreshPool) {
	std::vector<uint64_t> nodeIds;
	std::vector<PropertyCollectionRoot> initial;
	nodeIds.reserve(RSS_PAGE_POOL_DATA_FRAME_COUNT);
	initial.reserve(RSS_PAGE_POOL_DATA_FRAME_COUNT);
	for (size_t index = 0U; index < RSS_PAGE_POOL_DATA_FRAME_COUNT; ++index) {
		const uint64_t nodeId = 1001U + index;
		nodeIds.push_back(nodeId);
		initial.push_back(sizedRoot(nodeId, static_cast<uint32_t>(3U + index), 2000U, 'a'));
	}
	static_cast<void>(publishInitial(nodeIds, initial));

	MetadataSnapshotPin oldSnapshot = metadata_->openSnapshot();
	const CatalogEntry sourcePool =
			metadata_->findCurrentPropertyPagePools(oldSnapshot, propertyOptions().page_owner_id).front();
	std::vector<PropertyCollectionRoot> replacement = initial;
	for (size_t index = 0U; index < replacement.size(); ++index) {
		replacement[index].entries.front().value =
				graph::PropertyValue(std::string(1950U, static_cast<char>('b' + index % 20U)));
	}
	const std::vector<EntityPropertyRootMutationPlan> plans =
			properties_->planEntityRootReplacements(oldSnapshot, replacement, propertyOptions());
	ASSERT_EQ(plans.size(), replacement.size());
	ASSERT_TRUE(std::ranges::all_of(plans, [](const EntityPropertyRootMutationPlan &plan) {
		return plan.kind == EntityPropertyRootMutationPlanKind::EPRMPK_EXISTING_PAGE;
	}));
	static_cast<void>(entities_->applyNativeGraphBatchTransaction(oldSnapshot, {}, {}, {}, plans, *properties_,
																  propertyOptions(), LogSequenceNumber(3U)));

	MetadataSnapshotPin current = metadata_->openSnapshot();
	const std::vector<CatalogEntry> pools =
			metadata_->findCurrentPropertyPagePools(current, propertyOptions().page_owner_id);
	ASSERT_EQ(pools.size(), 2U);
	const auto destination = std::ranges::find_if(
			pools, [source = sourcePool.segment_id](const CatalogEntry &pool) { return pool.segment_id != source; });
	ASSERT_NE(destination, pools.end());
	std::set<uint8_t> destinationFrames;
	for (const PropertyCollectionRoot &root: replacement) {
		const EntityPropertyKey key{EntityPropertyOwnerKind::EPOK_NODE, root.owner_entity_id};
		const EntityPropertyLocation rootLocation = metadata_->findEntityPropertyLocation(current, key);
		const PageLocation pageLocation = metadata_->findPageLocation(current, rootLocation.page_id);
		EXPECT_EQ(pageLocation.segment_id, destination->segment_id);
		EXPECT_TRUE(destinationFrames.insert(pageLocation.page_index).second);
		EXPECT_EQ(properties_->readEntity(current, key), root);
	}
	EXPECT_EQ(destinationFrames.size(), replacement.size());
	EXPECT_EQ(
			properties_->readEntity(oldSnapshot, {EntityPropertyOwnerKind::EPOK_NODE, initial.front().owner_entity_id}),
			initial.front());
	current.release();
	oldSnapshot.release();

	properties_.reset();
	pages_.reset();
	entities_.reset();
	metadata_.reset();
	metadata_ = RelocatableMetadataStore::open(io_);
	entities_ = std::make_unique<EntityPageStore>(io_, *metadata_);
	pages_ = std::make_unique<SlottedPagePool>(io_, *metadata_);
	properties_ = std::make_unique<PropertyCollectionRootStore>(*pages_, *metadata_);
	MetadataSnapshotPin reopened = metadata_->openSnapshot();
	EXPECT_EQ(metadata_->findCurrentPropertyPagePools(reopened, propertyOptions().page_owner_id).size(), 2U);
	for (const PropertyCollectionRoot &root: replacement) {
		const EntityPropertyKey key{EntityPropertyOwnerKind::EPOK_NODE, root.owner_entity_id};
		EXPECT_EQ(properties_->readEntity(reopened, key), root);
	}
	reopened.release();
}

TEST_F(PropertyPagePoolPackingTest, DoesNotPublishAnyConcurrentFreshPoolCowReplacementBeforeSuperblockPublication) {
	const std::array<uint64_t, 2U> nodeIds{1001U, 1002U};
	const std::array initial{
			sizedRoot(nodeIds[0], 3U, 2000U, 'a'),
			sizedRoot(nodeIds[1], 4U, 2000U, 'b'),
	};
	static_cast<void>(publishInitial(nodeIds, initial));

	MetadataSnapshotPin oldSnapshot = metadata_->openSnapshot();
	const CatalogEntry sourcePool =
			metadata_->findCurrentPropertyPagePools(oldSnapshot, propertyOptions().page_owner_id).front();
	std::vector<MetadataSnapshotPin> retainedGenerations;
	retainedGenerations.reserve(RSS_PAGE_POOL_DATA_FRAME_COUNT - initial.size());
	PropertyCollectionRoot currentRoot = initial.front();
	// The two initial roots occupy two source frames. Keep exactly every remaining
	// source frame pinned, so the following two-root batch must reserve one fresh
	// pool but has not done so before the injected publication boundary.
	for (size_t iteration = 0U; iteration + initial.size() < RSS_PAGE_POOL_DATA_FRAME_COUNT; ++iteration) {
		currentRoot.entries.front().value =
				graph::PropertyValue(std::string(1950U, static_cast<char>('c' + iteration % 20U)));
		MetadataSnapshotPin snapshot = metadata_->openSnapshot();
		const std::array roots{currentRoot};
		const std::vector<EntityPropertyRootMutationPlan> plans =
				properties_->planEntityRootReplacements(snapshot, roots, propertyOptions());
		static_cast<void>(entities_->applyNativeGraphBatchTransaction(
				snapshot, {}, {}, {}, plans, *properties_, propertyOptions(), LogSequenceNumber(3U + iteration)));
		retainedGenerations.push_back(std::move(snapshot));
	}

	PropertyCollectionRoot firstReplacement = currentRoot;
	firstReplacement.entries.front().value = graph::PropertyValue(std::string(1950U, 'x'));
	PropertyCollectionRoot secondReplacement = initial.back();
	secondReplacement.entries.front().value = graph::PropertyValue(std::string(1950U, 'y'));
	const std::array replacements{firstReplacement, secondReplacement};
	MetadataSnapshotPin beforePublication = metadata_->openSnapshot();
	const std::vector<EntityPropertyRootMutationPlan> plans =
			properties_->planEntityRootReplacements(beforePublication, replacements, propertyOptions());
	ASSERT_EQ(plans.size(), replacements.size());
	ASSERT_TRUE(std::ranges::all_of(plans, [](const EntityPropertyRootMutationPlan &plan) {
		return plan.kind == EntityPropertyRootMutationPlanKind::EPRMPK_EXISTING_PAGE;
	}));
	MetadataPublishOptions interrupted;
	interrupted.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE;
	EXPECT_THROW(static_cast<void>(entities_->applyNativeGraphBatchTransaction(
				 beforePublication, {}, {}, {}, plans, *properties_, propertyOptions(), LogSequenceNumber(100U), interrupted)),
			 BootstrapPublishInterrupted);
	beforePublication.release();

	MetadataSnapshotPin current = metadata_->openSnapshot();
	EXPECT_EQ(metadata_->findCurrentPropertyPagePools(current, propertyOptions().page_owner_id).size(), 1U);
	EXPECT_EQ(metadata_->findPageLocation(current, metadata_->findEntityPropertyLocation(
				 current, {EntityPropertyOwnerKind::EPOK_NODE, currentRoot.owner_entity_id}).page_id)
				  .segment_id,
			  sourcePool.segment_id);
	EXPECT_EQ(properties_->readEntity(current, {EntityPropertyOwnerKind::EPOK_NODE, currentRoot.owner_entity_id}),
			  currentRoot);
	EXPECT_EQ(properties_->readEntity(current, {EntityPropertyOwnerKind::EPOK_NODE, initial.back().owner_entity_id}),
			  initial.back());
	current.release();

	oldSnapshot.release();
	for (MetadataSnapshotPin &snapshot: retainedGenerations) {
		snapshot.release();
	}
	properties_.reset();
	pages_.reset();
	entities_.reset();
	metadata_.reset();
	metadata_ = RelocatableMetadataStore::open(io_);
	entities_ = std::make_unique<EntityPageStore>(io_, *metadata_);
	pages_ = std::make_unique<SlottedPagePool>(io_, *metadata_);
	properties_ = std::make_unique<PropertyCollectionRootStore>(*pages_, *metadata_);
	MetadataSnapshotPin reopened = metadata_->openSnapshot();
	EXPECT_EQ(metadata_->findCurrentPropertyPagePools(reopened, propertyOptions().page_owner_id).size(), 1U);
	EXPECT_EQ(properties_->readEntity(reopened, {EntityPropertyOwnerKind::EPOK_NODE, currentRoot.owner_entity_id}),
			  currentRoot);
	EXPECT_EQ(properties_->readEntity(reopened, {EntityPropertyOwnerKind::EPOK_NODE, initial.back().owner_entity_id}),
			  initial.back());
	reopened.release();
}

TEST_F(PropertyPagePoolPackingTest, RollsBackFreshPropertyCowPoolWhenPublicationIsInterrupted) {
	const std::array<uint64_t, 1U> nodeIds{1001U};
	const PropertyCollectionRoot initial = sizedRoot(1001U, 3U, 2000U, 'a');
	static_cast<void>(publishInitial(nodeIds, std::span(&initial, 1U)));

	MetadataSnapshotPin initialSnapshot = metadata_->openSnapshot();
	std::vector<MetadataSnapshotPin> retainedGenerations;
	retainedGenerations.reserve(RSS_PAGE_POOL_DATA_FRAME_COUNT);
	PropertyCollectionRoot beforeFallback = initial;
	for (size_t iteration = 0U; iteration + 1U < RSS_PAGE_POOL_DATA_FRAME_COUNT; ++iteration) {
		beforeFallback.entries.front().value =
				graph::PropertyValue(std::string(1950U, static_cast<char>('b' + iteration % 20U)));
		MetadataSnapshotPin current = metadata_->openSnapshot();
		const std::array roots{beforeFallback};
		const std::vector<EntityPropertyRootMutationPlan> plans =
				properties_->planEntityRootReplacements(current, roots, propertyOptions());
		static_cast<void>(entities_->applyNativeGraphBatchTransaction(
				current, {}, {}, {}, plans, *properties_, propertyOptions(), LogSequenceNumber(3U + iteration)));
		retainedGenerations.push_back(std::move(current));
	}

	PropertyCollectionRoot failedReplacement = beforeFallback;
	failedReplacement.entries.front().value = graph::PropertyValue(std::string(1950U, 'z'));
	const EntityPropertyKey key{EntityPropertyOwnerKind::EPOK_NODE, initial.owner_entity_id};
	MetadataSnapshotPin before = metadata_->openSnapshot();
	const std::array failedRoots{failedReplacement};
	const std::vector<EntityPropertyRootMutationPlan> failedPlans =
			properties_->planEntityRootReplacements(before, failedRoots, propertyOptions());
	MetadataPublishOptions interrupted;
	interrupted.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE;
	EXPECT_THROW(static_cast<void>(entities_->applyNativeGraphBatchTransaction(
				 before, {}, {}, {}, failedPlans, *properties_, propertyOptions(), LogSequenceNumber(100U), interrupted)),
			 BootstrapPublishInterrupted);
	before.release();

	MetadataSnapshotPin recovered = metadata_->openSnapshot();
	EXPECT_EQ(metadata_->findCurrentPropertyPagePools(recovered, propertyOptions().page_owner_id).size(), 1U);
	EXPECT_EQ(properties_->readEntity(recovered, key), beforeFallback);
	recovered.release();

	MetadataSnapshotPin tornSnapshot = metadata_->openSnapshot();
	const std::array tornRoots{failedReplacement};
	const std::vector<EntityPropertyRootMutationPlan> tornPlans =
			properties_->planEntityRootReplacements(tornSnapshot, tornRoots, propertyOptions());
	MetadataPublishOptions torn;
	torn.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE;
	EXPECT_THROW(static_cast<void>(entities_->applyNativeGraphBatchTransaction(
				 tornSnapshot, {}, {}, {}, tornPlans, *properties_, propertyOptions(), LogSequenceNumber(101U), torn)),
			 BootstrapPublishInterrupted);
	tornSnapshot.release();

	EXPECT_EQ(properties_->readEntity(initialSnapshot, key), initial);
	initialSnapshot.release();
	for (MetadataSnapshotPin &snapshot: retainedGenerations) {
		snapshot.release();
	}
	properties_.reset();
	pages_.reset();
	entities_.reset();
	metadata_.reset();
	metadata_ = RelocatableMetadataStore::open(io_);
	entities_ = std::make_unique<EntityPageStore>(io_, *metadata_);
	pages_ = std::make_unique<SlottedPagePool>(io_, *metadata_);
	properties_ = std::make_unique<PropertyCollectionRootStore>(*pages_, *metadata_);
	MetadataSnapshotPin afterTornRecovery = metadata_->openSnapshot();
	EXPECT_EQ(metadata_->findCurrentPropertyPagePools(afterTornRecovery, propertyOptions().page_owner_id).size(), 1U);
	EXPECT_EQ(properties_->readEntity(afterTornRecovery, key), beforeFallback);
	afterTornRecovery.release();

	MetadataSnapshotPin retrySnapshot = metadata_->openSnapshot();
	const std::array retryRoots{failedReplacement};
	const std::vector<EntityPropertyRootMutationPlan> retryPlans =
			properties_->planEntityRootReplacements(retrySnapshot, retryRoots, propertyOptions());
	static_cast<void>(entities_->applyNativeGraphBatchTransaction(
			retrySnapshot, {}, {}, {}, retryPlans, *properties_, propertyOptions(), LogSequenceNumber(101U)));
	retrySnapshot.release();
	MetadataSnapshotPin current = metadata_->openSnapshot();
	EXPECT_EQ(properties_->readEntity(current, key), failedReplacement);
	// Reopen releases the historical pins. Retrying may therefore reuse the
	// reconciled source-pool frame instead of allocating another fresh pool.
	EXPECT_EQ(metadata_->findCurrentPropertyPagePools(current, propertyOptions().page_owner_id).size(), 1U);
	current.release();
}

TEST_F(PropertyPagePoolPackingTest, RecoversFreshPropertyCowPoolAfterInactiveSuperblockPublication) {
	const std::array<uint64_t, 1U> nodeIds{1001U};
	const PropertyCollectionRoot initial = sizedRoot(1001U, 3U, 2000U, 'a');
	static_cast<void>(publishInitial(nodeIds, std::span(&initial, 1U)));

	MetadataSnapshotPin initialSnapshot = metadata_->openSnapshot();
	const CatalogEntry sourcePool =
			metadata_->findCurrentPropertyPagePools(initialSnapshot, propertyOptions().page_owner_id).front();
	std::vector<MetadataSnapshotPin> retainedGenerations;
	retainedGenerations.reserve(RSS_PAGE_POOL_DATA_FRAME_COUNT);
	PropertyCollectionRoot beforeFallback = initial;
	for (size_t iteration = 0U; iteration + 1U < RSS_PAGE_POOL_DATA_FRAME_COUNT; ++iteration) {
		beforeFallback.entries.front().value =
				graph::PropertyValue(std::string(1950U, static_cast<char>('b' + iteration % 20U)));
		MetadataSnapshotPin snapshot = metadata_->openSnapshot();
		const std::array roots{beforeFallback};
		const std::vector<EntityPropertyRootMutationPlan> plans =
				properties_->planEntityRootReplacements(snapshot, roots, propertyOptions());
		static_cast<void>(entities_->applyNativeGraphBatchTransaction(
				snapshot, {}, {}, {}, plans, *properties_, propertyOptions(), LogSequenceNumber(3U + iteration)));
		retainedGenerations.push_back(std::move(snapshot));
	}

	PropertyCollectionRoot replacement = beforeFallback;
	replacement.entries.front().value = graph::PropertyValue(std::string(1950U, 'z'));
	MetadataSnapshotPin before = metadata_->openSnapshot();
	const std::array roots{replacement};
	const std::vector<EntityPropertyRootMutationPlan> plans =
			properties_->planEntityRootReplacements(before, roots, propertyOptions());
	MetadataPublishOptions published;
	published.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE;
	EXPECT_THROW(static_cast<void>(entities_->applyNativeGraphBatchTransaction(
				 before, {}, {}, {}, plans, *properties_, propertyOptions(), LogSequenceNumber(100U), published)),
			 BootstrapPublishInterrupted);
	before.release();
	initialSnapshot.release();
	for (MetadataSnapshotPin &snapshot: retainedGenerations) {
		snapshot.release();
	}

	properties_.reset();
	pages_.reset();
	entities_.reset();
	metadata_.reset();
	metadata_ = RelocatableMetadataStore::open(io_);
	entities_ = std::make_unique<EntityPageStore>(io_, *metadata_);
	pages_ = std::make_unique<SlottedPagePool>(io_, *metadata_);
	properties_ = std::make_unique<PropertyCollectionRootStore>(*pages_, *metadata_);

	const EntityPropertyKey key{EntityPropertyOwnerKind::EPOK_NODE, initial.owner_entity_id};
	MetadataSnapshotPin recovered = metadata_->openSnapshot();
	EXPECT_EQ(properties_->readEntity(recovered, key), replacement);
	EXPECT_EQ(metadata_->findCurrentPropertyPagePools(recovered, propertyOptions().page_owner_id).size(), 2U);
	EXPECT_NE(metadata_->findPageLocation(recovered, metadata_->findEntityPropertyLocation(recovered, key).page_id)
				  .segment_id,
			  sourcePool.segment_id);
	recovered.release();
}
