/**
 * @file test_NativeGraphWritePlanner_LabelIndex.cpp
 * @brief End-to-end label-index tests for the native graph writer.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "graph/core/PropertyTypes.hpp"
#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/entity/NativeGraphMutationExecutor.hpp"
#include "graph/storage/relocatable/entity/NativeGraphWritePlanner.hpp"
#include "graph/storage/relocatable/entity/PageBackedGraphReadAdapter.hpp"
#include "graph/storage/relocatable/index/BTreeIndex.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"
#include "graph/storage/relocatable/index/IndexKeyCodec.hpp"
#include "graph/storage/relocatable/schema/IndexDefinition.hpp"
#include "storage/relocatable/entity/NativeGraphWritePlannerTestFixture.hpp"

using namespace graph::storage;
using graph::PropertyValue;

namespace {

	constexpr uint64_t LABEL_INDEX_ID = 77U;
	constexpr uint64_t PERSON_LABEL_TOKEN = 7U;
	constexpr uint64_t EMPLOYEE_LABEL_TOKEN = 9U;

	IndexRootLocation allocateLabelIndexRoot(BTreePageStore &indexes) {
		SlottedPagePoolAllocationRequest allocation;
		allocation.segment_id = SegmentId(71U);
		allocation.pool_kind = SegmentKind::SGK_INDEX_PAGE_POOL;
		allocation.pool_owner_id = LABEL_INDEX_ID;
		allocation.generation = Generation(1U);
		allocation.page_id = PageId(101U);
		allocation.page_owner_id = LABEL_INDEX_ID;
		allocation.page_owner_kind = PageOwnerKind::POK_INDEX;
		PageOwnerHeader owner;
		owner.page_id = allocation.page_id;
		owner.owner_id = allocation.page_owner_id;
		owner.kind = allocation.page_owner_kind;
		owner.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
		owner.generation = allocation.generation;
		owner.committed_lsn = LogSequenceNumber(2U);
		BTreePageHeader header;
		header.index_id = allocation.page_owner_id;
		header.page_id = allocation.page_id;
		header.generation = allocation.generation;
		if (!indexes.allocate(allocation, BTreePage::create(owner, header), PhysicalSlotId(40U), PhysicalSlotId(40U),
							 LogSequenceNumber(2U))
				 .has_value()) {
			throw std::runtime_error("native label-index test could not allocate its initial B+Tree root");
		}
		const IndexRootLocation root{LABEL_INDEX_ID, allocation.page_id, allocation.generation};
		static_cast<void>(indexes.publishRoots(std::span(&root, 1U), {}, LogSequenceNumber(3U)));
		return root;
	}

	NativeGraphMutationTokenResolvers labelTokens() {
		NativeGraphMutationTokenResolvers tokens;
		tokens.schema_token = [](const MetadataSnapshotPin &, SystemCatalogSchemaTokenKind kind,
									 std::string_view name) -> std::optional<uint64_t> {
			if (kind != SystemCatalogSchemaTokenKind::SCSTK_LABEL) {
				return std::nullopt;
			}
			if (name == "Person") {
				return PERSON_LABEL_TOKEN;
			}
			if (name == "Employee") {
				return EMPLOYEE_LABEL_TOKEN;
			}
			return std::nullopt;
		};
		tokens.property_key_token =
				[](const MetadataSnapshotPin &, std::string_view) -> std::optional<uint32_t> { return std::nullopt; };
		tokens.property_key_name =
				[](const MetadataSnapshotPin &, uint32_t) -> std::optional<std::string> { return std::nullopt; };
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

	IndexDefinition labelIndexDefinition() {
		IndexDefinition definition;
		definition.index_id = LABEL_INDEX_ID;
		definition.name = "native-node-labels";
		definition.entity_kind = IndexDefinitionEntityKind::IDEK_NODE;
		definition.kind = IndexDefinitionKind::IDK_LABEL;
		return definition;
	}

	std::vector<std::byte> labelKey(uint64_t labelToken, uint64_t nodeId) {
		return IndexKeyCodec::encodeEntryKey(PropertyValue(static_cast<int64_t>(labelToken)), nodeId);
	}

} // namespace

class NativeGraphLabelIndexMutationTest : public graph::storage::test::NativeGraphWritePlannerTestFixture {};

TEST_F(NativeGraphLabelIndexMutationTest, PublishesLogicalLabelIndexDeltasWithEntityLifecycleAndSnapshots) {
	const IndexDefinition definition = labelIndexDefinition();
	uint64_t nodeId = 0U;
	{
		BTreePageStore indexes(*slotted_pages_, *metadata_);
		static_cast<void>(allocateLabelIndexRoot(indexes));
		NativeGraphWritePlanner writer(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
		PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
		NativeGraphMutationExecutor executor(
				writer, graph, indexes, *vector_pages_, *vector_manifests_, *metadata_, labelTokens(),
				[definition](const MetadataSnapshotPin &) { return std::vector<IndexDefinition>{definition}; },
				[](const MetadataSnapshotPin &) { return std::vector<ConstraintDefinition>{}; }, executorOptions());
		BTreeIndex labelIndex(LABEL_INDEX_ID, indexes, *metadata_);

		MetadataSnapshotPin beforeCreate = metadata_->openSnapshot();
		LogicalGraphMutationBatch create;
		create.node_creates = {{{"Person"}, {}}};
		const NativeGraphMutationExecutionResult created = executor.execute(create);
		ASSERT_TRUE(created.committed());
		ASSERT_EQ(created.result.created_node_ids.size(), 1U);
		nodeId = created.result.created_node_ids.front();
		const std::vector<std::byte> personKey = labelKey(PERSON_LABEL_TOKEN, nodeId);
		EXPECT_THROW(static_cast<void>(graph.readNode(beforeCreate, nodeId)), FormatException);
		EXPECT_FALSE(labelIndex.find(beforeCreate, personKey).has_value());
		beforeCreate.release();

		MetadataSnapshotPin beforeReplace = metadata_->openSnapshot();
		ASSERT_EQ(graph.readNode(beforeReplace, nodeId).label_token_ids,
				  (std::vector<uint64_t>{PERSON_LABEL_TOKEN}));
		ASSERT_EQ(labelIndex.find(beforeReplace, personKey), std::optional<uint64_t>(nodeId));
		LogicalGraphMutationBatch replace;
		replace.node_label_replacements = {
				{{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, nodeId},
				 {"Employee"}}};
		ASSERT_TRUE(executor.execute(replace).committed());

		const std::vector<std::byte> employeeKey = labelKey(EMPLOYEE_LABEL_TOKEN, nodeId);
		// A pinned reader must retain the former Entity image and derived Index root.
		EXPECT_EQ(graph.readNode(beforeReplace, nodeId).label_token_ids,
			  (std::vector<uint64_t>{PERSON_LABEL_TOKEN}));
		EXPECT_EQ(labelIndex.find(beforeReplace, personKey), std::optional<uint64_t>(nodeId));
		MetadataSnapshotPin afterReplace = metadata_->openSnapshot();
		EXPECT_EQ(graph.readNode(afterReplace, nodeId).label_token_ids,
			  (std::vector<uint64_t>{EMPLOYEE_LABEL_TOKEN}));
		EXPECT_FALSE(labelIndex.find(afterReplace, personKey).has_value());
		EXPECT_EQ(labelIndex.find(afterReplace, employeeKey), std::optional<uint64_t>(nodeId));
		afterReplace.release();
		beforeReplace.release();
	}

	reopenNativeStores();
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	NativeGraphWritePlanner writer(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	NativeGraphMutationExecutor executor(
			writer, graph, indexes, *vector_pages_, *vector_manifests_, *metadata_, labelTokens(),
			[definition](const MetadataSnapshotPin &) { return std::vector<IndexDefinition>{definition}; },
			[](const MetadataSnapshotPin &) { return std::vector<ConstraintDefinition>{}; }, executorOptions());
	BTreeIndex labelIndex(LABEL_INDEX_ID, indexes, *metadata_);
	const std::vector<std::byte> employeeKey = labelKey(EMPLOYEE_LABEL_TOKEN, nodeId);
	MetadataSnapshotPin reopened = metadata_->openSnapshot();
	EXPECT_EQ(graph.readNode(reopened, nodeId).label_token_ids, (std::vector<uint64_t>{EMPLOYEE_LABEL_TOKEN}));
	EXPECT_EQ(labelIndex.find(reopened, employeeKey), std::optional<uint64_t>(nodeId));
	reopened.release();

	LogicalGraphMutationBatch erase;
	erase.node_deletes = {{nodeId, false, 1U}};
	ASSERT_TRUE(executor.execute(erase).committed());
	MetadataSnapshotPin afterErase = metadata_->openSnapshot();
	EXPECT_THROW(static_cast<void>(graph.readNode(afterErase, nodeId)), FormatException);
	EXPECT_FALSE(labelIndex.find(afterErase, employeeKey).has_value());
	afterErase.release();
}
