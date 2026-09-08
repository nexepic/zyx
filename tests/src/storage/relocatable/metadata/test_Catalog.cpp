/**
 * @file test_Catalog.cpp
 * @brief Lifecycle, relocation, and reuse tests for the isolated relocatable catalog.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <utility>

#include <gtest/gtest.h>

#include "graph/storage/relocatable/metadata/Catalog.hpp"

using namespace graph::storage;

namespace {

	void expectFormatError(const std::function<void()> &operation, FormatError expected) {
		try {
			operation();
			FAIL() << "Expected FormatException";
		} catch (const FormatException &error) {
			EXPECT_EQ(error.error(), expected);
		}
	}

	CatalogEntry allocateAndPublish(SegmentCatalog &catalog, SegmentKind kind, uint64_t ownerId, uint64_t lsn) {
		const CatalogEntry building = catalog.allocate(kind, ownerId);
		catalog.publish(building.segment_id, building.generation, LogSequenceNumber(lsn));
		return catalog.find(building.segment_id);
	}

	constexpr std::array<SegmentKind, 13> ALL_KINDS = {
			SegmentKind::SGK_ENTITY_PAGE_POOL,		SegmentKind::SGK_PROPERTY_PAGE_POOL,
			SegmentKind::SGK_INDEX_PAGE_POOL,	SegmentKind::SGK_VECTOR_RAW_POOL,
			SegmentKind::SGK_VECTOR_PQ_POOL,	SegmentKind::SGK_VECTOR_ADJACENCY_POOL,
			SegmentKind::SGK_CODEBOOK_EXTENT,
			SegmentKind::SGK_METADATA_PAGE_POOL,			SegmentKind::SGK_VECTOR_MANIFEST_POOL,
			SegmentKind::SGK_STATE_PAGE_POOL,		SegmentKind::SGK_LARGE_VALUE_DATA_POOL,
			SegmentKind::SGK_LARGE_VALUE_MANIFEST_POOL,
			SegmentKind::SGK_BULK_SCRATCH,
	};

} // namespace

TEST(RelocatableCatalogTest, AllocationPublishRetirementAndTypeIndependentReuse) {
	SegmentCatalog catalog;
	const CatalogEntry building = catalog.allocate(SegmentKind::SGK_ENTITY_PAGE_POOL, 42);
	EXPECT_TRUE(building.segment_id.isValid());
	EXPECT_TRUE(building.physical_slot.isValid());
	EXPECT_EQ(building.generation.value(), 1u);
	EXPECT_EQ(building.state, SegmentState::SGS_BUILDING);
	EXPECT_EQ(catalog.stats().reserved_slots, 1u);

	catalog.publish(building.segment_id, building.generation, LogSequenceNumber(4));
	const CatalogEntry sealed = catalog.find(building.segment_id);
	EXPECT_EQ(sealed.state, SegmentState::SGS_SEALED);
	EXPECT_EQ(sealed.committed_lsn.value(), 4u);
	EXPECT_EQ(catalog.stats().live_slots, 1u);

	auto pin = catalog.pin(sealed.segment_id);
	EXPECT_EQ(pin.entry().physical_slot, sealed.physical_slot);
	catalog.retire(sealed.segment_id, sealed.generation, LogSequenceNumber(5));
	EXPECT_EQ(catalog.find(sealed.segment_id).state, SegmentState::SGS_RETIRED);
	EXPECT_EQ(catalog.collectGarbage(), 0u);

	pin.release();
	EXPECT_EQ(catalog.collectGarbage(), 1u);
	expectFormatError([&] { static_cast<void>(catalog.find(sealed.segment_id)); }, FormatError::FERR_INVALID_HEADER);

	const CatalogEntry reused = allocateAndPublish(catalog, SegmentKind::SGK_VECTOR_PQ_POOL, 99, 6);
	EXPECT_EQ(reused.physical_slot, sealed.physical_slot);
	EXPECT_EQ(reused.kind, SegmentKind::SGK_VECTOR_PQ_POOL);
	EXPECT_EQ(catalog.stats().free_slots, 0u);
}

TEST(RelocatableCatalogTest, RelocationPublishesOnlyAfterCopyAndDefersSourceReuse) {
	SegmentCatalog catalog;
	const CatalogEntry source = allocateAndPublish(catalog, SegmentKind::SGK_ENTITY_PAGE_POOL, 7, 5);
	std::map<uint64_t, std::array<std::byte, 16>> syntheticBytes;
	for (size_t index = 0; index < syntheticBytes[source.physical_slot.value()].size(); ++index) {
		syntheticBytes[source.physical_slot.value()][index] = static_cast<std::byte>(index + 1U);
	}

	auto oldPin = catalog.pin(source.segment_id);
	auto oldSnapshot = catalog.openSnapshot(LogSequenceNumber(5));
	RelocationPlan plan = catalog.beginRelocation(source.segment_id, source.generation);
	EXPECT_NE(plan.destination_slot, source.physical_slot);
	expectFormatError([&] { catalog.publishRelocation(plan, LogSequenceNumber(10)); }, FormatError::FERR_INVALID_STATE);
	catalog.abortRelocation(plan);

	plan = catalog.beginRelocation(source.segment_id, source.generation);
	syntheticBytes[plan.destination_slot.value()] = syntheticBytes.at(plan.source_slot.value());
	catalog.markRelocationCopied(plan);
	catalog.abortRelocation(plan);

	plan = catalog.beginRelocation(source.segment_id, source.generation);
	syntheticBytes[plan.destination_slot.value()] = syntheticBytes.at(plan.source_slot.value());
	catalog.markRelocationCopied(plan);
	catalog.publishRelocation(plan, LogSequenceNumber(10));

	const CatalogEntry relocated = catalog.find(source.segment_id);
	EXPECT_EQ(relocated.physical_slot, plan.destination_slot);
	EXPECT_EQ(relocated.generation, Generation(2));
	EXPECT_EQ(relocated.state, SegmentState::SGS_SEALED);
	EXPECT_EQ(relocated.committed_lsn, LogSequenceNumber(10));
	EXPECT_EQ(syntheticBytes.at(relocated.physical_slot.value()), syntheticBytes.at(source.physical_slot.value()));
	EXPECT_EQ(catalog.collectGarbage(), 0u);

	oldPin.release();
	EXPECT_EQ(catalog.collectGarbage(), 0u);
	oldSnapshot.release();
	EXPECT_EQ(catalog.collectGarbage(), 1u);
	EXPECT_EQ(catalog.stats().live_slots, 1u);
	EXPECT_EQ(catalog.stats().free_slots, 1u);
}

TEST(RelocatableCatalogTest, EverySyntheticSubtypeRelocatesIntoEveryOtherSubtypeHole) {
	for (const SegmentKind targetKind: ALL_KINDS) {
		for (const SegmentKind holeKind: ALL_KINDS) {
			SegmentCatalog catalog;
			const CatalogEntry target = allocateAndPublish(catalog, targetKind, 1, 1);
			const CatalogEntry hole = allocateAndPublish(catalog, holeKind, 2, 2);
			catalog.retire(hole.segment_id, hole.generation, LogSequenceNumber(3));
			EXPECT_EQ(catalog.collectGarbage(), 1u)
					<< static_cast<uint16_t>(targetKind) << ':' << static_cast<uint16_t>(holeKind);

			const RelocationPlan plan = catalog.beginRelocation(target.segment_id, target.generation);
			EXPECT_EQ(plan.destination_slot, hole.physical_slot);
			catalog.markRelocationCopied(plan);
			catalog.publishRelocation(plan, LogSequenceNumber(4));
			EXPECT_EQ(catalog.find(target.segment_id).physical_slot, hole.physical_slot);
			EXPECT_EQ(catalog.collectGarbage(), 1u);
		}
	}
}

TEST(RelocatableCatalogTest, RecoveryDiscardsUnpublishedAndRetiredPhysicalSlots) {
	SegmentCatalog catalog;
	const CatalogEntry building = catalog.allocate(SegmentKind::SGK_PROPERTY_PAGE_POOL, 1);
	const CatalogEntry sealed = allocateAndPublish(catalog, SegmentKind::SGK_INDEX_PAGE_POOL, 2, 2);
	static_cast<void>(catalog.beginRelocation(sealed.segment_id, sealed.generation));
	EXPECT_EQ(catalog.stats().reserved_slots, 2u);

	auto pin = catalog.pin(sealed.segment_id);
	expectFormatError([&] { catalog.recover(); }, FormatError::FERR_INVALID_STATE);
	pin.release();
	auto snapshot = catalog.openSnapshot(LogSequenceNumber(2));
	expectFormatError([&] { catalog.recover(); }, FormatError::FERR_INVALID_STATE);
	snapshot.release();
	catalog.recover();
	expectFormatError([&] { static_cast<void>(catalog.find(building.segment_id)); }, FormatError::FERR_INVALID_HEADER);
	EXPECT_EQ(catalog.find(sealed.segment_id).physical_slot, sealed.physical_slot);
	EXPECT_EQ(catalog.stats().pending_relocations, 0u);
	EXPECT_EQ(catalog.stats().free_slots, 2u);

	const RelocationPlan relocation = catalog.beginRelocation(sealed.segment_id, sealed.generation);
	catalog.markRelocationCopied(relocation);
	catalog.publishRelocation(relocation, LogSequenceNumber(3));
	catalog.recover();
	EXPECT_EQ(catalog.find(sealed.segment_id).physical_slot, relocation.destination_slot);
	EXPECT_EQ(catalog.stats().free_slots, 2u);

	const CatalogEntry relocated = catalog.find(sealed.segment_id);
	catalog.retire(relocated.segment_id, relocated.generation, LogSequenceNumber(4));
	catalog.recover();
	expectFormatError([&] { static_cast<void>(catalog.find(sealed.segment_id)); }, FormatError::FERR_INVALID_HEADER);
	EXPECT_EQ(catalog.stats().free_slots, 3u);
	EXPECT_EQ(catalog.stats().retired_slots, 0u);
}

TEST(RelocatableCatalogTest, RejectsInvalidOrStaleLifecycleRequests) {
	SegmentCatalog catalog;
	expectFormatError([&] { static_cast<void>(catalog.allocate(SegmentKind::SGK_NONE, 1)); },
					  FormatError::FERR_INVALID_ENUM);
	expectFormatError(
			[&] {
				static_cast<void>(catalog.allocate(
						static_cast<SegmentKind>(static_cast<uint16_t>(SegmentKind::SGK_BULK_SCRATCH) + 1U),
						1));
			},
					  FormatError::FERR_INVALID_ENUM);
	expectFormatError([&] { static_cast<void>(catalog.openSnapshot(LogSequenceNumber{})); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(catalog.find(SegmentId(99))); }, FormatError::FERR_INVALID_HEADER);

	const CatalogEntry building = catalog.allocate(SegmentKind::SGK_ENTITY_PAGE_POOL, 1);
	expectFormatError([&] { static_cast<void>(catalog.pin(building.segment_id)); }, FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { catalog.retire(building.segment_id, building.generation, LogSequenceNumber(1)); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(catalog.beginRelocation(building.segment_id, building.generation)); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { catalog.publish(building.segment_id, building.generation, LogSequenceNumber{}); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { catalog.publish(building.segment_id, Generation(2), LogSequenceNumber(1)); },
					  FormatError::FERR_INVALID_STATE);
	catalog.publish(building.segment_id, building.generation, LogSequenceNumber(1));
	expectFormatError([&] { catalog.publish(building.segment_id, building.generation, LogSequenceNumber(2)); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { catalog.retire(building.segment_id, building.generation, LogSequenceNumber{}); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { catalog.retire(building.segment_id, building.generation, LogSequenceNumber(1)); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(catalog.beginRelocation(building.segment_id, Generation(2))); },
					  FormatError::FERR_INVALID_STATE);

	const RelocationPlan plan = catalog.beginRelocation(building.segment_id, building.generation);
	expectFormatError([&] { static_cast<void>(catalog.beginRelocation(building.segment_id, building.generation)); },
					  FormatError::FERR_INVALID_STATE);
	RelocationPlan wrongPlan = plan;
	++wrongPlan.relocation_id;
	expectFormatError([&] { catalog.markRelocationCopied(wrongPlan); }, FormatError::FERR_INVALID_STATE);
	wrongPlan = plan;
	wrongPlan.segment_id = SegmentId(plan.segment_id.value() + 1U);
	expectFormatError([&] { catalog.markRelocationCopied(wrongPlan); }, FormatError::FERR_INVALID_STATE);
	wrongPlan = plan;
	wrongPlan.source_slot = PhysicalSlotId(plan.source_slot.value() + 1U);
	expectFormatError([&] { catalog.markRelocationCopied(wrongPlan); }, FormatError::FERR_INVALID_STATE);
	wrongPlan = plan;
	wrongPlan.destination_slot = PhysicalSlotId(plan.destination_slot.value() + 1U);
	expectFormatError([&] { catalog.markRelocationCopied(wrongPlan); }, FormatError::FERR_INVALID_STATE);
	wrongPlan = plan;
	wrongPlan.source_generation = Generation(plan.source_generation.value() + 1U);
	expectFormatError([&] { catalog.markRelocationCopied(wrongPlan); }, FormatError::FERR_INVALID_STATE);
	wrongPlan = plan;
	wrongPlan.destination_generation = Generation(plan.destination_generation.value() + 1U);
	expectFormatError([&] { catalog.markRelocationCopied(wrongPlan); }, FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { catalog.retire(building.segment_id, building.generation, LogSequenceNumber(2)); },
					  FormatError::FERR_INVALID_STATE);
	catalog.markRelocationCopied(plan);
	expectFormatError([&] { catalog.markRelocationCopied(plan); }, FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { catalog.publishRelocation(plan, LogSequenceNumber{}); }, FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { catalog.publishRelocation(plan, LogSequenceNumber(1)); }, FormatError::FERR_INVALID_STATE);
	catalog.abortRelocation(plan);
	expectFormatError([&] { catalog.abortRelocation(plan); }, FormatError::FERR_INVALID_STATE);

	auto pin = catalog.pin(building.segment_id);
	SegmentPin movedPin = std::move(pin);
	EXPECT_FALSE(pin.isValid());
	EXPECT_TRUE(movedPin.isValid());
	SnapshotPin snapshot = catalog.openSnapshot(LogSequenceNumber(1));
	SnapshotPin movedSnapshot = std::move(snapshot);
	EXPECT_FALSE(snapshot.isValid());
	EXPECT_TRUE(movedSnapshot.isValid());
	EXPECT_EQ(movedSnapshot.lsn(), LogSequenceNumber(1));

	catalog.retire(building.segment_id, building.generation, LogSequenceNumber(3));
	expectFormatError([&] { catalog.retire(building.segment_id, building.generation, LogSequenceNumber(4)); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(catalog.beginRelocation(building.segment_id, building.generation)); },
					  FormatError::FERR_INVALID_STATE);
}

TEST(RelocatableCatalogTest, EnforcesConfiguredLimitsWithoutLeakingReservations) {
	SegmentCatalogLimits limits;
	limits.maximum_physical_slots = 1;
	SegmentCatalog physicalSlotLimited(limits);
	const CatalogEntry first = physicalSlotLimited.allocate(SegmentKind::SGK_ENTITY_PAGE_POOL, 1);
	expectFormatError([&] { static_cast<void>(physicalSlotLimited.allocate(SegmentKind::SGK_PROPERTY_PAGE_POOL, 2)); },
					  FormatError::FERR_ARITHMETIC_OVERFLOW);
	EXPECT_EQ(physicalSlotLimited.stats().total_slots, 1u);
	EXPECT_EQ(physicalSlotLimited.stats().reserved_slots, 1u);
	physicalSlotLimited.recover();
	EXPECT_EQ(physicalSlotLimited.allocate(SegmentKind::SGK_PROPERTY_PAGE_POOL, 2).physical_slot, first.physical_slot);

	limits = {};
	limits.maximum_segment_id = 1;
	SegmentCatalog segmentIdLimited(limits);
	static_cast<void>(segmentIdLimited.allocate(SegmentKind::SGK_ENTITY_PAGE_POOL, 1));
	expectFormatError([&] { static_cast<void>(segmentIdLimited.allocate(SegmentKind::SGK_PROPERTY_PAGE_POOL, 2)); },
					  FormatError::FERR_ARITHMETIC_OVERFLOW);

	limits = {};
	limits.maximum_generation = 0;
	SegmentCatalog emptyGenerationRange(limits);
	expectFormatError([&] { static_cast<void>(emptyGenerationRange.allocate(SegmentKind::SGK_ENTITY_PAGE_POOL, 1)); },
					  FormatError::FERR_ARITHMETIC_OVERFLOW);
	EXPECT_EQ(emptyGenerationRange.stats().total_slots, 0u);

	limits.maximum_generation = 1;
	SegmentCatalog generationLimited(limits);
	const CatalogEntry sealed = allocateAndPublish(generationLimited, SegmentKind::SGK_ENTITY_PAGE_POOL, 1, 1);
	expectFormatError(
			[&] { static_cast<void>(generationLimited.beginRelocation(sealed.segment_id, sealed.generation)); },
			FormatError::FERR_ARITHMETIC_OVERFLOW);
	EXPECT_EQ(generationLimited.stats().reserved_slots, 0u);

	limits = {};
	limits.maximum_relocation_id = 1;
	SegmentCatalog relocationIdLimited(limits);
	const CatalogEntry relocationSource = allocateAndPublish(relocationIdLimited, SegmentKind::SGK_ENTITY_PAGE_POOL, 1, 1);
	const RelocationPlan plan =
			relocationIdLimited.beginRelocation(relocationSource.segment_id, relocationSource.generation);
	relocationIdLimited.abortRelocation(plan);
	expectFormatError(
			[&] {
				static_cast<void>(
						relocationIdLimited.beginRelocation(relocationSource.segment_id, relocationSource.generation));
			},
			FormatError::FERR_ARITHMETIC_OVERFLOW);
	EXPECT_EQ(relocationIdLimited.stats().reserved_slots, 0u);
}

TEST(RelocatableCatalogTest, PinsAndSnapshotsSupportMoveAssignmentAndSharedReferences) {
	SegmentCatalog catalog;
	const CatalogEntry sealed = allocateAndPublish(catalog, SegmentKind::SGK_ENTITY_PAGE_POOL, 1, 1);

	SegmentPin firstPin = catalog.pin(sealed.segment_id);
	SegmentPin secondPin = catalog.pin(sealed.segment_id);
	firstPin = std::move(secondPin);
	EXPECT_TRUE(firstPin.isValid());
	EXPECT_FALSE(secondPin.isValid());
	firstPin.release();

	SnapshotPin firstSnapshot = catalog.openSnapshot(LogSequenceNumber(1));
	SnapshotPin secondSnapshot = catalog.openSnapshot(LogSequenceNumber(1));
	firstSnapshot = std::move(secondSnapshot);
	EXPECT_TRUE(firstSnapshot.isValid());
	EXPECT_FALSE(secondSnapshot.isValid());
	firstSnapshot.release();
}
