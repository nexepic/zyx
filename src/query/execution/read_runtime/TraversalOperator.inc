				case logical::LogicalOpType::LOP_TRAVERSAL: {
                    debug::PerfTrace::addValue("logical_plan.consumer_adapter.expand.calls", 1);
					const auto &traversal = dynamic_cast<const logical::LogicalTraversal &>(op);
					const std::string &direction = traversal.getDirection();
					if (direction != "out" && direction != "in" && direction != "both") {
						throw std::invalid_argument("logical-plan Traversal has an unknown direction");
					}
					if (const std::optional<bool> direct = tryVisitDirectTraversal(traversal, state, visitor);
						direct.has_value()) {
						return *direct;
					}
					return visitOperator(requireOnlyChild(traversal), state, [&](LogicalPlanReadRow row) {
						const LogicalPlanReadValue &sourceBinding = requireBinding(row, traversal.getSourceVar());
						// The child scan has already validated source predicates. Traversal only
						// needs the stable identity, so retain deferred native references.
						const uint64_t sourceNodeId = nodeBindingId(sourceBinding, state);
						size_t sourceEdges = 0U;
						const bool topologyOnly = traversal.getEdgeProperties().empty();
						std::optional<uint64_t> relationshipTypeToken;
						if (!traversal.getEdgeType().empty() &&
							state.shared.snapshot.supportsNativeRelationshipTypeTokens(traversal.getEdgeType())) {
							relationshipTypeToken = state.shared.snapshot.findRelationshipTypeToken(traversal.getEdgeType());
							if (!relationshipTypeToken.has_value()) {
								return true;
							}
						}
						// Reference pages carry authenticated endpoints and type tokens. They
						// can drive selected Edge-property predicates even when the pattern
						// omits an Edge binding; no complete Edge object is required.
						const bool useReferencePages =
								state.shared.snapshot.supportsNativeAdjacentEdgeRefScans() &&
								(traversal.getEdgeType().empty() || relationshipTypeToken.has_value());
						std::vector<std::pair<std::string, PropertyValue>> edgePredicates;
						edgePredicates.reserve(traversal.getEdgeProperties().size());
						for (const auto &[key, expected]: traversal.getEdgeProperties()) {
							if (key.empty()) {
								throw std::invalid_argument("logical-plan Edge Property predicate has an empty key");
							}
							edgePredicates.emplace_back(key, expected);
						}
						std::ranges::sort(edgePredicates, {}, &std::pair<std::string, PropertyValue>::first);
						std::vector<std::string_view> edgePropertyKeys;
						edgePropertyKeys.reserve(edgePredicates.size());
						for (const auto &[key, expected]: edgePredicates) {
							static_cast<void>(expected);
							edgePropertyKeys.push_back(key);
						}
						const auto visitDirection = [&](storage::LogicalGraphReadDirection adjacencyDirection) {
							std::optional<storage::LogicalGraphAdjacencyContinuation> continuation;
							uint64_t previousEdgeId = 0U;
							if (useReferencePages) {
								struct TargetCandidate {
										size_t reference_index = 0U;
										uint64_t target_id = 0U;
										size_t property_ordinal = 0U;
									};
								MaterializedByteReservation pendingReferenceBytes(&state.shared.materialized_budget);
								const size_t requestedBatchCapacity =
										edgePredicates.empty() && traversal.getTargetProperties().empty()
												? size_t{1U}
												: std::max<size_t>(
														  1U, std::min({DEFAULT_PROPERTY_BATCH_ROWS,
																		state.shared.bounds.maximum_adjacency_page_records,
																		state.shared.bounds.maximum_edges_per_node}));
								// Include the live source page, retained references and target scratch.
								size_t bytesPerReference = 2U * sizeof(storage::EntityRef) + sizeof(TargetCandidate) +
										sizeof(uint64_t) + 2U * sizeof(size_t);
								if (!edgePredicates.empty()) {
									bytesPerReference = checkedSizeAdd(
											bytesPerReference,
											checkedSizeAdd(sizeof(uint64_t) + sizeof(size_t),
														   checkedSizeMul(edgePredicates.size(),
																		  2U * (sizeof(PropertyValue) +
																				sizeof(std::vector<PropertyValue>)),
																		  "logical-plan adjacent reference batch "
																		  "estimate overflowed"),
														   "logical-plan adjacent reference batch estimate overflowed"),
											"logical-plan adjacent reference batch estimate overflowed");
								}
								if (!traversal.getTargetProperties().empty()) {
									bytesPerReference = checkedSizeAdd(
											bytesPerReference,
											checkedSizeAdd(
													sizeof(uint64_t),
													checkedSizeMul(
															traversal.getTargetProperties().size(),
															sizeof(PropertyValue) + sizeof(std::vector<PropertyValue>),
															"logical-plan adjacent target batch estimate overflowed"),
													"logical-plan adjacent target batch estimate overflowed"),
											"logical-plan adjacent target batch estimate overflowed");
								}
								const size_t budgetRows = state.shared.materialized_budget.available() / bytesPerReference;
								const size_t batchCapacity =
										std::min(requestedBatchCapacity, std::max<size_t>(1U, budgetRows));
								pendingReferenceBytes.set(
										checkedSizeMul(batchCapacity, sizeof(storage::EntityRef),
													   "logical-plan adjacent reference buffer size overflowed"));
								std::vector<storage::EntityRef> pendingReferences;
								pendingReferences.reserve(batchCapacity);
								pendingReferenceBytes.set(checkedSizeMul(pendingReferences.capacity(), sizeof(storage::EntityRef),
									"logical-plan adjacent reference buffer capacity overflowed"));
								const auto flushReferences = [&]() {
									if (pendingReferences.empty()) {
										return true;
									}
									if (!edgePredicates.empty() && state.shared.snapshot.supportsNodeExistenceProbes() &&
											!std::ranges::any_of(edgePredicates, [](const auto &predicate) { return isNull(predicate.second); })) {
										detail::BudgetMemoryResource endpointMemory(state.shared.materialized_budget);
										std::pmr::vector<uint64_t> endpointIds{&endpointMemory};
										endpointIds.reserve(pendingReferences.size());
										for (const auto &reference: pendingReferences) {
											endpointIds.push_back(adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
												? reference.target_node_id : reference.source_node_id);
										}
										detail::sortUniqueIdsGuarded(endpointIds, [&] { checkGuardNow(state); });
										MaterializedByteReservation presenceBytes(&state.shared.materialized_budget);
										presenceBytes.set(packedBooleanBytes(endpointIds.size()));
										const auto present = state.shared.snapshot.containsNodesSorted(endpointIds);
										presenceBytes.set(packedBooleanBytes(present.capacity()));
										if (present.size() != endpointIds.size())
											throw std::runtime_error("logical graph backend changed adjacent endpoint cardinality");
										if (std::ranges::any_of(present, [](bool value) { return !value; }))
											throw std::runtime_error("logical graph backend returned an adjacent Edge with a missing endpoint");
									}
									detail::BudgetMemoryResource selectionMemory(state.shared.materialized_budget);
									SelectionVector selection(pendingReferences.size(), &selectionMemory);
									MaterializedByteReservation selectedPropertyBytes(&state.shared.materialized_budget);
									std::vector<std::vector<PropertyValue>> selectedProperties;
									if (!edgePredicates.empty()) {
										MaterializedByteReservation orderingBytes(&state.shared.materialized_budget);
										orderingBytes.set(checkedSizeMul(pendingReferences.size(), sizeof(size_t) + sizeof(uint64_t),
											"logical-plan adjacent Edge ordering size overflowed"));
										std::vector<size_t> sortedOrdinals(pendingReferences.size());
										std::iota(sortedOrdinals.begin(), sortedOrdinals.end(), size_t{0U});
										std::ranges::sort(sortedOrdinals, [&](const size_t left, const size_t right) {
											return pendingReferences[left].entity_id <
												   pendingReferences[right].entity_id;
										});
										std::vector<uint64_t> edgeIds;
										edgeIds.reserve(pendingReferences.size());
										orderingBytes.set(checkedSizeAdd(
											checkedSizeMul(sortedOrdinals.capacity(), sizeof(size_t), "logical-plan adjacent Edge ordinal capacity overflowed"),
											checkedSizeMul(edgeIds.capacity(), sizeof(uint64_t), "logical-plan adjacent Edge ID capacity overflowed"),
											"logical-plan adjacent Edge ordering capacity overflowed"));
										for (const size_t ordinal: sortedOrdinals) {
											const uint64_t edgeId = pendingReferences[ordinal].entity_id;
											if (!edgeIds.empty() && edgeIds.back() == edgeId) {
												throw std::runtime_error("logical graph backend returned duplicate "
																		 "adjacent Edge references");
											}
											edgeIds.push_back(edgeId);
										}
										const bool impossiblePredicate =
												std::ranges::any_of(edgePredicates, [](const auto &predicate) {
													return isNull(predicate.second);
												});
										if (impossiblePredicate) {
											selection.rejectAll();
										} else {
											size_t keyBytes = 0U;
											for (const std::string_view key: edgePropertyKeys) {
												keyBytes = checkedSizeAdd(
														keyBytes, key.size(),
														"logical-plan adjacent Edge Property key size overflowed");
											}
											const size_t columnFrames = checkedSizeAdd(
													checkedSizeMul(edgePropertyKeys.size(),
																   sizeof(std::vector<PropertyValue>),
																   "logical-plan adjacent Edge Property frame size "
																   "overflowed"),
													checkedSizeMul(checkedSizeMul(edgePropertyKeys.size(),
																				  edgeIds.size(),
																				  "logical-plan adjacent Edge Property "
																				  "frame size overflowed"),
																   sizeof(PropertyValue),
																   "logical-plan adjacent Edge Property frame size "
																   "overflowed"),
													"logical-plan adjacent Edge Property frame size overflowed");
											const size_t reorderFrames = checkedSizeAdd(
													checkedSizeMul(
															edgePropertyKeys.size(), sizeof(std::vector<PropertyValue>),
															"logical-plan adjacent Edge reorder frame size overflowed"),
													checkedSizeMul(
															checkedSizeMul(edgePropertyKeys.size(),
																		   pendingReferences.size(),
																		   "logical-plan adjacent Edge reorder frame "
																		   "size overflowed"),
															sizeof(PropertyValue),
															"logical-plan adjacent Edge reorder frame size overflowed"),
													"logical-plan adjacent Edge reorder frame size overflowed");
											const size_t fixedBytes = checkedSizeAdd(columnFrames, reorderFrames,
												"logical-plan adjacent Edge Property batch size overflowed");
											const size_t requestedBytes = checkedSizeAdd(
													fixedBytes, keyBytes,
													"logical-plan adjacent Edge Property batch size overflowed");
											if (selectedPropertyBytes.wouldExceed(requestedBytes)) {
												throw std::runtime_error("logical-plan adjacent Edge Property batch "
																		 "exceeded its byte bound");
											}
											selectedPropertyBytes.set(requestedBytes);
											const std::function<void(size_t)> admission = [&](size_t bytes) {
												checkGuard(state);
												const size_t next = checkedSizeAdd(
														selectedPropertyBytes.bytes(), bytes,
														"logical-plan adjacent Edge Property batch size overflowed");
												if (selectedPropertyBytes.wouldExceed(next)) {
													throw std::runtime_error("logical-plan adjacent Edge Property "
																			 "batch exceeded its byte bound");
												}
												selectedPropertyBytes.set(next);
											};
											auto sortedColumns =
													state.shared.snapshot.supportsPropertyReadAdmission()
															? state.shared.snapshot.readEdgePropertyColumnsSortedWithAdmission(
																	  edgeIds, edgePropertyKeys, admission)
															: state.shared.snapshot.readEdgePropertyColumnsSorted(
																	  edgeIds, edgePropertyKeys);
											const size_t selectedEdgeBytes = propertyColumnBytes(
													sortedColumns, edgePropertyKeys,
													"logical-plan adjacent Edge Property batch size overflowed", state.shared.context);
											const size_t retainedEdgeBytes = checkedSizeAdd(
													fixedBytes - columnFrames, selectedEdgeBytes,
													"logical-plan adjacent Edge Property batch size overflowed");
											if (selectedPropertyBytes.wouldExceed(retainedEdgeBytes)) {
												throw std::runtime_error("logical-plan adjacent Edge Property batch "
																		 "exceeded its byte bound");
											}
											selectedPropertyBytes.set(retainedEdgeBytes);
											if (sortedColumns.size() != edgePredicates.size()) {
												throw std::runtime_error("logical graph backend changed adjacent Edge "
																		 "Property column cardinality");
											}
											selectedProperties.assign(edgePredicates.size(), {});
											for (size_t propertyIndex = 0U; propertyIndex < sortedColumns.size();
												 ++propertyIndex) {
												if (sortedColumns[propertyIndex].size() != pendingReferences.size()) {
													throw std::runtime_error("logical graph backend changed adjacent "
																			 "Edge Property result cardinality");
												}
												std::vector<PropertyValue> values(pendingReferences.size());
												for (size_t sortedIndex = 0U; sortedIndex < sortedOrdinals.size();
													 ++sortedIndex) {
													checkGuard(state);
													values[sortedOrdinals[sortedIndex]] =
															std::move(sortedColumns[propertyIndex][sortedIndex]);
												}
												selection.forEachSelected([&](const size_t index) {
													if (isNull(values[index]) ||
														values[index] != edgePredicates[propertyIndex].second) {
														selection.reject(index);
													}
												});
												selectedProperties[propertyIndex] = std::move(values);
											}
										}
									}
									// Source columns and reorder scratch have left scope; retain only the moved columns.
									selectedPropertyBytes.set(propertyColumnBytes(selectedProperties, {},
										"logical-plan retained adjacent Property size overflowed", state.shared.context));
									// Resolve target predicates in one bounded column batch.  The
									// previous per-reference complete read made a high-degree
									// anchored traversal pay one Entity/Property lookup per Edge.
									detail::BudgetMemoryResource targetScratchMemory(state.shared.materialized_budget);
									std::pmr::vector<TargetCandidate> targets{&targetScratchMemory};
									targets.reserve(pendingReferences.size());
									for (size_t index = 0U; index < pendingReferences.size(); ++index) {
										if (!selection.includes(index)) {
											continue;
										}
										const storage::EntityRef &reference = pendingReferences[index];
										const uint64_t targetId =
												adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
														? reference.target_node_id
														: reference.source_node_id;
										targets.push_back({index, targetId, 0U});
									}
									if (targets.empty()) {
										pendingReferences.clear();
										return true;
									}
									std::pmr::vector<uint64_t> targetIds{&targetScratchMemory};
									targetIds.reserve(targets.size());
									for (const TargetCandidate &target: targets) {
										// Deduplicate after sorting.  A linear search here turns a
										// high-degree adjacency batch into O(n^2) work before the
										// column reader even starts; the batch is already bounded and
										// sorting gives deterministic I/O order.
										targetIds.push_back(target.target_id);
									}
									detail::sortUniqueIdsGuarded(targetIds, [&] {
										if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
									});
									MaterializedByteReservation targetLabelsBytes(&state.shared.materialized_budget);
									std::vector<bool> targetLabelsMatch;
									if (!traversal.getTargetLabels().empty()) {
										targetLabelsBytes.set(packedBooleanBytes(targetIds.size()));
										targetLabelsMatch = state.shared.snapshot.readNodeLabelMatchesSorted(
												targetIds, traversal.getTargetLabels());
										targetLabelsBytes.set(packedBooleanBytes(targetLabelsMatch.capacity()));
										if (targetLabelsMatch.size() != targetIds.size()) {
											throw std::runtime_error(
													"logical graph backend changed target label-match cardinality");
										}
									}
									// The immutable plan owns literal predicates throughout execution.
									// Retain validation without copying their potentially large payloads
									// or property names on every adjacency batch.
									const auto &targetPredicates = traversal.getTargetProperties();
									validateTraversalTargetProperties(traversal);
									if (traversal.getTargetLabels().empty() && targetPredicates.empty()) {
										if (state.shared.snapshot.supportsNodeExistenceProbes() && edgePredicates.empty()) {
											MaterializedByteReservation targetPresenceBytes(&state.shared.materialized_budget);
											targetPresenceBytes.set(packedBooleanBytes(targetIds.size()));
											const std::vector<bool> present =
													state.shared.snapshot.containsNodesSorted(targetIds);
											targetPresenceBytes.set(packedBooleanBytes(present.capacity()));
											if (present.size() != targetIds.size()) {
												throw std::runtime_error(
														"logical graph backend changed adjacent endpoint cardinality");
											}
											if (std::ranges::any_of(present, [](const bool value) { return !value; })) {
												throw std::runtime_error("logical graph backend returned an adjacent "
																		 "Edge with a missing endpoint");
											}
										} else if (!state.shared.snapshot.supportsNodeExistenceProbes()) {
											for (const uint64_t targetId: targetIds) {
												validateNodeIdentity(state, targetId);
											}
										}
									}
									std::pmr::vector<std::string_view> targetKeyViews{&targetScratchMemory};
									targetKeyViews.reserve(targetPredicates.size());
									for (const auto &[key, expected]: targetPredicates) targetKeyViews.push_back(key);
									MaterializedByteReservation targetPropertyBytes(&state.shared.materialized_budget);
									size_t targetKeyBytes = 0U;
									for (const std::string_view key: targetKeyViews) {
										targetKeyBytes = checkedSizeAdd(
												targetKeyBytes, key.size(),
												"logical-plan adjacent target Property key size overflowed");
									}
									const size_t targetFrames = checkedSizeAdd(
											checkedSizeMul(
													targetKeyViews.size(), sizeof(std::vector<PropertyValue>),
													"logical-plan adjacent target Property frame size overflowed"),
											checkedSizeMul(
													checkedSizeMul(targetKeyViews.size(), targetIds.size(),
																   "logical-plan adjacent target Property frame size "
																   "overflowed"),
													sizeof(PropertyValue),
													"logical-plan adjacent target Property frame size overflowed"),
											"logical-plan adjacent target Property frame size overflowed");
									const size_t targetFixedBytes = targetFrames; // IDs are charged by targetScratchMemory.
									targetPropertyBytes.set(checkedSizeAdd(
											targetFixedBytes, targetKeyBytes,
											"logical-plan adjacent target Property batch size overflowed"));
									const std::function<void(size_t)> targetAdmission = [&](size_t bytes) {
										checkGuard(state);
										const size_t next = checkedSizeAdd(
												targetPropertyBytes.bytes(), bytes,
												"logical-plan adjacent target Property batch size overflowed");
										if (targetPropertyBytes.wouldExceed(next)) {
											throw std::runtime_error("logical-plan adjacent target Property batch "
																	 "exceeded its byte bound");
										}
										targetPropertyBytes.set(next);
									};
									auto targetColumns =
											targetPredicates.empty() ? std::vector<std::vector<PropertyValue>>{}
											: state.shared.snapshot.supportsPropertyReadAdmission()
													? state.shared.snapshot.readNodePropertyColumnsSortedWithAdmission(
															  targetIds, targetKeyViews, targetAdmission)
													: state.shared.snapshot.readNodePropertyColumnsSorted(targetIds,
																								   targetKeyViews);
									const size_t targetPayloadBytes = propertyColumnBytes(
											targetColumns, targetKeyViews,
											"logical-plan adjacent target Property batch size overflowed", state.shared.context);
									const size_t retainedTargetBytes = checkedSizeAdd(
											targetFixedBytes - targetFrames, targetPayloadBytes,
											"logical-plan adjacent target Property batch size overflowed");
									if (targetPropertyBytes.wouldExceed(retainedTargetBytes)) {
										throw std::runtime_error(
												"logical-plan adjacent target Property batch exceeded its byte bound");
									}
									targetPropertyBytes.set(retainedTargetBytes);
									if (targetColumns.size() != targetPredicates.size()) {
										throw std::runtime_error("logical graph backend changed target Node Property "
																 "column cardinality");
									}
									for (const std::vector<PropertyValue> &column: targetColumns) {
										if (column.size() != targetIds.size()) {
											throw std::runtime_error("logical graph backend changed target Node "
																	 "Property result cardinality");
										}
									}
									for (TargetCandidate &target: targets) {
										target.property_ordinal = static_cast<size_t>(
												std::ranges::lower_bound(targetIds, target.target_id) -
												targetIds.begin());
									}
									for (const TargetCandidate &candidate: targets) {
										checkGuard(state);
										const size_t targetOrdinal = candidate.property_ordinal;
										if (!targetLabelsMatch.empty() && !targetLabelsMatch[targetOrdinal]) {
											continue;
										}
										bool targetMatches = true;
										for (size_t propertyIndex = 0U; propertyIndex < targetPredicates.size();
											 ++propertyIndex) {
											if (isNull(targetColumns[propertyIndex][targetOrdinal]) ||
												targetColumns[propertyIndex][targetOrdinal] !=
														targetPredicates[propertyIndex].second) {
												targetMatches = false;
												break;
											}
										}
										if (!targetMatches) {
											continue;
										}
										const storage::EntityRef &reference =
												pendingReferences[candidate.reference_index];
										MaterializedByteReservation expandedPropertyBytes(&state.shared.materialized_budget);
										expandedPropertyBytes.set(logicalReadRowBytes(row, state.shared.context));
										LogicalPlanReadRow expanded = detail::copyLogicalReadRowGuarded(row, [&] { checkGuardNow(state); });
										expandedPropertyBytes.set(logicalReadRowBytes(expanded, state.shared.context));
										if (!traversal.getEdgeVar().empty() &&
											!bindValue(expanded, traversal.getEdgeVar(), reference)) {
											continue;
										}
										if (!traversal.getEdgeVar().empty()) {
											for (size_t propertyIndex = 0U; propertyIndex < edgePredicates.size();
												 ++propertyIndex) {
												expanded.property_cache[traversal.getEdgeVar()].insert_or_assign(
														edgePredicates[propertyIndex].first,
														copyReservedPropertyValue(selectedProperties[propertyIndex][candidate.reference_index],
															state, expandedPropertyBytes));
											}
										}
										if (!traversal.getTargetVar().empty()) {
											const LogicalPlanReadValue targetBinding = LogicalPlanReadValue(
													storage::EntityRef{candidate.target_id, false, 0U, 0U, 0U});
											if (!bindValue(expanded, traversal.getTargetVar(), targetBinding)) {
												continue;
											}
											for (size_t propertyIndex = 0U; propertyIndex < targetPredicates.size();
												 ++propertyIndex) {
												expanded.property_cache[traversal.getTargetVar()].insert_or_assign(
														targetPredicates[propertyIndex].first,
														copyReservedPropertyValue(targetColumns[propertyIndex][targetOrdinal],
															state, expandedPropertyBytes));
											}
										}
										if (!visitor(std::move(expanded))) {
											pendingReferences.clear();
											return false;
										}
									}
									pendingReferences.clear();
									return true;
								};
								for (;;) {
									const size_t remaining = state.shared.bounds.maximum_edges_per_node - sourceEdges;
									if (remaining == 0U) {
										return true;
									}
									const size_t pageRecords =
											std::min(remaining, state.shared.bounds.maximum_adjacency_page_records);
									MaterializedByteReservation referenceBytes(&state.shared.materialized_budget);
									const auto page = readReferencePageBudgeted(state, sourceNodeId, adjacencyDirection, continuation, pageRecords, referenceBytes);
									if (page.edges.size() > pageRecords ||
										(page.edges.empty() && page.continuation.has_value()) ||
										(continuation.has_value() &&
										 (page.edges.empty() ||
										  page.edges.front().entity_id != continuation->next_edge_id))) {
										throw std::runtime_error(
												"logical graph backend returned an invalid adjacency reference page");
									}
									for (const storage::EntityRef &reference: page.edges) {
										checkGuard(state);
										validateEdgeRef(
												reference,
												"logical graph backend returned an invalid adjacent Edge reference");
										if (reference.entity_id <= previousEdgeId) {
											throw std::runtime_error("logical graph backend returned an invalid "
																	 "adjacent Edge reference");
										}
										previousEdgeId = reference.entity_id;
										const bool endpointMatches =
												adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
														? reference.source_node_id == sourceNodeId
														: reference.target_node_id == sourceNodeId;
										if (!endpointMatches) {
											throw std::runtime_error("logical graph backend returned an invalid "
																	 "adjacent Edge reference");
										}
										incrementBounded(sourceEdges,
														 "logical-plan Traversal per-Node Edge counter overflowed");
										incrementBounded(state.task.stats.edges_scanned,
														 "logical-plan read runtime Edge counter overflowed");
										if (direction == "both" &&
											adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_INCOMING &&
											reference.source_node_id == sourceNodeId) {
											continue;
										}
										if (relationshipTypeToken.has_value() &&
											reference.type_token != *relationshipTypeToken) {
											continue;
										}
										pendingReferences.push_back(reference);
										if (pendingReferences.size() >= batchCapacity && !flushReferences()) {
											return false;
										}
									}
									if (!flushReferences()) {
										return false;
									}
									if (!page.continuation.has_value()) {
										return true;
									}
									if (page.edges.empty() ||
										page.continuation->previous_edge_id != page.edges.back().entity_id ||
										page.continuation->next_edge_id == 0U ||
										page.continuation->previous_edge_id == page.continuation->next_edge_id) {
										throw std::runtime_error("logical graph backend returned an invalid adjacency "
																 "reference continuation");
									}
									continuation = page.continuation;
								}
							}
							for (;;) {
								const size_t remaining = state.shared.bounds.maximum_edges_per_node - sourceEdges;
								if (remaining == 0U) { // ZYX_COV_EXCL_LINE: a full adjacency bound returns immediately
													   // after the preceding page.
									return true;
								}
								const size_t pageRecords =
										std::min(remaining, state.shared.bounds.maximum_adjacency_page_records);
								MaterializedByteReservation topologyBytes(&state.shared.materialized_budget);
								MaterializedByteReservation fullPageBytes(&state.shared.materialized_budget);
								std::optional<storage::LogicalGraphAdjacencyPage> fullPage;
								std::optional<storage::LogicalGraphAdjacencyTopologyPage> topologyPage;
								// Relationship types and endpoints are topology data. When a
								// backend exposes native topology pages, authenticate that
								// topology before decoding the complete Edge payload. This
								// keeps the fallback traversal on the same read-shape contract
								// as the reference-page and direct traversal paths.
								const bool prefilterWithTopology =
										!topologyOnly &&
										state.shared.snapshot.supportsNativeAdjacentEdgeTopologyPages();
								if (topologyOnly || prefilterWithTopology) {
									topologyPage = readTopologyPageBudgeted(state, sourceNodeId, adjacencyDirection,
										continuation, pageRecords, topologyBytes);
								} else {
									if (state.shared.snapshot.supportsAdjacencyReadAdmission()) {
										size_t propertyBytes = 0U, bufferBytes = 0U;
										const auto updateBudget = [&] {
											checkGuardNow(state);
											fullPageBytes.set(checkedSizeAdd(propertyBytes, bufferBytes, "logical-plan adjacency source size overflowed"));
										};
										fullPage = state.shared.snapshot.scanAdjacentEdgesWithAdmission(sourceNodeId, adjacencyDirection,
											continuation, pageRecords,
											[&](size_t bytes) { propertyBytes = checkedSizeAdd(propertyBytes, bytes, "adjacency Property size overflowed"); updateBudget(); },
											[&](size_t bytes) { bufferBytes = bytes; updateBudget(); });
									} else if (state.shared.snapshot.supportsAdjacencyPropertyReadAdmission()) {
										fullPage = state.shared.snapshot.scanAdjacentEdgesWithPropertyAdmission(sourceNodeId, adjacencyDirection,
											continuation, pageRecords, [&](size_t bytes) {
												checkGuardNow(state);
												fullPageBytes.set(checkedSizeAdd(fullPageBytes.bytes(), bytes, "logical-plan adjacency source size overflowed"));
											});
									} else {
										fullPage = state.shared.snapshot.scanAdjacentEdges(sourceNodeId, adjacencyDirection, continuation, pageRecords);
									}
									for (const auto &edge : fullPage->edges) {
										validateEdge(edge);
										static_cast<void>(inlineResultPropertyBytes(edge.properties, state));
									}
									retainShapedBatchBytes(fullPage->edges, fullPageBytes, state.shared.context);
								}
								const bool topologyPageSelected = topologyOnly || prefilterWithTopology;
								const size_t pageSize =
										topologyPageSelected ? topologyPage->edges.size() : fullPage->edges.size();
								if (pageSize > pageRecords) {
									throw std::runtime_error(
											"logical graph backend exceeded the adjacency page bound (returned " +
											std::to_string(pageSize) + ", requested " + std::to_string(pageRecords) +
											")");
								}
								const auto &pageContinuation =
										topologyPageSelected ? topologyPage->continuation : fullPage->continuation;
								if (pageSize == 0U) {
									if (pageContinuation.has_value()) {
										throw std::runtime_error(
												"logical graph backend returned an empty nonterminal adjacency page");
									}
									return true;
								}
								const bool targetNeedsRecord = !traversal.getTargetLabels().empty() ||
															   !traversal.getTargetProperties().empty();
								// A selected column cannot distinguish a missing Property from an
								// explicitly stored NULL. Keep the complete-read path for nullable
								// target predicates, whose existing semantics distinguish those cases.
								const bool targetPropertiesCanBeSelected =
										!traversal.getTargetProperties().empty() &&
										!std::ranges::any_of(
												traversal.getTargetProperties(),
												[](const auto &predicate) { return isNull(predicate.second); });
								const bool canBatchTargetShape =
										state.shared.snapshot.supportsNativeNodeRefScans() && targetNeedsRecord &&
										(traversal.getTargetLabels().empty() ||
										 state.shared.snapshot.supportsNodeExistenceProbes()) &&
										(traversal.getTargetProperties().empty() || targetPropertiesCanBeSelected);
								detail::BudgetMemoryResource pageTargetMemory(state.shared.materialized_budget);
								std::pmr::unordered_map<uint64_t, bool> pageTargetMatches{&pageTargetMemory};
								// Both topology and complete adjacency pages already carry endpoint
								// IDs. Validate their liveness once per bounded page instead of
								// performing a Directory lookup for every Edge. The map is page-local
								// so memory remains bounded by the adjacency page limit and
								// early-stop semantics stay unchanged at the visitor boundary.
								std::pmr::unordered_map<uint64_t, bool> pageTargetPresence{&pageTargetMemory};
								if (state.shared.snapshot.supportsNodeExistenceProbes() &&
									traversal.getTargetLabels().empty() && traversal.getTargetProperties().empty()) {
									// Validate the topology record before building a batch request.  The
									// sorted existence API intentionally rejects zero IDs with
									// invalid_argument, while malformed storage topology must retain the
									// logical runtime's fail-closed runtime_error contract.
									if (topologyPage.has_value()) {
										for (const storage::LogicalGraphEdgeTopology &topology: topologyPage->edges) {
											validateEdge(topology);
										}
									} else if (fullPage.has_value()) {
										for (const storage::LogicalGraphEdge &edge: fullPage->edges) {
											validateEdge(edge);
										}
									}
									std::pmr::vector<uint64_t> pageTargetIds{&pageTargetMemory};
									pageTargetIds.reserve(pageSize);
									if (topologyPage.has_value()) {
										for (const auto &topology: topologyPage->edges) {
											pageTargetIds.push_back(
													adjacencyDirection ==
																	storage::LogicalGraphReadDirection::LGRD_OUTGOING
															? topology.target_node_id
															: topology.source_node_id);
										}
									} else if (fullPage.has_value()) {
										for (const storage::LogicalGraphEdge &edge: fullPage->edges) {
											pageTargetIds.push_back(
													adjacencyDirection ==
																	storage::LogicalGraphReadDirection::LGRD_OUTGOING
															? edge.target_node_id
															: edge.source_node_id);
										}
									}
									detail::sortUniqueIdsGuarded(pageTargetIds, [&] {
										if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
									});
									MaterializedByteReservation presentBytes(&state.shared.materialized_budget);
									presentBytes.set(packedBooleanBytes(pageTargetIds.size()));
									const std::vector<bool> present = state.shared.snapshot.containsNodesSorted(pageTargetIds);
									presentBytes.set(packedBooleanBytes(present.capacity()));
									if (present.size() != pageTargetIds.size()) {
										throw std::runtime_error(
												"logical graph backend changed adjacent endpoint cardinality");
									}
									for (size_t index = 0U; index < pageTargetIds.size(); ++index) {
										pageTargetPresence.emplace(pageTargetIds[index], present[index]);
									}
								}
								if (canBatchTargetShape) {
									std::pmr::vector<uint64_t> targetIds{&pageTargetMemory};
									targetIds.reserve(pageSize);
									if (topologyPage.has_value()) {
										for (const storage::LogicalGraphEdgeTopology &topology: topologyPage->edges) {
											validateEdge(topology);
											targetIds.push_back(
													adjacencyDirection ==
																	storage::LogicalGraphReadDirection::LGRD_OUTGOING
															? topology.target_node_id
															: topology.source_node_id);
										}
									} else {
										for (const storage::LogicalGraphEdge &edge: fullPage->edges) {
											validateEdge(edge);
											targetIds.push_back(
													adjacencyDirection ==
																	storage::LogicalGraphReadDirection::LGRD_OUTGOING
															? edge.target_node_id
															: edge.source_node_id);
										}
									}
									detail::sortUniqueIdsGuarded(targetIds, [&] {
										if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
									});
									MaterializedByteReservation labelsBytes(&state.shared.materialized_budget);
									std::vector<bool> labelsMatch;
									if (!traversal.getTargetLabels().empty()) {
										labelsBytes.set(packedBooleanBytes(targetIds.size()));
										labelsMatch = state.shared.snapshot.readNodeLabelMatchesSorted(
												targetIds, traversal.getTargetLabels());
										labelsBytes.set(packedBooleanBytes(labelsMatch.capacity()));
										if (labelsMatch.size() != targetIds.size()) {
											throw std::runtime_error(
													"logical graph backend changed target label-match cardinality");
										}
									}
									std::pmr::vector<std::pair<std::string_view, const PropertyValue *>> orderedTargetPredicates{&pageTargetMemory};
									orderedTargetPredicates.reserve(traversal.getTargetProperties().size());
									for (const auto &[key, expected]: traversal.getTargetProperties()) {
										checkGuardNow(state);
										if (key.empty()) throw std::invalid_argument("logical-plan target Property predicate has an empty key");
										orderedTargetPredicates.emplace_back(key, &expected);
									}
									std::ranges::sort(orderedTargetPredicates, {}, [](const auto &predicate) { return predicate.first; });
									std::pmr::vector<std::string_view> targetKeyViews{&pageTargetMemory};
									targetKeyViews.reserve(orderedTargetPredicates.size());
									for (const auto &[key, expected] : orderedTargetPredicates) targetKeyViews.push_back(key);
									MaterializedByteReservation targetPropertyBytes(&state.shared.materialized_budget);
									size_t targetKeyBytes = 0U;
									for (const std::string_view key: targetKeyViews) {
										targetKeyBytes = checkedSizeAdd(
												targetKeyBytes, key.size(),
												"logical-plan paged target Property key size overflowed");
									}
									const size_t targetFrames = checkedSizeAdd(
											checkedSizeMul(targetKeyViews.size(), sizeof(std::vector<PropertyValue>),
														   "logical-plan paged target Property frame size overflowed"),
											checkedSizeMul(
													checkedSizeMul(
															targetKeyViews.size(), targetIds.size(),
															"logical-plan paged target Property frame size overflowed"),
													sizeof(PropertyValue),
													"logical-plan paged target Property frame size overflowed"),
											"logical-plan paged target Property frame size overflowed");
									const size_t targetFixedBytes = targetFrames;
									targetPropertyBytes.set(
											checkedSizeAdd(targetFixedBytes, targetKeyBytes,
														   "logical-plan paged target Property batch size overflowed"));
									const std::function<void(size_t)> targetAdmission = [&](size_t bytes) {
										checkGuard(state);
										const size_t next = checkedSizeAdd(
												targetPropertyBytes.bytes(), bytes,
												"logical-plan paged target Property batch size overflowed");
										if (targetPropertyBytes.wouldExceed(next)) {
											throw std::runtime_error(
													"logical-plan paged target Property batch exceeded its byte bound");
										}
										targetPropertyBytes.set(next);
									};
									const auto targetColumns =
											targetKeyViews.empty() ? std::vector<std::vector<PropertyValue>>{}
											: state.shared.snapshot.supportsPropertyReadAdmission()
													? state.shared.snapshot.readNodePropertyColumnsSortedWithAdmission(
															  targetIds, targetKeyViews, targetAdmission)
													: state.shared.snapshot.readNodePropertyColumnsSorted(targetIds,
																								   targetKeyViews);
									const size_t targetPayloadBytes = propertyColumnBytes(
											targetColumns, targetKeyViews,
											"logical-plan paged target Property batch size overflowed", state.shared.context);
									const size_t retainedTargetBytes =
											checkedSizeAdd(targetFixedBytes - targetFrames, targetPayloadBytes,
														   "logical-plan paged target Property batch size overflowed");
									if (targetPropertyBytes.wouldExceed(retainedTargetBytes)) {
										throw std::runtime_error(
												"logical-plan paged target Property batch exceeded its byte bound");
									}
									targetPropertyBytes.set(retainedTargetBytes);
									if (targetColumns.size() != targetKeyViews.size() ||
										std::ranges::any_of(targetColumns, [&](const auto &column) {
											return column.size() != targetIds.size();
										})) {
										throw std::runtime_error(
												"logical graph backend changed target Property column cardinality");
									}
									for (size_t index = 0U; index < targetIds.size(); ++index) {
										bool matches = traversal.getTargetLabels().empty() || labelsMatch[index];
										for (size_t column = 0U; matches && column < targetKeyViews.size(); ++column) {
											checkGuardNow(state);
											matches = targetColumns[column][index] == *orderedTargetPredicates[column].second;
										}
										pageTargetMatches.emplace(targetIds[index], matches);
									}
								}
								const auto processEdge = [&](const storage::LogicalGraphEdge &edge) {
									checkGuard(state);
									validateEdge(edge);
									validateAdjacentEdgeOrder(edge.edge_id, previousEdgeId, "Traversal");
									const bool endpointMatches =
											adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
													? edge.source_node_id == sourceNodeId
													: edge.target_node_id == sourceNodeId;
									if (!endpointMatches) {
										throw std::runtime_error(
												"logical graph backend returned an invalid adjacent Edge");
									}
									incrementBounded(sourceEdges,
													 "logical-plan Traversal per-Node Edge counter overflowed");
									incrementBounded(state.task.stats.edges_scanned,
													 "logical-plan read runtime Edge counter overflowed");
									if (direction == "both" &&
										adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_INCOMING &&
										edge.source_node_id == sourceNodeId) {
										return true;
									}
									const uint64_t prefilterTargetId = adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
										? edge.target_node_id : edge.source_node_id;
									const auto targetMatch = pageTargetMatches.find(prefilterTargetId);
									if (targetMatch != pageTargetMatches.end() && !targetMatch->second) return true;
									if (!edgeMatches(state.shared.snapshot, edge, traversal.getEdgeType(),
													 traversal.getEdgeProperties(), state)) {
										return true;
									}
									const uint64_t targetId =
											adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
													? edge.target_node_id
													: edge.source_node_id;
									const auto existingTarget = row.values.find(traversal.getTargetVar());
									// If no target variable is requested, endpoint existence is sufficient;
									// avoid reading and decoding a Node that can never reach the result.
									const bool canDeferTarget = !targetNeedsRecord &&
																state.shared.allow_deferred_entity_refs &&
																state.shared.snapshot.supportsNodeExistenceProbes() &&
																(traversal.getTargetVar().empty() ||
																 (state.shared.snapshot.supportsNativeNodeRefScans() &&
																  existingTarget == row.values.end()));
									MaterializedByteReservation uncachedTargetBytes(&state.shared.materialized_budget);
									std::optional<storage::LogicalGraphNode> uncachedTarget;
									const storage::LogicalGraphNode *target = nullptr;
									if (canBatchTargetShape) {
										const auto targetMatch = pageTargetMatches.find(targetId);
										if (targetMatch == pageTargetMatches.end()) {
											throw std::runtime_error(
													"logical graph backend lost a batched target predicate result");
										}
										if (!targetMatch->second) {
											return true;
										}
										// If the target is not part of the output, selected columns are
										// sufficient; do not materialize a Node that cannot be observed.
										if (!traversal.getTargetVar().empty()) {
											target = &readNodeCachedBorrowed(state, targetId, uncachedTarget, uncachedTargetBytes);
											if (!nodeMatches(state.shared.snapshot, *target, traversal.getTargetLabels(),
															 traversal.getTargetProperties(), state)) {
												return true;
											}
										}
									} else if (targetNeedsRecord || !canDeferTarget) {
										target = &readNodeCachedBorrowed(state, targetId, uncachedTarget, uncachedTargetBytes);
										if (!nodeMatches(state.shared.snapshot, *target, traversal.getTargetLabels(),
														 traversal.getTargetProperties(), state)) {
											return true;
										}
									} else if (state.shared.snapshot.supportsNodeExistenceProbes()) {
										const auto pagePresence = pageTargetPresence.find(targetId);
										const bool present = pagePresence != pageTargetPresence.end()
																	 ? pagePresence->second
																	 : state.shared.snapshot.containsNode(targetId);
										if (!present) {
											throw std::runtime_error("logical graph backend returned an adjacent Edge "
																	 "with a missing endpoint");
										}
									}
									MaterializedByteReservation outputReservation(&state.shared.materialized_budget);
									constexpr const char *outputMessage = "logical-plan traversal output size overflowed";
									size_t outputBytes = logicalReadRowBytes(row, state.shared.context);
									const auto addBindingBytes = [&](const std::string &alias, size_t payloadBytes) {
										outputBytes = checkedSizeAdd(outputBytes, alias.size(), outputMessage);
										outputBytes = checkedSizeAdd(outputBytes, sizeof(LogicalPlanReadValue), outputMessage);
										outputBytes = checkedSizeAdd(outputBytes, payloadBytes, outputMessage);
									};
									if (!traversal.getEdgeVar().empty()) {
										addBindingBytes(traversal.getEdgeVar(), topologyOnly ? sizeof(storage::EntityRef) : logicalEdgeCacheBytes(edge, state.shared.context));
									}
									if (!traversal.getTargetVar().empty()) {
										addBindingBytes(traversal.getTargetVar(), canDeferTarget ? sizeof(storage::EntityRef) : logicalNodeCacheBytes(*target, state.shared.context));
									}
									outputReservation.set(outputBytes);
									LogicalPlanReadRow expanded = detail::copyLogicalReadRowGuarded(row, [&] { checkGuardNow(state); });
									if (!traversal.getEdgeVar().empty()) {
										LogicalPlanReadValue edgeBinding =
												topologyOnly
														? LogicalPlanReadValue(storage::EntityRef{edge.edge_id, true,
																								  edge.source_node_id,
																								  edge.target_node_id, 0U})
														: LogicalPlanReadValue(detail::copyLogicalEntityGuarded(edge, [&] { checkGuardNow(state); }));
										if (!bindValue(expanded, traversal.getEdgeVar(), std::move(edgeBinding))) {
											return true;
									}
									}
									if (!traversal.getTargetVar().empty()) {
										LogicalPlanReadValue targetBinding =
												canDeferTarget ? LogicalPlanReadValue(storage::EntityRef{
																		 targetId, false, 0U, 0U, 0U})
															   : LogicalPlanReadValue(detail::copyLogicalEntityGuarded(*target, [&] { checkGuardNow(state); }));
										if (!bindValue(expanded, traversal.getTargetVar(), std::move(targetBinding))) {
											return true;
										}
									}
									outputReservation.set(logicalReadRowBytes(expanded, state.shared.context));
									if (chargedReceiver != nullptr) {
										return (*chargedReceiver)(std::move(expanded), std::move(outputReservation));
									}
									outputReservation.reset();
									if (!visitor(std::move(expanded))) {
										return false;
									}
									return true;
								};
								if (topologyOnly) {
									for (const storage::LogicalGraphEdgeTopology &topology: topologyPage->edges) {
										const storage::LogicalGraphEdge edge{topology.edge_id, topology.source_node_id,
																			 topology.target_node_id, topology.type};
										if (!processEdge(edge)) {
											return false;
										}
									}
								} else if (prefilterWithTopology) {
									// Keep the original adjacency order while issuing one sorted,
									// complete read only for type-matching Edge IDs. Rejected
									// topology records still pass through processEdge so counters,
									// endpoint validation, duplicate handling, and early-stop
									// semantics remain identical to the complete-page path.
									detail::BudgetMemoryResource matchingIdMemory(state.shared.materialized_budget);
									std::pmr::vector<uint64_t> matchingIds{&matchingIdMemory};
									matchingIds.reserve(topologyPage->edges.size());
									for (const storage::LogicalGraphEdgeTopology &topology: topologyPage->edges) {
										checkGuardNow(state);
										validateEdge(topology);
										const uint64_t targetId = adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
											? topology.target_node_id : topology.source_node_id;
										const auto presence = pageTargetPresence.find(targetId);
										if ((traversal.getEdgeType().empty() || topology.type == traversal.getEdgeType()) && presence != pageTargetPresence.end() && !presence->second) {
											throw std::runtime_error("logical graph backend returned an adjacent Edge with a missing endpoint");
										}
										const auto target = pageTargetMatches.find(targetId);
										if ((traversal.getEdgeType().empty() || topology.type == traversal.getEdgeType()) &&
												(target == pageTargetMatches.end() || target->second)) {
											matchingIds.push_back(topology.edge_id);
										}
									}
									std::ranges::sort(matchingIds);
									matchingIds.erase(std::unique(matchingIds.begin(), matchingIds.end()),
													  matchingIds.end());
									MaterializedByteReservation matchingEdgeBytes(&state.shared.materialized_budget);
									const std::vector<storage::LogicalGraphEdge> matchingEdges =
											matchingIds.empty() ? std::vector<storage::LogicalGraphEdge>{}
																: readEdgesShapedSortedBudgeted(
																		  state, matchingIds, completeEdgeReadShape(), &matchingEdgeBytes);
									retainShapedBatchBytes(matchingEdges, matchingEdgeBytes, state.shared.context);
									if (matchingEdges.size() != matchingIds.size()) {
										throw std::runtime_error(
												"logical graph backend changed typed Edge batch cardinality");
									}
									for (const storage::LogicalGraphEdgeTopology &topology: topologyPage->edges) {
										const auto matching = std::ranges::lower_bound(matchingIds, topology.edge_id);
										if (matching == matchingIds.end() || *matching != topology.edge_id) {
											const storage::LogicalGraphEdge rejected{
													topology.edge_id, topology.source_node_id, topology.target_node_id,
													topology.type};
											if (!processEdge(rejected)) {
												return false;
											}
											continue;
										}
										const size_t matchingIndex =
												static_cast<size_t>(matching - matchingIds.begin());
										const storage::LogicalGraphEdge &edge = matchingEdges[matchingIndex];
										if (edge.edge_id != topology.edge_id ||
											edge.source_node_id != topology.source_node_id ||
											edge.target_node_id != topology.target_node_id ||
											edge.type != topology.type) {
											throw std::runtime_error("logical graph backend changed typed Edge "
																	 "topology during materialization");
										}
										if (!processEdge(edge)) {
											return false;
										}
									}
								} else {
									for (const storage::LogicalGraphEdge &edge: fullPage->edges) {
										if (!processEdge(edge)) {
											return false;
										}
									}
								}
								if (!pageContinuation.has_value() ||
									sourceEdges == state.shared.bounds.maximum_edges_per_node) {
									return true;
								}
								if (pageContinuation->previous_edge_id == 0U || pageContinuation->next_edge_id == 0U ||
									pageContinuation->previous_edge_id == pageContinuation->next_edge_id) {
									throw std::runtime_error(
											"logical graph backend returned an invalid adjacency continuation");
								}
								continuation = pageContinuation;
							}
						};
						if (direction == "out" || direction == "both") {
							if (!visitDirection(storage::LogicalGraphReadDirection::LGRD_OUTGOING)) {
								return false;
							}
						}
						if (direction == "in" || direction == "both") {
							return visitDirection(storage::LogicalGraphReadDirection::LGRD_INCOMING);
						}
						return true;
					});
