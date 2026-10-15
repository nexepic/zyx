/**
 * @file RelocatableIntegrityScanner.cpp
 * @brief Read-only cross-directory integrity verification for relocatable storage.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/detail/PropertyDecodeWorkspace.hpp"

#include "graph/storage/relocatable/maintenance/RelocatableIntegrityScanner.hpp"

#include <array>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

#include "graph/core/Edge.hpp"
#include "graph/core/Node.hpp"
#include "graph/storage/relocatable/adjacency/AdjacencyKeyCodec.hpp"
#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"
#include "graph/storage/relocatable/allocation/SharedPagePoolAllocator.hpp"
#include "graph/storage/relocatable/allocation/SlottedPage.hpp"
#include "graph/storage/relocatable/entity/EntityLabelSet.hpp"
#include "graph/storage/relocatable/entity/EntityPage.hpp"
#include "graph/storage/relocatable/index/BTreePage.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/metadata/StateRecord.hpp"
#include "graph/storage/relocatable/property/LargeValue.hpp"
#include "graph/storage/relocatable/property/LargeValueStore.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRoot.hpp"
#include "graph/storage/relocatable/vector/VectorAdjacency.hpp"
#include "graph/storage/relocatable/vector/VectorAdjacencyMapping.hpp"
#include "graph/storage/relocatable/vector/VectorAdjacencyPageStore.hpp"
#include "graph/storage/relocatable/vector/VectorCodebookStore.hpp"
#include "graph/storage/relocatable/vector/VectorDeltaMapping.hpp"
#include "graph/storage/relocatable/vector/VectorManifestPage.hpp"
#include "graph/storage/relocatable/vector/VectorNodeMapping.hpp"
#include "graph/storage/relocatable/vector/VectorPage.hpp"
#include "graph/storage/relocatable/vector/VectorSemanticCodec.hpp"
#include "graph/utils/ChecksumUtils.hpp"
#include "graph/utils/OwnedPmrAllocator.hpp"

namespace graph::storage {

	namespace {

        template<class Key, class Value>
        using ScanMap = std::map<Key, Value, std::less<Key>,
            utils::OwnedPmrAllocator<std::pair<const Key, Value>>>;
        template<class Key>
        using ScanSet = std::set<Key, std::less<Key>, utils::OwnedPmrAllocator<Key>>;

		[[noreturn]] void throwIntegrityError(const char *message) {
			throw FormatException(FormatError::FERR_INVALID_STATE, message);
		}

		void requireIntegrity(bool condition, const char *message) {
			if (!condition) {
				throwIntegrityError(message);
			}
		}

		[[nodiscard]] bool isSharedPool(SegmentKind kind) {
			switch (kind) {
				case SegmentKind::SGK_ENTITY_PAGE_POOL:
				case SegmentKind::SGK_PROPERTY_PAGE_POOL:
				case SegmentKind::SGK_INDEX_PAGE_POOL:
				case SegmentKind::SGK_VECTOR_RAW_POOL:
				case SegmentKind::SGK_VECTOR_PQ_POOL:
				case SegmentKind::SGK_VECTOR_ADJACENCY_POOL:
				case SegmentKind::SGK_VECTOR_MANIFEST_POOL:
				case SegmentKind::SGK_STATE_PAGE_POOL:
				case SegmentKind::SGK_LARGE_VALUE_DATA_POOL:
				case SegmentKind::SGK_LARGE_VALUE_MANIFEST_POOL:
					return true;
				default:
					return false;
			}
		}

		[[nodiscard]] bool hasExpectedPageOwner(SegmentKind poolKind, PageOwnerKind ownerKind) {
			switch (poolKind) {
			case SegmentKind::SGK_ENTITY_PAGE_POOL:
				return ownerKind == PageOwnerKind::POK_ENTITY || ownerKind == PageOwnerKind::POK_ENTITY_LABEL;
				case SegmentKind::SGK_PROPERTY_PAGE_POOL:
					return ownerKind == PageOwnerKind::POK_PROPERTY;
				case SegmentKind::SGK_INDEX_PAGE_POOL:
					return ownerKind == PageOwnerKind::POK_INDEX;
				case SegmentKind::SGK_VECTOR_RAW_POOL:
					return ownerKind == PageOwnerKind::POK_VECTOR_RAW;
				case SegmentKind::SGK_VECTOR_PQ_POOL:
					return ownerKind == PageOwnerKind::POK_VECTOR_PQ;
				case SegmentKind::SGK_VECTOR_ADJACENCY_POOL:
					return ownerKind == PageOwnerKind::POK_VECTOR_ADJACENCY;
				case SegmentKind::SGK_VECTOR_MANIFEST_POOL:
					return ownerKind == PageOwnerKind::POK_VECTOR_MANIFEST;
				case SegmentKind::SGK_STATE_PAGE_POOL:
					return ownerKind == PageOwnerKind::POK_STATE;
				case SegmentKind::SGK_LARGE_VALUE_DATA_POOL:
					return ownerKind == PageOwnerKind::POK_LARGE_VALUE_DATA;
				case SegmentKind::SGK_LARGE_VALUE_MANIFEST_POOL:
					return ownerKind == PageOwnerKind::POK_LARGE_VALUE_MANIFEST;
				default:
					return false;
			}
		}

		using StableSlotKey = std::tuple<uint64_t, uint16_t, uint32_t>;

		[[nodiscard]] StableSlotKey stableSlotKey(PageId pageId, uint16_t slotId, uint32_t generation) {
			return {pageId.value(), slotId, generation};
		}

		[[nodiscard]] bool hasNativeEntityPropertyRootMagic(std::span<const std::byte> bytes) {
			return bytes.size() >= sizeof(uint32_t) &&
				   LittleEndianCodec::readU32(bytes, 0U) == RSS_ENTITY_PROPERTY_COLLECTION_ROOT_MAGIC;
		}

		[[nodiscard]] EntityPropertyKey entityPropertyKeyForRoot(const PropertyCollectionRoot &root) {
			const EntityPropertyOwnerKind ownerKind = root.owner_entity_type == Node::typeId
													  ? EntityPropertyOwnerKind::EPOK_NODE
													  : EntityPropertyOwnerKind::EPOK_EDGE;
			// Both Entity Property codecs validate the owner kind and identity before
			// returning a root, so only the cross-directory conversion belongs here.
			return {ownerKind, root.owner_entity_id};
		}

		void validateNativeNodeLabels(const NodeEntityRecord &node, std::span<const uint64_t> overflow) {
			// EntityPage and EntityLabelSet codecs independently validate count,
			// nonzero IDs, and strict ordering. The scanner owns only the relationship
			// between the two otherwise-valid sequences.
			if (!overflow.empty()) {
				requireIntegrity(node.inline_label_ids[node.inline_label_count - 1U] < overflow.front(),
								 "relocatable integrity scan found non-canonical combined Node labels");
			}
		}

		[[nodiscard]] bool isFullSegmentExtent(const ExtentLocation &location) {
			return (static_cast<uint8_t>(location.first_page == RSS_PAGE_POOL_FIRST_DATA_FRAME) &
					static_cast<uint8_t>(location.page_count == RSS_PAGE_POOL_DATA_FRAME_COUNT)) != 0U;
		}

		[[nodiscard]] PageOwnerKind vectorPageOwnerKind(VectorStorageKind kind) {
			// VectorManifestPage::open verifies this enum before the scanner reaches
			// a manifest entry, leaving only the two persisted values here.
			return kind == VectorStorageKind::VSK_RAW ? PageOwnerKind::POK_VECTOR_RAW : PageOwnerKind::POK_VECTOR_PQ;
		}

		struct ScannedPage {
			PageLocation location;
			CatalogEntry pool;
			PageOwnerHeader owner;
			utils::OwnedVector<std::byte> payload;
		};

		[[nodiscard]] bool lessKey(std::span<const std::byte> left, std::span<const std::byte> right) {
			return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end());
		}

		[[nodiscard]] uint64_t decodeCanonicalU64Key(std::span<const std::byte> key, const char *message) {
			if (key.size() != sizeof(uint64_t)) {
				throwIntegrityError(message);
			}
			uint64_t value = 0U;
			for (const std::byte byte: key) {
				value = (value << 8U) | std::to_integer<uint8_t>(byte);
			}
			return value;
		}

		void appendBigEndianU16(utils::OwnedVector<std::byte> &bytes, uint16_t value) {
			bytes.push_back(static_cast<std::byte>(value >> 8U));
			bytes.push_back(static_cast<std::byte>(value));
		}

		void appendBigEndianU64(utils::OwnedVector<std::byte> &bytes, uint64_t value) {
			for (int shift = 56; shift >= 0; shift -= 8) {
				bytes.push_back(static_cast<std::byte>(value >> shift));
			}
		}

		[[nodiscard]] utils::OwnedVector<std::byte> largeValueContentIndexKey(const LargeValueRef &reference) {
			// Routed references originate from validated Manifest codecs. Re-encoding
			// their complete identity here must not duplicate those local checks.
			utils::OwnedVector<std::byte> key;
			key.reserve(1U + sizeof(uint16_t) * 2U + sizeof(uint64_t) + reference.content_hash.size() +
						reference.media_type.size() + sizeof(uint64_t));
			key.push_back(std::byte{1U});
			appendBigEndianU16(key, static_cast<uint16_t>(reference.kind));
			appendBigEndianU16(key, static_cast<uint16_t>(reference.media_type.size()));
			appendBigEndianU64(key, reference.logical_bytes);
			key.insert(key.end(), reference.content_hash.begin(), reference.content_hash.end());
			key.insert(key.end(), reinterpret_cast<const std::byte *>(reference.media_type.data()),
					   reinterpret_cast<const std::byte *>(reference.media_type.data() + reference.media_type.size()));
			appendBigEndianU64(key, reference.value_id.value());
			return key;
		}

		struct BTreeScanWorkItem {
			PageId page_id{};
			uint16_t expected_level = 0;
			std::optional<utils::OwnedVector<std::byte>> lower_inclusive;
			std::optional<utils::OwnedVector<std::byte>> upper_exclusive;
		};

		[[nodiscard]] utils::OwnedVector<BTreePageEntry>
		scanIndexTree(const ScanMap<uint64_t, ScannedPage> &pages, const IndexRootLocation &root,
				  size_t maximumEntries, std::optional<BTreeLeafValueLayout> expectedLeafValueLayout = std::nullopt) {
			utils::OwnedVector<BTreePageEntry> result;
			utils::OwnedVector<BTreePageHeader> leaves;
			ScanSet<PageId> visited;
			// The full Index Root Directory pass below has already fail-closed on an
			// absent root page before any manifest mapping reaches this helper.
			const ScannedPage &rootPage = pages.at(root.root_page_id.value());
			const BTreePage rootNode = BTreePage::open(rootPage.owner, rootPage.payload);
			const BTreePageHeader rootHeader = rootNode.header();
			if (rootHeader.left_sibling.isValid() || rootHeader.right_sibling.isValid()) {
				throwIntegrityError(
						"relocatable integrity scan found an adjacency mapping root with sibling links");
			}

			utils::OwnedVector<BTreeScanWorkItem> work;
			work.push_back({root.root_page_id, rootHeader.level, std::nullopt, std::nullopt});
			while (!work.empty()) {
				BTreeScanWorkItem item = std::move(work.back());
				work.pop_back();
				if (!visited.insert(item.page_id).second) {
					throwIntegrityError("relocatable integrity scan found a cycle in an adjacency mapping tree");
				}
				const auto page = pages.find(item.page_id.value());
				if (page == pages.end()) {
					throwIntegrityError(
							"relocatable integrity scan found an adjacency mapping child outside an Index page");
				}
				if (page->second.owner.kind != PageOwnerKind::POK_INDEX ||
					page->second.owner.owner_id != root.index_id) {
					throwIntegrityError("relocatable integrity scan found an Index tree child with foreign ownership");
				}
				const BTreePage current = BTreePage::open(page->second.owner, page->second.payload);
				const BTreePageHeader header = current.header();
				if (header.level != item.expected_level) {
					throwIntegrityError(
							"relocatable integrity scan found an adjacency mapping child with mismatched level");
				}
				const utils::OwnedVector<BTreePageEntry> entries = current.entries();
				for (const BTreePageEntry &entry: entries) {
					const uint8_t outsideRange = static_cast<uint8_t>(item.lower_inclusive.has_value() &&
																	  lessKey(entry.key, *item.lower_inclusive)) |
												 static_cast<uint8_t>(item.upper_exclusive.has_value() &&
																	  !lessKey(entry.key, *item.upper_exclusive));
					if (outsideRange != 0U) {
						throwIntegrityError("relocatable integrity scan found an adjacency mapping key outside its "
											"separator range");
					}
				}
				if (header.kind == BTreeNodeKind::BPN_LEAF) {
					if (expectedLeafValueLayout.has_value() &&
						header.leaf_value_layout != *expectedLeafValueLayout) {
						throwIntegrityError("relocatable integrity scan found an Index tree with an unexpected leaf layout");
					}
					for (const BTreePageEntry &entry: entries) {
						if (result.size() == maximumEntries) {
							throwIntegrityError("relocatable integrity scan found too many adjacency mapping entries");
						}
						result.push_back(entry);
					}
					leaves.push_back(header);
					continue;
				}
				for (size_t childOrdinal = entries.size() + 1U; childOrdinal > 0U; --childOrdinal) {
					const size_t ordinal = childOrdinal - 1U;
					const PageId child =
							ordinal == 0U ? header.leftmost_child : PageId(entries[ordinal - 1U].value_or_child);
					std::optional<utils::OwnedVector<std::byte>> lower = item.lower_inclusive;
					std::optional<utils::OwnedVector<std::byte>> upper = item.upper_exclusive;
					if (ordinal != 0U) {
						lower = entries[ordinal - 1U].key;
					}
					if (ordinal < entries.size()) {
						upper = entries[ordinal].key;
					}
					work.push_back(
							{child, static_cast<uint16_t>(header.level - 1U), std::move(lower), std::move(upper)});
				}
			}
			for (size_t ordinal = 0; ordinal < leaves.size(); ++ordinal) {
				const BTreePageHeader &leaf = leaves[ordinal];
				const PageId expectedLeft = ordinal == 0U ? PageId{} : leaves[ordinal - 1U].page_id;
				const PageId expectedRight = ordinal + 1U == leaves.size() ? PageId{} : leaves[ordinal + 1U].page_id;
				if (leaf.left_sibling != expectedLeft || leaf.right_sibling != expectedRight) {
					throwIntegrityError("relocatable integrity scan found non-reciprocal adjacency mapping leaf links");
				}
			}
			return result;
		}

	} // namespace

	RelocatableIntegrityScanner::RelocatableIntegrityScanner(std::shared_ptr<StorageIO> io,
															 RelocatableMetadataStore &metadata,
            std::shared_ptr<std::pmr::memory_resource> workspaceOwner) :
        workspace_owner_(std::move(workspaceOwner)), io_(std::move(io)), metadata_(metadata) {
		if (!io_ || !metadata_.usesDataStorage(io_)) {
			throw std::invalid_argument("relocatable integrity scanner requires the metadata data StorageIO");
		}
	}

	RelocatableIntegrityReport RelocatableIntegrityScanner::scan() const {
        // The owner outlives every workspace and decoded owned payload. Legacy
        // standalone callers retain their existing explicit allocation scope.
        std::optional<utils::OwnedAllocationScope> allocationScope;
        if (workspace_owner_) allocationScope.emplace(workspace_owner_);
        StorageIO::SequentialReadScope sequentialReads(*io_);
		const MetadataSnapshotPin snapshot = metadata_.openSnapshot();
		const Superblock &superblock = snapshot.superblock();
		RelocatableIntegrityReport report{superblock.generation, superblock.checkpoint_lsn};
		ScanMap<uint64_t, CatalogEntry> catalog;
		metadata_.catalog_.visit(superblock, [this, &superblock, &report, &catalog](const CatalogEntry &entry) {
			requireIntegrity(catalog.emplace(entry.segment_id.value(), entry).second,
							 "relocatable integrity scan found duplicate Catalog Segment IDs");
			++report.catalog_entries;
			// Scratch is transaction-private and deliberately has no public root.
			// A read-only integrity scan cannot know whether its owner is still alive,
			// so a sealed scratch entry is recovery-required rather than silently
			// counted as healthy graph storage.
			if ((entry.kind == SegmentKind::SGK_BULK_SCRATCH || entry.kind == SegmentKind::SGK_QUERY_SCRATCH) &&
				entry.state == SegmentState::SGS_SEALED) {
				throwIntegrityError("relocatable integrity scan found private scratch requiring owner quiescence or writable recovery");
			}
			if ((entry.kind == SegmentKind::SGK_LARGE_VALUE_DATA_POOL &&
				 entry.owner_id != RSS_LARGE_VALUE_SHARED_DATA_POOL_OWNER_ID) ||
				(entry.kind == SegmentKind::SGK_LARGE_VALUE_MANIFEST_POOL &&
				 entry.owner_id != RSS_LARGE_VALUE_SHARED_MANIFEST_POOL_OWNER_ID)) {
				throwIntegrityError("relocatable integrity scan found a LargeValue pool outside its shared namespace");
			}
			if (entry.state == SegmentState::SGS_SEALED) {
				const uint8_t unavailable =
						static_cast<uint8_t>(!metadata_.allocator_.isAllocated(superblock, entry.physical_slot));
				if (unavailable != 0U) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "relocatable integrity scan found an unavailable sealed Catalog Segment ID " +
												  std::to_string(entry.segment_id.value()) + " at physical slot " +
												  std::to_string(entry.physical_slot.value()));
				}
				++report.sealed_segments;
			}
		});
		ScanSet<uint64_t> retiredSegments;
		metadata_.locations_.visitRetireQueue(superblock, [this, &superblock, &catalog, &report,
													   &retiredSegments](const RetireQueueEntry &entry) {
			const auto current = catalog.find(entry.segment_id.value());
			// The Retire Queue LocationLeaf codec already rejects zero identities,
			// generations, and unknown dispositions. The scanner owns only the
			// relationships to the current Catalog, allocator, and snapshot generation.
			const uint8_t invalid =
					static_cast<uint8_t>(entry.retired_generation > superblock.generation) |
					static_cast<uint8_t>(current == catalog.end()) |
					static_cast<uint8_t>(current != catalog.end() &&
										 entry.retired_generation < current->second.generation) |
					static_cast<uint8_t>(!metadata_.allocator_.isAllocated(superblock, entry.physical_slot));
			if (invalid != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable integrity scan found an invalid Retire Queue entry");
			}
			if (entry.disposition == RetireQueueDisposition::RQD_RELOCATED_SOURCE) {
				requireIntegrity(current->second.physical_slot != entry.physical_slot &&
									 current->second.state != SegmentState::SGS_OBSOLETE,
							 "relocatable integrity scan found a current Catalog mapping for a relocated source");
			} else {
				requireIntegrity(current->second.state == SegmentState::SGS_RETIRED &&
									 current->second.physical_slot == entry.physical_slot &&
									 retiredSegments.insert(entry.segment_id.value()).second,
							 "relocatable integrity scan found an inconsistent retired Segment queue entry");
			}
			++report.queued_retired_slots;
		});

		SharedPagePoolAllocator pages(io_, metadata_, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
		PhysicalSegmentAllocator extents(io_, metadata_);
		LargeValueStore largeValues(io_, metadata_, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
		VectorCodebookStore codebooks(extents, metadata_);
		ScanMap<uint64_t, ScannedPage> scannedPages;
		ScanMap<uint64_t, VectorManifestLocation> scannedManifests;
		ScanMap<uint64_t, VectorCodebookHeader> scannedCodebooks;
		ScanMap<uint64_t, VectorCodebookExtentHeader> scannedCodebookExtents;
		ScanMap<uint64_t, LargeValueManifest> scannedLargeValueManifests;
		ScanMap<uint64_t, LargeValueDataPage> scannedLargeValueDataPages;
		ScanMap<uint64_t, StateLocation> scannedStates;
		ScanMap<uint64_t, VectorPageHeader> vectorPages;
		ScanMap<uint64_t, ScanSet<uint64_t>> rawVectorIdsByIndex;
		ScanMap<StableSlotKey, std::pair<EntityPageRecordKind, uint64_t>> scannedEntitySlots;
		ScanMap<StableSlotKey, utils::OwnedVector<uint64_t>> scannedLabelSlots;
		ScanMap<StableSlotKey, EntityPropertyKey> scannedEntityPropertySlots;
		// Node and Edge logical IDs share the directory type with the native
		// Entity Map. Validate their targets below instead of rejecting the root
		// outright: a clean native database necessarily has entries here.
		metadata_.locations_.visitPages(superblock, [&](const PageLocation &location) {
			const auto catalogEntry = catalog.find(location.segment_id.value());
			requireIntegrity(catalogEntry != catalog.end(),
							 "relocatable integrity scan found a Page Directory target missing from the Catalog");
			const uint8_t invalidPool = static_cast<uint8_t>(catalogEntry->second.state != SegmentState::SGS_SEALED) |
										static_cast<uint8_t>(!isSharedPool(catalogEntry->second.kind)) |
										static_cast<uint8_t>(catalogEntry->second.generation != location.generation);
			if (invalidPool != 0U) {
				throwIntegrityError(
						"relocatable integrity scan found a Page Directory target outside its current shared pool");
			}
			const SharedPagePoolContents contents = pages.read(snapshot, location.page_id);
			const uint8_t invalidFrame =
					static_cast<uint8_t>(contents.location != location) |
					static_cast<uint8_t>(contents.owner.flags != 0U) |
					static_cast<uint8_t>(!hasExpectedPageOwner(catalogEntry->second.kind, contents.owner.kind));
			if (invalidFrame != 0U) {
				throwIntegrityError("relocatable integrity scan found a stale or retired current page frame");
			}
			// LocationDirectory route validation gives every logical PageId exactly one
			// leaf position, so a second insertion cannot be produced by a decoded tree.
			static_cast<void>(scannedPages.emplace(
					location.page_id.value(), ScannedPage{location, contents.pool, contents.owner, contents.payload}));
			++report.page_locations;
			switch (contents.owner.kind) {
				case PageOwnerKind::POK_ENTITY: {
					const uint8_t invalidOwner =
							static_cast<uint8_t>(contents.owner.owner_id != catalogEntry->second.owner_id) |
							static_cast<uint8_t>(location.free_bytes != 0U);
					if (invalidOwner != 0U) {
						throwIntegrityError("relocatable integrity scan found an Entity page with stale owner or "
											"free-space metadata");
					}
					const EntityPage page = EntityPage::open(contents.owner, contents.payload);
					const EntityPageHeader header = page.header();
					for (uint32_t rawSlot = 0U; rawSlot < header.slot_capacity; ++rawSlot) {
						const uint16_t slot = static_cast<uint16_t>(rawSlot);
						const bool live = header.record_kind == EntityPageRecordKind::EPRK_NODE
												  ? page.findNode(slot).has_value()
												  : page.findEdge(slot).has_value();
						if (live) {
							// A typed Entity page enumerates each slot ordinal once and PageId
							// uniqueness was established by the Location Directory above.
							static_cast<void>(scannedEntitySlots.emplace(
									stableSlotKey(location.page_id, slot, page.slotGeneration(slot)),
									std::pair{header.record_kind, page.entityId(slot)}));
						}
					}
					break;
				}
				case PageOwnerKind::POK_ENTITY_LABEL: {
					if (contents.owner.owner_id != catalogEntry->second.owner_id) {
						throwIntegrityError(
								"relocatable integrity scan found a LabelSet page with a foreign Entity-pool owner");
					}
					const SlottedPage page = SlottedPage::open(contents.owner, contents.payload);
					if (page.stats().contiguous_free_bytes != location.free_bytes) {
						throwIntegrityError("relocatable integrity scan found stale LabelSet page free-space metadata");
					}
					for (const SlottedSlotReference &reference: page.liveSlots()) {
						const utils::OwnedVector<uint64_t> labels = EntityLabelSetCodec::decode(page.read(reference));
						static_cast<void>(scannedLabelSlots.emplace(
								stableSlotKey(reference.page_id, reference.slot_id, reference.generation), labels));
					}
					break;
				}
				case PageOwnerKind::POK_PROPERTY: {
					const SlottedPage page = SlottedPage::open(contents.owner, contents.payload);
					if (page.stats().contiguous_free_bytes != location.free_bytes) {
						throwIntegrityError("relocatable integrity scan found stale slotted page free-space metadata");
					}
					for (const SlottedSlotReference &reference: page.liveSlots()) {
                        detail::PropertyDecodeWorkspace propertyWorkspace(workspace_owner_);
						const utils::OwnedVector<std::byte> bytes = page.read(reference);
						PropertyCollectionRoot root;
						if (hasNativeEntityPropertyRootMagic(bytes)) {
							root = EntityPropertyCollectionRootCodec::decode(bytes);
						} else if (EntityPropertyLargeValueReferenceCodec::hasMagic(bytes)) {
							const EntityPropertyLargeValueReference largeValueReference =
									EntityPropertyLargeValueReferenceCodec::decode(bytes);
							const PropertyCollectionRoot referenceOwner{
									largeValueReference.owner_entity_id, largeValueReference.owner_entity_type, {}};
							const EntityPropertyKey referenceKey = entityPropertyKeyForRoot(referenceOwner);
							const uint8_t invalidReference =
									static_cast<uint8_t>(largeValueReference.value.kind != LargeValueKind::LVK_BYTES) |
									static_cast<uint8_t>(largeValueReference.value.media_type !=
													 RSS_ENTITY_PROPERTY_LARGE_VALUE_MEDIA_TYPE);
							if (invalidReference != 0U) {
								throwIntegrityError("relocatable integrity scan found an Entity Property LargeValue reference "
													"with an invalid content type");
							}
							const std::vector<std::byte> payload = largeValues.read(snapshot, largeValueReference.value);
							if (EntityPropertyLargeValuePayloadCodec::hasMagic(payload)) {
								root.owner_entity_id = largeValueReference.owner_entity_id;
								root.owner_entity_type = largeValueReference.owner_entity_type;
								root.entries = EntityPropertyLargeValuePayloadCodec::decode(payload);
							} else {
								root = EntityPropertyCollectionRootCodec::decode(payload);
								const EntityPropertyKey resolvedKey = entityPropertyKeyForRoot(root);
								if (resolvedKey != referenceKey) {
									throwIntegrityError("relocatable integrity scan found an Entity Property LargeValue root "
															 "with mismatched ownership");
								}
							}
						} else {
							throwIntegrityError(
									"relocatable integrity scan found a retired or malformed Entity Property payload");
						}
						const EntityPropertyKey key = entityPropertyKeyForRoot(root);
						static_cast<void>(scannedEntityPropertySlots.emplace(
								stableSlotKey(reference.page_id, reference.slot_id, reference.generation), key));
					}
					break;
				}
				case PageOwnerKind::POK_STATE: {
					const SlottedPage page = SlottedPage::open(contents.owner, contents.payload);
					if (page.stats().contiguous_free_bytes != location.free_bytes) {
						throwIntegrityError("relocatable integrity scan found stale State page free-space metadata");
					}
					for (const SlottedSlotReference &reference: page.liveSlots()) {
						const StateRecord record = StateRecordCodec::decode(page.read(reference));
						const StateLocation state{record.state_id, reference.page_id, reference.slot_id,
												  reference.generation};
						if (!scannedStates.emplace(record.state_id, state).second) {
							throwIntegrityError(
									"relocatable integrity scan found duplicate State IDs across State pages");
						}
					}
					break;
				}
				case PageOwnerKind::POK_INDEX: {
					const BTreePage page = BTreePage::open(contents.owner, contents.payload);
					if (page.slottedPage().stats().contiguous_free_bytes != location.free_bytes) {
						throwIntegrityError("relocatable integrity scan found stale Index page free-space metadata");
					}
					break;
				}
				case PageOwnerKind::POK_VECTOR_RAW:
				case PageOwnerKind::POK_VECTOR_PQ: {
					const VectorPage page = VectorPage::open(contents.owner, contents.payload);
					const VectorPageHeader header = page.header();
					const bool raw = contents.owner.kind == PageOwnerKind::POK_VECTOR_RAW;
					const uint32_t expectedFreeBytes =
							static_cast<uint32_t>(header.slot_capacity - header.live_count) * header.record_bytes;
					const uint8_t invalidHeader =
							static_cast<uint8_t>(header.page_id != location.page_id) |
							static_cast<uint8_t>(header.index_id != contents.owner.owner_id) |
							static_cast<uint8_t>(header.generation != location.generation) |
							static_cast<uint8_t>(location.free_bytes != expectedFreeBytes) |
							static_cast<uint8_t>(raw != (header.kind == VectorStorageKind::VSK_RAW));
					if (invalidHeader != 0U) {
						throwIntegrityError("relocatable integrity scan found a VectorPage owner or kind mismatch");
					}
					vectorPages.emplace(location.page_id.value(), header);
					break;
				}
				case PageOwnerKind::POK_VECTOR_ADJACENCY: {
					const uint8_t invalidOwner =
							static_cast<uint8_t>(contents.pool.owner_id == 0U) |
							static_cast<uint8_t>((contents.pool.owner_id & RSS_DERIVED_ADJACENCY_OWNER_MASK) != 0U) |
							static_cast<uint8_t>(contents.owner.owner_id !=
														 (contents.pool.owner_id | RSS_DERIVED_ADJACENCY_OWNER_MASK));
					if (invalidOwner != 0U) {
						throwIntegrityError(
								"relocatable integrity scan found an adjacency page with invalid owner identity");
					}
					const SlottedPage page = SlottedPage::open(contents.owner, contents.payload);
					if (page.stats().contiguous_free_bytes != location.free_bytes) {
						throwIntegrityError(
								"relocatable integrity scan found stale adjacency page free-space metadata");
					}
					uint64_t previousVectorId = 0U;
					for (const SlottedSlotReference &reference: page.liveSlots()) {
						const VectorAdjacencyRecord record = VectorAdjacencyCodec::decode(page.read(reference));
						if (record.vector_id <= previousVectorId) {
							throwIntegrityError(
									"relocatable integrity scan found unordered adjacency records in one page");
						}
						previousVectorId = record.vector_id;
					}
					break;
				}
				case PageOwnerKind::POK_VECTOR_MANIFEST: {
					const VectorManifestPage page = VectorManifestPage::open(contents.owner, contents.payload);
					const VectorManifest &manifest = page.manifest();
					const VectorManifestLocation manifestLocation{manifest.index_id, manifest.page_id,
																  manifest.generation};
					if (location.free_bytes != 0U) {
						throwIntegrityError(
								"relocatable integrity scan found stale Vector Manifest page free-space metadata");
					}
					if (!scannedManifests.emplace(manifest.index_id, manifestLocation).second) {
						throwIntegrityError(
								"relocatable integrity scan found duplicate Vector Manifest index ownership");
					}
					break;
				}
				case PageOwnerKind::POK_LARGE_VALUE_MANIFEST: {
					if (location.free_bytes != 0U) {
						throwIntegrityError(
								"relocatable integrity scan found stale LargeValue Manifest page free-space metadata");
					}
					const LargeValueManifest manifest =
							LargeValueManifestPage::open(contents.owner, contents.payload).manifest();
					static_cast<void>(scannedLargeValueManifests.emplace(manifest.page_id.value(), manifest));
					break;
				}
				case PageOwnerKind::POK_LARGE_VALUE_DATA: {
					if (location.free_bytes != 0U) {
						throwIntegrityError(
								"relocatable integrity scan found stale LargeValue data page free-space metadata");
					}
					const LargeValueDataPage data = LargeValueDataFrame::open(contents.owner, contents.payload).page();
					static_cast<void>(scannedLargeValueDataPages.emplace(data.page_id.value(), data));
					break;
				}
				default: // ZYX_COV_EXCL_LINE: invalidFrame proves the typed owner matches its validated pool kind.
					throwIntegrityError("relocatable integrity scan found an unsupported shared-page owner kind");
			}
		});

		struct LargeValueNodeSpan {
			uint32_t chunk_count = 0U;
			uint64_t logical_bytes = 0U;
		};
		ScanMap<uint64_t, LargeValueRef> routedLargeValueReferences;
		ScanSet<uint64_t> reachableLargeValueManifestPages;
		ScanSet<uint64_t> reachableLargeValueDataPages;
		metadata_.visitLargeValueManifestLocations(snapshot, [&](const LargeValueManifestLocation &route) {
			const auto rootPage = scannedPages.find(route.manifest_page_id.value());
			const auto root = scannedLargeValueManifests.find(route.manifest_page_id.value());
			// LocationLeaf decoding owns route-field validity. Page/Manifest codecs
			// bind a decoded manifest PageId to its Page Directory key, leaving only
			// cross-object reachability, generation, identity, and root shape here.
			const uint8_t invalidRoute = static_cast<uint8_t>(rootPage == scannedPages.end()) |
					static_cast<uint8_t>(root == scannedLargeValueManifests.end()) |
					static_cast<uint8_t>(rootPage != scannedPages.end() &&
										 rootPage->second.location.generation != route.generation) |
					static_cast<uint8_t>(root != scannedLargeValueManifests.end() &&
										 (root->second.value_id != route.value_id ||
										  root->second.node_kind != LargeValueManifestNodeKind::LVMNK_ROOT));
			if (invalidRoute != 0U) {
				throwIntegrityError("relocatable integrity scan found an invalid LargeValue Manifest directory route");
			}

			const LargeValueRef rootReference = root->second.reference();
			routedLargeValueReferences.emplace(rootReference.value_id.value(), rootReference);
			std::function<LargeValueNodeSpan(const LargeValueManifest &)> scanNode;
			scanNode = [&](const LargeValueManifest &node) -> LargeValueNodeSpan {
				// Typed manifests require every child level to be exactly one below
				// its parent, so a cycle is structurally impossible after decoding.
				reachableLargeValueManifestPages.insert(node.page_id.value());
				const uint64_t nodeLogicalBytes = node.isRoot() ? node.logical_bytes : node.node_logical_bytes;
				if (node.isLeaf()) {
					if (nodeLogicalBytes != 0U && node.chunks.empty()) {
						throwIntegrityError(
								"relocatable integrity scan found a LargeValue leaf without canonical chunk coverage");
					}
					for (const LargeValueChunk &chunk: node.chunks) {
						for (uint16_t ordinal = 0U; ordinal < chunk.data_page_count; ++ordinal) {
							const PageId pageId(chunk.first_data_page_id.value() + ordinal);
							const auto data = scannedLargeValueDataPages.find(pageId.value());
							const uint8_t invalidData =
									static_cast<uint8_t>(data == scannedLargeValueDataPages.end()) |
									static_cast<uint8_t>(data != scannedLargeValueDataPages.end() &&
														 (data->second.value_id != rootReference.value_id ||
														  data->second.generation != rootReference.generation ||
														  data->second.chunk_ordinal != chunk.ordinal ||
														  data->second.page_ordinal != ordinal ||
														  data->second.page_count != chunk.data_page_count ||
														  data->second.stored_chunk_bytes != chunk.stored_bytes));
							if (invalidData != 0U) {
								throwIntegrityError("relocatable integrity scan found an invalid or multiply "
														"referenced LargeValue data page");
							}
							// Canonical, non-overlapping child/chunk ordinal ranges make a
							// second reference to the same validated data page impossible.
							reachableLargeValueDataPages.insert(pageId.value());
						}
					}
					return {static_cast<uint32_t>(node.chunks.size()), nodeLogicalBytes};
				}

				uint64_t subtreeChunkCount = 0U;
				for (const LargeValueManifestChild &range: node.children) {
					const auto child = scannedLargeValueManifests.find(range.page_id.value());
					const uint8_t invalidChild =
							static_cast<uint8_t>(child == scannedLargeValueManifests.end()) |
							static_cast<uint8_t>(
									child != scannedLargeValueManifests.end() &&
									(child->second.reference() != rootReference ||
									 child->second.node_kind == LargeValueManifestNodeKind::LVMNK_ROOT ||
									 child->second.node_level + 1U != node.node_level ||
									 child->second.first_chunk_ordinal != range.first_chunk_ordinal ||
									 child->second.first_uncompressed_offset != range.uncompressed_offset ||
									 child->second.node_logical_bytes != range.logical_bytes));
					if (invalidChild != 0U) {
						throwIntegrityError("relocatable integrity scan found a LargeValue Manifest child that "
											"disagrees with its parent");
					}
					const LargeValueNodeSpan span = scanNode(child->second);
					if (span.chunk_count != range.chunk_count || span.logical_bytes != range.logical_bytes) {
						throwIntegrityError("relocatable integrity scan found a LargeValue Manifest child with an "
											"invalid subtree range");
					}
					// The parent codec has already validated canonical uint32 child
					// ordinal ranges, including their aggregate overflow bound.
					subtreeChunkCount += span.chunk_count;
				}
				return {static_cast<uint32_t>(subtreeChunkCount), nodeLogicalBytes};
			};
			static_cast<void>(scanNode(root->second));
			++report.large_value_manifest_routes;
		});
		if (reachableLargeValueManifestPages.size() != scannedLargeValueManifests.size()) {
			throwIntegrityError("relocatable integrity scan found an orphan LargeValue Manifest page");
		}
		if (reachableLargeValueDataPages.size() != scannedLargeValueDataPages.size()) {
			throwIntegrityError("relocatable integrity scan found an orphan LargeValue data page");
		}
		report.large_value_manifest_nodes = reachableLargeValueManifestPages.size();
		report.large_value_data_pages = reachableLargeValueDataPages.size();

		const std::optional<IndexRootLocation> contentIndexRoot =
				metadata_.locations_.findIndexRootOptional(superblock, RSS_LARGE_VALUE_CONTENT_INDEX_ID);
		if (!routedLargeValueReferences.empty() && !contentIndexRoot.has_value()) {
			throwIntegrityError("relocatable integrity scan found LargeValue Manifests without their content index");
		}
		if (contentIndexRoot.has_value()) {
			const size_t maximumEntries = routedLargeValueReferences.size() + 1U;
			const utils::OwnedVector<BTreePageEntry> entries = scanIndexTree(scannedPages, *contentIndexRoot, maximumEntries);
			if (entries.size() != routedLargeValueReferences.size()) {
				throwIntegrityError("relocatable integrity scan found an incomplete or orphan LargeValue content index");
			}
			ScanMap<utils::OwnedVector<std::byte>, uint64_t> expectedEntries;
			for (const auto &[valueId, reference]: routedLargeValueReferences) {
				static_cast<void>(valueId);
				expectedEntries.emplace(largeValueContentIndexKey(reference), reference.value_id.value());
			}
			for (const BTreePageEntry &entry: entries) {
				const auto expected = expectedEntries.find(entry.key);
				if (expected == expectedEntries.end() || entry.value_or_child != expected->second) {
					throwIntegrityError("relocatable integrity scan found a LargeValue content-index key or value mismatch");
				}
			}
			report.large_value_content_index_entries = entries.size();
		}

		ScanMap<uint64_t, ExtentLocation> scannedExtents;
		metadata_.locations_.visitExtents(superblock, [&](const ExtentLocation &location) {
			const PhysicalSegmentContents contents = extents.read(snapshot, location.segment_id);
			// A Location Directory route has exactly one entry for an ExtentId.
			const uint8_t invalidExtent =
					static_cast<uint8_t>(contents.entry.state != SegmentState::SGS_SEALED) |
					static_cast<uint8_t>(contents.entry.generation != location.generation) |
					static_cast<uint8_t>(!isFullSegmentExtent(location));
			if (invalidExtent != 0U) {
				throwIntegrityError("relocatable integrity scan found an invalid or duplicate Extent Directory target");
			}
			static_cast<void>(scannedExtents.emplace(location.extent_id.value(), location));
			if (contents.entry.kind == SegmentKind::SGK_CODEBOOK_EXTENT) {
				const VectorCodebookExtentHeader header =
						VectorCodebookExtentHeaderCodec::decode(contents.header_extension);
				const uint8_t invalidCodebook = static_cast<uint8_t>(header.extent_id != location.extent_id) |
												static_cast<uint8_t>(contents.entry.owner_id != header.codebook_id);
				if (invalidCodebook != 0U) {
					throwIntegrityError(
							"relocatable integrity scan found a codebook header that disagrees with its extent");
				}
				static_cast<void>(scannedCodebookExtents.emplace(header.extent_id.value(), header));
				VectorCodebookExtentHeaderCodec::validateBlock(header, contents.payload);
				if (header.kind == VectorCodebookExtentKind::VCEK_MANIFEST) {
					const VectorCodebookManifest manifest = VectorCodebookManifestCodec::decode(contents.payload);
					const uint8_t invalidManifest =
							static_cast<uint8_t>(!VectorCodebookExtentIdCodec::isManifestExtentForCodebook(
									header.extent_id, header.codebook_id)) |
							static_cast<uint8_t>(manifest.header.codebook_id != header.codebook_id) |
							static_cast<uint8_t>(manifest.header.generation != header.codebook_generation);
					if (invalidManifest != 0U) {
						throwIntegrityError("relocatable integrity scan found an invalid or duplicate Vector Codebook "
											"manifest extent");
					}
					static_cast<void>(scannedCodebooks.emplace(header.codebook_id, manifest.header));
				}
			} else {
				throwIntegrityError("relocatable integrity scan found an unsupported Extent Directory segment kind");
			}
			++report.extent_locations;
		});

		ScanSet<uint64_t> referencedCodebookExtents;
		for (const auto &[codebookId, expectedHeader]: scannedCodebooks) {
			const VectorCodebookManifest immutableManifest = codebooks.manifest(snapshot, codebookId);
			codebooks.validate(snapshot, codebookId, expectedHeader.payload_bytes, immutableManifest.entries.size());
			static_cast<void>(referencedCodebookExtents.insert(
					VectorCodebookExtentIdCodec::manifestExtentId(codebookId).value()));
			for (const VectorCodebookManifestEntry &entry: immutableManifest.entries) {
				static_cast<void>(referencedCodebookExtents.insert(entry.extent_id.value()));
			}
		}
		if (referencedCodebookExtents.size() != scannedCodebookExtents.size()) {
			throwIntegrityError("relocatable integrity scan found an orphan Vector Codebook data extent");
		}
		metadata_.locations_.visitStates(superblock, [&](const StateLocation &location) {
			const auto actual = scannedStates.find(location.state_id);
			if (actual == scannedStates.end()) {
				throwIntegrityError(
						"relocatable integrity scan found a State Directory reference to an absent State record");
			}
			if (actual->second != location) {
				throwIntegrityError("relocatable integrity scan found a stale State Directory reference");
			}
			// Exact StateLocation equality above proves the source page and kind: the
			// scanned State map is populated only while decoding POK_STATE pages.
			const auto page = scannedPages.find(location.page_id.value());
			const SlottedPage opened = SlottedPage::open(page->second.owner, page->second.payload);
			const StateRecord record = StateRecordCodec::decode(
					opened.read({location.page_id, location.slot_id, location.slot_generation}));
			if (record.storage_kind == StateRecordStorageKind::SRS_LARGE_VALUE) {
				const auto reference = routedLargeValueReferences.find(record.large_value_id.value());
				if (reference == routedLargeValueReferences.end() || reference->second.kind != LargeValueKind::LVK_BYTES ||
					reference->second.media_type != RSS_STATE_LARGE_VALUE_MEDIA_TYPE ||
					largeValues.manifest(snapshot, record.large_value_id).reference() != reference->second) {
					throwIntegrityError(
							"relocatable integrity scan found a State reference to an absent or non-State LargeValue");
				}
			}
			++report.state_locations;
		});
		if (report.state_locations != scannedStates.size()) {
			throwIntegrityError("relocatable integrity scan found a State record missing from its directory");
		}

		metadata_.locations_.visitIndexRoots(superblock, [&](const IndexRootLocation &root) {
			const auto page = scannedPages.find(root.root_page_id.value());
			requireIntegrity(page != scannedPages.end(),
							 "relocatable integrity scan found an Index Root Directory reference to an absent page");
			const uint8_t invalidRoot = static_cast<uint8_t>(page->second.owner.kind != PageOwnerKind::POK_INDEX) |
										static_cast<uint8_t>(page->second.owner.owner_id != root.index_id) |
										static_cast<uint8_t>(page->second.location.generation != root.root_generation);
			requireIntegrity(invalidRoot == 0U,
							 "relocatable integrity scan found an Index Root Directory reference mismatch");
		});

		ScanMap<uint64_t, EntityPageLocation> nativeNodes;
		ScanMap<uint64_t, EntityPageLocation> nativeEdges;
		ScanSet<StableSlotKey> mappedEntitySlots;
		ScanMap<StableSlotKey, uint64_t> labelSlotOwners;
		const auto validateNativeEntityLocation = [&scannedPages, &scannedEntitySlots, &scannedLabelSlots, &nativeNodes,
														   &nativeEdges, &mappedEntitySlots,
														   &labelSlotOwners](const EntityPageLocation &location,
																								 EntityPageRecordKind expectedKind) {
			const auto page = scannedPages.find(location.page_id.value());
			if (page == scannedPages.end() || page->second.owner.kind != PageOwnerKind::POK_ENTITY) {
				throwIntegrityError(expectedKind == EntityPageRecordKind::EPRK_NODE
												? "relocatable integrity scan found a retired fixed-record Node directory entry"
												: "relocatable integrity scan found a retired fixed-record Edge directory entry");
			}
			const StableSlotKey slotKey = stableSlotKey(location.page_id, location.slot_id, location.slot_generation);
			const auto slot = scannedEntitySlots.find(slotKey);
			// LocationLeaf decoding already requires nonzero EntityId, PageId, and
			// slot generation; this scan owns target liveness and record-kind matching.
			requireIntegrity(slot != scannedEntitySlots.end() && slot->second.first == expectedKind &&
								 slot->second.second == location.entity_id,
							 "relocatable integrity scan found an Entity Map entry that does not resolve to a live "
							 "same-kind slot with the same Entity ID");
			requireIntegrity(
					mappedEntitySlots.insert(slotKey).second,
					"relocatable integrity scan found multiple Entity Map entries for one live Entity page slot");
			ScanMap<uint64_t, EntityPageLocation> &entities =
					expectedKind == EntityPageRecordKind::EPRK_NODE ? nativeNodes : nativeEdges;
			static_cast<void>(entities.emplace(location.entity_id, location));
			if (expectedKind != EntityPageRecordKind::EPRK_NODE) {
				return true;
			}
			const EntityPage entityPage = EntityPage::open(page->second.owner, page->second.payload);
			const std::optional<NodeEntityRecord> node =
					entityPage.findNode(location.slot_id, location.slot_generation, location.entity_id);
			requireIntegrity(node.has_value(),
							 "relocatable integrity scan found an Entity Map Node entry without a live Node record");
			if (node->label_overflow.isEmpty()) {
				validateNativeNodeLabels(*node, {});
				return true;
			}
			const StableSlotKey labelKey = stableSlotKey(node->label_overflow.page_id, node->label_overflow.slot_id,
														 node->label_overflow.slot_generation);
			const auto labels = scannedLabelSlots.find(labelKey);
			requireIntegrity(
					labels != scannedLabelSlots.end(),
					"relocatable integrity scan found a Node LabelSet reference to an absent live LabelSet slot");
			validateNativeNodeLabels(*node, labels->second);
			requireIntegrity(labelSlotOwners.emplace(labelKey, location.entity_id).second,
							 "relocatable integrity scan found one LabelSet slot owned by multiple Nodes");
			return true;
		};
		metadata_.visitNodePageLocations(snapshot, [&validateNativeEntityLocation](const EntityPageLocation &location) {
			static_cast<void>(validateNativeEntityLocation(location, EntityPageRecordKind::EPRK_NODE));
		});
		metadata_.visitEdgePageLocations(snapshot, [&validateNativeEntityLocation](const EntityPageLocation &location) {
			static_cast<void>(validateNativeEntityLocation(location, EntityPageRecordKind::EPRK_EDGE));
		});
		if (mappedEntitySlots.size() != scannedEntitySlots.size()) {
			throwIntegrityError("relocatable integrity scan found a live Entity page slot without an Entity Map entry");
		}
		if (labelSlotOwners.size() != scannedLabelSlots.size()) {
			throwIntegrityError("relocatable integrity scan found a live LabelSet slot without one owning Node");
		}

		ScanSet<StableSlotKey> mappedEntityPropertySlots;
		metadata_.visitEntityPropertyLocations(snapshot, [&](const EntityPropertyLocation &location) {
			const StableSlotKey slotKey = stableSlotKey(location.page_id, location.slot_id, location.slot_generation);
			const auto property = scannedEntityPropertySlots.find(slotKey);
			// EntityPropertyKey validity is a LocationLeaf codec invariant.
			requireIntegrity(property != scannedEntityPropertySlots.end() && property->second == location.key,
							 "relocatable integrity scan found an Entity Property Directory entry that does not "
							 "resolve to its exact root");
			requireIntegrity(
					mappedEntityPropertySlots.insert(slotKey).second,
					"relocatable integrity scan found multiple Entity Property Directory entries for one root");
			const bool ownerExists = location.key.owner_kind == EntityPropertyOwnerKind::EPOK_NODE
											 ? nativeNodes.contains(location.key.entity_id)
											 : nativeEdges.contains(location.key.entity_id);
			requireIntegrity(
					ownerExists,
					"relocatable integrity scan found an Entity Property root without its native Entity Map owner");
		});
		if (mappedEntityPropertySlots.size() != scannedEntityPropertySlots.size()) {
			throwIntegrityError("relocatable integrity scan found an Entity Property root without a directory entry");
		}

		// Ordinary graph adjacency is a key-only derived access path. Validate it
		// against the authoritative Edge records before scanning unrelated derived
		// structures, so an orphan, missing, or one-direction Edge fails closed.
		ScanMap<uint64_t, EdgeEntityRecord> authoritativeEdges;
		for (const auto &[edgeId, location]: nativeEdges) {
			const auto page = scannedPages.find(location.page_id.value());
			requireIntegrity(page != scannedPages.end(),
							 "relocatable integrity scan found an Edge Entity Map entry outside a current page");
			const EntityPage entityPage = EntityPage::open(page->second.owner, page->second.payload);
			const std::optional<EdgeEntityRecord> edge =
					entityPage.findEdge(location.slot_id, location.slot_generation, location.entity_id);
			requireIntegrity(edge.has_value(),
							 "relocatable integrity scan found an Edge Entity Map entry without a live Edge record");
			requireIntegrity(nativeNodes.contains(edge->source_node_id) && nativeNodes.contains(edge->target_node_id),
							 "relocatable integrity scan found an Edge whose endpoint is absent from the Entity Map");
			requireIntegrity(authoritativeEdges.emplace(edgeId, *edge).second,
							 "relocatable integrity scan found duplicate authoritative Edge identity");
		}
		const std::optional<IndexRootLocation> adjacencyRoot =
				metadata_.locations_.findIndexRootOptional(superblock, RSS_GLOBAL_ADJACENCY_INDEX_ID);
		if (authoritativeEdges.empty()) {
			requireIntegrity(!adjacencyRoot.has_value(),
							 "relocatable integrity scan found an empty global adjacency tree");
		} else {
			requireIntegrity(adjacencyRoot.has_value(),
							 "relocatable integrity scan found live Edges without the global adjacency tree");
		}
		if (adjacencyRoot.has_value()) {
			if (authoritativeEdges.size() > std::numeric_limits<size_t>::max() / 2U) {
				throwIntegrityError("relocatable integrity scan cannot bound global adjacency entry validation");
			}
			const size_t expectedEntryCount = authoritativeEdges.size() * 2U;
			const utils::OwnedVector<BTreePageEntry> entries =
					scanIndexTree(scannedPages, *adjacencyRoot, expectedEntryCount, BTreeLeafValueLayout::BLVL_EMPTY);
			requireIntegrity(entries.size() == expectedEntryCount,
							 "relocatable integrity scan found an incomplete global adjacency tree");
			ScanSet<utils::OwnedVector<std::byte>> expectedKeys;
			for (const auto &[edgeId, edge]: authoritativeEdges) {
				static_cast<void>(expectedKeys.insert(
						AdjacencyKeyCodec::encode({GlobalAdjacencyDirection::GAD_OUT, edge.source_node_id, edgeId})));
				static_cast<void>(expectedKeys.insert(
						AdjacencyKeyCodec::encode({GlobalAdjacencyDirection::GAD_IN, edge.target_node_id, edgeId})));
			}
			for (const BTreePageEntry &entry: entries) {
				if (entry.value_or_child != 0U || !expectedKeys.erase(entry.key)) {
					throwIntegrityError(
							"relocatable integrity scan found an orphan, duplicate, or nonempty global adjacency entry");
				}
			}
			requireIntegrity(expectedKeys.empty(),
							 "relocatable integrity scan found a live Edge missing a global adjacency entry");
		}

		metadata_.locations_.visitVectorLocations(superblock, [&](const VectorLocation &location) {
			const auto page = scannedPages.find(location.page_id.value());
			const auto vector = vectorPages.find(location.page_id.value());
			requireIntegrity(page != scannedPages.end(),
							 "relocatable integrity scan found a Vector Location reference to an absent page");
			requireIntegrity(vector != vectorPages.end(),
							 "relocatable integrity scan found a Vector Location outside a VectorPage");
			const VectorPage opened = VectorPage::open(page->second.owner, page->second.payload);
			const utils::OwnedVector<std::byte> record = opened.read(location.slot_id, location.slot_generation);
			const uint8_t invalidRecordIdentity =
					static_cast<uint8_t>(location.vector_id == 0U) |
					static_cast<uint8_t>(LittleEndianCodec::readU64(record, 0U) != location.vector_id);
			if (invalidRecordIdentity != 0U) {
				throwIntegrityError(
						"relocatable integrity scan found a Vector Location with mismatched record identity");
			}
			if (opened.header().kind == VectorStorageKind::VSK_RAW) {
				static_cast<void>(rawVectorIdsByIndex[opened.header().index_id].insert(location.vector_id));
			}
			++report.vector_locations;
		});

		metadata_.locations_.visitVectorManifests(superblock, [&](const VectorManifestLocation &location) {
			const auto actual = scannedManifests.find(location.index_id);
			const auto page = scannedPages.find(location.manifest_page_id.value());
			const uint8_t missingManifest = static_cast<uint8_t>(actual == scannedManifests.end()) |
											static_cast<uint8_t>(page == scannedPages.end());
			requireIntegrity(
					missingManifest == 0U,
					"relocatable integrity scan found a Vector Manifest Directory reference to an absent page");
			requireIntegrity(actual->second == location,
							 "relocatable integrity scan found a stale Vector Manifest Directory reference");
			const VectorManifest manifest =
					VectorManifestPage::open(page->second.owner, page->second.payload).manifest();
			if (manifest.raw_write_tail_page_id.isValid()) {
				const auto tailPage = scannedPages.find(manifest.raw_write_tail_page_id.value());
				const auto tailHeader = vectorPages.find(manifest.raw_write_tail_page_id.value());
				const uint8_t invalidTail =
						static_cast<uint8_t>(tailPage == scannedPages.end()) |
						static_cast<uint8_t>(tailHeader == vectorPages.end()) |
						static_cast<uint8_t>(tailPage != scannedPages.end() &&
											 tailPage->second.pool.kind != SegmentKind::SGK_VECTOR_RAW_POOL) |
						static_cast<uint8_t>(tailPage != scannedPages.end() &&
											 tailPage->second.owner.kind != PageOwnerKind::POK_VECTOR_RAW) |
						static_cast<uint8_t>(tailPage != scannedPages.end() &&
											 tailPage->second.owner.owner_id != manifest.index_id) |
						static_cast<uint8_t>(tailHeader != vectorPages.end() &&
											 tailHeader->second.index_id != manifest.index_id) |
						static_cast<uint8_t>(tailHeader != vectorPages.end() &&
											 tailHeader->second.kind != VectorStorageKind::VSK_RAW) |
						static_cast<uint8_t>(tailHeader != vectorPages.end() &&
											 static_cast<uint64_t>(tailHeader->second.record_bytes) !=
													 sizeof(uint64_t) +
															 static_cast<uint64_t>(manifest.dimensions) * 2U);
				if (invalidTail != 0U) {
					throwIntegrityError("relocatable integrity scan found an invalid Vector Manifest raw-write tail");
				}
			}
			const auto rawVectorIds = rawVectorIdsByIndex.find(manifest.index_id);
			const ScanSet<uint64_t> emptyRawVectorIds;
			const ScanSet<uint64_t> &allRawVectorIds =
					rawVectorIds == rawVectorIdsByIndex.end() ? emptyRawVectorIds : rawVectorIds->second;
			ScanSet<uint64_t> semanticVectorIds = allRawVectorIds;
			ScanSet<uint64_t> baseVectorIds = allRawVectorIds;
			utils::OwnedVector<VectorDeltaEntry> semanticDeltaEntries;
			if (manifest.delta_mapping_index_id != 0U) {
				const std::optional<IndexRootLocation> mappingRoot =
						metadata_.locations_.findIndexRootOptional(superblock, manifest.delta_mapping_index_id);
				const uint8_t invalidMappingRoot =
						static_cast<uint8_t>(!VectorDeltaMapping::isMappingIndexId(manifest.delta_mapping_index_id)) |
						static_cast<uint8_t>(!mappingRoot.has_value()) |
						static_cast<uint8_t>(mappingRoot.has_value() &&
											 mappingRoot->root_page_id != manifest.delta_mapping_root_page_id) |
						static_cast<uint8_t>(mappingRoot.has_value() &&
											 mappingRoot->root_generation != manifest.delta_mapping_root_generation);
				if (invalidMappingRoot != 0U) {
					throwIntegrityError(
							"relocatable integrity scan found a Vector Manifest delta mapping-root mismatch");
				}
				const utils::OwnedVector<BTreePageEntry> deltaEntries =
						scanIndexTree(scannedPages, *mappingRoot, manifest.delta_entry_count);
				if (deltaEntries.size() != manifest.delta_entry_count) {
					throwIntegrityError("relocatable integrity scan found an incomplete Vector delta mapping");
				}
				for (const BTreePageEntry &entry: deltaEntries) {
					const VectorDeltaEntry delta = VectorDeltaMapping::decodeEntry(entry);
					const uint64_t vectorId = delta.vector_id;
					const VectorDeltaOperation operation = delta.operation;
					const bool hasCurrentRawLocation = allRawVectorIds.contains(vectorId);
					VectorDeltaMapping::validateCurrentRawMembership(delta, hasCurrentRawLocation);
					if (operation == VectorDeltaOperation::VDO_ERASE) {
					static_cast<void>(baseVectorIds.insert(vectorId));
				} else if (operation == VectorDeltaOperation::VDO_INSERT) {
					static_cast<void>(baseVectorIds.erase(vectorId));
				}
					semanticDeltaEntries.push_back(delta);
				}
			}
			if (manifest.node_mapping_root_page_id.isValid()) {
				const std::optional<IndexRootLocation> nodeMappingRoot =
						metadata_.locations_.findIndexRootOptional(superblock, manifest.index_id);
				const uint8_t invalidNodeMappingRoot =
						static_cast<uint8_t>(!nodeMappingRoot.has_value()) |
						static_cast<uint8_t>(nodeMappingRoot.has_value() &&
											 nodeMappingRoot->root_page_id != manifest.node_mapping_root_page_id) |
						static_cast<uint8_t>(nodeMappingRoot.has_value() &&
											 nodeMappingRoot->root_generation != manifest.node_mapping_root_generation);
				if (invalidNodeMappingRoot != 0U) {
					throwIntegrityError(
							"relocatable integrity scan found a Vector Manifest Node-to-Vector mapping-root mismatch");
				}
				const std::optional<IndexRootLocation> vectorNodeMappingRoot =
						metadata_.locations_.findIndexRootOptional(superblock, manifest.vector_node_mapping_index_id);
				const uint8_t invalidVectorNodeMappingRoot =
						static_cast<uint8_t>(
								!VectorNodeMapping::isMappingIndexId(manifest.vector_node_mapping_index_id)) |
						static_cast<uint8_t>(!vectorNodeMappingRoot.has_value()) |
						static_cast<uint8_t>(vectorNodeMappingRoot.has_value() &&
											 vectorNodeMappingRoot->root_page_id !=
													 manifest.vector_node_mapping_root_page_id) |
						static_cast<uint8_t>(vectorNodeMappingRoot.has_value() &&
											 vectorNodeMappingRoot->root_generation !=
													 manifest.vector_node_mapping_root_generation);
				if (invalidVectorNodeMappingRoot != 0U) {
					throwIntegrityError(
							"relocatable integrity scan found a Vector Manifest Vector-to-Node mapping-root mismatch");
				}
				const ScanSet<uint64_t> &expectedVectorIds = semanticVectorIds;
				const utils::OwnedVector<BTreePageEntry> nodeEntries =
						scanIndexTree(scannedPages, *nodeMappingRoot, expectedVectorIds.size());
				if (nodeEntries.size() != expectedVectorIds.size()) {
					throwIntegrityError("relocatable integrity scan found an incomplete Node-to-Vector mapping");
				}
				ScanMap<uint64_t, uint64_t> nodeByVector;
				ScanMap<uint64_t, uint64_t> vectorByNode;
				for (const BTreePageEntry &entry: nodeEntries) {
					const uint64_t nodeId = decodeCanonicalU64Key(
							entry.key, "relocatable integrity scan found a malformed Node-to-Vector mapping key");
					if (nodeId == 0U || entry.value_or_child == 0U ||
						!expectedVectorIds.contains(entry.value_or_child) ||
						!nodeByVector.emplace(entry.value_or_child, nodeId).second) {
						throwIntegrityError("relocatable integrity scan found a non-bijective Node-to-Vector mapping");
					}
					// Canonical B+Tree keys make NodeId unique before this conversion.
					static_cast<void>(vectorByNode.emplace(nodeId, entry.value_or_child));
				}
				const utils::OwnedVector<BTreePageEntry> reverseEntries =
						scanIndexTree(scannedPages, *vectorNodeMappingRoot, expectedVectorIds.size());
				if (reverseEntries.size() != expectedVectorIds.size()) {
					throwIntegrityError("relocatable integrity scan found an incomplete Vector-to-Node mapping");
				}
				for (const BTreePageEntry &entry: reverseEntries) {
					const uint64_t vectorId = decodeCanonicalU64Key(
							entry.key, "relocatable integrity scan found a malformed Vector-to-Node mapping key");
					if (entry.value_or_child == 0U) {
						throwIntegrityError(
								"relocatable integrity scan found a malformed Vector-to-Node mapping entry");
					}
					const auto expected = nodeByVector.find(vectorId);
					// A count-equal, duplicate-free Node mapping has exactly one entry for every expected Vector ID.
					if (vectorId == 0U || !expectedVectorIds.contains(vectorId) ||
						expected->second != entry.value_or_child) {
						throwIntegrityError(
								"relocatable integrity scan found a noncanonical Vector-to-Node mapping key");
					}
				}
				ScanMap<uint64_t, VectorDeltaEntry> deltaByVector;
				for (const VectorDeltaEntry &delta: semanticDeltaEntries) {
					deltaByVector.emplace(delta.vector_id, delta);
					if (delta.operation == VectorDeltaOperation::VDO_INSERT) {
						// Membership validation and the exact semantic mapping cardinality
						// prove that every current insert has a mapping entry.
						const auto currentNode = nodeByVector.find(delta.vector_id);
						if (currentNode->second != delta.node_id) {
							throwIntegrityError(
									"relocatable integrity scan found a Vector delta insert without its semantic mapping");
						}
					}
				}
				for (const VectorDeltaEntry &delta: semanticDeltaEntries) {
					if (delta.operation != VectorDeltaOperation::VDO_ERASE) {
						continue;
					}
					const auto replacement = vectorByNode.find(delta.node_id);
					if (replacement == vectorByNode.end()) {
						continue;
					}
					const auto replacementDelta = deltaByVector.find(replacement->second);
					if (replacementDelta == deltaByVector.end()) {
						throwIntegrityError(
								"relocatable integrity scan found a Vector delta tombstone with an invalid replacement");
					}
				}
			} else if (!semanticDeltaEntries.empty()) {
				throwIntegrityError(
						"relocatable integrity scan found Vector delta entries without semantic mapping roots");
			}
			if (manifest.pq_mapping_index_id != 0U) {
				const std::optional<IndexRootLocation> mappingRoot =
						metadata_.locations_.findIndexRootOptional(superblock, manifest.pq_mapping_index_id);
				const uint8_t invalidMappingRoot =
						static_cast<uint8_t>(!mappingRoot.has_value()) |
						static_cast<uint8_t>(mappingRoot.has_value() &&
											 mappingRoot->root_page_id != manifest.pq_mapping_root_page_id) |
						static_cast<uint8_t>(mappingRoot.has_value() &&
											 mappingRoot->root_generation != manifest.pq_mapping_root_generation);
				if (invalidMappingRoot != 0U) {
					throwIntegrityError("relocatable integrity scan found a Vector Manifest PQ mapping-root mismatch");
				}
			}
			if (manifest.adjacency_mapping_index_id != 0U) {
				const std::optional<IndexRootLocation> mappingRoot =
						metadata_.locations_.findIndexRootOptional(superblock, manifest.adjacency_mapping_index_id);
				const uint8_t invalidMappingRoot =
						static_cast<uint8_t>(!mappingRoot.has_value()) |
						static_cast<uint8_t>(mappingRoot.has_value() &&
											 mappingRoot->root_page_id != manifest.adjacency_mapping_root_page_id) |
						static_cast<uint8_t>(mappingRoot.has_value() &&
											 mappingRoot->root_generation !=
													 manifest.adjacency_mapping_root_generation);
				if (invalidMappingRoot != 0U) {
					throwIntegrityError(
							"relocatable integrity scan found a Vector Manifest adjacency mapping-root mismatch");
				}
				if (rawVectorIds == rawVectorIdsByIndex.end()) {
					throwIntegrityError(
							"relocatable integrity scan found an adjacency generation without raw Vector IDs");
				}
				const utils::OwnedVector<BTreePageEntry> entries =
						scanIndexTree(scannedPages, *mappingRoot, baseVectorIds.size());
				if (entries.size() != baseVectorIds.size()) {
					throwIntegrityError("relocatable integrity scan found an adjacency mapping that does not cover "
										"every raw Vector");
				}
				auto expectedVectorId = baseVectorIds.begin();
				for (const BTreePageEntry &entry: entries) {
					if (entry.key != VectorAdjacencyMapping::key(*expectedVectorId) || entry.value_or_child == 0U) {
						throwIntegrityError(
								"relocatable integrity scan found an adjacency mapping key or page value mismatch");
					}
					const auto adjacencyPage = scannedPages.find(entry.value_or_child);
					// The preceding Page Directory scan has already bound an adjacency page's pool kind to its owner
					// kind and verified its record order; only the derived owner namespace remains mapping-specific
					// here.
					if (adjacencyPage == scannedPages.end() ||
						adjacencyPage->second.pool.kind != SegmentKind::SGK_VECTOR_ADJACENCY_POOL ||
						adjacencyPage->second.owner.owner_id !=
								VectorAdjacencyPageStore::derivedOwnerId(manifest.index_id)) {
						throwIntegrityError(
								"relocatable integrity scan found an adjacency mapping page with invalid ownership");
					}
					const SlottedPage page =
							SlottedPage::open(adjacencyPage->second.owner, adjacencyPage->second.payload);
					bool found = false;
					for (const SlottedSlotReference &reference: page.liveSlots()) {
						const VectorAdjacencyRecord record = VectorAdjacencyCodec::decode(page.read(reference));
						if (record.vector_id == *expectedVectorId) {
							for (const uint64_t neighbor: record.neighbors) {
								if (!baseVectorIds.contains(neighbor)) {
									throwIntegrityError(
											"relocatable integrity scan found an adjacency neighbor outside its "
											"raw Vector index");
								}
							}
							found = true;
						}
					}
						if (!found) {
							throwIntegrityError(
									"relocatable integrity scan found an adjacency mapping page without its Vector record");
						}
						++report.vector_adjacencies;
						++expectedVectorId;
				}
			}
			for (const VectorManifestEntry &entry: manifest.entries) {
				const auto vectorPage = scannedPages.find(entry.page_id.value());
				const auto vectorHeader = vectorPages.find(entry.page_id.value());
				const uint8_t missingVectorPage = static_cast<uint8_t>(vectorPage == scannedPages.end()) |
												  static_cast<uint8_t>(vectorHeader == vectorPages.end());
				if (missingVectorPage != 0U) {
					throwIntegrityError(
							"relocatable integrity scan found a manifest reference to an absent VectorPage");
				}
				const uint8_t invalidVectorPage =
						static_cast<uint8_t>(vectorPage->second.owner.owner_id != manifest.index_id) |
						static_cast<uint8_t>(vectorPage->second.owner.kind != vectorPageOwnerKind(entry.storage_kind)) |
						static_cast<uint8_t>(vectorHeader->second.live_count != entry.record_count) |
						static_cast<uint8_t>(entry.storage_kind == VectorStorageKind::VSK_PQ &&
											 manifest.codebook_id == 0U) |
						static_cast<uint8_t>(entry.storage_kind == VectorStorageKind::VSK_PQ &&
											 vectorHeader->second.codebook_generation != manifest.codebook_generation);
				if (invalidVectorPage != 0U) {
					throwIntegrityError("relocatable integrity scan found a manifest vector-page reference mismatch");
				}
			}
			if (manifest.codebook_id != 0U) {
				const auto codebook = scannedCodebooks.find(manifest.codebook_id);
				if (codebook == scannedCodebooks.end()) {
					throwIntegrityError("relocatable integrity scan found a manifest reference to an absent codebook");
				}
				if (codebook->second.generation != manifest.codebook_generation) {
					throwIntegrityError("relocatable integrity scan found a manifest codebook reference mismatch");
				}
			}
			++report.vector_manifests;
		});
		if (report.vector_manifests != scannedManifests.size()) {
			throwIntegrityError("relocatable integrity scan found a manifest page missing from its directory");
		}
		return report;
	}

} // namespace graph::storage
