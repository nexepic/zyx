		// Page-local native Node scans do not promise logical-ID order. Batch selected
		// Property columns therefore sort only the bounded lookup request, then restore
		// the scan order before exposing rows to the logical runtime.
		[[nodiscard]] std::optional<bool>
		tryVisitBatchedNativeNodeRefScan(const logical::LogicalNodeScan &scan,
										 const std::vector<std::pair<std::string, PropertyValue>> &parameterPredicates,
										 ExecutionState &state, const InternalVisitor &visitor,
										 const NodeColumnReceiver *columnReceiver = nullptr) {
			if (!state.shared.allow_deferred_entity_refs || !state.shared.snapshot.supportsNativeNodeRefScans()) {
				return std::nullopt;
			}

			std::vector<std::string> propertyKeys;
			propertyKeys.reserve(scan.getPropertyPredicates().size() + parameterPredicates.size() +
								 scan.getRangePredicates().size());
			const auto appendKey = [&propertyKeys](std::string_view key) {
				if (key.empty()) {
					throw std::invalid_argument("logical-plan Node Property predicate has an empty key");
				}
				if (std::ranges::find(propertyKeys, key) == propertyKeys.end()) {
					propertyKeys.emplace_back(key);
				}
			};
			const auto collectEqualityKeys = [&](const auto &predicates) {
				for (const auto &[key, expected]: predicates) {
					if (isNull(expected))
						return false;
					appendKey(key);
				}
				return true;
			};
			if (!collectEqualityKeys(scan.getPropertyPredicates()) || !collectEqualityKeys(parameterPredicates)) {
				return std::nullopt;
			}
			for (const logical::RangePredicate &range: scan.getRangePredicates()) {
				appendKey(range.key);
			}
			if (const auto &composite = scan.getCompositeEquality(); composite.has_value()) {
				if (composite->keys.empty() || composite->keys.size() != composite->values.size()) {
					throw std::invalid_argument("logical-plan Node composite predicate has an invalid shape");
				}
				for (size_t index = 0U; index < composite->keys.size(); ++index) {
					if (isNull(composite->values[index])) {
						return std::nullopt;
					}
					appendKey(composite->keys[index]);
				}
			}
			if (columnReceiver) {
				for (const auto key : columnReceiver->keys) appendKey(key);
			}
			if (propertyKeys.empty()) {
				return std::nullopt;
			}
			MaterializedByteReservation propertyKeyBytes(&state.shared.materialized_budget);
			if (columnReceiver) {
				size_t bytes = checkedSizeMul(propertyKeys.capacity(), sizeof(std::string),
					"logical-plan Node column key capacity overflowed");
				for (const auto &key : propertyKeys) bytes = checkedSizeAdd(bytes,
					detail::dynamicStringBufferBytes(key), "logical-plan Node column key buffers overflowed");
				if (propertyKeyBytes.wouldExceed(bytes)) return std::nullopt;
				propertyKeyBytes.set(bytes);
			}

			struct PendingNodeRef {
				storage::EntityRef reference;
			};
			// Keep reference buffering below a quarter of the available allowance so
			// IDs, ordinal mappings, selected values and a row can coexist at flush.
			const size_t byteBatchCapacity =
					std::max<size_t>(1U, state.shared.materialized_budget.available() / sizeof(PendingNodeRef) / 4U);
			const size_t batchCapacity = std::min({DEFAULT_PROPERTY_BATCH_ROWS, byteBatchCapacity,
												   state.shared.bounds.maximum_node_records, state.shared.bounds.maximum_rows});
			BoundedBatch<PendingNodeRef> pending(
					batchCapacity, state.shared.bounds.maximum_materialized_bytes,
					[](const auto &) { return sizeof(PendingNodeRef); }, &state.shared.materialized_budget,
                            state.shared.context && state.shared.context->resource_account, sizeof(PendingNodeRef));
			pending.reserve(batchCapacity);
			bool continueVisitor = true;
            const storage::LogicalGraphNodeScanProvider::NodeScanPropertySource *certifiedSource = nullptr;
            // The provider owns these IDs until this synchronous callback returns.
            // Keep the batch intact instead of constructing and charging one
            // temporary EntityRef per candidate before rebuilding lookup IDs.
            std::span<const uint64_t> certifiedIds;
            const auto referenceAt = [&](size_t ordinal) -> storage::EntityRef {
                if (certifiedSource) return {certifiedIds[ordinal], false, 0U, 0U, 0U};
                return pending.values()[ordinal].reference;
            };
            const auto pendingCount = [&] { return certifiedSource ? certifiedIds.size() : pending.size(); };
			const auto flushPending = [&]() {
				if (!pendingCount()) {
					return continueVisitor;
				}
				std::vector<std::string_view> keyViews;
				keyViews.reserve(propertyKeys.size());
				for (const auto &key: propertyKeys)
					keyViews.push_back(key);
				struct NodeColumnBytePressure {};
				std::function<bool(size_t, size_t)> processRange;
				processRange = [&](size_t begin, size_t end) -> bool {
					try {
						MaterializedByteReservation lookupBytes(&state.shared.materialized_budget);
						const size_t scratch = checkedSizeMul(end - begin, sizeof(uint64_t) + 2U * sizeof(size_t),
															  "logical-plan Node column lookup size overflowed");
						if (lookupBytes.wouldExceed(scratch))
							throw NodeColumnBytePressure{};
						lookupBytes.set(scratch);
                        std::vector<size_t> lookupOrdinals(end - begin);
                        std::vector<uint64_t> ownedNodeIds;
                        std::span<const uint64_t> nodeIds;
                        const auto borrowed = certifiedSource ? certifiedIds.subspan(begin, end - begin) :
                            std::span<const uint64_t>{};
                        if (certifiedSource && std::ranges::is_sorted(borrowed)) {
                            // The native complete scan is monotonic. Pass its
                            // authenticated ID span directly to the field source;
                            // preserve an identity ordinal map for every consumer.
                            uint64_t previous = 0;
                            for (const auto id : borrowed) {
                                if (!id || id <= previous)
                                    throw std::runtime_error("logical graph native Node reference scan returned duplicate Node IDs");
                                previous = id;
                            }
                            nodeIds = borrowed;
                            std::iota(lookupOrdinals.begin(), lookupOrdinals.end(), size_t{0});
                            debug::PerfTrace::addValue("logical_plan.certified_scan.direct_id_rows", static_cast<int64_t>(nodeIds.size()));
                        } else {
                            std::vector<size_t> sortedOrdinals(end - begin);
                            std::iota(sortedOrdinals.begin(), sortedOrdinals.end(), begin);
                            const auto ownerLess = [&](size_t left, size_t right) {
                                return referenceAt(left).entity_id < referenceAt(right).entity_id;
                            };
                            if (!std::ranges::is_sorted(sortedOrdinals, ownerLess))
                                std::ranges::sort(sortedOrdinals, ownerLess);
                            ownedNodeIds.reserve(end - begin);
                            for (size_t sorted = 0; sorted < sortedOrdinals.size(); ++sorted) {
                                const size_t original = sortedOrdinals[sorted];
                                const uint64_t id = referenceAt(original).entity_id;
                                if (!ownedNodeIds.empty() && ownedNodeIds.back() == id)
                                    throw std::runtime_error("logical graph native Node reference scan returned duplicate Node IDs");
                                ownedNodeIds.push_back(id);
                                lookupOrdinals[original - begin] = sorted;
                            }
                            nodeIds = ownedNodeIds;
                        }

#if ZYX_BUILD_STREAMING_COUNT_PATH
                        if (streamingCountEnabled() && certifiedSource && columnReceiver && columnReceiver->consumeCount && keyViews.size() == 1U) {
                            std::optional<size_t> accepted;
                            bool guardFailed = false;
                            const auto guard = [&] {
                                try { checkGuardNow(state); } catch (...) { guardFailed = true; throw; }
                            };
                            try {
                                detail::BoundNodeColumnPredicates predicates(scan, parameterPredicates, propertyKeys,
                                    state.shared.materialized_budget);
                                predicates.appendSingleColumnPredicates(columnReceiver->countPredicates);
                                if (!predicates.predicates().empty()) {
                                    detail::StreamingPredicateCount count(state.shared.snapshot, nodeIds.size(),
                                        predicates.predicates(), state.shared.materialized_budget);
                                    MaterializedByteReservation scratch(&state.shared.materialized_budget);
                                    const auto owning = [&](size_t row, size_t column, std::optional<PropertyValue> value) {
                                        count.value(row, column, value, guard);
                                    };
                                    storage::PropertyColumnBatchReadSinks runs;
                                    runs.scalars = [&](std::span<const size_t> rows, size_t column, PropertyType type,
                                        std::span<const uint64_t> words) { count.scalars(rows, column, type, words, guard); };
                                    runs.texts = [&](std::span<const size_t> rows, size_t column,
                                        std::span<const std::string_view> values) { count.texts(rows, column, values, guard); };
                                    (*certifiedSource)(nodeIds, owning, runs, [&](size_t bytes) {
                                        guard(); scratch.set(checkedSizeAdd(scratch.bytes(), bytes, "streaming count source overflowed"));
                                    });
                                    accepted = count.finish(guard);
                                }
                            } catch (const MaterializedByteLimitExceeded &) {
                                if (guardFailed) throw;
                                throw NodeColumnBytePressure{};
                            } catch (const storage::LogicalGraphBufferPressure &) {
                                if (guardFailed) throw;
                                if (nodeIds.size() > 1U) throw NodeColumnBytePressure{};
                                // An indivisible source retains the established owning fallback.
                            }
                            if (accepted) {
                                debug::PerfTrace::addValue("logical_plan.streaming_count.source_rows", static_cast<int64_t>(nodeIds.size()));
                                debug::PerfTrace::addValue("logical_plan.streaming_count.state_bytes", static_cast<int64_t>(nodeIds.size()));
                                // Consumer exceptions escape without replay of an already delivered reduction.
                                return columnReceiver->consumeCount(*accepted);
                            }
                        }
#endif
						if (columnReceiver && columnReceiver->consumeDirect &&
							state.shared.snapshot.supportsNodePropertyCellBatches()) {
							std::optional<detail::DirectPropertyColumns> direct;
							std::optional<detail::BoundColumnLayout> directLayout;
							std::optional<detail::BatchEnvelope> directEnvelope;
							detail::BudgetMemoryResource selectionMemory(state.shared.materialized_budget);
							std::optional<SelectionVector> selected;
							bool guardFailed = false;
							const auto directGuard = [&] {
								try { checkGuardNow(state); } catch (...) { guardFailed = true; throw; }
							};
							try {
								direct.emplace(state.shared.snapshot, nodeIds.size(), keyViews.size(), state.shared.materialized_budget);
								selected.emplace(nodeIds.size(), &selectionMemory);
								detail::BoundNodeColumnPredicates predicates(scan, parameterPredicates, propertyKeys, state.shared.materialized_budget);
								MaterializedByteReservation sourceScratch(&state.shared.materialized_budget);
								const auto emitValue = [&](size_t row, size_t column, std::optional<PropertyValue> value) {
									direct->append(row, column, std::move(value), directGuard);
								};
								const auto emitScalar = [&](size_t row, size_t column, PropertyType type, uint64_t bits) {
									direct->appendScalar(row, column, type, bits, directGuard);
								};
                                const auto emitText = [&](size_t row, size_t column, std::string_view text) {
                                    direct->appendText(row, column, text, directGuard);
                                };
								const auto admitSource = [&](size_t bytes) {
									directGuard();
									sourceScratch.set(checkedSizeAdd(sourceScratch.bytes(), bytes, "direct source bytes overflowed"));
								};
                                bool deliveredRuns = false;
                                bool generalSourceFallback = false;
                                if (certifiedSource || (state.shared.snapshot.hasImmutableReadIdentity() && nodeIds.size() > 1U && keyViews.size() == 1U)) {
                                    bool emitted = false;
                                    const auto owning = [&](size_t row, size_t column, std::optional<PropertyValue> value) {
                                        emitted = true; emitValue(row, column, std::move(value));
                                    };
                                    storage::PropertyColumnBatchReadSinks runs;
                                    runs.scalars = [&](std::span<const size_t> rows, size_t column, PropertyType type,
                                            std::span<const uint64_t> bits) {
                                        emitted = true; direct->appendScalarRun(rows, column, type, bits, directGuard);
                                    };
                                    runs.texts = [&](std::span<const size_t> rows, size_t column,
                                            std::span<const std::string_view> texts) {
                                        emitted = true; direct->appendTextRun(rows, column, texts, directGuard);
                                    };
                                    if (certifiedSource) {
                                        try {
                                            (*certifiedSource)(nodeIds, std::ref(owning), runs, std::ref(admitSource));
                                        } catch (const storage::LogicalGraphBufferPressure &) {
                                            if (guardFailed) throw;
                                            // Split private batches first. An indivisible large value
                                            // retains the existing general owning path and query budget;
                                            // it is not admitted as a bounded certified-source batch.
                                            if (nodeIds.size() > 1U) throw NodeColumnBytePressure{};
                                            direct.reset();
                                            sourceScratch.reset();
                                            direct.emplace(state.shared.snapshot, nodeIds.size(), keyViews.size(),
                                                state.shared.materialized_budget);
                                            state.shared.snapshot.readNodePropertyCellsWithViewsSortedWithAdmission(
                                                nodeIds, keyViews, std::ref(emitValue), std::ref(emitScalar),
                                                std::ref(emitText), std::ref(admitSource));
                                            generalSourceFallback = true;
                                            debug::PerfTrace::addValue("logical_plan.certified_scan.general_value_fallback", 1);
                                        }
                                        deliveredRuns = true;
                                    } else deliveredRuns = state.shared.snapshot.tryReadNodePropertyColumnBatchesSortedWithAdmission(
                                        nodeIds, keyViews, std::ref(owning), runs, std::ref(admitSource));
                                    if (!deliveredRuns && emitted)
                                        throw std::invalid_argument("unsupported Property batch emitted partial cells");
                                    if (deliveredRuns && !generalSourceFallback)
                                        debug::PerfTrace::addValue("logical_plan.direct_property.run_batches", 1);
                                }
                                if (!deliveredRuns)
                                    state.shared.snapshot.readNodePropertyCellsWithViewsSortedWithAdmission(nodeIds, keyViews,
                                        std::ref(emitValue), std::ref(emitScalar), std::ref(emitText), std::ref(admitSource));
								direct->seal(); sourceScratch.reset();
								(void)predicates.filter(*direct, *selected, directGuard);
								if (state.shared.bound_program) {
									try {
										directLayout.emplace(propertyKeys, state.shared.materialized_budget,
											std::pmr::get_default_resource(), directGuard);
										detail::BudgetMemoryResource metadataMemory(state.shared.materialized_budget);
										std::pmr::vector<detail::BatchColumnSource> sources(&metadataMemory);
										std::pmr::vector<uint32_t> selectedRows(&metadataMemory);
										std::pmr::vector<uint64_t> originalOrdinals(nodeIds.size(), 0U, &metadataMemory);
										for (size_t c = 0; c < propertyKeys.size(); ++c) sources.push_back(detail::BatchColumnSource::direct(*direct, c));
										for (size_t original = 0; original < lookupOrdinals.size(); ++original) {
											directGuard(); const auto sorted = lookupOrdinals[original]; originalOrdinals[sorted] = original;
											if (selected->includes(sorted)) selectedRows.push_back(static_cast<uint32_t>(sorted));
										}
										directEnvelope.emplace(state.shared.bound_program->root().program, *directLayout,
											state.shared.snapshot, nodeIds.size(), state.shared.materialized_budget);
										directEnvelope->publish(propertyKeys, sources, nodeIds.size(), selectedRows, originalOrdinals, directGuard);
										debug::PerfTrace::addValue("logical_plan.batch_envelope.published", 1);
									} catch (const MaterializedByteLimitExceeded &) {
										if (guardFailed) throw;
										directEnvelope.reset(); directLayout.reset();
										debug::PerfTrace::addValue("logical_plan.batch_envelope.admission_fallback", 1);
									}
								}
							} catch (const MaterializedByteLimitExceeded &) {
								if (guardFailed) throw;
								// Private construction only: there have been no consumer effects.
								throw NodeColumnBytePressure{};
							}
							debug::PerfTrace::addValue("logical_plan.direct_property.batches", 1);
							debug::PerfTrace::addValue("logical_plan.direct_property.rows", static_cast<int64_t>(direct->size()));
							debug::PerfTrace::addValue("logical_plan.direct_property.rejected", static_cast<int64_t>(direct->size() - selected->selectedCount()));
							// Consumer exceptions are outside preparation fallback and never replay.
							return columnReceiver->consumeDirect(propertyKeys, *direct, *selected, lookupOrdinals);
						}
						MaterializedByteReservation rowBytes(&state.shared.materialized_budget);
						LogicalPlanReadRow row;
						const auto prepareRow = [&](const storage::EntityRef &reference) {
							row.values.emplace(scan.getVariable(), reference);
							for (const auto &key: propertyKeys)
								row.property_cache[scan.getVariable()].emplace(key, PropertyValue());
						};
						if (!columnReceiver) prepareRow(referenceAt(begin));
						size_t structuralBytes = columnReceiver ? 0U : logicalReadRowBytes(row, state.shared.context);
						if (rowBytes.wouldExceed(structuralBytes))
							throw NodeColumnBytePressure{};
						rowBytes.set(structuralBytes);
						MaterializedByteReservation columnBytes(&state.shared.materialized_budget);
						const std::function<void(size_t)> admission = [&](size_t bytes) {
							checkGuard(state);
							const size_t next = checkedSizeAdd(columnBytes.bytes(), bytes,
															   "logical-plan Node source size overflowed");
							if (columnBytes.wouldExceed(next))
								throw NodeColumnBytePressure{};
							columnBytes.set(next);
							graph::debug::PerfTrace::addValue(
									"logical_plan.node_predicate_source_bytes",
									static_cast<int64_t>(
											std::min(bytes, static_cast<size_t>(std::numeric_limits<int64_t>::max()))));
						};
						auto columns = state.shared.snapshot.supportsPropertyReadAdmission()
											   ? state.shared.snapshot.readNodePropertyColumnsSortedWithAdmission(
														 nodeIds, keyViews, admission)
											   : state.shared.snapshot.readNodePropertyColumnsSorted(nodeIds, keyViews);
						if (columns.size() != propertyKeys.size()) {
							throw std::runtime_error("logical graph backend changed Node Property column cardinality");
						}
						for (const auto &column: columns) {
							if (column.size() != end - begin) {
								throw std::runtime_error(
										"logical graph backend changed sorted Node Property result cardinality");
							}
						}
						const size_t retained =
								propertyColumnBytes(columns, keyViews, "logical-plan Node selected size overflowed", state.shared.context);
						if (columnBytes.wouldExceed(retained))
							throw NodeColumnBytePressure{};
						columnBytes.set(retained);
						std::optional<detail::BoundNodeColumnPredicates> boundPredicates;
						try {
							boundPredicates.emplace(scan, parameterPredicates, propertyKeys, state.shared.materialized_budget);
						} catch (const MaterializedByteLimitExceeded &) {
							// Optional metadata is admitted only after the source and row
							// owners. Declining it preserves the existing scalar path.
						}
						detail::BudgetMemoryResource selectionMemory(state.shared.materialized_budget);
						std::optional<SelectionVector> selection;
						const size_t bitmapBytes = SelectionVector::requiredStorageBytes(end - begin);
						if (boundPredicates && bitmapBytes <= state.shared.materialized_budget.available()) {
							try { selection.emplace(end - begin, &selectionMemory); }
							catch (const MaterializedByteLimitExceeded &) {
								// Optional bitmap admission can decline only before filtering.
							}
						}
						std::optional<detail::BoundNodeScanBatch> scanBatch;
						if (selection && state.shared.bound_program) {
							try {
								scanBatch.emplace(*state.shared.bound_program, state.shared.snapshot, propertyKeys, columns, nodeIds,
									state.shared.materialized_budget, [&] { checkGuardNow(state); });
								debug::PerfTrace::addValue("logical_plan.bound_scan.published", 1);
							} catch (const MaterializedByteLimitExceeded &) {
								debug::PerfTrace::addValue("logical_plan.bound_scan.admission_fallback", 1);
							}
						}
						if (selection) {
							const auto filtered = scanBatch
								? boundPredicates->filter(scanBatch->envelope(), scanBatch->layout(), *selection, [&] { checkGuard(state); })
								: boundPredicates->filter(columns, *selection, state.shared.materialized_budget, [&] { checkGuard(state); });
							debug::PerfTrace::addValue("logical_plan.node_column_filter.typed_columns",
								static_cast<int64_t>(filtered.typed_columns));
							debug::PerfTrace::addValue("logical_plan.node_column_filter.scalar_columns",
								static_cast<int64_t>(filtered.scalar_columns));
							debug::PerfTrace::addValue("logical_plan.node_column_filter.rejected_before_rows",
								static_cast<int64_t>(selection->size() - selection->selectedCount()));
							if (scanBatch) {
								try { scanBatch->publishSelection(*selection, lookupOrdinals, [&] { checkGuardNow(state); }); }
								catch (const MaterializedByteLimitExceeded &) { scanBatch.reset(); }
							}
							// Payload mutation invalidates the borrowed source matrix. Release
							// every envelope before clearing rejected cells or moving rows.
							scanBatch.reset();
							// Rejected payloads cannot escape. Release them before handing a
							// survivor downstream, retaining only charged matrix slots/NULLs.
							for (size_t index = 0U; index < selection->size(); ++index) {
								if (selection->includes(index)) continue;
								for (auto &column : columns) {
									checkGuard(state);
									const size_t bytes = detail::retainedPropertyValueBytes(column[index], [&] { checkGuardNow(state); });
									column[index] = PropertyValue();
									columnBytes.set(checkedSizeAdd(columnBytes.bytes() - bytes, 1U,
										"logical-plan rejected column bytes overflowed"));
								}
							}
						}
						// Predicate instructions borrow the source only during filtering.
						// Release their capacity before downstream owning-output admission.
						boundPredicates.reset();
						if (columnReceiver && selection) {
							debug::PerfTrace::addValue("logical_plan.node_column_consumer.batches", 1);
							continueVisitor = columnReceiver->consume(propertyKeys, columns, *selection, lookupOrdinals);
							return continueVisitor;
						}
						if (columnReceiver) {
							prepareRow(referenceAt(begin));
							structuralBytes = logicalReadRowBytes(row, state.shared.context);
							if (rowBytes.wouldExceed(structuralBytes)) throw NodeColumnBytePressure{};
							rowBytes.set(structuralBytes);
						}
						// No byte-pressure retry is issued after a row is delivered: only
						// pre-read/pre-delivery pressure may split this original scan range.
						for (size_t index = begin; index < end && continueVisitor; ++index) {
							checkGuard(state);
							const size_t sorted = lookupOrdinals[index - begin];
							if (selection && !selection->includes(sorted)) continue;
							if (index != begin) {
								row = LogicalPlanReadRow{};
								prepareRow(referenceAt(index));
								rowBytes.set(structuralBytes);
							}
							for (size_t column = 0U; column < propertyKeys.size(); ++column) {
								const size_t valueBytes = detail::retainedPropertyValueBytes(columns[column][sorted], [&] { checkGuardNow(state); });
								columnBytes.set(columnBytes.bytes() - valueBytes);
								rowBytes.set(checkedSizeAdd(rowBytes.bytes() - 1U, valueBytes,
															"logical-plan Node row size overflowed"));
								row.property_cache.at(scan.getVariable()).at(propertyKeys[column]) =
										std::move(columns[column][sorted]);
							}
							if (selection.has_value() || nodeRefScanMatches(state.shared.snapshot, row, scan.getVariable(), scan, parameterPredicates,
												   state, true)) {
								rowBytes.reset();
								continueVisitor = visitor(std::move(row));
							} else {
								row = LogicalPlanReadRow{};
								rowBytes.reset();
							}
						}
						return continueVisitor;
					} catch (const NodeColumnBytePressure &) {
						if (end - begin == 1U)
							throw std::runtime_error("logical-plan Node Property batch exceeded its byte bound");
					}
					graph::debug::PerfTrace::addValue("logical_plan.node_predicate_split_batches", 1);
					const size_t middle = begin + (end - begin) / 2U;
					return processRange(begin, middle) && processRange(middle, end);
				};
				continueVisitor = processRange(0U, pendingCount());
				pending.clear();
				return continueVisitor;
			};

            const auto *scanProvider = dynamic_cast<const storage::LogicalGraphNodeScanProvider *>(&state.shared.snapshot);
            if (scanProvider && columnReceiver && columnReceiver->consumeDirect &&
                state.shared.snapshot.hasImmutableReadIdentity()) {
                MaterializedByteReservation batchSourceBytes(&state.shared.materialized_budget);
                detail::BudgetMemoryResource sourceKeyMemory(state.shared.materialized_budget);
                std::pmr::vector<std::string_view> sourceKeys(&sourceKeyMemory);
                bool emittedBatch = false;
                std::optional<bool> result;
                try {
                    sourceKeys.reserve(propertyKeys.size());
                    for (const auto &key : propertyKeys) sourceKeys.push_back(key);
                    result = scanProvider->tryVisitNodeScanBatches(
                        state.shared.bounds.maximum_node_records, batchCapacity, scan.getLabels(), sourceKeys,
                        [&](std::span<const uint64_t> ids, size_t examined,
                            const storage::LogicalGraphNodeScanProvider::NodeScanPropertySource &source) {
                            emittedBatch = true;
                            if (ids.size() > batchCapacity || examined < ids.size() || examined > batchCapacity)
                                throw std::runtime_error("certified scan batch changed its bounded cardinality");
                            checkGuardNow(state);
                            if (!pending.empty() || certifiedSource)
                                throw std::logic_error("certified scan batch overlaps an active source");
                            if (examined > (std::numeric_limits<size_t>::max)() - state.task.stats.nodes_scanned)
                                throw std::overflow_error("logical-plan Node counter overflowed");
                            state.task.stats.nodes_scanned += examined;
                            for (const auto id : ids)
                                if (!id) throw std::runtime_error("certified scan batch returned a zero Node ID");
                            debug::PerfTrace::addValue("logical_plan.certified_scan.borrowed_ids", static_cast<int64_t>(ids.size()));
                            certifiedIds = ids;
                            certifiedSource = &source;
                            const auto release = [&] { certifiedSource = nullptr; certifiedIds = {}; };
                            try { const bool more = flushPending(); release(); return more; }
                            catch (...) { release(); throw; }
                        }, [&](size_t bytes) { batchSourceBytes.set(bytes); });
                } catch (const MaterializedByteLimitExceeded &) {
                    if (emittedBatch) throw;
                    // No source was exposed: the established lower-memory path
                    // may still execute within the same query budget.
                }
                if (result) return *result;
                if (emittedBatch) throw std::runtime_error("unsupported scan batch emitted a prefix");
            }

			state.shared.snapshot.visitNodeRefsForLabelPredicate(
					state.shared.bounds.maximum_node_records, scan.getLabels(),
					[&](const storage::EntityRef &reference, const bool labelsMatch) {
						checkGuard(state);
						validateNodeRef(reference,
										"logical graph backend returned an invalid or Edge reference from a Node scan");
						incrementBounded(state.task.stats.nodes_scanned,
										 "logical-plan read runtime Node counter overflowed");
						if (!labelsMatch) {
							return true;
						}
						if (!pending.empty() && pending.wouldExceedBytes(PendingNodeRef{reference}) && !flushPending())
							return false;
						pending.append(PendingNodeRef{reference});
						return pending.size() != batchCapacity || flushPending();
					});
			if (continueVisitor) {
				static_cast<void>(flushPending());
			}
			return continueVisitor;
		}

		// Consume one authenticated native index batch.  Keeping this logic in a
		// batch-local helper lets a cursor stop after the downstream LIMIT without
		// collecting the complete index result in memory.
		[[nodiscard]] bool visitIndexedCandidateBatch(
				const storage::IndexCandidateBatch &nativeCandidates, const logical::LogicalNodeScan &scan,
				const std::vector<std::pair<std::string, PropertyValue>> &equalityPredicates,
				const std::vector<std::pair<std::string, PropertyValue>> &parameterPredicates, size_t &candidateRecords,
				std::unordered_set<uint64_t> &seenCandidateIds, MaterializedByteReservation &seenCandidateBytes,
				ExecutionState &state, const InternalVisitor &visitor) {
			if (nativeCandidates.snapshot_generation == 0U) {
				throw std::runtime_error("logical graph native indexed Node candidates have no snapshot generation");
			}
			const size_t candidateBytes = logicalIndexCandidateBatchBytes(nativeCandidates, state.shared.context);
			if (candidateBytes > state.shared.bounds.maximum_candidate_bytes) {
				throw std::runtime_error("logical graph indexed Node candidate batch exceeded its byte bound");
			}
			debug::PerfTrace::addValue(
					"logical_plan.index_candidate.materialized_bytes",
					static_cast<int64_t>(
							std::min(candidateBytes, static_cast<size_t>(std::numeric_limits<int64_t>::max()))));
			const std::optional<uint64_t> snapshotGeneration = state.shared.snapshot.nativeSnapshotGeneration();
			if (!snapshotGeneration.has_value() || *snapshotGeneration == 0U ||
				nativeCandidates.snapshot_generation != *snapshotGeneration) {
				throw std::runtime_error(
						"logical graph native indexed Node candidates do not belong to the pinned snapshot");
			}
			if (candidateRecords > state.shared.bounds.maximum_node_records ||
				nativeCandidates.candidates.size() > state.shared.bounds.maximum_node_records - candidateRecords) {
				throw std::runtime_error("logical graph indexed Node lookup exceeded the Node scan bound");
			}
			candidateRecords += nativeCandidates.candidates.size();
			std::unordered_set<std::string> requiredCandidateKeys;
			for (const auto &[key, value]: equalityPredicates) {
				static_cast<void>(value);
				if (key.empty()) {
					throw std::invalid_argument("logical-plan indexed Node predicate has an empty Property key");
				}
				requiredCandidateKeys.insert(key);
			}
			for (const logical::RangePredicate &range: scan.getRangePredicates()) {
				if (range.key.empty()) {
					throw std::invalid_argument("logical-plan indexed Node range predicate has an empty Property key");
				}
				requiredCandidateKeys.insert(range.key);
			}
			std::unordered_map<std::string, const std::vector<PropertyValue> *> candidateColumns;
			if (nativeCandidates.predicates_verified) {
				if (!nativeCandidates.candidates.empty() && nativeCandidates.verified_property_columns.empty()) {
					throw std::runtime_error(
							"logical graph native indexed Node candidates have no verified Property columns");
				}
				for (const auto &column: nativeCandidates.verified_property_columns) {
					if (column.key.empty() || column.values.size() != nativeCandidates.candidates.size() ||
						!candidateColumns.emplace(column.key, &column.values).second) {
						throw std::runtime_error("logical graph native indexed Node candidates have mismatched "
												 "verified Property fields");
					}
				}
				for (const std::string &key: requiredCandidateKeys) {
					if (!candidateColumns.contains(key)) {
						throw std::runtime_error("logical graph native indexed Node candidates omit a requested "
												 "verified Property field");
					}
				}
			}
			// The immutable producer batch stays alive while rows are handed downstream.
			// Reserve its retained storage as well as each independent row-cache copy.
			MaterializedByteReservation verifiedCandidateBytes(&state.shared.materialized_budget);
			if (nativeCandidates.predicates_verified)
				verifiedCandidateBytes.set(candidateBytes);
			MaterializedByteReservation candidateScratchBytes(&state.shared.materialized_budget);
			const bool residualColumns = !nativeCandidates.predicates_verified && state.shared.snapshot.supportsNativeNodeRefScans() &&
				!nativeCandidates.candidates.empty();
			if (!residualColumns) {
				candidateScratchBytes.set(checkedSizeAdd(
					checkedSizeMul(nativeCandidates.candidates.size(), sizeof(uint64_t), "logical-plan candidate scratch overflowed"),
					nativeCandidates.candidates.size() / 8U + (nativeCandidates.candidates.size() % 8U != 0U),
					"logical-plan candidate scratch overflowed"));
			}
			uint64_t previousNodeId = 0U;
			std::vector<uint64_t> candidateNodeIds;
			if (!residualColumns) candidateNodeIds.reserve(nativeCandidates.candidates.size());
			for (const storage::EntityRef &candidate: nativeCandidates.candidates) {
				validateNodeRef(candidate,
								"logical graph indexed Node lookup returned invalid or non-monotonic Node references");
				if (candidate.entity_id <= previousNodeId) {
					throw std::runtime_error(
							"logical graph indexed Node lookup returned a non-monotonic candidate batch");
				}
				previousNodeId = candidate.entity_id;
				if (seenCandidateIds.contains(candidate.entity_id)) {
					throw std::runtime_error("logical graph indexed Node lookup returned duplicate Node references");
				}

				if (!residualColumns) candidateNodeIds.push_back(candidate.entity_id);
			}
			const auto rememberCandidate = [&](uint64_t id) {
				const size_t previousSeenBytes = seenCandidateBytes.bytes();
				seenCandidateBytes.set(checkedSizeAdd(previousSeenBytes, sizeof(uint64_t),
													  "logical-plan candidate seen-ID size overflowed"));
				try {
					seenCandidateIds.insert(id);
				} catch (...) {
					seenCandidateBytes.set(previousSeenBytes);
					throw;
				}
			};
			std::vector<bool> candidateLabelsMatch(residualColumns ? 0U : nativeCandidates.candidates.size(), true);
			if (!nativeCandidates.label_scope_verified && !candidateNodeIds.empty() && !scan.getLabels().empty()) {
				candidateLabelsMatch = state.shared.snapshot.readNodeLabelMatchesSorted(candidateNodeIds, scan.getLabels());
				if (candidateLabelsMatch.size() != candidateNodeIds.size()) {
					throw std::runtime_error("logical graph backend changed indexed Node label-match cardinality");
				}
				if (std::ranges::any_of(candidateLabelsMatch, [](const bool matches) { return !matches; })) {
					throw std::runtime_error(
							"logical graph native indexed Node candidates violate the requested label scope");
				}
			}
			// An index candidate stream is only an access path.  When the index does
			// not already provide verified predicate columns, validate the residual
			// predicates as one selected-column batch instead of reopening a complete
			// Node for every candidate.  Complete materialization remains limited to
			// candidates that survive the batch; deferred consumers can keep the
			// authenticated EntityRef all the way to their result boundary.
			if (residualColumns) {
				std::vector<std::string> propertyKeys;
				const auto appendPropertyKey = [&propertyKeys](const std::string &key) {
					if (key.empty()) {
						throw std::invalid_argument("logical-plan indexed Node predicate has an empty Property key");
					}
					if (std::ranges::find(propertyKeys, key) == propertyKeys.end()) {
						propertyKeys.push_back(key);
					}
				};
				for (const auto &[key, value]: scan.getPropertyPredicates()) {
					static_cast<void>(value);
					appendPropertyKey(key);
				}
				for (const auto &[key, value]: parameterPredicates) {
					static_cast<void>(value);
					appendPropertyKey(key);
				}
				for (const logical::RangePredicate &range: scan.getRangePredicates()) {
					appendPropertyKey(range.key);
				}
				if (const auto &composite = scan.getCompositeEquality(); composite.has_value()) {
					if (composite->keys.empty() || composite->keys.size() != composite->values.size()) {
						throw std::invalid_argument(
								"logical-plan indexed Node composite predicate has an invalid shape");
					}
					for (const std::string &key: composite->keys) {
						appendPropertyKey(key);
					}
				}
				std::vector<std::string_view> keyViews;
				keyViews.reserve(propertyKeys.size());
				for (const std::string &key: propertyKeys) {
					keyViews.push_back(key);
				}
				const size_t minimumCandidateBytes =
						checkedSizeAdd(sizeof(storage::EntityRef),
									   checkedSizeMul(propertyKeys.size(), sizeof(PropertyValue),
													  "logical-plan indexed Node batch capacity overflowed"),
									   "logical-plan indexed Node batch capacity overflowed");
				const size_t byteBatchCapacity =
						std::max<size_t>(1U, state.shared.bounds.maximum_materialized_bytes / minimumCandidateBytes);
				// Adapt the row cap before reading. Admission-capable readers reserve
				// payload bytes at the source; compatibility readers are checked on
				// return. Pre-delivery pressure can further split these ranges.
				const size_t candidateBatchCapacity =
						std::min({DEFAULT_PROPERTY_BATCH_ROWS, state.shared.bounds.maximum_node_records,
								  state.shared.bounds.maximum_rows, byteBatchCapacity});
				if (candidateBatchCapacity == 0U) {
					throw std::runtime_error("logical-plan indexed Node Property batch has no positive capacity");
				}
				struct IndexedColumnBytePressure {};
				std::function<bool(size_t, size_t)> processRange;
				processRange = [&](size_t offset, size_t count) -> bool {
					try {
						checkGuard(state);
						MaterializedByteReservation idBytes(&state.shared.materialized_budget);
						const size_t idSlots = checkedSizeMul(count, sizeof(uint64_t), "logical-plan residual IDs overflowed");
						if (idBytes.wouldExceed(idSlots)) throw IndexedColumnBytePressure{};
						idBytes.set(idSlots);
						std::vector<uint64_t> batchIds;
						batchIds.reserve(count);
						idBytes.set(checkedSizeMul(batchIds.capacity(), sizeof(uint64_t), "logical-plan residual IDs overflowed"));
						for (size_t i = 0U; i < count; ++i) batchIds.push_back(nativeCandidates.candidates[offset + i].entity_id);
						std::vector<bool> labelsMatch;
						if (!nativeCandidates.label_scope_verified && !scan.getLabels().empty()) {
							const size_t labelSlots = checkedSizeAdd(idSlots, count / 8U + (count % 8U != 0U), "logical-plan residual labels overflowed");
							if (idBytes.wouldExceed(labelSlots)) throw IndexedColumnBytePressure{};
							idBytes.set(labelSlots);
							labelsMatch = state.shared.snapshot.readNodeLabelMatchesSorted(batchIds, scan.getLabels());
							if (labelsMatch.size() != count) throw std::runtime_error("logical graph indexed Node labels changed cardinality");
							idBytes.set(checkedSizeAdd(idSlots, labelsMatch.capacity() / 8U + (labelsMatch.capacity() % 8U != 0U), "logical-plan residual labels overflowed"));
						}
						MaterializedByteReservation rowBytes(&state.shared.materialized_budget);
						LogicalPlanReadRow row;
						const auto prepareRow = [&](const storage::EntityRef &candidate) {
							row.values.emplace(scan.getVariable(), candidate);
							for (const auto &key: propertyKeys)
								row.property_cache[scan.getVariable()].emplace(key, PropertyValue());
						};
						prepareRow(nativeCandidates.candidates[offset]);
						const size_t structuralBytes = logicalReadRowBytes(row, state.shared.context);
						if (rowBytes.wouldExceed(structuralBytes))
							throw IndexedColumnBytePressure{};
						rowBytes.set(structuralBytes);
						MaterializedByteReservation columnBytes(&state.shared.materialized_budget);
						const std::function<void(size_t)> admission = [&](size_t bytes) {
							checkGuard(state);
							const size_t next = checkedSizeAdd(columnBytes.bytes(), bytes,
															   "logical-plan indexed Node source size overflowed");
							if (columnBytes.wouldExceed(next))
								throw IndexedColumnBytePressure{};
							columnBytes.set(next);
							debug::PerfTrace::addValue(
									"logical_plan.index_predicate_source_bytes",
									static_cast<int64_t>(
											std::min(bytes, static_cast<size_t>(std::numeric_limits<int64_t>::max()))));
						};
						auto columns = propertyKeys.empty() ? std::vector<std::vector<PropertyValue>>{}
									   : state.shared.snapshot.supportsPropertyReadAdmission()
											   ? state.shared.snapshot.readNodePropertyColumnsSortedWithAdmission(
														 batchIds, keyViews, admission)
											   : state.shared.snapshot.readNodePropertyColumnsSorted(batchIds, keyViews);
						if (columns.size() != propertyKeys.size() ||
							std::ranges::any_of(columns, [&](const auto &column) { return column.size() != count; }))
							throw std::runtime_error(
									"logical graph backend changed indexed Node Property column cardinality");
						const size_t selectedBytes = propertyColumnBytes(
								columns, keyViews, "logical-plan indexed Node Property batch size overflowed", state.shared.context);
						if (columnBytes.wouldExceed(selectedBytes))
							throw IndexedColumnBytePressure{};
						columnBytes.set(selectedBytes);
						for (size_t batchIndex = 0U; batchIndex < count; ++batchIndex) {
							checkGuard(state);
							const size_t candidateIndex = offset + batchIndex;
							const auto &candidate = nativeCandidates.candidates[candidateIndex];
							rememberCandidate(candidate.entity_id);
							incrementBounded(state.task.stats.nodes_scanned,
											 "logical-plan indexed Node lookup counter overflowed");
							if (batchIndex != 0U) {
								row = LogicalPlanReadRow{};
								prepareRow(candidate);
								rowBytes.set(structuralBytes);
							}
							for (size_t column = 0U; column < propertyKeys.size(); ++column) {
								const size_t valueBytes =
										detail::retainedPropertyValueBytes(columns[column][batchIndex], [&] { checkGuardNow(state); });
								columnBytes.set(columnBytes.bytes() - valueBytes);
								rowBytes.set(checkedSizeAdd(rowBytes.bytes() - 1U, valueBytes,
															"logical-plan indexed Node row size overflowed"));
								row.property_cache.at(scan.getVariable()).at(propertyKeys[column]) =
										std::move(columns[column][batchIndex]);
							}
							if (!nodeRefScanMatches(state.shared.snapshot, row, scan.getVariable(), scan, parameterPredicates,
													state, labelsMatch.empty() || labelsMatch[batchIndex])) {
								row = LogicalPlanReadRow{};
								rowBytes.reset();
								continue;
							}
							if (!state.shared.allow_deferred_entity_refs) {
								auto node = readNodeCached(state, candidate.entity_id);
								if (!nodeScanMatches(state.shared.snapshot, node, scan, parameterPredicates, state)) {
									throw std::runtime_error(
											"logical graph indexed Node candidate changed during materialization");
								}
								row.values[scan.getVariable()] = std::move(node);
								row.property_cache.erase(scan.getVariable());
								rowBytes.set(logicalReadRowBytes(row, state.shared.context));
							}
							rowBytes.reset();
							if (!visitor(std::move(row)))
								return false;
						}
						return true;
					} catch (const IndexedColumnBytePressure &) {
						if (count == 1U)
							throw std::runtime_error(
									"logical-plan indexed Node Property batch exceeded its byte bound");
					}
					debug::PerfTrace::addValue("logical_plan.index_predicate_split_batches", 1);
					const size_t left = count / 2U;
					return processRange(offset, left) && processRange(offset + left, count - left);
				};
				for (size_t offset = 0U; offset < nativeCandidates.candidates.size(); offset += candidateBatchCapacity) {
					if (!processRange(offset, std::min(candidateBatchCapacity, nativeCandidates.candidates.size() - offset)))
						return false;
				}
				return true;
			}
			for (size_t candidateIndex = 0U; candidateIndex < nativeCandidates.candidates.size(); ++candidateIndex) {
				checkGuard(state);
				const storage::EntityRef &candidate = nativeCandidates.candidates[candidateIndex];
				rememberCandidate(candidate.entity_id);
				incrementBounded(state.task.stats.nodes_scanned, "logical-plan indexed Node lookup counter overflowed");
				if ((nativeCandidates.exact_match_verified || nativeCandidates.predicates_verified) &&
					state.shared.allow_deferred_entity_refs && state.shared.snapshot.supportsNativeNodeRefScans()) {
					MaterializedByteReservation rowBytes(&state.shared.materialized_budget);
					LogicalPlanReadRow row;
					row.values.emplace(scan.getVariable(), candidate);
					rowBytes.set(logicalReadRowBytes(row, state.shared.context));
					const auto copyProperty = [&](const std::string &key, const PropertyValue &value) {
						size_t previous = 0U;
						if (const auto cache = row.property_cache.find(scan.getVariable());
							cache != row.property_cache.end()) {
							if (const auto found = cache->second.find(key); found != cache->second.end()) {
								previous =
										checkedSizeAdd(key.size(), property_utils::getPropertyValueSize(found->second),
													   "logical-plan candidate row size overflowed");
							}
						}
						const size_t next =
								checkedSizeAdd(rowBytes.bytes() - previous,
											   checkedSizeAdd(key.size(), property_utils::getPropertyValueSize(value),
															  "logical-plan candidate row size overflowed"),
											   "logical-plan candidate row size overflowed");
						rowBytes.set(next);
						row.property_cache[scan.getVariable()].insert_or_assign(key, value);
					};
					// Include the cache variable name before publishing any owned values.
					rowBytes.set(checkedSizeAdd(rowBytes.bytes(), scan.getVariable().size(),
												"logical-plan candidate row size overflowed"));
					if (nativeCandidates.predicates_verified) {
						for (const auto &column: nativeCandidates.verified_property_columns) {
							copyProperty(column.key, column.values[candidateIndex]);
						}
						if (!nodeRefScanMatches(state.shared.snapshot, row, scan.getVariable(), scan, parameterPredicates,
												state, candidateLabelsMatch[candidateIndex])) {
							throw std::runtime_error("logical graph native indexed Node candidates violate their "
													 "verified Property predicates");
						}
					} else {
						for (const auto &[key, value]: equalityPredicates) {
							copyProperty(key, value);
						}
					}
					rowBytes.reset();
					if (!visitor(std::move(row))) {
						return false;
					}
					continue;
				}
				const storage::LogicalGraphNode node = readNodeCached(state, candidate.entity_id);
				if (nodeScanMatches(state.shared.snapshot, node, scan, parameterPredicates, state)) {
					LogicalPlanReadRow row;
					row.values.emplace(scan.getVariable(), node);
					if (!visitor(std::move(row))) {
						return false;
					}
				}
			}
			return true;
		}

		// LIMIT may cross only cardinality-preserving projections. Keep the
		// original input ordinal as the final key so ties match stable full sort.
		[[nodiscard]] bool visitBoundedTopKSort(const logical::LogicalSort &sort, ExecutionState &state,
			const InternalVisitor &visitor, const ChargedRowVisitor *chargedReceiver, size_t maximum) {
			using Record = detail::QuerySpillRowCodec::Record;
			const auto &items = sort.getSortItems();
			MaterializedByteReservation metadata(&state.shared.materialized_budget);
			metadata.set(checkedSizeMul(items.size(), sizeof(const expressions::Expression *), "Top-K metadata overflowed"));
			std::vector<const expressions::Expression *> candidates;
			candidates.reserve(items.size());
			metadata.set(checkedSizeMul(candidates.capacity(), sizeof(const expressions::Expression *), "Top-K metadata overflowed"));
			for (const auto &item : items) candidates.push_back(item.expression.get());
			std::optional<detail::BoundExpressionInputs> inputs;
			if (state.shared.bound_program)
				inputs.emplace(*state.shared.bound_program, state.shared.bound_program->id(sort), candidates,
					state.shared.snapshot, state.shared.materialized_budget, [&] { checkGuardNow(state); },
					std::pmr::get_default_resource(), state.shared.context);
			const auto bytes = [&](const Record &record) {
				size_t total = checkedSizeAdd(logicalReadRowBytes(record.row, state.shared.context),
					checkedSizeMul(record.keys.capacity(), sizeof(PropertyValue), "Top-K keys overflowed"), "Top-K row overflowed");
				for (const auto &key : record.keys)
					total = checkedSizeAdd(total, retainedExpressionValueBytes(key, state), "Top-K payload overflowed");
				return total;
			};
			BoundedBatch<Record> rows(maximum, state.shared.bounds.maximum_materialized_bytes, bytes,
				&state.shared.materialized_budget, true, sizeof(LogicalPlanReadRow));
			const auto precedes = [&](const Record &left, const Record &right) {
				checkGuard(state);
				for (size_t i = 0; i < items.size(); ++i) {
					if (left.keys[i] == right.keys[i]) continue;
					if (detail::orderByPrecedes(left.keys[i], right.keys[i], items[i].ascending)) return true;
					if (detail::orderByPrecedes(right.keys[i], left.keys[i], items[i].ascending)) return false;
					return left.ordinal < right.ordinal;
				}
				return left.ordinal < right.ordinal;
			};
			const auto decorate = [&](Record &record, MaterializedByteReservation &charge) {
				charge.set(checkedSizeAdd(logicalReadRowBytes(record.row, state.shared.context),
					checkedSizeMul(items.size(), sizeof(PropertyValue), "Top-K keys overflowed"), "Top-K row overflowed"));
				record.keys.reserve(items.size());
				charge.set(bytes(record));
				for (const auto &item : items) {
					checkGuard(state);
					PropertyValue key;
					{ ScopedBoundInputs scoped(state, inputs, record.row); key = scalarValue(evaluateExpression(*item.expression, record.row, state)); }
					charge.set(checkedSizeAdd(bytes(record), retainedExpressionValueBytes(key, state), "Top-K key growth overflowed"));
					record.keys.push_back(std::move(key));
					charge.set(bytes(record));
				}
			};
			const auto retain = [&](Record record, MaterializedByteReservation charge) {
				decorate(record, charge);
				if (rows.size() == maximum) {
					if (!precedes(record, rows.values().front())) return;
					std::pop_heap(rows.values().begin(), rows.values().end(), precedes);
					{ auto removed = rows.popBackCharged(); }
				}
				rows.appendCharged(std::move(record), std::move(charge));
				std::push_heap(rows.values().begin(), rows.values().end(), precedes);
			};
			struct Pending { MaterializedByteReservation charge; Record record; };
			std::optional<Pending> first;
			size_t seen = 0U;
			const ChargedRowVisitor collect = [&](LogicalPlanReadRow row, MaterializedByteReservation charge) {
				checkGuard(state);
				if (seen == state.shared.bounds.maximum_rows)
					throw std::runtime_error("logical-plan Sort exceeded its materialization row bound");
				Record record{std::move(row), {}, seen++};
				if (seen == 1U) {
					if (!charge.bytes()) charge.set(bytes(record));
					first.emplace(Pending{std::move(charge), std::move(record)});
					return true;
				}
				if (first) { retain(std::move(first->record), std::move(first->charge)); first.reset(); }
				retain(std::move(record), std::move(charge));
				return true;
			};
			const InternalVisitor uncharged = [&](LogicalPlanReadRow row) {
				return collect(std::move(row), MaterializedByteReservation(&state.shared.materialized_budget));
			};
			const auto expressions = selectBatchableExpressions(candidates);
			const auto complete = selectBatchableCompleteEntityVariables(candidates);
			const auto &child = requireOnlyChild(sort);
			const auto batched = tryVisitBatchedPropertyExpressions(child, expressions, state, uncharged, {}, complete, &collect);
			const bool completed = batched ? *batched : visitOperator(child, state, uncharged,
				supportsChargedRowHandoff(child.getType()) ? &collect : nullptr);
			if (!completed) throw std::runtime_error("Top-K could not prove its input was exhausted");
			const auto emit = [&](Record record, MaterializedByteReservation charge) {
				std::vector<PropertyValue>().swap(record.keys);
				charge.set(logicalReadRowBytes(record.row, state.shared.context));
				if (chargedReceiver) return (*chargedReceiver)(std::move(record.row), std::move(charge));
				charge.reset(); return visitor(std::move(record.row));
			};
			debug::PerfTrace::addValue("logical_plan.sort.topk_input_rows", seen);
			debug::PerfTrace::addValue("logical_plan.sort.topk_retained_rows", rows.size() + (first ? 1U : 0U));
			if (first) return emit(std::move(first->record), std::move(first->charge));
			std::sort_heap(rows.values().begin(), rows.values().end(), precedes);
			std::reverse(rows.values().begin(), rows.values().end());
			while (!rows.empty()) {
				auto row = rows.popBackCharged();
				if (!emit(std::move(row.value), std::move(row.reservation))) return false;
			}
			return true;
		}

		[[nodiscard]] bool visitSpillableSort(const logical::LogicalSort &sort,ExecutionState &state,
			const InternalVisitor &visitor,const ChargedRowVisitor *chargedReceiver,
			storage::LogicalQueryScratchProvider &provider) {
			using Record=detail::QuerySpillRowCodec::Record;
			const auto &items=sort.getSortItems();
			MaterializedByteReservation metadata(&state.shared.materialized_budget);
			metadata.set(checkedSizeMul(items.size(),4U*sizeof(const expressions::Expression *)+sizeof(uint8_t),"Sort spill metadata overflowed"));
			std::vector<const expressions::Expression *> candidates;std::vector<uint8_t> ascending;
			candidates.reserve(items.size());ascending.reserve(items.size());
			for(const auto &item:items){if(!item.expression)throw std::invalid_argument("Sort has an empty expression");candidates.push_back(item.expression.get());ascending.push_back(item.ascending?1:0);}
			std::optional<detail::BoundExpressionInputs> inputs;
			if(state.shared.bound_program)inputs.emplace(*state.shared.bound_program,state.shared.bound_program->id(sort),candidates,
				state.shared.snapshot,state.shared.materialized_budget,[&]{checkGuardNow(state);},std::pmr::get_default_resource(),state.shared.context);
			const auto fixedScalar=[](const PropertyValue &value){const auto type=value.getType();return type==PropertyType::NULL_TYPE || type==PropertyType::BOOLEAN || type==PropertyType::INTEGER || type==PropertyType::DOUBLE;};
			const auto rowBytes=[&](const LogicalPlanReadRow &row){checkGuard(state);
				const bool simple=row.property_cache.empty() && row.entity_metadata_cache.empty() && row.complete_entity_property_cache.empty() && row.path_cache.empty() &&
					std::ranges::all_of(row.values,[&](const auto &entry){const auto *value=std::get_if<PropertyValue>(&entry.second);return value?fixedScalar(*value):std::holds_alternative<storage::EntityRef>(entry.second);});
				return logicalReadRowBytes(row,simple?nullptr:state.shared.context);};
			const auto keyBytes=[&](const PropertyValue &value){if(!fixedScalar(value))return retainedExpressionValueBytes(value,state);checkGuard(state);return detail::retainedPropertyValueBytes(value,[]{});};

			const auto bytes=[&](const Record &record){size_t total=rowBytes(record.row);
				total=checkedSizeAdd(total,checkedSizeMul(record.keys.capacity(),sizeof(PropertyValue),"Sort spill keys overflowed"),"Sort spill row overflowed");
				for(const auto &key:record.keys)total=checkedSizeAdd(total,keyBytes(key),"Sort spill key payload overflowed");return total;};
			detail::ExternalQuerySort sorter(provider,state.shared.materialized_budget,state.shared.bounds.maximum_rows,
				state.shared.bounds.maximum_materialized_bytes,ascending,bytes,[&]{checkGuard(state);},128U*1024U*1024U,1024U,[&]{checkGuardNow(state);},state.shared.context?state.shared.context->resource_account:nullptr);
			struct Pending {MaterializedByteReservation charge;Record record;};std::optional<Pending> pending;bool multiple=false;
			const auto decorate=[&](Record &record,MaterializedByteReservation &charge){
				charge.set(checkedSizeAdd(rowBytes(record.row),checkedSizeMul(items.size(),sizeof(PropertyValue),"Sort spill keys overflowed"),"Sort spill row overflowed"));
				record.keys.reserve(items.size());charge.set(bytes(record));
				for(const auto &item:items){checkGuard(state);PropertyValue key;
					{ScopedBoundInputs scoped(state,inputs,record.row);key=scalarValue(evaluateExpression(*item.expression,record.row,state));}
					const auto extra=keyBytes(key);charge.set(checkedSizeAdd(bytes(record),extra,"Sort spill key growth overflowed"));record.keys.push_back(std::move(key));charge.set(bytes(record));
				}
			};
			const ChargedRowVisitor collect=[&](LogicalPlanReadRow row,MaterializedByteReservation charge){
				if(!multiple && !pending){pending.emplace(Pending{std::move(charge),Record{std::move(row),{},0U}});if(!pending->charge.bytes())pending->charge.set(bytes(pending->record));return true;}
				if(!multiple){multiple=true;decorate(pending->record,pending->charge);sorter.append(std::move(pending->record),std::move(pending->charge));pending.reset();}
				Record record{std::move(row),{},0U};decorate(record,charge);sorter.append(std::move(record),std::move(charge));return true;
			};
			const InternalVisitor uncharged=[&](LogicalPlanReadRow row){return collect(std::move(row),MaterializedByteReservation(&state.shared.materialized_budget));};
			const auto expressions=selectBatchableExpressions(candidates);const auto complete=selectBatchableCompleteEntityVariables(candidates);
			const auto &child=requireOnlyChild(sort);const auto batched=tryVisitBatchedPropertyExpressions(child,expressions,state,uncharged,{},complete,&collect);
			const bool completed=batched ? *batched : visitOperator(child,state,uncharged,supportsChargedRowHandoff(child.getType())?&collect:nullptr);
			if(!completed)throw std::runtime_error("Sort spill could not prove its input was exhausted");
			const auto emit=[&](Record record,MaterializedByteReservation charge){std::vector<PropertyValue>().swap(record.keys);charge.set(rowBytes(record.row));
				if(chargedReceiver)return (*chargedReceiver)(std::move(record.row),std::move(charge));charge.reset();return visitor(std::move(record.row));};
			if(pending)return emit(std::move(pending->record),std::move(pending->charge));
			const bool result=sorter.finish(emit);if(sorter.spilled())debug::PerfTrace::addValue("logical_plan.sort.spilled",1);return result;
		}

