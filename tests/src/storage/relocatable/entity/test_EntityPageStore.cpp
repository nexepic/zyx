/**
 * @file test_EntityPageStore.cpp
 * @brief Durable EntityPage pool, Entity Map, COW, and retirement tests.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "storage/relocatable/entity/EntityPageStoreTestFixture.hpp"

#include <limits>

TEST_F(RelocatableEntityPageStoreTest, AtomicallyAllocatesReadsAndReopensNodeAndEdgePages) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore store(io, *metadata);
	MetadataSnapshotPin before = metadata->openSnapshot();
	const NodeEntityRecord firstNode = node(11U, 12U);
	const std::optional<EntityPageInsertResult> nodeAllocation =
			store.allocateNode(newPool(31U, 901U, 101U, 40U), 1001U, firstNode, LogSequenceNumber(2U));
	ASSERT_TRUE(nodeAllocation.has_value());
	const EdgeEntityRecord firstEdge = edge(1001U, 1002U, 21U);
	const std::optional<EntityPageInsertResult> edgeAllocation =
			store.allocateEdge(newPool(32U, 902U, 201U, 41U), 2001U, firstEdge, LogSequenceNumber(3U));
	ASSERT_TRUE(edgeAllocation.has_value());

	expectFormatError([&] { static_cast<void>(store.readNode(before, 1001U)); }, FormatError::FERR_INVALID_STATE);
	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(store.readNode(current, 1001U), firstNode);
	EXPECT_EQ(store.readEdge(current, 2001U), firstEdge);
	EXPECT_EQ(metadata->findNodePageLocation(current, 1001U), nodeAllocation->location);
	EXPECT_EQ(metadata->findEdgePageLocation(current, 2001U), edgeAllocation->location);
	EXPECT_EQ(metadata->findPageLocation(current, PageId(101U)), nodeAllocation->page);
	EXPECT_EQ(metadata->findPageLocation(current, PageId(201U)), edgeAllocation->page);
	const std::vector<CatalogEntry> pools = store.currentPools(current);
	ASSERT_EQ(pools.size(), 2U);
	EXPECT_EQ(pools.front().segment_id, SegmentId(31U));
	EXPECT_EQ(pools.back().segment_id, SegmentId(32U));
	before.release();
	current.release();

	metadata.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	EntityPageStore reopenedStore(io, *reopened);
	const MetadataSnapshotPin reopenedSnapshot = reopened->openSnapshot();
	EXPECT_EQ(reopenedStore.readNode(reopenedSnapshot, 1001U), firstNode);
	EXPECT_EQ(reopenedStore.readEdge(reopenedSnapshot, 2001U), firstEdge);
}

TEST_F(RelocatableEntityPageStoreTest, AppendsAndInsertsThroughCowWithoutChangingStableMappings) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore store(io, *metadata);
	const NodeEntityRecord firstNode = node(11U, 12U);
	const std::optional<EntityPageInsertResult> first =
			store.allocateNode(newPool(31U, 901U, 101U, 40U), 1001U, firstNode, LogSequenceNumber(2U));
	ASSERT_TRUE(first.has_value());
	MetadataSnapshotPin original = metadata->openSnapshot();
	const NodeEntityRecord appendedNode = node(21U, 22U, 17U);
	const EntityPageAppendRequest appendRequest{first->pool, PageId(102U)};
	const EntityPageInsertResult appended =
			store.appendNodePage(appendRequest, 1002U, appendedNode, LogSequenceNumber(3U));
	EXPECT_EQ(appended.pool.segment_id, first->pool.segment_id);
	EXPECT_NE(appended.page.page_index, first->page.page_index);

	const NodeEntityRecord insertedNode = node(31U, 32U, 27U);
	const EntityPageInsertResult inserted =
			store.insertNode(original, PageId(101U), 1003U, insertedNode, LogSequenceNumber(4U));
	EXPECT_NE(inserted.page.page_index, first->page.page_index);
	EXPECT_EQ(inserted.location.page_id, PageId(101U));
	EXPECT_EQ(inserted.location.slot_id, 1U);
	EXPECT_EQ(inserted.location.slot_generation, 1U);

	EXPECT_EQ(store.readNode(original, 1001U), firstNode);
	expectFormatError([&] { static_cast<void>(store.readNode(original, 1002U)); }, FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(store.readNode(original, 1003U)); }, FormatError::FERR_INVALID_STATE);
	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(store.readNode(current, 1001U), firstNode);
	EXPECT_EQ(store.readNode(current, 1002U), appendedNode);
	EXPECT_EQ(store.readNode(current, 1003U), insertedNode);
	EXPECT_EQ(metadata->findNodePageLocation(current, 1001U), first->location);
	EXPECT_EQ(metadata->findNodePageLocation(current, 1003U), inserted.location);
	EXPECT_EQ(store.freeSlotCount(current, PageId(101U), EntityPageRecordKind::EPRK_NODE),
			  EntityPage::create({PageId(1U), 1U, PageOwnerKind::POK_ENTITY, 0U, RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
								  Generation(1U), LogSequenceNumber(1U)},
								 EntityPageRecordKind::EPRK_NODE)
							  .header()
							  .slot_capacity -
					  2U);
	EXPECT_EQ(store.currentPools(current).size(), 1U);
	original.release();
	store.reclaimRetiredFrames(SegmentId(31U));
	EXPECT_EQ(store.readNode(current, 1001U), firstNode);
}

TEST_F(RelocatableEntityPageStoreTest, MovesCowImageToFreshPoolWhenEveryCurrentEntityPoolIsFull) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore store(io, *metadata);
	const NodeEntityRecord originalNode = node(11U, 12U);
	const std::optional<EntityPageInsertResult> first =
			store.allocateNode(newPool(31U, 901U, 101U, 40U), 1001U, originalNode, LogSequenceNumber(2U));
	ASSERT_TRUE(first.has_value());

	// Fill every frame in the only current Entity pool while retaining the
	// first page as the COW source. The next mutation must therefore seed a
	// new pool rather than relying on an unpublished frame elsewhere.
	for (uint64_t ordinal = 1U; ordinal < RSS_PAGE_POOL_DATA_FRAME_COUNT; ++ordinal) {
		const EntityPageAppendRequest append{first->pool, PageId(101U + ordinal)};
		static_cast<void>(store.appendNodePage(append, 1001U + ordinal, node(21U + ordinal, 22U + ordinal),
																		 LogSequenceNumber(2U + ordinal)));
	}
	MetadataSnapshotPin before = metadata->openSnapshot();
	ASSERT_EQ(store.currentPools(before).size(), 1U);
	EXPECT_EQ(store.availableFrameCount(before, first->pool), 0U);

	EntityPageBatchMutation mutation;
	mutation.page_id = PageId(101U);
	mutation.record_kind = EntityPageRecordKind::EPRK_NODE;
	mutation.node_updates.push_back({1001U, node(71U, 72U, 37U)});
	mutation.replacement_new_pool = EntityPageFreshPoolDestination{
				SegmentId(32U), 901U, Generation(1U), PhysicalSlotId(41U), PhysicalSlotId(41U)};
	const EntityPageBatchResult published =
			store.applyBatch(before, std::span(&mutation, 1U), LogSequenceNumber(2U + RSS_PAGE_POOL_DATA_FRAME_COUNT));
	ASSERT_EQ(published.pages.size(), 1U);
	EXPECT_EQ(published.pages.front().pool.segment_id, SegmentId(32U));
	EXPECT_EQ(published.pages.front().pool.physical_slot, PhysicalSlotId(41U));
	EXPECT_EQ(published.pages.front().page.page_id, PageId(101U));

	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(metadata->findPageLocation(current, PageId(101U)).segment_id, SegmentId(32U));
	EXPECT_EQ(metadata->findNodePageLocation(current, 1001U), first->location);
	EXPECT_EQ(store.readNode(before, 1001U), originalNode);
	EXPECT_EQ(store.readNode(current, 1001U), mutation.node_updates.front().record);
	EXPECT_EQ(store.currentPools(current).size(), 2U);
	current.release();
	before.release();

	metadata.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	EntityPageStore reopenedStore(io, *reopened);
	const MetadataSnapshotPin recovered = reopened->openSnapshot();
	EXPECT_EQ(reopenedStore.readNode(recovered, 1001U), mutation.node_updates.front().record);
}

TEST_F(RelocatableEntityPageStoreTest, NativeBatchPacksMatchingFreshCowAndNewPagePoolPlansAtomically) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	const NodeEntityRecord original = node(11U, 12U);
	ASSERT_TRUE(entities.allocateNode(newPool(31U, 901U, 101U, 40U), 1001U, original, LogSequenceNumber(2U))
					.has_value());
	SlottedPagePool pages(io, *metadata);
	PropertyCollectionRootStore properties(pages, *metadata);
	const PropertyCollectionRootStoreOptions propertyOptions{PhysicalSlotId(42U), PhysicalSlotId(42U), 902U};
	const std::array<EntityPropertyRootMutationPlan, 0U> noPropertyPlans{};
	MetadataSnapshotPin before = metadata->openSnapshot();

	const EntityPageFreshPoolDestination destination{
			SegmentId(32U), 901U, Generation(1U), PhysicalSlotId(41U), PhysicalSlotId(41U)};
	const std::array mutations = {
			EntityPageBatchMutation{.page_id = PageId(101U),
								.record_kind = EntityPageRecordKind::EPRK_NODE,
								.node_updates = {{1001U, node(71U, 72U, 37U)}},
								.replacement_new_pool = destination},
	};
	const EntityPageBatchNewPool newPage{
			.request = newPool(32U, 901U, 102U, 41U),
			.record_kind = EntityPageRecordKind::EPRK_NODE,
			.node_insertions = {{1002U, node(81U, 82U, 47U)}},
	};

	EntityPageBatchNewPool conflictingPage = newPage;
	conflictingPage.request.pool_owner_id = 902U;
	expectFormatError(
			[&] {
				static_cast<void>(entities.applyNativeGraphBatchTransaction(
						before, mutations, {}, std::span(&conflictingPage, 1), noPropertyPlans, properties,
						propertyOptions, LogSequenceNumber(3U)));
			},
			FormatError::FERR_INVALID_STATE);
	const MetadataSnapshotPin unchanged = metadata->openSnapshot();
	EXPECT_EQ(unchanged.superblock(), before.superblock());
	EXPECT_EQ(entities.readNode(unchanged, 1001U), original);
	EXPECT_FALSE(metadata->findPageLocationOptional(unchanged, PageId(102U)).has_value());

	const EntityPageBatchResult published = entities.applyNativeGraphBatchTransaction(
			before, mutations, {}, std::span(&newPage, 1), noPropertyPlans, properties, propertyOptions,
			LogSequenceNumber(3U));
	ASSERT_EQ(published.pages.size(), 1U);
	ASSERT_EQ(published.new_pool_pages.size(), 1U);
	EXPECT_EQ(published.pages.front().pool.segment_id, destination.segment_id);
	EXPECT_EQ(published.new_pool_pages.front().pool.segment_id, destination.segment_id);
	EXPECT_EQ(published.pages.front().pool.physical_slot, destination.first_slot);
	EXPECT_EQ(published.new_pool_pages.front().pool.physical_slot, destination.first_slot);
	EXPECT_NE(published.pages.front().page.page_index, published.new_pool_pages.front().page.page_index);

	const MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(metadata->findPageLocation(current, PageId(101U)).segment_id, destination.segment_id);
	EXPECT_EQ(metadata->findPageLocation(current, PageId(102U)).segment_id, destination.segment_id);
	EXPECT_EQ(entities.readNode(before, 1001U), original);
	EXPECT_EQ(entities.readNode(current, 1001U), mutations.front().node_updates.front().record);
	EXPECT_EQ(entities.readNode(current, 1002U), newPage.node_insertions.front().record);
}

TEST_F(RelocatableEntityPageStoreTest,
	   NativeBatchExposesPrivateCandidatesOnlyInsideItsOnePublicationScope) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	SlottedPagePool pages(io, *metadata);
	PropertyCollectionRootStore properties(pages, *metadata);
	const PropertyCollectionRootStoreOptions propertyOptions{PhysicalSlotId(42U), PhysicalSlotId(42U), 902U};
	MetadataSnapshotPin before = metadata->openSnapshot();
	const std::array<EntityPageBatchMutation, 0U> noMutations{};
	const std::array<EntityPageBatchAppend, 0U> noAppends{};
	const std::array<EntityPropertyRootMutationPlan, 0U> noPropertyPlans{};
	const EntityPageAuxiliaryMutationBatch noAuxiliaryPages;
	const std::array<EntityPageBatchNewPool, 1U> newNode{{
			{.request = newPool(31U, 901U, 101U, 40U),
			 .record_kind = EntityPageRecordKind::EPRK_NODE,
			 .node_insertions = {{1001U, node(11U, 12U)}}},
	}};
	NativeGraphDerivedStateMutation derived;
	derived.private_segment_plan = {SegmentKind::SGK_BULK_SCRATCH, 991U, PhysicalSlotId(1U),
			PhysicalSlotId(std::numeric_limits<uint64_t>::max()), RSS_ENVELOPE_BYTES, {}, 1U};
	bool privateScopeCalled = false;
	NativeGraphPrivateSegmentMaterialization privateIdentity;

	const EntityPageBatchResult published = entities.applyNativeGraphBatchTransaction(
			before, noMutations, noAppends, newNode, noPropertyPlans, properties, propertyOptions, noAuxiliaryPages,
			derived,
			[](std::vector<std::vector<IndexTreeMutationPoolBatch>> &batches) { EXPECT_TRUE(batches.empty()); },
			LogSequenceNumber(2U), {}, {}, {}, {},
			[](const NativeGraphPrivateSegmentSet &identities, size_t ordinal) {
				EXPECT_EQ(identities.size(), 1U);
				EXPECT_EQ(ordinal, 0U);
				return std::vector<std::byte>{std::byte{0x5A}};
			},
			[&privateScopeCalled, &privateIdentity](const NativeGraphPrivateSegmentSet &identities,
															 const NativeGraphPrivateSegmentReader &reader) {
				ASSERT_EQ(identities.size(), 1U);
				privateIdentity = identities.front();
				EXPECT_EQ(reader(privateIdentity), std::vector<std::byte>({std::byte{0x5A}}));
				privateScopeCalled = true;
			});

	EXPECT_TRUE(privateScopeCalled);
	EXPECT_EQ(published.superblock.generation, Generation(2U));
	EXPECT_THROW(static_cast<void>(entities.readNode(before, 1001U)), FormatException);
	MetadataSnapshotPin after = metadata->openSnapshot();
	EXPECT_EQ(entities.readNode(after, 1001U), newNode.front().node_insertions.front().record);
	EXPECT_THROW(static_cast<void>(metadata->findCatalog(after, privateIdentity.segment_id)), FormatException);
	after.release();
	before.release();
}

TEST_F(RelocatableEntityPageStoreTest,
	   PrivateCandidateConsumerFailureLeavesTheEntireEntityBatchUnpublished) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	SlottedPagePool pages(io, *metadata);
	PropertyCollectionRootStore properties(pages, *metadata);
	const PropertyCollectionRootStoreOptions propertyOptions{PhysicalSlotId(42U), PhysicalSlotId(42U), 902U};
	MetadataSnapshotPin before = metadata->openSnapshot();
	const std::array<EntityPageBatchMutation, 0U> noMutations{};
	const std::array<EntityPageBatchAppend, 0U> noAppends{};
	const std::array<EntityPropertyRootMutationPlan, 0U> noPropertyPlans{};
	const EntityPageAuxiliaryMutationBatch noAuxiliaryPages;
	const std::array<EntityPageBatchNewPool, 1U> newNode{{
			{.request = newPool(31U, 901U, 101U, 40U),
			 .record_kind = EntityPageRecordKind::EPRK_NODE,
			 .node_insertions = {{1001U, node(11U, 12U)}}},
	}};
	NativeGraphDerivedStateMutation derived;
	derived.private_segment_plan = {SegmentKind::SGK_BULK_SCRATCH, 992U, PhysicalSlotId(1U),
			PhysicalSlotId(std::numeric_limits<uint64_t>::max()), RSS_ENVELOPE_BYTES, {}, 1U};
	NativeGraphPrivateSegmentMaterialization privateIdentity;

	expectFormatError(
			[&] {
				static_cast<void>(entities.applyNativeGraphBatchTransaction(
						before, noMutations, noAppends, newNode, noPropertyPlans, properties, propertyOptions,
						noAuxiliaryPages, derived,
						[](std::vector<std::vector<IndexTreeMutationPoolBatch>> &batches) {
							EXPECT_TRUE(batches.empty());
						},
						LogSequenceNumber(2U), {}, {}, {}, {},
						[](const NativeGraphPrivateSegmentSet &, size_t) {
							return std::vector<std::byte>{std::byte{0x6A}};
						},
						[&privateIdentity](const NativeGraphPrivateSegmentSet &identities,
											 const NativeGraphPrivateSegmentReader &) {
							privateIdentity = identities.front();
							throw FormatException(FormatError::FERR_INVALID_STATE,
												  "private candidate consumer rejected its source");
						}));
			},
			FormatError::FERR_INVALID_STATE);

	MetadataSnapshotPin after = metadata->openSnapshot();
	EXPECT_EQ(after.superblock(), before.superblock());
	EXPECT_THROW(static_cast<void>(entities.readNode(after, 1001U)), FormatException);
	ASSERT_TRUE(privateIdentity.isValid());
	EXPECT_THROW(static_cast<void>(metadata->findCatalog(after, privateIdentity.segment_id)), FormatException);
	after.release();
	before.release();
}

TEST_F(RelocatableEntityPageStoreTest,
	   NativeGraphBatchRejectsAStaleSnapshotBeforeItCanOverwriteANewerEntityGeneration) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	SlottedPagePool pages(io, *metadata);
	PropertyCollectionRootStore properties(pages, *metadata);
	const PropertyCollectionRootStoreOptions propertyOptions{PhysicalSlotId(41U), PhysicalSlotId(41U), 902U};
	const std::array<EntityPropertyRootMutationPlan, 0U> noPropertyPlans{};

	const NodeEntityRecord original = node(11U, 12U);
	ASSERT_TRUE(entities.allocateNode(newPool(31U, 901U, 101U, 40U), 1001U, original, LogSequenceNumber(2U))
				.has_value());
	MetadataSnapshotPin stale = metadata->openSnapshot();
	const NodeEntityRecord winner = node(21U, 22U);
	static_cast<void>(entities.updateNode(stale, 1001U, winner, LogSequenceNumber(3U)));

	const std::array<EntityPageBatchMutation, 1U> staleMutation{{
			{.page_id = PageId(101U),
			 .record_kind = EntityPageRecordKind::EPRK_NODE,
			 .node_updates = {{1001U, node(31U, 32U)}}},
	}};
	expectFormatError(
			[&] {
				static_cast<void>(entities.applyNativeGraphBatchTransaction(
						stale, staleMutation, {}, {}, noPropertyPlans, properties, propertyOptions,
						LogSequenceNumber(4U)));
			},
			FormatError::FERR_INVALID_STATE);

	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(current.superblock().generation, Generation(3U));
	EXPECT_EQ(entities.readNode(stale, 1001U), original);
	EXPECT_EQ(entities.readNode(current, 1001U), winner);
	stale.release();
	current.release();

	metadata.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	EntityPageStore reopenedEntities(io, *reopened);
	MetadataSnapshotPin recovered = reopened->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation, Generation(3U));
	EXPECT_EQ(reopenedEntities.readNode(recovered, 1001U), winner);
	recovered.release();
}

TEST_F(RelocatableEntityPageStoreTest, UpdatesAndRetiresFinalEntityPageWithoutBreakingPinnedSnapshots) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore store(io, *metadata);
	const NodeEntityRecord firstNode = node(11U, 12U);
	const std::optional<EntityPageInsertResult> first =
			store.allocateNode(newPool(31U, 901U, 101U, 40U), 1001U, firstNode, LogSequenceNumber(2U));
	ASSERT_TRUE(first.has_value());
	MetadataSnapshotPin original = metadata->openSnapshot();
	const NodeEntityRecord replacementNode = node(81U, 82U, 37U);
	const EntityPageMutationResult updated = store.updateNode(original, 1001U, replacementNode, LogSequenceNumber(3U));
	EXPECT_NE(updated.page.page_index, first->page.page_index);
	MetadataSnapshotPin updatedSnapshot = metadata->openSnapshot();
	EXPECT_EQ(store.readNode(original, 1001U), firstNode);
	EXPECT_EQ(store.readNode(updatedSnapshot, 1001U), replacementNode);
	EXPECT_EQ(metadata->findNodePageLocation(updatedSnapshot, 1001U), first->location);
	expectFormatError([&] { static_cast<void>(store.updateNode(original, 1001U, firstNode, LogSequenceNumber(4U))); },
					  FormatError::FERR_INVALID_STATE);

	const EntityPageMutationResult erased = store.eraseNode(updatedSnapshot, 1001U, LogSequenceNumber(4U));
	EXPECT_EQ(erased.page.page_id, PageId(101U));
	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_FALSE(metadata->findNodePageLocationOptional(current, 1001U).has_value());
	EXPECT_FALSE(metadata->findPageLocationOptional(current, PageId(101U)).has_value());
	expectFormatError([&] { static_cast<void>(store.readNode(current, 1001U)); }, FormatError::FERR_INVALID_STATE);
	EXPECT_EQ(store.readNode(original, 1001U), firstNode);
	EXPECT_EQ(store.readNode(updatedSnapshot, 1001U), replacementNode);

	original.release();
	updatedSnapshot.release();
	store.reclaimRetiredFrames(SegmentId(31U));
	current.release();
	static_cast<void>(store.retireEmptyPool(SegmentId(31U), Generation(1U), LogSequenceNumber(5U)));
	MetadataSnapshotPin retired = metadata->openSnapshot();
	EXPECT_EQ(metadata->findCatalog(retired, SegmentId(31U)).state, SegmentState::SGS_RETIRED);
	retired.release();
	static_cast<void>(store.releaseRetiredEmptyPool(SegmentId(31U), Generation(1U), LogSequenceNumber(6U)));
	const MetadataSnapshotPin released = metadata->openSnapshot();
	EXPECT_FALSE(metadata->isPhysicalSlotAllocated(released, PhysicalSlotId(40U)));
}

TEST_F(RelocatableEntityPageStoreTest, ErasingAnEntityAtomicallyUnpublishesItsOwnerKeyedPropertyRoot) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	ASSERT_TRUE(entities.allocateNode(newPool(31U, 901U, 101U, 40U), 1001U, node(11U, 12U), LogSequenceNumber(2U))
						.has_value());
	SlottedPagePool pages(io, *metadata);
	PropertyCollectionRootStore properties(pages, *metadata);
	const PropertyCollectionRoot root{1001U, graph::Node::typeId, {{3U, graph::PropertyValue("before deletion")}}};
	const PropertyCollectionRootStoreOptions options{PhysicalSlotId(41U), PhysicalSlotId(41U), 902U};
	const EntityPropertyKey key{EntityPropertyOwnerKind::EPOK_NODE, 1001U};
	static_cast<void>(test::publishPropertyRoot(entities, properties, *metadata, root, options));

	MetadataSnapshotPin before = metadata->openSnapshot();
	ASSERT_TRUE(metadata->findEntityPropertyLocationOptional(before, key).has_value());
	static_cast<void>(entities.eraseNode(before, 1001U, LogSequenceNumber(4U)));
	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_FALSE(metadata->findNodePageLocationOptional(current, 1001U).has_value());
	EXPECT_FALSE(metadata->findEntityPropertyLocationOptional(current, key).has_value());
	EXPECT_EQ(properties.readEntity(before, key), root);
	expectFormatError([&] { static_cast<void>(properties.readEntity(current, key)); }, FormatError::FERR_INVALID_STATE);
	before.release();
	current.release();
}

TEST_F(RelocatableEntityPageStoreTest, FailedEntityErasureKeepsItsOwnerKeyedPropertyRootReachable) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	const NodeEntityRecord original = node(11U, 12U);
	ASSERT_TRUE(
			entities.allocateNode(newPool(31U, 901U, 101U, 40U), 1001U, original, LogSequenceNumber(2U)).has_value());
	SlottedPagePool pages(io, *metadata);
	PropertyCollectionRootStore properties(pages, *metadata);
	const PropertyCollectionRoot root{1001U, graph::Node::typeId, {{3U, graph::PropertyValue("not deleted")}}};
	const PropertyCollectionRootStoreOptions options{PhysicalSlotId(41U), PhysicalSlotId(41U), 902U};
	const EntityPropertyKey key{EntityPropertyOwnerKind::EPOK_NODE, 1001U};
	static_cast<void>(test::publishPropertyRoot(entities, properties, *metadata, root, options));

	MetadataSnapshotPin before = metadata->openSnapshot();
	MetadataPublishOptions interrupted;
	interrupted.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE;
	EXPECT_THROW(static_cast<void>(entities.eraseNode(before, 1001U, LogSequenceNumber(4U), interrupted)),
				 BootstrapPublishInterrupted);
	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(current.superblock(), before.superblock());
	EXPECT_EQ(entities.readNode(current, 1001U), original);
	EXPECT_EQ(properties.readEntity(current, key), root);
	before.release();
	current.release();
}

TEST_F(RelocatableEntityPageStoreTest, NativeBatchPublishesLabelSetAppendAndReplacementWithItsNode) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	const NodeEntityRecord initial = node(11U, 12U);
	const std::optional<EntityPageInsertResult> allocated =
			entities.allocateNode(newPool(31U, 901U, 101U, 40U), 1001U, initial, LogSequenceNumber(2U));
	ASSERT_TRUE(allocated.has_value());
	const NodeEntityRecord initialUnrelated = node(21U, 22U);
	const std::optional<EntityPageInsertResult> allocatedUnrelated =
			entities.allocateNode(newPool(32U, 902U, 201U, 41U), 1002U, initialUnrelated, LogSequenceNumber(3U));
	ASSERT_TRUE(allocatedUnrelated.has_value());
	SlottedPagePool pages(io, *metadata);
	EntityLabelSetStore labelStore(io, *metadata, pages);
	PropertyCollectionRootStore properties(pages, *metadata);
	const PropertyCollectionRootStoreOptions propertyOptions{PhysicalSlotId(41U), PhysicalSlotId(41U), 902U};
	const std::array<EntityPropertyRootMutationPlan, 0U> noPropertyPlans{};

	MetadataSnapshotPin before = metadata->openSnapshot();
	const CatalogEntry entityPool = metadata->findCatalog(before, SegmentId(31U));
	const std::vector<uint64_t> firstLabels = orderedLabels(7U, RSS_ENTITY_INLINE_LABEL_CAPACITY + 2U);
	const LabelPagePayload firstPayload =
			labelPagePayload(entityPool, PageId(102U), LogSequenceNumber(4U),
							 std::span(firstLabels).subspan(RSS_ENTITY_INLINE_LABEL_CAPACITY));
	const NodeEntityRecord firstNode = nodeWithOverflow(firstLabels, firstPayload.overflow);
	const NodeEntityRecord updatedUnrelated = node(23U, 24U);
	const std::array<EntityPageBatchMutation, 2U> firstMutation{{
			{.page_id = PageId(101U),
			 .record_kind = EntityPageRecordKind::EPRK_NODE,
			 .node_updates = {{1001U, firstNode}}},
			{.page_id = PageId(201U),
			 .record_kind = EntityPageRecordKind::EPRK_NODE,
			 .node_updates = {{1002U, updatedUnrelated}}},
	}};
	EntityPageAuxiliaryMutationBatch firstAuxiliary;
	firstAuxiliary.appends.push_back(
			{entityPool, PageId(102U), PageOwnerKind::POK_ENTITY_LABEL, firstPayload.payload, firstPayload.free_bytes});
	firstAuxiliary.node_update_bindings.push_back({1001U, allocated->location, firstNode});

	MetadataPublishOptions interrupted;
	interrupted.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE;
	EXPECT_THROW(static_cast<void>(entities.applyNativeGraphBatchTransaction(
							 before, firstMutation, {}, {}, noPropertyPlans, properties, propertyOptions, firstAuxiliary,
							 LogSequenceNumber(4U), interrupted)),
				 BootstrapPublishInterrupted);
	MetadataSnapshotPin notPublished = metadata->openSnapshot();
	EXPECT_EQ(notPublished.superblock(), before.superblock());
	EXPECT_EQ(entities.readNode(notPublished, 1001U), initial);
	EXPECT_FALSE(metadata->findPageLocationOptional(notPublished, PageId(102U)).has_value());
	notPublished.release();
	before.release();

	MetadataSnapshotPin firstWrite = metadata->openSnapshot();
	const EntityPageBatchResult firstPublished =
			entities.applyNativeGraphBatchTransaction(firstWrite, firstMutation, {}, {}, noPropertyPlans, properties,
											  propertyOptions, firstAuxiliary, LogSequenceNumber(4U));
	EXPECT_EQ(firstPublished.pages.size(), 2U);
	EXPECT_TRUE(firstPublished.appended_pages.empty());
	EXPECT_TRUE(firstPublished.new_pool_pages.empty());
	firstWrite.release();

	MetadataSnapshotPin afterFirst = metadata->openSnapshot();
	EXPECT_EQ(entities.readNode(afterFirst, 1001U), firstNode);
	EXPECT_EQ(entities.readNode(afterFirst, 1002U), updatedUnrelated);
	EXPECT_EQ(labelStore.readNodeLabels(afterFirst, entities.readNode(afterFirst, 1001U)), firstLabels);
	const PageLocation firstLabelLocation = metadata->findPageLocation(afterFirst, PageId(102U));
	const std::vector<uint64_t> grownLabels = orderedLabels(7U, RSS_ENTITY_INLINE_LABEL_CAPACITY + 3U);
	const LabelPagePayload grownPayload =
			labelPagePayload(entityPool, PageId(102U), LogSequenceNumber(5U),
							 std::span(grownLabels).subspan(RSS_ENTITY_INLINE_LABEL_CAPACITY));
	const NodeEntityRecord grownNode = nodeWithOverflow(grownLabels, grownPayload.overflow);
	const std::array<EntityPageBatchMutation, 1U> grownMutation{{
			{.page_id = PageId(101U),
			 .record_kind = EntityPageRecordKind::EPRK_NODE,
			 .node_updates = {{1001U, grownNode}}},
	}};
	EntityPageAuxiliaryMutationBatch grownAuxiliary;
	grownAuxiliary.replacements.push_back({entityPool, firstLabelLocation, PageOwnerKind::POK_ENTITY_LABEL,
										   LogSequenceNumber(4U), grownPayload.payload, grownPayload.free_bytes,
										   false, {}, {}});
	grownAuxiliary.node_update_bindings.push_back(
			{1001U, metadata->findNodePageLocation(afterFirst, 1001U), grownNode});
	static_cast<void>(entities.applyNativeGraphBatchTransaction(afterFirst, grownMutation, {}, {}, noPropertyPlans,
															properties, propertyOptions, grownAuxiliary,
															LogSequenceNumber(5U)));
	EXPECT_EQ(labelStore.readNodeLabels(afterFirst, entities.readNode(afterFirst, 1001U)), firstLabels);

	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(entities.readNode(current, 1001U), grownNode);
	EXPECT_EQ(labelStore.readNodeLabels(current, entities.readNode(current, 1001U)), grownLabels);
	EXPECT_NE(metadata->findPageLocation(current, PageId(102U)).page_index, firstLabelLocation.page_index);
	afterFirst.release();
	current.release();
}

TEST_F(RelocatableEntityPageStoreTest, NativeBatchPublishesLabelSetNewPoolWithoutLeakingAuxiliaryResults) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	SlottedPagePool pages(io, *metadata);
	EntityLabelSetStore labelStore(io, *metadata, pages);
	PropertyCollectionRootStore properties(pages, *metadata);
	const PropertyCollectionRootStoreOptions propertyOptions{PhysicalSlotId(42U), PhysicalSlotId(42U), 903U};
	const std::array<EntityPropertyRootMutationPlan, 0U> noPropertyPlans{};
	const std::vector<uint64_t> allLabels = orderedLabels(7U, RSS_ENTITY_INLINE_LABEL_CAPACITY + 1U);
	CatalogEntry plannedLabelPool;
	plannedLabelPool.segment_id = SegmentId(32U);
	plannedLabelPool.kind = SegmentKind::SGK_ENTITY_PAGE_POOL;
	plannedLabelPool.owner_id = 902U;
	plannedLabelPool.generation = Generation(1U);
	plannedLabelPool.state = SegmentState::SGS_SEALED;
	plannedLabelPool.committed_lsn = LogSequenceNumber(2U);
	const LabelPagePayload labelPayload =
			labelPagePayload(plannedLabelPool, PageId(201U), LogSequenceNumber(2U),
							 std::span(allLabels).subspan(RSS_ENTITY_INLINE_LABEL_CAPACITY));
	const NodeEntityRecord initial = node(11U, 12U);
	const std::optional<EntityPageInsertResult> allocated =
			entities.allocateNode(newPool(31U, 901U, 101U, 40U), 1001U, initial, LogSequenceNumber(2U));
	ASSERT_TRUE(allocated.has_value());
	const NodeEntityRecord nodeRecord = nodeWithOverflow(allLabels, labelPayload.overflow);
	const std::array<EntityPageBatchMutation, 1U> nodeMutation{{
			{.page_id = PageId(101U),
			 .record_kind = EntityPageRecordKind::EPRK_NODE,
			 .node_updates = {{1001U, nodeRecord}}},
	}};
	EntityPageAuxiliaryMutationBatch auxiliary;
	auxiliary.new_pools.push_back({SegmentId(32U), 902U, Generation(1U), PageId(201U), PhysicalSlotId(41U),
								   PhysicalSlotId(41U), PageOwnerKind::POK_ENTITY_LABEL, labelPayload.payload,
								   labelPayload.free_bytes});
	MetadataSnapshotPin empty = metadata->openSnapshot();
	auxiliary.node_update_bindings.push_back({1001U, allocated->location, nodeRecord});
	const EntityPageBatchResult published =
			entities.applyNativeGraphBatchTransaction(empty, nodeMutation, {}, {}, noPropertyPlans, properties,
													  propertyOptions, auxiliary, LogSequenceNumber(3U));
	ASSERT_EQ(published.pages.size(), 1U);
	EXPECT_TRUE(published.appended_pages.empty());
	EXPECT_TRUE(published.new_pool_pages.empty());
	empty.release();

	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(metadata->findCatalog(current, SegmentId(32U)).kind, SegmentKind::SGK_ENTITY_PAGE_POOL);
	EXPECT_EQ(labelStore.readNodeLabels(current, entities.readNode(current, 1001U)), allLabels);
	EXPECT_TRUE(metadata->findPageLocationOptional(current, PageId(201U)).has_value());
	current.release();

	metadata.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	EntityPageStore reopenedEntities(io, *reopened);
	SlottedPagePool reopenedPages(io, *reopened);
	EntityLabelSetStore reopenedLabels(io, *reopened, reopenedPages);
	MetadataSnapshotPin reopenedSnapshot = reopened->openSnapshot();
	EXPECT_EQ(reopenedLabels.readNodeLabels(reopenedSnapshot, reopenedEntities.readNode(reopenedSnapshot, 1001U)),
			  allLabels);
}

TEST_F(RelocatableEntityPageStoreTest, NativeBatchRejectsMalformedOrForeignAuxiliaryLabelFramesBeforePublication) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	SlottedPagePool pages(io, *metadata);
	PropertyCollectionRootStore properties(pages, *metadata);
	const PropertyCollectionRootStoreOptions propertyOptions{PhysicalSlotId(42U), PhysicalSlotId(42U), 903U};
	const std::array<EntityPropertyRootMutationPlan, 0U> noPropertyPlans{};
	const std::vector<uint64_t> allLabels = orderedLabels(7U, RSS_ENTITY_INLINE_LABEL_CAPACITY + 1U);
	CatalogEntry plannedLabelPool;
	plannedLabelPool.segment_id = SegmentId(32U);
	plannedLabelPool.kind = SegmentKind::SGK_ENTITY_PAGE_POOL;
	plannedLabelPool.owner_id = 902U;
	plannedLabelPool.generation = Generation(1U);
	plannedLabelPool.state = SegmentState::SGS_SEALED;
	plannedLabelPool.committed_lsn = LogSequenceNumber(2U);
	LabelPagePayload labelPayload = labelPagePayload(plannedLabelPool, PageId(201U), LogSequenceNumber(2U),
													 std::span(allLabels).subspan(RSS_ENTITY_INLINE_LABEL_CAPACITY));
	const NodeEntityRecord initial = node(11U, 12U);
	const std::optional<EntityPageInsertResult> allocated =
			entities.allocateNode(newPool(31U, 901U, 101U, 40U), 1001U, initial, LogSequenceNumber(2U));
	ASSERT_TRUE(allocated.has_value());
	const NodeEntityRecord nodeRecord = nodeWithOverflow(allLabels, labelPayload.overflow);
	const std::array<EntityPageBatchMutation, 1U> nodeMutation{{
			{.page_id = PageId(101U),
			 .record_kind = EntityPageRecordKind::EPRK_NODE,
			 .node_updates = {{1001U, nodeRecord}}},
	}};
	MetadataSnapshotPin empty = metadata->openSnapshot();
	EntityPageAuxiliaryMutationBatch foreignKind;
	foreignKind.new_pools.push_back({SegmentId(32U), 902U, Generation(1U), PageId(201U), PhysicalSlotId(41U),
									 PhysicalSlotId(41U), PageOwnerKind::POK_PROPERTY, labelPayload.payload,
									 labelPayload.free_bytes});
	foreignKind.node_update_bindings.push_back({1001U, allocated->location, nodeRecord});
	expectFormatError(
			[&] {
				static_cast<void>(entities.applyNativeGraphBatchTransaction(
						empty, nodeMutation, {}, {}, noPropertyPlans, properties, propertyOptions, foreignKind,
						LogSequenceNumber(2U)));
			},
			FormatError::FERR_INVALID_HEADER);
	EXPECT_EQ(entities.readNode(empty, 1001U), initial);
	EXPECT_FALSE(metadata->findPageLocationOptional(empty, PageId(201U)).has_value());

	labelPayload.payload.back() ^= std::byte{1U};
	EntityPageAuxiliaryMutationBatch corruptPayload;
	corruptPayload.new_pools.push_back({SegmentId(32U), 902U, Generation(1U), PageId(201U), PhysicalSlotId(41U),
										PhysicalSlotId(41U), PageOwnerKind::POK_ENTITY_LABEL, labelPayload.payload,
										labelPayload.free_bytes});
	corruptPayload.node_update_bindings.push_back({1001U, allocated->location, nodeRecord});
	expectFormatError(
			[&] {
				static_cast<void>(entities.applyNativeGraphBatchTransaction(
						empty, nodeMutation, {}, {}, noPropertyPlans, properties, propertyOptions, corruptPayload,
						LogSequenceNumber(2U)));
			},
			FormatError::FERR_CHECKSUM_MISMATCH);
	EXPECT_EQ(entities.readNode(empty, 1001U), initial);
	EXPECT_FALSE(metadata->findPageLocationOptional(empty, PageId(201U)).has_value());
	empty.release();
}

TEST_F(RelocatableEntityPageStoreTest, NativeBatchPublishesEntityLabelSetAndPropertyRootAsOneGeneration) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	const NodeEntityRecord initial = node(11U, 12U);
	ASSERT_TRUE(
			entities.allocateNode(newPool(31U, 901U, 101U, 40U), 1001U, initial, LogSequenceNumber(2U)).has_value());
	SlottedPagePool pages(io, *metadata);
	EntityLabelSetStore labelStore(io, *metadata, pages);
	PropertyCollectionRootStore properties(pages, *metadata);
	const PropertyCollectionRootStoreOptions propertyOptions{PhysicalSlotId(41U), PhysicalSlotId(42U), 902U};
	const PropertyCollectionRoot root{
			1001U, graph::Node::typeId, {{3U, graph::PropertyValue("atomic native LabelSet Property root")}}};
	const std::vector<uint64_t> completeLabels = orderedLabels(7U, RSS_ENTITY_INLINE_LABEL_CAPACITY + 2U);

	const auto planBatch = [&](const MetadataSnapshotPin &snapshot) {
		const EntityLabelSetMutationPlan labels = labelStore.planNodeLabels(
				snapshot, 1001U, completeLabels,
				EntityLabelSetWritePlan{.append_page = EntityLabelSetAppendRequest{SegmentId(31U), PageId(102U)}},
				LogSequenceNumber(3U));
		const EntityPropertyRootMutationPlan property =
				properties.planEntityRootReplacement(snapshot, root, propertyOptions);
		const std::array<EntityPageBatchMutation, 1U> nodeMutation{{
				{.page_id = labels.node_location.page_id,
				 .record_kind = EntityPageRecordKind::EPRK_NODE,
				 .node_updates = {{1001U, labels.replacement_node}}},
		}};
		return std::tuple{labels, property, nodeMutation};
	};

	MetadataSnapshotPin before = metadata->openSnapshot();
	auto [firstLabelPlan, firstPropertyPlan, firstNodeMutation] = planBatch(before);
	const std::array firstPropertyPlans{firstPropertyPlan};
	MetadataPublishOptions interrupted;
	interrupted.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE;
	EXPECT_THROW(static_cast<void>(entities.applyNativeGraphBatchTransaction(
						 before, firstNodeMutation, {}, {}, firstPropertyPlans, properties, propertyOptions,
						 firstLabelPlan.auxiliary_pages, LogSequenceNumber(3U), interrupted)),
				 BootstrapPublishInterrupted);
	MetadataSnapshotPin notPublished = metadata->openSnapshot();
	EXPECT_EQ(notPublished.superblock(), before.superblock());
	EXPECT_EQ(entities.readNode(notPublished, 1001U), initial);
	EXPECT_FALSE(metadata->findPageLocationOptional(notPublished, PageId(102U)).has_value());
	EXPECT_FALSE(metadata->findEntityPropertyLocationOptional(
								 notPublished, EntityPropertyKey{EntityPropertyOwnerKind::EPOK_NODE, 1001U})
						 .has_value());
	notPublished.release();
	before.release();

	MetadataSnapshotPin retry = metadata->openSnapshot();
	auto [labelPlan, propertyPlan, nodeMutation] = planBatch(retry);
	const std::array propertyPlans{propertyPlan};
	const EntityPageBatchResult published = entities.applyNativeGraphBatchTransaction(
			retry, nodeMutation, {}, {}, propertyPlans, properties, propertyOptions, labelPlan.auxiliary_pages,
			LogSequenceNumber(3U));
	ASSERT_EQ(published.pages.size(), 1U);
	ASSERT_EQ(published.properties.size(), 1U);
	retry.release();

	MetadataSnapshotPin current = metadata->openSnapshot();
	const NodeEntityRecord currentNode = entities.readNode(current, 1001U);
	EXPECT_EQ(currentNode, labelPlan.replacement_node);
	EXPECT_EQ(labelStore.readNodeLabels(current, currentNode), completeLabels);
	EXPECT_EQ(properties.readEntity(current, EntityPropertyKey{EntityPropertyOwnerKind::EPOK_NODE, 1001U}), root);
	EXPECT_TRUE(metadata->findPageLocationOptional(current, PageId(102U)).has_value());
	current.release();
}

TEST_F(RelocatableEntityPageStoreTest, NativeBatchPublishesEntityAndOwnerKeyedPropertiesInOneGeneration) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	SlottedPagePool pages(io, *metadata);
	PropertyCollectionRootStore properties(pages, *metadata);
	const PropertyCollectionRootStoreOptions propertyOptions{PhysicalSlotId(41U), PhysicalSlotId(41U), 902U};
	const EntityPropertyKey propertyKey{EntityPropertyOwnerKind::EPOK_NODE, 1001U};
	const PropertyCollectionRoot initialRoot{
			1001U, graph::Node::typeId, {{3U, graph::PropertyValue("initial native root")}}};
	MetadataSnapshotPin empty = metadata->openSnapshot();
	const EntityPropertyRootMutationPlan initialPlan =
			properties.planEntityRootReplacement(empty, initialRoot, propertyOptions);
	ASSERT_EQ(initialPlan.kind, EntityPropertyRootMutationPlanKind::EPRMPK_NEW_PAGE_REQUIRED);
	const std::array<EntityPageBatchNewPool, 1U> newNodePool{{
			{.request = newPool(31U, 901U, 101U, 40U),
			 .record_kind = EntityPageRecordKind::EPRK_NODE,
			 .node_insertions = {{1001U, node(11U, 12U)}}},
	}};
	const std::array propertyPlans{initialPlan};
	MetadataPublishOptions interrupted;
	interrupted.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE;
	EXPECT_THROW(static_cast<void>(entities.applyNativeGraphBatchTransaction(empty, {}, {}, newNodePool, propertyPlans,
																			 properties, propertyOptions,
																			 LogSequenceNumber(2U), interrupted)),
				 BootstrapPublishInterrupted);
	MetadataSnapshotPin beforeRetry = metadata->openSnapshot();
	EXPECT_EQ(beforeRetry.superblock(), empty.superblock());
	EXPECT_FALSE(metadata->findNodePageLocationOptional(beforeRetry, 1001U).has_value());
	EXPECT_FALSE(metadata->findEntityPropertyLocationOptional(beforeRetry, propertyKey).has_value());
	empty.release();

	const EntityPropertyRootMutationPlan retryPlan =
			properties.planEntityRootReplacement(beforeRetry, initialRoot, propertyOptions);
	const std::array retryPropertyPlans{retryPlan};
	const EntityPageBatchResult created = entities.applyNativeGraphBatchTransaction(
			beforeRetry, {}, {}, newNodePool, retryPropertyPlans, properties, propertyOptions, LogSequenceNumber(3U));
	ASSERT_EQ(created.new_pool_pages.size(), 1U);
	ASSERT_EQ(created.properties.size(), 1U);
	EXPECT_EQ(created.properties.front().key, propertyKey);
	MetadataSnapshotPin original = metadata->openSnapshot();
	EXPECT_EQ(entities.readNode(original, 1001U), newNodePool.front().node_insertions.front().record);
	EXPECT_EQ(properties.readEntity(original, propertyKey), initialRoot);
	EXPECT_EQ(metadata->findEntityPropertyLocation(original, propertyKey), created.properties.front());

	NodeEntityRecord updatedNode = newNodePool.front().node_insertions.front().record;
	updatedNode.inline_label_ids = {9U, 11U, 13U};
	PropertyCollectionRoot replacementRoot = initialRoot;
	replacementRoot.entries.front().value = graph::PropertyValue("replaced native root");
	const EntityPropertyRootMutationPlan replacementPlan =
			properties.planEntityRootReplacement(original, replacementRoot, propertyOptions);
	ASSERT_EQ(replacementPlan.kind, EntityPropertyRootMutationPlanKind::EPRMPK_EXISTING_PAGE);
	const std::array<EntityPageBatchMutation, 1U> nodeMutation{{
			{.page_id = created.new_pool_pages.front().page.page_id,
			 .record_kind = EntityPageRecordKind::EPRK_NODE,
			 .node_updates = {{1001U, updatedNode}}},
	}};
	const std::array replacementPlans{replacementPlan};
	const EntityPageBatchResult replaced = entities.applyNativeGraphBatchTransaction(
			original, nodeMutation, {}, {}, replacementPlans, properties, propertyOptions, LogSequenceNumber(4U));
	ASSERT_EQ(replaced.pages.size(), 1U);
	ASSERT_EQ(replaced.properties.size(), 1U);
	EXPECT_EQ(replaced.properties.front().key, propertyKey);
	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(entities.readNode(original, 1001U), newNodePool.front().node_insertions.front().record);
	EXPECT_EQ(properties.readEntity(original, propertyKey), initialRoot);
	EXPECT_EQ(entities.readNode(current, 1001U), updatedNode);
	EXPECT_EQ(properties.readEntity(current, propertyKey), replacementRoot);
	EXPECT_NE(metadata->findPageLocation(original, nodeMutation.front().page_id).page_index,
			  metadata->findPageLocation(current, nodeMutation.front().page_id).page_index);
	EXPECT_NE(metadata->findPageLocation(original, replaced.properties.front().page_id).page_index,
			  metadata->findPageLocation(current, replaced.properties.front().page_id).page_index);
}

TEST_F(RelocatableEntityPageStoreTest, NativeBatchPublishesLargePropertyAndItsReferenceAtomically) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	SlottedPagePool pages(io, *metadata);
	LargeValueStore largeValues(io, *metadata);
	PropertyCollectionRootStore properties(pages, largeValues, *metadata);
	const PropertyCollectionRootStoreOptions propertyOptions{PhysicalSlotId(41U), PhysicalSlotId(48U), 902U};
	const size_t maximumInlineBytes =
			RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES - RSS_SLOTTED_PAGE_HEADER_BYTES - RSS_SLOTTED_PAGE_SLOT_BYTES;
	const PropertyCollectionRoot root{
			1001U, graph::Node::typeId, {{3U, graph::PropertyValue(std::string(maximumInlineBytes + 128U, 'L'))}}};
	const std::array<EntityPageBatchNewPool, 1U> newNodePool{{
			{.request = newPool(31U, 901U, 101U, 40U),
			 .record_kind = EntityPageRecordKind::EPRK_NODE,
			 .node_insertions = {{1001U, node(11U, 12U)}}},
	}};

	MetadataSnapshotPin before = metadata->openSnapshot();
	const std::vector<PropertyCollectionRoot> roots{root};
	const std::vector<EntityPropertyRootMutationPlan> plans =
			properties.planEntityRootReplacements(before, roots, propertyOptions);
	ASSERT_EQ(plans.size(), 1U);
	ASSERT_EQ(plans.front().large_values.size(), 1U);
	ASSERT_EQ(plans.front().large_values.front().reference.value_id, LargeValueId(1U));
	const std::array propertyPlans{plans.front()};
	MetadataPublishOptions interrupted;
	interrupted.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE;
	EXPECT_THROW(static_cast<void>(entities.applyNativeGraphBatchTransaction(before, {}, {}, newNodePool, propertyPlans,
																			 properties, propertyOptions,
																			 LogSequenceNumber(2U), interrupted)),
				 BootstrapPublishInterrupted);
	EXPECT_FALSE(metadata->findLargeValueManifestOptional(before, LargeValueId(1U)).has_value());
	EXPECT_FALSE(metadata->findNodePageLocationOptional(before, 1001U).has_value());
	EXPECT_FALSE(metadata->findEntityPropertyLocationOptional(
								 before, EntityPropertyKey{EntityPropertyOwnerKind::EPOK_NODE, 1001U})
						 .has_value());
	before.release();

	MetadataSnapshotPin retry = metadata->openSnapshot();
	const std::vector<EntityPropertyRootMutationPlan> retryPlans =
			properties.planEntityRootReplacements(retry, roots, propertyOptions);
	ASSERT_EQ(retryPlans.front().large_values.size(), 1U);
	const std::array retryPropertyPlans{retryPlans.front()};
	const EntityPageBatchResult published = entities.applyNativeGraphBatchTransaction(
			retry, {}, {}, newNodePool, retryPropertyPlans, properties, propertyOptions, LogSequenceNumber(2U));
	ASSERT_EQ(published.new_pool_pages.size(), 1U);
	ASSERT_EQ(published.properties.size(), 1U);
	EXPECT_EQ(published.properties.front().key, (EntityPropertyKey{EntityPropertyOwnerKind::EPOK_NODE, 1001U}));

	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(properties.readEntity(current, published.properties.front().key), root);
	EXPECT_TRUE(metadata->findLargeValueManifestOptional(current, LargeValueId(1U)).has_value());
	EXPECT_EQ(current.superblock().next_large_value_id, 2U);
}

TEST_F(RelocatableEntityPageStoreTest, NativeBatchPacksNewOwnerKeyedRootsIntoOnePropertyPage) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	SlottedPagePool pages(io, *metadata);
	PropertyCollectionRootStore properties(pages, *metadata);
	const PropertyCollectionRootStoreOptions propertyOptions{PhysicalSlotId(41U), PhysicalSlotId(41U), 902U};
	const PropertyCollectionRoot firstRoot{
			1001U, graph::Node::typeId, {{3U, graph::PropertyValue("first packed root")}}};
	const PropertyCollectionRoot secondRoot{
			1002U, graph::Node::typeId, {{5U, graph::PropertyValue("second packed root")}}};
	MetadataSnapshotPin empty = metadata->openSnapshot();
	const EntityPropertyRootMutationPlan firstPlan =
			properties.planEntityRootReplacement(empty, firstRoot, propertyOptions);
	const EntityPropertyRootMutationPlan secondPlan =
			properties.planEntityRootReplacement(empty, secondRoot, propertyOptions);
	ASSERT_EQ(firstPlan.kind, EntityPropertyRootMutationPlanKind::EPRMPK_NEW_PAGE_REQUIRED);
	ASSERT_EQ(secondPlan.kind, EntityPropertyRootMutationPlanKind::EPRMPK_NEW_PAGE_REQUIRED);
	const std::array<EntityPageBatchNewPool, 1U> newNodePool{{
			{.request = newPool(31U, 901U, 101U, 40U),
			 .record_kind = EntityPageRecordKind::EPRK_NODE,
			 .node_insertions = {{1001U, node(11U, 12U)}, {1002U, node(21U, 22U)}}},
	}};
	// Intentionally reverse the input order: physical packing must use canonical
	// owner-key order, not the caller's staging order.
	const std::array propertyPlans{secondPlan, firstPlan};
	const EntityPageBatchResult created = entities.applyNativeGraphBatchTransaction(
			empty, {}, {}, newNodePool, propertyPlans, properties, propertyOptions, LogSequenceNumber(2U));
	ASSERT_EQ(created.properties.size(), 2U);
	EXPECT_EQ(created.properties[0].key, (EntityPropertyKey{EntityPropertyOwnerKind::EPOK_NODE, 1001U}));
	EXPECT_EQ(created.properties[1].key, (EntityPropertyKey{EntityPropertyOwnerKind::EPOK_NODE, 1002U}));
	EXPECT_EQ(created.properties[0].page_id, created.properties[1].page_id);
	EXPECT_NE(created.properties[0].slot_id, created.properties[1].slot_id);
	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(properties.readEntity(current, created.properties[0].key), firstRoot);
	EXPECT_EQ(properties.readEntity(current, created.properties[1].key), secondRoot);
	EXPECT_EQ(metadata->findPageLocation(current, created.properties[0].page_id),
			  metadata->findPageLocation(current, created.properties[1].page_id));
}

TEST_F(RelocatableEntityPageStoreTest, NativeBatchCoalescesFreshRootsIntoOneExistingPropertyPageCow) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	SlottedPagePool pages(io, *metadata);
	PropertyCollectionRootStore properties(pages, *metadata);
	const PropertyCollectionRootStoreOptions propertyOptions{PhysicalSlotId(41U), PhysicalSlotId(41U), 902U};
	const PropertyCollectionRoot initialRoot{
			1001U, graph::Node::typeId, {{3U, graph::PropertyValue("existing Property root")}}};
	const std::array<EntityPageBatchNewPool, 1U> initialNodePool{{
			{.request = newPool(31U, 901U, 101U, 40U),
			 .record_kind = EntityPageRecordKind::EPRK_NODE,
			 .node_insertions = {{1001U, node(11U, 12U)}}},
	}};

	MetadataSnapshotPin empty = metadata->openSnapshot();
	const EntityPropertyRootMutationPlan initialPlan =
			properties.planEntityRootReplacement(empty, initialRoot, propertyOptions);
	const std::array initialPlans{initialPlan};
	const EntityPageBatchResult initial = entities.applyNativeGraphBatchTransaction(
			empty, {}, {}, initialNodePool, initialPlans, properties, propertyOptions, LogSequenceNumber(2U));
	ASSERT_EQ(initial.properties.size(), 1U);
	empty.release();

	const PropertyCollectionRoot secondRoot{
			1002U, graph::Node::typeId, {{5U, graph::PropertyValue("second Property root")}}};
	const PropertyCollectionRoot thirdRoot{
			1003U, graph::Node::typeId, {{7U, graph::PropertyValue("third Property root")}}};
	MetadataSnapshotPin before = metadata->openSnapshot();
	const std::array<PropertyCollectionRoot, 2U> newRoots{secondRoot, thirdRoot};
	const std::vector<EntityPropertyRootMutationPlan> plans =
			properties.planEntityRootReplacements(before, newRoots, propertyOptions);
	ASSERT_EQ(plans.size(), 1U);
	ASSERT_EQ(plans.front().kind, EntityPropertyRootMutationPlanKind::EPRMPK_EXISTING_PAGE);
	ASSERT_TRUE(plans.front().page_replacement.has_value());
	ASSERT_EQ(plans.front().page_replacement->expected_location.page_id, initial.properties.front().page_id);
	ASSERT_EQ(plans.front().page_replacement->upserted_properties.size(), 2U);
	const std::array<EntityPageBatchMutation, 1U> nodeMutation{{
			{.page_id = initialNodePool.front().request.page_id,
			 .record_kind = EntityPageRecordKind::EPRK_NODE,
			 .node_insertions = {{1002U, node(21U, 22U)}, {1003U, node(31U, 32U)}}},
	}};
	const EntityPageBatchResult published = entities.applyNativeGraphBatchTransaction(
			before, nodeMutation, {}, {}, plans, properties, propertyOptions, LogSequenceNumber(3U));
	ASSERT_EQ(published.properties.size(), 2U);
	EXPECT_EQ(published.properties[0].page_id, initial.properties.front().page_id);
	EXPECT_EQ(published.properties[1].page_id, initial.properties.front().page_id);

	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(properties.readEntity(current, published.properties[0].key), secondRoot);
	EXPECT_EQ(properties.readEntity(current, published.properties[1].key), thirdRoot);
	EXPECT_EQ(properties.readEntity(before, EntityPropertyKey{EntityPropertyOwnerKind::EPOK_NODE, 1001U}), initialRoot);
	EXPECT_FALSE(metadata->findEntityPropertyLocationOptional(
						 before, EntityPropertyKey{EntityPropertyOwnerKind::EPOK_NODE, 1002U})
					 .has_value());
	EXPECT_FALSE(metadata->findEntityPropertyLocationOptional(
						 before, EntityPropertyKey{EntityPropertyOwnerKind::EPOK_NODE, 1003U})
					 .has_value());
}

TEST_F(RelocatableEntityPageStoreTest, NativeBatchCoalescesMixedRootUpdatesAndDeletesInOnePropertyPageCow) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	SlottedPagePool pages(io, *metadata);
	PropertyCollectionRootStore properties(pages, *metadata);
	const PropertyCollectionRootStoreOptions propertyOptions{PhysicalSlotId(41U), PhysicalSlotId(41U), 902U};
	const PropertyCollectionRoot firstRoot{
			1001U, graph::Node::typeId, {{3U, graph::PropertyValue("first original root")}}};
	const PropertyCollectionRoot secondRoot{
			1002U, graph::Node::typeId, {{5U, graph::PropertyValue("second original root")}}};
	const PropertyCollectionRoot thirdRoot{
			1003U, graph::Node::typeId, {{7U, graph::PropertyValue("third original root")}}};
	const std::array<EntityPageBatchNewPool, 1U> nodePool{{
			{.request = newPool(31U, 901U, 101U, 40U),
			 .record_kind = EntityPageRecordKind::EPRK_NODE,
			 .node_insertions = {{1001U, node(11U, 12U)}, {1002U, node(21U, 22U)}, {1003U, node(31U, 32U)}}},
	}};
	MetadataSnapshotPin empty = metadata->openSnapshot();
	const std::array initialPlans{
			properties.planEntityRootReplacement(empty, firstRoot, propertyOptions),
			properties.planEntityRootReplacement(empty, secondRoot, propertyOptions),
			properties.planEntityRootReplacement(empty, thirdRoot, propertyOptions),
	};
	static_cast<void>(entities.applyNativeGraphBatchTransaction(empty, {}, {}, nodePool, initialPlans, properties,
															 propertyOptions, LogSequenceNumber(2U)));
	empty.release();

	PropertyCollectionRoot firstReplacement = firstRoot;
	firstReplacement.entries.front().value = graph::PropertyValue("first replacement root");
	PropertyCollectionRoot removed = secondRoot;
	removed.entries.clear();
	PropertyCollectionRoot thirdReplacement = thirdRoot;
	thirdReplacement.entries.front().value = graph::PropertyValue("third replacement root");
	MetadataSnapshotPin before = metadata->openSnapshot();
	const std::array<PropertyCollectionRoot, 3U> roots{firstReplacement, removed, thirdReplacement};
	const std::vector<EntityPropertyRootMutationPlan> plans =
			properties.planEntityRootReplacements(before, roots, propertyOptions);
	ASSERT_EQ(plans.size(), 1U);
	ASSERT_TRUE(plans.front().page_replacement.has_value());
	EXPECT_EQ(plans.front().page_replacement->upserted_properties.size(), 2U);
	EXPECT_EQ(plans.front().page_replacement->erased_properties.size(), 1U);
	EXPECT_FALSE(plans.front().page_replacement->retire_page);
	const EntityPageBatchResult published = entities.applyNativeGraphBatchTransaction(
			before, {}, {}, {}, plans, properties, propertyOptions, LogSequenceNumber(3U));
	ASSERT_EQ(published.properties.size(), 2U);

	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(properties.readEntity(current, EntityPropertyKey{EntityPropertyOwnerKind::EPOK_NODE, 1001U}),
			  firstReplacement);
	EXPECT_FALSE(metadata->findEntityPropertyLocationOptional(
						 current, EntityPropertyKey{EntityPropertyOwnerKind::EPOK_NODE, 1002U})
					 .has_value());
	EXPECT_EQ(properties.readEntity(current, EntityPropertyKey{EntityPropertyOwnerKind::EPOK_NODE, 1003U}),
			  thirdReplacement);
	EXPECT_EQ(properties.readEntity(before, EntityPropertyKey{EntityPropertyOwnerKind::EPOK_NODE, 1001U}), firstRoot);
	EXPECT_EQ(properties.readEntity(before, EntityPropertyKey{EntityPropertyOwnerKind::EPOK_NODE, 1002U}),
			  secondRoot);
	EXPECT_EQ(properties.readEntity(before, EntityPropertyKey{EntityPropertyOwnerKind::EPOK_NODE, 1003U}), thirdRoot);
}

TEST_F(RelocatableEntityPageStoreTest, NativeBatchCoalescesLargeValueReferencesIntoOneExistingPropertyPageCow) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	SlottedPagePool pages(io, *metadata);
	LargeValueStore largeValues(io, *metadata);
	PropertyCollectionRootStore properties(pages, largeValues, *metadata);
	const PropertyCollectionRootStoreOptions propertyOptions{PhysicalSlotId(41U), PhysicalSlotId(48U), 902U};
	const PropertyCollectionRoot initialRoot{
			1001U, graph::Node::typeId, {{3U, graph::PropertyValue("existing Property root")}}};
	const std::array<EntityPageBatchNewPool, 1U> initialNodePool{{
			{.request = newPool(31U, 901U, 101U, 40U),
			 .record_kind = EntityPageRecordKind::EPRK_NODE,
			 .node_insertions = {{1001U, node(11U, 12U)}}},
	}};
	MetadataSnapshotPin empty = metadata->openSnapshot();
	const EntityPropertyRootMutationPlan initialPlan =
			properties.planEntityRootReplacement(empty, initialRoot, propertyOptions);
	const std::array initialPlans{initialPlan};
	static_cast<void>(entities.applyNativeGraphBatchTransaction(empty, {}, {}, initialNodePool, initialPlans,
															 properties, propertyOptions, LogSequenceNumber(2U)));
	empty.release();

	const size_t largeBytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
	const PropertyCollectionRoot secondRoot{
			1002U, graph::Node::typeId, {{5U, graph::PropertyValue(std::string(largeBytes, 'a'))}}};
	const PropertyCollectionRoot thirdRoot{
			1003U, graph::Node::typeId, {{7U, graph::PropertyValue(std::string(largeBytes, 'b'))}}};
	MetadataSnapshotPin before = metadata->openSnapshot();
	const std::array<PropertyCollectionRoot, 2U> newRoots{secondRoot, thirdRoot};
	const std::vector<EntityPropertyRootMutationPlan> plans =
			properties.planEntityRootReplacements(before, newRoots, propertyOptions);
	ASSERT_EQ(plans.size(), 1U);
	ASSERT_TRUE(plans.front().page_replacement.has_value());
	ASSERT_EQ(plans.front().page_replacement->upserted_properties.size(), 2U);
	ASSERT_EQ(plans.front().large_values.size(), 2U);
	EXPECT_EQ(plans.front().large_values[0].reference.value_id, LargeValueId(1U));
	EXPECT_EQ(plans.front().large_values[1].reference.value_id, LargeValueId(2U));
	const std::array<EntityPageBatchMutation, 1U> nodeMutation{{
			{.page_id = initialNodePool.front().request.page_id,
			 .record_kind = EntityPageRecordKind::EPRK_NODE,
			 .node_insertions = {{1002U, node(21U, 22U)}, {1003U, node(31U, 32U)}}},
	}};
	const EntityPageBatchResult published = entities.applyNativeGraphBatchTransaction(
			before, nodeMutation, {}, {}, plans, properties, propertyOptions, LogSequenceNumber(3U));
	ASSERT_EQ(published.properties.size(), 2U);
	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(properties.readEntity(current, published.properties[0].key), secondRoot);
	EXPECT_EQ(properties.readEntity(current, published.properties[1].key), thirdRoot);
	EXPECT_TRUE(metadata->findLargeValueManifestOptional(current, LargeValueId(1U)).has_value());
	EXPECT_TRUE(metadata->findLargeValueManifestOptional(current, LargeValueId(2U)).has_value());
	EXPECT_EQ(current.superblock().next_large_value_id, 3U);
}

TEST_F(RelocatableEntityPageStoreTest, NativeBatchSplitsNewRootsAcrossPagesWithinOnePropertyPool) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	SlottedPagePool pages(io, *metadata);
	PropertyCollectionRootStore properties(pages, *metadata);
	const PropertyCollectionRootStoreOptions propertyOptions{PhysicalSlotId(41U), PhysicalSlotId(42U), 902U};
	const size_t payloadBytes =
			(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES - RSS_SLOTTED_PAGE_HEADER_BYTES - 2U * RSS_SLOTTED_PAGE_SLOT_BYTES) / 2U;
	const PropertyCollectionRoot firstRoot{
			1001U, graph::Node::typeId, {{3U, graph::PropertyValue(std::string(payloadBytes, 'a'))}}};
	const PropertyCollectionRoot secondRoot{
			1002U, graph::Node::typeId, {{5U, graph::PropertyValue(std::string(payloadBytes, 'b'))}}};
	MetadataSnapshotPin empty = metadata->openSnapshot();
	const EntityPropertyRootMutationPlan firstPlan =
			properties.planEntityRootReplacement(empty, firstRoot, propertyOptions);
	const EntityPropertyRootMutationPlan secondPlan =
			properties.planEntityRootReplacement(empty, secondRoot, propertyOptions);
	ASSERT_EQ(firstPlan.kind, EntityPropertyRootMutationPlanKind::EPRMPK_NEW_PAGE_REQUIRED);
	ASSERT_EQ(secondPlan.kind, EntityPropertyRootMutationPlanKind::EPRMPK_NEW_PAGE_REQUIRED);
	const std::array<EntityPageBatchNewPool, 1U> newNodePool{{
			{.request = newPool(31U, 901U, 101U, 40U),
			 .record_kind = EntityPageRecordKind::EPRK_NODE,
			 .node_insertions = {{1001U, node(11U, 12U)}, {1002U, node(21U, 22U)}}},
	}};
	const std::array propertyPlans{firstPlan, secondPlan};
	const EntityPageBatchResult created = entities.applyNativeGraphBatchTransaction(
			empty, {}, {}, newNodePool, propertyPlans, properties, propertyOptions, LogSequenceNumber(2U));
	ASSERT_EQ(created.properties.size(), 2U);
	EXPECT_NE(created.properties[0].page_id, created.properties[1].page_id);
	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(properties.readEntity(current, created.properties[0].key), firstRoot);
	EXPECT_EQ(properties.readEntity(current, created.properties[1].key), secondRoot);
	EXPECT_EQ(metadata->findPageLocation(current, created.properties[0].page_id).segment_id,
			  metadata->findPageLocation(current, created.properties[1].page_id).segment_id);

	// Both existing Property pages are full, but their published pool still has
	// free frames. A later root must append a new page to that pool rather than
	// reserve a second physical pool.
	const PropertyCollectionRoot thirdRoot{
			1003U, graph::Node::typeId, {{7U, graph::PropertyValue(std::string(payloadBytes, 'c'))}}};
	const EntityPropertyRootMutationPlan thirdPlan =
			properties.planEntityRootReplacement(current, thirdRoot, propertyOptions);
	ASSERT_EQ(thirdPlan.kind, EntityPropertyRootMutationPlanKind::EPRMPK_NEW_PAGE_REQUIRED);
	ASSERT_FALSE(thirdPlan.page_replacement.has_value());
	const std::array<EntityPageBatchMutation, 1U> nodeInsertion{{
			{.page_id = newNodePool.front().request.page_id,
			 .record_kind = EntityPageRecordKind::EPRK_NODE,
			 .node_insertions = {{1003U, node(31U, 32U)}}},
	}};
	const std::array thirdPlans{thirdPlan};
	const EntityPageBatchResult appended = entities.applyNativeGraphBatchTransaction(
			current, nodeInsertion, {}, {}, thirdPlans, properties, propertyOptions, LogSequenceNumber(3U));
	ASSERT_EQ(appended.properties.size(), 1U);
	EXPECT_NE(appended.properties.front().page_id, created.properties[0].page_id);
	EXPECT_NE(appended.properties.front().page_id, created.properties[1].page_id);
	MetadataSnapshotPin afterAppend = metadata->openSnapshot();
	EXPECT_EQ(properties.readEntity(afterAppend, appended.properties.front().key), thirdRoot);
	EXPECT_EQ(metadata->findPageLocation(afterAppend, appended.properties.front().page_id).segment_id,
			  metadata->findPageLocation(afterAppend, created.properties[0].page_id).segment_id);
}

TEST_F(RelocatableEntityPageStoreTest,
	   NativeBatchReservesPropertyCowFrameBeforeAppendingAnotherPageToThatPool) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	SlottedPagePool pages(io, *metadata);
	PropertyCollectionRootStore properties(pages, *metadata);
	const PropertyCollectionRootStoreOptions propertyOptions{PhysicalSlotId(41U), PhysicalSlotId(42U), 902U};
	const size_t payloadBytes =
			(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES - RSS_SLOTTED_PAGE_HEADER_BYTES - 2U * RSS_SLOTTED_PAGE_SLOT_BYTES) / 2U;
	const PropertyCollectionRoot firstRoot{
			1001U, graph::Node::typeId, {{3U, graph::PropertyValue(std::string(payloadBytes, 'a'))}}};
	const PropertyCollectionRoot secondRoot{
			1002U, graph::Node::typeId, {{5U, graph::PropertyValue(std::string(payloadBytes, 'b'))}}};
	const std::array<EntityPageBatchNewPool, 1U> nodePool{{
			{.request = newPool(31U, 901U, 101U, 40U),
			 .record_kind = EntityPageRecordKind::EPRK_NODE,
			 .node_insertions = {{1001U, node(11U, 12U)}, {1002U, node(21U, 22U)}}},
	}};

	MetadataSnapshotPin empty = metadata->openSnapshot();
	const std::array initialPlans{
			properties.planEntityRootReplacement(empty, firstRoot, propertyOptions),
			properties.planEntityRootReplacement(empty, secondRoot, propertyOptions),
	};
	const EntityPageBatchResult initial = entities.applyNativeGraphBatchTransaction(
			empty, {}, {}, nodePool, initialPlans, properties, propertyOptions, LogSequenceNumber(2U));
	ASSERT_EQ(initial.properties.size(), 2U);
	empty.release();

	const PropertyCollectionRoot replacementFirst{
			1001U, graph::Node::typeId, {{3U, graph::PropertyValue(std::string(payloadBytes, 'c'))}}};
	const PropertyCollectionRoot thirdRoot{
			1003U, graph::Node::typeId, {{7U, graph::PropertyValue(std::string(payloadBytes, 'd'))}}};
	MetadataSnapshotPin before = metadata->openSnapshot();
	const std::array<PropertyCollectionRoot, 2U> requestedRoots{replacementFirst, thirdRoot};
	const std::vector<EntityPropertyRootMutationPlan> plans =
			properties.planEntityRootReplacements(before, requestedRoots, propertyOptions);
	ASSERT_EQ(plans.size(), 2U);
	EXPECT_TRUE(std::ranges::any_of(plans, [](const EntityPropertyRootMutationPlan &plan) {
		return plan.kind == EntityPropertyRootMutationPlanKind::EPRMPK_EXISTING_PAGE;
	}));
	EXPECT_TRUE(std::ranges::any_of(plans, [](const EntityPropertyRootMutationPlan &plan) {
		return plan.kind == EntityPropertyRootMutationPlanKind::EPRMPK_NEW_PAGE_REQUIRED;
	}));
	const std::array<EntityPageBatchMutation, 1U> nodeMutation{{
			{.page_id = nodePool.front().request.page_id,
			 .record_kind = EntityPageRecordKind::EPRK_NODE,
			 .node_updates = {{1001U, node(31U, 32U)}},
			 .node_insertions = {{1003U, node(41U, 42U)}}},
	}};
	const EntityPageBatchResult published = entities.applyNativeGraphBatchTransaction(
			before, nodeMutation, {}, {}, plans, properties, propertyOptions, LogSequenceNumber(3U));
	ASSERT_EQ(published.properties.size(), 2U);

	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(properties.readEntity(current, EntityPropertyKey{EntityPropertyOwnerKind::EPOK_NODE, 1001U}),
			  replacementFirst);
	EXPECT_EQ(properties.readEntity(current, EntityPropertyKey{EntityPropertyOwnerKind::EPOK_NODE, 1003U}), thirdRoot);
	EXPECT_EQ(metadata->findPageLocation(current, initial.properties.front().page_id).segment_id,
			  metadata->findPageLocation(current, published.properties.back().page_id).segment_id);
}

TEST_F(RelocatableEntityPageStoreTest, NativeBatchRetiresFinalOwnerKeyedPropertyPageAfterSnapshotsDrain) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	SlottedPagePool pages(io, *metadata);
	PropertyCollectionRootStore properties(pages, *metadata);
	const PropertyCollectionRootStoreOptions propertyOptions{PhysicalSlotId(41U), PhysicalSlotId(41U), 902U};
	const EntityPropertyKey key{EntityPropertyOwnerKind::EPOK_NODE, 1001U};
	const PropertyCollectionRoot root{
			1001U, graph::Node::typeId, {{3U, graph::PropertyValue("final Property root")}}};
	const std::array<EntityPageBatchNewPool, 1U> newNodePool{{
			{.request = newPool(31U, 901U, 101U, 40U),
			 .record_kind = EntityPageRecordKind::EPRK_NODE,
			 .node_insertions = {{1001U, node(11U, 12U)}}},
	}};
	MetadataSnapshotPin empty = metadata->openSnapshot();
	const EntityPropertyRootMutationPlan initialPlan =
			properties.planEntityRootReplacement(empty, root, propertyOptions);
	const std::array initialPlans{initialPlan};
	static_cast<void>(entities.applyNativeGraphBatchTransaction(empty, {}, {}, newNodePool, initialPlans, properties,
																propertyOptions, LogSequenceNumber(2U)));
	empty.release();

	MetadataSnapshotPin beforeClear = metadata->openSnapshot();
	const EntityPropertyLocation originalLocation = metadata->findEntityPropertyLocation(beforeClear, key);
	const PageLocation originalPage = metadata->findPageLocation(beforeClear, originalLocation.page_id);
	const CatalogEntry propertyPool = metadata->findCatalog(beforeClear, originalPage.segment_id);
	PropertyCollectionRoot emptyRoot = root;
	emptyRoot.entries.clear();
	const EntityPropertyRootMutationPlan clearPlan =
			properties.planEntityRootReplacement(beforeClear, emptyRoot, propertyOptions);
	ASSERT_EQ(clearPlan.kind, EntityPropertyRootMutationPlanKind::EPRMPK_EXISTING_PAGE);
	ASSERT_TRUE(clearPlan.page_replacement.has_value());
	EXPECT_TRUE(clearPlan.page_replacement->retire_page);
	EXPECT_TRUE(clearPlan.replacement_payload.empty());
	EXPECT_EQ(clearPlan.replacement_free_bytes, 0U);
	const std::array clearPlans{clearPlan};
	const EntityPageBatchResult cleared = entities.applyNativeGraphBatchTransaction(
			beforeClear, {}, {}, {}, clearPlans, properties, propertyOptions, LogSequenceNumber(3U));
	EXPECT_TRUE(cleared.properties.empty());

	MetadataSnapshotPin afterClear = metadata->openSnapshot();
	EXPECT_FALSE(metadata->findEntityPropertyLocationOptional(afterClear, key).has_value());
	EXPECT_FALSE(metadata->findPageLocationOptional(afterClear, originalPage.page_id).has_value());
	expectFormatError([&] { static_cast<void>(properties.readEntity(afterClear, key)); },
					  FormatError::FERR_INVALID_STATE);
	EXPECT_EQ(properties.readEntity(beforeClear, key), root);

	const uint8_t unavailableWhilePinned = pages.availableFrameCount(afterClear, propertyPool.segment_id);
	pages.reclaimRetiredFrames(propertyPool.segment_id);
	EXPECT_EQ(pages.availableFrameCount(afterClear, propertyPool.segment_id), unavailableWhilePinned);
	afterClear.release();
	beforeClear.release();
	pages.reclaimRetiredFrames(propertyPool.segment_id);
	MetadataSnapshotPin reclaimed = metadata->openSnapshot();
	EXPECT_EQ(pages.availableFrameCount(reclaimed, propertyPool.segment_id),
			  static_cast<uint8_t>(unavailableWhilePinned + 1U));
	reclaimed.release();

	static_cast<void>(pages.retireEmptyPool(propertyPool.segment_id, propertyPool.generation, LogSequenceNumber(4U)));
	MetadataSnapshotPin retired = metadata->openSnapshot();
	EXPECT_EQ(metadata->findCatalog(retired, propertyPool.segment_id).state, SegmentState::SGS_RETIRED);
	retired.release();
	static_cast<void>(
			pages.releaseRetiredEmptyPool(propertyPool.segment_id, propertyPool.generation, LogSequenceNumber(5U)));
	MetadataSnapshotPin released = metadata->openSnapshot();
	EXPECT_FALSE(metadata->isPhysicalSlotAllocated(released, propertyPool.physical_slot));
	EXPECT_EQ(metadata->findCatalog(released, propertyPool.segment_id).state, SegmentState::SGS_OBSOLETE);
}

TEST_F(RelocatableEntityPageStoreTest, NativeBatchKeepsSharedPropertyPageWhenAnotherRootSurvives) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	SlottedPagePool pages(io, *metadata);
	PropertyCollectionRootStore properties(pages, *metadata);
	const PropertyCollectionRootStoreOptions propertyOptions{PhysicalSlotId(41U), PhysicalSlotId(41U), 902U};
	const EntityPropertyKey firstKey{EntityPropertyOwnerKind::EPOK_NODE, 1001U};
	const EntityPropertyKey secondKey{EntityPropertyOwnerKind::EPOK_NODE, 1002U};
	const PropertyCollectionRoot firstRoot{
			1001U, graph::Node::typeId, {{3U, graph::PropertyValue("first shared Property root")}}};
	const PropertyCollectionRoot secondRoot{
			1002U, graph::Node::typeId, {{5U, graph::PropertyValue("second shared Property root")}}};
	const std::array<EntityPageBatchNewPool, 1U> newNodePool{{
			{.request = newPool(31U, 901U, 101U, 40U),
			 .record_kind = EntityPageRecordKind::EPRK_NODE,
			 .node_insertions = {{1001U, node(11U, 12U)}, {1002U, node(21U, 22U)}}},
	}};
	MetadataSnapshotPin empty = metadata->openSnapshot();
	const EntityPropertyRootMutationPlan firstPlan =
			properties.planEntityRootReplacement(empty, firstRoot, propertyOptions);
	const EntityPropertyRootMutationPlan secondPlan =
			properties.planEntityRootReplacement(empty, secondRoot, propertyOptions);
	const std::array initialPlans{firstPlan, secondPlan};
	const EntityPageBatchResult created = entities.applyNativeGraphBatchTransaction(
			empty, {}, {}, newNodePool, initialPlans, properties, propertyOptions, LogSequenceNumber(2U));
	ASSERT_EQ(created.properties.size(), 2U);
	ASSERT_EQ(created.properties[0].page_id, created.properties[1].page_id);
	empty.release();

	MetadataSnapshotPin beforeClear = metadata->openSnapshot();
	const PageLocation originalPage = metadata->findPageLocation(beforeClear, created.properties[0].page_id);
	PropertyCollectionRoot clearRoot = firstRoot;
	clearRoot.entries.clear();
	const EntityPropertyRootMutationPlan clearPlan =
			properties.planEntityRootReplacement(beforeClear, clearRoot, propertyOptions);
	ASSERT_EQ(clearPlan.kind, EntityPropertyRootMutationPlanKind::EPRMPK_EXISTING_PAGE);
	ASSERT_TRUE(clearPlan.page_replacement.has_value());
	EXPECT_FALSE(clearPlan.page_replacement->retire_page);
	const std::array clearPlans{clearPlan};
	static_cast<void>(entities.applyNativeGraphBatchTransaction(beforeClear, {}, {}, {}, clearPlans, properties,
																propertyOptions, LogSequenceNumber(3U)));

	MetadataSnapshotPin afterClear = metadata->openSnapshot();
	EXPECT_FALSE(metadata->findEntityPropertyLocationOptional(afterClear, firstKey).has_value());
	EXPECT_EQ(properties.readEntity(afterClear, secondKey), secondRoot);
	EXPECT_NE(metadata->findPageLocation(afterClear, created.properties[0].page_id).page_index,
			  originalPage.page_index);
	EXPECT_EQ(properties.readEntity(beforeClear, firstKey), firstRoot);
	EXPECT_EQ(properties.readEntity(beforeClear, secondKey), secondRoot);
}

TEST_F(RelocatableEntityPageStoreTest, NativeBatchPublishesPropertyPageRetirementOnlyAtCompleteSuperblockBoundary) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	SlottedPagePool pages(io, *metadata);
	PropertyCollectionRootStore properties(pages, *metadata);
	const PropertyCollectionRootStoreOptions propertyOptions{PhysicalSlotId(41U), PhysicalSlotId(41U), 902U};
	const EntityPropertyKey key{EntityPropertyOwnerKind::EPOK_NODE, 1001U};
	const PropertyCollectionRoot root{
			1001U, graph::Node::typeId, {{3U, graph::PropertyValue("fault-injected Property retirement")}}};
	const std::array<EntityPageBatchNewPool, 1U> newNodePool{{
			{.request = newPool(31U, 901U, 101U, 40U),
			 .record_kind = EntityPageRecordKind::EPRK_NODE,
			 .node_insertions = {{1001U, node(11U, 12U)}}},
	}};
	MetadataSnapshotPin empty = metadata->openSnapshot();
	const EntityPropertyRootMutationPlan initialPlan =
			properties.planEntityRootReplacement(empty, root, propertyOptions);
	const std::array initialPlans{initialPlan};
	static_cast<void>(entities.applyNativeGraphBatchTransaction(empty, {}, {}, newNodePool, initialPlans, properties,
																propertyOptions, LogSequenceNumber(2U)));
	empty.release();

	MetadataSnapshotPin beforeClear = metadata->openSnapshot();
	const PageId pageId = metadata->findEntityPropertyLocation(beforeClear, key).page_id;
	PropertyCollectionRoot emptyRoot = root;
	emptyRoot.entries.clear();
	EntityPropertyRootMutationPlan clearPlan =
			properties.planEntityRootReplacement(beforeClear, emptyRoot, propertyOptions);
	const std::array firstClearPlans{clearPlan};
	MetadataPublishOptions beforeInactive;
	beforeInactive.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE;
	EXPECT_THROW(static_cast<void>(entities.applyNativeGraphBatchTransaction(beforeClear, {}, {}, {}, firstClearPlans,
																			 properties, propertyOptions,
																			 LogSequenceNumber(3U), beforeInactive)),
				 BootstrapPublishInterrupted);
	MetadataSnapshotPin afterBeforeInactive = metadata->openSnapshot();
	EXPECT_EQ(afterBeforeInactive.superblock(), beforeClear.superblock());
	EXPECT_EQ(metadata->findEntityPropertyLocation(afterBeforeInactive, key).page_id, pageId);
	EXPECT_TRUE(metadata->findPageLocationOptional(afterBeforeInactive, pageId).has_value());
	EXPECT_EQ(properties.readEntity(afterBeforeInactive, key), root);
	afterBeforeInactive.release();
	beforeClear.release();

	MetadataSnapshotPin beforeAfterInactive = metadata->openSnapshot();
	clearPlan = properties.planEntityRootReplacement(beforeAfterInactive, emptyRoot, propertyOptions);
	const std::array secondClearPlans{clearPlan};
	MetadataPublishOptions afterInactive;
	afterInactive.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE;
	EXPECT_THROW(static_cast<void>(entities.applyNativeGraphBatchTransaction(
						 beforeAfterInactive, {}, {}, {}, secondClearPlans, properties, propertyOptions,
						 LogSequenceNumber(3U), afterInactive)),
				 BootstrapPublishInterrupted);
	beforeAfterInactive.release();

	metadata.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	SlottedPagePool reopenedPages(io, *reopened);
	PropertyCollectionRootStore reopenedProperties(reopenedPages, *reopened);
	MetadataSnapshotPin published = reopened->openSnapshot();
	EXPECT_FALSE(reopened->findEntityPropertyLocationOptional(published, key).has_value());
	EXPECT_FALSE(reopened->findPageLocationOptional(published, pageId).has_value());
	expectFormatError([&] { static_cast<void>(reopenedProperties.readEntity(published, key)); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableEntityPageStoreTest, ReusesFreedSlotsOnlyWithANewDurableGeneration) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore store(io, *metadata);
	const NodeEntityRecord firstNode = node(11U, 12U);
	const std::optional<EntityPageInsertResult> first =
			store.allocateNode(newPool(31U, 901U, 101U, 40U), 1001U, firstNode, LogSequenceNumber(2U));
	ASSERT_TRUE(first.has_value());
	MetadataSnapshotPin afterFirst = metadata->openSnapshot();
	const NodeEntityRecord secondNode = node(21U, 22U, 17U);
	const EntityPageInsertResult second =
			store.insertNode(afterFirst, PageId(101U), 1002U, secondNode, LogSequenceNumber(3U));
	MetadataSnapshotPin full = metadata->openSnapshot();
	const EntityPageLocation firstLocation = metadata->findNodePageLocation(full, 1001U);
	EXPECT_EQ(firstLocation, first->location);
	EXPECT_EQ(metadata->findNodePageLocation(full, 1002U), second.location);

	static_cast<void>(store.eraseNode(full, 1001U, LogSequenceNumber(4U)));
	MetadataSnapshotPin afterErase = metadata->openSnapshot();
	EXPECT_FALSE(metadata->findNodePageLocationOptional(afterErase, 1001U).has_value());
	EXPECT_EQ(store.readNode(full, 1001U), firstNode);
	EXPECT_EQ(store.readNode(afterErase, 1002U), secondNode);
	const NodeEntityRecord replacementNode = node(31U, 32U, 27U);
	const EntityPageInsertResult replacement =
			store.insertNode(afterErase, PageId(101U), 1003U, replacementNode, LogSequenceNumber(5U));
	EXPECT_EQ(replacement.location.slot_id, firstLocation.slot_id);
	EXPECT_EQ(replacement.location.slot_generation, firstLocation.slot_generation + 1U);

	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(store.readNode(current, 1002U), secondNode);
	EXPECT_EQ(store.readNode(current, 1003U), replacementNode);
	EXPECT_EQ(metadata->findNodePageLocation(current, 1003U), replacement.location);
	SharedPagePoolAllocator readOnlyAllocator(io, *metadata, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
	const SharedPagePoolContents contents = readOnlyAllocator.read(current, PageId(101U));
	const EntityPage page = EntityPage::open(contents.owner, contents.payload);
	expectFormatError([&] { static_cast<void>(page.findNode(firstLocation.slot_id, firstLocation.slot_generation)); },
					  FormatError::FERR_INVALID_STATE);
	EXPECT_EQ(*page.findNode(replacement.location.slot_id, replacement.location.slot_generation), replacementNode);
}

TEST_F(RelocatableEntityPageStoreTest, EvacuatesEntityPagesWithoutChangingEntityMapLocations) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore store(io, *metadata);
	const NodeEntityRecord sourceNode = node(11U, 12U);
	const std::optional<EntityPageInsertResult> source =
			store.allocateNode(newPool(31U, 901U, 101U, 40U), 1001U, sourceNode, LogSequenceNumber(2U));
	ASSERT_TRUE(source.has_value());
	const NodeEntityRecord destinationNode = node(21U, 22U, 17U);
	const std::optional<EntityPageInsertResult> destination =
			store.allocateNode(newPool(32U, 901U, 102U, 41U), 1002U, destinationNode, LogSequenceNumber(3U));
	ASSERT_TRUE(destination.has_value());
	MetadataSnapshotPin original = metadata->openSnapshot();
	const EntityPageMutationResult moved =
			store.evacuateNodePage(original, PageId(101U), SegmentId(32U), LogSequenceNumber(4U));
	EXPECT_EQ(moved.pool.segment_id, SegmentId(32U));
	EXPECT_NE(moved.page.page_index, destination->page.page_index);
	EXPECT_EQ(store.readNode(original, 1001U), sourceNode);
	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(metadata->findPageLocation(current, PageId(101U)), moved.page);
	EXPECT_EQ(metadata->findNodePageLocation(current, 1001U), source->location);
	EXPECT_EQ(store.readNode(current, 1001U), sourceNode);
	EXPECT_EQ(store.readNode(current, 1002U), destinationNode);
	expectFormatError(
			[&] {
				static_cast<void>(
						store.evacuateNodePage(original, PageId(101U), SegmentId(32U), LogSequenceNumber(5U)));
			},
			FormatError::FERR_INVALID_STATE);
	original.release();
	store.reclaimRetiredFrames(SegmentId(31U));
	current.release();
	static_cast<void>(store.retireEmptyPool(SegmentId(31U), Generation(1U), LogSequenceNumber(5U)));
	MetadataSnapshotPin retired = metadata->openSnapshot();
	EXPECT_EQ(metadata->findCatalog(retired, SegmentId(31U)).state, SegmentState::SGS_RETIRED);
}

TEST_F(RelocatableEntityPageStoreTest, FailedPublicationAndWrongOwnerNeverExposeAnEntityMapping) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore store(io, *metadata);
	MetadataPublishOptions interrupted;
	interrupted.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE;
	EXPECT_THROW(static_cast<void>(store.allocateNode(newPool(31U, 901U, 101U, 40U), 1001U, node(11U, 12U),
													  LogSequenceNumber(2U), interrupted)),
				 BootstrapPublishInterrupted);
	MetadataSnapshotPin beforeRecovery = metadata->openSnapshot();
	EXPECT_FALSE(metadata->isPhysicalSlotAllocated(beforeRecovery, PhysicalSlotId(40U)));
	EXPECT_FALSE(metadata->findNodePageLocationOptional(beforeRecovery, 1001U).has_value());
	beforeRecovery.release();

	const std::optional<EntityPageInsertResult> allocated =
			store.allocateNode(newPool(31U, 901U, 101U, 40U), 1001U, node(11U, 12U), LogSequenceNumber(2U));
	ASSERT_TRUE(allocated.has_value());
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const PageOwnerHeader wrongOwner{
			PageId(101U),		  902U, PageOwnerKind::POK_ENTITY, 0U, RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, Generation(1U),
			LogSequenceNumber(2U)};
	snapshot.release();
	overwritePageOwner(io, *metadata, PageId(101U), wrongOwner);
	snapshot = metadata->openSnapshot();
	expectFormatError([&] { static_cast<void>(store.readNode(snapshot, 1001U)); }, FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableEntityPageStoreTest, MaintainsEdgePagesAcrossAppendCowDeletionAndEvacuation) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore store(io, *metadata);
	const EdgeEntityRecord firstEdge = edge(10U, 11U, 21U);
	const std::optional<EntityPageInsertResult> first =
			store.allocateEdge(newPool(31U, 901U, 201U, 40U), 2001U, firstEdge, LogSequenceNumber(2U));
	ASSERT_TRUE(first.has_value());
	const EdgeEntityRecord destinationEdge = edge(12U, 13U, 22U);
	const std::optional<EntityPageInsertResult> destination =
			store.allocateEdge(newPool(32U, 901U, 202U, 41U), 2002U, destinationEdge, LogSequenceNumber(3U));
	ASSERT_TRUE(destination.has_value());

	MetadataSnapshotPin initial = metadata->openSnapshot();
	const EdgeEntityRecord appendedEdge = edge(14U, 15U, 23U);
	const EntityPageInsertResult appended =
			store.appendEdgePage({first->pool, PageId(203U)}, 2003U, appendedEdge, LogSequenceNumber(4U));
	EXPECT_EQ(appended.location.page_id, PageId(203U));
	EXPECT_EQ(store.readEdge(initial, 2001U), firstEdge);
	expectFormatError([&] { static_cast<void>(store.readEdge(initial, 2003U)); }, FormatError::FERR_INVALID_STATE);

	MetadataSnapshotPin beforeInsert = metadata->openSnapshot();
	const EdgeEntityRecord insertedEdge = edge(16U, 17U, 24U);
	const EntityPageInsertResult inserted =
			store.insertEdge(beforeInsert, PageId(201U), 2004U, insertedEdge, LogSequenceNumber(5U));
	EXPECT_EQ(inserted.location.page_id, PageId(201U));
	EXPECT_EQ(store.freeSlotCount(metadata->openSnapshot(), PageId(201U), EntityPageRecordKind::EPRK_EDGE),
			  EntityPage::create({PageId(1U), 1U, PageOwnerKind::POK_ENTITY, 0U, RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
								  Generation(1U), LogSequenceNumber(1U)},
								 EntityPageRecordKind::EPRK_EDGE)
							  .header()
							  .slot_capacity -
					  2U);

	MetadataSnapshotPin beforeUpdate = metadata->openSnapshot();
	const EdgeEntityRecord replacement = edge(18U, 19U, 25U);
	static_cast<void>(store.updateEdge(beforeUpdate, 2004U, replacement, LogSequenceNumber(6U)));
	MetadataSnapshotPin afterUpdate = metadata->openSnapshot();
	EXPECT_EQ(store.readEdge(beforeUpdate, 2004U), insertedEdge);
	EXPECT_EQ(store.readEdge(afterUpdate, 2004U), replacement);
	static_cast<void>(store.eraseEdge(afterUpdate, 2004U, LogSequenceNumber(7U)));
	MetadataSnapshotPin afterPartialErase = metadata->openSnapshot();
	EXPECT_EQ(store.readEdge(afterPartialErase, 2001U), firstEdge);
	expectFormatError([&] { static_cast<void>(store.readEdge(afterPartialErase, 2004U)); },
					  FormatError::FERR_INVALID_STATE);

	const EntityPageMutationResult moved =
			store.evacuateEdgePage(afterPartialErase, PageId(203U), SegmentId(32U), LogSequenceNumber(8U));
	EXPECT_EQ(moved.pool.segment_id, SegmentId(32U));
	MetadataSnapshotPin afterEvacuation = metadata->openSnapshot();
	EXPECT_EQ(store.readEdge(afterEvacuation, 2003U), appendedEdge);
	EXPECT_EQ(metadata->findPageLocation(afterEvacuation, PageId(203U)), moved.page);

	static_cast<void>(store.eraseEdge(afterEvacuation, 2001U, LogSequenceNumber(9U)));
	MetadataSnapshotPin afterFinalErase = metadata->openSnapshot();
	EXPECT_FALSE(metadata->findEdgePageLocationOptional(afterFinalErase, 2001U).has_value());
	EXPECT_FALSE(metadata->findPageLocationOptional(afterFinalErase, PageId(201U)).has_value());
	EXPECT_EQ(store.readEdge(afterFinalErase, 2002U), destinationEdge);
	EXPECT_EQ(store.readEdge(afterFinalErase, 2003U), appendedEdge);

	initial.release();
	beforeInsert.release();
	beforeUpdate.release();
	afterUpdate.release();
	afterPartialErase.release();
	afterEvacuation.release();
	afterFinalErase.release();
	store.reclaimRetiredFrames(SegmentId(31U));
}

TEST_F(RelocatableEntityPageStoreTest, RejectsInvalidRequestsAndDoesNotPublishNoSpaceAllocations) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore store(io, *metadata);
	MetadataSnapshotPin empty = metadata->openSnapshot();

	expectFormatError([&] { static_cast<void>(store.readNode(empty, 0U)); }, FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(store.readEdge(empty, 0U)); }, FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(store.freeSlotCount(empty, PageId{}, EntityPageRecordKind::EPRK_NODE)); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(store.freeSlotCount(empty, PageId(101U), static_cast<EntityPageRecordKind>(77U)));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] { static_cast<void>(store.evacuateNodePage(empty, PageId{}, SegmentId(31U), LogSequenceNumber(2U))); },
			FormatError::FERR_INVALID_HEADER);

	EntityPageNewPoolRequest invalid = newPool(31U, 901U, 101U, 40U);
	invalid.first_slot = PhysicalSlotId(41U);
	invalid.last_slot = PhysicalSlotId(40U);
	expectFormatError(
			[&] { static_cast<void>(store.allocateNode(invalid, 1001U, node(11U, 12U), LogSequenceNumber(2U))); },
			FormatError::FERR_INVALID_HEADER);
	invalid = newPool(31U, 901U, 101U, 40U);
	expectFormatError(
			[&] { static_cast<void>(store.allocateNode(invalid, 0U, node(11U, 12U), LogSequenceNumber(2U))); },
			FormatError::FERR_INVALID_HEADER);
	NodeEntityRecord invalidNode = node(11U, 12U);
	invalidNode.inline_label_ids[1] = invalidNode.inline_label_ids[0];
	expectFormatError(
			[&] { static_cast<void>(store.allocateNode(invalid, 1001U, invalidNode, LogSequenceNumber(2U))); },
			FormatError::FERR_INVALID_HEADER);

	const std::optional<EntityPageInsertResult> first =
			store.allocateNode(newPool(31U, 901U, 101U, 40U), 1001U, node(11U, 12U), LogSequenceNumber(2U));
	ASSERT_TRUE(first.has_value());
	const std::optional<EntityPageInsertResult> unavailable =
			store.allocateNode(newPool(32U, 902U, 102U, 40U), 1002U, node(21U, 22U), LogSequenceNumber(3U));
	EXPECT_FALSE(unavailable.has_value());

	expectFormatError(
			[&] { static_cast<void>(store.appendNodePage({}, 1002U, node(21U, 22U), LogSequenceNumber(3U))); },
			FormatError::FERR_INVALID_HEADER);
	const EntityPageAppendRequest duplicatePage{first->pool, PageId(101U)};
	expectFormatError(
			[&] {
				static_cast<void>(store.appendNodePage(duplicatePage, 1002U, node(21U, 22U), LogSequenceNumber(3U)));
			},
			FormatError::FERR_INVALID_STATE);

	MetadataSnapshotPin current = metadata->openSnapshot();
	expectFormatError(
			[&] {
				static_cast<void>(store.insertNode(current, PageId(101U), 0U, node(21U, 22U), LogSequenceNumber(3U)));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(store.insertNode(current, PageId(101U), 1002U, node(21U, 22U), LogSequenceNumber{}));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(store.updateNode(current, 0U, node(21U, 22U), LogSequenceNumber(3U))); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(store.updateNode(current, 1001U, node(21U, 22U), LogSequenceNumber{})); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(store.eraseNode(current, 0U, LogSequenceNumber(3U))); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(store.eraseNode(current, 1001U, LogSequenceNumber{})); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(store.evacuateNodePage(current, PageId(101U), SegmentId(31U), LogSequenceNumber(3U)));
			},
			FormatError::FERR_INVALID_STATE);
	expectFormatError(
			[&] {
				static_cast<void>(store.evacuateNodePage(current, PageId(101U), SegmentId(31U), LogSequenceNumber{}));
			},
			FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableEntityPageStoreTest, FailsClosedForWrongRecordKind) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore store(io, *metadata);
	const std::optional<EntityPageInsertResult> allocated =
			store.allocateEdge(newPool(31U, 901U, 201U, 40U), 2001U, edge(10U, 11U, 21U), LogSequenceNumber(2U));
	ASSERT_TRUE(allocated.has_value());
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	expectFormatError(
			[&] { static_cast<void>(store.freeSlotCount(snapshot, PageId(201U), EntityPageRecordKind::EPRK_NODE)); },
			FormatError::FERR_INVALID_STATE);
	EXPECT_EQ(store.readEdge(snapshot, 2001U), edge(10U, 11U, 21U));
}

TEST_F(RelocatableEntityPageStoreTest, BatchCowPublishesNodeAndEdgePagesAndMapsInOneGeneration) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore store(io, *metadata);
	const NodeEntityRecord originalNode = node(11U, 12U);
	const EdgeEntityRecord originalEdge = edge(1001U, 1002U, 21U);
	const std::optional<EntityPageInsertResult> allocatedNode =
			store.allocateNode(newPool(31U, 901U, 101U, 40U), 1001U, originalNode, LogSequenceNumber(2U));
	const std::optional<EntityPageInsertResult> allocatedEdge =
			store.allocateEdge(newPool(32U, 902U, 201U, 41U), 2001U, originalEdge, LogSequenceNumber(3U));
	ASSERT_TRUE(allocatedNode.has_value());
	ASSERT_TRUE(allocatedEdge.has_value());

	MetadataSnapshotPin before = metadata->openSnapshot();
	const NodeEntityRecord updatedNode = node(81U, 82U, 37U);
	const NodeEntityRecord insertedNode = node(91U, 92U, 47U);
	const EdgeEntityRecord updatedEdge = edge(1001U, 1003U, 31U);
	const EdgeEntityRecord insertedEdge = edge(1002U, 1001U, 32U);
	const std::array<EntityPageBatchMutation, 2> mutations{{
			{.page_id = PageId(101U),
			 .record_kind = EntityPageRecordKind::EPRK_NODE,
			 .node_updates = {{1001U, updatedNode}},
			 .node_insertions = {{1002U, insertedNode}}},
			{.page_id = PageId(201U),
			 .record_kind = EntityPageRecordKind::EPRK_EDGE,
			 .edge_updates = {{2001U, updatedEdge}},
			 .edge_insertions = {{2002U, insertedEdge}}},
	}};
	const EntityPageBatchResult result = store.applyBatch(before, mutations, LogSequenceNumber(4U));
	ASSERT_EQ(result.pages.size(), mutations.size());
	EXPECT_EQ(result.superblock.generation.value(), before.superblock().generation.value() + 1U);
	EXPECT_EQ(result.pages[0].pool.segment_id, SegmentId(31U));
	EXPECT_EQ(result.pages[1].pool.segment_id, SegmentId(32U));
	EXPECT_NE(result.pages[0].page.page_index, allocatedNode->page.page_index);
	EXPECT_NE(result.pages[1].page.page_index, allocatedEdge->page.page_index);

	EXPECT_EQ(store.readNode(before, 1001U), originalNode);
	EXPECT_EQ(store.readEdge(before, 2001U), originalEdge);
	expectFormatError([&] { static_cast<void>(store.readNode(before, 1002U)); }, FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(store.readEdge(before, 2002U)); }, FormatError::FERR_INVALID_STATE);

	const MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(current.superblock(), result.superblock);
	EXPECT_EQ(store.readNode(current, 1001U), updatedNode);
	EXPECT_EQ(store.readNode(current, 1002U), insertedNode);
	EXPECT_EQ(store.readEdge(current, 2001U), updatedEdge);
	EXPECT_EQ(store.readEdge(current, 2002U), insertedEdge);
	EXPECT_EQ(metadata->findNodePageLocation(current, 1001U), allocatedNode->location);
	EXPECT_EQ(metadata->findNodePageLocation(current, 1002U).page_id, PageId(101U));
	EXPECT_EQ(metadata->findEdgePageLocation(current, 2001U), allocatedEdge->location);
	EXPECT_EQ(metadata->findEdgePageLocation(current, 2002U).page_id, PageId(201U));
	EXPECT_EQ(metadata->findPageLocation(current, PageId(101U)), result.pages[0].page);
	EXPECT_EQ(metadata->findPageLocation(current, PageId(201U)), result.pages[1].page);
}

TEST_F(RelocatableEntityPageStoreTest, BatchMutationAtomicallyRewritesNodesAndAppendsEdgePage) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore store(io, *metadata);
	const NodeEntityRecord originalSource = node(0U, 0U);
	const std::optional<EntityPageInsertResult> source =
			store.allocateNode(newPool(31U, 901U, 101U, 40U), 1001U, originalSource, LogSequenceNumber(2U));
	ASSERT_TRUE(source.has_value());
	const NodeEntityRecord originalTarget = node(0U, 0U, 17U);
	const EntityPageInsertResult target =
			store.appendNodePage({source->pool, PageId(102U)}, 1002U, originalTarget, LogSequenceNumber(3U));

	MetadataSnapshotPin before = metadata->openSnapshot();
	NodeEntityRecord updatedSource = originalSource;
	updatedSource.inline_label_ids = {13U, 15U, 17U};
	NodeEntityRecord updatedTarget = originalTarget;
	updatedTarget.inline_label_ids = {23U, 25U, 27U};
	const EdgeEntityRecord createdEdge = edge(1001U, 1002U, 51U);
	const std::array<EntityPageBatchMutation, 2U> mutations{{
			{.page_id = source->page.page_id,
			 .record_kind = EntityPageRecordKind::EPRK_NODE,
			 .node_updates = {{1001U, updatedSource}}},
			{.page_id = target.page.page_id,
			 .record_kind = EntityPageRecordKind::EPRK_NODE,
			 .node_updates = {{1002U, updatedTarget}}},
	}};
	const std::array<EntityPageBatchAppend, 1U> appends{{
			{.request = {source->pool, PageId(201U)},
			 .record_kind = EntityPageRecordKind::EPRK_EDGE,
			 .edge_insertions = {{2001U, createdEdge}}},
	}};
	const EntityPageBatchResult result = store.applyBatchWithAppends(before, mutations, appends, LogSequenceNumber(4U));
	ASSERT_EQ(result.pages.size(), mutations.size());
	ASSERT_EQ(result.appended_pages.size(), appends.size());
	ASSERT_EQ(result.appended_pages.front().locations.size(), 1U);
	EXPECT_EQ(result.superblock.generation, Generation(4U));
	EXPECT_EQ(result.appended_pages.front().pool.segment_id, source->pool.segment_id);
	EXPECT_EQ(result.appended_pages.front().page.page_id, PageId(201U));
	EXPECT_EQ(result.appended_pages.front().locations.front(), (EntityPageLocation{2001U, PageId(201U), 0U, 1U}));

	EXPECT_EQ(store.readNode(before, 1001U), originalSource);
	EXPECT_EQ(store.readNode(before, 1002U), originalTarget);
	expectFormatError([&] { static_cast<void>(store.readEdge(before, 2001U)); }, FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(metadata->findPageLocation(before, PageId(201U))); },
					  FormatError::FERR_INVALID_STATE);

	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(store.readNode(current, 1001U), updatedSource);
	EXPECT_EQ(store.readNode(current, 1002U), updatedTarget);
	EXPECT_EQ(store.readEdge(current, 2001U), createdEdge);
	EXPECT_EQ(metadata->findEdgePageLocation(current, 2001U), result.appended_pages.front().locations.front());
	EXPECT_EQ(metadata->findPageLocation(current, PageId(201U)), result.appended_pages.front().page);
	EXPECT_NE(result.pages[0].page.page_index, source->page.page_index);
	EXPECT_NE(result.pages[1].page.page_index, target.page.page_index);
}

TEST_F(RelocatableEntityPageStoreTest, BatchMutationAppendFailpointRecoversAllFramesForRetry) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore store(io, *metadata);
	const std::optional<EntityPageInsertResult> source =
			store.allocateNode(newPool(31U, 901U, 101U, 40U), 1001U, node(0U, 0U), LogSequenceNumber(2U));
	ASSERT_TRUE(source.has_value());
	const MetadataSnapshotPin before = metadata->openSnapshot();
	const std::array<EntityPageBatchMutation, 1U> mutations{{
			{.page_id = PageId(101U),
			 .record_kind = EntityPageRecordKind::EPRK_NODE,
			 .node_updates = {{1001U, node(2001U, 0U)}}},
	}};
	const std::array<EntityPageBatchAppend, 1U> appends{{
			{.request = {source->pool, PageId(201U)},
			 .record_kind = EntityPageRecordKind::EPRK_EDGE,
			 .edge_insertions = {{2001U, edge(1001U, 1002U, 51U)}}},
	}};
	MetadataPublishOptions interrupted;
	interrupted.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE;
	EXPECT_THROW(static_cast<void>(
						 store.applyBatchWithAppends(before, mutations, appends, LogSequenceNumber(3U), interrupted)),
				 BootstrapPublishInterrupted);
	const MetadataSnapshotPin afterFailure = metadata->openSnapshot();
	EXPECT_EQ(afterFailure.superblock(), before.superblock());
	EXPECT_EQ(store.readNode(afterFailure, 1001U), node(0U, 0U));
	EXPECT_FALSE(metadata->findPageLocationOptional(afterFailure, PageId(201U)).has_value());
	EXPECT_FALSE(metadata->findEdgePageLocationOptional(afterFailure, 2001U).has_value());

	const EntityPageBatchResult retried =
			store.applyBatchWithAppends(afterFailure, mutations, appends, LogSequenceNumber(4U));
	const MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(current.superblock(), retried.superblock);
	EXPECT_EQ(store.readNode(current, 1001U), mutations.front().node_updates.front().record);
	EXPECT_EQ(store.readEdge(current, 2001U), appends.front().edge_insertions.front().record);
	EXPECT_TRUE(metadata->findPageLocationOptional(current, PageId(201U)).has_value());
}

TEST_F(RelocatableEntityPageStoreTest, BatchTransactionAtomicallyAllocatesNewEdgePool) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore store(io, *metadata);
	const NodeEntityRecord originalNode = node(0U, 0U);
	const std::optional<EntityPageInsertResult> source =
			store.allocateNode(newPool(31U, 901U, 101U, 40U), 1001U, originalNode, LogSequenceNumber(2U));
	ASSERT_TRUE(source.has_value());
	const MetadataSnapshotPin before = metadata->openSnapshot();
	NodeEntityRecord updatedNode = originalNode;
	updatedNode.inline_label_ids = {13U, 15U, 17U};
	const std::array<EntityPageBatchMutation, 1U> mutations{{
			{.page_id = source->page.page_id,
			 .record_kind = EntityPageRecordKind::EPRK_NODE,
			 .node_updates = {{1001U, updatedNode}}},
	}};
	const std::array<EntityPageBatchNewPool, 1U> newPools{{
			{.request = newPool(32U, 902U, 201U, 41U),
			 .record_kind = EntityPageRecordKind::EPRK_EDGE,
			 .edge_insertions = {{2001U, edge(1001U, 1002U, 51U)}}},
	}};
	const EntityPageBatchResult result =
			store.applyBatchTransaction(before, mutations, {}, newPools, LogSequenceNumber(3U));
	ASSERT_EQ(result.pages.size(), 1U);
	ASSERT_TRUE(result.appended_pages.empty());
	ASSERT_EQ(result.new_pool_pages.size(), 1U);
	ASSERT_EQ(result.new_pool_pages.front().locations.size(), 1U);
	EXPECT_EQ(result.new_pool_pages.front().pool.segment_id, SegmentId(32U));
	EXPECT_EQ(result.new_pool_pages.front().pool.physical_slot, PhysicalSlotId(41U));
	EXPECT_EQ(result.new_pool_pages.front().page.page_id, PageId(201U));
	EXPECT_EQ(result.new_pool_pages.front().locations.front(), (EntityPageLocation{2001U, PageId(201U), 0U, 1U}));

	EXPECT_EQ(store.readNode(before, 1001U), originalNode);
	expectFormatError([&] { static_cast<void>(store.readEdge(before, 2001U)); }, FormatError::FERR_INVALID_STATE);
	const MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(store.readNode(current, 1001U), updatedNode);
	EXPECT_EQ(store.readEdge(current, 2001U), newPools.front().edge_insertions.front().record);
	const CatalogEntry currentNewPool = metadata->findCatalog(current, SegmentId(32U));
	EXPECT_EQ(currentNewPool.segment_id, result.new_pool_pages.front().pool.segment_id);
	EXPECT_EQ(currentNewPool.physical_slot, result.new_pool_pages.front().pool.physical_slot);
	EXPECT_EQ(currentNewPool.generation, result.new_pool_pages.front().pool.generation);
	EXPECT_EQ(metadata->findPageLocation(current, PageId(201U)), result.new_pool_pages.front().page);
	EXPECT_EQ(metadata->findEdgePageLocation(current, 2001U), result.new_pool_pages.front().locations.front());
}

TEST_F(RelocatableEntityPageStoreTest, BatchTransactionNewPoolFailpointPreservesOldTopologyAndRetries) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore store(io, *metadata);
	const std::optional<EntityPageInsertResult> source =
			store.allocateNode(newPool(31U, 901U, 101U, 40U), 1001U, node(0U, 0U), LogSequenceNumber(2U));
	ASSERT_TRUE(source.has_value());
	const MetadataSnapshotPin before = metadata->openSnapshot();
	const std::array<EntityPageBatchMutation, 1U> mutations{{
			{.page_id = PageId(101U),
			 .record_kind = EntityPageRecordKind::EPRK_NODE,
			 .node_updates = {{1001U, node(2001U, 0U)}}},
	}};
	const std::array<EntityPageBatchNewPool, 1U> newPools{{
			{.request = newPool(32U, 902U, 201U, 41U),
			 .record_kind = EntityPageRecordKind::EPRK_EDGE,
			 .edge_insertions = {{2001U, edge(1001U, 1002U, 51U)}}},
	}};
	MetadataPublishOptions interrupted;
	interrupted.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE;
	EXPECT_THROW(static_cast<void>(store.applyBatchTransaction(before, mutations, {}, newPools, LogSequenceNumber(3U),
															   interrupted)),
				 BootstrapPublishInterrupted);
	const MetadataSnapshotPin afterFailure = metadata->openSnapshot();
	EXPECT_EQ(afterFailure.superblock(), before.superblock());
	EXPECT_EQ(store.readNode(afterFailure, 1001U), node(0U, 0U));
	EXPECT_FALSE(metadata->isPhysicalSlotAllocated(afterFailure, PhysicalSlotId(41U)));
	EXPECT_FALSE(metadata->findPageLocationOptional(afterFailure, PageId(201U)).has_value());
	EXPECT_FALSE(metadata->findEdgePageLocationOptional(afterFailure, 2001U).has_value());

	const EntityPageBatchResult retried =
			store.applyBatchTransaction(afterFailure, mutations, {}, newPools, LogSequenceNumber(4U));
	const MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(current.superblock(), retried.superblock);
	EXPECT_EQ(store.readNode(current, 1001U), mutations.front().node_updates.front().record);
	EXPECT_EQ(store.readEdge(current, 2001U), newPools.front().edge_insertions.front().record);
	EXPECT_EQ(metadata->findCatalog(current, SegmentId(32U)).physical_slot, PhysicalSlotId(41U));
}

TEST_F(RelocatableEntityPageStoreTest, BatchTransactionRecoversPublishedNewPoolAfterInactiveSuperblockFailure) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore store(io, *metadata);
	const MetadataSnapshotPin before = metadata->openSnapshot();
	const std::array<EntityPageBatchNewPool, 1U> newPools{{
			{.request = newPool(32U, 902U, 201U, 41U),
			 .record_kind = EntityPageRecordKind::EPRK_EDGE,
			 .edge_insertions = {{2001U, edge(1001U, 1002U, 51U)}}},
	}};
	MetadataPublishOptions interrupted;
	interrupted.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE;
	EXPECT_THROW(static_cast<void>(
						 store.applyBatchTransaction(before, {}, {}, newPools, LogSequenceNumber(2U), interrupted)),
				 BootstrapPublishInterrupted);
	const MetadataSnapshotPin recovered = metadata->openSnapshot();
	EXPECT_GT(recovered.superblock().generation, before.superblock().generation);
	EXPECT_EQ(store.readEdge(recovered, 2001U), newPools.front().edge_insertions.front().record);
	EXPECT_EQ(metadata->findCatalog(recovered, SegmentId(32U)).physical_slot, PhysicalSlotId(41U));
}

TEST_F(RelocatableEntityPageStoreTest, BatchTransactionAllocatesSeveralNewPoolsFromOneOverlappingRange) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore store(io, *metadata);
	const MetadataSnapshotPin before = metadata->openSnapshot();
	EntityPageBatchNewPool nodePool{.request = newPool(31U, 901U, 101U, 40U),
									.record_kind = EntityPageRecordKind::EPRK_NODE,
									.node_insertions = {{1001U, node(0U, 0U)}}};
	EntityPageBatchNewPool edgePool{.request = newPool(32U, 902U, 201U, 40U),
									.record_kind = EntityPageRecordKind::EPRK_EDGE,
									.edge_insertions = {{2001U, edge(1001U, 1002U, 51U)}}};
	edgePool.request.last_slot = PhysicalSlotId(41U);
	const std::array newPools{nodePool, edgePool};
	const EntityPageBatchResult result = store.applyBatchTransaction(before, {}, {}, newPools, LogSequenceNumber(2U));
	ASSERT_EQ(result.new_pool_pages.size(), 2U);
	EXPECT_EQ(result.new_pool_pages[0].pool.physical_slot, PhysicalSlotId(40U));
	EXPECT_EQ(result.new_pool_pages[1].pool.physical_slot, PhysicalSlotId(41U));
	EXPECT_NE(result.new_pool_pages[0].pool.physical_slot, result.new_pool_pages[1].pool.physical_slot);
	const MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(store.readNode(current, 1001U), nodePool.node_insertions.front().record);
	EXPECT_EQ(store.readEdge(current, 2001U), edgePool.edge_insertions.front().record);
	EXPECT_EQ(metadata->findCatalog(current, SegmentId(31U)).physical_slot, PhysicalSlotId(40U));
	EXPECT_EQ(metadata->findCatalog(current, SegmentId(32U)).physical_slot, PhysicalSlotId(41U));
}

TEST_F(RelocatableEntityPageStoreTest, BatchTransactionPacksSeveralTypedInitialPagesIntoOneFreshPool) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore store(io, *metadata);
	MetadataSnapshotPin before = metadata->openSnapshot();
	EntityPageBatchNewPool nodePage{.request = newPool(31U, 901U, 101U, 40U),
									.record_kind = EntityPageRecordKind::EPRK_NODE,
									.node_insertions = {{1001U, node(0U, 0U)}}};
	EntityPageBatchNewPool edgePage{.request = newPool(31U, 901U, 201U, 40U),
									.record_kind = EntityPageRecordKind::EPRK_EDGE,
									.edge_insertions = {{2001U, edge(1001U, 1002U, 51U)}}};
	const std::array newPools{nodePage, edgePage};

	const EntityPageBatchResult result = store.applyBatchTransaction(before, {}, {}, newPools, LogSequenceNumber(2U));
	ASSERT_EQ(result.new_pool_pages.size(), 2U);
	EXPECT_EQ(result.new_pool_pages[0].pool.segment_id, SegmentId(31U));
	EXPECT_EQ(result.new_pool_pages[1].pool.segment_id, SegmentId(31U));
	EXPECT_EQ(result.new_pool_pages[0].pool.physical_slot, PhysicalSlotId(40U));
	EXPECT_EQ(result.new_pool_pages[1].pool.physical_slot, PhysicalSlotId(40U));
	EXPECT_NE(result.new_pool_pages[0].page.page_index, result.new_pool_pages[1].page.page_index);

	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(store.currentPools(current).size(), 1U);
	EXPECT_EQ(store.readNode(current, 1001U), nodePage.node_insertions.front().record);
	EXPECT_EQ(store.readEdge(current, 2001U), edgePage.edge_insertions.front().record);
	EXPECT_EQ(metadata->findPageLocation(current, PageId(101U)), result.new_pool_pages[0].page);
	EXPECT_EQ(metadata->findPageLocation(current, PageId(201U)), result.new_pool_pages[1].page);
	before.release();
	current.release();

	metadata.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	EntityPageStore reopenedStore(io, *reopened);
	const MetadataSnapshotPin reopenedSnapshot = reopened->openSnapshot();
	EXPECT_EQ(reopenedStore.currentPools(reopenedSnapshot).size(), 1U);
	EXPECT_EQ(reopenedStore.readNode(reopenedSnapshot, 1001U), nodePage.node_insertions.front().record);
	EXPECT_EQ(reopenedStore.readEdge(reopenedSnapshot, 2001U), edgePage.edge_insertions.front().record);
}

TEST_F(RelocatableEntityPageStoreTest, BatchTransactionRecoversAllPackedFreshPoolFramesAfterPublicationFailure) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore store(io, *metadata);
	MetadataSnapshotPin before = metadata->openSnapshot();
	EntityPageBatchNewPool nodePage{.request = newPool(31U, 901U, 101U, 40U),
									.record_kind = EntityPageRecordKind::EPRK_NODE,
									.node_insertions = {{1001U, node(0U, 0U)}}};
	EntityPageBatchNewPool edgePage{.request = newPool(31U, 901U, 201U, 40U),
									.record_kind = EntityPageRecordKind::EPRK_EDGE,
									.edge_insertions = {{2001U, edge(1001U, 1002U, 51U)}}};
	const std::array newPools{nodePage, edgePage};
	MetadataPublishOptions interrupted;
	interrupted.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE;
	EXPECT_THROW(static_cast<void>(
						 store.applyBatchTransaction(before, {}, {}, newPools, LogSequenceNumber(2U), interrupted)),
				 BootstrapPublishInterrupted);
	MetadataSnapshotPin unchanged = metadata->openSnapshot();
	EXPECT_EQ(unchanged.superblock(), before.superblock());
	EXPECT_TRUE(store.currentPools(unchanged).empty());
	EXPECT_FALSE(metadata->isPhysicalSlotAllocated(unchanged, PhysicalSlotId(40U)));
	EXPECT_FALSE(metadata->findPageLocationOptional(unchanged, PageId(101U)).has_value());
	EXPECT_FALSE(metadata->findPageLocationOptional(unchanged, PageId(201U)).has_value());
	EXPECT_FALSE(metadata->findNodePageLocationOptional(unchanged, 1001U).has_value());
	EXPECT_FALSE(metadata->findEdgePageLocationOptional(unchanged, 2001U).has_value());

	const EntityPageBatchResult retried =
			store.applyBatchTransaction(unchanged, {}, {}, newPools, LogSequenceNumber(3U));
	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(current.superblock(), retried.superblock);
	EXPECT_EQ(store.currentPools(current).size(), 1U);
	EXPECT_EQ(store.readNode(current, 1001U), nodePage.node_insertions.front().record);
	EXPECT_EQ(store.readEdge(current, 2001U), edgePage.edge_insertions.front().record);
}

TEST_F(RelocatableEntityPageStoreTest, BatchTransactionRejectsConflictingFreshPoolIdentities) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore store(io, *metadata);
	const MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EntityPageBatchNewPool nodePage{.request = newPool(31U, 901U, 101U, 40U),
									.record_kind = EntityPageRecordKind::EPRK_NODE,
									.node_insertions = {{1001U, node(0U, 0U)}}};
	EntityPageBatchNewPool edgePage{.request = newPool(31U, 902U, 201U, 40U),
									.record_kind = EntityPageRecordKind::EPRK_EDGE,
									.edge_insertions = {{2001U, edge(1001U, 1002U, 51U)}}};
	const std::array newPools{nodePage, edgePage};
	expectFormatError(
			[&] { static_cast<void>(store.applyBatchTransaction(snapshot, {}, {}, newPools, LogSequenceNumber(2U))); },
			FormatError::FERR_INVALID_STATE);
	EXPECT_TRUE(store.currentPools(snapshot).empty());
	EXPECT_FALSE(metadata->findPageLocationOptional(snapshot, PageId(101U)).has_value());
	EXPECT_FALSE(metadata->findPageLocationOptional(snapshot, PageId(201U)).has_value());
}

TEST_F(RelocatableEntityPageStoreTest, BatchTransactionNewPoolSupportsNodesAndRejectsMalformedRequests) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore store(io, *metadata);
	const std::optional<EntityPageInsertResult> nodePage =
			store.allocateNode(newPool(31U, 901U, 101U, 40U), 1001U, node(0U, 0U), LogSequenceNumber(2U));
	const std::optional<EntityPageInsertResult> edgePage =
			store.allocateEdge(newPool(35U, 905U, 201U, 45U), 2001U, edge(1001U, 1002U, 21U), LogSequenceNumber(3U));
	ASSERT_TRUE(nodePage.has_value());
	ASSERT_TRUE(edgePage.has_value());
	const MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const std::array<EntityPageBatchNewPool, 1U> valid{{
			{.request = newPool(32U, 902U, 102U, 41U),
			 .record_kind = EntityPageRecordKind::EPRK_NODE,
			 .node_insertions = {{1003U, node(31U, 32U)}, {1002U, node(21U, 22U)}}},
	}};
	const EntityPageBatchResult inserted = store.applyBatchTransaction(snapshot, {}, {}, valid, LogSequenceNumber(4U));
	ASSERT_EQ(inserted.new_pool_pages.size(), 1U);
	ASSERT_EQ(inserted.new_pool_pages.front().locations.size(), 2U);
	EXPECT_EQ(inserted.new_pool_pages.front().locations[0], (EntityPageLocation{1002U, PageId(102U), 1U, 1U}));
	EXPECT_EQ(inserted.new_pool_pages.front().locations[1], (EntityPageLocation{1003U, PageId(102U), 0U, 1U}));
	const MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(store.readNode(current, 1002U), valid.front().node_insertions[1].record);
	EXPECT_EQ(store.readNode(current, 1003U), valid.front().node_insertions[0].record);

	const auto expectNewPoolError = [&](const EntityPageBatchNewPool &newPoolRequest, FormatError expected) {
		expectFormatError(
				[&] {
					static_cast<void>(store.applyBatchTransaction(current, {}, {}, std::span(&newPoolRequest, 1),
																  LogSequenceNumber(5U)));
				},
				expected);
	};
	EntityPageBatchNewPool invalidPage{.request = newPool(33U, 903U, 0U, 42U),
									   .record_kind = EntityPageRecordKind::EPRK_NODE,
									   .node_insertions = {{1004U, node(41U, 42U)}}};
	expectNewPoolError(invalidPage, FormatError::FERR_INVALID_HEADER);
	EntityPageBatchNewPool invalidKind{.request = newPool(33U, 903U, 103U, 42U),
									   .record_kind = static_cast<EntityPageRecordKind>(77U),
									   .node_insertions = {{1004U, node(41U, 42U)}}};
	expectNewPoolError(invalidKind, FormatError::FERR_INVALID_HEADER);
	EntityPageBatchNewPool emptyNode{.request = newPool(33U, 903U, 103U, 42U),
									 .record_kind = EntityPageRecordKind::EPRK_NODE};
	expectNewPoolError(emptyNode, FormatError::FERR_INVALID_HEADER);
	EntityPageBatchNewPool mixedKinds{.request = newPool(33U, 903U, 103U, 42U),
									  .record_kind = EntityPageRecordKind::EPRK_NODE,
									  .node_insertions = {{1004U, node(41U, 42U)}},
									  .edge_insertions = {{2002U, edge(1001U, 1002U, 22U)}}};
	expectNewPoolError(mixedKinds, FormatError::FERR_INVALID_HEADER);
	EntityPageBatchNewPool zeroNode{.request = newPool(33U, 903U, 103U, 42U),
									.record_kind = EntityPageRecordKind::EPRK_NODE,
									.node_insertions = {{1004U, node(41U, 42U)}, {0U, node(51U, 52U)}}};
	expectNewPoolError(zeroNode, FormatError::FERR_INVALID_HEADER);
	EntityPageBatchNewPool duplicateNode{.request = newPool(33U, 903U, 103U, 42U),
										 .record_kind = EntityPageRecordKind::EPRK_NODE,
										 .node_insertions = {{1004U, node(41U, 42U)}, {1004U, node(51U, 52U)}}};
	expectNewPoolError(duplicateNode, FormatError::FERR_INVALID_HEADER);
	EntityPageBatchNewPool publishedNode{.request = newPool(33U, 903U, 103U, 42U),
										 .record_kind = EntityPageRecordKind::EPRK_NODE,
										 .node_insertions = {{1001U, node(41U, 42U)}}};
	expectNewPoolError(publishedNode, FormatError::FERR_INVALID_STATE);
	EntityPageBatchNewPool zeroEdge{
			.request = newPool(33U, 903U, 203U, 42U),
			.record_kind = EntityPageRecordKind::EPRK_EDGE,
			.edge_insertions = {{2002U, edge(1001U, 1002U, 22U)}, {0U, edge(1002U, 1001U, 23U)}}};
	expectNewPoolError(zeroEdge, FormatError::FERR_INVALID_HEADER);
	EntityPageBatchNewPool duplicateEdge{
			.request = newPool(33U, 903U, 203U, 42U),
			.record_kind = EntityPageRecordKind::EPRK_EDGE,
			.edge_insertions = {{2002U, edge(1001U, 1002U, 22U)}, {2002U, edge(1002U, 1001U, 23U)}}};
	expectNewPoolError(duplicateEdge, FormatError::FERR_INVALID_HEADER);
	EntityPageBatchNewPool publishedEdge{.request = newPool(33U, 903U, 203U, 42U),
										 .record_kind = EntityPageRecordKind::EPRK_EDGE,
										 .edge_insertions = {{2001U, edge(1001U, 1002U, 22U)}}};
	expectNewPoolError(publishedEdge, FormatError::FERR_INVALID_STATE);
	EntityPageBatchNewPool invalidPool{.request = newPool(33U, 0U, 103U, 42U),
									   .record_kind = EntityPageRecordKind::EPRK_NODE,
									   .node_insertions = {{1004U, node(41U, 42U)}}};
	expectNewPoolError(invalidPool, FormatError::FERR_INVALID_HEADER);
	const std::array<EntityPageBatchNewPool, 2U> duplicatePage{{
			{.request = newPool(33U, 903U, 103U, 42U),
			 .record_kind = EntityPageRecordKind::EPRK_NODE,
			 .node_insertions = {{1004U, node(41U, 42U)}}},
			{.request = newPool(34U, 904U, 103U, 43U),
			 .record_kind = EntityPageRecordKind::EPRK_NODE,
			 .node_insertions = {{1005U, node(51U, 52U)}}},
	}};
	expectFormatError(
			[&] {
				static_cast<void>(store.applyBatchTransaction(current, {}, {}, duplicatePage, LogSequenceNumber(5U)));
			},
			FormatError::FERR_INVALID_HEADER);
	EXPECT_FALSE(metadata->findPageLocationOptional(current, PageId(103U)).has_value());
	EXPECT_FALSE(metadata->findNodePageLocationOptional(current, 1004U).has_value());
}
