/**
 * @file CatalogTree.cpp
 * @brief Copy-on-write mutation and checked lookup for relocatable Catalog pages.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/metadata/CatalogTree.hpp"
#include "graph/debug/PerfTrace.hpp"

#include "graph/storage/relocatable/metadata/MetadataPageRouteDirectory.hpp"
#include "graph/storage/relocatable/metadata/MetadataWriteBatch.hpp"

#include <algorithm>
#include <bitset>
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



		void validateVisiblePage(Generation pageGeneration, LogSequenceNumber pageLsn, const Superblock &superblock,
								 const char *message);

		[[nodiscard]] uint64_t leafOrdinal(SegmentId segmentId) {
			return (segmentId.value() - 1U) / RSS_CATALOG_PAGE_MAX_ENTRIES;
		}

		[[nodiscard]] PhysicalPageRef catalogRootReference(MetadataPageFile &pages, const Superblock &superblock,
                MetadataPageId *expectedIdentity = nullptr) {
            if (expectedIdentity) *expectedIdentity = MetadataPageId{};
			if (!superblock.usesPageDirectoryBootstrap()) {
				return superblock.catalog_root;
			}
			if (!superblock.page_directory_root.isValid()) {
				throwCatalogTreeError(FormatError::FERR_INVALID_HEADER, "relocatable Catalog bootstrap root is absent");
			}
			const auto bootstrapView = pages.readPageDirectoryBootstrapView(superblock.page_directory_root);
            const auto &bootstrap = *bootstrapView;
			validateVisiblePage(bootstrap.generation, bootstrap.committed_lsn, superblock,
								"relocatable Catalog bootstrap is newer than its Superblock");
			// An unpublished batch keeps the prior header until all final COW pairs
			// have been collected. Its candidate root is authoritative for writers.
			if (bootstrap.generation != superblock.generation) {
				return superblock.catalog_root;
			}
			const MetadataPageId rootId =
					bootstrap.metadata_directory_roots[static_cast<size_t>(MetadataDirectoryRootOrdinal::MDRO_CATALOG)];
			if (!rootId.isValid()) {
				return {};
			}
            if (expectedIdentity) *expectedIdentity = rootId;
			MetadataPageRouteDirectory routes(pages);
			return routes.find(superblock, bootstrap.metadata_page_directory_root, rootId);
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

        void validateDecodedLeafOrdinal(const DecodedMetadataPageView<CatalogLeafPage> &page, uint64_t expectedOrdinal) {
            const auto &entries = page->entries;
            if (entries.empty()) {
                throwCatalogTreeError(FormatError::FERR_INVALID_STATE,
                    "a reachable relocatable Catalog leaf must not be empty");
            }
            // The codec validates strict ID order. leafOrdinal is monotonic,
            // so its two extrema prove membership for every immutable entry.
            if (leafOrdinal(entries.front().segment_id) != expectedOrdinal ||
                leafOrdinal(entries.back().segment_id) != expectedOrdinal) {
                throwCatalogTreeError(FormatError::FERR_INVALID_HEADER,
                    "relocatable Catalog leaf does not match its radix route");
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
					static_cast<uint8_t>(rawKind > static_cast<uint16_t>(SegmentKind::SGK_QUERY_SCRATCH)) |
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
			node->page.page_id = batch.allocateMetadataPageId();
			node->page.generation = generation;
			node->page.committed_lsn = lsn;
			return node;
		}

		[[nodiscard]] std::unique_ptr<MutableLeaf> makeLeaf(Generation generation, LogSequenceNumber lsn,
															MetadataWriteBatch &batch) {
			auto leaf = std::make_unique<MutableLeaf>();
			leaf->page.page_id = batch.allocateMetadataPageId();
			leaf->page.generation = generation;
			leaf->page.committed_lsn = lsn;
			return leaf;
		}

		[[nodiscard]] CatalogRadixPage readRadixForMutation(MetadataPageFile &pages, PhysicalPageRef reference,
																			const MetadataWriteBatch &batch, bool resolveAll = true) {
			const MetadataRadixPage unresolved = pages.readMetadataRadixUnresolved(reference);
			// Existing logical pages must follow the generation that published their
			// route. New batch-owned pages are physical until the next route branch.
			if (!unresolved.logical_children_enabled) {
				return pages.readCatalogRadix(reference, batch.candidate());
			}
		if (!resolveAll) return unresolved;
			CatalogRadixPage page = unresolved;
			const PageDirectoryBootstrap currentBootstrap =
					pages.readPageDirectoryBootstrap(batch.current().page_directory_root);
			for (size_t index = 0U; index < page.logical_children.size(); ++index) {
				const MetadataPageId childId = page.logical_children[index];
				if (!childId.isValid()) {
					continue;
				}
				const auto pending = batch.pendingMetadataPageRoute(childId);
				page.children[index] = pending.has_value()
						? *pending
						: MetadataPageRouteDirectory(pages).find(batch.current(), currentBootstrap.metadata_page_directory_root,
																			 childId);
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
			debug::PerfTrace::addValue("native_graph.catalog.mutation_child_route_resolutions", 1);
		}

		[[nodiscard]] std::unique_ptr<MutableNode> loadInternalNode(MetadataPageFile &pages, PhysicalPageRef reference,
															uint16_t expectedLevel,
															const MetadataWriteBatch &batch, bool resolveAll = true) {
			CatalogRadixPage page = readRadixForMutation(pages, reference, batch, resolveAll);
			if (page.kind != CatalogRadixPageKind::CRPK_INTERNAL || page.level != expectedLevel) {
				throwCatalogTreeError(FormatError::FERR_INVALID_HEADER,
									  "relocatable Catalog radix parent has an invalid child level or kind");
			}
			validateVisiblePage(page.generation, page.committed_lsn, batch.candidate(),
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
						   MetadataPageFile &pages, MetadataWriteBatch &batch) {
			node.page.generation = generation;
			node.page.committed_lsn = lsn;
			const uint32_t index = CatalogRadixPageCodec::childIndex(entry.segment_id, node.page.level);
			resolveMutationChild(node.page, index, batch.current(), pages, &batch);
			if (node.page.level == 0) {
				auto iterator = node.leaf_children.find(index);
				if (iterator == node.leaf_children.end()) {
					const uint64_t ordinal = leafOrdinal(entry.segment_id);
					std::unique_ptr<MutableLeaf> leaf =
							node.page.children[index].isValid()
							? loadLeaf(pages, node.page.children[index], ordinal, batch.candidate())
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
								? loadInternalNode(pages, node.page.children[index], childLevel, batch, false)
								: makeRadixNode(CatalogRadixPageKind::CRPK_INTERNAL, childLevel, generation, lsn,
												batch);
				iterator = node.node_children.emplace(index, std::move(child)).first;
			}
			applyMutation(*iterator->second, entry, generation, lsn, pages, batch);
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
									   [](PhysicalPageRef reference) { return reference.isValid(); }) ||
					std::ranges::any_of(node.page.logical_children,
									   [](MetadataPageId id) { return id.isValid(); });
		}

		[[nodiscard]] bool applyErase(MutableNode &node, SegmentId segmentId, Generation generation,
									  LogSequenceNumber lsn, MetadataPageFile &pages, MetadataWriteBatch &batch) {
			node.page.generation = generation;
			node.page.committed_lsn = lsn;
			const uint32_t index = CatalogRadixPageCodec::childIndex(segmentId, node.page.level);
			resolveMutationChild(node.page, index, batch.current(), pages, &batch);
			if (node.page.level == 0U) {
				auto iterator = node.leaf_children.find(index);
				if (iterator == node.leaf_children.end()) {
					if (!node.page.children[index].isValid()) {
						throwCatalogTreeError(FormatError::FERR_INVALID_STATE,
											  "relocatable Catalog erase targets an absent Segment ID");
					}
					iterator = node.leaf_children
									   .emplace(index, loadLeaf(pages, node.page.children[index],
																		leafOrdinal(segmentId), batch.candidate()))
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
					node.page.logical_children[index] = {};
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
								   .emplace(index,
											loadInternalNode(pages, node.page.children[index], childLevel, batch, false))
									   .first;
			}
			if (applyErase(*iterator->second, segmentId, generation, lsn, pages, batch)) {
				node.node_children.erase(iterator);
				node.page.children[index] = {};
				node.page.logical_children[index] = {};
			}
			return !hasChildren(node);
		}

		void normalizeNode(MutableNode &node, CatalogRadixPageKind kind, Generation generation, LogSequenceNumber lsn,
						   bool retainLogicalChildren) {
			node.page.kind = kind;
			node.page.generation = generation;
			node.page.committed_lsn = lsn;
			if (node.page.logical_children_enabled && !retainLogicalChildren) {
				// Candidate COW pages remain physical until the final route branch is ready.
				node.page.logical_children.fill({});
				node.page.logical_children_enabled = false;
				node.page.logical_children_resolved = false;
			}
		}

		[[nodiscard]] PhysicalPageRef writeLeaf(MutableLeaf &leaf, Generation generation, LogSequenceNumber lsn,
												MetadataPageFile &pages, MetadataWriteBatch &batch) {
			leaf.page.generation = generation;
			leaf.page.committed_lsn = lsn;
			const PhysicalPageRef reference = batch.allocatePage();
			pages.writeCatalogLeaf(reference, leaf.page);
			batch.registerMetadataPageRoute(leaf.page.page_id, reference);
			return reference;
		}

		[[nodiscard]] MetadataPageId readChildPageId(MetadataPageFile &pages, PhysicalPageRef reference,
													 bool radixChild) {
			if (!reference.isValid()) {
				throwCatalogTreeError(FormatError::FERR_INVALID_HEADER,
									  "relocatable Catalog radix child reference is invalid");
			}
			return radixChild ? pages.readMetadataRadixUnresolved(reference).page_id
							  : pages.readCatalogLeafView(reference)->page_id;
		}

		void encodeLogicalChildren(CatalogRadixPage &page, MetadataPageFile &pages) {
			for (size_t index = 0; index < page.children.size(); ++index) {
				const PhysicalPageRef reference = page.children[index];
				if (!reference.isValid()) {
					page.logical_children[index] = {};
					continue;
				}
				page.logical_children[index] = readChildPageId(pages, reference, page.level != 0U);
				page.children[index] = {};
			}
			page.logical_children_enabled = true;
		}

		[[nodiscard]] PhysicalPageRef logicalizeOwnedInternalChildren(PhysicalPageRef reference,
																 MetadataPageFile &pages, MetadataWriteBatch &batch) {
			if (!batch.ownsAllocatedPage(reference)) {
				return reference;
			}
			// Candidate child identities cannot be resolved before the route branch
			// is published, so preserve the encoded form during logicalization.
			CatalogRadixPage page = pages.readMetadataRadixUnresolved(reference);
			if ((page.kind != CatalogRadixPageKind::CRPK_ROOT && page.kind != CatalogRadixPageKind::CRPK_INTERNAL) ||
				page.directory_type != MetadataDirectoryType::MDT_CATALOG || page.generation != batch.generation() ||
				page.committed_lsn != batch.checkpointLsn()) {
				throwCatalogTreeError(FormatError::FERR_INVALID_STATE,
								  "relocatable Catalog logical-child publication found an invalid candidate radix page");
			}
			if (page.logical_children_enabled) {
				return reference;
			}
			if (page.level != 0U) {
				for (PhysicalPageRef &child: page.children) {
					if (child.isValid()) {
						child = logicalizeOwnedInternalChildren(child, pages, batch);
					}
				}
			}
			encodeLogicalChildren(page, pages);
			const PhysicalPageRef logicalReference = batch.allocatePage();
			pages.writeCatalogRadix(logicalReference, page);
			batch.registerMetadataPageRoute(page.page_id, logicalReference);
			return logicalReference;
		}

		[[nodiscard]] PhysicalPageRef writeNode(MutableNode &node, CatalogRadixPageKind kind, Generation generation,
												LogSequenceNumber lsn, MetadataPageFile &pages,
												MetadataWriteBatch &batch) {
			const bool retainLogicalChildren = batch.candidate().usesPageDirectoryBootstrap();
			if (retainLogicalChildren && !node.page.logical_children_enabled) {
				for (size_t index = 0U; index < node.page.children.size(); ++index) {
					if (node.page.children[index].isValid() && !node.leaf_children.contains(index) &&
						!node.node_children.contains(index))
						node.page.logical_children[index] = readChildPageId(
								pages, node.page.children[index], node.page.level != 0U);
				}
				node.page.logical_children_enabled = true;
			}
			normalizeNode(node, kind, generation, lsn, retainLogicalChildren);
			if (node.page.level == 0) {
				for (auto &[index, leaf]: node.leaf_children) {
					node.page.children[index] = writeLeaf(*leaf, generation, lsn, pages, batch);
					if (retainLogicalChildren) node.page.logical_children[index] = leaf->page.page_id;
				}
			} else {
				for (auto &[index, child]: node.node_children) {
					node.page.children[index] =
							writeNode(*child, CatalogRadixPageKind::CRPK_INTERNAL, generation, lsn, pages, batch);
					if (retainLogicalChildren) node.page.logical_children[index] = child->page.page_id;
				}
			}
			if (retainLogicalChildren) {
				node.page.children.fill({});
				node.page.logical_children_resolved = false;
				debug::PerfTrace::addValue("native_graph.catalog.retained_logical_radix_pages", 1);
			}
			const PhysicalPageRef reference = batch.allocatePage();
			pages.writeCatalogRadix(reference, node.page);
			batch.registerMetadataPageRoute(node.page.page_id, reference);
			return reference;
		}

		[[nodiscard]] CatalogRadixPage readRootForMutation(MetadataPageFile &pages, const MetadataWriteBatch &batch, bool resolveAll = true) {
			CatalogRadixPage root = readRadixForMutation(pages, catalogRootReference(pages, batch.candidate()), batch, resolveAll);
			if (root.kind != CatalogRadixPageKind::CRPK_ROOT) {
				throwCatalogTreeError(FormatError::FERR_INVALID_HEADER,
									  "relocatable Catalog Superblock root is not a root radix page");
			}
			validateVisiblePage(root.generation, root.committed_lsn, batch.candidate(),
								"relocatable Catalog root is newer than its mutation batch");
			return root;
		}

		[[nodiscard]] CatalogRadixPage readRoot(MetadataPageFile &pages, const Superblock &superblock) {
			CatalogRadixPage root = pages.readCatalogRadix(catalogRootReference(pages, superblock), superblock);
			if (root.kind != CatalogRadixPageKind::CRPK_ROOT) {
				throwCatalogTreeError(FormatError::FERR_INVALID_HEADER,
									  "relocatable Catalog Superblock root is not a root radix page");
			}
			validateVisiblePage(root.generation, root.committed_lsn, superblock,
								"relocatable Catalog root is newer than its Superblock");
			return root;
		}

        // Point lookups authenticate the complete codec page, then resolve only
        // the requested child through the snapshot's published route branch.
        // Full traversal and mutable COW preparation still resolve every child.
        [[nodiscard]] DecodedMetadataPageView<CatalogRadixPage> readLookupRadix(MetadataPageFile &pages, PhysicalPageRef reference,
                MetadataPageId expectedIdentity = {}) {
            auto page = pages.readMetadataRadixUnresolvedView(reference);
            if (page->directory_type != MetadataDirectoryType::MDT_CATALOG) {
                throwCatalogTreeError(FormatError::FERR_INVALID_STATE,
                    "relocatable Catalog radix has an unexpected metadata directory type");
            }
            if (expectedIdentity.isValid() && page->page_id != expectedIdentity) {
                throwCatalogTreeError(FormatError::FERR_INVALID_STATE,
                    "relocatable Catalog radix identity does not match its logical route");
            }
            return page;
        }

        [[nodiscard]] std::optional<PhysicalPageRef> resolveLookupChild(MetadataPageFile &pages,
                const CatalogRadixPage &page, const Superblock &superblock, SegmentId id, bool optional) {
            const auto ordinal = leafOrdinal(id);
            if (page.kind == CatalogRadixPageKind::CRPK_ROOT) {
                uint64_t quotient = ordinal;
                for (uint16_t level = 0U; level <= page.level; ++level) quotient /= RSS_CATALOG_RADIX_FANOUT;
                if (quotient != 0U) {
                    pages.resolveMetadataRadixChildren(page, superblock, {}, {});
                    if (optional) return std::nullopt;
                    throwCatalogTreeError(FormatError::FERR_INVALID_HEADER,
                        "relocatable metadata radix root does not cover the requested leaf ordinal");
                }
            }
            const std::array<uint32_t, 1U> indices{CatalogRadixPageCodec::childIndexForLeafOrdinal(ordinal, page.level)};
            std::array<PhysicalPageRef, 1U> references{};
            pages.resolveMetadataRadixChildren(page, superblock, indices, references);
            if (references.front().isValid()) return references.front();
            if (optional) return std::nullopt;
            throwCatalogTreeError(FormatError::FERR_INVALID_HEADER,
                "relocatable metadata radix route has no child page");
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
					const auto leaf = pages.readCatalogLeafView(reference);
					validateVisiblePage(leaf->generation, leaf->committed_lsn, superblock,
										"relocatable Catalog traversal leaf is newer than its Superblock");
					validateDecodedLeafOrdinal(leaf, childFirstLeafOrdinal);
					for (const CatalogEntry &entry: leaf->entries) {
						visitor(entry);
					}
					continue;
				}
				CatalogRadixPage child = pages.readCatalogRadix(reference, superblock);
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

	struct CatalogAppendSession::Impl final {
		struct PendingNode final {
			uint64_t group = 0U;
			MutableNode node;
			std::bitset<RSS_CATALOG_RADIX_FANOUT> assigned_children;
		};

		MetadataPageFile &pages;
		MetadataWriteBatch &batch;
		const Superblock &current;
		uint64_t expectedSegmentId;
		size_t expectedCount;
		size_t observed = 0U;
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
			 SegmentId firstSegmentId, SegmentId lastSegmentId, size_t count) :
			pages(pageFile), batch(writeBatch), current(batch.candidate()), expectedSegmentId(firstSegmentId.value()),
			expectedCount(count), targetLevel(requiredRootLevel(lastSegmentId)),
			grownSourceExtensions(static_cast<size_t>(targetLevel)), pending(static_cast<size_t>(targetLevel) + 1U) {
			if (!batch.usesAllocator(allocator) || !firstSegmentId.isValid() || !lastSegmentId.isValid() ||
				lastSegmentId < firstSegmentId || count == 0U ||
				count - 1U > lastSegmentId.value() - firstSegmentId.value() ||
				lastSegmentId.value() - firstSegmentId.value() != count - 1U) {
				throwCatalogTreeError(
						FormatError::FERR_INVALID_HEADER,
						"relocatable streamed Catalog append requires one exact contiguous Segment-ID range");
			}
			if (current.catalog_root.isValid()) {
				sourceRoot = readRootForMutation(pages, batch);
				sourceRootLevel = sourceRoot->level;
			}
			if (sourceRoot.has_value() && targetLevel > sourceRootLevel) {
				// Only the top-level frame may carry CRPK_ROOT. Preserve the old
				// snapshot and attach a demoted COW copy below the grown root.
				MutableNode demotedSourceRoot;
				demotedSourceRoot.page = *sourceRoot;
				PhysicalPageRef preserved = writeNode(demotedSourceRoot, CatalogRadixPageKind::CRPK_INTERNAL,
													  batch.generation(), batch.checkpointLsn(), pages, batch);
				grownSourceExtensions[sourceRootLevel] = preserved;
				for (uint16_t level = static_cast<uint16_t>(sourceRootLevel + 1U); level < targetLevel; ++level) {
					MutableNode extension;
					extension.page.kind = CatalogRadixPageKind::CRPK_INTERNAL;
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
				return group == 0U
							   ? std::optional<CatalogRadixPage>(loadInternalNode(pages, *grownSourceExtensions[level],
																		  level, batch)
																		 ->page)
							   : std::nullopt;
			}
			if (!sourceRoot.has_value() || sourceRootLevel < level) {
				return std::nullopt;
			}
			if (sourceRootLevel == level) {
				return group == 0U ? sourceRoot : std::nullopt;
			}
			uint64_t factor = 1U;
			for (uint16_t index = 0U; index <= level; ++index) {
				if (factor > std::numeric_limits<uint64_t>::max() / RSS_CATALOG_RADIX_FANOUT) {
					throwCatalogTreeError(FormatError::FERR_ARITHMETIC_OVERFLOW,
										  "relocatable streamed Catalog radix route overflows its logical domain");
				}
				factor *= RSS_CATALOG_RADIX_FANOUT;
			}
			if (group > std::numeric_limits<uint64_t>::max() / factor) {
				throwCatalogTreeError(FormatError::FERR_ARITHMETIC_OVERFLOW,
									  "relocatable streamed Catalog radix group overflows its logical domain");
			}
			CatalogRadixPage node = *sourceRoot;
			const uint64_t leaf = group * factor;
			for (uint16_t nodeLevel = sourceRootLevel; nodeLevel > level; --nodeLevel) {
				const PhysicalPageRef child =
						node.children[CatalogRadixPageCodec::childIndexForLeafOrdinal(leaf, nodeLevel)];
				if (!child.isValid()) {
					return std::nullopt;
				}
				node = loadInternalNode(pages, child, static_cast<uint16_t>(nodeLevel - 1U), batch)->page;
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
			result.page.level = level;
			result.page.page_id = batch.allocateMetadataPageId();
			result.page.generation = batch.generation();
			result.page.committed_lsn = batch.checkpointLsn();
			if (grownSourceRootChild.has_value() && level == targetLevel && group == 0U) {
				result.page.children[0U] = *grownSourceRootChild;
			}
			return result;
		}

		void appendChild(uint16_t level, uint64_t childOrdinal, PhysicalPageRef child) {
			const uint64_t group = childOrdinal / RSS_CATALOG_RADIX_FANOUT;
			const uint32_t childIndex = static_cast<uint32_t>(childOrdinal % RSS_CATALOG_RADIX_FANOUT);
			if (level > targetLevel || (level == targetLevel && group != 0U)) {
				throwCatalogTreeError(FormatError::FERR_INVALID_STATE,
									  "relocatable streamed Catalog append has an out-of-range radix child");
			}
			if (pending[level].has_value() && pending[level]->group != group) {
				flushLevel(level);
			}
			if (!pending[level].has_value()) {
				pending[level].emplace(PendingNode{group, beginNode(level, group), {}});
			}
			CatalogRadixPage &page = pending[level]->node.page;
			if (pending[level]->assigned_children.test(childIndex) && page.children[childIndex] != child) {
				throwCatalogTreeError(FormatError::FERR_INVALID_STATE,
									  "relocatable streamed Catalog append assigned one radix child twice");
			}
			page.children[childIndex] = child;
			pending[level]->assigned_children.set(childIndex);
		}

		void flushLevel(uint16_t level) {
			if (!pending[level].has_value()) {
				return;
			}
			PendingNode completed = std::move(*pending[level]);
			pending[level].reset();
			const PhysicalPageRef reference = writeNode(completed.node,
														level == targetLevel ? CatalogRadixPageKind::CRPK_ROOT
																			 : CatalogRadixPageKind::CRPK_INTERNAL,
														batch.generation(), batch.checkpointLsn(), pages, batch);
			if (level == targetLevel) {
				if (completed.group != 0U) {
					throwCatalogTreeError(FormatError::FERR_INVALID_STATE,
										  "relocatable streamed Catalog append exceeded its planned root coverage");
				}
				batch.setCatalogRoot(reference);
				return;
			}
			appendChild(static_cast<uint16_t>(level + 1U), completed.group, reference);
		}

		void finishLeaf() {
			if (activeLeaf == nullptr) {
				return;
			}
			const PhysicalPageRef reference =
					writeLeaf(*activeLeaf, batch.generation(), batch.checkpointLsn(), pages, batch);
			appendChild(0U, activeLeafOrdinal, reference);
			activeLeaf.reset();
		}

		void append(const CatalogEntry &entry) {
			if (finished || observed == expectedCount || entry.segment_id.value() != expectedSegmentId) {
				throwCatalogTreeError(
						FormatError::FERR_INVALID_STATE,
						"relocatable streamed Catalog source diverged from its immutable Segment-ID range");
			}
			validateMutationEntry(entry, batch.checkpointLsn());
			batch.observeLogicalSegmentId(entry.segment_id);
			const uint64_t leaf = leafOrdinal(entry.segment_id);
			if (activeLeaf == nullptr || activeLeafOrdinal != leaf) {
				finishLeaf();
				activeLeafOrdinal = leaf;
				const uint64_t parentGroup = leaf / RSS_CATALOG_RADIX_FANOUT;
				if (const auto parent = sourceNode(0U, parentGroup); parent.has_value()) {
					const PhysicalPageRef reference =
							parent->children[CatalogRadixPageCodec::childIndexForLeafOrdinal(leaf, 0U)];
					activeLeaf = reference.isValid() ? loadLeaf(pages, reference, leaf, current)
													 : makeLeaf(batch.generation(), batch.checkpointLsn(), batch);
				} else {
					activeLeaf = makeLeaf(batch.generation(), batch.checkpointLsn(), batch);
				}
			}
			auto position = std::lower_bound(activeLeaf->page.entries.begin(), activeLeaf->page.entries.end(),
											 entry.segment_id, [](const CatalogEntry &candidate, SegmentId segmentId) {
												 return candidate.segment_id < segmentId;
											 });
			if (position != activeLeaf->page.entries.end() && position->segment_id == entry.segment_id) {
				throwCatalogTreeError(
						FormatError::FERR_INVALID_STATE,
						"relocatable streamed Catalog append attempted to publish an existing Segment ID");
			}
			activeLeaf->page.entries.insert(position, entry);
			activeLeaf->page.minimum_segment_id = activeLeaf->page.entries.front().segment_id;
			activeLeaf->page.maximum_segment_id = activeLeaf->page.entries.back().segment_id;
			activeLeaf->page.generation = batch.generation();
			activeLeaf->page.committed_lsn = batch.checkpointLsn();
			validateLeafOrdinal(activeLeaf->page, leaf);
			++observed;
			if (expectedSegmentId != std::numeric_limits<uint64_t>::max()) {
				++expectedSegmentId;
			}
		}

		void finish() {
			if (finished || observed != expectedCount) {
				throwCatalogTreeError(FormatError::FERR_INVALID_STATE,
									  "relocatable streamed Catalog append did not consume its exact bounded source");
			}
			finishLeaf();
			for (uint16_t level = 0U; level <= targetLevel; ++level) {
				flushLevel(level);
			}
			finished = true;
		}
	};

	CatalogAppendSession::CatalogAppendSession(std::unique_ptr<Impl> implementation) noexcept :
		implementation_(std::move(implementation)) {}
	CatalogAppendSession::CatalogAppendSession(CatalogAppendSession &&) noexcept = default;
	CatalogAppendSession &CatalogAppendSession::operator=(CatalogAppendSession &&) noexcept = default;
	CatalogAppendSession::~CatalogAppendSession() = default;
	void CatalogAppendSession::append(const CatalogEntry &entry) {
		if (implementation_ == nullptr) {
			throwCatalogTreeError(FormatError::FERR_INVALID_STATE, "relocatable Catalog append session is not active");
		}
		implementation_->append(entry);
	}
	void CatalogAppendSession::finish() {
		if (implementation_ == nullptr) {
			throwCatalogTreeError(FormatError::FERR_INVALID_STATE, "relocatable Catalog append session is not active");
		}
		implementation_->finish();
	}
	bool CatalogAppendSession::isFinished() const noexcept {
		return implementation_ != nullptr && implementation_->finished;
	}

	CatalogAppendSession CatalogTree::beginAppend(MetadataWriteBatch &batch, SegmentId firstSegmentId,
												  SegmentId lastSegmentId, size_t expectedCount) {
		return CatalogAppendSession(std::make_unique<CatalogAppendSession::Impl>(
				pages_, allocator_, batch, firstSegmentId, lastSegmentId, expectedCount));
	}

	void CatalogTree::prepareAppend(MetadataWriteBatch &batch, SegmentId firstSegmentId, SegmentId lastSegmentId,
									size_t expectedCount, const EntryCursor &cursor) {
		if (!cursor) {
			throwCatalogTreeError(FormatError::FERR_INVALID_HEADER,
								  "relocatable streamed Catalog append requires a source cursor");
		}
		auto session = beginAppend(batch, firstSegmentId, lastSegmentId, expectedCount);
		for (;;) {
			const std::optional<CatalogEntry> entry = cursor();
			if (!entry.has_value()) {
				break;
			}
			session.append(*entry);
		}
		session.finish();
	}

	void CatalogTree::prepareAppend(MetadataWriteBatch &batch, SegmentId firstSegmentId, SegmentId lastSegmentId,
									size_t expectedCount, const EntryProducer &producer) {
		if (!producer) {
			throwCatalogTreeError(FormatError::FERR_INVALID_HEADER,
								  "relocatable streamed Catalog append requires a source producer");
		}
		auto session = beginAppend(batch, firstSegmentId, lastSegmentId, expectedCount);
		producer([&session](const CatalogEntry &entry) { session.append(entry); });
		session.finish();
	}

	Superblock CatalogTree::upsertAndPublish(SuperblockFile &bootstrap, const Superblock &current,
											 std::span<const CatalogEntry> entries, LogSequenceNumber checkpointLsn,
											 BootstrapPublishFailpoint failpoint) {
		if (pages_.storageIO().get() != bootstrap.storageIO().get()) {
			throwCatalogTreeError(FormatError::FERR_INVALID_STATE,
								  "relocatable Catalog page and Superblock files must share one StorageIO");
		}
		SuperblockCodec::validate(current);
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
		if (!catalogRootReference(pages_, current).isValid()) {
			root = makeRadixNode(CatalogRadixPageKind::CRPK_ROOT, targetLevel, generation, checkpointLsn, batch);
		} else {
			root = std::make_unique<MutableNode>();
			root->page = readRootForMutation(pages_, batch, false);
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
			applyMutation(*root, entry, generation, checkpointLsn, pages_, batch);
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
		if (!catalogRootReference(pages_, current).isValid()) {
			throwCatalogTreeError(FormatError::FERR_INVALID_STATE,
								  "relocatable Catalog erase requires a published Catalog root");
		}
		const Generation generation = batch.generation();
		const LogSequenceNumber checkpointLsn = batch.checkpointLsn();
		auto root = std::make_unique<MutableNode>();
		root->page = readRootForMutation(pages_, batch, false);
		for (const SegmentId segmentId: segmentIds) {
			static_cast<void>(applyErase(*root, segmentId, generation, checkpointLsn, pages_, batch));
		}
		batch.setCatalogRoot(
				writeNode(*root, CatalogRadixPageKind::CRPK_ROOT, generation, checkpointLsn, pages_, batch));
	}

	void CatalogTree::prepareLogicalRootChildren(MetadataWriteBatch &batch) {
		if (!batch.candidate().usesPageDirectoryBootstrap() || !batch.candidate().catalog_root.isValid() ||
			!batch.ownsAllocatedPage(batch.candidate().catalog_root)) {
			return;
		}
		batch.setCatalogRoot(logicalizeOwnedInternalChildren(batch.candidate().catalog_root, pages_, batch));
	}

	std::optional<CatalogEntry> CatalogTree::findOptional(const Superblock &superblock, SegmentId segmentId) const {
		SuperblockCodec::validate(superblock);
		if (!segmentId.isValid()) {
			throwCatalogTreeError(FormatError::FERR_INVALID_STATE,
								  "relocatable Catalog lookup requires a valid Segment ID");
		}
		MetadataPageId rootIdentity;
        const auto rootReference = catalogRootReference(pages_, superblock, &rootIdentity);
		if (!rootReference.isValid()) {
			return std::nullopt;
		}

		auto nodeView = readLookupRadix(pages_, rootReference, rootIdentity);
        const auto *node = nodeView.get();
        if (node->kind != CatalogRadixPageKind::CRPK_ROOT) {
            throwCatalogTreeError(FormatError::FERR_INVALID_HEADER,
                "relocatable Catalog Superblock root is not a root radix page");
        }
        validateVisiblePage(node->generation, node->committed_lsn, superblock,
            "relocatable Catalog root is newer than its Superblock");
		for (;;) {
			const std::optional<PhysicalPageRef> childReference = resolveLookupChild(pages_, *node, superblock, segmentId, true);
			if (!childReference.has_value()) {
				return std::nullopt;
			}
            const MetadataPageId childIdentity = node->logical_children_enabled
                ? node->logical_children[CatalogRadixPageCodec::childIndexForLeafOrdinal(leafOrdinal(segmentId), node->level)]
                : MetadataPageId{};
			if (node->level == 0) {
				const auto leaf = pages_.readCatalogLeafView(*childReference);
                if (childIdentity.isValid() && leaf->page_id != childIdentity) {
                    throwCatalogTreeError(FormatError::FERR_INVALID_STATE,
                        "relocatable Catalog leaf identity does not match its logical route");
                }
				validateVisiblePage(leaf->generation, leaf->committed_lsn, superblock,
									"relocatable Catalog leaf is newer than its Superblock");
				validateDecodedLeafOrdinal(leaf, leafOrdinal(segmentId));
				const auto iterator = std::lower_bound(
						leaf->entries.begin(), leaf->entries.end(), segmentId,
						[](const CatalogEntry &entry, SegmentId target) { return entry.segment_id < target; });
				if (iterator == leaf->entries.end() || iterator->segment_id != segmentId) {
					return std::nullopt;
				}
				return *iterator;
			}

			auto child = readLookupRadix(pages_, *childReference, childIdentity);
			if (child->kind != CatalogRadixPageKind::CRPK_INTERNAL || child->level + 1U != node->level) {
				throwCatalogTreeError(FormatError::FERR_INVALID_HEADER,
									  "relocatable Catalog radix route has an invalid child level or kind");
			}
			validateVisiblePage(child->generation, child->committed_lsn, superblock,
								"relocatable Catalog radix child is newer than its Superblock");
			nodeView = std::move(child);
            node = nodeView.get();
		}
	}

	CatalogEntry CatalogTree::find(const Superblock &superblock, SegmentId segmentId) const {
		SuperblockCodec::validate(superblock);
		MetadataPageId rootIdentity;
        const auto rootReference = catalogRootReference(pages_, superblock, &rootIdentity);
		const uint8_t invalidLookup = static_cast<uint8_t>(!segmentId.isValid()) |
									  static_cast<uint8_t>(!rootReference.isValid());
		if (invalidLookup != 0U) {
			throwCatalogTreeError(FormatError::FERR_INVALID_STATE,
								  "relocatable Catalog lookup requires a Segment ID and published Catalog root");
		}

		auto nodeView = readLookupRadix(pages_, rootReference, rootIdentity);
        const auto *node = nodeView.get();
        if (node->kind != CatalogRadixPageKind::CRPK_ROOT) {
            throwCatalogTreeError(FormatError::FERR_INVALID_HEADER,
                "relocatable Catalog Superblock root is not a root radix page");
        }
        validateVisiblePage(node->generation, node->committed_lsn, superblock,
            "relocatable Catalog root is newer than its Superblock");
		for (;;) {
			const PhysicalPageRef childReference = *resolveLookupChild(pages_, *node, superblock, segmentId, false);
            const MetadataPageId childIdentity = node->logical_children_enabled
                ? node->logical_children[CatalogRadixPageCodec::childIndexForLeafOrdinal(leafOrdinal(segmentId), node->level)]
                : MetadataPageId{};
			if (node->level == 0) {
				const auto leaf = pages_.readCatalogLeafView(childReference);
                if (childIdentity.isValid() && leaf->page_id != childIdentity) {
                    throwCatalogTreeError(FormatError::FERR_INVALID_STATE,
                        "relocatable Catalog leaf identity does not match its logical route");
                }
				validateVisiblePage(leaf->generation, leaf->committed_lsn, superblock,
									"relocatable Catalog leaf is newer than its Superblock");
				validateDecodedLeafOrdinal(leaf, leafOrdinal(segmentId));
				const auto iterator = std::lower_bound(
						leaf->entries.begin(), leaf->entries.end(), segmentId,
						[](const CatalogEntry &entry, SegmentId target) { return entry.segment_id < target; });
				if (iterator == leaf->entries.end() || iterator->segment_id != segmentId) {
					throwCatalogTreeError(FormatError::FERR_INVALID_STATE,
										  "relocatable Catalog Segment ID is not present");
				}
				return *iterator;
			}

			auto child = readLookupRadix(pages_, childReference, childIdentity);
			if (child->kind != CatalogRadixPageKind::CRPK_INTERNAL || child->level + 1U != node->level) {
				throwCatalogTreeError(FormatError::FERR_INVALID_HEADER,
									  "relocatable Catalog radix route has an invalid child level or kind");
			}
			validateVisiblePage(child->generation, child->committed_lsn, superblock,
								"relocatable Catalog radix child is newer than its Superblock");
			nodeView = std::move(child);
            node = nodeView.get();
		}
	}

	void CatalogTree::visit(const Superblock &superblock,
							const std::function<void(const CatalogEntry &)> &visitor) const {
		SuperblockCodec::validate(superblock);
		if (!visitor) {
			throwCatalogTreeError(FormatError::FERR_INVALID_HEADER,
								  "relocatable Catalog traversal requires an entry visitor");
		}
		const PhysicalPageRef rootReference = catalogRootReference(pages_, superblock);
		if (!rootReference.isValid()) {
			return;
		}
		const CatalogRadixPage root = readRoot(pages_, superblock);
		std::set<PhysicalPageRef> visitedPages;
		static_cast<void>(visitedPages.insert(rootReference));
		visitNode(pages_, root, superblock, 0, visitedPages, visitor);
	}

} // namespace graph::storage
