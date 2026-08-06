/**
 * @file RelocatableBTreePageStoreTestUtils.hpp
 * @brief Shared fixtures and helpers for relocatable B+Tree page-store tests.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/entity/EntityLabelSetStore.hpp"
#include "graph/storage/relocatable/entity/EntityPageStore.hpp"
#include "graph/storage/relocatable/entity/NativeGraphWritePlanner.hpp"
#include "graph/storage/relocatable/index/BTreeIndex.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"
#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRootStore.hpp"

namespace relocatable_btree_test {

	using namespace graph::storage;
	using namespace graph::storage;

	inline void expectFormatError(const std::function<void()> &operation, FormatError expected) {
		try {
			operation();
			FAIL() << "Expected FormatException";
		} catch (const FormatException &error) {
			EXPECT_EQ(error.error(), expected);
		}
	}

	inline Superblock superblock() {
		Superblock result;
		result.database_id = DatabaseId(0x1020304050607080ULL);
		result.generation = Generation(1);
		result.checkpoint_lsn = LogSequenceNumber(1);
		return result;
	}

	inline SlottedPagePoolAllocationRequest allocationRequest() {
		SlottedPagePoolAllocationRequest result;
		result.segment_id = SegmentId(71);
		result.pool_kind = SegmentKind::SGK_INDEX_PAGE_POOL;
		result.pool_owner_id = 77;
		result.generation = Generation(1);
		result.page_id = PageId(101);
		result.page_owner_id = 17;
		result.page_owner_kind = PageOwnerKind::POK_INDEX;
		return result;
	}

	[[nodiscard]] inline NativeGraphWritePlannerOptions nativeBTreeWriteOptions() {
		NativeGraphWritePlannerOptions options;
		options.first_slot = PhysicalSlotId(41U);
		options.last_slot = PhysicalSlotId(41U);
		options.entity_pool_owner_id = 17U;
		options.property_roots = {PhysicalSlotId(41U), PhysicalSlotId(41U), 18U};
		return options;
	}

	// B+Tree structural mutations must use the same native transaction and
	// publication path as production schema/index writes. No test-only fixed
	// entity transaction is allowed to own an Index mutation.
	class NativeBTreeWriteContext final {
	public:
		NativeBTreeWriteContext(std::shared_ptr<StorageIO> io, RelocatableMetadataStore &metadata,
								SlottedPagePool &pages) :
			entities_(io, metadata), labels_(std::move(io), metadata, pages),
			properties_(pages, metadata), indexes_(pages, metadata),
			planner_(metadata, entities_, labels_, properties_, indexes_, nativeBTreeWriteOptions()) {}

		[[nodiscard]] NativeGraphWriteTransaction beginTransaction() { return planner_.beginTransaction(); }

	private:
		EntityPageStore entities_;
		EntityLabelSetStore labels_;
		PropertyCollectionRootStore properties_;
		BTreePageStore indexes_;
		NativeGraphWritePlanner planner_;
	};

	inline PageOwnerHeader pageOwner(LogSequenceNumber lsn) {
		const auto request = allocationRequest();
		PageOwnerHeader owner;
		owner.page_id = request.page_id;
		owner.owner_id = request.page_owner_id;
		owner.kind = request.page_owner_kind;
		owner.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
		owner.generation = request.generation;
		owner.committed_lsn = lsn;
		return owner;
	}

	inline BTreePage emptyPage(LogSequenceNumber lsn) {
		BTreePageHeader header;
		header.index_id = 17;
		header.page_id = PageId(101);
		header.generation = Generation(1);
		return BTreePage::create(pageOwner(lsn), header);
	}

	inline BTreePage appendedPage(LogSequenceNumber lsn) {
		PageOwnerHeader owner = pageOwner(lsn);
		owner.page_id = PageId(102);
		BTreePageHeader header;
		header.index_id = owner.owner_id;
		header.page_id = owner.page_id;
		header.generation = owner.generation;
		return BTreePage::create(owner, header);
	}

	inline BTreePage appendedPage(PageId pageId, LogSequenceNumber lsn) {
		PageOwnerHeader owner = pageOwner(lsn);
		owner.page_id = pageId;
		BTreePageHeader header;
		header.index_id = owner.owner_id;
		header.page_id = owner.page_id;
		header.generation = owner.generation;
		return BTreePage::create(owner, header);
	}

	inline BTreePage foreignIndexPage(LogSequenceNumber lsn) {
		PageOwnerHeader owner = pageOwner(lsn);
		owner.page_id = PageId(102);
		owner.owner_id = 18;
		BTreePageHeader header;
		header.index_id = owner.owner_id;
		header.page_id = owner.page_id;
		header.generation = owner.generation;
		return BTreePage::create(owner, header);
	}

	inline std::vector<std::byte> largeKey(std::byte marker) { return std::vector<std::byte>(2400U, marker); }

	// Keep adjacent keys large enough to require a structural split while making
	// their first bytes distinguishable, so packed prefix compression cannot turn
	// a deliberately full fixture into a tiny page.
	inline std::vector<std::byte> packedLargeKey(uint16_t value) {
		std::vector<std::byte> key(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES / 2U, std::byte{0x5A});
		key[0] = static_cast<std::byte>(value >> 8U);
		key[1] = static_cast<std::byte>(value);
		return key;
	}

	inline std::vector<std::byte> wideKey(uint16_t value) {
		std::vector<std::byte> key(1024U, std::byte{'k'});
		key[0] = static_cast<std::byte>(value >> 8U);
		key[1] = static_cast<std::byte>(value);
		return key;
	}

	inline std::vector<std::byte> mediumKey(std::byte marker, std::byte suffix) {
		std::vector<std::byte> key(1024U, marker);
		key.back() = suffix;
		return key;
	}

	inline std::vector<std::byte> smallKey(uint16_t value) {
		std::vector<std::byte> key(32U, std::byte{'k'});
		key[0] = static_cast<std::byte>(value >> 8U);
		key[1] = static_cast<std::byte>(value);
		return key;
	}

	inline PageOwnerHeader treeOwner(const CatalogEntry &pool, PageId pageId, LogSequenceNumber lsn) {
		PageOwnerHeader owner;
		owner.page_id = pageId;
		owner.owner_id = 17;
		owner.kind = PageOwnerKind::POK_INDEX;
		owner.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
		owner.generation = pool.generation;
		owner.committed_lsn = lsn;
		return owner;
	}

	inline BTreePage rebuildPage(const CatalogEntry &pool, BTreePageHeader header, LogSequenceNumber lsn,
								  std::span<const BTreePageEntry> entries) {
		header.entry_count = 0;
		BTreePage result = BTreePage::create(treeOwner(pool, header.page_id, lsn), header);
		for (const BTreePageEntry &entry: entries) {
			result.insert(entry);
		}
		return result;
	}

	// Writes a checksummed but semantically inconsistent B+Tree frame so tests
	// can exercise tree-level validation after the page codec has accepted it.
	inline void overwriteCurrentBTreePage(StorageIO &io, RelocatableMetadataStore &metadata, PageId pageId,
										   const PageOwnerHeader &owner, const BTreePage &replacement) {
		MetadataSnapshotPin snapshot = metadata.openSnapshot();
		const PageLocation location = metadata.findPageLocation(snapshot, pageId);
		const CatalogEntry pool = metadata.findCatalog(snapshot, location.segment_id);
		const uint64_t frameOffset =
				PhysicalSegmentAllocator::checkedSlotOffset(snapshot.superblock().data_region_offset, pool.physical_slot) +
				static_cast<uint64_t>(location.page_index) * RSS_PAGE_SIZE;
		snapshot.release();
		const auto ownerBytes = PageOwnerHeaderCodec::encode(owner);
		const std::vector<std::byte> payload = replacement.serialize();
		io.writeAt(frameOffset, ownerBytes.data(), ownerBytes.size());
		io.writeAt(frameOffset + RSS_PAGE_OWNER_HEADER_BYTES, payload.data(), payload.size());
	}

	inline std::vector<BTreePage> twoPageTree(const CatalogEntry &pool, std::span<const PageId> pageIds,
												  LogSequenceNumber lsn) {
		if (pageIds.size() != 2U) {
			throw std::invalid_argument("two-page B+Tree fixture needs exactly two Page IDs");
		}
		BTreePageHeader rootHeader;
		rootHeader.index_id = 17;
		rootHeader.page_id = pageIds[0];
		rootHeader.generation = pool.generation;
		rootHeader.kind = BTreeNodeKind::BPN_INTERNAL;
		rootHeader.level = 1;
		rootHeader.leftmost_child = pageIds[1];
		BTreePage root = BTreePage::create(treeOwner(pool, pageIds[0], lsn), rootHeader);
		root.insert({{std::byte{'m'}}, pageIds[1].value()});

		BTreePageHeader leafHeader;
		leafHeader.index_id = 17;
		leafHeader.page_id = pageIds[1];
		leafHeader.generation = pool.generation;
		BTreePage leaf = BTreePage::create(treeOwner(pool, pageIds[1], lsn), leafHeader);
		leaf.insert({{std::byte{'a'}}, 1});
		std::vector<BTreePage> result;
		result.push_back(std::move(root));
		result.push_back(std::move(leaf));
		return result;
	}

	inline std::vector<BTreePage> successorRepairTree(const CatalogEntry &pool, std::span<const PageId> pageIds,
														 LogSequenceNumber lsn, bool breakSuccessorPredecessorLink = false,
														 bool breakPredecessorSuccessorLink = false,
														 bool usePackedLargeKeys = false) {
		if (pageIds.size() != 7U) {
			throw std::invalid_argument("successor-link B+Tree fixture needs seven Page IDs");
		}
		BTreePageHeader rootHeader;
		rootHeader.index_id = 17;
		rootHeader.page_id = pageIds[0];
		rootHeader.generation = pool.generation;
		rootHeader.kind = BTreeNodeKind::BPN_INTERNAL;
		rootHeader.level = 2;
		rootHeader.leftmost_child = pageIds[1];
		BTreePage root = BTreePage::create(treeOwner(pool, pageIds[0], lsn), rootHeader);
		root.insert({usePackedLargeKeys ? packedLargeKey(0x0300U) : std::vector<std::byte>{std::byte{'c'}},
						 pageIds[2].value()});

		BTreePageHeader leftParentHeader;
		leftParentHeader.index_id = 17;
		leftParentHeader.page_id = pageIds[1];
		leftParentHeader.generation = pool.generation;
		leftParentHeader.kind = BTreeNodeKind::BPN_INTERNAL;
		leftParentHeader.level = 1;
		leftParentHeader.leftmost_child = pageIds[3];
		BTreePage leftParent = BTreePage::create(treeOwner(pool, pageIds[1], lsn), leftParentHeader);
		leftParent.insert({usePackedLargeKeys ? packedLargeKey(0x0200U) : std::vector<std::byte>{std::byte{'b'}},
							 pageIds[4].value()});

		BTreePageHeader rightParentHeader = leftParentHeader;
		rightParentHeader.page_id = pageIds[2];
		rightParentHeader.leftmost_child = pageIds[5];
		BTreePage rightParent = BTreePage::create(treeOwner(pool, pageIds[2], lsn), rightParentHeader);
		rightParent.insert({usePackedLargeKeys ? packedLargeKey(0x0400U) : std::vector<std::byte>{std::byte{'d'}},
							  pageIds[6].value()});

		std::vector<BTreePage> pages;
		pages.reserve(pageIds.size());
		pages.push_back(std::move(root));
		pages.push_back(std::move(leftParent));
		pages.push_back(std::move(rightParent));
		for (size_t index = 0; index < 4U; ++index) {
			BTreePageHeader leafHeader;
			leafHeader.index_id = 17;
			leafHeader.page_id = pageIds[index + 3U];
			leafHeader.generation = pool.generation;
			leafHeader.left_sibling = index == 0U ? PageId{} : pageIds[index + 2U];
			leafHeader.right_sibling = index == 3U ? PageId{} : pageIds[index + 4U];
			if (breakSuccessorPredecessorLink && index == 2U) {
				leafHeader.left_sibling = PageId(999);
			}
			if (breakPredecessorSuccessorLink && index == 1U) {
				leafHeader.right_sibling = PageId(999);
			}
			BTreePage leaf = BTreePage::create(treeOwner(pool, leafHeader.page_id, lsn), leafHeader);
			const std::vector<std::byte> key = usePackedLargeKeys
																? packedLargeKey(static_cast<uint16_t>(0x0100U + index * 0x0100U))
																: index == 0U   ? largeKey(std::byte{'a'})
																		 : index == 1U ? largeKey(std::byte{'b'})
																		 : index == 2U ? largeKey(std::byte{'c'})
																							 : largeKey(std::byte{'d'});
			leaf.insert({key,
																	 index + 1U});
			pages.push_back(std::move(leaf));
		}
		return pages;
	}

	class RelocatableBTreePageStoreTest : public ::testing::Test {
	protected:
		std::filesystem::path dataPath;
		std::filesystem::path walPath;
		std::shared_ptr<std::fstream> dataFile;
		std::shared_ptr<std::fstream> walFile;
		std::shared_ptr<StorageIO> io;
		std::shared_ptr<StorageIO> walIo;

		void SetUp() override {
			const auto uuid = boost::uuids::random_generator()();
			dataPath = std::filesystem::temp_directory_path() / ("relocatable_btree_page_store_" + to_string(uuid) + ".dat");
			walPath = dataPath.string() + ".foreign";
			dataFile = std::make_shared<std::fstream>(dataPath, std::ios::in | std::ios::out | std::ios::binary |
																		std::ios::trunc);
			walFile = std::make_shared<std::fstream>(walPath,
													 std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
			ASSERT_TRUE(dataFile->is_open());
			ASSERT_TRUE(walFile->is_open());
			io = std::make_shared<StorageIO>(dataFile, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
			walIo = std::make_shared<StorageIO>(walFile, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		}

		void TearDown() override {
			io.reset();
			walIo.reset();
			if (dataFile && dataFile->is_open()) {
				dataFile->close();
			}
			if (walFile && walFile->is_open()) {
				walFile->close();
			}
			dataFile.reset();
			walFile.reset();
			std::error_code error;
			std::filesystem::remove(dataPath, error);
			std::filesystem::remove(walPath, error);
		}
	};

} // namespace relocatable_btree_test
