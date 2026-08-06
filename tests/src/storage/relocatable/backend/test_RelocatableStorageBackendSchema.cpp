/**
 * @file test_RelocatableStorageBackendSchema.cpp
 * @brief End-to-end online scalar-index and Vector DDL tests.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "storage/relocatable/backend/RelocatableStorageBackendSchemaTestFixture.hpp"

TEST_F(RelocatableStorageBackendSchemaTest, BuildsHistoricalScalarEntriesPublishesDefinitionsAndDropsThem) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::vector<uint64_t> nodes =
			backend.createLogicalNodes({{{"Person"}, {{"score", graph::PropertyValue(int64_t{42})}}}});
	ASSERT_EQ(nodes.size(), 1U);

	const LogicalGraphIndexDefinition created = backend.createLogicalNodeIndex({"person_score", "Person", {"score"}});
	EXPECT_NE(created.index_id, 0U);
	EXPECT_EQ(created.kind, LogicalGraphIndexKind::LGIK_PROPERTY);
	EXPECT_EQ(backend.listLogicalIndexes(8U), std::vector<LogicalGraphIndexDefinition>{created});
	RelocatableReadSnapshot snapshot = backend.openSnapshot();
	EXPECT_EQ(backend.readIndexDefinitions(snapshot, 8U).size(), 1U);
	snapshot.release();
	const std::vector<uint64_t> laterNodes =
			backend.createLogicalNodes({{{"Person"}, {{"score", graph::PropertyValue(int64_t{7})}}}});
	ASSERT_EQ(laterNodes.size(), 1U);
	backend.close();

	const std::vector<BTreePageEntry> entries = readIndexEntries(created.index_id);
	ASSERT_EQ(entries.size(), 2U);
	EXPECT_EQ(entries[0],
			  (BTreePageEntry{IndexKeyCodec::encodeEntryKey(graph::PropertyValue(int64_t{7}), laterNodes.front()),
							  laterNodes.front()}));
	EXPECT_EQ(entries[1],
			  (BTreePageEntry{IndexKeyCodec::encodeEntryKey(graph::PropertyValue(int64_t{42}), nodes.front()),
							  nodes.front()}));

	RelocatableStorageBackend reopened(options());
	reopened.open();
	EXPECT_EQ(reopened.listLogicalIndexes(8U), std::vector<LogicalGraphIndexDefinition>{created});
	EXPECT_TRUE(reopened.dropLogicalIndexByName(created.name));
	EXPECT_FALSE(reopened.dropLogicalIndexByName(created.name));
	EXPECT_TRUE(reopened.listLogicalIndexes(8U).empty());
	reopened.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, DerivesStableNamesForUnnamedScalarAndVectorIndexes) {
	auto vectorValue = [](double first, double second) {
		return graph::PropertyValue(
				std::vector<graph::PropertyValue>{graph::PropertyValue(first), graph::PropertyValue(second)});
	};
	RelocatableStorageBackend backend(options());
	backend.open();
	static_cast<void>(backend.createLogicalNodes(
			{{{"Person"}, {{"score", graph::PropertyValue(int64_t{42})}, {"embedding", vectorValue(1.0, -2.0)}}}}));

	const LogicalGraphIndexDefinition scalar = backend.createLogicalNodeIndex({"", "Person", {"score"}});
	EXPECT_EQ(scalar.name, "index_node_6_Person_5_score");
	const LogicalGraphIndexDefinition vector =
			backend.createLogicalNodeVectorIndex({"", "Person", "embedding", 2U, "INNER_PRODUCT"});
	EXPECT_EQ(vector.name, "index_vector_node_6_Person_9_embedding_2_ip");
	EXPECT_EQ(vector.vector_metric, "ip");
	EXPECT_EQ(backend.listLogicalIndexes(8U), (std::vector<LogicalGraphIndexDefinition>{scalar, vector}));
	backend.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, RejectsStaleSemanticManifestBeforeVectorTraining) {
	auto vectorValue = [](double first, double second) {
		return graph::PropertyValue(
				std::vector<graph::PropertyValue>{graph::PropertyValue(first), graph::PropertyValue(second)});
	};
	RelocatableStorageBackend writer(options());
	writer.open();
	static_cast<void>(writer.createLogicalNodes(
			{{{"Person"}, {{"embedding", vectorValue(1.0, -2.0)}}}}));
	const LogicalGraphIndexDefinition index =
			writer.createLogicalNodeVectorIndex({"person_embedding", "Person", "embedding", 2U, "l2"});
	writer.close();

	VectorManifest stale = readVectorManifest(index.index_id);
	stale.node_mapping_root_generation = Generation(stale.node_mapping_root_generation.value() + 1U);
	overwriteVectorManifest(stale);

	RelocatableStorageBackend corrupted(options());
	corrupted.open();
	EXPECT_THROW(static_cast<void>(corrupted.trainLogicalVector({"person_embedding", 16U, 16U})), FormatException);
	corrupted.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, MaintainsUniqueConstraintSupportTreeWithoutPublishingPublicIndexMetadata) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const LogicalGraphConstraintDefinition constraint =
			backend.createLogicalConstraint({"user_email_unique",
											 LogicalGraphConstraintEntityKind::LGCEK_NODE,
											 LogicalGraphConstraintKind::LGCK_UNIQUE,
											 "User",
											 {"email"},
											 LogicalGraphConstraintPropertyType::LGCPT_NONE});
	ASSERT_TRUE(isSystemCatalogConstraintDefinitionId(constraint.constraint_id));
	EXPECT_TRUE(backend.listLogicalIndexes(8U).empty());

	const std::vector<uint64_t> first =
			backend.createLogicalNodes({{{"User"}, {{"email", graph::PropertyValue(std::string("a@example.test"))}}}});
	ASSERT_EQ(first.size(), 1U);
	backend.deleteLogicalNodes({{first.front(), false, 0U}});
	const std::vector<uint64_t> replacement =
			backend.createLogicalNodes({{{"User"}, {{"email", graph::PropertyValue(std::string("a@example.test"))}}}});
	ASSERT_EQ(replacement.size(), 1U);
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();

	const std::vector<BTreePageEntry> entries = readIndexEntries(constraint.constraint_id);
	ASSERT_EQ(entries.size(), 1U);
	const std::vector<graph::PropertyValue> values = {graph::PropertyValue(std::string("a@example.test"))};
	const std::vector<std::byte> canonical = ConstraintSupportTree::canonicalKey(values);
	EXPECT_EQ(entries.front(),
			  (BTreePageEntry{ConstraintSupportTree::entryKey(canonical, replacement.front()), replacement.front()}));
}

TEST_F(RelocatableStorageBackendSchemaTest, ResolvesExactUniqueNodeLookupsThroughThePinnedSupportTree) {
	RelocatableStorageBackend backend(options());
	backend.open();
	static_cast<void>(backend.createLogicalConstraint({"user_email_unique",
													   LogicalGraphConstraintEntityKind::LGCEK_NODE,
													   LogicalGraphConstraintKind::LGCK_UNIQUE,
													   "User",
													   {"email"},
													   LogicalGraphConstraintPropertyType::LGCPT_NONE}));
	const std::vector<uint64_t> nodes =
			backend.createLogicalNodes({{{"User"},
										 {{"email", graph::PropertyValue(std::string("ada@example.test"))},
										  {"payload", graph::PropertyValue(std::string("large retained text"))}}}});
	ASSERT_EQ(nodes.size(), 1U);

	const auto lookup = [&backend](const std::string &email) {
		const std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
		return snapshot->findNodesByExactProperties({"User"}, {{"email", graph::PropertyValue(email)}}, 1U, 2U);
	};
	const auto matched = lookup("ada@example.test");
	ASSERT_TRUE(matched.has_value());
	ASSERT_EQ(matched->size(), 1U);
	EXPECT_EQ(matched->front().node_id, nodes.front());
	const auto missing = lookup("missing@example.test");
	ASSERT_TRUE(missing.has_value());
	EXPECT_TRUE(missing->empty());

	{
		const std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
		EXPECT_EQ(snapshot->findNodesByExactProperties(
				  {"User"}, {{"payload", graph::PropertyValue(std::string("large retained text"))}}, 1U, 2U),
			  std::nullopt);
	}

	backend.replaceLogicalNodeProperties(
			{{nodes.front(),
			  {{"email", graph::PropertyValue(std::string("grace@example.test"))},
			   {"payload", graph::PropertyValue(std::string("replacement retained text"))}}}});
	const auto oldKey = lookup("ada@example.test");
	ASSERT_TRUE(oldKey.has_value());
	EXPECT_TRUE(oldKey->empty());
	const auto updated = lookup("grace@example.test");
	ASSERT_TRUE(updated.has_value());
	ASSERT_EQ(updated->size(), 1U);
	EXPECT_EQ(updated->front().node_id, nodes.front());

	backend.deleteLogicalNodes({{nodes.front(), false, 0U}});
	const auto deleted = lookup("grace@example.test");
	ASSERT_TRUE(deleted.has_value());
	EXPECT_TRUE(deleted->empty());
	backend.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, RejectsPersistedUniqueSupportTreeEntriesWithMismatchedEntityValues) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const LogicalGraphConstraintDefinition constraint =
			backend.createLogicalConstraint({"user_email_unique",
											 LogicalGraphConstraintEntityKind::LGCEK_NODE,
											 LogicalGraphConstraintKind::LGCK_UNIQUE,
											 "User",
											 {"email"},
											 LogicalGraphConstraintPropertyType::LGCPT_NONE});
	const std::vector<uint64_t> nodes =
			backend.createLogicalNodes({{{"User"}, {{"email", graph::PropertyValue(std::string("ada@example.test"))}}}});
	ASSERT_EQ(nodes.size(), 1U);
	backend.close();

	const std::vector<graph::PropertyValue> values = {graph::PropertyValue(std::string("ada@example.test"))};
	const std::vector<std::byte> key = ConstraintSupportTree::entryKey(ConstraintSupportTree::canonicalKey(values), nodes.front());
	// Preserve a valid B+Tree page encoding while modelling a stale support-tree payload.
	overwriteBTreeLeafEntry(constraint.constraint_id, key, {key, nodes.front() + 1U});

	RelocatableStorageBackend corrupted(options());
	corrupted.open();
	RelocatableReadSnapshot snapshot = corrupted.openSnapshot();
	EXPECT_THROW(static_cast<void>(corrupted.findNodesByExactProperties(
				snapshot, {"User"}, {{"email", graph::PropertyValue(std::string("ada@example.test"))}}, 1U, 2U)),
			 FormatException);
	snapshot.release();
	EXPECT_THROW(static_cast<void>(corrupted.verifyIntegrity()), FormatException);
	corrupted.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, PermitsAtomicUniqueKeySwapAndPublishesOnlyTheFinalSupportTreeState) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const LogicalGraphConstraintDefinition constraint =
			backend.createLogicalConstraint({"user_email_unique",
											 LogicalGraphConstraintEntityKind::LGCEK_NODE,
											 LogicalGraphConstraintKind::LGCK_UNIQUE,
											 "User",
											 {"email"},
											 LogicalGraphConstraintPropertyType::LGCPT_NONE});
	const std::vector<uint64_t> nodes =
			backend.createLogicalNodes({{{"User"}, {{"email", graph::PropertyValue(std::string("a@example.test"))}}},
										{{"User"}, {{"email", graph::PropertyValue(std::string("b@example.test"))}}}});
	ASSERT_EQ(nodes.size(), 2U);

	LogicalGraphMutationBatch swap;
	swap.property_replacements = {
			{{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, nodes[0]},
			 {{"email", graph::PropertyValue(std::string("b@example.test"))}}},
			{{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, nodes[1]},
			 {{"email", graph::PropertyValue(std::string("a@example.test"))}}},
	};
	EXPECT_NO_THROW(static_cast<void>(backend.executeLogicalGraphMutationBatch(swap)));
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();

	const std::vector<graph::PropertyValue> a = {graph::PropertyValue(std::string("a@example.test"))};
	const std::vector<graph::PropertyValue> b = {graph::PropertyValue(std::string("b@example.test"))};
	std::vector<BTreePageEntry> expected = {
			{ConstraintSupportTree::entryKey(ConstraintSupportTree::canonicalKey(a), nodes[1]), nodes[1]},
			{ConstraintSupportTree::entryKey(ConstraintSupportTree::canonicalKey(b), nodes[0]), nodes[0]},
	};
	std::ranges::sort(expected, {}, &BTreePageEntry::key);
	EXPECT_EQ(readIndexEntries(constraint.constraint_id), expected);
}

TEST_F(RelocatableStorageBackendSchemaTest, DetachDeleteErasesEdgeUniqueSupportEntryInTheSamePublication) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::vector<uint64_t> firstNodes = backend.createLogicalNodes({{{"User"}, {}}, {{"User"}, {}}});
	ASSERT_EQ(firstNodes.size(), 2U);
	const LogicalGraphConstraintDefinition constraint =
			backend.createLogicalConstraint({"knows_code_unique",
											 LogicalGraphConstraintEntityKind::LGCEK_EDGE,
											 LogicalGraphConstraintKind::LGCK_UNIQUE,
											 "KNOWS",
											 {"code"},
											 LogicalGraphConstraintPropertyType::LGCPT_NONE});
	const std::vector<uint64_t> firstEdge = backend.createLogicalEdges(
			{{firstNodes[0], firstNodes[1], "KNOWS", {{"code", graph::PropertyValue(std::string("edge-code"))}}}});
	ASSERT_EQ(firstEdge.size(), 1U);
	backend.deleteLogicalNodes({{firstNodes[0], true, 4U}});

	const std::vector<uint64_t> laterNodes = backend.createLogicalNodes({{{"User"}, {}}, {{"User"}, {}}});
	ASSERT_EQ(laterNodes.size(), 2U);
	const std::vector<uint64_t> replacement = backend.createLogicalEdges(
			{{laterNodes[0], laterNodes[1], "KNOWS", {{"code", graph::PropertyValue(std::string("edge-code"))}}}});
	ASSERT_EQ(replacement.size(), 1U);
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();

	const std::vector<graph::PropertyValue> values = {graph::PropertyValue(std::string("edge-code"))};
	const std::vector<std::byte> canonical = ConstraintSupportTree::canonicalKey(values);
	EXPECT_EQ(readIndexEntries(constraint.constraint_id),
			  (std::vector<BTreePageEntry>{
					  {ConstraintSupportTree::entryKey(canonical, replacement.front()), replacement.front()}}));
}

TEST_F(RelocatableStorageBackendSchemaTest, NamedRelationshipConstraintsExcludeUntypedRelationships) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes({{{"User"}, {}}, {{"User"}, {}}});
	ASSERT_EQ(nodes.size(), 2U);

	// Creating the constraint after this edge exercises full-scan validation;
	// subsequent creates exercise the affected-entity validation path.
	const std::vector<uint64_t> firstUntyped = backend.createLogicalEdges(
			{{nodes[0], nodes[1], "", {{"code", graph::PropertyValue(std::string("shared-code"))}}}});
	ASSERT_EQ(firstUntyped.size(), 1U);
	static_cast<void>(backend.createLogicalConstraint({"knows_code_unique",
													   LogicalGraphConstraintEntityKind::LGCEK_EDGE,
													   LogicalGraphConstraintKind::LGCK_UNIQUE,
													   "KNOWS",
													   {"code"},
													   LogicalGraphConstraintPropertyType::LGCPT_NONE}));

	const std::vector<uint64_t> secondUntyped = backend.createLogicalEdges(
			{{nodes[0], nodes[1], "", {{"code", graph::PropertyValue(std::string("shared-code"))}}}});
	ASSERT_EQ(secondUntyped.size(), 1U);
	const std::vector<uint64_t> named = backend.createLogicalEdges(
			{{nodes[0], nodes[1], "KNOWS", {{"code", graph::PropertyValue(std::string("shared-code"))}}}});
	ASSERT_EQ(named.size(), 1U);
	EXPECT_THROW(
			static_cast<void>(backend.createLogicalEdges(
					{{nodes[0], nodes[1], "KNOWS", {{"code", graph::PropertyValue(std::string("shared-code"))}}}})),
			std::runtime_error);

	RelocatableReadSnapshot snapshot = backend.openSnapshot();
	EXPECT_TRUE(backend.readEdge(snapshot, firstUntyped.front()).type.empty());
	EXPECT_TRUE(backend.readEdge(snapshot, secondUntyped.front()).type.empty());
	EXPECT_EQ(backend.readEdge(snapshot, named.front()).type, "KNOWS");
	snapshot.release();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, PublishesScalarIndexBatchesOnlyAfterEveryHistoricalTreeBuilds) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes(
			{{{"Person"}, {{"score", graph::PropertyValue(int64_t{42})}, {"rank", graph::PropertyValue(int64_t{7})}}},
			 {{"Person"}, {{"rank", graph::PropertyValue(int64_t{9})}}}});
	ASSERT_EQ(nodes.size(), 2U);

	const std::vector<LogicalGraphNodeIndexCreateRequest> requests = {{"person_score", "Person", {"score"}},
																	  {"person_rank", "Person", {"rank"}}};
	const std::vector<LogicalGraphIndexDefinition> created = backend.createLogicalNodeIndexes(requests);
	ASSERT_EQ(created.size(), 2U);
	EXPECT_LT(created[0].index_id, created[1].index_id);
	EXPECT_EQ(created[0].kind, LogicalGraphIndexKind::LGIK_PROPERTY);
	EXPECT_EQ(created[1].kind, LogicalGraphIndexKind::LGIK_PROPERTY);
	EXPECT_EQ(backend.listLogicalIndexes(8U), created);
	backend.close();

	const std::vector<BTreePageEntry> scoreEntries = readIndexEntries(created[0].index_id);
	ASSERT_EQ(scoreEntries.size(), 1U);
	EXPECT_EQ(scoreEntries.front(),
			  (BTreePageEntry{IndexKeyCodec::encodeEntryKey(graph::PropertyValue(int64_t{42}), nodes.front()),
							  nodes.front()}));
	const std::vector<BTreePageEntry> rankEntries = readIndexEntries(created[1].index_id);
	ASSERT_EQ(rankEntries.size(), 2U);

	RelocatableStorageBackend reopened(options());
	reopened.open();
	EXPECT_EQ(reopened.listLogicalIndexes(8U), created);
	reopened.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, RejectsFailedScalarIndexBatchWithoutPublishingAPartialDefinitionSet) {
	RelocatableStorageBackend backend(options());
	backend.open();
	static_cast<void>(backend.createLogicalNodes(
			{{{"Person"}, {{"score", graph::PropertyValue(int64_t{42})}, {"rank", graph::PropertyValue(int64_t{7})}}},
			 {{"Person"}, {{"rank", graph::PropertyValue(int64_t{9})}}}}));
	backend.close();

	RelocatableStorageBackendOptions constrainedOptions = options();
	constrainedOptions.maximum_online_index_entries = 1U;
	RelocatableStorageBackend constrained(constrainedOptions);
	constrained.open();
	const std::vector<LogicalGraphNodeIndexCreateRequest> requests = {{"person_score", "Person", {"score"}},
																	  {"person_rank", "Person", {"rank"}}};
	EXPECT_THROW(static_cast<void>(constrained.createLogicalNodeIndexes(requests)), FormatException);
	EXPECT_TRUE(constrained.listLogicalIndexes(8U).empty());
	constrained.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, RejectsInvalidOrExistingScalarIndexBatchesBeforePublication) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::vector<LogicalGraphNodeIndexCreateRequest> duplicateSchema = {
			{"person_score_first", "Person", {"score"}}, {"person_score_second", "Person", {"score"}}};
	EXPECT_THROW(static_cast<void>(backend.createLogicalNodeIndexes(duplicateSchema)), std::invalid_argument);
	const std::vector<LogicalGraphNodeIndexCreateRequest> duplicateProperty = {
			{"person_score_pair", "Person", {"score", "score"}}};
	EXPECT_THROW(static_cast<void>(backend.createLogicalNodeIndexes(duplicateProperty)), std::invalid_argument);
	EXPECT_TRUE(backend.listLogicalIndexes(8U).empty());

	const LogicalGraphIndexDefinition original = backend.createLogicalNodeIndex({"person_score", "Person", {"score"}});
	const std::vector<LogicalGraphNodeIndexCreateRequest> conflictsWithExisting = {
			{"person_rank", "Person", {"rank"}}, {"person_score_again", "Person", {"score"}}};
	EXPECT_THROW(static_cast<void>(backend.createLogicalNodeIndexes(conflictsWithExisting)), FormatException);
	EXPECT_EQ(backend.listLogicalIndexes(8U), std::vector<LogicalGraphIndexDefinition>{original});
	backend.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, BuildsHistoricalVectorStateAndPublishesItsDefinitionAtomically) {
	auto vectorValue = [](double first, double second) {
		return graph::PropertyValue(
				std::vector<graph::PropertyValue>{graph::PropertyValue(first), graph::PropertyValue(second)});
	};
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::vector<uint64_t> nodes =
			backend.createLogicalNodes({{{"Person"}, {{"embedding", vectorValue(1.0, -2.0)}}},
										{{"Other"}, {{"embedding", vectorValue(5.0, 8.0)}}}});
	ASSERT_EQ(nodes.size(), 2U);

	const LogicalGraphIndexDefinition created =
			backend.createLogicalNodeVectorIndex({"person_embedding", "Person", "embedding", 2U, "INNER_PRODUCT"});
	EXPECT_GE(created.index_id, RSS_SYSTEM_CATALOG_INDEX_ID_FIRST);
	EXPECT_EQ(created.kind, LogicalGraphIndexKind::LGIK_VECTOR);
	EXPECT_EQ(created.vector_dimensions, 2U);
	EXPECT_EQ(created.vector_metric, "ip");
	EXPECT_EQ(backend.listLogicalIndexes(8U), std::vector<LogicalGraphIndexDefinition>{created});

	const std::vector<uint64_t> later =
			backend.createLogicalNodes({{{"Person"}, {{"embedding", vectorValue(-3.0, 4.0)}}}});
	ASSERT_EQ(later.size(), 1U);
	backend.close();

	const std::vector<BTreePageEntry> mappings = readIndexEntries(created.index_id);
	ASSERT_EQ(mappings.size(), 2U);
	EXPECT_EQ(mappings[0], (BTreePageEntry{VectorSemanticCodec::nodeMappingKey(nodes.front()), 1U}));
	EXPECT_EQ(mappings[1], (BTreePageEntry{VectorSemanticCodec::nodeMappingKey(later.front()), 2U}));
	const VectorManifest manifest = readVectorManifest(created.index_id);
	EXPECT_EQ(manifest.index_id, created.index_id);
	EXPECT_EQ(manifest.dimensions, 2U);
	EXPECT_EQ(manifest.metric, VectorDistanceMetric::VDM_INNER_PRODUCT);
	EXPECT_TRUE(manifest.node_mapping_root_page_id.isValid());
	EXPECT_TRUE(manifest.raw_write_tail_page_id.isValid());

	RelocatableStorageBackend reopened(options());
	reopened.open();
	EXPECT_EQ(reopened.listLogicalIndexes(8U), std::vector<LogicalGraphIndexDefinition>{created});
	EXPECT_TRUE(reopened.dropLogicalIndexByName(created.name));
	EXPECT_FALSE(reopened.dropLogicalIndexByName(created.name));
	EXPECT_TRUE(reopened.listLogicalIndexes(8U).empty());
	const VectorIndexStateReclaimReport reclamation =
			reopened.reclaimObsoleteVectorIndexState(vectorStateReclaimOptions());
	EXPECT_EQ(reclamation.manifests.dropped_manifests_retired, 1U);
	EXPECT_GT(reclamation.raw.raw_pages_retired, 0U);
	EXPECT_EQ(reclamation.raw.vector_locations_erased, 2U);
	// A Vector index now owns two semantic B+Trees: NodeId-to-VectorId and
	// VectorId-to-NodeId. Dropping it retires both immutable roots together.
	EXPECT_EQ(reclamation.node_mappings.dropped_index_roots_retired, 2U);
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, SearchesSnapshotPinnedLogicalVectorStateWithBoundedExactRefinement) {
	auto vectorValue = [](double first, double second) {
		return graph::PropertyValue(
				std::vector<graph::PropertyValue>{graph::PropertyValue(first), graph::PropertyValue(second)});
	};
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::vector<uint64_t> nodes =
			backend.createLogicalNodes({{{"Person"}, {{"embedding", vectorValue(1.0, -2.0)}}},
										{{"Person"}, {{"embedding", vectorValue(-3.0, 4.0)}}}});
	ASSERT_EQ(nodes.size(), 2U);
	static_cast<void>(
			backend.createLogicalNodeVectorIndex({"person_embedding", "Person", "embedding", 2U, "inner_product"}));

	auto snapshot = backend.openLogicalGraphVectorSearchSnapshot();
	ASSERT_NE(snapshot, nullptr);
	LogicalGraphVectorSearchRequest request;
	request.index_name = "person_embedding";
	request.query_vector = {1.0F, -2.0F};
	request.top_k = 2U;
	request.maximum_mapping_entries = 2U;
	request.maximum_raw_vector_reads = 2U;
	const std::vector<LogicalGraphVectorSearchHit> hits = snapshot->searchLogicalVector(request).hits;
	ASSERT_EQ(hits.size(), 2U);
	EXPECT_EQ(hits[0].node_id, nodes[0]);
	EXPECT_EQ(hits[1].node_id, nodes[1]);
	EXPECT_LT(hits[0].score, hits[1].score);
	EXPECT_EQ(snapshot->readNode(hits.front().node_id).node_id, nodes.front());

	request.maximum_mapping_entries = 1U;
	EXPECT_THROW(static_cast<void>(snapshot->searchLogicalVector(request)), std::runtime_error);
	request.maximum_mapping_entries = 2U;
	request.maximum_raw_vector_reads = 1U;
	EXPECT_THROW(static_cast<void>(snapshot->searchLogicalVector(request)), std::runtime_error);
	snapshot.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, UsesPublishedAdjacencyCandidatesWithExactRawRefinement) {
	auto vectorValue = [](double first, double second) {
		return graph::PropertyValue(
				std::vector<graph::PropertyValue>{graph::PropertyValue(first), graph::PropertyValue(second)});
	};
	auto bfloatRecord = [](uint64_t vectorId, float first, float second) {
		CanonicalVectorRecord record;
		record.vector_id = vectorId;
		record.raw_bfloat16.resize(2U * sizeof(uint16_t));
		LittleEndianCodec::writeU16(record.raw_bfloat16, 0U,
									static_cast<uint16_t>(std::bit_cast<uint32_t>(first) >> 16U));
		LittleEndianCodec::writeU16(record.raw_bfloat16, sizeof(uint16_t),
									static_cast<uint16_t>(std::bit_cast<uint32_t>(second) >> 16U));
		return record;
	};
	RelocatableStorageBackend writer(options());
	writer.open();
	const std::vector<uint64_t> nodes =
			writer.createLogicalNodes({{{"Person"}, {{"embedding", vectorValue(0.0, 0.0)}}},
									   {{"Person"}, {{"embedding", vectorValue(0.0, 1.0)}}}});
	ASSERT_EQ(nodes.size(), 2U);
	const LogicalGraphIndexDefinition index =
			writer.createLogicalNodeVectorIndex({"person_embedding", "Person", "embedding", 2U, "l2"});
	writer.close();

	const std::vector<CanonicalVectorRecord> records = {bfloatRecord(1U, 0.0F, 0.0F), bfloatRecord(2U, 0.0F, 1.0F)};
	publishVectorAdjacency(index.index_id, records);

	auto backend = std::make_shared<RelocatableStorageBackend>(options());
	backend->open();
	graph::query::QueryEngine engine(backend, backend);
	const graph::query::QueryResult result = engine.execute(
			"CALL db.index.vector.queryNodes('person_embedding', 2, [0.0, 1.0]) YIELD node, score RETURN node, score");
	ASSERT_EQ(result.rowCount(), 2U);
	EXPECT_EQ(result.getRows()[0].at("node").asLogicalNode().node_id, nodes[1]);
	EXPECT_EQ(result.getRows()[1].at("node").asLogicalNode().node_id, nodes[0]);
	EXPECT_DOUBLE_EQ(std::get<double>(result.getRows()[0].at("score").asPrimitive().getVariant()), 0.0);
	EXPECT_DOUBLE_EQ(std::get<double>(result.getRows()[1].at("score").asPrimitive().getVariant()), 1.0);
	auto snapshot = backend->openLogicalGraphVectorSearchSnapshot();
	ASSERT_NE(snapshot, nullptr);
	LogicalGraphVectorSearchRequest bounded;
	bounded.index_name = index.name;
	bounded.query_vector = {0.0F, 1.0F};
	bounded.top_k = 1U;
	bounded.maximum_mapping_entries = 2U;
	bounded.maximum_raw_vector_reads = 2U;
	bounded.candidate_search_budget = 1U;
	const LogicalGraphVectorSearchResult budgetLimited = snapshot->searchLogicalVector(bounded);
	EXPECT_EQ(budgetLimited.hits.size(), 1U);
	EXPECT_EQ(budgetLimited.stats.candidate_discoveries, 2U);
	EXPECT_EQ(budgetLimited.stats.candidate_expansions, 1U);

	bounded.maximum_mapping_entries = 1U;
	bounded.maximum_raw_vector_reads = 1U;
	EXPECT_THROW(static_cast<void>(snapshot->searchLogicalVector(bounded)), std::runtime_error);
	// Keep the mapping ceiling above the frontier size so this exercises the
	// independent raw-record safety ceiling while expanding the first neighbor.
	bounded.maximum_mapping_entries = 2U;
	EXPECT_THROW(static_cast<void>(snapshot->searchLogicalVector(bounded)), std::runtime_error);
	snapshot.reset();
	backend->close();
}

TEST_F(RelocatableStorageBackendSchemaTest, RanksPersistentAdjacencyCandidatesWithGenerationCheckedPq) {
	auto vectorValue = [](double first, double second) {
		return graph::PropertyValue(
				std::vector<graph::PropertyValue>{graph::PropertyValue(first), graph::PropertyValue(second)});
	};
	auto bfloatRecord = [](uint64_t vectorId, float first, float second) {
		CanonicalVectorRecord record;
		record.vector_id = vectorId;
		record.raw_bfloat16.resize(2U * sizeof(uint16_t));
		LittleEndianCodec::writeU16(record.raw_bfloat16, 0U,
									static_cast<uint16_t>(std::bit_cast<uint32_t>(first) >> 16U));
		LittleEndianCodec::writeU16(record.raw_bfloat16, sizeof(uint16_t),
									static_cast<uint16_t>(std::bit_cast<uint32_t>(second) >> 16U));
		return record;
	};

	RelocatableStorageBackend writer(options());
	writer.open();
	const std::vector<uint64_t> nodes =
			writer.createLogicalNodes({{{"Person"}, {{"embedding", vectorValue(0.0, 0.0)}}},
									   {{"Person"}, {{"embedding", vectorValue(0.0, 1.0)}}},
									   {{"Person"}, {{"embedding", vectorValue(10.0, 10.0)}}},
									   {{"Person"}, {{"embedding", vectorValue(9.0, 10.0)}}}});
	ASSERT_EQ(nodes.size(), 4U);
	const LogicalGraphIndexDefinition index =
			writer.createLogicalNodeVectorIndex({"person_embedding", "Person", "embedding", 2U, "l2"});
	writer.close();

	const std::vector<CanonicalVectorRecord> records = {bfloatRecord(1U, 0.0F, 0.0F), bfloatRecord(2U, 0.0F, 1.0F),
														bfloatRecord(3U, 10.0F, 10.0F), bfloatRecord(4U, 9.0F, 10.0F)};
	publishVectorPq(index.index_id, records);
	publishVectorAdjacency(index.index_id, records, 3U);

	RelocatableStorageBackend backend(options());
	backend.open();
	auto snapshot = backend.openLogicalGraphVectorSearchSnapshot();
	ASSERT_NE(snapshot, nullptr);
	LogicalGraphVectorSearchRequest request;
	request.index_name = index.name;
	request.query_vector = {9.0F, 10.0F};
	request.top_k = 1U;
	request.maximum_mapping_entries = 8U;
	request.maximum_raw_vector_reads = 1U;
	request.candidate_search_budget = 1U;
	request.candidate_refinement_budget = 1U;
	const LogicalGraphVectorSearchResult search = snapshot->searchLogicalVector(request);
	const std::vector<LogicalGraphVectorSearchHit> &hits = search.hits;
	ASSERT_EQ(hits.size(), 1U);
	EXPECT_EQ(hits.front().node_id, nodes[3]);
	EXPECT_FLOAT_EQ(hits.front().score, 0.0F);
	EXPECT_TRUE(search.stats.used_adjacency);
	EXPECT_TRUE(search.stats.used_pq);
	EXPECT_EQ(search.stats.candidate_expansions, 1U);
	EXPECT_EQ(search.stats.candidate_discoveries, records.size());
	EXPECT_EQ(search.stats.pq_record_reads, records.size());
	EXPECT_EQ(search.stats.raw_vector_reads, 1U);
	request.mode = LogicalGraphVectorSearchMode::LGVSM_EXACT;
	request.maximum_raw_vector_reads = records.size();
	const LogicalGraphVectorSearchResult exact = snapshot->searchLogicalVector(request);
	ASSERT_EQ(exact.hits.size(), 1U);
	EXPECT_EQ(exact.hits.front(), hits.front());
	EXPECT_EQ(exact.stats.scanned_mapping_entries, records.size());
	EXPECT_EQ(exact.stats.raw_vector_reads, records.size());
	EXPECT_FALSE(exact.stats.used_adjacency);
	EXPECT_FALSE(exact.stats.used_pq);

	// Updating an indexed Vector must retain the immutable PQ/adjacency base for
	// pinned readers and add the replacement only through the current delta.
	backend.replaceLogicalNodeProperties({{nodes[3], {{"embedding", vectorValue(9.0, 11.0)}}}});
	auto currentSnapshot = backend.openLogicalGraphVectorSearchSnapshot();
	ASSERT_NE(currentSnapshot, nullptr);
	request.mode = LogicalGraphVectorSearchMode::LGVSM_AUTO;
	request.query_vector = {9.0F, 11.0F};
	request.maximum_mapping_entries = 8U;
	request.maximum_raw_vector_reads = 2U;
	request.candidate_search_budget = 1U;
	request.candidate_refinement_budget = 2U;
	const LogicalGraphVectorSearchResult current = currentSnapshot->searchLogicalVector(request);
	ASSERT_EQ(current.hits.size(), 1U);
	EXPECT_EQ(current.hits.front().node_id, nodes[3]);
	EXPECT_FLOAT_EQ(current.hits.front().score, 0.0F);
	EXPECT_TRUE(current.stats.used_adjacency);
	EXPECT_TRUE(current.stats.used_pq);
	EXPECT_TRUE(current.stats.used_delta);
	EXPECT_EQ(current.stats.scanned_delta_entries, 2U);

	// The removed base record remains physically present for the pinned reader,
	// but AUTO search must suppress it before looking up its now-erased semantic
	// mapping. Vector 3 and the replacement tie at distance one; stable Node-ID
	// ordering therefore makes the expected current result unambiguous.
	request.query_vector = {9.0F, 10.0F};
	const LogicalGraphVectorSearchResult tombstoneMerged = currentSnapshot->searchLogicalVector(request);
	ASSERT_EQ(tombstoneMerged.hits.size(), 1U);
	EXPECT_EQ(tombstoneMerged.hits.front().node_id, nodes[2]);
	EXPECT_FLOAT_EQ(tombstoneMerged.hits.front().score, 1.0F);
	EXPECT_TRUE(tombstoneMerged.stats.used_adjacency);
	EXPECT_TRUE(tombstoneMerged.stats.used_pq);
	EXPECT_TRUE(tombstoneMerged.stats.used_delta);

	// Replacing the base entry point is legal. The old adjacency generation no
	// longer has a valid traversal root, so AUTO search deliberately falls back
	// to the authoritative current semantic mapping rather than following it.
	backend.replaceLogicalNodeProperties({{nodes[0], {{"embedding", vectorValue(0.0, 0.25)}}}});
	auto entryPointTombstonedSnapshot = backend.openLogicalGraphVectorSearchSnapshot();
	ASSERT_NE(entryPointTombstonedSnapshot, nullptr);
	request.query_vector = {0.0F, 0.25F};
	request.maximum_raw_vector_reads = records.size();
	request.candidate_refinement_budget = records.size();
	const LogicalGraphVectorSearchResult entryPointTombstoned =
			entryPointTombstonedSnapshot->searchLogicalVector(request);
	ASSERT_EQ(entryPointTombstoned.hits.size(), 1U);
	EXPECT_EQ(entryPointTombstoned.hits.front().node_id, nodes[0]);
	EXPECT_FLOAT_EQ(entryPointTombstoned.hits.front().score, 0.0F);
	EXPECT_TRUE(entryPointTombstoned.stats.used_delta);
	EXPECT_EQ(entryPointTombstoned.stats.scanned_delta_entries, 4U);
	EXPECT_FALSE(entryPointTombstoned.stats.used_adjacency);
	EXPECT_FALSE(entryPointTombstoned.stats.used_pq);
	EXPECT_EQ(entryPointTombstoned.stats.scanned_mapping_entries, records.size());
	EXPECT_EQ(entryPointTombstoned.stats.raw_vector_reads, records.size());
	entryPointTombstonedSnapshot.reset();

	request.mode = LogicalGraphVectorSearchMode::LGVSM_EXACT;
	request.query_vector = {9.0F, 11.0F};
	request.maximum_raw_vector_reads = records.size();
	const LogicalGraphVectorSearchResult pinnedExact = snapshot->searchLogicalVector(request);
	ASSERT_EQ(pinnedExact.hits.size(), 1U);
	EXPECT_EQ(pinnedExact.hits.front().node_id, nodes[3]);
	EXPECT_FLOAT_EQ(pinnedExact.hits.front().score, 1.0F);
	currentSnapshot.reset();
	snapshot.reset();
	backend.close();

	advanceManifestCodebookGeneration(index.index_id);
	request.mode = LogicalGraphVectorSearchMode::LGVSM_AUTO;
	request.maximum_raw_vector_reads = 2U;
	request.candidate_refinement_budget = 2U;
	RelocatableStorageBackend corrupted(options());
	corrupted.open();
	auto corruptedSnapshot = corrupted.openLogicalGraphVectorSearchSnapshot();
	ASSERT_NE(corruptedSnapshot, nullptr);
	EXPECT_THROW(static_cast<void>(corruptedSnapshot->searchLogicalVector(request)), FormatException);
	corruptedSnapshot.reset();
	corrupted.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, RejectsDerivedVectorSearchBudgetCombinationsBeforeCandidateTraversal) {
	auto vectorValue = [](double first, double second) {
		return graph::PropertyValue(
				std::vector<graph::PropertyValue>{graph::PropertyValue(first), graph::PropertyValue(second)});
	};
	auto bfloatRecord = [](uint64_t vectorId, float first, float second) {
		CanonicalVectorRecord record;
		record.vector_id = vectorId;
		record.raw_bfloat16.resize(2U * sizeof(uint16_t));
		LittleEndianCodec::writeU16(record.raw_bfloat16, 0U,
									static_cast<uint16_t>(std::bit_cast<uint32_t>(first) >> 16U));
		LittleEndianCodec::writeU16(record.raw_bfloat16, sizeof(uint16_t),
									static_cast<uint16_t>(std::bit_cast<uint32_t>(second) >> 16U));
		return record;
	};

	RelocatableStorageBackend writer(options());
	writer.open();
	static_cast<void>(writer.createLogicalNodes({{{"Person"}, {{"embedding", vectorValue(0.0, 0.0)}}},
												 {{"Person"}, {{"embedding", vectorValue(0.0, 1.0)}}}}));
	const LogicalGraphIndexDefinition index =
			writer.createLogicalNodeVectorIndex({"person_embedding", "Person", "embedding", 2U, "l2"});
	writer.close();

	const std::vector<CanonicalVectorRecord> records = {bfloatRecord(1U, 0.0F, 0.0F), bfloatRecord(2U, 0.0F, 1.0F)};
	publishVectorPq(index.index_id, records);
	publishVectorAdjacency(index.index_id, records);

	RelocatableStorageBackend backend(options());
	backend.open();
	auto snapshot = backend.openLogicalGraphVectorSearchSnapshot();
	ASSERT_NE(snapshot, nullptr);
	LogicalGraphVectorSearchRequest request;
	request.index_name = index.name;
	request.query_vector = {0.0F, 1.0F};
	request.top_k = 1U;
	request.maximum_mapping_entries = records.size();
	request.maximum_raw_vector_reads = records.size();
	request.candidate_search_budget = 1U;
	request.candidate_refinement_budget = 1U;

	request.candidate_search_budget = 0U;
	EXPECT_THROW(static_cast<void>(snapshot->searchLogicalVector(request)), std::invalid_argument);
	request.candidate_search_budget = request.maximum_mapping_entries + 1U;
	EXPECT_THROW(static_cast<void>(snapshot->searchLogicalVector(request)), std::invalid_argument);
	request.candidate_search_budget = 1U;
	request.candidate_refinement_budget = 0U;
	EXPECT_THROW(static_cast<void>(snapshot->searchLogicalVector(request)), std::invalid_argument);
	request.candidate_refinement_budget = request.maximum_raw_vector_reads + 1U;
	EXPECT_THROW(static_cast<void>(snapshot->searchLogicalVector(request)), std::invalid_argument);
	request.candidate_refinement_budget = 1U;
	request.top_k = 2U;
	EXPECT_THROW(static_cast<void>(snapshot->searchLogicalVector(request)), std::invalid_argument);

	request.top_k = 1U;
	EXPECT_NO_THROW(static_cast<void>(snapshot->searchLogicalVector(request)));
	request.top_k = records.size();
	request.candidate_search_budget = records.size();
	request.candidate_refinement_budget = records.size();
	request.query_vector = {0.0F, 0.5F};
	const LogicalGraphVectorSearchResult expanded = snapshot->searchLogicalVector(request);
	EXPECT_EQ(expanded.hits.size(), records.size());
	EXPECT_GT(expanded.stats.candidate_expansions, 1U);
	EXPECT_FLOAT_EQ(expanded.hits[0].score, expanded.hits[1].score);
	EXPECT_LT(expanded.hits[0].node_id, expanded.hits[1].node_id);

	request.top_k = 1U;
	request.maximum_mapping_entries = 1U;
	request.maximum_raw_vector_reads = 1U;
	request.candidate_search_budget = 1U;
	request.candidate_refinement_budget = 1U;
	EXPECT_THROW(static_cast<void>(snapshot->searchLogicalVector(request)), std::runtime_error);
	snapshot.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, ValidatesNonFiniteAndZeroCosineQueriesAtTheStorageSearchBoundary) {
	auto vectorValue = [](double first, double second) {
		return graph::PropertyValue(
				std::vector<graph::PropertyValue>{graph::PropertyValue(first), graph::PropertyValue(second)});
	};
	RelocatableStorageBackend writer(options());
	writer.open();
	static_cast<void>(writer.createLogicalNodes({{{"Person"}, {{"embedding", vectorValue(1.0, 0.0)}}}}));
	const LogicalGraphIndexDefinition index =
			writer.createLogicalNodeVectorIndex({"person_embedding", "Person", "embedding", 2U, "cosine"});
	writer.close();

	const VectorManifest manifest = readVectorManifest(index.index_id);
	IndexDefinition definition;
	definition.index_id = index.index_id;
	definition.kind = IndexDefinitionKind::IDK_VECTOR;
	definition.vector_dimensions = manifest.dimensions;
	definition.vector_metric = "cosine";
	LogicalGraphVectorSearchRequest request;
	request.index_name = index.name;
	request.query_vector = {0.0F, 0.0F};
	request.top_k = 1U;
	request.mode = LogicalGraphVectorSearchMode::LGVSM_EXACT;
	request.maximum_mapping_entries = 1U;
	request.maximum_raw_vector_reads = 1U;

	expectDirectVectorSearchInvalidArgument(definition, manifest, request,
											"relocatable cosine Vector search query must have a nonzero L2 norm");
	request.query_vector = {std::numeric_limits<float>::quiet_NaN(), 0.0F};
	expectDirectVectorSearchInvalidArgument(
			definition, manifest, request,
			"relocatable logical Vector search requires a finite query matching the persisted dimensions");
}

TEST_F(RelocatableStorageBackendSchemaTest, RejectsDeltaManifestCountThatDisagreesWithItsPersistedOverlay) {
	auto vectorValue = [](double first, double second) {
		return graph::PropertyValue(
				std::vector<graph::PropertyValue>{graph::PropertyValue(first), graph::PropertyValue(second)});
	};
	auto bfloatRecord = [](uint64_t vectorId, float first, float second) {
		CanonicalVectorRecord record;
		record.vector_id = vectorId;
		record.raw_bfloat16.resize(2U * sizeof(uint16_t));
		LittleEndianCodec::writeU16(record.raw_bfloat16, 0U,
								static_cast<uint16_t>(std::bit_cast<uint32_t>(first) >> 16U));
		LittleEndianCodec::writeU16(record.raw_bfloat16, sizeof(uint16_t),
								static_cast<uint16_t>(std::bit_cast<uint32_t>(second) >> 16U));
		return record;
	};

	RelocatableStorageBackend writer(options());
	writer.open();
	const std::vector<uint64_t> nodes =
			writer.createLogicalNodes({{{"Person"}, {{"embedding", vectorValue(0.0, 0.0)}}},
											 {{"Person"}, {{"embedding", vectorValue(0.0, 1.0)}}}});
	ASSERT_EQ(nodes.size(), 2U);
	const LogicalGraphIndexDefinition index =
			writer.createLogicalNodeVectorIndex({"person_embedding", "Person", "embedding", 2U, "l2"});
	writer.close();
	const std::array<CanonicalVectorRecord, 2U> records = {
			bfloatRecord(1U, 0.0F, 0.0F), bfloatRecord(2U, 0.0F, 1.0F)};
	publishVectorAdjacency(index.index_id, records);

	RelocatableStorageBackend updater(options());
	updater.open();
	updater.replaceLogicalNodeProperties({{nodes.back(), {{"embedding", vectorValue(1.0, 1.0)}}}});
	updater.close();

	const VectorManifest manifest = readVectorManifest(index.index_id);
	ASSERT_NE(manifest.delta_mapping_index_id, 0U);
	ASSERT_GT(manifest.delta_entry_count, 1U);
	ASSERT_LT(manifest.delta_entry_count, std::numeric_limits<uint64_t>::max());
	IndexDefinition definition;
	definition.index_id = index.index_id;
	definition.kind = IndexDefinitionKind::IDK_VECTOR;
	definition.vector_dimensions = manifest.dimensions;
	definition.vector_metric = "l2";
	LogicalGraphVectorSearchRequest request;
	request.index_name = index.name;
	request.query_vector = {0.0F, 1.0F};
	request.top_k = 1U;
	request.maximum_mapping_entries = 8U;
	request.maximum_raw_vector_reads = 8U;
	request.candidate_search_budget = 1U;
	VectorManifest corrupt = manifest;
	++corrupt.delta_entry_count;
	expectDirectVectorSearchRejected(definition, corrupt, request,
									 "relocatable logical Vector search delta overlay count disagrees with its Manifest");

	request.maximum_mapping_entries = manifest.delta_entry_count - 1U;
	RelocatableStorageBackend bounded(options());
	bounded.open();
	auto snapshot = bounded.openLogicalGraphVectorSearchSnapshot();
	ASSERT_NE(snapshot, nullptr);
	EXPECT_THROW(static_cast<void>(snapshot->searchLogicalVector(request)), std::runtime_error);
	snapshot.reset();
	bounded.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, RejectsPartialAndStaleVectorManifestIdentitiesAtTheSearchBoundary) {
	auto vectorValue = [](double first, double second) {
		return graph::PropertyValue(
				std::vector<graph::PropertyValue>{graph::PropertyValue(first), graph::PropertyValue(second)});
	};
	RelocatableStorageBackend writer(options());
	writer.open();
	static_cast<void>(writer.createLogicalNodes({{{"Person"}, {{"embedding", vectorValue(0.0, 0.0)}}}}));
	const LogicalGraphIndexDefinition index =
			writer.createLogicalNodeVectorIndex({"person_embedding", "Person", "embedding", 2U, "l2"});
	writer.close();

	const VectorManifest original = readVectorManifest(index.index_id);
	LogicalGraphVectorSearchRequest request;
	request.index_name = index.name;
	request.query_vector = {0.0F, 0.0F};
	request.top_k = 1U;
	request.maximum_mapping_entries = 1U;
	request.maximum_raw_vector_reads = 1U;

	const auto expectRejected = [&] {
		RelocatableStorageBackend backend(options());
		backend.open();
		auto snapshot = backend.openLogicalGraphVectorSearchSnapshot();
		ASSERT_NE(snapshot, nullptr);
		EXPECT_THROW(static_cast<void>(snapshot->searchLogicalVector(request)), FormatException);
		snapshot.reset();
		backend.close();
	};

	VectorManifest corrupt = original;
	corrupt.node_mapping_root_generation = Generation(original.node_mapping_root_generation.value() + 1U);
	overwriteVectorManifest(corrupt);
	expectRejected();
	overwriteVectorManifest(original);

	corrupt = original;
	corrupt.metric = VectorDistanceMetric::VDM_INNER_PRODUCT;
	overwriteVectorManifest(corrupt);
	expectRejected();
	overwriteVectorManifest(original);

	IndexDefinition definition;
	definition.index_id = index.index_id;
	definition.kind = IndexDefinitionKind::IDK_VECTOR;
	definition.vector_dimensions = original.dimensions;
	definition.vector_metric = "l2";

	corrupt = original;
	corrupt.vector_node_mapping_root_page_id = PageId(1U);
	expectDirectVectorSearchRejected(definition, corrupt, request,
									 "relocatable logical Vector search found a stale Vector-to-Node mapping root");

	// A public Index ID cannot masquerade as a delta root, even when the rest
	// of this forged identity is incomplete.
	corrupt = original;
	corrupt.delta_mapping_index_id = original.index_id;
	expectDirectVectorSearchRejected(definition, corrupt, request,
								 "relocatable logical Vector search found an invalid semantic Manifest");

	corrupt = original;
	corrupt.adjacency_generation = Generation(1U);
	expectDirectVectorSearchRejected(
			definition, corrupt, request,
			"relocatable logical Vector search found a partial or incompatible adjacency generation");

	corrupt = original;
	corrupt.adjacency_mapping_index_id = RSS_ADJACENCY_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0xD5);
	expectDirectVectorSearchRejected(
			definition, corrupt, request,
			"relocatable logical Vector search found a partial or incompatible adjacency generation");
	corrupt = original;
	corrupt.adjacency_mapping_root_page_id = PageId(1U);
	expectDirectVectorSearchRejected(
			definition, corrupt, request,
			"relocatable logical Vector search found a partial or incompatible adjacency generation");
	corrupt = original;
	corrupt.adjacency_mapping_root_generation = Generation(1U);
	expectDirectVectorSearchRejected(
			definition, corrupt, request,
			"relocatable logical Vector search found a partial or incompatible adjacency generation");
	corrupt = original;
	corrupt.entry_point_vector_id = 1U;
	expectDirectVectorSearchRejected(
			definition, corrupt, request,
			"relocatable logical Vector search found a partial or incompatible adjacency generation");

	corrupt = original;
	corrupt.adjacency_generation = Generation(1U);
	corrupt.entry_point_vector_id = 1U;
	corrupt.adjacency_mapping_index_id = RSS_ADJACENCY_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0xD3);
	corrupt.adjacency_mapping_root_page_id = PageId(1U);
	corrupt.adjacency_mapping_root_generation = Generation(1U);
	expectDirectVectorSearchRejected(definition, corrupt, request,
									 "relocatable logical Vector search found a stale adjacency mapping root");

	corrupt = original;
	corrupt.codebook_id = 1U;
	corrupt.codebook_generation = Generation(1U);
	expectDirectVectorSearchRejected(definition, corrupt, request,
									 "relocatable logical Vector search found a partial PQ generation");

	corrupt = original;
	corrupt.codebook_generation = Generation(1U);
	expectDirectVectorSearchRejected(definition, corrupt, request,
									 "relocatable logical Vector search found a partial PQ generation");
	corrupt = original;
	corrupt.pq_mapping_index_id = RSS_PQ_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0xD6);
	expectDirectVectorSearchRejected(definition, corrupt, request,
									 "relocatable logical Vector search found a partial PQ generation");
	corrupt = original;
	corrupt.pq_mapping_root_page_id = PageId(1U);
	expectDirectVectorSearchRejected(definition, corrupt, request,
									 "relocatable logical Vector search found a partial PQ generation");
	corrupt = original;
	corrupt.pq_mapping_root_generation = Generation(1U);
	expectDirectVectorSearchRejected(definition, corrupt, request,
									 "relocatable logical Vector search found a partial PQ generation");

	corrupt = original;
	corrupt.codebook_id = 1U;
	corrupt.codebook_generation = Generation(1U);
	corrupt.pq_mapping_index_id = RSS_PQ_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0xD4);
	corrupt.pq_mapping_root_page_id = PageId(1U);
	corrupt.pq_mapping_root_generation = Generation(1U);
	expectDirectVectorSearchRejected(definition, corrupt, request,
									 "relocatable logical Vector search found a stale PQ mapping root");
}

TEST_F(RelocatableStorageBackendSchemaTest, RejectsStaleAndNonFiniteRawVectorRecordsBeforeScoring) {
	auto vectorValue = [](double first, double second) {
		return graph::PropertyValue(
				std::vector<graph::PropertyValue>{graph::PropertyValue(first), graph::PropertyValue(second)});
	};
	RelocatableStorageBackend writer(options());
	writer.open();
	static_cast<void>(writer.createLogicalNodes({{{"Person"}, {{"embedding", vectorValue(0.0, 0.0)}}}}));
	const LogicalGraphIndexDefinition index =
			writer.createLogicalNodeVectorIndex({"person_embedding", "Person", "embedding", 2U, "l2"});
	writer.close();

	LogicalGraphVectorSearchRequest request;
	request.index_name = index.name;
	request.query_vector = {0.0F, 0.0F};
	request.top_k = 1U;
	request.mode = LogicalGraphVectorSearchMode::LGVSM_EXACT;
	request.maximum_mapping_entries = 1U;
	request.maximum_raw_vector_reads = 1U;
	const auto expectRejected = [&](const char *expectedMessage) {
		RelocatableStorageBackend backend(options());
		backend.open();
		auto snapshot = backend.openLogicalGraphVectorSearchSnapshot();
		ASSERT_NE(snapshot, nullptr);
		try {
			static_cast<void>(snapshot->searchLogicalVector(request));
			FAIL() << "expected the corrupted raw Vector record to be rejected";
		} catch (const FormatException &error) {
			EXPECT_STREQ(error.what(), expectedMessage);
		}
		snapshot.reset();
		backend.close();
	};

	const std::vector<std::byte> original = readVectorRecord(1U);
	std::vector<std::byte> corrupt = original;
	LittleEndianCodec::writeU64(corrupt, 0U, 2U);
	overwriteVectorRecord(1U, corrupt);
	expectRejected("relocatable logical Vector search resolved a stale raw Vector record");
	overwriteVectorRecord(1U, original);

	corrupt = original;
	LittleEndianCodec::writeU16(corrupt, sizeof(uint64_t), 0x7F80U);
	overwriteVectorRecord(1U, corrupt);
	expectRejected("relocatable logical Vector search decoded a non-finite raw coordinate");
}

TEST_F(RelocatableStorageBackendSchemaTest, RejectsMalformedAndNonBijectiveSemanticVectorMappings) {
	auto vectorValue = [](double first, double second) {
		return graph::PropertyValue(
				std::vector<graph::PropertyValue>{graph::PropertyValue(first), graph::PropertyValue(second)});
	};
	RelocatableStorageBackend writer(options());
	writer.open();
	static_cast<void>(writer.createLogicalNodes({{{"Person"}, {{"embedding", vectorValue(0.0, 0.0)}}}}));
	const LogicalGraphIndexDefinition index =
			writer.createLogicalNodeVectorIndex({"person_embedding", "Person", "embedding", 2U, "l2"});
	writer.close();

	LogicalGraphVectorSearchRequest request;
	request.index_name = index.name;
	request.query_vector = {0.0F, 0.0F};
	request.top_k = 1U;
	request.mode = LogicalGraphVectorSearchMode::LGVSM_EXACT;
	request.maximum_mapping_entries = 1U;
	request.maximum_raw_vector_reads = 1U;
	const auto expectRejected = [&](const char *expectedMessage) {
		RelocatableStorageBackend backend(options());
		backend.open();
		auto snapshot = backend.openLogicalGraphVectorSearchSnapshot();
		ASSERT_NE(snapshot, nullptr);
		try {
			static_cast<void>(snapshot->searchLogicalVector(request));
			FAIL() << "expected the corrupted semantic mapping to be rejected";
		} catch (const FormatException &error) {
			EXPECT_STREQ(error.what(), expectedMessage);
		}
		snapshot.reset();
		backend.close();
	};

	const BTreePageEntry nodeMapping{VectorSemanticCodec::nodeMappingKey(1U), 1U};
	const BTreePageEntry vectorMapping{VectorSemanticCodec::vectorMappingKey(1U), 1U};
	const VectorManifest manifest = readVectorManifest(index.index_id);

	overwriteBTreeLeafEntry(index.index_id, nodeMapping.key, {nodeMapping.key, 0U});
	expectRejected("relocatable logical Vector search found a malformed Node-to-Vector mapping");
	overwriteBTreeLeafEntry(index.index_id, nodeMapping.key, nodeMapping);

	const std::vector<std::byte> zeroNodeKey(sizeof(uint64_t), std::byte{});
	overwriteBTreeLeafEntry(index.index_id, nodeMapping.key, {zeroNodeKey, 1U});
	expectRejected("relocatable logical Vector search found a noncanonical Node mapping key");
	overwriteBTreeLeafEntry(index.index_id, zeroNodeKey, nodeMapping);

	const BTreePageEntry wrongNodeMapping{VectorSemanticCodec::nodeMappingKey(2U), 1U};
	overwriteBTreeLeafEntry(index.index_id, nodeMapping.key, wrongNodeMapping);
	expectRejected("relocatable logical Vector search found inconsistent semantic mapping directions");
	overwriteBTreeLeafEntry(index.index_id, wrongNodeMapping.key, nodeMapping);

	overwriteBTreeLeafEntry(manifest.vector_node_mapping_index_id, vectorMapping.key, {vectorMapping.key, 0U});
	expectRejected("relocatable Vector-to-Node mapping B+Tree contains an invalid Node ID value");
	overwriteBTreeLeafEntry(manifest.vector_node_mapping_index_id, vectorMapping.key, vectorMapping);
}

TEST_F(RelocatableStorageBackendSchemaTest, RejectsMissingAdjacencyMappingBeforeCandidateExpansion) {
	auto vectorValue = [](double first, double second) {
		return graph::PropertyValue(
				std::vector<graph::PropertyValue>{graph::PropertyValue(first), graph::PropertyValue(second)});
	};
	auto bfloatRecord = [](uint64_t vectorId, float first, float second) {
		CanonicalVectorRecord record;
		record.vector_id = vectorId;
		record.raw_bfloat16.resize(2U * sizeof(uint16_t));
		LittleEndianCodec::writeU16(record.raw_bfloat16, 0U,
									static_cast<uint16_t>(std::bit_cast<uint32_t>(first) >> 16U));
		LittleEndianCodec::writeU16(record.raw_bfloat16, sizeof(uint16_t),
									static_cast<uint16_t>(std::bit_cast<uint32_t>(second) >> 16U));
		return record;
	};

	RelocatableStorageBackend writer(options());
	writer.open();
	static_cast<void>(writer.createLogicalNodes({{{"Person"}, {{"embedding", vectorValue(0.0, 0.0)}}}}));
	const LogicalGraphIndexDefinition index =
			writer.createLogicalNodeVectorIndex({"person_embedding", "Person", "embedding", 2U, "l2"});
	writer.close();
	const std::vector<CanonicalVectorRecord> records = {bfloatRecord(1U, 0.0F, 0.0F)};
	publishVectorAdjacency(index.index_id, records);

	const VectorManifest manifest = readVectorManifest(index.index_id);
	const std::vector<std::byte> entryKey = VectorSemanticCodec::vectorMappingKey(manifest.entry_point_vector_id);
	const std::vector<std::byte> replacementKey =
			VectorSemanticCodec::vectorMappingKey(manifest.entry_point_vector_id + 1U);
	const std::vector<BTreePageEntry> mappings = readIndexEntries(manifest.adjacency_mapping_index_id);
	ASSERT_EQ(mappings.size(), 1U);
	ASSERT_EQ(mappings.front().key, entryKey);
	const BTreePageEntry mapping = mappings.front();
	overwriteBTreeLeafEntry(manifest.adjacency_mapping_index_id, entryKey, {replacementKey, mapping.value_or_child});

	RelocatableStorageBackend backend(options());
	backend.open();
	auto snapshot = backend.openLogicalGraphVectorSearchSnapshot();
	ASSERT_NE(snapshot, nullptr);
	LogicalGraphVectorSearchRequest request;
	request.index_name = index.name;
	request.query_vector = {0.0F, 0.0F};
	request.top_k = 1U;
	request.maximum_mapping_entries = 2U;
	request.maximum_raw_vector_reads = 2U;
	request.candidate_search_budget = 1U;
	try {
		static_cast<void>(snapshot->searchLogicalVector(request));
		FAIL() << "expected the missing adjacency mapping to be rejected";
	} catch (const FormatException &error) {
		EXPECT_STREQ(error.what(), "relocatable logical Vector search found no adjacency record for a candidate");
	}
	snapshot.reset();
	backend.close();
	overwriteBTreeLeafEntry(manifest.adjacency_mapping_index_id, replacementKey, mapping);
}

TEST_F(RelocatableStorageBackendSchemaTest, RejectsCorruptPqInventoryMappingsAndCodesBeforeRefinement) {
	auto vectorValue = [](double first, double second) {
		return graph::PropertyValue(
				std::vector<graph::PropertyValue>{graph::PropertyValue(first), graph::PropertyValue(second)});
	};
	auto bfloatRecord = [](uint64_t vectorId, float first, float second) {
		CanonicalVectorRecord record;
		record.vector_id = vectorId;
		record.raw_bfloat16.resize(2U * sizeof(uint16_t));
		LittleEndianCodec::writeU16(record.raw_bfloat16, 0U,
									static_cast<uint16_t>(std::bit_cast<uint32_t>(first) >> 16U));
		LittleEndianCodec::writeU16(record.raw_bfloat16, sizeof(uint16_t),
									static_cast<uint16_t>(std::bit_cast<uint32_t>(second) >> 16U));
		return record;
	};
	RelocatableStorageBackend writer(options());
	writer.open();
	static_cast<void>(writer.createLogicalNodes({{{"Person"}, {{"embedding", vectorValue(0.0, 0.0)}}},
												 {{"Person"}, {{"embedding", vectorValue(0.0, 1.0)}}}}));
	const LogicalGraphIndexDefinition index =
			writer.createLogicalNodeVectorIndex({"person_embedding", "Person", "embedding", 2U, "l2"});
	writer.close();
	const std::vector<CanonicalVectorRecord> records = {bfloatRecord(1U, 0.0F, 0.0F), bfloatRecord(2U, 0.0F, 1.0F)};
	publishVectorPq(index.index_id, records);
	publishVectorAdjacency(index.index_id, records);

	const VectorManifest manifest = readVectorManifest(index.index_id);
	const auto pqEntry = std::ranges::find_if(manifest.entries, [](const VectorManifestEntry &entry) {
		return entry.storage_kind == VectorStorageKind::VSK_PQ;
	});
	ASSERT_NE(pqEntry, manifest.entries.end());
	ASSERT_NE(manifest.entry_point_vector_id, 0U);
	LogicalGraphVectorSearchRequest request;
	request.index_name = index.name;
	request.query_vector = {0.0F, 0.0F};
	request.top_k = 1U;
	request.maximum_mapping_entries = records.size();
	request.maximum_raw_vector_reads = 1U;
	request.candidate_search_budget = 1U;
	request.candidate_refinement_budget = 1U;
	const auto expectRejected = [&](const char *expectedMessage) {
		RelocatableStorageBackend backend(options());
		backend.open();
		auto snapshot = backend.openLogicalGraphVectorSearchSnapshot();
		ASSERT_NE(snapshot, nullptr);
		try {
			static_cast<void>(snapshot->searchLogicalVector(request));
			FAIL() << "expected the corrupt PQ state to be rejected";
		} catch (const FormatException &error) {
			EXPECT_STREQ(error.what(), expectedMessage);
		}
		snapshot.reset();
		backend.close();
	};

	IndexDefinition definition;
	definition.index_id = index.index_id;
	definition.kind = IndexDefinitionKind::IDK_VECTOR;
	definition.vector_dimensions = manifest.dimensions;
	definition.vector_metric = "l2";
	VectorManifest missingInventory = manifest;
	missingInventory.entries.clear();
	expectDirectVectorSearchRejected(definition, missingInventory, request,
									 "relocatable logical Vector search PQ generation has no Manifest page inventory");

	const BTreePageEntry pqMapping{VectorSemanticCodec::vectorMappingKey(manifest.entry_point_vector_id),
								   pqEntry->page_id.value()};
	overwriteBTreeLeafEntry(manifest.pq_mapping_index_id, pqMapping.key,
							{pqMapping.key, manifest.raw_write_tail_page_id.value()});
	expectRejected("relocatable logical Vector search PQ mapping escaped its Manifest inventory");
	overwriteBTreeLeafEntry(manifest.pq_mapping_index_id, pqMapping.key, pqMapping);

	// A missing mapping is distinct from a mapping that targets a page outside
	// the Manifest inventory. Both states must be rejected before page I/O.
	const std::vector<std::byte> missingMappingKey =
			VectorSemanticCodec::vectorMappingKey(manifest.entry_point_vector_id + 100U);
	overwriteBTreeLeafEntry(manifest.pq_mapping_index_id, pqMapping.key,
							{missingMappingKey, pqMapping.value_or_child});
	expectRejected("relocatable logical Vector search PQ mapping escaped its Manifest inventory");
	overwriteBTreeLeafEntry(manifest.pq_mapping_index_id, missingMappingKey, pqMapping);

	const std::vector<std::byte> original = readVectorPageRecord(pqEntry->page_id, manifest.entry_point_vector_id);
	std::vector<std::byte> corrupt = original;
	const uint64_t staleVectorId = manifest.entry_point_vector_id + 100U;
	LittleEndianCodec::writeU64(corrupt, 0U, staleVectorId);
	overwriteVectorPageRecord(pqEntry->page_id, manifest.entry_point_vector_id, corrupt);
	expectRejected("relocatable logical Vector search resolved an incompatible PQ page");
	overwriteVectorPageRecord(pqEntry->page_id, staleVectorId, original);

	corrupt = original;
	corrupt[sizeof(uint64_t)] = std::byte{0xFFU};
	overwriteVectorPageRecord(pqEntry->page_id, manifest.entry_point_vector_id, corrupt);
	expectRejected("relocatable logical Vector search found an out-of-range PQ centroid code");
}

TEST_F(RelocatableStorageBackendSchemaTest, ExecutesTheExistingVectorProcedureOverTheLogicalSearchBoundary) {
	auto vectorValue = [](double first, double second) {
		return graph::PropertyValue(
				std::vector<graph::PropertyValue>{graph::PropertyValue(first), graph::PropertyValue(second)});
	};
	auto backend = std::make_shared<RelocatableStorageBackend>(options());
	backend->open();
	const std::vector<uint64_t> nodes =
			backend->createLogicalNodes({{{"Person"}, {{"embedding", vectorValue(1.0, -2.0)}}},
										 {{"Person"}, {{"embedding", vectorValue(-3.0, 4.0)}}}});
	ASSERT_EQ(nodes.size(), 2U);
	static_cast<void>(
			backend->createLogicalNodeVectorIndex({"person_embedding", "Person", "embedding", 2U, "inner_product"}));

	graph::query::QueryEngine engine(backend, backend);
	const graph::query::QueryResult result = engine.execute(
			"CALL db.index.vector.queryNodes('person_embedding', 2, [1.0, -2.0]) YIELD node, score RETURN node, score");
	ASSERT_EQ(result.getColumns(), (std::vector<std::string>{"node", "score"}));
	ASSERT_EQ(result.rowCount(), 2U);
	ASSERT_TRUE(result.getRows()[0].at("node").isLogicalNode());
	EXPECT_EQ(result.getRows()[0].at("node").asLogicalNode().node_id, nodes[0]);
	EXPECT_EQ(result.getRows()[1].at("node").asLogicalNode().node_id, nodes[1]);
	EXPECT_LT(std::get<double>(result.getRows()[0].at("score").asPrimitive().getVariant()),
			  std::get<double>(result.getRows()[1].at("score").asPrimitive().getVariant()));
	backend->close();
}

TEST_F(RelocatableStorageBackendSchemaTest, TrainsAndReusesAPersistentPqGenerationThroughTheLogicalProcedure) {
	auto vectorValue = [](double first, double second) {
		return graph::PropertyValue(
				std::vector<graph::PropertyValue>{graph::PropertyValue(first), graph::PropertyValue(second)});
	};
	auto backend = std::make_shared<RelocatableStorageBackend>(options());
	backend->open();
	static_cast<void>(backend->createLogicalNodes({{{"Person"}, {{"embedding", vectorValue(0.0, 0.0)}}},
												   {{"Person"}, {{"embedding", vectorValue(0.0, 1.0)}}},
												   {{"Person"}, {{"embedding", vectorValue(10.0, 10.0)}}}}));
	const LogicalGraphIndexDefinition index =
			backend->createLogicalNodeVectorIndex({"person_embedding", "Person", "embedding", 2U, "l2"});

	graph::query::QueryEngine engine(backend, backend);
	const graph::query::QueryResult procedure = engine.execute("CALL db.index.vector.train('person_embedding')");
	ASSERT_EQ(procedure.getColumns(), (std::vector<std::string>{"status"}));
	ASSERT_EQ(procedure.rowCount(), 1U);
	EXPECT_EQ(procedure.getRows().front().at("status").asPrimitive(), graph::PropertyValue("Success"));
	const graph::query::QueryResult projectedProcedure =
			engine.execute("CALL db.index.vector.train('person_embedding') YIELD status "
						   "WHERE status = 'Success' RETURN status AS training_status");
	ASSERT_EQ(projectedProcedure.getColumns(), (std::vector<std::string>{"training_status"}));
	ASSERT_EQ(projectedProcedure.rowCount(), 1U);
	EXPECT_EQ(projectedProcedure.getRows().front().at("training_status").asPrimitive(),
			  graph::PropertyValue("Success"));
	const graph::query::QueryResult skippedProcedure =
			engine.execute("CALL db.index.vector.train('person_embedding') YIELD status RETURN status SKIP 1 LIMIT 1");
	EXPECT_TRUE(skippedProcedure.isEmpty());
	static_cast<void>(backend->createLogicalNodeVectorIndex({"empty_embedding", "Empty", "embedding", 2U, "l2"}));
	const graph::query::QueryResult emptyProcedure =
			engine.execute("CALL db.index.vector.train('empty_embedding') YIELD status RETURN status");
	ASSERT_EQ(emptyProcedure.getColumns(), (std::vector<std::string>{"status"}));
	ASSERT_EQ(emptyProcedure.rowCount(), 1U);
	EXPECT_EQ(emptyProcedure.getRows().front().at("status").asPrimitive(),
			  graph::PropertyValue("Skipped (Empty Data)"));

	const VectorManifest first = readVectorManifest(index.index_id);
	EXPECT_NE(first.codebook_id, 0U);
	EXPECT_TRUE(first.codebook_generation.isValid());
	EXPECT_TRUE(VectorPqMapping::isMappingIndexId(first.pq_mapping_index_id));
	EXPECT_TRUE(first.pq_mapping_root_page_id.isValid());
	EXPECT_TRUE(first.pq_mapping_root_generation.isValid());
	EXPECT_TRUE(first.adjacency_generation.isValid());
	EXPECT_TRUE(VectorAdjacencyMapping::isMappingIndexId(first.adjacency_mapping_index_id));
	EXPECT_TRUE(first.adjacency_mapping_root_page_id.isValid());
	EXPECT_TRUE(first.adjacency_mapping_root_generation.isValid());
	EXPECT_EQ(first.delta_entry_count, 0U);
	EXPECT_TRUE(std::ranges::any_of(first.entries, [](const VectorManifestEntry &entry) {
		return entry.storage_kind == VectorStorageKind::VSK_PQ;
	}));
	EXPECT_NO_THROW(static_cast<void>(backend->verifyIntegrity()));

	const LogicalGraphVectorTrainingResult retry = backend->trainLogicalVector({"person_embedding", 16U, 16U});
	EXPECT_TRUE(retry.trained);
	EXPECT_TRUE(retry.reused_existing_generation);
	EXPECT_EQ(readVectorManifest(index.index_id), first);

	auto exactSnapshot = backend->openLogicalGraphVectorSearchSnapshot();
	LogicalGraphVectorSearchRequest exact;
	exact.index_name = "person_embedding";
	exact.query_vector = {0.0F, 1.0F};
	exact.top_k = 1U;
	exact.mode = LogicalGraphVectorSearchMode::LGVSM_EXACT;
	exact.maximum_mapping_entries = 16U;
	exact.maximum_raw_vector_reads = 16U;
	EXPECT_EQ(exactSnapshot->searchLogicalVector(exact).hits.size(), 1U);
	exactSnapshot.reset();

	graph::query::QueryContext readOnly;
	readOnly.execMode = graph::query::ExecMode::EM_READ_ONLY;
	EXPECT_THROW(static_cast<void>(engine.execute("CALL db.index.vector.train('person_embedding')", readOnly)),
				 std::runtime_error);
	EXPECT_THROW(static_cast<void>(backend->trainLogicalVector({"missing", 16U, 16U})), std::runtime_error);
	backend->close();

	RelocatableStorageBackend reopened(options());
	reopened.open();
	const LogicalGraphVectorTrainingResult afterReopen = reopened.trainLogicalVector({"person_embedding", 16U, 16U});
	EXPECT_TRUE(afterReopen.trained);
	EXPECT_TRUE(afterReopen.reused_existing_generation);
	reopened.close();
}
