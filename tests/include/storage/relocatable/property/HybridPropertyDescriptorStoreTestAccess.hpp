#pragma once
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/metadata/MetadataDirectoryEvacuation.hpp"
#include <gtest/gtest.h>
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
    // Isolated read fixture only: a real, empty durable descriptor and valid
    // feature flag. Candidate nonempty routing still comes from TestAccess.
    static Superblock enableCompactReadFixture(RelocatableMetadataStore &store) {
        auto candidate = publish(store);
        candidate.generation = Generation(candidate.generation.value() + 1);
        candidate.checkpoint_lsn = LogSequenceNumber(candidate.checkpoint_lsn.value() + 1);
        candidate.flags |= RSS_SUPERBLOCK_FLAG_HYBRID_COLUMN_MEMBERSHIP;
        store.bootstrap_.publish(candidate);
        return candidate;
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
}
