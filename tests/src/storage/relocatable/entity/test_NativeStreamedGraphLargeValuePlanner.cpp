/**
 * @file test_NativeStreamedGraphLargeValuePlanner.cpp
 * @brief Bounded streamed LargeValue planning tests.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#include "storage/relocatable/metadata/RelocatableMetadataStoreTestFixture.hpp"

#include <array>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "graph/core/Edge.hpp"
#include "graph/storage/relocatable/allocation/SlottedPage.hpp"
#include "graph/storage/relocatable/entity/NativeStreamedGraphLargeValueContentRunBuilder.hpp"
#include "graph/storage/relocatable/entity/NativeStreamedGraphLargeValuePlanner.hpp"
#include "graph/storage/relocatable/entity/NativeStreamedGraphLargeValueReferenceMapBuilder.hpp"

namespace graph::storage {

	namespace {

		[[nodiscard]] NativeStreamedGraphDeferredPublication largeRootPublication(uint64_t edgeId) {
			const NativeBulkEdgeRecordSource source = [edgeId] {
				return [edgeId, emitted = false]() mutable -> std::optional<NativeBulkEdgeRecord> {
					if (emitted) {
						return std::nullopt;
					}
					emitted = true;
					return NativeBulkEdgeRecord{
							edgeId, 1U, 2U, "REL", {{"body", graph::PropertyValue(std::string(5000U, 'x'))}}};
				};
			};
			const std::array<NativeGraphLogicalIdLeaseRange, 1U> lease = {{NativeGraphLogicalIdLeaseRange{edgeId, 1U}}};
			return NativeStreamedGraphDeferredPublication::create(
					source, lease, {1U, 1U},
					{[](std::string_view name) -> std::optional<uint64_t> {
						 return name == "REL" ? std::optional<uint64_t>(7U) : std::nullopt;
					 },
					 [](std::string_view name) -> std::optional<uint32_t> {
						 return name == "body" ? std::optional<uint32_t>(9U) : std::nullopt;
					 }});
		}

		[[nodiscard]] NativeStreamedGraphDeferredPublication inlineRootPublication(uint64_t edgeId) {
			const NativeBulkEdgeRecordSource source = [edgeId] {
				return [edgeId, emitted = false]() mutable -> std::optional<NativeBulkEdgeRecord> {
					if (emitted) {
						return std::nullopt;
					}
					emitted = true;
					return NativeBulkEdgeRecord{edgeId, 1U, 2U, "REL", {{"body", graph::PropertyValue("small")}}};
				};
			};
			const std::array<NativeGraphLogicalIdLeaseRange, 1U> lease = {{NativeGraphLogicalIdLeaseRange{edgeId, 1U}}};
			return NativeStreamedGraphDeferredPublication::create(
					source, lease, {1U, 1U},
					{[](std::string_view name) -> std::optional<uint64_t> {
						 return name == "REL" ? std::optional<uint64_t>(7U) : std::nullopt;
					 },
					 [](std::string_view name) -> std::optional<uint32_t> {
						 return name == "body" ? std::optional<uint32_t>(9U) : std::nullopt;
					 }});
		}

		[[nodiscard]] NativeStreamedGraphDeferredPublication duplicateLargeRootPublication() {
			const NativeBulkEdgeRecordSource source = [] {
				return [ordinal = 0U]() mutable -> std::optional<NativeBulkEdgeRecord> {
					if (ordinal == 3U) {
						return std::nullopt;
					}
					const uint64_t edgeId = ++ordinal;
					return NativeBulkEdgeRecord{
							edgeId,
							1U,
							2U,
							"REL",
							{{"body", graph::PropertyValue(std::string(5000U, edgeId == 2U ? 'b' : 'a'))}}};
				};
			};
			const std::array<NativeGraphLogicalIdLeaseRange, 1U> lease = {{NativeGraphLogicalIdLeaseRange{1U, 3U}}};
			return NativeStreamedGraphDeferredPublication::create(
					source, lease, {3U, 1U},
					{[](std::string_view) -> std::optional<uint64_t> { return 7U; },
					 [](std::string_view) -> std::optional<uint32_t> { return 9U; }});
		}

		[[nodiscard]] NativeStreamedGraphDeferredPublication mixedRootPublication() {
			const NativeBulkEdgeRecordSource source = [] {
				return [ordinal = 0U]() mutable -> std::optional<NativeBulkEdgeRecord> {
					if (ordinal == 2U) {
						return std::nullopt;
					}
					const uint64_t edgeId = 30U + ordinal++;
					return NativeBulkEdgeRecord{
							edgeId, 1U, 2U, "REL",
							{{"body", graph::PropertyValue(edgeId == 30U ? "small" : std::string(5000U, 'x'))}}};
				};
			};
			const std::array<NativeGraphLogicalIdLeaseRange, 1U> lease = {{NativeGraphLogicalIdLeaseRange{30U, 2U}}};
			return NativeStreamedGraphDeferredPublication::create(
					source, lease, {2U, 1U},
					{[](std::string_view) -> std::optional<uint64_t> { return 7U; },
					 [](std::string_view) -> std::optional<uint32_t> { return 9U; }});
		}

		[[nodiscard]] NativeStreamedGraphDeferredPublication emptyAndLargeRootPublication() {
			const NativeBulkEdgeRecordSource source = [] {
				return [ordinal = 0U]() mutable -> std::optional<NativeBulkEdgeRecord> {
					if (ordinal == 2U) {
						return std::nullopt;
					}
					const uint64_t edgeId = 40U + ordinal++;
					return NativeBulkEdgeRecord{edgeId, 1U, 2U, "REL",
											  edgeId == 40U ? std::vector<LogicalGraphWriteProperty>{}
														 : std::vector<LogicalGraphWriteProperty>{
																	 {"body", graph::PropertyValue(std::string(5000U, 'x'))}}};
				};
			};
			const std::array<NativeGraphLogicalIdLeaseRange, 1U> lease = {{NativeGraphLogicalIdLeaseRange{40U, 2U}}};
			return NativeStreamedGraphDeferredPublication::create(
					source, lease, {2U, 1U},
					{[](std::string_view) -> std::optional<uint64_t> { return 7U; },
					 [](std::string_view) -> std::optional<uint32_t> { return 9U; }});
		}

		[[nodiscard]] std::vector<std::byte> referenceMapKey(uint8_t value) {
			std::vector<std::byte> result(sizeof(uint64_t));
			result.back() = static_cast<std::byte>(value);
			return result;
		}

		[[nodiscard]] NativeStreamedGraphLargeValueReferenceMapSource duplicateReferenceMap() {
			const auto entries = std::make_shared<std::vector<BTreePageEntry>>(
					std::initializer_list<BTreePageEntry>{{referenceMapKey(1U), 1U}, {referenceMapKey(2U), 2U},
																						 {referenceMapKey(3U), 1U}});
			return [entries] {
				return [entries, ordinal = 0U]() mutable -> std::optional<BTreePageEntry> {
					if (ordinal == entries->size()) {
						return std::nullopt;
					}
					return (*entries)[ordinal++];
				};
			};
		}

		[[nodiscard]] NativeStreamedGraphLargeValueReferenceMapSource
		referenceMap(std::vector<BTreePageEntry> entries) {
			auto sharedEntries = std::make_shared<std::vector<BTreePageEntry>>(std::move(entries));
			return [sharedEntries] {
				return [sharedEntries, ordinal = 0U]() mutable -> std::optional<BTreePageEntry> {
					if (ordinal == sharedEntries->size()) {
						return std::nullopt;
					}
					return (*sharedEntries)[ordinal++];
				};
			};
		}

	} // namespace

	TEST_F(RelocatableMetadataStoreTest, StreamedLargeValuePlannerMeasuresAndEmitsOneRootAtATime) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
		LargeValueStore values(io, *metadata);
		const NativeStreamedGraphDeferredPublication publication = largeRootPublication(11U);
		const NativeStreamedGraphLargeValuePlan plan =
				NativeStreamedGraphLargeValuePlanner::measure(publication, values);
		ASSERT_EQ(plan.property_pages.page_count, 1U);
		ASSERT_EQ(plan.property_pages.property_count, 1U);
		ASSERT_EQ(plan.value_count, 1U);
		EXPECT_GT(plan.data_page_count, 0U);
		EXPECT_GT(plan.manifest_page_count, 0U);

		std::vector<NativeStreamedGraphLargeValueItem> items;
		const NativeStreamedGraphLargePropertyRootEncoder encoder =
				NativeStreamedGraphLargeValuePlanner::makeRootEncoder(
						values, LargeValueId(1U), plan, {},
						[&items](NativeStreamedGraphLargeValueItem &&item) { items.push_back(std::move(item)); });
		std::vector<NativeStreamedGraphPropertyPage> pages;
		NativeStreamedGraphPropertyPageMaterializer::materializePages(
				publication,
				[](size_t ordinal) {
					return NativeStreamedGraphPropertyPageAssignment{
							{PageId(100U + ordinal), 23U, PageOwnerKind::POK_PROPERTY, 0U,
							 RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, Generation(1U), LogSequenceNumber(2U)}};
				},
				[&pages](NativeStreamedGraphPropertyPage &&page) { pages.push_back(std::move(page)); }, encoder);

		ASSERT_EQ(items.size(), 1U);
		EXPECT_EQ(items.front().stored_reference.owner_entity_id, 11U);
		EXPECT_EQ(items.front().stored_reference.owner_entity_type, Edge::typeId);
		EXPECT_EQ(items.front().stored_reference.value.value_id, LargeValueId(1U));
		EXPECT_EQ(items.front().planned_data_page_count, plan.data_page_count);
		EXPECT_EQ(items.front().planned_manifest_page_count, plan.manifest_page_count);
		const std::vector<PropertyCollectionRootEntry> entries =
				EntityPropertyLargeValuePayloadCodec::decode(items.front().logical_bytes);
		ASSERT_EQ(entries.size(), 1U);
		EXPECT_EQ(entries.front().key_token, 9U);

		ASSERT_EQ(pages.size(), 1U);
		const SlottedPage page = SlottedPage::open(pages.front().owner, pages.front().payload);
		const EntityPropertyLocation &location = pages.front().property_locations.front();
		const EntityPropertyLargeValueReference stored = EntityPropertyLargeValueReferenceCodec::decode(
				page.read({location.page_id, location.slot_id, location.slot_generation}));
		EXPECT_EQ(stored, items.front().stored_reference);
	}

	TEST_F(RelocatableMetadataStoreTest, StreamedLargeValuePlannerReplaysItsExactMeasuredPlanWithoutRetainingItems) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
		LargeValueStore values(io, *metadata);
		const NativeStreamedGraphDeferredPublication publication = largeRootPublication(12U);
		const NativeStreamedGraphLargeValuePlan plan =
				NativeStreamedGraphLargeValuePlanner::measure(publication, values);

		size_t itemCount = 0U;
		size_t pageCount = 0U;
		NativeStreamedGraphLargeValuePlanner::materializePropertyPages(
				publication, values, LargeValueId(1U), plan, {},
				[](size_t ordinal) {
					return NativeStreamedGraphPropertyPageAssignment{
							{PageId(200U + ordinal), 31U, PageOwnerKind::POK_PROPERTY, 0U,
							 RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, Generation(1U), LogSequenceNumber(2U)}};
				},
				[&pageCount](NativeStreamedGraphPropertyPage &&) { ++pageCount; });
		NativeStreamedGraphLargeValuePlanner::materializeLargeValues(
				publication, values, LargeValueId(1U), plan, {},
				[&itemCount](NativeStreamedGraphLargeValueItem &&item) {
					EXPECT_EQ(item.stored_reference.value.value_id, LargeValueId(1U));
					EXPECT_FALSE(item.logical_bytes.empty());
					++itemCount;
				});

		EXPECT_EQ(itemCount, plan.value_count);
		EXPECT_EQ(pageCount, plan.property_pages.page_count);
	}

	TEST_F(RelocatableMetadataStoreTest, StreamedLargeValuePlannerReplaysInlinePropertiesWithoutALargeValueLease) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
		LargeValueStore values(io, *metadata);
		const NativeStreamedGraphDeferredPublication publication = inlineRootPublication(17U);
		const NativeStreamedGraphLargeValuePlan plan =
				NativeStreamedGraphLargeValuePlanner::measure(publication, values);
		EXPECT_EQ(plan.reference_count, 0U);
		EXPECT_EQ(plan.value_count, 0U);
		EXPECT_EQ(plan.data_page_count, 0U);
		EXPECT_EQ(plan.manifest_page_count, 0U);

		size_t pageCount = 0U;
		NativeStreamedGraphLargeValuePlanner::materializePropertyPages(
				publication, values, LargeValueId{}, plan, {},
				[](size_t ordinal) {
					return NativeStreamedGraphPropertyPageAssignment{
							{PageId(250U + ordinal), 37U, PageOwnerKind::POK_PROPERTY, 0U,
							 RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, Generation(1U), LogSequenceNumber(2U)}};
				},
				[&pageCount](NativeStreamedGraphPropertyPage &&) { ++pageCount; });
		NativeStreamedGraphLargeValuePlanner::materializeLargeValues(
				publication, values, LargeValueId{}, plan, {},
				[](NativeStreamedGraphLargeValueItem &&) { ADD_FAILURE() << "inline roots must not emit LargeValues"; });

		EXPECT_EQ(pageCount, plan.property_pages.page_count);
	}

	TEST_F(RelocatableMetadataStoreTest, StreamedLargeValuePlannerRewritesDuplicateReferencesAndMaterializesCanonicalsOnce) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
		LargeValueStore values(io, *metadata);
		const NativeStreamedGraphDeferredPublication publication = duplicateLargeRootPublication();
		const NativeStreamedGraphLargeValuePlan measured =
				NativeStreamedGraphLargeValuePlanner::measure(publication, values);
		ASSERT_EQ(measured.reference_count, 3U);
		ASSERT_EQ(measured.value_count, 3U);
		const NativeStreamedGraphLargeValuePlan canonical = NativeStreamedGraphLargeValuePlanner::deriveCanonicalPlan(
				publication, values, LargeValueId(1U), measured, {}, duplicateReferenceMap());
		EXPECT_EQ(canonical.reference_count, 3U);
		EXPECT_EQ(canonical.value_count, 2U);

		std::vector<LargeValueId> materialized;
		NativeStreamedGraphLargeValuePlanner::materializeLargeValues(
				publication, values, LargeValueId(1U), canonical, {},
				[&materialized](NativeStreamedGraphLargeValueItem &&item) {
					materialized.push_back(item.stored_reference.value.value_id);
				},
				duplicateReferenceMap());
		EXPECT_EQ(materialized, (std::vector<LargeValueId>{LargeValueId(1U), LargeValueId(2U)}));

		std::vector<EntityPropertyLargeValueReference> stored;
		NativeStreamedGraphLargeValuePlanner::materializePropertyPages(
				publication, values, LargeValueId(1U), canonical, {},
				[](size_t ordinal) {
					return NativeStreamedGraphPropertyPageAssignment{
							{PageId(400U + ordinal), 47U, PageOwnerKind::POK_PROPERTY, 0U,
							 RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, Generation(1U), LogSequenceNumber(2U)}};
				},
				[&stored](NativeStreamedGraphPropertyPage &&page) {
					const SlottedPage slotted = SlottedPage::open(page.owner, page.payload);
					for (const EntityPropertyLocation &location: page.property_locations) {
						stored.push_back(EntityPropertyLargeValueReferenceCodec::decode(
								slotted.read({location.page_id, location.slot_id, location.slot_generation})));
					}
				},
				duplicateReferenceMap());
		ASSERT_EQ(stored.size(), 3U);
		EXPECT_EQ(stored[0].value.value_id, LargeValueId(1U));
		EXPECT_EQ(stored[1].value.value_id, LargeValueId(2U));
		EXPECT_EQ(stored[2].value.value_id, LargeValueId(1U));
	}

	TEST_F(RelocatableMetadataStoreTest, StreamedLargeValuePlannerCanonicalizesOverflowRootsWhileLeavingInlineRootsUntouched) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
		LargeValueStore values(io, *metadata);
		const NativeStreamedGraphDeferredPublication publication = mixedRootPublication();
		const NativeStreamedGraphLargeValuePlan measured =
				NativeStreamedGraphLargeValuePlanner::measure(publication, values);
		ASSERT_EQ(measured.reference_count, 1U);
		ASSERT_EQ(measured.value_count, 1U);

		const NativeStreamedGraphLargeValuePlan canonical = NativeStreamedGraphLargeValuePlanner::deriveCanonicalPlan(
				publication, values, LargeValueId(1U), measured, {},
				referenceMap({{referenceMapKey(1U), 1U}}));
		EXPECT_EQ(canonical.reference_count, 1U);
		EXPECT_EQ(canonical.value_count, 1U);
		EXPECT_EQ(canonical.data_page_count, measured.data_page_count);
		EXPECT_EQ(canonical.manifest_page_count, measured.manifest_page_count);

		std::vector<LargeValueId> materialized;
		NativeStreamedGraphLargeValuePlanner::materializeLargeValues(
				publication, values, LargeValueId(1U), canonical, {},
				[&materialized](NativeStreamedGraphLargeValueItem &&item) {
					materialized.push_back(item.stored_reference.value.value_id);
				},
				referenceMap({{referenceMapKey(1U), 1U}}));
		EXPECT_EQ(materialized, (std::vector<LargeValueId>{LargeValueId(1U)}));
	}

	TEST_F(RelocatableMetadataStoreTest, StreamedLargeValuePlannerSkipsEmptyRootsDuringCanonicalAndPhysicalReplay) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
		LargeValueStore values(io, *metadata);
		const NativeStreamedGraphDeferredPublication publication = emptyAndLargeRootPublication();
		const NativeStreamedGraphLargeValuePlan measured =
				NativeStreamedGraphLargeValuePlanner::measure(publication, values);
		ASSERT_EQ(measured.reference_count, 1U);
		ASSERT_EQ(measured.value_count, 1U);

		const NativeStreamedGraphLargeValuePlan canonical = NativeStreamedGraphLargeValuePlanner::deriveCanonicalPlan(
				publication, values, LargeValueId(1U), measured, {}, referenceMap({{referenceMapKey(1U), 1U}}));
		std::vector<LargeValueId> materialized;
		NativeStreamedGraphLargeValuePlanner::materializeLargeValues(
				publication, values, LargeValueId(1U), canonical, {},
				[&materialized](NativeStreamedGraphLargeValueItem &&item) {
					materialized.push_back(item.stored_reference.value.value_id);
				},
				referenceMap({{referenceMapKey(1U), 1U}}));

		EXPECT_EQ(materialized, (std::vector<LargeValueId>{LargeValueId(1U)}));
	}

	TEST_F(RelocatableMetadataStoreTest, StreamedLargeValuePlannerRejectsAReplayThatDropsAnOverflowRoot) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
		LargeValueStore values(io, *metadata);
		const auto firstPass = std::make_shared<bool>(true);
		const NativeBulkEdgeRecordSource source = [firstPass] {
			const bool emitLargeRoot = std::exchange(*firstPass, false);
			return [emitLargeRoot, emitted = false]() mutable -> std::optional<NativeBulkEdgeRecord> {
				if (emitted) {
					return std::nullopt;
				}
				emitted = true;
				return NativeBulkEdgeRecord{
						13U,
						1U,
						2U,
						"REL",
						{{"body", graph::PropertyValue(std::string(emitLargeRoot ? 5000U : 1U, 'x'))}}};
			};
		};
		const std::array<NativeGraphLogicalIdLeaseRange, 1U> lease = {{NativeGraphLogicalIdLeaseRange{13U, 1U}}};
		const NativeStreamedGraphDeferredPublication publication = NativeStreamedGraphDeferredPublication::create(
				source, lease, {1U, 1U},
				{[](std::string_view) -> std::optional<uint64_t> { return 7U; },
				 [](std::string_view) -> std::optional<uint32_t> { return 9U; }});
		const NativeStreamedGraphLargeValuePlan plan =
				NativeStreamedGraphLargeValuePlanner::measure(publication, values);
		ASSERT_EQ(plan.value_count, 1U);

		EXPECT_THROW(NativeStreamedGraphLargeValuePlanner::materializePropertyPages(
							 publication, values, LargeValueId(1U), plan, {},
							 [](size_t ordinal) {
								 return NativeStreamedGraphPropertyPageAssignment{
										 {PageId(300U + ordinal), 41U, PageOwnerKind::POK_PROPERTY, 0U,
										  RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, Generation(1U), LogSequenceNumber(2U)}};
							 },
							 [](NativeStreamedGraphPropertyPage &&) {}),
					 FormatException);
	}

	TEST_F(RelocatableMetadataStoreTest, StreamedLargeValuePlannerRejectsAPhysicalReplayThatDropsAnOverflowRoot) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
		LargeValueStore values(io, *metadata);
		const auto firstPass = std::make_shared<bool>(true);
		const NativeBulkEdgeRecordSource source = [firstPass] {
			const bool emitLargeRoot = std::exchange(*firstPass, false);
			return [emitLargeRoot, emitted = false]() mutable -> std::optional<NativeBulkEdgeRecord> {
				if (emitted) {
					return std::nullopt;
				}
				emitted = true;
				return NativeBulkEdgeRecord{
						14U,
						1U,
						2U,
						"REL",
						{{"body", graph::PropertyValue(std::string(emitLargeRoot ? 5000U : 1U, 'x'))}}};
			};
		};
		const std::array<NativeGraphLogicalIdLeaseRange, 1U> lease = {{NativeGraphLogicalIdLeaseRange{14U, 1U}}};
		const NativeStreamedGraphDeferredPublication publication = NativeStreamedGraphDeferredPublication::create(
				source, lease, {1U, 1U},
				{[](std::string_view) -> std::optional<uint64_t> { return 7U; },
				 [](std::string_view) -> std::optional<uint32_t> { return 9U; }});
		const NativeStreamedGraphLargeValuePlan plan =
				NativeStreamedGraphLargeValuePlanner::measure(publication, values);
		ASSERT_EQ(plan.value_count, 1U);

		EXPECT_THROW(
				NativeStreamedGraphLargeValuePlanner::materializeLargeValues(
						publication, values, LargeValueId(1U), plan, {}, [](NativeStreamedGraphLargeValueItem &&) {}),
				FormatException);
	}

	TEST_F(RelocatableMetadataStoreTest, StreamedLargeValuePlannerRejectsIncompletePublicPlanningContracts) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
		LargeValueStore values(io, *metadata);
		const NativeStreamedGraphDeferredPublication publication = largeRootPublication(15U);
		const NativeStreamedGraphLargeValuePlan plan =
				NativeStreamedGraphLargeValuePlanner::measure(publication, values);

		EXPECT_THROW(
				static_cast<void>(NativeStreamedGraphLargeValuePlanner::makeRootEncoder(
						values, LargeValueId{}, plan, {}, [](NativeStreamedGraphLargeValueItem &&) {})),
				FormatException);
		NativeStreamedGraphLargeValuePlan noReferences = plan;
		noReferences.reference_count = 0U;
		EXPECT_THROW(
				static_cast<void>(NativeStreamedGraphLargeValuePlanner::makeRootEncoder(
						values, LargeValueId(1U), noReferences, {}, [](NativeStreamedGraphLargeValueItem &&) {})),
				FormatException);
		NativeStreamedGraphLargeValuePlan noValues = plan;
		noValues.value_count = 0U;
		EXPECT_THROW(
				static_cast<void>(NativeStreamedGraphLargeValuePlanner::makeRootEncoder(
						values, LargeValueId(1U), noValues, {}, [](NativeStreamedGraphLargeValueItem &&) {})),
				FormatException);
		NativeStreamedGraphLargeValuePlan excessiveValues = plan;
		excessiveValues.value_count = excessiveValues.reference_count + 1U;
		EXPECT_THROW(
				static_cast<void>(NativeStreamedGraphLargeValuePlanner::makeRootEncoder(
						values, LargeValueId(1U), excessiveValues, {}, [](NativeStreamedGraphLargeValueItem &&) {})),
				FormatException);
		EXPECT_THROW(static_cast<void>(NativeStreamedGraphLargeValuePlanner::makeRootEncoder(
						values, LargeValueId(1U), plan, {}, {})),
				 FormatException);
		NativeStreamedGraphLargeValuePlan overflowingRange = plan;
		overflowingRange.reference_count = 2U;
		EXPECT_THROW(
				static_cast<void>(NativeStreamedGraphLargeValuePlanner::makeRootEncoder(
						values, LargeValueId(std::numeric_limits<uint64_t>::max()), overflowingRange, {},
						[](NativeStreamedGraphLargeValueItem &&) {})),
				FormatException);

		EXPECT_THROW(
				NativeStreamedGraphLargeValuePlanner::materializePropertyPages(
						publication, values, LargeValueId(1U), plan, {}, {},
						[](NativeStreamedGraphPropertyPage &&) {}),
				FormatException);
		EXPECT_THROW(
				NativeStreamedGraphLargeValuePlanner::materializePropertyPages(
						publication, values, LargeValueId(1U), plan, {},
						[](size_t) { return NativeStreamedGraphPropertyPageAssignment{}; }, {}),
				FormatException);
		EXPECT_THROW(
				NativeStreamedGraphLargeValuePlanner::materializePropertyPages(
						publication, values, LargeValueId{}, plan, {},
						[](size_t ordinal) {
							return NativeStreamedGraphPropertyPageAssignment{
									{PageId(490U + ordinal), 53U, PageOwnerKind::POK_PROPERTY, 0U,
									 RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, Generation(1U), LogSequenceNumber(2U)}};
						},
						[](NativeStreamedGraphPropertyPage &&) {}),
				FormatException);
		EXPECT_THROW(NativeStreamedGraphLargeValuePlanner::materializeLargeValues(
						publication, values, LargeValueId{}, plan, {}, [](NativeStreamedGraphLargeValueItem &&) {}),
				 FormatException);
		EXPECT_THROW(NativeStreamedGraphLargeValuePlanner::materializeLargeValues(
						publication, values, LargeValueId(1U), plan, {}, {}),
				 FormatException);

		NativeStreamedGraphLargeValuePlan missingMeasuredPages = plan;
		missingMeasuredPages.property_pages.page_count = 0U;
		EXPECT_THROW(
				NativeStreamedGraphLargeValuePlanner::materializePropertyPages(
						publication, values, LargeValueId(1U), missingMeasuredPages, {},
						[](size_t ordinal) {
							return NativeStreamedGraphPropertyPageAssignment{
									{PageId(500U + ordinal), 53U, PageOwnerKind::POK_PROPERTY, 0U,
									 RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, Generation(1U), LogSequenceNumber(2U)}};
						},
						[](NativeStreamedGraphPropertyPage &&) {}),
				FormatException);
		NativeStreamedGraphLargeValuePlan missingMeasuredData = plan;
		missingMeasuredData.data_page_count = 0U;
		EXPECT_THROW(NativeStreamedGraphLargeValuePlanner::materializeLargeValues(
						publication, values, LargeValueId(1U), missingMeasuredData, {},
						[](NativeStreamedGraphLargeValueItem &&) {}),
				 FormatException);
		NativeStreamedGraphLargeValuePlan missingMeasuredManifest = plan;
		missingMeasuredManifest.manifest_page_count = 0U;
		EXPECT_THROW(NativeStreamedGraphLargeValuePlanner::materializeLargeValues(
						publication, values, LargeValueId(1U), missingMeasuredManifest, {},
						[](NativeStreamedGraphLargeValueItem &&) {}),
				 FormatException);
	}

	TEST_F(RelocatableMetadataStoreTest, StreamedLargeValuePlannerRejectsOverflowRootsBeyondTheMeasuredPlan) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
		LargeValueStore values(io, *metadata);
		const NativeStreamedGraphDeferredPublication publication = largeRootPublication(16U);
		const NativeStreamedGraphLargeValuePlan plan =
				NativeStreamedGraphLargeValuePlanner::measure(publication, values);
		const PropertyCollectionRoot root{16U, Edge::typeId, {{9U, graph::PropertyValue(std::string(5000U, 'x'))}}};
		const NativeStreamedGraphLargePropertyRootEncoder encoder =
				NativeStreamedGraphLargeValuePlanner::makeRootEncoder(values, LargeValueId(1U), plan, {},
																	  [](NativeStreamedGraphLargeValueItem &&) {});

		EXPECT_NO_THROW(static_cast<void>(encoder(root, EntityPropertyCollectionRootCodec::encode(root))));
		EXPECT_THROW(static_cast<void>(encoder(root, EntityPropertyCollectionRootCodec::encode(root))),
					 FormatException);

		NativeStreamedGraphLargeValuePlan missingOverflowPlan = plan;
		missingOverflowPlan.reference_count = 0U;
		missingOverflowPlan.value_count = 0U;
		EXPECT_THROW(NativeStreamedGraphLargeValuePlanner::materializeLargeValues(
							 publication, values, LargeValueId(1U), missingOverflowPlan, {},
							 [](NativeStreamedGraphLargeValueItem &&) {}),
					 FormatException);
	}

	TEST_F(RelocatableMetadataStoreTest, StreamedLargeValuePlannerRejectsDivergentCanonicalReferenceSources) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
		LargeValueStore values(io, *metadata);
		const NativeStreamedGraphDeferredPublication publication = duplicateLargeRootPublication();
		const NativeStreamedGraphLargeValuePlan measured =
				NativeStreamedGraphLargeValuePlanner::measure(publication, values);

		EXPECT_THROW(static_cast<void>(NativeStreamedGraphLargeValuePlanner::deriveCanonicalPlan(
						publication, values, LargeValueId{}, measured, {}, duplicateReferenceMap())),
				 FormatException);
		EXPECT_THROW(static_cast<void>(NativeStreamedGraphLargeValuePlanner::deriveCanonicalPlan(
						publication, values, LargeValueId(1U), measured, {}, {})),
				 FormatException);
		NativeStreamedGraphLargeValuePlan noReferences = measured;
		noReferences.reference_count = 0U;
		EXPECT_THROW(static_cast<void>(NativeStreamedGraphLargeValuePlanner::deriveCanonicalPlan(
						publication, values, LargeValueId(1U), noReferences, {}, duplicateReferenceMap())),
				 FormatException);
		NativeStreamedGraphLargeValuePlan mismatched = measured;
		mismatched.value_count = mismatched.reference_count - 1U;
		EXPECT_THROW(static_cast<void>(NativeStreamedGraphLargeValuePlanner::deriveCanonicalPlan(
						publication, values, LargeValueId(1U), mismatched, {}, duplicateReferenceMap())),
				 FormatException);
		const NativeStreamedGraphLargeValueReferenceMapSource noCursor = [] {
			return OrderedBTreeEntryCursor{};
		};
		EXPECT_THROW(static_cast<void>(NativeStreamedGraphLargeValuePlanner::deriveCanonicalPlan(
						publication, values, LargeValueId(1U), measured, {}, noCursor)),
				 FormatException);
		const NativeStreamedGraphLargeValueReferenceMapSource missingEntry = [] {
			return []() -> std::optional<BTreePageEntry> { return std::nullopt; };
		};
		EXPECT_THROW(static_cast<void>(NativeStreamedGraphLargeValuePlanner::deriveCanonicalPlan(
						publication, values, LargeValueId(1U), measured, {}, missingEntry)),
				 FormatException);
		const NativeStreamedGraphLargeValueReferenceMapSource invalidIdentity = [] {
			return [emitted = false]() mutable -> std::optional<BTreePageEntry> {
				if (std::exchange(emitted, true)) {
					return std::nullopt;
				}
				return BTreePageEntry{{std::byte{0x01}}, 1U};
			};
		};
		EXPECT_THROW(static_cast<void>(NativeStreamedGraphLargeValuePlanner::deriveCanonicalPlan(
						publication, values, LargeValueId(1U), measured, {}, invalidIdentity)),
				 FormatException);
		EXPECT_THROW(static_cast<void>(NativeStreamedGraphLargeValuePlanner::deriveCanonicalPlan(
						publication, values, LargeValueId(1U), measured, {},
						referenceMap({{referenceMapKey(1U), 0U}, {referenceMapKey(2U), 2U},
									  {referenceMapKey(3U), 3U}}))),
				 FormatException);
		EXPECT_THROW(static_cast<void>(NativeStreamedGraphLargeValuePlanner::deriveCanonicalPlan(
						publication, values, LargeValueId(1U), measured, {},
						referenceMap({{referenceMapKey(1U), 2U}, {referenceMapKey(2U), 2U},
									  {referenceMapKey(3U), 3U}}))),
				 FormatException);
		EXPECT_THROW(static_cast<void>(NativeStreamedGraphLargeValuePlanner::deriveCanonicalPlan(
						publication, values, LargeValueId(1U), measured, {},
						referenceMap({{referenceMapKey(1U), 1U}, {referenceMapKey(2U), 2U},
									  {referenceMapKey(3U), 3U}, {referenceMapKey(4U), 3U}}))),
				 FormatException);
	}

	TEST_F(RelocatableMetadataStoreTest, StreamedLargeValuePrivateRunsPreserveCanonicalContentAndReferenceOrder) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
		LargeValueStore values(io, *metadata);
		const NativeStreamedGraphDeferredPublication publication = duplicateLargeRootPublication();
		const NativeStreamedGraphLargeValuePlan plan =
				NativeStreamedGraphLargeValuePlanner::measure(publication, values);
		ASSERT_EQ(plan.reference_count, 3U);
		ASSERT_EQ(plan.value_count, 3U);

		std::vector<BTreePageEntry> contentEntries;
		NativeStreamedGraphLargeValuePlanner::materializeLargeValues(
				publication, values, LargeValueId(1U), plan, {},
				[&contentEntries, &values](NativeStreamedGraphLargeValueItem &&item) {
					contentEntries.push_back(
							{values.contentIndexKey(item.stored_reference.value), item.stored_reference.value.value_id.value()});
				});
		ASSERT_EQ(contentEntries.size(), 3U);
		std::ranges::sort(contentEntries, {}, &BTreePageEntry::key);
		const size_t contentEntryBytes = RSS_NATIVE_BULK_RUN_HEADER_BYTES + RSS_NATIVE_BULK_RUN_RECORD_HEADER_BYTES +
									 BTreePageEntryCodec::encodedSize(contentEntries.front());
		const NativeStreamedGraphLargeValueContentRunPlan contentPlan =
				NativeStreamedGraphLargeValueContentRunBuilder::derivePlanForValueCount(
						values, plan.value_count, contentEntryBytes, 2U);
		ASSERT_EQ(contentPlan.initial_fragment_count, plan.value_count);
		EXPECT_GT(contentPlan.candidate_segment_count, 0U);
		EXPECT_EQ(NativeStreamedGraphLargeValueContentRunBuilder::derivePlanForValueCount(
						  values, 0U, contentEntryBytes, 2U)
						  .initial_fragment_count,
				  0U);
		const NativeStreamedGraphLargeValueContentRunPlan measuredContentPlan =
				NativeStreamedGraphLargeValueContentRunBuilder::derivePlan(
						publication, values, LargeValueId(1U), plan, {}, contentEntryBytes, 2U);
		EXPECT_EQ(measuredContentPlan.initial_fragment_count, contentPlan.initial_fragment_count);
		EXPECT_EQ(measuredContentPlan.candidate_segment_count, contentPlan.candidate_segment_count);
		NativeStreamedGraphLargeValuePlan emptyPlan = plan;
		emptyPlan.value_count = 0U;
		EXPECT_EQ(NativeStreamedGraphLargeValueContentRunBuilder::derivePlan(
						  publication, values, LargeValueId(1U), emptyPlan, {}, contentEntryBytes, 2U)
						  .initial_fragment_count,
				  0U);

		std::vector<NativeBulkRunFragment> contentRuns;
		NativeStreamedGraphLargeValueContentRunBuilder::buildRuns(
				publication, values, LargeValueId(1U), plan, {}, contentEntryBytes, contentPlan.initial_fragment_count,
				[&contentRuns](NativeBulkRunFragment &&run) { contentRuns.push_back(std::move(run)); });
		ASSERT_EQ(contentRuns.size(), contentPlan.initial_fragment_count);
		for (const NativeBulkRunFragment &run: contentRuns) {
			ASSERT_EQ(run.entries.size(), 1U);
		}

		const OrderedBTreeEntrySource source = [contentEntries] {
			return [contentEntries, ordinal = 0U]() mutable -> std::optional<BTreePageEntry> {
				if (ordinal == contentEntries.size()) {
					return std::nullopt;
				}
				return contentEntries[ordinal++];
			};
		};
		const BTreePageEntry firstReference{referenceMapKey(1U), 1U};
		const size_t referenceEntryBytes = RSS_NATIVE_BULK_RUN_HEADER_BYTES + RSS_NATIVE_BULK_RUN_RECORD_HEADER_BYTES +
									   BTreePageEntryCodec::encodedSize(firstReference);
		const NativeStreamedGraphLargeValueReferenceMapRunPlan referencePlan =
				NativeStreamedGraphLargeValueReferenceMapBuilder::derivePlanForValueCount(
						plan.reference_count, referenceEntryBytes, 2U);
		ASSERT_EQ(referencePlan.initial_fragment_count, plan.reference_count);
		EXPECT_EQ(NativeStreamedGraphLargeValueReferenceMapBuilder::derivePlanForValueCount(
						  0U, referenceEntryBytes, 2U)
						  .initial_fragment_count,
				  0U);

		std::vector<NativeBulkRunFragment> referenceRuns;
		NativeStreamedGraphLargeValueReferenceMapBuilder::buildRuns(
				source, values, LargeValueId(1U), plan.reference_count, referenceEntryBytes,
				referencePlan.initial_fragment_count,
				[&referenceRuns](NativeBulkRunFragment &&run) { referenceRuns.push_back(std::move(run)); });
		ASSERT_EQ(referenceRuns.size(), referencePlan.initial_fragment_count);
		std::unordered_map<uint8_t, uint64_t> canonicalByProvisional;
		for (const NativeBulkRunFragment &run: referenceRuns) {
			ASSERT_EQ(run.entries.size(), 1U);
			ASSERT_EQ(run.entries.front().key.size(), sizeof(uint64_t));
			canonicalByProvisional.emplace(std::to_integer<uint8_t>(run.entries.front().key.back()),
									   run.entries.front().value_or_child);
		}
		ASSERT_EQ(canonicalByProvisional.size(), 3U);
		// The content-key ordering, rather than input record order, chooses the
		// canonical ID. Equal payloads must still share one canonical identity.
		EXPECT_EQ(canonicalByProvisional.at(1U), canonicalByProvisional.at(3U));
		EXPECT_NE(canonicalByProvisional.at(1U), canonicalByProvisional.at(2U));
	}

	TEST_F(RelocatableMetadataStoreTest, StreamedLargeValuePrivateRunsRejectInvalidBoundsAndContentSources) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
		LargeValueStore values(io, *metadata);
		const NativeStreamedGraphDeferredPublication publication = largeRootPublication(16U);
		const NativeStreamedGraphLargeValuePlan plan =
				NativeStreamedGraphLargeValuePlanner::measure(publication, values);
		constexpr size_t runBytes = RSS_NATIVE_BULK_RUN_HEADER_BYTES + 1024U;

		EXPECT_THROW(static_cast<void>(NativeStreamedGraphLargeValueContentRunBuilder::derivePlanForValueCount(
						 values, 1U, runBytes, 1U)),
				 FormatException);
		EXPECT_THROW(static_cast<void>(NativeStreamedGraphLargeValueReferenceMapBuilder::derivePlanForValueCount(
						 1U, runBytes, 1U)),
				 FormatException);
		EXPECT_THROW(NativeStreamedGraphLargeValueContentRunBuilder::buildRuns(
						 publication, values, LargeValueId{}, plan, {}, runBytes, 1U,
						 [](NativeBulkRunFragment &&) {}),
				 FormatException);
		EXPECT_THROW(NativeStreamedGraphLargeValueContentRunBuilder::buildRuns(
						 publication, values, LargeValueId(1U), plan, {}, runBytes, 0U,
						 [](NativeBulkRunFragment &&) {}),
				 FormatException);
		EXPECT_THROW(NativeStreamedGraphLargeValueContentRunBuilder::buildRuns(
						 publication, values, LargeValueId(1U), plan, {}, runBytes, 1U, {}),
				 FormatException);
		EXPECT_THROW(static_cast<void>(NativeStreamedGraphLargeValueContentRunBuilder::derivePlanForValueCount(
						 values, 1U, RSS_NATIVE_BULK_RUN_HEADER_BYTES, 2U)),
				 FormatException);
		EXPECT_THROW(static_cast<void>(NativeStreamedGraphLargeValueReferenceMapBuilder::derivePlanForValueCount(
						 1U, RSS_NATIVE_BULK_RUN_HEADER_BYTES, 2U)),
				 FormatException);

		const OrderedBTreeEntrySource noCursor = [] { return OrderedBTreeEntryCursor{}; };
		EXPECT_THROW(NativeStreamedGraphLargeValueReferenceMapBuilder::buildRuns(
						 noCursor, values, LargeValueId(1U), 1U, runBytes, 1U,
						 [](NativeBulkRunFragment &&) {}),
				 FormatException);
		const OrderedBTreeEntrySource malformedEntry = [] {
			return [emitted = false]() mutable -> std::optional<BTreePageEntry> {
				if (std::exchange(emitted, true)) {
					return std::nullopt;
				}
				return BTreePageEntry{{std::byte{0x01U}}, 1U};
			};
		};
		EXPECT_THROW(NativeStreamedGraphLargeValueReferenceMapBuilder::buildRuns(
						 malformedEntry, values, LargeValueId(1U), 1U, runBytes, 1U,
						 [](NativeBulkRunFragment &&) {}),
				 FormatException);

		const NativeStreamedGraphDeferredPublication duplicates = duplicateLargeRootPublication();
		const NativeStreamedGraphLargeValuePlan duplicatePlan =
				NativeStreamedGraphLargeValuePlanner::measure(duplicates, values);
		std::vector<BTreePageEntry> entries;
		NativeStreamedGraphLargeValuePlanner::materializeLargeValues(
				duplicates, values, LargeValueId(1U), duplicatePlan, {},
				[&entries, &values](NativeStreamedGraphLargeValueItem &&item) {
					entries.push_back(
							{values.contentIndexKey(item.stored_reference.value), item.stored_reference.value.value_id.value()});
				});
		std::ranges::sort(entries, {}, &BTreePageEntry::key);
		const auto sourceFor = [](std::vector<BTreePageEntry> sourceEntries) {
			return [sourceEntries = std::move(sourceEntries)] {
				return [sourceEntries, ordinal = 0U]() mutable -> std::optional<BTreePageEntry> {
					if (ordinal == sourceEntries.size()) {
						return std::nullopt;
					}
					return sourceEntries[ordinal++];
				};
			};
		};
		const size_t exactReferenceRunBytes = RSS_NATIVE_BULK_RUN_HEADER_BYTES +
				RSS_NATIVE_BULK_RUN_RECORD_HEADER_BYTES + BTreePageEntryCodec::encodedSize({referenceMapKey(1U), 1U});
		const size_t exactContentRunBytes = RSS_NATIVE_BULK_RUN_HEADER_BYTES + RSS_NATIVE_BULK_RUN_RECORD_HEADER_BYTES +
				BTreePageEntryCodec::encodedSize(entries.front());
		EXPECT_THROW(NativeStreamedGraphLargeValueContentRunBuilder::buildRuns(
						 duplicates, values, LargeValueId(1U), duplicatePlan, {}, exactContentRunBytes, 1U,
						 [](NativeBulkRunFragment &&) {}),
				 FormatException);
		EXPECT_THROW(NativeStreamedGraphLargeValueReferenceMapBuilder::buildRuns(
						 sourceFor(entries), values, LargeValueId(1U), duplicatePlan.reference_count, exactReferenceRunBytes,
						 1U, [](NativeBulkRunFragment &&) {}),
				 FormatException);
		std::vector<BTreePageEntry> zeroValue = entries;
		zeroValue.front().value_or_child = 0U;
		EXPECT_THROW(NativeStreamedGraphLargeValueReferenceMapBuilder::buildRuns(
						 sourceFor(std::move(zeroValue)), values, LargeValueId(1U), duplicatePlan.reference_count, runBytes,
						 duplicatePlan.reference_count, [](NativeBulkRunFragment &&) {}),
				 FormatException);
		std::vector<BTreePageEntry> mismatchedIdentity = entries;
		++mismatchedIdentity.front().value_or_child;
		EXPECT_THROW(NativeStreamedGraphLargeValueReferenceMapBuilder::buildRuns(
						 sourceFor(std::move(mismatchedIdentity)), values, LargeValueId(1U), duplicatePlan.reference_count,
						 runBytes, duplicatePlan.reference_count, [](NativeBulkRunFragment &&) {}),
				 FormatException);
		EXPECT_THROW(NativeStreamedGraphLargeValueReferenceMapBuilder::buildRuns(
						 sourceFor(entries), values, LargeValueId(1U), duplicatePlan.reference_count - 1U, runBytes,
						 duplicatePlan.reference_count, [](NativeBulkRunFragment &&) {}),
				 FormatException);
		entries.pop_back();
		EXPECT_THROW(NativeStreamedGraphLargeValueReferenceMapBuilder::buildRuns(
						 sourceFor(std::move(entries)), values, LargeValueId(1U), duplicatePlan.reference_count, runBytes,
						 duplicatePlan.reference_count, [](NativeBulkRunFragment &&) {}),
				 FormatException);
	}

} // namespace graph::storage
