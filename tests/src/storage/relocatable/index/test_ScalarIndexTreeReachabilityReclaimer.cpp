/**
 * @file test_ScalarIndexTreeReachabilityReclaimer.cpp
 * @brief Reachability tests for obsolete public scalar-index B+Trees.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <filesystem>
#include <fstream>
#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/index/BTreeIndex.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"
#include "graph/storage/relocatable/index/BulkBTreeBuilder.hpp"
#include "graph/storage/relocatable/entity/EntityLabelSetStore.hpp"
#include "graph/storage/relocatable/entity/EntityPageStore.hpp"
#include "graph/storage/relocatable/entity/NativeGraphWritePlanner.hpp"
#include "graph/storage/relocatable/index/ScalarIndexTreeReachabilityReclaimer.hpp"
#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRootStore.hpp"
#include "graph/storage/relocatable/schema/SystemCatalog.hpp"
#include "graph/storage/relocatable/vector/VectorManifestStore.hpp"
#include "graph/storage/relocatable/vector/VectorNodeMapping.hpp"

using namespace graph::storage;

namespace {

	Superblock initialSuperblock() {
		Superblock result;
		result.database_id = DatabaseId(0x7E7F808182838485ULL);
		result.generation = Generation(1U);
		result.checkpoint_lsn = LogSequenceNumber(1U);
		return result;
	}

} // namespace

class RelocatableScalarIndexTreeReachabilityReclaimerTest : public ::testing::Test {
protected:
	std::filesystem::path data_path_;
	std::filesystem::path wal_path_;
	std::shared_ptr<std::fstream> data_file_;
	std::shared_ptr<std::fstream> wal_file_;
	std::shared_ptr<StorageIO> data_io_;
	std::shared_ptr<StorageIO> wal_io_;
	std::unique_ptr<RelocatableMetadataStore> metadata_;
	std::unique_ptr<SlottedPagePool> pages_;
	std::unique_ptr<BTreePageStore> trees_;
	std::unique_ptr<EntityPageStore> page_entities_;
	std::unique_ptr<EntityLabelSetStore> labels_;
	std::unique_ptr<PropertyCollectionRootStore> properties_;
	std::unique_ptr<NativeGraphWritePlanner> native_writes_;
	std::unique_ptr<SystemCatalog> catalog_;
	std::unique_ptr<VectorManifestStore> vector_manifests_;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		data_path_ = std::filesystem::temp_directory_path() / ("relocatable_scalar_index_reclaim_" + to_string(uuid));
		wal_path_ = data_path_.string() + ".foreign";
		data_file_ = std::make_shared<std::fstream>(data_path_,
				std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		wal_file_ = std::make_shared<std::fstream>(wal_path_,
				std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(data_file_->is_open());
		ASSERT_TRUE(wal_file_->is_open());
		data_io_ = std::make_shared<StorageIO>(data_file_, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		wal_io_ = std::make_shared<StorageIO>(wal_file_, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		metadata_ = RelocatableMetadataStore::initializeNew(data_io_, initialSuperblock());
		rebuildStores();
	}

	void rebuildStores() {
		pages_ = std::make_unique<SlottedPagePool>(data_io_, *metadata_);
		trees_ = std::make_unique<BTreePageStore>(*pages_, *metadata_);
		page_entities_ = std::make_unique<EntityPageStore>(data_io_, *metadata_);
		labels_ = std::make_unique<EntityLabelSetStore>(data_io_, *metadata_, *pages_);
		properties_ = std::make_unique<PropertyCollectionRootStore>(*pages_, *metadata_);
		NativeGraphWritePlannerOptions writeOptions;
		writeOptions.first_slot = PhysicalSlotId(40U);
		writeOptions.last_slot = PhysicalSlotId(95U);
		writeOptions.entity_pool_owner_id = 9401U;
		writeOptions.property_roots = {PhysicalSlotId(40U), PhysicalSlotId(95U), 9402U};
		native_writes_ = std::make_unique<NativeGraphWritePlanner>(*metadata_, *page_entities_, *labels_, *properties_,
																																										*trees_, writeOptions);
		catalog_ = std::make_unique<SystemCatalog>(*trees_, *metadata_);
		vector_manifests_ = std::make_unique<VectorManifestStore>(data_io_, *metadata_);
	}

	void reopen() {
		vector_manifests_.reset();
		catalog_.reset();
		native_writes_.reset();
		properties_.reset();
		labels_.reset();
		page_entities_.reset();
		trees_.reset();
		pages_.reset();
		metadata_.reset();
		metadata_ = RelocatableMetadataStore::open(data_io_);
		rebuildStores();
	}

	void TearDown() override {
		vector_manifests_.reset();
		catalog_.reset();
		native_writes_.reset();
		properties_.reset();
		labels_.reset();
		page_entities_.reset();
		trees_.reset();
		pages_.reset();
		metadata_.reset();
		data_io_.reset();
		wal_io_.reset();
		if (data_file_ && data_file_->is_open()) {
			data_file_->close();
		}
		if (wal_file_ && wal_file_->is_open()) {
			wal_file_->close();
		}
		std::error_code error;
		std::filesystem::remove(data_path_, error);
		std::filesystem::remove(wal_path_, error);
	}

	[[nodiscard]] LogSequenceNumber nextCheckpointLsn() const {
		const MetadataSnapshotPin snapshot = metadata_->openSnapshot();
		return LogSequenceNumber(snapshot.superblock().checkpoint_lsn.value() + 1U);
	}

	[[nodiscard]] IndexDefinition sealedDefinition(uint64_t labelTokenId, std::string name,
															 IndexDefinitionKind kind = IndexDefinitionKind::IDK_PROPERTY) const {
		IndexDefinition definition{0U, std::move(name), IndexDefinitionEntityKind::IDEK_NODE, kind, labelTokenId,
									 {"score"}, 0U, {}};
		if (kind == IndexDefinitionKind::IDK_LABEL) {
			definition.label_token_id = 0U;
			definition.property_names.clear();
		} else if (kind == IndexDefinitionKind::IDK_COMPOSITE) {
			definition.property_names = {"score", "name"};
		}
		return definition;
	}

	[[nodiscard]] IndexDefinition vectorDefinition(uint64_t labelTokenId, std::string name) const {
		return {0U, std::move(name), IndexDefinitionEntityKind::IDEK_NODE, IndexDefinitionKind::IDK_VECTOR, labelTokenId,
				{"embedding"}, 2U, "l2"};
	}

	void buildRoot(uint64_t indexId, uint64_t value, size_t entryCount = 1U) {
		ASSERT_NE(entryCount, 0U);
		auto entries = std::make_shared<std::vector<BTreePageEntry>>();
		entries->reserve(entryCount);
		if (entryCount == 1U) {
			entries->push_back({{std::byte{0x61U}}, value});
		}
		for (size_t ordinal = entryCount == 1U ? 1U : 0U; ordinal < entryCount; ++ordinal) {
			std::vector<std::byte> key(sizeof(uint64_t));
			for (size_t byte = 0U; byte < key.size(); ++byte) {
				const uint32_t shift = static_cast<uint32_t>((key.size() - byte - 1U) * 8U);
				key[byte] = static_cast<std::byte>((static_cast<uint64_t>(ordinal) >> shift) & UINT64_C(0xFF));
			}
			entries->push_back({std::move(key), value + ordinal});
		}
		const OrderedBTreeEntrySource source = [entries] {
			auto position = std::make_shared<size_t>(0U);
			return [entries, position]() -> std::optional<BTreePageEntry> {
				if (*position == entries->size()) {
					return std::nullopt;
				}
				return (*entries)[(*position)++];
			};
		};
		const BulkBTreeBuildOptions options = {PhysicalSlotId(40U), PhysicalSlotId(95U), indexId, entryCount,
												  entryCount + 1U};
		static_cast<void>(BulkBTreeBuilder::build(*trees_, *metadata_, indexId, source, options, nextCheckpointLsn()));
	}

	void buildManualRoot(uint64_t indexId, uint64_t poolOwnerId, bool duplicateInternalChild = false) {
		const std::vector<PageId> pageIds =
				metadata_->allocateLogicalPageIds(duplicateInternalChild ? 2U : 1U, nextCheckpointLsn());
		MetadataSnapshotPin snapshot = metadata_->openSnapshot();
		const SegmentId segmentId(snapshot.superblock().next_logical_segment_id);
		snapshot.release();
		const LogSequenceNumber rootLsn = nextCheckpointLsn();
		const PageId rootPageId = pageIds.front();
		PageOwnerHeader rootOwner{rootPageId,
								 indexId,
								 PageOwnerKind::POK_INDEX,
								 0U,
								 RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
								 Generation(1U),
								 rootLsn};
		BTreePageHeader rootHeader;
		rootHeader.index_id = indexId;
		rootHeader.page_id = rootPageId;
		rootHeader.generation = Generation(1U);
		if (duplicateInternalChild) {
			rootHeader.kind = BTreeNodeKind::BPN_INTERNAL;
			rootHeader.level = 1U;
			rootHeader.leftmost_child = pageIds[1U];
		}
		BTreePage root = BTreePage::create(rootOwner, rootHeader);
		if (duplicateInternalChild) {
			root.insert({{std::byte{0x80U}}, pageIds[1U].value()});
		} else {
			root.insert({{std::byte{0x61U}}, 1U});
		}
		SlottedPagePoolAllocationRequest request;
		request.segment_id = segmentId;
		request.pool_kind = SegmentKind::SGK_INDEX_PAGE_POOL;
		request.pool_owner_id = poolOwnerId;
		request.generation = Generation(1U);
		request.page_id = rootPageId;
		request.page_owner_id = indexId;
		request.page_owner_kind = PageOwnerKind::POK_INDEX;
		ASSERT_TRUE(trees_->allocate(request, root, PhysicalSlotId(40U), PhysicalSlotId(95U), rootLsn).has_value());
		if (duplicateInternalChild) {
			const LogSequenceNumber childLsn = nextCheckpointLsn();
			const PageId childPageId = pageIds[1U];
			const PageOwnerHeader childOwner{childPageId,
										  indexId,
										  PageOwnerKind::POK_INDEX,
										  0U,
										  RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
										  Generation(1U),
										  childLsn};
			BTreePageHeader childHeader;
			childHeader.index_id = indexId;
			childHeader.page_id = childPageId;
			childHeader.generation = Generation(1U);
			BTreePage child = BTreePage::create(childOwner, childHeader);
			child.insert({{std::byte{0x61U}}, 1U});
			SlottedPagePoolAppendRequest append;
			append.segment_id = segmentId;
			append.generation = Generation(1U);
			append.page_id = childPageId;
			append.page_owner_id = indexId;
			append.page_owner_kind = PageOwnerKind::POK_INDEX;
			static_cast<void>(trees_->append(append, child, childLsn));
		}
		const IndexRootLocation rootLocation{indexId, rootPageId, Generation(1U)};
		static_cast<void>(trees_->publishRoots(std::span(&rootLocation, 1U), {}, nextCheckpointLsn()));
	}

	IndexDefinition publishIndex(IndexDefinition definition) {
		NativeGraphWriteTransaction transaction = native_writes_->beginTransaction();
		SystemCatalogWriteSession session = catalog_->beginWrite(transaction);
		definition = session.createIndexDefinition(std::move(definition));
		session.finish();
		EXPECT_TRUE(transaction.commit().committed());
		return definition;
	}

	void publishSealed(IndexDefinition &definition) { definition = publishIndex(std::move(definition)); }

	IndexDefinition publishDynamic(std::string name) {
		return publishIndex(sealedDefinition(0U, std::move(name), IndexDefinitionKind::IDK_COMPOSITE));
	}

	ConstraintDefinition publishConstraint(ConstraintDefinitionKind kind, std::string name) {
		ConstraintDefinition definition;
		definition.name = std::move(name);
		definition.entity_kind = ConstraintDefinitionEntityKind::CDEK_NODE;
		definition.kind = kind;
		definition.schema_token_id = 7U;
		definition.property_names = {"email"};
		NativeGraphWriteTransaction transaction = native_writes_->beginTransaction();
		SystemCatalogWriteSession session = catalog_->beginWrite(transaction);
		definition = session.createConstraintDefinition(std::move(definition));
		session.finish();
		EXPECT_TRUE(transaction.commit().committed());
		return definition;
	}

	ConstraintDefinition publishUniqueConstraint() {
		return publishConstraint(ConstraintDefinitionKind::CDK_UNIQUE, "person_email_unique");
	}

	void tombstoneSealed(uint64_t indexId) {
		MetadataSnapshotPin snapshot = metadata_->openSnapshot();
		const std::optional<IndexDefinition> definition = catalog_->findIndexDefinitionById(snapshot, indexId);
		snapshot.release();
		ASSERT_TRUE(definition.has_value());
		NativeGraphWriteTransaction transaction = native_writes_->beginTransaction();
		SystemCatalogWriteSession session = catalog_->beginWrite(transaction);
		session.dropIndexDefinition(*definition);
		session.finish();
		ASSERT_TRUE(transaction.commit().committed());
	}

	void dropConstraint(const ConstraintDefinition &definition) {
		NativeGraphWriteTransaction transaction = native_writes_->beginTransaction();
		SystemCatalogWriteSession session = catalog_->beginWrite(transaction);
		session.dropConstraintDefinition(definition);
		session.finish();
		ASSERT_TRUE(transaction.commit().committed());
	}

	[[nodiscard]] ScalarIndexTreeReachabilityReclaimer reclaimer() {
		return ScalarIndexTreeReachabilityReclaimer(*trees_, *catalog_, *vector_manifests_, *metadata_);
	}

	[[nodiscard]] VectorManifest materializeVectorManifest(uint64_t indexId) {
		MetadataSnapshotPin snapshot = metadata_->openSnapshot();
		const IndexRootLocation root = metadata_->findIndexRoot(snapshot, indexId);
		const uint64_t vectorNodeMappingIndexId = VectorNodeMapping::selectMappingIndexId(snapshot, *metadata_, indexId);
		snapshot.release();
		buildRoot(vectorNodeMappingIndexId, indexId);
		snapshot = metadata_->openSnapshot();
		const IndexRootLocation vectorNodeRoot = metadata_->findIndexRoot(snapshot, vectorNodeMappingIndexId);
		snapshot.release();
		VectorManifest manifest;
		manifest.index_id = indexId;
		manifest.dimensions = 2U;
		manifest.metric = VectorDistanceMetric::VDM_L2;
		manifest.node_mapping_root_page_id = root.root_page_id;
		manifest.node_mapping_root_generation = root.root_generation;
		manifest.vector_node_mapping_index_id = vectorNodeRoot.index_id;
		manifest.vector_node_mapping_root_page_id = vectorNodeRoot.root_page_id;
		manifest.vector_node_mapping_root_generation = vectorNodeRoot.root_generation;
		return vector_manifests_->materialize(manifest, PhysicalSlotId(40U), PhysicalSlotId(95U), nextCheckpointLsn())
				.manifest;
	}

	[[nodiscard]] ScalarIndexTreeReclaimOptions options() const {
		return {8U, 8U, 8U, 16U, 16U, 16U};
	}
};

TEST_F(RelocatableScalarIndexTreeReachabilityReclaimerTest, RetiresDroppedAndOrphanedTreesWithoutBreakingPinnedSnapshots) {
	IndexDefinition sealed = sealedDefinition(7U, "sealed_score");
	publishSealed(sealed);
	buildRoot(sealed.index_id, 7U);
	const IndexDefinition activeDynamic = publishDynamic("online_score");
	buildRoot(activeDynamic.index_id, activeDynamic.index_id);
	const uint64_t orphanedOnlineId = activeDynamic.index_id + 1U;
	buildRoot(orphanedOnlineId, orphanedOnlineId);
	tombstoneSealed(sealed.index_id);

	MetadataSnapshotPin oldSnapshot = metadata_->openSnapshot();
	auto reclaimer = this->reclaimer();
	const ScalarIndexTreeReclaimReport report = reclaimer.reclaim(options(), nextCheckpointLsn());
	EXPECT_EQ(report.dropped_index_roots_retired, 2U);
	EXPECT_EQ(report.active_roots_retained, 1U);
	EXPECT_EQ(report.index_tree_pages_retired, 2U);
	EXPECT_GT(report.published_generation, report.scanned_generation);
	const std::vector<std::byte> rootKey = {std::byte{0x61}};
	EXPECT_EQ(BTreeIndex(sealed.index_id, *trees_, *metadata_).find(oldSnapshot, rootKey),
			  std::optional<uint64_t>(7U));
	oldSnapshot.release();

	MetadataSnapshotPin current = metadata_->openSnapshot();
	EXPECT_FALSE(metadata_->findIndexRootOptional(current, sealed.index_id).has_value());
	EXPECT_TRUE(metadata_->findIndexRootOptional(current, activeDynamic.index_id).has_value());
	EXPECT_FALSE(metadata_->findIndexRootOptional(current, orphanedOnlineId).has_value());
	current.release();

	const ScalarIndexTreeReclaimReport second = reclaimer.reclaim(options(), nextCheckpointLsn());
	EXPECT_EQ(second.index_tree_pages_retired, 0U);
	EXPECT_EQ(second.published_generation, second.scanned_generation);
}

TEST_F(RelocatableScalarIndexTreeReachabilityReclaimerTest,
	   DoesNotPartiallyRetireAnObsoleteTreeWhenMetadataPublicationIsInterrupted) {
	IndexDefinition definition = sealedDefinition(7U, "interrupted_score");
	publishSealed(definition);
	buildRoot(definition.index_id, 71U);
	tombstoneSealed(definition.index_id);

	MetadataSnapshotPin oldSnapshot = metadata_->openSnapshot();
	const std::vector<std::byte> rootKey = {std::byte{0x61U}};
	EXPECT_EQ(BTreeIndex(definition.index_id, *trees_, *metadata_).find(oldSnapshot, rootKey),
			  std::optional<uint64_t>(71U));

	MetadataPublishOptions interrupted;
	interrupted.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE;
	auto treeReclaimer = reclaimer();
	EXPECT_THROW(static_cast<void>(treeReclaimer.reclaim(options(), nextCheckpointLsn(), interrupted)),
				 BootstrapPublishInterrupted);

	// The old root stays visible until one atomic publication retires the root
	// mapping and every reachable page together.
	MetadataSnapshotPin current = metadata_->openSnapshot();
	EXPECT_TRUE(metadata_->findIndexRootOptional(current, definition.index_id).has_value());
	EXPECT_EQ(BTreeIndex(definition.index_id, *trees_, *metadata_).find(current, rootKey),
			  std::optional<uint64_t>(71U));
	current.release();
	oldSnapshot.release();
	reopen();

	auto reopenedReclaimer = reclaimer();
	const ScalarIndexTreeReclaimReport retry = reopenedReclaimer.reclaim(options(), nextCheckpointLsn());
	EXPECT_EQ(retry.dropped_index_roots_retired, 1U);
	EXPECT_EQ(retry.index_tree_pages_retired, 1U);

	current = metadata_->openSnapshot();
	EXPECT_FALSE(metadata_->findIndexRootOptional(current, definition.index_id).has_value());
}

TEST_F(RelocatableScalarIndexTreeReachabilityReclaimerTest, RejectsUnknownSealedRootsAndInvalidBounds) {
	buildRoot(11U, 11U);
	auto reclaimer = this->reclaimer();
	ScalarIndexTreeReclaimOptions invalid = options();
	invalid.maximum_tree_pages = 0U;
	EXPECT_THROW(static_cast<void>(reclaimer.reclaim(invalid, nextCheckpointLsn())), FormatException);
	EXPECT_THROW(static_cast<void>(reclaimer.reclaim(options(), nextCheckpointLsn())), FormatException);
}

TEST_F(RelocatableScalarIndexTreeReachabilityReclaimerTest, RetainsActiveConstraintSupportTreeAndRetiresDroppedOrOrphanedRoots) {
	const ConstraintDefinition active = publishUniqueConstraint();
	const uint64_t activeId = active.constraint_id;
	const uint64_t orphanId = activeId + 1U;
	buildRoot(activeId, 7U);
	buildRoot(orphanId, 8U);

	auto reclaimer = this->reclaimer();
	const ScalarIndexTreeReclaimReport first = reclaimer.reclaim(options(), nextCheckpointLsn());
	EXPECT_EQ(first.active_constraint_support_roots_retained, 1U);
	EXPECT_EQ(first.orphaned_constraint_support_roots_retired, 1U);
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_TRUE(metadata_->findIndexRootOptional(snapshot, activeId).has_value());
	EXPECT_FALSE(metadata_->findIndexRootOptional(snapshot, orphanId).has_value());
	snapshot.release();

	dropConstraint(active);
	const ScalarIndexTreeReclaimReport second = reclaimer.reclaim(options(), nextCheckpointLsn());
	EXPECT_EQ(second.orphaned_constraint_support_roots_retired, 1U);
	snapshot = metadata_->openSnapshot();
	EXPECT_FALSE(metadata_->findIndexRootOptional(snapshot, activeId).has_value());
	EXPECT_EQ(second.index_tree_pages_retired, 1U);
}

TEST_F(RelocatableScalarIndexTreeReachabilityReclaimerTest,
	   RetainsTombstonedVectorMappingUntilItsVisibleManifestIsRetired) {
	IndexDefinition vector = vectorDefinition(7U, "sealed_embedding");
	publishSealed(vector);
	buildRoot(vector.index_id, 7U);
	const VectorManifest manifest = materializeVectorManifest(vector.index_id);
	tombstoneSealed(vector.index_id);

	auto reclaimer = this->reclaimer();
	const ScalarIndexTreeReclaimReport retained = reclaimer.reclaim(options(), nextCheckpointLsn());
	EXPECT_EQ(retained.visible_vector_roots_retained, 2U);
	EXPECT_EQ(retained.index_tree_pages_retired, 0U);

	MetadataSnapshotPin oldSnapshot = metadata_->openSnapshot();
	static_cast<void>(vector_manifests_->retire(vector.index_id, manifest.committed_lsn, nextCheckpointLsn()));
	const ScalarIndexTreeReclaimReport retired = reclaimer.reclaim(options(), nextCheckpointLsn());
	EXPECT_EQ(retired.dropped_index_roots_retired, 2U);
	EXPECT_EQ(retired.index_tree_pages_retired, 2U);
	const std::vector<std::byte> rootKey = {std::byte{0x61}};
	EXPECT_EQ(BTreeIndex(vector.index_id, *trees_, *metadata_).find(oldSnapshot, rootKey),
			  std::optional<uint64_t>(7U));
	oldSnapshot.release();
	MetadataSnapshotPin current = metadata_->openSnapshot();
	EXPECT_FALSE(metadata_->findIndexRootOptional(current, vector.index_id).has_value());
	current.release();
}

TEST_F(RelocatableScalarIndexTreeReachabilityReclaimerTest, RejectsEveryZeroScanBoundAndAnInvalidCheckpointLsn) {
	auto reclaimer = this->reclaimer();
	const std::array<size_t ScalarIndexTreeReclaimOptions::*, 6U> boundedMembers = {
			&ScalarIndexTreeReclaimOptions::maximum_index_definitions,
			&ScalarIndexTreeReclaimOptions::maximum_constraint_definitions,
			&ScalarIndexTreeReclaimOptions::maximum_vector_manifests,
			&ScalarIndexTreeReclaimOptions::maximum_index_roots,
			&ScalarIndexTreeReclaimOptions::maximum_tree_pages,
			&ScalarIndexTreeReclaimOptions::maximum_retirements,
	};
	for (const auto member: boundedMembers) {
		ScalarIndexTreeReclaimOptions invalid = options();
		invalid.*member = 0U;
		EXPECT_THROW(static_cast<void>(reclaimer.reclaim(invalid, nextCheckpointLsn())), FormatException);
	}
	EXPECT_THROW(static_cast<void>(reclaimer.reclaim(options(), LogSequenceNumber{})), FormatException);
}

TEST_F(RelocatableScalarIndexTreeReachabilityReclaimerTest, RetainsANonUniqueConstraintWithoutASupportTree) {
	static_cast<void>(publishConstraint(ConstraintDefinitionKind::CDK_NOT_NULL, "person_email_not_null"));
	auto reclaimer = this->reclaimer();
	const ScalarIndexTreeReclaimReport report = reclaimer.reclaim(options(), nextCheckpointLsn());
	EXPECT_EQ(report.constraint_definitions, 1U);
	EXPECT_EQ(report.active_constraint_support_roots_retained, 0U);
	EXPECT_EQ(report.index_tree_pages_retired, 0U);
}

TEST_F(RelocatableScalarIndexTreeReachabilityReclaimerTest,
	   RetainsNodeKeySupportTreesAndRejectsMissingOrUnneededConstraintSupportTrees) {
	const ConstraintDefinition nodeKey =
			publishConstraint(ConstraintDefinitionKind::CDK_NODE_KEY, "person_email_node_key");
	buildRoot(nodeKey.constraint_id, 7U);
	auto reclaimer = this->reclaimer();
	const ScalarIndexTreeReclaimReport retained = reclaimer.reclaim(options(), nextCheckpointLsn());
	EXPECT_EQ(retained.active_constraint_support_roots_retained, 1U);

	const ConstraintDefinition unique = publishUniqueConstraint();
	EXPECT_THROW(static_cast<void>(reclaimer.reclaim(options(), nextCheckpointLsn())), FormatException);
	buildRoot(unique.constraint_id, 9U);
	const ConstraintDefinition notNull =
			publishConstraint(ConstraintDefinitionKind::CDK_NOT_NULL, "person_email_not_null");
	buildRoot(notNull.constraint_id, 11U);
	EXPECT_THROW(static_cast<void>(reclaimer.reclaim(options(), nextCheckpointLsn())), FormatException);
}

TEST_F(RelocatableScalarIndexTreeReachabilityReclaimerTest, RejectsAnActiveVectorRootWithoutItsVisibleManifest) {
	IndexDefinition vector = vectorDefinition(7U, "active_embedding");
	publishSealed(vector);
	buildRoot(vector.index_id, 7U);
	auto reclaimer = this->reclaimer();
	EXPECT_THROW(static_cast<void>(reclaimer.reclaim(options(), nextCheckpointLsn())), FormatException);
}

TEST_F(RelocatableScalarIndexTreeReachabilityReclaimerTest, RejectsAnIncompleteVisibleVectorManifest) {
	VectorManifest rawOnly;
	rawOnly.index_id = RSS_SYSTEM_CATALOG_INDEX_ID_FIRST + 37U;
	rawOnly.dimensions = 2U;
	rawOnly.metric = VectorDistanceMetric::VDM_L2;
	static_cast<void>(vector_manifests_->materialize(rawOnly, PhysicalSlotId(40U), PhysicalSlotId(95U),
			nextCheckpointLsn()));
	auto reclaimer = this->reclaimer();
	EXPECT_THROW(static_cast<void>(reclaimer.reclaim(options(), nextCheckpointLsn())), FormatException);
}

TEST_F(RelocatableScalarIndexTreeReachabilityReclaimerTest, RejectsAVisibleVectorManifestWhoseMappingRootWasReplaced) {
	IndexDefinition vector = vectorDefinition(7U, "stale_embedding");
	publishSealed(vector);
	buildRoot(vector.index_id, 7U);
	static_cast<void>(materializeVectorManifest(vector.index_id));
	buildRoot(vector.index_id, 8U);
	auto reclaimer = this->reclaimer();
	EXPECT_THROW(static_cast<void>(reclaimer.reclaim(options(), nextCheckpointLsn())), FormatException);
}

TEST_F(RelocatableScalarIndexTreeReachabilityReclaimerTest, EnforcesVectorManifestAndIndexRootScanBounds) {
	IndexDefinition first = vectorDefinition(7U, "first_embedding");
	IndexDefinition second = vectorDefinition(1U, "second_embedding");
	publishSealed(first);
	publishSealed(second);
	buildRoot(first.index_id, first.index_id);
	static_cast<void>(materializeVectorManifest(first.index_id));
	buildRoot(second.index_id, second.index_id);
	static_cast<void>(materializeVectorManifest(second.index_id));
	auto reclaimer = this->reclaimer();
	ScalarIndexTreeReclaimOptions manifestBound = options();
	manifestBound.maximum_vector_manifests = 1U;
	EXPECT_THROW(static_cast<void>(reclaimer.reclaim(manifestBound, nextCheckpointLsn())), FormatException);

	const uint64_t metadataRootOne = (UINT64_C(1) << 63U) | 0x100U;
	const uint64_t metadataRootTwo = (UINT64_C(1) << 63U) | 0x101U;
	buildRoot(metadataRootOne, 1U);
	buildRoot(metadataRootTwo, 2U);
	ScalarIndexTreeReclaimOptions rootBound = options();
	rootBound.maximum_index_roots = 1U;
	EXPECT_THROW(static_cast<void>(reclaimer.reclaim(rootBound, nextCheckpointLsn())), FormatException);
}

TEST_F(RelocatableScalarIndexTreeReachabilityReclaimerTest, EnforcesRetirementAndTreePageBounds) {
	IndexDefinition first = sealedDefinition(7U, "label_index", IndexDefinitionKind::IDK_LABEL);
	IndexDefinition second = sealedDefinition(8U, "composite_index", IndexDefinitionKind::IDK_COMPOSITE);
	publishSealed(first);
	publishSealed(second);
	const std::array definitions = {first, second};
	buildRoot(definitions[0].index_id, 7U);
	buildRoot(definitions[1].index_id, 8U);
	tombstoneSealed(definitions[0].index_id);
	tombstoneSealed(definitions[1].index_id);
	auto reclaimer = this->reclaimer();
	ScalarIndexTreeReclaimOptions retirementBound = options();
	retirementBound.maximum_retirements = 1U;
	EXPECT_THROW(static_cast<void>(reclaimer.reclaim(retirementBound, nextCheckpointLsn())), FormatException);
}

TEST_F(RelocatableScalarIndexTreeReachabilityReclaimerTest, RejectsAnOversizedMultiLevelTreeBeforeRetiringAnyRoot) {
	const uint64_t orphanedOnlineId = RSS_SYSTEM_CATALOG_INDEX_ID_FIRST + 7U;
	buildRoot(orphanedOnlineId, 1U, 1024U);
	auto reclaimer = this->reclaimer();
	ScalarIndexTreeReclaimOptions treeBound = options();
	treeBound.maximum_tree_pages = 1U;
	EXPECT_THROW(static_cast<void>(reclaimer.reclaim(treeBound, nextCheckpointLsn())), FormatException);
}

TEST_F(RelocatableScalarIndexTreeReachabilityReclaimerTest, RetiresEveryPageOfAnOrphanedMultiLevelTree) {
	const uint64_t orphanedOnlineId = RSS_SYSTEM_CATALOG_INDEX_ID_FIRST + 9U;
	buildRoot(orphanedOnlineId, 1U, 1024U);
	auto reclaimer = this->reclaimer();
	ScalarIndexTreeReclaimOptions treeOptions = options();
	treeOptions.maximum_tree_pages = 32U;
	treeOptions.maximum_retirements = 32U;
	const ScalarIndexTreeReclaimReport report = reclaimer.reclaim(treeOptions, nextCheckpointLsn());
	EXPECT_EQ(report.dropped_index_roots_retired, 1U);
	EXPECT_GT(report.index_tree_pages_retired, 1U);
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_FALSE(metadata_->findIndexRootOptional(snapshot, orphanedOnlineId).has_value());
}

TEST_F(RelocatableScalarIndexTreeReachabilityReclaimerTest, RetiresABTreeFromASharedIndexPool) {
	const uint64_t orphanedOnlineId = RSS_SYSTEM_CATALOG_INDEX_ID_FIRST + 11U;
	buildManualRoot(orphanedOnlineId, orphanedOnlineId + 1U);
	auto reclaimer = this->reclaimer();
	const ScalarIndexTreeReclaimReport report = reclaimer.reclaim(options(), nextCheckpointLsn());
	EXPECT_EQ(report.dropped_index_roots_retired, 1U);
	EXPECT_EQ(report.index_tree_pages_retired, 1U);
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_FALSE(metadata_->findIndexRootOptional(snapshot, orphanedOnlineId).has_value());
}

TEST_F(RelocatableScalarIndexTreeReachabilityReclaimerTest, ReleasesRetiredTreeFramesOnlyAfterPinnedSnapshotsDrain) {
	const uint64_t orphanedOnlineId = RSS_SYSTEM_CATALOG_INDEX_ID_FIRST + 13U;
	buildManualRoot(orphanedOnlineId, orphanedOnlineId + 1U);
	MetadataSnapshotPin oldSnapshot = metadata_->openSnapshot();
	const IndexRootLocation oldRoot = metadata_->findIndexRoot(oldSnapshot, orphanedOnlineId);
	const PageLocation oldLocation = metadata_->findPageLocation(oldSnapshot, oldRoot.root_page_id);
	const uint8_t availableBefore = pages_->availableFrameCount(oldSnapshot, oldLocation.segment_id);

	auto reclaimer = this->reclaimer();
	const ScalarIndexTreeReclaimReport report = reclaimer.reclaim(options(), nextCheckpointLsn());
	EXPECT_EQ(report.index_tree_pages_retired, 1U);
	EXPECT_EQ(BTreeIndex(orphanedOnlineId, *trees_, *metadata_).find(oldSnapshot, std::array<std::byte, 1U>{std::byte{0x61U}}),
			  std::optional<uint64_t>(1U));
	MetadataSnapshotPin current = metadata_->openSnapshot();
	EXPECT_FALSE(metadata_->findIndexRootOptional(current, orphanedOnlineId).has_value());
	EXPECT_EQ(pages_->availableFrameCount(current, oldLocation.segment_id), availableBefore);
	current.release();

	oldSnapshot.release();
	trees_->reclaimRetiredFrames(oldLocation.segment_id);
	MetadataSnapshotPin reclaimed = metadata_->openSnapshot();
	EXPECT_EQ(pages_->availableFrameCount(reclaimed, oldLocation.segment_id), static_cast<uint8_t>(availableBefore + 1U));
}

TEST_F(RelocatableScalarIndexTreeReachabilityReclaimerTest, RejectsABTreeWithOneChildReferencedTwice) {
	const uint64_t orphanedOnlineId = RSS_SYSTEM_CATALOG_INDEX_ID_FIRST + 12U;
	buildManualRoot(orphanedOnlineId, orphanedOnlineId, true);
	auto reclaimer = this->reclaimer();
	EXPECT_THROW(static_cast<void>(reclaimer.reclaim(options(), nextCheckpointLsn())), FormatException);
}

TEST_F(RelocatableScalarIndexTreeReachabilityReclaimerTest, RequiresEveryCollaboratorToUseTheSameMetadataStore) {
	const std::filesystem::path foreignPath = data_path_.string() + ".foreign";
	const std::filesystem::path foreignWalPath = foreignPath.string() + ".foreign";
	auto foreignFile = std::make_shared<std::fstream>(foreignPath,
			std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
	auto foreignWalFile = std::make_shared<std::fstream>(foreignWalPath,
			std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
	ASSERT_TRUE(foreignFile->is_open());
	ASSERT_TRUE(foreignWalFile->is_open());
	auto foreignIo = std::make_shared<StorageIO>(foreignFile, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto foreignWalIo = std::make_shared<StorageIO>(foreignWalFile, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto foreignMetadata = RelocatableMetadataStore::initializeNew(foreignIo, initialSuperblock());
	SlottedPagePool foreignPages(foreignIo, *foreignMetadata);
	BTreePageStore foreignTrees(foreignPages, *foreignMetadata);
	SystemCatalog foreignCatalog(foreignTrees, *foreignMetadata);
	VectorManifestStore foreignManifests(foreignIo, *foreignMetadata);
	EXPECT_THROW(
			static_cast<void>(ScalarIndexTreeReachabilityReclaimer(
					foreignTrees, *catalog_, *vector_manifests_, *metadata_)),
			std::invalid_argument);
	EXPECT_THROW(
			static_cast<void>(ScalarIndexTreeReachabilityReclaimer(
					*trees_, foreignCatalog, *vector_manifests_, *metadata_)),
			std::invalid_argument);
	EXPECT_THROW(
			static_cast<void>(ScalarIndexTreeReachabilityReclaimer(
					*trees_, *catalog_, foreignManifests, *metadata_)),
			std::invalid_argument);
	foreignMetadata.reset();
	foreignIo.reset();
	foreignWalIo.reset();
	foreignFile->close();
	foreignWalFile->close();
	foreignFile.reset();
	foreignWalFile.reset();
	std::error_code error;
	std::filesystem::remove(foreignPath, error);
	std::filesystem::remove(foreignWalPath, error);
}
