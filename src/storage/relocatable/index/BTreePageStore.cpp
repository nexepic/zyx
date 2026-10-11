/**
 * @file BTreePageStore.cpp
 * @brief Durable B+Tree page materialization and replacement for relocatable.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/index/BTreePageStore.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <type_traits>

#include "graph/storage/relocatable/index/InitialBTreeBuilder.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"

namespace graph::storage {

	namespace {

		// Transaction-local bootstrap is deliberately bounded to one shared-page
		// pool worth of input. Large DDL construction must use the streaming bulk
		// builder, which does not retain every leaf entry in memory.
		constexpr size_t RSS_INITIAL_TREE_MAX_STAGED_ENTRY_BYTES =
				static_cast<size_t>(RSS_PAGE_POOL_DATA_FRAME_COUNT) * RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;

		[[nodiscard]] PageOwnerHeader indexPageOwner(uint64_t indexId, PageId pageId, Generation generation,
													 LogSequenceNumber committedLsn) {
			PageOwnerHeader owner;
			owner.page_id = pageId;
			owner.owner_id = indexId;
			owner.kind = PageOwnerKind::POK_INDEX;
			owner.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
			owner.generation = generation;
			owner.committed_lsn = committedLsn;
			return owner;
		}

		[[nodiscard]] LogSequenceNumber provisionalPlanCheckpointLsn(const MetadataSnapshotPin &snapshot) {
			const LogSequenceNumber publishedLsn = snapshot.superblock().checkpoint_lsn;
			if (!publishedLsn.isValid() || publishedLsn.value() == std::numeric_limits<uint64_t>::max()) {
				throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW,
									  "relocatable B+Tree structural plan cannot advance the checkpoint LSN");
			}
			return LogSequenceNumber(publishedLsn.value() + 1U);
		}

		[[nodiscard]] BTreePage makeLeafPage(const BTreePageHeader &source, const PageOwnerHeader &owner,
											 PageId leftSibling, PageId rightSibling,
											 std::span<const BTreePageEntry> entries) {
			BTreePageHeader header = source;
			header.page_id = owner.page_id;
			header.generation = owner.generation;
			header.kind = BTreeNodeKind::BPN_LEAF;
			header.level = 0;
			header.left_sibling = leftSibling;
			header.right_sibling = rightSibling;
			header.leftmost_child = {};
			header.entry_count = 0;
			// Every structural preview and final materialization already has the
			// complete ordered entry set. Encode it once instead of repeatedly
			// replacing the packed payload for each entry.
			return BTreePage::createWithEntries(owner, header, entries);
		}

		[[nodiscard]] BTreePage makeInternalPage(const BTreePageHeader &source, const PageOwnerHeader &owner,
												 std::span<const BTreePageEntry> entries) {
			BTreePageHeader header = source;
			header.page_id = owner.page_id;
			header.generation = owner.generation;
			header.entry_count = 0;
			return BTreePage::createWithEntries(owner, header, entries);
		}

		[[nodiscard]] bool lessKey(std::span<const std::byte> left, std::span<const std::byte> right) {
			return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end());
		}

		// A deep mutation plans every COW replacement from one pinned snapshot.
		// `child_ordinal` identifies the next child selected by this node; the
		// structural insert path uses it to place a promoted separator exactly once.
		struct BTreePathNode {
			BTreePageHeader header;
			PageLocation location;
			LogSequenceNumber owner_lsn{};
			std::vector<BTreePageEntry> entries;
			size_t child_ordinal = 0;
		};

		// A structural mutation is planned before the metadata writer is entered.
		// References can name an existing stable PageId or one PageId allocated by
		// the single publication batch; this prevents an intermediate tree shape
		// from becoming visible while promotions travel toward the root.
		struct PlannedPageReference {
			PageId existing_page_id{};
			std::optional<size_t> appended_ordinal;

			[[nodiscard]] static PlannedPageReference existing(PageId pageId) {
				if (!pageId.isValid()) { // ZYX_COV_EXCL_LINE: every stable reference comes from a validated B+Tree
					// header or child entry before the internal mutation plan is created.
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "relocatable B+Tree mutation plan has an invalid stable Page ID");
				}
				return {pageId, std::nullopt};
			}

			[[nodiscard]] static PlannedPageReference appended(size_t ordinal) { return {{}, ordinal}; }

			[[nodiscard]] PageId resolve(std::span<const PageId> appendedPageIds) const {
				if (appended_ordinal.has_value()) {
					if (*appended_ordinal >=
								appendedPageIds.size() || // ZYX_COV_EXCL_LINE: ordinals originate from this
														  // plan's append vector; Metadata allocates every Page ID.
						!appendedPageIds[*appended_ordinal]
								 .isValid()) { // ZYX_COV_EXCL_LINE: Metadata allocated this append ID.
						throw FormatException(FormatError::FERR_INVALID_STATE,
											  "relocatable B+Tree mutation plan received an invalid appended Page ID");
					}
					return appendedPageIds[*appended_ordinal];
				}
				return existing_page_id;
			}

			[[nodiscard]] PageId preview() const { return appended_ordinal.has_value() ? PageId(1) : existing_page_id; }
		};

		struct PlannedInternalEntry {
			std::vector<std::byte> key;
			PlannedPageReference right_child;
		};

		struct PlannedBTreePage {
			BTreePageHeader source_header;
			PlannedPageReference page_ref;
			LogSequenceNumber expected_owner_lsn{};
			PlannedPageReference left_sibling;
			PlannedPageReference right_sibling;
			PlannedPageReference leftmost_child;
			std::vector<BTreePageEntry> leaf_entries;
			std::vector<PlannedInternalEntry> internal_entries;
		};

		[[nodiscard]] std::vector<PlannedInternalEntry> planInternalEntries(std::span<const BTreePageEntry> entries) {
			std::vector<PlannedInternalEntry> planned;
			planned.reserve(entries.size());
			for (const BTreePageEntry &entry: entries) {
				planned.push_back({entry.key, PlannedPageReference::existing(PageId(entry.value_or_child))});
			}
			return planned;
		}

		[[nodiscard]] std::vector<BTreePageEntry>
		previewInternalEntries(std::span<const PlannedInternalEntry> entries) {
			std::vector<BTreePageEntry> preview;
			preview.reserve(entries.size());
			for (const PlannedInternalEntry &entry: entries) {
				preview.push_back({entry.key, entry.right_child.preview().value()});
			}
			return preview;
		}

		[[nodiscard]] BTreePageHeader previewInternalHeader(const BTreePageHeader &source,
															const PlannedPageReference &leftmostChild) {
			BTreePageHeader header = source;
			header.page_id = PageId(1);
			header.leftmost_child = leftmostChild.preview();
			return header;
		}

		[[nodiscard]] BTreePage materializePlannedPage(uint64_t indexId, const CatalogEntry &pool,
													   const PlannedBTreePage &planned,
													   std::span<const PageId> appendedPageIds,
													   LogSequenceNumber checkpointLsn) {
			BTreePageHeader header = planned.source_header;
			header.page_id = planned.page_ref.resolve(appendedPageIds);
			header.generation = pool.generation;
			const LogSequenceNumber ownerLsn =
					planned.page_ref.appended_ordinal.has_value() ? checkpointLsn : planned.expected_owner_lsn;
			const PageOwnerHeader owner = indexPageOwner(indexId, header.page_id, pool.generation, ownerLsn);
			if (header.kind == BTreeNodeKind::BPN_LEAF) {
				return makeLeafPage(header, owner, planned.left_sibling.resolve(appendedPageIds),
									planned.right_sibling.resolve(appendedPageIds), planned.leaf_entries);
			}
			header.leftmost_child = planned.leftmost_child.resolve(appendedPageIds);
			std::vector<BTreePageEntry> entries;
			entries.reserve(planned.internal_entries.size());
			for (const PlannedInternalEntry &entry: planned.internal_entries) {
				entries.push_back({entry.key, entry.right_child.resolve(appendedPageIds).value()});
			}
			return makeInternalPage(header, owner, entries);
		}

		[[nodiscard]] std::optional<size_t> findBalancedLeafSplitIndex(const BTreePageHeader &source, uint64_t indexId,
																	   LogSequenceNumber leftOwnerLsn,
																	   LogSequenceNumber rightOwnerLsn,
																	   PageId leftSibling, PageId rightSibling,
																	   std::span<const BTreePageEntry> entries) {
			std::optional<size_t> selected;
			uint64_t smallestSkew = std::numeric_limits<uint64_t>::max();
			for (size_t candidate = 1; candidate < entries.size(); ++candidate) {
				try {
					BTreePage left =
							makeLeafPage(source, indexPageOwner(indexId, PageId(1), source.generation, leftOwnerLsn),
										 leftSibling, PageId(2), entries.subspan(0, candidate));
					BTreePage right =
							makeLeafPage(source, indexPageOwner(indexId, PageId(2), source.generation, rightOwnerLsn),
										 PageId(1), rightSibling, entries.subspan(candidate));
					const uint64_t leftBytes = left.slottedPage().stats().live_bytes;
					const uint64_t rightBytes = right.slottedPage().stats().live_bytes;
					const uint64_t skew = leftBytes > rightBytes ? leftBytes - rightBytes : rightBytes - leftBytes;
					if (!selected.has_value() || skew < smallestSkew) {
						selected = candidate;
						smallestSkew = skew;
					}
				} catch (const FormatException &error) {
					if (error.error() != FormatError::FERR_INVALID_STATE) { // ZYX_COV_EXCL_LINE: the locally rebuilt
						// leaf can only reject its variable-size capacity, never persisted bytes.
						throw;
					}
				}
			}
			return selected;
		}

		[[nodiscard]] std::optional<size_t>
		findBalancedInternalSplitIndex(const BTreePageHeader &source, uint64_t indexId, LogSequenceNumber leftOwnerLsn,
									   LogSequenceNumber rightOwnerLsn, std::span<const BTreePageEntry> entries) {
			if (entries.size() < 2U) { // ZYX_COV_EXCL_LINE: this helper runs only after inserting one promoted
				// separator into an already-full internal page.
				return std::nullopt;
			}
			std::optional<size_t> selected;
			uint64_t smallestSkew = std::numeric_limits<uint64_t>::max();
			for (size_t promoted = 0; promoted < entries.size(); ++promoted) {
				try {
					BTreePage left = makeInternalPage(
							source, indexPageOwner(indexId, PageId(1), source.generation, leftOwnerLsn),
							entries.subspan(0, promoted));
					BTreePageHeader rightHeader = source;
					rightHeader.page_id = PageId(2);
					rightHeader.left_sibling = {};
					rightHeader.right_sibling = {};
					rightHeader.leftmost_child = PageId(entries[promoted].value_or_child);
					BTreePage right = makeInternalPage(
							rightHeader, indexPageOwner(indexId, PageId(2), source.generation, rightOwnerLsn),
							entries.subspan(promoted + 1U));
					const uint64_t leftBytes = left.slottedPage().stats().live_bytes;
					const uint64_t rightBytes = right.slottedPage().stats().live_bytes;
					const uint64_t skew = leftBytes > rightBytes ? leftBytes - rightBytes : rightBytes - leftBytes;
					if (!selected.has_value() || skew < smallestSkew) {
						selected = promoted;
						smallestSkew = skew;
					}
				} catch (const FormatException &error) {
					if (error.error() != FormatError::FERR_INVALID_STATE) { // ZYX_COV_EXCL_LINE: preview pages are
						// locally constructed from validated entries and can only run out of capacity.
						throw;
					}
				}
			}
			return selected;
		}

		// The fixed slotted-page header, its live slot directory, and every live
		// record are part of the durable page image. Use that encoded footprint
		// instead of record count so variable-length keys cannot defeat the B+Tree
		// deletion occupancy invariant.
		constexpr uint32_t RSS_BTREE_MIN_NODE_OCCUPIED_BYTES = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES / 2U;

		struct BTreeDeleteMutationNode {
			BTreePageHeader header;
			PageLocation location;
			LogSequenceNumber owner_lsn{};
			std::vector<BTreePageEntry> entries;
			bool dirty = false;
			bool retired = false;
		};

		[[nodiscard]] BTreePage materializeDeleteMutationNode(uint64_t indexId, const BTreeDeleteMutationNode &node) {
			const PageOwnerHeader owner =
					indexPageOwner(indexId, node.header.page_id, node.header.generation, node.owner_lsn);
			if (node.header.kind == BTreeNodeKind::BPN_LEAF) {
				return makeLeafPage(node.header, owner, node.header.left_sibling, node.header.right_sibling,
									node.entries);
			}
			return makeInternalPage(node.header, owner, node.entries);
		}

		[[nodiscard]] uint32_t encodedNodeOccupancy(const BTreePage &page) {
			const SlottedPageStats stats = page.slottedPage().stats();
			const uint64_t unusedBytes = static_cast<uint64_t>(stats.contiguous_free_bytes) + stats.reclaimable_bytes;
			if (unusedBytes > RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES) { // ZYX_COV_EXCL_LINE: SlottedPage validates
				// its locally materialized stats before this occupancy calculation.
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable B+Tree page reports an invalid encoded free-space total");
			}
			return static_cast<uint32_t>(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES - unusedBytes);
		}

		[[nodiscard]] bool isUnderfullDeleteNode(uint64_t indexId, const BTreeDeleteMutationNode &node) {
			if (node.entries.empty()) {
				return true;
			}
			return encodedNodeOccupancy(materializeDeleteMutationNode(indexId, node)) <
				   RSS_BTREE_MIN_NODE_OCCUPIED_BYTES;
		}

	} // namespace

	struct OverlayPageReference {
		PageId existing_page_id{};
		size_t virtual_ordinal = 0U;
		bool is_virtual = false;

		[[nodiscard]] static OverlayPageReference existing(PageId pageId) { return {pageId, 0U, false}; }

		[[nodiscard]] static OverlayPageReference virtualPage(size_t ordinal) { return {{}, ordinal, true}; }

		constexpr bool operator==(const OverlayPageReference &) const = default;
	};

	struct OverlayInternalEntry {
		std::vector<std::byte> key;
		OverlayPageReference right_child;
	};

	struct OverlayMutationNode {
		BTreePageHeader header;
		std::optional<OverlayPageReference> left_sibling;
		std::optional<OverlayPageReference> right_sibling;
		std::optional<OverlayPageReference> leftmost_child;
		std::vector<BTreePageEntry> leaf_entries;
		std::vector<OverlayInternalEntry> internal_entries;
		bool dirty = false;
		bool retired = false;
	};

	// A loaded page is durable only after SharedPagePoolAllocator and BTreePage
	// have verified its physical and typed owner identities. Keep that identity
	// out of virtual mutation nodes so a missing fence is not representable.
	struct ExistingOverlayMutationNode {
		OverlayMutationNode node;
		PageLocation location;
		LogSequenceNumber owner_lsn{};
	};

	class TransactionBTreeOverlay final : public std::enable_shared_from_this<TransactionBTreeOverlay> {
	public:
		TransactionBTreeOverlay(const BTreePageStore &store, RelocatableMetadataStore &metadata,
								const MetadataSnapshotPin &snapshot, uint64_t indexId, bool rebuildOnFrameExhaustion,
								bool forceFreshRebuild, BTreeLeafValueLayout leafValueLayout) :
			store_(store), metadata_(metadata), snapshot_(snapshot), index_id_(indexId),
			leaf_value_layout_(leafValueLayout), rebuild_on_frame_exhaustion_(rebuildOnFrameExhaustion),
			force_fresh_rebuild_(forceFreshRebuild),
			root_(loadRoot()) {}

		void apply(const BTreeScalarIndexOperation &operation) {
			if (operation.kind == BTreeScalarIndexOperationKind::BSIOP_UPSERT) {
				if (leaf_value_layout_ == BTreeLeafValueLayout::BLVL_EMPTY && operation.value != 0U) {
					throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "relocatable key-only B+Tree overlay cannot upsert a nonempty value");
				}
				upsert(operation.key, operation.value);
				return;
			}
			if (operation.kind == BTreeScalarIndexOperationKind::BSIOP_ERASE) {
				erase(operation.key);
				return;
			}
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable transaction-local B+Tree overlay received an unknown scalar operation");
		}

		[[nodiscard]] BTreeStructuralMutationPlan buildPlan() {
			struct ExistingReplacement {
				PageLocation location;
				OverlayPageReference reference;
			};
			std::vector<ExistingReplacement> replacements;
			std::vector<IndexTreePageRetirement> retirements;
			for (const auto &[pageId, existing]: existing_nodes_) {
				static_cast<void>(pageId);
				const OverlayMutationNode &node = existing.node;
				if (node.retired) {
					retirements.push_back({existing.location, existing.owner_lsn});
				} else if (node.dirty) {
					replacements.push_back({existing.location, OverlayPageReference::existing(node.header.page_id)});
				}
			}
			std::vector<size_t> liveVirtuals;
			for (size_t ordinal = 0U; ordinal < virtual_nodes_.size(); ++ordinal) {
				if (!virtual_nodes_[ordinal].retired) {
					liveVirtuals.push_back(ordinal);
				}
			}
			const size_t rebuildThreshold = static_cast<size_t>(RSS_PAGE_POOL_DATA_FRAME_COUNT) * 4U;
			bool rebuiltDenseTree = false;
			if (liveVirtuals.size() > rebuildThreshold) {
				rebuiltDenseTree = rebuildDenseTree(true);
				if (rebuiltDenseTree) {
					liveVirtuals.clear();
					for (size_t ordinal = 0U; ordinal < virtual_nodes_.size(); ++ordinal) {
						if (!virtual_nodes_[ordinal].retired) {
							liveVirtuals.push_back(ordinal);
						}
					}
					replacements.clear();
					retirements.clear();
					for (const auto &[pageId, existing]: existing_nodes_) {
						static_cast<void>(pageId);
						const OverlayMutationNode &node = existing.node;
						if (node.retired) {
							retirements.push_back({existing.location, existing.owner_lsn});
						}
					}
				}
			}

			std::map<uint64_t, size_t> requestOrdinals;
			std::vector<IndexTreeMutationPoolRequest> requests;
			const auto ensureRequest = [&](SegmentId segmentId) {
				const auto [iterator, inserted] = requestOrdinals.emplace(segmentId.value(), requests.size());
				if (inserted) {
					requests.push_back({segmentId, {}, 0U, {}});
				}
				return iterator->second;
			};
			for (const ExistingReplacement &replacement: replacements) {
				requests[ensureRequest(replacement.location.segment_id)].expected_replacements.push_back(
						replacement.location);
			}
			for (const IndexTreePageRetirement &retirement: retirements) {
				requests[ensureRequest(retirement.location.segment_id)].expected_retirements.push_back(retirement);
			}
			const auto rebuildIntoFreshPools = [&] {
				static_cast<void>(rebuildDenseTree(false));
				liveVirtuals.clear();
				for (size_t ordinal = 0U; ordinal < virtual_nodes_.size(); ++ordinal) {
					if (!virtual_nodes_[ordinal].retired) {
						liveVirtuals.push_back(ordinal);
					}
				}
				replacements.clear();
				retirements.clear();
				for (const auto &[pageId, existing]: existing_nodes_) {
					static_cast<void>(pageId);
					retirements.push_back({existing.location, existing.owner_lsn});
				}
				requestOrdinals.clear();
				requests.clear();
				for (const IndexTreePageRetirement &retirement: retirements) {
					requests[ensureRequest(retirement.location.segment_id)].expected_retirements.push_back(retirement);
				}
				fresh_rebuild_applied_ = true;
			};
			if (force_fresh_rebuild_ && !fresh_rebuild_applied_ && (!replacements.empty() || !liveVirtuals.empty())) {
				rebuildIntoFreshPools();
			} else if (rebuild_on_frame_exhaustion_ &&
					   std::ranges::any_of(requests, [this](const IndexTreeMutationPoolRequest &request) {
						   const CatalogEntry pool = metadata_.findCatalog(snapshot_, request.segment_id);
						   return !store_.hasReplacementFrameCapacity(pool, request.expected_replacements.size());
					   })) {
				rebuildIntoFreshPools();
			}
			if (requests.empty() && liveVirtuals.empty()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable transaction-local B+Tree overlay produced no durable page mutation");
			}

			struct VirtualPlacement {
				size_t pool_ordinal = 0U;
				size_t appended_ordinal = 0U;
			};
			std::optional<BTreeNewIndexTreePoolMutation> newPool;
			std::vector<BTreeNewIndexTreePoolMutation> additionalNewPools;
			std::vector<VirtualPlacement> virtualPlacements(virtual_nodes_.size());
			if (!liveVirtuals.empty()) {
				std::vector<PageLocation> expectedLocations;
				expectedLocations.reserve(replacements.size());
				for (const ExistingReplacement &replacement: replacements) {
					expectedLocations.push_back(replacement.location);
				}
				size_t firstUnplaced = 0U;
				const auto appendToCurrentPool = [&](size_t candidate) {
					const std::optional<SegmentId> selected =
							store_.selectAppendPool(snapshot_, expectedLocations, candidate);
					if (!selected.has_value()) {
						return false;
					}
					const size_t ordinal = ensureRequest(*selected);
					requests[ordinal].appended_page_count = candidate;
					for (size_t index = 0U; index < candidate; ++index) {
						virtualPlacements[liveVirtuals[index]] = {ordinal, index};
					}
					firstUnplaced = candidate;
					return true;
				};
				if (!fresh_rebuild_applied_ && rebuiltDenseTree) {
					const size_t maximumCurrentPoolAppend =
							std::min(liveVirtuals.size(), static_cast<size_t>(RSS_PAGE_POOL_DATA_FRAME_COUNT));
					for (size_t candidate = maximumCurrentPoolAppend; candidate != 0U; --candidate) {
						if (appendToCurrentPool(candidate)) {
							break;
						}
					}
				} else if (!fresh_rebuild_applied_ &&
						   liveVirtuals.size() <= static_cast<size_t>(RSS_PAGE_POOL_DATA_FRAME_COUNT)) {
					// A single structural publication can safely use any current typed
					// Index pool selected under the same metadata snapshot.
					static_cast<void>(appendToCurrentPool(liveVirtuals.size()));
				}
				for (size_t first = firstUnplaced; first < liveVirtuals.size();) {
					const size_t count =
							std::min(static_cast<size_t>(RSS_PAGE_POOL_DATA_FRAME_COUNT), liveVirtuals.size() - first);
					const size_t freshOrdinal = newPool.has_value() ? additionalNewPools.size() + 1U : 0U;
					if (!newPool.has_value()) {
						newPool = {index_id_, count};
					} else {
						additionalNewPools.push_back({index_id_, count});
					}
					for (size_t offset = 0U; offset < count; ++offset) {
						virtualPlacements[liveVirtuals[first + offset]] = {requests.size() + freshOrdinal, offset};
					}
					first += count;
				}
			}
			// selectAppendPool() accounts for every stable replacement and appended
			// virtual page under this pinned metadata snapshot. It returns no pool
			// unless that complete batch fits, in which case the loop above reserves
			// a fresh pool. A second capacity check here cannot observe a new state.

			std::optional<IndexTreeMutationAppendedPageReference> replacementRoot;
			std::optional<PageId> stableReplacementRoot;
			if (root_.is_virtual) {
				const auto rootVirtual = std::ranges::find(liveVirtuals, root_.virtual_ordinal);
				if (rootVirtual == liveVirtuals.end()) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable transaction-local B+Tree overlay root was retired before publication");
				}
				replacementRoot = {virtualPlacements[root_.virtual_ordinal].pool_ordinal,
								   virtualPlacements[root_.virtual_ordinal].appended_ordinal};
			} else {
				stableReplacementRoot = root_.existing_page_id;
			}

			const OverlayMutationNode &finalRoot = nodeFor(root_);
			const bool finalTreeEmpty = finalRoot.header.kind == BTreeNodeKind::BPN_LEAF &&
									finalRoot.leaf_entries.empty();
			const IndexRootLocation expectedRoot = expected_root_;
			const BTreeStructuralMutationPreparer preparer = [self = shared_from_this(),
															  replacements = std::move(replacements),
															  retirements = std::move(retirements),
															  liveVirtuals = std::move(liveVirtuals), requests,
															  virtualPlacements = std::move(virtualPlacements),
															  freshPoolCount =
																	  static_cast<size_t>(newPool.has_value()) +
																	  additionalNewPools.size()](
																	 std::vector<IndexTreeMutationPoolBatch> &batches,
																	 LogSequenceNumber checkpointLsn) {
				const size_t expectedBatchCount = requests.size() + freshPoolCount;
				if (batches.size() != expectedBatchCount) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable transaction-local B+Tree overlay received an invalid pool batch shape");
				}
				std::vector<PageId> virtualIdsByOrdinal(self->virtual_nodes_.size());
				for (const size_t virtualOrdinal: liveVirtuals) {
					const VirtualPlacement placement = virtualPlacements[virtualOrdinal];
					const IndexTreeMutationPoolBatch &batch = batches.at(placement.pool_ordinal);
					if (placement.appended_ordinal >= batch.appended_page_ids.size()) {
						throw FormatException(FormatError::FERR_INVALID_STATE,
											  "relocatable transaction-local B+Tree overlay received an invalid "
											  "virtual Page-ID placement");
					}
					virtualIdsByOrdinal[virtualOrdinal] = batch.appended_page_ids[placement.appended_ordinal];
				}
				for (size_t poolOrdinal = 0U; poolOrdinal < batches.size(); ++poolOrdinal) {
					IndexTreeMutationPoolBatch &batch = batches[poolOrdinal];
					const size_t expectedAppends = poolOrdinal < requests.size()
														   ? requests[poolOrdinal].appended_page_count
														   : batches[poolOrdinal].appended_page_ids.size();
					if (poolOrdinal < requests.size() &&
						(batch.pool.segment_id != requests[poolOrdinal].segment_id ||
						 batch.expected_replacements != requests[poolOrdinal].expected_replacements ||
						 batch.expected_retirements != requests[poolOrdinal].expected_retirements)) {
						throw FormatException(FormatError::FERR_INVALID_STATE,
											  "relocatable transaction-local B+Tree overlay metadata fences changed "
											  "before preparation");
					}
					if (batch.appended_page_ids.size() != expectedAppends) {
						throw FormatException(
								FormatError::FERR_INVALID_STATE,
								"relocatable transaction-local B+Tree overlay append count changed before preparation");
					}
					std::vector<SharedPagePoolPreparedReplacement> preparedReplacements;
					preparedReplacements.reserve(batch.expected_replacements.size());
					for (const PageLocation &location: batch.expected_replacements) {
						ExistingOverlayMutationNode &existing = self->existingFor(location.page_id);
						OverlayMutationNode &node = existing.node;
						// The batch equality fence above and ExistingOverlayMutationNode's
						// construction from the pinned read prove this durable source.
						BTreePage page = self->materialize(node, location.page_id, batch.pool.generation,
														   existing.owner_lsn, virtualIdsByOrdinal);
						std::vector<std::byte> bytes = page.serialize();
						PageOwnerHeader owner =
								indexPageOwner(self->index_id_, location.page_id, batch.pool.generation, checkpointLsn);
						owner.payload_bytes = static_cast<uint32_t>(bytes.size());
						preparedReplacements.push_back({std::move(owner), existing.owner_lsn, std::move(bytes),
														page.slottedPage().stats().contiguous_free_bytes});
					}
					std::vector<SharedPagePoolPreparedAppend> preparedAppends;
					std::vector<size_t> batchVirtuals;
					for (const size_t virtualOrdinal: liveVirtuals) {
						if (virtualPlacements[virtualOrdinal].pool_ordinal == poolOrdinal) {
							batchVirtuals.push_back(virtualOrdinal);
						}
					}
					if (!batchVirtuals.empty()) {
						preparedAppends.reserve(batchVirtuals.size());
						for (const size_t virtualOrdinal: batchVirtuals) {
							const VirtualPlacement placement = virtualPlacements[virtualOrdinal];
							OverlayMutationNode &node =
									self->nodeFor(OverlayPageReference::virtualPage(virtualOrdinal));
							const PageId pageId = batch.appended_page_ids[placement.appended_ordinal];
							BTreePage page = self->materialize(node, pageId, batch.pool.generation, checkpointLsn,
															   virtualIdsByOrdinal);
							std::vector<std::byte> bytes = page.serialize();
							PageOwnerHeader owner =
									indexPageOwner(self->index_id_, pageId, batch.pool.generation, checkpointLsn);
							owner.payload_bytes = static_cast<uint32_t>(bytes.size());
							preparedAppends.push_back({std::move(owner), std::move(bytes),
													   page.slottedPage().stats().contiguous_free_bytes});
						}
					}
					std::vector<SharedPagePoolPreparedRetirement> preparedRetirements;
					preparedRetirements.reserve(batch.expected_retirements.size());
					for (const IndexTreePageRetirement &retirement: batch.expected_retirements) {
						preparedRetirements.push_back({retirement.location, self->index_id_, PageOwnerKind::POK_INDEX,
													   retirement.expected_owner_lsn});
					}
					if (batch.newly_reserved) {
						self->pages().prepareNewPoolPages(batch.pool, preparedAppends, batch.appended_locations,
														  checkpointLsn, batch.prepared_new_pool_prefix_count);
					} else {
						self->pages().prepareMutationPages(batch.pool, batch.expected_replacements,
														   preparedReplacements, preparedAppends,
														   batch.replacement_locations, batch.appended_locations,
														   checkpointLsn, preparedRetirements);
					}
				}
			};
			return {expectedRoot,		   std::move(requests), newPool, std::move(additionalNewPools), replacementRoot,
					stableReplacementRoot, std::move(preparer), finalTreeEmpty};
		}

	private:
		[[nodiscard]] bool rebuildDenseTree(bool requireReduction) {
			std::vector<BTreePageEntry> entries;
			std::set<uint64_t> visitedExisting;
			std::set<size_t> visitedVirtual;
			collectEntries(root_, nodeFor(root_).header.level, entries, visitedExisting, visitedVirtual);
			if (entries.empty()) {
				// An empty root leaf is a valid scalar-index state. When a source pool
				// cannot COW the final erase, retire the old reachable tree and publish
				// one fresh empty root instead of rejecting the logical deletion.
				for (OverlayMutationNode &node: virtual_nodes_) {
					node.retired = true;
					node.dirty = false;
				}
				for (const uint64_t pageId: visitedExisting) {
					OverlayMutationNode &node = existing_nodes_.at(pageId).node;
					node.retired = true;
					node.dirty = false;
				}
				BTreePageHeader emptyRootHeader;
				emptyRootHeader.index_id = index_id_;
				emptyRootHeader.generation = expected_root_.root_generation;
				emptyRootHeader.kind = BTreeNodeKind::BPN_LEAF;
				emptyRootHeader.level = 0U;
				emptyRootHeader.leaf_value_layout = leaf_value_layout_;
				root_ = createVirtual(emptyRootHeader);
				nodeFor(root_).dirty = true;
				return true;
			}
			const InitialBTreePlan plan = InitialBTreeBuilder::plan(index_id_, entries, leaf_value_layout_);
			const size_t liveVirtualCount = static_cast<size_t>(std::ranges::count_if(
					virtual_nodes_, [](const OverlayMutationNode &node) { return !node.retired; }));
			if (requireReduction && plan.nodes.size() >= liveVirtualCount) {
				return false;
			}

			for (OverlayMutationNode &node: virtual_nodes_) {
				node.retired = true;
				node.dirty = false;
			}
			for (const uint64_t pageId: visitedExisting) {
				OverlayMutationNode &node = existing_nodes_.at(pageId).node;
				node.retired = true;
				node.dirty = false;
			}

			std::vector<OverlayPageReference> rebuilt;
			rebuilt.reserve(plan.nodes.size());
			for (const InitialBTreePlanNode &planned: plan.nodes) {
				BTreePageHeader header;
				header.index_id = index_id_;
				header.generation = expected_root_.root_generation;
				header.kind = planned.level == 0U ? BTreeNodeKind::BPN_LEAF : BTreeNodeKind::BPN_INTERNAL;
				header.level = planned.level;
				if (planned.level == 0U) {
					header.leaf_value_layout = leaf_value_layout_;
				}
				rebuilt.push_back(createVirtual(header));
			}
			for (size_t ordinal = 0U; ordinal < plan.nodes.size(); ++ordinal) {
				const InitialBTreePlanNode &planned = plan.nodes[ordinal];
				OverlayMutationNode &node = nodeFor(rebuilt[ordinal]);
				node.dirty = true;
				if (planned.level == 0U) {
					node.leaf_entries = planned.leaf_entries;
					node.left_sibling = ordinal == 0U ? std::nullopt : std::optional(rebuilt[ordinal - 1U]);
					node.right_sibling =
							ordinal + 1U == plan.leaf_count ? std::nullopt : std::optional(rebuilt[ordinal + 1U]);
					continue;
				}
				node.leftmost_child = rebuilt[planned.children.front()];
				for (size_t child = 1U; child < planned.children.size(); ++child) {
					const size_t childOrdinal = planned.children[child];
					node.internal_entries.push_back({plan.nodes[childOrdinal].first_key, rebuilt[childOrdinal]});
				}
			}
			root_ = rebuilt[plan.root_ordinal];
			return true;
		}

		void collectEntries(const OverlayPageReference &reference, uint16_t expectedLevel,
							std::vector<BTreePageEntry> &entries, std::set<uint64_t> &visitedExisting,
							std::set<size_t> &visitedVirtual) {
			const bool firstVisit = reference.is_virtual
											? visitedVirtual.insert(reference.virtual_ordinal).second
											: visitedExisting.insert(reference.existing_page_id.value()).second;
			if (!firstVisit) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable transaction-local B+Tree rebuild found a cycle or shared child page");
			}
			OverlayMutationNode &node = loadNode(reference, expectedLevel);
			if (node.header.kind == BTreeNodeKind::BPN_LEAF) {
				if (expectedLevel != 0U || (node.leaf_entries.empty() && reference != root_)) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "relocatable transaction-local B+Tree rebuild found an empty non-root leaf");
				}
				if (node.leaf_entries.empty()) {
					return;
				}
				for (const BTreePageEntry &entry: node.leaf_entries) {
					const bool invalidValue = leaf_value_layout_ == BTreeLeafValueLayout::BLVL_U64
											  ? entry.value_or_child == 0U
											  : entry.value_or_child != 0U;
					if (entry.key.empty() || invalidValue ||
						(!entries.empty() && !lessKey(entries.back().key, entry.key))) {
						throw FormatException(
								FormatError::FERR_INVALID_STATE,
								"relocatable transaction-local B+Tree rebuild found unordered leaf entries");
					}
					entries.push_back(entry);
				}
				return;
			}
			if (expectedLevel == 0U || node.header.kind != BTreeNodeKind::BPN_INTERNAL) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable transaction-local B+Tree rebuild found an invalid internal level");
			}
			for (size_t child = 0U; child <= node.internal_entries.size(); ++child) {
				collectEntries(childAt(node, child), static_cast<uint16_t>(expectedLevel - 1U), entries,
							   visitedExisting, visitedVirtual);
			}
		}

		[[nodiscard]] OverlayPageReference loadRoot() {
			expected_root_ = metadata_.findIndexRoot(snapshot_, index_id_);
			const OverlayPageReference root = OverlayPageReference::existing(expected_root_.root_page_id);
			OverlayMutationNode &node =
					loadExisting(root.existing_page_id, expected_root_.root_generation.value(), readLevel(root));
			if (node.header.left_sibling.isValid() || node.header.right_sibling.isValid()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable transaction-local B+Tree overlay root is stale or malformed");
			}
			return root;
		}

		[[nodiscard]] uint16_t readLevel(OverlayPageReference root) const {
			const BTreePage page = store_.read(snapshot_, root.existing_page_id);
			return page.header().level;
		}

		[[nodiscard]] OverlayMutationNode &loadExisting(PageId pageId, uint64_t expectedGeneration,
														uint16_t expectedLevel) {
			const auto existing = existing_nodes_.find(pageId.value());
			if (existing != existing_nodes_.end()) {
				if (existing->second.node.header.level != expectedLevel ||
					existing->second.node.header.generation.value() != expectedGeneration) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable transaction-local B+Tree overlay revisited a page with a different identity");
				}
				return existing->second.node;
			}
			BTreePage page = store_.read(snapshot_, pageId);
			const BTreePageHeader header = page.header();
			const PageLocation location = metadata_.findPageLocation(snapshot_, pageId);
			const LogSequenceNumber ownerLsn = page.slottedPage().ownerCommittedLsn();
			const uint8_t invalid = static_cast<uint8_t>(header.index_id != index_id_) |
									static_cast<uint8_t>(header.page_id != pageId) |
									static_cast<uint8_t>(header.generation.value() != expectedGeneration) |
									static_cast<uint8_t>(header.level != expectedLevel) |
									static_cast<uint8_t>(location.page_id != pageId) |
									static_cast<uint8_t>(location.generation != header.generation) |
									static_cast<uint8_t>(!ownerLsn.isValid());
			if (invalid != 0U) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable transaction-local B+Tree overlay encountered a foreign or malformed page");
			}
			if (header.kind == BTreeNodeKind::BPN_LEAF && header.leaf_value_layout != leaf_value_layout_) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable transaction-local B+Tree overlay found a mismatched leaf layout");
			}
			OverlayMutationNode node;
			node.header = header;
			if (header.kind == BTreeNodeKind::BPN_LEAF) {
				if (header.left_sibling.isValid()) {
					node.left_sibling = OverlayPageReference::existing(header.left_sibling);
				}
				if (header.right_sibling.isValid()) {
					node.right_sibling = OverlayPageReference::existing(header.right_sibling);
				}
				node.leaf_entries = page.entries();
			} else {
				node.leftmost_child = OverlayPageReference::existing(header.leftmost_child);
				for (const BTreePageEntry &entry: page.entries()) {
					node.internal_entries.push_back(
							{entry.key, OverlayPageReference::existing(PageId(entry.value_or_child))});
				}
			}
			return existing_nodes_
					.emplace(pageId.value(), ExistingOverlayMutationNode{std::move(node), location, ownerLsn})
					.first->second.node;
		}

		[[nodiscard]] ExistingOverlayMutationNode &existingFor(PageId pageId) {
			return existing_nodes_.at(pageId.value());
		}

		[[nodiscard]] OverlayMutationNode &nodeFor(const OverlayPageReference &reference) {
			if (reference.is_virtual) {
				return virtual_nodes_.at(reference.virtual_ordinal);
			}
			return existingFor(reference.existing_page_id).node;
		}

		[[nodiscard]] OverlayMutationNode &loadNode(const OverlayPageReference &reference, uint16_t expectedLevel) {
			if (reference.is_virtual) {
				return nodeFor(reference);
			}
			return loadExisting(reference.existing_page_id, expected_root_.root_generation.value(), expectedLevel);
		}

		[[nodiscard]] OverlayPageReference createVirtual(BTreePageHeader header) {
			header.page_id = PageId(1U);
			header.generation = expected_root_.root_generation;
			header.entry_count = 0U;
			OverlayMutationNode node;
			node.header = std::move(header);
			virtual_nodes_.push_back(std::move(node));
			return OverlayPageReference::virtualPage(virtual_nodes_.size() - 1U);
		}

		[[nodiscard]] static OverlayPageReference childAt(const OverlayMutationNode &parent, size_t ordinal) {
			return ordinal == 0U ? parent.leftmost_child.value() : parent.internal_entries.at(ordinal - 1U).right_child;
		}

		[[nodiscard]] static size_t childOrdinal(const OverlayMutationNode &parent, const OverlayPageReference &child) {
			for (size_t ordinal = 0U; ordinal <= parent.internal_entries.size(); ++ordinal) {
				if (childAt(parent, ordinal) == child) {
					return ordinal;
				}
			}
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable transaction-local B+Tree overlay parent lost its selected child");
		}

		[[nodiscard]] static PageId preview(const OverlayPageReference &reference) {
			return reference.is_virtual ? PageId(1U) : reference.existing_page_id;
		}

		[[nodiscard]] static PageId resolve(const OverlayPageReference &reference,
											const std::vector<PageId> &virtualIds) {
			if (!reference.is_virtual) {
				return reference.existing_page_id;
			}
			const PageId pageId = virtualIds.at(reference.virtual_ordinal);
			if (!pageId.isValid()) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable transaction-local B+Tree overlay could not resolve a virtual Page ID");
			}
			return pageId;
		}

		[[nodiscard]] BTreePage materialize(const OverlayMutationNode &node, PageId pageId, Generation generation,
											LogSequenceNumber ownerLsn, const std::vector<PageId> &virtualIds) const {
			const PageOwnerHeader owner = indexPageOwner(index_id_, pageId, generation, ownerLsn);
			if (node.header.kind == BTreeNodeKind::BPN_LEAF) {
				const PageId left = node.left_sibling.has_value() ? resolve(*node.left_sibling, virtualIds) : PageId{};
				const PageId right =
						node.right_sibling.has_value() ? resolve(*node.right_sibling, virtualIds) : PageId{};
				return makeLeafPage(node.header, owner, left, right, node.leaf_entries);
			}
			BTreePageHeader header = node.header;
			header.leftmost_child = resolve(*node.leftmost_child, virtualIds);
			std::vector<BTreePageEntry> entries;
			entries.reserve(node.internal_entries.size());
			for (const OverlayInternalEntry &entry: node.internal_entries) {
				entries.push_back({entry.key, resolve(entry.right_child, virtualIds).value()});
			}
			return makeInternalPage(header, owner, entries);
		}

		[[nodiscard]] BTreePage materializePreview(const OverlayMutationNode &node) const {
			std::vector<PageId> virtualIds(virtual_nodes_.size(), PageId(1U));
			return materialize(node, node.header.page_id, node.header.generation, LogSequenceNumber(1U), virtualIds);
		}

		[[nodiscard]] bool underfull(const OverlayMutationNode &node) const {
			const bool empty = node.header.kind == BTreeNodeKind::BPN_LEAF ? node.leaf_entries.empty()
																		   : node.internal_entries.empty();
			return empty || encodedNodeOccupancy(materializePreview(node)) < RSS_BTREE_MIN_NODE_OCCUPIED_BYTES;
		}

		[[nodiscard]] bool fits(const OverlayMutationNode &node) const {
			try {
				static_cast<void>(materializePreview(node));
				return true;
			} catch (const FormatException &) {
				return false;
			}
		}

		[[nodiscard]] std::vector<OverlayPageReference> pathFor(std::span<const std::byte> key) {
			std::vector<OverlayPageReference> path;
			OverlayPageReference current = root_;
			uint16_t expectedLevel = nodeFor(root_).header.level;
			for (;;) {
				OverlayMutationNode &node = loadNode(current, expectedLevel);
				path.push_back(current);
				if (node.header.kind == BTreeNodeKind::BPN_LEAF) {
					return path;
				}
				OverlayPageReference next = childAt(node, 0U);
				for (size_t ordinal = 0U; ordinal < node.internal_entries.size(); ++ordinal) {
					if (lessKey(key, node.internal_entries[ordinal].key)) {
						break;
					}
					next = node.internal_entries[ordinal].right_child;
				}
				current = next;
				--expectedLevel;
			}
		}

		void upsert(std::span<const std::byte> key, uint64_t value) {
			std::vector<OverlayPageReference> path = pathFor(key);
			OverlayMutationNode &leaf = nodeFor(path.back());
			bool replaced = false;
			for (BTreePageEntry &entry: leaf.leaf_entries) {
				if (std::ranges::equal(entry.key, key)) {
					entry.value_or_child = value;
					replaced = true;
					break;
				}
			}
			if (!replaced) {
				leaf.leaf_entries.push_back({std::vector<std::byte>(key.begin(), key.end()), value});
				std::ranges::sort(leaf.leaf_entries, {}, &BTreePageEntry::key);
			}
			leaf.dirty = true;
			if (fits(leaf)) {
				return;
			}
			splitLeaf(path);
		}

		void splitLeaf(std::vector<OverlayPageReference> &path) {
			const OverlayPageReference leftRef = path.back();
			OverlayMutationNode &left = nodeFor(leftRef);
			std::vector<BTreePageEntry> entries = left.leaf_entries;
			const std::optional<size_t> split = findBalancedLeafSplitIndex(
					left.header, index_id_, LogSequenceNumber(1U), LogSequenceNumber(1U),
					left.left_sibling.has_value() ? preview(*left.left_sibling) : PageId{},
					left.right_sibling.has_value() ? preview(*left.right_sibling) : PageId{}, entries);
			if (!split.has_value()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable transaction-local B+Tree overlay cannot split an oversized leaf");
			}
			BTreePageHeader rightHeader = left.header;
			rightHeader.page_id = PageId(1U);
			OverlayPageReference rightRef = createVirtual(rightHeader);
			OverlayMutationNode &leftAfter = nodeFor(leftRef);
			OverlayMutationNode &right = nodeFor(rightRef);
			right.leaf_entries.assign(entries.begin() + static_cast<ptrdiff_t>(*split), entries.end());
			right.left_sibling = leftRef;
			right.right_sibling = leftAfter.right_sibling;
			leftAfter.leaf_entries.assign(entries.begin(), entries.begin() + static_cast<ptrdiff_t>(*split));
			leftAfter.right_sibling = rightRef;
			leftAfter.dirty = true;
			if (right.right_sibling.has_value()) {
				OverlayMutationNode &next = loadNode(*right.right_sibling, 0U);
				if (!next.left_sibling.has_value() || *next.left_sibling != leftRef) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable transaction-local B+Tree overlay found a broken leaf successor link");
				}
				next.left_sibling = rightRef;
				next.dirty = true;
			}
			insertPromoted(path, path.size() - 1U, right.leaf_entries.front().key, rightRef);
		}

		void insertPromoted(std::vector<OverlayPageReference> &path, size_t childPathIndex,
							std::vector<std::byte> separator, OverlayPageReference rightChild) {
			if (childPathIndex == 0U) {
				OverlayMutationNode &oldRoot = nodeFor(path.front());
				BTreePageHeader rootHeader;
				rootHeader.index_id = index_id_;
				rootHeader.page_id = PageId(1U);
				rootHeader.generation = expected_root_.root_generation;
				rootHeader.kind = BTreeNodeKind::BPN_INTERNAL;
				rootHeader.level = static_cast<uint16_t>(oldRoot.header.level + 1U);
				rootHeader.leftmost_child = PageId(1U);
				const OverlayPageReference newRoot = createVirtual(rootHeader);
				OverlayMutationNode &root = nodeFor(newRoot);
				root.leftmost_child = path.front();
				root.internal_entries.push_back({std::move(separator), rightChild});
				root_ = newRoot;
				return;
			}
			const OverlayPageReference parentRef = path[childPathIndex - 1U];
			OverlayMutationNode &parent = nodeFor(parentRef);
			const size_t ordinal = childOrdinal(parent, path[childPathIndex]);
			parent.internal_entries.insert(parent.internal_entries.begin() + static_cast<ptrdiff_t>(ordinal),
										   {std::move(separator), rightChild});
			parent.dirty = true;
			if (fits(parent)) {
				return;
			}
			splitInternal(path, childPathIndex - 1U);
		}

		void splitInternal(std::vector<OverlayPageReference> &path, size_t pathIndex) {
			const OverlayPageReference leftRef = path[pathIndex];
			OverlayMutationNode &left = nodeFor(leftRef);
			std::vector<OverlayInternalEntry> combined = left.internal_entries;
			std::optional<size_t> selected;
			uint64_t smallestSkew = std::numeric_limits<uint64_t>::max();
			// Variable-length keys can make an endpoint separator the only split
			// that fits. This matches the durable deep-split planner and preserves
			// the child reference carried by a unary internal page.
			for (size_t promoted = 0U; promoted < combined.size(); ++promoted) {
				OverlayMutationNode leftCandidate = left;
				leftCandidate.internal_entries.assign(combined.begin(),
													  combined.begin() + static_cast<ptrdiff_t>(promoted));
				OverlayMutationNode rightCandidate = left;
				rightCandidate.internal_entries.assign(combined.begin() + static_cast<ptrdiff_t>(promoted + 1U),
													   combined.end());
				rightCandidate.leftmost_child = combined[promoted].right_child;
				try {
					const uint32_t leftBytes = encodedNodeOccupancy(materializePreview(leftCandidate));
					const uint32_t rightBytes = encodedNodeOccupancy(materializePreview(rightCandidate));
					const uint64_t skew = leftBytes > rightBytes ? leftBytes - rightBytes : rightBytes - leftBytes;
					if (!selected.has_value() || skew < smallestSkew) {
						selected = promoted;
						smallestSkew = skew;
					}
				} catch (const FormatException &) {
				}
			}
			if (!selected.has_value()) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable transaction-local B+Tree overlay cannot split an oversized internal page");
			}
			const OverlayInternalEntry promoted = combined[*selected];
			BTreePageHeader rightHeader = left.header;
			rightHeader.page_id = PageId(1U);
			OverlayPageReference rightRef = createVirtual(rightHeader);
			OverlayMutationNode &leftAfter = nodeFor(leftRef);
			OverlayMutationNode &right = nodeFor(rightRef);
			right.leftmost_child = promoted.right_child;
			right.internal_entries.assign(combined.begin() + static_cast<ptrdiff_t>(*selected + 1U), combined.end());
			leftAfter.internal_entries.assign(combined.begin(), combined.begin() + static_cast<ptrdiff_t>(*selected));
			leftAfter.dirty = true;
			insertPromoted(path, pathIndex, promoted.key, rightRef);
		}

		void erase(std::span<const std::byte> key) {
			std::vector<OverlayPageReference> path = pathFor(key);
			OverlayMutationNode &leaf = nodeFor(path.back());
			const auto erased = std::ranges::find_if(leaf.leaf_entries, [&key](const BTreePageEntry &entry) {
				return std::ranges::equal(entry.key, key);
			});
			if (erased == leaf.leaf_entries.end()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable transaction-local B+Tree overlay cannot erase an absent scalar key");
			}
			const bool erasedFirst = erased == leaf.leaf_entries.begin();
			leaf.leaf_entries.erase(erased);
			leaf.dirty = true;
			if (erasedFirst && !leaf.leaf_entries.empty() && path.size() > 1U) {
				OverlayMutationNode &parent = nodeFor(path[path.size() - 2U]);
				const size_t ordinal = childOrdinal(parent, path.back());
				if (ordinal != 0U) {
					parent.internal_entries[ordinal - 1U].key = leaf.leaf_entries.front().key;
					parent.dirty = true;
				}
			}
			rebalanceAfterErase(path);
		}

		void retire(const OverlayPageReference &reference) {
			OverlayMutationNode &node = nodeFor(reference);
			node.retired = true;
			node.dirty = false;
		}

		void rebalanceAfterErase(std::vector<OverlayPageReference> &path) {
			size_t pathIndex = path.size() - 1U;
			for (;;) {
				const OverlayPageReference currentRef = path[pathIndex];
				OverlayMutationNode &current = nodeFor(currentRef);
				if (pathIndex == 0U) {
					if (current.header.kind == BTreeNodeKind::BPN_INTERNAL && current.internal_entries.empty()) {
						const OverlayPageReference child = *current.leftmost_child;
						retire(currentRef);
						root_ = child;
						collapseUnaryRoot();
					}
					return;
				}
				if (!underfull(current)) {
					return;
				}
				OverlayMutationNode &parent = nodeFor(path[pathIndex - 1U]);
				if (parent.internal_entries.empty()) {
					--pathIndex;
					continue;
				}
				const size_t currentOrdinal = childOrdinal(parent, currentRef);
				const size_t leftOrdinal =
						currentOrdinal < parent.internal_entries.size() ? currentOrdinal : currentOrdinal - 1U;
				const OverlayPageReference leftRef = childAt(parent, leftOrdinal);
				const OverlayPageReference rightRef = childAt(parent, leftOrdinal + 1U);
				OverlayMutationNode &left = loadNode(leftRef, current.header.level);
				OverlayMutationNode &right = loadNode(rightRef, current.header.level);
				if (current.header.kind == BTreeNodeKind::BPN_LEAF) {
					if (!rebalanceLeaf(parent, leftOrdinal, leftRef, rightRef, left, right)) {
						// Variable-length keys can leave one nonempty leaf below the
						// byte occupancy target while neither a legal redistribution nor
						// a one-page merge fits. Keeping it is the only valid layout.
						return;
					}
				} else {
					rebalanceInternal(parent, leftOrdinal, leftRef, rightRef, left, right, pathIndex);
				}
				if (!nodeFor(path[pathIndex]).retired && !underfull(nodeFor(path[pathIndex]))) {
					// A merge can leave the surviving child healthy while removing the
					// final separator from its parent. Continue to that unary parent so
					// the root-collapse path cannot publish an unnecessary tree level.
					if (!nodeFor(path[pathIndex - 1U]).internal_entries.empty()) {
						return;
					}
				}
				--pathIndex;
			}
		}

		[[nodiscard]] bool rebalanceLeaf(OverlayMutationNode &parent, size_t leftOrdinal,
										 const OverlayPageReference &leftRef, const OverlayPageReference &rightRef,
										 OverlayMutationNode &left, OverlayMutationNode &right) {
			if (!left.right_sibling.has_value() || !right.left_sibling.has_value() || *left.right_sibling != rightRef ||
				*right.left_sibling != leftRef) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable transaction-local B+Tree overlay found leaf siblings with broken links");
			}
			std::vector<BTreePageEntry> combined = left.leaf_entries;
			combined.insert(combined.end(), right.leaf_entries.begin(), right.leaf_entries.end());
			std::optional<size_t> redistribution;
			uint64_t smallestSkew = std::numeric_limits<uint64_t>::max();
			for (size_t split = 1U; split < combined.size(); ++split) {
				OverlayMutationNode leftCandidate = left;
				OverlayMutationNode rightCandidate = right;
				leftCandidate.leaf_entries.assign(combined.begin(), combined.begin() + static_cast<ptrdiff_t>(split));
				rightCandidate.leaf_entries.assign(combined.begin() + static_cast<ptrdiff_t>(split), combined.end());
				try {
					const uint32_t leftBytes = encodedNodeOccupancy(materializePreview(leftCandidate));
					const uint32_t rightBytes = encodedNodeOccupancy(materializePreview(rightCandidate));
					if (leftBytes >= RSS_BTREE_MIN_NODE_OCCUPIED_BYTES &&
						rightBytes >= RSS_BTREE_MIN_NODE_OCCUPIED_BYTES) {
						const uint64_t skew = leftBytes > rightBytes ? leftBytes - rightBytes : rightBytes - leftBytes;
						if (!redistribution.has_value() || skew < smallestSkew) {
							redistribution = split;
							smallestSkew = skew;
						}
					}
				} catch (const FormatException &) {
				}
			}
			if (redistribution.has_value()) {
				left.leaf_entries.assign(combined.begin(), combined.begin() + static_cast<ptrdiff_t>(*redistribution));
				right.leaf_entries.assign(combined.begin() + static_cast<ptrdiff_t>(*redistribution), combined.end());
				left.dirty = true;
				right.dirty = true;
				parent.internal_entries[leftOrdinal].key = right.leaf_entries.front().key;
				parent.dirty = true;
				return true;
			}
			const bool retainRight = left.leaf_entries.empty() && !right.leaf_entries.empty();
			OverlayMutationNode merged = retainRight ? right : left;
			merged.leaf_entries = std::move(combined);
			if (retainRight) {
				merged.left_sibling = left.left_sibling;
			} else {
				merged.right_sibling = right.right_sibling;
			}
			if (!fits(merged)) {
				if (!left.leaf_entries.empty() && !right.leaf_entries.empty()) {
					// The current nonempty leaf remains readable and ordered. A later
					// delete may make a merge possible; publishing no rebalance avoids
					// manufacturing an invalid variable-length page layout.
					return false;
				}
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable transaction-local B+Tree overlay cannot merge an empty underfull leaf");
			}
			if (retainRight) {
				right.leaf_entries = std::move(merged.leaf_entries);
				right.left_sibling = merged.left_sibling;
				right.dirty = true;
				retire(leftRef);
				if (right.left_sibling.has_value()) {
					OverlayMutationNode &previous = loadNode(*right.left_sibling, 0U);
					previous.right_sibling = rightRef;
					previous.dirty = true;
				}
				if (leftOrdinal == 0U) {
					parent.leftmost_child = rightRef;
					parent.internal_entries.erase(parent.internal_entries.begin());
				} else {
					parent.internal_entries.erase(parent.internal_entries.begin() +
												  static_cast<ptrdiff_t>(leftOrdinal - 1U));
				}
			} else {
				left.leaf_entries = std::move(merged.leaf_entries);
				left.right_sibling = merged.right_sibling;
				left.dirty = true;
				retire(rightRef);
				if (left.right_sibling.has_value()) {
					OverlayMutationNode &next = loadNode(*left.right_sibling, 0U);
					next.left_sibling = leftRef;
					next.dirty = true;
				}
				parent.internal_entries.erase(parent.internal_entries.begin() + static_cast<ptrdiff_t>(leftOrdinal));
			}
			parent.dirty = true;
			return true;
		}

		void rebalanceInternal(OverlayMutationNode &parent, size_t leftOrdinal, const OverlayPageReference &leftRef,
							   const OverlayPageReference &rightRef, OverlayMutationNode &left,
							   OverlayMutationNode &right, size_t) {
			OverlayInternalEntry bridge = parent.internal_entries[leftOrdinal];
			bridge.right_child = *right.leftmost_child;
			std::vector<OverlayInternalEntry> combined = left.internal_entries;
			combined.push_back(bridge);
			combined.insert(combined.end(), right.internal_entries.begin(), right.internal_entries.end());
			std::optional<size_t> redistribution;
			uint64_t smallestSkew = std::numeric_limits<uint64_t>::max();
			for (size_t promoted = 1U; promoted + 1U < combined.size(); ++promoted) {
				OverlayMutationNode leftCandidate = left;
				OverlayMutationNode rightCandidate = right;
				leftCandidate.internal_entries.assign(combined.begin(),
													  combined.begin() + static_cast<ptrdiff_t>(promoted));
				rightCandidate.leftmost_child = combined[promoted].right_child;
				rightCandidate.internal_entries.assign(combined.begin() + static_cast<ptrdiff_t>(promoted + 1U),
													   combined.end());
				try {
					const uint32_t leftBytes = encodedNodeOccupancy(materializePreview(leftCandidate));
					const uint32_t rightBytes = encodedNodeOccupancy(materializePreview(rightCandidate));
					if (leftBytes >= RSS_BTREE_MIN_NODE_OCCUPIED_BYTES &&
						rightBytes >= RSS_BTREE_MIN_NODE_OCCUPIED_BYTES) {
						const uint64_t skew = leftBytes > rightBytes ? leftBytes - rightBytes : rightBytes - leftBytes;
						if (!redistribution.has_value() || skew < smallestSkew) {
							redistribution = promoted;
							smallestSkew = skew;
						}
					}
				} catch (const FormatException &error) {
					if (error.error() != FormatError::FERR_INVALID_STATE) {
						throw;
					}
				}
			}
			if (redistribution.has_value()) {
				left.internal_entries.assign(combined.begin(),
											 combined.begin() + static_cast<ptrdiff_t>(*redistribution));
				right.leftmost_child = combined[*redistribution].right_child;
				right.internal_entries.assign(combined.begin() + static_cast<ptrdiff_t>(*redistribution + 1U),
											  combined.end());
				left.dirty = true;
				right.dirty = true;
				parent.internal_entries[leftOrdinal].key = combined[*redistribution].key;
				parent.dirty = true;
				return;
			}
			OverlayMutationNode merged = left;
			merged.internal_entries = std::move(combined);
			if (!fits(merged)) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable transaction-local B+Tree overlay cannot merge an underfull internal pair");
			}
			left.internal_entries = std::move(merged.internal_entries);
			left.dirty = true;
			retire(rightRef);
			parent.internal_entries.erase(parent.internal_entries.begin() + static_cast<ptrdiff_t>(leftOrdinal));
			parent.dirty = true;
		}

		void collapseUnaryRoot() {
			while (true) {
				OverlayMutationNode &candidate = nodeFor(root_);
				if (candidate.header.kind != BTreeNodeKind::BPN_INTERNAL || !candidate.internal_entries.empty()) {
					break;
				}
				const OverlayPageReference child = *candidate.leftmost_child;
				retire(root_);
				root_ = child;
			}
			OverlayMutationNode &root = nodeFor(root_);
			if (root.header.kind == BTreeNodeKind::BPN_LEAF &&
				(root.left_sibling.has_value() || root.right_sibling.has_value())) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable transaction-local B+Tree overlay cannot publish a root leaf with siblings");
			}
		}

		[[nodiscard]] SlottedPagePool &pages() const { return const_cast<SlottedPagePool &>(store_.pages_); }

		const BTreePageStore &store_;
		RelocatableMetadataStore &metadata_;
		const MetadataSnapshotPin &snapshot_;
		uint64_t index_id_ = 0U;
		BTreeLeafValueLayout leaf_value_layout_ = BTreeLeafValueLayout::BLVL_U64;
		bool rebuild_on_frame_exhaustion_ = false;
		bool force_fresh_rebuild_ = false;
		bool fresh_rebuild_applied_ = false;
		IndexRootLocation expected_root_;
		// loadRoot() runs while root_ is initialized and immediately populates
		// this map, so it must be constructed before root_.
		std::map<uint64_t, ExistingOverlayMutationNode> existing_nodes_;
		OverlayPageReference root_;
		std::vector<OverlayMutationNode> virtual_nodes_;
	};

	namespace {

		struct RootLeafMutationSource {
			IndexRootLocation root;
			CatalogEntry pool;
			PageLocation location;
			BTreePage page;
			BTreePageHeader header;
			LogSequenceNumber owner_lsn{};
		};

		[[nodiscard]] RootLeafMutationSource loadRootLeafMutationSource(const BTreePageStore &store,
																		RelocatableMetadataStore &metadata,
																		const MetadataSnapshotPin &snapshot,
																		uint64_t indexId) {
			if (!snapshot.isValid() || indexId == 0U) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "relocatable root-leaf mutation plan requires a live snapshot and Index ID");
			}
			const IndexRootLocation root = metadata.findIndexRoot(snapshot, indexId);
			BTreePage page = store.read(snapshot, root.root_page_id);
			const BTreePageHeader header = page.header();
			const uint8_t invalidRoot = static_cast<uint8_t>(header.index_id != indexId) |
										static_cast<uint8_t>(header.page_id != root.root_page_id) |
										static_cast<uint8_t>(header.generation != root.root_generation) |
										static_cast<uint8_t>(header.kind != BTreeNodeKind::BPN_LEAF) |
										static_cast<uint8_t>(header.left_sibling.isValid()) |
										static_cast<uint8_t>(header.right_sibling.isValid());
			if (invalidRoot != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable root-leaf mutation plan requires an unsplit current leaf root");
			}
			const PageLocation location = metadata.findPageLocation(snapshot, root.root_page_id);
			const CatalogEntry pool = metadata.findCatalog(snapshot, location.segment_id);
			const SlottedPage &slotted = page.slottedPage();
			const LogSequenceNumber ownerLsn = slotted.ownerCommittedLsn();
			const uint8_t invalidLocation = static_cast<uint8_t>(pool.kind != SegmentKind::SGK_INDEX_PAGE_POOL) |
											static_cast<uint8_t>(pool.generation != location.generation) |
											static_cast<uint8_t>(pool.state != SegmentState::SGS_SEALED) |
											static_cast<uint8_t>(location.segment_id != pool.segment_id) |
											static_cast<uint8_t>(slotted.pageId() != root.root_page_id) |
											static_cast<uint8_t>(slotted.pageGeneration() != location.generation) |
											static_cast<uint8_t>(slotted.ownerKind() != PageOwnerKind::POK_INDEX) |
											static_cast<uint8_t>(slotted.ownerId() != indexId) |
											static_cast<uint8_t>(location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
											static_cast<uint8_t>(location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
											static_cast<uint8_t>(!ownerLsn.isValid());
			if (invalidLocation != 0U) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable root-leaf mutation plan found an unavailable or incompatible index page");
			}
			return {root, pool, location, std::move(page), header, ownerLsn};
		}

		[[nodiscard]] BTreeRootLeafMutationPlan makeRootLeafMutationPlan(const RootLeafMutationSource &source,
																		 std::span<const BTreePageEntry> entries,
																		 const char *structuralMessage) {
			const PageOwnerHeader owner = indexPageOwner(source.root.index_id, source.header.page_id,
														 source.header.generation, source.owner_lsn);
			try {
				const BTreePage replacement = makeLeafPage(source.header, owner, {}, {}, entries);
				return {source.root,
						source.pool,
						source.location,
						source.owner_lsn,
						replacement.serialize(),
						replacement.slottedPage().stats().contiguous_free_bytes};
			} catch (const FormatException &error) {
				if (error.error() != FormatError::FERR_INVALID_STATE) {
					throw;
				}
				throw FormatException(FormatError::FERR_INVALID_STATE, structuralMessage);
			}
		}

	} // namespace

	BTreePageStore::BTreePageStore(SlottedPagePool &pages, RelocatableMetadataStore &metadata) :
		pages_(pages), metadata_(metadata) {
		if (!pages_.usesMetadataStore(metadata_)) {
			throw std::invalid_argument(
					"relocatable B+Tree page store requires its slotted-page pool's metadata store");
		}
	}

	std::optional<SlottedPagePoolAllocation> BTreePageStore::allocate(const SlottedPagePoolAllocationRequest &request,
																	  const BTreePage &page, PhysicalSlotId firstSlot,
																	  PhysicalSlotId lastSlot,
																	  LogSequenceNumber checkpointLsn,
																	  const MetadataPublishOptions &options) {
		const uint8_t invalidKind = static_cast<uint8_t>(request.pool_kind != SegmentKind::SGK_INDEX_PAGE_POOL) |
									static_cast<uint8_t>(request.page_owner_kind != PageOwnerKind::POK_INDEX);
		if (invalidKind != 0U) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable B+Tree page allocation requires an index page pool and index owner");
		}
		const BTreePageHeader header = page.header();
		const uint8_t invalid = static_cast<uint8_t>(header.index_id != request.page_owner_id) |
								static_cast<uint8_t>(header.page_id != request.page_id) |
								static_cast<uint8_t>(header.generation != request.generation) |
								static_cast<uint8_t>(page.slottedPage().ownerCommittedLsn() != checkpointLsn);
		if (invalid != 0U) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable B+Tree page does not match its atomic pool-allocation identity");
		}
		return pages_.allocatePage(request, page.slottedPage(), firstSlot, lastSlot, checkpointLsn, options);
	}

	SharedPagePoolAllocation BTreePageStore::append(const SlottedPagePoolAppendRequest &request, const BTreePage &page,
													LogSequenceNumber checkpointLsn,
													const MetadataPublishOptions &options) {
		const BTreePageHeader header = page.header();
		const uint8_t invalid = static_cast<uint8_t>(header.index_id != request.page_owner_id) |
								static_cast<uint8_t>(header.page_id != request.page_id) |
								static_cast<uint8_t>(header.generation != request.generation) |
								static_cast<uint8_t>(request.page_owner_kind != PageOwnerKind::POK_INDEX) |
								static_cast<uint8_t>(page.slottedPage().ownerCommittedLsn() != checkpointLsn);
		if (invalid != 0U) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable appended B+Tree page does not match its index-pool identity");
		}
		return pages_.appendPage(request, page.slottedPage(), checkpointLsn, options);
	}

	BTreePage BTreePageStore::read(const MetadataSnapshotPin &snapshot, PageId pageId, BufferPoolReadHint hint) const {
		const SlottedPage raw = pages_.read(snapshot, pageId, hint);
		PageOwnerHeader owner;
		owner.page_id = raw.pageId();
		owner.owner_id = raw.ownerId();
		owner.kind = raw.ownerKind();
		owner.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
		owner.generation = raw.pageGeneration();
		owner.committed_lsn = raw.ownerCommittedLsn();
		return BTreePage::open(owner, raw.serialize());
	}

	BTreeRootLeafMutationPlan BTreePageStore::planRootLeafUpsert(const MetadataSnapshotPin &snapshot, uint64_t indexId,
																 std::span<const std::byte> key, uint64_t value) const {
		RootLeafMutationSource source = loadRootLeafMutationSource(*this, metadata_, snapshot, indexId);
		std::vector<BTreePageEntry> entries = source.page.entries();
		bool replaced = false;
		for (BTreePageEntry &entry: entries) {
			if (std::ranges::equal(entry.key, key)) {
				entry.value_or_child = value;
				replaced = true;
			}
		}
		if (!replaced) {
			entries.push_back({std::vector<std::byte>(key.begin(), key.end()), value});
			std::ranges::sort(entries, {}, &BTreePageEntry::key);
		}
		return makeRootLeafMutationPlan(source, entries,
										"relocatable root-leaf upsert plan requires a structural split");
	}

	BTreeRootLeafMutationPlan BTreePageStore::planRootLeafErase(const MetadataSnapshotPin &snapshot, uint64_t indexId,
																std::span<const std::byte> key) const {
		RootLeafMutationSource source = loadRootLeafMutationSource(*this, metadata_, snapshot, indexId);
		std::vector<BTreePageEntry> entries = source.page.entries();
		const auto erased = std::ranges::find_if(
				entries, [&](const BTreePageEntry &entry) { return std::ranges::equal(entry.key, key); });
		if (erased == entries.end()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable root-leaf erase plan cannot remove an absent scalar key");
		}
		entries.erase(erased);
		return makeRootLeafMutationPlan(source, entries, "relocatable root-leaf erase plan has an invalid replacement");
	}

	void BTreePageStore::prepareRootLeafMutation(const BTreeRootLeafMutationPlan &plan, const CatalogEntry &pool,
												 const PageLocation &source, PageLocation &replacement,
												 LogSequenceNumber checkpointLsn) const {
		const uint8_t invalidPlan =
				static_cast<uint8_t>(!checkpointLsn.isValid()) |
				static_cast<uint8_t>(plan.expected_root.index_id == 0U) |
				static_cast<uint8_t>(!plan.expected_root.root_page_id.isValid()) |
				static_cast<uint8_t>(!plan.expected_root.root_generation.isValid()) |
				static_cast<uint8_t>(plan.expected_root.root_page_id != plan.expected_location.page_id) |
				static_cast<uint8_t>(plan.expected_root.root_generation != plan.expected_location.generation) |
				static_cast<uint8_t>(pool.segment_id != plan.expected_pool.segment_id) |
				static_cast<uint8_t>(pool.physical_slot != plan.expected_pool.physical_slot) |
				static_cast<uint8_t>(pool.kind != SegmentKind::SGK_INDEX_PAGE_POOL) |
				static_cast<uint8_t>(pool.kind != plan.expected_pool.kind) |
				static_cast<uint8_t>(pool.owner_id != plan.expected_pool.owner_id) |
				static_cast<uint8_t>(pool.generation != plan.expected_pool.generation) |
				static_cast<uint8_t>(pool.committed_lsn != plan.expected_pool.committed_lsn) |
				static_cast<uint8_t>(pool.state != SegmentState::SGS_SEALED) |
				static_cast<uint8_t>(source != plan.expected_location) |
				static_cast<uint8_t>(source.segment_id != pool.segment_id) |
				static_cast<uint8_t>(source.generation != pool.generation) |
				static_cast<uint8_t>(!plan.expected_owner_lsn.isValid()) |
				static_cast<uint8_t>(plan.replacement_payload.size() != RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES) |
				static_cast<uint8_t>(plan.replacement_free_bytes >
									 RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES - RSS_SLOTTED_PAGE_HEADER_BYTES);
		if (invalidPlan != 0U) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable root-leaf upsert frame plan changed before COW preparation");
		}
		const PageOwnerHeader owner =
				indexPageOwner(plan.expected_root.index_id, source.page_id, pool.generation, checkpointLsn);
		const BTreePage replacementPage = BTreePage::open(owner, plan.replacement_payload);
		const BTreePageHeader header = replacementPage.header();
		const uint8_t invalidPayload =
				static_cast<uint8_t>(header.index_id != plan.expected_root.index_id) |
				static_cast<uint8_t>(header.page_id != source.page_id) |
				static_cast<uint8_t>(header.generation != pool.generation) |
				static_cast<uint8_t>(header.kind != BTreeNodeKind::BPN_LEAF) |
				static_cast<uint8_t>(header.level != 0U) | static_cast<uint8_t>(header.left_sibling.isValid()) |
				static_cast<uint8_t>(header.right_sibling.isValid()) |
				static_cast<uint8_t>(replacementPage.slottedPage().stats().contiguous_free_bytes !=
									 plan.replacement_free_bytes);
		if (invalidPayload != 0U) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable root-leaf upsert frame plan has an invalid replacement payload");
		}
		const SharedPagePoolPreparedReplacement prepared{owner, plan.expected_owner_lsn, plan.replacement_payload,
														 plan.replacement_free_bytes};
		const std::array<PageLocation, 1> expected{source};
		const std::array<SharedPagePoolPreparedReplacement, 1> replacements{prepared};
		const std::array<SharedPagePoolPreparedAppend, 0> noAppends{};
		std::array<PageLocation, 1> replacementLocations{replacement};
		std::array<PageLocation, 0> noAppendLocations{};
		pages_.prepareMutationPages(pool, expected, replacements, noAppends, replacementLocations, noAppendLocations,
									checkpointLsn);
		replacement = replacementLocations.front();
	}

	void BTreePageStore::prepareNewPoolInitialPages(const CatalogEntry &pool, std::span<const BTreePage> pages,
													std::span<PageLocation> locations,
													LogSequenceNumber checkpointLsn) const {
		const uint8_t invalidPool =
				static_cast<uint8_t>(pool.kind != SegmentKind::SGK_INDEX_PAGE_POOL) |
				static_cast<uint8_t>(pool.owner_id == 0U) | static_cast<uint8_t>(!pool.segment_id.isValid()) |
				static_cast<uint8_t>(!pool.physical_slot.isValid()) |
				static_cast<uint8_t>(pool.generation != Generation(1U)) |
				static_cast<uint8_t>(pool.state != SegmentState::SGS_SEALED) |
				static_cast<uint8_t>(pool.committed_lsn != checkpointLsn) | static_cast<uint8_t>(pages.empty()) |
				static_cast<uint8_t>(pages.size() != locations.size()) |
				static_cast<uint8_t>(pages.size() > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
				static_cast<uint8_t>(!checkpointLsn.isValid());
		if (invalidPool != 0U) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable initial B+Tree preparation has an invalid pool or page batch");
		}
		std::set<PageId> pageIds;
		std::vector<SharedPagePoolPreparedAppend> prepared;
		prepared.reserve(pages.size());
		for (size_t ordinal = 0U; ordinal < pages.size(); ++ordinal) {
			const BTreePageHeader header = pages[ordinal].header();
			const PageLocation &location = locations[ordinal];
			const std::vector<std::byte> payload = pages[ordinal].serialize();
			const uint8_t invalidPage =
					static_cast<uint8_t>(!location.page_id.isValid()) |
					static_cast<uint8_t>(location.segment_id != pool.segment_id) |
					static_cast<uint8_t>(location.generation != pool.generation) |
					static_cast<uint8_t>(location.page_index != 0U) | static_cast<uint8_t>(location.free_bytes != 0U) |
					static_cast<uint8_t>(!pageIds.insert(location.page_id).second) |
					static_cast<uint8_t>(header.index_id != pool.owner_id) |
					static_cast<uint8_t>(header.page_id != location.page_id) |
					static_cast<uint8_t>(header.generation != pool.generation) |
					static_cast<uint8_t>(pages[ordinal].slottedPage().pageId() != location.page_id) |
					static_cast<uint8_t>(pages[ordinal].slottedPage().pageGeneration() != pool.generation) |
					static_cast<uint8_t>(pages[ordinal].slottedPage().ownerId() != pool.owner_id) |
					static_cast<uint8_t>(pages[ordinal].slottedPage().ownerKind() != PageOwnerKind::POK_INDEX) |
					static_cast<uint8_t>(pages[ordinal].slottedPage().ownerCommittedLsn() != checkpointLsn);
			if (invalidPage != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable initial B+Tree preparation has an invalid page identity");
			}
			PageOwnerHeader owner = indexPageOwner(pool.owner_id, location.page_id, pool.generation, checkpointLsn);
			owner.payload_bytes = static_cast<uint32_t>(payload.size());
			prepared.push_back({std::move(owner), payload, pages[ordinal].slottedPage().stats().contiguous_free_bytes});
		}
		pages_.prepareNewPoolPages(pool, prepared, locations, checkpointLsn);
	}

	bool BTreePageStore::hasReplacementFrameCapacity(const CatalogEntry &pool, size_t requiredFrames) const {
		if (pool.kind != SegmentKind::SGK_INDEX_PAGE_POOL || pool.state != SegmentState::SGS_SEALED) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable root-leaf upsert capacity query requires one current Index page pool");
		}
		const uint8_t availableFrames = pages_.pool_.availableFrameCount(pool);
		const bool hasCapacity =
				requiredFrames <= std::numeric_limits<uint8_t>::max() && availableFrames >= requiredFrames;
		return hasCapacity;
	}

	SharedPagePoolAllocation BTreePageStore::replace(const BTreePage &page, LogSequenceNumber checkpointLsn,
													 const MetadataPublishOptions &options) {
		return pages_.replacePage(page.slottedPage(), checkpointLsn, options);
	}

	SharedPagePoolAllocation BTreePageStore::evacuate(const BTreePage &page, SegmentId destinationSegmentId,
													  LogSequenceNumber checkpointLsn,
													  const MetadataPublishOptions &options) {
		const BTreePageHeader header = page.header();
		if (!destinationSegmentId.isValid() || !checkpointLsn.isValid()) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable B+Tree page evacuation requires a destination pool and checkpoint LSN");
		}
		MetadataSnapshotPin snapshot = metadata_.openSnapshot();
		try {
			const SharedPagePoolContents source = pages_.pool_.read(snapshot, header.page_id);
			const CatalogEntry target = metadata_.findCatalog(snapshot, destinationSegmentId);
			const uint8_t invalidSource =
					static_cast<uint8_t>(source.pool.kind != SegmentKind::SGK_INDEX_PAGE_POOL) |
					static_cast<uint8_t>(source.owner.page_id != header.page_id) |
					static_cast<uint8_t>(source.owner.owner_id != header.index_id) |
					static_cast<uint8_t>(source.owner.kind != PageOwnerKind::POK_INDEX) |
					static_cast<uint8_t>(source.owner.generation != header.generation) |
					static_cast<uint8_t>(source.owner.committed_lsn != page.slottedPage().ownerCommittedLsn()) |
					static_cast<uint8_t>(target.kind != SegmentKind::SGK_INDEX_PAGE_POOL);
			if (invalidSource != 0U) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable B+Tree page evacuation requires a current compatible typed page and pool");
			}
			const std::optional<IndexRootLocation> visibleRoot =
					metadata_.findIndexRootOptional(snapshot, header.index_id);
			std::optional<IndexRootLocation> expectedRoot;
			std::optional<IndexRootLocation> replacementRoot;
			if (visibleRoot.has_value() && visibleRoot->root_page_id == header.page_id) {
				if (visibleRoot->root_generation != header.generation) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "relocatable B+Tree root evacuation found a stale root generation");
				}
				expectedRoot = visibleRoot;
				replacementRoot = {header.index_id, header.page_id, target.generation};
			}
			PageOwnerHeader destinationOwner = source.owner;
			destinationOwner.generation = target.generation;
			destinationOwner.committed_lsn = checkpointLsn;
			const BTreePage relocated = page.rebindOwner(destinationOwner);
			const std::vector<std::byte> bytes = relocated.serialize();
			snapshot.release();

			SharedPagePoolEvacuationRequest request;
			request.source_segment_id = source.pool.segment_id;
			request.source_generation = source.pool.generation;
			request.destination_segment_id = destinationSegmentId;
			request.destination_generation = target.generation;
			request.page_id = header.page_id;
			request.expected_owner_lsn = source.owner.committed_lsn;
			request.page_owner_id = header.index_id;
			request.page_owner_kind = PageOwnerKind::POK_INDEX;
			request.page_free_bytes = relocated.slottedPage().stats().contiguous_free_bytes;
			request.payload = bytes;
			return pages_.pool_.evacuateIndexPage(request, expectedRoot, replacementRoot, checkpointLsn, options);
		} catch (...) {
			snapshot.release();
			throw;
		}
	}

	Superblock BTreePageStore::publishRoots(std::span<const IndexRootLocation> roots,
											std::span<const uint64_t> erasedIndexIds, LogSequenceNumber checkpointLsn,
											const MetadataPublishOptions &options) {
		if (!roots.empty()) {
			MetadataSnapshotPin snapshot = metadata_.openSnapshot();
			for (const IndexRootLocation &root: roots) {
				const uint8_t invalidIdentity = static_cast<uint8_t>(root.index_id == 0U) |
												static_cast<uint8_t>(!root.root_page_id.isValid()) |
												static_cast<uint8_t>(!root.root_generation.isValid());
				if (invalidIdentity != 0U) {
					throw FormatException(FormatError::FERR_INVALID_HEADER,
										  "relocatable B+Tree root publication has an invalid logical identity");
				}
				const BTreePageHeader header = read(snapshot, root.root_page_id).header();
				const uint8_t invalidOwner = static_cast<uint8_t>(header.index_id != root.index_id) |
											 static_cast<uint8_t>(header.page_id != root.root_page_id) |
											 static_cast<uint8_t>(header.generation != root.root_generation);
				if (invalidOwner != 0U) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "relocatable B+Tree root publication does not match the page's index owner");
				}
			}
			snapshot.release();
		}
		return metadata_.upsertIndexRoots(roots, erasedIndexIds, checkpointLsn, options);
	}

	Superblock BTreePageStore::retireIndexTreePages(Generation expectedMetadataGeneration,
													std::span<const IndexRootLocation> erasedRoots,
													std::span<const RetiredIndexTreePage> pages,
													LogSequenceNumber checkpointLsn,
													const MetadataPublishOptions &publishOptions) {
		if (erasedRoots.empty() || pages.empty()) { // ZYX_COV_EXCL_LINE: the private reclaimer calls this only
			// after a nonempty stale-root traversal has included every root page.
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable Index-tree retirement requires roots and pages");
		}
		std::set<uint64_t> rootIds;
		uint64_t previousRootIndexId = 0U;
		for (const IndexRootLocation &root: erasedRoots) {
			const uint8_t invalidRoot = static_cast<uint8_t>(root.index_id == 0U) |
										static_cast<uint8_t>(root.index_id <= previousRootIndexId) |
										static_cast<uint8_t>(!root.root_page_id.isValid()) |
										static_cast<uint8_t>(!root.root_generation.isValid());
			if (invalidRoot != 0U || !rootIds.insert(root.index_id).second) { // ZYX_COV_EXCL_LINE: the
				// reclaimer canonically sorts and validates every Index Root before this private boundary.
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "relocatable Index-tree retirement has non-canonical Index Roots");
			}
			previousRootIndexId = root.index_id;
		}
		std::vector<PageRetirement> retirements;
		retirements.reserve(pages.size());
		uint64_t previousPageId = 0U;
		for (const RetiredIndexTreePage &page: pages) {
			const uint8_t invalid = static_cast<uint8_t>(!page.location.page_id.isValid()) |
									static_cast<uint8_t>(page.location.page_id.value() <= previousPageId) |
									static_cast<uint8_t>(rootIds.find(page.index_id) == rootIds.end()) |
									static_cast<uint8_t>(!page.owner_lsn.isValid());
			if (invalid != 0U) { // ZYX_COV_EXCL_LINE: the reclaimer emits strictly increasing pages derived
				// from one validated B+Tree traversal, all owned by one planned root.
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "relocatable Index-tree retirement has a non-canonical page plan");
			}
			retirements.push_back({page.location, page.index_id, PageOwnerKind::POK_INDEX, page.owner_lsn});
			previousPageId = page.location.page_id.value();
		}
		for (const IndexRootLocation &root: erasedRoots) {
			const auto rootPage = std::ranges::lower_bound(
					pages, root.root_page_id.value(), {},
					[](const RetiredIndexTreePage &page) { return page.location.page_id.value(); });
			const bool rootIsRetired = rootPage != pages.end() && rootPage->location.page_id == root.root_page_id &&
									   rootPage->location.generation == root.root_generation &&
									   rootPage->index_id == root.index_id;
			if (!rootIsRetired) { // ZYX_COV_EXCL_LINE: the traversal starts at each root and records that
				// exact `(PageId, generation, IndexId)` tuple before this private publication boundary.
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "relocatable Index-tree retirement omits a root page from its retirement plan");
			}
		}
		const ExistingPoolPagesRetirementPreparer preparer =
				[this, checkpointLsn](std::span<const ExistingPoolPageRetirementBatch> batches) {
					for (const ExistingPoolPageRetirementBatch &batch: batches) {
						std::vector<SharedPagePoolPreparedRetirement> prepared;
						prepared.reserve(batch.retirements.size());
						for (const PageRetirement &retirement: batch.retirements) {
							prepared.push_back({retirement.location, retirement.expected_owner_id,
												retirement.expected_owner_kind, retirement.expected_owner_lsn});
						}
						const std::array<PageLocation, 0> noReplacements{};
						const std::array<SharedPagePoolPreparedReplacement, 0> noReplacementPayloads{};
						const std::array<SharedPagePoolPreparedAppend, 0> noAppends{};
						std::array<PageLocation, 0> noReplacementLocations{};
						std::array<PageLocation, 0> noAppendLocations{};
						pages_.prepareMutationPages(batch.pool, noReplacements, noReplacementPayloads, noAppends,
													noReplacementLocations, noAppendLocations, checkpointLsn, prepared);
					}
				};
		return metadata_.retirePagesInExistingPools(expectedMetadataGeneration, erasedRoots, retirements, checkpointLsn,
													preparer, publishOptions);
	}

	void BTreePageStore::reclaimRetiredFrames(SegmentId segmentId) const { pages_.reclaimRetiredFrames(segmentId); }

	Superblock BTreePageStore::materializeAndPublishTree(SegmentId poolSegmentId, uint64_t indexId, size_t pageCount,
														 size_t rootPageOrdinal, LogSequenceNumber checkpointLsn,
														 const NewTreePageFactory &factory,
														 const MetadataPublishOptions &options) {
		const uint8_t invalidInput = static_cast<uint8_t>(!poolSegmentId.isValid()) |
									 static_cast<uint8_t>(indexId == 0U) | static_cast<uint8_t>(pageCount == 0U) |
									 static_cast<uint8_t>(rootPageOrdinal >= pageCount) |
									 static_cast<uint8_t>(!factory);
		if (invalidInput != 0U) {
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"relocatable B+Tree tree materialization requires valid identity, page count, root, and factory");
		}
		// Remove unreachable frames from a prior failed batch before reserving this
		// pool's free frames under the metadata writer.
		pages_.reconcilePool(poolSegmentId);
		const NewIndexTreePagesPreparer preparer = [this, indexId, pageCount, checkpointLsn,
													&factory](const CatalogEntry &pool, std::span<const PageId> pageIds,
															  std::span<PageLocation> locations) {
			const std::vector<BTreePage> materialized = factory(pool, pageIds);
			if (materialized.size() != pageCount) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable B+Tree page factory did not materialize the requested page batch");
			}
			std::vector<SharedPagePoolPreparedAppend> prepared;
			prepared.reserve(materialized.size());
			for (size_t index = 0; index < materialized.size(); ++index) {
				const BTreePage &page = materialized[index];
				const BTreePageHeader header = page.header();
				const uint8_t invalid =
						static_cast<uint8_t>(header.index_id != indexId) |
						static_cast<uint8_t>(header.page_id != pageIds[index]) |
						static_cast<uint8_t>(header.generation != pool.generation) |
						static_cast<uint8_t>(page.slottedPage().pageId() != pageIds[index]) |
						static_cast<uint8_t>(page.slottedPage().pageGeneration() != pool.generation) |
						static_cast<uint8_t>(page.slottedPage().ownerId() != indexId) |
						static_cast<uint8_t>(page.slottedPage().ownerKind() != PageOwnerKind::POK_INDEX) |
						static_cast<uint8_t>(page.slottedPage().ownerCommittedLsn() != checkpointLsn);
				if (invalid != 0U) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable B+Tree page factory returned a page with the wrong durable identity");
				}
				std::vector<std::byte> bytes = page.serialize();
				PageOwnerHeader owner;
				owner.page_id = pageIds[index];
				owner.owner_id = indexId;
				owner.kind = PageOwnerKind::POK_INDEX;
				owner.payload_bytes = static_cast<uint32_t>(bytes.size());
				owner.generation = pool.generation;
				owner.committed_lsn = checkpointLsn;
				prepared.push_back(
						{std::move(owner), std::move(bytes), page.slottedPage().stats().contiguous_free_bytes});
			}
			pages_.prepareAppendPages(pool, prepared, locations, checkpointLsn);
		};
		return metadata_.materializeNewIndexTreePages(poolSegmentId, indexId, pageCount, rootPageOrdinal, checkpointLsn,
													  preparer, options);
	}

	Superblock BTreePageStore::materializeAndPublishTreeAcrossPools(
			std::span<const IndexTreeMaterializationPoolOptions> pools, uint64_t indexId,
			IndexTreeMaterializedPageReference rootPage, LogSequenceNumber checkpointLsn,
			const MultiPoolNewTreePageFactory &factory, const MetadataPublishOptions &options) {
		const uint8_t invalidInput = static_cast<uint8_t>(pools.empty()) | static_cast<uint8_t>(indexId == 0U) |
									 static_cast<uint8_t>(!checkpointLsn.isValid()) | static_cast<uint8_t>(!factory) |
									 static_cast<uint8_t>(rootPage.pool_ordinal >= pools.size());
		if (invalidInput != 0U) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable multi-pool B+Tree materialization requires pools, an index, a root, an "
								  "LSN, and a factory");
		}

		std::vector<NewIndexTreeMaterializationPoolRequest> requests;
		requests.reserve(pools.size());
		for (const IndexTreeMaterializationPoolOptions &pool: pools) {
			const uint8_t invalidPool = static_cast<uint8_t>(!pool.first_slot.isValid()) |
										static_cast<uint8_t>(!pool.last_slot.isValid()) |
										static_cast<uint8_t>(pool.first_slot > pool.last_slot) |
										static_cast<uint8_t>(pool.page_count == 0U) |
										static_cast<uint8_t>(pool.page_count > RSS_PAGE_POOL_DATA_FRAME_COUNT);
			if (invalidPool != 0U) {
				throw FormatException(
						FormatError::FERR_INVALID_HEADER,
						"relocatable multi-pool B+Tree materialization has an invalid pool range or page count");
			}
			requests.push_back({pool.first_slot, pool.last_slot, pool.pool_owner_id, pool.page_count});
		}
		if (rootPage.page_ordinal >= pools[rootPage.pool_ordinal].page_count) {
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"relocatable multi-pool B+Tree materialization root does not name one requested page");
		}

		const IndexTreeMultiPoolMaterializationPreparer preparer =
				[this, indexId, checkpointLsn, &factory](std::vector<IndexTreeMaterializationPoolBatch> &batches) {
					std::vector<IndexTreeMaterializedPool> materializedPools;
					materializedPools.reserve(batches.size());
					for (const IndexTreeMaterializationPoolBatch &batch: batches) {
						materializedPools.push_back({batch.pool, batch.page_ids});
					}
					for (size_t poolOrdinal = 0; poolOrdinal < batches.size(); ++poolOrdinal) {
						IndexTreeMaterializationPoolBatch &batch = batches[poolOrdinal];
						std::vector<SharedPagePoolPreparedAppend> prepared;
						prepared.reserve(batch.page_ids.size());
						for (size_t pageOrdinal = 0; pageOrdinal < batch.page_ids.size(); ++pageOrdinal) {
							BTreePage page = factory(materializedPools, poolOrdinal, pageOrdinal);
							const BTreePageHeader header = page.header();
							const uint8_t invalid =
									static_cast<uint8_t>(header.index_id != indexId) |
									static_cast<uint8_t>(header.page_id != batch.page_ids[pageOrdinal]) |
									static_cast<uint8_t>(header.generation != batch.pool.generation) |
									static_cast<uint8_t>(page.slottedPage().pageId() != batch.page_ids[pageOrdinal]) |
									static_cast<uint8_t>(page.slottedPage().pageGeneration() != batch.pool.generation) |
									static_cast<uint8_t>(page.slottedPage().ownerId() != indexId) |
									static_cast<uint8_t>(page.slottedPage().ownerKind() != PageOwnerKind::POK_INDEX) |
									static_cast<uint8_t>(page.slottedPage().ownerCommittedLsn() != checkpointLsn);
							if (invalid != 0U) {
								throw FormatException(FormatError::FERR_INVALID_STATE,
													  "relocatable multi-pool B+Tree factory returned a page with the "
													  "wrong durable identity");
							}
							std::vector<std::byte> bytes = page.serialize();
							PageOwnerHeader owner = indexPageOwner(indexId, batch.page_ids[pageOrdinal],
																   batch.pool.generation, checkpointLsn);
							owner.payload_bytes = static_cast<uint32_t>(bytes.size());
							prepared.push_back({std::move(owner), std::move(bytes),
												page.slottedPage().stats().contiguous_free_bytes});
						}
						pages_.prepareNewPoolPages(batch.pool, prepared, batch.locations, checkpointLsn);
					}
				};
		return metadata_.materializeNewIndexTreePagesAcrossPools(
				requests, indexId, {rootPage.pool_ordinal, rootPage.page_ordinal}, checkpointLsn, preparer, options);
	}

	std::optional<SegmentId> BTreePageStore::selectAppendPool(const MetadataSnapshotPin &snapshot,
															  std::span<const PageLocation> expectedReplacements,
															  size_t appendedPageCount) const {
		// ZYX_COV_EXCL_START: only the local B+Tree planner calls this with its bounded nonempty append batch.
		if (appendedPageCount == 0U || appendedPageCount > RSS_PAGE_POOL_DATA_FRAME_COUNT) { // ZYX_COV_EXCL_LINE:
			// private mutation planners derive a nonzero append count bounded by one shared pool.
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable B+Tree append-pool selection requires a bounded nonempty append batch");
		}
		// ZYX_COV_EXCL_STOP
		std::map<uint64_t, size_t> replacementCounts;
		for (const PageLocation &location: expectedReplacements) {
			// ZYX_COV_EXCL_START: replacement locations come from one validated pinned mutation path.
			if (!location.segment_id.isValid() ||
				location.page_index <
						RSS_PAGE_POOL_FIRST_DATA_FRAME || // ZYX_COV_EXCL_LINE:
														  // Page Directory decoding and the pinned typed mutation
														  // plan already validate every location.
				location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable B+Tree append-pool selection has an invalid replacement location");
			}
			// ZYX_COV_EXCL_STOP
			++replacementCounts[location.segment_id.value()];
		}

		std::optional<SegmentId> selected;
		uint8_t smallestSurplus = std::numeric_limits<uint8_t>::max();
		for (const CatalogEntry &candidate: metadata_.findCurrentIndexPagePools(snapshot)) {
			const auto replacement = replacementCounts.find(candidate.segment_id.value());
			const size_t replacementCount = replacement == replacementCounts.end() ? 0U : replacement->second;
			if (replacementCount > RSS_PAGE_POOL_DATA_FRAME_COUNT - appendedPageCount) { // ZYX_COV_EXCL_LINE:
				// the bounded planner cannot name more than the remaining frames of one pool.
				continue;
			}
			const uint8_t requiredFrames = static_cast<uint8_t>(replacementCount + appendedPageCount);
			const uint8_t availableFrames = pages_.availableFrameCount(snapshot, candidate.segment_id);
			if (availableFrames < requiredFrames) {
				continue;
			}
			const uint8_t surplus = static_cast<uint8_t>(availableFrames - requiredFrames);
			// The Metadata scan is sorted by SegmentId, so an equal-surplus candidate
			// cannot improve the deterministic first choice.
			if (!selected.has_value() || surplus < smallestSurplus) {
				selected = candidate.segment_id;
				smallestSurplus = surplus;
			}
		}
		return selected;
	}

	NewIndexTreePoolRequest BTreePageStore::newPoolRequest(IndexPagePoolGrowthOptions growth, uint64_t indexId,
														   size_t appendedPageCount) {
		const uint8_t invalid = static_cast<uint8_t>(!growth.first_slot.isValid()) |
								static_cast<uint8_t>(!growth.last_slot.isValid()) |
								static_cast<uint8_t>(growth.first_slot > growth.last_slot) |
								static_cast<uint8_t>(indexId == 0U) | static_cast<uint8_t>(appendedPageCount == 0U) |
								static_cast<uint8_t>(appendedPageCount > RSS_PAGE_POOL_DATA_FRAME_COUNT);
		// ZYX_COV_EXCL_START: growth requests are constructed only after the local B+Tree planner validates every
		// field.
		if (invalid != 0U) {
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"relocatable B+Tree growth requires a valid physical-slot range and bounded append batch");
		}
		// ZYX_COV_EXCL_STOP
		return {growth.first_slot, growth.last_slot, growth.pool_owner_id, appendedPageCount};
	}

	SharedPagePoolAllocation BTreePageStore::upsert(uint64_t indexId, std::span<const std::byte> key, uint64_t value,
													LogSequenceNumber checkpointLsn,
													const MetadataPublishOptions &options) {
		auto result = upsertImpl(indexId, key, value, std::nullopt, checkpointLsn, options);
		if (auto *allocation = std::get_if<SharedPagePoolAllocation>(&result)) {
			return std::move(*allocation);
		}
		throw FormatException(FormatError::FERR_INVALID_STATE,
							  "relocatable B+Tree upsert unexpectedly deferred its structural publication");
	}

	SharedPagePoolAllocation BTreePageStore::upsert(uint64_t indexId, std::span<const std::byte> key, uint64_t value,
													IndexPagePoolGrowthOptions growth, LogSequenceNumber checkpointLsn,
													const MetadataPublishOptions &options) {
		auto result = upsertImpl(indexId, key, value, growth, checkpointLsn, options);
		if (auto *allocation = std::get_if<SharedPagePoolAllocation>(&result)) {
			return std::move(*allocation);
		}
		throw FormatException(FormatError::FERR_INVALID_STATE,
							  "relocatable B+Tree upsert unexpectedly deferred its structural publication");
	}

	BTreeStructuralMutationPlan BTreePageStore::planDeepStructuralUpsert(const MetadataSnapshotPin &snapshot,
																		 uint64_t indexId,
																		 std::span<const std::byte> key,
																		 uint64_t value) const {
		if (!snapshot.isValid() || indexId == 0U) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable deep structural B+Tree plan requires a live snapshot and Index ID");
		}
		// Plan-only execution returns before page preparation or publication while
		// reusing the validated split-propagation planner.  Its temporary page
		// owners must nevertheless form a representable successor to the pinned
		// metadata generation because the common planner validates that invariant.
		auto result = const_cast<BTreePageStore *>(this)->upsertImpl(
				indexId, key, value, std::nullopt, provisionalPlanCheckpointLsn(snapshot), {}, true, &snapshot);
		if (auto *plan = std::get_if<BTreeStructuralMutationPlan>(&result)) {
			return std::move(*plan);
		}
		throw FormatException(FormatError::FERR_INVALID_STATE,
							  "relocatable deep structural B+Tree plan requires a leaf split");
	}

	std::variant<SharedPagePoolAllocation, BTreeStructuralMutationPlan>
	BTreePageStore::upsertImpl(uint64_t indexId, std::span<const std::byte> key, uint64_t value,
							   std::optional<IndexPagePoolGrowthOptions> growth, LogSequenceNumber checkpointLsn,
							   const MetadataPublishOptions &options, bool planStructuralOnly,
							   const MetadataSnapshotPin *planningSnapshot) {
		if (indexId == 0U) {
			throw FormatException(FormatError::FERR_INVALID_HEADER, "relocatable B+Tree upsert requires an Index ID");
		}
		// Keep root selection and the complete COW path on one metadata generation.
		// Reopening after the root-type probe admitted an unnecessary state-change
		// window in which a concurrent publication could replace the root shape.
		MetadataSnapshotPin ownedMutationSnapshot;
		const MetadataSnapshotPin *activeMutationSnapshot = planningSnapshot;
		if (activeMutationSnapshot == nullptr) {
			ownedMutationSnapshot = metadata_.openSnapshot();
			activeMutationSnapshot = &ownedMutationSnapshot;
		}
		const MetadataSnapshotPin &mutationSnapshot = *activeMutationSnapshot;
		const IndexRootLocation mutationRoot = metadata_.findIndexRoot(mutationSnapshot, indexId);
		const BTreePageHeader mutationRootHeader = read(mutationSnapshot, mutationRoot.root_page_id).header();
		if (mutationRootHeader.kind == BTreeNodeKind::BPN_LEAF) {
			if (planningSnapshot == nullptr) {
				ownedMutationSnapshot.release();
			}
			if (planStructuralOnly) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable deep structural B+Tree plan requires an internal root");
			}
			return upsertRootLeafImpl(indexId, key, value, growth, checkpointLsn, options);
		}
		PageId currentPageId = mutationRoot.root_page_id;
		uint16_t expectedLevel = mutationRootHeader.level;
		std::vector<BTreePathNode> path;
		path.reserve(static_cast<size_t>(expectedLevel) + 1U);
		while (true) {
			BTreePage current = read(mutationSnapshot, currentPageId);
			const BTreePageHeader currentHeader = current.header();
			if (currentHeader.index_id != indexId) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable B+Tree deep upsert encountered a foreign Index page");
			}
			path.push_back({currentHeader, metadata_.findPageLocation(mutationSnapshot, currentPageId),
							current.slottedPage().ownerCommittedLsn(), current.entries(), 0});
			if (currentHeader.kind == BTreeNodeKind::BPN_LEAF) {
				// ZYX_COV_EXCL_START: BTreePage::open admits leaf pages only at level zero.
				if (expectedLevel != 0U) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "relocatable B+Tree deep upsert encountered a leaf above level zero");
				}
				// ZYX_COV_EXCL_STOP
				const BTreePathNode &leaf = path.back();
				std::vector<BTreePageEntry> entries = leaf.entries;
				bool replaced = false;
				for (BTreePageEntry &entry: entries) {
					if (std::ranges::equal(entry.key, key)) {
						entry.value_or_child = value;
						replaced = true;
					}
				}
				if (!replaced) {
					entries.push_back({std::vector<std::byte>(key.begin(), key.end()), value});
					std::ranges::sort(entries, {}, &BTreePageEntry::key);
				}
				std::optional<BTreePage> replacement;
				if (replaced) {
					replacement.emplace(makeLeafPage(
							leaf.header,
							indexPageOwner(indexId, leaf.header.page_id, leaf.header.generation, leaf.owner_lsn),
							leaf.header.left_sibling, leaf.header.right_sibling, entries));
					if (planningSnapshot == nullptr) {
						ownedMutationSnapshot.release();
					}
					if (planStructuralOnly) {
						throw FormatException(FormatError::FERR_INVALID_STATE,
											  "relocatable deep structural B+Tree plan requires a leaf split");
					}
					return replace(*replacement, checkpointLsn, options);
				}
				try {
					replacement.emplace(makeLeafPage(
							leaf.header,
							indexPageOwner(indexId, leaf.header.page_id, leaf.header.generation, leaf.owner_lsn),
							leaf.header.left_sibling, leaf.header.right_sibling, entries));
				} catch (const FormatException &error) {
					// ZYX_COV_EXCL_START: these candidates are reconstructed from validated leaf records.
					if (error.error() != FormatError::FERR_INVALID_STATE) {
						throw;
					}
					// ZYX_COV_EXCL_STOP
					const std::optional<size_t> splitIndex =
							findBalancedLeafSplitIndex(leaf.header, indexId, leaf.owner_lsn, checkpointLsn,
													   leaf.header.left_sibling, leaf.header.right_sibling, entries);
					if (!splitIndex.has_value()) {
						throw FormatException(FormatError::FERR_INVALID_STATE,
											  "relocatable B+Tree deep leaf split cannot fit the requested key");
					}

					// read() has already validated each path member against the same pinned
					// Page Directory and shared-pool owner. The publication coordinator
					// revalidates these immutable locations under its writer guard.

					std::vector<BTreePageEntry> leftEntries(entries.begin(), entries.begin() + *splitIndex);
					std::vector<BTreePageEntry> rightEntries(entries.begin() + *splitIndex, entries.end());
					std::vector<PageLocation> expectedReplacements;
					std::vector<PlannedBTreePage> replacementPlans;
					std::vector<PlannedBTreePage> appendedPlans;
					std::set<PageId> replacementIds;
					const auto addReplacement = [&](const BTreePathNode &node, PlannedBTreePage plan) {
						// ZYX_COV_EXCL_START: the local plan carries locations loaded from its pinned Page Directory
						// path.
						if (node.location.page_id != node.header.page_id || node.location.page_index == 0U) {
							throw FormatException(FormatError::FERR_INVALID_STATE,
												  "relocatable B+Tree deep split has an invalid replacement location");
						}
						// ZYX_COV_EXCL_STOP
						// ZYX_COV_EXCL_START: each path page can be planned for replacement once.
						if (!replacementIds.insert(node.header.page_id).second) {
							throw FormatException(
									FormatError::FERR_INVALID_STATE,
									"relocatable B+Tree deep split planned one replacement Page ID twice");
						}
						// ZYX_COV_EXCL_STOP
						expectedReplacements.push_back(node.location);
						replacementPlans.push_back(std::move(plan));
					};
					const auto addAppended = [&](PlannedBTreePage plan) {
						const PlannedPageReference reference = PlannedPageReference::appended(appendedPlans.size());
						plan.page_ref = reference;
						appendedPlans.push_back(std::move(plan));
						return reference;
					};
					const PlannedPageReference leafReference = PlannedPageReference::existing(leaf.header.page_id);
					PlannedBTreePage rightLeafPlan;
					rightLeafPlan.source_header = leaf.header;
					rightLeafPlan.expected_owner_lsn = checkpointLsn;
					rightLeafPlan.left_sibling = leafReference;
					rightLeafPlan.right_sibling = {leaf.header.right_sibling, std::nullopt};
					rightLeafPlan.leaf_entries = rightEntries;
					const PlannedPageReference rightLeafReference = addAppended(std::move(rightLeafPlan));

					PlannedBTreePage leftLeafPlan;
					leftLeafPlan.source_header = leaf.header;
					leftLeafPlan.page_ref = leafReference;
					leftLeafPlan.expected_owner_lsn = leaf.owner_lsn;
					leftLeafPlan.left_sibling = {leaf.header.left_sibling, std::nullopt};
					leftLeafPlan.right_sibling = rightLeafReference;
					leftLeafPlan.leaf_entries = std::move(leftEntries);
					addReplacement(leaf, std::move(leftLeafPlan));

					if (leaf.header.right_sibling.isValid()) {
						BTreePage sibling = read(mutationSnapshot, leaf.header.right_sibling);
						const BTreePageHeader siblingHeader = sibling.header();
						const PageLocation siblingLocation =
								metadata_.findPageLocation(mutationSnapshot, leaf.header.right_sibling);
						if (siblingHeader.kind != BTreeNodeKind::BPN_LEAF || siblingHeader.index_id != indexId ||
							siblingHeader.left_sibling != leaf.header.page_id) {
							throw FormatException(FormatError::FERR_INVALID_STATE,
												  "relocatable B+Tree deep leaf split found an invalid right sibling");
						}
						BTreePathNode siblingNode{siblingHeader, siblingLocation,
												  sibling.slottedPage().ownerCommittedLsn(), sibling.entries(), 0};
						PlannedBTreePage siblingPlan;
						siblingPlan.source_header = siblingHeader;
						siblingPlan.page_ref = PlannedPageReference::existing(siblingHeader.page_id);
						siblingPlan.expected_owner_lsn = siblingNode.owner_lsn;
						siblingPlan.left_sibling = rightLeafReference;
						siblingPlan.right_sibling = {siblingHeader.right_sibling, std::nullopt};
						siblingPlan.leaf_entries = std::move(siblingNode.entries);
						addReplacement(siblingNode, std::move(siblingPlan));
					}

					std::vector<std::byte> promotedKey = rightEntries.front().key;
					PlannedPageReference promotedRightChild = rightLeafReference;
					std::optional<size_t> replacementRootOrdinal;
					bool insertedIntoAncestor = false;
					for (size_t pathIndex = path.size() - 1U; pathIndex-- > 0U;) {
						const BTreePathNode &parent = path[pathIndex];
						// The pinned descent records child_ordinal while selecting this
						// parent's existing child, so it is within the decoded entry range.
						const PlannedPageReference parentReference =
								PlannedPageReference::existing(parent.header.page_id);
						const PlannedPageReference parentLeftmost =
								PlannedPageReference::existing(parent.header.leftmost_child);
						std::vector<PlannedInternalEntry> parentEntries = planInternalEntries(parent.entries);
						parentEntries.insert(parentEntries.begin() + parent.child_ordinal,
											 {promotedKey, promotedRightChild});
						const BTreePageHeader previewHeader = previewInternalHeader(parent.header, parentLeftmost);
						const std::vector<BTreePageEntry> previewEntries = previewInternalEntries(parentEntries);
						try {
							static_cast<void>(makeInternalPage(
									previewHeader,
									indexPageOwner(indexId, PageId(1), parent.header.generation, parent.owner_lsn),
									previewEntries));
							PlannedBTreePage parentPlan;
							parentPlan.source_header = parent.header;
							parentPlan.page_ref = parentReference;
							parentPlan.expected_owner_lsn = parent.owner_lsn;
							parentPlan.leftmost_child = parentLeftmost;
							parentPlan.internal_entries = std::move(parentEntries);
							addReplacement(parent, std::move(parentPlan));
							insertedIntoAncestor = true;
							break;
						} catch (const FormatException &parentError) {
							// ZYX_COV_EXCL_START: this preview contains only codec-validated internal entries.
							if (parentError.error() != FormatError::FERR_INVALID_STATE) {
								throw;
							}
							// ZYX_COV_EXCL_STOP
						}

						const std::optional<size_t> parentSplitIndex = findBalancedInternalSplitIndex(
								previewHeader, indexId, parent.owner_lsn, checkpointLsn, previewEntries);
						if (!parentSplitIndex.has_value()) {
							throw FormatException(FormatError::FERR_INVALID_STATE,
												  "relocatable B+Tree deep split cannot fit promoted internal pages");
						}
						std::vector<PlannedInternalEntry> leftInternalEntries(
								parentEntries.begin(), parentEntries.begin() + *parentSplitIndex);
						std::vector<PlannedInternalEntry> rightInternalEntries(
								parentEntries.begin() + *parentSplitIndex + 1U, parentEntries.end());
						const std::vector<std::byte> nextPromotedKey = parentEntries[*parentSplitIndex].key;
						const PlannedPageReference rightInternalLeftmost = parentEntries[*parentSplitIndex].right_child;
						PlannedBTreePage rightInternalPlan;
						rightInternalPlan.source_header = parent.header;
						rightInternalPlan.source_header.left_sibling = {};
						rightInternalPlan.source_header.right_sibling = {};
						rightInternalPlan.expected_owner_lsn = checkpointLsn;
						rightInternalPlan.leftmost_child = rightInternalLeftmost;
						rightInternalPlan.internal_entries = std::move(rightInternalEntries);
						const PlannedPageReference rightInternalReference = addAppended(std::move(rightInternalPlan));

						PlannedBTreePage leftInternalPlan;
						leftInternalPlan.source_header = parent.header;
						leftInternalPlan.page_ref = parentReference;
						leftInternalPlan.expected_owner_lsn = parent.owner_lsn;
						leftInternalPlan.leftmost_child = parentLeftmost;
						leftInternalPlan.internal_entries = std::move(leftInternalEntries);
						addReplacement(parent, std::move(leftInternalPlan));
						promotedKey = nextPromotedKey;
						promotedRightChild = rightInternalReference;
					}

					if (!insertedIntoAncestor) {
						const BTreePathNode &oldRoot = path.front();
						// ZYX_COV_EXCL_START: the persisted address space cannot materialize a tree at this height.
						if (oldRoot.header.level == std::numeric_limits<uint16_t>::max()) {
							throw FormatException(
									FormatError::FERR_ARITHMETIC_OVERFLOW,
									"relocatable B+Tree root level cannot grow beyond the persisted representation");
						}
						// ZYX_COV_EXCL_STOP
						BTreePageHeader newRootHeader;
						newRootHeader.index_id = indexId;
						newRootHeader.generation = leaf.header.generation;
						newRootHeader.kind = BTreeNodeKind::BPN_INTERNAL;
						newRootHeader.level = static_cast<uint16_t>(oldRoot.header.level + 1U);
						newRootHeader.leftmost_child = oldRoot.header.page_id;
						PlannedBTreePage newRootPlan;
						newRootPlan.source_header = newRootHeader;
						newRootPlan.expected_owner_lsn = checkpointLsn;
						newRootPlan.leftmost_child = PlannedPageReference::existing(oldRoot.header.page_id);
						newRootPlan.internal_entries.push_back({std::move(promotedKey), promotedRightChild});
						const PlannedPageReference newRootReference = addAppended(std::move(newRootPlan));
						// The metadata coordinator indexes the concatenated replacement and
						// append vectors, not the append vector alone.
						replacementRootOrdinal = expectedReplacements.size() + *newRootReference.appended_ordinal;
					}

					const size_t appendedPageCount = appendedPlans.size();
					std::vector<IndexTreeMutationPoolRequest> requests;
					std::vector<std::vector<size_t>> replacementPlanIndexes;
					std::map<uint64_t, size_t> requestIndexes;
					const auto ensureRequest = [&](SegmentId segmentId) {
						const auto [iterator, inserted] = requestIndexes.emplace(segmentId.value(), requests.size());
						if (inserted) {
							requests.push_back({segmentId, {}, 0, {}});
							replacementPlanIndexes.emplace_back();
						}
						return iterator->second;
					};
					for (size_t index = 0; index < expectedReplacements.size(); ++index) {
						const size_t requestIndex = ensureRequest(expectedReplacements[index].segment_id);
						requests[requestIndex].expected_replacements.push_back(expectedReplacements[index]);
						replacementPlanIndexes[requestIndex].push_back(index);
					}
					std::optional<BTreeNewIndexTreePoolMutation> newPool;
					size_t appendPoolOrdinal = 0;
					if (const std::optional<SegmentId> selectedPool =
								selectAppendPool(mutationSnapshot, expectedReplacements, appendedPageCount);
						selectedPool.has_value()) {
						appendPoolOrdinal = ensureRequest(*selectedPool);
						requests[appendPoolOrdinal].appended_page_count = appendedPageCount;
					} else {
						if (!growth.has_value() && !planStructuralOnly) {
							throw FormatException(
									FormatError::FERR_INVALID_STATE,
									"relocatable B+Tree mutation needs an index-pool growth range when all "
									"current pools are full");
						}
						appendPoolOrdinal = requests.size();
						newPool = {growth.has_value() ? growth->pool_owner_id : indexId, appendedPageCount};
					}
					std::optional<IndexTreeMutationAppendedPageReference> replacementRoot;
					if (replacementRootOrdinal.has_value()) {
						// ZYX_COV_EXCL_START: appended root ordinals are derived from the append-only tail of this
						// plan.
						if (*replacementRootOrdinal < expectedReplacements.size()) {
							throw FormatException(
									FormatError::FERR_INVALID_STATE,
									"relocatable B+Tree deep split planned a root outside its append batch");
						}
						// ZYX_COV_EXCL_STOP
						replacementRoot = {appendPoolOrdinal, *replacementRootOrdinal - expectedReplacements.size()};
					}
					const BTreeStructuralMutationPreparer preparer = [this, indexId,
																	  replacementPlans = std::move(replacementPlans),
																	  appendedPlans = std::move(appendedPlans),
																	  replacementPlanIndexes =
																			  std::move(replacementPlanIndexes),
																	  appendPoolOrdinal,
																	  hasNewPool = newPool.has_value()](
																			 std::vector<IndexTreeMutationPoolBatch>
																					 &batches,
																			 LogSequenceNumber checkpointLsn) {
						// ZYX_COV_EXCL_START: the private Metadata callback derives batches from this exact request
						// vector.
						if (batches.size() != replacementPlanIndexes.size() + (hasNewPool ? 1U : 0U) ||
							appendPoolOrdinal >= batches.size()) {
							throw FormatException(
									FormatError::FERR_INVALID_STATE,
									"relocatable B+Tree multi-pool mutation received an unexpected pool batch");
						}
						// ZYX_COV_EXCL_STOP
						const std::vector<PageId> &appendedPageIds = batches[appendPoolOrdinal].appended_page_ids;
						// ZYX_COV_EXCL_START: Metadata reserves exactly the append count requested by this mutation
						// plan.
						if (appendedPageIds.size() != appendedPlans.size()) {
							throw FormatException(
									FormatError::FERR_INVALID_STATE,
									"relocatable B+Tree multi-pool mutation assigned an unexpected append batch");
						}
						// ZYX_COV_EXCL_STOP
						for (size_t poolIndex = 0; poolIndex < batches.size(); ++poolIndex) {
							IndexTreeMutationPoolBatch &batch = batches[poolIndex];
							const std::vector<size_t> emptyPlanIndexes;
							const std::vector<size_t> &planIndexes = poolIndex < replacementPlanIndexes.size()
																			 ? replacementPlanIndexes[poolIndex]
																			 : emptyPlanIndexes;
							// ZYX_COV_EXCL_START: request grouping and callback batches share the same immutable plan.
							if (batch.expected_replacements.size() != planIndexes.size()) {
								throw FormatException(
										FormatError::FERR_INVALID_STATE,
										"relocatable B+Tree multi-pool mutation replacement batch changed "
										"before preparation");
							}
							// ZYX_COV_EXCL_STOP
							std::vector<BTreePage> materialized;
							materialized.reserve(planIndexes.size() +
												 (poolIndex == appendPoolOrdinal ? appendedPlans.size() : 0U));
							for (size_t index = 0; index < planIndexes.size(); ++index) {
								const PlannedBTreePage &plan = replacementPlans[planIndexes[index]];
								// ZYX_COV_EXCL_START: Metadata preserves every requested replacement identity while
								// invoking the private callback.
								if (batch.expected_replacements[index].page_id !=
									plan.page_ref.resolve(appendedPageIds)) {
									throw FormatException(FormatError::FERR_INVALID_STATE,
														  "relocatable B+Tree multi-pool replacement Page ID changed "
														  "before publication");
								}
								// ZYX_COV_EXCL_STOP
								materialized.push_back(materializePlannedPage(indexId, batch.pool, plan,
																			  appendedPageIds, checkpointLsn));
							}
							if (poolIndex == appendPoolOrdinal) {
								for (const PlannedBTreePage &plan: appendedPlans) {
									materialized.push_back(materializePlannedPage(indexId, batch.pool, plan,
																				  appendedPageIds, checkpointLsn));
								}
							}
							std::vector<SharedPagePoolPreparedReplacement> preparedReplacements;
							std::vector<SharedPagePoolPreparedAppend> preparedAppends;
							preparedReplacements.reserve(planIndexes.size());
							preparedAppends.reserve(poolIndex == appendPoolOrdinal ? appendedPlans.size() : 0U);
							for (size_t index = 0; index < materialized.size(); ++index) {
								const bool isReplacement = index < planIndexes.size();
								const PageId expectedPageId = isReplacement
																	  ? batch.expected_replacements[index].page_id
																	  : appendedPageIds[index - planIndexes.size()];
								const BTreePage &page = materialized[index];
								const BTreePageHeader header = page.header();
								const uint8_t invalidPage =
										static_cast<uint8_t>(header.index_id != indexId) |
										static_cast<uint8_t>(header.page_id != expectedPageId) |
										static_cast<uint8_t>(header.generation != batch.pool.generation) |
										static_cast<uint8_t>(page.slottedPage().pageId() != expectedPageId) |
										static_cast<uint8_t>(page.slottedPage().pageGeneration() !=
															 batch.pool.generation) |
										static_cast<uint8_t>(page.slottedPage().ownerId() != indexId) |
										static_cast<uint8_t>(page.slottedPage().ownerKind() !=
															 PageOwnerKind::POK_INDEX) |
										static_cast<uint8_t>(!page.slottedPage().ownerCommittedLsn().isValid());
								// ZYX_COV_EXCL_START: pages are materialized solely from this plan and
								// Metadata-assigned Page IDs.
								if (invalidPage != 0U) {
									throw FormatException(
											FormatError::FERR_INVALID_STATE,
											"relocatable B+Tree multi-pool factory returned a page with the "
											"wrong durable identity");
								}
								// ZYX_COV_EXCL_STOP
								std::vector<std::byte> bytes = page.serialize();
								PageOwnerHeader owner =
										indexPageOwner(indexId, expectedPageId, batch.pool.generation, checkpointLsn);
								owner.payload_bytes = static_cast<uint32_t>(bytes.size());
								if (isReplacement) {
									preparedReplacements.push_back(
											{std::move(owner), page.slottedPage().ownerCommittedLsn(), std::move(bytes),
											 page.slottedPage().stats().contiguous_free_bytes});
								} else {
									// ZYX_COV_EXCL_START: appended owners are constructed with this callback's
									// checkpoint LSN.
									if (page.slottedPage().ownerCommittedLsn() != checkpointLsn) {
										throw FormatException(
												FormatError::FERR_INVALID_STATE,
												"relocatable appended multi-pool B+Tree page does not use the "
												"publication LSN");
									}
									// ZYX_COV_EXCL_STOP
									preparedAppends.push_back({std::move(owner), std::move(bytes),
															   page.slottedPage().stats().contiguous_free_bytes});
								}
							}
							if (batch.newly_reserved) {
								pages_.prepareNewPoolPages(batch.pool, preparedAppends, batch.appended_locations,
														   checkpointLsn, batch.prepared_new_pool_prefix_count);
							} else {
								pages_.prepareMutationPages(
										batch.pool, batch.expected_replacements, preparedReplacements, preparedAppends,
										batch.replacement_locations, batch.appended_locations, checkpointLsn);
							}
						}
					};
					const BTreeStructuralMutationPlan mutationPlan{
							mutationRoot,	 std::move(requests), std::move(newPool), {},
							replacementRoot, std::nullopt,		  std::move(preparer)};
					if (planStructuralOnly) {
						return mutationPlan;
					}
					if (planningSnapshot == nullptr) {
						ownedMutationSnapshot.release();
					}
					for (const IndexTreeMutationPoolRequest &request: mutationPlan.pool_requests) {
						pages_.reconcilePool(request.segment_id);
					}
					std::optional<NewIndexTreePoolRequest> newPoolRequest;
					if (mutationPlan.new_pool_mutation.has_value()) {
						newPoolRequest = BTreePageStore::newPoolRequest(
								*growth, indexId, mutationPlan.new_pool_mutation->appended_page_count);
						newPoolRequest->owner_id = mutationPlan.new_pool_mutation->owner_id;
					}
					const Superblock published = metadata_.mutateIndexTreePagesAcrossPools(
							mutationPlan.pool_requests, newPoolRequest, mutationPlan.expected_root,
							mutationPlan.replacement_root, mutationPlan.stable_replacement_root, checkpointLsn,
							[&mutationPlan, checkpointLsn](std::vector<IndexTreeMutationPoolBatch> &batches) {
								mutationPlan.preparer(batches, checkpointLsn);
							},
							options);
					MetadataSnapshotPin publishedSnapshot = metadata_.openSnapshot();
					const IndexRootLocation publishedRoot = metadata_.findIndexRoot(publishedSnapshot, indexId);
					const PageLocation publishedRootLocation =
							metadata_.findPageLocation(publishedSnapshot, publishedRoot.root_page_id);
					const CatalogEntry publishedRootPool =
							metadata_.findCatalog(publishedSnapshot, publishedRootLocation.segment_id);
					const BTreePage publishedRootPage = read(publishedSnapshot, publishedRoot.root_page_id);
					PageOwnerHeader owner;
					owner.page_id = publishedRootPage.slottedPage().pageId();
					owner.owner_id = publishedRootPage.slottedPage().ownerId();
					owner.kind = publishedRootPage.slottedPage().ownerKind();
					owner.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
					owner.generation = publishedRootPage.slottedPage().pageGeneration();
					owner.committed_lsn = publishedRootPage.slottedPage().ownerCommittedLsn();
					publishedSnapshot.release();
					return SharedPagePoolAllocation{published, publishedRootPool, publishedRootLocation, owner};
				}
				ownedMutationSnapshot.release();
				return replace(*replacement, checkpointLsn, options);
			}
			// ZYX_COV_EXCL_START: the leaf branch above handles the only other codec-valid node kind.
			if (currentHeader.kind != BTreeNodeKind::BPN_INTERNAL || !currentHeader.leftmost_child.isValid()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable B+Tree deep upsert encountered an invalid internal page layout");
			}
			// ZYX_COV_EXCL_STOP
			if (currentHeader.level != expectedLevel) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable B+Tree deep upsert encountered an invalid internal path page");
			}
			PageId nextPageId = currentHeader.leftmost_child;
			for (size_t index = 0; index < path.back().entries.size(); ++index) {
				const BTreePageEntry &entry = path.back().entries[index];
				if (lessKey(key, entry.key)) {
					break;
				}
				nextPageId = PageId(entry.value_or_child);
				path.back().child_ordinal = index + 1U;
			}
			// ZYX_COV_EXCL_START: BTreePage::open rejects an internal page with an invalid leftmost or entry child.
			if (!nextPageId.isValid()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable B+Tree deep upsert selected an invalid child Page ID");
			}
			// ZYX_COV_EXCL_STOP
			currentPageId = nextPageId;
			--expectedLevel;
		}
	}

	bool BTreePageStore::erase(uint64_t indexId, std::span<const std::byte> key, LogSequenceNumber checkpointLsn,
							   const MetadataPublishOptions &options) {
		return eraseImpl(indexId, key, checkpointLsn, options);
	}

	BTreeStructuralMutationPlan
	BTreePageStore::planScalarIndexTransactionMutation(const MetadataSnapshotPin &snapshot, uint64_t indexId,
																				   std::span<const BTreeScalarIndexOperation> operations,
																				   bool rebuildOnFrameExhaustion, bool forceFreshRebuild,
																				   BTreeLeafValueLayout leafValueLayout) const {
		if (!snapshot.isValid() || indexId == 0U || operations.empty() ||
			(leafValueLayout != BTreeLeafValueLayout::BLVL_U64 && leafValueLayout != BTreeLeafValueLayout::BLVL_EMPTY)) {
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"relocatable scalar Index transaction overlay requires a live snapshot, Index ID, and operations");
		}
		auto overlay = std::make_shared<TransactionBTreeOverlay>(*this, metadata_, snapshot, indexId,
																				 rebuildOnFrameExhaustion, forceFreshRebuild, leafValueLayout);
		for (const BTreeScalarIndexOperation &operation: operations) {
			overlay->apply(operation);
		}
		return overlay->buildPlan();
	}

	BTreeTreeRetirementPlan BTreePageStore::planTreeRetirement(const MetadataSnapshotPin &snapshot, uint64_t indexId,
															   size_t maximumPages) const {
		if (!snapshot.isValid() || indexId == 0U || maximumPages == 0U) {
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"relocatable B+Tree retirement plan requires a live snapshot, Index ID, and page bound");
		}
		const IndexRootLocation root = metadata_.findIndexRoot(snapshot, indexId);
		std::vector<PageId> pending{root.root_page_id};
		std::set<PageId> visited;
		std::map<uint64_t, IndexTreeMutationPoolRequest> requests;
		while (!pending.empty()) {
			const PageId pageId = pending.back();
			pending.pop_back();
			if (visited.size() == maximumPages || !visited.insert(pageId).second) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable B+Tree retirement plan found an oversized or cyclic tree");
			}
			const PageLocation location = metadata_.findPageLocation(snapshot, pageId);
			const CatalogEntry pool = metadata_.findCatalog(snapshot, location.segment_id);
			const BTreePage page = read(snapshot, pageId);
			const BTreePageHeader header = page.header();
			const uint8_t invalidPage =
					static_cast<uint8_t>(pool.kind != SegmentKind::SGK_INDEX_PAGE_POOL) |
					// Index pools are shared allocation containers. The page/header
					// owner carries the tree's IndexId, while the pool owner identifies
					// only the allocation namespace for all pages in that pool.
					static_cast<uint8_t>(pool.owner_id == 0U) |
					static_cast<uint8_t>(pool.generation != location.generation) |
					static_cast<uint8_t>(header.index_id != indexId) | static_cast<uint8_t>(header.page_id != pageId) |
					static_cast<uint8_t>(header.generation != location.generation) |
					static_cast<uint8_t>(static_cast<uint8_t>(pageId == root.root_page_id) &
											 static_cast<uint8_t>(header.generation != root.root_generation)) |
					static_cast<uint8_t>(pageId == root.root_page_id &&
													 (header.left_sibling.isValid() || header.right_sibling.isValid())) |
					static_cast<uint8_t>(!page.slottedPage().ownerCommittedLsn().isValid());
			if (invalidPage != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable B+Tree retirement plan found a mismatched owned page");
			}
			IndexTreeMutationPoolRequest &request = requests[location.segment_id.value()];
			request.segment_id = location.segment_id;
			request.expected_retirements.push_back({location, page.slottedPage().ownerCommittedLsn()});
			if (header.kind == BTreeNodeKind::BPN_INTERNAL) {
				if (!header.leftmost_child.isValid()) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable B+Tree retirement plan found an internal page without a left child");
				}
				pending.push_back(header.leftmost_child);
				for (const BTreePageEntry &entry: page.entries()) {
					const PageId child(entry.value_or_child);
					if (!child.isValid()) {
						throw FormatException(FormatError::FERR_INVALID_STATE,
											  "relocatable B+Tree retirement plan found an invalid internal child");
					}
					pending.push_back(child);
				}
			}
		}

		BTreeTreeRetirementPlan result;
		result.expected_root = root;
		result.pool_requests.reserve(requests.size());
		for (auto &[segmentId, request]: requests) {
			static_cast<void>(segmentId);
			std::ranges::sort(request.expected_retirements, {}, [](const IndexTreePageRetirement &retirement) {
				return retirement.location.page_id.value();
			});
			result.pool_requests.push_back(std::move(request));
		}
		result.preparer = [this, indexId, expectedRequests = result.pool_requests](
								  std::vector<IndexTreeMutationPoolBatch> &batches, LogSequenceNumber checkpointLsn) {
			if (batches.size() != expectedRequests.size()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable B+Tree retirement publication changed its pool cardinality");
			}
			for (size_t ordinal = 0U; ordinal < batches.size(); ++ordinal) {
				const IndexTreeMutationPoolRequest &expected = expectedRequests[ordinal];
				IndexTreeMutationPoolBatch &batch = batches[ordinal];
				const bool invalidBatch = batch.pool.segment_id != expected.segment_id ||
										  !batch.expected_replacements.empty() ||
										  batch.appended_page_ids.size() != 0U || batch.newly_reserved ||
										  batch.expected_retirements != expected.expected_retirements;
				if (invalidBatch) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "relocatable B+Tree retirement publication changed its immutable page plan");
				}
				std::vector<SharedPagePoolPreparedRetirement> prepared;
				prepared.reserve(batch.expected_retirements.size());
				for (const IndexTreePageRetirement &retirement: batch.expected_retirements) {
					prepared.push_back(
							{retirement.location, indexId, PageOwnerKind::POK_INDEX, retirement.expected_owner_lsn});
				}
				const std::array<PageLocation, 0U> noReplacements{};
				const std::array<SharedPagePoolPreparedReplacement, 0U> noReplacementPayloads{};
				const std::array<SharedPagePoolPreparedAppend, 0U> noAppends{};
				pages_.prepareMutationPages(batch.pool, noReplacements, noReplacementPayloads, noAppends,
											batch.replacement_locations, batch.appended_locations, checkpointLsn,
											prepared);
			}
		};
		return result;
	}

	BTreeInitialTreeMutationPlan
	BTreePageStore::planInitialTreeMutation(const MetadataSnapshotPin &snapshot, uint64_t indexId,
											std::span<const BTreePageEntry> entries,
											BTreeInitialTreeValueBinder valueBinder,
											BTreeLeafValueLayout leafValueLayout) const {
		return planFreshTreeMutation(snapshot, indexId, entries, std::move(valueBinder),
									 BTreeFreshTreeRootMode::BFTM_REQUIRE_ABSENT, leafValueLayout);
	}

	BTreeInitialTreeMutationPlan
	BTreePageStore::planReplacementTreeMutation(const MetadataSnapshotPin &snapshot, uint64_t indexId,
												std::span<const BTreePageEntry> entries,
												BTreeInitialTreeValueBinder valueBinder,
												BTreeLeafValueLayout leafValueLayout) const {
		return planFreshTreeMutation(snapshot, indexId, entries, std::move(valueBinder),
									 BTreeFreshTreeRootMode::BFTM_REQUIRE_PRESENT, leafValueLayout);
	}

	BTreeInitialTreeMutationPlan BTreePageStore::planFreshTreeMutation(const MetadataSnapshotPin &snapshot,
																									uint64_t indexId,
																									std::span<const BTreePageEntry> entries,
																									BTreeInitialTreeValueBinder valueBinder,
																									BTreeFreshTreeRootMode rootMode,
																									BTreeLeafValueLayout leafValueLayout) const {
		const uint8_t invalidInput = static_cast<uint8_t>(!snapshot.isValid()) | static_cast<uint8_t>(indexId == 0U) |
									 static_cast<uint8_t>(leafValueLayout != BTreeLeafValueLayout::BLVL_U64 &&
															  leafValueLayout != BTreeLeafValueLayout::BLVL_EMPTY);
		if (invalidInput != 0U) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable fresh B+Tree plan requires a live snapshot, Index ID, and root mode");
		}
		const bool hasCurrentRoot = metadata_.findIndexRootOptional(snapshot, indexId).has_value();
		if ((rootMode == BTreeFreshTreeRootMode::BFTM_REQUIRE_ABSENT && hasCurrentRoot) ||
			(rootMode == BTreeFreshTreeRootMode::BFTM_REQUIRE_PRESENT && !hasCurrentRoot)) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable fresh B+Tree plan has an incompatible visible Index root state");
		}
		size_t stagedEntryBytes = 0U;
		for (const BTreePageEntry &entry: entries) {
			if (entry.key.size() > RSS_INITIAL_TREE_MAX_STAGED_ENTRY_BYTES - RSS_BTREE_ENTRY_HEADER_BYTES ||
				stagedEntryBytes >
						RSS_INITIAL_TREE_MAX_STAGED_ENTRY_BYTES - RSS_BTREE_ENTRY_HEADER_BYTES - entry.key.size()) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "relocatable initial B+Tree plan exceeds its bounded transaction-local input");
			}
			stagedEntryBytes += RSS_BTREE_ENTRY_HEADER_BYTES + entry.key.size();
		}

		const std::vector<BTreePageEntry> sourceEntries(entries.begin(), entries.end());
		const bool keyOnlyTree = leafValueLayout == BTreeLeafValueLayout::BLVL_EMPTY;
		if ((keyOnlyTree && (valueBinder || std::ranges::any_of(sourceEntries, [](const BTreePageEntry &entry) {
				 return entry.value_or_child != 0U;
			 }))) ||
			(!keyOnlyTree && !valueBinder && std::ranges::any_of(sourceEntries, [](const BTreePageEntry &entry) {
				return entry.value_or_child == 0U;
			}))) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable initial B+Tree leaf values disagree with its selected layout");
		}
		std::vector<BTreePageEntry> planningEntries = sourceEntries;
		if (valueBinder) {
			for (BTreePageEntry &entry: planningEntries) {
				// B+Tree values have a fixed encoded width, so a nonzero probe value
				// plans the exact same physical layout as the later bound PageId.
				if (entry.value_or_child == 0U) {
					entry.value_or_child = 1U;
				}
			}
		}
		const bool emptyTree = planningEntries.empty();
		const InitialBTreePlan initialPlan =
				emptyTree ? InitialBTreePlan{} : InitialBTreeBuilder::plan(indexId, planningEntries, leafValueLayout);
		const size_t pageCount = emptyTree ? 1U : initialPlan.nodes.size();
		BTreeInitialTreeMutationPlan result;
		result.index_id = indexId;
		for (size_t firstPage = 0U; firstPage < pageCount;) {
			const size_t count = std::min(static_cast<size_t>(RSS_PAGE_POOL_DATA_FRAME_COUNT), pageCount - firstPage);
			result.new_pool_mutations.push_back({indexId, count});
			firstPage += count;
		}
		const size_t rootOrdinal = emptyTree ? 0U : initialPlan.root_ordinal;
		result.root_page = {rootOrdinal / RSS_PAGE_POOL_DATA_FRAME_COUNT, rootOrdinal % RSS_PAGE_POOL_DATA_FRAME_COUNT};
		result.preparer = [this, indexId, emptyTree, initialPlan, pageCount, sourceEntries, leafValueLayout,
						   valueBinder = std::move(valueBinder), pools = result.new_pool_mutations](
								  std::vector<IndexTreeMutationPoolBatch> &batches, LogSequenceNumber checkpointLsn) {
			if (!checkpointLsn.isValid() || batches.size() != pools.size()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable initial B+Tree preparation changed its pool plan or checkpoint");
			}
			std::vector<PageId> pageIds;
			std::vector<Generation> generations;
			pageIds.reserve(pageCount);
			generations.reserve(pageCount);
			for (size_t ordinal = 0U; ordinal < batches.size(); ++ordinal) {
				const IndexTreeMutationPoolBatch &batch = batches[ordinal];
				const BTreeNewIndexTreePoolMutation &expected = pools[ordinal];
				const uint8_t invalidBatch =
						static_cast<uint8_t>(!batch.newly_reserved) |
						static_cast<uint8_t>(batch.pool.kind != SegmentKind::SGK_INDEX_PAGE_POOL) |
						static_cast<uint8_t>(batch.pool.owner_id != indexId) |
						static_cast<uint8_t>(batch.pool.owner_id != expected.owner_id) |
						static_cast<uint8_t>(batch.pool.generation != Generation(1U)) |
						static_cast<uint8_t>(batch.pool.state != SegmentState::SGS_SEALED) |
						static_cast<uint8_t>(batch.pool.committed_lsn != checkpointLsn) |
						static_cast<uint8_t>(batch.appended_page_ids.size() != expected.appended_page_count) |
						static_cast<uint8_t>(batch.appended_locations.size() != expected.appended_page_count);
				if (invalidBatch != 0U) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "relocatable initial B+Tree preparation changed a fresh Index pool identity");
				}
				for (const PageId pageId: batch.appended_page_ids) {
					pageIds.push_back(pageId);
					generations.push_back(batch.pool.generation);
				}
			}
			// Each batch above is checked against its immutable requested count, so the
			// concatenated metadata identities exactly cover the precomputed page plan.
			std::vector<BTreePageEntry> resolvedEntries = sourceEntries;
			if (valueBinder) {
				const BTreeInitialTreeValueBindingContext context{batches, pageIds, generations};
				valueBinder(resolvedEntries, context);
			}
			for (size_t ordinal = 0U; ordinal < sourceEntries.size(); ++ordinal) {
				const bool invalidValue = leafValueLayout == BTreeLeafValueLayout::BLVL_U64
											  ? resolvedEntries[ordinal].value_or_child == 0U
											  : resolvedEntries[ordinal].value_or_child != 0U;
				if (resolvedEntries[ordinal].key != sourceEntries[ordinal].key || invalidValue) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable initial B+Tree value binder changed a key or left an unresolved value");
				}
			}
			// The callback may bind values only. The cardinality, key, and fixed-width
			// value checks above make the deterministic initial layout immutable.
			const InitialBTreePlan resolvedPlan =
					emptyTree ? InitialBTreePlan{}
							  : InitialBTreeBuilder::plan(indexId, resolvedEntries, leafValueLayout);

			std::vector<BTreePage> pages;
			if (emptyTree) {
				BTreePageHeader header;
				header.index_id = indexId;
				header.page_id = pageIds.front();
				header.generation = generations.front();
				header.leaf_value_layout = leafValueLayout;
				pages.push_back(BTreePage::create(
						indexPageOwner(indexId, header.page_id, header.generation, checkpointLsn), header));
			} else {
				pages = InitialBTreeBuilder::materialize(indexId, resolvedPlan, pageIds, generations, checkpointLsn);
			}
			// InitialBTreeBuilder materializes exactly one page per immutable plan node.
			for (size_t poolOrdinal = 0U, pageOffset = 0U; poolOrdinal < batches.size(); ++poolOrdinal) {
				IndexTreeMutationPoolBatch &batch = batches[poolOrdinal];
				const size_t count = batch.appended_page_ids.size();
				prepareNewPoolInitialPages(batch.pool, std::span<const BTreePage>(pages).subspan(pageOffset, count),
										   std::span<PageLocation>(batch.appended_locations), checkpointLsn);
				pageOffset += count;
			}
		};
		return result;
	}

	bool BTreePageStore::eraseImpl(uint64_t indexId, std::span<const std::byte> key, LogSequenceNumber checkpointLsn,
								   const MetadataPublishOptions &options) {
		if (indexId == 0U || !checkpointLsn.isValid()) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable B+Tree erase requires an Index ID and a checkpoint LSN");
		}
		MetadataSnapshotPin snapshot = metadata_.openSnapshot();
		const IndexRootLocation root = metadata_.findIndexRoot(snapshot, indexId);
		std::map<uint64_t, BTreeDeleteMutationNode> nodes;
		const auto loadNode = [&](PageId pageId, uint16_t expectedLevel) -> BTreeDeleteMutationNode & {
			const auto existing = nodes.find(pageId.value());
			if (existing != nodes.end()) {
				if (existing->second.header.level != expectedLevel) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "relocatable B+Tree erase revisited a page at an inconsistent tree level");
				}
				return existing->second;
			}
			BTreePage page = read(snapshot, pageId);
			const BTreePageHeader header = page.header();
			const PageLocation location = metadata_.findPageLocation(snapshot, pageId);
			const LogSequenceNumber ownerLsn = page.slottedPage().ownerCommittedLsn();
			const uint8_t invalid = static_cast<uint8_t>(header.index_id != indexId) |
									static_cast<uint8_t>(header.page_id != pageId) |
									static_cast<uint8_t>(header.generation != root.root_generation) |
									static_cast<uint8_t>(header.level != expectedLevel) |
									static_cast<uint8_t>(location.page_id != pageId) |
									static_cast<uint8_t>(location.generation != header.generation) |
									static_cast<uint8_t>(!ownerLsn.isValid());
			if (invalid != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable B+Tree erase encountered a foreign, stale, or malformed path page");
			}
			return nodes.emplace(pageId.value(), BTreeDeleteMutationNode{header, location, ownerLsn, page.entries()})
					.first->second;
		};
		const auto nodeFor = [&](PageId pageId) -> BTreeDeleteMutationNode & {
			const auto iterator = nodes.find(pageId.value());
			// ZYX_COV_EXCL_START: callers use Page IDs loaded into the immutable traversal path.
			if (iterator == nodes.end()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable B+Tree erase lost a loaded mutation page");
			}
			// ZYX_COV_EXCL_STOP
			return iterator->second;
		};
		const auto childAt = [](const BTreeDeleteMutationNode &parent, size_t ordinal) {
			return ordinal == 0U ? parent.header.leftmost_child : PageId(parent.entries[ordinal - 1U].value_or_child);
		};
		const auto childOrdinal = [&childAt](const BTreeDeleteMutationNode &parent, PageId childId) {
			for (size_t ordinal = 0; ordinal <= parent.entries.size(); ++ordinal) {
				if (childAt(parent, ordinal) == childId) {
					return ordinal;
				}
			}
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable B+Tree erase parent does not reference its selected child");
		};
		BTreePage initialRoot = read(snapshot, root.root_page_id);
		const BTreePageHeader initialRootHeader = initialRoot.header();
		const uint8_t invalidRoot = static_cast<uint8_t>(initialRootHeader.index_id != indexId) |
									static_cast<uint8_t>(initialRootHeader.page_id != root.root_page_id) |
									static_cast<uint8_t>(initialRootHeader.generation != root.root_generation) |
									static_cast<uint8_t>(initialRootHeader.left_sibling.isValid() ||
															  initialRootHeader.right_sibling.isValid());
		if (invalidRoot != 0U) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable B+Tree erase root does not match the Index Root Directory");
		}
		std::vector<BTreePathNode> path;
		uint16_t expectedLevel = initialRootHeader.level;
		PageId currentPageId = root.root_page_id;
		for (;;) {
			BTreeDeleteMutationNode &current = loadNode(currentPageId, expectedLevel);
			BTreePathNode pathNode{current.header, current.location, current.owner_lsn, current.entries, 0};
			if (current.header.kind == BTreeNodeKind::BPN_LEAF) {
				path.push_back(std::move(pathNode));
				break;
			}
			PageId next = current.header.leftmost_child;
			for (size_t index = 0; index < current.entries.size(); ++index) {
				if (lessKey(key, current.entries[index].key)) {
					break;
				}
				next = PageId(current.entries[index].value_or_child);
				pathNode.child_ordinal = index + 1U;
			}
			path.push_back(std::move(pathNode));
			currentPageId = next;
			--expectedLevel;
		}

		BTreeDeleteMutationNode &leaf = nodeFor(path.back().header.page_id);
		const auto erased = std::ranges::find_if(
				leaf.entries, [&key](const BTreePageEntry &entry) { return std::ranges::equal(entry.key, key); });
		if (erased == leaf.entries.end()) {
			snapshot.release();
			return false;
		}
		leaf.entries.erase(erased);
		leaf.dirty = true;

		std::vector<IndexTreePageRetirement> retirements;
		const auto retire = [&retirements](BTreeDeleteMutationNode &node) {
			// ZYX_COV_EXCL_START: retirement removes this node from the remaining reachable mutation plan.
			if (node.retired) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable B+Tree erase attempted to retire one page more than once");
			}
			// ZYX_COV_EXCL_STOP
			node.retired = true;
			node.dirty = false;
			retirements.push_back({node.location, node.owner_lsn});
		};
		std::optional<PageId> collapsedRoot;
		size_t pathIndex = path.size() - 1U;
		while (true) {
			BTreeDeleteMutationNode &current = nodeFor(path[pathIndex].header.page_id);
			if (pathIndex == 0U) {
				if (current.header.kind == BTreeNodeKind::BPN_INTERNAL && current.entries.empty()) {
					const PageId nextRoot = current.header.leftmost_child;
					retire(current);
					collapsedRoot = nextRoot;
				}
				break;
			}
			if (!isUnderfullDeleteNode(indexId, current)) {
				break;
			}

			BTreeDeleteMutationNode &parent = nodeFor(path[pathIndex - 1U].header.page_id);
			const size_t currentOrdinal = childOrdinal(parent, current.header.page_id);
			if (parent.entries.empty()) {
				// A pre-existing unary parent cannot donate a sibling at this level.
				// Continue upward so the root-collapse/merge path repairs that shape
				// without publishing an empty non-root page.
				--pathIndex;
				continue;
			}
			const size_t leftOrdinal = currentOrdinal < parent.entries.size() ? currentOrdinal : currentOrdinal - 1U;
			BTreeDeleteMutationNode &left = loadNode(childAt(parent, leftOrdinal), current.header.level);
			BTreeDeleteMutationNode &right = loadNode(childAt(parent, leftOrdinal + 1U), current.header.level);
			// loadNode has already validated both sibling node kinds and levels against
			// current.header.level; a retired sibling is the only mutable-plan conflict.
			if (left.retired || right.retired) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable B+Tree erase found an invalid sibling pair while rebalancing");
			}

			if (current.header.kind == BTreeNodeKind::BPN_LEAF) {
				const uint8_t invalidLinks = static_cast<uint8_t>(left.header.right_sibling != right.header.page_id) |
											 static_cast<uint8_t>(right.header.left_sibling != left.header.page_id);
				if (invalidLinks != 0U) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable B+Tree erase found leaf siblings that disagree with their parent order");
				}
				if (!left.entries.empty() && !right.entries.empty() &&
					!lessKey(left.entries.back().key, right.entries.front().key)) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable B+Tree erase cannot merge leaf siblings with overlapping key ranges");
				}
				std::vector<BTreePageEntry> combined = left.entries;
				combined.insert(combined.end(), right.entries.begin(), right.entries.end());
				std::optional<size_t> redistribution;
				uint64_t smallestSkew = std::numeric_limits<uint64_t>::max();
				for (size_t split = 1U; split < combined.size(); ++split) {
					BTreeDeleteMutationNode leftCandidate = left;
					BTreeDeleteMutationNode rightCandidate = right;
					leftCandidate.entries.assign(combined.begin(), combined.begin() + static_cast<ptrdiff_t>(split));
					rightCandidate.entries.assign(combined.begin() + static_cast<ptrdiff_t>(split), combined.end());
					try {
						const uint32_t leftBytes =
								encodedNodeOccupancy(materializeDeleteMutationNode(indexId, leftCandidate));
						const uint32_t rightBytes =
								encodedNodeOccupancy(materializeDeleteMutationNode(indexId, rightCandidate));
						if (leftBytes >= RSS_BTREE_MIN_NODE_OCCUPIED_BYTES &&
							rightBytes >= RSS_BTREE_MIN_NODE_OCCUPIED_BYTES) {
							const uint64_t skew =
									leftBytes > rightBytes ? leftBytes - rightBytes : rightBytes - leftBytes;
							if (!redistribution.has_value() || skew < smallestSkew) {
								redistribution = split;
								smallestSkew = skew;
							}
						}
					} catch (const FormatException &error) {
						// ZYX_COV_EXCL_START: these candidates are reconstructed from validated internal records.
						if (error.error() != FormatError::FERR_INVALID_STATE) {
							throw;
						}
						// ZYX_COV_EXCL_STOP
					}
				}
				if (redistribution.has_value()) {
					left.entries.assign(combined.begin(), combined.begin() + static_cast<ptrdiff_t>(*redistribution));
					right.entries.assign(combined.begin() + static_cast<ptrdiff_t>(*redistribution), combined.end());
					left.dirty = true;
					right.dirty = true;
					parent.entries[leftOrdinal].key = right.entries.front().key;
					parent.dirty = true;
					break;
				}

				const bool retainRight = left.entries.empty() && !right.entries.empty();
				BTreeDeleteMutationNode merged = retainRight ? right : left;
				merged.entries = std::move(combined);
				if (retainRight) {
					merged.header.left_sibling = left.header.left_sibling;
				} else {
					merged.header.right_sibling = right.header.right_sibling;
				}
				try {
					static_cast<void>(materializeDeleteMutationNode(indexId, merged));
				} catch (const FormatException &error) {
					if (error.error() == FormatError::FERR_INVALID_STATE) {
						if (!current.entries.empty()) {
							// Variable-length keys can make both a half-full split and a
							// one-page merge physically impossible. Retaining this nonempty
							// leaf is safer than publishing an invalid layout; a later delete
							// can merge it once the combined encoded payload fits.
							break;
						}
						throw FormatException(
								FormatError::FERR_INVALID_STATE,
								"relocatable B+Tree erase cannot redistribute or merge an underfull leaf pair");
					}
					throw;
				}
				if (retainRight) {
					right.entries = std::move(merged.entries);
					right.header.left_sibling = merged.header.left_sibling;
					right.dirty = true;
					const PageId previousLeaf = left.header.left_sibling;
					retire(left);
					if (previousLeaf.isValid()) {
						BTreeDeleteMutationNode &previous = loadNode(previousLeaf, 0U);
						if (previous.header.right_sibling != left.header.page_id) {
							throw FormatException(
									FormatError::FERR_INVALID_STATE,
									"relocatable B+Tree erase cannot repair a broken predecessor leaf link");
						}
						previous.header.right_sibling = right.header.page_id;
						previous.dirty = true;
					}
					if (leftOrdinal == 0U) {
						parent.header.leftmost_child = right.header.page_id;
						parent.entries.erase(parent.entries.begin());
					} else {
						parent.entries.erase(parent.entries.begin() + static_cast<ptrdiff_t>(leftOrdinal - 1U));
					}
				} else {
					left.entries = std::move(merged.entries);
					left.header.right_sibling = merged.header.right_sibling;
					left.dirty = true;
					const PageId nextLeaf = right.header.right_sibling;
					retire(right);
					if (nextLeaf.isValid()) {
						BTreeDeleteMutationNode &next = loadNode(nextLeaf, 0U);
						if (next.header.left_sibling != right.header.page_id) {
							throw FormatException(
									FormatError::FERR_INVALID_STATE,
									"relocatable B+Tree erase cannot repair a broken successor leaf link");
						}
						next.header.left_sibling = left.header.page_id;
						next.dirty = true;
					}
					parent.entries.erase(parent.entries.begin() + static_cast<ptrdiff_t>(leftOrdinal));
				}
				parent.dirty = true;
				--pathIndex;
				continue;
			}

			const BTreePageEntry parentSeparator = parent.entries[leftOrdinal];
			BTreePageEntry bridge = parentSeparator;
			bridge.value_or_child = right.header.leftmost_child.value();
			std::vector<BTreePageEntry> combined = left.entries;
			combined.push_back(bridge);
			combined.insert(combined.end(), right.entries.begin(), right.entries.end());
			std::optional<size_t> redistribution;
			uint64_t smallestSkew = std::numeric_limits<uint64_t>::max();
			for (size_t promoted = 1U; promoted + 1U < combined.size(); ++promoted) {
				BTreeDeleteMutationNode leftCandidate = left;
				BTreeDeleteMutationNode rightCandidate = right;
				leftCandidate.entries.assign(combined.begin(), combined.begin() + static_cast<ptrdiff_t>(promoted));
				rightCandidate.header.leftmost_child = PageId(combined[promoted].value_or_child);
				rightCandidate.entries.assign(combined.begin() + static_cast<ptrdiff_t>(promoted + 1U), combined.end());
				try {
					const uint32_t leftBytes =
							encodedNodeOccupancy(materializeDeleteMutationNode(indexId, leftCandidate));
					const uint32_t rightBytes =
							encodedNodeOccupancy(materializeDeleteMutationNode(indexId, rightCandidate));
					if (leftBytes >= RSS_BTREE_MIN_NODE_OCCUPIED_BYTES &&
						rightBytes >= RSS_BTREE_MIN_NODE_OCCUPIED_BYTES) {
						const uint64_t skew = leftBytes > rightBytes ? leftBytes - rightBytes : rightBytes - leftBytes;
						if (!redistribution.has_value() || skew < smallestSkew) {
							redistribution = promoted;
							smallestSkew = skew;
						}
					}
				} catch (const FormatException &error) {
					if (error.error() != FormatError::FERR_INVALID_STATE) {
						throw;
					}
				}
			}
			if (redistribution.has_value()) {
				left.entries.assign(combined.begin(), combined.begin() + static_cast<ptrdiff_t>(*redistribution));
				right.header.leftmost_child = PageId(combined[*redistribution].value_or_child);
				right.entries.assign(combined.begin() + static_cast<ptrdiff_t>(*redistribution + 1U), combined.end());
				left.dirty = true;
				right.dirty = true;
				parent.entries[leftOrdinal].key = combined[*redistribution].key;
				parent.dirty = true;
				break;
			}

			BTreeDeleteMutationNode merged = left;
			merged.entries = std::move(combined);
			try {
				static_cast<void>(materializeDeleteMutationNode(indexId, merged));
			} catch (const FormatException &error) {
				if (error.error() == FormatError::FERR_INVALID_STATE) {
					if (!current.entries.empty()) {
						// Internal separator promotion temporarily removes one cell from
						// the two children. With variable-length keys, preserving a
						// nonempty node is the only valid representation when neither a
						// balanced redistribution nor a merge fits one page.
						break;
					}
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable B+Tree erase cannot redistribute or merge an underfull internal pair");
				}
				throw;
			}
			left.entries = std::move(merged.entries);
			left.dirty = true;
			retire(right);
			parent.entries.erase(parent.entries.begin() + static_cast<ptrdiff_t>(leftOrdinal));
			parent.dirty = true;
			--pathIndex;
		}

		if (collapsedRoot.has_value()) {
			PageId finalRoot = *collapsedRoot;
			uint16_t finalRootLevel = initialRootHeader.level;
			while (finalRootLevel != 0U) {
				--finalRootLevel;
				BTreeDeleteMutationNode &candidate = loadNode(finalRoot, finalRootLevel);
				if (candidate.header.kind != BTreeNodeKind::BPN_INTERNAL || !candidate.entries.empty()) {
					break;
				}
				// ZYX_COV_EXCL_START: BTreePage::open guarantees that every internal node has a leftmost child.
				if (!candidate.header.leftmost_child.isValid()) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable B+Tree erase cannot recursively collapse a malformed internal root");
				}
				// ZYX_COV_EXCL_STOP
				const PageId child = candidate.header.leftmost_child;
				retire(candidate);
				finalRoot = child;
			}
			if (finalRootLevel == 0U) {
				BTreeDeleteMutationNode &candidate = loadNode(finalRoot, 0U);
				if (candidate.header.left_sibling.isValid() || candidate.header.right_sibling.isValid()) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "relocatable B+Tree erase cannot publish a root leaf with sibling links");
				}
			}
			collapsedRoot = finalRoot;
		}
		struct ReplacementPlan {
			PageLocation location;
			BTreePage page;
			LogSequenceNumber expected_owner_lsn{};
		};
		auto replacementPlans = std::make_shared<std::vector<ReplacementPlan>>();
		for (const auto &[pageId, node]: nodes) {
			static_cast<void>(pageId);
			// Retiring a node clears dirty, so only live dirty nodes can have a replacement.
			if (node.dirty) {
				replacementPlans->push_back(
						{node.location, materializeDeleteMutationNode(indexId, node), node.owner_lsn});
			}
		}
		// ZYX_COV_EXCL_START: a successful erase always dirties its leaf or retires a reachable page.
		if (replacementPlans->empty() && retirements.empty()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable B+Tree erase produced no reachable page mutation");
		}
		// ZYX_COV_EXCL_STOP
		std::vector<IndexTreeMutationPoolRequest> requests;
		std::vector<std::vector<size_t>> replacementPlanIndexes;
		std::map<uint64_t, size_t> requestIndexes;
		const auto ensureRequest = [&](SegmentId segmentId) {
			const auto [iterator, inserted] = requestIndexes.emplace(segmentId.value(), requests.size());
			if (inserted) {
				requests.push_back({segmentId, {}, 0, {}});
				replacementPlanIndexes.emplace_back();
			}
			return iterator->second;
		};
		for (size_t index = 0; index < replacementPlans->size(); ++index) {
			const size_t requestIndex = ensureRequest((*replacementPlans)[index].location.segment_id);
			requests[requestIndex].expected_replacements.push_back((*replacementPlans)[index].location);
			replacementPlanIndexes[requestIndex].push_back(index);
		}
		for (const IndexTreePageRetirement &retirement: retirements) {
			const size_t requestIndex = ensureRequest(retirement.location.segment_id);
			requests[requestIndex].expected_retirements.push_back(retirement);
		}
		const BTreeStructuralMutationPreparer preparer = [this, indexId, replacementPlans,
														  replacementPlanIndexes = std::move(replacementPlanIndexes)](
																 std::vector<IndexTreeMutationPoolBatch> &batches,
																 LogSequenceNumber checkpointLsn) {
			// ZYX_COV_EXCL_START: Metadata creates one callback batch per immutable request group.
			if (batches.size() != replacementPlanIndexes.size()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable B+Tree erase received an unexpected multi-pool mutation batch");
			}
			// ZYX_COV_EXCL_STOP
			for (size_t poolIndex = 0; poolIndex < batches.size(); ++poolIndex) {
				IndexTreeMutationPoolBatch &batch = batches[poolIndex];
				const std::vector<size_t> &planIndexes = replacementPlanIndexes[poolIndex];
				// ZYX_COV_EXCL_START: erase never requests appends or new pools, and Metadata preserves replacements.
				if (batch.expected_replacements.size() != planIndexes.size() || !batch.appended_page_ids.empty() ||
					batch.newly_reserved) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "relocatable B+Tree erase mutation batch changed before preparation");
				}
				// ZYX_COV_EXCL_STOP
				std::vector<SharedPagePoolPreparedReplacement> preparedReplacements;
				preparedReplacements.reserve(planIndexes.size());
				for (size_t index = 0; index < planIndexes.size(); ++index) {
					const ReplacementPlan &plan = (*replacementPlans)[planIndexes[index]];
					// ZYX_COV_EXCL_START: plans and batches are derived from the same pinned Page Directory generation.
					if (batch.expected_replacements[index] != plan.location) {
						throw FormatException(
								FormatError::FERR_INVALID_STATE,
								"relocatable B+Tree erase replacement location changed before publication");
					}
					// ZYX_COV_EXCL_STOP
					const BTreePageHeader header = plan.page.header();
					const uint8_t invalidPage = static_cast<uint8_t>(header.index_id != indexId) |
												static_cast<uint8_t>(header.page_id != plan.location.page_id) |
												static_cast<uint8_t>(header.generation != batch.pool.generation) |
												static_cast<uint8_t>(plan.page.slottedPage().ownerCommittedLsn() !=
																	 plan.expected_owner_lsn);
					// ZYX_COV_EXCL_START: the local replacement is built only from a validated current page and its
					// immutable plan.
					if (invalidPage != 0U) {
						throw FormatException(
								FormatError::FERR_INVALID_STATE,
								"relocatable B+Tree erase materialized a replacement with an invalid identity");
					}
					// ZYX_COV_EXCL_STOP
					std::vector<std::byte> bytes = plan.page.serialize();
					PageOwnerHeader owner =
							indexPageOwner(indexId, plan.location.page_id, batch.pool.generation, checkpointLsn);
					owner.payload_bytes = static_cast<uint32_t>(bytes.size());
					preparedReplacements.push_back({std::move(owner), plan.expected_owner_lsn, std::move(bytes),
													plan.page.slottedPage().stats().contiguous_free_bytes});
				}
				std::vector<SharedPagePoolPreparedRetirement> preparedRetirements;
				preparedRetirements.reserve(batch.expected_retirements.size());
				for (const IndexTreePageRetirement &retirement: batch.expected_retirements) {
					preparedRetirements.push_back(
							{retirement.location, indexId, PageOwnerKind::POK_INDEX, retirement.expected_owner_lsn});
				}
				const std::vector<SharedPagePoolPreparedAppend> noAppends;
				pages_.prepareMutationPages(batch.pool, batch.expected_replacements, preparedReplacements, noAppends,
											batch.replacement_locations, batch.appended_locations, checkpointLsn,
											preparedRetirements);
			}
		};
		const BTreeStructuralMutationPlan mutationPlan{root,		 std::move(requests), std::nullopt,		  {},
													   std::nullopt, collapsedRoot,		  std::move(preparer)};
		snapshot.release();
		for (const IndexTreeMutationPoolRequest &request: mutationPlan.pool_requests) {
			pages_.reconcilePool(request.segment_id);
		}
		static_cast<void>(metadata_.mutateIndexTreePagesAcrossPools(
				mutationPlan.pool_requests, std::nullopt, mutationPlan.expected_root, mutationPlan.replacement_root,
				mutationPlan.stable_replacement_root, checkpointLsn,
				[&mutationPlan, checkpointLsn](std::vector<IndexTreeMutationPoolBatch> &batches) {
					mutationPlan.preparer(batches, checkpointLsn);
				},
				options));
		return true;
	}

	SharedPagePoolAllocation BTreePageStore::upsertRootLeaf(uint64_t indexId, std::span<const std::byte> key,
															uint64_t value, LogSequenceNumber checkpointLsn,
															const MetadataPublishOptions &options) {
		return upsertRootLeafImpl(indexId, key, value, std::nullopt, checkpointLsn, options);
	}

	SharedPagePoolAllocation BTreePageStore::upsertRootLeaf(uint64_t indexId, std::span<const std::byte> key,
															uint64_t value, IndexPagePoolGrowthOptions growth,
															LogSequenceNumber checkpointLsn,
															const MetadataPublishOptions &options) {
		return upsertRootLeafImpl(indexId, key, value, growth, checkpointLsn, options);
	}

	SharedPagePoolAllocation BTreePageStore::upsertRootLeafImpl(uint64_t indexId, std::span<const std::byte> key,
																uint64_t value,
																std::optional<IndexPagePoolGrowthOptions> growth,
																LogSequenceNumber checkpointLsn,
																const MetadataPublishOptions &options) {
		if (indexId == 0U) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable B+Tree root-leaf upsert requires an Index ID");
		}
		MetadataSnapshotPin snapshot = metadata_.openSnapshot();
		const IndexRootLocation root = metadata_.findIndexRoot(snapshot, indexId);
		BTreePage current = read(snapshot, root.root_page_id);
		const BTreePageHeader header = current.header();
		const uint8_t invalidRoot = static_cast<uint8_t>(header.index_id != indexId) |
									static_cast<uint8_t>(header.page_id != root.root_page_id) |
									static_cast<uint8_t>(header.generation != root.root_generation) |
									static_cast<uint8_t>(header.kind != BTreeNodeKind::BPN_LEAF) |
									static_cast<uint8_t>(header.left_sibling.isValid()) |
									static_cast<uint8_t>(header.right_sibling.isValid());
		if (invalidRoot != 0U) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable root-leaf upsert requires one unsplit leaf root owned by the target index");
		}
		const PageLocation expectedRootLocation = metadata_.findPageLocation(snapshot, root.root_page_id);
		const LogSequenceNumber expectedOwnerLsn = current.slottedPage().ownerCommittedLsn();
		std::vector<BTreePageEntry> entries = current.entries();
		bool replaced = false;
		for (BTreePageEntry &entry: entries) {
			if (std::ranges::equal(entry.key, key)) {
				entry.value_or_child = value;
				replaced = true;
			}
		}
		if (!replaced) {
			entries.push_back({std::vector<std::byte>(key.begin(), key.end()), value});
			std::ranges::sort(entries, {}, &BTreePageEntry::key);
		}
		const PageOwnerHeader sourceOwner =
				indexPageOwner(indexId, header.page_id, header.generation, expectedOwnerLsn);
		if (replaced) {
			BTreePage replacement = makeLeafPage(header, sourceOwner, {}, {}, entries);
			snapshot.release();
			return replace(replacement, checkpointLsn, options);
		}
		std::optional<BTreePage> rootReplacement;
		try {
			rootReplacement.emplace(makeLeafPage(header, sourceOwner, {}, {}, entries));
		} catch (const FormatException &error) {
			// ZYX_COV_EXCL_START: a root leaf rebuilt from validated entries can only fail its capacity check.
			if (error.error() != FormatError::FERR_INVALID_STATE) {
				throw;
			}
			// ZYX_COV_EXCL_STOP
		}
		if (rootReplacement.has_value()) {
			snapshot.release();
			return replace(*rootReplacement, checkpointLsn, options);
		}

		// The attempted insert cannot fit in the root leaf. Preserve that leaf's
		// PageId through a COW replacement, append its new sibling and a new root,
		// then publish all three Page Directory entries with the root switch.
		const std::optional<size_t> splitIndex =
				findBalancedLeafSplitIndex(header, indexId, expectedOwnerLsn, checkpointLsn, {}, {}, entries);
		if (!splitIndex.has_value()) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable B+Tree root-leaf split cannot fit the requested key into either leaf page");
		}
		const std::array<PageLocation, 1> expectedReplacements = {expectedRootLocation};
		std::vector<IndexTreeMutationPoolRequest> requests;
		requests.push_back({expectedRootLocation.segment_id, {expectedRootLocation}, 0, {}});
		std::optional<NewIndexTreePoolRequest> newPool;
		size_t appendPoolOrdinal = 0;
		if (const std::optional<SegmentId> selectedPool = selectAppendPool(snapshot, expectedReplacements, 2U);
			selectedPool.has_value()) {
			if (*selectedPool != expectedRootLocation.segment_id) {
				appendPoolOrdinal = requests.size();
				requests.push_back({*selectedPool, {}, 0, {}});
			}
			requests[appendPoolOrdinal].appended_page_count = 2U;
		} else {
			if (!growth.has_value()) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable root-leaf split needs an index-pool growth range when all current pools are full");
			}
			appendPoolOrdinal = requests.size();
			newPool = newPoolRequest(*growth, indexId, 2U);
		}
		snapshot.release();
		for (const IndexTreeMutationPoolRequest &request: requests) {
			pages_.reconcilePool(request.segment_id);
		}
		const IndexTreeMultiPoolMutationPreparer preparer = [this, indexId, header, expectedOwnerLsn,
															 entries = std::move(entries), splitIndex = *splitIndex,
															 appendPoolOrdinal, checkpointLsn](
																	std::vector<IndexTreeMutationPoolBatch> &batches) {
			// ZYX_COV_EXCL_START: Metadata builds callback batches directly from this root-split request.
			if (batches.empty() || appendPoolOrdinal >= batches.size() ||
				batches.front().expected_replacements.size() != 1U ||
				batches.front().expected_replacements.front().page_id != header.page_id) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable root-leaf split received an unexpected multi-pool mutation batch");
			}
			// ZYX_COV_EXCL_STOP
			const std::vector<PageId> &appendedPageIds = batches[appendPoolOrdinal].appended_page_ids;
			// ZYX_COV_EXCL_START: this request reserves exactly the leaf and root.
			if (appendedPageIds.size() != 2U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable root-leaf split received an unexpected appended Page ID batch");
			}
			// ZYX_COV_EXCL_STOP
			const PageId rightLeafId = appendedPageIds[0];
			const PageId newRootId = appendedPageIds[1];
			for (size_t poolIndex = 0; poolIndex < batches.size(); ++poolIndex) {
				IndexTreeMutationPoolBatch &batch = batches[poolIndex];
				std::vector<BTreePage> materialized;
				if (poolIndex == 0U) {
					materialized.push_back(makeLeafPage(
							header, indexPageOwner(indexId, header.page_id, batch.pool.generation, expectedOwnerLsn),
							{}, rightLeafId, std::span(entries).subspan(0, splitIndex)));
				}
				if (poolIndex == appendPoolOrdinal) {
					materialized.push_back(makeLeafPage(
							header, indexPageOwner(indexId, rightLeafId, batch.pool.generation, checkpointLsn),
							header.page_id, {}, std::span(entries).subspan(splitIndex)));
					BTreePageHeader rootHeader;
					rootHeader.index_id = indexId;
					rootHeader.page_id = newRootId;
					rootHeader.generation = batch.pool.generation;
					rootHeader.kind = BTreeNodeKind::BPN_INTERNAL;
					rootHeader.level = 1;
					rootHeader.leftmost_child = header.page_id;
					BTreePage newRoot = BTreePage::create(
							indexPageOwner(indexId, newRootId, batch.pool.generation, checkpointLsn), rootHeader);
					newRoot.insert({entries[splitIndex].key, rightLeafId.value()});
					materialized.push_back(std::move(newRoot));
				}
				const size_t replacementCount = batch.expected_replacements.size();
				const size_t appendedCount = batch.appended_page_ids.size();
				// ZYX_COV_EXCL_START: each plan yields one page and the immutable batch counts exactly those plans.
				if (materialized.size() != replacementCount + appendedCount) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "relocatable root-leaf split did not materialize every multi-pool page");
				}
				// ZYX_COV_EXCL_STOP
				std::vector<SharedPagePoolPreparedReplacement> replacements;
				std::vector<SharedPagePoolPreparedAppend> appends;
				replacements.reserve(replacementCount);
				appends.reserve(appendedCount);
				for (size_t index = 0; index < materialized.size(); ++index) {
					const bool isReplacement = index < replacementCount;
					const PageId expectedPageId = isReplacement ? batch.expected_replacements[index].page_id
																: batch.appended_page_ids[index - replacementCount];
					const BTreePage &page = materialized[index];
					const BTreePageHeader pageHeader = page.header();
					const uint8_t invalidPage =
							static_cast<uint8_t>(pageHeader.index_id != indexId) |
							static_cast<uint8_t>(pageHeader.page_id != expectedPageId) |
							static_cast<uint8_t>(pageHeader.generation != batch.pool.generation) |
							static_cast<uint8_t>(page.slottedPage().ownerCommittedLsn() !=
												 (isReplacement ? expectedOwnerLsn : checkpointLsn));
					// ZYX_COV_EXCL_START: all split pages are locally created from the validated root and
					// Metadata-assigned Page IDs.
					if (invalidPage != 0U) {
						throw FormatException(
								FormatError::FERR_INVALID_STATE,
								"relocatable root-leaf split materialized a page with an invalid durable identity");
					}
					// ZYX_COV_EXCL_STOP
					std::vector<std::byte> bytes = page.serialize();
					PageOwnerHeader owner =
							indexPageOwner(indexId, expectedPageId, batch.pool.generation, checkpointLsn);
					owner.payload_bytes = static_cast<uint32_t>(bytes.size());
					if (isReplacement) {
						replacements.push_back({std::move(owner), expectedOwnerLsn, std::move(bytes),
												page.slottedPage().stats().contiguous_free_bytes});
					} else {
						appends.push_back(
								{std::move(owner), std::move(bytes), page.slottedPage().stats().contiguous_free_bytes});
					}
				}
				if (batch.newly_reserved) {
					pages_.prepareNewPoolPages(batch.pool, appends, batch.appended_locations, checkpointLsn);
				} else {
					pages_.prepareMutationPages(batch.pool, batch.expected_replacements, replacements, appends,
												batch.replacement_locations, batch.appended_locations, checkpointLsn);
				}
			}
		};
		const Superblock published = metadata_.mutateIndexTreePagesAcrossPools(
				requests, newPool, root, IndexTreeMutationAppendedPageReference{appendPoolOrdinal, 1U}, std::nullopt,
				checkpointLsn, preparer, options);
		MetadataSnapshotPin publishedSnapshot = metadata_.openSnapshot();
		const IndexRootLocation publishedRoot = metadata_.findIndexRoot(publishedSnapshot, indexId);
		const PageLocation publishedLocation =
				metadata_.findPageLocation(publishedSnapshot, publishedRoot.root_page_id);
		const CatalogEntry publishedPool = metadata_.findCatalog(publishedSnapshot, publishedLocation.segment_id);
		const BTreePage publishedRootPage = read(publishedSnapshot, publishedRoot.root_page_id);
		PageOwnerHeader owner;
		owner.page_id = publishedRootPage.slottedPage().pageId();
		owner.owner_id = publishedRootPage.slottedPage().ownerId();
		owner.kind = publishedRootPage.slottedPage().ownerKind();
		owner.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
		owner.generation = publishedRootPage.slottedPage().pageGeneration();
		owner.committed_lsn = publishedRootPage.slottedPage().ownerCommittedLsn();
		publishedSnapshot.release();
		return {published, publishedPool, publishedLocation, owner};
	}
} // namespace graph::storage
