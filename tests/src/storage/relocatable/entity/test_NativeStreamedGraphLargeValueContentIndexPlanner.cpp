/**
 * @file test_NativeStreamedGraphLargeValueContentIndexPlanner.cpp
 * @brief Deferred LargeValue content-index planning tests.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#include "storage/relocatable/metadata/RelocatableMetadataStoreTestFixture.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "graph/storage/relocatable/entity/NativeStreamedGraphLargeValueContentIndexPlanner.hpp"
#include "graph/storage/relocatable/index/BTreeIndex.hpp"

namespace graph::storage {

	namespace {

		struct PrivateCandidates {
			NativeGraphPrivateSegmentSet identities{SegmentId(100U), Generation(7U), 2U, 88U};
			std::vector<std::optional<std::vector<std::byte>>> payloads =
					std::vector<std::optional<std::vector<std::byte>>>(identities.size());

			void set(size_t ordinal, std::vector<std::byte> payload) { payloads.at(ordinal) = std::move(payload); }

			[[nodiscard]] NativeGraphPrivateSegmentReader reader() const {
				return [this](const NativeGraphPrivateSegmentMaterialization &identity) {
					const std::optional<std::vector<std::byte>> &payload = payloads.at(identities.ordinalOf(identity));
					if (!payload.has_value()) {
						throw FormatException(FormatError::FERR_INVALID_STATE,
											  "private content-index candidate is not materialized");
					}
					return *payload;
				};
			}
		};

	} // namespace

	TEST_F(RelocatableMetadataStoreTest, StreamedLargeValueContentIndexPlansAnInitialTreeFromPrivateRun) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
		LargeValueStore values(io, *metadata);
		const std::vector<std::byte> bytes{std::byte{0x01}, std::byte{0x02}};
		const LargeValueRef reference =
				values.planReference(LargeValueId(1U), Generation(1U), bytes, LargeValueKind::LVK_BYTES,
									 RSS_ENTITY_PROPERTY_LARGE_VALUE_MEDIA_TYPE, {}, LargeValueExtentClass::LVEC_32K);
		PrivateCandidates candidates;
		const NativeBulkRunFragmentLocation fragmentLocation{candidates.identities[0U].segment_id,
															 candidates.identities[0U].generation,
															 candidates.identities[0U].owner_id};
		const NativeBulkRunFragment fragment{{{values.contentIndexKey(reference), reference.value_id.value()}}};
		const std::vector<std::byte> fragmentBytes = NativeBulkRunFragmentCodec::encode(fragment);
		candidates.set(0U, fragmentBytes);
		NativeBulkRunManifest manifest;
		manifest.fragments.push_back(fragmentLocation);
		manifest.entry_count = 1U;
		manifest.encoded_bytes = fragmentBytes.size();
		candidates.set(1U, NativeBulkRunManifestCodec::encode(manifest));
		const NativeBulkRunFragmentLocation root{candidates.identities[1U].segment_id,
												 candidates.identities[1U].generation,
												 candidates.identities[1U].owner_id};
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const NativeGraphDeferredIndexPlan plan = NativeStreamedGraphLargeValueContentIndexPlanner::plan(
				values, *metadata, root, {PhysicalSlotId(10U), PhysicalSlotId(20U), 8U, 4U, 1U},
				{candidates.identities, candidates.reader(), snapshot}, LogSequenceNumber(2U));

		EXPECT_EQ(plan.mutation.index_id, RSS_LARGE_VALUE_CONTENT_INDEX_ID);
		EXPECT_FALSE(plan.mutation.expected_root.has_value());
		ASSERT_TRUE(plan.mutation.replacement_root.has_value());
		EXPECT_TRUE(plan.mutation.replacement_root->newly_reserved);
		EXPECT_FALSE(plan.mutation.new_pool_requests.empty());
		EXPECT_TRUE(static_cast<bool>(plan.preparer));
		snapshot.release();
	}

	TEST_F(RelocatableMetadataStoreTest, StreamedLargeValueContentIndexRejectsAForeignPrivateManifest) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
		LargeValueStore values(io, *metadata);
		PrivateCandidates candidates;
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		EXPECT_THROW(static_cast<void>(NativeStreamedGraphLargeValueContentIndexPlanner::plan(
							 values, *metadata, {SegmentId(999U), Generation(1U), 88U},
							 {PhysicalSlotId(10U), PhysicalSlotId(20U), 8U, 4U, 1U},
							 {candidates.identities, candidates.reader(), snapshot}, LogSequenceNumber(2U))),
					 FormatException);
		snapshot.release();
	}

	TEST_F(RelocatableMetadataStoreTest, StreamedLargeValueContentIndexRejectsAForeignStore) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
		PrivateCandidates candidates;
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const std::filesystem::path foreignPath = testFilePath.string() + ".foreign";
		auto foreignFile = std::make_shared<std::fstream>(
				foreignPath, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(foreignFile->is_open());
		auto foreignIo = std::make_shared<StorageIO>(foreignFile, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		auto foreignMetadata = RelocatableMetadataStore::initializeNew(foreignIo, superblock(1U, 1U));
		LargeValueStore foreignValues(foreignIo, *foreignMetadata);

		expectFormatError(
				[&] {
					static_cast<void>(NativeStreamedGraphLargeValueContentIndexPlanner::plan(
							foreignValues, *metadata,
							{candidates.identities[1U].segment_id, candidates.identities[1U].generation,
							 candidates.identities[1U].owner_id},
							{PhysicalSlotId(10U), PhysicalSlotId(20U), 8U, 4U, 1U},
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

	TEST_F(RelocatableMetadataStoreTest, StreamedLargeValueContentIndexDeduplicatesContentIdentities) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
		LargeValueStore values(io, *metadata);
		const std::vector<std::byte> bytes{std::byte{0x01}, std::byte{0x02}};
		const LargeValueRef first = values.planReference(
				LargeValueId(1U), Generation(1U), bytes, LargeValueKind::LVK_BYTES,
				RSS_ENTITY_PROPERTY_LARGE_VALUE_MEDIA_TYPE, {}, LargeValueExtentClass::LVEC_32K);
		const LargeValueRef second = values.planReference(
				LargeValueId(2U), Generation(1U), bytes, LargeValueKind::LVK_BYTES,
				RSS_ENTITY_PROPERTY_LARGE_VALUE_MEDIA_TYPE, {}, LargeValueExtentClass::LVEC_32K);
		PrivateCandidates candidates;
		const NativeBulkRunFragmentLocation fragmentLocation{candidates.identities[0U].segment_id,
																												candidates.identities[0U].generation,
																												candidates.identities[0U].owner_id};
		NativeBulkRunFragment duplicateFragment;
		duplicateFragment.entries = {{values.contentIndexKey(first), first.value_id.value()},
											 {values.contentIndexKey(second), second.value_id.value()}};
		candidates.set(0U, NativeBulkRunFragmentCodec::encode(duplicateFragment));
		NativeBulkRunManifest manifest;
		manifest.fragments.push_back(fragmentLocation);
		manifest.entry_count = 2U;
		manifest.encoded_bytes = candidates.payloads[0U]->size();
		candidates.set(1U, NativeBulkRunManifestCodec::encode(manifest));
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const NativeGraphDeferredIndexPlan plan = NativeStreamedGraphLargeValueContentIndexPlanner::plan(
				values, *metadata,
				{candidates.identities[1U].segment_id, candidates.identities[1U].generation,
				 candidates.identities[1U].owner_id},
				{PhysicalSlotId(10U), PhysicalSlotId(20U), 8U, 4U, 1U},
				{candidates.identities, candidates.reader(), snapshot}, LogSequenceNumber(2U));
		EXPECT_FALSE(plan.mutation.expected_root.has_value());
		snapshot.release();
	}

	TEST_F(RelocatableMetadataStoreTest, StreamedLargeValueContentIndexRejectsInvalidEntryShape) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
		LargeValueStore values(io, *metadata);
		const std::vector<std::byte> validBytes{std::byte{0x01}};
		const LargeValueRef validReference = values.planReference(
				LargeValueId(1U), Generation(1U), validBytes, LargeValueKind::LVK_BYTES,
				RSS_ENTITY_PROPERTY_LARGE_VALUE_MEDIA_TYPE, {}, LargeValueExtentClass::LVEC_32K);
		PrivateCandidates candidates;
		const NativeBulkRunFragmentLocation fragmentLocation{candidates.identities[0U].segment_id,
																												candidates.identities[0U].generation,
																												candidates.identities[0U].owner_id};
		NativeBulkRunFragment malformedFragment;
		malformedFragment.entries = {{{std::byte{0x01}}, 1U}};
		candidates.set(0U, NativeBulkRunFragmentCodec::encode(malformedFragment));
		NativeBulkRunManifest manifest;
		manifest.fragments.push_back(fragmentLocation);
		manifest.entry_count = 1U;
		manifest.encoded_bytes = candidates.payloads[0U]->size();
		candidates.set(1U, NativeBulkRunManifestCodec::encode(manifest));
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		expectFormatError(
				[&] {
					static_cast<void>(NativeStreamedGraphLargeValueContentIndexPlanner::plan(
							values, *metadata,
							{candidates.identities[1U].segment_id, candidates.identities[1U].generation,
							 candidates.identities[1U].owner_id},
							{PhysicalSlotId(10U), PhysicalSlotId(20U), 8U, 4U, 1U},
							{candidates.identities, candidates.reader(), snapshot}, LogSequenceNumber(2U)));
				},
				FormatError::FERR_INVALID_STATE);

		// The content key is well-formed here, but a zero canonical value ID
		// would make the published index point at no LargeValue.
		malformedFragment.entries = {{values.contentIndexKey(validReference), 0U}};
		candidates.set(0U, NativeBulkRunFragmentCodec::encode(malformedFragment));
		manifest.encoded_bytes = candidates.payloads[0U]->size();
		candidates.set(1U, NativeBulkRunManifestCodec::encode(manifest));
		expectFormatError(
				[&] {
					static_cast<void>(NativeStreamedGraphLargeValueContentIndexPlanner::plan(
							values, *metadata,
							{candidates.identities[1U].segment_id, candidates.identities[1U].generation,
							 candidates.identities[1U].owner_id},
							{PhysicalSlotId(10U), PhysicalSlotId(20U), 8U, 4U, 1U},
							{candidates.identities, candidates.reader(), snapshot}, LogSequenceNumber(2U)));
				},
				FormatError::FERR_INVALID_STATE);
		snapshot.release();
	}

	TEST_F(RelocatableMetadataStoreTest, StreamedLargeValueContentIndexReplacesAnExistingTree) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
		LargeValueStore values(io, *metadata);
		const std::vector<std::byte> bytes{std::byte{0x01}, std::byte{0x02}};
		const LargeValueRef reference = values.planReference(
				LargeValueId(1U), Generation(1U), bytes, LargeValueKind::LVK_BYTES,
				RSS_ENTITY_PROPERTY_LARGE_VALUE_MEDIA_TYPE, {}, LargeValueExtentClass::LVEC_32K);
		const LargeValueRef replacementReference = values.planReference(
				LargeValueId(2U), Generation(1U), std::vector<std::byte>{std::byte{0x03}}, LargeValueKind::LVK_BYTES,
				RSS_ENTITY_PROPERTY_LARGE_VALUE_MEDIA_TYPE, {}, LargeValueExtentClass::LVEC_32K);
		PrivateCandidates candidates;
		const NativeBulkRunFragmentLocation fragmentLocation{candidates.identities[0U].segment_id,
																												candidates.identities[0U].generation,
																												candidates.identities[0U].owner_id};
		NativeBulkRunFragment fragment{{{values.contentIndexKey(reference), reference.value_id.value()}}};
		candidates.set(0U, NativeBulkRunFragmentCodec::encode(fragment));
		NativeBulkRunManifest manifest;
		manifest.fragments.push_back(fragmentLocation);
		manifest.entry_count = 1U;
		manifest.encoded_bytes = candidates.payloads[0U]->size();
		candidates.set(1U, NativeBulkRunManifestCodec::encode(manifest));
		const NativeBulkRunFragmentLocation root{candidates.identities[1U].segment_id,
																 candidates.identities[1U].generation,
																 candidates.identities[1U].owner_id};
		const NativeStreamedGraphLargeValueContentIndexPlanningOptions options{
				PhysicalSlotId(10U), PhysicalSlotId(20U), 8U, 4U, 1U};
		const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
		const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
		const std::array<EntityPageNewPoolBatchRequest, 0U> noEntityPools{};
		const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
		const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyBatches{};
		const std::array<NativeGraphLargeValueMutation, 0U> noLargeValues{};
		MetadataSnapshotPin initialSnapshot = metadata->openSnapshot();
		const NativeGraphDeferredIndexPlanningContext initialPlanning{candidates.identities, candidates.reader(),
																													 initialSnapshot};
		const NativeGraphDeferredIndexPlan initial = NativeStreamedGraphLargeValueContentIndexPlanner::plan(
				values, *metadata, root, options, initialPlanning, LogSequenceNumber(2U));
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
		MetadataSnapshotPin initialVisibleSnapshot = metadata->openSnapshot();
		const IndexRootLocation initialRoot =
				metadata->findIndexRoot(initialVisibleSnapshot, RSS_LARGE_VALUE_CONTENT_INDEX_ID);
		initialVisibleSnapshot.release();
		NativeBulkRunFragment replacementFragment{{{values.contentIndexKey(replacementReference),
																															 replacementReference.value_id.value()}}};
		candidates.set(0U, NativeBulkRunFragmentCodec::encode(replacementFragment));

		MetadataSnapshotPin replacementSnapshot = metadata->openSnapshot();
		const NativeGraphDeferredIndexPlan replacement = NativeStreamedGraphLargeValueContentIndexPlanner::plan(
				values, *metadata, root, options,
				{candidates.identities, candidates.reader(), replacementSnapshot}, LogSequenceNumber(3U));
		ASSERT_TRUE(replacement.mutation.expected_root.has_value());
		std::vector<IndexTreeMutationPoolBatch> undersized;
		EXPECT_THROW(replacement.preparer(undersized), FormatException);
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
		MetadataSnapshotPin replacementVisibleSnapshot = metadata->openSnapshot();
		const IndexRootLocation replacementRoot =
				metadata->findIndexRoot(replacementVisibleSnapshot, RSS_LARGE_VALUE_CONTENT_INDEX_ID);
		EXPECT_NE(replacementRoot.root_page_id, initialRoot.root_page_id);
		BTreeIndex index(RSS_LARGE_VALUE_CONTENT_INDEX_ID, values.contentIndex(), *metadata);
		OrderedBTreeEntryCursor cursor = index.orderedSource(replacementVisibleSnapshot, 8U)();
		size_t entryCount = 0U;
		for (std::optional<BTreePageEntry> entry = cursor(); entry.has_value(); entry = cursor()) {
			++entryCount;
		}
		EXPECT_EQ(entryCount, 2U);
		replacementVisibleSnapshot.release();
	}

	TEST_F(RelocatableMetadataStoreTest, StreamedLargeValueContentIndexRejectsIncompletePlanningContracts) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
		LargeValueStore values(io, *metadata);
		PrivateCandidates candidates;
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const NativeBulkRunFragmentLocation root{candidates.identities[1U].segment_id,
												  candidates.identities[1U].generation,
												  candidates.identities[1U].owner_id};
		const NativeStreamedGraphLargeValueContentIndexPlanningOptions validOptions{
				PhysicalSlotId(10U), PhysicalSlotId(20U), 8U, 4U, 1U};
		const auto invoke = [&](NativeBulkRunFragmentLocation privateRoot,
							  NativeStreamedGraphLargeValueContentIndexPlanningOptions options,
							  LogSequenceNumber checkpoint) {
			return NativeStreamedGraphLargeValueContentIndexPlanner::plan(
					values, *metadata, privateRoot, options, {candidates.identities, candidates.reader(), snapshot}, checkpoint);
		};

		EXPECT_THROW(static_cast<void>(invoke({}, validOptions, LogSequenceNumber(2U))), FormatException);
		auto invalidSlots = validOptions;
		invalidSlots.first_slot = {};
		EXPECT_THROW(static_cast<void>(invoke(root, invalidSlots, LogSequenceNumber(2U))), FormatException);
		invalidSlots = validOptions;
		invalidSlots.last_slot = {};
		EXPECT_THROW(static_cast<void>(invoke(root, invalidSlots, LogSequenceNumber(2U))), FormatException);
		invalidSlots = validOptions;
		invalidSlots.last_slot = PhysicalSlotId(9U);
		EXPECT_THROW(static_cast<void>(invoke(root, invalidSlots, LogSequenceNumber(2U))), FormatException);
		invalidSlots = validOptions;
		invalidSlots.maximum_entries = 0U;
		EXPECT_THROW(static_cast<void>(invoke(root, invalidSlots, LogSequenceNumber(2U))), FormatException);
		invalidSlots = validOptions;
		invalidSlots.maximum_pages = 0U;
		EXPECT_THROW(static_cast<void>(invoke(root, invalidSlots, LogSequenceNumber(2U))), FormatException);
		invalidSlots = validOptions;
		invalidSlots.maximum_manifest_depth = 0U;
		EXPECT_THROW(static_cast<void>(invoke(root, invalidSlots, LogSequenceNumber(2U))), FormatException);
		EXPECT_THROW(static_cast<void>(invoke(root, validOptions, {})), FormatException);
		MetadataSnapshotPin invalidSnapshot;
		EXPECT_THROW(static_cast<void>(NativeStreamedGraphLargeValueContentIndexPlanner::plan(
						values, *metadata, root, validOptions,
						{candidates.identities, candidates.reader(), invalidSnapshot}, LogSequenceNumber(2U))),
					 FormatException);

		const NativeGraphPrivateSegmentReader noReader;
		EXPECT_THROW(static_cast<void>(NativeStreamedGraphLargeValueContentIndexPlanner::plan(
						 values, *metadata, root, validOptions, {candidates.identities, noReader, snapshot},
						 LogSequenceNumber(2U))),
				 FormatException);
		snapshot.release();
	}

} // namespace graph::storage
