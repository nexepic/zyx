/**
 * @file test_NativeStreamedGraphEntityPageMaterializer.cpp
 * @brief Bounded fixed Entity-page streamed materialization tests.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "graph/storage/relocatable/entity/NativeStreamedGraphEntityPageMaterializer.hpp"

using namespace graph::storage;

namespace {

	[[nodiscard]] NativeBulkEdgeRecord record(uint64_t edgeId) {
		return {edgeId, edgeId, edgeId + 1U, "REL", {}};
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
					return name == "REL" ? std::optional<uint64_t>(19U) : std::nullopt;
				},
				[](std::string_view) -> std::optional<uint32_t> { return std::nullopt; }};
	}

	[[nodiscard]] NativeStreamedGraphDeferredPublication publication(std::vector<NativeBulkEdgeRecord> records) {
		const NativeGraphLogicalIdLeaseRange lease{records.front().edge_id, static_cast<uint64_t>(records.size())};
		NativeStreamedGraphPublicationPlan plan;
		plan.record_count = records.size();
		plan.first_edge_id = records.front().edge_id;
		plan.last_edge_id = records.back().edge_id;
		plan.edge_entity_page_count =
				(records.size() - 1U) / EntityPage::recordCapacity(EntityPageRecordKind::EPRK_EDGE) + 1U;
		return NativeStreamedGraphDeferredPublication::create(source(std::move(records)), std::span(&lease, 1U), plan,
													 resolvers());
	}

	[[nodiscard]] PageOwnerHeader owner(size_t ordinal) {
		return {PageId(100U + ordinal), 1U, PageOwnerKind::POK_ENTITY, 0U, RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
				Generation(1U), LogSequenceNumber(9U)};
	}

} // namespace

TEST(NativeStreamedGraphEntityPageMaterializerTest, EmitsCodecCheckedPagesAndCanonicalEdgeMappings) {
	const size_t capacity = EntityPage::recordCapacity(EntityPageRecordKind::EPRK_EDGE);
	std::vector<NativeBulkEdgeRecord> records;
	for (size_t ordinal = 0U; ordinal < capacity + 1U; ++ordinal) {
		records.push_back(record(100U + ordinal));
	}
	const NativeStreamedGraphDeferredPublication deferred = publication(records);
	std::vector<NativeStreamedGraphEntityPage> pages;
	NativeStreamedGraphEntityPageMaterializer::materialize(
			deferred, [](size_t ordinal) { return NativeStreamedGraphEntityPageAssignment{owner(ordinal)}; },
			[&pages](NativeStreamedGraphEntityPage &&page) { pages.push_back(std::move(page)); });

	ASSERT_EQ(pages.size(), 2U);
	EXPECT_EQ(pages.front().edge_locations.front().entity_id, 100U);
	EXPECT_EQ(pages.back().edge_locations.front().entity_id, 100U + capacity);
	for (const NativeStreamedGraphEntityPage &page: pages) {
		const EntityPage decoded = EntityPage::open(page.owner, page.payload);
		EXPECT_EQ(decoded.header().record_kind, EntityPageRecordKind::EPRK_EDGE);
		for (const EntityPageLocation &location: page.edge_locations) {
			EXPECT_TRUE(decoded.findEdge(location.slot_id, location.slot_generation).has_value());
		}
	}
}

TEST(NativeStreamedGraphEntityPageMaterializerTest, RejectsAnIncompleteMetadataAssignment) {
	const NativeStreamedGraphDeferredPublication deferred = publication({record(100U)});
	EXPECT_THROW(
			NativeStreamedGraphEntityPageMaterializer::materialize(
					deferred,
					[](size_t) { return NativeStreamedGraphEntityPageAssignment{}; },
					[](NativeStreamedGraphEntityPage &&) {}),
			FormatException);
}
