#include <gtest/gtest.h>
#include <cerrno>
#include "graph/storage/PwriteHelper.hpp"
#include "query/api/LogicalProfileObservation.hpp"
#include "graph/storage/relocatable/property/HybridPropertyDescriptor.hpp"
#include "graph/storage/relocatable/backend/RelocatableStorageBackend.hpp"
#include "graph/storage/relocatable/entity/NativeGraphWritePlanner.hpp"
#include "graph/storage/relocatable/maintenance/RelocatableIntegrityScanner.hpp"
#include "graph/storage/relocatable/metadata/MetadataDirectoryEvacuation.hpp"
#include "storage/relocatable/entity/NativeGraphWritePlannerTestFixture.hpp"
#ifdef _WIN32
#include <windows.h>
#endif

namespace graph::storage {
// Exercises the metadata-owned publication, not a new standalone graph API.
class HybridPropertyDescriptorStoreTestAccess {
public:
    enum class Action { HDA_REPLACE, HDA_RELOCATE, HDA_CLEAR, HDA_CLONE };
    static Superblock publish(RelocatableMetadataStore &store, Action action = Action::HDA_REPLACE,
        const MetadataPublishOptions &options = {}) {
        auto writer = store.beginWrite();
        const auto current = store.bootstrap_.recover();
        MetadataWriteBatch batch(store.metadata_page_pool_, current,
            LogSequenceNumber(current.checkpoint_lsn.value() + 1));
        if (action == Action::HDA_CLEAR) batch.setHybridPropertyDescriptorRoot({});
        else if (action == Action::HDA_RELOCATE) store.pages_.prepareHybridPropertyDescriptorRelocation(batch, current);
        else if (action == Action::HDA_CLONE) cloneMetadataDirectoryRootsForEvacuation(store.pages_, batch, current);
        else store.pages_.prepareHybridPropertyDescriptor(batch, {current.database_id, batch.generation(), {}});
        store.publishBatch(batch, writer, options);
        return batch.candidate();
    }
    static HybridPropertyDescriptorPage read(RelocatableMetadataStore &store, const MetadataSnapshotPin &pin) {
        return store.pages_.readHybridPropertyDescriptor(pin.superblock().hybrid_property_descriptor_root, pin.superblock());
    }
    static void rejectUninstalledRoots(RelocatableMetadataStore &store) {
        auto writer = store.beginWrite(); const auto current = store.bootstrap_.recover();
        MetadataWriteBatch batch(store.metadata_page_pool_, current, LogSequenceNumber(current.checkpoint_lsn.value() + 1));
        HybridPropertyDescriptor descriptor{current.database_id, batch.generation(), {}};
        descriptor.roots[0] = {PageId(111), batch.generation()};
        EXPECT_THROW(store.pages_.prepareHybridPropertyDescriptor(batch, descriptor), FormatException);
        EXPECT_EQ(batch.allocatedPageCount(), 0U);
        EXPECT_EQ(batch.candidate().hybrid_property_descriptor_root, current.hybrid_property_descriptor_root);
    }
    static void overwriteForCorruptionTest(RelocatableMetadataStore &store, const Superblock &snapshot,
        const HybridPropertyDescriptorPage &page) {
        const auto bytes = HybridPropertyDescriptorCodec::encode(page);
        const auto reference = snapshot.hybrid_property_descriptor_root;
        store.io_->writeAt({store.pages_.pageOffset(reference), reference.generation.value()}, bytes.data(), bytes.size());
        store.io_->sync();
    }
};
} // namespace graph::storage
using namespace graph::storage;
namespace {
using Access = HybridPropertyDescriptorStoreTestAccess;
using Action = Access::Action;
#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
NativeGraphWritePlannerOptions options() {
    NativeGraphWritePlannerOptions result;
    result.first_slot = PhysicalSlotId(40); result.last_slot = PhysicalSlotId(80);
    result.entity_pool_owner_id = 901;
    result.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 902};
    return result;
}
#endif
class HybridPropertyDescriptorStoreTest : public graph::storage::test::NativeGraphWritePlannerTestFixture {
    file_handle_t native_handle_ = INVALID_FILE_HANDLE;
    void releaseStores() {
        vector_manifests_.reset(); vector_pages_.reset(); properties_.reset(); labels_.reset();
        btree_pages_.reset(); slotted_pages_.reset(); entities_.reset(); metadata_.reset(); io_.reset();
    }
protected:
    void useNativeIo() {
        releaseStores(); file_->flush(); native_handle_ = portable_open_rw(path_.string().c_str());
        ASSERT_NE(native_handle_, INVALID_FILE_HANDLE);
        io_ = std::make_shared<StorageIO>(file_, native_handle_, native_handle_); reopenNativeStores();
    }
    void TearDown() override {
        releaseStores();
        if (native_handle_ != INVALID_FILE_HANDLE) EXPECT_EQ(portable_close_rw(native_handle_), 0);
        graph::storage::test::NativeGraphWritePlannerTestFixture::TearDown();
    }
};

TEST_F(HybridPropertyDescriptorStoreTest, DefaultFormatCannotPublishCandidateRoots) {
    if constexpr (RSS_HYBRID_PROPERTY_FORMAT_ENABLED) GTEST_SKIP() << "checked by the default format build";
    const auto before = metadata_->openSnapshot().superblock(); const auto bytes = io_->fileSize();
    EXPECT_EQ(RSS_SUPERBLOCK_VERSION, 31); EXPECT_EQ(RSS_SUPERBLOCK_HEADER_BYTES, 336U);
    EXPECT_THROW(Access::publish(*metadata_), FormatException);
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->fileSize(), bytes);
    auto invalid = before; invalid.hybrid_property_descriptor_root = {PhysicalSlotId(2), 1, Generation(1)};
    EXPECT_THROW(SuperblockCodec::encode(invalid), FormatException);
}
TEST_F(HybridPropertyDescriptorStoreTest, FenceIncludesDescriptorEvenWhenOtherGraphRootsAreUnchanged) {
    auto before = metadata_->openSnapshot().superblock(); auto after = before;
    after.hybrid_property_descriptor_root = {PhysicalSlotId(2), 1, Generation(1)};
    const auto oldFence = RelocatableStorageBackend::makeLogicalGraphMutationFence(before);
    const auto newFence = RelocatableStorageBackend::makeLogicalGraphMutationFence(after);
    EXPECT_NE(oldFence, newFence);
    RecordProperty("graph_fence_format_token", std::to_string(newFence.format_token));
    RecordProperty("format_profile", std::to_string(RSS_FORMAT_VERSION));
    EXPECT_EQ(newFence.graph_state_tokens[21], 2U);
    EXPECT_EQ(newFence.graph_state_tokens[22], 1U);
    EXPECT_EQ(newFence.graph_state_tokens[23], 1U);
}
TEST_F(HybridPropertyDescriptorStoreTest, OtherFormatBootstrapWithItsOwnValidChecksumsIsRejected) {
    // Both documented layouts have identical root-free fields apart from
    // version, header width, reserve offset and checksums.
    Superblock current; current.database_id = DatabaseId(9);
    current.generation = Generation(1); current.checkpoint_lsn = LogSequenceNumber(1);
    auto bytes = SuperblockCodec::encode(current);
    const uint16_t foreignVersion = RSS_HYBRID_PROPERTY_FORMAT_ENABLED ? 31 : 32;
    const uint16_t foreignHeader = RSS_HYBRID_PROPERTY_FORMAT_ENABLED ? 336 : 352;
    LittleEndianCodec::writeU16(bytes, 4, foreignVersion);
    LittleEndianCodec::writeU16(bytes, 6, foreignHeader);
    LittleEndianCodec::writeU32(bytes, 112, graph::utils::calculateCrc(bytes.data() + foreignHeader, bytes.size() - foreignHeader));
    LittleEndianCodec::writeU32(bytes, 116, 0);
    LittleEndianCodec::writeU32(bytes, 116, graph::utils::calculateCrc(bytes.data(), foreignHeader));
    EXPECT_THROW(SuperblockCodec::decode(bytes), FormatException);
}
#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
TEST_F(HybridPropertyDescriptorStoreTest, NativePublicationRetainsOldPinAndReopensWithoutDuplicatingRowRouting) {
    const auto first = Access::publish(*metadata_); auto old = metadata_->openSnapshot();
    ASSERT_EQ(RSS_SUPERBLOCK_VERSION, 32); ASSERT_EQ(RSS_SUPERBLOCK_HEADER_BYTES, 352U);
    const auto bytes = SuperblockCodec::encode(first); EXPECT_EQ(SuperblockCodec::decode(bytes), first);
    EXPECT_EQ(LittleEndianCodec::readU64(bytes, 336), first.hybrid_property_descriptor_root.physical_slot.value());
    const auto oldDescriptor = Access::read(*metadata_, old);
    const auto second = Access::publish(*metadata_); auto fresh = metadata_->openSnapshot();
    EXPECT_NE(first.hybrid_property_descriptor_root, second.hybrid_property_descriptor_root);
    EXPECT_EQ(Access::read(*metadata_, old), oldDescriptor);
    EXPECT_EQ(Access::read(*metadata_, fresh).descriptor.semantic_generation, second.generation);
    EXPECT_EQ(first.entity_property_directory_root, second.entity_property_directory_root);
    EXPECT_NO_THROW(RelocatableIntegrityScanner(io_, *metadata_).scan());
    old.release(); fresh.release(); reopenNativeStores();
    auto recovered = metadata_->openSnapshot(); EXPECT_EQ(recovered.superblock(), second);
    EXPECT_EQ(Access::read(*metadata_, recovered).descriptor.semantic_generation, second.generation);
    auto readOnly = RelocatableMetadataStore::openReadOnly(io_); auto readPin = readOnly->openSnapshot();
    EXPECT_EQ(Access::read(*readOnly, readPin).descriptor.semantic_generation, second.generation);
    EXPECT_THROW(Access::publish(*readOnly), FormatException);
}
TEST_F(HybridPropertyDescriptorStoreTest, DescriptorRelocationAndConservativeDirectoryClonePreserveSemanticIdentity) {
    Access::publish(*metadata_); auto original = metadata_->openSnapshot();
    const auto descriptor = Access::read(*metadata_, original).descriptor;
    Access::publish(*metadata_, Action::HDA_RELOCATE); auto relocated = metadata_->openSnapshot();
    EXPECT_EQ(Access::read(*metadata_, relocated).descriptor, descriptor);
    EXPECT_NE(original.superblock().hybrid_property_descriptor_root, relocated.superblock().hybrid_property_descriptor_root);
    Access::publish(*metadata_, Action::HDA_CLONE); auto cloned = metadata_->openSnapshot();
    EXPECT_EQ(Access::read(*metadata_, cloned).descriptor, descriptor);
    EXPECT_EQ(Access::read(*metadata_, original).descriptor, descriptor);
    EXPECT_NO_THROW(RelocatableIntegrityScanner(io_, *metadata_).scan());
    original.release(); relocated.release(); cloned.release(); reopenNativeStores();
    auto recovered = metadata_->openSnapshot(); EXPECT_EQ(Access::read(*metadata_, recovered).descriptor, descriptor);
}
TEST_F(HybridPropertyDescriptorStoreTest, ClearRetainsPinnedDescriptorUntilReclamationAndReopenHasNoRoot) {
    Access::publish(*metadata_); auto old = metadata_->openSnapshot(); const auto original = Access::read(*metadata_, old);
    const auto cleared = Access::publish(*metadata_, Action::HDA_CLEAR);
    EXPECT_EQ(cleared.hybrid_property_descriptor_root, PhysicalPageRef{});
    metadata_->collectMetadataGarbage(); EXPECT_EQ(Access::read(*metadata_, old), original);
    old.release(); metadata_->collectMetadataGarbage(); reopenNativeStores();
    EXPECT_EQ(metadata_->openSnapshot().superblock().hybrid_property_descriptor_root, PhysicalPageRef{});
    EXPECT_NO_THROW(RelocatableIntegrityScanner(io_, *metadata_).scan());
}
TEST_F(HybridPropertyDescriptorStoreTest, EachBootstrapInterruptionRecoversOneCompleteDescriptorGeneration) {
    Access::publish(*metadata_);
    for (auto failure : {BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE,
        BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE, BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE}) {
        const auto before = metadata_->openSnapshot().superblock();
        MetadataPublishOptions publishOptions; publishOptions.bootstrap_failpoint = failure;
        EXPECT_THROW(Access::publish(*metadata_, Action::HDA_REPLACE, publishOptions), std::exception);
        reopenNativeStores(); auto recovered = metadata_->openSnapshot();
        const auto current = recovered.superblock();
        EXPECT_TRUE(current.generation == before.generation || current.generation.value() == before.generation.value() + 1);
        const auto page = Access::read(*metadata_, recovered);
        EXPECT_EQ(page.generation, current.hybrid_property_descriptor_root.generation);
        EXPECT_EQ(page.descriptor.semantic_generation, current.generation);
        EXPECT_NO_THROW(RelocatableIntegrityScanner(io_, *metadata_).scan());
    }
}
TEST_F(HybridPropertyDescriptorStoreTest, NonemptyUninstalledRootsRejectBeforeFrameAllocation) {
    const auto before = metadata_->openSnapshot().superblock(); const auto bytes = io_->fileSize();
    Access::rejectUninstalledRoots(*metadata_);
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->fileSize(), bytes);
}
TEST_F(HybridPropertyDescriptorStoreTest, DescriptorChangesCannotBeMisclassifiedAsIdOnlyOrBlindAdjacencyAdvance) {
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options());
    auto transaction = planner.beginTransaction(); const auto id = transaction.createNode({});
    Access::publish(*metadata_);
    EXPECT_FALSE(transaction.commit().committed());
    auto current = metadata_->openSnapshot(); EXPECT_FALSE(metadata_->findNodePageLocationOptional(current, id));
}
TEST_F(HybridPropertyDescriptorStoreTest, ChecksummedWrongIdentitiesAndUnsupportedRoutingRejectOnReadOnlyOpen) {
    Access::publish(*metadata_); auto snapshot = metadata_->openSnapshot();
    const auto superblock = snapshot.superblock(); const auto original = Access::read(*metadata_, snapshot);
    for (unsigned mutation = 0; mutation < 3; ++mutation) {
        auto damaged = original;
        if (mutation == 0) damaged.descriptor.database_id = DatabaseId(999);
        if (mutation == 1) damaged.generation = Generation(original.generation.value() + 1);
        if (mutation == 2) damaged.descriptor.roots[0] = {PageId(111), original.descriptor.semantic_generation};
        Access::overwriteForCorruptionTest(*metadata_, superblock, damaged);
        EXPECT_THROW(Access::read(*metadata_, snapshot), FormatException);
        const auto pins = metadata_->activeSnapshotCount();
        EXPECT_THROW(metadata_->openSnapshot(), FormatException);
        EXPECT_EQ(metadata_->activeSnapshotCount(), pins);
        io_->resetStats();
        EXPECT_THROW(RelocatableMetadataStore::openReadOnly(io_), FormatException);
        EXPECT_EQ(io_->stats().write_calls, 0U);
        Access::overwriteForCorruptionTest(*metadata_, superblock, original);
        EXPECT_EQ(Access::read(*metadata_, snapshot), original);
    }
}
TEST_F(HybridPropertyDescriptorStoreTest, SnapshotAuthenticationIsCapturedByTheChargedProfileRegistry) {
    Access::publish(*metadata_);
    graph::query::QueryContext context;
    graph::query::detail::LogicalProfileObservation observation(context, 1024 * 1024);
    auto snapshot = metadata_->openSnapshot();
    const auto report = observation.report();
    const auto &metrics = std::get<graph::PropertyValue::MapType>(report.getVariant());
    ASSERT_TRUE(metrics.contains("native_graph.read.metadata_decodes.hybrid_descriptor"));
    EXPECT_EQ(std::get<int64_t>(metrics.at("native_graph.read.metadata_decodes.hybrid_descriptor").getVariant()), 1);
}
TEST_F(HybridPropertyDescriptorStoreTest, NativeDiskFullDataFenceAndUncertainRootFenceRecoverCompleteState) {
    ASSERT_NO_FATAL_FAILURE(useNativeIo()); Access::publish(*metadata_);
    for (unsigned fault = 0; fault < 3; ++fault) {
        const auto before = metadata_->openSnapshot().superblock();
        if (fault == 0) {
            StorageIO::ScopedNativeWriteHookForTesting refusal([](file_handle_t fd, const void *data, size_t bytes, pwrite_off_t offset) {
                if (bytes == RSS_PAGE_SIZE && LittleEndianCodec::readU32(
                    {static_cast<const std::byte *>(data), bytes}, 0) == RSS_HYBRID_PROPERTY_DESCRIPTOR_FRAME_MAGIC) {
#ifdef _WIN32
                    SetLastError(ERROR_DISK_FULL);
#else
                    errno = ENOSPC;
#endif
                    return static_cast<pwrite_ssize_t>(-1);
                }
                return portable_pwrite(fd, data, bytes, offset);
            });
            EXPECT_THROW(Access::publish(*metadata_), std::exception);
        } else {
            unsigned calls = 0;
            StorageIO::ScopedNativeSyncHookForTesting refusal([&](file_handle_t fd) {
                if (calls++ == fault - 1) {
#ifdef _WIN32
                    SetLastError(ERROR_DISK_FULL);
#else
                    errno = ENOSPC;
#endif
                    return -1;
                }
                return portable_fsync(fd);
            });
            EXPECT_THROW(Access::publish(*metadata_), std::exception);
            EXPECT_GE(calls, fault);
        }
        reopenNativeStores(); auto recovered = metadata_->openSnapshot();
        const auto current = recovered.superblock();
        if (fault < 2) EXPECT_EQ(current, before);
        else EXPECT_TRUE(current.generation == before.generation || current.generation.value() == before.generation.value() + 1);
        EXPECT_EQ(Access::read(*metadata_, recovered).descriptor.semantic_generation, current.generation);
        EXPECT_NO_THROW(RelocatableIntegrityScanner(io_, *metadata_).scan());
        recovered.release(); EXPECT_NO_THROW(Access::publish(*metadata_));
    }
}
#endif
} // namespace
