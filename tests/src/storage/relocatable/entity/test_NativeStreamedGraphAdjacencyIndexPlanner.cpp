/**
 * @file test_NativeStreamedGraphAdjacencyIndexPlanner.cpp
 * @brief Deferred global-adjacency planning tests for streamed Edge publication.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#include "storage/relocatable/metadata/RelocatableMetadataStoreTestFixture.hpp"

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "graph/storage/relocatable/adjacency/AdjacencyKeyCodec.hpp"
#include "graph/storage/relocatable/entity/NativeStreamedGraphAdjacencyIndexPlanner.hpp"
#include "graph/storage/relocatable/entity/NativeStreamedGraphPublicationIndexPlanner.hpp"
#include "graph/storage/relocatable/index/BTreeIndex.hpp"
#include "graph/storage/relocatable/property/LargeValueStore.hpp"

namespace graph::storage {

	namespace {

		[[nodiscard]] OrderedBTreeEntrySource catalogSourceFor(std::vector<BTreePageEntry> entries) {
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

		struct PrivateCandidates {
			NativeGraphPrivateSegmentSet identities{SegmentId(100U), Generation(7U), 4U, 88U};
			std::vector<std::optional<std::vector<std::byte>>> payloads =
					std::vector<std::optional<std::vector<std::byte>>>(identities.size());

			void set(size_t ordinal, std::vector<std::byte> payload) { payloads.at(ordinal) = std::move(payload); }

			[[nodiscard]] NativeBulkRunFragmentLocation location(size_t ordinal) const {
				const NativeGraphPrivateSegmentMaterialization &identity = identities.at(ordinal);
				return {identity.segment_id, identity.generation, identity.owner_id};
			}

			[[nodiscard]] NativeGraphPrivateSegmentReader reader() const {
				return [this](const NativeGraphPrivateSegmentMaterialization &identity) {
					const std::optional<std::vector<std::byte>> &payload = payloads.at(identities.ordinalOf(identity));
					if (!payload.has_value()) {
						throw FormatException(FormatError::FERR_INVALID_STATE,
											  "private adjacency candidate is not materialized");
					}
					return *payload;
				};
			}
		};

		void writeRun(PrivateCandidates &candidates, size_t fragmentOrdinal, size_t manifestOrdinal,
					  const BTreePageEntry &entry) {
			const NativeBulkRunFragment fragment{{entry}};
			const std::vector<std::byte> bytes = NativeBulkRunFragmentCodec::encode(fragment);
			candidates.set(fragmentOrdinal, bytes);
			NativeBulkRunManifest manifest;
			manifest.fragments.push_back(candidates.location(fragmentOrdinal));
			manifest.entry_count = 1U;
			manifest.encoded_bytes = bytes.size();
			candidates.set(manifestOrdinal, NativeBulkRunManifestCodec::encode(manifest));
		}

		[[nodiscard]] NativeBulkEdgePrivateAdjacencyRoots writeDirections(PrivateCandidates &candidates,
																		  uint64_t tag = 1U) {
			writeRun(candidates, 0U, 1U,
					 {AdjacencyKeyCodec::encode({GlobalAdjacencyDirection::GAD_OUT, 7U, 11U}), tag});
			writeRun(candidates, 2U, 3U, {AdjacencyKeyCodec::encode({GlobalAdjacencyDirection::GAD_IN, 9U, 11U}), tag});
			return {candidates.location(1U), candidates.location(3U)};
		}

	} // namespace

	TEST_F(RelocatableMetadataStoreTest, StreamedAdjacencyIndexPlansOneInitialGlobalTreeFromBothDirections) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
		SlottedPagePool pool(io, *metadata);
		BTreePageStore pages(pool, *metadata);
		PrivateCandidates candidates;
		const NativeBulkEdgePrivateAdjacencyRoots roots = writeDirections(candidates);
		MetadataSnapshotPin snapshot = metadata->openSnapshot();

		const NativeGraphDeferredIndexPlan plan = NativeStreamedGraphAdjacencyIndexPlanner::plan(
				pages, *metadata, roots, {PhysicalSlotId(10U), PhysicalSlotId(20U), 8U, 4U, 1U},
				{candidates.identities, candidates.reader(), snapshot}, LogSequenceNumber(2U));

		EXPECT_EQ(plan.mutation.index_id, RSS_GLOBAL_ADJACENCY_INDEX_ID);
		EXPECT_FALSE(plan.mutation.expected_root.has_value());
		ASSERT_TRUE(plan.mutation.replacement_root.has_value());
		EXPECT_TRUE(plan.mutation.replacement_root->newly_reserved);
		EXPECT_FALSE(plan.mutation.new_pool_requests.empty());
		EXPECT_TRUE(static_cast<bool>(plan.preparer));
		snapshot.release();
	}

	TEST_F(RelocatableMetadataStoreTest,
		   StreamedAdjacencyIndexPublishesAndReplacesItsTreeThroughTheDeferredGraphCoordinator) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
		SlottedPagePool pool(io, *metadata);
		BTreePageStore pages(pool, *metadata);
		NativeGraphDerivedStateMutation derived;
		for (size_t ordinal = 0U; ordinal < 4U; ++ordinal) {
			NativeGraphPrivateSegmentRequest scratch;
			scratch.kind = SegmentKind::SGK_BULK_SCRATCH;
			scratch.owner_id = 88U;
			scratch.first_slot = PhysicalSlotId(30U);
			scratch.last_slot = PhysicalSlotId(39U);
			derived.private_segments.push_back(std::move(scratch));
		}
		const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
		const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
		const std::array<EntityPageNewPoolBatchRequest, 0U> noEntityPools{};
		const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
		const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyBatches{};
		const std::array<NativeGraphLargeValueMutation, 0U> noLargeValues{};
		const auto location = [](const NativeGraphPrivateSegmentSet &identities, size_t ordinal) {
			return NativeBulkRunFragmentLocation{identities[ordinal].segment_id, identities[ordinal].generation,
												 identities[ordinal].owner_id};
		};

		const Superblock published = metadata->mutateNativeGraphPagesWithLargeValues(
				noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyBatches,
				noLargeValues, derived, LogSequenceNumber(2U),
				[](std::span<EntityPageReplacementMaterialization>, std::span<EntityPageAppendBatchMaterialization>,
				   std::span<EntityPageNewPoolBatchMaterialization>,
				   std::span<NativeGraphPropertyPageReplacementMaterialization>,
				   std::span<NativeGraphPropertyPageBatchMaterialization>,
				   std::span<LargeValueMaterializationResult>) {},
				[](std::vector<std::vector<IndexTreeMutationPoolBatch>> &) {}, {}, {}, {}, {}, {},
				MetadataPublishOptions{}, NativeGraphLogicalIdReservation{}, std::nullopt,
				[&location](const NativeGraphPrivateSegmentSet &identities, size_t ordinal) {
					if (identities.size() != 4U) {
						throw FormatException(FormatError::FERR_INVALID_STATE,
											  "adjacency publication lost a private-run identity");
					}
					const BTreePageEntry entry =
							ordinal < 2U ? BTreePageEntry{AdjacencyKeyCodec::encode(
																  {GlobalAdjacencyDirection::GAD_OUT, 7U, 11U}),
														  1U}
										 : BTreePageEntry{AdjacencyKeyCodec::encode(
																  {GlobalAdjacencyDirection::GAD_IN, 9U, 11U}),
														  1U};
					if (ordinal == 0U || ordinal == 2U) {
						return NativeBulkRunFragmentCodec::encode({{entry}});
					}
					NativeBulkRunManifest manifest;
					manifest.fragments = {location(identities, ordinal - 1U)};
					manifest.entry_count = 1U;
					manifest.encoded_bytes = NativeBulkRunFragmentCodec::encode({{entry}}).size();
					return NativeBulkRunManifestCodec::encode(manifest);
				},
				{},
				[&metadata, &pages, &location](const NativeGraphDeferredIndexPlanningContext &context) {
					return NativeStreamedGraphAdjacencyIndexPlanner::plan(
							pages, *metadata,
							{location(context.private_segments, 1U), location(context.private_segments, 3U)},
							{PhysicalSlotId(10U), PhysicalSlotId(20U), 8U, 4U, 1U}, context, LogSequenceNumber(2U));
				});

		EXPECT_EQ(published.generation, Generation(2U));
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const std::optional<IndexRootLocation> initialRoot =
				metadata->findIndexRootOptional(snapshot, RSS_GLOBAL_ADJACENCY_INDEX_ID);
		ASSERT_TRUE(initialRoot.has_value());
		snapshot.release();

		NativeGraphDerivedStateMutation replacement;
		for (size_t ordinal = 0U; ordinal < 4U; ++ordinal) {
			NativeGraphPrivateSegmentRequest scratch;
			scratch.kind = SegmentKind::SGK_BULK_SCRATCH;
			scratch.owner_id = 88U;
			scratch.first_slot = PhysicalSlotId(30U);
			scratch.last_slot = PhysicalSlotId(39U);
			replacement.private_segments.push_back(std::move(scratch));
		}
		const Superblock replaced = metadata->mutateNativeGraphPagesWithLargeValues(
				noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyBatches,
				noLargeValues, replacement, LogSequenceNumber(3U),
				[](std::span<EntityPageReplacementMaterialization>, std::span<EntityPageAppendBatchMaterialization>,
				   std::span<EntityPageNewPoolBatchMaterialization>,
				   std::span<NativeGraphPropertyPageReplacementMaterialization>,
				   std::span<NativeGraphPropertyPageBatchMaterialization>,
				   std::span<LargeValueMaterializationResult>) {},
				[](std::vector<std::vector<IndexTreeMutationPoolBatch>> &) {}, {}, {}, {}, {}, {},
				MetadataPublishOptions{}, NativeGraphLogicalIdReservation{}, std::nullopt,
				[&location](const NativeGraphPrivateSegmentSet &identities, size_t ordinal) {
					const BTreePageEntry entry =
							ordinal < 2U ? BTreePageEntry{AdjacencyKeyCodec::encode(
																  {GlobalAdjacencyDirection::GAD_OUT, 8U, 12U}),
														  1U}
										 : BTreePageEntry{AdjacencyKeyCodec::encode(
																  {GlobalAdjacencyDirection::GAD_IN, 10U, 12U}),
														  1U};
					if (ordinal == 0U || ordinal == 2U) {
						return NativeBulkRunFragmentCodec::encode({{entry}});
					}
					NativeBulkRunManifest manifest;
					manifest.fragments = {location(identities, ordinal - 1U)};
					manifest.entry_count = 1U;
					manifest.encoded_bytes = NativeBulkRunFragmentCodec::encode({{entry}}).size();
					return NativeBulkRunManifestCodec::encode(manifest);
				},
				{},
				[&metadata, &pages, &location](const NativeGraphDeferredIndexPlanningContext &context) {
					return NativeStreamedGraphAdjacencyIndexPlanner::plan(
							pages, *metadata,
							{location(context.private_segments, 1U), location(context.private_segments, 3U)},
							{PhysicalSlotId(10U), PhysicalSlotId(20U), 8U, 4U, 1U}, context, LogSequenceNumber(3U));
				});

		EXPECT_EQ(replaced.generation, Generation(3U));
		snapshot = metadata->openSnapshot();
		const std::optional<IndexRootLocation> replacementRoot =
				metadata->findIndexRootOptional(snapshot, RSS_GLOBAL_ADJACENCY_INDEX_ID);
		ASSERT_TRUE(replacementRoot.has_value());
		EXPECT_NE(replacementRoot->root_page_id, initialRoot->root_page_id);
		BTreeIndex index(RSS_GLOBAL_ADJACENCY_INDEX_ID, pages, *metadata, BTreeLeafValueLayout::BLVL_EMPTY);
		OrderedBTreeEntryCursor cursor = index.orderedSource(snapshot, 8U)();
		size_t entryCount = 0U;
		for (std::optional<BTreePageEntry> entry = cursor(); entry.has_value(); entry = cursor()) {
			++entryCount;
		}
		EXPECT_EQ(entryCount, 4U);
		snapshot.release();

		// A replacement plan cannot publish if the coordinator drops the old-tree
		// retirement partition before it invokes the deferred preparer.
		PrivateCandidates candidates;
		const NativeBulkEdgePrivateAdjacencyRoots roots = writeDirections(candidates);
		snapshot = metadata->openSnapshot();
		const NativeGraphDeferredIndexPlan rejectedPlan = NativeStreamedGraphAdjacencyIndexPlanner::plan(
				pages, *metadata, roots, {PhysicalSlotId(10U), PhysicalSlotId(20U), 8U, 4U, 1U},
				{candidates.identities, candidates.reader(), snapshot}, LogSequenceNumber(4U));
		expectFormatError(
				[&rejectedPlan] {
					std::vector<IndexTreeMutationPoolBatch> missingRetirementBatches;
					rejectedPlan.preparer(missingRetirementBatches);
				},
				FormatError::FERR_INVALID_STATE);
		snapshot.release();
	}

	TEST_F(RelocatableMetadataStoreTest, StreamedAdjacencyIndexRejectsForeignPrivateRun) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
		SlottedPagePool pool(io, *metadata);
		BTreePageStore pages(pool, *metadata);
		PrivateCandidates candidates;
		const NativeBulkEdgePrivateAdjacencyRoots roots = writeDirections(candidates);
		MetadataSnapshotPin snapshot = metadata->openSnapshot();

		expectFormatError(
				[&] {
					static_cast<void>(NativeStreamedGraphAdjacencyIndexPlanner::plan(
							pages, *metadata, {{SegmentId(999U), Generation(1U), 88U}, roots.incoming},
							{PhysicalSlotId(10U), PhysicalSlotId(20U), 8U, 4U, 1U},
							{candidates.identities, candidates.reader(), snapshot}, LogSequenceNumber(2U)));
				},
				FormatError::FERR_INVALID_STATE);
		snapshot.release();
	}

	TEST_F(RelocatableMetadataStoreTest, StreamedAdjacencyIndexRejectsForeignFragmentInsideAValidManifest) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
		SlottedPagePool pool(io, *metadata);
		BTreePageStore pages(pool, *metadata);
		PrivateCandidates candidates;
		const NativeBulkEdgePrivateAdjacencyRoots roots = writeDirections(candidates);
		NativeBulkRunManifest outgoingManifest;
		outgoingManifest.fragments.push_back({SegmentId(999U), Generation(1U), candidates.identities.front().owner_id});
		outgoingManifest.entry_count = 1U;
		outgoingManifest.encoded_bytes = candidates.payloads[0U]->size();
		candidates.set(1U, NativeBulkRunManifestCodec::encode(outgoingManifest));
		MetadataSnapshotPin snapshot = metadata->openSnapshot();

		expectFormatError(
				[&] {
					static_cast<void>(NativeStreamedGraphAdjacencyIndexPlanner::plan(
							pages, *metadata, roots, {PhysicalSlotId(10U), PhysicalSlotId(20U), 8U, 4U, 1U},
							{candidates.identities, candidates.reader(), snapshot}, LogSequenceNumber(2U)));
				},
				FormatError::FERR_INVALID_STATE);
		snapshot.release();
	}

	TEST_F(RelocatableMetadataStoreTest, StreamedAdjacencyIndexRejectsNonInsertionsBeforeInitialPublication) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
		SlottedPagePool pool(io, *metadata);
		BTreePageStore pages(pool, *metadata);
		PrivateCandidates candidates;
		const NativeBulkEdgePrivateAdjacencyRoots roots = writeDirections(candidates, 2U);
		MetadataSnapshotPin snapshot = metadata->openSnapshot();

		expectFormatError(
				[&] {
					static_cast<void>(NativeStreamedGraphAdjacencyIndexPlanner::plan(
							pages, *metadata, roots, {PhysicalSlotId(10U), PhysicalSlotId(20U), 8U, 4U, 1U},
							{candidates.identities, candidates.reader(), snapshot}, LogSequenceNumber(2U)));
				},
				FormatError::FERR_INVALID_STATE);
		snapshot.release();
	}

	TEST_F(RelocatableMetadataStoreTest, StreamedAdjacencyIndexRejectsAForeignPageStore) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
		PrivateCandidates candidates;
		const NativeBulkEdgePrivateAdjacencyRoots roots = writeDirections(candidates);
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const std::filesystem::path foreignPath = testFilePath.string() + ".foreign";
		auto foreignFile = std::make_shared<std::fstream>(
				foreignPath, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(foreignFile->is_open());
		auto foreignIo = std::make_shared<StorageIO>(foreignFile, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		auto foreignMetadata = RelocatableMetadataStore::initializeNew(foreignIo, superblock(1U, 1U));
		SlottedPagePool foreignPool(foreignIo, *foreignMetadata);
		BTreePageStore foreignPages(foreignPool, *foreignMetadata);

		expectFormatError(
				[&] {
					static_cast<void>(NativeStreamedGraphAdjacencyIndexPlanner::plan(
							foreignPages, *metadata, roots, {PhysicalSlotId(10U), PhysicalSlotId(20U), 8U, 4U, 1U},
							{candidates.identities, candidates.reader(), snapshot}, LogSequenceNumber(2U)));
				},
				FormatError::FERR_INVALID_HEADER);

		snapshot.release();
		foreignMetadata.reset();
		foreignIo.reset();
		foreignFile->close();
		std::error_code error;
		std::filesystem::remove(foreignPath, error);
	}

	TEST_F(RelocatableMetadataStoreTest, StreamedAdjacencyIndexRejectsIncompletePlanningContracts) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
		SlottedPagePool pool(io, *metadata);
		BTreePageStore pages(pool, *metadata);
		PrivateCandidates candidates;
		const NativeBulkEdgePrivateAdjacencyRoots roots = writeDirections(candidates);
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const NativeStreamedGraphAdjacencyIndexPlanningOptions valid{PhysicalSlotId(10U), PhysicalSlotId(20U), 8U, 4U,
																	 1U};
		const auto invoke = [&](NativeBulkEdgePrivateAdjacencyRoots privateRoots,
								NativeStreamedGraphAdjacencyIndexPlanningOptions options,
								LogSequenceNumber checkpoint) {
			return NativeStreamedGraphAdjacencyIndexPlanner::plan(
					pages, *metadata, privateRoots, options, {candidates.identities, candidates.reader(), snapshot},
					checkpoint);
		};
		EXPECT_THROW(static_cast<void>(invoke({}, valid, LogSequenceNumber(2U))), FormatException);
		auto invalidRoots = roots;
		invalidRoots.incoming = {};
		EXPECT_THROW(static_cast<void>(invoke(invalidRoots, valid, LogSequenceNumber(2U))), FormatException);
		MetadataSnapshotPin invalidSnapshot;
		EXPECT_THROW(static_cast<void>(NativeStreamedGraphAdjacencyIndexPlanner::plan(
							 pages, *metadata, roots, valid,
							 {candidates.identities, candidates.reader(), invalidSnapshot}, LogSequenceNumber(2U))),
					 FormatException);
		auto invalid = valid;
		invalid.first_slot = {};
		EXPECT_THROW(static_cast<void>(invoke(roots, invalid, LogSequenceNumber(2U))), FormatException);
		invalid = valid;
		invalid.last_slot = {};
		EXPECT_THROW(static_cast<void>(invoke(roots, invalid, LogSequenceNumber(2U))), FormatException);
		invalid = valid;
		invalid.last_slot = PhysicalSlotId(9U);
		EXPECT_THROW(static_cast<void>(invoke(roots, invalid, LogSequenceNumber(2U))), FormatException);
		invalid = valid;
		invalid.maximum_entries = 0U;
		EXPECT_THROW(static_cast<void>(invoke(roots, invalid, LogSequenceNumber(2U))), FormatException);
		invalid = valid;
		invalid.maximum_pages = 0U;
		EXPECT_THROW(static_cast<void>(invoke(roots, invalid, LogSequenceNumber(2U))), FormatException);
		invalid = valid;
		invalid.maximum_manifest_depth = 0U;
		EXPECT_THROW(static_cast<void>(invoke(roots, invalid, LogSequenceNumber(2U))), FormatException);
		EXPECT_THROW(static_cast<void>(invoke(roots, valid, {})), FormatException);
		const NativeGraphPrivateSegmentReader noReader;
		EXPECT_THROW(static_cast<void>(NativeStreamedGraphAdjacencyIndexPlanner::plan(
							 pages, *metadata, roots, valid, {candidates.identities, noReader, snapshot},
							 LogSequenceNumber(2U))),
					 FormatException);
		snapshot.release();
	}

	TEST_F(RelocatableMetadataStoreTest, StreamedIndexBundleContainsOnlyTheRequiredGlobalAdjacencyRoot) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
		SlottedPagePool pool(io, *metadata);
		BTreePageStore pages(pool, *metadata);
		SystemCatalog catalog(pages, *metadata);
		LargeValueStore values(io, *metadata);
		PrivateCandidates candidates;
		NativeStreamedGraphPrivatePreparation prepared;
		prepared.adjacency_roots = writeDirections(candidates);
		MetadataSnapshotPin snapshot = metadata->openSnapshot();

		const NativeGraphDeferredIndexPlans plans = NativeStreamedGraphPublicationIndexPlanner::plan(
				catalog, pages, &values, *metadata, prepared,
				{{PhysicalSlotId(10U), PhysicalSlotId(20U), 8U, 4U},
				 {PhysicalSlotId(10U), PhysicalSlotId(20U), 8U, 4U, 1U},
				 {PhysicalSlotId(10U), PhysicalSlotId(20U), 8U, 4U, 1U}},
				{candidates.identities, candidates.reader(), snapshot}, LogSequenceNumber(2U));

		ASSERT_EQ(plans.plans.size(), 1U);
		EXPECT_EQ(plans.plans.front().mutation.index_id, RSS_GLOBAL_ADJACENCY_INDEX_ID);
		snapshot.release();
	}

	TEST_F(RelocatableMetadataStoreTest, StreamedIndexBundleIncludesChangedCatalogBeforeAdjacency) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
		SlottedPagePool pool(io, *metadata);
		BTreePageStore pages(pool, *metadata);
		SystemCatalog catalog(pages, *metadata);
		PrivateCandidates candidates;
		NativeStreamedGraphPrivatePreparation prepared;
		prepared.adjacency_roots = writeDirections(candidates);
		prepared.catalog_tokens.final_catalog_entries = catalogSourceFor({{{std::byte{0x11}}, 7U}});
		prepared.catalog_tokens.catalog_changed = true;
		MetadataSnapshotPin snapshot = metadata->openSnapshot();

		const NativeGraphDeferredIndexPlans plans = NativeStreamedGraphPublicationIndexPlanner::plan(
				catalog, pages, nullptr, *metadata, prepared,
				{{PhysicalSlotId(10U), PhysicalSlotId(20U), 8U, 4U},
				 {PhysicalSlotId(10U), PhysicalSlotId(20U), 8U, 4U, 1U},
				 {PhysicalSlotId(10U), PhysicalSlotId(20U), 8U, 4U, 1U}},
				{candidates.identities, candidates.reader(), snapshot}, LogSequenceNumber(2U));

		ASSERT_EQ(plans.plans.size(), 2U);
		EXPECT_EQ(plans.plans[0].mutation.index_id, RSS_GLOBAL_ADJACENCY_INDEX_ID);
		EXPECT_EQ(plans.plans[1].mutation.index_id, RSS_SYSTEM_CATALOG_INDEX_ID);
		snapshot.release();
	}

	TEST_F(RelocatableMetadataStoreTest, StreamedIndexBundleValidatesACompleteLargeValueStoreBeforeReadingItsRun) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
		SlottedPagePool pool(io, *metadata);
		BTreePageStore pages(pool, *metadata);
		SystemCatalog catalog(pages, *metadata);
		LargeValueStore values(io, *metadata);
		PrivateCandidates candidates;
		NativeStreamedGraphPrivatePreparation prepared;
		prepared.adjacency_roots = writeDirections(candidates);
		prepared.large_values.emplace();
		prepared.large_values->content_run = candidates.location(1U);
		const NativeStreamedGraphPublicationIndexPlanningOptions options{
				{PhysicalSlotId(10U), PhysicalSlotId(20U), 8U, 4U},
				{PhysicalSlotId(10U), PhysicalSlotId(20U), 8U, 4U, 1U},
				{PhysicalSlotId(10U), PhysicalSlotId(20U), 8U, 4U, 1U}};
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		EXPECT_THROW(static_cast<void>(NativeStreamedGraphPublicationIndexPlanner::plan(
							 catalog, pages, &values, *metadata, prepared, options,
							 {candidates.identities, candidates.reader(), snapshot}, LogSequenceNumber(2U))),
					 FormatException);
		snapshot.release();
	}

	TEST_F(RelocatableMetadataStoreTest, StreamedIndexBundleRejectsInvalidPublicationFence) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
		SlottedPagePool pool(io, *metadata);
		BTreePageStore pages(pool, *metadata);
		SystemCatalog catalog(pages, *metadata);
		PrivateCandidates candidates;
		NativeStreamedGraphPrivatePreparation prepared;
		prepared.adjacency_roots = writeDirections(candidates);
		const NativeStreamedGraphPublicationIndexPlanningOptions options{
				{PhysicalSlotId(10U), PhysicalSlotId(20U), 8U, 4U},
				{PhysicalSlotId(10U), PhysicalSlotId(20U), 8U, 4U, 1U},
				{PhysicalSlotId(10U), PhysicalSlotId(20U), 8U, 4U, 1U}};
		const NativeGraphPrivateSegmentReader reader = candidates.reader();
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const NativeGraphDeferredIndexPlanningContext invalidSnapshot{candidates.identities, reader, snapshot};
		snapshot.release();
		expectFormatError(
				[&] {
					static_cast<void>(NativeStreamedGraphPublicationIndexPlanner::plan(
							catalog, pages, nullptr, *metadata, prepared, options, invalidSnapshot,
							LogSequenceNumber(2U)));
				},
				FormatError::FERR_INVALID_HEADER);

		MetadataSnapshotPin validSnapshot = metadata->openSnapshot();
		const NativeGraphDeferredIndexPlanningContext validPlanning{candidates.identities, reader, validSnapshot};
		expectFormatError(
				[&] {
					static_cast<void>(NativeStreamedGraphPublicationIndexPlanner::plan(
							catalog, pages, nullptr, *metadata, prepared, options, validPlanning, {}));
				},
				FormatError::FERR_INVALID_HEADER);
		validSnapshot.release();
	}

	TEST_F(RelocatableMetadataStoreTest, StreamedIndexBundleRejectsLargeValueWithoutNativeStore) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
		SlottedPagePool pool(io, *metadata);
		BTreePageStore pages(pool, *metadata);
		SystemCatalog catalog(pages, *metadata);
		PrivateCandidates candidates;
		NativeStreamedGraphPrivatePreparation prepared;
		prepared.adjacency_roots = writeDirections(candidates);
		prepared.large_values.emplace();
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const NativeStreamedGraphPublicationIndexPlanningOptions options{
				{PhysicalSlotId(10U), PhysicalSlotId(20U), 8U, 4U},
				{PhysicalSlotId(10U), PhysicalSlotId(20U), 8U, 4U, 1U},
				{PhysicalSlotId(10U), PhysicalSlotId(20U), 8U, 4U, 1U}};
		expectFormatError(
				[&] {
					static_cast<void>(NativeStreamedGraphPublicationIndexPlanner::plan(
							catalog, pages, nullptr, *metadata, prepared, options,
							{candidates.identities, candidates.reader(), snapshot}, LogSequenceNumber(2U)));
				},
				FormatError::FERR_INVALID_STATE);
		snapshot.release();
	}

	TEST_F(RelocatableMetadataStoreTest, StreamedIndexBundleRejectsForeignStores) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
		SlottedPagePool pool(io, *metadata);
		BTreePageStore pages(pool, *metadata);
		SystemCatalog catalog(pages, *metadata);
		PrivateCandidates candidates;
		NativeStreamedGraphPrivatePreparation prepared;
		prepared.adjacency_roots = writeDirections(candidates);
		const NativeStreamedGraphPublicationIndexPlanningOptions options{
				{PhysicalSlotId(10U), PhysicalSlotId(20U), 8U, 4U},
				{PhysicalSlotId(10U), PhysicalSlotId(20U), 8U, 4U, 1U},
				{PhysicalSlotId(10U), PhysicalSlotId(20U), 8U, 4U, 1U}};
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const NativeGraphPrivateSegmentReader reader = candidates.reader();
		const NativeGraphDeferredIndexPlanningContext planning{candidates.identities, reader, snapshot};

		const std::filesystem::path foreignPath = testFilePath.string() + ".foreign";
		auto foreignFile = std::make_shared<std::fstream>(foreignPath, std::ios::in | std::ios::out | std::ios::binary |
																			   std::ios::trunc);
		ASSERT_TRUE(foreignFile->is_open());
		auto foreignIo = std::make_shared<StorageIO>(foreignFile, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		auto foreignMetadata = RelocatableMetadataStore::initializeNew(foreignIo, superblock(1U, 1U));
		SlottedPagePool foreignPool(foreignIo, *foreignMetadata);
		BTreePageStore foreignPages(foreignPool, *foreignMetadata);
		SystemCatalog foreignCatalog(foreignPages, *foreignMetadata);
		LargeValueStore foreignValues(foreignIo, *foreignMetadata);

		expectFormatError(
				[&] {
					static_cast<void>(NativeStreamedGraphPublicationIndexPlanner::plan(
							foreignCatalog, pages, nullptr, *metadata, prepared, options, planning,
							LogSequenceNumber(2U)));
				},
				FormatError::FERR_INVALID_HEADER);
		expectFormatError(
				[&] {
					static_cast<void>(NativeStreamedGraphPublicationIndexPlanner::plan(
							catalog, foreignPages, nullptr, *metadata, prepared, options, planning,
							LogSequenceNumber(2U)));
				},
				FormatError::FERR_INVALID_HEADER);
		expectFormatError(
				[&] {
					static_cast<void>(NativeStreamedGraphPublicationIndexPlanner::plan(
							catalog, pages, &foreignValues, *metadata, prepared, options, planning,
							LogSequenceNumber(2U)));
				},
				FormatError::FERR_INVALID_STATE);

		prepared.large_values.emplace();
		expectFormatError(
				[&] {
					static_cast<void>(NativeStreamedGraphPublicationIndexPlanner::plan(
							catalog, pages, &foreignValues, *metadata, prepared, options, planning,
							LogSequenceNumber(2U)));
				},
				FormatError::FERR_INVALID_STATE);
		snapshot.release();
		foreignMetadata.reset();
		foreignIo.reset();
		foreignFile->close();
		std::error_code error;
		std::filesystem::remove(foreignPath, error);
	}

} // namespace graph::storage
