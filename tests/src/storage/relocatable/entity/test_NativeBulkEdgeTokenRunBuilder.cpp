/**
 * @file test_NativeBulkEdgeTokenRunBuilder.cpp
 * @brief Bounded token-name run construction regressions for streamed Edges.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#include <algorithm>
#include <limits>
#include <optional>
#include <vector>

#include <gtest/gtest.h>

#include "graph/storage/relocatable/entity/NativeBulkEdgeTokenRunBuilder.hpp"
#include "graph/storage/relocatable/schema/SystemCatalog.hpp"

using namespace graph::storage;

namespace {

	NativeBulkEdgeRecord tokenRecord(uint64_t id, std::string type, std::vector<std::string> propertyNames) {
		NativeBulkEdgeRecord record;
		record.edge_id = id;
		record.source_node_id = 1U;
		record.target_node_id = 2U;
		record.type = std::move(type);
		for (std::string &name: propertyNames) {
			record.properties.push_back({std::move(name), graph::PropertyValue(int64_t(id))});
		}
		return record;
	}

	NativeBulkEdgeRecordSource source(std::vector<NativeBulkEdgeRecord> records) {
		return [records = std::move(records)]() mutable {
			size_t ordinal = 0U;
			return [records, ordinal]() mutable -> std::optional<NativeBulkEdgeRecord> {
				if (ordinal == records.size()) {
					return std::nullopt;
				}
				return records[ordinal++];
			};
		};
	}

} // namespace

TEST(NativeBulkEdgeTokenRunBuilderTest, SortsAndDeduplicatesRelationshipAndPropertyNamesWithinBoundedRuns) {
	std::vector<NativeBulkRunFragment> fragments;
	NativeBulkEdgeTokenRunBuilder::buildRuns(
			source({tokenRecord(1U, "REL_B", {"weight", "name"}), tokenRecord(2U, "REL_A", {"name", "age"}),
					tokenRecord(3U, "REL_B", {"age"})}),
			3U, RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES, 4U,
			[&fragments](NativeBulkRunFragment &&fragment) { fragments.push_back(std::move(fragment)); });
	ASSERT_EQ(fragments.size(), 1U);
	const std::vector<BTreePageEntry> &entries = fragments.front().entries;
	ASSERT_EQ(entries.size(), 5U);
	EXPECT_TRUE(std::ranges::is_sorted(entries, {}, &BTreePageEntry::key));
	EXPECT_EQ(entries[0].key,
			  NativeBulkEdgeTokenRunBuilder::encodeKey(NativeBulkEdgeTokenKind::NBETK_PROPERTY, "age"));
	EXPECT_EQ(entries[1].key,
			  NativeBulkEdgeTokenRunBuilder::encodeKey(NativeBulkEdgeTokenKind::NBETK_PROPERTY, "name"));
	EXPECT_EQ(entries[2].key,
			  NativeBulkEdgeTokenRunBuilder::encodeKey(NativeBulkEdgeTokenKind::NBETK_PROPERTY, "weight"));
	EXPECT_EQ(entries[3].key,
			  NativeBulkEdgeTokenRunBuilder::encodeKey(NativeBulkEdgeTokenKind::NBETK_RELATIONSHIP_TYPE, "REL_A"));
	EXPECT_EQ(entries[4].key,
			  NativeBulkEdgeTokenRunBuilder::encodeKey(NativeBulkEdgeTokenKind::NBETK_RELATIONSHIP_TYPE, "REL_B"));
}

TEST(NativeBulkEdgeTokenRunBuilderTest, RejectsInvalidNamesAndCandidateFragmentOverflow) {
	EXPECT_THROW(static_cast<void>(NativeBulkEdgeTokenRunBuilder::encodeKey(
			static_cast<NativeBulkEdgeTokenKind>(0U), "REL")), FormatException);
	EXPECT_THROW(static_cast<void>(NativeBulkEdgeTokenRunBuilder::encodeKey(NativeBulkEdgeTokenKind::NBETK_PROPERTY, "")),
				 FormatException);
	EXPECT_THROW(static_cast<void>(NativeBulkEdgeTokenRunBuilder::encodeKey(
								  NativeBulkEdgeTokenKind::NBETK_PROPERTY,
								  std::string(RSS_SYSTEM_CATALOG_NAME_MAX_BYTES + 1U, 'x'))),
			 FormatException);

	const size_t oneEntry = RSS_NATIVE_BULK_RUN_HEADER_BYTES +
							NativeBulkEdgeTokenRunBuilder::encodeKey(NativeBulkEdgeTokenKind::NBETK_RELATIONSHIP_TYPE, "REL").size() +
							sizeof(uint32_t) + sizeof(uint64_t);
	EXPECT_THROW(NativeBulkEdgeTokenRunBuilder::buildRuns(
					 source({tokenRecord(1U, "REL_A", {}), tokenRecord(2U, "REL_B", {})}), 2U,
					 RSS_NATIVE_BULK_RUN_HEADER_BYTES + oneEntry, 1U,
					 [](NativeBulkRunFragment &&) {}),
				 FormatException);
	EXPECT_THROW(NativeBulkEdgeTokenRunBuilder::buildRuns(
					 source({tokenRecord(1U, "REL", {})}), 1U, RSS_NATIVE_BULK_RUN_HEADER_BYTES + 1U, 1U,
					 [](NativeBulkRunFragment &&) {}),
				 FormatException);
	const auto sourceWithOneRecord = source({tokenRecord(1U, "REL", {})});
	const auto visitor = [](NativeBulkRunFragment &&) {};
	EXPECT_THROW(NativeBulkEdgeTokenRunBuilder::buildRuns(
					 sourceWithOneRecord, std::numeric_limits<size_t>::max(), RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES,
					 1U, visitor),
			 FormatException);
	EXPECT_THROW(NativeBulkEdgeTokenRunBuilder::buildRuns(
					 sourceWithOneRecord, 1U, std::numeric_limits<size_t>::max(), 1U, visitor),
			 FormatException);
	EXPECT_THROW(NativeBulkEdgeTokenRunBuilder::buildRuns(
					 sourceWithOneRecord, 1U, RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES,
					 std::numeric_limits<size_t>::max(), visitor),
				 FormatException);
}

TEST(NativeBulkEdgeTokenRunBuilderTest, RejectsMissingSourceVisitorAndNonFiniteBounds) {
	const auto validSource = source({tokenRecord(1U, "REL", {})});
	const auto visitor = [](NativeBulkRunFragment &&) {};
	EXPECT_THROW(NativeBulkEdgeTokenRunBuilder::buildRuns({}, 1U, RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES, 1U, visitor),
				 FormatException);
	EXPECT_THROW(NativeBulkEdgeTokenRunBuilder::buildRuns(validSource, 0U, RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES, 1U,
								 visitor),
				 FormatException);
	EXPECT_THROW(NativeBulkEdgeTokenRunBuilder::buildRuns(validSource, 1U, RSS_NATIVE_BULK_RUN_HEADER_BYTES, 1U,
								 visitor),
				 FormatException);
	EXPECT_THROW(NativeBulkEdgeTokenRunBuilder::buildRuns(validSource, 1U, RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES, 0U,
								 visitor),
				 FormatException);
	EXPECT_THROW(NativeBulkEdgeTokenRunBuilder::buildRuns(validSource, 1U, RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES, 1U,
								 {}),
				 FormatException);
	EXPECT_THROW(NativeBulkEdgeTokenRunBuilder::buildRuns(validSource, std::numeric_limits<size_t>::max(),
								 RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES, 1U, visitor),
				 FormatException);
}

TEST(NativeBulkEdgeTokenRunBuilderTest, RejectsEmptyAndUnorderedSealedSources) {
	const auto visitor = [](NativeBulkRunFragment &&) {};
	EXPECT_THROW(NativeBulkEdgeTokenRunBuilder::buildRuns(source({}), 1U, RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES, 1U,
								 visitor),
				 FormatException);
	EXPECT_THROW(NativeBulkEdgeTokenRunBuilder::buildRuns(
				 source({tokenRecord(0U, "REL", {})}), 1U, RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES, 1U, visitor),
				 FormatException);
	EXPECT_THROW(NativeBulkEdgeTokenRunBuilder::buildRuns(
				 source({tokenRecord(2U, "REL", {}), tokenRecord(1U, "REL", {})}), 2U,
				 RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES, 1U, visitor),
				 FormatException);
}

TEST(NativeBulkEdgeTokenRunBuilderTest, AcceptsRecordsWithoutTokenNames) {
	bool called = false;
	NativeBulkEdgeTokenRunBuilder::buildRuns(
			source({tokenRecord(1U, "", {})}), 1U, RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES, 1U,
			[&called](NativeBulkRunFragment &&) { called = true; });
	EXPECT_FALSE(called);
}

TEST(NativeBulkEdgeTokenRunBuilderTest, RejectsOversizedPropertyTokenNames) {
	const std::string oversized(RSS_SYSTEM_CATALOG_NAME_MAX_BYTES + 1U, 'p');
	EXPECT_THROW(NativeBulkEdgeTokenRunBuilder::buildRuns(
				 source({tokenRecord(1U, "REL", {oversized})}), 1U, RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES, 1U,
				 [](NativeBulkRunFragment &&) {}),
				 FormatException);
}
