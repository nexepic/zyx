/**
 * @file test_NativeStreamedGraphCatalogIndexPlanner.cpp
 * @brief Deferred System Catalog planning tests for streamed Edge publication.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#include "storage/relocatable/metadata/RelocatableMetadataStoreTestFixture.hpp"

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

} // namespace graph::storage
