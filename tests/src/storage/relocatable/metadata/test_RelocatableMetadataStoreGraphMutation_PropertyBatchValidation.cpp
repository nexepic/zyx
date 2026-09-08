/**
 * @file test_RelocatableMetadataStoreGraphMutation_PropertyBatchValidation.cpp
 * @brief Property page batch input contracts for graph metadata publication.
 */

#include "storage/relocatable/metadata/RelocatableMetadataStoreTestFixture.hpp"

#include "graph/core/Edge.hpp"
#include "graph/core/Node.hpp"
#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/entity/EntityPageStore.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRootStore.hpp"

#include <array>
#include <functional>
#include <string>
#include <vector>

namespace {

const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
const std::array<EntityPageNewPoolBatchRequest, 0U> noEntityPools{};
const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
const std::array<NativeGraphLargeValueMutation, 0U> noLargeValues{};

void noOpGraphMaterializer(std::span<EntityPageReplacementMaterialization>,
						   std::span<EntityPageAppendBatchMaterialization>,
						   std::span<EntityPageNewPoolBatchMaterialization>,
						   std::span<NativeGraphPropertyPageReplacementMaterialization>,
						   std::span<NativeGraphPropertyPageBatchMaterialization>,
						   std::span<LargeValueMaterializationResult>) {}

} // namespace

TEST_F(RelocatableMetadataStoreTest, RejectsMalformedFreshPropertyPageBatchesBeforeMaterialization) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	MetadataSnapshotPin before = store->openSnapshot();
	const Superblock beforeSuperblock = before.superblock();
	before.release();

	const auto reject = [&](const std::function<void(NativeGraphPropertyPageBatchRequest &)> &forge,
						   FormatError expected = FormatError::FERR_INVALID_HEADER) {
		NativeGraphPropertyPageBatchRequest request;
		request.kind = NativeGraphPropertyPageBatchKind::NGPBK_CREATE_POOL;
		request.page_owner_id = 1U;
		request.first_slot = PhysicalSlotId(40U);
		request.last_slot = PhysicalSlotId(40U);
		request.page_property_keys = {{{EntityPropertyOwnerKind::EPOK_NODE, 1U}}};
		forge(request);
		const std::array<NativeGraphPropertyPageBatchRequest, 1U> batches{{request}};
		expectFormatError(
				[&] {
					static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
							noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, batches,
							noLargeValues, {}, LogSequenceNumber(2U), noOpGraphMaterializer, {}));
				},
				expected);
		MetadataSnapshotPin unchanged = store->openSnapshot();
		EXPECT_EQ(unchanged.superblock(), beforeSuperblock);
		unchanged.release();
	};

	reject([](auto &request) { request.kind = static_cast<NativeGraphPropertyPageBatchKind>(255U); });
	reject([](auto &request) { request.page_owner_id = 0U; });
	reject([](auto &request) { request.page_property_keys.clear(); });
	reject([](auto &request) { request.page_property_keys = {{}}; });
	reject([](auto &request) { request.page_property_keys.front().front() = {}; });
	// Appending targets a published pool identity; it cannot claim new pool slots.
	reject([](auto &request) { request.kind = NativeGraphPropertyPageBatchKind::NGPBK_APPEND_EXISTING_POOL; });
	reject([](auto &request) {
		request.page_property_keys.front().push_back({EntityPropertyOwnerKind::EPOK_NODE, 1U});
	},
		   FormatError::FERR_INVALID_STATE);
	reject([](auto &request) { request.expected_pool = entry(40U, 1U, 1U); });
	reject([](auto &request) { request.first_slot = {}; });
	reject([](auto &request) { request.last_slot = {}; });
	reject([](auto &request) {
		request.first_slot = PhysicalSlotId(41U);
		request.last_slot = PhysicalSlotId(40U);
	});
}

TEST_F(RelocatableMetadataStoreTest, RejectsMalformedPropertyRelocationsBeforeMaterialization) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	EntityPageStore entities(io, *store);
	SlottedPagePool pages(io, *store);
	PropertyCollectionRootStore properties(pages, *store);
	const PropertyCollectionRootStoreOptions propertyOptions{PhysicalSlotId(41U), PhysicalSlotId(48U), 902U};
	NodeEntityRecord node;
	node.inline_label_ids = {7U, 9U, 11U};
	node.inline_label_count = 3U;
	const EntityPageNewPoolRequest nodePoolRequest{
			.segment_id = SegmentId(31U),
			.pool_owner_id = 901U,
			.generation = Generation(1U),
			.page_id = PageId(101U),
			.first_slot = PhysicalSlotId(40U),
			.last_slot = PhysicalSlotId(40U),
	};
	const std::array<EntityPageBatchNewPool, 1U> nodePools{{
			{.request = nodePoolRequest,
			 .record_kind = EntityPageRecordKind::EPRK_NODE,
			 .node_insertions = {{1001U, node}, {1002U, node}}},
	}};
	const EntityPropertyKey propertyKey{EntityPropertyOwnerKind::EPOK_NODE, 1001U};
	const PropertyCollectionRoot initialRoot{
			1001U, graph::Node::typeId, {{3U, graph::PropertyValue("original Property root")}}};
	MetadataSnapshotPin empty = store->openSnapshot();
	const EntityPropertyRootMutationPlan initialPlan =
			properties.planEntityRootReplacement(empty, initialRoot, propertyOptions);
	ASSERT_EQ(initialPlan.kind, EntityPropertyRootMutationPlanKind::EPRMPK_NEW_PAGE_REQUIRED);
	static_cast<void>(entities.applyNativeGraphBatchTransaction(
			empty, {}, {}, nodePools, std::span(&initialPlan, 1U), properties, propertyOptions, LogSequenceNumber(2U)));
	empty.release();

	MetadataSnapshotPin before = store->openSnapshot();
	PropertyCollectionRoot replacementRoot = initialRoot;
	replacementRoot.entries.front().value = graph::PropertyValue("replaced Property root");
	const EntityPropertyRootMutationPlan planned =
			properties.planEntityRootReplacement(before, replacementRoot, propertyOptions);
	ASSERT_EQ(planned.kind, EntityPropertyRootMutationPlanKind::EPRMPK_EXISTING_PAGE);
	ASSERT_TRUE(planned.page_replacement.has_value());
	const NativeGraphPropertyPageReplacementRequest validRequest = *planned.page_replacement;
	ASSERT_EQ(validRequest.upserted_properties.size(), 1U);
	const Superblock beforeSuperblock = before.superblock();
	const EntityPageLocation beforeNode = store->findNodePageLocation(before, 1001U);
	const EntityPropertyLocation beforeProperty = store->findEntityPropertyLocation(before, propertyKey);
	const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyBatches{};

	const auto expectRejected = [&](NativeGraphPropertyPageReplacementRequest forged, FormatError expected,
									const char *scenario) {
		SCOPED_TRACE(scenario);
		const std::array requests{std::move(forged)};
		bool materializerCalled = false;
		expectFormatError(
				[&] {
					static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
							noEntityReplacements, noEntityAppends, noEntityPools, requests, noPropertyBatches,
							noLargeValues, {}, LogSequenceNumber(3U),
							[&materializerCalled](std::span<EntityPageReplacementMaterialization>,
														std::span<EntityPageAppendBatchMaterialization>,
														std::span<EntityPageNewPoolBatchMaterialization>,
														std::span<NativeGraphPropertyPageReplacementMaterialization>,
														std::span<NativeGraphPropertyPageBatchMaterialization>,
														std::span<LargeValueMaterializationResult>) { materializerCalled = true; },
							{}));
				},
				expected);
		EXPECT_FALSE(materializerCalled);
		MetadataSnapshotPin unchanged = store->openSnapshot();
		EXPECT_EQ(unchanged.superblock(), beforeSuperblock);
		EXPECT_EQ(store->findNodePageLocation(unchanged, 1001U), beforeNode);
		EXPECT_EQ(store->findEntityPropertyLocation(unchanged, propertyKey), beforeProperty);
		EXPECT_EQ(properties.readEntity(unchanged, propertyKey), initialRoot);
		unchanged.release();
	};

	NativeGraphPropertyPageReplacementRequest invalidKey = validRequest;
	invalidKey.relocated_properties.push_back({});
	expectRejected(std::move(invalidKey), FormatError::FERR_INVALID_HEADER, "invalid relocation key");
	NativeGraphPropertyPageReplacementRequest missingSource = validRequest;
	missingSource.relocated_properties.push_back({EntityPropertyOwnerKind::EPOK_NODE, 1002U});
	expectRejected(std::move(missingSource), FormatError::FERR_INVALID_STATE, "owner without Property root");
	NativeGraphPropertyPageReplacementRequest conflictingUpsert = validRequest;
	conflictingUpsert.relocated_properties.push_back(propertyKey);
	expectRejected(std::move(conflictingUpsert), FormatError::FERR_INVALID_STATE, "relocation conflicts with its upsert");
	before.release();
}

TEST_F(RelocatableMetadataStoreTest, PublishesEdgePropertyReplacementAndErasureWithItsEdgeNamespace) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	EntityPageStore entities(io, *store);
	SlottedPagePool pages(io, *store);
	PropertyCollectionRootStore properties(pages, *store);
	const PropertyCollectionRootStoreOptions propertyOptions{PhysicalSlotId(41U), PhysicalSlotId(48U), 902U};
	constexpr uint64_t edgeId = 2001U;
	constexpr uint64_t secondEdgeId = 2002U;
	const EdgeEntityRecord edge{1001U, 1002U, 7U};
	const EdgeEntityRecord secondEdge{1002U, 1003U, 7U};
	const EntityPageNewPoolRequest edgePoolRequest{
			.segment_id = SegmentId(32U),
			.pool_owner_id = 901U,
			.generation = Generation(1U),
			.page_id = PageId(201U),
			.first_slot = PhysicalSlotId(40U),
			.last_slot = PhysicalSlotId(40U),
	};
	const std::array<EntityPageBatchNewPool, 1U> edgePools{{
			{.request = edgePoolRequest,
			 .record_kind = EntityPageRecordKind::EPRK_EDGE,
			 .edge_insertions = {{edgeId, edge}, {secondEdgeId, secondEdge}}},
	}};
	const EntityPropertyKey propertyKey{EntityPropertyOwnerKind::EPOK_EDGE, edgeId};
	const PropertyCollectionRoot initialRoot{
			edgeId, graph::Edge::typeId, {{3U, graph::PropertyValue("original edge Property root")}}};

	MetadataSnapshotPin empty = store->openSnapshot();
	const EntityPropertyRootMutationPlan initialPlan =
			properties.planEntityRootReplacement(empty, initialRoot, propertyOptions);
	ASSERT_EQ(initialPlan.kind, EntityPropertyRootMutationPlanKind::EPRMPK_NEW_PAGE_REQUIRED);
	static_cast<void>(entities.applyNativeGraphBatchTransaction(
			empty, {}, {}, edgePools, std::span(&initialPlan, 1U), properties, propertyOptions, LogSequenceNumber(2U)));
	empty.release();

	// A small replacement remains in the source Property page, proving that the
	// Edge namespace follows the same atomic upsert path as Node properties.
	MetadataSnapshotPin beforeReplacement = store->openSnapshot();
	PropertyCollectionRoot replacementRoot = initialRoot;
	replacementRoot.entries.front().value = graph::PropertyValue("replaced edge Property root");
	const EntityPropertyRootMutationPlan replacementPlan =
			properties.planEntityRootReplacement(beforeReplacement, replacementRoot, propertyOptions);
	ASSERT_EQ(replacementPlan.kind, EntityPropertyRootMutationPlanKind::EPRMPK_EXISTING_PAGE);
	ASSERT_TRUE(replacementPlan.page_replacement.has_value());
	ASSERT_EQ(replacementPlan.page_replacement->upserted_properties.front().key, propertyKey);
	static_cast<void>(entities.applyNativeGraphBatchTransaction(
			beforeReplacement, {}, {}, {}, std::span(&replacementPlan, 1U), properties, propertyOptions,
			LogSequenceNumber(3U)));
	beforeReplacement.release();

	// Fill the shared page from a second Edge owner, then enlarge the first root.
	// The replacement cannot fit in the source page and must relocate atomically.
	const PropertyCollectionRoot secondRoot{
			secondEdgeId, graph::Edge::typeId, {{3U, graph::PropertyValue(std::string(2600U, 's'))}}};
	MetadataSnapshotPin beforeSecondRoot = store->openSnapshot();
	const EntityPropertyRootMutationPlan secondPlan =
			properties.planEntityRootReplacement(beforeSecondRoot, secondRoot, propertyOptions);
	ASSERT_EQ(secondPlan.kind, EntityPropertyRootMutationPlanKind::EPRMPK_EXISTING_PAGE);
	static_cast<void>(entities.applyNativeGraphBatchTransaction(
			beforeSecondRoot, {}, {}, {}, std::span(&secondPlan, 1U), properties, propertyOptions, LogSequenceNumber(4U)));
	beforeSecondRoot.release();

	MetadataSnapshotPin beforeRelocation = store->openSnapshot();
	PropertyCollectionRoot relocationRoot = replacementRoot;
	relocationRoot.entries.front().value = graph::PropertyValue(std::string(2200U, 'r'));
	const EntityPropertyRootMutationPlan relocationPlan =
			properties.planEntityRootReplacement(beforeRelocation, relocationRoot, propertyOptions);
	ASSERT_EQ(relocationPlan.kind, EntityPropertyRootMutationPlanKind::EPRMPK_NEW_PAGE_REQUIRED);
	ASSERT_TRUE(relocationPlan.page_replacement.has_value());
	ASSERT_EQ(relocationPlan.page_replacement->relocated_properties, std::vector<EntityPropertyKey>{propertyKey});
	static_cast<void>(entities.applyNativeGraphBatchTransaction(
			beforeRelocation, {}, {}, {}, std::span(&relocationPlan, 1U), properties, propertyOptions, LogSequenceNumber(5U)));
	beforeRelocation.release();

	MetadataSnapshotPin beforeErasure = store->openSnapshot();
	EXPECT_EQ(properties.readEntity(beforeErasure, propertyKey), relocationRoot);
	const EntityPropertyRootMutationPlan erasurePlan = properties.planEntityRootReplacement(
			beforeErasure, {edgeId, graph::Edge::typeId, {}}, propertyOptions);
	ASSERT_EQ(erasurePlan.kind, EntityPropertyRootMutationPlanKind::EPRMPK_EXISTING_PAGE);
	ASSERT_TRUE(erasurePlan.page_replacement.has_value());
	ASSERT_EQ(erasurePlan.page_replacement->erased_properties, std::vector<EntityPropertyKey>{propertyKey});
	static_cast<void>(entities.applyNativeGraphBatchTransaction(
			beforeErasure, {}, {}, {}, std::span(&erasurePlan, 1U), properties, propertyOptions, LogSequenceNumber(6U)));
	beforeErasure.release();

	MetadataSnapshotPin current = store->openSnapshot();
	EXPECT_EQ(entities.readEdge(current, edgeId), edge);
	EXPECT_EQ(entities.readEdge(current, secondEdgeId), secondEdge);
	EXPECT_FALSE(store->findEntityPropertyLocationOptional(current, propertyKey).has_value());
	EXPECT_FALSE(properties.readEntityOptional(current, propertyKey).has_value());
	EXPECT_EQ(properties.readEntity(current, {EntityPropertyOwnerKind::EPOK_EDGE, secondEdgeId}), secondRoot);
	current.release();
}
