/**
 * @file test_RelocatableStorageBackendSchema_IndexContracts.cpp
 * @brief Index candidate, lookup, mutation and batch publication contracts.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "storage/relocatable/backend/RelocatableStorageBackendSchemaTestFixture.hpp"

#include <atomic>
#ifndef _WIN32
#include <cerrno>
#endif

#include "graph/debug/PerfTrace.hpp"
#include "graph/query/QueryGuard.hpp"
#include "graph/storage/relocatable/format/SuperblockFile.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

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
	const std::vector<uint64_t> nodes = backend.createLogicalNodes(
			{{{"User"}, {{"email", graph::PropertyValue(std::string("ada@example.test"))}}}});
	ASSERT_EQ(nodes.size(), 1U);
	backend.close();

	const std::vector<graph::PropertyValue> values = {graph::PropertyValue(std::string("ada@example.test"))};
	const std::vector<std::byte> key =
			ConstraintSupportTree::entryKey(ConstraintSupportTree::canonicalKey(values), nodes.front());
	// Preserve a valid B+Tree page encoding while modelling a stale support-tree payload.
	overwriteBTreeLeafEntry(constraint.constraint_id, key, {key, nodes.front() + 1U});

	RelocatableStorageBackend corrupted(options());
	corrupted.open();
	RelocatableReadSnapshot snapshot = corrupted.openSnapshot();
	EXPECT_THROW(
			static_cast<void>(corrupted.findNodesByExactProperties(
					snapshot, {"User"}, {{"email", graph::PropertyValue(std::string("ada@example.test"))}}, 1U, 2U)),
			FormatException);
	snapshot.release();
	EXPECT_THROW(static_cast<void>(corrupted.verifyIntegrity()), FormatException);
	corrupted.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, RejectsPersistedScalarIndexEntriesWithMismatchedEntityValues) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes(
			{{{"Person"}, {{"score", graph::PropertyValue(int64_t{42})}}}});
	ASSERT_EQ(nodes.size(), 1U);
	const LogicalGraphIndexDefinition index = backend.createLogicalNodeIndex({"person_score", "Person", {"score"}});
	backend.close();

	const std::vector<std::byte> key = IndexKeyCodec::encodeEntryKey(graph::PropertyValue(int64_t{42}), nodes.front());
	// Keep the page structurally valid while modelling a stale durable payload
	// that cannot be created through the public graph writer.
	overwriteBTreeLeafEntry(index.index_id, key, {key, nodes.front() + 1U});

	RelocatableStorageBackend corrupted(options());
	corrupted.open();
	RelocatableReadSnapshot snapshot = corrupted.openSnapshot();
	std::unique_ptr<LogicalGraphReadSnapshot> graphSnapshot = corrupted.openLogicalGraphReadSnapshot();
	EXPECT_THROW(static_cast<void>(corrupted.findNodesByExactProperties(
				snapshot, {"Person"}, {{"score", graph::PropertyValue(int64_t{42})}}, 1U, 1U)),
			 FormatException);
	EXPECT_THROW(static_cast<void>(graphSnapshot->visitNodeCandidatesByExactProperties(
				{"Person"}, {{"score", graph::PropertyValue(int64_t{42})}}, 1U, 1U,
				[](const graph::storage::IndexCandidateBatch &) { return true; })),
			 FormatException);
	snapshot.release();
	graphSnapshot.reset();
	corrupted.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, RejectsPersistedScalarIndexKeysThatDisagreeWithThePropertyRecord) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes(
			{{{"Person"}, {{"score", graph::PropertyValue(int64_t{42})}}}});
	ASSERT_EQ(nodes.size(), 1U);
	const LogicalGraphIndexDefinition index = backend.createLogicalNodeIndex({"person_score", "Person", {"score"}});
	backend.close();

	const std::vector<std::byte> storedKey = IndexKeyCodec::encodeEntryKey(graph::PropertyValue(int64_t{42}), nodes.front());
	const std::vector<std::byte> staleKey = IndexKeyCodec::encodeEntryKey(graph::PropertyValue(int64_t{43}), nodes.front());
	// A page checksum authenticates this replacement key, but not its agreement
	// with the separate authoritative Property collection.
	overwriteBTreeLeafEntry(index.index_id, storedKey, {staleKey, nodes.front()});

	RelocatableStorageBackend corrupted(options());
	corrupted.open();
	RelocatableReadSnapshot snapshot = corrupted.openSnapshot();
	EXPECT_THROW(static_cast<void>(corrupted.findNodesByExactProperties(
				snapshot, {"Person"}, {{"score", graph::PropertyValue(int64_t{43})}}, 1U, 1U)),
			 FormatException);
	snapshot.release();
	corrupted.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, RejectsPersistedScalarRangeIndexEntriesWithMismatchedEntityValues) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes(
			{{{"Person"}, {{"score", graph::PropertyValue(int64_t{42})}}}});
	ASSERT_EQ(nodes.size(), 1U);
	const LogicalGraphIndexDefinition index = backend.createLogicalNodeIndex({"person_score", "Person", {"score"}});
	backend.close();

	const std::vector<std::byte> key = IndexKeyCodec::encodeEntryKey(graph::PropertyValue(int64_t{42}), nodes.front());
	overwriteBTreeLeafEntry(index.index_id, key, {key, nodes.front() + 1U});

	RelocatableStorageBackend corrupted(options());
	corrupted.open();
	RelocatableReadSnapshot snapshot = corrupted.openSnapshot();
	std::unique_ptr<LogicalGraphReadSnapshot> graphSnapshot = corrupted.openLogicalGraphReadSnapshot();
	EXPECT_THROW(static_cast<void>(corrupted.findNodesByPropertyRange(
				snapshot, {"Person"},
				{"score", graph::PropertyValue(int64_t{42}), graph::PropertyValue(), true, true}, 1U, 1U)),
			 FormatException);
	EXPECT_THROW(static_cast<void>(graphSnapshot->visitNodeCandidatesByPropertyRange(
				{"Person"},
				{"score", graph::PropertyValue(int64_t{42}), graph::PropertyValue(), true, true}, 1U, 1U,
				[](const graph::storage::IndexCandidateBatch &) { return true; })),
			 FormatException);
	snapshot.release();
	graphSnapshot.reset();
	corrupted.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, RejectsPersistedCompositeRangeIndexEntriesWithMismatchedEntityValues) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes(
			{{{"Person"}, {{"city", graph::PropertyValue("Shanghai")}, {"age", graph::PropertyValue(int64_t{30})}}}});
	ASSERT_EQ(nodes.size(), 1U);
	const LogicalGraphIndexDefinition index =
			backend.createLogicalNodeIndex({"person_city_age", "Person", {"city", "age"}});
	backend.close();

	const std::vector<graph::PropertyValue> values = {graph::PropertyValue("Shanghai"), graph::PropertyValue(int64_t{30})};
	const std::vector<std::byte> key = IndexKeyCodec::encodeCompositeEntryKey(values, nodes.front());
	overwriteBTreeLeafEntry(index.index_id, key, {key, nodes.front() + 1U});

	RelocatableStorageBackend corrupted(options());
	corrupted.open();
	RelocatableReadSnapshot snapshot = corrupted.openSnapshot();
	std::unique_ptr<LogicalGraphReadSnapshot> graphSnapshot = corrupted.openLogicalGraphReadSnapshot();
	const LogicalGraphCompositePropertyRange request{
			{{"city", graph::PropertyValue("Shanghai")}},
			{"age", graph::PropertyValue(int64_t{30}), graph::PropertyValue(), true, true}};
	EXPECT_THROW(static_cast<void>(corrupted.findNodesByCompositePropertyRange(snapshot, {"Person"}, request, 1U, 2U)),
			 FormatException);
	EXPECT_THROW(static_cast<void>(graphSnapshot->visitNodeCandidatesByCompositePropertyRange(
				{"Person"}, request, 1U, 2U,
				[](const graph::storage::IndexCandidateBatch &) { return true; })),
			 FormatException);
	snapshot.release();
	graphSnapshot.reset();
	corrupted.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, RejectsIndexCandidatePagesWithMismatchedOwners) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes(
			{{{"Person"}, {{"score", graph::PropertyValue(int64_t{42})},
																{"city", graph::PropertyValue("Shanghai")},
																{"age", graph::PropertyValue(int64_t{30})}}}});
	ASSERT_EQ(nodes.size(), 1U);
	const LogicalGraphIndexDefinition scalar = backend.createLogicalNodeIndex({"person_score", "Person", {"score"}});
	const LogicalGraphIndexDefinition composite =
			backend.createLogicalNodeIndex({"person_city_age", "Person", {"city", "age"}});
	backend.close();

    // Read-only open leaves the deliberately corrupt candidate for the
    // point/range read under test; writable recovery may reject it earlier.
    auto readOnlyOptions = options(); readOnlyOptions.open_mode = OpenMode::OPEN_READ_ONLY_EXISTING_FILE;
	mutateBTreeRootOwner(scalar.index_id, [](PageOwnerHeader &owner) { ++owner.owner_id; });
	RelocatableStorageBackend scalarCorrupted(readOnlyOptions);
	scalarCorrupted.open();
	RelocatableReadSnapshot scalarSnapshot = scalarCorrupted.openSnapshot();
	EXPECT_THROW(static_cast<void>(scalarCorrupted.findNodesByExactProperties(
				scalarSnapshot, {"Person"}, {{"score", graph::PropertyValue(int64_t{42})}}, 1U, 4U)), FormatException);
	scalarSnapshot.release();
	scalarCorrupted.close();

	mutateBTreeRootOwner(scalar.index_id, [](PageOwnerHeader &owner) { owner.kind = PageOwnerKind::POK_ENTITY; });
	RelocatableStorageBackend scalarRangeCorrupted(readOnlyOptions);
	scalarRangeCorrupted.open();
	RelocatableReadSnapshot scalarRangeSnapshot = scalarRangeCorrupted.openSnapshot();
	EXPECT_THROW(static_cast<void>(scalarRangeCorrupted.findNodesByPropertyRange(
				scalarRangeSnapshot, {"Person"},
				{"score", graph::PropertyValue(int64_t{0}), graph::PropertyValue(int64_t{100}), true, true}, 1U, 4U)),
				FormatException);
	scalarRangeSnapshot.release();
	scalarRangeCorrupted.close();

	mutateBTreeRootOwner(composite.index_id, [](PageOwnerHeader &owner) { owner.kind = PageOwnerKind::POK_ENTITY; });
	RelocatableStorageBackend compositeCorrupted(readOnlyOptions);
	compositeCorrupted.open();
	RelocatableReadSnapshot compositeSnapshot = compositeCorrupted.openSnapshot();
	const LogicalGraphCompositePropertyRange request{
			{{"city", graph::PropertyValue("Shanghai")}},
			{"age", graph::PropertyValue(int64_t{30}), graph::PropertyValue(), true, true}};
	EXPECT_THROW(static_cast<void>(compositeCorrupted.findNodesByCompositePropertyRange(
				compositeSnapshot, {"Person"}, request, 1U, 4U)), FormatException);
	compositeSnapshot.release();
	compositeCorrupted.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, RejectsIndexCandidatePagesWithChecksumCorruption) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes(
			{{{"Person"}, {{"score", graph::PropertyValue(int64_t{42})}}}});
	ASSERT_EQ(nodes.size(), 1U);
	const LogicalGraphIndexDefinition index = backend.createLogicalNodeIndex({"person_score", "Person", {"score"}});
	backend.close();
	flipBTreeRootPayloadByte(index.index_id);

	RelocatableStorageBackend corrupted(options());
	corrupted.open();
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = corrupted.openLogicalGraphReadSnapshot();
	ASSERT_NE(snapshot, nullptr);
	EXPECT_THROW(static_cast<void>(snapshot->visitNodeCandidatesByPropertyRange(
				{"Person"}, {"score", graph::PropertyValue(int64_t{0}), graph::PropertyValue(int64_t{100}), true, true}, 1U,
				4U, [](const graph::storage::IndexCandidateBatch &) { return true; })), FormatException);
	snapshot.reset();
	corrupted.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, ResolvesExactScalarAndCompositeNodeLookupsThroughPinnedIndexes) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes({{{"Person"},
																	 {{"score", graph::PropertyValue(int64_t{42})},
																	  {"city", graph::PropertyValue("Shanghai")},
																	  {"age", graph::PropertyValue(int64_t{30})}}},
																	{{"Person"},
																	 {{"score", graph::PropertyValue(int64_t{42})},
																	  {"city", graph::PropertyValue("Beijing")},
																	  {"age", graph::PropertyValue(int64_t{31})}}},
																	{{"Person"},
																	 {{"score", graph::PropertyValue(int64_t{7})},
																	  {"city", graph::PropertyValue("Shanghai")},
																	  {"age", graph::PropertyValue(int64_t{29})}}}});
	ASSERT_EQ(nodes.size(), 3U);
	static_cast<void>(backend.createLogicalNodeIndex({"person_score", "Person", {"score"}}));
	static_cast<void>(backend.createLogicalNodeIndex({"person_city_age", "Person", {"city", "age"}}));

	{
		const std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
		ASSERT_NE(snapshot, nullptr);
		const auto scoreMatches = snapshot->findNodesByExactProperties(
				{"Person"}, {{"score", graph::PropertyValue(int64_t{42})}}, 3U, 3U);
		ASSERT_TRUE(scoreMatches.has_value());
		ASSERT_EQ(scoreMatches->size(), 2U);
		EXPECT_EQ(scoreMatches->at(0).node_id, nodes.at(0));
		EXPECT_EQ(scoreMatches->at(1).node_id, nodes.at(1));
		EXPECT_THROW(static_cast<void>(snapshot->findNodesByExactProperties(
							 {"Person"}, {{"score", graph::PropertyValue(int64_t{42})}}, 1U, 3U)),
					 FormatException);
		// The overflow-safe probe must reject an unrepresentable result bound
		// instead of wrapping maximum-size + 1 to zero and returning a false miss.
		EXPECT_THROW(static_cast<void>(snapshot->findNodesByExactProperties(
							 {"Person"}, {{"score", graph::PropertyValue(int64_t{42})}},
							 std::numeric_limits<size_t>::max(), 3U)), FormatException);

		const auto compositeMatch = snapshot->findNodesByExactProperties(
				{"Person"}, {{"age", graph::PropertyValue(int64_t{30})}, {"city", graph::PropertyValue("Shanghai")}},
				3U, 3U);
		ASSERT_TRUE(compositeMatch.has_value());
		ASSERT_EQ(compositeMatch->size(), 1U);
		EXPECT_EQ(compositeMatch->front().node_id, nodes.at(0));
		const graph::storage::LogicalGraphCompositePropertyRange compositeRangeRequest{
				{{"city", graph::PropertyValue("Shanghai")}},
				{"age", graph::PropertyValue(int64_t{29}), graph::PropertyValue(int64_t{31}), true, false}};
		const auto compositeRange =
				snapshot->findNodesByCompositePropertyRange({"Person"}, compositeRangeRequest, 3U, 3U);
		ASSERT_TRUE(compositeRange.has_value());
		ASSERT_EQ(compositeRange->size(), 2U);
		EXPECT_EQ(compositeRange->at(0).node_id, nodes.at(0));
		EXPECT_EQ(compositeRange->at(1).node_id, nodes.at(2));
		const auto compositeRangeLookup = [&snapshot](graph::PropertyValue minValue, graph::PropertyValue maxValue,
																	 bool minInclusive = true, bool maxInclusive = true,
																	 size_t maximumNodes = 3U) {
			return snapshot->findNodesByCompositePropertyRange(
					{"Person"}, {{{"city", graph::PropertyValue("Shanghai")}},
								 {"age", std::move(minValue), std::move(maxValue), minInclusive, maxInclusive}},
					maximumNodes, 3U);
		};
		const auto lowerOnly = compositeRangeLookup(graph::PropertyValue(int64_t{30}), graph::PropertyValue());
		ASSERT_TRUE(lowerOnly.has_value());
		ASSERT_EQ(lowerOnly->size(), 1U);
		EXPECT_EQ(lowerOnly->front().node_id, nodes.at(0));
		const auto upperInclusive = compositeRangeLookup(graph::PropertyValue(), graph::PropertyValue(int64_t{30}));
		ASSERT_TRUE(upperInclusive.has_value());
		ASSERT_EQ(upperInclusive->size(), 2U);
		EXPECT_EQ(upperInclusive->at(0).node_id, nodes.at(0));
		EXPECT_EQ(upperInclusive->at(1).node_id, nodes.at(2));
		const auto openInterval = compositeRangeLookup(
				graph::PropertyValue(int64_t{29}), graph::PropertyValue(int64_t{31}), false, false);
		ASSERT_TRUE(openInterval.has_value());
		ASSERT_EQ(openInterval->size(), 1U);
		EXPECT_EQ(openInterval->front().node_id, nodes.at(0));
		EXPECT_THROW(static_cast<void>(compositeRangeLookup(
					 graph::PropertyValue(int64_t{29}), graph::PropertyValue(), true, true, 1U)),
				 FormatException);
		EXPECT_EQ(snapshot->findNodesByCompositePropertyRange(
						  {"Person"}, {{{"age", graph::PropertyValue(int64_t{30})}},
										  {"city", graph::PropertyValue("Shanghai"), graph::PropertyValue(), true, true}},
						  3U, 3U),
				  std::nullopt);
		const auto unknownProperty = snapshot->findNodesByCompositePropertyRange(
				{"Person"}, {{{"city", graph::PropertyValue("Shanghai")}},
							 {"missing", graph::PropertyValue(int64_t{1}), graph::PropertyValue(), true, true}},
				3U, 3U);
		ASSERT_TRUE(unknownProperty.has_value());
		EXPECT_TRUE(unknownProperty->empty());
		EXPECT_EQ(snapshot->findNodesByCompositePropertyRange(
						  {"Person"}, {{{"city", graph::PropertyValue(std::vector<graph::PropertyValue>{
										  graph::PropertyValue("Shanghai")})}},
										  {"age", graph::PropertyValue(int64_t{29}), graph::PropertyValue(), true, true}},
						  3U, 3U),
				  std::nullopt);
		const auto missing = snapshot->findNodesByExactProperties(
				{"Person"}, {{"score", graph::PropertyValue(int64_t{999})}}, 3U, 3U);
		ASSERT_TRUE(missing.has_value());
		EXPECT_TRUE(missing->empty());
		EXPECT_EQ(
				snapshot->findNodesByExactProperties({"Person"},
													 {{"score", graph::PropertyValue(std::vector<graph::PropertyValue>{
																		graph::PropertyValue(int64_t{42})})}},
													 3U, 3U),
				std::nullopt);
	}
	backend.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, ReusesTheSnapshotReadSessionForExactIndexCandidates) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes(
			{{{"Person"}, {{"score", graph::PropertyValue(int64_t{42})}}},
			 {{"Person"}, {{"score", graph::PropertyValue(int64_t{42})}}}});
	ASSERT_EQ(nodes.size(), 2U);
	static_cast<void>(backend.createLogicalNodeIndex({"person_score", "Person", {"score"}}));

	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(snapshot, nullptr);
	EXPECT_TRUE(snapshot->supportsNativeExactNodeCandidates());
	const bool valuesWereEnabled = graph::debug::PerfTrace::areValuesEnabled();
	graph::debug::PerfTrace::reset();
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	const auto first = snapshot->findNodeCandidatesByExactProperties(
			{"Person"}, {{"score", graph::PropertyValue(int64_t{42})}}, 2U, 1U);
	const auto second = snapshot->findNodeCandidatesByExactProperties(
			{"Person"}, {{"score", graph::PropertyValue(int64_t{42})}}, 2U, 1U);
	const graph::debug::PerfTrace::Snapshot trace = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setValueCollectionEnabled(valuesWereEnabled);

	ASSERT_TRUE(first.has_value());
	ASSERT_TRUE(second.has_value());
	ASSERT_TRUE(snapshot->nativeSnapshotGeneration().has_value());
	EXPECT_EQ(first->snapshot_generation, *snapshot->nativeSnapshotGeneration());
	EXPECT_EQ(second->snapshot_generation, *snapshot->nativeSnapshotGeneration());
	EXPECT_TRUE(first->exact_match_verified);
	EXPECT_TRUE(second->exact_match_verified);
	ASSERT_EQ(first->candidates.size(), nodes.size());
	EXPECT_EQ(first->candidates, second->candidates);
	EXPECT_EQ(first->candidates.at(0).entity_id, nodes.at(0));
	EXPECT_EQ(first->candidates.at(1).entity_id, nodes.at(1));
	ASSERT_TRUE(trace.contains("native_graph.read.index_candidate_label_token_resolution"));
	ASSERT_TRUE(trace.contains("native_graph.read.index_candidate_constraint_definition_read"));
	ASSERT_TRUE(trace.contains("native_graph.read.index_candidate_definition_read"));
	ASSERT_TRUE(trace.contains("native_graph.read.index_candidate_property_token_resolution"));
	ASSERT_TRUE(trace.contains("native_graph.read.property_selected_column_batch_owners"));
	EXPECT_EQ(trace.at("native_graph.read.index_candidate_label_token_resolution").totalValue, 1);
	EXPECT_EQ(trace.at("native_graph.read.index_candidate_property_token_resolution").totalValue, 1);
	EXPECT_EQ(trace.at("native_graph.read.index_candidate_constraint_definition_read").totalValue, 1);
	EXPECT_EQ(trace.at("native_graph.read.index_candidate_definition_read").totalValue, 1);
	EXPECT_EQ(trace.at("native_graph.read.property_selected_column_batch_owners").totalValue,
			  static_cast<int64_t>(nodes.size() * 2U));

	snapshot.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, ReusesTheSnapshotReadSessionForScalarRangeIndexes) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes(
			{{{"Person"}, {{"score", graph::PropertyValue(int64_t{7})}}},
			 {{"Person"}, {{"score", graph::PropertyValue(int64_t{42})}}}});
	ASSERT_EQ(nodes.size(), 2U);
	static_cast<void>(backend.createLogicalNodeIndex({"person_score", "Person", {"score"}}));

	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(snapshot, nullptr);
	const bool valuesWereEnabled = graph::debug::PerfTrace::areValuesEnabled();
	graph::debug::PerfTrace::reset();
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	const auto first = snapshot->findNodesByPropertyRange(
			{"Person"}, {"score", graph::PropertyValue(int64_t{7}), graph::PropertyValue(), true, true}, 2U, 1U);
	const auto second = snapshot->findNodesByPropertyRange(
			{"Person"}, {"score", graph::PropertyValue(int64_t{7}), graph::PropertyValue(), true, true}, 2U, 1U);
	const graph::debug::PerfTrace::Snapshot trace = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setValueCollectionEnabled(valuesWereEnabled);

	ASSERT_TRUE(first.has_value());
	ASSERT_TRUE(second.has_value());
	ASSERT_EQ(first->size(), nodes.size());
	EXPECT_EQ(*first, *second);
	ASSERT_TRUE(trace.contains("native_graph.read.range_index_label_token_resolution"));
	ASSERT_TRUE(trace.contains("native_graph.read.range_index_property_token_resolution"));
	ASSERT_TRUE(trace.contains("native_graph.read.range_index_definition_read"));
	ASSERT_TRUE(trace.contains("native_graph.read.index_candidate_root_descent"));
	ASSERT_TRUE(trace.contains("native_graph.read.property_selected_batch_owners"));
	EXPECT_EQ(trace.at("native_graph.read.range_index_label_token_resolution").totalValue, 1);
	EXPECT_EQ(trace.at("native_graph.read.range_index_property_token_resolution").totalValue, 1);
	EXPECT_EQ(trace.at("native_graph.read.range_index_definition_read").totalValue, 1);
	EXPECT_EQ(trace.at("native_graph.read.index_candidate_root_descent").totalValue, 2);
	// The first range lookup fills the snapshot-owned selected-property cache;
	// the repeated lookup reuses those values and performs no second owner batch.
	EXPECT_EQ(trace.at("native_graph.read.property_selected_batch_owners").totalValue, 2);

	snapshot.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, ScalarRangeVerificationUsesOneGlobalPropertyBatch) {
	RelocatableStorageBackend backend(options());
	backend.open();
	std::vector<LogicalGraphNodeCreateRequest> requests;
	requests.reserve(300U);
	for (int64_t score = 1; score <= 300; ++score) {
		requests.push_back({{"Person"}, {{"score", graph::PropertyValue(score)}}});
	}
	const std::vector<uint64_t> nodes = backend.createLogicalNodes(requests);
	ASSERT_EQ(nodes.size(), requests.size());
	static_cast<void>(backend.createLogicalNodeIndex({"person_score", "Person", {"score"}}));
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(snapshot, nullptr);
	const bool valuesWereEnabled = graph::debug::PerfTrace::areValuesEnabled();
	graph::debug::PerfTrace::reset();
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	const auto candidates = snapshot->findNodeCandidatesByPropertyRange(
			{"Person"}, {"score", graph::PropertyValue(int64_t{1}), graph::PropertyValue(int64_t{300}), true, true},
			requests.size(), 1U);
	const graph::debug::PerfTrace::Snapshot trace = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setValueCollectionEnabled(valuesWereEnabled);
	ASSERT_TRUE(candidates.has_value());
	ASSERT_EQ(candidates->candidates.size(), requests.size());
	ASSERT_EQ(candidates->verified_property_columns.size(), 1U);
	EXPECT_EQ(candidates->verified_property_columns.front().values.size(), requests.size());
	ASSERT_TRUE(trace.contains("native_graph.read.property_selected_batch_owners"));
	EXPECT_EQ(trace.at("native_graph.read.property_selected_batch_owners").totalValue,
			  static_cast<int64_t>(requests.size()));
	ASSERT_TRUE(trace.contains("native_graph.read.directory_range_cursor_root_descents"));
	// Label validation and Property validation use separate authenticated
	// directory namespaces; each is descended once for the complete batch.
	EXPECT_LE(trace.at("native_graph.read.directory_range_cursor_root_descents").totalValue, 2);
	ASSERT_TRUE(trace.contains("native_graph.read.index_candidate_liveness_probes"));
	ASSERT_TRUE(trace.contains("native_graph.read.index_candidate_verified_rows"));
	EXPECT_EQ(trace.at("native_graph.read.index_candidate_liveness_probes").totalValue,
			  static_cast<int64_t>(requests.size()));
	EXPECT_EQ(trace.at("native_graph.read.index_candidate_verified_rows").totalValue,
			  static_cast<int64_t>(requests.size()));
	snapshot.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, ScalarRangeCandidateStreamStopsBeforeReadingLaterBatches) {
	RelocatableStorageBackend backend(options());
	backend.open();
	std::vector<LogicalGraphNodeCreateRequest> requests;
	requests.reserve(600U);
	for (int64_t score = 1; score <= 600; ++score) {
		requests.push_back({{"Person"}, {{"score", graph::PropertyValue(score)}}});
	}
	const std::vector<uint64_t> nodes = backend.createLogicalNodes(requests);
	ASSERT_EQ(nodes.size(), requests.size());
	static_cast<void>(backend.createLogicalNodeIndex({"person_score", "Person", {"score"}}));
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(snapshot, nullptr);
	ASSERT_TRUE(snapshot->supportsNativeRangeNodeCandidates());

	const bool valuesWereEnabled = graph::debug::PerfTrace::areValuesEnabled();
	graph::debug::PerfTrace::reset();
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	size_t callbackCount = 0U;
	size_t callbackEntries = 0U;
	const auto stopped = snapshot->visitNodeCandidatesByPropertyRange(
			{"Person"}, {"score", graph::PropertyValue(int64_t{1}), graph::PropertyValue(int64_t{600}), true, true},
			requests.size(), 1U, [&](const graph::storage::IndexCandidateBatch &batch) -> bool {
				++callbackCount;
				callbackEntries += batch.candidates.size();
				EXPECT_EQ(batch.snapshot_generation, snapshot->nativeSnapshotGeneration().value());
				EXPECT_TRUE(batch.predicates_verified);
				EXPECT_EQ(batch.verified_property_columns.size(), 1U);
				if (batch.verified_property_columns.size() != 1U) {
					return false;
				}
				EXPECT_EQ(batch.verified_property_columns.front().values.size(), batch.candidates.size());
				EXPECT_TRUE(std::ranges::is_sorted(batch.candidates, {}, &graph::storage::EntityRef::entity_id));
				return false;
			});
		const graph::debug::PerfTrace::Snapshot trace = graph::debug::PerfTrace::snapshotAndReset();
		graph::debug::PerfTrace::setValueCollectionEnabled(valuesWereEnabled);

	ASSERT_TRUE(stopped.has_value());
	EXPECT_FALSE(*stopped);
	EXPECT_EQ(callbackCount, 1U);
	EXPECT_EQ(callbackEntries, graph::storage::BTreeIndex::DEFAULT_CURSOR_BATCH_ENTRIES);
	ASSERT_TRUE(trace.contains("native_graph.read.index_candidate_root_descent"));
	EXPECT_EQ(trace.at("native_graph.read.index_candidate_root_descent").totalValue, 1);
	// Early termination must not validate a second Property batch or read the
	// remaining index leaves after the first bounded batch was accepted.
	ASSERT_TRUE(trace.contains("native_graph.read.property_selected_batch_owners"));
	EXPECT_EQ(trace.at("native_graph.read.property_selected_batch_owners").totalValue,
			static_cast<int64_t>(graph::storage::BTreeIndex::DEFAULT_CURSOR_BATCH_ENTRIES));

	snapshot.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, CandidateStreamsEnforceNodeResultBounds) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::vector<LogicalGraphNodeCreateRequest> requests{
			{{"Person"}, {{"score", graph::PropertyValue(int64_t{7})},
								 {"city", graph::PropertyValue("Shanghai")}, {"age", graph::PropertyValue(int64_t{7})}}},
			{{"Person"}, {{"score", graph::PropertyValue(int64_t{7})},
								 {"city", graph::PropertyValue("Shanghai")}, {"age", graph::PropertyValue(int64_t{42})}}}};
	const std::vector<uint64_t> nodes = backend.createLogicalNodes(requests);
	ASSERT_EQ(nodes.size(), 2U);
	static_cast<void>(backend.createLogicalNodeIndex({"person_score", "Person", {"score"}}));
	static_cast<void>(backend.createLogicalNodeIndex({"person_city_age", "Person", {"city", "age"}}));
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(snapshot, nullptr);

	const auto acceptAll = [](const graph::storage::IndexCandidateBatch &) { return true; };
	EXPECT_THROW(static_cast<void>(snapshot->visitNodeCandidatesByExactProperties(
			{"Person"}, {{"score", graph::PropertyValue(int64_t{7})}}, 1U, 8U, acceptAll)), FormatException);
	EXPECT_THROW(static_cast<void>(snapshot->visitNodeCandidatesByPropertyRange(
			{"Person"}, {"score", graph::PropertyValue(int64_t{7}), graph::PropertyValue(int64_t{7}), true, true},
			1U, 8U, acceptAll)), FormatException);
	const LogicalGraphCompositePropertyRange composite{
			{{"city", graph::PropertyValue("Shanghai")}},
			{"age", graph::PropertyValue(int64_t{0}), graph::PropertyValue(int64_t{100}), true, true}};
	EXPECT_THROW(static_cast<void>(snapshot->visitNodeCandidatesByCompositePropertyRange(
			{"Person"}, composite, 1U, 8U, acceptAll)), FormatException);

	snapshot.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, CandidateStreamsEnforceProducerByteBounds) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::string largeValue(256U, 'x');
	const std::vector<uint64_t> nodes = backend.createLogicalNodes({
			{{"Person"}, {{"code", graph::PropertyValue(largeValue)},
								 {"city", graph::PropertyValue(largeValue)}, {"age", graph::PropertyValue(int64_t{7})}}}});
	ASSERT_EQ(nodes.size(), 1U);
	static_cast<void>(backend.createLogicalNodeIndex({"person_code", "Person", {"code"}}));
	static_cast<void>(backend.createLogicalNodeIndex({"person_city_age", "Person", {"city", "age"}}));
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(snapshot, nullptr);
	const auto acceptAll = [](const graph::storage::IndexCandidateBatch &) { return true; };
	EXPECT_THROW(static_cast<void>(snapshot->visitNodeCandidatesByExactProperties(
			{"Person"}, {{"code", graph::PropertyValue(largeValue)}}, 1U, 8U, 1U, acceptAll)), FormatException);
	EXPECT_THROW(static_cast<void>(snapshot->visitNodeCandidatesByPropertyRange(
			{"Person"}, {"code", graph::PropertyValue(std::string("a")), graph::PropertyValue(), true, true}, 1U, 8U,
			1U, acceptAll)), FormatException);
	const LogicalGraphCompositePropertyRange composite{
			{{"city", graph::PropertyValue(largeValue)}},
			{"age", graph::PropertyValue(int64_t{0}), graph::PropertyValue(int64_t{10}), true, true}};
	EXPECT_THROW(static_cast<void>(snapshot->visitNodeCandidatesByCompositePropertyRange(
			{"Person"}, composite, 1U, 8U, 1U, acceptAll)), FormatException);
	snapshot.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, RangeIndexCandidatesRetainOnlyVerifiedIndexedProperties) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes(
			{{{"Person"}, {{"score", graph::PropertyValue(int64_t{7})},
								{"unused", graph::PropertyValue("large payload")}}},
			 { {"Person"}, {{"score", graph::PropertyValue(int64_t{42})},
								{"unused", graph::PropertyValue("another payload")}}}});
	ASSERT_EQ(nodes.size(), 2U);
	static_cast<void>(backend.createLogicalNodeIndex({"person_score", "Person", {"score"}}));
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(snapshot, nullptr);
	ASSERT_TRUE(snapshot->supportsNativeRangeNodeCandidates());
	const auto candidates = snapshot->findNodeCandidatesByPropertyRange(
			{"Person"}, {"score", graph::PropertyValue(int64_t{7}), graph::PropertyValue(), true, true}, 2U, 2U);
	ASSERT_TRUE(candidates.has_value());
	EXPECT_TRUE(candidates->predicates_verified);
	EXPECT_FALSE(candidates->exact_match_verified);
	ASSERT_EQ(candidates->candidates.size(), nodes.size());
	ASSERT_EQ(candidates->verified_property_columns.size(), 1U);
	EXPECT_EQ(candidates->verified_property_columns.front().key, "score");
	ASSERT_EQ(candidates->verified_property_columns.front().values.size(), nodes.size());
	for (size_t index = 0U; index < nodes.size(); ++index) {
		EXPECT_EQ(candidates->verified_property_columns.front().values[index],
				  graph::PropertyValue(int64_t{index == 0U ? 7 : 42}));
	}
	EXPECT_EQ(candidates->candidates.front().entity_id, nodes.front());
	snapshot.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, ScalarRangeCandidateStreamConsumesMultipleBatchesInLogicalOrder) {
	RelocatableStorageBackend backend(options());
	backend.open();
	std::vector<LogicalGraphNodeCreateRequest> requests;
	requests.reserve(513U);
	for (int64_t score = 1; score <= 513; ++score) {
		requests.push_back({{"Person"}, {{"score", graph::PropertyValue(score)}}});
	}
	const std::vector<uint64_t> nodes = backend.createLogicalNodes(requests);
	ASSERT_EQ(nodes.size(), requests.size());
	static_cast<void>(backend.createLogicalNodeIndex({"person_score", "Person", {"score"}}));
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(snapshot, nullptr);
	size_t callbackCount = 0U;
	size_t callbackEntries = 0U;
	uint64_t previousInBatch = 0U;
	const auto completed = snapshot->visitNodeCandidatesByPropertyRange(
			{"Person"}, {"score", graph::PropertyValue(int64_t{1}), graph::PropertyValue(int64_t{513}), true, true},
			requests.size(), 1U, [&](const graph::storage::IndexCandidateBatch &batch) {
				++callbackCount;
				callbackEntries += batch.candidates.size();
				uint64_t previous = 0U;
				for (size_t index = 0U; index < batch.candidates.size(); ++index) {
					EXPECT_GT(batch.candidates[index].entity_id, previous);
					previous = batch.candidates[index].entity_id;
					EXPECT_EQ(batch.verified_property_columns.front().values[index],
							graph::PropertyValue(static_cast<int64_t>(batch.candidates[index].entity_id)));
				}
				EXPECT_GT(previous, previousInBatch);
				previousInBatch = previous;
				return true;
			});
	ASSERT_TRUE(completed.has_value());
	EXPECT_TRUE(*completed);
	EXPECT_EQ(callbackCount, 3U);
	EXPECT_EQ(callbackEntries, requests.size());
	snapshot.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, ExactCandidateStreamDescendsIndexRootOnceAcrossBatches) {
	RelocatableStorageBackend backend(options());
	backend.open();
	std::vector<LogicalGraphNodeCreateRequest> requests;
	requests.reserve(600U);
	for (size_t index = 0U; index < 600U; ++index) {
		requests.push_back({{"Person"}, {{"score", graph::PropertyValue(int64_t{7})}}});
	}
	const std::vector<uint64_t> nodes = backend.createLogicalNodes(requests);
	ASSERT_EQ(nodes.size(), requests.size());
	static_cast<void>(backend.createLogicalNodeIndex({"person_score", "Person", {"score"}}));
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(snapshot, nullptr);
	const bool valuesWereEnabled = graph::debug::PerfTrace::areValuesEnabled();
	graph::debug::PerfTrace::reset();
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	size_t callbackCount = 0U;
	size_t candidateCount = 0U;
	const auto completed = snapshot->visitNodeCandidatesByExactProperties(
			{"Person"}, {{"score", graph::PropertyValue(int64_t{7})}}, requests.size(), 1U,
			[&](const graph::storage::IndexCandidateBatch &batch) {
				++callbackCount;
				candidateCount += batch.candidates.size();
				EXPECT_TRUE(batch.exact_match_verified);
				EXPECT_TRUE(batch.predicates_verified);
				EXPECT_EQ(batch.verified_property_columns.size(), 1U);
				if (batch.verified_property_columns.size() != 1U) {
					return false;
				}
				EXPECT_EQ(batch.candidates.size(), batch.verified_property_columns.front().values.size());
				EXPECT_TRUE(std::ranges::is_sorted(batch.candidates, {}, &graph::storage::EntityRef::entity_id));
				return true;
			});
	const graph::debug::PerfTrace::Snapshot trace = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setValueCollectionEnabled(valuesWereEnabled);
	ASSERT_TRUE(completed.has_value());
	EXPECT_TRUE(*completed);
	EXPECT_GT(callbackCount, 1U);
	EXPECT_EQ(candidateCount, nodes.size());
	ASSERT_TRUE(trace.contains("native_graph.read.index_candidate_root_descent"));
	EXPECT_EQ(trace.at("native_graph.read.index_candidate_root_descent").totalValue, 1);
	ASSERT_TRUE(trace.contains("native_graph.read.index_candidate_batches"));
	EXPECT_EQ(trace.at("native_graph.read.index_candidate_batches").totalValue,
			static_cast<int64_t>(callbackCount));
	ASSERT_TRUE(trace.contains("native_graph.read.index_candidate_emitted_rows"));
	EXPECT_EQ(trace.at("native_graph.read.index_candidate_emitted_rows").totalValue,
			static_cast<int64_t>(nodes.size()));
	snapshot.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, ScalarRangeCandidateStreamPreservesOrderWithinValueBatches) {
	RelocatableStorageBackend backend(options());
	backend.open();
	std::vector<LogicalGraphNodeCreateRequest> requests;
	requests.reserve(600U);
	for (int64_t score = 600; score >= 1; --score) {
		requests.push_back({{"Person"}, {{"score", graph::PropertyValue(score)}}});
	}
	const std::vector<uint64_t> nodes = backend.createLogicalNodes(requests);
	ASSERT_EQ(nodes.size(), requests.size());
	static_cast<void>(backend.createLogicalNodeIndex({"person_score", "Person", {"score"}}));
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(snapshot, nullptr);
	std::vector<uint64_t> observed;
	const auto completed = snapshot->visitNodeCandidatesByPropertyRange(
			{"Person"}, {"score", graph::PropertyValue(int64_t{1}), graph::PropertyValue(int64_t{600}), true, true},
			requests.size(), 1U, 1U << 20U, [&](const graph::storage::IndexCandidateBatch &batch) {
				EXPECT_TRUE(std::ranges::is_sorted(batch.candidates, {}, &EntityRef::entity_id));
				for (const EntityRef &candidate: batch.candidates) {
					observed.push_back(candidate.entity_id);
				}
				return true;
			});
	ASSERT_TRUE(completed.has_value());
	EXPECT_TRUE(*completed);
	std::vector<uint64_t> expected = nodes;
	std::ranges::sort(observed);
	std::ranges::sort(expected);
	EXPECT_EQ(observed, expected);
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();
}

TEST_F(RelocatableStorageBackendSchemaTest,
		CandidateStreamsRemainBoundToTheirSnapshotAcrossIndexUpdates) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::vector<uint64_t> initialNodes = backend.createLogicalNodes(
			{{{"Person"}, {{"score", graph::PropertyValue(int64_t{7})},
									 {"city", graph::PropertyValue("Shanghai")},
									 {"age", graph::PropertyValue(int64_t{7})}}}});
	ASSERT_EQ(initialNodes.size(), 1U);
	static_cast<void>(backend.createLogicalNodeIndex({"person_score", "Person", {"score"}}));
	static_cast<void>(backend.createLogicalNodeIndex({"person_city_age", "Person", {"city", "age"}}));

	std::unique_ptr<LogicalGraphReadSnapshot> oldSnapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(oldSnapshot, nullptr);
	ASSERT_TRUE(oldSnapshot->nativeSnapshotGeneration().has_value());
	const uint64_t oldGeneration = *oldSnapshot->nativeSnapshotGeneration();

	const std::vector<uint64_t> laterNodes = backend.createLogicalNodes(
			{{{"Person"}, {{"score", graph::PropertyValue(int64_t{7})},
									  {"city", graph::PropertyValue("Shanghai")},
									  {"age", graph::PropertyValue(int64_t{42})}}}});
	ASSERT_EQ(laterNodes.size(), 1U);

	const LogicalGraphPropertyRange scalarRange{
			"score", graph::PropertyValue(int64_t{0}), graph::PropertyValue(int64_t{100}), true, true};
	const LogicalGraphCompositePropertyRange compositeRange{
			{{"city", graph::PropertyValue("Shanghai")} },
			{"age", graph::PropertyValue(int64_t{0}), graph::PropertyValue(int64_t{100}), true, true}};
	const auto collectExact = [](const LogicalGraphReadSnapshot &snapshot) -> std::vector<uint64_t> {
		std::vector<uint64_t> ids;
		const auto completed = snapshot.visitNodeCandidatesByExactProperties(
				{"Person"}, {{"score", graph::PropertyValue(int64_t{7})}}, 16U, 16U,
				[&](const graph::storage::IndexCandidateBatch &batch) -> bool {
					if (!batch.exact_match_verified || !batch.predicates_verified ||
							batch.candidates.size() != batch.verified_property_columns.front().values.size()) {
						return false;
					}
					EXPECT_EQ(batch.snapshot_generation, snapshot.nativeSnapshotGeneration().value());
					for (size_t index = 0U; index < batch.candidates.size(); ++index) {
						if (!ids.empty()) {
							EXPECT_LT(ids.back(), batch.candidates[index].entity_id);
						}
						ids.push_back(batch.candidates[index].entity_id);
					}
					return true;
				});
		if (!completed.has_value()) {
			ADD_FAILURE() << "exact candidate stream unexpectedly declined";
			return {};
		}
		EXPECT_TRUE(*completed);
		return ids;
	};

	const auto collectScalar = [](const LogicalGraphReadSnapshot &snapshot,
									 const LogicalGraphPropertyRange &range) -> std::vector<uint64_t> {
			std::vector<uint64_t> ids;
			const auto completed = snapshot.visitNodeCandidatesByPropertyRange(
					{"Person"}, range, 16U, 16U, [&](const graph::storage::IndexCandidateBatch &batch) -> bool {
						EXPECT_TRUE(batch.predicates_verified);
						EXPECT_TRUE(batch.label_scope_verified);
						EXPECT_EQ(batch.snapshot_generation, snapshot.nativeSnapshotGeneration().value());
						if (batch.verified_property_columns.size() != 1U ||
								batch.candidates.size() != batch.verified_property_columns.front().values.size()) {
							return false;
						}
						for (size_t index = 0U; index < batch.candidates.size(); ++index) {
							if (!ids.empty()) {
								EXPECT_LT(ids.back(), batch.candidates[index].entity_id);
							}
							ids.push_back(batch.candidates[index].entity_id);
							EXPECT_EQ(batch.verified_property_columns.front().values[index].getType(),
															  graph::PropertyType::INTEGER);
						}
						return true;
					});
		if (!completed.has_value()) {
			ADD_FAILURE() << "scalar candidate stream unexpectedly declined";
			return {};
		}
		EXPECT_TRUE(*completed);
		return ids;
	};
	const auto collectComposite = [](const LogicalGraphReadSnapshot &snapshot,
									 const LogicalGraphCompositePropertyRange &range) -> std::vector<uint64_t> {
			std::vector<uint64_t> ids;
			const auto completed = snapshot.visitNodeCandidatesByCompositePropertyRange(
					{"Person"}, range, 16U, 16U, [&](const graph::storage::IndexCandidateBatch &batch) -> bool {
						EXPECT_TRUE(batch.predicates_verified);
						EXPECT_EQ(batch.snapshot_generation, snapshot.nativeSnapshotGeneration().value());
						if (batch.verified_property_columns.size() != 2U ||
								batch.candidates.size() != batch.verified_property_columns[0].values.size() ||
								batch.candidates.size() != batch.verified_property_columns[1].values.size()) {
							return false;
						}
						for (size_t index = 0U; index < batch.candidates.size(); ++index) {
							if (!ids.empty()) {
								EXPECT_LT(ids.back(), batch.candidates[index].entity_id);
							}
							ids.push_back(batch.candidates[index].entity_id);
							EXPECT_EQ(batch.verified_property_columns[0].values[index], graph::PropertyValue("Shanghai"));
						}
						return true;
					});
		if (!completed.has_value()) {
			ADD_FAILURE() << "composite candidate stream unexpectedly declined";
			return {};
		}
		EXPECT_TRUE(*completed);
		return ids;
	};

	const std::vector<uint64_t> oldExact = collectExact(*oldSnapshot);
	const std::vector<uint64_t> oldScalar = collectScalar(*oldSnapshot, scalarRange);
	const std::vector<uint64_t> oldComposite = collectComposite(*oldSnapshot, compositeRange);
	EXPECT_EQ(oldExact, initialNodes);
	EXPECT_EQ(oldScalar, initialNodes);
	EXPECT_EQ(oldComposite, initialNodes);

	std::unique_ptr<LogicalGraphReadSnapshot> currentSnapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(currentSnapshot, nullptr);
	ASSERT_TRUE(currentSnapshot->nativeSnapshotGeneration().has_value());
	EXPECT_GT(*currentSnapshot->nativeSnapshotGeneration(), oldGeneration);
	const std::vector<uint64_t> currentExact = collectExact(*currentSnapshot);
	const std::vector<uint64_t> currentScalar = collectScalar(*currentSnapshot, scalarRange);
	const std::vector<uint64_t> currentComposite = collectComposite(*currentSnapshot, compositeRange);
	EXPECT_EQ(currentExact, (std::vector<uint64_t>{initialNodes.front(), laterNodes.front()}));
	EXPECT_EQ(currentScalar, (std::vector<uint64_t>{initialNodes.front(), laterNodes.front()}));
	EXPECT_EQ(currentComposite, (std::vector<uint64_t>{initialNodes.front(), laterNodes.front()}));

	currentSnapshot.reset();
	oldSnapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();
}

TEST_F(RelocatableStorageBackendSchemaTest,
		CandidateStreamKeepsConcurrentWriterInvisibleToPinnedSnapshot) {
	RelocatableStorageBackend backend(options());
	backend.open();
	std::vector<LogicalGraphNodeCreateRequest> initialRequests;
	initialRequests.reserve(128U);
	for (int64_t score = 1; score <= 128; ++score) {
		initialRequests.push_back({{"Person"}, {{"score", graph::PropertyValue(score)}}});
	}
	const std::vector<uint64_t> initialNodes = backend.createLogicalNodes(initialRequests);
	ASSERT_EQ(initialNodes.size(), initialRequests.size());
	static_cast<void>(backend.createLogicalNodeIndex({"person_score", "Person", {"score"}}));
	std::unique_ptr<LogicalGraphReadSnapshot> oldSnapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(oldSnapshot, nullptr);

	std::vector<uint64_t> observedByOldSnapshot;
	std::exception_ptr readerFailure;
	std::atomic<bool> readerStarted{false};
	std::thread reader([&] {
		try {
			readerStarted.store(true, std::memory_order_release);
			const auto result = oldSnapshot->findNodeCandidatesByPropertyRange(
					{"Person"}, {"score", graph::PropertyValue(int64_t{1}), graph::PropertyValue(int64_t{256}), true, true},
					256U, 1U);
			if (!result.has_value()) {
				throw std::runtime_error("concurrent candidate lookup unexpectedly declined");
			}
			for (const EntityRef &candidate: result->candidates) {
				observedByOldSnapshot.push_back(candidate.entity_id);
			}
		} catch (...) {
			readerFailure = std::current_exception();
		}
	});
	while (!readerStarted.load(std::memory_order_acquire)) {
		std::this_thread::yield();
	}

	const std::vector<uint64_t> laterNodes = backend.createLogicalNodes(
			{{{"Person"}, {{"score", graph::PropertyValue(int64_t{200})}}},
			 { {"Person"}, {{"score", graph::PropertyValue(int64_t{201})}}}});
	ASSERT_EQ(laterNodes.size(), 2U);
	reader.join();
	if (readerFailure != nullptr) {
		std::rethrow_exception(readerFailure);
	}

	EXPECT_EQ(observedByOldSnapshot, initialNodes);
	std::unique_ptr<LogicalGraphReadSnapshot> currentSnapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(currentSnapshot, nullptr);
	const auto current = currentSnapshot->findNodeCandidatesByPropertyRange(
			{"Person"}, {"score", graph::PropertyValue(int64_t{1}), graph::PropertyValue(int64_t{256}), true, true},
			256U, 1U);
	ASSERT_TRUE(current.has_value());
	EXPECT_EQ(current->candidates.size(), initialNodes.size() + laterNodes.size());
	currentSnapshot.reset();
	oldSnapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();
}

TEST_F(RelocatableStorageBackendSchemaTest,
		CandidateStreamsApplyLivenessWithoutBreakingAnOlderSnapshot) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes({
			{{"Person"}, {{"score", graph::PropertyValue(int64_t{7})},
										{"city", graph::PropertyValue("Shanghai")}, {"age", graph::PropertyValue(int64_t{7})}}},
			{{"Person"}, {{"score", graph::PropertyValue(int64_t{7})},
										{"city", graph::PropertyValue("Shanghai")}, {"age", graph::PropertyValue(int64_t{7})}}}});
	ASSERT_EQ(nodes.size(), 2U);
	static_cast<void>(backend.createLogicalNodeIndex({"person_score", "Person", {"score"}}));
	static_cast<void>(backend.createLogicalNodeIndex({"person_city_age", "Person", {"city", "age"}}));
	std::unique_ptr<LogicalGraphReadSnapshot> oldSnapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(oldSnapshot, nullptr);
	backend.deleteLogicalNodes({{nodes.back(), false, 0U}});

	const LogicalGraphPropertyRange range{
			"score", graph::PropertyValue(int64_t{7}), graph::PropertyValue(int64_t{7}), true, true};
	const LogicalGraphCompositePropertyRange compositeRange{
			{{"city", graph::PropertyValue("Shanghai")} },
			{"age", graph::PropertyValue(int64_t{7}), graph::PropertyValue(int64_t{7}), true, true}};
	const auto collectExact = [&](const LogicalGraphReadSnapshot &snapshot) {
		std::vector<uint64_t> ids;
		const auto completed = snapshot.visitNodeCandidatesByExactProperties(
				{"Person"}, {{"score", graph::PropertyValue(int64_t{7})}}, 8U, 8U,
				[&](const graph::storage::IndexCandidateBatch &batch) -> bool {
					if (!batch.exact_match_verified || !batch.predicates_verified ||
							batch.verified_property_columns.size() != 1U ||
							batch.candidates.size() != batch.verified_property_columns.front().values.size()) {
						return false;
					}
					for (size_t index = 0U; index < batch.candidates.size(); ++index) {
						ids.push_back(batch.candidates[index].entity_id);
						EXPECT_EQ(batch.verified_property_columns.front().values[index], graph::PropertyValue(int64_t{7}));
					}
					return true;
				});
		if (!completed.has_value()) {
			ADD_FAILURE() << "exact liveness candidate stream unexpectedly declined";
			return std::vector<uint64_t>{};
		}
		EXPECT_TRUE(*completed);
		return ids;
	};
	const auto collect = [&](const LogicalGraphReadSnapshot &snapshot) {
		std::vector<uint64_t> ids;
		const auto completed = snapshot.visitNodeCandidatesByPropertyRange(
				{"Person"}, range, 8U, 8U, [&](const graph::storage::IndexCandidateBatch &batch) -> bool {
					if (!batch.predicates_verified || batch.snapshot_generation == 0U ||
							batch.verified_property_columns.size() != 1U ||
							batch.candidates.size() != batch.verified_property_columns.front().values.size()) {
						return false;
					}
					for (size_t index = 0U; index < batch.candidates.size(); ++index) {
						ids.push_back(batch.candidates[index].entity_id);
						EXPECT_EQ(batch.verified_property_columns.front().values[index], graph::PropertyValue(int64_t{7}));
					}
					return true;
				});
		if (!completed.has_value()) {
			ADD_FAILURE() << "liveness candidate stream unexpectedly declined";
			return std::vector<uint64_t>{};
		}
		EXPECT_TRUE(*completed);
		return ids;
	};
	const auto collectComposite = [&](const LogicalGraphReadSnapshot &snapshot) {
		std::vector<uint64_t> ids;
		const auto completed = snapshot.visitNodeCandidatesByCompositePropertyRange(
				{"Person"}, compositeRange, 8U, 8U, [&](const graph::storage::IndexCandidateBatch &batch) {
					if (batch.verified_property_columns.size() != 2U ||
							batch.candidates.size() != batch.verified_property_columns[0].values.size() ||
							batch.candidates.size() != batch.verified_property_columns[1].values.size()) {
						return false;
					}
					for (size_t index = 0U; index < batch.candidates.size(); ++index) {
						ids.push_back(batch.candidates[index].entity_id);
						EXPECT_EQ(batch.verified_property_columns[0].values[index], graph::PropertyValue("Shanghai"));
					}
					return true;
				});
		if (!completed.has_value()) {
			ADD_FAILURE() << "composite liveness candidate stream unexpectedly declined";
			return ids;
		}
		EXPECT_TRUE(*completed);
		return ids;
	};

	EXPECT_EQ(collectExact(*oldSnapshot), nodes);
	EXPECT_EQ(collect(*oldSnapshot), nodes);
	EXPECT_EQ(collectComposite(*oldSnapshot), nodes);
	std::unique_ptr<LogicalGraphReadSnapshot> currentSnapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(currentSnapshot, nullptr);
	EXPECT_EQ(collectExact(*currentSnapshot), std::vector<uint64_t>{nodes.front()});
	EXPECT_EQ(collect(*currentSnapshot), std::vector<uint64_t>{nodes.front()});
	EXPECT_EQ(collectComposite(*currentSnapshot), std::vector<uint64_t>{nodes.front()});
	currentSnapshot.reset();
	oldSnapshot.reset();
	backend.close();

	RelocatableStorageBackend reopened(options());
	reopened.open();
	std::unique_ptr<LogicalGraphReadSnapshot> reopenedSnapshot = reopened.openLogicalGraphReadSnapshot();
	ASSERT_NE(reopenedSnapshot, nullptr);
	EXPECT_EQ(collectExact(*reopenedSnapshot), std::vector<uint64_t>{nodes.front()});
	EXPECT_EQ(collect(*reopenedSnapshot), std::vector<uint64_t>{nodes.front()});
	EXPECT_EQ(collectComposite(*reopenedSnapshot), std::vector<uint64_t>{nodes.front()});
	reopenedSnapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendSchemaTest,
		CandidateStreamsRejectAnIndexedPropertyAfterReplacementAndPreserveOldSnapshot) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes({
			{{"Person"}, {{"score", graph::PropertyValue(int64_t{42})}}}});
	ASSERT_EQ(nodes.size(), 1U);
	static_cast<void>(backend.createLogicalNodeIndex({"person_score", "Person", {"score"}}));
	std::unique_ptr<LogicalGraphReadSnapshot> oldSnapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(oldSnapshot, nullptr);

	// Replacing the complete Property collection removes the indexed field. The
	// old snapshot must retain the prior generation, while the current generation
	// must not return a stale index candidate or silently treat the field as live.
	backend.replaceLogicalNodeProperties({{nodes.front(), {}}});

	const auto oldExact = oldSnapshot->findNodeCandidatesByExactProperties(
			{"Person"}, {{"score", graph::PropertyValue(int64_t{42})}}, 2U, 2U);
	ASSERT_TRUE(oldExact.has_value());
	ASSERT_EQ(oldExact->candidates.size(), 1U);
	EXPECT_EQ(oldExact->candidates.front().entity_id, nodes.front());

	std::unique_ptr<LogicalGraphReadSnapshot> currentSnapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(currentSnapshot, nullptr);
	const auto currentExact = currentSnapshot->findNodeCandidatesByExactProperties(
			{"Person"}, {{"score", graph::PropertyValue(int64_t{42})}}, 2U, 2U);
	ASSERT_TRUE(currentExact.has_value());
	EXPECT_TRUE(currentExact->candidates.empty());
	const auto currentRange = currentSnapshot->findNodeCandidatesByPropertyRange(
			{"Person"}, {"score", graph::PropertyValue(int64_t{0}), graph::PropertyValue(int64_t{100}), true, true}, 2U,
			2U);
	ASSERT_TRUE(currentRange.has_value());
	EXPECT_TRUE(currentRange->candidates.empty());

	currentSnapshot.reset();
	oldSnapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();

	RelocatableStorageBackend reopened(options());
	reopened.open();
	std::unique_ptr<LogicalGraphReadSnapshot> reopenedSnapshot = reopened.openLogicalGraphReadSnapshot();
	ASSERT_NE(reopenedSnapshot, nullptr);
	const auto reopenedExact = reopenedSnapshot->findNodeCandidatesByExactProperties(
			{"Person"}, {{"score", graph::PropertyValue(int64_t{42})}}, 2U, 2U);
	ASSERT_TRUE(reopenedExact.has_value());
	EXPECT_TRUE(reopenedExact->candidates.empty());
	reopenedSnapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, UniqueCandidateStreamEnforcesProducerByteBound) {
	RelocatableStorageBackend backend(options());
	backend.open();
	static_cast<void>(backend.createLogicalConstraint({"person_code_unique",
			LogicalGraphConstraintEntityKind::LGCEK_NODE, LogicalGraphConstraintKind::LGCK_UNIQUE,
			"Person", {"code"}, LogicalGraphConstraintPropertyType::LGCPT_NONE}));
	const std::string largeValue(256U, 'x');
	const std::vector<uint64_t> nodes = backend.createLogicalNodes({
			{{"Person"}, {{"code", graph::PropertyValue(largeValue)}}}});
	ASSERT_EQ(nodes.size(), 1U);
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(snapshot, nullptr);
	ASSERT_TRUE(snapshot->supportsNativeExactNodeCandidates());
	const auto acceptAll = [](const graph::storage::IndexCandidateBatch &) { return true; };
	EXPECT_THROW(static_cast<void>(snapshot->visitNodeCandidatesByExactProperties(
			{"Person"}, {{"code", graph::PropertyValue(largeValue)}}, 1U, 4U, 1U, acceptAll)), FormatException);
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, ReusesCatalogDescriptorAcrossEntityPagesDuringInsertionPlanning) {
	RelocatableStorageBackend backend(options());
	backend.open();
	std::vector<LogicalGraphNodeCreateRequest> initial;
	initial.reserve(600U);
	for (size_t index = 0U; index < 600U; ++index) {
		initial.push_back({{"CatalogReuse"}, {}});
	}
	const std::vector<uint64_t> initialNodes = backend.createLogicalNodes(initial);
	ASSERT_EQ(initialNodes.size(), initial.size());

	const bool valuesWereEnabled = graph::debug::PerfTrace::areValuesEnabled();
	graph::debug::PerfTrace::reset();
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	ASSERT_EQ(backend.createLogicalNodes({{{"CatalogReuse"}, {}}}).size(), 1U);
	const graph::debug::PerfTrace::Snapshot trace = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setValueCollectionEnabled(valuesWereEnabled);

	// The planner keeps a snapshot-pinned insertion target cache after the
	// initial batch. A subsequent insert should reuse it without rescanning the
	// complete Entity Directory or reloading the pool descriptor.
	EXPECT_TRUE(trace.contains("native_graph.commit.entity_insert_target_cache_hits"));
	EXPECT_EQ(trace.at("native_graph.commit.entity_insert_target_cache_hits").totalValue, 1);
	EXPECT_FALSE(trace.contains("native_graph.commit.entity_insert_node_directory_pages"));
	EXPECT_FALSE(trace.contains("native_graph.commit.entity_insert_catalog_lookups"));

	// A delete-only publication must preserve the untouched insertion targets so
	// the following insert still avoids a full Entity Directory scan.
	backend.deleteLogicalNodes({{initialNodes.front()}});
	graph::debug::PerfTrace::reset();
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	ASSERT_EQ(backend.createLogicalNodes({{{"CatalogReuse"}, {}}}).size(), 1U);
	const graph::debug::PerfTrace::Snapshot afterDelete = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setValueCollectionEnabled(valuesWereEnabled);
	ASSERT_TRUE(afterDelete.contains("native_graph.commit.entity_insert_target_cache_hits"));
	EXPECT_EQ(afterDelete.at("native_graph.commit.entity_insert_target_cache_hits").totalValue, 1);
	EXPECT_FALSE(afterDelete.contains("native_graph.commit.entity_insert_node_directory_pages"));
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, GroupsBulkNodeInsertionsByBoundedEntityPageTargets) {
	RelocatableStorageBackend backend(options());
	backend.open();
	std::vector<LogicalGraphNodeCreateRequest> requests;
	requests.reserve(600U);
	for (size_t index = 0U; index < 600U; ++index) {
		requests.push_back({{"GroupedInsertions"}, {}});
	}
	const bool valuesWereEnabled = graph::debug::PerfTrace::areValuesEnabled();
	graph::debug::PerfTrace::reset();
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	ASSERT_EQ(backend.createLogicalNodes(requests).size(), requests.size());
	const graph::debug::PerfTrace::Snapshot trace = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setValueCollectionEnabled(valuesWereEnabled);

	ASSERT_TRUE(trace.contains("native_graph.commit.entity_insert_target_pages"));
	ASSERT_TRUE(trace.contains("native_graph.commit.entity_insert_target_records"));
	EXPECT_EQ(trace.at("native_graph.commit.entity_insert_target_records").totalValue,
			static_cast<int64_t>(requests.size()));
	EXPECT_GT(trace.at("native_graph.commit.entity_insert_target_pages").totalValue, 0);
	EXPECT_LT(trace.at("native_graph.commit.entity_insert_target_pages").totalValue,
			trace.at("native_graph.commit.entity_insert_target_records").totalValue);
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, ResolvesBulkNodeMutationsThroughOneSortedDirectoryBatch) {
	RelocatableStorageBackend backend(options());
	backend.open();
	std::vector<LogicalGraphNodeCreateRequest> creates;
	creates.reserve(600U);
	for (size_t index = 0U; index < 600U; ++index) {
		creates.push_back({{"BulkMutation"}, {{"value", graph::PropertyValue(static_cast<int64_t>(index))}}});
	}
	const std::vector<uint64_t> nodes = backend.createLogicalNodes(creates);
	ASSERT_EQ(nodes.size(), creates.size());

	std::vector<LogicalGraphPropertyReplaceRequest> replacements;
	replacements.reserve(nodes.size());
	for (const uint64_t nodeId: nodes) {
		replacements.push_back({nodeId, {{"value", graph::PropertyValue(int64_t{-1})}}});
	}
	const bool valuesWereEnabled = graph::debug::PerfTrace::areValuesEnabled();
	graph::debug::PerfTrace::reset();
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	backend.replaceLogicalNodeProperties(replacements);
	const graph::debug::PerfTrace::Snapshot trace = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setValueCollectionEnabled(valuesWereEnabled);

	ASSERT_TRUE(trace.contains("native_graph.read.entity_directory_batch_requests"));
	EXPECT_EQ(trace.at("native_graph.read.entity_directory_batch_requests").totalValue, 1);
	ASSERT_TRUE(trace.contains("native_graph.read.entity_directory_batch_ids"));
	EXPECT_EQ(trace.at("native_graph.read.entity_directory_batch_ids").totalValue,
			static_cast<int64_t>(nodes.size()));
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, ResolvesBulkEdgeMutationsThroughOneSortedDirectoryBatch) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes({{{"BulkMutation"}, {}}, {{"BulkMutation"}, {}}});
	ASSERT_EQ(nodes.size(), 2U);
	std::vector<LogicalGraphEdgeCreateRequest> creates;
	creates.reserve(600U);
	for (size_t index = 0U; index < 600U; ++index) {
		creates.push_back({nodes.front(), nodes.back(), "LINK", {{"value", graph::PropertyValue(static_cast<int64_t>(index))}}});
	}
	const std::vector<uint64_t> edges = backend.createLogicalEdges(creates);
	ASSERT_EQ(edges.size(), creates.size());

	std::vector<LogicalGraphPropertyReplaceRequest> replacements;
	replacements.reserve(edges.size());
	for (const uint64_t edgeId: edges) {
		replacements.push_back({edgeId, {{"value", graph::PropertyValue(int64_t{-1})}}});
	}
	const bool valuesWereEnabled = graph::debug::PerfTrace::areValuesEnabled();
	graph::debug::PerfTrace::reset();
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	backend.replaceLogicalEdgeProperties(replacements);
	const graph::debug::PerfTrace::Snapshot trace = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setValueCollectionEnabled(valuesWereEnabled);

	ASSERT_TRUE(trace.contains("native_graph.read.entity_directory_batch_requests"));
	EXPECT_EQ(trace.at("native_graph.read.entity_directory_batch_requests").totalValue, 1);
	ASSERT_TRUE(trace.contains("native_graph.read.entity_directory_batch_ids"));
	EXPECT_EQ(trace.at("native_graph.read.entity_directory_batch_ids").totalValue,
			static_cast<int64_t>(edges.size()));
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, CompositeRangeCandidatesRetainLogicalIndexFieldOrder) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes(
			{{{"Person"}, {{"city", graph::PropertyValue("Shanghai")}, {"age", graph::PropertyValue(int64_t{7})}}},
			 { {"Person"}, {{"city", graph::PropertyValue("Shanghai")}, {"age", graph::PropertyValue(int64_t{42})}}}});
	ASSERT_EQ(nodes.size(), 2U);
	static_cast<void>(backend.createLogicalNodeIndex({"person_city_age", "Person", {"city", "age"}}));
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(snapshot, nullptr);
	const LogicalGraphCompositePropertyRange request{
			{{"city", graph::PropertyValue("Shanghai")}},
			{"age", graph::PropertyValue(int64_t{7}), graph::PropertyValue(), true, true}};
	const auto candidates = snapshot->findNodeCandidatesByCompositePropertyRange({"Person"}, request, 2U, 2U);
	ASSERT_TRUE(candidates.has_value());
	EXPECT_TRUE(candidates->predicates_verified);
	ASSERT_EQ(candidates->candidates.size(), nodes.size());
	ASSERT_EQ(candidates->verified_property_columns.size(), 2U);
	EXPECT_EQ(candidates->verified_property_columns[0].key, "city");
	EXPECT_EQ(candidates->verified_property_columns[1].key, "age");
	ASSERT_EQ(candidates->verified_property_columns[0].values.size(), nodes.size());
	ASSERT_EQ(candidates->verified_property_columns[1].values.size(), nodes.size());
	for (size_t index = 0U; index < nodes.size(); ++index) {
		EXPECT_EQ(candidates->verified_property_columns[0].values[index], graph::PropertyValue("Shanghai"));
		EXPECT_EQ(candidates->verified_property_columns[1].values[index],
				  graph::PropertyValue(int64_t{index == 0U ? 7 : 42}));
	}
	snapshot.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, CompositeRangeCandidateStreamStopsAtFirstBatch) {
	RelocatableStorageBackend backend(options());
	backend.open();
	std::vector<LogicalGraphNodeCreateRequest> requests;
	requests.reserve(600U);
	for (int64_t age = 1; age <= 600; ++age) {
		requests.push_back({{"Person"}, {{"city", graph::PropertyValue("Shanghai")},
				{"age", graph::PropertyValue(age)}}});
	}
	const std::vector<uint64_t> nodes = backend.createLogicalNodes(requests);
	ASSERT_EQ(nodes.size(), requests.size());
	static_cast<void>(backend.createLogicalNodeIndex({"person_city_age", "Person", {"city", "age"}}));
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(snapshot, nullptr);
	const bool valuesWereEnabled = graph::debug::PerfTrace::areValuesEnabled();
	graph::debug::PerfTrace::reset();
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	const LogicalGraphCompositePropertyRange request{
			{{"city", graph::PropertyValue("Shanghai")} },
			{"age", graph::PropertyValue(int64_t{1}), graph::PropertyValue(int64_t{600}), true, true}};
	size_t callbackCount = 0U;
	size_t callbackEntries = 0U;
	const auto completed = snapshot->visitNodeCandidatesByCompositePropertyRange(
			{"Person"}, request, requests.size(), 2U,
			[&](const graph::storage::IndexCandidateBatch &batch) -> bool {
				++callbackCount;
				callbackEntries += batch.candidates.size();
				EXPECT_TRUE(batch.predicates_verified);
				EXPECT_EQ(batch.verified_property_columns.size(), 2U);
				if (batch.verified_property_columns.size() != 2U) return false;
				EXPECT_EQ(batch.candidates.size(), batch.verified_property_columns.front().values.size());
				EXPECT_EQ(batch.candidates.size(), batch.verified_property_columns.back().values.size());
				EXPECT_TRUE(std::ranges::is_sorted(batch.candidates, {}, &graph::storage::EntityRef::entity_id));
				return false;
			});
	const graph::debug::PerfTrace::Snapshot trace = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setValueCollectionEnabled(valuesWereEnabled);
	ASSERT_TRUE(completed.has_value());
	EXPECT_FALSE(*completed);
	EXPECT_EQ(callbackCount, 1U);
	EXPECT_EQ(callbackEntries, graph::storage::BTreeIndex::DEFAULT_CURSOR_BATCH_ENTRIES);
	ASSERT_TRUE(trace.contains("native_graph.read.index_candidate_root_descent"));
	EXPECT_EQ(trace.at("native_graph.read.index_candidate_root_descent").totalValue, 1);
	ASSERT_TRUE(trace.contains("native_graph.read.property_selected_column_batch_owners"));
	EXPECT_EQ(trace.at("native_graph.read.property_selected_column_batch_owners").totalValue,
			static_cast<int64_t>(graph::storage::BTreeIndex::DEFAULT_CURSOR_BATCH_ENTRIES));
	ASSERT_TRUE(trace.contains("native_graph.read.index_candidate_liveness_probes"));
	ASSERT_TRUE(trace.contains("native_graph.read.index_candidate_batches"));
	EXPECT_EQ(trace.at("native_graph.read.index_candidate_liveness_probes").totalValue,
			  static_cast<int64_t>(graph::storage::BTreeIndex::DEFAULT_CURSOR_BATCH_ENTRIES));
	EXPECT_EQ(trace.at("native_graph.read.index_candidate_batches").totalValue, 1);
	snapshot.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, CompositeRangeAppliesUpperBoundBeforeCandidateBudget) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes(
			{{{"Person"}, {{"city", graph::PropertyValue("Shanghai")}, {"age", graph::PropertyValue(int64_t{7})}}},
			 { {"Person"}, {{"city", graph::PropertyValue("Shanghai")}, {"age", graph::PropertyValue(int64_t{42})}}},
			 { {"Person"}, {{"city", graph::PropertyValue("Shanghai")}, {"age", graph::PropertyValue(int64_t{99})}}}});
	ASSERT_EQ(nodes.size(), 3U);
	static_cast<void>(backend.createLogicalNodeIndex({"person_city_age", "Person", {"city", "age"}}));
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(snapshot, nullptr);

	// Only the first indexed value belongs to this range.  The cursor must stop
	// at the encoded upper key before counting later leaf entries against the
	// caller's result bound.
	const LogicalGraphCompositePropertyRange request{
			{{"city", graph::PropertyValue("Shanghai")}},
			{"age", graph::PropertyValue(int64_t{7}), graph::PropertyValue(int64_t{7}), true, true}};
	const auto result = snapshot->findNodesByCompositePropertyRange({"Person"}, request, 1U, 4U);
	ASSERT_TRUE(result.has_value());
	ASSERT_EQ(result->size(), 1U);
	EXPECT_EQ(result->front().node_id, nodes.front());

	snapshot.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, ReusesTheSnapshotReadSessionForCompositeRangeIndexes) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes(
			{{{"Person"}, {{"city", graph::PropertyValue("Shanghai")}, {"age", graph::PropertyValue(int64_t{7})}}},
			 {{"Person"}, {{"city", graph::PropertyValue("Shanghai")}, {"age", graph::PropertyValue(int64_t{42})}}},
			 {{"Person"}, {{"city", graph::PropertyValue("Beijing")}, {"age", graph::PropertyValue(int64_t{42})}}}});
	ASSERT_EQ(nodes.size(), 3U);
	static_cast<void>(backend.createLogicalNodeIndex({"person_city_age", "Person", {"city", "age"}}));

	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(snapshot, nullptr);
	const LogicalGraphCompositePropertyRange request{
			{{"city", graph::PropertyValue("Shanghai")}},
			{"age", graph::PropertyValue(int64_t{7}), graph::PropertyValue(), true, true}};
	const bool valuesWereEnabled = graph::debug::PerfTrace::areValuesEnabled();
	graph::debug::PerfTrace::reset();
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	const auto first = snapshot->findNodesByCompositePropertyRange({"Person"}, request, 2U, 2U);
	const auto second = snapshot->findNodesByCompositePropertyRange({"Person"}, request, 2U, 2U);
	const graph::debug::PerfTrace::Snapshot trace = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setValueCollectionEnabled(valuesWereEnabled);

	ASSERT_TRUE(first.has_value());
	ASSERT_TRUE(second.has_value());
	ASSERT_EQ(first->size(), 2U);
	EXPECT_EQ(*first, *second);
	EXPECT_EQ(first->at(0).node_id, nodes.at(0));
	EXPECT_EQ(first->at(1).node_id, nodes.at(1));
	ASSERT_TRUE(trace.contains("native_graph.read.composite_range_index_label_token_resolution"));
	ASSERT_TRUE(trace.contains("native_graph.read.composite_range_index_property_token_resolution"));
	ASSERT_TRUE(trace.contains("native_graph.read.composite_range_index_definition_read"));
	ASSERT_TRUE(trace.contains("native_graph.read.index_candidate_root_descent"));
	ASSERT_TRUE(trace.contains("native_graph.read.property_selected_column_batch_owners"));
	EXPECT_EQ(trace.at("native_graph.read.composite_range_index_label_token_resolution").totalValue, 1);
	EXPECT_EQ(trace.at("native_graph.read.composite_range_index_property_token_resolution").totalValue, 2);
	EXPECT_EQ(trace.at("native_graph.read.composite_range_index_definition_read").totalValue, 1);
	EXPECT_EQ(trace.at("native_graph.read.index_candidate_root_descent").totalValue, 2);
	// Both composite indexed fields are read once for the first lookup and then
	// served from the same snapshot-owned cache on the repeated lookup.
	EXPECT_EQ(trace.at("native_graph.read.property_selected_column_batch_owners").totalValue,
			  static_cast<int64_t>(first->size()));

	snapshot.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, ResolvesScalarNodeRangesThroughPinnedIndexes) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes(
			{{{"Person"}, {{"score", graph::PropertyValue(int64_t{7})}}},
			 {{"Person"}, {{"score", graph::PropertyValue(int64_t{42})}}},
			 {{"Person"}, {{"score", graph::PropertyValue(int64_t{42})}}},
			 {{"Person"}, {{"score", graph::PropertyValue(int64_t{50})}}}});
	ASSERT_EQ(nodes.size(), 4U);
	static_cast<void>(backend.createLogicalNodeIndex({"person_score", "Person", {"score"}}));

	{
	const std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(snapshot, nullptr);
	const auto lookup = [&snapshot](graph::PropertyValue minValue, graph::PropertyValue maxValue, bool minInclusive = true,
										 bool maxInclusive = true, size_t maximumNodes = 4U) {
		return snapshot->findNodesByPropertyRange(
				{"Person"}, {"score", std::move(minValue), std::move(maxValue), minInclusive, maxInclusive}, maximumNodes,
				1U);
	};

	const auto lower = lookup(graph::PropertyValue(int64_t{42}), graph::PropertyValue());
	ASSERT_TRUE(lower.has_value());
	ASSERT_EQ(lower->size(), 3U);
	EXPECT_EQ(lower->at(0).node_id, nodes.at(1));
	EXPECT_EQ(lower->at(1).node_id, nodes.at(2));
	EXPECT_EQ(lower->at(2).node_id, nodes.at(3));

	const auto upper = lookup(graph::PropertyValue(), graph::PropertyValue(int64_t{42}));
	ASSERT_TRUE(upper.has_value());
	ASSERT_EQ(upper->size(), 3U);
	EXPECT_EQ(upper->at(0).node_id, nodes.at(0));
	EXPECT_EQ(upper->at(1).node_id, nodes.at(1));
	EXPECT_EQ(upper->at(2).node_id, nodes.at(2));

	const auto open = lookup(graph::PropertyValue(int64_t{7}), graph::PropertyValue(int64_t{50}), false, false);
	ASSERT_TRUE(open.has_value());
	ASSERT_EQ(open->size(), 2U);
	EXPECT_EQ(open->at(0).node_id, nodes.at(1));
	EXPECT_EQ(open->at(1).node_id, nodes.at(2));
	const auto reversed = lookup(graph::PropertyValue(int64_t{50}), graph::PropertyValue(int64_t{7}));
	ASSERT_TRUE(reversed.has_value());
	EXPECT_TRUE(reversed->empty());
	const auto emptyExclusive =
			lookup(graph::PropertyValue(int64_t{42}), graph::PropertyValue(int64_t{42}), false, true);
	ASSERT_TRUE(emptyExclusive.has_value());
	EXPECT_TRUE(emptyExclusive->empty());
	const auto unknownLabel = snapshot->findNodesByPropertyRange(
			{"MissingLabel"}, {"score", graph::PropertyValue(int64_t{7}), graph::PropertyValue(), true, true}, 4U,
			1U);
	ASSERT_TRUE(unknownLabel.has_value());
	EXPECT_TRUE(unknownLabel->empty());
	const auto unknownProperty = snapshot->findNodesByPropertyRange(
			{"Person"}, {"missing", graph::PropertyValue(int64_t{7}), graph::PropertyValue(), true, true}, 4U, 1U);
	ASSERT_TRUE(unknownProperty.has_value());
	EXPECT_TRUE(unknownProperty->empty());
	EXPECT_EQ(snapshot->findNodesByPropertyRange(
				  {"Person", "Other"}, {"score", graph::PropertyValue(int64_t{7}), graph::PropertyValue(), true, true},
				  4U, 1U),
			  std::nullopt);
	EXPECT_EQ(snapshot->findNodesByPropertyRange(
				  {"Person"}, {"", graph::PropertyValue(int64_t{7}), graph::PropertyValue(), true, true}, 4U, 1U),
			  std::nullopt);
	EXPECT_THROW(static_cast<void>(lookup(graph::PropertyValue(int64_t{42}), graph::PropertyValue(), true, true, 2U)),
				 FormatException);
	EXPECT_EQ(lookup(graph::PropertyValue(std::vector<graph::PropertyValue>{graph::PropertyValue(int64_t{7})}),
						 graph::PropertyValue()),
				  std::nullopt);
	}
	backend.close();
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
	const bool valuesWereEnabled = graph::debug::PerfTrace::areValuesEnabled();
	graph::debug::PerfTrace::reset();
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	const std::vector<LogicalGraphIndexDefinition> created = backend.createLogicalNodeIndexes(requests);
	const graph::debug::PerfTrace::Snapshot indexBuildTrace = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setValueCollectionEnabled(valuesWereEnabled);
	ASSERT_EQ(created.size(), 2U);
	ASSERT_TRUE(indexBuildTrace.contains("native_graph.read.property_selected_column_batch_owners"));
	// Both definitions share one bounded Property-column pass over the matching
	// Node owners; the old per-definition implementation performed two passes.
	EXPECT_EQ(indexBuildTrace.at("native_graph.read.property_selected_column_batch_owners").totalValue, 2);
	EXPECT_FALSE(indexBuildTrace.contains("native_graph.read.property_optional_lookup"));
	EXPECT_LT(created[0].index_id, created[1].index_id);
	EXPECT_EQ(created[0].kind, LogicalGraphIndexKind::LGIK_PROPERTY);
	EXPECT_EQ(created[1].kind, LogicalGraphIndexKind::LGIK_PROPERTY);
	RelocatableReadSnapshot snapshot = backend.openSnapshot();
	EXPECT_THROW(static_cast<void>(backend.readIndexDefinitions(snapshot, 1U)), FormatException);
	EXPECT_EQ(backend.readIndexDefinitions(snapshot, 2U).size(), 2U);
	snapshot.release();
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

TEST_F(RelocatableStorageBackendSchemaTest, RejectsScalarIndexesThatExceedThePublishedDefinitionCapacity) {
	RelocatableStorageBackendOptions constrainedOptions = options();
	constrainedOptions.maximum_index_definitions = 1U;
	RelocatableStorageBackend backend(std::move(constrainedOptions));
	backend.open();

	const LogicalGraphIndexDefinition first = backend.createLogicalNodeIndex({"person_score", "Person", {"score"}});
	EXPECT_THROW(static_cast<void>(backend.createLogicalNodeIndex({"person_rank", "Person", {"rank"}})),
			 FormatException);
	EXPECT_EQ(backend.listLogicalIndexes(1U), std::vector<LogicalGraphIndexDefinition>{first});
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, RejectsInvalidOrExistingScalarIndexBatchesBeforePublication) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::vector<LogicalGraphNodeIndexCreateRequest> empty;
	EXPECT_THROW(static_cast<void>(backend.createLogicalNodeIndexes(empty)), std::invalid_argument);
	EXPECT_TRUE(backend.listLogicalIndexes(8U).empty());
	const std::vector<LogicalGraphNodeIndexCreateRequest> tooMany(options().maximum_index_definitions + 1U);
	EXPECT_THROW(static_cast<void>(backend.createLogicalNodeIndexes(tooMany)), std::invalid_argument);
	EXPECT_TRUE(backend.listLogicalIndexes(8U).empty());

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
