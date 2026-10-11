/**
 * @file test_EntityPageStoreScanCache.cpp
 * @brief Buffer Pool admission tests for snapshot-pinned Entity scans.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <vector>

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"
#include "graph/storage/relocatable/entity/EntityPageStore.hpp"
#include "graph/storage/relocatable/entity/NativeReadSession.hpp"

using namespace graph::storage;

namespace {

	Superblock initialSuperblock() {
		Superblock result;
		result.database_id = DatabaseId(0x1020304050607080ULL);
		result.generation = Generation(1U);
		result.checkpoint_lsn = LogSequenceNumber(1U);
		return result;
	}

	EntityPageNewPoolRequest newPool(uint64_t segmentId, uint64_t ownerId, uint64_t pageId, uint64_t physicalSlot) {
		return {.segment_id = SegmentId(segmentId),
				.pool_owner_id = ownerId,
				.generation = Generation(1U),
				.page_id = PageId(pageId),
				.first_slot = PhysicalSlotId(physicalSlot),
				.last_slot = PhysicalSlotId(physicalSlot)};
	}

	NodeEntityRecord node(uint64_t labelId) {
		NodeEntityRecord result;
		result.inline_label_ids.front() = labelId;
		result.inline_label_count = 1U;
		return result;
	}

	EdgeEntityRecord edge(uint64_t source, uint64_t target, uint64_t typeId) {
		return {.source_node_id = source, .target_node_id = target, .type_id = typeId};
	}

} // namespace

class EntityPageStoreScanCacheTest : public ::testing::Test {
protected:
	std::filesystem::path path_;
	std::shared_ptr<std::fstream> file_;
	std::shared_ptr<StorageIO> io_;
	std::unique_ptr<RelocatableMetadataStore> metadata_;
	std::unique_ptr<EntityPageStore> entities_;
	MetadataSnapshotPin snapshot_;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		path_ = std::filesystem::temp_directory_path() / ("entity_page_scan_cache_" + to_string(uuid));
		file_ = std::make_shared<std::fstream>(path_,
											 std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(file_->is_open());
		io_ = std::make_shared<StorageIO>(file_, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		metadata_ = RelocatableMetadataStore::initializeNew(io_, initialSuperblock());
		entities_ = std::make_unique<EntityPageStore>(io_, *metadata_);

		MetadataSnapshotPin before = metadata_->openSnapshot();
		const std::array<EntityPageBatchNewPool, 2U> pages{{
				{.request = newPool(31U, 901U, 101U, 40U),
				 .record_kind = EntityPageRecordKind::EPRK_NODE,
				 .node_insertions = {{1001U, node(7U)}, {1002U, node(9U)}, {1003U, node(11U)}}},
				{.request = newPool(31U, 901U, 201U, 40U),
				 .record_kind = EntityPageRecordKind::EPRK_EDGE,
				 .edge_insertions = {{2001U, edge(1001U, 1002U, 41U)},
									 {2002U, edge(1002U, 1003U, 43U)},
									 {2003U, edge(1003U, 1001U, 45U)}}},
		}};
		const EntityPageBatchResult published =
				entities_->applyBatchTransaction(before, {}, {}, pages, LogSequenceNumber(2U));
		ASSERT_EQ(published.new_pool_pages.size(), pages.size());
		before.release();
		io_->sync();
		io_->enableBufferPool(64U * RSS_PAGE_SIZE);
		snapshot_ = metadata_->openSnapshot();
	}

	MetadataSnapshotPin publishScanEdgePages() {
		const std::array<EntityPageBatchNewPool, 3U> pages{{
				{.request = newPool(51U, 951U, 301U, 60U),
				 .record_kind = EntityPageRecordKind::EPRK_EDGE,
				 .edge_insertions = {{3001U, edge(1001U, 1002U, 41U)}}},
				{.request = newPool(51U, 951U, 302U, 60U),
				 .record_kind = EntityPageRecordKind::EPRK_EDGE,
				 .edge_insertions = {{3002U, edge(1002U, 1003U, 43U)}}},
				{.request = newPool(52U, 952U, 303U, 61U),
				 .record_kind = EntityPageRecordKind::EPRK_EDGE,
				 .edge_insertions = {{3003U, edge(1003U, 1001U, 45U)}}},
		}};
		static_cast<void>(entities_->applyBatchTransaction(snapshot_, {}, {}, pages, LogSequenceNumber(3U)));
		return metadata_->openSnapshot();
	}

	void TearDown() override {
		snapshot_.release();
		entities_.reset();
		metadata_.reset();
		io_.reset();
		if (file_ && file_->is_open()) {
			file_->close();
		}
		file_.reset();
		std::error_code error;
		std::filesystem::remove(path_, error);
	}

	static uint64_t scanPageReadDelta(const BufferPoolStats &before, const BufferPoolStats &after) {
		return after.disk_read_bytes - before.disk_read_bytes;
	}
};

TEST_F(EntityPageStoreScanCacheTest, NodeScansUseTypedEntityDirectoryAndReusePageReads) {
	io_->resetBufferPoolStats();
	std::vector<uint64_t> firstIds;
	EXPECT_TRUE(entities_->visitNodesUntil(snapshot_, 8U, [&](uint64_t nodeId, const NodeEntityRecord &) {
		firstIds.push_back(nodeId);
		return true;
	}));
	EXPECT_EQ(firstIds, (std::vector<uint64_t>{1001U, 1002U, 1003U}));
	const BufferPoolStats afterFirst = io_->bufferPoolStats();

	std::vector<uint64_t> secondIds;
	EXPECT_TRUE(entities_->visitNodesUntil(snapshot_, 8U, [&](uint64_t nodeId, const NodeEntityRecord &) {
		secondIds.push_back(nodeId);
		return true;
	}));
	EXPECT_EQ(secondIds, firstIds);
	const BufferPoolStats afterSecond = io_->bufferPoolStats();
	// Typed scans visit only the Node Entity directory and its Node page. The
	// second scan may re-admit the directory and data page, but never an Edge
	// page or the generic Page Directory stream.
	EXPECT_EQ(scanPageReadDelta(afterFirst, afterSecond), 2U * RSS_PAGE_SIZE);
	EXPECT_EQ(afterSecond.misses, afterFirst.misses + 2U);
	EXPECT_EQ(afterSecond.resident_bytes, afterFirst.resident_bytes);

	std::vector<uint64_t> resumedIds;
	static_cast<void>(entities_->visitNodesAfter(
			snapshot_, 1001U, 8U, 8U,
			[&](uint64_t nodeId, const NodeEntityRecord &) {
				resumedIds.push_back(nodeId);
				return true;
			}));
	EXPECT_EQ(resumedIds, (std::vector<uint64_t>{1002U, 1003U}));
	const BufferPoolStats afterResumed = io_->bufferPoolStats();
	// Ordered resume uses the same typed Entity directory contract and does not
	// fall back to the generic Page Directory traversal.
	EXPECT_EQ(scanPageReadDelta(afterSecond, afterResumed), 2U * RSS_PAGE_SIZE);
	EXPECT_EQ(afterResumed.misses, afterSecond.misses + 2U);
	EXPECT_EQ(afterResumed.resident_bytes, afterSecond.resident_bytes);
}

TEST_F(EntityPageStoreScanCacheTest, EdgeScansUseTypedEntityDirectoryWhilePointReadsRemainResident) {
	io_->resetBufferPoolStats();
	std::vector<uint64_t> firstIds;
	EXPECT_TRUE(entities_->visitEdgesUntil(snapshot_, 8U, [&](uint64_t edgeId, const EdgeEntityRecord &) {
		firstIds.push_back(edgeId);
		return true;
	}));
	EXPECT_EQ(firstIds, (std::vector<uint64_t>{2001U, 2002U, 2003U}));
	const BufferPoolStats afterFirst = io_->bufferPoolStats();

	std::vector<uint64_t> secondIds;
	EXPECT_TRUE(entities_->visitEdgesUntil(snapshot_, 8U, [&](uint64_t edgeId, const EdgeEntityRecord &) {
		secondIds.push_back(edgeId);
		return true;
	}));
	EXPECT_EQ(secondIds, firstIds);
	const BufferPoolStats afterSecond = io_->bufferPoolStats();
	EXPECT_EQ(scanPageReadDelta(afterFirst, afterSecond), 2U * RSS_PAGE_SIZE);
	EXPECT_EQ(afterSecond.misses, afterFirst.misses + 2U);
	EXPECT_EQ(afterSecond.resident_bytes, afterFirst.resident_bytes);

	EXPECT_EQ(entities_->readEdge(snapshot_, 2001U), edge(1001U, 1002U, 41U));
	const BufferPoolStats afterFirstPoint = io_->bufferPoolStats();
	EXPECT_EQ(scanPageReadDelta(afterSecond, afterFirstPoint), 2U * RSS_PAGE_SIZE);
	EXPECT_EQ(afterFirstPoint.misses, afterSecond.misses + 2U);
	EXPECT_EQ(afterFirstPoint.resident_bytes, afterSecond.resident_bytes + 2U * RSS_PAGE_SIZE);

	EXPECT_EQ(entities_->readEdge(snapshot_, 2001U), edge(1001U, 1002U, 41U));
	const BufferPoolStats afterSecondPoint = io_->bufferPoolStats();
	EXPECT_EQ(afterSecondPoint.disk_read_bytes, afterFirstPoint.disk_read_bytes);
	EXPECT_EQ(afterSecondPoint.misses, afterFirstPoint.misses);
	EXPECT_GT(afterSecondPoint.hits, afterFirstPoint.hits);
}

TEST_F(EntityPageStoreScanCacheTest, ScanReusesOnlyTheCurrentSnapshotsMatchingPoolCatalog) {
	auto current = publishScanEdgePages();
	graph::debug::PerfTrace::reset();
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	std::vector<uint64_t> ids;
	try {
		EXPECT_TRUE(entities_->visitEdgesUntil(current, 10U, [&](uint64_t id, const EdgeEntityRecord &) {
			ids.push_back(id);
			return true;
		}));
	} catch (...) {
		graph::debug::PerfTrace::setValueCollectionEnabled(false);
		throw;
	}
	const auto trace = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setValueCollectionEnabled(false);
	EXPECT_EQ(ids, (std::vector<uint64_t>{2001U, 2002U, 2003U, 3001U, 3002U, 3003U}));
	ASSERT_TRUE(trace.contains("native_graph.read.entity_scan_catalog_cache_hits"));
	EXPECT_EQ(trace.at("native_graph.read.entity_scan_catalog_cache_hits").totalValue, 1);
	EXPECT_EQ(trace.at("native_graph.read.shared_page_route_resolution").totalValue, 3);
	EXPECT_EQ(trace.at("native_graph.read.edge_page_scan.pages").totalValue, 4);
	// Reusing a Catalog never crosses a scan or snapshot boundary.
	std::vector<uint64_t> oldIds;
	EXPECT_TRUE(entities_->visitEdgesUntil(snapshot_, 10U, [&](uint64_t id, const EdgeEntityRecord &) {
		oldIds.push_back(id);
		return true;
	}));
	EXPECT_EQ(oldIds, (std::vector<uint64_t>{2001U, 2002U, 2003U}));
}

TEST_F(EntityPageStoreScanCacheTest, CatalogReuseStillRejectsAChecksumValidForeignPageOwner) {
	auto current = publishScanEdgePages();
	const auto location = metadata_->findPageLocation(current, PageId(302U));
	const auto pool = metadata_->findCatalog(current, location.segment_id);
	const uint64_t offset = PhysicalSegmentAllocator::checkedSlotOffset(
			current.superblock().data_region_offset, pool.physical_slot) +
			static_cast<uint64_t>(location.page_index) * RSS_PAGE_SIZE;
	std::vector<uint64_t> visited;
	bool corrupted = false;
	try {
		static_cast<void>(entities_->visitEdgesUntil(current, 10U, [&](uint64_t id, const EdgeEntityRecord &) {
			visited.push_back(id);
			if (id == 3001U) {
				std::array<std::byte, RSS_PAGE_OWNER_HEADER_BYTES> bytes{};
				if (io_->readAt(offset, bytes.data(), bytes.size()) != bytes.size()) {
					throw std::runtime_error("test owner header read was truncated");
				}
				auto owner = PageOwnerHeaderCodec::decode(bytes);
				++owner.owner_id;
				const auto encoded = PageOwnerHeaderCodec::encode(owner);
				// Fault injection must invalidate the generation-tagged view;
				// generic byte writes deliberately preserve immutable typed frames.
				io_->writeControlRecord({offset, location.generation.value()}, 0U, encoded);
				corrupted = true;
			}
			return true;
		}));
		FAIL() << "cached Catalog must not authenticate a foreign owner";
	} catch (const FormatException &error) {
		EXPECT_EQ(error.error(), FormatError::FERR_INVALID_STATE);
	}
	EXPECT_TRUE(corrupted);
	EXPECT_EQ(visited, (std::vector<uint64_t>{2001U, 2002U, 2003U, 3001U}));
}

TEST_F(EntityPageStoreScanCacheTest, EdgeScanRecordsPerfTraceWhenTracingIsEnabled) {
	graph::debug::PerfTrace::reset();
	graph::debug::PerfTrace::setEnabled(true);
	std::vector<uint64_t> edgeIds;
	EXPECT_TRUE(entities_->visitEdgesUntil(snapshot_, 8U, [&](uint64_t edgeId, const EdgeEntityRecord &) {
		edgeIds.push_back(edgeId);
		return true;
	}));
	const auto trace = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setEnabled(false);

	EXPECT_EQ(edgeIds, (std::vector<uint64_t>{2001U, 2002U, 2003U}));
	EXPECT_TRUE(trace.contains("native_graph.read.edge_entity_record"));
	EXPECT_TRUE(trace.contains("native_graph.read.edge_entity_visitor"));
	ASSERT_TRUE(trace.contains("native_graph.read.edge_page_scan.pages"));
	ASSERT_TRUE(trace.contains("native_graph.read.edge_page_scan.records"));
	EXPECT_EQ(trace.at("native_graph.read.edge_page_scan.pages").totalValue, 1);
	EXPECT_EQ(trace.at("native_graph.read.edge_page_scan.records").totalValue, 3);
}

TEST_F(EntityPageStoreScanCacheTest, SnapshotBoundReadSessionUsesTypedEntityDirectories) {
	NativeReadSession session;
	graph::debug::PerfTrace::reset();
	graph::debug::PerfTrace::setValueCollectionEnabled(true);

	EXPECT_TRUE(entities_->visitNodesUntil(snapshot_, 8U,
			[](uint64_t, const NodeEntityRecord &) { return true; }, &session));
	EXPECT_TRUE(entities_->visitEdgesUntil(snapshot_, 8U,
			[](uint64_t, const EdgeEntityRecord &) { return true; }, &session));

	const auto trace = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setValueCollectionEnabled(false);
	EXPECT_FALSE(trace.contains("native_graph.read.entity_pool_catalog_scan"));
	EXPECT_FALSE(trace.contains("native_graph.read.entity_pool_cache_hit"));
	ASSERT_TRUE(trace.contains("native_graph.read.node_page_scan.pages"));
	ASSERT_TRUE(trace.contains("native_graph.read.edge_page_scan.pages"));
	EXPECT_EQ(trace.at("native_graph.read.node_page_scan.pages").totalValue, 1);
	EXPECT_EQ(trace.at("native_graph.read.edge_page_scan.pages").totalValue, 1);
	ASSERT_TRUE(trace.contains("native_graph.read.shared_page_known_location_pin"));
	EXPECT_EQ(trace.at("native_graph.read.shared_page_known_location_pin").totalValue, 2);
	EXPECT_TRUE(trace.contains("native_graph.read.shared_page_route_resolution"));
}
