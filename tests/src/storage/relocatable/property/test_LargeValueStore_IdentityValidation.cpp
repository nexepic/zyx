/**
 * @file test_LargeValueStore_IdentityValidation.cpp
 * @brief LargeValue logical-identity boundary tests.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "storage/relocatable/metadata/RelocatableMetadataStoreTestFixture.hpp"

#include <array>

#include "graph/storage/relocatable/property/LargeValueStore.hpp"

namespace {

	[[nodiscard]] LargeValueWriteOptions largeValueWriteOptions() {
		LargeValueWriteOptions result;
		result.first_data_slot = PhysicalSlotId(40U);
		result.last_data_slot = PhysicalSlotId(80U);
		result.first_manifest_slot = PhysicalSlotId(81U);
		result.last_manifest_slot = PhysicalSlotId(90U);
		result.kind = LargeValueKind::LVK_BYTES;
		result.media_type = "application/octet-stream";
		return result;
	}

} // namespace

TEST_F(RelocatableMetadataStoreTest, LargeValueStoreRejectsWritesAfterDurableIdentityExhaustion) {
	Superblock exhausted = superblock(1U, 1U);
	// Zero is the persisted representation for an exhausted, never-reused
	// LargeValue identity namespace.
	exhausted.next_large_value_id = 0U;
	auto metadata = RelocatableMetadataStore::initializeNew(io, exhausted);
	LargeValueStore store(io, *metadata);
	const std::array input{std::byte{0x42U}};
	MetadataSnapshotPin before = metadata->openSnapshot();
	const Superblock beforeSuperblock = before.superblock();
	before.release();

	expectFormatError(
			[&] { static_cast<void>(store.write(input, largeValueWriteOptions(), LogSequenceNumber(2U))); },
			FormatError::FERR_ARITHMETIC_OVERFLOW);

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(snapshot.superblock(), beforeSuperblock);
	EXPECT_FALSE(metadata->findLargeValueManifestOptional(snapshot, LargeValueId(1U)).has_value());
}
