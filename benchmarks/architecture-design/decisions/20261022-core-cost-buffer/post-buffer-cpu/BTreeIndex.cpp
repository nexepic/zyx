#include "cpu-phase.hpp"
/**
 * @file BTreeIndex.cpp
 * @brief Snapshot-pinned read traversal for isolated relocatable B+Tree pages.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/index/BTreeIndex.hpp"

#include <algorithm>
#include <deque>
#include <memory>
#include <set>
#include <stdexcept>

#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/metadata/MetadataPageReadContext.hpp"
#include "graph/utils/OwnedPmrObject.hpp"
#include "graph/utils/OwnedSharedPtr.hpp"

namespace graph::storage {

	namespace {

		[[noreturn]] void throwBTreeIndexError(FormatError error, const char *message) {
			throw FormatException(error, message);
		}

		[[nodiscard]] bool lessKey(std::span<const std::byte> left, std::span<const std::byte> right) {
			return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end());
		}

		[[nodiscard]] bool isSupportedScanLeafReadHint(BufferPoolReadHint hint) {
			return hint == BufferPoolReadHint::BPRH_NORMAL || hint == BufferPoolReadHint::BPRH_SEQUENTIAL;
		}

		void validateIndexPage(const BTreePage &page, uint64_t indexId) {
			const BTreePageHeader header = page.header();
			const uint8_t invalid = static_cast<uint8_t>(header.index_id != indexId) |
									static_cast<uint8_t>(!header.page_id.isValid()) |
									static_cast<uint8_t>(!header.generation.isValid());
			if (invalid != 0U) {
				throwBTreeIndexError(FormatError::FERR_INVALID_STATE,
									 "relocatable B+Tree traversal reached a page owned by another index");
			}
		}

	} // namespace

	struct BTreeIndex::RangeCursor::State : utils::OwnedPmrObject {
		const BTreeIndex *index = nullptr;
		const MetadataSnapshotPin *snapshot = nullptr;
		size_t maximum_entries = 0U;
		size_t emitted = 0U;
		bool exhausted = false;
		std::optional<BTreePage> leaf;
		utils::OwnedStdVector<BTreePageEntry> entries;
		size_t entry_index = 0U;
		utils::OwnedStdVector<BTreeIndex::TraversalStep> traversal;
		utils::OwnedHeapReservation previous_leaf_last_key_credit{utils::OwnedAllocationScope::owner()};
        std::optional<std::vector<std::byte>> previous_leaf_last_key;
		const CancellationCheckpoint *cancellation = nullptr;
	};

	BTreeIndex::RangeCursor::RangeCursor(std::unique_ptr<State> state) : state_(std::move(state)) {}
	BTreeIndex::RangeCursor::~RangeCursor() = default;
	BTreeIndex::RangeCursor::RangeCursor(RangeCursor &&) noexcept = default;
	BTreeIndex::RangeCursor &BTreeIndex::RangeCursor::operator=(RangeCursor &&) noexcept = default;

	std::optional<BTreePageEntry> BTreeIndex::RangeCursor::next(const std::function<void(size_t)> &boundaryKeyAdmission) {
		if (state_ == nullptr || state_->index == nullptr || state_->snapshot == nullptr) {
			throwBTreeIndexError(FormatError::FERR_INVALID_STATE,
								 "relocatable B+Tree range cursor has no snapshot-bound state");
		}
		return state_->index->nextRangeCursor(*state_, boundaryKeyAdmission);
	}

	BTreeIndex::BTreeIndex(uint64_t indexId, BTreePageStore &pages, RelocatableMetadataStore &metadata,
						   BTreeLeafValueLayout leafValueLayout, std::optional<BTreeRootReadBinding> rootBinding) :
        root_binding_(std::move(rootBinding)), index_id_(indexId), pages_(pages), metadata_(metadata), leaf_value_layout_(leafValueLayout) {
        if (root_binding_ && (!root_binding_->database_id.isValid() ||
            !root_binding_->snapshot_generation.isValid() || root_binding_->root.index_id != indexId ||
            !root_binding_->root.root_page_id.isValid() || !root_binding_->root.root_generation.isValid()))
            throw std::invalid_argument("relocatable bound B+Tree requires an exact database, snapshot and root identity");
		if (index_id_ == 0U) {
			throw std::invalid_argument("relocatable B+Tree index requires a valid Index ID");
		}
		if (leaf_value_layout_ != BTreeLeafValueLayout::BLVL_U64 &&
			leaf_value_layout_ != BTreeLeafValueLayout::BLVL_EMPTY) {
			throw std::invalid_argument("relocatable B+Tree index requires a supported leaf-value layout");
		}
		if (!pages_.usesMetadataStore(metadata_)) {
			throw std::invalid_argument("relocatable B+Tree index requires its page store's metadata store");
		}
	}

	std::optional<BTreePage> BTreeIndex::resolveRoot(const MetadataSnapshotPin &snapshot,
			utils::OwnedStdVector<BTreePageEntry> *decodedEntries) const {
		const std::optional<IndexRootLocation> root = metadata_.findIndexRootOptional(snapshot, index_id_);
        if (root_binding_ && (snapshot.superblock().database_id != root_binding_->database_id ||
            snapshot.superblock().generation != root_binding_->snapshot_generation || !root ||
            *root != root_binding_->root))
            throwBTreeIndexError(FormatError::FERR_INVALID_STATE,
                "descriptor-bound B+Tree root disagrees with its pinned namespace or snapshot");
		if (!root.has_value()) {
			return std::nullopt;
		}
		BTreePage page = decodedEntries != nullptr
			? pages_.readDecoded(snapshot, root->root_page_id, *decodedEntries)
			: pages_.read(snapshot, root->root_page_id);
		const BTreePageHeader header = page.header();
		const uint8_t invalid = static_cast<uint8_t>(header.index_id != index_id_) |
								static_cast<uint8_t>(header.page_id != root->root_page_id) |
								static_cast<uint8_t>(header.generation != root->root_generation);
		if (invalid != 0U) {
			throwBTreeIndexError(FormatError::FERR_INVALID_STATE,
								 "relocatable Index Root Directory disagrees with its B+Tree root page");
		}
		const uint8_t rootHasSibling = static_cast<uint8_t>(header.left_sibling.isValid()) |
									   static_cast<uint8_t>(header.right_sibling.isValid());
		if (header.kind == BTreeNodeKind::BPN_LEAF && rootHasSibling != 0U) {
			throwBTreeIndexError(FormatError::FERR_INVALID_HEADER,
								 "relocatable B+Tree root leaf cannot have sibling links");
		}
		return page;
	}

	BTreePage BTreeIndex::descendToLeaf(const MetadataSnapshotPin &snapshot, BTreePage page,
											std::optional<std::span<const std::byte>> key,
											BufferPoolReadHint leafReadHint, const CancellationCheckpoint *cancellation,
											utils::OwnedStdVector<TraversalStep> *traversal,
			utils::OwnedStdVector<BTreePageEntry> *leafEntries, LeafRoute *leafRoute,
			utils::OwnedStdVector<BTreePageEntry> *decodedRootEntries) const {
		auto leaf = descendToLeafFromRoot(snapshot, page, key, leafReadHint, cancellation, traversal, leafEntries, leafRoute, decodedRootEntries);
		return leaf.has_value() ? std::move(*leaf) : std::move(page);
	}

	std::optional<BTreePage> BTreeIndex::descendToLeafFromRoot(const MetadataSnapshotPin &snapshot, const BTreePage &root,
											std::optional<std::span<const std::byte>> key,
											BufferPoolReadHint leafReadHint, const CancellationCheckpoint *cancellation,
											utils::OwnedStdVector<TraversalStep> *traversal,
			utils::OwnedStdVector<BTreePageEntry> *leafEntries, LeafRoute *leafRoute,
			utils::OwnedStdVector<BTreePageEntry> *decodedRootEntries) const {
		const BTreePage *page = &root;
		std::optional<utils::OwnedStdVector<BTreePageEntry>> decodedEntries;
		if (decodedRootEntries != nullptr) decodedEntries.emplace(std::move(*decodedRootEntries));
		std::optional<BTreePage> ownedPage;
		std::optional<utils::OwnedStdVector<std::byte>> lowerInclusive;
		std::optional<utils::OwnedStdVector<std::byte>> upperExclusive;
		for (;;) {
			if (cancellation != nullptr) {
				cancellation->check();
			}
			validateIndexPage(*page, index_id_);
			const BTreePageHeader header = page->header();
			auto entries = decodedEntries.has_value() ? std::move(*decodedEntries) : page->entries();
			decodedEntries.reset();
			if (header.kind == BTreeNodeKind::BPN_LEAF) {
				if (header.leaf_value_layout != leaf_value_layout_) {
					throwBTreeIndexError(FormatError::FERR_INVALID_STATE,
									 "relocatable B+Tree leaf layout disagrees with its index identity");
				}
				for (const BTreePageEntry &entry: entries) {
					const uint8_t outsideRange =
							static_cast<uint8_t>(lowerInclusive.has_value() && lessKey(entry.key, *lowerInclusive)) |
							static_cast<uint8_t>(upperExclusive.has_value() && !lessKey(entry.key, *upperExclusive));
					if (outsideRange != 0U) {
						throwBTreeIndexError(FormatError::FERR_INVALID_HEADER,
											 "relocatable B+Tree leaf key violates its separator range");
					}
				}
				if (leafEntries != nullptr) *leafEntries = std::move(entries);
				if (leafRoute != nullptr)
					*leafRoute = {header.page_id, std::move(lowerInclusive), std::move(upperExclusive)};
				return ownedPage;
			}
			for (const BTreePageEntry &entry: entries) {
				const uint8_t outsideRange =
						static_cast<uint8_t>(lowerInclusive.has_value() && lessKey(entry.key, *lowerInclusive)) |
						static_cast<uint8_t>(upperExclusive.has_value() && !lessKey(entry.key, *upperExclusive));
				if (outsideRange != 0U) {
					throwBTreeIndexError(FormatError::FERR_INVALID_HEADER,
										 "relocatable B+Tree separator violates its parent range");
				}
			}
			size_t childIndex = 0;
			if (key.has_value()) {
				while (childIndex < entries.size() && !lessKey(*key, entries[childIndex].key)) {
					++childIndex;
				}
			}
			const std::optional<utils::OwnedStdVector<std::byte>> parentLowerInclusive = lowerInclusive;
			const std::optional<utils::OwnedStdVector<std::byte>> parentUpperExclusive = upperExclusive;
			PageId childId = header.leftmost_child;
			if (childIndex != 0U) {
				childId = PageId(entries[childIndex - 1U].value_or_child);
				lowerInclusive = entries[childIndex - 1U].key;
			}
			if (childIndex < entries.size()) {
				upperExclusive = entries[childIndex].key;
			}
			if (traversal != nullptr) {
				traversal->push_back({header.page_id, header.level, childIndex, parentLowerInclusive,
										 parentUpperExclusive, header, std::move(entries)});
			}
			const BufferPoolReadHint childHint = header.level == 1U ? leafReadHint : BufferPoolReadHint::BPRH_NORMAL;
			utils::OwnedStdVector<BTreePageEntry> childEntries;
			BTreePage child = pages_.readDecoded(snapshot, childId, childEntries, childHint);
			validateIndexPage(child, index_id_);
			if (child.header().level + 1U != header.level) {
				throwBTreeIndexError(FormatError::FERR_INVALID_HEADER,
									 "relocatable B+Tree child level does not descend by one");
			}
			decodedEntries.emplace(std::move(childEntries));
			ownedPage.emplace(std::move(child));
			page = &*ownedPage;
		}
	}

	BTreeIndex::LeafRoute BTreeIndex::advanceToExpectedSiblingLeaf(
			const MetadataSnapshotPin &snapshot, utils::OwnedStdVector<TraversalStep> &traversal,
			const CancellationCheckpoint *cancellation) const {
		for (size_t ancestorOrdinal = traversal.size(); ancestorOrdinal != 0U; --ancestorOrdinal) {
			if (cancellation != nullptr) {
				cancellation->check();
			}
			TraversalStep &ancestor = traversal[ancestorOrdinal - 1U];
			// `descendToLeaf` validated and captured every route page. Reuse that
			// decoded view; the snapshot still guards the eventual child read.
			const BTreePageHeader parentHeader = ancestor.header;
			if (parentHeader.kind != BTreeNodeKind::BPN_INTERNAL || parentHeader.level != ancestor.level) {
				throwBTreeIndexError(FormatError::FERR_INVALID_HEADER,
								 "relocatable B+Tree traversal path contains an invalid internal page");
			}
			const utils::OwnedStdVector<BTreePageEntry> &parentEntries = ancestor.entries;
			if (ancestor.child_index + 1U >= parentEntries.size() + 1U) {
				continue;
			}

			const size_t nextChildIndex = ancestor.child_index + 1U;
			PageId nextPageId = nextChildIndex == 0U ? parentHeader.leftmost_child
																				 : PageId(parentEntries[nextChildIndex - 1U].value_or_child);
			std::optional<utils::OwnedStdVector<std::byte>> lower = ancestor.parent_lower_inclusive;
			std::optional<utils::OwnedStdVector<std::byte>> upper = ancestor.parent_upper_exclusive;
			if (nextChildIndex != 0U) {
				lower = parentEntries[nextChildIndex - 1U].key;
			}
			if (nextChildIndex < parentEntries.size()) {
				upper = parentEntries[nextChildIndex].key;
			}
			ancestor.child_index = nextChildIndex;
			traversal.resize(ancestorOrdinal);

			uint16_t expectedLevel = static_cast<uint16_t>(parentHeader.level - 1U);
			while (expectedLevel != 0U) {
				if (cancellation != nullptr) {
					cancellation->check();
				}
				utils::OwnedStdVector<BTreePageEntry> entries;
				const BTreePage internal = pages_.readDecoded(snapshot, nextPageId, entries, BufferPoolReadHint::BPRH_NORMAL);
				validateIndexPage(internal, index_id_);
				const BTreePageHeader internalHeader = internal.header();
				if (internalHeader.kind != BTreeNodeKind::BPN_INTERNAL || internalHeader.level != expectedLevel) {
					throwBTreeIndexError(FormatError::FERR_INVALID_HEADER,
									 "relocatable B+Tree routed successor has an invalid internal level");
				}
				for (const BTreePageEntry &entry: entries) {
					const uint8_t outsideRange =
							static_cast<uint8_t>(lower.has_value() && lessKey(entry.key, *lower)) |
							static_cast<uint8_t>(upper.has_value() && !lessKey(entry.key, *upper));
					if (outsideRange != 0U) {
						throwBTreeIndexError(FormatError::FERR_INVALID_HEADER,
											 "relocatable B+Tree separator violates its parent range");
					}
				}
				traversal.push_back({internalHeader.page_id, internalHeader.level, 0U, lower, upper,
										 internalHeader, std::move(entries)});
				if (!traversal.back().entries.empty()) {
					upper = traversal.back().entries.front().key;
				}
				nextPageId = internalHeader.leftmost_child;
				--expectedLevel;
			}
			return {nextPageId, std::move(lower), std::move(upper)};
		}
		throwBTreeIndexError(FormatError::FERR_INVALID_HEADER,
							 "relocatable B+Tree sibling is outside the routed leaf sequence");
	}

	std::optional<uint64_t> BTreeIndex::find(const MetadataSnapshotPin &snapshot,
											 std::span<const std::byte> key) const {
        utils::OwnedAllocationScope allocation(utils::OwnedAllocationScope::active() ? utils::OwnedAllocationScope::owner()
            : (snapshot.readContext() ? snapshot.readContext()->memoryOwner() : nullptr));
		std::optional<BTreePage> root = resolveRoot(snapshot);
		if (!root.has_value()) {
			return std::nullopt;
		}
		utils::OwnedStdVector<BTreePageEntry> entries;
		// Reuse the entries authenticated during descent, including separator
		// bounds and leaf layout, rather than decoding the same leaf again.
		static_cast<void>(descendToLeaf(snapshot, std::move(*root), key, BufferPoolReadHint::BPRH_NORMAL,
				nullptr, nullptr, &entries));
		const auto position = std::lower_bound(entries.begin(), entries.end(), key,
											   [](const BTreePageEntry &entry, std::span<const std::byte> candidate) {
												   return lessKey(entry.key, candidate);
											   });
		if (position == entries.end() || !std::ranges::equal(position->key, key)) {
			return std::nullopt;
		}
		return position->value_or_child;
	}

	template<class Key>
    utils::OwnedStdVector<std::optional<uint64_t>> BTreeIndex::findSortedKeys(
            const MetadataSnapshotPin &snapshot, const std::span<const Key> keys) const {
        utils::OwnedAllocationScope allocation(utils::OwnedAllocationScope::active() ? utils::OwnedAllocationScope::owner()
            : (snapshot.readContext() ? snapshot.readContext()->memoryOwner() : nullptr));
		for (size_t position = 1U; position < keys.size(); ++position) {
			if (!lessKey(keys[position - 1U], keys[position]))
				throwBTreeIndexError(FormatError::FERR_INVALID_HEADER,
						"relocatable B+Tree exact batch keys must be strictly increasing");
		}
		utils::OwnedStdVector<std::optional<uint64_t>> result(keys.size());
		if (keys.empty()) return result;
		const auto root = resolveRoot(snapshot);
		if (!root.has_value()) return result;
		LeafRoute route;
		utils::OwnedStdVector<BTreePageEntry> entries;
		bool haveLeaf = false;
		for (size_t position = 0U; position < keys.size(); ++position) {
			const auto &key = keys[position];
			if (!haveLeaf || (route.lower_inclusive && lessKey(key, *route.lower_inclusive)) ||
				(route.upper_exclusive && !lessKey(key, *route.upper_exclusive))) {
				static_cast<void>(descendToLeafFromRoot(snapshot, *root, key, BufferPoolReadHint::BPRH_NORMAL,
						nullptr, nullptr, &entries, &route));
				haveLeaf = true;
			}
			const auto found = std::lower_bound(entries.begin(), entries.end(), key,
					[](const BTreePageEntry &entry, std::span<const std::byte> candidate) {
						return lessKey(entry.key, candidate);
					});
			if (found != entries.end() && std::ranges::equal(found->key, key))
				result[position] = found->value_or_child;
		}
		return result;
	}

    std::vector<std::optional<uint64_t>> BTreeIndex::findSorted(
            const MetadataSnapshotPin &snapshot, std::span<const std::vector<std::byte>> keys) const {
        auto values = findSortedKeys(snapshot, keys);
        return std::move(static_cast<std::vector<std::optional<uint64_t>> &>(values));
    }

    utils::OwnedStdVector<std::optional<uint64_t>> BTreeIndex::findSortedOwned(
            const MetadataSnapshotPin &snapshot, std::span<const utils::OwnedStdVector<std::byte>> keys) const {
        return findSortedKeys(snapshot, keys);
    }

	utils::OwnedStdVector<BTreePageEntry> BTreeIndex::scan(const MetadataSnapshotPin &snapshot,
																 std::optional<std::span<const std::byte>> lowerBound,
																 size_t maximumEntries, BufferPoolReadHint initialLeafReadHint,
																 const CancellationCheckpoint *cancellation, bool excludeLowerBound) const {
        utils::OwnedAllocationScope allocation(utils::OwnedAllocationScope::active() ? utils::OwnedAllocationScope::owner()
            : (snapshot.readContext() ? snapshot.readContext()->memoryOwner() : nullptr));
		if (!isSupportedScanLeafReadHint(initialLeafReadHint)) {
			throwBTreeIndexError(FormatError::FERR_INVALID_ENUM,
							 "relocatable B+Tree range scan received an unsupported initial-leaf read hint");
		}
		utils::OwnedStdVector<BTreePageEntry> result;
		if (maximumEntries == 0U) {
			return result;
		}
		debug::PerfTrace::addValue("native_graph.read.root_descents", 1);
		utils::OwnedStdVector<BTreePageEntry> rootEntries;
		std::optional<BTreePage> root = resolveRoot(snapshot, &rootEntries);
		if (!root.has_value()) {
			return result;
		}
		// Sibling links are an acceleration structure, not a second authority for
		// tree membership. The descent stack proves the expected successor leaf
		// without rereading the root or unrelated routing pages on every hop.
		utils::OwnedStdVector<TraversalStep> traversal;
		utils::OwnedStdVector<BTreePageEntry> entries;
		BTreePage leaf = descendToLeaf(snapshot, std::move(*root), lowerBound, initialLeafReadHint, cancellation,
									  &traversal, &entries, nullptr, &rootEntries);
		std::set<PageId> visited;
		std::optional<utils::OwnedStdVector<std::byte>> previousLeafLastKey;
		for (;;) {
			if (cancellation != nullptr) {
				cancellation->check();
			}
			const BTreePageHeader header = leaf.header();
			if (!visited.insert(header.page_id).second) {
				throwBTreeIndexError(FormatError::FERR_INVALID_HEADER,
									 "relocatable B+Tree leaf sibling chain contains a cycle");
			}
			if (!entries.empty()) {
				if (previousLeafLastKey.has_value() && !lessKey(*previousLeafLastKey, entries.front().key)) {
					throwBTreeIndexError(FormatError::FERR_INVALID_HEADER,
										 "relocatable B+Tree leaf siblings are not globally ordered");
				}
				previousLeafLastKey = entries.back().key;
			}
			auto position = entries.begin();
			if (lowerBound.has_value()) {
				position = excludeLowerBound
							   ? std::upper_bound(entries.begin(), entries.end(), *lowerBound,
													[](std::span<const std::byte> candidate, const BTreePageEntry &entry) {
														return lessKey(candidate, entry.key);
													})
							   : std::lower_bound(entries.begin(), entries.end(), *lowerBound,
												  [](const BTreePageEntry &entry, std::span<const std::byte> candidate) {
													  return lessKey(entry.key, candidate);
												  });
			}
			for (; position != entries.end() && result.size() < maximumEntries; ++position) {
				if (cancellation != nullptr) {
					cancellation->check();
				}
				result.push_back(std::move(*position));
			}
			if (result.size() == maximumEntries || !header.right_sibling.isValid()) {
				return result;
			}
			utils::OwnedStdVector<BTreePageEntry> siblingEntries;
			BTreePage sibling = pages_.readDecoded(snapshot, header.right_sibling, siblingEntries, BufferPoolReadHint::BPRH_SEQUENTIAL);
			validateIndexPage(sibling, index_id_);
			const BTreePageHeader siblingHeader = sibling.header();
			const uint8_t invalidSibling = static_cast<uint8_t>(siblingHeader.kind != BTreeNodeKind::BPN_LEAF) |
									   static_cast<uint8_t>(siblingHeader.level != 0U) |
									   static_cast<uint8_t>(siblingHeader.leaf_value_layout != leaf_value_layout_) |
									   static_cast<uint8_t>(siblingHeader.left_sibling != header.page_id);
			if (invalidSibling != 0U) {
				throwBTreeIndexError(FormatError::FERR_INVALID_HEADER,
								 "relocatable B+Tree leaf sibling link is not reciprocal");
			}
			if (siblingEntries.empty()) {
				throwBTreeIndexError(FormatError::FERR_INVALID_HEADER,
								 "relocatable B+Tree linked non-root leaf is empty");
			}
			const LeafRoute successor = advanceToExpectedSiblingLeaf(snapshot, traversal, cancellation);
			if (siblingHeader.page_id != successor.page_id) {
				throwBTreeIndexError(FormatError::FERR_INVALID_HEADER,
								 "relocatable B+Tree sibling is outside the routed leaf sequence");
			}
			for (const BTreePageEntry &entry: siblingEntries) {
				const uint8_t outsideRange =
						static_cast<uint8_t>(successor.lower_inclusive.has_value() &&
											 lessKey(entry.key, *successor.lower_inclusive)) |
						static_cast<uint8_t>(successor.upper_exclusive.has_value() &&
											 !lessKey(entry.key, *successor.upper_exclusive));
				if (outsideRange != 0U) {
					throwBTreeIndexError(FormatError::FERR_INVALID_HEADER,
									 "relocatable B+Tree sibling key violates its routed separator range");
				}
			}
			leaf = std::move(sibling);
			entries = std::move(siblingEntries);
		}
	}

	BTreeIndex::RangeCursor BTreeIndex::openRangeCursor(
			const MetadataSnapshotPin &snapshot, const std::optional<std::span<const std::byte>> lowerBound,
			const size_t maximumEntries, const BufferPoolReadHint initialLeafReadHint,
			const bool excludeLowerBound, const CancellationCheckpoint *cancellation) const {
        utils::OwnedAllocationScope allocation(utils::OwnedAllocationScope::active() ? utils::OwnedAllocationScope::owner()
            : (snapshot.readContext() ? snapshot.readContext()->memoryOwner() : nullptr));
		if (!snapshot.isValid() || maximumEntries == 0U) {
			throwBTreeIndexError(FormatError::FERR_INVALID_HEADER,
								 "relocatable B+Tree range cursor requires a live snapshot and finite entry bound");
		}
		if (!isSupportedScanLeafReadHint(initialLeafReadHint)) {
			throwBTreeIndexError(FormatError::FERR_INVALID_ENUM,
								 "relocatable B+Tree range cursor received an unsupported initial-leaf read hint");
		}

		auto state = std::unique_ptr<RangeCursor::State>(new (utils::OwnedAllocationScope::owner()) RangeCursor::State());
		state->index = this;
		state->snapshot = &snapshot;
		state->maximum_entries = maximumEntries;
		state->cancellation = cancellation;
		if (cancellation != nullptr) {
			cancellation->check();
		}
		// This cursor is the single root-to-leaf descent for a logical range.
		// Successor leaves use the retained routed path instead of restarting.
		debug::PerfTrace::addValue("native_btree.read.range_cursor_root_descent", 1);
		debug::PerfTrace::addValue("native_graph.read.root_descents", 1);
		utils::OwnedStdVector<BTreePageEntry> rootEntries;
		std::optional<BTreePage> root = resolveRoot(snapshot, &rootEntries);
		if (!root.has_value()) {
			state->exhausted = true;
			return RangeCursor(std::move(state));
		}
		state->leaf.emplace(descendToLeaf(snapshot, std::move(*root), lowerBound, initialLeafReadHint, cancellation,
														 &state->traversal, &state->entries, nullptr, &rootEntries));
		if (lowerBound.has_value()) {
			state->entry_index = excludeLowerBound
								 ? static_cast<size_t>(std::upper_bound(
														 state->entries.begin(), state->entries.end(), *lowerBound,
														 [](std::span<const std::byte> candidate, const BTreePageEntry &entry) {
															 return lessKey(candidate, entry.key);
														 }) - state->entries.begin())
								 : static_cast<size_t>(std::lower_bound(
														 state->entries.begin(), state->entries.end(), *lowerBound,
														 [](const BTreePageEntry &entry, std::span<const std::byte> candidate) {
															 return lessKey(entry.key, candidate);
														 }) - state->entries.begin());
		}
		return RangeCursor(std::move(state));
	}

	std::optional<BTreePageEntry> BTreeIndex::nextRangeCursor(RangeCursor::State &state,
		const std::function<void(size_t)> &boundaryKeyAdmission) const {
        cpu_phase::Scope cpuPhase(3);
        utils::OwnedAllocationScope allocation(utils::OwnedAllocationScope::active() ? utils::OwnedAllocationScope::owner()
            : state.previous_leaf_last_key_credit.owner());
		if (state.exhausted) {
			if (boundaryKeyAdmission) boundaryKeyAdmission(0U);
			return std::nullopt;
		}
		for (;;) {
			if (state.cancellation != nullptr) {
				state.cancellation->check();
			}
			if (boundaryKeyAdmission) {
				boundaryKeyAdmission(state.previous_leaf_last_key.has_value()
						? state.previous_leaf_last_key->capacity() : 0U);
			}
			if (state.emitted == state.maximum_entries) {
				if (state.entry_index < state.entries.size()) {
					throwBTreeIndexError(FormatError::FERR_INVALID_STATE,
									 "relocatable B+Tree range cursor exceeded its configured entry bound");
				}
				// Advance once only to distinguish the final permitted entry from a
				// truncated result. The cursor remains bounded to one leaf and route.
				if (state.leaf.has_value() && state.leaf->header().right_sibling.isValid()) {
					const BTreePageHeader header = state.leaf->header();
					const BTreePage sibling = pages_.read(*state.snapshot, header.right_sibling,
																					 BufferPoolReadHint::BPRH_SEQUENTIAL);
					validateIndexPage(sibling, index_id_);
					const BTreePageHeader siblingHeader = sibling.header();
					const uint8_t invalidSibling =
							static_cast<uint8_t>(siblingHeader.kind != BTreeNodeKind::BPN_LEAF) |
							static_cast<uint8_t>(siblingHeader.level != 0U) |
							static_cast<uint8_t>(siblingHeader.leaf_value_layout != leaf_value_layout_) |
							static_cast<uint8_t>(siblingHeader.left_sibling != header.page_id);
					if (invalidSibling != 0U || sibling.entries().empty()) {
						throwBTreeIndexError(FormatError::FERR_INVALID_HEADER,
														 "relocatable B+Tree range cursor has an invalid successor leaf");
					}
					const LeafRoute successor =
							advanceToExpectedSiblingLeaf(*state.snapshot, state.traversal, state.cancellation);
					if (siblingHeader.page_id != successor.page_id) {
						throwBTreeIndexError(FormatError::FERR_INVALID_HEADER,
														 "relocatable B+Tree sibling is outside the routed leaf sequence");
					}
					throwBTreeIndexError(FormatError::FERR_INVALID_STATE,
									 "relocatable B+Tree range cursor exceeded its configured entry bound");
				}
				state.exhausted = true;
				return std::nullopt;
			}
			if (state.entry_index < state.entries.size()) {
				// Only the final key of a leaf participates in sibling validation.
				// Admit and copy before mutating the emission position, so budget
				// rejection or allocation failure cannot consume an entry.
                if (state.entry_index + 1U == state.entries.size() &&
                    state.leaf->header().right_sibling.isValid() && !state.previous_leaf_last_key.has_value()) {
                    const auto &key = state.entries[state.entry_index].key;
                    if (boundaryKeyAdmission) boundaryKeyAdmission(key.size());
                    else state.previous_leaf_last_key_credit.set(utils::detail::addStandardHeapBytes(
                        utils::detail::standardContainerProxyBytes(),
                        utils::detail::standardAllocatorRequestBytes(key.size(), alignof(std::byte))));
                    try {
                        // Construct in its final optional: moving a temporary
                        // would allocate another empty proxy on debug MSVC STL.
                        state.previous_leaf_last_key.emplace(key);
                    } catch (...) {
                        if (!boundaryKeyAdmission) state.previous_leaf_last_key_credit.set(0U);
                        throw;
                    }
                    if (boundaryKeyAdmission) boundaryKeyAdmission(state.previous_leaf_last_key->capacity());
                    else state.previous_leaf_last_key_credit.set(
                        utils::detail::standardVectorHeapBufferBytes(*state.previous_leaf_last_key));
                }
				BTreePageEntry result = std::move(state.entries[state.entry_index]);
                ++state.entry_index;
				++state.emitted;
				return result;
			}
			if (!state.leaf.has_value() || !state.leaf->header().right_sibling.isValid()) {
				state.exhausted = true;
				return std::nullopt;
			}

			const BTreePageHeader header = state.leaf->header();
			utils::OwnedStdVector<BTreePageEntry> siblingEntries;
			BTreePage sibling = pages_.readDecoded(*state.snapshot, header.right_sibling, siblingEntries,
																	 BufferPoolReadHint::BPRH_SEQUENTIAL);
			validateIndexPage(sibling, index_id_);
			const BTreePageHeader siblingHeader = sibling.header();
			const uint8_t invalidSibling = static_cast<uint8_t>(siblingHeader.kind != BTreeNodeKind::BPN_LEAF) |
										   static_cast<uint8_t>(siblingHeader.level != 0U) |
										   static_cast<uint8_t>(siblingHeader.leaf_value_layout != leaf_value_layout_) |
										   static_cast<uint8_t>(siblingHeader.left_sibling != header.page_id);
			if (invalidSibling != 0U) {
				throwBTreeIndexError(FormatError::FERR_INVALID_HEADER,
								 "relocatable B+Tree leaf sibling link is not reciprocal");
			}
			if (siblingEntries.empty()) {
				throwBTreeIndexError(FormatError::FERR_INVALID_HEADER,
								 "relocatable B+Tree linked non-root leaf is empty");
			}
			if (state.previous_leaf_last_key.has_value() &&
				!lessKey(*state.previous_leaf_last_key, siblingEntries.front().key)) {
				throwBTreeIndexError(FormatError::FERR_INVALID_HEADER,
								 "relocatable B+Tree leaf siblings are not globally ordered");
			}
			const LeafRoute successor =
					advanceToExpectedSiblingLeaf(*state.snapshot, state.traversal, state.cancellation);
			if (siblingHeader.page_id != successor.page_id) {
				throwBTreeIndexError(FormatError::FERR_INVALID_HEADER,
								 "relocatable B+Tree sibling is outside the routed leaf sequence");
			}
			for (const BTreePageEntry &entry: siblingEntries) {
				const uint8_t outsideRange =
						static_cast<uint8_t>(successor.lower_inclusive.has_value() &&
													 lessKey(entry.key, *successor.lower_inclusive)) |
						static_cast<uint8_t>(successor.upper_exclusive.has_value() &&
													 !lessKey(entry.key, *successor.upper_exclusive));
				if (outsideRange != 0U) {
					throwBTreeIndexError(FormatError::FERR_INVALID_HEADER,
													 "relocatable B+Tree sibling key violates its routed separator range");
				}
			}
			state.previous_leaf_last_key.reset();
            state.previous_leaf_last_key_credit.set(0U);
			state.leaf.emplace(std::move(sibling));
			state.entries = std::move(siblingEntries);
			state.entry_index = 0U;
			if (boundaryKeyAdmission) boundaryKeyAdmission(0U);
		}
	}

	OrderedBTreeEntrySource BTreeIndex::orderedSource(const MetadataSnapshotPin &snapshot, size_t maximumEntries,
																	 size_t cursorBatchEntries) const {
        utils::OwnedAllocationScope allocation(utils::OwnedAllocationScope::active() ? utils::OwnedAllocationScope::owner()
            : (snapshot.readContext() ? snapshot.readContext()->memoryOwner() : nullptr));
		if (!snapshot.isValid() || maximumEntries == 0U) {
			throwBTreeIndexError(FormatError::FERR_INVALID_HEADER,
									 "relocatable B+Tree ordered source requires a live snapshot and finite entry bound");
		}
		const size_t batchEntries = cursorBatchEntries == 0U
				? std::min(DEFAULT_CURSOR_BATCH_ENTRIES, maximumEntries)
																				 : cursorBatchEntries;
		if (batchEntries == 0U || batchEntries > maximumEntries) {
			throwBTreeIndexError(FormatError::FERR_INVALID_HEADER,
									 "relocatable B+Tree ordered source has an invalid cursor batch bound");
		}
		auto index = utils::makeSharedOwned<BTreeIndex>(index_id_, pages_, metadata_, leaf_value_layout_, root_binding_);
		return [index = std::move(index), snapshot = &snapshot, maximumEntries] {
            utils::OwnedAllocationScope allocation(utils::OwnedAllocationScope::active() ? utils::OwnedAllocationScope::owner()
                : (snapshot->readContext() ? snapshot->readContext()->memoryOwner() : nullptr));
			struct Cursor final {
				std::shared_ptr<BTreeIndex> index;
				const MetadataSnapshotPin *snapshot = nullptr;
				size_t maximum_entries = 0U;
				std::optional<BTreeIndex::RangeCursor> range;

				[[nodiscard]] std::optional<BTreePageEntry> next() {
					if (!range.has_value()) {
						range.emplace(index->openRangeCursor(*snapshot, std::nullopt, maximum_entries,
																		 BufferPoolReadHint::BPRH_SEQUENTIAL));
					}
					return range->next();
				}
			};
			auto cursor = utils::makeSharedOwned<Cursor>(Cursor{index, snapshot, maximumEntries, std::nullopt});
			return [cursor] { return cursor->next(); };
		};
	}

} // namespace graph::storage
