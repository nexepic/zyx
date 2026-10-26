		// Extract only the semantics-preserving subset of an outer Filter that can
		// be used to drive the global topology cursor.  The complete expression is
		// still evaluated by the normal Filter visitor, so this is an acceleration
		// hint rather than a second predicate implementation.  We intentionally
		// accept conjunctions of edge-property/literal equalities only; OR, dynamic
		// expressions, and comparisons with unknown values retain the generic path.
		// Views are valid for this synchronous execution: the logical plan owns
		// literal payloads and inline predicate values throughout the call.
		struct DirectEdgePredicateView {
			std::string_view key;
			const PropertyValue *value = nullptr;
			const std::string *text = nullptr;
			PropertyValue scalar;

			[[nodiscard]] bool matches(const PropertyValue &actual) const {
				if (text != nullptr) {
					const auto *actualText = std::get_if<std::string>(&actual.getVariant());
					return actualText != nullptr && *actualText == *text;
				}
				return actual == (value == nullptr ? scalar : *value);
			}
			[[nodiscard]] bool sameValue(const DirectEdgePredicateView &other) const {
				if (text == nullptr)
					return other.matches(value == nullptr ? scalar : *value);
				if (other.text != nullptr)
					return *text == *other.text;
				const auto &actual = other.value == nullptr ? other.scalar : *other.value;
				const auto *otherText = std::get_if<std::string>(&actual.getVariant());
				return otherText != nullptr && *text == *otherText;
			}
			[[nodiscard]] PropertyValue copyValue() const {
				return text != nullptr ? PropertyValue(*text) : value == nullptr ? scalar : *value;
			}
		};

		[[nodiscard]] std::optional<std::vector<DirectEdgePredicateView>>
		inspectDirectEdgePropertyFilterViews(const expressions::Expression &expression,
											 const std::string &edgeVariable) {
			if (edgeVariable.empty()) {
				return std::nullopt;
			}
			std::vector<DirectEdgePredicateView> result;
			const auto visit = [&](const auto &self, const expressions::Expression &current) -> bool {
				using ExpressionType = expressions::ExpressionType;
				if (current.getExpressionType() == ExpressionType::BINARY_OP) {
					const auto *binary = dynamic_cast<const expressions::BinaryOpExpression *>(&current);
					if (binary == nullptr || binary->getLeft() == nullptr || binary->getRight() == nullptr) {
						return false;
					}
					if (binary->getOperator() == expressions::BinaryOperatorType::BOP_AND) {
						return self(self, *binary->getLeft()) && self(self, *binary->getRight());
					}
					if (binary->getOperator() != expressions::BinaryOperatorType::BOP_EQUAL) {
						return false;
					}
					const auto *left =
							dynamic_cast<const expressions::VariableReferenceExpression *>(binary->getLeft());
					const auto *right =
							dynamic_cast<const expressions::VariableReferenceExpression *>(binary->getRight());
					const expressions::VariableReferenceExpression *property = nullptr;
					const expressions::Expression *literal = nullptr;
					if (left != nullptr && left->hasProperty() && left->getVariableName() == edgeVariable) {
						property = left;
						literal = binary->getRight();
					} else if (right != nullptr && right->hasProperty() && right->getVariableName() == edgeVariable) {
						property = right;
						literal = binary->getLeft();
					}
					if (property == nullptr || literal == nullptr ||
						literal->getExpressionType() != ExpressionType::LITERAL ||
						property->getPropertyName().empty()) {
						return false;
					}
					const auto &constant = dynamic_cast<const expressions::LiteralExpression &>(*literal);
					DirectEdgePredicateView predicate{property->getPropertyName(), nullptr, nullptr, PropertyValue()};
					if (constant.isNull())
						return false;
					if (constant.isPropertyValue()) {
						predicate.value = &constant.getPropertyValue();
						if (isNull(*predicate.value))
							return false;
					} else if (constant.isString())
						predicate.text = &constant.getStringValue();
					else if (constant.isBoolean())
						predicate.scalar = PropertyValue(constant.getBooleanValue());
					else if (constant.isInteger())
						predicate.scalar = PropertyValue(constant.getIntegerValue());
					else
						predicate.scalar = PropertyValue(constant.getDoubleValue());
					const auto existing = std::ranges::find(result, predicate.key, &DirectEdgePredicateView::key);
					if (existing != result.end())
						return existing->sameValue(predicate);
					result.push_back(std::move(predicate));
					return true;
				}
				return false;
			};
			if (!visit(visit, expression) || result.empty()) {
				return std::nullopt;
			}
			return result;
		}

		[[nodiscard]] std::optional<std::vector<std::pair<std::string, PropertyValue>>>
		inspectDirectEdgePropertyFilter(const expressions::Expression &expression, const std::string &edgeVariable,
										ExecutionState &) {
			const auto views = inspectDirectEdgePropertyFilterViews(expression, edgeVariable);
			if (!views.has_value())
				return std::nullopt;
			std::vector<std::pair<std::string, PropertyValue>> result;
			result.reserve(views->size());
			for (const auto &view: *views)
				result.emplace_back(std::string(view.key), view.copyValue());
			return result;
		}

		// COUNT(*) and COUNT(n) over a simple Node scan need only logical IDs and,
		// when present, label tokens. Keep this a logical-shape path so every
		// equivalent plan can reuse it without manufacturing per-Node rows.
		[[nodiscard]] std::optional<bool> tryVisitDirectNodeCount(const logical::LogicalAggregate &aggregate,
																  ExecutionState &state,
																  const InternalVisitor &visitor) {
			const auto &groupExpressions = aggregate.getGroupByExprs();
			const auto &aggregations = aggregate.getAggregations();
			if (!groupExpressions.empty() || aggregations.size() != 1U ||
				!state.shared.snapshot.supportsNativeNodeRefScans()) {
				return std::nullopt;
			}
			const logical::LogicalAggItem &count = aggregations.front();
			if (count.functionName != "count" || count.distinct || count.alias.empty() || count.extraArg) {
				return std::nullopt;
			}

			const std::vector<logical::LogicalOperator *> children = aggregate.getChildren();
			if (children.size() != 1U || children.front() == nullptr ||
				children.front()->getType() != logical::LogicalOpType::LOP_NODE_SCAN) {
				return std::nullopt;
			}
			const auto &scan = dynamic_cast<const logical::LogicalNodeScan &>(*children.front());
			if (!scan.getChildren().empty() || scan.getVariable().empty() || !scan.getPropertyPredicates().empty() ||
				!scan.getRangePredicates().empty() || scan.getCompositeEquality().has_value()) {
				return std::nullopt;
			}
			// Parameter predicates are resolved at execution time rather than stored in
			// getPropertyPredicates(). A count fast path is valid only when that
			// resolved set is empty; otherwise it would silently count rows that the
			// ordinary Node scan filters out.
			const auto parameterPredicates = resolveParameterPropertyPredicates(scan, state);
			if (!parameterPredicates.has_value()) {
				LogicalPlanReadRow result;
				result.values.emplace(count.alias, PropertyValue(int64_t{0}));
				return visitor(std::move(result));
			}
			if (!parameterPredicates->empty()) {
				return std::nullopt;
			}
			if (count.argument != nullptr) {
				const auto *variable =
						dynamic_cast<const expressions::VariableReferenceExpression *>(count.argument.get());
				if (variable == nullptr || variable->hasProperty() ||
					variable->getVariableName() != scan.getVariable()) {
					return std::nullopt;
				}
			}

			debug::ScopedPerfTimer timer("logical_plan.direct_node_count");
			size_t inputRows = 0U;
			uint64_t previousNodeId = 0U;
			auto visitReference = [&](const storage::EntityRef &reference, bool labelsMatch) {
				checkGuard(state);
				validateNodeRef(reference,
								"logical graph direct Node count returned an invalid or non-monotonic Node reference");
				if (reference.entity_id <= previousNodeId) {
					throw std::runtime_error(
							"logical graph direct Node count returned an invalid or non-monotonic Node reference");
				}
				previousNodeId = reference.entity_id;
				incrementBounded(state.task.stats.nodes_scanned, "logical-plan direct Node count counter overflowed");
				if (!labelsMatch) {
					return true;
				}
				if (inputRows == state.shared.bounds.maximum_rows) {
					throw std::runtime_error("logical-plan Aggregate exceeded its input row bound");
				}
				incrementBounded(inputRows, "logical-plan Aggregate input counter overflowed");
				return true;
			};
			if (scan.getLabels().empty()) {
				state.shared.snapshot.visitNodeRefs(state.shared.bounds.maximum_node_records,
											 [&visitReference](const storage::EntityRef &reference) {
												 return visitReference(reference, true);
											 });
			} else {
				state.shared.snapshot.visitNodeRefsForLabelPredicate(
						state.shared.bounds.maximum_node_records, scan.getLabels(),
						[&visitReference](const storage::EntityRef &reference, bool labelsMatch) {
							return visitReference(reference, labelsMatch);
						});
			}

			LogicalPlanReadRow result;
			result.values.emplace(count.alias, PropertyValue(static_cast<int64_t>(inputRows)));
			return visitor(std::move(result));
		}

		// An anchored one-hop COUNT(v) can use adjacency topology when the Edge
		// predicate is empty. The source scan and any target predicates still use the
		// ordinary bounded/index-aware runtime path.
		// A nested anchored COUNT is handled as two bounded global Edge passes. This
		// is deliberately expressed in terms of logical operator shape rather than a
		// Cypher string: it removes one B+Tree root descent per intermediate source
		// while preserving path multiplicity and all query resource bounds.
		[[nodiscard]] std::optional<bool> tryVisitBatchedVariableLengthCount(const logical::LogicalAggregate &aggregate,
																			 ExecutionState &state,
																			 const InternalVisitor &visitor) {
			const auto &groupExpressions = aggregate.getGroupByExprs();
			const auto &aggregations = aggregate.getAggregations();
			if (!groupExpressions.empty() || aggregations.size() != 1U ||
				!state.shared.snapshot.supportsNativeBatchAdjacentEdgeRefs() ||
				!state.shared.snapshot.supportsNodeExistenceProbes()) {
				return std::nullopt;
			}
			const logical::LogicalAggItem &count = aggregations.front();
			if (count.functionName != "count" || count.distinct || count.alias.empty() || count.extraArg) {
				return std::nullopt;
			}
			const std::vector<logical::LogicalOperator *> aggregateChildren = aggregate.getChildren();
			if (aggregateChildren.size() != 1U || aggregateChildren.front() == nullptr ||
				aggregateChildren.front()->getType() != logical::LogicalOpType::LOP_VAR_LENGTH_TRAVERSAL) {
				return std::nullopt;
			}
			const auto &traversal =
					dynamic_cast<const logical::LogicalVarLengthTraversal &>(*aggregateChildren.front());
			if (traversal.getSourceVar().empty() || traversal.getTargetVar().empty() ||
				traversal.getSourceVar() == traversal.getTargetVar() || !traversal.getEdgeVar().empty() ||
				traversal.getMinHops() < 0 || traversal.getMaxHops() < traversal.getMinHops() ||
				(traversal.getDirection() != "out" && traversal.getDirection() != "in" &&
				 traversal.getDirection() != "both")) {
				return std::nullopt;
			}
			if (count.argument != nullptr) {
				const auto *variable =
						dynamic_cast<const expressions::VariableReferenceExpression *>(count.argument.get());
				if (variable == nullptr || variable->hasProperty() ||
					variable->getVariableName() != traversal.getTargetVar()) {
					return std::nullopt;
				}
			}
			const std::vector<logical::LogicalOperator *> traversalChildren = traversal.getChildren();
			if (traversalChildren.size() != 1U || traversalChildren.front() == nullptr ||
				traversalChildren.front()->getType() != logical::LogicalOpType::LOP_NODE_SCAN) {
				return std::nullopt;
			}
			const auto &sourceScan = dynamic_cast<const logical::LogicalNodeScan &>(*traversalChildren.front());
			if (sourceScan.getVariable() != traversal.getSourceVar() || !sourceScan.getChildren().empty()) {
				return std::nullopt;
			}

			int maximumHops = traversal.getMaxHops();
			if (state.shared.context != nullptr && state.shared.context->maxVarLengthDepth.has_value()) {
				if (*state.shared.context->maxVarLengthDepth < 0) {
					throw std::invalid_argument("logical-plan variable-length count has a negative query depth bound");
				}
				maximumHops = std::min(maximumHops, *state.shared.context->maxVarLengthDepth);
			}
			if (static_cast<uint64_t>(maximumHops) > state.shared.bounds.maximum_node_records) {
				throw std::runtime_error("logical-plan variable-length count exceeds its configured depth bound");
			}

			std::optional<uint64_t> relationshipTypeToken;
			if (!traversal.getEdgeType().empty()) {
				if (!state.shared.snapshot.supportsNativeRelationshipTypeTokens(traversal.getEdgeType())) {
					return std::nullopt;
				}
				relationshipTypeToken = state.shared.snapshot.findRelationshipTypeToken(traversal.getEdgeType());
			}

			storage::EntityReadShape targetShape;
			targetShape.include_labels = !traversal.getTargetLabels().empty();
			for (const auto &[key, expected]: traversal.getTargetProperties()) {
				static_cast<void>(expected);
				if (key.empty()) {
					throw std::invalid_argument("logical-plan variable-length target predicate has an empty key");
				}
				const std::optional<uint32_t> token = state.shared.snapshot.findPropertyToken(key);
				if (!token.has_value()) {
					// A storage-neutral null result cannot distinguish an absent key from
					// a backend without native Property tokens. Preserve correctness by
					// retaining the ordinary traversal in either case.
					return std::nullopt;
				}
				targetShape.property_tokens.push_back(*token);
			}
			std::ranges::sort(targetShape.property_tokens);
			targetShape.property_tokens.erase(
					std::unique(targetShape.property_tokens.begin(), targetShape.property_tokens.end()),
					targetShape.property_tokens.end());
			validateEntityReadShape(targetShape);

			struct FrontierPath {
				uint64_t current_node_id = 0U;
				std::vector<uint64_t> visited_edge_ids;
			};
			struct AdjacentRecord {
				uint64_t source_node_id = 0U;
				uint64_t target_node_id = 0U;
				uint64_t edge_id = 0U;

				bool operator==(const AdjacentRecord &) const = default;
			};
			const auto pathBytes = [](const FrontierPath &path) {
				return checkedSizeMul(path.visited_edge_ids.capacity(), sizeof(uint64_t),
									  "logical-plan variable-length frontier size overflowed");
			};
			const auto incrementResult = [&](uint64_t &resultCount) {
				if (resultCount == state.shared.bounds.maximum_rows) {
					throw std::runtime_error("logical-plan Aggregate exceeded its input row bound");
				}
				if (resultCount == static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
					throw std::overflow_error("logical-plan variable-length count exceeds integer result range");
				}
				++resultCount;
			};

			debug::ScopedPerfTimer timer("logical_plan.batched_variable_length_count");
			std::vector<FrontierPath> frontier;
			size_t frontierBytes = 0U;
			uint64_t resultCount = 0U;
			static_cast<void>(visitOperator(sourceScan, state, [&](LogicalPlanReadRow row) {
				checkGuard(state);
				const LogicalPlanReadValue &binding = requireBinding(row, traversal.getSourceVar());
				const uint64_t sourceId = nodeBindingId(binding, state);
				if (sourceId == 0U || frontier.size() == state.shared.bounds.maximum_node_records) {
					throw std::runtime_error("logical-plan variable-length count exceeded its source Node bound");
				}
				if (traversal.getMinHops() == 0 && maximumHops >= 0) {
					storage::LogicalGraphNode source;
					if (traversal.getTargetLabels().empty() && traversal.getTargetProperties().empty()) {
						source = patternSourceIdentity(state, binding);
					} else {
						const std::array<uint64_t, 1U> ids{sourceId};
						const auto batch = readNodesShapedSortedBudgeted(state, ids, targetShape);
						if (batch.size() != 1U || batch.front().node_id != sourceId) {
							throw std::runtime_error("logical graph zero-hop target changed cardinality or identity");
						}
						source = batch.front();
						validateNode(source, state);
						if (source.properties.size() > state.shared.bounds.maximum_property_entries ||
							logicalNodeCacheBytes(source, state.shared.context) > state.shared.bounds.maximum_materialized_bytes) {
							throw std::runtime_error("logical-plan zero-hop target exceeded its materialized bound");
						}
					}
					if (nodeMatches(state.shared.snapshot, source, traversal.getTargetLabels(),
									traversal.getTargetProperties(), state)) {
						incrementResult(resultCount);
					}
				}
				FrontierPath path;
				path.current_node_id = sourceId;
				path.visited_edge_ids.reserve(1U);
				// Relationship identity, rather than Node identity, constrains a trail.
				// Retain one bounded slot for the first expansion; no source Node is marked.
				const size_t pathPayloadBytes = pathBytes(path);
				const size_t previousCapacity = frontier.capacity();
				frontier.push_back(std::move(path));
				const size_t addedSlotBytes =
						checkedSizeMul(frontier.capacity() - previousCapacity, sizeof(FrontierPath),
									   "logical-plan variable-length frontier size overflowed");
				frontierBytes = checkedSizeAdd(frontierBytes,
											   checkedSizeAdd(pathPayloadBytes, addedSlotBytes,
															  "logical-plan variable-length frontier size overflowed"),
											   "logical-plan variable-length frontier size overflowed");
				if (frontierBytes > state.shared.bounds.maximum_materialized_bytes) {
					throw std::runtime_error("logical-plan variable-length count exceeded its materialized byte bound");
				}
				return true;
			}));
			std::ranges::sort(frontier, {}, &FrontierPath::current_node_id);
			if (std::ranges::adjacent_find(frontier, {}, &FrontierPath::current_node_id) != frontier.end()) {
				throw std::runtime_error("logical-plan variable-length source scan returned a duplicate Node");
			}

			size_t scannedEdges = 0U;
			for (int depth = 1; depth <= maximumHops && !frontier.empty(); ++depth) {
				checkGuard(state);
				std::vector<uint64_t> currentNodeIds;
				currentNodeIds.reserve(frontier.size());
				for (const FrontierPath &path: frontier) {
					currentNodeIds.push_back(path.current_node_id);
				}
				std::ranges::sort(currentNodeIds);
				currentNodeIds.erase(std::unique(currentNodeIds.begin(), currentNodeIds.end()), currentNodeIds.end());
				if (currentNodeIds.size() > state.shared.bounds.maximum_node_records) {
					throw std::runtime_error("logical-plan variable-length frontier exceeded its Node bound");
				}

				std::vector<size_t> edgeCounts(currentNodeIds.size(), 0U);
				const size_t layerFixedBytes =
						checkedSizeAdd(checkedSizeMul(currentNodeIds.capacity(), sizeof(uint64_t),
													  "logical-plan variable-length layer size overflowed"),
									   checkedSizeMul(edgeCounts.capacity(), sizeof(size_t),
													  "logical-plan variable-length layer size overflowed"),
									   "logical-plan variable-length layer size overflowed");
				const size_t frontierAndLayerBytes = checkedSizeAdd(
						frontierBytes, layerFixedBytes, "logical-plan variable-length layer size overflowed");
				if (frontierAndLayerBytes >= state.shared.bounds.maximum_materialized_bytes) {
					throw std::runtime_error("logical-plan variable-length count exceeded its materialized byte bound");
				}
				const size_t maximumAdjacencyRecords =
						(state.shared.bounds.maximum_materialized_bytes - frontierAndLayerBytes) / sizeof(AdjacentRecord);
				std::vector<AdjacentRecord> adjacency;
				const size_t estimatedAdjacency =
						std::min(state.shared.bounds.maximum_edge_records,
								 checkedSizeMul(currentNodeIds.size(), size_t{4U},
												"logical-plan variable-length adjacency capacity overflowed"));
				adjacency.reserve(std::min(estimatedAdjacency, maximumAdjacencyRecords));
				const auto appendAdjacency = [&](const AdjacentRecord record) {
					if (adjacency.size() == maximumAdjacencyRecords) {
						throw std::runtime_error(
								"logical-plan variable-length count exceeded its materialized byte bound");
					}
					if (adjacency.size() == adjacency.capacity()) {
						const size_t doubled = adjacency.capacity() == 0U
													   ? 1U
													   : (adjacency.capacity() > maximumAdjacencyRecords / 2U
																  ? maximumAdjacencyRecords
																  : adjacency.capacity() * 2U);
						adjacency.reserve(std::min(maximumAdjacencyRecords, doubled));
					}
					adjacency.push_back(record);
				};
				const auto visitDirection = [&](const storage::LogicalGraphReadDirection adjacencyDirection) {
					if (scannedEdges == state.shared.bounds.maximum_edge_records) {
						throw std::runtime_error("logical-plan variable-length count exceeded its Edge bound");
					}
					const size_t remaining = state.shared.bounds.maximum_edge_records - scannedEdges;
					const size_t edgeVisitBound =
							checkedSizeAdd(remaining, 1U, "logical-plan variable-length Edge bound overflowed");
					state.shared.snapshot.visitAdjacentEdgeRefsForNodes(
							currentNodeIds, adjacencyDirection, edgeVisitBound,
							[&](const storage::EntityRef &reference) {
								checkGuard(state);
								validateEdgeRef(
										reference,
										"logical graph batch adjacency returned an invalid or non-Edge reference");
								if (scannedEdges == state.shared.bounds.maximum_edge_records) {
									throw std::runtime_error(
											"logical-plan variable-length count exceeded its Edge bound");
								}
								++scannedEdges;
								incrementBounded(state.task.stats.edges_scanned,
												 "logical-plan variable-length Edge counter overflowed");
								const uint64_t sourceId =
										adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
												? reference.source_node_id
												: reference.target_node_id;
								const uint64_t targetId =
										adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
												? reference.target_node_id
												: reference.source_node_id;
								const auto source = std::ranges::lower_bound(currentNodeIds, sourceId);
								if (source == currentNodeIds.end() || *source != sourceId) {
									throw std::runtime_error(
											"logical graph batch adjacency returned an inconsistent endpoint");
								}
								const bool duplicateSelfLoop =
										traversal.getDirection() == "both" &&
										adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_INCOMING &&
										reference.source_node_id == reference.target_node_id;
								if (duplicateSelfLoop) {
									return true;
								}
								const size_t sourceOrdinal = static_cast<size_t>(source - currentNodeIds.begin());
								if (edgeCounts[sourceOrdinal] == state.shared.bounds.maximum_edges_per_node) {
									throw std::runtime_error(
											"logical-plan variable-length count exceeded its per-Node Edge bound");
								}
								++edgeCounts[sourceOrdinal];
								if (relationshipTypeToken.has_value() &&
									reference.type_token != *relationshipTypeToken) {
									return true;
								}
								appendAdjacency({sourceId, targetId, reference.entity_id});
								return true;
							});
				};
				if (relationshipTypeToken.has_value() || traversal.getEdgeType().empty()) {
					if (traversal.getDirection() == "out" || traversal.getDirection() == "both") {
						visitDirection(storage::LogicalGraphReadDirection::LGRD_OUTGOING);
					}
					if (traversal.getDirection() == "in" || traversal.getDirection() == "both") {
						visitDirection(storage::LogicalGraphReadDirection::LGRD_INCOMING);
					}
				}
				std::ranges::sort(adjacency, [](const AdjacentRecord &left, const AdjacentRecord &right) {
					return std::tie(left.source_node_id, left.edge_id, left.target_node_id) <
						   std::tie(right.source_node_id, right.edge_id, right.target_node_id);
				});
				if (std::ranges::adjacent_find(adjacency) != adjacency.end()) {
					throw std::runtime_error("logical graph batch adjacency returned a duplicate Edge reference");
				}
				if (adjacency.empty()) {
					break;
				}
				const size_t adjacencyBytes = checkedSizeMul(adjacency.capacity(), sizeof(AdjacentRecord),
															 "logical-plan variable-length adjacency size overflowed");
				const size_t layerBytes = checkedSizeAdd(layerFixedBytes, adjacencyBytes,
														 "logical-plan variable-length layer size overflowed");
				const size_t retainedLayerBytes =
						checkedSizeAdd(frontierBytes, layerBytes, "logical-plan variable-length layer size overflowed");
				const size_t targetVectorBytes =
						checkedSizeMul(adjacency.size(), sizeof(uint64_t),
									   "logical-plan variable-length target vector size overflowed");
				if (targetVectorBytes > state.shared.bounds.maximum_materialized_bytes -
												std::min(retainedLayerBytes, state.shared.bounds.maximum_materialized_bytes)) {
					throw std::runtime_error("logical-plan variable-length count exceeded its materialized byte bound");
				}

				std::vector<uint64_t> targetNodeIds;
				targetNodeIds.reserve(adjacency.size());
				for (const AdjacentRecord &record: adjacency) {
					targetNodeIds.push_back(record.target_node_id);
				}
				std::ranges::sort(targetNodeIds);
				targetNodeIds.erase(std::unique(targetNodeIds.begin(), targetNodeIds.end()), targetNodeIds.end());
				if (targetNodeIds.size() > state.shared.bounds.maximum_node_records) {
					throw std::runtime_error("logical-plan variable-length endpoints exceeded the Node bound");
				}
				std::vector<uint8_t> targetMatches(targetNodeIds.size(), uint8_t{0U});
				if (traversal.getTargetLabels().empty() && traversal.getTargetProperties().empty()) {
					const std::vector<bool> present = state.shared.snapshot.containsNodesSorted(targetNodeIds);
					if (present.size() != targetNodeIds.size() ||
						std::ranges::any_of(present, [](const bool value) { return !value; })) {
						throw std::runtime_error(
								"logical graph batch adjacency returned an Edge with a missing endpoint");
					}
					std::fill(targetMatches.begin(), targetMatches.end(), uint8_t{1U});
				} else {
					const std::vector<storage::LogicalGraphNode> targets =
							readNodesShapedSortedBudgeted(state, targetNodeIds, targetShape);
					if (targets.size() != targetNodeIds.size()) {
						throw std::runtime_error("logical graph backend changed variable-length target cardinality");
					}
					size_t targetBytes = 0U;
					for (size_t index = 0U; index < targets.size(); ++index) {
						if (targets[index].node_id != targetNodeIds[index]) {
							throw std::runtime_error("logical graph backend changed variable-length target identity");
						}
						targetBytes = checkedSizeAdd(targetBytes, logicalNodeCacheBytes(targets[index], state.shared.context),
													 "logical-plan variable-length target batch size overflowed");
						const size_t stagedBytes = checkedSizeAdd(
								retainedLayerBytes,
								checkedSizeAdd(targetVectorBytes, targetBytes,
											   "logical-plan variable-length target batch size overflowed"),
								"logical-plan variable-length target batch size overflowed");
						if (stagedBytes > state.shared.bounds.maximum_materialized_bytes) {
							throw std::runtime_error(
									"logical-plan variable-length count exceeded its materialized byte bound");
						}
						targetMatches[index] = nodeMatches(state.shared.snapshot, targets[index], traversal.getTargetLabels(),
														   traversal.getTargetProperties(), state);
					}
				}

				std::vector<FrontierPath> nextFrontier;
				size_t nextFrontierBytes = 0U;
				const size_t retainedBytes =
						checkedSizeAdd(retainedLayerBytes,
									   checkedSizeAdd(targetVectorBytes, targetMatches.capacity() * sizeof(uint8_t),
													  "logical-plan variable-length layer size overflowed"),
									   "logical-plan variable-length layer size overflowed");
				for (const FrontierPath &path: frontier) {
					checkGuard(state);
					const auto first = std::ranges::lower_bound(adjacency, path.current_node_id, {},
																&AdjacentRecord::source_node_id);
					const auto last = std::ranges::upper_bound(adjacency, path.current_node_id, {},
															   &AdjacentRecord::source_node_id);
					for (auto candidate = first; candidate != last; ++candidate) {
						if (std::ranges::find(path.visited_edge_ids, candidate->edge_id) !=
							path.visited_edge_ids.end()) {
							continue;
						}
						const auto target = std::ranges::lower_bound(targetNodeIds, candidate->target_node_id);
						if (target == targetNodeIds.end() || *target != candidate->target_node_id) {
							throw std::runtime_error("logical-plan variable-length target lookup lost an endpoint");
						}
						if (depth >= traversal.getMinHops() &&
							targetMatches[static_cast<size_t>(target - targetNodeIds.begin())]) {
							incrementResult(resultCount);
						}
						if (depth == maximumHops) {
							continue;
						}
						FrontierPath next;
						next.current_node_id = candidate->target_node_id;
						next.visited_edge_ids.reserve(path.visited_edge_ids.size() + 1U);
						next.visited_edge_ids.insert(next.visited_edge_ids.end(), path.visited_edge_ids.begin(),
													 path.visited_edge_ids.end());
						next.visited_edge_ids.push_back(candidate->edge_id);
						const size_t nextPathBytes = pathBytes(next);
						const size_t previousCapacity = nextFrontier.capacity();
						nextFrontier.push_back(std::move(next));
						const size_t addedSlotBytes =
								checkedSizeMul(nextFrontier.capacity() - previousCapacity, sizeof(FrontierPath),
											   "logical-plan variable-length frontier size overflowed");
						nextFrontierBytes =
								checkedSizeAdd(nextFrontierBytes,
											   checkedSizeAdd(nextPathBytes, addedSlotBytes,
															  "logical-plan variable-length frontier size overflowed"),
											   "logical-plan variable-length frontier size overflowed");
						if (checkedSizeAdd(retainedBytes, nextFrontierBytes,
										   "logical-plan variable-length frontier size overflowed") >
							state.shared.bounds.maximum_materialized_bytes) {
							throw std::runtime_error(
									"logical-plan variable-length count exceeded its materialized byte bound");
						}
					}
				}
				frontier = std::move(nextFrontier);
				frontierBytes = nextFrontierBytes;
			}

			LogicalPlanReadRow result;
			result.values.emplace(count.alias, PropertyValue(static_cast<int64_t>(resultCount)));
			return visitor(std::move(result));
		}

		[[nodiscard]] std::optional<bool>
		tryVisitAnchoredTwoHopRelationshipCount(const logical::LogicalAggregate &aggregate, ExecutionState &state,
												const InternalVisitor &visitor) {
			const auto &groupExpressions = aggregate.getGroupByExprs();
			const auto &aggregations = aggregate.getAggregations();
			if (!groupExpressions.empty() || aggregations.size() != 1U ||
				!state.shared.snapshot.supportsNativeEdgeRefScans() || !state.shared.snapshot.supportsNodeExistenceProbes()) {
				return std::nullopt;
			}
			const logical::LogicalAggItem &count = aggregations.front();
			if (count.functionName != "count" || count.distinct || count.alias.empty() || count.extraArg ||
				!count.argument) {
				return std::nullopt;
			}
			const auto *countVariable =
					dynamic_cast<const expressions::VariableReferenceExpression *>(count.argument.get());
			if (countVariable == nullptr || countVariable->hasProperty()) {
				return std::nullopt;
			}
			const std::vector<logical::LogicalOperator *> aggregateChildren = aggregate.getChildren();
			if (debug::PerfTrace::areValuesEnabled() && !aggregateChildren.empty() &&
				aggregateChildren.front() != nullptr) {
				debug::PerfTrace::addValue("logical_plan.two_hop.aggregate_child_type",
										   static_cast<int64_t>(aggregateChildren.front()->getType()));
			}
			if (aggregateChildren.size() != 1U || aggregateChildren.front() == nullptr ||
				aggregateChildren.front()->getType() != logical::LogicalOpType::LOP_TRAVERSAL) {
				return std::nullopt;
			}
			const auto &outer = dynamic_cast<const logical::LogicalTraversal &>(*aggregateChildren.front());
			if (outer.getTargetVar().empty() || countVariable->getVariableName() != outer.getTargetVar() ||
				!outer.getEdgeProperties().empty() || !outer.getTargetProperties().empty() ||
				(outer.getDirection() != "out" && outer.getDirection() != "in" && outer.getDirection() != "both")) {
				return std::nullopt;
			}
			const std::vector<logical::LogicalOperator *> outerChildren = outer.getChildren();
			if (outerChildren.size() != 1U || outerChildren.front() == nullptr ||
				outerChildren.front()->getType() != logical::LogicalOpType::LOP_TRAVERSAL) {
				return std::nullopt;
			}
			const auto &inner = dynamic_cast<const logical::LogicalTraversal &>(*outerChildren.front());
			if (inner.getTargetVar().empty() || inner.getTargetVar() != outer.getSourceVar() ||
				!inner.getEdgeProperties().empty() || !inner.getTargetProperties().empty() ||
				!inner.getTargetProperties().empty() ||
				(inner.getDirection() != "out" && inner.getDirection() != "in" && inner.getDirection() != "both")) {
				return std::nullopt;
			}
			const std::vector<logical::LogicalOperator *> innerChildren = inner.getChildren();
			if (innerChildren.size() != 1U || innerChildren.front() == nullptr ||
				innerChildren.front()->getType() != logical::LogicalOpType::LOP_NODE_SCAN) {
				return std::nullopt;
			}
			const auto &seed = dynamic_cast<const logical::LogicalNodeScan &>(*innerChildren.front());
			if (seed.getVariable() != inner.getSourceVar()) {
				return std::nullopt;
			}
			if (state.shared.bounds.maximum_rows == std::numeric_limits<size_t>::max() ||
				state.shared.bounds.maximum_edge_records == std::numeric_limits<size_t>::max()) {
				throw std::overflow_error("logical-plan two-hop relationship count bound overflowed");
			}
			std::optional<uint64_t> innerType;
			if (!inner.getEdgeType().empty()) {
				if (!state.shared.snapshot.supportsNativeRelationshipTypeTokens(inner.getEdgeType())) {
					return std::nullopt;
				}
				innerType = state.shared.snapshot.findRelationshipTypeToken(inner.getEdgeType());
				if (!innerType.has_value()) {
					LogicalPlanReadRow result;
					result.values.emplace(count.alias, PropertyValue(int64_t{0}));
					return visitor(std::move(result));
				}
			}
			std::optional<uint64_t> outerType;
			if (!outer.getEdgeType().empty()) {
				if (!state.shared.snapshot.supportsNativeRelationshipTypeTokens(outer.getEdgeType())) {
					return std::nullopt;
				}
				outerType = state.shared.snapshot.findRelationshipTypeToken(outer.getEdgeType());
				if (!outerType.has_value()) {
					LogicalPlanReadRow result;
					result.values.emplace(count.alias, PropertyValue(int64_t{0}));
					return visitor(std::move(result));
				}
			}

			detail::BudgetMemoryResource intermediateMemory(state.shared.materialized_budget);
			const auto bitBytes = [](size_t capacity) { return capacity / 8U + (capacity % 8U != 0U); };
			const auto visitBatchRefs = [&](std::span<const uint64_t> sources,
				storage::LogicalGraphReadDirection direction, size_t bound,
				const std::function<bool(const storage::EntityRef &)> &consume) {
				if (state.shared.snapshot.supportsBatchAdjacencyReferenceBufferAdmission()) {
					MaterializedByteReservation sourceBytes(&state.shared.materialized_budget);
					state.shared.snapshot.visitAdjacentEdgeRefsForNodesWithBufferAdmission(sources, direction, bound, consume,
						[&](size_t bytes) {
							if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
							if (sourceBytes.wouldExceed(bytes)) throw storage::LogicalGraphBufferPressure();
							sourceBytes.set(bytes);
						});
				} else {
					state.shared.snapshot.visitAdjacentEdgeRefsForNodes(sources, direction, bound, consume);
				}
			};

			std::pmr::vector<uint64_t> seedIds(&intermediateMemory);
			static_cast<void>(visitOperator(seed, state, [&](LogicalPlanReadRow row) {
				const uint64_t id = nodeBindingId(requireBinding(row, seed.getVariable()), state);
				if (id == 0U || seedIds.size() == state.shared.bounds.maximum_node_records) {
					throw std::runtime_error("logical-plan two-hop relationship count exceeded its source Node bound");
				}
				seedIds.push_back(id);
				return true;
			}));
			detail::sortUniqueIdsGuarded(seedIds, [&] {
				if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
			});

			using Multiplicity = uint64_t;
			const auto expand = [&](const std::pmr::vector<uint64_t> &sources, const std::string &direction,
									const std::optional<uint64_t> &type,
									std::pmr::unordered_map<uint64_t, Multiplicity> *next) {
				std::pmr::unordered_set<uint64_t> sourceSet(&intermediateMemory);
				sourceSet.reserve(sources.size());
				for (const uint64_t id: sources) {
					checkGuard(state);
					sourceSet.insert(id);
				}
				std::pmr::unordered_map<uint64_t, size_t> sourceEdgeCounts(&intermediateMemory);
				sourceEdgeCounts.reserve(sources.size());
				std::pmr::vector<uint64_t> targetIds(&intermediateMemory);
				size_t scanned = 0U;
				const auto consume = [&](const storage::EntityRef &reference) {
					checkGuard(state);
					validateEdgeRef(reference,
									"logical-plan two-hop relationship count received an invalid or Node reference");
					if (scanned == state.shared.bounds.maximum_edge_records) {
						throw std::runtime_error("logical-plan two-hop relationship count exceeded its Edge bound");
					}
					++scanned;
					incrementBounded(state.task.stats.edges_scanned, "logical-plan two-hop Edge counter overflowed");
					const bool outgoing = sourceSet.contains(reference.source_node_id);
					const bool incoming = sourceSet.contains(reference.target_node_id);
					const auto consumeOne = [&](const uint64_t sourceId, const uint64_t targetId) {
						if (sourceEdgeCounts[sourceId] == state.shared.bounds.maximum_edges_per_node) {
							throw std::runtime_error(
									"logical-plan two-hop relationship count exceeded its per-Node Edge bound");
						}
						++sourceEdgeCounts[sourceId];
						if (type.has_value() && reference.type_token != *type) {
							return;
						}
						if (next != nullptr) {
							const Multiplicity previous = next->contains(targetId) ? next->at(targetId) : 0U;
							const Multiplicity increment = sources.empty() ? 0U : 1U;
							if (previous > std::numeric_limits<Multiplicity>::max() - increment) {
								throw std::overflow_error("logical-plan two-hop path multiplicity overflowed");
							}
							next->insert_or_assign(targetId, previous + increment);
							// Probe each distinct endpoint once; multiplicity stays in the map.
							if (previous == 0U) targetIds.push_back(targetId);
						}
					};
					if (direction == "out" || direction == "both") {
						if (outgoing)
							consumeOne(reference.source_node_id, reference.target_node_id);
					}
					if ((direction == "in" || direction == "both") && incoming &&
						!(outgoing && reference.source_node_id == reference.target_node_id)) {
						consumeOne(reference.target_node_id, reference.source_node_id);
					}
					return true;
				};
				const auto visitDirection = [&](const storage::LogicalGraphReadDirection adjacencyDirection) {
					if (scanned == state.shared.bounds.maximum_edge_records) {
						throw std::runtime_error("logical-plan two-hop relationship count exceeded its Edge bound");
					}
					const size_t remaining = state.shared.bounds.maximum_edge_records - scanned;
					const size_t edgeVisitBound =
							checkedSizeAdd(remaining, 1U, "logical-plan two-hop Edge bound overflowed");
					visitBatchRefs(sources, adjacencyDirection, edgeVisitBound, consume);
				};
				if (direction == "out" || direction == "both") {
					visitDirection(storage::LogicalGraphReadDirection::LGRD_OUTGOING);
				}
				if (direction == "in" || direction == "both") {
					visitDirection(storage::LogicalGraphReadDirection::LGRD_INCOMING);
				}
				if (next != nullptr) {
					if (next->size() > state.shared.bounds.maximum_node_records) {
						throw std::runtime_error(
								"logical-plan two-hop relationship count exceeded its intermediate Node bound");
					}
					detail::sortUniqueIdsGuarded(targetIds, [&] {
						if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
					});
					MaterializedByteReservation presentBytes(&state.shared.materialized_budget);
					presentBytes.set(checkedSizeAdd(bitBytes(targetIds.size()), sizeof(size_t),
						"logical-plan two-hop bit slots overflowed"));
					const std::vector<bool> present = state.shared.snapshot.containsNodesSorted(targetIds);
					presentBytes.set(bitBytes(present.capacity()));
					checkGuard(state);
					if (present.size() != targetIds.size() ||
						std::ranges::any_of(present, [](bool value) { return !value; })) {
						throw std::runtime_error(
								"logical graph backend returned an Edge topology with a missing endpoint");
					}
					const std::vector<std::string> &labels = inner.getTargetLabels();
					if (!labels.empty()) {
						MaterializedByteReservation matchesBytes(&state.shared.materialized_budget);
						matchesBytes.set(checkedSizeAdd(bitBytes(targetIds.size()), sizeof(size_t),
							"logical-plan two-hop bit slots overflowed"));
						const std::vector<bool> matches = state.shared.snapshot.readNodeLabelMatchesSorted(targetIds, labels);
						matchesBytes.set(bitBytes(matches.capacity()));
						checkGuard(state);
						if (matches.size() != targetIds.size()) {
							throw std::runtime_error(
									"logical graph backend changed two-hop intermediate label cardinality");
						}
						for (size_t index = 0U; index < targetIds.size(); ++index) {
							checkGuard(state);
							if (!matches[index]) {
								next->erase(targetIds[index]);
							}
						}
					}
				}
			};
			std::pmr::unordered_map<uint64_t, Multiplicity> intermediate(&intermediateMemory);
			if (!seedIds.empty()) {
				expand(seedIds, inner.getDirection(), innerType, &intermediate);
			}
			std::pmr::vector<uint64_t> intermediateIds(&intermediateMemory);
			intermediateIds.reserve(intermediate.size());
			for (const auto &[id, multiplicity]: intermediate) {
				checkGuard(state);
				static_cast<void>(multiplicity);
				intermediateIds.push_back(id);
			}
			detail::sortUniqueIdsGuarded(intermediateIds, [&] {
				if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
			});
			uint64_t resultCount = 0U;
			// The second pass retains multiplicity from the first hop.  It uses the same
			// endpoint/type checks but does not materialize intermediate rows.
			std::pmr::unordered_set<uint64_t> sourceSet(&intermediateMemory);
			sourceSet.reserve(intermediate.size());
			for (const uint64_t id: intermediateIds) {
				checkGuard(state);
				sourceSet.insert(id);
			}
			std::pmr::unordered_map<uint64_t, size_t> edgeCounts(&intermediateMemory);
			std::pmr::unordered_map<uint64_t, Multiplicity> finalCandidates(&intermediateMemory);
			std::pmr::vector<uint64_t> finalTargetIds(&intermediateMemory);
			size_t scanned = 0U;
			const auto consumeOuterReference = [&](const storage::EntityRef &reference) {
				checkGuard(state);
				validateEdgeRef(reference,
								"logical-plan two-hop relationship count received an invalid or Node reference");
				if (scanned == state.shared.bounds.maximum_edge_records) {
					throw std::runtime_error("logical-plan two-hop relationship count exceeded its Edge bound");
				}
				++scanned;
				incrementBounded(state.task.stats.edges_scanned, "logical-plan two-hop Edge counter overflowed");
				const bool outgoing = sourceSet.contains(reference.source_node_id);
				const bool incoming = sourceSet.contains(reference.target_node_id);
				const auto consume = [&](uint64_t sourceId, uint64_t targetId) {
					if (edgeCounts[sourceId] == state.shared.bounds.maximum_edges_per_node) {
						throw std::runtime_error(
								"logical-plan two-hop relationship count exceeded its per-Node Edge bound");
					}
					++edgeCounts[sourceId];
					if ((outerType.has_value() && reference.type_token != *outerType))
						return;
					const uint64_t multiplicity = intermediate.at(sourceId);
					const auto [entry, inserted] = finalCandidates.try_emplace(targetId, 0U);
					if (entry->second > std::numeric_limits<uint64_t>::max() - multiplicity) {
						throw std::overflow_error("logical-plan two-hop relationship count overflowed");
					}
					entry->second += multiplicity;
					if (inserted) finalTargetIds.push_back(targetId);
				};
				if (outer.getDirection() == "out" || outer.getDirection() == "both") {
					if (outgoing)
						consume(reference.source_node_id, reference.target_node_id);
				}
				if ((outer.getDirection() == "in" || outer.getDirection() == "both") && incoming &&
					!(outgoing && reference.source_node_id == reference.target_node_id)) {
					consume(reference.target_node_id, reference.source_node_id);
				}
				return true;
			};
			const auto visitOuterDirection = [&](const storage::LogicalGraphReadDirection adjacencyDirection) {
				if (scanned == state.shared.bounds.maximum_edge_records) {
					throw std::runtime_error("logical-plan two-hop relationship count exceeded its Edge bound");
				}
				const size_t remaining = state.shared.bounds.maximum_edge_records - scanned;
				const size_t edgeVisitBound =
						checkedSizeAdd(remaining, 1U, "logical-plan two-hop Edge bound overflowed");
				visitBatchRefs(intermediateIds, adjacencyDirection, edgeVisitBound,
															 consumeOuterReference);
			};
			if (outer.getDirection() == "out" || outer.getDirection() == "both") {
				visitOuterDirection(storage::LogicalGraphReadDirection::LGRD_OUTGOING);
			}
			if (outer.getDirection() == "in" || outer.getDirection() == "both") {
				visitOuterDirection(storage::LogicalGraphReadDirection::LGRD_INCOMING);
			}
			detail::sortUniqueIdsGuarded(finalTargetIds, [&] {
				if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
			});
			MaterializedByteReservation finalPresentBytes(&state.shared.materialized_budget);
			finalPresentBytes.set(checkedSizeAdd(bitBytes(finalTargetIds.size()), sizeof(size_t),
				"logical-plan two-hop bit slots overflowed"));
			const std::vector<bool> finalPresent = state.shared.snapshot.containsNodesSorted(finalTargetIds);
			finalPresentBytes.set(bitBytes(finalPresent.capacity()));
			checkGuard(state);
			if (finalPresent.size() != finalTargetIds.size() ||
				std::ranges::any_of(finalPresent, [](bool value) { return !value; })) {
				throw std::runtime_error("logical graph backend returned an Edge topology with a missing endpoint");
			}
			MaterializedByteReservation finalLabelBytes(&state.shared.materialized_budget);
			std::vector<bool> finalLabels;
			if (!outer.getTargetLabels().empty()) {
				finalLabelBytes.set(checkedSizeAdd(bitBytes(finalTargetIds.size()), sizeof(size_t),
					"logical-plan two-hop label bit slots overflowed"));
				finalLabels = state.shared.snapshot.readNodeLabelMatchesSorted(finalTargetIds, outer.getTargetLabels());
				finalLabelBytes.set(bitBytes(finalLabels.capacity()));
				checkGuard(state);
				if (finalLabels.size() != finalTargetIds.size()) {
					throw std::runtime_error("logical graph backend changed two-hop target label cardinality");
				}
			}
			for (const auto &[targetId, multiplicity]: finalCandidates) {
				checkGuard(state);
				const auto position = std::ranges::lower_bound(finalTargetIds, targetId);
				if (position == finalTargetIds.end() ||
					(!finalLabels.empty() && !finalLabels[static_cast<size_t>(position - finalTargetIds.begin())])) {
					continue;
				}
				if (resultCount > std::numeric_limits<uint64_t>::max() - multiplicity) {
					throw std::overflow_error("logical-plan two-hop relationship count overflowed");
				}
				resultCount += multiplicity;
				if (resultCount > state.shared.bounds.maximum_rows) {
					throw std::runtime_error("logical-plan Aggregate exceeded its input row bound");
				}
			}
			LogicalPlanReadRow result;
			if (resultCount > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
				throw std::overflow_error("logical-plan two-hop relationship count exceeds integer result range");
			}
			result.values.emplace(count.alias, PropertyValue(static_cast<int64_t>(resultCount)));
			return visitor(std::move(result));
		}

		[[nodiscard]] std::optional<bool> tryVisitAnchoredRelationshipCount(const logical::LogicalAggregate &aggregate,
																			ExecutionState &state,
																			const InternalVisitor &visitor) {
			const auto &groupExpressions = aggregate.getGroupByExprs();
			const auto &aggregations = aggregate.getAggregations();
			if (!groupExpressions.empty() || aggregations.size() != 1U) {
				return std::nullopt;
			}
			const logical::LogicalAggItem &count = aggregations.front();
			if (count.functionName != "count" || count.distinct || count.alias.empty() || count.extraArg ||
				!count.argument) {
				return std::nullopt;
			}
			const auto *countVariable =
					dynamic_cast<const expressions::VariableReferenceExpression *>(count.argument.get());
			if (countVariable == nullptr || countVariable->hasProperty()) {
				return std::nullopt;
			}

			const std::vector<logical::LogicalOperator *> aggregateChildren = aggregate.getChildren();
			if (aggregateChildren.size() != 1U || aggregateChildren.front() == nullptr ||
				aggregateChildren.front()->getType() != logical::LogicalOpType::LOP_TRAVERSAL) {
				return std::nullopt;
			}
			const auto &traversal = dynamic_cast<const logical::LogicalTraversal &>(*aggregateChildren.front());
			if (traversal.getEdgeVar().empty() || traversal.getTargetVar().empty() ||
				countVariable->getVariableName() != traversal.getTargetVar() ||
				!traversal.getEdgeProperties().empty()) {
				return std::nullopt;
			}
			if (traversal.getDirection() != "out" && traversal.getDirection() != "in" &&
				traversal.getDirection() != "both") {
				return std::nullopt;
			}

			const std::vector<logical::LogicalOperator *> traversalChildren = traversal.getChildren();
			if (traversalChildren.size() != 1U || traversalChildren.front() == nullptr ||
				traversalChildren.front()->getType() != logical::LogicalOpType::LOP_NODE_SCAN) {
				return std::nullopt;
			}
			const auto &seed = dynamic_cast<const logical::LogicalNodeScan &>(*traversalChildren.front());
			if (seed.getVariable() != traversal.getSourceVar()) {
				return std::nullopt;
			}
			std::optional<uint64_t> relationshipTypeToken;
			if (!traversal.getEdgeType().empty() &&
				state.shared.snapshot.supportsNativeRelationshipTypeTokens(traversal.getEdgeType())) {
				relationshipTypeToken = state.shared.snapshot.findRelationshipTypeToken(traversal.getEdgeType());
				if (!relationshipTypeToken.has_value()) {
					LogicalPlanReadRow result;
					result.values.emplace(count.alias, PropertyValue(int64_t{0}));
					return visitor(std::move(result));
				}
			}
			const bool useNativeAdjacencyRefs = state.shared.snapshot.supportsNativeAdjacentEdgeRefScans() &&
												(traversal.getEdgeType().empty() || relationshipTypeToken.has_value());
			// Nullable columns need a presence probe to distinguish stored NULL
			// from an absent Property without loading unrelated values.
			const bool targetPropertiesCanBeSelected =
					useNativeAdjacencyRefs && !traversal.getTargetProperties().empty() &&
					(state.shared.snapshot.supportsPropertyPresenceProbes() ||
					 !std::ranges::any_of(traversal.getTargetProperties(),
						[](const auto &predicate) { return isNull(predicate.second); }));

			debug::ScopedPerfTimer timer("logical_plan.anchored_relationship_count");
			size_t inputRows = 0U;
			const bool supportsExistenceProbes = state.shared.snapshot.supportsNodeExistenceProbes();
			const auto visitSource = [&](LogicalPlanReadRow row) {
				const LogicalPlanReadValue &sourceBinding = requireBinding(row, traversal.getSourceVar());
				// The topology cursor needs only the stable logical ID. Keep a native
				// deferred source as a reference instead of reopening its full Node record;
				// source labels and predicates were already validated by the scan.
				const uint64_t sourceNodeId = nodeBindingId(sourceBinding, state);
				size_t sourceEdges = 0U;
				struct PendingTarget {
					uint64_t edge_id = 0U;
					uint64_t source_node_id = 0U;
					uint64_t target_node_id = 0U;
					bool type_matches = false;
				};
				MaterializedByteReservation pendingBytes(&state.shared.materialized_budget);
				std::vector<PendingTarget> pendingTargets;
				using LabelMatch = std::pair<uint64_t, bool>;
				MaterializedByteReservation labelCacheBytes(&state.shared.materialized_budget);
				std::vector<LabelMatch> targetLabelCache;
				const auto cachedLabel = [&](uint64_t id) { return std::ranges::find(targetLabelCache, id, &LabelMatch::first); };
				// Target Property values are stable for the pinned snapshot.  Keep a
				// bounded per-source cache so a super-node whose adjacency spans many
				// pages does not reread the same target on every page flush.
				using PropertyCacheEntry = std::pair<uint64_t, std::vector<PropertyValue>>;
				MaterializedByteReservation propertyCacheSlots(&state.shared.materialized_budget);
				MaterializedByteReservation targetPropertyCacheReservation(&state.shared.materialized_budget);
				std::vector<PropertyCacheEntry> targetPropertyCache;
				const auto cachedProperties = [&](uint64_t id) {
					return std::ranges::find(targetPropertyCache, id, &PropertyCacheEntry::first);
				};
				size_t targetPropertyCacheBytes = 0U;
				const size_t targetBatchCapacity = std::max<size_t>(
						1U, std::min(state.shared.bounds.maximum_adjacency_page_records, state.shared.bounds.maximum_edges_per_node));
				const auto visitDirection = [&](storage::LogicalGraphReadDirection direction) {
					uint64_t previousEdgeId = 0U;
					const auto flushTargets = [&]() {
						if (pendingTargets.empty()) {
							return true;
						}
						// Both native references and ordinary topology pages can use the
						// same bounded target validation batch.  The latter used to fall
						// through to one existence/label read per Edge even though COUNT
						// has no visitor early-termination boundary.
						MaterializedByteReservation targetIdsBytes(&state.shared.materialized_budget);
						std::vector<uint64_t> targetIds;
						if (useNativeAdjacencyRefs ||
							(traversal.getTargetProperties().empty() &&
							 (!traversal.getTargetLabels().empty() || supportsExistenceProbes))) {
							targetIdsBytes.set(checkedSizeMul(pendingTargets.size(), sizeof(uint64_t),
								"logical-plan anchored endpoint slots overflowed"));
							targetIds.reserve(pendingTargets.size());
							targetIdsBytes.set(checkedSizeMul(targetIds.capacity(), sizeof(uint64_t),
								"logical-plan anchored endpoint capacity overflowed"));
							for (const PendingTarget &candidate: pendingTargets) {
								targetIds.push_back(candidate.target_node_id);
							}
							detail::sortUniqueIdsGuarded(targetIds, [&] {
								if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
							});
						}
						const auto bitBytes = [](size_t capacity) { return capacity / 8U + (capacity % 8U != 0U); };
						MaterializedByteReservation labelBitsBytes(&state.shared.materialized_budget);
						std::vector<bool> labelsMatch;
						if (!traversal.getTargetLabels().empty()) {
							labelBitsBytes.set(checkedSizeAdd(bitBytes(targetIds.size()), sizeof(size_t),
								"logical-plan anchored label bit slots overflowed"));
							labelsMatch.resize(targetIds.size());
							labelBitsBytes.set(bitBytes(labelsMatch.capacity()));
							MaterializedByteReservation uncachedIdsBytes(&state.shared.materialized_budget);
							uncachedIdsBytes.set(checkedSizeMul(targetIds.size(), sizeof(uint64_t),
								"logical-plan anchored cold label slots overflowed"));
							std::vector<uint64_t> uncachedIds;
							uncachedIds.reserve(targetIds.size());
							uncachedIdsBytes.set(checkedSizeMul(uncachedIds.capacity(), sizeof(uint64_t),
								"logical-plan anchored cold label capacity overflowed"));
							for (size_t index = 0U; index < targetIds.size(); ++index) {
								const auto cached = cachedLabel(targetIds[index]);
								if (cached == targetLabelCache.end()) uncachedIds.push_back(targetIds[index]);
								else labelsMatch[index] = cached->second;
							}
							if (!uncachedIds.empty()) {
								MaterializedByteReservation resolvedBitsBytes(&state.shared.materialized_budget);
								resolvedBitsBytes.set(checkedSizeAdd(bitBytes(uncachedIds.size()), sizeof(size_t),
									"logical-plan anchored resolved bit slots overflowed"));
								const std::vector<bool> resolved = state.shared.snapshot.readNodeLabelMatchesSorted(
										uncachedIds, traversal.getTargetLabels());
								resolvedBitsBytes.set(bitBytes(resolved.capacity()));
								checkGuard(state);
								if (resolved.size() != uncachedIds.size()) {
									throw std::runtime_error(
											"logical graph backend changed anchored target label-match cardinality");
								}
								const size_t cacheLimit = std::min(state.shared.bounds.maximum_edges_per_node, size_t{64U});
								for (size_t index = 0U; index < uncachedIds.size(); ++index) {
									const auto position = std::ranges::lower_bound(targetIds, uncachedIds[index]);
									labelsMatch[static_cast<size_t>(position - targetIds.begin())] = resolved[index];
									if (targetLabelCache.size() == cacheLimit) continue;
									if (targetLabelCache.size() == targetLabelCache.capacity()) {
										const size_t old = targetLabelCache.capacity();
										const size_t next = old == 0U ? std::min(cacheLimit, size_t{8U}) : old + std::min(old, cacheLimit - old);
										const size_t peak = (old + next) * sizeof(LabelMatch);
										if (labelCacheBytes.wouldExceed(peak)) continue;
										labelCacheBytes.set(peak);
										targetLabelCache.reserve(next);
										labelCacheBytes.set(targetLabelCache.capacity() * sizeof(LabelMatch));
										debug::PerfTrace::addValue("logical_plan.anchored_label_cache_capacity", static_cast<int64_t>(targetLabelCache.capacity()));
									}
									targetLabelCache.emplace_back(uncachedIds[index], resolved[index]);
								}
							}
						}
						MaterializedByteReservation keySlotsBytes(&state.shared.materialized_budget);
						MaterializedByteReservation coldIdsBytes(&state.shared.materialized_budget);
						MaterializedByteReservation targetPropertyReadBytes(&state.shared.materialized_budget);
						std::vector<std::string_view> targetPropertyKeys;
						std::vector<std::vector<PropertyValue>> resolvedPropertyColumns;
						std::vector<uint64_t> uncachedPropertyIds;
						size_t targetPropertyStructureBytes = 0U;
						if (targetPropertiesCanBeSelected) {
							// Keep the current flush complete even when the cross-page cache
							// reaches its byte budget. Oversized values are used once and are
							// deliberately not retained for later adjacency pages.
							for (const auto &predicate: traversal.getTargetProperties())
								if (predicate.first.empty()) throw std::invalid_argument("logical-plan target Node Property predicate has an empty key");
							keySlotsBytes.set(checkedSizeMul(traversal.getTargetProperties().size(), sizeof(std::string_view),
								"logical-plan anchored key view slots overflowed"));
							targetPropertyKeys.reserve(traversal.getTargetProperties().size());
							keySlotsBytes.set(checkedSizeMul(targetPropertyKeys.capacity(), sizeof(std::string_view),
								"logical-plan anchored key view capacity overflowed"));
							for (const auto &[key, expected]: traversal.getTargetProperties()) {
								static_cast<void>(expected);
								if (key.empty()) {
									throw std::invalid_argument(
											"logical-plan target Node Property predicate has an empty key");
								}
								if (std::ranges::find(targetPropertyKeys, key) == targetPropertyKeys.end()) {
									targetPropertyKeys.push_back(key);
								}
							}
							const auto &keyViews = targetPropertyKeys; // Borrow stable plan-owned names.
							const size_t coldCount = static_cast<size_t>(std::ranges::count_if(targetIds,
								[&](uint64_t id) { return cachedProperties(id) == targetPropertyCache.end(); }));
							coldIdsBytes.set(checkedSizeMul(coldCount, sizeof(uint64_t), "logical-plan anchored cold owner slots overflowed"));
							uncachedPropertyIds.reserve(coldCount);
							coldIdsBytes.set(checkedSizeMul(uncachedPropertyIds.capacity(), sizeof(uint64_t),
								"logical-plan anchored cold owner capacity overflowed"));
							for (const uint64_t targetId: targetIds) {
								if (cachedProperties(targetId) == targetPropertyCache.end()) {
									uncachedPropertyIds.push_back(targetId);
								}
							}
							if (!uncachedPropertyIds.empty()) {
								targetPropertyStructureBytes = checkedSizeAdd(
										checkedSizeMul(
												keyViews.size(), sizeof(std::vector<PropertyValue>),
												"logical-plan anchored target Property column structure overflowed"),
										checkedSizeMul(
												checkedSizeMul(keyViews.size(), uncachedPropertyIds.size(),
															   "logical-plan anchored target Property column structure "
															   "overflowed"),
												sizeof(PropertyValue),
												"logical-plan anchored target Property column structure overflowed"),
										"logical-plan anchored target Property column structure overflowed");
								targetPropertyReadBytes.set(targetPropertyStructureBytes);
								const std::function<void(size_t)> admission = [&](size_t bytes) {
									checkGuard(state);
									targetPropertyReadBytes.set(checkedSizeAdd(targetPropertyReadBytes.bytes(), bytes,
										"logical-plan anchored target Property batch size overflowed"));
								};
								resolvedPropertyColumns = state.shared.snapshot.supportsPropertyReadAdmission()
									? state.shared.snapshot.readNodePropertyColumnsSortedWithAdmission(uncachedPropertyIds, keyViews, admission)
									: state.shared.snapshot.readNodePropertyColumnsSorted(uncachedPropertyIds, keyViews);
								if (resolvedPropertyColumns.size() != targetPropertyKeys.size())
									throw std::runtime_error("logical graph backend changed anchored target Property column cardinality");
								for (const auto &column: resolvedPropertyColumns)
									if (column.size() != uncachedPropertyIds.size())
										throw std::runtime_error("logical graph backend changed anchored target Property result cardinality");
							}
							size_t actualColumnsBytes = checkedSizeMul(resolvedPropertyColumns.capacity(), sizeof(std::vector<PropertyValue>),
								"logical-plan anchored column capacity overflowed");
							for (const auto &column: resolvedPropertyColumns) {
								actualColumnsBytes = checkedSizeAdd(actualColumnsBytes, checkedSizeMul(column.capacity(), sizeof(PropertyValue),
									"logical-plan anchored column value capacity overflowed"), "logical-plan anchored column size overflowed");
								for (const auto &value: column) actualColumnsBytes = checkedSizeAdd(actualColumnsBytes,
									property_utils::getPropertyValueSize(value), "logical-plan anchored column payload overflowed");
							}
							targetPropertyReadBytes.set(actualColumnsBytes);
							checkGuard(state);
							const size_t cacheLimit = std::min(state.shared.bounds.maximum_edges_per_node, size_t{64U});
							for (size_t ordinal = 0U; ordinal < uncachedPropertyIds.size() && targetPropertyCache.size() < cacheLimit; ++ordinal) {
								size_t payloadBytes = 0U;
								for (const auto &column: resolvedPropertyColumns)
									payloadBytes = checkedSizeAdd(payloadBytes, property_utils::getPropertyValueSize(column[ordinal]),
										"logical-plan anchored Property cache payload overflowed");
								const size_t valueBytes = checkedSizeAdd(payloadBytes,
									checkedSizeMul(targetPropertyKeys.size(), sizeof(PropertyValue), "logical-plan anchored Property cache slots overflowed"),
									"logical-plan anchored Property cache size overflowed");
								const size_t nextValues = checkedSizeAdd(targetPropertyCacheBytes, valueBytes,
									"logical-plan anchored Property cache size overflowed");
								const size_t old = targetPropertyCache.capacity();
								const bool grow = targetPropertyCache.size() == old;
								const size_t next = grow ? (old == 0U ? std::min(cacheLimit, size_t{8U}) : old + std::min(old, cacheLimit - old)) : old;
								const size_t extraSlots = grow ? next * sizeof(PropertyCacheEntry) : 0U;
								const size_t combined = checkedSizeAdd(nextValues, extraSlots, "logical-plan anchored Property cache peak overflowed");
								if (checkedSizeAdd(combined, propertyCacheSlots.bytes(), "logical-plan anchored Property cache limit overflowed")
									> MAXIMUM_MATERIALIZED_CACHE_BYTES || targetPropertyCacheReservation.wouldExceed(combined)) continue;
								targetPropertyCacheReservation.set(nextValues);
								if (grow) {
									propertyCacheSlots.set((old + next) * sizeof(PropertyCacheEntry));
									targetPropertyCache.reserve(next);
									propertyCacheSlots.set(targetPropertyCache.capacity() * sizeof(PropertyCacheEntry));
								}
								std::vector<PropertyValue> values;
								values.reserve(targetPropertyKeys.size());
								const size_t actualBytes = checkedSizeAdd(payloadBytes,
									checkedSizeMul(values.capacity(), sizeof(PropertyValue), "logical-plan anchored Property cache capacity overflowed"),
									"logical-plan anchored Property cache capacity overflowed");
								targetPropertyCacheReservation.set(checkedSizeAdd(targetPropertyCacheBytes, actualBytes,
									"logical-plan anchored Property cache actual size overflowed"));
								for (const auto &column: resolvedPropertyColumns) values.push_back(column[ordinal]);
								targetPropertyCache.emplace_back(uncachedPropertyIds[ordinal], std::move(values));
								targetPropertyCacheBytes += actualBytes;
							}
						}
						MaterializedByteReservation existenceBytes(&state.shared.materialized_budget);
						std::vector<bool> targetExists;
						if (state.shared.snapshot.supportsNodeExistenceProbes()) {
							existenceBytes.set(checkedSizeAdd(bitBytes(targetIds.size()), sizeof(size_t),
								"logical-plan anchored existence bit slots overflowed"));
							targetExists = state.shared.snapshot.containsNodesSorted(targetIds);
							existenceBytes.set(bitBytes(targetExists.capacity()));
							checkGuard(state);
							if (targetExists.size() != targetIds.size()) {
								throw std::runtime_error(
										"logical graph backend changed anchored target existence cardinality");
							}
						}
						for (const PendingTarget &candidate: pendingTargets) {
							checkGuard(state);
							if (!candidate.type_matches) {
								continue;
							}
							const auto position = std::ranges::lower_bound(targetIds, candidate.target_node_id);
							if (position == targetIds.end() || *position != candidate.target_node_id) {
								throw std::runtime_error("logical graph backend lost an anchored target Node");
							}
							const size_t ordinal = static_cast<size_t>(position - targetIds.begin());
							if (!targetExists.empty() && !targetExists[ordinal]) {
								throw std::runtime_error(
										"logical graph backend returned an Edge topology with a missing endpoint");
							}
							if (!labelsMatch.empty() && !labelsMatch[ordinal]) {
								continue;
							}
							bool matches = true;
							if (targetPropertiesCanBeSelected) {
								for (size_t index = 0U; index < traversal.getTargetProperties().size(); ++index) {
									const auto &[key, expected] = traversal.getTargetProperties()[index];
									const auto keyPosition = std::ranges::find(targetPropertyKeys, key);
									if (keyPosition == targetPropertyKeys.end()) {
										throw std::logic_error(
												"logical-plan anchored target Property column is missing");
									}
									const size_t columnIndex =
											static_cast<size_t>(keyPosition - targetPropertyKeys.begin());
									const auto cached = cachedProperties(candidate.target_node_id);
									const PropertyValue *actual = nullptr;
									if (cached != targetPropertyCache.end()) {
										actual = &cached->second[columnIndex];
									} else {
										const auto propertyRow =
												std::ranges::lower_bound(uncachedPropertyIds, candidate.target_node_id);
										if (propertyRow == uncachedPropertyIds.end() ||
											*propertyRow != candidate.target_node_id) {
											throw std::runtime_error(
													"logical graph backend lost anchored target Property values");
										}
										actual = &resolvedPropertyColumns[columnIndex][static_cast<size_t>(
												propertyRow - uncachedPropertyIds.begin())];
									}
									if (actual == nullptr || *actual != expected) {
										matches = false;
										break;
									}
									if (isNull(*actual)) {
										checkGuardNow(state);
										const auto present = state.shared.snapshot.nodePropertyExists(candidate.target_node_id, key);
										if (!present) throw std::runtime_error("anchored count Property presence probe is unavailable");
										if (!*present) { matches = false; break; }
									}
								}
							}
							if (!matches) {
								continue;
							}
							if (inputRows == state.shared.bounds.maximum_rows) {
								throw std::runtime_error("logical-plan Aggregate exceeded its input row bound");
							}
							incrementBounded(inputRows, "logical-plan Aggregate input counter overflowed");
						}
						pendingTargets.clear();
						return true;
					};
					const auto consumeTopology = [&](const uint64_t edgeId, const uint64_t edgeSourceNodeId,
													 const uint64_t edgeTargetNodeId, const bool typeMatches) {
						checkGuard(state);
						if (sourceEdges == state.shared.bounds.maximum_edges_per_node) {
							throw std::runtime_error(
									"logical-plan anchored relationship count exceeded its per-Node Edge bound");
						}
						validateAdjacentEdgeOrder(edgeId, previousEdgeId, "anchored relationship count");
						const bool endpointMatches = direction == storage::LogicalGraphReadDirection::LGRD_OUTGOING
															 ? edgeSourceNodeId == sourceNodeId
															 : edgeTargetNodeId == sourceNodeId;
						if (!endpointMatches) {
							throw std::runtime_error(
									"logical graph backend returned an invalid anchored relationship topology");
						}
						++sourceEdges;
						incrementBounded(state.task.stats.edges_scanned,
										 "logical-plan anchored relationship Edge counter overflowed");
						if (traversal.getDirection() == "both" &&
							direction == storage::LogicalGraphReadDirection::LGRD_INCOMING &&
							edgeSourceNodeId == sourceNodeId) {
							return true;
						}
						if (!typeMatches) {
							return true;
						}
						const uint64_t targetId = direction == storage::LogicalGraphReadDirection::LGRD_OUTGOING
														  ? edgeTargetNodeId
														  : edgeSourceNodeId;
						// Native reference pages already expose the complete endpoint set for
						// this bounded batch. Keep endpoint liveness in that same batch even
						// when no target label/property predicate is present.
						const bool requiresBatchedTargetRead =
								(useNativeAdjacencyRefs && targetPropertiesCanBeSelected) ||
								(traversal.getTargetProperties().empty() &&
								 (!traversal.getTargetLabels().empty() || supportsExistenceProbes));
						if (!requiresBatchedTargetRead) {
							if (!traversal.getTargetLabels().empty() || !traversal.getTargetProperties().empty()) {
								const storage::LogicalGraphNode target = readNodeCached(state, targetId);
								if (!nodeMatches(state.shared.snapshot, target, traversal.getTargetLabels(),
												 traversal.getTargetProperties(), state)) {
									return true;
								}
							} else if (supportsExistenceProbes) {
								if (!state.shared.snapshot.containsNode(targetId)) {
									throw std::runtime_error(
											"logical graph backend returned an Edge topology with a missing endpoint");
								}
							} else {
								validateNodeIdentity(state, targetId);
							}
						}
						// A native reference batch validates all target endpoints in
						// flushTargets().  Do not probe this endpoint eagerly here: doing
						// so turns a page-bounded batch into one Directory lookup per Edge.
						if (requiresBatchedTargetRead) {
							if (pendingTargets.size() == pendingTargets.capacity()) {
								const size_t old = pendingTargets.capacity();
								size_t next = old == 0U ? std::min(targetBatchCapacity, size_t{16U})
									: old + std::min(old, targetBatchCapacity - old);
								size_t peak = checkedSizeMul(old + next, sizeof(PendingTarget),
									"logical-plan anchored pending capacity overflowed");
								if (pendingBytes.wouldExceed(peak)) {
									if (old != 0U) {
										if (!flushTargets()) return false;
									} else {
										next = 1U;
										peak = sizeof(PendingTarget);
									}
								}
								if (pendingTargets.size() == pendingTargets.capacity()) {
									pendingBytes.set(peak);
									pendingTargets.reserve(next);
									debug::PerfTrace::addValue("logical_plan.anchored_pending_capacity",
										static_cast<int64_t>(std::min(pendingTargets.capacity(), static_cast<size_t>(std::numeric_limits<int64_t>::max()))));
									pendingBytes.set(checkedSizeMul(pendingTargets.capacity(), sizeof(PendingTarget),
										"logical-plan anchored pending capacity overflowed"));
								}
							}
							pendingTargets.push_back({edgeId, edgeSourceNodeId, edgeTargetNodeId, typeMatches});
							if (pendingTargets.size() >= targetBatchCapacity) {
								return flushTargets();
							}
							return true;
						}
						if (inputRows == state.shared.bounds.maximum_rows) {
							throw std::runtime_error("logical-plan Aggregate exceeded its input row bound");
						}
						incrementBounded(inputRows, "logical-plan Aggregate input counter overflowed");
						return true;
					};
					if (sourceEdges == state.shared.bounds.maximum_edges_per_node) {
						// Ask for one bounded probe so an additional Edge is rejected rather
						// than silently truncating the aggregate input.
						if (useNativeAdjacencyRefs) {
							MaterializedByteReservation referenceBytes(&state.shared.materialized_budget);
							const auto page = readReferencePageBudgeted(state, sourceNodeId, direction, std::nullopt, 1U, referenceBytes);
							if (!page.edges.empty()) {
								throw std::runtime_error(
										"logical-plan anchored relationship count exceeded its per-Node Edge bound");
							}
						} else {
							state.shared.snapshot.visitAdjacentEdgeTopologies(
									sourceNodeId, direction, 1U, [](const storage::LogicalGraphEdgeTopology &) -> bool {
										throw std::runtime_error("logical-plan anchored relationship count exceeded "
																 "its per-Node Edge bound");
									});
						}
						return;
					}
					const size_t directionLimit = state.shared.bounds.maximum_edges_per_node - sourceEdges;
					if (useNativeAdjacencyRefs) {
						std::optional<storage::LogicalGraphAdjacencyContinuation> continuation;
						for (;;) {
							if (sourceEdges == state.shared.bounds.maximum_edges_per_node) {
								throw std::runtime_error(
										"logical-plan anchored relationship count exceeded its per-Node Edge bound");
							}
							const size_t pageRecords = std::min(state.shared.bounds.maximum_edges_per_node - sourceEdges,
																state.shared.bounds.maximum_adjacency_page_records);
							MaterializedByteReservation referenceBytes(&state.shared.materialized_budget);
							const auto page = readReferencePageBudgeted(state, sourceNodeId, direction, continuation, pageRecords, referenceBytes);
							if (page.edges.size() > pageRecords ||
								(page.edges.empty() && page.continuation.has_value()) ||
								(continuation.has_value() &&
								 (page.edges.empty() || page.edges.front().entity_id != continuation->next_edge_id))) {
								throw std::runtime_error("logical graph backend returned an invalid anchored "
														 "relationship reference page");
							}
							for (const storage::EntityRef &reference: page.edges) {
								validateEdgeRef(reference, "logical graph backend returned an invalid or Node "
														   "reference from an anchored relationship page");
								const bool typeMatches = !relationshipTypeToken.has_value() ||
														 reference.type_token == *relationshipTypeToken;
								if (!consumeTopology(reference.entity_id, reference.source_node_id,
													 reference.target_node_id, typeMatches)) {
									return;
								}
							}
							if (!page.continuation.has_value()) {
								if (!flushTargets()) {
									return;
								}
								return;
							}
							if (page.edges.empty() ||
								page.continuation->previous_edge_id != page.edges.back().entity_id ||
								page.continuation->next_edge_id == 0U ||
								page.continuation->previous_edge_id == page.continuation->next_edge_id) {
								throw std::runtime_error("logical graph backend returned an invalid anchored "
														 "relationship reference continuation");
							}
							continuation = page.continuation;
						}
					}
					state.shared.snapshot.visitAdjacentEdgeTopologies(
							sourceNodeId, direction, directionLimit,
							[&](const storage::LogicalGraphEdgeTopology &edge) {
								return consumeTopology(edge.edge_id, edge.source_node_id, edge.target_node_id,
													   traversal.getEdgeType().empty() ||
															   edge.type == traversal.getEdgeType());
							});
					flushTargets();
				};
				if (traversal.getDirection() == "out" || traversal.getDirection() == "both") {
					visitDirection(storage::LogicalGraphReadDirection::LGRD_OUTGOING);
				}
				if (traversal.getDirection() == "both" || traversal.getDirection() == "in") {
					visitDirection(storage::LogicalGraphReadDirection::LGRD_INCOMING);
				}
				return true;
			};
			const InternalVisitor sourceVisitor = visitSource;
			const bool canUseGlobalSourceSet = traversal.getTargetProperties().empty() && supportsExistenceProbes &&
											   state.shared.snapshot.supportsNativeBatchAdjacentEdgeRefs() &&
											   (traversal.getEdgeType().empty() || relationshipTypeToken.has_value());
			if (!canUseGlobalSourceSet) {
				static_cast<void>(visitOperator(seed, state, sourceVisitor));
			} else {
				std::vector<uint64_t> sourceIds;
				sourceIds.reserve(std::min(state.shared.bounds.maximum_node_records,
										   state.shared.bounds.maximum_adjacency_page_records * size_t{2U}));
				static_cast<void>(visitOperator(seed, state, [&](LogicalPlanReadRow row) {
					const uint64_t sourceId = nodeBindingId(requireBinding(row, traversal.getSourceVar()), state);
					if (sourceId == 0U) {
						throw std::runtime_error(
								"logical-plan anchored relationship count returned an invalid source Node");
					}
					if (sourceIds.size() == state.shared.bounds.maximum_node_records) {
						throw std::runtime_error(
								"logical-plan anchored relationship count exceeded its source Node bound");
					}
					sourceIds.push_back(sourceId);
					return true;
				}));
				std::ranges::sort(sourceIds);
				sourceIds.erase(std::unique(sourceIds.begin(), sourceIds.end()), sourceIds.end());
				const size_t globalSourceThreshold = std::max<size_t>(1U, state.shared.bounds.maximum_adjacency_page_records);
				if (sourceIds.size() < globalSourceThreshold) {
					for (const uint64_t sourceId: sourceIds) {
						LogicalPlanReadRow row;
						row.values.emplace(traversal.getSourceVar(),
										   LogicalPlanReadValue(storage::EntityRef{sourceId, false, 0U, 0U, 0U}));
						static_cast<void>(visitSource(std::move(row)));
					}
				} else {
					struct PendingGlobalEdge {
						uint64_t target_node_id = 0U;
					};
					std::unordered_set<uint64_t> sourceSet;
					sourceSet.reserve(sourceIds.size());
					for (const uint64_t sourceId: sourceIds) {
						static_cast<void>(sourceSet.insert(sourceId));
					}
					std::unordered_map<uint64_t, size_t> sourceEdgeCounts;
					sourceEdgeCounts.reserve(sourceIds.size());
					size_t globalEdgesScanned = 0U;
					std::vector<PendingGlobalEdge> pending;
					const size_t globalBatchCapacity =
							std::max<size_t>(1U, std::min(state.shared.bounds.maximum_adjacency_page_records,
														  state.shared.bounds.maximum_edge_records));
					pending.reserve(globalBatchCapacity);
					const auto flushGlobal = [&]() {
						if (pending.empty()) {
							return;
						}
						std::vector<uint64_t> targetIds;
						targetIds.reserve(pending.size());
						for (const PendingGlobalEdge &candidate: pending) {
							targetIds.push_back(candidate.target_node_id);
						}
						detail::sortUniqueIdsGuarded(targetIds, [&] {
							if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
						});
						const std::vector<bool> present = state.shared.snapshot.containsNodesSorted(targetIds);
						if (present.size() != targetIds.size()) {
							throw std::runtime_error(
									"logical graph backend changed anchored global target existence cardinality");
						}
						const std::vector<bool> labelsMatch = traversal.getTargetLabels().empty()
																	  ? std::vector<bool>(targetIds.size(), true)
																	  : state.shared.snapshot.readNodeLabelMatchesSorted(
																				targetIds, traversal.getTargetLabels());
						if (labelsMatch.size() != targetIds.size()) {
							throw std::runtime_error(
									"logical graph backend changed anchored global target label cardinality");
						}
						for (const PendingGlobalEdge &candidate: pending) {
							const auto position = std::ranges::lower_bound(targetIds, candidate.target_node_id);
							if (position == targetIds.end() || *position != candidate.target_node_id ||
								!present[static_cast<size_t>(position - targetIds.begin())]) {
								throw std::runtime_error(
										"logical graph backend returned an Edge topology with a missing endpoint");
							}
							if (!labelsMatch[static_cast<size_t>(position - targetIds.begin())]) {
								continue;
							}
							if (inputRows == state.shared.bounds.maximum_rows) {
								throw std::runtime_error("logical-plan Aggregate exceeded its input row bound");
							}
							incrementBounded(inputRows, "logical-plan Aggregate input counter overflowed");
						}
						pending.clear();
					};
					const auto visitGlobalDirection = [&](const storage::LogicalGraphReadDirection direction) {
						if (globalEdgesScanned == state.shared.bounds.maximum_edge_records) {
							throw std::runtime_error(
									"logical-plan anchored global relationship count exceeded its Edge bound");
						}
						const size_t remaining = state.shared.bounds.maximum_edge_records - globalEdgesScanned;
						const size_t edgeVisitBound =
								checkedSizeAdd(remaining, 1U, "logical-plan anchored global Edge bound overflowed");
						state.shared.snapshot.visitAdjacentEdgeRefsForNodes(
								sourceIds, direction, edgeVisitBound, [&](const storage::EntityRef &reference) {
									checkGuard(state);
									validateEdgeRef(reference, "logical-plan anchored global relationship count "
															   "received an invalid or Node reference");
									if (globalEdgesScanned == state.shared.bounds.maximum_edge_records) {
										throw std::runtime_error("logical-plan anchored global relationship count "
																 "exceeded its Edge bound");
									}
									incrementBounded(globalEdgesScanned,
													 "logical-plan anchored global Edge counter overflowed");
									incrementBounded(state.task.stats.edges_scanned,
													 "logical-plan anchored global Edge statistics overflowed");
									const bool outgoing =
											direction == storage::LogicalGraphReadDirection::LGRD_OUTGOING;
									const uint64_t sourceId =
											outgoing ? reference.source_node_id : reference.target_node_id;
									if (!sourceSet.contains(sourceId)) {
										return true;
									}
									const auto countSourceEdge = [&](const uint64_t sourceId) {
										const size_t count = sourceEdgeCounts[sourceId];
										if (count == state.shared.bounds.maximum_edges_per_node) {
											throw std::runtime_error("logical-plan anchored relationship count "
																	 "exceeded its per-Node Edge bound");
										}
										sourceEdgeCounts[sourceId] = count + 1U;
									};
									countSourceEdge(sourceId);
									// BOTH visits the incoming and outgoing indexes independently. A
									// self-loop is one adjacency row for its source and must not be
									// emitted again by the incoming pass.
									if (traversal.getDirection() == "both" && !outgoing &&
										reference.source_node_id == reference.target_node_id) {
										return true;
									}
									if (relationshipTypeToken.has_value() &&
										reference.type_token != *relationshipTypeToken) {
										return true;
									}
									pending.push_back({outgoing ? reference.target_node_id : reference.source_node_id});
									if (pending.size() >= globalBatchCapacity) {
										flushGlobal();
									}
									return true;
								});
					};
					if (traversal.getDirection() == "out" || traversal.getDirection() == "both") {
						visitGlobalDirection(storage::LogicalGraphReadDirection::LGRD_OUTGOING);
					}
					if (traversal.getDirection() == "in" || traversal.getDirection() == "both") {
						visitGlobalDirection(storage::LogicalGraphReadDirection::LGRD_INCOMING);
					}
					flushGlobal();
				}
			}
			LogicalPlanReadRow result;
			result.values.emplace(count.alias, PropertyValue(static_cast<int64_t>(inputRows)));
			return visitor(std::move(result));
		}

		[[nodiscard]] std::optional<bool> tryVisitDirectRelationshipCount(const logical::LogicalAggregate &aggregate,
																		  ExecutionState &state,
																		  const InternalVisitor &visitor) {
			const auto &groupExpressions = aggregate.getGroupByExprs();
			const auto &aggregations = aggregate.getAggregations();
			if (!groupExpressions.empty() || aggregations.size() != 1U) {
				return std::nullopt;
			}
			const logical::LogicalAggItem &count = aggregations.front();
			if (count.functionName != "count" || count.distinct || count.alias.empty() || count.extraArg) {
				return std::nullopt;
			}

			const std::vector<logical::LogicalOperator *> aggregateChildren = aggregate.getChildren();
			if (aggregateChildren.size() != 1U || aggregateChildren.front() == nullptr) {
				return std::nullopt;
			}
			const logical::LogicalOperator *traversalOperator = aggregateChildren.front();
			MaterializedByteReservation predicateViewBytes(&state.shared.materialized_budget);
			std::vector<DirectEdgePredicateView> pushedEdgePredicates;
			if (traversalOperator->getType() == logical::LogicalOpType::LOP_FILTER) {
				const auto &filter = dynamic_cast<const logical::LogicalFilter &>(*traversalOperator);
				if (!filter.getPredicate()) {
					return std::nullopt;
				}
				const logical::LogicalOperator &filterChild = requireOnlyChild(filter);
				if (filterChild.getType() != logical::LogicalOpType::LOP_TRAVERSAL) {
					return std::nullopt;
				}
				const auto &filteredTraversal = dynamic_cast<const logical::LogicalTraversal &>(filterChild);
				auto inspected =
						inspectDirectEdgePropertyFilterViews(*filter.getPredicate(), filteredTraversal.getEdgeVar());
				if (!inspected.has_value()) {
					return std::nullopt;
				}
				pushedEdgePredicates = std::move(*inspected);
				traversalOperator = &filterChild;
			}
			if (traversalOperator->getType() != logical::LogicalOpType::LOP_TRAVERSAL) {
				return std::nullopt;
			}
			const auto &traversal = dynamic_cast<const logical::LogicalTraversal &>(*traversalOperator);
			if (traversal.getDirection() != "out" || traversal.getEdgeVar().empty() ||
				!traversal.getTargetLabels().empty() || !traversal.getTargetProperties().empty()) {
				return std::nullopt;
			}
			if (count.argument) {
				const auto *variable =
						dynamic_cast<const expressions::VariableReferenceExpression *>(count.argument.get());
				if (variable == nullptr || variable->hasProperty() ||
					variable->getVariableName() != traversal.getEdgeVar()) {
					return std::nullopt;
				}
			}

			const std::vector<logical::LogicalOperator *> traversalChildren = traversal.getChildren();
			if (traversalChildren.size() != 1U || traversalChildren.front() == nullptr ||
				traversalChildren.front()->getType() != logical::LogicalOpType::LOP_NODE_SCAN) {
				return std::nullopt;
			}
			const auto &seed = dynamic_cast<const logical::LogicalNodeScan &>(*traversalChildren.front());
			if (seed.getVariable() != traversal.getSourceVar() || !seed.getLabels().empty() ||
				!seed.getPropertyPredicates().empty() || !seed.getRangePredicates().empty() ||
				seed.getCompositeEquality().has_value()) {
				return std::nullopt;
			}

			if (state.shared.bounds.maximum_rows == std::numeric_limits<size_t>::max()) {
				throw std::overflow_error("logical-plan direct relationship count input bound overflowed");
			}
			debug::ScopedPerfTimer timer("logical_plan.direct_relationship_count");
			if (debug::PerfTrace::areValuesEnabled()) {
				debug::PerfTrace::addValue("logical_plan.direct_relationship_count", 1);
			}
			// Defer endpoint validation until the scan completes. The same bounded
			// endpoint set was previously probed once per Property batch, which
			// restarted the Entity Directory cursor hundreds of times on large scans.
			// No result is emitted before this set is authenticated.
			const size_t endpointCacheCapacity = state.shared.bounds.maximum_node_records;
			MaterializedByteReservation endpointBytes(&state.shared.materialized_budget);
			std::unordered_set<uint64_t> endpointsToValidate;
			// Charge the key plus conservative node/bucket links before inserting.
			// Do not reserve from the adjacency page bound: a large configured page
			// may otherwise allocate an endpoint table before the shared byte budget
			// has admitted any endpoint.
			constexpr size_t endpointEntryBytes = sizeof(uint64_t) + 2U * sizeof(void *);
			const bool supportsExistenceProbes = state.shared.snapshot.supportsNodeExistenceProbes();
			const auto validateEndpoints = [&]() {
				MaterializedByteReservation endpointScratch(&state.shared.materialized_budget);
				endpointScratch.set(checkedSizeMul(endpointsToValidate.size(), sizeof(uint64_t),
												   "logical-plan count endpoint scratch overflowed"));
				std::vector<uint64_t> endpointIds(endpointsToValidate.begin(), endpointsToValidate.end());
				if (endpointIds.empty()) {
					return;
				}
				std::ranges::sort(endpointIds);
				if (supportsExistenceProbes) {
					endpointScratch.set(
							checkedSizeAdd(endpointScratch.bytes(),
										   endpointIds.size() / 8U + static_cast<size_t>(endpointIds.size() % 8U != 0U),
										   "logical-plan count endpoint presence scratch overflowed"));
					const std::vector<bool> present = state.shared.snapshot.containsNodesSorted(endpointIds);
					if (present.size() != endpointIds.size()) {
						throw std::runtime_error("logical graph backend changed Node existence batch cardinality");
					}
					for (size_t index = 0U; index < endpointIds.size(); ++index) {
						if (!present[index]) {
							throw std::runtime_error(
									"logical graph backend returned an Edge topology with a missing endpoint");
						}
					}
				} else {
					for (const uint64_t nodeId: endpointIds) {
						validateNodeIdentity(state, nodeId);
					}
				}
			};

			std::vector<DirectEdgePredicateView> edgePredicates;
			edgePredicates.reserve(traversal.getEdgeProperties().size() + pushedEdgePredicates.size());
			for (const auto &[key, expected]: traversal.getEdgeProperties()) {
				if (key.empty()) {
					throw std::invalid_argument("logical-plan Edge Property predicate has an empty key");
				}
				if (isNull(expected)) {
					LogicalPlanReadRow result;
					result.values.emplace(count.alias, PropertyValue(int64_t{0}));
					return visitor(std::move(result));
				}
				edgePredicates.push_back({key, &expected, nullptr, PropertyValue()});
			}
			std::ranges::sort(edgePredicates, {}, &DirectEdgePredicateView::key);
			for (const auto &predicate: pushedEdgePredicates) {
				const auto existing = std::ranges::find(edgePredicates, predicate.key, &DirectEdgePredicateView::key);
				if (existing == edgePredicates.end())
					edgePredicates.push_back(predicate);
				else if (!existing->sameValue(predicate)) {
					LogicalPlanReadRow result;
					result.values.emplace(count.alias, PropertyValue(int64_t{0}));
					return visitor(std::move(result));
				}
			}
			std::vector<std::string_view> edgePropertyKeys;
			edgePropertyKeys.reserve(edgePredicates.size());
			for (const auto &predicate: edgePredicates) {
				edgePropertyKeys.push_back(predicate.key);
			}
			std::optional<uint64_t> relationshipTypeToken;
			if (!traversal.getEdgeType().empty() &&
				state.shared.snapshot.supportsNativeRelationshipTypeTokens(traversal.getEdgeType())) {
				relationshipTypeToken = state.shared.snapshot.findRelationshipTypeToken(traversal.getEdgeType());
				if (!relationshipTypeToken.has_value()) {
					LogicalPlanReadRow result;
					result.values.emplace(count.alias, PropertyValue(int64_t{0}));
					return visitor(std::move(result));
				}
			}

			// Only view/scalar slots are retained here; payloads stay in the plan.
			predicateViewBytes.set(
					checkedSizeAdd(checkedSizeMul(checkedSizeAdd(edgePredicates.size(), pushedEdgePredicates.size(),
																 "logical-plan count predicate count overflowed"),
												  sizeof(DirectEdgePredicateView),
												  "logical-plan count predicate view size overflowed"),
								   checkedSizeMul(edgePropertyKeys.size(), sizeof(std::string_view),
												  "logical-plan count predicate key view size overflowed"),
								   "logical-plan count predicate scratch overflowed"));
			size_t inputRows = 0U;
			size_t edgeRecords = 0U;
			// Keep topology-only counts batched as well.  A batch of one was useful
			// for an early prototype, but it turns every topology page into a
			// per-edge callback and prevents the same bounded execution substrate
			// from amortizing validation and selection work when no Property column
			// is requested.
			const size_t batchCapacity = std::min(
					{DEFAULT_PROPERTY_BATCH_ROWS, state.shared.bounds.maximum_edge_records, state.shared.bounds.maximum_rows});
			BoundedBatch<storage::LogicalGraphEdgeTopology> pending(
					batchCapacity, state.shared.bounds.maximum_materialized_bytes,
					[](const auto &edge) {
						return checkedSizeAdd(sizeof(storage::LogicalGraphEdgeTopology), edge.type.size(),
											  "logical-plan count topology size overflowed");
					},
					&state.shared.materialized_budget,
                            state.shared.context && state.shared.context->resource_account, sizeof(storage::LogicalGraphEdgeTopology));
			pending.reserve(batchCapacity);
			const auto countTopology = [&](const storage::LogicalGraphEdgeTopology &edge) {
				if (inputRows == state.shared.bounds.maximum_rows) {
					throw std::runtime_error("logical-plan Aggregate exceeded its input row bound");
				}
				incrementBounded(inputRows, "logical-plan Aggregate input counter overflowed");
			};
			const auto flushPending = [&]() {
				if (pending.empty()) {
					return;
				}
				for (const auto &edge: pending.values()) {
					for (const uint64_t endpointId: {edge.source_node_id, edge.target_node_id}) {
						if (!endpointsToValidate.contains(endpointId) &&
							endpointsToValidate.size() == endpointCacheCapacity) {
							throw std::runtime_error(
									"logical-plan direct relationship count exceeded its endpoint Node bound");
						}
						if (!endpointsToValidate.contains(endpointId)) {
							const size_t previous = endpointBytes.bytes();
							endpointBytes.set(checkedSizeAdd(previous, endpointEntryBytes,
															 "logical-plan count endpoint size overflowed"));
							try {
								endpointsToValidate.insert(endpointId);
							} catch (...) {
								endpointBytes.set(previous);
								throw;
							}
						}
					}
				}
				if (edgePredicates.empty()) {
					for (const storage::LogicalGraphEdgeTopology &edge: pending.values()) {
						countTopology(edge);
					}
					pending.clear();
					return;
				}
				MaterializedByteReservation ordinalBytes(&state.shared.materialized_budget);
				ordinalBytes.set(
						checkedSizeMul(pending.size(), sizeof(size_t), "logical-plan count ordinal size overflowed"));
				std::vector<size_t> sortedOrdinals(pending.size());
				for (size_t index = 0U; index < sortedOrdinals.size(); ++index) {
					sortedOrdinals[index] = index;
				}
				std::ranges::sort(sortedOrdinals, [&pending](const size_t left, const size_t right) {
					return pending.values()[left].edge_id < pending.values()[right].edge_id;
				});
				detail::BudgetMemoryResource selectionMemory(state.shared.materialized_budget);
				SelectionVector selection(pending.size(), &selectionMemory);
				struct CountPropertyBytePressure {};
				std::function<void(size_t, size_t)> selectRange;
				selectRange = [&](size_t begin, size_t end) {
					try {
						MaterializedByteReservation idBytes(&state.shared.materialized_budget);
						const size_t requestedIds =
								checkedSizeMul(end - begin, sizeof(uint64_t), "logical-plan count ID size overflowed");
						if (idBytes.wouldExceed(requestedIds))
							throw CountPropertyBytePressure{};
						idBytes.set(requestedIds);
						std::vector<uint64_t> edgeIds;
						edgeIds.reserve(end - begin);
						for (size_t index = begin; index < end; ++index) {
							const uint64_t id = pending.values()[sortedOrdinals[index]].edge_id;
							if (!edgeIds.empty() && edgeIds.back() == id) {
								throw std::runtime_error(
										"logical graph direct relationship count returned duplicate Edge IDs");
							}
							edgeIds.push_back(id);
						}
						MaterializedByteReservation columnBytes(&state.shared.materialized_budget);
						const std::function<void(size_t)> admission = [&](size_t bytes) {
							checkGuard(state);
							const size_t next = checkedSizeAdd(columnBytes.bytes(), bytes,
															   "logical-plan count source size overflowed");
							if (columnBytes.wouldExceed(next))
								throw CountPropertyBytePressure{};
							columnBytes.set(next);
							debug::PerfTrace::addValue(
									"logical_plan.direct_relationship_count_source_bytes",
									static_cast<int64_t>(
											std::min(bytes, static_cast<size_t>(std::numeric_limits<int64_t>::max()))));
						};
						const auto sortedColumns =
								state.shared.snapshot.supportsPropertyReadAdmission()
										? state.shared.snapshot.readEdgePropertyColumnsSortedWithAdmission(
												  edgeIds, edgePropertyKeys, admission)
										: state.shared.snapshot.readEdgePropertyColumnsSorted(edgeIds, edgePropertyKeys);
						if (sortedColumns.size() != edgePredicates.size()) {
							throw std::runtime_error("logical graph backend changed Edge Property column cardinality");
						}
						for (const auto &column: sortedColumns) {
							if (column.size() != end - begin) {
								throw std::runtime_error(
										"logical graph backend changed sorted Edge Property result cardinality");
							}
						}
						const size_t retained =
								propertyColumnBytes(sortedColumns, edgePropertyKeys,
													"logical-plan direct Edge Property batch size overflowed", state.shared.context);
						if (columnBytes.wouldExceed(retained))
							throw CountPropertyBytePressure{};
						columnBytes.set(retained);
						for (size_t property = 0U; property < edgePredicates.size(); ++property) {
							for (size_t index = 0U; index < end - begin; ++index) {
								const size_t original = sortedOrdinals[begin + index];
								if (selection.includes(original) &&
									(isNull(sortedColumns[property][index]) ||
									 !edgePredicates[property].matches(sortedColumns[property][index])))
									selection.reject(original);
							}
						}
						return;
					} catch (const CountPropertyBytePressure &) {
						// Destroy columns and refund their lease before issuing a smaller read.
						if (end - begin == 1U) {
							throw std::runtime_error("logical-plan direct Edge Property batch exceeded its byte bound");
						}
					}
					debug::PerfTrace::addValue("logical_plan.direct_relationship_count_split_batches", 1);
					const size_t middle = begin + (end - begin) / 2U;
					selectRange(begin, middle);
					selectRange(middle, end);
				};
				selectRange(0U, pending.size());
				for (size_t index = 0U; index < pending.size(); ++index) {
					if (!selection.includes(index)) {
						continue;
					}
					countTopology(pending.values()[index]);
				}
				pending.clear();
			};
			const auto consumeTopology = [&](const storage::LogicalGraphEdgeTopology &edge) {
				checkGuard(state);
				if (edgeRecords == state.shared.bounds.maximum_edge_records) {
					throw std::runtime_error("logical-plan direct relationship count exceeded its Edge record bound");
				}
				incrementBounded(edgeRecords, "logical-plan direct relationship count Edge counter overflowed");
				incrementBounded(state.task.stats.edges_scanned, "logical-plan read runtime Edge counter overflowed");
				if (edge.edge_id == 0U || edge.source_node_id == 0U || edge.target_node_id == 0U) {
					throw std::runtime_error(
							"logical graph backend returned an Edge topology with invalid identity or endpoints");
				}
				if (!relationshipTypeToken.has_value() && !traversal.getEdgeType().empty() &&
					edge.type != traversal.getEdgeType()) {
					return true;
				}
				if (!pending.empty() && pending.wouldExceedBytes(edge))
					flushPending();
				pending.append(edge);
				if (pending.size() == batchCapacity) {
					flushPending();
				}
				return true;
			};
			// The default Edge-reference implementation projects complete Edge records.
			// Use it only when the snapshot explicitly provides the native cheap path;
			// generic readers retain their topology-only projection.
			const size_t edgeVisitBound =
					checkedSizeAdd(state.shared.bounds.maximum_edge_records, 1U,
								   "logical-plan direct relationship count Edge bound overflowed");
			if (state.shared.snapshot.supportsNativeEdgeRefScans() &&
				(traversal.getEdgeType().empty() || relationshipTypeToken.has_value())) {
				state.shared.snapshot.visitEdgeRefsForTypePredicate(edgeVisitBound, relationshipTypeToken, [&consumeTopology, &relationshipTypeToken](
																	 const storage::EntityRef &reference) {
					validateEdgeRef(reference,
									"logical-plan direct relationship count received an invalid or Node reference");
					if (relationshipTypeToken.has_value() && reference.type_token != *relationshipTypeToken) {
						return true;
					}
					return consumeTopology(
							{reference.entity_id, reference.source_node_id, reference.target_node_id, {}});
				});
			} else {
				state.shared.snapshot.visitEdgeTopologies(edgeVisitBound, consumeTopology);
			}
			flushPending();
			validateEndpoints();
			endpointsToValidate.clear();
			endpointBytes.reset();
			std::vector<DirectEdgePredicateView>{}.swap(edgePredicates);
			std::vector<DirectEdgePredicateView>{}.swap(pushedEdgePredicates);
			std::vector<std::string_view>{}.swap(edgePropertyKeys);
			predicateViewBytes.reset();
			if (debug::PerfTrace::areValuesEnabled()) {
				debug::PerfTrace::addValue("logical_plan.direct_relationship_count_edge_records",
										   static_cast<int64_t>(edgeRecords));
			}

			LogicalPlanReadRow result;
			result.values.emplace(count.alias, PropertyValue(static_cast<int64_t>(inputRows)));
			return visitor(std::move(result));
		}

		[[nodiscard]] std::optional<bool> tryVisitDirectTraversal(
				const logical::LogicalTraversal &traversal, ExecutionState &state, const InternalVisitor &visitor,
				const std::vector<std::pair<std::string, PropertyValue>> *additionalEdgeProperties = nullptr) {
			if (!state.shared.allow_deferred_entity_refs || !state.shared.snapshot.supportsNativeNodeRefScans() ||
				!state.shared.snapshot.supportsNativeEdgeRefScans() ||
				(traversal.getDirection() != "out" && traversal.getDirection() != "in")) {
				return std::nullopt;
			}
			const bool hasAdditionalEdgeProperties =
					additionalEdgeProperties != nullptr && !additionalEdgeProperties->empty();
			const std::vector<logical::LogicalOperator *> children = traversal.getChildren();
			if (children.size() != 1U || children.front() == nullptr ||
				children.front()->getType() != logical::LogicalOpType::LOP_NODE_SCAN) {
				return std::nullopt;
			}
			const auto &sourceScan = dynamic_cast<const logical::LogicalNodeScan &>(*children.front());
			// An unanchored topology traversal is cheaper as one global Edge scan;
			// constrained sources retain the adjacency cursor for locality.
			const bool sourceConstrained =
					!sourceScan.getLabels().empty() || !sourceScan.getPropertyPredicates().empty() ||
					!sourceScan.getRangePredicates().empty() || sourceScan.getCompositeEquality().has_value() ||
					!sourceScan.getParameterPropertyPredicates().empty();
			if (sourceConstrained && state.shared.snapshot.supportsNativeAdjacentEdgeRefScans()) {
				return std::nullopt;
			}
			if (sourceScan.getVariable() != traversal.getSourceVar() ||
				((!traversal.getEdgeProperties().empty() || hasAdditionalEdgeProperties) &&
				 traversal.getEdgeVar().empty()) ||
				(!traversal.getTargetProperties().empty() && traversal.getTargetVar().empty())) {
				return std::nullopt;
			}

			const auto parameterPredicates = resolveParameterPropertyPredicates(sourceScan, state);
			if (!parameterPredicates.has_value()) {
				return true;
			}
			struct SourceRow {
				uint64_t node_id = 0U;
				// A source Property predicate may already have selected fields in its
				// row cache. Retain only that useful state; label-only and unfiltered
				// sources need just their stable EntityId until an Edge survives.
				std::optional<LogicalPlanReadRow> cached_row;
				size_t edges_seen = 0U;
			};
			std::vector<SourceRow> sources;
			sources.reserve(std::min(state.shared.bounds.maximum_node_records, size_t{4'096U}));
			// An unconstrained source has no predicate that can reduce the Edge
			// stream.  Keep source bindings lazy and let the global Edge cursor drive
			// the pipeline; scanning every Node first would add an unrelated O(|V|)
			// pass to an O(|E|) operation.  The map is query-local and bounded by the
			// same source-record guard used by the old scan path.
			const bool globalEdgePath = !sourceConstrained;
			std::unordered_map<uint64_t, size_t> sourceOrdinals;
			if (globalEdgePath) {
				sourceOrdinals.reserve(std::min(state.shared.bounds.maximum_node_records, size_t{4'096U}));
			} else {
				state.shared.snapshot.visitNodeRefsForLabelPredicate(
						state.shared.bounds.maximum_node_records, sourceScan.getLabels(),
						[&](const storage::EntityRef &reference, const bool labelsMatch) {
							checkGuard(state);
							if (reference.is_edge || reference.entity_id == 0U) {
								throw std::runtime_error(
										"logical graph direct traversal returned an invalid source Node reference");
							}
							incrementBounded(state.task.stats.nodes_scanned,
											 "logical-plan direct traversal Node counter overflowed");
							if (labelsMatch) {
								sources.push_back({reference.entity_id, std::nullopt, 0U});
							}
							return true;
						});
				if (sources.empty()) {
					return true;
				}
				std::ranges::sort(sources, {}, &SourceRow::node_id);
				if (const auto duplicate = std::ranges::adjacent_find(sources, {}, &SourceRow::node_id);
					duplicate != sources.end()) {
					throw std::runtime_error(
							"logical graph direct traversal returned duplicate source Node references");
				}
			}

			std::vector<std::pair<std::string, PropertyValue>> sourceEqualities = sourceScan.getPropertyPredicates();
			sourceEqualities.insert(sourceEqualities.end(), parameterPredicates->begin(), parameterPredicates->end());
			std::vector<std::string> sourcePropertyKeys;
			sourcePropertyKeys.reserve(sourceEqualities.size() + sourceScan.getRangePredicates().size());
			bool impossibleSourcePredicate = false;
			const auto appendSourcePropertyKey = [&sourcePropertyKeys](const std::string &key) {
				if (key.empty()) {
					throw std::invalid_argument("logical-plan Property predicate has an empty key");
				}
				if (std::ranges::find(sourcePropertyKeys, key) == sourcePropertyKeys.end()) {
					sourcePropertyKeys.push_back(key);
				}
			};
			for (const auto &[key, expected]: sourceEqualities) {
				appendSourcePropertyKey(key);
				impossibleSourcePredicate = impossibleSourcePredicate || isNull(expected);
			}
			for (const logical::RangePredicate &range: sourceScan.getRangePredicates()) {
				appendSourcePropertyKey(range.key);
			}
			if (const auto &composite = sourceScan.getCompositeEquality(); composite.has_value()) {
				if (composite->keys.empty() || composite->keys.size() != composite->values.size()) {
					throw std::invalid_argument("logical-plan Node composite predicate has an invalid shape");
				}
				for (size_t index = 0U; index < composite->keys.size(); ++index) {
					appendSourcePropertyKey(composite->keys[index]);
					impossibleSourcePredicate = impossibleSourcePredicate || isNull(composite->values[index]);
				}
			}
			if (impossibleSourcePredicate) {
				return true;
			}
			if (!sourcePropertyKeys.empty()) {
				std::vector<uint64_t> sourceIds;
				sourceIds.reserve(sources.size());
				for (const SourceRow &source: sources) {
					sourceIds.push_back(source.node_id);
				}
				struct SourcePropertyColumn {
					std::string key;
					std::vector<PropertyValue> values;
				};
				std::vector<SourcePropertyColumn> sourceColumns;
				sourceColumns.reserve(sourcePropertyKeys.size());
				std::vector<std::string_view> sourcePropertyKeyViews;
				sourcePropertyKeyViews.reserve(sourcePropertyKeys.size());
				for (const std::string &key: sourcePropertyKeys) {
					sourcePropertyKeyViews.push_back(key);
				}
				MaterializedByteReservation sourcePropertyBytes(&state.shared.materialized_budget);
				const size_t sourceFrames = checkedSizeAdd(
						checkedSizeMul(sourcePropertyKeyViews.size(), sizeof(std::vector<PropertyValue>),
									   "logical-plan source Property frame size overflowed"),
						checkedSizeMul(checkedSizeMul(sourcePropertyKeyViews.size(), sourceIds.size(),
													  "logical-plan source Property frame size overflowed"),
									   sizeof(PropertyValue), "logical-plan source Property frame size overflowed"),
						"logical-plan source Property frame size overflowed");
				const size_t sourceIdBytes = checkedSizeMul(sourceIds.capacity(), sizeof(uint64_t),
															"logical-plan source Property ID size overflowed");
				sourcePropertyBytes.set(checkedSizeAdd(sourceFrames, sourceIdBytes,
													   "logical-plan source Property batch size overflowed"));
				const std::function<void(size_t)> admitSourceProperty = [&](size_t bytes) {
					checkGuard(state);
					sourcePropertyBytes.set(checkedSizeAdd(sourcePropertyBytes.bytes(), bytes,
														   "logical-plan source Property payload size overflowed"));
				};
				const bool supportsSourceAdmission = state.shared.snapshot.supportsPropertyReadAdmission();
				std::vector<std::vector<PropertyValue>> sourceValues =
						supportsSourceAdmission
								? state.shared.snapshot.readNodePropertyColumnsSortedWithAdmission(
										  sourceIds, sourcePropertyKeyViews, admitSourceProperty)
								: state.shared.snapshot.readNodePropertyColumnsSorted(sourceIds, sourcePropertyKeyViews);
				if (sourceValues.size() != sourcePropertyKeys.size()) {
					throw std::runtime_error("logical graph backend changed source Node Property column cardinality");
				}
				for (size_t column = 0U; column < sourcePropertyKeys.size(); ++column) {
					if (sourceValues[column].size() != sources.size()) {
						throw std::runtime_error(
								"logical graph backend changed sorted Node Property result cardinality");
					}
				}
				const size_t retainedSourceBytes = propertyColumnBytes(
						sourceValues, sourcePropertyKeyViews, "logical-plan source Property batch size overflowed", state.shared.context);
				const size_t retainedSourceBatchBytes =
						checkedSizeAdd(sourceIdBytes, std::max(sourceFrames, retainedSourceBytes),
									   "logical-plan source Property batch size overflowed");
				if (sourcePropertyBytes.wouldExceed(retainedSourceBatchBytes)) {
					throw std::runtime_error("shared materialization exceeded its byte bound");
				}
				sourcePropertyBytes.set(retainedSourceBatchBytes);
				for (size_t column = 0U; column < sourcePropertyKeys.size(); ++column) {
					sourceColumns.push_back({sourcePropertyKeys[column], std::move(sourceValues[column])});
				}
				const auto sourceValue = [&sourceColumns](const size_t sourceIndex,
														  const std::string &key) -> const PropertyValue & {
					const auto found = std::ranges::find(sourceColumns, key, &SourcePropertyColumn::key);
					if (found == sourceColumns.end()) { // ZYX_COV_EXCL_LINE: keys are collected before column reads.
						throw std::logic_error("logical-plan source Property column is missing its requested key");
					}
					return found->values[sourceIndex];
				};
				std::vector<SourceRow> selectedSources;
				selectedSources.reserve(sources.size());
				for (size_t sourceIndex = 0U; sourceIndex < sources.size(); ++sourceIndex) {
					checkGuard(state);
					bool matches = true;
					for (const auto &[key, expected]: sourceEqualities) {
						const PropertyValue &actual = sourceValue(sourceIndex, key);
						if (isNull(actual) || actual != expected) {
							matches = false;
							break;
						}
					}
					for (const logical::RangePredicate &range: sourceScan.getRangePredicates()) {
						if (!matches) {
							break;
						}
						const PropertyValue &actual = sourceValue(sourceIndex, range.key);
						if (isNull(actual) ||
							(!isNull(range.minValue) &&
							 (range.minInclusive ? actual < range.minValue : actual <= range.minValue)) ||
							(!isNull(range.maxValue) &&
							 (range.maxInclusive ? actual > range.maxValue : actual >= range.maxValue))) {
							matches = false;
						}
					}
					if (const auto &composite = sourceScan.getCompositeEquality(); matches && composite.has_value()) {
						for (size_t index = 0U; index < composite->keys.size(); ++index) {
							const PropertyValue &actual = sourceValue(sourceIndex, composite->keys[index]);
							if (isNull(actual) || actual != composite->values[index]) {
								matches = false;
								break;
							}
						}
					}
					if (!matches) {
						continue;
					}
					SourceRow selected = std::move(sources[sourceIndex]);
					LogicalPlanReadRow row;
					row.values.emplace(sourceScan.getVariable(),
									   storage::EntityRef{selected.node_id, false, 0U, 0U, 0U});
					for (SourcePropertyColumn &column: sourceColumns) {
						// The temporary sorted columns have no later consumer. Transfer
						// each surviving value into the row cache instead of keeping a
						// second owned copy alive until the whole source batch is done.
						row.property_cache[sourceScan.getVariable()].insert_or_assign(
								column.key, std::move(column.values[sourceIndex]));
					}
					selected.cached_row.emplace(std::move(row));
					selectedSources.push_back(std::move(selected));
				}
				sources = std::move(selectedSources);
				if (sources.empty()) {
					return true;
				}
			}

			std::vector<std::pair<std::string, PropertyValue>> edgePredicates;
			edgePredicates.reserve(traversal.getEdgeProperties().size() +
								   (hasAdditionalEdgeProperties ? additionalEdgeProperties->size() : 0U));
			for (const auto &[key, expected]: traversal.getEdgeProperties()) {
				if (key.empty()) {
					throw std::invalid_argument("logical-plan Edge Property predicate has an empty key");
				}
				edgePredicates.emplace_back(key, expected);
			}
			// Inline relationship properties originate in an unordered map; keep
			// their request deterministic.  Predicates extracted from an outer
			// expression are appended below in expression order.
			std::ranges::sort(edgePredicates, {}, &std::pair<std::string, PropertyValue>::first);
			if (hasAdditionalEdgeProperties) {
				for (const auto &[key, expected]: *additionalEdgeProperties) {
					if (key.empty()) {
						throw std::invalid_argument("logical-plan Edge Property predicate has an empty key");
					}
					const auto existing =
							std::ranges::find(edgePredicates, key, &std::pair<std::string, PropertyValue>::first);
					if (existing != edgePredicates.end()) {
						if (existing->second != expected) {
							// A contradictory pushed hint can never match.  Retain the
							// generic path so the original filter owns its semantics.
							return std::nullopt;
						}
						continue;
					}
					edgePredicates.emplace_back(key, expected);
				}
			}
			std::vector<std::string_view> edgePropertyKeys;
			edgePropertyKeys.reserve(edgePredicates.size());
			for (const auto &[key, expected]: edgePredicates) {
				static_cast<void>(expected);
				edgePropertyKeys.push_back(key);
			}
			const std::vector<std::pair<std::string, PropertyValue>> &targetPredicates =
					traversal.getTargetProperties();
			validateTraversalTargetProperties(traversal);
			std::optional<uint64_t> relationshipTypeToken;
			if (!traversal.getEdgeType().empty() &&
				state.shared.snapshot.supportsNativeRelationshipTypeTokens(traversal.getEdgeType())) {
				relationshipTypeToken = state.shared.snapshot.findRelationshipTypeToken(traversal.getEdgeType());
				if (!relationshipTypeToken.has_value()) {
					return true;
				}
			}

			debug::ScopedPerfTimer timer("logical_plan.direct_edge_scan");
			debug::PerfTrace::addValue("logical_plan.direct_edge_scan", 1);
			bool continueVisitor = true;
			size_t edgeRecords = 0U;
			struct PendingEdge {
				storage::LogicalGraphEdgeTopology topology;
				// Preserve the snapshot-local relationship type token when the input
				// came from a native reference cursor.  A topology projection may not
				// carry this token, so zero means that the type identity is unknown.
				uint64_t type_token = 0U;
				size_t source_ordinal = 0U;
			};
			// Property roots are owner-keyed and benefit from a sorted multi-lookup.
			// Keep the batch bounded and retain the record-at-a-time path for
			// topology-only traversals, whose callers may stop after one result.
			// Property-bearing traversals use bounded batches; topology-only rows
			// remain one-at-a-time so a visitor can stop before the next physical
			// Edge is fetched (an observable early-termination contract).
			const size_t desiredEdgeBatchCapacity =
					edgePredicates.empty() && targetPredicates.empty()
							? size_t{1U}
							: std::min({DEFAULT_PROPERTY_BATCH_ROWS, state.shared.bounds.maximum_edge_records,
										state.shared.bounds.maximum_rows});
			const size_t edgeBatchCapacity = std::min(desiredEdgeBatchCapacity,
					std::max<size_t>(1U, state.shared.materialized_budget.available() / (8U * sizeof(PendingEdge))));
			detail::BudgetMemoryResource pendingMemory(state.shared.materialized_budget);
			std::pmr::vector<PendingEdge> pending{&pendingMemory};
			pending.reserve(edgeBatchCapacity);
			const auto flushPending = [&]() {
				if (pending.empty()) {
					return continueVisitor;
				}
				// Property-bearing global traversals already admit a bounded batch.
				// Validate all newly observed source endpoints in that same batch so
				// one Entity Directory descent is not paid per Edge.  Topology-only
				// traversal keeps edgeBatchCapacity == 1 and therefore retains its
				// strict visitor early-termination contract.
				if (globalEdgePath && state.shared.snapshot.supportsNodeExistenceProbes()) {
					std::vector<uint64_t> sourceIds;
					sourceIds.reserve(pending.size());
					for (const PendingEdge &candidate: pending) {
						if (candidate.source_ordinal >= sources.size()) {
							throw std::runtime_error("logical graph direct traversal lost a source ordinal");
						}
						sourceIds.push_back(sources[candidate.source_ordinal].node_id);
					}
					std::ranges::sort(sourceIds);
					sourceIds.erase(std::unique(sourceIds.begin(), sourceIds.end()), sourceIds.end());
					debug::PerfTrace::addValue("logical_plan.direct_source_existence_batch_requests", 1);
					debug::PerfTrace::addValue("logical_plan.direct_source_existence_batch_ids",
											   static_cast<int64_t>(sourceIds.size()));
					const std::vector<bool> present = state.shared.snapshot.containsNodesSorted(sourceIds);
					if (present.size() != sourceIds.size()) {
						throw std::runtime_error("logical graph backend changed direct traversal source cardinality");
					}
					if (std::ranges::any_of(present, [](const bool value) { return !value; })) {
						throw std::runtime_error(
								"logical graph backend returned an Edge topology with a missing source Node");
					}
				}
				detail::BudgetMemoryResource selectionMemory(state.shared.materialized_budget);
				SelectionVector selection(pending.size(), &selectionMemory);
				MaterializedByteReservation selectedPropertyBytes(&state.shared.materialized_budget);
				MaterializedByteReservation reorderedPropertyBytes(&state.shared.materialized_budget);
				std::vector<std::vector<PropertyValue>> selectedProperties;
				if (!edgePredicates.empty()) {
					MaterializedByteReservation orderingBytes(&state.shared.materialized_budget);
					orderingBytes.set(checkedSizeMul(pending.size(), 2U * sizeof(size_t) + sizeof(uint64_t),
						"logical-plan direct traversal ordering size overflowed"));
					std::vector<size_t> sortedOrdinals(pending.size());
					for (size_t index = 0U; index < sortedOrdinals.size(); ++index) {
						sortedOrdinals[index] = index;
					}
					std::ranges::sort(sortedOrdinals, [&pending](const size_t left, const size_t right) {
						return pending[left].topology.edge_id < pending[right].topology.edge_id;
					});
					std::vector<uint64_t> edgeIds;
					edgeIds.reserve(pending.size());
					std::vector<size_t> uniqueOrdinals(pending.size());
					orderingBytes.set(checkedSizeAdd(
						checkedSizeMul(sortedOrdinals.capacity() + uniqueOrdinals.capacity(), sizeof(size_t),
							"logical-plan direct traversal ordering capacity overflowed"),
						checkedSizeMul(edgeIds.capacity(), sizeof(uint64_t),
							"logical-plan direct traversal Edge ID capacity overflowed"),
						"logical-plan direct traversal ordering capacity overflowed"));
					for (const size_t ordinal: sortedOrdinals) {
						const uint64_t edgeId = pending[ordinal].topology.edge_id;
						if (edgeIds.empty() || edgeId != edgeIds.back()) {
							edgeIds.push_back(edgeId);
						}
						uniqueOrdinals[ordinal] = edgeIds.size() - 1U;
					}
					if (std::ranges::any_of(edgePredicates,
											[](const auto &predicate) { return isNull(predicate.second); })) {
						selection.rejectAll();
					} else {
						size_t keyBytes = 0U;
						for (const std::string_view key: edgePropertyKeys) {
							keyBytes =
									checkedSizeAdd(keyBytes, key.size(),
												   "logical-plan direct traversal Edge Property key size overflowed");
						}
						if (selectedPropertyBytes.wouldExceed(keyBytes)) {
							throw std::runtime_error(
									"logical-plan direct traversal Edge Property batch exceeded its byte bound");
						}
						selectedPropertyBytes.set(keyBytes);
						const std::function<void(size_t)> admission = [&](size_t bytes) {
							checkGuard(state);
							const size_t next =
									checkedSizeAdd(selectedPropertyBytes.bytes(), bytes,
												   "logical-plan direct traversal Edge Property batch size overflowed");
							if (selectedPropertyBytes.wouldExceed(next)) {
								throw std::runtime_error("logical-plan direct traversal Edge Property batch "
														 "exceeded its byte bound");
							}
							selectedPropertyBytes.set(next);
						};
						auto sortedColumns =
								state.shared.snapshot.supportsPropertyReadAdmission()
										? state.shared.snapshot.readEdgePropertyColumnsSortedWithAdmission(
												  edgeIds, edgePropertyKeys, admission)
										: state.shared.snapshot.readEdgePropertyColumnsSorted(edgeIds, edgePropertyKeys);
						const size_t selectedEdgeBytes = propertyColumnBytes(
								sortedColumns, edgePropertyKeys,
								"logical-plan direct traversal Edge Property batch size overflowed", state.shared.context);
						if (selectedPropertyBytes.wouldExceed(selectedEdgeBytes)) {
							throw std::runtime_error(
									"logical-plan direct traversal Edge Property batch exceeded its byte bound");
						}
						selectedPropertyBytes.set(selectedEdgeBytes);
						if (sortedColumns.size() != edgePredicates.size()) {
							throw std::runtime_error("logical graph backend changed Edge Property column cardinality");
						}
						reorderedPropertyBytes.set(checkedSizeMul(edgePredicates.size(), sizeof(std::vector<PropertyValue>),
							"logical-plan reordered Property column size overflowed"));
						selectedProperties.resize(edgePredicates.size());
						reorderedPropertyBytes.set(checkedSizeMul(selectedProperties.capacity(), sizeof(std::vector<PropertyValue>),
							"logical-plan reordered Property column size overflowed"));
						for (size_t propertyIndex = 0U; propertyIndex < edgePredicates.size(); ++propertyIndex) {
							const std::vector<PropertyValue> &sortedValues = sortedColumns[propertyIndex];
							if (sortedValues.size() != edgeIds.size()) {
								throw std::runtime_error(
										"logical graph backend changed sorted Edge Property result cardinality");
							}
							const size_t previousBytes = reorderedPropertyBytes.bytes();
							reorderedPropertyBytes.set(checkedSizeAdd(previousBytes,
								checkedSizeMul(pending.size(), sizeof(PropertyValue), "logical-plan reordered Property slot size overflowed"),
								"logical-plan reordered Property size overflowed"));
							std::vector<PropertyValue> values(pending.size());
							reorderedPropertyBytes.set(checkedSizeAdd(previousBytes,
								checkedSizeMul(values.capacity(), sizeof(PropertyValue), "logical-plan reordered Property slot size overflowed"),
								"logical-plan reordered Property size overflowed"));
							for (const size_t ordinal: sortedOrdinals) {
								const auto checkpoint = [&] { checkGuardNow(state); };
								const PropertyValue &source = sortedValues[uniqueOrdinals[ordinal]];
								const size_t beforeCopy = reorderedPropertyBytes.bytes();
								reorderedPropertyBytes.set(checkedSizeAdd(beforeCopy,
									detail::retainedPropertyValueBytes(source, checkpoint), "logical-plan reordered Property payload size overflowed"));
								values[ordinal] = detail::copyPropertyValueGuarded(source, checkpoint);
								reorderedPropertyBytes.set(checkedSizeAdd(beforeCopy,
									detail::retainedPropertyValueBytes(values[ordinal], checkpoint), "logical-plan reordered Property payload size overflowed"));
							}
							selection.forEachSelected([&](const size_t index) {
								if (isNull(values[index]) || values[index] != edgePredicates[propertyIndex].second) {
									selection.reject(index);
								}
							});
							selectedProperties[propertyIndex] = std::move(values);
						}
					}
				}

				// Sorted source columns have been destroyed; reordered copies retain their own reservation.
				selectedPropertyBytes.reset();

				struct TargetCandidate {
					LogicalPlanReadRow row;
					uint64_t target_id = 0U;
					size_t property_ordinal = 0U;
				};
				// Resolve labels before Property columns so a label-rejected target does
				// not consume a selected-Property read. Binding is checked first because
				// a repeated variable can reject a non-self traversal before any target
				// access is observable.
				MaterializedByteReservation targetCopiedPropertyBytes(&state.shared.materialized_budget);
				std::vector<TargetCandidate> targets;
				targets.reserve(pending.size());
				selection.forEachSelected([&](const size_t index) {
					checkGuard(state);
					const PendingEdge &candidate = pending[index];
					SourceRow &source = sources[candidate.source_ordinal];
					const uint64_t targetId = traversal.getDirection() == "out" ? candidate.topology.target_node_id
																				: candidate.topology.source_node_id;
					LogicalPlanReadRow expanded;
					if (source.cached_row.has_value()) {
						const size_t previous = targetCopiedPropertyBytes.bytes();
						targetCopiedPropertyBytes.set(checkedSizeAdd(previous, logicalReadRowBytes(*source.cached_row, state.shared.context),
							"logical-plan expanded row size overflowed"));
						expanded = detail::copyLogicalReadRowGuarded(*source.cached_row, [&] { checkGuardNow(state); });
						targetCopiedPropertyBytes.set(checkedSizeAdd(previous, logicalReadRowBytes(expanded, state.shared.context),
							"logical-plan expanded row size overflowed"));
					}
					if (!source.cached_row.has_value()) {
						expanded.values.emplace(sourceScan.getVariable(),
												storage::EntityRef{source.node_id, false, 0U, 0U, 0U});
					}
					const storage::EntityRef edgeRef{candidate.topology.edge_id, true,
													 candidate.topology.source_node_id,
													 candidate.topology.target_node_id, candidate.type_token};
					const storage::EntityRef targetRef{targetId, false, 0U, 0U, 0U};
					if (!bindValue(expanded, traversal.getEdgeVar(), edgeRef) ||
						!bindValue(expanded, traversal.getTargetVar(), targetRef)) {
						return;
					}
					for (size_t propertyIndex = 0U; propertyIndex < edgePredicates.size(); ++propertyIndex) {
						expanded.property_cache[traversal.getEdgeVar()].insert_or_assign(
								edgePredicates[propertyIndex].first, copyReservedPropertyValue(
							selectedProperties[propertyIndex][index], state, targetCopiedPropertyBytes));
					}
					targets.push_back({std::move(expanded), targetId, 0U});
				});

				// Target labels are a predicate on the whole pending batch, not on the
				// Edge callback. Resolve the fixed predicate once and retain the original
				// Edge order while applying the sorted result.
				if (!targets.empty() && !traversal.getTargetLabels().empty()) {
					std::vector<uint64_t> targetIds;
					targetIds.reserve(targets.size());
					for (const TargetCandidate &target: targets) {
						if (targetIds.empty() || targetIds.back() != target.target_id) {
							targetIds.push_back(target.target_id);
						}
					}
					detail::sortUniqueIdsGuarded(targetIds, [&] {
						if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
					});
					const std::vector<bool> labelsMatch =
							state.shared.snapshot.readNodeLabelMatchesSorted(targetIds, traversal.getTargetLabels());
					if (labelsMatch.size() != targetIds.size()) {
						throw std::runtime_error("logical graph backend changed target label-match cardinality");
					}
					std::vector<TargetCandidate> labelMatched;
					labelMatched.reserve(targets.size());
					for (TargetCandidate &target: targets) {
						const auto position = std::ranges::lower_bound(targetIds, target.target_id);
						const size_t ordinal = static_cast<size_t>(position - targetIds.begin());
						if (position != targetIds.end() && labelsMatch[ordinal]) {
							labelMatched.push_back(std::move(target));
						}
					}
					targets = std::move(labelMatched);
				}

				if (!targets.empty() && !targetPredicates.empty()) {
					std::vector<std::string> targetPropertyKeys;
					targetPropertyKeys.reserve(targetPredicates.size());
					bool impossibleTargetPredicate = false;
					for (const auto &[key, expected]: targetPredicates) {
						if (key.empty()) {
							throw std::invalid_argument("logical-plan Node Property predicate has an empty key");
						}
						if (std::ranges::find(targetPropertyKeys, key) == targetPropertyKeys.end()) {
							targetPropertyKeys.push_back(key);
						}
						impossibleTargetPredicate = impossibleTargetPredicate || isNull(expected);
					}
					if (impossibleTargetPredicate) {
						targets.clear();
					} else {
						std::vector<size_t> sortedOrdinals(targets.size());
						for (size_t index = 0U; index < sortedOrdinals.size(); ++index) {
							sortedOrdinals[index] = index;
						}
						std::ranges::sort(sortedOrdinals, [&targets](const size_t left, const size_t right) {
							return targets[left].target_id < targets[right].target_id;
						});
						std::vector<uint64_t> targetIds;
						targetIds.reserve(targets.size());
						for (const size_t ordinal: sortedOrdinals) {
							if (targetIds.empty() || targets[ordinal].target_id != targetIds.back()) {
								targetIds.push_back(targets[ordinal].target_id);
							}
							targets[ordinal].property_ordinal = targetIds.size() - 1U;
						}
						struct TargetPropertyColumn {
							std::string key;
							std::vector<PropertyValue> values;
						};
						std::vector<std::string_view> targetPropertyKeyViews;
						targetPropertyKeyViews.reserve(targetPropertyKeys.size());
						for (const std::string &key: targetPropertyKeys) {
							targetPropertyKeyViews.push_back(key);
						}
						MaterializedByteReservation targetPropertyBytes(&state.shared.materialized_budget);
						size_t targetKeyBytes = 0U;
						for (const std::string_view key: targetPropertyKeyViews) {
							targetKeyBytes = checkedSizeAdd(targetKeyBytes, key.size(),
															"logical-plan direct target Property key size overflowed");
						}
						if (targetPropertyBytes.wouldExceed(targetKeyBytes)) {
							throw std::runtime_error(
									"logical-plan direct target Property batch exceeded its byte bound");
						}
						targetPropertyBytes.set(targetKeyBytes);
						const std::function<void(size_t)> targetAdmission = [&](size_t bytes) {
							checkGuard(state);
							const size_t next =
									checkedSizeAdd(targetPropertyBytes.bytes(), bytes,
												   "logical-plan direct target Property batch size overflowed");
							if (targetPropertyBytes.wouldExceed(next)) {
								throw std::runtime_error(
										"logical-plan direct target Property batch exceeded its byte bound");
							}
							targetPropertyBytes.set(next);
						};
						auto targetValues = state.shared.snapshot.supportsPropertyReadAdmission()
													? state.shared.snapshot.readNodePropertyColumnsSortedWithAdmission(
															  targetIds, targetPropertyKeyViews, targetAdmission)
													: state.shared.snapshot.readNodePropertyColumnsSorted(
															  targetIds, targetPropertyKeyViews);
						const size_t selectedTargetPayloadBytes =
								propertyColumnBytes(targetValues, targetPropertyKeyViews,
													"logical-plan direct target Property batch size overflowed", state.shared.context);
						if (targetPropertyBytes.wouldExceed(selectedTargetPayloadBytes)) {
							throw std::runtime_error(
									"logical-plan direct target Property batch exceeded its byte bound");
						}
						targetPropertyBytes.set(selectedTargetPayloadBytes);
						if (targetValues.size() != targetPropertyKeys.size()) {
							throw std::runtime_error(
									"logical graph backend changed target Node Property column cardinality");
						}
						std::vector<TargetPropertyColumn> targetColumns;
						targetColumns.reserve(targetPropertyKeys.size());
						for (size_t column = 0U; column < targetPropertyKeys.size(); ++column) {
							if (targetValues[column].size() != targetIds.size()) {
								throw std::runtime_error(
										"logical graph backend changed sorted target Node Property result cardinality");
							}
							targetColumns.push_back({targetPropertyKeys[column], std::move(targetValues[column])});
						}
						for (TargetCandidate &target: targets) {
							checkGuard(state);
							bool matchesTarget = true;
							for (const auto &[key, expected]: targetPredicates) {
								const auto column = std::ranges::find(targetColumns, key, &TargetPropertyColumn::key);
								if (column ==
									targetColumns.end()) { // ZYX_COV_EXCL_LINE: keys are collected before column reads.
									throw std::logic_error(
											"logical-plan target Property column is missing its requested key");
								}
								const PropertyValue &actual = column->values[target.property_ordinal];
								if (isNull(actual) || actual != expected) {
									matchesTarget = false;
									break;
								}
							}
							if (!matchesTarget) {
								continue;
							}
							for (const TargetPropertyColumn &column: targetColumns) {
								target.row.property_cache[traversal.getTargetVar()].insert_or_assign(
										column.key, column.values[target.property_ordinal]);
							}
							if (continueVisitor) {
								continueVisitor = visitor(std::move(target.row));
							}
						}
					}
				} else {
					for (TargetCandidate &target: targets) {
						checkGuard(state);
						if (!continueVisitor) {
							break;
						}
						continueVisitor = visitor(std::move(target.row));
					}
				}
				pending.clear();
				return continueVisitor;
			};
			const auto consumeTopology = [&](const storage::LogicalGraphEdgeTopology &edge,
											 const uint64_t typeToken = 0U) {
				checkGuard(state);
				validateEdge(edge);
				if (edgeRecords == state.shared.bounds.maximum_edge_records) {
					throw std::runtime_error("logical-plan direct traversal exceeded its global Edge bound");
				}
				incrementBounded(edgeRecords, "logical-plan direct traversal Edge counter overflowed");
				incrementBounded(state.task.stats.edges_scanned, "logical-plan read runtime Edge counter overflowed");

				const uint64_t sourceId = traversal.getDirection() == "out" ? edge.source_node_id : edge.target_node_id;
				size_t sourceOrdinal = 0U;
				if (globalEdgePath) {
					if (sourceId == 0U) {
						throw std::runtime_error("logical graph direct traversal returned a zero source Node ID");
					}
					const auto existing = sourceOrdinals.find(sourceId);
					if (existing == sourceOrdinals.end()) {
						if (sources.size() == state.shared.bounds.maximum_node_records) {
							throw std::runtime_error(
									"logical-plan direct traversal exceeded its source Node record bound");
						}
						// Property-bearing batches validate source liveness in flushPending().
						// Keep the scalar check only for topology-only batches, whose capacity is
						// intentionally one to preserve strict early termination.
						if (state.shared.snapshot.supportsNodeExistenceProbes() && edgeBatchCapacity == 1U &&
							!state.shared.snapshot.containsNode(sourceId)) {
							return true;
						}
						sourceOrdinal = sources.size();
						sourceOrdinals.emplace(sourceId, sourceOrdinal);
						sources.push_back({sourceId, std::nullopt, 0U});
					} else {
						sourceOrdinal = existing->second;
					}
				} else {
					auto source = std::ranges::lower_bound(sources, sourceId, {}, &SourceRow::node_id);
					if (source == sources.end() || source->node_id != sourceId ||
						source->edges_seen == state.shared.bounds.maximum_edges_per_node) {
						return true;
					}
					incrementBounded(source->edges_seen,
									 "logical-plan direct traversal per-Node Edge counter overflowed");
					sourceOrdinal = static_cast<size_t>(source - sources.begin());
				}
				if (globalEdgePath) {
					SourceRow &source = sources[sourceOrdinal];
					if (source.edges_seen == state.shared.bounds.maximum_edges_per_node) {
						return true;
					}
					incrementBounded(source.edges_seen,
									 "logical-plan direct traversal per-Node Edge counter overflowed");
				}
				if (!relationshipTypeToken.has_value() && !traversal.getEdgeType().empty() &&
					edge.type != traversal.getEdgeType()) {
					return true;
				}
				pending.push_back({{edge.edge_id, edge.source_node_id, edge.target_node_id, {}}, typeToken, sourceOrdinal});
				return pending.size() != edgeBatchCapacity || flushPending();
			};
			const size_t edgeVisitBound = checkedSizeAdd(state.shared.bounds.maximum_edge_records, 1U,
														 "logical-plan direct traversal Edge bound overflowed");
			if (traversal.getEdgeType().empty() || relationshipTypeToken.has_value()) {
				state.shared.snapshot.visitEdgeRefsForTypePredicate(edgeVisitBound, relationshipTypeToken, [&consumeTopology, &relationshipTypeToken](
																	 const storage::EntityRef &reference) {
					validateEdgeRef(reference, "logical-plan direct traversal received an invalid or Node reference");
					if (relationshipTypeToken.has_value() && reference.type_token != *relationshipTypeToken) {
						return true;
					}
					return consumeTopology(
							{reference.entity_id, reference.source_node_id, reference.target_node_id, {}},
							reference.type_token);
				});
			} else {
				state.shared.snapshot.visitEdgeTopologies(edgeVisitBound, consumeTopology);
			}
			if (continueVisitor) {
				static_cast<void>(flushPending());
			}
			const int64_t tracedEdgeRecords = edgeRecords > static_cast<size_t>(std::numeric_limits<int64_t>::max())
													  ? std::numeric_limits<int64_t>::max()
													  : static_cast<int64_t>(edgeRecords);
			debug::PerfTrace::addValue("logical_plan.direct_edge_records", tracedEdgeRecords);
			return continueVisitor;
		}

