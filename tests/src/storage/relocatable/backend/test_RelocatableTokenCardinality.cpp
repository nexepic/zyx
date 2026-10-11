#include "graph/storage/relocatable/entity/NativeGraphWritePlanner.hpp"
#include "graph/storage/relocatable/schema/SystemCatalog.hpp"
#include "graph/storage/relocatable/entity/NativeMembershipAccessCost.hpp"
#include "storage/relocatable/backend/RelocatableStorageBackendTestFixture.hpp"
using namespace relocatable_storage_backend_test;
namespace {
	struct TokenStatisticsView {
		std::shared_ptr<std::fstream> file;
		std::shared_ptr<StorageIO> io;
		std::unique_ptr<RelocatableMetadataStore> metadata;
		SlottedPagePool pool;
		EntityPageStore entities;
		EntityLabelSetStore labels;
		PropertyCollectionRootStore properties;
		BTreePageStore pages;
		SystemCatalog catalog;
		explicit TokenStatisticsView(const std::filesystem::path &path) :
			file(std::make_shared<std::fstream>(path, std::ios::in | std::ios::out | std::ios::binary)),
			io(std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE)),
			metadata(RelocatableMetadataStore::open(io)), pool(io, *metadata, SharedPagePoolOpenMode::SPPOM_READ_ONLY),
			entities(io, *metadata), labels(io, *metadata, pool), properties(pool, *metadata), pages(pool, *metadata),
			catalog(pages, *metadata) {}
		void seedPartial() {
			const uint64_t first = io->fileSize() / RSS_SEGMENT_SIZE + 8U;
			NativeGraphWritePlannerOptions options;
			options.first_slot = PhysicalSlotId(first);
			options.last_slot = PhysicalSlotId(first + 64U);
			options.entity_pool_owner_id = 0x223456U;
			options.property_roots = {PhysicalSlotId(first), PhysicalSlotId(first + 64U), 0x223457U};
			NativeGraphWritePlanner planner(*metadata, entities, labels, properties, pages, options);
			auto tx = planner.beginTransaction();
			auto session = catalog.beginWrite(tx);
			session.setTokenCardinality(SystemCatalogSchemaTokenKind::SCSTK_LABEL, 999U, 88U);
			session.setTokenCardinality(SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, 0U, 1234U);
			session.finish();
			tx.stageScalarIndexErase(
					pages, RSS_SYSTEM_CATALOG_INDEX_ID,
					SystemCatalogTokenCardinalityCodec::completeKey(SystemCatalogSchemaTokenKind::SCSTK_LABEL));
			tx.stageScalarIndexErase(pages, RSS_SYSTEM_CATALOG_INDEX_ID,
									 SystemCatalogTokenCardinalityCodec::completeKey(
											 SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE));
			if (!tx.commit().committed())
				throw std::runtime_error("partial statistics fixture conflict");
			io->sync();
		}
		std::optional<uint64_t> count(SystemCatalogSchemaTokenKind kind, std::string_view name) {
			auto snapshot = metadata->openSnapshot();
			const auto token =
					name.empty() ? std::optional<uint64_t>(0U) : catalog.findSchemaToken(snapshot, kind, name);
			if (!token)
				return std::nullopt;
			return catalog.readTokenCardinality(snapshot, kind, *token);
		}
	};
} // namespace
TEST_F(RelocatableStorageBackendTest, TokenCountsFollowOrdinaryFinalStateAndDetachDeletion) {
	auto options = backendOptions();
	options.open_mode = OpenMode::OPEN_CREATE_NEW_FILE;
	auto backend = RelocatableStorageBackend::initializeNew(options);

	backend->open();
	const auto nodes = backend->createLogicalNodes({{{"Person", "Other"}, {}}, {{"Person"}, {}}});
	const auto typed = backend->createLogicalEdges({{nodes[0], nodes[1], "KNOWS", {}}, {nodes[0], nodes[0], "", {}}});
	const auto label = SystemCatalogSchemaTokenKind::SCSTK_LABEL;
	const auto type = SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE;
	TokenStatisticsView view(data_path_);
	EXPECT_EQ(view.count(label, "Person"), std::optional<uint64_t>(2U));
	EXPECT_EQ(view.count(label, "Other"), std::optional<uint64_t>(1U));
	EXPECT_EQ(view.count(type, "KNOWS"), std::optional<uint64_t>(1U));
	EXPECT_EQ(view.count(type, ""), std::optional<uint64_t>(1U));
	auto old = view.metadata->openSnapshot();
	const auto personToken = view.catalog.findSchemaToken(old, label, "Person");
	ASSERT_TRUE(personToken);
	LogicalGraphMutationBatch replace;
	replace.node_label_replacements.push_back(
			{{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, nodes[0]},
			 {"Other"}});
	{
		auto tx = backend->beginLogicalGraphTransaction(false);
		static_cast<void>(tx->stageMutationBatch(replace));
		tx->rollback();
	}
	EXPECT_EQ(view.count(label, "Person"), std::optional<uint64_t>(2U));
	{
		auto tx = backend->beginLogicalGraphTransaction(false);
		static_cast<void>(tx->stageMutationBatch(replace));
		replace.node_label_replacements.front().labels = {"Person", "Other"};
		static_cast<void>(tx->stageMutationBatch(replace));
		static_cast<void>(tx->commit());
	}
	EXPECT_EQ(view.count(label, "Person"), std::optional<uint64_t>(2U));
	backend->deleteLogicalNodes({{nodes[0], true, 16U}});
	EXPECT_EQ(view.count(label, "Person"), std::optional<uint64_t>(1U));
	EXPECT_EQ(view.count(label, "Other"), std::optional<uint64_t>(0U));
	EXPECT_EQ(view.count(type, "KNOWS"), std::optional<uint64_t>(0U));
	EXPECT_EQ(view.count(type, ""), std::optional<uint64_t>(0U));
	EXPECT_EQ(view.catalog.readTokenCardinality(old, label, *personToken), std::optional<uint64_t>(2U));

	{
		auto tx = backend->beginLogicalGraphTransaction(false);
		LogicalGraphMutationBatch create;
		create.node_creates = {{{"Person"}, {}}};
		const auto staged = tx->stageMutationBatch(create);
		ASSERT_EQ(staged.created_node_ids.size(), 1U);
		LogicalGraphMutationBatch erase;
		erase.node_deletes = {{staged.created_node_ids.front(), false, 0U}};
		static_cast<void>(tx->stageMutationBatch(erase));
		static_cast<void>(tx->commit());
	}
	EXPECT_EQ(view.count(label, "Person"), std::optional<uint64_t>(1U));
	old.release();
	backend->close();
	backend->open();
	EXPECT_EQ(view.count(label, "Person"), std::optional<uint64_t>(1U));
	backend->close();
}

TEST_F(RelocatableStorageBackendTest, TokenCountsFollowStreamedImportForKnownNewAndUntypedEdges) {
	auto options = backendOptions();
	options.open_mode = OpenMode::OPEN_CREATE_NEW_FILE;
	options.online_index_sort_run_bytes = 4096U;
	options.maximum_online_index_sort_runs = 2U;
	auto backend = RelocatableStorageBackend::initializeNew(options);
	backend->open();
	const auto nodes = backend->createLogicalNodes({{{"Person"}, {}}});
	static_cast<void>(backend->createLogicalEdges({{nodes[0], nodes[0], "KNOWS", {}}, {nodes[0], nodes[0], "", {}}}));
	TokenStatisticsView view(data_path_);
	auto old = view.metadata->openSnapshot();
	const auto type = SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE;
	const auto token = view.catalog.findSchemaToken(old, type, "KNOWS");
	ASSERT_TRUE(token);
	auto transaction = backend->beginLogicalGraphTransaction(false);
	auto *bulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(transaction.get());
	ASSERT_NE(bulk, nullptr);
	const std::vector<uint64_t> endpoints(513U, nodes[0]);
	const auto known = bulk->appendPersistedBulkEdges({"KNOWS", endpoints, endpoints, {}});
	const auto fresh = bulk->appendPersistedBulkEdges({"NewType", endpoints, endpoints, {}});
	const auto untyped = bulk->appendPersistedBulkEdges({"", endpoints, endpoints, {}});
	EXPECT_EQ(view.count(type, "KNOWS"), std::optional<uint64_t>(1U));
	static_cast<void>(transaction->commit());
	transaction.reset();
	EXPECT_EQ(view.count(type, "KNOWS"), std::optional<uint64_t>(514U));
	EXPECT_EQ(view.count(type, "NewType"), std::optional<uint64_t>(513U));
	EXPECT_EQ(view.count(type, ""), std::optional<uint64_t>(514U));
	EXPECT_EQ(view.catalog.readTokenCardinality(old, type, *token), std::optional<uint64_t>(1U));
	EXPECT_FALSE(view.catalog.findSchemaToken(old, type, "NewType").has_value());
	{
		auto snapshot = backend->openLogicalGraphReadSnapshot();
		std::vector<uint64_t> ids;
		snapshot->visitEdgeRefsForTypePredicate(513U, snapshot->findRelationshipTypeToken("NewType"),
												[&](const EntityRef &ref) {
													ids.push_back(ref.entity_id);
													return true;
												});
		EXPECT_EQ(ids, fresh);
	}
	old.release();
	backend->close();
	backend->open();
	EXPECT_EQ(view.count(type, "NewType"), std::optional<uint64_t>(513U));
	{
		auto tx = backend->beginLogicalGraphTransaction(false);
		auto *untypedBulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(tx.get());
		ASSERT_NE(untypedBulk, nullptr);
		const std::array<uint64_t, 1U> one{nodes[0]};
		static_cast<void>(untypedBulk->appendPersistedBulkEdges({"", one, one, {}}));
		static_cast<void>(tx->commit());
	}
	EXPECT_EQ(view.count(type, ""), std::optional<uint64_t>(515U));
	EXPECT_EQ(view.count(type, "KNOWS"), std::optional<uint64_t>(514U));
	backend->close();
}

TEST_F(RelocatableStorageBackendTest, TokenCountsRebaseStreamedWritesAndRollBackCatalogFailure) {
	auto options = backendOptions();
	options.open_mode = OpenMode::OPEN_CREATE_NEW_FILE;
	auto backend = RelocatableStorageBackend::initializeNew(options);
	backend->open();
	const auto nodes = backend->createLogicalNodes({{{"Person"}, {}}, {{"Person"}, {}}});
	static_cast<void>(backend->createLogicalEdges({{nodes[0], nodes[1], "KNOWS", {}}}));
	TokenStatisticsView view(data_path_);
	auto old = view.metadata->openSnapshot();
	const auto type = SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE;
	const auto token = view.catalog.findSchemaToken(old, type, "KNOWS");
	ASSERT_TRUE(token);
	const std::array<uint64_t, 1U> sources{nodes[0]}, targets{nodes[1]};
	{
		auto tx = backend->beginLogicalGraphTransaction(false);
		auto *bulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(tx.get());
		ASSERT_NE(bulk, nullptr);
		static_cast<void>(bulk->appendPersistedBulkEdges({"KNOWS", sources, targets, {}}));
		static_cast<void>(backend->createLogicalEdges({{nodes[1], nodes[0], "KNOWS", {}}}));
		EXPECT_EQ(view.count(type, "KNOWS"), std::optional<uint64_t>(2U));
		static_cast<void>(tx->commit());
	}
	EXPECT_EQ(view.count(type, "KNOWS"), std::optional<uint64_t>(3U));
	auto rejected = backend->beginLogicalGraphTransaction(false);
	auto *bulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(rejected.get());
	ASSERT_NE(bulk, nullptr);
	const auto ids = bulk->appendPersistedBulkEdges({"KNOWS", sources, targets, {}});
	bool injected = false;
	{
		StorageIO::ScopedNativeWriteHookForTesting hook([&](file_handle_t fd, const void *bytes, size_t size,
															pwrite_off_t offset) {
			bool catalogPage = false;
			if (!injected && size == RSS_PAGE_SIZE) {
				try {
					const auto owner = PageOwnerHeaderCodec::decode(
							std::span(static_cast<const std::byte *>(bytes), size).first(RSS_PAGE_OWNER_HEADER_BYTES));
					catalogPage =
							owner.kind == PageOwnerKind::POK_INDEX && owner.owner_id == RSS_SYSTEM_CATALOG_INDEX_ID;
				} catch (const std::exception &) {
				}
			}
			if (catalogPage) {
				injected = true;
#ifdef _WIN32
				SetLastError(ERROR_DISK_FULL);
#else
				errno = ENOSPC;
#endif
				return static_cast<pwrite_ssize_t>(-1);
			}
			return portable_pwrite(fd, bytes, size, offset);
		});
		EXPECT_THROW(static_cast<void>(rejected->commit()), std::system_error);
	}
	EXPECT_TRUE(injected);
	EXPECT_EQ(rejected->state(), LogicalGraphTransactionState::LGTTS_ROLLED_BACK);
	rejected.reset();
	EXPECT_EQ(view.count(type, "KNOWS"), std::optional<uint64_t>(3U));
	{
		auto snapshot = backend->openLogicalGraphReadSnapshot();
		EXPECT_THROW(static_cast<void>(snapshot->readEdge(ids.front())), FormatException);
	}
	{
		auto tx = backend->beginLogicalGraphTransaction(false);
		auto *retry = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(tx.get());
		ASSERT_NE(retry, nullptr);
		static_cast<void>(retry->appendPersistedBulkEdges({"KNOWS", sources, targets, {}}));
		static_cast<void>(tx->commit());
	}
	EXPECT_EQ(view.count(type, "KNOWS"), std::optional<uint64_t>(4U));
	EXPECT_EQ(view.catalog.readTokenCardinality(old, type, *token), std::optional<uint64_t>(1U));
	old.release();
	backend->close();
}

TEST_F(RelocatableStorageBackendTest, TokenStatisticsRebuildReplacesPartialRecordsAndPublishesCompleteSnapshots) {
	auto options = backendOptions();
	options.open_mode = OpenMode::OPEN_CREATE_NEW_FILE;
	options.online_index_sort_run_bytes = 4096U;
	options.maximum_online_index_sort_runs = 2U;
	auto backend = RelocatableStorageBackend::initializeNew(options);
	backend->open();
	std::vector<LogicalGraphNodeCreateRequest> requests(513U, {{"Person", "Other", "Person"}, {}});
	const auto nodes = backend->createLogicalNodes(requests);
	std::vector<LogicalGraphEdgeCreateRequest> edges(513U, {nodes[0], nodes[0], "", {}});
	static_cast<void>(backend->createLogicalEdges(edges));
	backend->close();
	TokenStatisticsView view(data_path_);
	view.seedPartial();
	auto old = view.metadata->openSnapshot();
	const auto label = SystemCatalogSchemaTokenKind::SCSTK_LABEL;
	const auto type = SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE;
	EXPECT_FALSE(view.catalog.hasCompleteTokenCardinality(old, label));
	backend->open();
	backend->rebuildLogicalTokenCardinalityStatistics();
	EXPECT_EQ(view.count(label, "Person"), std::optional<uint64_t>(513U));
	EXPECT_EQ(view.count(label, "Other"), std::optional<uint64_t>(513U));
	EXPECT_EQ(view.count(type, ""), std::optional<uint64_t>(513U));
	{
		auto current = view.metadata->openSnapshot();
		EXPECT_EQ(view.catalog.readTokenCardinality(current, label, 999U), std::optional<uint64_t>(0U));
	}
	EXPECT_FALSE(view.catalog.hasCompleteTokenCardinality(old, label));
	EXPECT_EQ(backend->readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({513U, 513U}));
	EXPECT_TRUE(backend->listLogicalIndexes(1U).empty());
	old.release();
	backend->close();
	backend->open();
	EXPECT_EQ(view.count(type, ""), std::optional<uint64_t>(513U));
	backend->close();
}

TEST_F(RelocatableStorageBackendTest, TokenStatisticsRebuildCancellationAndScanBoundsNeverPublishPartialCounts) {
	auto options = backendOptions();
	options.open_mode = OpenMode::OPEN_CREATE_NEW_FILE;
	options.maximum_online_index_scan_nodes = 1U;
	auto backend = RelocatableStorageBackend::initializeNew(options);
	backend->open();
	static_cast<void>(backend->createLogicalNodes({{{"Person"}, {}}, {{"Person"}, {}}}));
	backend->close();
	TokenStatisticsView view(data_path_);
	view.seedPartial();
	backend->open();
	auto before = view.metadata->openSnapshot();
	const auto generation = before.superblock().generation;
	before.release();
	EXPECT_THROW(backend->rebuildLogicalTokenCardinalityStatistics(), std::runtime_error);
	EXPECT_FALSE(view.count(SystemCatalogSchemaTokenKind::SCSTK_LABEL, "Person").has_value());
	{
		auto after = view.metadata->openSnapshot();
		EXPECT_EQ(after.superblock().generation, generation);
	}
	backend->close();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	options.maximum_online_index_scan_nodes = 8U;
	RelocatableStorageBackend retry(options);
	retry.open();
	size_t calls = 0U;
	EXPECT_THROW(retry.rebuildLogicalTokenCardinalityStatistics([&] {
		if (++calls == 2U)
			throw std::runtime_error("cancel statistics");
	}),
				 std::runtime_error);
	EXPECT_EQ(calls, 2U);
	{
		auto after = view.metadata->openSnapshot();
		EXPECT_EQ(after.superblock().generation, generation);
	}
	retry.rebuildLogicalTokenCardinalityStatistics();
	EXPECT_EQ(view.count(SystemCatalogSchemaTokenKind::SCSTK_LABEL, "Person"), std::optional<uint64_t>(2U));
	retry.close();
}

TEST_F(RelocatableStorageBackendTest, TokenStatisticsRebuildRejectsAChangedSourceSnapshot) {
	auto options = backendOptions();
	options.open_mode = OpenMode::OPEN_CREATE_NEW_FILE;
	auto backend = RelocatableStorageBackend::initializeNew(options);
	backend->open();
	static_cast<void>(backend->createLogicalNodes({{{"Person"}, {}}}));
	backend->close();
	TokenStatisticsView view(data_path_);
	view.seedPartial();
	backend->open();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend concurrent(options);
	concurrent.open();
	size_t calls = 0U;
	EXPECT_THROW(backend->rebuildLogicalTokenCardinalityStatistics([&] {
		if (++calls == 2U)
			static_cast<void>(concurrent.createLogicalNodes({{{"Person"}, {}}}));
	}),
				 FormatException);
	EXPECT_GE(calls, 2U);
	concurrent.close();
	backend->close();
	backend->open();
	EXPECT_FALSE(view.count(SystemCatalogSchemaTokenKind::SCSTK_LABEL, "Person").has_value());
	EXPECT_EQ(backend->readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({2U, 0U}));
	backend->rebuildLogicalTokenCardinalityStatistics();
	EXPECT_EQ(view.count(SystemCatalogSchemaTokenKind::SCSTK_LABEL, "Person"), std::optional<uint64_t>(2U));
	backend->close();
	options.open_mode = OpenMode::OPEN_READ_ONLY_EXISTING_FILE;
	RelocatableStorageBackend readOnly(options);
	readOnly.open();
	EXPECT_THROW(readOnly.rebuildLogicalTokenCardinalityStatistics(), std::runtime_error);
	readOnly.close();
}

TEST_F(RelocatableStorageBackendTest, TokenStatisticsRebuildCatalogWriteFailureRetainsUnknownStateAndAllowsRetry) {
	auto options = backendOptions();
	options.open_mode = OpenMode::OPEN_CREATE_NEW_FILE;
	auto backend = RelocatableStorageBackend::initializeNew(options);
	backend->open();
	static_cast<void>(backend->createLogicalNodes({{{"Person"}, {}}}));
	backend->close();
	TokenStatisticsView view(data_path_);
	view.seedPartial();
	backend->open();
	auto before = view.metadata->openSnapshot();
	const auto generation = before.superblock().generation;
	before.release();
	bool injected = false;
	{
		StorageIO::ScopedNativeWriteHookForTesting hook([&](file_handle_t fd, const void *bytes, size_t size,
															pwrite_off_t offset) {
			bool catalogPage = false;
			if (!injected && size == RSS_PAGE_SIZE) {
				try {
					const auto owner = PageOwnerHeaderCodec::decode(
							std::span(static_cast<const std::byte *>(bytes), size).first(RSS_PAGE_OWNER_HEADER_BYTES));
					catalogPage =
							owner.kind == PageOwnerKind::POK_INDEX && owner.owner_id == RSS_SYSTEM_CATALOG_INDEX_ID;
				} catch (const std::exception &) {
				}
			}
			if (catalogPage) {
				injected = true;
#ifdef _WIN32
				SetLastError(ERROR_DISK_FULL);
#else
				errno = ENOSPC;
#endif
				return static_cast<pwrite_ssize_t>(-1);
			}
			return portable_pwrite(fd, bytes, size, offset);
		});
		EXPECT_THROW(backend->rebuildLogicalTokenCardinalityStatistics(), std::system_error);
	}
	EXPECT_TRUE(injected);
	{
		auto unchanged = view.metadata->openSnapshot();
		EXPECT_EQ(unchanged.superblock().generation, generation);
		EXPECT_FALSE(view.catalog.hasCompleteTokenCardinality(unchanged, SystemCatalogSchemaTokenKind::SCSTK_LABEL));
	}
	backend->rebuildLogicalTokenCardinalityStatistics();
	EXPECT_EQ(view.count(SystemCatalogSchemaTokenKind::SCSTK_LABEL, "Person"), std::optional<uint64_t>(1U));
	backend->close();
}

TEST_F(RelocatableStorageBackendTest, LabelPlanningEstimatesUseMetadataAndFollowCommittedCounts) {
	auto options = backendOptions(); options.open_mode = OpenMode::OPEN_CREATE_NEW_FILE;
	auto backend = RelocatableStorageBackend::initializeNew(options); backend->open();
	const auto nodes = backend->createLogicalNodes({{{"Rare", "Common"}, {}}, {{"Common"}, {}}, {{"Common"}, {}}});
	const std::vector<std::string> rare{"Rare"}, common{"Common"}, both{"Common", "Rare"}, unknown{"Unknown"};
	EXPECT_EQ(backend->estimateLogicalNodeCardinality(rare), 1U);
	EXPECT_EQ(backend->estimateLogicalNodeCardinality(common), 3U);
	EXPECT_EQ(backend->estimateLogicalNodeCardinality(both), 1U);
	EXPECT_EQ(backend->estimateLogicalNodeCardinality({}), 3U);
	EXPECT_EQ(backend->estimateLogicalNodeCardinality(unknown), std::nullopt);
	backend->deleteLogicalNodes({{nodes[1], true, 16U}});
	EXPECT_EQ(backend->estimateLogicalNodeCardinality(common), 2U);
	EXPECT_EQ(backend->estimateLogicalNodeCardinality(rare), 1U);
	backend->close();
}

TEST_F(RelocatableStorageBackendTest, LabelPlanningEstimatesRejectPartialStatisticsAndRecoverAfterRebuild) {
	auto options = backendOptions(); options.open_mode = OpenMode::OPEN_CREATE_NEW_FILE;
	auto backend = RelocatableStorageBackend::initializeNew(options); backend->open();
	const auto nodes = backend->createLogicalNodes({{{"Person"}, {}}});
	backend->close();
	{ TokenStatisticsView view(data_path_); view.seedPartial(); }
	backend->open();
	const std::vector<std::string> person{"Person"}, invalid{""};
	EXPECT_EQ(backend->estimateLogicalNodeCardinality(person), std::nullopt);
	const std::vector<std::vector<std::string>> groups{{"Person"}, {}};
	const auto partial = backend->readLogicalPlanningStatistics(groups);
	ASSERT_TRUE(partial.graph); EXPECT_EQ(partial.graph->node_count, 1U);
	EXPECT_EQ(partial.node_counts, (std::vector<std::optional<uint64_t>>{std::nullopt, 1U}));
	EXPECT_EQ(backend->estimateLogicalNodeCardinality({}), 1U);
	EXPECT_EQ(backend->estimateLogicalNodeCardinality(invalid), std::nullopt);
	backend->rebuildLogicalTokenCardinalityStatistics();
	EXPECT_EQ(backend->estimateLogicalNodeCardinality(person), 1U);
	backend->deleteLogicalNodes({{nodes.front(), true, 16U}});
	EXPECT_EQ(backend->estimateLogicalNodeCardinality(person), 0U);
	backend->close();
	backend->open();
	EXPECT_EQ(backend->estimateLogicalNodeCardinality(person), 0U);
	backend->close();
}

TEST_F(RelocatableStorageBackendTest, PlanningStatisticsBatchPreservesInputOrderAndFreshCommittedCounts) {
 auto options = backendOptions(); options.open_mode = OpenMode::OPEN_CREATE_NEW_FILE;
 auto backend = RelocatableStorageBackend::initializeNew(options); backend->open();
 const auto nodes = backend->createLogicalNodes({{{"Rare", "Common"}, {}}, {{"Common"}, {}}});
 const std::vector<std::vector<std::string>> requests{{"Common"}, {"Rare"}, {"Unknown"}, {}, {""}, {"Common", "Rare"}};
 const auto first = backend->readLogicalPlanningStatistics(requests);
 ASSERT_TRUE(first.graph); EXPECT_EQ(first.graph->node_count, 2U); EXPECT_EQ(first.graph->edge_count, 0U);
 const std::vector<std::optional<uint64_t>> expected{2U, 1U, std::nullopt, 2U, std::nullopt, 1U};
 EXPECT_EQ(first.node_counts, expected);
 backend->deleteLogicalNodes({{nodes.front(), true, 16U}});
 const auto next = backend->readLogicalPlanningStatistics(requests);
 ASSERT_TRUE(next.graph); EXPECT_EQ(next.graph->node_count, 1U);
 EXPECT_EQ(next.node_counts, (std::vector<std::optional<uint64_t>>{1U, 0U, std::nullopt, 1U, std::nullopt, 0U}));
 EXPECT_EQ(first.node_counts, expected);
 backend->close(); backend->open();
 EXPECT_EQ(backend->readLogicalPlanningStatistics(requests).node_counts, next.node_counts);
 backend->close();
}

TEST_F(RelocatableStorageBackendTest, PlanningStatisticsBatchRetainsUnrelatedCostsWhenOneCountIsCorrupt) {
 auto options = backendOptions(); options.open_mode = OpenMode::OPEN_CREATE_NEW_FILE;
 auto backend = RelocatableStorageBackend::initializeNew(options); backend->open();
 static_cast<void>(backend->createLogicalNodes({{{"Rare", "Common"}, {}}, {{"Common"}, {}}}));
 backend->close();
 {
  TokenStatisticsView view(data_path_);
  auto snapshot = view.metadata->openSnapshot();
  const auto token = view.catalog.findSchemaToken(snapshot, SystemCatalogSchemaTokenKind::SCSTK_LABEL, "Rare");
  ASSERT_TRUE(token); snapshot.release();
  const uint64_t first = view.io->fileSize() / RSS_SEGMENT_SIZE + 8U;
  NativeGraphWritePlannerOptions plannerOptions;
  plannerOptions.first_slot = PhysicalSlotId(first); plannerOptions.last_slot = PhysicalSlotId(first + 64U);
  plannerOptions.entity_pool_owner_id = 0x223456U;
  plannerOptions.property_roots = {PhysicalSlotId(first), PhysicalSlotId(first + 64U), 0x223457U};
  NativeGraphWritePlanner planner(*view.metadata, view.entities, view.labels, view.properties, view.pages, plannerOptions);
  auto tx = planner.beginTransaction();
  tx.stageScalarIndexUpsert(view.pages, RSS_SYSTEM_CATALOG_INDEX_ID,
    SystemCatalogTokenCardinalityCodec::countKey(SystemCatalogSchemaTokenKind::SCSTK_LABEL, *token), 0U);
  ASSERT_TRUE(tx.commit().committed()); view.io->sync();
 }
 backend->open();
 const std::vector<std::vector<std::string>> requests{{"Rare"}, {"Common"}, {}};
 const auto result = backend->readLogicalPlanningStatistics(requests);
 ASSERT_TRUE(result.graph); EXPECT_EQ(result.graph->node_count, 2U);
 EXPECT_EQ(result.node_counts, (std::vector<std::optional<uint64_t>>{std::nullopt, 2U, 2U}));
 const std::vector<std::string> rare{"Rare"};
 EXPECT_THROW(static_cast<void>(backend->estimateLogicalNodeCardinality(rare)), FormatException);
 backend->close();
}

TEST_F(RelocatableStorageBackendTest, PlanningStatisticsBatchRetainsUnrelatedCostsWhenOneNameIsCorrupt) {
 auto options = backendOptions(); options.open_mode = OpenMode::OPEN_CREATE_NEW_FILE;
 auto backend = RelocatableStorageBackend::initializeNew(options); backend->open();
 static_cast<void>(backend->createLogicalNodes({{{"Rare", "Common"}, {}}, {{"Common"}, {}}}));
 backend->close();
 {
  TokenStatisticsView view(data_path_);
  auto snapshot = view.metadata->openSnapshot();
  const auto token = view.catalog.findSchemaToken(snapshot, SystemCatalogSchemaTokenKind::SCSTK_LABEL, "Rare");
  ASSERT_TRUE(token); snapshot.release();
  const uint64_t first = view.io->fileSize() / RSS_SEGMENT_SIZE + 8U;
  NativeGraphWritePlannerOptions plannerOptions;
  plannerOptions.first_slot = PhysicalSlotId(first); plannerOptions.last_slot = PhysicalSlotId(first + 64U);
  plannerOptions.entity_pool_owner_id = 0x223456U;
  plannerOptions.property_roots = {PhysicalSlotId(first), PhysicalSlotId(first + 64U), 0x223457U};
  NativeGraphWritePlanner planner(*view.metadata, view.entities, view.labels, view.properties, view.pages, plannerOptions);
  auto tx = planner.beginTransaction();
  tx.stageScalarIndexUpsert(view.pages, RSS_SYSTEM_CATALOG_INDEX_ID,
    SystemCatalogTokenKeyCodec::schemaTokenKey(SystemCatalogSchemaTokenKind::SCSTK_LABEL, *token, "Wrong"), *token);
  ASSERT_TRUE(tx.commit().committed()); view.io->sync();
 }
 backend->open();
 const std::vector<std::vector<std::string>> requests{{"Rare"}, {"Common"}, {}};
 const auto result = backend->readLogicalPlanningStatistics(requests);
 ASSERT_TRUE(result.graph); EXPECT_EQ(result.graph->node_count, 2U);
 EXPECT_EQ(result.node_counts, (std::vector<std::optional<uint64_t>>{std::nullopt, 2U, 2U}));
 const std::vector<std::string> rare{"Rare"};
 EXPECT_THROW(static_cast<void>(backend->estimateLogicalNodeCardinality(rare)), FormatException);
 backend->close();
}

TEST(NativeMembershipAccessCostTest, UnknownOrInconsistentCountsRetainAuthenticatedCandidates) {
 for (bool edges : {false, true}) {
  EXPECT_TRUE(NativeMembershipAccessCost::preferCandidates(0U, 1U, 100U, edges));
  EXPECT_TRUE(NativeMembershipAccessCost::preferCandidates(100U, 0U, 100U, edges));
  EXPECT_TRUE(NativeMembershipAccessCost::preferCandidates(100U, 101U, 100U, edges));
  EXPECT_TRUE(NativeMembershipAccessCost::preferCandidates(101U, 1U, 100U, edges));
 }
}

TEST(NativeMembershipAccessCostTest, KnownDenseCountsChooseScanOnlyWithinCallerBudget) {
 for (bool edges : {false, true}) {
  for (size_t total : {1024U, 8192U, 65536U}) {
   EXPECT_FALSE(NativeMembershipAccessCost::preferCandidates(total, total, total, edges));
   EXPECT_TRUE(NativeMembershipAccessCost::preferCandidates(total, total, total-1U, edges));
  }
 }
}

TEST(NativeMembershipAccessCostTest, IncreasingCandidateCountCannotReenterCandidatePath) {
 for (bool edges : {false, true}) {
  for (size_t total : {1024U, 8192U, 65536U}) {
   bool choseScan = false;
   for (size_t candidates = 1; candidates <= total; ++candidates) {
    const bool selected = NativeMembershipAccessCost::preferCandidates(total, candidates, total, edges);
    if (choseScan) ASSERT_FALSE(selected);
    choseScan = choseScan || !selected;
   }
   EXPECT_TRUE(choseScan);
  }
 }
}
