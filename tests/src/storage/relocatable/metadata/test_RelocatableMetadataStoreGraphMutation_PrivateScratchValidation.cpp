/**
 * @file test_RelocatableMetadataStoreGraphMutation_PrivateScratchValidation.cpp
 * @brief Input-contract tests for legacy private scratch candidates.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "storage/relocatable/metadata/RelocatableMetadataStoreTestFixture.hpp"

#include "graph/storage/relocatable/vector/VectorMappingIds.hpp"

#include <array>
#include <limits>
#include <vector>

TEST_F(RelocatableMetadataStoreTest, RejectsMismatchedLegacyPrivateScratchHeadersBeforeMaterialization) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
	const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
	const std::array<EntityPageNewPoolBatchRequest, 0U> noEntityPools{};
	const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
	const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyBatches{};
	const std::array<NativeGraphLargeValueMutation, 0U> noLargeValues{};
	MetadataSnapshotPin before = store->openSnapshot();
	const Superblock beforeSuperblock = before.superblock();
	before.release();

	const auto privateCandidate = [] {
		NativeGraphPrivateSegmentRequest request;
		request.kind = SegmentKind::SGK_BULK_SCRATCH;
		request.owner_id = 71U;
		request.first_slot = PhysicalSlotId(40U);
		request.last_slot = PhysicalSlotId(42U);
		request.payload = {std::byte{0x5AU}};
		return request;
	};
	const auto rejectsBeforeMaterialization = [&](NativeGraphDerivedStateMutation derived) {
		bool graphPreparerCalled = false;
		bool derivedPreparerCalled = false;
		expectFormatError(
				[&] {
					static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
							noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements,
							noPropertyBatches, noLargeValues, derived, LogSequenceNumber(2U),
							[&](std::span<EntityPageReplacementMaterialization>,
								std::span<EntityPageAppendBatchMaterialization>,
								std::span<EntityPageNewPoolBatchMaterialization>,
								std::span<NativeGraphPropertyPageReplacementMaterialization>,
								std::span<NativeGraphPropertyPageBatchMaterialization>,
								std::span<LargeValueMaterializationResult>) { graphPreparerCalled = true; },
							[&](std::vector<std::vector<IndexTreeMutationPoolBatch>> &) {
								derivedPreparerCalled = true;
							}));
				},
				FormatError::FERR_INVALID_HEADER);
		EXPECT_FALSE(graphPreparerCalled);
		EXPECT_FALSE(derivedPreparerCalled);
		MetadataSnapshotPin unchanged = store->openSnapshot();
		EXPECT_EQ(unchanged.superblock(), beforeSuperblock);
		unchanged.release();
	};

	// Both candidates are individually valid. Their incompatible header extensions
	// must still be rejected before a planner can observe mixed scratch images.
	{
		NativeGraphDerivedStateMutation derived;
		NativeGraphPrivateSegmentRequest first = privateCandidate();
		first.header_bytes = RSS_ENVELOPE_BYTES + sizeof(uint64_t);
		first.header_extension.assign(sizeof(uint64_t), std::byte{0x11});
		NativeGraphPrivateSegmentRequest second = first;
		second.header_extension.assign(sizeof(uint64_t), std::byte{0x22});
		derived.private_segments = {std::move(first), std::move(second)};
		rejectsBeforeMaterialization(std::move(derived));
	}
	{
		NativeGraphDerivedStateMutation derived;
		NativeGraphPrivateSegmentRequest first = privateCandidate();
		first.header_bytes = RSS_ENVELOPE_BYTES + sizeof(uint64_t);
		first.header_extension.assign(sizeof(uint64_t), std::byte{0x33});
		NativeGraphPrivateSegmentRequest second = first;
		second.header_bytes += sizeof(uint64_t);
		second.header_extension.assign(second.header_bytes - RSS_ENVELOPE_BYTES, std::byte{0x33});
		derived.private_segments = {std::move(first), std::move(second)};
		rejectsBeforeMaterialization(std::move(derived));
	}
}

TEST_F(RelocatableMetadataStoreTest, RejectsEveryMalformedPrivateScratchRequestBeforePublication) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
	const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
	const std::array<EntityPageNewPoolBatchRequest, 0U> noEntityPools{};
	const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
	const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyBatches{};
	const std::array<NativeGraphLargeValueMutation, 0U> noLargeValues{};

	const auto validRequest = [] {
		NativeGraphPrivateSegmentRequest request;
		request.kind = SegmentKind::SGK_BULK_SCRATCH;
		request.owner_id = 71U;
		request.first_slot = PhysicalSlotId(40U);
		request.last_slot = PhysicalSlotId(40U);
		request.payload = {std::byte{0x5AU}};
		return request;
	};
	const auto expectRejected = [&](NativeGraphPrivateSegmentRequest request) {
		NativeGraphDerivedStateMutation derived;
		derived.private_segments.push_back(std::move(request));
		expectFormatError(
				[&] {
					static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
							noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements,
							noPropertyBatches, noLargeValues, derived, LogSequenceNumber(2U),
							[](std::span<EntityPageReplacementMaterialization>,
							   std::span<EntityPageAppendBatchMaterialization>,
							   std::span<EntityPageNewPoolBatchMaterialization>,
							   std::span<NativeGraphPropertyPageReplacementMaterialization>,
							std::span<NativeGraphPropertyPageBatchMaterialization>,
							std::span<LargeValueMaterializationResult>) {},
							[](std::vector<std::vector<IndexTreeMutationPoolBatch>> &) {}));
				},
				FormatError::FERR_INVALID_HEADER);
		MetadataSnapshotPin unchanged = store->openSnapshot();
		EXPECT_EQ(unchanged.superblock().generation, Generation(1U));
		EXPECT_FALSE(store->isPhysicalSlotAllocated(unchanged, PhysicalSlotId(40U)));
		unchanged.release();
	};

	{
		auto request = validRequest();
		request.kind = SegmentKind::SGK_NONE;
		expectRejected(std::move(request));
	}
	{
		auto request = validRequest();
		request.owner_id = 0U;
		expectRejected(std::move(request));
	}
	{
		auto request = validRequest();
		request.first_slot = {};
		expectRejected(std::move(request));
	}
	{
		auto request = validRequest();
		request.last_slot = {};
		expectRejected(std::move(request));
	}
	{
		auto request = validRequest();
		request.first_slot = PhysicalSlotId(41U);
		expectRejected(std::move(request));
	}
	{
		auto request = validRequest();
		request.header_bytes = 0U;
		expectRejected(std::move(request));
	}
	{
		auto request = validRequest();
		request.header_bytes = static_cast<uint16_t>(RSS_MAX_HEADER_BYTES + alignof(uint64_t));
		expectRejected(std::move(request));
	}
	{
		auto request = validRequest();
		request.header_bytes = static_cast<uint16_t>(RSS_ENVELOPE_BYTES + 1U);
		expectRejected(std::move(request));
	}
	{
		auto request = validRequest();
		request.header_bytes = static_cast<uint16_t>(RSS_ENVELOPE_BYTES + alignof(uint64_t));
		expectRejected(std::move(request));
	}
	{
		auto request = validRequest();
		request.payload.clear();
		expectRejected(std::move(request));
	}
	{
		auto request = validRequest();
		request.payload.assign(RSS_SEGMENT_SIZE - request.header_bytes + 1U, std::byte{0x5AU});
		expectRejected(std::move(request));
	}
}

TEST_F(RelocatableMetadataStoreTest, RejectsInhomogeneousAndOvercommittedPrivateScratchCandidatesBeforePublication) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
	const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
	const std::array<EntityPageNewPoolBatchRequest, 0U> noEntityPools{};
	const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
	const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyBatches{};
	const std::array<NativeGraphLargeValueMutation, 0U> noLargeValues{};
	const auto candidate = [] {
		NativeGraphPrivateSegmentRequest request;
		request.kind = SegmentKind::SGK_BULK_SCRATCH;
		request.owner_id = 71U;
		request.first_slot = PhysicalSlotId(40U);
		request.last_slot = PhysicalSlotId(40U);
		request.payload = {std::byte{0x5AU}};
		return request;
	};
	const auto expectRejected = [&](std::vector<NativeGraphPrivateSegmentRequest> requests) {
		NativeGraphDerivedStateMutation derived;
		derived.private_segments = std::move(requests);
		expectFormatError(
				[&] {
					static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
							noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements,
							noPropertyBatches, noLargeValues, derived, LogSequenceNumber(2U),
							[](std::span<EntityPageReplacementMaterialization>,
							   std::span<EntityPageAppendBatchMaterialization>,
							   std::span<EntityPageNewPoolBatchMaterialization>,
							   std::span<NativeGraphPropertyPageReplacementMaterialization>,
							   std::span<NativeGraphPropertyPageBatchMaterialization>,
							   std::span<LargeValueMaterializationResult>) {},
							[](std::vector<std::vector<IndexTreeMutationPoolBatch>> &) {}));
				},
				FormatError::FERR_INVALID_HEADER);
		MetadataSnapshotPin unchanged = store->openSnapshot();
		EXPECT_EQ(unchanged.superblock().generation, Generation(1U));
		unchanged.release();
	};

	{
		auto first = candidate();
		auto second = candidate();
		second.owner_id = 72U;
		expectRejected({std::move(first), std::move(second)});
	}
	{
		auto first = candidate();
		auto second = candidate();
		second.header_bytes = static_cast<uint16_t>(RSS_ENVELOPE_BYTES + alignof(uint64_t));
		second.header_extension.assign(alignof(uint64_t), std::byte{0x7AU});
		expectRejected({std::move(first), std::move(second)});
	}
	{
		auto first = candidate();
		auto second = candidate();
		expectRejected({std::move(first), std::move(second)});
	}
}

TEST_F(RelocatableMetadataStoreTest, RejectsMalformedCompactPrivateScratchPlansBeforePublication) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
	const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
	const std::array<EntityPageNewPoolBatchRequest, 0U> noEntityPools{};
	const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
	const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyBatches{};
	const std::array<NativeGraphLargeValueMutation, 0U> noLargeValues{};
	const auto validPlan = [] {
		NativeGraphPrivateSegmentPlan plan;
		plan.kind = SegmentKind::SGK_BULK_SCRATCH;
		plan.owner_id = 71U;
		plan.first_slot = PhysicalSlotId(40U);
		plan.last_slot = PhysicalSlotId(41U);
		plan.segment_count = 1U;
		return plan;
	};
	const auto expectRejected = [&](NativeGraphDerivedStateMutation derived) {
		expectFormatError(
				[&] {
					static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
							noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements,
							noPropertyBatches, noLargeValues, derived, LogSequenceNumber(2U),
							[](std::span<EntityPageReplacementMaterialization>,
							   std::span<EntityPageAppendBatchMaterialization>,
							   std::span<EntityPageNewPoolBatchMaterialization>,
							   std::span<NativeGraphPropertyPageReplacementMaterialization>,
							   std::span<NativeGraphPropertyPageBatchMaterialization>,
							   std::span<LargeValueMaterializationResult>) {},
							[](std::vector<std::vector<IndexTreeMutationPoolBatch>> &) {}, {}, {},
							std::span<const PageRetirement>{}, std::span<const LargeValueId>{}, {},
							MetadataPublishOptions{}, NativeGraphLogicalIdReservation{}, std::nullopt,
							[](const NativeGraphPrivateSegmentSet &, size_t) {
								return NativeGraphPrivateSegmentReservation::payload();
							}));
				},
				FormatError::FERR_INVALID_HEADER);
		MetadataSnapshotPin unchanged = store->openSnapshot();
		EXPECT_EQ(unchanged.superblock().generation, Generation(1U));
		unchanged.release();
	};

	{
		NativeGraphDerivedStateMutation derived;
		auto plan = validPlan();
		plan.kind = SegmentKind::SGK_NONE;
		derived.private_segment_plan = std::move(plan);
		expectRejected(std::move(derived));
	}
	{
		NativeGraphDerivedStateMutation derived;
		auto plan = validPlan();
		plan.segment_count = 0U;
		derived.private_segment_plan = std::move(plan);
		expectRejected(std::move(derived));
	}
	{
		NativeGraphDerivedStateMutation derived;
		auto plan = validPlan();
		plan.segment_count = std::numeric_limits<size_t>::max();
		derived.private_segment_plan = std::move(plan);
		expectRejected(std::move(derived));
	}
	{
		NativeGraphDerivedStateMutation derived;
		auto plan = validPlan();
		plan.segment_count = 3U;
		derived.private_segment_plan = std::move(plan);
		expectRejected(std::move(derived));
	}
	{
		NativeGraphDerivedStateMutation derived;
		derived.private_segment_plan = validPlan();
		NativeGraphPrivateSegmentRequest request;
		request.kind = SegmentKind::SGK_BULK_SCRATCH;
		request.owner_id = 71U;
		request.first_slot = PhysicalSlotId(40U);
		request.last_slot = PhysicalSlotId(40U);
		request.payload = {std::byte{0x5AU}};
		derived.private_segments.push_back(std::move(request));
		expectRejected(std::move(derived));
	}
}

TEST_F(RelocatableMetadataStoreTest, RejectsMalformedDerivedIndexDefinitionsBeforeAnyMaterializer) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
	const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
	const std::array<EntityPageNewPoolBatchRequest, 0U> noEntityPools{};
	const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
	const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyBatches{};
	const std::array<NativeGraphLargeValueMutation, 0U> noLargeValues{};
	MetadataSnapshotPin before = store->openSnapshot();
	const Superblock beforeSuperblock = before.superblock();
	before.release();

	const auto freshRoot = [] {
		NativeGraphIndexTreeMutation mutation;
		mutation.index_id = 77U;
		mutation.new_pool_requests.push_back({PhysicalSlotId(40U), PhysicalSlotId(40U), mutation.index_id, 1U});
		mutation.replacement_root = NativeGraphIndexTreeAppendedPageReference{0U, 0U, true};
		return mutation;
	};
	const auto pinnedRoot = [] {
		NativeGraphIndexTreeMutation mutation;
		mutation.index_id = 77U;
		mutation.expected_root = {mutation.index_id, PageId(9U), Generation(1U)};
		mutation.pool_requests.push_back({SegmentId(21U), {}, 1U, {}});
		mutation.replacement_root = NativeGraphIndexTreeAppendedPageReference{0U, 0U, false};
		return mutation;
	};
	const auto retiredRoot = [] {
		NativeGraphIndexTreeMutation mutation;
		mutation.index_id = 77U;
		mutation.expected_root = {mutation.index_id, PageId(9U), Generation(1U)};
		const PageLocation rootLocation{
				PageId(9U), SegmentId(21U), RSS_PAGE_POOL_FIRST_DATA_FRAME, Generation(1U), 0U};
		mutation.pool_requests.push_back({SegmentId(21U), {}, 0U, {{rootLocation, LogSequenceNumber(1U)}}});
		mutation.retire_root = true;
		return mutation;
	};
	const auto rejectsBeforeMaterialization = [&](NativeGraphDerivedStateMutation derived) {
		bool graphPreparerCalled = false;
		bool derivedPreparerCalled = false;
		expectFormatError(
				[&] {
					static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
							noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements,
							noPropertyBatches, noLargeValues, derived, LogSequenceNumber(2U),
							[&](std::span<EntityPageReplacementMaterialization>,
								std::span<EntityPageAppendBatchMaterialization>,
								std::span<EntityPageNewPoolBatchMaterialization>,
								std::span<NativeGraphPropertyPageReplacementMaterialization>,
								std::span<NativeGraphPropertyPageBatchMaterialization>,
								std::span<LargeValueMaterializationResult>) { graphPreparerCalled = true; },
							[&](std::vector<std::vector<IndexTreeMutationPoolBatch>> &) {
								derivedPreparerCalled = true;
							}));
				},
				FormatError::FERR_INVALID_HEADER);
		EXPECT_FALSE(graphPreparerCalled);
		EXPECT_FALSE(derivedPreparerCalled);
		MetadataSnapshotPin unchanged = store->openSnapshot();
		EXPECT_EQ(unchanged.superblock(), beforeSuperblock);
		unchanged.release();
	};

	// These are public coordinator inputs. Every malformed root/pool shape must
	// fail before it can reserve a page-pool slot or invoke a typed writer.
	{
		NativeGraphDerivedStateMutation derived;
		auto mutation = freshRoot();
		mutation.index_id = 0U;
		derived.index_tree_mutations.push_back(std::move(mutation));
		rejectsBeforeMaterialization(std::move(derived));
	}
	{
		NativeGraphDerivedStateMutation derived;
		derived.index_tree_mutations = {freshRoot(), freshRoot()};
		rejectsBeforeMaterialization(std::move(derived));
	}
	{
		NativeGraphDerivedStateMutation derived;
		auto mutation = freshRoot();
		mutation.expected_root = {78U, PageId(9U), Generation(1U)};
		derived.index_tree_mutations.push_back(std::move(mutation));
		rejectsBeforeMaterialization(std::move(derived));
	}
	{
		NativeGraphDerivedStateMutation derived;
		auto mutation = freshRoot();
		mutation.expected_root = {mutation.index_id, {}, Generation(1U)};
		derived.index_tree_mutations.push_back(std::move(mutation));
		rejectsBeforeMaterialization(std::move(derived));
	}
	{
		NativeGraphDerivedStateMutation derived;
		auto mutation = freshRoot();
		mutation.replacement_root.reset();
		derived.index_tree_mutations.push_back(std::move(mutation));
		rejectsBeforeMaterialization(std::move(derived));
	}
	{
		NativeGraphDerivedStateMutation derived;
		auto mutation = freshRoot();
		mutation.replacement_root->newly_reserved = false;
		derived.index_tree_mutations.push_back(std::move(mutation));
		rejectsBeforeMaterialization(std::move(derived));
	}
	{
		NativeGraphDerivedStateMutation derived;
		auto mutation = freshRoot();
		mutation.stable_replacement_root = PageId(9U);
		derived.index_tree_mutations.push_back(std::move(mutation));
		rejectsBeforeMaterialization(std::move(derived));
	}
	{
		NativeGraphDerivedStateMutation derived;
		auto mutation = freshRoot();
		mutation.new_pool_requests.front().first_slot = {};
		derived.index_tree_mutations.push_back(std::move(mutation));
		rejectsBeforeMaterialization(std::move(derived));
	}
	{
		NativeGraphDerivedStateMutation derived;
		auto mutation = freshRoot();
		mutation.new_pool_requests.front().appended_page_count = 0U;
		derived.index_tree_mutations.push_back(std::move(mutation));
		rejectsBeforeMaterialization(std::move(derived));
	}
	{
		NativeGraphDerivedStateMutation derived;
		auto mutation = freshRoot();
		mutation.replacement_root->appended_page_ordinal = 1U;
		derived.index_tree_mutations.push_back(std::move(mutation));
		rejectsBeforeMaterialization(std::move(derived));
	}
	{
		NativeGraphDerivedStateMutation derived;
		auto mutation = pinnedRoot();
		mutation.pool_requests.front().segment_id = {};
		derived.index_tree_mutations.push_back(std::move(mutation));
		rejectsBeforeMaterialization(std::move(derived));
	}
	{
		NativeGraphDerivedStateMutation derived;
		auto mutation = pinnedRoot();
		mutation.pool_requests.push_back(mutation.pool_requests.front());
		derived.index_tree_mutations.push_back(std::move(mutation));
		rejectsBeforeMaterialization(std::move(derived));
	}
	{
		NativeGraphDerivedStateMutation derived;
		auto mutation = pinnedRoot();
		mutation.pool_requests.front().expected_replacements.push_back({});
		derived.index_tree_mutations.push_back(std::move(mutation));
		rejectsBeforeMaterialization(std::move(derived));
	}
	{
		NativeGraphDerivedStateMutation derived;
		auto mutation = pinnedRoot();
		mutation.pool_requests.front().expected_retirements.push_back({{}, LogSequenceNumber{}});
		derived.index_tree_mutations.push_back(std::move(mutation));
		rejectsBeforeMaterialization(std::move(derived));
	}

	// A root retirement is a public, immutable publication request. Its retired
	// root must be pinned and it cannot mix retirement with replacement work.
	{
		NativeGraphDerivedStateMutation derived;
		auto mutation = retiredRoot();
		mutation.expected_root.reset();
		derived.index_tree_mutations.push_back(std::move(mutation));
		rejectsBeforeMaterialization(std::move(derived));
	}
	{
		NativeGraphDerivedStateMutation derived;
		auto mutation = retiredRoot();
		mutation.new_pool_requests.push_back({PhysicalSlotId(40U), PhysicalSlotId(40U), mutation.index_id, 1U});
		derived.index_tree_mutations.push_back(std::move(mutation));
		rejectsBeforeMaterialization(std::move(derived));
	}
	{
		NativeGraphDerivedStateMutation derived;
		auto mutation = retiredRoot();
		mutation.replacement_root = NativeGraphIndexTreeAppendedPageReference{0U, 0U, false};
		derived.index_tree_mutations.push_back(std::move(mutation));
		rejectsBeforeMaterialization(std::move(derived));
	}
	{
		NativeGraphDerivedStateMutation derived;
		auto mutation = retiredRoot();
		mutation.stable_replacement_root = PageId(10U);
		derived.index_tree_mutations.push_back(std::move(mutation));
		rejectsBeforeMaterialization(std::move(derived));
	}
	{
		NativeGraphDerivedStateMutation derived;
		auto mutation = retiredRoot();
		mutation.pool_requests.clear();
		derived.index_tree_mutations.push_back(std::move(mutation));
		rejectsBeforeMaterialization(std::move(derived));
	}
	{
		NativeGraphDerivedStateMutation derived;
		auto mutation = retiredRoot();
		mutation.pool_requests.front().expected_replacements.push_back(
				{PageId(10U), SegmentId(21U), RSS_PAGE_POOL_FIRST_DATA_FRAME, Generation(1U), 0U});
		derived.index_tree_mutations.push_back(std::move(mutation));
		rejectsBeforeMaterialization(std::move(derived));
	}
	{
		NativeGraphDerivedStateMutation derived;
		auto mutation = retiredRoot();
		mutation.pool_requests.front().appended_page_count = 1U;
		derived.index_tree_mutations.push_back(std::move(mutation));
		rejectsBeforeMaterialization(std::move(derived));
	}
	{
		NativeGraphDerivedStateMutation derived;
		auto mutation = retiredRoot();
		mutation.pool_requests.front().expected_retirements.clear();
		derived.index_tree_mutations.push_back(std::move(mutation));
		rejectsBeforeMaterialization(std::move(derived));
	}
}

TEST_F(RelocatableMetadataStoreTest, RejectsIncoherentLargeValueAndRetirementIndexInputsBeforePublication) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
	const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
	const std::array<EntityPageNewPoolBatchRequest, 0U> noEntityPools{};
	const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
	const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyBatches{};
	const std::array<NativeGraphLargeValueMutation, 0U> noLargeValues{};
	const PageRetirement retirement{{PageId(9U), SegmentId(7U), RSS_PAGE_POOL_FIRST_DATA_FRAME, Generation(1U), 0U},
			77U, PageOwnerKind::POK_LARGE_VALUE_DATA, LogSequenceNumber(1U)};
	const std::array<PageRetirement, 1U> retirements{{retirement}};
	const std::array<LargeValueId, 1U> erased{{LargeValueId(77U)}};
	const auto graphPreparer = [](std::span<EntityPageReplacementMaterialization>,
								 std::span<EntityPageAppendBatchMaterialization>,
								 std::span<EntityPageNewPoolBatchMaterialization>,
								 std::span<NativeGraphPropertyPageReplacementMaterialization>,
								 std::span<NativeGraphPropertyPageBatchMaterialization>,
								 std::span<LargeValueMaterializationResult>) {};
	const auto derivedPreparer = [](std::vector<std::vector<IndexTreeMutationPoolBatch>> &) {};
	const auto rejects = [&](NativeGraphDerivedStateMutation derived) {
		expectFormatError(
				[&] {
					static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
							noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements,
							noPropertyBatches, noLargeValues, derived, LogSequenceNumber(2U), graphPreparer,
							derivedPreparer, {}, {}, retirements, erased));
				},
				FormatError::FERR_INVALID_HEADER);
	};

	// Retirement changes the LargeValue identity namespace and therefore must
	// carry an expected content-index root, even when no new value is created.
	{
		NativeGraphDerivedStateMutation derived;
		NativeGraphIndexTreeMutation contentIndex;
		contentIndex.index_id = RSS_LARGE_VALUE_CONTENT_INDEX_ID;
		contentIndex.new_pool_requests.push_back(
				{PhysicalSlotId(40U), PhysicalSlotId(40U), contentIndex.index_id, 1U});
		contentIndex.replacement_root = NativeGraphIndexTreeAppendedPageReference{0U, 0U, true};
		derived.index_tree_mutations.push_back(std::move(contentIndex));
		rejects(std::move(derived));
	}

	// A stable replacement cannot point at a page that the same mutation retires.
	{
		NativeGraphDerivedStateMutation derived;
		NativeGraphIndexTreeMutation mutation;
		mutation.index_id = 77U;
		mutation.expected_root = {77U, PageId(10U), Generation(1U)};
		mutation.pool_requests.push_back({SegmentId(21U), {}, 0U,
				{{retirement.location, retirement.expected_owner_lsn}}});
		mutation.stable_replacement_root = retirement.location.page_id;
		derived.index_tree_mutations.push_back(std::move(mutation));
		rejects(std::move(derived));
	}

	const auto pinnedIndex = [] {
		NativeGraphIndexTreeMutation mutation;
		mutation.index_id = 77U;
		mutation.expected_root = {77U, PageId(10U), Generation(1U)};
		mutation.pool_requests.push_back({SegmentId(21U), {}, 1U, {}});
		mutation.replacement_root = NativeGraphIndexTreeAppendedPageReference{0U, 0U, false};
		return mutation;
	};
	const auto rejectIndex = [&](NativeGraphIndexTreeMutation mutation) {
		NativeGraphDerivedStateMutation derived;
		derived.index_tree_mutations.push_back(std::move(mutation));
		// These cases target the index-request header checks. Do not attach the
		// deliberately incomplete LargeValue retirement fixture above: its
		// content-index gate would reject first and mask the intended validation.
		expectFormatError(
				[&] {
					static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
							noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements,
							noPropertyBatches, noLargeValues, derived, LogSequenceNumber(2U), graphPreparer,
							derivedPreparer));
				},
				FormatError::FERR_INVALID_HEADER);
	};
	{
		auto mutation = pinnedIndex();
		mutation.expected_root->root_generation = {};
		rejectIndex(std::move(mutation));
	}
	{
		auto mutation = pinnedIndex();
		mutation.pool_requests.front().expected_replacements.push_back(
				{PageId(11U), SegmentId(21U), RSS_PAGE_POOL_FIRST_DATA_FRAME, Generation(1U), 0U});
		mutation.pool_requests.front().expected_retirements.push_back(
				{{PageId(11U), SegmentId(21U), RSS_PAGE_POOL_FIRST_DATA_FRAME, Generation(1U), 0U}, {}});
		rejectIndex(std::move(mutation));
	}
	{
		auto mutation = pinnedIndex();
		mutation.pool_requests.front().appended_page_count = RSS_PAGE_POOL_DATA_FRAME_COUNT + 1U;
		rejectIndex(std::move(mutation));
	}
	{
		auto mutation = pinnedIndex();
		mutation.new_pool_requests.push_back({PhysicalSlotId(40U), PhysicalSlotId(40U), 0U, 1U});
		rejectIndex(std::move(mutation));
	}
	{
		auto mutation = pinnedIndex();
		mutation.replacement_root->pool_ordinal = 2U;
		rejectIndex(std::move(mutation));
	}
	{
		auto mutation = pinnedIndex();
		mutation.replacement_root->appended_page_ordinal = 1U;
		rejectIndex(std::move(mutation));
	}
	{
		auto mutation = pinnedIndex();
		mutation.stable_replacement_root = PageId{};
		rejectIndex(std::move(mutation));
	}

	// Reused values are authenticated by their complete identity before the
	// snapshot is opened; an invalid identity must not reach that lookup.
	{
		NativeGraphDerivedStateMutation derived;
		derived.reused_large_values.push_back({});
		expectFormatError(
				[&] {
					static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
							noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements,
							noPropertyBatches, noLargeValues, derived, LogSequenceNumber(2U), graphPreparer,
							derivedPreparer));
				},
				FormatError::FERR_INVALID_HEADER);
	}
}

TEST_F(RelocatableMetadataStoreTest, RejectsMalformedVectorBootstrapHeadersAndIncompleteMappingRootsBeforeMaterialization) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
	const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
	const std::array<EntityPageNewPoolBatchRequest, 0U> noEntityPools{};
	const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
	const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyBatches{};
	const std::array<NativeGraphLargeValueMutation, 0U> noLargeValues{};
	MetadataSnapshotPin before = store->openSnapshot();
	const Superblock beforeSuperblock = before.superblock();
	before.release();

	const auto freshRoot = [](uint64_t indexId, PhysicalSlotId slot) {
		NativeGraphIndexTreeMutation root;
		root.index_id = indexId;
		root.new_pool_requests.push_back({slot, slot, indexId, 1U});
		root.replacement_root = NativeGraphIndexTreeAppendedPageReference{0U, 0U, true};
		return root;
	};
	const auto pinnedRoot = [](uint64_t indexId) {
		NativeGraphIndexTreeMutation root;
		root.index_id = indexId;
		root.expected_root = {indexId, PageId(indexId), Generation(1U)};
		root.pool_requests.push_back({SegmentId(indexId), {}, 1U, {}});
		root.replacement_root = NativeGraphIndexTreeAppendedPageReference{0U, 0U, false};
		return root;
	};
	const auto retiredRoot = [](uint64_t indexId) {
		NativeGraphIndexTreeMutation root;
		root.index_id = indexId;
		root.expected_root = {indexId, PageId(indexId), Generation(1U)};
		const PageLocation location{PageId(indexId), SegmentId(indexId), RSS_PAGE_POOL_FIRST_DATA_FRAME,
									Generation(1U), 0U};
		root.pool_requests.push_back({SegmentId(indexId), {}, 0U, {{location, LogSequenceNumber(1U)}}});
		root.retire_root = true;
		return root;
	};
	const auto rejectsBeforeMaterialization = [&](NativeGraphDerivedStateMutation derived) {
		bool graphPreparerCalled = false;
		bool derivedPreparerCalled = false;
		bool vectorPreparerCalled = false;
		expectFormatError(
				[&] {
					static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
							noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements,
							noPropertyBatches, noLargeValues, derived, LogSequenceNumber(2U),
							[&](std::span<EntityPageReplacementMaterialization>,
									std::span<EntityPageAppendBatchMaterialization>,
									std::span<EntityPageNewPoolBatchMaterialization>,
									std::span<NativeGraphPropertyPageReplacementMaterialization>,
									std::span<NativeGraphPropertyPageBatchMaterialization>,
									std::span<LargeValueMaterializationResult>) { graphPreparerCalled = true; },
							[&](std::vector<std::vector<IndexTreeMutationPoolBatch>> &) {
								derivedPreparerCalled = true;
							},
							[&](std::vector<NativeGraphVectorBootstrapMaterialization> &) {
								vectorPreparerCalled = true;
							}));
				},
				FormatError::FERR_INVALID_HEADER);
		EXPECT_FALSE(graphPreparerCalled);
		EXPECT_FALSE(derivedPreparerCalled);
		EXPECT_FALSE(vectorPreparerCalled);
		MetadataSnapshotPin unchanged = store->openSnapshot();
		EXPECT_EQ(unchanged.superblock(), beforeSuperblock);
		EXPECT_FALSE(store->isPhysicalSlotAllocated(unchanged, PhysicalSlotId(40U)));
		EXPECT_FALSE(store->isPhysicalSlotAllocated(unchanged, PhysicalSlotId(41U)));
		unchanged.release();
	};
	const auto bootstrap = [] {
		NativeGraphVectorBootstrapMutation mutation;
		mutation.index_id = 101U;
		mutation.vector_node_mapping_index_id = 102U;
		mutation.first_vector_id = 1U;
		mutation.vector_count = 1U;
		mutation.dimensions = 1U;
		mutation.metric = VectorDistanceMetric::VDM_L2;
		mutation.raw_page_count = 1U;
		mutation.first_slot = PhysicalSlotId(40U);
		mutation.last_slot = PhysicalSlotId(42U);
		return mutation;
	};
	const auto rejectBootstrap = [&](const std::function<void(NativeGraphVectorBootstrapMutation &)> &forge) {
		NativeGraphDerivedStateMutation derived;
		auto mutation = bootstrap();
		forge(mutation);
		derived.vector_bootstraps.push_back(std::move(mutation));
		rejectsBeforeMaterialization(std::move(derived));
	};

	// The coordinator accepts these public Vector bootstrap headers directly.
	// Reject each independently supplied identity and resource bound before it
	// can reserve a Raw or Manifest page.
	rejectBootstrap([](auto &mutation) { mutation.index_id = 0U; });
	rejectBootstrap([](auto &mutation) { mutation.vector_node_mapping_index_id = 0U; });
	rejectBootstrap([](auto &mutation) { mutation.vector_node_mapping_index_id = mutation.index_id; });
	rejectBootstrap([](auto &mutation) { mutation.first_vector_id = 0U; });
	rejectBootstrap([](auto &mutation) { mutation.vector_count = 0U; });
	rejectBootstrap([](auto &mutation) { mutation.dimensions = 0U; });
	rejectBootstrap([](auto &mutation) { mutation.metric = static_cast<VectorDistanceMetric>(255U); });
	rejectBootstrap([](auto &mutation) { mutation.raw_page_count = 0U; });
	rejectBootstrap([](auto &mutation) { mutation.raw_page_count = 2U; });
	rejectBootstrap([](auto &mutation) { mutation.first_slot = {}; });
	rejectBootstrap([](auto &mutation) { mutation.last_slot = {}; });
	rejectBootstrap([](auto &mutation) {
		mutation.first_slot = PhysicalSlotId(43U);
		mutation.last_slot = PhysicalSlotId(42U);
	});

	// A Vector bootstrap cannot expose Raw or Manifest pages unless both semantic
	// mapping trees are new immutable roots in the same publication.
	{
		NativeGraphDerivedStateMutation derived;
		derived.vector_bootstraps.push_back(bootstrap());
		rejectsBeforeMaterialization(std::move(derived));
	}
	{
		NativeGraphDerivedStateMutation derived;
		derived.index_tree_mutations.push_back(freshRoot(101U, PhysicalSlotId(40U)));
		derived.vector_bootstraps.push_back(bootstrap());
		rejectsBeforeMaterialization(std::move(derived));
	}
	{
		NativeGraphDerivedStateMutation derived;
		derived.index_tree_mutations = {pinnedRoot(101U), freshRoot(102U, PhysicalSlotId(41U))};
		derived.vector_bootstraps.push_back(bootstrap());
		rejectsBeforeMaterialization(std::move(derived));
	}
	{
		NativeGraphDerivedStateMutation derived;
		derived.index_tree_mutations = {freshRoot(101U, PhysicalSlotId(40U)), retiredRoot(102U)};
		derived.vector_bootstraps.push_back(bootstrap());
		rejectsBeforeMaterialization(std::move(derived));
	}
}

TEST_F(RelocatableMetadataStoreTest, RejectsMalformedVectorDeltaHeadersAndUnmatchedMappingTransitionsBeforeMaterialization) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
	const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
	const std::array<EntityPageNewPoolBatchRequest, 0U> noEntityPools{};
	const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
	const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyBatches{};
	const std::array<NativeGraphLargeValueMutation, 0U> noLargeValues{};
	constexpr uint64_t deltaMappingId = RSS_DELTA_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x11);

	const auto validDelta = [] {
		NativeGraphVectorDeltaMutation mutation;
		mutation.index_id = 101U;
		mutation.vector_node_mapping_index_id = 102U;
		mutation.expected_delta_mapping_index_id = deltaMappingId;
		mutation.delta_mapping_index_id = deltaMappingId;
		mutation.expected_node_mapping_root = {mutation.index_id, PageId(11U), Generation(1U)};
		mutation.expected_vector_node_mapping_root = {mutation.vector_node_mapping_index_id, PageId(12U), Generation(1U)};
		mutation.expected_delta_mapping_root = {deltaMappingId, PageId(13U), Generation(1U)};
		mutation.expected_manifest_pool.segment_id = SegmentId(21U);
		mutation.expected_manifest_location =
				{PageId(14U), SegmentId(21U), RSS_PAGE_POOL_FIRST_DATA_FRAME, Generation(1U), 0U};
		mutation.expected_manifest = {mutation.index_id, PageId(14U), Generation(1U)};
		mutation.expected_manifest_owner_lsn = LogSequenceNumber(1U);
		mutation.dimensions = 1U;
		mutation.metric = VectorDistanceMetric::VDM_L2;
		mutation.first_vector_id = 1U;
		mutation.inserted_vector_count = 1U;
		mutation.delta_entry_count = 1U;
		mutation.raw_pages = {{NativeGraphVectorDeltaRawPlacement::NGVDRP_NEW_POOL, {}, {}, {}, 0U, 1U, 1U}};
		mutation.first_slot = PhysicalSlotId(40U);
		mutation.last_slot = PhysicalSlotId(41U);
		return mutation;
	};
	const auto expectRejected = [&](NativeGraphDerivedStateMutation derived, FormatError expectedError) {
		bool graphPreparerCalled = false;
		bool derivedPreparerCalled = false;
		bool vectorPreparerCalled = false;
		expectFormatError(
				[&] {
					static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
							noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements,
							noPropertyBatches, noLargeValues, derived, LogSequenceNumber(2U),
							[&](std::span<EntityPageReplacementMaterialization>,
									std::span<EntityPageAppendBatchMaterialization>,
									std::span<EntityPageNewPoolBatchMaterialization>,
									std::span<NativeGraphPropertyPageReplacementMaterialization>,
									std::span<NativeGraphPropertyPageBatchMaterialization>,
									std::span<LargeValueMaterializationResult>) { graphPreparerCalled = true; },
							[&](std::vector<std::vector<IndexTreeMutationPoolBatch>> &) { derivedPreparerCalled = true; },
							{}, [&](std::vector<NativeGraphVectorDeltaMaterialization> &) { vectorPreparerCalled = true; }));
				},
				expectedError);
		EXPECT_FALSE(graphPreparerCalled);
		EXPECT_FALSE(derivedPreparerCalled);
		EXPECT_FALSE(vectorPreparerCalled);
	};
	const auto reject = [&](const std::function<void(NativeGraphVectorDeltaMutation &)> &forge) {
		NativeGraphDerivedStateMutation derived;
		auto mutation = validDelta();
		forge(mutation);
		derived.vector_deltas.push_back(std::move(mutation));
		expectRejected(std::move(derived), FormatError::FERR_INVALID_HEADER);
	};

	// The coordinator accepts Vector plans from the graph planner. Check every
	// independently supplied header fence before looking up its mapping roots.
	// A well-formed header must then fail closed when the corresponding mapping
	// mutations are absent from the same publication.
	reject([](auto &) {});
	reject([](auto &mutation) { mutation.metric = VectorDistanceMetric::VDM_COSINE; });
	reject([](auto &mutation) { mutation.metric = VectorDistanceMetric::VDM_INNER_PRODUCT; });
	reject([](auto &mutation) { mutation.index_id = 0U; });
	reject([](auto &mutation) { mutation.vector_node_mapping_index_id = 0U; });
	reject([](auto &mutation) { mutation.vector_node_mapping_index_id = mutation.index_id; });
	reject([](auto &mutation) { mutation.expected_delta_mapping_root.reset(); });
	reject([](auto &mutation) {
		mutation.expected_delta_mapping_index_id = 17U;
		mutation.expected_delta_mapping_root->index_id = 17U;
	});
	reject([](auto &mutation) { mutation.expected_delta_mapping_root->index_id = mutation.index_id; });
	reject([](auto &mutation) { mutation.expected_delta_mapping_root->root_page_id = {}; });
	reject([](auto &mutation) { mutation.expected_delta_mapping_root->root_generation = {}; });
	reject([](auto &mutation) { mutation.delta_entry_count = 0U; });
	reject([](auto &mutation) { mutation.delta_mapping_index_id = 17U; });
	reject([](auto &mutation) { mutation.expected_node_mapping_root.index_id = 77U; });
	reject([](auto &mutation) { mutation.expected_vector_node_mapping_root.index_id = 77U; });
	reject([](auto &mutation) { mutation.expected_node_mapping_root.root_page_id = {}; });
	reject([](auto &mutation) { mutation.expected_vector_node_mapping_root.root_generation = {}; });
	reject([](auto &mutation) { mutation.expected_manifest_pool.segment_id = {}; });
	reject([](auto &mutation) { mutation.expected_manifest_location.page_id = {}; });
	reject([](auto &mutation) { mutation.expected_manifest_location.generation = {}; });
	reject([](auto &mutation) { mutation.expected_manifest.index_id = 77U; });
	reject([](auto &mutation) { mutation.expected_manifest.manifest_page_id = PageId(15U); });
	reject([](auto &mutation) { mutation.expected_manifest.generation = Generation(2U); });
	reject([](auto &mutation) { mutation.expected_manifest_owner_lsn = {}; });
	reject([](auto &mutation) { mutation.dimensions = 0U; });
	reject([](auto &mutation) { mutation.metric = static_cast<VectorDistanceMetric>(255U); });
	reject([](auto &mutation) { mutation.first_vector_id = 0U; });
	reject([](auto &mutation) { mutation.inserted_vector_count = 0U; });
	reject([](auto &mutation) { mutation.first_vector_id = std::numeric_limits<uint64_t>::max(); mutation.inserted_vector_count = 2U; });
	reject([](auto &mutation) { mutation.first_slot = {}; });
	reject([](auto &mutation) { mutation.last_slot = {}; });
	reject([](auto &mutation) { mutation.first_slot = PhysicalSlotId(42U); mutation.last_slot = PhysicalSlotId(41U); });

	const auto pinnedMapping = [](uint64_t indexId, PageId rootPageId) {
		NativeGraphIndexTreeMutation mapping;
		mapping.index_id = indexId;
		mapping.expected_root = {indexId, rootPageId, Generation(1U)};
		const PageLocation replacement{
				rootPageId, SegmentId(indexId), RSS_PAGE_POOL_FIRST_DATA_FRAME, Generation(1U), 0U};
		mapping.pool_requests.push_back({SegmentId(indexId), {replacement}, 0U, {}});
		return mapping;
	};

	// Supply each preceding mapping fence in turn. This makes the later
	// transition clauses observable without materializing data or vector pages.
	{
		NativeGraphDerivedStateMutation derived;
		auto mutation = validDelta();
		derived.index_tree_mutations.push_back(
				pinnedMapping(mutation.index_id, mutation.expected_node_mapping_root.root_page_id));
		derived.vector_deltas.push_back(std::move(mutation));
		expectRejected(std::move(derived), FormatError::FERR_INVALID_HEADER);
	}
	{
		NativeGraphDerivedStateMutation derived;
		auto mutation = validDelta();
		derived.index_tree_mutations = {
				pinnedMapping(mutation.index_id, mutation.expected_node_mapping_root.root_page_id),
				pinnedMapping(mutation.vector_node_mapping_index_id,
							  mutation.expected_vector_node_mapping_root.root_page_id)};
		derived.vector_deltas.push_back(std::move(mutation));
		expectRejected(std::move(derived), FormatError::FERR_INVALID_HEADER);
	}
	{
		NativeGraphDerivedStateMutation derived;
		auto mutation = validDelta();
		derived.index_tree_mutations = {
				pinnedMapping(mutation.index_id, mutation.expected_node_mapping_root.root_page_id),
				pinnedMapping(mutation.vector_node_mapping_index_id,
							  mutation.expected_vector_node_mapping_root.root_page_id),
				pinnedMapping(mutation.delta_mapping_index_id,
							  mutation.expected_delta_mapping_root->root_page_id)};
		derived.vector_deltas.push_back(std::move(mutation));
		expectRejected(std::move(derived), FormatError::FERR_INVALID_STATE);
	}
}
