		[[nodiscard]] std::optional<bool> findPatternReferenceMatch(const uint64_t sourceNodeId,
																	const std::string &edgeType,
																	expressions::PatternDirection direction,
																	ExecutionState &state) {
			if (!state.shared.snapshot.supportsNativeAdjacentEdgeRefScans() || !state.shared.snapshot.supportsNodeExistenceProbes() ||
				(!edgeType.empty() && !state.shared.snapshot.supportsNativeRelationshipTypeTokens(edgeType)) ||
				(direction != expressions::PatternDirection::PAT_OUTGOING &&
				 direction != expressions::PatternDirection::PAT_INCOMING &&
				 direction != expressions::PatternDirection::PAT_BOTH)) {
				return std::nullopt;
			}
			if (sourceNodeId == 0U) {
				throw std::runtime_error("logical-plan pattern expression has an invalid source Node identity");
			}
			std::optional<uint64_t> edgeTypeToken;
			if (!edgeType.empty()) {
				edgeTypeToken = state.shared.snapshot.findRelationshipTypeToken(edgeType);
				if (!edgeTypeToken.has_value()) {
					return false;
				}
			}
			size_t sourceEdges = 0U;
			const auto scanDirection = [&](const storage::LogicalGraphReadDirection adjacencyDirection) {
				std::optional<storage::LogicalGraphAdjacencyContinuation> continuation;
				uint64_t previousEdgeId = 0U;
				for (;;) {
					if (sourceEdges == state.shared.bounds.maximum_edges_per_node) {
						throw std::runtime_error("logical-plan pattern expression exceeded its per-Node Edge bound");
					}
					const size_t pageRecords = std::min(state.shared.bounds.maximum_edges_per_node - sourceEdges,
														state.shared.bounds.maximum_adjacency_page_records);
					MaterializedByteReservation referenceBytes(&state.shared.materialized_budget);
					const auto page = readReferencePageBudgeted(state, sourceNodeId, adjacencyDirection, continuation, pageRecords, referenceBytes);
					if (page.edges.size() > pageRecords || (page.edges.empty() && page.continuation.has_value()) ||
						(continuation.has_value() &&
						 (page.edges.empty() || page.edges.front().entity_id != continuation->next_edge_id))) {
						throw std::runtime_error(
								"logical graph backend returned an invalid pattern-expression reference page");
					}
					// Validate and filter the complete page before probing endpoints.  The
					// endpoint requests are then sorted/deduplicated for one bounded
					// Directory lookup, while candidates retain their original order.
					MaterializedByteReservation candidateBytes(&state.shared.materialized_budget);
					candidateBytes.set(checkedSizeMul(page.edges.size(), sizeof(const storage::EntityRef *),
						"logical-plan pattern candidate slots overflowed"));
					std::vector<const storage::EntityRef *> candidates;
					candidates.reserve(page.edges.size());
					candidateBytes.set(checkedSizeMul(candidates.capacity(), sizeof(const storage::EntityRef *),
						"logical-plan pattern candidate capacity overflowed"));
					for (const storage::EntityRef &reference: page.edges) {
						checkGuard(state);
						validateEdgeRef(reference,
										"logical graph backend returned an invalid pattern-expression reference");
						if (reference.entity_id <= previousEdgeId) {
							throw std::runtime_error(
									"logical graph backend returned an invalid pattern-expression reference");
						}
						previousEdgeId = reference.entity_id;
						const bool endpointMatches =
								adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
										? reference.source_node_id == sourceNodeId
										: reference.target_node_id == sourceNodeId;
						if (!endpointMatches) {
							throw std::runtime_error(
									"logical graph backend returned an invalid pattern-expression endpoint");
						}
						incrementBounded(sourceEdges,
										 "logical-plan pattern-expression per-Node Edge counter overflowed");
						incrementBounded(state.task.stats.edges_scanned,
										 "logical-plan pattern-expression Edge counter overflowed");
						if (direction == expressions::PatternDirection::PAT_BOTH &&
							adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_INCOMING &&
							reference.source_node_id == sourceNodeId) {
							continue;
						}
						if (edgeTypeToken.has_value() && reference.type_token != *edgeTypeToken) {
							continue;
						}
						candidates.push_back(&reference);
					}
					if (candidates.empty()) {
						if (!page.continuation.has_value()) {
							return false;
						}
					} else if (state.shared.snapshot.supportsNodeExistenceProbes()) {
						MaterializedByteReservation targetBytes(&state.shared.materialized_budget);
						targetBytes.set(checkedSizeMul(candidates.size(), sizeof(uint64_t),
							"logical-plan pattern endpoint slots overflowed"));
						std::vector<uint64_t> targetIds;
						targetIds.reserve(candidates.size());
						targetBytes.set(checkedSizeMul(targetIds.capacity(), sizeof(uint64_t),
							"logical-plan pattern endpoint capacity overflowed"));
						for (const storage::EntityRef *reference: candidates) {
							targetIds.push_back(adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
														? reference->target_node_id
														: reference->source_node_id);
						}
						detail::sortUniqueIdsGuarded(targetIds, [&] {
							if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
						});
						MaterializedByteReservation presenceBytes(&state.shared.materialized_budget);
						// Reserve a complete machine word for the final partial bit block.
						const auto presenceSize = [&](size_t bits) {
							return checkedSizeMul(bits / 8U + (bits % 8U != 0U), 1U,
								"logical-plan pattern presence capacity overflowed");
						};
						presenceBytes.set(checkedSizeAdd(presenceSize(targetIds.size()), sizeof(size_t),
							"logical-plan pattern presence slots overflowed"));
						const std::vector<bool> present = state.shared.snapshot.containsNodesSorted(targetIds);
						presenceBytes.set(presenceSize(present.capacity()));
						checkGuard(state);
						if (present.size() != targetIds.size()) {
							throw std::runtime_error(
									"logical graph backend changed pattern-expression endpoint cardinality");
						}
						for (const storage::EntityRef *reference: candidates) {
							const uint64_t targetId =
									adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
											? reference->target_node_id
											: reference->source_node_id;
							const auto position = std::ranges::lower_bound(targetIds, targetId);
							if (position == targetIds.end() || *position != targetId ||
								!present[static_cast<size_t>(position - targetIds.begin())]) {
								throw std::runtime_error(
										"logical graph backend returned an Edge topology with a missing endpoint");
							}
							return true;
						}
					} else {
						// Preserve the defensive fallback for logical readers that do not
						// implement the optional batch probe.
						const uint64_t targetId =
								adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
										? candidates.front()->target_node_id
										: candidates.front()->source_node_id;
						if (!state.shared.snapshot.containsNode(targetId)) {
							throw std::runtime_error(
									"logical graph backend returned an Edge topology with a missing endpoint");
						}
						return true;
					}
					if (!page.continuation.has_value()) {
						return false;
					}
					if (page.edges.empty() || page.continuation->previous_edge_id != page.edges.back().entity_id ||
						page.continuation->next_edge_id == 0U ||
						page.continuation->previous_edge_id == page.continuation->next_edge_id) {
						throw std::runtime_error(
								"logical graph backend returned an incomplete pattern-expression reference scan");
					}
					continuation = page.continuation;
				}
			};
			if (direction == expressions::PatternDirection::PAT_OUTGOING ||
				direction == expressions::PatternDirection::PAT_BOTH) {
				if (scanDirection(storage::LogicalGraphReadDirection::LGRD_OUTGOING)) {
					return true;
				}
			}
			if (direction == expressions::PatternDirection::PAT_INCOMING ||
				direction == expressions::PatternDirection::PAT_BOTH) {
				if (scanDirection(storage::LogicalGraphReadDirection::LGRD_INCOMING)) {
					return true;
				}
			}
			return false;
		}

		[[nodiscard]] bool visitPatternTargets(const storage::LogicalGraphNode &source, const std::string &edgeType,
											   std::string_view targetLabel, expressions::PatternDirection direction,
											   ExecutionState &state, const PatternMatchVisitor &visitor,
											   const PatternBindingReadShape *bindingShape = nullptr,
											   bool lazyCandidates = false) {
			if (!visitor) {
				throw std::invalid_argument("logical-plan pattern expression requires a target visitor");
			}
			validateNode(source, state);
			const auto validateCandidate = [&](const storage::LogicalGraphEdge &edge,
											   const storage::LogicalGraphNode &target) {
				validateNode(target, state);
				validateEdge(edge);
				if (lazyCandidates && (target.properties.size() > state.shared.bounds.maximum_property_entries ||
									   edge.properties.size() > state.shared.bounds.maximum_property_entries ||
									   checkedSizeAdd(logicalNodeCacheBytes(target, state.shared.context), logicalEdgeCacheBytes(edge, state.shared.context),
													  "logical-plan EXISTS candidate byte count overflowed") >
											   state.shared.bounds.maximum_materialized_bytes)) {
					throw std::runtime_error("logical-plan EXISTS candidate exceeded its Property or byte bound");
				}
			};

			const auto appendNamedProperties = [&](auto &records, const std::vector<uint64_t> &ids,
												   const std::vector<std::string> &keys, bool edgeRecords,
												   MaterializedByteReservation &recordBytes) {
				if (ids.empty() || keys.empty())
					return;
				if (keys.size() > state.shared.bounds.maximum_property_entries) {
					throw std::runtime_error("logical-plan pattern named columns exceeded their Property bound");
				}
				for (size_t index = 0U; index < records.size(); ++index) {
					const auto &record = records[index];
					uint64_t id;
					if constexpr (requires { record.node_id; })
						id = record.node_id;
					else
						id = record.edge_id;
					if (id != ids[index])
						throw std::runtime_error("logical graph pattern named columns changed entity order");
				}
				std::vector<std::string_view> keyViews;
				keyViews.reserve(keys.size());
				for (const auto &key: keys) {
					if (std::ranges::any_of(records, [&](const auto &record) {
							return std::ranges::find(record.properties, key,
													 &storage::LogicalGraphPropertyEntry::key) ==
								   record.properties.end();
						}))
						keyViews.push_back(key);
				}
				// Compatibility readers may already return complete records despite
				// the shape. Reuse those fields instead of issuing a redundant column read.
				if (keyViews.empty())
					return;
				checkGuard(state);
				MaterializedByteReservation columnBytes(&state.shared.materialized_budget);
				const size_t columnFrames = checkedSizeAdd(
						checkedSizeMul(keyViews.size(), sizeof(std::vector<PropertyValue>),
									   "logical-plan pattern column frame size overflowed"),
						checkedSizeMul(checkedSizeMul(keyViews.size(), ids.size(),
													  "logical-plan pattern column frame size overflowed"),
									   sizeof(PropertyValue), "logical-plan pattern column frame size overflowed"),
						"logical-plan pattern column frame size overflowed");
				columnBytes.set(columnFrames);
				const std::function<void(size_t)> admitProperty = [&](size_t bytes) {
					checkGuard(state);
					columnBytes.set(checkedSizeAdd(columnBytes.bytes(), bytes,
												   "logical-plan pattern Property source size overflowed"));
				};
				const bool sourceAdmission = state.shared.snapshot.supportsPropertyReadAdmission();
				auto columns =
						edgeRecords ? (sourceAdmission ? state.shared.snapshot.readEdgePropertyColumnsSortedWithAdmission(
																 ids, keyViews, admitProperty)
													   : state.shared.snapshot.readEdgePropertyColumnsSorted(ids, keyViews))
									: (sourceAdmission ? state.shared.snapshot.readNodePropertyColumnsSortedWithAdmission(
																 ids, keyViews, admitProperty)
													   : state.shared.snapshot.readNodePropertyColumnsSorted(ids, keyViews));
				if (columns.size() != keyViews.size()) {
					throw std::runtime_error("logical graph pattern named columns changed column count");
				}
				size_t retainedColumns = checkedSizeMul(columns.capacity(), sizeof(std::vector<PropertyValue>),
														"logical-plan pattern column size overflowed");
				for (const auto &column: columns) {
					if (column.size() != ids.size()) {
						throw std::runtime_error("logical graph pattern named columns changed row count");
					}
					retainedColumns = checkedSizeAdd(retainedColumns,
													 checkedSizeMul(column.capacity(), sizeof(PropertyValue),
																	"logical-plan pattern column size overflowed"),
													 "logical-plan pattern column size overflowed");
					for (const PropertyValue &value: column) {
						retainedColumns = checkedSizeAdd(retainedColumns, property_utils::getPropertyValueSize(value),
														 "logical-plan pattern column size overflowed");
					}
				}
				columnBytes.set(retainedColumns);
				for (size_t column = 0U; column < keyViews.size(); ++column) {
					for (size_t index = 0U; index < records.size(); ++index) {
						checkGuard(state);
						auto &entries = records[index].properties;
						const auto existing =
								std::ranges::find(entries, keyViews[column], &storage::LogicalGraphPropertyEntry::key);
						const size_t transferred = property_utils::getPropertyValueSize(columns[column][index]);
						const size_t previousValue =
								existing == entries.end() ? 0U : property_utils::getPropertyValueSize(existing->value);
						const size_t entryGrowth =
								existing == entries.end()
										? checkedSizeAdd(sizeof(storage::LogicalGraphPropertyEntry),
														 keyViews[column].size(),
														 "logical-plan pattern Property entry size overflowed")
										: 0U;
						columnBytes.set(columnBytes.bytes() - transferred);
						recordBytes.set(
								checkedSizeAdd(recordBytes.bytes(),
											   checkedSizeAdd(entryGrowth, transferred,
															  "logical-plan pattern Property handoff size overflowed"),
											   "logical-plan pattern Property handoff size overflowed"));
						if (existing == entries.end())
							entries.push_back({std::string(keyViews[column]), std::move(columns[column][index])});
						else
							existing->value = std::move(columns[column][index]);
						recordBytes.set(recordBytes.bytes() - previousValue);
					}
				}
				size_t bytes = 0U;
				for (const auto &record: records) {
					size_t recordBytes;
					if constexpr (requires { record.node_id; })
						recordBytes = logicalNodeCacheBytes(record, state.shared.context);
					else
						recordBytes = logicalEdgeCacheBytes(record, state.shared.context);
					bytes = checkedSizeAdd(bytes, recordBytes, "logical-plan pattern named columns size overflowed");
					if (record.properties.size() > state.shared.bounds.maximum_property_entries ||
						bytes > state.shared.bounds.maximum_materialized_bytes) {
						throw std::runtime_error(
								"logical-plan pattern named columns exceeded their materialized bound");
					}
				}
			};

			if (direction != expressions::PatternDirection::PAT_OUTGOING &&
				direction != expressions::PatternDirection::PAT_INCOMING &&
				direction != expressions::PatternDirection::PAT_BOTH) {
				throw std::invalid_argument("logical-plan pattern expression has an unknown direction");
			}
			std::optional<uint64_t> edgeTypeToken;
			const bool canUseReferencePages =
					state.shared.snapshot.supportsNativeAdjacentEdgeRefScans() &&
					(edgeType.empty() || state.shared.snapshot.supportsNativeRelationshipTypeTokens(edgeType));
			if (canUseReferencePages && !edgeType.empty()) {
				edgeTypeToken = state.shared.snapshot.findRelationshipTypeToken(edgeType);
				if (!edgeTypeToken.has_value()) {
					return true;
				}
			}
			size_t sourceEdges = 0U;
			// The target label predicate is fixed for this pattern invocation. Keep a
			// bounded per-invocation cache so repeated targets in a supernode do not
			// reopen the same Entity Map entry for the same logical predicate.
			using LabelMatch = std::pair<uint64_t, bool>;
			MaterializedByteReservation labelCacheBytes(&state.shared.materialized_budget);
			std::vector<LabelMatch> targetLabelMatches;
			const auto cachedLabel = [&](uint64_t id) {
				return std::ranges::find(targetLabelMatches, id, &LabelMatch::first);
			};
			MaterializedByteReservation labelFilterBytes(&state.shared.materialized_budget);
			std::vector<std::string> targetLabelFilter;
			if (!targetLabel.empty()) {
				const size_t nameBytes = std::max(targetLabel.size(), std::string{}.capacity());
				labelFilterBytes.set(checkedSizeAdd(sizeof(std::string), nameBytes,
					"logical-plan pattern label filter slots overflowed"));
				targetLabelFilter.reserve(1U);
				labelFilterBytes.set(checkedSizeAdd(checkedSizeMul(targetLabelFilter.capacity(), sizeof(std::string),
					"logical-plan pattern label filter capacity overflowed"), nameBytes,
					"logical-plan pattern label filter name overflowed"));
				targetLabelFilter.emplace_back(targetLabel);
				labelFilterBytes.set(checkedSizeAdd(checkedSizeMul(targetLabelFilter.capacity(), sizeof(std::string),
					"logical-plan pattern label filter capacity overflowed"), targetLabelFilter.front().capacity(),
					"logical-plan pattern label filter name overflowed"));
			}
			const auto visitDirection = [&](storage::LogicalGraphReadDirection adjacencyDirection) {
				std::optional<storage::LogicalGraphAdjacencyContinuation> continuation;
				uint64_t previousEdgeId = 0U;
				if (canUseReferencePages) {
					for (;;) {
						if (sourceEdges == state.shared.bounds.maximum_edges_per_node) {
							throw std::runtime_error(
									"logical-plan pattern expression exceeded its per-Node Edge bound");
						}
						const size_t pageRecords =
								std::min(state.shared.bounds.maximum_edges_per_node - sourceEdges,
										 lazyCandidates ? size_t{1U} : state.shared.bounds.maximum_adjacency_page_records);
						MaterializedByteReservation referenceBytes(&state.shared.materialized_budget);
						const auto page = readReferencePageBudgeted(state, source.node_id, adjacencyDirection, continuation, pageRecords, referenceBytes);
						if (page.edges.size() > pageRecords || (page.edges.empty() && page.continuation.has_value())) {
							throw std::runtime_error("logical graph backend returned an invalid pattern-expression "
													 "adjacency reference page");
						}
						if (continuation.has_value() &&
							(page.edges.empty() || page.edges.front().entity_id != continuation->next_edge_id)) {
							throw std::runtime_error("logical graph backend returned a discontinuous "
													 "pattern-expression adjacency reference page");
						}
						// Validate and filter the whole bounded reference page first.  A
						// single sorted label-column lookup then serves all distinct target
						// Nodes on the page instead of reopening the Entity Map per Edge.
						MaterializedByteReservation candidateBytes(&state.shared.materialized_budget);
						candidateBytes.set(checkedSizeMul(page.edges.size(), sizeof(const storage::EntityRef *),
							"logical-plan pattern candidate slots overflowed"));
						std::vector<const storage::EntityRef *> candidates;
						candidates.reserve(page.edges.size());
						candidateBytes.set(checkedSizeMul(candidates.capacity(), sizeof(const storage::EntityRef *),
							"logical-plan pattern candidate capacity overflowed"));
						for (const storage::EntityRef &reference: page.edges) {
							checkGuard(state);
							validateEdgeRef(
									reference,
									"logical graph backend returned an invalid pattern-expression adjacency reference");
							if (reference.entity_id <= previousEdgeId) {
								throw std::runtime_error("logical graph backend returned an invalid pattern-expression "
														 "adjacency reference");
							}
							previousEdgeId = reference.entity_id;
							const bool endpointMatches =
									adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
											? reference.source_node_id == source.node_id
											: reference.target_node_id == source.node_id;
							if (!endpointMatches) {
								throw std::runtime_error(
										"logical graph backend returned an invalid pattern-expression Edge reference");
							}
							incrementBounded(sourceEdges,
											 "logical-plan pattern-expression per-Node Edge counter overflowed");
							incrementBounded(state.task.stats.edges_scanned,
											 "logical-plan pattern-expression Edge counter overflowed");
							if (direction == expressions::PatternDirection::PAT_BOTH &&
								adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_INCOMING &&
								reference.source_node_id == source.node_id) {
								continue;
							}
							if (edgeTypeToken.has_value() && reference.type_token != *edgeTypeToken) {
								continue;
							}
							const uint64_t targetId =
									adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
											? reference.target_node_id
											: reference.source_node_id;
							static_cast<void>(targetId);
							candidates.push_back(&reference);
						}
						if (state.shared.snapshot.supportsNodeExistenceProbes() && !candidates.empty()) {
							detail::BudgetMemoryResource endpointMemory(state.shared.materialized_budget);
							std::pmr::vector<uint64_t> endpointIds{&endpointMemory};
							endpointIds.reserve(candidates.size());
							for (const auto *candidate: candidates) {
								endpointIds.push_back(adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
										? candidate->target_node_id : candidate->source_node_id);
							}
							detail::sortUniqueIdsGuarded(endpointIds, [&] {
								if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
							});
							MaterializedByteReservation presenceBytes(&state.shared.materialized_budget);
							presenceBytes.set(packedBooleanBytes(endpointIds.size()));
							const auto present = state.shared.snapshot.containsNodesSorted(endpointIds);
							presenceBytes.set(packedBooleanBytes(present.capacity()));
							checkGuard(state);
							if (present.size() != endpointIds.size()) {
								throw std::runtime_error("logical graph backend changed pattern-expression endpoint cardinality");
							}
							if (std::ranges::any_of(present, [](bool value) { return !value; })) {
								throw std::runtime_error("logical graph backend returned a pattern-expression Edge with a missing endpoint");
							}
						}
						MaterializedByteReservation labelIdsBytes(&state.shared.materialized_budget);
						MaterializedByteReservation labelBitsBytes(&state.shared.materialized_budget);
						std::vector<uint64_t> targetIds;
						std::vector<bool> matches;
						if (!targetLabel.empty() && !candidates.empty()) {
							labelIdsBytes.set(checkedSizeMul(candidates.size(), sizeof(uint64_t),
								"logical-plan pattern label slots overflowed"));
							targetIds.reserve(candidates.size());
							labelIdsBytes.set(checkedSizeMul(targetIds.capacity(), sizeof(uint64_t),
								"logical-plan pattern label capacity overflowed"));
							for (const auto *candidate: candidates) {
								const uint64_t id = adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
									? candidate->target_node_id : candidate->source_node_id;
								if (cachedLabel(id) == targetLabelMatches.end()) targetIds.push_back(id);
							}
							detail::sortUniqueIdsGuarded(targetIds, [&] {
								if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
							});
							if (!targetIds.empty()) {
								labelBitsBytes.set(checkedSizeAdd(targetIds.size() / 8U + (targetIds.size() % 8U != 0U),
									sizeof(size_t), "logical-plan pattern label bit slots overflowed"));
								matches = state.shared.snapshot.readNodeLabelMatchesSorted(targetIds, targetLabelFilter);
								labelBitsBytes.set(matches.capacity() / 8U + (matches.capacity() % 8U != 0U));
								checkGuard(state);
								if (matches.size() != targetIds.size())
									throw std::runtime_error("logical graph backend changed pattern-expression target label cardinality");
								const size_t cacheLimit = std::min(state.shared.bounds.maximum_rows, size_t{64U});
								for (size_t index = 0U; index < targetIds.size() && targetLabelMatches.size() < cacheLimit; ++index) {
									if (targetLabelMatches.size() == targetLabelMatches.capacity()) {
										const size_t old = targetLabelMatches.capacity();
										const size_t next = old == 0U ? std::min(cacheLimit, size_t{8U}) : old + std::min(old, cacheLimit - old);
										const size_t peak = (old + next) * sizeof(LabelMatch);
										if (labelCacheBytes.wouldExceed(peak)) break; // Cache insertion is optional.
										labelCacheBytes.set(peak);
										targetLabelMatches.reserve(next);
										labelCacheBytes.set(targetLabelMatches.capacity() * sizeof(LabelMatch));
									}
									targetLabelMatches.emplace_back(targetIds[index], matches[index]);
								}
							}
						}
						size_t matchingCount = 0U;
						for (const storage::EntityRef *referencePointer: candidates) {
							const storage::EntityRef &reference = *referencePointer;
							const uint64_t targetId =
									adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
											? reference.target_node_id
											: reference.source_node_id;
							if (!targetLabel.empty()) {
								const auto cached = cachedLabel(targetId);
								const auto position = std::ranges::lower_bound(targetIds, targetId);
								const bool labelsMatch = cached != targetLabelMatches.end() ? cached->second
									: (position != targetIds.end() && *position == targetId && matches[position - targetIds.begin()]);
								if (!labelsMatch) {
									continue;
								}
							}
							candidates[matchingCount++] = referencePointer;
						}
						candidates.resize(matchingCount);
						std::vector<uint64_t>().swap(targetIds);
						std::vector<bool>().swap(matches);
						labelIdsBytes.reset();
						labelBitsBytes.reset();
						if (bindingShape == nullptr) {
							// Complete/fallback paths retain the original lazy visitor
							// contract.  In particular, EXISTS may stop after the first
							// match and must not read the rest of the page speculatively.
							for (const storage::EntityRef *referencePointer: candidates) {
								const storage::EntityRef &reference = *referencePointer;
								const storage::LogicalGraphEdge edge = readEdgeCached(state, reference.entity_id);
								if (edge.source_node_id != reference.source_node_id ||
									edge.target_node_id != reference.target_node_id ||
									(!edgeType.empty() && edge.type != edgeType)) {
									throw std::runtime_error("logical graph backend returned an adjacency reference "
															 "with mismatched Edge data");
								}
								const uint64_t targetId =
										adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
												? reference.target_node_id
												: reference.source_node_id;
								const storage::LogicalGraphNode target = readNodeCached(state, targetId);
								if (!visitor(edge, target)) {
									return false;
								}
							}
							if (!page.continuation.has_value()) {
								return true;
							}
							if (sourceEdges == state.shared.bounds.maximum_edges_per_node || page.edges.empty() ||
								page.continuation->previous_edge_id != page.edges.back().entity_id ||
								page.continuation->next_edge_id == 0U ||
								page.continuation->previous_edge_id == page.continuation->next_edge_id) {
								throw std::runtime_error("logical graph backend returned an incomplete "
														 "pattern-expression adjacency reference scan");
							}
							continuation = page.continuation;
							continue;
						}
						MaterializedByteReservation gatherBytes(&state.shared.materialized_budget);
						gatherBytes.set(checkedSizeMul(candidates.size(), 2U * sizeof(uint64_t),
							"logical-plan pattern gather slots overflowed"));
						std::vector<uint64_t> pageEdgeIds;
						pageEdgeIds.reserve(candidates.size());
						gatherBytes.set(checkedSizeMul(checkedSizeAdd(pageEdgeIds.capacity(), candidates.size(),
							"logical-plan pattern gather slots overflowed"), sizeof(uint64_t),
							"logical-plan pattern gather capacity overflowed"));
						std::vector<uint64_t> pageTargetIds;
						pageTargetIds.reserve(candidates.size());
						gatherBytes.set(checkedSizeMul(checkedSizeAdd(pageEdgeIds.capacity(), pageTargetIds.capacity(),
							"logical-plan pattern gather capacity overflowed"), sizeof(uint64_t),
							"logical-plan pattern gather capacity overflowed"));
						for (const storage::EntityRef *referencePointer: candidates) {
							pageEdgeIds.push_back(referencePointer->entity_id);
							pageTargetIds.push_back(adjacencyDirection ==
																	storage::LogicalGraphReadDirection::LGRD_OUTGOING
															? referencePointer->target_node_id
															: referencePointer->source_node_id);
						}
						detail::sortUniqueIdsGuarded(pageEdgeIds, [&] {
							if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
						});
						detail::sortUniqueIdsGuarded(pageTargetIds, [&] {
							if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
						});
						storage::EntityReadShape edgeShape =
								bindingShape == nullptr ? completeEdgeReadShape() : bindingShape->relationship;
						if (!edgeType.empty()) {
							edgeShape.include_type = true;
						}
						storage::EntityReadShape targetShape =
								bindingShape == nullptr ? completeNodeReadShape() : bindingShape->target;
						targetShape.include_labels = targetShape.include_labels || !targetLabel.empty();
						MaterializedByteReservation pageEdgeBytes(&state.shared.materialized_budget);
						std::vector<storage::LogicalGraphEdge> pageEdges =
								pageEdgeIds.empty()
										? std::vector<storage::LogicalGraphEdge>{}
										: readEdgesShapedSortedBudgeted(state, pageEdgeIds, edgeShape, &pageEdgeBytes);
						if (pageEdges.size() != pageEdgeIds.size()) {
							throw std::runtime_error(
									"logical graph backend changed pattern-expression Edge batch cardinality");
						}
						retainShapedBatchBytes(pageEdges, pageEdgeBytes, state.shared.context);
						MaterializedByteReservation pageTargetBytes(&state.shared.materialized_budget);
						std::vector<storage::LogicalGraphNode> pageTargets =
								pageTargetIds.empty() ? std::vector<storage::LogicalGraphNode>{}
													  : readNodesShapedSortedBudgeted(state, pageTargetIds, targetShape,
																					  &pageTargetBytes);
						if (pageTargets.size() != pageTargetIds.size()) {
							throw std::runtime_error(
									"logical graph backend changed pattern-expression target batch cardinality");
						}
						retainShapedBatchBytes(pageTargets, pageTargetBytes, state.shared.context);
						if (bindingShape != nullptr) {
							appendNamedProperties(pageEdges, pageEdgeIds, bindingShape->relationship_named_properties,
												  true, pageEdgeBytes);
							retainShapedBatchBytes(pageEdges, pageEdgeBytes, state.shared.context);
							appendNamedProperties(pageTargets, pageTargetIds, bindingShape->target_named_properties,
												  false, pageTargetBytes);
							retainShapedBatchBytes(pageTargets, pageTargetBytes, state.shared.context);
						}
						for (const storage::EntityRef *referencePointer: candidates) {
							const storage::EntityRef &reference = *referencePointer;
							const uint64_t targetId =
									adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
											? reference.target_node_id
											: reference.source_node_id;
							const auto edgePosition = std::ranges::lower_bound(pageEdges, reference.entity_id, {},
																			   &storage::LogicalGraphEdge::edge_id);
							const auto targetPosition = std::ranges::lower_bound(pageTargets, targetId, {},
																				 &storage::LogicalGraphNode::node_id);
							if (edgePosition == pageEdges.end() || targetPosition == pageTargets.end() ||
								edgePosition->edge_id != reference.entity_id || targetPosition->node_id != targetId) {
								throw std::runtime_error("logical graph backend lost a pattern-expression batch entry");
							}
							const storage::LogicalGraphEdge &edge = *edgePosition;
							if (edge.source_node_id != reference.source_node_id ||
								edge.target_node_id != reference.target_node_id ||
								(!edgeType.empty() && edge.type != edgeType)) {
								throw std::runtime_error("logical graph backend returned an adjacency reference with "
														 "mismatched Edge data");
							}
							validateCandidate(edge, *targetPosition);
							if (!visitor(edge, *targetPosition)) {
								return false;
							}
						}
						if (!page.continuation.has_value()) {
							return true;
						}
						if (sourceEdges == state.shared.bounds.maximum_edges_per_node || page.edges.empty() ||
							page.continuation->previous_edge_id != page.edges.back().entity_id ||
							page.continuation->next_edge_id == 0U ||
							page.continuation->previous_edge_id == page.continuation->next_edge_id) {
							throw std::runtime_error("logical graph backend returned an incomplete pattern-expression "
													 "adjacency reference scan");
						}
						continuation = page.continuation;
					}
				}
				for (;;) {
					if (sourceEdges == state.shared.bounds.maximum_edges_per_node) {
						throw std::runtime_error("logical-plan pattern expression exceeded its per-Node Edge bound");
					}
					const size_t pageRecords =
							std::min(state.shared.bounds.maximum_edges_per_node - sourceEdges,
									 lazyCandidates ? size_t{1U} : state.shared.bounds.maximum_adjacency_page_records);
					// The pattern visitor has no Edge Property predicate on this path.
					// Scan topology first, then materialize the selected full Edge batch
					// only for candidates that survive endpoint/type validation.
					MaterializedByteReservation topologyBytes(&state.shared.materialized_budget);
					const auto page = readTopologyPageBudgeted(state, source.node_id,
						adjacencyDirection, continuation, pageRecords, topologyBytes);
					if (page.edges.size() > pageRecords || (page.edges.empty() && page.continuation.has_value())) {
						throw std::runtime_error(
								"logical graph backend returned an invalid pattern-expression adjacency page");
					}
					if (continuation.has_value() &&
						(page.edges.empty() || page.edges.front().edge_id != continuation->next_edge_id)) {
						throw std::runtime_error(
								"logical graph backend returned a discontinuous pattern-expression adjacency page");
					}
					// A complete adjacency page still exposes endpoint IDs independently of
					// the target Node payload. Authenticate the distinct endpoint set once
					// before the per-match materialization loop so parallel edges do not
					// repeat an Entity Directory descent. Readers without the optional batch
					// probe retain the complete-read fallback below.
					MaterializedByteReservation endpointIdsBytes(&state.shared.materialized_budget);
					endpointIdsBytes.set(checkedSizeMul(page.edges.size(), sizeof(uint64_t),
						"logical-plan pattern topology endpoint slots overflowed"));
					std::vector<uint64_t> pageTargetIds;
					pageTargetIds.reserve(page.edges.size());
					endpointIdsBytes.set(checkedSizeMul(pageTargetIds.capacity(), sizeof(uint64_t),
						"logical-plan pattern topology endpoint capacity overflowed"));
					for (const storage::LogicalGraphEdgeTopology &edge: page.edges) {
						checkGuard(state);
						validateEdge(edge);
						const bool selfLoopDuplicate =
								direction == expressions::PatternDirection::PAT_BOTH &&
								adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_INCOMING &&
								edge.source_node_id == source.node_id;
						if (!selfLoopDuplicate && (edgeType.empty() || edge.type == edgeType)) {
							pageTargetIds.push_back(adjacencyDirection ==
																	storage::LogicalGraphReadDirection::LGRD_OUTGOING
															? edge.target_node_id
															: edge.source_node_id);
						}
					}
					detail::sortUniqueIdsGuarded(pageTargetIds, [&] {
						if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
					});
					MaterializedByteReservation presenceBytes(&state.shared.materialized_budget);
					if (state.shared.snapshot.supportsNodeExistenceProbes()) {
						presenceBytes.set(checkedSizeAdd(pageTargetIds.size() / 8U + (pageTargetIds.size() % 8U != 0U),
							sizeof(size_t), "logical-plan pattern topology presence slots overflowed"));
					}
					const std::vector<bool> pageTargetPresent =
							state.shared.snapshot.supportsNodeExistenceProbes()
									? state.shared.snapshot.containsNodesSorted(pageTargetIds)
									: std::vector<bool>{};
					presenceBytes.set(pageTargetPresent.capacity() / 8U + (pageTargetPresent.capacity() % 8U != 0U));
					checkGuard(state);
					if (state.shared.snapshot.supportsNodeExistenceProbes() &&
						pageTargetPresent.size() != pageTargetIds.size()) {
						throw std::runtime_error(
								"logical graph backend changed pattern-expression endpoint cardinality");
					}
					if (state.shared.snapshot.supportsNodeExistenceProbes() &&
						std::ranges::any_of(pageTargetPresent, [](bool present) { return !present; })) {
						throw std::runtime_error("logical graph backend returned a pattern-expression Edge "
												 "with a missing endpoint");
					}
					if (!targetLabel.empty() && !pageTargetIds.empty()) {
						MaterializedByteReservation labelBytes(&state.shared.materialized_budget);
						labelBytes.set(packedBooleanBytes(pageTargetIds.size()));
						std::vector<bool> labels;
						if (state.shared.snapshot.supportsNodeExistenceProbes()) {
							labels = state.shared.snapshot.readNodeLabelMatchesSorted(pageTargetIds, targetLabelFilter);
						} else {
							MaterializedByteReservation nodesBytes(&state.shared.materialized_budget);
							const auto nodes = readNodesShapedSortedBudgeted(
									state, pageTargetIds, {.include_labels = true}, &nodesBytes);
							if (nodes.size() != pageTargetIds.size()) {
								throw std::runtime_error("logical graph backend changed pattern-expression target batch cardinality");
							}
							labels.reserve(nodes.size());
							for (size_t index = 0U; index < nodes.size(); ++index) {
								if (nodes[index].node_id != pageTargetIds[index]) {
									throw std::runtime_error("logical graph backend returned mismatched pattern-expression target identity");
								}
								labels.push_back(std::ranges::find(nodes[index].labels, targetLabel) != nodes[index].labels.end());
							}
						}
						labelBytes.set(packedBooleanBytes(labels.capacity()));
						checkGuard(state);
						if (labels.size() != pageTargetIds.size()) {
							throw std::runtime_error("logical graph backend changed pattern-expression target label cardinality");
						}
						size_t accepted = 0U;
						for (size_t index = 0U; index < labels.size(); ++index) {
							if (labels[index]) pageTargetIds[accepted++] = pageTargetIds[index];
						}
						pageTargetIds.resize(accepted);
					}
					MaterializedByteReservation edgeIdsBytes(&state.shared.materialized_budget);
					edgeIdsBytes.set(checkedSizeMul(page.edges.size(), sizeof(uint64_t),
						"logical-plan pattern topology Edge slots overflowed"));
					std::vector<uint64_t> pageEdgeIds;
					pageEdgeIds.reserve(page.edges.size());
					edgeIdsBytes.set(checkedSizeMul(pageEdgeIds.capacity(), sizeof(uint64_t),
						"logical-plan pattern topology Edge capacity overflowed"));
					for (const storage::LogicalGraphEdgeTopology &candidate: page.edges) {
						// Do not decode complete Edge payloads for candidates that are
						// guaranteed to be discarded by direction/type semantics.  The
						// topology page remains the validation source; only surviving IDs
						// enter the bounded shaped batch below.
						const bool selfLoopDuplicate =
								direction == expressions::PatternDirection::PAT_BOTH &&
								adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_INCOMING &&
								candidate.source_node_id == source.node_id;
						const uint64_t targetId = adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
								? candidate.target_node_id : candidate.source_node_id;
						if (!selfLoopDuplicate && (edgeType.empty() || candidate.type == edgeType) &&
							(targetLabel.empty() || std::ranges::binary_search(pageTargetIds, targetId))) {
							pageEdgeIds.push_back(candidate.edge_id);
						}
					}
					detail::sortUniqueIdsGuarded(pageEdgeIds, [&] {
						if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
					});
					storage::EntityReadShape edgeShape =
							bindingShape == nullptr ? completeEdgeReadShape() : bindingShape->relationship;
					if (!edgeType.empty()) {
						edgeShape.include_type = true;
					}
					storage::EntityReadShape targetShape =
							bindingShape == nullptr ? completeNodeReadShape() : bindingShape->target;
					targetShape.include_labels = targetShape.include_labels || !targetLabel.empty();
					MaterializedByteReservation pageEdgeBytes(&state.shared.materialized_budget);
					std::vector<storage::LogicalGraphEdge> pageEdgeRecords =
							pageEdgeIds.empty()
									? std::vector<storage::LogicalGraphEdge>{}
									: readEdgesShapedSortedBudgeted(state, pageEdgeIds, edgeShape, &pageEdgeBytes);
					if (pageEdgeRecords.size() != pageEdgeIds.size()) {
						throw std::runtime_error(
								"logical graph backend changed pattern-expression Edge batch cardinality");
					}
					retainShapedBatchBytes(pageEdgeRecords, pageEdgeBytes, state.shared.context);
					MaterializedByteReservation pageTargetBytes(&state.shared.materialized_budget);
					std::vector<storage::LogicalGraphNode> pageTargetRecords =
							pageTargetIds.empty() ? std::vector<storage::LogicalGraphNode>{}
												  : readNodesShapedSortedBudgeted(state, pageTargetIds, targetShape,
																				  &pageTargetBytes);
					if (pageTargetRecords.size() != pageTargetIds.size()) {
						throw std::runtime_error(
								"logical graph backend changed pattern-expression target batch cardinality");
					}
					retainShapedBatchBytes(pageTargetRecords, pageTargetBytes, state.shared.context);
					if (bindingShape != nullptr) {
						appendNamedProperties(pageEdgeRecords, pageEdgeIds, bindingShape->relationship_named_properties,
											  true, pageEdgeBytes);
						retainShapedBatchBytes(pageEdgeRecords, pageEdgeBytes, state.shared.context);
						appendNamedProperties(pageTargetRecords, pageTargetIds, bindingShape->target_named_properties,
											  false, pageTargetBytes);
						retainShapedBatchBytes(pageTargetRecords, pageTargetBytes, state.shared.context);
					}
					for (const storage::LogicalGraphEdgeTopology &edge: page.edges) {
						checkGuard(state);
						validateEdge(edge);
						validateAdjacentEdgeOrder(edge.edge_id, previousEdgeId, "pattern-expression");
						const bool endpointMatches =
								adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
										? edge.source_node_id == source.node_id
										: edge.target_node_id == source.node_id;
						if (!endpointMatches) {
							throw std::runtime_error(
									"logical graph backend returned an invalid pattern-expression Edge");
						}
						incrementBounded(sourceEdges,
										 "logical-plan pattern-expression per-Node Edge counter overflowed");
						incrementBounded(state.task.stats.edges_scanned,
										 "logical-plan pattern-expression Edge counter overflowed");
						// A correct global adjacency index returns a self-loop in both directions.
						// OUT is visited first, so omit only that duplicate without retaining a
						// degree-sized Edge-ID set.
						if (direction == expressions::PatternDirection::PAT_BOTH &&
							adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_INCOMING &&
							edge.source_node_id == source.node_id) {
							continue;
						}
						if (!edgeType.empty() && edge.type != edgeType) {
							continue;
						}
						const uint64_t targetId =
								adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
										? edge.target_node_id
										: edge.source_node_id;
						if (!targetLabel.empty() && !std::ranges::binary_search(pageTargetIds, targetId)) {
							continue;
						}
						const auto edgePosition = std::ranges::lower_bound(pageEdgeIds, edge.edge_id);
						if (edgePosition == pageEdgeIds.end() || *edgePosition != edge.edge_id) {
							throw std::runtime_error(
									"logical graph backend lost a pattern-expression Edge batch entry");
						}
						const storage::LogicalGraphEdge &materializedEdge =
								pageEdgeRecords[static_cast<size_t>(edgePosition - pageEdgeIds.begin())];
						if (materializedEdge.edge_id != edge.edge_id ||
							materializedEdge.source_node_id != edge.source_node_id ||
							materializedEdge.target_node_id != edge.target_node_id ||
							(!edgeType.empty() && materializedEdge.type != edgeType)) {
							throw std::runtime_error(
									"logical graph backend returned an adjacency Edge with mismatched batch data");
						}
						const auto targetPosition = std::ranges::lower_bound(pageTargetRecords, targetId, {},
																			 &storage::LogicalGraphNode::node_id);
						if (targetPosition == pageTargetRecords.end() || targetPosition->node_id != targetId) {
							throw std::runtime_error(
									"logical graph backend lost a pattern-expression target batch entry");
						}
						if (!targetLabel.empty() &&
							std::ranges::find(targetPosition->labels, targetLabel) == targetPosition->labels.end()) {
							continue;
						}
						validateCandidate(materializedEdge, *targetPosition);
						if (!visitor(materializedEdge, *targetPosition)) {
							return false;
						}
					}
					if (!page.continuation.has_value()) {
						return true;
					}
					if (sourceEdges == state.shared.bounds.maximum_edges_per_node ||
						page.continuation->previous_edge_id != page.edges.back().edge_id ||
						page.continuation->next_edge_id == 0U ||
						page.continuation->previous_edge_id == page.continuation->next_edge_id) {
						throw std::runtime_error(
								"logical graph backend returned an incomplete pattern-expression adjacency scan");
					}
					continuation = page.continuation;
				}
			};
			if ((direction == expressions::PatternDirection::PAT_OUTGOING ||
				 direction == expressions::PatternDirection::PAT_BOTH) &&
				!visitDirection(storage::LogicalGraphReadDirection::LGRD_OUTGOING)) {
				return false;
			}
			if (direction == expressions::PatternDirection::PAT_INCOMING ||
				direction == expressions::PatternDirection::PAT_BOTH) {
				return visitDirection(storage::LogicalGraphReadDirection::LGRD_INCOMING);
			}
			return true;
		}

