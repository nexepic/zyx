#include "NativeGraphWritePlannerContext.hpp"

namespace graph::storage {

	NativeGraphWriteCommitResult NativeGraphWriteTransaction::commit(const MetadataPublishOptions &publishOptions) {
        debug::PerfTrace::addValue("native_graph.write.graph_commit_calls", 1);
		debug::ScopedPerfTimer timer("native_graph.commit");
		requireActive("commit", true);
        if (covered_adjacency_pending_)
            fail(FormatError::FERR_INVALID_STATE, "covered candidate finalization must succeed before graph commit");
        if (hybrid_vector_pending_)
            fail(FormatError::FERR_INVALID_STATE, "failed native Vector coordination requires rollback before commit");
        std::optional<utils::OwnedAllocationScope> hybridAllocation;
        if (hybrid_vectors_) hybridAllocation.emplace(hybrid_columns_->memory);
        const auto finishHybridVectors = [this](void *) noexcept {
            if (!hybrid_vectors_ || state_ == NativeGraphWriteTransactionState::NGWTS_ACTIVE) return;
            std::vector<StagedVectorBootstrap>().swap(vector_bootstraps_);
            std::vector<StagedVectorDelta>().swap(vector_deltas_);
            std::vector<StagedScalarIndexMutation>().swap(scalar_index_mutations_);
            std::vector<StagedInitialScalarIndexMutation>().swap(initial_scalar_index_mutations_);
            hybrid_vectors_.reset(); hybrid_columns_.reset(); hybrid_catalog_tokens_.reset(); hybrid_catalog_credit_.reset(); hybrid_vector_pending_ = false;
        };
        // Declared before all publication closures: their retained buffers die
        // before the coordinator refunds its temporary/retained heap credit.
        std::unique_ptr<void, decltype(finishHybridVectors)> hybridCleanup(this, finishHybridVectors);
        if (hybrid_vectors_ && (!hybrid_vectors_->semantics.empty() || hybrid_catalog_credit_)) {
            // Known pre-publication cancellation leaves the complete staged plan
            // available for retry; later preparation failures require rollback.
            if (hybrid_columns_->cancellation) hybrid_columns_->cancellation->check();
            hybrid_vector_pending_ = true;
        }
        if (rebuild_entity_free_summaries_ && (!nodes_.empty() || !edges_.empty() || !properties_.empty() ||
            !scalar_index_mutations_.empty() || !initial_scalar_index_mutations_.empty() || !vector_bootstraps_.empty() || !vector_deltas_.empty()))
            fail(FormatError::FERR_INVALID_STATE, "free-space initialization cannot mix ordinary graph mutations");
        if (free_summary_checkpoint_) free_summary_checkpoint_();
		const bool hasGraphWork = rebuild_entity_free_summaries_ || !nodes_.empty() || !edges_.empty() || !properties_.empty() ||
								  !scalar_index_mutations_.empty() || !initial_scalar_index_mutations_.empty() ||
								  !vector_bootstraps_.empty() || !vector_deltas_.empty();
		if (!hasGraphWork && node_id_ranges_.empty() && edge_id_ranges_.empty() && !hybrid_conversion_ && !hybrid_columns_) {
			fail(FormatError::FERR_INVALID_STATE, "native graph write transaction cannot commit without staged work");
		}
        if (hybrid_conversion_ || (hybrid_columns_ && !hybrid_catalog_credit_ && (!hybrid_vectors_ || hybrid_vectors_->semantics.empty()))) {
            // This checkpoint is provably before publication. Cancellation
            // retains an active source/request for an independent retry and
            // avoids ordinary lease-vector scratch and the publication lock.
            const auto *cancellation = hybrid_columns_ ? hybrid_columns_->cancellation : hybrid_conversion_->conversion->cancellation;
            if (cancellation) cancellation->check();
            std::lock_guard publicationLock(context_->publication_mutex);
            auto current = context_->metadata->openSnapshot();
            if (current.superblock() != snapshot_.superblock()) {
                current.release(); snapshot_.release(); hybrid_conversion_.reset(); hybrid_columns_.reset();
                state_ = NativeGraphWriteTransactionState::NGWTS_CONFLICTED;
                return {NativeGraphWriteCommitOutcome::NGWCO_CONFLICTED, {}, {}};
            }
            current.release();
            try {
                const auto &base = snapshot_.superblock();
                const auto &placement = context_->options.property_roots;
                const bool mapNoop = hybrid_columns_ && hybrid_columns_->cells.empty() && hybrid_columns_->deletions.empty();
                const auto published = mapNoop ? base : hybrid_columns_ ? context_->metadata->mutateHybridPropertyColumns(
                    {base.database_id, base.generation, base.hybrid_property_descriptor_root, hybrid_columns_->cells,
                        hybrid_columns_->memory, placement.first_slot, placement.last_slot, placement.page_owner_id, cancellation,
                        hybrid_columns_->field_lifecycle, hybrid_columns_->deletions},
                    nextCheckpointLsn(base), publishOptions) : context_->metadata->mutateNativeGraphPagesWithLargeValues(
                    {}, {}, {}, {}, {}, {}, {}, nextCheckpointLsn(snapshot_.superblock()),
                    [](auto, auto, auto, auto, auto, auto) {}, [](auto &) {}, {}, {}, {}, {}, {},
                    publishOptions, {}, snapshot_.superblock().generation, {}, {}, {}, {}, {}, hybrid_conversion_);
                snapshot_.release(); hybrid_conversion_.reset(); hybrid_columns_.reset();
                state_ = NativeGraphWriteTransactionState::NGWTS_COMMITTED;
                return {NativeGraphWriteCommitOutcome::NGWCO_COMMITTED, published, {}};
            } catch (...) {
                snapshot_.release(); hybrid_conversion_.reset(); hybrid_columns_.reset();
                state_ = NativeGraphWriteTransactionState::NGWTS_INDETERMINATE;
                throw;
            }
        }
		NativeGraphLogicalIdReservation logicalIds;
		logicalIds.node_ranges = node_id_ranges_;
		logicalIds.edge_ranges = edge_id_ranges_;
		std::lock_guard publicationLock(context_->publication_mutex);
		if (!hasGraphWork) {
			// Logical-ID leasing is already durable. A reservation-only transaction
			// must not publish a second, empty metadata generation just to acknowledge
			// IDs that are intentionally allowed to remain unused.
			if (!context_->metadata->claimNativeGraphLogicalIdReservation(logicalIds)) {
				snapshot_.release();
				state_ = NativeGraphWriteTransactionState::NGWTS_CONFLICTED;
				return {NativeGraphWriteCommitOutcome::NGWCO_CONFLICTED, {}, {}};
			}
			MetadataSnapshotPin current = context_->metadata->openSnapshot();
			const Superblock committed = current.superblock();
			current.release();
			snapshot_.release();
			state_ = NativeGraphWriteTransactionState::NGWTS_COMMITTED;
			return {NativeGraphWriteCommitOutcome::NGWCO_COMMITTED, committed, {}};
		}
		MetadataSnapshotPin current = context_->metadata->openSnapshot();
		if (current.superblock() != snapshot_.superblock()) {
			if (isLogicalIdLeaseOnlyAdvance(*context_->metadata, snapshot_.superblock(), current.superblock())) {
				snapshot_.release();
				snapshot_ = std::move(current);
			} else if (!rebaseAdjacencyOnly(current)) {
				current.release();
				context_->metadata->abandonNativeGraphLogicalIdReservation(logicalIds);
				snapshot_.release();
				state_ = NativeGraphWriteTransactionState::NGWTS_CONFLICTED;
				return {NativeGraphWriteCommitOutcome::NGWCO_CONFLICTED, {}, {}};
			}
		} else {
			current.release();
		}
		{
			debug::ScopedPerfTimer adjacencyPlanTimer("native_graph.commit.adjacency_plan");
			prepareGlobalAdjacencyMutationPlan();
		}

		const LogSequenceNumber checkpointLsn = nextCheckpointLsn(snapshot_.superblock());
		uint64_t nextSegmentId = snapshot_.superblock().next_logical_segment_id;
		uint64_t nextPageId = snapshot_.superblock().next_logical_page_id;
		if (nextSegmentId == 0U || nextPageId == 0U) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native graph write received invalid logical Segment or Page high-water marks");
		}
		const auto nextSegment = [&nextSegmentId] {
			if (nextSegmentId == std::numeric_limits<uint64_t>::max()) {
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW, "native graph write exhausted logical Segment IDs");
			}
			return SegmentId(nextSegmentId);
		};
		const auto reserveSegment = [&nextSegmentId, &nextSegment] {
			const SegmentId segmentId = nextSegment();
			++nextSegmentId;
			return segmentId;
		};
		const auto nextPage = [&nextPageId] {
			if (nextPageId == std::numeric_limits<uint64_t>::max()) {
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW, "native graph write exhausted logical Page IDs");
			}
			return PageId(nextPageId);
		};
		const auto reservePage = [&nextPageId, &nextPage] {
			const PageId pageId = nextPage();
			++nextPageId;
			return pageId;
		};

		std::optional<std::vector<CatalogEntry>> currentPools;
		const auto getCurrentPools = [&]() -> const std::vector<CatalogEntry> & {
			if (!currentPools) {
				currentPools = context_->entities->currentPools(snapshot_);
				debug::PerfTrace::addValue("native_graph.commit.entity_pool_discoveries", 1);
			}
			return *currentPools;
		};
		// Fresh pools can receive either initial logical pages or COW images of
		// existing PageIds. Both uses consume the same bounded frame budget and
		// become visible through this transaction's single metadata publication.
		std::vector<FreshEntityPool> labelPools;
		std::map<uint64_t, uint8_t> freeFrames;
		const auto availableFrames = [&](const CatalogEntry &pool) -> uint8_t & {
			auto found = freeFrames.find(pool.segment_id.value());
			if (found == freeFrames.end()) {
				const auto count = context_->entities->availableFrameCount(snapshot_, pool);
				found = freeFrames.emplace(pool.segment_id.value(), count).first;
				debug::PerfTrace::addValue("native_graph.commit.entity_pool_capacity_queries", 1);
			}
			return found->second;
		};
		std::map<uint64_t, uint8_t> reservedFrames;
		const auto tryReserveCurrentFrame = [&](const CatalogEntry &pool) {
			auto &available = availableFrames(pool);
			if (reservedFrames[pool.segment_id.value()] >= available) {
				// Reclaim only frames absent from every active snapshot before declaring
				// the pool full. Consecutive short writes must not exhaust a finite
				// COW pool merely because their retired frames were never revisited.
				context_->entities->reclaimRetiredFrames(pool.segment_id);
				available = context_->entities->availableFrameCount(snapshot_, pool);
				if (reservedFrames[pool.segment_id.value()] >= available) {
					return false;
				}
			}
			++reservedFrames[pool.segment_id.value()];
			return true;
		};
		const auto reserveCurrentFrame = [&tryReserveCurrentFrame](const CatalogEntry &pool) {
			if (!tryReserveCurrentFrame(pool)) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native graph write has no COW frame capacity in the selected Entity pool");
			}
		};
		const auto reserveReplacementFrame = [&getCurrentPools, &tryReserveCurrentFrame, &labelPools, &reserveSegment,
											  this](const CatalogEntry &source) {
			const auto compatible = [&source](const CatalogEntry &candidate) {
				return candidate.kind == SegmentKind::SGK_ENTITY_PAGE_POOL &&
					   candidate.state == SegmentState::SGS_SEALED && candidate.owner_id == source.owner_id &&
					   candidate.generation.isValid();
			};
			if (compatible(source) && tryReserveCurrentFrame(source)) {
				return EntityPageReplacementDestination{source, std::nullopt};
			}
			for (const CatalogEntry &candidate: getCurrentPools()) {
				if (candidate.segment_id != source.segment_id && compatible(candidate) &&
					tryReserveCurrentFrame(candidate)) {
					return EntityPageReplacementDestination{candidate, std::nullopt};
				}
			}
			for (FreshEntityPool &candidate: labelPools) {
				if (candidate.owner_id == source.owner_id && candidate.generation.isValid() &&
					candidate.reservedFrameCount() < RSS_PAGE_POOL_DATA_FRAME_COUNT) {
					++candidate.replacement_frame_count;
					return EntityPageReplacementDestination{{}, candidate.replacementDestination()};
				}
			}
			FreshEntityPool destination{reserveSegment(),
										source.owner_id,
										Generation(1U),
										context_->options.first_slot,
										context_->options.last_slot,
										0U,
										1U};
			const EntityPageFreshPoolDestination fresh = destination.replacementDestination();
			labelPools.push_back(std::move(destination));
			return EntityPageReplacementDestination{{}, fresh};
		};
		const auto findAppendPool = [&]() -> std::optional<CatalogEntry> {
			for (const CatalogEntry &pool: getCurrentPools()) {
				if (reservedFrames[pool.segment_id.value()] < availableFrames(pool)) {
					return pool;
				}
			}
			return std::nullopt;
		};

		// A fresh pool is only reserved after a LabelSet plan materializes it. A
		// plan that overwrites an existing slot must not consume an unused logical
		// PageId or SegmentId from the durable high-water marks.
		EntityPageAuxiliaryMutationBatch auxiliary;
		std::set<uint64_t> auxiliaryReplacementPages;
		std::set<uint64_t> auxiliaryAppendPages;
		std::set<uint64_t> auxiliaryNewPages;
		const auto accountAuxiliary = [&](EntityPageAuxiliaryMutationBatch plan) {
			for (EntityPageAuxiliaryReplacement &replacement: plan.replacements) {
				if (!auxiliaryReplacementPages.insert(replacement.expected_location.page_id.value()).second) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native graph write cannot replace one auxiliary Entity page twice in one transaction");
				}
				// Retirement drops the old frame through the metadata publication; it
				// does not consume a second COW frame in the source pool.
				if (!replacement.retire_page) {
					const EntityPageReplacementDestination destination =
							reserveReplacementFrame(replacement.expected_pool);
					replacement.replacement_pool = destination.current_pool;
					replacement.replacement_new_pool = destination.fresh_pool;
				}
			}
			for (const EntityPageAuxiliaryAppend &append: plan.appends) {
				if (!auxiliaryAppendPages.insert(append.page_id.value()).second) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native graph write allocated a duplicate LabelSet append Page ID");
				}
				reserveCurrentFrame(append.expected_pool);
			}
			for (const EntityPageAuxiliaryNewPool &pool: plan.new_pools) {
				if (!auxiliaryNewPages.insert(pool.page_id.value()).second) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native graph write allocated a duplicate LabelSet new-pool Page ID");
				}
			}
			appendAuxiliary(auxiliary, std::move(plan));
		};

		// Build all LabelSet pages before Entity placement. The auxiliary frames
		// retain exact Node bindings and remain unreachable until the final batch.
		for (auto &[nodeId, change]: nodes_) {
			if (change.erased || !change.labels.has_value()) {
				continue;
			}
			const std::span<const uint64_t> labels(*change.labels);
			EntityLabelSetWritePlan labelPlan;
			if (change.inserted) {
				labelPlan.excluded_page_ids.reserve(auxiliaryReplacementPages.size());
				for (const uint64_t pageId: auxiliaryReplacementPages) {
					labelPlan.excluded_page_ids.emplace_back(pageId);
				}
			}
			FreshEntityPool candidateNewPool;
			FreshEntityPool *sharedNewPool = nullptr;
			bool createsNewPool = false;
			PageId candidatePageId{};
			if (labels.size() > RSS_ENTITY_INLINE_LABEL_CAPACITY) {
				candidatePageId = nextPage();
				if (const std::optional<CatalogEntry> appendPool = findAppendPool(); appendPool.has_value()) {
					labelPlan.append_page = EntityLabelSetAppendRequest{appendPool->segment_id, candidatePageId};
				} else {
					for (FreshEntityPool &pool: labelPools) {
						if (pool.reservedFrameCount() < RSS_PAGE_POOL_DATA_FRAME_COUNT) {
							sharedNewPool = &pool;
							break;
						}
					}
					if (sharedNewPool == nullptr) {
						candidateNewPool = {nextSegment(),
											context_->options.entity_pool_owner_id,
											Generation(1U),
											context_->options.first_slot,
											context_->options.last_slot,
											0U,
											0U};
						sharedNewPool = &candidateNewPool;
						createsNewPool = true;
					}
					labelPlan.new_pool = EntityLabelSetNewPoolRequest{
							sharedNewPool->segment_id, sharedNewPool->owner_id,	  sharedNewPool->generation,
							candidatePageId,		   sharedNewPool->first_slot, sharedNewPool->last_slot};
				}
			}

			if (change.inserted) {
				EntityLabelSetInitialNodePlan labelResult = context_->labels->planInitialNodeLabels(
						snapshot_, change.record, labels, labelPlan, checkpointLsn);
				change.record = std::move(labelResult.node);
				if (!labelResult.overflow.isEmpty()) {
					auxiliary.node_insertion_bindings.push_back({nodeId, change.record});
				}
				const bool appended = !labelResult.auxiliary_pages.appends.empty();
				const bool created = !labelResult.auxiliary_pages.new_pools.empty();
				if (appended || created) {
					static_cast<void>(reservePage());
				}
				if (created) {
					if (createsNewPool) {
						static_cast<void>(reserveSegment());
						candidateNewPool.initial_frame_count = labelResult.auxiliary_pages.new_pools.size();
						labelPools.push_back(candidateNewPool);
					} else {
						sharedNewPool->initial_frame_count += labelResult.auxiliary_pages.new_pools.size();
					}
				}
				accountAuxiliary(std::move(labelResult.auxiliary_pages));
			} else {
				EntityLabelSetMutationPlan labelResult =
						context_->labels->planNodeLabels(snapshot_, nodeId, labels, labelPlan, checkpointLsn);
				change.record = std::move(labelResult.replacement_node);
				const bool appended = !labelResult.auxiliary_pages.appends.empty();
				const bool created = !labelResult.auxiliary_pages.new_pools.empty();
				if (appended || created) {
					static_cast<void>(reservePage());
				}
				if (created) {
					if (createsNewPool) {
						static_cast<void>(reserveSegment());
						candidateNewPool.initial_frame_count = labelResult.auxiliary_pages.new_pools.size();
						labelPools.push_back(candidateNewPool);
					} else {
						sharedNewPool->initial_frame_count += labelResult.auxiliary_pages.new_pools.size();
					}
				}
				accountAuxiliary(std::move(labelResult.auxiliary_pages));
			}
		}

		std::vector<EntityPageBatchMutation> mutations;
		std::vector<EntityPageBatchAppend> appends;
		std::vector<EntityPageBatchNewPool> newPools;
		std::map<uint64_t, size_t> mutationOrdinals;
		std::map<uint64_t, uint16_t> remainingSlots;
		struct ExistingInsertCandidate {
			PageId page_id{};
			EntityPageRecordKind kind = EntityPageRecordKind::EPRK_NODE;
			CatalogEntry pool;
			uint16_t remaining = 0U;
			bool planned = false;
			bool cached = false;
		};
		const bool needsNodeInsertSearch = std::ranges::any_of(nodes_, [](const auto &entry) {
			return entry.second.inserted && !entry.second.erased;
		}) || rebuild_entity_free_summaries_;
		const bool needsEdgeInsertSearch = std::ranges::any_of(edges_, [](const auto &entry) {
			return entry.second.inserted && !entry.second.erased;
		}) || rebuild_entity_free_summaries_;
		using PageKindEntry = std::pair<const uint64_t, EntityPageRecordKind>;
        std::map<uint64_t, EntityPageRecordKind, std::less<uint64_t>, utils::OwnedPmrAllocator<PageKindEntry>> currentEntityPageKinds;
		utils::OwnedVector<ExistingInsertCandidate> existingInsertCandidates;
		existingInsertCandidates.reserve(currentEntityPageKinds.size());
		const auto targetKindIndex = [](EntityPageRecordKind kind) -> size_t {
			if (kind == EntityPageRecordKind::EPRK_NODE) {
				return 0U;
			}
			if (kind == EntityPageRecordKind::EPRK_EDGE) {
				return 1U;
			}
			fail(FormatError::FERR_INVALID_STATE, "native graph write encountered an unknown Entity record kind");
		};
        std::array<std::optional<EntityFreeSpaceIndex::Proof>, 2> freeSummaryProofs;
        std::array<bool, 2> freeSummaryRead{}, freeSummaryManaged{};
        const auto loadFreeSummary = [&](EntityPageRecordKind kind) {
            const size_t ordinal = targetKindIndex(kind);
            if (!freeSummaryRead[ordinal]) {
                freeSummaryProofs[ordinal] = EntityFreeSpaceIndex::readProof(*context_->metadata, *context_->indexes, snapshot_, kind);
                bool emptySource = !EntityFreeSpaceIndex::entityRoot(snapshot_.superblock(), kind).isValid();
                if (!freeSummaryProofs[ordinal] && !emptySource) {
                    bool any = false;
                    const auto first = [&](const EntityPageLocation &) { any = true; return false; };
                    if (kind == EntityPageRecordKind::EPRK_NODE) (void)context_->metadata->visitNodePageLocationsUntil(snapshot_, first);
                    else (void)context_->metadata->visitEdgePageLocationsUntil(snapshot_, first);
                    emptySource = !any;
                }
                freeSummaryManaged[ordinal] = rebuild_entity_free_summaries_ ||
                    (freeSummaryProofs[ordinal] ? freeSummaryProofs[ordinal]->fresh : emptySource);
                freeSummaryRead[ordinal] = true;
            }
            return freeSummaryManaged[ordinal];
        };
        using FreeCountEntry = std::pair<const uint64_t, uint16_t>;
        std::map<uint64_t, uint16_t, std::less<uint64_t>, utils::OwnedPmrAllocator<FreeCountEntry>> indexedFreeCounts;
		// All pages observed in one snapshot belong to a stable Catalog
		// generation. Cache the pool descriptor per segment while checking free
		// insertion slots instead of descending the Catalog once per page.
        using PoolEntry = std::pair<const uint64_t, CatalogEntry>;
		std::map<uint64_t, CatalogEntry, std::less<uint64_t>, utils::OwnedPmrAllocator<PoolEntry>> currentEntityPools;
		std::vector<NativeGraphWritePlannerContext::InsertionTarget> priorInsertionTargets;
		bool priorInsertionCacheValid = false;
		bool usedInsertionTargetCache = false;
        std::array<bool, 2> insertionKindsComplete{};
		{
			std::lock_guard cacheLock(context_->insertion_cache_mutex);
			const auto &cache = context_->insertion_cache;
			priorInsertionCacheValid = cache.valid;
			priorInsertionTargets = cache.targets;
            // PhysicalPageRef includes the COW generation, so this also
            // rejects a retired frame address reused by another publication.
            insertionKindsComplete = {
                cache.valid && cache.complete_kinds[0] && cache.node_directory_root == snapshot_.superblock().node_entity_directory_root,
                cache.valid && cache.complete_kinds[1] && cache.edge_directory_root == snapshot_.superblock().edge_entity_directory_root};
			// Directory roots are physical COW identities and can change after an
			// unrelated insertion. Cached logical PageIds remain usable; each selected
			// candidate is re-authenticated lazily against the current snapshot below.
			const auto hasCachedKind = [&](EntityPageRecordKind kind) {
				return std::ranges::any_of(cache.targets, [kind](const auto &target) { return target.kind == kind; });
			};
			usedInsertionTargetCache = !rebuild_entity_free_summaries_ && cache.valid &&
					(!needsNodeInsertSearch || insertionKindsComplete[0] || hasCachedKind(EntityPageRecordKind::EPRK_NODE)) &&
                    (!needsEdgeInsertSearch || insertionKindsComplete[1] || hasCachedKind(EntityPageRecordKind::EPRK_EDGE));
			if (usedInsertionTargetCache) {
				existingInsertCandidates.reserve(cache.targets.size());
				for (const auto &target : cache.targets) {
					existingInsertCandidates.push_back(
							{target.page_id, target.kind, target.pool, target.remaining, false, true});
					currentEntityPools.emplace(target.pool.segment_id.value(), target.pool);
				}
				debug::PerfTrace::addValue("native_graph.commit.entity_insert_target_cache_hits", 1);
			}
		}
		if (!usedInsertionTargetCache) {
			// Updates/deletes never need an insertion target. Avoid walking the
			// complete Entity Directory in those transactions; mixed writes inspect
			// only the kind that can consume a free slot.
            const auto discover = [&](EntityPageRecordKind kind, size_t needed) {
                const auto type = targetKindIndex(kind);
                if (!rebuild_entity_free_summaries_ && loadFreeSummary(kind)) {
                    if (freeSummaryProofs[type]) {
                        insertionKindsComplete[type] = EntityFreeSpaceIndex::visitCandidates(*context_->metadata, *context_->indexes,
                            snapshot_, kind, *freeSummaryProofs[type], needed, [&](const auto &entry) {
                                if (!currentEntityPageKinds.emplace(entry.page.value(), kind).second ||
                                    !indexedFreeCounts.emplace(entry.page.value(), entry.free_slots).second)
                                    fail(FormatError::FERR_INVALID_STATE, "free-space index repeats an entity page");
                                debug::PerfTrace::addValue("native_graph.write.entity_free_summary.candidate_pages", 1);
                            });
                    } else insertionKindsComplete[type] = true; // New empty directory, not a legacy migration.
                    return;
                }
                size_t visited = 0U;
                const auto visit = [&](const EntityPageLocation &location) {
                    if ((visited++ & 127U) == 0U && free_summary_checkpoint_) free_summary_checkpoint_();
                    debug::PerfTrace::addValue(kind == EntityPageRecordKind::EPRK_NODE ?
                        "native_graph.commit.entity_insert_node_directory_pages" : "native_graph.commit.entity_insert_edge_directory_pages", 1);
                    currentEntityPageKinds.emplace(location.page_id.value(), kind);
                };
                if (kind == EntityPageRecordKind::EPRK_NODE) context_->metadata->visitNodePageLocations(snapshot_, visit);
                else context_->metadata->visitEdgePageLocations(snapshot_, visit);
                insertionKindsComplete[type] = true;
            };
            if (needsNodeInsertSearch) discover(EntityPageRecordKind::EPRK_NODE,
                (std::max)(size_t{1}, size_t(std::ranges::count_if(nodes_, [](const auto &entry) { return entry.second.inserted && !entry.second.erased; }))));
            if (needsEdgeInsertSearch) discover(EntityPageRecordKind::EPRK_EDGE,
                (std::max)(size_t{1}, size_t(std::ranges::count_if(edges_, [](const auto &entry) { return entry.second.inserted && !entry.second.erased; }))));

			for (const auto &[pageValue, kind]: currentEntityPageKinds) {
				const PageId pageId(pageValue);
				const PageLocation location = context_->metadata->findPageLocation(snapshot_, pageId);
				auto poolIterator = currentEntityPools.find(location.segment_id.value());
				if (poolIterator == currentEntityPools.end()) {
					debug::PerfTrace::addValue("native_graph.commit.entity_insert_catalog_lookups", 1);
					poolIterator = currentEntityPools.emplace(location.segment_id.value(),
							context_->metadata->findCatalog(snapshot_, location.segment_id)).first;
				}
				const CatalogEntry &pool = poolIterator->second;
				if (!isCurrentPool(pool) || location.generation != pool.generation) {
					fail(FormatError::FERR_INVALID_STATE,
							"native graph write found an Entity Map page outside a current Entity pool");
				}
				const uint16_t remaining = context_->entities->freeSlotCount(snapshot_, pageId, kind);
                if (const auto indexed = indexedFreeCounts.find(pageValue); indexed != indexedFreeCounts.end() && indexed->second != remaining)
                    fail(FormatError::FERR_INVALID_STATE, "free-space index disagrees with the authenticated entity page");
                if (free_summary_checkpoint_) free_summary_checkpoint_();
				if (remaining != 0U) {
					existingInsertCandidates.push_back({pageId, kind, pool, remaining, false, false});
				}
			}
		}
        using InsertionOrderKey = std::pair<uint16_t, size_t>;
		std::array<std::set<InsertionOrderKey, std::less<InsertionOrderKey>,
            utils::OwnedPmrAllocator<InsertionOrderKey>>, 2U> existingInsertOrder;
		for (size_t ordinal = 0U; ordinal < existingInsertCandidates.size(); ++ordinal) {
			existingInsertOrder[targetKindIndex(existingInsertCandidates[ordinal].kind)].emplace(
					existingInsertCandidates[ordinal].remaining, ordinal);
		}

		const auto ensureMutation = [&](PageId pageId, EntityPageRecordKind kind) -> EntityPageBatchMutation & {
			const auto existing = mutationOrdinals.find(pageId.value());
			if (existing != mutationOrdinals.end()) {
				EntityPageBatchMutation &mutation = mutations[existing->second];
				if (mutation.record_kind != kind) {
					fail(FormatError::FERR_INVALID_STATE, "native graph write mixed Entity record kinds in one page");
				}
				return mutation;
			}
			const PageLocation location = context_->metadata->findPageLocation(snapshot_, pageId);
			auto poolIterator = currentEntityPools.find(location.segment_id.value());
			if (poolIterator == currentEntityPools.end()) {
				debug::PerfTrace::addValue("native_graph.commit.entity_mutation_catalog_lookups", 1);
				poolIterator = currentEntityPools.emplace(location.segment_id.value(),
						context_->metadata->findCatalog(snapshot_, location.segment_id)).first;
			}
			const CatalogEntry &pool = poolIterator->second;
			if (!isCurrentPool(pool) || location.generation != pool.generation) {
				fail(FormatError::FERR_INVALID_STATE, "native graph write selected a stale Entity page pool");
			}
			const EntityPageReplacementDestination replacement = reserveReplacementFrame(pool);
			remainingSlots.emplace(pageId.value(), context_->entities->freeSlotCount(snapshot_, pageId, kind));
			const size_t ordinal = mutations.size();
			EntityPageBatchMutation mutation;
			mutation.page_id = pageId;
			mutation.record_kind = kind;
			mutation.replacement_pool = replacement.current_pool;
			mutation.replacement_new_pool = replacement.fresh_pool;
			mutations.push_back(std::move(mutation));
			mutationOrdinals.emplace(pageId.value(), ordinal);
			return mutations.back();
		};
		// Resolve existing entities through one ordered Directory range per record
		// kind. Each selected page is still authenticated by ensureMutation(), but
		// large update/delete batches no longer pay one root-to-leaf descent per ID.
		std::vector<uint64_t> nodeMutationIds;
		std::vector<uint64_t> edgeMutationIds;
		// Endpoint reads participate in validation but do not modify Node records.
		// Adjacency is published separately; only labels or deletion need Node COW.
		const auto changesExistingNode = [](const auto &change) {
			return !change.inserted && (change.erased || change.labels.has_value());
		};
		for (const auto &[nodeId, change]: nodes_) {
			if (changesExistingNode(change)) {
				nodeMutationIds.push_back(nodeId);
			}
		}
		for (const auto &[edgeId, change]: edges_) {
			if (!change.inserted) {
				edgeMutationIds.push_back(edgeId);
			}
		}
		std::vector<std::optional<EntityPageLocation>> nodeMutationLocations(nodeMutationIds.size());
		std::vector<std::optional<EntityPageLocation>> edgeMutationLocations(edgeMutationIds.size());
		if (!nodeMutationIds.empty()) {
			context_->metadata->findNodePageLocationsSorted(snapshot_, nodeMutationIds, nodeMutationLocations);
		}
		if (!edgeMutationIds.empty()) {
			context_->metadata->findEdgePageLocationsSorted(snapshot_, edgeMutationIds, edgeMutationLocations);
		}
		const auto requireMutationLocation = [](uint64_t entityId, const std::vector<uint64_t> &ids,
				const std::vector<std::optional<EntityPageLocation>> &locations,
				const char *message) -> EntityPageLocation {
			const auto iterator = std::ranges::lower_bound(ids, entityId);
			if (iterator == ids.end() || *iterator != entityId) {
				throw std::logic_error(message);
			}
			const size_t ordinal = static_cast<size_t>(iterator - ids.begin());
			if (!locations[ordinal].has_value()) {
				fail(FormatError::FERR_INVALID_STATE, message);
			}
			return *locations[ordinal];
		};

		for (const auto &[nodeId, change]: nodes_) {
			if (!changesExistingNode(change)) {
				continue;
			}
			const EntityPageLocation location = requireMutationLocation(
				nodeId, nodeMutationIds, nodeMutationLocations,
				"native graph write could not resolve an existing Node mutation location");
			EntityPageBatchMutation &mutation = ensureMutation(location.page_id, EntityPageRecordKind::EPRK_NODE);
			if (change.erased) {
				mutation.erased_node_ids.push_back(nodeId);
			} else {
				mutation.node_updates.push_back({nodeId, change.record});
			}
		}
		for (const auto &[edgeId, change]: edges_) {
			if (change.inserted) {
				continue;
			}
			const EntityPageLocation location = requireMutationLocation(
				edgeId, edgeMutationIds, edgeMutationLocations,
				"native graph write could not resolve an existing Edge mutation location");
			EntityPageBatchMutation &mutation = ensureMutation(location.page_id, EntityPageRecordKind::EPRK_EDGE);
			if (change.erased) {
				mutation.erased_edge_ids.push_back(edgeId);
			} else {
				mutation.edge_updates.push_back({edgeId, change.record});
			}
		}

		// The target kind keeps append and fresh-pool ordinals disjoint. Their
		// vectors both start at zero, so a bool is insufficient to identify one.
		enum class OpenTargetStorage : uint8_t {
			OTS_MUTATION = 1,
			OTS_APPEND = 2,
			OTS_NEW_POOL = 3,
		};
		struct OpenTarget {
			EntityPageRecordKind kind = EntityPageRecordKind::EPRK_NODE;
			OpenTargetStorage storage = OpenTargetStorage::OTS_MUTATION;
			size_t ordinal = 0U;
			uint16_t remaining = 0U;
		};
		std::vector<OpenTarget> targets;
		// Keep open insertion targets ordered by remaining capacity. The previous
		// linear best-fit scan made a large batch O(records * targets); this index
		// keeps page grouping deterministic while reducing selection to O(log N).
		std::array<std::set<std::pair<uint16_t, size_t>>, 2U> targetOrder;
		const auto addTarget = [&](OpenTarget target) {
			const size_t ordinal = targets.size();
			targets.push_back(target);
			targetOrder[targetKindIndex(target.kind)].emplace(target.remaining, ordinal);
		};
		for (const auto &[pageValue, ordinal]: mutationOrdinals) {
			const EntityPageBatchMutation &mutation = mutations[ordinal];
			const uint16_t remaining = remainingSlots.at(pageValue);
			if (remaining != 0U) {
				addTarget({mutation.record_kind, OpenTargetStorage::OTS_MUTATION, ordinal, remaining});
			}
		}
		// LabelSet pages allocated above are initial frames of the same physical
		// Entity pool namespace. New Node/Edge pages may fill their remaining
		// frames instead of allocating a second pool for one transaction.
		std::vector<std::pair<SegmentId, size_t>> freshPoolPageCounts;
		freshPoolPageCounts.reserve(labelPools.size());
		for (const FreshEntityPool &pool: labelPools) {
			if (pool.reservedFrameCount() == 0U || pool.reservedFrameCount() > RSS_PAGE_POOL_DATA_FRAME_COUNT) { // fresh LabelSet pool reservations are bounded when the planner creates them.
				fail(FormatError::FERR_INVALID_STATE,
					 "native graph write recorded an invalid fresh LabelSet pool frame count");
			}
			freshPoolPageCounts.emplace_back(pool.segment_id, pool.reservedFrameCount());
		}
		const auto createNewEntityPage = [&](EntityPageRecordKind kind) -> OpenTarget {
			for (const auto &[segmentId, count]: freshPoolPageCounts) {
				if (count < RSS_PAGE_POOL_DATA_FRAME_COUNT) {
					const size_t ordinal = newPools.size();
					EntityPageNewPoolRequest request{
							segmentId,	   context_->options.entity_pool_owner_id, Generation(1U),
							reservePage(), context_->options.first_slot,		   context_->options.last_slot};
					newPools.push_back({request, kind, {}, {}});
					for (auto &entry: freshPoolPageCounts) {
						if (entry.first == segmentId) {
							++entry.second;
							break;
						}
					}
					return {kind, OpenTargetStorage::OTS_NEW_POOL, ordinal, EntityPage::recordCapacity(kind)};
				}
			}
			const SegmentId segmentId = reserveSegment();
			freshPoolPageCounts.push_back({segmentId, 1U});
			const size_t ordinal = newPools.size();
			EntityPageNewPoolRequest request{
					segmentId,	   context_->options.entity_pool_owner_id, Generation(1U),
					reservePage(), context_->options.first_slot,		   context_->options.last_slot};
			newPools.push_back({request, kind, {}, {}});
			return {kind, OpenTargetStorage::OTS_NEW_POOL, ordinal, EntityPage::recordCapacity(kind)};
		};
		const auto createAppendEntityPage = [&](EntityPageRecordKind kind) -> OpenTarget {
			const std::optional<CatalogEntry> pool = findAppendPool();
			if (!pool.has_value()) {
				return createNewEntityPage(kind);
			}
			reserveCurrentFrame(*pool);
			const size_t ordinal = appends.size();
			appends.push_back({{*pool, reservePage()}, kind, {}, {}});
			return {kind, OpenTargetStorage::OTS_APPEND, ordinal, EntityPage::recordCapacity(kind)};
		};
		const auto refreshCachedCandidate = [&](ExistingInsertCandidate &candidate) {
			if (!candidate.cached) {
				return candidate.remaining != 0U;
			}
			const std::optional<PageLocation> location =
					context_->metadata->findPageLocationOptional(snapshot_, candidate.page_id);
			if (!location.has_value()) {
				candidate.remaining = 0U;
				return false;
			}
			const CatalogEntry pool = context_->metadata->findCatalog(snapshot_, location->segment_id);
			if (!isCurrentPool(pool) || location->generation != pool.generation) {
				candidate.remaining = 0U;
				return false;
			}
			candidate.pool = pool;
			currentEntityPools.insert_or_assign(pool.segment_id.value(), pool);
			candidate.remaining = context_->entities->freeSlotCount(snapshot_, candidate.page_id, candidate.kind);
			return candidate.remaining != 0U;
		};
		const auto insertNew = [&](uint64_t entityId, const auto &record, EntityPageRecordKind kind) {
			const size_t kindIndex = targetKindIndex(kind);
			std::optional<size_t> targetOrdinal;
			if (!targetOrder[kindIndex].empty()) {
				targetOrdinal = targetOrder[kindIndex].begin()->second;
			}
			if (!targetOrdinal.has_value()) {
				std::optional<size_t> candidateOrdinal;
				for (auto iterator = existingInsertOrder[kindIndex].begin();
						iterator != existingInsertOrder[kindIndex].end();) {
					const auto [indexedRemaining, ordinal] = *iterator++;
					ExistingInsertCandidate &candidate = existingInsertCandidates[ordinal];
					if (candidate.cached && candidate.remaining != indexedRemaining) {
						existingInsertOrder[kindIndex].erase({indexedRemaining, ordinal});
						if (refreshCachedCandidate(candidate)) {
							existingInsertOrder[kindIndex].emplace(candidate.remaining, ordinal);
						}
						continue;
					}
					if (candidate.cached && !refreshCachedCandidate(candidate)) {
						existingInsertOrder[kindIndex].erase({indexedRemaining, ordinal});
						continue;
					}
					if (!candidate.planned && candidate.remaining != 0U &&
							reservedFrames[candidate.pool.segment_id.value()] < availableFrames(candidate.pool)) {
						candidateOrdinal = ordinal;
						break;
					}
				}
				if (candidateOrdinal.has_value()) {
					ExistingInsertCandidate &candidate = existingInsertCandidates[*candidateOrdinal];
					static_cast<void>(ensureMutation(candidate.page_id, kind));
					const size_t ordinal = mutationOrdinals.at(candidate.page_id.value());
					candidate.planned = true;
					addTarget({kind, OpenTargetStorage::OTS_MUTATION, ordinal, candidate.remaining});
					targetOrdinal = targets.size() - 1U;
				} else {
					addTarget(createAppendEntityPage(kind));
					targetOrdinal = targets.size() - 1U;
				}
			}
			OpenTarget &target = targets[*targetOrdinal];
			// The selected target is the smallest remaining page. Re-key it after
			// consuming one slot so the next insert keeps the same best-fit order.
			targetOrder[kindIndex].erase({target.remaining, *targetOrdinal});
			if constexpr (std::is_same_v<std::decay_t<decltype(record)>, NodeEntityRecord>) {
				if (target.storage == OpenTargetStorage::OTS_MUTATION) {
					mutations[target.ordinal].node_insertions.push_back({entityId, record});
				} else if (target.storage == OpenTargetStorage::OTS_APPEND) {
					appends[target.ordinal].node_insertions.push_back({entityId, record});
				} else {
					newPools[target.ordinal].node_insertions.push_back({entityId, record});
				}
			} else {
				if (target.storage == OpenTargetStorage::OTS_MUTATION) {
					mutations[target.ordinal].edge_insertions.push_back({entityId, record});
				} else if (target.storage == OpenTargetStorage::OTS_APPEND) {
					appends[target.ordinal].edge_insertions.push_back({entityId, record});
				} else {
					newPools[target.ordinal].edge_insertions.push_back({entityId, record});
				}
			}
			--target.remaining;
			if (target.remaining != 0U) {
				targetOrder[kindIndex].emplace(target.remaining, *targetOrdinal);
			}
		};

		for (const auto &[nodeId, change]: nodes_) {
			if (change.inserted && !change.erased) {
				insertNew(nodeId, change.record, EntityPageRecordKind::EPRK_NODE);
			}
		}
		for (const auto &[edgeId, change]: edges_) {
			if (change.inserted && !change.erased) {
				insertNew(edgeId, change.record, EntityPageRecordKind::EPRK_EDGE);
			}
		}
		// Record the generic page-grouping result once the bounded target index has
		// assigned every insertion.  These counters attribute write amplification
		// without exposing physical locations to the logical API.
		size_t insertionTargetPages = 0U;
		size_t insertionTargetRecords = 0U;
		const auto accountInsertionTargets = [&](const auto &batches) {
			for (const auto &batch: batches) {
				if (batch.node_insertions.size() > std::numeric_limits<size_t>::max() - batch.edge_insertions.size()) {
					fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
						"native graph insertion target record count overflowed");
				}
				const size_t records = batch.node_insertions.size() + batch.edge_insertions.size();
				if (records != 0U) {
					if (insertionTargetPages == std::numeric_limits<size_t>::max() ||
						insertionTargetRecords > std::numeric_limits<size_t>::max() - records) {
						fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
							"native graph insertion target accounting overflowed");
					}
					++insertionTargetPages;
					insertionTargetRecords += records;
				}
			}
		};
		accountInsertionTargets(mutations);
		accountInsertionTargets(appends);
		accountInsertionTargets(newPools);
		debug::PerfTrace::addValue("native_graph.commit.entity_insert_target_pages",
				static_cast<int64_t>(insertionTargetPages));
		debug::PerfTrace::addValue("native_graph.commit.entity_insert_target_records",
				static_cast<int64_t>(insertionTargetRecords));

        std::shared_ptr<query::execution::MaterializedByteBudget> pendingFieldsMemory;
        std::optional<utils::OwnedVector<NativeHybridPropertyColumnReplacement>> pendingFields;
        std::optional<utils::OwnedVector<uint64_t>> pendingPropertyKeys;
        std::optional<utils::OwnedVector<EntityPropertyKey>> nativeOwnerDeletions;
        std::optional<utils::OwnedVector<uint32_t>> pendingCatalogTokens;
        std::vector<EntityPropertyRootMutationPlan> propertyPlans;
        propertyPlans.reserve(properties_.size());
        std::vector<PropertyCollectionRoot> propertyRoots;
        propertyRoots.reserve(properties_.size());
        // An absent immutable source group has no per-owner authority to
        // authenticate. Reuse only that negative fact within this publication;
        // existing groups still validate every owner's Entity/competing ROW
        // route. Never carry this hint across an I/O mutation or a transaction.
        struct AbsentPropertyGroup {
            EntityPropertyOwnerKind kind;
            uint64_t group;
            uint64_t epoch;
        };
        std::optional<AbsentPropertyGroup> absentPropertyGroup;
        for (const auto &[key, root] : properties_) {
            const auto owner = EntityPropertyKey::fromDirectoryId(key);
            utils::OwnedHeapReservation boundary(utils::OwnedAllocationScope::owner());
            const SelectedPropertyReadAdmission admission = [&](size_t bytes) { boundary.set(bytes); };
            bool native = false;
            if (native_graph_coordinated_) {
                const uint64_t epoch = context_->metadata->dataMutationEpoch();
                const uint64_t group = owner.isValid() ? 1U + (owner.entity_id - 1U) / RSS_HYBRID_GROUP_CAPACITY : 0U;
                bool absent = absentPropertyGroup && owner.isValid() && epoch != UINT64_MAX &&
                    absentPropertyGroup->epoch == epoch && absentPropertyGroup->kind == owner.owner_kind &&
                    absentPropertyGroup->group == group && context_->metadata->ownsSnapshot(snapshot_);
                if (absent) {
                    debug::PerfTrace::addValue("native_graph.write.group_absence_cache_hits", 1);
                    // Observers may attempt a write; they cannot retag a hint.
                    absent = context_->metadata->dataMutationEpoch() == epoch &&
                        context_->metadata->ownsSnapshot(snapshot_);
                }
                if (!absent) {
                    native = context_->properties->hasHybridPropertyGroup(snapshot_, owner, nullptr, &admission);
                    debug::PerfTrace::addValue("native_graph.write.group_absence_cache_probes", 1);
                    if (!native && owner.isValid() && epoch != UINT64_MAX &&
                            context_->metadata->dataMutationEpoch() == epoch)
                        absentPropertyGroup = AbsentPropertyGroup{owner.owner_kind, group, epoch};
                    else absentPropertyGroup.reset();
                }
            }
            boundary.set(0);
            if (!native) { propertyRoots.push_back(root); continue; }
            const auto node = nodes_.find(owner.entity_id); const auto edge = edges_.find(owner.entity_id);
            const bool inserted = owner.owner_kind == EntityPropertyOwnerKind::EPOK_NODE ?
                node != nodes_.end() && node->second.inserted && !node->second.erased :
                edge != edges_.end() && edge->second.inserted && !edge->second.erased;
            if (!pendingFieldsMemory) {
                pendingFieldsMemory = nativeFieldWorkspace();
                pendingFields.emplace(utils::OwnedPmrAllocator<NativeHybridPropertyColumnReplacement>(pendingFieldsMemory));
                pendingPropertyKeys.emplace(utils::OwnedPmrAllocator<uint64_t>(pendingFieldsMemory));
                nativeOwnerDeletions.emplace(utils::OwnedPmrAllocator<EntityPropertyKey>(pendingFieldsMemory));
                pendingCatalogTokens.emplace(utils::OwnedPmrAllocator<uint32_t>(pendingFieldsMemory));
            }
            utils::OwnedAllocationScope scope(pendingFieldsMemory);
            if (pendingPropertyKeys->size() == 64U)
                fail(FormatError::FERR_INVALID_STATE, "native graph property batch exceeds64owners");
            pendingPropertyKeys->push_back(key);
            if (inserted) {
                if (root.entries.size() > 64U - pendingFields->size())
                    fail(FormatError::FERR_INVALID_STATE, "native owner creation exceeds64changed fields");
                for (const auto &entry : root.entries)
                    pendingFields->push_back({owner, entry.key_token, OwnedPropertyValue(entry.value), false});
            } else {
                const bool erased = owner.owner_kind == EntityPropertyOwnerKind::EPOK_NODE ?
                    node != nodes_.end() && node->second.erased : edge != edges_.end() && edge->second.erased;
                if (erased) { nativeOwnerDeletions->push_back(owner); continue; }
                // Reuse the authenticated map differ without granting the
                // temporary transaction any publication authority.
                NativeGraphWriteTransaction fields(context_, context_->metadata->openSnapshot());
                if (fields.baseSuperblock() != snapshot_.superblock())
                    fail(FormatError::FERR_INVALID_STATE, "native graph field planning lost its source fence");
                fields.stageHybridPropertyMaps(std::span(&root, 1U), NativeHybridPropertyMapMode::NHPMM_REPLACE, pendingFieldsMemory);
                if (fields.hybrid_columns_->cells.size() > 64U - pendingFields->size())
                    fail(FormatError::FERR_INVALID_STATE, "native graph property batch exceeds64changed fields");
                for (auto &cell : fields.hybrid_columns_->cells) pendingFields->push_back(std::move(cell));
                fields.rollback();
            }
        }


		{
			debug::ScopedPerfTimer propertyPlanTimer("native_graph.commit.property_plan");
			propertyPlans = context_->properties->planEntityRootReplacements(snapshot_, propertyRoots,
																						 context_->options.property_roots);
		}

		struct PendingDerivedIndexBinding {
			NativeGraphIndexTreeMutation mutation;
			size_t expected_pool_count = 0U;
			BTreeStructuralMutationPreparer preparer;
		};
		std::vector<PendingDerivedIndexBinding> pendingDerivedBindings;
		pendingDerivedBindings.reserve(scalar_index_mutations_.size() + initial_scalar_index_mutations_.size() + 1U);
		const std::vector<BTreeScalarIndexOperation> *deferredAdjacencyOperations = nullptr;
		std::optional<NativeGraphPrivateSegmentPlan> deferredAdjacencyPrivateSegmentPlan;
		NativeGraphPrivateSegmentImagePreparer privateSegmentImagePreparer;
		NativeGraphDeferredIndexPlanner deferredIndexPlanner;
		if (global_adjacency_deferred_bulk_) {
			const std::vector<BTreeScalarIndexOperation> &operations = canonicalGlobalAdjacencyOperations();
			const NativeBulkRunFragmentLayout fragmentLayout = planDeferredAdjacencyFragments(operations);
			const size_t fragmentCount = fragmentLayout.fragments.size();
			deferredAdjacencyOperations = &operations;
			// Scratch candidates are a private append-only suffix, not allocator
			// destinations for final graph pages. Do not inherit the final-page
			// placement range: a bounded arbitrary range would reintroduce a
			// per-fragment slot map or reject a valid current file tail.
			deferredAdjacencyPrivateSegmentPlan = {SegmentKind::SGK_BULK_SCRATCH,
					RSS_GLOBAL_ADJACENCY_INDEX_ID, PhysicalSlotId(1U),
					PhysicalSlotId(std::numeric_limits<uint64_t>::max()),
					RSS_ENVELOPE_BYTES, {}, fragmentCount + 1U};
			privateSegmentImagePreparer = [operations = deferredAdjacencyOperations, fragmentLayout](
					const NativeGraphPrivateSegmentSet &identities,
					size_t ordinal) -> std::vector<std::byte> {
				const size_t fragmentCount = fragmentLayout.fragments.size();
				if (operations == nullptr || identities.size() != fragmentCount + 1U || ordinal >= identities.size()) { // ZYX_COV_EXCL_LINE: metadata invokes this callback with the immutable partition it just validated.
					fail(FormatError::FERR_INVALID_STATE,
						 "native graph deferred adjacency bulk run lost its scratch identity partition");
				}
				if (ordinal < fragmentCount) {
					std::vector<std::byte> encoded = NativeBulkRunFragmentCodec::encode(
							deferredAdjacencyFragment(*operations, fragmentLayout.fragments[ordinal]));
					if (encoded.size() != fragmentLayout.fragments[ordinal].encoded_bytes) {
						fail(FormatError::FERR_INVALID_STATE,
							 "native graph deferred adjacency source changed after fragment planning");
					}
					return encoded;
				}
				NativeBulkRunManifest manifest;
				manifest.fragments.reserve(fragmentCount);
				for (size_t fragmentOrdinal = 0U; fragmentOrdinal < fragmentCount; ++fragmentOrdinal) {
					const NativeGraphPrivateSegmentMaterialization identity = identities[fragmentOrdinal];
					manifest.fragments.push_back({identity.segment_id, identity.generation, identity.owner_id});
				}
				manifest.entry_count = operations->size();
				manifest.encoded_bytes = fragmentLayout.encoded_bytes;
				return NativeBulkRunManifestCodec::encode(manifest);
			};
			deferredIndexPlanner = [indexes = context_->indexes, metadata = context_->metadata,
										options = context_->options, checkpointLsn, fragmentCount](
										const NativeGraphDeferredIndexPlanningContext &planning) {
					if (indexes == nullptr || metadata == nullptr || planning.private_segments.size() != fragmentCount + 1U) { // ZYX_COV_EXCL_LINE: the planner captures live stores and metadata supplies the exact scratch partition.
					fail(FormatError::FERR_INVALID_STATE,
						 "native graph deferred adjacency planner lost its storage or scratch partition");
				}
				const NativeGraphPrivateSegmentMaterialization manifestIdentity = planning.private_segments.back();
				const NativeBulkRunFragmentLocation manifestLocation{
						manifestIdentity.segment_id, manifestIdentity.generation, manifestIdentity.owner_id};
				// The returned B+Tree materialization callback outlives this planning
				// context. Copy the scoped handles, rather than retaining a reference to
				// the temporary context object passed by the metadata coordinator.
				const NativeBulkRunPayloadReader reader = [privateSegments = planning.private_segments,
						privateSegmentReader = planning.private_segment_reader](const NativeBulkRunFragmentLocation &location) {
					const NativeGraphPrivateSegmentMaterialization identity{
							location.segment_id, location.generation, location.transaction_id};
					try {
						static_cast<void>(privateSegments.ordinalOf(identity));
					} catch (const FormatException &) {
						fail(FormatError::FERR_INVALID_STATE,
								 "native graph deferred adjacency planner received a foreign scratch reference");
					}
					return privateSegmentReader(identity);
				};
				const OrderedBTreeEntrySource operations = NativeBulkRunStore::orderedSourceFromPayloadReader(
						manifestLocation, 1U, reader);
				BulkBTreeBuildOptions build;
				build.first_slot = options.first_slot;
				build.last_slot = options.last_slot;
				build.pool_owner_id = RSS_GLOBAL_ADJACENCY_INDEX_ID;
				build.maximum_entries = maximumAdjacencyTreeEntries(options);
				build.maximum_pages = options.maximum_global_adjacency_tree_pages;
				build.leaf_value_layout = BTreeLeafValueLayout::BLVL_EMPTY;
				NativeGraphDeferredIndexPlan result;
				result.mutation.index_id = RSS_GLOBAL_ADJACENCY_INDEX_ID;
				const std::optional<IndexRootLocation> existingRoot =
						metadata->findIndexRootOptional(planning.current_snapshot, RSS_GLOBAL_ADJACENCY_INDEX_ID);
				if (!existingRoot.has_value()) {
					const OrderedBTreeEntrySource entries = [operations] {
						OrderedBTreeEntryCursor cursor = operations();
						return [cursor = std::move(cursor)]() mutable -> std::optional<BTreePageEntry> {
							std::optional<BTreePageEntry> operation = cursor();
							if (!operation.has_value()) {
								return std::nullopt;
							}
							if (operation->value_or_child != 1U) {
								fail(FormatError::FERR_INVALID_STATE,
									 "native graph cannot erase an adjacency key from an absent deferred tree");
							}
							return BTreePageEntry{std::move(operation->key), 0U};
						};
					};
					const BTreeInitialTreeMutationPlan plan = BulkBTreeBuilder::planNativeTree(
							*indexes, *metadata, planning.current_snapshot, RSS_GLOBAL_ADJACENCY_INDEX_ID, entries, build);
					for (const BTreeNewIndexTreePoolMutation &pool: plan.new_pool_mutations) {
						result.mutation.new_pool_requests.push_back(
								{build.first_slot, build.last_slot, pool.owner_id, pool.appended_page_count});
					}
					result.mutation.replacement_root = {
							plan.root_page.pool_ordinal, plan.root_page.appended_page_ordinal, true};
					result.preparer = [preparer = plan.preparer, checkpointLsn](
							std::vector<IndexTreeMutationPoolBatch> &batches) { preparer(batches, checkpointLsn); };
					return result;
				}

				const MetadataSnapshotPin *snapshot = &planning.current_snapshot;
				BTreeIndex existing(RSS_GLOBAL_ADJACENCY_INDEX_ID, *indexes, *metadata,
									BTreeLeafValueLayout::BLVL_EMPTY);
				const OrderedBTreeEntrySource oldEntries =
						[existing = std::move(existing), snapshot,
						 maximumEntries = maximumAdjacencyTreeEntries(options)]() mutable {
							return existing.orderedSource(*snapshot, maximumEntries)();
						};
				const OrderedBTreeEntrySource merged = GlobalAdjacencyMutationMerger::merge(
						oldEntries, operations, maximumAdjacencyTreeEntries(options));
				const BTreeInitialTreeMutationPlan replacement = BulkBTreeBuilder::planNativeReplacementTree(
						*indexes, *metadata, planning.current_snapshot, RSS_GLOBAL_ADJACENCY_INDEX_ID, merged, build);
				const BTreeTreeRetirementPlan retirement = indexes->planTreeRetirement(
						planning.current_snapshot, RSS_GLOBAL_ADJACENCY_INDEX_ID, options.maximum_global_adjacency_tree_pages);
				if (replacement.entry_count == 0U) {
					result.mutation.expected_root = retirement.expected_root;
					result.mutation.pool_requests = retirement.pool_requests;
					result.mutation.retire_root = true;
					result.preparer = [preparer = retirement.preparer, checkpointLsn](
							std::vector<IndexTreeMutationPoolBatch> &batches) { preparer(batches, checkpointLsn); };
					return result;
				}
				result.mutation.expected_root = retirement.expected_root;
				result.mutation.pool_requests = retirement.pool_requests;
				for (const BTreeNewIndexTreePoolMutation &pool: replacement.new_pool_mutations) {
					result.mutation.new_pool_requests.push_back(
							{build.first_slot, build.last_slot, pool.owner_id, pool.appended_page_count});
				}
				result.mutation.replacement_root = {
						replacement.root_page.pool_ordinal, replacement.root_page.appended_page_ordinal, true};
				const size_t retirementPoolCount = retirement.pool_requests.size();
				result.preparer = [retirementPreparer = retirement.preparer, replacementPreparer = replacement.preparer,
							retirementPoolCount, checkpointLsn](std::vector<IndexTreeMutationPoolBatch> &batches) {
					if (batches.size() < retirementPoolCount) {
						fail(FormatError::FERR_INVALID_STATE,
							 "native graph deferred adjacency replacement changed its pool partition");
					}
					std::vector<IndexTreeMutationPoolBatch> retired(
							batches.begin(), batches.begin() + static_cast<ptrdiff_t>(retirementPoolCount));
					std::vector<IndexTreeMutationPoolBatch> fresh(
							batches.begin() + static_cast<ptrdiff_t>(retirementPoolCount), batches.end());
					retirementPreparer(retired, checkpointLsn);
					replacementPreparer(fresh, checkpointLsn);
					std::move(retired.begin(), retired.end(), batches.begin());
					std::move(fresh.begin(), fresh.end(),
							batches.begin() + static_cast<ptrdiff_t>(retirementPoolCount));
				};
				return result;
			};
		}
		// A new graph-owned LargeValue cannot become reachable without its full
		// content-identity entry. Plan this reserved tree before binding any
		// derived mutation so its current-pool frames join the shared budget.
		std::vector<NativeGraphLargeValueMutation> contentValuesToPublish;
		std::optional<LargeValueContentIndexPublicationPlan> contentIndexPlan;
		LargeValueStore *contentIndexStore = nullptr;
		for (const EntityPropertyRootMutationPlan &propertyPlan: propertyPlans) {
			contentValuesToPublish.insert(contentValuesToPublish.end(), propertyPlan.large_values.begin(),
														 propertyPlan.large_values.end());
		}
		if (!contentValuesToPublish.empty() && (!pendingFields || pendingFields->empty())) {
			const bool conflictsWithUserMutation =
					std::ranges::any_of(scalar_index_mutations_,
										[](const StagedScalarIndexMutation &mutation) {
											return mutation.index_id == RSS_LARGE_VALUE_CONTENT_INDEX_ID;
										}) ||
					std::ranges::any_of(initial_scalar_index_mutations_,
										[](const StagedInitialScalarIndexMutation &mutation) {
											return mutation.index_id == RSS_LARGE_VALUE_CONTENT_INDEX_ID;
										});
			if (conflictsWithUserMutation) {
				fail(FormatError::FERR_INVALID_STATE,
						"native graph transaction cannot modify the reserved LargeValue content index directly");
			}
			contentIndexStore = context_->properties->largeValueStore();
			if (contentIndexStore == nullptr || !contentIndexStore->usesMetadataStore(*context_->metadata)) { // the attached property store owns a validated local LargeValue store.
				fail(FormatError::FERR_INVALID_STATE,
						"native graph LargeValue materialization has no local persistent content index");
			}
			contentIndexPlan = contentIndexStore->planNativeGraphContentIndexMutation(
					snapshot_, contentValuesToPublish, context_->options.first_slot, context_->options.last_slot);
		}
		// Every current-pool B+Tree mutation now competes in one deterministic
		// frame budget. Fresh rebuilds are selected before any typed preparer runs,
		// so scalar, adjacency, and LargeValue content trees cannot overbook a pool.
        std::vector<NativeGraphEntityFreeSummaryFence> freeSummaryFences;
        const auto stageFreeSummaries = [&](EntityPageRecordKind kind) {
            const size_t type = targetKindIndex(kind);
            const auto insertedCount = [kind](const auto &page) {
                return kind == EntityPageRecordKind::EPRK_NODE ? page.node_insertions.size() : page.edge_insertions.size();
            };
            bool changed = false;
            for (const auto &page : mutations) if (page.record_kind == kind)
                changed = changed || insertedCount(page) || (kind == EntityPageRecordKind::EPRK_NODE ? !page.erased_node_ids.empty() : !page.erased_edge_ids.empty());
            for (const auto &page : appends) changed = changed || (page.record_kind == kind && insertedCount(page));
            for (const auto &page : newPools) changed = changed || (page.record_kind == kind && insertedCount(page));
            if (!changed && !rebuild_entity_free_summaries_) return;
            const bool managed = loadFreeSummary(kind);
            // Physical evacuation invalidates insertion authority without
            // changing the summary's contents. Complete scan writes must still
            // maintain its changed entries; retaining the old marker does not
            // certify unvisited pages or enable the insertion fast path.
            const bool retainStale = RSS_SCAN_PATH_FORMAT_ENABLED && !managed && freeSummaryProofs[type].has_value();
            if (!managed && !retainStale) return;
            const uint64_t id = EntityFreeSpaceIndex::indexId(kind);
            auto position = std::lower_bound(scalar_index_mutations_.begin(), scalar_index_mutations_.end(), id,
                [](const auto &staged, uint64_t key) { return staged.index_id < key; });
            if (position != scalar_index_mutations_.end() && position->index_id == id)
                fail(FormatError::FERR_INVALID_STATE, "free-space summary overlaps a caller Index mutation");
            position = scalar_index_mutations_.insert(position, {context_->indexes, id, {}, {}, {}, BTreeLeafValueLayout::BLVL_U64});
            auto &operations = position->operations;
            uint64_t freePages = freeSummaryProofs[type] ? freeSummaryProofs[type]->free_pages : 0U;
            BTreeIndex existing(id, *context_->indexes, *context_->metadata);
            const auto append = [&](BTreeScalarIndexOperationKind operation, const auto &key, uint64_t value = 1U) {
                if (!free_summary_credit_)
                    free_summary_credit_ = std::make_unique<utils::OwnedHeapReservation>(utils::OwnedAllocationScope::owner(), 1024U);
                const size_t admitted = free_summary_credit_->bytes();
                if (admitted > (std::numeric_limits<size_t>::max)() - 256U)
                    fail(FormatError::FERR_INVALID_STATE, "free-space summary operation admission overflows");
                // Covers vector growth and each small owned key before STL allocation.
                // Retain admission with the staged operations on failed commits too.
                free_summary_credit_->set(admitted + 256U);
                operations.push_back({operation, std::vector<std::byte>(key.begin(), key.end()), value});
            };
            if (rebuild_entity_free_summaries_) {
                if (freeSummaryProofs[type]) {
                    const uint64_t count = freeSummaryProofs[type]->free_pages;
                    auto old = existing.openRangeCursor(snapshot_, std::nullopt, size_t(count) + 3U, BufferPoolReadHint::BPRH_NORMAL);
                    size_t visited = 0U;
                    while (const auto entry = old.next()) {
                        if (++visited > count + 2U) fail(FormatError::FERR_INVALID_STATE, "summary rebuild found uncounted entries");
                        append(BTreeScalarIndexOperationKind::BSIOP_ERASE, entry->key);
                    }
                    if (visited != count + 2U) fail(FormatError::FERR_INVALID_STATE, "summary rebuild found missing entries");
                }
                freePages = 0U;
                for (const auto &candidate : existingInsertCandidates) if (candidate.kind == kind && candidate.remaining) {
                    append(BTreeScalarIndexOperationKind::BSIOP_UPSERT,
                        EntityFreeSpaceIndex::entryKey(candidate.page_id, candidate.remaining, kind));
                    ++freePages;
                }
            } else {
                const uint16_t capacity = EntityPage::recordCapacity(kind);
                for (const auto &page : mutations) {
                    if (page.record_kind != kind) continue;
                    const size_t inserted = insertedCount(page);
                    const size_t erased = kind == EntityPageRecordKind::EPRK_NODE ? page.erased_node_ids.size() : page.erased_edge_ids.size();
                    if (!inserted && !erased) continue;
                    const uint16_t before = remainingSlots.at(page.page_id.value());
                    if (inserted > size_t(before) + erased || size_t(before) + erased - inserted > capacity)
                        fail(FormatError::FERR_INVALID_STATE, "summary mutation has an impossible free-slot count");
                    const auto after = uint16_t(size_t(before) + erased - inserted);
                    if (before == after) continue;
                    if (before) {
                        const auto key = EntityFreeSpaceIndex::entryKey(page.page_id, before, kind);
                        if (!freePages || !existing.find(snapshot_, key))
                            fail(FormatError::FERR_INVALID_STATE, "fresh summary is missing a changed free page");
                        append(BTreeScalarIndexOperationKind::BSIOP_ERASE, key); --freePages;
                    }
                    if (after && after < capacity) {
                        append(BTreeScalarIndexOperationKind::BSIOP_UPSERT, EntityFreeSpaceIndex::entryKey(page.page_id, after, kind));
                        ++freePages;
                    }
                }
                const auto addFresh = [&](const auto &page) {
                    if (page.record_kind != kind) return;
                    const size_t count = insertedCount(page);
                    if (!count || count > capacity) fail(FormatError::FERR_INVALID_STATE, "fresh summary page has invalid occupancy");
                    if (count < capacity) {
                        append(BTreeScalarIndexOperationKind::BSIOP_UPSERT,
                            EntityFreeSpaceIndex::entryKey(page.request.page_id, uint16_t(capacity - count), kind));
                        ++freePages;
                    }
                };
                for (const auto &page : appends) addFresh(page);
                for (const auto &page : newPools) addFresh(page);
            }
            const auto sourceRoot = EntityFreeSpaceIndex::entityRoot(snapshot_.superblock(), kind);
            const uint64_t generation = retainStale ? freeSummaryProofs[type]->directory_generation :
                (changed ? snapshot_.superblock().generation.value() + 1U : sourceRoot.generation.value());
            if (generation == (std::numeric_limits<uint64_t>::max)() || freePages == (std::numeric_limits<uint64_t>::max)())
                fail(FormatError::FERR_INVALID_STATE, "free-space summary marker encoding overflows");
            append(BTreeScalarIndexOperationKind::BSIOP_UPSERT, EntityFreeSpaceIndex::generationKey(), generation + 1U);
            append(BTreeScalarIndexOperationKind::BSIOP_UPSERT, EntityFreeSpaceIndex::countKey(), freePages + 1U);
            freeSummaryFences.push_back({id, sourceRoot, Generation(generation), freePages});
            debug::PerfTrace::addValue("native_graph.write.entity_free_summary.changed_operations", operations.size());
            if (!freeSummaryProofs[type]) {
                std::vector<BTreePageEntry> entries;
                entries.reserve(operations.size());
                for (const auto &operation : operations) {
                    BTreePageEntry entry;
                    entry.key.assign(operation.key.begin(), operation.key.end());
                    entry.value_or_child = operation.value;
                    entries.push_back(std::move(entry));
                }
                std::ranges::sort(entries, {}, &BTreePageEntry::key);
                auto plan = context_->indexes->planInitialTreeMutation(snapshot_, id, entries);
                scalar_index_mutations_.erase(position);
                auto initial = std::lower_bound(initial_scalar_index_mutations_.begin(), initial_scalar_index_mutations_.end(), id,
                    [](const auto &staged, uint64_t key) { return staged.index_id < key; });
                initial_scalar_index_mutations_.insert(initial, {context_->indexes, id, std::move(plan)});
            }
        };
        stageFreeSummaries(EntityPageRecordKind::EPRK_NODE);
        stageFreeSummaries(EntityPageRecordKind::EPRK_EDGE);

		if (contentIndexPlan.has_value()) {
			prepareIndexMutationPlans(&*contentIndexPlan, [this, &contentIndexPlan, &contentValuesToPublish,
													contentIndexStore] {
				*contentIndexPlan = contentIndexStore->planNativeGraphContentIndexMutation(
							snapshot_, contentValuesToPublish, context_->options.first_slot, context_->options.last_slot, true);
			});
		} else {
			prepareIndexMutationPlans();
		}
		for (const StagedScalarIndexMutation &staged: scalar_index_mutations_) {
            if (staged.images) {
                if (staged.plan || staged.retirement_plan)
                    fail(FormatError::FERR_INVALID_STATE, "ordinary membership images overlap a scalar plan");
                continue;
            }
			if (staged.index == nullptr || staged.index_id == 0U ||
				!staged.index->usesMetadataStore(*context_->metadata)) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native graph staged scalar Index operation stream has no valid finalized overlay plan");
			}
			if (staged.retirement_plan.has_value()) {
				const BTreeTreeRetirementPlan &plan = *staged.retirement_plan;
				if (staged.plan.has_value() || !staged.operations.empty() || !plan.preparer || // retirement planning retains one pinned root and does not coexist with a structural plan.
					plan.expected_root.index_id != staged.index_id || plan.pool_requests.empty()) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native graph staged scalar Index retirement lost its pinned root or frame preparer");
				}
				NativeGraphIndexTreeMutation mutation;
				mutation.index_id = staged.index_id;
				mutation.expected_root = plan.expected_root;
				mutation.pool_requests = plan.pool_requests;
				mutation.retire_root = true;
				pendingDerivedBindings.push_back({std::move(mutation), plan.pool_requests.size(), plan.preparer});
				continue;
			}
			if (!staged.plan.has_value()) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native graph staged scalar Index operation stream has no finalized overlay plan");
			}
			const BTreeStructuralMutationPlan &plan = *staged.plan;
			if (plan.expected_root.index_id != staged.index_id || !plan.preparer) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native graph staged scalar Index plan lost its root identity or frame preparer");
			}
			NativeGraphIndexTreeMutation mutation;
			mutation.index_id = staged.index_id;
			mutation.expected_root = plan.expected_root;
			mutation.pool_requests = plan.pool_requests;
			if (plan.new_pool_mutation.has_value()) {
				mutation.new_pool_requests.push_back({context_->options.first_slot, context_->options.last_slot,
													  plan.new_pool_mutation->owner_id,
													  plan.new_pool_mutation->appended_page_count});
			}
			for (const BTreeNewIndexTreePoolMutation &fresh: plan.additional_new_pool_mutations) {
				mutation.new_pool_requests.push_back({context_->options.first_slot, context_->options.last_slot,
													  fresh.owner_id, fresh.appended_page_count});
			}
			if (plan.replacement_root.has_value()) {
				const IndexTreeMutationAppendedPageReference reference = *plan.replacement_root;
				const bool newlyReserved = reference.pool_ordinal >= plan.pool_requests.size();
				const size_t poolOrdinal =
						newlyReserved ? reference.pool_ordinal - plan.pool_requests.size() : reference.pool_ordinal;
				const size_t poolCount =
						newlyReserved ? mutation.new_pool_requests.size() : mutation.pool_requests.size();
				if (poolOrdinal >= poolCount) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native graph staged scalar Index root references a missing appended page pool");
				}
				mutation.replacement_root = {poolOrdinal, reference.appended_page_ordinal, newlyReserved};
			}
			mutation.stable_replacement_root = plan.stable_replacement_root;
			const size_t freshPoolCount =
					static_cast<size_t>(plan.new_pool_mutation.has_value()) + plan.additional_new_pool_mutations.size();
			pendingDerivedBindings.push_back(
					{std::move(mutation), plan.pool_requests.size() + freshPoolCount, plan.preparer});
		}
		for (const StagedInitialScalarIndexMutation &staged: initial_scalar_index_mutations_) {
			if (staged.index == nullptr || staged.index_id == 0U || !staged.plan.has_value() || // finalized initial index plans retain their validated local index and preflight output.
				!staged.index->usesMetadataStore(*context_->metadata)) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native graph staged initial scalar Index has no valid finalized tree plan");
			}
			const BTreeInitialTreeMutationPlan &plan = *staged.plan;
			if (plan.index_id != staged.index_id || plan.new_pool_mutations.empty() || !plan.preparer || // preflight builds the root location from the same nonempty pool mutation set.
				plan.root_page.pool_ordinal >= plan.new_pool_mutations.size() ||
				plan.root_page.appended_page_ordinal >=
						plan.new_pool_mutations[plan.root_page.pool_ordinal].appended_page_count) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native graph staged initial scalar Index plan lost its root or page-preparation identity");
			}
			NativeGraphIndexTreeMutation mutation;
			mutation.index_id = plan.index_id;
			for (const BTreeNewIndexTreePoolMutation &pool: plan.new_pool_mutations) {
				mutation.new_pool_requests.push_back({context_->options.first_slot, context_->options.last_slot,
													  pool.owner_id, pool.appended_page_count});
			}
			mutation.replacement_root = {plan.root_page.pool_ordinal, plan.root_page.appended_page_ordinal, true};
			pendingDerivedBindings.push_back({std::move(mutation), plan.new_pool_mutations.size(), plan.preparer});
		}
		if (global_adjacency_plan_.has_value()) {
			const BTreeStructuralMutationPlan &plan = *global_adjacency_plan_;
			if (plan.expected_root.index_id != RSS_GLOBAL_ADJACENCY_INDEX_ID || !plan.preparer) { // canonical adjacency planning pins its own root and preparer.
				fail(FormatError::FERR_INVALID_STATE,
					 "native graph global adjacency overlay lost its pinned root or frame preparer");
			}
			NativeGraphIndexTreeMutation mutation;
			mutation.index_id = RSS_GLOBAL_ADJACENCY_INDEX_ID;
			mutation.expected_root = plan.expected_root;
			mutation.pool_requests = plan.pool_requests;
			if (plan.new_pool_mutation.has_value()) {
				mutation.new_pool_requests.push_back({context_->options.first_slot, context_->options.last_slot,
													  plan.new_pool_mutation->owner_id,
													  plan.new_pool_mutation->appended_page_count});
			}
			for (const BTreeNewIndexTreePoolMutation &fresh: plan.additional_new_pool_mutations) {
				mutation.new_pool_requests.push_back({context_->options.first_slot, context_->options.last_slot,
													  fresh.owner_id, fresh.appended_page_count});
			}
			if (plan.replacement_root.has_value()) {
				const IndexTreeMutationAppendedPageReference reference = *plan.replacement_root;
				const bool newlyReserved = reference.pool_ordinal >= plan.pool_requests.size();
				const size_t poolOrdinal =
						newlyReserved ? reference.pool_ordinal - plan.pool_requests.size() : reference.pool_ordinal;
				const size_t poolCount =
						newlyReserved ? mutation.new_pool_requests.size() : mutation.pool_requests.size();
				if (poolOrdinal >= poolCount) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native graph global adjacency root references a missing appended page pool");
				}
				mutation.replacement_root = {poolOrdinal, reference.appended_page_ordinal, newlyReserved};
			}
			mutation.stable_replacement_root = plan.stable_replacement_root;
			const size_t freshPoolCount =
					static_cast<size_t>(plan.new_pool_mutation.has_value()) + plan.additional_new_pool_mutations.size();
			pendingDerivedBindings.push_back(
					{std::move(mutation), plan.pool_requests.size() + freshPoolCount, plan.preparer});
		}
		if (global_adjacency_retirement_plan_.has_value()) {
			const BTreeTreeRetirementPlan &plan = *global_adjacency_retirement_plan_;
			if (plan.expected_root.index_id != RSS_GLOBAL_ADJACENCY_INDEX_ID || !plan.preparer) { // canonical adjacency retirement pins its own root and preparer.
				fail(FormatError::FERR_INVALID_STATE,
					 "native graph global adjacency retirement lost its pinned root or frame preparer");
			}
			NativeGraphIndexTreeMutation mutation;
			mutation.index_id = RSS_GLOBAL_ADJACENCY_INDEX_ID;
			mutation.expected_root = plan.expected_root;
			mutation.pool_requests = plan.pool_requests;
			mutation.retire_root = true;
			pendingDerivedBindings.push_back({std::move(mutation), plan.pool_requests.size(), plan.preparer});
		}
		if (global_adjacency_initial_plan_.has_value()) {
			const BTreeInitialTreeMutationPlan &plan = *global_adjacency_initial_plan_;
			if (plan.index_id != RSS_GLOBAL_ADJACENCY_INDEX_ID || plan.new_pool_mutations.empty() || !plan.preparer || // canonical adjacency initial planning derives its root from one nonempty prepared pool set.
				plan.root_page.pool_ordinal >= plan.new_pool_mutations.size() ||
				plan.root_page.appended_page_ordinal >=
						plan.new_pool_mutations[plan.root_page.pool_ordinal].appended_page_count) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native graph initial global adjacency tree lost its root or page-preparation identity");
			}
			NativeGraphIndexTreeMutation mutation;
			mutation.index_id = RSS_GLOBAL_ADJACENCY_INDEX_ID;
			for (const BTreeNewIndexTreePoolMutation &pool: plan.new_pool_mutations) {
				mutation.new_pool_requests.push_back({context_->options.first_slot, context_->options.last_slot,
													  pool.owner_id, pool.appended_page_count});
			}
			mutation.replacement_root = {plan.root_page.pool_ordinal, plan.root_page.appended_page_ordinal, true};
			pendingDerivedBindings.push_back({std::move(mutation), plan.new_pool_mutations.size(), plan.preparer});
		}
		if (contentIndexPlan.has_value()) {
			const size_t poolCount = contentIndexPlan->mutation.pool_requests.size() +
					contentIndexPlan->mutation.new_pool_requests.size();
			pendingDerivedBindings.push_back(
					{std::move(contentIndexPlan->mutation), poolCount, std::move(contentIndexPlan->preparer)});
		}
		std::ranges::sort(pendingDerivedBindings, {},
						  [](const PendingDerivedIndexBinding &binding) { return binding.mutation.index_id; });
		if (std::ranges::adjacent_find(pendingDerivedBindings, {}, [](const PendingDerivedIndexBinding &binding) {
				return binding.mutation.index_id;
			}) != pendingDerivedBindings.end()) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native graph transaction has overlapping existing and initial scalar Index mutations");
		}
		NativeGraphDerivedStateMutation derivedState;
        derivedState.entity_free_summary_fences = std::move(freeSummaryFences);
        if (global_adjacency_images_) derivedState.index_images.push_back(global_adjacency_images_);
        for (const auto &staged : scalar_index_mutations_) if (staged.images) derivedState.index_images.push_back(staged.images);
		derivedState.index_tree_mutations.reserve(pendingDerivedBindings.size());
		derivedState.private_segment_plan = std::move(deferredAdjacencyPrivateSegmentPlan);
		for (const EntityPropertyRootMutationPlan &propertyPlan: propertyPlans) {
			derivedState.reused_large_values.insert(derivedState.reused_large_values.end(),
													propertyPlan.reused_large_values.begin(),
													propertyPlan.reused_large_values.end());
		}
		std::ranges::sort(derivedState.reused_large_values, {}, &LargeValueRef::value_id);
		derivedState.reused_large_values.erase(std::unique(derivedState.reused_large_values.begin(),
														   derivedState.reused_large_values.end(),
														   [](const LargeValueRef &left, const LargeValueRef &right) {
															   return left.value_id == right.value_id && left == right;
														   }),
											   derivedState.reused_large_values.end());
		for (size_t ordinal = 1U; ordinal < derivedState.reused_large_values.size(); ++ordinal) {
			if (derivedState.reused_large_values[ordinal - 1U].value_id ==
				derivedState.reused_large_values[ordinal].value_id) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native graph Property plans reused one LargeValue ID with conflicting references");
			}
		}
		struct DerivedIndexBinding {
			size_t expected_pool_count = 0U;
			BTreeStructuralMutationPreparer preparer;
		};
		std::vector<DerivedIndexBinding> derivedBindings;
		derivedBindings.reserve(pendingDerivedBindings.size());
		for (PendingDerivedIndexBinding &binding: pendingDerivedBindings) {
			derivedState.index_tree_mutations.push_back(std::move(binding.mutation));
			derivedBindings.push_back({binding.expected_pool_count, std::move(binding.preparer)});
		}
		NativeGraphDerivedStatePreparer derivedStatePreparer;
		if (!derivedBindings.empty() || !derivedState.index_images.empty()) {
			derivedStatePreparer = [bindings = std::move(derivedBindings),
									checkpointLsn](std::vector<std::vector<IndexTreeMutationPoolBatch>> &batches) {
				if (batches.size() != bindings.size()) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native graph scalar Index publication changed its immutable tree cardinality");
				}
				for (size_t ordinal = 0U; ordinal < bindings.size(); ++ordinal) {
					const DerivedIndexBinding &binding = bindings[ordinal];
					if (!binding.preparer || batches[ordinal].size() != binding.expected_pool_count) { // each private derived-index binding retains its preparer and exact pool count from the same immutable planning pass.
						fail(FormatError::FERR_INVALID_STATE,
							 "native graph scalar Index publication changed its immutable pool plan");
					}
					binding.preparer(batches[ordinal], checkpointLsn);
				}
			};
		} else if (!derivedState.empty()) {
			derivedStatePreparer = [](std::vector<std::vector<IndexTreeMutationPoolBatch>> &batches) {
					if (!batches.empty()) { // a reused LargeValue-only derived state has no scalar Index binding, so the paired coordinator callback receives no Index batches.
					fail(FormatError::FERR_INVALID_STATE,
						 "native graph reused LargeValue publication unexpectedly prepared Index pages");
				}
			};
		}

		std::vector<StagedVectorBootstrap> vectorBootstrapBindings;
		vectorBootstrapBindings.reserve(vector_bootstraps_.size());
		for (StagedVectorBootstrap &staged: vector_bootstraps_) {
			const NativeGraphVectorBootstrapMutation &mutation = staged.mutation;
			const uint8_t invalid = // vector bootstrap captures three stores from one validated planner context.
					static_cast<uint8_t>(staged.mapping_trees == nullptr || staged.raw_pages == nullptr ||
										 staged.manifests == nullptr) |
					static_cast<uint8_t>(!staged.mapping_trees->usesMetadataStore(*context_->metadata)) |
					static_cast<uint8_t>(!staged.raw_pages->usesMetadataStore(*context_->metadata)) |
					static_cast<uint8_t>(!staged.manifests->usesMetadataStore(*context_->metadata)) |
					static_cast<uint8_t>(mutation.index_id == 0U || mutation.vector_node_mapping_index_id == 0U ||
									 mutation.vector_count == 0U || mutation.first_vector_id == 0U ||
									 mutation.dimensions == 0U || staged.records.size() != mutation.vector_count ||
									 staged.maximum_pages == 0U);
			if (invalid != 0U) { // vector bootstrap staging validates these captured stores and canonical records before commit constructs this private binding.
				fail(FormatError::FERR_INVALID_STATE,
					 "native graph staged Vector bootstrap lost its typed stores or immutable record plan");
			}
			for (size_t ordinal = 0U; ordinal < staged.records.size(); ++ordinal) {
				const StagedVectorBootstrapRecord &record = staged.records[ordinal];
				if (record.node_id == 0U || record.vector_id != mutation.first_vector_id + ordinal || // staging emits this immutable record sequence after validating the public bootstrap request.
					record.raw_record.size() !=
							sizeof(uint64_t) + static_cast<size_t>(mutation.dimensions) * sizeof(uint16_t) ||
					LittleEndianCodec::readU64(record.raw_record, 0U) != record.vector_id ||
					(ordinal != 0U && record.node_id <= staged.records[ordinal - 1U].node_id)) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native graph staged Vector bootstrap lost canonical Node or Vector identities");
				}
			}
			vectorBootstrapBindings.push_back(std::move(staged));
		}
		std::ranges::sort(vectorBootstrapBindings, {},
						  [](const StagedVectorBootstrap &staged) { return staged.mutation.index_id; });
		if (std::ranges::adjacent_find(vectorBootstrapBindings, {}, [](const StagedVectorBootstrap &staged) { // public staging rejects a second bootstrap for the same Index before this private sort.
				return staged.mutation.index_id;
			}) != vectorBootstrapBindings.end()) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native graph transaction staged duplicate Vector bootstrap Index IDs");
		}
		derivedState.vector_bootstraps.reserve(vectorBootstrapBindings.size());
		for (const StagedVectorBootstrap &staged: vectorBootstrapBindings) {
			derivedState.vector_bootstraps.push_back(staged.mutation);
		}

		NativeGraphVectorBootstrapPreparer vectorBootstrapPreparer;
		if (!vectorBootstrapBindings.empty()) {
			vectorBootstrapPreparer = [bindings = std::move(vectorBootstrapBindings),
									   checkpointLsn](std::vector<NativeGraphVectorBootstrapMaterialization>
															  &materializations) {
				if (materializations.size() != bindings.size()) { // the metadata coordinator allocates one bootstrap materialization for every submitted derived-state binding.
					fail(FormatError::FERR_INVALID_STATE,
						 "native graph Vector bootstrap publication changed its immutable Index cardinality");
				}
				for (size_t ordinal = 0U; ordinal < bindings.size(); ++ordinal) {
					const StagedVectorBootstrap &staged = bindings[ordinal];
					NativeGraphVectorBootstrapMaterialization &materialization = materializations[ordinal];
					const NativeGraphVectorBootstrapMutation &mutation = staged.mutation;
					const uint32_t recordBytes = static_cast<uint32_t>(
							sizeof(uint64_t) + static_cast<size_t>(mutation.dimensions) * sizeof(uint16_t));
					const uint16_t capacity = VectorPageStore::rawSlotCapacity(recordBytes);
					const uint8_t invalid =
							static_cast<uint8_t>(staged.raw_pages == nullptr || staged.manifests == nullptr) |
							static_cast<uint8_t>(capacity == 0U) |
							static_cast<uint8_t>(materialization.mutation.index_id != mutation.index_id) |
							static_cast<uint8_t>(materialization.mutation.vector_node_mapping_index_id !=
												 mutation.vector_node_mapping_index_id) |
							static_cast<uint8_t>(materialization.mutation.first_vector_id != mutation.first_vector_id) |
							static_cast<uint8_t>(materialization.mutation.vector_count != mutation.vector_count) |
							static_cast<uint8_t>(materialization.mutation.dimensions != mutation.dimensions) |
							static_cast<uint8_t>(materialization.mutation.metric != mutation.metric) |
							static_cast<uint8_t>(materialization.node_mapping_root.index_id != mutation.index_id) |
							static_cast<uint8_t>(materialization.vector_node_mapping_root.index_id !=
												 mutation.vector_node_mapping_index_id) |
							static_cast<uint8_t>(!materialization.node_mapping_root.root_page_id.isValid()) |
							static_cast<uint8_t>(!materialization.vector_node_mapping_root.root_page_id.isValid()) |
							static_cast<uint8_t>(materialization.raw_pages.size() != mutation.raw_page_count) |
							static_cast<uint8_t>(materialization.raw_pools.empty()) |
							static_cast<uint8_t>(staged.records.size() != mutation.vector_count);
					if (invalid != 0U) { // the coordinator returns the same validated bootstrap mutation and mapping roots it received from this binding.
						fail(FormatError::FERR_INVALID_STATE,
							 "native graph Vector bootstrap materialization changed its metadata-owned identity");
					}
					const size_t expectedRawPoolCount =
							(materialization.raw_pages.size() - 1U) / RSS_PAGE_POOL_DATA_FRAME_COUNT + 1U;
					if (materialization.raw_pools.size() != expectedRawPoolCount) { // the authenticated bootstrap allocator derives Raw-pool cardinality from its immutable page count.
						fail(FormatError::FERR_INVALID_STATE,
							 "native graph Vector bootstrap has a noncanonical Raw-pool cardinality");
					}

					std::vector<VectorPage> rawPages;
					rawPages.reserve(materialization.raw_pages.size());
					materialization.vector_locations.clear();
					materialization.vector_locations.reserve(staged.records.size());
					size_t firstRecord = 0U;
					for (size_t pageOrdinal = 0U; pageOrdinal < materialization.raw_pages.size(); ++pageOrdinal) {
						PageLocation &location = materialization.raw_pages[pageOrdinal];
						const CatalogEntry &pool =
								materialization.raw_pools[pageOrdinal / RSS_PAGE_POOL_DATA_FRAME_COUNT];
						if (location.segment_id != pool.segment_id || location.generation != pool.generation || // materialization locations are allocated from their ordinal-selected Raw pool in the same coordinator pass.
							location.page_index != 0U || location.free_bytes != 0U) {
							fail(FormatError::FERR_INVALID_STATE,
								 "native graph Vector bootstrap assigned a Raw page to the wrong pool");
						}
						VectorPageHeader header;
						header.index_id = mutation.index_id;
						header.page_id = location.page_id;
						header.generation = pool.generation;
						header.kind = VectorStorageKind::VSK_RAW;
						header.record_bytes = recordBytes;
						header.slot_capacity = capacity;
						header.bitmap_bytes = static_cast<uint16_t>((static_cast<uint32_t>(capacity) + 7U) / 8U);
						header.committed_lsn = checkpointLsn;
						const PageOwnerHeader owner{header.page_id,
													mutation.index_id,
													PageOwnerKind::POK_VECTOR_RAW,
													0U,
													RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
													header.generation,
													checkpointLsn};
						VectorPage page = VectorPage::create(owner, header);
						const size_t pageEnd =
								std::min(firstRecord + static_cast<size_t>(capacity), staged.records.size());
						for (size_t recordOrdinal = firstRecord; recordOrdinal < pageEnd; ++recordOrdinal) {
							const StagedVectorBootstrapRecord &record = staged.records[recordOrdinal];
							if (record.vector_id != mutation.first_vector_id + recordOrdinal || // staged records are immutable and their ordinal/vector identity was authenticated before private preparation.
								record.raw_record.size() != recordBytes ||
								LittleEndianCodec::readU64(record.raw_record, 0U) != record.vector_id) {
								fail(FormatError::FERR_INVALID_STATE,
									 "native graph Vector bootstrap changed a canonical Raw record");
							}
							const uint16_t slot = page.insert(record.raw_record);
							materialization.vector_locations.push_back(
									{record.vector_id, location.page_id, slot, page.slotGeneration(slot)});
						}
						firstRecord = pageEnd;
						rawPages.push_back(std::move(page));
					}
					if (firstRecord != staged.records.size()) { // page iteration consumes the exact staged-record count using the same positive Raw capacity used by preflight.
						fail(FormatError::FERR_INVALID_STATE,
							 "native graph Vector bootstrap Raw-page plan does not cover every Vector record");
					}
					for (size_t poolOrdinal = 0U; poolOrdinal < materialization.raw_pools.size(); ++poolOrdinal) {
						const size_t firstPage = poolOrdinal * RSS_PAGE_POOL_DATA_FRAME_COUNT;
						const size_t pageCount = std::min(static_cast<size_t>(RSS_PAGE_POOL_DATA_FRAME_COUNT),
														  rawPages.size() - firstPage);
						staged.raw_pages->prepareRawNewPoolPages(
								materialization.raw_pools[poolOrdinal],
								std::span<const VectorPage>(rawPages).subspan(firstPage, pageCount),
								std::span<PageLocation>(materialization.raw_pages).subspan(firstPage, pageCount),
								checkpointLsn);
					}

					VectorManifest manifest;
					manifest.index_id = mutation.index_id;
					manifest.page_id = materialization.manifest_page.page_id;
					manifest.generation = materialization.manifest_pool.generation;
					manifest.committed_lsn = checkpointLsn;
					manifest.dimensions = mutation.dimensions;
					manifest.metric = mutation.metric;
					manifest.node_mapping_root_page_id = materialization.node_mapping_root.root_page_id;
					manifest.node_mapping_root_generation = materialization.node_mapping_root.root_generation;
					manifest.vector_node_mapping_index_id = mutation.vector_node_mapping_index_id;
					manifest.vector_node_mapping_root_page_id = materialization.vector_node_mapping_root.root_page_id;
					manifest.vector_node_mapping_root_generation =
							materialization.vector_node_mapping_root.root_generation;
					manifest.raw_write_tail_page_id = materialization.raw_pages.back().page_id;
					manifest.entries.reserve(rawPages.size());
					for (const VectorPage &page: rawPages) {
						manifest.entries.push_back(
								{page.header().page_id, VectorStorageKind::VSK_RAW, page.header().live_count});
					}
					const PageOwnerHeader manifestOwner{manifest.page_id,
														mutation.index_id,
														PageOwnerKind::POK_VECTOR_MANIFEST,
														0U,
														RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
														manifest.generation,
														checkpointLsn};
					staged.manifests->prepareNewPoolInitial(materialization.manifest_pool,
															VectorManifestPage::create(manifestOwner, manifest),
															checkpointLsn, materialization.manifest_page);
					materialization.manifest_location = {mutation.index_id, manifest.page_id, manifest.generation};
				}
			};
		}

		std::vector<StagedVectorDelta> vectorDeltaBindings;
		vectorDeltaBindings.reserve(vector_deltas_.size());
		for (StagedVectorDelta &staged: vector_deltas_) {
			const NativeGraphVectorDeltaMutation &mutation = staged.mutation;
			const uint8_t invalid = // vector delta captures three stores from one validated planner context.
					static_cast<uint8_t>(staged.mapping_trees == nullptr || staged.raw_pages == nullptr ||
										 staged.manifests == nullptr) |
					static_cast<uint8_t>(!staged.mapping_trees->usesMetadataStore(*context_->metadata)) |
					static_cast<uint8_t>(!staged.raw_pages->usesMetadataStore(*context_->metadata)) |
					static_cast<uint8_t>(!staged.manifests->usesMetadataStore(*context_->metadata)) |
					static_cast<uint8_t>(mutation.index_id == 0U || mutation.vector_node_mapping_index_id == 0U ||
										 mutation.dimensions == 0U || staged.maximum_raw_pages == 0U);
			if (invalid != 0U) { // vector delta staging validates its captured stores and source fence before commit constructs this private binding.
				fail(FormatError::FERR_INVALID_STATE,
					 "native graph staged Vector delta lost its typed stores or immutable source fence");
			}
			vectorDeltaBindings.push_back(std::move(staged));
		}
		std::ranges::sort(vectorDeltaBindings, {},
						  [](const StagedVectorDelta &staged) { return staged.mutation.index_id; });
		if (std::ranges::adjacent_find(vectorDeltaBindings, {}, [](const StagedVectorDelta &staged) { // public staging rejects a second delta for one Index before this private sort.
				return staged.mutation.index_id;
			}) != vectorDeltaBindings.end()) {
			fail(FormatError::FERR_INVALID_STATE, "native graph transaction staged duplicate Vector delta Index IDs");
		}
		derivedState.vector_deltas.reserve(vectorDeltaBindings.size());
		for (const StagedVectorDelta &staged: vectorDeltaBindings) {
			derivedState.vector_deltas.push_back(staged.mutation);
		}

		NativeGraphVectorDeltaPreparer vectorDeltaPreparer;
		if (!vectorDeltaBindings.empty()) {
			auto bindings = std::make_shared<std::vector<StagedVectorDelta>>(std::move(vectorDeltaBindings));
			vectorDeltaPreparer = [bindings = std::move(bindings), checkpointLsn](
										  std::vector<NativeGraphVectorDeltaMaterialization> &materializations) {
				if (materializations.size() != bindings->size()) { // the metadata coordinator allocates one delta materialization for every submitted derived-state binding.
					fail(FormatError::FERR_INVALID_STATE,
						 "native graph Vector delta publication changed its immutable Index cardinality");
				}
				for (size_t ordinal = 0U; ordinal < bindings->size(); ++ordinal) {
					const StagedVectorDelta &staged = (*bindings)[ordinal];
					NativeGraphVectorDeltaMaterialization &materialization = materializations[ordinal];
					const NativeGraphVectorDeltaMutation &mutation = staged.mutation;
					const uint32_t recordBytes = static_cast<uint32_t>(
							sizeof(uint64_t) + static_cast<size_t>(mutation.dimensions) * sizeof(uint16_t));
					const uint16_t capacity = VectorPageStore::rawSlotCapacity(recordBytes);
					if (capacity == 0U || materialization.mutation.index_id != mutation.index_id || // preflight fixes the Raw capacity and the coordinator returns the same authenticated mutation and mapping roots.
						materialization.mutation.raw_pages.size() != staged.raw_page_images.size() ||
						materialization.node_mapping_root.index_id != mutation.index_id ||
						materialization.vector_node_mapping_root.index_id != mutation.vector_node_mapping_index_id ||
						(materialization.delta_mapping_root.index_id != mutation.delta_mapping_index_id) ||
						(mutation.delta_mapping_index_id != 0U &&
						 (!materialization.delta_mapping_root.root_page_id.isValid() ||
						  !materialization.delta_mapping_root.root_generation.isValid()))) {
						fail(FormatError::FERR_INVALID_STATE,
							 "native graph Vector delta materialization changed its source or mapping identity");
					}
					materialization.vector_locations.clear();
					materialization.vector_locations.reserve(static_cast<size_t>(mutation.inserted_vector_count));
					VectorManifest manifest = staged.source_manifest;
					manifest.page_id = materialization.manifest_replacement.page_id;
					manifest.generation = materialization.manifest_replacement.generation;
					manifest.committed_lsn = checkpointLsn;
					manifest.node_mapping_root_page_id = materialization.node_mapping_root.root_page_id;
					manifest.node_mapping_root_generation = materialization.node_mapping_root.root_generation;
					manifest.vector_node_mapping_root_page_id = materialization.vector_node_mapping_root.root_page_id;
					manifest.vector_node_mapping_root_generation =
							materialization.vector_node_mapping_root.root_generation;
					manifest.delta_mapping_index_id = mutation.delta_mapping_index_id;
					manifest.delta_mapping_root_page_id = materialization.delta_mapping_root.root_page_id;
					manifest.delta_mapping_root_generation = materialization.delta_mapping_root.root_generation;
					manifest.delta_entry_count = mutation.delta_entry_count;
					std::vector<std::vector<VectorPage>> freshPages(materialization.raw_new_pools.size());
					std::vector<std::vector<size_t>> freshPageOrdinals(materialization.raw_new_pools.size());
					for (size_t rawOrdinal = 0U; rawOrdinal < staged.raw_page_images.size(); ++rawOrdinal) {
						const StagedVectorDeltaRawPage &stagedRaw = staged.raw_page_images[rawOrdinal];
						NativeGraphVectorDeltaRawPageMaterialization &raw = materialization.raw_pages[rawOrdinal];
						if (raw.mutation.placement != stagedRaw.mutation.placement || // the coordinator replays the exact authenticated Raw-page plan retained by this delta binding.
							raw.mutation.inserted_record_count != stagedRaw.mutation.inserted_record_count ||
							raw.mutation.page_ordinal != stagedRaw.mutation.page_ordinal) {
							fail(FormatError::FERR_INVALID_STATE,
								 "native graph Vector delta Raw-page identity changed during materialization");
						}
						const bool cow = raw.mutation.placement == NativeGraphVectorDeltaRawPlacement::NGVDRP_COW_TAIL;
						VectorPage page = [&]() {
							if (cow) {
								if (!stagedRaw.tail_image.has_value()) { // COW Raw transitions retain their source tail image during delta preflight.
									fail(FormatError::FERR_INVALID_STATE,
										 "native graph Vector delta COW tail lost its retained page image");
								}
								const PageOwnerHeader owner{raw.location.page_id,
															mutation.index_id,
															PageOwnerKind::POK_VECTOR_RAW,
															0U,
															RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
															raw.location.generation,
															checkpointLsn};
								return stagedRaw.tail_image->rebindOwner(owner);
							}
							VectorPageHeader header;
							header.index_id = mutation.index_id;
							header.page_id = raw.location.page_id;
							header.generation = raw.pool.generation;
							header.kind = VectorStorageKind::VSK_RAW;
							header.record_bytes = recordBytes;
							header.slot_capacity = capacity;
							header.bitmap_bytes = static_cast<uint16_t>((static_cast<uint32_t>(capacity) + 7U) / 8U);
							header.committed_lsn = checkpointLsn;
							const PageOwnerHeader owner{header.page_id,
														mutation.index_id,
														PageOwnerKind::POK_VECTOR_RAW,
														0U,
														RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
														header.generation,
														checkpointLsn};
							VectorPage created = VectorPage::create(owner, header);
							for (const size_t recordOrdinal: stagedRaw.record_ordinals) {
									if (recordOrdinal >= staged.records.size() || // preflight emits Raw record ordinals from the immutable staged insertion sequence.
									staged.records[recordOrdinal].kind != NativeVectorMutationKind::NVMK_INSERT ||
									staged.records[recordOrdinal].raw_record.size() != recordBytes ||
									LittleEndianCodec::readU64(staged.records[recordOrdinal].raw_record, 0U) !=
											staged.records[recordOrdinal].vector_id) {
									fail(FormatError::FERR_INVALID_STATE,
										 "native graph Vector delta lost a canonical inserted Raw record");
								}
								static_cast<void>(created.insert(staged.records[recordOrdinal].raw_record));
							}
							return created;
						}();
						for (const size_t recordOrdinal: stagedRaw.record_ordinals) {
							const StagedVectorDeltaRecord &record = staged.records[recordOrdinal];
							const std::optional<uint16_t> slot = page.findVectorId(record.vector_id);
							if (!slot.has_value()) { // each validated inserted Raw record is inserted into this fresh page before its location is captured.
								fail(FormatError::FERR_INVALID_STATE,
									 "native graph Vector delta Raw page lost an inserted Vector ID");
							}
							materialization.vector_locations.push_back(
									{record.vector_id, raw.location.page_id, *slot, page.slotGeneration(*slot)});
						}
						const VectorManifestEntry updatedEntry{raw.location.page_id, VectorStorageKind::VSK_RAW,
															   static_cast<uint32_t>(page.header().live_count)};
						auto entry = std::lower_bound(
								manifest.entries.begin(), manifest.entries.end(), updatedEntry,
								[](const VectorManifestEntry &left, const VectorManifestEntry &right) {
									return std::tuple<uint16_t, uint64_t>{static_cast<uint16_t>(left.storage_kind),
																		  left.page_id.value()} <
										   std::tuple<uint16_t, uint64_t>{static_cast<uint16_t>(right.storage_kind),
																		  right.page_id.value()};
								});
						const bool matchingEntry = entry != manifest.entries.end() &&
												   entry->page_id == updatedEntry.page_id &&
												   entry->storage_kind == updatedEntry.storage_kind;
						if (cow) {
							if (!matchingEntry) { // a COW transition originates from a Manifest entry authenticated by the source snapshot.
								fail(FormatError::FERR_INVALID_STATE,
									 "native graph Vector delta COW tail is absent from its source Manifest");
							}
							*entry = updatedEntry;
						} else if (matchingEntry) { // a non-COW Raw page receives a newly allocated page identity absent from the source Manifest.
							fail(FormatError::FERR_INVALID_STATE, "native graph Vector delta assigned a new Raw page "
																  "that already exists in its Manifest");
						} else {
							manifest.entries.insert(entry, updatedEntry);
						}
						if (cow) {
							staged.raw_pages->prepareRawReplacement(raw.pool, raw.mutation.expected_location,
																	raw.mutation.expected_owner_lsn, page,
																	checkpointLsn, raw.location);
						} else if (raw.mutation.placement ==
								   NativeGraphVectorDeltaRawPlacement::NGVDRP_APPEND_TO_CURRENT_POOL) {
							staged.raw_pages->prepareRawAppend(raw.pool, page, checkpointLsn, raw.location);
						} else {
							const size_t poolOrdinal = static_cast<size_t>(raw.mutation.new_pool_ordinal - 1U);
								if (poolOrdinal >= freshPages.size() || // new-pool ordinals and pool identities are allocated together by the authenticated delta materialization.
								raw.pool.segment_id != materialization.raw_new_pools[poolOrdinal].segment_id ||
								raw.pool.physical_slot != materialization.raw_new_pools[poolOrdinal].physical_slot ||
								raw.pool.generation != materialization.raw_new_pools[poolOrdinal].generation) {
								fail(FormatError::FERR_INVALID_STATE,
									 "native graph Vector delta fresh Raw page lost its assigned pool identity");
							}
							freshPages[poolOrdinal].push_back(std::move(page));
							freshPageOrdinals[poolOrdinal].push_back(rawOrdinal);
						}
					}
					for (size_t poolOrdinal = 0U; poolOrdinal < freshPages.size(); ++poolOrdinal) {
						if (freshPages[poolOrdinal].empty() || // the delta materializer emits one nonempty bounded Raw-page group for every allocated fresh pool.
							freshPages[poolOrdinal].size() > RSS_PAGE_POOL_DATA_FRAME_COUNT) {
							fail(FormatError::FERR_INVALID_STATE,
								 "native graph Vector delta has an invalid fresh Raw-pool page group");
						}
						std::vector<PageLocation> locations;
						for (const size_t rawOrdinal: freshPageOrdinals[poolOrdinal]) {
							locations.push_back(materialization.raw_pages[rawOrdinal].location);
						}
						staged.raw_pages->prepareRawNewPoolPages(materialization.raw_new_pools[poolOrdinal],
																 freshPages[poolOrdinal], locations, checkpointLsn);
						for (size_t index = 0U; index < locations.size(); ++index) {
							materialization.raw_pages[freshPageOrdinals[poolOrdinal][index]].location =
									locations[index];
						}
					}
					if (!materialization.raw_pages.empty()) {
						manifest.raw_write_tail_page_id = materialization.raw_pages.back().location.page_id;
					}
					const PageOwnerHeader owner{manifest.page_id,
												mutation.index_id,
												PageOwnerKind::POK_VECTOR_MANIFEST,
												0U,
												RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
												manifest.generation,
												checkpointLsn};
					const VectorManifestPage page = VectorManifestPage::create(owner, manifest);
					if (mutation.manifest_in_new_pool) {
						staged.manifests->prepareNewPoolReplacement(materialization.manifest_new_pool, page,
																	checkpointLsn,
																	materialization.manifest_replacement);
					} else {
						staged.manifests->prepareReplacement(mutation.expected_manifest_pool,
															 mutation.expected_manifest_location,
															 mutation.expected_manifest_owner_lsn, page, checkpointLsn,
															 materialization.manifest_replacement);
					}
					materialization.manifest_location = {mutation.index_id, manifest.page_id, manifest.generation};
				}
			};
		}

		const auto refreshInsertionTargetCache = [&](const EntityPageBatchResult &published) noexcept {
			try {
				MetadataSnapshotPin nextSnapshot = context_->metadata->openSnapshot();
				std::map<std::pair<uint64_t, uint16_t>, NativeGraphWritePlannerContext::InsertionTarget> targets;
				// Preserve untouched targets for update/delete-only transactions and for
				// COW publications that changed a Directory root. PageIds are logical and
				// never reused; the next insertion lazily re-authenticates each target.
				if (priorInsertionCacheValid) {
					for (const auto &target: priorInsertionTargets) {
						targets[{target.page_id.value(), static_cast<uint16_t>(target.kind)}] = target;
					}
				}
				for (const ExistingInsertCandidate &candidate: existingInsertCandidates) {
					if (candidate.remaining != 0U) {
						targets[{candidate.page_id.value(), static_cast<uint16_t>(candidate.kind)}] =
								{candidate.page_id, candidate.kind, candidate.pool, candidate.remaining};
                    } else targets.erase({candidate.page_id.value(), static_cast<uint16_t>(candidate.kind)});
				}
				for (size_t ordinal = 0U; ordinal < mutations.size(); ++ordinal) {
					const EntityPageBatchMutation &mutation = mutations[ordinal];
					const uint16_t oldRemaining = remainingSlots.at(mutation.page_id.value());
					const size_t inserted = mutation.record_kind == EntityPageRecordKind::EPRK_NODE
							? mutation.node_insertions.size() : mutation.edge_insertions.size();
					const size_t erased = mutation.record_kind == EntityPageRecordKind::EPRK_NODE
							? mutation.erased_node_ids.size() : mutation.erased_edge_ids.size();
					const auto key = std::make_pair(mutation.page_id.value(), static_cast<uint16_t>(mutation.record_kind));
					if (ordinal >= published.pages.size() || published.pages[ordinal].page_retired ||
							inserted > static_cast<size_t>(oldRemaining) + erased) {
						targets.erase(key);
						continue;
					}
					const uint16_t remaining = static_cast<uint16_t>(static_cast<size_t>(oldRemaining) + erased - inserted);
					if (remaining == 0U) {
						targets.erase(key);
						continue;
					}
					const PageLocation location = context_->metadata->findPageLocation(nextSnapshot, mutation.page_id);
					const CatalogEntry pool = context_->metadata->findCatalog(nextSnapshot, location.segment_id);
					targets[key] = {mutation.page_id, mutation.record_kind, pool, remaining};
				}
				const auto addFreshTarget = [&](PageId pageId, EntityPageRecordKind kind, size_t inserted) {
					const uint16_t capacity = EntityPage::recordCapacity(kind);
					if (inserted >= capacity) return;
					const PageLocation location = context_->metadata->findPageLocation(nextSnapshot, pageId);
					const CatalogEntry pool = context_->metadata->findCatalog(nextSnapshot, location.segment_id);
					targets[{pageId.value(), static_cast<uint16_t>(kind)}] =
							{pageId, kind, pool, static_cast<uint16_t>(capacity - inserted)};
				};
				for (const EntityPageBatchAppend &append: appends) {
					addFreshTarget(append.request.page_id, append.record_kind,
							append.record_kind == EntityPageRecordKind::EPRK_NODE ? append.node_insertions.size()
																								 : append.edge_insertions.size());
				}
				for (const EntityPageBatchNewPool &newPool: newPools) {
					addFreshTarget(newPool.request.page_id, newPool.record_kind,
							newPool.record_kind == EntityPageRecordKind::EPRK_NODE ? newPool.node_insertions.size()
																								 : newPool.edge_insertions.size());
				}
				std::lock_guard cacheLock(context_->insertion_cache_mutex);
				context_->insertion_cache.targets.clear();
				context_->insertion_cache.targets.reserve(targets.size());
				for (const auto &[key, target]: targets) {
					static_cast<void>(key);
					context_->insertion_cache.targets.push_back(target);
				}
				context_->insertion_cache.node_directory_root = nextSnapshot.superblock().node_entity_directory_root;
				context_->insertion_cache.edge_directory_root = nextSnapshot.superblock().edge_entity_directory_root;
				context_->insertion_cache.complete_kinds = insertionKindsComplete;
				context_->insertion_cache.valid = true;
				nextSnapshot.release();
			} catch (...) {
				std::lock_guard cacheLock(context_->insertion_cache_mutex);
				context_->insertion_cache.valid = false;
				context_->insertion_cache.targets.clear();
			}
		};

		try {
			std::vector<uint64_t> vectorRebuildIndexIds;
			for (const StagedVectorDelta &staged: vector_deltas_) {
				if (staged.mutation.maintenance_rebuild_required) {
					vectorRebuildIndexIds.push_back(staged.mutation.index_id);
				}
			}
			std::ranges::sort(vectorRebuildIndexIds);
			const EntityPageBatchResult published = [&] {
				debug::ScopedPerfTimer publishTimer("native_graph.commit.entity_publish");
                if (hybrid_vectors_) {
                    if (!mutations.empty() || !appends.empty() || !newPools.empty() || !propertyPlans.empty() || !auxiliary.empty())
                        fail(FormatError::FERR_INVALID_STATE, "native Vector coordination cannot mix ordinary Entity/ROW work");
                    const auto &base = snapshot_.superblock(); const auto &fields = *hybrid_columns_;
                    const auto &placement = context_->options.property_roots;
                    NativeHybridVectorPublication proof(*context_->metadata, derivedState,
                        {base.database_id, base.generation, base.hybrid_property_descriptor_root, fields.cells,
                            fields.memory, placement.first_slot, placement.last_slot, placement.page_owner_id,
                            fields.cancellation, fields.field_lifecycle, fields.deletions}, hybrid_vectors_->semantics, bool(hybrid_catalog_credit_),
                        hybrid_catalog_tokens_ ? std::span<const uint32_t>(*hybrid_catalog_tokens_) : std::span<const uint32_t>{});
                    EntityPageBatchResult result;
                    result.superblock = context_->metadata->mutateNativeGraphPagesWithLargeValues(
                        {}, {}, {}, {}, {}, {}, derivedState, checkpointLsn,
                        [](auto pages, auto appends, auto pools, auto properties, auto propertyBatches, auto largeValues) {
                            if (!pages.empty() || !appends.empty() || !pools.empty() || !properties.empty() || !propertyBatches.empty() || !largeValues.empty())
                                fail(FormatError::FERR_INVALID_STATE, "native Vector-only preparer received unrelated graph images");
                        }, derivedStatePreparer, vectorBootstrapPreparer, vectorDeltaPreparer, {}, {}, {}, publishOptions,
                        {}, base.generation, {}, {}, {}, {}, {}, std::nullopt, std::nullopt, &proof);
                    return result;
                }
                std::vector<EntityPropertyKey, utils::OwnedPmrAllocator<EntityPropertyKey>> topologyPropertyOwners;
                std::optional<NativeHybridTopologyPublication> topologyProof;
                utils::OwnedVector<scan_candidate_detail::ScanNodeChange> scanChanges;
                if constexpr (RSS_SCAN_PATH_FORMAT_ENABLED) {
                    if (native_graph_coordinated_ && snapshot_.superblock().hybrid_property_descriptor_root.isValid()) {
                    for (const auto &[id, node] : nodes_) {
                        if (node.inserted && node.erased) continue;
                        using Kind = scan_candidate_detail::ScanNodeChangeKind;
                        if (node.erased) scanChanges.push_back({id, Kind::SNCK_DELETE, {}});
                        else if (node.inserted || node.labels) {
                            if (!node.labels) fail(FormatError::FERR_INVALID_STATE, "new scan Node has no complete labels");
                            scanChanges.push_back({id, node.inserted ? Kind::SNCK_CREATE : Kind::SNCK_RELABEL, *node.labels});
                        }
                    }
                    if (scanChanges.size() > 64U)
                        fail(FormatError::FERR_UNSUPPORTED_VERSION, "scan Node changes exceed the atomic source workspace window");
                    }
                }
                const bool schemaOnlyCandidate = native_schema_coordinated_ && nodes_.empty() && edges_.empty() && properties_.empty() &&
                    node_id_ranges_.empty() && edge_id_ranges_.empty() && vector_deltas_.empty();
                if ((native_graph_coordinated_ || schemaOnlyCandidate) && snapshot_.superblock().hybrid_property_descriptor_root.isValid()) {
                    // No field/directory authority is granted by ordinary graph
                    // staging. The metadata writer proves every affected owner.
                    for (const auto &[key, root] : properties_) {
                        if (pendingPropertyKeys && std::ranges::find(*pendingPropertyKeys, key) != pendingPropertyKeys->end()) continue;
                        topologyPropertyOwners.push_back({root.owner_entity_type == Node::typeId ?
                            EntityPropertyOwnerKind::EPOK_NODE : EntityPropertyOwnerKind::EPOK_EDGE, root.owner_entity_id});
                    }
                    // Existing schema stays fixed for this preservation path.
                    // Token/sequence/config/count changes are field-independent;
                    // index/constraint DDL needs its own native coordinator.
                    bool schemaPreserved = true, schemaControlsKnown = true;
                    for (const auto &index : scalar_index_mutations_) if (index.index_id == RSS_SYSTEM_CATALOG_INDEX_ID)
                        for (const auto &operation : index.operations) {
                            if (operation.key.empty()) { schemaPreserved = false; schemaControlsKnown = false; continue; }
                            const auto kind = std::to_integer<uint8_t>(operation.key.front());
                            if (kind > 13) schemaControlsKnown = false;
                            if (kind > 13 || (kind >= 6 && kind <= 11))
                                schemaPreserved = false;
                            if (pendingCatalogTokens && !pendingFields->empty() && kind == 2U) {
                                if (operation.kind != BTreeScalarIndexOperationKind::BSIOP_UPSERT || operation.key.size() <= 5U ||
                                    !operation.value || operation.value > UINT32_MAX || pendingCatalogTokens->size() == 64U)
                                    fail(FormatError::FERR_INVALID_STATE, "native creation has an invalid new Catalog token");
                                uint32_t token = 0;
                                for (size_t i = 1; i <= 4; ++i) token = (token << 8U) | std::to_integer<uint32_t>(operation.key[i]);
                                if (token != operation.value) fail(FormatError::FERR_INVALID_STATE, "native creation token reverse identity disagrees");
                                pendingCatalogTokens->push_back(token);
                            }
                        }
                    if (pendingCatalogTokens) {
                        std::ranges::sort(*pendingCatalogTokens);
                        if (std::ranges::adjacent_find(*pendingCatalogTokens) != pendingCatalogTokens->end())
                            fail(FormatError::FERR_INVALID_STATE, "native creation repeats a Catalog token allocation");
                    }
                    topologyProof.emplace(NativeHybridTopologyPublication(*context_->metadata, *context_->entities,
                        snapshot_.superblock(), topologyPropertyOwners, schemaPreserved, derivedState,
                        pendingFields ? std::span<const NativeHybridPropertyColumnReplacement>(*pendingFields) : std::span<const NativeHybridPropertyColumnReplacement>{},
                        pendingFieldsMemory, pendingCatalogTokens ? std::span<const uint32_t>(*pendingCatalogTokens) : std::span<const uint32_t>{},
                        nativeOwnerDeletions ? std::span<const EntityPropertyKey>(*nativeOwnerDeletions) : std::span<const EntityPropertyKey>{},
                        schemaOnlyCandidate && schemaControlsKnown, scanChanges, context_->options.property_roots.page_owner_id));
                }
				return context_->entities->applyNativeGraphBatchTransaction(
						snapshot_, mutations, appends, newPools, propertyPlans, *context_->properties,
						context_->options.property_roots, auxiliary, derivedState, derivedStatePreparer, checkpointLsn,
						publishOptions, logicalIds, vectorBootstrapPreparer, vectorDeltaPreparer,
                        privateSegmentImagePreparer, {}, deferredIndexPlanner, {}, topologyProof ? &*topologyProof : nullptr);
			}();
            if (hybrid_vectors_ && !hybrid_columns_->deletions.empty()) {
                std::lock_guard lock(context_->insertion_cache_mutex);
                context_->insertion_cache.valid = false; context_->insertion_cache.targets.clear();
            } else refreshInsertionTargetCache(published);
			snapshot_.release();
			state_ = NativeGraphWriteTransactionState::NGWTS_COMMITTED;
			return {NativeGraphWriteCommitOutcome::NGWCO_COMMITTED, published.superblock,
					std::move(vectorRebuildIndexIds)};
		} catch (...) {
			snapshot_.release();
			state_ = NativeGraphWriteTransactionState::NGWTS_INDETERMINATE;
			throw;
		}
	}

} // namespace graph::storage
