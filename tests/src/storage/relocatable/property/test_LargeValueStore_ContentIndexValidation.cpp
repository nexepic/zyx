/**
 * @file test_LargeValueStore_ContentIndexValidation.cpp
 * @brief LargeValue content-index public input validation tests.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "storage/relocatable/metadata/RelocatableMetadataStoreTestFixture.hpp"

#include <array>

#include "graph/storage/relocatable/property/LargeValueStore.hpp"

TEST_F(RelocatableMetadataStoreTest, LargeValueContentIndexPlannerRejectsAnInvalidLastAllocationSlot) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore store(io, *metadata);
	const std::array input{std::byte{0x42U}};
	const LargeValueRef reference = store.planReference(
			LargeValueId(1U), Generation(1U), input, LargeValueKind::LVK_BYTES, "application/octet-stream", {});
	const NativeGraphLargeValueMutation value{
			reference,
			{input.begin(), input.end()},
			PhysicalSlotId(40U),
			PhysicalSlotId(80U),
			PhysicalSlotId(81U),
			PhysicalSlotId(90U),
			{},
			LargeValueExtentClass::LVEC_32K,
			store.plannedDataPageCount(input, {}, LargeValueExtentClass::LVEC_32K),
			store.plannedManifestPageCount(input, {}, LargeValueExtentClass::LVEC_32K),
	};
	const std::array values{value};
	MetadataSnapshotPin snapshot = metadata->openSnapshot();

	expectFormatError(
			[&] {
				static_cast<void>(store.planNativeGraphContentIndexMutation(snapshot, values, PhysicalSlotId(40U),
																		 PhysicalSlotId{}));
			},
			FormatError::FERR_INVALID_HEADER);
	snapshot.release();
}
