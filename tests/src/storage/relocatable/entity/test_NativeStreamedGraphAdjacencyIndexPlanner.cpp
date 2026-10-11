/**
 * @file test_NativeStreamedGraphAdjacencyIndexPlanner.cpp
 * @brief Deferred global-adjacency planning tests for streamed Edge publication.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#include "storage/relocatable/metadata/RelocatableMetadataStoreTestFixture.hpp"

#include <optional>
#include <utility>
#include <vector>

#include "graph/storage/relocatable/adjacency/AdjacencyKeyCodec.hpp"
#include "graph/storage/relocatable/entity/NativeStreamedGraphAdjacencyIndexPlanner.hpp"
#include "graph/storage/relocatable/entity/NativeStreamedGraphPublicationIndexPlanner.hpp"

namespace graph::storage {

	namespace {

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

	TEST_F(RelocatableMetadataStoreTest, StreamedIndexBundleContainsOnlyTheRequiredGlobalAdjacencyRoot) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
		SlottedPagePool pool(io, *metadata);
		BTreePageStore pages(pool, *metadata);
		SystemCatalog catalog(pages, *metadata);
		PrivateCandidates candidates;
		NativeStreamedGraphPrivatePreparation prepared;
		prepared.adjacency_roots = writeDirections(candidates);
		MetadataSnapshotPin snapshot = metadata->openSnapshot();

		const NativeGraphDeferredIndexPlans plans = NativeStreamedGraphPublicationIndexPlanner::plan(
				catalog, pages, nullptr, *metadata, prepared,
				{{PhysicalSlotId(10U), PhysicalSlotId(20U), 8U, 4U},
				 {PhysicalSlotId(10U), PhysicalSlotId(20U), 8U, 4U, 1U},
				 {PhysicalSlotId(10U), PhysicalSlotId(20U), 8U, 4U, 1U}},
				{candidates.identities, candidates.reader(), snapshot}, LogSequenceNumber(2U));

		ASSERT_EQ(plans.plans.size(), 1U);
		EXPECT_EQ(plans.plans.front().mutation.index_id, RSS_GLOBAL_ADJACENCY_INDEX_ID);
		snapshot.release();
	}

} // namespace graph::storage
