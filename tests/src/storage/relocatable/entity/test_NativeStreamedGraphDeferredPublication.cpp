/**
 * @file test_NativeStreamedGraphDeferredPublication.cpp
 * @brief Callback-scoped streamed Edge final-page source tests.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#include <array>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "graph/core/Edge.hpp"
#include "graph/storage/relocatable/entity/NativeStreamedGraphDeferredPublication.hpp"

using namespace graph::storage;

namespace {

	[[nodiscard]] NativeBulkEdgeRecord record(uint64_t edgeId) {
		return {edgeId, edgeId, edgeId + 1U, "REL", {{"weight", graph::PropertyValue(int64_t(edgeId))}}};
	}

	[[nodiscard]] NativeBulkEdgeRecordSource source(std::vector<NativeBulkEdgeRecord> records) {
		return [records = std::move(records)] {
			return [records, ordinal = size_t{0U}]() mutable -> std::optional<NativeBulkEdgeRecord> {
				return ordinal == records.size() ? std::nullopt
												 : std::optional<NativeBulkEdgeRecord>(records[ordinal++]);
			};
		};
	}

	[[nodiscard]] NativeBulkEdgeMaterializerResolvers resolvers() {
		return {[](std::string_view name) -> std::optional<uint64_t> {
					return name == "REL" ? std::optional<uint64_t>(17U) : std::nullopt;
				},
				[](std::string_view name) -> std::optional<uint32_t> {
					return name == "weight" ? std::optional<uint32_t>(9U) : std::nullopt;
				}};
	}

} // namespace

TEST(NativeStreamedGraphDeferredPublicationTest, RewindsBoundedEntityPagesAndPropertyRootsWithoutRetainingRecords) {
	const size_t capacity = EntityPage::recordCapacity(EntityPageRecordKind::EPRK_EDGE);
	ASSERT_GT(capacity, 0U);
	std::vector<NativeBulkEdgeRecord> records;
	for (size_t ordinal = 0U; ordinal < capacity + 1U; ++ordinal) {
		records.push_back(record(100U + ordinal));
	}
	const NativeGraphLogicalIdLeaseRange lease{100U, static_cast<uint64_t>(records.size())};
	NativeStreamedGraphPublicationPlan shape;
	shape.record_count = records.size();
	shape.first_edge_id = records.front().edge_id;
	shape.last_edge_id = records.back().edge_id;
	shape.edge_entity_page_count = 2U;
	const NativeStreamedGraphDeferredPublication publication =
			NativeStreamedGraphDeferredPublication::create(source(records), std::span(&lease, 1U), shape, resolvers());
	EXPECT_EQ(publication.recordCount(), records.size());
	EXPECT_EQ(publication.firstEdgeId(), records.front().edge_id);
	EXPECT_EQ(publication.lastEdgeId(), records.back().edge_id);
	EXPECT_EQ(publication.entityPageCount(), shape.edge_entity_page_count);

	std::vector<uint64_t> entityIds;
	publication.visitEntityPages([&entityIds](NativeBulkEdgeEntityPageBatch &&batch) {
		for (const NativeBulkEdgeMaterializedRecord &materialized: batch.records) {
			entityIds.push_back(materialized.edge_id);
			EXPECT_EQ(materialized.entity.type_id, 17U);
		}
	});
	ASSERT_EQ(entityIds.size(), records.size());
	EXPECT_EQ(entityIds.front(), 100U);
	EXPECT_EQ(entityIds.back(), 100U + records.size() - 1U);

	std::vector<PropertyCollectionRoot> roots;
	publication.visitPropertyRoots([&roots](PropertyCollectionRoot &&root) { roots.push_back(std::move(root)); });
	ASSERT_EQ(roots.size(), records.size());
	EXPECT_EQ(roots.front().owner_entity_id, 100U);
	EXPECT_EQ(roots.front().owner_entity_type, graph::Edge::typeId);
	ASSERT_EQ(roots.front().entries.size(), 1U);
	EXPECT_EQ(roots.front().entries.front().key_token, 9U);
}

TEST(NativeStreamedGraphDeferredPublicationTest, RejectsSourceDivergenceFromItsImmutableShape) {
	const NativeGraphLogicalIdLeaseRange lease{100U, 2U};
	NativeStreamedGraphPublicationPlan shape;
	shape.record_count = 2U;
	shape.first_edge_id = 100U;
	shape.last_edge_id = 101U;
	shape.edge_entity_page_count = 1U;
	const NativeStreamedGraphDeferredPublication publication = NativeStreamedGraphDeferredPublication::create(
			source({record(100U)}), std::span(&lease, 1U), shape, resolvers());
	EXPECT_THROW(publication.visitEntityPages([](NativeBulkEdgeEntityPageBatch &&) {}), FormatException);
	EXPECT_THROW(publication.visitPropertyRoots([](PropertyCollectionRoot &&) {}), FormatException);
}

TEST(NativeStreamedGraphDeferredPublicationTest, RejectsAReplayWithDifferentLeaseOwnedSpan) {
	const NativeGraphLogicalIdLeaseRange lease{100U, 3U};
	NativeStreamedGraphPublicationPlan shape;
	shape.record_count = 2U;
	shape.first_edge_id = 100U;
	shape.last_edge_id = 101U;
	shape.edge_entity_page_count = 1U;
	const NativeStreamedGraphDeferredPublication publication = NativeStreamedGraphDeferredPublication::create(
			source({record(101U), record(102U)}), std::span(&lease, 1U), shape, resolvers());
	EXPECT_THROW(publication.visitEntityPages([](NativeBulkEdgeEntityPageBatch &&) {}), FormatException);
	EXPECT_THROW(publication.visitPropertyRoots([](PropertyCollectionRoot &&) {}), FormatException);
}

TEST(NativeStreamedGraphDeferredPublicationTest, RejectsADeclaredEntityPageCountThatDisagreesWithTheCodec) {
	const NativeGraphLogicalIdLeaseRange lease{100U, 1U};
	NativeStreamedGraphPublicationPlan shape;
	shape.record_count = 1U;
	shape.first_edge_id = 100U;
	shape.last_edge_id = 100U;
	shape.edge_entity_page_count = 2U;
	EXPECT_THROW(static_cast<void>(NativeStreamedGraphDeferredPublication::create(
						 source({record(100U)}), std::span(&lease, 1U), shape, resolvers())),
				 FormatException);
}

TEST(NativeStreamedGraphDeferredPublicationTest, RejectsADeclaredSpanOutsideItsDurableLease) {
	const NativeGraphLogicalIdLeaseRange lease{100U, 1U};
	NativeStreamedGraphPublicationPlan shape;
	shape.record_count = 1U;
	shape.first_edge_id = 99U;
	shape.last_edge_id = 99U;
	shape.edge_entity_page_count = 1U;
	EXPECT_THROW(static_cast<void>(NativeStreamedGraphDeferredPublication::create(
						 source({record(100U)}), std::span(&lease, 1U), shape, resolvers())),
				 FormatException);

	shape.first_edge_id = 100U;
	shape.last_edge_id = 101U;
	EXPECT_THROW(static_cast<void>(NativeStreamedGraphDeferredPublication::create(
						 source({record(100U)}), std::span(&lease, 1U), shape, resolvers())),
				 FormatException);
}

TEST(NativeStreamedGraphDeferredPublicationTest, RejectsMissingMaterializationVisitors) {
	const NativeGraphLogicalIdLeaseRange lease{100U, 1U};
	NativeStreamedGraphPublicationPlan shape;
	shape.record_count = 1U;
	shape.first_edge_id = 100U;
	shape.last_edge_id = 100U;
	shape.edge_entity_page_count = 1U;
	const NativeStreamedGraphDeferredPublication publication = NativeStreamedGraphDeferredPublication::create(
			source({record(100U)}), std::span(&lease, 1U), shape, resolvers());
	const NativeBulkEdgeEntityPageBatchVisitor noEntityVisitor;
	const NativeStreamedGraphPropertyRootVisitor noPropertyVisitor;
	EXPECT_THROW(publication.visitEntityPages(noEntityVisitor), FormatException);
	EXPECT_THROW(publication.visitPropertyRoots(noPropertyVisitor), FormatException);
}

TEST(NativeStreamedGraphDeferredPublicationTest, RejectsIncompleteCreationContractsBeforeOpeningTheSource) {
	const NativeGraphLogicalIdLeaseRange lease{100U, 1U};
	const auto invoke = [&](const NativeBulkEdgeRecordSource &records, NativeStreamedGraphPublicationPlan shape,
							NativeBulkEdgeMaterializerResolvers materializers,
							std::span<const NativeGraphLogicalIdLeaseRange> leases) {
		EXPECT_THROW(static_cast<void>(
							 NativeStreamedGraphDeferredPublication::create(records, leases, shape, materializers)),
					 FormatException);
	};

	NativeStreamedGraphPublicationPlan valid;
	valid.record_count = 1U;
	valid.edge_entity_page_count = 1U;
	NativeBulkEdgeMaterializerResolvers validResolvers = resolvers();
	const NativeBulkEdgeRecordSource validSource = source({record(100U)});
	invoke({}, valid, validResolvers, std::span(&lease, 1U));
	NativeStreamedGraphPublicationPlan noRecords = valid;
	noRecords.record_count = 0U;
	invoke(validSource, noRecords, validResolvers, std::span(&lease, 1U));
	NativeStreamedGraphPublicationPlan noPages = valid;
	noPages.edge_entity_page_count = 0U;
	invoke(validSource, noPages, validResolvers, std::span(&lease, 1U));
	NativeBulkEdgeMaterializerResolvers noRelationship = validResolvers;
	noRelationship.relationship_type_token = {};
	invoke(validSource, valid, noRelationship, std::span(&lease, 1U));
	NativeBulkEdgeMaterializerResolvers noProperty = validResolvers;
	noProperty.property_key_token = {};
	invoke(validSource, valid, noProperty, std::span(&lease, 1U));
	invoke(validSource, valid, validResolvers, {});
}

TEST(NativeStreamedGraphDeferredPublicationTest, RejectsMalformedOrderedLeases) {
	NativeStreamedGraphPublicationPlan shape;
	shape.record_count = 1U;
	shape.edge_entity_page_count = 1U;
	const NativeBulkEdgeRecordSource records = source({record(100U)});
	const NativeBulkEdgeMaterializerResolvers materializers = resolvers();
	const auto reject = [&](std::span<const NativeGraphLogicalIdLeaseRange> leases) {
		EXPECT_THROW(static_cast<void>(
							 NativeStreamedGraphDeferredPublication::create(records, leases, shape, materializers)),
					 FormatException);
	};
	const NativeGraphLogicalIdLeaseRange zeroFirst{0U, 1U};
	reject(std::span(&zeroFirst, 1U));
	const NativeGraphLogicalIdLeaseRange zeroCount{100U, 0U};
	reject(std::span(&zeroCount, 1U));
	const NativeGraphLogicalIdLeaseRange overflowing{std::numeric_limits<uint64_t>::max(), 2U};
	reject(std::span(&overflowing, 1U));
	const std::array<NativeGraphLogicalIdLeaseRange, 2U> unordered{{{102U, 1U}, {101U, 1U}}};
	reject(unordered);
}

TEST(NativeStreamedGraphDeferredPublicationTest, RejectsIncompleteAndNoncanonicalSourceSpans) {
	const NativeGraphLogicalIdLeaseRange lease{100U, 1U};
	const NativeBulkEdgeMaterializerResolvers materializers = resolvers();
	NativeStreamedGraphPublicationPlan shape;
	shape.record_count = 1U;
	shape.edge_entity_page_count = 1U;
	shape.first_edge_id = 0U;
	shape.last_edge_id = 100U;
	EXPECT_THROW(static_cast<void>(NativeStreamedGraphDeferredPublication::create(
						 source({record(100U)}), std::span(&lease, 1U), shape, materializers)),
				 FormatException);
	shape.first_edge_id = 100U;
	shape.last_edge_id = 0U;
	EXPECT_THROW(static_cast<void>(NativeStreamedGraphDeferredPublication::create(
						 source({record(100U)}), std::span(&lease, 1U), shape, materializers)),
				 FormatException);

	NativeStreamedGraphPublicationPlan inferred = shape;
	inferred.first_edge_id = 0U;
	inferred.last_edge_id = 0U;
	const std::array<NativeGraphLogicalIdLeaseRange, 2U> sparseLeases{{{100U, 1U}, {102U, 1U}}};
	NativeBulkEdgeRecord zero = record(100U);
	zero.edge_id = 0U;
	EXPECT_THROW(static_cast<void>(NativeStreamedGraphDeferredPublication::create(source({zero}), sparseLeases,
																				  inferred, materializers)),
				 FormatException);
	EXPECT_THROW(static_cast<void>(NativeStreamedGraphDeferredPublication::create(
						 source({record(100U), record(101U)}), sparseLeases, inferred, materializers)),
				 FormatException);
}

TEST(NativeStreamedGraphDeferredPublicationTest, RejectsLegacySparseLeaseSourcesWithDuplicateOrTruncatedRecords) {
	const std::array<NativeGraphLogicalIdLeaseRange, 2U> leases = {NativeGraphLogicalIdLeaseRange{100U, 1U},
																   NativeGraphLogicalIdLeaseRange{102U, 1U}};
	const NativeBulkEdgeMaterializerResolvers materializers = resolvers();
	const NativeStreamedGraphPublicationPlan shape{2U, 1U};
	EXPECT_THROW(static_cast<void>(NativeStreamedGraphDeferredPublication::create(source({record(102U), record(102U)}),
																				  leases, shape, materializers)),
				 FormatException);
	EXPECT_THROW(static_cast<void>(NativeStreamedGraphDeferredPublication::create(source({record(100U)}), leases, shape,
																				  materializers)),
				 FormatException);
}

TEST(NativeStreamedGraphDeferredPublicationTest, RejectsAReplayThatChangesOnlyItsLastEdgeId) {
	const NativeGraphLogicalIdLeaseRange lease{100U, 3U};
	NativeStreamedGraphPublicationPlan shape;
	shape.record_count = 2U;
	shape.first_edge_id = 100U;
	shape.last_edge_id = 101U;
	shape.edge_entity_page_count = 1U;
	const NativeStreamedGraphDeferredPublication publication = NativeStreamedGraphDeferredPublication::create(
			source({record(100U), record(102U)}), std::span(&lease, 1U), shape, resolvers());
	EXPECT_THROW(publication.visitEntityPages([](NativeBulkEdgeEntityPageBatch &&) {}), FormatException);
	EXPECT_THROW(publication.visitPropertyRoots([](PropertyCollectionRoot &&) {}), FormatException);
}

TEST(NativeStreamedGraphDeferredPublicationTest, DefersLegacySingleLeaseSourceReadsUntilMaterialization) {
	const auto sourceOpenCount = std::make_shared<size_t>(0U);
	const NativeBulkEdgeRecordSource deferredSource = [sourceOpenCount] {
		++*sourceOpenCount;
		return [emitted = false]() mutable -> std::optional<NativeBulkEdgeRecord> {
			if (emitted) {
				return std::nullopt;
			}
			emitted = true;
			return record(100U);
		};
	};
	const NativeGraphLogicalIdLeaseRange lease{100U, 1U};
	const NativeStreamedGraphPublicationPlan shape{1U, 1U};
	const NativeStreamedGraphDeferredPublication publication =
			NativeStreamedGraphDeferredPublication::create(deferredSource, std::span(&lease, 1U), shape, resolvers());
	EXPECT_EQ(*sourceOpenCount, 0U);

	publication.visitPropertyRoots([](PropertyCollectionRoot &&) {});
	EXPECT_EQ(*sourceOpenCount, 1U);
}

TEST(NativeStreamedGraphDeferredPublicationTest, DerivesLegacySparseLeaseSpanByVerifyingTheSource) {
	const std::array<NativeGraphLogicalIdLeaseRange, 2U> leases = {NativeGraphLogicalIdLeaseRange{100U, 1U},
																   NativeGraphLogicalIdLeaseRange{102U, 1U}};
	const NativeStreamedGraphPublicationPlan shape{2U, 1U};
	const NativeStreamedGraphDeferredPublication publication = NativeStreamedGraphDeferredPublication::create(
			source({record(100U), record(102U)}), leases, shape, resolvers());
	EXPECT_EQ(publication.firstEdgeId(), 100U);
	EXPECT_EQ(publication.lastEdgeId(), 102U);

	size_t rootCount = 0U;
	publication.visitPropertyRoots([&rootCount](PropertyCollectionRoot &&) { ++rootCount; });
	EXPECT_EQ(rootCount, 2U);
}
