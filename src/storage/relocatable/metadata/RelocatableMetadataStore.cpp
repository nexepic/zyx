#include "MetadataStoreOperationsInternal.hpp"

namespace graph::storage {

	RelocatableMetadataStore::RelocatableMetadataStore(std::shared_ptr<StorageIO> io, const Superblock &current,
													   bool readOnly, std::shared_ptr<std::pmr::memory_resource> memoryOwner, std::shared_ptr<std::pmr::memory_resource> controlOwner, std::shared_ptr<std::pmr::memory_resource> leaseMemoryOwner,
                                                       std::shared_ptr<MetadataSnapshotCoordination> coordination,
                                                       std::unique_lock<std::shared_mutex> openLock) :
		io_(std::move(io)), bootstrap_(io_), pages_(io_, current),
		metadata_page_pool_(openMetadataPoolWithControlScratch(io_, current, controlOwner)),
		catalog_(pages_, metadata_page_pool_), locations_(pages_, metadata_page_pool_),
		allocator_(pages_, metadata_page_pool_), pool_ownership_(catalog_, allocator_, metadata_page_pool_, readOnly),
		snapshots_(bootstrap_, {}, std::move(memoryOwner), controlOwner, std::move(coordination)), retirement_(catalog_, locations_, snapshots_),
		reservation_coordinator_(catalog_, allocator_, locations_),
		segment_retirement_coordinator_(catalog_, allocator_, locations_, retirement_),
		segment_relocation_coordinator_(catalog_, allocator_, locations_, retirement_),
		retire_queue_reclaimer_(catalog_, allocator_, locations_, retirement_),
        lease_memory_owner_(std::move(leaseMemoryOwner)),
        outstanding_node_id_leases_(lease_memory_owner_ ? lease_memory_owner_.get() : std::pmr::new_delete_resource()),
        outstanding_edge_id_leases_(lease_memory_owner_ ? lease_memory_owner_.get() : std::pmr::new_delete_resource()),
        read_only_(readOnly), synchronized_source_(current) {
        std::optional<utils::OwnedAllocationScope> scratch;
        if (controlOwner) scratch.emplace(controlOwner);
        auto writer = snapshots_.adoptWriteLock(std::move(openLock));
        if (!readOnly) snapshots_.recoverAllocatorUnderWrite(writer, metadata_page_pool_, current);
		pool_ownership_.restoreCatalogOwnership(current);
		pool_ownership_.verifyAfterOpen(current);
        if (current.hybrid_property_descriptor_root.isValid())
            static_cast<void>(pages_.readHybridPropertyDescriptor(current.hybrid_property_descriptor_root, current));
        // A rejected private candidate can leave appends behind a ROW root.
        // Every writable candidate-profile reopen must therefore recover before
        // its first publication, including metadata-only publications.
        native_pool_recovery_required_ = !readOnly && RSS_HYBRID_PROPERTY_FORMAT_ENABLED;
	}

	RelocatableMetadataStore::~RelocatableMetadataStore() = default;

	MetadataWriteGuard RelocatableMetadataStore::beginWrite(bool allowHybridMutation) {
		if (read_only_) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable read-only metadata store cannot publish or reclaim metadata");
		}

        return snapshots_.beginWriteWithAdmission([this, allowHybridMutation](const Superblock &current) {
            if constexpr (!RSS_HYBRID_PROPERTY_FORMAT_ENABLED) return;
            if (!current.hybrid_property_descriptor_root.isValid()) return;
            const auto descriptor = pages_.readHybridPropertyDescriptor(current.hybrid_property_descriptor_root, current);
            if (!allowHybridMutation && std::ranges::any_of(descriptor.descriptor.roots, [](const auto &root) { return root.page_id.isValid(); }))
                throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION,
                    "native hybrid candidate rejects unadapted writes and pool reconciliation");
        }, [this](MetadataWriteGuard &writer) {
            const auto current = bootstrap_.recover();
            if (current != synchronized_source_) {
                // Another handle published under the same file barrier. Rebuild
                // missing local allocation/retirement proof before allocating;
                // every handle's active roots protect its old physical frames.
                io_->discardBufferPoolForExternalRebase();
                snapshots_.recoverAllocatorUnderWrite(writer, metadata_page_pool_, current);
                pool_ownership_.restoreCatalogOwnership(current);
                pool_ownership_.verifyAfterOpen(current);
                pages_.rebindSuperblock(current);
                synchronized_source_ = current;
                debug::PerfTrace::addValue("native_metadata.external_source_refresh", 1);
            }
            if constexpr (!RSS_HYBRID_PROPERTY_FORMAT_ENABLED) return;
            // An optional admission can abandon private appends while its
            // durable source still uses ROW. Clean them before ANY next write.
            if (native_pool_recovery_required_) recoverNativePoolFrames(writer, current);
            if (!current.hybrid_property_descriptor_root.isValid()) return;
            const auto descriptor = pages_.readHybridPropertyDescriptor(current.hybrid_property_descriptor_root, current);
            if (std::ranges::none_of(descriptor.descriptor.roots, [](const auto &root) { return root.page_id.isValid(); })) return;
            native_safe_io_epoch_ = io_->dataMutationEpoch();
            writer.release_observer_ = [this, exceptions = std::uncaught_exceptions()]() noexcept {
                const auto epoch = io_->dataMutationEpoch();
                // Saturation loses the physical-change signal. An interrupted
                // writer then requires conservative recovery, even when it
                // cannot prove that its first physical write completed.
                if (epoch != native_safe_io_epoch_ || (epoch == UINT64_MAX && std::uncaught_exceptions() > exceptions))
                    native_pool_recovery_required_ = true;
            };
        });
	}

	std::unique_ptr<RelocatableMetadataStore> RelocatableMetadataStore::initializeNew(std::shared_ptr<StorageIO> io,
																					  const Superblock &initial, std::shared_ptr<std::pmr::memory_resource> memoryOwner, std::shared_ptr<std::pmr::memory_resource> controlOwner, std::shared_ptr<std::pmr::memory_resource> leaseMemoryOwner) {
        std::optional<utils::OwnedAllocationScope> allocation;
        if (memoryOwner) allocation.emplace(memoryOwner);
		initializeRelocatableMetadataFile(io, initial);
		return open(std::move(io), std::move(memoryOwner), std::move(controlOwner), std::move(leaseMemoryOwner));
	}

	std::unique_ptr<RelocatableMetadataStore> RelocatableMetadataStore::open(std::shared_ptr<StorageIO> io, std::shared_ptr<std::pmr::memory_resource> memoryOwner, std::shared_ptr<std::pmr::memory_resource> controlOwner, std::shared_ptr<std::pmr::memory_resource> leaseMemoryOwner, std::shared_ptr<MetadataSnapshotCoordination> coordination) {
        std::optional<utils::OwnedAllocationScope> allocation;
        if (memoryOwner) allocation.emplace(memoryOwner);
        if (!coordination) coordination = makeMetadataSnapshotCoordination(controlOwner ? controlOwner : memoryOwner);
        std::unique_lock openLock(coordination->publication_mutex);
		const Superblock current = recoverRelocatableMetadataSuperblock(io);
		return std::unique_ptr<RelocatableMetadataStore>(new RelocatableMetadataStore(std::move(io), current, false, std::move(memoryOwner), std::move(controlOwner), std::move(leaseMemoryOwner), std::move(coordination), std::move(openLock)));
	}

	std::unique_ptr<RelocatableMetadataStore> RelocatableMetadataStore::openReadOnly(std::shared_ptr<StorageIO> io, std::shared_ptr<std::pmr::memory_resource> memoryOwner, std::shared_ptr<std::pmr::memory_resource> controlOwner, std::shared_ptr<std::pmr::memory_resource> leaseMemoryOwner, std::shared_ptr<MetadataSnapshotCoordination> coordination) {
        std::optional<utils::OwnedAllocationScope> allocation;
        if (memoryOwner) allocation.emplace(memoryOwner);
        if (!coordination) coordination = makeMetadataSnapshotCoordination(controlOwner ? controlOwner : memoryOwner);
        std::unique_lock openLock(coordination->publication_mutex);
		const Superblock current = recoverRelocatableMetadataSuperblock(io);
		return std::unique_ptr<RelocatableMetadataStore>(new RelocatableMetadataStore(std::move(io), current, true, std::move(memoryOwner), std::move(controlOwner), std::move(leaseMemoryOwner), std::move(coordination), std::move(openLock)));
	}

    MetadataSnapshotPin RelocatableMetadataStore::openSnapshot() const {
        auto snapshot = snapshots_.openSnapshot();
        if (snapshot.superblock().hybrid_property_descriptor_root.isValid())
            static_cast<void>(pages_.readHybridPropertyDescriptor(snapshot.superblock().hybrid_property_descriptor_root,
                snapshot.superblock()));
        return snapshot;
    }

	void RelocatableMetadataStore::refreshAfterExternalPublication() {
		if (read_only_) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable read-only metadata store cannot refresh a writable publication state");
		}
		MetadataWriteGuard writer = beginWrite();
		if (snapshots_.activeSnapshotCount() != 0U) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable metadata refresh requires all local snapshots to be released");
		}
		const Superblock current = bootstrap_.recover();
		metadata_page_pool_.rebaseToPublishedSuperblock(current);
		pool_ownership_.restoreCatalogOwnership(current);
		metadata_page_pool_.recover(current);
		pool_ownership_.verifyAfterOpen(current);
	}

	NativeGraphLogicalIdLease
	RelocatableMetadataStore::reserveNativeGraphLogicalIds(uint64_t nodeCount, uint64_t edgeCount,
														   const MetadataPublishOptions &options) {
        debug::PerfTrace::addValue("native_graph.write.id_reservation_calls", 1);
		if (read_only_ || (nodeCount == 0U && edgeCount == 0U)) {
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"native graph logical-ID leasing requires a writable store and positive bounded work");
		}
		std::lock_guard leaseLock(logical_id_lease_mutex_);
		// This direct, failpoint-aware API cannot silently consume a cached range:
		// discard its unused tail so every direct call begins at the durable
		// high-water mark and never lets an older cached ID overtake it later.
		available_node_ids_ = {};
		available_edge_ids_ = {};
		// This typed publication only advances ID high-water marks and the
        // bootstrap route context; it does not mutate graph/native payloads.
        // beginWrite still authenticates the active descriptor and performs
        // required interrupted-pool recovery under the writer lock.
        MetadataWriteGuard writer = beginWrite(true);
		const Superblock current = bootstrap_.recover();
		if (!current.checkpoint_lsn.isValid() ||
			current.checkpoint_lsn.value() == std::numeric_limits<uint64_t>::max() ||
			(current.next_logical_node_id == 0U && nodeCount != 0U) ||
			(current.next_logical_edge_id == 0U && edgeCount != 0U) ||
			(nodeCount != 0U && nodeCount - 1U > std::numeric_limits<uint64_t>::max() - current.next_logical_node_id) ||
			(edgeCount != 0U && edgeCount - 1U > std::numeric_limits<uint64_t>::max() - current.next_logical_edge_id)) {
			throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW,
								  "native graph logical-ID lease exceeds a durable logical-ID namespace");
		}
		MetadataWriteBatch batch(metadata_page_pool_, current, LogSequenceNumber(current.checkpoint_lsn.value() + 1U));
		NativeGraphLogicalIdLease lease;
		if (nodeCount != 0U) {
			lease.nodes = {batch.allocateLogicalNodeIdRange(nodeCount), nodeCount};
		}
		if (edgeCount != 0U) {
			lease.edges = {batch.allocateLogicalEdgeIdRange(edgeCount), edgeCount};
		}
        auto prepared = prepareLogicalIdCapabilities(
            nodeCount == 0U ? std::span<const NativeGraphLogicalIdLeaseRange>{} : std::span(&lease.nodes, 1U),
            edgeCount == 0U ? std::span<const NativeGraphLogicalIdLeaseRange>{} : std::span(&lease.edges, 1U),
            outstanding_node_id_leases_, outstanding_edge_id_leases_,
            "native graph logical-ID lease capability overlaps an outstanding lease");
		// Leasing changes only durable high-water marks; it must not create a
		// metadata-pool ownership transition or perturb graph-visible roots. A
		// bootstrap header still advances so the candidate generation retains a
		// same-generation route context for the next graph publication.
		prepareBootstrapRoutes(batch);
		batch.publish(bootstrap_, options.bootstrap_failpoint);
        synchronized_source_ = batch.candidate();
        // Optional diagnostics cannot turn an acknowledged publication into failure.
        try { debug::PerfTrace::addValue("native_graph.write.id_reservation_publications", 1); }
        catch (...) {}
		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate(), false);
        native_safe_io_epoch_ = io_->dataMutationEpoch();
        // The lease mutex excludes intervening registrations; these maps use
        // equal std allocators and transfer their prepared nodes without allocation.
        outstanding_node_id_leases_.merge(prepared.nodes);
        outstanding_edge_id_leases_.merge(prepared.edges);
		lease.superblock = batch.candidate();
		return lease;
	}

	NativeGraphLogicalIdReservation
	RelocatableMetadataStore::acquireNativeGraphLogicalIds(uint64_t nodeCount, uint64_t edgeCount,
														   uint64_t minimumLeaseBlockSize) {
        debug::PerfTrace::addValue("native_graph.write.id_reservation_calls", 1);
		if (read_only_ || (nodeCount == 0U && edgeCount == 0U) || minimumLeaseBlockSize == 0U ||
			minimumLeaseBlockSize == std::numeric_limits<uint64_t>::max()) {
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"native graph logical-ID acquisition requires a writable store and bounded positive work");
		}
		std::lock_guard leaseLock(logical_id_lease_mutex_);
		const auto validateRequest = [](uint64_t count, uint64_t first, const char *entityName) {
			if (count != 0U && (first == 0U || count - 1U > std::numeric_limits<uint64_t>::max() - first)) {
				throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW,
									  std::string("native graph ") + entityName +
											  " logical-ID acquisition exceeds its durable namespace");
			}
		};
		const auto take = [](NativeGraphLogicalIdLeaseRange &available, uint64_t &remaining,
							 std::vector<NativeGraphLogicalIdLeaseRange> &result) {
			if (remaining == 0U || available.empty()) {
				return;
			}
			const uint64_t taken = std::min(available.count, remaining);
			result.push_back({available.first_id, taken});
			available.first_id += taken;
			available.count -= taken;
			remaining -= taken;
		};
		uint64_t remainingNodes = nodeCount;
		uint64_t remainingEdges = edgeCount;
		NativeGraphLogicalIdReservation result;
		take(available_node_ids_, remainingNodes, result.node_ranges);
		take(available_edge_ids_, remainingEdges, result.edge_ranges);
        const auto prepareOutstanding = [this](const NativeGraphLogicalIdReservation &reservation) {
            return prepareLogicalIdCapabilities(reservation.node_ranges, reservation.edge_ranges,
                outstanding_node_id_leases_, outstanding_edge_id_leases_,
                "native graph logical-ID acquisition overlaps an outstanding lease");
        };
        const auto registerPrepared = [this](PreparedLogicalIdCapabilities &prepared) {
            outstanding_node_id_leases_.merge(prepared.nodes);
            outstanding_edge_id_leases_.merge(prepared.edges);
        };
		if (remainingNodes == 0U && remainingEdges == 0U) {
            debug::PerfTrace::addValue("native_graph.write.id_lease_cache_hits", 1);
            auto prepared = prepareOutstanding(result);
            registerPrepared(prepared);
			return result;
		}
		const uint64_t requestedNodes = remainingNodes == 0U ? 0U : std::max(remainingNodes, minimumLeaseBlockSize);
		const uint64_t requestedEdges = remainingEdges == 0U ? 0U : std::max(remainingEdges, minimumLeaseBlockSize);
		// This typed publication only advances ID high-water marks and the
        // bootstrap route context; it does not mutate graph/native payloads.
        // beginWrite still authenticates the active descriptor and performs
        // required interrupted-pool recovery under the writer lock.
        MetadataWriteGuard writer = beginWrite(true);
		const Superblock current = bootstrap_.recover();
		validateRequest(requestedNodes, current.next_logical_node_id, "Node");
		validateRequest(requestedEdges, current.next_logical_edge_id, "Edge");
		if (!current.checkpoint_lsn.isValid() ||
			current.checkpoint_lsn.value() == std::numeric_limits<uint64_t>::max()) {
			throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW,
								  "native graph logical-ID acquisition cannot advance the checkpoint LSN");
		}
		MetadataWriteBatch batch(metadata_page_pool_, current, LogSequenceNumber(current.checkpoint_lsn.value() + 1U));
		if (requestedNodes != 0U) {
			static_cast<void>(batch.allocateLogicalNodeIdRange(requestedNodes));
		}
		if (requestedEdges != 0U) {
			static_cast<void>(batch.allocateLogicalEdgeIdRange(requestedEdges));
		}
        NativeGraphLogicalIdLeaseRange nextAvailableNodes{current.next_logical_node_id, requestedNodes};
        NativeGraphLogicalIdLeaseRange nextAvailableEdges{current.next_logical_edge_id, requestedEdges};
        if (remainingNodes != 0U) take(nextAvailableNodes, remainingNodes, result.node_ranges);
        if (remainingEdges != 0U) take(nextAvailableEdges, remainingEdges, result.edge_ranges);
        auto prepared = prepareOutstanding(result);
        prepareBootstrapRoutes(batch);
        batch.publish(bootstrap_);
        synchronized_source_ = batch.candidate();
        // Optional diagnostics cannot turn an acknowledged publication into failure.
        try { debug::PerfTrace::addValue("native_graph.write.id_reservation_publications", 1); }
        catch (...) {}
        snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate(), false);
        native_safe_io_epoch_ = io_->dataMutationEpoch();
        if (requestedNodes != 0U) available_node_ids_ = nextAvailableNodes;
        if (requestedEdges != 0U) available_edge_ids_ = nextAvailableEdges;
        registerPrepared(prepared);
		return result;
	}

	bool
	RelocatableMetadataStore::claimNativeGraphLogicalIdReservation(const NativeGraphLogicalIdReservation &reservation) {
		if (reservation.empty()) {
			return true;
		}
		std::lock_guard leaseLock(logical_id_lease_mutex_);
		const auto validateRanges = [](const std::pmr::map<uint64_t, uint64_t> &outstanding,
									   std::span<const NativeGraphLogicalIdLeaseRange> ranges) {
			std::set<uint64_t> firstIds;
			for (const NativeGraphLogicalIdLeaseRange &range: ranges) {
				const auto iterator = outstanding.find(range.first_id);
				if (range.first_id == 0U || range.count == 0U || !firstIds.insert(range.first_id).second ||
					iterator == outstanding.end() || iterator->second != range.count) {
					return false;
				}
			}
			return true;
		};
		if (!validateRanges(outstanding_node_id_leases_, reservation.node_ranges) ||
			!validateRanges(outstanding_edge_id_leases_, reservation.edge_ranges)) {
			return false;
		}
		for (const NativeGraphLogicalIdLeaseRange &range: reservation.node_ranges) {
			outstanding_node_id_leases_.erase(range.first_id);
		}
		for (const NativeGraphLogicalIdLeaseRange &range: reservation.edge_ranges) {
			outstanding_edge_id_leases_.erase(range.first_id);
		}
		return true;
	}

	void RelocatableMetadataStore::abandonNativeGraphLogicalIdReservation(
			const NativeGraphLogicalIdReservation &reservation) noexcept {
        abandonNativeGraphLogicalIdReservation(reservation.node_ranges, reservation.edge_ranges);
    }

    void RelocatableMetadataStore::abandonNativeGraphLogicalIdReservation(
            std::span<const NativeGraphLogicalIdLeaseRange> nodes,
            std::span<const NativeGraphLogicalIdLeaseRange> edges) noexcept {
		try {
			std::lock_guard leaseLock(logical_id_lease_mutex_);
			const auto abandonRanges = [](std::pmr::map<uint64_t, uint64_t> &outstanding,
										  std::span<const NativeGraphLogicalIdLeaseRange> ranges) {
				for (const NativeGraphLogicalIdLeaseRange &range: ranges) {
					const auto iterator = outstanding.find(range.first_id);
					if (iterator != outstanding.end() && iterator->second == range.count) {
						outstanding.erase(iterator);
					}
				}
			};
			abandonRanges(outstanding_node_id_leases_, nodes);
			abandonRanges(outstanding_edge_id_leases_, edges);
		} catch (...) {
			// Rollback is best-effort only for an in-memory capability. Reuse still
			// cannot occur after process restart because durable high-water marks only advance.
		}
	}

	bool RelocatableMetadataStore::ownsSnapshot(const MetadataSnapshotPin &snapshot) const noexcept {
		return snapshots_.owns(snapshot);
	}

	bool RelocatableMetadataStore::isIdentityOnlyBootstrapAdvance(const Superblock &before,
																  const Superblock &after) const {
		if (!before.usesPageDirectoryBootstrap() || !after.usesPageDirectoryBootstrap() ||
			!before.page_directory_root.isValid() || !after.page_directory_root.isValid()) {
			return false;
		}
		const PageDirectoryBootstrap previous = pages_.readPageDirectoryBootstrap(before.page_directory_root);
		const PageDirectoryBootstrap current = pages_.readPageDirectoryBootstrap(after.page_directory_root);
		return previous.data_page_directory_root == current.data_page_directory_root &&
			   previous.metadata_page_directory_root == current.metadata_page_directory_root &&
			   previous.metadata_directory_roots == current.metadata_directory_roots;
	}

    namespace {
    void prepareBootstrapWithRoutes(MetadataWriteBatch &batch, MetadataPageRouteDirectory &routes) {
		if (batch.candidate().usesPageDirectoryBootstrap() && !batch.bootstrapRoutePublished()) {
			if (batch.metadataPageRoutes().empty() && !batch.pendingBootstrapDataRoot().has_value() &&
				!batch.bootstrapRouteRefreshRequested()) {
				batch.reserveMetadataFrames(1U);
				static_cast<void>(routes.publishIdentityOnlyBootstrap(batch));
				return;
			}
			const size_t requiredFrames = routes.nextBootstrapFrameCount(batch);
			batch.reserveMetadataFrames(requiredFrames);
			static_cast<void>(
					routes.publishNextBootstrap(batch, batch.pendingBootstrapDataRoot().value_or(PhysicalPageRef{})));
		}
	}

    } // namespace

	void RelocatableMetadataStore::prepareBootstrapRoutes(MetadataWriteBatch &batch) {
        MetadataPageRouteDirectory routes(pages_);
        if (batch.candidate().usesPageDirectoryBootstrap() && batch.metadataPageRoutes().empty() &&
            !batch.pendingBootstrapDataRoot().has_value() && !batch.bootstrapRouteRefreshRequested() &&
            batch.ensureMetadataFrameCapacity(1U) != 0U) {
            // Publish ownership of the bounded capacity pool in this same
            // ID-only batch. Graph directories remain unchanged.
            try { debug::PerfTrace::addValue("native_graph.write.id_capacity_pool", 1); } catch (...) {}
            prepareBatchPublication(batch);
            return;
        }
        prepareBootstrapWithRoutes(batch, routes);
    }

    void RelocatableMetadataStore::prepareBatchPublication(MetadataWriteBatch &batch) {
        if (batch.publication_prepared_) return;
        MetadataPageRouteDirectory routes(pages_);
        if (batch.candidate().hybrid_property_descriptor_root.isValid()) {
            const auto descriptor = pages_.readHybridPropertyDescriptor(batch.candidate().hybrid_property_descriptor_root,
                batch.candidate());
            if (!batch.hybrid_property_publication_ && std::ranges::any_of(descriptor.descriptor.roots,
                [](const auto &root) { return root.page_id.isValid(); }))
                throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION,
                    "nonempty native hybrid publication requires its metadata-owned candidate coordinator");
        }
		// The first Catalog COW can allocate a metadata pool that was not visible
		// while the ownership pass enumerated pools. Reconcile it while the Catalog
		// root still uses physical children; logical route children are not visible
		// until the final bootstrap publication.
		pool_ownership_.preparePublication(batch);
		catalog_.prepareLogicalRootChildren(batch);
		// Catalog logicalization can allocate a metadata pool. Reconcile it while
		// the allocator root is still physical so setting its ownership bit composes
		// with all data-pool allocations already staged in this batch.
		pool_ownership_.preparePublication(batch);
		allocator_.prepareLogicalRootChildren(batch);
		locations_.prepareLogicalRootChildren(batch);
		// Any metadata pool created by allocator or location-root logicalization is
		// reconciled through the pending route overlay before the final publication.
		pool_ownership_.preparePublication(batch);
		if (batch.candidate().usesPageDirectoryBootstrap()) {
			for (size_t pass = 0U; pass < 8U; ++pass) {
				const bool identityOnly = batch.metadataPageRoutes().empty() &&
										  !batch.pendingBootstrapDataRoot().has_value() &&
										  !batch.bootstrapRouteRefreshRequested();
				const size_t frameCount = identityOnly ? 1U : routes.nextBootstrapFrameCount(batch);
				if (batch.ensureMetadataFrameCapacity(frameCount) == 0U) {
					break;
				}
				// A capacity pool is private until this Catalog publication. Logicalize
				// the resulting COW root before calculating the final route budget again.
				pool_ownership_.preparePublication(batch);
				catalog_.prepareLogicalRootChildren(batch);
				allocator_.prepareLogicalRootChildren(batch);
				locations_.prepareLogicalRootChildren(batch);
				if (pass == 7U) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "relocatable metadata route-capacity publication did not converge");
				}
			}
		}
		prepareBootstrapWithRoutes(batch, routes);
        batch.publication_prepared_ = true;
    }

	void RelocatableMetadataStore::publishBatch(MetadataWriteBatch &batch, const MetadataWriteGuard &writer,
												const MetadataPublishOptions &options) {
        prepareBatchPublication(batch);
		batch.publish(bootstrap_, options.bootstrap_failpoint);
        synchronized_source_ = batch.candidate();
		pages_.rebindSuperblock(batch.candidate());
		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate(), false);
        native_safe_io_epoch_ = io_->dataMutationEpoch();
        native_pool_recovery_required_ = false;
	}

	Superblock RelocatableMetadataStore::upsertCatalog(std::span<const CatalogEntry> entries,
													   LogSequenceNumber checkpointLsn,
													   const MetadataPublishOptions &options) {
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		for (const CatalogEntry &entry: entries) {
			batch.reserveDataSlot(entry.physical_slot);
			batch.observeLogicalSegmentId(entry.segment_id);
		}
		catalog_.prepareUpsert(batch, entries);
		publishBatch(batch, writer, options);
		return batch.candidate();
	}

	Superblock RelocatableMetadataStore::upsertLocations(std::span<const PageLocation> pages,
														 std::span<const ExtentLocation> extents,
														 LogSequenceNumber checkpointLsn,
														 const MetadataPublishOptions &options) {
		return upsertLocationsImpl(pages, extents, checkpointLsn, nullptr, options);
	}

	Superblock RelocatableMetadataStore::upsertLocations(std::span<const PageLocation> pages,
														 std::span<const ExtentLocation> extents,
														 LogSequenceNumber checkpointLsn,
														 const LocationPublicationPreparer &preparer,
														 const MetadataPublishOptions &options) {
		if (!preparer) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable prepared location publication requires a data preparation callback");
		}
		return upsertLocationsImpl(pages, extents, checkpointLsn, &preparer, options);
	}

	Superblock RelocatableMetadataStore::upsertEntityPageLocations(std::span<const EntityPageLocation> nodes,
																   std::span<const uint64_t> erasedNodeIds,
																   std::span<const EntityPageLocation> edges,
																   std::span<const uint64_t> erasedEdgeIds,
																   LogSequenceNumber checkpointLsn,
																   const MetadataPublishOptions &options) {
		if ((nodes.empty() && erasedNodeIds.empty()) && (edges.empty() && erasedEdgeIds.empty())) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable Entity Map publication requires a Node or Edge mutation");
		}
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		const auto validateLocation = [&](const EntityPageLocation &location) {
			const std::optional<PageLocation> page = locations_.findPageOptional(current, location.page_id);
			if (!page.has_value()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable Entity Map entry references an absent Page Directory entry");
			}
			const CatalogEntry pool = catalog_.find(current, page->segment_id);
			const uint8_t invalid = static_cast<uint8_t>(location.entity_id == 0U) |
									static_cast<uint8_t>(!location.page_id.isValid()) |
									static_cast<uint8_t>(location.slot_id == std::numeric_limits<uint16_t>::max()) |
									static_cast<uint8_t>(location.slot_generation == 0U) |
									static_cast<uint8_t>(pool.kind != SegmentKind::SGK_ENTITY_PAGE_POOL) |
									static_cast<uint8_t>(pool.state != SegmentState::SGS_SEALED) |
									static_cast<uint8_t>(pool.generation != page->generation) |
									static_cast<uint8_t>(!allocator_.isAllocated(current, pool.physical_slot));
			if (invalid != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable Entity Map entry does not resolve to a current entity page pool");
			}
		};
		for (const EntityPageLocation &location: nodes) {
			validateLocation(location);
		}
		for (const EntityPageLocation &location: edges) {
			validateLocation(location);
		}
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		for (const EntityPageLocation &location: nodes) {
			batch.observeLogicalNodeId(location.entity_id);
		}
		for (const EntityPageLocation &location: edges) {
			batch.observeLogicalEdgeId(location.entity_id);
		}
		if (!nodes.empty() || !erasedNodeIds.empty()) {
			locations_.prepareReplaceNodePageLocations(batch, nodes, erasedNodeIds);
		}
		if (!edges.empty() || !erasedEdgeIds.empty()) {
			locations_.prepareReplaceEdgePageLocations(batch, edges, erasedEdgeIds);
		}
		publishBatch(batch, writer, options);
		return batch.candidate();
	}

	Superblock RelocatableMetadataStore::upsertEntityPropertyLocations(
			std::span<const EntityPropertyLocation> locations, std::span<const EntityPropertyKey> erasedKeys,
			LogSequenceNumber checkpointLsn, const MetadataPublishOptions &options) {
		if (locations.empty() && erasedKeys.empty()) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable Entity Property location publication requires a mutation");
		}
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		for (const EntityPropertyLocation &location: locations) {
			const PageLocation page = locations_.findPage(current, location.page_id);
			const CatalogEntry pool = catalog_.find(current, page.segment_id);
			const uint8_t invalid = static_cast<uint8_t>(!location.key.isValid()) |
									static_cast<uint8_t>(!location.page_id.isValid()) |
									static_cast<uint8_t>(location.slot_id == std::numeric_limits<uint16_t>::max()) |
									static_cast<uint8_t>(location.slot_generation == 0U) |
									static_cast<uint8_t>(pool.kind != SegmentKind::SGK_PROPERTY_PAGE_POOL) |
									static_cast<uint8_t>(pool.state != SegmentState::SGS_SEALED) |
									static_cast<uint8_t>(pool.generation != page.generation) |
									static_cast<uint8_t>(!allocator_.isAllocated(current, pool.physical_slot));
			if (invalid != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable Entity Property location does not target a current Property page");
			}
		}
		for (const EntityPropertyKey &key: erasedKeys) {
			if (!key.isValid()) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "relocatable Entity Property deletion has an invalid key");
			}
		}
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		locations_.prepareReplaceEntityProperties(batch, locations, erasedKeys);
		publishBatch(batch, writer, options);
		return batch.candidate();
	}

	std::optional<EntityPropertyPagePoolMaterialization> RelocatableMetadataStore::materializeEntityPropertyPagePool(
			uint64_t pageOwnerId, EntityPropertyKey property, PhysicalSlotId firstSlot, PhysicalSlotId lastSlot,
			LogSequenceNumber checkpointLsn, const EntityPropertyPagePoolPreparer &preparer,
			const MetadataPublishOptions &options) {
		const uint8_t invalidInput =
				static_cast<uint8_t>(pageOwnerId == 0U) | static_cast<uint8_t>(!property.isValid()) |
				static_cast<uint8_t>(!firstSlot.isValid()) | static_cast<uint8_t>(!lastSlot.isValid()) |
				static_cast<uint8_t>(firstSlot > lastSlot) | static_cast<uint8_t>(!checkpointLsn.isValid()) |
				static_cast<uint8_t>(!preparer);
		if (invalidInput != 0U) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable Entity Property page materialization requires an owner, key, slot "
								  "range, LSN, and preparer");
		}

		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		if (locations_.findEntityPropertyOptional(current, property).has_value()) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable Entity Property page materialization cannot replace an existing owner key");
		}
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		const SegmentId segmentId = batch.allocateLogicalSegmentId();
		const PageId pageId = batch.allocateLogicalPageId();
		// The writer-held MetadataWriteBatch allocates from durable high-water
		// marks, so newly issued Segment and Page IDs cannot already be present.
		if (catalog_.findOptional(current, segmentId).has_value() ||
			locations_.findPageOptional(current, pageId).has_value()) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable Entity Property page materialization allocated a published logical identity");
		}
		const std::optional<PhysicalSlotId> physicalSlot =
				allocator_.prepareReserveFirstFree(batch, firstSlot, lastSlot);
		if (!physicalSlot.has_value()) {
			return std::nullopt;
		}

		CatalogEntry pool;
		pool.segment_id = segmentId;
		pool.physical_slot = *physicalSlot;
		pool.kind = SegmentKind::SGK_PROPERTY_PAGE_POOL;
		pool.owner_id = pageOwnerId;
		pool.generation = Generation(1);
		pool.state = SegmentState::SGS_SEALED;
		pool.committed_lsn = checkpointLsn;
		batch.observeLogicalSegmentId(segmentId);

		PageLocation page{pageId, segmentId, 0U, pool.generation, 0U};
		EntityPropertyLocation location{property, pageId, 0U, 0U};
		preparer(pool, pageId, page, location);
		const uint8_t invalidPrepared = static_cast<uint8_t>(page.page_id != pageId) |
										static_cast<uint8_t>(page.segment_id != pool.segment_id) |
										static_cast<uint8_t>(page.generation != pool.generation) |
										static_cast<uint8_t>(page.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
										static_cast<uint8_t>(page.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
										static_cast<uint8_t>(page.free_bytes > RSS_PAGE_SIZE) |
										static_cast<uint8_t>(location.key != property) |
										static_cast<uint8_t>(location.page_id != pageId) |
										static_cast<uint8_t>(location.slot_id == std::numeric_limits<uint16_t>::max()) |
										static_cast<uint8_t>(location.slot_generation == 0U);
		if (invalidPrepared != 0U) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable Entity Property page preparer returned an invalid page or directory location");
		}

		catalog_.prepareUpsert(batch, std::span(&pool, 1));
		locations_.prepareUpsertPages(batch, std::span(&page, 1));
		locations_.prepareReplaceEntityProperties(batch, std::span(&location, 1), {});
		publishBatch(batch, writer, options);
		return EntityPropertyPagePoolMaterialization{batch.candidate(), pool, page, location};
	}

	std::optional<StatePagePoolMaterialization>
	RelocatableMetadataStore::materializeStatePagePool(uint64_t pageOwnerId, uint64_t stateId, PhysicalSlotId firstSlot,
													   PhysicalSlotId lastSlot, LogSequenceNumber checkpointLsn,
													   const StatePagePoolPreparer &preparer,
													   const MetadataPublishOptions &options) {
		const uint8_t invalidInput = static_cast<uint8_t>(pageOwnerId == 0U) | static_cast<uint8_t>(stateId == 0U) |
									 static_cast<uint8_t>(!firstSlot.isValid()) |
									 static_cast<uint8_t>(!lastSlot.isValid()) |
									 static_cast<uint8_t>(firstSlot > lastSlot) |
									 static_cast<uint8_t>(!checkpointLsn.isValid()) | static_cast<uint8_t>(!preparer);
		if (invalidInput != 0U) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable State page materialization requires an owner, State ID, slot range, "
								  "LSN, and preparer");
		}

		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		if (locations_.findStateOptional(current, stateId).has_value()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable State page materialization cannot replace an existing State ID");
		}
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		const SegmentId segmentId = batch.allocateLogicalSegmentId();
		const PageId pageId = batch.allocateLogicalPageId();
		// The writer-held MetadataWriteBatch allocates from durable high-water
		// marks, so newly issued Segment and Page IDs cannot already be present.
		if (catalog_.findOptional(current, segmentId).has_value() ||
			locations_.findPageOptional(current, pageId).has_value()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable State page materialization allocated a published logical identity");
		}
		const std::optional<PhysicalSlotId> physicalSlot =
				allocator_.prepareReserveFirstFree(batch, firstSlot, lastSlot);
		if (!physicalSlot.has_value()) {
			return std::nullopt;
		}

		CatalogEntry pool;
		pool.segment_id = segmentId;
		pool.physical_slot = *physicalSlot;
		pool.kind = SegmentKind::SGK_STATE_PAGE_POOL;
		pool.owner_id = pageOwnerId;
		pool.generation = Generation(1);
		pool.state = SegmentState::SGS_SEALED;
		pool.committed_lsn = checkpointLsn;
		batch.observeLogicalSegmentId(segmentId);

		PageLocation page{pageId, segmentId, 0U, pool.generation, 0U};
		StateLocation state{stateId, pageId, 0U, 0U};
		preparer(pool, pageId, page, state);
		const uint8_t invalidPrepared = static_cast<uint8_t>(page.page_id != pageId) |
										static_cast<uint8_t>(page.segment_id != pool.segment_id) |
										static_cast<uint8_t>(page.generation != pool.generation) |
										static_cast<uint8_t>(page.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
										static_cast<uint8_t>(page.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
										static_cast<uint8_t>(page.free_bytes > RSS_PAGE_SIZE) |
										static_cast<uint8_t>(state.state_id != stateId) |
										static_cast<uint8_t>(state.page_id != pageId) |
										static_cast<uint8_t>(state.slot_id == std::numeric_limits<uint16_t>::max()) |
										static_cast<uint8_t>(state.slot_generation == 0U);
		if (invalidPrepared != 0U) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable State page preparer returned an invalid page or directory location");
		}

		catalog_.prepareUpsert(batch, std::span(&pool, 1));
		locations_.prepareUpsertPages(batch, std::span(&page, 1));
		locations_.prepareReplaceStates(batch, std::span(&state, 1), {});
		publishBatch(batch, writer, options);
		return StatePagePoolMaterialization{batch.candidate(), pool, page, state};
	}

	Superblock RelocatableMetadataStore::replaceStatePageAndUpsertLocation(const PageLocation &expectedPage,
																		   StateLocation state,
																		   LogSequenceNumber checkpointLsn,
																		   const StatePageReplacementPreparer &preparer,
																		   const MetadataPublishOptions &options) {
		const uint8_t invalidInput = static_cast<uint8_t>(!expectedPage.page_id.isValid()) |
									 static_cast<uint8_t>(!expectedPage.segment_id.isValid()) |
									 static_cast<uint8_t>(!expectedPage.generation.isValid()) |
									 static_cast<uint8_t>(expectedPage.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
									 static_cast<uint8_t>(expectedPage.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
									 static_cast<uint8_t>(state.state_id == 0U) |
									 static_cast<uint8_t>(!checkpointLsn.isValid()) | static_cast<uint8_t>(!preparer);
		if (invalidInput != 0U) {
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"relocatable State page replacement requires a current page, State ID, LSN, and preparer");
		}

		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		const PageLocation currentPage = locations_.findPage(current, expectedPage.page_id);
		const CatalogEntry pool = catalog_.find(current, currentPage.segment_id);
		const std::optional<StateLocation> visibleState = locations_.findStateOptional(current, state.state_id);
		const uint8_t invalidCurrent =
				static_cast<uint8_t>(currentPage != expectedPage) |
				static_cast<uint8_t>(pool.kind != SegmentKind::SGK_STATE_PAGE_POOL) |
				static_cast<uint8_t>(pool.state != SegmentState::SGS_SEALED) |
				static_cast<uint8_t>(pool.generation != currentPage.generation) |
				static_cast<uint8_t>(!allocator_.isAllocated(current, pool.physical_slot)) |
				static_cast<uint8_t>(visibleState.has_value() && *visibleState != state);
		if (invalidCurrent != 0U) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable State page replacement has a stale page, non-State pool, or State route");
		}

		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		PageLocation replacement = expectedPage;
		preparer(pool, expectedPage, replacement, state);
		const uint8_t invalidPrepared = static_cast<uint8_t>(replacement.page_id != expectedPage.page_id) |
										static_cast<uint8_t>(replacement.segment_id != pool.segment_id) |
										static_cast<uint8_t>(replacement.generation != pool.generation) |
										static_cast<uint8_t>(replacement.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
										static_cast<uint8_t>(replacement.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
										static_cast<uint8_t>(replacement.free_bytes > RSS_PAGE_SIZE) |
										static_cast<uint8_t>(state.page_id != expectedPage.page_id) |
										static_cast<uint8_t>(state.slot_id == std::numeric_limits<uint16_t>::max()) |
										static_cast<uint8_t>(state.slot_generation == 0U);
		if (invalidPrepared != 0U) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable State page replacement preparer returned an invalid page or directory location");
		}

		locations_.prepareReplacePages(batch, std::span(&replacement, 1), {});
		locations_.prepareReplaceStates(batch, std::span(&state, 1), {});
		publishBatch(batch, writer, options);
		return batch.candidate();
	}

	Superblock RelocatableMetadataStore::upsertStateLocations(std::span<const StateLocation> locations,
															  std::span<const uint64_t> erasedStateIds,
															  LogSequenceNumber checkpointLsn,
															  const MetadataPublishOptions &options) {
		if (locations.empty() && erasedStateIds.empty()) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable State location publication requires a mutation");
		}
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		for (const StateLocation &location: locations) {
			const PageLocation page = locations_.findPage(current, location.page_id);
			const CatalogEntry pool = catalog_.find(current, page.segment_id);
			const uint8_t invalid = static_cast<uint8_t>(location.state_id == 0U) |
									static_cast<uint8_t>(!location.page_id.isValid()) |
									static_cast<uint8_t>(location.slot_id == std::numeric_limits<uint16_t>::max()) |
									static_cast<uint8_t>(location.slot_generation == 0U) |
									static_cast<uint8_t>(pool.kind != SegmentKind::SGK_STATE_PAGE_POOL) |
									static_cast<uint8_t>(pool.state != SegmentState::SGS_SEALED) |
									static_cast<uint8_t>(pool.generation != page.generation) |
									static_cast<uint8_t>(!allocator_.isAllocated(current, pool.physical_slot));
			if (invalid != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable State location does not target a current State page");
			}
		}
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		locations_.prepareReplaceStates(batch, locations, erasedStateIds);
		publishBatch(batch, writer, options);
		return batch.candidate();
	}

	Superblock RelocatableMetadataStore::upsertVectorLocations(std::span<const VectorLocation> locations,
															   std::span<const uint64_t> erasedVectorIds,
															   LogSequenceNumber checkpointLsn,
															   const MetadataPublishOptions &options) {
		if (locations.empty() && erasedVectorIds.empty()) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable Vector Location Directory publication requires a mutation");
		}
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		for (const VectorLocation &location: locations) {
			const PageLocation page = locations_.findPage(current, location.page_id);
			const CatalogEntry pool = catalog_.find(current, page.segment_id);
			const bool vectorPool =
					pool.kind == SegmentKind::SGK_VECTOR_RAW_POOL || pool.kind == SegmentKind::SGK_VECTOR_PQ_POOL;
			const uint8_t invalid =
					static_cast<uint8_t>(location.vector_id == 0U) | static_cast<uint8_t>(!location.page_id.isValid()) |
					static_cast<uint8_t>(location.slot_id == std::numeric_limits<uint16_t>::max()) |
					static_cast<uint8_t>(location.slot_generation == 0U) | static_cast<uint8_t>(!vectorPool) |
					static_cast<uint8_t>(pool.state != SegmentState::SGS_SEALED) |
					static_cast<uint8_t>(pool.generation != page.generation) |
					static_cast<uint8_t>(!allocator_.isAllocated(current, pool.physical_slot));
			if (invalid != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable Vector location does not target a current raw or PQ vector page");
			}
		}
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		locations_.prepareReplaceVectorLocations(batch, locations, erasedVectorIds);
		publishBatch(batch, writer, options);
		return batch.candidate();
	}

	Superblock RelocatableMetadataStore::upsertVectorManifests(std::span<const VectorManifestLocation> manifests,
															   std::span<const uint64_t> erasedIndexIds,
															   LogSequenceNumber checkpointLsn,
															   const MetadataPublishOptions &options) {
		if (manifests.empty() && erasedIndexIds.empty()) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable Vector Manifest Directory publication requires a mutation");
		}
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		for (const VectorManifestLocation &manifest: manifests) {
			const PageLocation page = locations_.findPage(current, manifest.manifest_page_id);
			const CatalogEntry pool = catalog_.find(current, page.segment_id);
			const uint8_t invalid = static_cast<uint8_t>(manifest.index_id == 0U) |
									static_cast<uint8_t>(!manifest.manifest_page_id.isValid()) |
									static_cast<uint8_t>(!manifest.generation.isValid()) |
									static_cast<uint8_t>(pool.kind != SegmentKind::SGK_VECTOR_MANIFEST_POOL) |
									static_cast<uint8_t>(pool.owner_id != manifest.index_id) |
									static_cast<uint8_t>(pool.state != SegmentState::SGS_SEALED) |
									static_cast<uint8_t>(pool.generation != page.generation) |
									static_cast<uint8_t>(page.generation != manifest.generation) |
									static_cast<uint8_t>(!allocator_.isAllocated(current, pool.physical_slot));
			if (invalid != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable Vector manifest does not target a current dedicated manifest page");
			}
		}
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		locations_.prepareReplaceVectorManifests(batch, manifests, erasedIndexIds);
		publishBatch(batch, writer, options);
		return batch.candidate();
	}

	Superblock RelocatableMetadataStore::upsertIndexRoots(std::span<const IndexRootLocation> roots,
														  std::span<const uint64_t> erasedIndexIds,
														  LogSequenceNumber checkpointLsn,
														  const MetadataPublishOptions &options) {
		if (roots.empty() && erasedIndexIds.empty()) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable Index Root Directory publication requires a mutation");
		}
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		for (const IndexRootLocation &root: roots) {
			const uint8_t invalidIdentity = static_cast<uint8_t>(root.index_id == 0U) |
											static_cast<uint8_t>(!root.root_page_id.isValid()) |
											static_cast<uint8_t>(!root.root_generation.isValid());
			if (invalidIdentity != 0U) { // ZYX_COV_EXCL_LINE: BTreePageStore validates every typed root before
				// invoking this private metadata-only directory publication primitive.
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "relocatable Index Root Directory entry has an invalid logical identity");
			}
			const PageLocation page = locations_.findPage(current, root.root_page_id);
			const CatalogEntry pool = catalog_.find(current, page.segment_id);
			const uint8_t invalid = static_cast<uint8_t>(pool.kind != SegmentKind::SGK_INDEX_PAGE_POOL) |
									static_cast<uint8_t>(pool.state != SegmentState::SGS_SEALED) |
									static_cast<uint8_t>(pool.generation != page.generation) |
									static_cast<uint8_t>(page.generation != root.root_generation) |
									static_cast<uint8_t>(!allocator_.isAllocated(current, pool.physical_slot));
			if (invalid != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable Index Root Directory entry does not target a current index page");
			}
		}
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		locations_.prepareReplaceIndexRoots(batch, roots, erasedIndexIds);
		publishBatch(batch, writer, options);
		return batch.candidate();
	}

} // namespace graph::storage
