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
#include <bitset>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>

#include "graph/storage/relocatable/allocation/SharedPagePool.hpp"
#include "graph/storage/relocatable/metadata/MetadataPageFile.hpp"
#include "graph/storage/relocatable/metadata/MetadataPageReadContext.hpp"
#include "graph/utils/OwnedPmrObject.hpp"
#include <memory_resource>
#include "graph/storage/relocatable/metadata/MetadataPageRouteDirectory.hpp"
#include "graph/storage/relocatable/metadata/MetadataWriteBatch.hpp"
#include "graph/debug/PerfTrace.hpp"
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
				   type == MetadataDirectoryType::MDT_ENTITY_PROPERTY || type == MetadataDirectoryType::MDT_STATE ||
				   type == MetadataDirectoryType::MDT_INDEX_ROOT ||
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
										 !isVectorManifest && !isLargeValueManifest && !isRetireQueue &&
										 entry.first_page < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
					static_cast<uint8_t>(entry.page_count == 0U) |
					static_cast<uint8_t>(!isEntity && !isProperty && !isState && !isIndexRoot && !isVectorLocation &&
										 !isVectorManifest && !isLargeValueManifest && !isRetireQueue &&
										 entry.first_page > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
					static_cast<uint8_t>(!isEntity && !isProperty && !isState && !isIndexRoot && !isVectorLocation &&
										 !isVectorManifest && !isLargeValueManifest && !isRetireQueue &&
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

        void validateDecodedRoute(const DecodedMetadataPageView<LocationLeafPage> &page, uint64_t ordinal,
                                  const Superblock &superblock) {
            validateVisible(page->generation, page->committed_lsn, superblock,
                "relocatable location leaf is newer than its Superblock");
            const auto &entries = page->entries;
            // Codec-validated strict order and monotonic leafOrdinal make this
            // equivalent to checking every entry. Mutable COW pages still use
            // the full validateRoute scan before they are written.
            if (!entries.empty() && (leafOrdinal(entries.front().logical_id) != ordinal ||
                                     leafOrdinal(entries.back().logical_id) != ordinal)) {
                throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
                    "relocatable location leaf does not match its radix route");
            }
        }

		[[nodiscard]] PhysicalPageRef physicalRootFor(const Superblock &superblock,
													  MetadataDirectoryType directoryType) {
			switch (directoryType) {
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

		[[nodiscard]] MetadataDirectoryRootOrdinal logicalRootOrdinal(MetadataDirectoryType directoryType) {
			switch (directoryType) {
				case MetadataDirectoryType::MDT_EXTENT:
					return MetadataDirectoryRootOrdinal::MDRO_EXTENT;
				case MetadataDirectoryType::MDT_NODE_ENTITY:
					return MetadataDirectoryRootOrdinal::MDRO_NODE_ENTITY;
				case MetadataDirectoryType::MDT_EDGE_ENTITY:
					return MetadataDirectoryRootOrdinal::MDRO_EDGE_ENTITY;
				case MetadataDirectoryType::MDT_ENTITY_PROPERTY:
					return MetadataDirectoryRootOrdinal::MDRO_ENTITY_PROPERTY;
				case MetadataDirectoryType::MDT_STATE:
					return MetadataDirectoryRootOrdinal::MDRO_STATE;
				case MetadataDirectoryType::MDT_INDEX_ROOT:
					return MetadataDirectoryRootOrdinal::MDRO_INDEX_ROOT;
				case MetadataDirectoryType::MDT_VECTOR_LOCATION:
					return MetadataDirectoryRootOrdinal::MDRO_VECTOR_LOCATION;
				case MetadataDirectoryType::MDT_VECTOR_MANIFEST:
					return MetadataDirectoryRootOrdinal::MDRO_VECTOR_MANIFEST;
				case MetadataDirectoryType::MDT_LARGE_VALUE_MANIFEST:
					return MetadataDirectoryRootOrdinal::MDRO_LARGE_VALUE_MANIFEST;
				case MetadataDirectoryType::MDT_RETIRE_QUEUE:
					return MetadataDirectoryRootOrdinal::MDRO_RETIRE_QUEUE;
				default: // ZYX_COV_EXCL_LINE: Page Directory stays physical bootstrap plumbing.
					throwLocationDirectoryError(FormatError::FERR_INVALID_ENUM,
												"relocatable location directory type has no logical bootstrap root");
			}
		}

		void setPhysicalRoot(MetadataWriteBatch &batch, MetadataDirectoryType directoryType, PhysicalPageRef root) {
			switch (directoryType) {
				case MetadataDirectoryType::MDT_EXTENT:
					batch.setExtentDirectoryRoot(root);
					return;
				case MetadataDirectoryType::MDT_NODE_ENTITY:
					batch.setNodeEntityDirectoryRoot(root);
					return;
				case MetadataDirectoryType::MDT_EDGE_ENTITY:
					batch.setEdgeEntityDirectoryRoot(root);
					return;
				case MetadataDirectoryType::MDT_ENTITY_PROPERTY:
					batch.setEntityPropertyDirectoryRoot(root);
					return;
				case MetadataDirectoryType::MDT_STATE:
					batch.setStateDirectoryRoot(root);
					return;
				case MetadataDirectoryType::MDT_INDEX_ROOT:
					batch.setIndexRootDirectoryRoot(root);
					return;
				case MetadataDirectoryType::MDT_VECTOR_LOCATION:
					batch.setVectorLocationDirectoryRoot(root);
					return;
				case MetadataDirectoryType::MDT_VECTOR_MANIFEST:
					batch.setVectorManifestDirectoryRoot(root);
					return;
				case MetadataDirectoryType::MDT_LARGE_VALUE_MANIFEST:
					batch.setLargeValueManifestDirectoryRoot(root);
					return;
				case MetadataDirectoryType::MDT_RETIRE_QUEUE:
					batch.setRetireQueueRoot(root);
					return;
				default:
					throwLocationDirectoryError(
							FormatError::FERR_INVALID_ENUM,
							"relocatable location directory has no logical-root publication target");
			}
		}

		[[nodiscard]] PhysicalPageRef rootFor(MetadataPageFile &pages, const Superblock &superblock,
													  MetadataDirectoryType directoryType,
													  const MetadataWriteBatch *batch = nullptr) {
			if (directoryType == MetadataDirectoryType::MDT_PAGE) {
				if (batch != nullptr) {
					if (const std::optional<PhysicalPageRef> pending = batch->pendingBootstrapDataRoot();
						pending.has_value()) {
						return *pending;
					}
				}
				if (!superblock.usesPageDirectoryBootstrap() || !superblock.page_directory_root.isValid()) {
					return superblock.page_directory_root;
				}
				const auto bootstrapView = pages.readPageDirectoryBootstrapView(superblock.page_directory_root);
            const auto &bootstrap = *bootstrapView;
				validateVisible(bootstrap.generation, bootstrap.committed_lsn, superblock,
								"Page Directory bootstrap is newer than its Superblock");
				return bootstrap.data_page_directory_root;
			}

			// A typed directory may be prepared more than once in one metadata
			// batch.  Once its candidate root is owned by that batch, it is the
			// authoritative COW source even before the final Superblock switch.
			// Without this check a second mutation would restart from the published
			// root and silently discard the first mutation.
			if (batch != nullptr) {
				const PhysicalPageRef candidateRoot = physicalRootFor(batch->candidate(), directoryType);
				if (candidateRoot.isValid() && batch->ownsAllocatedPage(candidateRoot)) {
					return candidateRoot;
				}
			}

			const PhysicalPageRef physicalRoot = physicalRootFor(superblock, directoryType);
			if (!superblock.usesPageDirectoryBootstrap()) {
				return physicalRoot;
			}
			if (!superblock.page_directory_root.isValid()) {
				throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
											"relocatable location bootstrap root is absent");
			}
			const auto bootstrapView = pages.readPageDirectoryBootstrapView(superblock.page_directory_root);
            const auto &bootstrap = *bootstrapView;
			validateVisible(bootstrap.generation, bootstrap.committed_lsn, superblock,
							"Page Directory bootstrap is newer than its Superblock");
			// A COW batch retains the prior header until its final route/header
			// publication. Its candidate physical root is authoritative meanwhile.
			if (bootstrap.generation != superblock.generation) {
				return physicalRoot;
			}
			const MetadataPageId rootId =
					bootstrap.metadata_directory_roots[static_cast<size_t>(logicalRootOrdinal(directoryType))];
			if (!rootId.isValid()) {
				return {};
			}
			return MetadataPageRouteDirectory(pages).find(superblock, bootstrap.metadata_page_directory_root, rootId);
		}

		struct MutableLeaf final {
			LocationLeafPage page;
		};

		struct MutableNode final {
			MutableNode() = default;
			MutableNode(const MutableNode &) = delete;
			MutableNode &operator=(const MutableNode &) = delete;
			MutableNode(MutableNode &&) noexcept = default;
			MutableNode &operator=(MutableNode &&) noexcept = default;
			CatalogRadixPage page;
			std::map<uint32_t, std::unique_ptr<MutableNode>> node_children;
			std::map<uint32_t, std::unique_ptr<MutableLeaf>> leaf_children;
		};

		struct PreparedFrameCursor final {
			std::span<const PhysicalPageRef> frames;
			size_t next = 0U;

			[[nodiscard]] PhysicalPageRef take() {
				if (next >= frames.size()) {
					throwLocationDirectoryError(FormatError::FERR_INVALID_STATE,
											"relocatable location tree exhausted its prepared metadata frames");
				}
				return frames[next++];
			}
		};

		class PreparedPageWriter final {
		public:
			static constexpr size_t MAX_BATCH_PAGES = 128U;

			PreparedPageWriter(MetadataPageFile &pages, MetadataWriteBatch &batch) : pageFile_(pages), batch_(batch) {}

			PreparedPageWriter(const PreparedPageWriter &) = delete;
			PreparedPageWriter &operator=(const PreparedPageWriter &) = delete;

			void enqueue(PhysicalPageRef reference, const std::array<std::byte, RSS_PAGE_SIZE> &bytes) {
				if (!reference.isValid() || reference.generation != batch_.generation()) {
					throwLocationDirectoryError(FormatError::FERR_INVALID_STATE,
											"relocatable location tree prepared an invalid metadata page reference");
				}
				pending_pages_.push_back({reference, bytes});
				if (pending_pages_.size() >= MAX_BATCH_PAGES) {
					flush();
				}
			}

			void flush() {
				size_t offset = 0U;
				while (offset < pending_pages_.size()) {
					const PhysicalPageRef first = pending_pages_[offset].reference;
					size_t count = 1U;
					while (offset + count < pending_pages_.size() && count < MAX_BATCH_PAGES) {
						const PhysicalPageRef previous = pending_pages_[offset + count - 1U].reference;
						const PhysicalPageRef next = pending_pages_[offset + count].reference;
						if (next.generation != first.generation || next.physical_slot != first.physical_slot ||
							next.frame_index != static_cast<uint8_t>(previous.frame_index + 1U)) {
							break;
						}
						++count;
					}
					std::vector<std::byte> buffer(count * RSS_PAGE_SIZE);
					for (size_t index = 0U; index < count; ++index) {
						std::memcpy(buffer.data() + index * RSS_PAGE_SIZE, pending_pages_[offset + index].bytes.data(), RSS_PAGE_SIZE);
					}
					pageFile_.storageIO()->writePages({pageFile_.pageOffset(first), first.generation.value()}, buffer.data(),
												 buffer.size());
					offset += count;
				}
				pending_pages_.clear();
			}

		private:
			struct PendingPage final {
				PhysicalPageRef reference;
				std::array<std::byte, RSS_PAGE_SIZE> bytes{};
			};

			MetadataPageFile &pageFile_;
			MetadataWriteBatch &batch_;
			std::vector<PendingPage> pending_pages_;
		};

		[[nodiscard]] size_t writePageCount(const MutableNode &node) {
			size_t count = 1U;
			if (node.page.level == 0U) {
				if (node.leaf_children.size() > std::numeric_limits<size_t>::max() - count) {
					throwLocationDirectoryError(FormatError::FERR_ARITHMETIC_OVERFLOW,
											"relocatable location tree write-page count overflows");
				}
				return count + node.leaf_children.size();
			}
			for (const auto &[index, child]: node.node_children) {
				static_cast<void>(index);
				const size_t childCount = writePageCount(*child);
				if (childCount > std::numeric_limits<size_t>::max() - count) {
					throwLocationDirectoryError(FormatError::FERR_ARITHMETIC_OVERFLOW,
											"relocatable location tree write-page count overflows");
				}
				count += childCount;
			}
			return count;
		}

		[[nodiscard]] PhysicalPageRef takeWriteFrame(MetadataWriteBatch &batch, PreparedFrameCursor *cursor) {
			return cursor == nullptr ? batch.allocatePage() : cursor->take();
		}

		[[nodiscard]] std::unique_ptr<MutableNode> makeNode(MetadataDirectoryType type, CatalogRadixPageKind kind,
															uint16_t level, Generation generation,
															LogSequenceNumber lsn, MetadataWriteBatch &batch) {
			auto node = std::make_unique<MutableNode>();
			node->page.kind = kind;
			node->page.directory_type = type;
			node->page.level = level;
			node->page.page_id = batch.allocateMetadataPageId();
			node->page.generation = generation;
			node->page.committed_lsn = lsn;
			return node;
		}

		[[nodiscard]] std::unique_ptr<MutableLeaf> makeLeaf(MetadataDirectoryType type, Generation generation,
															LogSequenceNumber lsn, MetadataWriteBatch &batch) {
			auto leaf = std::make_unique<MutableLeaf>();
			leaf->page.directory_type = type;
			leaf->page.page_id = batch.allocateMetadataPageId();
			leaf->page.generation = generation;
			leaf->page.committed_lsn = lsn;
			return leaf;
		}

		void validateBatchVisible(PhysicalPageRef reference, Generation generation, LogSequenceNumber lsn,
								  const MetadataWriteBatch &batch) {
			if (batch.ownsAllocatedPage(reference)) {
				if (generation != batch.generation() || lsn != batch.checkpointLsn()) {
					throwLocationDirectoryError(
							FormatError::FERR_INVALID_STATE,
							"relocatable location directory batch frame has an invalid visibility fence");
				}
				return;
			}
			validateVisible(generation, lsn, batch.current(),
							"relocatable location directory references a future published page");
		}

		[[nodiscard]] MetadataRadixPage readRadixForMutation(MetadataPageFile &pages, PhysicalPageRef reference,
															 const MetadataWriteBatch &batch, bool resolveAll = true) {
			const MetadataRadixPage unresolved = pages.readMetadataRadixUnresolved(reference);
			if (!unresolved.logical_children_enabled) {
				return pages.readMetadataRadix(reference, batch.candidate());
			}
			if (!resolveAll) return unresolved;
			MetadataRadixPage page = unresolved;
			const PageDirectoryBootstrap currentBootstrap =
					pages.readPageDirectoryBootstrap(batch.current().page_directory_root);
			for (size_t index = 0U; index < page.logical_children.size(); ++index) {
				const MetadataPageId childId = page.logical_children[index];
				if (!childId.isValid()) {
					continue;
				}
				const auto pending = batch.pendingMetadataPageRoute(childId);
				page.children[index] =
						pending.has_value()
								? *pending
								: MetadataPageRouteDirectory(pages).find(
										  batch.current(), currentBootstrap.metadata_page_directory_root, childId);
			}
			page.logical_children_resolved = true;
			return page;
		}


		void resolveMutationChild(MetadataRadixPage &page, uint32_t index,
				const Superblock &superblock, MetadataPageFile &pages, const MetadataWriteBatch *batch) {
			if (!page.logical_children_enabled || page.children[index].isValid() ||
				!page.logical_children[index].isValid()) return;
			const auto id = page.logical_children[index];
			if (batch) {
				const auto pending = batch->pendingMetadataPageRoute(id);
				if (pending) page.children[index] = *pending;
				else {
					const auto bootstrap = pages.readPageDirectoryBootstrap(batch->current().page_directory_root);
					page.children[index] = MetadataPageRouteDirectory(pages).find(
							batch->current(), bootstrap.metadata_page_directory_root, id);
				}
			} else {
				pages.resolveMetadataRadixChildren(page, superblock, std::span(&index, 1U),
						std::span(&page.children[index], 1U));
			}
			debug::PerfTrace::addValue("native_graph.directory.mutation_child_route_resolutions", 1);
		}

		[[nodiscard]] std::unique_ptr<MutableNode> loadNode(MetadataPageFile &pages, PhysicalPageRef reference,
															uint16_t level, MetadataDirectoryType type,
															const Superblock &superblock,
															const MetadataWriteBatch *batch = nullptr) {
			CatalogRadixPage page = batch != nullptr ? readRadixForMutation(pages, reference, *batch, false)
													 : pages.readMetadataRadix(reference, superblock);
			if (page.kind != CatalogRadixPageKind::CRPK_INTERNAL || page.directory_type != type ||
				page.level != level) {
				throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
											"relocatable location directory has an invalid internal radix page");
			}
			if (batch != nullptr) {
				validateBatchVisible(reference, page.generation, page.committed_lsn, *batch);
			} else {
				validateVisible(page.generation, page.committed_lsn, superblock,
								"relocatable location internal page is newer than its Superblock");
			}
			auto node = std::make_unique<MutableNode>();
			node->page = std::move(page);
			return node;
		}

		[[nodiscard]] std::unique_ptr<MutableLeaf> loadLeaf(MetadataPageFile &pages, PhysicalPageRef reference,
															uint64_t ordinal, MetadataDirectoryType type,
															const Superblock &superblock,
															const MetadataWriteBatch *batch = nullptr) {
			LocationLeafPage page = pages.readLocationLeaf(reference);
			if (page.directory_type != type) {
				throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
											"relocatable location leaf has an unexpected directory type");
			}
			if (batch != nullptr) {
				validateBatchVisible(reference, page.generation, page.committed_lsn, *batch);
				for (const LocationLeafEntry &entry: page.entries) {
					if (leafOrdinal(entry.logical_id) != ordinal) {
						throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
													"relocatable location leaf does not match its radix route");
					}
				}
			} else {
				validateRoute(page, ordinal, superblock);
			}
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
			resolveMutationChild(node.page, index, superblock, pages, &batch);
			if (node.page.level == 0U) {
				auto iterator = node.leaf_children.find(index);
				if (iterator == node.leaf_children.end()) {
					std::unique_ptr<MutableLeaf> leaf =
							node.page.children[index].isValid()
									? loadLeaf(pages, node.page.children[index], ordinal, type, superblock, &batch)
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
								? loadNode(pages, node.page.children[index], childLevel, type, superblock, &batch)
								: makeNode(type, CatalogRadixPageKind::CRPK_INTERNAL, childLevel, generation, lsn,
										   batch);
				iterator = node.node_children.emplace(index, std::move(child)).first;
			}
			applyMutation(*iterator->second, entry, type, generation, lsn, superblock, pages, batch);
		}

		void applyErase(MutableNode &node, uint64_t logicalId, MetadataDirectoryType type, Generation generation,
						LogSequenceNumber lsn, const Superblock &superblock, MetadataPageFile &pages,
						const MetadataWriteBatch *batch = nullptr) {
			node.page.generation = generation;
			node.page.committed_lsn = lsn;
			const uint64_t ordinal = leafOrdinal(logicalId);
			const uint32_t index = CatalogRadixPageCodec::childIndexForLeafOrdinal(ordinal, node.page.level);
			resolveMutationChild(node.page, index, superblock, pages, batch);
			if (node.page.level != 0U) {
				auto iterator = node.node_children.find(index);
				if (iterator == node.node_children.end()) {
					// Root growth and mutations can retain a child only in the
					// mutable tree until publication writes its physical reference.
					if (!node.page.children[index].isValid()) {
						throwLocationDirectoryError(FormatError::FERR_INVALID_STATE,
								"relocatable location directory deletion references an absent radix route");
					}
					const uint16_t childLevel = node.page.level - 1U;
					iterator = node.node_children
									   .emplace(index, loadNode(pages, node.page.children[index], childLevel, type,
																superblock, batch))
									   .first;
				}
				applyErase(*iterator->second, logicalId, type, generation, lsn, superblock, pages, batch);
				return;
			}
			auto iterator = node.leaf_children.find(index);
			if (iterator == node.leaf_children.end()) {
				// Root growth and mutations can retain a child only in the
				// mutable tree until publication writes its physical reference.
				if (!node.page.children[index].isValid()) {
					throwLocationDirectoryError(FormatError::FERR_INVALID_STATE,
							"relocatable location directory deletion references an absent radix route");
				}
				iterator = node.leaf_children
								   .emplace(index, loadLeaf(pages, node.page.children[index], ordinal, type, superblock,
															batch))
								   .first;
			}
			auto &entries = iterator->second->page.entries;
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
				node.page.logical_children[index] = {};
				return;
			}
			iterator->second->page.generation = generation;
			iterator->second->page.committed_lsn = lsn;
			validatePage(iterator->second->page);
		}

		[[nodiscard]] PhysicalPageRef writeLeaf(MutableLeaf &leaf, Generation generation, LogSequenceNumber lsn,
												MetadataPageFile &pages, MetadataWriteBatch &batch,
												PreparedFrameCursor *preparedFrames = nullptr,
												PreparedPageWriter *preparedWriter = nullptr) {
			leaf.page.generation = generation;
			leaf.page.committed_lsn = lsn;
			validatePage(leaf.page);
			PhysicalPageRef reference;
			{
				debug::ScopedPerfTimer timer("native_graph.directory.write_tree.allocate_page");
				reference = takeWriteFrame(batch, preparedFrames);
			}
			{
				debug::ScopedPerfTimer timer("native_graph.directory.write_tree.write_leaf");
				if (preparedWriter == nullptr) {
					pages.writeLocationLeaf(reference, leaf.page);
				} else {
					preparedWriter->enqueue(reference, LocationLeafPageCodec::encode(leaf.page));
				}
			}
			{
				debug::ScopedPerfTimer timer("native_graph.directory.write_tree.register_route");
				batch.registerMetadataPageRoute(leaf.page.page_id, reference);
			}
			return reference;
		}

		void normalizePhysicalChildren(MetadataRadixPage &page) {
			if (!page.logical_children_enabled) {
				return;
			}
			page.logical_children.fill({});
			page.logical_children_enabled = false;
			page.logical_children_resolved = false;
		}

		[[nodiscard]] MetadataPageId readLocationChildPageId(MetadataPageFile &pages, PhysicalPageRef reference,
															 bool radixChild) {
			if (!reference.isValid()) {
				throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
											"relocatable location radix child reference is invalid");
			}
			return radixChild ? pages.readMetadataRadixUnresolved(reference).page_id
							  : pages.readLocationLeafView(reference)->page_id;
		}

		void encodeLogicalChildren(MetadataRadixPage &page, MetadataPageFile &pages) {
			for (size_t index = 0U; index < page.children.size(); ++index) {
				const PhysicalPageRef reference = page.children[index];
				if (!reference.isValid()) {
					page.logical_children[index] = {};
					continue;
				}
				page.logical_children[index] = readLocationChildPageId(pages, reference, page.level != 0U);
				page.children[index] = {};
			}
			page.logical_children_enabled = true;
			page.logical_children_resolved = false;
		}

		[[nodiscard]] PhysicalPageRef logicalizeOwnedInternalChildren(PhysicalPageRef reference,
																	  MetadataDirectoryType directoryType, bool isRoot,
																	  MetadataPageFile &pages,
																	  MetadataWriteBatch &batch) {
			if (!batch.ownsAllocatedPage(reference)) {
				return reference;
			}
			// The final bootstrap route is not available while logicalizing a COW page.
			MetadataRadixPage page = pages.readMetadataRadixUnresolved(reference);
			const uint8_t invalid = static_cast<uint8_t>(page.kind != (isRoot ? CatalogRadixPageKind::CRPK_ROOT
																			  : CatalogRadixPageKind::CRPK_INTERNAL)) |
									static_cast<uint8_t>(page.directory_type != directoryType) |
									static_cast<uint8_t>(page.generation != batch.generation()) |
									static_cast<uint8_t>(page.committed_lsn != batch.checkpointLsn());
			if (invalid != 0U) {
				throwLocationDirectoryError(
						FormatError::FERR_INVALID_STATE,
						"relocatable location logical-child publication found an invalid candidate radix page");
			}
			if (page.logical_children_enabled) {
				return reference;
			}
			if (page.level != 0U) {
				for (PhysicalPageRef &child: page.children) {
					if (child.isValid()) {
						child = logicalizeOwnedInternalChildren(child, directoryType, false, pages, batch);
					}
				}
			}
			encodeLogicalChildren(page, pages);
			const PhysicalPageRef logicalReference = batch.allocatePage();
			pages.writeMetadataRadix(logicalReference, page);
			batch.registerMetadataPageRoute(page.page_id, logicalReference);
			return logicalReference;
		}

		[[nodiscard]] PhysicalPageRef writeNode(MutableNode &node, CatalogRadixPageKind kind, Generation generation,
											LogSequenceNumber lsn, MetadataPageFile &pages,
											MetadataWriteBatch &batch,
											PreparedFrameCursor *preparedFrames = nullptr,
											PreparedPageWriter *preparedWriter = nullptr) {
			node.page.kind = kind;
			// A published logical radix already authenticates stable child IDs.
			// Retain untouched IDs instead of decoding every sibling to recover
			// them again during final logicalization. Physical legacy pages retain
			// the established conversion path.
			const bool retainLogicalChildren = batch.candidate().usesPageDirectoryBootstrap();
			if (retainLogicalChildren && !node.page.logical_children_enabled) {
				// Newly created children already expose their IDs below. Only a
				// legacy physical sibling needs one conversion read; every new COW
				// internal page must be logical even below an already-logical parent.
				for (size_t index = 0U; index < node.page.children.size(); ++index) {
					if (node.page.children[index].isValid() && !node.leaf_children.contains(index) &&
						!node.node_children.contains(index))
						node.page.logical_children[index] = readLocationChildPageId(
								pages, node.page.children[index], node.page.level != 0U);
				}
				node.page.logical_children_enabled = true;
			}
			if (!retainLogicalChildren) normalizePhysicalChildren(node.page);
			node.page.generation = generation;
			node.page.committed_lsn = lsn;
			if (node.page.level == 0U) {
				for (auto &[index, leaf]: node.leaf_children) {
					node.page.children[index] =
							writeLeaf(*leaf, generation, lsn, pages, batch, preparedFrames, preparedWriter);
					if (retainLogicalChildren) node.page.logical_children[index] = leaf->page.page_id;
				}
			} else {
				for (auto &[index, child]: node.node_children) {
					node.page.children[index] =
							writeNode(*child, CatalogRadixPageKind::CRPK_INTERNAL, generation, lsn, pages, batch,
									  preparedFrames, preparedWriter);
					if (retainLogicalChildren) node.page.logical_children[index] = child->page.page_id;
				}
			}
			if (retainLogicalChildren) {
				node.page.children.fill({});
				node.page.logical_children_resolved = false;
				debug::PerfTrace::addValue("native_graph.directory.retained_logical_radix_pages", 1);
			}
			PhysicalPageRef reference;
			{
				debug::ScopedPerfTimer timer("native_graph.directory.write_tree.allocate_page");
				reference = takeWriteFrame(batch, preparedFrames);
			}
			{
				debug::ScopedPerfTimer timer("native_graph.directory.write_tree.write_radix");
				if (preparedWriter == nullptr) {
					pages.writeMetadataRadix(reference, node.page);
				} else {
					preparedWriter->enqueue(reference, MetadataRadixPageCodec::encode(node.page));
				}
			}
			{
				debug::ScopedPerfTimer timer("native_graph.directory.write_tree.register_route");
				batch.registerMetadataPageRoute(node.page.page_id, reference);
			}
			return reference;
		}

		[[nodiscard]] CatalogRadixPage readRoot(MetadataPageFile &pages, const Superblock &superblock,
												MetadataDirectoryType type, const MetadataWriteBatch *batch = nullptr, bool resolveAll = true) {
			const PhysicalPageRef reference = rootFor(pages, superblock, type, batch);
			CatalogRadixPage root = batch != nullptr ? readRadixForMutation(pages, reference, *batch, resolveAll)
													 : pages.readMetadataRadix(reference, superblock);
			if (root.kind != CatalogRadixPageKind::CRPK_ROOT || root.directory_type != type) {
				throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
											"relocatable location Superblock root has an invalid directory type");
			}
			if (batch != nullptr) {
				validateBatchVisible(reference, root.generation, root.committed_lsn, *batch);
			} else {
				validateVisible(root.generation, root.committed_lsn, superblock,
								"relocatable location root is newer than its Superblock");
			}
			return root;
		}

		void publishPageDirectoryRoot(MetadataWriteBatch &batch, PhysicalPageRef dataRoot) {
			if (!batch.candidate().usesPageDirectoryBootstrap()) {
				batch.setPageDirectoryRoot(dataRoot);
				return;
			}
			batch.setPendingBootstrapDataRoot(dataRoot);
		}

		[[nodiscard]] CatalogRadixPage readAppendNode(MetadataPageFile &pages, PhysicalPageRef reference,
													  uint16_t level, MetadataDirectoryType type,
													  const Superblock &superblock,
													  const MetadataWriteBatch *batch = nullptr) {
			CatalogRadixPage page = pages.readMetadataRadix(reference, superblock);
			if (page.kind != CatalogRadixPageKind::CRPK_INTERNAL || page.directory_type != type ||
				page.level != level) {
				throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
											"relocatable streamed location append has an invalid internal radix page");
			}
			if (batch != nullptr) {
				validateBatchVisible(reference, page.generation, page.committed_lsn, *batch);
			} else {
				validateVisible(page.generation, page.committed_lsn, superblock,
								"relocatable streamed location append references a future radix page");
			}
			return page;
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
		const size_t usedBytes =
				RSS_LOCATION_LEAF_HEADER_BYTES + static_cast<size_t>(entryCount) * RSS_LOCATION_ENTRY_BYTES;
		if (!std::all_of(bytes.begin() + usedBytes, bytes.end(),
						 [](std::byte value) { return value == std::byte{}; })) {
			throwLocationDirectoryError(FormatError::FERR_RESERVED_BITS,
										"relocatable location leaf trailing bytes are nonzero");
		}
		LocationLeafPage page;
		page.directory_type = static_cast<MetadataDirectoryType>(rawDirectoryType);
		page.page_id = MetadataPageId(LittleEndianCodec::readU64(bytes, PAGE_ID_OFFSET));
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

	struct LocationDirectoryRangeCursor::Impl final : utils::OwnedPmrObject {
		struct RouteFrame final {
            explicit RouteFrame(std::pmr::memory_resource *memory) : children(memory) {}
			uint16_t level = 0U;
			// Only the cursor's bounded ordinal range is resolved. Keep this view
			// separate from the codec page, whose full-resolution flag stays false.
			std::pmr::vector<PhysicalPageRef> children;
			PhysicalPageRef reference{};
			uint64_t ordinal_base = 0U;
			uint32_t next_child = 0U;
		};

        explicit Impl(const std::shared_ptr<std::pmr::memory_resource> &memory)
            : route(memory ? memory.get() : std::pmr::new_delete_resource()) {}
        // The lazy cursor outlives its creation scope; every advance rebinds
        // this context before decoding routes or allocating retained leaves.
        std::shared_ptr<MetadataPageReadContext> read_context;
		MetadataPageFile *pages = nullptr;
		Superblock superblock;
		MetadataDirectoryType directory_type = MetadataDirectoryType::MDT_PAGE;
		uint64_t next_logical_id = 1U;
		uint64_t upper_logical_id = std::numeric_limits<uint64_t>::max();
		const CancellationCheckpoint *cancellation = nullptr;
		// Borrowed only by the synchronous sorted multi-lookup methods below;
		// their local cursor is destroyed before either caller-owned span expires.
		std::span<const uint64_t> requested_entity_ids;
		std::span<const EntityPropertyKey> requested_property_keys;
		std::pmr::vector<RouteFrame> route;
		std::optional<DecodedMetadataPageView<LocationLeafPage>> leaf;
		size_t leaf_entry_index = 0U;
		uint64_t leaf_ordinal = 0U;
		std::optional<uint64_t> previous_leaf_ordinal;
		size_t metadata_pages_read = 0U;
		bool initialized = false;
		bool exhausted = false;

		void checkCancellation() const {
			if (cancellation != nullptr) {
				cancellation->check();
			}
		}

		[[nodiscard]] RouteFrame resolveFrame(const CatalogRadixPage &page,
				PhysicalPageRef reference, uint64_t ordinalBase) {
			RouteFrame frame(route.get_allocator().resource());
			frame.level = page.level;
			frame.reference = reference;
			frame.ordinal_base = ordinalBase;
			const uint64_t minimum = leafOrdinal(next_logical_id);
			const uint64_t maximum = leafOrdinal(upper_logical_id);
			const uint64_t factor = radixFactor(page.level);
			std::array<uint32_t, RSS_CATALOG_RADIX_FANOUT> indices{};
			std::array<PhysicalPageRef, RSS_CATALOG_RADIX_FANOUT> references{};
			size_t count = 0U;
			const auto selectRequested = [&](const auto requested, const auto ordinalOf) {
				const uint64_t lower = std::max(minimum, ordinalBase);
				auto position = std::lower_bound(requested.begin(), requested.end(), lower,
						[&](const auto &key, uint64_t value) { return ordinalOf(key) < value; });
				for (; position != requested.end(); ++position) {
					const uint64_t ordinal = ordinalOf(*position);
					if (ordinal > maximum) break;
					const uint64_t index = (ordinal - ordinalBase) / factor;
					if (index >= RSS_CATALOG_RADIX_FANOUT) break;
					if (count == 0U || indices[count - 1U] != index)
						indices[count++] = static_cast<uint32_t>(index);
				}
			};
			if (!requested_entity_ids.empty()) {
				selectRequested(requested_entity_ids, [](uint64_t id) { return leafOrdinal(id); });
			} else if (!requested_property_keys.empty()) {
				selectRequested(requested_property_keys,
						[](const EntityPropertyKey &key) { return leafOrdinal(key.directoryId()); });
			} else {
				for (uint32_t index = 0U; index < RSS_CATALOG_RADIX_FANOUT; ++index) {
					if (index != 0U && factor > (std::numeric_limits<uint64_t>::max() - ordinalBase) / index) break;
					const uint64_t ordinal = ordinalBase + static_cast<uint64_t>(index) * factor;
					if (ordinal > maximum) break;
					if (ordinal < minimum && factor <= minimum - ordinal) continue;
					indices[count++] = index;
				}
			}
			checkCancellation();
			pages->resolveMetadataRadixChildren(page, superblock, std::span(indices).first(count),
					std::span(references).first(count));
            // Keep canonical child indices, but allocate only their requested
            // prefix. Shallow/sparse lookups need not retain 256 empty refs.
            frame.children.resize(count == 0U ? 0U : static_cast<size_t>(indices[count - 1U]) + 1U);
			for (size_t index = 0U; index < count; ++index) frame.children[indices[index]] = references[index];
			return frame;
		}

		void initialize() {
			if (initialized || exhausted) {
				return;
			}
			initialized = true;
			checkCancellation();
			const PhysicalPageRef rootReference = rootFor(*pages, superblock, directory_type);
			if (!rootReference.isValid()) {
				exhausted = true;
				return;
			}
			checkCancellation();
			const auto root =
					pages->readMetadataRadixUnresolvedView(rootReference, BufferPoolReadHint::BPRH_SEQUENTIAL);
			++metadata_pages_read;
			debug::PerfTrace::addValue("native_graph.read.directory_range_cursor_root_descents", 1);
			debug::PerfTrace::addValue("native_graph.read.root_descents", 1);
			debug::PerfTrace::addValue("native_graph.read.directory_range_cursor_metadata_pages", 1);
			if (root->kind != CatalogRadixPageKind::CRPK_ROOT || root->directory_type != directory_type ||
				root->level > RSS_CATALOG_RADIX_MAX_LEVEL) {
				throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
										"relocatable location range cursor has an invalid directory root");
			}
			validateVisible(root->generation, root->committed_lsn, superblock,
							"relocatable location range cursor root is newer than its Superblock");
			// The immutable authenticated root fixes the maximum live route depth.
            // Reserving the format's maximum wastes nine frames for shallow trees.
            route.reserve(static_cast<size_t>(root->level) + 1U);
			route.push_back(resolveFrame(*root, rootReference, 0U));
		}

		[[nodiscard]] bool referenceIsOnRoute(const PhysicalPageRef reference) const {
			return std::ranges::any_of(route, [reference](const RouteFrame &frame) {
				return frame.reference == reference;
			});
		}

		[[nodiscard]] bool loadNextLeaf() {
			initialize();
			leaf.reset();
			leaf_entry_index = 0U;
			const uint64_t minimumOrdinal = leafOrdinal(next_logical_id);
			const uint64_t maximumOrdinal = leafOrdinal(upper_logical_id);
			while (!route.empty()) {
				RouteFrame &frame = route.back();
				const uint64_t factor = radixFactor(frame.level);
				bool descended = false;
				while (frame.next_child < frame.children.size()) {
					const uint32_t childIndex = frame.next_child++;
					if (childIndex != 0U &&
						factor > (std::numeric_limits<uint64_t>::max() - frame.ordinal_base) / childIndex) {
						throwLocationDirectoryError(
								FormatError::FERR_ARITHMETIC_OVERFLOW,
								"relocatable location range cursor exceeds the logical-ID domain");
					}
					const uint64_t childOrdinal =
							frame.ordinal_base + static_cast<uint64_t>(childIndex) * factor;
					if (childOrdinal > maximumOrdinal) {
						frame.next_child = RSS_CATALOG_RADIX_FANOUT;
						break;
					}
					const bool endsBeforeMinimum =
							childOrdinal < minimumOrdinal && factor <= minimumOrdinal - childOrdinal;
					if (endsBeforeMinimum) {
						continue;
					}
					const PhysicalPageRef childReference = frame.children[childIndex];
					if (!childReference.isValid()) {
						continue;
					}
					if (referenceIsOnRoute(childReference)) {
						throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
											"relocatable location range cursor found a cyclic radix route");
					}
					checkCancellation();
					if (frame.level == 0U) {
						auto candidate =
								pages->readLocationLeafView(childReference, BufferPoolReadHint::BPRH_SEQUENTIAL);
						++metadata_pages_read;
						debug::PerfTrace::addValue("native_graph.read.directory_range_cursor_metadata_pages", 1);
						if (candidate->directory_type != directory_type) {
							throwLocationDirectoryError(
									FormatError::FERR_INVALID_HEADER,
									"relocatable location range cursor leaf has an unexpected type");
						}
						validateDecodedRoute(candidate, childOrdinal, superblock);
						if (previous_leaf_ordinal.has_value() && childOrdinal <= *previous_leaf_ordinal) {
							throwLocationDirectoryError(
									FormatError::FERR_INVALID_HEADER,
									"relocatable location range cursor leaf order is not strictly increasing");
						}
						previous_leaf_ordinal = childOrdinal;
						leaf_ordinal = childOrdinal;
						leaf_entry_index = static_cast<size_t>(std::lower_bound(
								candidate->entries.begin(), candidate->entries.end(), next_logical_id,
								[](const LocationLeafEntry &entry, const uint64_t id) {
									return entry.logical_id < id;
								}) -
														 candidate->entries.begin());
						leaf.emplace(std::move(candidate));
						if (leaf_entry_index < (*leaf)->entries.size() &&
							(*leaf)->entries[leaf_entry_index].logical_id <= upper_logical_id) {
							return true;
						}
						leaf.reset();
						continue;
					}

					const auto child =
						pages->readMetadataRadixUnresolvedView(childReference, BufferPoolReadHint::BPRH_SEQUENTIAL);
					++metadata_pages_read;
					debug::PerfTrace::addValue("native_graph.read.directory_range_cursor_metadata_pages", 1);
					if (child->kind != CatalogRadixPageKind::CRPK_INTERNAL ||
						child->directory_type != directory_type || child->level + 1U != frame.level) {
						throwLocationDirectoryError(
								FormatError::FERR_INVALID_HEADER,
								"relocatable location range cursor has an invalid internal route");
					}
					validateVisible(child->generation, child->committed_lsn, superblock,
								"relocatable location range cursor internal page is newer than its Superblock");
					if (route.size() >= static_cast<size_t>(RSS_CATALOG_RADIX_MAX_LEVEL) + 1U) {
						throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
											"relocatable location range cursor exceeds the maximum radix depth");
					}
					route.push_back(resolveFrame(*child, childReference, childOrdinal));
					descended = true;
					break;
				}
				if (!descended && frame.next_child >= frame.children.size()) {
					route.pop_back();
				}
			}
			exhausted = true;
			return false;
		}

		[[nodiscard]] std::optional<LocationLeafEntry> nextEntry() {
			checkCancellation();
			if (exhausted) {
				return std::nullopt;
			}
			for (;;) {
				if (!leaf.has_value()) {
					if (!loadNextLeaf()) {
						return std::nullopt;
					}
				} else {
					leaf_entry_index = static_cast<size_t>(std::lower_bound(
							(*leaf)->entries.begin() + static_cast<std::ptrdiff_t>(leaf_entry_index),
							(*leaf)->entries.end(), next_logical_id,
							[](const LocationLeafEntry &entry, const uint64_t id) {
								return entry.logical_id < id;
							}) -
												 (*leaf)->entries.begin());
				}
				if (leaf_entry_index >= (*leaf)->entries.size()) {
					leaf.reset();
					continue;
				}
				const LocationLeafEntry result = (*leaf)->entries[leaf_entry_index++];
				if (result.logical_id > upper_logical_id) {
					exhausted = true;
					leaf.reset();
					return std::nullopt;
				}
				if (result.logical_id == std::numeric_limits<uint64_t>::max()) {
					exhausted = true;
					leaf.reset();
				} else {
					next_logical_id = result.logical_id + 1U;
				}
				return result;
			}
		}

		[[nodiscard]] std::optional<LocationLeafEntry> nextAtOrAfter(const uint64_t minimumLogicalId) {
			if (minimumLogicalId == 0U) {
				throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
										"relocatable location range cursor seek requires a positive logical ID");
			}
			if (minimumLogicalId < next_logical_id) {
				throwLocationDirectoryError(FormatError::FERR_INVALID_STATE,
										"relocatable location range cursor cannot seek backwards");
			}
			if (minimumLogicalId > upper_logical_id) {
				exhausted = true;
				leaf.reset();
				return std::nullopt;
			}
			next_logical_id = minimumLogicalId;
			if (leaf.has_value() && leaf_ordinal < leafOrdinal(minimumLogicalId)) {
				leaf.reset();
			}
			return nextEntry();
		}
	};

	LocationDirectoryRangeCursor::LocationDirectoryRangeCursor(std::unique_ptr<Impl> implementation) noexcept :
		implementation_(std::move(implementation)) {}
	LocationDirectoryRangeCursor::LocationDirectoryRangeCursor(LocationDirectoryRangeCursor &&) noexcept = default;
	LocationDirectoryRangeCursor &
	LocationDirectoryRangeCursor::operator=(LocationDirectoryRangeCursor &&) noexcept = default;
	LocationDirectoryRangeCursor::~LocationDirectoryRangeCursor() = default;

	std::optional<LocationLeafEntry> LocationDirectoryRangeCursor::next() {
		if (implementation_ == nullptr) {
			throwLocationDirectoryError(FormatError::FERR_INVALID_STATE,
									"relocatable location range cursor has no snapshot-bound state");
		}
		std::optional<MetadataPageReadContext::Scope> scope;
        if (implementation_->read_context) scope.emplace(implementation_->read_context, *implementation_->pages->storageIO());
		return implementation_->nextEntry();
	}

	std::optional<LocationLeafEntry>
	LocationDirectoryRangeCursor::nextAtOrAfter(const uint64_t minimumLogicalId) {
		if (implementation_ == nullptr) {
			throwLocationDirectoryError(FormatError::FERR_INVALID_STATE,
									"relocatable location range cursor has no snapshot-bound state");
		}
		std::optional<MetadataPageReadContext::Scope> scope;
        if (implementation_->read_context) scope.emplace(implementation_->read_context, *implementation_->pages->storageIO());
		return implementation_->nextAtOrAfter(minimumLogicalId);
	}

	size_t LocationDirectoryRangeCursor::metadataPagesRead() const noexcept {
		return implementation_ == nullptr ? 0U : implementation_->metadata_pages_read;
	}

	bool LocationDirectoryRangeCursor::exhausted() const noexcept {
		return implementation_ == nullptr || implementation_->exhausted;
	}

	LocationDirectoryRangeCursor LocationDirectory::openRangeCursor(
			const Superblock &superblock, const MetadataDirectoryType directoryType, const uint64_t lowerInclusive,
			const uint64_t upperInclusive, const CancellationCheckpoint *cancellation) const {
		SuperblockCodec::validate(superblock);
		if (!isLocationDirectoryType(directoryType) || lowerInclusive == 0U || lowerInclusive > upperInclusive) {
			throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
									"relocatable location range cursor requires a valid type and logical range");
		}
		auto context = MetadataPageReadContext::Scope::currentOwner(*pages_.storageIO());
        const auto memory = utils::OwnedAllocationScope::active() ? utils::OwnedAllocationScope::owner()
            : (context ? context->memoryOwner() : nullptr);
        static_assert(alignof(LocationDirectoryRangeCursor::Impl) <= alignof(std::max_align_t));
        auto implementation = std::unique_ptr<LocationDirectoryRangeCursor::Impl>(
            new (memory) LocationDirectoryRangeCursor::Impl(memory));
        implementation->read_context = std::move(context);
		implementation->pages = &pages_;
		implementation->superblock = superblock;
		implementation->directory_type = directoryType;
		implementation->next_logical_id = lowerInclusive;
		implementation->upper_logical_id = upperInclusive;
		implementation->cancellation = cancellation;
		debug::PerfTrace::addValue("native_graph.read.directory_range_calls", 1);
		return LocationDirectoryRangeCursor(std::move(implementation));
	}

	struct LocationDirectoryAppendSession::Impl final {
		struct PendingNode final {
			uint64_t group = 0U;
			MutableNode node;
			std::bitset<RSS_CATALOG_RADIX_FANOUT> assigned_children;
		};
		MetadataPageFile &pages;
		MetadataWriteBatch &batch;
		const Superblock &current;
		MetadataDirectoryType directoryType;
		uint64_t expectedLogicalId;
		uint64_t lastLogicalId;
		size_t expectedCount;
		size_t observed = 0U;
		bool requireContiguous = true;
		uint16_t targetLevel;
		std::optional<CatalogRadixPage> sourceRoot;
		uint16_t sourceRootLevel = 0U;
		std::vector<std::optional<PhysicalPageRef>> grownSourceExtensions;
		std::optional<PhysicalPageRef> grownSourceRootChild;
		std::vector<std::optional<PendingNode>> pending;
		uint64_t activeLeafOrdinal = 0U;
		std::unique_ptr<MutableLeaf> activeLeaf;
		bool finished = false;

		Impl(MetadataPageFile &pageFile, MetadataPageAllocator &allocator, MetadataWriteBatch &writeBatch,
			 MetadataDirectoryType type, uint64_t firstLogicalId, uint64_t lastLogicalId, size_t count,
			 bool contiguous = true) :
			// A final streamed publication appends distinct monotonic Page-ID spans
			// (Entity, Property, then LargeValue) in one MetadataWriteBatch.  Start
			// from the candidate so a later session extends the prior session's COW
			// root without collecting its earlier routes in memory.
			pages(pageFile), batch(writeBatch), current(batch.candidate()), directoryType(type),
			expectedLogicalId(firstLogicalId), lastLogicalId(lastLogicalId), expectedCount(count),
			requireContiguous(contiguous), targetLevel(requiredRootLevel(lastLogicalId)),
			grownSourceExtensions(static_cast<size_t>(targetLevel)), pending(static_cast<size_t>(targetLevel) + 1U) {
			if (!batch.usesAllocator(allocator) ||
				(directoryType != MetadataDirectoryType::MDT_PAGE &&
				 directoryType != MetadataDirectoryType::MDT_EDGE_ENTITY &&
				 directoryType != MetadataDirectoryType::MDT_ENTITY_PROPERTY &&
				 directoryType != MetadataDirectoryType::MDT_LARGE_VALUE_MANIFEST) ||
				firstLogicalId == 0U || lastLogicalId < firstLogicalId || count == 0U ||
				count - 1U > lastLogicalId - firstLogicalId ||
				(requireContiguous && lastLogicalId - firstLogicalId != count - 1U)) {
				throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
											"relocatable streamed location append requires one exact bounded range");
			}
			if (rootFor(pages, current, directoryType).isValid()) {
				sourceRoot = readRoot(pages, current, directoryType, &batch);
				sourceRootLevel = sourceRoot->level;
			}
			if (sourceRoot.has_value() && sourceRootLevel > targetLevel) {
				// A reservation can name IDs below an independently published lease.
				// Keep the existing root height so flushing the new low-ID path cannot
				// replace a higher-level root and orphan its other subtrees.
				targetLevel = sourceRootLevel;
				grownSourceExtensions.resize(static_cast<size_t>(targetLevel));
				pending.resize(static_cast<size_t>(targetLevel) + 1U);
			}
			if (sourceRoot.has_value() && targetLevel > sourceRootLevel) {
				// A root cannot become another root's child: readers require every
				// non-top radix page to carry CRPK_INTERNAL. Persist one demoted COW
				// copy before extending the path, while the old snapshot retains its
				// original CRPK_ROOT frame.
				MutableNode demotedSourceRoot;
				demotedSourceRoot.page = *sourceRoot;
				PhysicalPageRef preserved = writeNode(demotedSourceRoot, CatalogRadixPageKind::CRPK_INTERNAL,
													  batch.generation(), batch.checkpointLsn(), pages, batch);
				grownSourceExtensions[sourceRootLevel] = preserved;
				for (uint16_t level = static_cast<uint16_t>(sourceRootLevel + 1U); level < targetLevel; ++level) {
					MutableNode extension;
					extension.page.kind = CatalogRadixPageKind::CRPK_INTERNAL;
					extension.page.directory_type = directoryType;
					extension.page.level = level;
					extension.page.page_id = batch.allocateMetadataPageId();
					extension.page.generation = batch.generation();
					extension.page.committed_lsn = batch.checkpointLsn();
					extension.page.children[0U] = preserved;
					preserved = writeNode(extension, CatalogRadixPageKind::CRPK_INTERNAL, batch.generation(),
										  batch.checkpointLsn(), pages, batch);
					grownSourceExtensions[level] = preserved;
				}
				grownSourceRootChild = preserved;
			}
		}

		[[nodiscard]] std::optional<CatalogRadixPage> sourceNode(uint16_t level, uint64_t group) const {
			if (level < grownSourceExtensions.size() && grownSourceExtensions[level].has_value()) {
				return group == 0U ? std::optional<CatalogRadixPage>(
											 readAppendNode(pages, *grownSourceExtensions[level], level, directoryType,
															batch.candidate(), &batch))
								   : std::nullopt;
			}
			if (!sourceRoot.has_value() || sourceRootLevel < level)
				return std::nullopt;
			if (sourceRootLevel == level)
				return group == 0U ? sourceRoot : std::nullopt;
			uint64_t factor = 1U;
			for (uint16_t index = 0U; index <= level; ++index) {
				if (factor > std::numeric_limits<uint64_t>::max() / RSS_CATALOG_RADIX_FANOUT) {
					throwLocationDirectoryError(
							FormatError::FERR_ARITHMETIC_OVERFLOW,
							"relocatable streamed location directory route overflows its radix domain");
				}
				factor *= RSS_CATALOG_RADIX_FANOUT;
			}
			if (group > std::numeric_limits<uint64_t>::max() / factor) {
				throwLocationDirectoryError(FormatError::FERR_ARITHMETIC_OVERFLOW,
											"relocatable streamed location directory group overflows its radix domain");
			}
			CatalogRadixPage node = *sourceRoot;
			const uint64_t leaf = group * factor;
			for (uint16_t nodeLevel = sourceRootLevel; nodeLevel > level; --nodeLevel) {
				const PhysicalPageRef child =
						node.children[CatalogRadixPageCodec::childIndexForLeafOrdinal(leaf, nodeLevel)];
				if (!child.isValid())
					return std::nullopt;
				node = readAppendNode(pages, child, static_cast<uint16_t>(nodeLevel - 1U), directoryType, current,
									  &batch);
			}
			return node;
		}

		[[nodiscard]] MutableNode beginNode(uint16_t level, uint64_t group) {
			if (const auto source = sourceNode(level, group); source.has_value()) {
				MutableNode result;
				result.page = *source;
				return result;
			}
			MutableNode result;
			result.page.kind =
					level == targetLevel ? CatalogRadixPageKind::CRPK_ROOT : CatalogRadixPageKind::CRPK_INTERNAL;
			result.page.directory_type = directoryType;
			result.page.level = level;
			result.page.page_id = batch.allocateMetadataPageId();
			result.page.generation = batch.generation();
			result.page.committed_lsn = batch.checkpointLsn();
			if (grownSourceRootChild.has_value() && level == targetLevel && group == 0U)
				result.page.children[0U] = *grownSourceRootChild;
			return result;
		}

		void appendChild(uint16_t level, uint64_t childOrdinal, PhysicalPageRef child) {
			const uint64_t group = childOrdinal / RSS_CATALOG_RADIX_FANOUT;
			const uint32_t childIndex = static_cast<uint32_t>(childOrdinal % RSS_CATALOG_RADIX_FANOUT);
			if (level > targetLevel || (level == targetLevel && group != 0U)) {
				throwLocationDirectoryError(FormatError::FERR_INVALID_STATE,
											"relocatable streamed location append has an out-of-range radix child");
			}
			if (pending[level].has_value() && pending[level]->group != group)
				flushLevel(level);
			if (!pending[level].has_value())
				pending[level].emplace(PendingNode{group, beginNode(level, group), {}});
			CatalogRadixPage &page = pending[level]->node.page;
			if (pending[level]->assigned_children.test(childIndex) && page.children[childIndex] != child) {
				throwLocationDirectoryError(FormatError::FERR_INVALID_STATE,
											"relocatable streamed location append assigned one radix child twice");
			}
			page.children[childIndex] = child;
			pending[level]->assigned_children.set(childIndex);
		}

		void flushLevel(uint16_t level) {
			if (!pending[level].has_value())
				return;
			PendingNode completed = std::move(*pending[level]);
			pending[level].reset();
			const PhysicalPageRef reference = writeNode(completed.node,
														level == targetLevel ? CatalogRadixPageKind::CRPK_ROOT
																			 : CatalogRadixPageKind::CRPK_INTERNAL,
														batch.generation(), batch.checkpointLsn(), pages, batch);
			if (level == targetLevel) {
				if (completed.group != 0U) {
					throwLocationDirectoryError(
							FormatError::FERR_INVALID_STATE,
							"relocatable streamed location append exceeded its planned root coverage");
				}
				if (directoryType == MetadataDirectoryType::MDT_PAGE)
					publishPageDirectoryRoot(batch, reference);
				else if (directoryType == MetadataDirectoryType::MDT_EDGE_ENTITY)
					batch.setEdgeEntityDirectoryRoot(reference);
				else if (directoryType == MetadataDirectoryType::MDT_ENTITY_PROPERTY)
					batch.setEntityPropertyDirectoryRoot(reference);
				else
					batch.setLargeValueManifestDirectoryRoot(reference);
				return;
			}
			appendChild(static_cast<uint16_t>(level + 1U), completed.group, reference);
		}

		void finishLeaf() {
			if (activeLeaf == nullptr)
				return;
			const PhysicalPageRef reference =
					writeLeaf(*activeLeaf, batch.generation(), batch.checkpointLsn(), pages, batch);
			appendChild(0U, activeLeafOrdinal, reference);
			activeLeaf.reset();
		}

		void append(const LocationLeafEntry &entry) {
			const bool invalidOrder =
					requireContiguous ? entry.logical_id != expectedLogicalId
									  : entry.logical_id < expectedLogicalId || entry.logical_id > lastLogicalId;
			if (finished || observed == expectedCount || invalidOrder) {
				const char *const message =
						directoryType == MetadataDirectoryType::MDT_PAGE
								? "relocatable streamed Page source diverged from its immutable ID range"
						: directoryType == MetadataDirectoryType::MDT_EDGE_ENTITY
								? "relocatable streamed Edge source diverged from its immutable ID range"
						: directoryType == MetadataDirectoryType::MDT_ENTITY_PROPERTY
								? "relocatable streamed Property source diverged from its immutable ID range"
						: directoryType == MetadataDirectoryType::MDT_LARGE_VALUE_MANIFEST
								? "relocatable streamed LargeValue Manifest source diverged from its immutable ID range"
								: "relocatable streamed location source diverged from its immutable ID range";
				throwLocationDirectoryError(FormatError::FERR_INVALID_STATE, message);
			}
			validateEntry(entry, directoryType);
			if (directoryType == MetadataDirectoryType::MDT_PAGE)
				batch.observeLogicalPageId(PageId(entry.logical_id));
			const uint64_t leaf = leafOrdinal(entry.logical_id);
			if (activeLeaf == nullptr || activeLeafOrdinal != leaf) {
				finishLeaf();
				activeLeafOrdinal = leaf;
				const uint64_t parentGroup = leaf / RSS_CATALOG_RADIX_FANOUT;
				if (const auto parent = sourceNode(0U, parentGroup); parent.has_value()) {
					const PhysicalPageRef reference =
							parent->children[CatalogRadixPageCodec::childIndexForLeafOrdinal(leaf, 0U)];
					activeLeaf = reference.isValid()
										 ? loadLeaf(pages, reference, leaf, directoryType, current, &batch)
										 : makeLeaf(directoryType, batch.generation(), batch.checkpointLsn(), batch);
				} else
					activeLeaf = makeLeaf(directoryType, batch.generation(), batch.checkpointLsn(), batch);
			}
			// Streamed sources are monotonic. Appending to a leaf's tail avoids a
			// per-record search and shift for the normal bulk path, while the
			// ordered-insert fallback preserves rebase of a lower sparse lease.
			if (activeLeaf->page.entries.empty() || activeLeaf->page.entries.back().logical_id < entry.logical_id) {
				activeLeaf->page.entries.push_back(entry);
			} else {
				auto position =
						std::lower_bound(activeLeaf->page.entries.begin(), activeLeaf->page.entries.end(),
										 entry.logical_id, [](const LocationLeafEntry &candidate, uint64_t logicalId) {
											 return candidate.logical_id < logicalId;
										 });
				if (position != activeLeaf->page.entries.end() && position->logical_id == entry.logical_id) {
					throwLocationDirectoryError(
							FormatError::FERR_INVALID_STATE,
							"relocatable streamed location append attempted to publish an existing logical ID");
				}
				activeLeaf->page.entries.insert(position, entry);
			}
			activeLeaf->page.generation = batch.generation();
			activeLeaf->page.committed_lsn = batch.checkpointLsn();
			// The loaded or newly created leaf was already complete, and this path
			// validates the appended entry, route, ordering, and uniqueness above.
			// Authenticate the complete bounded leaf once at its write boundary.
			++observed;
			if (entry.logical_id != std::numeric_limits<uint64_t>::max()) {
				expectedLogicalId = entry.logical_id + 1U;
			}
		}

		void finish() {
			if (finished || observed != expectedCount) {
				throwLocationDirectoryError(
						FormatError::FERR_INVALID_STATE,
						"relocatable streamed location append did not consume its exact bounded source");
			}
			finishLeaf();
			for (uint16_t level = 0U; level <= targetLevel; ++level)
				flushLevel(level);
			finished = true;
		}
	};

	LocationDirectoryAppendSession::LocationDirectoryAppendSession(std::unique_ptr<Impl> implementation) noexcept :
		implementation_(std::move(implementation)) {}
	LocationDirectoryAppendSession::LocationDirectoryAppendSession(LocationDirectoryAppendSession &&) noexcept =
			default;
	LocationDirectoryAppendSession &
	LocationDirectoryAppendSession::operator=(LocationDirectoryAppendSession &&) noexcept = default;
	LocationDirectoryAppendSession::~LocationDirectoryAppendSession() = default;
	void LocationDirectoryAppendSession::append(const PageLocation &location) {
		if (implementation_ == nullptr || implementation_->directoryType != MetadataDirectoryType::MDT_PAGE) {
			throwLocationDirectoryError(FormatError::FERR_INVALID_STATE,
										"relocatable Page directory append session is not active");
		}
		implementation_->append({location.page_id.value(), location.segment_id, location.generation,
								 location.page_index, 1U, location.free_bytes});
	}
	void LocationDirectoryAppendSession::append(const EntityPageLocation &location) {
		if (implementation_ == nullptr || implementation_->directoryType != MetadataDirectoryType::MDT_EDGE_ENTITY) {
			throwLocationDirectoryError(FormatError::FERR_INVALID_STATE,
										"relocatable Edge directory append session is not active");
		}
		implementation_->append({location.entity_id, SegmentId(location.page_id.value()),
								 Generation(location.slot_generation), location.slot_id, 1U});
	}
	void LocationDirectoryAppendSession::append(const EntityPropertyLocation &location) {
		if (implementation_ == nullptr ||
			implementation_->directoryType != MetadataDirectoryType::MDT_ENTITY_PROPERTY || !location.key.isValid()) {
			throwLocationDirectoryError(FormatError::FERR_INVALID_STATE,
										"relocatable Entity Property directory append session is not active");
		}
		implementation_->append({location.key.directoryId(), SegmentId(location.page_id.value()),
								 Generation(location.slot_generation), location.slot_id, 1U});
	}
	void LocationDirectoryAppendSession::append(const LargeValueManifestLocation &location) {
		if (implementation_ == nullptr ||
			implementation_->directoryType != MetadataDirectoryType::MDT_LARGE_VALUE_MANIFEST ||
			!location.value_id.isValid() || !location.manifest_page_id.isValid() || !location.generation.isValid()) {
			throwLocationDirectoryError(FormatError::FERR_INVALID_STATE,
										"relocatable LargeValue Manifest directory append session is not active");
		}
		implementation_->append({location.value_id.value(), SegmentId(location.manifest_page_id.value()),
								 location.generation, RSS_PAGE_POOL_FIRST_DATA_FRAME, 1U});
	}
	void LocationDirectoryAppendSession::finish() {
		if (implementation_ == nullptr)
			throwLocationDirectoryError(FormatError::FERR_INVALID_STATE,
										"relocatable location append session is not active");
		implementation_->finish();
	}
	bool LocationDirectoryAppendSession::isFinished() const noexcept {
		return implementation_ != nullptr && implementation_->finished;
	}

	LocationDirectoryAppendSession LocationDirectory::beginAppendPageLocations(MetadataWriteBatch &batch,
																			   uint64_t firstPageId,
																			   uint64_t lastPageId,
																			   size_t expectedCount) {
		return LocationDirectoryAppendSession(std::make_unique<LocationDirectoryAppendSession::Impl>(
				pages_, allocator_, batch, MetadataDirectoryType::MDT_PAGE, firstPageId, lastPageId, expectedCount));
	}
	LocationDirectoryAppendSession LocationDirectory::beginAppendEdgePageLocations(MetadataWriteBatch &batch,
																				   uint64_t firstEdgeId,
																				   uint64_t lastEdgeId,
																				   size_t expectedCount) {
		return LocationDirectoryAppendSession(std::make_unique<LocationDirectoryAppendSession::Impl>(
				pages_, allocator_, batch, MetadataDirectoryType::MDT_EDGE_ENTITY, firstEdgeId, lastEdgeId,
				expectedCount, false));
	}
	LocationDirectoryAppendSession LocationDirectory::beginAppendEdgePropertyLocations(MetadataWriteBatch &batch,
																					   uint64_t firstEdgeId,
																					   uint64_t lastEdgeId,
																					   size_t expectedCount) {
		if (firstEdgeId == 0U || lastEdgeId < firstEdgeId || lastEdgeId > ((UINT64_C(1) << 63U) - 1U)) {
			throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
										"relocatable streamed Edge Property append has an invalid Edge-ID range");
		}
		const uint64_t mask = UINT64_C(1) << 63U;
		return LocationDirectoryAppendSession(std::make_unique<LocationDirectoryAppendSession::Impl>(
				pages_, allocator_, batch, MetadataDirectoryType::MDT_ENTITY_PROPERTY, firstEdgeId | mask,
				lastEdgeId | mask, expectedCount, false));
	}

	LocationDirectoryAppendSession LocationDirectory::beginAppendLargeValueManifestLocations(MetadataWriteBatch &batch,
																							 LargeValueId firstValueId,
																							 LargeValueId lastValueId,
																							 size_t expectedCount) {
		if (!firstValueId.isValid() || !lastValueId.isValid() || lastValueId < firstValueId) {
			throwLocationDirectoryError(
					FormatError::FERR_INVALID_HEADER,
					"relocatable streamed LargeValue Manifest append has an invalid value-ID range");
		}
		return LocationDirectoryAppendSession(std::make_unique<LocationDirectoryAppendSession::Impl>(
				pages_, allocator_, batch, MetadataDirectoryType::MDT_LARGE_VALUE_MANIFEST, firstValueId.value(),
				lastValueId.value(), expectedCount, false));
	}

	void LocationDirectory::prepareAppendLocations(MetadataWriteBatch &batch, MetadataDirectoryType directoryType,
												   uint64_t firstLogicalId, uint64_t lastLogicalId,
												   size_t expectedCount, const LocationEntryProducer &producer) {
		if (!producer) {
			throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
										"relocatable streamed location append requires a source producer");
		}
		if (directoryType != MetadataDirectoryType::MDT_PAGE &&
			directoryType != MetadataDirectoryType::MDT_EDGE_ENTITY) {
			throwLocationDirectoryError(FormatError::FERR_INVALID_ENUM,
										"relocatable streamed location append has no compatible directory session");
		}
		auto session = directoryType == MetadataDirectoryType::MDT_PAGE
							   ? beginAppendPageLocations(batch, firstLogicalId, lastLogicalId, expectedCount)
							   : beginAppendEdgePageLocations(batch, firstLogicalId, lastLogicalId, expectedCount);
		producer([&session, directoryType](const LocationLeafEntry &entry) {
			if (directoryType == MetadataDirectoryType::MDT_PAGE) {
				session.append(PageLocation{PageId(entry.logical_id), entry.segment_id, entry.first_page,
											entry.generation, entry.free_bytes});
			} else {
				if (entry.generation.value() > std::numeric_limits<uint32_t>::max()) {
					throwLocationDirectoryError(
							FormatError::FERR_INVALID_STATE,
							"relocatable streamed Edge directory slot generation exceeds its codec");
				}
				session.append(EntityPageLocation{entry.logical_id, PageId(entry.segment_id.value()), entry.first_page,
												  static_cast<uint32_t>(entry.generation.value())});
			}
		});
		session.finish();
	}

	void LocationDirectory::prepareAppendEdgePageLocations(MetadataWriteBatch &batch, uint64_t firstEdgeId,
														   uint64_t lastEdgeId, size_t expectedCount,
														   const EdgePageLocationCursor &cursor) {
		if (!cursor) {
			throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
										"relocatable streamed Edge directory append requires a source cursor");
		}
		const EdgePageLocationProducer producer = [&cursor](const EdgePageLocationSink &consume) {
			for (;;) {
				const std::optional<EntityPageLocation> location = cursor();
				if (!location.has_value()) {
					return;
				}
				consume(*location);
			}
		};
		prepareAppendEdgePageLocations(batch, firstEdgeId, lastEdgeId, expectedCount, producer);
	}

	void LocationDirectory::prepareAppendEdgePageLocations(MetadataWriteBatch &batch, uint64_t firstEdgeId,
														   uint64_t lastEdgeId, size_t expectedCount,
														   const EdgePageLocationProducer &producer) {
		if (!producer) {
			throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
										"relocatable streamed Edge directory append requires a source producer");
		}
		const LocationEntryProducer converted = [&producer](const LocationEntrySink &consume) {
			producer([&consume](const EntityPageLocation &location) {
				consume({location.entity_id, SegmentId(location.page_id.value()), Generation(location.slot_generation),
						 location.slot_id, 1U});
			});
		};
		prepareAppendLocations(batch, MetadataDirectoryType::MDT_EDGE_ENTITY, firstEdgeId, lastEdgeId, expectedCount,
							   converted);
	}

	void LocationDirectory::prepareAppendPageLocations(MetadataWriteBatch &batch, uint64_t firstPageId,
													   uint64_t lastPageId, size_t expectedCount,
													   const PageLocationCursor &cursor) {
		if (!cursor) {
			throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
										"relocatable streamed Page directory append requires a source cursor");
		}
		const PageLocationProducer producer = [&cursor](const PageLocationSink &consume) {
			for (;;) {
				const std::optional<PageLocation> location = cursor();
				if (!location.has_value()) {
					return;
				}
				consume(*location);
			}
		};
		prepareAppendPageLocations(batch, firstPageId, lastPageId, expectedCount, producer);
	}

	void LocationDirectory::prepareAppendPageLocations(MetadataWriteBatch &batch, uint64_t firstPageId,
													   uint64_t lastPageId, size_t expectedCount,
													   const PageLocationProducer &producer) {
		if (!producer) {
			throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
										"relocatable streamed Page directory append requires a source producer");
		}
		const LocationEntryProducer converted = [&producer](const LocationEntrySink &consume) {
			producer([&consume](const PageLocation &location) {
				consume({location.page_id.value(), location.segment_id, location.generation, location.page_index, 1U,
						 location.free_bytes});
			});
		};
		prepareAppendLocations(batch, MetadataDirectoryType::MDT_PAGE, firstPageId, lastPageId, expectedCount,
							   converted);
	}

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
		debug::ScopedPerfTimer prepareTimer("native_graph.directory.prepare_upsert");
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
		// A streamed Page append may have already built a candidate root in this
		// batch. Later typed pages must COW from that root rather than discard it
		// by reopening the published generation.
		const Superblock &current =
				directoryType == MetadataDirectoryType::MDT_PAGE ? batch.candidate() : batch.current();
		const Generation generation = batch.generation();
		const LogSequenceNumber lsn = batch.checkpointLsn();
		const uint16_t targetLevel = requiredRootLevel(entries.back().logical_id);
		std::unique_ptr<MutableNode> root;
		if (!rootFor(pages_, current, directoryType, &batch).isValid()) {
			root = makeNode(directoryType, CatalogRadixPageKind::CRPK_ROOT, targetLevel, generation, lsn, batch);
		} else {
			root = std::make_unique<MutableNode>();
			root->page = readRoot(pages_, current, directoryType, &batch, false);
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
		{
			debug::ScopedPerfTimer mutationTimer("native_graph.directory.prepare_upsert.apply_mutations");
			for (const LocationLeafEntry &entry: entries) {
				applyMutation(*root, entry, directoryType, generation, lsn, current, pages_, batch);
			}
		}
		const size_t allocatedBefore = batch.allocatedPageCount();
		PhysicalPageRef newRoot;
		{
			debug::ScopedPerfTimer writeTimer("native_graph.directory.prepare_upsert.write_tree");
			const std::vector<PhysicalPageRef> preparedFrames = batch.allocatePages(writePageCount(*root));
			PreparedFrameCursor cursor{preparedFrames};
			PreparedPageWriter writer(pages_, batch);
			newRoot =
					writeNode(*root, CatalogRadixPageKind::CRPK_ROOT, generation, lsn, pages_, batch, &cursor, &writer);
			writer.flush();
			if (cursor.next != preparedFrames.size()) {
				throwLocationDirectoryError(FormatError::FERR_INVALID_STATE,
										"relocatable location tree did not consume every prepared metadata frame");
			}
		}
		if (debug::PerfTrace::areValuesEnabled()) {
			debug::PerfTrace::addValue("native_graph.commit.directory_publication_pages",
					static_cast<int64_t>(batch.allocatedPageCount() - allocatedBefore));
		}
		switch (directoryType) {
			case MetadataDirectoryType::MDT_PAGE:
				publishPageDirectoryRoot(batch, newRoot);
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
		debug::ScopedPerfTimer prepareTimer("native_graph.directory.prepare_replace");
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
		// See prepareUpsert(): Page mutations in one batch compose from the
		// candidate root so streamed routes remain reachable with later Index pages.
		const Superblock &current =
				directoryType == MetadataDirectoryType::MDT_PAGE ? batch.candidate() : batch.current();
		const Generation generation = batch.generation();
		const LogSequenceNumber lsn = batch.checkpointLsn();
		std::unique_ptr<MutableNode> root;
		if (!rootFor(pages_, current, directoryType, &batch).isValid()) {
			if (entries.empty()) {
				throwLocationDirectoryError(FormatError::FERR_INVALID_STATE,
											"relocatable location replacement cannot erase an absent directory");
			}
			root = makeNode(directoryType, CatalogRadixPageKind::CRPK_ROOT,
							requiredRootLevel(entries.back().logical_id), generation, lsn, batch);
		} else {
			root = std::make_unique<MutableNode>();
			root->page = readRoot(pages_, current, directoryType, &batch, false);
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
		{
			debug::ScopedPerfTimer mutationTimer("native_graph.directory.prepare_replace.apply_mutations");
			for (const LocationLeafEntry &entry: entries) {
				applyMutation(*root, entry, directoryType, generation, lsn, current, pages_, batch);
			}
			for (uint64_t logicalId: erasedLogicalIds) {
				applyErase(*root, logicalId, directoryType, generation, lsn, current, pages_, &batch);
			}
		}
		const size_t allocatedBefore = batch.allocatedPageCount();
		PhysicalPageRef newRoot;
		{
			debug::ScopedPerfTimer writeTimer("native_graph.directory.prepare_replace.write_tree");
			const std::vector<PhysicalPageRef> preparedFrames = batch.allocatePages(writePageCount(*root));
			PreparedFrameCursor cursor{preparedFrames};
			PreparedPageWriter writer(pages_, batch);
			newRoot =
					writeNode(*root, CatalogRadixPageKind::CRPK_ROOT, generation, lsn, pages_, batch, &cursor, &writer);
			writer.flush();
			if (cursor.next != preparedFrames.size()) {
				throwLocationDirectoryError(FormatError::FERR_INVALID_STATE,
										"relocatable location tree did not consume every prepared metadata frame");
			}
		}
		if (debug::PerfTrace::areValuesEnabled()) {
			debug::PerfTrace::addValue("native_graph.commit.directory_publication_pages",
					static_cast<int64_t>(batch.allocatedPageCount() - allocatedBefore));
		}
		switch (directoryType) {
			case MetadataDirectoryType::MDT_PAGE:
				publishPageDirectoryRoot(batch, newRoot);
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

	void LocationDirectory::prepareLogicalRootChildren(MetadataWriteBatch &batch) {
		if (!batch.candidate().usesPageDirectoryBootstrap()) {
			return;
		}
		// The bootstrap owns the data-directory root's physical anchor, while the
		// directory itself may route every descendant through stable PageIds.
		if (const std::optional<PhysicalPageRef> dataRoot = batch.pendingBootstrapDataRoot();
			dataRoot.has_value() && batch.ownsAllocatedPage(*dataRoot)) {
			batch.setPendingBootstrapDataRoot(
					logicalizeOwnedInternalChildren(*dataRoot, MetadataDirectoryType::MDT_PAGE, true, pages_, batch));
		}
		constexpr std::array directoryTypes = {
				MetadataDirectoryType::MDT_EXTENT,
				MetadataDirectoryType::MDT_NODE_ENTITY,
				MetadataDirectoryType::MDT_EDGE_ENTITY,
				MetadataDirectoryType::MDT_ENTITY_PROPERTY,
				MetadataDirectoryType::MDT_STATE,
				MetadataDirectoryType::MDT_INDEX_ROOT,
				MetadataDirectoryType::MDT_VECTOR_LOCATION,
				MetadataDirectoryType::MDT_VECTOR_MANIFEST,
				MetadataDirectoryType::MDT_LARGE_VALUE_MANIFEST,
				MetadataDirectoryType::MDT_RETIRE_QUEUE,
		};
		for (const MetadataDirectoryType directoryType: directoryTypes) {
			const PhysicalPageRef rootReference = physicalRootFor(batch.candidate(), directoryType);
			if (!rootReference.isValid() || !batch.ownsAllocatedPage(rootReference)) {
				continue;
			}
			setPhysicalRoot(batch, directoryType,
							logicalizeOwnedInternalChildren(rootReference, directoryType, true, pages_, batch));
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
		SuperblockCodec::validate(superblock);
		if (logicalId == 0U) {
			throwLocationDirectoryError(FormatError::FERR_INVALID_STATE, "relocatable location lookup requires an ID");
		}
		const PhysicalPageRef rootReference = rootFor(pages_, superblock, directoryType);
		if (!rootReference.isValid()) {
			return std::nullopt;
		}
		// Reuse the root just authenticated above: readRoot would resolve it again.
		auto node = pages_.readMetadataRadixUnresolvedView(rootReference);
		if (node->kind != CatalogRadixPageKind::CRPK_ROOT || node->directory_type != directoryType) {
			throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
					"relocatable location Superblock root has an invalid directory type");
		}
		validateVisible(node->generation, node->committed_lsn, superblock,
				"relocatable location root is newer than its Superblock");
		const uint64_t ordinal = leafOrdinal(logicalId);
		for (;;) {
			if (node->kind == CatalogRadixPageKind::CRPK_ROOT) {
				uint64_t quotient = ordinal;
				for (uint16_t level = 0U; level <= node->level; ++level) quotient /= RSS_CATALOG_RADIX_FANOUT;
				if (quotient != 0U) {
					// Even an out-of-domain miss must reject a foreign route context.
					pages_.resolveMetadataRadixChildren(*node, superblock, {}, {});
					return std::nullopt;
				}
			}
			const std::array<uint32_t, 1U> indices{CatalogRadixPageCodec::childIndexForLeafOrdinal(ordinal, node->level)};
			std::array<PhysicalPageRef, 1U> references{};
			pages_.resolveMetadataRadixChildren(*node, superblock, indices, references);
			const PhysicalPageRef reference = references.front();
			if (!reference.isValid()) {
				return std::nullopt;
			}
			if (node->level == 0U) {
				const auto leaf = pages_.readLocationLeafView(reference);
				if (leaf->directory_type != directoryType) {
					throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
												"relocatable location leaf has an unexpected directory type");
				}
				validateDecodedRoute(leaf, ordinal, superblock);
				const auto position = std::lower_bound(
						leaf->entries.begin(), leaf->entries.end(), logicalId,
						[](const LocationLeafEntry &entry, uint64_t id) { return entry.logical_id < id; });
				if (position == leaf->entries.end() || position->logical_id != logicalId) {
					return std::nullopt;
				}
				return *position;
			}
			auto child = pages_.readMetadataRadixUnresolvedView(reference);
			if (child->kind != CatalogRadixPageKind::CRPK_INTERNAL || child->directory_type != directoryType ||
				child->level + 1U != node->level) {
				throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
											"relocatable location directory has an invalid radix route");
			}
			validateVisible(child->generation, child->committed_lsn, superblock,
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
		SuperblockCodec::validate(superblock);
		const PhysicalPageRef rootReference = rootFor(pages_, superblock, directoryType);
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
							pages_.readMetadataRadix(childReference, superblock, BufferPoolReadHint::BPRH_SEQUENTIAL);
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
				const auto leaf =
						pages_.readLocationLeafView(childReference, BufferPoolReadHint::BPRH_SEQUENTIAL);
				if (leaf->directory_type != directoryType) {
					throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
												"relocatable entity range absence proof found a leaf of another type");
				}
				validateDecodedRoute(leaf, childOrdinal, superblock);
				const auto found = std::lower_bound(
						leaf->entries.begin(), leaf->entries.end(), firstLogicalId,
						[](const LocationLeafEntry &entry, uint64_t id) { return entry.logical_id < id; });
				if (found != leaf->entries.end() && found->logical_id <= lastLogicalId) {
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
		SuperblockCodec::validate(superblock);
		if (!visitor) { // ZYX_COV_EXCL_LINE: public traversal wrappers reject an empty visitor before delegation.
			throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
										"relocatable location directory traversal requires a visitor");
		}
		const PhysicalPageRef rootReference = rootFor(pages_, superblock, directoryType);
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
					const auto leaf =
							pages_.readLocationLeafView(childReference, BufferPoolReadHint::BPRH_SEQUENTIAL);
					if (leaf->directory_type != directoryType) {
						throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
													"relocatable location directory leaf has an unexpected type");
					}
					validateDecodedRoute(leaf, childOrdinal, superblock);
					for (const LocationLeafEntry &entry: leaf->entries) {
						if (!visitor(entry)) {
							return false;
						}
					}
					continue;
				}
				const CatalogRadixPage child =
						pages_.readMetadataRadix(childReference, superblock, BufferPoolReadHint::BPRH_SEQUENTIAL);
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
		SuperblockCodec::validate(superblock);
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
		const PhysicalPageRef rootReference = rootFor(pages_, superblock, directoryType);
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
					const auto leaf =
							pages_.readLocationLeafView(childReference, BufferPoolReadHint::BPRH_SEQUENTIAL);
					if (leaf->directory_type != directoryType) {
						throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
													"relocatable location directory leaf has an unexpected type");
					}
					validateDecodedRoute(leaf, childOrdinal, superblock);
					auto entry = leaf->entries.begin();
					if (childOrdinal == firstOrdinal) {
						entry = std::upper_bound(leaf->entries.begin(), leaf->entries.end(), afterLogicalId,
												 [](uint64_t id, const LocationLeafEntry &candidate) {
													 return id < candidate.logical_id;
												 });
					}
					for (; entry != leaf->entries.end(); ++entry) {
						if (!visitor(*entry)) {
							return false;
						}
					}
					continue;
				}
				const CatalogRadixPage child =
						pages_.readMetadataRadix(childReference, superblock, BufferPoolReadHint::BPRH_SEQUENTIAL);
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

	void LocationDirectory::findEntityPageLocationsSorted(
			const Superblock &superblock, const MetadataDirectoryType directoryType,
			const std::span<const uint64_t> entityIds,
			const std::span<std::optional<EntityPageLocation>> results,
			const CancellationCheckpoint *cancellation) const {
		if (directoryType != MetadataDirectoryType::MDT_NODE_ENTITY &&
			directoryType != MetadataDirectoryType::MDT_EDGE_ENTITY) {
			throwLocationDirectoryError(FormatError::FERR_INVALID_ENUM,
															"relocatable Entity multi-lookup requires a Node or Edge directory");
		}
		if (entityIds.size() != results.size()) {
			throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
															"relocatable Entity multi-lookup requires equal ID and result spans");
		}
		std::fill(results.begin(), results.end(), std::nullopt);
		if (entityIds.empty()) {
			return;
		}
		debug::PerfTrace::addValue("native_graph.read.entity_directory_batch_requests", 1);
		debug::PerfTrace::addValue("native_graph.read.entity_directory_batch_ids",
				static_cast<int64_t>(entityIds.size()));
		uint64_t previousId = 0U;
		for (const uint64_t entityId: entityIds) {
			if (entityId == 0U || entityId <= previousId) {
				throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
															"relocatable Entity multi-lookup IDs must be strictly ordered and nonzero");
			}
			previousId = entityId;
		}

		LocationDirectoryRangeCursor cursor = openRangeCursor(
				superblock, directoryType, entityIds.front(), entityIds.back(), cancellation);
		cursor.implementation_->requested_entity_ids = entityIds;
		std::optional<LocationLeafEntry> pending;
		for (size_t index = 0U; index < entityIds.size(); ++index) {
			const uint64_t requested = entityIds[index];
			if (pending.has_value() && pending->logical_id < requested) {
				pending.reset();
			}
			if (!pending.has_value()) {
				pending = cursor.nextAtOrAfter(requested);
			}
			if (!pending.has_value()) {
				break;
			}
			if (pending->logical_id != requested) {
				continue;
			}
			results[index] = EntityPageLocation{requested, PageId(pending->segment_id.value()), pending->first_page,
															static_cast<uint32_t>(pending->generation.value())};
			pending.reset();
		}
	}

	void LocationDirectory::findEntityPropertiesSorted(
			const Superblock &superblock, const std::span<const EntityPropertyKey> keys,
			const std::span<std::optional<EntityPropertyLocation>> results,
			const CancellationCheckpoint *cancellation) const {
		if (keys.size() != results.size()) {
			throwLocationDirectoryError(
					FormatError::FERR_INVALID_HEADER,
					"relocatable Entity Property multi-lookup requires equal key and result spans");
		}
		std::fill(results.begin(), results.end(), std::nullopt);
		if (keys.empty()) {
			return;
		}
		uint64_t previousDirectoryId = 0U;
		for (const EntityPropertyKey &key: keys) {
			const uint64_t directoryId = key.directoryId();
			if (directoryId <= previousDirectoryId) {
				throwLocationDirectoryError(
						FormatError::FERR_INVALID_HEADER,
						"relocatable Entity Property multi-lookup keys must be strictly ordered");
			}
			previousDirectoryId = directoryId;
		}

		LocationDirectoryRangeCursor cursor = openRangeCursor(
				superblock, MetadataDirectoryType::MDT_ENTITY_PROPERTY, keys.front().directoryId(),
				keys.back().directoryId(), cancellation);
		cursor.implementation_->requested_property_keys = keys;
		std::optional<LocationLeafEntry> pending;
		for (size_t index = 0U; index < keys.size(); ++index) {
			const uint64_t requested = keys[index].directoryId();
			if (pending.has_value() && pending->logical_id < requested) {
				pending.reset();
			}
			if (!pending.has_value()) {
				pending = cursor.nextAtOrAfter(requested);
			}
			if (!pending.has_value()) {
				break;
			}
			if (pending->logical_id != requested) {
				continue;
			}
			results[index] = EntityPropertyLocation{
					keys[index], PageId(pending->segment_id.value()), pending->first_page,
					static_cast<uint32_t>(pending->generation.value())};
			pending.reset();
		}
	}

} // namespace graph::storage
