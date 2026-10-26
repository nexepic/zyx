		// A bulk builder reopens its input while final pages are materialized, not
		// only while it computes the layout. Keep the writer-fenced pin alive for
		// both passes; it remains private to this candidate and is destroyed before
		// the metadata writer leaves scope.
		std::optional<MetadataSnapshotPin> deferredPlanningSnapshot;
		if (earlyPublicationPlanner || deferredIndexPlanner || deferredIndexPlansPlanner) {
			// The private images are durable, but no Catalog/Allocator/Superblock
			// root reaches them. A planner can therefore make both B+Tree passes
			// before this coordinator assigns any destination Index identities.
			io_->sync();
			// Opening a regular snapshot here would attempt to acquire the reader
			// side of the same publication barrier held by `writer`. Pin the already
			// fenced durable root instead, and keep that pin confined to planning.
			deferredPlanningSnapshot.emplace(snapshots_.pinCurrentUnderWrite(writer, current));
			try {
				if (earlyPublicationPlanner) {
					debug::ScopedPerfTimer timer("native_graph.streamed.early_publication");
					earlyPublicationPlanner({*privateSegmentSet, candidateReader, candidateWriter, candidateFence,
																					 *deferredPlanningSnapshot, batch, current, checkpointLsn,
																					 deferIndexPlan});
					if (privateScratchState->candidate_payloads_rewritten) {
						io_->sync();
					}
				}
			} catch (...) {
				deferredPlanningSnapshot->release();
				throw;
			}
			if (deferredIndexPlanner) {
				NativeGraphDeferredIndexPlan plan;
				try {
					plan = deferredIndexPlanner({*privateSegmentSet, candidateReader, *deferredPlanningSnapshot});
				} catch (...) {
					deferredPlanningSnapshot->release();
					throw;
				}
				deferredPlans.push_back(std::move(plan));
			}
			if (deferredIndexPlansPlanner) {
				try {
					NativeGraphDeferredIndexPlans plans =
							deferredIndexPlansPlanner({*privateSegmentSet, candidateReader, *deferredPlanningSnapshot});
					deferredPlans.insert(deferredPlans.end(), std::make_move_iterator(plans.plans.begin()),
										 std::make_move_iterator(plans.plans.end()));
				} catch (...) {
					deferredPlanningSnapshot->release();
					throw;
				}
			}
            std::set<uint64_t> deferredIndexIds;
			for (NativeGraphDeferredIndexPlan &plan: deferredPlans) {
                if (!deferredIndexIds.insert(plan.mutation.index_id).second)
                    throw FormatException(FormatError::FERR_INVALID_STATE, "deferred index plans repeat a root");
                if (plan.images) {
                    auto &proof = *plan.images;
                    const auto &bare = plan.mutation;
                    if (!privateSegmentSet || !privateSegmentSet->size() || (!proof.private_segments_ || !proof.private_segments_->size()))
                        throw FormatException(FormatError::FERR_INVALID_STATE, "deferred images have no scoped private source");
                    const auto first = proof.private_segments_->front(), actualFirst = privateSegmentSet->front();
                    if (proof.metadata_ != this || proof.source_generation_ != current.generation || proof.materialized_ ||
                        !proof.memory_ || !proof.reader_ || !proof.entry_count_ || !proof.page_count_ ||
                        proof.private_segments_->size() != privateSegmentSet->size() || first.segment_id != actualFirst.segment_id ||
                        first.generation != actualFirst.generation || first.owner_id != actualFirst.owner_id ||
                        locations_.findIndexRootOptional(current, proof.index_id_) != proof.source_root_ ||
                        bare.index_id != proof.index_id_ || bare.expected_root || !bare.pool_requests.empty() ||
                        !bare.new_pool_requests.empty() || bare.replacement_root || bare.stable_replacement_root || bare.retire_root ||
                        plan.preparer || std::ranges::any_of(effectiveIndexMutations, [&proof](const auto &other) {
                            return other.index_id == proof.index_id_;
                        }))
                        throw FormatException(FormatError::FERR_INVALID_STATE, "deferred images have no exact owned source receipt");
                    if (proof.index_id_ != RSS_GLOBAL_ADJACENCY_INDEX_ID) {
                        BTreePageStore schemaPages(*deferredSchemaPool, *this); SystemCatalog schema(schemaPages, *this);
                        const auto definition = schema.findIndexDefinitionById(*deferredPlanningSnapshot, proof.index_id_);
                        if (!definition || definition->kind != IndexDefinitionKind::IDK_LABEL ||
                            definition->entity_kind != IndexDefinitionEntityKind::IDEK_EDGE)
                            throw FormatException(FormatError::FERR_INVALID_STATE, "streamed receipt lost its EDGE membership schema");
                    }
                    { utils::OwnedHeapReservation leaseRead(proof.memory_, 2U * RSS_SEGMENT_SIZE);
                      static_cast<void>(proof.reader_(proof.private_segments_->front())); }
                    if (proof.incremental_) {
                        prepareStreamedAdjacencyImages(batch, writer, *deferredPlanningSnapshot, proof,
                            proof.index_id_ == RSS_GLOBAL_ADJACENCY_INDEX_ID ? streamedAdjacencyPlacement : streamedMembershipPlacement);
                        continue;
                    }
                    proof.materialized_ = true;
                    plan.mutation = std::move(proof.fresh_mutation_); plan.preparer = std::move(proof.fresh_preparer_);
                }

				const NativeGraphIndexTreeMutation &mutation = plan.mutation;
				const bool replacesRoot = mutation.replacement_root.has_value();
				const bool retiresRoot = mutation.retire_root;
				bool allowedIndexId = mutation.index_id == RSS_GLOBAL_ADJACENCY_INDEX_ID ||
											mutation.index_id == RSS_SYSTEM_CATALOG_INDEX_ID ||
											mutation.index_id == RSS_LARGE_VALUE_CONTENT_INDEX_ID;
				if (!allowedIndexId && isSystemCatalogIndexDefinitionId(mutation.index_id) &&
						replacesRoot && mutation.expected_root.has_value()) {
					// Logical members are admitted only by their authenticated published
					// schema in this writer-fenced snapshot, never by an arbitrary ID.
					BTreePageStore schemaPages(*deferredSchemaPool, *this);
					SystemCatalog schema(schemaPages, *this);
					const auto definition = schema.findIndexDefinitionById(*deferredPlanningSnapshot, mutation.index_id);
					allowedIndexId = definition.has_value() && definition->kind == IndexDefinitionKind::IDK_LABEL &&
							definition->entity_kind == IndexDefinitionEntityKind::IDEK_EDGE;
				}
				if (!plan.preparer || !allowedIndexId || // effective index mutations are assembled only from authenticated preparers and allowed IDs.
					std::ranges::any_of(effectiveIndexMutations,
										[&mutation](const NativeGraphIndexTreeMutation &existing) {
											return existing.index_id == mutation.index_id;
										}) ||
					(replacesRoot == retiresRoot) ||
					(replacesRoot &&
					 (mutation.new_pool_requests.empty() || !mutation.replacement_root->newly_reserved ||
					  mutation.stable_replacement_root.has_value())) ||
					(retiresRoot && (!mutation.expected_root.has_value() || !mutation.new_pool_requests.empty() ||
									 mutation.stable_replacement_root.has_value() || mutation.pool_requests.empty()))) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph deferred Index planner returned an invalid root transition");
				}
				const std::optional<IndexRootLocation> visibleRoot =
						locations_.findIndexRootOptional(current, mutation.index_id);
				if ((mutation.expected_root.has_value() &&
					 (!visibleRoot.has_value() || *visibleRoot != *mutation.expected_root)) ||
					(!mutation.expected_root.has_value() && visibleRoot.has_value())) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph deferred Index planner lost its current root fence");
				}
				if (replacesRoot) {
					const NativeGraphIndexTreeAppendedPageReference &root = *mutation.replacement_root;
					if (root.pool_ordinal >= mutation.new_pool_requests.size() ||
						root.appended_page_ordinal >=
								mutation.new_pool_requests[root.pool_ordinal].appended_page_count) {
						throw FormatException(
								FormatError::FERR_INVALID_STATE,
								"native graph deferred Index planner returned an invalid fresh root reference");
					}
				}
				std::vector<IndexTreeMutationPoolBatch> batches;
				batches.reserve(mutation.pool_requests.size() + mutation.new_pool_requests.size());
				bool retiredExpectedRoot = false;
				std::set<uint64_t> sourcePoolIds;
				for (const IndexTreeMutationPoolRequest &request: mutation.pool_requests) {
					if (!request.segment_id.isValid() ||
						(request.expected_replacements.empty() && request.appended_page_count == 0U &&
							 request.expected_retirements.empty()) ||
						(request.replacement_segment_id.isValid() && request.expected_replacements.empty()) ||
						request.appended_page_count > RSS_PAGE_POOL_DATA_FRAME_COUNT ||
						!sourcePoolIds.insert(request.segment_id.value()).second) {
						throw FormatException(FormatError::FERR_INVALID_STATE,
											  "native graph deferred Index planner has an invalid source pool request");
					}
					const CatalogEntry pool = catalog_.find(current, request.segment_id);
					if (pool.kind != SegmentKind::SGK_INDEX_PAGE_POOL || pool.state != SegmentState::SGS_SEALED ||
						pool.owner_id == 0U || !allocator_.isAllocated(current, pool.physical_slot)) {
						throw FormatException(
								FormatError::FERR_INVALID_STATE,
								"native graph deferred Index planner references an unavailable source pool");
					}
					CatalogEntry replacementPool = pool;
					if (request.replacement_segment_id.isValid()) {
						replacementPool = catalog_.find(current, request.replacement_segment_id);
						if (replacementPool.segment_id == pool.segment_id ||
							replacementPool.kind != SegmentKind::SGK_INDEX_PAGE_POOL ||
							replacementPool.owner_id != pool.owner_id || replacementPool.state != SegmentState::SGS_SEALED ||
							!allocator_.isAllocated(current, replacementPool.physical_slot)) {
							throw FormatException(
									FormatError::FERR_INVALID_STATE,
									"native graph deferred Index planner references an unavailable cross-pool replacement destination");
						}
					}
					for (const IndexTreePageRetirement &retirement: request.expected_retirements) {
						const PageLocation visible = locations_.findPage(current, retirement.location.page_id);
						if (visible != retirement.location || visible.segment_id != pool.segment_id ||
							visible.generation != pool.generation || !retirement.expected_owner_lsn.isValid() ||
							!pageIds.insert(visible.page_id.value()).second) {
							throw FormatException(
									FormatError::FERR_INVALID_STATE,
									"native graph deferred Index planner references a stale retirement page");
						}
						retiredExpectedRoot =
								retiredExpectedRoot ||
								(mutation.expected_root.has_value() &&
								 retirement.location.page_id == mutation.expected_root->root_page_id &&
								 retirement.location.generation == mutation.expected_root->root_generation);
					}
					batches.push_back({pool,
								   request.expected_replacements,
								   {},
								   request.expected_replacements,
								   {},
								   false,
								   request.expected_retirements,
								   0U,
								   replacementPool});
				}
				if (mutation.expected_root.has_value() && !retiredExpectedRoot) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph deferred Index replacement did not retire its fenced root");
				}
				for (const NewIndexTreePoolRequest &request: mutation.new_pool_requests) {
					if (!request.first_slot.isValid() || !request.last_slot.isValid() ||
						request.first_slot > request.last_slot || request.owner_id != mutation.index_id ||
						request.appended_page_count == 0U ||
						request.appended_page_count > RSS_PAGE_POOL_DATA_FRAME_COUNT) {
						throw FormatException(FormatError::FERR_INVALID_STATE,
											  "native graph deferred Index planner has an invalid fresh pool request");
					}
				}
				effectiveIndexMutations.push_back(std::move(plan.mutation));
				derivedIndexBatches.push_back(std::move(batches));
				deferredIndexPreparers.push_back(std::move(plan.preparer));
			}
		}
            std::set<uint64_t> ordinaryImageIds;
            if (!derivedState.index_images.empty()) {
                const auto &first = derivedState.index_images.front();
                if (!first || !first->memory_)
                    throw FormatException(FormatError::FERR_INVALID_STATE, "ordinary images require an owned source");
                ordinaryIndexSource.emplace(snapshots_.pinCurrentUnderWrite(writer, current));
                ordinaryIndexPlacement.reset(new HybridPropertyPoolPlacement(*this, batch, writer, *ordinaryIndexSource,
                    first->first_slot_, first->last_slot_, first->memory_, [] {}, true));
                // The regular frame planner has already budgeted these pools.
                // Reserve no destinations from them for private image work.
                const auto exclude = [&](SegmentId id) {
                    if (!id.isValid() || ordinaryIndexPlacement->contains(id)) return;
                    const auto pool = catalog_.find(current, id);
                    if (pool.kind != SegmentKind::SGK_INDEX_PAGE_POOL)
                        throw FormatException(FormatError::FERR_INVALID_STATE, "ordinary competing plan lost its Index pool");
                    ordinaryIndexPlacement->reservations_.push_back({pool, 0, 0, 0, false, false, false});
                };
                for (const auto &mutation : effectiveIndexMutations) for (const auto &request : mutation.pool_requests) {
                    exclude(request.segment_id); exclude(request.replacement_segment_id);
                }
            }
            for (const auto &owned : derivedState.index_images) {
                if (!owned || !ordinaryImageIds.insert(owned->index_id_).second)
                    throw FormatException(FormatError::FERR_INVALID_STATE, "ordinary image receipts repeat an index");
                auto &proof = *owned;
                if (proof.metadata_ != this || proof.source_generation_ != current.generation || proof.materialized_ ||
                    proof.private_segments_ || proof.reader_ || !proof.memory_ || !proof.incremental_ ||
                    (proof.index_id_ != RSS_GLOBAL_ADJACENCY_INDEX_ID && !isSystemCatalogIndexDefinitionId(proof.index_id_)) ||
                    locations_.findIndexRootOptional(current, proof.index_id_) != proof.source_root_ ||
                    std::ranges::any_of(effectiveIndexMutations, [&proof](const auto &other) {return other.index_id == proof.index_id_;}) ||
                    std::ranges::any_of(deferredPlans, [&proof](const auto &other) {return other.mutation.index_id == proof.index_id_;}))
                    throw FormatException(FormatError::FERR_INVALID_STATE, "ordinary index images lost their exact owned source");
                if (proof.index_id_ != RSS_GLOBAL_ADJACENCY_INDEX_ID) {
                    BTreePageStore schemaPages(*deferredSchemaPool, *this); SystemCatalog schema(schemaPages, *this);
                    const auto definition = schema.findIndexDefinitionById(*ordinaryIndexSource, proof.index_id_);
                    if (!definition || definition->kind != IndexDefinitionKind::IDK_LABEL ||
                        definition->entity_kind != IndexDefinitionEntityKind::IDEK_EDGE)
                        throw FormatException(FormatError::FERR_INVALID_STATE, "ordinary image receipt lost its EDGE membership schema");
                }
                prepareStreamedAdjacencyImages(batch, writer, *ordinaryIndexSource, proof, ordinaryIndexPlacement);
                debug::PerfTrace::addValue(proof.index_id_ == RSS_GLOBAL_ADJACENCY_INDEX_ID ?
                    "native_graph.write.ordinary_adjacency.prepared_images" : "native_graph.write.ordinary_membership.prepared_images", int64_t(proof.images_.size()));
            }
		std::vector<CatalogEntry> newPoolEntries;
		newPoolEntries.reserve(newPoolRequests.size() + freshReplacementPools.size());
		std::map<uint64_t, size_t> newPoolEntryBySegment;
		for (const EntityPageNewPoolBatchRequest &request: newPoolRequests) {
			const auto fresh = freshReplacementPools.find(request.reservation.segment_id.value());
			if (fresh != freshReplacementPools.end() && !hasSameFreshEntityPoolIdentity(fresh->second, request)) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable Entity page transaction conflicts with its fresh COW-pool reservation");
			}
		}
		for (const EntityPageNewPoolBatchRequest &request: newPoolRequests) {
			const auto existing = newPoolEntryBySegment.find(request.reservation.segment_id.value());
			if (existing != newPoolEntryBySegment.end()) {
				const CatalogEntry &entry = newPoolEntries[existing->second];
				newPoolMaterializations.push_back(
						{entry, {request.page_id, entry.segment_id, 0U, entry.generation, 0U}, request.owner_kind});
				continue;
			}

			const std::optional<PhysicalSlotId> physicalSlot =
					findFirstFreeNewPoolSlot(request.first_slot, request.last_slot);
			if (!physicalSlot.has_value()) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable Entity page new-pool batch has no free physical slot in its request range");
			}
			CatalogEntry entry;
			entry.segment_id = request.reservation.segment_id;
			entry.physical_slot = *physicalSlot;
			entry.kind = request.reservation.kind;
			entry.owner_id = request.reservation.owner_id;
			entry.generation = request.reservation.segment_generation;
			entry.state = request.reservation.state;
			entry.committed_lsn = checkpointLsn;
			batch.observeLogicalSegmentId(entry.segment_id);
			batch.reserveDataSlot(entry.physical_slot);
			newPoolEntryBySegment.emplace(entry.segment_id.value(), newPoolEntries.size());
			newPoolEntries.push_back(entry);
			newPoolMaterializations.push_back(
					{entry, {request.page_id, entry.segment_id, 0U, entry.generation, 0U}, request.owner_kind});
		}
		for (const auto &[segmentId, destination]: freshReplacementPools) {
			if (newPoolEntryBySegment.contains(segmentId)) {
				continue;
			}
			const std::optional<PhysicalSlotId> physicalSlot =
					findFirstFreeNewPoolSlot(destination.first_slot, destination.last_slot);
			if (!physicalSlot.has_value()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable Entity page fresh COW destination has no free physical slot");
			}
			CatalogEntry entry;
			entry.segment_id = destination.segment_id;
			entry.physical_slot = *physicalSlot;
			entry.kind = SegmentKind::SGK_ENTITY_PAGE_POOL;
			entry.owner_id = destination.pool_owner_id;
			entry.generation = destination.generation;
			entry.state = SegmentState::SGS_SEALED;
			entry.committed_lsn = checkpointLsn;
			batch.observeLogicalSegmentId(entry.segment_id);
			batch.reserveDataSlot(entry.physical_slot);
			newPoolEntryBySegment.emplace(segmentId, newPoolEntries.size());
			newPoolEntries.push_back(entry);
		}
		for (size_t ordinal = 0U; ordinal < replacementFreshDestinations.size(); ++ordinal) {
			if (!replacementFreshDestinations[ordinal].has_value()) {
				continue;
			}
			const EntityPageFreshPoolDestination &destination = *replacementFreshDestinations[ordinal];
			const auto entryOrdinal = newPoolEntryBySegment.find(destination.segment_id.value());
			if (entryOrdinal == newPoolEntryBySegment.end()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable Entity page fresh COW destination was not materialized");
			}
			const CatalogEntry &entry = newPoolEntries[entryOrdinal->second];
			EntityPageReplacementMaterialization &materialization = materializations[ordinal];
			materialization.pool = entry;
			materialization.replacement = {materialization.expected_location.page_id, entry.segment_id, 0U,
										   entry.generation, 0U};
		}
		std::vector<CatalogEntry> freshPropertyReplacementPoolEntries;
		freshPropertyReplacementPoolEntries.reserve(propertyReplacements.size());
		struct FreshPropertyReplacementPool {
			CatalogEntry entry;
			size_t prepared_frames = 0U;
		};
		std::map<uint64_t, FreshPropertyReplacementPool> freshPropertyReplacementPools;
		for (size_t ordinal = 0U; ordinal < propertyReplacements.size(); ++ordinal) {
			if (!propertyReplacementNeedsFreshPool[ordinal]) {
				continue;
			}
			const NativeGraphPropertyPageReplacementRequest &request = propertyReplacements[ordinal];
			const bool invalidReservation = !request.first_slot.isValid() || !request.last_slot.isValid() ||
											request.first_slot > request.last_slot;
			if (invalidReservation) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"native graph Property COW exhausted a pinned pool without a fresh-pool reservation range");
			}
			FreshPropertyReplacementPool *destination = nullptr;
			auto existing = freshPropertyReplacementPools.find(request.expected_page_owner_id);
			if (existing != freshPropertyReplacementPools.end() &&
				existing->second.prepared_frames < RSS_PAGE_POOL_DATA_FRAME_COUNT) {
				destination = &existing->second;
			} else {
				const std::optional<PhysicalSlotId> physicalSlot =
						findFirstFreeNewPoolSlot(request.first_slot, request.last_slot);
				if (!physicalSlot.has_value()) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph Property COW has no free physical slot for a fresh pool");
				}
				CatalogEntry entry;
				entry.segment_id = batch.allocateLogicalSegmentId();
				entry.physical_slot = *physicalSlot;
				entry.kind = SegmentKind::SGK_PROPERTY_PAGE_POOL;
				entry.owner_id = request.expected_page_owner_id;
				entry.generation = Generation(1U);
				entry.state = SegmentState::SGS_SEALED;
				entry.committed_lsn = checkpointLsn;
				batch.reserveDataSlot(entry.physical_slot);
				freshPropertyReplacementPoolEntries.push_back(entry);
				auto [replacement, inserted] = freshPropertyReplacementPools.insert_or_assign(
						request.expected_page_owner_id, FreshPropertyReplacementPool{entry});
				static_cast<void>(inserted);
				destination = &replacement->second;
			}
			const bool initializesPool = destination->prepared_frames++ == 0U;
			propertyMaterializations[ordinal].pool = destination->entry;
			propertyMaterializations[ordinal].replacement = {
					propertyMaterializations[ordinal].expected_location.page_id, destination->entry.segment_id, 0U,
					destination->entry.generation, 0U};
			propertyMaterializations[ordinal].destination_is_new_pool = true;
			propertyMaterializations[ordinal].destination_initializes_new_pool = initializesPool;
		}
		std::vector<CatalogEntry> newPropertyPoolEntries;
		newPropertyPoolEntries.reserve(propertyPageBatchRequests.size());
		for (const NativeGraphPropertyPageBatchRequest &request: propertyPageBatchRequests) {
			CatalogEntry entry = request.expected_pool;
			if (request.kind == NativeGraphPropertyPageBatchKind::NGPBK_CREATE_POOL) {
				const std::optional<PhysicalSlotId> physicalSlot =
						findFirstFreeNewPoolSlot(request.first_slot, request.last_slot);
				if (!physicalSlot.has_value()) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"native graph Property pool creation has no free physical slot in its request range");
				}
				entry.segment_id = batch.allocateLogicalSegmentId();
				entry.physical_slot = *physicalSlot;
				entry.kind = SegmentKind::SGK_PROPERTY_PAGE_POOL;
				entry.owner_id = request.page_owner_id;
				entry.generation = Generation(1U);
				entry.state = SegmentState::SGS_SEALED;
				entry.committed_lsn = checkpointLsn;
				if (catalog_.findOptional(current, entry.segment_id).has_value()) { // the writer-held batch allocates a fresh monotonic logical Segment ID.
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"native graph Property pool creation allocated a published logical Segment ID");
				}
				batch.reserveDataSlot(entry.physical_slot);
				newPropertyPoolEntries.push_back(entry);
			}

			std::vector<NativeGraphPropertyPageInitialMaterialization> initialPages;
			initialPages.reserve(request.page_property_keys.size());
			for (const std::vector<EntityPropertyKey> &pageKeys: request.page_property_keys) {
				const PageId pageId = batch.allocateLogicalPageId();
				if (locations_.findPageOptional(current, pageId).has_value() || // logical Page IDs come from the monotonic batch allocator and are checked before materialization.
					!pageIds.insert(pageId.value()).second) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph Property page batch allocated a published logical Page ID");
				}
				std::vector<EntityPropertyLocation> properties;
				properties.reserve(pageKeys.size());
				for (const EntityPropertyKey &key: pageKeys) {
					properties.push_back({key, pageId, std::numeric_limits<uint16_t>::max(), 0U});
				}
				initialPages.push_back({{pageId, entry.segment_id, 0U, entry.generation, 0U}, std::move(properties)});
			}
			propertyPageBatchMaterializations.push_back(
					{request.kind, entry, request.page_owner_id, std::move(initialPages)});
		}
		struct SharedLargeValuePoolCapacity {
			CatalogEntry pool;
			size_t available_frames = 0U;
			size_t prepared_frames = 0U;
			bool is_new_pool = false;
		};
		std::vector<SharedLargeValuePoolCapacity> sharedLargeValueDataPools;
		std::vector<SharedLargeValuePoolCapacity> sharedLargeValueManifestPools;
		if (!largeValues.empty()) {
			// Publication owns the writer lock, so a regular snapshot acquisition would
			// self-deadlock. This pin reads the already fenced root without reacquiring
			// the reader lock; the header checks below remain fail-closed.
			MetadataSnapshotPin poolSnapshot = snapshots_.pinCurrentUnderWrite(writer, current);
			for (const CatalogEntry &pool: findCurrentLargeValueDataPagePools(poolSnapshot)) {
				if (!allocator_.isAllocated(current, pool.physical_slot)) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph LargeValue capacity query found an unallocated pool slot");
				}
				sharedLargeValueDataPools.push_back({pool, SharedPagePoolAllocator::availableFrameCountUnderWrite(
																   *io_, current.data_region_offset, pool)});
			}
			for (const CatalogEntry &pool: findCurrentLargeValueManifestPagePools(poolSnapshot)) {
				if (!allocator_.isAllocated(current, pool.physical_slot)) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"native graph LargeValue Manifest capacity query found an unallocated pool slot");
				}
				sharedLargeValueManifestPools.push_back({pool, SharedPagePoolAllocator::availableFrameCountUnderWrite(
																	   *io_, current.data_region_offset, pool)});
			}
			poolSnapshot.release();
		}
		std::vector<CatalogEntry> newLargeValuePoolEntries;
		const auto allocateSharedLargeValuePages =
				[this, &batch, &current, &pageIds](std::vector<SharedLargeValuePoolCapacity> &candidates,
												   size_t &remainingPages,
												   std::vector<LargeValueDataMaterializationPoolBatch> &destination,
												   const char *duplicatePageMessage) {
					for (SharedLargeValuePoolCapacity &candidate: candidates) {
						if (remainingPages == 0U) {
							break;
						}
						if (candidate.available_frames == 0U) {
							continue;
						}
						const size_t pages = std::min(remainingPages, candidate.available_frames);
						const size_t preparedPrefix = candidate.is_new_pool ? candidate.prepared_frames : 0U;
						LargeValueDataMaterializationPoolBatch poolBatch{
								candidate.pool, {}, {}, candidate.is_new_pool, preparedPrefix};
						poolBatch.page_ids.reserve(pages);
						poolBatch.locations.reserve(pages);
						for (size_t pageOrdinal = 0U; pageOrdinal < pages; ++pageOrdinal) {
							const PageId pageId = batch.allocateLogicalPageId();
						if (locations_.findPageOptional(current, pageId).has_value() || // logical Page IDs come from the monotonic batch allocator and are checked before materialization.
								!pageIds.insert(pageId.value()).second) {
								throw FormatException(FormatError::FERR_INVALID_STATE, duplicatePageMessage);
							}
							poolBatch.page_ids.push_back(pageId);
							poolBatch.locations.push_back(
									{pageId, candidate.pool.segment_id, 0U, candidate.pool.generation, 0U});
						}
						destination.push_back(std::move(poolBatch));
						candidate.available_frames -= pages;
						if (candidate.is_new_pool) {
							candidate.prepared_frames += pages;
						}
						remainingPages -= pages;
					}
				};
		for (const NativeGraphLargeValueMutation &mutation: largeValues) {
			LargeValueMaterializationResult materialization;
			materialization.value_id = jointNativeContent ?
                LargeValueId(jointRowLargeFirst.value() + largeValueMaterializations.size()) : batch.allocateLargeValueId();
			if (materialization.value_id != mutation.reference.value_id) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph LargeValue allocation does not match its pinned logical value ID");
			}
			size_t remainingDataPages = mutation.planned_data_page_count;
			materialization.data_pools.reserve((remainingDataPages + RSS_PAGE_POOL_DATA_FRAME_COUNT - 1U) /
											   RSS_PAGE_POOL_DATA_FRAME_COUNT);
			allocateSharedLargeValuePages(
					sharedLargeValueDataPools, remainingDataPages, materialization.data_pools,
					"native graph LargeValue allocation reused a published shared-pool data Page ID");
			while (remainingDataPages != 0U) {
				CatalogEntry pool;
				pool.segment_id = batch.allocateLogicalSegmentId();
				pool.physical_slot = reserveDistinctNewPoolSlot(
						mutation.first_data_slot, mutation.last_data_slot,
						"native graph LargeValue materialization has no free data-pool slot");
				pool.kind = SegmentKind::SGK_LARGE_VALUE_DATA_POOL;
				pool.owner_id = RSS_LARGE_VALUE_SHARED_DATA_POOL_OWNER_ID;
				pool.generation = mutation.reference.generation;
				pool.state = SegmentState::SGS_SEALED;
				pool.committed_lsn = checkpointLsn;
				if (catalog_.findOptional(current, pool.segment_id).has_value()) { // the writer-held batch allocates a fresh monotonic logical Segment ID.
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph LargeValue allocation reused a published logical Segment ID");
				}
				batch.reserveDataSlot(pool.physical_slot);
				newLargeValuePoolEntries.push_back(pool);
				sharedLargeValueDataPools.push_back({pool, RSS_PAGE_POOL_DATA_FRAME_COUNT, 0U, true});
				allocateSharedLargeValuePages(
						sharedLargeValueDataPools, remainingDataPages, materialization.data_pools,
						"native graph LargeValue allocation reused a published logical data Page ID");
			}
			size_t remainingManifestPages = mutation.planned_manifest_page_count;
			materialization.manifest_pools.reserve((remainingManifestPages + RSS_PAGE_POOL_DATA_FRAME_COUNT - 1U) /
												   RSS_PAGE_POOL_DATA_FRAME_COUNT);
			allocateSharedLargeValuePages(
					sharedLargeValueManifestPools, remainingManifestPages, materialization.manifest_pools,
					"native graph LargeValue allocation reused a published shared-pool Manifest Page ID");
			while (remainingManifestPages != 0U) {
				CatalogEntry pool;
				pool.segment_id = batch.allocateLogicalSegmentId();
				pool.physical_slot = reserveDistinctNewPoolSlot(
						mutation.first_manifest_slot, mutation.last_manifest_slot,
						"native graph LargeValue materialization has no free manifest-pool slot");
				pool.kind = SegmentKind::SGK_LARGE_VALUE_MANIFEST_POOL;
				pool.owner_id = RSS_LARGE_VALUE_SHARED_MANIFEST_POOL_OWNER_ID;
				pool.generation = mutation.reference.generation;
				pool.state = SegmentState::SGS_SEALED;
				pool.committed_lsn = checkpointLsn;
				if (catalog_.findOptional(current, pool.segment_id).has_value()) { // the writer-held batch allocates a fresh monotonic logical Segment ID.
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph LargeValue allocation reused a published manifest Segment ID");
				}
				batch.reserveDataSlot(pool.physical_slot);
				newLargeValuePoolEntries.push_back(pool);
				sharedLargeValueManifestPools.push_back({pool, RSS_PAGE_POOL_DATA_FRAME_COUNT, 0U, true});
				allocateSharedLargeValuePages(sharedLargeValueManifestPools, remainingManifestPages,
											  materialization.manifest_pools,
											  "native graph LargeValue allocation reused a published manifest Page ID");
			}
			materialization.manifest_pool = materialization.manifest_pools.front().pool;
			materialization.manifest_page_id = materialization.manifest_pools.front().page_ids.front();
			materialization.manifest_location = materialization.manifest_pools.front().locations.front();
			largeValueMaterializations.push_back(std::move(materialization));
		}
		// Allocate every fresh derived Index pool from this same candidate batch.
		// Its logical Segment/Page IDs and physical slot reservation therefore have
		// exactly the same visibility boundary as the Node, Edge, and Property work.
		std::vector<CatalogEntry> newDerivedIndexPoolEntries;
		for (size_t mutationOrdinal = 0U; mutationOrdinal < effectiveIndexMutations.size(); ++mutationOrdinal) {
			const NativeGraphIndexTreeMutation &mutation = effectiveIndexMutations[mutationOrdinal];
			std::vector<IndexTreeMutationPoolBatch> &batches = derivedIndexBatches[mutationOrdinal];
			for (const NewIndexTreePoolRequest &request: mutation.new_pool_requests) {
				const std::optional<PhysicalSlotId> physicalSlot =
						findFirstFreeNewPoolSlot(request.first_slot, request.last_slot);
				if (!physicalSlot.has_value()) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"native graph derived-state mutation has no free physical slot for a new Index pool");
				}
				const SegmentId segmentId = batch.allocateLogicalSegmentId();
				if (catalog_.findOptional(current, segmentId).has_value()) { // the writer-held batch allocates a fresh monotonic logical Segment ID.
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph derived-state mutation allocated a published Index Segment ID");
				}
				CatalogEntry pool;
				pool.segment_id = segmentId;
				pool.physical_slot = *physicalSlot;
				pool.kind = SegmentKind::SGK_INDEX_PAGE_POOL;
				pool.owner_id = request.owner_id;
				pool.generation = Generation(1U);
				pool.state = SegmentState::SGS_SEALED;
				pool.committed_lsn = checkpointLsn;
				batch.reserveDataSlot(pool.physical_slot);

				IndexTreeMutationPoolBatch materialization;
				materialization.pool = pool;
				materialization.newly_reserved = true;
				materialization.appended_page_ids.reserve(request.appended_page_count);
				materialization.appended_locations.reserve(request.appended_page_count);
				for (size_t pageOrdinal = 0U; pageOrdinal < request.appended_page_count; ++pageOrdinal) {
					const PageId pageId = batch.allocateLogicalPageId();
					if (locations_.findPageOptional(current, pageId).has_value() || // logical Page IDs come from the monotonic batch allocator and are checked before materialization.
						!pageIds.insert(pageId.value()).second) {
						throw FormatException(
								FormatError::FERR_INVALID_STATE,
								"native graph derived-state mutation allocated a published Index Page ID");
					}
					materialization.appended_page_ids.push_back(pageId);
					materialization.appended_locations.push_back({pageId, pool.segment_id, 0U, pool.generation, 0U});
				}
				newDerivedIndexPoolEntries.push_back(pool);
				batches.push_back(std::move(materialization));
			}
		}
		std::vector<NativeGraphVectorBootstrapMaterialization> vectorBootstrapMaterializations;
		std::vector<CatalogEntry> newVectorBootstrapPoolEntries;
		vectorBootstrapMaterializations.reserve(derivedState.vector_bootstraps.size());
		const auto resolveFreshDerivedRoot = [&effectiveIndexMutations, &derivedIndexBatches](uint64_t indexId) {
			const auto mutation =
					std::ranges::find(effectiveIndexMutations, indexId, &NativeGraphIndexTreeMutation::index_id);
			if (mutation == effectiveIndexMutations.end() || !mutation->replacement_root.has_value() || // Vector bootstrap preflight creates its matching freshly reserved mapping-root mutation before this private replay resolver runs.
				!mutation->replacement_root->newly_reserved) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph Vector bootstrap lost a freshly allocated mapping root");
			}
			const size_t mutationOrdinal = static_cast<size_t>(mutation - effectiveIndexMutations.begin());
			const NativeGraphIndexTreeAppendedPageReference &reference = *mutation->replacement_root;
			const size_t poolOrdinal = mutation->pool_requests.size() + reference.pool_ordinal;
			if (mutationOrdinal >= derivedIndexBatches.size() || // the replay batches are materialized one-for-one from the authenticated effective Index mutations.
				poolOrdinal >= derivedIndexBatches[mutationOrdinal].size() ||
				reference.appended_page_ordinal >=
						derivedIndexBatches[mutationOrdinal][poolOrdinal].appended_locations.size()) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"native graph Vector bootstrap has an invalid freshly allocated mapping-root location");
			}
			const PageLocation &location = derivedIndexBatches[mutationOrdinal][poolOrdinal]
												   .appended_locations[reference.appended_page_ordinal];
			return IndexRootLocation{indexId, location.page_id, location.generation};
		};
		for (const NativeGraphVectorBootstrapMutation &mutation: derivedState.vector_bootstraps) {
			NativeGraphVectorBootstrapMaterialization materialization;
			materialization.mutation = mutation;
			materialization.node_mapping_root = resolveFreshDerivedRoot(mutation.index_id);
			materialization.vector_node_mapping_root = resolveFreshDerivedRoot(mutation.vector_node_mapping_index_id);
			for (uint64_t offset = 0U; offset < mutation.vector_count; ++offset) {
				if (batch.allocateLogicalVectorId() != mutation.first_vector_id + offset) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph Vector bootstrap lost its reserved logical Vector-ID range");
				}
			}
			const size_t rawPoolCount =
					(static_cast<size_t>(mutation.raw_page_count) - 1U) / RSS_PAGE_POOL_DATA_FRAME_COUNT + 1U;
			materialization.raw_pools.reserve(rawPoolCount);
			materialization.raw_pages.reserve(mutation.raw_page_count);
			for (size_t poolOrdinal = 0U; poolOrdinal < rawPoolCount; ++poolOrdinal) {
				CatalogEntry pool;
				pool.segment_id = batch.allocateLogicalSegmentId();
				pool.physical_slot = reserveDistinctNewPoolSlot(
						mutation.first_slot, mutation.last_slot,
						"native graph Vector bootstrap has no free physical slot for a Raw pool");
				pool.kind = SegmentKind::SGK_VECTOR_RAW_POOL;
				pool.owner_id = mutation.index_id;
				pool.generation = Generation(1U);
				pool.state = SegmentState::SGS_SEALED;
				pool.committed_lsn = checkpointLsn;
				if (catalog_.findOptional(current, pool.segment_id).has_value()) { // the writer-held batch allocates a fresh monotonic logical Segment ID.
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph Vector bootstrap allocated a published Raw-pool Segment ID");
				}
				batch.reserveDataSlot(pool.physical_slot);
				materialization.raw_pools.push_back(pool);
				newVectorBootstrapPoolEntries.push_back(pool);
			}
			for (size_t pageOrdinal = 0U; pageOrdinal < mutation.raw_page_count; ++pageOrdinal) {
				const PageId pageId = batch.allocateLogicalPageId();
						if (locations_.findPageOptional(current, pageId).has_value() || // vector delta Page IDs are allocated once by the authenticated batch.
					!pageIds.insert(pageId.value()).second) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph Vector bootstrap allocated a published Raw Page ID");
				}
				const CatalogEntry &pool = materialization.raw_pools[pageOrdinal / RSS_PAGE_POOL_DATA_FRAME_COUNT];
				materialization.raw_pages.push_back({pageId, pool.segment_id, 0U, pool.generation, 0U});
			}
			materialization.manifest_pool.segment_id = batch.allocateLogicalSegmentId();
			materialization.manifest_pool.physical_slot = reserveDistinctNewPoolSlot(
					mutation.first_slot, mutation.last_slot,
					"native graph Vector bootstrap has no free physical slot for a Manifest pool");
			materialization.manifest_pool.kind = SegmentKind::SGK_VECTOR_MANIFEST_POOL;
			materialization.manifest_pool.owner_id = mutation.index_id;
			materialization.manifest_pool.generation = Generation(1U);
			materialization.manifest_pool.state = SegmentState::SGS_SEALED;
			materialization.manifest_pool.committed_lsn = checkpointLsn;
			if (catalog_.findOptional(current, materialization.manifest_pool.segment_id).has_value()) { // the writer-held batch allocates a fresh monotonic logical Segment ID.
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph Vector bootstrap allocated a published Manifest-pool Segment ID");
			}
			batch.reserveDataSlot(materialization.manifest_pool.physical_slot);
			materialization.manifest_page.page_id = batch.allocateLogicalPageId();
			materialization.manifest_page.segment_id = materialization.manifest_pool.segment_id;
			materialization.manifest_page.generation = materialization.manifest_pool.generation;
			if (locations_.findPageOptional(current, materialization.manifest_page.page_id).has_value() || // the manifest Page ID is allocated once by the authenticated vector plan.
				!pageIds.insert(materialization.manifest_page.page_id.value()).second) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph Vector bootstrap allocated a published Manifest Page ID");
			}
			materialization.manifest_location = {mutation.index_id, materialization.manifest_page.page_id,
												 materialization.manifest_page.generation};
			newVectorBootstrapPoolEntries.push_back(materialization.manifest_pool);
			vectorBootstrapMaterializations.push_back(std::move(materialization));
		}
		for (const NativeGraphVectorDeltaMutation *mutation: vectorDeltasByVectorRange) {
			for (uint64_t offset = 0U; offset < mutation->inserted_vector_count; ++offset) {
				if (batch.allocateLogicalVectorId() != mutation->first_vector_id + offset) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph Vector delta lost its reserved logical Vector-ID range");
				}
			}
		}
		std::vector<NativeGraphVectorDeltaMaterialization> vectorDeltaMaterializations;
		std::vector<CatalogEntry> newVectorDeltaPoolEntries;
		vectorDeltaMaterializations.reserve(derivedState.vector_deltas.size());
		for (const NativeGraphVectorDeltaMutation &mutation: derivedState.vector_deltas) {
			NativeGraphVectorDeltaMaterialization materialization;
			materialization.mutation = mutation;
			std::map<uint32_t, size_t> rawNewPoolOrdinals;
			for (const NativeGraphVectorDeltaRawPageMutation &rawMutation: mutation.raw_pages) {
				NativeGraphVectorDeltaRawPageMaterialization raw;
				raw.mutation = rawMutation;
				if (rawMutation.placement == NativeGraphVectorDeltaRawPlacement::NGVDRP_COW_TAIL) {
					raw.pool = rawMutation.expected_pool;
					raw.location = rawMutation.expected_location;
				} else if (rawMutation.placement == NativeGraphVectorDeltaRawPlacement::NGVDRP_APPEND_TO_CURRENT_POOL) {
					raw.pool = rawMutation.expected_pool;
					const PageId pageId = batch.allocateLogicalPageId();
						if (locations_.findPageOptional(current, pageId).has_value() || // vector delta Page IDs are allocated once by the authenticated batch.
						!pageIds.insert(pageId.value()).second) {
						throw FormatException(FormatError::FERR_INVALID_STATE,
											  "native graph Vector delta allocated a published Raw Page ID");
					}
					raw.location = {pageId, raw.pool.segment_id, 0U, raw.pool.generation, 0U};
				} else {
					auto [position, inserted] = rawNewPoolOrdinals.try_emplace(rawMutation.new_pool_ordinal,
																			   materialization.raw_new_pools.size());
					if (inserted) {
						CatalogEntry pool;
						pool.segment_id = batch.allocateLogicalSegmentId();
						pool.physical_slot = reserveDistinctNewPoolSlot(
								mutation.first_slot, mutation.last_slot,
								"native graph Vector delta has no free physical slot for a Raw pool");
						pool.kind = SegmentKind::SGK_VECTOR_RAW_POOL;
						pool.owner_id = mutation.index_id;
						pool.generation = Generation(1U);
						pool.state = SegmentState::SGS_SEALED;
						pool.committed_lsn = checkpointLsn;
						if (catalog_.findOptional(current, pool.segment_id).has_value()) { // the writer-held batch allocates a fresh monotonic logical Segment ID.
							throw FormatException(
									FormatError::FERR_INVALID_STATE,
									"native graph Vector delta allocated a published Raw-pool Segment ID");
						}
						batch.reserveDataSlot(pool.physical_slot);
						newVectorDeltaPoolEntries.push_back(pool);
						materialization.raw_new_pools.push_back(pool);
					}
					raw.pool = materialization.raw_new_pools[position->second];
					const PageId pageId = batch.allocateLogicalPageId();
						if (locations_.findPageOptional(current, pageId).has_value() || // appended Index Page IDs are allocated once by the authenticated batch.
						!pageIds.insert(pageId.value()).second) {
						throw FormatException(FormatError::FERR_INVALID_STATE,
											  "native graph Vector delta allocated a published fresh Raw Page ID");
					}
					raw.location = {pageId, raw.pool.segment_id, 0U, raw.pool.generation, 0U};
				}
				materialization.raw_pages.push_back(std::move(raw));
			}
			if (mutation.manifest_in_new_pool) {
				materialization.manifest_new_pool.segment_id = batch.allocateLogicalSegmentId();
				materialization.manifest_new_pool.physical_slot = reserveDistinctNewPoolSlot(
						mutation.first_slot, mutation.last_slot,
						"native graph Vector delta has no free physical slot for a Manifest pool");
				materialization.manifest_new_pool.kind = SegmentKind::SGK_VECTOR_MANIFEST_POOL;
				materialization.manifest_new_pool.owner_id = mutation.index_id;
				materialization.manifest_new_pool.generation = Generation(1U);
				materialization.manifest_new_pool.state = SegmentState::SGS_SEALED;
				materialization.manifest_new_pool.committed_lsn = checkpointLsn;
				if (catalog_.findOptional(current, materialization.manifest_new_pool.segment_id).has_value()) { // the writer-held batch allocates a fresh monotonic logical Segment ID.
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph Vector delta allocated a published Manifest-pool Segment ID");
				}
				batch.reserveDataSlot(materialization.manifest_new_pool.physical_slot);
				newVectorDeltaPoolEntries.push_back(materialization.manifest_new_pool);
			}
			const CatalogEntry &manifestPool =
					mutation.manifest_in_new_pool ? materialization.manifest_new_pool : mutation.expected_manifest_pool;
			materialization.manifest_replacement = {mutation.expected_manifest_location.page_id,
													manifestPool.segment_id, 0U, manifestPool.generation, 0U};
			materialization.manifest_location = {mutation.index_id, materialization.manifest_replacement.page_id,
												 materialization.manifest_replacement.generation};
			vectorDeltaMaterializations.push_back(std::move(materialization));
		}
		std::vector<CatalogEntry> allNewPoolEntries;
		allNewPoolEntries.reserve(newPoolEntries.size() + freshPropertyReplacementPoolEntries.size() +
								  newPropertyPoolEntries.size() + newLargeValuePoolEntries.size() +
								  newDerivedIndexPoolEntries.size() + newVectorBootstrapPoolEntries.size() +
								  newVectorDeltaPoolEntries.size());
		allNewPoolEntries.insert(allNewPoolEntries.end(), newPoolEntries.begin(), newPoolEntries.end());
		allNewPoolEntries.insert(allNewPoolEntries.end(), freshPropertyReplacementPoolEntries.begin(),
								 freshPropertyReplacementPoolEntries.end());
		allNewPoolEntries.insert(allNewPoolEntries.end(), newPropertyPoolEntries.begin(), newPropertyPoolEntries.end());
		allNewPoolEntries.insert(allNewPoolEntries.end(), newLargeValuePoolEntries.begin(),
								 newLargeValuePoolEntries.end());
		allNewPoolEntries.insert(allNewPoolEntries.end(), newDerivedIndexPoolEntries.begin(),
								 newDerivedIndexPoolEntries.end());
		allNewPoolEntries.insert(allNewPoolEntries.end(), newVectorBootstrapPoolEntries.begin(),
								 newVectorBootstrapPoolEntries.end());
		allNewPoolEntries.insert(allNewPoolEntries.end(), newVectorDeltaPoolEntries.begin(),
								 newVectorDeltaPoolEntries.end());
		for (const CatalogEntry &entry: allNewPoolEntries) {
			// Early streamed coordinators may have prepared a fresh pool under the
			// same MetadataWriteBatch. Its allocator transition is already part of
			// the candidate root, so never replay that transition.
			if (!allocator_.isAllocated(batch.candidate(), entry.physical_slot)) {
				allocator_.prepareSetAllocated(batch, entry.physical_slot, true);
			}
		}
		if (!allNewPoolEntries.empty()) {
			std::ranges::sort(allNewPoolEntries, {}, &CatalogEntry::segment_id);
			catalog_.prepareUpsert(batch, allNewPoolEntries);
		}
		for (size_t mutationOrdinal = 0U; mutationOrdinal < effectiveIndexMutations.size(); ++mutationOrdinal) {
			const NativeGraphIndexTreeMutation &mutation = effectiveIndexMutations[mutationOrdinal];
			std::vector<IndexTreeMutationPoolBatch> &batches = derivedIndexBatches[mutationOrdinal];
			for (size_t poolOrdinal = 0U; poolOrdinal < mutation.pool_requests.size(); ++poolOrdinal) {
				IndexTreeMutationPoolBatch &pool = batches[poolOrdinal];
				const size_t appendedCount = mutation.pool_requests[poolOrdinal].appended_page_count;
				pool.appended_page_ids.reserve(appendedCount);
				pool.appended_locations.reserve(appendedCount);
				for (size_t pageOrdinal = 0U; pageOrdinal < appendedCount; ++pageOrdinal) {
					const PageId pageId = batch.allocateLogicalPageId();
					if (locations_.findPageOptional(current, pageId).has_value() || // appended Index Page IDs are allocated once by the authenticated batch.
						!pageIds.insert(pageId.value()).second) {
						throw FormatException(
								FormatError::FERR_INVALID_STATE,
								"native graph derived-state mutation allocated a published appended Index Page ID");
					}
					pool.appended_page_ids.push_back(pageId);
					pool.appended_locations.push_back({pageId, pool.pool.segment_id, 0U, pool.pool.generation, 0U});
				}
			}
		}

		const std::vector<EntityPageReplacementMaterialization> plannedMaterializations = materializations;
		const std::vector<EntityPageAppendBatchMaterialization> plannedAppendMaterializations = appendMaterializations;
		const std::vector<EntityPageNewPoolBatchMaterialization> plannedNewPoolMaterializations =
				newPoolMaterializations;
		const std::vector<NativeGraphPropertyPageReplacementMaterialization> plannedPropertyMaterializations =
				propertyMaterializations;
		const std::vector<NativeGraphPropertyPageBatchMaterialization> plannedPropertyPageBatchMaterializations =
				propertyPageBatchMaterializations;
		const std::vector<LargeValueMaterializationResult> plannedLargeValueMaterializations =
				largeValueMaterializations;
		const std::vector<std::vector<IndexTreeMutationPoolBatch>> plannedDerivedIndexBatches = derivedIndexBatches;
		const std::vector<NativeGraphVectorBootstrapMaterialization> plannedVectorBootstrapMaterializations =
				vectorBootstrapMaterializations;
		if (hasLargeValueRetirements) {
			largeValueRetirementPreparer(largeValueRetirementBatches);
		}
		{
			debug::ScopedPerfTimer preparationTimer("native_graph.mutation.prepare_materializations");
			preparer(materializations, appendMaterializations, newPoolMaterializations, propertyMaterializations,
					 propertyPageBatchMaterializations, largeValueMaterializations);
		}
		if (!vectorBootstrapMaterializations.empty()) {
			vectorBootstrapPreparer(vectorBootstrapMaterializations);
		}
		try {
			if (privateSegmentSet != nullptr && privateSegmentPreparer) {
				privateSegmentPreparer(*privateSegmentSet, candidateReader);
			}
			if (!derivedState.empty()) {
				const size_t ordinaryCount = derivedState.index_tree_mutations.size();
				std::vector<std::vector<IndexTreeMutationPoolBatch>> ordinaryBatches(
						std::make_move_iterator(derivedIndexBatches.begin()),
						std::make_move_iterator(derivedIndexBatches.begin() + static_cast<ptrdiff_t>(ordinaryCount)));
				derivedStatePreparer(ordinaryBatches);
				std::move(ordinaryBatches.begin(), ordinaryBatches.end(), derivedIndexBatches.begin());
			}
			const size_t deferredOffset = derivedState.index_tree_mutations.size();
			if (derivedIndexBatches.size() != deferredOffset + deferredIndexPreparers.size()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph deferred Index planner changed its materialization partition");
			}
			for (size_t ordinal = 0U; ordinal < deferredIndexPreparers.size(); ++ordinal) {
				deferredIndexPreparers[ordinal](derivedIndexBatches[deferredOffset + ordinal]);
			}
			if (deferredPlanningSnapshot.has_value()) {
				deferredPlanningSnapshot->release();
			}
			privateScratchState->active = false;
		} catch (...) {
			if (deferredPlanningSnapshot.has_value()) {
				deferredPlanningSnapshot->release();
			}
			privateScratchState->active = false;
			throw;
		}
		if (privateSegmentSet != nullptr) {
			const size_t materializedCandidateCount = privateScratchState->compact_candidate.has_value()
																? privateScratchState->compact_candidate->materialized_entries.size()
																: privateSegmentCount;
			if (materializedCandidateCount > static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
				throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW,
									  "native graph private scratch materialization count exceeds PerfTrace range");
			}
			debug::PerfTrace::addValue("native_graph.streamed.scratch.candidate_segments",
									static_cast<int64_t>(materializedCandidateCount));
			debug::ScopedPerfTimer cleanupTimer("native_graph.mutation.private_scratch_cleanup");
			// Candidate scratch is only a writer-scoped transport. Authenticate every
			// frame before clearing it so allocator-free slots never retain a route-like
			// envelope after the final generation becomes visible.
			std::vector<std::byte> privateScratchCleanupImage(RSS_SEGMENT_SIZE);
			const auto validateCleanupEntry = [&](size_t ordinal, const CatalogEntry &entry) {
				const uint16_t headerBytes = privateHeaderBytesAt(ordinal);
				const uint64_t offset = PhysicalSegmentAllocator::checkedSlotOffset(
						current.data_region_offset, entry.physical_slot);
				const size_t bytesRead = io_->readPages(
						{offset, entry.generation.value()}, privateScratchCleanupImage.data(), privateScratchCleanupImage.size(),
						BufferPoolReadHint::BPRH_SEQUENTIAL);
				if (bytesRead != privateScratchCleanupImage.size()) {
					throw FormatException(FormatError::FERR_BUFFER_TOO_SMALL,
										  "native graph private scratch cleanup found a truncated candidate segment");
				}
				const SegmentEnvelope envelope =
						SegmentEnvelopeCodec::decode(
								std::span<const std::byte>(privateScratchCleanupImage.data(), RSS_ENVELOPE_BYTES));
				const uint8_t invalid = static_cast<uint8_t>(envelope.kind != SegmentKind::SGK_BULK_SCRATCH) |
										static_cast<uint8_t>(envelope.state != SegmentState::SGS_SEALED) |
										static_cast<uint8_t>(envelope.segment_id != entry.segment_id) |
										static_cast<uint8_t>(envelope.owner_id != entry.owner_id) |
										static_cast<uint8_t>(envelope.generation != entry.generation) |
										static_cast<uint8_t>(envelope.committed_lsn != entry.committed_lsn) |
										static_cast<uint8_t>(envelope.header_bytes != headerBytes) |
										static_cast<uint8_t>(envelope.payload_bytes == 0U) |
										static_cast<uint8_t>(envelope.payload_bytes > RSS_SEGMENT_SIZE - headerBytes);
				if (invalid != 0U) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph private scratch cleanup found a foreign or invalid candidate segment");
				}
			};
			if (privateScratchState->compact_candidate.has_value()) {
				for (const auto &[ordinal, entry]: privateScratchState->compact_candidate->materialized_entries) {
					validateCleanupEntry(ordinal, entry);
				}
			} else {
				for (size_t ordinal = 0U; ordinal < privateSegmentCount; ++ordinal) {
					validateCleanupEntry(ordinal, privateEntryAt(ordinal));
				}
			}
			const std::array<std::byte, RSS_SEGMENT_SIZE> zeros{};
			const auto clearCleanupEntry = [&](const CatalogEntry &entry) {
				const uint64_t offset = PhysicalSegmentAllocator::checkedSlotOffset(
						current.data_region_offset, entry.physical_slot);
				io_->writePagesThrough({offset, entry.generation.value()}, zeros.data(), zeros.size());
			};
			if (privateScratchState->compact_candidate.has_value()) {
				for (const auto &[ordinal, entry]: privateScratchState->compact_candidate->materialized_entries) {
					static_cast<void>(ordinal);
					clearCleanupEntry(entry);
				}
			} else {
				for (size_t ordinal = 0U; ordinal < privateSegmentCount; ++ordinal) {
					clearCleanupEntry(privateEntryAt(ordinal));
				}
			}
		}
		debug::ScopedPerfTimer materializationTimer("native_graph.mutation.finalize_materializations");
		std::set<std::pair<uint64_t, uint8_t>> destinationFrames;
        // Private images participate in the same final destination validation
        // as every regular Entity/Property/Index materializer.
        const auto registerImageFrames = [&](const NativeGraphDeferredIndexImages &proof) {
            for (const auto &page : proof.installed_pages_)
                if (!destinationFrames.emplace(page.segment_id.value(), page.page_index).second)
                    throw FormatException(FormatError::FERR_INVALID_STATE, "index image destinations overlap");
        };
        for (const auto &proof : derivedState.index_images) registerImageFrames(*proof);
        for (const auto &plan : deferredPlans) if (plan.images) registerImageFrames(*plan.images);
        const auto validateImagePools = [&](const auto &placement) {
            if (!placement) return;
            for (const auto &reservation : placement->reservations_) if (reservation.prepared) {
                const auto found = std::ranges::find(allNewPoolEntries, reservation.pool.segment_id, &CatalogEntry::segment_id);
                if (found != allNewPoolEntries.end() && !hasSameCatalogEntry(*found, reservation.pool))
                    throw FormatException(FormatError::FERR_INVALID_STATE, "index images conflict with another pool identity");
            }
        };
        validateImagePools(ordinaryIndexPlacement); validateImagePools(streamedAdjacencyPlacement);
        validateImagePools(streamedMembershipPlacement);
