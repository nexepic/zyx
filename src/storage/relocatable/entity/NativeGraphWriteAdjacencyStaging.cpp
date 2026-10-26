#include "NativeGraphWritePlannerContext.hpp"

namespace graph::storage {

	void NativeGraphWriteTransaction::stageGlobalAdjacencyMutation(GlobalAdjacencyDirection direction, uint64_t nodeId,
																   uint64_t edgeId,
																   BTreeScalarIndexOperationKind kind) {
		if ((direction != GlobalAdjacencyDirection::GAD_OUT && direction != GlobalAdjacencyDirection::GAD_IN) || // this private helper is called only by Edge create/erase paths with canonical directions, nonzero identities, and key-only operation kinds.
			nodeId == 0U || edgeId == 0U ||
			(kind != BTreeScalarIndexOperationKind::BSIOP_UPSERT &&
			 kind != BTreeScalarIndexOperationKind::BSIOP_ERASE)) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "native graph global adjacency mutation requires canonical key and operation identities");
		}
		requireAdjacencyOperationCapacity(global_adjacency_operations_.size(), 1U,
										  context_->options.maximum_global_adjacency_write_entries);
		global_adjacency_operations_.push_back({kind, AdjacencyKeyCodec::encode({direction, nodeId, edgeId}), 0U});
		global_adjacency_operations_canonical_ = false;
		global_adjacency_plan_.reset(); global_adjacency_images_.reset();
		global_adjacency_retirement_plan_.reset();
		global_adjacency_initial_plan_.reset();
		global_adjacency_deferred_bulk_ = false;
	}

	const std::vector<BTreeScalarIndexOperation> &NativeGraphWriteTransaction::canonicalGlobalAdjacencyOperations() {
		if (global_adjacency_operations_canonical_) {
			return global_adjacency_operations_;
		}
		const auto less = [](const BTreeScalarIndexOperation &left, const BTreeScalarIndexOperation &right) {
			return std::lexicographical_compare(left.key.begin(), left.key.end(), right.key.begin(), right.key.end());
		};
		// stable_sort retains statement order inside an equal-key group, preserving
		// the only cancellable sequence: provisional UPSERT followed by ERASE.
		std::stable_sort(global_adjacency_operations_.begin(), global_adjacency_operations_.end(), less);
		for (const BTreeScalarIndexOperation &operation: global_adjacency_operations_) {
			if (operation.value != 0U || operation.key.empty() || // this private vector is populated only by stageGlobalAdjacencyMutation with a typed key and zero value.
				(operation.kind != BTreeScalarIndexOperationKind::BSIOP_UPSERT &&
				 operation.kind != BTreeScalarIndexOperationKind::BSIOP_ERASE)) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native graph global adjacency operation stream is not key-only and canonical");
			}
			static_cast<void>(AdjacencyKeyCodec::decode(operation.key));
		}
		size_t output = 0U;
		for (size_t input = 0U; input < global_adjacency_operations_.size();) {
			size_t next = input + 1U;
			while (next < global_adjacency_operations_.size() &&
				   !less(global_adjacency_operations_[input], global_adjacency_operations_[next]) &&
				   !less(global_adjacency_operations_[next], global_adjacency_operations_[input])) {
				++next;
			}
			if (next - input == 1U) {
				// A unique group normally stays at the same index. Moving a vector to
				// itself is not a no-op and may empty its key, so compact only when a
				// preceding cancelled group has actually created a gap.
				if (output != input) {
					global_adjacency_operations_[output] = std::move(global_adjacency_operations_[input]);
				}
				++output;
			} else if (next - input == 2U && // erasing a provisional Edge removes its two staged upserts directly, so the public API cannot retain a duplicate-key pair for this defensive canonicalizer.
					   global_adjacency_operations_[input].kind == BTreeScalarIndexOperationKind::BSIOP_UPSERT &&
					   global_adjacency_operations_[input + 1U].kind == BTreeScalarIndexOperationKind::BSIOP_ERASE) {
				// A provisional Edge created and removed in one transaction leaves no
				// durable adjacency key or empty-root publication behind.
			} else { // public Edge mutations emit at most one final-state operation per directional key after provisional cancellation.
				fail(FormatError::FERR_INVALID_STATE,
					 "native graph transaction stages contradictory duplicate global adjacency keys");
			}
			input = next;
		}
		global_adjacency_operations_.resize(output);
		global_adjacency_operations_canonical_ = true;
		return global_adjacency_operations_;
	}

	void NativeGraphWriteTransaction::prepareGlobalAdjacencyMutationPlan() {
		global_adjacency_plan_.reset(); global_adjacency_images_.reset();
		global_adjacency_retirement_plan_.reset();
		global_adjacency_initial_plan_.reset();
		if (global_adjacency_operations_.empty()) {
			return;
		}
		const std::vector<BTreeScalarIndexOperation> &operations = canonicalGlobalAdjacencyOperations();
		if (operations.empty()) { // provisional Edge deletion removes its keys before planning; any other nonempty staged vector remains nonempty after canonicalization.
			return;
		}
		if (operations.size() > context_->options.maximum_global_adjacency_write_entries) { // every addition passes requireAdjacencyOperationCapacity with this same bound.
			fail(FormatError::FERR_INVALID_STATE,
				 "native graph global adjacency mutation exceeds its configured entry bound");
		}
		if (context_->metadata->findIndexRootOptional(snapshot_, RSS_GLOBAL_ADJACENCY_INDEX_ID).has_value()) {
			if (operations.size() >= context_->options.global_adjacency_bulk_rebuild_entry_threshold) {
				global_adjacency_deferred_bulk_ = true;
				return;
			}
            const auto memory = utils::OwnedAllocationScope::owner();
            if (memory && operations.size() < NativeGraphWritePlannerOptions::DEFAULT_GLOBAL_ADJACENCY_BULK_REBUILD_ENTRY_THRESHOLD) {
                auto proof = std::shared_ptr<NativeGraphDeferredIndexImages>(
                    new (memory) NativeGraphDeferredIndexImages(*context_->metadata, snapshot_.superblock().generation,
                        RSS_GLOBAL_ADJACENCY_INDEX_ID, context_->metadata->findIndexRoot(snapshot_, RSS_GLOBAL_ADJACENCY_INDEX_ID),
                        memory, context_->options.first_slot, context_->options.last_slot),
                    std::default_delete<NativeGraphDeferredIndexImages>(), utils::OwnedPmrAllocator<std::byte>(memory));
                proof->incremental_ = true;
                proof->plan_ = context_->indexes->planGlobalAdjacencyImagesFromSource(snapshot_, operations.size(),
                    [&](const std::function<void(const BTreeScalarIndexOperation &)> &emit) {
                        for (const auto &operation : operations) emit(operation);
                    }, memory, [target = proof.get()](BTreePage page) { target->images_.push_back(std::move(page)); });
                if (proof->plan_.final_tree_empty) {
                    // Terminal adjacency deletion still removes the root using
                    // the existing bounded, authenticated retirement protocol.
                    global_adjacency_retirement_plan_ = context_->indexes->planTreeRetirement(snapshot_,
                        RSS_GLOBAL_ADJACENCY_INDEX_ID, context_->options.maximum_global_adjacency_tree_pages);
                    return;
                }
                proof->assignments_credit_.set(16384U +
                    (proof->plan_.pool_requests.size() + proof->plan_.additional_new_pool_mutations.size() +
                        size_t(bool(proof->plan_.new_pool_mutation))) * 8192U);
                global_adjacency_images_ = std::move(proof);
                debug::PerfTrace::addValue("native_graph.write.ordinary_adjacency.incremental_plans", 1);
                return;
            }
			global_adjacency_plan_ = context_->indexes->planScalarIndexTransactionMutation(
					snapshot_, RSS_GLOBAL_ADJACENCY_INDEX_ID, operations, true, false,
					BTreeLeafValueLayout::BLVL_EMPTY);
			if (global_adjacency_plan_->final_tree_empty) {
				global_adjacency_retirement_plan_ =
						context_->indexes->planTreeRetirement(snapshot_, RSS_GLOBAL_ADJACENCY_INDEX_ID,
															  context_->options.maximum_global_adjacency_tree_pages);
				global_adjacency_plan_.reset(); global_adjacency_images_.reset();
			}
			return;
		}
		for (const BTreeScalarIndexOperation &operation: operations) {
			if (operation.kind != BTreeScalarIndexOperationKind::BSIOP_UPSERT) { // a persisted Edge always has an existing global adjacency root; a new tree receives only Edge-create upserts.
				fail(FormatError::FERR_INVALID_STATE,
					 "native graph cannot erase a global adjacency key from an absent tree");
			}
		}
		if (operations.size() >= context_->options.global_adjacency_bulk_rebuild_entry_threshold) {
			// A large first tree can be built from a private, same-file run after the
			// coordinator owns the writer. This removes the second B+Tree-entry copy
			// from the commit's planning phase while preserving one atomic publication.
			global_adjacency_deferred_bulk_ = true;
			return;
		}
		// Keep the first-tree bulk source transaction-owned as well. This avoids a
		// second entry/key allocation while preserving the builder's rewindable
		// source contract through the enclosing commit.
		const auto *initialOperations = &global_adjacency_operations_;
		const OrderedBTreeEntrySource source = [initialOperations] {
			auto ordinal = std::make_shared<size_t>(0U);
			return [initialOperations, ordinal]() -> std::optional<BTreePageEntry> {
				if (*ordinal == initialOperations->size()) {
					return std::nullopt;
				}
				return BTreePageEntry{(*initialOperations)[(*ordinal)++].key, 0U};
			};
		};
		BulkBTreeBuildOptions options;
		options.first_slot = context_->options.first_slot;
		options.last_slot = context_->options.last_slot;
		options.pool_owner_id = RSS_GLOBAL_ADJACENCY_INDEX_ID;
		options.maximum_entries = maximumAdjacencyTreeEntries(context_->options);
		options.maximum_pages = context_->options.maximum_global_adjacency_tree_pages;
		options.leaf_value_layout = BTreeLeafValueLayout::BLVL_EMPTY;
		global_adjacency_initial_plan_ = BulkBTreeBuilder::planNativeTree(
				*context_->indexes, *context_->metadata, snapshot_, RSS_GLOBAL_ADJACENCY_INDEX_ID, source, options);
	}

	bool NativeGraphWriteTransaction::rebaseAdjacencyOnly(MetadataSnapshotPin &current) {
		if (!current.isValid() || current.superblock().hybrid_property_descriptor_root != snapshot_.superblock().hybrid_property_descriptor_root ||
            next_vector_id_ != snapshot_.superblock().next_logical_vector_id ||
			!scalar_index_mutations_.empty() || !initial_scalar_index_mutations_.empty() ||
			!vector_bootstraps_.empty() || !vector_deltas_.empty()) {
			return false;
		}

		std::vector<uint64_t> nodeIds;
		nodeIds.reserve(nodes_.size());
		for (const auto &[nodeId, change]: nodes_) {
			static_cast<void>(change);
			nodeIds.push_back(nodeId);
		}
		const std::vector<std::optional<NodeEntityRecord>> nodeRecords =
				nodeIds.empty() ? std::vector<std::optional<NodeEntityRecord>>()
									: context_->entities->readNodesSorted(current, nodeIds);
		if (nodeRecords.size() != nodeIds.size()) {
			fail(FormatError::FERR_INVALID_STATE, "native graph adjacency rebase changed Node batch cardinality");
		}
		const auto nodeRecordFor = [&](uint64_t nodeId) -> const std::optional<NodeEntityRecord> & {
			const auto iterator = std::ranges::lower_bound(nodeIds, nodeId);
			if (iterator == nodeIds.end() || *iterator != nodeId) {
				fail(FormatError::FERR_INVALID_STATE, "native graph adjacency rebase lost a Node mutation ID");
			}
			return nodeRecords[static_cast<size_t>(iterator - nodeIds.begin())];
		};
		for (const auto &[nodeId, change]: nodes_) {
			if (change.erased || (change.labels.has_value() && !change.inserted)) {
				return false;
			}
			if (change.inserted) {
				if (nodeRecordFor(nodeId).has_value()) {
					return false;
				}
			} else if (!nodeRecordFor(nodeId).has_value() || *nodeRecordFor(nodeId) != change.record) {
				return false;
			}
		}

		size_t erasedEdgeCount = 0U;
		std::vector<uint64_t> edgeIds;
		edgeIds.reserve(edges_.size());
		for (const auto &[edgeId, change]: edges_) {
			static_cast<void>(change);
			edgeIds.push_back(edgeId);
		}
		const std::vector<std::optional<EdgeEntityRecord>> edgeRecords =
				edgeIds.empty() ? std::vector<std::optional<EdgeEntityRecord>>()
										: context_->entities->readEdgesSorted(current, edgeIds);
		if (edgeRecords.size() != edgeIds.size()) {
			fail(FormatError::FERR_INVALID_STATE, "native graph adjacency rebase changed Edge batch cardinality");
		}
		const auto edgeRecordFor = [&](uint64_t edgeId) -> const std::optional<EdgeEntityRecord> & {
			const auto iterator = std::ranges::lower_bound(edgeIds, edgeId);
			if (iterator == edgeIds.end() || *iterator != edgeId) {
				fail(FormatError::FERR_INVALID_STATE, "native graph adjacency rebase lost an Edge mutation ID");
			}
			return edgeRecords[static_cast<size_t>(iterator - edgeIds.begin())];
		};
		for (const auto &[edgeId, change]: edges_) {
			const EntityPropertyKey propertyKey{EntityPropertyOwnerKind::EPOK_EDGE, edgeId};
			const auto property = properties_.find(propertyKey.directoryId());
			if (change.inserted) {
				if (property != properties_.end() || edgeRecordFor(edgeId).has_value()) {
					return false;
				}
				continue;
			}
			if (!change.erased || property == properties_.end() || property->second.owner_entity_id != edgeId ||
				property->second.owner_entity_type != Edge::typeId ||
				!property->second.entries.empty() ||
				!edgeRecordFor(edgeId).has_value() || *edgeRecordFor(edgeId) != change.record) {
				return false;
			}
			++erasedEdgeCount;
		}
		if (properties_.size() != erasedEdgeCount) {
			return false;
		}

		const std::vector<BTreeScalarIndexOperation> &operations = canonicalGlobalAdjacencyOperations();
		if (operations.size() != edges_.size() * 2U ||
			std::ranges::any_of(operations, [](const BTreeScalarIndexOperation &operation) {
				return (operation.kind != BTreeScalarIndexOperationKind::BSIOP_UPSERT &&
						operation.kind != BTreeScalarIndexOperationKind::BSIOP_ERASE) ||
					   operation.value != 0U;
			})) {
			return false;
		}

		// All source fences have been revalidated before changing the transaction's
		// planning snapshot. Subsequent COW plans therefore target the current root.
		snapshot_.release();
		snapshot_ = std::move(current);
		return true;
	}

	void NativeGraphWriteTransaction::eraseEdge(uint64_t edgeId) {
		requireActive("erase an Edge");
		const auto staged = edges_.find(edgeId);
		if (staged != edges_.end() && staged->second.inserted) {
			const EdgeEntityRecord record = staged->second.record;
			const std::array<std::vector<std::byte>, 2U> provisionalKeys = {
					AdjacencyKeyCodec::encode({GlobalAdjacencyDirection::GAD_OUT, record.source_node_id, edgeId}),
					AdjacencyKeyCodec::encode({GlobalAdjacencyDirection::GAD_IN, record.target_node_id, edgeId})};
			const auto eraseProvisionalKey = [this](const std::vector<std::byte> &key) {
				const auto operation = std::ranges::find_if(
						global_adjacency_operations_, [&key](const BTreeScalarIndexOperation &candidate) {
							return candidate.kind == BTreeScalarIndexOperationKind::BSIOP_UPSERT &&
								   candidate.value == 0U && candidate.key == key;
						});
				if (operation == global_adjacency_operations_.end()) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native graph write lost a provisional global adjacency key for a newly created Edge");
				}
				global_adjacency_operations_.erase(operation);
			};
			eraseProvisionalKey(provisionalKeys[0U]);
			eraseProvisionalKey(provisionalKeys[1U]);
			global_adjacency_operations_canonical_ = false;
			global_adjacency_plan_.reset(); global_adjacency_images_.reset();
			global_adjacency_retirement_plan_.reset();
			global_adjacency_initial_plan_.reset();
			properties_.erase(EntityPropertyKey{EntityPropertyOwnerKind::EPOK_EDGE, edgeId}.directoryId());
			edges_.erase(staged);
			return;
		}
		// Removing an Edge must stage both exact keys or neither key. Check before
		// mutableEdge records an erase candidate in the transaction overlay.
		requireAdjacencyOperationCapacity(global_adjacency_operations_.size(), 2U,
										  context_->options.maximum_global_adjacency_write_entries);
		EdgeChange &change = mutableEdge(edgeId);
		const EdgeEntityRecord record = change.record;
		stageGlobalAdjacencyMutation(GlobalAdjacencyDirection::GAD_OUT, record.source_node_id, edgeId,
									 BTreeScalarIndexOperationKind::BSIOP_ERASE);
		stageGlobalAdjacencyMutation(GlobalAdjacencyDirection::GAD_IN, record.target_node_id, edgeId,
									 BTreeScalarIndexOperationKind::BSIOP_ERASE);
		change.erased = true;
		// Keep deletion in the owner-keyed Property batch. Removing only the
		// directory entry would leave the root slot reachable in its Property page.
		properties_.insert_or_assign(EntityPropertyKey{EntityPropertyOwnerKind::EPOK_EDGE, edgeId}.directoryId(),
									 PropertyCollectionRoot{edgeId, Edge::typeId, {}});
	}

	bool NativeGraphWriteTransaction::hasRemainingIncidentEdge(uint64_t nodeId) const {
		return nodeHasIncidentEdges(nodeId);
	}

	void NativeGraphWriteTransaction::eraseNode(uint64_t nodeId) {
		requireActive("erase a Node");
		NodeChange &change = mutableNode(nodeId);
		if (change.inserted) {
			if (hasRemainingIncidentEdge(nodeId)) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native graph write rejects Node deletion while an incident Edge remains");
			}
			properties_.erase(EntityPropertyKey{EntityPropertyOwnerKind::EPOK_NODE, nodeId}.directoryId());
			nodes_.erase(nodeId);
			return;
		}
		if (hasRemainingIncidentEdge(nodeId)) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native graph write rejects Node deletion while an incident Edge remains");
		}
		change.erased = true;
		// An empty root is an explicit page-local erase plan, published atomically
		// with the Entity Map deletion rather than leaving an orphaned page slot.
		properties_.insert_or_assign(EntityPropertyKey{EntityPropertyOwnerKind::EPOK_NODE, nodeId}.directoryId(),
									 PropertyCollectionRoot{nodeId, Node::typeId, {}});
	}

	void NativeGraphWriteTransaction::prepareIndexMutationPlans(
			LargeValueContentIndexPublicationPlan *contentIndexPlan,
			const std::function<void()> &forceFreshContentIndexPlan) {
		struct PoolDemand {
			BTreePageStore *reclaimer = nullptr;
			SegmentId segment_id{};
			size_t required_frames = 0U;
		};
		struct Candidate {
			BTreePageStore *reclaimer = nullptr;
			std::function<const std::vector<IndexTreeMutationPoolRequest> &()> requests;
			std::function<void()> force_fresh;
		};
		using PoolKey = uint64_t;
		std::map<PoolKey, PoolDemand> demands;
		std::map<PoolKey, std::vector<std::pair<size_t, size_t>>> contributions;
		std::vector<Candidate> candidates;
		for (size_t stagedOrdinal = 0U; stagedOrdinal < scalar_index_mutations_.size(); ++stagedOrdinal) {
			StagedScalarIndexMutation &staged = scalar_index_mutations_[stagedOrdinal];
            staged.images.reset();
			if (staged.index == nullptr || staged.index_id == 0U || // staged scalar mutations retain their validated local index and identity.
				!staged.index->usesMetadataStore(*context_->metadata)) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native graph staged scalar Index operation stream is incomplete or foreign");
			}
			if (staged.retirement_plan.has_value()) {
				const BTreeTreeRetirementPlan &retirement = *staged.retirement_plan;
				if (!staged.operations.empty() || staged.plan.has_value() || !retirement.preparer || // retirement plans are created from one pinned root and cannot coexist with mutation operations.
					retirement.expected_root.index_id != staged.index_id || retirement.pool_requests.empty()) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native graph staged scalar Index retirement plan is incomplete or contradictory");
				}
				continue;
			}
			if (staged.operations.empty()) {
				fail(FormatError::FERR_INVALID_STATE, "native graph staged scalar Index operation stream is empty");
			}
            const auto memory = utils::OwnedAllocationScope::owner();
            if (memory && isSystemCatalogIndexDefinitionId(staged.index_id) && staged.operations.size() <
                NativeGraphWritePlannerOptions::DEFAULT_GLOBAL_ADJACENCY_BULK_REBUILD_ENTRY_THRESHOLD) {
                SystemCatalog schema(*staged.index, *context_->metadata);
                const auto definition = schema.findIndexDefinitionById(snapshot_, staged.index_id);
                if (definition && definition->kind == IndexDefinitionKind::IDK_LABEL &&
                    definition->entity_kind == IndexDefinitionEntityKind::IDEK_EDGE) {
                    auto proof = std::shared_ptr<NativeGraphDeferredIndexImages>(
                        new (memory) NativeGraphDeferredIndexImages(*context_->metadata, snapshot_.superblock().generation,
                            staged.index_id, context_->metadata->findIndexRoot(snapshot_, staged.index_id), memory,
                            context_->options.first_slot, context_->options.last_slot),
                        std::default_delete<NativeGraphDeferredIndexImages>(), utils::OwnedPmrAllocator<std::byte>(memory));
                    proof->incremental_ = true;
                    proof->plan_ = staged.index->planStreamedMembershipImagesFromSource(snapshot_, *definition,
                        staged.operations.size(), [&](const std::function<void(const BTreeScalarIndexOperation &)> &emit) {
                            for (const auto &operation : staged.operations) emit(operation);
                        }, memory, [target = proof.get()](BTreePage page) { target->images_.push_back(std::move(page)); });
                    if (!proof->plan_.final_tree_empty) {
                        proof->assignments_credit_.set(16384U +
                            (proof->plan_.pool_requests.size() + proof->plan_.additional_new_pool_mutations.size() +
                                size_t(bool(proof->plan_.new_pool_mutation))) * 8192U);
                        staged.plan.reset(); staged.images = std::move(proof);
                        debug::PerfTrace::addValue("native_graph.write.ordinary_membership.incremental_plans", 1);
                        continue;
                    }
                    // Empty membership roots keep their ordinary scalar plan.

                }
            }
			staged.plan =
					staged.index->planScalarIndexTransactionMutation(
						snapshot_, staged.index_id, staged.operations, true, false, staged.leaf_value_layout);
			candidates.push_back({staged.index,
					[&staged]() -> const std::vector<IndexTreeMutationPoolRequest> & {
						return staged.plan->pool_requests;
					},
					[this, &staged] {
						staged.plan = staged.index->planScalarIndexTransactionMutation(
								snapshot_, staged.index_id, staged.operations, false, true, staged.leaf_value_layout);
					}});
		}
		if (global_adjacency_plan_.has_value()) {
			candidates.push_back({context_->indexes,
					[this]() -> const std::vector<IndexTreeMutationPoolRequest> & {
						return global_adjacency_plan_->pool_requests;
					},
					[this] {
						global_adjacency_plan_ = context_->indexes->planScalarIndexTransactionMutation(
								snapshot_, RSS_GLOBAL_ADJACENCY_INDEX_ID, canonicalGlobalAdjacencyOperations(), true, true,
								BTreeLeafValueLayout::BLVL_EMPTY);
					}});
		}
		if (contentIndexPlan != nullptr) {
			if (!forceFreshContentIndexPlan) {
				fail(FormatError::FERR_INVALID_STATE,
						"native graph content-index capacity planning lost its fresh-rebuild callback");
			}
			Candidate contentCandidate;
			contentCandidate.reclaimer = &context_->properties->largeValueStore()->contentIndex();
			contentCandidate.requests = [contentIndexPlan]() -> const std::vector<IndexTreeMutationPoolRequest> & {
				return contentIndexPlan->mutation.pool_requests;
			};
			contentCandidate.force_fresh = forceFreshContentIndexPlan;
			candidates.push_back(std::move(contentCandidate));
		}

		const auto addDemand = [&](size_t candidateOrdinal, BTreePageStore *reclaimer, SegmentId segmentId,
								   size_t requiredFrames) {
			if (requiredFrames == 0U) {
				return;
			}
			auto [position, inserted] =
				demands.try_emplace(segmentId.value(), PoolDemand{reclaimer, segmentId, 0U});
			static_cast<void>(inserted);
			if (requiredFrames > std::numeric_limits<size_t>::max() - position->second.required_frames) {
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
						"native graph staged scalar Index frame demand overflows host address space");
			}
			position->second.required_frames += requiredFrames;
			contributions[segmentId.value()].push_back({candidateOrdinal, requiredFrames});
		};
		for (size_t candidateOrdinal = 0U; candidateOrdinal < candidates.size(); ++candidateOrdinal) {
			const Candidate &candidate = candidates[candidateOrdinal];
			for (const IndexTreeMutationPoolRequest &pool: candidate.requests()) {
				// A cross-pool replacement consumes a frame from the destination, while
				// appends and same-pool replacements consume frames from the source.
				// Keep those demands separate so a full source pool does not trigger a
				// needless dense rebuild when a compatible destination has capacity.
				const SegmentId replacementPool = pool.replacement_segment_id.isValid()
						? pool.replacement_segment_id
						: pool.segment_id;
				addDemand(candidateOrdinal, candidate.reclaimer, replacementPool,
						pool.expected_replacements.size());
				addDemand(candidateOrdinal, candidate.reclaimer, pool.segment_id, pool.appended_page_count);
			}
		}

		std::map<PoolKey, size_t> availableFrames;
		for (const auto &[key, demand]: demands) {
			const CatalogEntry currentPool = context_->metadata->findCatalog(snapshot_, demand.segment_id);
			if (demand.reclaimer->hasReplacementFrameCapacity(currentPool, demand.required_frames)) {
				availableFrames.emplace(key, demand.required_frames);
				continue;
			}
			// Reclaim only retired frames invisible to every active snapshot before
			// selecting the immutable fresh-pool fallback.
			demand.reclaimer->reclaimRetiredFrames(demand.segment_id);
			size_t available = 0U;
			while (available < RSS_PAGE_POOL_DATA_FRAME_COUNT &&
				   demand.reclaimer->hasReplacementFrameCapacity(currentPool, available + 1U)) {
				++available;
			}
			availableFrames.emplace(key, available);
		}

		std::set<size_t> forceFreshRebuilds;
		// All candidates are accounted against the same current-pool frame budget;
		// independent trees may safely consume distinct frames in one publication.
		// Only an aggregate shortfall below forces a deterministic fresh rebuild.
		bool selected = true;
		while (selected) {
			selected = false;
			for (const auto &[key, demand]: demands) {
				static_cast<void>(demand);
				const auto available = availableFrames.find(key);
				if (available == availableFrames.end()) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native graph scalar Index capacity scan lost a pool availability result");
				}
				size_t remainingDemand = 0U;
				for (const auto &[candidateOrdinal, requiredFrames]: contributions.at(key)) {
					if (!forceFreshRebuilds.contains(candidateOrdinal)) {
						if (requiredFrames > std::numeric_limits<size_t>::max() - remainingDemand) {
							fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
								 "native graph staged scalar Index shared-pool demand overflows host address space");
						}
						remainingDemand += requiredFrames;
					}
				}
				if (remainingDemand <= available->second) {
					continue;
				}
				std::optional<size_t> selectedOrdinal;
				size_t selectedFrames = 0U;
				for (const auto &[candidateOrdinal, requiredFrames]: contributions.at(key)) {
					if (forceFreshRebuilds.contains(candidateOrdinal)) {
						continue;
					}
					if (!selectedOrdinal.has_value() || requiredFrames > selectedFrames) {
						selectedOrdinal = candidateOrdinal;
						selectedFrames = requiredFrames;
					}
				}
				if (!selectedOrdinal.has_value()) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native graph scalar Index cannot reduce an exhausted shared-pool demand");
				}
				forceFreshRebuilds.insert(*selectedOrdinal);
				selected = true;
			}
		}
		for (const size_t candidateOrdinal: forceFreshRebuilds) {
			if (candidateOrdinal >= candidates.size() || !candidates[candidateOrdinal].force_fresh) { // capacity planning records callbacks from the same candidate vector it replays.
				fail(FormatError::FERR_INVALID_STATE,
						"native graph Index capacity planner lost a fresh-rebuild callback");
			}
			candidates[candidateOrdinal].force_fresh();
			for (const IndexTreeMutationPoolRequest &pool: candidates[candidateOrdinal].requests()) {
				if (!pool.expected_replacements.empty() || pool.appended_page_count != 0U) {
					fail(FormatError::FERR_INVALID_STATE,
							"native graph Index fresh rebuild retained a current-pool frame mutation");
				}
			}
		}
		for (const StagedInitialScalarIndexMutation &staged: initial_scalar_index_mutations_) {
			if (staged.index == nullptr || staged.index_id == 0U || !staged.plan.has_value() || // initial index plans retain their validated local index and preflight output.
				!staged.index->usesMetadataStore(*context_->metadata) || staged.plan->index_id != staged.index_id ||
				staged.plan->new_pool_mutations.empty() || !staged.plan->preparer) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native graph staged initial scalar Index plan is incomplete or foreign");
			}
		}
	}

} // namespace graph::storage
