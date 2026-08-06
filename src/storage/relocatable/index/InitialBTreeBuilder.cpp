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

		[[nodiscard]] bool canInsert(BTreePage &page, const BTreePageEntry &entry) {
			try {
				page.insert(entry);
				return true;
			} catch (const FormatException &error) {
				if (error.error() != FormatError::FERR_INVALID_STATE) {
					throw;
				}
				return false;
			}
		}

		void appendNode(InitialBTreePlan &plan, InitialBTreePlanNode node) { plan.nodes.push_back(std::move(node)); }

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
					if (node.level != 0U || node.leaf_entries.empty() ||
						!std::ranges::equal(node.first_key, node.leaf_entries.front().key)) {
						fail(FormatError::FERR_INVALID_STATE, "relocatable initial B+Tree leaf plan is malformed");
					}
					continue;
				}
				if (node.level == 0U || node.children.empty() || node.children.front() >= ordinal) {
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
		InitialBTreePlan result;
		result.leaf_value_layout = leafValueLayout;
		BTreePage leaf = measuringPage(indexId, BTreeNodeKind::BPN_LEAF, 0U, leafValueLayout);
		InitialBTreePlanNode node;
		std::vector<size_t> levelNodes;
		for (const BTreePageEntry &entry: entries) {
			const bool invalidValue = leafValueLayout == BTreeLeafValueLayout::BLVL_U64 ? entry.value_or_child == 0U
																						: entry.value_or_child != 0U;
			if (entry.key.empty() || invalidValue ||
				(!node.leaf_entries.empty() && !lessKey(node.leaf_entries.back().key, entry.key))) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable initial B+Tree entries have an invalid leaf value, key, or order");
			}
			if (!canInsert(leaf, entry)) {
				if (node.leaf_entries.empty()) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable initial B+Tree entry cannot fit in an empty leaf");
				}
				node.first_key = node.leaf_entries.front().key;
				appendNode(result, std::move(node));
				levelNodes.push_back(result.nodes.size() - 1U);
				leaf = measuringPage(indexId, BTreeNodeKind::BPN_LEAF, 0U, leafValueLayout);
				node = {};
				// The fresh measurement page has the same validated layout as the
				// page that rejected this entry only after it contained prior entries.
				leaf.insert(entry);
			}
			node.leaf_entries.push_back(entry);
		}
		node.first_key = node.leaf_entries.front().key;
		appendNode(result, std::move(node));
		levelNodes.push_back(result.nodes.size() - 1U);
		result.leaf_count = levelNodes.size();

		uint16_t level = 0U;
		while (levelNodes.size() > 1U) {
			++level;
			std::vector<size_t> nextLevel;
			BTreePage parent =
					measuringPage(indexId, BTreeNodeKind::BPN_INTERNAL, level, BTreeLeafValueLayout::BLVL_U64);
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
				if (!canInsert(parent, {childFirstKey, PageId(1U).value()})) {
					appendNode(result, std::move(parentNode));
					nextLevel.push_back(result.nodes.size() - 1U);
					parent = measuringPage(indexId, BTreeNodeKind::BPN_INTERNAL, level, BTreeLeafValueLayout::BLVL_U64);
					parentNode = {};
					parentNode.level = level;
					parentNode.first_key = childFirstKey;
					parentNode.children.push_back(childOrdinal);
					continue;
				}
				parentNode.children.push_back(childOrdinal);
			}
			appendNode(result, std::move(parentNode));
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
				pages.push_back(BTreePage::createWithEntries({header.page_id, indexId, PageOwnerKind::POK_INDEX, 0U,
															  RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, header.generation,
															  checkpointLsn},
															 header, node.leaf_entries));
				continue;
			}
			header.kind = BTreeNodeKind::BPN_INTERNAL;
			header.leftmost_child = pageIds[node.children.front()];
			std::vector<BTreePageEntry> entries;
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
