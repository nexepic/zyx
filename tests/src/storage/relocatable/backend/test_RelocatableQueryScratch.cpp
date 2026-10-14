#include "storage/relocatable/backend/RelocatableStorageBackendTestFixture.hpp"
#include "graph/storage/LogicalQueryScratch.hpp"
using namespace relocatable_storage_backend_test;
TEST_F(RelocatableStorageBackendTest, QueryScratchOwnsPrivateHandlesAndReleasesDespiteLongGraphPins) {
	materializeNativeDatabase();auto options=backendOptions();options.open_mode=OpenMode::OPEN_EXISTING_FILE;options.query_scratch_enabled=true;RelocatableStorageBackend backend(options);backend.open();
	const auto id=backend.createLogicalNodes({{{"ScratchPin"},{{"value",PropertyValue(7)}}}})[0];auto old=backend.openLogicalGraphReadSnapshot();
	auto *provider=dynamic_cast<LogicalQueryScratchProvider *>(old.get());ASSERT_NE(provider,nullptr);
	auto first=provider->openQueryScratchSession(2U*RSS_SEGMENT_SIZE,2U),second=provider->openQueryScratchSession(RSS_SEGMENT_SIZE,1U);
	ASSERT_NE(first,nullptr);ASSERT_NE(second,nullptr);const std::vector<std::byte> bytes(4096,std::byte{0x3A});
	const auto handle=first->write(bytes);EXPECT_EQ(first->read(handle),bytes);
	EXPECT_THROW(second->read(handle),std::invalid_argument);
	EXPECT_THROW(backend.close(),std::logic_error);
	first->release(handle);EXPECT_THROW(first->read(handle),std::invalid_argument);
	// An ordinary graph pin remains live while query-only slots become reusable.
	const auto again=first->write(bytes);EXPECT_EQ(first->read(again),bytes);first->finish();second->finish();
	EXPECT_THROW(first->write(bytes),std::logic_error);
	EXPECT_EQ(old->readNodeProperty(id,"value"),PropertyValue(7));old.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));backend.close();backend.open();backend.close();
}
TEST_F(RelocatableStorageBackendTest, QueryScratchEnforcesDiskFragmentsAndPhysicalReadOnlyPermissions) {
	materializeNativeDatabase();auto options=backendOptions();options.open_mode=OpenMode::OPEN_EXISTING_FILE;options.query_scratch_enabled=true;RelocatableStorageBackend backend(options);backend.open();
	const std::vector<std::byte> bytes(64,std::byte{0x11});auto scratch=backend.openNativeQueryScratchSession(RSS_SEGMENT_SIZE,1);(void)scratch->write(bytes);
	EXPECT_THROW(scratch->write(bytes),std::runtime_error);
	scratch->finish();backend.close();const auto size=std::filesystem::file_size(data_path_);options.open_mode=OpenMode::OPEN_READ_ONLY_EXISTING_FILE;RelocatableStorageBackend reader(options);reader.open();reader.resetIoStats();auto snapshot=reader.openLogicalGraphReadSnapshot();
	auto *provider=dynamic_cast<LogicalQueryScratchProvider *>(snapshot.get());ASSERT_NE(provider,nullptr);EXPECT_EQ(provider->openQueryScratchSession(RSS_SEGMENT_SIZE,1),nullptr);
	EXPECT_EQ(reader.ioStats().data.write_bytes,0U);snapshot.reset();reader.close();EXPECT_EQ(std::filesystem::file_size(data_path_),size);
}
#include "graph/query/api/QueryEngine.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/query/QueryGuard.hpp"
TEST_F(RelocatableStorageBackendTest, NativeLogicalReadonlySortSpillsAndPreservesCompleteDescendingCollection) {
	materializeNativeDatabase();auto options=backendOptions();options.open_mode=OpenMode::OPEN_EXISTING_FILE;options.query_scratch_enabled=true;auto backend=std::make_shared<RelocatableStorageBackend>(options);backend->open();
	graph::query::LogicalReadExecutionOptions reads;reads.bounds.maximum_materialized_bytes=8U*1024U*1024U;
	graph::query::QueryEngine engine(std::static_pointer_cast<LogicalGraphReadBackend>(backend),reads);const auto fence=backend->currentNativeLogicalGraphMutationFence();
	const bool wasEnabled=graph::debug::PerfTrace::isEnabled(),wasCollecting=graph::debug::PerfTrace::areValuesEnabled();
	struct RestoreTrace{bool enabled,collecting;~RestoreTrace(){graph::debug::PerfTrace::setValueCollectionEnabled(collecting);graph::debug::PerfTrace::setEnabled(enabled);}}restore{wasEnabled,wasCollecting};
	graph::debug::PerfTrace::setEnabled(true);graph::debug::PerfTrace::setValueCollectionEnabled(true);(void)graph::debug::PerfTrace::snapshotAndReset();
	graph::query::QueryGuard guard(120000,64);graph::query::QueryContext context;context.guard=&guard;
	const auto result=engine.execute("UNWIND range(1,20000) AS x WITH x ORDER BY x DESC RETURN collect(x) AS values",context);ASSERT_EQ(result.rowCount(),1U);
	const auto &values=std::get<std::vector<PropertyValue>>(result.getRows()[0].at("values").asPrimitive().getVariant());ASSERT_EQ(values.size(),20000U);
	for(size_t i=0;i<values.size();++i)EXPECT_EQ(values[i],PropertyValue(static_cast<int64_t>(20000U-i)));
	const auto trace=graph::debug::PerfTrace::snapshotAndReset();ASSERT_TRUE(trace.contains("logical_plan.sort.spilled"));
	EXPECT_EQ(trace.at("logical_plan.sort.spilled").totalValue,1);
	graph::debug::PerfTrace::setValueCollectionEnabled(wasCollecting);graph::debug::PerfTrace::setEnabled(wasEnabled);
	EXPECT_EQ(backend->currentNativeLogicalGraphMutationFence(),fence);
	EXPECT_NO_THROW(static_cast<void>(backend->verifyIntegrity()));backend->close();
}
TEST_F(RelocatableStorageBackendTest, QueryScratchInternalPolicyCanDenyPermissionWithoutWritingTheFile) {
	materializeNativeDatabase();auto options=backendOptions();options.open_mode=OpenMode::OPEN_EXISTING_FILE;options.query_scratch_enabled=true;options.query_scratch_enabled=false;
	RelocatableStorageBackend backend(options);backend.open();const auto before=backend.currentNativeMetadataGeneration();backend.resetIoStats();
	EXPECT_EQ(backend.openNativeQueryScratchSession(RSS_SEGMENT_SIZE,1),nullptr);
	EXPECT_EQ(backend.currentNativeMetadataGeneration(),before);
	EXPECT_EQ(backend.ioStats().data.write_bytes,0U);backend.close();
}

TEST_F(RelocatableStorageBackendTest, QueryScratchCleanupIoFailureImmediatelyFencesFurtherWrites) {
	for (const bool batch : {false, true}) {
		SCOPED_TRACE(batch);
		materializeNativeDatabase();auto options=backendOptions();options.open_mode=OpenMode::OPEN_EXISTING_FILE;options.query_scratch_enabled=true;
		RelocatableStorageBackend backend(options);backend.open();
		const auto node=backend.createLogicalNodes({{{"ScratchFailure"},{{"value",PropertyValue(7)}}}}).at(0);
		auto old=backend.openLogicalGraphReadSnapshot();auto scratch=backend.openNativeQueryScratchSession(RSS_SEGMENT_SIZE,1);
		const std::vector<std::byte> bytes(64,std::byte{0x11});const auto handle=scratch->write(bytes);
		bool injected=false;
		{
			StorageIO::ScopedNativeWriteHookForTesting hook([&](file_handle_t,const void *,size_t,pwrite_off_t){injected=true;return static_cast<pwrite_ssize_t>(-1);});
			if(batch)EXPECT_THROW(scratch->finish(),std::exception);
			else EXPECT_THROW(scratch->release(handle),std::exception);
		}
		ASSERT_TRUE(injected);EXPECT_TRUE(backend.hasIndeterminateWriteOutcome());
		EXPECT_THROW(scratch->write(bytes),std::logic_error);
		EXPECT_THROW(scratch->read(handle),std::logic_error);
		EXPECT_THROW(backend.replaceLogicalNodeProperties({{node,{{"value",PropertyValue(99)}}}}),std::logic_error);
		EXPECT_EQ(old->readNodeProperty(node,"value"),PropertyValue(7));
		scratch.reset();old.reset();backend.close();backend.open();
		auto recovered=backend.openLogicalGraphReadSnapshot();EXPECT_EQ(recovered->readNodeProperty(node,"value"),PropertyValue(7));recovered.reset();
		EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));backend.close();
		std::filesystem::remove(data_path_);
	}
}
