/**
 * @file test_RelocatableMetadataStoreValidation.cpp
 * @brief Validation coverage for relocatable Metadata Store public operations.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <span>
#include <vector>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"
#include "graph/storage/relocatable/metadata/MetadataPageFile.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/property/LargeValueStore.hpp"
#include "graph/storage/relocatable/vector/VectorMappingIds.hpp"
#include "graph/storage/relocatable/vector/VectorManifestStore.hpp"
#include "graph/storage/relocatable/vector/VectorPageStore.hpp"
#include "graph/utils/Sha256.hpp"

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

	Superblock initialSuperblock() {
		Superblock result;
		result.database_id = DatabaseId(0x1020304050607080ULL);
		result.generation = Generation(1);
		result.checkpoint_lsn = LogSequenceNumber(1);
		return result;
	}

	CatalogSlotReservationRequest reservationRequest(SegmentId segmentId, SegmentKind kind) {
		CatalogSlotReservationRequest request;
		request.segment_id = segmentId;
		request.kind = kind;
		request.owner_id = 17;
		request.segment_generation = Generation(1);
		request.state = SegmentState::SGS_SEALED;
		request.require_absent = true;
		return request;
	}

	void materializeInitialIndexBatches(BTreePageStore &indexes,
									 std::vector<std::vector<IndexTreeMutationPoolBatch>> &batches,
									 std::span<const uint64_t> indexIds, LogSequenceNumber checkpointLsn) {
		if (batches.size() != indexIds.size()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
							  "test Index materializer received an unexpected tree cardinality");
		}
		for (size_t treeOrdinal = 0U; treeOrdinal < batches.size(); ++treeOrdinal) {
			if (batches[treeOrdinal].size() != 1U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
								  "test Index materializer requires one fresh pool per tree");
			}
			IndexTreeMutationPoolBatch &batch = batches[treeOrdinal].front();
			if (!batch.newly_reserved || !batch.expected_replacements.empty() ||
				batch.appended_page_ids.empty() ||
				batch.appended_page_ids.size() != batch.appended_locations.size()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
								  "test Index materializer received an invalid fresh-pool plan");
			}
			std::vector<BTreePage> pages;
			pages.reserve(batch.appended_page_ids.size());
			for (const PageId pageId: batch.appended_page_ids) {
				const PageOwnerHeader owner{pageId, indexIds[treeOrdinal], PageOwnerKind::POK_INDEX, 0U,
										RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, batch.pool.generation, checkpointLsn};
				BTreePageHeader header;
				header.index_id = indexIds[treeOrdinal];
				header.page_id = pageId;
				header.generation = batch.pool.generation;
				pages.push_back(BTreePage::create(owner, header));
			}
			indexes.prepareNewPoolInitialPages(batch.pool, pages, batch.appended_locations, checkpointLsn);
		}
	}

	void materializeInitialVectorBootstrap(VectorPageStore &vectors, VectorManifestStore &manifests,
									 std::vector<NativeGraphVectorBootstrapMaterialization> &plans,
									 LogSequenceNumber checkpointLsn) {
		for (NativeGraphVectorBootstrapMaterialization &plan: plans) {
			const uint32_t recordBytes =
					static_cast<uint32_t>(sizeof(uint64_t) + static_cast<size_t>(plan.mutation.dimensions) * sizeof(uint16_t));
			const uint16_t capacity = VectorPageStore::rawSlotCapacity(recordBytes);
			if (capacity == 0U || plan.raw_pages.empty() ||
				plan.raw_pools.size() !=
						(plan.raw_pages.size() - 1U) / RSS_PAGE_POOL_DATA_FRAME_COUNT + 1U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
								  "test Vector bootstrap materializer received a noncanonical Raw-page plan");
			}

			std::vector<VectorPage> rawPages;
			rawPages.reserve(plan.raw_pages.size());
			plan.vector_locations.clear();
			uint64_t nextVectorId = plan.mutation.first_vector_id;
			uint64_t remaining = plan.mutation.vector_count;
			for (size_t pageOrdinal = 0U; pageOrdinal < plan.raw_pages.size(); ++pageOrdinal) {
				PageLocation &location = plan.raw_pages[pageOrdinal];
				const CatalogEntry &pool = plan.raw_pools[pageOrdinal / RSS_PAGE_POOL_DATA_FRAME_COUNT];
				VectorPageHeader header;
				header.index_id = plan.mutation.index_id;
				header.page_id = location.page_id;
				header.generation = pool.generation;
				header.kind = VectorStorageKind::VSK_RAW;
				header.record_bytes = recordBytes;
				header.slot_capacity = capacity;
				header.bitmap_bytes = static_cast<uint16_t>((static_cast<uint32_t>(capacity) + 7U) / 8U);
				header.committed_lsn = checkpointLsn;
				const PageOwnerHeader owner{header.page_id, header.index_id, PageOwnerKind::POK_VECTOR_RAW, 0U,
											RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, header.generation, checkpointLsn};
				VectorPage page = VectorPage::create(owner, header);
				const uint64_t count = std::min<uint64_t>(remaining, capacity);
				for (uint64_t ordinal = 0U; ordinal < count; ++ordinal) {
					std::vector<std::byte> record(recordBytes);
					LittleEndianCodec::writeU64(record, 0U, nextVectorId);
					const uint16_t slot = page.insert(record);
					plan.vector_locations.push_back({nextVectorId, header.page_id, slot, page.slotGeneration(slot)});
					++nextVectorId;
				}
				remaining -= count;
				rawPages.push_back(std::move(page));
			}
			if (remaining != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
								  "test Vector bootstrap materializer did not place all Vector records");
			}
			for (size_t poolOrdinal = 0U; poolOrdinal < plan.raw_pools.size(); ++poolOrdinal) {
				const size_t firstPage = poolOrdinal * RSS_PAGE_POOL_DATA_FRAME_COUNT;
				const size_t pageCount = std::min(static_cast<size_t>(RSS_PAGE_POOL_DATA_FRAME_COUNT),
											  rawPages.size() - firstPage);
				vectors.prepareRawNewPoolPages(plan.raw_pools[poolOrdinal],
										  std::span<const VectorPage>(rawPages).subspan(firstPage, pageCount),
										  std::span<PageLocation>(plan.raw_pages).subspan(firstPage, pageCount), checkpointLsn);
			}

			VectorManifest manifest;
			manifest.index_id = plan.mutation.index_id;
			manifest.page_id = plan.manifest_page.page_id;
			manifest.generation = plan.manifest_pool.generation;
			manifest.committed_lsn = checkpointLsn;
			manifest.dimensions = plan.mutation.dimensions;
			manifest.metric = plan.mutation.metric;
			// The metadata-coordinator contract tests may deliberately use simple
			// non-production mapping IDs. The physical bootstrap fixture only needs
			// a canonical durable Raw/Manifest pair; semantic mapping behavior is
			// verified through the production write planner suites.
			manifest.raw_write_tail_page_id = plan.raw_pages.back().page_id;
			for (const VectorPage &page: rawPages) {
				manifest.entries.push_back({page.header().page_id, VectorStorageKind::VSK_RAW, page.header().live_count});
			}
			const PageOwnerHeader manifestOwner{manifest.page_id, manifest.index_id, PageOwnerKind::POK_VECTOR_MANIFEST,
													0U, RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, manifest.generation, checkpointLsn};
			manifests.prepareNewPoolInitial(plan.manifest_pool, VectorManifestPage::create(manifestOwner, manifest),
											checkpointLsn, plan.manifest_page);
			plan.manifest_location = {manifest.index_id, manifest.page_id, manifest.generation};
		}
	}


} // namespace

class RelocatableMetadataStoreValidationTest : public ::testing::Test {
	protected:
		std::filesystem::path dataPath;
		std::shared_ptr<std::fstream> dataFile;
		std::shared_ptr<StorageIO> io;

		void SetUp() override {
			const auto uuid = boost::uuids::random_generator()();
			dataPath = std::filesystem::temp_directory_path() /
					   ("relocatable_metadata_validation_" + to_string(uuid) + ".dat");
			dataFile = std::make_shared<std::fstream>(dataPath, std::ios::in | std::ios::out | std::ios::binary |
															std::ios::trunc);
			ASSERT_TRUE(dataFile->is_open());
			io = std::make_shared<StorageIO>(dataFile, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		}

		void TearDown() override {
			io.reset();
			if (dataFile && dataFile->is_open()) {
				dataFile->close();
			}
			dataFile.reset();
			std::error_code error;
			std::filesystem::remove(dataPath, error);
		}
	};
