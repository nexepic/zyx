/**
 * @file test_DataManager_Initialization.cpp
 * @brief Tests for DataManager initialization-related defensive branches:
 *
 *   - Line 81:  preadBytes() returns -1 when storageIO_ is null
 *               (the !storageIO_ || !storageIO_->hasPreadSupport() branch)
 *   - Line 151: getOrCreateTokenId() throws when tokenRegistry_ is null
 *   - Line 160: resolveTokenId() throws when tokenRegistry_ is null
 *   - Line 169: resolveTokenName() returns "" when tokenRegistry_ is null
 *               and tokenId != 0
 *
 * All four cases require a DataManager that was constructed but never had
 * setSystemStateManager() called (so tokenRegistry_ remains nullptr), or
 * a DataManager constructed with storageIO = nullptr.
 *
 * We build the minimal dependency graph directly — SegmentTracker with an
 * all-zero FileHeader (no segment chains to load), empty IDAllocators, and
 * a no-pread StorageIO — so no actual file I/O occurs during construction.
 **/

#include <gtest/gtest.h>
#include <array>
#include <memory>
#include "graph/storage/IDAllocator.hpp"
#include "graph/storage/SegmentTracker.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/StorageHeaders.hpp"
#include "graph/storage/PwriteHelper.hpp"
#include "graph/storage/data/DataManager.hpp"

using namespace graph::storage;

namespace {

/**
 * Build a minimal DataManager suitable for testing constructor-level branches.
 *
 * The DataManager is constructed but NOT initialized (initialize() is not
 * called). This means tokenRegistry_ remains null and no entity managers
 * are created. Only the methods that operate on storageIO_ or tokenRegistry_
 * directly can be safely called on this object.
 *
 * @param useNullStorageIO  When true, passes storageIO = nullptr.
 *                          When false, passes a StorageIO with INVALID_FILE_HANDLE
 *                          (hasPreadSupport() == false, hasPwriteSupport() == false).
 */
std::shared_ptr<DataManager> makeMinimalDataManager(bool useNullStorageIO = true) {
    // A default-constructed FileHeader has all segment-chain heads == 0,
    // so SegmentTracker::loadSegments() will never call io_->readAt().
    static FileHeader dummyHeader{};

    // A no-op StorageIO: both file-handle fields are INVALID_FILE_HANDLE.
    // This makes hasPreadSupport() == false, but the object is valid.
    auto noOpIO = std::make_shared<StorageIO>(
        nullptr, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);

    // SegmentTracker is required by the DataManager constructor.
    // Pass the no-op IO so the object is live; chain heads are 0 so
    // loadSegmentChain() exits immediately without calling io_->readAt().
    auto segmentTracker = std::make_shared<SegmentTracker>(noOpIO, dummyHeader);

    // IDAllocators is an array of shared_ptrs; leaving them null is fine
    // because none of the target methods (preadBytes, token methods) use them.
    IDAllocators allocators{};

    // Choose the storageIO passed to DataManager
    std::shared_ptr<StorageIO> dmIO = useNullStorageIO ? nullptr : noOpIO;

    return std::make_shared<DataManager>(
        nullptr,          // fstream — not used by target methods
        64,               // cacheSize — minimal page pool
        dummyHeader,
        allocators,
        segmentTracker,
        dmIO);
}

} // anonymous namespace

// ============================================================================
// preadBytes: returns -1 when storageIO_ is null (line 81 true branch)
// ============================================================================

TEST(DataManagerInitialization, PreadBytes_NullStorageIO_ReturnsMinus1) {
    auto dm = makeMinimalDataManager(/*useNullStorageIO=*/true);

    // hasPreadSupport() should be false when storageIO_ is null
    EXPECT_FALSE(dm->hasPreadSupport());

    // preadBytes must return -1 for the null storageIO path
    char buf[16]{};
    ssize_t result = dm->preadBytes(buf, sizeof(buf), 0);
    EXPECT_EQ(result, -1);
}

// ============================================================================
// preadBytes: returns -1 when storageIO_ has no pread support (line 81)
// ============================================================================

TEST(DataManagerInitialization, PreadBytes_NoPreadSupport_ReturnsMinus1) {
    // StorageIO with INVALID_FILE_HANDLE => hasPreadSupport() == false
    auto dm = makeMinimalDataManager(/*useNullStorageIO=*/false);

    EXPECT_FALSE(dm->hasPreadSupport());

    char buf[16]{};
    ssize_t result = dm->preadBytes(buf, sizeof(buf), 0);
    EXPECT_EQ(result, -1);
}

// ============================================================================
// getOrCreateTokenId: throws when tokenRegistry_ is null (line 151)
// ============================================================================

TEST(DataManagerInitialization, GetOrCreateTokenId_NotInitialized_Throws) {
    auto dm = makeMinimalDataManager();

    // Empty string still returns 0 (early-exit before registry check)
    EXPECT_EQ(dm->getOrCreateTokenId(""), 0);

    // Non-empty string must throw because tokenRegistry_ is null
    EXPECT_THROW(dm->getOrCreateTokenId("SomeLabel"), std::runtime_error);
}

// ============================================================================
// resolveTokenId: throws when tokenRegistry_ is null (line 160)
// ============================================================================

TEST(DataManagerInitialization, ResolveTokenId_NotInitialized_Throws) {
    auto dm = makeMinimalDataManager();

    // Empty string still returns 0 (early-exit before registry check)
    EXPECT_EQ(dm->resolveTokenId(""), 0);

    // Non-empty string must throw because tokenRegistry_ is null
    EXPECT_THROW(dm->resolveTokenId("AnyLabel"), std::runtime_error);
}

// ============================================================================
// resolveTokenName: returns "" when tokenRegistry_ is null and tokenId != 0
// (line 169 — the false branch of `if (!tokenRegistry_)` returns "")
// ============================================================================

TEST(DataManagerInitialization, ResolveTokenName_NotInitialized_NonZeroId_ReturnsEmpty) {
    auto dm = makeMinimalDataManager();

    // tokenId == 0 returns "" via the first early-exit (line 166)
    EXPECT_EQ(dm->resolveTokenName(0), "");

    // tokenId != 0 with null registry must return "" via line 169
    EXPECT_EQ(dm->resolveTokenName(42), "");
    EXPECT_EQ(dm->resolveTokenName(1), "");
}

// ============================================================================
// markDeletionPerformed: deleteOperationPerformedFlag_ is null (line 297)
// ============================================================================

TEST(DataManagerInitialization, MarkDeletionPerformed_NullFlag_NoOp) {
    // DataManager not constructed via FileStorage, so
    // deleteOperationPerformedFlag_ is never set (remains nullptr).
    auto dm = makeMinimalDataManager(/*useNullStorageIO=*/false);

    // Should not crash — the null guard returns without storing.
    EXPECT_NO_THROW(dm->markDeletionPerformed());
}

// ============================================================================
// bulkLoadEntities: returns empty when hasPreadSupport() is false (line 1192)
// ============================================================================

TEST(DataManagerInitialization, BulkLoadEntities_NoPread_ReturnsEmpty) {
    auto dm = makeMinimalDataManager(/*useNullStorageIO=*/false);
    EXPECT_FALSE(dm->hasPreadSupport());

    auto nodes = dm->bulkLoadEntities<graph::Node>(1, 100);
    EXPECT_TRUE(nodes.empty());

    auto edges = dm->bulkLoadEntities<graph::Edge>(1, 100);
    EXPECT_TRUE(edges.empty());

    auto props = dm->bulkLoadEntities<graph::Property>(1, 100);
    EXPECT_TRUE(props.empty());
}

// ============================================================================
// bulkLoadPropertyEntities: returns empty when hasPreadSupport() is false
// ============================================================================

TEST(DataManagerInitialization, BulkLoadPropertyEntities_NoPread_ReturnsEmpty) {
    auto dm = makeMinimalDataManager(/*useNullStorageIO=*/false);
    EXPECT_FALSE(dm->hasPreadSupport());

    std::vector<int64_t> ids = {1, 2, 3};
    auto result = dm->bulkLoadPropertyEntities(ids, nullptr);
    EXPECT_TRUE(result.empty());
}

// ============================================================================
// bulkLoadPropertyEntities: returns empty when ids is empty
// ============================================================================

TEST(DataManagerInitialization, BulkLoadPropertyEntities_EmptyIds_ReturnsEmpty) {
    auto dm = makeMinimalDataManager(/*useNullStorageIO=*/false);

    std::vector<int64_t> empty;
    auto result = dm->bulkLoadPropertyEntities(empty, nullptr);
    EXPECT_TRUE(result.empty());
}
