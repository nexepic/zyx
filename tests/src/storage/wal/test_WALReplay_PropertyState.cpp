/**
 * @file test_WALReplay_PropertyState.cpp
 * @author ZYX Contributors
 * @date 2026
 *
 * @copyright Copyright (c) 2026
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 **/

#include "storage/wal/WALReplayTestFixture.hpp"

// ============================================================================
// Property & State Recovery Tests
// ============================================================================

TEST_F(WALReplayTest, PropertyRecoverySurvivesReopen) {
	int64_t nodeId = 0;

	{
		Database db(dbPath.string());
		db.open();
		auto dm = db.getStorage()->getDataManager();

		auto txn = db.beginTransaction();
		Node n(0, 1);
		dm->addNode(n);
		nodeId = n.getId();
		dm->addNodeProperties(nodeId, {{"name", PropertyValue(std::string("Alice"))},
										{"age", PropertyValue(static_cast<int64_t>(30))}});
		txn.commit();
		db.close();
	}

	// Reopen — trigger WAL recovery via beginReadOnlyTransaction, then verify
	{
		Database db2(dbPath.string());
		db2.open();

		// Trigger WAL recovery
		{
			auto txn = db2.beginReadOnlyTransaction();
			txn.commit();
		}

		auto dm = db2.getStorage()->getDataManager();

		Node retrieved = dm->getNode(nodeId);
		EXPECT_TRUE(retrieved.isActive()) << "Node with properties should be recovered";

		auto props = dm->getNodeProperties(nodeId);
		EXPECT_FALSE(props.empty()) << "Properties should be recovered via WAL replay";

		auto nameIt = props.find("name");
		EXPECT_NE(nameIt, props.end()) << "Property 'name' should exist";
		if (nameIt != props.end()) {
			EXPECT_EQ(std::get<std::string>(nameIt->second.getVariant()), "Alice");
		}

		auto ageIt = props.find("age");
		EXPECT_NE(ageIt, props.end()) << "Property 'age' should exist";
		if (ageIt != props.end()) {
			EXPECT_EQ(std::get<int64_t>(ageIt->second.getVariant()), 30);
		}

		db2.close();
	}
}

TEST_F(WALReplayTest, PropertyDeletionRecoverySurvivesReopen) {
	int64_t nodeId = 0;

	{
		Database db(dbPath.string());
		db.open();
		auto dm = db.getStorage()->getDataManager();

		// Create node with properties
		{
			auto txn = db.beginTransaction();
			Node n(0, 1);
			dm->addNode(n);
			nodeId = n.getId();
			dm->addNodeProperties(nodeId, {{"keep", PropertyValue(std::string("stay"))},
											{"remove", PropertyValue(std::string("gone"))}});
			txn.commit();
		}

		// Remove one property
		{
			auto txn = db.beginTransaction();
			dm->removeNodeProperty(nodeId, "remove");
			txn.commit();
		}

		db.close();
	}

	// Reopen with WAL recovery
	{
		Database db2(dbPath.string());
		db2.open();

		// Trigger WAL recovery
		{
			auto txn = db2.beginReadOnlyTransaction();
			txn.commit();
		}

		auto dm = db2.getStorage()->getDataManager();

		Node retrieved = dm->getNode(nodeId);
		EXPECT_TRUE(retrieved.isActive());

		auto props = dm->getNodeProperties(nodeId);
		EXPECT_NE(props.find("keep"), props.end()) << "Kept property should survive";
		EXPECT_EQ(props.find("remove"), props.end()) << "Removed property should not survive";

		db2.close();
	}
}

TEST_F(WALReplayTest, MultiEntityTypesSingleTransactionRecovery) {
	int64_t nodeId1 = 0, nodeId2 = 0, edgeId = 0;

	{
		Database db(dbPath.string());
		db.open();
		auto dm = db.getStorage()->getDataManager();

		auto txn = db.beginTransaction();

		Node n1(0, 10);
		dm->addNode(n1);
		nodeId1 = n1.getId();

		Node n2(0, 20);
		dm->addNode(n2);
		nodeId2 = n2.getId();

		Edge e(0, nodeId1, nodeId2, 5);
		dm->addEdge(e);
		edgeId = e.getId();

		dm->addNodeProperties(nodeId1, {{"key", PropertyValue(std::string("value"))}});
		dm->addEdgeProperties(edgeId, {{"weight", PropertyValue(3.14)}});

		txn.commit();
		db.close();
	}

	// Reopen with WAL recovery — all entity types should be recovered
	{
		Database db2(dbPath.string());
		db2.open();

		// Trigger WAL recovery
		{
			auto txn = db2.beginReadOnlyTransaction();
			txn.commit();
		}

		auto dm = db2.getStorage()->getDataManager();

		Node r1 = dm->getNode(nodeId1);
		EXPECT_TRUE(r1.isActive()) << "Node 1 should be recovered";
		EXPECT_EQ(r1.getLabelId(), 10);

		Node r2 = dm->getNode(nodeId2);
		EXPECT_TRUE(r2.isActive()) << "Node 2 should be recovered";
		EXPECT_EQ(r2.getLabelId(), 20);

		Edge re = dm->getEdge(edgeId);
		EXPECT_TRUE(re.isActive()) << "Edge should be recovered";
		EXPECT_EQ(re.getSourceNodeId(), nodeId1);
		EXPECT_EQ(re.getTargetNodeId(), nodeId2);

		auto nodeProps = dm->getNodeProperties(nodeId1);
		EXPECT_NE(nodeProps.find("key"), nodeProps.end()) << "Node property should be recovered";

		auto edgeProps = dm->getEdgeProperties(edgeId);
		EXPECT_NE(edgeProps.find("weight"), edgeProps.end()) << "Edge property should be recovered";

		db2.close();
	}
}

TEST_F(WALReplayTest, BlobEntityRecoverySurvivesReopen) {
	int64_t blobId = 0;

	{
		Database db(dbPath.string());
		db.open();
		auto dm = db.getStorage()->getDataManager();

		auto txn = db.beginTransaction();
		Blob blob(0, "test blob data for WAL recovery");
		dm->addBlobEntity(blob);
		blobId = blob.getId();
		ASSERT_GT(blobId, 0);
		txn.commit();
		db.close();
	}

	{
		Database db2(dbPath.string());
		db2.open();
		auto dm = db2.getStorage()->getDataManager();

		Blob retrieved = dm->getBlob(blobId);
		EXPECT_TRUE(retrieved.isActive()) << "Committed blob should be visible after WAL replay";

		db2.close();
	}
}

TEST_F(WALReplayTest, BlobEntityDeleteRecoverySurvivesReopen) {
	int64_t blobId = 0;

	{
		Database db(dbPath.string());
		db.open();
		auto dm = db.getStorage()->getDataManager();

		{
			auto txn = db.beginTransaction();
			Blob blob(0, "blob to be deleted");
			dm->addBlobEntity(blob);
			blobId = blob.getId();
			txn.commit();
		}

		{
			auto txn = db.beginTransaction();
			Blob toDelete = dm->getBlob(blobId);
			dm->deleteBlob(toDelete);
			txn.commit();
		}

		db.close();
	}

	{
		Database db2(dbPath.string());
		db2.open();
		auto dm = db2.getStorage()->getDataManager();

		Blob retrieved = dm->getBlob(blobId);
		EXPECT_FALSE(retrieved.isActive()) << "Deleted blob should be inactive after WAL replay";

		db2.close();
	}
}

TEST_F(WALReplayTest, IndexEntityRecoverySurvivesReopen) {
	int64_t indexId = 0;

	{
		Database db(dbPath.string());
		db.open();
		auto dm = db.getStorage()->getDataManager();

		auto txn = db.beginTransaction();
		Index idx(0, Index::NodeType::LEAF, 1);
		dm->addIndexEntity(idx);
		indexId = idx.getId();
		ASSERT_GT(indexId, 0);
		txn.commit();
		db.close();
	}

	{
		Database db2(dbPath.string());
		db2.open();
		auto dm = db2.getStorage()->getDataManager();

		Index retrieved = dm->getIndex(indexId);
		EXPECT_TRUE(retrieved.isActive()) << "Committed index should be visible after WAL replay";

		db2.close();
	}
}

TEST_F(WALReplayTest, IndexEntityDeleteRecoverySurvivesReopen) {
	int64_t indexId = 0;

	{
		Database db(dbPath.string());
		db.open();
		auto dm = db.getStorage()->getDataManager();

		{
			auto txn = db.beginTransaction();
			Index idx(0, Index::NodeType::LEAF, 2);
			dm->addIndexEntity(idx);
			indexId = idx.getId();
			txn.commit();
		}

		{
			auto txn = db.beginTransaction();
			Index toDelete = dm->getIndex(indexId);
			dm->deleteIndex(toDelete);
			txn.commit();
		}

		db.close();
	}

	{
		Database db2(dbPath.string());
		db2.open();
		auto dm = db2.getStorage()->getDataManager();

		Index retrieved = dm->getIndex(indexId);
		EXPECT_FALSE(retrieved.isActive()) << "Deleted index should be inactive after WAL replay";

		db2.close();
	}
}

TEST_F(WALReplayTest, StateEntityRecoverySurvivesReopen) {
	int64_t stateId = 0;

	{
		Database db(dbPath.string());
		db.open();
		auto dm = db.getStorage()->getDataManager();

		auto txn = db.beginTransaction();
		State st(0, "test_state_key", "state_data_value");
		dm->addStateEntity(st);
		stateId = st.getId();
		ASSERT_GT(stateId, 0);
		txn.commit();
		db.close();
	}

	{
		Database db2(dbPath.string());
		db2.open();
		auto dm = db2.getStorage()->getDataManager();

		State retrieved = dm->getState(stateId);
		EXPECT_TRUE(retrieved.isActive()) << "Committed state should be visible after WAL replay";

		db2.close();
	}
}

TEST_F(WALReplayTest, StateEntityDeleteRecoverySurvivesReopen) {
	int64_t stateId = 0;

	{
		Database db(dbPath.string());
		db.open();
		auto dm = db.getStorage()->getDataManager();

		{
			auto txn = db.beginTransaction();
			State st(0, "state_to_del", "data");
			dm->addStateEntity(st);
			stateId = st.getId();
			txn.commit();
		}

		{
			auto txn = db.beginTransaction();
			State toDelete = dm->getState(stateId);
			dm->deleteState(toDelete);
			txn.commit();
		}

		db.close();
	}

	{
		Database db2(dbPath.string());
		db2.open();
		auto dm = db2.getStorage()->getDataManager();

		State retrieved = dm->getState(stateId);
		EXPECT_FALSE(retrieved.isActive()) << "Deleted state should be inactive after WAL replay";

		db2.close();
	}
}

TEST_F(WALReplayTest, PropertyEntityDirectRecoverySurvivesReopen) {
	int64_t propId = 0;

	{
		Database db(dbPath.string());
		db.open();
		auto dm = db.getStorage()->getDataManager();

		auto txn = db.beginTransaction();
		Node n(0, 1);
		dm->addNode(n);

		Property prop(0, n.getId(), 0); // entityType=0 means Node
		dm->addPropertyEntity(prop);
		propId = prop.getId();
		ASSERT_GT(propId, 0);
		txn.commit();
		db.close();
	}

	{
		Database db2(dbPath.string());
		db2.open();
		auto dm = db2.getStorage()->getDataManager();

		Property retrieved = dm->getProperty(propId);
		EXPECT_TRUE(retrieved.isActive()) << "Committed property entity should be visible after WAL replay";

		db2.close();
	}
}

TEST_F(WALReplayTest, PropertyEntityDirectDeleteRecoverySurvivesReopen) {
	int64_t propId = 0;

	{
		Database db(dbPath.string());
		db.open();
		auto dm = db.getStorage()->getDataManager();

		{
			auto txn = db.beginTransaction();
			Node n(0, 1);
			dm->addNode(n);
			Property prop(0, n.getId(), 0);
			dm->addPropertyEntity(prop);
			propId = prop.getId();
			txn.commit();
		}

		{
			auto txn = db.beginTransaction();
			Property toDelete = dm->getProperty(propId);
			dm->deleteProperty(toDelete);
			txn.commit();
		}

		db.close();
	}

	{
		Database db2(dbPath.string());
		db2.open();
		auto dm = db2.getStorage()->getDataManager();

		Property retrieved = dm->getProperty(propId);
		EXPECT_FALSE(retrieved.isActive()) << "Deleted property entity should be inactive after WAL replay";

		db2.close();
	}
}

TEST_F(WALReplayTest, AllEntityTypesInSingleTransactionRecovery) {
	int64_t nodeId = 0, edgeId = 0, blobId = 0, indexId = 0, stateId = 0, propId = 0;

	{
		Database db(dbPath.string());
		db.open();
		auto dm = db.getStorage()->getDataManager();

		auto txn = db.beginTransaction();

		Node n1(0, 1);
		dm->addNode(n1);
		nodeId = n1.getId();

		Node n2(0, 2);
		dm->addNode(n2);

		Edge e(0, n1.getId(), n2.getId(), 1);
		dm->addEdge(e);
		edgeId = e.getId();

		Property prop(0, n1.getId(), 0);
		dm->addPropertyEntity(prop);
		propId = prop.getId();

		Blob blob(0, "all entity types test");
		dm->addBlobEntity(blob);
		blobId = blob.getId();

		Index idx(0, Index::NodeType::LEAF, 3);
		dm->addIndexEntity(idx);
		indexId = idx.getId();

		State st(0, "all_types_key", "all_types_data");
		dm->addStateEntity(st);
		stateId = st.getId();

		txn.commit();
		db.close();
	}

	{
		Database db2(dbPath.string());
		db2.open();
		auto dm = db2.getStorage()->getDataManager();

		Node nRet = dm->getNode(nodeId);
		EXPECT_TRUE(nRet.isActive()) << "Node should survive all-types WAL replay";

		Edge eRet = dm->getEdge(edgeId);
		EXPECT_TRUE(eRet.isActive()) << "Edge should survive all-types WAL replay";

		Property pRet = dm->getProperty(propId);
		EXPECT_TRUE(pRet.isActive()) << "Property should survive all-types WAL replay";

		Blob bRet = dm->getBlob(blobId);
		EXPECT_TRUE(bRet.isActive()) << "Blob should survive all-types WAL replay";

		Index iRet = dm->getIndex(indexId);
		EXPECT_TRUE(iRet.isActive()) << "Index should survive all-types WAL replay";

		State sRet = dm->getState(stateId);
		EXPECT_TRUE(sRet.isActive()) << "State should survive all-types WAL replay";

		db2.close();
	}
}

TEST_F(WALReplayTest, InjectedWALRecordsReadableForAllEntityTypes) {
	std::string walPath = dbPath.string() + "-wal";

	// Phase 1: Create a clean DB with a committed node
	{
		Database db(dbPath.string());
		db.open();
		auto txn = db.beginTransaction();
		Node n(0, 1);
		db.getStorage()->getDataManager()->addNode(n);
		txn.commit();
		db.close();
		// db.close() deletes the WAL file; that is fine here since we want
		// Phase 2 to inject fresh WAL records into a new WAL.
	}

	// Phase 2: Inject WAL records for ALL entity types (Property, Blob, Index, State)
	// Save the WAL content before mgr.close() so Phase 2.5 and Phase 3 can read it.
	std::vector<uint8_t> injectedWALContent;
	{
		WALManager mgr;
		mgr.open(dbPath.string());

		uint64_t txnId = 50000;
		mgr.writeBegin(txnId);

		// Property ADD
		Property prop(100, 1, 0);
		auto propSer = graph::utils::FixedSizeSerializer::serializeToBuffer(prop, Property::getTotalSize());
		mgr.writeEntityChange(txnId, static_cast<uint8_t>(Property::typeId),
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_ADDED),
							  100, std::vector<uint8_t>(propSer.begin(), propSer.end()));

		// Blob ADD
		Blob blob(101, "injected blob");
		auto blobSer = graph::utils::FixedSizeSerializer::serializeToBuffer(blob, Blob::getTotalSize());
		mgr.writeEntityChange(txnId, static_cast<uint8_t>(Blob::typeId),
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_ADDED),
							  101, std::vector<uint8_t>(blobSer.begin(), blobSer.end()));

		// Index ADD
		Index idx(102, Index::NodeType::LEAF, 1);
		auto idxSer = graph::utils::FixedSizeSerializer::serializeToBuffer(idx, Index::getTotalSize());
		mgr.writeEntityChange(txnId, static_cast<uint8_t>(Index::typeId),
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_ADDED),
							  102, std::vector<uint8_t>(idxSer.begin(), idxSer.end()));

		// State ADD
		State st(103, "key", "val");
		auto stSer = graph::utils::FixedSizeSerializer::serializeToBuffer(st, State::getTotalSize());
		mgr.writeEntityChange(txnId, static_cast<uint8_t>(State::typeId),
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_ADDED),
							  103, std::vector<uint8_t>(stSer.begin(), stSer.end()));

		mgr.writeCommit(txnId);
		mgr.sync();

		// Read WAL content before close (close deletes the WAL)
		{
			std::ifstream walIn(walPath, std::ios::binary);
			ASSERT_TRUE(walIn.is_open()) << "WAL file should exist before mgr.close()";
			injectedWALContent.assign(std::istreambuf_iterator<char>(walIn),
									  std::istreambuf_iterator<char>());
		}
		ASSERT_GT(injectedWALContent.size(), sizeof(WALFileHeader));

		mgr.close();

		// Restore WAL so Phase 2.5 and Phase 3 can read the injected records
		{
			std::ofstream walOut(walPath, std::ios::binary | std::ios::trunc);
			ASSERT_TRUE(walOut.is_open()) << "Should be able to write WAL file";
			walOut.write(reinterpret_cast<const char *>(injectedWALContent.data()),
						 static_cast<std::streamsize>(injectedWALContent.size()));
		}
	}

	// Phase 2.5: Verify the WAL records are readable and contain all entity types
	{
		WALManager mgr;
		mgr.open(dbPath.string());
		auto result = mgr.readRecords();

		// Read and save WAL content again before mgr.close() deletes it
		{
			std::ifstream walIn(walPath, std::ios::binary);
			if (walIn.is_open()) {
				injectedWALContent.assign(std::istreambuf_iterator<char>(walIn),
										  std::istreambuf_iterator<char>());
			}
		}

		mgr.close();

		ASSERT_FALSE(result.records.empty());

		std::set<uint8_t> entityTypesFound;
		for (const auto &rec : result.records) {
			if (rec.header.type == WALRecordType::WAL_ENTITY_WRITE) {
				if (rec.data.size() >= sizeof(WALEntityPayload)) {
					auto payload = deserializeEntityPayload(rec.data.data());
					entityTypesFound.insert(payload.entityType);
				}
			}
		}

		EXPECT_TRUE(entityTypesFound.count(static_cast<uint8_t>(Property::typeId)))
				<< "WAL should contain Property entity records";
		EXPECT_TRUE(entityTypesFound.count(static_cast<uint8_t>(Blob::typeId)))
				<< "WAL should contain Blob entity records";
		EXPECT_TRUE(entityTypesFound.count(static_cast<uint8_t>(Index::typeId)))
				<< "WAL should contain Index entity records";
		EXPECT_TRUE(entityTypesFound.count(static_cast<uint8_t>(State::typeId)))
				<< "WAL should contain State entity records";

		// Restore WAL for Phase 3 (mgr.close() above deleted it)
		if (!injectedWALContent.empty()) {
			std::ofstream walOut(walPath, std::ios::binary | std::ios::trunc);
			if (walOut.is_open()) {
				walOut.write(reinterpret_cast<const char *>(injectedWALContent.data()),
							 static_cast<std::streamsize>(injectedWALContent.size()));
			}
		}
	}

	// Phase 3: Reopen DB — WAL recovery should exercise replayAddOrModify for all 4 types
	{
		Database db2(dbPath.string());
		db2.open();
		// Must start a transaction to trigger WAL recovery (ensureWALAndTransactionManager)
		auto txn = db2.beginReadOnlyTransaction();
		txn.commit();
		db2.close();
	}
}

TEST_F(WALReplayTest, InjectedWALDeleteRecordsForAllEntityTypes) {
	int64_t propId = 0, blobId = 0, indexId = 0, stateId = 0;

	// Phase 1: Create DB with entities of all types
	{
		Database db(dbPath.string());
		db.open();
		auto dm = db.getStorage()->getDataManager();

		auto txn = db.beginTransaction();
		Node n(0, 1);
		dm->addNode(n);

		Property prop(0, n.getId(), 0);
		dm->addPropertyEntity(prop);
		propId = prop.getId();

		Blob blob(0, "to delete");
		dm->addBlobEntity(blob);
		blobId = blob.getId();

		Index idx(0, Index::NodeType::LEAF, 1);
		dm->addIndexEntity(idx);
		indexId = idx.getId();

		State st(0, "to_del", "data");
		dm->addStateEntity(st);
		stateId = st.getId();

		txn.commit();
		db.close();
	}

	// Phase 2: Inject DELETE WAL records for all entity types
	{
		WALManager mgr;
		mgr.open(dbPath.string());

		uint64_t txnId = 50001;
		mgr.writeBegin(txnId);

		// Property DELETE
		Property prop(propId, 1, 0);
		auto propSer = graph::utils::FixedSizeSerializer::serializeToBuffer(prop, Property::getTotalSize());
		mgr.writeEntityChange(txnId, static_cast<uint8_t>(Property::typeId),
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_DELETED),
							  propId, std::vector<uint8_t>(propSer.begin(), propSer.end()));

		// Blob DELETE
		Blob blob(blobId, "to delete");
		auto blobSer = graph::utils::FixedSizeSerializer::serializeToBuffer(blob, Blob::getTotalSize());
		mgr.writeEntityChange(txnId, static_cast<uint8_t>(Blob::typeId),
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_DELETED),
							  blobId, std::vector<uint8_t>(blobSer.begin(), blobSer.end()));

		// Index DELETE
		Index idx(indexId, Index::NodeType::LEAF, 1);
		auto idxSer = graph::utils::FixedSizeSerializer::serializeToBuffer(idx, Index::getTotalSize());
		mgr.writeEntityChange(txnId, static_cast<uint8_t>(Index::typeId),
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_DELETED),
							  indexId, std::vector<uint8_t>(idxSer.begin(), idxSer.end()));

		// State DELETE
		State st(stateId, "to_del", "data");
		auto stSer = graph::utils::FixedSizeSerializer::serializeToBuffer(st, State::getTotalSize());
		mgr.writeEntityChange(txnId, static_cast<uint8_t>(State::typeId),
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_DELETED),
							  stateId, std::vector<uint8_t>(stSer.begin(), stSer.end()));

		mgr.writeCommit(txnId);
		mgr.sync();
		mgr.close();
	}

	// Phase 3: Reopen — WAL recovery should exercise replayDelete for all 4 types
	{
		Database db2(dbPath.string());
		EXPECT_NO_THROW(db2.open());
		{ auto txn = db2.beginReadOnlyTransaction(); txn.commit(); }
		db2.close();
	}
}

TEST_F(WALReplayTest, InjectedPropertyEntityAddRecovery) {
	// Phase 1: Create a DB with a node (we need something so the DB file is valid)
	int64_t nodeId = 0;
	{
		Database db(dbPath.string());
		db.open();
		auto txn = db.beginTransaction();
		Node n(0, 1);
		db.getStorage()->getDataManager()->addNode(n);
		nodeId = n.getId();
		txn.commit();
		db.close();
	}

	// Phase 2: Manually inject a Property entity WAL record
	{
		WALManager mgr;
		mgr.open(dbPath.string());

		Property prop(1, nodeId, 0); // id=1, entityId=nodeId, entityType=0 (Node)
		auto propSer = graph::utils::FixedSizeSerializer::serializeToBuffer(prop, Property::getTotalSize());
		std::vector<uint8_t> serialized(propSer.begin(), propSer.end());

		uint64_t txnId = 9999;
		mgr.writeBegin(txnId);
		mgr.writeEntityChange(txnId, Property::typeId,
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_ADDED),
							  prop.getId(), serialized);
		mgr.writeCommit(txnId);
		mgr.sync();
		mgr.close();
	}

	// Phase 3: Reopen — WAL recovery should replay the Property add
	{
		Database db2(dbPath.string());
		EXPECT_NO_THROW(db2.open());
		{ auto txn = db2.beginReadOnlyTransaction(); txn.commit(); }
		db2.close();
	}
}

TEST_F(WALReplayTest, InjectedBlobEntityAddRecovery) {
	{
		Database db(dbPath.string());
		db.open();
		auto txn = db.beginTransaction();
		Node n(0, 1);
		db.getStorage()->getDataManager()->addNode(n);
		txn.commit();
		db.close();
	}

	{
		WALManager mgr;
		mgr.open(dbPath.string());

		Blob blob(1, "injected blob data");
		auto serialized = graph::utils::FixedSizeSerializer::serializeToBuffer(blob, Blob::getTotalSize());

		uint64_t txnId = 10001;
		mgr.writeBegin(txnId);
		mgr.writeEntityChange(txnId, Blob::typeId,
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_ADDED),
							  blob.getId(),
							  std::vector<uint8_t>(serialized.begin(), serialized.end()));
		mgr.writeCommit(txnId);
		mgr.sync();
		mgr.close();
	}

	// Reopen — WAL recovery should replay the Blob add (exercising replayAddOrModify<Blob>)
	{
		Database db2(dbPath.string());
		EXPECT_NO_THROW(db2.open());
		{ auto txn = db2.beginReadOnlyTransaction(); txn.commit(); }
		db2.close();
	}
}

TEST_F(WALReplayTest, InjectedIndexEntityAddRecovery) {
	{
		Database db(dbPath.string());
		db.open();
		auto txn = db.beginTransaction();
		Node n(0, 1);
		db.getStorage()->getDataManager()->addNode(n);
		txn.commit();
		db.close();
	}

	{
		WALManager mgr;
		mgr.open(dbPath.string());

		Index idx(1, Index::NodeType::LEAF, 1);
		auto serialized = graph::utils::FixedSizeSerializer::serializeToBuffer(idx, Index::getTotalSize());

		uint64_t txnId = 10002;
		mgr.writeBegin(txnId);
		mgr.writeEntityChange(txnId, Index::typeId,
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_ADDED),
							  idx.getId(),
							  std::vector<uint8_t>(serialized.begin(), serialized.end()));
		mgr.writeCommit(txnId);
		mgr.sync();
		mgr.close();
	}

	{
		Database db2(dbPath.string());
		EXPECT_NO_THROW(db2.open());
		{ auto txn = db2.beginReadOnlyTransaction(); txn.commit(); }
		auto dm = db2.getStorage()->getDataManager();
		Index retrieved = dm->getIndex(1);
		EXPECT_TRUE(retrieved.isActive()) << "Injected index should be recovered via WAL replay";
		db2.close();
	}
}

TEST_F(WALReplayTest, InjectedStateEntityAddRecovery) {
	{
		Database db(dbPath.string());
		db.open();
		auto txn = db.beginTransaction();
		Node n(0, 1);
		db.getStorage()->getDataManager()->addNode(n);
		txn.commit();
		db.close();
	}

	{
		WALManager mgr;
		mgr.open(dbPath.string());

		State st(1, "injected_key", "injected_val");
		auto serialized = graph::utils::FixedSizeSerializer::serializeToBuffer(st, State::getTotalSize());

		uint64_t txnId = 10003;
		mgr.writeBegin(txnId);
		mgr.writeEntityChange(txnId, State::typeId,
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_ADDED),
							  st.getId(),
							  std::vector<uint8_t>(serialized.begin(), serialized.end()));
		mgr.writeCommit(txnId);
		mgr.sync();
		mgr.close();
	}

	{
		Database db2(dbPath.string());
		EXPECT_NO_THROW(db2.open());
		{ auto txn = db2.beginReadOnlyTransaction(); txn.commit(); }
		auto dm = db2.getStorage()->getDataManager();
		State retrieved = dm->getState(1);
		EXPECT_TRUE(retrieved.isActive()) << "Injected state should be recovered via WAL replay";
		db2.close();
	}
}

TEST_F(WALReplayTest, InjectedPropertyEntityDeleteRecovery) {
	int64_t propId = 0;
	{
		Database db(dbPath.string());
		db.open();
		auto dm = db.getStorage()->getDataManager();
		auto txn = db.beginTransaction();
		Node n(0, 1);
		dm->addNode(n);
		Property prop(0, n.getId(), 0);
		dm->addPropertyEntity(prop);
		propId = prop.getId();
		txn.commit();
		db.close();
	}

	// Inject a DELETE WAL record for the property
	{
		WALManager mgr;
		mgr.open(dbPath.string());

		Property prop(propId, 1, 0); // reconstruct with same id
		auto serialized = graph::utils::FixedSizeSerializer::serializeToBuffer(prop, Property::getTotalSize());

		uint64_t txnId = 10004;
		mgr.writeBegin(txnId);
		mgr.writeEntityChange(txnId, Property::typeId,
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_DELETED),
							  propId,
							  std::vector<uint8_t>(serialized.begin(), serialized.end()));
		mgr.writeCommit(txnId);
		mgr.sync();
		mgr.close();
	}

	// Reopen — WAL recovery should replay the Property delete (exercising replayDelete<Property>)
	{
		Database db2(dbPath.string());
		EXPECT_NO_THROW(db2.open());
		{ auto txn = db2.beginReadOnlyTransaction(); txn.commit(); }
		db2.close();
	}
}

TEST_F(WALReplayTest, InjectedBlobEntityDeleteRecovery) {
	int64_t blobId = 0;
	{
		Database db(dbPath.string());
		db.open();
		auto dm = db.getStorage()->getDataManager();
		auto txn = db.beginTransaction();
		Blob blob(0, "blob to inject-delete");
		dm->addBlobEntity(blob);
		blobId = blob.getId();
		txn.commit();
		db.close();
	}

	{
		WALManager mgr;
		mgr.open(dbPath.string());

		Blob blob(blobId, "blob to inject-delete");
		auto serialized = graph::utils::FixedSizeSerializer::serializeToBuffer(blob, Blob::getTotalSize());

		uint64_t txnId = 10005;
		mgr.writeBegin(txnId);
		mgr.writeEntityChange(txnId, Blob::typeId,
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_DELETED),
							  blobId,
							  std::vector<uint8_t>(serialized.begin(), serialized.end()));
		mgr.writeCommit(txnId);
		mgr.sync();
		mgr.close();
	}

	// Reopen — WAL recovery should replay the Blob delete (exercising replayDelete<Blob>)
	{
		Database db2(dbPath.string());
		EXPECT_NO_THROW(db2.open());
		{ auto txn = db2.beginReadOnlyTransaction(); txn.commit(); }
		db2.close();
	}
}

TEST_F(WALReplayTest, InjectedIndexEntityDeleteRecovery) {
	int64_t indexId = 0;
	{
		Database db(dbPath.string());
		db.open();
		auto dm = db.getStorage()->getDataManager();
		auto txn = db.beginTransaction();
		Index idx(0, Index::NodeType::LEAF, 5);
		dm->addIndexEntity(idx);
		indexId = idx.getId();
		txn.commit();
		db.close();
	}

	{
		WALManager mgr;
		mgr.open(dbPath.string());

		Index idx(indexId, Index::NodeType::LEAF, 5);
		auto serialized = graph::utils::FixedSizeSerializer::serializeToBuffer(idx, Index::getTotalSize());

		uint64_t txnId = 10006;
		mgr.writeBegin(txnId);
		mgr.writeEntityChange(txnId, Index::typeId,
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_DELETED),
							  indexId,
							  std::vector<uint8_t>(serialized.begin(), serialized.end()));
		mgr.writeCommit(txnId);
		mgr.sync();
		mgr.close();
	}

	// Reopen — WAL recovery should replay the Index delete (exercising replayDelete<Index>)
	{
		Database db2(dbPath.string());
		EXPECT_NO_THROW(db2.open());
		{ auto txn = db2.beginReadOnlyTransaction(); txn.commit(); }
		db2.close();
	}
}

TEST_F(WALReplayTest, InjectedStateEntityDeleteRecovery) {
	int64_t stateId = 0;
	{
		Database db(dbPath.string());
		db.open();
		auto dm = db.getStorage()->getDataManager();
		auto txn = db.beginTransaction();
		State st(0, "state_to_del", "data");
		dm->addStateEntity(st);
		stateId = st.getId();
		txn.commit();
		db.close();
	}

	{
		WALManager mgr;
		mgr.open(dbPath.string());

		State st(stateId, "state_to_del", "data");
		auto serialized = graph::utils::FixedSizeSerializer::serializeToBuffer(st, State::getTotalSize());

		uint64_t txnId = 10007;
		mgr.writeBegin(txnId);
		mgr.writeEntityChange(txnId, State::typeId,
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_DELETED),
							  stateId,
							  std::vector<uint8_t>(serialized.begin(), serialized.end()));
		mgr.writeCommit(txnId);
		mgr.sync();
		mgr.close();
	}

	// Reopen — WAL recovery should replay the State delete (exercising replayDelete<State>)
	{
		Database db2(dbPath.string());
		EXPECT_NO_THROW(db2.open());
		{ auto txn = db2.beginReadOnlyTransaction(); txn.commit(); }
		db2.close();
	}
}

TEST_F(WALReplayTest, DeleteAllEntityTypesRecovery) {
	int64_t blobId = 0, indexId = 0, stateId = 0, propId = 0;

	{
		Database db(dbPath.string());
		db.open();
		auto dm = db.getStorage()->getDataManager();

		// Create entities
		{
			auto txn = db.beginTransaction();

			Node n(0, 1);
			dm->addNode(n);

			Property prop(0, n.getId(), 0);
			dm->addPropertyEntity(prop);
			propId = prop.getId();

			Blob blob(0, "delete all test");
			dm->addBlobEntity(blob);
			blobId = blob.getId();

			Index idx(0, Index::NodeType::LEAF, 4);
			dm->addIndexEntity(idx);
			indexId = idx.getId();

			State st(0, "del_all_key", "del_all_data");
			dm->addStateEntity(st);
			stateId = st.getId();

			txn.commit();
		}

		// Delete all entities
		{
			auto txn = db.beginTransaction();
			Property pDel = dm->getProperty(propId);
			dm->deleteProperty(pDel);
			Blob bDel = dm->getBlob(blobId);
			dm->deleteBlob(bDel);
			Index iDel = dm->getIndex(indexId);
			dm->deleteIndex(iDel);
			State sDel = dm->getState(stateId);
			dm->deleteState(sDel);
			txn.commit();
		}

		db.close();
	}

	{
		Database db2(dbPath.string());
		db2.open();
		{ auto txn = db2.beginReadOnlyTransaction(); txn.commit(); }
		auto dm = db2.getStorage()->getDataManager();

		Property pRet = dm->getProperty(propId);
		EXPECT_FALSE(pRet.isActive()) << "Deleted property should be inactive after WAL replay";

		Blob bRet = dm->getBlob(blobId);
		EXPECT_FALSE(bRet.isActive()) << "Deleted blob should be inactive after WAL replay";

		Index iRet = dm->getIndex(indexId);
		EXPECT_FALSE(iRet.isActive()) << "Deleted index should be inactive after WAL replay";

		State sRet = dm->getState(stateId);
		EXPECT_FALSE(sRet.isActive()) << "Deleted state should be inactive after WAL replay";

		db2.close();
	}
}
