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
#include "graph/query/execution/MaterializedByteBudget.hpp"

#include "graph/core/Edge.hpp"
#include "graph/storage/relocatable/entity/NativeGraphMutationExecutor.hpp"
#include "graph/storage/relocatable/entity/PageBackedGraphReadAdapter.hpp"
#include "graph/core/Node.hpp"
#include "graph/storage/relocatable/entity/NativeGraphWritePlanner.hpp"
#include "graph/storage/relocatable/entity/NativeScalarIndexMutationPlanner.hpp"
#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/index/BTreeIndex.hpp"
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

TEST_F(NativeScalarIndexMutationPlannerTest, RefreshesPublishedRootCacheAfterSnapshotGenerationChanges) {
	const IndexDefinition label = nodeLabelDefinition();
	NativeScalarIndexMutationPlanner scalar(
			*metadata_, *btree_pages_, {label},
			[](const MetadataSnapshotPin &, std::string_view) -> std::optional<uint32_t> { return std::nullopt; });
	NativeGraphWritePlanner graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());

	NativeGraphWriteTransaction beforeRoot = graph.beginTransaction();
	scalar.stageNodeLabels(beforeRoot, 1U, std::array<uint64_t, 1U>{7U}, false);
	beforeRoot.rollback();

	static_cast<void>(allocateEmptyIndexRoot(*btree_pages_, label.index_id, 71U, 101U, 40U, 2U));
	NativeGraphWriteTransaction afterRoot = graph.beginTransaction();
	scalar.stageNodeLabels(afterRoot, 1U, std::array<uint64_t, 1U>{7U}, false);
	ASSERT_TRUE(afterRoot.commit().committed());

	BTreeIndex index(label.index_id, *btree_pages_, *metadata_);
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	const std::vector<std::byte> key =
			IndexKeyCodec::encodeEntryKey(PropertyValue(int64_t(7)), 1U);
	EXPECT_EQ(index.find(snapshot, key), std::optional<uint64_t>(1U));
	snapshot.release();
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

TEST_F(NativeScalarIndexMutationPlannerTest, PropertyDeltaSkipsUnchangedCanonicalKeysAndRejectsInvalidOwners) {
	allocateEmptyIndexRoot(*btree_pages_, 18U, 72U, 102U, 41U, 2U);
	IndexDefinition definition;
	definition.index_id = 18U;
	definition.name = "score";
	definition.entity_kind = IndexDefinitionEntityKind::IDEK_NODE;
	definition.kind = IndexDefinitionKind::IDK_PROPERTY;
	definition.property_names = {"score"};
	NativeScalarIndexMutationPlanner scalar(*metadata_, *btree_pages_, {definition},
		[](const MetadataSnapshotPin &, std::string_view) -> std::optional<uint32_t> { return 17U; });
	NativeGraphWritePlanner graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	auto transaction = graph.beginTransaction();
	const PropertyCollectionRoot before{1U, Node::typeId, {{17U, PropertyValue(42)}, {18U, PropertyValue("old")}}};
	const PropertyCollectionRoot after{1U, Node::typeId, {{17U, PropertyValue(42)}, {18U, PropertyValue("new")}}};
	scalar.stagePropertyDelta(transaction, IndexDefinitionEntityKind::IDEK_NODE, 1U, {}, before, {}, after);
	EXPECT_THROW(static_cast<void>(transaction.commit()), FormatException);
	auto invalid = after;
	invalid.owner_entity_id = 2U;
	EXPECT_THROW(scalar.stagePropertyDelta(transaction, IndexDefinitionEntityKind::IDEK_NODE, 1U, {}, before, {}, invalid), FormatException);
	EXPECT_THROW(scalar.stagePropertyDelta(transaction, IndexDefinitionEntityKind::IDEK_NODE, 1U,
		std::array<uint64_t, 2U>{2U, 1U}, before, {}, after), FormatException);
	transaction.rollback();
}

TEST_F(NativeScalarIndexMutationPlannerTest, PropertyDeltaMaintainsPartialMembershipAndPinnedOldIndex) {
	allocateEmptyIndexRoot(*btree_pages_, 18U, 72U, 102U, 41U, 2U);
	IndexDefinition definition;
	definition.index_id = 18U;
	definition.name = "score";
	definition.entity_kind = IndexDefinitionEntityKind::IDEK_NODE;
	definition.kind = IndexDefinitionKind::IDK_PROPERTY;
	definition.label_token_id = 7U;
	definition.property_names = {"score"};
	NativeScalarIndexMutationPlanner scalar(*metadata_, *btree_pages_, {definition},
		[](const MetadataSnapshotPin &, std::string_view) -> std::optional<uint32_t> { return 17U; });
	NativeGraphWritePlanner graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	const std::array<uint64_t, 1U> labels{7U};
	const PropertyCollectionRoot before{1U, Node::typeId, {{17U, PropertyValue(42)}}};
	const PropertyCollectionRoot after{1U, Node::typeId, {{17U, PropertyValue(43)}}};
	{
		auto seed = graph.beginTransaction();
		scalar.stageProperties(seed, IndexDefinitionEntityKind::IDEK_NODE, 1U, labels, before, false);
		ASSERT_TRUE(seed.commit().committed());
	}
	auto oldSnapshot = metadata_->openSnapshot();
	{
		auto update = graph.beginTransaction();
		scalar.stagePropertyDelta(update, IndexDefinitionEntityKind::IDEK_NODE, 1U, labels, before, labels, after);
		ASSERT_TRUE(update.commit().committed());
	}
	BTreeIndex index(18U, *btree_pages_, *metadata_);
	const auto oldKey = IndexKeyCodec::encodeEntryKey(PropertyValue(42), 1U);
	const auto newKey = IndexKeyCodec::encodeEntryKey(PropertyValue(43), 1U);
	auto current = metadata_->openSnapshot();
	EXPECT_EQ(index.find(oldSnapshot, oldKey), std::optional<uint64_t>(1U));
	EXPECT_FALSE(index.find(current, oldKey));
	EXPECT_EQ(index.find(current, newKey), std::optional<uint64_t>(1U));
	current.release();
	oldSnapshot.release();
	{
		auto remove = graph.beginTransaction();
		scalar.stagePropertyDelta(remove, IndexDefinitionEntityKind::IDEK_NODE, 1U, labels, after, {}, after);
		ASSERT_TRUE(remove.commit().committed());
	}
	current = metadata_->openSnapshot();
	EXPECT_FALSE(index.find(current, newKey));
	current.release();
}

TEST_F(NativeScalarIndexMutationPlannerTest, ExecutorPreservesUnchangedPropertyAndLabelIndexRoots) {
	allocateEmptyIndexRoot(*btree_pages_, 17U, 71U, 101U, 40U, 2U);
	allocateEmptyIndexRoot(*btree_pages_, 18U, 72U, 102U, 41U, 4U);
	IndexDefinition definition;
	definition.index_id = 18U;
	definition.name = "score";
	definition.entity_kind = IndexDefinitionEntityKind::IDEK_NODE;
	definition.kind = IndexDefinitionKind::IDK_PROPERTY;
	definition.property_names = {"score"};
	NativeGraphWritePlanner writer(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	NativeGraphMutationTokenResolvers tokens;
	tokens.schema_token = [](const MetadataSnapshotPin &, SystemCatalogSchemaTokenKind, std::string_view name)
		-> std::optional<uint64_t> { return name == "Person" ? std::optional<uint64_t>(7U) : std::nullopt; };
	tokens.property_key_token = [](const MetadataSnapshotPin &, std::string_view name) -> std::optional<uint32_t> {
		if (name == "score") return 17U;
		if (name == "other") return 18U;
		return std::nullopt;
	};
	tokens.property_key_name = [](const MetadataSnapshotPin &, uint32_t token) -> std::optional<std::string> {
		if (token == 17U) return "score";
		if (token == 18U) return "other";
		return std::nullopt;
	};
	NativeGraphMutationExecutorOptions options{8U, 8U, 64U, 8U, 64U, 8U, 8U, 16U, 4U};
	NativeGraphMutationExecutor executor(writer, graph, *btree_pages_, *vector_pages_, *vector_manifests_, *metadata_,
		tokens, [definition](const MetadataSnapshotPin &) { return std::vector<IndexDefinition>{nodeLabelDefinition(), definition}; },
		[](const MetadataSnapshotPin &) { return std::vector<ConstraintDefinition>{}; }, options);
	LogicalGraphMutationBatch create;
	create.node_creates = {{{"Person"}, {{"score", PropertyValue(42)}, {"other", PropertyValue("old")}}}};
	const auto created = executor.execute(create);
	ASSERT_TRUE(created.committed());
	const auto id = created.result.created_node_ids.front();
	auto oldSnapshot = metadata_->openSnapshot();
	const auto oldPropertyRoot = metadata_->findIndexRootOptional(oldSnapshot, 18U);
	const auto oldLabelRoot = metadata_->findIndexRootOptional(oldSnapshot, 17U);
	LogicalGraphMutationBatch update;
	update.property_replacements = {{{LogicalGraphMutationEntityKind::LGMK_NODE,
		LogicalGraphMutationReferenceKind::LGRK_PERSISTED, id}, {{"score", PropertyValue(42)}, {"other", PropertyValue("new")}}}};
	ASSERT_TRUE(executor.execute(update).committed());
	auto current = metadata_->openSnapshot();
	EXPECT_EQ(metadata_->findIndexRootOptional(current, 18U), oldPropertyRoot);
	EXPECT_EQ(metadata_->findIndexRootOptional(current, 17U), oldLabelRoot);
	EXPECT_EQ(graph.readNode(oldSnapshot, id).properties.back().value, PropertyValue("old"));
	EXPECT_EQ(graph.readNode(current, id).properties.back().value, PropertyValue("new"));
	current.release();
	oldSnapshot.release();
}

TEST_F(NativeScalarIndexMutationPlannerTest, NodeCompositeDeltaHandlesTypeChangesAndMissingFields) {
	allocateEmptyIndexRoot(*btree_pages_, 18U, 72U, 102U, 41U, 2U);
	IndexDefinition definition;
	definition.index_id = 18U;
	definition.name = "node_pair";
	definition.entity_kind = IndexDefinitionEntityKind::IDEK_NODE;
	definition.kind = IndexDefinitionKind::IDK_COMPOSITE;
	definition.property_names = {"first", "second"};
	NativeScalarIndexMutationPlanner scalar(*metadata_, *btree_pages_, {definition},
		[](const MetadataSnapshotPin &, std::string_view name) -> std::optional<uint32_t> {
			return name == "first" ? 17U : 18U;
		});
	NativeGraphWritePlanner graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	const PropertyCollectionRoot before{1U, Node::typeId, {{17U, PropertyValue(42)}, {18U, PropertyValue(7)}}};
	const PropertyCollectionRoot after{1U, Node::typeId, {{17U, PropertyValue("42")}, {18U, PropertyValue(7)}}};
	const PropertyCollectionRoot missing{1U, Node::typeId, {{17U, PropertyValue("42")}}};
	{
		auto seed = graph.beginTransaction();
		scalar.stageProperties(seed, IndexDefinitionEntityKind::IDEK_NODE, 1U, {}, before, false);
		ASSERT_TRUE(seed.commit().committed());
	}
	{
		auto update = graph.beginTransaction();
		EXPECT_THROW(scalar.stagePropertyDelta(update, IndexDefinitionEntityKind::IDEK_NODE, 1U,
			std::array<uint64_t, 2U>{7U, 1U}, before, {}, after), FormatException);
		scalar.stagePropertyDelta(update, IndexDefinitionEntityKind::IDEK_NODE, 1U, {}, before, {}, after);
		ASSERT_TRUE(update.commit().committed());
	}
	BTreeIndex index(18U, *btree_pages_, *metadata_);
	const auto oldKey = IndexKeyCodec::tryEncodeCompositeEntryKey(std::vector<PropertyValue>{PropertyValue(42), PropertyValue(7)}, 1U);
	const auto newKey = IndexKeyCodec::tryEncodeCompositeEntryKey(std::vector<PropertyValue>{PropertyValue("42"), PropertyValue(7)}, 1U);
	ASSERT_TRUE(oldKey);
	ASSERT_TRUE(newKey);
	auto current = metadata_->openSnapshot();
	EXPECT_FALSE(index.find(current, *oldKey));
	EXPECT_EQ(index.find(current, *newKey), std::optional<uint64_t>(1U));
	current.release();
	{
		auto remove = graph.beginTransaction();
		scalar.stagePropertyDelta(remove, IndexDefinitionEntityKind::IDEK_NODE, 1U, {}, after, {}, missing);
		ASSERT_TRUE(remove.commit().committed());
	}
	current = metadata_->openSnapshot();
	EXPECT_FALSE(index.find(current, *newKey));
	current.release();
}

TEST_F(NativeScalarIndexMutationPlannerTest, EdgePropertyDeltaRemovesAnIndexedField) {
	allocateEmptyIndexRoot(*btree_pages_, 18U, 72U, 102U, 41U, 2U);
	IndexDefinition definition;
	definition.index_id = 18U;
	definition.name = "edge_score";
	definition.entity_kind = IndexDefinitionEntityKind::IDEK_EDGE;
	definition.kind = IndexDefinitionKind::IDK_PROPERTY;
	definition.property_names = {"score"};
	NativeScalarIndexMutationPlanner scalar(*metadata_, *btree_pages_, {definition},
		[](const MetadataSnapshotPin &, std::string_view) -> std::optional<uint32_t> { return 17U; });
	NativeGraphWritePlanner graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	const PropertyCollectionRoot before{1U, Edge::typeId, {{17U, PropertyValue(42)}}};
	const PropertyCollectionRoot after{1U, Edge::typeId, {}};
	{
		auto seed = graph.beginTransaction();
		scalar.stageProperties(seed, IndexDefinitionEntityKind::IDEK_EDGE, 1U, {}, before, false);
		ASSERT_TRUE(seed.commit().committed());
	}
	{
		auto remove = graph.beginTransaction();
		EXPECT_THROW(scalar.stagePropertyDelta(remove, IndexDefinitionEntityKind::IDEK_EDGE, 1U,
			std::array<uint64_t, 1U>{7U}, before, {}, after), FormatException);
		scalar.stagePropertyDelta(remove, IndexDefinitionEntityKind::IDEK_EDGE, 1U, {}, before, {}, after);
		ASSERT_TRUE(remove.commit().committed());
	}
	BTreeIndex index(18U, *btree_pages_, *metadata_);
	auto current = metadata_->openSnapshot();
	EXPECT_FALSE(index.find(current, IndexKeyCodec::encodeEntryKey(PropertyValue(42), 1U)));
	current.release();
}

TEST_F(NativeScalarIndexMutationPlannerTest, BorrowedPropertyDeltaVisitorSharesScalarAndCompositeKeyRulesWithoutWriting) {
    allocateEmptyIndexRoot(*btree_pages_, 18, 72, 102, 41, 2);
    allocateEmptyIndexRoot(*btree_pages_, 19, 73, 103, 42, 4);
    IndexDefinition single; single.index_id = 18; single.name = "score";
    single.entity_kind = IndexDefinitionEntityKind::IDEK_NODE; single.kind = IndexDefinitionKind::IDK_PROPERTY;
    single.property_names = {"score"}; single.label_token_id = 7;
    auto composite = single; composite.index_id = 19; composite.name = "score_tag";
    composite.kind = IndexDefinitionKind::IDK_COMPOSITE; composite.property_names = {"score", "tag"};
    NativeScalarIndexMutationPlanner scalar(*metadata_, *btree_pages_, {single, composite},
        [](const MetadataSnapshotPin &, std::string_view name) -> std::optional<uint32_t> { return name == "score" ? 17U : 18U; });
    const PropertyCollectionRoot before{1, Node::typeId, {{17, PropertyValue(42)}, {18, PropertyValue("a")}}};
    const PropertyCollectionRoot after{1, Node::typeId, {{17, PropertyValue(43)}, {18, PropertyValue("b")}}};
    const std::array<uint64_t, 1> labels{7}; auto pin = metadata_->openSnapshot();
    struct Delta { uint64_t index, owner; bool erase; std::vector<std::byte> key; };
    std::vector<Delta> deltas; const auto epoch = io_->dataMutationEpoch(); const auto generation = pin.superblock().generation;
    const NativeScalarIndexMutationPlanner::PropertyDeltaVisitor visitor = [&](uint64_t index, std::span<const std::byte> key, uint64_t owner, bool erase) {
        deltas.push_back({index, owner, erase, {key.begin(), key.end()}});
    };
    scalar.visitPropertyDelta(pin, IndexDefinitionEntityKind::IDEK_NODE, 1, labels, before, labels, after, visitor);
    ASSERT_EQ(deltas.size(), 4U);
    EXPECT_EQ(deltas[0].index, 18U); EXPECT_TRUE(deltas[0].erase);
    EXPECT_EQ(deltas[1].index, 18U); EXPECT_FALSE(deltas[1].erase);
    EXPECT_EQ(deltas[0].key, IndexKeyCodec::encodeEntryKey(PropertyValue(42), 1));
    EXPECT_EQ(deltas[1].key, IndexKeyCodec::encodeEntryKey(PropertyValue(43), 1));
    const std::array oldTuple{PropertyValue(42), PropertyValue("a")};
    const std::array newTuple{PropertyValue(43), PropertyValue("b")};
    EXPECT_EQ(deltas[2].key, IndexKeyCodec::encodeCompositeEntryKey(oldTuple, 1));
    EXPECT_EQ(deltas[3].key, IndexKeyCodec::encodeCompositeEntryKey(newTuple, 1));
    for (const auto &delta : deltas) EXPECT_EQ(delta.owner, 1U);
    deltas.clear(); scalar.visitPropertyDelta(pin, IndexDefinitionEntityKind::IDEK_NODE, 1, labels, before, labels, before, visitor);
    EXPECT_TRUE(deltas.empty());
    scalar.visitPropertyDelta(pin, IndexDefinitionEntityKind::IDEK_NODE, 1, labels, before, {}, after, visitor);
    ASSERT_EQ(deltas.size(), 2U); EXPECT_TRUE(deltas[0].erase); EXPECT_TRUE(deltas[1].erase);
    EXPECT_EQ(io_->dataMutationEpoch(), epoch); EXPECT_EQ(metadata_->openSnapshot().superblock().generation, generation);
}
TEST_F(NativeScalarIndexMutationPlannerTest, BorrowedDeltaVisitorPropagatesPreparationFailureWithoutPublication) {
    allocateEmptyIndexRoot(*btree_pages_, 18, 72, 102, 41, 2);
    IndexDefinition definition; definition.index_id = 18; definition.name = "score";
    definition.entity_kind = IndexDefinitionEntityKind::IDEK_NODE; definition.kind = IndexDefinitionKind::IDK_PROPERTY;
    definition.property_names = {"score"};
    NativeScalarIndexMutationPlanner scalar(*metadata_, *btree_pages_, {definition},
        [](const MetadataSnapshotPin &, std::string_view) -> std::optional<uint32_t> { return 17; });
    const PropertyCollectionRoot before{1, Node::typeId, {{17, PropertyValue(42)}}};
    const PropertyCollectionRoot after{1, Node::typeId, {{17, PropertyValue(43)}}};
    auto pin = metadata_->openSnapshot(); const auto epoch = io_->dataMutationEpoch();
    EXPECT_THROW(scalar.visitPropertyDelta(pin, IndexDefinitionEntityKind::IDEK_NODE, 1, {}, before, {}, after,
        [](auto, auto, auto, auto) { throw std::runtime_error("preparation refused"); }), std::runtime_error);
    EXPECT_EQ(io_->dataMutationEpoch(), epoch); EXPECT_EQ(metadata_->openSnapshot().superblock(), pin.superblock());
}
TEST_F(NativeScalarIndexMutationPlannerTest, BorrowedDeltaVisitorRequiresDomainAndCallbackEvenWithoutDefinitions) {
    NativeScalarIndexMutationPlanner scalar(*metadata_, *btree_pages_, {},
        [](const MetadataSnapshotPin &, std::string_view) -> std::optional<uint32_t> { return {}; });
    const PropertyCollectionRoot root{1, Node::typeId, {}}; auto pin = metadata_->openSnapshot();
    EXPECT_THROW(scalar.visitPropertyDelta(pin, IndexDefinitionEntityKind::IDEK_NODE, 1, {}, root, {}, root, {}), FormatException);
    const auto foreignPath = std::filesystem::path(path_.string() + "_foreign");
    {
        auto file = std::make_shared<std::fstream>(foreignPath, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
        auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
        auto other = RelocatableMetadataStore::initializeNew(io, graph::storage::test::nativeGraphInitialSuperblock());
        auto foreign = other->openSnapshot();
        EXPECT_EQ(foreign.superblock().database_id, pin.superblock().database_id);
        EXPECT_EQ(foreign.superblock().generation, pin.superblock().generation);
        EXPECT_THROW(scalar.visitPropertyDelta(foreign, IndexDefinitionEntityKind::IDEK_NODE, 1, {}, root, {}, root,
            [](auto, auto, auto, auto) {}), FormatException);
    }
    std::filesystem::remove(foreignPath);
}

TEST_F(NativeScalarIndexMutationPlannerTest, PublishedRootCacheRetainsItsAllocationOwnerOutsideTheConstructionScope) {
    allocateEmptyIndexRoot(*btree_pages_, 18, 72, 102, 41, 2);
    IndexDefinition definition; definition.index_id = 18; definition.name = "score";
    definition.entity_kind = IndexDefinitionEntityKind::IDEK_NODE; definition.kind = IndexDefinitionKind::IDK_PROPERTY;
    definition.property_names = {"score"};
    auto memory = std::make_shared<query::execution::MaterializedByteBudget>(65536);
    std::unique_ptr<NativeScalarIndexMutationPlanner> scalar;
    {
        utils::OwnedAllocationScope scope(memory);
        scalar = std::make_unique<NativeScalarIndexMutationPlanner>(*metadata_, *btree_pages_,
            utils::OwnedStdVector<IndexDefinition>{definition},
            [](const MetadataSnapshotPin &, std::string_view) -> std::optional<uint32_t> { return 17; });
    }
    const auto retainedDefinitions = memory->used(); auto pin = metadata_->openSnapshot();
    const PropertyCollectionRoot missing{1, Node::typeId, {}};
    scalar->visitPropertyDelta(pin, IndexDefinitionEntityKind::IDEK_NODE, 1, {}, missing, {}, missing,
        [](auto, auto, auto, auto) { ADD_FAILURE() << "missing values must not produce an index key"; });
    EXPECT_GT(memory->used(), retainedDefinitions); const auto populated = memory->used();
    scalar->visitPropertyDelta(pin, IndexDefinitionEntityKind::IDEK_NODE, 1, {}, missing, {}, missing,
        [](auto, auto, auto, auto) { ADD_FAILURE(); });
    EXPECT_EQ(memory->used(), populated);
    scalar.reset(); EXPECT_EQ(memory->used(), 0U);
}
