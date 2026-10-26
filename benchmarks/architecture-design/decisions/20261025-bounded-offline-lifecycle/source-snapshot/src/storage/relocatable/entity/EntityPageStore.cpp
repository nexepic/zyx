/**
 * @file EntityPageStore.cpp
 * @brief Snapshot-safe Node and Edge persistence on fixed relocatable pages.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/entity/EntityPageStore.hpp"

#include "graph/storage/relocatable/entity/EntityLabelSet.hpp"
#include "graph/storage/relocatable/entity/NativeReadSession.hpp"
#include "graph/storage/relocatable/property/LargeValueStore.hpp"

#include "graph/debug/PerfTrace.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <unordered_set>
namespace graph::storage {

	namespace {

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		void validatePinnedEntityContents(const MetadataSnapshotPin &snapshot, PageId pageId,
				const SharedPagePoolPinnedView &contents) {
			const uint8_t invalid =
					static_cast<uint8_t>(contents.pool.kind != SegmentKind::SGK_ENTITY_PAGE_POOL) |
					static_cast<uint8_t>(contents.pool.owner_id == 0U) |
					static_cast<uint8_t>(contents.pool.state != SegmentState::SGS_SEALED) |
					static_cast<uint8_t>(contents.location.page_id != pageId) |
					static_cast<uint8_t>(contents.location.segment_id != contents.pool.segment_id) |
					static_cast<uint8_t>(contents.location.generation != contents.pool.generation) |
					static_cast<uint8_t>(contents.owner.page_id != pageId) |
					static_cast<uint8_t>(contents.owner.owner_id != contents.pool.owner_id) |
					static_cast<uint8_t>(contents.owner.kind != PageOwnerKind::POK_ENTITY) |
					static_cast<uint8_t>((contents.owner.flags & ~RSS_PAGE_OWNER_KNOWN_FLAGS) != 0U) |
					static_cast<uint8_t>(contents.owner.payload_bytes != RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES) |
					static_cast<uint8_t>(contents.owner.generation != contents.location.generation) |
					static_cast<uint8_t>(contents.owner.committed_lsn > snapshot.superblock().checkpoint_lsn);
			if (invalid != 0U) {
				fail(FormatError::FERR_INVALID_STATE,
					 "relocatable pinned Entity page disagrees with its pool, owner, or metadata snapshot");
			}
		}

		[[nodiscard]] uint64_t elapsedNanoseconds(const std::chrono::steady_clock::time_point start) {
			return static_cast<uint64_t>(
					std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
		}

		[[nodiscard]] bool isNodePage(EntityPageRecordKind kind) { return kind == EntityPageRecordKind::EPRK_NODE; }

		[[nodiscard]] bool isEntityPageKind(EntityPageRecordKind kind) {
			return kind == EntityPageRecordKind::EPRK_NODE || kind == EntityPageRecordKind::EPRK_EDGE;
		}

		[[nodiscard]] PageLocation unpublishedLocation(PageId pageId, SegmentId segmentId, Generation generation) {
			return {pageId, segmentId, 0U, generation, 0U};
		}

		[[nodiscard]] bool sameCatalogEntry(const CatalogEntry &left, const CatalogEntry &right) {
			const uint8_t mismatch = static_cast<uint8_t>(left.segment_id != right.segment_id) |
									 static_cast<uint8_t>(left.physical_slot != right.physical_slot) |
									 static_cast<uint8_t>(left.kind != right.kind) |
									 static_cast<uint8_t>(left.owner_id != right.owner_id) |
									 static_cast<uint8_t>(left.generation != right.generation) |
									 static_cast<uint8_t>(left.state != right.state) |
									 static_cast<uint8_t>(left.committed_lsn != right.committed_lsn);
			return mismatch == 0U;
		}

		[[nodiscard]] bool isAuxiliaryEntityPoolPageOwnerKind(PageOwnerKind kind) {
			return kind == PageOwnerKind::POK_ENTITY_LABEL;
		}

        std::vector<std::byte> serializeEntityForOwner(const EntityPage &page, const PageOwnerHeader &owner) {
            const auto header = page.header();
            if (header.generation == owner.generation && header.committed_lsn == owner.committed_lsn)
                return page.serialize();
            return page.rebindOwner(owner).serialize();
        }

        std::vector<std::byte> serializeAuxiliaryForOwner(const CatalogEntry &source,
            std::span<const std::byte> payload, const PageOwnerHeader &destination) {
            if (source.generation == destination.generation)
                return {payload.begin(), payload.end()};
            auto previous = destination; previous.generation = source.generation;
            auto page = SlottedPage::open(previous, payload);
            page.rebindOwner(destination);
            return page.serialize();
        }

		[[nodiscard]] PageOwnerHeader auxiliaryOwnerFor(PageId pageId, const CatalogEntry &pool, PageOwnerKind kind,
														LogSequenceNumber checkpointLsn) {
			return {pageId, pool.owner_id, kind, 0U, RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, pool.generation, checkpointLsn};
		}

		void validateAuxiliaryPayload(const CatalogEntry &pool, PageId pageId, PageOwnerKind kind,
									  std::span<const std::byte> payload, uint32_t freeBytes,
									  LogSequenceNumber checkpointLsn) {
			const PageOwnerHeader owner = auxiliaryOwnerFor(pageId, pool, kind, checkpointLsn);
			if (kind == PageOwnerKind::POK_ENTITY_LABEL) {
				const SlottedPage page = SlottedPage::open(owner, payload);
				if (page.stats().contiguous_free_bytes != freeBytes) {
					fail(FormatError::FERR_INVALID_HEADER,
						 "native Entity page batch auxiliary page free-byte count disagrees with its slotted payload");
				}
				for (const SlottedSlotReference &slot: page.liveSlots()) {
					const std::vector<std::byte> record = page.read(slot);
					static_cast<void>(EntityLabelSetCodec::decode(record));
				}
				return;
			}
			fail(FormatError::FERR_INVALID_ENUM, "native Entity page batch has an unsupported auxiliary owner kind"); // all auxiliary batch inputs pass the typed owner-kind validation immediately before this helper.
		}

		struct PropertyLargeValueDerivedStatePlan {
			NativeGraphDerivedStateMutation state;
			NativeGraphDerivedStatePreparer preparer;
		};

		[[nodiscard]] PropertyLargeValueDerivedStatePlan planPropertyLargeValueDerivedState(
				const MetadataSnapshotPin &snapshot, std::span<const EntityPropertyRootMutationPlan> propertyPlans,
				const PropertyCollectionRootStore &propertyRoots,
				const PropertyCollectionRootStoreOptions &propertyOptions, LogSequenceNumber checkpointLsn) {
			PropertyLargeValueDerivedStatePlan result;
			std::vector<NativeGraphLargeValueMutation> newValues;
			for (const EntityPropertyRootMutationPlan &plan: propertyPlans) {
				newValues.insert(newValues.end(), plan.large_values.begin(), plan.large_values.end());
				result.state.reused_large_values.insert(result.state.reused_large_values.end(),
																plan.reused_large_values.begin(), plan.reused_large_values.end());
			}
			std::ranges::sort(result.state.reused_large_values, {}, &LargeValueRef::value_id);
			result.state.reused_large_values.erase(
					std::unique(result.state.reused_large_values.begin(), result.state.reused_large_values.end(),
								[](const LargeValueRef &left, const LargeValueRef &right) { return left == right; }),
					result.state.reused_large_values.end());
			for (size_t ordinal = 1U; ordinal < result.state.reused_large_values.size(); ++ordinal) {
				if (!result.state.reused_large_values[ordinal - 1U].value_id.isValid() ||
					result.state.reused_large_values[ordinal - 1U].value_id ==
							result.state.reused_large_values[ordinal].value_id) {
					fail(FormatError::FERR_INVALID_HEADER,
							 "native Entity page batch reuses one LargeValue identity with conflicting references");
				}
			}
			if (!result.state.reused_large_values.empty() &&
				!result.state.reused_large_values.back().value_id.isValid()) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "native Entity page batch reuses an invalid LargeValue identity");
			}
			if (newValues.empty()) {
				if (!result.state.empty()) {
					result.preparer = [](std::vector<std::vector<IndexTreeMutationPoolBatch>> &batches) {
						if (!batches.empty()) { // reused-only LargeValue state has no index-tree mutation, so the metadata coordinator passes an empty ordinary batch partition.
							fail(FormatError::FERR_INVALID_STATE,
								 "reused LargeValue publication unexpectedly prepared Index pages");
						}
					};
				}
				return result;
			}
			LargeValueStore *largeValues = propertyRoots.largeValueStore();
			if (largeValues == nullptr) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native Entity page batch LargeValue plans require a local persistent content index");
			}
			LargeValueContentIndexPublicationPlan indexPlan = largeValues->planNativeGraphContentIndexMutation(
					snapshot, newValues, propertyOptions.first_slot, propertyOptions.last_slot);
			result.state.index_tree_mutations.push_back(std::move(indexPlan.mutation));
			result.preparer = [preparer = std::move(indexPlan.preparer), checkpointLsn](
									  std::vector<std::vector<IndexTreeMutationPoolBatch>> &batches) {
				if (batches.size() != 1U || !preparer) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native Entity page batch changed its LargeValue content-index materialization plan");
				}
				preparer(batches.front(), checkpointLsn);
			};
			return result;
		}

	} // namespace

	EntityPageStore::EntityPageStore(std::shared_ptr<StorageIO> io, RelocatableMetadataStore &metadata) :
		EntityPageStore(std::move(io), metadata, SharedPagePoolOpenMode::SPPOM_RECONCILE) {}

	EntityPageStore::EntityPageStore(std::shared_ptr<StorageIO> io, RelocatableMetadataStore &metadata,
									 SharedPagePoolOpenMode openMode) :
		allocator_(std::move(io), metadata, openMode), metadata_(metadata) {}

	std::optional<EntityPageInsertResult> EntityPageStore::allocateNode(const EntityPageNewPoolRequest &request,
																		uint64_t nodeId, const NodeEntityRecord &record,
																		LogSequenceNumber checkpointLsn,
																		const MetadataPublishOptions &options) {
		validateRecord(record);
		return allocate(request, nodeId, EntityPageRecordKind::EPRK_NODE, &record, nullptr, checkpointLsn, options);
	}

	std::optional<EntityPageInsertResult> EntityPageStore::allocateEdge(const EntityPageNewPoolRequest &request,
																		uint64_t edgeId, const EdgeEntityRecord &record,
																		LogSequenceNumber checkpointLsn,
																		const MetadataPublishOptions &options) {
		validateRecord(record);
		return allocate(request, edgeId, EntityPageRecordKind::EPRK_EDGE, nullptr, &record, checkpointLsn, options);
	}

	EntityPageInsertResult EntityPageStore::appendNodePage(const EntityPageAppendRequest &request, uint64_t nodeId,
														   const NodeEntityRecord &record,
														   LogSequenceNumber checkpointLsn,
														   const MetadataPublishOptions &options) {
		validateRecord(record);
		return append(request, nodeId, EntityPageRecordKind::EPRK_NODE, &record, nullptr, checkpointLsn, options);
	}

	EntityPageInsertResult EntityPageStore::appendEdgePage(const EntityPageAppendRequest &request, uint64_t edgeId,
														   const EdgeEntityRecord &record,
														   LogSequenceNumber checkpointLsn,
														   const MetadataPublishOptions &options) {
		validateRecord(record);
		return append(request, edgeId, EntityPageRecordKind::EPRK_EDGE, nullptr, &record, checkpointLsn, options);
	}

	EntityPageInsertResult EntityPageStore::insertNode(const MetadataSnapshotPin &snapshot, PageId pageId,
													   uint64_t nodeId, const NodeEntityRecord &record,
													   LogSequenceNumber checkpointLsn,
													   const MetadataPublishOptions &options) {
		validateRecord(record);
		return insert(snapshot, pageId, nodeId, EntityPageRecordKind::EPRK_NODE, &record, nullptr, checkpointLsn,
					  options);
	}

	EntityPageInsertResult EntityPageStore::insertEdge(const MetadataSnapshotPin &snapshot, PageId pageId,
													   uint64_t edgeId, const EdgeEntityRecord &record,
													   LogSequenceNumber checkpointLsn,
													   const MetadataPublishOptions &options) {
		validateRecord(record);
		return insert(snapshot, pageId, edgeId, EntityPageRecordKind::EPRK_EDGE, nullptr, &record, checkpointLsn,
					  options);
	}

	NodeEntityRecord EntityPageStore::readNode(const MetadataSnapshotPin &snapshot, uint64_t nodeId) const {
		if (nodeId == 0U) {
			fail(FormatError::FERR_INVALID_HEADER, "relocatable Node read requires a nonzero Entity ID");
		}
		const EntityPageLocation location = metadata_.findNodePageLocation(snapshot, nodeId);
		return readNodeAt(snapshot, location);
	}

	NodeEntityRecord EntityPageStore::readNodeAt(const MetadataSnapshotPin &snapshot,
														 const EntityPageLocation &location) const {
		if (location.entity_id == 0U || !location.page_id.isValid() || location.slot_generation == 0U) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "relocatable Entity Map Node entry has an invalid logical ID, Page ID, or slot generation");
		}
		ReadPage loaded = loadReadPage(snapshot, location.page_id, EntityPageRecordKind::EPRK_NODE);
		// EntityPage's generation-aware lookup rejects an unavailable slot before
		// returning. An Entity Map lookup is therefore total at this layer.
		const std::optional<NodeEntityRecord> record =
				loaded.page.findNode(location.slot_id, location.slot_generation, location.entity_id);
		return *record;
	}

	std::vector<std::optional<NodeEntityRecord>> EntityPageStore::readNodesSorted(
			const MetadataSnapshotPin &snapshot, const std::span<const uint64_t> nodeIds,
			const std::function<void(size_t)> &retainedBufferAdmission) const {
		if (!metadata_.ownsSnapshot(snapshot)) {
			fail(FormatError::FERR_INVALID_STATE, "relocatable sorted Node read requires this store's active snapshot");
		}
		uint64_t previousId = 0U;
		for (const uint64_t nodeId: nodeIds) {
			if (nodeId == 0U || nodeId <= previousId) {
				fail(FormatError::FERR_INVALID_HEADER,
						 "relocatable sorted Node read requires strictly increasing nonzero IDs");
			}
			previousId = nodeId;
		}
		const auto admit = [&](size_t base, size_t count, size_t width) {
			if (width != 0U && count > (std::numeric_limits<size_t>::max() - base) / width) {
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW, "sorted Node buffer admission size overflowed");
			}
			const size_t bytes = base + count * width;
			if (retainedBufferAdmission) retainedBufferAdmission(bytes);
			return bytes;
		};
		admit(0U, nodeIds.size(), sizeof(std::optional<NodeEntityRecord>));
		std::vector<std::optional<NodeEntityRecord>> result(nodeIds.size());
		if (nodeIds.empty()) {
			return result;
		}
		const bool traceValues = debug::PerfTrace::areValuesEnabled();
		if (traceValues) {
			debug::PerfTrace::addValue("native_graph.read.node_sorted_batch_calls", 1);
			debug::PerfTrace::addValue("native_graph.read.node_sorted_batch_entries",
					static_cast<int64_t>(nodeIds.size()));
			// One ordered multi-lookup opens one Entity-directory range cursor.
			debug::PerfTrace::addValue("native_graph.read.node_sorted_batch_directory_range_calls", 1);
		}
		const size_t resultBytes = admit(0U, result.capacity(), sizeof(std::optional<NodeEntityRecord>));
		admit(resultBytes, nodeIds.size(), sizeof(std::optional<EntityPageLocation>));
		std::vector<std::optional<EntityPageLocation>> locations(nodeIds.size());
		const size_t locationBytes = admit(resultBytes, locations.capacity(), sizeof(std::optional<EntityPageLocation>));
		metadata_.findNodePageLocationsSorted(snapshot, nodeIds, locations);
		std::vector<size_t> present;
		size_t presentCount = 0U;
		for (const auto &location: locations) if (location.has_value()) ++presentCount;
		admit(locationBytes, presentCount, sizeof(size_t));
		present.reserve(presentCount);
		admit(locationBytes, present.capacity(), sizeof(size_t));
		for (size_t index = 0U; index < locations.size(); ++index) {
			if (locations[index].has_value()) {
				present.push_back(index);
			}
		}
		if (traceValues) {
			debug::PerfTrace::addValue("native_graph.read.node_sorted_batch_unique_io_ids",
					static_cast<int64_t>(present.size()));
		}
		std::ranges::sort(present, [&locations](const size_t left, const size_t right) {
			const EntityPageLocation &lhs = *locations[left];
			const EntityPageLocation &rhs = *locations[right];
			return lhs.page_id != rhs.page_id ? lhs.page_id < rhs.page_id : lhs.slot_id < rhs.slot_id;
		});
		std::optional<ReadPage> current;
		PageId currentPage{};
		size_t pinnedPages = 0U;
		for (const size_t index: present) {
			const EntityPageLocation &location = *locations[index];
			if (!current.has_value() || location.page_id != currentPage) {
				current.emplace(loadReadPage(snapshot, location.page_id, EntityPageRecordKind::EPRK_NODE,
														 BufferPoolReadHint::BPRH_SEQUENTIAL));
				currentPage = location.page_id;
				++pinnedPages;
			}
			result[index] = current->page.findNode(location.slot_id, location.slot_generation, location.entity_id);
		}
		if (traceValues) {
			debug::PerfTrace::addValue("native_graph.read.node_sorted_batch_entity_page_pins",
					static_cast<int64_t>(pinnedPages));
		}
		current.reset();
		std::vector<size_t>().swap(present);
		std::vector<std::optional<EntityPageLocation>>().swap(locations);
		admit(0U, result.capacity(), sizeof(std::optional<NodeEntityRecord>));
		return result;
	}

	EdgeEntityRecord EntityPageStore::readEdge(const MetadataSnapshotPin &snapshot, uint64_t edgeId) const {
		if (edgeId == 0U) {
			fail(FormatError::FERR_INVALID_HEADER, "relocatable Edge read requires a nonzero Entity ID");
		}
		const EntityPageLocation location = metadata_.findEdgePageLocation(snapshot, edgeId);
		return readEdgeAt(snapshot, location);
	}

	std::vector<std::optional<EdgeEntityRecord>> EntityPageStore::readEdgesSorted(
			const MetadataSnapshotPin &snapshot, const std::span<const uint64_t> edgeIds,
			const std::function<void(size_t)> &retainedBufferAdmission) const {
		if (!metadata_.ownsSnapshot(snapshot)) {
			fail(FormatError::FERR_INVALID_STATE, "relocatable sorted Edge read requires this store's active snapshot");
		}
		uint64_t previousId = 0U;
		for (const uint64_t edgeId: edgeIds) {
			if (edgeId == 0U || edgeId <= previousId) {
				fail(FormatError::FERR_INVALID_HEADER,
						 "relocatable sorted Edge read requires strictly increasing nonzero IDs");
			}
			previousId = edgeId;
		}
		const auto admit = [&](size_t base, size_t count, size_t width) {
			if (width != 0U && count > (std::numeric_limits<size_t>::max() - base) / width) {
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW, "sorted Edge buffer admission size overflowed");
			}
			const size_t bytes = base + count * width;
			if (retainedBufferAdmission) retainedBufferAdmission(bytes);
			return bytes;
		};
		admit(0U, edgeIds.size(), sizeof(std::optional<EdgeEntityRecord>));
		std::vector<std::optional<EdgeEntityRecord>> result(edgeIds.size());
		if (edgeIds.empty()) {
			return result;
		}
		const bool traceValues = debug::PerfTrace::areValuesEnabled();
		if (traceValues) {
			debug::PerfTrace::addValue("native_graph.read.edge_sorted_batch_calls", 1);
			debug::PerfTrace::addValue("native_graph.read.edge_sorted_batch_entries",
					static_cast<int64_t>(edgeIds.size()));
			// One ordered multi-lookup opens one Entity-directory range cursor.
			debug::PerfTrace::addValue("native_graph.read.edge_sorted_batch_directory_range_calls", 1);
		}
		const size_t resultBytes = admit(0U, result.capacity(), sizeof(std::optional<EdgeEntityRecord>));
		admit(resultBytes, edgeIds.size(), sizeof(std::optional<EntityPageLocation>));
		std::vector<std::optional<EntityPageLocation>> locations(edgeIds.size());
		const size_t locationBytes = admit(resultBytes, locations.capacity(), sizeof(std::optional<EntityPageLocation>));
		metadata_.findEdgePageLocationsSorted(snapshot, edgeIds, locations);
		std::vector<size_t> present;
		size_t presentCount = 0U;
		for (const auto &location: locations) if (location.has_value()) ++presentCount;
		admit(locationBytes, presentCount, sizeof(size_t));
		present.reserve(presentCount);
		admit(locationBytes, present.capacity(), sizeof(size_t));
		for (size_t index = 0U; index < locations.size(); ++index) {
			if (locations[index].has_value()) {
				present.push_back(index);
			}
		}
		if (traceValues) {
			debug::PerfTrace::addValue("native_graph.read.edge_sorted_batch_unique_io_ids",
					static_cast<int64_t>(present.size()));
		}
		std::ranges::sort(present, [&locations](const size_t left, const size_t right) {
			const EntityPageLocation &lhs = *locations[left];
			const EntityPageLocation &rhs = *locations[right];
			return lhs.page_id != rhs.page_id ? lhs.page_id < rhs.page_id : lhs.slot_id < rhs.slot_id;
		});
		std::optional<ReadPage> current;
		PageId currentPage{};
		size_t pinnedPages = 0U;
		for (const size_t index: present) {
			const EntityPageLocation &location = *locations[index];
			if (!current.has_value() || location.page_id != currentPage) {
				current.emplace(loadReadPage(snapshot, location.page_id, EntityPageRecordKind::EPRK_EDGE,
														 BufferPoolReadHint::BPRH_SEQUENTIAL));
				currentPage = location.page_id;
				++pinnedPages;
			}
			result[index] = current->page.findEdge(location.slot_id, location.slot_generation, location.entity_id);
		}
		if (traceValues) {
			debug::PerfTrace::addValue("native_graph.read.edge_sorted_batch_entity_page_pins",
					static_cast<int64_t>(pinnedPages));
		}
		current.reset();
		std::vector<size_t>().swap(present);
		std::vector<std::optional<EntityPageLocation>>().swap(locations);
		admit(0U, result.capacity(), sizeof(std::optional<EdgeEntityRecord>));
		return result;
	}

	SharedPagePoolContents EntityPageStore::readAuxiliaryPage(const MetadataSnapshotPin &snapshot, PageId pageId,
															  PageOwnerKind expectedOwnerKind,
															  BufferPoolReadHint hint) const {
		if (!snapshot.isValid() || !pageId.isValid() || !isAuxiliaryEntityPoolPageOwnerKind(expectedOwnerKind)) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "relocatable auxiliary Entity page read requires a pinned snapshot, Page ID, and typed owner kind");
		}
		SharedPagePoolContents contents = allocator_.read(snapshot, pageId, hint);
		const uint8_t invalid =
				static_cast<uint8_t>(contents.pool.kind != SegmentKind::SGK_ENTITY_PAGE_POOL) |
				static_cast<uint8_t>(contents.pool.owner_id == 0U) |
				static_cast<uint8_t>(contents.pool.state != SegmentState::SGS_SEALED) |
				static_cast<uint8_t>(contents.location.page_id != pageId) |
				static_cast<uint8_t>(contents.location.segment_id != contents.pool.segment_id) |
				static_cast<uint8_t>(contents.location.generation != contents.pool.generation) |
				static_cast<uint8_t>(contents.owner.page_id != pageId) |
				static_cast<uint8_t>(contents.owner.owner_id != contents.pool.owner_id) |
				static_cast<uint8_t>(contents.owner.kind != expectedOwnerKind) |
				static_cast<uint8_t>((contents.owner.flags & ~RSS_PAGE_OWNER_KNOWN_FLAGS) != 0U) |
				static_cast<uint8_t>(contents.owner.payload_bytes != RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES) |
				static_cast<uint8_t>(contents.owner.generation != contents.location.generation) |
				static_cast<uint8_t>(contents.owner.committed_lsn > snapshot.superblock().checkpoint_lsn);
		if (invalid != 0U) {
			fail(FormatError::FERR_INVALID_STATE,
				 "relocatable auxiliary Entity page disagrees with its current pool, owner, or pinned snapshot");
		}
		return contents;
	}

	void EntityPageStore::prefetchAuxiliaryPage(const MetadataSnapshotPin &snapshot, PageId pageId,
												BufferPoolReadHint admissionHint) const {
		if (!snapshot.isValid() || !pageId.isValid()) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "relocatable auxiliary Entity page prefetch requires a pinned snapshot and Page ID");
		}
		const std::array<PageId, 1U> pages{pageId};
		allocator_.prefetch(snapshot, pages, admissionHint);
	}

	EdgeEntityRecord EntityPageStore::readEdgeAt(const MetadataSnapshotPin &snapshot,
												 const EntityPageLocation &location) const {
		if (location.entity_id == 0U || !location.page_id.isValid() || location.slot_generation == 0U) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "relocatable Entity Map Edge entry has an invalid logical ID, Page ID, or slot generation");
		}
		ReadPage loaded = loadReadPage(snapshot, location.page_id, EntityPageRecordKind::EPRK_EDGE);
		const std::optional<EdgeEntityRecord> record =
				loaded.page.findEdge(location.slot_id, location.slot_generation, location.entity_id);
		return *record;
	}

	bool EntityPageStore::visitNodesUntil(
			const MetadataSnapshotPin &snapshot, size_t maximumRecords,
			const std::function<bool(uint64_t nodeId, const NodeEntityRecord &)> &visitor,
			NativeReadSession *session) const {
		if (maximumRecords == 0U || !visitor) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "relocatable native Node scan requires a nonzero record bound and visitor");
		}
		debug::ScopedPerfTimer timer("native_graph.read.node_page_scan");
		const bool tracingValues = debug::PerfTrace::areValuesEnabled();
		std::optional<NativeReadSession> localSession;
		NativeReadSession &activeSession = selectNativeReadSession(snapshot, session, localSession);
		size_t visited = 0U;
		std::optional<ReadPage> currentPage;
		PageId currentPageId;
		return metadata_.visitNodePageLocationsUntil(snapshot, [&](const EntityPageLocation &entry) {
			if (visited == maximumRecords) {
				return false;
			}
			if (entry.entity_id == 0U || !entry.page_id.isValid() || entry.slot_generation == 0U) {
				fail(FormatError::FERR_INVALID_STATE,
					 "relocatable native Node scan contains an invalid typed Entity Map entry");
			}
			const bool newPage = !currentPage.has_value() || currentPageId != entry.page_id;
			ReadPage &loaded = loadScanReadPage(snapshot, entry.page_id, EntityPageRecordKind::EPRK_NODE,
													 currentPage, currentPageId);
			if (newPage && tracingValues) {
				debug::PerfTrace::addValue("native_graph.read.node_page_scan.pages", 1);
			}
			const std::optional<NodeEntityRecord> record =
					loaded.page.findNode(entry.slot_id, entry.slot_generation, entry.entity_id);
			if (!record.has_value()) {
				fail(FormatError::FERR_INVALID_STATE,
					 "relocatable native Node scan could not resolve its typed Entity Map slot");
			}
			++visited;
			if (tracingValues) {
				debug::PerfTrace::addValue("native_graph.read.node_page_scan.records", 1);
			}
			return visitor(entry.entity_id, *record) && visited < maximumRecords;
		});
	}

	bool EntityPageStore::visitEdgesUntil(
			const MetadataSnapshotPin &snapshot, size_t maximumRecords,
			const std::function<bool(uint64_t edgeId, const EdgeEntityRecord &)> &visitor,
			NativeReadSession *session) const {
		if (maximumRecords == 0U || !visitor) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "relocatable native Edge scan requires a nonzero record bound and visitor");
		}
		debug::ScopedPerfTimer timer("native_graph.read.edge_page_scan");
		const bool tracing = debug::PerfTrace::isEnabled();
		const bool tracingValues = debug::PerfTrace::areValuesEnabled();
		uint64_t entityRecordNanoseconds = 0U;
		uint64_t visitorNanoseconds = 0U;
		uint64_t records = 0U;
		std::optional<NativeReadSession> localSession;
		NativeReadSession &activeSession = selectNativeReadSession(snapshot, session, localSession);
		size_t visited = 0U;
		std::optional<ReadPage> currentPage;
		PageId currentPageId;
		const bool exhausted = metadata_.visitEdgePageLocationsUntil(snapshot, [&](const EntityPageLocation &entry) {
			if (visited == maximumRecords) {
				return false;
			}
			if (entry.entity_id == 0U || !entry.page_id.isValid() || entry.slot_generation == 0U) {
				fail(FormatError::FERR_INVALID_STATE,
					 "relocatable native Edge scan contains an invalid typed Entity Map entry");
			}
			const bool newPage = !currentPage.has_value() || currentPageId != entry.page_id;
			ReadPage &loaded = loadScanReadPage(snapshot, entry.page_id, EntityPageRecordKind::EPRK_EDGE,
													 currentPage, currentPageId);
			if (tracingValues && newPage) {
				// Count each typed Entity page once; loadScanReadPage keeps it resident
				// while consecutive Entity Map entries refer to the same page.
				debug::PerfTrace::addValue("native_graph.read.edge_page_scan.pages", 1);
			}
			const auto recordStart = tracing ? std::chrono::steady_clock::now()
														 : std::chrono::steady_clock::time_point{};
			const std::optional<EdgeEntityRecord> record =
					loaded.page.findEdge(entry.slot_id, entry.slot_generation, entry.entity_id);
			if (!record.has_value()) {
				fail(FormatError::FERR_INVALID_STATE,
					 "relocatable native Edge scan could not resolve its typed Entity Map slot");
			}
			++visited;
			if (tracingValues) {
				debug::PerfTrace::addValue("native_graph.read.edge_page_scan.records", 1);
			}
			const auto visitorStart = tracing ? std::chrono::steady_clock::now()
														 : std::chrono::steady_clock::time_point{};
			const bool keepGoing = visitor(entry.entity_id, *record);
			if (tracing) {
				entityRecordNanoseconds += elapsedNanoseconds(recordStart);
				visitorNanoseconds += elapsedNanoseconds(visitorStart);
				++records;
			}
			return keepGoing && visited < maximumRecords;
		});
		if (tracing) {
			debug::PerfTrace::addDurationBatch("native_graph.read.edge_entity_record", entityRecordNanoseconds, records);
			debug::PerfTrace::addDurationBatch("native_graph.read.edge_entity_visitor", visitorNanoseconds, records);
		}
		return exhausted;
	}

	bool EntityPageStore::visitNodesAfter(const MetadataSnapshotPin &snapshot, uint64_t afterNodeId,
										  size_t maximumRecords, size_t maximumMetadataPages,
										  const std::function<bool(uint64_t nodeId, const NodeEntityRecord &)> &visitor,
										  size_t *visitedMetadataPages) const {
		if (maximumRecords == 0U || maximumMetadataPages == 0U || !visitor) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "relocatable resumable native Node scan requires positive bounds and a visitor");
		}
		uint64_t previousId = afterNodeId;
		size_t visited = 0U;
		std::optional<ReadPage> currentPage;
		PageId currentPageId;
		return metadata_.visitNodePageLocationsAfter(
				snapshot, afterNodeId, maximumMetadataPages,
				[&](const EntityPageLocation &entry) {
					if (visited == maximumRecords) { // the callback returns false when this count is reached, and LocationDirectory stops before invoking it again.
						fail(FormatError::FERR_INVALID_STATE,
							 "relocatable resumable native Node scan exceeded its caller-supplied record bound");
					}
					if (entry.entity_id == 0U || entry.entity_id <= previousId || !entry.page_id.isValid() ||
						entry.slot_generation == 0U) {
						fail(FormatError::FERR_INVALID_STATE, "relocatable resumable native Node scan contains an "
															  "invalid or unordered Entity Map entry");
					}
					ReadPage &loaded = loadScanReadPage(snapshot, entry.page_id, EntityPageRecordKind::EPRK_NODE,
																currentPage, currentPageId);
					const std::optional<NodeEntityRecord> record =
							loaded.page.findNode(entry.slot_id, entry.slot_generation, entry.entity_id);
					++visited;
					previousId = entry.entity_id;
					return visitor(entry.entity_id, *record) && visited < maximumRecords;
				},
				visitedMetadataPages);
	}

	EntityPageMutationResult EntityPageStore::updateNode(const MetadataSnapshotPin &snapshot, uint64_t nodeId,
														 const NodeEntityRecord &record,
														 LogSequenceNumber checkpointLsn,
														 const MetadataPublishOptions &options) {
		validateRecord(record);
		return update(snapshot, nodeId, EntityPageRecordKind::EPRK_NODE, &record, nullptr, checkpointLsn, options);
	}

	EntityPageMutationResult EntityPageStore::updateEdge(const MetadataSnapshotPin &snapshot, uint64_t edgeId,
														 const EdgeEntityRecord &record,
														 LogSequenceNumber checkpointLsn,
														 const MetadataPublishOptions &options) {
		validateRecord(record);
		return update(snapshot, edgeId, EntityPageRecordKind::EPRK_EDGE, nullptr, &record, checkpointLsn, options);
	}

	EntityPageBatchResult EntityPageStore::applyBatch(const MetadataSnapshotPin &snapshot,
													  std::span<const EntityPageBatchMutation> mutations,
													  LogSequenceNumber checkpointLsn,
													  const MetadataPublishOptions &options) {
		const std::array<EntityPageBatchAppend, 0U> noAppends{};
		return applyBatchWithAppends(snapshot, mutations, noAppends, checkpointLsn, options);
	}

	EntityPageBatchResult EntityPageStore::applyBatchWithAppends(const MetadataSnapshotPin &snapshot,
																 std::span<const EntityPageBatchMutation> mutations,
																 std::span<const EntityPageBatchAppend> appends,
																 LogSequenceNumber checkpointLsn,
																 const MetadataPublishOptions &options) {
		const std::array<EntityPageBatchNewPool, 0U> noNewPools{};
		return applyBatchTransaction(snapshot, mutations, appends, noNewPools, checkpointLsn, options);
	}

	EntityPageBatchResult EntityPageStore::applyBatchTransaction(const MetadataSnapshotPin &snapshot,
																 std::span<const EntityPageBatchMutation> mutations,
																 std::span<const EntityPageBatchAppend> appends,
																 std::span<const EntityPageBatchNewPool> newPools,
																 LogSequenceNumber checkpointLsn,
																 const MetadataPublishOptions &options) {
		const std::array<EntityPropertyRootMutationPlan, 0U> noPropertyPlans{};
		return applyBatchTransactionInternal(snapshot, mutations, appends, newPools, noPropertyPlans, nullptr, nullptr,
											 nullptr, nullptr, nullptr, nullptr, nullptr, checkpointLsn, options, {});
	}

	EntityPageBatchResult EntityPageStore::applyAuxiliaryEntityBatchTransaction(
			const MetadataSnapshotPin &snapshot, std::span<const EntityPageBatchMutation> mutations,
			std::span<const EntityPageBatchAppend> appends, std::span<const EntityPageBatchNewPool> newPools,
			const EntityPageAuxiliaryMutationBatch &auxiliaryPages, LogSequenceNumber checkpointLsn,
			const MetadataPublishOptions &options) {
		const std::array<EntityPropertyRootMutationPlan, 0U> noPropertyPlans{};
		return applyBatchTransactionInternal(snapshot, mutations, appends, newPools, noPropertyPlans, nullptr, nullptr,
											 &auxiliaryPages, nullptr, nullptr, nullptr, nullptr, checkpointLsn,
											 options, {});
	}

	EntityPageBatchResult EntityPageStore::applyNativeGraphBatchTransaction(
			const MetadataSnapshotPin &snapshot, std::span<const EntityPageBatchMutation> mutations,
			std::span<const EntityPageBatchAppend> appends, std::span<const EntityPageBatchNewPool> newPools,
			std::span<const EntityPropertyRootMutationPlan> propertyPlans,
			const PropertyCollectionRootStore &propertyRoots, const PropertyCollectionRootStoreOptions &propertyOptions,
			LogSequenceNumber checkpointLsn, const MetadataPublishOptions &options) {
		if (!propertyRoots.usesMetadataStore(metadata_) || !propertyOptions.first_slot.isValid() ||
			propertyOptions.first_slot > propertyOptions.last_slot || propertyOptions.page_owner_id == 0U) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "native Entity page batch requires one metadata domain and valid Property-pool allocation options");
		}
		const EntityPageAuxiliaryMutationBatch noAuxiliaryPages;
		return applyNativeGraphBatchTransaction(snapshot, mutations, appends, newPools, propertyPlans, propertyRoots,
												propertyOptions, noAuxiliaryPages, checkpointLsn, options);
	}

	EntityPageBatchResult EntityPageStore::applyNativeGraphBatchTransaction(
			const MetadataSnapshotPin &snapshot, std::span<const EntityPageBatchMutation> mutations,
			std::span<const EntityPageBatchAppend> appends, std::span<const EntityPageBatchNewPool> newPools,
			std::span<const EntityPropertyRootMutationPlan> propertyPlans,
			const PropertyCollectionRootStore &propertyRoots, const PropertyCollectionRootStoreOptions &propertyOptions,
			const EntityPageAuxiliaryMutationBatch &auxiliaryPages, LogSequenceNumber checkpointLsn,
			const MetadataPublishOptions &options) {
		if (!propertyRoots.usesMetadataStore(metadata_) || !propertyOptions.first_slot.isValid() ||
			propertyOptions.first_slot > propertyOptions.last_slot || propertyOptions.page_owner_id == 0U) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "native Entity page batch requires one metadata domain and valid Property-pool allocation options");
		}
		PropertyLargeValueDerivedStatePlan derivedState = planPropertyLargeValueDerivedState(
				snapshot, propertyPlans, propertyRoots, propertyOptions, checkpointLsn);
		return applyNativeGraphBatchTransaction(snapshot, mutations, appends, newPools, propertyPlans, propertyRoots,
												propertyOptions, auxiliaryPages, derivedState.state, derivedState.preparer,
												checkpointLsn, options);
	}

	EntityPageBatchResult EntityPageStore::applyNativeGraphBatchTransaction(
			const MetadataSnapshotPin &snapshot, std::span<const EntityPageBatchMutation> mutations,
			std::span<const EntityPageBatchAppend> appends, std::span<const EntityPageBatchNewPool> newPools,
			std::span<const EntityPropertyRootMutationPlan> propertyPlans,
			const PropertyCollectionRootStore &propertyRoots, const PropertyCollectionRootStoreOptions &propertyOptions,
			const EntityPageAuxiliaryMutationBatch &auxiliaryPages, const NativeGraphDerivedStateMutation &derivedState,
			const NativeGraphDerivedStatePreparer &derivedStatePreparer, LogSequenceNumber checkpointLsn,
			const MetadataPublishOptions &options, NativeGraphLogicalIdReservation logicalIdReservation,
		const NativeGraphVectorBootstrapPreparer &vectorBootstrapPreparer,
		const NativeGraphVectorDeltaPreparer &vectorDeltaPreparer,
		const NativeGraphPrivateSegmentImagePreparer &privateSegmentImagePreparer,
		const NativeGraphPrivateSegmentPreparer &privateSegmentPreparer,
		const NativeGraphDeferredIndexPlanner &deferredIndexPlanner,
        const NativeGraphEarlyPublicationPlanner &earlyPublicationPlanner,
        const NativeHybridTopologyPublication *hybridTopology) {
        if (hybridTopology && hybridTopology->entities_ != this)
            fail(FormatError::FERR_INVALID_STATE, "native topology proof belongs to another Entity store");
		if (!propertyRoots.usesMetadataStore(metadata_) || !propertyOptions.first_slot.isValid() ||
			propertyOptions.first_slot > propertyOptions.last_slot || propertyOptions.page_owner_id == 0U ||
			(derivedState.empty() ? static_cast<bool>(derivedStatePreparer) : !derivedStatePreparer) ||
			(derivedState.vector_bootstraps.empty() ? static_cast<bool>(vectorBootstrapPreparer)
													: !vectorBootstrapPreparer) ||
			(derivedState.vector_deltas.empty() ? static_cast<bool>(vectorDeltaPreparer) : !vectorDeltaPreparer)) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "native Entity page batch requires valid Property options and a matching derived-state preparer");
		}
		return applyBatchTransactionInternal(snapshot, mutations, appends, newPools, propertyPlans, &propertyRoots,
										 &propertyOptions, &auxiliaryPages, &derivedState, &derivedStatePreparer,
										 &vectorBootstrapPreparer, &vectorDeltaPreparer, checkpointLsn, options,
									 logicalIdReservation, &privateSegmentImagePreparer, &privateSegmentPreparer,
                                     &deferredIndexPlanner, &earlyPublicationPlanner, hybridTopology);
	}

	EntityPageBatchResult EntityPageStore::applyBatchTransactionInternal(
			const MetadataSnapshotPin &snapshot, std::span<const EntityPageBatchMutation> mutations,
			std::span<const EntityPageBatchAppend> appends, std::span<const EntityPageBatchNewPool> newPools,
			std::span<const EntityPropertyRootMutationPlan> propertyPlans,
			const PropertyCollectionRootStore *propertyRoots, const PropertyCollectionRootStoreOptions *propertyOptions,
			const EntityPageAuxiliaryMutationBatch *auxiliaryPages, const NativeGraphDerivedStateMutation *derivedState,
		const NativeGraphDerivedStatePreparer *derivedStatePreparer,
		const NativeGraphVectorBootstrapPreparer *vectorBootstrapPreparer,
		const NativeGraphVectorDeltaPreparer *vectorDeltaPreparer, LogSequenceNumber checkpointLsn,
		const MetadataPublishOptions &options, NativeGraphLogicalIdReservation logicalIdReservation,
		const NativeGraphPrivateSegmentImagePreparer *privateSegmentImagePreparer,
		const NativeGraphPrivateSegmentPreparer *privateSegmentPreparer,
		const NativeGraphDeferredIndexPlanner *deferredIndexPlanner,
        const NativeGraphEarlyPublicationPlanner *earlyPublicationPlanner,
        const NativeHybridTopologyPublication *hybridTopology) {
		debug::ScopedPerfTimer timer("native_graph.entity_batch");
		const bool hasDerivedState = derivedState != nullptr && !derivedState->empty();
		const bool hasVectorBootstrap = hasDerivedState && !derivedState->vector_bootstraps.empty();
		const bool hasVectorDelta = hasDerivedState && !derivedState->vector_deltas.empty();
		if ((mutations.empty() && appends.empty() && newPools.empty() && propertyPlans.empty() &&
			 (auxiliaryPages == nullptr || auxiliaryPages->empty()) && !hasDerivedState &&
			 logicalIdReservation.empty()) ||
			!checkpointLsn.isValid() ||
			(!propertyPlans.empty() && (propertyRoots == nullptr || propertyOptions == nullptr)) ||
			(propertyRoots == nullptr) != (propertyOptions == nullptr) ||
			(hasDerivedState && (derivedStatePreparer == nullptr || !*derivedStatePreparer)) ||
			(!hasDerivedState && derivedStatePreparer != nullptr && *derivedStatePreparer) ||
			(hasVectorBootstrap && (vectorBootstrapPreparer == nullptr || !*vectorBootstrapPreparer)) ||
			(!hasVectorBootstrap && vectorBootstrapPreparer != nullptr && *vectorBootstrapPreparer) ||
			(hasVectorDelta && (vectorDeltaPreparer == nullptr || !*vectorDeltaPreparer)) ||
			(!hasVectorDelta && vectorDeltaPreparer != nullptr && *vectorDeltaPreparer)) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "relocatable Entity page batch requires work, a checkpoint LSN, and complete Property planning state");
		}

		struct StagedPage {
			LoadedPage loaded;
			bool retire_page = false;

			StagedPage(LoadedPage loaded, bool retirePage) : loaded(std::move(loaded)), retire_page(retirePage) {}
		};
		struct StagedAppendPage {
			CatalogEntry pool;
			EntityPage page;
			std::vector<EntityPageLocation> mappings;

			StagedAppendPage(CatalogEntry pool, EntityPage page, std::vector<EntityPageLocation> mappings) :
				pool(std::move(pool)), page(std::move(page)), mappings(std::move(mappings)) {}
		};
		struct StagedNewPoolPage {
			EntityPageNewPoolRequest request;
			EntityPage page;
			std::vector<EntityPageLocation> mappings;

			StagedNewPoolPage(EntityPageNewPoolRequest request, EntityPage page,
							  std::vector<EntityPageLocation> mappings) :
				request(std::move(request)), page(std::move(page)), mappings(std::move(mappings)) {}
		};
		const EntityPageAuxiliaryMutationBatch emptyAuxiliaryPages;
		const EntityPageAuxiliaryMutationBatch &auxiliary =
				auxiliaryPages == nullptr ? emptyAuxiliaryPages : *auxiliaryPages;
		std::set<uint64_t> pageIds;
		std::vector<StagedPage> stagedPages;
		std::vector<StagedAppendPage> stagedAppends;
		std::vector<StagedNewPoolPage> stagedNewPools;
		std::vector<EntityPageAuxiliaryReplacement> stagedAuxiliaryReplacements;
		std::vector<EntityPageAuxiliaryAppend> stagedAuxiliaryAppends;
		std::vector<EntityPageAuxiliaryNewPool> stagedAuxiliaryNewPools;
		std::vector<EntityPageReplacementRequest> requests;
		std::vector<EntityPageAppendBatchRequest> appendRequests;
		std::vector<EntityPageNewPoolBatchRequest> newPoolRequests;
		std::vector<NativeGraphPropertyPageReplacementRequest> propertyReplacementRequests;
		std::vector<NativeGraphPropertyPageBatchRequest> propertyPageBatchRequests;
		std::vector<NativeGraphLargeValueMutation> largeValueMutations;
		std::vector<size_t> replacementPropertyPlanOrdinals;
		std::vector<std::vector<std::vector<size_t>>> propertyPageBatchPlanOrdinals;
		stagedPages.reserve(mutations.size());
		stagedAppends.reserve(appends.size());
		stagedNewPools.reserve(newPools.size());
		stagedAuxiliaryReplacements.reserve(auxiliary.replacements.size());
		stagedAuxiliaryAppends.reserve(auxiliary.appends.size());
		stagedAuxiliaryNewPools.reserve(auxiliary.new_pools.size());
		requests.reserve(mutations.size() + auxiliary.replacements.size());
		appendRequests.reserve(appends.size() + auxiliary.appends.size());
		newPoolRequests.reserve(newPools.size() + auxiliary.new_pools.size());
		propertyReplacementRequests.reserve(propertyPlans.size());
		propertyPageBatchRequests.reserve(propertyPlans.size());
		replacementPropertyPlanOrdinals.reserve(propertyPlans.size());
		propertyPageBatchPlanOrdinals.reserve(propertyPlans.size());
		largeValueMutations.reserve(propertyPlans.size());
		// Authenticate every Entity ID touched by this publication through one
		// ordered Directory range per record kind. The page-local loops below still
		// validate page ownership and slot generations, but no longer repeat a
		// radix root descent for each update, erase, or insertion.
		std::vector<uint64_t> batchNodeIds;
		std::vector<uint64_t> batchEdgeIds;
		const auto collectNodeId = [&batchNodeIds](uint64_t nodeId) { batchNodeIds.push_back(nodeId); };
		const auto collectEdgeId = [&batchEdgeIds](uint64_t edgeId) { batchEdgeIds.push_back(edgeId); };
		for (const EntityPageBatchMutation &mutation: mutations) {
			for (const auto &update: mutation.node_updates) collectNodeId(update.node_id);
			for (const auto &insertion: mutation.node_insertions) collectNodeId(insertion.node_id);
			for (const uint64_t nodeId: mutation.erased_node_ids) collectNodeId(nodeId);
			for (const auto &update: mutation.edge_updates) collectEdgeId(update.edge_id);
			for (const auto &insertion: mutation.edge_insertions) collectEdgeId(insertion.edge_id);
			for (const uint64_t edgeId: mutation.erased_edge_ids) collectEdgeId(edgeId);
		}
		for (const EntityPageBatchAppend &append: appends) {
			for (const auto &insertion: append.node_insertions) collectNodeId(insertion.node_id);
			for (const auto &insertion: append.edge_insertions) collectEdgeId(insertion.edge_id);
		}
		for (const EntityPageBatchNewPool &newPool: newPools) {
			for (const auto &insertion: newPool.node_insertions) collectNodeId(insertion.node_id);
			for (const auto &insertion: newPool.edge_insertions) collectEdgeId(insertion.edge_id);
		}
		const auto normalizeIds = [](std::vector<uint64_t> &ids) {
			std::ranges::sort(ids);
			ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
		};
		normalizeIds(batchNodeIds);
		normalizeIds(batchEdgeIds);
		std::vector<std::optional<EntityPageLocation>> batchNodeLocations(batchNodeIds.size());
		std::vector<std::optional<EntityPageLocation>> batchEdgeLocations(batchEdgeIds.size());
		if (!batchNodeIds.empty()) {
			metadata_.findNodePageLocationsSorted(snapshot, batchNodeIds, batchNodeLocations);
			debug::PerfTrace::addValue("native_graph.commit.entity_page_batch_directory_requests", 1);
		}
		if (!batchEdgeIds.empty()) {
			metadata_.findEdgePageLocationsSorted(snapshot, batchEdgeIds, batchEdgeLocations);
			debug::PerfTrace::addValue("native_graph.commit.entity_page_batch_directory_requests", 1);
		}
		const auto findBatchLocation = [](const std::vector<uint64_t> &ids,
				const std::vector<std::optional<EntityPageLocation>> &locations,
				uint64_t entityId) -> std::optional<EntityPageLocation> {
			const auto iterator = std::ranges::lower_bound(ids, entityId);
			if (iterator == ids.end() || *iterator != entityId) {
				return std::nullopt;
			}
			return locations[static_cast<size_t>(iterator - ids.begin())];
		};
		const auto nodeLocation = [&](uint64_t nodeId) { return findBatchLocation(batchNodeIds, batchNodeLocations, nodeId); };
		const auto edgeLocation = [&](uint64_t edgeId) { return findBatchLocation(batchEdgeIds, batchEdgeLocations, edgeId); };
		const auto requireNodeLocation = [&](uint64_t nodeId) {
			const auto location = nodeLocation(nodeId);
			if (!location.has_value()) {
				fail(FormatError::FERR_INVALID_STATE, "relocatable Entity batch Node mapping is absent");
			}
			return *location;
		};
		const auto requireEdgeLocation = [&](uint64_t edgeId) {
			const auto location = edgeLocation(edgeId);
			if (!location.has_value()) {
				fail(FormatError::FERR_INVALID_STATE, "relocatable Entity batch Edge mapping is absent");
			}
			return *location;
		};
		std::set<uint64_t> mutatedPropertyKeys;
		std::set<uint64_t> plannedLargeValueIds;
		std::vector<size_t> pendingNewPropertyPlanOrdinals;
		pendingNewPropertyPlanOrdinals.reserve(propertyPlans.size());
		for (size_t ordinal = 0U; ordinal < propertyPlans.size(); ++ordinal) {
			const EntityPropertyRootMutationPlan &plan = propertyPlans[ordinal];
			switch (plan.kind) {
				case EntityPropertyRootMutationPlanKind::EPRMPK_NO_CHANGE:
					if (!plan.key.isValid() || plan.page_replacement.has_value() || !plan.replacement_payload.empty() ||
						!plan.encoded_root.empty() || plan.replacement_free_bytes != 0U || !plan.large_values.empty() ||
						!plan.reused_large_values.empty()) {
						fail(FormatError::FERR_INVALID_HEADER,
							 "native Entity page batch received a malformed unchanged Property-root plan");
					}
					break;
				case EntityPropertyRootMutationPlanKind::EPRMPK_EXISTING_PAGE: {
					if (!plan.key.isValid() || !plan.page_replacement.has_value() || !plan.encoded_root.empty() ||
						!mutatedPropertyKeys.insert(plan.key.directoryId()).second) {
						fail(FormatError::FERR_INVALID_HEADER,
							 "native Entity page batch received conflicting existing-page Property-root plans");
					}
					const NativeGraphPropertyPageReplacementRequest &request = *plan.page_replacement;
					const bool containsPlanKey =
							std::ranges::any_of(request.upserted_properties,
												[key = plan.key](const EntityPropertyLocation &location) {
													return location.key == key;
												}) ||
							std::ranges::find(request.erased_properties, plan.key) != request.erased_properties.end();
					const bool validNonRetirement =
							!request.retire_page &&
							(!request.upserted_properties.empty() || !request.erased_properties.empty() ||
							 !request.relocated_properties.empty()) &&
							containsPlanKey;
					const bool validRetirement =
							!request.retire_page ||
							(request.upserted_properties.empty() &&
							 (!request.erased_properties.empty() || !request.relocated_properties.empty()) &&
							 plan.replacement_payload.empty() && plan.replacement_free_bytes == 0U);
					if ((!validNonRetirement && !request.retire_page) || !validRetirement ||
						(request.upserted_properties.empty() &&
						 (!plan.large_values.empty() || !plan.reused_large_values.empty()))) {
						fail(FormatError::FERR_INVALID_HEADER,
							 "native Entity page batch Property-root plan has an invalid page-local final state");
					}
					propertyReplacementRequests.push_back(request);
					replacementPropertyPlanOrdinals.push_back(ordinal);
					break;
				}
				case EntityPropertyRootMutationPlanKind::EPRMPK_NEW_PAGE_REQUIRED: {
					if (!plan.key.isValid() || plan.encoded_root.empty() ||
						plan.encoded_root.size() > RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES - RSS_SLOTTED_PAGE_HEADER_BYTES -
														   RSS_SLOTTED_PAGE_SLOT_BYTES ||
						!mutatedPropertyKeys.insert(plan.key.directoryId()).second) {
						fail(FormatError::FERR_INVALID_HEADER,
							 "native Entity page batch received a malformed new-pool Property-root plan");
					}
					if (plan.page_replacement.has_value()) {
						const NativeGraphPropertyPageReplacementRequest &request = *plan.page_replacement;
						const bool containsRelocatedKey =
								std::ranges::find(request.relocated_properties, plan.key) !=
								request.relocated_properties.end();
						const bool validRetirement =
								!request.retire_page ||
								(request.upserted_properties.empty() &&
								 (!request.erased_properties.empty() || !request.relocated_properties.empty()) &&
								 plan.replacement_payload.empty() && plan.replacement_free_bytes == 0U);
						const bool validReplacement =
								request.retire_page ||
								(plan.replacement_payload.size() == RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES &&
								 plan.replacement_free_bytes <=
										 RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES - RSS_SLOTTED_PAGE_HEADER_BYTES);
						if (!containsRelocatedKey || !validRetirement || !validReplacement) {
							fail(FormatError::FERR_INVALID_HEADER,
								 "native Entity page batch received a malformed relocating Property-root plan");
						}
						propertyReplacementRequests.push_back(request);
						replacementPropertyPlanOrdinals.push_back(ordinal);
					} else if (!plan.replacement_payload.empty() || plan.replacement_free_bytes != 0U) {
						fail(FormatError::FERR_INVALID_HEADER,
							 "native Entity page batch received a malformed new-pool Property-root plan");
					}
					pendingNewPropertyPlanOrdinals.push_back(ordinal);
					break;
				}
			}
			for (const NativeGraphLargeValueMutation &largeValue: plan.large_values) {
				if (!largeValue.reference.value_id.isValid() ||
					!plannedLargeValueIds.insert(largeValue.reference.value_id.value()).second) {
					fail(FormatError::FERR_INVALID_HEADER,
						 "native Entity page batch received duplicate or invalid LargeValue IDs");
				}
				largeValueMutations.push_back(largeValue);
			}
		}
		// A Property-page COW source remains allocated until a later reconciliation
		// pass. Reclaim frames that no active snapshot can resolve before deciding
		// whether this batch needs another Property pool.
		std::map<uint64_t, size_t> propertyReplacementDemand;
		for (const NativeGraphPropertyPageReplacementRequest &request: propertyReplacementRequests) {
			if (!request.retire_page) {
				++propertyReplacementDemand[request.expected_pool.segment_id.value()];
			}
		}
		for (const auto &[segmentId, requiredFrames]: propertyReplacementDemand) {
			const CatalogEntry pool = metadata_.findCatalog(snapshot, SegmentId(segmentId));
			if (requiredFrames > allocator_.availableFrameCount(pool)) {
				allocator_.reclaimRetiredFrames(pool.segment_id);
			}
		}
		std::ranges::sort(pendingNewPropertyPlanOrdinals, {},
						  [&propertyPlans](size_t ordinal) { return propertyPlans[ordinal].key.directoryId(); });
		std::vector<std::vector<size_t>> pendingPropertyPages;
		std::vector<size_t> currentPropertyPagePlans;
		currentPropertyPagePlans.reserve(pendingNewPropertyPlanOrdinals.size());
		size_t currentPropertyPageBytes = RSS_SLOTTED_PAGE_HEADER_BYTES;
		const auto finishPropertyPage = [&] {
			if (currentPropertyPagePlans.empty()) {
				return;
			}
			pendingPropertyPages.push_back(std::move(currentPropertyPagePlans));
			currentPropertyPagePlans = {};
			currentPropertyPageBytes = RSS_SLOTTED_PAGE_HEADER_BYTES;
		};
		for (const size_t ordinal: pendingNewPropertyPlanOrdinals) {
			const size_t recordBytes = propertyPlans[ordinal].encoded_root.size() + RSS_SLOTTED_PAGE_SLOT_BYTES;
			if (!currentPropertyPagePlans.empty() &&
				currentPropertyPageBytes > RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES - recordBytes) {
				finishPropertyPage();
			}
			currentPropertyPagePlans.push_back(ordinal);
			currentPropertyPageBytes += recordBytes;
		}
		finishPropertyPage();

		const auto keysForPages = [&propertyPlans](std::span<const std::vector<size_t>> pageOrdinals) {
			std::vector<std::vector<EntityPropertyKey>> pages;
			pages.reserve(pageOrdinals.size());
			for (const std::vector<size_t> &ordinals: pageOrdinals) {
				std::vector<EntityPropertyKey> keys;
				keys.reserve(ordinals.size());
				for (const size_t ordinal: ordinals) {
					keys.push_back(propertyPlans[ordinal].key);
				}
				pages.push_back(std::move(keys));
			}
			return pages;
		};
		if (!pendingPropertyPages.empty()) {
			struct PropertyPoolCapacity {
				CatalogEntry pool;
				size_t available_frames = 0U;
			};
			std::map<uint64_t, size_t> replacementFramesByPool;
			for (const NativeGraphPropertyPageReplacementRequest &request: propertyReplacementRequests) {
				if (!request.retire_page) {
					++replacementFramesByPool[request.expected_pool.segment_id.value()];
				}
			}
			std::vector<PropertyPoolCapacity> capacities;
			for (const CatalogEntry &pool:
				 metadata_.findCurrentPropertyPagePools(snapshot, propertyOptions->page_owner_id)) {
				const size_t physicalFree = allocator_.availableFrameCount(pool);
				const size_t replacementFrames = replacementFramesByPool[pool.segment_id.value()];
				// The metadata writer can move COW replacements from an exhausted
				// source pool into a fresh compatible pool. Reserve existing frames
				// conservatively for those replacements; only surplus frames may
				// serve new logical Property pages. Do not reject a legal relocation
				// here or underflow when replacement demand exceeds free capacity.
				capacities.push_back({pool, physicalFree > replacementFrames ? physicalFree - replacementFrames : 0U});
			}
			std::ranges::sort(capacities, [](const PropertyPoolCapacity &left,
												 const PropertyPoolCapacity &right) {
				return std::tuple<size_t, uint64_t>{left.available_frames, left.pool.segment_id.value()} <
					   std::tuple<size_t, uint64_t>{right.available_frames, right.pool.segment_id.value()};
			});

			size_t nextPage = 0U;
			for (const PropertyPoolCapacity &capacity: capacities) {
				if (nextPage == pendingPropertyPages.size()) {
					break;
				}
				const size_t pageCount =
						std::min(capacity.available_frames, pendingPropertyPages.size() - nextPage);
				if (pageCount == 0U) {
					continue;
				}
				std::vector<std::vector<size_t>> pageOrdinals(
						pendingPropertyPages.begin() + static_cast<std::ptrdiff_t>(nextPage),
						pendingPropertyPages.begin() + static_cast<std::ptrdiff_t>(nextPage + pageCount));
				propertyPageBatchRequests.push_back(
						{NativeGraphPropertyPageBatchKind::NGPBK_APPEND_EXISTING_POOL, capacity.pool,
						 propertyOptions->page_owner_id, {}, {}, keysForPages(pageOrdinals)});
				propertyPageBatchPlanOrdinals.push_back(std::move(pageOrdinals));
				nextPage += pageCount;
			}
			while (nextPage < pendingPropertyPages.size()) {
				const size_t pageCount =
						std::min(static_cast<size_t>(RSS_PAGE_POOL_DATA_FRAME_COUNT),
								 pendingPropertyPages.size() - nextPage);
				std::vector<std::vector<size_t>> pageOrdinals(
						pendingPropertyPages.begin() + static_cast<std::ptrdiff_t>(nextPage),
						pendingPropertyPages.begin() + static_cast<std::ptrdiff_t>(nextPage + pageCount));
				propertyPageBatchRequests.push_back(
						{NativeGraphPropertyPageBatchKind::NGPBK_CREATE_POOL, {}, propertyOptions->page_owner_id,
						 propertyOptions->first_slot, propertyOptions->last_slot, keysForPages(pageOrdinals)});
				propertyPageBatchPlanOrdinals.push_back(std::move(pageOrdinals));
				nextPage += pageCount;
			}
		}

		for (const EntityPageBatchMutation &mutation: mutations) {
			if (!mutation.page_id.isValid() || !isEntityPageKind(mutation.record_kind) ||
				!pageIds.insert(mutation.page_id.value()).second) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable Entity page batch requires unique valid Page IDs and record kinds");
			}
			const bool nodeKind = isNodePage(mutation.record_kind);
			const bool hasNodeOperations = !mutation.node_updates.empty() || !mutation.node_insertions.empty() ||
										   !mutation.erased_node_ids.empty();
			const bool hasEdgeOperations = !mutation.edge_updates.empty() || !mutation.edge_insertions.empty() ||
										   !mutation.erased_edge_ids.empty();
			if ((!nodeKind && hasNodeOperations) || (nodeKind && hasEdgeOperations) ||
				(!hasNodeOperations && !hasEdgeOperations)) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable Entity page batch mixes record kinds or contains an empty page mutation");
			}

			LoadedPage loaded = loadPage(snapshot, mutation.page_id, mutation.record_kind);
			std::set<uint64_t> touchedEntityIds;
			EntityPageReplacementRequest request;
			request.expected_pool = loaded.contents.pool;
			request.replacement_pool = mutation.replacement_pool;
			request.replacement_new_pool = mutation.replacement_new_pool;
			request.expected_location = loaded.contents.location;
			if (nodeKind) {
				for (const EntityPageNodeUpdate &update: mutation.node_updates) {
					validateRecord(update.record);
					if (update.node_id == 0U || !touchedEntityIds.insert(update.node_id).second) {
						fail(FormatError::FERR_INVALID_HEADER,
							 "relocatable Entity page batch Node update requires one nonzero Entity ID per page");
					}
					const EntityPageLocation location = requireNodeLocation(update.node_id);
					if (location.page_id != mutation.page_id) {
						fail(FormatError::FERR_INVALID_STATE,
							 "relocatable Entity page batch Node update targets a foreign Entity Map page");
					}
					loaded.page.replace(location.slot_id, location.slot_generation, update.node_id, update.record);
				}
				for (const EntityPageNodeInsertion &insertion: mutation.node_insertions) {
					validateRecord(insertion.record);
					if (insertion.node_id == 0U || !touchedEntityIds.insert(insertion.node_id).second) {
						fail(FormatError::FERR_INVALID_HEADER, "relocatable Entity page batch Node insertion requires "
															   "one new nonzero Entity ID per page");
					}
					if (nodeLocation(insertion.node_id).has_value()) {
						fail(FormatError::FERR_INVALID_STATE,
							 "relocatable Entity page batch Node insertion reuses a published Entity ID");
					}
					const uint16_t slot = loaded.page.insert(insertion.node_id, insertion.record);
					request.inserted_nodes.push_back(
							mappingFor(insertion.node_id, mutation.page_id, loaded.page, slot));
				}
				for (const uint64_t nodeId: mutation.erased_node_ids) {
					if (nodeId == 0U || !touchedEntityIds.insert(nodeId).second) {
						fail(FormatError::FERR_INVALID_HEADER,
							 "relocatable Entity page batch Node erase requires one nonzero Entity ID per page");
					}
					const EntityPageLocation location = requireNodeLocation(nodeId);
					if (location.page_id != mutation.page_id) {
						fail(FormatError::FERR_INVALID_STATE,
							 "relocatable Entity page batch Node erase targets a foreign Entity Map page");
					}
					loaded.page.erase(location.slot_id, location.slot_generation, nodeId);
					request.erased_node_ids.push_back(nodeId);
				}
			} else {
				for (const EntityPageEdgeUpdate &update: mutation.edge_updates) {
					validateRecord(update.record);
					if (update.edge_id == 0U || !touchedEntityIds.insert(update.edge_id).second) {
						fail(FormatError::FERR_INVALID_HEADER,
							 "relocatable Entity page batch Edge update requires one nonzero Entity ID per page");
					}
					const EntityPageLocation location = requireEdgeLocation(update.edge_id);
					if (location.page_id != mutation.page_id) {
						fail(FormatError::FERR_INVALID_STATE,
							 "relocatable Entity page batch Edge update targets a foreign Entity Map page");
					}
					loaded.page.replace(location.slot_id, location.slot_generation, update.edge_id, update.record);
				}
				for (const EntityPageEdgeInsertion &insertion: mutation.edge_insertions) {
					validateRecord(insertion.record);
					if (insertion.edge_id == 0U || !touchedEntityIds.insert(insertion.edge_id).second) {
						fail(FormatError::FERR_INVALID_HEADER, "relocatable Entity page batch Edge insertion requires "
															   "one new nonzero Entity ID per page");
					}
					if (edgeLocation(insertion.edge_id).has_value()) {
						fail(FormatError::FERR_INVALID_STATE,
							 "relocatable Entity page batch Edge insertion reuses a published Entity ID");
					}
					const uint16_t slot = loaded.page.insert(insertion.edge_id, insertion.record);
					request.inserted_edges.push_back(
							mappingFor(insertion.edge_id, mutation.page_id, loaded.page, slot));
				}
				for (const uint64_t edgeId: mutation.erased_edge_ids) {
					if (edgeId == 0U || !touchedEntityIds.insert(edgeId).second) {
						fail(FormatError::FERR_INVALID_HEADER,
							 "relocatable Entity page batch Edge erase requires one nonzero Entity ID per page");
					}
					const EntityPageLocation location = requireEdgeLocation(edgeId);
					if (location.page_id != mutation.page_id) {
						fail(FormatError::FERR_INVALID_STATE,
							 "relocatable Entity page batch Edge erase targets a foreign Entity Map page");
					}
					loaded.page.erase(location.slot_id, location.slot_generation, edgeId);
					request.erased_edge_ids.push_back(edgeId);
				}
			}
			std::ranges::sort(request.inserted_nodes, {}, &EntityPageLocation::entity_id);
			std::ranges::sort(request.erased_node_ids);
			std::ranges::sort(request.inserted_edges, {}, &EntityPageLocation::entity_id);
			std::ranges::sort(request.erased_edge_ids);
			const bool retirePage = loaded.page.header().live_count == 0U;
			request.retire_page = retirePage;
			if (retirePage) {
				// A retired PageId has no successor frame. Ignore the planner's
				// placement hint instead of treating it as an attempted COW move.
				request.replacement_pool = {};
				request.replacement_new_pool.reset();
			}
			if (!retirePage) {
				PageOwnerHeader owner = loaded.contents.owner;
				owner.committed_lsn = checkpointLsn;
				loaded.page = loaded.page.rebindOwner(owner);
			}
			stagedPages.emplace_back(std::move(loaded), retirePage);
			requests.push_back(std::move(request));
		}
		for (const EntityPageBatchAppend &append: appends) {
			if (!append.request.page_id.isValid() || !isEntityPageKind(append.record_kind) ||
				!pageIds.insert(append.request.page_id.value()).second) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable Entity page batch append requires a unique Page ID and record kind");
			}
			const bool nodeKind = isNodePage(append.record_kind);
			const bool hasNodes = !append.node_insertions.empty();
			const bool hasEdges = !append.edge_insertions.empty();
			const uint8_t invalidShape = (static_cast<uint8_t>(nodeKind) & static_cast<uint8_t>(!hasNodes)) |
										 (static_cast<uint8_t>(!nodeKind) & static_cast<uint8_t>(!hasEdges)) |
										 (static_cast<uint8_t>(nodeKind) & static_cast<uint8_t>(hasEdges)) |
										 (static_cast<uint8_t>(!nodeKind) & static_cast<uint8_t>(hasNodes));
			if (invalidShape != 0U) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable Entity page batch append requires exactly one nonempty record kind");
			}

			const uint64_t firstEntityId =
					nodeKind ? append.node_insertions.front().node_id : append.edge_insertions.front().edge_id;
			validateAppendRequest(append.request, firstEntityId, append.record_kind, checkpointLsn);
			const PageOwnerHeader owner = ownerFor(append.request.page_id, append.request.expected_pool.owner_id,
												   append.request.expected_pool.generation, checkpointLsn);
			EntityPage page = EntityPage::create(owner, append.record_kind);
			std::set<uint64_t> insertedIds;
			EntityPageAppendBatchRequest request;
			request.expected_pool = append.request.expected_pool;
			request.page_id = append.request.page_id;
			EntityPageLocation mapping;
			if (nodeKind) {
				std::vector<IdentifiedNodeEntityRecord> records;
				records.reserve(std::min<size_t>(append.node_insertions.size(), page.header().slot_capacity));
				for (const EntityPageNodeInsertion &insertion: append.node_insertions) {
					validateRecord(insertion.record);
					if (insertion.node_id == 0U || !insertedIds.insert(insertion.node_id).second) {
						fail(FormatError::FERR_INVALID_HEADER,
							 "relocatable Entity page batch Node append requires unique nonzero Entity IDs");
					}
					if (nodeLocation(insertion.node_id).has_value()) {
						fail(FormatError::FERR_INVALID_STATE,
							 "relocatable Entity page batch Node append reuses a published Entity ID");
					}
					if (records.size() == page.header().slot_capacity) fail(FormatError::FERR_INVALID_STATE, "Entity append batch exceeds page capacity");
					records.push_back({insertion.node_id, insertion.record});
				}
				const auto slots = page.insertNodes(records);
				for (size_t i = 0U; i < records.size(); ++i) {
					const uint16_t slot = slots[i];
					mapping = mappingFor(records[i].entity_id, append.request.page_id, page, slot);
					request.inserted_nodes.push_back(mapping);
				}
				std::ranges::sort(request.inserted_nodes, {}, &EntityPageLocation::entity_id);
			} else {
				std::vector<IdentifiedEdgeEntityRecord> records;
				records.reserve(std::min<size_t>(append.edge_insertions.size(), page.header().slot_capacity));
				for (const EntityPageEdgeInsertion &insertion: append.edge_insertions) {
					validateRecord(insertion.record);
					if (insertion.edge_id == 0U || !insertedIds.insert(insertion.edge_id).second) {
						fail(FormatError::FERR_INVALID_HEADER,
							 "relocatable Entity page batch Edge append requires unique nonzero Entity IDs");
					}
					if (edgeLocation(insertion.edge_id).has_value()) {
						fail(FormatError::FERR_INVALID_STATE,
							 "relocatable Entity page batch Edge append reuses a published Entity ID");
					}
					if (records.size() == page.header().slot_capacity) fail(FormatError::FERR_INVALID_STATE, "Entity append batch exceeds page capacity");
					records.push_back({insertion.edge_id, insertion.record});
				}
				const auto slots = page.insertEdges(records);
				for (size_t i = 0U; i < records.size(); ++i) {
					const uint16_t slot = slots[i];
					mapping = mappingFor(records[i].entity_id, append.request.page_id, page, slot);
					request.inserted_edges.push_back(mapping);
				}
				std::ranges::sort(request.inserted_edges, {}, &EntityPageLocation::entity_id);
			}
			std::vector<EntityPageLocation> mappings = nodeKind ? request.inserted_nodes : request.inserted_edges;
			stagedAppends.emplace_back(append.request.expected_pool, std::move(page), std::move(mappings));
			appendRequests.push_back(std::move(request));
		}
		for (const EntityPageBatchNewPool &newPool: newPools) {
			if (!newPool.request.page_id.isValid() || !isEntityPageKind(newPool.record_kind) ||
				!pageIds.insert(newPool.request.page_id.value()).second) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable Entity page new-pool batch requires a unique Page ID and record kind");
			}
			const bool nodeKind = isNodePage(newPool.record_kind);
			const bool hasNodes = !newPool.node_insertions.empty();
			const bool hasEdges = !newPool.edge_insertions.empty();
			const uint8_t invalidShape = (static_cast<uint8_t>(nodeKind) & static_cast<uint8_t>(!hasNodes)) |
										 (static_cast<uint8_t>(!nodeKind) & static_cast<uint8_t>(!hasEdges)) |
										 (static_cast<uint8_t>(nodeKind) & static_cast<uint8_t>(hasEdges)) |
										 (static_cast<uint8_t>(!nodeKind) & static_cast<uint8_t>(hasNodes));
			if (invalidShape != 0U) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable Entity page new-pool batch requires exactly one nonempty record kind");
			}
			const uint64_t firstEntityId =
					nodeKind ? newPool.node_insertions.front().node_id : newPool.edge_insertions.front().edge_id;
			validateNewPoolRequest(newPool.request, firstEntityId, newPool.record_kind, checkpointLsn);
			const PageOwnerHeader owner = ownerFor(newPool.request.page_id, newPool.request.pool_owner_id,
												   newPool.request.generation, checkpointLsn);
			EntityPage page = EntityPage::create(owner, newPool.record_kind);
			std::set<uint64_t> insertedIds;
			EntityPageNewPoolBatchRequest request;
			request.reservation.segment_id = newPool.request.segment_id;
			request.reservation.kind = SegmentKind::SGK_ENTITY_PAGE_POOL;
			request.reservation.owner_id = newPool.request.pool_owner_id;
			request.reservation.segment_generation = newPool.request.generation;
			request.reservation.state = SegmentState::SGS_SEALED;
			request.reservation.require_absent = true;
			request.page_id = newPool.request.page_id;
			request.first_slot = newPool.request.first_slot;
			request.last_slot = newPool.request.last_slot;
			EntityPageLocation mapping;
			if (nodeKind) {
				std::vector<IdentifiedNodeEntityRecord> records;
				records.reserve(std::min<size_t>(newPool.node_insertions.size(), page.header().slot_capacity));
				for (const EntityPageNodeInsertion &insertion: newPool.node_insertions) {
					validateRecord(insertion.record);
					if (insertion.node_id == 0U || !insertedIds.insert(insertion.node_id).second) {
						fail(FormatError::FERR_INVALID_HEADER,
							 "relocatable Entity page new-pool Node batch requires unique nonzero Entity IDs");
					}
					if (nodeLocation(insertion.node_id).has_value()) {
						fail(FormatError::FERR_INVALID_STATE,
							 "relocatable Entity page new-pool Node batch reuses a published Entity ID");
					}
					if (records.size() == page.header().slot_capacity) fail(FormatError::FERR_INVALID_STATE, "Entity append batch exceeds page capacity");
					records.push_back({insertion.node_id, insertion.record});
				}
				const auto slots = page.insertNodes(records);
				for (size_t i = 0U; i < records.size(); ++i) {
					const uint16_t slot = slots[i];
					mapping = mappingFor(records[i].entity_id, newPool.request.page_id, page, slot);
					request.inserted_nodes.push_back(mapping);
				}
				std::ranges::sort(request.inserted_nodes, {}, &EntityPageLocation::entity_id);
			} else {
				std::vector<IdentifiedEdgeEntityRecord> records;
				records.reserve(std::min<size_t>(newPool.edge_insertions.size(), page.header().slot_capacity));
				for (const EntityPageEdgeInsertion &insertion: newPool.edge_insertions) {
					validateRecord(insertion.record);
					if (insertion.edge_id == 0U || !insertedIds.insert(insertion.edge_id).second) {
						fail(FormatError::FERR_INVALID_HEADER,
							 "relocatable Entity page new-pool Edge batch requires unique nonzero Entity IDs");
					}
					if (edgeLocation(insertion.edge_id).has_value()) {
						fail(FormatError::FERR_INVALID_STATE,
							 "relocatable Entity page new-pool Edge batch reuses a published Entity ID");
					}
					if (records.size() == page.header().slot_capacity) fail(FormatError::FERR_INVALID_STATE, "Entity append batch exceeds page capacity");
					records.push_back({insertion.edge_id, insertion.record});
				}
				const auto slots = page.insertEdges(records);
				for (size_t i = 0U; i < records.size(); ++i) {
					const uint16_t slot = slots[i];
					mapping = mappingFor(records[i].entity_id, newPool.request.page_id, page, slot);
					request.inserted_edges.push_back(mapping);
				}
				std::ranges::sort(request.inserted_edges, {}, &EntityPageLocation::entity_id);
			}
			std::vector<EntityPageLocation> mappings = nodeKind ? request.inserted_nodes : request.inserted_edges;
			stagedNewPools.emplace_back(newPool.request, std::move(page), std::move(mappings));
			newPoolRequests.push_back(std::move(request));
		}
		const bool hasLabelSetAuxiliary =
				std::ranges::any_of(auxiliary.replacements,
									[](const EntityPageAuxiliaryReplacement &replacement) {
										return replacement.owner_kind == PageOwnerKind::POK_ENTITY_LABEL;
									}) ||
				std::ranges::any_of(auxiliary.appends,
									[](const EntityPageAuxiliaryAppend &append) {
										return append.owner_kind == PageOwnerKind::POK_ENTITY_LABEL;
									}) ||
				std::ranges::any_of(auxiliary.new_pools, [](const EntityPageAuxiliaryNewPool &newPool) {
					return newPool.owner_kind == PageOwnerKind::POK_ENTITY_LABEL;
				});
		if (hasLabelSetAuxiliary && auxiliary.node_update_bindings.empty() &&
			auxiliary.node_insertion_bindings.empty()) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "native Entity page batch LabelSet frames require the Node binding that makes them reachable");
		}
		std::set<uint64_t> boundNodeIds;
		for (const EntityPageAuxiliaryNodeUpdateBinding &binding: auxiliary.node_update_bindings) {
			const uint8_t malformed =
					static_cast<uint8_t>(binding.node_id == 0U) |
					static_cast<uint8_t>(!binding.expected_location.page_id.isValid()) |
					static_cast<uint8_t>(binding.expected_location.slot_id == std::numeric_limits<uint16_t>::max()) |
					static_cast<uint8_t>(binding.expected_location.slot_generation == 0U) |
					static_cast<uint8_t>(!boundNodeIds.insert(binding.node_id).second);
			if (malformed != 0U) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "native Entity page batch LabelSet binding has an incomplete or duplicate Node identity");
			}
			validateRecord(binding.replacement_record);
			if (metadata_.findNodePageLocation(snapshot, binding.node_id) != binding.expected_location) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native Entity page batch LabelSet binding is stale against the pinned Node Entity Map");
			}
			size_t matchingUpdates = 0U;
			for (const EntityPageBatchMutation &mutation: mutations) {
				if (mutation.record_kind != EntityPageRecordKind::EPRK_NODE ||
					mutation.page_id != binding.expected_location.page_id) {
					continue;
				}
				for (const EntityPageNodeUpdate &update: mutation.node_updates) {
					if (update.node_id == binding.node_id) {
						if (update.record != binding.replacement_record) {
							fail(FormatError::FERR_INVALID_STATE, "native Entity page batch LabelSet binding disagrees "
																  "with its Node replacement record");
						}
						++matchingUpdates;
					}
				}
			}
			if (matchingUpdates != 1U) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native Entity page batch LabelSet binding requires exactly one matching Node update");
			}
		}
		for (const EntityPageAuxiliaryNodeInsertionBinding &binding: auxiliary.node_insertion_bindings) {
			const uint8_t malformed = static_cast<uint8_t>(binding.node_id == 0U) |
									  static_cast<uint8_t>(binding.inserted_record.label_overflow.isEmpty()) |
									  static_cast<uint8_t>(!boundNodeIds.insert(binding.node_id).second);
			if (malformed != 0U) {
				fail(FormatError::FERR_INVALID_HEADER, "native Entity page batch LabelSet insertion binding has an "
													   "incomplete or duplicate Node identity");
			}
			validateRecord(binding.inserted_record);
			size_t matchingInsertions = 0U;
			const auto matchInsertion = [&binding, &matchingInsertions](const EntityPageNodeInsertion &insertion) {
				if (insertion.node_id != binding.node_id) {
					return;
				}
				if (insertion.record != binding.inserted_record) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native Entity page batch LabelSet insertion binding disagrees with its Node record");
				}
				++matchingInsertions;
			};
			for (const EntityPageBatchMutation &mutation: mutations) {
				if (mutation.record_kind == EntityPageRecordKind::EPRK_NODE) {
					for (const EntityPageNodeInsertion &insertion: mutation.node_insertions) {
						matchInsertion(insertion);
					}
				}
			}
			for (const EntityPageBatchAppend &append: appends) {
				if (append.record_kind == EntityPageRecordKind::EPRK_NODE) {
					for (const EntityPageNodeInsertion &insertion: append.node_insertions) {
						matchInsertion(insertion);
					}
				}
			}
			for (const EntityPageBatchNewPool &newPool: newPools) {
				if (newPool.record_kind == EntityPageRecordKind::EPRK_NODE) {
					for (const EntityPageNodeInsertion &insertion: newPool.node_insertions) {
						matchInsertion(insertion);
					}
				}
			}
			if (matchingInsertions != 1U) {
				fail(FormatError::FERR_INVALID_STATE, "native Entity page batch LabelSet insertion binding requires "
													  "exactly one matching Node insertion");
			}
		}
		for (const EntityPageAuxiliaryReplacement &replacement: auxiliary.replacements) {
			const uint8_t invalid =
					static_cast<uint8_t>(!replacement.expected_pool.segment_id.isValid()) |
					static_cast<uint8_t>(!replacement.expected_pool.physical_slot.isValid()) |
					static_cast<uint8_t>(replacement.expected_pool.kind != SegmentKind::SGK_ENTITY_PAGE_POOL) |
					static_cast<uint8_t>(replacement.expected_pool.owner_id == 0U) |
					static_cast<uint8_t>(!replacement.expected_pool.generation.isValid()) |
					static_cast<uint8_t>(replacement.expected_pool.state != SegmentState::SGS_SEALED) |
					static_cast<uint8_t>(!replacement.expected_location.page_id.isValid()) |
					static_cast<uint8_t>(replacement.expected_location.segment_id !=
										 replacement.expected_pool.segment_id) |
					static_cast<uint8_t>(replacement.expected_location.generation !=
										 replacement.expected_pool.generation) |
					static_cast<uint8_t>(replacement.expected_location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
					static_cast<uint8_t>(replacement.expected_location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
					static_cast<uint8_t>(!replacement.expected_owner_lsn.isValid()) |
					static_cast<uint8_t>(!isAuxiliaryEntityPoolPageOwnerKind(replacement.owner_kind)) |
					static_cast<uint8_t>(replacement.free_bytes > RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES) |
					static_cast<uint8_t>(replacement.retire_page &&
										 (!replacement.payload.empty() || replacement.free_bytes != 0U)) |
					static_cast<uint8_t>(!replacement.retire_page &&
										 replacement.payload.size() != RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES) |
					static_cast<uint8_t>(!pageIds.insert(replacement.expected_location.page_id.value()).second);
			if (invalid != 0U) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "native Entity page batch received an invalid auxiliary replacement frame");
			}
			if (!replacement.retire_page) {
				validateAuxiliaryPayload(replacement.expected_pool, replacement.expected_location.page_id,
										 replacement.owner_kind, replacement.payload, replacement.free_bytes,
										 checkpointLsn);
			}
			EntityPageReplacementRequest request;
			request.expected_pool = replacement.expected_pool;
			request.replacement_pool = replacement.replacement_pool;
			request.replacement_new_pool = replacement.replacement_new_pool;
			request.expected_location = replacement.expected_location;
			request.retire_page = replacement.retire_page;
			request.owner_kind = replacement.owner_kind;
			requests.push_back(std::move(request));
			stagedAuxiliaryReplacements.push_back(replacement);
		}
		for (const EntityPageAuxiliaryAppend &append: auxiliary.appends) {
			const uint8_t invalid =
					static_cast<uint8_t>(!append.expected_pool.segment_id.isValid()) |
					static_cast<uint8_t>(!append.expected_pool.physical_slot.isValid()) |
					static_cast<uint8_t>(append.expected_pool.kind != SegmentKind::SGK_ENTITY_PAGE_POOL) |
					static_cast<uint8_t>(append.expected_pool.owner_id == 0U) |
					static_cast<uint8_t>(!append.expected_pool.generation.isValid()) |
					static_cast<uint8_t>(append.expected_pool.state != SegmentState::SGS_SEALED) |
					static_cast<uint8_t>(!append.page_id.isValid()) |
					static_cast<uint8_t>(!isAuxiliaryEntityPoolPageOwnerKind(append.owner_kind)) |
					static_cast<uint8_t>(append.payload.size() != RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES) |
					static_cast<uint8_t>(append.free_bytes > RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES) |
					static_cast<uint8_t>(!pageIds.insert(append.page_id.value()).second);
			if (invalid != 0U) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "native Entity page batch received an invalid auxiliary append frame");
			}
			validateAuxiliaryPayload(append.expected_pool, append.page_id, append.owner_kind, append.payload,
									 append.free_bytes, checkpointLsn);
			EntityPageAppendBatchRequest request;
			request.expected_pool = append.expected_pool;
			request.page_id = append.page_id;
			request.owner_kind = append.owner_kind;
			appendRequests.push_back(std::move(request));
			stagedAuxiliaryAppends.push_back(append);
		}
		for (const EntityPageAuxiliaryNewPool &newPool: auxiliary.new_pools) {
			const uint8_t invalid = static_cast<uint8_t>(!newPool.segment_id.isValid()) |
									static_cast<uint8_t>(newPool.pool_owner_id == 0U) |
									static_cast<uint8_t>(!newPool.generation.isValid()) |
									static_cast<uint8_t>(!newPool.page_id.isValid()) |
									static_cast<uint8_t>(!newPool.first_slot.isValid()) |
									static_cast<uint8_t>(!newPool.last_slot.isValid()) |
									static_cast<uint8_t>(newPool.first_slot > newPool.last_slot) |
									static_cast<uint8_t>(!isAuxiliaryEntityPoolPageOwnerKind(newPool.owner_kind)) |
									static_cast<uint8_t>(newPool.payload.size() != RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES) |
									static_cast<uint8_t>(newPool.free_bytes > RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES) |
									static_cast<uint8_t>(!pageIds.insert(newPool.page_id.value()).second);
			if (invalid != 0U) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "native Entity page batch received an invalid auxiliary new-pool frame");
			}
			CatalogEntry prospectivePool;
			prospectivePool.segment_id = newPool.segment_id;
			prospectivePool.kind = SegmentKind::SGK_ENTITY_PAGE_POOL;
			prospectivePool.owner_id = newPool.pool_owner_id;
			prospectivePool.generation = newPool.generation;
			prospectivePool.state = SegmentState::SGS_SEALED;
			prospectivePool.committed_lsn = checkpointLsn;
			validateAuxiliaryPayload(prospectivePool, newPool.page_id, newPool.owner_kind, newPool.payload,
									 newPool.free_bytes, checkpointLsn);
			EntityPageNewPoolBatchRequest request;
			request.reservation.segment_id = newPool.segment_id;
			request.reservation.kind = SegmentKind::SGK_ENTITY_PAGE_POOL;
			request.reservation.owner_id = newPool.pool_owner_id;
			request.reservation.segment_generation = newPool.generation;
			request.reservation.state = SegmentState::SGS_SEALED;
			request.reservation.require_absent = true;
			request.page_id = newPool.page_id;
			request.first_slot = newPool.first_slot;
			request.last_slot = newPool.last_slot;
			request.owner_kind = newPool.owner_kind;
			newPoolRequests.push_back(std::move(request));
			stagedAuxiliaryNewPools.push_back(newPool);
		}

		std::vector<EntityPageReplacementMaterialization> published;
		std::vector<EntityPageAppendBatchMaterialization> publishedAppends;
		std::vector<EntityPageNewPoolBatchMaterialization> publishedNewPools;
		std::vector<NativeGraphPropertyPageReplacementMaterialization> publishedProperties;
		std::vector<NativeGraphPropertyPageBatchMaterialization> publishedPropertyPageBatches;
		std::set<SegmentId> preparedPropertyPageBatches;
		const NativeGraphDerivedStateMutation noDerivedState{};
		const NativeGraphDerivedStatePreparer noDerivedStatePreparer{};
		const NativeGraphVectorBootstrapPreparer noVectorBootstrapPreparer{};
		const NativeGraphVectorDeltaPreparer noVectorDeltaPreparer{};
		const NativeGraphPrivateSegmentImagePreparer noPrivateSegmentImagePreparer{};
		const NativeGraphPrivateSegmentPreparer noPrivateSegmentPreparer{};
		const NativeGraphEarlyPublicationPlanner noEarlyPublicationPlanner{};
		const NativeGraphDeferredIndexPlanner noDeferredIndexPlanner{};
		const NativeGraphDerivedStateMutation &activeDerivedState = derivedState != nullptr ? *derivedState : noDerivedState;
		const NativeGraphDerivedStatePreparer &activeDerivedStatePreparer =
				hasDerivedState ? *derivedStatePreparer : noDerivedStatePreparer;
		const NativeGraphVectorBootstrapPreparer &activeVectorBootstrapPreparer =
				hasVectorBootstrap ? *vectorBootstrapPreparer : noVectorBootstrapPreparer;
		const NativeGraphVectorDeltaPreparer &activeVectorDeltaPreparer =
				hasVectorDelta ? *vectorDeltaPreparer : noVectorDeltaPreparer;
		const NativeGraphDeferredIndexPlanner &activeDeferredIndexPlanner =
				deferredIndexPlanner != nullptr ? *deferredIndexPlanner : noDeferredIndexPlanner;
		const NativeGraphPrivateSegmentImagePreparer &activePrivateSegmentImagePreparer =
				privateSegmentImagePreparer != nullptr ? *privateSegmentImagePreparer : noPrivateSegmentImagePreparer;
		const NativeGraphPrivateSegmentPreparer &activePrivateSegmentPreparer =
				privateSegmentPreparer != nullptr ? *privateSegmentPreparer : noPrivateSegmentPreparer;
		const NativeGraphEarlyPublicationPlanner &activeEarlyPublicationPlanner =
				earlyPublicationPlanner != nullptr ? *earlyPublicationPlanner : noEarlyPublicationPlanner;
		Superblock superblock;
		try {
			superblock = metadata_.mutateNativeGraphPagesWithLargeValues(
					requests, appendRequests, newPoolRequests, propertyReplacementRequests, propertyPageBatchRequests,
					largeValueMutations, activeDerivedState, checkpointLsn,
					[this, &stagedPages, &stagedAppends, &stagedNewPools, &stagedAuxiliaryReplacements,
					 &stagedAuxiliaryAppends, &stagedAuxiliaryNewPools, checkpointLsn, &published, &publishedAppends,
					 &publishedNewPools, &publishedProperties, &publishedPropertyPageBatches, &largeValueMutations,
					 &preparedPropertyPageBatches, &propertyPlans, &replacementPropertyPlanOrdinals,
					 &propertyPageBatchPlanOrdinals, propertyRoots](
							std::span<EntityPageReplacementMaterialization> pages,
							std::span<EntityPageAppendBatchMaterialization> materializedAppends,
							std::span<EntityPageNewPoolBatchMaterialization> materializedNewPools,
							std::span<NativeGraphPropertyPageReplacementMaterialization> materializedProperties,
							std::span<NativeGraphPropertyPageBatchMaterialization> materializedPropertyPageBatches,
							std::span<LargeValueMaterializationResult> materializedLargeValues) {
						const uint8_t mismatchedMaterializationCount =
								static_cast<uint8_t>(pages.size() !=
													 stagedPages.size() + stagedAuxiliaryReplacements.size()) |
								static_cast<uint8_t>(materializedAppends.size() !=
													 stagedAppends.size() + stagedAuxiliaryAppends.size()) |
								static_cast<uint8_t>(materializedNewPools.size() !=
													 stagedNewPools.size() + stagedAuxiliaryNewPools.size()) |
								static_cast<uint8_t>(materializedProperties.size() !=
													 replacementPropertyPlanOrdinals.size()) |
								static_cast<uint8_t>(materializedPropertyPageBatches.size() !=
														 propertyPageBatchPlanOrdinals.size()) |
								static_cast<uint8_t>(materializedLargeValues.size() != largeValueMutations.size());
						if (mismatchedMaterializationCount != 0U) {
							fail(FormatError::FERR_INVALID_STATE,
								 "relocatable Entity page batch metadata materialization count is inconsistent");
						}
						if (!materializedLargeValues.empty())
							propertyRoots->preparePlannedLargeValues(largeValueMutations, materializedLargeValues, checkpointLsn);
						struct PoolWork {
							CatalogEntry pool;
							bool destination_is_new_pool = false;
							std::vector<size_t> replacement_ordinals;
							std::vector<size_t> append_ordinals;
							std::vector<size_t> new_pool_ordinals;
							std::vector<size_t> auxiliary_replacement_ordinals;
							std::vector<size_t> auxiliary_append_ordinals;
							std::vector<size_t> auxiliary_new_pool_ordinals;
						};
						std::map<uint64_t, PoolWork> workByPool;
						const auto workFor = [&workByPool](const CatalogEntry &pool) -> PoolWork & {
							auto [iterator, inserted] = workByPool.try_emplace(
									pool.segment_id.value(), PoolWork{pool, false, {}, {}, {}, {}, {}, {}});
							if (!inserted && !sameCatalogEntry(iterator->second.pool, pool)) {
								fail(FormatError::FERR_INVALID_STATE,
									 "relocatable Entity page batch materialized conflicting pool identities");
							}
							return iterator->second;
						};
						for (size_t index = 0U; index < stagedPages.size(); ++index) {
							StagedPage &staged = stagedPages[index];
							if (pages[index].retire_page != staged.retire_page) {
								fail(FormatError::FERR_INVALID_STATE,
									 "relocatable Entity page batch metadata retirement plan is inconsistent");
							}
							PoolWork &work = workFor(pages[index].pool);
							work.destination_is_new_pool =
									work.destination_is_new_pool || pages[index].destination_is_new_pool;
							work.replacement_ordinals.push_back(index);
						}
						for (size_t index = 0U; index < stagedAppends.size(); ++index) {
							if (!sameCatalogEntry(materializedAppends[index].pool, stagedAppends[index].pool)) {
								fail(FormatError::FERR_INVALID_STATE,
									 "relocatable Entity page batch append materialized another pool");
							}
							workFor(materializedAppends[index].pool).append_ordinals.push_back(index);
						}
						for (size_t index = 0U; index < stagedNewPools.size(); ++index) {
							const CatalogEntry &pool = materializedNewPools[index].pool;
							const EntityPageNewPoolRequest &request = stagedNewPools[index].request;
							const uint8_t invalid =
									static_cast<uint8_t>(pool.segment_id != request.segment_id) |
									static_cast<uint8_t>(pool.kind != SegmentKind::SGK_ENTITY_PAGE_POOL) |
									static_cast<uint8_t>(pool.owner_id != request.pool_owner_id) |
									static_cast<uint8_t>(pool.generation != request.generation) |
									static_cast<uint8_t>(pool.state != SegmentState::SGS_SEALED) |
									static_cast<uint8_t>(pool.committed_lsn != checkpointLsn) |
									static_cast<uint8_t>(materializedNewPools[index].initial_location.page_id !=
														 request.page_id) |
									static_cast<uint8_t>(materializedNewPools[index].initial_location.segment_id !=
														 pool.segment_id) |
									static_cast<uint8_t>(materializedNewPools[index].initial_location.generation !=
														 pool.generation);
							if (invalid != 0U) {
								fail(FormatError::FERR_INVALID_STATE,
									 "relocatable Entity page batch materialized an invalid new-pool identity");
							}
							PoolWork &work = workFor(pool);
							work.destination_is_new_pool = true;
							work.new_pool_ordinals.push_back(index);
						}
						const size_t auxiliaryReplacementOffset = stagedPages.size();
						const size_t auxiliaryAppendOffset = stagedAppends.size();
						const size_t auxiliaryNewPoolOffset = stagedNewPools.size();
						for (size_t index = 0U; index < stagedAuxiliaryReplacements.size(); ++index) {
							const EntityPageAuxiliaryReplacement &staged = stagedAuxiliaryReplacements[index];
							const EntityPageReplacementMaterialization &materialized =
									pages[auxiliaryReplacementOffset + index];
							const bool usesFreshDestination = staged.replacement_new_pool.has_value();
							const CatalogEntry &expectedDestination = staged.replacement_pool.segment_id.isValid()
																			  ? staged.replacement_pool
																			  : staged.expected_pool;
							const uint8_t invalid =
									static_cast<uint8_t>(!usesFreshDestination &&
														 !sameCatalogEntry(materialized.pool, expectedDestination)) |
									static_cast<uint8_t>(
											usesFreshDestination &&
											(materialized.pool.segment_id != staged.replacement_new_pool->segment_id ||
											 materialized.pool.owner_id != staged.replacement_new_pool->pool_owner_id ||
											 materialized.pool.generation != staged.replacement_new_pool->generation ||
											 !materialized.destination_is_new_pool)) |
									static_cast<uint8_t>(materialized.expected_location != staged.expected_location) |
									static_cast<uint8_t>(materialized.retire_page != staged.retire_page) |
									static_cast<uint8_t>(materialized.owner_kind != staged.owner_kind);
							if (invalid != 0U) {
								fail(FormatError::FERR_INVALID_STATE,
									 "native Entity page batch materialized another auxiliary replacement identity");
							}
							PoolWork &work = workFor(materialized.pool);
							work.destination_is_new_pool =
									work.destination_is_new_pool || materialized.destination_is_new_pool;
							work.auxiliary_replacement_ordinals.push_back(index);
						}
						for (size_t index = 0U; index < stagedAuxiliaryAppends.size(); ++index) {
							const EntityPageAuxiliaryAppend &staged = stagedAuxiliaryAppends[index];
							const EntityPageAppendBatchMaterialization &materialized =
									materializedAppends[auxiliaryAppendOffset + index];
							if (!sameCatalogEntry(materialized.pool, staged.expected_pool) ||
								materialized.appended_location.page_id != staged.page_id ||
								materialized.owner_kind != staged.owner_kind) {
								fail(FormatError::FERR_INVALID_STATE,
									 "native Entity page batch materialized another auxiliary append identity");
							}
							workFor(materialized.pool).auxiliary_append_ordinals.push_back(index);
						}
						for (size_t index = 0U; index < stagedAuxiliaryNewPools.size(); ++index) {
							const EntityPageAuxiliaryNewPool &staged = stagedAuxiliaryNewPools[index];
							const EntityPageNewPoolBatchMaterialization &materialized =
									materializedNewPools[auxiliaryNewPoolOffset + index];
							const CatalogEntry &pool = materialized.pool;
							const uint8_t invalid =
									static_cast<uint8_t>(pool.segment_id != staged.segment_id) |
									static_cast<uint8_t>(pool.kind != SegmentKind::SGK_ENTITY_PAGE_POOL) |
									static_cast<uint8_t>(pool.owner_id != staged.pool_owner_id) |
									static_cast<uint8_t>(pool.generation != staged.generation) |
									static_cast<uint8_t>(pool.state != SegmentState::SGS_SEALED) |
									static_cast<uint8_t>(pool.committed_lsn != checkpointLsn) |
									static_cast<uint8_t>(materialized.initial_location.page_id != staged.page_id) |
									static_cast<uint8_t>(materialized.initial_location.segment_id != pool.segment_id) |
									static_cast<uint8_t>(materialized.initial_location.generation != pool.generation) |
									static_cast<uint8_t>(materialized.owner_kind != staged.owner_kind);
							if (invalid != 0U) {
								fail(FormatError::FERR_INVALID_STATE,
									 "native Entity page batch materialized another auxiliary new-pool identity");
							}
							PoolWork &work = workFor(pool);
							work.destination_is_new_pool = true;
							work.auxiliary_new_pool_ordinals.push_back(index);
						}
						for (auto &[segmentId, work]: workByPool) {
							static_cast<void>(segmentId);
							if (work.destination_is_new_pool) {
								if (!work.append_ordinals.empty() || !work.auxiliary_append_ordinals.empty()) {
									fail(FormatError::FERR_INVALID_STATE, "relocatable Entity page batch appended "
																		  "directly to an unpublished Entity pool");
								}
								const size_t newPoolCount =
										work.replacement_ordinals.size() + work.auxiliary_replacement_ordinals.size() +
										work.new_pool_ordinals.size() + work.auxiliary_new_pool_ordinals.size();
								if (newPoolCount == 0U || newPoolCount > RSS_PAGE_POOL_DATA_FRAME_COUNT) {
									fail(FormatError::FERR_INVALID_STATE,
										 "relocatable Entity page batch has an invalid fresh-pool frame count");
								}
								std::vector<SharedPagePoolPreparedAppend> initialPages;
								std::vector<PageLocation> initialLocations;
								std::vector<size_t> replacementResultOrdinals;
								std::vector<size_t> auxiliaryReplacementResultOrdinals;
								initialPages.reserve(newPoolCount);
								initialLocations.reserve(newPoolCount);
								replacementResultOrdinals.reserve(work.replacement_ordinals.size());
								auxiliaryReplacementResultOrdinals.reserve(work.auxiliary_replacement_ordinals.size());
								for (const size_t ordinal: work.replacement_ordinals) {
									const EntityPageReplacementMaterialization &materialization = pages[ordinal];
									const StagedPage &staged = stagedPages[ordinal];
									if (staged.retire_page || !materialization.destination_is_new_pool) {
										fail(FormatError::FERR_INVALID_STATE,
											 "relocatable Entity page batch mixed a retirement into a fresh COW pool");
									}
									const EntityPageHeader header = staged.loaded.page.header();
									const auto owner = ownerFor(header.page_id, work.pool.owner_id, work.pool.generation, checkpointLsn);
                                initialPages.push_back({owner, serializeEntityForOwner(staged.loaded.page, owner), 0U});
									initialLocations.push_back(unpublishedLocation(header.page_id, work.pool.segment_id,
																				   work.pool.generation));
									replacementResultOrdinals.push_back(ordinal);
								}
								for (const size_t ordinal: work.auxiliary_replacement_ordinals) {
									const EntityPageAuxiliaryReplacement &staged = stagedAuxiliaryReplacements[ordinal];
									const EntityPageReplacementMaterialization &materialization =
											pages[auxiliaryReplacementOffset + ordinal];
									if (staged.retire_page || !materialization.destination_is_new_pool) {
										fail(FormatError::FERR_INVALID_STATE,
											 "relocatable Entity page batch mixed an auxiliary retirement into a fresh "
											 "COW pool");
									}
									initialPages.push_back(
											{auxiliaryOwnerFor(staged.expected_location.page_id, work.pool,
															   staged.owner_kind, checkpointLsn),
											 serializeAuxiliaryForOwner(staged.expected_pool, staged.payload,
                                                     auxiliaryOwnerFor(staged.expected_location.page_id, work.pool,
                                                         staged.owner_kind, checkpointLsn)), staged.free_bytes});
									initialLocations.push_back(unpublishedLocation(staged.expected_location.page_id,
																				   work.pool.segment_id,
																				   work.pool.generation));
									auxiliaryReplacementResultOrdinals.push_back(ordinal);
								}
								for (const size_t ordinal: work.new_pool_ordinals) {
									const EntityPage &page = stagedNewPools[ordinal].page;
									const EntityPageHeader header = page.header();
									initialPages.push_back({ownerFor(header.page_id, work.pool.owner_id,
																	 work.pool.generation, checkpointLsn),
															page.serialize(), 0U});
									initialLocations.push_back(unpublishedLocation(header.page_id, work.pool.segment_id,
																				   work.pool.generation));
								}
								for (const size_t ordinal: work.auxiliary_new_pool_ordinals) {
									const EntityPageAuxiliaryNewPool &staged = stagedAuxiliaryNewPools[ordinal];
									initialPages.push_back({auxiliaryOwnerFor(staged.page_id, work.pool,
																			  staged.owner_kind, checkpointLsn),
															staged.payload, staged.free_bytes});
									initialLocations.push_back(unpublishedLocation(staged.page_id, work.pool.segment_id,
																				   work.pool.generation));
								}
								allocator_.prepareNewPoolPages(work.pool, initialPages, initialLocations,
															   checkpointLsn);
								for (size_t index = 0U; index < replacementResultOrdinals.size(); ++index) {
									pages[replacementResultOrdinals[index]].replacement = initialLocations[index];
								}
								for (size_t index = 0U; index < auxiliaryReplacementResultOrdinals.size(); ++index) {
									pages[auxiliaryReplacementOffset + auxiliaryReplacementResultOrdinals[index]]
											.replacement = initialLocations[replacementResultOrdinals.size() + index];
								}
								for (size_t index = 0U; index < work.new_pool_ordinals.size(); ++index) {
									materializedNewPools[work.new_pool_ordinals[index]].initial_location =
											initialLocations[replacementResultOrdinals.size() +
															 auxiliaryReplacementResultOrdinals.size() + index];
								}
								for (size_t index = 0U; index < work.auxiliary_new_pool_ordinals.size(); ++index) {
									materializedNewPools[auxiliaryNewPoolOffset +
														 work.auxiliary_new_pool_ordinals[index]]
											.initial_location =
											initialLocations[replacementResultOrdinals.size() +
															 auxiliaryReplacementResultOrdinals.size() +
															 work.new_pool_ordinals.size() + index];
								}
								std::map<uint64_t,
										 std::pair<CatalogEntry, std::vector<SharedPagePoolPreparedRetirement>>>
										retirementsBySourcePool;
								const auto retireSource =
										[&retirementsBySourcePool](const CatalogEntry &source, PageLocation location,
																   uint64_t ownerId, PageOwnerKind ownerKind,
																   LogSequenceNumber ownerLsn) {
											auto [iterator, inserted] = retirementsBySourcePool.try_emplace(
													source.segment_id.value(), source,
													std::vector<SharedPagePoolPreparedRetirement>{});
											if (!inserted && !sameCatalogEntry(iterator->second.first, source)) {
												fail(FormatError::FERR_INVALID_STATE,
													 "relocatable Entity page batch has conflicting fresh-COW source "
													 "identities");
											}
											iterator->second.second.push_back({location, ownerId, ownerKind, ownerLsn});
										};
								for (const size_t ordinal: replacementResultOrdinals) {
									const StagedPage &staged = stagedPages[ordinal];
									retireSource(staged.loaded.contents.pool, staged.loaded.contents.location,
												 staged.loaded.contents.pool.owner_id, PageOwnerKind::POK_ENTITY,
												 staged.loaded.contents.owner.committed_lsn);
								}
								for (const size_t ordinal: auxiliaryReplacementResultOrdinals) {
									const EntityPageAuxiliaryReplacement &staged = stagedAuxiliaryReplacements[ordinal];
									retireSource(staged.expected_pool, staged.expected_location,
												 staged.expected_pool.owner_id, staged.owner_kind,
												 staged.expected_owner_lsn);
								}
								for (auto &[sourceId, source]: retirementsBySourcePool) {
									static_cast<void>(sourceId);
									const std::vector<PageLocation> noReplacementSources;
									const std::vector<SharedPagePoolPreparedReplacement> noReplacements;
									const std::vector<SharedPagePoolPreparedAppend> noAppends;
									std::vector<PageLocation> noReplacementLocations;
									std::vector<PageLocation> noAppendLocations;
									allocator_.prepareMutationPages(source.first, noReplacementSources, noReplacements,
																	noAppends, noReplacementLocations,
																	noAppendLocations, checkpointLsn, source.second);
								}
								continue;
							}
							std::vector<SharedPagePoolPreparedCrossPoolReplacement> replacements;
							std::vector<SharedPagePoolPreparedAppend> appendPayloads;
							std::vector<PageLocation> replacementLocations;
							std::vector<PageLocation> appendLocations;
							std::vector<SharedPagePoolPreparedRetirement> retirements;
							std::vector<size_t> replacementResultOrdinals;
							std::vector<size_t> auxiliaryReplacementResultOrdinals;
							replacements.reserve(work.replacement_ordinals.size() +
												 work.auxiliary_replacement_ordinals.size());
							replacementLocations.reserve(work.replacement_ordinals.size() +
														 work.auxiliary_replacement_ordinals.size());
							retirements.reserve(work.replacement_ordinals.size() +
												work.auxiliary_replacement_ordinals.size());
							for (const size_t ordinal: work.replacement_ordinals) {
								const EntityPageReplacementMaterialization &materialization = pages[ordinal];
								const StagedPage &staged = stagedPages[ordinal];
								if (staged.retire_page) {
									retirements.push_back(
											{materialization.expected_location, staged.loaded.contents.pool.owner_id,
											 PageOwnerKind::POK_ENTITY, staged.loaded.contents.owner.committed_lsn});
									continue;
								}
								const EntityPageHeader header = staged.loaded.page.header();
								const PageOwnerHeader owner = ownerFor(header.page_id, work.pool.owner_id,
																	   work.pool.generation, checkpointLsn);
								replacements.push_back({staged.loaded.contents.pool, materialization.expected_location,
														staged.loaded.contents.owner.committed_lsn, owner,
														serializeEntityForOwner(staged.loaded.page, owner), 0U});
								replacementLocations.push_back(unpublishedLocation(header.page_id, work.pool.segment_id,
																				   work.pool.generation));
								replacementResultOrdinals.push_back(ordinal);
							}
							for (const size_t ordinal: work.auxiliary_replacement_ordinals) {
								const EntityPageAuxiliaryReplacement &staged = stagedAuxiliaryReplacements[ordinal];
								const EntityPageReplacementMaterialization &materialization =
										pages[auxiliaryReplacementOffset + ordinal];
								if (staged.retire_page) {
									retirements.push_back({materialization.expected_location,
														   staged.expected_pool.owner_id, staged.owner_kind,
														   staged.expected_owner_lsn});
									continue;
								}
								replacements.push_back({staged.expected_pool, materialization.expected_location,
														staged.expected_owner_lsn,
														auxiliaryOwnerFor(staged.expected_location.page_id, work.pool,
																		  staged.owner_kind, checkpointLsn),
														serializeAuxiliaryForOwner(staged.expected_pool, staged.payload,
                                                     auxiliaryOwnerFor(staged.expected_location.page_id, work.pool,
                                                         staged.owner_kind, checkpointLsn)), staged.free_bytes});
								replacementLocations.push_back(unpublishedLocation(
										staged.expected_location.page_id, work.pool.segment_id, work.pool.generation));
								auxiliaryReplacementResultOrdinals.push_back(ordinal);
							}
							std::vector<size_t> appendResultOrdinals;
							std::vector<size_t> auxiliaryAppendResultOrdinals;
							appendPayloads.reserve(work.append_ordinals.size() + work.auxiliary_append_ordinals.size());
							appendLocations.reserve(work.append_ordinals.size() +
													work.auxiliary_append_ordinals.size());
							appendResultOrdinals.reserve(work.append_ordinals.size());
							auxiliaryAppendResultOrdinals.reserve(work.auxiliary_append_ordinals.size());
							for (const size_t ordinal: work.append_ordinals) {
								const StagedAppendPage &staged = stagedAppends[ordinal];
								const EntityPageHeader header = staged.page.header();
								const PageOwnerHeader owner = ownerFor(header.page_id, work.pool.owner_id,
																	   work.pool.generation, checkpointLsn);
								appendPayloads.push_back({owner, staged.page.serialize(), 0U});
								appendLocations.push_back(unpublishedLocation(header.page_id, work.pool.segment_id,
																			  work.pool.generation));
								appendResultOrdinals.push_back(ordinal);
							}
							for (const size_t ordinal: work.auxiliary_append_ordinals) {
								const EntityPageAuxiliaryAppend &staged = stagedAuxiliaryAppends[ordinal];
								appendPayloads.push_back(
										{auxiliaryOwnerFor(staged.page_id, work.pool, staged.owner_kind, checkpointLsn),
										 staged.payload, staged.free_bytes});
								appendLocations.push_back(unpublishedLocation(staged.page_id, work.pool.segment_id,
																			  work.pool.generation));
								auxiliaryAppendResultOrdinals.push_back(ordinal);
							}
							if (!replacements.empty()) {
								allocator_.prepareCrossPoolReplacementPages(work.pool, replacements,
																			replacementLocations, checkpointLsn);
							}
							if (!appendPayloads.empty() || !retirements.empty()) {
								const std::vector<PageLocation> noReplacementSources;
								const std::vector<SharedPagePoolPreparedReplacement> noReplacements;
								std::vector<PageLocation> noReplacementLocations;
								allocator_.prepareMutationPages(work.pool, noReplacementSources, noReplacements,
																appendPayloads, noReplacementLocations, appendLocations,
																checkpointLsn, retirements);
							}
							for (size_t index = 0U; index < replacementResultOrdinals.size(); ++index) {
								pages[replacementResultOrdinals[index]].replacement = replacementLocations[index];
							}
							for (size_t index = 0U; index < auxiliaryReplacementResultOrdinals.size(); ++index) {
								pages[auxiliaryReplacementOffset + auxiliaryReplacementResultOrdinals[index]]
										.replacement = replacementLocations[replacementResultOrdinals.size() + index];
							}
							for (size_t index = 0U; index < appendResultOrdinals.size(); ++index) {
								materializedAppends[appendResultOrdinals[index]].appended_location =
										appendLocations[index];
							}
							for (size_t index = 0U; index < auxiliaryAppendResultOrdinals.size(); ++index) {
								materializedAppends[auxiliaryAppendOffset + auxiliaryAppendResultOrdinals[index]]
										.appended_location = appendLocations[appendResultOrdinals.size() + index];
							}
						}
						if ((!materializedProperties.empty() || !materializedPropertyPageBatches.empty()) &&
							propertyRoots == nullptr) {
							fail(FormatError::FERR_INVALID_STATE,
								 "native Entity page batch materialized Property work without a Property-root store");
						}
						for (size_t index = 0U; index < materializedProperties.size(); ++index) {
							propertyRoots->preparePlannedEntityRootReplacement(
									propertyPlans[replacementPropertyPlanOrdinals[index]],
									materializedProperties[index], checkpointLsn);
						}
						for (size_t index = 0U; index < materializedPropertyPageBatches.size(); ++index) {
							preparedPropertyPageBatches.insert(
									materializedPropertyPageBatches[index].pool.segment_id);
							propertyRoots->preparePlannedEntityRootPageBatch(
									propertyPlans, propertyPageBatchPlanOrdinals[index],
									materializedPropertyPageBatches[index], checkpointLsn);
						}
						published.assign(pages.begin(), pages.end());
						publishedAppends.assign(materializedAppends.begin(), materializedAppends.end());
						publishedNewPools.assign(materializedNewPools.begin(), materializedNewPools.end());
						publishedProperties.assign(materializedProperties.begin(), materializedProperties.end());
						publishedPropertyPageBatches.assign(materializedPropertyPageBatches.begin(),
														 materializedPropertyPageBatches.end());
					},
					activeDerivedStatePreparer, activeVectorBootstrapPreparer, activeVectorDeltaPreparer, {}, {}, {},
					options, logicalIdReservation, snapshot.superblock().generation,
					activePrivateSegmentImagePreparer, activePrivateSegmentPreparer, activeDeferredIndexPlanner,
                    activeEarlyPublicationPlanner, {}, std::nullopt, std::nullopt, nullptr, hybridTopology);
		} catch (...) {
            // This adapter cannot authorize ordinary pool reconciliation for
            // native graphs. Keep the original publication error; the typed
            // metadata writer/recovery owns native candidate-frame cleanup.
            if (hybridTopology) throw;
			// A callback may already have written a candidate frame when another pool
			// fails or A/B Superblock publication is interrupted. Once metadata releases
			// its writer guard, reconciliation restores the currently reachable generation.
			std::set<SegmentId> touchedPools;
			for (const StagedPage &staged: stagedPages) {
				touchedPools.insert(staged.loaded.contents.pool.segment_id);
			}
			for (const EntityPageBatchMutation &mutation: mutations) {
				if (mutation.replacement_pool.segment_id.isValid()) {
					touchedPools.insert(mutation.replacement_pool.segment_id);
				}
			}
			for (const StagedAppendPage &staged: stagedAppends) {
				touchedPools.insert(staged.pool.segment_id);
			}
			for (const EntityPageAuxiliaryReplacement &staged: stagedAuxiliaryReplacements) {
				touchedPools.insert(staged.expected_pool.segment_id);
				if (staged.replacement_pool.segment_id.isValid()) {
					touchedPools.insert(staged.replacement_pool.segment_id);
				}
			}
			for (const EntityPageAuxiliaryAppend &staged: stagedAuxiliaryAppends) {
				touchedPools.insert(staged.expected_pool.segment_id);
			}
			for (const NativeGraphPropertyPageReplacementRequest &request: propertyReplacementRequests) {
				touchedPools.insert(request.expected_pool.segment_id);
			}
			MetadataSnapshotPin recovered = metadata_.openSnapshot();
			const std::vector<CatalogEntry> recoveredPools = metadata_.findCurrentEntityPagePools(recovered);
			for (const StagedNewPoolPage &staged: stagedNewPools) {
				if (std::ranges::any_of(recoveredPools, [&staged](const CatalogEntry &pool) {
						return pool.segment_id == staged.request.segment_id;
					})) {
					touchedPools.insert(staged.request.segment_id);
				}
			}
			for (const EntityPageAuxiliaryNewPool &staged: stagedAuxiliaryNewPools) {
				if (std::ranges::any_of(recoveredPools, [&staged](const CatalogEntry &pool) {
						return pool.segment_id == staged.segment_id;
					})) {
					touchedPools.insert(staged.segment_id);
				}
			}
			for (const NativeGraphPropertyPageBatchRequest &request: propertyPageBatchRequests) {
				for (const CatalogEntry &pool:
					 metadata_.findCurrentPropertyPagePools(recovered, request.page_owner_id)) {
					if (preparedPropertyPageBatches.contains(pool.segment_id)) {
						touchedPools.insert(pool.segment_id);
					}
				}
			}
			if (hasDerivedState) {
				// The derived-state callback can write candidate B+Tree frames in any
				// current index pool before a later failure prevents publication. Scan
				// the recovered catalog so reconciliation cannot leave one such frame
				// looking reusable with its pre-write frame ownership.
				for (const CatalogEntry &pool: metadata_.findCurrentIndexPagePools(recovered)) {
					touchedPools.insert(pool.segment_id);
				}
			}
			recovered.release();
			for (const SegmentId segmentId: touchedPools) {
				allocator_.reclaimRetiredFrames(segmentId);
			}
			throw;
		}
		EntityPageBatchResult result;
		result.superblock = superblock;
		// Auxiliary LabelSet frames have no Entity Map identity and deliberately do
		// not leak through this Node/Edge-oriented result type.
		result.pages.reserve(stagedPages.size());
		for (size_t index = 0U; index < stagedPages.size(); ++index) {
			const EntityPageReplacementMaterialization &materialization = published[index];
			result.pages.push_back(
					{superblock, materialization.pool,
					 materialization.retire_page ? materialization.expected_location : materialization.replacement,
					 materialization.retire_page});
		}
		result.appended_pages.reserve(stagedAppends.size());
		for (size_t index = 0U; index < stagedAppends.size(); ++index) {
			result.appended_pages.push_back({publishedAppends[index].pool, publishedAppends[index].appended_location,
											 stagedAppends[index].mappings});
		}
		result.new_pool_pages.reserve(stagedNewPools.size());
		for (size_t index = 0U; index < stagedNewPools.size(); ++index) {
			result.new_pool_pages.push_back({publishedNewPools[index].pool, publishedNewPools[index].initial_location,
											 stagedNewPools[index].mappings});
		}
		result.properties.reserve(publishedProperties.size() + publishedPropertyPageBatches.size());
		for (size_t index = 0U; index < publishedProperties.size(); ++index) {
			const EntityPropertyRootMutationPlan &plan = propertyPlans[replacementPropertyPlanOrdinals[index]];
			const NativeGraphPropertyPageReplacementRequest &request = *plan.page_replacement;
			result.properties.insert(result.properties.end(), request.upserted_properties.begin(),
									 request.upserted_properties.end());
		}
		for (const NativeGraphPropertyPageBatchMaterialization &materialization: publishedPropertyPageBatches) {
			for (const NativeGraphPropertyPageInitialMaterialization &page: materialization.pages) {
				result.properties.insert(result.properties.end(), page.upserted_properties.begin(),
										 page.upserted_properties.end());
			}
		}
		std::ranges::sort(result.properties, {},
						  [](const EntityPropertyLocation &location) { return location.key.directoryId(); });
		return result;
	}

	EntityPageMutationResult EntityPageStore::eraseNode(const MetadataSnapshotPin &snapshot, uint64_t nodeId,
														LogSequenceNumber checkpointLsn,
														const MetadataPublishOptions &options) {
		return erase(snapshot, nodeId, EntityPageRecordKind::EPRK_NODE, checkpointLsn, options);
	}

	EntityPageMutationResult EntityPageStore::eraseEdge(const MetadataSnapshotPin &snapshot, uint64_t edgeId,
														LogSequenceNumber checkpointLsn,
														const MetadataPublishOptions &options) {
		return erase(snapshot, edgeId, EntityPageRecordKind::EPRK_EDGE, checkpointLsn, options);
	}

	EntityPageMutationResult EntityPageStore::evacuateNodePage(const MetadataSnapshotPin &snapshot, PageId pageId,
															   SegmentId destinationSegmentId,
															   LogSequenceNumber checkpointLsn,
															   const MetadataPublishOptions &options) {
		return evacuate(snapshot, pageId, destinationSegmentId, EntityPageRecordKind::EPRK_NODE, checkpointLsn,
						options);
	}

	EntityPageMutationResult EntityPageStore::evacuateEdgePage(const MetadataSnapshotPin &snapshot, PageId pageId,
															   SegmentId destinationSegmentId,
															   LogSequenceNumber checkpointLsn,
															   const MetadataPublishOptions &options) {
		return evacuate(snapshot, pageId, destinationSegmentId, EntityPageRecordKind::EPRK_EDGE, checkpointLsn,
						options);
	}

	EntityPageMutationResult EntityPageStore::evacuatePoolPage(const MetadataSnapshotPin &snapshot, PageId pageId,
															   SegmentId destinationSegmentId,
															   LogSequenceNumber checkpointLsn,
															   const MetadataPublishOptions &options) {
		if (!pageId.isValid() || !destinationSegmentId.isValid() || !checkpointLsn.isValid()) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "relocatable Entity-pool evacuation requires page, destination, and LSN identities");
		}
		const SharedPagePoolContents contents = allocator_.read(snapshot, pageId);
		const uint8_t invalid =
				static_cast<uint8_t>(contents.pool.kind != SegmentKind::SGK_ENTITY_PAGE_POOL) |
				static_cast<uint8_t>(contents.pool.state != SegmentState::SGS_SEALED) |
				static_cast<uint8_t>(contents.location.page_id != pageId) |
				static_cast<uint8_t>(contents.owner.page_id != pageId) |
				static_cast<uint8_t>(contents.owner.owner_id != contents.pool.owner_id) |
				static_cast<uint8_t>(contents.owner.generation != contents.pool.generation) |
				static_cast<uint8_t>(contents.owner.committed_lsn > snapshot.superblock().checkpoint_lsn);
		if (invalid != 0U) {
			fail(FormatError::FERR_INVALID_STATE,
				 "relocatable Entity-pool evacuation resolved an invalid source page ownership record");
		}
		switch (contents.owner.kind) {
			case PageOwnerKind::POK_ENTITY: {
				const EntityPageRecordKind kind =
						EntityPage::open(contents.owner, contents.payload).header().record_kind;
				return kind == EntityPageRecordKind::EPRK_NODE
							   ? evacuateNodePage(snapshot, pageId, destinationSegmentId, checkpointLsn, options)
							   : evacuateEdgePage(snapshot, pageId, destinationSegmentId, checkpointLsn, options);
			}
			case PageOwnerKind::POK_ENTITY_LABEL:
				return evacuateAuxiliaryPage(snapshot, pageId, destinationSegmentId, contents.owner.kind, checkpointLsn,
											 options);
			default:
				fail(FormatError::FERR_INVALID_STATE,
					 "relocatable Entity-pool evacuation encountered an unsupported page owner kind");
		}
	}

	uint16_t EntityPageStore::freeSlotCount(const MetadataSnapshotPin &snapshot, PageId pageId,
											EntityPageRecordKind expectedKind) const {
		LoadedPage loaded = loadPage(snapshot, pageId, expectedKind);
		const EntityPageHeader header = loaded.page.header();
		return static_cast<uint16_t>(header.slot_capacity - header.live_count);
	}

	uint8_t EntityPageStore::availableFrameCount(const MetadataSnapshotPin &snapshot,
												 const CatalogEntry &expectedPool) const {
		const CatalogEntry current = metadata_.findCatalog(snapshot, expectedPool.segment_id);
		if (!sameCatalogEntry(current, expectedPool) || current.kind != SegmentKind::SGK_ENTITY_PAGE_POOL ||
			current.state != SegmentState::SGS_SEALED) {
			fail(FormatError::FERR_INVALID_STATE,
				 "relocatable Entity page frame-capacity query requires the current sealed pool identity");
		}
		return allocator_.availableFrameCount(current);
	}

	std::vector<CatalogEntry> EntityPageStore::currentPools(const MetadataSnapshotPin &snapshot) const {
		return metadata_.findCurrentEntityPagePools(snapshot);
	}

	void EntityPageStore::reclaimRetiredFrames(SegmentId segmentId) const {
		allocator_.reclaimRetiredFrames(segmentId);
	}

	Superblock EntityPageStore::retireEmptyPool(SegmentId segmentId, Generation expectedGeneration,
												LogSequenceNumber checkpointLsn,
												const MetadataPublishOptions &options) {
		return allocator_.retireEmptyPool(segmentId, expectedGeneration, checkpointLsn, options);
	}

	Superblock EntityPageStore::releaseRetiredEmptyPool(SegmentId segmentId, Generation expectedGeneration,
														LogSequenceNumber checkpointLsn,
														const MetadataPublishOptions &options) {
		return allocator_.releaseRetiredEmptyPool(segmentId, expectedGeneration, checkpointLsn, options);
	}

    std::optional<Superblock> EntityPageStore::tryReleaseRetiredEmptyPool(SegmentId segmentId, Generation expectedGeneration,
														LogSequenceNumber checkpointLsn,
														const MetadataPublishOptions &options) {
        auto result = allocator_.tryReleaseRetiredEmptyPool(segmentId, expectedGeneration, checkpointLsn, options);
        return result;
    }

	EntityPageStore::LoadedPage EntityPageStore::loadPage(const MetadataSnapshotPin &snapshot, PageId pageId,
														  EntityPageRecordKind expectedKind,
														  BufferPoolReadHint hint) const {
		if (!pageId.isValid() || !isEntityPageKind(expectedKind)) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "relocatable Entity page read has an invalid Page ID or record kind");
		}
		SharedPagePoolContents contents = allocator_.read(snapshot, pageId, hint);
		const uint8_t invalid =
				static_cast<uint8_t>(contents.pool.kind != SegmentKind::SGK_ENTITY_PAGE_POOL) |
				static_cast<uint8_t>(contents.pool.owner_id == 0U) |
				static_cast<uint8_t>(contents.pool.state != SegmentState::SGS_SEALED) |
				static_cast<uint8_t>(contents.location.page_id != pageId) |
				static_cast<uint8_t>(contents.location.segment_id != contents.pool.segment_id) |
				static_cast<uint8_t>(contents.location.generation != contents.pool.generation) |
				static_cast<uint8_t>(contents.owner.page_id != pageId) |
				static_cast<uint8_t>(contents.owner.owner_id != contents.pool.owner_id) |
				static_cast<uint8_t>(contents.owner.kind != PageOwnerKind::POK_ENTITY) |
				static_cast<uint8_t>((contents.owner.flags & ~RSS_PAGE_OWNER_KNOWN_FLAGS) != 0U) |
				static_cast<uint8_t>(contents.owner.payload_bytes != RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES) |
				static_cast<uint8_t>(contents.owner.generation != contents.location.generation) |
				static_cast<uint8_t>(contents.owner.committed_lsn > snapshot.superblock().checkpoint_lsn);
		if (invalid != 0U) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable Entity page disagrees with its current pool, owner, or pinned metadata "
								  "snapshot: page=" +
										  std::to_string(pageId.value()) +
										  " pool=" + std::to_string(contents.pool.segment_id.value()) +
										  " location=" + std::to_string(contents.location.segment_id.value()) + "/" +
										  std::to_string(contents.location.page_index) +
										  " owner=" + std::to_string(contents.owner.owner_id) + "/" +
										  std::to_string(static_cast<uint16_t>(contents.owner.kind)) +
										  " generation=" + std::to_string(contents.owner.generation.value()) + "/" +
										  std::to_string(contents.location.generation.value()) +
										  " lsn=" + std::to_string(contents.owner.committed_lsn.value()) + "/" +
										  std::to_string(snapshot.superblock().checkpoint_lsn.value()));
		}
		EntityPage page = EntityPage::open(contents.owner, contents.payload);
		if (page.header().record_kind != expectedKind) {
			fail(FormatError::FERR_INVALID_STATE,
				 "relocatable Entity Map resolves to a page with another fixed-record kind");
		}
		return {std::move(contents), std::move(page)};
	}

	EntityPageStore::ReadPage EntityPageStore::loadReadPage(const MetadataSnapshotPin &snapshot, const PageId pageId,
																											const EntityPageRecordKind expectedKind,
																											const BufferPoolReadHint hint) const {
		if (!pageId.isValid() || !isEntityPageKind(expectedKind)) {
			fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable Entity page read view has an invalid Page ID or record kind");
		}
		if (std::optional<SharedPagePoolPinnedView> contents = allocator_.readPinned(snapshot, pageId, hint)) {
			validatePinnedEntityContents(snapshot, pageId, *contents);
			ReadPage result(std::move(*contents));
			if (result.page.header().record_kind != expectedKind) {
				fail(FormatError::FERR_INVALID_STATE,
						 "relocatable Entity Map resolves to a pinned page with another fixed-record kind");
			}
			return result;
		}
		return ReadPage(loadPage(snapshot, pageId, expectedKind, hint));
	}

	EntityPageStore::LoadedPage &EntityPageStore::loadScanPage(const MetadataSnapshotPin &snapshot, PageId pageId,
															   EntityPageRecordKind expectedKind,
															   std::optional<LoadedPage> &currentPage,
															   PageId &currentPageId) const {
		if (!currentPage.has_value() || currentPageId != pageId) {
			currentPage.emplace(loadPage(snapshot, pageId, expectedKind, BufferPoolReadHint::BPRH_SEQUENTIAL));
			currentPageId = pageId;
		}
		return *currentPage;
	}

	EntityPageStore::ReadPage &EntityPageStore::loadScanReadPage(const MetadataSnapshotPin &snapshot,
																											const PageId pageId,
																											const EntityPageRecordKind expectedKind,
																											std::optional<ReadPage> &currentPage,
																											PageId &currentPageId) const {
		if (!currentPage.has_value() || currentPageId != pageId) {
			// The previous pinned page authenticates this Catalog entry under the
			// same scan snapshot. Retain no additional cache; a pool change falls
			// back to normal resolution. Every page keeps its own fresh location,
			// allocated-slot, envelope, owner, checksum and typed-slot validation.
			currentPage.emplace([&]() -> ReadPage {
				if (currentPage.has_value() && currentPage->pinned_contents.has_value()) {
					const CatalogEntry &pool = currentPage->pinned_contents->pool;
					const PageLocation location = metadata_.findPageLocation(snapshot, pageId);
					if (location.segment_id == pool.segment_id && location.generation == pool.generation) {
						if (auto contents = allocator_.readPinnedKnownLocation(
								snapshot, pool, location, BufferPoolReadHint::BPRH_SEQUENTIAL)) {
							validatePinnedEntityContents(snapshot, pageId, *contents);
							ReadPage result(std::move(*contents));
							if (result.page.header().record_kind != expectedKind) {
								fail(FormatError::FERR_INVALID_STATE,
										"relocatable Entity Map resolves to a pinned page with another fixed-record kind");
							}
							debug::PerfTrace::addValue("native_graph.read.entity_scan_catalog_cache_hits", 1);
							return result;
						}
					}
				}
				return loadReadPage(snapshot, pageId, expectedKind, BufferPoolReadHint::BPRH_SEQUENTIAL);
			}());
			currentPageId = pageId;
		}
		return *currentPage;
	}

	std::optional<EntityPageInsertResult>
	EntityPageStore::allocate(const EntityPageNewPoolRequest &request, uint64_t entityId, EntityPageRecordKind kind,
							  const NodeEntityRecord *node, const EdgeEntityRecord *edge,
							  LogSequenceNumber checkpointLsn, const MetadataPublishOptions &options) {
		validateNewPoolRequest(request, entityId, kind, checkpointLsn);
		const PageOwnerHeader owner =
				ownerFor(request.page_id, request.pool_owner_id, request.generation, checkpointLsn);
		EntityPage page = EntityPage::create(owner, kind);
		const uint16_t slot = isNodePage(kind) ? page.insert(entityId, *node) : page.insert(entityId, *edge);
		const EntityPageLocation mapping = mappingFor(entityId, request.page_id, page, slot);
		PageLocation location = unpublishedLocation(request.page_id, request.segment_id, request.generation);
		CatalogSlotReservationRequest catalogRequest;
		catalogRequest.segment_id = request.segment_id;
		catalogRequest.kind = SegmentKind::SGK_ENTITY_PAGE_POOL;
		catalogRequest.owner_id = request.pool_owner_id;
		catalogRequest.segment_generation = request.generation;
		catalogRequest.state = SegmentState::SGS_SEALED;
		catalogRequest.require_absent = true;
		const EntityPagePoolReservationPreparer preparer =
				[this, &page, checkpointLsn](std::span<const CatalogEntry> entries, std::span<PageLocation> locations) {
					// This typed metadata call owns one reservation and page location.
					prepareNewPoolPage(entries.front(), page, checkpointLsn, locations.front());
				};
		const std::optional<CatalogSlotReservations> reservation =
				isNodePage(kind)
						? metadata_.reserveFirstFreeAndUpsertCatalogAndEntityPageLocations(
								  std::span(&catalogRequest, 1), std::span(&location, 1), std::span(&mapping, 1), {},
								  request.first_slot, request.last_slot, checkpointLsn, preparer, options)
						: metadata_.reserveFirstFreeAndUpsertCatalogAndEntityPageLocations(
								  std::span(&catalogRequest, 1), std::span(&location, 1), {}, std::span(&mapping, 1),
								  request.first_slot, request.last_slot, checkpointLsn, preparer, options);
		if (!reservation.has_value()) {
			return std::nullopt;
		}
		return EntityPageInsertResult{reservation->superblock, reservation->entries.front(), location, mapping};
	}

	EntityPageInsertResult EntityPageStore::append(const EntityPageAppendRequest &request, uint64_t entityId,
												   EntityPageRecordKind kind, const NodeEntityRecord *node,
												   const EdgeEntityRecord *edge, LogSequenceNumber checkpointLsn,
												   const MetadataPublishOptions &options) {
		validateAppendRequest(request, entityId, kind, checkpointLsn);
		const PageOwnerHeader owner = ownerFor(request.page_id, request.expected_pool.owner_id,
											   request.expected_pool.generation, checkpointLsn);
		EntityPage page = EntityPage::create(owner, kind);
		const uint16_t slot = isNodePage(kind) ? page.insert(entityId, *node) : page.insert(entityId, *edge);
		const EntityPageLocation mapping = mappingFor(entityId, request.page_id, page, slot);
		PageLocation published = unpublishedLocation(request.page_id, request.expected_pool.segment_id,
													 request.expected_pool.generation);
		const ExistingPoolPagePreparer preparer = [this, &page, checkpointLsn, &published](const CatalogEntry &pool,
																						   PageLocation &location) {
			prepareAppendPage(pool, page, checkpointLsn, location);
			published = location;
		};
		const Superblock superblock =
				isNodePage(kind) ? metadata_.appendEntityPageToExistingPool(request.expected_pool, published,
																			std::span(&mapping, 1), {}, checkpointLsn,
																			preparer, options)
								 : metadata_.appendEntityPageToExistingPool(request.expected_pool, published, {},
																			std::span(&mapping, 1), checkpointLsn,
																			preparer, options);
		return {superblock, request.expected_pool, published, mapping};
	}

	EntityPageInsertResult EntityPageStore::insert(const MetadataSnapshotPin &snapshot, PageId pageId,
												   uint64_t entityId, EntityPageRecordKind kind,
												   const NodeEntityRecord *node, const EdgeEntityRecord *edge,
												   LogSequenceNumber checkpointLsn,
												   const MetadataPublishOptions &options) {
		if (entityId == 0U || !checkpointLsn.isValid()) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "relocatable Entity page insertion requires a logical Entity ID and a checkpoint LSN");
		}
		LoadedPage loaded = loadPage(snapshot, pageId, kind);
		const uint16_t slot =
				isNodePage(kind) ? loaded.page.insert(entityId, *node) : loaded.page.insert(entityId, *edge);
		PageOwnerHeader owner = loaded.contents.owner;
		owner.committed_lsn = checkpointLsn;
		loaded.page = loaded.page.rebindOwner(owner);
		const EntityPageLocation mapping = mappingFor(entityId, pageId, loaded.page, slot);
		PageLocation published =
				unpublishedLocation(pageId, loaded.contents.pool.segment_id, loaded.contents.pool.generation);
		const ExistingPoolPageReplacementPreparer preparer =
				[this, &loaded, checkpointLsn, &published](const CatalogEntry &pool, const PageLocation &expected,
														   PageLocation &replacement) {
					prepareReplacementPage(pool, expected, loaded.contents.owner.committed_lsn, loaded.page,
										   checkpointLsn, replacement);
					published = replacement;
				};
		const Superblock superblock =
				isNodePage(kind)
						? metadata_.replaceEntityPageInExistingPool(loaded.contents.pool, loaded.contents.location,
																	published, std::span(&mapping, 1), {}, {}, {},
																	checkpointLsn, preparer, options)
						: metadata_.replaceEntityPageInExistingPool(loaded.contents.pool, loaded.contents.location,
																	published, {}, {}, std::span(&mapping, 1), {},
																	checkpointLsn, preparer, options);
		return {superblock, loaded.contents.pool, published, mapping};
	}

	EntityPageMutationResult EntityPageStore::update(const MetadataSnapshotPin &snapshot, uint64_t entityId,
													 EntityPageRecordKind kind, const NodeEntityRecord *node,
													 const EdgeEntityRecord *edge, LogSequenceNumber checkpointLsn,
													 const MetadataPublishOptions &options) {
		if (entityId == 0U || !checkpointLsn.isValid()) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "relocatable Entity page update requires a logical Entity ID and a checkpoint LSN");
		}
		const EntityPageLocation mapping = isNodePage(kind) ? metadata_.findNodePageLocation(snapshot, entityId)
															: metadata_.findEdgePageLocation(snapshot, entityId);
		LoadedPage loaded = loadPage(snapshot, mapping.page_id, kind);
		if (isNodePage(kind)) {
			loaded.page.replace(mapping.slot_id, mapping.slot_generation, entityId, *node);
		} else {
			loaded.page.replace(mapping.slot_id, mapping.slot_generation, entityId, *edge);
		}
		PageOwnerHeader owner = loaded.contents.owner;
		owner.committed_lsn = checkpointLsn;
		loaded.page = loaded.page.rebindOwner(owner);
		PageLocation published =
				unpublishedLocation(mapping.page_id, loaded.contents.pool.segment_id, loaded.contents.pool.generation);
		const ExistingPoolPageReplacementPreparer preparer =
				[this, &loaded, checkpointLsn, &published](const CatalogEntry &pool, const PageLocation &expected,
														   PageLocation &replacement) {
					prepareReplacementPage(pool, expected, loaded.contents.owner.committed_lsn, loaded.page,
										   checkpointLsn, replacement);
					published = replacement;
				};
		const Superblock superblock =
				metadata_.replaceEntityPageInExistingPool(loaded.contents.pool, loaded.contents.location, published, {},
														  {}, {}, {}, checkpointLsn, preparer, options);
		return {superblock, loaded.contents.pool, published};
	}

	EntityPageMutationResult EntityPageStore::erase(const MetadataSnapshotPin &snapshot, uint64_t entityId,
													EntityPageRecordKind kind, LogSequenceNumber checkpointLsn,
													const MetadataPublishOptions &options) {
		if (entityId == 0U || !checkpointLsn.isValid()) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "relocatable Entity page deletion requires a logical Entity ID and a checkpoint LSN");
		}
		const EntityPageLocation mapping = isNodePage(kind) ? metadata_.findNodePageLocation(snapshot, entityId)
															: metadata_.findEdgePageLocation(snapshot, entityId);
		// Use the complete Entity-page batch primitive even for one deletion. It
		// removes the matching owner-keyed Property directory entry in the same
		// publication and keeps direct deletion aligned with graph transactions.
		EntityPageBatchMutation mutation;
		mutation.page_id = mapping.page_id;
		mutation.record_kind = kind;
		if (isNodePage(kind)) {
			mutation.erased_node_ids.push_back(entityId);
		} else {
			mutation.erased_edge_ids.push_back(entityId);
		}
		const EntityPageBatchResult result = applyBatch(snapshot, std::span(&mutation, 1), checkpointLsn, options);
		if (result.pages.size() != 1U) {
			fail(FormatError::FERR_INVALID_STATE,
				 "relocatable Entity page deletion batch did not return one page mutation result");
		}
		return result.pages.front();
	}

	EntityPageMutationResult EntityPageStore::evacuate(const MetadataSnapshotPin &snapshot, PageId pageId,
													   SegmentId destinationSegmentId, EntityPageRecordKind kind,
													   LogSequenceNumber checkpointLsn,
													   const MetadataPublishOptions &options) {
		if (!pageId.isValid() || !destinationSegmentId.isValid() || !isEntityPageKind(kind) ||
			!checkpointLsn.isValid()) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "relocatable Entity page evacuation requires page, destination, record-kind, and LSN identities");
		}
		LoadedPage loaded = loadPage(snapshot, pageId, kind);
		const CatalogEntry destinationPool = metadata_.findCatalog(snapshot, destinationSegmentId);
		const uint8_t invalidDestination =
				static_cast<uint8_t>(destinationPool.segment_id == loaded.contents.pool.segment_id) |
				static_cast<uint8_t>(destinationPool.kind != SegmentKind::SGK_ENTITY_PAGE_POOL) |
				static_cast<uint8_t>(destinationPool.owner_id != loaded.contents.pool.owner_id) |
				static_cast<uint8_t>(destinationPool.state != SegmentState::SGS_SEALED) |
				static_cast<uint8_t>(!destinationPool.generation.isValid());
		if (invalidDestination != 0U) {
			fail(FormatError::FERR_INVALID_STATE,
				 "relocatable Entity page evacuation requires another sealed Entity pool in the same ownership domain");
		}
		const PageOwnerHeader destinationOwner =
				ownerFor(pageId, destinationPool.owner_id, destinationPool.generation, checkpointLsn);
		const EntityPage relocated = loaded.page.rebindOwner(destinationOwner);
		const std::vector<std::byte> payload = relocated.serialize();
		SharedPagePoolEvacuationRequest request;
		request.source_segment_id = loaded.contents.pool.segment_id;
		request.source_generation = loaded.contents.pool.generation;
		request.destination_segment_id = destinationPool.segment_id;
		request.destination_generation = destinationPool.generation;
		request.page_id = pageId;
		request.expected_owner_lsn = loaded.contents.owner.committed_lsn;
		request.page_owner_id = destinationPool.owner_id;
		request.page_owner_kind = PageOwnerKind::POK_ENTITY;
		request.payload = payload;
		const SharedPagePoolAllocation relocatedPage = allocator_.evacuatePage(request, checkpointLsn, options);
		return {relocatedPage.superblock, relocatedPage.pool, relocatedPage.location};
	}

	EntityPageMutationResult EntityPageStore::evacuateAuxiliaryPage(const MetadataSnapshotPin &snapshot, PageId pageId,
																	SegmentId destinationSegmentId,
																	PageOwnerKind ownerKind,
																	LogSequenceNumber checkpointLsn,
																	const MetadataPublishOptions &options) {
		if (!isAuxiliaryEntityPoolPageOwnerKind(ownerKind)) { // this private helper is dispatched only by the POK_ENTITY_LABEL switch case in evacuatePoolPage.
			fail(FormatError::FERR_INVALID_HEADER, // the public dispatcher reaches this private helper only through its POK_ENTITY_LABEL case.
					 "relocatable Entity-pool auxiliary evacuation requires a supported owner kind");
		}
		const SharedPagePoolContents contents = allocator_.read(snapshot, pageId);
		const CatalogEntry destination = metadata_.findCatalog(snapshot, destinationSegmentId);
		const uint8_t invalid =
				static_cast<uint8_t>(contents.pool.kind != SegmentKind::SGK_ENTITY_PAGE_POOL) |
				static_cast<uint8_t>(contents.pool.state != SegmentState::SGS_SEALED) |
				static_cast<uint8_t>(contents.location.page_id != pageId) |
				static_cast<uint8_t>(contents.location.segment_id != contents.pool.segment_id) |
				static_cast<uint8_t>(contents.owner.page_id != pageId) |
				static_cast<uint8_t>(contents.owner.owner_id != contents.pool.owner_id) |
				static_cast<uint8_t>(contents.owner.kind != ownerKind) |
				static_cast<uint8_t>((contents.owner.flags & ~RSS_PAGE_OWNER_KNOWN_FLAGS) != 0U) |
				static_cast<uint8_t>(contents.owner.payload_bytes != RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES) |
				static_cast<uint8_t>(contents.owner.generation != contents.pool.generation) |
				static_cast<uint8_t>(contents.owner.committed_lsn > snapshot.superblock().checkpoint_lsn) |
				static_cast<uint8_t>(destination.segment_id == contents.pool.segment_id) |
				static_cast<uint8_t>(destination.kind != SegmentKind::SGK_ENTITY_PAGE_POOL) |
				static_cast<uint8_t>(destination.owner_id != contents.pool.owner_id) |
				static_cast<uint8_t>(destination.state != SegmentState::SGS_SEALED) |
				static_cast<uint8_t>(!destination.generation.isValid());
		if (invalid != 0U) {
			fail(FormatError::FERR_INVALID_STATE,
				 "relocatable Entity-pool auxiliary evacuation requires compatible source and destination pools");
		}
		std::vector<std::byte> payload;
		if (ownerKind == PageOwnerKind::POK_ENTITY_LABEL) { // the private-entry invariant above fixes the sole supported auxiliary owner kind.
			SlottedPage page = SlottedPage::open(contents.owner, contents.payload);
			for (const SlottedSlotReference &slot: page.liveSlots()) {
				const std::vector<std::byte> record = page.read(slot);
				static_cast<void>(EntityLabelSetCodec::decode(record));
			}
			page.rebindOwner(auxiliaryOwnerFor(pageId, destination, ownerKind, checkpointLsn));
			payload = page.serialize();
		} else {
			fail(FormatError::FERR_INVALID_ENUM, // the preceding private-entry invariant admits only POK_ENTITY_LABEL.
				 "relocatable Entity-pool evacuation encountered an unsupported auxiliary page owner kind");
		}
		SharedPagePoolEvacuationRequest request;
		request.source_segment_id = contents.pool.segment_id;
		request.source_generation = contents.pool.generation;
		request.destination_segment_id = destination.segment_id;
		request.destination_generation = destination.generation;
		request.page_id = pageId;
		request.expected_owner_lsn = contents.owner.committed_lsn;
		request.page_owner_id = destination.owner_id;
		request.page_owner_kind = ownerKind;
		request.payload = std::move(payload);
		const SharedPagePoolAllocation relocated = allocator_.evacuatePage(request, checkpointLsn, options);
		return {relocated.superblock, relocated.pool, relocated.location};
	}

	void EntityPageStore::validateNewPoolRequest(const EntityPageNewPoolRequest &request, uint64_t entityId,
												 EntityPageRecordKind kind, LogSequenceNumber checkpointLsn) {
		const uint8_t invalid =
				static_cast<uint8_t>(!request.segment_id.isValid()) |
				static_cast<uint8_t>(request.pool_owner_id == 0U) |
				static_cast<uint8_t>(!request.generation.isValid()) | static_cast<uint8_t>(!request.page_id.isValid()) |
				static_cast<uint8_t>(!request.first_slot.isValid()) |
				static_cast<uint8_t>(!request.last_slot.isValid()) |
				static_cast<uint8_t>(request.first_slot > request.last_slot) | static_cast<uint8_t>(entityId == 0U) |
				static_cast<uint8_t>(!isEntityPageKind(kind)) | static_cast<uint8_t>(!checkpointLsn.isValid());
		if (invalid != 0U) {
			fail(FormatError::FERR_INVALID_HEADER, "relocatable Entity page new-pool allocation requires complete "
												   "pool, page, Entity, and LSN identities");
		}
	}

	void EntityPageStore::validateAppendRequest(const EntityPageAppendRequest &request, uint64_t entityId,
												EntityPageRecordKind kind, LogSequenceNumber checkpointLsn) {
		const CatalogEntry &pool = request.expected_pool;
		const uint8_t invalid =
				static_cast<uint8_t>(!pool.segment_id.isValid()) | static_cast<uint8_t>(!pool.physical_slot.isValid()) |
				static_cast<uint8_t>(pool.owner_id == 0U) | static_cast<uint8_t>(!pool.generation.isValid()) |
				static_cast<uint8_t>(pool.kind != SegmentKind::SGK_ENTITY_PAGE_POOL) |
				static_cast<uint8_t>(pool.state != SegmentState::SGS_SEALED) |
				static_cast<uint8_t>(!request.page_id.isValid()) | static_cast<uint8_t>(entityId == 0U) |
				static_cast<uint8_t>(!isEntityPageKind(kind)) | static_cast<uint8_t>(!checkpointLsn.isValid());
		if (invalid != 0U) {
			fail(FormatError::FERR_INVALID_HEADER, "relocatable Entity page append requires a sealed Entity pool and "
												   "complete page, Entity, and LSN identities");
		}
	}

	void EntityPageStore::validateRecord(const NodeEntityRecord &node) {
		static_cast<void>(NodeEntityRecordCodec::encode(node));
	}

	void EntityPageStore::validateRecord(const EdgeEntityRecord &edge) {
		static_cast<void>(EdgeEntityRecordCodec::encode(edge));
	}

	PageOwnerHeader EntityPageStore::ownerFor(PageId pageId, uint64_t ownerId, Generation generation,
											  LogSequenceNumber checkpointLsn) {
		return {pageId,		ownerId,	  PageOwnerKind::POK_ENTITY, 0U, RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
				generation, checkpointLsn};
	}

	EntityPageLocation EntityPageStore::mappingFor(uint64_t entityId, PageId pageId, const EntityPage &page,
												   uint16_t slot) {
		if (entityId == 0U || page.entityId(slot) != entityId) {
			fail(FormatError::FERR_INVALID_STATE,
				 "relocatable Entity page insertion did not preserve its logical Entity ID");
		}
		return {entityId, pageId, slot, page.slotGeneration(slot)};
	}

	void EntityPageStore::prepareNewPoolPage(const CatalogEntry &pool, const EntityPage &page,
											 LogSequenceNumber checkpointLsn, PageLocation &location) const {
		const EntityPageHeader header = page.header();
		const std::vector<std::byte> payload = page.serialize();
		const PageOwnerHeader owner = ownerFor(header.page_id, pool.owner_id, pool.generation, checkpointLsn);
		const SharedPagePoolPreparedAppend prepared{owner, payload, 0U};
		allocator_.prepareNewPoolPages(pool, std::span(&prepared, 1), std::span(&location, 1), checkpointLsn);
	}

	void EntityPageStore::prepareAppendPage(const CatalogEntry &pool, const EntityPage &page,
											LogSequenceNumber checkpointLsn, PageLocation &location) const {
		const EntityPageHeader header = page.header();
		const std::vector<std::byte> payload = page.serialize();
		const PageOwnerHeader owner = ownerFor(header.page_id, pool.owner_id, pool.generation, checkpointLsn);
		const SharedPagePoolPreparedAppend prepared{owner, payload, 0U};
		allocator_.prepareAppendPages(pool, std::span(&prepared, 1), std::span(&location, 1), checkpointLsn);
	}

	void EntityPageStore::prepareReplacementPage(const CatalogEntry &pool, const PageLocation &expected,
												 LogSequenceNumber expectedOwnerLsn, const EntityPage &page,
												 LogSequenceNumber checkpointLsn, PageLocation &replacement) const {
		const EntityPageHeader header = page.header();
		const std::vector<std::byte> payload = page.serialize();
		const PageOwnerHeader owner = ownerFor(header.page_id, pool.owner_id, pool.generation, checkpointLsn);
		const SharedPagePoolPreparedReplacement prepared{owner, expectedOwnerLsn, payload, 0U};
		allocator_.prepareMutationPages(pool, std::span(&expected, 1), std::span(&prepared, 1), {},
										std::span(&replacement, 1), {}, checkpointLsn);
	}

} // namespace graph::storage
