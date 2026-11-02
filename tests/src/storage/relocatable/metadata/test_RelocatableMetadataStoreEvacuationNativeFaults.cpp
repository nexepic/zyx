/**
 * @file test_RelocatableMetadataStoreEvacuationNativeFaults.cpp
 * @brief Native I/O fault boundaries for routed metadata-pool evacuation.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "storage/relocatable/metadata/RelocatableMetadataStoreTestFixture.hpp"

#include <cerrno>
#include <system_error>

#include "graph/storage/PreadHelper.hpp"
#include "graph/storage/PwriteHelper.hpp"
#include "graph/storage/relocatable/metadata/CatalogRadix.hpp"
#include "graph/storage/relocatable/metadata/PageDirectoryBootstrap.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

	bool isCandidateBootstrapHeader(const void *bytes, size_t byteCount) {
		if (byteCount != RSS_PAGE_SIZE) {
			return false;
		}
		try {
			const PageDirectoryBootstrap bootstrap = PageDirectoryBootstrapCodec::decode(
					std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount));
			return bootstrap.generation == Generation(10U) && bootstrap.metadata_page_directory_root.isValid();
		} catch (const FormatException &) {
			return false;
		}
	}

	bool isCandidateRouteRadixPage(const void *bytes, size_t byteCount) {
		if (byteCount != RSS_PAGE_SIZE) {
			return false;
		}
		try {
			const MetadataRadixPage page = MetadataRadixPageCodec::decode(
					std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount));
			return page.directory_type == MetadataDirectoryType::MDT_METADATA_PAGE_ROUTE &&
				   page.generation == Generation(10U) && !page.logical_children_enabled;
		} catch (const FormatException &) {
			return false;
		}
	}

	bool isCandidateSuperblock(const void *bytes, size_t byteCount, pwrite_off_t offset) {
		static_cast<void>(bytes);
		return byteCount == RSS_SUPERBLOCK_BYTES && offset >= 0 &&
			   static_cast<uint64_t>(offset) < RSS_FILE_BOOTSTRAP_BYTES;
	}

	void expectOldRoutedGeneration(const RelocatableMetadataStore &store) {
		MetadataSnapshotPin snapshot = store.openSnapshot();
		EXPECT_EQ(snapshot.superblock().generation, Generation(9U));
		EXPECT_EQ(store.findCatalog(snapshot, SegmentId(200U)).physical_slot, PhysicalSlotId(2U));
		EXPECT_EQ(store.findExtentLocation(snapshot, ExtentId(1U)),
				  (ExtentLocation{ExtentId(1U), SegmentId(20U), RSS_PAGE_POOL_FIRST_DATA_FRAME, 1U, Generation(1U)}));
	}

	void expectPublishedRoutedGeneration(const RelocatableMetadataStore &store) {
		MetadataSnapshotPin snapshot = store.openSnapshot();
		EXPECT_EQ(snapshot.superblock().generation, Generation(10U));
		EXPECT_EQ(store.findCatalog(snapshot, SegmentId(200U)).physical_slot, PhysicalSlotId(2U));
		EXPECT_EQ(store.findExtentLocation(snapshot, ExtentId(1U)),
				  (ExtentLocation{ExtentId(1U), SegmentId(20U), RSS_PAGE_POOL_FIRST_DATA_FRAME, 1U, Generation(1U)}));
	}

} // namespace
