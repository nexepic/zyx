#include <atomic>
#include <cerrno>
#include "storage/relocatable/backend/RelocatableStorageBackendTestFixture.hpp"
#include "graph/storage/relocatable/format/SuperblockFile.hpp"
#ifdef _WIN32
#include <windows.h>
#endif
using namespace relocatable_storage_backend_test;
namespace {
enum class RowFault { RFLT_NO_SPACE, RFLT_TORN_DATA, RFLT_TORN_ROOT, RFLT_FINAL_SYNC };
void rowIoError(bool noSpace) {
#ifdef _WIN32
	SetLastError(noSpace?ERROR_DISK_FULL:ERROR_WRITE_FAULT);
#else
	errno=noSpace?ENOSPC:EIO;
#endif
}
}

TEST_F(RelocatableStorageBackendTest, RowChunkFaultMatrixRetainsOldPinsAndRecoversOnlyCompleteGraphs) {
	for(const auto fault:{RowFault::RFLT_NO_SPACE,RowFault::RFLT_TORN_DATA,RowFault::RFLT_TORN_ROOT,RowFault::RFLT_FINAL_SYNC}) {
		SCOPED_TRACE(static_cast<int>(fault));
		const auto path=std::filesystem::path(data_path_.string()+"-fault-"+std::to_string(static_cast<int>(fault)));
		struct Remove{std::filesystem::path path;~Remove(){std::error_code error;std::filesystem::remove(path,error);}}cleanup{path};
		auto options=backendOptions();options.data_path=path;options.open_mode=OpenMode::OPEN_CREATE_NEW_FILE;
		{auto initialization=RelocatableStorageBackend::initializeNew(options);initialization->open();initialization->close();}
		options.open_mode=OpenMode::OPEN_EXISTING_FILE;RelocatableStorageBackend backend(options);backend.open();
		const std::string body(256U*1024U,'x');const auto ids=backend.createLogicalNodes({{{"ChunkFault"},{{"body",PropertyValue(body)},{"rank",PropertyValue(5)}}}});
		ASSERT_EQ(ids.size(),1U);auto old=backend.openLogicalGraphReadSnapshot();const auto fence=*old->mutationFence();
		std::atomic<bool> injected{false},graphWritten{false};
		{
			StorageIO::ScopedNativeWriteHookForTesting writeHook([&](file_handle_t fd,const void *bytes,size_t count,pwrite_off_t offset){
				bool data=false,graph=false;
				if(count==RSS_PAGE_SIZE) {
					const auto page=std::span<const std::byte>(static_cast<const std::byte *>(bytes),count);
					try {data=PageOwnerHeaderCodec::decode(page.first(RSS_PAGE_OWNER_HEADER_BYTES)).kind==PageOwnerKind::POK_LARGE_VALUE_DATA;}catch(const std::exception &){}
					try {graph=RelocatableStorageBackend::makeLogicalGraphMutationFence(SuperblockCodec::decode(page))!=fence;}catch(const std::exception &){}
				}
				const bool target=(data&&(fault==RowFault::RFLT_NO_SPACE||fault==RowFault::RFLT_TORN_DATA))||(graph&&fault==RowFault::RFLT_TORN_ROOT);
				if(target&&!injected.exchange(true)) {
					if(fault!=RowFault::RFLT_NO_SPACE) {const auto wrote=portable_pwrite(fd,bytes,64U,offset);EXPECT_EQ(wrote,64);}
					rowIoError(fault==RowFault::RFLT_NO_SPACE);return static_cast<pwrite_ssize_t>(-1);
				}
				const auto wrote=portable_pwrite(fd,bytes,count,offset);
				if(graph&&wrote==static_cast<pwrite_ssize_t>(count))graphWritten=true;
				return wrote;
			});
			StorageIO::ScopedNativeSyncHookForTesting syncHook([&](file_handle_t fd){
				if(fault==RowFault::RFLT_FINAL_SYNC&&graphWritten&&!injected.exchange(true)){rowIoError(false);return -1;}
				return portable_fsync(fd);
			});
			EXPECT_THROW(backend.replaceLogicalNodeProperties({{ids[0],{{"body",PropertyValue(body)},{"rank",PropertyValue(6)}}}}),std::exception);
		}
		ASSERT_TRUE(injected);EXPECT_EQ(old->readNodeProperty(ids[0],"rank"),PropertyValue(5));EXPECT_EQ(old->readNodeProperty(ids[0],"body"),PropertyValue(body));
		if(fault==RowFault::RFLT_FINAL_SYNC){EXPECT_TRUE(backend.hasIndeterminateWriteOutcome());EXPECT_THROW(backend.replaceLogicalNodeProperties({{ids[0],{{"rank",PropertyValue(99)}}}}),std::logic_error);}
		old.reset();backend.close();backend.open();auto recovered=backend.openLogicalGraphReadSnapshot();
		const auto rank=recovered->readNodeProperty(ids[0],"rank");
		if(fault==RowFault::RFLT_FINAL_SYNC)EXPECT_TRUE(rank==PropertyValue(5)||rank==PropertyValue(6));
		else EXPECT_EQ(rank,PropertyValue(5));
		EXPECT_EQ(recovered->readNodeProperty(ids[0],"body"),PropertyValue(body));recovered.reset();
		EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
		backend.replaceLogicalNodeProperties({{ids[0],{{"body",PropertyValue(body)},{"rank",PropertyValue(7)}}}});
		recovered=backend.openLogicalGraphReadSnapshot();EXPECT_EQ(recovered->readNodeProperty(ids[0],"rank"),PropertyValue(7));recovered.reset();backend.close();
	}
}

TEST_F(RelocatableStorageBackendTest, PhysicalReadOnlyRowChunkReadersPreserveValuesAndNeverWriteTheFile) {
	materializeNativeDatabase();auto options=backendOptions();options.open_mode=OpenMode::OPEN_EXISTING_FILE;
	uint64_t nodeId=0U;const std::string body(256U*1024U,'x');
	{RelocatableStorageBackend writer(options);writer.open();nodeId=writer.createLogicalNodes({{{"ReadonlyChunks"},{{"body",PropertyValue(body)},{"rank",PropertyValue(7)}}}}).at(0U);writer.close();}
	const auto before=std::filesystem::file_size(data_path_);options.open_mode=OpenMode::OPEN_READ_ONLY_EXISTING_FILE;
	RelocatableStorageBackend reader(options);reader.open();reader.resetIoStats();auto snapshot=reader.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readNodeProperty(nodeId,"rank"),PropertyValue(7));EXPECT_EQ(snapshot->readNodeProperty(nodeId,"body"),PropertyValue(body));
	EXPECT_EQ(snapshot->nodePropertyIsNonNull(nodeId,"body"),true);
	EXPECT_THROW(reader.replaceLogicalNodeProperties({{nodeId,{{"rank",PropertyValue(99)}}}}),std::exception);
	EXPECT_EQ(reader.ioStats().data.write_bytes,0U);snapshot.reset();reader.close();EXPECT_EQ(std::filesystem::file_size(data_path_),before);
}
