#include "storage/relocatable/backend/RelocatableStorageBackendTestFixture.hpp"
#include "graph/storage/LogicalQueryScratch.hpp"
using namespace relocatable_storage_backend_test;
TEST_F(RelocatableStorageBackendTest, QueryScratchOwnsPrivateHandlesAndReleasesDespiteLongGraphPins) {
	materializeNativeDatabase();auto options=backendOptions();options.open_mode=OpenMode::OPEN_EXISTING_FILE;options.query_scratch_enabled=true;RelocatableStorageBackend backend(options);backend.open();
	const auto id=backend.createLogicalNodes({{{"ScratchPin"},{{"value",PropertyValue(7)}}}})[0];auto old=backend.openLogicalGraphReadSnapshot();
	auto *provider=dynamic_cast<LogicalQueryScratchProvider *>(old.get());
	ASSERT_NE(provider,nullptr);
	auto first=provider->openQueryScratchSession(2U*RSS_SEGMENT_SIZE,2U),second=provider->openQueryScratchSession(RSS_SEGMENT_SIZE,1U);
	ASSERT_NE(first,nullptr);
	ASSERT_NE(second,nullptr);const std::vector<std::byte> bytes(4096,std::byte{0x3A});
	const auto handle=first->write(bytes);
	EXPECT_EQ(first->read(handle),bytes);
	EXPECT_THROW(second->read(handle),std::invalid_argument);
	EXPECT_THROW(backend.close(),std::logic_error);
	first->release(handle);
	EXPECT_THROW(first->read(handle),std::invalid_argument);
	// An ordinary graph pin remains live while query-only slots become reusable.
	const auto again=first->write(bytes);
	EXPECT_EQ(first->read(again),bytes);first->finish();second->finish();
	EXPECT_THROW(first->write(bytes),std::logic_error);
	EXPECT_EQ(old->readNodeProperty(id,"value"),PropertyValue(7));old.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));backend.close();backend.open();backend.close();
}
TEST_F(RelocatableStorageBackendTest, QueryScratchEnforcesDiskFragmentsAndPhysicalReadOnlyPermissions) {
	materializeNativeDatabase();auto options=backendOptions();options.open_mode=OpenMode::OPEN_EXISTING_FILE;options.query_scratch_enabled=true;RelocatableStorageBackend backend(options);backend.open();
	const std::vector<std::byte> bytes(64,std::byte{0x11});auto scratch=backend.openNativeQueryScratchSession(RSS_SEGMENT_SIZE,1);(void)scratch->write(bytes);
	EXPECT_THROW(scratch->write(bytes),std::runtime_error);
	scratch->finish();backend.close();const auto size=std::filesystem::file_size(data_path_);options.open_mode=OpenMode::OPEN_READ_ONLY_EXISTING_FILE;RelocatableStorageBackend reader(options);reader.open();reader.resetIoStats();auto snapshot=reader.openLogicalGraphReadSnapshot();
	auto *provider=dynamic_cast<LogicalQueryScratchProvider *>(snapshot.get());
	ASSERT_NE(provider,nullptr);
	EXPECT_EQ(provider->openQueryScratchSession(RSS_SEGMENT_SIZE,1),nullptr);
	EXPECT_EQ(reader.ioStats().data.write_bytes,0U);snapshot.reset();reader.close();
	EXPECT_EQ(std::filesystem::file_size(data_path_),size);
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
	const auto result=engine.execute("UNWIND range(1,20000) AS x WITH x ORDER BY x DESC RETURN collect(x) AS values",context);
	ASSERT_EQ(result.rowCount(),1U);
	const auto &values=std::get<std::vector<PropertyValue>>(result.getRows()[0].at("values").asPrimitive().getVariant());
	ASSERT_EQ(values.size(),20000U);
	for(size_t i=0;i<values.size();++i)EXPECT_EQ(values[i],PropertyValue(static_cast<int64_t>(20000U-i)));
	const auto trace=graph::debug::PerfTrace::snapshotAndReset();
	ASSERT_TRUE(trace.contains("logical_plan.sort.spilled"));
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
		ASSERT_TRUE(injected);
	EXPECT_TRUE(backend.hasIndeterminateWriteOutcome());
		EXPECT_THROW(scratch->write(bytes),std::logic_error);
		EXPECT_THROW(scratch->read(handle),std::logic_error);
		EXPECT_THROW(backend.replaceLogicalNodeProperties({{node,{{"value",PropertyValue(99)}}}}),std::logic_error);
		EXPECT_EQ(old->readNodeProperty(node,"value"),PropertyValue(7));
		scratch.reset();old.reset();backend.close();backend.open();
		auto recovered=backend.openLogicalGraphReadSnapshot();
	EXPECT_EQ(recovered->readNodeProperty(node,"value"),PropertyValue(7));recovered.reset();
		EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));backend.close();
		std::filesystem::remove(data_path_);
	}
}

#include "storage/relocatable/backend/QueryScratchDiskBudget.hpp"
#include <barrier>
#include <thread>
#include <atomic>
TEST(QueryScratchDiskBudgetTest, ThreeLevelsRefundOnlyReclaimedExtentsAndRetainOrphans) {
	using graph::storage::detail::QueryScratchDiskBudget;
	auto process=std::make_shared<QueryScratchDiskBudget>(100);
	{
		auto first=process->open("a",80,60),second=process->open("a",80,60),other=process->open("b",80,60);
		first.acquire(60);
	EXPECT_THROW(first.acquire(1),std::runtime_error);
		second.acquire(20);
	EXPECT_THROW(second.acquire(1),std::runtime_error);
		other.acquire(20);
	EXPECT_THROW(other.acquire(1),std::runtime_error);
		EXPECT_EQ(process->usage().bytes,100U);
		first.release(10);other.acquire(10);
	EXPECT_EQ(process->usage().bytes,100U);
		EXPECT_THROW(process->recovered("a"),std::logic_error);
		second.release(20);second.close();other.release(30);other.close();
		EXPECT_EQ(process->usage().bytes,50U);
	}
	EXPECT_EQ(process->usage().orphan_bytes,50U);
	EXPECT_EQ(process->usage().sessions,0U);
	auto denied=process->open("b",80,60);
	EXPECT_THROW(denied.acquire(51),std::runtime_error);denied.close();
	process->recovered("a");
	EXPECT_EQ(process->usage().bytes,0U);
	EXPECT_EQ(process->usage().domains,0U);
}
TEST(QueryScratchDiskBudgetTest, ConcurrentDomainsCannotOverAdmitProcessQuota) {
	using graph::storage::detail::QueryScratchDiskBudget;
	auto process=std::make_shared<QueryScratchDiskBudget>(100);std::barrier ready(3);std::atomic<unsigned> admitted{0};
	auto worker=[&](const char *name){auto session=process->open(name,80,60);ready.arrive_and_wait();try{session.acquire(60);++admitted;}catch(const std::runtime_error &){};};
	std::thread first(worker,"first"),second(worker,"second");ready.arrive_and_wait();first.join();second.join();
	EXPECT_EQ(admitted,1U);
	EXPECT_EQ(process->usage().bytes,60U);
	EXPECT_EQ(process->usage().orphan_bytes,60U);
	process->recovered("first");process->recovered("second");
	EXPECT_EQ(process->usage().bytes,0U);
}
TEST(QueryScratchDiskBudgetTest, SlotsMovesAndInvalidConfigurationPreserveOwnership) {
	using graph::storage::detail::QueryScratchDiskBudget;
	auto process=std::make_shared<QueryScratchDiskBudget>(100,1);
	auto first=process->open("a",80,60);
	EXPECT_THROW(process->open("a",80,60),std::runtime_error);
	EXPECT_THROW(process->open("a",70,60),std::invalid_argument);
	first.acquire(40);auto moved=std::move(first);
	EXPECT_THROW(first.acquire(1),std::logic_error);
	moved.release(40);moved.close();
	EXPECT_EQ(process->usage().sessions,0U);
	EXPECT_THROW(process->open("a",101,60),std::invalid_argument);
	std::vector<QueryScratchDiskBudget::Session> domains;
	for(size_t i=0;i<64;++i)domains.push_back(process->open(std::to_string(i),80,60));
	EXPECT_THROW(process->open("overflow",80,60),std::runtime_error);
	domains.clear();
	EXPECT_EQ(process->usage().domains,0U);
}
TEST_F(RelocatableStorageBackendTest, QueryScratchDatabaseQuotaIsSharedAcrossSessionsAndRefundedAfterCleanup) {
	materializeNativeDatabase();auto options=backendOptions();options.open_mode=OpenMode::OPEN_EXISTING_FILE;options.query_scratch_enabled=true;
	options.maximum_database_query_scratch_bytes=RSS_SEGMENT_SIZE;
	RelocatableStorageBackend backend(options);backend.open();
	auto first=backend.openNativeQueryScratchSession(RSS_SEGMENT_SIZE,1),second=backend.openNativeQueryScratchSession(RSS_SEGMENT_SIZE,1);
	const std::vector<std::byte> bytes(64,std::byte{0x11});const auto handle=first->write(bytes);
	backend.resetIoStats();
	EXPECT_THROW(second->write(bytes),std::runtime_error);
	EXPECT_EQ(backend.ioStats().data.write_bytes,0U);
	EXPECT_FALSE(backend.hasIndeterminateWriteOutcome());
	first->release(handle);const auto admitted=second->write(bytes);
	EXPECT_EQ(second->read(admitted),bytes);
	first->finish();second->finish();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));backend.close();backend.open();backend.close();
}

#include "graph/storage/relocatable/format/SuperblockFile.hpp"
#include <cerrno>
#ifdef _WIN32
#include <windows.h>
#endif
namespace {
enum class ScratchFault { QSFT_NO_SPACE, QSFT_TORN_DATA, QSFT_TORN_ROOT, QSFT_FINAL_SYNC };
void scratchIoError(bool noSpace) {
#ifdef _WIN32
	SetLastError(noSpace?ERROR_DISK_FULL:ERROR_WRITE_FAULT);
#else
	errno=noSpace?ENOSPC:EIO;
#endif
}
}
TEST_F(RelocatableStorageBackendTest, QueryScratchPublicationFaultsPreserveGraphAndRecoverOrphanQuota) {
	for(const auto fault:{ScratchFault::QSFT_NO_SPACE,ScratchFault::QSFT_TORN_DATA,ScratchFault::QSFT_TORN_ROOT,ScratchFault::QSFT_FINAL_SYNC}) {
		SCOPED_TRACE(static_cast<int>(fault));materializeNativeDatabase();auto options=backendOptions();options.open_mode=OpenMode::OPEN_EXISTING_FILE;options.query_scratch_enabled=true;
		RelocatableStorageBackend backend(options);backend.open();const auto node=backend.createLogicalNodes({{{"ScratchFault"},{{"value",PropertyValue(7)}}}}).at(0);
		auto old=backend.openLogicalGraphReadSnapshot();const auto fence=backend.currentNativeLogicalGraphMutationFence();
		auto scratch=backend.openNativeQueryScratchSession(RSS_SEGMENT_SIZE,1);const std::vector<std::byte> bytes(4096,std::byte{0x11});
		bool injected=false,rootWritten=false,dataWritten=false;
		{
			StorageIO::ScopedNativeWriteHookForTesting writeHook([&](file_handle_t fd,const void *buffer,size_t count,pwrite_off_t offset){
				bool root=false;
				if(count==RSS_PAGE_SIZE)try{static_cast<void>(SuperblockCodec::decode(std::span(static_cast<const std::byte *>(buffer),count)));root=true;}catch(const std::exception &){}
				bool data=false;
				if(count>=RSS_ENVELOPE_BYTES)try{data=SegmentEnvelopeCodec::decode(std::span(static_cast<const std::byte *>(buffer),count).first(RSS_ENVELOPE_BYTES)).kind==SegmentKind::SGK_QUERY_SCRATCH;}catch(const std::exception &){}
				const bool target=(data&&(fault==ScratchFault::QSFT_NO_SPACE||fault==ScratchFault::QSFT_TORN_DATA))||(root&&dataWritten&&fault==ScratchFault::QSFT_TORN_ROOT);
				if(target&&!injected){injected=true;if(fault!=ScratchFault::QSFT_NO_SPACE)EXPECT_EQ(portable_pwrite(fd,buffer,64,offset),64);scratchIoError(fault==ScratchFault::QSFT_NO_SPACE);return static_cast<pwrite_ssize_t>(-1);}
				const auto written=portable_pwrite(fd,buffer,count,offset);if(data&&written==static_cast<pwrite_ssize_t>(count))dataWritten=true;
				if(root&&dataWritten&&written==static_cast<pwrite_ssize_t>(count))rootWritten=true;return written;
			});
			StorageIO::ScopedNativeSyncHookForTesting syncHook([&](file_handle_t fd){if(fault==ScratchFault::QSFT_FINAL_SYNC&&rootWritten&&!injected){injected=true;scratchIoError(false);return -1;}return portable_fsync(fd);});
			EXPECT_THROW(scratch->write(bytes),std::exception);
		}
		ASSERT_TRUE(injected);
	EXPECT_TRUE(backend.hasIndeterminateWriteOutcome());
		EXPECT_EQ(old->readNodeProperty(node,"value"),PropertyValue(7));
	EXPECT_EQ(backend.currentNativeLogicalGraphMutationFence(),fence);
		scratch.reset();old.reset();backend.close();backend.open();
		EXPECT_EQ(backend.currentNativeLogicalGraphMutationFence(),fence);
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
		auto recovered=backend.openLogicalGraphReadSnapshot();
	EXPECT_EQ(recovered->readNodeProperty(node,"value"),PropertyValue(7));recovered.reset();
		auto admitted=backend.openNativeQueryScratchSession(RSS_SEGMENT_SIZE,1);
	EXPECT_NO_THROW(static_cast<void>(admitted->write(bytes)));admitted->finish();backend.close();std::filesystem::remove(data_path_);
	}
}
TEST_F(RelocatableStorageBackendTest, PhysicalReadonlyOpeningCannotReclaimLiveScratchOrDeclareItHealthy) {
	materializeNativeDatabase();auto options=backendOptions();options.open_mode=OpenMode::OPEN_EXISTING_FILE;options.query_scratch_enabled=true;
	RelocatableStorageBackend writer(options);writer.open();auto scratch=writer.openNativeQueryScratchSession(RSS_SEGMENT_SIZE,1);
	const std::vector<std::byte> bytes(4096,std::byte{0x3A});const auto handle=scratch->write(bytes);
	options.open_mode=OpenMode::OPEN_READ_ONLY_EXISTING_FILE;RelocatableStorageBackend reader(options);reader.open();reader.resetIoStats();
	EXPECT_THROW(static_cast<void>(reader.verifyIntegrity()),FormatException);
	EXPECT_EQ(reader.ioStats().data.write_bytes,0U);reader.close();
	EXPECT_EQ(scratch->read(handle),bytes);
	scratch->finish();
	EXPECT_NO_THROW(static_cast<void>(writer.verifyIntegrity()));writer.close();
}

TEST_F(RelocatableStorageBackendTest, QueryScratchAllocatorPublicationInterleavesWithGraphWritesWithoutLosingRoots) {
	materializeNativeDatabase();auto options=backendOptions();options.open_mode=OpenMode::OPEN_EXISTING_FILE;options.query_scratch_enabled=true;
	RelocatableStorageBackend backend(options);backend.open();const auto node=backend.createLogicalNodes({{{"ConcurrentScratch"},{{"value",PropertyValue(0)}}}}).at(0);
	auto old=backend.openLogicalGraphReadSnapshot();auto scratch=backend.openNativeQueryScratchSession(2U*RSS_SEGMENT_SIZE,2);
	const std::vector<std::byte> bytes(4096,std::byte{0x3A});std::barrier ready(3);std::array<std::exception_ptr,2> errors{};
	std::thread scratchWriter([&]{ready.arrive_and_wait();try{for(size_t i=0;i<10;++i){const auto handle=scratch->write(bytes);if(scratch->read(handle)!=bytes)throw std::runtime_error("scratch content changed");scratch->release(handle);}}catch(...){errors[0]=std::current_exception();}});
	std::thread graphWriter([&]{ready.arrive_and_wait();try{for(int64_t i=1;i<=10;++i)backend.replaceLogicalNodeProperties({{node,{{"value",PropertyValue(i)}}}});}catch(...){errors[1]=std::current_exception();}});
	ready.arrive_and_wait();scratchWriter.join();graphWriter.join();
	ASSERT_EQ(errors[0],nullptr);
	ASSERT_EQ(errors[1],nullptr);
	scratch->finish();
	EXPECT_EQ(old->readNodeProperty(node,"value"),PropertyValue(0));old.reset();
	auto current=backend.openLogicalGraphReadSnapshot();
	EXPECT_EQ(current->readNodeProperty(node,"value"),PropertyValue(10));current.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));backend.close();backend.open();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));backend.close();
}
TEST_F(RelocatableStorageBackendTest, CorruptScratchReadAndOrphanRecoveryNeverReclaimUserPagesOrRefundQuota) {
	materializeNativeDatabase();auto options=backendOptions();options.open_mode=OpenMode::OPEN_EXISTING_FILE;options.query_scratch_enabled=true;
	RelocatableStorageBackend backend(options);backend.open();const auto node=backend.createLogicalNodes({{{"ScratchCorruption"},{{"value",PropertyValue(7)}}}}).at(0);
	auto old=backend.openLogicalGraphReadSnapshot();auto scratch=backend.openNativeQueryScratchSession(RSS_SEGMENT_SIZE,1);
	const std::vector<std::byte> bytes(4096,std::byte{0x3A});std::vector<std::byte> original;pwrite_off_t corruptedOffset=0;uint64_t handle=0;
	{
		StorageIO::ScopedNativeWriteHookForTesting hook([&](file_handle_t fd,const void *buffer,size_t count,pwrite_off_t offset){
			bool query=false;if(count>=RSS_ENVELOPE_BYTES)try{query=SegmentEnvelopeCodec::decode(std::span(static_cast<const std::byte *>(buffer),count).first(RSS_ENVELOPE_BYTES)).kind==SegmentKind::SGK_QUERY_SCRATCH;}catch(const std::exception &){}
			if(query&&original.empty()){
				original.assign(static_cast<const std::byte *>(buffer),static_cast<const std::byte *>(buffer)+count);corruptedOffset=offset;
				auto corrupt=original;corrupt.at(RSS_ENVELOPE_BYTES)^=std::byte{1};return portable_pwrite(fd,corrupt.data(),corrupt.size(),offset);
			}
			return portable_pwrite(fd,buffer,count,offset);
		});
		handle=scratch->write(bytes);
	}
	ASSERT_FALSE(original.empty());
	EXPECT_THROW(scratch->read(handle),FormatException);
	EXPECT_THROW(scratch->release(handle),FormatException);
	EXPECT_TRUE(backend.hasIndeterminateWriteOutcome());
	EXPECT_EQ(old->readNodeProperty(node,"value"),PropertyValue(7));scratch.reset();old.reset();backend.close();
	auto budget=graph::storage::detail::QueryScratchDiskBudget::processBudget();const auto quarantined=budget->usage().orphan_bytes;
	EXPECT_GE(quarantined,RSS_SEGMENT_SIZE);
	{
		options.open_mode=OpenMode::OPEN_READ_ONLY_EXISTING_FILE;RelocatableStorageBackend reader(options);reader.open();reader.resetIoStats();
		auto snapshot=reader.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readNodeProperty(node,"value"),PropertyValue(7));snapshot.reset();
		EXPECT_THROW(static_cast<void>(reader.verifyIntegrity()),FormatException);
	EXPECT_EQ(reader.ioStats().data.write_bytes,0U);reader.close();
	}
	size_t unsafeWrites=0;
	{
		StorageIO::ScopedNativeWriteHookForTesting hook([&](file_handle_t fd,const void *buffer,size_t count,pwrite_off_t offset){
			// Metadata startup may reconcile its authenticated private control bitmap.
			// It must never write data frames, graph roots or scratch-disposal state.
			bool control=false;
			if(count==RSS_METADATA_PAGE_POOL_HEADER_BYTES){
				const auto relative=static_cast<size_t>(offset)%RSS_PAGE_SIZE;
				if(relative==RSS_ENVELOPE_BYTES || relative==RSS_ENVELOPE_BYTES+RSS_METADATA_PAGE_POOL_HEADER_BYTES){
					std::array<std::byte,RSS_ENVELOPE_BYTES> envelope{};
					if(portable_pread(fd,envelope.data(),envelope.size(),offset-static_cast<pwrite_off_t>(relative))==static_cast<pwrite_ssize_t>(envelope.size())){
						try{const auto entry=SegmentEnvelopeCodec::decode(envelope);const auto header=MetadataPagePoolHeaderCodec::decode(std::span(static_cast<const std::byte *>(buffer),count));
							control=entry.kind==SegmentKind::SGK_METADATA_PAGE_POOL && header.segment_id==entry.segment_id && header.generation==entry.generation;
						}catch(const std::exception &){}
					}
				}
			}
			if(!control)++unsafeWrites;return portable_pwrite(fd,buffer,count,offset);
		});
		EXPECT_THROW(backend.open(),FormatException);
	}
	EXPECT_EQ(unsafeWrites,0U);
	EXPECT_EQ(budget->usage().orphan_bytes,quarantined);
	// Repair only the test-owned corrupt page, then require authenticated recovery.
	const auto fd=portable_open_rw(data_path_.string().c_str());
	ASSERT_NE(fd,INVALID_FILE_HANDLE);
	EXPECT_EQ(portable_pwrite(fd,original.data(),original.size(),corruptedOffset),static_cast<pwrite_ssize_t>(original.size()));
	EXPECT_EQ(portable_fsync(fd),0);portable_close(fd);
	backend.open();
	EXPECT_EQ(budget->usage().orphan_bytes,quarantined-RSS_SEGMENT_SIZE);
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));backend.close();
}
