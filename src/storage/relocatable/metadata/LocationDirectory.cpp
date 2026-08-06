/**
 * @file LocationDirectory.cpp
 * @brief Explicit codecs and COW trees for Page and Extent directories.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/metadata/LocationDirectory.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>

#include "graph/storage/relocatable/allocation/SharedPagePool.hpp"
#include "graph/storage/relocatable/metadata/MetadataPageFile.hpp"
#include "graph/storage/relocatable/metadata/MetadataWriteBatch.hpp"
#include "graph/utils/ChecksumUtils.hpp"

namespace graph::storage {

	namespace {

		constexpr size_t MAGIC_OFFSET = 0;
		constexpr size_t VERSION_OFFSET = 4;
		constexpr size_t HEADER_BYTES_OFFSET = 6;
		constexpr size_t ENTRY_BYTES_OFFSET = 8;
		constexpr size_t ENTRY_COUNT_OFFSET = 12;
		constexpr size_t PAGE_ID_OFFSET = 16;
		constexpr size_t GENERATION_OFFSET = 24;
		constexpr size_t LSN_OFFSET = 32;
		constexpr size_t DIRECTORY_TYPE_OFFSET = 40;
		constexpr size_t HEADER_RESERVED_OFFSET = 44;
		constexpr size_t PAYLOAD_CRC_OFFSET = 48;
		constexpr size_t HEADER_CRC_OFFSET = 52;
		constexpr size_t TRAILING_RESERVED_OFFSET = 56;

		constexpr size_t ENTRY_LOGICAL_ID_OFFSET = 0;
		constexpr size_t ENTRY_SEGMENT_ID_OFFSET = 8;
		constexpr size_t ENTRY_GENERATION_OFFSET = 16;
		constexpr size_t ENTRY_FIRST_PAGE_OFFSET = 24;
		constexpr size_t ENTRY_PAGE_COUNT_OFFSET = 26;
		constexpr size_t ENTRY_FREE_BYTES_OFFSET = 28;

		[[noreturn]] void throwLocationDirectoryError(FormatError error, const char *message) {
			throw FormatException(error, message);
		}

		[[nodiscard]] uint32_t calculateCrc(std::span<const std::byte> bytes) {
			return utils::calculateCrc(bytes.data(), bytes.size());
		}

		[[nodiscard]] bool isLocationDirectoryType(MetadataDirectoryType type) {
			return type == MetadataDirectoryType::MDT_PAGE || type == MetadataDirectoryType::MDT_EXTENT ||
				   type == MetadataDirectoryType::MDT_NODE_ENTITY || type == MetadataDirectoryType::MDT_EDGE_ENTITY ||
				   type == MetadataDirectoryType::MDT_ENTITY_PROPERTY ||
				   type == MetadataDirectoryType::MDT_STATE || type == MetadataDirectoryType::MDT_INDEX_ROOT ||
				   type == MetadataDirectoryType::MDT_VECTOR_LOCATION ||
				   type == MetadataDirectoryType::MDT_VECTOR_MANIFEST ||
				   type == MetadataDirectoryType::MDT_LARGE_VALUE_MANIFEST ||
				   type == MetadataDirectoryType::MDT_RETIRE_QUEUE;
		}

		void validateEntry(const LocationLeafEntry &entry, MetadataDirectoryType directoryType) {
			const bool isEntity = directoryType == MetadataDirectoryType::MDT_NODE_ENTITY ||
								  directoryType == MetadataDirectoryType::MDT_EDGE_ENTITY;
			const bool isPage = directoryType == MetadataDirectoryType::MDT_PAGE;
			const bool isProperty = directoryType == MetadataDirectoryType::MDT_ENTITY_PROPERTY;
			const bool isState = directoryType == MetadataDirectoryType::MDT_STATE;
			const bool isIndexRoot = directoryType == MetadataDirectoryType::MDT_INDEX_ROOT;
			const bool isVectorLocation = directoryType == MetadataDirectoryType::MDT_VECTOR_LOCATION;
			const bool isVectorManifest = directoryType == MetadataDirectoryType::MDT_VECTOR_MANIFEST;
			const bool isLargeValueManifest = directoryType == MetadataDirectoryType::MDT_LARGE_VALUE_MANIFEST;
			const bool isRetireQueue = directoryType == MetadataDirectoryType::MDT_RETIRE_QUEUE;
			const uint8_t invalid =
					static_cast<uint8_t>(entry.logical_id == 0U) | static_cast<uint8_t>(!entry.segment_id.isValid()) |
					static_cast<uint8_t>(!entry.generation.isValid()) |
					static_cast<uint8_t>(!isEntity && !isProperty && !isState && !isIndexRoot && !isVectorLocation &&
									 !isVectorManifest && !isLargeValueManifest &&
										 !isRetireQueue && entry.first_page < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
					static_cast<uint8_t>(entry.page_count == 0U) |
					static_cast<uint8_t>(!isEntity && !isProperty && !isState && !isIndexRoot && !isVectorLocation &&
									 !isVectorManifest && !isLargeValueManifest &&
										 !isRetireQueue && entry.first_page > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
					static_cast<uint8_t>(!isEntity && !isProperty && !isState && !isIndexRoot && !isVectorLocation &&
									 !isVectorManifest && !isLargeValueManifest &&
										 !isRetireQueue &&
										 entry.page_count > RSS_PAGE_POOL_DATA_FRAME_COUNT - entry.first_page + 1U) |
					static_cast<uint8_t>((directoryType == MetadataDirectoryType::MDT_PAGE || isEntity) &&
										 entry.page_count != 1U) |
					static_cast<uint8_t>((isProperty || isState || isVectorLocation) &&
										 (entry.page_count != 1U ||
										  entry.first_page == std::numeric_limits<uint16_t>::max() ||
										  entry.generation.value() > std::numeric_limits<uint32_t>::max())) |
					static_cast<uint8_t>(
							(isIndexRoot || isVectorManifest || isLargeValueManifest) &&
							(entry.first_page != RSS_PAGE_POOL_FIRST_DATA_FRAME || entry.page_count != 1U)) |
					static_cast<uint8_t>(
							isRetireQueue &&
							(entry.page_count != 1U || entry.free_bytes != 0U ||
							 (entry.first_page != static_cast<uint16_t>(RetireQueueDisposition::RQD_RELOCATED_SOURCE) &&
							  entry.first_page !=
									  static_cast<uint16_t>(RetireQueueDisposition::RQD_RETIRED_SEGMENT)))) |
					static_cast<uint8_t>(!isPage && entry.free_bytes != 0U) |
					static_cast<uint8_t>(entry.free_bytes > RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES);
			if (invalid != 0U) {
				throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
											"relocatable location directory entry is invalid");
			}
		}

		void validatePage(const LocationLeafPage &page) {
			if (!isLocationDirectoryType(page.directory_type) || !page.page_id.isValid() ||
				!page.generation.isValid() || !page.committed_lsn.isValid() || page.entries.empty() ||
				page.entries.size() > RSS_LOCATION_LEAF_MAX_ENTRIES) {
				throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
											"relocatable location leaf header is invalid");
			}
			uint64_t previous = 0;
			for (const LocationLeafEntry &entry: page.entries) {
				validateEntry(entry, page.directory_type);
				if (entry.logical_id <= previous) {
					throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
												"relocatable location leaf entries are not strictly ordered");
				}
				previous = entry.logical_id;
			}
		}

		void writeHeader(std::span<std::byte> bytes, const LocationLeafPage &page, uint32_t payloadCrc,
						 uint32_t headerCrc) {
			LittleEndianCodec::writeU32(bytes, MAGIC_OFFSET, RSS_LOCATION_LEAF_MAGIC);
			LittleEndianCodec::writeU16(bytes, VERSION_OFFSET, RSS_LOCATION_LEAF_VERSION);
			LittleEndianCodec::writeU16(bytes, HEADER_BYTES_OFFSET, RSS_LOCATION_LEAF_HEADER_BYTES);
			LittleEndianCodec::writeU32(bytes, ENTRY_BYTES_OFFSET, RSS_LOCATION_ENTRY_BYTES);
			LittleEndianCodec::writeU32(bytes, ENTRY_COUNT_OFFSET, static_cast<uint32_t>(page.entries.size()));
			LittleEndianCodec::writeU64(bytes, PAGE_ID_OFFSET, page.page_id.value());
			LittleEndianCodec::writeU64(bytes, GENERATION_OFFSET, page.generation.value());
			LittleEndianCodec::writeU64(bytes, LSN_OFFSET, page.committed_lsn.value());
			LittleEndianCodec::writeU32(bytes, DIRECTORY_TYPE_OFFSET, static_cast<uint32_t>(page.directory_type));
			LittleEndianCodec::writeU32(bytes, HEADER_RESERVED_OFFSET, 0);
			LittleEndianCodec::writeU32(bytes, PAYLOAD_CRC_OFFSET, payloadCrc);
			LittleEndianCodec::writeU32(bytes, HEADER_CRC_OFFSET, headerCrc);
			LittleEndianCodec::writeU64(bytes, TRAILING_RESERVED_OFFSET, 0);
		}

		void writeEntry(std::span<std::byte> bytes, size_t offset, const LocationLeafEntry &entry) {
			LittleEndianCodec::writeU64(bytes, offset + ENTRY_LOGICAL_ID_OFFSET, entry.logical_id);
			LittleEndianCodec::writeU64(bytes, offset + ENTRY_SEGMENT_ID_OFFSET, entry.segment_id.value());
			LittleEndianCodec::writeU64(bytes, offset + ENTRY_GENERATION_OFFSET, entry.generation.value());
			LittleEndianCodec::writeU16(bytes, offset + ENTRY_FIRST_PAGE_OFFSET, entry.first_page);
			LittleEndianCodec::writeU16(bytes, offset + ENTRY_PAGE_COUNT_OFFSET, entry.page_count);
			LittleEndianCodec::writeU32(bytes, offset + ENTRY_FREE_BYTES_OFFSET, entry.free_bytes);
		}

		[[nodiscard]] LocationLeafEntry readEntry(std::span<const std::byte> bytes, size_t offset) {
			LocationLeafEntry entry;
			entry.logical_id = LittleEndianCodec::readU64(bytes, offset + ENTRY_LOGICAL_ID_OFFSET);
			entry.segment_id = SegmentId(LittleEndianCodec::readU64(bytes, offset + ENTRY_SEGMENT_ID_OFFSET));
			entry.generation = Generation(LittleEndianCodec::readU64(bytes, offset + ENTRY_GENERATION_OFFSET));
			entry.first_page = LittleEndianCodec::readU16(bytes, offset + ENTRY_FIRST_PAGE_OFFSET);
			entry.page_count = LittleEndianCodec::readU16(bytes, offset + ENTRY_PAGE_COUNT_OFFSET);
			entry.free_bytes = LittleEndianCodec::readU32(bytes, offset + ENTRY_FREE_BYTES_OFFSET);
			return entry;
		}

		[[nodiscard]] uint64_t leafOrdinal(uint64_t logicalId) {
			return (logicalId - 1U) / RSS_LOCATION_LEAF_MAX_ENTRIES;
		}

		[[nodiscard]] uint64_t radixFactor(uint16_t level) {
			uint64_t factor = 1;
			for (uint16_t index = 0; index < level; ++index) {
				if (factor > std::numeric_limits<uint64_t>::max() /
									 RSS_CATALOG_RADIX_FANOUT) { // ZYX_COV_EXCL_LINE: format caps radix levels below
																 // this overflow threshold.
					throwLocationDirectoryError(FormatError::FERR_ARITHMETIC_OVERFLOW,
												"relocatable location radix level exceeds the logical-ID domain");
				}
				factor *= RSS_CATALOG_RADIX_FANOUT;
			}
			return factor;
		}

		[[nodiscard]] uint16_t requiredRootLevel(uint64_t maximumLogicalId) {
			uint64_t quotient = leafOrdinal(maximumLogicalId);
			uint16_t level = 0;
			while (quotient >= RSS_CATALOG_RADIX_FANOUT) {
				quotient /= RSS_CATALOG_RADIX_FANOUT;
				++level;
			}
			return level;
		}

		void validateVisible(Generation generation, LogSequenceNumber lsn, const Superblock &superblock,
							 const char *message) {
			if (!generation.isValid() || generation > superblock.generation || !lsn.isValid() ||
				lsn > superblock.checkpoint_lsn) {
				throwLocationDirectoryError(FormatError::FERR_INVALID_STATE, message);
			}
		}

		void validateRoute(const LocationLeafPage &page, uint64_t ordinal, const Superblock &superblock) {
			validateVisible(page.generation, page.committed_lsn, superblock,
							"relocatable location leaf is newer than its Superblock");
			for (const LocationLeafEntry &entry: page.entries) {
				if (leafOrdinal(entry.logical_id) != ordinal) {
					throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
												"relocatable location leaf does not match its radix route");
				}
			}
		}

		[[nodiscard]] PhysicalPageRef rootFor(const Superblock &superblock, MetadataDirectoryType directoryType) {
			switch (directoryType) {
				case MetadataDirectoryType::MDT_PAGE:
					return superblock.page_directory_root;
				case MetadataDirectoryType::MDT_EXTENT:
					return superblock.extent_directory_root;
				case MetadataDirectoryType::MDT_NODE_ENTITY:
					return superblock.node_entity_directory_root;
				case MetadataDirectoryType::MDT_EDGE_ENTITY:
					return superblock.edge_entity_directory_root;
				case MetadataDirectoryType::MDT_ENTITY_PROPERTY:
					return superblock.entity_property_directory_root;
				case MetadataDirectoryType::MDT_STATE:
					return superblock.state_directory_root;
				case MetadataDirectoryType::MDT_INDEX_ROOT:
					return superblock.index_root_directory_root;
				case MetadataDirectoryType::MDT_VECTOR_LOCATION:
					return superblock.vector_location_directory_root;
				case MetadataDirectoryType::MDT_VECTOR_MANIFEST:
					return superblock.vector_manifest_directory_root;
				case MetadataDirectoryType::MDT_LARGE_VALUE_MANIFEST:
					return superblock.large_value_manifest_directory_root;
				case MetadataDirectoryType::MDT_RETIRE_QUEUE:
					return superblock.retire_queue_root;
				default: // ZYX_COV_EXCL_LINE: all callers use the validated Page, Extent, Node, or Edge directory
						 // types.
					throwLocationDirectoryError(FormatError::FERR_INVALID_ENUM,
												"relocatable location directory type has no Superblock root");
			}
		}

		struct MutableLeaf final {
			LocationLeafPage page;
		};

		struct MutableNode final {
			CatalogRadixPage page;
			std::map<uint32_t, std::unique_ptr<MutableNode>> node_children;
			std::map<uint32_t, std::unique_ptr<MutableLeaf>> leaf_children;
		};

		[[nodiscard]] std::unique_ptr<MutableNode> makeNode(MetadataDirectoryType type, CatalogRadixPageKind kind,
															uint16_t level, Generation generation,
															LogSequenceNumber lsn, MetadataWriteBatch &batch) {
			auto node = std::make_unique<MutableNode>();
			node->page.kind = kind;
			node->page.directory_type = type;
			node->page.level = level;
			node->page.page_id = batch.allocatePageId();
			node->page.generation = generation;
			node->page.committed_lsn = lsn;
			return node;
		}

		[[nodiscard]] std::unique_ptr<MutableLeaf> makeLeaf(MetadataDirectoryType type, Generation generation,
															LogSequenceNumber lsn, MetadataWriteBatch &batch) {
			auto leaf = std::make_unique<MutableLeaf>();
			leaf->page.directory_type = type;
			leaf->page.page_id = batch.allocatePageId();
			leaf->page.generation = generation;
			leaf->page.committed_lsn = lsn;
			return leaf;
		}

		[[nodiscard]] std::unique_ptr<MutableNode> loadNode(MetadataPageFile &pages, PhysicalPageRef reference,
															uint16_t level, MetadataDirectoryType type,
															const Superblock &superblock) {
			CatalogRadixPage page = pages.readMetadataRadix(reference);
			if (page.kind != CatalogRadixPageKind::CRPK_INTERNAL || page.directory_type != type ||
				page.level != level) {
				throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
											"relocatable location directory has an invalid internal radix page");
			}
			validateVisible(page.generation, page.committed_lsn, superblock,
							"relocatable location internal page is newer than its Superblock");
			auto node = std::make_unique<MutableNode>();
			node->page = std::move(page);
			return node;
		}

		[[nodiscard]] std::unique_ptr<MutableLeaf> loadLeaf(MetadataPageFile &pages, PhysicalPageRef reference,
															uint64_t ordinal, MetadataDirectoryType type,
															const Superblock &superblock) {
			LocationLeafPage page = pages.readLocationLeaf(reference);
			if (page.directory_type != type) {
				throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
											"relocatable location leaf has an unexpected directory type");
			}
			validateRoute(page, ordinal, superblock);
			auto leaf = std::make_unique<MutableLeaf>();
			leaf->page = std::move(page);
			return leaf;
		}

		void upsertLeaf(MutableLeaf &leaf, const LocationLeafEntry &entry, Generation generation,
						LogSequenceNumber lsn) {
			auto position = std::lower_bound(leaf.page.entries.begin(), leaf.page.entries.end(), entry.logical_id,
											 [](const LocationLeafEntry &candidate, uint64_t logicalId) {
												 return candidate.logical_id < logicalId;
											 });
			if (position != leaf.page.entries.end() && position->logical_id == entry.logical_id) {
				*position = entry;
			} else {
				leaf.page.entries.insert(position, entry);
			}
			leaf.page.generation = generation;
			leaf.page.committed_lsn = lsn;
			validatePage(leaf.page);
		}

		void applyMutation(MutableNode &node, const LocationLeafEntry &entry, MetadataDirectoryType type,
						   Generation generation, LogSequenceNumber lsn, const Superblock &superblock,
						   MetadataPageFile &pages, MetadataWriteBatch &batch) {
			node.page.generation = generation;
			node.page.committed_lsn = lsn;
			const uint64_t ordinal = leafOrdinal(entry.logical_id);
			const uint32_t index = CatalogRadixPageCodec::childIndexForLeafOrdinal(ordinal, node.page.level);
			if (node.page.level == 0U) {
				auto iterator = node.leaf_children.find(index);
				if (iterator == node.leaf_children.end()) {
					std::unique_ptr<MutableLeaf> leaf =
							node.page.children[index].isValid()
									? loadLeaf(pages, node.page.children[index], ordinal, type, superblock)
									: makeLeaf(type, generation, lsn, batch);
					iterator = node.leaf_children.emplace(index, std::move(leaf)).first;
				}
				upsertLeaf(*iterator->second, entry, generation, lsn);
				return;
			}
			auto iterator = node.node_children.find(index);
			if (iterator == node.node_children.end()) {
				const uint16_t childLevel = node.page.level - 1U;
				std::unique_ptr<MutableNode> child =
						node.page.children[index].isValid()
								? loadNode(pages, node.page.children[index], childLevel, type, superblock)
								: makeNode(type, CatalogRadixPageKind::CRPK_INTERNAL, childLevel, generation, lsn,
										   batch);
				iterator = node.node_children.emplace(index, std::move(child)).first;
			}
			applyMutation(*iterator->second, entry, type, generation, lsn, superblock, pages, batch);
		}

		void applyErase(MutableNode &node, uint64_t logicalId, MetadataDirectoryType type, Generation generation,
						LogSequenceNumber lsn, const Superblock &superblock, MetadataPageFile &pages) {
			node.page.generation = generation;
			node.page.committed_lsn = lsn;
			const uint64_t ordinal = leafOrdinal(logicalId);
			const uint32_t index = CatalogRadixPageCodec::childIndexForLeafOrdinal(ordinal, node.page.level);
			if (!node.page.children[index].isValid()) {
				throwLocationDirectoryError(FormatError::FERR_INVALID_STATE,
											"relocatable location directory deletion references an absent radix route");
			}
			if (node.page.level != 0U) {
				auto iterator = node.node_children.find(index);
				if (iterator == node.node_children.end()) {
					const uint16_t childLevel = node.page.level - 1U;
					iterator = node.node_children
									   .emplace(index, loadNode(pages, node.page.children[index], childLevel, type,
																superblock))
									   .first;
				}
				applyErase(*iterator->second, logicalId, type, generation, lsn, superblock, pages);
				return;
			}
			auto iterator = node.leaf_children.find(index);
			if (iterator == node.leaf_children.end()) {
				iterator =
						node.leaf_children
								.emplace(index, loadLeaf(pages, node.page.children[index], ordinal, type, superblock))
								.first;
			}
			std::vector<LocationLeafEntry> &entries = iterator->second->page.entries;
			const auto position = std::lower_bound(
					entries.begin(), entries.end(), logicalId,
					[](const LocationLeafEntry &candidate, uint64_t id) { return candidate.logical_id < id; });
			if (position == entries.end() || position->logical_id != logicalId) {
				throwLocationDirectoryError(FormatError::FERR_INVALID_STATE,
											"relocatable location directory deletion references an absent logical ID");
			}
			entries.erase(position);
			if (entries.empty()) {
				node.leaf_children.erase(iterator);
				node.page.children[index] = {};
				return;
			}
			iterator->second->page.generation = generation;
			iterator->second->page.committed_lsn = lsn;
			validatePage(iterator->second->page);
		}

		[[nodiscard]] PhysicalPageRef writeLeaf(MutableLeaf &leaf, Generation generation, LogSequenceNumber lsn,
												MetadataPageFile &pages, MetadataWriteBatch &batch) {
			leaf.page.generation = generation;
			leaf.page.committed_lsn = lsn;
			const PhysicalPageRef reference = batch.allocatePage();
			pages.writeLocationLeaf(reference, leaf.page);
			return reference;
		}

		[[nodiscard]] PhysicalPageRef writeNode(MutableNode &node, CatalogRadixPageKind kind, Generation generation,
												LogSequenceNumber lsn, MetadataPageFile &pages,
												MetadataWriteBatch &batch) {
			node.page.kind = kind;
			node.page.generation = generation;
			node.page.committed_lsn = lsn;
			if (node.page.level == 0U) {
				for (auto &[index, leaf]: node.leaf_children) {
					node.page.children[index] = writeLeaf(*leaf, generation, lsn, pages, batch);
				}
			} else {
				for (auto &[index, child]: node.node_children) {
					node.page.children[index] =
							writeNode(*child, CatalogRadixPageKind::CRPK_INTERNAL, generation, lsn, pages, batch);
				}
			}
			const PhysicalPageRef reference = batch.allocatePage();
			pages.writeMetadataRadix(reference, node.page);
			return reference;
		}

		[[nodiscard]] CatalogRadixPage readRoot(MetadataPageFile &pages, const Superblock &superblock,
												MetadataDirectoryType type) {
			CatalogRadixPage root = pages.readMetadataRadix(rootFor(superblock, type));
			if (root.kind != CatalogRadixPageKind::CRPK_ROOT || root.directory_type != type) {
				throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
											"relocatable location Superblock root has an invalid directory type");
			}
			validateVisible(root.generation, root.committed_lsn, superblock,
							"relocatable location root is newer than its Superblock");
			return root;
		}

	} // namespace

	uint64_t EntityPropertyKey::directoryId() const {
		if (!isValid()) {
			throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
										"relocatable Entity Property key has an invalid owner kind or Entity ID");
		}
		return owner_kind == EntityPropertyOwnerKind::EPOK_EDGE ? entity_id | (UINT64_C(1) << 63U) : entity_id;
	}

	EntityPropertyKey EntityPropertyKey::fromDirectoryId(uint64_t directoryId) {
		if (directoryId == 0U) {
			throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
										"relocatable Entity Property directory contains an invalid zero key");
		}
		const bool isEdge = (directoryId & (UINT64_C(1) << 63U)) != 0U;
		const EntityPropertyKey key{isEdge ? EntityPropertyOwnerKind::EPOK_EDGE : EntityPropertyOwnerKind::EPOK_NODE,
									directoryId & ((UINT64_C(1) << 63U) - 1U)};
		if (!key.isValid()) {
			throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
										"relocatable Entity Property directory contains a malformed key");
		}
		return key;
	}

	std::array<std::byte, RSS_PAGE_SIZE> LocationLeafPageCodec::encode(const LocationLeafPage &page) {
		validatePage(page);
		std::array<std::byte, RSS_PAGE_SIZE> bytes{};
		writeHeader(bytes, page, 0, 0);
		for (size_t index = 0; index < page.entries.size(); ++index) {
			writeEntry(bytes, RSS_LOCATION_LEAF_HEADER_BYTES + index * RSS_LOCATION_ENTRY_BYTES, page.entries[index]);
		}
		const uint32_t payloadCrc = calculateCrc(std::span(bytes).subspan(RSS_LOCATION_LEAF_HEADER_BYTES));
		writeHeader(bytes, page, payloadCrc, 0);
		writeHeader(bytes, page, payloadCrc, calculateCrc(std::span(bytes).first(RSS_LOCATION_LEAF_HEADER_BYTES)));
		return bytes;
	}

	LocationLeafPage LocationLeafPageCodec::decode(std::span<const std::byte> bytes) {
		if (bytes.size() < RSS_PAGE_SIZE) {
			throwLocationDirectoryError(FormatError::FERR_BUFFER_TOO_SMALL, "relocatable location leaf is truncated");
		}
		if (bytes.size() != RSS_PAGE_SIZE) {
			throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
										"relocatable location leaf has an unexpected size");
		}
		std::array<std::byte, RSS_LOCATION_LEAF_HEADER_BYTES> header{};
		std::copy_n(bytes.begin(), header.size(), header.begin());
		const uint32_t expectedHeaderCrc = LittleEndianCodec::readU32(header, HEADER_CRC_OFFSET);
		LittleEndianCodec::writeU32(header, HEADER_CRC_OFFSET, 0);
		if (calculateCrc(header) != expectedHeaderCrc) {
			throwLocationDirectoryError(FormatError::FERR_CHECKSUM_MISMATCH,
										"relocatable location leaf header checksum does not match");
		}
		const uint32_t rawDirectoryType = LittleEndianCodec::readU32(bytes, DIRECTORY_TYPE_OFFSET);
		const uint32_t entryCount = LittleEndianCodec::readU32(bytes, ENTRY_COUNT_OFFSET);
		const uint8_t invalidHeader =
				static_cast<uint8_t>(LittleEndianCodec::readU32(bytes, MAGIC_OFFSET) != RSS_LOCATION_LEAF_MAGIC) |
				static_cast<uint8_t>(LittleEndianCodec::readU16(bytes, VERSION_OFFSET) != RSS_LOCATION_LEAF_VERSION) |
				static_cast<uint8_t>(LittleEndianCodec::readU16(bytes, HEADER_BYTES_OFFSET) !=
									 RSS_LOCATION_LEAF_HEADER_BYTES) |
				static_cast<uint8_t>(LittleEndianCodec::readU32(bytes, ENTRY_BYTES_OFFSET) !=
									 RSS_LOCATION_ENTRY_BYTES) |
				static_cast<uint8_t>(entryCount == 0U || entryCount > RSS_LOCATION_LEAF_MAX_ENTRIES) |
				static_cast<uint8_t>(LittleEndianCodec::readU32(bytes, HEADER_RESERVED_OFFSET) != 0U) |
				static_cast<uint8_t>(LittleEndianCodec::readU64(bytes, TRAILING_RESERVED_OFFSET) != 0U) |
				static_cast<uint8_t>(
						rawDirectoryType != static_cast<uint32_t>(MetadataDirectoryType::MDT_PAGE) &&
						rawDirectoryType != static_cast<uint32_t>(MetadataDirectoryType::MDT_EXTENT) &&
						rawDirectoryType != static_cast<uint32_t>(MetadataDirectoryType::MDT_NODE_ENTITY) &&
						rawDirectoryType != static_cast<uint32_t>(MetadataDirectoryType::MDT_EDGE_ENTITY) &&
						rawDirectoryType != static_cast<uint32_t>(MetadataDirectoryType::MDT_ENTITY_PROPERTY) &&
						rawDirectoryType != static_cast<uint32_t>(MetadataDirectoryType::MDT_STATE) &&
						rawDirectoryType != static_cast<uint32_t>(MetadataDirectoryType::MDT_INDEX_ROOT) &&
						rawDirectoryType != static_cast<uint32_t>(MetadataDirectoryType::MDT_VECTOR_LOCATION) &&
						rawDirectoryType != static_cast<uint32_t>(MetadataDirectoryType::MDT_VECTOR_MANIFEST) &&
						rawDirectoryType != static_cast<uint32_t>(MetadataDirectoryType::MDT_LARGE_VALUE_MANIFEST) &&
						rawDirectoryType != static_cast<uint32_t>(MetadataDirectoryType::MDT_RETIRE_QUEUE));
		if (invalidHeader != 0U) {
			throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
										"relocatable location leaf header is invalid");
		}
		if (calculateCrc(bytes.subspan(RSS_LOCATION_LEAF_HEADER_BYTES)) !=
			LittleEndianCodec::readU32(bytes, PAYLOAD_CRC_OFFSET)) {
			throwLocationDirectoryError(FormatError::FERR_CHECKSUM_MISMATCH,
										"relocatable location leaf payload checksum does not match");
		}
		LocationLeafPage page;
		page.directory_type = static_cast<MetadataDirectoryType>(rawDirectoryType);
		page.page_id = PageId(LittleEndianCodec::readU64(bytes, PAGE_ID_OFFSET));
		page.generation = Generation(LittleEndianCodec::readU64(bytes, GENERATION_OFFSET));
		page.committed_lsn = LogSequenceNumber(LittleEndianCodec::readU64(bytes, LSN_OFFSET));
		page.entries.reserve(entryCount);
		for (uint32_t index = 0; index < entryCount; ++index) {
			page.entries.push_back(readEntry(bytes, RSS_LOCATION_LEAF_HEADER_BYTES + index * RSS_LOCATION_ENTRY_BYTES));
		}
		validatePage(page);
		return page;
	}

	LocationDirectory::LocationDirectory(MetadataPageFile &pages, MetadataPageAllocator &allocator) :
		pages_(pages), allocator_(allocator) {}

	void LocationDirectory::prepareUpsertPages(MetadataWriteBatch &batch, std::span<const PageLocation> entries) {
		std::vector<LocationLeafEntry> converted;
		converted.reserve(entries.size());
		for (const PageLocation &entry: entries) {
			batch.observeLogicalPageId(entry.page_id);
			converted.push_back(
					{entry.page_id.value(), entry.segment_id, entry.generation, entry.page_index, 1, entry.free_bytes});
		}
		prepareUpsert(batch, MetadataDirectoryType::MDT_PAGE, converted);
	}

	void LocationDirectory::prepareReplacePages(MetadataWriteBatch &batch, std::span<const PageLocation> entries,
												std::span<const uint64_t> erasedPageIds) {
		std::vector<LocationLeafEntry> converted;
		converted.reserve(entries.size());
		for (const PageLocation &entry: entries) {
			batch.observeLogicalPageId(entry.page_id);
			converted.push_back(
					{entry.page_id.value(), entry.segment_id, entry.generation, entry.page_index, 1, entry.free_bytes});
		}
		prepareReplace(batch, MetadataDirectoryType::MDT_PAGE, converted, erasedPageIds);
	}

	void LocationDirectory::prepareUpsertExtents(MetadataWriteBatch &batch, std::span<const ExtentLocation> entries) {
		std::vector<LocationLeafEntry> converted;
		converted.reserve(entries.size());
		for (const ExtentLocation &entry: entries) {
			converted.push_back(
					{entry.extent_id.value(), entry.segment_id, entry.generation, entry.first_page, entry.page_count});
		}
		prepareUpsert(batch, MetadataDirectoryType::MDT_EXTENT, converted);
	}

	void LocationDirectory::prepareReplaceExtents(MetadataWriteBatch &batch, std::span<const ExtentLocation> entries,
												  std::span<const uint64_t> erasedExtentIds) {
		std::vector<LocationLeafEntry> converted;
		converted.reserve(entries.size());
		for (const ExtentLocation &entry: entries) {
			converted.push_back(
					{entry.extent_id.value(), entry.segment_id, entry.generation, entry.first_page, entry.page_count});
		}
		prepareReplace(batch, MetadataDirectoryType::MDT_EXTENT, converted, erasedExtentIds);
	}

	void LocationDirectory::prepareReplaceNodePageLocations(MetadataWriteBatch &batch,
															std::span<const EntityPageLocation> entries,
															std::span<const uint64_t> erasedEntityIds) {
		std::vector<LocationLeafEntry> converted;
		converted.reserve(entries.size());
		for (const EntityPageLocation &entry: entries) {
			converted.push_back({entry.entity_id, SegmentId(entry.page_id.value()), Generation(entry.slot_generation),
								 entry.slot_id, 1});
		}
		prepareReplace(batch, MetadataDirectoryType::MDT_NODE_ENTITY, converted, erasedEntityIds);
	}

	void LocationDirectory::prepareReplaceEdgePageLocations(MetadataWriteBatch &batch,
															std::span<const EntityPageLocation> entries,
															std::span<const uint64_t> erasedEntityIds) {
		std::vector<LocationLeafEntry> converted;
		converted.reserve(entries.size());
		for (const EntityPageLocation &entry: entries) {
			converted.push_back({entry.entity_id, SegmentId(entry.page_id.value()), Generation(entry.slot_generation),
								 entry.slot_id, 1});
		}
		prepareReplace(batch, MetadataDirectoryType::MDT_EDGE_ENTITY, converted, erasedEntityIds);
	}

	void LocationDirectory::prepareReplaceEntityProperties(MetadataWriteBatch &batch,
														   std::span<const EntityPropertyLocation> entries,
														   std::span<const EntityPropertyKey> erasedKeys) {
		std::vector<LocationLeafEntry> converted;
		converted.reserve(entries.size());
		for (const EntityPropertyLocation &entry: entries) {
			converted.push_back({entry.key.directoryId(), SegmentId(entry.page_id.value()),
								 Generation(entry.slot_generation), entry.slot_id, 1});
		}
		std::vector<uint64_t> erased;
		erased.reserve(erasedKeys.size());
		for (const EntityPropertyKey &key: erasedKeys) {
			erased.push_back(key.directoryId());
		}
		prepareReplace(batch, MetadataDirectoryType::MDT_ENTITY_PROPERTY, converted, erased);
	}

	void LocationDirectory::prepareReplaceStates(MetadataWriteBatch &batch, std::span<const StateLocation> entries,
												 std::span<const uint64_t> erasedStateIds) {
		std::vector<LocationLeafEntry> converted;
		converted.reserve(entries.size());
		for (const StateLocation &entry: entries) {
			converted.push_back({entry.state_id, SegmentId(entry.page_id.value()), Generation(entry.slot_generation),
								 entry.slot_id, 1});
		}
		prepareReplace(batch, MetadataDirectoryType::MDT_STATE, converted, erasedStateIds);
	}

	void LocationDirectory::prepareReplaceIndexRoots(MetadataWriteBatch &batch,
													 std::span<const IndexRootLocation> entries,
													 std::span<const uint64_t> erasedIndexIds) {
		std::vector<LocationLeafEntry> converted;
		converted.reserve(entries.size());
		for (const IndexRootLocation &entry: entries) {
			converted.push_back({entry.index_id, SegmentId(entry.root_page_id.value()), entry.root_generation,
								 RSS_PAGE_POOL_FIRST_DATA_FRAME, 1});
		}
		prepareReplace(batch, MetadataDirectoryType::MDT_INDEX_ROOT, converted, erasedIndexIds);
	}

	void LocationDirectory::prepareReplaceVectorLocations(MetadataWriteBatch &batch,
														  std::span<const VectorLocation> entries,
														  std::span<const uint64_t> erasedVectorIds) {
		std::vector<LocationLeafEntry> converted;
		converted.reserve(entries.size());
		for (const VectorLocation &entry: entries) {
			converted.push_back({entry.vector_id, SegmentId(entry.page_id.value()), Generation(entry.slot_generation),
								 entry.slot_id, 1});
		}
		prepareReplace(batch, MetadataDirectoryType::MDT_VECTOR_LOCATION, converted, erasedVectorIds);
	}

	void LocationDirectory::prepareReplaceVectorManifests(MetadataWriteBatch &batch,
														  std::span<const VectorManifestLocation> entries,
														  std::span<const uint64_t> erasedIndexIds) {
		std::vector<LocationLeafEntry> converted;
		converted.reserve(entries.size());
		for (const VectorManifestLocation &entry: entries) {
			converted.push_back({entry.index_id, SegmentId(entry.manifest_page_id.value()), entry.generation,
								 RSS_PAGE_POOL_FIRST_DATA_FRAME, 1});
		}
		prepareReplace(batch, MetadataDirectoryType::MDT_VECTOR_MANIFEST, converted, erasedIndexIds);
	}

	void LocationDirectory::prepareReplaceLargeValueManifests(MetadataWriteBatch &batch,
															  std::span<const LargeValueManifestLocation> entries,
															  std::span<const LargeValueId> erasedValueIds) {
		std::vector<LocationLeafEntry> converted;
		converted.reserve(entries.size());
		for (const LargeValueManifestLocation &entry: entries) {
			converted.push_back({entry.value_id.value(), SegmentId(entry.manifest_page_id.value()), entry.generation,
								 RSS_PAGE_POOL_FIRST_DATA_FRAME, 1});
		}
		std::vector<uint64_t> erased;
		erased.reserve(erasedValueIds.size());
		for (const LargeValueId valueId: erasedValueIds) {
			erased.push_back(valueId.value());
		}
		prepareReplace(batch, MetadataDirectoryType::MDT_LARGE_VALUE_MANIFEST, converted, erased);
	}

	void LocationDirectory::prepareReplaceRetireQueue(MetadataWriteBatch &batch,
													  std::span<const RetireQueueEntry> entries,
													  std::span<const PhysicalSlotId> erasedSlots) {
		std::vector<LocationLeafEntry> converted;
		converted.reserve(entries.size());
		for (const RetireQueueEntry &entry: entries) {
			converted.push_back({entry.physical_slot.value(), entry.segment_id, entry.retired_generation,
								 static_cast<uint16_t>(entry.disposition), 1});
		}
		std::vector<uint64_t> erased;
		erased.reserve(erasedSlots.size());
		for (const PhysicalSlotId slot: erasedSlots) {
			erased.push_back(slot.value());
		}
		prepareReplace(batch, MetadataDirectoryType::MDT_RETIRE_QUEUE, converted, erased);
	}

	void LocationDirectory::prepareUpsert(MetadataWriteBatch &batch, MetadataDirectoryType directoryType,
										  std::span<const LocationLeafEntry> entries) {
		if (!batch.usesAllocator(allocator_) || entries.empty()) {
			throwLocationDirectoryError(FormatError::FERR_INVALID_STATE,
										"relocatable location directory needs one matching metadata batch and entries");
		}
		uint64_t previous = 0;
		for (const LocationLeafEntry &entry: entries) {
			validateEntry(entry, directoryType);
			if (entry.logical_id <= previous) {
				throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
											"relocatable location directory mutations must be strictly ordered");
			}
			previous = entry.logical_id;
		}
		const Superblock &current = batch.current();
		const Generation generation = batch.generation();
		const LogSequenceNumber lsn = batch.checkpointLsn();
		const uint16_t targetLevel = requiredRootLevel(entries.back().logical_id);
		std::unique_ptr<MutableNode> root;
		if (!rootFor(current, directoryType).isValid()) {
			root = makeNode(directoryType, CatalogRadixPageKind::CRPK_ROOT, targetLevel, generation, lsn, batch);
		} else {
			root = std::make_unique<MutableNode>();
			root->page = readRoot(pages_, current, directoryType);
			while (root->page.level < targetLevel) {
				root->page.kind = CatalogRadixPageKind::CRPK_INTERNAL;
				root->page.generation = generation;
				root->page.committed_lsn = lsn;
				auto grown = makeNode(directoryType, CatalogRadixPageKind::CRPK_ROOT,
									  static_cast<uint16_t>(root->page.level + 1U), generation, lsn, batch);
				grown->node_children.emplace(0U, std::move(root));
				root = std::move(grown);
			}
		}
		for (const LocationLeafEntry &entry: entries) {
			applyMutation(*root, entry, directoryType, generation, lsn, current, pages_, batch);
		}
		const PhysicalPageRef newRoot =
				writeNode(*root, CatalogRadixPageKind::CRPK_ROOT, generation, lsn, pages_, batch);
		switch (directoryType) {
			case MetadataDirectoryType::MDT_PAGE:
				batch.setPageDirectoryRoot(newRoot);
				break;
			case MetadataDirectoryType::MDT_EXTENT:
				batch.setExtentDirectoryRoot(newRoot);
				break;
			case MetadataDirectoryType::MDT_NODE_ENTITY:
				batch.setNodeEntityDirectoryRoot(newRoot);
				break;
			case MetadataDirectoryType::MDT_EDGE_ENTITY:
				batch.setEdgeEntityDirectoryRoot(newRoot);
				break;
			default: // ZYX_COV_EXCL_LINE: prepareUpsert accepts only Page, Extent, Node, or Edge directory types.
				throwLocationDirectoryError(FormatError::FERR_INVALID_ENUM,
											"relocatable location directory type has no publication target");
		}
	}

	void LocationDirectory::prepareReplace(MetadataWriteBatch &batch, MetadataDirectoryType directoryType,
										   std::span<const LocationLeafEntry> entries,
										   std::span<const uint64_t> erasedLogicalIds) {
		if (!batch.usesAllocator(allocator_) || (entries.empty() && erasedLogicalIds.empty())) {
			throwLocationDirectoryError(
					FormatError::FERR_INVALID_STATE,
					"relocatable location replacement requires a matching batch and at least one mutation");
		}
		uint64_t previousEntry = 0;
		for (const LocationLeafEntry &entry: entries) {
			validateEntry(entry, directoryType);
			if (entry.logical_id <= previousEntry) {
				throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
											"relocatable location replacement entries must be strictly ordered");
			}
			previousEntry = entry.logical_id;
		}
		uint64_t previousErased = 0;
		for (uint64_t logicalId: erasedLogicalIds) {
			const auto overlap =
					std::lower_bound(entries.begin(), entries.end(), logicalId,
									 [](const LocationLeafEntry &entry, uint64_t id) { return entry.logical_id < id; });
			if (logicalId == 0U || logicalId <= previousErased ||
				(overlap != entries.end() && overlap->logical_id == logicalId)) {
				throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
											"relocatable location replacement has invalid or overlapping deletion IDs");
			}
			previousErased = logicalId;
		}
		const Superblock &current = batch.current();
		const Generation generation = batch.generation();
		const LogSequenceNumber lsn = batch.checkpointLsn();
		std::unique_ptr<MutableNode> root;
		if (!rootFor(current, directoryType).isValid()) {
			if (entries.empty()) {
				throwLocationDirectoryError(FormatError::FERR_INVALID_STATE,
											"relocatable location replacement cannot erase an absent directory");
			}
			root = makeNode(directoryType, CatalogRadixPageKind::CRPK_ROOT,
							requiredRootLevel(entries.back().logical_id), generation, lsn, batch);
		} else {
			root = std::make_unique<MutableNode>();
			root->page = readRoot(pages_, current, directoryType);
			const uint16_t targetLevel =
					entries.empty() ? root->page.level : requiredRootLevel(entries.back().logical_id);
			while (root->page.level < targetLevel) {
				root->page.kind = CatalogRadixPageKind::CRPK_INTERNAL;
				root->page.generation = generation;
				root->page.committed_lsn = lsn;
				auto grown = makeNode(directoryType, CatalogRadixPageKind::CRPK_ROOT,
									  static_cast<uint16_t>(root->page.level + 1U), generation, lsn, batch);
				grown->node_children.emplace(0U, std::move(root));
				root = std::move(grown);
			}
		}
		for (const LocationLeafEntry &entry: entries) {
			applyMutation(*root, entry, directoryType, generation, lsn, current, pages_, batch);
		}
		for (uint64_t logicalId: erasedLogicalIds) {
			applyErase(*root, logicalId, directoryType, generation, lsn, current, pages_);
		}
		const PhysicalPageRef newRoot =
				writeNode(*root, CatalogRadixPageKind::CRPK_ROOT, generation, lsn, pages_, batch);
		switch (directoryType) {
			case MetadataDirectoryType::MDT_PAGE:
				batch.setPageDirectoryRoot(newRoot);
				break;
			case MetadataDirectoryType::MDT_EXTENT:
				batch.setExtentDirectoryRoot(newRoot);
				break;
			case MetadataDirectoryType::MDT_NODE_ENTITY:
				batch.setNodeEntityDirectoryRoot(newRoot);
				break;
			case MetadataDirectoryType::MDT_EDGE_ENTITY:
				batch.setEdgeEntityDirectoryRoot(newRoot);
				break;
			case MetadataDirectoryType::MDT_ENTITY_PROPERTY:
				batch.setEntityPropertyDirectoryRoot(newRoot);
				break;
			case MetadataDirectoryType::MDT_STATE:
				batch.setStateDirectoryRoot(newRoot);
				break;
			case MetadataDirectoryType::MDT_INDEX_ROOT:
				batch.setIndexRootDirectoryRoot(newRoot);
				break;
			case MetadataDirectoryType::MDT_VECTOR_LOCATION:
				batch.setVectorLocationDirectoryRoot(newRoot);
				break;
			case MetadataDirectoryType::MDT_VECTOR_MANIFEST:
				batch.setVectorManifestDirectoryRoot(newRoot);
				break;
			case MetadataDirectoryType::MDT_LARGE_VALUE_MANIFEST:
				batch.setLargeValueManifestDirectoryRoot(newRoot);
				break;
			case MetadataDirectoryType::MDT_RETIRE_QUEUE:
				batch.setRetireQueueRoot(newRoot);
				break;
			default: // ZYX_COV_EXCL_LINE: prepareReplace accepts only entity or Property directories.
				throwLocationDirectoryError(FormatError::FERR_INVALID_ENUM,
											"relocatable location replacement has no publication target");
		}
	}

	LocationLeafEntry LocationDirectory::find(const Superblock &superblock, MetadataDirectoryType directoryType,
											  uint64_t logicalId) const {
		const std::optional<LocationLeafEntry> entry = findOptional(superblock, directoryType, logicalId);
		if (!entry.has_value()) {
			throwLocationDirectoryError(FormatError::FERR_INVALID_STATE,
										"relocatable location lookup requires an ID and published directory root");
		}
		return *entry;
	}

	std::optional<LocationLeafEntry> LocationDirectory::findOptional(const Superblock &superblock,
																	 MetadataDirectoryType directoryType,
																	 uint64_t logicalId) const {
		static_cast<void>(SuperblockCodec::encode(superblock));
		if (logicalId == 0U) {
			throwLocationDirectoryError(FormatError::FERR_INVALID_STATE, "relocatable location lookup requires an ID");
		}
		if (!rootFor(superblock, directoryType).isValid()) {
			return std::nullopt;
		}
		CatalogRadixPage node = readRoot(pages_, superblock, directoryType);
		const uint64_t ordinal = leafOrdinal(logicalId);
		for (;;) {
			const std::optional<PhysicalPageRef> reference =
					CatalogRadixPageCodec::findChildForLeafOrdinal(node, ordinal);
			if (!reference.has_value()) {
				return std::nullopt;
			}
			if (node.level == 0U) {
				const LocationLeafPage leaf = pages_.readLocationLeaf(*reference);
				if (leaf.directory_type != directoryType) {
					throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
												"relocatable location leaf has an unexpected directory type");
				}
				validateRoute(leaf, ordinal, superblock);
				const auto position = std::lower_bound(
						leaf.entries.begin(), leaf.entries.end(), logicalId,
						[](const LocationLeafEntry &entry, uint64_t id) { return entry.logical_id < id; });
				if (position == leaf.entries.end() || position->logical_id != logicalId) {
					return std::nullopt;
				}
				return *position;
			}
			CatalogRadixPage child = pages_.readMetadataRadix(*reference);
			if (child.kind != CatalogRadixPageKind::CRPK_INTERNAL || child.directory_type != directoryType ||
				child.level + 1U != node.level) {
				throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
											"relocatable location directory has an invalid radix route");
			}
			validateVisible(child.generation, child.committed_lsn, superblock,
							"relocatable location internal page is newer than its Superblock");
			node = std::move(child);
		}
	}

	void LocationDirectory::requireRangeAbsent(const Superblock &superblock, MetadataDirectoryType directoryType,
											   uint64_t firstLogicalId, uint32_t count) const {
		if ((directoryType != MetadataDirectoryType::MDT_NODE_ENTITY &&
			 directoryType != MetadataDirectoryType::MDT_EDGE_ENTITY) ||
			firstLogicalId == 0U || count == 0U ||
			static_cast<uint64_t>(count) - 1U > std::numeric_limits<uint64_t>::max() - firstLogicalId) {
			throwLocationDirectoryError(
					FormatError::FERR_INVALID_HEADER,
					"relocatable entity range absence proof requires a valid nonempty Node or Edge range");
		}
		static_cast<void>(SuperblockCodec::encode(superblock));
		const PhysicalPageRef rootReference = rootFor(superblock, directoryType);
		if (!rootReference.isValid()) {
			return;
		}
		const uint64_t lastLogicalId = firstLogicalId + static_cast<uint64_t>(count) - 1U;
		const uint64_t firstOrdinal = leafOrdinal(firstLogicalId);
		const uint64_t lastOrdinal = leafOrdinal(lastLogicalId);
		std::set<PhysicalPageRef> visited;
		static_cast<void>(visited.insert(rootReference));
		std::function<void(const CatalogRadixPage &, uint64_t)> visitNode;
		visitNode = [&](const CatalogRadixPage &node, uint64_t ordinalBase) {
			const uint64_t factor = radixFactor(node.level);
			for (uint32_t index = 0U; index < RSS_CATALOG_RADIX_FANOUT; ++index) {
				const PhysicalPageRef childReference = node.children[index];
				if (!childReference.isValid()) {
					continue;
				}
				if (index != 0U && factor > (std::numeric_limits<uint64_t>::max() - ordinalBase) / index) {
					throwLocationDirectoryError(FormatError::FERR_ARITHMETIC_OVERFLOW,
												"relocatable entity range absence proof exceeds the logical-ID domain");
				}
				const uint64_t childOrdinal = ordinalBase + static_cast<uint64_t>(index) * factor;
				const bool endsBeforeRange = childOrdinal < firstOrdinal && factor <= firstOrdinal - childOrdinal;
				if (childOrdinal > lastOrdinal || endsBeforeRange) {
					continue;
				}
				if (!visited.insert(childReference).second) {
					throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
												"relocatable entity range absence proof found a reused metadata page");
				}
				if (node.level != 0U) {
					const CatalogRadixPage child =
							pages_.readMetadataRadix(childReference, BufferPoolReadHint::BPRH_SEQUENTIAL);
					if (child.kind != CatalogRadixPageKind::CRPK_INTERNAL || child.directory_type != directoryType ||
						child.level + 1U != node.level) {
						throwLocationDirectoryError(
								FormatError::FERR_INVALID_HEADER,
								"relocatable entity range absence proof found an invalid radix route");
					}
					validateVisible(child.generation, child.committed_lsn, superblock,
									"relocatable entity range absence proof found a future metadata page");
					visitNode(child, childOrdinal);
					continue;
				}
				const LocationLeafPage leaf =
						pages_.readLocationLeaf(childReference, BufferPoolReadHint::BPRH_SEQUENTIAL);
				if (leaf.directory_type != directoryType) {
					throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
												"relocatable entity range absence proof found a leaf of another type");
				}
				validateRoute(leaf, childOrdinal, superblock);
				const auto found = std::lower_bound(
						leaf.entries.begin(), leaf.entries.end(), firstLogicalId,
						[](const LocationLeafEntry &entry, uint64_t id) { return entry.logical_id < id; });
				if (found != leaf.entries.end() && found->logical_id <= lastLogicalId) {
					throwLocationDirectoryError(
							FormatError::FERR_INVALID_STATE,
							"relocatable entity range absence proof found an existing logical entity ID");
				}
			}
		};
		visitNode(readRoot(pages_, superblock, directoryType), 0U);
	}

	void LocationDirectory::requireNodeRangeAbsent(const Superblock &superblock, uint64_t firstNodeId,
												   uint32_t count) const {
		requireRangeAbsent(superblock, MetadataDirectoryType::MDT_NODE_ENTITY, firstNodeId, count);
	}

	void LocationDirectory::requireEdgeRangeAbsent(const Superblock &superblock, uint64_t firstEdgeId,
												   uint32_t count) const {
		requireRangeAbsent(superblock, MetadataDirectoryType::MDT_EDGE_ENTITY, firstEdgeId, count);
	}

	void LocationDirectory::visit(const Superblock &superblock, MetadataDirectoryType directoryType,
								  const std::function<void(const LocationLeafEntry &)> &visitor) const {
		static_cast<void>(visitUntil(superblock, directoryType, [&visitor](const LocationLeafEntry &entry) {
			visitor(entry);
			return true;
		}));
	}

	bool LocationDirectory::visitUntil(const Superblock &superblock, MetadataDirectoryType directoryType,
									   const std::function<bool(const LocationLeafEntry &)> &visitor) const {
		static_cast<void>(SuperblockCodec::encode(superblock));
		if (!visitor) { // ZYX_COV_EXCL_LINE: public traversal wrappers reject an empty visitor before delegation.
			throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
										"relocatable location directory traversal requires a visitor");
		}
		const PhysicalPageRef rootReference = rootFor(superblock, directoryType);
		if (!rootReference.isValid()) {
			return true;
		}
		std::set<PhysicalPageRef> visited;
		static_cast<void>(visited.insert(rootReference));
		std::function<bool(const CatalogRadixPage &, uint64_t)> visitNode;
		visitNode = [&](const CatalogRadixPage &node, uint64_t ordinalBase) {
			const uint64_t factor = radixFactor(node.level);
			for (uint32_t index = 0; index < RSS_CATALOG_RADIX_FANOUT; ++index) {
				const PhysicalPageRef childReference = node.children[index];
				if (!childReference.isValid()) {
					continue;
				}
				if (index != 0U && factor > (std::numeric_limits<uint64_t>::max() - ordinalBase) /
													index) { // ZYX_COV_EXCL_LINE: validated maximum radix depth keeps
															 // every route representable.
					throwLocationDirectoryError(FormatError::FERR_ARITHMETIC_OVERFLOW,
												"relocatable location radix route exceeds the logical-ID domain");
				}
				const uint64_t childOrdinal = ordinalBase + static_cast<uint64_t>(index) * factor;
				if (!visited.insert(childReference).second) {
					throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
												"relocatable location directory reuses one physical metadata page");
				}
				if (node.level == 0U) {
					const LocationLeafPage leaf =
							pages_.readLocationLeaf(childReference, BufferPoolReadHint::BPRH_SEQUENTIAL);
					if (leaf.directory_type != directoryType) {
						throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
													"relocatable location directory leaf has an unexpected type");
					}
					validateRoute(leaf, childOrdinal, superblock);
					for (const LocationLeafEntry &entry: leaf.entries) {
						if (!visitor(entry)) {
							return false;
						}
					}
					continue;
				}
				const CatalogRadixPage child =
						pages_.readMetadataRadix(childReference, BufferPoolReadHint::BPRH_SEQUENTIAL);
				if (child.kind != CatalogRadixPageKind::CRPK_INTERNAL || child.directory_type != directoryType ||
					child.level + 1U != node.level) {
					throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
												"relocatable location directory has an invalid internal route");
				}
				validateVisible(child.generation, child.committed_lsn, superblock,
								"relocatable location internal page is newer than its Superblock");
				if (!visitNode(child, childOrdinal)) {
					return false;
				}
			}
			return true;
		};
		return visitNode(readRoot(pages_, superblock, directoryType), 0);
	}

	bool LocationDirectory::visitAfter(const Superblock &superblock, MetadataDirectoryType directoryType,
									   uint64_t afterLogicalId, size_t maximumMetadataPages,
									   const std::function<bool(const LocationLeafEntry &)> &visitor,
									   size_t *visitedMetadataPages) const {
		static_cast<void>(SuperblockCodec::encode(superblock));
		if (!visitor || maximumMetadataPages == 0U) {
			throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
										"relocatable resumable location traversal requires a page bound and visitor");
		}
		if (afterLogicalId == std::numeric_limits<uint64_t>::max()) {
			if (visitedMetadataPages != nullptr) {
				*visitedMetadataPages = 0U;
			}
			return true;
		}
		const PhysicalPageRef rootReference = rootFor(superblock, directoryType);
		if (!rootReference.isValid()) {
			if (visitedMetadataPages != nullptr) {
				*visitedMetadataPages = 0U;
			}
			return true;
		}
		const uint64_t firstOrdinal = afterLogicalId == 0U ? 0U : leafOrdinal(afterLogicalId);
		size_t visitedPageCount = 1U; // The root is read before the recursive walk starts.
		std::set<PhysicalPageRef> visited;
		static_cast<void>(visited.insert(rootReference));
		std::function<bool(const CatalogRadixPage &, uint64_t)> visitNode;
		visitNode = [&](const CatalogRadixPage &node, uint64_t ordinalBase) {
			const uint64_t factor = radixFactor(node.level);
			for (uint32_t index = 0; index < RSS_CATALOG_RADIX_FANOUT; ++index) {
				const PhysicalPageRef childReference = node.children[index];
				if (!childReference.isValid()) {
					continue;
				}
				if (index != 0U && factor > (std::numeric_limits<uint64_t>::max() - ordinalBase) / index) {
					throwLocationDirectoryError(
							FormatError::FERR_ARITHMETIC_OVERFLOW,
							"relocatable resumable location traversal exceeds the logical-ID domain");
				}
				const uint64_t childOrdinal = ordinalBase + static_cast<uint64_t>(index) * factor;
				// A child covers factor consecutive leaf ordinals. Skip only ranges
				// that end strictly before the requested continuation ordinal.
				if (childOrdinal < firstOrdinal && factor <= firstOrdinal - childOrdinal) {
					continue;
				}
				if (!visited.insert(childReference).second) {
					throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
												"relocatable location directory reuses one physical metadata page");
				}
				if (visitedPageCount == maximumMetadataPages) {
					throwLocationDirectoryError(
							FormatError::FERR_INVALID_STATE,
							"relocatable resumable location traversal exceeded its metadata-page bound");
				}
				++visitedPageCount;
				if (node.level == 0U) {
					const LocationLeafPage leaf =
							pages_.readLocationLeaf(childReference, BufferPoolReadHint::BPRH_SEQUENTIAL);
					if (leaf.directory_type != directoryType) {
						throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
													"relocatable location directory leaf has an unexpected type");
					}
					validateRoute(leaf, childOrdinal, superblock);
					auto entry = leaf.entries.begin();
					if (childOrdinal == firstOrdinal) {
						entry = std::upper_bound(leaf.entries.begin(), leaf.entries.end(), afterLogicalId,
												 [](uint64_t id, const LocationLeafEntry &candidate) {
													 return id < candidate.logical_id;
												 });
					}
					for (; entry != leaf.entries.end(); ++entry) {
						if (!visitor(*entry)) {
							return false;
						}
					}
					continue;
				}
				const CatalogRadixPage child =
						pages_.readMetadataRadix(childReference, BufferPoolReadHint::BPRH_SEQUENTIAL);
				if (child.kind != CatalogRadixPageKind::CRPK_INTERNAL || child.directory_type != directoryType ||
					child.level + 1U != node.level) {
					throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
												"relocatable location directory has an invalid internal route");
				}
				validateVisible(child.generation, child.committed_lsn, superblock,
								"relocatable location internal page is newer than its Superblock");
				if (!visitNode(child, childOrdinal)) {
					return false;
				}
			}
			return true;
		};
		const bool exhausted = visitNode(readRoot(pages_, superblock, directoryType), 0U);
		if (visitedMetadataPages != nullptr) {
			*visitedMetadataPages = visitedPageCount;
		}
		return exhausted;
	}

	PageLocation LocationDirectory::findPage(const Superblock &superblock, PageId pageId) const {
		const LocationLeafEntry entry = find(superblock, MetadataDirectoryType::MDT_PAGE, pageId.value());
		return {pageId, entry.segment_id, entry.first_page, entry.generation, entry.free_bytes};
	}

	std::optional<PageLocation> LocationDirectory::findPageOptional(const Superblock &superblock, PageId pageId) const {
		const std::optional<LocationLeafEntry> entry =
				findOptional(superblock, MetadataDirectoryType::MDT_PAGE, pageId.value());
		if (!entry.has_value()) {
			return std::nullopt;
		}
		return PageLocation{pageId, entry->segment_id, entry->first_page, entry->generation, entry->free_bytes};
	}

	ExtentLocation LocationDirectory::findExtent(const Superblock &superblock, ExtentId extentId) const {
		const LocationLeafEntry entry = find(superblock, MetadataDirectoryType::MDT_EXTENT, extentId.value());
		return {extentId, entry.segment_id, entry.first_page, entry.page_count, entry.generation};
	}

	std::optional<ExtentLocation> LocationDirectory::findExtentOptional(const Superblock &superblock,
																		ExtentId extentId) const {
		const std::optional<LocationLeafEntry> entry =
				findOptional(superblock, MetadataDirectoryType::MDT_EXTENT, extentId.value());
		if (!entry.has_value()) {
			return std::nullopt;
		}
		return ExtentLocation{extentId, entry->segment_id, entry->first_page, entry->page_count, entry->generation};
	}

	EntityPageLocation LocationDirectory::findNodePageLocation(const Superblock &superblock, uint64_t nodeId) const {
		const LocationLeafEntry entry = find(superblock, MetadataDirectoryType::MDT_NODE_ENTITY, nodeId);
		return {nodeId, PageId(entry.segment_id.value()), entry.first_page,
				static_cast<uint32_t>(entry.generation.value())};
	}

	EntityPageLocation LocationDirectory::findEdgePageLocation(const Superblock &superblock, uint64_t edgeId) const {
		const LocationLeafEntry entry = find(superblock, MetadataDirectoryType::MDT_EDGE_ENTITY, edgeId);
		return {edgeId, PageId(entry.segment_id.value()), entry.first_page,
				static_cast<uint32_t>(entry.generation.value())};
	}

	std::optional<EntityPageLocation> LocationDirectory::findNodePageLocationOptional(const Superblock &superblock,
																					  uint64_t nodeId) const {
		const std::optional<LocationLeafEntry> entry =
				findOptional(superblock, MetadataDirectoryType::MDT_NODE_ENTITY, nodeId);
		if (!entry.has_value()) {
			return std::nullopt;
		}
		return EntityPageLocation{nodeId, PageId(entry->segment_id.value()), entry->first_page,
								  static_cast<uint32_t>(entry->generation.value())};
	}

	std::optional<EntityPageLocation> LocationDirectory::findEdgePageLocationOptional(const Superblock &superblock,
																					  uint64_t edgeId) const {
		const std::optional<LocationLeafEntry> entry =
				findOptional(superblock, MetadataDirectoryType::MDT_EDGE_ENTITY, edgeId);
		if (!entry.has_value()) {
			return std::nullopt;
		}
		return EntityPageLocation{edgeId, PageId(entry->segment_id.value()), entry->first_page,
								  static_cast<uint32_t>(entry->generation.value())};
	}

	EntityPropertyLocation LocationDirectory::findEntityProperty(const Superblock &superblock,
																 const EntityPropertyKey &key) const {
		const LocationLeafEntry entry = find(superblock, MetadataDirectoryType::MDT_ENTITY_PROPERTY, key.directoryId());
		return {key, PageId(entry.segment_id.value()), entry.first_page,
				static_cast<uint32_t>(entry.generation.value())};
	}

	std::optional<EntityPropertyLocation>
	LocationDirectory::findEntityPropertyOptional(const Superblock &superblock, const EntityPropertyKey &key) const {
		const std::optional<LocationLeafEntry> entry =
				findOptional(superblock, MetadataDirectoryType::MDT_ENTITY_PROPERTY, key.directoryId());
		if (!entry.has_value()) {
			return std::nullopt;
		}
		return EntityPropertyLocation{key, PageId(entry->segment_id.value()), entry->first_page,
									  static_cast<uint32_t>(entry->generation.value())};
	}

	StateLocation LocationDirectory::findState(const Superblock &superblock, uint64_t stateId) const {
		const LocationLeafEntry entry = find(superblock, MetadataDirectoryType::MDT_STATE, stateId);
		return {stateId, PageId(entry.segment_id.value()), entry.first_page,
				static_cast<uint32_t>(entry.generation.value())};
	}

	std::optional<StateLocation> LocationDirectory::findStateOptional(const Superblock &superblock,
																	  uint64_t stateId) const {
		const std::optional<LocationLeafEntry> entry =
				findOptional(superblock, MetadataDirectoryType::MDT_STATE, stateId);
		if (!entry.has_value()) {
			return std::nullopt;
		}
		return StateLocation{stateId, PageId(entry->segment_id.value()), entry->first_page,
							 static_cast<uint32_t>(entry->generation.value())};
	}

	IndexRootLocation LocationDirectory::findIndexRoot(const Superblock &superblock, uint64_t indexId) const {
		const LocationLeafEntry entry = find(superblock, MetadataDirectoryType::MDT_INDEX_ROOT, indexId);
		return {indexId, PageId(entry.segment_id.value()), entry.generation};
	}

	std::optional<IndexRootLocation> LocationDirectory::findIndexRootOptional(const Superblock &superblock,
																			  uint64_t indexId) const {
		const std::optional<LocationLeafEntry> entry =
				findOptional(superblock, MetadataDirectoryType::MDT_INDEX_ROOT, indexId);
		if (!entry.has_value()) {
			return std::nullopt;
		}
		return IndexRootLocation{indexId, PageId(entry->segment_id.value()), entry->generation};
	}

	VectorLocation LocationDirectory::findVectorLocation(const Superblock &superblock, uint64_t vectorId) const {
		const LocationLeafEntry entry = find(superblock, MetadataDirectoryType::MDT_VECTOR_LOCATION, vectorId);
		return {vectorId, PageId(entry.segment_id.value()), entry.first_page,
				static_cast<uint32_t>(entry.generation.value())};
	}

	std::optional<VectorLocation> LocationDirectory::findVectorLocationOptional(const Superblock &superblock,
																				uint64_t vectorId) const {
		const std::optional<LocationLeafEntry> entry =
				findOptional(superblock, MetadataDirectoryType::MDT_VECTOR_LOCATION, vectorId);
		if (!entry.has_value()) {
			return std::nullopt;
		}
		return VectorLocation{vectorId, PageId(entry->segment_id.value()), entry->first_page,
							  static_cast<uint32_t>(entry->generation.value())};
	}

	VectorManifestLocation LocationDirectory::findVectorManifest(const Superblock &superblock, uint64_t indexId) const {
		const LocationLeafEntry entry = find(superblock, MetadataDirectoryType::MDT_VECTOR_MANIFEST, indexId);
		return {indexId, PageId(entry.segment_id.value()), entry.generation};
	}

	std::optional<VectorManifestLocation> LocationDirectory::findVectorManifestOptional(const Superblock &superblock,
																						uint64_t indexId) const {
		const std::optional<LocationLeafEntry> entry =
				findOptional(superblock, MetadataDirectoryType::MDT_VECTOR_MANIFEST, indexId);
		if (!entry.has_value()) {
			return std::nullopt;
		}
		return VectorManifestLocation{indexId, PageId(entry->segment_id.value()), entry->generation};
	}

	LargeValueManifestLocation LocationDirectory::findLargeValueManifest(const Superblock &superblock,
																		 LargeValueId valueId) const {
		const LocationLeafEntry entry =
				find(superblock, MetadataDirectoryType::MDT_LARGE_VALUE_MANIFEST, valueId.value());
		return {valueId, PageId(entry.segment_id.value()), entry.generation};
	}

	std::optional<LargeValueManifestLocation>
	LocationDirectory::findLargeValueManifestOptional(const Superblock &superblock, LargeValueId valueId) const {
		if (!valueId.isValid()) {
			throwLocationDirectoryError(FormatError::FERR_INVALID_STATE,
										"relocatable LargeValue manifest lookup requires an ID");
		}
		const std::optional<LocationLeafEntry> entry =
				findOptional(superblock, MetadataDirectoryType::MDT_LARGE_VALUE_MANIFEST, valueId.value());
		if (!entry.has_value()) {
			return std::nullopt;
		}
		return LargeValueManifestLocation{valueId, PageId(entry->segment_id.value()), entry->generation};
	}

	std::optional<RetireQueueEntry> LocationDirectory::findRetireQueueEntryOptional(const Superblock &superblock,
																					PhysicalSlotId slot) const {
		if (!slot.isValid()) {
			throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
										"relocatable Retire Queue lookup requires a valid physical slot");
		}
		const std::optional<LocationLeafEntry> entry =
				findOptional(superblock, MetadataDirectoryType::MDT_RETIRE_QUEUE, slot.value());
		if (!entry.has_value()) {
			return std::nullopt;
		}
		return RetireQueueEntry{slot, entry->segment_id, entry->generation,
								static_cast<RetireQueueDisposition>(entry->first_page)};
	}

	void LocationDirectory::visitPages(const Superblock &superblock,
									   const std::function<void(const PageLocation &)> &visitor) const {
		if (!visitor) {
			throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
										"relocatable Page directory traversal requires a visitor");
		}
		visit(superblock, MetadataDirectoryType::MDT_PAGE, [&visitor](const LocationLeafEntry &entry) {
			visitor({PageId(entry.logical_id), entry.segment_id, entry.first_page, entry.generation, entry.free_bytes});
		});
	}

	bool LocationDirectory::visitPagesUntil(const Superblock &superblock,
											const std::function<bool(const PageLocation &)> &visitor) const {
		if (!visitor) {
			throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
										"relocatable Page directory traversal requires a visitor");
		}
		return visitUntil(superblock, MetadataDirectoryType::MDT_PAGE, [&visitor](const LocationLeafEntry &entry) {
			return visitor(
					{PageId(entry.logical_id), entry.segment_id, entry.first_page, entry.generation, entry.free_bytes});
		});
	}

	void
	LocationDirectory::visitNodePageLocations(const Superblock &superblock,
											  const std::function<void(const EntityPageLocation &)> &visitor) const {
		if (!visitor) {
			throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
										"relocatable Entity Map Node traversal requires a visitor");
		}
		visit(superblock, MetadataDirectoryType::MDT_NODE_ENTITY, [&visitor](const LocationLeafEntry &entry) {
			visitor({entry.logical_id, PageId(entry.segment_id.value()), entry.first_page,
					 static_cast<uint32_t>(entry.generation.value())});
		});
	}

	void
	LocationDirectory::visitEdgePageLocations(const Superblock &superblock,
											  const std::function<void(const EntityPageLocation &)> &visitor) const {
		if (!visitor) {
			throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
										"relocatable Entity Map Edge traversal requires a visitor");
		}
		visit(superblock, MetadataDirectoryType::MDT_EDGE_ENTITY, [&visitor](const LocationLeafEntry &entry) {
			visitor({entry.logical_id, PageId(entry.segment_id.value()), entry.first_page,
					 static_cast<uint32_t>(entry.generation.value())});
		});
	}

	bool LocationDirectory::visitNodePageLocationsUntil(
			const Superblock &superblock, const std::function<bool(const EntityPageLocation &)> &visitor) const {
		if (!visitor) {
			throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
										"relocatable Entity Map Node traversal requires a visitor");
		}
		return visitUntil(superblock, MetadataDirectoryType::MDT_NODE_ENTITY,
						  [&visitor](const LocationLeafEntry &entry) {
							  return visitor({entry.logical_id, PageId(entry.segment_id.value()), entry.first_page,
														 static_cast<uint32_t>(entry.generation.value())});
						  });
	}

	bool LocationDirectory::visitEdgePageLocationsUntil(
			const Superblock &superblock, const std::function<bool(const EntityPageLocation &)> &visitor) const {
		if (!visitor) {
			throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
										"relocatable Entity Map Edge traversal requires a visitor");
		}
		return visitUntil(superblock, MetadataDirectoryType::MDT_EDGE_ENTITY,
						  [&visitor](const LocationLeafEntry &entry) {
							  return visitor({entry.logical_id, PageId(entry.segment_id.value()), entry.first_page,
														 static_cast<uint32_t>(entry.generation.value())});
						  });
	}

	bool LocationDirectory::visitNodePageLocationsAfter(
			const Superblock &superblock, uint64_t afterNodeId, size_t maximumMetadataPages,
			const std::function<bool(const EntityPageLocation &)> &visitor, size_t *visitedMetadataPages) const {
		if (!visitor || maximumMetadataPages == 0U) {
			throwLocationDirectoryError(
					FormatError::FERR_INVALID_HEADER,
					"relocatable resumable Entity Map Node traversal requires a page bound and visitor");
		}
		return visitAfter(
				superblock, MetadataDirectoryType::MDT_NODE_ENTITY, afterNodeId, maximumMetadataPages,
				[&visitor](const LocationLeafEntry &entry) {
					return visitor({entry.logical_id, PageId(entry.segment_id.value()), entry.first_page,
												static_cast<uint32_t>(entry.generation.value())});
				},
				visitedMetadataPages);
	}

	void
	LocationDirectory::visitEntityProperties(const Superblock &superblock,
											 const std::function<void(const EntityPropertyLocation &)> &visitor) const {
		if (!visitor) {
			throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
										"relocatable Entity Property directory traversal requires a visitor");
		}
		visit(superblock, MetadataDirectoryType::MDT_ENTITY_PROPERTY, [&visitor](const LocationLeafEntry &entry) {
			visitor({EntityPropertyKey::fromDirectoryId(entry.logical_id), PageId(entry.segment_id.value()),
					 entry.first_page, static_cast<uint32_t>(entry.generation.value())});
		});
	}

	void LocationDirectory::visitStates(const Superblock &superblock,
										const std::function<void(const StateLocation &)> &visitor) const {
		if (!visitor) {
			throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
										"relocatable State directory traversal requires a visitor");
		}
		visit(superblock, MetadataDirectoryType::MDT_STATE, [&visitor](const LocationLeafEntry &entry) {
			visitor({entry.logical_id, PageId(entry.segment_id.value()), entry.first_page,
					 static_cast<uint32_t>(entry.generation.value())});
		});
	}

	void LocationDirectory::visitExtents(const Superblock &superblock,
										 const std::function<void(const ExtentLocation &)> &visitor) const {
		if (!visitor) {
			throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
										"relocatable Extent directory traversal requires a visitor");
		}
		visit(superblock, MetadataDirectoryType::MDT_EXTENT, [&visitor](const LocationLeafEntry &entry) {
			visitor({ExtentId(entry.logical_id), entry.segment_id, entry.first_page, entry.page_count,
					 entry.generation});
		});
	}

	void LocationDirectory::visitIndexRoots(const Superblock &superblock,
											const std::function<void(const IndexRootLocation &)> &visitor) const {
		if (!visitor) {
			throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
										"relocatable Index Root directory traversal requires a visitor");
		}
		visit(superblock, MetadataDirectoryType::MDT_INDEX_ROOT, [&visitor](const LocationLeafEntry &entry) {
			visitor({entry.logical_id, PageId(entry.segment_id.value()), entry.generation});
		});
	}

	void LocationDirectory::visitVectorLocations(const Superblock &superblock,
												 const std::function<void(const VectorLocation &)> &visitor) const {
		if (!visitor) {
			throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
										"relocatable Vector Location directory traversal requires a visitor");
		}
		visit(superblock, MetadataDirectoryType::MDT_VECTOR_LOCATION, [&visitor](const LocationLeafEntry &entry) {
			visitor({entry.logical_id, PageId(entry.segment_id.value()), entry.first_page,
					 static_cast<uint32_t>(entry.generation.value())});
		});
	}

	void
	LocationDirectory::visitVectorManifests(const Superblock &superblock,
											const std::function<void(const VectorManifestLocation &)> &visitor) const {
		if (!visitor) {
			throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
										"relocatable Vector Manifest directory traversal requires a visitor");
		}
		visit(superblock, MetadataDirectoryType::MDT_VECTOR_MANIFEST, [&visitor](const LocationLeafEntry &entry) {
			visitor({entry.logical_id, PageId(entry.segment_id.value()), entry.generation});
		});
	}

	bool LocationDirectory::visitVectorManifestsAfter(
			const Superblock &superblock, uint64_t afterIndexId, size_t maximumMetadataPages,
			const std::function<bool(const VectorManifestLocation &)> &visitor, size_t *visitedMetadataPages) const {
		if (!visitor || maximumMetadataPages == 0U) {
			throwLocationDirectoryError(
					FormatError::FERR_INVALID_HEADER,
					"relocatable resumable Vector Manifest traversal requires a page bound and visitor");
		}
		return visitAfter(
				superblock, MetadataDirectoryType::MDT_VECTOR_MANIFEST, afterIndexId, maximumMetadataPages,
				[&visitor](const LocationLeafEntry &entry) {
					return visitor({entry.logical_id, PageId(entry.segment_id.value()), entry.generation});
				},
				visitedMetadataPages);
	}

	void LocationDirectory::visitLargeValueManifests(
			const Superblock &superblock,
			const std::function<void(const LargeValueManifestLocation &)> &visitor) const {
		if (!visitor) {
			throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
										"relocatable LargeValue Manifest directory traversal requires a visitor");
		}
		visit(superblock, MetadataDirectoryType::MDT_LARGE_VALUE_MANIFEST, [&visitor](const LocationLeafEntry &entry) {
			visitor({LargeValueId(entry.logical_id), PageId(entry.segment_id.value()), entry.generation});
		});
	}

	void LocationDirectory::visitRetireQueue(const Superblock &superblock,
											 const std::function<void(const RetireQueueEntry &)> &visitor) const {
		if (!visitor) {
			throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
										"relocatable Retire Queue traversal requires a visitor");
		}
		visit(superblock, MetadataDirectoryType::MDT_RETIRE_QUEUE, [&visitor](const LocationLeafEntry &entry) {
			visitor({PhysicalSlotId(entry.logical_id), entry.segment_id, entry.generation,
					 static_cast<RetireQueueDisposition>(entry.first_page)});
		});
	}

} // namespace graph::storage
