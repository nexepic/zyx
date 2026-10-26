		[[nodiscard]] PropertyMap mergeProperties(const std::unordered_map<std::string, PropertyValue> &properties) {
			PropertyMap result;
			result.reserve(properties.size());
			for (const auto &[key, value]: properties) {
				if (key.empty() || value.getType() == PropertyType::NULL_TYPE || !result.emplace(key, value).second) {
					throw std::logic_error("logical MERGE has an invalid match Property");
				}
			}
			return result;
		}

		void validateMergeLabels(const std::vector<std::string> &labels) {
			std::set<std::string> unique;
			for (const std::string &label: labels) {
				if (label.empty() || !unique.insert(label).second) {
					throw std::logic_error("logical MERGE has an empty or duplicate Node label");
				}
			}
		}

		[[nodiscard]] std::string normalizedMergeDirection(const std::string &direction) {
			std::string normalized;
			normalized.reserve(direction.size());
			for (const char value: direction) {
				normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(value))));
			}
			if (normalized != "out" && normalized != "in" && normalized != "both") {
				throw std::logic_error("logical MERGE has an unsupported relationship direction");
			}
			return normalized;
		}

		struct MergeRuntimeRow {
			LogicalPlanReadRow row;
			std::unordered_map<std::string, LogicalGraphMutationEntityReference> entities;
			std::vector<LogicalGraphMutationEntityReference> recent_nodes;
		};

		struct MergeNodeMatch {
			LogicalGraphMutationEntityReference reference;
			storage::LogicalGraphNode node;
			bool created = false;
		};

		struct MergeEdgeMatch {
			LogicalGraphMutationEntityReference reference;
			storage::LogicalGraphEdge edge;
			bool created = false;
			bool resolved = false;
		};

		using MergeIdentityProperties = std::vector<std::pair<std::string, PropertyValue>>;

		struct MergeNodeMatchKey {
			const logical::LogicalMergeNode *operation = nullptr;
			MergeIdentityProperties properties;

			[[nodiscard]] bool operator<(const MergeNodeMatchKey &other) const {
				if (std::less<const logical::LogicalMergeNode *>{}(operation, other.operation)) {
					return true;
				}
				if (std::less<const logical::LogicalMergeNode *>{}(other.operation, operation)) {
					return false;
				}
				return properties < other.properties;
			}
		};

		struct MergeEdgeMatchKey {
			const logical::LogicalMergeEdge *operation = nullptr;
			LogicalGraphMutationEntityReference source;
			LogicalGraphMutationEntityReference target;
			MergeIdentityProperties properties;

			[[nodiscard]] static bool referenceLess(const LogicalGraphMutationEntityReference &left,
													const LogicalGraphMutationEntityReference &right) {
				if (left.entity_kind != right.entity_kind) {
					return left.entity_kind < right.entity_kind;
				}
				if (left.reference_kind != right.reference_kind) {
					return left.reference_kind < right.reference_kind;
				}
				return left.value < right.value;
			}

			[[nodiscard]] bool operator<(const MergeEdgeMatchKey &other) const {
				if (std::less<const logical::LogicalMergeEdge *>{}(operation, other.operation)) {
					return true;
				}
				if (std::less<const logical::LogicalMergeEdge *>{}(other.operation, operation)) {
					return false;
				}
				if (referenceLess(source, other.source)) {
					return true;
				}
				if (referenceLess(other.source, source)) {
					return false;
				}
				if (referenceLess(target, other.target)) {
					return true;
				}
				if (referenceLess(other.target, target)) {
					return false;
				}
				return properties < other.properties;
			}
		};

		struct MergeExecutionState {
			storage::LogicalGraphReadSnapshot &snapshot;
			const LogicalPlanReadRuntimeBounds &bounds;
			const QueryContext &context;
			const LogicalPlanReadRuntime &reader;
			LogicalGraphMutationBatch batch;
			std::vector<storage::LogicalGraphPropertyPatchRequest> property_patches;
			size_t patch_entry_limit = 0U;
			MutableGraphEntities modified_entities;
			std::vector<PropertyMap> created_node_properties;
			std::vector<PropertyMap> created_edge_properties;
			std::map<MergeNodeMatchKey, MergeNodeMatch> node_matches;
			std::map<MergeEdgeMatchKey, MergeEdgeMatch> edge_matches;
			size_t operator_count = 0U;

			MergeExecutionState(storage::LogicalGraphReadSnapshot &readSnapshot,
								const LogicalPlanReadRuntimeBounds &readBounds, const QueryContext &queryContext,
								const LogicalPlanReadRuntime &readRuntime) :
				snapshot(readSnapshot), bounds(readBounds), context(queryContext), reader(readRuntime) {}
		};

		[[nodiscard]] MergeIdentityProperties canonicalMergeIdentityProperties(const PropertyMap &properties) {
			MergeIdentityProperties result;
			result.reserve(properties.size());
			for (const auto &[key, value]: properties) {
				result.emplace_back(key, value);
			}
			std::ranges::sort(result, {}, &std::pair<std::string, PropertyValue>::first);
			return result;
		}

		[[nodiscard]] PropertyMap evaluateMergeIdentityProperties(
				const std::unordered_map<std::string, PropertyValue> &literalProperties,
				const std::unordered_map<std::string, std::shared_ptr<expressions::Expression>> &expressionProperties,
				MergeRuntimeRow &row, MergeExecutionState &state) {
			if (literalProperties.size() > state.bounds.maximum_property_entries ||
				expressionProperties.size() > state.bounds.maximum_property_entries - literalProperties.size())
				throw std::runtime_error("logical MERGE identity Properties exceeded their entry bound");
			PropertyMap result = mergeProperties(literalProperties);
			for (const auto &[key, expression]: expressionProperties) {
				state.context.checkGuard();
				if (key.empty() || !expression || result.contains(key)) {
					throw std::logic_error("logical MERGE has an invalid dynamic match Property");
				}
				const PropertyValue value = evaluateCreatePropertyExpression(
						*expression, state.context, row.row, state.snapshot, state.bounds, row.row.property_cache);
				if (value.getType() == PropertyType::NULL_TYPE) {
					throw std::runtime_error("logical MERGE cannot use NULL as an identity Property");
				}
				result.emplace(key, value);
			}
			return result;
		}

		using MergeRowVisitor = std::function<void(MergeRuntimeRow &&)>;

		[[nodiscard]] bool mergeNodePropertiesMatch(const PropertyMap &actual, const PropertyMap &expected) {
			for (const auto &[key, value]: expected) {
				const auto found = actual.find(key);
				if (found == actual.end() || found->second != value) {
					return false;
				}
			}
			return true;
		}

		void enforceMergePropertyColumnBytes(
				const std::vector<std::vector<PropertyValue>> &columns,
				const MergeIdentityProperties &properties, size_t maximumBytes) {
			size_t bytes = 0U;
			for (size_t column = 0U; column < columns.size(); ++column) {
				const size_t keyBytes = column < properties.size() ? properties[column].first.size() : 0U;
				if (keyBytes > maximumBytes - bytes) {
					throw std::runtime_error("logical MERGE relationship Property batch exceeded its byte bound");
				}
				bytes += keyBytes;
				for (const PropertyValue &value: columns[column]) {
					const size_t valueBytes = property_utils::getPropertyValueSize(value);
					if (valueBytes > maximumBytes - bytes) {
						throw std::runtime_error("logical MERGE relationship Property batch exceeded its byte bound");
					}
					bytes += valueBytes;
				}
			}
		}

		[[nodiscard]] std::optional<storage::LogicalGraphNode> findMergeNode(const logical::LogicalMergeNode &operation,
																			 const PropertyMap &properties,
																			 MergeExecutionState &state) {
			std::vector<std::pair<std::string, PropertyValue>> predicates;
			predicates.reserve(properties.size());
			for (const auto &[key, value]: properties) {
				predicates.emplace_back(key, value);
			}
			std::ranges::sort(predicates, {}, &std::pair<std::string, PropertyValue>::first);
			logical::LogicalNodeScan scan("__logical_merge_node", operation.getLabels(), std::move(predicates));
			std::optional<storage::LogicalGraphNode> found;
			const LogicalPlanReadRuntimeStats stats = state.reader.executeWithSnapshot(
					state.snapshot, scan, state.bounds,
					[&](const LogicalPlanReadRow &row) {
						state.context.checkGuard();
						const auto binding = row.values.find(scan.getVariable());
						if (binding == row.values.end() ||
							!std::holds_alternative<storage::LogicalGraphNode>(binding->second)) {
							throw std::logic_error("logical MERGE Node scan produced an invalid binding");
						}
						found = std::get<storage::LogicalGraphNode>(binding->second);
						return false;
					},
					state.context);
			if (!found.has_value()) {
				requireCompleteReadForMutation(stats, state.bounds, "logical MERGE Node scan");
			}
			return found;
		}

		void synchronizeMergePropertyAliases(MergeRuntimeRow &row, const std::string &variable,
			const PropertyMap &properties, const QueryContext &context) {
			const auto target = row.entities.find(variable);
			if (target == row.entities.end()) throw std::logic_error("logical MERGE lost its Property target reference");
			for (const auto &[alias, reference] : row.entities) {
				context.checkGuard();
				if (reference == target->second) row.row.property_cache.insert_or_assign(alias, properties);
			}
		}

		[[nodiscard]] bool applyMergeActions(PropertyMap &properties,
											 const std::vector<logical::MergeSetAction> &actions,
											 const std::string &targetVariable, MergeRuntimeRow &row,
											 MergeExecutionState &state) {
			if (actions.empty()) {
				return false;
			}
			if (targetVariable.empty()) {
				throw std::logic_error("logical MERGE cannot apply ON CREATE or ON MATCH to an anonymous entity");
			}
			bool changed = false;
			for (const logical::MergeSetAction &action: actions) {
				state.context.checkGuard();
				if (action.variable != targetVariable || !action.expression ||
					(action.type == logical::SetActionType::LSET_PROPERTY && action.key.empty()) ||
					(action.type != logical::SetActionType::LSET_PROPERTY && action.type != logical::SetActionType::LSET_MAP_MERGE &&
					 action.type != logical::SetActionType::LSET_MAP_REPLACE)) {
					throw std::logic_error("logical MERGE has an unsupported ON CREATE or ON MATCH assignment");
				}
				const PropertyValue value =
						evaluateCreatePropertyExpression(*action.expression, state.context, row.row, state.snapshot,
														 state.bounds, row.row.property_cache);
				if (action.type == logical::SetActionType::LSET_MAP_REPLACE) {
					changed = replacePropertyMap(properties, value, state.context, state.bounds.maximum_property_entries) || changed;
				} else if (action.type == logical::SetActionType::LSET_MAP_MERGE) {
					changed = mergePropertyMap(properties, value, state.context, state.bounds.maximum_property_entries) || changed;
				} else if (value.getType() == PropertyType::NULL_TYPE) {
					changed = properties.erase(action.key) != 0U || changed;
				} else {
					const auto existing = properties.find(action.key);
					if (existing == properties.end() || existing->second != value) {
						assignBoundedProperty(properties, action.key, value, state.bounds.maximum_property_entries);
						changed = true;
					}
				}
				synchronizeMergePropertyAliases(row, targetVariable, properties, state.context);
			}
			return changed;
		}

		void bindMergeNode(MergeRuntimeRow &row, const std::string &variable,
						   const LogicalGraphMutationEntityReference &reference, const storage::LogicalGraphNode &node,
						   const std::optional<PropertyMap> &properties) {
			if (!variable.empty()) {
				const auto existing = row.entities.find(variable);
				if (existing != row.entities.end() && existing->second != reference) {
					throw std::logic_error("logical MERGE cannot rebind a Node variable");
				}
				const auto value = row.row.values.find(variable);
				if (value != row.row.values.end() &&
					!std::holds_alternative<storage::LogicalGraphNode>(value->second)) {
					throw std::logic_error("logical MERGE Node variable is bound to a non-Node value");
				}
				row.entities.insert_or_assign(variable, reference);
				row.row.values.insert_or_assign(variable, node);
				if (properties.has_value()) {
					row.row.property_cache.insert_or_assign(variable, *properties);
				}
			}
			row.recent_nodes.push_back(reference);
		}

		void bindMergeEdge(MergeRuntimeRow &row, const std::string &variable,
						   const LogicalGraphMutationEntityReference &reference, const storage::LogicalGraphEdge &edge,
						   const std::optional<PropertyMap> &properties) {
			if (variable.empty()) {
				return;
			}
			const auto existing = row.entities.find(variable);
			if (existing != row.entities.end() && existing->second != reference) {
				throw std::logic_error("logical MERGE cannot rebind a relationship variable");
			}
			const auto value = row.row.values.find(variable);
			if (value != row.row.values.end() && !std::holds_alternative<storage::LogicalGraphEdge>(value->second)) {
				throw std::logic_error("logical MERGE relationship variable is bound to a non-relationship value");
			}
			row.entities.insert_or_assign(variable, reference);
			row.row.values.insert_or_assign(variable, edge);
			if (properties.has_value()) {
				row.row.property_cache.insert_or_assign(variable, *properties);
			}
		}

		[[nodiscard]] LogicalGraphMutationEntityReference resolveMergeNode(const MergeRuntimeRow &row,
																		   const std::string &variable, bool source) {
			if (!variable.empty()) {
				const auto found = row.entities.find(variable);
				if (found == row.entities.end() ||
					found->second.entity_kind != LogicalGraphMutationEntityKind::LGMK_NODE) {
					throw std::logic_error("logical MERGE cannot resolve a relationship Node endpoint");
				}
				return found->second;
			}
			if (row.recent_nodes.size() < 2U) {
				throw std::logic_error("logical MERGE cannot infer an anonymous relationship Node endpoint");
			}
			return row.recent_nodes[row.recent_nodes.size() - (source ? 2U : 1U)];
		}

		[[nodiscard]] std::optional<storage::LogicalGraphEdge>
		findMergeEdge(const logical::LogicalMergeEdge &operation, uint64_t sourceNodeId, uint64_t targetNodeId,
					  const PropertyMap &properties, const std::string &direction, MergeExecutionState &state) {
			if (properties.size() > state.bounds.maximum_property_entries) {
				throw std::runtime_error("logical MERGE relationship exceeded its Property bound");
			}
			const MergeIdentityProperties orderedProperties = canonicalMergeIdentityProperties(properties);
			std::vector<std::string_view> propertyKeys;
			propertyKeys.reserve(orderedProperties.size());
			for (const auto &[key, value]: orderedProperties) {
				static_cast<void>(value);
				propertyKeys.push_back(key);
			}
			size_t remaining = state.bounds.maximum_edges_per_node;
			const auto scanDirection = [&](storage::LogicalGraphReadDirection scanDirection,
										   const std::function<bool(const storage::LogicalGraphEdge &)> &matches)
					-> std::optional<storage::LogicalGraphEdge> {
				std::optional<storage::LogicalGraphAdjacencyContinuation> continuation;
				uint64_t previousEdgeId = 0U;
				for (;;) {
					if (remaining == 0U) {
						throw std::runtime_error("logical MERGE relationship scan reached its configured bound");
					}
					const size_t pageLimit = std::min(remaining, state.bounds.maximum_adjacency_page_records);
					// MERGE identity matching only needs topology for the scan.  Do not
					// decode every candidate's Property collection; materialize the
					// complete Edge only after endpoint/type predicates match.
					const storage::LogicalGraphAdjacencyTopologyPage page =
							state.snapshot.scanAdjacentEdgeTopologies(sourceNodeId, scanDirection, continuation, pageLimit);
					if (page.edges.size() > pageLimit || (page.edges.empty() && page.continuation.has_value()) ||
							(continuation.has_value() &&
									(page.edges.empty() || page.edges.front().edge_id != continuation->next_edge_id))) {
						throw std::runtime_error("logical MERGE relationship scan returned an invalid bounded page");
					}
					std::vector<const storage::LogicalGraphEdgeTopology *> candidates;
					candidates.reserve(page.edges.size());
					for (const storage::LogicalGraphEdgeTopology &topology: page.edges) {
						state.context.checkGuard();
						if (topology.edge_id == 0U || (previousEdgeId != 0U && topology.edge_id <= previousEdgeId)) {
							throw std::runtime_error(
									"logical MERGE relationship scan returned non-monotonic adjacency Edge IDs");
						}
						previousEdgeId = topology.edge_id;
						if (direction == "both" && scanDirection == storage::LogicalGraphReadDirection::LGRD_INCOMING &&
							topology.source_node_id == sourceNodeId && topology.target_node_id == sourceNodeId) {
							continue;
						}
						--remaining;
						if (topology.type == operation.getEdgeType() &&
							matches(storage::LogicalGraphEdge{topology.edge_id, topology.source_node_id,
															  topology.target_node_id, topology.type})) {
							candidates.push_back(&topology);
						}
						if (remaining == 0U && page.continuation.has_value()) {
							throw std::runtime_error("logical MERGE relationship scan reached its configured bound");
						}
					}
					std::optional<size_t> matchOrdinal;
					if (!candidates.empty() && orderedProperties.empty()) {
						matchOrdinal = 0U;
					} else if (!candidates.empty()) {
						std::vector<uint64_t> edgeIds;
						edgeIds.reserve(candidates.size());
						for (const storage::LogicalGraphEdgeTopology *candidate: candidates) {
							edgeIds.push_back(candidate->edge_id);
						}
						size_t admittedPropertyBytes = 0U;
						for (const auto &[key, expected]: orderedProperties) {
							static_cast<void>(expected);
							if (key.size() > state.bounds.maximum_materialized_bytes - admittedPropertyBytes) {
								throw std::runtime_error(
										"logical MERGE relationship Property batch exceeded its byte bound");
							}
							admittedPropertyBytes += key.size();
						}
						const std::function<void(size_t)> admission = [&](size_t bytes) {
							state.context.checkGuard();
							if (bytes > state.bounds.maximum_materialized_bytes - admittedPropertyBytes) {
								throw std::runtime_error(
										"logical MERGE relationship Property batch exceeded its byte bound");
							}
							admittedPropertyBytes += bytes;
						};
						const std::vector<std::vector<PropertyValue>> columns =
								state.snapshot.supportsPropertyReadAdmission()
										? state.snapshot.readEdgePropertyColumnsSortedWithAdmission(
												  edgeIds, propertyKeys, admission)
										: state.snapshot.readEdgePropertyColumnsSorted(edgeIds, propertyKeys);
						if (columns.size() != orderedProperties.size()) {
							throw std::runtime_error(
									"logical MERGE relationship Property batch returned an invalid column count");
						}
						for (const auto &column: columns) {
							if (column.size() != candidates.size()) {
								throw std::runtime_error(
										"logical MERGE relationship Property batch returned an invalid row count");
							}
						}
						enforceMergePropertyColumnBytes(columns, orderedProperties,
														state.bounds.maximum_materialized_bytes);
						for (size_t row = 0U; row < candidates.size() && !matchOrdinal.has_value(); ++row) {
							bool matchesProperties = true;
							for (size_t column = 0U; column < columns.size(); ++column) {
								if (columns[column][row] != orderedProperties[column].second) {
									matchesProperties = false;
									break;
								}
							}
							if (matchesProperties) {
								matchOrdinal = row;
							}
						}
					}
					if (matchOrdinal.has_value()) {
						const storage::LogicalGraphEdgeTopology &topology = *candidates[*matchOrdinal];
						const storage::LogicalGraphEdge edge =
								state.snapshot.readEdgeShaped(topology.edge_id, completeEdgeReadShape());
						if (edge.edge_id != topology.edge_id || edge.source_node_id != topology.source_node_id ||
							edge.target_node_id != topology.target_node_id || edge.type != topology.type) {
							throw std::runtime_error("logical MERGE relationship scan returned mismatched Edge topology");
						}
						const PropertyMap actual =
								readEdgeProperties(state.snapshot, edge, state.bounds.maximum_property_entries);
						if (!mergeNodePropertiesMatch(actual, properties)) {
							throw std::runtime_error(
									"logical MERGE relationship selected Properties disagreed with the complete Edge");
						}
						return edge;
					}
					if (!page.continuation.has_value()) {
						return std::nullopt;
					}
					if (page.continuation->previous_edge_id != page.edges.back().edge_id ||
							page.continuation->next_edge_id == 0U ||
							page.continuation->previous_edge_id == page.continuation->next_edge_id) {
						throw std::runtime_error("logical MERGE relationship scan returned an invalid continuation");
					}
					continuation = page.continuation;
				}
			};

			if (direction == "out") {
				return scanDirection(
						storage::LogicalGraphReadDirection::LGRD_OUTGOING, [&](const storage::LogicalGraphEdge &edge) {
							return edge.source_node_id == sourceNodeId && edge.target_node_id == targetNodeId;
						});
			}
			if (direction == "in") {
				return scanDirection(
						storage::LogicalGraphReadDirection::LGRD_INCOMING, [&](const storage::LogicalGraphEdge &edge) {
							return edge.source_node_id == targetNodeId && edge.target_node_id == sourceNodeId;
						});
			}
			if (const auto outgoing = scanDirection(storage::LogicalGraphReadDirection::LGRD_OUTGOING,
														[&](const storage::LogicalGraphEdge &edge) {
																return edge.source_node_id == sourceNodeId &&
																	   edge.target_node_id == targetNodeId;
															});
				outgoing.has_value()) {
				return outgoing;
			}
			return scanDirection(storage::LogicalGraphReadDirection::LGRD_INCOMING,
								 [&](const storage::LogicalGraphEdge &edge) {
									 return edge.source_node_id == targetNodeId && edge.target_node_id == sourceNodeId;
								 });
		}

		void applyMergeNode(const logical::LogicalMergeNode &operation, MergeRuntimeRow &row,
							MergeExecutionState &state) {
			validateMergeLabels(operation.getLabels());
			const PropertyMap matchProperties = evaluateMergeIdentityProperties(
					operation.getMatchProps(), operation.getMatchPropertyExpressions(), row, state);
			if (!operation.getVariable().empty()) {
				if (const auto existing = row.entities.find(operation.getVariable()); existing != row.entities.end()) {
					if (existing->second.entity_kind != LogicalGraphMutationEntityKind::LGMK_NODE) {
						throw std::logic_error("logical MERGE Node variable is bound to a relationship");
					}
					const auto value = row.row.values.find(operation.getVariable());
					if (value == row.row.values.end() ||
						!std::holds_alternative<storage::LogicalGraphNode>(value->second)) {
						throw std::logic_error("logical MERGE Node binding is malformed");
					}
					if (!operation.getOnMatchItems().empty()) {
						MutableGraphEntity &entity = mutableEntityForValue(state.modified_entities, value->second);
						PropertyMap &properties = mutableEntityProperties(entity, state.snapshot, state.bounds);
						entity.properties_changed = applyMergeActions(properties, operation.getOnMatchItems(),
																	  operation.getVariable(), row, state) ||
													entity.properties_changed;
					}
					row.recent_nodes.push_back(existing->second);
					return;
				}
			}

			MergeNodeMatch match;
			const MergeNodeMatchKey cacheKey{&operation, canonicalMergeIdentityProperties(matchProperties)};
			const auto cached = state.node_matches.find(cacheKey);
			if (cached != state.node_matches.end()) {
				match = cached->second;
			} else if (const auto existing = findMergeNode(operation, matchProperties, state); existing.has_value()) {
				match.reference = {LogicalGraphMutationEntityKind::LGMK_NODE,
								   LogicalGraphMutationReferenceKind::LGRK_PERSISTED, existing->node_id};
				match.node = *existing;
			} else {
				PropertyMap properties = matchProperties;
				const size_t ordinal = state.batch.node_creates.size();
				const LogicalGraphMutationEntityReference reference = createdNodeReference(ordinal);
				const storage::LogicalGraphNode actionNode{0U, operation.getLabels()};
				MergeRuntimeRow actionRow = row;
				if (!operation.getVariable().empty()) {
					actionRow.entities.insert_or_assign(operation.getVariable(), reference);
					actionRow.row.values.insert_or_assign(operation.getVariable(), actionNode);
					actionRow.row.property_cache.insert_or_assign(operation.getVariable(), properties);
				}
				static_cast<void>(applyMergeActions(properties, operation.getOnCreateItems(), operation.getVariable(),
													actionRow, state));
				LogicalGraphNodeCreateRequest request;
				request.labels = operation.getLabels();
				request.properties = sortedProperties(properties);
				state.batch.node_creates.push_back(std::move(request));
				state.created_node_properties.push_back(std::move(properties));
				match.reference = reference;
				match.node = actionNode;
				match.created = true;
			}
			state.node_matches.insert_or_assign(cacheKey, match);
			if (match.created) {
				bindMergeNode(row, operation.getVariable(), match.reference, match.node,
							  state.created_node_properties.at(static_cast<size_t>(match.reference.value)));
				return;
			}

			if (operation.getOnMatchItems().empty()) {
				bindMergeNode(row, operation.getVariable(), match.reference, match.node, std::nullopt);
				return;
			}
			MutableGraphEntity &entity =
					mutableEntityForValue(state.modified_entities, LogicalPlanReadValue(match.node));
			PropertyMap &properties = mutableEntityProperties(entity, state.snapshot, state.bounds);
			bindMergeNode(row, operation.getVariable(), match.reference, match.node, properties);
			entity.properties_changed =
					applyMergeActions(properties, operation.getOnMatchItems(), operation.getVariable(), row, state) ||
					entity.properties_changed;
		}

		void applyMergeEdge(const logical::LogicalMergeEdge &operation, MergeRuntimeRow &row,
							MergeExecutionState &state) {
			const std::string direction = normalizedMergeDirection(operation.getDirection());
			const PropertyMap matchProperties = evaluateMergeIdentityProperties(
					operation.getMatchProps(), operation.getMatchPropertyExpressions(), row, state);
			const LogicalGraphMutationEntityReference source = resolveMergeNode(row, operation.getSourceVar(), true);
			const LogicalGraphMutationEntityReference target = resolveMergeNode(row, operation.getTargetVar(), false);
			const MergeEdgeMatchKey cacheKey{&operation, source, target,
											 canonicalMergeIdentityProperties(matchProperties)};
			MergeEdgeMatch match;
			if (const auto cached = state.edge_matches.find(cacheKey); cached != state.edge_matches.end()) {
				match = cached->second;
			} else if (source.reference_kind == LogicalGraphMutationReferenceKind::LGRK_PERSISTED &&
					   target.reference_kind == LogicalGraphMutationReferenceKind::LGRK_PERSISTED) {
				if (const auto existing =
							findMergeEdge(operation, source.value, target.value, matchProperties, direction, state);
					existing.has_value()) {
					match.reference = {LogicalGraphMutationEntityKind::LGMK_EDGE,
									   LogicalGraphMutationReferenceKind::LGRK_PERSISTED, existing->edge_id};
					match.edge = *existing;
					match.resolved = true;
				}
			}
			if (!match.resolved) {
				PropertyMap properties = matchProperties;
				const size_t ordinal = state.batch.edge_creates.size();
				const LogicalGraphMutationEntityReference reference{
						LogicalGraphMutationEntityKind::LGMK_EDGE, LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE,
						ordinal};
				const storage::LogicalGraphEdge actionEdge{0U, 0U, 0U, operation.getEdgeType()};
				MergeRuntimeRow actionRow = row;
				if (!operation.getEdgeVar().empty()) {
					actionRow.entities.insert_or_assign(operation.getEdgeVar(), reference);
					actionRow.row.values.insert_or_assign(operation.getEdgeVar(), actionEdge);
					actionRow.row.property_cache.insert_or_assign(operation.getEdgeVar(), properties);
				}
				static_cast<void>(applyMergeActions(properties, operation.getOnCreateActions(), operation.getEdgeVar(),
													actionRow, state));
				LogicalGraphBatchEdgeCreateRequest request;
				request.source = direction == "in" ? target : source;
				request.target = direction == "in" ? source : target;
				request.type = operation.getEdgeType();
				request.properties = sortedProperties(properties);
				state.batch.edge_creates.push_back(std::move(request));
				state.created_edge_properties.push_back(std::move(properties));
				match.reference = reference;
				match.edge = actionEdge;
				match.created = true;
				match.resolved = true;
			}
			state.edge_matches.insert_or_assign(cacheKey, match);
			if (match.created) {
				bindMergeEdge(row, operation.getEdgeVar(), match.reference, match.edge,
							  state.created_edge_properties.at(static_cast<size_t>(match.reference.value)));
				return;
			}
			if (operation.getOnMatchActions().empty()) {
				bindMergeEdge(row, operation.getEdgeVar(), match.reference, match.edge, std::nullopt);
				return;
			}
			MutableGraphEntity &entity =
					mutableEntityForValue(state.modified_entities, LogicalPlanReadValue(match.edge));
			PropertyMap &properties = mutableEntityProperties(entity, state.snapshot, state.bounds);
			bindMergeEdge(row, operation.getEdgeVar(), match.reference, match.edge, properties);
			entity.properties_changed =
					applyMergeActions(properties, operation.getOnMatchActions(), operation.getEdgeVar(), row, state) ||
					entity.properties_changed;
		}

		[[nodiscard]] storage::LogicalGraphPropertyPatchRequest finalPropertyPatch(
			const MutableGraphEntity &entity, const QueryContext &context, size_t maximumScratchBytes) {
			storage::LogicalGraphPropertyPatchRequest patch{{entity.kind, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, entity.id}, {}, {}};
			// This owner accounts the continuous borrowed lookup descriptors.
			// Output patch payloads remain owned by the caller's mutation batch.
			MaterializedByteBudget scratchBudget(maximumScratchBytes);
			detail::BudgetMemoryResource scratchResource(scratchBudget);
			using OriginalProperty = std::pair<std::string_view, const PropertyValue *>;
			std::pmr::vector<OriginalProperty> original{&scratchResource};
			original.reserve(entity.direct_properties.size());
			for (const auto &property : entity.direct_properties) {
				context.checkGuard();
				original.emplace_back(property.key, &property.value);
				if (!entity.properties->contains(property.key)) patch.removals.push_back(property.key);
			}
			std::sort(original.begin(), original.end(), [&](const auto &left, const auto &right) {
				context.checkGuard(); return left.first < right.first;
			});
			for (const auto &[key, value] : *entity.properties) {
				context.checkGuard();
				const auto before = std::lower_bound(original.begin(), original.end(), std::string_view(key),
					[&](const auto &property, std::string_view candidate) {
						context.checkGuard(); return property.first < candidate;
					});
				if (before == original.end() || before->first != key || *before->second != value) patch.sets.push_back({key, value});
			}
			std::ranges::sort(patch.sets, {}, &storage::LogicalGraphWriteProperty::key);
			std::ranges::sort(patch.removals);
			return patch;
		}

		[[nodiscard]] MergeRuntimeRow makeMergeRow(LogicalPlanReadRow row) {
			MergeRuntimeRow result;
			result.row = std::move(row);
			for (const auto &[variable, value]: result.row.values) {
				if (const auto *node = std::get_if<storage::LogicalGraphNode>(&value)) {
					result.entities.emplace(variable,
											LogicalGraphMutationEntityReference{
													LogicalGraphMutationEntityKind::LGMK_NODE,
													LogicalGraphMutationReferenceKind::LGRK_PERSISTED, node->node_id});
				} else if (const auto *edge = std::get_if<storage::LogicalGraphEdge>(&value)) {
					result.entities.emplace(variable,
											LogicalGraphMutationEntityReference{
													LogicalGraphMutationEntityKind::LGMK_EDGE,
													LogicalGraphMutationReferenceKind::LGRK_PERSISTED, edge->edge_id});
				}
			}
			return result;
		}

		void visitMergeOperator(const logical::LogicalOperator &operation, MergeExecutionState &state,
								const MergeRowVisitor &visitor) {
			state.context.checkGuard();
			if (++state.operator_count > state.bounds.maximum_rows) {
				throw std::runtime_error("logical MERGE exceeded its mutation-operation bound");
			}
			const logical::LogicalOperator *child = requireOptionalOnlyChild(operation);
			const auto apply = [&](MergeRuntimeRow &&row) {
				if (operation.getType() == logical::LogicalOpType::LOP_MERGE_NODE) {
					const auto *node = dynamic_cast<const logical::LogicalMergeNode *>(&operation);
					if (node == nullptr) {
						throw std::logic_error("logical MERGE tree has an invalid MergeNode operator");
					}
					applyMergeNode(*node, row, state);
				} else if (operation.getType() == logical::LogicalOpType::LOP_MERGE_EDGE) {
					const auto *edge = dynamic_cast<const logical::LogicalMergeEdge *>(&operation);
					if (edge == nullptr) {
						throw std::logic_error("logical MERGE tree has an invalid MergeEdge operator");
					}
					applyMergeEdge(*edge, row, state);
				} else {
					throw std::logic_error("logical MERGE tree has an unsupported operator");
				}
				visitor(std::move(row));
			};
			if (child == nullptr) {
				apply({});
				return;
			}
			if (child->getType() == logical::LogicalOpType::LOP_MERGE_NODE ||
				child->getType() == logical::LogicalOpType::LOP_MERGE_EDGE) {
				visitMergeOperator(*child, state, apply);
				return;
			}
			const LogicalPlanReadRuntimeStats stats = state.reader.executeWithSnapshot(
					state.snapshot, *child, state.bounds,
					[&](const LogicalPlanReadRow &row) {
						apply(makeMergeRow(row));
						return true;
					},
					state.context);
			requireCompleteReadForMutation(stats, state.bounds, "logical MATCH ... MERGE");
		}

		void appendModifiedMergeEntities(MergeExecutionState &state) {
			for (const auto &[_, entity]: state.modified_entities) {
				if (!entity.properties_changed && !entity.labels_changed) {
					continue;
				}
				if (entity.properties_changed) {
					if (!entity.properties.has_value()) {
						throw std::logic_error("logical MERGE lost a staged Property collection");
					}
					if (state.snapshot.supportsShapedPropertyReadAdmission()) {
						auto patch = finalPropertyPatch(entity, state.context, state.bounds.maximum_materialized_bytes);
						if (!patch.sets.empty() || !patch.removals.empty()) {
							for (const size_t count : {entity.direct_properties.size(), patch.sets.size(), patch.removals.size()}) {
								if (count > (std::numeric_limits<size_t>::max)() - state.patch_entry_limit)
									throw std::overflow_error("logical MERGE Property patch entry count overflowed");
								state.patch_entry_limit += count;
							}
							state.property_patches.push_back(std::move(patch));
						}
					} else {
						LogicalGraphPropertyMutationRequest replacement;
						replacement.entity = {entity.kind, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, entity.id};
						replacement.properties = sortedProperties(*entity.properties);
						state.batch.property_replacements.push_back(std::move(replacement));
					}
				}
				if (entity.labels_changed) {
					if (entity.kind != LogicalGraphMutationEntityKind::LGMK_NODE) {
						throw std::logic_error("logical MERGE has a label change for a non-Node entity");
					}
					state.batch.node_label_replacements.push_back(
							{{LogicalGraphMutationEntityKind::LGMK_NODE,
							  LogicalGraphMutationReferenceKind::LGRK_PERSISTED, entity.id},
							 entity.labels});
				}
			}
		}

		void synchronizeCreatedMergeEntities(MergeExecutionState &state) {
			if (state.batch.node_creates.size() != state.created_node_properties.size() ||
				state.batch.edge_creates.size() != state.created_edge_properties.size()) {
				throw std::logic_error("logical MERGE lost its created-entity Property state");
			}
			for (size_t index = 0U; index < state.batch.node_creates.size(); ++index) {
				state.batch.node_creates[index].properties = sortedProperties(state.created_node_properties[index]);
			}
			for (size_t index = 0U; index < state.batch.edge_creates.size(); ++index) {
				state.batch.edge_creates[index].properties = sortedProperties(state.created_edge_properties[index]);
			}
		}

		[[nodiscard]] PropertyMap &mergePropertiesForPostMutation(const LogicalGraphMutationEntityReference &reference,
																  const std::string &variable, MergeRuntimeRow &row,
																  MergeExecutionState &state,
																  MutableGraphEntity **persistedEntity = nullptr) {
			if (reference.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE) {
				if (reference.entity_kind != LogicalGraphMutationEntityKind::LGMK_NODE ||
					reference.value >= state.created_node_properties.size()) {
					throw std::logic_error("logical MERGE SET has an invalid created Node reference");
				}
				return state.created_node_properties[static_cast<size_t>(reference.value)];
			}
			if (reference.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE) {
				if (reference.entity_kind != LogicalGraphMutationEntityKind::LGMK_EDGE ||
					reference.value >= state.created_edge_properties.size()) {
					throw std::logic_error("logical MERGE SET has an invalid created relationship reference");
				}
				return state.created_edge_properties[static_cast<size_t>(reference.value)];
			}
			if (reference.reference_kind != LogicalGraphMutationReferenceKind::LGRK_PERSISTED) {
				throw std::logic_error("logical MERGE SET has an invalid entity reference kind");
			}
			const auto value = row.row.values.find(variable);
			if (value == row.row.values.end()) {
				throw std::logic_error("logical MERGE SET could not resolve its entity binding");
			}
			MutableGraphEntity &entity = mutableEntityForValue(state.modified_entities, value->second);
			if (entity.kind != reference.entity_kind || entity.id != reference.value) {
				throw std::logic_error("logical MERGE SET entity binding does not match its stable reference");
			}
			if (persistedEntity != nullptr) {
				*persistedEntity = &entity;
			}
			return mutableEntityProperties(entity, state.snapshot, state.bounds);
		}

		void applyMergePostMutations(const std::vector<const logical::LogicalOperator *> &operations,
									 MergeRuntimeRow &row, MergeExecutionState &state) {
			for (const logical::LogicalOperator *operation: operations) {
				if (operation == nullptr) {
					throw std::logic_error("logical MERGE mutation chain contains a null operation");
				}
				if (operation->getType() == logical::LogicalOpType::LOP_SET) {
					const auto *set = dynamic_cast<const logical::LogicalSet *>(operation);
					if (set == nullptr || set->getItems().empty()) { // collectMergePostMutationChain verifies the concrete SET type and nonempty shape before constructing this private chain.
						throw std::logic_error("logical MERGE mutation chain has an invalid SET operation");
					}
					for (const logical::LogicalSetItem &item: set->getItems()) {
						state.context.checkGuard();
						if (item.variable.empty()) {
							throw std::logic_error("logical MERGE SET has an empty entity variable");
						}
						const auto reference = row.entities.find(item.variable);
						if (reference == row.entities.end()) {
							throw std::logic_error("logical MERGE SET could not resolve its entity variable");
						}
						if (item.type == logical::SetActionType::LSET_PROPERTY ||
							item.type == logical::SetActionType::LSET_MAP_REPLACE ||
							item.type == logical::SetActionType::LSET_MAP_MERGE) {
							if (!item.expression ||
								(item.type == logical::SetActionType::LSET_PROPERTY && item.key.empty())) {
								throw std::logic_error("logical MERGE SET has an incomplete Property assignment");
							}
							MutableGraphEntity *persisted = nullptr;
							PropertyMap &properties = mergePropertiesForPostMutation(reference->second, item.variable,
																					 row, state, &persisted);
							const PropertyValue value = evaluateCreatePropertyExpression(
									*item.expression, state.context, row.row, state.snapshot, state.bounds,
									row.row.property_cache);
							bool changed = false;
							if (item.type == logical::SetActionType::LSET_PROPERTY) {
								if (value.getType() == PropertyType::NULL_TYPE) {
									changed = properties.erase(item.key) != 0U;
								} else if (const auto existing = properties.find(item.key);
										   existing == properties.end() || existing->second != value) {
									assignBoundedProperty(properties, item.key, value, state.bounds.maximum_property_entries);
									changed = true;
								}
							} else if (item.type == logical::SetActionType::LSET_MAP_REPLACE) {
								changed = replacePropertyMap(properties, value, state.context, state.bounds.maximum_property_entries);
							} else {
								changed = mergePropertyMap(properties, value, state.context, state.bounds.maximum_property_entries);
							}
							if (persisted != nullptr) {
								persisted->properties_changed = persisted->properties_changed || changed;
							}
							synchronizeMergePropertyAliases(row, item.variable, properties, state.context);
							continue;
						}
						if (item.type != logical::SetActionType::LSET_LABEL || item.key.empty() ||
							reference->second.entity_kind != LogicalGraphMutationEntityKind::LGMK_NODE) {
							throw std::logic_error("logical MERGE SET has an unsupported label assignment");
						}
						const auto binding = row.row.values.find(item.variable);
						if (binding == row.row.values.end() ||
							!std::holds_alternative<storage::LogicalGraphNode>(binding->second)) {
							throw std::logic_error("logical MERGE SET label target is not a Node");
						}
						if (reference->second.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE) {
							const size_t ordinal = static_cast<size_t>(reference->second.value);
							if (ordinal >= state.batch.node_creates.size()) {
								throw std::logic_error("logical MERGE SET has an invalid created Node label reference");
							}
							auto &labels = state.batch.node_creates[ordinal].labels;
							if (std::ranges::find(labels, item.key) == labels.end()) {
								labels.push_back(item.key);
								std::get<storage::LogicalGraphNode>(binding->second).labels = labels;
							}
						} else {
							MutableGraphEntity &entity =
									mutableEntityForValue(state.modified_entities, binding->second);
							if (std::ranges::find(entity.labels, item.key) == entity.labels.end()) {
								entity.labels.push_back(item.key);
								entity.labels_changed = true;
								std::get<storage::LogicalGraphNode>(binding->second).labels = entity.labels;
							}
						}
					}
					continue;
				}
				if (operation->getType() != logical::LogicalOpType::LOP_REMOVE) { // the private merge-post chain collector admits only SET and REMOVE operations.
					throw std::logic_error("logical MERGE mutation chain has an unsupported operation");
				}
				const auto *remove = dynamic_cast<const logical::LogicalRemove *>(operation);
				if (remove == nullptr || remove->getItems().empty()) { // collectMergePostMutationChain verifies the concrete REMOVE type and nonempty shape before constructing this private chain.
					throw std::logic_error("logical MERGE mutation chain has an invalid REMOVE operation");
				}
				for (const logical::LogicalRemoveItem &item: remove->getItems()) {
					state.context.checkGuard();
					const auto reference = row.entities.find(item.variable);
					if (item.variable.empty() || item.key.empty() || reference == row.entities.end()) {
						throw std::logic_error("logical MERGE REMOVE has an invalid entity target");
					}
					if (item.type == logical::LogicalRemoveActionType::LREM_PROPERTY) {
						MutableGraphEntity *persisted = nullptr;
						PropertyMap &properties = mergePropertiesForPostMutation(reference->second, item.variable, row,
																				 state, &persisted);
						const bool changed = properties.erase(item.key) != 0U;
						if (persisted != nullptr) {
							persisted->properties_changed = persisted->properties_changed || changed;
						}
						synchronizeMergePropertyAliases(row, item.variable, properties, state.context);
						continue;
					}
					if (item.type != logical::LogicalRemoveActionType::LREM_LABEL ||
						reference->second.entity_kind != LogicalGraphMutationEntityKind::LGMK_NODE) {
						throw std::logic_error("logical MERGE REMOVE has an unsupported label target");
					}
					const auto binding = row.row.values.find(item.variable);
					if (binding == row.row.values.end() ||
						!std::holds_alternative<storage::LogicalGraphNode>(binding->second)) {
						throw std::logic_error("logical MERGE REMOVE label target is not a Node");
					}
					if (reference->second.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE) {
						const size_t ordinal = static_cast<size_t>(reference->second.value);
						if (ordinal >= state.batch.node_creates.size()) {
							throw std::logic_error("logical MERGE REMOVE has an invalid created Node label reference");
						}
						auto &labels = state.batch.node_creates[ordinal].labels;
						labels.erase(std::remove(labels.begin(), labels.end(), item.key), labels.end());
						std::get<storage::LogicalGraphNode>(binding->second).labels = labels;
					} else {
						MutableGraphEntity &entity = mutableEntityForValue(state.modified_entities, binding->second);
						const size_t previous = entity.labels.size();
						entity.labels.erase(std::remove(entity.labels.begin(), entity.labels.end(), item.key),
											entity.labels.end());
						entity.labels_changed = entity.labels_changed || entity.labels.size() != previous;
						std::get<storage::LogicalGraphNode>(binding->second).labels = entity.labels;
					}
				}
			}
		}

		struct MergePostMutationChain {
			const logical::LogicalOperator *merge_root = nullptr;
			std::vector<const logical::LogicalOperator *> operations;
		};

		struct MergeResultPipeline {
			MergePostMutationChain mutation_chain;
			const logical::LogicalSort *sort = nullptr;
			const logical::LogicalProject *projection = nullptr;
			const logical::LogicalSkip *skip = nullptr;
			const logical::LogicalLimit *limit = nullptr;
		};

		// Cache only values that cannot observe a newly assigned durable entity ID.
		// This preserves publication-time IDs for created entities while avoiding a
		// second evaluation of volatile scalar expressions in RETURN and ORDER BY.
		struct MergePrepublicationResultCache {
			std::vector<std::vector<std::optional<PropertyValue>>> sort_keys;
			std::vector<std::vector<std::optional<ResultValue>>> projection_values;
		};

		[[nodiscard]] std::optional<MergePostMutationChain>
		collectMergePostMutationChain(const logical::LogicalOperator &root) {
			MergePostMutationChain chain;
			const logical::LogicalOperator *current = &root;
			while (current != nullptr && (current->getType() == logical::LogicalOpType::LOP_SET ||
										  current->getType() == logical::LogicalOpType::LOP_REMOVE)) {
				if (current->getType() == logical::LogicalOpType::LOP_SET) {
					const auto *set = dynamic_cast<const logical::LogicalSet *>(current);
					if (set == nullptr || set->getItems().empty()) {
						throw std::logic_error("logical MERGE mutation chain has an invalid SET operation");
					}
				} else {
					const auto *remove = dynamic_cast<const logical::LogicalRemove *>(current);
					if (remove == nullptr || remove->getItems().empty()) {
						throw std::logic_error("logical MERGE mutation chain has an invalid REMOVE operation");
					}
				}
				chain.operations.push_back(current);
				current = requireOptionalOnlyChild(*current);
			}
			if (current == nullptr || (current->getType() != logical::LogicalOpType::LOP_MERGE_NODE &&
									   current->getType() != logical::LogicalOpType::LOP_MERGE_EDGE)) {
				return std::nullopt;
			}
			if ((current->getType() == logical::LogicalOpType::LOP_MERGE_NODE &&
				 dynamic_cast<const logical::LogicalMergeNode *>(current) == nullptr) ||
				(current->getType() == logical::LogicalOpType::LOP_MERGE_EDGE &&
				 dynamic_cast<const logical::LogicalMergeEdge *>(current) == nullptr)) {
				throw std::logic_error("logical MERGE mutation chain has an invalid merge operator");
			}
			std::ranges::reverse(chain.operations);
			chain.merge_root = current;
			return chain;
		}

		[[nodiscard]] std::optional<MergeResultPipeline>
		collectMergeResultPipeline(const logical::LogicalOperator &root) {
			// The logical builder places mutation result operators outside the write:
			// LIMIT -> SKIP -> PROJECT -> SORT -> MERGE.  Keep the write and output
			// pipelines separate so pagination never reduces the set of rows mutated.
			MergeResultPipeline pipeline;
			const logical::LogicalOperator *current = &root;
			if (current->getType() == logical::LogicalOpType::LOP_LIMIT) {
				pipeline.limit = dynamic_cast<const logical::LogicalLimit *>(current);
				if (pipeline.limit == nullptr) {
					throw std::logic_error("logical MERGE result pipeline has an invalid Limit operator");
				}
				current = requireOptionalOnlyChild(*current);
			}
			if (current != nullptr && current->getType() == logical::LogicalOpType::LOP_SKIP) {
				pipeline.skip = dynamic_cast<const logical::LogicalSkip *>(current);
				if (pipeline.skip == nullptr) {
					throw std::logic_error("logical MERGE result pipeline has an invalid Skip operator");
				}
				current = requireOptionalOnlyChild(*current);
			}
			if (current != nullptr && current->getType() == logical::LogicalOpType::LOP_PROJECT) {
				pipeline.projection = dynamic_cast<const logical::LogicalProject *>(current);
				if (pipeline.projection == nullptr) {
					throw std::logic_error("logical MERGE result pipeline has an invalid Project operator");
				}
				current = requireOptionalOnlyChild(*current);
			}
			if (current != nullptr && current->getType() == logical::LogicalOpType::LOP_SORT) {
				pipeline.sort = dynamic_cast<const logical::LogicalSort *>(current);
				if (pipeline.sort == nullptr) {
					throw std::logic_error("logical MERGE result pipeline has an invalid Sort operator");
				}
				current = requireOptionalOnlyChild(*current);
			}
			if (current == nullptr) {
				return std::nullopt;
			}
			const std::optional<MergePostMutationChain> mutationChain = collectMergePostMutationChain(*current);
			if (!mutationChain.has_value()) {
				return std::nullopt;
			}
			pipeline.mutation_chain = *mutationChain;
			return pipeline;
		}

		[[nodiscard]] bool mergeBatchHasMutations(const LogicalGraphMutationBatch &batch) {
			return !batch.node_creates.empty() || !batch.edge_creates.empty() || !batch.property_replacements.empty() ||
				   !batch.node_label_replacements.empty() || !batch.edge_deletes.empty() || !batch.node_deletes.empty();
		}

		[[nodiscard]] uint64_t resolveMergeNodeId(const LogicalGraphMutationEntityReference &reference,
												  const storage::LogicalGraphMutationBatchResult &result) {
			if (reference.entity_kind != LogicalGraphMutationEntityKind::LGMK_NODE) {
				throw std::logic_error("logical MERGE relationship endpoint is not a Node");
			}
			if (reference.reference_kind == LogicalGraphMutationReferenceKind::LGRK_PERSISTED) {
				return reference.value;
			}
			if (reference.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE) {
				return result.created_node_ids.at(static_cast<size_t>(reference.value));
			}
			throw std::logic_error("logical MERGE relationship endpoint has an invalid reference kind");
		}

		[[nodiscard]] ResultValue materializeMergeReference(const LogicalGraphMutationEntityReference &reference,
															const MergeExecutionState &state,
															const storage::LogicalGraphMutationBatchResult &result) {
			if (reference.entity_kind == LogicalGraphMutationEntityKind::LGMK_NODE) {
				if (reference.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE) {
					const size_t ordinal = static_cast<size_t>(reference.value);
					return LogicalNodeResult{result.created_node_ids.at(ordinal),
											 state.batch.node_creates.at(ordinal).labels,
											 state.created_node_properties.at(ordinal)};
				}
				if (reference.reference_kind != LogicalGraphMutationReferenceKind::LGRK_PERSISTED) {
					throw std::logic_error("logical MERGE has an invalid Node projection reference");
				}
				const storage::LogicalGraphNode node = state.snapshot.readNodeShaped(reference.value, completeNodeReadShape());
				const auto changed = state.modified_entities.find(
						{static_cast<uint8_t>(LogicalGraphMutationEntityKind::LGMK_NODE), reference.value});
				const PropertyMap properties =
						changed != state.modified_entities.end() && changed->second.properties.has_value()
								? *changed->second.properties
								: readNodeProperties(state.snapshot, node, state.bounds.maximum_property_entries);
				const std::vector<std::string> &labels =
						changed != state.modified_entities.end() ? changed->second.labels : node.labels;
				return LogicalNodeResult{node.node_id, labels, properties};
			}
			if (reference.entity_kind != LogicalGraphMutationEntityKind::LGMK_EDGE) {
				throw std::logic_error("logical MERGE has an invalid projection entity kind");
			}
			if (reference.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE) {
				const size_t ordinal = static_cast<size_t>(reference.value);
				const LogicalGraphBatchEdgeCreateRequest &request = state.batch.edge_creates.at(ordinal);
				return LogicalEdgeResult{result.created_edge_ids.at(ordinal),
										 resolveMergeNodeId(request.source, result),
										 resolveMergeNodeId(request.target, result), request.type,
										 state.created_edge_properties.at(ordinal)};
			}
			if (reference.reference_kind != LogicalGraphMutationReferenceKind::LGRK_PERSISTED) {
				throw std::logic_error("logical MERGE has an invalid relationship projection reference");
			}
			const storage::LogicalGraphEdge edge = state.snapshot.readEdgeShaped(reference.value, completeEdgeReadShape());
			const auto changed = state.modified_entities.find(
					{static_cast<uint8_t>(LogicalGraphMutationEntityKind::LGMK_EDGE), reference.value});
			const PropertyMap properties =
					changed != state.modified_entities.end() && changed->second.properties.has_value()
							? *changed->second.properties
							: readEdgeProperties(state.snapshot, edge, state.bounds.maximum_property_entries);
			return LogicalEdgeResult{edge.edge_id, edge.source_node_id, edge.target_node_id, edge.type, properties};
		}

		[[nodiscard]] ResultValue
		materializePrepublicationMergeReference(const LogicalGraphMutationEntityReference &reference,
												const MergeExecutionState &state) {
			const auto provisionalNodeId = [](const LogicalGraphMutationEntityReference &nodeReference) {
				if (nodeReference.entity_kind != LogicalGraphMutationEntityKind::LGMK_NODE) {
					throw std::logic_error("logical MERGE relationship endpoint is not a Node");
				}
				if (nodeReference.reference_kind == LogicalGraphMutationReferenceKind::LGRK_PERSISTED) {
					return nodeReference.value;
				}
				if (nodeReference.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE) {
					return std::numeric_limits<uint64_t>::max() - nodeReference.value;
				}
				throw std::logic_error("logical MERGE relationship endpoint has an invalid reference kind");
			};
			if (reference.entity_kind == LogicalGraphMutationEntityKind::LGMK_NODE &&
				reference.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE) {
				const size_t ordinal = static_cast<size_t>(reference.value);
				return LogicalNodeResult{std::numeric_limits<uint64_t>::max() - reference.value,
										 state.batch.node_creates.at(ordinal).labels,
										 state.created_node_properties.at(ordinal)};
			}
			if (reference.entity_kind == LogicalGraphMutationEntityKind::LGMK_EDGE &&
				reference.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE) {
				const size_t ordinal = static_cast<size_t>(reference.value);
				const LogicalGraphBatchEdgeCreateRequest &request = state.batch.edge_creates.at(ordinal);
				return LogicalEdgeResult{std::numeric_limits<uint64_t>::max() - reference.value,
										 provisionalNodeId(request.source), provisionalNodeId(request.target),
										 request.type, state.created_edge_properties.at(ordinal)};
			}
			return materializeMergeReference(reference, state, {});
		}

		[[nodiscard]] bool mergeExpressionDependsOnCreatedEntity(const expressions::Expression &expression,
																 const MergeRuntimeRow &row) {
			std::set<std::string> variables;
			expressions::ExpressionUtils::collectVariables(&expression, variables);
			return std::ranges::any_of(variables, [&row](const std::string &variable) {
				const auto entity = row.entities.find(variable);
				return entity != row.entities.end() &&
					   (entity->second.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE ||
						entity->second.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE);
			});
		}

		[[nodiscard]] MergePrepublicationResultCache
		preflightMergeResultExpressions(const MergeResultPipeline &pipeline, const std::vector<MergeRuntimeRow> &rows,
										const MergeExecutionState &state) {
			MergePrepublicationResultCache cache;
			const auto resolve = [&state](const MergeRuntimeRow &row, const std::string &variable) {
				if (const auto entity = row.entities.find(variable); entity != row.entities.end()) {
					return materializePrepublicationMergeReference(entity->second, state);
				}
				const auto scalar = row.row.values.find(variable);
				if (scalar != row.row.values.end() && std::holds_alternative<PropertyValue>(scalar->second)) {
					return ResultValue(std::get<PropertyValue>(scalar->second));
				}
				throw std::logic_error("logical MERGE RETURN could not resolve a variable");
			};
			const auto evaluate = [&](const expressions::Expression &expression, const MergeRuntimeRow &row) {
				return evaluateMutationProjectionExpression(
						expression, row.row, state.snapshot, state.bounds, state.context,
						[&resolve, &row](const LogicalPlanReadRow &, const std::string &variable) {
							return resolve(row, variable);
						});
			};
			// Sort evaluation is deliberately deferred for a single row by the read runtime.
			// Preserve that behavior here while still rejecting failures before publication.
			if (pipeline.sort != nullptr && rows.size() > 1U) {
				const std::vector<logical::LogicalSortItem> &items = pipeline.sort->getSortItems();
				if (items.empty()) {
					throw std::logic_error("logical MERGE ORDER BY has no sort items");
				}
				cache.sort_keys.assign(rows.size(), std::vector<std::optional<PropertyValue>>(items.size()));
				for (size_t rowIndex = 0U; rowIndex < rows.size(); ++rowIndex) {
					const MergeRuntimeRow &row = rows[rowIndex];
					for (size_t itemIndex = 0U; itemIndex < items.size(); ++itemIndex) {
						const logical::LogicalSortItem &item = items[itemIndex];
						state.context.checkGuard();
						if (!item.expression) {
							throw std::logic_error("logical MERGE ORDER BY has an empty sort expression");
						}
						const ResultValue value = evaluate(*item.expression, row);
						if (!value.isPrimitive()) {
							throw std::logic_error(
									"logical MERGE ORDER BY expression must evaluate to a Property value");
						}
						if (!mergeExpressionDependsOnCreatedEntity(*item.expression, row)) {
							cache.sort_keys[rowIndex][itemIndex] = value.asPrimitive();
						}
					}
				}
			}
			if (pipeline.projection == nullptr) {
				return cache;
			}
			for (const logical::LogicalProjectItem &item: pipeline.projection->getItems()) {
				if (item.alias.empty() || !item.expression) {
					throw std::logic_error("logical MERGE RETURN has an invalid output item");
				}
			}
			const std::vector<logical::LogicalProjectItem> &items = pipeline.projection->getItems();
			cache.projection_values.assign(rows.size(), std::vector<std::optional<ResultValue>>(items.size()));
			for (size_t rowIndex = 0U; rowIndex < rows.size(); ++rowIndex) {
				const MergeRuntimeRow &row = rows[rowIndex];
				for (size_t itemIndex = 0U; itemIndex < items.size(); ++itemIndex) {
					const logical::LogicalProjectItem &item = items[itemIndex];
					state.context.checkGuard();
					ResultValue value = evaluate(*item.expression, row);
					if (!mergeExpressionDependsOnCreatedEntity(*item.expression, row)) {
						cache.projection_values[rowIndex][itemIndex] = std::move(value);
					}
				}
			}
			return cache;
		}

		[[nodiscard]] QueryResult buildMergeResult(const MergeResultPipeline &pipeline,
												   const std::vector<std::string> &implicitColumns,
												   const std::vector<MergeRuntimeRow> &rows,
												   const MergeExecutionState &state,
												   const storage::LogicalGraphMutationBatchResult &result,
												   const MergePrepublicationResultCache &prepublicationCache) {
			QueryResult queryResult;
			queryResult.setColumns(pipeline.projection != nullptr ? pipeline.projection->getOutputVariables()
																  : implicitColumns);
			std::vector<size_t> outputOrder;
			outputOrder.reserve(rows.size());
			for (size_t index = 0U; index < rows.size(); ++index) {
				outputOrder.push_back(index);
			}
			const auto resolveForRow = [&](const MergeRuntimeRow &mergeRow,
										   const std::string &variable) -> ResultValue {
				if (const auto entity = mergeRow.entities.find(variable); entity != mergeRow.entities.end()) {
					return materializeMergeReference(entity->second, state, result);
				}
				const auto scalar = mergeRow.row.values.find(variable);
				if (scalar != mergeRow.row.values.end() && std::holds_alternative<PropertyValue>(scalar->second)) {
					return std::get<PropertyValue>(scalar->second);
				}
				throw std::logic_error("logical MERGE RETURN could not resolve a variable");
			};
			if (pipeline.sort != nullptr && outputOrder.size() > 1U) {
				const std::vector<logical::LogicalSortItem> &items = pipeline.sort->getSortItems();
				if (items.empty()) {
					throw std::logic_error("logical MERGE ORDER BY has no sort items");
				}
				struct DecoratedRow {
					size_t index = 0U;
					std::vector<PropertyValue> keys;
				};
				std::vector<DecoratedRow> decoratedRows;
				decoratedRows.reserve(outputOrder.size());
				for (const size_t index: outputOrder) {
					DecoratedRow decorated{index, {}};
					decorated.keys.reserve(items.size());
					for (size_t itemIndex = 0U; itemIndex < items.size(); ++itemIndex) {
						const logical::LogicalSortItem &item = items[itemIndex];
						state.context.checkGuard();
						if (!item.expression) {
							throw std::logic_error("logical MERGE ORDER BY has an empty sort expression");
						}
						if (index < prepublicationCache.sort_keys.size() &&
							itemIndex < prepublicationCache.sort_keys[index].size() &&
							prepublicationCache.sort_keys[index][itemIndex].has_value()) {
							decorated.keys.push_back(*prepublicationCache.sort_keys[index][itemIndex]);
							continue;
						}
						const ResultValue key = evaluateMutationProjectionExpression(
								*item.expression, rows[index].row, state.snapshot, state.bounds, state.context,
								[&resolveForRow, &mergeRow = rows[index]](const LogicalPlanReadRow &,
																		  const std::string &variable) {
									return resolveForRow(mergeRow, variable);
								});
						if (!key.isPrimitive()) {
							throw std::logic_error(
									"logical MERGE ORDER BY expression must evaluate to a Property value");
						}
						decorated.keys.push_back(key.asPrimitive());
					}
					decoratedRows.push_back(std::move(decorated));
				}
				std::stable_sort(
						decoratedRows.begin(), decoratedRows.end(),
						[&items, &context = state.context](const DecoratedRow &left, const DecoratedRow &right) {
							context.checkGuard();
							for (size_t index = 0U; index < items.size(); ++index) {
								if (left.keys[index] == right.keys[index]) {
									continue;
								}
								return items[index].ascending ? left.keys[index] < right.keys[index]
															  : left.keys[index] > right.keys[index];
							}
							return false;
						});
				outputOrder.clear();
				for (const DecoratedRow &row: decoratedRows) {
					outputOrder.push_back(row.index);
				}
			}
			size_t first = 0U;
			if (pipeline.skip != nullptr) {
				if (pipeline.skip->getOffset() < 0) {
					throw std::logic_error("logical MERGE SKIP cannot be negative");
				}
				const uint64_t offset = static_cast<uint64_t>(pipeline.skip->getOffset());
				first = offset >= outputOrder.size() ? outputOrder.size() : static_cast<size_t>(offset);
			}
			size_t last = outputOrder.size();
			if (pipeline.limit != nullptr) {
				if (pipeline.limit->getLimit() < 0) {
					throw std::logic_error("logical MERGE LIMIT cannot be negative");
				}
				const uint64_t limit = static_cast<uint64_t>(pipeline.limit->getLimit());
				const size_t remaining = outputOrder.size() - first;
				if (limit < remaining) {
					last = first + static_cast<size_t>(limit);
				}
			}
			queryResult.setPipelineRowCount(last - first);
			for (size_t orderedIndex = first; orderedIndex < last; ++orderedIndex) {
				const MergeRuntimeRow &mergeRow = rows[outputOrder[orderedIndex]];
				QueryResult::Row output;
				if (pipeline.projection == nullptr) {
					if (implicitColumns.empty()) {
						continue;
					}
					output.reserve(implicitColumns.size());
					for (const std::string &column: implicitColumns) {
						state.context.checkGuard();
						if (!output.emplace(column, resolveForRow(mergeRow, column)).second) {
							throw std::logic_error("logical MERGE produced duplicate output columns");
						}
					}
					queryResult.addRow(std::move(output));
					continue;
				}
				const std::vector<logical::LogicalProjectItem> &items = pipeline.projection->getItems();
				for (size_t itemIndex = 0U; itemIndex < items.size(); ++itemIndex) {
					const logical::LogicalProjectItem &item = items[itemIndex];
					state.context.checkGuard();
					if (item.alias.empty() || !item.expression) {
						throw std::logic_error("logical MERGE RETURN has an invalid output item");
					}
					ResultValue value;
					const size_t rowIndex = outputOrder[orderedIndex];
					if (rowIndex < prepublicationCache.projection_values.size() &&
						itemIndex < prepublicationCache.projection_values[rowIndex].size() &&
						prepublicationCache.projection_values[rowIndex][itemIndex].has_value()) {
						value = *prepublicationCache.projection_values[rowIndex][itemIndex];
					} else {
						value = evaluateMutationProjectionExpression(
								*item.expression, mergeRow.row, state.snapshot, state.bounds, state.context,
								[&resolveForRow, &mergeRow](const LogicalPlanReadRow &, const std::string &variable) {
									return resolveForRow(mergeRow, variable);
								});
					}
					if (!output.emplace(item.alias, std::move(value)).second) {
						throw std::logic_error("logical MERGE RETURN has duplicate output aliases");
					}
				}
				queryResult.addRow(std::move(output));
			}
			return queryResult;
		}

