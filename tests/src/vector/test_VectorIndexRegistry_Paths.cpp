/**
 * @file test_VectorIndexRegistry_Paths.cpp
 * @brief Branch coverage tests for VectorIndexRegistry.cpp:
 *        - loadRawVector with blobId == 0
 *        - loadPQCodes with blobId == 0
 *        - loadAdjacency with blobId == 0 and small data
 *        - loadQuantizer when not trained
 *        - saveAdjacency with empty neighbors
 *        - setBlobPtrs update path (ids not empty)
 *        - setBlobPtrs insert path with root change
 *        - getAllNodeIds with mappingIndexId == 0
 */

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <vector>
#include <cstring>

#include "graph/core/Database.hpp"
#include "graph/storage/data/BlobManager.hpp"

#include "graph/vector/VectorIndexRegistry.hpp"

using namespace graph::vector;

class VectorRegistryPathsTest : public ::testing::Test {
protected:
	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		dbPath = std::filesystem::temp_directory_path() / ("vec_paths_" + to_string(uuid) + ".zyx");
		database = std::make_unique<graph::Database>(dbPath.string());
		database->open();

		dm = database->getStorage()->getDataManager();
		sm = database->getStorage()->getSystemStateManager();
	}

	void TearDown() override {
		registry.reset();
		dm.reset();
		sm.reset();
		database->close();
		database.reset();
		std::error_code ec;
		std::filesystem::remove_all(dbPath, ec);
		std::filesystem::remove(dbPath.string() + "-wal", ec);
	}

	std::filesystem::path dbPath;
	std::unique_ptr<graph::Database> database;
	std::shared_ptr<graph::storage::DataManager> dm;
	std::shared_ptr<graph::storage::state::SystemStateManager> sm;
	std::unique_ptr<VectorIndexRegistry> registry;

	void createRegistry() {
		auto txn = database->beginTransaction();
		registry = std::make_unique<VectorIndexRegistry>(dm, sm, "test_vec");
		txn.commit();
	}
};

// ============================================================================
// loadRawVector with blobId == 0 returns empty
// ============================================================================

TEST_F(VectorRegistryPathsTest, LoadRawVectorZeroBlobId) {
	createRegistry();
	auto vec = registry->loadRawVector(0);
	EXPECT_TRUE(vec.empty());
}

// ============================================================================
// loadPQCodes with blobId == 0 returns empty
// ============================================================================

TEST_F(VectorRegistryPathsTest, LoadPQCodesZeroBlobId) {
	createRegistry();
	auto codes = registry->loadPQCodes(0);
	EXPECT_TRUE(codes.empty());
}

// ============================================================================
// loadAdjacency with blobId == 0 returns empty
// ============================================================================

TEST_F(VectorRegistryPathsTest, LoadAdjacencyZeroBlobId) {
	createRegistry();
	auto adj = registry->loadAdjacency(0);
	EXPECT_TRUE(adj.empty());
}

// ============================================================================
// loadQuantizer when not trained
// ============================================================================

TEST_F(VectorRegistryPathsTest, LoadQuantizerNotTrained) {
	createRegistry();
	auto pq = registry->loadQuantizer();
	EXPECT_EQ(pq, nullptr);
}

// ============================================================================
// saveAdjacency with empty neighbors
// ============================================================================

TEST_F(VectorRegistryPathsTest, SaveAdjacencyEmpty) {
	createRegistry();
	auto txn = database->beginTransaction();
	int64_t blobId = registry->saveAdjacency({});
	EXPECT_NE(blobId, 0);

	// Load back — should be empty
	auto adj = registry->loadAdjacency(blobId);
	EXPECT_TRUE(adj.empty());
	txn.commit();
}

// ============================================================================
// saveAdjacency with non-empty neighbors
// ============================================================================

TEST_F(VectorRegistryPathsTest, SaveAdjacencyNonEmpty) {
	createRegistry();
	auto txn = database->beginTransaction();
	std::vector<int64_t> neighbors = {1, 2, 3, 4, 5};
	int64_t blobId = registry->saveAdjacency(neighbors);
	EXPECT_NE(blobId, 0);

	auto adj = registry->loadAdjacency(blobId);
	EXPECT_EQ(adj.size(), 5u);
	EXPECT_EQ(adj[0], 1);
	EXPECT_EQ(adj[4], 5);
	txn.commit();
}

// ============================================================================
// saveRawVector and loadRawVector roundtrip
// ============================================================================

TEST_F(VectorRegistryPathsTest, SaveAndLoadRawVector) {
	createRegistry();
	auto txn = database->beginTransaction();

	std::vector<BFloat16> vec(10);
	for (int i = 0; i < 10; i++) {
		vec[i] = BFloat16(static_cast<float>(i) * 0.5f);
	}

	int64_t blobId = registry->saveRawVector(vec);
	EXPECT_NE(blobId, 0);

	auto loaded = registry->loadRawVector(blobId);
	EXPECT_EQ(loaded.size(), 10u);
	txn.commit();
}

// ============================================================================
// savePQCodes and loadPQCodes roundtrip
// ============================================================================

TEST_F(VectorRegistryPathsTest, SaveAndLoadPQCodes) {
	createRegistry();
	auto txn = database->beginTransaction();

	std::vector<uint8_t> codes = {0, 1, 2, 3, 4, 5, 6, 7};
	int64_t blobId = registry->savePQCodes(codes);
	EXPECT_NE(blobId, 0);

	auto loaded = registry->loadPQCodes(blobId);
	EXPECT_EQ(loaded.size(), 8u);
	EXPECT_EQ(loaded[0], 0);
	EXPECT_EQ(loaded[7], 7);
	txn.commit();
}

// ============================================================================
// getAllNodeIds when no mappings exist
// ============================================================================

TEST_F(VectorRegistryPathsTest, GetAllNodeIdsEmpty) {
	createRegistry();
	auto ids = registry->getAllNodeIds(100);
	EXPECT_TRUE(ids.empty());
}

// ============================================================================
// setBlobPtrs insert path
// ============================================================================

TEST_F(VectorRegistryPathsTest, SetAndGetBlobPtrs) {
	createRegistry();
	auto txn = database->beginTransaction();

	VectorBlobPtrs ptrs = {100, 200, 300};
	registry->setBlobPtrs(42, ptrs);

	auto loaded = registry->getBlobPtrs(42);
	EXPECT_EQ(loaded.rawBlob, 100);
	EXPECT_EQ(loaded.pqBlob, 200);
	EXPECT_EQ(loaded.adjBlob, 300);

	txn.commit();
}

// ============================================================================
// setBlobPtrs update path (overwrite existing)
// ============================================================================

TEST_F(VectorRegistryPathsTest, UpdateBlobPtrs) {
	createRegistry();
	auto txn = database->beginTransaction();

	VectorBlobPtrs ptrs1 = {100, 200, 300};
	registry->setBlobPtrs(42, ptrs1);

	VectorBlobPtrs ptrs2 = {400, 500, 600};
	registry->setBlobPtrs(42, ptrs2);

	auto loaded = registry->getBlobPtrs(42);
	EXPECT_EQ(loaded.rawBlob, 400);
	EXPECT_EQ(loaded.pqBlob, 500);
	EXPECT_EQ(loaded.adjBlob, 600);

	txn.commit();
}

// ============================================================================
// updateConfig
// ============================================================================

TEST_F(VectorRegistryPathsTest, UpdateConfig) {
	createRegistry();
	auto txn = database->beginTransaction();

	VectorIndexConfig newConfig = registry->getConfig();
	newConfig.dimension = 128;
	newConfig.pqSubspaces = 8;
	newConfig.entryPointNodeId = 42;
	registry->updateConfig(newConfig);

	EXPECT_EQ(registry->getConfig().dimension, 128u);
	EXPECT_EQ(registry->getConfig().pqSubspaces, 8u);
	EXPECT_EQ(registry->getConfig().entryPointNodeId, 42);

	txn.commit();
}

// ============================================================================
// updateEntryPoint
// ============================================================================

TEST_F(VectorRegistryPathsTest, UpdateEntryPoint) {
	createRegistry();
	auto txn = database->beginTransaction();

	registry->updateEntryPoint(99);
	EXPECT_EQ(registry->getConfig().entryPointNodeId, 99);

	txn.commit();
}

// ============================================================================
// getBlobPtrs cache hit: second call returns from cache
// Exercises line 107-108: mappingCache_.contains(nodeId) true branch
// ============================================================================

TEST_F(VectorRegistryPathsTest, GetBlobPtrsCacheHit) {
	createRegistry();
	auto txn = database->beginTransaction();

	VectorBlobPtrs ptrs = {111, 222, 333};
	registry->setBlobPtrs(50, ptrs);

	// First call: cache miss, reads from B+Tree
	auto loaded1 = registry->getBlobPtrs(50);
	EXPECT_EQ(loaded1.rawBlob, 111);

	// Second call: cache hit (line 107-108)
	auto loaded2 = registry->getBlobPtrs(50);
	EXPECT_EQ(loaded2.rawBlob, 111);
	EXPECT_EQ(loaded2.pqBlob, 222);
	EXPECT_EQ(loaded2.adjBlob, 333);

	txn.commit();
}

// ============================================================================
// getBlobPtrs for non-existent node returns zeros
// Exercises line 111-112: ids.empty() true branch
// ============================================================================

TEST_F(VectorRegistryPathsTest, GetBlobPtrsNonExistent) {
	createRegistry();
	auto loaded = registry->getBlobPtrs(9999);
	EXPECT_EQ(loaded.rawBlob, 0);
	EXPECT_EQ(loaded.pqBlob, 0);
	EXPECT_EQ(loaded.adjBlob, 0);
}

// ============================================================================
// loadAdjacency with data smaller than sizeof(uint32_t)
// Exercises line 204: data.size() < sizeof(uint32_t) true branch
// ============================================================================

TEST_F(VectorRegistryPathsTest, LoadAdjacencyTruncatedData) {
	createRegistry();
	auto txn = database->beginTransaction();

	// Create a blob chain with data shorter than sizeof(uint32_t)
	std::string shortData = "ab"; // 2 bytes < 4 bytes
	auto blobs = dm->getBlobManager()->createBlobChain(0, 0, shortData);
	ASSERT_FALSE(blobs.empty());

	auto adj = registry->loadAdjacency(blobs[0].getId());
	EXPECT_TRUE(adj.empty());

	txn.commit();
}

// ============================================================================
// getAllNodeIds after inserting mappings
// Exercises the normal path with data in the B+Tree
// ============================================================================

TEST_F(VectorRegistryPathsTest, GetAllNodeIdsWithData) {
	createRegistry();
	auto txn = database->beginTransaction();

	// Insert some mappings
	VectorBlobPtrs ptrs1 = {10, 20, 30};
	VectorBlobPtrs ptrs2 = {40, 50, 60};
	registry->setBlobPtrs(100, ptrs1);
	registry->setBlobPtrs(200, ptrs2);

	auto ids = registry->getAllNodeIds(100);
	EXPECT_GE(ids.size(), 2u);

	txn.commit();
}

// ============================================================================
// loadQuantizer when binaryData is empty (trained but data missing)
// Exercises line 95-96: binaryData.empty() true branch
// ============================================================================

TEST_F(VectorRegistryPathsTest, LoadQuantizerEmptyData) {
	createRegistry();
	auto txn = database->beginTransaction();

	// Set isTrained=true via the public API but don't save any codebook data.
	// After createRegistry(), config already has codebookKey set correctly.
	auto cfg = registry->getConfig();
	cfg.isTrained = true;
	registry->updateConfig(cfg);

	auto pq = registry->loadQuantizer();
	EXPECT_EQ(pq, nullptr);

	txn.commit();
}
