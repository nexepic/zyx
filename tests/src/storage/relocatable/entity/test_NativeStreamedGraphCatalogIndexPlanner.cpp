/**
 * @file test_NativeStreamedGraphCatalogIndexPlanner.cpp
 * @brief Deferred System Catalog planning tests for streamed Edge publication.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#include "storage/relocatable/metadata/RelocatableMetadataStoreTestFixture.hpp"

#include <array>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "graph/storage/relocatable/entity/NativeStreamedGraphCatalogIndexPlanner.hpp"

namespace graph::storage {

	namespace {

		[[nodiscard]] OrderedBTreeEntrySource sourceFor(std::vector<BTreePageEntry> entries) {
			const auto retained = std::make_shared<const std::vector<BTreePageEntry>>(std::move(entries));
			return [retained] {
				auto ordinal = std::make_shared<size_t>(0U);
				return [retained, ordinal]() mutable -> std::optional<BTreePageEntry> {
					if (*ordinal == retained->size()) {
						return std::nullopt;
					}
					return (*retained)[(*ordinal)++];
				};
			};
		}

	} // namespace

	TEST_F(RelocatableMetadataStoreTest, StreamedCatalogIndexPlansAnInitialTreeWithoutPublishingIt) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
		SlottedPagePool pool(io, *metadata);
		BTreePageStore pages(pool, *metadata);
		SystemCatalog catalog(pages, *metadata);
		MetadataSnapshotPin snapshot = metadata->openSnapshot();

		const NativeGraphDeferredIndexPlan plan = *NativeStreamedGraphCatalogIndexPlanner::plan(
				catalog, *metadata, sourceFor({{{std::byte{0x11}}, 7U}}), true,
				{PhysicalSlotId(10U), PhysicalSlotId(20U), 8U, 4U}, snapshot, LogSequenceNumber(2U));

		EXPECT_EQ(plan.mutation.index_id, RSS_SYSTEM_CATALOG_INDEX_ID);
		EXPECT_FALSE(plan.mutation.expected_root.has_value());
		ASSERT_TRUE(plan.mutation.replacement_root.has_value());
		EXPECT_TRUE(plan.mutation.replacement_root->newly_reserved);
		EXPECT_FALSE(plan.mutation.new_pool_requests.empty());
		EXPECT_TRUE(static_cast<bool>(plan.preparer));
		snapshot.release();
	}

	TEST_F(RelocatableMetadataStoreTest, StreamedCatalogIndexSkipsAnUnchangedCatalog) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
		SlottedPagePool pool(io, *metadata);
		BTreePageStore pages(pool, *metadata);
		SystemCatalog catalog(pages, *metadata);
		MetadataSnapshotPin snapshot = metadata->openSnapshot();

		EXPECT_FALSE(NativeStreamedGraphCatalogIndexPlanner::plan(catalog, *metadata, {}, false,
																  {PhysicalSlotId(10U), PhysicalSlotId(20U), 8U, 4U},
																  snapshot, LogSequenceNumber(2U))
							 .has_value());
		snapshot.release();
	}

	TEST_F(RelocatableMetadataStoreTest, StreamedCatalogIndexRejectsChangedCatalogWithoutABoundedSource) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
		SlottedPagePool pool(io, *metadata);
		BTreePageStore pages(pool, *metadata);
		SystemCatalog catalog(pages, *metadata);
		MetadataSnapshotPin snapshot = metadata->openSnapshot();

		expectFormatError(
				[&] {
					static_cast<void>(NativeStreamedGraphCatalogIndexPlanner::plan(
							catalog, *metadata, {}, true, {PhysicalSlotId(10U), PhysicalSlotId(20U), 8U, 4U}, snapshot,
							LogSequenceNumber(2U)));
				},
				FormatError::FERR_INVALID_HEADER);
		snapshot.release();
	}

	TEST_F(RelocatableMetadataStoreTest, StreamedCatalogIndexRejectsEveryInvalidPlanningBound) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
		SlottedPagePool pool(io, *metadata);
		BTreePageStore pages(pool, *metadata);
		SystemCatalog catalog(pages, *metadata);
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const NativeStreamedGraphCatalogIndexPlanningOptions valid{PhysicalSlotId(10U), PhysicalSlotId(20U), 8U, 4U};
		const auto expectInvalid = [&](NativeStreamedGraphCatalogIndexPlanningOptions options,
									const MetadataSnapshotPin &pin, LogSequenceNumber lsn) {
			EXPECT_THROW(static_cast<void>(NativeStreamedGraphCatalogIndexPlanner::plan(
						catalog, *metadata, {}, true, options, pin, lsn)), FormatException);
		};
		auto invalid = valid;
		invalid.first_slot = {};
		expectInvalid(invalid, snapshot, LogSequenceNumber(2U));
		invalid = valid;
		invalid.last_slot = {};
		expectInvalid(invalid, snapshot, LogSequenceNumber(2U));
		invalid = valid;
		invalid.first_slot = PhysicalSlotId(21U);
		expectInvalid(invalid, snapshot, LogSequenceNumber(2U));
		invalid = valid;
		invalid.maximum_entries = 0U;
		expectInvalid(invalid, snapshot, LogSequenceNumber(2U));
		invalid = valid;
		invalid.maximum_pages = 0U;
		expectInvalid(invalid, snapshot, LogSequenceNumber(2U));
		expectInvalid(valid, snapshot, {});
		snapshot.release();
	}

	TEST_F(RelocatableMetadataStoreTest, StreamedCatalogIndexRejectsForeignCatalogAndReleasedSnapshot) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
		SlottedPagePool pool(io, *metadata);
		BTreePageStore pages(pool, *metadata);
		SystemCatalog catalog(pages, *metadata);
		const std::filesystem::path foreignPath = testFilePath.string() + ".foreign";
		auto foreignFile = std::make_shared<std::fstream>(
				foreignPath, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(foreignFile->is_open());
		auto foreignIo = std::make_shared<StorageIO>(foreignFile, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		auto foreignMetadata = RelocatableMetadataStore::initializeNew(foreignIo, superblock(1U, 1U));
		SlottedPagePool foreignPool(foreignIo, *foreignMetadata);
		BTreePageStore foreignPages(foreignPool, *foreignMetadata);
		SystemCatalog foreignCatalog(foreignPages, *foreignMetadata);
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const NativeStreamedGraphCatalogIndexPlanningOptions options{PhysicalSlotId(10U), PhysicalSlotId(20U), 8U, 4U};

		expectFormatError(
				[&] {
					static_cast<void>(NativeStreamedGraphCatalogIndexPlanner::plan(
							foreignCatalog, *metadata, sourceFor({{{std::byte{0x11}}, 7U}}), true, options, snapshot,
							LogSequenceNumber(2U)));
				},
				FormatError::FERR_INVALID_HEADER);
		snapshot.release();
		expectFormatError(
				[&] {
					static_cast<void>(NativeStreamedGraphCatalogIndexPlanner::plan(
							catalog, *metadata, sourceFor({{{std::byte{0x11}}, 7U}}), true, options, snapshot,
							LogSequenceNumber(2U)));
				},
				FormatError::FERR_INVALID_HEADER);
	}

	TEST_F(RelocatableMetadataStoreTest, StreamedCatalogIndexReplacesAnExistingTreeAndRetiresItsOldPools) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
		SlottedPagePool pool(io, *metadata);
		BTreePageStore pages(pool, *metadata);
		SystemCatalog catalog(pages, *metadata);
		const NativeStreamedGraphCatalogIndexPlanningOptions options{PhysicalSlotId(10U), PhysicalSlotId(20U), 8U, 4U};

		const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
		const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
		const std::array<EntityPageNewPoolBatchRequest, 0U> noEntityPools{};
		const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
		const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyBatches{};
		const std::array<NativeGraphLargeValueMutation, 0U> noLargeValues{};

		MetadataSnapshotPin initialSnapshot = metadata->openSnapshot();
		const NativeGraphDeferredIndexPlan initial = *NativeStreamedGraphCatalogIndexPlanner::plan(
				catalog, *metadata, sourceFor({{{std::byte{0x11}}, 7U}}), true, options, initialSnapshot,
				LogSequenceNumber(2U));
		NativeGraphDerivedStateMutation initialDerived;
		initialDerived.index_tree_mutations.push_back(initial.mutation);
		static_cast<void>(metadata->mutateNativeGraphPagesWithLargeValues(
				noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyBatches,
				noLargeValues, initialDerived, LogSequenceNumber(2U),
				[](std::span<EntityPageReplacementMaterialization>, std::span<EntityPageAppendBatchMaterialization>,
				   std::span<EntityPageNewPoolBatchMaterialization>,
				   std::span<NativeGraphPropertyPageReplacementMaterialization>,
				   std::span<NativeGraphPropertyPageBatchMaterialization>, std::span<LargeValueMaterializationResult>) {},
				[&initial](std::vector<std::vector<IndexTreeMutationPoolBatch>> &batches) {
					ASSERT_EQ(batches.size(), 1U);
					initial.preparer(batches.front());
				}, {}, {}, {}, {}, {}, MetadataPublishOptions{}, NativeGraphLogicalIdReservation{}, std::nullopt));
		initialSnapshot.release();

		MetadataSnapshotPin replacementSnapshot = metadata->openSnapshot();
		expectFormatError(
				[&] {
					static_cast<void>(NativeStreamedGraphCatalogIndexPlanner::plan(
							catalog, *metadata, sourceFor({}), true, options, replacementSnapshot, LogSequenceNumber(3U)));
				},
				FormatError::FERR_INVALID_STATE);
		const NativeGraphDeferredIndexPlan replacement = *NativeStreamedGraphCatalogIndexPlanner::plan(
				catalog, *metadata, sourceFor({{{std::byte{0x11}}, 7U}, {{std::byte{0x22}}, 9U}}), true, options,
				replacementSnapshot, LogSequenceNumber(3U));
		ASSERT_TRUE(replacement.mutation.expected_root.has_value());
		ASSERT_FALSE(replacement.mutation.pool_requests.empty());
		ASSERT_TRUE(replacement.mutation.replacement_root.has_value());
		std::vector<IndexTreeMutationPoolBatch> undersizedBatches;
		EXPECT_THROW(replacement.preparer(undersizedBatches), FormatException);

		NativeGraphDerivedStateMutation replacementDerived;
		replacementDerived.index_tree_mutations.push_back(replacement.mutation);
		static_cast<void>(metadata->mutateNativeGraphPagesWithLargeValues(
				noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyBatches,
				noLargeValues, replacementDerived, LogSequenceNumber(3U),
				[](std::span<EntityPageReplacementMaterialization>, std::span<EntityPageAppendBatchMaterialization>,
				   std::span<EntityPageNewPoolBatchMaterialization>,
				   std::span<NativeGraphPropertyPageReplacementMaterialization>,
				   std::span<NativeGraphPropertyPageBatchMaterialization>, std::span<LargeValueMaterializationResult>) {},
				[&replacement](std::vector<std::vector<IndexTreeMutationPoolBatch>> &batches) {
					ASSERT_EQ(batches.size(), 1U);
					replacement.preparer(batches.front());
				}, {}, {}, {}, {}, {}, MetadataPublishOptions{}, NativeGraphLogicalIdReservation{}, std::nullopt));
		replacementSnapshot.release();

		MetadataSnapshotPin current = metadata->openSnapshot();
		EXPECT_TRUE(metadata->findIndexRootOptional(current, RSS_SYSTEM_CATALOG_INDEX_ID).has_value());
		current.release();
	}

} // namespace graph::storage
