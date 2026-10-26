				case logical::LogicalOpType::LOP_SINGLE_ROW: {
					if (!op.getChildren().empty()) { // ZYX_COV_EXCL_LINE: LogicalSingleRow is a concrete leaf and
													 // cannot own a child.
						throw std::runtime_error("logical SingleRow unexpectedly has a child");
					}
					return state.task.injected_row == nullptr ? visitor({}) :
						visitor(detail::copyLogicalReadRowGuarded(*state.task.injected_row, [&] { checkGuardNow(state); }));
				}
				case logical::LogicalOpType::LOP_NODE_SCAN: {
					const auto &scan = dynamic_cast<const logical::LogicalNodeScan &>(op);
					if (!scan.getChildren().empty() || scan.getVariable().empty()) {
						throw std::invalid_argument("logical-plan Node scan has an invalid shape or variable");
					}
					const auto parameterPredicates = resolveParameterPropertyPredicates(scan, state);
					if (!parameterPredicates.has_value()) {
						return true;
					}
					std::vector<std::pair<std::string, PropertyValue>> equalityPredicates =
							scan.getPropertyPredicates();
					equalityPredicates.insert(equalityPredicates.end(), parameterPredicates->begin(),
											  parameterPredicates->end());
					const bool exactPropertyLookup = !equalityPredicates.empty() && scan.getRangePredicates().empty() &&
													 !scan.getCompositeEquality().has_value();
					const bool rangePropertyLookup = equalityPredicates.empty() &&
													 scan.getRangePredicates().size() == 1U &&
													 !scan.getCompositeEquality().has_value();
					const bool compositeRangePropertyLookup = !equalityPredicates.empty() &&
															  scan.getRangePredicates().size() == 1U &&
															  !scan.getCompositeEquality().has_value();
					// Prefer the streaming candidate contract.  The visitor is wired
					// directly to the downstream operator, so LIMIT/SKIP can stop the
					// producer without materializing another result vector. Backends that
					// do not expose the contract continue through the defensive legacy
					// lookup branch below.
					const auto consumeCandidateStream = [&](const auto &producer) -> std::optional<bool> {
						try {
							const auto stream = producer();
							if (!stream.has_value()) {
								return std::nullopt;
							}
							return *stream;
						} catch (const std::length_error &error) {
							// Storage-neutral adapters report their byte guard as a
							// length_error. Normalize that boundary at the logical
							// runtime so native and compatibility producers expose the
							// same query-facing resource failure.
							if (std::string_view(error.what()) ==
								"logical graph candidate stream exceeded its byte bound") {
								throw std::runtime_error(
										"logical graph indexed Node candidate batch exceeded its byte bound");
							}
							throw;
						}
					};
					size_t indexedCandidateRecords = 0U;
					MaterializedByteReservation seenIndexedCandidateBytes(&state.shared.materialized_budget);
					std::unordered_set<uint64_t> seenIndexedCandidateIds;
					const auto candidateVisitor = [&](const storage::IndexCandidateBatch &batch) {
						return visitIndexedCandidateBatch(batch, scan, equalityPredicates, *parameterPredicates,
														  indexedCandidateRecords, seenIndexedCandidateIds,
														  seenIndexedCandidateBytes, state, visitor);
					};
					if (exactPropertyLookup && state.shared.snapshot.supportsNativeExactNodeCandidates()) {
						if (const auto streamed = consumeCandidateStream([&] {
								return state.shared.snapshot.visitNodeCandidatesByExactProperties(
										scan.getLabels(), equalityPredicates, state.shared.bounds.maximum_node_records,
										state.shared.bounds.maximum_property_entries,
										state.shared.snapshot.supportsPropertyReadAdmission()
												? std::min(state.shared.bounds.maximum_candidate_bytes,
														   std::max<size_t>(1U,
																			state.shared.materialized_budget.available() / 2U))
												: state.shared.bounds.maximum_candidate_bytes,
										candidateVisitor);
							});
							streamed.has_value()) {
							return *streamed;
						}
					}
					if (rangePropertyLookup && state.shared.snapshot.supportsNativeRangeNodeCandidates()) {
						const logical::RangePredicate &range = scan.getRangePredicates().front();
						if (const auto streamed = consumeCandidateStream([&] {
								return state.shared.snapshot.visitNodeCandidatesByPropertyRange(
										scan.getLabels(),
										{range.key, range.minValue, range.maxValue, range.minInclusive,
										 range.maxInclusive},
										state.shared.bounds.maximum_node_records, state.shared.bounds.maximum_property_entries,
										state.shared.snapshot.supportsPropertyReadAdmission()
												? std::min(state.shared.bounds.maximum_candidate_bytes,
														   std::max<size_t>(1U,
																			state.shared.materialized_budget.available() / 2U))
												: state.shared.bounds.maximum_candidate_bytes,
										candidateVisitor);
							});
							streamed.has_value()) {
							return *streamed;
						}
					}
					if (compositeRangePropertyLookup && state.shared.snapshot.supportsNativeRangeNodeCandidates()) {
						const logical::RangePredicate &range = scan.getRangePredicates().front();
						if (const auto streamed = consumeCandidateStream([&] {
								return state.shared.snapshot.visitNodeCandidatesByCompositePropertyRange(
										scan.getLabels(),
										{equalityPredicates,
										 {range.key, range.minValue, range.maxValue, range.minInclusive,
										  range.maxInclusive}},
										state.shared.bounds.maximum_node_records, state.shared.bounds.maximum_property_entries,
										state.shared.snapshot.supportsPropertyReadAdmission()
												? std::min(state.shared.bounds.maximum_candidate_bytes,
														   std::max<size_t>(1U,
																			state.shared.materialized_budget.available() / 2U))
												: state.shared.bounds.maximum_candidate_bytes,
										candidateVisitor);
							});
							streamed.has_value()) {
							return *streamed;
						}
					}
					std::optional<storage::IndexCandidateBatch> exactIndexCandidates;
					std::optional<storage::IndexCandidateBatch> rangeIndexCandidates;
					std::optional<std::vector<storage::LogicalGraphNode>> indexNodes;
					if (exactPropertyLookup && state.shared.snapshot.supportsNativeExactNodeCandidates()) {
						exactIndexCandidates = state.shared.snapshot.findNodeCandidatesByExactProperties(
								scan.getLabels(), equalityPredicates, state.shared.bounds.maximum_node_records,
								state.shared.bounds.maximum_property_entries);
					} else if (exactPropertyLookup) {
						indexNodes = state.shared.snapshot.findNodesByExactProperties(scan.getLabels(), equalityPredicates,
																			   state.shared.bounds.maximum_node_records,
																			   state.shared.bounds.maximum_property_entries);
					} else if (rangePropertyLookup) {
						const logical::RangePredicate &range = scan.getRangePredicates().front();
						if (state.shared.snapshot.supportsNativeRangeNodeCandidates()) {
							rangeIndexCandidates = state.shared.snapshot.findNodeCandidatesByPropertyRange(
									scan.getLabels(),
									{range.key, range.minValue, range.maxValue, range.minInclusive, range.maxInclusive},
									state.shared.bounds.maximum_node_records, state.shared.bounds.maximum_property_entries);
						} else {
							indexNodes = state.shared.snapshot.findNodesByPropertyRange(
									scan.getLabels(),
									{range.key, range.minValue, range.maxValue, range.minInclusive, range.maxInclusive},
									state.shared.bounds.maximum_node_records, state.shared.bounds.maximum_property_entries);
						}
					} else if (compositeRangePropertyLookup) {
						const logical::RangePredicate &range = scan.getRangePredicates().front();
						if (state.shared.snapshot.supportsNativeRangeNodeCandidates()) {
							rangeIndexCandidates = state.shared.snapshot.findNodeCandidatesByCompositePropertyRange(
									scan.getLabels(),
									{equalityPredicates,
									 {range.key, range.minValue, range.maxValue, range.minInclusive,
									  range.maxInclusive}},
									state.shared.bounds.maximum_node_records, state.shared.bounds.maximum_property_entries);
						} else {
							indexNodes = state.shared.snapshot.findNodesByCompositePropertyRange(
									scan.getLabels(),
									{equalityPredicates,
									 {range.key, range.minValue, range.maxValue, range.minInclusive,
									  range.maxInclusive}},
									state.shared.bounds.maximum_node_records, state.shared.bounds.maximum_property_entries);
						}
					}
					if (exactIndexCandidates.has_value() || rangeIndexCandidates.has_value()) {
						const storage::IndexCandidateBatch &nativeCandidates =
								exactIndexCandidates.has_value() ? *exactIndexCandidates : *rangeIndexCandidates;
						return visitIndexedCandidateBatch(nativeCandidates, scan, equalityPredicates,
														  *parameterPredicates, indexedCandidateRecords,
														  seenIndexedCandidateIds, seenIndexedCandidateBytes, state,
														  visitor);
					}
					if (indexNodes.has_value()) {
						if (indexNodes->size() > state.shared.bounds.maximum_node_records) {
							throw std::runtime_error("logical graph indexed Node lookup exceeded the Node scan bound");
						}
						uint64_t previousNodeId = 0U;
						for (const storage::LogicalGraphNode &node: *indexNodes) {
							checkGuard(state);
							validateNode(node, state);
							if (node.node_id <= previousNodeId) {
								throw std::runtime_error(
										"logical graph indexed Node lookup returned non-monotonic Node IDs");
							}
							for (const std::string &label: scan.getLabels()) {
								if (std::ranges::find(node.labels, label) == node.labels.end()) {
									throw std::runtime_error("logical graph indexed Node lookup returned a Node "
															 "outside the requested label scope");
								}
							}
							previousNodeId = node.node_id;
							incrementBounded(state.task.stats.nodes_scanned,
											 "logical-plan indexed Node lookup counter overflowed");
							// The physical index is an acceleration only. Re-evaluate all
							// logical scan predicates before exposing an indexed candidate.
							if (nodeScanMatches(state.shared.snapshot, node, scan, *parameterPredicates, state)) {
								LogicalPlanReadRow row;
								row.values.emplace(scan.getVariable(), node);
								if (!visitor(std::move(row))) {
									return false;
								}
							}
						}
						return true;
					}
					if (const std::optional<bool> batched =
								tryVisitBatchedNativeNodeRefScan(scan, *parameterPredicates, state, visitor, columnReceiver);
						batched.has_value()) {
						return *batched;
					}
					if (state.shared.allow_deferred_entity_refs && state.shared.snapshot.supportsNativeNodeRefScans()) {
						bool continueVisitor = true;
						// Keep label evaluation on the page-local reference scan. The native
						// adapter resolves label tokens once and reports a match fact, avoiding
						// a second point Entity-Map read for every scanned Node.
						state.shared.snapshot.visitNodeRefsForLabelPredicate(
								state.shared.bounds.maximum_node_records, scan.getLabels(),
								[&](const storage::EntityRef &reference, const bool labelsMatch) {
									checkGuard(state);
									validateNodeRef(reference, "logical graph backend returned an invalid or Edge "
															   "reference from a Node scan");
									incrementBounded(state.task.stats.nodes_scanned,
													 "logical-plan read runtime Node counter overflowed");
									if (!labelsMatch) {
										return true;
									}
									LogicalPlanReadRow row;
									row.values.emplace(scan.getVariable(), reference);
									if (nodeRefScanMatches(state.shared.snapshot, row, scan.getVariable(), scan,
														   *parameterPredicates, state, true)) {
										continueVisitor = visitor(std::move(row));
									}
									return continueVisitor;
								});
						return continueVisitor;
					}
					uint64_t afterNodeId = 0U;
					for (;;) {
						const storage::LogicalGraphNodePage page =
								state.shared.snapshot.scanNodesAfter(afterNodeId, state.shared.bounds.maximum_node_page_records,
															  state.shared.bounds.maximum_metadata_pages_per_node_page);
						if (page.nodes.size() > state.shared.bounds.maximum_node_page_records) {
							throw std::runtime_error("logical graph backend exceeded the Node page bound");
						}
						if (page.nodes.empty()) {
							if (page.may_have_more) {
								throw std::runtime_error(
										"logical graph backend returned an empty nonterminal Node page");
							}
							return true;
						}
						uint64_t previousNodeId = afterNodeId;
						for (const storage::LogicalGraphNode &node: page.nodes) {
							checkGuard(state);
							if (node.node_id <= previousNodeId) {
								throw std::runtime_error("logical graph backend returned non-monotonic Node page IDs");
							}
							previousNodeId = node.node_id;
							incrementBounded(state.task.stats.nodes_scanned,
											 "logical-plan read runtime Node counter overflowed");
							if (nodeScanMatches(state.shared.snapshot, node, scan, *parameterPredicates, state)) {
								LogicalPlanReadRow row;
								row.values.emplace(scan.getVariable(), node);
								if (!visitor(std::move(row))) {
									return false;
								}
							}
							if (state.task.stats.nodes_scanned == state.shared.bounds.maximum_node_records) {
								return false;
							}
						}
						afterNodeId = previousNodeId;
						if (!page.may_have_more) {
							return true;
						}
					}
				}
				case logical::LogicalOpType::LOP_FILTER: {
					const auto &filter = dynamic_cast<const logical::LogicalFilter &>(op);
					if (!filter.getPredicate()) {
						throw std::invalid_argument("logical-plan Filter has no predicate");
					}
					const logical::LogicalOperator &filterChild = requireOnlyChild(filter);
					std::optional<detail::BoundExpressionInputs> boundInputs;
					if (state.shared.bound_program) {
						const expressions::Expression *predicate = filter.getPredicate().get();
						boundInputs.emplace(*state.shared.bound_program, state.shared.bound_program->id(op),
							std::span<const expressions::Expression *const>(&predicate, 1U), state.shared.snapshot,
							state.shared.materialized_budget, [&] { checkGuardNow(state); }, std::pmr::get_default_resource(), state.shared.context);
					}
					const auto acceptFilteredRow = [&](LogicalPlanReadRow row, MaterializedByteReservation charge) {
						PropertyValue result;
						{ ScopedBoundInputs scopedInputs(state, boundInputs, row);
							result = evaluateScalarExpression(*filter.getPredicate(), row, state); }
						if (booleanOrNull(result) != true)
							return true;
						if (chargedReceiver != nullptr) {
							if (charge.bytes() != 0U)
								charge.set(logicalReadRowBytes(row, state.shared.context));
							return (*chargedReceiver)(std::move(row), std::move(charge));
						}
						charge.reset();
						return visitor(std::move(row));
					};
					// A WHERE clause directly above a single-hop traversal is a common
					// shape for Edge-property predicates.  Feed its safe equality
					// constraints into the same global topology path used by inline
					// relationship properties, while retaining the Filter evaluator as
					// the semantic authority.
					if (filterChild.getType() == logical::LogicalOpType::LOP_TRAVERSAL) {
						const auto &traversal = dynamic_cast<const logical::LogicalTraversal &>(filterChild);
						if (const auto edgeProperties = inspectDirectEdgePropertyFilter(*filter.getPredicate(),
																						traversal.getEdgeVar(), state);
							edgeProperties.has_value()) {
							if (const std::optional<bool> direct = tryVisitDirectTraversal(
										traversal, state,
										[&](LogicalPlanReadRow row) {
											const PropertyValue result =
													evaluateScalarExpression(*filter.getPredicate(), row, state);
											return booleanOrNull(result) == true ? visitor(std::move(row)) : true;
										},
										&*edgeProperties);
								direct.has_value()) {
								return *direct;
							}
						}
					}
					// The optimizer stores literal predicates on a NodeScan so the scan can
					// use an exact/range/composite index.  Do not let the generic property
					// prefetch path consume that scan first: it would enumerate every Node,
					// defeating the index and LIMIT early-stop contract.  Residual predicates
					// remain authoritative in the normal Filter evaluator below and can still
					// use the candidate row's selected-property cache.
					const auto *filterScan = dynamic_cast<const logical::LogicalNodeScan *>(&filterChild);
					const bool hasIndexableScanShape =
							filterScan != nullptr && (!filterScan->getPropertyPredicates().empty() ||
													  !filterScan->getRangePredicates().empty() ||
													  filterScan->getCompositeEquality().has_value());
					const bool forwardsDirectColumns = columnReceiver && columnReceiver->filterOwner == &op;
					if (!hasIndexableScanShape && !forwardsDirectColumns) {
						const std::vector<const expressions::Expression *> candidates{filter.getPredicate().get()};
						const std::vector<const expressions::Expression *> propertyExpressions =
								selectBatchableExpressions(candidates);
						const std::vector<std::string> completeEntityVariables =
								selectBatchableCompleteEntityVariables(candidates);
						if (const std::optional<bool> batched = tryVisitBatchedPropertyExpressions(
									filterChild, propertyExpressions, state,
									[&](LogicalPlanReadRow row) { return visitor(std::move(row)); },
									[&](LogicalPlanReadRow &row, ExecutionState &predicateState) {
										const PropertyValue result =
												evaluateScalarExpression(*filter.getPredicate(), row, predicateState);
										return booleanOrNull(result) == true;
									},
									completeEntityVariables, chargedReceiver, true);
							batched.has_value()) {
							return *batched;
						}
					}
					const ChargedRowVisitor chargedInput = acceptFilteredRow;
					return visitOperator(
							filterChild, state,
							[&](LogicalPlanReadRow row) {
								return acceptFilteredRow(std::move(row),
														 MaterializedByteReservation(&state.shared.materialized_budget));
							},
							supportsChargedRowHandoff(filterChild.getType()) ? &chargedInput : nullptr,
							forwardsDirectColumns ? columnReceiver : nullptr);
				}
				case logical::LogicalOpType::LOP_PROJECT: {
					const auto &project = dynamic_cast<const logical::LogicalProject &>(op);
					const auto &items = project.getItems();
					if (items.empty()) {
						throw std::invalid_argument("logical-plan Project has no items");
					}
					std::unordered_set<std::string> aliases;
					aliases.reserve(items.size());
					for (const logical::LogicalProjectItem &item: items) {
						if (item.alias.empty() || !item.expression || !aliases.insert(item.alias).second) {
							throw std::invalid_argument("logical-plan Project has an invalid output item");
						}
					}
					detail::BudgetMemoryResource distinctMemory(state.shared.materialized_budget);
					std::pmr::unordered_set<std::pmr::string> distinctRows{&distinctMemory};
					std::optional<detail::ExternalQueryKeySet> externalDistinct;
					if (project.isDistinct() && state.shared.allow_cross_row_entity_batches &&
					        state.shared.bounds.maximum_materialized_bytes >= detail::ExternalQueryKeySet::MINIMUM_QUERY_BYTES) {
					    auto *provider = dynamic_cast<storage::LogicalQueryScratchProvider *>(&state.shared.snapshot);
					    if (provider && provider->queryScratchEnabled())
					        externalDistinct.emplace(*provider, state.shared.materialized_budget, distinctMemory,
					            state.shared.bounds.maximum_distinct_rows, state.shared.bounds.maximum_materialized_bytes / 8U,
					            [&] { checkGuard(state); }, [&] { checkGuardNow(state); },
                                128U * 1024U * 1024U, 1024U,
                                state.shared.context ? state.shared.context->resource_account : nullptr);
					}
					const auto finishDistinct = [&](bool completed) {
					    if (externalDistinct) {
					        externalDistinct->finish();
					        if (externalDistinct->spilled()) debug::PerfTrace::addValue("logical_plan.distinct.spilled", 1);
					    }
					    return completed;
					};
					const auto *identityReference =
							items.size() == 1U && !project.isDistinct()
									? dynamic_cast<const expressions::VariableReferenceExpression *>(
											  items.front().expression.get())
									: nullptr;
					const bool canForwardIdentity = identityReference != nullptr && !identityReference->hasProperty();
					const logical::LogicalOperator &projectChild = requireOnlyChild(project);
					std::optional<detail::BoundProjectionReferences> boundReferences;
					std::optional<detail::BoundExpressionInputs> boundInputs;
					if (state.shared.bound_program) {
						std::pmr::vector<const expressions::Expression *> expressionInputs(&distinctMemory);
						expressionInputs.reserve(items.size());
						for (const auto &item : items) expressionInputs.push_back(item.expression.get());
						boundInputs.emplace(*state.shared.bound_program, state.shared.bound_program->id(op), expressionInputs,
							state.shared.snapshot, state.shared.materialized_budget, [&] { checkGuardNow(state); }, std::pmr::get_default_resource(), state.shared.context);
					}
					// Single-item ownership forwarding stays on its existing path. The
					// transition adapter benefits repeated direct references in wider
					// projections without charging constant-only expressions for slots.
					if (!state.shared.bound_program && items.size() > 1U && std::ranges::any_of(items, [](const auto &item) {
						return dynamic_cast<const expressions::VariableReferenceExpression *>(item.expression.get()) != nullptr;
					})) {
						MaterializedByteReservation inputNamesBytes(&state.shared.materialized_budget);
						const auto inputs = projectChild.getOutputVariables();
						size_t bytes = checkedSizeMul(inputs.capacity(), sizeof(std::string),
							"logical-plan binding input names overflowed");
						for (const auto &name : inputs) bytes = checkedSizeAdd(bytes, detail::dynamicStringBufferBytes(name),
							"logical-plan binding input name buffers overflowed");
						inputNamesBytes.set(bytes);
						boundReferences.emplace(inputs, items, state.shared.materialized_budget);
					}
					const auto projectRow = [&](LogicalPlanReadRow row, MaterializedByteReservation charge) {
						if (!project.isDistinct() && items.size() == 1U) {
							const auto *propertyReference =
									dynamic_cast<const expressions::VariableReferenceExpression *>(
											items.front().expression.get());
							if (propertyReference != nullptr && propertyReference->hasProperty()) {
								auto cachedEntity = row.property_cache.find(propertyReference->getVariableName());
								if (cachedEntity != row.property_cache.end()) {
									auto cachedValue = cachedEntity->second.find(propertyReference->getPropertyName());
									if (cachedValue != cachedEntity->second.end()) {
										PropertyValue projectedValue = std::move(cachedValue->second);
										row.values.clear();
										row.property_cache.clear();
										row.complete_entity_property_cache.clear();
										row.entity_metadata_cache.clear();
										row.path_cache.clear();
										row.values.emplace(items.front().alias, std::move(projectedValue));
										charge.set(logicalReadRowBytes(row, state.shared.context));
										if (chargedReceiver != nullptr) {
											return (*chargedReceiver)(std::move(row), std::move(charge));
										}
										charge.reset();
										return visitor(std::move(row));
									}
								}
							}
						}
						if (!project.isDistinct() && items.size() == 1U) {
							const auto *function = dynamic_cast<const expressions::FunctionCallExpression *>(
									items.front().expression.get());
							const auto *entityFunction =
									function == nullptr
											? nullptr
											: dynamic_cast<const expressions::EntityIntrospectionFunction *>(
													  expressions::FunctionRegistry::getInstance().lookupScalarFunction(
															  function->getFunctionName()));
							if (function != nullptr && entityFunction != nullptr &&
								lowerCase(function->getFunctionName()) == "id" && function->getArgumentCount() == 1U) {
								const PropertyValue identity =
										evaluateScalarExpression(*items.front().expression, row, state);
								row.values.clear();
								row.property_cache.clear();
								row.complete_entity_property_cache.clear();
								row.entity_metadata_cache.clear();
								row.path_cache.clear();
								row.values.emplace(items.front().alias, identity);
								charge.set(logicalReadRowBytes(row, state.shared.context));
								if (chargedReceiver != nullptr) {
									return (*chargedReceiver)(std::move(row), std::move(charge));
								}
								charge.reset();
								return visitor(std::move(row));
							}
						}
						if (canForwardIdentity && row.values.contains(identityReference->getVariableName())) {
							const std::string &source = identityReference->getVariableName();
							const std::string &alias = items.front().alias;
							auto forwarded = std::move(row.values.at(source));
							row.values.clear();
							row.values.emplace(alias, std::move(forwarded));
							const auto retainAndRenameAlias = [&](auto &cache) {
								auto entry = cache.find(source);
								if (entry == cache.end()) {
									cache.clear();
									return;
								}
								if (source == alias) {
									std::erase_if(cache,
												  [&](const auto &candidate) { return candidate.first != alias; });
									return;
								}
								auto retained = cache.extract(entry);
								cache.clear();
								retained.key() = alias;
								cache.insert(std::move(retained));
							};
							retainAndRenameAlias(row.property_cache);
							if (source != alias && row.complete_entity_property_cache.erase(source) != 0U) {
								row.complete_entity_property_cache.clear();
								row.complete_entity_property_cache.insert(alias);
							} else {
								std::erase_if(row.complete_entity_property_cache,
											  [&](const std::string &variable) { return variable != alias; });
							}
							retainAndRenameAlias(row.entity_metadata_cache);
							retainAndRenameAlias(row.path_cache);
							charge.set(logicalReadRowBytes(row, state.shared.context));
							if (chargedReceiver != nullptr) {
								return (*chargedReceiver)(std::move(row), std::move(charge));
							}
							charge.reset();
							return visitor(std::move(row));
						}
						const size_t inputBytes = logicalReadRowBytes(row, state.shared.context);
						if (charge.bytes() == 0U)
							charge.set(inputBytes);
						const size_t inputChargeBytes = charge.bytes();
						size_t projectedBytes = 0U;
						LogicalPlanReadRow projected;
						projected.values.reserve(items.size());
						{
							ScopedBoundInputs scopedInputs(state, boundInputs, row);
							std::optional<detail::BoundProjectionReferences::RowBinding> inputSlots;
							if (boundReferences) inputSlots.emplace(*boundReferences, row);
							size_t itemIndex = 0U;
							for (const logical::LogicalProjectItem &item: items) {
								checkGuard(state);
								const auto *instruction = boundReferences ? &boundReferences->instruction(itemIndex) : nullptr;
								const auto *value = instruction == nullptr ? nullptr : boundReferences->value(*instruction);
								if (value != nullptr && instruction->kind == detail::ProjectionReferenceKind::PRK_VALUE) {
									debug::PerfTrace::addValue("logical_plan.bound_projection.slot_reads", 1);
									projected.values.emplace(item.alias, *value);
								} else if (value != nullptr && instruction->kind == detail::ProjectionReferenceKind::PRK_PROPERTY) {
									debug::PerfTrace::addValue("logical_plan.bound_projection.property_reads", 1);
									projected.values.emplace(item.alias, readProperty(state.shared.snapshot, row,
										instruction->expression->getVariableName(), instruction->expression->getPropertyName(), state, value));
								} else {
									projected.values.emplace(item.alias, evaluateExpression(*item.expression, row, state));
								}
								++itemIndex;
								propagateProjectedEntityCaches(*item.expression, row, projected, item.alias, state);
								projectedBytes = logicalReadRowBytes(projected, state.shared.context);
								charge.set(checkedSizeAdd(inputChargeBytes, projectedBytes,
														  "logical-plan Project live row size overflowed"));
							}
						} // Slot borrows end before input destruction or downstream callbacks.
						if (project.isDistinct()) {
							auto key = distinctRowKey(projected, items, &distinctMemory);
							if (externalDistinct) {
								if (!externalDistinct->insert(std::move(key))) return true;
							} else {
								if (distinctRows.contains(key)) return true;
								if (distinctRows.size() == state.shared.bounds.maximum_distinct_rows)
									throw std::runtime_error("logical-plan DISTINCT exceeded its row bound");
								distinctRows.insert(std::move(key));
							}
						}
						row = {};
						if (chargedReceiver != nullptr) {
							charge.set(projectedBytes);
							return (*chargedReceiver)(std::move(projected), std::move(charge));
						}
						charge.reset();
						return visitor(std::move(projected));
					};
					const auto emitProjectedRow = [&](LogicalPlanReadRow row) {
						return projectRow(std::move(row), MaterializedByteReservation(&state.shared.materialized_budget));
					};
					const ChargedRowVisitor chargedProject = projectRow;

					// Fuse scalar observers across one projection boundary. The eligibility
					// check rejects every bare/path-dependent use, so the internal NULL
					// slots cannot escape or participate in entity/path comparisons.
					const auto *pathProject = dynamic_cast<const logical::LogicalProject *>(&projectChild);
					if (pathProject != nullptr && !pathProject->isDistinct()) {
						std::unordered_map<std::string, const expressions::Expression *> pathExpressions;
						std::unordered_set<std::string> childAliases;
						bool valid = !pathProject->getItems().empty();
						for (const auto &item: pathProject->getItems()) {
							valid = valid && !item.alias.empty() && item.expression &&
									childAliases.insert(item.alias).second;
							if (item.expression &&
								item.expression->getExpressionType() == expressions::ExpressionType::SHORTEST_PATH) {
								pathExpressions.emplace(item.alias, item.expression.get());
							}
						}
						std::unordered_map<std::string, unsigned> demands;
						std::function<bool(const expressions::Expression *)> inspectDemand;
						inspectDemand = [&](const expressions::Expression *expression) -> bool {
							if (expression == nullptr)
								return false;
							using expressions::ExpressionType;
							switch (expression->getExpressionType()) {
								case ExpressionType::LITERAL:
								case ExpressionType::PARAMETER:
									return true;
								case ExpressionType::VARIABLE_REFERENCE:
								case ExpressionType::PROPERTY_ACCESS: {
									const auto &reference =
											static_cast<const expressions::VariableReferenceExpression &>(*expression);
									return !pathExpressions.contains(reference.getVariableName());
								}
								case ExpressionType::FUNCTION_CALL: {
									const auto &function =
											static_cast<const expressions::FunctionCallExpression &>(*expression);
									const auto name = lowerCase(function.getFunctionName());
									if ((name == "size" || name == "length") && function.getArgumentCount() == 1U) {
										const auto *reference =
												dynamic_cast<const expressions::VariableReferenceExpression *>(
														function.getArguments().front().get());
										if (reference != nullptr && !reference->hasProperty() &&
											pathExpressions.contains(reference->getVariableName())) {
											demands[reference->getVariableName()] |= name == "size" ? 1U : 2U;
											return true;
										}
									}
									return std::ranges::all_of(function.getArguments(), [&](const auto &argument) {
										return inspectDemand(argument.get());
									});
								}
								case ExpressionType::BINARY_OP: {
									const auto &binary =
											static_cast<const expressions::BinaryOpExpression &>(*expression);
									if (binary.getOperator() == expressions::BinaryOperatorType::BOP_AND ||
										binary.getOperator() == expressions::BinaryOperatorType::BOP_OR)
										return false;
									return inspectDemand(binary.getLeft()) && inspectDemand(binary.getRight());
								}
								case ExpressionType::UNARY_OP:
									return inspectDemand(
											static_cast<const expressions::UnaryOpExpression &>(*expression)
													.getOperand());
								case ExpressionType::IS_NULL:
									return inspectDemand(static_cast<const expressions::IsNullExpression &>(*expression)
																 .getExpression());
								case ExpressionType::EXPR_LIST_LITERAL: {
									const auto &list =
											static_cast<const expressions::ListLiteralExpression &>(*expression);
									return list.isStatic() ||
										   std::ranges::all_of(list.getElements(), [&](const auto &element) {
											   return inspectDemand(element.get());
										   });
								}
								default:
									return false;
							}
						};
						for (const auto &item: items)
							valid = valid && inspectDemand(item.expression.get());
						// Dropped paths retain ordinary evaluation/error semantics.
						for (const auto &[alias, expression]: pathExpressions) {
							static_cast<void>(expression);
							valid = valid && demands.contains(alias);
						}
						if (valid && !pathExpressions.empty()) {
							const auto emitScalarPathRow = [&](LogicalPlanReadRow row) {
								LogicalPlanReadRow intermediate;
								for (const auto &item: pathProject->getItems()) {
									checkGuard(state);
									if (pathExpressions.contains(item.alias)) {
										const unsigned demand = demands.at(item.alias);
										if ((demand & 1U) != 0U)
											intermediate.entity_metadata_cache[item.alias]["$shortest_path_size"] =
													evaluateScalarExpression(*item.expression, row, state,
																			 ShortestPathResultMode::SPR_SIZE);
										if ((demand & 2U) != 0U)
											intermediate.entity_metadata_cache[item.alias]["$shortest_path_length"] =
													evaluateScalarExpression(*item.expression, row, state,
																			 ShortestPathResultMode::SPR_LENGTH);
										intermediate.values.emplace(item.alias, PropertyValue());
									} else {
										intermediate.values.emplace(item.alias,
																	evaluateExpression(*item.expression, row, state));
										propagateProjectedEntityCaches(*item.expression, row, intermediate, item.alias, state);
									}
								}
								if (logicalReadRowBytes(intermediate, state.shared.context) > state.shared.bounds.maximum_materialized_bytes) {
									throw std::runtime_error(
											"logical-plan scalar path projection exceeded its materialized bound");
								}
								return emitProjectedRow(std::move(intermediate));
							};
							std::vector<const expressions::Expression *> candidates;
							for (const auto &item: pathProject->getItems())
								candidates.push_back(item.expression.get());
							const auto selected = selectBatchableExpressions(candidates);
							const auto complete = selectBatchableCompleteEntityVariables(candidates);
							const auto &input = requireOnlyChild(*pathProject);
							if (!selected.empty() || !complete.empty()) {
								if (const auto batched = tryVisitBatchedPropertyExpressions(
											input, selected, state, emitScalarPathRow, {}, complete);
									batched)
									return finishDistinct(*batched);
							}
							return finishDistinct(visitOperator(input, state, emitScalarPathRow));
						}
					}
					std::vector<const expressions::Expression *> projectionCandidates;
					projectionCandidates.reserve(items.size());
					for (const logical::LogicalProjectItem &item: items) {
						projectionCandidates.push_back(item.expression.get());
					}
					const std::vector<const expressions::Expression *> projectionExpressions =
							selectBatchableExpressions(projectionCandidates);
					const std::vector<std::string> completeEntityVariables =
							selectBatchableCompleteEntityVariables(projectionCandidates);
					if (!projectionExpressions.empty() || !completeEntityVariables.empty()) {
						if (const std::optional<bool> batched = tryVisitBatchedPropertyExpressions(
									projectChild, projectionExpressions, state, emitProjectedRow, {},
									completeEntityVariables, chargedReceiver != nullptr ? &chargedProject : nullptr,
									!items.empty() && !project.isDistinct() && std::ranges::all_of(items, [](const auto &item) {
										return item.expression && item.expression->getExpressionType() == expressions::ExpressionType::PROPERTY_ACCESS;
									}));
							batched.has_value()) {
							return finishDistinct(*batched);
						}
					}
					return finishDistinct(visitOperator(projectChild, state, emitProjectedRow,
										 chargedReceiver != nullptr && supportsChargedRowHandoff(projectChild.getType())
												 ? &chargedProject
												 : nullptr));
				}
