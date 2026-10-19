/**
 * @file test_RelocatableStorageBackend.cpp
 * @brief Integration tests for the format-safe relocatable logical read backend.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/
#include "storage/relocatable/backend/RelocatableStorageBackendTestFixture.hpp"

#include <atomic>
#include <cerrno>
#include <iterator>
#include <limits>

#include "graph/storage/relocatable/vector/VectorAdjacency.hpp"
#include "graph/storage/relocatable/vector/VectorDeltaMapping.hpp"
#include "graph/storage/relocatable/entity/NativeStreamedGraphPublicationPlan.hpp"
#include "graph/storage/relocatable/index/NativeBulkRun.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

using namespace relocatable_storage_backend_test;

TEST_F(RelocatableStorageBackendTest, WritableOpenReclaimsInterruptedSingleFileBulkScratchBeforeReadersExist) {
	materializeNativeDatabase();
	SegmentId scratchSegment;
	{
		auto data = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
		ASSERT_TRUE(data->is_open());
		auto io = std::make_shared<StorageIO>(data, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		auto metadata = RelocatableMetadataStore::open(io);
		PhysicalSegmentAllocator physical(io, *metadata);
		NativeBulkRunStore store(physical, *metadata,
									 {PhysicalSlotId(40U), PhysicalSlotId(40U), RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES});
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const LogSequenceNumber checkpoint(snapshot.superblock().checkpoint_lsn.value() + 1U);
		snapshot.release();
		const NativeBulkRunFragment fragment{{{{std::byte{0x10U}}, 1U}}};
		scratchSegment = store.write(71U, fragment, checkpoint).segment_id;
		metadata.reset();
		io->sync();
		io.reset();
		data->close();
	}

	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	ASSERT_NO_THROW(backend.open());
	ASSERT_NO_THROW(backend.close());

	auto data = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(data->is_open());
	auto io = std::make_shared<StorageIO>(data, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(metadata->findCatalog(snapshot, scratchSegment).state, SegmentState::SGS_OBSOLETE);
	EXPECT_FALSE(metadata->isPhysicalSlotAllocated(snapshot, PhysicalSlotId(40U)));
}

TEST_F(RelocatableStorageBackendTest, ReadsCanonicalLogicalRecordsOnlyFromNativeEntityPages) {
	const NativeGraphFixture fixture = materializeNativeGraphFixture();
	RelocatableStorageBackend backend(backendOptions());
	ASSERT_NO_THROW(backend.open());
	RelocatableReadSnapshot snapshot = backend.openSnapshot();
	const LogicalGraphNode person = backend.readNode(snapshot, fixture.person_node_id);
	EXPECT_EQ(person.node_id, fixture.person_node_id);
	EXPECT_EQ(person.labels, (std::vector<std::string>{"Person"}));
	EXPECT_EQ(person.properties,
			  (std::vector<LogicalGraphPropertyEntry>{{"alpha", PropertyValue(int64_t(42))},
																										 {"beta", PropertyValue(std::string("value"))}}));
	const LogicalGraphEdge knows = backend.readEdge(snapshot, fixture.knows_edge_id);
	EXPECT_EQ(knows.edge_id, fixture.knows_edge_id);
	EXPECT_EQ(knows.source_node_id, fixture.person_node_id);
	EXPECT_EQ(knows.target_node_id, fixture.person_node_id);
	EXPECT_EQ(knows.type, "KNOWS");
	EXPECT_EQ(knows.properties, (std::vector<LogicalGraphPropertyEntry>{{"alpha", PropertyValue(int64_t(7))}}));
	std::vector<uint64_t> nodeIds;
	backend.visitNodes(snapshot, 2U, [&nodeIds](const LogicalGraphNode &node) { nodeIds.push_back(node.node_id); });
	EXPECT_EQ(nodeIds, (std::vector<uint64_t>{fixture.person_node_id, fixture.unlabeled_node_id}));
	std::vector<uint64_t> edgeIds;
	backend.visitEdges(snapshot, 2U, [&edgeIds](const LogicalGraphEdge &edge) { edgeIds.push_back(edge.edge_id); });
	EXPECT_EQ(edgeIds, (std::vector<uint64_t>{fixture.knows_edge_id, fixture.mentions_edge_id}));
	std::vector<std::string> logicalNodeLabels;
	backend.visitNodes(snapshot, 2U, [&logicalNodeLabels](const LogicalGraphNode &node) {
		logicalNodeLabels.insert(logicalNodeLabels.end(), node.labels.begin(), node.labels.end());
	});
	EXPECT_EQ(logicalNodeLabels, (std::vector<std::string>{"Person"}));
	std::vector<std::string> logicalEdgeTypes;
	backend.visitEdges(snapshot, 2U, [&logicalEdgeTypes](const LogicalGraphEdge &edge) {
		logicalEdgeTypes.push_back(edge.type);
	});
	EXPECT_EQ(logicalEdgeTypes, (std::vector<std::string>{"KNOWS", "MENTIONS"}));
	std::vector<std::string> logicalOutgoingTypes;
	backend.visitAdjacentEdges(snapshot, fixture.person_node_id, LogicalGraphReadDirection::LGRD_OUTGOING, 2U,
																	[&logicalOutgoingTypes](const LogicalGraphEdge &edge) {
		logicalOutgoingTypes.push_back(edge.type);
	});
	EXPECT_EQ(logicalOutgoingTypes, (std::vector<std::string>{"KNOWS", "MENTIONS"}));
	std::vector<std::string> logicalIncomingTypes;
	backend.visitAdjacentEdges(snapshot, fixture.person_node_id, LogicalGraphReadDirection::LGRD_INCOMING, 2U,
																   [&logicalIncomingTypes](const LogicalGraphEdge &edge) {
		logicalIncomingTypes.push_back(edge.type);
	});
	EXPECT_EQ(logicalIncomingTypes, (std::vector<std::string>{"KNOWS", "MENTIONS"}));
	EXPECT_THROW(backend.visitNodes(snapshot, 2U, std::function<void(const LogicalGraphNode &)>{}),
				 FormatException);
	EXPECT_THROW(backend.visitEdges(snapshot, 2U, std::function<void(const LogicalGraphEdge &)>{}),
				 FormatException);
	EXPECT_THROW(
			backend.visitAdjacentEdges(snapshot, fixture.person_node_id, LogicalGraphReadDirection::LGRD_OUTGOING, 2U,
																	  std::function<void(const LogicalGraphEdge &)>{}),
			FormatException);
	EXPECT_THROW(
			backend.visitAdjacentEdges(snapshot, fixture.person_node_id, LogicalGraphReadDirection::LGRD_INCOMING, 2U,
																	  std::function<void(const LogicalGraphEdge &)>{}),
			FormatException);
	const std::optional<uint32_t> alphaToken = backend.findPropertyKeyToken(snapshot, "alpha");
	ASSERT_TRUE(alphaToken.has_value());
	EXPECT_EQ(backend.findPropertyKey(snapshot, *alphaToken), std::optional<std::string>("alpha"));
	EXPECT_EQ(backend.findPropertyKeyToken(snapshot, "missing"), std::nullopt);
	const std::optional<uint64_t> personToken = backend.findSchemaTokenId(snapshot, "Person");
	ASSERT_TRUE(personToken.has_value());
	EXPECT_EQ(backend.findSchemaToken(snapshot, *personToken),
			  std::optional<TokenDefinition>({*personToken, "Person"}));
	EXPECT_EQ(backend.findSchemaTokenId(snapshot, "unknown"), std::nullopt);
	std::unique_ptr<LogicalGraphReadSnapshot> logicalSnapshot = backend.openLogicalGraphReadSnapshot();
	const LogicalGraphNode logicalPerson = logicalSnapshot->readNode(fixture.person_node_id);
	EXPECT_EQ(logicalPerson.labels, (std::vector<std::string>{"Person"}));
	EXPECT_EQ(logicalPerson.properties,
			  (std::vector<LogicalGraphPropertyEntry>{{"alpha", PropertyValue(int64_t(42))},
																			 {"beta", PropertyValue(std::string("value"))}}));
	const LogicalGraphEdge logicalKnows = logicalSnapshot->readEdge(fixture.knows_edge_id);
	EXPECT_EQ(logicalKnows.type, "KNOWS");
	EXPECT_EQ(logicalKnows.properties,
			  (std::vector<LogicalGraphPropertyEntry>{{"alpha", PropertyValue(int64_t(7))}}));
	logicalSnapshot.reset();
	snapshot.release();
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, ExposesFormatIndependentBoundedLogicalGraphReadSnapshot) {
	const NativeGraphFixture fixture = materializeNativeGraphFixture();
	RelocatableStorageBackend backend(backendOptions());
	ASSERT_NO_THROW(backend.open());
	LogicalGraphReadBackend &logicalBackend = backend;
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = logicalBackend.openLogicalGraphReadSnapshot();
	ASSERT_NE(snapshot, nullptr);
	EXPECT_EQ(snapshot->readNode(fixture.person_node_id),
			  (LogicalGraphNode{fixture.person_node_id,
									{"Person"},
									{{"alpha", PropertyValue(int64_t(42))},
									 {"beta", PropertyValue(std::string("value"))}}}));
	EXPECT_EQ(snapshot->readNode(fixture.unlabeled_node_id),
				  (LogicalGraphNode{fixture.unlabeled_node_id, {}}));
	EXPECT_EQ(snapshot->readEdge(fixture.knows_edge_id),
				  (LogicalGraphEdge{fixture.knows_edge_id,
									fixture.person_node_id,
									fixture.person_node_id,
									"KNOWS",
									{{"alpha", PropertyValue(int64_t(7))}}}));

	std::vector<uint64_t> nodeIds;
	snapshot->visitNodes(2U, [&nodeIds](const LogicalGraphNode &node) {
		nodeIds.push_back(node.node_id);
		return true;
	});
	EXPECT_EQ(nodeIds, (std::vector<uint64_t>{fixture.person_node_id, fixture.unlabeled_node_id}));
	std::vector<uint64_t> limitedNodeIds;
	snapshot->visitNodes(2U, [&limitedNodeIds](const LogicalGraphNode &node) {
		limitedNodeIds.push_back(node.node_id);
		return false;
	});
	EXPECT_EQ(limitedNodeIds, (std::vector<uint64_t>{fixture.person_node_id}));
	const LogicalGraphNodePage firstNodePage = snapshot->scanNodesAfter(0U, 1U, 8U);
	ASSERT_EQ(firstNodePage.nodes.size(), 1U);
	EXPECT_EQ(firstNodePage.nodes.front().node_id, fixture.person_node_id);
	EXPECT_TRUE(firstNodePage.may_have_more);
	EXPECT_LE(firstNodePage.metadata_pages_visited, 8U);
	const LogicalGraphNodePage secondNodePage = snapshot->scanNodesAfter(fixture.person_node_id, 1U, 8U);
	ASSERT_EQ(secondNodePage.nodes.size(), 1U);
	EXPECT_EQ(secondNodePage.nodes.front().node_id, fixture.unlabeled_node_id);
	EXPECT_TRUE(secondNodePage.may_have_more);
	EXPECT_LE(secondNodePage.metadata_pages_visited, 8U);
	const LogicalGraphNodePage exhaustedNodePage = snapshot->scanNodesAfter(fixture.unlabeled_node_id, 1U, 8U);
	EXPECT_TRUE(exhaustedNodePage.nodes.empty());
	EXPECT_FALSE(exhaustedNodePage.may_have_more);
	EXPECT_LE(exhaustedNodePage.metadata_pages_visited, 8U);
	EXPECT_THROW(static_cast<void>(snapshot->scanNodesAfter(0U, 1U, 0U)), FormatException);
	EXPECT_THROW(static_cast<void>(snapshot->scanNodesAfter(0U, 0U, 8U)), FormatException);
	EXPECT_THROW(static_cast<void>(snapshot->scanNodesAfter(0U, 1U, 1U)), FormatException);
	std::vector<std::string> edgeTypes;
	snapshot->visitEdges(2U, [&edgeTypes](const LogicalGraphEdge &edge) {
		edgeTypes.push_back(edge.type);
		return true;
	});
	EXPECT_EQ(edgeTypes, (std::vector<std::string>{"KNOWS", "MENTIONS"}));
	std::vector<std::string> outgoingTypes;
	snapshot->visitAdjacentEdges(fixture.person_node_id, LogicalGraphReadDirection::LGRD_OUTGOING, 2U,
									 [&outgoingTypes](const LogicalGraphEdge &edge) {
										 outgoingTypes.push_back(edge.type);
										 return true;
									 });
	const std::vector<std::string> traversalOrder = outgoingTypes;
	std::ranges::sort(outgoingTypes);
	EXPECT_EQ(outgoingTypes, (std::vector<std::string>{"KNOWS", "MENTIONS"}));
	std::vector<std::string> limitedOutgoingTypes;
	snapshot->visitAdjacentEdges(fixture.person_node_id, LogicalGraphReadDirection::LGRD_OUTGOING, 2U,
								 [&limitedOutgoingTypes](const LogicalGraphEdge &edge) {
									 limitedOutgoingTypes.push_back(edge.type);
									 return false;
								 });
	ASSERT_EQ(limitedOutgoingTypes.size(), 1U);
	EXPECT_EQ(limitedOutgoingTypes.front(), traversalOrder.front());
	std::vector<std::string> incomingTypes;
	snapshot->visitAdjacentEdges(fixture.person_node_id, LogicalGraphReadDirection::LGRD_INCOMING, 2U,
								 [&incomingTypes](const LogicalGraphEdge &edge) {
									 incomingTypes.push_back(edge.type);
									 return true;
								 });
	std::ranges::sort(incomingTypes);
	EXPECT_EQ(incomingTypes, (std::vector<std::string>{"KNOWS", "MENTIONS"}));
	EXPECT_THROW(snapshot->visitNodes(2U, std::function<bool(const LogicalGraphNode &)>{}), FormatException);
	EXPECT_THROW(snapshot->visitEdges(2U, std::function<bool(const LogicalGraphEdge &)>{}), FormatException);
	EXPECT_THROW(snapshot->visitAdjacentEdges(fixture.person_node_id, LogicalGraphReadDirection::LGRD_OUTGOING, 2U,
											  std::function<bool(const LogicalGraphEdge &)>{}),
				 FormatException);
	EXPECT_THROW(snapshot->visitAdjacentEdges(fixture.person_node_id, static_cast<LogicalGraphReadDirection>(0), 2U,
											  [](const LogicalGraphEdge &) { return true; }),
				 FormatException);
	EXPECT_THROW(backend.close(), std::logic_error);
	snapshot.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, LogicalGraphReadResumesAdjacencyWithoutRestartingTheChain) {
	const NativeGraphFixture fixture = materializeNativeGraphFixture();
	RelocatableStorageBackend backend(backendOptions());
	ASSERT_NO_THROW(backend.open());
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(snapshot, nullptr);

	const LogicalGraphAdjacencyPage first =
			snapshot->scanAdjacentEdges(fixture.person_node_id, LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 1U);
	ASSERT_EQ(first.edges.size(), 1U);
	EXPECT_TRUE(first.edges.front().edge_id == fixture.knows_edge_id ||
				fixture.mentions_edge_id == first.edges.front().edge_id);
	ASSERT_TRUE(first.continuation.has_value());
	EXPECT_EQ(first.continuation->previous_edge_id, first.edges.front().edge_id);
	EXPECT_NE(first.continuation->next_edge_id, first.edges.front().edge_id);
	const LogicalGraphAdjacencyPage second =
			snapshot->scanAdjacentEdges(fixture.person_node_id, LogicalGraphReadDirection::LGRD_OUTGOING,
															first.continuation, 1U);
	ASSERT_EQ(second.edges.size(), 1U);
	EXPECT_EQ(second.edges.front().edge_id, first.continuation->next_edge_id);
	EXPECT_NE(second.edges.front().edge_id, first.edges.front().edge_id);
	EXPECT_FALSE(second.continuation.has_value());

	const LogicalGraphAdjacencyPage incoming =
			snapshot->scanAdjacentEdges(fixture.person_node_id, LogicalGraphReadDirection::LGRD_INCOMING, std::nullopt,
															2U);
	EXPECT_EQ(incoming.edges.size(), 2U);
	EXPECT_FALSE(incoming.continuation.has_value());
	EXPECT_THROW(static_cast<void>(snapshot->scanAdjacentEdges(
															 fixture.person_node_id, LogicalGraphReadDirection::LGRD_OUTGOING,
															 LogicalGraphAdjacencyContinuation{first.edges.front().edge_id, 0U}, 1U)),
				 FormatException);
	EXPECT_THROW(static_cast<void>(snapshot->scanAdjacentEdges(
															 fixture.person_node_id, LogicalGraphReadDirection::LGRD_OUTGOING,
															 LogicalGraphAdjacencyContinuation{second.edges.front().edge_id,
																											 first.edges.front().edge_id},
															 1U)),
				 FormatException);
	EXPECT_THROW(static_cast<void>(
						 snapshot->scanAdjacentEdges(fixture.person_node_id, LogicalGraphReadDirection::LGRD_OUTGOING,
																				 std::nullopt, 0U)),
				 FormatException);
	EXPECT_THROW(static_cast<void>(snapshot->scanAdjacentEdges(fixture.person_node_id,
																				 static_cast<LogicalGraphReadDirection>(0),
																				   std::nullopt, 1U)),
				 FormatException);

	snapshot.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, DatabaseFactoryExposesTheBoundedRelocatableLogicalBoundary) {
	const NativeGraphFixture fixture = materializeNativeGraphFixture();

	graph::Database internalDatabase(data_path_.string());
	internalDatabase.open();
	ASSERT_NE(internalDatabase.getStorageBackend(), nullptr);
	EXPECT_EQ(internalDatabase.getStorageBackend()->formatKind(), StorageFormatKind::SFM_RELOCATABLE);
	EXPECT_TRUE(internalDatabase.usesLogicalBackend());
	// Query execution is available through the bounded logical adapters; the
	// physical StorageBackend interface remains independent of legacy storage.
	EXPECT_TRUE(internalDatabase.getStorageBackend()->capabilities().supports_query_execution);
	EXPECT_FALSE(internalDatabase.getStorageBackend()->capabilities().supports_read_write_transactions);
	// The Database facade must dispatch lifecycle operations through the relocatable
	// backend rather than assuming a retired physical storage handle exists.
	EXPECT_NO_THROW(internalDatabase.flush());

	auto engine = internalDatabase.getQueryEngine();
	ASSERT_NE(engine, nullptr);
	const graph::query::QueryResult internalResult = engine->execute("MATCH (n:Person) RETURN n");
	ASSERT_EQ(internalResult.rowCount(), 1U);
	const auto &logicalNode = internalResult.getRows().front().at("n").asLogicalNode();
	EXPECT_EQ(logicalNode.node_id, fixture.person_node_id);
	EXPECT_EQ(logicalNode.labels, (std::vector<std::string>{"Person"}));
	EXPECT_EQ(logicalNode.properties.at("alpha"), PropertyValue(42));
	ASSERT_NE(internalDatabase.getThreadPool(), nullptr);
	// A logical backend owns no retired physical storage or vector-index subsystem.
	// Resizing after query-engine initialization must preserve that separation.
	EXPECT_NO_THROW(internalDatabase.setThreadPoolSize(1U));
	{
		auto transaction = internalDatabase.beginTransaction();
		EXPECT_TRUE(transaction.isActive());
		transaction.commit();
	}
	{
		auto transaction = internalDatabase.beginBulkTransaction();
		EXPECT_TRUE(transaction.isActive());
		transaction.commit();
	}
	{
		auto transaction = internalDatabase.beginReadOnlyTransaction();
		EXPECT_TRUE(transaction.isReadOnly());
		transaction.commit();
	}
	internalDatabase.close();

	zyx::Database publicDatabase(data_path_.string());
	publicDatabase.open();
	{
		zyx::Result publicResult = publicDatabase.execute("MATCH (n:Person) RETURN n");
		ASSERT_TRUE(publicResult.isSuccess());
		ASSERT_TRUE(publicResult.hasNext());
		publicResult.next();
		const auto node = std::get<std::shared_ptr<zyx::Node>>(publicResult.get("n"));
		ASSERT_NE(node, nullptr);
		EXPECT_EQ(node->id, static_cast<int64_t>(fixture.person_node_id));
		EXPECT_EQ(node->labels, (std::vector<std::string>{"Person"}));
		EXPECT_EQ(std::get<int64_t>(node->properties.at("alpha")), 42);
		EXPECT_EQ(std::get<int64_t>(publicResult.get("alpha")), 42);
		EXPECT_TRUE(std::holds_alternative<std::monostate>(publicResult.get("missing")));
		zyx::Result allNodes = publicDatabase.execute("MATCH (n) RETURN n");
		ASSERT_TRUE(allNodes.isSuccess());
		ASSERT_TRUE(allNodes.hasNext());
		allNodes.next();
		ASSERT_TRUE(allNodes.hasNext());
		allNodes.next();
		const auto unlabeledNode = std::get<std::shared_ptr<zyx::Node>>(allNodes.get("n"));
		ASSERT_NE(unlabeledNode, nullptr);
		EXPECT_EQ(unlabeledNode->id, static_cast<int64_t>(fixture.unlabeled_node_id));
		EXPECT_TRUE(unlabeledNode->labels.empty());

		zyx::Result publicEdgeResult = publicDatabase.execute("MATCH (n:Person)-[r:KNOWS]->(m) RETURN r");
		ASSERT_TRUE(publicEdgeResult.isSuccess()) << publicEdgeResult.getError();
		ASSERT_TRUE(publicEdgeResult.hasNext());
		publicEdgeResult.next();
		const auto edge = std::get<std::shared_ptr<zyx::Edge>>(publicEdgeResult.get("r"));
		ASSERT_NE(edge, nullptr);
		EXPECT_EQ(edge->id, static_cast<int64_t>(fixture.knows_edge_id));
		EXPECT_EQ(edge->sourceId, static_cast<int64_t>(fixture.person_node_id));
		EXPECT_EQ(edge->targetId, static_cast<int64_t>(fixture.person_node_id));
		EXPECT_EQ(edge->type, "KNOWS");
		EXPECT_EQ(std::get<int64_t>(edge->properties.at("alpha")), 7);
		EXPECT_EQ(std::get<int64_t>(publicEdgeResult.get("alpha")), 7);
		EXPECT_TRUE(std::holds_alternative<std::monostate>(publicEdgeResult.get("missing")));
		EXPECT_TRUE(publicDatabase.execute("CREATE (:Person)").isSuccess());
		EXPECT_TRUE(publicDatabase.execute("BEGIN").isSuccess());
		EXPECT_TRUE(publicDatabase.execute("ROLLBACK").isSuccess());
	}
	publicDatabase.close();

	zyx_driver_db_t *driverDatabase = nullptr;
	ASSERT_EQ(zyx_driver_db_open(data_path_.string().c_str(), &driverDatabase, nullptr), ZYX_DRIVER_OK);
	ASSERT_NE(driverDatabase, nullptr);
	zyx_driver_result_t *driverResult = nullptr;
	ASSERT_EQ(zyx_driver_db_execute(driverDatabase, "MATCH (n:Person) RETURN n", nullptr, &driverResult, nullptr),
			  ZYX_DRIVER_OK);
	ASSERT_NE(driverResult, nullptr);
	ASSERT_EQ(zyx_driver_result_next(driverResult, nullptr), ZYX_DRIVER_ROW);
	int64_t nodeId = 0;
	EXPECT_EQ(zyx_driver_result_get_node_id(driverResult, 0U, &nodeId, nullptr), ZYX_DRIVER_OK);
	EXPECT_EQ(nodeId, static_cast<int64_t>(fixture.person_node_id));
	zyx_driver_result_free(driverResult);

	zyx_driver_txn_t *driverTransaction = nullptr;
	ASSERT_EQ(zyx_driver_txn_begin(driverDatabase, &driverTransaction, nullptr), ZYX_DRIVER_OK);
	ASSERT_NE(driverTransaction, nullptr);
	EXPECT_EQ(zyx_driver_txn_rollback(driverTransaction, nullptr), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_txn_close(driverTransaction, nullptr), ZYX_DRIVER_OK);
	driverTransaction = nullptr;
	ASSERT_EQ(zyx_driver_txn_begin_read_only(driverDatabase, &driverTransaction, nullptr), ZYX_DRIVER_OK);
	ASSERT_NE(driverTransaction, nullptr);
	EXPECT_EQ(zyx_driver_txn_commit(driverTransaction, nullptr), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_txn_close(driverTransaction, nullptr), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_db_close(driverDatabase, nullptr), ZYX_DRIVER_OK);
}

TEST_F(RelocatableStorageBackendTest, DatabaseFactoryFailsClosedForIncompleteAndUnknownFiles) {
	{
		std::ofstream unknown(data_path_, std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(unknown.is_open());
		unknown.put('x');
		unknown.close();
		graph::Database database(data_path_.string());
		EXPECT_THROW(database.open(), std::runtime_error);
		EXPECT_FALSE(database.openIfExists());
	}
	{
		std::ofstream empty(data_path_, std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(empty.is_open());
		empty.close();
		graph::Database database(data_path_.string());
		EXPECT_THROW(database.open(), std::runtime_error);
	}
	{
		std::ofstream existing(data_path_, std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(existing.is_open());
		existing.put('x');
		existing.close();
		EXPECT_THROW(static_cast<void>(
							 StorageBackendFactory::create(data_path_.string(), 4096U, OpenMode::OPEN_CREATE_NEW_FILE)),
					 std::runtime_error);
		ASSERT_TRUE(std::filesystem::remove(data_path_));
		const StorageBackendSelection creation =
				StorageBackendFactory::create(data_path_.string(), 4096U, OpenMode::OPEN_CREATE_NEW_FILE);
		ASSERT_NE(creation.backend, nullptr);
		ASSERT_NE(creation.relocatable_storage, nullptr);
		EXPECT_EQ(creation.backend->formatKind(), StorageFormatKind::SFM_RELOCATABLE);
		EXPECT_NO_THROW(creation.backend->open());
		EXPECT_NO_THROW(creation.backend->close());
	}
	{
		const std::filesystem::path directory = data_path_.string() + ".directory";
		ASSERT_TRUE(std::filesystem::create_directory(directory));
		EXPECT_THROW(static_cast<void>(StorageBackendFactory::create(directory.string(), 4096U,
																	 OpenMode::OPEN_CREATE_OR_OPEN_FILE)),
					 std::runtime_error);
		std::error_code error;
		std::filesystem::remove(directory, error);
		EXPECT_FALSE(error);
	}
}

TEST_F(RelocatableStorageBackendTest, NativeDatabaseCreationPublishesLogicalWritesAndReopens) {
	ASSERT_FALSE(std::filesystem::exists(data_path_));
	zyx::Database database(data_path_.string());
	ASSERT_NO_THROW(database.open());

	const int64_t first = database.createNode("Person", {{"name", std::string("Ada")}});
	const int64_t second = database.createNode("Person", {{"name", std::string("Bob")}});
	const int64_t edge = database.createEdge(first, second, "KNOWS", {{"since", int64_t(2026)}});
	EXPECT_GT(first, 0);
	EXPECT_GT(second, first);
	EXPECT_GT(edge, 0);

	zyx::Result result = database.execute(
			"MATCH (source:Person {name: 'Ada'})-[r:KNOWS]->(target:Person {name: 'Bob'}) RETURN r.since AS since");
	ASSERT_TRUE(result.isSuccess()) << result.getError();
	ASSERT_TRUE(result.hasNext());
	result.next();
	EXPECT_EQ(std::get<int64_t>(result.get("since")), 2026);
	EXPECT_FALSE(result.hasNext());
	ASSERT_NO_THROW(database.save());
	ASSERT_NO_THROW(database.close());

	zyx::Database reopened(data_path_.string());
	ASSERT_NO_THROW(reopened.open());
	result = reopened.execute("MATCH (n:Person) RETURN n.name AS name ORDER BY name");
	ASSERT_TRUE(result.isSuccess());
	ASSERT_TRUE(result.hasNext());
	result.next();
	EXPECT_EQ(std::get<std::string>(result.get("name")), "Ada");
	ASSERT_TRUE(result.hasNext());
	result.next();
	EXPECT_EQ(std::get<std::string>(result.get("name")), "Bob");
	EXPECT_FALSE(result.hasNext());
	EXPECT_NO_THROW(reopened.close());
}

TEST_F(RelocatableStorageBackendTest, NativeInitializationRejectsUnsafeModesAndPathsWithoutReplacingFiles) {
	RelocatableStorageBackendOptions nonCreate = backendOptions();
	nonCreate.open_mode = OpenMode::OPEN_EXISTING_FILE;
	EXPECT_THROW(static_cast<void>(RelocatableStorageBackend::initializeNew(std::move(nonCreate))), std::invalid_argument);
	RelocatableStorageBackendOptions undersizedPool = backendOptions();
	undersizedPool.open_mode = OpenMode::OPEN_CREATE_NEW_FILE;
	undersizedPool.buffer_pool_bytes = RSS_BUFFER_POOL_PAGE_BYTES - 1U;
	EXPECT_THROW(static_cast<void>(RelocatableStorageBackend::initializeNew(std::move(undersizedPool))), std::invalid_argument);

	RelocatableStorageBackendOptions emptyPath = backendOptions();
	emptyPath.open_mode = OpenMode::OPEN_CREATE_NEW_FILE;
	emptyPath.data_path.clear();
	EXPECT_THROW(static_cast<void>(RelocatableStorageBackend::initializeNew(std::move(emptyPath))), std::invalid_argument);

	RelocatableStorageBackendOptions absentParent = backendOptions();
	absentParent.open_mode = OpenMode::OPEN_CREATE_NEW_FILE;
	absentParent.data_path = data_path_.parent_path() / "relocatable_missing_parent" / "database.dat";
	EXPECT_THROW(static_cast<void>(RelocatableStorageBackend::initializeNew(std::move(absentParent))), std::runtime_error);

	const std::filesystem::path nonDirectoryParent = data_path_.string() + ".parent";
	{
		std::ofstream parentFile(nonDirectoryParent, std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(parentFile.is_open());
		parentFile << "not a directory";
	}
	RelocatableStorageBackendOptions nonDirectoryParentPath = backendOptions();
	nonDirectoryParentPath.open_mode = OpenMode::OPEN_CREATE_NEW_FILE;
	nonDirectoryParentPath.data_path = nonDirectoryParent / "database.dat";
	EXPECT_THROW(static_cast<void>(RelocatableStorageBackend::initializeNew(std::move(nonDirectoryParentPath))),
			 std::runtime_error);
	std::error_code removeParentError;
	EXPECT_TRUE(std::filesystem::remove(nonDirectoryParent, removeParentError));
	EXPECT_FALSE(removeParentError);

	{
		std::ofstream existing(data_path_, std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(existing.is_open());
		existing << "preserve";
	}
	RelocatableStorageBackendOptions existingPath = backendOptions();
	existingPath.open_mode = OpenMode::OPEN_CREATE_NEW_FILE;
	EXPECT_THROW(static_cast<void>(RelocatableStorageBackend::initializeNew(std::move(existingPath))), std::runtime_error);
	std::ifstream preserved(data_path_, std::ios::binary);
	ASSERT_TRUE(preserved.is_open());
	EXPECT_EQ(std::string(std::istreambuf_iterator<char>(preserved), {}), "preserve");
}

TEST_F(RelocatableStorageBackendTest, InitialBootstrapPublicationFailureRemovesPartialFileAndAllowsRetry) {
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_CREATE_NEW_FILE;
	std::atomic<bool> failedConvergencePublication{false};
	{
		StorageIO::ScopedNativeWriteHookForTesting failConvergenceSuperblock(
				[&failedConvergencePublication](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if ((offset == SuperblockFile::slotOffset(0U) || offset == SuperblockFile::slotOffset(1U)) &&
						byteCount == RSS_PAGE_SIZE && !failedConvergencePublication.load(std::memory_order_relaxed)) {
						const Superblock candidate = SuperblockCodec::decode(
								std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount));
						if (candidate.generation == Generation(2U) && candidate.usesPageDirectoryBootstrap()) {
							failedConvergencePublication.store(true, std::memory_order_relaxed);
#ifdef _WIN32
							SetLastError(ERROR_WRITE_FAULT);
#else
							errno = EIO;
#endif
							return static_cast<pwrite_ssize_t>(-1);
						}
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		EXPECT_THROW(static_cast<void>(RelocatableStorageBackend::initializeNew(std::move(options))), std::system_error);
	}
	EXPECT_TRUE(failedConvergencePublication.load(std::memory_order_relaxed));
	EXPECT_FALSE(std::filesystem::exists(data_path_));

	RelocatableStorageBackendOptions retryOptions = backendOptions();
	retryOptions.open_mode = OpenMode::OPEN_CREATE_NEW_FILE;
	auto retry = RelocatableStorageBackend::initializeNew(std::move(retryOptions));
	ASSERT_NE(retry, nullptr);
	ASSERT_NO_THROW(retry->open());
	EXPECT_NO_THROW(retry->close());
}

TEST_F(RelocatableStorageBackendTest, RejectsReleasedSnapshotsUnsupportedOpenModesAndInvalidFiles) {
	materializeNativeDatabase();
	std::ifstream beforeInput(data_path_, std::ios::binary);
	ASSERT_TRUE(beforeInput.is_open());
	const std::vector<char> bytesBefore{std::istreambuf_iterator<char>(beforeInput), {}};
	RelocatableStorageBackend backend(backendOptions());
	backend.open();
	RelocatableReadSnapshot snapshot = backend.openSnapshot();
	snapshot.release();
	EXPECT_THROW(static_cast<void>(backend.readNode(snapshot, 100)), std::invalid_argument);
	backend.close();
	std::ifstream afterInput(data_path_, std::ios::binary);
	ASSERT_TRUE(afterInput.is_open());
	const std::vector<char> bytesAfter{std::istreambuf_iterator<char>(afterInput), {}};
	EXPECT_EQ(bytesAfter, bytesBefore);

	RelocatableStorageBackendOptions unsupported = backendOptions();
	unsupported.open_mode = OpenMode::OPEN_CREATE_OR_OPEN_FILE;
	RelocatableStorageBackend unsupportedBackend(std::move(unsupported));
	EXPECT_THROW(unsupportedBackend.open(), std::invalid_argument);

	{
		std::ofstream legacy{data_path_, std::ios::binary | std::ios::trunc};
		legacy.put('x');
	}
	RelocatableStorageBackend isolated(backendOptions());
	EXPECT_THROW(isolated.open(), std::runtime_error);

	RelocatableStorageBackendOptions missing = backendOptions();
	missing.data_path = data_path_.string() + ".missing";
	RelocatableStorageBackend missingBackend(std::move(missing));
	EXPECT_THROW(missingBackend.open(), std::runtime_error);

	RelocatableStorageBackendOptions emptyPath = backendOptions();
	emptyPath.data_path.clear();
	RelocatableStorageBackend emptyPathBackend(std::move(emptyPath));
	EXPECT_THROW(emptyPathBackend.open(), std::runtime_error);
}

TEST_F(RelocatableStorageBackendTest, ReadOnlyFlushIsANoOpAndPreservesTheCompleteDatabaseFile) {
	materializeNativeDatabase();
	std::ifstream before(data_path_, std::ios::binary);
	ASSERT_TRUE(before.is_open());
	const std::vector<char> expected{std::istreambuf_iterator<char>(before), {}};

	RelocatableStorageBackend backend(backendOptions());
	backend.open();
	ASSERT_TRUE(backend.isReadOnly());
	EXPECT_NO_THROW(backend.flush());
	backend.close();

	std::ifstream after(data_path_, std::ios::binary);
	ASSERT_TRUE(after.is_open());
	const std::vector<char> actual{std::istreambuf_iterator<char>(after), {}};
	EXPECT_EQ(actual, expected);
}

TEST_F(RelocatableStorageBackendTest, RejectsSnapshotsOwnedByAnotherNativeDatabase) {
	materializeNativeDatabase();
	const auto uuid = boost::uuids::random_generator()();
	const std::filesystem::path foreignPath =
			std::filesystem::temp_directory_path() / ("relocatable_backend_foreign_" + to_string(uuid) + ".dat");
	{
		RelocatableStorageBackendOptions foreignOptions = backendOptions();
		foreignOptions.data_path = foreignPath;
		foreignOptions.open_mode = OpenMode::OPEN_CREATE_NEW_FILE;
		const std::shared_ptr<RelocatableStorageBackend> foreign =
				RelocatableStorageBackend::initializeNew(std::move(foreignOptions));
		foreign->open();
		foreign->close();
	}

	RelocatableStorageBackend local(backendOptions());
	RelocatableStorageBackendOptions foreignReadOptions = backendOptions();
	foreignReadOptions.data_path = foreignPath;
	RelocatableStorageBackend foreign(std::move(foreignReadOptions));
	local.open();
	foreign.open();
	RelocatableReadSnapshot localSnapshot = local.openSnapshot();
	EXPECT_THROW(static_cast<void>(foreign.containsLogicalNode(localSnapshot, 1U)), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(foreign.readNode(localSnapshot, 1U)), std::invalid_argument);
	localSnapshot.release();
	foreign.close();
	local.close();
	std::error_code error;
	std::filesystem::remove(foreignPath, error);
}

TEST_F(RelocatableStorageBackendTest, WritableOpenRejectsEveryUnboundedOrInconsistentWorkLimit) {
	materializeNativeDatabase();
	const auto expectRejected = [this](const auto &configure) {
		RelocatableStorageBackendOptions options = backendOptions();
		options.open_mode = OpenMode::OPEN_EXISTING_FILE;
		configure(options);
		RelocatableStorageBackend backend(std::move(options));
		EXPECT_THROW(backend.open(), std::invalid_argument);
		EXPECT_FALSE(backend.isOpen());
	};
	const size_t unbounded = std::numeric_limits<size_t>::max();

	// A backend cannot construct a page pool without at least one complete raw
	// page; reject this before touching the existing database file.
	expectRejected([](auto &options) { options.buffer_pool_bytes = RSS_BUFFER_POOL_PAGE_BYTES - 1U; });

	expectRejected([](auto &options) { options.maximum_index_definitions = 0U; });
	expectRejected([unbounded](auto &options) { options.maximum_index_definitions = unbounded; });
	expectRejected([](auto &options) { options.maximum_constraint_definitions = 0U; });
	expectRejected([unbounded](auto &options) { options.maximum_constraint_definitions = unbounded; });
	expectRejected([](auto &options) { options.maximum_online_index_entries = 0U; });
	expectRejected([unbounded](auto &options) { options.maximum_online_index_entries = unbounded; });
	expectRejected([](auto &options) { options.maximum_online_index_scan_nodes = 0U; });
	expectRejected([unbounded](auto &options) { options.maximum_online_index_scan_nodes = unbounded; });
	expectRejected([](auto &options) { options.maximum_online_index_pages = 0U; });
	expectRejected([unbounded](auto &options) { options.maximum_online_index_pages = unbounded; });
	expectRejected([](auto &options) { options.online_index_sort_run_bytes = 0U; });
	expectRejected([unbounded](auto &options) { options.online_index_sort_run_bytes = unbounded; });
	expectRejected([](auto &options) { options.maximum_online_index_sort_runs = 1U; });
	expectRejected([unbounded](auto &options) { options.maximum_online_index_sort_runs = unbounded; });
	expectRejected([](auto &options) { options.vector_delta_rebuild_threshold = 0U; });
	expectRejected([unbounded](auto &options) { options.vector_delta_rebuild_threshold = unbounded; });
	expectRejected([](auto &options) {
		options.vector_delta_rebuild_threshold = static_cast<size_t>(std::numeric_limits<uint32_t>::max()) + 1U;
	});
	expectRejected([](auto &options) { options.maximum_vector_delta_overlay_entries = 0U; });
	expectRejected([unbounded](auto &options) { options.maximum_vector_delta_overlay_entries = unbounded; });
	expectRejected([](auto &options) {
		options.maximum_vector_delta_overlay_entries = static_cast<size_t>(std::numeric_limits<uint32_t>::max()) + 1U;
	});
	expectRejected([](auto &options) {
		options.vector_delta_rebuild_threshold = 2U;
		options.maximum_vector_delta_overlay_entries = 1U;
	});
	expectRejected([](auto &options) { options.maximum_vector_delta_pages = 0U; });
	expectRejected([unbounded](auto &options) { options.maximum_vector_delta_pages = unbounded; });
	expectRejected([](auto &options) {
		options.maximum_vector_delta_pages = 1U;
		options.maximum_vector_delta_overlay_entries =
				VectorDeltaMapping::maximumEntriesForPageBudget(1U) + 1U;
	});
	expectRejected([](auto &options) { options.maximum_vector_training_records = 0U; });
	expectRejected([unbounded](auto &options) { options.maximum_vector_training_records = unbounded; });
	expectRejected([](auto &options) { options.maximum_vector_training_pages = 0U; });
	expectRejected([unbounded](auto &options) { options.maximum_vector_training_pages = unbounded; });
	expectRejected([](auto &options) { options.maximum_vector_training_samples = 0U; });
	expectRejected([unbounded](auto &options) { options.maximum_vector_training_samples = unbounded; });
	expectRejected([](auto &options) { options.vector_training_iterations = 0U; });
	expectRejected([](auto &options) { options.maximum_adjacency_read_edges = 0U; });
	expectRejected([unbounded](auto &options) { options.maximum_adjacency_read_edges = unbounded; });
	expectRejected([](auto &options) { options.maximum_vector_adjacency_exact_records = 0U; });
	expectRejected([unbounded](auto &options) { options.maximum_vector_adjacency_exact_records = unbounded; });
	expectRejected([](auto &options) { options.maximum_vector_adjacency_degree = 0U; });
	expectRejected([](auto &options) {
		options.maximum_vector_adjacency_degree = RSS_VECTOR_ADJACENCY_MAX_NEIGHBORS + 1U;
	});
	expectRejected([](auto &options) { options.vector_adjacency_candidate_search_budget = 0U; });
	expectRejected([unbounded](auto &options) { options.vector_adjacency_candidate_search_budget = unbounded; });
	expectRejected([](auto &options) {
		options.vector_adjacency_pruning_alpha = std::numeric_limits<float>::quiet_NaN();
	});
	expectRejected([](auto &options) { options.vector_adjacency_pruning_alpha = 0.5F; });
	expectRejected([](auto &options) { options.maximum_constraint_support_collisions = 0U; });
	expectRejected([unbounded](auto &options) { options.maximum_constraint_support_collisions = unbounded; });
}

TEST_F(RelocatableStorageBackendTest, NativeLogicalMutationPublishesCanonicalMetadata) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	EXPECT_FALSE(backend.capabilities().supports_read_write_transactions);
	const std::vector<uint64_t> created = backend.createLogicalNodes(
			{{{"ONLINE_LABEL"}, {{"online_score", PropertyValue(int64_t(17))}}}});
	ASSERT_EQ(created.size(), 1U);

	RelocatableReadSnapshot snapshot = backend.openSnapshot();
	const std::optional<uint32_t> propertyToken = backend.findPropertyKeyToken(snapshot, "online_score");
	ASSERT_TRUE(propertyToken.has_value());
	EXPECT_EQ(backend.findPropertyKey(snapshot, *propertyToken), std::optional<std::string>("online_score"));
	const std::optional<uint64_t> labelToken = backend.findSchemaTokenId(snapshot, "ONLINE_LABEL");
	ASSERT_TRUE(labelToken.has_value());
	EXPECT_EQ(backend.findSchemaToken(snapshot, *labelToken),
			  std::optional<TokenDefinition>({*labelToken, "ONLINE_LABEL"}));
	std::unique_ptr<LogicalGraphReadSnapshot> logicalSnapshot = backend.openLogicalGraphReadSnapshot();
	EXPECT_EQ(logicalSnapshot->readNode(created.front()).properties,
			  (std::vector<LogicalGraphPropertyEntry>{{"online_score", PropertyValue(int64_t(17))}}));
	logicalSnapshot.reset();
	snapshot.release();
	backend.close();

	RelocatableStorageBackend readOnly(backendOptions());
	readOnly.open();
	EXPECT_THROW(static_cast<void>(readOnly.createLogicalNodes({{{"forbidden"}, {}}})), std::logic_error);
	readOnly.close();
}

TEST_F(RelocatableStorageBackendTest, ReadOnlyBackendRejectsEveryNonemptyLogicalWriteSurface) {
	materializeNativeDatabase();
	RelocatableStorageBackend backend(backendOptions());
	backend.open();

	EXPECT_THROW(static_cast<void>(backend.beginLogicalGraphTransaction(false)), std::logic_error);
	EXPECT_TRUE(backend.createLogicalNodes({}).empty());
	EXPECT_TRUE(backend.createLogicalEdges({}).empty());
	EXPECT_NO_THROW(backend.replaceLogicalNodeProperties({}));
	EXPECT_NO_THROW(backend.replaceLogicalEdgeProperties({}));
	EXPECT_NO_THROW(backend.deleteLogicalEdges({}));
	EXPECT_NO_THROW(backend.deleteLogicalNodes({}));
	EXPECT_TRUE(backend.executeLogicalGraphMutationBatch({}).created_node_ids.empty());

	EXPECT_THROW(static_cast<void>(backend.createLogicalNodes({{{"forbidden"}, {}}})), std::logic_error);
	EXPECT_THROW(static_cast<void>(backend.createLogicalEdges({{1U, 1U, "forbidden", {}}})), std::logic_error);
	EXPECT_THROW(backend.replaceLogicalNodeProperties({{1U, {}}}), std::logic_error);
	EXPECT_THROW(backend.replaceLogicalEdgeProperties({{1U, {}}}), std::logic_error);
	EXPECT_THROW(backend.deleteLogicalEdges({{1U}}), std::logic_error);
	EXPECT_THROW(backend.deleteLogicalNodes({{1U, true, 1U}}), std::logic_error);
	LogicalGraphMutationBatch write;
	write.node_creates = {{{"forbidden"}, {}}};
	EXPECT_THROW(static_cast<void>(backend.executeLogicalGraphMutationBatch(write)), std::logic_error);

	// The streamed bulk publisher is an internal bridge, but it is still a
	// mutating entry point and must reject a read-only handle before it inspects
	// the durable scratch source or its publication inputs.
	RelocatableStorageBackendOptions writerOptions = backendOptions();
	writerOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend writer(std::move(writerOptions));
	writer.open();
	const std::unique_ptr<NativeBulkRunStore> scratch = writer.createNativeBulkRunStore();
	const LogicalGraphMutationFence writerFence = writer.currentNativeLogicalGraphMutationFence();
	EXPECT_THROW(backend.commitNativeStreamedBulkEdges(*scratch, {}, {}, {}, writerFence), std::logic_error);
	writer.close();
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, DirectNativeReadSurfaceRejectsInvalidBoundsAndUnknownLogicalIds) {
	const NativeGraphFixture fixture = materializeNativeGraphFixture();
	RelocatableStorageBackend backend(backendOptions());
	backend.open();
	RelocatableReadSnapshot snapshot = backend.openSnapshot();

	EXPECT_FALSE(backend.containsLogicalNode(snapshot, 0U));
	EXPECT_FALSE(backend.containsLogicalNode(snapshot, fixture.mentions_edge_id + 1000U));
	EXPECT_THROW(static_cast<void>(backend.readNode(snapshot, 0U)), FormatException);
	EXPECT_THROW(static_cast<void>(backend.readEdge(snapshot, 0U)), FormatException);
	EXPECT_THROW(static_cast<void>(backend.visitNodesUntil(snapshot, 0U,
				[](const LogicalGraphNode &) { return true; })), FormatException);
	EXPECT_THROW(static_cast<void>(backend.visitEdgesUntil(snapshot, 0U,
				[](const LogicalGraphEdge &) { return true; })), FormatException);
	EXPECT_THROW(static_cast<void>(backend.visitAdjacentEdgesUntil(
				snapshot, fixture.person_node_id, LogicalGraphReadDirection::LGRD_OUTGOING, 0U,
				[](const LogicalGraphEdge &) { return true; })), FormatException);
	EXPECT_THROW(static_cast<void>(backend.scanAdjacentEdges(
				snapshot, fixture.person_node_id, LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 0U)),
				FormatException);

	// Exact lookup is an optimization only. Unsupported predicates must request
	// the caller's scan fallback, while a missing canonical label is a complete
	// empty answer with no physical lookup ambiguity.
	EXPECT_EQ(backend.findNodesByExactProperties(snapshot, {}, {{"alpha", PropertyValue(int64_t(42))}}, 1U, 4U),
				std::nullopt);
	EXPECT_EQ(backend.findNodesByExactProperties(snapshot, {"Person", "Other"},
				{{"alpha", PropertyValue(int64_t(42))}}, 1U, 4U), std::nullopt);
	EXPECT_EQ(backend.findNodesByExactProperties(snapshot, {"Person"}, {{"", PropertyValue(int64_t(42))}}, 1U, 4U),
				std::nullopt);
	EXPECT_EQ(backend.findNodesByExactProperties(snapshot, {"Person"}, {{"alpha", PropertyValue()}}, 1U, 4U),
				std::nullopt);
	EXPECT_EQ(backend.findNodesByExactProperties(snapshot, {"NoSuchLabel"},
				{{"alpha", PropertyValue(int64_t(42))}}, 1U, 4U),
				(std::optional<std::vector<LogicalGraphNode>>(std::vector<LogicalGraphNode>{})));
	EXPECT_THROW(static_cast<void>(backend.findNodesByExactProperties(
				snapshot, {"Person"}, {{"alpha", PropertyValue(int64_t(42))}}, 0U, 4U)), FormatException);
	EXPECT_THROW(static_cast<void>(backend.findNodesByExactProperties(
				snapshot, {"Person"}, {{"alpha", PropertyValue(int64_t(42))}}, 1U, 0U)), FormatException);

	snapshot.release();
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, RejectsInvalidOptimisticFenceBeforeWritingAnyGraphState) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	LogicalGraphMutationBatch write;
	write.node_creates = {{{"must_not_publish"}, {}}};
	EXPECT_THROW(static_cast<void>(backend.executeLogicalGraphMutationBatch(write, LogicalGraphMutationFence{})),
			std::invalid_argument);

	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	EXPECT_THROW(static_cast<void>(snapshot->readNode(1U)), FormatException);
	snapshot.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, LogicalWriteAdapterCreatesAtomicNodesAndEdgesWithoutLegacyStorage) {
	static_cast<void>(materializeNativeGraphFixture());
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));

	const std::vector<LogicalGraphNodeCreateRequest> nodes = {
			{{"Person", "Online"}, {{"name", PropertyValue("Ada")}, {"score", PropertyValue(int64_t(42))}}},
			{{"Online"}, {{"name", PropertyValue("Bob")}}},
	};
	const std::vector<uint64_t> nodeIds = backend.createLogicalNodes(nodes);
	ASSERT_EQ(nodeIds.size(), nodes.size());
	EXPECT_NE(nodeIds[0], nodeIds[1]);
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));

	const std::vector<LogicalGraphEdgeCreateRequest> edges = {
			{nodeIds[0], nodeIds[1], "FOLLOWS", {{"since", PropertyValue(int64_t(2026))}}},
			{nodeIds[1], nodeIds[0], "", {}},
	};
	const std::vector<uint64_t> edgeIds = backend.createLogicalEdges(edges);
	ASSERT_EQ(edgeIds.size(), edges.size());
	EXPECT_NE(edgeIds[0], edgeIds[1]);
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));

	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	const LogicalGraphNode first = snapshot->readNode(nodeIds[0]);
	EXPECT_EQ(first.labels, (std::vector<std::string>{"Person", "Online"}));
	EXPECT_EQ(first.properties, (std::vector<LogicalGraphPropertyEntry>{
											   {"name", PropertyValue("Ada")}, {"score", PropertyValue(int64_t(42))}}));
	const LogicalGraphEdge edge = snapshot->readEdge(edgeIds[0]);
	EXPECT_EQ(edge.source_node_id, nodeIds[0]);
	EXPECT_EQ(edge.target_node_id, nodeIds[1]);
	EXPECT_EQ(edge.type, "FOLLOWS");
	EXPECT_EQ(edge.properties, (std::vector<LogicalGraphPropertyEntry>{{"since", PropertyValue(int64_t(2026))}}));
	const LogicalGraphEdge untyped = snapshot->readEdge(edgeIds[1]);
	EXPECT_TRUE(untyped.type.empty());
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));

	backend.replaceLogicalNodeProperties(
			{{nodeIds[0], {{"name", PropertyValue("Grace")}, {"active", PropertyValue(true)}}}});
	backend.replaceLogicalEdgeProperties({{edgeIds[0], {}}});
	// Replacing an already empty collection is a no-op and must not require a
	// spurious metadata publication.
	backend.replaceLogicalEdgeProperties({{edgeIds[0], {}}});
	EXPECT_THROW(backend.replaceLogicalNodeProperties({{0U, {}}}), std::invalid_argument);

	snapshot = backend.openLogicalGraphReadSnapshot();
	const LogicalGraphNode replaced = snapshot->readNode(nodeIds[0]);
	EXPECT_EQ(replaced.properties, (std::vector<LogicalGraphPropertyEntry>{{"name", PropertyValue("Grace")},
															{"active", PropertyValue(true)}}));
	const LogicalGraphEdge cleared = snapshot->readEdge(edgeIds[0]);
	EXPECT_TRUE(cleared.properties.empty());
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();

	RelocatableStorageBackend readOnly(backendOptions());
	readOnly.open();
	snapshot = readOnly.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readNode(nodeIds[1]).labels, (std::vector<std::string>{"Online"}));
	EXPECT_EQ(snapshot->readEdge(edgeIds[0]).type, "FOLLOWS");
	EXPECT_TRUE(snapshot->readEdge(edgeIds[1]).type.empty());
	snapshot.reset();
	readOnly.close();
}

TEST_F(RelocatableStorageBackendTest, LogicalWriteAdapterDeletesEdgesAndDetachDeletesNodesAtomically) {
	static_cast<void>(materializeNativeGraphFixture());
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();

	const std::vector<uint64_t> nodeIds = backend.createLogicalNodes(
			{{{"Online"}, {{"name", PropertyValue("Ada")}}}, {{"Online"}, {{"name", PropertyValue("Bob")}}}});
	ASSERT_EQ(nodeIds.size(), 2U);
	const uint64_t sourceNodeId = nodeIds[0];
	const uint64_t targetNodeId = nodeIds[1];

	const uint64_t standaloneEdgeId =
			backend.createLogicalEdges(
						   {{targetNodeId, targetNodeId, "FOLLOWS", {{"kind", PropertyValue("standalone")}}}})
					.front();

	backend.deleteLogicalEdges({{standaloneEdgeId}});
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(standaloneEdgeId)), FormatException);
	snapshot.reset();

	const std::vector<uint64_t> incidentEdgeIds =
			backend.createLogicalEdges({{sourceNodeId, targetNodeId, "FOLLOWS", {{"kind", PropertyValue("out")}}},
										{targetNodeId, sourceNodeId, "FOLLOWS", {{"kind", PropertyValue("in")}}},
										{sourceNodeId, sourceNodeId, "FOLLOWS", {{"kind", PropertyValue("self")}}}});
	ASSERT_EQ(incidentEdgeIds.size(), 3U);

	// The limit is enforced before any mutation is staged, so callers can retry
	// the same operation with a larger explicitly approved work bound.
	EXPECT_THROW(backend.deleteLogicalNodes({{sourceNodeId, true, 1U}}), FormatException);
	backend.deleteLogicalNodes({{sourceNodeId, true, 3U}});
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));

	snapshot = backend.openLogicalGraphReadSnapshot();
	EXPECT_THROW(static_cast<void>(snapshot->readNode(sourceNodeId)), FormatException);
	for (const uint64_t edgeId: incidentEdgeIds) {
		EXPECT_THROW(static_cast<void>(snapshot->readEdge(edgeId)), FormatException);
	}
	EXPECT_TRUE(snapshot->scanAdjacentEdges(targetNodeId, LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 8U)
					.edges.empty());
	EXPECT_TRUE(snapshot->scanAdjacentEdges(targetNodeId, LogicalGraphReadDirection::LGRD_INCOMING, std::nullopt, 8U)
					.edges.empty());
	snapshot.reset();

	backend.deleteLogicalNodes({{targetNodeId, false, 0U}});
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, LogicalMutationBatchResolvesCreatedReferencesInOnePublication) {
	const NativeGraphFixture fixture = materializeNativeGraphFixture();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();

	const LogicalGraphMutationEntityReference firstCreatedNode{
			LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 0U};
	const LogicalGraphMutationEntityReference secondCreatedNode{
			LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 1U};
	const LogicalGraphMutationEntityReference firstCreatedEdge{
			LogicalGraphMutationEntityKind::LGMK_EDGE, LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE, 0U};
	LogicalGraphMutationBatch batch;
	batch.node_creates = {{{"Person"}, {{"name", PropertyValue("Ada")}}}, {{"Online"}, {}}};
	batch.edge_creates = {{firstCreatedNode, secondCreatedNode, "CONNECTS", {}}};
	batch.property_replacements = {{secondCreatedNode, {{"active", PropertyValue(true)}}},
								   {firstCreatedEdge, {{"since", PropertyValue(int64_t(2026))}}}};
	// A label replacement against a created Node is folded into its final fixed
	// record before the Node can become visible, rather than publishing a second
	// intermediate label state.
	batch.node_label_replacements = {{secondCreatedNode, {"Person", "Online"}}};

	const LogicalGraphMutationBatchResult result = backend.executeLogicalGraphMutationBatch(batch);
	ASSERT_EQ(result.created_node_ids.size(), 2U);
	ASSERT_EQ(result.created_edge_ids.size(), 1U);
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));

	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	const LogicalGraphNode first = snapshot->readNode(result.created_node_ids[0]);
	const LogicalGraphNode second = snapshot->readNode(result.created_node_ids[1]);
	EXPECT_EQ(first.labels, (std::vector<std::string>{"Person"}));
	EXPECT_EQ(second.labels, (std::vector<std::string>{"Person", "Online"}));
	EXPECT_EQ(first.properties, (std::vector<LogicalGraphPropertyEntry>{{"name", PropertyValue("Ada")}}));
	EXPECT_EQ(second.properties, (std::vector<LogicalGraphPropertyEntry>{{"active", PropertyValue(true)}}));
	const LogicalGraphEdge edge = snapshot->readEdge(result.created_edge_ids[0]);
	EXPECT_EQ(edge.source_node_id, result.created_node_ids[0]);
	EXPECT_EQ(edge.target_node_id, result.created_node_ids[1]);
	EXPECT_EQ(edge.properties, (std::vector<LogicalGraphPropertyEntry>{{"since", PropertyValue(int64_t(2026))}}));
	snapshot.reset();

	LogicalGraphMutationBatch existingLabel;
	existingLabel.node_label_replacements = {
			{{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED,
			  fixture.unlabeled_node_id},
			 {"Online"}}};
	EXPECT_TRUE(backend.executeLogicalGraphMutationBatch(existingLabel).created_node_ids.empty());
	snapshot = backend.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readNode(fixture.unlabeled_node_id).labels, (std::vector<std::string>{"Online"}));
	snapshot.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, LogicalMutationBatchRejectsAStaleReadSnapshotFenceWithoutPublishing) {
	static_cast<void>(materializeNativeGraphFixture());
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();

	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(snapshot, nullptr);
	const std::optional<LogicalGraphMutationFence> fence = snapshot->mutationFence();
	ASSERT_TRUE(fence.has_value());
	ASSERT_TRUE(fence->isValid());

	LogicalGraphMutationBatch batch;
	batch.node_creates = {{{"Person"}, {}}};
	const LogicalGraphMutationBatchResult first = backend.executeLogicalGraphMutationBatch(batch, fence);
	ASSERT_EQ(first.created_node_ids.size(), 1U);
	EXPECT_THROW(static_cast<void>(backend.executeLogicalGraphMutationBatch(batch, fence)),
				 LogicalGraphMutationConflict);

	// The rejected batch never advances the logical Node allocator or publishes
	// a partially staged entity, so the next unfenced write receives the next
	// contiguous ID after the successful fenced publication.
	const std::vector<uint64_t> second = backend.createLogicalNodes({{{"Person"}, {}}});
	ASSERT_EQ(second.size(), 1U);
	EXPECT_GT(second.front(), first.created_node_ids.front());
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	snapshot.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, LogicalMutationFenceEncodesEveryGraphVisibleRoot) {
	Superblock superblock;
	superblock.database_id = DatabaseId(7U);
	std::array<PhysicalPageRef *, LogicalGraphMutationFence::GRAPH_STATE_ROOT_COUNT> roots = {
			&superblock.node_entity_directory_root,
			&superblock.edge_entity_directory_root,
			&superblock.entity_property_directory_root,
			&superblock.large_value_manifest_directory_root,
			&superblock.vector_location_directory_root,
			&superblock.vector_manifest_directory_root,
			&superblock.state_directory_root,
        &superblock.hybrid_property_descriptor_root,
	};
	for (size_t index = 0U; index < roots.size(); ++index) {
		*roots[index] = {PhysicalSlotId(101U + index), static_cast<uint8_t>(RSS_PAGE_POOL_FIRST_DATA_FRAME + index),
						 Generation(201U + index)};
	}

	const LogicalGraphMutationFence fence = RelocatableStorageBackend::makeLogicalGraphMutationFence(superblock);
	ASSERT_TRUE(fence.isValid());
	ASSERT_EQ(fence.graph_state_tokens.size(), LogicalGraphMutationFence::GRAPH_STATE_ROOT_COUNT *
													   LogicalGraphMutationFence::GRAPH_STATE_TOKENS_PER_ROOT);
	for (size_t index = 0U; index < roots.size(); ++index) {
		const size_t offset = index * LogicalGraphMutationFence::GRAPH_STATE_TOKENS_PER_ROOT;
		EXPECT_EQ(fence.graph_state_tokens[offset], roots[index]->physical_slot.value());
		EXPECT_EQ(fence.graph_state_tokens[offset + 1U], roots[index]->frame_index);
		EXPECT_EQ(fence.graph_state_tokens[offset + 2U], roots[index]->generation.value());
	}
}

TEST_F(RelocatableStorageBackendTest, LogicalTransactionStagesMultipleBatchesAndPublishesOnce) {
	const NativeGraphFixture fixture = materializeNativeGraphFixture();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();

	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	const LogicalGraphMutationEntityReference persistedSource{LogicalGraphMutationEntityKind::LGMK_NODE,
															  LogicalGraphMutationReferenceKind::LGRK_PERSISTED, fixture.person_node_id};
	const LogicalGraphMutationEntityReference locallyCreatedNode{
			LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 0U};
	LogicalGraphMutationBatch first;
	first.node_creates = {{{"TxnFirst"}, {}}};
	const LogicalGraphMutationBatchResult stagedFirst = transaction->stageMutationBatch(std::move(first));
	ASSERT_EQ(stagedFirst.created_node_ids.size(), 1U);

	LogicalGraphMutationBatch second;
	second.node_creates = {{{"TxnSecond"}, {}}};
	second.edge_creates = {{persistedSource, locallyCreatedNode, "TXN_LINK", {}}};
	second.property_replacements = {{locallyCreatedNode, {{"rank", PropertyValue(int64_t(7))}}}};
	second.node_label_replacements = {{locallyCreatedNode, {"TxnSecond", "TxnTagged"}}};
	const LogicalGraphMutationBatchResult stagedSecond = transaction->stageMutationBatch(std::move(second));
	ASSERT_EQ(stagedSecond.created_node_ids.size(), 1U);
	ASSERT_EQ(stagedSecond.created_edge_ids.size(), 1U);

	// Staging owns only a logical request. Neither the transaction's snapshot nor
	// concurrent snapshots may observe its Node before the single commit.
	EXPECT_THROW(static_cast<void>(transaction->readSnapshot().readNode(stagedFirst.created_node_ids.front())), FormatException);
	const LogicalGraphMutationBatchResult result = transaction->commit();
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_COMMITTED);
	EXPECT_EQ(result.created_node_ids,
			  (std::vector<uint64_t>{stagedFirst.created_node_ids.front(), stagedSecond.created_node_ids.front()}));
	EXPECT_EQ(result.created_edge_ids, stagedSecond.created_edge_ids);
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));

	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(snapshot, nullptr);
	EXPECT_EQ(snapshot->readNode(result.created_node_ids[0]).labels, (std::vector<std::string>{"TxnFirst"}));
	const LogicalGraphNode createdSecond = snapshot->readNode(result.created_node_ids[1]);
	EXPECT_EQ(createdSecond.labels, (std::vector<std::string>{"TxnSecond", "TxnTagged"}));
	EXPECT_EQ(createdSecond.properties, (std::vector<LogicalGraphPropertyEntry>{{"rank", PropertyValue(int64_t(7))}}));
	const LogicalGraphEdge edge = snapshot->readEdge(result.created_edge_ids[0]);
	EXPECT_EQ(edge.source_node_id, fixture.person_node_id);
	EXPECT_EQ(edge.target_node_id, result.created_node_ids[1]);
	EXPECT_TRUE(edge.properties.empty());
	snapshot.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, LogicalTransactionFoldsLaterPropertiesIntoAnEarlierCreatedEdge) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();

	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	const LogicalGraphMutationEntityReference firstNode{
			LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 0U};
	const LogicalGraphMutationEntityReference secondNode{
			LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 1U};
	LogicalGraphMutationBatch create;
	create.node_creates = {{{"FoldedEdgeSource"}, {}}, {{"FoldedEdgeTarget"}, {}}};
	create.edge_creates = {{firstNode, secondNode, "FOLDED_EDGE", {}}};
	const LogicalGraphMutationBatchResult staged = transaction->stageMutationBatch(std::move(create));
	ASSERT_EQ(staged.created_edge_ids.size(), 1U);

	LogicalGraphMutationBatch update;
	update.property_replacements = {{
			{LogicalGraphMutationEntityKind::LGMK_EDGE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED,
			 staged.created_edge_ids.front()},
			{{"weight", PropertyValue(int64_t(9))}},
	}};
	EXPECT_TRUE(transaction->stageMutationBatch(std::move(update)).created_edge_ids.empty());
	EXPECT_EQ(transaction->commit(), staged);

	backend.close();
	RelocatableStorageBackend reopened(backendOptions());
	reopened.open();
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = reopened.openLogicalGraphReadSnapshot();
	const LogicalGraphEdge edge = snapshot->readEdge(staged.created_edge_ids.front());
	EXPECT_EQ(edge.type, "FOLDED_EDGE");
	EXPECT_EQ(edge.properties, (std::vector<LogicalGraphPropertyEntry>{{"weight", PropertyValue(int64_t(9))}}));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest, LogicalTransactionPreviewReservesOneStableCreationShape) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();

	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	LogicalGraphMutationBatch oneNode;
	oneNode.node_creates = {{{"Previewed"}, {}}};
	const LogicalGraphMutationBatchResult firstPreview = transaction->previewMutationBatch(oneNode);
	const LogicalGraphMutationBatchResult secondPreview = transaction->previewMutationBatch(oneNode);
	ASSERT_EQ(firstPreview.created_node_ids.size(), 1U);
	EXPECT_EQ(secondPreview, firstPreview);

	LogicalGraphMutationBatch differentShape = oneNode;
	differentShape.node_creates.push_back({{"Previewed"}, {}});
	EXPECT_THROW(static_cast<void>(transaction->previewMutationBatch(differentShape)), std::logic_error);
	EXPECT_THROW(static_cast<void>(transaction->stageMutationBatch(differentShape)), std::logic_error);

	// Matching the Node count alone is insufficient: the outstanding lease also
	// fixes the Edge creation shape for the statement that may consume it.
	LogicalGraphMutationBatch differentEdgeShape = oneNode;
	differentEdgeShape.edge_creates.push_back({});
	EXPECT_THROW(static_cast<void>(transaction->previewMutationBatch(differentEdgeShape)), std::logic_error);
	EXPECT_THROW(static_cast<void>(transaction->stageMutationBatch(differentEdgeShape)), std::logic_error);

	const LogicalGraphMutationBatchResult staged = transaction->stageMutationBatch(std::move(oneNode));
	EXPECT_EQ(staged, firstPreview);
	const LogicalGraphMutationBatchResult committed = transaction->commit();
	EXPECT_EQ(committed, firstPreview);
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_COMMITTED);
	EXPECT_THROW(static_cast<void>(transaction->previewMutationBatch({})), std::logic_error);
	EXPECT_THROW(static_cast<void>(transaction->readSnapshot()), std::logic_error);
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, LogicalTransactionRejectsCallerSuppliedCreationIdentityAndAbandonsPreview) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();

	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	LogicalGraphMutationBatch previewed;
	previewed.node_creates = {{{"PreviewedThenRolledBack"}, {}}};
	const uint64_t previewId = transaction->previewMutationBatch(previewed).created_node_ids.front();
	LogicalGraphMutationBatch callerOwnedPlan = previewed;
	callerOwnedPlan.creation_identity_plan = LogicalGraphCreationIdentityPlan{};
	EXPECT_THROW(static_cast<void>(transaction->stageMutationBatch(std::move(callerOwnedPlan))), std::invalid_argument);
	transaction->rollback();
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_ROLLED_BACK);

	const std::vector<uint64_t> committed = backend.createLogicalNodes({{{"AfterPreviewRollback"}, {}}});
	ASSERT_EQ(committed.size(), 1U);
	EXPECT_NE(committed.front(), previewId);
	EXPECT_THROW(static_cast<void>(transaction->commit()), std::logic_error);
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, LogicalTransactionOverlayProvidesReadYourWritesAndResumableCreatedAdjacency) {
	static_cast<void>(materializeNativeGraphFixture());
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();

	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	LogicalGraphTransactionOverlay overlay(*transaction);
	const std::vector<uint64_t> nodes = overlay.createLogicalNodes({{{"Overlay"}, {}}, {{"Overlay"}, {}}});
	ASSERT_EQ(nodes.size(), 2U);
	const std::vector<uint64_t> edges = overlay.createLogicalEdges({{nodes[0], nodes[1], "OVERLAY_LINK", {}},
										  {nodes[0], nodes[1], "OVERLAY_LINK", {}},
										  {nodes[0], nodes[1], "OVERLAY_LINK", {}}});
	ASSERT_EQ(edges.size(), 3U);
	// A staged replacement must be visible to all later reads in this same
	// transaction without publishing a physical graph generation.
	overlay.replaceLogicalNodeProperties(
			{{nodes[0], {{"name", PropertyValue("Grace")}, {"rank", PropertyValue(int64_t(7))}}}});

	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = overlay.openLogicalGraphReadSnapshot();
	ASSERT_NE(snapshot, nullptr);
	const LogicalGraphNode source = snapshot->readNode(nodes[0]);
	// The transaction overlay carries owner-keyed properties directly.
	EXPECT_EQ(source.properties, (std::vector<LogicalGraphPropertyEntry>{{"name", PropertyValue("Grace")},
																					 {"rank", PropertyValue(int64_t(7))}}));
	const LogicalGraphAdjacencyPage first =
			snapshot->scanAdjacentEdges(nodes[0], LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 2U);
	ASSERT_EQ(first.edges.size(), 2U);
	ASSERT_TRUE(first.continuation.has_value());
	const LogicalGraphAdjacencyPage second =
			snapshot->scanAdjacentEdges(nodes[0], LogicalGraphReadDirection::LGRD_OUTGOING, first.continuation, 2U);
	ASSERT_EQ(second.edges.size(), 1U);
	EXPECT_FALSE(second.continuation.has_value());
	snapshot.reset();

	// The base backend still sees none of the staged Nodes before commit.
	snapshot = backend.openLogicalGraphReadSnapshot();
	EXPECT_THROW(static_cast<void>(snapshot->readNode(nodes[0])), FormatException);
	snapshot.reset();
	static_cast<void>(transaction->commit());
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));

	snapshot = backend.openLogicalGraphReadSnapshot();
	const LogicalGraphNode committed = snapshot->readNode(nodes[0]);
	EXPECT_EQ(committed.properties, (std::vector<LogicalGraphPropertyEntry>{{"name", PropertyValue("Grace")},
													  {"rank", PropertyValue(int64_t(7))}}));
	snapshot.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, LogicalTransactionRollbackPublishesNothing) {
	static_cast<void>(materializeNativeGraphFixture());
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();

	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	LogicalGraphMutationBatch batch;
	batch.node_creates = {{{"RolledBack"}, {{"value", PropertyValue(int64_t(1))}}}};
	const LogicalGraphMutationBatchResult staged = transaction->stageMutationBatch(std::move(batch));
	ASSERT_EQ(staged.created_node_ids.size(), 1U);
	transaction->rollback();
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_ROLLED_BACK);
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));

	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(snapshot, nullptr);
	EXPECT_THROW(static_cast<void>(snapshot->readNode(staged.created_node_ids.front())), FormatException);
	snapshot.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, ActiveLogicalTransactionDestructorAbandonsItsProvisionalIdsWithoutPublishing) {
	static_cast<void>(materializeNativeGraphFixture());
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();

	uint64_t provisionalId = 0U;
	Superblock afterLease;
	{
		std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
		LogicalGraphMutationBatch batch;
		batch.node_creates = {{{"DestructorAbandoned"}, {}}};
		const LogicalGraphMutationBatchResult staged = transaction->stageMutationBatch(std::move(batch));
		ASSERT_EQ(staged.created_node_ids.size(), 1U);
		provisionalId = staged.created_node_ids.front();

		RelocatableReadSnapshot leased = backend.openSnapshot();
		afterLease = leased.superblock();
		leased.release();
	}

	RelocatableReadSnapshot afterDestructor = backend.openSnapshot();
	EXPECT_EQ(afterDestructor.superblock(), afterLease);
	afterDestructor.release();
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	EXPECT_THROW(static_cast<void>(snapshot->readNode(provisionalId)), FormatException);
	snapshot.reset();

	const std::vector<uint64_t> committed = backend.createLogicalNodes({{{"AfterDestructor"}, {}}});
	ASSERT_EQ(committed.size(), 1U);
	EXPECT_GT(committed.front(), provisionalId);
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, LogicalTransactionCancelsCreatedEntitiesWithoutReusingProvisionalIds) {
	const NativeGraphFixture fixture = materializeNativeGraphFixture();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();

	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	const LogicalGraphMutationEntityReference firstCreatedNode{
			LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 0U};
	const LogicalGraphMutationEntityReference secondCreatedNode{
			LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 1U};
	LogicalGraphMutationBatch create;
	create.node_creates = {{{"Cancelled"}, {{"name", PropertyValue("discarded")}}},
						   {{"Survives"}, {{"name", PropertyValue("kept")}}}};
	create.edge_creates = {
			{firstCreatedNode, secondCreatedNode, "CANCELLED_LINK", {{"rank", PropertyValue(int64_t(7))}}}};
	const LogicalGraphMutationBatchResult stagedCreate = transaction->stageMutationBatch(std::move(create));
	ASSERT_EQ(stagedCreate.created_node_ids.size(), 2U);
	ASSERT_EQ(stagedCreate.created_edge_ids.size(), 1U);

	LogicalGraphMutationBatch erase;
	erase.edge_deletes = {{stagedCreate.created_edge_ids.front()}};
	erase.node_deletes = {{stagedCreate.created_node_ids.front(), false, 0U}};
	static_cast<void>(transaction->stageMutationBatch(std::move(erase)));
	const LogicalGraphMutationBatchResult committed = transaction->commit();
	EXPECT_EQ(committed.created_node_ids, (std::vector<uint64_t>{stagedCreate.created_node_ids.back()}));
	EXPECT_TRUE(committed.created_edge_ids.empty());
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));

	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	EXPECT_THROW(static_cast<void>(snapshot->readNode(stagedCreate.created_node_ids.front())), FormatException);
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(stagedCreate.created_edge_ids.front())), FormatException);
	const LogicalGraphNode survivor = snapshot->readNode(stagedCreate.created_node_ids.back());
	EXPECT_EQ(survivor.properties, (std::vector<LogicalGraphPropertyEntry>{{"name", PropertyValue("kept")}}));
	snapshot.reset();

	const std::vector<uint64_t> afterCancellation = backend.createLogicalNodes({{{"AfterCancellation"}, {}}});
	ASSERT_EQ(afterCancellation.size(), 1U);
	EXPECT_GT(afterCancellation.front(), stagedCreate.created_node_ids.back());
	const std::vector<uint64_t> afterCancellationEdge =
			backend.createLogicalEdges({{fixture.person_node_id, fixture.unlabeled_node_id, "AFTER_CANCELLATION", {}}});
	ASSERT_EQ(afterCancellationEdge.size(), 1U);
	std::unique_ptr<LogicalGraphTransaction> allCancelled = backend.beginLogicalGraphTransaction(false);
	LogicalGraphMutationBatch onlyCreate;
	onlyCreate.node_creates = {{{"OnlyCancelled"}, {}}};
	const LogicalGraphMutationBatchResult stagedOnlyCreate = allCancelled->stageMutationBatch(std::move(onlyCreate));
	ASSERT_EQ(stagedOnlyCreate.created_node_ids.size(), 1U);
	LogicalGraphMutationBatch onlyDelete;
	onlyDelete.node_deletes = {{stagedOnlyCreate.created_node_ids.front(), false, 0U}};
	static_cast<void>(allCancelled->stageMutationBatch(std::move(onlyDelete)));
	EXPECT_TRUE(allCancelled->commit().created_node_ids.empty());
	const std::vector<uint64_t> afterOnlyCancellation = backend.createLogicalNodes({{{"AfterOnlyCancellation"}, {}}});
	ASSERT_EQ(afterOnlyCancellation.size(), 1U);
	EXPECT_GT(afterOnlyCancellation.front(), stagedOnlyCreate.created_node_ids.front());
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, LogicalTransactionAtomicallyCombinesPersistedNodePropertyAndLabelReplacement) {
	const NativeGraphFixture fixture = materializeNativeGraphFixture();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();

	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	const LogicalGraphMutationEntityReference persistedNode{LogicalGraphMutationEntityKind::LGMK_NODE,
															LogicalGraphMutationReferenceKind::LGRK_PERSISTED, fixture.person_node_id};
	LogicalGraphMutationBatch property;
	property.property_replacements = {{persistedNode, {{"alpha", PropertyValue(int64_t(9))}}}};
	EXPECT_TRUE(transaction->stageMutationBatch(std::move(property)).created_node_ids.empty());
	LogicalGraphMutationBatch labels;
	labels.node_label_replacements = {{persistedNode, {"Other"}}};
	EXPECT_TRUE(transaction->stageMutationBatch(std::move(labels)).created_node_ids.empty());
	EXPECT_TRUE(transaction->commit().created_node_ids.empty());
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_COMMITTED);
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));

	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	const LogicalGraphNode node = snapshot->readNode(fixture.person_node_id);
	EXPECT_EQ(node.labels, (std::vector<std::string>{"Other"}));
	EXPECT_EQ(node.properties, (std::vector<LogicalGraphPropertyEntry>{{"alpha", PropertyValue(int64_t(9))}}));
	snapshot.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, LogicalTransactionRejectsStaleFenceWithoutPublishingStagedWork) {
	static_cast<void>(materializeNativeGraphFixture());
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();

	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	const std::vector<uint64_t> concurrent = backend.createLogicalNodes({{{"ConcurrentWriter"}, {}}});
	ASSERT_EQ(concurrent.size(), 1U);
	LogicalGraphMutationBatch batch;
	batch.node_creates = {{{"MustNotPublish"}, {}}};
	const LogicalGraphMutationBatchResult staged = transaction->stageMutationBatch(std::move(batch));
	ASSERT_EQ(staged.created_node_ids.size(), 1U);
	EXPECT_THROW(static_cast<void>(transaction->commit()), LogicalGraphMutationConflict);
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_CONFLICTED);
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));

	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(snapshot, nullptr);
	EXPECT_NO_THROW(static_cast<void>(snapshot->readNode(concurrent.front())));
	// Leases make the staged identity durable before it is exposed. A conflicting
	// writer therefore cannot reuse it; the uncommitted Node stays absent.
	EXPECT_NE(staged.created_node_ids.front(), concurrent.front());
	EXPECT_THROW(static_cast<void>(snapshot->readNode(staged.created_node_ids.front())), std::exception);
	snapshot.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, ReadOnlyLogicalTransactionRejectsStagingAndCanCommitEmpty) {
	const NativeGraphFixture fixture = materializeNativeGraphFixture();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();

	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(true);
	EXPECT_TRUE(transaction->isReadOnly());
	EXPECT_NO_THROW(static_cast<void>(transaction->readSnapshot().readNode(fixture.person_node_id)));
	LogicalGraphMutationBatch batch;
	batch.node_creates = {{{"Forbidden"}, {}}};
	EXPECT_THROW(static_cast<void>(transaction->previewMutationBatch(batch)), std::logic_error);
	EXPECT_THROW(static_cast<void>(transaction->stageMutationBatch(std::move(batch))), std::logic_error);
	const LogicalGraphMutationBatchResult result = transaction->commit();
	EXPECT_TRUE(result.created_node_ids.empty());
	EXPECT_TRUE(result.created_edge_ids.empty());
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_COMMITTED);
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, WritableEmptyLogicalTransactionCommitPreservesThePublishedGeneration) {
	static_cast<void>(materializeNativeGraphFixture());
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();

	RelocatableReadSnapshot before = backend.openSnapshot();
	const Superblock publishedBeforeCommit = before.superblock();
	before.release();

	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	const LogicalGraphMutationBatchResult committed = transaction->commit();
	EXPECT_TRUE(committed.created_node_ids.empty());
	EXPECT_TRUE(committed.created_edge_ids.empty());
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_COMMITTED);

	RelocatableReadSnapshot after = backend.openSnapshot();
	EXPECT_EQ(after.superblock(), publishedBeforeCommit);
	after.release();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, PublicDirectApiUsesTheLogicalWriteAdapter) {
	static_cast<void>(materializeNativeGraphFixture());

	zyx::Database database(data_path_.string());
	database.open();
	const int64_t first = database.createNode("PublicOnline", {{"name", std::string("Ada")}});
	const int64_t second =
			database.createNode(std::vector<std::string>{"Person", "PublicOnline"}, {{"score", int64_t(9)}});
	const int64_t edge = database.createEdge(first, second, "PUBLIC_FOLLOWS", {{"since", int64_t(2026)}});
	EXPECT_GT(first, 0);
	EXPECT_GT(second, first);
	EXPECT_GT(edge, 0);

	zyx::Result result = database.execute("MATCH (n:PublicOnline) RETURN n");
	ASSERT_TRUE(result.isSuccess()) << result.getError();
	ASSERT_TRUE(result.hasNext());
	result.next();
	const auto firstNode = std::get<std::shared_ptr<zyx::Node>>(result.get("n"));
	ASSERT_NE(firstNode, nullptr);
	EXPECT_EQ(firstNode->id, first);
	EXPECT_EQ(std::get<std::string>(firstNode->properties.at("name")), "Ada");
	EXPECT_TRUE(database.execute("CREATE (:PublicOnline)").isSuccess());
	database.close();

	RelocatableStorageBackend readOnly(backendOptions());
	readOnly.open();
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = readOnly.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(static_cast<uint64_t>(edge)).type, "PUBLIC_FOLLOWS");
	snapshot.reset();
	readOnly.close();
}

TEST_F(RelocatableStorageBackendTest, PublicLogicalTransactionProvidesReadYourWritesAndAtomicCommit) {
	static_cast<void>(materializeNativeGraphFixture());

	zyx::Database database(data_path_.string());
	database.open();
	EXPECT_FALSE(database.hasActiveTransaction());
	auto transaction = database.beginTransaction();
	ASSERT_TRUE(transaction.isActive());
	ASSERT_FALSE(transaction.isReadOnly());
	EXPECT_TRUE(database.hasActiveTransaction());
	const zyx::Result create = transaction.execute("CREATE (n:PublicTxn {name: 'Ada'}) RETURN n.name AS name");
	ASSERT_TRUE(create.isSuccess()) << create.getError();
	ASSERT_TRUE(create.hasNext());
	create.next();
	EXPECT_EQ(std::get<std::string>(create.get("name")), "Ada");
	const zyx::Result set = transaction.execute("MATCH (n:PublicTxn) SET n.rank = 7 RETURN n.rank AS rank");
	ASSERT_TRUE(set.isSuccess()) << set.getError();
	ASSERT_TRUE(set.hasNext());
	set.next();
	EXPECT_EQ(std::get<int64_t>(set.get("rank")), 7);

	const zyx::Result readYourWrites = transaction.execute("MATCH (n:PublicTxn) RETURN n.name AS name, n.rank AS rank");
	ASSERT_TRUE(readYourWrites.isSuccess()) << readYourWrites.getError();
	ASSERT_TRUE(readYourWrites.hasNext());
	readYourWrites.next();
	EXPECT_EQ(std::get<std::string>(readYourWrites.get("name")), "Ada");
	EXPECT_EQ(std::get<int64_t>(readYourWrites.get("rank")), 7);
	const zyx::Result outsideBeforeCommit = database.execute("MATCH (n:PublicTxn) RETURN n");
	ASSERT_TRUE(outsideBeforeCommit.isSuccess()) << outsideBeforeCommit.getError();
	EXPECT_FALSE(outsideBeforeCommit.hasNext());

	transaction.commit();
	EXPECT_FALSE(transaction.isActive());
	EXPECT_FALSE(database.hasActiveTransaction());
	const zyx::Result outsideAfterCommit = database.execute("MATCH (n:PublicTxn) RETURN n.name AS name");
	ASSERT_TRUE(outsideAfterCommit.isSuccess()) << outsideAfterCommit.getError();
	ASSERT_TRUE(outsideAfterCommit.hasNext());
	outsideAfterCommit.next();
	EXPECT_EQ(std::get<std::string>(outsideAfterCommit.get("name")), "Ada");
	database.close();
}

TEST_F(RelocatableStorageBackendTest, UniqueConstraintLookupFindsLargeStagedNodesBeforeCreatingRelationships) {
	static_cast<void>(materializeNativeGraphFixture());

	zyx::Database database(data_path_.string());
	database.open();
	const zyx::Result createUnique =
			database.execute("CREATE CONSTRAINT memory_id_unique FOR (n:Memory) REQUIRE n.id IS UNIQUE");
	ASSERT_TRUE(createUnique.isSuccess()) << createUnique.getError();
	const std::string retainedText(64U * 1024U, 'x');
	ASSERT_TRUE(database.execute("CREATE (:Memory {id: 'historical', text: '" + retainedText + "'})").isSuccess());

	auto transaction = database.beginTransaction();
	ASSERT_TRUE(transaction.execute("CREATE (:Memory {id: 'source', text: '" + retainedText + "'})").isSuccess());
	ASSERT_TRUE(transaction.execute("CREATE (:Memory {id: 'target', text: '" + retainedText + "'})").isSuccess());
	const std::unordered_map<std::string, zyx::Value> endpoints = {{"sourceId", std::string("source")},
																   {"targetId", std::string("target")}};
	const zyx::Result relationship = transaction.execute(
			"MATCH (source:Memory {id: $sourceId}), (target:Memory {id: $targetId}) CREATE (source)-[:LINK]->(target)",
			endpoints);
	ASSERT_TRUE(relationship.isSuccess()) << relationship.getError();
	transaction.commit();

	zyx::Result persisted = database.execute(
			"MATCH (source:Memory {id: 'source'})-[:LINK]->(target:Memory {id: 'target'}) RETURN source.id AS id");
	ASSERT_TRUE(persisted.isSuccess()) << persisted.getError();
	ASSERT_TRUE(persisted.hasNext());
	persisted.next();
	EXPECT_EQ(std::get<std::string>(persisted.get("id")), "source");
	database.close();
}

TEST_F(RelocatableStorageBackendTest, LogicalConstraintWritesRejectMissingAndIncorrectPropertyValuesAtomically) {
	static_cast<void>(materializeNativeGraphFixture());

	zyx::Database database(data_path_.string());
	database.open();
	const zyx::Result createNotNull =
			database.execute("CREATE CONSTRAINT strict_name FOR (n:Strict) REQUIRE n.name IS NOT NULL");
	ASSERT_TRUE(createNotNull.isSuccess()) << createNotNull.getError();
	const zyx::Result createTyped =
			database.execute("CREATE CONSTRAINT typed_age FOR (n:Typed) REQUIRE n.age IS ::INTEGER");
	ASSERT_TRUE(createTyped.isSuccess()) << createTyped.getError();

	const zyx::Result missingRequired = database.execute("CREATE (:Strict)");
	EXPECT_FALSE(missingRequired.isSuccess());
	const zyx::Result wrongType = database.execute("CREATE (:Typed {age: 'invalid'})");
	EXPECT_FALSE(wrongType.isSuccess());

	zyx::Result strictRows = database.execute("MATCH (n:Strict) RETURN n");
	ASSERT_TRUE(strictRows.isSuccess()) << strictRows.getError();
	EXPECT_FALSE(strictRows.hasNext());
	zyx::Result typedRows = database.execute("MATCH (n:Typed) RETURN n");
	ASSERT_TRUE(typedRows.isSuccess()) << typedRows.getError();
	EXPECT_FALSE(typedRows.hasNext());

	ASSERT_TRUE(database.execute("CREATE (:Strict {name: 'Ada'})").isSuccess());
	ASSERT_TRUE(database.execute("CREATE (:Typed {age: 7})").isSuccess());
	const zyx::Result invalidUpdate = database.execute("MATCH (n:Typed) SET n.age = 'invalid'");
	EXPECT_FALSE(invalidUpdate.isSuccess());
	zyx::Result validTyped = database.execute("MATCH (n:Typed) RETURN n.age AS age");
	ASSERT_TRUE(validTyped.isSuccess()) << validTyped.getError();
	ASSERT_TRUE(validTyped.hasNext());
	validTyped.next();
	EXPECT_EQ(std::get<int64_t>(validTyped.get("age")), 7);
	database.close();
}

TEST_F(RelocatableStorageBackendTest, PersistsNodeKeyAndEdgeConstraintsAcrossIntegrityCheckedReopen) {
	static_cast<void>(materializeNativeGraphFixture());

	RelocatableStorageBackendOptions writableOptions = backendOptions();
	writableOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(writableOptions));
	backend.open();
	const LogicalGraphConstraintDefinition nodeKey =
			backend.createLogicalConstraint({"account_tenant_email_key",
											 LogicalGraphConstraintEntityKind::LGCEK_NODE,
											 LogicalGraphConstraintKind::LGCK_NODE_KEY,
											 "Account",
											 {"tenant", "email"},
											 LogicalGraphConstraintPropertyType::LGCPT_NONE});
	const LogicalGraphConstraintDefinition edgeNotNull =
			backend.createLogicalConstraint({"reports_weight_present",
											 LogicalGraphConstraintEntityKind::LGCEK_EDGE,
											 LogicalGraphConstraintKind::LGCK_NOT_NULL,
											 "REPORTS",
											 {"weight"},
											 LogicalGraphConstraintPropertyType::LGCPT_NONE});
	const LogicalGraphConstraintDefinition edgeType =
			backend.createLogicalConstraint({"reports_weight_integer",
											 LogicalGraphConstraintEntityKind::LGCEK_EDGE,
											 LogicalGraphConstraintKind::LGCK_PROPERTY_TYPE,
											 "REPORTS",
											 {"weight"},
											 LogicalGraphConstraintPropertyType::LGCPT_INTEGER});
	const LogicalGraphConstraintDefinition edgeUnique =
			backend.createLogicalConstraint({"reports_code_unique",
											 LogicalGraphConstraintEntityKind::LGCEK_EDGE,
											 LogicalGraphConstraintKind::LGCK_UNIQUE,
											 "REPORTS",
											 {"code"},
											 LogicalGraphConstraintPropertyType::LGCPT_NONE});

	const std::vector<uint64_t> accounts = backend.createLogicalNodes(
			{{{"Account"},
			  {{"tenant", PropertyValue(int64_t{1})}, {"email", PropertyValue(std::string("first@example.test"))}}},
			 {{"Account"},
			  {{"tenant", PropertyValue(int64_t{1})}, {"email", PropertyValue(std::string("second@example.test"))}}}});
	ASSERT_EQ(accounts.size(), 2U);

	EXPECT_THROW(
			static_cast<void>(backend.createLogicalNodes({{{"Account"}, {{"tenant", PropertyValue(int64_t{1})}}}})),
			std::runtime_error);
	EXPECT_THROW(static_cast<void>(
						 backend.createLogicalNodes({{{"Account"},
													  {{"tenant", PropertyValue(int64_t{1})},
													   {"email", PropertyValue(std::string("first@example.test"))}}}})),
				 std::runtime_error);

	const std::vector<uint64_t> persisted = backend.createLogicalEdges(
			{{accounts[0],
			  accounts[1],
			  "REPORTS",
			  {{"weight", PropertyValue(int64_t{7})}, {"code", PropertyValue(std::string("r-1"))}}}});
	ASSERT_EQ(persisted.size(), 1U);
	EXPECT_THROW(static_cast<void>(backend.createLogicalEdges(
						 {{accounts[0], accounts[1], "REPORTS", {{"code", PropertyValue(std::string("r-2"))}}}})),
				 std::runtime_error);
	EXPECT_THROW(static_cast<void>(backend.createLogicalEdges({{accounts[0],
																accounts[1],
																"REPORTS",
																{{"weight", PropertyValue(std::string("seven"))},
																 {"code", PropertyValue(std::string("r-3"))}}}})),
				 std::runtime_error);
	EXPECT_THROW(static_cast<void>(backend.createLogicalEdges(
						 {{accounts[0],
						   accounts[1],
						   "REPORTS",
						   {{"weight", PropertyValue(int64_t{8})}, {"code", PropertyValue(std::string("r-1"))}}}})),
				 std::runtime_error);
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();

	RelocatableStorageBackendOptions integrityOptions = backendOptions();
	integrityOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	integrityOptions.verify_integrity_on_open = true;
	RelocatableStorageBackend reopened(integrityOptions);
	EXPECT_NO_THROW(reopened.open());
	ASSERT_EQ(reopened.listLogicalConstraints(8U),
			  (std::vector<LogicalGraphConstraintDefinition>{nodeKey, edgeNotNull, edgeType, edgeUnique}));
	EXPECT_TRUE(reopened.dropLogicalConstraintByName(edgeType.name));
	EXPECT_FALSE(reopened.dropLogicalConstraintByName(edgeType.name));
	EXPECT_NO_THROW(
			static_cast<void>(reopened.createLogicalEdges({{accounts[0],
															accounts[1],
															"REPORTS",
															{{"weight", PropertyValue(std::string("now-permitted"))},
															 {"code", PropertyValue(std::string("r-4"))}}}})));
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest, ValidatesExistingGraphBeforePublishingConstraintDefinitions) {
	static_cast<void>(materializeNativeGraphFixture());

	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> accounts = backend.createLogicalNodes(
			{{{"ExistingAccount"},
			  {{"tenant", PropertyValue(int64_t{1})}, {"email", PropertyValue(std::string("first@example.test"))}}},
			 {{"ExistingAccount"},
			  {{"tenant", PropertyValue(int64_t{1})}, {"email", PropertyValue(std::string("second@example.test"))}}},
			 {{"DuplicateAccount"}, {{"code", PropertyValue(std::string("shared"))}}},
			 {{"DuplicateAccount"}, {{"code", PropertyValue(std::string("shared"))}}}});
	ASSERT_EQ(accounts.size(), 4U);
	const std::vector<uint64_t> badEdge = backend.createLogicalEdges(
			{{accounts[0], accounts[1], "ExistingReport", {{"weight", PropertyValue(std::string("invalid"))}}}});
	ASSERT_EQ(badEdge.size(), 1U);

	const LogicalGraphConstraintDefinition nodeKey =
			backend.createLogicalConstraint({"existing_account_key",
											 LogicalGraphConstraintEntityKind::LGCEK_NODE,
											 LogicalGraphConstraintKind::LGCK_NODE_KEY,
											 "ExistingAccount",
											 {"tenant", "email"},
											 LogicalGraphConstraintPropertyType::LGCPT_NONE});
	EXPECT_THROW(
			static_cast<void>(backend.createLogicalConstraint({"existing_report_weight",
															   LogicalGraphConstraintEntityKind::LGCEK_EDGE,
															   LogicalGraphConstraintKind::LGCK_PROPERTY_TYPE,
															   "ExistingReport",
															   {"weight"},
															   LogicalGraphConstraintPropertyType::LGCPT_INTEGER})),
			std::runtime_error);
	EXPECT_THROW(static_cast<void>(backend.createLogicalConstraint({"duplicate_account_code",
																	LogicalGraphConstraintEntityKind::LGCEK_NODE,
																	LogicalGraphConstraintKind::LGCK_UNIQUE,
																	"DuplicateAccount",
																	{"code"},
																	LogicalGraphConstraintPropertyType::LGCPT_NONE})),
				 std::runtime_error);
	EXPECT_EQ(backend.listLogicalConstraints(8U), std::vector<LogicalGraphConstraintDefinition>{nodeKey});

	backend.deleteLogicalEdges({{badEdge.front()}});
	backend.deleteLogicalNodes({{accounts[3], false, 0U}});
	const LogicalGraphConstraintDefinition edgeType =
			backend.createLogicalConstraint({"existing_report_weight",
											 LogicalGraphConstraintEntityKind::LGCEK_EDGE,
											 LogicalGraphConstraintKind::LGCK_PROPERTY_TYPE,
											 "ExistingReport",
											 {"weight"},
											 LogicalGraphConstraintPropertyType::LGCPT_INTEGER});
	const LogicalGraphConstraintDefinition duplicateUnique =
			backend.createLogicalConstraint({"duplicate_account_code",
											 LogicalGraphConstraintEntityKind::LGCEK_NODE,
											 LogicalGraphConstraintKind::LGCK_UNIQUE,
											 "DuplicateAccount",
											 {"code"},
											 LogicalGraphConstraintPropertyType::LGCPT_NONE});
	EXPECT_EQ(backend.listLogicalConstraints(8U),
			  (std::vector<LogicalGraphConstraintDefinition>{nodeKey, edgeType, duplicateUnique}));
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, TrainsVectorIndexesThroughTheDurableLogicalBackendBoundary) {
	static_cast<void>(materializeNativeGraphFixture());

	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const auto vectorValue = [](double first, double second) {
		return PropertyValue(std::vector<PropertyValue>{PropertyValue(first), PropertyValue(second)});
	};
	static_cast<void>(backend.createLogicalNodes({{{"TrainingVector"}, {{"embedding", vectorValue(0.0, 0.0)}}},
												  {{"TrainingVector"}, {{"embedding", vectorValue(0.0, 1.0)}}},
												  {{"TrainingVector"}, {{"embedding", vectorValue(3.0, 3.0)}}}}));
	const LogicalGraphIndexDefinition index =
			backend.createLogicalNodeVectorIndex({"training_embedding", "TrainingVector", "embedding", 2U, "l2"});
	ASSERT_EQ(index.kind, LogicalGraphIndexKind::LGIK_VECTOR);

	EXPECT_THROW(static_cast<void>(backend.trainLogicalVector({"training_embedding", 0U, 16U})), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(backend.trainLogicalVector({"missing_index", 16U, 16U})), std::runtime_error);
	const LogicalGraphVectorTrainingResult trained = backend.trainLogicalVector({"training_embedding", 16U, 16U});
	EXPECT_TRUE(trained.trained);
	EXPECT_FALSE(trained.reused_existing_generation);
	EXPECT_EQ(trained.vector_count, 3U);
	const LogicalGraphVectorTrainingResult reused = backend.trainLogicalVector({"training_embedding", 16U, 16U});
	EXPECT_TRUE(reused.trained);
	EXPECT_TRUE(reused.reused_existing_generation);
	EXPECT_EQ(reused.vector_count, trained.vector_count);

	static_cast<void>(backend.createLogicalNodeVectorIndex({"empty_embedding", "NoVectors", "embedding", 2U, "l2"}));
	const LogicalGraphVectorTrainingResult empty = backend.trainLogicalVector({"empty_embedding", 16U, 16U});
	EXPECT_FALSE(empty.trained);
	EXPECT_EQ(empty.vector_count, 0U);
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();

	RelocatableStorageBackendOptions reopenOptions = backendOptions();
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend reopened(std::move(reopenOptions));
	reopened.open();
	const LogicalGraphVectorTrainingResult afterReopen = reopened.trainLogicalVector({"training_embedding", 16U, 16U});
	EXPECT_TRUE(afterReopen.trained);
	EXPECT_TRUE(afterReopen.reused_existing_generation);
	EXPECT_EQ(afterReopen.vector_count, trained.vector_count);
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest, CypherLogicalTransactionControlUsesTheReadYourWritesOverlay) {
	static_cast<void>(materializeNativeGraphFixture());

	zyx::Database database(data_path_.string());
	database.open();
	zyx::Result begin = database.execute("BEGIN");
	ASSERT_TRUE(begin.isSuccess()) << begin.getError();
	ASSERT_TRUE(database.execute("CREATE (n:CypherTxn {name: 'Ada'})").isSuccess());
	zyx::Result staged = database.execute("MATCH (n:CypherTxn) RETURN n.name AS name");
	ASSERT_TRUE(staged.isSuccess()) << staged.getError();
	ASSERT_TRUE(staged.hasNext());
	staged.next();
	EXPECT_EQ(std::get<std::string>(staged.get("name")), "Ada");
	EXPECT_TRUE(database.execute("ROLLBACK").isSuccess());
	zyx::Result rolledBack = database.execute("MATCH (n:CypherTxn) RETURN n");
	ASSERT_TRUE(rolledBack.isSuccess()) << rolledBack.getError();
	EXPECT_FALSE(rolledBack.hasNext());

	ASSERT_TRUE(database.execute("BEGIN").isSuccess());
	ASSERT_TRUE(database.execute("CREATE (n:CypherTxn {name: 'Grace'})").isSuccess());
	ASSERT_TRUE(database.execute("COMMIT").isSuccess());
	zyx::Result committed = database.execute("MATCH (n:CypherTxn) RETURN n.name AS name");
	ASSERT_TRUE(committed.isSuccess()) << committed.getError();
	ASSERT_TRUE(committed.hasNext());
	committed.next();
	EXPECT_EQ(std::get<std::string>(committed.get("name")), "Grace");
	database.close();
}

TEST_F(RelocatableStorageBackendTest, DriverAbiLogicalTransactionProvidesReadYourWritesAndRollback) {
	static_cast<void>(materializeNativeGraphFixture());

	zyx_driver_db_t *database = nullptr;
	zyx_driver_txn_t *transaction = nullptr;
	zyx_driver_result_t *result = nullptr;
	zyx_driver_error_t *error = nullptr;
	ASSERT_EQ(zyx_driver_db_open(data_path_.string().c_str(), &database, &error), ZYX_DRIVER_OK);
	ASSERT_NE(database, nullptr);
	ASSERT_EQ(zyx_driver_txn_begin(database, &transaction, &error), ZYX_DRIVER_OK);
	ASSERT_NE(transaction, nullptr);
	ASSERT_EQ(zyx_driver_txn_execute(transaction, "CREATE (n:DriverTxn {name: 'Ada'})", nullptr, &result, &error),
			  ZYX_DRIVER_OK);
	zyx_driver_result_free(result);
	result = nullptr;
	ASSERT_EQ(zyx_driver_txn_execute(transaction, "MATCH (n:DriverTxn) RETURN n", nullptr, &result, &error),
			  ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_result_next(result, &error), ZYX_DRIVER_ROW);
	int64_t nodeId = 0;
	EXPECT_EQ(zyx_driver_result_get_node_id(result, 0U, &nodeId, &error), ZYX_DRIVER_OK);
	EXPECT_GT(nodeId, 0);
	zyx_driver_result_free(result);
	result = nullptr;
	ASSERT_EQ(zyx_driver_txn_rollback(transaction, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_txn_close(transaction, &error), ZYX_DRIVER_OK);
	transaction = nullptr;
	ASSERT_EQ(zyx_driver_db_execute(database, "MATCH (n:DriverTxn) RETURN n", nullptr, &result, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_result_next(result, &error), ZYX_DRIVER_DONE);
	zyx_driver_result_free(result);
	ASSERT_EQ(zyx_driver_db_close(database, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(error, nullptr);
}

TEST_F(RelocatableStorageBackendTest, PublicLogicalReadOnlyTransactionAndStaleCommitFailClosed) {
	static_cast<void>(materializeNativeGraphFixture());

	zyx::Database database(data_path_.string());
	database.open();
	ASSERT_TRUE(database.execute("CREATE (:ReadOnlySeed {name: 'Ada'})").isSuccess());
	auto readOnly = database.beginReadOnlyTransaction();
	ASSERT_TRUE(readOnly.isReadOnly());
	zyx::Result read = readOnly.execute("MATCH (n:ReadOnlySeed) RETURN n.name AS name");
	ASSERT_TRUE(read.isSuccess()) << read.getError();
	ASSERT_TRUE(read.hasNext());
	read.next();
	EXPECT_EQ(std::get<std::string>(read.get("name")), "Ada");
	zyx::Result forbidden = readOnly.execute("CREATE (:ReadOnlyForbidden)");
	EXPECT_FALSE(forbidden.isSuccess());
	EXPECT_NE(forbidden.getError().find("Read-only transaction"), std::string::npos);
	readOnly.commit();

	auto stale = database.beginTransaction();
	ASSERT_TRUE(stale.execute("CREATE (:StagedOnly)").isSuccess());
	ASSERT_TRUE(database.execute("CREATE (:ConcurrentWriter)").isSuccess());
	EXPECT_THROW(stale.commit(), graph::storage::LogicalGraphMutationConflict);
	EXPECT_FALSE(stale.isActive());
	EXPECT_FALSE(database.hasActiveTransaction());
	zyx::Result stagedCheck = database.execute("MATCH (n:StagedOnly) RETURN n");
	ASSERT_TRUE(stagedCheck.isSuccess()) << stagedCheck.getError();
	EXPECT_FALSE(stagedCheck.hasNext());
	zyx::Result concurrentCheck = database.execute("MATCH (n:ConcurrentWriter) RETURN n");
	ASSERT_TRUE(concurrentCheck.isSuccess()) << concurrentCheck.getError();
	EXPECT_TRUE(concurrentCheck.hasNext());
	database.close();
}

TEST_F(RelocatableStorageBackendTest, ReadOnlyOpenRecoversDurableInterruptedPublicationWithoutCreatingSidecars) {
	materializeNativeDatabase();
	publishInterruptedGeneration();
	const uintmax_t bytesBeforeOpen = std::filesystem::file_size(data_path_);
	EXPECT_FALSE(std::filesystem::exists(data_path_.string() + ".wal"));
	RelocatableStorageBackend backend(backendOptions());
	EXPECT_NO_THROW(backend.open());
	EXPECT_TRUE(backend.isReadOnly());
	backend.close();
	EXPECT_EQ(std::filesystem::file_size(data_path_), bytesBeforeOpen);
	EXPECT_FALSE(std::filesystem::exists(data_path_.string() + ".wal"));
}

TEST_F(RelocatableStorageBackendTest,
       FinalGraphPublicationSyncFailureRequiresReopenBeforeAnotherWriteAndRecoversTheCompleteGeneration) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();

	std::atomic<uint32_t> inactiveSuperblockWrites{0U};
	std::atomic<uint32_t> syncCalls{0U};
	{
		StorageIO::ScopedNativeWriteHookForTesting observeInactiveSuperblock(
				[&inactiveSuperblockWrites](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (byteCount == RSS_PAGE_SIZE &&
						(offset == SuperblockFile::slotOffset(0U) || offset == SuperblockFile::slotOffset(1U))) {
						inactiveSuperblockWrites.fetch_add(1U, std::memory_order_relaxed);
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		StorageIO::ScopedNativeSyncHookForTesting failFinalPublicationFence(
				[&inactiveSuperblockWrites, &syncCalls](file_handle_t fd) {
					syncCalls.fetch_add(1U, std::memory_order_relaxed);
					// The first inactive Superblock reserves durable logical IDs. Fail
					// only after the Graph batch writes its own root publication.
					if (inactiveSuperblockWrites.load(std::memory_order_relaxed) >= 2U) {
#ifdef _WIN32
						SetLastError(ERROR_WRITE_FAULT);
#else
						errno = EIO;
#endif
						return -1;
					}
					return portable_fsync(fd);
				});
		EXPECT_THROW(static_cast<void>(backend.createLogicalNodes({{{"PublishedBeforeFenceFailure"}, {}}})),
				 std::runtime_error);
	}
	EXPECT_GE(inactiveSuperblockWrites.load(std::memory_order_relaxed), 2U);
	EXPECT_GE(syncCalls.load(std::memory_order_relaxed), 2U);
	EXPECT_THROW(static_cast<void>(backend.createLogicalNodes({{{"MustReopenFirst"}, {}}})), std::logic_error);
	backend.close();

	RelocatableStorageBackendOptions reopenOptions = backendOptions();
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend reopened(std::move(reopenOptions));
	reopened.open();
	RelocatableReadSnapshot recovered = reopened.openSnapshot();
	std::vector<std::string> labels;
	reopened.visitNodes(recovered, 8U, [&labels](const LogicalGraphNode &node) {
		labels.insert(labels.end(), node.labels.begin(), node.labels.end());
	});
	EXPECT_EQ(labels, (std::vector<std::string>{"PublishedBeforeFenceFailure"}));
	recovered.release();
	EXPECT_EQ(reopened.createLogicalNodes({{{"WritableAfterReopen"}, {}}}).size(), 1U);
	reopened.close();
}
