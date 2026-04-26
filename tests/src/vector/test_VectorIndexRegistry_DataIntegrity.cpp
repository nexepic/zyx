/**
 * @file test_VectorIndexRegistry_DataIntegrity.cpp
 * @brief Tests targeting uncovered branches in VectorIndexRegistry.cpp:
 *        - getBlobPtrs data.size() != sizeof(VectorBlobPtrs) (line 116-117)
 */

// Include system/standard headers FIRST, before the #define hack
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <vector>
#include <cstring>

// Include project headers that might pull in ranges/etc
#include "graph/core/Database.hpp"
#include "graph/storage/data/BlobManager.hpp"

// Now expose private members for VectorIndexRegistry only
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#define private public
#pragma clang diagnostic pop
#include "graph/vector/VectorIndexRegistry.hpp"
#undef private

using namespace graph::vector;

class VectorRegistryDataIntegrityTest : public ::testing::Test {
protected:
	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		dbPath = std::filesystem::temp_directory_path() / ("vec_data_integ_" + to_string(uuid) + ".zyx");
		database = std::make_unique<graph::Database>(dbPath.string());
		database->open();

		dm = database->getStorage()->getDataManager();
		sm = database->getStorage()->getSystemStateManager();
	}

	void TearDown() override {
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
};

// ============================================================================
// getBlobPtrs: data.size() != sizeof(VectorBlobPtrs) -> return {0,0,0}
// Uses #define private public to access mappingTree_ and corrupt the blob data
// ============================================================================

TEST_F(VectorRegistryDataIntegrityTest, GetBlobPtrsCorruptedBlobSize) {
	auto registry = std::make_shared<VectorIndexRegistry>(dm, sm, "corrupt_blob_test");
	VectorIndexConfig cfg;
	cfg.dimension = 4;
	registry->updateConfig(cfg);

	int64_t nodeId = 42;
	VectorBlobPtrs validPtrs{100, 200, 300};
	registry->setBlobPtrs(nodeId, validPtrs);

	// Now find the blob ID from the B+Tree using the exposed private member
	auto ids = registry->mappingTree_->find(registry->config_.mappingIndexId,
	                                        graph::PropertyValue(nodeId));
	ASSERT_FALSE(ids.empty());
	int64_t blobId = ids[0];

	// Overwrite the blob with wrong-sized data (not sizeof(VectorBlobPtrs))
	std::string wrongSizeData = "short";
	(void)dm->getBlobManager()->updateBlobChain(blobId, nodeId, 0, wrongSizeData);

	// Create a fresh registry (bypasses cache) to force read from B+Tree
	auto registry2 = std::make_shared<VectorIndexRegistry>(dm, sm, "corrupt_blob_test");

	// getBlobPtrs should hit the size mismatch branch and return {0,0,0}
	auto ptrs = registry2->getBlobPtrs(nodeId);
	EXPECT_EQ(ptrs.rawBlob, 0);
	EXPECT_EQ(ptrs.pqBlob, 0);
	EXPECT_EQ(ptrs.adjBlob, 0);
}
