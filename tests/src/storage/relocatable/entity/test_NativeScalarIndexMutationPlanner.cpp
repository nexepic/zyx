/**
 * @file test_NativeScalarIndexMutationPlanner.cpp
 * @brief Public validation tests for native scalar Index mutation planning.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include <limits>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

#include "graph/core/Edge.hpp"
#include "graph/core/Node.hpp"
#include "graph/storage/relocatable/entity/NativeGraphWritePlanner.hpp"
#include "graph/storage/relocatable/entity/NativeScalarIndexMutationPlanner.hpp"
#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"
#include "graph/storage/relocatable/index/IndexKeyCodec.hpp"
#include "graph/storage/relocatable/schema/SystemCatalog.hpp"
#include "storage/relocatable/entity/NativeGraphWritePlannerTestFixture.hpp"

using namespace graph;
using namespace graph::storage;

namespace {

	IndexDefinition nodeLabelDefinition(uint64_t indexId = 17U) {
		IndexDefinition definition;
		definition.index_id = indexId;
		definition.name = "node_labels";
		definition.entity_kind = IndexDefinitionEntityKind::IDEK_NODE;
		definition.kind = IndexDefinitionKind::IDK_LABEL;
		return definition;
	}

	IndexRootLocation allocateEmptyIndexRoot(BTreePageStore &indexes, uint64_t indexId, uint64_t segmentId,
											 uint64_t pageId, uint64_t slot, uint64_t checkpointLsn) {
		SlottedPagePoolAllocationRequest allocation;
		allocation.segment_id = SegmentId(segmentId);
		allocation.pool_kind = SegmentKind::SGK_INDEX_PAGE_POOL;
		allocation.pool_owner_id = indexId;
		allocation.generation = Generation(1U);
		allocation.page_id = PageId(pageId);
		allocation.page_owner_id = indexId;
		allocation.page_owner_kind = PageOwnerKind::POK_INDEX;
		PageOwnerHeader owner;
		owner.page_id = allocation.page_id;
		owner.owner_id = allocation.page_owner_id;
		owner.kind = allocation.page_owner_kind;
		owner.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
		owner.generation = allocation.generation;
		owner.committed_lsn = LogSequenceNumber(checkpointLsn);
		BTreePageHeader header;
		header.index_id = indexId;
		header.page_id = allocation.page_id;
		header.generation = allocation.generation;
		if (!indexes.allocate(allocation, BTreePage::create(owner, header), PhysicalSlotId(slot), PhysicalSlotId(slot),
							LogSequenceNumber(checkpointLsn))
					 .has_value()) {
			throw std::runtime_error("native scalar-index planner test could not allocate an Index root");
		}
		const IndexRootLocation root{indexId, allocation.page_id, allocation.generation};
		static_cast<void>(indexes.publishRoots(std::span(&root, 1U), {}, LogSequenceNumber(checkpointLsn + 1U)));
		return root;
	}

	NativeGraphWritePlannerOptions plannerOptions() {
		NativeGraphWritePlannerOptions options;
		options.first_slot = PhysicalSlotId(40U);
		options.last_slot = PhysicalSlotId(80U);
		options.entity_pool_owner_id = 901U;
		options.logical_id_lease_block_size = 1U;
		options.property_roots = {PhysicalSlotId(40U), PhysicalSlotId(80U), 902U};
		return options;
	}

} // namespace

class NativeScalarIndexMutationPlannerTest : public graph::storage::test::NativeGraphWritePlannerTestFixture {};

TEST_F(NativeScalarIndexMutationPlannerTest, RejectsMalformedDefinitionSetsAndResolverContracts) {
	const IndexDefinition valid = nodeLabelDefinition();
	const auto resolver = [](const MetadataSnapshotPin &, std::string_view) -> std::optional<uint32_t> {
		return std::nullopt;
	};

	IndexDefinition zeroId = valid;
	zeroId.index_id = 0U;
	EXPECT_THROW(static_cast<void>(NativeScalarIndexMutationPlanner(*metadata_, *btree_pages_, {zeroId}, resolver)),
			 FormatException);

	IndexDefinition duplicate = valid;
	EXPECT_THROW(static_cast<void>(
				 NativeScalarIndexMutationPlanner(*metadata_, *btree_pages_, {valid, duplicate}, resolver)),
			 FormatException);

	IndexDefinition property = valid;
	property.index_id = 18U;
	property.kind = IndexDefinitionKind::IDK_PROPERTY;
	EXPECT_THROW(static_cast<void>(NativeScalarIndexMutationPlanner(*metadata_, *btree_pages_, {property}, resolver)),
			 FormatException);

	EXPECT_THROW(static_cast<void>(NativeScalarIndexMutationPlanner(*metadata_, *btree_pages_, {valid}, {})),
			 std::invalid_argument);
	{
		auto foreignMetadata = RelocatableMetadataStore::open(io_);
		SlottedPagePool foreignPages(io_, *foreignMetadata);
		BTreePageStore foreignIndexes(foreignPages, *foreignMetadata);
		EXPECT_THROW(static_cast<void>(NativeScalarIndexMutationPlanner(*metadata_, foreignIndexes, {valid}, resolver)),
				 std::invalid_argument);
	}

}

TEST_F(NativeScalarIndexMutationPlannerTest, RejectsInvalidMutationInputsBeforeStagingAnyIndexOperation) {
	const IndexDefinition valid = nodeLabelDefinition();
	NativeScalarIndexMutationPlanner scalar(
			*metadata_, *btree_pages_, {valid},
			[](const MetadataSnapshotPin &, std::string_view) -> std::optional<uint32_t> { return std::nullopt; });
	ASSERT_EQ(scalar.definitions().size(), 1U);
	EXPECT_EQ(scalar.definitions().front().index_id, valid.index_id);
	NativeGraphWritePlanner graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	NativeGraphWriteTransaction transaction = graph.beginTransaction();

	EXPECT_THROW(scalar.stageNodeLabels(transaction, 0U, std::array<uint64_t, 0U>{}, false), FormatException);
	EXPECT_THROW(scalar.stageNodeLabels(transaction, 1U, std::array<uint64_t, 1U>{0U}, false), FormatException);
	EXPECT_THROW(scalar.stageNodeLabels(transaction, 1U, std::array<uint64_t, 2U>{7U, 7U}, false), FormatException);
	EXPECT_THROW(scalar.stageNodeLabels(transaction, 1U, std::array<uint64_t, 2U>{9U, 7U}, false), FormatException);
	EXPECT_THROW(scalar.stageEdgeType(transaction, 0U, 7U, false), FormatException);
	EXPECT_NO_THROW(scalar.stageEdgeType(transaction, 1U, RSS_UNTYPED_RELATIONSHIP_TYPE_TOKEN, false));
	EXPECT_THROW(scalar.stageEdgeType(transaction,
							 1U,
							 static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1U,
							 false),
			 FormatException);

	const PropertyCollectionRoot nodeRoot = {1U, Node::typeId, {{17U, PropertyValue(int64_t(1))}}};
	const PropertyCollectionRoot edgeRoot = {1U, Edge::typeId, {{17U, PropertyValue(int64_t(1))}}};
	EXPECT_THROW(scalar.stageProperties(transaction, static_cast<IndexDefinitionEntityKind>(255U), 1U, {}, nodeRoot,
									 false),
			 FormatException);
	EXPECT_THROW(scalar.stageProperties(transaction, IndexDefinitionEntityKind::IDEK_EDGE, 1U,
									 std::array<uint64_t, 1U>{7U}, edgeRoot, false),
			 FormatException);
	EXPECT_THROW(scalar.stageProperties(transaction, IndexDefinitionEntityKind::IDEK_NODE, 1U,
									 std::array<uint64_t, 1U>{7U}, edgeRoot, false),
			 FormatException);
	const PropertyCollectionRoot duplicateKeys = {
			1U, Node::typeId, {{17U, PropertyValue(int64_t(1))}, {17U, PropertyValue(int64_t(2))}}};
	EXPECT_THROW(scalar.stageProperties(transaction, IndexDefinitionEntityKind::IDEK_NODE, 1U, {}, duplicateKeys, false),
			 FormatException);
	const PropertyCollectionRoot zeroOwner = {0U, Node::typeId, {{17U, PropertyValue(int64_t(1))}}};
	EXPECT_THROW(scalar.stageProperties(transaction, IndexDefinitionEntityKind::IDEK_NODE, 0U, {}, zeroOwner, false),
			 FormatException);
	const PropertyCollectionRoot zeroToken = {1U, Node::typeId, {{0U, PropertyValue(int64_t(1))}}};
	EXPECT_THROW(scalar.stageProperties(transaction, IndexDefinitionEntityKind::IDEK_NODE, 1U, {}, zeroToken, false),
			 FormatException);

	const std::array<std::byte, 1U> key = {std::byte{0x42}};
	EXPECT_THROW(scalar.stageConstraintSupportUpsert(transaction, 1U, key, 1U), FormatException);
	EXPECT_THROW(scalar.stageConstraintSupportUpsert(transaction, RSS_SYSTEM_CATALOG_CONSTRAINT_ID_FIRST, {}, 1U),
			 FormatException);
	EXPECT_THROW(scalar.stageConstraintSupportErase(transaction, RSS_SYSTEM_CATALOG_CONSTRAINT_ID_FIRST, key, 0U),
			 FormatException);
	transaction.rollback();
}

TEST_F(NativeScalarIndexMutationPlannerTest,
       AppliesActiveIndexValidationOnlyWhenThePublishedDefinitionNeedsAnIndexKey) {
	const IndexRootLocation labelRoot = allocateEmptyIndexRoot(*btree_pages_, 17U, 71U, 101U, 40U, 2U);
	const IndexRootLocation propertyRoot = allocateEmptyIndexRoot(*btree_pages_, 18U, 72U, 102U, 41U, 4U);
	EXPECT_EQ(labelRoot.index_id, 17U);
	EXPECT_EQ(propertyRoot.index_id, 18U);

	IndexDefinition label = nodeLabelDefinition(labelRoot.index_id);
	IndexDefinition property;
	property.index_id = propertyRoot.index_id;
	property.name = "node_score";
	property.entity_kind = IndexDefinitionEntityKind::IDEK_NODE;
	property.kind = IndexDefinitionKind::IDK_PROPERTY;
	property.property_names = {"score"};
	NativeScalarIndexMutationPlanner scalar(
			*metadata_, *btree_pages_, {label, property},
			[](const MetadataSnapshotPin &, std::string_view) -> std::optional<uint32_t> { return 0U; });
	NativeGraphWritePlanner graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	NativeGraphWriteTransaction transaction = graph.beginTransaction();

	EXPECT_THROW(scalar.stageNodeLabels(transaction, 1U,
								 std::array<uint64_t, 1U>{static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1U},
								 false),
			 FormatException);
	const PropertyCollectionRoot root = {1U, Node::typeId, {{17U, PropertyValue(int64_t(42))}}};
	EXPECT_THROW(scalar.stageProperties(transaction, IndexDefinitionEntityKind::IDEK_NODE, 1U, {}, root, false),
			 FormatException);
	transaction.rollback();
}

TEST_F(NativeScalarIndexMutationPlannerTest,
       SkipsActivePropertyIndexEntriesWhenTheResolvedValueCannotProduceACanonicalKey) {
	const IndexRootLocation rootLocation = allocateEmptyIndexRoot(*btree_pages_, 18U, 72U, 102U, 41U, 2U);
	IndexDefinition property;
	property.index_id = rootLocation.index_id;
	property.name = "node_score";
	property.entity_kind = IndexDefinitionEntityKind::IDEK_NODE;
	property.kind = IndexDefinitionKind::IDK_PROPERTY;
	property.property_names = {"score"};
	NativeGraphWritePlanner graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	const PropertyCollectionRoot present = {1U, Node::typeId, {{17U, PropertyValue(int64_t(42))}}};
	const PropertyCollectionRoot absent = {1U, Node::typeId, {{18U, PropertyValue(int64_t(42))}}};
	const PropertyCollectionRoot oversized = {
			1U, Node::typeId, {{17U, PropertyValue(std::string(RSS_INDEX_KEY_MAX_BYTES, 'x'))}}};

	NativeScalarIndexMutationPlanner missingToken(
			*metadata_, *btree_pages_, {property},
			[](const MetadataSnapshotPin &, std::string_view) -> std::optional<uint32_t> { return std::nullopt; });
	NativeGraphWriteTransaction missingTokenTransaction = graph.beginTransaction();
	EXPECT_NO_THROW(missingToken.stageProperties(missingTokenTransaction, IndexDefinitionEntityKind::IDEK_NODE, 1U,
																	 {}, present, false));
	EXPECT_THROW(static_cast<void>(missingTokenTransaction.commit()), FormatException);
	EXPECT_EQ(missingTokenTransaction.state(), NativeGraphWriteTransactionState::NGWTS_ACTIVE);
	missingTokenTransaction.rollback();

	NativeScalarIndexMutationPlanner missingValue(
			*metadata_, *btree_pages_, {property},
			[](const MetadataSnapshotPin &, std::string_view) -> std::optional<uint32_t> { return 17U; });
	NativeGraphWriteTransaction missingValueTransaction = graph.beginTransaction();
	EXPECT_NO_THROW(missingValue.stageProperties(missingValueTransaction, IndexDefinitionEntityKind::IDEK_NODE, 1U,
																	 {}, absent, false));
	EXPECT_THROW(static_cast<void>(missingValueTransaction.commit()), FormatException);
	EXPECT_EQ(missingValueTransaction.state(), NativeGraphWriteTransactionState::NGWTS_ACTIVE);
	missingValueTransaction.rollback();

	NativeGraphWriteTransaction oversizedTransaction = graph.beginTransaction();
	EXPECT_NO_THROW(missingValue.stageProperties(oversizedTransaction, IndexDefinitionEntityKind::IDEK_NODE, 1U, {},
																	 oversized, false));
	EXPECT_THROW(static_cast<void>(oversizedTransaction.commit()), FormatException);
	EXPECT_EQ(oversizedTransaction.state(), NativeGraphWriteTransactionState::NGWTS_ACTIVE);
	oversizedTransaction.rollback();
}
