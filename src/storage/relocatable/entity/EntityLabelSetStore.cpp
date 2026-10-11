/**
 * @file EntityLabelSetStore.cpp
 * @brief Atomic durable Node-label storage on native Entity pages.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/entity/EntityLabelSetStore.hpp"

#include "graph/storage/relocatable/entity/EntityPageStore.hpp"

#include <algorithm>
#include <limits>
#include <memory_resource>
#include <type_traits>
#include <array>
#include <ranges>
#include <stdexcept>
#include <unordered_map>

namespace graph::storage {

	namespace {

		// Deallocation only adjusts the counter: user callbacks may throw and must
		// never run while containers unwind after an admission failure.
		class LabelIndexMemoryResource final : public std::pmr::memory_resource {
		public:
			LabelIndexMemoryResource(size_t &retained, const std::function<void(size_t)> &admit)
				: retained_(retained), admit_(admit) {}
		private:
			void *do_allocate(size_t bytes, size_t alignment) override {
				admit_(bytes);
				void *memory = std::pmr::get_default_resource()->allocate(bytes, alignment);
				retained_ += bytes;
				return memory;
			}
			void do_deallocate(void *memory, size_t bytes, size_t alignment) override {
				std::pmr::get_default_resource()->deallocate(memory, bytes, alignment);
				retained_ -= bytes;
			}
			bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override {
				return this == &other;
			}
			size_t &retained_;
			std::function<void(size_t)> admit_;
		};

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		[[nodiscard]] PageOwnerHeader ownerFor(PageId pageId, const CatalogEntry &pool, PageOwnerKind kind,
											   LogSequenceNumber checkpointLsn) {
			PageOwnerHeader owner;
			owner.page_id = pageId;
			owner.owner_id = pool.owner_id;
			owner.kind = kind;
			owner.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
			owner.generation = pool.generation;
			owner.committed_lsn = checkpointLsn;
			return owner;
		}

		[[nodiscard]] bool isSealedEntityPool(const CatalogEntry &pool) {
			return pool.segment_id.isValid() && pool.kind == SegmentKind::SGK_ENTITY_PAGE_POOL && pool.owner_id != 0U &&
				   pool.generation.isValid() && pool.state == SegmentState::SGS_SEALED && pool.committed_lsn.isValid();
		}

		template<typename Contents>
		void validateContents(const Contents &contents, PageId pageId, PageOwnerKind ownerKind,
							  const MetadataSnapshotPin &snapshot) {
			const uint8_t invalid =
					static_cast<uint8_t>(!isSealedEntityPool(contents.pool)) |
					static_cast<uint8_t>(contents.location.page_id != pageId) |
					static_cast<uint8_t>(contents.location.segment_id != contents.pool.segment_id) |
					static_cast<uint8_t>(contents.location.generation != contents.pool.generation) |
					static_cast<uint8_t>(contents.owner.page_id != pageId) |
					static_cast<uint8_t>(contents.owner.owner_id != contents.pool.owner_id) |
					static_cast<uint8_t>(contents.owner.kind != ownerKind) |
					static_cast<uint8_t>((contents.owner.flags & ~RSS_PAGE_OWNER_KNOWN_FLAGS) != 0U) |
					static_cast<uint8_t>(contents.owner.payload_bytes != RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES) |
					static_cast<uint8_t>(contents.owner.generation != contents.location.generation) |
					static_cast<uint8_t>(contents.owner.committed_lsn > snapshot.superblock().checkpoint_lsn);
			if (invalid != 0U) {
				fail(FormatError::FERR_INVALID_STATE,
					 "relocatable Entity label store resolved a page with an invalid owner or pool identity");
			}
		}

		struct StagedReplacement {
			CatalogEntry pool;
			PageLocation expected_location;
			PageOwnerKind owner_kind = PageOwnerKind::POK_NONE;
			LogSequenceNumber expected_owner_lsn{};
			std::vector<std::byte> payload;
			uint32_t free_bytes = 0U;
			bool retire_page = false;
		};

		struct StagedAppend {
			CatalogEntry pool;
			PageId page_id{};
			PageOwnerKind owner_kind = PageOwnerKind::POK_NONE;
			std::vector<std::byte> payload;
			uint32_t free_bytes = 0U;
		};

		struct StagedNewPool {
			EntityLabelSetNewPoolRequest request;
			PageOwnerKind owner_kind = PageOwnerKind::POK_ENTITY_LABEL;
			std::vector<std::byte> payload;
			uint32_t free_bytes = 0U;
		};

	} // namespace

	EntityLabelSetStore::EntityLabelSetStore(std::shared_ptr<StorageIO> io, RelocatableMetadataStore &metadata,
											 SlottedPagePool &pages, SharedPagePoolOpenMode openMode) :
		io_(std::move(io)), allocator_(io_, metadata, openMode), metadata_(metadata), pages_(pages) {
		if (!pages_.usesMetadataStore(metadata_)) {
			throw std::invalid_argument(
					"relocatable Entity label store requires the matching SlottedPagePool metadata");
		}
	}

	std::vector<uint64_t> EntityLabelSetStore::read(const MetadataSnapshotPin &snapshot,
													const EntityLabelOverflowReference &reference,
		const std::function<void(size_t)> &bufferAdmission) const {
		if (!reference.isValid()) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "relocatable Entity label-set read requires a complete stable slot reference");
		}
		if (bufferAdmission) bufferAdmission(RSS_PAGE_SIZE);
		const auto decode = [&](const PageOwnerHeader &owner, std::span<const std::byte> payload, size_t pageBytes) {
			const auto page = SlottedPageReadView::open(owner, payload);
			const SlottedSlotReference slot{reference.page_id, reference.slot_id, reference.slot_generation};
			const std::function<void(size_t)> admit = [&](size_t bytes) {
				if (bytes > std::numeric_limits<size_t>::max() - pageBytes)
					fail(FormatError::FERR_ARITHMETIC_OVERFLOW, "label point buffers overflowed");
				if (bufferAdmission) bufferAdmission(pageBytes + bytes);
			};
			admit(0U);
			return EntityLabelSetCodec::decode(page.read(slot), bufferAdmission ? admit : std::function<void(size_t)>{});
		};
		auto result = [&]() {
			if (auto pinned = allocator_.readPinned(snapshot, reference.page_id)) {
				validateContents(*pinned, reference.page_id, PageOwnerKind::POK_ENTITY_LABEL, snapshot);
				return decode(pinned->owner, pinned->payload(), pinned->pin.bytes().size());
			}
			SharedPagePoolContents contents = allocator_.read(snapshot, reference.page_id);
			validateContents(contents, reference.page_id, PageOwnerKind::POK_ENTITY_LABEL, snapshot);
			return decode(contents.owner, contents.payload, contents.payload.capacity());
		}();
		if (bufferAdmission) bufferAdmission(result.capacity() * sizeof(uint64_t));
		return result;
	}

	std::vector<uint64_t> EntityLabelSetStore::readNodeLabels(const MetadataSnapshotPin &snapshot,
																					 const NodeEntityRecord &node,
		const std::function<void(size_t)> &bufferAdmission) const {
		if (node.inline_label_count > RSS_ENTITY_INLINE_LABEL_CAPACITY) {
			fail(FormatError::FERR_INVALID_STATE,
				 "relocatable Node fixed-label prefix exceeds the inline LabelSet capacity");
		}
		const std::span<const uint64_t> prefix(node.inline_label_ids.data(), node.inline_label_count);
		if (node.label_overflow.isEmpty()) {
			validateNodeLabels(node, prefix);
		} else {
			if (node.inline_label_count != RSS_ENTITY_INLINE_LABEL_CAPACITY)
				fail(FormatError::FERR_INVALID_STATE, "label point overflow requires a complete inline prefix");
			if (!node.label_overflow.isValid())
				fail(FormatError::FERR_INVALID_HEADER, "label point requires a complete overflow reference");
			validateFullLabelSet(prefix);
		}
		std::vector<uint64_t> overflow = node.label_overflow.isEmpty()
			? std::vector<uint64_t>{} : read(snapshot, node.label_overflow, bufferAdmission);
		std::vector<uint64_t> labels;
		const size_t overflowBytes = overflow.capacity() * sizeof(uint64_t);
		if (bufferAdmission) bufferAdmission(overflowBytes + (node.inline_label_count + overflow.size()) * sizeof(uint64_t));
		labels.reserve(node.inline_label_count + overflow.size());
		if (bufferAdmission) bufferAdmission(overflowBytes + labels.capacity() * sizeof(uint64_t));
		const auto inlineEnd = node.inline_label_ids.begin() + node.inline_label_count;
		labels.insert(labels.end(), node.inline_label_ids.begin(), inlineEnd);
		labels.insert(labels.end(), overflow.begin(), overflow.end());
		validateNodeLabels(node, labels);
		std::vector<uint64_t>().swap(overflow);
		if (bufferAdmission) bufferAdmission(labels.capacity() * sizeof(uint64_t));
		return labels;
	}

	bool EntityLabelSetStore::containsNodeLabel(const MetadataSnapshotPin &snapshot,
			const NodeEntityRecord &node, uint64_t labelToken,
		const std::function<void(size_t)> &bufferAdmission) const {
		if (node.inline_label_count > RSS_ENTITY_INLINE_LABEL_CAPACITY)
			fail(FormatError::FERR_INVALID_STATE, "relocatable Node fixed-label prefix exceeds the inline LabelSet capacity");
		if (node.label_overflow.isEmpty()) {
			const std::span<const uint64_t> labels(node.inline_label_ids.data(), node.inline_label_count);
			validateNodeLabels(node, labels);
			return std::ranges::binary_search(labels, labelToken);
		}
		// An inline hit must still authenticate and validate overflow labels.
		auto labels = readNodeLabels(snapshot, node, bufferAdmission);
		const bool found = std::ranges::binary_search(labels, labelToken);
		std::vector<uint64_t>().swap(labels);
		if (bufferAdmission) bufferAdmission(0U);
		return found;
	}

	std::vector<std::vector<uint64_t>> EntityLabelSetStore::readNodeLabelsSorted(
			const MetadataSnapshotPin &snapshot, const std::span<const NodeEntityRecord> nodes,
		const std::function<void(size_t)> &resultBufferAdmission) const {
		if (nodes.size() > std::numeric_limits<size_t>::max() / sizeof(std::vector<uint64_t>))
			fail(FormatError::FERR_ARITHMETIC_OVERFLOW, "label batch result slots overflowed");
		for (const auto &node: nodes) {
			if (node.inline_label_count > RSS_ENTITY_INLINE_LABEL_CAPACITY)
				fail(FormatError::FERR_INVALID_STATE, "relocatable Node fixed-label prefix exceeds the inline LabelSet capacity");
			const std::span<const uint64_t> prefix(node.inline_label_ids.data(), node.inline_label_count);
			if (node.label_overflow.isEmpty()) {
				validateNodeLabels(node, prefix);
			} else {
				if (!node.label_overflow.isValid())
					fail(FormatError::FERR_INVALID_HEADER, "label batch requires a complete overflow reference");
				if (node.inline_label_count != RSS_ENTITY_INLINE_LABEL_CAPACITY)
					fail(FormatError::FERR_INVALID_STATE, "label batch overflow requires a complete inline prefix");
				validateFullLabelSet(prefix);
			}
		}
		if (resultBufferAdmission) resultBufferAdmission(nodes.size() * sizeof(std::vector<uint64_t>));
		std::vector<std::vector<uint64_t>> result(nodes.size());
		size_t retained = result.capacity() * sizeof(std::vector<uint64_t>);
		const auto admit = [&](size_t extra) {
			if (extra > std::numeric_limits<size_t>::max() - retained)
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW, "label batch buffers overflowed");
			if (resultBufferAdmission) resultBufferAdmission(retained + extra);
		};
		admit(0U);
		size_t cacheSlots = 0U;
		size_t heldPageBytes = 0U;
		const auto growCache = [&](auto &array) {
			if (array.size() < array.capacity()) return;
			using Item = typename std::decay_t<decltype(array)>::value_type;
			const size_t oldBytes = array.capacity() * sizeof(Item);
			if (array.capacity() > (std::numeric_limits<size_t>::max() / sizeof(Item) - 1U) / 2U)
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW, "label page cache slots overflowed");
			const size_t next = array.capacity() * 2U + 1U;
			// retained includes old storage, which remains live during reserve.
			admit(next * sizeof(Item));
			array.reserve(next);
			const size_t newBytes = array.capacity() * sizeof(Item);
			admit(newBytes);
			retained += newBytes - oldBytes;
			cacheSlots += newBytes - oldBytes;
			admit(0U);
		};
		LabelIndexMemoryResource indexMemory(retained, admit);
		std::pmr::unordered_map<uint64_t, size_t> loadedPages(&indexMemory);
		// Page storage grows only for unique overflow pages. Moving owners preserves
		// their heap payload or Buffer Pool pin; views never borrow owner objects.
		std::vector<SharedPagePoolContents> pages;
		std::vector<SharedPagePoolPinnedView> pins;
		std::vector<SlottedPageReadView> views;
		for (size_t index = 0U; index < nodes.size(); ++index) {
			const NodeEntityRecord &node = nodes[index];
			std::vector<uint64_t> labels;
			const auto inlineEnd = node.inline_label_ids.begin() + node.inline_label_count;
			if (!node.label_overflow.isEmpty()) {
				const uint64_t pageKey = node.label_overflow.page_id.value();
				auto [pageIt, inserted] = loadedPages.emplace(pageKey, views.size());
				if (inserted) {
					growCache(views);
					admit(RSS_PAGE_SIZE);
					if (auto pinned = allocator_.readPinned(snapshot, node.label_overflow.page_id)) {
						validateContents(*pinned, node.label_overflow.page_id, PageOwnerKind::POK_ENTITY_LABEL, snapshot);
						growCache(pins);
						const size_t pageBytes = pinned->pin.bytes().size();
						admit(pageBytes);
						retained += pageBytes; heldPageBytes += pageBytes;
						pins.emplace_back(std::move(*pinned));
						views.emplace_back(SlottedPageReadView::open(pins.back().owner, pins.back().payload()));
					} else {
						SharedPagePoolContents contents = allocator_.read(snapshot, node.label_overflow.page_id);
						validateContents(contents, node.label_overflow.page_id, PageOwnerKind::POK_ENTITY_LABEL, snapshot);
						growCache(pages);
						const size_t pageBytes = contents.payload.capacity();
						admit(pageBytes);
						retained += pageBytes; heldPageBytes += pageBytes;
						pages.emplace_back(std::move(contents));
						views.emplace_back(SlottedPageReadView::open(pages.back().owner, pages.back().payload));
					}
				}
				const SlottedPageReadView &page = views[pageIt->second];
				const SlottedSlotReference slot{node.label_overflow.page_id, node.label_overflow.slot_id,
						node.label_overflow.slot_generation};
				const std::vector<uint64_t> overflow = EntityLabelSetCodec::decode(page.read(slot), resultBufferAdmission ? admit : std::function<void(size_t)>{});
				const size_t overflowBytes = overflow.capacity() * sizeof(uint64_t);
				admit(overflowBytes + (node.inline_label_count + overflow.size()) * sizeof(uint64_t));
				labels.reserve(node.inline_label_count + overflow.size());
				admit(overflowBytes + labels.capacity() * sizeof(uint64_t));
				labels.insert(labels.end(), node.inline_label_ids.begin(), inlineEnd);
				labels.insert(labels.end(), overflow.begin(), overflow.end());
			} else {
				admit(node.inline_label_count * sizeof(uint64_t));
				labels.reserve(node.inline_label_count);
				admit(labels.capacity() * sizeof(uint64_t));
				labels.insert(labels.end(), node.inline_label_ids.begin(), inlineEnd);
			}
			if (!node.label_overflow.isEmpty()) validateNodeLabels(node, labels);
			admit(labels.capacity() * sizeof(uint64_t));
			retained += labels.capacity() * sizeof(uint64_t);
			result[index] = std::move(labels);
		}
		std::vector<SlottedPageReadView>().swap(views);
		std::vector<SharedPagePoolPinnedView>().swap(pins);
		std::vector<SharedPagePoolContents>().swap(pages);
		{ std::pmr::unordered_map<uint64_t, size_t> empty(&indexMemory); loadedPages.swap(empty); }
		retained -= cacheSlots;
		retained -= heldPageBytes;
		admit(0U);
		return result;
	}

	EntityLabelSetMutationPlan EntityLabelSetStore::planNodeLabels(const MetadataSnapshotPin &snapshot, uint64_t nodeId,
																   std::span<const uint64_t> labels,
																   const EntityLabelSetWritePlan &plan,
																   LogSequenceNumber checkpointLsn) const {
		if (nodeId == 0U || !checkpointLsn.isValid()) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "relocatable Entity label update requires a Node ID and checkpoint LSN");
		}
		validateFullLabelSet(labels);
		validateExcludedPages(plan.excluded_page_ids);
		const EntityPageLocation nodeLocation = metadata_.findNodePageLocation(snapshot, nodeId);
		LoadedNodePage nodePage = loadNodePage(snapshot, nodeLocation.page_id);
		const std::optional<NodeEntityRecord> currentNode =
				nodePage.page.findNode(nodeLocation.slot_id, nodeLocation.slot_generation, nodeId);
		if (!currentNode.has_value()) {
			fail(FormatError::FERR_INVALID_STATE,
				 "relocatable Node Entity Map resolves to a missing Node fixed-record slot");
		}
		static_cast<void>(readNodeLabels(snapshot, *currentNode));

		const EntityLabelOverflowReference previous = currentNode->label_overflow;
		const bool needsOverflow = labels.size() > RSS_ENTITY_INLINE_LABEL_CAPACITY;
		const std::vector<std::byte> encoded =
				needsOverflow ? EntityLabelSetCodec::encode(labels.subspan(RSS_ENTITY_INLINE_LABEL_CAPACITY))
							  : std::vector<std::byte>{};

		std::vector<StagedReplacement> replacements;
		std::vector<StagedAppend> appends;
		std::vector<StagedNewPool> newPools;
		replacements.reserve(3U);
		appends.reserve(1U);
		newPools.reserve(1U);

		EntityLabelOverflowReference overflow;
		bool retiredEmptyLabelPage = false;
		std::optional<LoadedLabelPage> source;
		if (!previous.isEmpty()) {
			source.emplace(loadLabelPage(snapshot, previous.page_id));
			const SlottedSlotReference sourceSlot{previous.page_id, previous.slot_id, previous.slot_generation};
			static_cast<void>(EntityLabelSetCodec::decode(source->page.read(sourceSlot)));
		}

		if (needsOverflow) {
			bool updatedInSource = false;
			if (source.has_value()) {
				const SlottedSlotReference sourceSlot{previous.page_id, previous.slot_id, previous.slot_generation};
				try {
					source->page.overwrite(sourceSlot, encoded);
					overflow = previous;
					updatedInSource = true;
				} catch (const FormatException &error) {
					if (error.error() != FormatError::FERR_INVALID_STATE) {
						throw;
					}
				}
			}

			if (!updatedInSource) {
				bool inserted = false;
				const uint32_t recordBytes = static_cast<uint32_t>(encoded.size());
				const std::vector<SegmentId> pools = eligiblePools(snapshot, plan);
				const std::optional<SlottedPagePlacement> placement =
						pools.empty()
								? std::nullopt
								: pages_.findBestFitPageForRecord(snapshot, pools, PageOwnerKind::POK_ENTITY_LABEL,
																					 recordBytes);
				if (placement.has_value()) {
					// A source overwrite can fail only when its immutable snapshot page
					// cannot fit this record. The same snapshot's best-fit lookup therefore
					// cannot select that source page as an insertion target.
					LoadedLabelPage target = loadLabelPage(snapshot, placement->page_id);
					try {
						const SlottedSlotReference slot = target.page.insert(encoded);
						overflow = asOverflowReference(slot);
						PageOwnerHeader owner = ownerFor(target.page.pageId(), target.contents.pool,
																	 PageOwnerKind::POK_ENTITY_LABEL, checkpointLsn);
						target.page.rebindOwner(owner);
						replacements.push_back({target.contents.pool, target.contents.location,
															PageOwnerKind::POK_ENTITY_LABEL,
															target.contents.owner.committed_lsn, target.page.serialize(),
															target.page.stats().contiguous_free_bytes, false});
						inserted = true;
					} catch (const FormatException &error) {
						if (error.error() != FormatError::FERR_INVALID_STATE) {
							throw;
						}
					}
				}

				if (!inserted && plan.append_page.has_value()) {
					validateAppendRequest(*plan.append_page);
					const CatalogEntry pool = metadata_.findCatalog(snapshot, plan.append_page->segment_id);
					if (isSealedEntityPool(pool) && allocator_.availableFrameCount(pool) != 0U &&
						!metadata_.findPageLocationOptional(snapshot, plan.append_page->page_id).has_value()) {
						PageOwnerHeader owner = ownerFor(plan.append_page->page_id, pool,
														 PageOwnerKind::POK_ENTITY_LABEL, checkpointLsn);
						SlottedPage target = SlottedPage::create(owner);
						const SlottedSlotReference slot = target.insert(encoded);
						overflow = asOverflowReference(slot);
						appends.push_back({pool, plan.append_page->page_id, PageOwnerKind::POK_ENTITY_LABEL,
										   target.serialize(), target.stats().contiguous_free_bytes});
						inserted = true;
					}
				}
				if (!inserted && plan.new_pool.has_value()) {
					validateNewPoolRequest(*plan.new_pool);
					if (metadata_.findPageLocationOptional(snapshot, plan.new_pool->page_id).has_value()) {
						fail(FormatError::FERR_INVALID_STATE,
							 "relocatable Node LabelSet plan reuses a published LabelSet Page ID");
					}
					PageOwnerHeader owner;
					owner.page_id = plan.new_pool->page_id;
					owner.owner_id = plan.new_pool->pool_owner_id;
					owner.kind = PageOwnerKind::POK_ENTITY_LABEL;
					owner.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
					owner.generation = plan.new_pool->generation;
					owner.committed_lsn = checkpointLsn;
					SlottedPage target = SlottedPage::create(owner);
					const SlottedSlotReference slot = target.insert(encoded);
					overflow = asOverflowReference(slot);
					newPools.push_back({*plan.new_pool, PageOwnerKind::POK_ENTITY_LABEL, target.serialize(),
										target.stats().contiguous_free_bytes});
					inserted = true;
				}
				if (!inserted) {
					if (plan.append_page.has_value() && !plan.new_pool.has_value()) {
						fail(FormatError::FERR_INVALID_STATE,
							 "relocatable Entity label append fallback has no current Entity-pool frame");
					}
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable Entity label update has no typed LabelSet page placement or fallback page plan");
				}
			}
		}

		if (source.has_value()) {
			// A failed source overwrite cannot be selected by the immutable best-fit
			// lookup, so no earlier replacement can refer to this source page.
			if (previous != overflow || !needsOverflow) {
				const SlottedSlotReference sourceSlot{previous.page_id, previous.slot_id, previous.slot_generation};
				source->page.erase(sourceSlot);
			}
			if (source->page.stats().live_slot_count == 0U) {
				replacements.push_back({source->contents.pool,
										source->contents.location,
										PageOwnerKind::POK_ENTITY_LABEL,
										source->contents.owner.committed_lsn,
										{},
										0U,
										true});
				retiredEmptyLabelPage = true;
			} else {
				PageOwnerHeader owner = ownerFor(source->page.pageId(), source->contents.pool,
												 PageOwnerKind::POK_ENTITY_LABEL, checkpointLsn);
				source->page.rebindOwner(owner);
				replacements.push_back({source->contents.pool, source->contents.location,
										PageOwnerKind::POK_ENTITY_LABEL, source->contents.owner.committed_lsn,
										source->page.serialize(), source->page.stats().contiguous_free_bytes, false});
			}
		}

		EntityPageAuxiliaryMutationBatch auxiliaryPages;
		auxiliaryPages.replacements.reserve(replacements.size());
		auxiliaryPages.appends.reserve(appends.size());
		auxiliaryPages.new_pools.reserve(newPools.size());
		for (const StagedReplacement &staged: replacements) {
			auxiliaryPages.replacements.push_back({staged.pool, staged.expected_location, staged.owner_kind,
													   staged.expected_owner_lsn, staged.payload, staged.free_bytes,
														staged.retire_page, {}, {}});
		}
		for (const StagedAppend &staged: appends) {
			auxiliaryPages.appends.push_back(
					{staged.pool, staged.page_id, staged.owner_kind, staged.payload, staged.free_bytes});
		}
		for (const StagedNewPool &staged: newPools) {
			auxiliaryPages.new_pools.push_back({staged.request.segment_id, staged.request.pool_owner_id,
												staged.request.generation, staged.request.page_id,
												staged.request.first_slot, staged.request.last_slot, staged.owner_kind,
												staged.payload, staged.free_bytes});
		}
		const NodeEntityRecord replacementNode = makeNodeRecord(*currentNode, labels, overflow);
		auxiliaryPages.node_update_bindings.push_back({nodeId, nodeLocation, replacementNode});
		return {nodeId,	  nodeLocation,			 replacementNode,		   previous,
				overflow, retiredEmptyLabelPage, std::move(auxiliaryPages)};
	}

	EntityLabelSetInitialNodePlan EntityLabelSetStore::planInitialNodeLabels(const MetadataSnapshotPin &snapshot,
																			 const NodeEntityRecord &nodeTemplate,
																			 std::span<const uint64_t> labels,
																			 const EntityLabelSetWritePlan &plan,
																			 LogSequenceNumber checkpointLsn) const {
		if (!checkpointLsn.isValid()) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "relocatable initial Node LabelSet plan requires a valid checkpoint LSN");
		}
		validateFullLabelSet(labels);
		validateExcludedPages(plan.excluded_page_ids);
		if (labels.size() <= RSS_ENTITY_INLINE_LABEL_CAPACITY) {
			return {makeNodeRecord(nodeTemplate, labels, {}), {}, {}};
		}

		const std::vector<std::byte> encoded =
				EntityLabelSetCodec::encode(labels.subspan(RSS_ENTITY_INLINE_LABEL_CAPACITY));
		const std::vector<SegmentId> pools = eligiblePools(snapshot, plan);
		const std::optional<SlottedPagePlacement> placement =
				pools.empty() ? std::nullopt
							  : pages_.findBestFitPageForRecord(snapshot, pools, PageOwnerKind::POK_ENTITY_LABEL,
																		 static_cast<uint32_t>(encoded.size()));
		const bool placementExcluded =
				placement.has_value() && std::ranges::binary_search(plan.excluded_page_ids, placement->page_id);
		if (placement.has_value() && !placementExcluded) {
			LoadedLabelPage target = loadLabelPage(snapshot, placement->page_id);
			const SlottedSlotReference slot = target.page.insert(encoded);
			const EntityLabelOverflowReference overflow = asOverflowReference(slot);
			PageOwnerHeader owner = ownerFor(target.page.pageId(), target.contents.pool,
											 PageOwnerKind::POK_ENTITY_LABEL, checkpointLsn);
			target.page.rebindOwner(owner);
			EntityPageAuxiliaryMutationBatch auxiliary;
			auxiliary.replacements.push_back({target.contents.pool, target.contents.location,
													  PageOwnerKind::POK_ENTITY_LABEL, target.contents.owner.committed_lsn,
													  target.page.serialize(), target.page.stats().contiguous_free_bytes,
														  false, {}, {}});
			return {makeNodeRecord(nodeTemplate, labels, overflow), overflow, std::move(auxiliary)};
		}

		if (plan.append_page.has_value()) {
			validateAppendRequest(*plan.append_page);
			const CatalogEntry pool = metadata_.findCatalog(snapshot, plan.append_page->segment_id);
			const uint8_t unavailable =
					static_cast<uint8_t>(!isSealedEntityPool(pool)) |
					static_cast<uint8_t>(allocator_.availableFrameCount(pool) == 0U) |
					static_cast<uint8_t>(
							metadata_.findPageLocationOptional(snapshot, plan.append_page->page_id).has_value());
			if (unavailable == 0U) {
				const PageOwnerHeader owner =
						ownerFor(plan.append_page->page_id, pool, PageOwnerKind::POK_ENTITY_LABEL, checkpointLsn);
				SlottedPage page = SlottedPage::create(owner);
				const SlottedSlotReference slot = page.insert(encoded);
				const EntityLabelOverflowReference overflow = asOverflowReference(slot);
				EntityPageAuxiliaryMutationBatch auxiliary;
				auxiliary.appends.push_back({pool, plan.append_page->page_id, PageOwnerKind::POK_ENTITY_LABEL,
											 page.serialize(), page.stats().contiguous_free_bytes});
				return {makeNodeRecord(nodeTemplate, labels, overflow), overflow, std::move(auxiliary)};
			}
		}

		if (plan.new_pool.has_value()) {
			validateNewPoolRequest(*plan.new_pool);
			if (metadata_.findPageLocationOptional(snapshot, plan.new_pool->page_id).has_value()) {
				fail(FormatError::FERR_INVALID_STATE,
					 "relocatable initial Node LabelSet plan reuses a published LabelSet Page ID");
			}
			const PageOwnerHeader owner{plan.new_pool->page_id,
										plan.new_pool->pool_owner_id,
										PageOwnerKind::POK_ENTITY_LABEL,
										0U,
										RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
										plan.new_pool->generation,
										checkpointLsn};
			SlottedPage page = SlottedPage::create(owner);
			const SlottedSlotReference slot = page.insert(encoded);
			const EntityLabelOverflowReference overflow = asOverflowReference(slot);
			EntityPageAuxiliaryMutationBatch auxiliary;
			auxiliary.new_pools.push_back({plan.new_pool->segment_id, plan.new_pool->pool_owner_id,
										   plan.new_pool->generation, plan.new_pool->page_id, plan.new_pool->first_slot,
										   plan.new_pool->last_slot, PageOwnerKind::POK_ENTITY_LABEL, page.serialize(),
										   page.stats().contiguous_free_bytes});
			return {makeNodeRecord(nodeTemplate, labels, overflow), overflow, std::move(auxiliary)};
		}

		fail(FormatError::FERR_INVALID_STATE,
			 "relocatable initial Node LabelSet plan requires an append or new-pool overflow placement");
	}

	EntityLabelSetUpdateResult EntityLabelSetStore::updateNodeLabels(const MetadataSnapshotPin &snapshot,
																	 uint64_t nodeId, std::span<const uint64_t> labels,
																	 const EntityLabelSetWritePlan &plan,
																	 LogSequenceNumber checkpointLsn,
																	 const MetadataPublishOptions &options) {
		const EntityLabelSetMutationPlan planned = planNodeLabels(snapshot, nodeId, labels, plan, checkpointLsn);
		const std::array<EntityPageBatchMutation, 1U> mutation{{
				{.page_id = planned.node_location.page_id,
				 .record_kind = EntityPageRecordKind::EPRK_NODE,
				 .node_updates = {{planned.node_id, planned.replacement_node}}},
		}};
		EntityPageStore entities(io_, metadata_, SharedPagePoolOpenMode::SPPOM_DEFER_RECONCILIATION);
		const EntityPageBatchResult published = entities.applyAuxiliaryEntityBatchTransaction(
				snapshot, mutation, {}, {}, planned.auxiliary_pages, checkpointLsn, options);
		return {published.superblock, planned.replacement_node, planned.previous_overflow, planned.current_overflow,
				planned.retired_empty_label_page};
	}

	EntityLabelSetStore::LoadedNodePage EntityLabelSetStore::loadNodePage(const MetadataSnapshotPin &snapshot,
																  PageId pageId) const {
		SharedPagePoolContents contents = allocator_.read(snapshot, pageId);
		validateContents(contents, pageId, PageOwnerKind::POK_ENTITY, snapshot);
		EntityPage page = EntityPage::open(contents.owner, contents.payload);
		if (page.header().record_kind != EntityPageRecordKind::EPRK_NODE) {
			fail(FormatError::FERR_INVALID_STATE,
				 "relocatable Node Entity Map resolves to a non-Node fixed Entity page");
		}
		return {std::move(contents), std::move(page)};
	}

	EntityLabelSetStore::LoadedLabelPage EntityLabelSetStore::loadLabelPage(const MetadataSnapshotPin &snapshot,
																PageId pageId) const {
		SharedPagePoolContents contents = allocator_.read(snapshot, pageId);
		validateContents(contents, pageId, PageOwnerKind::POK_ENTITY_LABEL, snapshot);
		SlottedPage page = SlottedPage::open(contents.owner, contents.payload);
		return {std::move(contents), std::move(page)};
	}

	std::vector<SegmentId> EntityLabelSetStore::eligiblePools(const MetadataSnapshotPin &snapshot,
															  const EntityLabelSetWritePlan &plan) const {
		std::vector<SegmentId> pools = plan.candidate_pool_segments;
		if (pools.empty()) {
			for (const CatalogEntry &pool: metadata_.findCurrentEntityPagePools(snapshot)) {
				pools.push_back(pool.segment_id);
			}
		}
		std::ranges::sort(pools);
		const auto duplicate = std::ranges::adjacent_find(pools);
		if (duplicate != pools.end() || std::ranges::any_of(pools, [](SegmentId id) { return !id.isValid(); })) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "relocatable LabelSet placement pools contain an invalid or duplicate Segment ID");
		}
		return pools;
	}

	NodeEntityRecord EntityLabelSetStore::makeNodeRecord(const NodeEntityRecord &current,
														 std::span<const uint64_t> labels,
														 const EntityLabelOverflowReference &overflow) {
		NodeEntityRecord result = current;
		result.inline_label_ids.fill(0U);
		result.inline_label_count = static_cast<uint8_t>(std::min(labels.size(), RSS_ENTITY_INLINE_LABEL_CAPACITY));
		for (size_t index = 0U; index < result.inline_label_count; ++index) {
			result.inline_label_ids[index] = labels[index];
		}
		result.label_overflow = overflow;
		static_cast<void>(NodeEntityRecordCodec::encode(result));
		return result;
	}

	EntityLabelOverflowReference EntityLabelSetStore::asOverflowReference(const SlottedSlotReference &slot) {
		return {slot.page_id, slot.slot_id, slot.generation};
	}

	void EntityLabelSetStore::validateFullLabelSet(std::span<const uint64_t> labels) {
		if (labels.size() > RSS_ENTITY_MAX_LABELS) {
			fail(FormatError::FERR_INVALID_STATE,
				 "relocatable Node label set exceeds the derived single-page LabelSet capacity");
		}
		if (std::ranges::any_of(labels, [](uint64_t label) { return label == 0U; }) ||
			!std::ranges::is_sorted(labels) || std::ranges::adjacent_find(labels) != labels.end()) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "relocatable Node labels require nonzero, strictly ordered unique Token IDs");
		}
	}

	void EntityLabelSetStore::validateNodeLabels(const NodeEntityRecord &node, std::span<const uint64_t> labels) {
		const uint8_t invalid =
				static_cast<uint8_t>(node.label_overflow.isEmpty() && labels.size() != node.inline_label_count) |
				static_cast<uint8_t>(!node.label_overflow.isEmpty() &&
									 node.inline_label_count != RSS_ENTITY_INLINE_LABEL_CAPACITY) |
				static_cast<uint8_t>(!node.label_overflow.isEmpty() &&
									 labels.size() <= RSS_ENTITY_INLINE_LABEL_CAPACITY);
		if (invalid != 0U) {
			fail(FormatError::FERR_INVALID_STATE,
				 "relocatable Node fixed-label prefix and LabelSet overflow reference disagree");
		}
		validateFullLabelSet(labels);
	}

	void EntityLabelSetStore::validateExcludedPages(std::span<const PageId> pageIds) {
		if (std::ranges::any_of(pageIds, [](PageId pageId) { return !pageId.isValid(); }) ||
			!std::ranges::is_sorted(pageIds) || std::ranges::adjacent_find(pageIds) != pageIds.end()) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "relocatable LabelSet plan has invalid or duplicate excluded Page IDs");
		}
	}

	void EntityLabelSetStore::validateAppendRequest(const EntityLabelSetAppendRequest &request) {
		if (!request.segment_id.isValid() || !request.page_id.isValid()) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "relocatable LabelSet append fallback requires a valid pool Segment ID and Page ID");
		}
	}

	void EntityLabelSetStore::validateNewPoolRequest(const EntityLabelSetNewPoolRequest &request) {
		const uint8_t invalid = static_cast<uint8_t>(!request.segment_id.isValid()) |
								static_cast<uint8_t>(request.pool_owner_id == 0U) |
								static_cast<uint8_t>(!request.generation.isValid()) |
								static_cast<uint8_t>(!request.page_id.isValid()) |
								static_cast<uint8_t>(!request.first_slot.isValid()) |
								static_cast<uint8_t>(!request.last_slot.isValid()) |
								static_cast<uint8_t>(request.first_slot > request.last_slot);
		if (invalid != 0U) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "relocatable LabelSet new-pool fallback has an invalid reservation or page identity");
		}
	}

} // namespace graph::storage
