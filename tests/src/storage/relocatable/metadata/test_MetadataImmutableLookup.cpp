/**
 * @file test_MetadataImmutableLookup.cpp
 * @brief Production metadata lookups borrow fixed page payloads under a pin.
 * Copyright (c) 2026 Nexepic. Licensed under the Apache License, Version 2.0.
 */
#include "storage/relocatable/metadata/RelocatableMetadataStoreTestFixture.hpp"
#include "graph/storage/relocatable/metadata/MetadataPageReadContext.hpp"
#include "graph/debug/PerfTrace.hpp"

TEST_F(RelocatableMetadataStoreTest, WarmCatalogLookupBorrowsRadixAndBootstrapAcrossOldSnapshotPublication) {
    auto store = RelocatableMetadataStore::initializeNewComplete(io, superblock(1U, 1U));
    auto entries = catalogEntriesForLeafCount(300U);
    static_cast<void>(store->upsertCatalog(entries, LogSequenceNumber(3U)));
    auto snapshot = store->openSnapshot();
    const auto expected = entries.back();
    EXPECT_EQ(store->findCatalog(snapshot, expected.segment_id).physical_slot, expected.physical_slot);
    graph::debug::PerfTrace::setValueCollectionEnabled(true); graph::debug::PerfTrace::reset();
    for (size_t i = 0; i < 100U; ++i)
        EXPECT_EQ(store->findCatalog(snapshot, expected.segment_id).physical_slot, expected.physical_slot);
    const auto counters = graph::debug::PerfTrace::snapshotAndReset();
    graph::debug::PerfTrace::setValueCollectionEnabled(false);
    ASSERT_TRUE(counters.contains("native_graph.read.decoded_metadata_borrow_fixed_bytes"));
    EXPECT_GT(counters.at("native_graph.read.decoded_metadata_borrow_fixed_bytes").totalValue, 100U * sizeof(MetadataRadixPage));
    // Small cached route results are copied out; page payloads must be borrowed.
    const auto copies = counters.find("native_graph.read.decoded_metadata_copy_fixed_bytes");
    EXPECT_LE(copies == counters.end() ? 0 : copies->second.totalValue, 100U * 128U);
    RecordProperty("copied_fixed_bytes", std::to_string(copies == counters.end() ? 0 : copies->second.totalValue));
    RecordProperty("borrowed_fixed_bytes", std::to_string(counters.at("native_graph.read.decoded_metadata_borrow_fixed_bytes").totalValue));
    EXPECT_LE(snapshot.readContext()->chargedBytes(), MetadataPageReadContext::MAXIMUM_CHARGED_BYTES);
    auto replacement = expected; replacement.physical_slot = PhysicalSlotId(10000U);
    replacement.committed_lsn = LogSequenceNumber(4U);
    static_cast<void>(store->upsertCatalog(std::span(&replacement, 1U), LogSequenceNumber(4U)));
    EXPECT_EQ(store->findCatalog(snapshot, expected.segment_id).physical_slot, expected.physical_slot);
    auto current = store->openSnapshot();
    EXPECT_EQ(store->findCatalog(current, expected.segment_id).physical_slot, replacement.physical_slot);
}
