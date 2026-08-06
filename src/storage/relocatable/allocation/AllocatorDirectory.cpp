/**
 * @file AllocatorDirectory.cpp
 * @brief Copy-on-write transitions for relocatable physical-slot free space.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/allocation/AllocatorDirectory.hpp"

#include "graph/storage/relocatable/metadata/MetadataWriteBatch.hpp"

#include <algorithm>
#include <bit>
#include <functional>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace graph::storage {

	namespace {

		[[noreturn]] void throwAllocatorDirectoryError(FormatError error, const char *message) {
			throw FormatException(error, message);
		}

		constexpr uint64_t RSS_MAX_PHYSICAL_PAGE_GENERATION = std::numeric_limits<uint64_t>::max() >> 5U;

		[[nodiscard]] bool sameSuperblock(const Superblock &left, const Superblock &right) { return left == right; }

		[[nodiscard]] uint64_t leafOrdinal(PhysicalSlotId slot) {
			if (!slot.isValid()) {
				throwAllocatorDirectoryError(FormatError::FERR_INVALID_HEADER,
											 "relocatable free-space slot is invalid");
			}
			return (slot.value() - 1U) / RSS_ALLOCATOR_PAGE_SLOT_CAPACITY;
		}

		[[nodiscard]] uint16_t rootLevelFor(uint64_t ordinal) {
			uint16_t level = 0;
			while (ordinal >= RSS_CATALOG_RADIX_FANOUT) {
				ordinal /= RSS_CATALOG_RADIX_FANOUT;
				++level;
			}
			return level;
		}

		[[nodiscard]] uint32_t childIndex(uint64_t ordinal, uint16_t level) {
			for (uint16_t index = 0; index < level; ++index) {
				ordinal /= RSS_CATALOG_RADIX_FANOUT;
			}
			return static_cast<uint32_t>(ordinal % RSS_CATALOG_RADIX_FANOUT);
		}

		[[nodiscard]] bool rootCoversOrdinal(uint64_t ordinal, uint16_t level) {
			for (uint16_t index = 0; index <= level; ++index) {
				ordinal /= RSS_CATALOG_RADIX_FANOUT;
			}
			return ordinal == 0U;
		}

		[[nodiscard]] uint64_t firstSlotFor(uint64_t ordinal) {
			return ordinal * RSS_ALLOCATOR_PAGE_SLOT_CAPACITY + 1U;
		}

		[[nodiscard]] uint64_t saturatedRadixPower(uint16_t exponent) {
			uint64_t value = 1U;
			for (uint16_t index = 0U; index < exponent; ++index) {
				if (value > std::numeric_limits<uint64_t>::max() / RSS_CATALOG_RADIX_FANOUT) {
					return std::numeric_limits<uint64_t>::max();
				}
				value *= RSS_CATALOG_RADIX_FANOUT;
			}
			return value;
		}

		[[nodiscard]] uint64_t saturatedAdd(uint64_t left, uint64_t right) {
			if (left > std::numeric_limits<uint64_t>::max() - right) {
				return std::numeric_limits<uint64_t>::max();
			}
			return left + right;
		}

		[[nodiscard]] uint64_t saturatedMultiply(uint64_t left, uint64_t right) {
			if (left == 0U || right == 0U) {
				return 0U;
			}
			if (left > std::numeric_limits<uint64_t>::max() / right) {
				return std::numeric_limits<uint64_t>::max();
			}
			return left * right;
		}

		[[nodiscard]] bool allocatorChildHasFree(const MetadataRadixPage &page, uint32_t index) {
			if (page.directory_type != MetadataDirectoryType::MDT_ALLOCATOR || index >= RSS_CATALOG_RADIX_FANOUT) {
				throwAllocatorDirectoryError(
						FormatError::FERR_INVALID_STATE,
						"relocatable free-space summary lookup has an invalid radix page or child index");
			}
			return (std::to_integer<uint8_t>(page.allocator_free_children[index / 8U]) & (1U << (index % 8U))) != 0U;
		}

		void setAllocatorChildHasFree(MetadataRadixPage &page, uint32_t index, bool hasFree) {
			if (page.directory_type != MetadataDirectoryType::MDT_ALLOCATOR || index >= RSS_CATALOG_RADIX_FANOUT) {
				throwAllocatorDirectoryError(
						FormatError::FERR_INVALID_STATE,
						"relocatable free-space summary update has an invalid radix page or child index");
			}
			std::byte &byte = page.allocator_free_children[index / 8U];
			const uint8_t mask = static_cast<uint8_t>(1U << (index % 8U));
			if (hasFree) {
				byte |= std::byte{mask};
			} else {
				byte &= ~std::byte{mask};
			}
		}

		[[nodiscard]] bool leafHasFreeSlot(const AllocatorLeafPage &leaf) {
			const uint32_t usedBytes = (leaf.slot_count + 7U) / 8U;
			for (uint32_t index = 0U; index < usedBytes; ++index) {
				uint8_t clearBits = static_cast<uint8_t>(~std::to_integer<uint8_t>(leaf.allocated_slots[index]));
				if (index + 1U == usedBytes && (leaf.slot_count % 8U) != 0U) {
					clearBits &= static_cast<uint8_t>((1U << (leaf.slot_count % 8U)) - 1U);
				}
				if (clearBits != 0U) {
					return true;
				}
			}
			return false;
		}

		[[nodiscard]] bool radixSubtreeHasFreeSlot(const MetadataRadixPage &page) {
			if (page.directory_type != MetadataDirectoryType::MDT_ALLOCATOR) {
				throwAllocatorDirectoryError(FormatError::FERR_INVALID_STATE,
											 "relocatable free-space summary has a non-allocator radix page");
			}
			for (uint32_t index = 0U; index < RSS_CATALOG_RADIX_FANOUT; ++index) {
				// An absent branch has never allocated a bitmap leaf and therefore
				// represents an entirely reusable physical-slot range.
				if (!page.children[index].isValid() || allocatorChildHasFree(page, index)) {
					return true;
				}
			}
			return false;
		}

		void validateRadix(const MetadataRadixPage &page, bool root, uint16_t expectedLevel,
						   const Superblock &superblock) {
			const uint8_t invalid = static_cast<uint8_t>(page.directory_type != MetadataDirectoryType::MDT_ALLOCATOR) |
									static_cast<uint8_t>(root && page.kind != CatalogRadixPageKind::CRPK_ROOT) |
									static_cast<uint8_t>(!root && page.kind != CatalogRadixPageKind::CRPK_INTERNAL) |
									static_cast<uint8_t>(page.level != expectedLevel) |
									static_cast<uint8_t>(page.generation > superblock.generation) |
									static_cast<uint8_t>(page.committed_lsn > superblock.checkpoint_lsn);
			if (invalid != 0U) {
				throwAllocatorDirectoryError(
						FormatError::FERR_INVALID_HEADER,
						"relocatable free-space radix page is invalid for this metadata generation");
			}
		}

		void validateLeafRoute(const AllocatorLeafPage &leaf, uint64_t ordinal, const Superblock &superblock) {
			const uint8_t invalid = static_cast<uint8_t>(leaf.first_slot != PhysicalSlotId(firstSlotFor(ordinal))) |
									static_cast<uint8_t>(leaf.generation > superblock.generation) |
									static_cast<uint8_t>(leaf.committed_lsn > superblock.checkpoint_lsn);
			if (invalid != 0U) {
				throwAllocatorDirectoryError(FormatError::FERR_INVALID_HEADER,
											 "relocatable free-space leaf is invalid for this radix route");
			}
		}

		[[nodiscard]] bool slotBit(const AllocatorLeafPage &leaf, PhysicalSlotId slot) {
			const uint64_t offset = slot.value() - leaf.first_slot.value();
			if (offset >= leaf.slot_count) {
				return false;
			}
			return (std::to_integer<uint8_t>(leaf.allocated_slots[offset / 8U]) & (1U << (offset % 8U))) != 0U;
		}

		void setSlotBit(AllocatorLeafPage &leaf, PhysicalSlotId slot, bool allocated) {
			const uint64_t offset = slot.value() - leaf.first_slot.value();
			if (offset >= leaf.slot_count) {
				throwAllocatorDirectoryError(FormatError::FERR_INVALID_HEADER,
											 "relocatable free-space slot lies outside its leaf range");
			}
			std::byte &byte = leaf.allocated_slots[offset / 8U];
			const uint8_t mask = static_cast<uint8_t>(1U << (offset % 8U));
			if (allocated) {
				byte |= std::byte{mask};
			} else {
				byte &= ~std::byte{mask};
			}
		}

		void validateRange(PhysicalSlotId firstSlot, PhysicalSlotId lastSlot) {
			if (!firstSlot.isValid() || !lastSlot.isValid() || firstSlot > lastSlot) {
				throwAllocatorDirectoryError(FormatError::FERR_INVALID_HEADER,
											 "relocatable free-space allocation range is invalid");
			}
		}

		[[nodiscard]] std::optional<AllocatorLeafPage>
		readLeafForOrdinal(const MetadataPageFile &pages, const Superblock &superblock, uint64_t ordinal) {
			if (!superblock.allocator_root.isValid()) {
				return std::nullopt;
			}
			MetadataRadixPage node = pages.readMetadataRadix(superblock.allocator_root);
			validateRadix(node, true, node.level, superblock);
			if (!rootCoversOrdinal(ordinal, node.level)) {
				return std::nullopt;
			}
			for (;;) {
				const PhysicalPageRef child = node.children[childIndex(ordinal, node.level)];
				if (!child.isValid()) {
					return std::nullopt;
				}
				if (node.level == 0U) {
					AllocatorLeafPage leaf = pages.readAllocatorLeaf(child);
					validateLeafRoute(leaf, ordinal, superblock);
					return leaf;
				}
				const uint16_t childLevel = static_cast<uint16_t>(node.level - 1U);
				node = pages.readMetadataRadix(child);
				validateRadix(node, false, childLevel, superblock);
			}
		}

		[[nodiscard]] std::optional<PhysicalSlotId>
		findFirstClearBit(const AllocatorLeafPage &leaf, PhysicalSlotId firstSlot, PhysicalSlotId lastSlot) {
			const uint64_t firstOffset = firstSlot.value() - leaf.first_slot.value();
			const uint64_t lastOffset = lastSlot.value() - leaf.first_slot.value();
			if (firstOffset > lastOffset || lastOffset >= leaf.slot_count) {
				throwAllocatorDirectoryError(FormatError::FERR_INVALID_STATE,
											 "relocatable free-space bitmap scan falls outside its leaf range");
			}

			const uint32_t firstByte = static_cast<uint32_t>(firstOffset / 8U);
			const uint32_t lastByte = static_cast<uint32_t>(lastOffset / 8U);
			for (uint32_t index = firstByte; index <= lastByte; ++index) {
				uint8_t clearBits = static_cast<uint8_t>(~std::to_integer<uint8_t>(leaf.allocated_slots[index]));
				if (index == firstByte) {
					clearBits &= static_cast<uint8_t>(0xFFU << (firstOffset % 8U));
				}
				if (index == lastByte && (lastOffset % 8U) != 7U) {
					clearBits &= static_cast<uint8_t>((1U << (lastOffset % 8U + 1U)) - 1U);
				}
				if (clearBits != 0U) {
					const uint64_t offset =
							static_cast<uint64_t>(index) * 8U + std::countr_zero(static_cast<uint32_t>(clearBits));
					return PhysicalSlotId(leaf.first_slot.value() + offset);
				}
			}
			return std::nullopt;
		}

		[[nodiscard]] std::optional<uint64_t> findFirstFreeLeafOrdinal(const MetadataPageFile &pages,
																	   const Superblock &superblock,
																	   uint64_t firstOrdinal, uint64_t lastOrdinal) {
			if (firstOrdinal > lastOrdinal) {
				return std::nullopt;
			}
			if (!superblock.allocator_root.isValid()) {
				return firstOrdinal;
			}

			const MetadataRadixPage root = pages.readMetadataRadix(superblock.allocator_root);
			validateRadix(root, true, root.level, superblock);
			const uint64_t rootSpan = saturatedRadixPower(static_cast<uint16_t>(root.level + 1U));
			const uint64_t rootLastOrdinal =
					rootSpan == std::numeric_limits<uint64_t>::max() ? rootSpan : rootSpan - 1U;
			if (firstOrdinal > rootLastOrdinal) {
				return firstOrdinal;
			}
			const uint64_t boundedLastOrdinal = std::min(lastOrdinal, rootLastOrdinal);

			std::function<std::optional<uint64_t>(const MetadataRadixPage &, bool, uint16_t, uint64_t)> visit;
			visit = [&](const MetadataRadixPage &node, bool isRoot, uint16_t expectedLevel,
						uint64_t ordinalBase) -> std::optional<uint64_t> {
				validateRadix(node, isRoot, expectedLevel, superblock);
				const uint64_t childSpan = saturatedRadixPower(node.level);
				const uint64_t boundedFirst = std::max(firstOrdinal, ordinalBase);
				const uint64_t nodeSpan = saturatedRadixPower(static_cast<uint16_t>(node.level + 1U));
				const uint64_t nodeLastOrdinal = nodeSpan == std::numeric_limits<uint64_t>::max()
														 ? nodeSpan
														 : saturatedAdd(ordinalBase, nodeSpan - 1U);
				const uint64_t boundedLast = std::min(boundedLastOrdinal, nodeLastOrdinal);
				if (boundedFirst > boundedLast) {
					return std::nullopt;
				}
				const uint64_t firstIndex = (boundedFirst - ordinalBase) / childSpan;
				const uint64_t lastIndex = (boundedLast - ordinalBase) / childSpan;
				if (lastIndex >= RSS_CATALOG_RADIX_FANOUT) {
					throwAllocatorDirectoryError(FormatError::FERR_INVALID_HEADER,
												 "relocatable free-space summary route exceeds a radix page's fan-out");
				}
				for (uint64_t rawIndex = firstIndex; rawIndex <= lastIndex; ++rawIndex) {
					const uint32_t index = static_cast<uint32_t>(rawIndex);
					const uint64_t childBase = saturatedAdd(ordinalBase, saturatedMultiply(rawIndex, childSpan));
					const uint64_t childLast = saturatedAdd(childBase, childSpan - 1U);
					const uint64_t candidate = std::max(boundedFirst, childBase);
					if (!node.children[index].isValid()) {
						return candidate;
					}
					if (!allocatorChildHasFree(node, index)) {
						continue;
					}
					if (node.level == 0U) {
						return candidate;
					}
					const MetadataRadixPage child = pages.readMetadataRadix(node.children[index]);
					if (const std::optional<uint64_t> found =
								visit(child, false, static_cast<uint16_t>(node.level - 1U), childBase);
						found.has_value()) {
						return found;
					}
					if (childLast == std::numeric_limits<uint64_t>::max()) {
						break;
					}
				}
				return std::nullopt;
			};

			if (const std::optional<uint64_t> found = visit(root, true, root.level, 0U); found.has_value()) {
				return found;
			}
			if (lastOrdinal > rootLastOrdinal && rootLastOrdinal != std::numeric_limits<uint64_t>::max()) {
				return rootLastOrdinal + 1U;
			}
			return std::nullopt;
		}

	} // namespace

	AllocatorDirectory::AllocatorDirectory(MetadataPageFile &pages, MetadataPageAllocator &allocator) :
		pages_(pages), allocator_(allocator) {}

	Superblock AllocatorDirectory::initializeNew(const Superblock &initial, PhysicalSlotId bootstrapPoolSlot,
														 PhysicalSlotId emergencyReserveSlot) {
		static_cast<void>(SuperblockCodec::encode(initial));
		const uint8_t invalidInitial = static_cast<uint8_t>(initial.allocator_root.isValid()) |
									   static_cast<uint8_t>(initial.catalog_root.isValid()) |
									   static_cast<uint8_t>(initial.page_directory_root.isValid()) |
									   static_cast<uint8_t>(initial.extent_directory_root.isValid()) |
									   static_cast<uint8_t>(initial.node_entity_directory_root.isValid()) |
					static_cast<uint8_t>(initial.edge_entity_directory_root.isValid()) |
					static_cast<uint8_t>(!bootstrapPoolSlot.isValid()) |
					static_cast<uint8_t>(emergencyReserveSlot.isValid() && emergencyReserveSlot == bootstrapPoolSlot);
		if (invalidInitial != 0U) {
			throwAllocatorDirectoryError(
					FormatError::FERR_INVALID_STATE,
					"relocatable free-space bootstrap requires an empty root and a valid metadata pool slot");
		}

		std::vector<PhysicalPageRef> allocatedPages;
		std::vector<PhysicalSlotId> createdPools;
		auto allocatePage = [&]() {
			const MetadataPageAllocation allocation =
					allocator_.allocatePage(initial.generation, initial.checkpoint_lsn, {});
			if (!allocation.page.isValid() || allocation.page.generation != initial.generation) {
				throwAllocatorDirectoryError(
						FormatError::FERR_INVALID_STATE,
						"relocatable free-space bootstrap allocator returned an invalid physical reference");
			}
			if (allocation.created_pool_slot.has_value()) {
				if (*allocation.created_pool_slot != allocation.page.physical_slot) {
					throwAllocatorDirectoryError(
							FormatError::FERR_INVALID_STATE,
							"relocatable free-space bootstrap allocator returned an invalid pool ownership record");
				}
				createdPools.push_back(*allocation.created_pool_slot);
			}
			allocatedPages.push_back(allocation.page);
			return allocation.page;
		};

		try {
			AllocatorLeafPage leaf;
			leaf.page_id = allocator_.allocatePageId();
			leaf.first_slot = PhysicalSlotId(1);
			leaf.slot_count = RSS_ALLOCATOR_PAGE_SLOT_CAPACITY;
			leaf.generation = initial.generation;
			leaf.committed_lsn = initial.checkpoint_lsn;
			setSlotBit(leaf, bootstrapPoolSlot, true);
			if (emergencyReserveSlot.isValid()) {
				setSlotBit(leaf, emergencyReserveSlot, true);
			}
			const PhysicalPageRef leafReference = allocatePage();
			pages_.writeAllocatorLeaf(leafReference, leaf);

			MetadataRadixPage root;
			root.kind = CatalogRadixPageKind::CRPK_ROOT;
			root.directory_type = MetadataDirectoryType::MDT_ALLOCATOR;
			root.level = 0;
			root.page_id = allocator_.allocatePageId();
			root.generation = initial.generation;
			root.committed_lsn = initial.checkpoint_lsn;
			root.children[0] = leafReference;
			setAllocatorChildHasFree(root, 0U, leafHasFreeSlot(leaf));
			const PhysicalPageRef rootReference = allocatePage();
			pages_.writeMetadataRadix(rootReference, root);

			if (createdPools.size() != 1U || createdPools.front() != bootstrapPoolSlot) {
				throwAllocatorDirectoryError(
						FormatError::FERR_INVALID_STATE,
						"relocatable free-space bootstrap did not allocate the required metadata pool slot");
			}
			Superblock initialized = initial;
			initialized.allocator_root = rootReference;
			return initialized;
		} catch (...) {
			for (auto iterator = allocatedPages.rbegin(); iterator != allocatedPages.rend(); ++iterator) {
				allocator_.abandonPage(*iterator);
			}
			for (auto iterator = createdPools.rbegin(); iterator != createdPools.rend(); ++iterator) {
				allocator_.abandonUnpublishedPool(*iterator);
			}
			throw;
		}
	}

	bool AllocatorDirectory::isAllocated(const Superblock &superblock, PhysicalSlotId slot) const {
		static_cast<void>(SuperblockCodec::encode(superblock));
		const uint64_t ordinal = leafOrdinal(slot);
		const std::optional<AllocatorLeafPage> leaf = readLeafForOrdinal(pages_, superblock, ordinal);
		if (!leaf.has_value()) {
			return false;
		}
		return slotBit(*leaf, slot);
	}

	std::optional<PhysicalSlotId> AllocatorDirectory::findFirstFree(const Superblock &superblock,
																	PhysicalSlotId firstSlot,
																	PhysicalSlotId lastSlot) const {
		static_cast<void>(SuperblockCodec::encode(superblock));
		validateRange(firstSlot, lastSlot);
		const uint64_t firstOrdinal = leafOrdinal(firstSlot);
		const uint64_t lastOrdinal = leafOrdinal(lastSlot);
		uint64_t candidateOrdinal = firstOrdinal;
		while (candidateOrdinal <= lastOrdinal) {
			const std::optional<uint64_t> availableOrdinal =
					findFirstFreeLeafOrdinal(pages_, superblock, candidateOrdinal, lastOrdinal);
			if (!availableOrdinal.has_value()) {
				return std::nullopt;
			}
			const std::optional<AllocatorLeafPage> leaf = readLeafForOrdinal(pages_, superblock, *availableOrdinal);
			if (!leaf.has_value()) {
				return PhysicalSlotId(std::max(firstSlot.value(), firstSlotFor(*availableOrdinal)));
			}
			const uint64_t leafFirstSlot = leaf->first_slot.value();
			const uint64_t leafLastSlot = leafFirstSlot + leaf->slot_count - 1U;
			const uint64_t scanFirstValue = std::max(firstSlot.value(), leafFirstSlot);
			const uint64_t scanLastValue = std::min(lastSlot.value(), leafLastSlot);
			if (scanFirstValue > scanLastValue) {
				if (*availableOrdinal == lastOrdinal || *availableOrdinal == std::numeric_limits<uint64_t>::max()) {
					return std::nullopt;
				}
				candidateOrdinal = *availableOrdinal + 1U;
				continue;
			}
			const PhysicalSlotId scanFirst(scanFirstValue);
			const PhysicalSlotId scanLast(scanLastValue);
			if (const std::optional<PhysicalSlotId> freeSlot = findFirstClearBit(*leaf, scanFirst, scanLast);
				freeSlot.has_value()) {
				return freeSlot;
			}

			// The root-to-leaf summary promised capacity, but this constrained query
			// may only cover an already-full suffix/prefix of that leaf. All interior
			// leaves are checked through their complete bitmap range on the next pass.
			if (*availableOrdinal == lastOrdinal || *availableOrdinal == std::numeric_limits<uint64_t>::max()) {
				return std::nullopt;
			}
			candidateOrdinal = *availableOrdinal + 1U;
		}
		return std::nullopt;
	}

	Superblock AllocatorDirectory::setAllocatedAndPublish(SuperblockFile &bootstrap, const Superblock &current,
														  PhysicalSlotId slot, bool allocated,
														  LogSequenceNumber checkpointLsn,
														  BootstrapPublishFailpoint failpoint) {
		static_cast<void>(SuperblockCodec::encode(current));
		if (!checkpointLsn.isValid() || checkpointLsn <= current.checkpoint_lsn ||
			current.generation.value() >= RSS_MAX_PHYSICAL_PAGE_GENERATION) {
			throwAllocatorDirectoryError(
					FormatError::FERR_INVALID_STATE,
					"relocatable free-space publication needs an advanced generation and checkpoint LSN");
		}
		if (bootstrap.storageIO() != pages_.storageIO()) {
			throwAllocatorDirectoryError(FormatError::FERR_INVALID_STATE,
										 "relocatable free-space directory requires shared bootstrap and page storage");
		}
		if (!sameSuperblock(bootstrap.recover(), current)) {
			throwAllocatorDirectoryError(FormatError::FERR_INVALID_STATE,
										 "relocatable free-space publication uses a stale Superblock generation");
		}
		if (isAllocated(current, slot) == allocated) {
			throwAllocatorDirectoryError(FormatError::FERR_INVALID_STATE,
										 "relocatable free-space transition does not change slot state");
		}
		MetadataWriteBatch batch(allocator_, current, checkpointLsn);
		prepareSetAllocated(batch, slot, allocated);
		batch.publish(bootstrap, failpoint);
		return batch.candidate();
	}

	void AllocatorDirectory::prepareSetAllocated(MetadataWriteBatch &batch, PhysicalSlotId slot, bool allocated) {
		if (!batch.usesAllocator(allocator_)) {
			throwAllocatorDirectoryError(
					FormatError::FERR_INVALID_STATE,
					"relocatable free-space directory and metadata batch use different page allocators");
		}
		const Superblock &current = batch.candidate();
		const Generation generation = batch.generation();
		const LogSequenceNumber checkpointLsn = batch.checkpointLsn();
		if (isAllocated(current, slot) == allocated) {
			throwAllocatorDirectoryError(FormatError::FERR_INVALID_STATE,
										 "relocatable free-space transition does not change slot state");
		}

		const uint64_t ordinal = leafOrdinal(slot);
		const uint16_t targetLevel = rootLevelFor(ordinal);
		struct WrittenRadixNode {
			PhysicalPageRef reference;
			bool has_free_slot = false;
		};
		std::function<WrittenRadixNode(const MetadataRadixPage &, bool)> writeNode;
		writeNode = [&](const MetadataRadixPage &source, bool isRoot) {
			MetadataRadixPage page = source;
			page.kind = isRoot ? CatalogRadixPageKind::CRPK_ROOT : CatalogRadixPageKind::CRPK_INTERNAL;
			page.directory_type = MetadataDirectoryType::MDT_ALLOCATOR;
			page.generation = generation;
			page.committed_lsn = checkpointLsn;
			const uint32_t index = childIndex(ordinal, page.level);
			if (page.level == 0U) {
				AllocatorLeafPage leaf;
				if (page.children[index].isValid()) {
					leaf = pages_.readAllocatorLeaf(page.children[index]);
					validateLeafRoute(leaf, ordinal, current);
				} else {
					leaf.page_id = batch.allocatePageId();
					leaf.first_slot = PhysicalSlotId(firstSlotFor(ordinal));
					leaf.slot_count = static_cast<uint32_t>(
							std::min<uint64_t>(RSS_ALLOCATOR_PAGE_SLOT_CAPACITY,
											   std::numeric_limits<uint64_t>::max() - leaf.first_slot.value() + 1U));
				}
				leaf.generation = generation;
				leaf.committed_lsn = checkpointLsn;
				setSlotBit(leaf, slot, allocated);
				const PhysicalPageRef leafReference = batch.allocatePage();
				pages_.writeAllocatorLeaf(leafReference, leaf);
				page.children[index] = leafReference;
				setAllocatorChildHasFree(page, index, leafHasFreeSlot(leaf));
			} else {
				MetadataRadixPage child;
				if (page.children[index].isValid()) {
					child = pages_.readMetadataRadix(page.children[index]);
					validateRadix(child, false, static_cast<uint16_t>(page.level - 1U), current);
				} else {
					child.kind = CatalogRadixPageKind::CRPK_INTERNAL;
					child.directory_type = MetadataDirectoryType::MDT_ALLOCATOR;
					child.level = static_cast<uint16_t>(page.level - 1U);
					child.page_id = batch.allocatePageId();
				}
				const WrittenRadixNode rewrittenChild = writeNode(child, false);
				page.children[index] = rewrittenChild.reference;
				setAllocatorChildHasFree(page, index, rewrittenChild.has_free_slot);
			}
			const PhysicalPageRef reference = batch.allocatePage();
			pages_.writeMetadataRadix(reference, page);
			return WrittenRadixNode{reference, radixSubtreeHasFreeSlot(page)};
		};

		MetadataRadixPage root;
		if (current.allocator_root.isValid()) {
			root = pages_.readMetadataRadix(current.allocator_root);
			validateRadix(root, true, root.level, current);
			while (root.level < targetLevel) {
				MetadataRadixPage oldRoot = root;
				oldRoot.kind = CatalogRadixPageKind::CRPK_INTERNAL;
				oldRoot.generation = generation;
				oldRoot.committed_lsn = checkpointLsn;
				const bool oldRootHasFreeSlot = radixSubtreeHasFreeSlot(oldRoot);
				const PhysicalPageRef oldRootReference = batch.allocatePage();
				pages_.writeMetadataRadix(oldRootReference, oldRoot);
				root = {};
				root.kind = CatalogRadixPageKind::CRPK_ROOT;
				root.directory_type = MetadataDirectoryType::MDT_ALLOCATOR;
				root.level = static_cast<uint16_t>(oldRoot.level + 1U);
				root.page_id = batch.allocatePageId();
				root.children[0] = oldRootReference;
				setAllocatorChildHasFree(root, 0U, oldRootHasFreeSlot);
			}
		} else {
			root.kind = CatalogRadixPageKind::CRPK_ROOT;
			root.directory_type = MetadataDirectoryType::MDT_ALLOCATOR;
			root.level = targetLevel;
			root.page_id = batch.allocatePageId();
		}
		batch.setAllocatorRoot(writeNode(root, true).reference);
	}

	std::optional<AllocatorReservation> AllocatorDirectory::reserveFirstFreeAndPublish(
			SuperblockFile &bootstrap, const Superblock &current, PhysicalSlotId firstSlot, PhysicalSlotId lastSlot,
			LogSequenceNumber checkpointLsn, BootstrapPublishFailpoint failpoint) {
		const std::optional<PhysicalSlotId> freeSlot = findFirstFree(current, firstSlot, lastSlot);
		if (!freeSlot.has_value()) {
			return std::nullopt;
		}
		return AllocatorReservation{
				setAllocatedAndPublish(bootstrap, current, *freeSlot, true, checkpointLsn, failpoint), *freeSlot};
	}

	std::optional<PhysicalSlotId> AllocatorDirectory::prepareReserveFirstFree(MetadataWriteBatch &batch,
																			  PhysicalSlotId firstSlot,
																			  PhysicalSlotId lastSlot) {
		const std::optional<PhysicalSlotId> freeSlot = findFirstFree(batch.candidate(), firstSlot, lastSlot);
		if (freeSlot.has_value()) {
			batch.reserveDataSlot(*freeSlot);
			prepareSetAllocated(batch, *freeSlot, true);
		}
		return freeSlot;
	}

} // namespace graph::storage
