/**
 * @file CatalogTree.cpp
 * @brief Copy-on-write mutation and checked lookup for relocatable Catalog pages.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/metadata/CatalogTree.hpp"

#include "graph/storage/relocatable/metadata/MetadataWriteBatch.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace graph::storage {

	namespace {

		constexpr uint64_t RSS_MAX_PHYSICAL_PAGE_GENERATION = std::numeric_limits<uint64_t>::max() >> 5U;
		static_assert(RSS_CATALOG_RADIX_MAX_LEVEL >= 7U);

		[[noreturn]] void throwCatalogTreeError(FormatError error, const char *message) {
			throw FormatException(error, message);
		}

		[[nodiscard]] bool sameSuperblock(const Superblock &left, const Superblock &right) { return left == right; }

		[[nodiscard]] uint64_t leafOrdinal(SegmentId segmentId) {
			return (segmentId.value() - 1U) / RSS_CATALOG_PAGE_MAX_ENTRIES;
		}

		[[nodiscard]] uint16_t requiredRootLevel(SegmentId maximumSegmentId) {
			uint64_t quotient = leafOrdinal(maximumSegmentId);
			uint16_t level = 0;
			while (quotient >= RSS_CATALOG_RADIX_FANOUT) {
				quotient /= RSS_CATALOG_RADIX_FANOUT;
				++level;
			}
			return level;
		}

		void validateLeafOrdinal(const CatalogLeafPage &page, uint64_t expectedOrdinal) {
			if (page.entries.empty()) {
				throwCatalogTreeError(FormatError::FERR_INVALID_STATE,
									  "a reachable relocatable Catalog leaf must not be empty");
			}
			for (const CatalogEntry &entry: page.entries) {
				if (leafOrdinal(entry.segment_id) != expectedOrdinal) {
					throwCatalogTreeError(FormatError::FERR_INVALID_HEADER,
										  "relocatable Catalog leaf does not match its radix route");
				}
			}
		}

		void validateVisiblePage(Generation pageGeneration, LogSequenceNumber pageLsn, const Superblock &superblock,
								 const char *message) {
			const uint8_t invalid = static_cast<uint8_t>(!pageGeneration.isValid()) |
									static_cast<uint8_t>(pageGeneration > superblock.generation) |
									static_cast<uint8_t>(!pageLsn.isValid()) |
									static_cast<uint8_t>(pageLsn > superblock.checkpoint_lsn);
			if (invalid != 0U) {
				throwCatalogTreeError(FormatError::FERR_INVALID_STATE, message);
			}
		}

		void validateMutationEntry(const CatalogEntry &entry, LogSequenceNumber checkpointLsn) {
			const uint16_t rawKind = static_cast<uint16_t>(entry.kind);
			const uint16_t rawState = static_cast<uint16_t>(entry.state);
			const uint8_t invalidEnum =
					static_cast<uint8_t>(rawKind < static_cast<uint16_t>(SegmentKind::SGK_ENTITY_PAGE_POOL)) |
					static_cast<uint8_t>(rawKind >
											static_cast<uint16_t>(SegmentKind::SGK_LARGE_VALUE_MANIFEST_POOL)) |
					static_cast<uint8_t>(rawState > static_cast<uint16_t>(SegmentState::SGS_OBSOLETE));
			if (invalidEnum != 0U) {
				throwCatalogTreeError(FormatError::FERR_INVALID_ENUM,
									  "relocatable Catalog mutation has an invalid kind or state");
			}
			const uint8_t allowedState = static_cast<uint8_t>(entry.state == SegmentState::SGS_SEALED) |
										 static_cast<uint8_t>(entry.state == SegmentState::SGS_RETIRED) |
										 static_cast<uint8_t>(entry.state == SegmentState::SGS_OBSOLETE);
			if (allowedState == 0U) {
				throwCatalogTreeError(FormatError::FERR_INVALID_STATE,
									  "relocatable Catalog mutation must be sealed, retired, or obsolete");
			}
			const uint8_t validIdentity = static_cast<uint8_t>(entry.segment_id.isValid()) &
										  static_cast<uint8_t>(entry.physical_slot.isValid()) &
										  static_cast<uint8_t>(entry.generation.isValid()) &
										  static_cast<uint8_t>(entry.committed_lsn.isValid()) &
										  static_cast<uint8_t>(entry.committed_lsn <= checkpointLsn);
			if (validIdentity == 0U) {
				throwCatalogTreeError(FormatError::FERR_INVALID_STATE,
									  "relocatable Catalog mutation has invalid identity or visibility");
			}
		}

		void validateMutationBatch(std::span<const CatalogEntry> entries, LogSequenceNumber checkpointLsn) {
			const uint8_t invalidBatch =
					static_cast<uint8_t>(entries.empty()) | static_cast<uint8_t>(!checkpointLsn.isValid());
			if (invalidBatch != 0U) {
				throwCatalogTreeError(FormatError::FERR_INVALID_HEADER,
									  "relocatable Catalog publication needs ordered entries and a checkpoint LSN");
			}
			SegmentId previous;
			for (const CatalogEntry &entry: entries) {
				validateMutationEntry(entry, checkpointLsn);
				if (previous.isValid() && entry.segment_id <= previous) {
					throwCatalogTreeError(FormatError::FERR_INVALID_HEADER,
										  "relocatable Catalog mutations must have strictly increasing Segment IDs");
				}
				previous = entry.segment_id;
			}
		}

		struct MutableLeaf final {
			CatalogLeafPage page;
		};

		struct MutableNode final {
			CatalogRadixPage page;
			std::map<uint32_t, std::unique_ptr<MutableNode>> node_children;
			std::map<uint32_t, std::unique_ptr<MutableLeaf>> leaf_children;
		};

		[[nodiscard]] std::unique_ptr<MutableNode> makeRadixNode(CatalogRadixPageKind kind, uint16_t level,
																 Generation generation, LogSequenceNumber lsn,
																 MetadataWriteBatch &batch) {
			auto node = std::make_unique<MutableNode>();
			node->page.kind = kind;
			node->page.level = level;
			node->page.page_id = batch.allocatePageId();
			node->page.generation = generation;
			node->page.committed_lsn = lsn;
			return node;
		}

		[[nodiscard]] std::unique_ptr<MutableLeaf> makeLeaf(Generation generation, LogSequenceNumber lsn,
															MetadataWriteBatch &batch) {
			auto leaf = std::make_unique<MutableLeaf>();
			leaf->page.page_id = batch.allocatePageId();
			leaf->page.generation = generation;
			leaf->page.committed_lsn = lsn;
			return leaf;
		}

		[[nodiscard]] std::unique_ptr<MutableNode> loadInternalNode(MetadataPageFile &pages, PhysicalPageRef reference,
																	uint16_t expectedLevel,
																	const Superblock &superblock) {
			CatalogRadixPage page = pages.readCatalogRadix(reference);
			if (page.kind != CatalogRadixPageKind::CRPK_INTERNAL || page.level != expectedLevel) {
				throwCatalogTreeError(FormatError::FERR_INVALID_HEADER,
									  "relocatable Catalog radix parent has an invalid child level or kind");
			}
			validateVisiblePage(page.generation, page.committed_lsn, superblock,
								"relocatable Catalog radix child is newer than its Superblock");
			auto node = std::make_unique<MutableNode>();
			node->page = std::move(page);
			return node;
		}

		[[nodiscard]] std::unique_ptr<MutableLeaf> loadLeaf(MetadataPageFile &pages, PhysicalPageRef reference,
															uint64_t ordinal, const Superblock &superblock) {
			CatalogLeafPage page = pages.readCatalogLeaf(reference);
			validateVisiblePage(page.generation, page.committed_lsn, superblock,
								"relocatable Catalog leaf is newer than its Superblock");
			validateLeafOrdinal(page, ordinal);
			auto leaf = std::make_unique<MutableLeaf>();
			leaf->page = std::move(page);
			return leaf;
		}

		void upsertLeaf(MutableLeaf &leaf, const CatalogEntry &entry, uint64_t ordinal, Generation generation,
						LogSequenceNumber lsn) {
			if (!leaf.page.entries.empty()) {
				validateLeafOrdinal(leaf.page, ordinal);
			}
			auto position = std::lower_bound(leaf.page.entries.begin(), leaf.page.entries.end(), entry.segment_id,
											 [](const CatalogEntry &candidate, SegmentId segmentId) {
												 return candidate.segment_id < segmentId;
											 });
			if (position != leaf.page.entries.end() && position->segment_id == entry.segment_id) {
				*position = entry;
			} else {
				leaf.page.entries.insert(position, entry);
			}
			leaf.page.minimum_segment_id = leaf.page.entries.front().segment_id;
			leaf.page.maximum_segment_id = leaf.page.entries.back().segment_id;
			leaf.page.generation = generation;
			leaf.page.committed_lsn = lsn;
		}

		void applyMutation(MutableNode &node, const CatalogEntry &entry, Generation generation, LogSequenceNumber lsn,
						   const Superblock &superblock, MetadataPageFile &pages, MetadataWriteBatch &batch) {
			node.page.generation = generation;
			node.page.committed_lsn = lsn;
			const uint32_t index = CatalogRadixPageCodec::childIndex(entry.segment_id, node.page.level);
			if (node.page.level == 0) {
				auto iterator = node.leaf_children.find(index);
				if (iterator == node.leaf_children.end()) {
					const uint64_t ordinal = leafOrdinal(entry.segment_id);
					std::unique_ptr<MutableLeaf> leaf =
							node.page.children[index].isValid()
									? loadLeaf(pages, node.page.children[index], ordinal, superblock)
									: makeLeaf(generation, lsn, batch);
					iterator = node.leaf_children.emplace(index, std::move(leaf)).first;
				}
				upsertLeaf(*iterator->second, entry, leafOrdinal(entry.segment_id), generation, lsn);
				return;
			}

			auto iterator = node.node_children.find(index);
			if (iterator == node.node_children.end()) {
				const uint16_t childLevel = node.page.level - 1U;
				std::unique_ptr<MutableNode> child =
						node.page.children[index].isValid()
								? loadInternalNode(pages, node.page.children[index], childLevel, superblock)
								: makeRadixNode(CatalogRadixPageKind::CRPK_INTERNAL, childLevel, generation, lsn,
												batch);
				iterator = node.node_children.emplace(index, std::move(child)).first;
			}
			applyMutation(*iterator->second, entry, generation, lsn, superblock, pages, batch);
		}

		void validateEraseBatch(std::span<const SegmentId> segmentIds) {
			if (segmentIds.empty()) {
				throwCatalogTreeError(FormatError::FERR_INVALID_HEADER,
								  "relocatable Catalog erase needs at least one Segment ID");
			}
			SegmentId previous;
			for (const SegmentId segmentId: segmentIds) {
				if (!segmentId.isValid() || (previous.isValid() && segmentId <= previous)) {
					throwCatalogTreeError(FormatError::FERR_INVALID_HEADER,
									  "relocatable Catalog erasures must use strictly ordered valid Segment IDs");
				}
				previous = segmentId;
			}
		}

		[[nodiscard]] bool hasChildren(const MutableNode &node) {
			return std::ranges::any_of(node.page.children,
									   [](PhysicalPageRef reference) { return reference.isValid(); });
		}

		[[nodiscard]] bool applyErase(MutableNode &node, SegmentId segmentId, Generation generation,
									 LogSequenceNumber lsn, const Superblock &superblock, MetadataPageFile &pages) {
			node.page.generation = generation;
			node.page.committed_lsn = lsn;
			const uint32_t index = CatalogRadixPageCodec::childIndex(segmentId, node.page.level);
			if (node.page.level == 0U) {
				auto iterator = node.leaf_children.find(index);
				if (iterator == node.leaf_children.end()) {
					if (!node.page.children[index].isValid()) {
						throwCatalogTreeError(FormatError::FERR_INVALID_STATE,
										  "relocatable Catalog erase targets an absent Segment ID");
					}
					iterator = node.leaf_children
							   .emplace(index, loadLeaf(pages, node.page.children[index], leafOrdinal(segmentId), superblock))
							   .first;
				}
				MutableLeaf &leaf = *iterator->second;
				const auto position = std::lower_bound(
						leaf.page.entries.begin(), leaf.page.entries.end(), segmentId,
						[](const CatalogEntry &entry, SegmentId target) { return entry.segment_id < target; });
				if (position == leaf.page.entries.end() || position->segment_id != segmentId) {
					throwCatalogTreeError(FormatError::FERR_INVALID_STATE,
									  "relocatable Catalog erase targets an absent Segment ID");
				}
				leaf.page.entries.erase(position);
				if (leaf.page.entries.empty()) {
					node.leaf_children.erase(iterator);
					node.page.children[index] = {};
					return !hasChildren(node);
				}
				leaf.page.minimum_segment_id = leaf.page.entries.front().segment_id;
				leaf.page.maximum_segment_id = leaf.page.entries.back().segment_id;
				leaf.page.generation = generation;
				leaf.page.committed_lsn = lsn;
				return false;
			}

			auto iterator = node.node_children.find(index);
			if (iterator == node.node_children.end()) {
				if (!node.page.children[index].isValid()) {
					throwCatalogTreeError(FormatError::FERR_INVALID_STATE,
									  "relocatable Catalog erase targets an absent Segment ID");
				}
				const uint16_t childLevel = node.page.level - 1U;
				iterator = node.node_children
						   .emplace(index, loadInternalNode(pages, node.page.children[index], childLevel, superblock))
						   .first;
			}
			if (applyErase(*iterator->second, segmentId, generation, lsn, superblock, pages)) {
				node.node_children.erase(iterator);
				node.page.children[index] = {};
			}
			return !hasChildren(node);
		}

		void normalizeNode(MutableNode &node, CatalogRadixPageKind kind, Generation generation, LogSequenceNumber lsn) {
			node.page.kind = kind;
			node.page.generation = generation;
			node.page.committed_lsn = lsn;
		}

		[[nodiscard]] PhysicalPageRef writeLeaf(MutableLeaf &leaf, Generation generation, LogSequenceNumber lsn,
												MetadataPageFile &pages, MetadataWriteBatch &batch) {
			leaf.page.generation = generation;
			leaf.page.committed_lsn = lsn;
			const PhysicalPageRef reference = batch.allocatePage();
			pages.writeCatalogLeaf(reference, leaf.page);
			return reference;
		}

		[[nodiscard]] PhysicalPageRef writeNode(MutableNode &node, CatalogRadixPageKind kind, Generation generation,
												LogSequenceNumber lsn, MetadataPageFile &pages,
												MetadataWriteBatch &batch) {
			normalizeNode(node, kind, generation, lsn);
			if (node.page.level == 0) {
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
			pages.writeCatalogRadix(reference, node.page);
			return reference;
		}

		[[nodiscard]] CatalogRadixPage readRoot(MetadataPageFile &pages, const Superblock &superblock) {
			CatalogRadixPage root = pages.readCatalogRadix(superblock.catalog_root);
			if (root.kind != CatalogRadixPageKind::CRPK_ROOT) {
				throwCatalogTreeError(FormatError::FERR_INVALID_HEADER,
									  "relocatable Catalog Superblock root is not a root radix page");
			}
			validateVisiblePage(root.generation, root.committed_lsn, superblock,
								"relocatable Catalog root is newer than its Superblock");
			return root;
		}

		[[nodiscard]] uint64_t radixStride(uint16_t level) {
			uint64_t stride = 1;
			for (uint16_t digit = 0; digit < level; ++digit) {
				if (stride > std::numeric_limits<uint64_t>::max() / RSS_CATALOG_RADIX_FANOUT) {
					throwCatalogTreeError(FormatError::FERR_ARITHMETIC_OVERFLOW,
										  "relocatable Catalog radix traversal stride overflows");
				}
				stride *= RSS_CATALOG_RADIX_FANOUT;
			}
			return stride;
		}

		void visitNode(MetadataPageFile &pages, const CatalogRadixPage &node, const Superblock &superblock,
					   uint64_t firstLeafOrdinal, std::set<PhysicalPageRef> &visitedPages,
					   const std::function<void(const CatalogEntry &)> &visitor) {
			const uint64_t stride = radixStride(node.level);
			for (uint32_t index = 0; index < RSS_CATALOG_RADIX_FANOUT; ++index) {
				const PhysicalPageRef reference = node.children[index];
				if (!reference.isValid()) {
					continue;
				}
				if (index > (std::numeric_limits<uint64_t>::max() - firstLeafOrdinal) / stride) {
					throwCatalogTreeError(FormatError::FERR_ARITHMETIC_OVERFLOW,
										  "relocatable Catalog radix traversal leaf ordinal overflows");
				}
				const uint64_t childFirstLeafOrdinal = firstLeafOrdinal + static_cast<uint64_t>(index) * stride;
				if (!visitedPages.insert(reference).second) {
					throwCatalogTreeError(FormatError::FERR_INVALID_STATE,
										  "relocatable Catalog traversal found a duplicate physical page reference");
				}
				if (node.level == 0U) {
					const CatalogLeafPage leaf = pages.readCatalogLeaf(reference);
					validateVisiblePage(leaf.generation, leaf.committed_lsn, superblock,
										"relocatable Catalog traversal leaf is newer than its Superblock");
					validateLeafOrdinal(leaf, childFirstLeafOrdinal);
					for (const CatalogEntry &entry: leaf.entries) {
						visitor(entry);
					}
					continue;
				}
				CatalogRadixPage child = pages.readCatalogRadix(reference);
				if (child.kind != CatalogRadixPageKind::CRPK_INTERNAL || child.level + 1U != node.level) {
					throwCatalogTreeError(FormatError::FERR_INVALID_HEADER,
										  "relocatable Catalog traversal has an invalid child level or kind");
				}
				validateVisiblePage(child.generation, child.committed_lsn, superblock,
									"relocatable Catalog traversal radix page is newer than its Superblock");
				visitNode(pages, child, superblock, childFirstLeafOrdinal, visitedPages, visitor);
			}
		}

	} // namespace

	CatalogTree::CatalogTree(MetadataPageFile &pages, MetadataPageAllocator &allocator) :
		pages_(pages), allocator_(allocator) {}

	Superblock CatalogTree::upsertAndPublish(SuperblockFile &bootstrap, const Superblock &current,
											 std::span<const CatalogEntry> entries, LogSequenceNumber checkpointLsn,
											 BootstrapPublishFailpoint failpoint) {
		if (pages_.storageIO().get() != bootstrap.storageIO().get()) {
			throwCatalogTreeError(FormatError::FERR_INVALID_STATE,
								  "relocatable Catalog page and Superblock files must share one StorageIO");
		}
		static_cast<void>(SuperblockCodec::encode(current));
		const uint8_t invalidPublication =
				static_cast<uint8_t>(!checkpointLsn.isValid()) |
				static_cast<uint8_t>(checkpointLsn <= current.checkpoint_lsn) |
				static_cast<uint8_t>(current.generation.value() >= RSS_MAX_PHYSICAL_PAGE_GENERATION);
		if (invalidPublication != 0U) {
			throwCatalogTreeError(
					FormatError::FERR_INVALID_STATE,
					"relocatable Catalog publication must advance a representable generation and checkpoint LSN");
		}
		if (!sameSuperblock(bootstrap.recover(), current)) {
			throwCatalogTreeError(FormatError::FERR_INVALID_STATE,
								  "relocatable Catalog publication uses a stale Superblock generation");
		}
		MetadataWriteBatch batch(allocator_, current, checkpointLsn);
		prepareUpsert(batch, entries);
		batch.publish(bootstrap, failpoint);
		return batch.candidate();
	}

	void CatalogTree::prepareUpsert(MetadataWriteBatch &batch, std::span<const CatalogEntry> entries) {
		if (!batch.usesAllocator(allocator_)) {
			throwCatalogTreeError(FormatError::FERR_INVALID_STATE,
								  "relocatable Catalog tree and metadata batch use different page allocators");
		}
		const Superblock &current = batch.candidate();
		const Generation generation = batch.generation();
		const LogSequenceNumber checkpointLsn = batch.checkpointLsn();
		validateMutationBatch(entries, checkpointLsn);

		const uint16_t targetLevel = requiredRootLevel(entries.back().segment_id);
		std::unique_ptr<MutableNode> root;
		if (!current.catalog_root.isValid()) {
			root = makeRadixNode(CatalogRadixPageKind::CRPK_ROOT, targetLevel, generation, checkpointLsn, batch);
		} else {
			root = std::make_unique<MutableNode>();
			root->page = readRoot(pages_, current);
			while (root->page.level < targetLevel) {
				root->page.kind = CatalogRadixPageKind::CRPK_INTERNAL;
				root->page.generation = generation;
				root->page.committed_lsn = checkpointLsn;
				auto grownRoot =
						makeRadixNode(CatalogRadixPageKind::CRPK_ROOT, static_cast<uint16_t>(root->page.level + 1U),
									  generation, checkpointLsn, batch);
				grownRoot->node_children.emplace(0U, std::move(root));
				root = std::move(grownRoot);
			}
		}

		for (const CatalogEntry &entry: entries) {
			applyMutation(*root, entry, generation, checkpointLsn, current, pages_, batch);
		}
		batch.setCatalogRoot(
				writeNode(*root, CatalogRadixPageKind::CRPK_ROOT, generation, checkpointLsn, pages_, batch));
	}

	void CatalogTree::prepareErase(MetadataWriteBatch &batch, std::span<const SegmentId> segmentIds) {
		if (!batch.usesAllocator(allocator_)) {
			throwCatalogTreeError(FormatError::FERR_INVALID_STATE,
							  "relocatable Catalog tree and metadata batch use different page allocators");
		}
		validateEraseBatch(segmentIds);
		const Superblock &current = batch.candidate();
		if (!current.catalog_root.isValid()) {
			throwCatalogTreeError(FormatError::FERR_INVALID_STATE,
							  "relocatable Catalog erase requires a published Catalog root");
		}
		const Generation generation = batch.generation();
		const LogSequenceNumber checkpointLsn = batch.checkpointLsn();
		auto root = std::make_unique<MutableNode>();
		root->page = readRoot(pages_, current);
		for (const SegmentId segmentId: segmentIds) {
			static_cast<void>(applyErase(*root, segmentId, generation, checkpointLsn, current, pages_));
		}
		batch.setCatalogRoot(
				writeNode(*root, CatalogRadixPageKind::CRPK_ROOT, generation, checkpointLsn, pages_, batch));
	}

	std::optional<CatalogEntry> CatalogTree::findOptional(const Superblock &superblock, SegmentId segmentId) const {
		static_cast<void>(SuperblockCodec::encode(superblock));
		if (!segmentId.isValid()) {
			throwCatalogTreeError(FormatError::FERR_INVALID_STATE, "relocatable Catalog lookup requires a valid Segment ID");
		}
		if (!superblock.catalog_root.isValid()) {
			return std::nullopt;
		}

		CatalogRadixPage node = readRoot(pages_, superblock);
		for (;;) {
			const std::optional<PhysicalPageRef> childReference = CatalogRadixPageCodec::findChildFor(node, segmentId);
			if (!childReference.has_value()) {
				return std::nullopt;
			}
			if (node.level == 0) {
				const CatalogLeafPage leaf = pages_.readCatalogLeaf(*childReference);
				validateVisiblePage(leaf.generation, leaf.committed_lsn, superblock,
									"relocatable Catalog leaf is newer than its Superblock");
				validateLeafOrdinal(leaf, leafOrdinal(segmentId));
				const auto iterator = std::lower_bound(
						leaf.entries.begin(), leaf.entries.end(), segmentId,
						[](const CatalogEntry &entry, SegmentId target) { return entry.segment_id < target; });
				if (iterator == leaf.entries.end() || iterator->segment_id != segmentId) {
					return std::nullopt;
				}
				return *iterator;
			}

			CatalogRadixPage child = pages_.readCatalogRadix(*childReference);
			if (child.kind != CatalogRadixPageKind::CRPK_INTERNAL || child.level + 1U != node.level) {
				throwCatalogTreeError(FormatError::FERR_INVALID_HEADER,
									  "relocatable Catalog radix route has an invalid child level or kind");
			}
			validateVisiblePage(child.generation, child.committed_lsn, superblock,
								"relocatable Catalog radix child is newer than its Superblock");
			node = std::move(child);
		}
	}

	CatalogEntry CatalogTree::find(const Superblock &superblock, SegmentId segmentId) const {
		static_cast<void>(SuperblockCodec::encode(superblock));
		const uint8_t invalidLookup =
				static_cast<uint8_t>(!segmentId.isValid()) | static_cast<uint8_t>(!superblock.catalog_root.isValid());
		if (invalidLookup != 0U) {
			throwCatalogTreeError(FormatError::FERR_INVALID_STATE,
							  "relocatable Catalog lookup requires a Segment ID and published Catalog root");
		}

		CatalogRadixPage node = readRoot(pages_, superblock);
		for (;;) {
			const PhysicalPageRef &childReference = CatalogRadixPageCodec::childFor(node, segmentId);
			if (node.level == 0) {
				const CatalogLeafPage leaf = pages_.readCatalogLeaf(childReference);
				validateVisiblePage(leaf.generation, leaf.committed_lsn, superblock,
									"relocatable Catalog leaf is newer than its Superblock");
				validateLeafOrdinal(leaf, leafOrdinal(segmentId));
				const auto iterator = std::lower_bound(
						leaf.entries.begin(), leaf.entries.end(), segmentId,
						[](const CatalogEntry &entry, SegmentId target) { return entry.segment_id < target; });
				if (iterator == leaf.entries.end() || iterator->segment_id != segmentId) {
					throwCatalogTreeError(FormatError::FERR_INVALID_STATE, "relocatable Catalog Segment ID is not present");
				}
				return *iterator;
			}

			CatalogRadixPage child = pages_.readCatalogRadix(childReference);
			if (child.kind != CatalogRadixPageKind::CRPK_INTERNAL || child.level + 1U != node.level) {
				throwCatalogTreeError(FormatError::FERR_INVALID_HEADER,
								  "relocatable Catalog radix route has an invalid child level or kind");
			}
			validateVisiblePage(child.generation, child.committed_lsn, superblock,
							"relocatable Catalog radix child is newer than its Superblock");
			node = std::move(child);
		}
	}

	void CatalogTree::visit(const Superblock &superblock,
							const std::function<void(const CatalogEntry &)> &visitor) const {
		static_cast<void>(SuperblockCodec::encode(superblock));
		if (!visitor) {
			throwCatalogTreeError(FormatError::FERR_INVALID_HEADER,
								  "relocatable Catalog traversal requires an entry visitor");
		}
		if (!superblock.catalog_root.isValid()) {
			return;
		}
		const CatalogRadixPage root = readRoot(pages_, superblock);
		std::set<PhysicalPageRef> visitedPages;
		static_cast<void>(visitedPages.insert(superblock.catalog_root));
		visitNode(pages_, root, superblock, 0, visitedPages, visitor);
	}

} // namespace graph::storage
