		class RelocatableLogicalGraphTransaction final : public LogicalGraphTransaction,
														 public LogicalGraphTransactionVectorSnapshotProvider,
														 public LogicalGraphBoundedBulkEdgeTransaction, public LogicalGraphTransactionMemoryProvider, public utils::OwnedPmrObject {
            struct OwnedCreationReservation {
                utils::OwnedVector<NativeGraphLogicalIdLeaseRange> node_ranges;
                utils::OwnedVector<NativeGraphLogicalIdLeaseRange> edge_ranges;
                OwnedCreationReservation() = delete;
                explicit OwnedCreationReservation(std::shared_ptr<std::pmr::memory_resource> memory)
                    : node_ranges(utils::OwnedPmrAllocator<NativeGraphLogicalIdLeaseRange>(memory)),
                      edge_ranges(utils::OwnedPmrAllocator<NativeGraphLogicalIdLeaseRange>(std::move(memory))) {}
                OwnedCreationReservation(const OwnedCreationReservation &source)
                    : node_ranges(source.node_ranges.begin(), source.node_ranges.end(), source.node_ranges.get_allocator()),
                      edge_ranges(source.edge_ranges.begin(), source.edge_ranges.end(), source.edge_ranges.get_allocator()) {}
                OwnedCreationReservation(OwnedCreationReservation &&) noexcept = default;
                OwnedCreationReservation &operator=(const OwnedCreationReservation &) = default;
                OwnedCreationReservation &operator=(OwnedCreationReservation &&) noexcept = default;
            };
            // Own fixed request slots without changing the public batch ABI.
            // Nested standard buffers carry a separate retained payload lease.
            struct OwnedMutationBatch {
                utils::OwnedVector<LogicalGraphNodeCreateRequest> node_creates;
                utils::OwnedVector<LogicalGraphBatchEdgeCreateRequest> edge_creates;
                utils::OwnedVector<LogicalGraphPropertyMutationRequest> property_replacements;
                utils::OwnedVector<LogicalGraphNodeLabelMutationRequest> node_label_replacements;
                utils::OwnedVector<LogicalGraphEdgeDeleteRequest> edge_deletes;
                utils::OwnedVector<LogicalGraphNodeDeleteRequest> node_deletes;
                explicit OwnedMutationBatch(const std::shared_ptr<std::pmr::memory_resource> &memory)
                    : node_creates(utils::OwnedPmrAllocator<LogicalGraphNodeCreateRequest>(memory)),
                      edge_creates(utils::OwnedPmrAllocator<LogicalGraphBatchEdgeCreateRequest>(memory)),
                      property_replacements(utils::OwnedPmrAllocator<LogicalGraphPropertyMutationRequest>(memory)),
                      node_label_replacements(utils::OwnedPmrAllocator<LogicalGraphNodeLabelMutationRequest>(memory)),
                      edge_deletes(utils::OwnedPmrAllocator<LogicalGraphEdgeDeleteRequest>(memory)),
                      node_deletes(utils::OwnedPmrAllocator<LogicalGraphNodeDeleteRequest>(memory)) {}
                OwnedMutationBatch(const OwnedMutationBatch &source, const LogicalGraphMutationBatch &incoming)
                    : OwnedMutationBatch(source.node_creates.get_allocator().owner()) {
                    const auto clone = [](auto &destination, const auto &old, const auto &added) {
                        if (added.size() > destination.max_size() - old.size())
                            throw std::length_error("logical transaction request slots overflowed");
                        destination.reserve(old.size() + added.size());
                        destination.insert(destination.end(), old.begin(), old.end());
                    };
                    clone(node_creates, source.node_creates, incoming.node_creates);
                    clone(edge_creates, source.edge_creates, incoming.edge_creates);
                    clone(property_replacements, source.property_replacements, incoming.property_replacements);
                    clone(node_label_replacements, source.node_label_replacements, incoming.node_label_replacements);
                    clone(edge_deletes, source.edge_deletes, incoming.edge_deletes);
                    clone(node_deletes, source.node_deletes, incoming.node_deletes);
                }
                OwnedMutationBatch(OwnedMutationBatch &&) noexcept = default;
                OwnedMutationBatch &operator=(const OwnedMutationBatch &) = delete;
                OwnedMutationBatch &operator=(OwnedMutationBatch &&) noexcept = default;
            };

		public:
			RelocatableLogicalGraphTransaction(RelocatableStorageBackend &backend, bool readOnly,
											   std::unique_ptr<LogicalGraphVectorSearchSnapshot> snapshot,
											   LogicalGraphMutationFence fence, Generation metadataGeneration, RelocatableReadSnapshot rawSnapshot,
                std::shared_ptr<std::pmr::memory_resource> memory) :
				backend_(backend), read_only_(readOnly), snapshot_(std::move(snapshot)), fence_(std::move(fence)),
				metadata_generation_(metadataGeneration), raw_snapshot_(std::move(rawSnapshot)),
                write_memory_(std::move(memory)), retained_(std::in_place, write_memory_) {
				if (snapshot_ == nullptr) { // transaction construction only receives a snapshot opened by the initialized native backend.
					throw std::logic_error("relocatable logical transaction could not open its read snapshot");
				}
				if (!fence_.isValid()) { // the public transaction factory derives this fence from the validated snapshot generation.
					throw std::logic_error("relocatable logical transaction requires a valid graph-state read fence");
				}
				if (!metadata_generation_.isValid()) { // metadata generation comes from the same validated snapshot as the transaction fence.
					throw std::logic_error("relocatable logical transaction requires a valid metadata generation");
				}
			}

			~RelocatableLogicalGraphTransaction() override {
				if (state_ == LogicalGraphTransactionState::LGTTS_ACTIVE) {
					rollback();
				}
			}

			[[nodiscard]] LogicalGraphTransactionState state() const override { return state_; }
			[[nodiscard]] bool isReadOnly() const override { return read_only_; }
            [[nodiscard]] std::shared_ptr<std::pmr::memory_resource> retainedWriteMemory() const override {
                return write_memory_;
            }

			[[nodiscard]] LogicalGraphReadSnapshot &readSnapshot() override {
				requireActive("read its snapshot");
				if (hasStreamedBulkEdges()) {
					throw std::logic_error(
							"relocatable streamed bulk Edge transaction does not support read-your-writes reads");
				}
				reads_exposed_ = true;
				raw_snapshot_.release();
				return *snapshot_;
			}

			[[nodiscard]] LogicalGraphVectorSearchSnapshot &pinnedLogicalGraphVectorSearchSnapshot() override {
				requireActive("read its Vector snapshot");
				if (hasStreamedBulkEdges()) {
					throw std::logic_error("relocatable streamed bulk Edge transaction does not support Vector "
										   "read-your-writes reads");
				}
				reads_exposed_ = true;
				raw_snapshot_.release();
				return *snapshot_;
			}

			[[nodiscard]] LogicalGraphMutationBatchResult
			previewMutationBatch(const LogicalGraphMutationBatch &batch) const override {
				requireActive("preview a mutation batch");
                utils::OwnedAllocationScope writeOwner(write_memory_);
				if (read_only_) {
					throw std::logic_error("relocatable read-only logical transaction cannot preview graph mutations");
				}
				if (batch.node_creates.empty() && batch.edge_creates.empty()) {
					return {};
				}
				if (!retained_->preview_reservation.has_value()) {
                    auto reservation = reserveOwnedCreationIds(static_cast<uint64_t>(batch.node_creates.size()),
                        static_cast<uint64_t>(batch.edge_creates.size()));
                    retained_->preview_reservation.emplace(PendingCreationReservation{
                        batch.node_creates.size(), batch.edge_creates.size(), std::move(reservation)});
				} else if (retained_->preview_reservation->node_count != batch.node_creates.size() ||
						   retained_->preview_reservation->edge_count != batch.edge_creates.size()) {
					throw std::logic_error("relocatable logical transaction must stage or discard its outstanding "
										   "creation preview first");
				}
				return provisionalResult(retained_->preview_reservation->reservation, batch.node_creates.size(),
										 batch.edge_creates.size());
			}

			[[nodiscard]] LogicalGraphMutationBatchResult stageMutationBatch(LogicalGraphMutationBatch batch) override {
				return stageMutationBatchWithPropertyPatches(std::move(batch), {}, 0U);
			}

			[[nodiscard]] bool supportsPropertyPatchStaging() const override { return true; }

			[[nodiscard]] LogicalGraphMutationBatchResult stageMutationBatchWithPropertyPatches(
				LogicalGraphMutationBatch batch, std::vector<LogicalGraphPropertyPatchRequest> patches,
				size_t maximumPropertyEntries, const LogicalGraphMutationReadControl &control = {}) override {
				requireActive("stage a mutation batch");
                utils::OwnedAllocationScope writeOwner(write_memory_);
				if (control.checkpoint) control.checkpoint();
				if (hasStreamedBulkEdges()) {
					throw std::logic_error(
							"relocatable streamed bulk Edge transaction cannot stage ordinary overlay mutations");
				}
				if (read_only_) {
					throw std::logic_error("relocatable read-only logical transaction cannot stage graph mutations");
				}
				if (batch.creation_identity_plan.has_value()) {
					throw std::invalid_argument(
							"relocatable logical transaction owns creation identity reservation normalization");
				}
				size_t patchEntries = validateStagedPropertyPatches(batch, patches, maximumPropertyEntries, control);
				if (maximumPropertyEntries > (std::numeric_limits<size_t>::max)() - retained_->property_patch_entry_bound)
					throw std::overflow_error("logical transaction Property patch entry bound overflowed");
                auto payloadBudget = std::dynamic_pointer_cast<query::execution::MaterializedByteBudget>(write_memory_);
                if (!payloadBudget) throw std::logic_error("native staging requires an owning WRITE budget");
                // Declare credit before private copies so every failure frees data first.
                query::execution::MaterializedByteReservation nextPayload(payloadBudget.get());
                nextPayload.set(detail::mutationNestedHeapBytes<true>(retained_->accumulated,
                    retained_->property_patches, payloadBudget->availableWithParent(), control.checkpoint));
                auto input = detail::AdmittedMutationInput::copy(payloadBudget, batch, patches, control.checkpoint);
                auto &normalizedBatch = input->batch;
                auto &normalizedPatches = input->patches;
				const size_t nodeOffset = retained_->issued_node_ids.size();
				const size_t edgeOffset = retained_->issued_edge_ids.size();
				OwnedCreationReservation reservation(retained_->issued_node_ids.get_allocator().owner());
				bool reservedForThisStage = false;
				if (retained_->preview_reservation.has_value()) {
					if (retained_->preview_reservation->node_count != normalizedBatch.node_creates.size() ||
						retained_->preview_reservation->edge_count != normalizedBatch.edge_creates.size()) {
						throw std::logic_error("relocatable logical transaction stage does not match its outstanding "
											   "creation preview");
					}
					// Keep the preview capability intact until the complete statement
					// validates. A failed statement may be corrected and retried with
					// the same preview shape without leaking its outstanding lease.
					reservation = retained_->preview_reservation->reservation;
				} else if (!normalizedBatch.node_creates.empty() || !normalizedBatch.edge_creates.empty()) {
					reservation =
							reserveOwnedCreationIds(static_cast<uint64_t>(normalizedBatch.node_creates.size()),
																	static_cast<uint64_t>(normalizedBatch.edge_creates.size()));
					reservedForThisStage = true;
				}
				try {
					LogicalGraphMutationBatchResult result =
							provisionalResult(reservation, normalizedBatch.node_creates.size(), normalizedBatch.edge_creates.size());
					utils::OwnedVector<uint64_t> nextIssuedNodeIds(retained_->issued_node_ids.begin(), retained_->issued_node_ids.end(), retained_->issued_node_ids.get_allocator());
					utils::OwnedVector<uint64_t> nextIssuedEdgeIds(retained_->issued_edge_ids.begin(), retained_->issued_edge_ids.end(), retained_->issued_edge_ids.get_allocator());
					OwnedCreationReservation nextCreationReservations = retained_->creation_reservations;
					OwnedMutationBatch nextAccumulated(retained_->accumulated, normalizedBatch);
					utils::OwnedVector<LogicalGraphPropertyPatchRequest> nextPatches(retained_->property_patches.get_allocator());
                    if (normalizedPatches.size() > nextPatches.max_size() - retained_->property_patches.size())
                        throw std::length_error("logical transaction patch slots overflowed");
                    nextPatches.reserve(retained_->property_patches.size() + normalizedPatches.size());
                    nextPatches.insert(nextPatches.end(), retained_->property_patches.begin(), retained_->property_patches.end());
					rebaseCreatedReferences(normalizedBatch, retained_->issued_node_ids, retained_->issued_edge_ids);
					for (auto &patch : normalizedPatches)
						rebaseReference(patch.entity, nodeOffset, edgeOffset, retained_->issued_node_ids, retained_->issued_edge_ids);
					appendIssuedIds(reservation.node_ranges, nextIssuedNodeIds, nextCreationReservations.node_ranges, "Node");
					appendIssuedIds(reservation.edge_ranges, nextIssuedEdgeIds, nextCreationReservations.edge_ranges, "Edge");
					foldCreatedEntityFinalState(normalizedBatch, nodeOffset, edgeOffset, nextAccumulated);
					// Complete-map replacement resets all older field operations.
					for (const auto &replacement : normalizedBatch.property_replacements)
						std::erase_if(nextPatches, [&](const auto &patch) { return patch.entity == replacement.entity; });
					append(nextAccumulated, normalizedBatch);
					foldStagedPropertyPatches(nextAccumulated, nextPatches, normalizedPatches,
						patchEntries, maximumPropertyEntries, control);
                    if (control.checkpoint) control.checkpoint();
					debug::PerfTrace::addValue("native_graph.write.transaction_retained_property_targets",
						static_cast<int64_t>(std::min(nextAccumulated.property_replacements.size(),
							static_cast<size_t>((std::numeric_limits<int64_t>::max)()))));
					debug::PerfTrace::addValue("native_graph.write.transaction_retained_label_targets",
						static_cast<int64_t>(std::min(nextAccumulated.node_label_replacements.size(),
							static_cast<size_t>((std::numeric_limits<int64_t>::max)()))));
					size_t retainedEntries = 0U;
					for (const auto &patch : nextPatches)
						for (size_t count : {patch.sets.size(), patch.removals.size()}) {
							if (count > (std::numeric_limits<size_t>::max)() - retainedEntries)
								throw std::overflow_error("logical transaction retained Property entries overflowed");
							retainedEntries += count;
						}
					debug::PerfTrace::addValue("native_graph.write.transaction_retained_patch_targets",
						static_cast<int64_t>(std::min(nextPatches.size(),
							static_cast<size_t>((std::numeric_limits<int64_t>::max)()))));
					debug::PerfTrace::addValue("native_graph.write.transaction_retained_patch_entries",
						static_cast<int64_t>(std::min(retainedEntries,
							static_cast<size_t>((std::numeric_limits<int64_t>::max)()))));

                    input->transferNestedTo(nextPayload);
                    input.reset();
                    const auto actualPayload = detail::mutationNestedHeapBytes<false>(nextAccumulated,
                        nextPatches, nextPayload.bytes(), control.checkpoint);
                    nextPayload.set(actualPayload);
					if (control.checkpoint) control.checkpoint();
                // Returning the receipt must remain non-allocating even when
                // the compiler does not perform named return-value elision.
                static_assert(std::is_nothrow_move_constructible_v<LogicalGraphMutationBatchResult>);
					retained_->issued_node_ids = std::move(nextIssuedNodeIds);
					retained_->issued_edge_ids = std::move(nextIssuedEdgeIds);
					retained_->creation_reservations = std::move(nextCreationReservations);
					retained_->accumulated = std::move(nextAccumulated);
					retained_->property_patches = std::move(nextPatches);
                    retained_->nested_payload_charge = std::move(nextPayload);
					retained_->property_patch_entry_bound += maximumPropertyEntries;
					if (retained_->preview_reservation.has_value()) {
						retained_->preview_reservation.reset();
					}
					return result;
				} catch (...) {
					if (reservedForThisStage) {
						backend_.abandonLogicalGraphCreationIds(reservation.node_ranges, reservation.edge_ranges);
					}
					throw;
				}
			}

			[[nodiscard]] std::vector<uint64_t>
			appendPersistedBulkEdges(const LogicalGraphPersistedBulkEdgeAppendRequest &request) override {
				requireActive("append streamed bulk Edges");
                utils::OwnedAllocationScope writeOwner(write_memory_);
				if (read_only_) {
					throw std::logic_error("relocatable read-only logical transaction cannot append bulk Edges");
				}
				// The first append establishes the streamed-only transaction mode. Later
				// Driver ABI writes append to the same bounded scratch writer and must
				// retain their contiguous ID stream, while ordinary overlay work is still
				// rejected by stageMutationBatch().
				if (!hasStreamedBulkEdges() && (!isEmpty(retained_->accumulated) || !retained_->property_patches.empty() || !retained_->issued_node_ids.empty() ||
												!retained_->issued_edge_ids.empty() || retained_->preview_reservation.has_value())) {
					throw std::logic_error("relocatable streamed bulk Edge transaction requires an otherwise empty "
										   "logical transaction");
				}
				if (request.sourceCount() != request.targetCount() || request.sourceCount() == 0U ||
					request.sourceCount() > backend_.maximumNativeBulkEdgeAppendRecords() ||
					request.sourceCount() >
							backend_.maximumNativeBulkEdgeTransactionRecords() - streamed_edge_record_count_) {
					throw std::invalid_argument("relocatable streamed bulk Edge append has invalid endpoint dimensions "
												"or exceeds its bound");
				}
				for (const LogicalGraphBulkEdgePropertyColumn &column: request.property_columns) {
					if (column.key.empty() || column.values.size() != request.sourceCount()) {
						throw std::invalid_argument(
								"relocatable streamed bulk Edge append has an invalid Property column");
					}
				}
				for (size_t column = 0U; column < request.property_columns.size(); ++column) {
					for (size_t prior = 0U; prior < column; ++prior) {
						if (request.property_columns[column].key == request.property_columns[prior].key) {
							throw std::invalid_argument(
									"relocatable streamed bulk Edge append has duplicate Property keys");
						}
					}
				}

				// Reject every row-local error before acquiring a durable lease or
				// writing scratch, so failed input has no partial side effect. Resolve
				// each distinct endpoint once per bounded append; sorting lets the native
				// Directory perform one ordered descent instead of one lookup per row.
				std::vector<uint64_t> endpointIds;
				endpointIds.reserve(request.sourceCount() * 2U);
				for (size_t row = 0U; row < request.sourceCount(); ++row) {
					const uint64_t sourceNodeId = request.sourceNodeIdAt(row);
					const uint64_t targetNodeId = request.targetNodeIdAt(row);
					if (sourceNodeId == 0U || targetNodeId == 0U) {
						throw std::invalid_argument("relocatable streamed bulk Edge append has a zero endpoint");
					}
					endpointIds.push_back(sourceNodeId);
					endpointIds.push_back(targetNodeId);
				}
				std::ranges::sort(endpointIds);
				endpointIds.erase(std::unique(endpointIds.begin(), endpointIds.end()), endpointIds.end());
				std::vector<bool> endpointPresent;
				if (snapshot_->supportsNodeExistenceProbes()) {
					endpointPresent = snapshot_->containsNodesSorted(endpointIds);
					if (endpointPresent.size() != endpointIds.size()) {
						throw std::logic_error(
								"relocatable streamed bulk Edge endpoint existence cardinality changed");
					}
				} else {
					endpointPresent.reserve(endpointIds.size());
					for (const uint64_t endpointId : endpointIds) {
						endpointPresent.push_back(snapshot_->readNodeShaped(endpointId, {}).node_id == endpointId);
					}
				}
				for (size_t index = 0U; index < endpointIds.size(); ++index) {
					if (!endpointPresent[index]) {
						throw std::invalid_argument(
								"relocatable streamed bulk Edge append has a missing persisted endpoint");
					}
				}
				size_t appendEncodedBytes = 0U;
				for (size_t row = 0U; row < request.sourceCount(); ++row) {
					const uint64_t sourceNodeId = request.sourceNodeIdAt(row);
					const uint64_t targetNodeId = request.targetNodeIdAt(row);
					NativeBulkEdgeRecord probe;
					probe.edge_id = 1U;
					probe.source_node_id = sourceNodeId;
					probe.target_node_id = targetNodeId;
					probe.type = request.type;
					probe.properties.reserve(request.property_columns.size());
					for (const LogicalGraphBulkEdgePropertyColumn &column: request.property_columns) {
						probe.properties.push_back({std::string(column.key), column.values[row]});
					}
					const size_t encodedBytes = NativeBulkEdgeRecordCodec::encodedSize(probe);
					if (encodedBytes > backend_.maximumNativeBulkEdgeAppendBytes() - appendEncodedBytes) {
						throw std::invalid_argument(
								"relocatable streamed bulk Edge append exceeds its encoded-byte bound");
					}
					appendEncodedBytes += encodedBytes;
				}

				try {
					if (bulk_session_lease_ == nullptr) {
						bulk_session_lease_ = backend_.tryAcquireNativeBulkSessionLease();
						if (bulk_session_lease_ == nullptr) {
							throw LogicalGraphMutationConflict(
									"relocatable streamed bulk Edge session conflicts with another active bulk session");
						}
					}
					ensureStreamedEdgeLease(request.sourceCount());
					std::vector<uint64_t> ids = issueStreamedBulkEdgeIds(request.sourceCount());
					{
						auto publicationFence = backend_.acquireNativePublicationGate();
						ensureBulkEdgeScratchWriter();
						bulk_edge_writer_->refreshCheckpointFloor(backend_.nextNativeBulkScratchCheckpointLsn());
						for (size_t row = 0U; row < ids.size(); ++row) {
							NativeBulkEdgeRecord record;
							record.edge_id = ids[row];
							record.source_node_id = request.sourceNodeIdAt(row);
							record.target_node_id = request.targetNodeIdAt(row);
							record.type = request.type;
							record.properties.reserve(request.property_columns.size());
							for (const LogicalGraphBulkEdgePropertyColumn &column: request.property_columns) {
								record.properties.push_back({std::string(column.key), column.values[row]});
							}
							bulk_edge_writer_->add(std::move(record));
						}
						// Leasing IDs and sealing scratch advance metadata through this
						// transaction's own handle. Treat that generation as the local
						// baseline; commit only rebases a later external publication.
						metadata_generation_ = backend_.currentNativeMetadataGeneration();
					}
					streamed_edge_record_count_ += ids.size();
					return ids;
				} catch (...) {
					failStreamedBulkEdgeTransaction();
					throw;
				}
			}

			[[nodiscard]] bool hasStreamedBulkEdges() const override { return bulk_edge_writer_ != nullptr; }

			[[nodiscard]] LogicalGraphMutationBatchResult commit() override {
				requireActive("commit");
                utils::OwnedAllocationScope writeOwner(write_memory_);
				if (read_only_) {
                    LogicalGraphMutationBatchResult emptyResult;
					if (!isEmpty(retained_->accumulated) || !retained_->property_patches.empty()) { // read-only entry points reject writes before staging.
						throw std::logic_error(
								"relocatable read-only logical transaction contains staged graph mutations");
					}
					state_ = LogicalGraphTransactionState::LGTTS_COMMITTED;
                    releaseRetainedMutationState();
					snapshot_.reset();
					raw_snapshot_.release();
					return emptyResult;
				}
				if (hasStreamedBulkEdges()) {
                    LogicalGraphMutationBatchResult emptyResult;
					std::exception_ptr cleanup_failure;
					try {
						snapshot_.reset();
						raw_snapshot_.release();
						const bool maintenanceAttached = backend_.maintenanceEnabled();
						const bool maintenanceQuiesced =
								backend_.currentNativeMetadataGeneration() != metadata_generation_ &&
								backend_.quiesceNativeMaintenanceForExternalRebase();
						{
							auto publicationFence = backend_.acquireNativePublicationGate();
							// A streamed transaction never exposes reads after its first
							// append. Release its original snapshot before rebasing the
							// process-local metadata-pool registry to another handle's root.
						if (backend_.currentNativeMetadataGeneration() != metadata_generation_) {
								backend_.refreshNativeMetadataAfterExternalPublication(maintenanceAttached &&
																					   !maintenanceQuiesced);
							} else if (maintenanceQuiesced) {
								backend_.resumeMaintenance();
							}
							snapshot_ = backend_.openLogicalGraphVectorSearchSnapshot();
						if (snapshot_ == nullptr) { // streamed rebase reopens the snapshot through the initialized backend before continuing.
								throw std::logic_error("relocatable streamed bulk Edge transaction could not rebase "
													   "its read snapshot");
							}
							bulk_edge_writer_->refreshCheckpointFloor(backend_.nextNativeBulkScratchCheckpointLsn());
							bulk_edge_manifest_ = bulk_edge_writer_->finish();
						}
						const NativeBulkEdgeRecordSource source = nativeBulkEdgeRecordSource(
								*bulk_edge_store_, bulk_edge_manifest_->location, BULK_EDGE_MANIFEST_DEPTH);
						NativeStreamedGraphPublicationPlan plan;
						if (backend_.currentNativeLogicalGraphMutationFence() == fence_) {
							plan = NativeStreamedGraphPublicationPlan::derive(
									source, retained_->streamed_edge_reservation.edge_ranges,
									backend_.nativeBulkEdgePublicationPreflightOptions(), [this](uint64_t nodeId) {
										return snapshot_->supportsNodeExistenceProbes()
												   ? snapshot_->containsNode(nodeId)
											   : snapshot_->readNodeShaped(nodeId, {}).node_id == nodeId;
									});
						}
						backend_.commitNativeStreamedBulkEdges(*bulk_edge_store_, bulk_edge_manifest_->location,
															   retained_->streamed_edge_reservation, plan, fence_);
						bulk_edge_writer_.reset();
						bulk_edge_store_.reset();
						bulk_edge_manifest_.reset();
						bulk_session_lease_.reset();
						state_ = LogicalGraphTransactionState::LGTTS_COMMITTED;
                        releaseRetainedMutationState();
						snapshot_.reset();
						raw_snapshot_.release();
						return emptyResult;
					} catch (...) {
						cleanup_failure = std::current_exception();
					}
					try {
						failStreamedBulkEdgeTransaction();
					} catch (...) {
						// Preserve the publication/preflight failure as the caller-visible
						// cause. The cleanup path has already fenced this handle for reopen.
					}
					state_ = backend_.hasIndeterminateWriteOutcome() ? LogicalGraphTransactionState::LGTTS_INDETERMINATE
																	 : LogicalGraphTransactionState::LGTTS_ROLLED_BACK;
                    releaseRetainedMutationState();
					snapshot_.reset();
					raw_snapshot_.release();
					if (cleanup_failure != nullptr) {
						std::rethrow_exception(cleanup_failure);
					}
					throw std::logic_error( // the preceding catch always records the publication exception before cleanup.
							"relocatable streamed bulk Edge commit requires the native streaming materializer");
				}

				try {
					CanonicalMutation canonical = canonicalizeFinalState();
					const bool blindRebase = !reads_exposed_ && retained_->property_patches.empty() &&
						!canonical.batch.property_replacements.empty() && canonical.batch.property_replacements.size() <= 64U &&
						canonical.batch.node_creates.empty() && canonical.batch.edge_creates.empty() &&
						canonical.batch.node_label_replacements.empty() && canonical.batch.node_deletes.empty() &&
						canonical.batch.edge_deletes.empty() && !canonical.batch.creation_identity_plan &&
						std::ranges::all_of(canonical.batch.property_replacements, [](const auto &request) {
							return request.entity.entity_kind == LogicalGraphMutationEntityKind::LGMK_NODE &&
								request.entity.reference_kind == LogicalGraphMutationReferenceKind::LGRK_PERSISTED;
						});
					if (!blindRebase && (!canonical.batch.property_replacements.empty() || !retained_->property_patches.empty()) &&
						backend_.currentNativeLogicalGraphMutationFence() != fence_)
						throw LogicalGraphMutationConflict("logical transaction Property source fence changed before final planning");
					CanonicalPropertyPatches patches = blindRebase
                        ? CanonicalPropertyPatches(write_memory_, retained_->property_patches.size())
                        : extractFinalPropertyPatches(canonical.batch);
					appendCanonicalStagedPropertyPatches(canonical.batch, patches);
					LogicalGraphMutationBatchResult result = blindRebase
						? backend_.executeNativeBlindNodePropertyReplacements(canonical.batch, raw_snapshot_, fence_)
						: patches.requests.empty()
						? backend_.executeLogicalGraphMutationBatch(canonical.batch, fence_)
						: backend_.executeLogicalGraphMutationBatchWithPropertyPatches(
							*snapshot_, canonical.batch, patches.requests, patches.entry_bound, fence_);
					if (result != canonical.durable_result) { // the executor returns the canonical result produced from the same fenced batch.
						throw std::logic_error(
								"relocatable logical transaction publication changed its canonical creation identity");
					}
					abandonOutstandingCreationReservations();
					state_ = LogicalGraphTransactionState::LGTTS_COMMITTED;
                    releaseRetainedMutationState();
					snapshot_.reset();
					raw_snapshot_.release();
					return result;
				} catch (const LogicalGraphMutationConflict &) {
					abandonOutstandingCreationReservations();
					state_ = LogicalGraphTransactionState::LGTTS_CONFLICTED;
					releaseRetainedMutationState();
					snapshot_.reset();
					raw_snapshot_.release();
					throw;
				} catch (...) {
					// A failed logical commit must not leave a caller with a reusable
					// transaction whose staged state may no longer be meaningful.
					abandonOutstandingCreationReservations();
					state_ = backend_.hasIndeterminateWriteOutcome()
						? LogicalGraphTransactionState::LGTTS_INDETERMINATE
						: LogicalGraphTransactionState::LGTTS_ROLLED_BACK;
					releaseRetainedMutationState();
					snapshot_.reset();
					raw_snapshot_.release();
					throw;
				}
			}

			void rollback() override {
				if (state_ != LogicalGraphTransactionState::LGTTS_ACTIVE) {
					throw std::logic_error("relocatable logical transaction is no longer active");
				}
				std::exception_ptr cleanup_failure;
				try {
					failStreamedBulkEdgeTransaction();
				} catch (...) {
					cleanup_failure = std::current_exception();
				}
				abandonOutstandingCreationReservations();
				releaseRetainedMutationState();
				snapshot_.reset();
				raw_snapshot_.release();
				state_ = LogicalGraphTransactionState::LGTTS_ROLLED_BACK;
				if (cleanup_failure != nullptr) {
					std::rethrow_exception(cleanup_failure);
				}
			}

		private:
			static constexpr uint16_t BULK_EDGE_MANIFEST_DEPTH = 8U;

			void ensureStreamedEdgeLease(size_t requestedIds) {
				if (requestedIds == 0U) { // the public bulk append validates a positive row count before requesting a lease.
					throw std::logic_error("relocatable streamed bulk Edge lease requires positive work");
				}
				if (streamed_edge_next_id_ != 0U && streamed_edge_lease_last_id_ >= streamed_edge_next_id_ && // lease state is established and refreshed only by ensureStreamedEdgeLease.
					requestedIds <= streamed_edge_lease_last_id_ - streamed_edge_next_id_ + 1U) {
					return;
				}
				const size_t remainingRecordAllowance =
						backend_.maximumNativeBulkEdgeTransactionRecords() - streamed_edge_record_count_;
				const uint64_t leaseCount = static_cast<uint64_t>(
						std::min(remainingRecordAllowance,
								 std::max(requestedIds, backend_.nativeBulkEdgeIdLeaseBlockRecords())));
				NativeGraphLogicalIdReservation lease = backend_.reserveNativeBulkEdgeSessionIds(leaseCount);
					if (!lease.node_ranges.empty() || lease.edge_ranges.size() != 1U) { // reserveNativeBulkEdgeSessionIds constructs exactly one Edge-only range.
					backend_.abandonLogicalGraphCreationIds(lease);
					throw std::logic_error(
							"relocatable streamed bulk Edge transaction requires one exact durable lease");
				}
				const NativeGraphLogicalIdLeaseRange range = lease.edge_ranges.front();
					if (range.first_id == 0U || range.count != leaseCount || // the metadata lease API validates positive bounded ranges before returning.
					range.count - 1U > std::numeric_limits<uint64_t>::max() - range.first_id) {
					backend_.abandonLogicalGraphCreationIds(lease);
					throw std::logic_error(
							"relocatable streamed bulk Edge transaction received an invalid durable ID lease");
				}
				if (!retained_->streamed_edge_reservation.edge_ranges.empty()) {
					const NativeGraphLogicalIdLeaseRange previous = retained_->streamed_edge_reservation.edge_ranges.back();
						if (previous.count - 1U > std::numeric_limits<uint64_t>::max() - previous.first_id || // stored lease ranges are validated and strictly ordered when acquired.
						previous.first_id + previous.count - 1U >= range.first_id) {
						backend_.abandonLogicalGraphCreationIds(lease);
						throw std::logic_error("relocatable streamed bulk Edge leases are not strictly ordered");
					}
				}
				try {
					retained_->streamed_edge_reservation.edge_ranges.push_back(range);
				} catch (...) {
					backend_.abandonLogicalGraphCreationIds(lease);
					throw;
				}
				streamed_edge_next_id_ = range.first_id;
				streamed_edge_lease_last_id_ = range.first_id + range.count - 1U;
			}

			[[nodiscard]] std::vector<uint64_t> issueStreamedBulkEdgeIds(size_t count) {
					if (count == 0U || streamed_edge_next_id_ == 0U || // callers reserve and consume only positive IDs within the authenticated lease.
					streamed_edge_lease_last_id_ < streamed_edge_next_id_ ||
					count > streamed_edge_lease_last_id_ - streamed_edge_next_id_ + 1U) {
					throw std::logic_error("relocatable streamed bulk Edge transaction exhausted its durable ID lease");
				}
				std::vector<uint64_t> ids;
				ids.reserve(count);
				for (size_t ordinal = 0U; ordinal < count; ++ordinal) {
					ids.push_back(streamed_edge_next_id_);
					streamed_edge_next_id_ = streamed_edge_next_id_ == std::numeric_limits<uint64_t>::max()
													 ? 0U
													 : streamed_edge_next_id_ + 1U;
				}
				return ids;
			}

			void ensureBulkEdgeScratchWriter() {
				if (bulk_edge_writer_ != nullptr) {
					return;
				}
				const uint64_t owner = backend_.allocateNativeBulkScratchOwner();
				auto store = backend_.createNativeBulkRunStore();
				auto writer = std::make_unique<NativeBulkEdgeRunStreamingWriter>(
						*store, owner,
						NativeBulkEdgeRunStreamingOptions{
								RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES,
								static_cast<uint64_t>(backend_.maximumNativeBulkEdgeTransactionRecords()),
								BULK_EDGE_MANIFEST_DEPTH},
						backend_.nextNativeBulkScratchCheckpointLsn());
				bulk_edge_store_ = std::move(store);
				bulk_edge_writer_ = std::move(writer);
			}

			void failStreamedBulkEdgeTransaction() {
				std::exception_ptr cleanup_failure;
				if (bulk_edge_writer_ != nullptr && bulk_edge_store_ != nullptr) { // these paired objects are created and retired together by the transaction.
					try {
						auto publicationFence = backend_.acquireNativePublicationGate();
						try {
							if (bulk_edge_manifest_.has_value()) {
								releaseNativeBulkEdgeManifest(*bulk_edge_store_, bulk_edge_manifest_->location,
															  backend_.nextNativeBulkScratchCheckpointLsn());
							} else {
								bulk_edge_writer_->refreshCheckpointFloor(backend_.nextNativeBulkScratchCheckpointLsn());
								bulk_edge_writer_->abort(backend_.nextNativeBulkScratchCheckpointLsn());
							}
						} catch (...) {
							// A sealed source may be malformed, or finish() may have been
							// interrupted after publishing scratch but before returning its
							// root. The held exclusive session lease proves all unpublished
							// scratch is safe to reclaim synchronously.
							bulk_edge_store_->reclaimUnpublishedOnOpen(backend_.nextNativeBulkScratchCheckpointLsn());
						}
					} catch (...) {
						cleanup_failure = std::current_exception();
						// Scratch is graph-unreachable, but an ambiguous allocator
						// publication still requires authoritative reopen recovery.
						backend_.requireReopenAfterNativeBulkStagingFailure();
					}
					bulk_edge_writer_.reset();
					bulk_edge_store_.reset();
					bulk_edge_manifest_.reset();
				}
                if (retained_ && !retained_->streamed_edge_reservation.empty()) {
                    backend_.abandonLogicalGraphCreationIds(retained_->streamed_edge_reservation);
                    // Keep capacity while this transaction remains active. The
                    // optional state frees it at termination without new proxies.
                    retained_->streamed_edge_reservation.node_ranges.clear();
                    retained_->streamed_edge_reservation.edge_ranges.clear();
                }
				streamed_edge_next_id_ = 0U;
				streamed_edge_lease_last_id_ = 0U;
				streamed_edge_record_count_ = 0U;
				// Release only after scratch and its durable ID capability have been
				// retired, so a retry cannot race cleanup through another handle.
				bulk_session_lease_.reset();
				if (cleanup_failure != nullptr) {
					std::rethrow_exception(cleanup_failure);
				}
			}

			[[nodiscard]] static std::optional<size_t> findIssuedOrdinal(std::span<const uint64_t> ids,
																		 uint64_t id) {
				const auto iterator = std::lower_bound(ids.begin(), ids.end(), id);
				if (iterator == ids.end() || *iterator != id) {
					return std::nullopt;
				}
				return static_cast<size_t>(iterator - ids.begin());
			}

			static void rebaseReference(LogicalGraphMutationEntityReference &reference, size_t nodeOffset,
										size_t edgeOffset, std::span<const uint64_t> issuedNodeIds,
										std::span<const uint64_t> issuedEdgeIds) {
				if (reference.entity_kind != LogicalGraphMutationEntityKind::LGMK_NODE && // public mutation builders validate entity references before rebasing.
					reference.entity_kind != LogicalGraphMutationEntityKind::LGMK_EDGE) {
					throw std::invalid_argument("relocatable logical transaction has an unknown entity kind");
				}
				uint64_t offset = 0U;
				switch (reference.reference_kind) {
					case LogicalGraphMutationReferenceKind::LGRK_PERSISTED:
						if (reference.entity_kind == LogicalGraphMutationEntityKind::LGMK_NODE &&
							findIssuedOrdinal(issuedNodeIds, reference.value).has_value()) {
							reference.reference_kind = LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE;
							reference.value = *findIssuedOrdinal(issuedNodeIds, reference.value);
						} else if (reference.entity_kind == LogicalGraphMutationEntityKind::LGMK_EDGE &&
								   findIssuedOrdinal(issuedEdgeIds, reference.value).has_value()) {
							reference.reference_kind = LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE;
							reference.value = *findIssuedOrdinal(issuedEdgeIds, reference.value);
						}
						return;
					case LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE:
						offset = static_cast<uint64_t>(nodeOffset);
						break;
					case LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE:
						offset = static_cast<uint64_t>(edgeOffset);
						break;
					default:
						throw std::invalid_argument(
								"relocatable logical transaction has an unknown entity reference kind");
				}
				if (reference.value > std::numeric_limits<uint64_t>::max() - offset) { // public creation IDs are bounded by the metadata lease and cannot overflow rebasing.
					throw std::overflow_error("relocatable logical transaction batch reference ordinal overflow");
				}
				reference.value += offset;
			}

			static void rebaseCreatedReferences(LogicalGraphMutationBatch &batch,
												std::span<const uint64_t> issuedNodeIds,
												std::span<const uint64_t> issuedEdgeIds) {
				const size_t nodeOffset = issuedNodeIds.size();
				const size_t edgeOffset = issuedEdgeIds.size();
					if (nodeOffset > static_cast<size_t>(std::numeric_limits<uint64_t>::max()) || // transaction creation vectors are bounded by the uint64 logical-ID domain.
					edgeOffset > static_cast<size_t>(std::numeric_limits<uint64_t>::max())) {
					throw std::overflow_error(
							"relocatable logical transaction batch offset exceeds reference capacity");
				}
				for (LogicalGraphBatchEdgeCreateRequest &edge: batch.edge_creates) {
					rebaseReference(edge.source, nodeOffset, edgeOffset, issuedNodeIds, issuedEdgeIds);
					rebaseReference(edge.target, nodeOffset, edgeOffset, issuedNodeIds, issuedEdgeIds);
				}
				for (LogicalGraphPropertyMutationRequest &property: batch.property_replacements) {
					rebaseReference(property.entity, nodeOffset, edgeOffset, issuedNodeIds, issuedEdgeIds);
				}
				for (LogicalGraphNodeLabelMutationRequest &label: batch.node_label_replacements) {
					rebaseReference(label.node, nodeOffset, edgeOffset, issuedNodeIds, issuedEdgeIds);
				}
			}

			[[nodiscard]] static std::vector<uint64_t>
			collectIssuedIds(std::span<const NativeGraphLogicalIdLeaseRange> ranges, size_t expectedCount,
							 const char *entityName) {
				std::vector<uint64_t> ids;
				ids.reserve(expectedCount);
				for (const NativeGraphLogicalIdLeaseRange &range: ranges) {
						if (range.first_id == 0U || range.count == 0U || // metadata-issued ranges are validated before canonicalization.
						range.count - 1U > std::numeric_limits<uint64_t>::max() - range.first_id ||
						range.count > expectedCount - ids.size()) {
						throw std::overflow_error(std::string("relocatable logical transaction ") + entityName +
												  " received an invalid durable ID lease");
					}
					for (uint64_t offset = 0U; offset < range.count; ++offset) {
						ids.push_back(range.first_id + offset);
					}
				}
				if (ids.size() != expectedCount || !std::ranges::is_sorted(ids) || // canonicalization receives ranges from the validated reservation capability.
					std::ranges::adjacent_find(ids) != ids.end()) {
					throw std::logic_error(std::string("relocatable logical transaction ") + entityName +
										   " received a noncanonical durable ID lease");
				}
				return ids;
			}

			static void appendIssuedIds(std::span<const NativeGraphLogicalIdLeaseRange> ranges,
										utils::OwnedVector<uint64_t> &ids,
										utils::OwnedVector<NativeGraphLogicalIdLeaseRange> &identityRanges,
										const char *entityName) {
				for (const NativeGraphLogicalIdLeaseRange &range: ranges) {
						if (range.first_id == 0U || range.count == 0U || // metadata-issued ranges are validated before canonicalization.
						range.count - 1U > std::numeric_limits<uint64_t>::max() - range.first_id ||
						range.count > std::numeric_limits<size_t>::max() - ids.size() ||
						(!ids.empty() && ids.back() >= range.first_id)) {
						throw std::logic_error(std::string("relocatable logical transaction ") + entityName +
											   " received a noncanonical durable ID lease");
					}
					ids.reserve(ids.size() + static_cast<size_t>(range.count));
					for (uint64_t offset = 0U; offset < range.count; ++offset) {
						ids.push_back(range.first_id + offset);
					}
					identityRanges.push_back({range.first_id, range.count});
				}
			}

            template<class Reservation>
			[[nodiscard]] static LogicalGraphMutationBatchResult
			provisionalResult(const Reservation &reservation, size_t nodeCount, size_t edgeCount) {
				LogicalGraphMutationBatchResult result;
				result.created_node_ids = collectIssuedIds(reservation.node_ranges, nodeCount, "Node");
				result.created_edge_ids = collectIssuedIds(reservation.edge_ranges, edgeCount, "Edge");
				return result;
			}

			template<typename Mutation, typename Reference>
			static void mergeFinalReplacements(utils::OwnedVector<Mutation> &destination, std::vector<Mutation> &source,
				const Reference &reference) {
				const auto target = [&](const Mutation &mutation) {
					const auto &entity = reference(mutation);
					return std::tuple{static_cast<uint8_t>(entity.entity_kind),
						static_cast<uint8_t>(entity.reference_kind), entity.value};
				};
				std::pmr::map<std::tuple<uint8_t, uint8_t, uint64_t>, size_t> positions(destination.get_allocator().resource());
				for (size_t index = 0U; index < destination.size(); ++index)
					positions.emplace(target(destination[index]), index);
				for (auto &replacement : source) {
					const auto key = target(replacement);
					if (const auto previous = positions.find(key); previous != positions.end()) {
						// The statement overlay already owns intermediate visibility.
						// Retain one authoritative final collection per entity here.
						destination[previous->second] = std::move(replacement);
					} else {
						positions.emplace(key, destination.size());
						destination.push_back(std::move(replacement));
					}
				}
			}

			static void append(OwnedMutationBatch &destination, LogicalGraphMutationBatch &source) {
				destination.node_creates.insert(destination.node_creates.end(),
					std::make_move_iterator(source.node_creates.begin()), std::make_move_iterator(source.node_creates.end()));
				destination.edge_creates.insert(destination.edge_creates.end(),
					std::make_move_iterator(source.edge_creates.begin()), std::make_move_iterator(source.edge_creates.end()));
				mergeFinalReplacements(destination.property_replacements, source.property_replacements,
					[](const auto &mutation) -> const auto & { return mutation.entity; });
				mergeFinalReplacements(destination.node_label_replacements, source.node_label_replacements,
					[](const auto &mutation) -> const auto & { return mutation.node; });
				destination.edge_deletes.insert(destination.edge_deletes.end(),
												std::make_move_iterator(source.edge_deletes.begin()),
												std::make_move_iterator(source.edge_deletes.end()));
				destination.node_deletes.insert(destination.node_deletes.end(),
												std::make_move_iterator(source.node_deletes.begin()),
												std::make_move_iterator(source.node_deletes.end()));
			}

			static auto propertyTarget(const LogicalGraphMutationEntityReference &entity) {
				return std::tuple{static_cast<uint8_t>(entity.entity_kind),
					static_cast<uint8_t>(entity.reference_kind), entity.value};
			}

			size_t validateStagedPropertyPatches(const LogicalGraphMutationBatch &batch,
				const std::vector<LogicalGraphPropertyPatchRequest> &patches, size_t maximumEntries,
				const LogicalGraphMutationReadControl &control) const {
				const auto checkpoint = [&] { if (control.checkpoint) control.checkpoint(); };
				if (!patches.empty() && (maximumEntries == 0U || patches.size() > maximumEntries))
					throw std::invalid_argument("logical transaction Property patches require a positive work bound");
				size_t entries = 0U;
				std::pmr::set<std::tuple<uint8_t, uint8_t, uint64_t>> targets(retained_->issued_node_ids.get_allocator().resource());
				for (const auto &patch : patches) {
					checkpoint();
					const auto &entity = patch.entity;
					const bool node = entity.entity_kind == LogicalGraphMutationEntityKind::LGMK_NODE;
					const bool edge = entity.entity_kind == LogicalGraphMutationEntityKind::LGMK_EDGE;
					const bool valid = ((node || edge) && entity.reference_kind == LogicalGraphMutationReferenceKind::LGRK_PERSISTED && entity.value != 0U) ||
						(node && entity.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE && entity.value < batch.node_creates.size()) ||
						(edge && entity.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE && entity.value < batch.edge_creates.size());
					if (!valid || !targets.insert(propertyTarget(entity)).second)
						throw std::invalid_argument("logical transaction Property patches require distinct valid targets");
					for (size_t count : {patch.sets.size(), patch.removals.size()}) {
						if (count > maximumEntries - entries)
							throw std::length_error("logical transaction Property patches exceeded their entry bound");
						entries += count;
					}
					std::pmr::set<std::string_view> keys(retained_->issued_node_ids.get_allocator().resource());
					for (const auto &field : patch.sets) {
						checkpoint();
						if (field.key.empty() || field.value.getType() == PropertyType::NULL_TYPE || !keys.insert(field.key).second)
							throw std::invalid_argument("logical transaction Property sets require unique non-null fields");
					}
					for (const auto &key : patch.removals) {
						checkpoint();
						if (key.empty() || !keys.insert(key).second)
							throw std::invalid_argument("logical transaction Property removals must be unique and disjoint");
					}
				}
				for (const auto &replacement : batch.property_replacements)
					if (targets.contains(propertyTarget(replacement.entity)))
						throw std::invalid_argument("logical transaction Property patches overlap complete replacements");
				for (const auto &deletion : batch.node_deletes)
					if (targets.contains(propertyTarget({LogicalGraphMutationEntityKind::LGMK_NODE,
						LogicalGraphMutationReferenceKind::LGRK_PERSISTED, deletion.node_id})))
						throw std::invalid_argument("logical transaction Property patch targets a deleted Node");
				for (const auto &deletion : batch.edge_deletes)
					if (targets.contains(propertyTarget({LogicalGraphMutationEntityKind::LGMK_EDGE,
						LogicalGraphMutationReferenceKind::LGRK_PERSISTED, deletion.edge_id})))
						throw std::invalid_argument("logical transaction Property patch targets a deleted Edge");
				return entries;
			}

            void applyStagedPropertyPatch(std::vector<LogicalGraphWriteProperty> &properties,
                LogicalGraphPropertyPatchRequest &patch, const LogicalGraphMutationReadControl &control) const {
                auto *budget = requirePayloadBudget(write_memory_);
                query::execution::MaterializedByteReservation outputCharge(budget);
                // Borrow inputs without moving their heap headers. Map slots are
                // PMR-owned; the complete output is admitted before its first copy.
                std::pmr::map<std::string_view, std::pair<const std::string *, const PropertyValue *>> fields(budget);
                const auto checkpoint = [&] { if (control.checkpoint) control.checkpoint(); };
                for (const auto &field : properties) {
                    checkpoint();
                    if (field.key.empty() || !fields.emplace(field.key, std::pair{&field.key, &field.value}).second)
                        throw std::invalid_argument("logical transaction Property source has duplicate or empty keys");
                }
                for (const auto &field : patch.sets) { checkpoint(); fields.insert_or_assign(field.key, std::pair{&field.key, &field.value}); }
                for (const auto &key : patch.removals) { checkpoint(); fields.erase(key); }
                detail::WriteCopySizer meter(budget->availableWithParent(), control.checkpoint);
                meter.add(detail::writeVectorCopyHeapBytes<LogicalGraphWriteProperty>(fields.size()));
                size_t temporaryHeader = 0U;
                for (const auto &[key, value] : fields) {
                    meter.tick();
                    if (value.second) temporaryHeader = (std::max)(temporaryHeader,
                        detail::movedPropertyHeaderHeapUpper(*value.second));
                    meter.add(detail::writeStringCopyHeapUpper(*value.first)); meter.property(*value.second);
                }
                // variant copy assignment may create one temporary before a
                // nonthrowing move into its previously scalar alternative.
                meter.add(temporaryHeader);
                outputCharge.set(meter.bytes());
                std::vector<LogicalGraphWriteProperty> output; output.reserve(fields.size());
                for (const auto &[key, value] : fields) {
                    checkpoint();
                    // Construct members in place, avoiding an allocating moved
                    // PropertyValue header on MSVC.
                    auto &field = output.emplace_back();
                    field.key.assign(key.data(), key.size()); field.value = *value.second;
                }
                checkpoint(); properties.swap(output);
                // The output may contain copies of incoming fields. Free those
                // consumed private inputs before refunding the output workspace;
                // their retained-input credit now covers the adopted output.
                std::vector<LogicalGraphWriteProperty>().swap(patch.sets);
                std::vector<std::string>().swap(patch.removals);
            }

            void mergeStagedPropertyPatch(LogicalGraphPropertyPatchRequest &previous,
                LogicalGraphPropertyPatchRequest &next, const LogicalGraphMutationReadControl &control) const {
                auto *budget = requirePayloadBudget(write_memory_);
                query::execution::MaterializedByteReservation outputCharge(budget);
                std::pmr::map<std::string_view, std::pair<const std::string *, const PropertyValue *>> fields(budget);
                const auto checkpoint = [&] { if (control.checkpoint) control.checkpoint(); };
                for (const auto &field : previous.sets) { checkpoint(); fields.emplace(field.key, std::pair{&field.key, &field.value}); }
                for (const auto &key : previous.removals) { checkpoint(); fields.emplace(key, std::pair<const std::string *, const PropertyValue *>{&key, nullptr}); }
                for (const auto &field : next.sets) { checkpoint(); fields.insert_or_assign(field.key, std::pair{&field.key, &field.value}); }
                for (const auto &key : next.removals) { checkpoint(); fields.insert_or_assign(key, std::pair<const std::string *, const PropertyValue *>{&key, nullptr}); }
                size_t sets = 0U;
                for (const auto &[key, value] : fields) if (value.second) ++sets;
                detail::WriteCopySizer meter(budget->availableWithParent(), control.checkpoint);
                meter.add(detail::writeVectorCopyHeapBytes<LogicalGraphWriteProperty>(sets));
                meter.add(detail::writeVectorCopyHeapBytes<std::string>(fields.size() - sets));
                size_t temporaryHeader = 0U;
                for (const auto &[key, value] : fields) {
                    meter.tick();
                    if (value.second) temporaryHeader = (std::max)(temporaryHeader,
                        detail::movedPropertyHeaderHeapUpper(*value.second));
                    meter.add(detail::writeStringCopyHeapUpper(*value.first));
                    if (value.second) meter.property(*value.second);
                }
                // variant copy assignment may create one temporary before a
                // nonthrowing move into its previously scalar alternative.
                meter.add(temporaryHeader);
                outputCharge.set(meter.bytes());
                std::vector<LogicalGraphWriteProperty> outputSets; outputSets.reserve(sets);
                std::vector<std::string> outputRemovals; outputRemovals.reserve(fields.size() - sets);
                for (const auto &[key, value] : fields) {
                    checkpoint();
                    if (value.second) {
                        auto &field = outputSets.emplace_back();
                        field.key.assign(key.data(), key.size()); field.value = *value.second;
                    } else outputRemovals.emplace_back(key);
                }
                checkpoint(); previous.sets.swap(outputSets); previous.removals.swap(outputRemovals);
                std::vector<LogicalGraphWriteProperty>().swap(next.sets);
                std::vector<std::string>().swap(next.removals);
            }

			void foldStagedPropertyPatches(OwnedMutationBatch &accumulated,
				utils::OwnedVector<LogicalGraphPropertyPatchRequest> &retained,
				std::vector<LogicalGraphPropertyPatchRequest> &patches, size_t &entries, size_t maximumEntries,
				const LogicalGraphMutationReadControl &control) const {
				for (auto &patch : patches) {
					if (control.checkpoint) control.checkpoint();
					std::vector<LogicalGraphWriteProperty> *source = nullptr;
					if (patch.entity.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE)
						source = &accumulated.node_creates.at(static_cast<size_t>(patch.entity.value)).properties;
					else if (patch.entity.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE)
						source = &accumulated.edge_creates.at(static_cast<size_t>(patch.entity.value)).properties;
					else {
						const auto full = std::ranges::find(accumulated.property_replacements,
							patch.entity, &LogicalGraphPropertyMutationRequest::entity);
						if (full != accumulated.property_replacements.end()) source = &full->properties;
					}
					if (source) {
						if (source->size() > maximumEntries - entries)
							throw std::length_error("logical transaction Property patch source exceeded its entry bound");
						entries += source->size();
						applyStagedPropertyPatch(*source, patch, control);
						continue;
					}
					const auto previous = std::ranges::find(retained, patch.entity, &LogicalGraphPropertyPatchRequest::entity);
					if (previous == retained.end()) retained.push_back(std::move(patch));
					else mergeStagedPropertyPatch(*previous, patch, control);
				}
			}

			void foldCreatedEntityFinalState(LogicalGraphMutationBatch &batch, size_t nodeOffset, size_t edgeOffset,
											 OwnedMutationBatch &previouslyAccumulated) {
				const auto nodeCreate = [&previouslyAccumulated, &batch,
										 nodeOffset](uint64_t ordinal) -> LogicalGraphNodeCreateRequest & {
					if (ordinal < nodeOffset) {
						return previouslyAccumulated.node_creates.at(static_cast<size_t>(ordinal));
					}
					const uint64_t local = ordinal - nodeOffset;
					if (local >= batch.node_creates.size()) {
						throw std::invalid_argument(
								"relocatable logical transaction has an invalid created Node reference");
					}
					return batch.node_creates.at(static_cast<size_t>(local));
				};
				const auto edgeCreate = [&previouslyAccumulated, &batch,
										 edgeOffset](uint64_t ordinal) -> LogicalGraphBatchEdgeCreateRequest & {
					if (ordinal < edgeOffset) {
						return previouslyAccumulated.edge_creates.at(static_cast<size_t>(ordinal));
					}
					const uint64_t local = ordinal - edgeOffset;
					if (local >= batch.edge_creates.size()) {
						throw std::invalid_argument(
								"relocatable logical transaction has an invalid created Edge reference");
					}
					return batch.edge_creates.at(static_cast<size_t>(local));
				};

                static_assert(std::is_nothrow_move_assignable_v<LogicalGraphPropertyMutationRequest>);
                static_assert(std::is_nothrow_move_assignable_v<LogicalGraphNodeLabelMutationRequest>);
                auto *const memory = retained_->issued_node_ids.get_allocator().resource();
                std::pmr::set<std::tuple<uint8_t, uint64_t>> createdPropertyTargets(memory);
                std::pmr::set<std::tuple<uint8_t, uint64_t>> persistedPropertyTargets(memory);
                // Only the disposable statement and its draft are modified here.
                // Stable compaction avoids a second unaccounted request buffer.
                std::erase_if(batch.property_replacements, [&](LogicalGraphPropertyMutationRequest &mutation) {
					switch (mutation.entity.reference_kind) {
						case LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE:
							if (mutation.entity.entity_kind != LogicalGraphMutationEntityKind::LGMK_NODE ||
								!createdPropertyTargets
										 .insert({static_cast<uint8_t>(mutation.entity.reference_kind),
												  mutation.entity.value})
										 .second) {
								throw std::invalid_argument("relocatable logical transaction has duplicate or invalid "
															"created Node Properties");
							}
							nodeCreate(mutation.entity.value).properties = std::move(mutation.properties);
                            return true;
					case LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE:
						if (mutation.entity.entity_kind != LogicalGraphMutationEntityKind::LGMK_EDGE ||
								!createdPropertyTargets
										 .insert({static_cast<uint8_t>(mutation.entity.reference_kind),
												  mutation.entity.value})
										 .second) {
								throw std::invalid_argument("relocatable logical transaction has duplicate or invalid "
															"created Edge Properties");
							}
							edgeCreate(mutation.entity.value).properties = std::move(mutation.properties);
                            return true;
						case LogicalGraphMutationReferenceKind::LGRK_PERSISTED:
							if (!persistedPropertyTargets
										 .insert({static_cast<uint8_t>(mutation.entity.entity_kind),
												  mutation.entity.value})
										 .second) {
								throw std::invalid_argument(
										"relocatable logical transaction has duplicate persisted Property replacements "
										"within one statement");
							}
							return false;
						default:
							throw std::invalid_argument(
									"relocatable logical transaction has an unknown Property reference kind");
					}
                });

                std::pmr::set<uint64_t> createdLabelTargets(memory);
                std::pmr::set<uint64_t> persistedLabelTargets(memory);
                std::erase_if(batch.node_label_replacements, [&](LogicalGraphNodeLabelMutationRequest &mutation) {
					switch (mutation.node.reference_kind) {
						case LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE:
							if (mutation.node.entity_kind != LogicalGraphMutationEntityKind::LGMK_NODE ||
								!createdLabelTargets.insert(mutation.node.value).second) {
								throw std::invalid_argument(
										"relocatable logical transaction has duplicate or invalid created Node labels");
							}
							nodeCreate(mutation.node.value).labels = std::move(mutation.labels);
                            return true;
						case LogicalGraphMutationReferenceKind::LGRK_PERSISTED:
							if (!persistedLabelTargets.insert(mutation.node.value).second) {
								throw std::invalid_argument("relocatable logical transaction has duplicate persisted "
															"Node label replacements "
															"within one statement");
							}
							return false;
						case LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE:
						default:
							throw std::invalid_argument(
									"relocatable logical transaction has an unknown Node label reference kind");
					}
                });
			}

            struct CanonicalMutation {
                query::execution::MaterializedByteReservation payload_charge;
                LogicalGraphMutationBatch batch;
                LogicalGraphMutationBatchResult durable_result;
                static query::execution::MaterializedByteReservation admit(
                        query::execution::MaterializedByteBudget *budget, const OwnedMutationBatch &source,
                        size_t nodeRanges, size_t edgeRanges) {
                    const std::function<void()> checkpoint;
                    detail::WriteCopySizer meter(budget->availableWithParent(), checkpoint);
                    meter.add(detail::mutationNestedHeapBytes<true>(source,
                        std::span<const LogicalGraphPropertyPatchRequest>{}, budget->availableWithParent()));
                    const auto slots = [&]<class T>(const auto &values, std::type_identity<T>) {
                        meter.add(detail::writeVectorCopyHeapBytes<T>(values.size()));
                    };
                    slots(source.node_creates, std::type_identity<LogicalGraphNodeCreateRequest>{});
                    slots(source.edge_creates, std::type_identity<LogicalGraphBatchEdgeCreateRequest>{});
                    slots(source.property_replacements, std::type_identity<LogicalGraphPropertyMutationRequest>{});
                    meter.add(query::execution::detail::multiplyPropertyBufferBytes(
                        source.property_replacements.size(), 2U * query::execution::detail::standardContainerProxyBytes()));
                    slots(source.node_label_replacements, std::type_identity<LogicalGraphNodeLabelMutationRequest>{});
                    slots(source.edge_deletes, std::type_identity<LogicalGraphEdgeDeleteRequest>{});
                    slots(source.node_deletes, std::type_identity<LogicalGraphNodeDeleteRequest>{});
                    meter.add(detail::writeVectorCopyHeapBytes<uint64_t>(source.node_creates.size()));
                    meter.add(detail::writeVectorCopyHeapBytes<uint64_t>(source.edge_creates.size()));
                    if (!source.node_creates.empty() || !source.edge_creates.empty()) {
                        using Range = decltype(LogicalGraphCreationIdentityPlan::node_id_ranges)::value_type;
                        meter.add(detail::writeVectorCopyHeapBytes<Range>(nodeRanges));
                        meter.add(detail::writeVectorCopyHeapBytes<Range>(edgeRanges));
                        meter.add(detail::writeVectorCopyHeapBytes<uint64_t>(source.node_creates.size()));
                        meter.add(detail::writeVectorCopyHeapBytes<uint64_t>(source.edge_creates.size()));
                        // Moving the local plan leaves four empty debug headers.
                        meter.add(4U * query::execution::detail::standardContainerProxyBytes());
                    }
                    query::execution::MaterializedByteReservation charge(budget);
                    charge.set(meter.bytes()); return charge;
                }
                CanonicalMutation(query::execution::MaterializedByteBudget *budget,
                        const OwnedMutationBatch &source, size_t nodeRanges, size_t edgeRanges)
                    : payload_charge(admit(budget, source, nodeRanges, edgeRanges)) {
                    batch.node_creates.reserve(source.node_creates.size());
                    batch.edge_creates.reserve(source.edge_creates.size());
                    batch.property_replacements.reserve(source.property_replacements.size());
                    batch.node_label_replacements.reserve(source.node_label_replacements.size());
                    batch.edge_deletes.reserve(source.edge_deletes.size());
                    batch.node_deletes.reserve(source.node_deletes.size());
                    durable_result.created_node_ids.reserve(source.node_creates.size());
                    durable_result.created_edge_ids.reserve(source.edge_creates.size());
                }
                CanonicalMutation(CanonicalMutation &&) noexcept = default;
            };

            struct CanonicalPropertyPatches {
                query::execution::MaterializedByteBudget *budget;
                query::execution::MaterializedByteReservation payload_charge;
                utils::OwnedVector<LogicalGraphPropertyPatchRequest> requests;
                size_t entry_bound = 0U;
                CanonicalPropertyPatches(const std::shared_ptr<std::pmr::memory_resource> &memory, size_t maximumRequests)
                    : budget(requirePayloadBudget(memory)), payload_charge(budget),
                      requests(utils::OwnedPmrAllocator<LogicalGraphPropertyPatchRequest>(memory)) {
                    requests.reserve(maximumRequests);
                }
                CanonicalPropertyPatches(CanonicalPropertyPatches &&) noexcept = default;
                void admitPatch(std::span<const LogicalGraphWriteProperty> sets,
                        std::span<const std::string> removals, bool willMove) {
                    const std::function<void()> checkpoint;
                    detail::WriteCopySizer meter(budget->availableWithParent(), checkpoint);
                    meter.properties(sets); meter.strings(removals);
                    if (willMove) meter.add(2U * query::execution::detail::standardContainerProxyBytes());
                    payload_charge.set(query::execution::detail::addPropertyBufferBytes(payload_charge.bytes(), meter.bytes()));
                }
                void appendCopy(const LogicalGraphPropertyPatchRequest &patch) {
                    admitPatch(patch.sets, patch.removals, false);
                    requests.push_back(patch);
                }
            };

			void appendCanonicalStagedPropertyPatches(const LogicalGraphMutationBatch &batch,
				CanonicalPropertyPatches &result) const {
				if (retained_->property_patches.empty()) return;
				auto *const memory = retained_->issued_node_ids.get_allocator().resource();
                std::pmr::set<uint64_t> deletedNodes(memory), detachedNodes(memory), deletedEdges(memory);
				for (const auto &deletion : batch.node_deletes) {
					deletedNodes.insert(deletion.node_id);
					if (deletion.detach) detachedNodes.insert(deletion.node_id);
				}
				for (const auto &deletion : batch.edge_deletes) deletedEdges.insert(deletion.edge_id);
				for (const auto &patch : retained_->property_patches) {
					if (patch.entity.entity_kind == LogicalGraphMutationEntityKind::LGMK_NODE) {
						if (deletedNodes.contains(patch.entity.value)) continue;
					} else {
						if (deletedEdges.contains(patch.entity.value)) continue;
						if (!detachedNodes.empty()) {
							const auto edge = snapshot_->readEdgeShaped(patch.entity.value, {.include_type = false});
							if (detachedNodes.contains(edge.source_node_id) || detachedNodes.contains(edge.target_node_id)) continue;
						}
					}
					result.appendCopy(patch);
				}
				if (retained_->property_patch_entry_bound > (std::numeric_limits<size_t>::max)() - result.entry_bound)
					throw std::overflow_error("logical transaction final Property patch bound overflowed");
				result.entry_bound += retained_->property_patch_entry_bound;
			}

			// Stage/overlay continue to own complete visible collections. At commit,
			// derive the delta from the pinned base and authoritative final state;
			// never trust a caller-supplied hint about which fields changed.
			[[nodiscard]] CanonicalPropertyPatches extractFinalPropertyPatches(LogicalGraphMutationBatch &batch) const {
                const auto maximumRequests = query::execution::detail::addPropertyBufferBytes(
                    batch.property_replacements.size(), retained_->property_patches.size());
                CanonicalPropertyPatches result(write_memory_, maximumRequests);
                query::execution::MaterializedByteReservation remainingCharge(requirePayloadBudget(write_memory_));
                remainingCharge.set(detail::writeVectorCopyHeapBytes<LogicalGraphPropertyMutationRequest>(
                    batch.property_replacements.size()));
                std::vector<LogicalGraphPropertyMutationRequest> remaining;
				remaining.reserve(batch.property_replacements.size());

				for (auto &replacement : batch.property_replacements) {
					// Explicit NULL in a complete map is distinct from patch REMOVE.
					if (replacement.entity.reference_kind != LogicalGraphMutationReferenceKind::LGRK_PERSISTED ||
						std::ranges::any_of(replacement.properties, [](const auto &field) {
							return field.value.getType() == PropertyType::NULL_TYPE;
						})) {
						remaining.push_back(std::move(replacement));
						continue;
					}
                    detail::PropertyDecodeWorkspace decodedSource(write_memory_);
					const EntityReadShape shape{.all_properties = true};
					std::vector<LogicalGraphPropertyEntry> source =
						replacement.entity.entity_kind == LogicalGraphMutationEntityKind::LGMK_NODE
							? snapshot_->readNodeShaped(replacement.entity.value, shape).properties
							: snapshot_->readEdgeShaped(replacement.entity.value, shape).properties;
					std::ranges::sort(source, {}, &LogicalGraphPropertyEntry::key);
					std::pmr::vector<const LogicalGraphWriteProperty *> final(retained_->issued_node_ids.get_allocator().resource());
					final.reserve(replacement.properties.size());
					for (const auto &field : replacement.properties) final.push_back(&field);
					std::ranges::sort(final, [](const auto *left, const auto *right) { return left->key < right->key; });
					for (size_t index = 0U; index < final.size(); ++index) {
						if (final[index]->key.empty() || (index != 0U && final[index - 1U]->key == final[index]->key))
							throw std::invalid_argument("logical transaction final properties require unique nonempty keys");
					}
                    // Reserve the maximum possible normalized delta before
                    // constructing its standard containers. Each final field
                    // and each source key is copied at most once.
                    {
                        auto *budget = requirePayloadBudget(write_memory_);
                        const std::function<void()> checkpoint;
                        detail::WriteCopySizer meter(budget->availableWithParent(), checkpoint);
                        meter.properties(replacement.properties);
                        meter.add(detail::writeVectorCopyHeapBytes<std::string>(source.size()));
                        for (const auto &field : source) meter.add(detail::writeStringCopyHeapUpper(field.key));
                        meter.add(2U * query::execution::detail::standardContainerProxyBytes());
                        result.payload_charge.set(query::execution::detail::addPropertyBufferBytes(
                            result.payload_charge.bytes(), meter.bytes()));
                    }
                    LogicalGraphPropertyPatchRequest patch{replacement.entity, {}, {}};
                    patch.sets.reserve(replacement.properties.size());
                    patch.removals.reserve(source.size());
					size_t oldIndex = 0U, finalIndex = 0U;
					while (oldIndex < source.size() || finalIndex < final.size()) {
						if (finalIndex == final.size() || (oldIndex < source.size() && source[oldIndex].key < final[finalIndex]->key)) {
							patch.removals.push_back(source[oldIndex++].key);
						} else if (oldIndex == source.size() || final[finalIndex]->key < source[oldIndex].key) {
							patch.sets.push_back(*final[finalIndex++]);
						} else {
							if (source[oldIndex].value != final[finalIndex]->value) patch.sets.push_back(*final[finalIndex]);
							++oldIndex;
							++finalIndex;
						}
					}
					if (patch.sets.empty() && patch.removals.empty()) {
						// Retain the original request so a no-op commit still validates
						// its optimistic graph fence against an intervening writer.
						remaining.push_back(std::move(replacement));
						continue;
					}
					for (size_t count : {source.size(), patch.sets.size(), patch.removals.size()}) {
						if (count > (std::numeric_limits<size_t>::max)() - result.entry_bound)
							throw std::overflow_error("logical transaction Property patch entry bound overflowed");
						result.entry_bound += count;
					}
					result.requests.push_back(std::move(patch));
				}
				batch.property_replacements = std::move(remaining);
				return result;
			}

			struct PendingCreationReservation {
				size_t node_count = 0U;
				size_t edge_count = 0U;
				OwnedCreationReservation reservation;
			};

			[[nodiscard]] CanonicalMutation canonicalizeFinalState() const {
				auto *const memory = retained_->issued_node_ids.get_allocator().resource();
				const size_t nodeCount = retained_->accumulated.node_creates.size();
				const size_t edgeCount = retained_->accumulated.edge_creates.size();
				if (retained_->issued_node_ids.size() != nodeCount || retained_->issued_edge_ids.size() != edgeCount) { // each creation request obtains its IDs before canonicalization.
					throw std::logic_error("relocatable logical transaction lost its durable creation identity");
				}
				std::pmr::vector<bool> cancelledNodes(nodeCount, false, memory);
				std::pmr::vector<bool> cancelledEdges(edgeCount, false, memory);
				std::pmr::vector<const LogicalGraphNodeDeleteRequest *> createdNodeDeletes(nodeCount, nullptr, memory);
				std::pmr::set<uint64_t> uniqueNodeDeletes(memory);
				std::pmr::set<uint64_t> persistedNodeDeletes(memory);
				std::pmr::map<uint64_t, const LogicalGraphNodeDeleteRequest *> persistedNodeDeleteRequests(memory);
				for (const LogicalGraphNodeDeleteRequest &request: retained_->accumulated.node_deletes) {
					if (request.node_id == 0U || !uniqueNodeDeletes.insert(request.node_id).second ||
						(request.detach && (request.maximum_detach_edges == 0U ||
											request.maximum_detach_edges == std::numeric_limits<size_t>::max()))) {
						throw std::invalid_argument("relocatable logical transaction requires unique Node deletes and "
																"positive finite detach bounds");
					}
					const std::optional<size_t> ordinal = findIssuedOrdinal(retained_->issued_node_ids, request.node_id);
					if (!ordinal.has_value()) {
						persistedNodeDeletes.insert(request.node_id);
						persistedNodeDeleteRequests.emplace(request.node_id, &request);
						continue;
					}
					cancelledNodes.at(*ordinal) = true;
					createdNodeDeletes.at(*ordinal) = &request;
				}
				std::pmr::set<uint64_t> uniqueEdgeDeletes(memory);
				std::pmr::set<uint64_t> persistedEdgeDeletes(memory);
				for (const LogicalGraphEdgeDeleteRequest &request: retained_->accumulated.edge_deletes) {
					if (request.edge_id == 0U || !uniqueEdgeDeletes.insert(request.edge_id).second) {
						throw std::invalid_argument(
								"relocatable logical transaction requires unique nonzero Edge deletes");
					}
					if (const std::optional<size_t> ordinal = findIssuedOrdinal(retained_->issued_edge_ids, request.edge_id);
						ordinal.has_value()) {
						cancelledEdges.at(*ordinal) = true;
					} else {
						persistedEdgeDeletes.insert(request.edge_id);
					}
				}

				const auto createdNodeOrdinal = [nodeCount](const LogicalGraphMutationEntityReference &reference,
															const char *operation) -> std::optional<size_t> {
					if (reference.reference_kind != LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE) {
						return std::nullopt;
					}
						if (reference.entity_kind != LogicalGraphMutationEntityKind::LGMK_NODE ||
						reference.value >= nodeCount) {
						throw std::invalid_argument(std::string("relocatable logical transaction ") + operation +
													" has an invalid created Node reference");
					}
					return static_cast<size_t>(reference.value);
				};
				const auto persistedNodeDeletion =
						[&persistedNodeDeleteRequests](const LogicalGraphMutationEntityReference &reference)
						-> const LogicalGraphNodeDeleteRequest * {
					if (reference.entity_kind != LogicalGraphMutationEntityKind::LGMK_NODE ||
						reference.reference_kind != LogicalGraphMutationReferenceKind::LGRK_PERSISTED) {
						return nullptr;
					}
					const auto iterator = persistedNodeDeleteRequests.find(reference.value);
					return iterator == persistedNodeDeleteRequests.end() ? nullptr : iterator->second;
				};
				for (size_t nodeOrdinal = 0U; nodeOrdinal < createdNodeDeletes.size(); ++nodeOrdinal) {
					const LogicalGraphNodeDeleteRequest *const deletion = createdNodeDeletes[nodeOrdinal];
					if (deletion == nullptr) {
						continue;
					}
					size_t liveIncidentCreatedEdges = 0U;
					for (size_t edgeOrdinal = 0U; edgeOrdinal < retained_->accumulated.edge_creates.size(); ++edgeOrdinal) {
						const LogicalGraphBatchEdgeCreateRequest &edge = retained_->accumulated.edge_creates[edgeOrdinal];
						const bool incident = (createdNodeOrdinal(edge.source, "Edge creation") == nodeOrdinal) ||
											  (createdNodeOrdinal(edge.target, "Edge creation") == nodeOrdinal);
						if (!incident || cancelledEdges[edgeOrdinal]) {
							continue;
						}
						if (!deletion->detach) {
							throw std::invalid_argument(
									"relocatable logical transaction cannot delete a newly created Node "
									"with a live Edge without detach semantics");
						}
						if (++liveIncidentCreatedEdges > deletion->maximum_detach_edges) {
							throw std::invalid_argument(
									"relocatable logical transaction detach delete exceeds its created Edge bound");
						}
					}
				}
				std::pmr::map<uint64_t, size_t> persistedDetachCreatedEdgeCounts(memory);
				for (size_t edgeOrdinal = 0U; edgeOrdinal < retained_->accumulated.edge_creates.size(); ++edgeOrdinal) {
					const LogicalGraphBatchEdgeCreateRequest &edge = retained_->accumulated.edge_creates[edgeOrdinal];
					const LogicalGraphNodeDeleteRequest *const persistedSourceDeletion =
							persistedNodeDeletion(edge.source);
					const LogicalGraphNodeDeleteRequest *const persistedTargetDeletion =
							persistedNodeDeletion(edge.target);
					if (((persistedSourceDeletion != nullptr && !persistedSourceDeletion->detach) ||
						 (persistedTargetDeletion != nullptr && !persistedTargetDeletion->detach)) &&
						!cancelledEdges[edgeOrdinal]) {
						throw std::invalid_argument(
								"relocatable logical transaction cannot delete a persisted Node with a live newly "
								"created Edge without detach semantics");
					}
					const auto countPersistedDetach = [&persistedDetachCreatedEdgeCounts, edgeOrdinal,
													   &cancelledEdges](const LogicalGraphNodeDeleteRequest *deletion) {
						if (deletion == nullptr || !deletion->detach || cancelledEdges[edgeOrdinal]) {
							return;
						}
						size_t &count = persistedDetachCreatedEdgeCounts[deletion->node_id];
						if (++count > deletion->maximum_detach_edges) {
							throw std::invalid_argument(
									"relocatable logical transaction detach delete exceeds its created Edge bound");
						}
					};
					countPersistedDetach(persistedSourceDeletion);
					if (persistedTargetDeletion != persistedSourceDeletion) {
						countPersistedDetach(persistedTargetDeletion);
					}
					const std::optional<size_t> source = createdNodeOrdinal(edge.source, "Edge creation");
					const std::optional<size_t> target = createdNodeOrdinal(edge.target, "Edge creation");
					if ((source.has_value() && cancelledNodes[*source]) ||
						(target.has_value() && cancelledNodes[*target]) ||
						(persistedSourceDeletion != nullptr && persistedSourceDeletion->detach) ||
						(persistedTargetDeletion != nullptr && persistedTargetDeletion->detach)) {
						cancelledEdges[edgeOrdinal] = true;
					}
				}
				std::pmr::map<uint64_t, size_t> persistedDetachRemainingBudgets(memory);
				for (const auto &[nodeId, deletion]: persistedNodeDeleteRequests) {
					if (!deletion->detach) {
						continue;
					}
					const size_t createdEdgeCount = persistedDetachCreatedEdgeCounts[nodeId];
					if (createdEdgeCount > deletion->maximum_detach_edges) {
						throw std::logic_error(
								"relocatable logical transaction lost its detach-created-Edge budget invariant");
					}
					const size_t remaining = deletion->maximum_detach_edges - createdEdgeCount;
					// Explicitly deleted persisted Edges are already absent from the
					// transaction's final graph. Read enough snapshot candidates to
					// skip them while still detecting one additional live incident Edge.
					const size_t deletedAllowance = persistedEdgeDeletes.size();
					if (deletedAllowance >= std::numeric_limits<size_t>::max() - remaining) {
						throw std::invalid_argument(
								"relocatable logical transaction detach delete cannot derive a bounded Edge scan");
					}
					const size_t scanBound = remaining + deletedAllowance + 1U;
					std::pmr::set<uint64_t> remainingPersistedEdges(memory);
					const auto collect = [&remainingPersistedEdges, &persistedEdgeDeletes,
										  remaining](const LogicalGraphEdgeTopology &edge) {
						if (persistedEdgeDeletes.contains(edge.edge_id)) {
							return true;
						}
						remainingPersistedEdges.insert(edge.edge_id);
						if (remainingPersistedEdges.size() > remaining) {
							throw std::invalid_argument("relocatable logical transaction detach delete exceeds its "
														"total incident Edge bound");
						}
						return true;
					};
					const auto collectDirection = [&](LogicalGraphReadDirection direction) {
						const LogicalGraphAdjacencyTopologyPage page =
								snapshot_->scanAdjacentEdgeTopologies(nodeId, direction, {}, scanBound);
						for (const LogicalGraphEdgeTopology &edge: page.edges) {
							static_cast<void>(collect(edge));
						}
					};
					collectDirection(LogicalGraphReadDirection::LGRD_OUTGOING);
					collectDirection(LogicalGraphReadDirection::LGRD_INCOMING);
					persistedDetachRemainingBudgets.emplace(nodeId, remaining);
				}

				CanonicalMutation canonical(requirePayloadBudget(write_memory_), retained_->accumulated,
                    retained_->creation_reservations.node_ranges.size(), retained_->creation_reservations.edge_ranges.size());
				std::pmr::vector<size_t> nodeOrdinals(nodeCount, std::numeric_limits<size_t>::max(), memory);
				std::pmr::vector<size_t> edgeOrdinals(edgeCount, std::numeric_limits<size_t>::max(), memory);
				for (size_t ordinal = 0U; ordinal < nodeCount; ++ordinal) {
					if (cancelledNodes[ordinal]) {
						continue;
					}
					nodeOrdinals[ordinal] = canonical.batch.node_creates.size();
					canonical.batch.node_creates.push_back(retained_->accumulated.node_creates[ordinal]);
					canonical.durable_result.created_node_ids.push_back(retained_->issued_node_ids.at(ordinal));
				}
				for (size_t ordinal = 0U; ordinal < edgeCount; ++ordinal) {
					if (cancelledEdges[ordinal]) {
						continue;
					}
					edgeOrdinals[ordinal] = canonical.batch.edge_creates.size();
					canonical.batch.edge_creates.push_back(retained_->accumulated.edge_creates[ordinal]);
					canonical.durable_result.created_edge_ids.push_back(retained_->issued_edge_ids.at(ordinal));
				}
				const auto remapReference = [&nodeOrdinals,
											 &edgeOrdinals](LogicalGraphMutationEntityReference &reference,
															const char *operation) {
					switch (reference.reference_kind) {
						case LogicalGraphMutationReferenceKind::LGRK_PERSISTED:
							return;
						case LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE:
							if (reference.entity_kind != LogicalGraphMutationEntityKind::LGMK_NODE ||
								reference.value >= nodeOrdinals.size() ||
								nodeOrdinals[static_cast<size_t>(reference.value)] ==
										std::numeric_limits<size_t>::max()) {
								throw std::invalid_argument(std::string("relocatable logical transaction ") +
															operation +
															" references a cancelled or invalid created Node");
						}
						reference.value = nodeOrdinals[static_cast<size_t>(reference.value)];
						return;
					case LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE: // foldCreatedEntityFinalState consumes valid created-Edge replacements and rejects malformed ones before canonical remapping.
						if (reference.entity_kind != LogicalGraphMutationEntityKind::LGMK_EDGE ||
								reference.value >= edgeOrdinals.size() ||
								edgeOrdinals[static_cast<size_t>(reference.value)] ==
										std::numeric_limits<size_t>::max()) {
								throw std::invalid_argument(std::string("relocatable logical transaction ") +
															operation +
															" references a cancelled or invalid created Edge");
							}
							reference.value = edgeOrdinals[static_cast<size_t>(reference.value)];
							return;
					}
					throw std::invalid_argument(std::string("relocatable logical transaction ") + operation +
												" has an unknown entity reference kind");
				};
				for (LogicalGraphBatchEdgeCreateRequest &request: canonical.batch.edge_creates) {
					remapReference(request.source, "Edge creation");
					remapReference(request.target, "Edge creation");
				}
				// A logical transaction accumulates statement final states, whereas the
				// native executor receives one final publication. Keep only the final
				// replacement for each persisted entity so successive SET/REMOVE-style
				// statements do not become duplicate targets in that one publication.
				std::pmr::map<std::tuple<uint8_t, uint64_t>, const LogicalGraphPropertyMutationRequest *>
						persistedPropertyReplacements(memory);
				for (const LogicalGraphPropertyMutationRequest &source: retained_->accumulated.property_replacements) {
					const auto &request = source;
					if (request.entity.reference_kind == LogicalGraphMutationReferenceKind::LGRK_PERSISTED) {
						if ((request.entity.entity_kind == LogicalGraphMutationEntityKind::LGMK_NODE &&
							 persistedNodeDeletes.contains(request.entity.value)) ||
							(request.entity.entity_kind == LogicalGraphMutationEntityKind::LGMK_EDGE &&
							 persistedEdgeDeletes.contains(request.entity.value))) {
							continue;
						}
						if (request.entity.entity_kind == LogicalGraphMutationEntityKind::LGMK_EDGE &&
							!persistedNodeDeleteRequests.empty()) {
							const auto edge = snapshot_->readEdgeShaped(request.entity.value, {.include_type = false});
							const auto detached = [&](uint64_t nodeId) {
								const auto found = persistedNodeDeleteRequests.find(nodeId);
								return found != persistedNodeDeleteRequests.end() && found->second->detach;
							};
							if (detached(edge.source_node_id) || detached(edge.target_node_id)) continue;
						}
						persistedPropertyReplacements.insert_or_assign(
								{static_cast<uint8_t>(request.entity.entity_kind), request.entity.value}, &request);
						continue;
					}
					if ((request.entity.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE && // every created-entity replacement is folded into its CREATE request before this final canonicalization pass.
						 request.entity.value < nodeOrdinals.size() &&
						 nodeOrdinals[static_cast<size_t>(request.entity.value)] ==
								 std::numeric_limits<size_t>::max()) ||
						(request.entity.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE &&
						 request.entity.value < edgeOrdinals.size() &&
						 edgeOrdinals[static_cast<size_t>(request.entity.value)] ==
								 std::numeric_limits<size_t>::max())) {
						continue;
					}
                    canonical.batch.property_replacements.push_back(request);
                    remapReference(canonical.batch.property_replacements.back().entity, "Property replacement");
				}
				for (auto &[key, request]: persistedPropertyReplacements) {
					static_cast<void>(key);
					canonical.batch.property_replacements.push_back(*request);
				}
				std::pmr::map<uint64_t, const LogicalGraphNodeLabelMutationRequest *> persistedLabelReplacements(memory);
				for (const LogicalGraphNodeLabelMutationRequest &source: retained_->accumulated.node_label_replacements) {
					const auto &request = source;
					if (request.node.reference_kind == LogicalGraphMutationReferenceKind::LGRK_PERSISTED) {
						if (persistedNodeDeletes.contains(request.node.value)) {
							continue;
						}
						persistedLabelReplacements.insert_or_assign(request.node.value, &request);
						continue;
					}
					if (request.node.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE && // created-Node labels are folded into CREATE before final canonicalization.
						request.node.value < nodeOrdinals.size() &&
						nodeOrdinals[static_cast<size_t>(request.node.value)] == std::numeric_limits<size_t>::max()) {
						continue;
					}
                    canonical.batch.node_label_replacements.push_back(request);
                    remapReference(canonical.batch.node_label_replacements.back().node, "Node label replacement");
				}
				for (auto &[nodeId, request]: persistedLabelReplacements) {
					static_cast<void>(nodeId);
					canonical.batch.node_label_replacements.push_back(*request);
				}
				for (const LogicalGraphEdgeDeleteRequest &request: retained_->accumulated.edge_deletes) {
					if (!findIssuedOrdinal(retained_->issued_edge_ids, request.edge_id).has_value()) {
						canonical.batch.edge_deletes.push_back(request);
					}
				}
				for (const LogicalGraphNodeDeleteRequest &request: retained_->accumulated.node_deletes) {
					if (!findIssuedOrdinal(retained_->issued_node_ids, request.node_id).has_value()) {
						LogicalGraphNodeDeleteRequest canonicalRequest = request;
						if (request.detach) {
							const size_t remaining = persistedDetachRemainingBudgets.at(request.node_id);
							if (remaining == 0U) {
								// No durable incident Edge remains after final-state folding.
								// Use the isolated-delete form so the lower bounded scanner is
								// not asked to express a zero-record detach scan.
								canonicalRequest.detach = false;
							} else {
								canonicalRequest.maximum_detach_edges = remaining;
							}
						}
						canonical.batch.node_deletes.push_back(std::move(canonicalRequest));
					}
				}
				if (nodeCount != 0U || edgeCount != 0U) {
					LogicalGraphCreationIdentityPlan plan;
					if (nodeCount != 0U) {
						plan.node_id_ranges.reserve(retained_->creation_reservations.node_ranges.size());
                        for (const auto &range : retained_->creation_reservations.node_ranges)
                            plan.node_id_ranges.push_back({range.first_id, range.count});
						plan.node_ids = canonical.durable_result.created_node_ids;
					}
					if (edgeCount != 0U) {
						plan.edge_id_ranges.reserve(retained_->creation_reservations.edge_ranges.size());
                        for (const auto &range : retained_->creation_reservations.edge_ranges)
                            plan.edge_id_ranges.push_back({range.first_id, range.count});
						plan.edge_ids = canonical.durable_result.created_edge_ids;
					}
					canonical.batch.creation_identity_plan = std::move(plan);
				}
				return canonical;
			}

			static bool isEmpty(const OwnedMutationBatch &batch) {
				return batch.node_creates.empty() && batch.edge_creates.empty() &&
					   batch.property_replacements.empty() && batch.node_label_replacements.empty() &&
					   batch.edge_deletes.empty() && batch.node_deletes.empty();
			}

            [[nodiscard]] OwnedCreationReservation reserveOwnedCreationIds(uint64_t nodes, uint64_t edges) const {
                auto native = backend_.reserveLogicalGraphCreationIds(nodes, edges);
                try {
                    OwnedCreationReservation owned(retained_->issued_node_ids.get_allocator().owner());
                    owned.node_ranges.assign(native.node_ranges.begin(), native.node_ranges.end());
                    owned.edge_ranges.assign(native.edge_ranges.begin(), native.edge_ranges.end());
                    return owned;
                } catch (...) {
                    backend_.abandonLogicalGraphCreationIds(native);
                    throw;
                }
            }

            void abandonOutstandingCreationReservations() noexcept {
                if (!retained_) return;
                // Keep native lease ranges in their native representation while
                // staging. Rollback must not allocate conversion vectors.
                backend_.abandonLogicalGraphCreationIds(retained_->creation_reservations.node_ranges, retained_->creation_reservations.edge_ranges);
				if (retained_->preview_reservation.has_value()) {
					backend_.abandonLogicalGraphCreationIds(retained_->preview_reservation->reservation.node_ranges, retained_->preview_reservation->reservation.edge_ranges);
					retained_->preview_reservation.reset();
				}
			}

            void releaseRetainedMutationState() noexcept {
                // Destruction releases every buffer without constructing empty
                // containers (which may allocate standard-library proxies).
                retained_.reset();
            }

			void requireActive(const char *operation) const {
				if (state_ != LogicalGraphTransactionState::LGTTS_ACTIVE || snapshot_ == nullptr || !retained_) {
					throw std::logic_error(std::string("relocatable logical transaction cannot ") + operation +
										   " after it has finished");
				}
			}


            static query::execution::MaterializedByteBudget *requirePayloadBudget(
                    const std::shared_ptr<std::pmr::memory_resource> &memory) {
                auto *budget = dynamic_cast<query::execution::MaterializedByteBudget *>(memory.get());
                if (!budget) throw std::logic_error("native transaction requires an owning WRITE budget");
                return budget;
            }
            struct RetainedMutationState {
                // Last to be destroyed: nested standard buffers precede their refund.
                query::execution::MaterializedByteReservation nested_payload_charge;
                OwnedMutationBatch accumulated;
                utils::OwnedVector<LogicalGraphPropertyPatchRequest> property_patches;
                size_t property_patch_entry_bound = 0U;
                mutable std::optional<PendingCreationReservation> preview_reservation;
                utils::OwnedVector<uint64_t> issued_node_ids;
                utils::OwnedVector<uint64_t> issued_edge_ids;
                OwnedCreationReservation creation_reservations;
                NativeGraphLogicalIdReservation streamed_edge_reservation;
                explicit RetainedMutationState(const std::shared_ptr<std::pmr::memory_resource> &memory)
                    : nested_payload_charge(requirePayloadBudget(memory)), accumulated(memory), property_patches(utils::OwnedPmrAllocator<LogicalGraphPropertyPatchRequest>(memory)),
                      issued_node_ids(utils::OwnedPmrAllocator<uint64_t>(memory)),
                      issued_edge_ids(utils::OwnedPmrAllocator<uint64_t>(memory)), creation_reservations(memory) {}
            };

			RelocatableStorageBackend &backend_;
			bool read_only_ = false;
			LogicalGraphTransactionState state_ = LogicalGraphTransactionState::LGTTS_ACTIVE;
			std::unique_ptr<LogicalGraphVectorSearchSnapshot> snapshot_;
			LogicalGraphMutationFence fence_;
			Generation metadata_generation_{};
			RelocatableReadSnapshot raw_snapshot_;
			bool reads_exposed_ = false;
            std::shared_ptr<std::pmr::memory_resource> write_memory_;
            std::optional<RetainedMutationState> retained_;
			std::unique_ptr<NativeBulkRunStore> bulk_edge_store_;
			std::unique_ptr<NativeBulkEdgeRunStreamingWriter> bulk_edge_writer_;
			std::unique_ptr<NativeBulkSessionLease> bulk_session_lease_;
			std::optional<NativeBulkEdgeRunManifest> bulk_edge_manifest_;

			uint64_t streamed_edge_next_id_ = 0U;
			uint64_t streamed_edge_lease_last_id_ = 0U;
			size_t streamed_edge_record_count_ = 0U;
		};
