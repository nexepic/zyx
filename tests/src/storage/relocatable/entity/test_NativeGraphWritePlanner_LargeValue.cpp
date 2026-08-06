/**
 * @file test_NativeGraphWritePlanner_LargeValue.cpp
 * @brief Production-boundary LargeValue tests for native graph writes.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "graph/core/Node.hpp"
#include "graph/core/PropertyTypes.hpp"
#include "graph/storage/relocatable/entity/NativeGraphMutationExecutor.hpp"
#include "graph/storage/relocatable/entity/NativeGraphWritePlanner.hpp"
#include "graph/storage/relocatable/entity/PageBackedGraphReadAdapter.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"
#include "graph/storage/relocatable/maintenance/RelocatableIntegrityScanner.hpp"
#include "graph/storage/relocatable/property/LargeValueStore.hpp"
#include "storage/relocatable/entity/NativeGraphWritePlannerTestFixture.hpp"

using namespace graph::storage;
using graph::PropertyValue;

namespace {

	constexpr uint64_t PERSON_LABEL_TOKEN = 7U;
	constexpr uint32_t BODY_PROPERTY_TOKEN = 17U;

	NativeGraphMutationTokenResolvers largeValueTokens() {
		NativeGraphMutationTokenResolvers tokens;
		tokens.schema_token = [](const MetadataSnapshotPin &, SystemCatalogSchemaTokenKind kind,
									 std::string_view name) -> std::optional<uint64_t> {
			return kind == SystemCatalogSchemaTokenKind::SCSTK_LABEL && name == "Person"
					   ? std::optional<uint64_t>(PERSON_LABEL_TOKEN)
					   : std::nullopt;
		};
		tokens.property_key_token = [](const MetadataSnapshotPin &, std::string_view name) -> std::optional<uint32_t> {
			return name == "body" ? std::optional<uint32_t>(BODY_PROPERTY_TOKEN) : std::nullopt;
		};
		tokens.property_key_name = [](const MetadataSnapshotPin &, uint32_t token) -> std::optional<std::string> {
			return token == BODY_PROPERTY_TOKEN ? std::optional<std::string>("body") : std::nullopt;
		};
		return tokens;
	}

	NativeGraphMutationExecutorOptions executorOptions() {
		NativeGraphMutationExecutorOptions options;
		options.maximum_index_definitions = 8U;
		options.maximum_constraint_definitions = 8U;
		options.maximum_constraint_entity_scan = 64U;
		options.maximum_constraint_support_collisions = 8U;
		options.maximum_vector_records = 64U;
		options.maximum_vector_pages = 8U;
		options.vector_delta_rebuild_threshold = 8U;
		options.maximum_vector_delta_overlay_entries = 16U;
		options.maximum_vector_delta_pages = 4U;
		return options;
	}

	MetadataPublishOptions withBootstrapFailpoint(BootstrapPublishFailpoint failpoint) {
		MetadataPublishOptions options;
		options.bootstrap_failpoint = failpoint;
		return options;
	}

	EntityPropertyLargeValueReference storedLargeValueReference(const MetadataSnapshotPin &snapshot,
															const RelocatableMetadataStore &metadata,
															const SlottedPagePool &pages, uint64_t nodeId) {
		const EntityPropertyKey key{EntityPropertyOwnerKind::EPOK_NODE, nodeId};
		const EntityPropertyLocation location = metadata.findEntityPropertyLocation(snapshot, key);
		const SlottedPage page = pages.read(snapshot, location.page_id);
		const std::vector<std::byte> bytes = page.read({location.page_id, location.slot_id, location.slot_generation});
		if (!EntityPropertyLargeValueReferenceCodec::hasMagic(bytes)) {
			throw std::runtime_error("native LargeValue test expected an Entity Property LargeValue reference");
		}
		return EntityPropertyLargeValueReferenceCodec::decode(bytes);
	}

	std::vector<std::byte> encodedEntityPropertyRoot(uint64_t nodeId, std::string_view body) {
		return EntityPropertyCollectionRootCodec::encode(
				{nodeId, graph::Node::typeId, {{BODY_PROPERTY_TOKEN, PropertyValue(std::string(body))}}});
	}

	PropertyCollectionRoot entityPropertyRoot(uint64_t nodeId, std::string_view body) {
		return {nodeId, graph::Node::typeId,
				{{BODY_PROPERTY_TOKEN, PropertyValue(std::string(body))}}};
	}

	struct LargeValuePoolIds {
		std::set<uint64_t> data;
		std::set<uint64_t> manifest;
	};

	LargeValuePoolIds collectLargeValuePoolIds(const MetadataSnapshotPin &snapshot,
										 const RelocatableMetadataStore &metadata, const LargeValueStore &store,
										 std::span<const LargeValueRef> references) {
		LargeValuePoolIds result;
		for (const LargeValueRef &reference: references) {
			for (const LargeValueManifest &node: store.manifestTree(snapshot, store.manifest(snapshot, reference))) {
				result.manifest.insert(metadata.findPageLocation(snapshot, node.page_id).segment_id.value());
				for (const LargeValueChunk &chunk: node.chunks) {
					for (uint16_t ordinal = 0U; ordinal < chunk.data_page_count; ++ordinal) {
						result.data.insert(metadata.findPageLocation(
										 snapshot, PageId(chunk.first_data_page_id.value() + ordinal))
									 .segment_id.value());
					}
				}
			}
		}
		return result;
	}

} // namespace

class NativeGraphLargeValueMutationTest : public graph::storage::test::NativeGraphWritePlannerTestFixture {
protected:
	std::unique_ptr<LargeValueStore> large_values_;

	void SetUp() override {
		NativeGraphWritePlannerTestFixture::SetUp();
		properties_.reset();
		large_values_ = std::make_unique<LargeValueStore>(io_, *metadata_);
		properties_ = std::make_unique<PropertyCollectionRootStore>(*slotted_pages_, *large_values_, *metadata_);
	}

	void TearDown() override {
		properties_.reset();
		large_values_.reset();
		NativeGraphWritePlannerTestFixture::TearDown();
	}

	void reopenWithLargeValues() {
		large_values_.reset();
		NativeGraphWritePlannerTestFixture::reopenNativeStores();
		properties_.reset();
		large_values_ = std::make_unique<LargeValueStore>(io_, *metadata_);
		properties_ = std::make_unique<PropertyCollectionRootStore>(*slotted_pages_, *large_values_, *metadata_);
	}
};

TEST_F(NativeGraphLargeValueMutationTest, ExternalizesOversizedLogicalPropertiesInTheNativeGraphGeneration) {
	const std::string firstBody(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, 'a');
	const std::string replacementBody(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES + 97U, 'b');
	uint64_t nodeId = 0U;
	{
		BTreePageStore indexes(*slotted_pages_, *metadata_);
		NativeGraphWritePlanner writer(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
		PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
		NativeGraphMutationExecutor executor(
				writer, graph, indexes, *vector_pages_, *vector_manifests_, *metadata_, largeValueTokens(),
				[](const MetadataSnapshotPin &) { return std::vector<IndexDefinition>{}; },
				[](const MetadataSnapshotPin &) { return std::vector<ConstraintDefinition>{}; }, executorOptions());

		MetadataSnapshotPin beforeCreate = metadata_->openSnapshot();
		LogicalGraphMutationBatch create;
		create.node_creates = {{{"Person"}, {{"body", PropertyValue(firstBody)}}}};
		const NativeGraphMutationExecutionResult created = executor.execute(create);
		ASSERT_TRUE(created.committed());
		ASSERT_EQ(created.result.created_node_ids.size(), 1U);
		nodeId = created.result.created_node_ids.front();
		EXPECT_FALSE(metadata_->findLargeValueManifestOptional(beforeCreate, LargeValueId(1U)).has_value());
		EXPECT_THROW(static_cast<void>(graph.readNode(beforeCreate, nodeId)), FormatException);
		beforeCreate.release();

		MetadataSnapshotPin beforeReplace = metadata_->openSnapshot();
		const EntityPropertyLargeValueReference firstReference =
				storedLargeValueReference(beforeReplace, *metadata_, *slotted_pages_, nodeId);
		EXPECT_EQ(firstReference.value.value_id, LargeValueId(1U));
		EXPECT_EQ(graph.readNode(beforeReplace, nodeId).properties,
				  (std::vector<PropertyCollectionRootEntry>{{BODY_PROPERTY_TOKEN, PropertyValue(firstBody)}}));
		ASSERT_TRUE(metadata_->findLargeValueManifestOptional(beforeReplace, LargeValueId(1U)).has_value());

		LogicalGraphMutationBatch replace;
		replace.property_replacements = {
				{{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, nodeId},
				 {{"body", PropertyValue(replacementBody)}}}};
		ASSERT_TRUE(executor.execute(replace).committed());

		// The old logical root and its immutable value remain readable by a pinned snapshot.
		EXPECT_EQ(storedLargeValueReference(beforeReplace, *metadata_, *slotted_pages_, nodeId).value.value_id,
			  LargeValueId(1U));
		EXPECT_EQ(graph.readNode(beforeReplace, nodeId).properties,
			  (std::vector<PropertyCollectionRootEntry>{{BODY_PROPERTY_TOKEN, PropertyValue(firstBody)}}));
		MetadataSnapshotPin afterReplace = metadata_->openSnapshot();
		const EntityPropertyLargeValueReference replacementReference =
				storedLargeValueReference(afterReplace, *metadata_, *slotted_pages_, nodeId);
		EXPECT_EQ(replacementReference.value.value_id, LargeValueId(2U));
		EXPECT_EQ(graph.readNode(afterReplace, nodeId).properties,
				  (std::vector<PropertyCollectionRootEntry>{{BODY_PROPERTY_TOKEN, PropertyValue(replacementBody)}}));
		EXPECT_TRUE(metadata_->findLargeValueManifestOptional(afterReplace, LargeValueId(2U)).has_value());
		EXPECT_EQ(afterReplace.superblock().next_large_value_id, 3U);
		afterReplace.release();
		beforeReplace.release();
	}

	reopenWithLargeValues();
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	NativeGraphWritePlanner writer(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	NativeGraphMutationExecutor executor(
			writer, graph, indexes, *vector_pages_, *vector_manifests_, *metadata_, largeValueTokens(),
			[](const MetadataSnapshotPin &) { return std::vector<IndexDefinition>{}; },
			[](const MetadataSnapshotPin &) { return std::vector<ConstraintDefinition>{}; }, executorOptions());
	MetadataSnapshotPin reopened = metadata_->openSnapshot();
	EXPECT_EQ(storedLargeValueReference(reopened, *metadata_, *slotted_pages_, nodeId).value.value_id,
		  LargeValueId(2U));
	EXPECT_EQ(graph.readNode(reopened, nodeId).properties,
		  (std::vector<PropertyCollectionRootEntry>{{BODY_PROPERTY_TOKEN, PropertyValue(replacementBody)}}));
	reopened.release();

	LogicalGraphMutationBatch erase;
	erase.node_deletes = {{nodeId, false, 1U}};
	ASSERT_TRUE(executor.execute(erase).committed());
	MetadataSnapshotPin afterErase = metadata_->openSnapshot();
	EXPECT_THROW(static_cast<void>(graph.readNode(afterErase, nodeId)), FormatException);
	EXPECT_FALSE(metadata_->findEntityPropertyLocationOptional(
					 afterErase, EntityPropertyKey{EntityPropertyOwnerKind::EPOK_NODE, nodeId})
					 .has_value());
	afterErase.release();
}

TEST_F(NativeGraphLargeValueMutationTest, PublishesCompressedLargeValueReferencesWithTheirOwningProperty) {
	const std::string body(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES + 211U, 'z');
	NativeGraphWritePlannerOptions plannerOptions;
	plannerOptions.property_roots.large_value_compression.codec = LargeValueCodec::LVC_LZ4;
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	NativeGraphWritePlanner writer(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions);
	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	NativeGraphMutationExecutor executor(
			writer, graph, indexes, *vector_pages_, *vector_manifests_, *metadata_, largeValueTokens(),
			[](const MetadataSnapshotPin &) { return std::vector<IndexDefinition>{}; },
			[](const MetadataSnapshotPin &) { return std::vector<ConstraintDefinition>{}; }, executorOptions());

	LogicalGraphMutationBatch create;
	create.node_creates = {{{"Person"}, {{"body", PropertyValue(body)}}}};
	const NativeGraphMutationExecutionResult result = executor.execute(create);
	ASSERT_TRUE(result.committed());
	ASSERT_EQ(result.result.created_node_ids.size(), 1U);

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	const EntityPropertyLargeValueReference reference =
			storedLargeValueReference(snapshot, *metadata_, *slotted_pages_, result.result.created_node_ids.front());
	EXPECT_EQ(reference.value.codec, LargeValueCodec::LVC_LZ4);
	EXPECT_EQ(large_values_->manifest(snapshot, reference.value).codec, LargeValueCodec::LVC_LZ4);
	EXPECT_EQ(graph.readNode(snapshot, result.result.created_node_ids.front()).properties,
			  (std::vector<PropertyCollectionRootEntry>{{BODY_PROPERTY_TOKEN, PropertyValue(body)}}));
}

TEST_F(NativeGraphLargeValueMutationTest, ReusesSharedLargeValueDataPoolAcrossGraphPublicationsAndReopen) {
	const std::string firstBody(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES + 31U, 'a');
	const std::string secondBody(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES + 79U, 'b');
	LargeValueRef firstReference;
	LargeValueRef secondReference;
	uint64_t firstNodeId = 0U;
	uint64_t secondNodeId = 0U;
	uint64_t expectedDataPageCount = 0U;
	{
		BTreePageStore indexes(*slotted_pages_, *metadata_);
		NativeGraphWritePlanner writer(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
		PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
		NativeGraphMutationExecutor executor(
				writer, graph, indexes, *vector_pages_, *vector_manifests_, *metadata_, largeValueTokens(),
				[](const MetadataSnapshotPin &) { return std::vector<IndexDefinition>{}; },
				[](const MetadataSnapshotPin &) { return std::vector<ConstraintDefinition>{}; }, executorOptions());

		LogicalGraphMutationBatch createFirst;
		createFirst.node_creates = {{{"Person"}, {{"body", PropertyValue(firstBody)}}}};
		const NativeGraphMutationExecutionResult first = executor.execute(createFirst);
		ASSERT_TRUE(first.committed());
		ASSERT_EQ(first.result.created_node_ids.size(), 1U);
		firstNodeId = first.result.created_node_ids.front();
		MetadataSnapshotPin beforeSecond = metadata_->openSnapshot();
		firstReference = storedLargeValueReference(beforeSecond, *metadata_, *slotted_pages_, firstNodeId).value;
		EXPECT_EQ(large_values_->read(beforeSecond, firstReference),
				  encodedEntityPropertyRoot(firstNodeId, firstBody));

		LogicalGraphMutationBatch createSecond;
		createSecond.node_creates = {{{"Person"}, {{"body", PropertyValue(secondBody)}}}};
		const NativeGraphMutationExecutionResult second = executor.execute(createSecond);
		ASSERT_TRUE(second.committed());
		ASSERT_EQ(second.result.created_node_ids.size(), 1U);
		secondNodeId = second.result.created_node_ids.front();
		MetadataSnapshotPin current = metadata_->openSnapshot();
		secondReference = storedLargeValueReference(current, *metadata_, *slotted_pages_, secondNodeId).value;
		const LargeValueManifest firstManifest = large_values_->manifest(current, firstReference);
		const LargeValueManifest secondManifest = large_values_->manifest(current, secondReference);
		ASSERT_FALSE(firstManifest.chunks.empty());
		ASSERT_FALSE(secondManifest.chunks.empty());
		const PageLocation firstLocation =
				metadata_->findPageLocation(current, firstManifest.chunks.front().first_data_page_id);
		const PageLocation secondLocation =
				metadata_->findPageLocation(current, secondManifest.chunks.front().first_data_page_id);
		EXPECT_EQ(firstLocation.segment_id, secondLocation.segment_id);
		const CatalogEntry sharedPool = metadata_->findCatalog(current, firstLocation.segment_id);
		EXPECT_EQ(sharedPool.kind, SegmentKind::SGK_LARGE_VALUE_DATA_POOL);
		EXPECT_EQ(sharedPool.owner_id, RSS_LARGE_VALUE_SHARED_DATA_POOL_OWNER_ID);
		const auto dataPageCount = [this, &current](const LargeValueManifest &manifest) {
			uint64_t count = 0U;
			for (const LargeValueManifest &node: large_values_->manifestTree(current, manifest)) {
				for (const LargeValueChunk &chunk: node.chunks) {
					count += chunk.data_page_count;
				}
			}
			return count;
		};
		expectedDataPageCount = dataPageCount(firstManifest) + dataPageCount(secondManifest);
		EXPECT_EQ(large_values_->read(current, firstReference),
				  encodedEntityPropertyRoot(firstNodeId, firstBody));
		EXPECT_EQ(large_values_->read(current, secondReference),
				  encodedEntityPropertyRoot(secondNodeId, secondBody));
		EXPECT_EQ(RelocatableIntegrityScanner(io_, *metadata_).scan().large_value_data_pages, expectedDataPageCount);
		EXPECT_EQ(large_values_->read(beforeSecond, firstReference),
				  encodedEntityPropertyRoot(firstNodeId, firstBody));
		current.release();
		beforeSecond.release();
	}

	reopenWithLargeValues();
	MetadataSnapshotPin reopened = metadata_->openSnapshot();
	EXPECT_EQ(large_values_->read(reopened, firstReference),
			  encodedEntityPropertyRoot(firstNodeId, firstBody));
	EXPECT_EQ(large_values_->read(reopened, secondReference),
			  encodedEntityPropertyRoot(secondNodeId, secondBody));
	EXPECT_EQ(RelocatableIntegrityScanner(io_, *metadata_).scan().large_value_data_pages, expectedDataPageCount);
}

TEST_F(NativeGraphLargeValueMutationTest, ReusesPublishedSharedPoolsForSeveralValuesInOneLaterPublication) {
	const std::array bodies{std::string(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES + 31U, 'e'),
						  std::string(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES + 47U, 'f'),
						  std::string(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES + 63U, 'g')};
	NativeGraphWritePlanner writer(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	const std::array<uint64_t, 0U> noLabels{};

	NativeGraphWriteTransaction first = writer.beginTransaction();
	const uint64_t firstNodeId = first.createNode(noLabels);
	first.replaceNodeProperty(entityPropertyRoot(firstNodeId, bodies[0]));
	ASSERT_TRUE(first.commit().committed());
	MetadataSnapshotPin firstSnapshot = metadata_->openSnapshot();
	const LargeValueRef firstReference =
			storedLargeValueReference(firstSnapshot, *metadata_, *slotted_pages_, firstNodeId).value;
	const std::array firstReferences{firstReference};
	const LargeValuePoolIds firstPools =
			collectLargeValuePoolIds(firstSnapshot, *metadata_, *large_values_, firstReferences);
	ASSERT_EQ(firstPools.data.size(), 1U);
	ASSERT_EQ(firstPools.manifest.size(), 1U);
	firstSnapshot.release();

	NativeGraphWriteTransaction later = writer.beginTransaction();
	const uint64_t secondNodeId = later.createNode(noLabels);
	const uint64_t thirdNodeId = later.createNode(noLabels);
	later.replaceNodeProperty(entityPropertyRoot(secondNodeId, bodies[1]));
	later.replaceNodeProperty(entityPropertyRoot(thirdNodeId, bodies[2]));
	ASSERT_TRUE(later.commit().committed());

	MetadataSnapshotPin current = metadata_->openSnapshot();
	const LargeValueRef secondReference =
			storedLargeValueReference(current, *metadata_, *slotted_pages_, secondNodeId).value;
	const LargeValueRef thirdReference =
			storedLargeValueReference(current, *metadata_, *slotted_pages_, thirdNodeId).value;
	const std::array references{firstReference, secondReference, thirdReference};
	const LargeValuePoolIds pools = collectLargeValuePoolIds(current, *metadata_, *large_values_, references);
	EXPECT_EQ(pools.data, firstPools.data);
	EXPECT_EQ(pools.manifest, firstPools.manifest);
	EXPECT_EQ(large_values_->read(current, firstReference), encodedEntityPropertyRoot(firstNodeId, bodies[0]));
	EXPECT_EQ(large_values_->read(current, secondReference), encodedEntityPropertyRoot(secondNodeId, bodies[1]));
	EXPECT_EQ(large_values_->read(current, thirdReference), encodedEntityPropertyRoot(thirdNodeId, bodies[2]));
	current.release();

	reopenWithLargeValues();
	MetadataSnapshotPin reopened = metadata_->openSnapshot();
	EXPECT_EQ(large_values_->read(reopened, secondReference), encodedEntityPropertyRoot(secondNodeId, bodies[1]));
	EXPECT_EQ(large_values_->read(reopened, thirdReference), encodedEntityPropertyRoot(thirdNodeId, bodies[2]));
}

TEST_F(NativeGraphLargeValueMutationTest, PacksMultipleFreshLargeValuesIntoSharedPoolsInOnePublication) {
	const std::string firstBody(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES + 31U, 'm');
	const std::string secondBody(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES + 79U, 'n');
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	NativeGraphWritePlanner writer(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	NativeGraphMutationExecutor executor(
			writer, graph, indexes, *vector_pages_, *vector_manifests_, *metadata_, largeValueTokens(),
			[](const MetadataSnapshotPin &) { return std::vector<IndexDefinition>{}; },
			[](const MetadataSnapshotPin &) { return std::vector<ConstraintDefinition>{}; }, executorOptions());

	LogicalGraphMutationBatch create;
	create.node_creates = {{{"Person"}, {{"body", PropertyValue(firstBody)}}},
						   {{"Person"}, {{"body", PropertyValue(secondBody)}}}};
	const NativeGraphMutationExecutionResult result = executor.execute(create);
	ASSERT_TRUE(result.committed());
	ASSERT_EQ(result.result.created_node_ids.size(), 2U);

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	const LargeValueRef firstReference = storedLargeValueReference(
			snapshot, *metadata_, *slotted_pages_, result.result.created_node_ids[0]).value;
	const LargeValueRef secondReference = storedLargeValueReference(
			snapshot, *metadata_, *slotted_pages_, result.result.created_node_ids[1]).value;
	EXPECT_NE(firstReference.value_id, secondReference.value_id);
	const std::array references{firstReference, secondReference};
	const LargeValuePoolIds pools =
			collectLargeValuePoolIds(snapshot, *metadata_, *large_values_, references);
	ASSERT_EQ(pools.data.size(), 1U);
	ASSERT_EQ(pools.manifest.size(), 1U);
	const CatalogEntry dataPool = metadata_->findCatalog(snapshot, SegmentId(*pools.data.begin()));
	const CatalogEntry manifestPool = metadata_->findCatalog(snapshot, SegmentId(*pools.manifest.begin()));
	EXPECT_EQ(dataPool.owner_id, RSS_LARGE_VALUE_SHARED_DATA_POOL_OWNER_ID);
	EXPECT_EQ(manifestPool.owner_id, RSS_LARGE_VALUE_SHARED_MANIFEST_POOL_OWNER_ID);
	size_t dataPoolEntries = 0U;
	size_t manifestPoolEntries = 0U;
	metadata_->visitCatalogEntries(snapshot, [&dataPoolEntries, &manifestPoolEntries](const CatalogEntry &entry) {
		dataPoolEntries += entry.kind == SegmentKind::SGK_LARGE_VALUE_DATA_POOL ? 1U : 0U;
		manifestPoolEntries += entry.kind == SegmentKind::SGK_LARGE_VALUE_MANIFEST_POOL ? 1U : 0U;
	});
	EXPECT_EQ(dataPoolEntries, 1U);
	EXPECT_EQ(manifestPoolEntries, 1U);
	EXPECT_EQ(large_values_->read(snapshot, firstReference),
			  encodedEntityPropertyRoot(result.result.created_node_ids[0], firstBody));
	EXPECT_EQ(large_values_->read(snapshot, secondReference),
			  encodedEntityPropertyRoot(result.result.created_node_ids[1], secondBody));
	snapshot.release();

	reopenWithLargeValues();
	MetadataSnapshotPin reopened = metadata_->openSnapshot();
	EXPECT_EQ(large_values_->read(reopened, firstReference),
			  encodedEntityPropertyRoot(result.result.created_node_ids[0], firstBody));
	EXPECT_EQ(large_values_->read(reopened, secondReference),
			  encodedEntityPropertyRoot(result.result.created_node_ids[1], secondBody));
}

TEST_F(NativeGraphLargeValueMutationTest, RecoversOldGenerationWhenSharedFreshPoolPublicationStopsBeforeSuperblock) {
	const std::string firstBody(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES + 31U, 'p');
	const std::string secondBody(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES + 79U, 'q');
	NativeGraphWritePlanner writer(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	MetadataSnapshotPin originalSnapshot = metadata_->openSnapshot();
	const Superblock original = originalSnapshot.superblock();
	originalSnapshot.release();

	NativeGraphWriteTransaction transaction = writer.beginTransaction();
	const std::array<uint64_t, 0U> noLabels{};
	const uint64_t firstNodeId = transaction.createNode(noLabels);
	const uint64_t secondNodeId = transaction.createNode(noLabels);
	MetadataSnapshotPin leaseSnapshot = metadata_->openSnapshot();
	const Superblock leasePublication = leaseSnapshot.superblock();
	leaseSnapshot.release();
	transaction.replaceNodeProperty(entityPropertyRoot(firstNodeId, firstBody));
	transaction.replaceNodeProperty(entityPropertyRoot(secondNodeId, secondBody));
	EXPECT_THROW(static_cast<void>(transaction.commit(
						 withBootstrapFailpoint(BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE))),
				 BootstrapPublishInterrupted);
	EXPECT_EQ(transaction.state(), NativeGraphWriteTransactionState::NGWTS_INDETERMINATE);

	reopenWithLargeValues();
	MetadataSnapshotPin recovered = metadata_->openSnapshot();
	// Logical-ID leasing is independently durable. A failed graph publication
	// must retain that lease but must not expose any graph or LargeValue root.
	EXPECT_EQ(recovered.superblock(), leasePublication);
	EXPECT_FALSE(metadata_->findNodePageLocationOptional(recovered, firstNodeId).has_value());
	EXPECT_FALSE(metadata_->findNodePageLocationOptional(recovered, secondNodeId).has_value());
	EXPECT_FALSE(metadata_->findLargeValueManifestOptional(recovered, LargeValueId(original.next_large_value_id))
					 .has_value());
	recovered.release();
	const RelocatableIntegrityReport report = RelocatableIntegrityScanner(io_, *metadata_).scan();
	EXPECT_EQ(report.large_value_manifest_routes, 0U);
	EXPECT_EQ(report.large_value_manifest_nodes, 0U);
	EXPECT_EQ(report.large_value_data_pages, 0U);
	EXPECT_EQ(report.large_value_content_index_entries, 0U);
}

TEST_F(NativeGraphLargeValueMutationTest, RecoversSharedFreshPoolsWhenPublicationStopsAfterSuperblock) {
	const std::string firstBody(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES + 31U, 'r');
	const std::string secondBody(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES + 79U, 's');
	NativeGraphWritePlanner writer(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	MetadataSnapshotPin originalSnapshot = metadata_->openSnapshot();
	const Superblock original = originalSnapshot.superblock();
	originalSnapshot.release();

	NativeGraphWriteTransaction transaction = writer.beginTransaction();
	const std::array<uint64_t, 0U> noLabels{};
	const uint64_t firstNodeId = transaction.createNode(noLabels);
	const uint64_t secondNodeId = transaction.createNode(noLabels);
	MetadataSnapshotPin leaseSnapshot = metadata_->openSnapshot();
	const Superblock leasePublication = leaseSnapshot.superblock();
	leaseSnapshot.release();
	transaction.replaceNodeProperty(entityPropertyRoot(firstNodeId, firstBody));
	transaction.replaceNodeProperty(entityPropertyRoot(secondNodeId, secondBody));
	EXPECT_THROW(static_cast<void>(transaction.commit(
						 withBootstrapFailpoint(BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE))),
				 BootstrapPublishInterrupted);
	EXPECT_EQ(transaction.state(), NativeGraphWriteTransactionState::NGWTS_INDETERMINATE);

	reopenWithLargeValues();
	MetadataSnapshotPin recovered = metadata_->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation, Generation(leasePublication.generation.value() + 1U));
	EXPECT_EQ(recovered.superblock().next_large_value_id, original.next_large_value_id + 2U);
	const LargeValueRef firstReference =
			storedLargeValueReference(recovered, *metadata_, *slotted_pages_, firstNodeId).value;
	const LargeValueRef secondReference =
			storedLargeValueReference(recovered, *metadata_, *slotted_pages_, secondNodeId).value;
	EXPECT_EQ(large_values_->read(recovered, firstReference), encodedEntityPropertyRoot(firstNodeId, firstBody));
	EXPECT_EQ(large_values_->read(recovered, secondReference), encodedEntityPropertyRoot(secondNodeId, secondBody));
	const std::array references{firstReference, secondReference};
	const LargeValuePoolIds pools =
			collectLargeValuePoolIds(recovered, *metadata_, *large_values_, references);
	EXPECT_EQ(pools.data.size(), 1U);
	EXPECT_EQ(pools.manifest.size(), 1U);
	recovered.release();
	const RelocatableIntegrityReport report = RelocatableIntegrityScanner(io_, *metadata_).scan();
	EXPECT_EQ(report.large_value_manifest_routes, 2U);
	EXPECT_EQ(report.large_value_content_index_entries, 2U);
}

TEST_F(NativeGraphLargeValueMutationTest,
	   RecoversNoPartialEntityLargeValueOrScalarIndexStateBeforeSuperblockPublication) {
	const std::string body(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES + 59U, 'u');
	const std::array<uint64_t, 0U> noLabels{};
	const std::array<std::byte, 1U> key = {std::byte{'u'}};
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	NativeGraphWritePlanner writer(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	NativeGraphWriteTransaction transaction = writer.beginTransaction();
	const uint64_t nodeId = transaction.createNode(noLabels);
	MetadataSnapshotPin leased = metadata_->openSnapshot();
	const Superblock leasePublication = leased.superblock();
	const LargeValueId firstLargeValueId(leasePublication.next_large_value_id);
	leased.release();
	transaction.replaceNodeProperty(entityPropertyRoot(nodeId, body));
	const std::array<BTreePageEntry, 1U> entries{{{std::vector<std::byte>(key.begin(), key.end()), nodeId}}};
	transaction.stageScalarIndexCreate(indexes, 43U, entries);

	EXPECT_THROW(static_cast<void>(transaction.commit(
				 withBootstrapFailpoint(BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE))),
			 BootstrapPublishInterrupted);
	EXPECT_EQ(transaction.state(), NativeGraphWriteTransactionState::NGWTS_INDETERMINATE);

	reopenWithLargeValues();
	MetadataSnapshotPin recovered = metadata_->openSnapshot();
	EXPECT_EQ(recovered.superblock(), leasePublication);
	EXPECT_FALSE(metadata_->findNodePageLocationOptional(recovered, nodeId).has_value());
	EXPECT_FALSE(metadata_->findLargeValueManifestOptional(recovered, firstLargeValueId).has_value());
	EXPECT_FALSE(metadata_->findIndexRootOptional(recovered, 43U).has_value());
	recovered.release();
}

TEST_F(NativeGraphLargeValueMutationTest,
	   RecoversOneCompleteEntityLargeValueAndScalarIndexStateAfterSuperblockPublication) {
	const std::string body(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES + 61U, 'v');
	const std::array<uint64_t, 0U> noLabels{};
	const std::array<std::byte, 1U> key = {std::byte{'v'}};
	uint64_t nodeId = 0U;
	{
		BTreePageStore indexes(*slotted_pages_, *metadata_);
		NativeGraphWritePlanner writer(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
		NativeGraphWriteTransaction transaction = writer.beginTransaction();
		nodeId = transaction.createNode(noLabels);
		MetadataSnapshotPin leased = metadata_->openSnapshot();
		const Superblock leasePublication = leased.superblock();
		const LargeValueId firstLargeValueId(leasePublication.next_large_value_id);
		leased.release();
		transaction.replaceNodeProperty(entityPropertyRoot(nodeId, body));
		const std::array<BTreePageEntry, 1U> entries{{{std::vector<std::byte>(key.begin(), key.end()), nodeId}}};
		transaction.stageScalarIndexCreate(indexes, 43U, entries);

		EXPECT_THROW(static_cast<void>(transaction.commit(
					 withBootstrapFailpoint(BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE))),
				 BootstrapPublishInterrupted);
		EXPECT_EQ(transaction.state(), NativeGraphWriteTransactionState::NGWTS_INDETERMINATE);
		MetadataSnapshotPin current = metadata_->openSnapshot();
		EXPECT_EQ(current.superblock().generation, Generation(leasePublication.generation.value() + 1U));
		EXPECT_TRUE(metadata_->findLargeValueManifestOptional(current, firstLargeValueId).has_value());
		EXPECT_TRUE(metadata_->findIndexRootOptional(current, 43U).has_value());
		current.release();
	}

	reopenWithLargeValues();
	BTreePageStore reopenedIndexes(*slotted_pages_, *metadata_);
	BTreeIndex reopenedIndex(43U, reopenedIndexes, *metadata_);
	MetadataSnapshotPin recovered = metadata_->openSnapshot();
	const EntityPropertyLargeValueReference reference =
			storedLargeValueReference(recovered, *metadata_, *slotted_pages_, nodeId);
	EXPECT_EQ(large_values_->read(recovered, reference.value), encodedEntityPropertyRoot(nodeId, body));
	EXPECT_EQ(reopenedIndex.find(recovered, key), std::optional<uint64_t>(nodeId));
	EXPECT_TRUE(metadata_->findIndexRoot(recovered, 43U).root_page_id.isValid());
	recovered.release();
}

TEST_F(NativeGraphLargeValueMutationTest, ReusesSnapshotVerifiedContentWithoutAllocatingAnotherLargeValue) {
	const std::string body(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES + 67U, 'd');
	const std::array<uint64_t, 0U> noLabels{};
	NativeGraphWritePlanner writer(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	NativeGraphWriteTransaction create = writer.beginTransaction();
	const uint64_t nodeId = create.createNode(noLabels);
	ASSERT_TRUE(create.commit().committed());

	PropertyCollectionRoot root;
	root.owner_entity_id = nodeId;
	root.owner_entity_type = graph::Node::typeId;
	root.entries = {{BODY_PROPERTY_TOKEN, PropertyValue(body)}};
	const std::vector<std::byte> canonicalBytes = EntityPropertyCollectionRootCodec::encode(root);
	LargeValueWriteOptions options;
	options.first_data_slot = PhysicalSlotId(1U);
	options.last_data_slot = PhysicalSlotId(64U);
	options.first_manifest_slot = PhysicalSlotId(1U);
	options.last_manifest_slot = PhysicalSlotId(64U);
	options.kind = LargeValueKind::LVK_BYTES;
	options.media_type = RSS_ENTITY_PROPERTY_LARGE_VALUE_MEDIA_TYPE;
	MetadataSnapshotPin beforeCanonicalWrite = metadata_->openSnapshot();
	const LogSequenceNumber canonicalWriteLsn(beforeCanonicalWrite.superblock().checkpoint_lsn.value() + 1U);
	beforeCanonicalWrite.release();
	const LargeValueWriteResult canonical = large_values_->write(canonicalBytes, options, canonicalWriteLsn);

	NativeGraphWriteTransaction replace = writer.beginTransaction();
	replace.replaceNodeProperty(root);
	ASSERT_TRUE(replace.commit().committed());

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_EQ(storedLargeValueReference(snapshot, *metadata_, *slotted_pages_, nodeId).value, canonical.reference);
	EXPECT_EQ(snapshot.superblock().next_large_value_id, 2U);
	EXPECT_EQ(large_values_->read(snapshot, canonical.reference), canonicalBytes);
	snapshot.release();
}

TEST_F(NativeGraphLargeValueMutationTest, RejectsExternalMutationOfReservedContentIndex) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	NativeGraphWritePlanner writer(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	NativeGraphWriteTransaction transaction = writer.beginTransaction();
	const std::array<std::byte, 1U> key{std::byte{'x'}};
	const std::array<BTreePageEntry, 1U> entries{{{std::vector<std::byte>(key.begin(), key.end()), 1U}}};

	EXPECT_THROW(transaction.stageScalarIndexUpsert(indexes, RSS_LARGE_VALUE_CONTENT_INDEX_ID, key, 1U),
			 FormatException);
	EXPECT_THROW(transaction.stageScalarIndexErase(indexes, RSS_LARGE_VALUE_CONTENT_INDEX_ID, key), FormatException);
	EXPECT_THROW(transaction.stageScalarIndexCreate(indexes, RSS_LARGE_VALUE_CONTENT_INDEX_ID, entries),
			 FormatException);
	EXPECT_THROW(transaction.stageScalarIndexDrop(indexes, RSS_LARGE_VALUE_CONTENT_INDEX_ID, 1U), FormatException);
}
