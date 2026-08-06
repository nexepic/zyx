/**
 * @file BulkBTreeBuilder.cpp
 * @brief Dense, two-pass B+Tree bulk materialization implementation.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/index/BulkBTreeBuilder.hpp"

#include <algorithm>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "graph/utils/ChecksumUtils.hpp"

namespace graph::storage {

	namespace {

		struct NodePlan {
			uint16_t level = 0;
			std::vector<std::byte> first_key;
			size_t leaf_entry_count = 0;
			std::vector<size_t> children;
		};

		struct InputSummary {
			uint64_t entry_count = 0;
			uint32_t semantic_crc = 0;
		};

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		[[nodiscard]] bool lessKey(std::span<const std::byte> left, std::span<const std::byte> right) {
			return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end());
		}

		void validateOptions(const BulkBTreeBuildOptions &options, uint64_t indexId, LogSequenceNumber checkpointLsn,
							 const OrderedBTreeEntrySource &source) {
			const uint8_t invalid = static_cast<uint8_t>(indexId == 0U) | static_cast<uint8_t>(!source) |
									static_cast<uint8_t>(!options.first_slot.isValid()) |
									static_cast<uint8_t>(!options.last_slot.isValid()) |
									static_cast<uint8_t>(options.first_slot > options.last_slot) |
									static_cast<uint8_t>(options.pool_owner_id == 0U) |
									static_cast<uint8_t>(options.maximum_entries == 0U) |
									static_cast<uint8_t>(options.maximum_pages == 0U) |
									static_cast<uint8_t>(options.leaf_value_layout != BTreeLeafValueLayout::BLVL_U64 &&
														 options.leaf_value_layout != BTreeLeafValueLayout::BLVL_EMPTY) |
									static_cast<uint8_t>(!checkpointLsn.isValid());
			if (invalid != 0U) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable bulk B+Tree build requires bounded input, pool placement, owner, index, and checkpoint LSN");
			}
		}

		[[nodiscard]] PageOwnerHeader pageOwner(uint64_t indexId, PageId pageId, Generation generation,
														LogSequenceNumber checkpointLsn) {
			PageOwnerHeader owner;
			owner.page_id = pageId;
			owner.owner_id = indexId;
			owner.kind = PageOwnerKind::POK_INDEX;
			owner.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
			owner.generation = generation;
			owner.committed_lsn = checkpointLsn;
			return owner;
		}

		[[nodiscard]] BTreePage measuringPage(uint64_t indexId, BTreeNodeKind kind, uint16_t level,
											 BTreeLeafValueLayout leafValueLayout) {
			const PageOwnerHeader owner = pageOwner(indexId, PageId(1), Generation(1), LogSequenceNumber(1));
			BTreePageHeader header;
			header.index_id = indexId;
			header.page_id = owner.page_id;
			header.generation = owner.generation;
			header.kind = kind;
			header.level = level;
			if (kind == BTreeNodeKind::BPN_LEAF) {
				header.leaf_value_layout = leafValueLayout;
			}
			if (kind == BTreeNodeKind::BPN_INTERNAL) {
				header.leftmost_child = PageId(1);
			}
			return BTreePage::create(owner, header);
		}

		[[nodiscard]] bool validLeafValue(const BTreePageEntry &entry, BTreeLeafValueLayout leafValueLayout) {
			return leafValueLayout == BTreeLeafValueLayout::BLVL_U64 ? entry.value_or_child != 0U
																									 : entry.value_or_child == 0U;
		}

		[[nodiscard]] bool tryInsert(BTreePage &page, const BTreePageEntry &entry) {
			try {
				page.insert(entry);
				return true;
			} catch (const FormatException &error) {
				// ZYX_COV_EXCL_LINE: strictly ordered prevalidated input can only make a measurement page full.
				if (error.error() != FormatError::FERR_INVALID_STATE) { // ZYX_COV_EXCL_LINE: validated input only fills a page.
					throw;
				}
				return false;
			}
		}

		void updateSemanticCrc(uint32_t &crc, const BTreePageEntry &entry) {
			const std::vector<std::byte> encoded = BTreePageEntryCodec::encode(entry);
			crc = utils::updateCrc(crc, encoded.data(), encoded.size());
		}

		[[nodiscard]] OrderedBTreeEntryCursor openCursor(const OrderedBTreeEntrySource &source) {
			OrderedBTreeEntryCursor cursor = source();
			if (!cursor) {
				fail(FormatError::FERR_INVALID_STATE, "relocatable bulk B+Tree source did not open a cursor");
			}
			return cursor;
		}

		void appendNode(std::vector<NodePlan> &nodes, NodePlan node, size_t maximumPages) {
			if (nodes.size() >= maximumPages) {
				fail(FormatError::FERR_INVALID_STATE, "relocatable bulk B+Tree build exceeded its configured page bound");
			}
			nodes.push_back(std::move(node));
		}

		[[nodiscard]] InputSummary planLeaves(uint64_t indexId, const OrderedBTreeEntrySource &source,
													const BulkBTreeBuildOptions &options, std::vector<NodePlan> &nodes,
													std::vector<size_t> &leafOrdinals) {
			OrderedBTreeEntryCursor cursor = openCursor(source);
			InputSummary summary;
			BTreePage page = measuringPage(indexId, BTreeNodeKind::BPN_LEAF, 0, options.leaf_value_layout);
			std::vector<std::byte> firstKey;
			std::vector<std::byte> previousKey;
			size_t entriesInPage = 0;
			for (;;) {
				std::optional<BTreePageEntry> next = cursor();
				if (!next.has_value()) {
					break;
				}
				const BTreePageEntry &entry = *next;
				if (entry.key.empty() || !validLeafValue(entry, options.leaf_value_layout) ||
					(!previousKey.empty() && !lessKey(previousKey, entry.key))) {
					fail(FormatError::FERR_INVALID_HEADER,
						 "relocatable bulk B+Tree source has an empty key, invalid leaf value, or noncanonical order");
				}
				if (summary.entry_count == options.maximum_entries) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable bulk B+Tree source exceeded its configured entry bound");
				}
				if (!tryInsert(page, entry)) {
					if (entriesInPage == 0U) {
						fail(FormatError::FERR_INVALID_STATE,
							 "relocatable bulk B+Tree source entry cannot fit in an empty leaf page");
					}
					NodePlan leaf;
					leaf.first_key = std::move(firstKey);
					leaf.leaf_entry_count = entriesInPage;
					appendNode(nodes, std::move(leaf), options.maximum_pages);
					leafOrdinals.push_back(nodes.size() - 1U);
					page = measuringPage(indexId, BTreeNodeKind::BPN_LEAF, 0, options.leaf_value_layout);
					firstKey = entry.key;
					entriesInPage = 0;
					if (!tryInsert(page, entry)) { // ZYX_COV_EXCL_LINE: a fresh leaf has the same header and capacity.
						fail(FormatError::FERR_INVALID_STATE,
							 "relocatable bulk B+Tree source entry cannot fit in a fresh leaf page");
					}
				}
				if (entriesInPage == 0U) {
					firstKey = entry.key;
				}
				previousKey = entry.key;
				++entriesInPage;
				++summary.entry_count;
				updateSemanticCrc(summary.semantic_crc, entry);
			}
			if (entriesInPage == 0U) {
				appendNode(nodes, {}, options.maximum_pages);
				leafOrdinals.push_back(nodes.size() - 1U);
			} else {
				NodePlan leaf;
				leaf.first_key = std::move(firstKey);
				leaf.leaf_entry_count = entriesInPage;
				appendNode(nodes, std::move(leaf), options.maximum_pages);
				leafOrdinals.push_back(nodes.size() - 1U);
			}
			return summary;
		}

		void planParentLevels(uint64_t indexId, const BulkBTreeBuildOptions &options, std::vector<NodePlan> &nodes,
							  std::vector<size_t> levelOrdinals) {
			uint16_t level = 0;
			while (levelOrdinals.size() > 1U) {
				if (level == std::numeric_limits<uint16_t>::max()) { // ZYX_COV_EXCL_LINE: max pages exhausts memory first.
					fail(FormatError::FERR_ARITHMETIC_OVERFLOW, "relocatable bulk B+Tree build exceeded the persisted tree depth");
				}
				++level;
				std::vector<size_t> parentOrdinals;
				NodePlan parent;
				parent.level = level;
				BTreePage page = measuringPage(indexId, BTreeNodeKind::BPN_INTERNAL, level,
															 BTreeLeafValueLayout::BLVL_U64);
				for (const size_t childOrdinal: levelOrdinals) {
					const NodePlan &child = nodes[childOrdinal];
					const std::vector<std::byte> childFirstKey = child.first_key;
					if (childFirstKey.empty()) { // ZYX_COV_EXCL_LINE: an empty root leaf bypasses parent planning.
						fail(FormatError::FERR_INVALID_STATE,
							 "relocatable bulk B+Tree build cannot use an empty leaf as an internal child");
					}
					if (parent.children.empty()) {
						parent.children.push_back(childOrdinal);
						parent.first_key = childFirstKey;
						continue;
					}
					if (!tryInsert(page, {childFirstKey, PageId(1).value()})) {
						appendNode(nodes, std::move(parent), options.maximum_pages);
						parentOrdinals.push_back(nodes.size() - 1U);
						parent = {};
						parent.level = level;
						parent.first_key = childFirstKey;
						parent.children.push_back(childOrdinal);
						page = measuringPage(indexId, BTreeNodeKind::BPN_INTERNAL, level,
																 BTreeLeafValueLayout::BLVL_U64);
						continue;
					}
					parent.children.push_back(childOrdinal);
				}
				appendNode(nodes, std::move(parent), options.maximum_pages);
				parentOrdinals.push_back(nodes.size() - 1U);
				if (parentOrdinals.size() >= levelOrdinals.size()) { // ZYX_COV_EXCL_LINE: a separator fits with its leaf key.
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable bulk B+Tree separators cannot reduce the planned tree height");
				}
				levelOrdinals = std::move(parentOrdinals);
			}
		}

		[[nodiscard]] std::vector<IndexTreeMaterializationPoolOptions>
		poolOptions(const BulkBTreeBuildOptions &options, size_t pageCount) {
			std::vector<IndexTreeMaterializationPoolOptions> pools;
			const size_t poolCount = (pageCount - 1U) / RSS_PAGE_POOL_DATA_FRAME_COUNT + 1U;
			pools.reserve(poolCount);
			for (size_t poolOrdinal = 0; poolOrdinal < poolCount; ++poolOrdinal) {
				const size_t firstPage = poolOrdinal * RSS_PAGE_POOL_DATA_FRAME_COUNT;
				const size_t remaining = pageCount - firstPage;
				pools.push_back({options.first_slot, options.last_slot, options.pool_owner_id,
								 std::min(remaining, static_cast<size_t>(RSS_PAGE_POOL_DATA_FRAME_COUNT))});
			}
			return pools;
		}

		[[nodiscard]] size_t globalPageOrdinal(std::span<const IndexTreeMaterializedPool> pools, size_t poolOrdinal,
														 size_t pageOrdinal) {
			if (poolOrdinal >= pools.size() || pageOrdinal >= pools[poolOrdinal].page_ids.size()) { // ZYX_COV_EXCL_LINE: store assigns coordinates.
				fail(FormatError::FERR_INVALID_STATE,
					 "relocatable bulk B+Tree materialization factory received an invalid Page ID assignment");
			}
			size_t result = pageOrdinal;
			for (size_t index = 0; index < poolOrdinal; ++index) {
				if (pools[index].page_ids.size() > std::numeric_limits<size_t>::max() - result) { // ZYX_COV_EXCL_LINE: pools are capped at 31 pages.
					fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
						 "relocatable bulk B+Tree Page ID assignment overflows host address space");
				}
				result += pools[index].page_ids.size();
			}
			return result;
		}

	} // namespace

	BTreeInitialTreeMutationPlan BulkBTreeBuilder::planNativeTree(
			BTreePageStore &pages, RelocatableMetadataStore &metadata, const MetadataSnapshotPin &snapshot,
			uint64_t indexId, const OrderedBTreeEntrySource &source, const BulkBTreeBuildOptions &options) {
		validateOptions(options, indexId, LogSequenceNumber(1U), source);
		if (!snapshot.isValid() || !pages.usesMetadataStore(metadata)) {
			fail(FormatError::FERR_INVALID_STATE,
					 "relocatable native bulk B+Tree planning requires one live metadata domain");
		}
		if (metadata.findIndexRootOptional(snapshot, indexId).has_value()) {
			fail(FormatError::FERR_INVALID_STATE,
					 "relocatable native bulk B+Tree planning cannot replace an existing Index root");
		}

		std::vector<NodePlan> plannedNodes;
		std::vector<size_t> leafOrdinals;
		const InputSummary summary = planLeaves(indexId, source, options, plannedNodes, leafOrdinals);
		planParentLevels(indexId, options, plannedNodes, leafOrdinals);
		const size_t pageCount = plannedNodes.size();
		if (pageCount == 0U) {
			fail(FormatError::FERR_INVALID_STATE, "relocatable native bulk B+Tree planning produced no pages");
		}
		const size_t rootOrdinal = pageCount - 1U;
		const size_t poolCount = (pageCount - 1U) / RSS_PAGE_POOL_DATA_FRAME_COUNT + 1U;
		BTreeInitialTreeMutationPlan result;
		result.index_id = indexId;
		result.new_pool_mutations.reserve(poolCount);
		for (size_t poolOrdinal = 0U; poolOrdinal < poolCount; ++poolOrdinal) {
			const size_t firstPage = poolOrdinal * RSS_PAGE_POOL_DATA_FRAME_COUNT;
			result.new_pool_mutations.push_back(
					{options.pool_owner_id,
					 std::min(pageCount - firstPage, static_cast<size_t>(RSS_PAGE_POOL_DATA_FRAME_COUNT))});
		}
		result.root_page = {rootOrdinal / RSS_PAGE_POOL_DATA_FRAME_COUNT,
								rootOrdinal % RSS_PAGE_POOL_DATA_FRAME_COUNT};

		auto nodes = std::make_shared<std::vector<NodePlan>>(std::move(plannedNodes));
		const size_t leafCount = leafOrdinals.size();
		result.preparer = [&pages, indexId, source, nodes = std::move(nodes), leafCount, summary,
								leafValueLayout = options.leaf_value_layout,
								pools = result.new_pool_mutations](std::vector<IndexTreeMutationPoolBatch> &batches,
														 LogSequenceNumber checkpointLsn) mutable {
			if (!checkpointLsn.isValid() || batches.size() != pools.size()) {
				fail(FormatError::FERR_INVALID_STATE,
						 "relocatable native bulk B+Tree preparation changed its pool plan or checkpoint");
			}
			std::vector<IndexTreeMaterializedPool> assignments;
			assignments.reserve(batches.size());
			std::vector<PageId> assignedPageIds;
			assignedPageIds.reserve(nodes->size());
			for (size_t ordinal = 0U; ordinal < batches.size(); ++ordinal) {
				IndexTreeMutationPoolBatch &batch = batches[ordinal];
				const BTreeNewIndexTreePoolMutation &expected = pools[ordinal];
				const uint8_t invalid = static_cast<uint8_t>(!batch.newly_reserved) |
										 static_cast<uint8_t>(batch.pool.kind != SegmentKind::SGK_INDEX_PAGE_POOL) |
										 static_cast<uint8_t>(batch.pool.owner_id != expected.owner_id) |
										 static_cast<uint8_t>(batch.pool.generation != Generation(1U)) |
										 static_cast<uint8_t>(batch.pool.state != SegmentState::SGS_SEALED) |
										 static_cast<uint8_t>(batch.pool.committed_lsn != checkpointLsn) |
										 static_cast<uint8_t>(batch.appended_page_ids.size() != expected.appended_page_count) |
										 static_cast<uint8_t>(batch.appended_locations.size() != expected.appended_page_count);
				if (invalid != 0U) {
					fail(FormatError::FERR_INVALID_STATE,
							 "relocatable native bulk B+Tree preparation changed a fresh pool identity");
				}
				assignments.push_back({batch.pool, std::span<const PageId>(batch.appended_page_ids)});
				assignedPageIds.insert(assignedPageIds.end(), batch.appended_page_ids.begin(),
									   batch.appended_page_ids.end());
			}
			if (assignedPageIds.size() != nodes->size()) {
				fail(FormatError::FERR_INVALID_STATE,
						 "relocatable native bulk B+Tree preparation lost a Page ID assignment");
			}
			OrderedBTreeEntryCursor secondCursor = openCursor(source);
			uint64_t secondEntryCount = 0U;
			uint32_t secondSemanticCrc = 0U;
			std::vector<std::byte> secondPreviousKey;
			size_t nextLeafOrdinal = 0U;
			const auto materializePage = [&](size_t poolOrdinal, size_t pageOrdinal) -> BTreePage {
				const size_t ordinal =
						globalPageOrdinal(std::span<const IndexTreeMaterializedPool>(assignments), poolOrdinal, pageOrdinal);
				if (ordinal >= nodes->size()) {
					fail(FormatError::FERR_INVALID_STATE,
							 "relocatable native bulk B+Tree preparation received an invalid page ordinal");
				}
				const IndexTreeMaterializedPool &pool = assignments[poolOrdinal];
				const PageId pageId = assignedPageIds[ordinal];
				const NodePlan &node = (*nodes)[ordinal];
				if (ordinal >= leafCount) {
					BTreePageHeader header;
					header.index_id = indexId;
					header.page_id = pageId;
					header.generation = pool.pool.generation;
					header.kind = BTreeNodeKind::BPN_INTERNAL;
					header.level = node.level;
					if (node.children.empty()) {
						fail(FormatError::FERR_INVALID_STATE,
								 "relocatable native bulk B+Tree internal page has no leftmost child");
					}
					header.leftmost_child = assignedPageIds[node.children.front()];
					BTreePage page = BTreePage::create(
							pageOwner(indexId, pageId, pool.pool.generation, checkpointLsn), header);
					for (size_t child = 1U; child < node.children.size(); ++child) {
						const size_t childOrdinal = node.children[child];
						page.insert({(*nodes)[childOrdinal].first_key, assignedPageIds[childOrdinal].value()});
					}
					return page;
				}
				if (nextLeafOrdinal != ordinal) {
					fail(FormatError::FERR_INVALID_STATE,
							 "relocatable native bulk B+Tree leaves were not materialized in order");
				}
				BTreePageHeader header;
				header.index_id = indexId;
				header.page_id = pageId;
				header.generation = pool.pool.generation;
				header.left_sibling = ordinal == 0U ? PageId{} : assignedPageIds[ordinal - 1U];
				header.right_sibling = ordinal + 1U == leafCount ? PageId{} : assignedPageIds[ordinal + 1U];
				header.leaf_value_layout = leafValueLayout;
				BTreePage page = BTreePage::create(
						pageOwner(indexId, pageId, pool.pool.generation, checkpointLsn), header);
				for (size_t entryOrdinal = 0U; entryOrdinal < node.leaf_entry_count; ++entryOrdinal) {
					const std::optional<BTreePageEntry> next = secondCursor();
					if (!next.has_value() || !validLeafValue(*next, leafValueLayout) ||
						(entryOrdinal == 0U && next->key != node.first_key) ||
						(!secondPreviousKey.empty() && !lessKey(secondPreviousKey, next->key))) {
						fail(FormatError::FERR_INVALID_STATE,
								 "relocatable native bulk B+Tree second pass differs from its planned leaves");
					}
					page.insert(*next);
					secondPreviousKey = next->key;
					++secondEntryCount;
					updateSemanticCrc(secondSemanticCrc, *next);
				}
				++nextLeafOrdinal;
				if (nextLeafOrdinal == leafCount &&
					(secondCursor().has_value() || secondEntryCount != summary.entry_count ||
					 secondSemanticCrc != summary.semantic_crc)) {
					fail(FormatError::FERR_INVALID_STATE,
							 "relocatable native bulk B+Tree second pass differs from its first pass");
				}
				return page;
			};
			for (size_t poolOrdinal = 0U; poolOrdinal < batches.size(); ++poolOrdinal) {
				IndexTreeMutationPoolBatch &batch = batches[poolOrdinal];
				std::vector<BTreePage> materialized;
				materialized.reserve(batch.appended_page_ids.size());
				for (size_t pageOrdinal = 0U; pageOrdinal < batch.appended_page_ids.size(); ++pageOrdinal) {
					materialized.push_back(materializePage(poolOrdinal, pageOrdinal));
				}
				pages.prepareNewPoolInitialPages(batch.pool, materialized, batch.appended_locations, checkpointLsn);
			}
		};
		return result;
	}

	BulkBTreeBuildResult BulkBTreeBuilder::build(BTreePageStore &pages, RelocatableMetadataStore &metadata,
															 uint64_t indexId, const OrderedBTreeEntrySource &source,
															 const BulkBTreeBuildOptions &options,
															 LogSequenceNumber checkpointLsn,
															 const MetadataPublishOptions &publishOptions) {
		validateOptions(options, indexId, checkpointLsn, source);
		if (!pages.usesMetadataStore(metadata)) {
			fail(FormatError::FERR_INVALID_STATE, "relocatable bulk B+Tree build requires one metadata store");
		}

		std::vector<NodePlan> nodes;
		std::vector<size_t> leafOrdinals;
		const InputSummary summary = planLeaves(indexId, source, options, nodes, leafOrdinals);
		const size_t leafCount = leafOrdinals.size();
		planParentLevels(indexId, options, nodes, leafOrdinals);
		const size_t rootOrdinal = nodes.size() - 1U;
		const std::vector<IndexTreeMaterializationPoolOptions> pools = poolOptions(options, nodes.size());
		const IndexTreeMaterializedPageReference rootPage{
				rootOrdinal / RSS_PAGE_POOL_DATA_FRAME_COUNT, rootOrdinal % RSS_PAGE_POOL_DATA_FRAME_COUNT};

		OrderedBTreeEntryCursor secondCursor = openCursor(source);
		auto assignedPageIds = std::make_shared<std::vector<PageId>>();
		uint64_t secondEntryCount = 0;
		uint32_t secondSemanticCrc = 0;
		std::vector<std::byte> secondPreviousKey;
		// Leaf entries need their page boundaries from the first pass. Wrap the
		// page factory so each invocation consumes exactly one planned leaf batch.
		size_t nextLeafOrdinal = 0;
		const BTreePageStore::MultiPoolNewTreePageFactory materializingFactory =
				[indexId, checkpointLsn, &nodes, leafCount, &secondCursor, &secondEntryCount, &secondSemanticCrc,
				 &secondPreviousKey, summary, leafValueLayout = options.leaf_value_layout, assignedPageIds, nextLeafOrdinal](
						std::span<const IndexTreeMaterializedPool> assignments, size_t poolOrdinal,
						size_t pageOrdinal) mutable {
					const size_t ordinal = globalPageOrdinal(assignments, poolOrdinal, pageOrdinal);
					if (assignedPageIds->empty()) {
						assignedPageIds->reserve(nodes.size());
						for (const IndexTreeMaterializedPool &pool: assignments) {
							assignedPageIds->insert(assignedPageIds->end(), pool.page_ids.begin(), pool.page_ids.end());
						}
					}
					if (ordinal >= leafCount) {
						if (ordinal >= nodes.size()) { // ZYX_COV_EXCL_LINE: store uses exactly the planned Page IDs.
							fail(FormatError::FERR_INVALID_STATE,
								 "relocatable bulk B+Tree materialization assigned an unexpected logical page");
						}
						const IndexTreeMaterializedPool &pool = assignments[poolOrdinal];
						const PageId pageId = (*assignedPageIds)[ordinal];
						const NodePlan &node = nodes[ordinal];
						BTreePageHeader header;
						header.index_id = indexId;
						header.page_id = pageId;
						header.generation = pool.pool.generation;
						header.kind = BTreeNodeKind::BPN_INTERNAL;
						header.level = node.level;
						if (node.children.empty()) { // ZYX_COV_EXCL_LINE: parent planning emits one leftmost child.
							fail(FormatError::FERR_INVALID_STATE,
								 "relocatable bulk B+Tree internal plan has no leftmost child");
						}
						header.leftmost_child = (*assignedPageIds)[node.children.front()];
						BTreePage page = BTreePage::create(
								pageOwner(indexId, pageId, pool.pool.generation, checkpointLsn), header);
						for (size_t childIndex = 1; childIndex < node.children.size(); ++childIndex) {
							const size_t childOrdinal = node.children[childIndex];
							page.insert({nodes[childOrdinal].first_key, (*assignedPageIds)[childOrdinal].value()});
						}
						return page;
					}
					if (nextLeafOrdinal != ordinal || assignedPageIds->empty() || assignedPageIds->size() != nodes.size()) { // ZYX_COV_EXCL_LINE: store invokes planned pages sequentially.
						fail(FormatError::FERR_INVALID_STATE,
							 "relocatable bulk B+Tree leaf materialization was not invoked in planned page order");
					}
					const IndexTreeMaterializedPool &pool = assignments[poolOrdinal];
					const PageId pageId = (*assignedPageIds)[ordinal];
					BTreePageHeader header;
					header.index_id = indexId;
					header.page_id = pageId;
					header.generation = pool.pool.generation;
					header.left_sibling = ordinal == 0U ? PageId{} : (*assignedPageIds)[ordinal - 1U];
					header.right_sibling = ordinal + 1U == leafCount ? PageId{} : (*assignedPageIds)[ordinal + 1U];
					header.leaf_value_layout = leafValueLayout;
					BTreePage page = BTreePage::create(pageOwner(indexId, pageId, pool.pool.generation, checkpointLsn), header);
					const size_t expectedEntries = nodes[ordinal].leaf_entry_count;
					for (size_t entryIndex = 0; entryIndex < expectedEntries; ++entryIndex) {
						std::optional<BTreePageEntry> next = secondCursor();
						if (!next.has_value()) {
							fail(FormatError::FERR_INVALID_STATE,
								 "relocatable bulk B+Tree second source pass ended before its planned leaf entries");
						}
						const BTreePageEntry &entry = *next;
						if (entry.key.empty() || !validLeafValue(entry, leafValueLayout) ||
							(!secondPreviousKey.empty() && !lessKey(secondPreviousKey, entry.key))) {
							fail(FormatError::FERR_INVALID_STATE,
								 "relocatable bulk B+Tree second source pass is not the validated ordered entry sequence");
						}
						if (entryIndex == 0U && entry.key != nodes[ordinal].first_key) {
							fail(FormatError::FERR_INVALID_STATE,
								 "relocatable bulk B+Tree second source pass changed one planned leaf boundary");
						}
						page.insert(entry);
						secondPreviousKey = entry.key;
						++secondEntryCount;
						updateSemanticCrc(secondSemanticCrc, entry);
					}
					++nextLeafOrdinal;
					if (nextLeafOrdinal == leafCount) {
						if (secondCursor().has_value() || secondEntryCount != summary.entry_count ||
							secondSemanticCrc != summary.semantic_crc) {
							fail(FormatError::FERR_INVALID_STATE,
								 "relocatable bulk B+Tree second source pass differs from the validated first pass");
						}
					}
					return page;
				};

		const Superblock superblock = pages.materializeAndPublishTreeAcrossPools(
				pools, indexId, rootPage, checkpointLsn, materializingFactory, publishOptions);
		return {superblock, summary.entry_count, nodes.size(), nodes[rootOrdinal].level, summary.semantic_crc};
	}

} // namespace graph::storage
