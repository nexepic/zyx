/**
 * @file test_NativeStreamedGraphPropertyPageMaterializer.cpp
 * @brief Focused bounded materialization tests for streamed Edge Property pages.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#include <array>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "graph/core/Edge.hpp"
#include "graph/core/Node.hpp"
#include "graph/storage/relocatable/allocation/SlottedPage.hpp"
#include "graph/storage/relocatable/entity/NativeStreamedGraphPropertyPageMaterializer.hpp"

namespace graph::storage {

	namespace {

		[[nodiscard]] NativeStreamedGraphDeferredPublication
		publicationFor(const std::vector<NativeBulkEdgeRecord> &records) {
			const NativeBulkEdgeRecordSource source = [records] {
				return [records, ordinal = size_t{0U}]() mutable -> std::optional<NativeBulkEdgeRecord> {
					return ordinal == records.size() ? std::nullopt
													 : std::optional<NativeBulkEdgeRecord>(records[ordinal++]);
				};
			};
			NativeStreamedGraphPublicationPlan plan;
			plan.record_count = records.size();
			plan.first_edge_id = records.front().edge_id;
			plan.last_edge_id = records.back().edge_id;
			plan.edge_entity_page_count = 1U;
			const std::array<NativeGraphLogicalIdLeaseRange, 1U> edgeIdRanges = {
					NativeGraphLogicalIdLeaseRange{records.front().edge_id, static_cast<uint64_t>(records.size())}};
			return NativeStreamedGraphDeferredPublication::create(
					source, edgeIdRanges, plan,
					{[](std::string_view name) -> std::optional<uint64_t> {
						 return name == "REL" ? std::optional<uint64_t>(1U) : std::nullopt;
					 },
					 [](std::string_view name) -> std::optional<uint32_t> {
						 return name == "weight" ? std::optional<uint32_t>(1U) : std::nullopt;
					 }});
		}

	} // namespace

	TEST(NativeStreamedGraphPropertyPageMaterializerTest, StreamsInlineRootsIntoOneSlottedPage) {
		const NativeStreamedGraphDeferredPublication publication = publicationFor({
				{11U, 1U, 2U, "REL", {{"weight", PropertyValue(int64_t{7})}}},
				{12U, 2U, 3U, "REL", {}},
				{13U, 3U, 4U, "REL", {{"weight", PropertyValue(int64_t{9})}}},
		});
		const NativeStreamedGraphInlinePropertyPageCount measured =
				NativeStreamedGraphPropertyPageMaterializer::measureInlinePages(publication);
		EXPECT_EQ(measured.page_count, 1U);
		EXPECT_EQ(measured.property_count, 2U);

		std::vector<NativeStreamedGraphPropertyPage> pages;
		NativeStreamedGraphPropertyPageMaterializer::materializeInlinePages(
				publication,
				[](size_t ordinal) {
					return NativeStreamedGraphPropertyPageAssignment{
							{PageId(101U + ordinal), 99U, PageOwnerKind::POK_PROPERTY, 0U,
							 RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, Generation(1U), LogSequenceNumber(2U)}};
				},
				[&pages](NativeStreamedGraphPropertyPage &&page) { pages.push_back(std::move(page)); });

		ASSERT_EQ(pages.size(), 1U);
		EXPECT_EQ(pages.front().property_locations.size(), 2U);
		EXPECT_EQ(pages.front().property_locations[0].key,
				  (EntityPropertyKey{EntityPropertyOwnerKind::EPOK_EDGE, 11U}));
		EXPECT_EQ(pages.front().property_locations[1].key,
				  (EntityPropertyKey{EntityPropertyOwnerKind::EPOK_EDGE, 13U}));
		const SlottedPage page = SlottedPage::open(pages.front().owner, pages.front().payload);
		EXPECT_EQ(page.stats().live_slot_count, 2U);
		EXPECT_EQ(pages.front().free_bytes, page.stats().contiguous_free_bytes);
	}

	TEST(NativeStreamedGraphPropertyPageMaterializerTest, DoesNotMaterializePropertyPagesForEdgesWithoutProperties) {
		const NativeStreamedGraphDeferredPublication publication = publicationFor({
				{11U, 1U, 2U, "REL", {}},
				{12U, 2U, 3U, "REL", {}},
		});

		const NativeStreamedGraphInlinePropertyPageCount measured =
				NativeStreamedGraphPropertyPageMaterializer::measureInlinePages(publication);
		EXPECT_EQ(measured.page_count, 0U);
		EXPECT_EQ(measured.property_count, 0U);

		size_t assignedPages = 0U;
		size_t publishedPages = 0U;
		NativeStreamedGraphPropertyPageMaterializer::materializeInlinePages(
				publication,
				[&assignedPages](size_t) {
					++assignedPages;
					return NativeStreamedGraphPropertyPageAssignment{};
				},
				[&publishedPages](NativeStreamedGraphPropertyPage &&) { ++publishedPages; });

		EXPECT_EQ(assignedPages, 0U);
		EXPECT_EQ(publishedPages, 0U);
	}

	TEST(NativeStreamedGraphPropertyPageMaterializerTest, StreamsInlineRootsAcrossSlottedPages) {
		// Each root remains below the inline threshold, while five roots exceed one
		// Property page. This is distinct from a single oversized root, which uses
		// the same-generation LargeValue path.
		const std::string payload(1024U, 'x');
		const NativeStreamedGraphDeferredPublication publication = publicationFor({
				{11U, 1U, 2U, "REL", {{"weight", PropertyValue(payload)}}},
				{12U, 1U, 2U, "REL", {{"weight", PropertyValue(payload)}}},
				{13U, 1U, 2U, "REL", {{"weight", PropertyValue(payload)}}},
				{14U, 1U, 2U, "REL", {{"weight", PropertyValue(payload)}}},
				{15U, 1U, 2U, "REL", {{"weight", PropertyValue(payload)}}},
		});
		const NativeStreamedGraphInlinePropertyPageCount measured =
				NativeStreamedGraphPropertyPageMaterializer::measureInlinePages(publication);
		EXPECT_EQ(measured.page_count, 2U);
		EXPECT_EQ(measured.property_count, 5U);

		std::vector<NativeStreamedGraphPropertyPage> pages;
		NativeStreamedGraphPropertyPageMaterializer::materializeInlinePages(
				publication,
				[](size_t ordinal) {
					return NativeStreamedGraphPropertyPageAssignment{
							{PageId(101U + ordinal), 99U, PageOwnerKind::POK_PROPERTY, 0U,
							 RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, Generation(1U), LogSequenceNumber(2U)}};
				},
				[&pages](NativeStreamedGraphPropertyPage &&page) { pages.push_back(std::move(page)); });

		ASSERT_EQ(pages.size(), 2U);
		EXPECT_EQ(pages[0].property_locations.size() + pages[1].property_locations.size(), 5U);
		EXPECT_EQ(SlottedPage::open(pages[0].owner, pages[0].payload).stats().live_slot_count, 3U);
		EXPECT_EQ(SlottedPage::open(pages[1].owner, pages[1].payload).stats().live_slot_count, 2U);
	}

	TEST(NativeStreamedGraphPropertyPageMaterializerTest, RejectsAPropertyRootThatNeedsLargeValuePublication) {
		const NativeStreamedGraphDeferredPublication publication =
				publicationFor({{11U, 1U, 2U, "REL", {{"weight", PropertyValue(std::string(5000U, 'x'))}}}});
		EXPECT_THROW(static_cast<void>(NativeStreamedGraphPropertyPageMaterializer::countInlinePages(publication)),
					 FormatException);
	}

	TEST(NativeStreamedGraphPropertyPageMaterializerTest, StreamsLargeRootsThroughACompactSameGenerationReference) {
		const NativeStreamedGraphDeferredPublication publication =
				publicationFor({{11U, 1U, 2U, "REL", {{"weight", PropertyValue(std::string(5000U, 'x'))}}}});
		size_t plannedRoots = 0U;
		const NativeStreamedGraphLargePropertyRootEncoder encodeLarge =
				[&plannedRoots](const PropertyCollectionRoot &root, std::vector<std::byte> &&encodedRoot) {
					EXPECT_GT(encodedRoot.size(), RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES - RSS_SLOTTED_PAGE_HEADER_BYTES -
														  RSS_SLOTTED_PAGE_SLOT_BYTES);
					++plannedRoots;
					LargeValueRef value;
					value.value_id = LargeValueId(1U);
					value.generation = Generation(1U);
					value.logical_bytes = encodedRoot.size();
					value.content_hash.front() = std::byte{1U};
					value.kind = LargeValueKind::LVK_BYTES;
					value.codec = LargeValueCodec::LVC_RAW;
					value.media_type = RSS_ENTITY_PROPERTY_LARGE_VALUE_MEDIA_TYPE;
					return EntityPropertyLargeValueReferenceCodec::encode(
							{root.owner_entity_id, root.owner_entity_type, std::move(value)});
				};
		const NativeStreamedGraphInlinePropertyPageCount measured =
				NativeStreamedGraphPropertyPageMaterializer::measurePages(publication, encodeLarge);
		EXPECT_EQ(measured.page_count, 1U);
		EXPECT_EQ(measured.property_count, 1U);

		std::vector<NativeStreamedGraphPropertyPage> pages;
		NativeStreamedGraphPropertyPageMaterializer::materializePages(
				publication,
				[](size_t ordinal) {
					return NativeStreamedGraphPropertyPageAssignment{
							{PageId(101U + ordinal), 99U, PageOwnerKind::POK_PROPERTY, 0U,
							 RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, Generation(1U), LogSequenceNumber(2U)}};
				},
				[&pages](NativeStreamedGraphPropertyPage &&page) { pages.push_back(std::move(page)); }, encodeLarge);

		EXPECT_EQ(plannedRoots, 2U); // One deterministic planning pass and one materialization pass.
		ASSERT_EQ(pages.size(), 1U);
		ASSERT_EQ(pages.front().property_locations.size(), 1U);
		const SlottedPage page = SlottedPage::open(pages.front().owner, pages.front().payload);
		const std::vector<std::byte> stored = page.read({pages.front().property_locations.front().page_id,
														 pages.front().property_locations.front().slot_id,
														 pages.front().property_locations.front().slot_generation});
		ASSERT_TRUE(EntityPropertyLargeValueReferenceCodec::hasMagic(stored));
		const EntityPropertyLargeValueReference reference = EntityPropertyLargeValueReferenceCodec::decode(stored);
		EXPECT_EQ(reference.owner_entity_id, 11U);
		EXPECT_EQ(reference.owner_entity_type, Edge::typeId);
		EXPECT_EQ(reference.value.value_id, LargeValueId(1U));
	}

	TEST(NativeStreamedGraphPropertyPageMaterializerTest, RejectsIncompleteConsumersAndInvalidPageAssignments) {
		const NativeStreamedGraphDeferredPublication publication =
				publicationFor({{11U, 1U, 2U, "REL", {{"weight", PropertyValue(int64_t{7})}}}});
		const NativeStreamedGraphPropertyPageVisitor visitor = [](NativeStreamedGraphPropertyPage &&) {};
		const NativeStreamedGraphPropertyPageAssignmentSource validAssignment = [](size_t) {
			return NativeStreamedGraphPropertyPageAssignment{{PageId(101U), 99U, PageOwnerKind::POK_PROPERTY, 0U,
															  RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, Generation(1U),
															  LogSequenceNumber(2U)}};
		};
		const NativeStreamedGraphPropertyPageAssignmentSource noAssignment;
		const NativeStreamedGraphPropertyPageVisitor noVisitor;
		EXPECT_THROW(
				NativeStreamedGraphPropertyPageMaterializer::materializeInlinePages(publication, noAssignment, visitor),
				FormatException);
		EXPECT_THROW(NativeStreamedGraphPropertyPageMaterializer::materializeInlinePages(publication, validAssignment,
																						 noVisitor),
					 FormatException);
		const auto rejectsOwner = [&](PageOwnerHeader owner) {
			EXPECT_THROW(NativeStreamedGraphPropertyPageMaterializer::materializeInlinePages(
								 publication,
								 [owner](size_t) { return NativeStreamedGraphPropertyPageAssignment{owner}; }, visitor),
						 FormatException);
		};
		const auto validOwner = [] {
			return PageOwnerHeader{PageId(101U),
								   99U,
								   PageOwnerKind::POK_PROPERTY,
								   0U,
								   RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
								   Generation(1U),
								   LogSequenceNumber(2U)};
		};
		PageOwnerHeader invalid = validOwner();
		invalid.page_id = {};
		rejectsOwner(invalid);
		invalid = validOwner();
		invalid.owner_id = 0U;
		rejectsOwner(invalid);
		invalid = validOwner();
		invalid.kind = PageOwnerKind::POK_ENTITY;
		rejectsOwner(invalid);
		invalid = validOwner();
		invalid.kind = PageOwnerKind::POK_PROPERTY;
		invalid.flags = 1U;
		rejectsOwner(invalid);
		invalid = validOwner();
		invalid.flags = 0U;
		invalid.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES - 1U;
		rejectsOwner(invalid);
		invalid = validOwner();
		invalid.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
		invalid.generation = {};
		rejectsOwner(invalid);
		invalid = validOwner();
		invalid.generation = Generation(1U);
		invalid.committed_lsn = {};
		rejectsOwner(invalid);
	}

	TEST(NativeStreamedGraphPropertyPageMaterializerTest,
		 RejectsMalformedLargeRootResultsBeforePublishingAnyPropertyPage) {
		const NativeStreamedGraphDeferredPublication publication =
				publicationFor({{11U, 1U, 2U, "REL", {{"weight", PropertyValue(std::string(5000U, 'x'))}}}});
		const NativeStreamedGraphPropertyPageAssignmentSource assignment = [](size_t) {
			return NativeStreamedGraphPropertyPageAssignment{
					{PageId(101U), 99U, PageOwnerKind::POK_PROPERTY, 0U, RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
					 Generation(1U), LogSequenceNumber(2U)}};
		};
		bool visited = false;
		const NativeStreamedGraphPropertyPageVisitor visitor = [&visited](NativeStreamedGraphPropertyPage &&) {
			visited = true;
		};
		const auto expectRejected = [&](const NativeStreamedGraphLargePropertyRootEncoder &encoder) {
			EXPECT_THROW(NativeStreamedGraphPropertyPageMaterializer::materializePages(
						 publication, assignment, visitor, encoder),
					 FormatException);
			EXPECT_FALSE(visited);
		};

		expectRejected([](const PropertyCollectionRoot &, std::vector<std::byte> &&) { return std::vector<std::byte>{}; });
		expectRejected([](const PropertyCollectionRoot &root, std::vector<std::byte> &&) {
			PropertyCollectionRoot wrongOwner = root;
			++wrongOwner.owner_entity_id;
			return EntityPropertyCollectionRootCodec::encode(wrongOwner);
		});
		expectRejected([](const PropertyCollectionRoot &root, std::vector<std::byte> &&encoded) {
			// Returning the oversized inline encoding proves that identity validation
			// alone is insufficient: the stored replacement must also fit one slot.
			EXPECT_EQ(EntityPropertyCollectionRootCodec::decode(encoded), root);
			return encoded;
		});
		expectRejected([](const PropertyCollectionRoot &root, std::vector<std::byte> &&encoded) {
			LargeValueRef value;
			value.value_id = LargeValueId(1U);
			value.generation = Generation(1U);
			value.logical_bytes = encoded.size();
			value.content_hash.front() = std::byte{1U};
			value.kind = LargeValueKind::LVK_BYTES;
			value.codec = LargeValueCodec::LVC_RAW;
			value.media_type = RSS_ENTITY_PROPERTY_LARGE_VALUE_MEDIA_TYPE;
			return EntityPropertyLargeValueReferenceCodec::encode(
					{root.owner_entity_id + 1U, root.owner_entity_type, std::move(value)});
		});
		expectRejected([](const PropertyCollectionRoot &root, std::vector<std::byte> &&encoded) {
			LargeValueRef value;
			value.value_id = LargeValueId(1U);
			value.generation = Generation(1U);
			value.logical_bytes = encoded.size();
			value.content_hash.front() = std::byte{1U};
			value.kind = LargeValueKind::LVK_BYTES;
			value.codec = LargeValueCodec::LVC_RAW;
			value.media_type = RSS_ENTITY_PROPERTY_LARGE_VALUE_MEDIA_TYPE;
			return EntityPropertyLargeValueReferenceCodec::encode(
					{root.owner_entity_id, Node::typeId, std::move(value)});
		});
		expectRejected([](const PropertyCollectionRoot &root, std::vector<std::byte> &&) {
			PropertyCollectionRoot wrongType = root;
			wrongType.owner_entity_type = Node::typeId;
			return EntityPropertyCollectionRootCodec::encode(wrongType);
		});
		expectRejected([](const PropertyCollectionRoot &root, std::vector<std::byte> &&) {
			PropertyCollectionRoot changed = root;
			changed.entries.front().value = PropertyValue(int64_t{99});
			return EntityPropertyCollectionRootCodec::encode(changed);
		});
	}

} // namespace graph::storage
