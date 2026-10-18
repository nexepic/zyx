/**
 * @file test_MetadataPageRouteDirectory.cpp
 * @brief Physical bootstrap route tests for typed metadata page IDs.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <system_error>
#include <vector>

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/StorageIO.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/relocatable/metadata/MetadataDirectoryEvacuation.hpp"
#include "graph/storage/relocatable/metadata/MetadataPageFile.hpp"
#include "graph/storage/relocatable/metadata/MetadataPageReadContext.hpp"
#include "graph/storage/relocatable/metadata/MetadataPageRouteDirectory.hpp"
#include "graph/storage/relocatable/metadata/MetadataWriteBatch.hpp"

using namespace graph::storage;

namespace {

	void expectFormatError(const std::function<void()> &operation, FormatError expected) {
		try {
			operation();
			FAIL() << "Expected FormatException";
		} catch (const FormatException &error) {
			EXPECT_EQ(error.error(), expected) << error.what();
		}
	}

	Superblock superblock(uint64_t generation, uint64_t checkpointLsn) {
		Superblock result;
		result.database_id = DatabaseId(0x1020304050607080ULL);
		result.generation = Generation(generation);
		result.checkpoint_lsn = LogSequenceNumber(checkpointLsn);
		return result;
	}

	PhysicalPageRef pageRef(uint64_t slot, uint8_t frame, uint64_t generation) {
		return {PhysicalSlotId(slot), frame, Generation(generation)};
	}

	class TestMetadataPageAllocator final : public MetadataPageAllocator {
	public:
		MetadataPageId allocateMetadataPageId() override { return MetadataPageId(nextMetadataPageId++); }

		MetadataPageAllocation allocatePage(Generation generation, LogSequenceNumber,
											std::span<const PhysicalSlotId> reservedSlots) override {
			for (;;) {
				const uint64_t index = nextPhysicalPage++;
				const PhysicalPageRef reference{PhysicalSlotId(index / RSS_PAGE_POOL_DATA_FRAME_COUNT + 1U),
													 static_cast<uint8_t>(index % RSS_PAGE_POOL_DATA_FRAME_COUNT + 1U), generation};
				if (std::ranges::find(reservedSlots, reference.physical_slot) == reservedSlots.end()) {
					return {reference, std::nullopt};
				}
			}
		}

		void abandonPage(PhysicalPageRef) noexcept override {}

	private:
		uint64_t nextMetadataPageId = 1U;
		uint64_t nextPhysicalPage = 128U;
	};

	MetadataPageRoute route(uint64_t metadataPageId, uint64_t slot, uint8_t frame, uint64_t generation) {
		return {MetadataPageId(metadataPageId), pageRef(slot, frame, generation)};
	}

} // namespace

class RelocatableMetadataPageRouteDirectoryTest : public ::testing::Test {
protected:
	std::filesystem::path testFilePath;
	std::shared_ptr<std::fstream> file;
	std::shared_ptr<StorageIO> io;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		testFilePath = std::filesystem::temp_directory_path() /
				("relocatable_metadata_page_route_directory_" + to_string(uuid) + ".dat");
		openFile(std::ios::trunc);
	}

	void TearDown() override {
		graph::debug::PerfTrace::setValueCollectionEnabled(false);
		graph::debug::PerfTrace::reset();
		io.reset();
		if (file && file->is_open()) {
			file->close();
		}
		file.reset();
		std::error_code error;
		std::filesystem::remove(testFilePath, error);
	}

	void reopen() {
		io.reset();
		file->close();
		file.reset();
		openFile({});
	}

	void openFile(std::ios::openmode extraMode) {
		file = std::make_shared<std::fstream>(testFilePath,
				std::ios::in | std::ios::out | std::ios::binary | extraMode);
		ASSERT_TRUE(file->is_open());
		io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	}
};

TEST_F(RelocatableMetadataPageRouteDirectoryTest, ResolvesCompleteRoutesAcrossMultipleRadixLevelsAfterReopen) {
	const Superblock current = superblock(1U, 1U);
	TestMetadataPageAllocator allocator;
	MetadataWriteBatch batch(allocator, current, LogSequenceNumber(2U));
	MetadataPageFile pages(io, batch.candidate());
	MetadataPageRouteDirectory directory(pages);
	const uint64_t farLeafOrdinal =
			static_cast<uint64_t>(RSS_CATALOG_RADIX_FANOUT) * RSS_CATALOG_RADIX_FANOUT;
	const std::vector<MetadataPageRoute> routes = {
			route(1U, 1000U, 1U, 1U),
			route(farLeafOrdinal * RSS_METADATA_PAGE_ROUTE_LEAF_MAX_ENTRIES + 1U, 1001U, 1U, 1U),
	};

	const PhysicalPageRef root = directory.buildFromCompleteRouteSet(batch, routes);
	EXPECT_TRUE(root.isValid());
	EXPECT_TRUE(batch.metadataPageRoutes().empty());
	EXPECT_EQ(pages.readMetadataRadix(root).level, 3U);
	EXPECT_EQ(directory.find(batch.candidate(), root, MetadataPageId(1U)), routes[0].physical_reference);
	EXPECT_EQ(directory.find(batch.candidate(), root, routes[1].page_id), routes[1].physical_reference);
	EXPECT_EQ(directory.collectRoutesFromRouteBranch(batch.candidate(), root), routes);
	const std::vector<PhysicalPageRef> branchFrames = directory.collectRouteBranchFrames(batch.candidate(), root);
	EXPECT_GT(branchFrames.size(), 2U);
	io->sync();

	reopen();
	MetadataPageFile reopenedPages(io, batch.candidate());
	MetadataPageRouteDirectory reopenedDirectory(reopenedPages);
	EXPECT_EQ(reopenedDirectory.find(batch.candidate(), root, MetadataPageId(1U)), routes[0].physical_reference);
	EXPECT_EQ(reopenedDirectory.find(batch.candidate(), root, routes[1].page_id), routes[1].physical_reference);
	EXPECT_EQ(reopenedDirectory.collectRoutesFromRouteBranch(batch.candidate(), root), routes);
	const std::array<MetadataPageId, 3U> ids{routes[1].page_id, routes[0].page_id, routes[1].page_id};
	std::array<PhysicalPageRef, 3U> results{};
	reopenedDirectory.findMany(batch.candidate(), root, ids, results);
	EXPECT_EQ(results[0], routes[1].physical_reference);
	EXPECT_EQ(results[1], routes[0].physical_reference);
	EXPECT_EQ(results[2], routes[1].physical_reference);
}

TEST_F(RelocatableMetadataPageRouteDirectoryTest, AuthenticatedTargetsServeRepeatedPointAndDuplicateBatchReads) {
	TestMetadataPageAllocator allocator;
	MetadataWriteBatch batch(allocator, superblock(1U, 1U), LogSequenceNumber(2U));
	MetadataPageFile pages(io, batch.candidate());
	MetadataPageRouteDirectory directory(pages);
	const std::vector routes{route(1U, 1000U, 1U, 1U), route(2U, 1001U, 1U, 1U)};
	const auto root = directory.buildFromCompleteRouteSet(batch, routes);
	auto context = std::make_shared<MetadataPageReadContext>(batch.candidate());
	MetadataPageReadContext::Scope scope(context, *io);
	EXPECT_EQ(directory.find(batch.candidate(), root, MetadataPageId(1U)), routes[0].physical_reference);
	EXPECT_EQ(directory.find(batch.candidate(), root, MetadataPageId(2U)), routes[1].physical_reference);
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	graph::debug::PerfTrace::reset();
	io->resetStats();
	EXPECT_EQ(directory.find(batch.candidate(), root, MetadataPageId(1U)), routes[0].physical_reference);
	const std::array ids{MetadataPageId(2U), MetadataPageId(1U), MetadataPageId(2U)};
	std::array<PhysicalPageRef, 3U> found{};
	directory.findMany(batch.candidate(), root, ids, found);
	EXPECT_EQ(found[0], routes[1].physical_reference);
	EXPECT_EQ(found[1], routes[0].physical_reference);
	EXPECT_EQ(found[2], routes[1].physical_reference);
	EXPECT_EQ(io->stats().read_bytes, 0U);
	const auto counters = graph::debug::PerfTrace::snapshotAndReset();
	ASSERT_TRUE(counters.contains("native_graph.read.resolved_metadata_route_cache_hits"));
	EXPECT_EQ(counters.at("native_graph.read.resolved_metadata_route_cache_hits").totalValue, 4);
	EXPECT_FALSE(counters.contains("native_graph.read.decoded_metadata_cache_hits"));
}

TEST_F(RelocatableMetadataPageRouteDirectoryTest, CachedTargetsDoNotHideCorruptionAfterAnIoWrite) {
	TestMetadataPageAllocator allocator;
	MetadataWriteBatch batch(allocator, superblock(1U, 1U), LogSequenceNumber(2U));
	MetadataPageFile pages(io, batch.candidate());
	MetadataPageRouteDirectory directory(pages);
	const auto root = directory.buildFromCompleteRouteSet(batch, std::array{route(1U, 1000U, 1U, 1U)});
	auto context = std::make_shared<MetadataPageReadContext>(batch.candidate());
	MetadataPageReadContext::Scope scope(context, *io);
	EXPECT_EQ(directory.find(batch.candidate(), root, MetadataPageId(1U)), pageRef(1000U, 1U, 1U));
	const std::byte corrupt{0U};
	io->writeAt(pages.pageOffset(root), &corrupt, 1U);
	EXPECT_THROW(directory.find(batch.candidate(), root, MetadataPageId(1U)), FormatException);
	const std::array ids{MetadataPageId(1U)};
	std::array<PhysicalPageRef, 1U> found{};
	EXPECT_THROW(directory.findMany(batch.candidate(), root, ids, found), FormatException);
}

TEST_F(RelocatableMetadataPageRouteDirectoryTest, RouteRootsAndVisibilityFencesRemainIndependentWithAWarmCache) {
	TestMetadataPageAllocator allocator;
	MetadataWriteBatch batch(allocator, superblock(1U, 1U), LogSequenceNumber(2U));
	MetadataPageFile pages(io, batch.candidate());
	MetadataPageRouteDirectory directory(pages);
	const auto first = directory.buildFromCompleteRouteSet(batch, std::array{route(1U, 1000U, 1U, 1U)});
	const auto second = directory.buildFromCompleteRouteSet(batch, std::array{route(1U, 2000U, 1U, 1U)});
	auto context = std::make_shared<MetadataPageReadContext>(batch.candidate());
	MetadataPageReadContext::Scope scope(context, *io);
	EXPECT_EQ(directory.find(batch.candidate(), first, MetadataPageId(1U)), pageRef(1000U, 1U, 1U));
	EXPECT_EQ(directory.find(batch.candidate(), second, MetadataPageId(1U)), pageRef(2000U, 1U, 1U));
	for (const bool staleGeneration : {false, true}) {
		auto stale = batch.candidate();
		stale.checkpoint_lsn = LogSequenceNumber(1U);
		if (staleGeneration) stale.generation = Generation(1U);
		EXPECT_THROW(directory.find(stale, first, MetadataPageId(1U)), FormatException);
		const std::array ids{MetadataPageId(1U)};
		std::array<PhysicalPageRef, 1U> found{};
		EXPECT_THROW(directory.findMany(stale, first, ids, found), FormatException);
	}
}

TEST_F(RelocatableMetadataPageRouteDirectoryTest, PartialCacheHitsStillValidateEveryMissingAndInvalidRequest) {
	TestMetadataPageAllocator allocator;
	MetadataWriteBatch batch(allocator, superblock(1U, 1U), LogSequenceNumber(2U));
	MetadataPageFile pages(io, batch.candidate());
	MetadataPageRouteDirectory directory(pages);
	const auto root = directory.buildFromCompleteRouteSet(batch, std::array{route(1U, 1000U, 1U, 1U)});
	auto context = std::make_shared<MetadataPageReadContext>(batch.candidate());
	MetadataPageReadContext::Scope scope(context, *io);
	static_cast<void>(directory.find(batch.candidate(), root, MetadataPageId(1U)));
	std::array<PhysicalPageRef, 2U> found{};
	for (const auto missing : {MetadataPageId{}, MetadataPageId(99U)}) {
		const std::array ids{MetadataPageId(1U), missing};
		EXPECT_THROW(directory.findMany(batch.candidate(), root, ids, found), FormatException);
	}
}

TEST_F(RelocatableMetadataPageRouteDirectoryTest, ZeroCacheBudgetPreservesAuthenticatedUncachedLookup) {
	TestMetadataPageAllocator allocator;
	MetadataWriteBatch batch(allocator, superblock(1U, 1U), LogSequenceNumber(2U));
	MetadataPageFile pages(io, batch.candidate());
	MetadataPageRouteDirectory directory(pages);
	const auto root = directory.buildFromCompleteRouteSet(batch, std::array{route(1U, 1000U, 1U, 1U)});
	auto context = std::make_shared<MetadataPageReadContext>(batch.candidate(), 0U);
	MetadataPageReadContext::Scope scope(context, *io);
	EXPECT_EQ(directory.find(batch.candidate(), root, MetadataPageId(1U)), pageRef(1000U, 1U, 1U));
	const std::array ids{MetadataPageId(1U), MetadataPageId(1U)};
	std::array<PhysicalPageRef, 2U> found{};
	directory.findMany(batch.candidate(), root, ids, found);
	EXPECT_EQ(found[0], found[1]);
	EXPECT_EQ(context->size(), 0U);
	EXPECT_EQ(context->chargedBytes(), 0U);
}

TEST_F(RelocatableMetadataPageRouteDirectoryTest, BatchResolvesSharedRouteLeafWithTwoPageReads) {
	TestMetadataPageAllocator allocator;
	MetadataWriteBatch batch(allocator, superblock(1U, 1U), LogSequenceNumber(2U));
	MetadataPageFile pages(io, batch.candidate());
	MetadataPageRouteDirectory directory(pages);
	std::vector<MetadataPageRoute> routes;
	std::array<MetadataPageId, 64U> ids{};
	std::array<PhysicalPageRef, 64U> results{};
	for (size_t index = 0U; index < ids.size(); ++index) {
		routes.push_back(route(index + 1U, 1000U + index, 1U, 1U));
		ids[ids.size() - index - 1U] = routes.back().page_id;
	}
	const auto root = directory.buildFromCompleteRouteSet(batch, routes);
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	graph::debug::PerfTrace::reset();
	try {
		directory.findMany(batch.candidate(), root, ids, results);
	} catch (...) {
		graph::debug::PerfTrace::setValueCollectionEnabled(false);
		throw;
	}
	const auto events = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setValueCollectionEnabled(false);
	for (size_t index = 0U; index < ids.size(); ++index) {
		EXPECT_EQ(results[index], routes[ids.size() - index - 1U].physical_reference);
	}
	const auto reads = events.find("native_graph.read.metadata_pages_read");
	ASSERT_NE(reads, events.end());
	EXPECT_EQ(reads->second.totalValue, 2);
	graph::debug::PerfTrace::reset();
	const std::array<MetadataPageId, 1U> missing{MetadataPageId(65U)};
	std::array<PhysicalPageRef, 1U> missingResult{};
	expectFormatError([&] { directory.findMany(batch.candidate(), root, missing, missingResult); },
			FormatError::FERR_INVALID_STATE);
	const std::array<MetadataPageId, 1U> invalid{MetadataPageId{}};
	expectFormatError([&] { directory.findMany(batch.candidate(), root, invalid, missingResult); },
			FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { directory.findMany(batch.candidate(), root, ids, missingResult); },
			FormatError::FERR_INVALID_HEADER);
	std::array<MetadataPageId, RSS_CATALOG_RADIX_FANOUT + 1U> oversizedIds;
	oversizedIds.fill(MetadataPageId(1U));
	std::array<PhysicalPageRef, RSS_CATALOG_RADIX_FANOUT + 1U> oversizedResults{};
	expectFormatError([&] { directory.findMany(batch.candidate(), root, oversizedIds, oversizedResults); },
			FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableMetadataPageRouteDirectoryTest, LogicalRadixChildrenShareRouteReadsAndPreserveSparseSlots) {
	TestMetadataPageAllocator allocator;
	MetadataWriteBatch batch(allocator, superblock(1U, 1U), LogSequenceNumber(2U));
	MetadataPageFile pages(io, batch.candidate());
	MetadataPageRouteDirectory directory(pages);
	std::vector<MetadataPageRoute> routes;
	for (uint64_t id = 1U; id <= 64U; ++id) routes.push_back(route(id, 1000U + id, 1U, 1U));
	const auto routeRoot = directory.buildFromCompleteRouteSet(batch, routes);
	MetadataRadixPage parent;
	parent.kind = CatalogRadixPageKind::CRPK_ROOT;
	parent.directory_type = MetadataDirectoryType::MDT_CATALOG;
	parent.page_id = MetadataPageId(10000U);
	parent.generation = Generation(2U);
	parent.committed_lsn = LogSequenceNumber(2U);
	parent.logical_children_enabled = true;
	for (size_t index = 0U; index < routes.size(); ++index) {
		parent.logical_children[index * 2U] = routes[routes.size() - index - 1U].page_id;
	}
	const auto parentReference = pageRef(40U, 1U, 2U);
	pages.writeMetadataRadix(parentReference, parent);
	PageDirectoryBootstrap bootstrap;
	bootstrap.generation = Generation(2U);
	bootstrap.committed_lsn = LogSequenceNumber(2U);
	bootstrap.metadata_page_directory_root = routeRoot;
	const auto bootstrapReference = pageRef(41U, 1U, 2U);
	pages.writePageDirectoryBootstrap(bootstrapReference, bootstrap);
	auto published = batch.candidate();
	published.page_directory_root = bootstrapReference;
	published.flags |= RSS_SUPERBLOCK_FLAG_PAGE_DIRECTORY_BOOTSTRAP;
	MetadataPageFile publishedPages(io, published);
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	graph::debug::PerfTrace::reset();
	MetadataRadixPage resolved;
	try {
		resolved = publishedPages.readMetadataRadix(parentReference);
	} catch (...) {
		graph::debug::PerfTrace::setValueCollectionEnabled(false);
		throw;
	}
	const auto events = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setValueCollectionEnabled(false);
	EXPECT_TRUE(resolved.logical_children_resolved);
	for (size_t index = 0U; index < routes.size(); ++index) {
		EXPECT_EQ(resolved.children[index * 2U], routes[routes.size() - index - 1U].physical_reference);
		EXPECT_FALSE(resolved.children[index * 2U + 1U].isValid());
	}
	const auto reads = events.find("native_graph.read.metadata_pages_read");
	ASSERT_NE(reads, events.end());
	// Parent, bootstrap header, route root, route leaf; no per-child restart.
	EXPECT_EQ(reads->second.totalValue, 4);
}

TEST_F(RelocatableMetadataPageRouteDirectoryTest, RejectsMissingFutureAndWronglyTypedRoutes) {
	const Superblock current = superblock(1U, 1U);
	TestMetadataPageAllocator allocator;
	MetadataWriteBatch batch(allocator, current, LogSequenceNumber(2U));
	MetadataPageFile pages(io, batch.candidate());
	MetadataPageRouteDirectory directory(pages);
	const std::vector<MetadataPageRoute> routes = {route(1U, 1000U, 1U, 1U)};
	const PhysicalPageRef root = directory.buildFromCompleteRouteSet(batch, routes);

	expectFormatError([&] { static_cast<void>(directory.find(batch.candidate(), root, MetadataPageId(2U))); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(directory.find(current, root, MetadataPageId(1U))); },
					  FormatError::FERR_INVALID_HEADER);
	const std::array<MetadataPageId, 1U> ids{MetadataPageId(1U)};
	std::array<PhysicalPageRef, 1U> results{};
	expectFormatError([&] { directory.findMany(current, root, ids, results); }, FormatError::FERR_INVALID_HEADER);
	const auto visibleRoot = pages.readMetadataRadix(root);
	auto futureLeaf = pages.readMetadataPageRouteLeaf(visibleRoot.children[0]);
	futureLeaf.committed_lsn = LogSequenceNumber(3U);
	pages.writeMetadataPageRouteLeaf(visibleRoot.children[0], futureLeaf);
	expectFormatError([&] { directory.findMany(batch.candidate(), root, ids, results); },
			FormatError::FERR_INVALID_HEADER);

	MetadataRadixPage wrongRoot = pages.readMetadataRadix(root);
	wrongRoot.directory_type = MetadataDirectoryType::MDT_PAGE;
	pages.writeMetadataRadix(root, wrongRoot);
	expectFormatError([&] { static_cast<void>(directory.find(batch.candidate(), root, MetadataPageId(1U))); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { directory.findMany(batch.candidate(), root, ids, results); },
			FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableMetadataPageRouteDirectoryTest, RejectsEmptyInvalidAndFutureRouteInputs) {
	const Superblock current = superblock(2U, 2U);
	TestMetadataPageAllocator allocator;
	MetadataWriteBatch batch(allocator, current, LogSequenceNumber(3U));
	MetadataPageFile pages(io, batch.candidate());
	MetadataPageRouteDirectory directory(pages);

	expectFormatError([&] {
		static_cast<void>(directory.buildFromCompleteRouteSet(batch, std::span<const MetadataPageRoute>{}));
	}, FormatError::FERR_INVALID_STATE);

	const MetadataPageRoute zeroLogical{MetadataPageId{}, pageRef(10U, 1U, 1U)};
	expectFormatError([&] {
		static_cast<void>(directory.buildFromCompleteRouteSet(batch, std::span(&zeroLogical, 1U)));
	}, FormatError::FERR_INVALID_HEADER);

	const MetadataPageRoute zeroPhysical{MetadataPageId(1U), PhysicalPageRef{}};
	expectFormatError([&] {
		static_cast<void>(directory.buildFromCompleteRouteSet(batch, std::span(&zeroPhysical, 1U)));
	}, FormatError::FERR_INVALID_HEADER);

	const MetadataPageRoute futurePhysical{MetadataPageId(1U), pageRef(10U, 1U, 4U)};
	expectFormatError([&] {
		static_cast<void>(directory.buildFromCompleteRouteSet(batch, std::span(&futurePhysical, 1U)));
	}, FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableMetadataPageRouteDirectoryTest, RejectsMissingAndWronglyTypedRouteDescendants) {
	const Superblock current = superblock(1U, 1U);
	TestMetadataPageAllocator allocator;
	MetadataWriteBatch batch(allocator, current, LogSequenceNumber(2U));
	MetadataPageFile pages(io, batch.candidate());
	MetadataPageRouteDirectory directory(pages);

	const PhysicalPageRef sparseRoot = directory.buildFromCompleteRouteSet(
			batch, std::vector{route(RSS_METADATA_PAGE_ROUTE_LEAF_MAX_ENTRIES + 1U, 600U, 1U, 1U)});
	expectFormatError([&] { static_cast<void>(directory.find(batch.candidate(), sparseRoot, MetadataPageId(1U))); },
					  FormatError::FERR_INVALID_STATE);

	MetadataRadixPage missingRoot;
	missingRoot.kind = CatalogRadixPageKind::CRPK_ROOT;
	missingRoot.directory_type = MetadataDirectoryType::MDT_METADATA_PAGE_ROUTE;
	missingRoot.page_id = MetadataPageId(401U);
	missingRoot.generation = Generation(1U);
	missingRoot.committed_lsn = LogSequenceNumber(1U);

	MetadataRadixPage internalRoot = missingRoot;
	internalRoot.level = 1U;
	internalRoot.page_id = MetadataPageId(402U);
	const PhysicalPageRef internalRootReference = pageRef(22U, 1U, 1U);
	const PhysicalPageRef malformedChildReference = pageRef(22U, 2U, 1U);
	internalRoot.children[0] = malformedChildReference;
	pages.writeMetadataRadix(internalRootReference, internalRoot);
	MetadataRadixPage malformedChild = missingRoot;
	malformedChild.page_id = MetadataPageId(403U);
	pages.writeMetadataRadix(malformedChildReference, malformedChild);
	expectFormatError([&] { static_cast<void>(directory.find(batch.candidate(), internalRootReference, MetadataPageId(1U))); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableMetadataPageRouteDirectoryTest, ResolvesLogicalRadixChildrenThroughPublishedBootstrapRoutes) {
	const Superblock current = superblock(1U, 1U);
	const PhysicalPageRef parentReference = pageRef(41U, 1U, 1U);
	const PhysicalPageRef childReference = pageRef(42U, 1U, 1U);
	const PhysicalPageRef bootstrapReference = pageRef(43U, 1U, 2U);
	MetadataPageFile pages(io, current);
	TestMetadataPageAllocator allocator;
	MetadataWriteBatch batch(allocator, current, LogSequenceNumber(2U));
	MetadataPageRouteDirectory directory(pages);
	const PhysicalPageRef routeRoot =
			directory.buildFromCompleteRouteSet(batch, std::vector{route(900U, childReference.physical_slot.value(),
																				 childReference.frame_index, 1U)});

	MetadataRadixPage parent;
	parent.kind = CatalogRadixPageKind::CRPK_ROOT;
	parent.directory_type = MetadataDirectoryType::MDT_CATALOG;
	parent.level = 0U;
	parent.page_id = MetadataPageId(901U);
	parent.generation = Generation(1U);
	parent.committed_lsn = LogSequenceNumber(1U);
	parent.logical_children_enabled = true;
	parent.logical_children[0] = MetadataPageId(900U);
	pages.writeMetadataRadix(parentReference, parent);

	PageDirectoryBootstrap bootstrap;
	bootstrap.generation = Generation(2U);
	bootstrap.committed_lsn = LogSequenceNumber(2U);
	bootstrap.metadata_page_directory_root = routeRoot;
	pages.writePageDirectoryBootstrap(bootstrapReference, bootstrap);

	Superblock published = superblock(2U, 2U);
	published.page_directory_root = bootstrapReference;
	published.flags |= RSS_SUPERBLOCK_FLAG_PAGE_DIRECTORY_BOOTSTRAP;
	MetadataPageFile publishedPages(io, published);
	const MetadataRadixPage resolved = publishedPages.readMetadataRadix(parentReference);
	EXPECT_TRUE(resolved.logical_children_enabled);
	EXPECT_EQ(resolved.logical_children[0], MetadataPageId(900U));
	EXPECT_EQ(resolved.children[0], childReference);

	Superblock candidate = published;
	candidate.generation = Generation(3U);
	candidate.checkpoint_lsn = LogSequenceNumber(3U);
	EXPECT_EQ(publishedPages.readMetadataRadix(parentReference, candidate).children[0], childReference);
	candidate.generation = Generation(4U);
	candidate.checkpoint_lsn = LogSequenceNumber(4U);
	expectFormatError([&] { static_cast<void>(publishedPages.readMetadataRadix(parentReference, candidate)); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataPageRouteDirectoryTest, RejectsUnorderedIncompleteOrFutureRouteSets) {
	const Superblock current = superblock(1U, 1U);
	TestMetadataPageAllocator allocator;
	MetadataWriteBatch batch(allocator, current, LogSequenceNumber(2U));
	MetadataPageFile pages(io, batch.candidate());
	MetadataPageRouteDirectory directory(pages);

	expectFormatError([&] { static_cast<void>(directory.buildFromCompleteRouteSet(batch, {})); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError(
			[&] {
				static_cast<void>(directory.buildFromCompleteRouteSet(
						batch, std::vector{route(2U, 1001U, 1U, 1U), route(1U, 1000U, 1U, 1U)}));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(directory.buildFromCompleteRouteSet(batch, std::vector{route(1U, 1000U, 1U, 3U)}));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(directory.buildFromCompleteRouteSet(batch, std::vector{route(0U, 1000U, 1U, 1U)}));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(directory.buildFromCompleteRouteSet(batch, std::vector{route(1U, 0U, 0U, 1U)}));
			},
			FormatError::FERR_INVALID_HEADER);

	const PhysicalPageRef root = directory.buildFromCompleteRouteSet(batch, std::vector{route(1U, 1000U, 1U, 1U)});
	expectFormatError([&] { static_cast<void>(directory.find(batch.candidate(), {}, MetadataPageId(1U))); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(directory.find(batch.candidate(), root, {})); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(directory.nextBootstrapFrameCount(batch)); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(directory.publishNextBootstrap(batch, {})); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataPageRouteDirectoryTest, RejectsRouteLeavesThatDoNotMatchTheirRadixOrdinal) {
	const Superblock current = superblock(1U, 1U);
	TestMetadataPageAllocator allocator;
	MetadataWriteBatch batch(allocator, current, LogSequenceNumber(2U));
	MetadataPageFile pages(io, batch.candidate());
	MetadataPageRouteDirectory directory(pages);
	const PhysicalPageRef root = directory.buildFromCompleteRouteSet(batch, std::vector{route(1U, 1000U, 1U, 1U)});
	MetadataRadixPage radix = pages.readMetadataRadix(root);
	ASSERT_TRUE(radix.children[0].isValid());
	MetadataPageRouteLeafPage leaf = pages.readMetadataPageRouteLeaf(radix.children[0]);
	ASSERT_EQ(leaf.routes.size(), 1U);
	leaf.routes[0].page_id = MetadataPageId(RSS_METADATA_PAGE_ROUTE_LEAF_MAX_ENTRIES + 1U);
	pages.writeMetadataPageRouteLeaf(radix.children[0], leaf);
	expectFormatError([&] { static_cast<void>(directory.find(batch.candidate(), root, MetadataPageId(1U))); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(directory.collectRoutesFromRouteBranch(batch.candidate(), root)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableMetadataPageRouteDirectoryTest, RejectsInvalidOrFutureRouteBranchReferences) {
	const Superblock current = superblock(1U, 1U);
	TestMetadataPageAllocator allocator;
	MetadataWriteBatch batch(allocator, current, LogSequenceNumber(2U));
	MetadataPageFile pages(io, batch.candidate());
	MetadataPageRouteDirectory directory(pages);

	expectFormatError([&] {
		static_cast<void>(directory.collectRoutesFromRouteBranch(current, PhysicalPageRef{}));
	}, FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] {
		static_cast<void>(directory.collectRoutesFromRouteBranch(current, pageRef(9U, 1U, 2U)));
	}, FormatError::FERR_INVALID_HEADER);

	const PhysicalPageRef rootReference = pageRef(9U, 1U, 1U);
	MetadataRadixPage root;
	root.kind = CatalogRadixPageKind::CRPK_ROOT;
	root.directory_type = MetadataDirectoryType::MDT_METADATA_PAGE_ROUTE;
	root.page_id = MetadataPageId(1U);
	root.generation = Generation(1U);
	root.committed_lsn = LogSequenceNumber(2U);
	pages.writeMetadataRadix(rootReference, root);
	expectFormatError([&] {
		static_cast<void>(directory.collectRoutesFromRouteBranch(current, rootReference));
	}, FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableMetadataPageRouteDirectoryTest, RejectsAliasedRouteBranchFramesAndTargets) {
	const Superblock current = superblock(1U, 1U);
	MetadataPageFile pages(io, current);
	MetadataPageRouteDirectory directory(pages);

	const PhysicalPageRef rootReference = pageRef(19U, 1U, 1U);
	const PhysicalPageRef internalReference = pageRef(20U, 1U, 1U);
	const PhysicalPageRef leafReference = pageRef(21U, 1U, 1U);
	MetadataRadixPage root;
	root.kind = CatalogRadixPageKind::CRPK_ROOT;
	root.directory_type = MetadataDirectoryType::MDT_METADATA_PAGE_ROUTE;
	root.level = 1U;
	root.page_id = MetadataPageId(1U);
	root.generation = Generation(1U);
	root.committed_lsn = LogSequenceNumber(1U);
	root.children[0] = internalReference;
	root.children[1] = internalReference;
	MetadataRadixPage internal = root;
	internal.kind = CatalogRadixPageKind::CRPK_INTERNAL;
	internal.level = 0U;
	internal.page_id = MetadataPageId(2U);
	internal.children.fill({});
	internal.children[0] = leafReference;
	MetadataPageRouteLeafPage leaf;
	leaf.page_id = MetadataPageId(3U);
	leaf.generation = Generation(1U);
	leaf.committed_lsn = LogSequenceNumber(1U);
	leaf.routes = {route(1U, 1000U, 1U, 1U)};
	pages.writeMetadataRadix(rootReference, root);
	pages.writeMetadataRadix(internalReference, internal);
	pages.writeMetadataPageRouteLeaf(leafReference, leaf);
	expectFormatError([&] { static_cast<void>(directory.collectRoutesFromRouteBranch(current, rootReference)); },
					  FormatError::FERR_INVALID_STATE);

	TestMetadataPageAllocator allocator;
	MetadataWriteBatch batch(allocator, current, LogSequenceNumber(2U));
	MetadataPageFile batchPages(io, batch.candidate());
	MetadataPageRouteDirectory batchDirectory(batchPages);
	const PhysicalPageRef routeRoot = batchDirectory.buildFromCompleteRouteSet(
			batch, std::vector{route(1U, 1000U, 1U, 1U), route(2U, 1001U, 1U, 1U)});
	MetadataRadixPage batchRadix = batchPages.readMetadataRadix(routeRoot);
	MetadataPageRouteLeafPage batchLeaf = batchPages.readMetadataPageRouteLeaf(batchRadix.children[0]);
	ASSERT_EQ(batchLeaf.routes.size(), 2U);
	batchLeaf.routes[1].physical_reference = batchLeaf.routes[0].physical_reference;
	batchPages.writeMetadataPageRouteLeaf(batchRadix.children[0], batchLeaf);
	expectFormatError([&] { static_cast<void>(batchDirectory.collectRoutesFromRouteBranch(batch.candidate(), routeRoot)); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataPageRouteDirectoryTest, RejectsRouteTargetsThatAliasBootstrapPlumbing) {
	const Superblock current = superblock(1U, 1U);
	TestMetadataPageAllocator allocator;
	MetadataWriteBatch batch(allocator, current, LogSequenceNumber(2U));
	MetadataPageFile pages(io, batch.candidate());
	MetadataPageRouteDirectory directory(pages);
	const PhysicalPageRef routeRoot = directory.buildFromCompleteRouteSet(
			batch, std::vector{route(1U, 1000U, 1U, 1U)});
	MetadataRadixPage radix = pages.readMetadataRadix(routeRoot);
	ASSERT_TRUE(radix.children[0].isValid());
	MetadataPageRouteLeafPage leaf = pages.readMetadataPageRouteLeaf(radix.children[0]);
	ASSERT_EQ(leaf.routes.size(), 1U);
	// Route targets must never point at the bootstrap frames used to locate them.
	leaf.routes[0].physical_reference = routeRoot;
	pages.writeMetadataPageRouteLeaf(radix.children[0], leaf);

	expectFormatError([&] { static_cast<void>(directory.collectRoutesFromRouteBranch(batch.candidate(), routeRoot)); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataPageRouteDirectoryTest, RejectsWronglyTypedInternalRouteChildDuringLookup) {
	const Superblock current = superblock(1U, 1U);
	TestMetadataPageAllocator allocator;
	MetadataWriteBatch batch(allocator, current, LogSequenceNumber(2U));
	MetadataPageFile pages(io, batch.candidate());
	MetadataPageRouteDirectory directory(pages);
	const PhysicalPageRef root = directory.buildFromCompleteRouteSet(
			batch, std::vector{route(1U, 1000U, 1U, 1U),
								 route(RSS_METADATA_PAGE_ROUTE_LEAF_MAX_ENTRIES + 1U, 1001U, 1U, 1U)});
	MetadataRadixPage rootPage = pages.readMetadataRadix(root);
	ASSERT_TRUE(rootPage.children[0].isValid());
	MetadataRadixPage child = pages.readMetadataRadix(rootPage.children[0]);
	child.directory_type = MetadataDirectoryType::MDT_PAGE;
	pages.writeMetadataRadix(rootPage.children[0], child);
	expectFormatError([&] { static_cast<void>(directory.find(batch.candidate(), root, MetadataPageId(1U))); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableMetadataPageRouteDirectoryTest, RejectsInvalidVisibilityAndRouteIdentityMetadata) {
	const Superblock current = superblock(1U, 1U);
	TestMetadataPageAllocator allocator;
	MetadataWriteBatch batch(allocator, current, LogSequenceNumber(2U));
	MetadataPageFile pages(io, batch.candidate());
	MetadataPageRouteDirectory directory(pages);

	const PhysicalPageRef routeRoot = directory.buildFromCompleteRouteSet(
			batch, std::vector{route(1U, 1000U, 1U, 1U)});
	MetadataRadixPage radix = pages.readMetadataRadix(routeRoot);
	ASSERT_TRUE(radix.children[0].isValid());
	MetadataPageRouteLeafPage leaf = pages.readMetadataPageRouteLeaf(radix.children[0]);
	ASSERT_EQ(leaf.routes.size(), 1U);
	leaf.routes[0].physical_reference.generation = Generation(3U);
	pages.writeMetadataPageRouteLeaf(radix.children[0], leaf);
	expectFormatError([&] { static_cast<void>(directory.collectRoutesFromRouteBranch(batch.candidate(), routeRoot)); },
					  FormatError::FERR_INVALID_HEADER);

	leaf.routes[0].physical_reference.generation = batch.generation();
	leaf.committed_lsn = LogSequenceNumber(3U);
	pages.writeMetadataPageRouteLeaf(radix.children[0], leaf);
	expectFormatError([&] { static_cast<void>(directory.collectRoutesFromRouteBranch(batch.candidate(), routeRoot)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableMetadataPageRouteDirectoryTest, RejectsEmptyPhysicalDirectoryRouteSet) {
	const Superblock current = superblock(1U, 1U);
	MetadataPageFile pages(io, current);
	MetadataPageRouteDirectory directory(pages);
	expectFormatError([&] { static_cast<void>(directory.initialBootstrapFrameCount(current)); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataPageRouteDirectoryTest, RejectsFuturePhysicalDirectoryRoot) {
	Superblock current = superblock(1U, 1U);
	current.catalog_root = pageRef(9U, 1U, 2U);
	MetadataPageFile pages(io, current);
	MetadataPageRouteDirectory directory(pages);
	expectFormatError([&] { static_cast<void>(directory.collectRoutesFromPhysicalDirectoryRoots(current)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableMetadataPageRouteDirectoryTest, RejectsBootstrapRootMissingFromTheCompleteRouteSet) {
	Superblock current = superblock(1U, 1U);
	const PhysicalPageRef catalogRoot = pageRef(1U, 1U, 1U);
	const PhysicalPageRef catalogLeaf = pageRef(1U, 2U, 1U);
	current.catalog_root = catalogRoot;
	TestMetadataPageAllocator allocator;
	MetadataWriteBatch batch(allocator, current, LogSequenceNumber(2U));
	MetadataPageFile pages(io, batch.candidate());
	CatalogRadixPage root;
	root.kind = CatalogRadixPageKind::CRPK_ROOT;
	root.directory_type = MetadataDirectoryType::MDT_CATALOG;
	root.level = 0U;
	root.page_id = MetadataPageId(101U);
	root.generation = Generation(1U);
	root.committed_lsn = LogSequenceNumber(1U);
	root.children[0] = catalogLeaf;
	CatalogLeafPage leaf;
	leaf.page_id = MetadataPageId(102U);
	leaf.minimum_segment_id = SegmentId(1U);
	leaf.maximum_segment_id = SegmentId(1U);
	leaf.generation = Generation(1U);
	leaf.committed_lsn = LogSequenceNumber(1U);
	leaf.entries = {{SegmentId(1U), PhysicalSlotId(100U), SegmentKind::SGK_ENTITY_PAGE_POOL, 7U,
					 Generation(1U), SegmentState::SGS_SEALED, LogSequenceNumber(1U)}};
	pages.writeCatalogRadix(catalogRoot, root);
	pages.writeCatalogLeaf(catalogLeaf, leaf);

	// A replacement root is not publishable until its metadata route is registered in this batch.
	batch.setCatalogRoot(batch.allocatePage());
	MetadataPageRouteDirectory directory(pages);
	expectFormatError([&] { static_cast<void>(directory.prepareInitialBootstrapMigration(batch)); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataPageRouteDirectoryTest, CountsAllFramesBeforeInitialBootstrapAllocation) {
	Superblock current = superblock(1U, 1U);
	const PhysicalPageRef catalogRoot = pageRef(1U, 1U, 1U);
	const PhysicalPageRef catalogLeaf = pageRef(1U, 2U, 1U);
	current.catalog_root = catalogRoot;
	MetadataPageFile pages(io, current);
	CatalogRadixPage root;
	root.kind = CatalogRadixPageKind::CRPK_ROOT;
	root.directory_type = MetadataDirectoryType::MDT_CATALOG;
	root.level = 0U;
	root.page_id = MetadataPageId(1U);
	root.generation = Generation(1U);
	root.committed_lsn = LogSequenceNumber(1U);
	root.children[0] = catalogLeaf;
	CatalogLeafPage leaf;
	leaf.page_id = MetadataPageId(RSS_METADATA_PAGE_ROUTE_LEAF_MAX_ENTRIES + 1U);
	leaf.minimum_segment_id = SegmentId(1U);
	leaf.maximum_segment_id = SegmentId(1U);
	leaf.generation = Generation(1U);
	leaf.committed_lsn = LogSequenceNumber(1U);
	leaf.entries = {{SegmentId(1U), PhysicalSlotId(100U), SegmentKind::SGK_ENTITY_PAGE_POOL, 7U,
						 Generation(1U), SegmentState::SGS_SEALED, LogSequenceNumber(1U)}};
	pages.writeCatalogRadix(catalogRoot, root);
	pages.writeCatalogLeaf(catalogLeaf, leaf);
	MetadataPageRouteDirectory directory(pages);
	// Distinct route-leaf ordinals require two leaves, one radix page, a root,
	// and the Bootstrap header.
	EXPECT_EQ(directory.initialBootstrapFrameCount(current), 5U);
}

TEST_F(RelocatableMetadataPageRouteDirectoryTest, CollectsEveryVisiblePhysicalDirectoryPageBeforeBootstrapMigration) {
	Superblock current = superblock(1U, 1U);
	const PhysicalPageRef catalogRoot = pageRef(1U, 1U, 1U);
	const PhysicalPageRef catalogLeaf = pageRef(1U, 2U, 1U);
	const PhysicalPageRef pageRoot = pageRef(1U, 3U, 1U);
	const PhysicalPageRef pageLeaf = pageRef(1U, 4U, 1U);
	current.catalog_root = catalogRoot;
	current.page_directory_root = pageRoot;
	MetadataPageFile pages(io, current);

	CatalogRadixPage catalogRadix;
	catalogRadix.kind = CatalogRadixPageKind::CRPK_ROOT;
	catalogRadix.directory_type = MetadataDirectoryType::MDT_CATALOG;
	catalogRadix.level = 0U;
	catalogRadix.page_id = MetadataPageId(101U);
	catalogRadix.generation = Generation(1U);
	catalogRadix.committed_lsn = LogSequenceNumber(1U);
	catalogRadix.children[0] = catalogLeaf;
	CatalogLeafPage catalog;
	catalog.page_id = MetadataPageId(102U);
	catalog.minimum_segment_id = SegmentId(1U);
	catalog.maximum_segment_id = SegmentId(1U);
	catalog.generation = Generation(1U);
	catalog.committed_lsn = LogSequenceNumber(1U);
	catalog.entries = {{SegmentId(1U), PhysicalSlotId(100U), SegmentKind::SGK_ENTITY_PAGE_POOL, 7U,
							Generation(1U), SegmentState::SGS_SEALED, LogSequenceNumber(1U)}};
	pages.writeCatalogRadix(catalogRoot, catalogRadix);
	pages.writeCatalogLeaf(catalogLeaf, catalog);

	CatalogRadixPage locationRadix = catalogRadix;
	locationRadix.directory_type = MetadataDirectoryType::MDT_PAGE;
	locationRadix.page_id = MetadataPageId(201U);
	locationRadix.children.fill({});
	locationRadix.children[0] = pageLeaf;
	LocationLeafPage location;
	location.directory_type = MetadataDirectoryType::MDT_PAGE;
	location.page_id = MetadataPageId(202U);
	location.generation = Generation(1U);
	location.committed_lsn = LogSequenceNumber(1U);
	location.entries = {{1U, SegmentId(22U), Generation(1U), 1U, 1U}};
	pages.writeMetadataRadix(pageRoot, locationRadix);
	pages.writeLocationLeaf(pageLeaf, location);

	MetadataPageRouteDirectory directory(pages);
	EXPECT_EQ(directory.collectRoutesFromPhysicalDirectoryRoots(current),
			  (std::vector<MetadataPageRoute>{{MetadataPageId(101U), catalogRoot},
													 {MetadataPageId(102U), catalogLeaf},
													 {MetadataPageId(201U), pageRoot},
													 {MetadataPageId(202U), pageLeaf}}));

	location.page_id = MetadataPageId(101U);
	pages.writeLocationLeaf(pageLeaf, location);
	expectFormatError([&] { static_cast<void>(directory.collectRoutesFromPhysicalDirectoryRoots(current)); },
					  FormatError::FERR_INVALID_STATE);

	location.page_id = MetadataPageId(202U);
	pages.writeLocationLeaf(pageLeaf, location);
	catalog.entries.clear();
	catalog.minimum_segment_id = {};
	catalog.maximum_segment_id = {};
	pages.writeCatalogLeaf(catalogLeaf, catalog);
	expectFormatError([&] { static_cast<void>(directory.collectRoutesFromPhysicalDirectoryRoots(current)); },
					  FormatError::FERR_INVALID_HEADER);

	catalog.minimum_segment_id = SegmentId(RSS_CATALOG_PAGE_MAX_ENTRIES + 1U);
	catalog.maximum_segment_id = catalog.minimum_segment_id;
	catalog.entries = {{catalog.minimum_segment_id, PhysicalSlotId(100U), SegmentKind::SGK_ENTITY_PAGE_POOL, 7U,
						Generation(1U), SegmentState::SGS_SEALED, LogSequenceNumber(1U)}};
	pages.writeCatalogLeaf(catalogLeaf, catalog);
	expectFormatError([&] { static_cast<void>(directory.collectRoutesFromPhysicalDirectoryRoots(current)); },
					  FormatError::FERR_INVALID_HEADER);

}

TEST_F(RelocatableMetadataPageRouteDirectoryTest, CollectsAllocatorAndExtentDirectoryRoutes) {
	Superblock current = superblock(1U, 1U);
	const PhysicalPageRef allocatorRoot = pageRef(2U, 1U, 1U);
	const PhysicalPageRef allocatorLeaf = pageRef(2U, 2U, 1U);
	const PhysicalPageRef extentRoot = pageRef(2U, 3U, 1U);
	const PhysicalPageRef extentLeaf = pageRef(2U, 4U, 1U);
	current.allocator_root = allocatorRoot;
	current.extent_directory_root = extentRoot;
	MetadataPageFile pages(io, current);

	MetadataRadixPage allocatorRadix;
	allocatorRadix.kind = CatalogRadixPageKind::CRPK_ROOT;
	allocatorRadix.directory_type = MetadataDirectoryType::MDT_ALLOCATOR;
	allocatorRadix.level = 0U;
	allocatorRadix.page_id = MetadataPageId(301U);
	allocatorRadix.generation = Generation(1U);
	allocatorRadix.committed_lsn = LogSequenceNumber(1U);
	allocatorRadix.children[0] = allocatorLeaf;
	AllocatorLeafPage allocator;
	allocator.page_id = MetadataPageId(302U);
	allocator.first_slot = PhysicalSlotId(1U);
	allocator.slot_count = 1U;
	allocator.generation = Generation(1U);
	allocator.committed_lsn = LogSequenceNumber(1U);
	pages.writeMetadataRadix(allocatorRoot, allocatorRadix);
	pages.writeAllocatorLeaf(allocatorLeaf, allocator);

	MetadataRadixPage extentRadix = allocatorRadix;
	extentRadix.directory_type = MetadataDirectoryType::MDT_EXTENT;
	extentRadix.page_id = MetadataPageId(401U);
	extentRadix.children.fill({});
	extentRadix.children[0] = extentLeaf;
	LocationLeafPage extent;
	extent.directory_type = MetadataDirectoryType::MDT_EXTENT;
	extent.page_id = MetadataPageId(402U);
	extent.generation = Generation(1U);
	extent.committed_lsn = LogSequenceNumber(1U);
	extent.entries = {{1U, SegmentId(22U), Generation(1U), 1U, 1U, 0U}};
	pages.writeMetadataRadix(extentRoot, extentRadix);
	pages.writeLocationLeaf(extentLeaf, extent);

	MetadataPageRouteDirectory directory(pages);
	EXPECT_EQ(directory.collectRoutesFromPhysicalDirectoryRoots(current),
			  (std::vector<MetadataPageRoute>{{MetadataPageId(301U), allocatorRoot},
											 {MetadataPageId(302U), allocatorLeaf},
											 {MetadataPageId(401U), extentRoot},
											 {MetadataPageId(402U), extentLeaf}}));

	extent.directory_type = MetadataDirectoryType::MDT_PAGE;
	pages.writeLocationLeaf(extentLeaf, extent);
	expectFormatError([&] { static_cast<void>(directory.collectRoutesFromPhysicalDirectoryRoots(current)); },
					  FormatError::FERR_INVALID_HEADER);

	extent.directory_type = MetadataDirectoryType::MDT_EXTENT;
	extent.entries.front().logical_id = RSS_LOCATION_LEAF_MAX_ENTRIES + 1U;
	pages.writeLocationLeaf(extentLeaf, extent);
	expectFormatError([&] { static_cast<void>(directory.collectRoutesFromPhysicalDirectoryRoots(current)); },
					  FormatError::FERR_INVALID_HEADER);

	allocator.first_slot = PhysicalSlotId(2U);
	pages.writeAllocatorLeaf(allocatorLeaf, allocator);
	expectFormatError([&] { static_cast<void>(directory.collectRoutesFromPhysicalDirectoryRoots(current)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableMetadataPageRouteDirectoryTest, StagedBootstrapOverlaysFinalBatchFramesWithoutDroppingCurrentRoutes) {
	Superblock current = superblock(1U, 1U);
	const PhysicalPageRef catalogRoot = pageRef(1U, 1U, 1U);
	const PhysicalPageRef catalogLeaf = pageRef(1U, 2U, 1U);
	current.catalog_root = catalogRoot;
	TestMetadataPageAllocator allocator;
	MetadataWriteBatch batch(allocator, current, LogSequenceNumber(2U));
	MetadataPageFile pages(io, batch.candidate());

	CatalogRadixPage root;
	root.kind = CatalogRadixPageKind::CRPK_ROOT;
	root.directory_type = MetadataDirectoryType::MDT_CATALOG;
	root.level = 0U;
	root.page_id = MetadataPageId(101U);
	root.generation = Generation(1U);
	root.committed_lsn = LogSequenceNumber(1U);
	root.children[0] = catalogLeaf;
	CatalogLeafPage original;
	original.page_id = MetadataPageId(102U);
	original.minimum_segment_id = SegmentId(1U);
	original.maximum_segment_id = SegmentId(1U);
	original.generation = Generation(1U);
	original.committed_lsn = LogSequenceNumber(1U);
	original.entries = {{SegmentId(1U), PhysicalSlotId(100U), SegmentKind::SGK_ENTITY_PAGE_POOL, 7U,
								 Generation(1U), SegmentState::SGS_SEALED, LogSequenceNumber(1U)}};
	pages.writeCatalogRadix(catalogRoot, root);
	pages.writeCatalogLeaf(catalogLeaf, original);

	CatalogLeafPage replacement = original;
	replacement.generation = batch.generation();
	replacement.committed_lsn = batch.checkpointLsn();
	const PhysicalPageRef replacementRef = batch.allocatePage();
	pages.writeCatalogLeaf(replacementRef, replacement);
	batch.registerMetadataPageRoute(replacement.page_id, replacementRef);

	MetadataPageRouteDirectory directory(pages);
	const PhysicalPageRef bootstrapReference = directory.prepareInitialBootstrapMigration(batch);
	EXPECT_EQ(batch.candidate().page_directory_root, bootstrapReference);
	EXPECT_TRUE(batch.candidate().usesPageDirectoryBootstrap());
	const PageDirectoryBootstrap bootstrap = pages.readPageDirectoryBootstrap(bootstrapReference);
	EXPECT_FALSE(bootstrap.data_page_directory_root.isValid());
	EXPECT_EQ(bootstrap.metadata_directory_roots[static_cast<size_t>(MetadataDirectoryRootOrdinal::MDRO_CATALOG)],
			  MetadataPageId(101U));
	EXPECT_FALSE(bootstrap.metadata_directory_roots[static_cast<size_t>(MetadataDirectoryRootOrdinal::MDRO_ALLOCATOR)].isValid());
	EXPECT_EQ(directory.find(batch.candidate(), bootstrap.metadata_page_directory_root, MetadataPageId(101U)),
			  catalogRoot);
	EXPECT_EQ(directory.find(batch.candidate(), bootstrap.metadata_page_directory_root, MetadataPageId(102U)),
			  replacementRef);
	EXPECT_EQ(directory.collectRoutesFromRouteBranch(batch.candidate(), bootstrap.metadata_page_directory_root),
			  (std::vector<MetadataPageRoute>{{MetadataPageId(101U), catalogRoot},
													 {MetadataPageId(102U), replacementRef}}));
}

TEST_F(RelocatableMetadataPageRouteDirectoryTest, InitialRouteBranchOverlaysPhysicalRootsWithBatchRoutes) {
	Superblock current = superblock(1U, 1U);
	const PhysicalPageRef catalogRoot = pageRef(1U, 1U, 1U);
	const PhysicalPageRef catalogLeaf = pageRef(1U, 2U, 1U);
	current.catalog_root = catalogRoot;
	TestMetadataPageAllocator allocator;
	MetadataWriteBatch batch(allocator, current, LogSequenceNumber(2U));
	MetadataPageFile pages(io, batch.candidate());

	CatalogRadixPage root;
	root.kind = CatalogRadixPageKind::CRPK_ROOT;
	root.directory_type = MetadataDirectoryType::MDT_CATALOG;
	root.level = 0U;
	root.page_id = MetadataPageId(101U);
	root.generation = Generation(1U);
	root.committed_lsn = LogSequenceNumber(1U);
	root.children[0] = catalogLeaf;
	CatalogLeafPage leaf;
	leaf.page_id = MetadataPageId(102U);
	leaf.minimum_segment_id = SegmentId(1U);
	leaf.maximum_segment_id = SegmentId(1U);
	leaf.generation = Generation(1U);
	leaf.committed_lsn = LogSequenceNumber(1U);
	leaf.entries = {{SegmentId(1U), PhysicalSlotId(100U), SegmentKind::SGK_ENTITY_PAGE_POOL, 7U,
					 Generation(1U), SegmentState::SGS_SEALED, LogSequenceNumber(1U)}};
	pages.writeCatalogRadix(catalogRoot, root);
	pages.writeCatalogLeaf(catalogLeaf, leaf);

	const PhysicalPageRef replacement = batch.allocatePage();
	batch.registerMetadataPageRoute(MetadataPageId(102U), replacement);
	MetadataPageRouteDirectory directory(pages);
	const PhysicalPageRef routeRoot = directory.buildInitialRouteBranch(batch);
	EXPECT_TRUE(routeRoot.isValid());
	EXPECT_EQ(directory.find(batch.candidate(), routeRoot, MetadataPageId(101U)), catalogRoot);
	EXPECT_EQ(directory.find(batch.candidate(), routeRoot, MetadataPageId(102U)), replacement);
}

TEST_F(RelocatableMetadataPageRouteDirectoryTest, NextRouteBranchOverlaysPublishedRoutesWithCurrentBatchFrames) {
	TestMetadataPageAllocator allocator;
	const Superblock initial = superblock(1U, 1U);
	PhysicalPageRef publishedRoot;
	{
		MetadataWriteBatch firstBatch(allocator, initial, LogSequenceNumber(2U));
		MetadataPageFile pages(io, firstBatch.candidate());
		MetadataPageRouteDirectory directory(pages);
		publishedRoot = directory.buildFromCompleteRouteSet(
				firstBatch, std::vector{route(1U, 1000U, 1U, 1U), route(2U, 1001U, 1U, 1U)});
	}

	const Superblock published = superblock(2U, 2U);
	MetadataWriteBatch nextBatch(allocator, published, LogSequenceNumber(3U));
	MetadataPageFile pages(io, nextBatch.candidate());
	MetadataPageRouteDirectory directory(pages);
	const PhysicalPageRef replacement = nextBatch.allocatePage();
	nextBatch.registerMetadataPageRoute(MetadataPageId(2U), replacement);
	const PhysicalPageRef nextRoot = directory.buildNextRouteBranch(nextBatch, publishedRoot);
	EXPECT_EQ(directory.find(nextBatch.candidate(), nextRoot, MetadataPageId(1U)), pageRef(1000U, 1U, 1U));
	EXPECT_EQ(directory.find(nextBatch.candidate(), nextRoot, MetadataPageId(2U)), replacement);
}

TEST_F(RelocatableMetadataPageRouteDirectoryTest, CountsExactFramesForNextBootstrapRoutePublication) {
	TestMetadataPageAllocator allocator;
	const Superblock initial = superblock(1U, 1U);
	PhysicalPageRef bootstrapReference;
	{
		MetadataWriteBatch firstBatch(allocator, initial, LogSequenceNumber(2U));
		MetadataPageFile pages(io, firstBatch.candidate());
		MetadataPageRouteDirectory directory(pages);
		const PhysicalPageRef routeRoot = directory.buildFromCompleteRouteSet(
				firstBatch, std::vector{route(1U, 1000U, 1U, 1U), route(2U, 1001U, 1U, 1U)});
		PageDirectoryBootstrap bootstrap;
		bootstrap.generation = firstBatch.generation();
		bootstrap.committed_lsn = firstBatch.checkpointLsn();
		bootstrap.metadata_page_directory_root = routeRoot;
		bootstrapReference = firstBatch.allocatePage();
		pages.writePageDirectoryBootstrap(bootstrapReference, bootstrap);
	}

	Superblock published = superblock(2U, 2U);
	published.page_directory_root = bootstrapReference;
	published.flags |= RSS_SUPERBLOCK_FLAG_PAGE_DIRECTORY_BOOTSTRAP;
	MetadataWriteBatch nextBatch(allocator, published, LogSequenceNumber(3U));
	MetadataPageFile pages(io, nextBatch.candidate());
	MetadataPageRouteDirectory directory(pages);
	const PhysicalPageRef replacement = nextBatch.allocatePage();
	nextBatch.registerMetadataPageRoute(MetadataPageId(2U), replacement);
	EXPECT_EQ(directory.nextBootstrapFrameCount(nextBatch), 3U);
}

TEST_F(RelocatableMetadataPageRouteDirectoryTest,
	   IdentityOnlyBootstrapPublicationPreservesThePublishedRouteBranchAndRejectsRouteChanges) {
	TestMetadataPageAllocator allocator;
	const Superblock initial = superblock(1U, 1U);
	PhysicalPageRef bootstrapReference;
	PhysicalPageRef publishedRouteRoot;
	{
		MetadataWriteBatch firstBatch(allocator, initial, LogSequenceNumber(2U));
		MetadataPageFile pages(io, firstBatch.candidate());
		MetadataPageRouteDirectory directory(pages);
		publishedRouteRoot = directory.buildFromCompleteRouteSet(
				firstBatch, std::vector{route(1U, 1000U, 1U, 1U), route(2U, 1001U, 1U, 1U)});
		PageDirectoryBootstrap bootstrap;
		bootstrap.generation = firstBatch.generation();
		bootstrap.committed_lsn = firstBatch.checkpointLsn();
		bootstrap.metadata_page_directory_root = publishedRouteRoot;
		bootstrapReference = firstBatch.allocatePage();
		pages.writePageDirectoryBootstrap(bootstrapReference, bootstrap);
	}

	Superblock published = superblock(2U, 2U);
	published.page_directory_root = bootstrapReference;
	published.flags |= RSS_SUPERBLOCK_FLAG_PAGE_DIRECTORY_BOOTSTRAP;
	MetadataWriteBatch identityOnly(allocator, published, LogSequenceNumber(3U));
	MetadataPageFile pages(io, identityOnly.candidate());
	MetadataPageRouteDirectory directory(pages);

	// A metadata-identity-only commit must advance the bootstrap generation
	// without rebuilding or replacing its already-published route branch.
	const PhysicalPageRef replacement = directory.publishIdentityOnlyBootstrap(identityOnly);
	EXPECT_NE(replacement, bootstrapReference);
	const PageDirectoryBootstrap header = pages.readPageDirectoryBootstrap(replacement);
	EXPECT_EQ(header.generation, Generation(3U));
	EXPECT_EQ(header.committed_lsn, LogSequenceNumber(3U));
	EXPECT_EQ(header.metadata_page_directory_root, publishedRouteRoot);
	EXPECT_EQ(directory.find(identityOnly.candidate(), header.metadata_page_directory_root, MetadataPageId(2U)),
			  pageRef(1001U, 1U, 1U));

	MetadataWriteBatch routeChange(allocator, published, LogSequenceNumber(3U));
	routeChange.registerMetadataPageRoute(MetadataPageId(2U), routeChange.allocatePage());
	expectFormatError([&] { static_cast<void>(directory.publishIdentityOnlyBootstrap(routeChange)); },
				  FormatError::FERR_INVALID_STATE);

	MetadataWriteBatch pendingDataRoot(allocator, published, LogSequenceNumber(3U));
	pendingDataRoot.setPendingBootstrapDataRoot(pendingDataRoot.allocatePage());
	expectFormatError([&] { static_cast<void>(directory.publishIdentityOnlyBootstrap(pendingDataRoot)); },
				  FormatError::FERR_INVALID_STATE);

	MetadataWriteBatch routeRefresh(allocator, published, LogSequenceNumber(3U));
	routeRefresh.requestBootstrapRouteRefresh();
	expectFormatError([&] { static_cast<void>(directory.publishIdentityOnlyBootstrap(routeRefresh)); },
				  FormatError::FERR_INVALID_STATE);

	// A torn or incorrectly ordered bootstrap header must not be copied into the
	// next generation, even when there are no pending route mutations.
	PageDirectoryBootstrap futureHeader = pages.readPageDirectoryBootstrap(bootstrapReference);
	futureHeader.committed_lsn = LogSequenceNumber(4U);
	pages.writePageDirectoryBootstrap(bootstrapReference, futureHeader);
	MetadataWriteBatch futureHeaderBatch(allocator, published, LogSequenceNumber(3U));
	expectFormatError([&] { static_cast<void>(directory.publishIdentityOnlyBootstrap(futureHeaderBatch)); },
				  FormatError::FERR_INVALID_STATE);

	PageDirectoryBootstrap missingRouteHeader = pages.readPageDirectoryBootstrap(bootstrapReference);
	missingRouteHeader.committed_lsn = LogSequenceNumber(2U);
	missingRouteHeader.metadata_page_directory_root = {};
	pages.writePageDirectoryBootstrap(bootstrapReference, missingRouteHeader);
	MetadataWriteBatch missingRouteBatch(allocator, published, LogSequenceNumber(3U));
	expectFormatError([&] { static_cast<void>(directory.publishIdentityOnlyBootstrap(missingRouteBatch)); },
				  FormatError::FERR_INVALID_STATE);

	MetadataWriteBatch preBootstrap(allocator, initial, LogSequenceNumber(2U));
	expectFormatError([&] { static_cast<void>(directory.publishIdentityOnlyBootstrap(preBootstrap)); },
				  FormatError::FERR_INVALID_STATE);

}

TEST_F(RelocatableMetadataPageRouteDirectoryTest,
	   IdentifiesACompleteRouteBranchPoolForRefreshOnlyEvacuation) {
	TestMetadataPageAllocator allocator;
	const Superblock initial = superblock(1U, 1U);
	MetadataWriteBatch construction(allocator, initial, LogSequenceNumber(2U));
	MetadataPageFile pages(io, construction.candidate());
	MetadataPageRouteDirectory directory(pages);
	const PhysicalPageRef routeRoot = directory.buildFromCompleteRouteSet(
			construction, std::vector{route(1U, 1000U, 1U, 1U), route(2U, 1001U, 1U, 1U)});
	const std::vector<PhysicalPageRef> branchFrames =
			directory.collectRouteBranchFrames(construction.candidate(), routeRoot);
	ASSERT_FALSE(branchFrames.empty());
	const PhysicalSlotId sourceSlot = branchFrames.front().physical_slot;
	uint32_t sourceAllocatedFrames = 0U;
	for (const PhysicalPageRef frame: branchFrames) {
		ASSERT_EQ(frame.physical_slot, sourceSlot);
		sourceAllocatedFrames |= 1U << (frame.frame_index - RSS_PAGE_POOL_FIRST_DATA_FRAME);
	}

	const PhysicalPageRef bootstrapReference = pageRef(sourceSlot.value() + 1U, 1U, 2U);
	PageDirectoryBootstrap bootstrap;
	bootstrap.generation = Generation(2U);
	bootstrap.committed_lsn = LogSequenceNumber(2U);
	bootstrap.metadata_page_directory_root = routeRoot;
	pages.writePageDirectoryBootstrap(bootstrapReference, bootstrap);
	Superblock published = superblock(2U, 2U);
	published.page_directory_root = bootstrapReference;
	published.flags |= RSS_SUPERBLOCK_FLAG_PAGE_DIRECTORY_BOOTSTRAP;

	EXPECT_TRUE(canRefreshMetadataRouteBranchForEvacuation(pages, published, sourceSlot, sourceAllocatedFrames));
	MetadataWriteBatch staleRefresh(allocator, initial, LogSequenceNumber(2U));
	expectFormatError(
			[&] {
				static_cast<void>(refreshMetadataRouteBranchForEvacuation(
						pages, staleRefresh, published, sourceSlot, sourceAllocatedFrames));
			},
			FormatError::FERR_INVALID_STATE);
	MetadataWriteBatch refresh(allocator, published, LogSequenceNumber(3U));
	const std::optional<uint32_t> refreshed =
			refreshMetadataRouteBranchForEvacuation(pages, refresh, published, sourceSlot, sourceAllocatedFrames);
	ASSERT_TRUE(refreshed.has_value());
	EXPECT_EQ(*refreshed, branchFrames.size());
	EXPECT_TRUE(refresh.bootstrapRouteRefreshRequested());
	EXPECT_TRUE(refresh.metadataPageRoutes().empty());
	EXPECT_THROW(static_cast<void>(canRefreshMetadataRouteBranchForEvacuation(
					 pages, published, sourceSlot,
					 sourceAllocatedFrames & ~(1U << (branchFrames.front().frame_index - RSS_PAGE_POOL_FIRST_DATA_FRAME)))),
			 FormatException);

	bootstrap.data_page_directory_root = branchFrames.front();
	pages.writePageDirectoryBootstrap(bootstrapReference, bootstrap);
	EXPECT_FALSE(canRefreshMetadataRouteBranchForEvacuation(pages, published, sourceSlot, sourceAllocatedFrames));
	bootstrap.data_page_directory_root = {};
	pages.writePageDirectoryBootstrap(bootstrapReference, bootstrap);

	MetadataWriteBatch refreshAndPublish(allocator, published, LogSequenceNumber(3U));
	refreshAndPublish.reserveDataSlot(sourceSlot);
	ASSERT_TRUE(refreshMetadataRouteBranchForEvacuation(pages, refreshAndPublish, published, sourceSlot,
																	 sourceAllocatedFrames)
					.has_value());
	const PhysicalPageRef replacementHeader = directory.publishNextBootstrap(refreshAndPublish, {});
	EXPECT_NE(replacementHeader.physical_slot, sourceSlot);
	const PageDirectoryBootstrap replacement = pages.readPageDirectoryBootstrap(replacementHeader);
	EXPECT_NE(replacement.metadata_page_directory_root.physical_slot, sourceSlot);
	// A pinned old Superblock still owns its original physical branch until the
	// metadata-pool reclaimer observes that the snapshot has been released.
	EXPECT_EQ(directory.find(published, routeRoot, MetadataPageId(1U)), pageRef(1000U, 1U, 1U));
	EXPECT_EQ(directory.find(refreshAndPublish.candidate(), replacement.metadata_page_directory_root, MetadataPageId(1U)),
			  pageRef(1000U, 1U, 1U));
	io->sync();
	reopen();
	MetadataPageFile reopenedPages(io, refreshAndPublish.candidate());
	MetadataPageRouteDirectory reopenedDirectory(reopenedPages);
	EXPECT_EQ(reopenedDirectory.find(refreshAndPublish.candidate(), replacement.metadata_page_directory_root,
																	 MetadataPageId(2U)),
			  pageRef(1001U, 1U, 1U));
}

TEST_F(RelocatableMetadataPageRouteDirectoryTest,
	   EvacuatesMixedRoutedTargetsRouteBranchAndBootstrapHeaderFromOnePool) {
	TestMetadataPageAllocator allocator;
	const Superblock initial = superblock(1U, 1U);
	MetadataWriteBatch construction(allocator, initial, LogSequenceNumber(2U));
	MetadataPageFile pages(io, construction.candidate());
	MetadataPageRouteDirectory directory(pages);
	const PhysicalPageRef routeTarget = pageRef(5U, RSS_PAGE_POOL_DATA_FRAME_COUNT, 1U);
	MetadataRadixPage targetPage;
	targetPage.kind = CatalogRadixPageKind::CRPK_ROOT;
	targetPage.directory_type = MetadataDirectoryType::MDT_PAGE;
	targetPage.level = 0U;
	targetPage.page_id = MetadataPageId(99U);
	targetPage.generation = Generation(1U);
	targetPage.committed_lsn = LogSequenceNumber(1U);
	targetPage.logical_children_enabled = true;
	pages.writeMetadataRadix(routeTarget, targetPage);
	const PhysicalPageRef routeRoot =
			directory.buildFromCompleteRouteSet(construction, std::vector{route(99U, 5U, routeTarget.frame_index, 1U)});
	const std::vector<PhysicalPageRef> branchFrames =
			directory.collectRouteBranchFrames(construction.candidate(), routeRoot);
	ASSERT_FALSE(branchFrames.empty());
	const PhysicalSlotId sourceSlot = branchFrames.front().physical_slot;
	ASSERT_EQ(sourceSlot, routeTarget.physical_slot);
	for (const PhysicalPageRef frame: branchFrames) {
		ASSERT_EQ(frame.physical_slot, sourceSlot);
		ASSERT_NE(frame.frame_index, routeTarget.frame_index);
	}
	const PhysicalPageRef sourceHeader = pageRef(sourceSlot.value(), RSS_PAGE_POOL_DATA_FRAME_COUNT - 1U, 2U);
	for (const PhysicalPageRef frame: branchFrames) {
		ASSERT_NE(frame.frame_index, sourceHeader.frame_index);
	}
	PageDirectoryBootstrap bootstrap;
	bootstrap.generation = Generation(2U);
	bootstrap.committed_lsn = LogSequenceNumber(2U);
	bootstrap.data_page_directory_root = routeTarget;
	bootstrap.metadata_page_directory_root = routeRoot;
	pages.writePageDirectoryBootstrap(sourceHeader, bootstrap);
	Superblock published = superblock(2U, 2U);
	published.page_directory_root = sourceHeader;
	published.flags |= RSS_SUPERBLOCK_FLAG_PAGE_DIRECTORY_BOOTSTRAP;

	uint32_t sourceFrames = 0U;
	for (const PhysicalPageRef frame: branchFrames) {
		sourceFrames |= 1U << (frame.frame_index - RSS_PAGE_POOL_FIRST_DATA_FRAME);
	}
	sourceFrames |= 1U << (routeTarget.frame_index - RSS_PAGE_POOL_FIRST_DATA_FRAME);
	sourceFrames |= 1U << (sourceHeader.frame_index - RSS_PAGE_POOL_FIRST_DATA_FRAME);
	ASSERT_TRUE(canEvacuateMixedRoutedMetadataPool(pages, published, sourceSlot, sourceFrames));
	EXPECT_FALSE(canRelocateRoutedMetadataPoolPages(pages, published, sourceSlot, sourceFrames));
	EXPECT_FALSE(canRefreshMetadataRouteBranchForEvacuation(pages, published, sourceSlot, sourceFrames));
	EXPECT_FALSE(canRefreshBootstrapHeaderForEvacuation(pages, published, sourceSlot, sourceFrames));
	EXPECT_FALSE(canEvacuateMixedRoutedMetadataPool(
			pages, published, sourceSlot, sourceFrames | (1U << (RSS_PAGE_POOL_DATA_FRAME_COUNT - 3U))));
	Superblock stale = published;
	stale.checkpoint_lsn = LogSequenceNumber(1U);
	MetadataWriteBatch staleBatch(allocator, published, LogSequenceNumber(3U));
	expectFormatError(
			[&] { static_cast<void>(evacuateMixedRoutedMetadataPool(pages, staleBatch, stale, sourceSlot, sourceFrames)); },
			FormatError::FERR_INVALID_STATE);

	MetadataWriteBatch relocation(allocator, published, LogSequenceNumber(3U));
	relocation.reserveDataSlot(sourceSlot);
	ASSERT_EQ(evacuateMixedRoutedMetadataPool(pages, relocation, published, sourceSlot, sourceFrames),
			  std::popcount(sourceFrames));
	ASSERT_TRUE(relocation.bootstrapRouteRefreshRequested());
	ASSERT_TRUE(relocation.pendingBootstrapDataRoot().has_value());
	const PhysicalPageRef replacementHeader = directory.publishNextBootstrap(relocation, *relocation.pendingBootstrapDataRoot());
	const PageDirectoryBootstrap replacement = pages.readPageDirectoryBootstrap(replacementHeader);
	const PhysicalPageRef replacementTarget =
			directory.find(relocation.candidate(), replacement.metadata_page_directory_root, MetadataPageId(99U));
	EXPECT_NE(replacementHeader.physical_slot, sourceSlot);
	EXPECT_NE(replacement.metadata_page_directory_root.physical_slot, sourceSlot);
	EXPECT_NE(replacementTarget, routeTarget);
	EXPECT_NE(replacementTarget.physical_slot, sourceSlot);
	EXPECT_EQ(replacement.data_page_directory_root, replacementTarget);
	EXPECT_EQ(directory.find(published, routeRoot, MetadataPageId(99U)), routeTarget);
	EXPECT_EQ(pages.readMetadataRadix(routeTarget, published).page_id, MetadataPageId(99U));
	EXPECT_EQ(pages.readMetadataRadix(replacementTarget, relocation.candidate()).page_id, MetadataPageId(99U));
}

TEST_F(RelocatableMetadataPageRouteDirectoryTest, RelocatesThePhysicalDataDirectoryAnchorWithItsRoute) {
	TestMetadataPageAllocator allocator;
	const Superblock initial = superblock(1U, 1U);
	MetadataWriteBatch construction(allocator, initial, LogSequenceNumber(2U));
	MetadataPageFile pages(io, construction.candidate());
	MetadataPageRouteDirectory directory(pages);
	const PhysicalPageRef dataRoot = pageRef(9U, 1U, 1U);
	MetadataRadixPage dataPage;
	dataPage.kind = CatalogRadixPageKind::CRPK_ROOT;
	dataPage.directory_type = MetadataDirectoryType::MDT_PAGE;
	dataPage.level = 0U;
	dataPage.page_id = MetadataPageId(99U);
	dataPage.generation = Generation(1U);
	dataPage.committed_lsn = LogSequenceNumber(1U);
	dataPage.logical_children_enabled = true;
	pages.writeMetadataRadix(dataRoot, dataPage);
	const PhysicalPageRef routeRoot = directory.buildFromCompleteRouteSet(construction, std::vector{route(99U, 9U, 1U, 1U)});
	const PhysicalPageRef bootstrapReference = pageRef(10U, 1U, 2U);
	PageDirectoryBootstrap bootstrap;
	bootstrap.generation = Generation(2U);
	bootstrap.committed_lsn = LogSequenceNumber(2U);
	bootstrap.data_page_directory_root = dataRoot;
	bootstrap.metadata_page_directory_root = routeRoot;
	pages.writePageDirectoryBootstrap(bootstrapReference, bootstrap);
	Superblock published = superblock(2U, 2U);
	published.page_directory_root = bootstrapReference;
	published.flags |= RSS_SUPERBLOCK_FLAG_PAGE_DIRECTORY_BOOTSTRAP;

	MetadataWriteBatch relocation(allocator, published, LogSequenceNumber(3U));
	relocation.reserveDataSlot(dataRoot.physical_slot);
	const uint32_t dataRootFrame = 1U << (dataRoot.frame_index - RSS_PAGE_POOL_FIRST_DATA_FRAME);
	EXPECT_EQ(relocateRoutedMetadataPoolPages(pages, relocation, published, dataRoot.physical_slot, dataRootFrame), 1U);
	ASSERT_TRUE(relocation.pendingBootstrapDataRoot().has_value());
	const PhysicalPageRef replacementDataRoot = *relocation.pendingBootstrapDataRoot();
	EXPECT_NE(replacementDataRoot.physical_slot, dataRoot.physical_slot);
	static_cast<void>(directory.publishNextBootstrap(relocation, replacementDataRoot));
	const PageDirectoryBootstrap replacement = pages.readPageDirectoryBootstrap(relocation.candidate().page_directory_root);
	EXPECT_EQ(replacement.data_page_directory_root, replacementDataRoot);
	EXPECT_EQ(directory.find(relocation.candidate(), replacement.metadata_page_directory_root, MetadataPageId(99U)),
			  replacementDataRoot);
	EXPECT_EQ(directory.find(published, routeRoot, MetadataPageId(99U)), dataRoot);
	io->sync();
	reopen();
	MetadataPageFile reopenedPages(io, relocation.candidate());
	MetadataPageRouteDirectory reopenedDirectory(reopenedPages);
	EXPECT_EQ(reopenedDirectory.find(relocation.candidate(), replacement.metadata_page_directory_root, MetadataPageId(99U)),
			  replacementDataRoot);
}

TEST_F(RelocatableMetadataPageRouteDirectoryTest,
	   RelocatesARoutedPhysicalChildPageByPublishingLogicalChildren) {
	TestMetadataPageAllocator allocator;
	const Superblock initial = superblock(1U, 1U);
	MetadataWriteBatch construction(allocator, initial, LogSequenceNumber(2U));
	MetadataPageFile pages(io, construction.candidate());
	MetadataPageRouteDirectory directory(pages);
	const PhysicalPageRef sourceRoot = pageRef(9U, 1U, 1U);
	const PhysicalPageRef sourceLeaf = pageRef(9U, 2U, 1U);
	const PhysicalPageRef unrouteableRoot = pageRef(11U, 1U, 1U);
	const PhysicalPageRef unrouteableChild = pageRef(12U, 1U, 1U);
	MetadataRadixPage root;
	root.kind = CatalogRadixPageKind::CRPK_ROOT;
	root.directory_type = MetadataDirectoryType::MDT_EXTENT;
	root.level = 0U;
	root.page_id = MetadataPageId(99U);
	root.generation = Generation(1U);
	root.committed_lsn = LogSequenceNumber(1U);
	root.children[0] = sourceLeaf;
	pages.writeMetadataRadix(sourceRoot, root);
	LocationLeafPage leaf;
	leaf.directory_type = MetadataDirectoryType::MDT_EXTENT;
	leaf.page_id = MetadataPageId(100U);
	leaf.generation = Generation(1U);
	leaf.committed_lsn = LogSequenceNumber(1U);
	leaf.entries = {{1U, SegmentId(2U), Generation(1U), 1U, 1U}};
	pages.writeLocationLeaf(sourceLeaf, leaf);
	MetadataRadixPage physicalChildWithoutRoute = root;
	physicalChildWithoutRoute.page_id = MetadataPageId(101U);
	physicalChildWithoutRoute.children[0] = unrouteableChild;
	pages.writeMetadataRadix(unrouteableRoot, physicalChildWithoutRoute);
	const PhysicalPageRef routeRoot = directory.buildFromCompleteRouteSet(
			construction,
			std::vector{route(99U, 9U, 1U, 1U), route(100U, 9U, 2U, 1U), route(101U, 11U, 1U, 1U)});
	const PhysicalPageRef bootstrapReference = pageRef(10U, 1U, 2U);
	PageDirectoryBootstrap bootstrap;
	bootstrap.generation = Generation(2U);
	bootstrap.committed_lsn = LogSequenceNumber(2U);
	bootstrap.metadata_page_directory_root = routeRoot;
	bootstrap.metadata_directory_roots[static_cast<size_t>(MetadataDirectoryRootOrdinal::MDRO_EXTENT)] =
			MetadataPageId(99U);
	pages.writePageDirectoryBootstrap(bootstrapReference, bootstrap);
	Superblock published = superblock(2U, 2U);
	published.page_directory_root = bootstrapReference;
	published.extent_directory_root = sourceRoot;
	published.flags |= RSS_SUPERBLOCK_FLAG_PAGE_DIRECTORY_BOOTSTRAP;
	const uint32_t sourceFrames = (1U << (sourceRoot.frame_index - RSS_PAGE_POOL_FIRST_DATA_FRAME)) |
								  (1U << (sourceLeaf.frame_index - RSS_PAGE_POOL_FIRST_DATA_FRAME));
	ASSERT_TRUE(canRelocateRoutedMetadataPoolPages(pages, published, sourceRoot.physical_slot, sourceFrames));

	MetadataWriteBatch relocation(allocator, published, LogSequenceNumber(3U));
	relocation.reserveDataSlot(sourceRoot.physical_slot);
	ASSERT_EQ(relocateRoutedMetadataPoolPages(pages, relocation, published, sourceRoot.physical_slot, sourceFrames), 2U);
	const PhysicalPageRef replacementHeader = directory.publishNextBootstrap(relocation, {});
	const PageDirectoryBootstrap replacementBootstrap = pages.readPageDirectoryBootstrap(replacementHeader);
	const PhysicalPageRef replacementRoot = directory.find(
			relocation.candidate(), replacementBootstrap.metadata_page_directory_root, MetadataPageId(99U));
	const PhysicalPageRef replacementLeaf = directory.find(
			relocation.candidate(), replacementBootstrap.metadata_page_directory_root, MetadataPageId(100U));
	EXPECT_NE(replacementRoot.physical_slot, sourceRoot.physical_slot);
	EXPECT_NE(replacementLeaf.physical_slot, sourceLeaf.physical_slot);
	EXPECT_EQ(relocation.candidate().extent_directory_root, replacementRoot);
	EXPECT_FALSE(pages.readMetadataRadixUnresolved(sourceRoot).logical_children_enabled);
	const MetadataRadixPage replacementRootPage = pages.readMetadataRadix(replacementRoot, relocation.candidate());
	EXPECT_TRUE(replacementRootPage.logical_children_enabled);
	EXPECT_EQ(replacementRootPage.logical_children[0], MetadataPageId(100U));
	EXPECT_EQ(replacementRootPage.children[0], replacementLeaf);
	EXPECT_EQ(pages.readMetadataRadix(sourceRoot, published).children[0], sourceLeaf);

	// A subsequent evacuation reads the published logical child through its
	// route, then emits an ordinary physical COW directory candidate.
	pages.rebindSuperblock(relocation.candidate());
	MetadataWriteBatch evacuation(allocator, relocation.candidate(), LogSequenceNumber(4U));
	ASSERT_NO_THROW(cloneMetadataDirectoryRootsForEvacuation(pages, evacuation, relocation.candidate()));
	const MetadataRadixPage evacuatedRoot =
			pages.readMetadataRadixUnresolved(evacuation.candidate().extent_directory_root);
	EXPECT_FALSE(evacuatedRoot.logical_children_enabled);
	EXPECT_FALSE(evacuatedRoot.logical_children_resolved);
	EXPECT_FALSE(evacuatedRoot.logical_children[0].isValid());
	EXPECT_TRUE(evacuatedRoot.children[0].isValid());

	EXPECT_FALSE(canRelocateRoutedMetadataPoolPages(
			pages, published, unrouteableRoot.physical_slot,
			1U << (unrouteableRoot.frame_index - RSS_PAGE_POOL_FIRST_DATA_FRAME)));
}

TEST_F(RelocatableMetadataPageRouteDirectoryTest, RefreshesAnIsolatedBootstrapHeaderPoolDuringEvacuation) {
	TestMetadataPageAllocator allocator;
	const Superblock initial = superblock(1U, 1U);
	MetadataWriteBatch construction(allocator, initial, LogSequenceNumber(2U));
	MetadataPageFile pages(io, construction.candidate());
	MetadataPageRouteDirectory directory(pages);
	const PhysicalPageRef routeRoot =
			directory.buildFromCompleteRouteSet(construction, std::vector{route(99U, 9U, 1U, 1U)});
	const PhysicalPageRef sourceHeader = pageRef(10U, 1U, 2U);
	PageDirectoryBootstrap bootstrap;
	bootstrap.generation = Generation(2U);
	bootstrap.committed_lsn = LogSequenceNumber(2U);
	bootstrap.metadata_page_directory_root = routeRoot;
	pages.writePageDirectoryBootstrap(sourceHeader, bootstrap);
	Superblock published = superblock(2U, 2U);
	published.page_directory_root = sourceHeader;
	published.flags |= RSS_SUPERBLOCK_FLAG_PAGE_DIRECTORY_BOOTSTRAP;
	const uint32_t sourceFrame = 1U << (sourceHeader.frame_index - RSS_PAGE_POOL_FIRST_DATA_FRAME);
	ASSERT_TRUE(canRefreshBootstrapHeaderForEvacuation(pages, published, sourceHeader.physical_slot, sourceFrame));
	MetadataWriteBatch staleRefresh(allocator, initial, LogSequenceNumber(2U));
	expectFormatError(
			[&] {
				static_cast<void>(refreshBootstrapHeaderForEvacuation(
						pages, staleRefresh, published, sourceHeader.physical_slot, sourceFrame));
			},
			FormatError::FERR_INVALID_STATE);

	MetadataWriteBatch refresh(allocator, published, LogSequenceNumber(3U));
	refresh.reserveDataSlot(sourceHeader.physical_slot);
	ASSERT_EQ(refreshBootstrapHeaderForEvacuation(pages, refresh, published, sourceHeader.physical_slot, sourceFrame),
			  1U);
	ASSERT_TRUE(refresh.bootstrapRouteRefreshRequested());
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	graph::debug::PerfTrace::reset();
	const PhysicalPageRef replacementHeader = directory.publishNextBootstrap(refresh, {});
	const auto counters = graph::debug::PerfTrace::snapshotAndReset();
	ASSERT_TRUE(counters.contains("native_graph.route.publication_base_traversals"));
	EXPECT_EQ(counters.at("native_graph.route.publication_base_traversals").totalValue, 1);
	EXPECT_NE(replacementHeader.physical_slot, sourceHeader.physical_slot);
	const PageDirectoryBootstrap replacement = pages.readPageDirectoryBootstrap(replacementHeader);
	EXPECT_EQ(directory.find(refresh.candidate(), replacement.metadata_page_directory_root, MetadataPageId(99U)),
			  pageRef(9U, 1U, 1U));
	EXPECT_EQ(directory.find(published, routeRoot, MetadataPageId(99U)), pageRef(9U, 1U, 1U));
	EXPECT_FALSE(canRefreshBootstrapHeaderForEvacuation(pages, published, sourceHeader.physical_slot,
																	 sourceFrame | (1U << RSS_PAGE_POOL_FIRST_DATA_FRAME)));
}

TEST_F(RelocatableMetadataPageRouteDirectoryTest, SelectedChildrenPreserveOrderSparseSlotsAndFullValidation) {
	TestMetadataPageAllocator allocator;
	MetadataWriteBatch batch(allocator, superblock(1U, 1U), LogSequenceNumber(2U));
	MetadataPageFile pages(io, batch.candidate());
	MetadataPageRouteDirectory routes(pages);
	const auto routeRoot = routes.buildFromCompleteRouteSet(batch,
			std::vector{route(1U, 1000U, 1U, 1U), route(2U, 1001U, 1U, 1U)});
	MetadataRadixPage parent;
	parent.kind = CatalogRadixPageKind::CRPK_ROOT;
	parent.directory_type = MetadataDirectoryType::MDT_NODE_ENTITY;
	parent.page_id = MetadataPageId(10000U);
	parent.generation = Generation(2U);
	parent.committed_lsn = LogSequenceNumber(2U);
	parent.logical_children_enabled = true;
	parent.logical_children[0] = MetadataPageId(1U);
	parent.logical_children[2] = MetadataPageId(2U);
	parent.logical_children[3] = MetadataPageId(99U); // Missing route outside the selected set.
	const auto parentReference = pageRef(40U, 1U, 2U);
	pages.writeMetadataRadix(parentReference, parent);
	PageDirectoryBootstrap bootstrap;
	bootstrap.generation = Generation(2U);
	bootstrap.committed_lsn = LogSequenceNumber(2U);
	bootstrap.metadata_page_directory_root = routeRoot;
	const auto bootstrapReference = pageRef(41U, 1U, 2U);
	pages.writePageDirectoryBootstrap(bootstrapReference, bootstrap);
	auto published = batch.candidate();
	published.page_directory_root = bootstrapReference;
	published.flags |= RSS_SUPERBLOCK_FLAG_PAGE_DIRECTORY_BOOTSTRAP;
	MetadataPageFile reader(io, published);
	const auto decoded = reader.readMetadataRadixUnresolved(parentReference);
	const std::array<uint32_t, 4U> indices{2U, 1U, 0U, 2U};
	std::array<PhysicalPageRef, 4U> selected{};
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	graph::debug::PerfTrace::reset();
	try {
		reader.resolveMetadataRadixChildren(decoded, published, indices, selected);
	} catch (...) {
		graph::debug::PerfTrace::setValueCollectionEnabled(false);
		throw;
	}
	const auto events = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setValueCollectionEnabled(false);
	EXPECT_EQ(selected[0], pageRef(1001U, 1U, 1U));
	EXPECT_FALSE(selected[1].isValid());
	EXPECT_EQ(selected[2], pageRef(1000U, 1U, 1U));
	EXPECT_EQ(selected[3], selected[0]);
	EXPECT_FALSE(decoded.logical_children_resolved);
	EXPECT_TRUE(std::ranges::none_of(decoded.children, [](auto ref) { return ref.isValid(); }));
	ASSERT_TRUE(events.contains("native_graph.read.metadata_pages_read"));
	EXPECT_EQ(events.at("native_graph.read.metadata_pages_read").totalValue, 3); // Bootstrap, route root, shared leaf.
	ASSERT_TRUE(events.contains("native_graph.read.radix_resolved_children"));
	EXPECT_EQ(events.at("native_graph.read.radix_resolved_children").totalValue, 3);
	EXPECT_THROW(static_cast<void>(reader.readMetadataRadix(parentReference)), FormatException);
	const std::array<uint32_t, 1U> missing{3U};
	std::array<PhysicalPageRef, 1U> result{};
	EXPECT_THROW(reader.resolveMetadataRadixChildren(decoded, published, missing, result), FormatException);
	const std::array<uint32_t, 1U> invalid{RSS_CATALOG_RADIX_FANOUT};
	EXPECT_THROW(reader.resolveMetadataRadixChildren(decoded, published, invalid, result), FormatException);
	EXPECT_THROW(reader.resolveMetadataRadixChildren(decoded, published, indices, result), FormatException);
	EXPECT_NO_THROW(reader.resolveMetadataRadixChildren(decoded, published, {}, {}));
	auto wrongDatabase = published;
	wrongDatabase.database_id = DatabaseId(42U);
	EXPECT_THROW(reader.resolveMetadataRadixChildren(decoded, wrongDatabase, indices, selected), FormatException);
	auto wrongLayout = published;
	wrongLayout.data_region_offset += RSS_SEGMENT_SIZE;
	EXPECT_THROW(reader.resolveMetadataRadixChildren(decoded, wrongLayout, indices, selected), FormatException);
	auto staleContext = published;
	staleContext.generation = Generation(4U);
	staleContext.checkpoint_lsn = LogSequenceNumber(4U);
	EXPECT_THROW(reader.resolveMetadataRadixChildren(decoded, staleContext, indices, selected), FormatException);
	// A reader rebound to a later publication must still use the explicitly pinned old route.
	reader.rebindSuperblock(staleContext);
	EXPECT_NO_THROW(reader.resolveMetadataRadixChildren(decoded, published, indices, selected));
	EXPECT_EQ(selected[2], pageRef(1000U, 1U, 1U));
}

TEST_F(RelocatableMetadataPageRouteDirectoryTest, PointAndBoundedCursorDoNotResolveUnvisitedSiblingRoutes) {
	TestMetadataPageAllocator allocator;
	MetadataWriteBatch batch(allocator, superblock(1U, 1U), LogSequenceNumber(2U));
	MetadataPageFile pages(io, batch.candidate());
	MetadataPageRouteDirectory routes(pages);
	const auto leafReference = pageRef(50U, 1U, 2U);
	LocationLeafPage leaf;
	leaf.directory_type = MetadataDirectoryType::MDT_NODE_ENTITY;
	leaf.page_id = MetadataPageId(901U);
	leaf.generation = Generation(2U);
	leaf.committed_lsn = LogSequenceNumber(2U);
	leaf.entries = {{1U, SegmentId(10U), Generation(1U), 0U, 1U, 0U}};
	pages.writeLocationLeaf(leafReference, leaf);
	const auto rootReference = pageRef(51U, 1U, 2U);
	const auto routeRoot = routes.buildFromCompleteRouteSet(batch,
			std::vector{MetadataPageRoute{MetadataPageId(900U), rootReference},
					MetadataPageRoute{leaf.page_id, leafReference}});
	MetadataRadixPage root;
	root.kind = CatalogRadixPageKind::CRPK_ROOT;
	root.level = 0U;
	root.directory_type = MetadataDirectoryType::MDT_NODE_ENTITY;
	root.page_id = MetadataPageId(900U);
	root.generation = Generation(2U);
	root.committed_lsn = LogSequenceNumber(2U);
	root.logical_children_enabled = true;
	root.logical_children[0] = leaf.page_id;
	root.logical_children[1] = MetadataPageId(902U); // Deliberately missing, beyond the bounded range.
	pages.writeMetadataRadix(rootReference, root);
	PageDirectoryBootstrap bootstrap;
	bootstrap.generation = Generation(2U);
	bootstrap.committed_lsn = LogSequenceNumber(2U);
	bootstrap.metadata_page_directory_root = routeRoot;
	bootstrap.metadata_directory_roots[static_cast<size_t>(MetadataDirectoryRootOrdinal::MDRO_NODE_ENTITY)] = root.page_id;
	const auto bootstrapReference = pageRef(52U, 1U, 2U);
	pages.writePageDirectoryBootstrap(bootstrapReference, bootstrap);
	auto published = batch.candidate();
	published.node_entity_directory_root = rootReference;
	published.page_directory_root = bootstrapReference;
	published.flags |= RSS_SUPERBLOCK_FLAG_PAGE_DIRECTORY_BOOTSTRAP;
	MetadataPageFile reader(io, published);
	LocationDirectory directory(reader, allocator);
	const auto type = MetadataDirectoryType::MDT_NODE_ENTITY;
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	graph::debug::PerfTrace::reset();
	try {
		EXPECT_EQ(directory.findNodePageLocation(published, 1U).entity_id, 1U);
	} catch (...) {
		graph::debug::PerfTrace::setValueCollectionEnabled(false);
		throw;
	}
	const auto trace = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setValueCollectionEnabled(false);
	ASSERT_TRUE(trace.contains("native_graph.read.metadata_pages_read"));
	// Root route (3), root (1), selected leaf route (3), data leaf (1).
	// Re-resolving the root would add another bootstrap/root/route-leaf pass.
	EXPECT_EQ(trace.at("native_graph.read.metadata_pages_read").totalValue, 8);
	EXPECT_FALSE(directory.findNodePageLocationOptional(published, 2U).has_value());
	EXPECT_FALSE(directory.findNodePageLocationOptional(published,
			1U + static_cast<uint64_t>(RSS_LOCATION_LEAF_MAX_ENTRIES) * RSS_CATALOG_RADIX_FANOUT).has_value());
	auto foreignLayout = published;
	foreignLayout.data_region_offset += RSS_SEGMENT_SIZE;
	EXPECT_THROW(static_cast<void>(directory.findNodePageLocationOptional(foreignLayout,
			1U + static_cast<uint64_t>(RSS_LOCATION_LEAF_MAX_ENTRIES) * RSS_CATALOG_RADIX_FANOUT)), FormatException);
	auto cursor = directory.openRangeCursor(published, type, 1U, 2U);
	ASSERT_TRUE(cursor.next().has_value());
	EXPECT_FALSE(cursor.next().has_value());
	EXPECT_THROW(static_cast<void>(reader.readMetadataRadix(rootReference)), FormatException);
	EXPECT_THROW(static_cast<void>(directory.findNodePageLocation(published, 1U + RSS_LOCATION_LEAF_MAX_ENTRIES)), FormatException);
	auto invalidCursor = directory.openRangeCursor(published, type,
			1U + RSS_LOCATION_LEAF_MAX_ENTRIES, 2U + RSS_LOCATION_LEAF_MAX_ENTRIES);
	EXPECT_THROW(static_cast<void>(invalidCursor.next()), FormatException);
	// Selected descendants retain type validation even when unrelated siblings are ignored.
	leaf.directory_type = MetadataDirectoryType::MDT_EDGE_ENTITY;
	pages.writeLocationLeaf(leafReference, leaf);
	EXPECT_THROW(static_cast<void>(directory.findNodePageLocation(published, 1U)), FormatException);
	auto wrongTypeCursor = directory.openRangeCursor(published, type, 1U, 2U);
	EXPECT_THROW(static_cast<void>(wrongTypeCursor.next()), FormatException);
}

TEST_F(RelocatableMetadataPageRouteDirectoryTest, SparseEntityAndPropertyBatchesSkipUnrequestedInteriorRoutes) {
	for (const auto type : {MetadataDirectoryType::MDT_NODE_ENTITY, MetadataDirectoryType::MDT_ENTITY_PROPERTY}) {
		SCOPED_TRACE(static_cast<int>(type));
		TestMetadataPageAllocator allocator;
		MetadataWriteBatch batch(allocator, superblock(1U, 1U), LogSequenceNumber(2U));
		MetadataPageFile pages(io, batch.candidate());
		MetadataPageRouteDirectory routes(pages);
		const auto leafReference = pageRef(50U, 1U, 2U);
		LocationLeafPage leaf;
		leaf.directory_type = type;
		leaf.page_id = MetadataPageId(901U);
		leaf.generation = Generation(2U);
		leaf.committed_lsn = LogSequenceNumber(2U);
		leaf.entries = {{1U, SegmentId(10U), Generation(1U), 0U, 1U, 0U}};
		pages.writeLocationLeaf(leafReference, leaf);
		const uint64_t farId = 1U + 2U * RSS_LOCATION_LEAF_MAX_ENTRIES;
		const auto farReference = pageRef(53U, 1U, 2U);
		LocationLeafPage farLeaf = leaf;
		farLeaf.page_id = MetadataPageId(903U);
		farLeaf.entries.front().logical_id = farId;
		pages.writeLocationLeaf(farReference, farLeaf);
		const auto rootReference = pageRef(51U, 1U, 2U);
		const auto routeRoot = routes.buildFromCompleteRouteSet(batch,
				std::vector{MetadataPageRoute{MetadataPageId(900U), rootReference},
						MetadataPageRoute{leaf.page_id, leafReference},
						MetadataPageRoute{farLeaf.page_id, farReference}});
		MetadataRadixPage root;
		root.kind = CatalogRadixPageKind::CRPK_ROOT;
		root.level = 0U;
		root.directory_type = type;
		root.page_id = MetadataPageId(900U);
		root.generation = Generation(2U);
		root.committed_lsn = LogSequenceNumber(2U);
		root.logical_children_enabled = true;
		root.logical_children[0] = leaf.page_id;
		root.logical_children[1] = MetadataPageId(902U); // Missing route between requested IDs.
		root.logical_children[2] = farLeaf.page_id;
		pages.writeMetadataRadix(rootReference, root);
		PageDirectoryBootstrap bootstrap;
		bootstrap.generation = Generation(2U);
		bootstrap.committed_lsn = LogSequenceNumber(2U);
		bootstrap.metadata_page_directory_root = routeRoot;
		bootstrap.metadata_directory_roots[static_cast<size_t>(type == MetadataDirectoryType::MDT_NODE_ENTITY ?
				MetadataDirectoryRootOrdinal::MDRO_NODE_ENTITY : MetadataDirectoryRootOrdinal::MDRO_ENTITY_PROPERTY)] = root.page_id;
		const auto bootstrapReference = pageRef(52U, 1U, 2U);
		pages.writePageDirectoryBootstrap(bootstrapReference, bootstrap);
		auto published = batch.candidate();
		published.node_entity_directory_root = rootReference;
		published.page_directory_root = bootstrapReference;
		published.flags |= RSS_SUPERBLOCK_FLAG_PAGE_DIRECTORY_BOOTSTRAP;
		MetadataPageFile reader(io, published);
		LocationDirectory directory(reader, allocator);
		const std::array<uint64_t, 3U> ids{1U, 2U, farId};
		if (type == MetadataDirectoryType::MDT_NODE_ENTITY) {
			std::array<std::optional<EntityPageLocation>, 3U> result{};
			directory.findEntityPageLocationsSorted(published, type, ids, result);
			ASSERT_TRUE(result[0].has_value());
			EXPECT_FALSE(result[1].has_value());
			ASSERT_TRUE(result[2].has_value());
			EXPECT_EQ(result[2]->entity_id, farId);
		} else {
			const std::array<EntityPropertyKey, 3U> keys{{
				{EntityPropertyOwnerKind::EPOK_NODE, 1U},
				{EntityPropertyOwnerKind::EPOK_NODE, 2U},
				{EntityPropertyOwnerKind::EPOK_NODE, farId}}};
			std::array<std::optional<EntityPropertyLocation>, 3U> result{};
			directory.findEntityPropertiesSorted(published, keys, result);
			ASSERT_TRUE(result[0].has_value());
			EXPECT_FALSE(result[1].has_value());
			ASSERT_TRUE(result[2].has_value());
			EXPECT_EQ(result[2]->key.entity_id, farId);
		}
		// A true range scan includes the hole and must still report the bad route.
		auto cursor = directory.openRangeCursor(published, type, 1U, farId);
		EXPECT_THROW(static_cast<void>(cursor.next()), FormatException);
	}
}

TEST_F(RelocatableMetadataPageRouteDirectoryTest, LookupRejectsAMisroutedEndpointEvenWhenTheRequestedEntryIsValid) {
    for (bool changeFirst : {false, true}) {
        const uint64_t start = changeFirst ? RSS_METADATA_PAGE_ROUTE_LEAF_MAX_ENTRIES + 1U : 1U;
        const Superblock current = superblock(1U, 1U);
        TestMetadataPageAllocator allocator;
        MetadataWriteBatch batch(allocator, current, LogSequenceNumber(2U));
        MetadataPageFile pages(io, batch.candidate());
        MetadataPageRouteDirectory directory(pages);
        const PhysicalPageRef root = directory.buildFromCompleteRouteSet(batch,
            std::vector{route(start, 1000U, 1U, 1U), route(start + 1U, 1001U, 1U, 1U), route(start + 2U, 1002U, 1U, 1U)});
        const auto radix = pages.readMetadataRadix(root);
        const size_t ordinal = changeFirst ? 1U : 0U;
        ASSERT_TRUE(radix.children[ordinal].isValid());
        auto leaf = pages.readMetadataPageRouteLeaf(radix.children[ordinal]);
        ASSERT_EQ(leaf.routes.size(), 3U);
        if (changeFirst) leaf.routes.front().page_id = MetadataPageId(start - 1U);
        else leaf.routes.back().page_id = MetadataPageId(RSS_METADATA_PAGE_ROUTE_LEAF_MAX_ENTRIES + 1U);
        pages.writeMetadataPageRouteLeaf(radix.children[ordinal], leaf);
        const MetadataPageId validRequested(changeFirst ? start + 2U : start);
        expectFormatError([&] { (void)directory.find(batch.candidate(), root, validRequested); },
            FormatError::FERR_INVALID_HEADER);
    }
}
