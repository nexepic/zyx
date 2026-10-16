/**
 * @file InitialBTreeBuilder.cpp
 * @brief Initial immutable B+Tree planning and materialization implementation.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/index/InitialBTreeBuilder.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace graph::storage {

	namespace {

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		[[nodiscard]] bool lessKey(std::span<const std::byte> left, std::span<const std::byte> right) {
			return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end());
		}

		[[nodiscard]] PageOwnerHeader probeOwner(uint64_t indexId, BTreeNodeKind kind, uint16_t level) {
			static_cast<void>(kind);
			static_cast<void>(level);
			return {PageId(1U),
					indexId,
					PageOwnerKind::POK_INDEX,
					0U,
					RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
					Generation(1U),
					LogSequenceNumber(1U)};
		}

		[[nodiscard]] bool isLeafValueLayout(BTreeLeafValueLayout layout) {
			return layout == BTreeLeafValueLayout::BLVL_U64 || layout == BTreeLeafValueLayout::BLVL_EMPTY;
		}

		[[nodiscard]] BTreePage measuringPage(uint64_t indexId, BTreeNodeKind kind, uint16_t level,
											  BTreeLeafValueLayout leafValueLayout) {
			const PageOwnerHeader owner = probeOwner(indexId, kind, level);
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
				header.leftmost_child = PageId(1U);
			}
			return BTreePage::create(owner, header);
		}

		// The planner only appends ordered entries. Derive the exact next encoded
		// size from the last/new pair, with a singleton at each restart boundary.
		// No temporary packed pages or repeated scans of growing prefixes.
		class AppendSizer {
		public:
			void reset() { bytes_ = RSS_BTREE_PACKED_HEADER_BYTES; }
			[[nodiscard]] bool appendFits(BTreeNodeKind kind, std::span<const BTreePageEntry> entries,
												 BTreeLeafValueLayout layout) {
				static_assert(RSS_BTREE_PACKED_RESTART_INTERVAL > 1U);
				const size_t ordinal = entries.size() - 1U;
				size_t increment;
				if (ordinal % RSS_BTREE_PACKED_RESTART_INTERVAL == 0U) {
					increment = PackedBTreePageCodec::encodedSize(kind, entries.last(1U), layout) - RSS_BTREE_PACKED_HEADER_BYTES;
				} else {
					const auto pair = entries.last(2U);
					increment = PackedBTreePageCodec::encodedSize(kind, pair, layout) -
						PackedBTreePageCodec::encodedSize(kind, pair.first(1U), layout);
				}
				if (increment > std::numeric_limits<size_t>::max() - bytes_) {
					fail(FormatError::FERR_ARITHMETIC_OVERFLOW, "relocatable initial B+Tree append size overflowed");
				}
				bytes_ += increment;
				constexpr size_t capacity = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES - RSS_SLOTTED_PAGE_HEADER_BYTES -
						2U * RSS_SLOTTED_PAGE_SLOT_BYTES - RSS_BTREE_PAGE_HEADER_BYTES;
				return bytes_ <= capacity;
			}
		private:
			size_t bytes_ = RSS_BTREE_PACKED_HEADER_BYTES;
		};

		void appendNode(InitialBTreePlan &plan, InitialBTreePlanNode node, InitialBTreeLeafStorage storage) {
            if (node.level == 0U && storage == InitialBTreeLeafStorage::IBLS_PACKED) {
                node.packed_leaf_entries = PackedBTreePageCodec::encodeOwned(
                    BTreeNodeKind::BPN_LEAF, node.leaf_entries, plan.leaf_value_layout);
                node.leaf_entries = {};
            }
            // Finish a leaf with exact backing rather than retaining geometric
            // spare capacity for every page in a large replacement tree.
            if (node.leaf_entries.size() != node.leaf_entries.capacity()) {
                utils::OwnedStdVector<BTreePageEntry> exact;
                exact.reserve(node.leaf_entries.size());
                for (auto &entry : node.leaf_entries) exact.push_back(std::move(entry));
                node.leaf_entries.swap(exact);
            }
            plan.nodes.push_back(std::move(node));
        }

		void validateMaterializationPlan(uint64_t indexId, const InitialBTreePlan &plan,
										 std::span<const PageId> pageIds, std::span<const Generation> generations,
										 LogSequenceNumber checkpointLsn) {
			const uint8_t invalid = static_cast<uint8_t>(indexId == 0U) | static_cast<uint8_t>(plan.nodes.empty()) |
									static_cast<uint8_t>(!isLeafValueLayout(plan.leaf_value_layout)) |
									static_cast<uint8_t>(plan.leaf_count == 0U) |
									static_cast<uint8_t>(plan.leaf_count > plan.nodes.size()) |
									static_cast<uint8_t>(plan.root_ordinal >= plan.nodes.size()) |
									static_cast<uint8_t>(pageIds.size() != plan.nodes.size()) |
									static_cast<uint8_t>(generations.size() != plan.nodes.size()) |
									static_cast<uint8_t>(!checkpointLsn.isValid());
			if (invalid != 0U) {
				fail(FormatError::FERR_INVALID_HEADER, "relocatable initial B+Tree materialization has an invalid "
													   "plan, identity assignment, or checkpoint");
			}
			for (size_t ordinal = 0U; ordinal < plan.nodes.size(); ++ordinal) {
				const InitialBTreePlanNode &node = plan.nodes[ordinal];
				if (!pageIds[ordinal].isValid() || !generations[ordinal].isValid()) {
					fail(FormatError::FERR_INVALID_HEADER,
						 "relocatable initial B+Tree materialization has an invalid Page identity");
				}
				if (ordinal < plan.leaf_count) {
                    std::optional<utils::OwnedStdVector<BTreePageEntry>> decoded;
                    if (!node.packed_leaf_entries.empty()) {
                        if (!node.leaf_entries.empty())
                            fail(FormatError::FERR_INVALID_STATE, "initial B+Tree leaf has mixed private representations");
                        decoded.emplace(PackedBTreePageCodec::decode(BTreeNodeKind::BPN_LEAF,
                            node.packed_leaf_entries, plan.leaf_value_layout));
                    }
                    const auto &entries = decoded ? *decoded : node.leaf_entries;
					if (node.level != 0U || entries.empty() ||
						!std::ranges::equal(node.first_key, entries.front().key)) {
						fail(FormatError::FERR_INVALID_STATE, "relocatable initial B+Tree leaf plan is malformed");
					}
					continue;
				}
				if (node.level == 0U || !node.packed_leaf_entries.empty() || node.children.empty() || node.children.front() >= ordinal) {
					fail(FormatError::FERR_INVALID_STATE, "relocatable initial B+Tree internal plan is malformed");
				}
				const InitialBTreePlanNode &leftmost = plan.nodes[node.children.front()];
				if (leftmost.first_key.empty() || !std::ranges::equal(node.first_key, leftmost.first_key)) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable initial B+Tree internal plan has an inconsistent first key");
				}
				const std::vector<std::byte> *previousKey = &leftmost.first_key;
				for (size_t childIndex = 1U; childIndex < node.children.size(); ++childIndex) {
					const size_t childOrdinal = node.children[childIndex];
					if (childOrdinal >= ordinal) {
						fail(FormatError::FERR_INVALID_STATE,
							 "relocatable initial B+Tree internal child plan is malformed");
					}
					const InitialBTreePlanNode &child = plan.nodes[childOrdinal];
					if (child.first_key.empty() || !lessKey(*previousKey, child.first_key)) {
						fail(FormatError::FERR_INVALID_STATE,
							 "relocatable initial B+Tree internal child plan is malformed");
					}
					previousKey = &child.first_key;
				}
			}
		}

	} // namespace

	InitialBTreePlan InitialBTreeBuilder::plan(uint64_t indexId, std::span<const BTreePageEntry> entries,
											   BTreeLeafValueLayout leafValueLayout) {
		if (indexId == 0U || entries.empty() || !isLeafValueLayout(leafValueLayout)) {
			fail(FormatError::FERR_INVALID_HEADER, "relocatable initial B+Tree plan requires an Index and entries");
		}
        size_t ordinal = 0U;
        return planFromCursor(indexId, [&]() -> std::optional<BTreePageEntry> {
            if (ordinal == entries.size()) return std::nullopt;
            return entries[ordinal++];
        }, leafValueLayout);
    }

    InitialBTreePlan InitialBTreeBuilder::planFromCursor(uint64_t indexId,
            const utils::OwnedFunction<std::optional<BTreePageEntry>()> &next,
            BTreeLeafValueLayout leafValueLayout, InitialBTreeLeafStorage leafStorage) {
        if (indexId == 0U || !next || !isLeafValueLayout(leafValueLayout) ||
            (leafStorage != InitialBTreeLeafStorage::IBLS_ENTRIES && leafStorage != InitialBTreeLeafStorage::IBLS_PACKED))
            fail(FormatError::FERR_INVALID_HEADER, "relocatable initial B+Tree plan requires an Index and entries");

		InitialBTreePlan result;
		result.leaf_value_layout = leafValueLayout;
		static_cast<void>(measuringPage(indexId, BTreeNodeKind::BPN_LEAF, 0U, leafValueLayout));
		InitialBTreePlanNode node;
		AppendSizer leafSizer;
		std::vector<size_t> levelNodes;
		for (auto input = next(); input; input = next()) {
            BTreePageEntry &entry = *input;
			const bool invalidValue = leafValueLayout == BTreeLeafValueLayout::BLVL_U64 ? entry.value_or_child == 0U
																						: entry.value_or_child != 0U;
			if (entry.key.empty() || invalidValue ||
				(!node.leaf_entries.empty() && !lessKey(node.leaf_entries.back().key, entry.key))) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable initial B+Tree entries have an invalid leaf value, key, or order");
			}
			node.leaf_entries.push_back(std::move(entry));
			if (!leafSizer.appendFits(BTreeNodeKind::BPN_LEAF, node.leaf_entries, leafValueLayout)) {
				BTreePageEntry overflow = std::move(node.leaf_entries.back());
                node.leaf_entries.pop_back();
				if (node.leaf_entries.empty()) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable initial B+Tree entry cannot fit in an empty leaf");
				}
				node.first_key = node.leaf_entries.front().key;
				appendNode(result, std::move(node), leafStorage);
				levelNodes.push_back(result.nodes.size() - 1U);
				node = {};
				leafSizer.reset();
				node.leaf_entries.push_back(std::move(overflow));
				if (!leafSizer.appendFits(BTreeNodeKind::BPN_LEAF, node.leaf_entries, leafValueLayout)) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable initial B+Tree entry cannot fit in an empty leaf");
				}
			}
		}
        if (node.leaf_entries.empty())
            fail(FormatError::FERR_INVALID_HEADER, "relocatable initial B+Tree plan requires an Index and entries");
		node.first_key = node.leaf_entries.front().key;
		appendNode(result, std::move(node), leafStorage);
		levelNodes.push_back(result.nodes.size() - 1U);
		result.leaf_count = levelNodes.size();

		uint16_t level = 0U;
		while (levelNodes.size() > 1U) {
			++level;
			std::vector<size_t> nextLevel;
			static_cast<void>(measuringPage(indexId, BTreeNodeKind::BPN_INTERNAL, level, BTreeLeafValueLayout::BLVL_U64));
			utils::OwnedStdVector<BTreePageEntry> separators;
			AppendSizer parentSizer;
			InitialBTreePlanNode parentNode;
			parentNode.level = level;
			for (const size_t childOrdinal: levelNodes) {
				const InitialBTreePlanNode &child = result.nodes[childOrdinal];
				const std::vector<std::byte> childFirstKey = child.first_key;
				if (parentNode.children.empty()) {
					parentNode.first_key = childFirstKey;
					parentNode.children.push_back(childOrdinal);
					continue;
				}
				// Use the maximum varint width during planning. Metadata-assigned
				// logical Page IDs are not guaranteed to fit in the one-byte test ID.
				separators.push_back({childFirstKey, std::numeric_limits<uint64_t>::max()});
				if (!parentSizer.appendFits(BTreeNodeKind::BPN_INTERNAL, separators, BTreeLeafValueLayout::BLVL_U64)) {
					appendNode(result, std::move(parentNode), leafStorage);
					nextLevel.push_back(result.nodes.size() - 1U);
					separators.clear();
					parentSizer.reset();
					parentNode = {};
					parentNode.level = level;
					parentNode.first_key = childFirstKey;
					parentNode.children.push_back(childOrdinal);
					continue;
				}
				parentNode.children.push_back(childOrdinal);
			}
			appendNode(result, std::move(parentNode), leafStorage);
			nextLevel.push_back(result.nodes.size() - 1U);
			levelNodes = std::move(nextLevel);
		}
		result.root_ordinal = levelNodes.front();
		return result;
	}

	std::vector<BTreePage> InitialBTreeBuilder::materialize(uint64_t indexId, const InitialBTreePlan &plan,
															std::span<const PageId> pageIds,
															std::span<const Generation> generations,
															LogSequenceNumber checkpointLsn) {
		validateMaterializationPlan(indexId, plan, pageIds, generations, checkpointLsn);
		std::vector<BTreePage> pages;
		pages.reserve(plan.nodes.size());
		for (size_t ordinal = 0U; ordinal < plan.nodes.size(); ++ordinal) {
			const InitialBTreePlanNode &node = plan.nodes[ordinal];
			BTreePageHeader header;
			header.index_id = indexId;
			header.page_id = pageIds[ordinal];
			header.generation = generations[ordinal];
			header.level = node.level;
			if (ordinal < plan.leaf_count) {
				header.left_sibling = ordinal == 0U ? PageId{} : pageIds[ordinal - 1U];
				header.right_sibling = ordinal + 1U == plan.leaf_count ? PageId{} : pageIds[ordinal + 1U];
				header.leaf_value_layout = plan.leaf_value_layout;
                std::optional<utils::OwnedStdVector<BTreePageEntry>> decoded;
                if (!node.packed_leaf_entries.empty()) decoded.emplace(PackedBTreePageCodec::decode(
                    BTreeNodeKind::BPN_LEAF, node.packed_leaf_entries, plan.leaf_value_layout));
                const auto &entries = decoded ? *decoded : node.leaf_entries;
				pages.push_back(BTreePage::createWithEntries({header.page_id, indexId, PageOwnerKind::POK_INDEX, 0U,
															  RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, header.generation,
															  checkpointLsn},
															 header, entries));
				continue;
			}
			header.kind = BTreeNodeKind::BPN_INTERNAL;
			header.leftmost_child = pageIds[node.children.front()];
			utils::OwnedStdVector<BTreePageEntry> entries;
			entries.reserve(node.children.size() - 1U);
			for (size_t childIndex = 1U; childIndex < node.children.size(); ++childIndex) {
				const size_t childOrdinal = node.children[childIndex];
				entries.push_back({plan.nodes[childOrdinal].first_key, pageIds[childOrdinal].value()});
			}
			pages.push_back(
					BTreePage::createWithEntries({header.page_id, indexId, PageOwnerKind::POK_INDEX, 0U,
												  RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, header.generation, checkpointLsn},
												 header, entries));
		}
		return pages;
	}

} // namespace graph::storage
