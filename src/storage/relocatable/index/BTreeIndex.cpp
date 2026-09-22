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

	struct BTreeIndex::RangeCursor::State {
		const BTreeIndex *index = nullptr;
		const MetadataSnapshotPin *snapshot = nullptr;
		size_t maximum_entries = 0U;
		size_t emitted = 0U;
		bool exhausted = false;
		std::optional<BTreePage> leaf;
		std::vector<BTreePageEntry> entries;
		size_t entry_index = 0U;
		std::vector<BTreeIndex::TraversalStep> traversal;
		std::optional<std::vector<std::byte>> previous_leaf_last_key;
		const CancellationCheckpoint *cancellation = nullptr;
	};

	BTreeIndex::RangeCursor::RangeCursor(std::unique_ptr<State> state) : state_(std::move(state)) {}
	BTreeIndex::RangeCursor::~RangeCursor() = default;
	BTreeIndex::RangeCursor::RangeCursor(RangeCursor &&) noexcept = default;
	BTreeIndex::RangeCursor &BTreeIndex::RangeCursor::operator=(RangeCursor &&) noexcept = default;

	std::optional<BTreePageEntry> BTreeIndex::RangeCursor::next() {
		if (state_ == nullptr || state_->index == nullptr || state_->snapshot == nullptr) {
			throwBTreeIndexError(FormatError::FERR_INVALID_STATE,
								 "relocatable B+Tree range cursor has no snapshot-bound state");
		}
		return state_->index->nextRangeCursor(*state_);
	}

	BTreeIndex::BTreeIndex(uint64_t indexId, BTreePageStore &pages, RelocatableMetadataStore &metadata,
						   BTreeLeafValueLayout leafValueLayout) :
		index_id_(indexId), pages_(pages), metadata_(metadata), leaf_value_layout_(leafValueLayout) {
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

	std::optional<BTreePage> BTreeIndex::resolveRoot(const MetadataSnapshotPin &snapshot) const {
		const std::optional<IndexRootLocation> root = metadata_.findIndexRootOptional(snapshot, index_id_);
		if (!root.has_value()) {
			return std::nullopt;
		}
		BTreePage page = pages_.read(snapshot, root->root_page_id);
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
											std::vector<TraversalStep> *traversal) const {
		std::optional<std::vector<std::byte>> lowerInclusive;
		std::optional<std::vector<std::byte>> upperExclusive;
		for (;;) {
			if (cancellation != nullptr) {
				cancellation->check();
			}
			validateIndexPage(page, index_id_);
			const BTreePageHeader header = page.header();
			if (header.kind == BTreeNodeKind::BPN_LEAF) {
				if (header.leaf_value_layout != leaf_value_layout_) {
					throwBTreeIndexError(FormatError::FERR_INVALID_STATE,
									 "relocatable B+Tree leaf layout disagrees with its index identity");
				}
				for (const BTreePageEntry &entry: page.entries()) {
					const uint8_t outsideRange =
							static_cast<uint8_t>(lowerInclusive.has_value() && lessKey(entry.key, *lowerInclusive)) |
							static_cast<uint8_t>(upperExclusive.has_value() && !lessKey(entry.key, *upperExclusive));
					if (outsideRange != 0U) {
						throwBTreeIndexError(FormatError::FERR_INVALID_HEADER,
											 "relocatable B+Tree leaf key violates its separator range");
					}
				}
				return page;
			}
			const std::vector<BTreePageEntry> entries = page.entries();
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
			const std::optional<std::vector<std::byte>> parentLowerInclusive = lowerInclusive;
			const std::optional<std::vector<std::byte>> parentUpperExclusive = upperExclusive;
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
										 parentUpperExclusive, header, entries});
			}
			const BufferPoolReadHint childHint = header.level == 1U ? leafReadHint : BufferPoolReadHint::BPRH_NORMAL;
			BTreePage child = pages_.read(snapshot, childId, childHint);
			validateIndexPage(child, index_id_);
			if (child.header().level + 1U != header.level) {
				throwBTreeIndexError(FormatError::FERR_INVALID_HEADER,
									 "relocatable B+Tree child level does not descend by one");
			}
			page = std::move(child);
		}
	}

	BTreeIndex::LeafRoute BTreeIndex::advanceToExpectedSiblingLeaf(
			const MetadataSnapshotPin &snapshot, std::vector<TraversalStep> &traversal,
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
			const std::vector<BTreePageEntry> &parentEntries = ancestor.entries;
			if (ancestor.child_index + 1U >= parentEntries.size() + 1U) {
				continue;
			}

			const size_t nextChildIndex = ancestor.child_index + 1U;
			PageId nextPageId = nextChildIndex == 0U ? parentHeader.leftmost_child
																				 : PageId(parentEntries[nextChildIndex - 1U].value_or_child);
			std::optional<std::vector<std::byte>> lower = ancestor.parent_lower_inclusive;
			std::optional<std::vector<std::byte>> upper = ancestor.parent_upper_exclusive;
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
				const BTreePage internal = pages_.read(snapshot, nextPageId, BufferPoolReadHint::BPRH_NORMAL);
				validateIndexPage(internal, index_id_);
				const BTreePageHeader internalHeader = internal.header();
				if (internalHeader.kind != BTreeNodeKind::BPN_INTERNAL || internalHeader.level != expectedLevel) {
					throwBTreeIndexError(FormatError::FERR_INVALID_HEADER,
									 "relocatable B+Tree routed successor has an invalid internal level");
				}
				const std::vector<BTreePageEntry> entries = internal.entries();
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
										 internalHeader, entries});
				if (!entries.empty()) {
					upper = entries.front().key;
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
		std::optional<BTreePage> root = resolveRoot(snapshot);
		if (!root.has_value()) {
			return std::nullopt;
		}
		const BTreePage leaf =
				descendToLeaf(snapshot, std::move(*root), key, BufferPoolReadHint::BPRH_NORMAL, nullptr);
		const std::vector<BTreePageEntry> entries = leaf.entries();
		const auto position = std::lower_bound(entries.begin(), entries.end(), key,
											   [](const BTreePageEntry &entry, std::span<const std::byte> candidate) {
												   return lessKey(entry.key, candidate);
											   });
		if (position == entries.end() || !std::ranges::equal(position->key, key)) {
			return std::nullopt;
		}
		return position->value_or_child;
	}

	std::vector<BTreePageEntry> BTreeIndex::scan(const MetadataSnapshotPin &snapshot,
																 std::optional<std::span<const std::byte>> lowerBound,
																 size_t maximumEntries, BufferPoolReadHint initialLeafReadHint,
																 const CancellationCheckpoint *cancellation, bool excludeLowerBound) const {
		if (!isSupportedScanLeafReadHint(initialLeafReadHint)) {
			throwBTreeIndexError(FormatError::FERR_INVALID_ENUM,
							 "relocatable B+Tree range scan received an unsupported initial-leaf read hint");
		}
		std::vector<BTreePageEntry> result;
		if (maximumEntries == 0U) {
			return result;
		}
		std::optional<BTreePage> root = resolveRoot(snapshot);
		if (!root.has_value()) {
			return result;
		}
		// Sibling links are an acceleration structure, not a second authority for
		// tree membership. The descent stack proves the expected successor leaf
		// without rereading the root or unrelated routing pages on every hop.
		std::vector<TraversalStep> traversal;
		BTreePage leaf = descendToLeaf(snapshot, std::move(*root), lowerBound, initialLeafReadHint, cancellation,
									  &traversal);
		std::set<PageId> visited;
		std::optional<std::vector<std::byte>> previousLeafLastKey;
		for (;;) {
			if (cancellation != nullptr) {
				cancellation->check();
			}
			const BTreePageHeader header = leaf.header();
			if (!visited.insert(header.page_id).second) {
				throwBTreeIndexError(FormatError::FERR_INVALID_HEADER,
									 "relocatable B+Tree leaf sibling chain contains a cycle");
			}
			const std::vector<BTreePageEntry> entries = leaf.entries();
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
				result.push_back(*position);
			}
			if (result.size() == maximumEntries || !header.right_sibling.isValid()) {
				return result;
			}
			BTreePage sibling = pages_.read(snapshot, header.right_sibling, BufferPoolReadHint::BPRH_SEQUENTIAL);
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
			const std::vector<BTreePageEntry> siblingEntries = sibling.entries();
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
		}
	}

	BTreeIndex::RangeCursor BTreeIndex::openRangeCursor(
			const MetadataSnapshotPin &snapshot, const std::optional<std::span<const std::byte>> lowerBound,
			const size_t maximumEntries, const BufferPoolReadHint initialLeafReadHint,
			const bool excludeLowerBound, const CancellationCheckpoint *cancellation) const {
		if (!snapshot.isValid() || maximumEntries == 0U) {
			throwBTreeIndexError(FormatError::FERR_INVALID_HEADER,
								 "relocatable B+Tree range cursor requires a live snapshot and finite entry bound");
		}
		if (!isSupportedScanLeafReadHint(initialLeafReadHint)) {
			throwBTreeIndexError(FormatError::FERR_INVALID_ENUM,
								 "relocatable B+Tree range cursor received an unsupported initial-leaf read hint");
		}

		auto state = std::make_unique<RangeCursor::State>();
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
		std::optional<BTreePage> root = resolveRoot(snapshot);
		if (!root.has_value()) {
			state->exhausted = true;
			return RangeCursor(std::move(state));
		}
		state->leaf.emplace(descendToLeaf(snapshot, std::move(*root), lowerBound, initialLeafReadHint, cancellation,
														 &state->traversal));
		state->entries = state->leaf->entries();
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

	std::optional<BTreePageEntry> BTreeIndex::nextRangeCursor(RangeCursor::State &state) const {
		if (state.exhausted) {
			return std::nullopt;
		}
		for (;;) {
			if (state.cancellation != nullptr) {
				state.cancellation->check();
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
				BTreePageEntry result = std::move(state.entries[state.entry_index++]);
				state.previous_leaf_last_key = result.key;
				++state.emitted;
				return result;
			}
			if (!state.leaf.has_value() || !state.leaf->header().right_sibling.isValid()) {
				state.exhausted = true;
				return std::nullopt;
			}

			const BTreePageHeader header = state.leaf->header();
			BTreePage sibling = pages_.read(*state.snapshot, header.right_sibling,
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
			std::vector<BTreePageEntry> siblingEntries = sibling.entries();
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
			state.leaf.emplace(std::move(sibling));
			state.entries = std::move(siblingEntries);
			state.entry_index = 0U;
		}
	}

	OrderedBTreeEntrySource BTreeIndex::orderedSource(const MetadataSnapshotPin &snapshot, size_t maximumEntries,
																	 size_t cursorBatchEntries) const {
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
		auto index = std::make_shared<BTreeIndex>(index_id_, pages_, metadata_, leaf_value_layout_);
		return [index = std::move(index), snapshot = &snapshot, maximumEntries] {
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
			auto cursor = std::make_shared<Cursor>(Cursor{index, snapshot, maximumEntries, std::nullopt});
			return [cursor] { return cursor->next(); };
		};
	}

} // namespace graph::storage
