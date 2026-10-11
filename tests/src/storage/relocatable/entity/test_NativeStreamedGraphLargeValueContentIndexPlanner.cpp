/**
 * @file test_NativeStreamedGraphLargeValueContentIndexPlanner.cpp
 * @brief Deferred LargeValue content-index planning tests.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#include "storage/relocatable/metadata/RelocatableMetadataStoreTestFixture.hpp"

#include <optional>
#include <utility>
#include <vector>

#include "graph/storage/relocatable/entity/NativeStreamedGraphLargeValueContentIndexPlanner.hpp"

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

} // namespace graph::storage
