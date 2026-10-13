#include "storage/relocatable/backend/RelocatableStorageBackendTestFixture.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/relocatable/format/SuperblockFile.hpp"

#include <atomic>
#include <cerrno>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace relocatable_storage_backend_test;

TEST_F(RelocatableStorageBackendTest, RepeatedWidePropertyStatementsRetainOneFinalCollectionPerEntity) {
	materializeNativeDatabase();
	auto options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const PropertyValue payload(std::string(16000U, 's'));
	const auto nodes = backend.createLogicalNodes({{{}, {{"payload", payload}, {"rank", PropertyValue(0)}}}});
	{
		auto transaction = backend.beginLogicalGraphTransaction(false);
		LogicalGraphTransactionOverlay overlay(*transaction);
		const bool enabled = graph::debug::PerfTrace::areValuesEnabled();
		graph::debug::PerfTrace::reset();
		graph::debug::PerfTrace::setValueCollectionEnabled(true);
		try {
			for (int rank = 1; rank <= 32; ++rank) {
				const auto reference = LogicalGraphMutationEntityReference{LogicalGraphMutationEntityKind::LGMK_NODE,
					LogicalGraphMutationReferenceKind::LGRK_PERSISTED, nodes.front()};
				LogicalGraphMutationBatch batch;
				batch.property_replacements = {{reference, {{"payload", payload}, {"rank", PropertyValue(rank)}}}};
				batch.node_label_replacements = {{reference, {"StageLabel" + std::to_string(rank)}}};
				static_cast<void>(overlay.executeLogicalGraphMutationBatch(batch));
				auto visible = overlay.openLogicalGraphReadSnapshot();
				const auto node = visible->readNodeShaped(nodes.front(), {.all_properties = true});
				const auto field = std::ranges::find(node.properties, "rank", &LogicalGraphPropertyEntry::key);
				if (field == node.properties.end() || field->value != PropertyValue(rank))
					throw std::runtime_error("statement overlay lost its current property value");
				const auto labels = visible->readNodeShaped(nodes.front(), {.include_labels = true}).labels;
				if (labels != std::vector<std::string>{"StageLabel" + std::to_string(rank)})
					throw std::runtime_error("statement overlay lost its current labels");
			}
		} catch (...) {
			graph::debug::PerfTrace::setValueCollectionEnabled(enabled);
			throw;
		}
		const auto trace = graph::debug::PerfTrace::snapshotAndReset();
		graph::debug::PerfTrace::setValueCollectionEnabled(enabled);
		ASSERT_TRUE(trace.contains("native_graph.write.transaction_retained_property_targets"));
		EXPECT_EQ(trace.at("native_graph.write.transaction_retained_property_targets").valueCalls, 32U);
		EXPECT_EQ(trace.at("native_graph.write.transaction_retained_property_targets").maximumValue, 1);
		ASSERT_TRUE(trace.contains("native_graph.write.transaction_retained_label_targets"));
		EXPECT_EQ(trace.at("native_graph.write.transaction_retained_label_targets").valueCalls, 32U);
		EXPECT_EQ(trace.at("native_graph.write.transaction_retained_label_targets").maximumValue, 1);
		static_cast<void>(transaction->commit());
	}
	auto after = backend.openLogicalGraphReadSnapshot();
	const auto node = after->readNodeShaped(nodes.front(), {.all_properties = true});
	const auto rank = std::ranges::find(node.properties, "rank", &LogicalGraphPropertyEntry::key);
	const auto retained = std::ranges::find(node.properties, "payload", &LogicalGraphPropertyEntry::key);
	ASSERT_NE(rank, node.properties.end());
	ASSERT_NE(retained, node.properties.end());
	EXPECT_EQ(rank->value, PropertyValue(32));
	EXPECT_EQ(retained->value, payload);
	EXPECT_EQ(after->readNodeShaped(nodes.front(), {.include_labels = true}).labels,
		(std::vector<std::string>{"StageLabel32"}));
	after.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, ExplicitCypherTransactionKeepsIntermediateVisibilityAndUsesNativeFinalPatches) {
	materializeNativeDatabase();
	zyx::Database database(data_path_.string());
	database.open();
	ASSERT_GT(database.createNode("ExplicitCypherPatch", {{"rank", int64_t(1)}, {"keep", std::string("retained")}, {"gone", int64_t(9)}}), 0);
	{
		auto transaction = database.beginTransaction();
		auto first = transaction.execute("MATCH (n:ExplicitCypherPatch) SET n.rank = 2 REMOVE n.gone");
		ASSERT_TRUE(first.isSuccess()) << first.getError();
		auto second = transaction.execute("MATCH (n:ExplicitCypherPatch) SET n.rank = n.rank + 1 RETURN n.rank AS rank, n.gone AS gone");
		ASSERT_TRUE(second.isSuccess()) << second.getError();
		ASSERT_TRUE(second.hasNext());
		second.next();
		EXPECT_EQ(std::get<int64_t>(second.get("rank")), 3);
		EXPECT_TRUE(std::holds_alternative<std::monostate>(second.get("gone")));
		const bool enabled = graph::debug::PerfTrace::areValuesEnabled();
		graph::debug::PerfTrace::reset();
		graph::debug::PerfTrace::setValueCollectionEnabled(true);
		try { transaction.commit(); }
		catch (...) { graph::debug::PerfTrace::setValueCollectionEnabled(enabled); throw; }
		const auto trace = graph::debug::PerfTrace::snapshotAndReset();
		graph::debug::PerfTrace::setValueCollectionEnabled(enabled);
		ASSERT_TRUE(trace.contains("native_graph.write.property_patch_targets"));
		EXPECT_EQ(trace.at("native_graph.write.property_patch_targets").totalValue, 1);
	}
	database.close();
	zyx::Database reopened(data_path_.string());
	reopened.open();
	auto result = reopened.execute("MATCH (n:ExplicitCypherPatch) RETURN n.rank AS rank, n.keep AS keep, n.gone AS gone");
	ASSERT_TRUE(result.isSuccess()) << result.getError();
	ASSERT_TRUE(result.hasNext());
	result.next();
	EXPECT_EQ(std::get<int64_t>(result.get("rank")), 3);
	EXPECT_EQ(std::get<std::string>(result.get("keep")), "retained");
	EXPECT_TRUE(std::holds_alternative<std::monostate>(result.get("gone")));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest, LaterDetachDeleteDiscardsEarlierPersistedEdgePropertyChanges) {
	materializeNativeDatabase();
	auto options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const auto nodes = backend.createLogicalNodes({{{}, {}}, {{}, {}}});
	const auto edges = backend.createLogicalEdges({{nodes.front(), nodes.back(), "DETACH_PATCH", {{"weight", PropertyValue(1)}}}});
	auto before = backend.openLogicalGraphReadSnapshot();
	{
		auto transaction = backend.beginLogicalGraphTransaction(false);
		LogicalGraphTransactionOverlay overlay(*transaction);
		overlay.replaceLogicalEdgeProperties({{edges.front(), {{"weight", PropertyValue(2)}}}});
		overlay.deleteLogicalNodes({{nodes.front(), true, 8U}});
		EXPECT_NO_THROW(static_cast<void>(transaction->commit()));
		EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_COMMITTED);
	}
	auto after = backend.openLogicalGraphReadSnapshot();
	EXPECT_THROW(static_cast<void>(after->readEdge(edges.front())), FormatException);
	EXPECT_THROW(static_cast<void>(after->readNode(nodes.front())), FormatException);
	EXPECT_NO_THROW(static_cast<void>(after->readNode(nodes.back())));
	EXPECT_EQ(before->readEdgeShaped(edges.front(), {.all_properties = true}).properties.front().value, PropertyValue(1));
	after.reset();
	before.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, ExplicitPropertyTransactionFinalSyncFailureIsIndeterminateUntilReopen) {
	materializeNativeDatabase();
	auto options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const auto nodes = backend.createLogicalNodes({{{}, {{"rank", PropertyValue(1)}, {"stamp", PropertyValue(1)}}}});
	auto physical = backend.openSnapshot();
	const auto oldRoot = physical.superblock().entity_property_directory_root;
	physical.release();
	std::atomic<bool> publicationWritten{false}, failedFinalSync{false};
	{
		auto transaction = backend.beginLogicalGraphTransaction(false);
		LogicalGraphTransactionOverlay overlay(*transaction);
		overlay.replaceLogicalNodeProperties({{nodes.front(), {{"rank", PropertyValue(2)}, {"stamp", PropertyValue(2)}}}});
		StorageIO::ScopedNativeWriteHookForTesting detectPublication(
			[&](file_handle_t fd, const void *bytes, size_t count, pwrite_off_t offset) {
				if (count == RSS_PAGE_SIZE && (offset == SuperblockFile::slotOffset(0U) || offset == SuperblockFile::slotOffset(1U))) {
					try {
						const auto candidate = SuperblockCodec::decode(std::span(static_cast<const std::byte *>(bytes), count));
						if (candidate.entity_property_directory_root != oldRoot) publicationWritten.store(true);
					} catch (const std::exception &) {
						// Other control frames are outside this graph publication.
					}
				}
				return portable_pwrite(fd, bytes, count, offset);
			});
		StorageIO::ScopedNativeSyncHookForTesting rejectFinalSync([&](file_handle_t fd) {
			if (publicationWritten.load() && !failedFinalSync.exchange(true)) {
#ifdef _WIN32
				SetLastError(ERROR_WRITE_FAULT);
#else
				errno = EIO;
#endif
				return -1;
			}
			return portable_fsync(fd);
		});
		EXPECT_THROW(static_cast<void>(transaction->commit()), std::runtime_error);
		EXPECT_TRUE(failedFinalSync.load());
		EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_INDETERMINATE);
	}
	EXPECT_THROW(static_cast<void>(backend.createLogicalNodes({{{}, {}}})), std::logic_error);
	backend.close();
	auto reopenedOptions = backendOptions();
	reopenedOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend reopened(std::move(reopenedOptions));
	reopened.open();
	auto snapshot = reopened.openLogicalGraphReadSnapshot();
	const auto node = snapshot->readNodeShaped(nodes.front(), {.all_properties = true});
	ASSERT_EQ(node.properties.size(), 2U);
	const auto rank = std::ranges::find(node.properties, "rank", &LogicalGraphPropertyEntry::key);
	const auto stamp = std::ranges::find(node.properties, "stamp", &LogicalGraphPropertyEntry::key);
	ASSERT_NE(rank, node.properties.end());
	ASSERT_NE(stamp, node.properties.end());
	EXPECT_EQ(rank->value, stamp->value);
	EXPECT_TRUE(rank->value == PropertyValue(1) || rank->value == PropertyValue(2));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest, ExplicitCompleteReplacementRetainsItsStoredNullField) {
	materializeNativeDatabase();
	auto options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const auto nodes = backend.createLogicalNodes({{{}, {{"rank", PropertyValue(1)}}}});
	{
		auto transaction = backend.beginLogicalGraphTransaction(false);
		LogicalGraphTransactionOverlay overlay(*transaction);
		overlay.replaceLogicalNodeProperties({{nodes.front(), {{"rank", PropertyValue{}}, {"added", PropertyValue(2)}}}});
		static_cast<void>(transaction->commit());
	}
	auto snapshot = backend.openLogicalGraphReadSnapshot();
	const auto node = snapshot->readNodeShaped(nodes.front(), {.all_properties = true});
	ASSERT_EQ(node.properties.size(), 2U);
	const auto rank = std::ranges::find(node.properties, "rank", &LogicalGraphPropertyEntry::key);
	ASSERT_NE(rank, node.properties.end());
	EXPECT_EQ(rank->value.getType(), PropertyType::NULL_TYPE);
	snapshot.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, ExplicitTransactionFoldsFinalNodeAndEdgePatchesIntoNativePublication) {
	materializeNativeDatabase();
	auto options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const PropertyValue payload(std::string(16000U, 't'));
	const auto nodes = backend.createLogicalNodes({{{"TxPatch"}, {{"rank", PropertyValue(1)}, {"payload", payload}, {"gone", PropertyValue(9)}}}, {{}, {}}});
	const auto edges = backend.createLogicalEdges({{nodes.front(), nodes.back(), "TX_PATCH", {{"weight", PropertyValue(1)}}}});
	auto before = backend.openLogicalGraphReadSnapshot();
	{
		auto transaction = backend.beginLogicalGraphTransaction(false);
		LogicalGraphTransactionOverlay overlay(*transaction);
		const auto node = LogicalGraphMutationEntityReference{LogicalGraphMutationEntityKind::LGMK_NODE,
			LogicalGraphMutationReferenceKind::LGRK_PERSISTED, nodes.front()};
		const auto edge = LogicalGraphMutationEntityReference{LogicalGraphMutationEntityKind::LGMK_EDGE,
			LogicalGraphMutationReferenceKind::LGRK_PERSISTED, edges.front()};
		{
			auto snapshot = overlay.openLogicalGraphReadSnapshot();
			const std::vector<LogicalGraphPropertyPatchRequest> patches{
				{node, {{"rank", PropertyValue(2)}}, {"gone"}}, {edge, {{"weight", PropertyValue(2)}}, {}}};
			static_cast<void>(overlay.executeLogicalGraphPropertyPatches(*snapshot, patches, 16U, *snapshot->mutationFence()));
		}
		{
			auto snapshot = overlay.openLogicalGraphReadSnapshot();
			const std::vector<LogicalGraphPropertyPatchRequest> patches{
				{node, {{"rank", PropertyValue(3)}}, {}}, {edge, {{"weight", PropertyValue(4)}}, {}}};
			static_cast<void>(overlay.executeLogicalGraphPropertyPatches(*snapshot, patches, 16U, *snapshot->mutationFence()));
		}
		auto visible = overlay.openLogicalGraphReadSnapshot();
		const auto staged = visible->readNodeShaped(nodes.front(), {.all_properties = true});
		EXPECT_EQ(std::ranges::find(staged.properties, "rank", &LogicalGraphPropertyEntry::key)->value, PropertyValue(3));
		EXPECT_EQ(staged.properties.size(), 2U);
		visible.reset();
		const bool enabled = graph::debug::PerfTrace::areValuesEnabled();
		graph::debug::PerfTrace::reset();
		graph::debug::PerfTrace::setValueCollectionEnabled(true);
		try { static_cast<void>(transaction->commit()); }
		catch (...) { graph::debug::PerfTrace::setValueCollectionEnabled(enabled); throw; }
		const auto trace = graph::debug::PerfTrace::snapshotAndReset();
		graph::debug::PerfTrace::setValueCollectionEnabled(enabled);
		ASSERT_TRUE(trace.contains("native_graph.write.property_patch_targets"));
		EXPECT_EQ(trace.at("native_graph.write.property_patch_targets").totalValue, 2);
		EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_COMMITTED);
	}
	auto after = backend.openLogicalGraphReadSnapshot();
	const auto finalNode = after->readNodeShaped(nodes.front(), {.all_properties = true});
	ASSERT_EQ(finalNode.properties.size(), 2U);
	EXPECT_EQ(std::ranges::find(finalNode.properties, "rank", &LogicalGraphPropertyEntry::key)->value, PropertyValue(3));
	EXPECT_EQ(std::ranges::find(finalNode.properties, "payload", &LogicalGraphPropertyEntry::key)->value, payload);
	EXPECT_EQ(after->readEdgeShaped(edges.front(), {.all_properties = true}).properties.front().value, PropertyValue(4));
	EXPECT_EQ(before->readNodeShaped(nodes.front(), {.all_properties = true}).properties.size(), 3U);
	EXPECT_EQ(before->readEdgeShaped(edges.front(), {.all_properties = true}).properties.front().value, PropertyValue(1));
	after.reset();
	before.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, ExplicitTransactionCompleteMapReplacementStillRemovesUnspecifiedFields) {
	materializeNativeDatabase();
	auto options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const auto nodes = backend.createLogicalNodes({{{}, {{"remove", PropertyValue(1)}, {"keep", PropertyValue(2)}}}});
	{
		auto transaction = backend.beginLogicalGraphTransaction(false);
		LogicalGraphTransactionOverlay overlay(*transaction);
		overlay.replaceLogicalNodeProperties({{nodes.front(), {{"keep", PropertyValue(3)}}}});
		static_cast<void>(transaction->commit());
	}
	auto after = backend.openLogicalGraphReadSnapshot();
	const auto finalNode = after->readNodeShaped(nodes.front(), {.all_properties = true});
	ASSERT_EQ(finalNode.properties.size(), 1U);
	EXPECT_EQ(finalNode.properties.front().key, "keep");
	EXPECT_EQ(finalNode.properties.front().value, PropertyValue(3));
	after.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, ExplicitTransactionUnchangedReplacementStillDetectsInterveningWriter) {
	materializeNativeDatabase();
	auto options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const auto nodes = backend.createLogicalNodes({{{}, {{"rank", PropertyValue(1)}}}});
	{
		auto transaction = backend.beginLogicalGraphTransaction(false);
		LogicalGraphTransactionOverlay overlay(*transaction);
		overlay.replaceLogicalNodeProperties({{nodes.front(), {{"rank", PropertyValue(1)}}}});
		static_cast<void>(backend.createLogicalNodes({{{"Intervening"}, {}}}));
		EXPECT_THROW(static_cast<void>(transaction->commit()), LogicalGraphMutationConflict);
		EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_CONFLICTED);
	}
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, CypherSetAndRemoveReachNativePropertyPatchConsumer) {
	materializeNativeDatabase();
	zyx::Database database(data_path_.string());
	database.open();
	ASSERT_GT(database.createNode("CypherNativePatch", {{"rank", int64_t(1)}, {"keep", std::string("retained")}, {"remove", int64_t(3)}}), 0);
	const bool enabled = graph::debug::PerfTrace::areValuesEnabled();
	graph::debug::PerfTrace::reset();
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	zyx::Result changed;
	try {
		changed = database.execute("MATCH (n:CypherNativePatch) SET n.rank = 2 REMOVE n.remove RETURN n.rank AS rank, n.keep AS keep");
	} catch (...) {
		graph::debug::PerfTrace::setValueCollectionEnabled(enabled);
		throw;
	}
	const auto trace = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setValueCollectionEnabled(enabled);
	ASSERT_TRUE(changed.isSuccess()) << changed.getError();
	ASSERT_TRUE(trace.contains("native_graph.write.property_patch_targets"));
	EXPECT_EQ(trace.at("native_graph.write.property_patch_targets").totalValue, 1);
	ASSERT_TRUE(changed.hasNext());
	changed.next();
	EXPECT_EQ(std::get<int64_t>(changed.get("rank")), 2);
	EXPECT_EQ(std::get<std::string>(changed.get("keep")), "retained");
	auto verify = database.execute("MATCH (n:CypherNativePatch) RETURN n.remove AS removed");
	ASSERT_TRUE(verify.isSuccess()) << verify.getError();
	ASSERT_TRUE(verify.hasNext());
	verify.next();
	EXPECT_TRUE(std::holds_alternative<std::monostate>(verify.get("removed")));
	database.close();
}

TEST_F(RelocatableStorageBackendTest, NativePropertyPatchDispatchRejectsStaleFenceAndKeepsUnspecifiedFields) {
	materializeNativeDatabase();
	auto options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const auto ids = backend.createLogicalNodes({{{"NativePatch"}, {{"rank", PropertyValue(1)}, {"keep", PropertyValue("original")}}}});
	auto before = backend.openLogicalGraphReadSnapshot();
	const auto fence = *before->mutationFence();
	const std::vector<LogicalGraphPropertyPatchRequest> patches{{
		{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, ids.front()},
		{{"rank", PropertyValue(2)}}, {"missing"}}};
	const bool enabled = graph::debug::PerfTrace::areValuesEnabled();
	graph::debug::PerfTrace::reset();
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	try {
		static_cast<void>(backend.executeLogicalGraphPropertyPatches(*before, patches, 4U, fence));
	} catch (...) {
		graph::debug::PerfTrace::setValueCollectionEnabled(enabled);
		throw;
	}
	const auto trace = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setValueCollectionEnabled(enabled);
	ASSERT_TRUE(trace.contains("native_graph.write.property_patch_targets"));
	EXPECT_EQ(trace.at("native_graph.write.property_patch_targets").totalValue, 1);
	EXPECT_THROW(static_cast<void>(backend.executeLogicalGraphPropertyPatches(*before, patches, 4U, fence)),
		LogicalGraphMutationConflict);
	auto current = backend.openLogicalGraphReadSnapshot();
	const auto node = current->readNodeShaped(ids.front(), {.all_properties = true});
	ASSERT_EQ(node.properties.size(), 2U);
	EXPECT_EQ(std::ranges::find(node.properties, "keep", &LogicalGraphPropertyEntry::key)->value, PropertyValue("original"));
	EXPECT_EQ(std::ranges::find(node.properties, "rank", &LogicalGraphPropertyEntry::key)->value, PropertyValue(2));
	current.reset();
	before.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, CreatedPropertyPatchesFoldIntoNativeCreationAndSurviveReopen) {
	materializeNativeDatabase();
	auto options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const LogicalGraphMutationEntityReference node{
		LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 0U};
	const LogicalGraphMutationEntityReference edge{
		LogicalGraphMutationEntityKind::LGMK_EDGE, LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE, 0U};
	LogicalGraphMutationBatch companion;
	companion.node_creates = {{{"PatchCreated"}, {{"keep", PropertyValue("original")}, {"remove", PropertyValue(1)}}}};
	companion.edge_creates = {{node, node, "PATCH_LINK", {{"keep", PropertyValue(2)}, {"remove", PropertyValue(3)}}}};
	const auto original = companion;
	const std::vector<LogicalGraphPropertyPatchRequest> patches{
		{node, {{"added", PropertyValue(4)}}, {"remove"}},
		{edge, {{"keep", PropertyValue(5)}}, {"remove"}}};
	auto snapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_TRUE(snapshot->mutationFence().has_value());
	const auto result = backend.executeLogicalGraphMutationBatchWithPropertyPatches(
		*snapshot, companion, patches, 8U, *snapshot->mutationFence());
	EXPECT_EQ(companion, original);
	ASSERT_EQ(result.created_node_ids.size(), 1U);
	ASSERT_EQ(result.created_edge_ids.size(), 1U);
	snapshot.reset();
	backend.close();
	auto reopenedOptions = backendOptions();
	reopenedOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend reopened(std::move(reopenedOptions));
	reopened.open();
	auto current = reopened.openLogicalGraphReadSnapshot();
	const auto createdNode = current->readNodeShaped(result.created_node_ids.front(), {.all_properties = true});
	ASSERT_EQ(createdNode.properties.size(), 2U);
	const auto added = std::ranges::find(createdNode.properties, "added", &LogicalGraphPropertyEntry::key);
	ASSERT_NE(added, createdNode.properties.end());
	EXPECT_EQ(added->value, PropertyValue(4));
	const auto keep = std::ranges::find(createdNode.properties, "keep", &LogicalGraphPropertyEntry::key);
	ASSERT_NE(keep, createdNode.properties.end());
	EXPECT_EQ(keep->value, PropertyValue("original"));
	const auto createdEdge = current->readEdgeShaped(result.created_edge_ids.front(), {.all_properties = true});
	ASSERT_EQ(createdEdge.properties.size(), 1U);
	EXPECT_EQ(createdEdge.properties.front().key, "keep");
	EXPECT_EQ(createdEdge.properties.front().value, PropertyValue(5));
	EXPECT_EQ(createdEdge.source_node_id, result.created_node_ids.front());
	EXPECT_EQ(createdEdge.target_node_id, result.created_node_ids.front());
	current.reset();
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest, PersistedPropertyPatchAdmissionFailureAllowsRetryAndPreservesOldLargeValueSnapshot) {
	materializeNativeDatabase();
	auto options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const PropertyValue payload(std::string(16000U, 'p'));
	const auto ids = backend.createLogicalNodes({{{"PatchPersisted"}, {{"payload", payload}, {"rank", PropertyValue(1)}}}});
	ASSERT_EQ(ids.size(), 1U);
	auto before = backend.openLogicalGraphReadSnapshot();
	const auto fence = *before->mutationFence();
	const std::vector<LogicalGraphPropertyPatchRequest> patches{{
		{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, ids.front()},
		{{"rank", PropertyValue(2)}}, {}}};
	size_t admissions = 0U;
	LogicalGraphMutationReadControl control;
	control.property_admission = [&](size_t bytes) {
		++admissions;
		if (bytes > 1U) throw std::length_error("Patch source exceeds test budget");
	};
	EXPECT_THROW(static_cast<void>(backend.executeLogicalGraphPropertyPatches(*before, patches, 3U, fence, control)), std::length_error);
	EXPECT_GT(admissions, 0U);
	EXPECT_EQ(backend.openLogicalGraphReadSnapshot()->mutationFence(), before->mutationFence());
	EXPECT_NO_THROW(static_cast<void>(backend.executeLogicalGraphPropertyPatches(*before, patches, 3U, fence)));
	const auto verify = [&](LogicalGraphReadSnapshot &snapshot, int rank) {
		const auto node = snapshot.readNodeShaped(ids.front(), {.all_properties = true});
		ASSERT_EQ(node.properties.size(), 2U);
		const auto field = std::ranges::find(node.properties, "rank", &LogicalGraphPropertyEntry::key);
		ASSERT_NE(field, node.properties.end());
		EXPECT_EQ(field->value, PropertyValue(rank));
		const auto value = std::ranges::find(node.properties, "payload", &LogicalGraphPropertyEntry::key);
		ASSERT_NE(value, node.properties.end());
		EXPECT_EQ(value->value, payload);
	};
	verify(*before, 1);
	auto after = backend.openLogicalGraphReadSnapshot();
	verify(*after, 2);
	after.reset();
	before.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, PropertyPatchUniqueFailureRollsBackAllTargetsAndCompanionLabels) {
	materializeNativeDatabase();
	auto options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const auto ids = backend.createLogicalNodes({
		{{"PatchUnique"}, {{"key", PropertyValue(1)}}},
		{{"PatchUnique"}, {{"key", PropertyValue(2)}}}});
	ASSERT_EQ(ids.size(), 2U);
	static_cast<void>(backend.createLogicalConstraint({"patch_unique", LogicalGraphConstraintEntityKind::LGCEK_NODE,
		LogicalGraphConstraintKind::LGCK_UNIQUE, "PatchUnique", {"key"}}));
	const auto reference = [&](size_t index) {
		return LogicalGraphMutationEntityReference{LogicalGraphMutationEntityKind::LGMK_NODE,
			LogicalGraphMutationReferenceKind::LGRK_PERSISTED, ids[index]};
	};
	LogicalGraphMutationBatch companion;
	companion.node_label_replacements = {{reference(0U), {"PatchUnique", "Touched"}}};
	std::vector<LogicalGraphPropertyPatchRequest> patches{
		{reference(0U), {{"key", PropertyValue(3)}}, {}},
		{reference(1U), {{"key", PropertyValue(3)}}, {}}};
	auto before = backend.openLogicalGraphReadSnapshot();
	const auto fence = *before->mutationFence();
	try {
		static_cast<void>(backend.executeLogicalGraphMutationBatchWithPropertyPatches(*before, companion, patches, 4U, fence));
		FAIL() << "Expected duplicate final constraint key to reject the batch";
	} catch (const std::runtime_error &error) {
		EXPECT_NE(std::string(error.what()).find("patch_unique"), std::string::npos);
	}
	auto unchanged = backend.openLogicalGraphReadSnapshot();
	EXPECT_EQ(unchanged->mutationFence(), before->mutationFence());
	for (size_t index = 0U; index < ids.size(); ++index) {
		const auto node = unchanged->readNodeShaped(ids[index], {.include_labels = true, .all_properties = true});
		ASSERT_EQ(node.properties.size(), 1U);
		EXPECT_EQ(node.properties.front().value, PropertyValue(static_cast<int>(index + 1U)));
		EXPECT_EQ(node.labels, (std::vector<std::string>{"PatchUnique"}));
	}
	patches[1].sets.front().value = PropertyValue(4);
	EXPECT_NO_THROW(static_cast<void>(backend.executeLogicalGraphMutationBatchWithPropertyPatches(
		*before, companion, patches, 4U, fence)));
	auto changed = backend.openLogicalGraphReadSnapshot();
	for (size_t index = 0U; index < ids.size(); ++index) {
		const auto node = changed->readNodeShaped(ids[index], {.include_labels = true, .all_properties = true});
		ASSERT_EQ(node.properties.size(), 1U);
		EXPECT_EQ(node.properties.front().value, PropertyValue(static_cast<int>(index + 3U)));
		EXPECT_EQ(std::ranges::find(node.labels, "Touched") != node.labels.end(), index == 0U);
	}
	changed.reset();
	unchanged.reset();
	before.reset();
	backend.close();
}
