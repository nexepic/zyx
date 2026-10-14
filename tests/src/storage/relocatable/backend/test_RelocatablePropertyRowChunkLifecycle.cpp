#include <iostream>
#include "graph/query/api/QueryEngine.hpp"
#include "storage/relocatable/backend/RelocatableStorageBackendTestFixture.hpp"

using namespace relocatable_storage_backend_test;
namespace {
LargeValueReachabilityReclaimOptions rowReclaimOptions() {
	LargeValueReachabilityReclaimOptions options; options.maximum_manifest_locations = 32U;
	options.maximum_manifest_metadata_pages = 64U; options.maximum_manifest_nodes = 64U;
	options.maximum_owner_locations = 64U; options.maximum_owner_metadata_pages = 64U;
	options.maximum_data_pages = 256U; options.maximum_retirements = 256U; options.maximum_page_locations = 2048U;
	options.maximum_pools = 64U; options.first_index_slot = PhysicalSlotId(1U); options.last_index_slot = PhysicalSlotId(999U);
	options.first_journal_state_slot = PhysicalSlotId(1000U); options.last_journal_state_slot = PhysicalSlotId(1015U);
	options.journal_state_page_owner_id = 0x4C564D4AU; options.journal_state_id = 0x4C564D4AU; return options;
}
}

TEST_F(RelocatableStorageBackendTest, RowChunkPrototypePreservesOldPinsTypeChangesNullRemovalAndReopen) {
	materializeNativeDatabase(); auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	options.adaptive_property_row_chunks = true; RelocatableStorageBackend backend(options); backend.open();
	const std::string body(256U*1024U, 'x');
	const auto ids = backend.createLogicalNodes({{{"ChunkRow"}, {{"body",PropertyValue(body)},{"rank",PropertyValue(5)},{"nullable",PropertyValue()}}}});
	ASSERT_EQ(ids.size(), 1U); auto old = backend.openLogicalGraphReadSnapshot();
	EXPECT_EQ(old->readNodeProperty(ids[0], "rank"), PropertyValue(5));
	EXPECT_EQ(old->nodePropertyExists(ids[0], "nullable"), true); EXPECT_EQ(old->nodePropertyExists(ids[0], "missing"), false);
	backend.replaceLogicalNodeProperties({{ids[0],{{"body",PropertyValue(body)},{"rank",PropertyValue("changed")}}}});
	auto current = backend.openLogicalGraphReadSnapshot(); EXPECT_EQ(current->readNodeProperty(ids[0],"rank"),PropertyValue("changed"));
	EXPECT_EQ(current->readNodeProperty(ids[0],"body"),PropertyValue(body)); EXPECT_EQ(current->nodePropertyExists(ids[0],"nullable"),false);
	EXPECT_EQ(old->readNodeProperty(ids[0],"rank"),PropertyValue(5)); EXPECT_EQ(old->readNodeProperty(ids[0],"body"),PropertyValue(body));
	current.reset(); old.reset(); backend.close();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE; options.adaptive_property_row_chunks = false;
	RelocatableStorageBackend reopened(options); reopened.open(); auto snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readNodeProperty(ids[0],"rank"),PropertyValue("changed")); EXPECT_EQ(snapshot->readNodeProperty(ids[0],"body"),PropertyValue(body));
	snapshot.reset(); EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity())); reopened.close();
}

TEST_F(RelocatableStorageBackendTest, RowChunkPrototypeLargeToInlineDeleteAndReclamationKeepPinnedVersionsAlive) {
	materializeNativeDatabase(); auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE; options.adaptive_property_row_chunks = true;
	RelocatableStorageBackend backend(options); backend.open(); const std::string body(256U*1024U,'x');
	const auto ids = backend.createLogicalNodes({{{"ChunkRow"},{{"body",PropertyValue(body)},{"rank",PropertyValue(5)}}}});
	ASSERT_EQ(ids.size(),1U); auto old = backend.openLogicalGraphReadSnapshot();
	backend.replaceLogicalNodeProperties({{ids[0],{{"rank",PropertyValue(6)}}}});
	backend.deleteLogicalNodes({{ids[0],false,0U}});
	uint64_t retired = 0U;
	for (size_t pass=0U;pass<8U;++pass) retired += backend.reclaimUnreachableLargeValues(rowReclaimOptions()).retired_values;
	EXPECT_EQ(old->readNodeProperty(ids[0],"body"),PropertyValue(body)); EXPECT_EQ(old->readNodeProperty(ids[0],"rank"),PropertyValue(5));
	auto current = backend.openLogicalGraphReadSnapshot(); EXPECT_FALSE(current->containsNode(ids[0])); current.reset(); old.reset();
	for (size_t pass=0U;pass<16U;++pass) retired += backend.reclaimUnreachableLargeValues(rowReclaimOptions()).retired_values;
	EXPECT_GT(retired, 0U);
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity())); backend.close(); backend.open();
	current = backend.openLogicalGraphReadSnapshot(); EXPECT_FALSE(current->containsNode(ids[0])); current.reset(); backend.close();
}

TEST_F(RelocatableStorageBackendTest, RowChunkPrototypeMeasuresCompleteNativePageCostsAgainstLegacyRowPlacement) {
	struct Result { uint64_t point=0U,presence=0U,full=0U,file=0U; } results[2];
	const std::string body(256U*1024U,'x');
	for (size_t variant=0U;variant<2U;++variant) {
		const auto path = std::filesystem::path(data_path_.string()+"-"+std::to_string(variant));
		struct Remove { std::filesystem::path path; ~Remove(){std::error_code error;std::filesystem::remove(path,error);} } cleanup{path};
		auto options=backendOptions();options.data_path=path;options.open_mode=OpenMode::OPEN_EXISTING_FILE;
		options.buffer_pool_bytes=64U*1024U;options.adaptive_property_row_chunks=variant==1U;
		auto initialization = options; initialization.open_mode = OpenMode::OPEN_CREATE_NEW_FILE;
		{ auto initialized = RelocatableStorageBackend::initializeNew(initialization); initialized->open(); initialized->close(); }
		RelocatableStorageBackend backend(options);backend.open();
		const auto ids=backend.createLogicalNodes({{{"ChunkRow"},{{"body",PropertyValue(body)},{"rank",PropertyValue(5)}}}}); ASSERT_EQ(ids.size(),1U);
		for (size_t operation=0U;operation<3U;++operation) {
			backend.close();backend.open();backend.resetIoStats();auto pin=backend.openLogicalGraphReadSnapshot();
			if(operation==0U) EXPECT_EQ(pin->readNodeProperty(ids[0],"rank"),PropertyValue(5));
			if(operation==1U) EXPECT_EQ(pin->nodePropertyIsNonNull(ids[0],"body"),true);
			if(operation==2U) EXPECT_EQ(pin->readNode(ids[0]).properties.size(),2U);
			const auto io=backend.ioStats().data;EXPECT_EQ(io.write_bytes,0U);
			if(operation==0U)results[variant].point=io.read_bytes;
			if(operation==1U)results[variant].presence=io.read_bytes;
			if(operation==2U)results[variant].full=io.read_bytes;
		}
		results[variant].file=std::filesystem::file_size(path);EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));backend.close();
		std::cout<<"ROW_CHUNK_COST variant="<<variant<<" point="<<results[variant].point<<" presence="<<results[variant].presence<<" full="<<results[variant].full<<" file="<<results[variant].file<<'\n';
	}
	EXPECT_LT(results[1].point,results[0].point);EXPECT_LT(results[1].presence,results[0].presence);
	EXPECT_LE(results[1].file,results[0].file+results[0].file/20U);
}

TEST_F(RelocatableStorageBackendTest, DefaultNativeProductionRowChunksServeScanAggregateMutationAndOwnedOutput) {
	materializeNativeDatabase(); graph::Database database(data_path_.string());database.open();
	auto backend=std::dynamic_pointer_cast<RelocatableStorageBackend>(database.getStorageBackend());ASSERT_NE(backend,nullptr);
	EXPECT_TRUE(backend->options().adaptive_property_row_chunks);
	auto engine=database.getQueryEngine();graph::query::QueryContext context;
	context.parameters.emplace("body",PropertyValue(std::string(256U*1024U,'x')));
	engine->execute("CREATE (:ProductionChunk {body:$body,rank:7}),(:ProductionChunk {rank:8})",context);
	const auto summed=engine->execute("MATCH (n:ProductionChunk) RETURN sum(n.rank) AS total,count(n.body) AS bodies");
	ASSERT_EQ(summed.rowCount(),1U);EXPECT_EQ(summed.getRows().front().at("total").asPrimitive(),PropertyValue(15));
	EXPECT_EQ(summed.getRows().front().at("bodies").asPrimitive(),PropertyValue(1));
	const auto changed=engine->execute("MATCH (n:ProductionChunk {rank:7}) SET n.rank=9 RETURN n.body AS body,n.rank AS rank");
	ASSERT_EQ(changed.rowCount(),1U);EXPECT_EQ(changed.getRows().front().at("rank").asPrimitive(),PropertyValue(9));
	database.close();EXPECT_EQ(changed.getRows().front().at("body").asPrimitive(),context.parameters.at("body"));
	database.open();EXPECT_EQ(database.getQueryEngine()->execute("MATCH (n:ProductionChunk) RETURN sum(n.rank) AS total").getRows().front().at("total").asPrimitive(),PropertyValue(17));database.close();
}
