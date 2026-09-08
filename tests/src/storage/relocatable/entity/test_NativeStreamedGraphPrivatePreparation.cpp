/**
 * @file test_NativeStreamedGraphPrivatePreparation.cpp
 * @brief Writer-scoped private streamed Edge preparation tests.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/entity/EntityLabelSetStore.hpp"
#include "graph/storage/relocatable/entity/EntityPageStore.hpp"
#include "graph/storage/relocatable/entity/NativeGraphWritePlanner.hpp"
#include "graph/storage/relocatable/entity/NativeStreamedGraphLargeValueReferenceMapBuilder.hpp"
#include "graph/storage/relocatable/entity/NativeStreamedGraphPrivatePreparation.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRootStore.hpp"

using namespace graph::storage;

namespace {

	[[nodiscard]] Superblock initialSuperblock() {
		Superblock result;
		result.database_id = DatabaseId(0xABCD123400001234ULL);
		result.generation = Generation(1U);
		result.checkpoint_lsn = LogSequenceNumber(1U);
		return result;
	}

	[[nodiscard]] NativeBulkEdgeRecordSource source(std::vector<NativeBulkEdgeRecord> records) {
		return [records = std::move(records)] {
			return [records, ordinal = size_t{0U}]() mutable -> std::optional<NativeBulkEdgeRecord> {
				return ordinal == records.size() ? std::nullopt
												 : std::optional<NativeBulkEdgeRecord>(records[ordinal++]);
			};
		};
	}

	struct PrivateCandidates {
		explicit PrivateCandidates(size_t count) :
			identities(SegmentId(100U), Generation(7U), count, 88U), payloads(count) {}

		NativeGraphPrivateSegmentSet identities;
		std::vector<std::optional<std::vector<std::byte>>> payloads;
		size_t fences = 0U;

		void write(const NativeGraphPrivateSegmentMaterialization &identity, std::vector<std::byte> payload) {
			const size_t ordinal = identities.ordinalOf(identity);
			ASSERT_FALSE(payloads[ordinal].has_value());
			payloads[ordinal] = std::move(payload);
		}

		[[nodiscard]] NativeGraphPrivateSegmentReader reader() const {
			return [this](const NativeGraphPrivateSegmentMaterialization &identity) {
				const auto &payload = payloads.at(identities.ordinalOf(identity));
				if (!payload.has_value()) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "candidate reader encountered an unfilled reservation");
				}
				return *payload;
			};
		}
	};

} // namespace

class NativeStreamedGraphPrivatePreparationTest : public ::testing::Test {
protected:
	std::filesystem::path path_;
	std::shared_ptr<std::fstream> file_;
	std::shared_ptr<StorageIO> io_;
	std::unique_ptr<RelocatableMetadataStore> metadata_;
	std::unique_ptr<EntityPageStore> entities_;
	std::unique_ptr<SlottedPagePool> slotted_pages_;
	std::unique_ptr<EntityLabelSetStore> labels_;
	std::unique_ptr<PropertyCollectionRootStore> properties_;

	void SetUp() override {
		path_ = std::filesystem::temp_directory_path() /
				("native_bulk_private_prepare_" + boost::uuids::to_string(boost::uuids::random_generator()()) + ".dat");
		file_ = std::make_shared<std::fstream>(path_,
											   std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(file_->is_open());
		io_ = std::make_shared<StorageIO>(file_, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		metadata_ = RelocatableMetadataStore::initializeNew(io_, initialSuperblock());
		entities_ = std::make_unique<EntityPageStore>(io_, *metadata_);
		slotted_pages_ = std::make_unique<SlottedPagePool>(io_, *metadata_);
		labels_ = std::make_unique<EntityLabelSetStore>(io_, *metadata_, *slotted_pages_);
		properties_ = std::make_unique<PropertyCollectionRootStore>(*slotted_pages_, *metadata_);
	}

	void TearDown() override {
		properties_.reset();
		labels_.reset();
		slotted_pages_.reset();
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
};

TEST_F(NativeStreamedGraphPrivatePreparationTest, PreparesTokenResolverAndBothAdjacencyFamiliesWithoutPublishingRoots) {
	BTreePageStore pages(*slotted_pages_, *metadata_);
	SystemCatalog catalog(pages, *metadata_);
	NativeBulkEdgeRecord first{100U, 1U, 2U, "KNOWS", {{"weight", graph::PropertyValue(int64_t(1U))}}};
	NativeBulkEdgeRecord second{101U, 2U, 3U, "LIKES", {{"cost", graph::PropertyValue(int64_t(2U))}}};
	const NativeBulkEdgeRecordSource records = source({first, second});
	const std::array<NativeGraphLogicalIdLeaseRange, 1U> lease{{{100U, 2U}}};
	const NativeBulkEdgePublicationPreflightOptions preflight{16U, RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES, 2U, 256U};
	const NativeStreamedGraphPublicationPlan plan =
			NativeStreamedGraphPublicationPlan::derive(records, lease, preflight, [](uint64_t) { return true; });
	const NativeStreamedGraphCandidateLayout layout = NativeStreamedGraphCandidateLayout::derive(plan);
	PrivateCandidates candidates(layout.candidate_count);
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	NativeStreamedGraphPrivatePreparationOptions options;
	options.families = {
			16U,
			16U,
			RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES,
			{2U, RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES, 16U, 4U, NativeBulkEdgeRunDuplicatePolicy::NBERDP_REJECT}};
	options.maximum_catalog_entries = 64U;
	NativeStreamedGraphPrivatePreparationCoordinator::prepare(
			records, lease, layout, candidates.identities, catalog, snapshot, options, candidates.reader(),
			[&](const NativeGraphPrivateSegmentMaterialization &identity, std::vector<std::byte> payload) {
				candidates.write(identity, std::move(payload));
			},
			[&] { ++candidates.fences; },
			[&](const NativeStreamedGraphPrivatePreparation &prepared) {
				ASSERT_TRUE(prepared.token_run.has_value());
				EXPECT_EQ(prepared.token_resolver.find(NativeBulkEdgeTokenKind::NBETK_RELATIONSHIP_TYPE, "KNOWS"), 1U);
				EXPECT_EQ(prepared.token_resolver.find(NativeBulkEdgeTokenKind::NBETK_RELATIONSHIP_TYPE, "LIKES"), 2U);
				EXPECT_EQ(prepared.token_resolver.find(NativeBulkEdgeTokenKind::NBETK_PROPERTY, "cost"), 1U);
				EXPECT_EQ(prepared.token_resolver.find(NativeBulkEdgeTokenKind::NBETK_PROPERTY, "weight"), 2U);
				EXPECT_TRUE(prepared.catalog_tokens.catalog_changed);
				EXPECT_TRUE(prepared.adjacency_roots.outgoing.isValid());
				EXPECT_TRUE(prepared.adjacency_roots.incoming.isValid());
			});
	EXPECT_FALSE(metadata_->findIndexRootOptional(snapshot, RSS_SYSTEM_CATALOG_INDEX_ID).has_value());
	EXPECT_GT(candidates.fences, 0U);
	snapshot.release();
}

TEST_F(NativeStreamedGraphPrivatePreparationTest, RejectsIncompletePrivatePreparationContractsBeforeWritingCandidates) {
	BTreePageStore pages(*slotted_pages_, *metadata_);
	SystemCatalog catalog(pages, *metadata_);
	const NativeBulkEdgeRecord record{100U, 1U, 2U, "KNOWS", {}};
	const NativeBulkEdgeRecordSource records = source({record});
	const std::array<NativeGraphLogicalIdLeaseRange, 1U> lease{{{100U, 1U}}};
	const NativeBulkEdgePublicationPreflightOptions preflight{16U, RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES, 2U, 256U};
	const NativeStreamedGraphPublicationPlan plan =
			NativeStreamedGraphPublicationPlan::derive(records, lease, preflight, [](uint64_t) { return true; });
	const NativeStreamedGraphCandidateLayout layout = NativeStreamedGraphCandidateLayout::derive(plan);
	PrivateCandidates candidates(layout.candidate_count);
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	NativeStreamedGraphPrivatePreparationOptions options;
	options.families = {
			16U,
			16U,
			RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES,
			{2U, RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES, 16U, 4U, NativeBulkEdgeRunDuplicatePolicy::NBERDP_REJECT}};
	options.maximum_catalog_entries = 64U;
	const NativeGraphPrivateSegmentWriter writer =
			[](const NativeGraphPrivateSegmentMaterialization &, std::vector<std::byte>) {};
	const NativeGraphPrivateSegmentFence fence = [] {};
	const NativeStreamedGraphPrivatePreparationVisitor visitor =
			[](const NativeStreamedGraphPrivatePreparation &) {};
	const auto invoke = [&](const NativeBulkEdgeRecordSource &candidateSource,
							  std::span<const NativeGraphLogicalIdLeaseRange> candidateLease,
							  const NativeStreamedGraphCandidateLayout &candidateLayout,
							  const NativeStreamedGraphPrivatePreparationOptions &candidateOptions,
							  const NativeGraphPrivateSegmentReader &reader,
							  const NativeGraphPrivateSegmentWriter &candidateWriter,
							  const NativeGraphPrivateSegmentFence &candidateFence,
							  const NativeStreamedGraphPrivatePreparationVisitor &candidateVisitor) {
		NativeStreamedGraphPrivatePreparationCoordinator::prepare(
				candidateSource, candidateLease, candidateLayout, candidates.identities, catalog, snapshot, candidateOptions,
				reader, candidateWriter, candidateFence, candidateVisitor);
	};
	const NativeBulkEdgeRecordSource noSource;
	EXPECT_THROW(invoke(noSource, lease, layout, options, candidates.reader(), writer, fence, visitor), FormatException);
	const std::array<NativeGraphLogicalIdLeaseRange, 0U> noLease{};
	EXPECT_THROW(invoke(records, noLease, layout, options, candidates.reader(), writer, fence, visitor), FormatException);
	NativeStreamedGraphCandidateLayout mismatchedLayout = layout;
	++mismatchedLayout.candidate_count;
	EXPECT_THROW(invoke(records, lease, mismatchedLayout, options, candidates.reader(), writer, fence, visitor),
			 FormatException);
	mismatchedLayout = layout;
	mismatchedLayout.outgoing_adjacency.count = 0U;
	EXPECT_THROW(invoke(records, lease, mismatchedLayout, options, candidates.reader(), writer, fence, visitor),
			 FormatException);
	mismatchedLayout = layout;
	mismatchedLayout.incoming_adjacency.count = 0U;
	EXPECT_THROW(invoke(records, lease, mismatchedLayout, options, candidates.reader(), writer, fence, visitor),
			 FormatException);
	NativeStreamedGraphPrivatePreparationOptions invalidOptions = options;
	invalidOptions.maximum_catalog_entries = 0U;
	EXPECT_THROW(invoke(records, lease, layout, invalidOptions, candidates.reader(), writer, fence, visitor),
			 FormatException);
	invalidOptions = options;
	invalidOptions.families.maximum_token_entries = 0U;
	EXPECT_THROW(invoke(records, lease, layout, invalidOptions, candidates.reader(), writer, fence, visitor),
			 FormatException);
	const NativeGraphPrivateSegmentReader noReader;
	EXPECT_THROW(invoke(records, lease, layout, options, noReader, writer, fence, visitor), FormatException);
	const NativeGraphPrivateSegmentWriter noWriter;
	EXPECT_THROW(invoke(records, lease, layout, options, candidates.reader(), noWriter, fence, visitor), FormatException);
	const NativeGraphPrivateSegmentFence noFence;
	EXPECT_THROW(invoke(records, lease, layout, options, candidates.reader(), writer, noFence, visitor), FormatException);
	const NativeStreamedGraphPrivatePreparationVisitor noVisitor;
	EXPECT_THROW(invoke(records, lease, layout, options, candidates.reader(), writer, fence, noVisitor), FormatException);
	MetadataSnapshotPin releasedSnapshot = metadata_->openSnapshot();
	releasedSnapshot.release();
	EXPECT_THROW(NativeStreamedGraphPrivatePreparationCoordinator::prepare(
					 records, lease, layout, candidates.identities, catalog, releasedSnapshot, options, candidates.reader(),
					 writer, fence, visitor),
			 FormatException);
	for (const auto &payload: candidates.payloads) {
		EXPECT_FALSE(payload.has_value());
	}
	snapshot.release();
}

TEST_F(NativeStreamedGraphPrivatePreparationTest, RejectsTokenRunsWithoutReservedResolverCandidates) {
	BTreePageStore pages(*slotted_pages_, *metadata_);
	SystemCatalog catalog(pages, *metadata_);
	const NativeBulkEdgeRecord first{100U, 1U, 2U, "KNOWS", {{"weight", graph::PropertyValue(int64_t(1U))}}};
	const NativeBulkEdgeRecord second{101U, 2U, 3U, "LIKES", {{"cost", graph::PropertyValue(int64_t(2U))}}};
	const NativeBulkEdgeRecordSource records = source({first, second});
	const std::array<NativeGraphLogicalIdLeaseRange, 1U> lease{{{100U, 2U}}};
	const NativeBulkEdgePublicationPreflightOptions preflight{16U, RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES, 2U, 256U};
	const NativeStreamedGraphPublicationPlan plan =
			NativeStreamedGraphPublicationPlan::derive(records, lease, preflight, [](uint64_t) { return true; });
	NativeStreamedGraphCandidateLayout layout = NativeStreamedGraphCandidateLayout::derive(plan);
	ASSERT_NE(layout.tokens.count, 0U);
	ASSERT_NE(layout.token_resolver.count, 0U);
	layout.token_resolver.count = 0U;
	PrivateCandidates candidates(layout.candidate_count);
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	NativeStreamedGraphPrivatePreparationOptions options;
	options.families = {
			16U,
			16U,
			RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES,
			{2U, RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES, 16U, 4U, NativeBulkEdgeRunDuplicatePolicy::NBERDP_REJECT}};
	options.maximum_catalog_entries = 64U;

	EXPECT_THROW(
			NativeStreamedGraphPrivatePreparationCoordinator::prepare(
					records, lease, layout, candidates.identities, catalog, snapshot, options, candidates.reader(),
					[&candidates](const NativeGraphPrivateSegmentMaterialization &identity, std::vector<std::byte> payload) {
						candidates.write(identity, std::move(payload));
					},
					[] {},
					[](const NativeStreamedGraphPrivatePreparation &) {}),
			FormatException);
	EXPECT_TRUE(candidates.payloads[layout.tokens.first_ordinal].has_value());
	snapshot.release();
}

TEST_F(NativeStreamedGraphPrivatePreparationTest, RejectsResolverCandidatesReservedForAnEmptyTokenStream) {
	BTreePageStore pages(*slotted_pages_, *metadata_);
	SystemCatalog catalog(pages, *metadata_);
	const NativeBulkEdgeRecord record{100U, 1U, 2U, {}, {}};
	const NativeBulkEdgeRecordSource records = source({record});
	const std::array<NativeGraphLogicalIdLeaseRange, 1U> lease{{{100U, 1U}}};
	const NativeBulkEdgePublicationPreflightOptions preflight{16U, RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES, 2U, 256U};
	const NativeStreamedGraphPublicationPlan plan =
			NativeStreamedGraphPublicationPlan::derive(records, lease, preflight, [](uint64_t) { return true; });
	NativeStreamedGraphCandidateLayout layout = NativeStreamedGraphCandidateLayout::derive(plan);
	ASSERT_EQ(plan.token_entry_count, 0U);
	ASSERT_EQ(layout.token_resolver.count, 0U);
	++layout.token_resolver.count;
	++layout.candidate_count;
	PrivateCandidates candidates(layout.candidate_count);
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	NativeStreamedGraphPrivatePreparationOptions options;
	options.families = {
			16U,
			16U,
			RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES,
			{2U, RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES, 16U, 4U, NativeBulkEdgeRunDuplicatePolicy::NBERDP_REJECT}};
	options.maximum_catalog_entries = 64U;
	EXPECT_THROW(NativeStreamedGraphPrivatePreparationCoordinator::prepare(
					 records, lease, layout, candidates.identities, catalog, snapshot, options, candidates.reader(),
					 [&candidates](const NativeGraphPrivateSegmentMaterialization &identity, std::vector<std::byte> payload) {
						 candidates.write(identity, std::move(payload));
					 },
					 [] {}, [](const NativeStreamedGraphPrivatePreparation &) {}),
				 FormatException);
	snapshot.release();
}

TEST_F(NativeStreamedGraphPrivatePreparationTest, RejectsIncompleteDeferredPreparationContractsBeforeWritingCandidates) {
	BTreePageStore pages(*slotted_pages_, *metadata_);
	SystemCatalog catalog(pages, *metadata_);
	const NativeBulkEdgeRecord record{100U, 1U, 2U, "KNOWS", {}};
	const NativeBulkEdgeRecordSource records = source({record});
	const std::array<NativeGraphLogicalIdLeaseRange, 1U> lease{{{100U, 1U}}};
	const NativeBulkEdgePublicationPreflightOptions preflight{16U, RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES, 2U, 256U};
	const NativeStreamedGraphPublicationPlan plan =
			NativeStreamedGraphPublicationPlan::derive(records, lease, preflight, [](uint64_t) { return true; });
	const NativeStreamedGraphCandidateLayout layout = NativeStreamedGraphCandidateLayout::derive(plan);
	PrivateCandidates candidates(layout.candidate_count);
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	NativeStreamedGraphPrivatePreparationOptions options;
	options.families = {
			16U,
			16U,
			RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES,
			{2U, RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES, 16U, 4U, NativeBulkEdgeRunDuplicatePolicy::NBERDP_REJECT}};
	options.maximum_catalog_entries = 64U;
	const NativeGraphPrivateSegmentWriter writer =
			[](const NativeGraphPrivateSegmentMaterialization &, std::vector<std::byte>) {};
	const NativeGraphPrivateSegmentFence fence = [] {};
	const NativeStreamedGraphPreparedPublicationVisitor visitor =
			[](const NativeStreamedGraphPrivatePreparation &, const NativeStreamedGraphDeferredPublication &) {};
	const auto invoke = [&](const NativeStreamedGraphPublicationPlan &candidatePlan,
							  const NativeStreamedGraphCandidateLayout &candidateLayout,
							  const NativeStreamedGraphPreparedPublicationVisitor &candidateVisitor) {
		NativeStreamedGraphPrivatePreparationCoordinator::prepareDeferredPublication(
				records, lease, candidatePlan, candidateLayout, candidates.identities, catalog, snapshot, options,
				candidates.reader(), writer, fence, candidateVisitor);
	};
	const NativeStreamedGraphPreparedPublicationVisitor noVisitor;
	EXPECT_THROW(invoke(plan, layout, noVisitor), FormatException);
	NativeStreamedGraphPublicationPlan missingRecords = plan;
	missingRecords.record_count = 0U;
	EXPECT_THROW(invoke(missingRecords, layout, visitor), FormatException);
	NativeStreamedGraphPublicationPlan missingEntityPages = plan;
	missingEntityPages.edge_entity_page_count = 0U;
	EXPECT_THROW(invoke(missingEntityPages, layout, visitor), FormatException);
	NativeStreamedGraphCandidateLayout mismatchedLayout = layout;
	++mismatchedLayout.candidate_count;
	EXPECT_THROW(invoke(plan, mismatchedLayout, visitor), FormatException);
	for (const auto &payload: candidates.payloads) {
		EXPECT_FALSE(payload.has_value());
	}
	snapshot.release();
}

TEST_F(NativeStreamedGraphPrivatePreparationTest, RejectsLargeValueCandidatesReservedForAnInlinePublication) {
	BTreePageStore pages(*slotted_pages_, *metadata_);
	SystemCatalog catalog(pages, *metadata_);
	const NativeBulkEdgeRecord record{100U, 1U, 2U, {}, {}};
	const NativeBulkEdgeRecordSource records = source({record});
	const std::array<NativeGraphLogicalIdLeaseRange, 1U> lease{{{100U, 1U}}};
	const NativeBulkEdgePublicationPreflightOptions preflight{16U, RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES, 2U, 256U};
	const NativeStreamedGraphPublicationPlan plan =
			NativeStreamedGraphPublicationPlan::derive(records, lease, preflight, [](uint64_t) { return true; });
	NativeStreamedGraphCandidateLayout layout = NativeStreamedGraphCandidateLayout::derive(plan);
	ASSERT_EQ(plan.large_value_count, 0U);
	ASSERT_EQ(layout.large_value_content.count, 0U);
	++layout.large_value_content.count;
	++layout.candidate_count;
	PrivateCandidates candidates(layout.candidate_count);
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	NativeStreamedGraphPrivatePreparationOptions options;
	options.families = {
			16U,
			16U,
			RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES,
			{2U, RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES, 16U, 4U, NativeBulkEdgeRunDuplicatePolicy::NBERDP_REJECT}};
	options.maximum_catalog_entries = 64U;
	EXPECT_THROW(NativeStreamedGraphPrivatePreparationCoordinator::prepareDeferredPublication(
					 records, lease, plan, layout, candidates.identities, catalog, snapshot, options, candidates.reader(),
					 [&candidates](const NativeGraphPrivateSegmentMaterialization &identity, std::vector<std::byte> payload) {
						 candidates.write(identity, std::move(payload));
					 },
					 [] {}, [](const NativeStreamedGraphPrivatePreparation &, const NativeStreamedGraphDeferredPublication &) {}),
				 FormatException);
	snapshot.release();

	// Validate the other side of the disjunction independently: a reference-map
	// reservation is also invalid when the publication has no overflow roots.
	layout = NativeStreamedGraphCandidateLayout::derive(plan);
	++layout.large_value_reference_map.count;
	++layout.candidate_count;
	PrivateCandidates referenceMapCandidates(layout.candidate_count);
	MetadataSnapshotPin referenceMapSnapshot = metadata_->openSnapshot();
	EXPECT_THROW(NativeStreamedGraphPrivatePreparationCoordinator::prepareDeferredPublication(
					 records, lease, plan, layout, referenceMapCandidates.identities, catalog, referenceMapSnapshot, options,
					 referenceMapCandidates.reader(),
					 [&referenceMapCandidates](const NativeGraphPrivateSegmentMaterialization &identity,
													 std::vector<std::byte> payload) {
						 referenceMapCandidates.write(identity, std::move(payload));
					 },
					 [] {}, [](const NativeStreamedGraphPrivatePreparation &, const NativeStreamedGraphDeferredPublication &) {}),
				 FormatException);
	referenceMapSnapshot.release();
}

TEST_F(NativeStreamedGraphPrivatePreparationTest,
	   RejectsIncompleteOrInconsistentLargeValuePrivatePreparationBeforePublication) {
	BTreePageStore pages(*slotted_pages_, *metadata_);
	SystemCatalog catalog(pages, *metadata_);
	LargeValueStore values(io_, *metadata_);
	const NativeBulkEdgeRecord oversized{100U,
									 1U,
									 2U,
									 "REL",
									 {{"body", graph::PropertyValue(graph::PropertyValue::Bytes(
												   RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, std::byte{0x4B}))}}};
	const NativeBulkEdgeRecordSource records = source({oversized});
	const std::array<NativeGraphLogicalIdLeaseRange, 1U> lease{{{100U, 1U}}};
	const NativeBulkEdgePublicationPreflightOptions preflight{16U, RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES, 2U, 256U};
	const NativeStreamedGraphPublicationPlan plan =
			NativeStreamedGraphPublicationPlan::derive(records, lease, preflight, [](uint64_t) { return true; });
	ASSERT_EQ(plan.large_value_count, 1U);
	const NativeStreamedGraphLargeValueContentRunPlan contentRun =
			NativeStreamedGraphLargeValueContentRunBuilder::derivePlanForValueCount(
					values, plan.large_value_count, RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES, 2U);
	const NativeStreamedGraphLargeValueReferenceMapRunPlan referenceMapRun =
			NativeStreamedGraphLargeValueReferenceMapBuilder::derivePlanForValueCount(
					plan.large_value_count, RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES, 2U);
	const NativeStreamedGraphCandidateLayout layout = NativeStreamedGraphCandidateLayout::derive(
			plan, contentRun.candidate_segment_count, referenceMapRun.candidate_segment_count);
	NativeStreamedGraphPrivatePreparationOptions options;
	options.families = {
			16U,
			16U,
			RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES,
			{2U, RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES, 16U, 4U, NativeBulkEdgeRunDuplicatePolicy::NBERDP_REJECT}};
	options.maximum_catalog_entries = 64U;
	options.large_value_store = &values;
	options.large_value_content_options = {RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES, 2U, 16U, 4U};

	const auto expectRejected = [&](const NativeStreamedGraphPublicationPlan &candidatePlan,
									 const NativeStreamedGraphCandidateLayout &candidateLayout,
									 const NativeStreamedGraphPrivatePreparationOptions &candidateOptions) {
		PrivateCandidates candidates(candidateLayout.candidate_count);
		MetadataSnapshotPin snapshot = metadata_->openSnapshot();
		EXPECT_THROW(NativeStreamedGraphPrivatePreparationCoordinator::prepareDeferredPublication(
						 records, lease, candidatePlan, candidateLayout, candidates.identities, catalog, snapshot,
						 candidateOptions, candidates.reader(),
						 [&candidates](const NativeGraphPrivateSegmentMaterialization &identity,
										   std::vector<std::byte> payload) { candidates.write(identity, std::move(payload)); },
						 [] {},
						 [](const NativeStreamedGraphPrivatePreparation &,
							const NativeStreamedGraphDeferredPublication &) {}),
					 FormatException);
		snapshot.release();
	};

	NativeStreamedGraphPrivatePreparationOptions noStore = options;
	noStore.large_value_store = nullptr;
	expectRejected(plan, layout, noStore);
	NativeStreamedGraphCandidateLayout missingContent = layout;
	missingContent.large_value_content.count = 0U;
	expectRejected(plan, missingContent, options);
	NativeStreamedGraphCandidateLayout missingReferenceMap = layout;
	missingReferenceMap.large_value_reference_map.count = 0U;
	expectRejected(plan, missingReferenceMap, options);
	NativeStreamedGraphPublicationPlan mismatchedPreflight = plan;
	++mismatchedPreflight.large_value_count;
	expectRejected(mismatchedPreflight, layout, options);
	NativeStreamedGraphCandidateLayout mismatchedContentLayout = layout;
	++mismatchedContentLayout.large_value_content.count;
	++mismatchedContentLayout.candidate_count;
	expectRejected(plan, mismatchedContentLayout, options);
	NativeStreamedGraphCandidateLayout mismatchedReferenceMapLayout = layout;
	++mismatchedReferenceMapLayout.large_value_reference_map.count;
	++mismatchedReferenceMapLayout.candidate_count;
	expectRejected(plan, mismatchedReferenceMapLayout, options);
}

TEST_F(NativeStreamedGraphPrivatePreparationTest,
	   ExposesOnlyCallbackScopedDeferredEntityAndPropertySourcesAfterPrivatePreparation) {
	BTreePageStore pages(*slotted_pages_, *metadata_);
	SystemCatalog catalog(pages, *metadata_);
	NativeBulkEdgeRecord first{100U, 1U, 2U, "KNOWS", {{"weight", graph::PropertyValue(int64_t(1U))}}};
	NativeBulkEdgeRecord second{101U, 2U, 3U, "LIKES", {{"cost", graph::PropertyValue(int64_t(2U))}}};
	const NativeBulkEdgeRecordSource records = source({first, second});
	const std::array<NativeGraphLogicalIdLeaseRange, 1U> lease{{{100U, 2U}}};
	const NativeBulkEdgePublicationPreflightOptions preflight{16U, RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES, 2U, 256U};
	const NativeStreamedGraphPublicationPlan plan =
			NativeStreamedGraphPublicationPlan::derive(records, lease, preflight, [](uint64_t) { return true; });
	const NativeStreamedGraphCandidateLayout layout = NativeStreamedGraphCandidateLayout::derive(plan);
	PrivateCandidates candidates(layout.candidate_count);
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	NativeStreamedGraphPrivatePreparationOptions options;
	options.families = {
			16U,
			16U,
			RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES,
			{2U, RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES, 16U, 4U, NativeBulkEdgeRunDuplicatePolicy::NBERDP_REJECT}};
	options.maximum_catalog_entries = 64U;
	std::vector<uint64_t> edgeIds;
	std::vector<uint64_t> propertyOwners;
	NativeStreamedGraphPrivatePreparationCoordinator::prepareDeferredPublication(
			records, lease, plan, layout, candidates.identities, catalog, snapshot, options, candidates.reader(),
			[&](const NativeGraphPrivateSegmentMaterialization &identity, std::vector<std::byte> payload) {
				candidates.write(identity, std::move(payload));
			},
			[&] { ++candidates.fences; },
			[&](const NativeStreamedGraphPrivatePreparation &prepared,
				const NativeStreamedGraphDeferredPublication &publication) {
				EXPECT_TRUE(prepared.token_resolver.find(NativeBulkEdgeTokenKind::NBETK_RELATIONSHIP_TYPE, "KNOWS")
									.has_value());
				ASSERT_TRUE(prepared.publication_shape.has_value());
				EXPECT_EQ(prepared.publication_shape->entity_page_count, plan.edge_entity_page_count);
				EXPECT_EQ(prepared.publication_shape->property_root_count, 2U);
				EXPECT_EQ(prepared.publication_shape->large_value_count, 0U);
				EXPECT_EQ(publication.recordCount(), 2U);
				publication.visitEntityPages([&edgeIds](NativeBulkEdgeEntityPageBatch &&batch) {
					for (const NativeBulkEdgeMaterializedRecord &record: batch.records) {
						edgeIds.push_back(record.edge_id);
					}
				});
				publication.visitPropertyRoots([&propertyOwners](PropertyCollectionRoot &&root) {
					propertyOwners.push_back(root.owner_entity_id);
				});
			});
	EXPECT_EQ(edgeIds, (std::vector<uint64_t>{100U, 101U}));
	EXPECT_EQ(propertyOwners, (std::vector<uint64_t>{100U, 101U}));
	snapshot.release();
}

TEST_F(NativeStreamedGraphPrivatePreparationTest, MaterializesLargeValueContentRunsInsideTheSamePrivateCandidateScope) {
	BTreePageStore pages(*slotted_pages_, *metadata_);
	SystemCatalog catalog(pages, *metadata_);
	LargeValueStore values(io_, *metadata_);
	const NativeBulkEdgeRecord oversized{100U,
										 1U,
										 2U,
										 "REL",
										 {{"body", graph::PropertyValue(graph::PropertyValue::Bytes(
														   RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, std::byte{0x4B}))}}};
	const NativeBulkEdgeRecordSource records = source({oversized});
	const std::array<NativeGraphLogicalIdLeaseRange, 1U> lease{{{100U, 1U}}};
	const NativeBulkEdgePublicationPreflightOptions preflight{16U, RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES, 2U, 256U};
	const NativeStreamedGraphPublicationPlan publicationPlan =
			NativeStreamedGraphPublicationPlan::derive(records, lease, preflight, [](uint64_t) { return true; });
	ASSERT_EQ(publicationPlan.large_value_count, 1U);
	const NativeStreamedGraphLargeValueContentRunPlan largeValueRun =
			NativeStreamedGraphLargeValueContentRunBuilder::derivePlanForValueCount(
					values, publicationPlan.large_value_count, RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES, 2U);
	const NativeStreamedGraphLargeValueReferenceMapRunPlan referenceMapRun =
			NativeStreamedGraphLargeValueReferenceMapBuilder::derivePlanForValueCount(
					publicationPlan.large_value_count, RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES, 2U);
	const NativeStreamedGraphCandidateLayout layout =
			NativeStreamedGraphCandidateLayout::derive(publicationPlan, largeValueRun.candidate_segment_count,
															 referenceMapRun.candidate_segment_count);
	PrivateCandidates candidates(layout.candidate_count);
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	NativeStreamedGraphPrivatePreparationOptions options;
	options.families = {
			16U,
			16U,
			RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES,
			{2U, RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES, 16U, 4U, NativeBulkEdgeRunDuplicatePolicy::NBERDP_REJECT}};
	options.maximum_catalog_entries = 64U;
	options.large_value_store = &values;
	options.large_value_content_options = {RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES, 2U, 16U, 4U};
	NativeStreamedGraphPrivatePreparationCoordinator::prepareDeferredPublication(
			records, lease, publicationPlan, layout, candidates.identities, catalog, snapshot, options,
			candidates.reader(),
			[&](const NativeGraphPrivateSegmentMaterialization &identity, std::vector<std::byte> payload) {
				candidates.write(identity, std::move(payload));
			},
			[&] { ++candidates.fences; },
			[&](const NativeStreamedGraphPrivatePreparation &prepared,
				const NativeStreamedGraphDeferredPublication &publication) {
				static_cast<void>(publication);
				ASSERT_TRUE(prepared.large_values.has_value());
				ASSERT_TRUE(prepared.publication_shape.has_value());
				EXPECT_EQ(prepared.publication_shape->large_value_count, 1U);
				EXPECT_EQ(prepared.publication_shape->large_value_data_page_count,
						  prepared.large_values->value_plan.data_page_count);
				EXPECT_EQ(prepared.publication_shape->large_value_manifest_page_count,
						  prepared.large_values->value_plan.manifest_page_count);
				EXPECT_EQ(prepared.large_values->first_value_id, LargeValueId(1U));
				EXPECT_EQ(prepared.large_values->value_plan.value_count, 1U);
				EXPECT_EQ(prepared.large_values->content_run_plan.initial_fragment_count,
						  largeValueRun.initial_fragment_count);
				EXPECT_EQ(prepared.large_values->content_run_plan.candidate_segment_count,
						  largeValueRun.candidate_segment_count);
				EXPECT_EQ(prepared.large_values->reference_map_plan.candidate_segment_count,
						  referenceMapRun.candidate_segment_count);
				EXPECT_TRUE(prepared.large_values->content_run.isValid());
			});
	snapshot.release();
}
