		[[nodiscard]] QueryResult executeCreateSetMutationChain(
				const CreateSetMutationChain &chain, const storage::LogicalGraphReadBackend &readBackend,
				storage::LogicalGraphWriteBackend &writeBackend, const LogicalPlanReadRuntimeBounds &bounds,
				const QueryContext &context, const logical::LogicalProject *projection,
				const std::vector<std::string> &implicitColumns, const logical::LogicalForeach *foreach = nullptr,
				const std::vector<LogicalPlanReadRow> *injectedInputRows = nullptr,
				const logical::LogicalProject *exportedProjection = nullptr,
				const logical::LogicalSort *resultSort = nullptr, const logical::LogicalSkip *resultSkip = nullptr,
				const logical::LogicalLimit *resultLimit = nullptr) {
			if (chain.operations.empty()) { // every private call site supplies a chain accepted by collectCreateSetMutationChain or collectForeachMutationChain.
				throw std::logic_error("logical CREATE/SET mutation chain cannot be empty");
			}
			size_t createsPerInputRow = 0U;
			for (const logical::LogicalOperator *operation: chain.operations) {
				if (operation == nullptr) { // the chain collectors append only their non-null traversal cursor before following an optional child.
					throw std::logic_error("logical CREATE/SET mutation chain contains a null operator");
				}
				if (operation->getType() == logical::LogicalOpType::LOP_CREATE_NODE ||
					operation->getType() == logical::LogicalOpType::LOP_CREATE_EDGE) {
					++createsPerInputRow;
				}
			}
			if (createsPerInputRow > std::numeric_limits<size_t>::max() / bounds.maximum_rows) {
				throw std::overflow_error("logical CREATE/SET mutation chain exceeded its staged-create bound");
			}
			const size_t maximumStagedCreates = createsPerInputRow * bounds.maximum_rows;

			const auto appendExpressionProperties = [&](const auto &propertyExpressions,
														std::vector<LogicalGraphWriteProperty> &properties,
														const LogicalPlanReadRow &row,
														storage::LogicalGraphReadSnapshot &snapshot,
														std::unordered_map<std::string, PropertyMap> &propertyCache) {
				if (properties.size() > bounds.maximum_property_entries ||
					propertyExpressions.size() > bounds.maximum_property_entries - properties.size())
					throw std::runtime_error("logical CREATE expression Properties exceeded their entry bound");
				std::vector<std::pair<std::string, std::shared_ptr<expressions::Expression>>> ordered;
				ordered.reserve(propertyExpressions.size());
				for (const auto &[key, expression]: propertyExpressions) {
					ordered.emplace_back(key, expression);
				}
				std::ranges::sort(ordered, {},
								  &std::pair<std::string, std::shared_ptr<expressions::Expression>>::first);
				for (const auto &[key, expression]: ordered) {
					if (key.empty() || !expression ||
						std::ranges::any_of(properties, [&key](const LogicalGraphWriteProperty &property) {
							return property.key == key;
						})) {
						throw std::logic_error(
								"logical CREATE/SET has an invalid or duplicate Property expression key");
					}
					properties.push_back({key, evaluateCreatePropertyExpression(*expression, context, row, snapshot,
																				bounds, propertyCache)});
				}
			};

			const auto applyPropertySet = [&](PropertyMap &properties, const logical::LogicalSetItem &item,
											  const LogicalPlanReadRow &row,
											  storage::LogicalGraphReadSnapshot &snapshot,
											  std::unordered_map<std::string, PropertyMap> &propertyCache) {
				if (!item.expression) {
					throw std::logic_error("logical CREATE/SET has a Property assignment without an expression");
				}
				const PropertyValue value = evaluateCreatePropertyExpression(*item.expression, context, row, snapshot,
																			 bounds, propertyCache);
				switch (item.type) {
					case logical::SetActionType::LSET_PROPERTY: {
						if (item.key.empty()) {
							throw std::logic_error("logical CREATE/SET Property assignment has an empty key");
						}
						if (value.getType() == PropertyType::NULL_TYPE) {
							return properties.erase(item.key) != 0U;
						}
						const auto found = properties.find(item.key);
						if (found != properties.end() && found->second == value) {
							return false;
						}
						assignBoundedProperty(properties, item.key, value, bounds.maximum_property_entries);
						return true;
					}
					case logical::SetActionType::LSET_MAP_REPLACE:
						return replacePropertyMap(properties, value, context, bounds.maximum_property_entries);
					case logical::SetActionType::LSET_MAP_MERGE:
						return mergePropertyMap(properties, value, context, bounds.maximum_property_entries);
					case logical::SetActionType::LSET_LABEL:
						throw std::logic_error(
								"logical CREATE/SET label assignment must not evaluate a Property expression");
				}
				throw std::logic_error("logical CREATE/SET has an unsupported Property assignment");
			};

			constexpr size_t MAX_CONFLICT_RETRIES = 4U;
			for (size_t attempt = 0U; attempt < MAX_CONFLICT_RETRIES; ++attempt) {
				std::unique_ptr<storage::LogicalGraphReadSnapshot> snapshot =
						readBackend.openLogicalGraphReadSnapshot();
				if (!snapshot) {
					throw std::runtime_error("logical CREATE/SET could not open a read snapshot");
				}
				const std::optional<storage::LogicalGraphMutationFence> fence = snapshot->mutationFence();
				if (!fence.has_value() || !fence->isValid()) {
					throw std::logic_error(
							"logical CREATE/SET requires a backend that exposes an optimistic read fence");
				}

				LogicalGraphMutationBatch batch;
				MutableGraphEntities persistedEntities;
				std::vector<CreateSetRuntimeRow> projectionRows;
				std::set<size_t> cancelledNodeCreates;
				std::set<size_t> cancelledEdgeCreates;
				std::set<uint64_t> deletedNodeIds;
				std::set<uint64_t> deletedEdgeIds;
				std::map<uint64_t, bool> deletedNodeDetach;
				std::unique_ptr<logical::LogicalSingleRow> implicitSeed;
				const logical::LogicalOperator *readRoot = chain.read_root;
				if (readRoot == nullptr) {
					implicitSeed = std::make_unique<logical::LogicalSingleRow>();
					readRoot = implicitSeed.get();
				}

				LogicalPlanReadRuntime reader(readBackend);
				const auto entityReferencesForRow = [](const LogicalPlanReadRow &source) {
					std::unordered_map<std::string, LogicalGraphMutationEntityReference> entities;
					entities.reserve(source.values.size());
					for (const auto &[variable, value]: source.values) {
						if (const auto *node = std::get_if<storage::LogicalGraphNode>(&value)) {
							entities.emplace(variable,
											 LogicalGraphMutationEntityReference{
													 LogicalGraphMutationEntityKind::LGMK_NODE,
													 LogicalGraphMutationReferenceKind::LGRK_PERSISTED, node->node_id});
						} else if (const auto *edge = std::get_if<storage::LogicalGraphEdge>(&value)) {
							entities.emplace(variable,
											 LogicalGraphMutationEntityReference{
													 LogicalGraphMutationEntityKind::LGMK_EDGE,
													 LogicalGraphMutationReferenceKind::LGRK_PERSISTED, edge->edge_id});
						}
					}
					return entities;
				};
				const auto stageRow = [&](const LogicalPlanReadRow &source, bool retainOutputRow) {
					context.checkGuard();
					std::unordered_map<std::string, LogicalGraphMutationEntityReference> entities =
							entityReferencesForRow(source);
					entities.reserve(entities.size() + createsPerInputRow);
					std::unordered_map<std::string, PropertyMap> propertyCache = source.property_cache;
					// A repeated FOREACH input still carries its snapshot cache. Restore
					// all staged bindings before CREATE or expressions read another entity.
					for (const auto &[variable, reference] : entities) {
						context.checkGuard();
						if (reference.reference_kind != LogicalGraphMutationReferenceKind::LGRK_PERSISTED) continue;
						const auto staged = persistedEntities.find({static_cast<uint8_t>(reference.entity_kind), reference.value});
						if (staged != persistedEntities.end() && staged->second.properties)
							propertyCache.insert_or_assign(variable, *staged->second.properties);
					}

					const auto resolveNode = [&entities](const std::string &variable) {
						const auto found = entities.find(variable);
						if (variable.empty() || found == entities.end() ||
							found->second.entity_kind != LogicalGraphMutationEntityKind::LGMK_NODE) {
							throw std::logic_error("logical CREATE/SET cannot resolve a relationship Node endpoint");
						}
						return found->second;
					};

					for (const logical::LogicalOperator *operation: chain.operations) {
						context.checkGuard();
						if (const auto *node = dynamic_cast<const logical::LogicalCreateNode *>(operation);
							node != nullptr) {
							if (const auto existing = entities.find(node->getVariable()); existing != entities.end()) {
								if (node->getVariable().empty() ||
									existing->second.entity_kind != LogicalGraphMutationEntityKind::LGMK_NODE ||
									!node->getLabels().empty() || !node->getProperties().empty() ||
									!node->getPropertyExprs().empty()) {
									throw std::logic_error("logical CREATE/SET cannot redeclare a bound Node");
								}
								continue;
							}
							if (batch.node_creates.size() + batch.edge_creates.size() >= maximumStagedCreates) {
								throw std::runtime_error("logical CREATE/SET exceeded its staged mutation bound");
							}
							LogicalGraphNodeCreateRequest request;
							request.labels = node->getLabels();
							request.properties = toLogicalProperties(node->getProperties(), bounds.maximum_property_entries);
							appendExpressionProperties(node->getPropertyExprs(), request.properties, source, *snapshot,
													   propertyCache);
							const LogicalGraphMutationEntityReference reference =
									createdNodeReference(batch.node_creates.size());
							batch.node_creates.push_back(std::move(request));
							if (!node->getVariable().empty()) {
								entities.emplace(node->getVariable(), reference);
								propertyCache.insert_or_assign(
										node->getVariable(),
										propertyMapFromLogicalProperties(batch.node_creates.back().properties,
																		 "logical CREATE/SET Node creation"));
							}
							continue;
						}
						if (const auto *edge = dynamic_cast<const logical::LogicalCreateEdge *>(operation);
							edge != nullptr) {
							if (!edge->getVariable().empty() && entities.contains(edge->getVariable())) {
								throw std::logic_error("logical CREATE/SET has an invalid relationship declaration");
							}
							if (batch.node_creates.size() + batch.edge_creates.size() >= maximumStagedCreates) {
								throw std::runtime_error("logical CREATE/SET exceeded its staged mutation bound");
							}
							LogicalGraphBatchEdgeCreateRequest request;
							const std::string &direction = edge->getDirection();
							if (direction != "out" && direction != "in" && direction != "both") {
								throw std::logic_error("logical CREATE/SET has an unsupported relationship direction");
							}
							const LogicalGraphMutationEntityReference sourceReference =
									resolveNode(edge->getSourceVar());
							const LogicalGraphMutationEntityReference targetReference =
									resolveNode(edge->getTargetVar());
							request.source = direction == "in" ? targetReference : sourceReference;
							request.target = direction == "in" ? sourceReference : targetReference;
							request.type = edge->getEdgeType();
							request.properties = toLogicalProperties(edge->getProperties(), bounds.maximum_property_entries);
							appendExpressionProperties(edge->getPropertyExprs(), request.properties, source, *snapshot,
													   propertyCache);
							const LogicalGraphMutationEntityReference reference{
									LogicalGraphMutationEntityKind::LGMK_EDGE,
									LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE,
									static_cast<uint64_t>(batch.edge_creates.size())};
							batch.edge_creates.push_back(std::move(request));
							if (!edge->getVariable().empty()) {
								entities.emplace(edge->getVariable(), reference);
								propertyCache.insert_or_assign(
										edge->getVariable(),
										propertyMapFromLogicalProperties(batch.edge_creates.back().properties,
																		 "logical CREATE/SET relationship creation"));
							}
							continue;
						}

						if (const auto *remove = dynamic_cast<const logical::LogicalRemove *>(operation);
							remove != nullptr) {
							if (remove->getItems().empty()) {
								throw std::logic_error("logical CREATE/SET chain has an empty REMOVE update");
							}
							for (const logical::LogicalRemoveItem &item: remove->getItems()) {
								const auto entity = entities.find(item.variable);
								if (item.variable.empty() || item.key.empty() || entity == entities.end()) {
									throw std::logic_error(
											"logical CREATE/SET cannot resolve a REMOVE entity variable");
								}
								const LogicalGraphMutationEntityReference reference = entity->second;
								if (item.type == logical::LogicalRemoveActionType::LREM_LABEL) {
									if (reference.entity_kind == LogicalGraphMutationEntityKind::LGMK_EDGE) {
										throw std::logic_error(
												"logical CREATE/SET cannot remove a label from a relationship");
									}
									if (reference.reference_kind ==
												LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE &&
										reference.value < batch.node_creates.size()) {
										auto &labels = batch.node_creates[static_cast<size_t>(reference.value)].labels;
										if (const auto found = std::ranges::find(labels, item.key);
											found != labels.end()) {
											labels.erase(found);
										}
										continue;
									}
									if (reference.reference_kind != LogicalGraphMutationReferenceKind::LGRK_PERSISTED) {
										throw std::logic_error(
												"logical CREATE/SET has an invalid Node label removal target");
									}
									const auto value = source.values.find(item.variable);
									if (value == source.values.end()) {
										throw std::logic_error("logical CREATE/SET lost a persisted Node binding");
									}
									MutableGraphEntity &mutableEntity =
											mutableEntityForValue(persistedEntities, value->second);
									if (mutableEntity.kind != LogicalGraphMutationEntityKind::LGMK_NODE ||
										mutableEntity.id != reference.value) {
										throw std::logic_error("logical CREATE/SET Node label removal binding "
															   "is inconsistent");
									}
									if (const auto found = std::ranges::find(mutableEntity.labels, item.key);
										found != mutableEntity.labels.end()) {
										mutableEntity.labels.erase(found);
										mutableEntity.labels_changed = true;
									}
									continue;
								}

								if (item.type != logical::LogicalRemoveActionType::LREM_PROPERTY) {
									throw std::logic_error("logical CREATE/SET has an unsupported REMOVE item type");
								}
								if (reference.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE ||
									reference.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE) {
									std::vector<LogicalGraphWriteProperty> *properties = nullptr;
									if (reference.reference_kind ==
												LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE &&
										reference.entity_kind == LogicalGraphMutationEntityKind::LGMK_NODE &&
										reference.value < batch.node_creates.size()) {
										properties =
												&batch.node_creates[static_cast<size_t>(reference.value)].properties;
									} else if (reference.reference_kind ==
													   LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE &&
											   reference.entity_kind == LogicalGraphMutationEntityKind::LGMK_EDGE &&
											   reference.value < batch.edge_creates.size()) {
										properties =
												&batch.edge_creates[static_cast<size_t>(reference.value)].properties;
									}
									if (properties == nullptr) {
										throw std::logic_error("logical CREATE/SET has an invalid created "
															   "Property removal target");
									}
									PropertyMap current =
											propertyMapFromLogicalProperties(*properties, "logical CREATE/SET");
									static_cast<void>(current.erase(item.key));
									*properties = sortedProperties(current);
									propertyCache.insert_or_assign(item.variable, std::move(current));
									continue;
								}
								if (reference.reference_kind != LogicalGraphMutationReferenceKind::LGRK_PERSISTED) {
									throw std::logic_error("logical CREATE/SET has an invalid persisted "
														   "Property removal target");
								}
								const auto value = source.values.find(item.variable);
								if (value == source.values.end()) {
									throw std::logic_error("logical CREATE/SET lost a persisted entity binding");
								}
								MutableGraphEntity &mutableEntity =
										mutableEntityForValue(persistedEntities, value->second);
								if (mutableEntity.kind != reference.entity_kind ||
									mutableEntity.id != reference.value) {
									throw std::logic_error(
											"logical CREATE/SET persisted entity binding is inconsistent");
								}
								PropertyMap &properties = mutableEntityProperties(mutableEntity, *snapshot, bounds);
								mutableEntity.properties_changed =
										properties.erase(item.key) != 0U || mutableEntity.properties_changed;
								propertyCache.insert_or_assign(item.variable, properties);
							}
							continue;
						}

						if (const auto *deleteOperator = dynamic_cast<const logical::LogicalDelete *>(operation);
							deleteOperator != nullptr) {
							if (deleteOperator->getVariables().empty()) {
								throw std::logic_error("logical CREATE/SET chain has an empty DELETE target list");
							}
							for (const std::string &variable: deleteOperator->getVariables()) {
								const auto entity = entities.find(variable);
								if (variable.empty() || entity == entities.end()) {
									throw std::logic_error(
											"logical CREATE/SET cannot resolve a DELETE entity variable");
								}
								const LogicalGraphMutationEntityReference reference = entity->second;
								if (reference.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE) {
									const auto isIncidentCreatedEdge =
											[&reference](const LogicalGraphBatchEdgeCreateRequest &edge) {
												return edge.source == reference || edge.target == reference;
											};
									if (deleteOperator->isDetach()) {
										// A provisional entity has no durable adjacency to scan. Fold every
										// already staged incident Edge into the same final-state cancellation.
										for (size_t edgeIndex = 0U; edgeIndex < batch.edge_creates.size();
											 ++edgeIndex) {
											if (isIncidentCreatedEdge(batch.edge_creates[edgeIndex])) {
												cancelledEdgeCreates.insert(edgeIndex);
											}
										}
									}
									cancelledNodeCreates.insert(static_cast<size_t>(reference.value));
									continue;
								}
								if (reference.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE) {
									cancelledEdgeCreates.insert(static_cast<size_t>(reference.value));
									continue;
								}
								const auto value = source.values.find(variable);
								if (value == source.values.end()) {
									throw std::logic_error("logical CREATE/SET lost a persisted DELETE binding");
								}
								if (reference.entity_kind == LogicalGraphMutationEntityKind::LGMK_NODE) {
									const auto *node = std::get_if<storage::LogicalGraphNode>(&value->second);
									if (node == nullptr || node->node_id != reference.value) {
										throw std::logic_error(
												"logical CREATE/SET persisted DELETE binding is inconsistent");
									}
									deletedNodeIds.insert(reference.value);
									deletedNodeDetach[reference.value] =
											deletedNodeDetach[reference.value] || deleteOperator->isDetach();
								} else {
									const auto *edge = std::get_if<storage::LogicalGraphEdge>(&value->second);
									if (edge == nullptr || edge->edge_id != reference.value) {
										throw std::logic_error(
												"logical CREATE/SET persisted DELETE binding is inconsistent");
									}
									deletedEdgeIds.insert(reference.value);
								}
							}
							continue;
						}

						const auto *set = dynamic_cast<const logical::LogicalSet *>(operation);
							if (set == nullptr || set->getItems().empty()) { // every private CreateSetMutationChain is produced by a collector that verifies the concrete SET type and nonempty shape.
							throw std::logic_error("logical CREATE/SET chain has an invalid update operator");
						}
						for (const logical::LogicalSetItem &item: set->getItems()) {
							const auto entity = entities.find(item.variable);
							if (item.variable.empty() || entity == entities.end()) {
								throw std::logic_error("logical CREATE/SET cannot resolve a SET entity variable");
							}
							const LogicalGraphMutationEntityReference reference = entity->second;
							if (item.type == logical::SetActionType::LSET_LABEL) {
								if (item.key.empty() ||
									reference.entity_kind != LogicalGraphMutationEntityKind::LGMK_NODE) {
									throw std::logic_error("logical CREATE/SET label assignment requires a "
														   "Node and nonempty label");
								}
								if (reference.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE) {
									if (reference.value >= batch.node_creates.size()) {
										throw std::logic_error(
												"logical CREATE/SET has an invalid created Node reference");
									}
									auto &labels = batch.node_creates[static_cast<size_t>(reference.value)].labels;
									if (std::ranges::find(labels, item.key) == labels.end()) {
										labels.push_back(item.key);
									}
									continue;
								}
								if (reference.reference_kind != LogicalGraphMutationReferenceKind::LGRK_PERSISTED) {
									throw std::logic_error("logical CREATE/SET has an invalid Node label target");
								}
								const auto value = source.values.find(item.variable);
								if (value == source.values.end()) {
									throw std::logic_error("logical CREATE/SET lost a persisted Node binding");
								}
								MutableGraphEntity &mutableEntity =
										mutableEntityForValue(persistedEntities, value->second);
								if (mutableEntity.kind != LogicalGraphMutationEntityKind::LGMK_NODE ||
									mutableEntity.id != reference.value) {
									throw std::logic_error("logical CREATE/SET Node label binding is inconsistent");
								}
								if (std::ranges::find(mutableEntity.labels, item.key) == mutableEntity.labels.end()) {
									mutableEntity.labels.push_back(item.key);
									mutableEntity.labels_changed = true;
								}
								continue;
							}

							if (reference.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE ||
								reference.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE) {
								std::vector<LogicalGraphWriteProperty> *properties = nullptr;
								if (reference.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE &&
									reference.entity_kind == LogicalGraphMutationEntityKind::LGMK_NODE &&
									reference.value < batch.node_creates.size()) {
									properties = &batch.node_creates[static_cast<size_t>(reference.value)].properties;
								} else if (reference.reference_kind ==
												   LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE &&
										   reference.entity_kind == LogicalGraphMutationEntityKind::LGMK_EDGE &&
										   reference.value < batch.edge_creates.size()) {
									properties = &batch.edge_creates[static_cast<size_t>(reference.value)].properties;
								}
								if (properties == nullptr) {
									throw std::logic_error("logical CREATE/SET has an invalid created Property target");
								}
								PropertyMap current =
										propertyMapFromLogicalProperties(*properties, "logical CREATE/SET");
								static_cast<void>(applyPropertySet(current, item, source, *snapshot, propertyCache));
								*properties = sortedProperties(current);
								propertyCache.insert_or_assign(item.variable, std::move(current));
								continue;
							}

							if (reference.reference_kind != LogicalGraphMutationReferenceKind::LGRK_PERSISTED) {
								throw std::logic_error("logical CREATE/SET has an invalid persisted Property target");
							}
							const auto value = source.values.find(item.variable);
							if (value == source.values.end()) {
								throw std::logic_error("logical CREATE/SET lost a persisted entity binding");
							}
							MutableGraphEntity &mutableEntity = mutableEntityForValue(persistedEntities, value->second);
							if (mutableEntity.kind != reference.entity_kind || mutableEntity.id != reference.value) {
								throw std::logic_error("logical CREATE/SET persisted entity binding is inconsistent");
							}
							PropertyMap &properties = mutableEntityProperties(mutableEntity, *snapshot, bounds);
							synchronizeEntityPropertyCache(source, mutableEntity, properties, propertyCache);
							mutableEntity.properties_changed =
									applyPropertySet(properties, item, source, *snapshot, propertyCache) ||
									mutableEntity.properties_changed;
							synchronizeEntityPropertyCache(source, mutableEntity, properties, propertyCache);
						}
					}
					if (retainOutputRow) {
						projectionRows.push_back({source, std::move(entities), {}});
					}
				};

				LogicalPlanReadRuntimeStats stats;
				if (foreach == nullptr) {
					if (injectedInputRows == nullptr) {
						stats = reader.executeWithSnapshot(
								*snapshot, *readRoot, bounds,
								[&stageRow](const LogicalPlanReadRow &source) {
									stageRow(source, true);
									return true;
								},
								context);
					} else {
						if (injectedInputRows->empty() || injectedInputRows->size() > bounds.maximum_rows) {
							throw std::logic_error(
									"logical IN TRANSACTIONS CALL has an invalid materialized input batch");
						}
						for (const LogicalPlanReadRow &inputRow: *injectedInputRows) {
							const LogicalPlanReadRuntimeStats rowStats = reader.executeWithSnapshotAndInputRow(
									*snapshot, *readRoot, bounds, inputRow,
									[&stageRow](const LogicalPlanReadRow &source) {
										stageRow(source, true);
										return true;
									},
									context);
							if (rowStats.rows_emitted == 0U) {
								throw std::runtime_error(
										"logical IN TRANSACTIONS CALL input row was not consumed by its subquery");
							}
							if (stats.rows_emitted > bounds.maximum_rows - rowStats.rows_emitted ||
								stats.nodes_scanned > bounds.maximum_node_records - rowStats.nodes_scanned ||
								stats.edges_scanned > bounds.maximum_edges_per_node - rowStats.edges_scanned) {
								throw std::runtime_error("logical IN TRANSACTIONS CALL exceeded a read bound");
							}
							stats.rows_emitted += rowStats.rows_emitted;
							stats.nodes_scanned += rowStats.nodes_scanned;
							stats.edges_scanned += rowStats.edges_scanned;
							stats.property_sets_materialized += rowStats.property_sets_materialized;
						}
					}
				} else {
					if (injectedInputRows != nullptr) {
						throw std::logic_error("logical IN TRANSACTIONS CALL does not support a FOREACH subquery body");
					}
					size_t iterationCount = 0U;
					stats = reader.executeWithSnapshot(
							*snapshot, *readRoot, bounds,
							[&](const LogicalPlanReadRow &source) {
								context.checkGuard();
								projectionRows.push_back({source, entityReferencesForRow(source), {}});

								LogicalPlanReadRow evaluationRow = source;
								const LogicalPlanReadValue evaluated =
										LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(
												*snapshot, *foreach->getListExpr(), evaluationRow, bounds, context);
								const auto *listValue = std::get_if<PropertyValue>(&evaluated);
								if (listValue == nullptr) {
									throw std::logic_error(
											"logical FOREACH list expression must evaluate to a Property value");
								}
								if (listValue->getType() != PropertyType::LIST) {
									// Preserve the established FOREACH lowering contract: a scalar
									// source produces no body iteration and still passes its input row on.
									return true;
								}
								const std::vector<PropertyValue> &values = listValue->getList();
								if (values.size() > bounds.maximum_rows ||
									iterationCount > bounds.maximum_rows - values.size()) {
									throw std::runtime_error("logical FOREACH exceeded its configured iteration bound");
								}
								for (const PropertyValue &value: values) {
									context.checkGuard();
									LogicalPlanReadRow iteration = source;
									iteration.values.insert_or_assign(foreach->getIterVar(), value);
									iteration.property_cache.erase(foreach->getIterVar());
									stageRow(iteration, false);
									++iterationCount;
								}
								return true;
							},
							context);
				}
				requireCompleteReadForMutation(stats, bounds,
											   foreach == nullptr ? "logical CREATE/SET" : "logical FOREACH");

				for (const uint64_t edgeId: deletedEdgeIds) {
					batch.edge_deletes.push_back({edgeId});
				}
				for (const uint64_t nodeId: deletedNodeIds) {
					batch.node_deletes.push_back({nodeId, deletedNodeDetach[nodeId],
												  deletedNodeDetach[nodeId] ? bounds.maximum_edges_per_node : 0U});
				}

				// Cancel created entities before publication and remap surviving endpoints.
				if (!cancelledNodeCreates.empty() || !cancelledEdgeCreates.empty()) {
					const size_t invalidOrdinal = std::numeric_limits<size_t>::max();
					std::vector<size_t> nodeRemap(batch.node_creates.size(), invalidOrdinal);
					std::vector<LogicalGraphNodeCreateRequest> survivingNodes;
					survivingNodes.reserve(batch.node_creates.size() - cancelledNodeCreates.size());
					for (size_t index = 0U; index < batch.node_creates.size(); ++index) {
						if (cancelledNodeCreates.contains(index))
							continue;
						nodeRemap[index] = survivingNodes.size();
						survivingNodes.push_back(std::move(batch.node_creates[index]));
					}
					std::vector<size_t> edgeRemap(batch.edge_creates.size(), invalidOrdinal);
					std::vector<LogicalGraphBatchEdgeCreateRequest> survivingEdges;
					survivingEdges.reserve(batch.edge_creates.size() - cancelledEdgeCreates.size());
					const auto remapEndpoint = [&](LogicalGraphMutationEntityReference &reference) {
						if (reference.reference_kind != LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE)
							return;
						if (reference.value >= nodeRemap.size() ||
							nodeRemap[static_cast<size_t>(reference.value)] == invalidOrdinal) {
							throw std::logic_error(
									"logical CREATE/SET deleted a Node still referenced by a surviving relationship");
						}
						reference.value = nodeRemap[static_cast<size_t>(reference.value)];
					};
					for (size_t index = 0U; index < batch.edge_creates.size(); ++index) {
						if (cancelledEdgeCreates.contains(index))
							continue;
						LogicalGraphBatchEdgeCreateRequest edge = std::move(batch.edge_creates[index]);
						remapEndpoint(edge.source);
						remapEndpoint(edge.target);
						edgeRemap[index] = survivingEdges.size();
						survivingEdges.push_back(std::move(edge));
					}
					batch.node_creates = std::move(survivingNodes);
					batch.edge_creates = std::move(survivingEdges);
					for (CreateSetRuntimeRow &runtimeRow: projectionRows) {
						for (auto &[variable, reference]: runtimeRow.entities) {
							if (reference.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE &&
								reference.value < nodeRemap.size() &&
								nodeRemap[static_cast<size_t>(reference.value)] != invalidOrdinal) {
								reference.value = nodeRemap[static_cast<size_t>(reference.value)];
							} else if (reference.reference_kind ==
											   LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE &&
									   reference.value < edgeRemap.size() &&
									   edgeRemap[static_cast<size_t>(reference.value)] != invalidOrdinal) {
								reference.value = edgeRemap[static_cast<size_t>(reference.value)];
							} else if (reference.reference_kind ==
											   LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE ||
									   reference.reference_kind ==
											   LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE) {
								runtimeRow.unavailable_implicit_entities.insert(variable);
							}
						}
					}
				}

				std::vector<storage::LogicalGraphPropertyPatchRequest> propertyPatches;
				size_t patchEntryLimit = 0U;
				for (const auto &[_, entity]: persistedEntities) {
					const LogicalGraphMutationEntityReference reference{
							entity.kind, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, entity.id};
					if ((entity.kind == LogicalGraphMutationEntityKind::LGMK_NODE &&
						 deletedNodeIds.contains(entity.id)) ||
						(entity.kind == LogicalGraphMutationEntityKind::LGMK_EDGE &&
						 deletedEdgeIds.contains(entity.id))) {
						continue;
					}
					if (entity.properties_changed) {
						if (snapshot->supportsShapedPropertyReadAdmission()) {
							auto patch = finalPropertyPatch(entity, context, bounds.maximum_materialized_bytes);
							if (!patch.sets.empty() || !patch.removals.empty()) {
								for (const size_t count : {entity.direct_properties.size(), patch.sets.size(), patch.removals.size()}) {
									if (count > (std::numeric_limits<size_t>::max)() - patchEntryLimit)
										throw std::overflow_error("logical CREATE/SET Property patch entry count overflowed");
									patchEntryLimit += count;
									}
								propertyPatches.push_back(std::move(patch));
							}
						} else batch.property_replacements.push_back({reference, sortedProperties(*entity.properties)});
					}
					if (entity.labels_changed) {
						batch.node_label_replacements.push_back({reference, entity.labels});
					}
				}
				// Validate the complete result surface against provisional IDs before
				// publication. The provisional result is discarded: it exists only to
				// ensure a failing RETURN cannot make this batch durable. A successful
				// publication below rebuilds the result with backend-assigned IDs.
				storage::LogicalGraphMutationBatchResult prepublicationResult;
				prepublicationResult.created_node_ids.reserve(batch.node_creates.size());
				for (size_t index = 0U; index < batch.node_creates.size(); ++index) {
					prepublicationResult.created_node_ids.push_back(std::numeric_limits<uint64_t>::max() - index);
				}
				prepublicationResult.created_edge_ids.reserve(batch.edge_creates.size());
				for (size_t index = 0U; index < batch.edge_creates.size(); ++index) {
					prepublicationResult.created_edge_ids.push_back(std::numeric_limits<uint64_t>::max() - index);
				}
				CreateSetPrepublicationResultCache prepublicationCache;
				static_cast<void>(buildCreateSetResult(projection, implicitColumns, projectionRows, *snapshot, batch,
													   prepublicationResult, persistedEntities, bounds, context,
													   exportedProjection, &prepublicationCache, true, resultSort,
													   resultSkip, resultLimit));
				try {
					// A periodic checkpoint can be deferred across a small batch. Check
					// immediately before its durable publication so an expired query never
					// makes the current mutation batch visible.
					context.checkGuardNow();
					storage::LogicalGraphMutationBatchResult result;
					if (!propertyPatches.empty()) {
						storage::LogicalGraphMutationReadControl control;
						control.checkpoint = [&context] { context.checkGuardNow(); };
						control.property_admission = [&context, &bounds](size_t bytes) {
							context.checkGuardNow();
							if (bytes > bounds.maximum_materialized_bytes)
								throw std::runtime_error("logical CREATE/SET Property patch source exceeded its byte bound");
						};
						result = writeBackend.executeLogicalGraphMutationBatchWithPropertyPatches(
							*snapshot, batch, propertyPatches, patchEntryLimit, *fence, control);
					} else result = executeMutationWithReadControl(writeBackend, batch, fence, bounds, context);
					return buildCreateSetResult(projection, implicitColumns, projectionRows, *snapshot, batch, result,
												persistedEntities, bounds, context, exportedProjection,
												&prepublicationCache, false, resultSort, resultSkip, resultLimit);
				} catch (const storage::LogicalGraphMutationConflict &) {
					// All created references and Property reads belong to this pinned view.
					// Replan the complete statement after a concurrent publication.
				}
			}
			throw std::runtime_error("logical CREATE/SET exceeded its optimistic conflict retry limit");
		}

		[[nodiscard]] QueryResult executeInTransactionsWriteOnlyCall(
				const InTransactionsWriteOnlyCall &batchedCall, const storage::LogicalGraphReadBackend &readBackend,
				storage::LogicalGraphWriteBackend &writeBackend, bool permitsIndependentBatchCommits,
				const LogicalPlanReadRuntimeBounds &bounds, const QueryContext &context) {
			if (!permitsIndependentBatchCommits) {
				throw std::runtime_error(
						"CALL { ... } IN TRANSACTIONS is not permitted inside an explicit logical transaction");
			}
			const logical::LogicalCallSubquery &call = *batchedCall.call;
			if (call.getBatchSize() < 0) {
				throw std::invalid_argument("logical IN TRANSACTIONS CALL has a negative batch size");
			}
			const size_t batchSize = call.getBatchSize() == 0 ? std::min<size_t>(1'000U, bounds.maximum_rows)
															  : static_cast<size_t>(call.getBatchSize());
			if (batchSize == 0U || batchSize > bounds.maximum_rows) {
				throw std::invalid_argument("logical IN TRANSACTIONS CALL has an invalid batch size");
			}

			const logical::LogicalOperator *input = call.getInput();
			std::unique_ptr<logical::LogicalSingleRow> implicitInput;
			if (input == nullptr) {
				implicitInput = std::make_unique<logical::LogicalSingleRow>();
				input = implicitInput.get();
			}
			std::unique_ptr<storage::LogicalGraphReadSnapshot> materializationSnapshot =
					readBackend.openLogicalGraphReadSnapshot();
			if (!materializationSnapshot) {
				throw std::runtime_error("logical IN TRANSACTIONS CALL could not open its input snapshot");
			}
			std::vector<LogicalPlanReadRow> inputRows;
			inputRows.reserve(std::min(batchSize, bounds.maximum_rows));
			LogicalPlanReadRuntime reader(readBackend);
			const LogicalPlanReadRuntimeStats inputStats = reader.executeWithSnapshot(
					*materializationSnapshot, *input, bounds,
					[&](const LogicalPlanReadRow &row) {
						if (inputRows.size() == bounds.maximum_rows) {
							throw std::runtime_error(
									"logical IN TRANSACTIONS CALL exceeded its materialized input bound");
						}
						inputRows.push_back(row);
						return true;
					},
					context);
			requireCompleteReadForMutation(inputStats, bounds, "logical IN TRANSACTIONS CALL input");

			QueryResult result;
			if (batchedCall.outer_projection != nullptr) {
				result.setColumns(batchedCall.outer_projection->getOutputVariables());
			} else if (batchedCall.exported_projection != nullptr) {
				result.setColumns(batchedCall.exported_projection->getOutputVariables());
			} else {
				result.setColumns(visibleMutationOutputColumns(call));
			}
			result.setPipelineRowCount(inputRows.size());
			const auto materializeProjectionValue = [&](const LogicalPlanReadValue &value) -> ResultValue {
				if (const auto *scalar = std::get_if<PropertyValue>(&value)) {
					return *scalar;
				}
				if (const auto *node = std::get_if<storage::LogicalGraphNode>(&value)) {
					return LogicalNodeResult{node->node_id, node->labels,
											 readInlineProperties(node->properties, bounds.maximum_property_entries)};
				}
				const auto &edge = std::get<storage::LogicalGraphEdge>(value);
				return LogicalEdgeResult{edge.edge_id, edge.source_node_id, edge.target_node_id, edge.type,
										 readInlineProperties(edge.properties, bounds.maximum_property_entries)};
			};
			std::vector<QueryResult::Row> precomputedProjectionRows;
			if (batchedCall.outer_projection != nullptr && batchedCall.exported_projection == nullptr) {
				precomputedProjectionRows.reserve(inputRows.size());
				// Result-pipeline failures must occur before the first independently
				// committed batch. The projection observes the materialized outer
				// snapshot, exactly as it would after a write-only CALL preserves a row.
				for (const LogicalPlanReadRow &row: inputRows) {
					std::unique_ptr<logical::LogicalOperator> projection = batchedCall.outer_projection->clone();
					projection->setChild(0U, std::make_unique<logical::LogicalSingleRow>());
					const LogicalPlanReadRuntimeStats projectionStats = reader.executeWithSnapshotAndInputRow(
							*materializationSnapshot, *projection, bounds, row,
							[&](const LogicalPlanReadRow &projected) {
								QueryResult::Row output;
								output.reserve(result.getColumns().size());
								for (const std::string &column: result.getColumns()) {
									const auto value = projected.values.find(column);
									if (value == projected.values.end() ||
										!output.emplace(column, materializeProjectionValue(value->second)).second) {
										throw std::logic_error(
												"logical IN TRANSACTIONS CALL could not materialize its outer result");
									}
								}
								precomputedProjectionRows.push_back(std::move(output));
								return true;
							},
							context);
					if (projectionStats.rows_emitted != 1U) {
						throw std::runtime_error("logical IN TRANSACTIONS CALL outer projection lost an input row");
					}
				}
			}
			for (size_t offset = 0U; offset < inputRows.size(); offset += batchSize) {
				const size_t count = std::min(batchSize, inputRows.size() - offset);
				std::vector<LogicalPlanReadRow> batchRows;
				batchRows.reserve(count);
				batchRows.insert(batchRows.end(), inputRows.begin() + static_cast<std::ptrdiff_t>(offset),
								 inputRows.begin() + static_cast<std::ptrdiff_t>(offset + count));
				const logical::LogicalProject *batchProjection =
						batchedCall.exported_projection != nullptr
								? (batchedCall.outer_projection != nullptr ? batchedCall.outer_projection
																		   : batchedCall.exported_projection)
								: nullptr;
				QueryResult batchResult = executeCreateSetMutationChain(
						batchedCall.mutation_chain, readBackend, writeBackend, bounds, context, batchProjection,
						std::vector<std::string>{}, nullptr, &batchRows, batchedCall.exported_projection);
				if (batchResult.getPipelineRowCount() != count) {
					throw std::runtime_error(
							"logical IN TRANSACTIONS CALL subquery did not preserve input cardinality");
				}
				if (batchedCall.outer_projection != nullptr && batchedCall.exported_projection == nullptr &&
					batchResult.rowCount() != 0U) {
					throw std::logic_error("logical IN TRANSACTIONS CALL produced an unexpected subquery result row");
				}
				for (const QueryResult::Row &row: batchResult.getRows()) {
					result.addRow(row);
				}
			}
			for (QueryResult::Row &row: precomputedProjectionRows) {
				result.addRow(std::move(row));
			}
			return result;
		}

