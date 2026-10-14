#include <barrier>
#include <thread>
#include <atomic>
#include "storage/relocatable/backend/RelocatableStorageBackendTestFixture.hpp"

using namespace relocatable_storage_backend_test;
namespace {
LogicalGraphMutationEntityReference isolationNode(uint64_t id) {
	return {LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, id};
}
LogicalGraphMutationBatch isolationSet(uint64_t id, std::string key, PropertyValue value) {
	LogicalGraphMutationBatch batch;
	batch.property_replacements = {{isolationNode(id), {{std::move(key), std::move(value)}}}};
	return batch;
}
}

TEST_F(RelocatableStorageBackendTest, IsolationWriteSkewRejectsSecondWriterAndPreservesReadOnlySnapshot) {
	materializeNativeDatabase();
	auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(options); backend.open();
	const auto ids = backend.createLogicalNodes({{{"Doctor"}, {{"on_call", PropertyValue(true)}}},
		{{"Doctor"}, {{"on_call", PropertyValue(true)}}}});
	ASSERT_EQ(ids.size(), 2U);
	auto reader = backend.beginLogicalGraphTransaction(true);
	auto first = backend.beginLogicalGraphTransaction(false), second = backend.beginLogicalGraphTransaction(false);
	for (auto *transaction : {first.get(), second.get()})
		for (auto id : ids) EXPECT_EQ(transaction->readSnapshot().readNodeProperty(id, "on_call"), PropertyValue(true));
	(void) first->stageMutationBatch(isolationSet(ids[0], "on_call", PropertyValue(false)));
	(void) second->stageMutationBatch(isolationSet(ids[1], "on_call", PropertyValue(false)));
	(void) first->commit();
	EXPECT_THROW(static_cast<void>(second->commit()), LogicalGraphMutationConflict);
	EXPECT_EQ(second->state(), LogicalGraphTransactionState::LGTTS_CONFLICTED);
	for (auto id : ids) EXPECT_EQ(reader->readSnapshot().readNodeProperty(id, "on_call"), PropertyValue(true));
	(void) reader->commit();
	auto fresh = backend.openLogicalGraphReadSnapshot();
	EXPECT_EQ(fresh->readNodeProperty(ids[0], "on_call"), PropertyValue(false));
	EXPECT_EQ(fresh->readNodeProperty(ids[1], "on_call"), PropertyValue(true));
	fresh.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity())); backend.close();
}

TEST_F(RelocatableStorageBackendTest, IsolationRangeReadsRetainOldMembershipAndDerivedWriteConflictsOnPhantom) {
	materializeNativeDatabase();
	auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(options); backend.open();
	(void) backend.createLogicalNodes({{{"Range"}, {}}});
	auto transaction = backend.beginLogicalGraphTransaction(false);
	const auto count = [&] {
		size_t rows = 0U;
		transaction->readSnapshot().visitNodes(10U, [&](const auto &) { ++rows; return true; });
		return rows;
	};
	EXPECT_EQ(count(), 1U);
	(void) backend.createLogicalNodes({{{"Range"}, {}}});
	EXPECT_EQ(count(), 1U);
	LogicalGraphMutationBatch derived;
	derived.node_creates = {{{"Summary"}, {{"observed", PropertyValue(count())}}}};
	const auto staged = transaction->stageMutationBatch(std::move(derived));
	EXPECT_THROW(static_cast<void>(transaction->commit()), LogicalGraphMutationConflict);
	auto fresh = backend.openLogicalGraphReadSnapshot();
	EXPECT_FALSE(fresh->containsNode(staged.created_node_ids.at(0U)));
	fresh.reset(); EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity())); backend.close();
}

TEST_F(RelocatableStorageBackendTest, IsolationEndpointDeletionRejectsStagedEdgeWithoutDanglingPublication) {
	materializeNativeDatabase();
	auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(options); backend.open();
	const auto ids = backend.createLogicalNodes({{{"Endpoint"}, {}}, {{"Endpoint"}, {}}});
	auto edgeTransaction = backend.beginLogicalGraphTransaction(false);
	LogicalGraphMutationBatch edge;
	edge.edge_creates = {{isolationNode(ids[0]), isolationNode(ids[1]), "LINK", {}}};
	const auto staged = edgeTransaction->stageMutationBatch(std::move(edge));
	auto deletion = backend.beginLogicalGraphTransaction(false);
	LogicalGraphMutationBatch remove; remove.node_deletes = {{ids[1], false, 0U}};
	(void) deletion->stageMutationBatch(std::move(remove)); (void) deletion->commit();
	EXPECT_THROW(static_cast<void>(edgeTransaction->commit()), LogicalGraphMutationConflict);
	auto fresh = backend.openLogicalGraphReadSnapshot();
	EXPECT_FALSE(fresh->containsNode(ids[1]));
	EXPECT_THROW(static_cast<void>(fresh->readEdge(staged.created_edge_ids.at(0U))), FormatException);
	fresh.reset(); EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity())); backend.close();
}

TEST_F(RelocatableStorageBackendTest, IsolationUniqueWritersConflictAndFreshRetryStillEnforcesConstraint) {
	materializeNativeDatabase();
	auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(options); backend.open();
	(void) backend.createLogicalConstraint({"isolation_unique", LogicalGraphConstraintEntityKind::LGCEK_NODE,
		LogicalGraphConstraintKind::LGCK_UNIQUE, "Unique", {"key"}});
	auto first = backend.beginLogicalGraphTransaction(false), second = backend.beginLogicalGraphTransaction(false);
	LogicalGraphMutationBatch a, b;
	a.node_creates = {{{"Unique"}, {{"key", PropertyValue(7)}}}}; b = a;
	const auto firstIds = first->stageMutationBatch(std::move(a));
	const auto secondIds = second->stageMutationBatch(std::move(b));
	(void) first->commit();
	EXPECT_THROW(static_cast<void>(second->commit()), LogicalGraphMutationConflict);
	EXPECT_THROW(static_cast<void>(second->commit()), std::logic_error);
	auto retry = backend.beginLogicalGraphTransaction(false);
	LogicalGraphMutationBatch duplicate; duplicate.node_creates = {{{"Unique"}, {{"key", PropertyValue(7)}}}};
	(void) retry->stageMutationBatch(std::move(duplicate));
	EXPECT_THROW(static_cast<void>(retry->commit()), std::runtime_error);
	EXPECT_EQ(retry->state(), LogicalGraphTransactionState::LGTTS_ROLLED_BACK);
	auto fresh = backend.openLogicalGraphReadSnapshot();
	EXPECT_TRUE(fresh->containsNode(firstIds.created_node_ids.at(0U)));
	EXPECT_FALSE(fresh->containsNode(secondIds.created_node_ids.at(0U)));
	fresh.reset(); EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity())); backend.close();
}

TEST_F(RelocatableStorageBackendTest, IsolationDisjointWritesOnSharedPropertyPageRejectStaleCopyAndFreshRetryPreservesBoth) {
	materializeNativeDatabase();
	auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(options); backend.open();
	const auto ids = backend.createLogicalNodes({{{"Shared"}, {{"value", PropertyValue(0)}}},
		{{"Shared"}, {{"value", PropertyValue(0)}}}});
	{
		auto data = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::binary);
		auto io = std::make_shared<StorageIO>(data, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		auto metadata = RelocatableMetadataStore::openReadOnly(io);
		auto snapshot = metadata->openSnapshot();
		const auto a = metadata->findEntityPropertyLocation(snapshot, {EntityPropertyOwnerKind::EPOK_NODE, ids[0]});
		const auto b = metadata->findEntityPropertyLocation(snapshot, {EntityPropertyOwnerKind::EPOK_NODE, ids[1]});
		ASSERT_EQ(a.page_id, b.page_id); ASSERT_NE(a.slot_id, b.slot_id);
	}
	auto first = backend.beginLogicalGraphTransaction(false), second = backend.beginLogicalGraphTransaction(false);
	(void) first->stageMutationBatch(isolationSet(ids[0], "value", PropertyValue(11)));
	(void) second->stageMutationBatch(isolationSet(ids[1], "value", PropertyValue(22)));
	(void) first->commit();
	EXPECT_THROW(static_cast<void>(second->commit()), LogicalGraphMutationConflict);
	auto retry = backend.beginLogicalGraphTransaction(false);
	(void) retry->stageMutationBatch(isolationSet(ids[1], "value", PropertyValue(22))); (void) retry->commit();
	auto fresh = backend.openLogicalGraphReadSnapshot();
	EXPECT_EQ(fresh->readNodeProperty(ids[0], "value"), PropertyValue(11));
	EXPECT_EQ(fresh->readNodeProperty(ids[1], "value"), PropertyValue(22));
	fresh.reset(); EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity())); backend.close();
	RelocatableStorageBackend reopened(options); reopened.open();
	auto durable = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(durable->readNodeProperty(ids[0], "value"), PropertyValue(11));
	EXPECT_EQ(durable->readNodeProperty(ids[1], "value"), PropertyValue(22));
	durable.reset(); reopened.close();
}

TEST_F(RelocatableStorageBackendTest, IsolationSimultaneousCommitHasOneWinnerAndExplicitFreshRetry) {
	materializeNativeDatabase();auto options=backendOptions();options.open_mode=OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(options);backend.open();
	const auto ids=backend.createLogicalNodes({{{"Race"},{{"value",PropertyValue(0)}}},{{"Race"},{{"value",PropertyValue(0)}}}});
	auto reader=backend.beginLogicalGraphTransaction(true);
	auto first=backend.beginLogicalGraphTransaction(false),second=backend.beginLogicalGraphTransaction(false);
	(void)first->stageMutationBatch(isolationSet(ids[0],"value",PropertyValue(11)));
	(void)second->stageMutationBatch(isolationSet(ids[1],"value",PropertyValue(22)));
	std::barrier start(3);std::atomic<int> committed{0},conflicted{0},unexpected{0};
	const auto commit=[&](auto *transaction){start.arrive_and_wait();try{(void)transaction->commit();++committed;}catch(const LogicalGraphMutationConflict &){++conflicted;}catch(...){++unexpected;}};
	std::thread a(commit,first.get()),b(commit,second.get());start.arrive_and_wait();a.join();b.join();
	EXPECT_EQ(committed,1);EXPECT_EQ(conflicted,1);EXPECT_EQ(unexpected,0);
	for(auto id:ids)EXPECT_EQ(reader->readSnapshot().readNodeProperty(id,"value"),PropertyValue(0));
	const bool firstLost=first->state()==LogicalGraphTransactionState::LGTTS_CONFLICTED;
	auto &loser=firstLost?first:second;EXPECT_THROW(static_cast<void>(loser->commit()),std::logic_error);
	auto retry=backend.beginLogicalGraphTransaction(false);
	(void)retry->stageMutationBatch(isolationSet(ids[firstLost?0:1],"value",PropertyValue(firstLost?11:22)));(void)retry->commit();
	auto fresh=backend.openLogicalGraphReadSnapshot();EXPECT_EQ(fresh->readNodeProperty(ids[0],"value"),PropertyValue(11));EXPECT_EQ(fresh->readNodeProperty(ids[1],"value"),PropertyValue(22));
	fresh.reset();(void)reader->commit();EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));backend.close();
}
