				case logical::LogicalOpType::LOP_NAMED_PATH: {
					const auto &namedPath = dynamic_cast<const logical::LogicalNamedPath &>(op);
					const auto &nodeVariables = namedPath.getNodeVariables();
					const auto &edgeVariables = namedPath.getEdgeVariables();
					const bool capturedVariableLengthPath = edgeVariables.empty() && nodeVariables.size() == 2U;
					if (namedPath.getPathVariable().empty() || nodeVariables.empty() ||
						(!capturedVariableLengthPath && edgeVariables.size() + 1U != nodeVariables.size())) {
						throw std::invalid_argument("logical-plan NamedPath has an invalid path shape");
					}
					for (const std::string &variable: nodeVariables) {
						if (variable.empty()) {
							throw std::invalid_argument("logical-plan NamedPath has an anonymous Node component");
						}
					}
					for (const std::string &variable: edgeVariables) {
						if (variable.empty()) {
							throw std::invalid_argument(
									"logical-plan NamedPath has an anonymous relationship component");
						}
					}
					const auto processNamedPathRow = [&](LogicalPlanReadRow row,
														 MaterializedByteReservation inputRowBytes) {
						if (inputRowBytes.bytes() == 0U) {
							admitReferenceResultBytes(
									inputRowBytes, logicalReadRowBytes(row, state.shared.context),
									"logical-plan NamedPath input row exceeded its shared byte bound");
						}
						MaterializedByteReservation boundNodeSlots(&state.shared.materialized_budget);
						MaterializedByteReservation boundNodePointerSlots(&state.shared.materialized_budget);
						MaterializedByteReservation deferredBoundNodeSlots(&state.shared.materialized_budget);
						MaterializedByteReservation nodePointerSlots(&state.shared.materialized_budget);
						MaterializedByteReservation edgePointerSlots(&state.shared.materialized_budget);
						MaterializedByteReservation boundEdgeSlots(&state.shared.materialized_budget);
						MaterializedByteReservation boundNodeResultBytes(&state.shared.materialized_budget);
						MaterializedByteReservation capturedNodeResultBytes(&state.shared.materialized_budget);
						MaterializedByteReservation edgeResultBytes(&state.shared.materialized_budget);
						MaterializedByteReservation inlineNodePayloadBytes(&state.shared.materialized_budget);
						MaterializedByteReservation inlineEdgePayloadBytes(&state.shared.materialized_budget);
						std::vector<storage::LogicalGraphNode> boundNodeRecords;
						size_t boundNodeCapacity = nodeVariables.size();
						if (capturedVariableLengthPath) {
							const auto captured = row.path_cache.find(nodeVariables.back());
							if (captured != row.path_cache.end()) {
								const size_t deferredCount = static_cast<size_t>(std::ranges::count_if(
										captured->second.node_refs,
										[](const storage::EntityRef &reference) { return reference.entity_id != 0U; }));
								boundNodeCapacity = checkedSizeAdd(boundNodeCapacity, deferredCount,
																   "logical-plan NamedPath Node capacity overflowed");
							}
						}
						reserveNamedPathSlots(boundNodeRecords, boundNodeCapacity, boundNodeSlots);
						std::vector<const storage::LogicalGraphNode *> boundNodes;
						reserveNamedPathSlots(boundNodes, nodeVariables.size(), boundNodePointerSlots);
						std::vector<storage::EntityRef> deferredBoundNodes;
						for (const std::string &variable: nodeVariables) {
							const LogicalPlanReadValue &value = requireBinding(row, variable);
							if (const auto *reference = std::get_if<storage::EntityRef>(&value)) {
								validateNodeRef(*reference, "logical-plan NamedPath received an invalid or Edge "
															"reference where a Node was required");
								appendNamedPathReference(deferredBoundNodes, *reference, nodeVariables.size(),
														 deferredBoundNodeSlots);
							} else {
								const auto *node = std::get_if<storage::LogicalGraphNode>(&value);
								if (node == nullptr)
									throw std::runtime_error("logical-plan expected a Node variable");
								validateNode(*node, state);
								const size_t previousBytes = inlineNodePayloadBytes.bytes();
								admitReferenceResultBytes(
										inlineNodePayloadBytes,
										checkedSizeAdd(previousBytes, logicalNodeCacheBytes(*node, state.shared.context) - sizeof(*node),
													   "logical-plan NamedPath inline Node payload overflowed"),
										"logical-plan NamedPath inline Node payload exceeded its shared byte bound");
								try {
									boundNodeRecords.push_back(*node);
								} catch (...) {
									inlineNodePayloadBytes.set(previousBytes);
									throw;
								}
							}
						}
						std::vector<storage::LogicalGraphNode> materializedBoundNodes =
								materializeNodeReferences(deferredBoundNodes, state, true, &boundNodeResultBytes);
						for (storage::LogicalGraphNode &node: materializedBoundNodes) {
							boundNodeRecords.push_back(std::move(node));
						}
						for (const std::string &variable: nodeVariables) {
							const LogicalPlanReadValue &value = requireBinding(row, variable);
							const uint64_t id = nodeBindingId(value, state);
							const auto position =
									std::ranges::find(boundNodeRecords, id, &storage::LogicalGraphNode::node_id);
							if (position == boundNodeRecords.end()) {
								throw std::runtime_error("logical-plan NamedPath lost a Node binding");
							}
							boundNodes.push_back(&*position);
						}
						std::vector<const storage::LogicalGraphNode *> nodes;
						if (!capturedVariableLengthPath) {
							nodes = std::move(boundNodes);
							nodePointerSlots = std::move(boundNodePointerSlots);
						}
						std::vector<const storage::LogicalGraphEdge *> edges;
						std::vector<storage::LogicalGraphEdge> boundEdgeRecords;
						if (capturedVariableLengthPath) {
							const auto captured = row.path_cache.find(nodeVariables.back());
							if (captured == row.path_cache.end() || captured->second.nodes.size() < 2U ||
								captured->second.nodes.size() !=
										captured->second.edges.size() + captured->second.edge_refs.size() + 1U ||
								(!captured->second.node_refs.empty() &&
								 captured->second.node_refs.size() != captured->second.nodes.size()) ||
								(captured->second.edges.size() != 0U && !captured->second.edge_refs.empty()) ||
								captured->second.nodes.front().node_id != boundNodes.front()->node_id ||
								captured->second.nodes.back().node_id != boundNodes.back()->node_id) {
								throw std::runtime_error(
										"logical-plan NamedPath lost its variable-length traversal route");
							}
							nodes.clear();
							reserveNamedPathSlots(nodes, captured->second.nodes.size(), nodePointerSlots);
							MaterializedByteReservation deferredCapturedNodeSlots(&state.shared.materialized_budget);
							std::vector<storage::EntityRef> deferredCapturedNodes;
							if (!captured->second.node_refs.empty()) {
								for (const storage::EntityRef &reference: captured->second.node_refs) {
									if (reference.entity_id != 0U) {
										appendNamedPathReference(deferredCapturedNodes, reference,
																 captured->second.node_refs.size(),
																 deferredCapturedNodeSlots);
									}
								}
							}
							std::vector<storage::LogicalGraphNode> materializedCapturedNodes =
									materializeNodeReferences(deferredCapturedNodes, state, true,
															  &capturedNodeResultBytes);
							size_t materializedCapturedOrdinal = 0U;
							for (size_t nodeIndex = 0U; nodeIndex < captured->second.nodes.size(); ++nodeIndex) {
								const storage::LogicalGraphNode *node = &captured->second.nodes[nodeIndex];
								if (!captured->second.node_refs.empty() &&
									captured->second.node_refs[nodeIndex].entity_id != 0U) {
									const storage::EntityRef &reference = captured->second.node_refs[nodeIndex];
									validateNodeRef(reference,
													"logical-plan NamedPath has an invalid deferred Node reference");
									if (reference.entity_id != node->node_id) {
										throw std::runtime_error(
												"logical-plan NamedPath has an invalid deferred Node reference");
									}
									if (materializedCapturedOrdinal >= materializedCapturedNodes.size()) {
										throw std::runtime_error("logical-plan NamedPath lost a deferred Node during "
																 "batch materialization");
									}
									if (materializedCapturedNodes[materializedCapturedOrdinal].node_id !=
										reference.entity_id) {
										throw std::runtime_error("logical-plan NamedPath changed deferred Node "
																 "identity during batch materialization");
									}
									boundNodeRecords.push_back(
											std::move(materializedCapturedNodes[materializedCapturedOrdinal++]));
									node = &boundNodeRecords.back();
								} else {
									validateNode(*node, state);
								}
								nodes.push_back(node);
							}
							const size_t edgeCapacity =
									checkedSizeAdd(captured->second.edges.size(), captured->second.edge_refs.size(),
												   "logical-plan NamedPath Edge capacity overflowed");
							reserveNamedPathSlots(boundEdgeRecords, captured->second.edge_refs.size(), boundEdgeSlots);
							reserveNamedPathSlots(edges, edgeCapacity, edgePointerSlots);
							for (const storage::LogicalGraphEdge &edge: captured->second.edges) {
								validateEdge(edge);
								edges.push_back(&edge);
							}
							std::vector<storage::LogicalGraphEdge> materializedReferences = materializeEdgeReferences(
									captured->second.edge_refs, state, true, &edgeResultBytes);
							for (storage::LogicalGraphEdge &edge: materializedReferences) {
								boundEdgeRecords.push_back(std::move(edge));
								edges.push_back(&boundEdgeRecords.back());
							}
						} else {
							reserveNamedPathSlots(boundEdgeRecords, edgeVariables.size(), boundEdgeSlots);
							reserveNamedPathSlots(edges, edgeVariables.size(), edgePointerSlots);
							struct PendingNamedEdge {
								std::optional<storage::EntityRef> reference;
								std::optional<storage::LogicalGraphEdge> complete;
							};
							MaterializedByteReservation pendingEdgeSlots(&state.shared.materialized_budget);
							std::vector<PendingNamedEdge> pendingEdges;
							reserveNamedPathSlots(pendingEdges, edgeVariables.size(), pendingEdgeSlots);
							MaterializedByteReservation deferredEdgeSlots(&state.shared.materialized_budget);
							std::vector<storage::EntityRef> deferredReferences;
							for (const std::string &variable: edgeVariables) {
								const LogicalPlanReadValue &value = requireBinding(row, variable);
								if (const auto *reference = std::get_if<storage::EntityRef>(&value)) {
									validateEdgeRef(*reference, "logical-plan NamedPath received an invalid or Node "
																"reference where an Edge was required");
									pendingEdges.push_back({*reference, std::nullopt});
									appendNamedPathReference(deferredReferences, *reference, edgeVariables.size(),
															 deferredEdgeSlots);
								} else {
									const auto *edge = std::get_if<storage::LogicalGraphEdge>(&value);
									if (edge == nullptr)
										throw std::runtime_error("logical-plan expected a relationship variable");
									validateEdge(*edge);
									const size_t previousBytes = inlineEdgePayloadBytes.bytes();
									admitReferenceResultBytes(
											inlineEdgePayloadBytes,
											checkedSizeAdd(previousBytes, logicalEdgeCacheBytes(*edge, state.shared.context) - sizeof(*edge),
														   "logical-plan NamedPath inline Edge payload overflowed"),
											"logical-plan NamedPath inline Edge payload exceeded its shared byte "
											"bound");
									try {
										pendingEdges.push_back({std::nullopt, *edge});
									} catch (...) {
										inlineEdgePayloadBytes.set(previousBytes);
										throw;
									}
								}
							}
							std::vector<storage::LogicalGraphEdge> materializedReferences =
									materializeEdgeReferences(deferredReferences, state, true, &edgeResultBytes);
							size_t deferredOrdinal = 0U;
							for (PendingNamedEdge &pending: pendingEdges) {
								if (pending.reference.has_value()) {
									if (deferredOrdinal >= materializedReferences.size()) {
										throw std::runtime_error("logical-plan NamedPath lost a deferred Edge during "
																 "batch materialization");
									}
									pending.complete = std::move(materializedReferences[deferredOrdinal++]);
								}
								if (!pending.complete.has_value()) {
									throw std::runtime_error("logical-plan NamedPath has an empty Edge binding");
								}
								boundEdgeRecords.push_back(std::move(*pending.complete));
								edges.push_back(&boundEdgeRecords.back());
							}
						}
						for (size_t index = 0U; index < edges.size(); ++index) {
							const uint64_t first = nodes[index]->node_id;
							const uint64_t second = nodes[index + 1U]->node_id;
							const storage::LogicalGraphEdge &edge = *edges[index];
							const bool connects = (edge.source_node_id == first && edge.target_node_id == second) ||
												  (edge.source_node_id == second && edge.target_node_id == first);
							if (!connects) {
								throw std::runtime_error(
										"logical-plan NamedPath relationship does not connect its adjacent Nodes");
							}
						}

						MaterializedByteReservation sourcePropertyBytes(&state.shared.materialized_budget);
						detail::BudgetMemoryResource propertyCacheMemory(state.shared.materialized_budget);
						MaterializedPropertyCache materialized(&propertyCacheMemory);
						const auto pathIdValue = [](uint64_t id) {
							if (id > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
								throw std::overflow_error(
										"logical-plan NamedPath cannot represent an entity ID in a Property value");
							}
							return PropertyValue(static_cast<int64_t>(id));
						};
						MaterializedByteReservation pathBytes(&state.shared.materialized_budget);
						const auto admitPathBytes = [&](size_t nextBytes) {
							if (pathBytes.wouldExceed(nextBytes)) {
								throw std::runtime_error(
										"logical-plan NamedPath result exceeded its shared byte bound");
							}
							pathBytes.set(nextBytes);
						};
						std::vector<PropertyValue> path;
						const size_t pathCapacity = checkedSizeAdd(nodes.size(), edges.size(),
																   "logical-plan NamedPath result capacity overflowed");
						admitPathBytes(checkedSizeMul(pathCapacity, sizeof(PropertyValue),
													  "logical-plan NamedPath result capacity overflowed"));
						path.reserve(pathCapacity);
						admitPathBytes(checkedSizeMul(path.capacity(), sizeof(PropertyValue),
													  "logical-plan NamedPath result capacity overflowed"));
						const auto appendEntity = [&](std::string_view type, uint64_t id,
													  const PropertyMap &properties) {
							const PropertyValue typeValue{std::string(type)};
							const PropertyValue idValue = pathIdValue(id);
							const bool hasType = properties.contains("_type");
							const bool hasId = properties.contains("_id");
							const size_t propertyCount = checkedSizeAdd(
									properties.size(), static_cast<size_t>(!hasType) + static_cast<size_t>(!hasId),
									"logical-plan NamedPath Property count overflowed");
							if (propertyCount > state.shared.bounds.maximum_property_entries) {
								throw std::runtime_error(
										"logical-plan NamedPath exceeded the configured Property bound");
							}
							constexpr const char *message = "logical-plan NamedPath result size overflowed";
							size_t mapBytes = checkedSizeAdd(
									sizeof(uint32_t),
									checkedSizeMul(propertyCount,
												   sizeof(PropertyValue::MapType::value_type) + sizeof(void *),
												   message),
									message);
							const auto addProperty = [&](std::string_view key, const PropertyValue &value) {
								mapBytes = checkedSizeAdd(
										mapBytes,
										checkedSizeAdd(checkedSizeAdd(key.size(), sizeof(size_t), message),
													   property_utils::getPropertyValueSize(value), message),
										message);
							};
							if (!hasType)
								addProperty("_type", typeValue);
							if (!hasId)
								addProperty("_id", idValue);
							for (const auto &[key, value]: properties)
								addProperty(key, value);
							admitPathBytes(checkedSizeAdd(pathBytes.bytes(), mapBytes, message));
							PropertyValue::MapType entityMap;
							entityMap.reserve(propertyCount);
							entityMap.emplace("_type", typeValue);
							entityMap.emplace("_id", idValue);
							for (const auto &[key, value]: properties)
								entityMap.insert_or_assign(key, value);
							path.emplace_back(std::move(entityMap));
						};
						for (size_t index = 0U; index < nodes.size(); ++index) {
							const storage::LogicalGraphNode &node = *nodes[index];
							appendEntity(
									"node", node.node_id,
									materializeEntityProperties(row, node, state, materialized, &sourcePropertyBytes));
							if (index == edges.size()) {
								continue;
							}
							const storage::LogicalGraphEdge &edge = *edges[index];
							appendEntity(
									"relationship", edge.edge_id,
									materializeEntityProperties(row, edge, state, materialized, &sourcePropertyBytes));
						}
						if (!bindValue(row, namedPath.getPathVariable(), PropertyValue(std::move(path)))) {
							throw std::runtime_error("logical-plan NamedPath attempted to rebind its path variable");
						}
						row.property_cache.erase(namedPath.getPathVariable());
						row.complete_entity_property_cache.erase(namedPath.getPathVariable());
						return visitor(std::move(row));
					};
					if (!state.shared.allow_cross_row_entity_batches) {
						return visitOperator(requireOnlyChild(namedPath), state, [&](LogicalPlanReadRow row) {
							return processNamedPathRow(std::move(row),
													   MaterializedByteReservation(&state.shared.materialized_budget));
						});
					}
					const size_t batchCapacity = std::min(DEFAULT_PROPERTY_BATCH_ROWS, state.shared.bounds.maximum_rows);
					if (batchCapacity == 0U) {
						throw std::runtime_error("logical-plan NamedPath has no positive batch capacity");
					}
					BoundedBatch<LogicalPlanReadRow> rows(
							batchCapacity, state.shared.bounds.maximum_materialized_bytes,
							[&state](const LogicalPlanReadRow &row) { return logicalReadRowBytes(row, state.shared.context); },
							&state.shared.materialized_budget,
                            state.shared.context && state.shared.context->resource_account, sizeof(LogicalPlanReadRow));
					rows.reserve(batchCapacity);
					const auto flushNamedPathRows = [&]() {
						if (rows.empty()) {
							return true;
						}
						prefetchEntityRefsInRows(rows.values(), state, true);
						std::reverse(rows.values().begin(), rows.values().end());
						while (!rows.empty()) {
							auto charged = rows.popBackCharged();
							if (!processNamedPathRow(std::move(charged.value), std::move(charged.reservation))) {
								rows.clear();
								return false;
							}
						}
						rows.clear();
						return true;
					};
					const bool childCompleted =
							visitOperator(requireOnlyChild(namedPath), state, [&](LogicalPlanReadRow row) {
								rows.append(std::move(row));
								return rows.size() != batchCapacity || flushNamedPathRows();
							});
					if (!flushNamedPathRows()) {
						return false;
					}
					return childCompleted;
				}
				case logical::LogicalOpType::LOP_VAR_LENGTH_TRAVERSAL: {
                    debug::PerfTrace::addValue("logical_plan.consumer_adapter.variable_expand.calls", 1);
					const auto &traversal = dynamic_cast<const logical::LogicalVarLengthTraversal &>(op);
					if (traversal.getSourceVar().empty() || !traversal.getEdgeVar().empty() ||
						traversal.getMinHops() < 0 || traversal.getMaxHops() < traversal.getMinHops()) {
						throw std::invalid_argument(
								"logical-plan variable-length Traversal has an unsupported or invalid shape");
					}
					const std::string &direction = traversal.getDirection();
					if (direction != "out" && direction != "in" && direction != "both") {
						throw std::invalid_argument("logical-plan variable-length Traversal has an unknown direction");
					}
					int maximumHops = traversal.getMaxHops();
					if (state.shared.context != nullptr && state.shared.context->maxVarLengthDepth.has_value()) {
						if (*state.shared.context->maxVarLengthDepth < 0) {
							throw std::invalid_argument(
									"logical-plan variable-length Traversal has a negative query depth bound");
						}
						maximumHops = std::min(maximumHops, *state.shared.context->maxVarLengthDepth);
					}
					if (static_cast<uint64_t>(maximumHops) > state.shared.bounds.maximum_node_records) {
						throw std::runtime_error(
								"logical-plan variable-length Traversal exceeds its configured depth bound");
					}
					if (maximumHops != 0 &&
						state.shared.bounds.maximum_edges_per_node >
								std::numeric_limits<size_t>::max() / static_cast<size_t>(maximumHops)) {
						throw std::overflow_error("logical-plan variable-length Traversal edge budget overflowed");
					}
					storage::EntityReadShape targetReadShape;
					targetReadShape.include_labels = !traversal.getTargetLabels().empty();
					for (const auto &[key, expected]: traversal.getTargetProperties()) {
						static_cast<void>(expected);
						if (key.empty())
							throw std::invalid_argument("logical-plan Property predicate has an empty key");
						const auto token = state.shared.snapshot.findPropertyToken(key);
						if (!token) {
							targetReadShape.all_properties = true;
						} else {
							targetReadShape.property_tokens.push_back(*token);
						}
					}
					if (targetReadShape.all_properties)
						targetReadShape.property_tokens.clear();
					std::ranges::sort(targetReadShape.property_tokens);
					targetReadShape.property_tokens.erase(
							std::unique(targetReadShape.property_tokens.begin(), targetReadShape.property_tokens.end()),
							targetReadShape.property_tokens.end());
					validateEntityReadShape(targetReadShape);
					// Name columns are also valid for staged/absent keys without a catalog
					// token. Missing and stored NULL both fail non-null equality predicates.
					// Nullable values additionally require a cheap presence probe.
					const bool useNamedPredicateColumns = targetReadShape.all_properties &&
						state.shared.snapshot.supportsPropertyReadAdmission() &&
						(state.shared.snapshot.supportsPropertyPresenceProbes() ||
						 !std::ranges::any_of(traversal.getTargetProperties(),
							[](const auto &predicate) { return isNull(predicate.second); }));
					const auto readNamedPredicateNodes = [&](const std::vector<uint64_t> &ids,
						MaterializedByteReservation &retainedTargets) {
						std::vector<std::string_view> keys;
						MaterializedByteReservation columnsBytes(&state.shared.materialized_budget);
						columnsBytes.set(checkedSizeMul(traversal.getTargetProperties().size(), sizeof(std::string_view),
							"traversal named key slots overflowed"));
						keys.reserve(traversal.getTargetProperties().size());
						for (const auto &[key, expected]: traversal.getTargetProperties()) {
							static_cast<void>(expected);
							if (std::ranges::find(keys, key) == keys.end()) keys.push_back(key);
						}
						if (keys.size() > state.shared.bounds.maximum_property_entries)
							throw std::runtime_error("traversal named columns exceeded their Property bound");
						auto nodes = readNodesShapedSortedBudgeted(state, ids,
							{.include_labels = targetReadShape.include_labels}, &retainedTargets);
						if (nodes.size() != ids.size()) throw std::runtime_error("traversal named targets changed cardinality");
						retainShapedBatchBytes(nodes, retainedTargets, state.shared.context);
						const size_t frames = checkedSizeAdd(checkedSizeMul(keys.capacity(), sizeof(std::string_view),
							"traversal named key capacity overflowed"), checkedSizeMul(keys.size(),
								checkedSizeAdd(sizeof(std::vector<PropertyValue>), checkedSizeMul(ids.size(), sizeof(PropertyValue),
									"traversal named column slots overflowed"), "traversal named column frames overflowed"),
								"traversal named column frames overflowed"), "traversal named frames overflowed");
						columnsBytes.set(frames);
						auto columns = state.shared.snapshot.readNodePropertyColumnsSortedWithAdmission(ids, keys, [&](size_t bytes) {
							checkGuard(state);
							columnsBytes.set(checkedSizeAdd(columnsBytes.bytes(), bytes, "traversal named source overflowed"));
						});
						if (columns.size() != keys.size()) throw std::runtime_error("traversal named columns changed cardinality");
						for (const auto &column: columns)
							if (column.size() != ids.size()) throw std::runtime_error("traversal named column changed row count");
						columnsBytes.set(checkedSizeAdd(keys.capacity() * sizeof(std::string_view),
							propertyColumnBytes(columns, keys, "traversal named columns overflowed", state.shared.context),
							"traversal named columns overflowed"));
						for (size_t column = 0U; column < keys.size(); ++column) for (size_t row = 0U; row < ids.size(); ++row) {
							checkGuard(state);
							validateNode(nodes[row], state);
							if (nodes[row].node_id != ids[row]) throw std::runtime_error("traversal named target changed identity");
							if (isNull(columns[column][row])) {
								const bool nullable = std::ranges::any_of(traversal.getTargetProperties(),
									[&](const auto &predicate) { return predicate.first == keys[column] && isNull(predicate.second); });
								if (!nullable) continue;
								const auto present = state.shared.snapshot.nodePropertyExists(ids[row], keys[column]);
								if (!present) throw std::runtime_error("traversal Property presence probe is unavailable");
								if (!*present) continue;
							}
							const size_t payload = detail::retainedPropertyValueBytes(columns[column][row], [&] { checkGuard(state); });
							columnsBytes.set(columnsBytes.bytes() - payload);
							retainedTargets.set(checkedSizeAdd(retainedTargets.bytes(),
								checkedSizeAdd(payload, sizeof(storage::LogicalGraphPropertyEntry) + keys[column].size(),
									"traversal named entry overflowed"), "traversal named handoff overflowed"));
							nodes[row].properties.push_back({std::string(keys[column]), std::move(columns[column][row])});
							retainShapedBatchBytes(nodes, retainedTargets, state.shared.context);
						}
						return nodes;
					};
					const auto readPredicateNode = [&](uint64_t id, MaterializedByteReservation &retainedTarget) {
						checkGuard(state);
						if (useNamedPredicateColumns) {
							auto nodes = readNamedPredicateNodes(std::vector<uint64_t>{id}, retainedTarget);
							return std::move(nodes.front());
						}
						auto node = state.shared.snapshot.readNodeShaped(id, targetReadShape);
						validateNode(node, state);
						if (node.node_id != id)
							throw std::runtime_error("logical graph traversal changed target identity");
						if (node.properties.size() > state.shared.bounds.maximum_property_entries ||
							logicalNodeCacheBytes(node, state.shared.context) > state.shared.bounds.maximum_materialized_bytes) {
							throw std::runtime_error(
									"logical-plan traversal predicate exceeded its materialized bound");
						}
						const size_t targetBytes = logicalNodeCacheBytes(node, state.shared.context);
						if (retainedTarget.wouldExceed(targetBytes)) {
							throw std::runtime_error(
									"logical-plan traversal predicate target exceeded its shared byte bound");
						}
						retainedTarget.set(targetBytes);
						return node;
					};
					const auto readPredicateNodes = [&](const std::vector<uint64_t> &ids,
														MaterializedByteReservation &retainedTargets) {
						checkGuard(state);
						if (ids.size() > state.shared.bounds.maximum_node_records) {
							throw std::runtime_error("logical-plan traversal predicate batch exceeded its Node bound");
						}
						if (useNamedPredicateColumns && !ids.empty()) return readNamedPredicateNodes(ids, retainedTargets);
						auto nodes = ids.empty() ? std::vector<storage::LogicalGraphNode>{}
												 : readNodesShapedSortedBudgeted(state, ids, targetReadShape,
																				 &retainedTargets);
						if (nodes.size() != ids.size()) {
							throw std::runtime_error("logical graph traversal changed target batch cardinality");
						}
						size_t bytes = checkedSizeMul(ids.capacity(), sizeof(uint64_t),
													  "logical-plan traversal predicate batch size overflowed");
						for (size_t index = 0U; index < nodes.size(); ++index) {
							checkGuard(state);
							validateNode(nodes[index], state);
							if (nodes[index].node_id != ids[index]) {
								throw std::runtime_error("logical graph traversal changed target batch identity");
							}
							bytes = checkedSizeAdd(bytes, logicalNodeCacheBytes(nodes[index], state.shared.context),
												   "logical-plan traversal predicate batch size overflowed");
							if (nodes[index].properties.size() > state.shared.bounds.maximum_property_entries ||
								bytes > state.shared.bounds.maximum_materialized_bytes) {
								throw std::runtime_error(
										"logical-plan traversal predicate batch exceeded its materialized bound");
							}
						}
						retainShapedBatchBytes(nodes, retainedTargets, state.shared.context);
						return nodes;
					};
					const bool batchPredicateTargets =
							state.shared.allow_cross_row_entity_batches &&
							(!traversal.getTargetLabels().empty() || !traversal.getTargetProperties().empty());
					const size_t maximumEdgeVisits =
							maximumHops == 0 ? 0U
											 : state.shared.bounds.maximum_edges_per_node * static_cast<size_t>(maximumHops);
					// An exact target lookup can build one reverse reachability envelope for
					// the whole operator. The forward DFS still owns path semantics; this
					// index-driven envelope only removes branches that cannot reach a target.
					bool reversePruningEnabled = false;
					std::unordered_map<uint64_t, int> reverseDistance;
					MaterializedByteReservation reverseEnvelopeBytes(&state.shared.materialized_budget);
					if (state.shared.snapshot.supportsNativeBatchAdjacentEdgeRefs() &&
						!traversal.getTargetProperties().empty()) {
						// Reverse pruning needs only seed IDs. Native candidates already validate
						// the target predicates; avoid hydrating unrelated target Properties.
						const auto readCandidateTargets = [&]() -> std::optional<std::vector<storage::LogicalGraphNode>> {
							if (!state.shared.snapshot.supportsNativeExactNodeCandidates()) return std::nullopt;
							std::vector<storage::LogicalGraphNode> targets;
							MaterializedByteReservation targetSlots(&state.shared.materialized_budget);
							bool verified = true;
							const size_t sourceBytes = std::min(state.shared.bounds.maximum_candidate_bytes,
									std::max<size_t>(1U, state.shared.materialized_budget.available() / 2U));
							const auto streamed = state.shared.snapshot.visitNodeCandidatesByExactProperties(
									traversal.getTargetLabels(), traversal.getTargetProperties(),
									state.shared.bounds.maximum_node_records, state.shared.bounds.maximum_property_entries, sourceBytes,
									[&](const storage::IndexCandidateBatch &batch) {
										checkGuard(state);
										if (batch.snapshot_generation == 0U || state.shared.snapshot.nativeSnapshotGeneration() != batch.snapshot_generation)
											throw std::runtime_error("reverse target candidates do not belong to the pinned snapshot");
										if (!batch.exact_match_verified || !batch.predicates_verified || !batch.label_scope_verified) { verified = false; return false; }
										const size_t bytes = logicalIndexCandidateBatchBytes(batch, state.shared.context);
										if (bytes > sourceBytes) throw std::runtime_error("reverse target candidate batch exceeded its byte bound");
										MaterializedByteReservation candidateSlots(&state.shared.materialized_budget);
										candidateSlots.set(bytes);
										if (batch.candidates.size() > state.shared.bounds.maximum_node_records - targets.size())
											throw std::runtime_error("reverse target candidates exceeded the Node bound");
										const size_t required = targets.size() + batch.candidates.size();
										if (required > targets.capacity()) {
											const size_t grown = targets.capacity() > state.shared.bounds.maximum_node_records / 2U
													? state.shared.bounds.maximum_node_records : targets.capacity() * 2U;
											const size_t capacity = std::max(required, grown);
											targetSlots.set(checkedSizeMul(checkedSizeAdd(targets.capacity(), capacity,
													"reverse target capacity overflowed"), sizeof(storage::LogicalGraphNode), "reverse target slots overflowed"));
											targets.reserve(capacity);
											targetSlots.set(checkedSizeMul(targets.capacity(), sizeof(storage::LogicalGraphNode), "reverse target slots overflowed"));
										}
										for (const auto &reference: batch.candidates) {
											validateNodeRef(reference, "reverse target candidates contain an invalid Node reference");
											storage::LogicalGraphNode target; target.node_id = reference.entity_id;
											targets.push_back(std::move(target));
										}
										debug::PerfTrace::addValue("logical_plan.reverse_target_candidate_rows", static_cast<int64_t>(batch.candidates.size()));
										return true;
									});
							if (!streamed.has_value() || !verified) return std::nullopt;
							if (!*streamed) throw std::runtime_error("reverse target candidate stream stopped before completion");
							return targets;
						};
						auto targetNodes = readCandidateTargets();
						if (!targetNodes.has_value()) {
							targetNodes = state.shared.snapshot.findNodesByExactProperties(
									traversal.getTargetLabels(), traversal.getTargetProperties(),
									state.shared.bounds.maximum_node_records, state.shared.bounds.maximum_property_entries);
						}
						if (targetNodes.has_value()) {
							reversePruningEnabled = true;
							if (targetNodes->size() > state.shared.bounds.maximum_node_records) {
								throw std::runtime_error(
										"logical graph exact target lookup exceeded the variable-length Node bound");
							}
							size_t targetBytes =
									checkedSizeMul(targetNodes->capacity(), sizeof(storage::LogicalGraphNode),
												   "logical-plan reverse reachability target storage overflowed");
							for (const storage::LogicalGraphNode &target: *targetNodes) {
								targetBytes =
										checkedSizeAdd(targetBytes, logicalNodeCacheBytes(target, state.shared.context),
													   "logical-plan reverse reachability target storage overflowed");
							}
							const auto reverseMapBytes = [](size_t entries) {
								// Account each hash node plus conservative link and bucket-pointer
								// overhead so the shared byte limit covers the retained envelope.
								return checkedSizeMul(entries,
													  sizeof(std::pair<const uint64_t, int>) + 3U * sizeof(void *),
													  "logical-plan reverse reachability map size overflowed");
							};
							std::vector<uint64_t> frontier;
							const size_t initialFrontierBytes =
									checkedSizeMul(targetNodes->size(), sizeof(uint64_t),
												   "logical-plan reverse reachability frontier size overflowed");
							reverseEnvelopeBytes.set(checkedSizeAdd(
									targetBytes,
									checkedSizeAdd(reverseMapBytes(targetNodes->size()), initialFrontierBytes,
												   "logical-plan reverse reachability storage overflowed"),
									"logical-plan reverse reachability storage overflowed"));
							frontier.reserve(targetNodes->size());
							reverseDistance.reserve(targetNodes->size());
							reverseEnvelopeBytes.set(checkedSizeAdd(
									targetBytes,
									checkedSizeAdd(
											reverseMapBytes(targetNodes->size()),
											checkedSizeMul(
													frontier.capacity(), sizeof(uint64_t),
													"logical-plan reverse reachability frontier size overflowed"),
											"logical-plan reverse reachability storage overflowed"),
									"logical-plan reverse reachability storage overflowed"));
							for (const storage::LogicalGraphNode &target: *targetNodes) {
								validateNode(target, state);
								if (!reverseDistance.emplace(target.node_id, 0).second) {
									throw std::runtime_error(
											"logical graph exact target lookup returned a duplicate Node");
								}
								frontier.push_back(target.node_id);
							}
							std::ranges::sort(frontier);
							const std::optional<uint64_t> reverseTypeToken =
									traversal.getEdgeType().empty()
											? std::nullopt
											: state.shared.snapshot.findRelationshipTypeToken(traversal.getEdgeType());
							size_t reverseEdgeVisits = 0U;
							const auto reverseDirection = [&](const std::span<const uint64_t> currentFrontier,
															  const storage::LogicalGraphReadDirection
																	  adjacencyDirection,
															  std::vector<uint64_t> &nextFrontier) {
								if (reverseEdgeVisits == state.shared.bounds.maximum_edge_records) {
									throw std::runtime_error(
											"logical-plan reverse reachability exceeded its Edge bound");
								}
								const size_t remaining = state.shared.bounds.maximum_edge_records - reverseEdgeVisits;
								const size_t edgeVisitBound = checkedSizeAdd(
										remaining, 1U, "logical-plan reverse reachability Edge bound overflowed");
								state.shared.snapshot.visitAdjacentEdgeRefsForNodes(
										currentFrontier, adjacencyDirection, edgeVisitBound,
										[&](const storage::EntityRef &reference) {
											checkGuard(state);
											validateEdgeRef(reference, "logical graph reverse reachability returned an "
																	   "invalid or non-Edge reference");
											if (reverseEdgeVisits == state.shared.bounds.maximum_edge_records) {
												throw std::runtime_error(
														"logical-plan reverse reachability exceeded its Edge bound");
											}
											++reverseEdgeVisits;
											incrementBounded(
													state.task.stats.edges_scanned,
													"logical-plan reverse reachability Edge counter overflowed");
											if (reverseTypeToken.has_value() &&
												reference.type_token != *reverseTypeToken) {
												return true;
											}
											const uint64_t currentId =
													adjacencyDirection ==
																	storage::LogicalGraphReadDirection::LGRD_INCOMING
															? reference.target_node_id
															: reference.source_node_id;
											const uint64_t predecessor =
													adjacencyDirection ==
																	storage::LogicalGraphReadDirection::LGRD_INCOMING
															? reference.source_node_id
															: reference.target_node_id;
											if (!std::ranges::binary_search(currentFrontier, currentId) ||
												predecessor == 0U) {
												throw std::runtime_error("logical graph reverse reachability returned "
																		 "an inconsistent endpoint");
											}
											const auto current = reverseDistance.find(currentId);
											if (current == reverseDistance.end() || current->second >= maximumHops) {
												return true;
											}
											if (!reverseDistance.contains(predecessor)) {
												const size_t nextSize = checkedSizeAdd(
														nextFrontier.size(), 1U,
														"logical-plan reverse reachability frontier size overflowed");
												const size_t nextCapacity =
														nextSize > nextFrontier.capacity()
																? std::max(nextSize,
																		   nextFrontier.capacity() == 0U
																				   ? 1U
																				   : checkedSizeMul(
																							 nextFrontier.capacity(),
																							 2U,
																							 "logical-plan reverse "
																							 "reachability frontier "
																							 "size overflowed"))
																: nextFrontier.capacity();
												const size_t nextBytes = checkedSizeAdd(
														targetBytes,
														checkedSizeAdd(
																reverseMapBytes(checkedSizeAdd(
																		reverseDistance.size(), 1U,
																		"logical-plan reverse reachability map size "
																		"overflowed")),
																checkedSizeMul(
																		checkedSizeAdd(
																				frontier.capacity(), nextCapacity,
																				"logical-plan reverse reachability "
																				"frontier size overflowed"),
																		sizeof(uint64_t),
																		"logical-plan reverse reachability frontier "
																		"size overflowed"),
																"logical-plan reverse reachability storage overflowed"),
														"logical-plan reverse reachability storage overflowed");
												reverseEnvelopeBytes.set(nextBytes);
												if (nextCapacity > nextFrontier.capacity())
													nextFrontier.reserve(nextCapacity);
												reverseDistance.emplace(predecessor, current->second + 1);
												nextFrontier.push_back(predecessor);
											}
											return true;
										});
							};
							const bool canExpandReverse =
									traversal.getEdgeType().empty() || reverseTypeToken.has_value();
							for (int depth = 0; canExpandReverse && depth < maximumHops && !frontier.empty(); ++depth) {
								std::vector<uint64_t> nextFrontier;
								if (direction == "out" || direction == "both") {
									reverseDirection(frontier, storage::LogicalGraphReadDirection::LGRD_INCOMING,
													 nextFrontier);
								}
								if (direction == "in" || direction == "both") {
									reverseDirection(frontier, storage::LogicalGraphReadDirection::LGRD_OUTGOING,
													 nextFrontier);
								}
								std::ranges::sort(nextFrontier);
								nextFrontier.erase(std::unique(nextFrontier.begin(), nextFrontier.end()),
												   nextFrontier.end());
								frontier = std::move(nextFrontier);
								reverseEnvelopeBytes.set(checkedSizeAdd(
										targetBytes,
										checkedSizeAdd(
												reverseMapBytes(reverseDistance.size()),
												checkedSizeMul(
														frontier.capacity(), sizeof(uint64_t),
														"logical-plan reverse reachability frontier size overflowed"),
												"logical-plan reverse reachability storage overflowed"),
										"logical-plan reverse reachability storage overflowed"));
							}
							std::vector<storage::LogicalGraphNode>().swap(*targetNodes);
							reverseEnvelopeBytes.set(checkedSizeAdd(
									reverseMapBytes(reverseDistance.size()),
									checkedSizeMul(frontier.capacity(), sizeof(uint64_t),
												   "logical-plan reverse reachability frontier size overflowed"),
									"logical-plan reverse reachability storage overflowed"));
						}
					}
					if (reversePruningEnabled && reverseDistance.empty()) {
						return true;
					}

					return visitOperator(requireOnlyChild(traversal), state, [&](LogicalPlanReadRow row) {
						const LogicalPlanReadValue &sourceBinding = requireBinding(row, traversal.getSourceVar());
						const uint64_t sourceId = nodeBindingId(sourceBinding, state);
						if (reversePruningEnabled) {
							const auto reachable = reverseDistance.find(sourceId);
							if (reachable == reverseDistance.end() || reachable->second > maximumHops) {
								return true;
							}
						}
						const bool sourceNeedsPredicate =
								traversal.getMinHops() == 0 &&
								(!traversal.getTargetLabels().empty() || !traversal.getTargetProperties().empty());
						MaterializedByteReservation sourcePredicateBytes(&state.shared.materialized_budget);
						const storage::LogicalGraphNode source =
								sourceNeedsPredicate ? readPredicateNode(sourceId, sourcePredicateBytes)
													 : patternSourceIdentity(state, sourceBinding);
						detail::BudgetMemoryResource pathIdentityMemory(state.shared.materialized_budget);
						std::pmr::unordered_set<uint64_t> pathEdges(&pathIdentityMemory);
						std::vector<storage::LogicalGraphNode> pathNodeRecords{source};
						// A variable-length topology traversal does not need a complete
						// endpoint Node when no target label/Property predicate exists. Keep
						// an aligned reference marker so output and NamedPath can materialize
						// the endpoint only at their respective boundaries.
						std::vector<storage::EntityRef> pathNodeRefs;
						pathNodeRefs.push_back({sourceId, false, 0U, 0U, 0U});
						std::vector<storage::LogicalGraphEdge> pathEdgeRecords;
						std::vector<storage::EntityRef> pathEdgeRefs;
						size_t edgeVisits = 0U;
						struct AdjacentEdge {
							storage::LogicalGraphEdgeTopology topology;
							std::optional<storage::EntityRef> reference;
						};

						const auto visitAdjacent = [&](uint64_t nodeId,
													   const std::function<bool(const AdjacentEdge &,
																				const storage::LogicalGraphNode &)>
															   &onEdge) {
							std::optional<uint64_t> relationshipTypeToken;
							const bool useNativeAdjacencyReferences =
									state.shared.snapshot.supportsNativeAdjacentEdgeRefScans() &&
									(traversal.getEdgeType().empty() ||
									 state.shared.snapshot.supportsNativeRelationshipTypeTokens(traversal.getEdgeType()));
							if (useNativeAdjacencyReferences && !traversal.getEdgeType().empty()) {
								relationshipTypeToken =
										state.shared.snapshot.findRelationshipTypeToken(traversal.getEdgeType());
								if (!relationshipTypeToken.has_value()) {
									return true;
								}
							}
							const auto visitDirection = [&](storage::LogicalGraphReadDirection adjacencyDirection) {
								std::optional<storage::LogicalGraphAdjacencyContinuation> continuation;
								uint64_t previousEdgeId = 0U;
								if (useNativeAdjacencyReferences) {
									for (;;) {
										if (edgeVisits == maximumEdgeVisits) {
											throw std::runtime_error("logical-plan variable-length Traversal exceeded "
																	 "its edge exploration bound");
										}
										const size_t pageRecords =
												std::min(maximumEdgeVisits - edgeVisits,
														 state.shared.bounds.maximum_adjacency_page_records);
										MaterializedByteReservation referenceBytes(&state.shared.materialized_budget);
										const auto page = readReferencePageBudgeted(state, nodeId, adjacencyDirection, continuation, pageRecords, referenceBytes);
										if (page.edges.size() > pageRecords ||
											(page.edges.empty() && page.continuation.has_value()) ||
											(continuation.has_value() &&
											 (page.edges.empty() ||
											  page.edges.front().entity_id != continuation->next_edge_id))) {
											throw std::runtime_error("logical graph backend returned an invalid "
																	 "variable-length adjacency reference page");
										}
										const bool deferPageTargets = traversal.getTargetLabels().empty() &&
																	  traversal.getTargetProperties().empty() &&
																	  state.shared.snapshot.supportsNodeExistenceProbes();
										MaterializedByteReservation pageEndpointBytes(&state.shared.materialized_budget);
										std::vector<uint64_t> pageEndpointIds;
										if (deferPageTargets || batchPredicateTargets) {
											pageEndpointBytes.set(checkedSizeMul(
													page.edges.size(), sizeof(uint64_t),
													"logical-plan traversal predicate endpoint slots overflowed"));
											pageEndpointIds.reserve(page.edges.size());
											pageEndpointBytes.set(checkedSizeMul(
													pageEndpointIds.capacity(), sizeof(uint64_t),
													"logical-plan traversal predicate endpoint capacity overflowed"));
											for (const storage::EntityRef &candidate: page.edges) {
												validateEdgeRef(candidate, "logical graph backend returned an invalid "
																		   "variable-length adjacency reference");
												if (direction == "both" &&
													adjacencyDirection ==
															storage::LogicalGraphReadDirection::LGRD_INCOMING &&
													candidate.source_node_id == nodeId) {
													continue;
												}
												if (relationshipTypeToken.has_value() &&
													candidate.type_token != *relationshipTypeToken) {
													continue;
												}
												pageEndpointIds.push_back(
														adjacencyDirection == storage::LogicalGraphReadDirection::
																					  LGRD_OUTGOING
																? candidate.target_node_id
																: candidate.source_node_id);
											}
											std::ranges::sort(pageEndpointIds);
											pageEndpointIds.erase(
													std::unique(pageEndpointIds.begin(), pageEndpointIds.end()),
													pageEndpointIds.end());
										}
										const std::vector<bool> pageEndpointsPresent =
												deferPageTargets ? state.shared.snapshot.containsNodesSorted(pageEndpointIds)
																 : std::vector<bool>{};
										MaterializedByteReservation predicateTargetBytes(&state.shared.materialized_budget);
										const auto predicateTargets =
												batchPredicateTargets
														? readPredicateNodes(pageEndpointIds, predicateTargetBytes)
														: std::vector<storage::LogicalGraphNode>{};
										const auto predicateTarget =
												[&](uint64_t id) -> const storage::LogicalGraphNode & {
											const auto found = std::ranges::lower_bound(
													predicateTargets, id, {}, &storage::LogicalGraphNode::node_id);
											if (found == predicateTargets.end() || found->node_id != id) {
												throw std::runtime_error(
														"logical graph traversal lost a batched target");
											}
											return *found;
										};
										if (deferPageTargets && pageEndpointsPresent.size() != pageEndpointIds.size()) {
											throw std::runtime_error("logical graph backend changed variable-length "
																	 "endpoint cardinality");
										}
										for (const storage::EntityRef &reference: page.edges) {
											checkGuard(state);
											validateEdgeRef(reference, "logical graph backend returned an invalid "
																	   "variable-length adjacency reference");
											if (reference.entity_id <= previousEdgeId) {
												throw std::runtime_error("logical graph backend returned an invalid "
																		 "variable-length adjacency reference");
											}
											previousEdgeId = reference.entity_id;
											const bool endpointMatches =
													adjacencyDirection ==
																	storage::LogicalGraphReadDirection::LGRD_OUTGOING
															? reference.source_node_id == nodeId
															: reference.target_node_id == nodeId;
											if (!endpointMatches) {
												throw std::runtime_error("logical graph backend returned an invalid "
																		 "variable-length Edge reference");
											}
											++edgeVisits;
											incrementBounded(state.task.stats.edges_scanned,
															 "logical-plan variable-length Edge counter overflowed");
											if (direction == "both" &&
												adjacencyDirection ==
														storage::LogicalGraphReadDirection::LGRD_INCOMING &&
												reference.source_node_id == nodeId) {
												continue;
											}
											if (relationshipTypeToken.has_value() &&
												reference.type_token != *relationshipTypeToken) {
												continue;
											}
											const uint64_t targetId =
													adjacencyDirection ==
																	storage::LogicalGraphReadDirection::LGRD_OUTGOING
															? reference.target_node_id
															: reference.source_node_id;
											const bool deferTargetNode = traversal.getTargetLabels().empty() &&
																		 traversal.getTargetProperties().empty();
											if (deferTargetNode) {
												if (state.shared.snapshot.supportsNodeExistenceProbes()) {
													const auto endpoint =
															std::ranges::lower_bound(pageEndpointIds, targetId);
													if (endpoint == pageEndpointIds.end() || *endpoint != targetId ||
														!pageEndpointsPresent[static_cast<size_t>(
																endpoint - pageEndpointIds.begin())]) {
														throw std::runtime_error(
																"logical graph backend returned a variable-length Edge "
																"with a missing endpoint");
													}
												} else {
													// Authenticate the endpoint with an identity read when no
													// existence probe is available; its Properties are not needed.
													validateNodeIdentity(state, targetId);
												}
											}
											MaterializedByteReservation localTargetBytes(&state.shared.materialized_budget);
											std::optional<storage::LogicalGraphNode> localTarget;
											const storage::LogicalGraphNode &target =
													deferTargetNode			? localTarget.emplace(
																					  storage::LogicalGraphNode{targetId, {}})
													: batchPredicateTargets ? predicateTarget(targetId)
																			: localTarget.emplace(readPredicateNode(
																					  targetId, localTargetBytes));
											const storage::LogicalGraphEdgeTopology topology{reference.entity_id,
																							 reference.source_node_id,
																							 reference.target_node_id,
																							 {}};
											if (!onEdge({topology, reference}, target)) {
												return false;
											}
										}
										if (!page.continuation.has_value()) {
											return true;
										}
										if (page.edges.empty() ||
											page.continuation->previous_edge_id != page.edges.back().entity_id ||
											page.continuation->next_edge_id == 0U ||
											page.continuation->previous_edge_id == page.continuation->next_edge_id) {
											throw std::runtime_error(
													"logical graph backend returned an invalid variable-length "
													"adjacency reference continuation");
										}
										continuation = page.continuation;
									}
								}
								for (;;) {
									if (edgeVisits == maximumEdgeVisits) {
										throw std::runtime_error("logical-plan variable-length Traversal exceeded its "
																 "edge exploration bound");
									}
									const size_t remaining = maximumEdgeVisits - edgeVisits;
									const size_t pageRecords =
											std::min(remaining, state.shared.bounds.maximum_adjacency_page_records);
									// Variable-length traversal never binds an Edge value or reads Edge
									// Properties. Use the topology cursor even when reference pages are
									// unavailable; target Nodes remain materialized only when predicates
									// require them.
									MaterializedByteReservation topologyBytes(&state.shared.materialized_budget);
									const auto page = readTopologyPageBudgeted(state, nodeId, adjacencyDirection,
										continuation, pageRecords, topologyBytes);
									if (page.edges.size() > pageRecords ||
										(page.edges.empty() && page.continuation.has_value())) {
										throw std::runtime_error("logical graph backend returned an invalid "
																 "variable-length adjacency page");
									}
									const bool deferPageTargets = traversal.getTargetLabels().empty() &&
																  traversal.getTargetProperties().empty() &&
																  state.shared.snapshot.supportsNodeExistenceProbes();
									MaterializedByteReservation pageEndpointBytes(&state.shared.materialized_budget);
									std::vector<uint64_t> pageEndpointIds;
									if (deferPageTargets || batchPredicateTargets) {
										pageEndpointBytes.set(checkedSizeMul(
												page.edges.size(), sizeof(uint64_t),
												"logical-plan traversal predicate endpoint slots overflowed"));
										pageEndpointIds.reserve(page.edges.size());
										pageEndpointBytes.set(checkedSizeMul(
												pageEndpointIds.capacity(), sizeof(uint64_t),
												"logical-plan traversal predicate endpoint capacity overflowed"));
										for (const storage::LogicalGraphEdgeTopology &candidate: page.edges) {
											const bool selfLoopDuplicate =
													direction == "both" &&
													adjacencyDirection ==
															storage::LogicalGraphReadDirection::LGRD_INCOMING &&
													candidate.source_node_id == nodeId;
											if (!selfLoopDuplicate && (traversal.getEdgeType().empty() ||
																	   candidate.type == traversal.getEdgeType())) {
												pageEndpointIds.push_back(
														adjacencyDirection == storage::LogicalGraphReadDirection::
																					  LGRD_OUTGOING
																? candidate.target_node_id
																: candidate.source_node_id);
											}
										}
										std::ranges::sort(pageEndpointIds);
										pageEndpointIds.erase(
												std::unique(pageEndpointIds.begin(), pageEndpointIds.end()),
												pageEndpointIds.end());
									}
									const std::vector<bool> pageEndpointsPresent =
											deferPageTargets ? state.shared.snapshot.containsNodesSorted(pageEndpointIds)
															 : std::vector<bool>{};
									MaterializedByteReservation predicateTargetBytes(&state.shared.materialized_budget);
									const auto predicateTargets =
											batchPredicateTargets
													? readPredicateNodes(pageEndpointIds, predicateTargetBytes)
													: std::vector<storage::LogicalGraphNode>{};
									const auto predicateTarget = [&](uint64_t id) -> const storage::LogicalGraphNode & {
										const auto found = std::ranges::lower_bound(
												predicateTargets, id, {}, &storage::LogicalGraphNode::node_id);
										if (found == predicateTargets.end() || found->node_id != id) {
											throw std::runtime_error("logical graph traversal lost a batched target");
										}
										return *found;
									};
									if (deferPageTargets && pageEndpointsPresent.size() != pageEndpointIds.size()) {
										throw std::runtime_error(
												"logical graph backend changed variable-length endpoint cardinality");
									}
									for (const storage::LogicalGraphEdgeTopology &edge: page.edges) {
										checkGuard(state);
										validateEdge(edge);
										validateAdjacentEdgeOrder(edge.edge_id, previousEdgeId, "variable-length");
										const bool endpointMatches =
												adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
														? edge.source_node_id == nodeId
														: edge.target_node_id == nodeId;
										if (!endpointMatches) {
											throw std::runtime_error("logical graph backend returned an invalid "
																	 "variable-length adjacent Edge");
										}
										++edgeVisits;
										incrementBounded(state.task.stats.edges_scanned,
														 "logical-plan variable-length Edge counter overflowed");
										if (direction == "both" &&
											adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_INCOMING &&
											edge.source_node_id == nodeId) {
											continue;
										}
										if (!traversal.getEdgeType().empty() && edge.type != traversal.getEdgeType()) {
											continue;
										}
										const uint64_t targetId =
												adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
														? edge.target_node_id
														: edge.source_node_id;
										const bool deferTargetNode = traversal.getTargetLabels().empty() &&
																	 traversal.getTargetProperties().empty();
										if (deferTargetNode) {
											if (state.shared.snapshot.supportsNodeExistenceProbes()) {
												const auto endpoint =
														std::ranges::lower_bound(pageEndpointIds, targetId);
												if (endpoint == pageEndpointIds.end() || *endpoint != targetId ||
													!pageEndpointsPresent[static_cast<size_t>(
															endpoint - pageEndpointIds.begin())]) {
													throw std::runtime_error(
															"logical graph backend returned a variable-length Edge "
															"with a missing endpoint");
												}
											} else {
												validateNodeIdentity(state, targetId);
											}
										}
										MaterializedByteReservation localTargetBytes(&state.shared.materialized_budget);
										std::optional<storage::LogicalGraphNode> localTarget;
										const storage::LogicalGraphNode &target =
												deferTargetNode
														? localTarget.emplace(storage::LogicalGraphNode{targetId, {}})
												: batchPredicateTargets ? predicateTarget(targetId)
																		: localTarget.emplace(readPredicateNode(
																				  targetId, localTargetBytes));
										if (!onEdge({{edge.edge_id, edge.source_node_id, edge.target_node_id,
													  edge.type},
													 std::nullopt},
													target)) {
											return false;
										}
									}
									if (!page.continuation.has_value()) {
										return true;
									}
									if (page.continuation->previous_edge_id != page.edges.back().edge_id ||
										page.continuation->next_edge_id == 0U ||
										page.continuation->previous_edge_id == page.continuation->next_edge_id) {
										throw std::runtime_error("logical graph backend returned an invalid "
																 "variable-length adjacency continuation");
									}
									continuation = page.continuation;
								}
							};
							if ((direction == "out" || direction == "both") &&
								!visitDirection(storage::LogicalGraphReadDirection::LGRD_OUTGOING)) {
								return false;
							}
							if (direction == "in" || direction == "both") {
								return visitDirection(storage::LogicalGraphReadDirection::LGRD_INCOMING);
							}
							return true;
						};
						std::function<bool(const storage::LogicalGraphNode &, int)> visitPath;
						visitPath = [&](const storage::LogicalGraphNode &node, int depth) {
							checkGuard(state);
							if (depth >= traversal.getMinHops() && depth <= maximumHops &&
								nodeMatches(state.shared.snapshot, node, traversal.getTargetLabels(),
											traversal.getTargetProperties(), state)) {
								const bool deferredNode = !pathNodeRefs.empty() && pathNodeRefs.back().entity_id != 0U;
								constexpr const char *outputMessage = "logical-plan path output size overflowed";
								size_t predictedBytes = logicalReadRowBytes(row, state.shared.context);
								predictedBytes = checkedSizeAdd(predictedBytes,
									checkedSizeMul(traversal.getTargetVar().size(), 2U, outputMessage), outputMessage);
								predictedBytes = checkedSizeAdd(predictedBytes, sizeof(LogicalPlanReadValue), outputMessage);
								predictedBytes = checkedSizeAdd(predictedBytes,
									deferredNode ? sizeof(storage::EntityRef) : logicalNodeCacheBytes(node, state.shared.context), outputMessage);
								for (const auto &record : pathNodeRecords)
									predictedBytes = checkedSizeAdd(predictedBytes, logicalNodeCacheBytes(record, state.shared.context), outputMessage);
								for (const auto &record : pathEdgeRecords)
									predictedBytes = checkedSizeAdd(predictedBytes, logicalEdgeCacheBytes(record, state.shared.context), outputMessage);
								predictedBytes = checkedSizeAdd(predictedBytes,
									checkedSizeMul(checkedSizeAdd(pathNodeRefs.size(), pathEdgeRefs.size(), outputMessage),
										sizeof(storage::EntityRef), outputMessage), outputMessage);
								MaterializedByteReservation outputReservation(&state.shared.materialized_budget);
								outputReservation.set(predictedBytes);
								LogicalPlanReadRow expanded = detail::copyLogicalReadRowGuarded(row, [&] { checkGuardNow(state); });
								LogicalPlanReadValue targetValue =
										deferredNode ? LogicalPlanReadValue(pathNodeRefs.back())
													 : LogicalPlanReadValue(detail::copyLogicalEntityGuarded(node, [&] { checkGuardNow(state); }));
								if (bindValue(expanded, traversal.getTargetVar(), std::move(targetValue))) {
									expanded.path_cache.insert_or_assign(
											traversal.getTargetVar(),
											detail::copyLogicalReadPathGuarded(pathNodeRecords, pathEdgeRecords, pathNodeRefs,
												pathEdgeRefs, [&] { checkGuardNow(state); }));
									outputReservation.set(logicalReadRowBytes(expanded, state.shared.context));
									bool keepGoing = true;
									if (chargedReceiver != nullptr) {
										keepGoing = (*chargedReceiver)(std::move(expanded), std::move(outputReservation));
									} else {
										outputReservation.reset();
										keepGoing = visitor(std::move(expanded));
									}
									if (!keepGoing) return false;
								}
							}
							if (depth == maximumHops) {
								return true;
							}
							return visitAdjacent(node.node_id, [&](const AdjacentEdge &edge,
																   const storage::LogicalGraphNode &target) {
								if (reversePruningEnabled) {
									const auto reachable = reverseDistance.find(target.node_id);
									if (reachable == reverseDistance.end() ||
										reachable->second > maximumHops - (depth + 1)) {
										return true;
									}
								}
								if (pathEdges.contains(edge.topology.edge_id)) {
									return true;
								}
								pathEdges.insert(edge.topology.edge_id);
								if (edge.reference.has_value()) {
									pathEdgeRefs.push_back(*edge.reference);
								} else {
									pathEdgeRecords.push_back({edge.topology.edge_id, edge.topology.source_node_id,
															   edge.topology.target_node_id, edge.topology.type});
								}
								pathNodeRecords.push_back(target);
								pathNodeRefs.push_back({target.node_id, false, 0U, 0U, 0U});
								const bool completed = visitPath(target, depth + 1);
								pathNodeRecords.pop_back();
								pathNodeRefs.pop_back();
								if (edge.reference.has_value()) {
									pathEdgeRefs.pop_back();
								} else {
									pathEdgeRecords.pop_back();
								}
								pathEdges.erase(edge.topology.edge_id);
								return completed;
							});
						};
						return visitPath(source, 0);
					});
				}
