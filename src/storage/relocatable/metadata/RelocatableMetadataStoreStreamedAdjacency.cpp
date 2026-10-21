#include "graph/storage/relocatable/metadata/NativeGraphDeferredIndexImages.hpp"
#include "graph/storage/relocatable/metadata/HybridPropertyPoolPlacement.hpp"
#include "graph/storage/relocatable/adjacency/AdjacencyKeyCodec.hpp"
#include "graph/debug/PerfTrace.hpp"
#include <algorithm>

namespace graph::storage {
void RelocatableMetadataStore::prepareStreamedAdjacencyImages(MetadataWriteBatch &batch,
    const MetadataWriteGuard &writer, const MetadataSnapshotPin &source, NativeGraphDeferredIndexImages &proof,
    std::unique_ptr<HybridPropertyPoolPlacement> &placement) {
    const auto require = [](bool valid, const char *message) {
        if (!valid) throw FormatException(FormatError::FERR_INVALID_STATE, message);
    };
    require(proof.incremental_ && !proof.materialized_ && proof.metadata_ == this && proof.memory_ &&
        ownsSnapshot(source) && source.superblock() == batch.current() && proof.source_generation_ == batch.current().generation &&
        proof.source_root_ && locations_.findIndexRootOptional(batch.current(), proof.index_id_) == proof.source_root_ &&
        proof.plan_.expected_root == *proof.source_root_ && proof.plan_.preparer && !proof.plan_.final_tree_empty,
        "streamed adjacency images lost their owned source or bounded plan");
    proof.materialized_ = true;
    utils::OwnedAllocationScope scope(proof.memory_);
    SharedPagePoolAllocator sourcePools(io_, *this, source);
    std::vector<IndexTreeMutationPoolBatch> assignments;
    assignments.reserve(proof.plan_.pool_requests.size() + proof.plan_.additional_new_pool_mutations.size() +
        size_t(bool(proof.plan_.new_pool_mutation)));
    utils::OwnedVector<uint64_t> erased{utils::OwnedPmrAllocator<uint64_t>(proof.memory_)};
    for (const auto &request : proof.plan_.pool_requests) {
        const auto pool = catalog_.find(batch.current(), request.segment_id);
        require(pool.kind == SegmentKind::SGK_INDEX_PAGE_POOL &&
            pool.state == SegmentState::SGS_SEALED && allocator_.isAllocated(batch.current(), pool.physical_slot) &&
            !request.appended_page_count && !request.replacement_segment_id.isValid(),
            "streamed adjacency source pool has an invalid owner or physical placement");
        const auto authenticate = [&](const PageLocation &page) {
            require(locations_.findPage(batch.current(), page.page_id) == page && page.segment_id == pool.segment_id &&
                page.generation == pool.generation, "index image replacement source changed");
            const auto owner = sourcePools.readPageOwner(pool, page.page_index);
            require(owner.page_id == page.page_id && owner.owner_id == proof.index_id_ &&
                owner.kind == PageOwnerKind::POK_INDEX && owner.generation == pool.generation && owner.flags == 0,
                "index image source page lost its authenticated owner");
        };
        for (const auto &page : request.expected_replacements) authenticate(page);
        for (const auto &page : request.expected_retirements) {
            authenticate(page.location);
            require(locations_.findPage(batch.current(), page.location.page_id) == page.location &&
                page.location.segment_id == pool.segment_id && page.location.generation == pool.generation,
                "streamed adjacency retirement source changed");
            erased.push_back(page.location.page_id.value());
        }
        IndexTreeMutationPoolBatch assignment; assignment.pool = pool; assignment.replacement_pool = pool;
        assignment.expected_replacements = request.expected_replacements; assignment.expected_retirements = request.expected_retirements;
        assignments.push_back(std::move(assignment));
    }
    const auto fresh = [&](const BTreeNewIndexTreePoolMutation &request) {
        require(request.owner_id == proof.index_id_ && request.appended_page_count &&
            request.appended_page_count <= RSS_PAGE_POOL_DATA_FRAME_COUNT, "streamed adjacency appended shape is invalid");
        IndexTreeMutationPoolBatch assignment; assignment.newly_reserved = true;
        assignment.pool.kind = SegmentKind::SGK_INDEX_PAGE_POOL; assignment.pool.owner_id = request.owner_id;
        assignment.pool.generation = Generation(1); assignment.pool.state = SegmentState::SGS_SEALED;
        assignment.pool.committed_lsn = batch.checkpointLsn(); assignment.replacement_pool = assignment.pool;
        for (size_t i = 0; i < request.appended_page_count; ++i) {
            const auto id = batch.allocateLogicalPageId(); assignment.appended_page_ids.push_back(id);
            assignment.appended_locations.push_back({id, {}, 0, Generation(1), 0});
        }
        assignments.push_back(std::move(assignment));
    };
    if (proof.plan_.new_pool_mutation) fresh(*proof.plan_.new_pool_mutation);
    for (const auto &request : proof.plan_.additional_new_pool_mutations) fresh(request);
    proof.plan_.preparer(assignments, batch.checkpointLsn());
    auto root = proof.plan_.stable_replacement_root.value_or(proof.plan_.expected_root.root_page_id);
    if (proof.plan_.replacement_root) {
        const auto &ref = *proof.plan_.replacement_root;
        root = assignments.at(ref.pool_ordinal).appended_page_ids.at(ref.appended_page_ordinal);
    }
    auto rootGeneration = proof.plan_.expected_root.root_generation;
    if (!placement) placement.reset(new HybridPropertyPoolPlacement(*this, batch, writer, source,
        proof.first_slot_, proof.last_slot_, proof.memory_, [] {}, true));
    utils::OwnedVector<PageLocation> published{utils::OwnedPmrAllocator<PageLocation>(proof.memory_)};
    published.reserve(proof.images_.size());
    utils::OwnedHeapReservation frameCredit(proof.memory_, 2U * RSS_PAGE_SIZE + 1024U);
    for (const auto &page : proof.images_) {
        require(page.header().index_id == proof.index_id_, "streamed adjacency image has a foreign namespace");
        const auto pool = placement->allocate(SegmentKind::SGK_INDEX_PAGE_POOL, proof.index_id_, 1);
        const PageOwnerHeader owner{page.header().page_id, pool.owner_id, PageOwnerKind::POK_INDEX, 0,
            RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, pool.generation, batch.checkpointLsn()};
        auto rebound = page.rebindOwner(owner);
        std::array<SharedPagePoolPreparedAppend, 1> appends;
        appends[0].owner = owner; appends[0].payload = rebound.serialize();
        appends[0].free_bytes = rebound.slottedPage().stats().contiguous_free_bytes;
        std::array<PageLocation, 1> locations{{{owner.page_id, pool.segment_id, 0, pool.generation, appends[0].free_bytes}}};
        placement->write(pool, appends, locations, false); published.push_back(locations[0]);
        if (owner.page_id == root) rootGeneration = pool.generation;
    }
    for (auto &reservation : placement->reservations_) if (reservation.fresh && !reservation.registered) {
        allocator_.prepareSetAllocated(batch, reservation.pool.physical_slot, true);
        catalog_.prepareUpsert(batch, std::span(&reservation.pool, 1));
        reservation.registered = true;
        debug::PerfTrace::addValue("native_graph.write.index_images.new_pools_registered", 1);
    }
    std::sort(published.begin(), published.end(), [](const auto &a, const auto &b) { return a.page_id < b.page_id; });
    std::sort(erased.begin(), erased.end());
    require(std::adjacent_find(published.begin(), published.end(), [](const auto &a, const auto &b) {
        return a.page_id == b.page_id;
    }) == published.end(), "streamed adjacency images repeat a logical page");
    if (!published.empty() || !erased.empty()) locations_.prepareReplacePages(batch, published, erased);
    const IndexRootLocation installed{proof.index_id_, root, rootGeneration};
    locations_.prepareReplaceIndexRoots(batch, std::span(&installed, 1), {});
    proof.installed_root_ = installed;
    if (proof.private_segments_) debug::PerfTrace::addValue(proof.index_id_ == RSS_GLOBAL_ADJACENCY_INDEX_ID ?
        "native_graph.write.streamed_adjacency.incremental_frames" : "native_graph.write.streamed_membership.incremental_frames", int64_t(published.size()));
    proof.installed_pages_ = std::move(published);
}
} // namespace graph::storage
