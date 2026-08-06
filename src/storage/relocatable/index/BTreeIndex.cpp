/**
 * @file BTreeIndex.cpp
 * @brief Snapshot-pinned read traversal for isolated relocatable B+Tree pages.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/index/BTreeIndex.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>

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
										BufferPoolReadHint leafReadHint) const {
		std::optional<std::vector<std::byte>> lowerInclusive;
		std::optional<std::vector<std::byte>> upperExclusive;
		for (;;) {
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
			PageId childId = header.leftmost_child;
			if (childIndex != 0U) {
				childId = PageId(entries[childIndex - 1U].value_or_child);
				lowerInclusive = entries[childIndex - 1U].key;
			}
			if (childIndex < entries.size()) {
				upperExclusive = entries[childIndex].key;
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

	std::optional<uint64_t> BTreeIndex::find(const MetadataSnapshotPin &snapshot,
											 std::span<const std::byte> key) const {
		std::optional<BTreePage> root = resolveRoot(snapshot);
		if (!root.has_value()) {
			return std::nullopt;
		}
		const BTreePage leaf = descendToLeaf(snapshot, std::move(*root), key, BufferPoolReadHint::BPRH_NORMAL);
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
												 size_t maximumEntries, BufferPoolReadHint initialLeafReadHint) const {
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
		BTreePage leaf = descendToLeaf(snapshot, std::move(*root), lowerBound, initialLeafReadHint);
		std::set<PageId> visited;
		std::optional<std::vector<std::byte>> previousLeafLastKey;
		for (;;) {
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
				position = std::lower_bound(entries.begin(), entries.end(), *lowerBound,
											[](const BTreePageEntry &entry, std::span<const std::byte> candidate) {
												return lessKey(entry.key, candidate);
											});
			}
			for (; position != entries.end() && result.size() < maximumEntries; ++position) {
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
			leaf = std::move(sibling);
		}
	}

} // namespace graph::storage
