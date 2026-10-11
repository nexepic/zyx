#include <array>
#include "graph/storage/relocatable/schema/SystemCatalog.hpp"
#include "storage/relocatable/metadata/RelocatableMetadataStoreTestFixture.hpp"

TEST_F(RelocatableMetadataStoreTest, RejectsUnregisteredLogicalMembershipDeferredRootWithoutPublishingScratch) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	MetadataSnapshotPin before = store->openSnapshot();
	const Superblock beforeSuperblock = before.superblock();
	before.release();

	NativeGraphDerivedStateMutation derived;
	NativeGraphPrivateSegmentRequest scratch;
	scratch.kind = SegmentKind::SGK_BULK_SCRATCH;
	scratch.owner_id = 78U;
	scratch.first_slot = PhysicalSlotId(40U);
	scratch.last_slot = PhysicalSlotId(40U);
	scratch.payload.assign(43U, std::byte{0x4E});
	derived.private_segments.push_back(std::move(scratch));
	const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
	const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
	const std::array<EntityPageNewPoolBatchRequest, 0U> noEntityPools{};
	const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
	const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyBatches{};
	const std::array<NativeGraphLargeValueMutation, 0U> noLargeValues{};
	bool plannerCalled = false;
	bool preparerCalled = false;

	expectFormatError(
			[&] {
				static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
						noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyBatches,
						noLargeValues, derived, LogSequenceNumber(2U),
						[&preparerCalled](std::span<EntityPageReplacementMaterialization>,
										  std::span<EntityPageAppendBatchMaterialization>,
										  std::span<EntityPageNewPoolBatchMaterialization>,
										  std::span<NativeGraphPropertyPageReplacementMaterialization>,
										  std::span<NativeGraphPropertyPageBatchMaterialization>,
										  std::span<LargeValueMaterializationResult>) { preparerCalled = true; },
						[](std::vector<std::vector<IndexTreeMutationPoolBatch>> &) {}, {}, {}, {}, {}, {},
						MetadataPublishOptions{}, NativeGraphLogicalIdReservation{}, std::nullopt, {}, {},
						[&plannerCalled](const NativeGraphDeferredIndexPlanningContext &) {
							plannerCalled = true;
							NativeGraphDeferredIndexPlan plan;
							plan.mutation.index_id = RSS_SYSTEM_CATALOG_INDEX_ID_FIRST;
							plan.mutation.new_pool_requests.push_back(
									{PhysicalSlotId(40U), PhysicalSlotId(40U), RSS_SYSTEM_CATALOG_INDEX_ID_FIRST, 1U});
							plan.mutation.replacement_root = {0U, 0U, true};
							plan.mutation.expected_root =
									IndexRootLocation{RSS_SYSTEM_CATALOG_INDEX_ID_FIRST, PageId(1U), Generation(1U)};
							plan.preparer = [](std::vector<IndexTreeMutationPoolBatch> &) {};
							return plan;
						}));
			},
			FormatError::FERR_INVALID_STATE);

	EXPECT_TRUE(plannerCalled);
	EXPECT_FALSE(preparerCalled);
	MetadataSnapshotPin unchanged = store->openSnapshot();
	EXPECT_EQ(unchanged.superblock(), beforeSuperblock);
	EXPECT_FALSE(store->isPhysicalSlotAllocated(unchanged, PhysicalSlotId(40U)));
	EXPECT_THROW(static_cast<void>(store->findCatalog(unchanged, SegmentId(beforeSuperblock.next_logical_segment_id))),
				 FormatException);
	unchanged.release();

	store.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin recovered = reopened->openSnapshot();
	EXPECT_EQ(recovered.superblock(), beforeSuperblock);
	EXPECT_FALSE(reopened->isPhysicalSlotAllocated(recovered, PhysicalSlotId(40U)));
	EXPECT_THROW(
			static_cast<void>(reopened->findCatalog(recovered, SegmentId(beforeSuperblock.next_logical_segment_id))),
			FormatException);
	recovered.release();
}
