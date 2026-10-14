#include "storage/relocatable/backend/RelocatableStorageBackendTestFixture.hpp"
#include "graph/debug/PerfTrace.hpp"

#include <limits>

using namespace relocatable_storage_backend_test;

namespace {
RelocatableStorageBackendOptions writableStagingOptions(RelocatableStorageBackendOptions options) {
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	return options;
}

LogicalGraphMutationEntityReference stagingNode(uint64_t id) {
	return {LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, id};
}
LogicalGraphMutationEntityReference stagingEdge(uint64_t id) {
	return {LogicalGraphMutationEntityKind::LGMK_EDGE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, id};
}

class StagingTrace {
public:
	StagingTrace() : enabled_(graph::debug::PerfTrace::areValuesEnabled()) {
		graph::debug::PerfTrace::reset();
		graph::debug::PerfTrace::setValueCollectionEnabled(true);
	}
	~StagingTrace() { graph::debug::PerfTrace::setValueCollectionEnabled(enabled_); }
private:
	bool enabled_;
};

const PropertyValue &stagingField(const std::vector<LogicalGraphPropertyEntry> &properties, std::string_view key) {
	const auto found = std::ranges::find(properties, key, &LogicalGraphPropertyEntry::key);
	if (found == properties.end()) throw std::runtime_error("missing staged test field");
	return found->value;
}
}

TEST_F(RelocatableStorageBackendTest, RepeatedRawPropertyStatementsRetainOnlyChangedNodeAndEdgeFields) {
	materializeNativeDatabase();
	auto options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const PropertyValue wide(std::string(16000U, 'w'));
	const auto node = backend.createLogicalNodes({{{"Raw"}, {{"payload", wide}, {"rank", PropertyValue(0)}}}}).front();
	const auto edge = backend.createLogicalEdges({{node, node, "RAW", {{"payload", wide}, {"rank", PropertyValue(0)}}}}).front();
	ASSERT_EQ(node, edge); // Distinct entity kinds must survive equal logical IDs.
	auto pinned = backend.openLogicalGraphReadSnapshot();
	auto transaction = backend.beginLogicalGraphTransaction(false);
	ASSERT_TRUE(transaction->supportsPropertyPatchStaging());
	LogicalGraphTransactionOverlay overlay(*transaction);
	{
		StagingTrace scope;
		for (int rank = 1; rank <= 32; ++rank) {
			auto visible = overlay.openLogicalGraphReadSnapshot();
			const std::vector<LogicalGraphPropertyPatchRequest> patches{
				{stagingNode(node), {{"rank", PropertyValue(rank)}, {"payload", wide}}, {"absent"}},
				{stagingEdge(edge), {{"rank", PropertyValue(rank)}, {"payload", wide}}, {}}};
			LogicalGraphMutationBatch companion;
			companion.node_label_replacements = {{stagingNode(node), {"Raw", "Updated"}}};
			static_cast<void>(overlay.executeLogicalGraphMutationBatchWithPropertyPatches(
				*visible, companion, patches, 10U, *visible->mutationFence()));
			EXPECT_EQ(stagingField(visible->readNodeShaped(node, {.all_properties = true}).properties, "rank"), PropertyValue(rank));
			EXPECT_EQ(stagingField(visible->readEdgeShaped(edge, {.all_properties = true}).properties, "rank"), PropertyValue(rank));
		}
		const auto trace = graph::debug::PerfTrace::snapshotAndReset();
		ASSERT_TRUE(trace.contains("native_graph.write.transaction_retained_property_targets"));
		EXPECT_EQ(trace.at("native_graph.write.transaction_retained_property_targets").maximumValue, 0);
		EXPECT_EQ(trace.at("native_graph.write.transaction_retained_patch_targets").maximumValue, 2);
		EXPECT_EQ(trace.at("native_graph.write.transaction_retained_patch_entries").maximumValue, 2);
		EXPECT_EQ(trace.at("native_graph.write.transaction_retained_patch_entries").valueCalls, 32U);
		static_cast<void>(transaction->commit());
	}
	auto after = backend.openLogicalGraphReadSnapshot();
	EXPECT_EQ(stagingField(after->readNodeShaped(node, {.all_properties = true}).properties, "rank"), PropertyValue(32));
	EXPECT_EQ(stagingField(after->readEdgeShaped(edge, {.all_properties = true}).properties, "rank"), PropertyValue(32));
	EXPECT_EQ(stagingField(after->readNodeShaped(node, {.all_properties = true}).properties, "payload"), wide);
	EXPECT_EQ(stagingField(pinned->readNodeShaped(node, {.all_properties = true}).properties, "rank"), PropertyValue(0));
	after.reset();
	pinned.reset();
	transaction.reset();
	backend.close();
	RelocatableStorageBackend reopened(backendOptions());
	reopened.open();
	EXPECT_EQ(stagingField(reopened.openLogicalGraphReadSnapshot()->readEdgeShaped(edge, {.all_properties = true}).properties, "rank"), PropertyValue(32));
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest, RawPropertyStagingFoldsLastOperationAcrossSetsRemovalsAndTypes) {
	materializeNativeDatabase();
	RelocatableStorageBackend backend(writableStagingOptions(backendOptions()));
	backend.open();
	const auto node = backend.createLogicalNodes({{{}, {{"rank", PropertyValue(0)}, {"keep", PropertyValue(5)}}}}).front();
	auto transaction = backend.beginLogicalGraphTransaction(false);
	const auto stage = [&](std::vector<LogicalGraphWriteProperty> sets, std::vector<std::string> removals) {
		static_cast<void>(transaction->stageMutationBatchWithPropertyPatches({},
			{{stagingNode(node), std::move(sets), std::move(removals)}}, 8U));
	};
	stage({{"rank", PropertyValue(1)}, {"new", PropertyValue(2)}}, {});
	stage({}, {"rank", "new"});
	stage({{"rank", PropertyValue("final")}, {"new", PropertyValue(true)}}, {});
	stage({{"added", PropertyValue(6)}}, {"keep"});
	static_cast<void>(transaction->commit());
	auto snapshot = backend.openLogicalGraphReadSnapshot();
	const auto properties = snapshot->readNodeShaped(node, {.all_properties = true}).properties;
	ASSERT_EQ(properties.size(), 3U);
	EXPECT_EQ(stagingField(properties, "rank"), PropertyValue("final"));
	EXPECT_EQ(stagingField(properties, "new"), PropertyValue(true));
	EXPECT_EQ(stagingField(properties, "added"), PropertyValue(6));
	snapshot.reset();
	transaction.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, CompleteReplacementResetsRawFieldsAndFollowingPatchPreservesStoredNull) {
	materializeNativeDatabase();
	RelocatableStorageBackend backend(writableStagingOptions(backendOptions()));
	backend.open();
	const auto node = backend.createLogicalNodes({{{}, {{"old", PropertyValue(1)}}}}).front();
	auto transaction = backend.beginLogicalGraphTransaction(false);
	static_cast<void>(transaction->stageMutationBatchWithPropertyPatches({}, {{stagingNode(node), {{"obsolete", PropertyValue(2)}}, {"old"}}}, 4U));
	LogicalGraphMutationBatch complete;
	complete.property_replacements = {{stagingNode(node), {{"null", PropertyValue()}, {"rank", PropertyValue(3)}}}};
	static_cast<void>(transaction->stageMutationBatch(complete));
	static_cast<void>(transaction->stageMutationBatchWithPropertyPatches({}, {{stagingNode(node), {{"rank", PropertyValue(4)}}, {}}}, 3U));
	static_cast<void>(transaction->commit());
	auto snapshot = backend.openLogicalGraphReadSnapshot();
	const auto properties = snapshot->readNodeShaped(node, {.all_properties = true}).properties;
	ASSERT_EQ(properties.size(), 2U);
	EXPECT_EQ(stagingField(properties, "null").getType(), PropertyType::NULL_TYPE);
	EXPECT_EQ(stagingField(properties, "rank"), PropertyValue(4));
	snapshot.reset();
	transaction.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, CompleteThenRawThenCompleteThenRawKeepsOnlyTheLastFullMap) {
	materializeNativeDatabase();
	RelocatableStorageBackend backend(writableStagingOptions(backendOptions()));
	backend.open();
	const auto node = backend.createLogicalNodes({{{}, {{"base", PropertyValue(1)}}}}).front();
	auto transaction = backend.beginLogicalGraphTransaction(false);
	LogicalGraphMutationBatch complete;
	complete.property_replacements = {{stagingNode(node), {{"first", PropertyValue(2)}}}};
	static_cast<void>(transaction->stageMutationBatch(complete));
	static_cast<void>(transaction->stageMutationBatchWithPropertyPatches({}, {{stagingNode(node), {{"discard", PropertyValue(3)}}, {"first"}}}, 3U));
	complete.property_replacements = {{stagingNode(node), {{"last", PropertyValue(4)}}}};
	static_cast<void>(transaction->stageMutationBatch(complete));
	static_cast<void>(transaction->stageMutationBatchWithPropertyPatches({}, {{stagingNode(node), {{"last", PropertyValue(5)}}, {}}}, 2U));
	static_cast<void>(transaction->commit());
	auto snapshot = backend.openLogicalGraphReadSnapshot();
	const auto properties = snapshot->readNodeShaped(node, {.all_properties = true}).properties;
	ASSERT_EQ(properties.size(), 1U);
	EXPECT_EQ(stagingField(properties, "last"), PropertyValue(5));
	snapshot.reset();
	transaction.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, RawPropertyStagingFoldsCreatedOrdinalsAndLaterProvisionalReferences) {
	materializeNativeDatabase();
	RelocatableStorageBackend backend(writableStagingOptions(backendOptions()));
	backend.open();
	auto transaction = backend.beginLogicalGraphTransaction(false);
	const LogicalGraphMutationEntityReference createdNode{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 0U};
	const LogicalGraphMutationEntityReference createdEdge{LogicalGraphMutationEntityKind::LGMK_EDGE, LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE, 0U};
	LogicalGraphMutationBatch creates;
	creates.node_creates = {{{}, {{"old", PropertyValue(1)}}}};
	creates.edge_creates = {{createdNode, createdNode, "CREATED", {{"old", PropertyValue(2)}}}};
	const auto first = transaction->stageMutationBatchWithPropertyPatches(creates,
		{{createdNode, {{"rank", PropertyValue(3)}}, {"old"}}, {createdEdge, {{"rank", PropertyValue(4)}}, {"old"}}}, 6U);
	ASSERT_EQ(first.created_node_ids.size(), 1U);
	ASSERT_EQ(first.created_edge_ids.size(), 1U);
	static_cast<void>(transaction->stageMutationBatchWithPropertyPatches({},
		{{stagingNode(first.created_node_ids.front()), {{"rank", PropertyValue(5)}}, {}},
		 {stagingEdge(first.created_edge_ids.front()), {{"rank", PropertyValue(6)}}, {}}}, 4U));
	const auto committed = transaction->commit();
	EXPECT_EQ(committed, first);
	auto snapshot = backend.openLogicalGraphReadSnapshot();
	const auto node = snapshot->readNodeShaped(first.created_node_ids.front(), {.all_properties = true});
	const auto edge = snapshot->readEdgeShaped(first.created_edge_ids.front(), {.all_properties = true});
	ASSERT_EQ(node.properties.size(), 1U);
	ASSERT_EQ(edge.properties.size(), 1U);
	EXPECT_EQ(stagingField(node.properties, "rank"), PropertyValue(5));
	EXPECT_EQ(stagingField(edge.properties, "rank"), PropertyValue(6));
	snapshot.reset();
	transaction.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, InvalidRawPropertyStagePreservesEarlierFieldsAndActiveTransaction) {
	materializeNativeDatabase();
	RelocatableStorageBackend backend(writableStagingOptions(backendOptions()));
	backend.open();
	const auto node = backend.createLogicalNodes({{{}, {{"rank", PropertyValue(0)}}}}).front();
	auto transaction = backend.beginLogicalGraphTransaction(false);
	static_cast<void>(transaction->stageMutationBatchWithPropertyPatches({}, {{stagingNode(node), {{"rank", PropertyValue(1)}}, {}}}, 2U));
	const std::vector<std::vector<LogicalGraphPropertyPatchRequest>> invalid{
		{{stagingNode(node), {{"", PropertyValue(2)}}, {}}},
		{{stagingNode(node), {{"rank", PropertyValue()}}, {}}},
		{{stagingNode(node), {{"rank", PropertyValue(2)}, {"rank", PropertyValue(3)}}, {}}},
		{{stagingNode(node), {{"rank", PropertyValue(2)}}, {"rank"}}},
		{{stagingNode(node), {}, {"rank", "rank"}}},
		{{stagingNode(0U), {{"rank", PropertyValue(2)}}, {}}},
		{{stagingNode(node), {}, {}}, {stagingNode(node), {}, {}}},
		{{{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE, 0U}, {}, {}}}};
	for (const auto &patches : invalid) {
		EXPECT_THROW(static_cast<void>(transaction->stageMutationBatchWithPropertyPatches({}, patches, 8U)), std::invalid_argument);
		EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_ACTIVE);
	}
	LogicalGraphMutationBatch overlap;
	overlap.property_replacements = {{stagingNode(node), {}}};
	EXPECT_THROW(static_cast<void>(transaction->stageMutationBatchWithPropertyPatches(overlap,
		{{stagingNode(node), {{"rank", PropertyValue(2)}}, {}}}, 2U)), std::invalid_argument);
	LogicalGraphMutationBatch deletion;
	deletion.node_deletes = {{node, false, 0U}};
	EXPECT_THROW(static_cast<void>(transaction->stageMutationBatchWithPropertyPatches(deletion,
		{{stagingNode(node), {{"rank", PropertyValue(2)}}, {}}}, 2U)), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(transaction->stageMutationBatchWithPropertyPatches({},
		{{stagingNode(node), {{"rank", PropertyValue(2)}, {"extra", PropertyValue(3)}}, {}}}, 1U)), std::length_error);
	static_cast<void>(transaction->commit());
	EXPECT_EQ(stagingField(backend.openLogicalGraphReadSnapshot()->readNodeShaped(node, {.all_properties = true}).properties, "rank"), PropertyValue(1));
	transaction.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, FailedCreatedSourceAdmissionDoesNotReplaceEarlierCreationFields) {
	materializeNativeDatabase();
	RelocatableStorageBackend backend(writableStagingOptions(backendOptions()));
	backend.open();
	auto transaction = backend.beginLogicalGraphTransaction(false);
	LogicalGraphMutationBatch create;
	create.node_creates = {{{}, {{"keep", PropertyValue(1)}, {"rank", PropertyValue(2)}}}};
	const auto ids = transaction->stageMutationBatch(create).created_node_ids;
	EXPECT_THROW(static_cast<void>(transaction->stageMutationBatchWithPropertyPatches({},
		{{stagingNode(ids.front()), {{"rank", PropertyValue(3)}}, {}}}, 1U)), std::length_error);
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_ACTIVE);
	static_cast<void>(transaction->commit());
	const auto properties = backend.openLogicalGraphReadSnapshot()->readNodeShaped(ids.front(), {.all_properties = true}).properties;
	EXPECT_EQ(stagingField(properties, "rank"), PropertyValue(2));
	EXPECT_EQ(stagingField(properties, "keep"), PropertyValue(1));
	transaction.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, RawPropertyWorkBoundOverflowRejectsOnlyTheNewStatement) {
	materializeNativeDatabase();
	RelocatableStorageBackend backend(writableStagingOptions(backendOptions()));
	backend.open();
	const auto node = backend.createLogicalNodes({{{}, {{"rank", PropertyValue(0)}}}}).front();
	auto transaction = backend.beginLogicalGraphTransaction(false);
	static_cast<void>(transaction->stageMutationBatchWithPropertyPatches({},
		{{stagingNode(node), {{"rank", PropertyValue(1)}}, {}}}, (std::numeric_limits<size_t>::max)()));
	EXPECT_THROW(static_cast<void>(transaction->stageMutationBatchWithPropertyPatches({},
		{{stagingNode(node), {{"rank", PropertyValue(2)}}, {}}}, 2U)), std::overflow_error);
	static_cast<void>(transaction->commit());
	EXPECT_EQ(stagingField(backend.openLogicalGraphReadSnapshot()->readNodeShaped(node, {.all_properties = true}).properties, "rank"), PropertyValue(1));
	transaction.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, RawOnlyPropertyTransactionConflictsWithAnInterveningWriter) {
	materializeNativeDatabase();
	RelocatableStorageBackend backend(writableStagingOptions(backendOptions()));
	backend.open();
	const auto node = backend.createLogicalNodes({{{}, {{"rank", PropertyValue(0)}}}}).front();
	auto transaction = backend.beginLogicalGraphTransaction(false);
	static_cast<void>(transaction->stageMutationBatchWithPropertyPatches({}, {{stagingNode(node), {{"rank", PropertyValue(1)}}, {}}}, 2U));
	backend.replaceLogicalNodeProperties({{node, {{"rank", PropertyValue(2)}}}});
	EXPECT_THROW(static_cast<void>(transaction->commit()), LogicalGraphMutationConflict);
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_CONFLICTED);
	EXPECT_EQ(stagingField(backend.openLogicalGraphReadSnapshot()->readNodeShaped(node, {.all_properties = true}).properties, "rank"), PropertyValue(2));
	transaction.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, RawPropertyCommitSourceBoundFailurePublishesNeitherFieldsNorLabels) {
	materializeNativeDatabase();
	RelocatableStorageBackend backend(writableStagingOptions(backendOptions()));
	backend.open();
	const auto node = backend.createLogicalNodes({{{"Old"}, {{"rank", PropertyValue(0)}, {"keep", PropertyValue(1)}}}}).front();
	auto before = backend.openLogicalGraphReadSnapshot();
	auto transaction = backend.beginLogicalGraphTransaction(false);
	LogicalGraphMutationBatch companion;
	companion.node_label_replacements = {{stagingNode(node), {"New"}}};
	static_cast<void>(transaction->stageMutationBatchWithPropertyPatches(companion,
		{{stagingNode(node), {{"rank", PropertyValue(2)}}, {}}}, 1U));
	EXPECT_THROW(static_cast<void>(transaction->commit()), std::runtime_error);
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_ROLLED_BACK);
	auto after = backend.openLogicalGraphReadSnapshot();
	EXPECT_EQ(after->mutationFence(), before->mutationFence());
	const auto unchanged = after->readNodeShaped(node, {.include_labels = true, .all_properties = true});
	EXPECT_EQ(unchanged.labels, (std::vector<std::string>{"Old"}));
	EXPECT_EQ(stagingField(unchanged.properties, "rank"), PropertyValue(0));
	after.reset();
	before.reset();
	transaction.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, CancellationAtEveryRawStagingCheckpointRestoresOverlayAndPriorTransactionFields) {
	materializeNativeDatabase();
	RelocatableStorageBackend backend(writableStagingOptions(backendOptions()));
	backend.open();
	const auto node = backend.createLogicalNodes({{{}, {{"rank", PropertyValue(0)}, {"keep", PropertyValue(5)}}}}).front();
	const auto prepare = [&](LogicalGraphTransactionOverlay &overlay, int scenario) {
		if (scenario == 2)
			return overlay.createLogicalNodes({{{}, {{"rank", PropertyValue(1)}, {"keep", PropertyValue(5)}}}}).front();
		if (scenario == 1) overlay.replaceLogicalNodeProperties({{node, {{"rank", PropertyValue(1)}, {"keep", PropertyValue(5)}}}});
		else {
			auto source = overlay.openLogicalGraphReadSnapshot();
			const std::vector<LogicalGraphPropertyPatchRequest> patches{{stagingNode(node), {{"rank", PropertyValue(1)}}, {}}};
			static_cast<void>(overlay.executeLogicalGraphPropertyPatches(*source, patches, 3U, *source->mutationFence()));
		}
		return node;
	};
	for (int scenario = 0; scenario < 3; ++scenario) {
		size_t checkpointCount = 0U;
		{
			auto transaction = backend.beginLogicalGraphTransaction(false);
			LogicalGraphTransactionOverlay overlay(*transaction);
			const auto target = prepare(overlay, scenario);
			auto source = overlay.openLogicalGraphReadSnapshot();
			LogicalGraphMutationReadControl control;
			control.checkpoint = [&] { ++checkpointCount; };
			const std::vector<LogicalGraphPropertyPatchRequest> patches{{stagingNode(target), {{"rank", PropertyValue(2)}}, {}}};
			static_cast<void>(overlay.executeLogicalGraphPropertyPatches(*source, patches, 3U, *source->mutationFence(), control));
			transaction->rollback();
		}
		ASSERT_GT(checkpointCount, 0U);
		ASSERT_LT(checkpointCount, 256U);
		for (size_t rejected = 1U; rejected <= checkpointCount; ++rejected) {
			SCOPED_TRACE(::testing::Message() << "scenario=" << scenario << " checkpoint=" << rejected);
			backend.replaceLogicalNodeProperties({{node, {{"rank", PropertyValue(0)}, {"keep", PropertyValue(5)}}}});
			auto transaction = backend.beginLogicalGraphTransaction(false);
			LogicalGraphTransactionOverlay overlay(*transaction);
			const auto target = prepare(overlay, scenario);
			auto source = overlay.openLogicalGraphReadSnapshot();
			const auto fence = *source->mutationFence();
			size_t calls = 0U;
			LogicalGraphMutationReadControl control;
			control.checkpoint = [&] { if (++calls == rejected) throw std::runtime_error("cancel raw staging"); };
			const std::vector<LogicalGraphPropertyPatchRequest> patches{{stagingNode(target), {{"rank", PropertyValue(2)}}, {}}};
			EXPECT_THROW(static_cast<void>(overlay.executeLogicalGraphPropertyPatches(*source, patches, 3U, fence, control)), std::runtime_error);
			EXPECT_EQ(source->mutationFence(), fence);
			EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_ACTIVE);
			const auto visible = source->readNodeShaped(target, {.all_properties = true}).properties;
			EXPECT_EQ(stagingField(visible, "rank"), PropertyValue(1));
			EXPECT_EQ(stagingField(visible, "keep"), PropertyValue(5));
			static_cast<void>(transaction->commit());
			EXPECT_EQ(stagingField(backend.openLogicalGraphReadSnapshot()->readNodeShaped(target, {.all_properties = true}).properties, "rank"), PropertyValue(1));
		}
	}
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, RawOnlyStagingRejectsStreamingModeMixAndReadOnlyBeforeMutation) {
	materializeNativeDatabase();
	RelocatableStorageBackend backend(writableStagingOptions(backendOptions()));
	backend.open();
	const auto node = backend.createLogicalNodes({{{}, {{"rank", PropertyValue(0)}}}}).front();
	const std::vector<LogicalGraphPropertyPatchRequest> patches{{stagingNode(node), {{"rank", PropertyValue(1)}}, {}}};
	{
		auto readOnly = backend.beginLogicalGraphTransaction(true);
		EXPECT_THROW(static_cast<void>(readOnly->stageMutationBatchWithPropertyPatches({}, patches, 2U)), std::logic_error);
		EXPECT_EQ(readOnly->state(), LogicalGraphTransactionState::LGTTS_ACTIVE);
		readOnly->rollback();
	}
	auto transaction = backend.beginLogicalGraphTransaction(false);
	static_cast<void>(transaction->stageMutationBatchWithPropertyPatches({}, patches, 2U));
	auto *bulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(transaction.get());
	ASSERT_NE(bulk, nullptr);
	const std::array<uint64_t, 1U> sources{node}, targets{node};
	EXPECT_THROW(static_cast<void>(bulk->appendPersistedBulkEdges({"MIX", sources, targets, {}})), std::logic_error);
	EXPECT_FALSE(bulk->hasStreamedBulkEdges());
	static_cast<void>(transaction->commit());
	EXPECT_EQ(stagingField(backend.openLogicalGraphReadSnapshot()->readNodeShaped(node, {.all_properties = true}).properties, "rank"), PropertyValue(1));
	transaction.reset();
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, RawPropertyRestorationPreservesRootsAndDoesNotCreateRemovedTemporaryTokens) {
	materializeNativeDatabase();
	RelocatableStorageBackend backend(writableStagingOptions(backendOptions()));
	backend.open();
	const auto node = backend.createLogicalNodes({{{}, {{"rank", PropertyValue(0)}, {"keep", PropertyValue(5)}}}}).front();
	auto before = backend.openLogicalGraphReadSnapshot();
	EXPECT_FALSE(before->findPropertyToken("temporary_raw_field"));
	auto transaction = backend.beginLogicalGraphTransaction(false);
	static_cast<void>(transaction->stageMutationBatchWithPropertyPatches({},
		{{stagingNode(node), {{"rank", PropertyValue(1)}, {"temporary_raw_field", PropertyValue(2)}}, {}}}, 4U));
	static_cast<void>(transaction->stageMutationBatchWithPropertyPatches({},
		{{stagingNode(node), {{"rank", PropertyValue(0)}}, {"temporary_raw_field"}}}, 5U));
	static_cast<void>(transaction->commit());
	auto after = backend.openLogicalGraphReadSnapshot();
	EXPECT_EQ(after->mutationFence(), before->mutationFence());
	EXPECT_FALSE(after->findPropertyToken("temporary_raw_field"));
	EXPECT_EQ(after->readNodeShaped(node, {.all_properties = true}).properties,
		before->readNodeShaped(node, {.all_properties = true}).properties);
	after.reset();
	before.reset();
	transaction.reset();
	backend.close();
}
