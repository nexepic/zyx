#include <atomic>
#include <barrier>
#include <thread>
#include "storage/relocatable/backend/RelocatableStorageBackendTestFixture.hpp"
using namespace relocatable_storage_backend_test;
namespace {
LogicalGraphMutationBatch replace(uint64_t id,int value){LogicalGraphMutationBatch b;b.property_replacements={{{LogicalGraphMutationEntityKind::LGMK_NODE,LogicalGraphMutationReferenceKind::LGRK_PERSISTED,id},{{"value",PropertyValue(value)}}}};return b;}
}
TEST_F(RelocatableStorageBackendTest, BlindPropertyRebaseMergesDisjointOwnersOnOnePhysicalPage) {
	materializeNativeDatabase();auto options=backendOptions();options.open_mode=OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(options);backend.open();const auto ids=backend.createLogicalNodes({{{"Rebase"},{{"value",PropertyValue(0)}}},{{"Rebase"},{{"value",PropertyValue(0)}}}});
	{
		auto file=std::make_shared<std::fstream>(data_path_,std::ios::in|std::ios::binary);auto io=std::make_shared<StorageIO>(file,INVALID_FILE_HANDLE,INVALID_FILE_HANDLE);
		auto metadata=RelocatableMetadataStore::openReadOnly(io);auto pin=metadata->openSnapshot();
		const auto a=metadata->findEntityPropertyLocation(pin,{EntityPropertyOwnerKind::EPOK_NODE,ids[0]});const auto b=metadata->findEntityPropertyLocation(pin,{EntityPropertyOwnerKind::EPOK_NODE,ids[1]});ASSERT_EQ(a.page_id,b.page_id);ASSERT_NE(a.slot_id,b.slot_id);
	}
	auto old=backend.openLogicalGraphReadSnapshot();auto a=backend.beginLogicalGraphTransaction(false),b=backend.beginLogicalGraphTransaction(false);
	(void)a->stageMutationBatch(replace(ids[0],11));(void)b->stageMutationBatch(replace(ids[1],22));
	std::barrier ready(3);std::atomic<int> successes{0},failures{0};const auto commit=[&](auto *txn){ready.arrive_and_wait();try{(void)txn->commit();++successes;}catch(...){++failures;}};
	std::thread first(commit,a.get()),second(commit,b.get());ready.arrive_and_wait();first.join();second.join();
	EXPECT_EQ(successes,2);
	EXPECT_EQ(failures,0);
	for(auto id:ids)EXPECT_EQ(old->readNodeProperty(id,"value"),PropertyValue(0));old.reset();
	auto fresh=backend.openLogicalGraphReadSnapshot();
	EXPECT_EQ(fresh->readNodeProperty(ids[0],"value"),PropertyValue(11));
	EXPECT_EQ(fresh->readNodeProperty(ids[1],"value"),PropertyValue(22));fresh.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));backend.close();backend.open();fresh=backend.openLogicalGraphReadSnapshot();
	EXPECT_EQ(fresh->readNodeProperty(ids[0],"value"),PropertyValue(11));
	EXPECT_EQ(fresh->readNodeProperty(ids[1],"value"),PropertyValue(22));fresh.reset();backend.close();
}
TEST_F(RelocatableStorageBackendTest, BlindPropertyRebaseRejectsOwnerChangeDeletionAndAbsenceChange) {
	materializeNativeDatabase();auto options=backendOptions();options.open_mode=OpenMode::OPEN_EXISTING_FILE;RelocatableStorageBackend backend(options);backend.open();
	const auto ids=backend.createLogicalNodes({{{"Rebase"},{{"value",PropertyValue(0)}}},{{"Rebase"},{}},{{"Rebase"},{{"value",PropertyValue(0)}}}});
	for(size_t i=0;i<ids.size();++i){SCOPED_TRACE(i);auto txn=backend.beginLogicalGraphTransaction(false);(void)txn->stageMutationBatch(replace(ids[i],9));
		if(i==2)backend.deleteLogicalNodes({{ids[i],false,0}});else backend.replaceLogicalNodeProperties({{ids[i],{{"value",PropertyValue(7)}}}});
		EXPECT_THROW(static_cast<void>(txn->commit()),LogicalGraphMutationConflict);
	EXPECT_EQ(txn->state(),LogicalGraphTransactionState::LGTTS_CONFLICTED);
	EXPECT_THROW(static_cast<void>(txn->commit()),std::logic_error);
	}
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));backend.close();
}
TEST_F(RelocatableStorageBackendTest, BlindPropertyRebaseRetainsFallbackAfterAnyExposedRead) {
	materializeNativeDatabase();auto options=backendOptions();options.open_mode=OpenMode::OPEN_EXISTING_FILE;RelocatableStorageBackend backend(options);backend.open();
	const auto ids=backend.createLogicalNodes({{{"Rebase"},{{"value",PropertyValue(0)}}},{{"Rebase"},{{"value",PropertyValue(0)}}}});
	auto txn=backend.beginLogicalGraphTransaction(false);
	EXPECT_EQ(txn->readSnapshot().readNodeProperty(ids[1],"value"),PropertyValue(0));(void)txn->stageMutationBatch(replace(ids[0],9));backend.replaceLogicalNodeProperties({{ids[1],{{"value",PropertyValue(7)}}}});
	EXPECT_THROW(static_cast<void>(txn->commit()),LogicalGraphMutationConflict);auto fresh=backend.openLogicalGraphReadSnapshot();
	EXPECT_EQ(fresh->readNodeProperty(ids[0],"value"),PropertyValue(0));fresh.reset();backend.close();
}
TEST_F(RelocatableStorageBackendTest, BlindPropertyRebaseValidatesLatestUniqueConstraintAndKeepsDefiniteFailureTerminal) {
	materializeNativeDatabase();auto options=backendOptions();options.open_mode=OpenMode::OPEN_EXISTING_FILE;RelocatableStorageBackend backend(options);backend.open();
	const auto ids=backend.createLogicalNodes({{{"Rebase"},{{"value",PropertyValue(1)}}},{{"Rebase"},{{"value",PropertyValue(2)}}}});
	auto a=backend.beginLogicalGraphTransaction(false),b=backend.beginLogicalGraphTransaction(false);(void)a->stageMutationBatch(replace(ids[0],7));(void)b->stageMutationBatch(replace(ids[1],7));
	// DDL after staging must be enforced by the latest-state executor too.
	(void)backend.createLogicalConstraint({"rebase_unique",LogicalGraphConstraintEntityKind::LGCEK_NODE,LogicalGraphConstraintKind::LGCK_UNIQUE,"Rebase",{"value"}});
	(void)a->commit();
	EXPECT_THROW(static_cast<void>(b->commit()),std::runtime_error);
	EXPECT_EQ(b->state(),LogicalGraphTransactionState::LGTTS_ROLLED_BACK);
	EXPECT_THROW(static_cast<void>(b->commit()),std::logic_error);
	auto fresh=backend.openLogicalGraphReadSnapshot();
	EXPECT_EQ(fresh->readNodeProperty(ids[0],"value"),PropertyValue(7));
	EXPECT_EQ(fresh->readNodeProperty(ids[1],"value"),PropertyValue(2));fresh.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));backend.close();
}
TEST_F(RelocatableStorageBackendTest, BlindPropertyRebaseRejectsChangedThenRestoredOwnerVersion) {
	materializeNativeDatabase();auto options=backendOptions();options.open_mode=OpenMode::OPEN_EXISTING_FILE;RelocatableStorageBackend backend(options);backend.open();
	const auto id=backend.createLogicalNodes({{{"Rebase"},{{"value",PropertyValue(0)}}}})[0];auto txn=backend.beginLogicalGraphTransaction(false);(void)txn->stageMutationBatch(replace(id,9));
	backend.replaceLogicalNodeProperties({{id,{{"value",PropertyValue(7)}}}});backend.replaceLogicalNodeProperties({{id,{{"value",PropertyValue(0)}}}});
	EXPECT_THROW(static_cast<void>(txn->commit()),LogicalGraphMutationConflict);
	EXPECT_EQ(txn->state(),LogicalGraphTransactionState::LGTTS_CONFLICTED);backend.close();
}
TEST_F(RelocatableStorageBackendTest, BlindPropertyRebaseFallsBackWithoutDiscardingOverCapDependencies) {
	materializeNativeDatabase();auto options=backendOptions();options.open_mode=OpenMode::OPEN_EXISTING_FILE;RelocatableStorageBackend backend(options);backend.open();
	std::vector<LogicalGraphNodeCreateRequest> creates(66,{{"Rebase"},{{"value",PropertyValue(0)}}});const auto ids=backend.createLogicalNodes(creates);auto txn=backend.beginLogicalGraphTransaction(false);LogicalGraphMutationBatch batch;
	for(size_t i=0;i<65;++i)batch.property_replacements.push_back(replace(ids[i],9).property_replacements[0]);(void)txn->stageMutationBatch(std::move(batch));
	backend.replaceLogicalNodeProperties({{ids[65],{{"value",PropertyValue(7)}}}});
	EXPECT_THROW(static_cast<void>(txn->commit()),LogicalGraphMutationConflict);
	auto fresh=backend.openLogicalGraphReadSnapshot();for(size_t i=0;i<65;++i)EXPECT_EQ(fresh->readNodeProperty(ids[i],"value"),PropertyValue(0));fresh.reset();backend.close();
}
