/**
 * @file test_RelocatableMaintenanceService.cpp
 * @brief End-to-end typed maintenance execution tests for relocatable storage.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <thread>
#include <vector>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"
#include "graph/storage/relocatable/allocation/SharedPagePoolAllocator.hpp"
#include "graph/storage/relocatable/entity/EntityPageStore.hpp"
#include "graph/storage/relocatable/property/LargeValueStore.hpp"
#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"
#include "storage/relocatable/TestSlottedPageRecord.hpp"
#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/maintenance/RelocatableMaintenanceService.hpp"
#include "graph/storage/relocatable/vector/VectorCodebookStore.hpp"
#include "graph/storage/relocatable/vector/VectorManifestStore.hpp"
#include "graph/storage/relocatable/vector/VectorPageStore.hpp"

using namespace graph::storage;
using graph::storage::test::TestSlottedPageRecord;
using graph::storage::test::TestSlottedPageRecordCodec;

namespace {

	Superblock initialSuperblock() {
		Superblock result;
		result.database_id = DatabaseId(0x9080706050403020ULL);
		result.generation = Generation(1U);
		result.checkpoint_lsn = LogSequenceNumber(1U);
		return result;
	}

	PageOwnerHeader propertyOwner(PageId pageId, LogSequenceNumber lsn) {
		return {pageId, 99U, PageOwnerKind::POK_PROPERTY, 0U, RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, Generation(1U), lsn};
	}

	EntityPageNewPoolRequest entityPool(uint64_t segmentId, uint64_t ownerId, uint64_t pageId,
										 uint64_t physicalSlot) {
		return {.segment_id = SegmentId(segmentId),
				.pool_owner_id = ownerId,
				.generation = Generation(1U),
				.page_id = PageId(pageId),
				.first_slot = PhysicalSlotId(physicalSlot),
				.last_slot = PhysicalSlotId(physicalSlot)};
	}

	NodeEntityRecord entityNode(uint64_t /*firstOutEdge*/, uint64_t /*firstInEdge*/, uint64_t firstLabel = 7U) {
		NodeEntityRecord result;
		result.inline_label_ids = {firstLabel, firstLabel + 2U, firstLabel + 4U};
		result.inline_label_count = 3U;
		return result;
	}

	SlottedPagePoolAllocationRequest propertyRequest(SegmentId segmentId, PageId pageId) {
		SlottedPagePoolAllocationRequest result;
		result.segment_id = segmentId;
		result.pool_kind = SegmentKind::SGK_PROPERTY_PAGE_POOL;
		result.pool_owner_id = 99U;
		result.generation = Generation(1U);
		result.page_id = pageId;
		result.page_owner_id = 99U;
		result.page_owner_kind = PageOwnerKind::POK_PROPERTY;
		return result;
	}

	SlottedPagePoolAllocationRequest btreeRequest(SegmentId segmentId, PageId pageId, Generation generation,
													  uint64_t poolOwnerId) {
		SlottedPagePoolAllocationRequest result;
		result.segment_id = segmentId;
		result.pool_kind = SegmentKind::SGK_INDEX_PAGE_POOL;
		result.pool_owner_id = poolOwnerId;
		result.generation = generation;
		result.page_id = pageId;
		result.page_owner_id = 17U;
		result.page_owner_kind = PageOwnerKind::POK_INDEX;
		return result;
	}

	BTreePage btreePage(const SlottedPagePoolAllocationRequest &request, LogSequenceNumber lsn) {
		PageOwnerHeader owner{request.page_id,
							  request.page_owner_id,
							  request.page_owner_kind,
							  0U,
							  RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
							  request.generation,
							  lsn};
		BTreePageHeader header;
		header.index_id = owner.owner_id;
		header.page_id = owner.page_id;
		header.generation = owner.generation;
		return BTreePage::create(owner, header);
	}

	SharedPagePoolAllocationRequest vectorRequest(SegmentId segmentId, PageId pageId, Generation generation,
													 uint64_t poolOwnerId) {
		SharedPagePoolAllocationRequest result;
		result.segment_id = segmentId;
		result.pool_kind = SegmentKind::SGK_VECTOR_RAW_POOL;
		result.pool_owner_id = poolOwnerId;
		result.generation = generation;
		result.page_id = pageId;
		result.page_owner_id = 17U;
		result.page_owner_kind = PageOwnerKind::POK_VECTOR_RAW;
		return result;
	}

	VectorPage vectorPage(const SharedPagePoolAllocationRequest &request, LogSequenceNumber lsn) {
		VectorPageHeader header;
		header.index_id = request.page_owner_id;
		header.page_id = request.page_id;
		header.generation = request.generation;
		header.kind = VectorStorageKind::VSK_RAW;
		header.record_bytes = 16U;
		header.slot_capacity = 2U;
		header.bitmap_bytes = 1U;
		header.committed_lsn = lsn;
		PageOwnerHeader owner{header.page_id,
							  header.index_id,
							  PageOwnerKind::POK_VECTOR_RAW,
							  0U,
							  RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
							  header.generation,
							  lsn};
		return VectorPage::create(owner, header);
	}

	VectorManifest vectorManifest(uint64_t indexId, PageId pageId, Generation generation, LogSequenceNumber lsn) {
		VectorManifest result;
		result.index_id = indexId;
		result.page_id = pageId;
		result.generation = generation;
		result.committed_lsn = lsn;
		result.dimensions = 16U;
		result.entries = {{PageId(pageId.value() + 100U), VectorStorageKind::VSK_RAW, 3U}};
		return result;
	}

	PageOwnerHeader manifestOwner(const VectorManifest &manifest) {
		return {manifest.page_id,
				manifest.index_id,
				PageOwnerKind::POK_VECTOR_MANIFEST,
				0U,
				RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
				manifest.generation,
				manifest.committed_lsn};
	}

	SharedPagePoolAllocationRequest manifestRequest(const VectorManifest &manifest, SegmentId segmentId) {
		SharedPagePoolAllocationRequest result;
		result.segment_id = segmentId;
		result.pool_kind = SegmentKind::SGK_VECTOR_MANIFEST_POOL;
		result.pool_owner_id = manifest.index_id;
		result.generation = manifest.generation;
		result.page_id = manifest.page_id;
		result.page_owner_id = manifest.index_id;
		result.page_owner_kind = PageOwnerKind::POK_VECTOR_MANIFEST;
		return result;
	}

	LargeValueWriteOptions largeValueWriteOptions() {
		LargeValueWriteOptions result;
		result.first_data_slot = PhysicalSlotId(40U);
		result.last_data_slot = PhysicalSlotId(80U);
		result.first_manifest_slot = PhysicalSlotId(81U);
		result.last_manifest_slot = PhysicalSlotId(90U);
		result.kind = LargeValueKind::LVK_BYTES;
		result.media_type = "application/octet-stream";
		return result;
	}

	std::vector<std::byte> largeValueBytes(size_t count) {
		std::vector<std::byte> result(count);
		for (size_t index = 0U; index < result.size(); ++index) {
			result[index] = static_cast<std::byte>((index * 29U + 7U) & 0xFFU);
		}
		return result;
	}

	void allocateLargeValueDataDestination(const std::shared_ptr<StorageIO> &io, RelocatableMetadataStore &metadata,
											  const LargeValueRef &reference, SegmentId segmentId, PageId pageId,
											  Generation poolGeneration, PhysicalSlotId slot, LogSequenceNumber checkpointLsn) {
		SharedPagePoolAllocator pools(io, metadata);
		const PageOwnerHeader owner{pageId, reference.value_id.value(), PageOwnerKind::POK_LARGE_VALUE_DATA, 0U,
															 RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, poolGeneration, checkpointLsn};
		const LargeValueDataPage spare{reference.value_id, pageId, reference.generation, 0U, 0U, 1U, 1U, 0U,
															  {std::byte{0xE1U}}};
		SharedPagePoolAllocationRequest request;
		request.segment_id = segmentId;
		request.pool_kind = SegmentKind::SGK_LARGE_VALUE_DATA_POOL;
		request.pool_owner_id = RSS_LARGE_VALUE_SHARED_DATA_POOL_OWNER_ID;
		request.generation = poolGeneration;
		request.page_id = pageId;
		request.page_owner_id = reference.value_id.value();
		request.page_owner_kind = PageOwnerKind::POK_LARGE_VALUE_DATA;
		request.payload = LargeValueDataFrame::create(owner, spare).serialize();
		ASSERT_TRUE(pools.allocateNewPoolPage(request, slot, slot, checkpointLsn).has_value());
	}

	void allocateLargeValueManifestDestination(const std::shared_ptr<StorageIO> &io, RelocatableMetadataStore &metadata,
													  const LargeValueManifest &source, SegmentId segmentId, PageId pageId,
													  Generation poolGeneration, PhysicalSlotId slot, LogSequenceNumber checkpointLsn) {
		SharedPagePoolAllocator pools(io, metadata);
		LargeValueManifest spare = source;
		spare.page_id = pageId;
		const PageOwnerHeader owner{pageId, source.value_id.value(), PageOwnerKind::POK_LARGE_VALUE_MANIFEST, 0U,
															 RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, poolGeneration, checkpointLsn};
		SharedPagePoolAllocationRequest request;
		request.segment_id = segmentId;
		request.pool_kind = SegmentKind::SGK_LARGE_VALUE_MANIFEST_POOL;
		request.pool_owner_id = RSS_LARGE_VALUE_SHARED_MANIFEST_POOL_OWNER_ID;
		request.generation = poolGeneration;
		request.page_id = pageId;
		request.page_owner_id = source.value_id.value();
		request.page_owner_kind = PageOwnerKind::POK_LARGE_VALUE_MANIFEST;
		request.payload = LargeValueManifestPage::create(owner, spare).serialize();
		ASSERT_TRUE(pools.allocateNewPoolPage(request, slot, slot, checkpointLsn).has_value());
	}

	template<typename Predicate>
	bool waitUntil(Predicate predicate) {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (std::chrono::steady_clock::now() < deadline) {
			if (predicate()) {
				return true;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return predicate();
	}

} // namespace

class RelocatableMaintenanceServiceTest : public ::testing::Test {
protected:
	std::filesystem::path path_;
	std::filesystem::path wal_path_;
	std::shared_ptr<std::fstream> file_;
	std::shared_ptr<std::fstream> wal_file_;
	std::shared_ptr<StorageIO> io_;
	std::shared_ptr<StorageIO> wal_io_;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		path_ = std::filesystem::temp_directory_path() / ("relocatable_maintenance_service_" + to_string(uuid));
		wal_path_ = path_.string() + ".foreign";
		file_ = std::make_shared<std::fstream>(path_,
											   std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		wal_file_ = std::make_shared<std::fstream>(wal_path_,
												   std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(file_->is_open());
		ASSERT_TRUE(wal_file_->is_open());
		io_ = std::make_shared<StorageIO>(file_, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		wal_io_ = std::make_shared<StorageIO>(wal_file_, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	}

	void TearDown() override {
		io_.reset();
		wal_io_.reset();
		if (file_ && file_->is_open()) {
			file_->close();
		}
		if (wal_file_ && wal_file_->is_open()) {
			wal_file_->close();
		}
		std::error_code error;
		std::filesystem::remove(path_, error);
		std::filesystem::remove(wal_path_, error);
	}
};

TEST_F(RelocatableMaintenanceServiceTest, ExecutesTypedPropertyCompactionAndReleasesTheSource) {
	auto metadata = RelocatableMetadataStore::initializeNew(io_, initialSuperblock());
	PhysicalSegmentAllocator physical(io_, *metadata);
	SlottedPagePool pages(io_, *metadata);
	BTreePageStore btreePages(pages, *metadata);
	VectorPageStore vectorPages(io_, *metadata);
	VectorManifestStore vectorManifests(io_, *metadata);
	EntityPageStore entities(io_, *metadata);
		std::mutex publicationMutex;
	VectorCodebookStore codebooks(physical, *metadata);
	LargeValueStore largeValues(io_, *metadata);

	SlottedPage source = SlottedPage::create(propertyOwner(PageId(101U), LogSequenceNumber(2U)));
	ASSERT_TRUE(pages.allocatePage(propertyRequest(SegmentId(71U), PageId(101U)), source, PhysicalSlotId(40U),
								   PhysicalSlotId(40U), LogSequenceNumber(2U))
						.has_value());
	SlottedPage destination = SlottedPage::create(propertyOwner(PageId(200U), LogSequenceNumber(3U)));
	ASSERT_TRUE(pages.allocatePage(propertyRequest(SegmentId(72U), PageId(200U)), destination, PhysicalSlotId(41U),
								   PhysicalSlotId(41U), LogSequenceNumber(3U))
						.has_value());

	RelocatableMaintenanceOptions options;
	options.loop.minimum_cycle_interval = std::chrono::milliseconds(1);
	options.page.maximum_page_moves = 1U;
	options.page.maximum_rewrite_bytes = RSS_PAGE_SIZE;
	RelocatableMaintenanceService service(pages, btreePages, vectorPages, vectorManifests, entities, codebooks, largeValues,
									physical, *metadata, publicationMutex, options);
	MetadataSnapshotPin pinned = metadata->openSnapshot();
	service.start();
	EXPECT_EQ(service.enqueuePropertyPageCompaction(SegmentId(71U), SegmentId(72U), 1U), 1U);
	ASSERT_TRUE(waitUntil([&] { return service.metrics().loop.deferred_attempts >= 2U; }));
	EXPECT_EQ(service.metrics().loop.completed_tasks, 0U);
	pinned.release();
	ASSERT_TRUE(waitUntil([&] { return service.metrics().loop.completed_tasks == 1U; }));
	service.stop();

	const RelocatableMaintenanceMetrics metrics = service.metrics();
	EXPECT_EQ(metrics.property_pages.moved_pages, 1U);
	EXPECT_EQ(metrics.property_pages.runs_started, 1U);
	EXPECT_EQ(metrics.property_pages.source_release_deferrals, 1U);
	EXPECT_EQ(metrics.loop.rewritten_bytes, static_cast<uint64_t>(RSS_PAGE_SIZE));
	EXPECT_EQ(metrics.loop.reclaimed_physical_bytes, static_cast<uint64_t>(RSS_SEGMENT_SIZE));
	const MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(metadata->findCatalog(snapshot, SegmentId(71U)).state, SegmentState::SGS_OBSOLETE);
	EXPECT_EQ(metadata->findPageLocation(snapshot, PageId(101U)).segment_id, SegmentId(72U));
}

TEST_F(RelocatableMaintenanceServiceTest, BoundsSpaceDuringConcurrentReadersWritersAndMaintenance) {
	constexpr size_t PAGE_COUNT = 20U;
	constexpr size_t WRITER_ITERATIONS = 64U;
	std::array<TestSlottedPageRecord, PAGE_COUNT> expected{};
	std::array<SlottedSlotReference, PAGE_COUNT> slots{};
	{
		auto metadata = RelocatableMetadataStore::initializeNew(io_, initialSuperblock());
		PhysicalSegmentAllocator physical(io_, *metadata);
		SlottedPagePool pages(io_, *metadata);
		BTreePageStore btreePages(pages, *metadata);
		VectorPageStore vectorPages(io_, *metadata);
		VectorManifestStore vectorManifests(io_, *metadata);
		EntityPageStore entities(io_, *metadata);
		std::mutex publicationMutex;
		VectorCodebookStore codebooks(physical, *metadata);
		LargeValueStore largeValues(io_, *metadata);

		uint64_t nextLsn = 2U;
		for (size_t index = 0U; index < PAGE_COUNT; ++index) {
			const PageId pageId(101U + index);
			expected[index].logical_id = index + 1U;
			expected[index].payload = {std::byte{static_cast<uint8_t>(index)}, std::byte{0x5A}};
			SlottedPage page = SlottedPage::create(propertyOwner(pageId, LogSequenceNumber(nextLsn)));
			slots[index] = page.insert(TestSlottedPageRecordCodec::encode(expected[index]));
			if (index == 0U) {
				ASSERT_TRUE(pages.allocatePage(propertyRequest(SegmentId(71U), pageId), page, PhysicalSlotId(40U),
											   PhysicalSlotId(40U), LogSequenceNumber(nextLsn))
									.has_value());
			} else {
				SlottedPagePoolAppendRequest append;
				append.segment_id = SegmentId(71U);
				append.generation = Generation(1U);
				append.page_id = pageId;
				append.page_owner_id = 99U;
				append.page_owner_kind = PageOwnerKind::POK_PROPERTY;
				static_cast<void>(pages.appendPage(append, page, LogSequenceNumber(nextLsn)));
			}
			++nextLsn;
		}
		SlottedPage destination = SlottedPage::create(propertyOwner(PageId(200U), LogSequenceNumber(nextLsn)));
		ASSERT_TRUE(pages.allocatePage(propertyRequest(SegmentId(72U), PageId(200U)), destination, PhysicalSlotId(41U),
									   PhysicalSlotId(41U), LogSequenceNumber(nextLsn))
							.has_value());

		RelocatableMaintenanceOptions options;
		options.loop.minimum_cycle_interval = std::chrono::milliseconds(1);
		options.page.maximum_page_moves = 1U;
		options.page.maximum_rewrite_bytes = RSS_PAGE_SIZE;
		RelocatableMaintenanceService service(pages, btreePages, vectorPages, vectorManifests, entities, codebooks,
										largeValues, physical, *metadata, publicationMutex, options);
		std::atomic_bool stopReader{false};
		std::atomic_bool readerStarted{false};
		std::atomic_uint64_t maximumAllocatedSegments{2U};
		std::exception_ptr workerFailure;
		std::mutex failureMutex;
		const auto recordFailure = [&](std::exception_ptr failure) {
			std::lock_guard lock(failureMutex);
			if (!workerFailure) {
				workerFailure = std::move(failure);
			}
		};

		std::thread reader([&] {
			size_t iteration = 0U;
			try {
				while (!stopReader.load(std::memory_order_acquire)) {
					MetadataSnapshotPin snapshot = metadata->openSnapshot();
					readerStarted.store(true, std::memory_order_release);
					const size_t index = iteration++ % PAGE_COUNT;
					if (TestSlottedPageRecordCodec::decode(pages.read(snapshot, PageId(101U + index)).read(slots[index])) !=
						expected[index]) {
						throw std::runtime_error("maintenance stress reader observed a non-snapshot Property value");
					}
					snapshot.release();
					std::this_thread::yield();
				}
			} catch (...) {
				recordFailure(std::current_exception());
			}
		});
		if (!waitUntil([&] { return readerStarted.load(std::memory_order_acquire); })) {
			stopReader.store(true, std::memory_order_release);
			reader.join();
			FAIL() << "maintenance stress reader did not start";
			return;
		}

		service.start();
		for (size_t index = 0U; index < PAGE_COUNT; ++index) {
			static_cast<void>(service.enqueuePropertyPageCompaction(SegmentId(71U), SegmentId(72U), 1U));
		}
		std::thread writer([&] {
			try {
				for (size_t iteration = 0U; iteration < WRITER_ITERATIONS; ++iteration) {
					std::lock_guard publicationLock(publicationMutex);
					{
						MetadataSnapshotPin snapshot = metadata->openSnapshot();
						SlottedPage page = pages.read(snapshot, PageId(200U));
						const uint64_t currentLsn = snapshot.superblock().checkpoint_lsn.value();
						snapshot.release();
						if (currentLsn == std::numeric_limits<uint64_t>::max()) {
							throw std::overflow_error("maintenance stress exhausted its checkpoint LSN");
						}
						const std::array writerValue = {std::byte{static_cast<uint8_t>(iteration)}};
						static_cast<void>(page.insert(writerValue));
						static_cast<void>(pages.replacePage(page, LogSequenceNumber(currentLsn + 1U)));
						pages.reclaimRetiredFrames(SegmentId(72U));
					}
					MetadataSnapshotPin observation = metadata->openSnapshot();
					const uint64_t allocated =
							static_cast<uint64_t>(metadata->isPhysicalSlotAllocated(observation, PhysicalSlotId(40U))) +
							static_cast<uint64_t>(metadata->isPhysicalSlotAllocated(observation, PhysicalSlotId(41U)));
					observation.release();
					maximumAllocatedSegments.store(
							std::max(maximumAllocatedSegments.load(std::memory_order_relaxed), allocated),
							std::memory_order_relaxed);
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
			} catch (...) {
				recordFailure(std::current_exception());
			}
		});

		writer.join();
		if (!waitUntil([&] { return service.metrics().loop.completed_tasks == PAGE_COUNT; })) {
			stopReader.store(true, std::memory_order_release);
			reader.join();
			ASSERT_TRUE(waitUntil([&] { return service.metrics().loop.completed_tasks == PAGE_COUNT; }));
		} else {
			stopReader.store(true, std::memory_order_release);
			reader.join();
		}
		service.stop();
		ASSERT_FALSE(static_cast<bool>(workerFailure));
		EXPECT_LE(maximumAllocatedSegments.load(std::memory_order_relaxed), 2U);
		EXPECT_EQ(metadata->observeCompactionState().retainedGenerationCount(), 0U);
		EXPECT_EQ(service.metrics().property_pages.moved_pages, PAGE_COUNT);
	}

	auto reopened = RelocatableMetadataStore::open(io_);
	SlottedPagePool pages(io_, *reopened);
	MetadataSnapshotPin snapshot = reopened->openSnapshot();
	for (size_t index = 0U; index < PAGE_COUNT; ++index) {
		EXPECT_EQ(reopened->findPageLocation(snapshot, PageId(101U + index)).segment_id, SegmentId(72U));
		EXPECT_EQ(TestSlottedPageRecordCodec::decode(pages.read(snapshot, PageId(101U + index)).read(slots[index])),
				  expected[index]);
	}
	EXPECT_FALSE(reopened->isPhysicalSlotAllocated(snapshot, PhysicalSlotId(40U)));
	EXPECT_TRUE(reopened->isPhysicalSlotAllocated(snapshot, PhysicalSlotId(41U)));
}

TEST_F(RelocatableMaintenanceServiceTest, ExecutesTypedBTreeVectorAndManifestPageCompactions) {
	auto metadata = RelocatableMetadataStore::initializeNew(io_, initialSuperblock());
	PhysicalSegmentAllocator physical(io_, *metadata);
	SlottedPagePool pages(io_, *metadata);
	BTreePageStore btreePages(pages, *metadata);
	VectorPageStore vectorPages(io_, *metadata);
	VectorManifestStore vectorManifests(io_, *metadata);
	EntityPageStore entities(io_, *metadata);
		std::mutex publicationMutex;
	VectorCodebookStore codebooks(physical, *metadata);
	LargeValueStore largeValues(io_, *metadata);

	const SlottedPagePoolAllocationRequest btreeSource =
			btreeRequest(SegmentId(71U), PageId(101U), Generation(1U), 77U);
	ASSERT_TRUE(btreePages
					.allocate(btreeSource, btreePage(btreeSource, LogSequenceNumber(2U)), PhysicalSlotId(40U),
							  PhysicalSlotId(40U), LogSequenceNumber(2U))
					.has_value());
	const IndexRootLocation root{17U, btreeSource.page_id, btreeSource.generation};
	static_cast<void>(btreePages.publishRoots(std::span(&root, 1U), {}, LogSequenceNumber(3U)));
	const SlottedPagePoolAllocationRequest btreeDestination =
			btreeRequest(SegmentId(72U), PageId(102U), Generation(2U), 78U);
	ASSERT_TRUE(btreePages
					.allocate(btreeDestination, btreePage(btreeDestination, LogSequenceNumber(4U)), PhysicalSlotId(41U),
							  PhysicalSlotId(41U), LogSequenceNumber(4U))
					.has_value());

	const SharedPagePoolAllocationRequest vectorSource =
			vectorRequest(SegmentId(73U), PageId(201U), Generation(1U), 79U);
	VectorPage sourceVectorPage = vectorPage(vectorSource, LogSequenceNumber(5U));
	const std::vector<std::byte> vectorPayload(16U, std::byte{0x31});
	const uint16_t vectorSlot = sourceVectorPage.insert(vectorPayload);
	ASSERT_TRUE(vectorPages
					.allocate(vectorSource, sourceVectorPage, PhysicalSlotId(42U), PhysicalSlotId(42U),
							  LogSequenceNumber(5U))
					.has_value());
	const VectorLocation vectorLocation{
			700U, vectorSource.page_id, vectorSlot, sourceVectorPage.slotGeneration(vectorSlot)};
	static_cast<void>(metadata->upsertVectorLocations(std::span(&vectorLocation, 1U), {}, LogSequenceNumber(6U)));
	const SharedPagePoolAllocationRequest vectorDestination =
			vectorRequest(SegmentId(74U), PageId(202U), Generation(2U), 80U);
	ASSERT_TRUE(vectorPages
					.allocate(vectorDestination, vectorPage(vectorDestination, LogSequenceNumber(7U)), PhysicalSlotId(43U),
							  PhysicalSlotId(43U), LogSequenceNumber(7U))
					.has_value());

	const VectorManifest manifestSource = vectorManifest(17U, PageId(301U), Generation(1U), LogSequenceNumber(8U));
	ASSERT_TRUE(vectorManifests
					.allocate(manifestRequest(manifestSource, SegmentId(75U)),
							  VectorManifestPage::create(manifestOwner(manifestSource), manifestSource), PhysicalSlotId(44U),
							  PhysicalSlotId(44U), LogSequenceNumber(8U))
					.has_value());
	const VectorManifest manifestDestination =
			vectorManifest(18U, PageId(302U), Generation(2U), LogSequenceNumber(9U));
	ASSERT_TRUE(vectorManifests
					.allocate(manifestRequest(manifestDestination, SegmentId(76U)),
							  VectorManifestPage::create(manifestOwner(manifestDestination), manifestDestination),
							  PhysicalSlotId(45U), PhysicalSlotId(45U), LogSequenceNumber(9U))
					.has_value());

	RelocatableMaintenanceOptions options;
	options.loop.minimum_cycle_interval = std::chrono::milliseconds(1);
	options.page.maximum_page_moves = 1U;
	options.page.maximum_rewrite_bytes = RSS_PAGE_SIZE;
	RelocatableMaintenanceService service(pages, btreePages, vectorPages, vectorManifests, entities, codebooks, largeValues,
									physical, *metadata, publicationMutex, options);
	service.start();
	EXPECT_EQ(service.enqueueBTreePageCompaction(SegmentId(71U), SegmentId(72U), 1U), 1U);
	EXPECT_EQ(service.enqueueVectorPageCompaction(SegmentId(73U), SegmentId(74U), 1U), 2U);
	EXPECT_EQ(service.enqueueVectorManifestCompaction(SegmentId(75U), SegmentId(76U), 1U), 3U);
	ASSERT_TRUE(waitUntil([&] { return service.metrics().loop.completed_tasks == 3U; }));
	service.stop();

	const RelocatableMaintenanceMetrics metrics = service.metrics();
	EXPECT_EQ(metrics.btree_pages.moved_pages, 1U);
	EXPECT_EQ(metrics.vector_pages.moved_pages, 1U);
	EXPECT_EQ(metrics.vector_manifests.moved_pages, 1U);
	EXPECT_EQ(metrics.loop.rewritten_bytes, static_cast<uint64_t>(3U * RSS_PAGE_SIZE));
	EXPECT_EQ(metrics.loop.reclaimed_physical_bytes, static_cast<uint64_t>(3U * RSS_SEGMENT_SIZE));
	const MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(metadata->findIndexRoot(snapshot, 17U).root_generation, Generation(2U));
	EXPECT_EQ(metadata->findVectorLocation(snapshot, 700U), vectorLocation);
	EXPECT_EQ(vectorPages.read(snapshot, vectorSource.page_id).read(vectorSlot, vectorLocation.slot_generation), vectorPayload);
	EXPECT_EQ(vectorManifests.read(snapshot, 17U).manifest().generation, Generation(2U));
	EXPECT_EQ(metadata->findCatalog(snapshot, SegmentId(71U)).state, SegmentState::SGS_OBSOLETE);
	EXPECT_EQ(metadata->findCatalog(snapshot, SegmentId(73U)).state, SegmentState::SGS_OBSOLETE);
	EXPECT_EQ(metadata->findCatalog(snapshot, SegmentId(75U)).state, SegmentState::SGS_OBSOLETE);
}

TEST_F(RelocatableMaintenanceServiceTest, DefersPageTasksForSnapshotAndRetiredGenerationPressure) {
	auto metadata = RelocatableMetadataStore::initializeNew(io_, initialSuperblock());
	PhysicalSegmentAllocator physical(io_, *metadata);
	SlottedPagePool pages(io_, *metadata);
	BTreePageStore btreePages(pages, *metadata);
	VectorPageStore vectorPages(io_, *metadata);
	VectorManifestStore vectorManifests(io_, *metadata);
	EntityPageStore entities(io_, *metadata);
		std::mutex publicationMutex;
	VectorCodebookStore codebooks(physical, *metadata);
	LargeValueStore largeValues(io_, *metadata);
		SlottedPage source = SlottedPage::create(propertyOwner(PageId(101U), LogSequenceNumber(2U)));
	ASSERT_TRUE(pages.allocatePage(propertyRequest(SegmentId(71U), PageId(101U)), source, PhysicalSlotId(40U),
									   PhysicalSlotId(40U), LogSequenceNumber(2U))
					.has_value());
	SlottedPage destination = SlottedPage::create(propertyOwner(PageId(102U), LogSequenceNumber(3U)));
	ASSERT_TRUE(pages.allocatePage(propertyRequest(SegmentId(72U), PageId(102U)), destination, PhysicalSlotId(41U),
									   PhysicalSlotId(41U), LogSequenceNumber(3U))
					.has_value());

	RelocatableMaintenanceOptions snapshotOptions;
	snapshotOptions.loop.minimum_cycle_interval = std::chrono::milliseconds(1);
	snapshotOptions.page.maximum_active_snapshots = 0U;
	RelocatableMaintenanceService snapshotService(pages, btreePages, vectorPages, vectorManifests, entities, codebooks,
												  largeValues, physical, *metadata, publicationMutex, snapshotOptions);
	MetadataSnapshotPin pinned = metadata->openSnapshot();
	snapshotService.start();
	EXPECT_EQ(snapshotService.enqueuePropertyPageCompaction(SegmentId(71U), SegmentId(72U), 1U), 1U);
	ASSERT_TRUE(waitUntil([&] { return snapshotService.metrics().loop.deferred_attempts != 0U; }));
	EXPECT_NE(snapshotService.metrics().property_pages.snapshot_pressure_rejections, 0U);
	snapshotService.stop();
	pinned.release();

	RelocatableMaintenanceOptions retainedOptions;
	retainedOptions.loop.minimum_cycle_interval = std::chrono::milliseconds(1);
	retainedOptions.page.maximum_retained_generations = 0U;
	RelocatableMaintenanceService retainedService(pages, btreePages, vectorPages, vectorManifests, entities, codebooks,
												  largeValues, physical, *metadata, publicationMutex, retainedOptions);
	retainedService.start();
	EXPECT_EQ(retainedService.enqueuePropertyPageCompaction(SegmentId(71U), SegmentId(72U), 1U), 1U);
	ASSERT_TRUE(waitUntil([&] { return retainedService.metrics().loop.deferred_attempts != 0U; }));
	EXPECT_NE(retainedService.metrics().property_pages.retained_generation_rejections, 0U);
	retainedService.stop();
}

TEST_F(RelocatableMaintenanceServiceTest, CompactsLargeValueDataAndManifestPoolsAfterSnapshotsDrain) {
	auto metadata = RelocatableMetadataStore::initializeNew(io_, initialSuperblock());
	PhysicalSegmentAllocator physical(io_, *metadata);
	SlottedPagePool pages(io_, *metadata);
	BTreePageStore btreePages(pages, *metadata);
	VectorPageStore vectorPages(io_, *metadata);
	VectorManifestStore vectorManifests(io_, *metadata);
	EntityPageStore entities(io_, *metadata);
	VectorCodebookStore codebooks(physical, *metadata);
	LargeValueStore largeValues(io_, *metadata);
	std::mutex publicationMutex;

	const std::vector<std::byte> input = largeValueBytes(31U);
	const LargeValueWriteResult written = largeValues.write(input, largeValueWriteOptions(), LogSequenceNumber(2U));
	ASSERT_EQ(written.manifest.chunks.size(), 1U);
	const PageId dataPageId = written.manifest.chunks.front().first_data_page_id;

	MetadataSnapshotPin before = metadata->openSnapshot();
	const PageLocation sourceDataLocation = metadata->findPageLocation(before, dataPageId);
	const LargeValueManifestLocation sourceManifestLocation =
			metadata->findLargeValueManifest(before, written.reference.value_id);
	const PageLocation sourceManifestLocationPage =
			metadata->findPageLocation(before, sourceManifestLocation.manifest_page_id);
	const CatalogEntry sourceDataPool = metadata->findCatalog(before, sourceDataLocation.segment_id);
	const CatalogEntry sourceManifestPool = metadata->findCatalog(before, sourceManifestLocationPage.segment_id);
	ASSERT_EQ(sourceDataPool.kind, SegmentKind::SGK_LARGE_VALUE_DATA_POOL);
	ASSERT_EQ(sourceManifestPool.kind, SegmentKind::SGK_LARGE_VALUE_MANIFEST_POOL);

	allocateLargeValueDataDestination(io_, *metadata, written.reference, SegmentId(71U), PageId(501U), Generation(2U),
												 PhysicalSlotId(100U), LogSequenceNumber(3U));
	allocateLargeValueManifestDestination(io_, *metadata, written.manifest, SegmentId(72U), PageId(502U), Generation(2U),
															 PhysicalSlotId(101U), LogSequenceNumber(4U));

	RelocatableMaintenanceOptions options;
	options.loop.minimum_cycle_interval = std::chrono::milliseconds(1);
	RelocatableMaintenanceService service(pages, btreePages, vectorPages, vectorManifests, entities, codebooks, largeValues,
												 physical, *metadata, publicationMutex, options);
	service.start();
	EXPECT_EQ(service.enqueueLargeValuePageCompaction(sourceDataLocation.segment_id, SegmentId(71U), 1U), 1U);
	EXPECT_EQ(service.enqueueLargeValuePageCompaction(sourceManifestLocationPage.segment_id, SegmentId(72U), 1U), 2U);
	ASSERT_TRUE(waitUntil([&] { return service.metrics().loop.deferred_attempts >= 2U; }));
	EXPECT_EQ(service.metrics().loop.completed_tasks, 0U);
	EXPECT_EQ(largeValues.read(before, written.reference), input);

	before.release();
	ASSERT_TRUE(waitUntil([&] { return service.metrics().loop.completed_tasks == 2U; }));
	service.stop();
	const RelocatableMaintenanceMetrics metrics = service.metrics();
	EXPECT_EQ(metrics.large_value_pages.runs_started, 2U);
	EXPECT_EQ(metrics.large_value_pages.moved_pages, 2U);
	EXPECT_EQ(metrics.large_value_pages.source_release_deferrals, 2U);
	EXPECT_EQ(metrics.large_value_pages.released_source_segments, 2U);
	EXPECT_EQ(metrics.large_value_pages.reclaimed_physical_bytes, static_cast<uint64_t>(2U * RSS_SEGMENT_SIZE));

	const MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(metadata->findPageLocation(current, dataPageId).segment_id, SegmentId(71U));
	EXPECT_EQ(metadata
					  ->findPageLocation(current,
								metadata->findLargeValueManifest(current, written.reference.value_id).manifest_page_id)
					  .segment_id,
			  SegmentId(72U));
	EXPECT_EQ(largeValues.read(current, written.reference), input);
	EXPECT_FALSE(metadata->isPhysicalSlotAllocated(current, sourceDataPool.physical_slot));
	EXPECT_FALSE(metadata->isPhysicalSlotAllocated(current, sourceManifestPool.physical_slot));
}

TEST_F(RelocatableMaintenanceServiceTest, DefersPageCompactionWhenTheTypedDestinationPoolIsFull) {
	auto metadata = RelocatableMetadataStore::initializeNew(io_, initialSuperblock());
	PhysicalSegmentAllocator physical(io_, *metadata);
	SlottedPagePool pages(io_, *metadata);
	BTreePageStore btreePages(pages, *metadata);
	VectorPageStore vectorPages(io_, *metadata);
	VectorManifestStore vectorManifests(io_, *metadata);
	EntityPageStore entities(io_, *metadata);
		std::mutex publicationMutex;
	VectorCodebookStore codebooks(physical, *metadata);
	LargeValueStore largeValues(io_, *metadata);
		const SlottedPagePoolAllocationRequest source = btreeRequest(SegmentId(71U), PageId(101U), Generation(1U), 77U);
	ASSERT_TRUE(btreePages
					.allocate(source, btreePage(source, LogSequenceNumber(2U)), PhysicalSlotId(40U), PhysicalSlotId(40U),
							  LogSequenceNumber(2U))
					.has_value());
	const IndexRootLocation root{17U, source.page_id, source.generation};
	static_cast<void>(btreePages.publishRoots(std::span(&root, 1U), {}, LogSequenceNumber(3U)));
	const SlottedPagePoolAllocationRequest destination =
			btreeRequest(SegmentId(72U), PageId(200U), Generation(2U), 78U);
	ASSERT_TRUE(btreePages
					.allocate(destination, btreePage(destination, LogSequenceNumber(4U)), PhysicalSlotId(41U),
							  PhysicalSlotId(41U), LogSequenceNumber(4U))
					.has_value());
	for (uint64_t frame = 1U; frame < RSS_PAGE_POOL_DATA_FRAME_COUNT; ++frame) {
		SlottedPagePoolAppendRequest append;
		append.segment_id = destination.segment_id;
		append.generation = destination.generation;
		append.page_id = PageId(destination.page_id.value() + frame);
		append.page_owner_id = destination.page_owner_id;
		append.page_owner_kind = PageOwnerKind::POK_INDEX;
		const SlottedPagePoolAllocationRequest page =
				btreeRequest(destination.segment_id, append.page_id, destination.generation, 78U);
		static_cast<void>(btreePages.append(append, btreePage(page, LogSequenceNumber(4U + frame)),
															  LogSequenceNumber(4U + frame)));
	}

	RelocatableMaintenanceOptions options;
	options.loop.minimum_cycle_interval = std::chrono::milliseconds(1);
	RelocatableMaintenanceService service(pages, btreePages, vectorPages, vectorManifests, entities, codebooks, largeValues,
									physical, *metadata, publicationMutex, options);
	service.start();
	EXPECT_EQ(service.enqueueBTreePageCompaction(SegmentId(71U), SegmentId(72U), 1U), 1U);
	ASSERT_TRUE(waitUntil([&] { return service.metrics().loop.deferred_attempts != 0U; }));
	EXPECT_NE(service.metrics().btree_pages.destination_capacity_rejections, 0U);
	service.stop();
	const MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(metadata->findPageLocation(snapshot, source.page_id).segment_id, source.segment_id);
	EXPECT_EQ(metadata->findCatalog(snapshot, source.segment_id).state, SegmentState::SGS_SEALED);
}

TEST_F(RelocatableMaintenanceServiceTest, ExecutesEntityPageAndExtentRelocationsThroughTypedTasks) {
	auto metadata = RelocatableMetadataStore::initializeNew(io_, initialSuperblock());
	PhysicalSegmentAllocator physical(io_, *metadata);
	SlottedPagePool pages(io_, *metadata);
	BTreePageStore btreePages(pages, *metadata);
	VectorPageStore vectorPages(io_, *metadata);
	VectorManifestStore vectorManifests(io_, *metadata);
	EntityPageStore entities(io_, *metadata);
	std::mutex publicationMutex;
	VectorCodebookStore codebooks(physical, *metadata);
	LargeValueStore largeValues(io_, *metadata);

	const NodeEntityRecord sourceNode = entityNode(11U, 12U);
	const NodeEntityRecord destinationNode = entityNode(21U, 22U, 17U);
	ASSERT_TRUE(entities.allocateNode(entityPool(71U, 17U, 101U, 40U), 1000U, sourceNode, LogSequenceNumber(2U))
					.has_value());
	ASSERT_TRUE(entities.allocateNode(entityPool(76U, 17U, 201U, 45U), 2000U, destinationNode, LogSequenceNumber(3U))
					.has_value());
	VectorCodebookWriteRequest codebookRequest;
	codebookRequest.codebook_id = 17U;
	codebookRequest.manifest_segment_id = SegmentId(72U);
	codebookRequest.generation = Generation(1U);
	codebookRequest.element_type = VectorCodebookElementType::VCET_FLOAT32;
	codebookRequest.dimensions = 2U;
	codebookRequest.subspace_count = 1U;
	codebookRequest.centroid_count = 3U;
	const std::vector<std::byte> codebookPayload(24U, std::byte{0x31});
	static_cast<void>(codebooks.writeImmutable(codebookRequest, codebookPayload, PhysicalSlotId(41U), PhysicalSlotId(41U),
																			 LogSequenceNumber(4U)));

	RelocatableMaintenanceOptions options;
	options.loop.minimum_cycle_interval = std::chrono::milliseconds(1);
	RelocatableMaintenanceService service(pages, btreePages, vectorPages, vectorManifests, entities, codebooks, largeValues,
									physical, *metadata, publicationMutex, options);
	service.start();
	EXPECT_EQ(service.enqueueEntityPageCompaction(SegmentId(71U), SegmentId(76U), 1U), 1U);
	EXPECT_EQ(service.enqueueCodebookExtentRelocation(17U, VectorCodebookExtentIdCodec::manifestExtentId(17U),
																  PhysicalSlotId(200U), PhysicalSlotId(200U)),
			  2U);
	ASSERT_TRUE(waitUntil([&] { return service.metrics().loop.completed_tasks == 2U; }));
	service.stop();

	const RelocatableMaintenanceMetrics metrics = service.metrics();
	EXPECT_EQ(metrics.entity_pages.moved_pages, 1U);
	EXPECT_EQ(metrics.segments.relocated_segments, 1U);
	EXPECT_EQ(metrics.loop.rewritten_bytes, static_cast<uint64_t>(RSS_PAGE_SIZE + RSS_SEGMENT_SIZE));
	EXPECT_EQ(metrics.loop.reclaimed_physical_bytes, static_cast<uint64_t>(2U * RSS_SEGMENT_SIZE));
	const MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_FALSE(metadata->isPhysicalSlotAllocated(snapshot, PhysicalSlotId(40U)));
	EXPECT_TRUE(metadata->isPhysicalSlotAllocated(snapshot, PhysicalSlotId(45U)));
	EXPECT_EQ(entities.readNode(snapshot, 1000U), sourceNode);
	EXPECT_EQ(entities.readNode(snapshot, 2000U), destinationNode);
	EXPECT_EQ(codebooks.read(snapshot, 17U).payload, codebookPayload);
}

TEST_F(RelocatableMaintenanceServiceTest, RetriesTypedRelocationSourceReleaseUntilSnapshotsDrain) {
	auto metadata = RelocatableMetadataStore::initializeNew(io_, initialSuperblock());
	PhysicalSegmentAllocator physical(io_, *metadata);
	SlottedPagePool pages(io_, *metadata);
	BTreePageStore btreePages(pages, *metadata);
	VectorPageStore vectorPages(io_, *metadata);
	VectorManifestStore vectorManifests(io_, *metadata);
	EntityPageStore entities(io_, *metadata);
	std::mutex publicationMutex;
	VectorCodebookStore codebooks(physical, *metadata);
	LargeValueStore largeValues(io_, *metadata);

	const NodeEntityRecord sourceNode = entityNode(11U, 12U);
	const NodeEntityRecord destinationNode = entityNode(21U, 22U, 17U);
	ASSERT_TRUE(entities.allocateNode(entityPool(71U, 17U, 101U, 40U), 1000U, sourceNode, LogSequenceNumber(2U))
					.has_value());
	ASSERT_TRUE(entities.allocateNode(entityPool(76U, 17U, 201U, 45U), 2000U, destinationNode, LogSequenceNumber(3U))
					.has_value());
	VectorCodebookWriteRequest codebookRequest;
	codebookRequest.codebook_id = 17U;
	codebookRequest.manifest_segment_id = SegmentId(72U);
	codebookRequest.generation = Generation(1U);
	codebookRequest.element_type = VectorCodebookElementType::VCET_FLOAT32;
	codebookRequest.dimensions = 2U;
	codebookRequest.subspace_count = 1U;
	codebookRequest.centroid_count = 3U;
	const std::vector<std::byte> codebookPayload(24U, std::byte{0x31});
	static_cast<void>(codebooks.writeImmutable(codebookRequest, codebookPayload, PhysicalSlotId(41U), PhysicalSlotId(41U),
																			 LogSequenceNumber(4U)));
	RelocatableMaintenanceOptions options;
	options.loop.minimum_cycle_interval = std::chrono::milliseconds(1);
	RelocatableMaintenanceService service(pages, btreePages, vectorPages, vectorManifests, entities, codebooks, largeValues,
									physical, *metadata, publicationMutex, options);
	MetadataSnapshotPin pinned = metadata->openSnapshot();
	service.start();
	EXPECT_EQ(service.enqueueEntityPageCompaction(SegmentId(71U), SegmentId(76U), 1U), 1U);
	EXPECT_EQ(service.enqueueCodebookExtentRelocation(17U, VectorCodebookExtentIdCodec::manifestExtentId(17U),
																  PhysicalSlotId(200U), PhysicalSlotId(200U)),
			  2U);
	ASSERT_TRUE(waitUntil([&] {
		const RelocatableMaintenanceMetrics metrics = service.metrics();
		return metrics.entity_pages.source_release_deferrals == 1U && metrics.segments.source_release_deferrals == 1U &&
			   metrics.loop.deferred_attempts >= 4U;
	}));
	EXPECT_EQ(service.metrics().loop.completed_tasks, 0U);
	pinned.release();
	ASSERT_TRUE(waitUntil([&] { return service.metrics().loop.completed_tasks == 2U; }));
	service.stop();

	const RelocatableMaintenanceMetrics metrics = service.metrics();
	EXPECT_EQ(metrics.entity_pages.moved_pages, 1U);
	EXPECT_EQ(metrics.entity_pages.released_source_segments, 1U);
	EXPECT_EQ(metrics.entity_pages.source_release_deferrals, 1U);
	EXPECT_EQ(metrics.segments.relocated_segments, 1U);
	EXPECT_EQ(metrics.segments.released_source_segments, 1U);
	EXPECT_EQ(metrics.segments.source_release_deferrals, 1U);
	EXPECT_EQ(metrics.loop.reclaimed_physical_bytes, static_cast<uint64_t>(2U * RSS_SEGMENT_SIZE));
	const MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_FALSE(metadata->isPhysicalSlotAllocated(snapshot, PhysicalSlotId(40U)));
	EXPECT_EQ(entities.readNode(snapshot, 1000U), sourceNode);
	EXPECT_EQ(entities.readNode(snapshot, 2000U), destinationNode);
	EXPECT_EQ(codebooks.read(snapshot, 17U).payload, codebookPayload);
}

TEST_F(RelocatableMaintenanceServiceTest, DefersEntityPageTasksForAdmissionAndDestinationPressure) {
	auto metadata = RelocatableMetadataStore::initializeNew(io_, initialSuperblock());
	PhysicalSegmentAllocator physical(io_, *metadata);
	SlottedPagePool pages(io_, *metadata);
	BTreePageStore btreePages(pages, *metadata);
	VectorPageStore vectorPages(io_, *metadata);
	VectorManifestStore vectorManifests(io_, *metadata);
	EntityPageStore entities(io_, *metadata);
	std::mutex publicationMutex;
	VectorCodebookStore codebooks(physical, *metadata);
	LargeValueStore largeValues(io_, *metadata);
	const NodeEntityRecord sourceNode = entityNode(11U, 12U);
	const NodeEntityRecord destinationNode = entityNode(21U, 22U, 17U);
	ASSERT_TRUE(entities.allocateNode(entityPool(71U, 17U, 101U, 40U), 1000U, sourceNode, LogSequenceNumber(2U))
					.has_value());
	EntityPageInsertResult destination =
			entities.allocateNode(entityPool(76U, 17U, 201U, 45U), 2000U, destinationNode, LogSequenceNumber(3U)).value();
	CatalogEntry destinationPool = destination.pool;
	for (uint64_t frame = 1U; frame < RSS_PAGE_POOL_DATA_FRAME_COUNT; ++frame) {
		destination = entities.appendNodePage({destinationPool, PageId(201U + frame)}, 2000U + frame,
																	 entityNode(30U + frame, 40U + frame), LogSequenceNumber(3U + frame));
		destinationPool = destination.pool;
	}
	const LogSequenceNumber ancillaryLsn(4U + RSS_PAGE_POOL_DATA_FRAME_COUNT);
	VectorCodebookWriteRequest codebookRequest;
	codebookRequest.codebook_id = 17U;
	codebookRequest.manifest_segment_id = SegmentId(72U);
	codebookRequest.generation = Generation(1U);
	codebookRequest.element_type = VectorCodebookElementType::VCET_FLOAT32;
	codebookRequest.dimensions = 2U;
	codebookRequest.subspace_count = 1U;
	codebookRequest.centroid_count = 3U;
	static_cast<void>(codebooks.writeImmutable(codebookRequest, std::vector<std::byte>(24U, std::byte{0x31}),
																		 PhysicalSlotId(41U), PhysicalSlotId(41U), ancillaryLsn));
	RelocatableMaintenanceOptions snapshotOptions;
	snapshotOptions.loop.minimum_cycle_interval = std::chrono::milliseconds(1);
	snapshotOptions.page.maximum_active_snapshots = 0U;
	RelocatableMaintenanceService snapshotService(pages, btreePages, vectorPages, vectorManifests, entities, codebooks,
												  largeValues, physical, *metadata, publicationMutex, snapshotOptions);
	MetadataSnapshotPin pinned = metadata->openSnapshot();
	snapshotService.start();
	EXPECT_EQ(snapshotService.enqueueEntityPageCompaction(SegmentId(71U), SegmentId(76U), 1U),
				  1U);
	ASSERT_TRUE(waitUntil([&] { return snapshotService.metrics().loop.deferred_attempts != 0U; }));
	EXPECT_NE(snapshotService.metrics().entity_pages.snapshot_pressure_rejections, 0U);
	snapshotService.stop();
	pinned.release();

	RelocatableMaintenanceOptions retainedOptions;
	retainedOptions.loop.minimum_cycle_interval = std::chrono::milliseconds(1);
	retainedOptions.page.maximum_retained_generations = 0U;
	RelocatableMaintenanceService retainedService(pages, btreePages, vectorPages, vectorManifests, entities, codebooks,
												  largeValues, physical, *metadata, publicationMutex, retainedOptions);
	retainedService.start();
	EXPECT_EQ(retainedService.enqueueEntityPageCompaction(SegmentId(71U), SegmentId(76U), 1U),
				  1U);
	EXPECT_EQ(retainedService.enqueueCodebookExtentRelocation(
				  17U, VectorCodebookExtentIdCodec::manifestExtentId(17U), PhysicalSlotId(51U), PhysicalSlotId(51U)),
			  2U);
	ASSERT_TRUE(waitUntil([&] { return retainedService.metrics().loop.deferred_attempts >= 2U; }));
	EXPECT_NE(retainedService.metrics().entity_pages.retained_generation_rejections, 0U);
	retainedService.stop();

	RelocatableMaintenanceOptions capacityOptions;
	capacityOptions.loop.minimum_cycle_interval = std::chrono::milliseconds(1);
	RelocatableMaintenanceService capacityService(pages, btreePages, vectorPages, vectorManifests, entities, codebooks,
												  largeValues, physical, *metadata, publicationMutex, capacityOptions);
	capacityService.start();
	EXPECT_EQ(capacityService.enqueueEntityPageCompaction(SegmentId(71U), SegmentId(76U), 1U),
				  1U);
	ASSERT_TRUE(waitUntil([&] { return capacityService.metrics().loop.deferred_attempts != 0U; }));
	EXPECT_NE(capacityService.metrics().entity_pages.destination_capacity_rejections, 0U);
	capacityService.stop();
	const MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(metadata->findPageLocation(snapshot, PageId(101U)).segment_id, SegmentId(71U));
	EXPECT_EQ(entities.readNode(snapshot, 1000U), sourceNode);
}

TEST_F(RelocatableMaintenanceServiceTest, FaultsClosedWhenCheckpointLsnCannotAdvance) {
	Superblock exhausted = initialSuperblock();
	exhausted.checkpoint_lsn = LogSequenceNumber(std::numeric_limits<uint64_t>::max() - 1U);
	auto metadata = RelocatableMetadataStore::initializeNew(io_, exhausted);
	PhysicalSegmentAllocator physical(io_, *metadata);
	SlottedPagePool pages(io_, *metadata);
	BTreePageStore btreePages(pages, *metadata);
	VectorPageStore vectorPages(io_, *metadata);
	VectorManifestStore vectorManifests(io_, *metadata);
	EntityPageStore entities(io_, *metadata);
		std::mutex publicationMutex;
	VectorCodebookStore codebooks(physical, *metadata);
	LargeValueStore largeValues(io_, *metadata);
		const LogSequenceNumber terminalLsn(std::numeric_limits<uint64_t>::max());
	SlottedPage source = SlottedPage::create(propertyOwner(PageId(101U), terminalLsn));
	ASSERT_TRUE(pages.allocatePage(propertyRequest(SegmentId(71U), PageId(101U)), source, PhysicalSlotId(40U),
									   PhysicalSlotId(40U), terminalLsn)
					.has_value());

	RelocatableMaintenanceOptions options;
	options.loop.minimum_cycle_interval = std::chrono::milliseconds(1);
	RelocatableMaintenanceService service(pages, btreePages, vectorPages, vectorManifests, entities, codebooks, largeValues,
									physical, *metadata, publicationMutex, options);
	service.start();
	EXPECT_EQ(service.enqueuePropertyPageCompaction(SegmentId(71U), SegmentId(72U), 1U), 1U);
	ASSERT_TRUE(waitUntil([&] { return service.metrics().loop.failed_tasks == 1U; }));
	EXPECT_THROW(std::rethrow_exception(service.lastFailure()), FormatException);
	EXPECT_EQ(service.metrics().loop.state, RelocatableMaintenanceLoopState::VMLS_FAULTED);
	service.stop();
}

TEST_F(RelocatableMaintenanceServiceTest, RejectsInvalidRequestsAndFaultsClosedWhenWritesAreForbidden) {
	auto metadata = RelocatableMetadataStore::initializeNew(io_, initialSuperblock());
	PhysicalSegmentAllocator physical(io_, *metadata);
	SlottedPagePool pages(io_, *metadata);
	BTreePageStore btreePages(pages, *metadata);
	VectorPageStore vectorPages(io_, *metadata);
	VectorManifestStore vectorManifests(io_, *metadata);
	EntityPageStore entities(io_, *metadata);
		std::mutex publicationMutex;
	VectorCodebookStore codebooks(physical, *metadata);
	LargeValueStore largeValues(io_, *metadata);

	RelocatableMaintenanceOptions invalidOptions;
	invalidOptions.page.maximum_page_moves = 0U;
	EXPECT_THROW(
		RelocatableMaintenanceService(pages, btreePages, vectorPages, vectorManifests, entities, codebooks, largeValues,
								physical, *metadata, publicationMutex, invalidOptions),
		std::invalid_argument);
	invalidOptions = {};
	invalidOptions.page.maximum_rewrite_bytes = RSS_PAGE_SIZE - 1U;
	EXPECT_THROW(
		RelocatableMaintenanceService(pages, btreePages, vectorPages, vectorManifests, entities, codebooks, largeValues,
								physical, *metadata, publicationMutex, invalidOptions),
		std::invalid_argument);
	invalidOptions = {};
	invalidOptions.segment.maximum_segment_moves = 0U;
	EXPECT_THROW(
		RelocatableMaintenanceService(pages, btreePages, vectorPages, vectorManifests, entities, codebooks, largeValues,
								physical, *metadata, publicationMutex, invalidOptions),
		std::invalid_argument);
	invalidOptions = {};
	invalidOptions.segment.maximum_rewrite_bytes = RSS_SEGMENT_SIZE - 1U;
	EXPECT_THROW(
		RelocatableMaintenanceService(pages, btreePages, vectorPages, vectorManifests, entities, codebooks, largeValues,
								physical, *metadata, publicationMutex, invalidOptions),
		std::invalid_argument);
	invalidOptions = {};
	invalidOptions.loop.maximum_rewrite_bytes_per_cycle = RSS_SEGMENT_SIZE - 1U;
	EXPECT_THROW(
		RelocatableMaintenanceService(pages, btreePages, vectorPages, vectorManifests, entities, codebooks, largeValues,
								physical, *metadata, publicationMutex, invalidOptions),
		std::invalid_argument);
	invalidOptions = {};
	invalidOptions.loop.maximum_outstanding_rewrite_bytes = RSS_SEGMENT_SIZE - 1U;
	EXPECT_THROW(
		RelocatableMaintenanceService(pages, btreePages, vectorPages, vectorManifests, entities, codebooks, largeValues,
								physical, *metadata, publicationMutex, invalidOptions),
		std::invalid_argument);

	RelocatableMaintenanceOptions options;
	options.loop.minimum_cycle_interval = std::chrono::milliseconds(1);
	RelocatableMaintenanceService service(pages, btreePages, vectorPages, vectorManifests, entities, codebooks, largeValues,
									physical, *metadata, publicationMutex, options, {}, [] { return false; });
	EXPECT_THROW(static_cast<void>(service.enqueuePropertyPageCompaction(SegmentId{}, SegmentId(2U), 1U)),
			 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(service.enqueueBTreePageCompaction(SegmentId(1U), SegmentId(1U), 1U)),
			 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(service.enqueueVectorPageCompaction(SegmentId(1U), SegmentId(2U), 0U)),
			 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(service.enqueueVectorManifestCompaction(SegmentId(1U), SegmentId{}, 1U)),
			 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(service.enqueueEntityPageCompaction(SegmentId(1U), SegmentId(1U), 1U)),
				 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(service.enqueueEntityPageCompaction(SegmentId{}, SegmentId(2U), 1U)),
				 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(service.enqueueEntityPageCompaction(SegmentId(1U), SegmentId{}, 1U)),
				 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(service.enqueueEntityPageCompaction(SegmentId(1U), SegmentId(2U), 0U)),
				 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(service.enqueueCodebookExtentRelocation(0U, ExtentId(1U), PhysicalSlotId(2U),
																											 PhysicalSlotId(2U))),
			 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(service.enqueueCodebookExtentRelocation(1U, ExtentId{}, PhysicalSlotId(2U),
																											 PhysicalSlotId(2U))),
			 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(service.enqueueCodebookExtentRelocation(1U, ExtentId(1U), PhysicalSlotId{},
																											 PhysicalSlotId(2U))),
			 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(service.enqueueCodebookExtentRelocation(1U, ExtentId(1U), PhysicalSlotId(2U),
																											 PhysicalSlotId{})),
			 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(service.enqueueCodebookExtentRelocation(1U, ExtentId(1U), PhysicalSlotId(3U),
																											 PhysicalSlotId(2U))),
			 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(service.enqueueLargeValuePageCompaction(SegmentId{}, SegmentId(2U), 1U)),
			 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(service.enqueueLargeValuePageCompaction(SegmentId(1U), SegmentId(1U), 1U)),
			 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(service.enqueueLargeValuePageCompaction(SegmentId(1U), SegmentId{}, 1U)),
			 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(service.enqueueLargeValuePageCompaction(SegmentId(1U), SegmentId(2U), 0U)),
			 std::invalid_argument);

	service.start();
	EXPECT_EQ(service.enqueuePropertyPageCompaction(SegmentId(1U), SegmentId(2U), 1U), 1U);
	ASSERT_TRUE(waitUntil([&] { return service.metrics().loop.failed_tasks == 1U; }));
	EXPECT_EQ(service.metrics().loop.state, RelocatableMaintenanceLoopState::VMLS_FAULTED);
	EXPECT_NE(service.lastFailure(), nullptr);
	service.stop();

	std::atomic_uint32_t writeChecks{0U};
	RelocatableMaintenanceService lockCheckedService(
			pages, btreePages, vectorPages, vectorManifests, entities, codebooks, largeValues, physical, *metadata,
			publicationMutex, options, {}, [&writeChecks] { return writeChecks.fetch_add(1U, std::memory_order_relaxed) == 0U; });
	lockCheckedService.start();
	EXPECT_EQ(lockCheckedService.enqueuePropertyPageCompaction(SegmentId(1U), SegmentId(2U), 1U), 1U);
	ASSERT_TRUE(waitUntil([&] { return lockCheckedService.metrics().loop.failed_tasks == 1U; }));
	EXPECT_GE(writeChecks.load(std::memory_order_relaxed), 2U);
	EXPECT_EQ(lockCheckedService.metrics().loop.state, RelocatableMaintenanceLoopState::VMLS_FAULTED);
	lockCheckedService.stop();

	RelocatableMaintenanceService lifecycleService(pages, btreePages, vectorPages, vectorManifests, entities, codebooks,
												largeValues, physical, *metadata, publicationMutex, options);
	EXPECT_EQ(lifecycleService.enqueuePropertyPageCompaction(SegmentId(1U), SegmentId(2U), 1U), 1U);
	EXPECT_EQ(lifecycleService.cancelQueuedTasks(), 1U);
	lifecycleService.start();
	lifecycleService.pause();
	EXPECT_EQ(lifecycleService.metrics().loop.state, RelocatableMaintenanceLoopState::VMLS_PAUSED);
	lifecycleService.resume();
	EXPECT_EQ(lifecycleService.metrics().loop.state, RelocatableMaintenanceLoopState::VMLS_RUNNING);
	 lifecycleService.stop();
}

TEST_F(RelocatableMaintenanceServiceTest, EnqueuesTypedVectorBaseRebuildAfterAdmission) {
	auto metadata = RelocatableMetadataStore::initializeNew(io_, initialSuperblock());
	PhysicalSegmentAllocator physical(io_, *metadata);
	SlottedPagePool pages(io_, *metadata);
	BTreePageStore btreePages(pages, *metadata);
	VectorPageStore vectorPages(io_, *metadata);
	VectorManifestStore vectorManifests(io_, *metadata);
	EntityPageStore entities(io_, *metadata);
		std::mutex publicationMutex;
	VectorCodebookStore codebooks(physical, *metadata);
	LargeValueStore largeValues(io_, *metadata);
		RelocatableMaintenanceOptions options;
	options.loop.minimum_cycle_interval = std::chrono::milliseconds(1);
	std::atomic_uint64_t rebuiltIndex{0U};
	RelocatableMaintenanceService service(
			pages, btreePages, vectorPages, vectorManifests, entities, codebooks, largeValues, physical, *metadata,
			publicationMutex, options, {}, {}, [&rebuiltIndex](uint64_t indexId) {
				rebuiltIndex.store(indexId, std::memory_order_release);
				return RelocatableMaintenanceTaskResult{RelocatableMaintenanceTaskStatus::VMTS_COMPLETED, 0U, 0U};
			});
	service.start();
	EXPECT_EQ(service.enqueueVectorBaseRebuild(77U), 1U);
	ASSERT_TRUE(waitUntil([&] { return service.metrics().loop.completed_tasks == 1U; }));
	EXPECT_EQ(rebuiltIndex.load(std::memory_order_acquire), 77U);
	service.stop();
}

TEST_F(RelocatableMaintenanceServiceTest, DeduplicatesAndReleasesVectorRebuildAdmission) {
	auto metadata = RelocatableMetadataStore::initializeNew(io_, initialSuperblock());
	PhysicalSegmentAllocator physical(io_, *metadata);
	SlottedPagePool pages(io_, *metadata);
	BTreePageStore btreePages(pages, *metadata);
	VectorPageStore vectorPages(io_, *metadata);
	VectorManifestStore vectorManifests(io_, *metadata);
	EntityPageStore entities(io_, *metadata);
		std::mutex publicationMutex;
	VectorCodebookStore codebooks(physical, *metadata);
	LargeValueStore largeValues(io_, *metadata);
		RelocatableMaintenanceOptions options;
	options.loop.minimum_cycle_interval = std::chrono::milliseconds(1);
	std::atomic_uint32_t executions{0U};
	RelocatableMaintenanceService service(
			pages, btreePages, vectorPages, vectorManifests, entities, codebooks, largeValues, physical, *metadata,
			publicationMutex, options, {}, {}, [&executions](uint64_t) {
				executions.fetch_add(1U, std::memory_order_relaxed);
				return RelocatableMaintenanceTaskResult{RelocatableMaintenanceTaskStatus::VMTS_COMPLETED, 0U, 0U};
			});

	EXPECT_EQ(service.enqueueVectorBaseRebuild(77U), 1U);
	EXPECT_EQ(service.enqueueVectorBaseRebuild(77U), 0U);
	EXPECT_EQ(service.cancelQueuedTasks(), 1U);
	// Cancellation must release the key so a durable maintenance debt can be
	// re-admitted after an operator pause/cancel cycle.
	EXPECT_EQ(service.enqueueVectorBaseRebuild(77U), 2U);
	service.start();
	ASSERT_TRUE(waitUntil([&] { return service.metrics().loop.completed_tasks == 1U; }));
	EXPECT_EQ(executions.load(std::memory_order_relaxed), 1U);
	service.stop();
}

TEST_F(RelocatableMaintenanceServiceTest, RecoversVectorDebtWhileTheOnlyQueueSlotIsOccupied) {
	auto metadata = RelocatableMetadataStore::initializeNew(io_, initialSuperblock());
	PhysicalSegmentAllocator physical(io_, *metadata);
	SlottedPagePool pages(io_, *metadata);
	BTreePageStore btreePages(pages, *metadata);
	VectorPageStore vectorPages(io_, *metadata);
	VectorManifestStore vectorManifests(io_, *metadata);
	EntityPageStore entities(io_, *metadata);
	std::mutex publicationMutex;
	VectorCodebookStore codebooks(physical, *metadata);
	LargeValueStore largeValues(io_, *metadata);
	RelocatableMaintenanceOptions options;
	options.loop.maximum_outstanding_tasks = 1U;
	options.loop.minimum_cycle_interval = std::chrono::milliseconds(1);
	options.vector_rebuild_recovery_metadata_pages_per_cycle = 2U;
	std::atomic_uint64_t rebuiltIndex{0U};
	std::atomic_uint32_t scans{0U};
	RelocatableMaintenanceService service(
			pages, btreePages, vectorPages, vectorManifests, entities, codebooks, largeValues, physical, *metadata,
			publicationMutex, options, {}, {},
			[&rebuiltIndex](uint64_t indexId) {
				rebuiltIndex.store(indexId, std::memory_order_release);
				return RelocatableMaintenanceTaskResult{RelocatableMaintenanceTaskStatus::VMTS_COMPLETED, 0U, 0U};
			},
			[&scans](uint64_t afterIndexId, size_t maximumMetadataPages) {
				EXPECT_EQ(maximumMetadataPages, 2U);
				const uint32_t scan = scans.fetch_add(1U, std::memory_order_relaxed);
				if (scan == 0U) {
					EXPECT_EQ(afterIndexId, 0U);
					return RelocatableVectorRebuildDebtScan{77U, 77U, 1U, false};
				}
				EXPECT_EQ(afterIndexId, 77U);
				return RelocatableVectorRebuildDebtScan{std::nullopt, 0U, 1U, true};
			});

	EXPECT_EQ(service.enqueueVectorRebuildRecovery(), 1U);
	service.start();
	ASSERT_TRUE(waitUntil([&] { return service.metrics().vector_rebuild_recovery_completed_rebuild_attempts >= 1U; }));
	EXPECT_EQ(rebuiltIndex.load(std::memory_order_acquire), 77U);
	EXPECT_THROW(static_cast<void>(service.enqueueVectorBaseRebuild(99U)), std::length_error);
	const RelocatableMaintenanceMetrics metrics = service.metrics();
	EXPECT_GE(metrics.vector_rebuild_recovery_scanned_metadata_pages, 1U);
	EXPECT_GE(metrics.loop.deferred_attempts, 1U);
	service.stop();
}
