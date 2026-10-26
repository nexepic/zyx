
        [[nodiscard]] QueryResult makeAdmittedProcedureResult(const QueryContext &context) {
            if (context.resource_account) return QueryResult(detail::ResourceLease(context.resource_account));
            return QueryResult{};
        }
        void setOwnedProcedureColumns(QueryResult &result, const logical::LogicalOperator &root) {
            const auto columns = root.getOwnedOutputVariables();
            result.setColumns(static_cast<const std::vector<std::string> &>(columns));
        }

        [[nodiscard]] PropertyValue makeProcedurePropertyValue() { return PropertyValue(); }
        template<class T> [[nodiscard]] PropertyValue makeProcedurePropertyValue(T &&source) {
            if (detail::ProcedurePayloadAdmissionScope::active())
                detail::ProcedurePayloadAdmissionScope::admit(detail::procedurePayloadHeapUpper<true>(source));
            return PropertyValue(std::forward<T>(source));
        }
        using ProcedureDistinctRows = std::unordered_set<utils::OwnedStdString,
            std::hash<std::string_view>, std::equal_to<>, utils::OwnedPmrAllocator<utils::OwnedStdString>>;
        using ProcedureOrderedDistinctRows = std::set<utils::OwnedStdString, std::less<>,
            utils::OwnedPmrAllocator<utils::OwnedStdString>>;

		using PropertyMap = std::unordered_map<std::string, PropertyValue>;
		[[nodiscard]] size_t checkedProcedureSizeAdd(size_t left, size_t right);
		[[nodiscard]] size_t checkedProcedureSizeMul(size_t left, size_t right);
		struct CachedNodeProperties {
			PropertyMap values;
			bool complete = false;
		};
		using NodePropertyMaps = std::unordered_map<uint64_t, CachedNodeProperties>;
		[[nodiscard]] std::optional<uint64_t> procedureNodeIdentity(const LogicalPlanReadValue &value) {
			if (const auto *node = std::get_if<storage::LogicalGraphNode>(&value)) {
				return node->node_id;
			}
			if (const auto *reference = std::get_if<storage::EntityRef>(&value);
					reference != nullptr && !reference->is_edge && reference->entity_id != 0U) {
				return reference->entity_id;
			}
			return std::nullopt;
		}
		// An options map cannot contain a key more than once. Keeping the accepted
		// names in one place makes the resource bound grow with the public contract.
		constexpr std::array<std::string_view, 5U> LEIDEN_OPTION_NAMES = {"maxIterations", "maxLevels", "resolution",
																			  "refinementThreshold", "concurrency"};

		// Complete materialization is still required at procedure result and
		// algorithm boundaries, but it must enter through the same explicit shape
		// contract as the regular logical read runtime.  This keeps native readers
		// from accidentally treating a procedure point read as an unplanned shape.
		[[nodiscard]] storage::EntityReadShape completeNodeReadShape() {
			return {.include_labels = true, .include_type = false, .all_properties = true};
		}

		[[nodiscard]] const logical::LogicalOperator *requireOnlyChild(const logical::LogicalOperator &op) {
			const std::vector<logical::LogicalOperator *> children = op.getChildren();
			if (children.size() != 1U || children.front() == nullptr) {
				throw std::logic_error("logical procedure projection requires exactly one child");
			}
			return children.front();
		}

		[[nodiscard]] PropertyMap takeNodeProperties(storage::LogicalGraphNode &node,
				size_t maximumEntries, const QueryContext &context) {
			context.checkGuard();
			if (node.properties.size() > maximumEntries) throw std::runtime_error("logical Vector procedure Node Properties exceeded its caller-supplied bound");
			{
				std::unordered_set<std::string_view> keys;
				for (const auto &entry: node.properties) {
					context.checkGuard();
					if (entry.key.empty() || !keys.insert(entry.key).second) throw std::runtime_error("logical Vector procedure read an invalid Node Property entry set");
				}
			}
            detail::ProcedurePayloadAdmissionScope::admit(detail::procedureEmptyMapHeapUpper<PropertyMap>());
			PropertyMap properties;
			detail::procedureMapReserve(properties, node.properties.size());
			for (auto &entry: node.properties) {
				context.checkGuard();
				detail::procedureMapEmplace(properties, std::move(entry.key), std::move(entry.value));
			}
			std::vector<storage::LogicalGraphPropertyEntry>().swap(node.properties);
			return properties;
		}

		void cacheResolvedNodeProperties(const LogicalPlanReadRow &row, const LogicalPlanReadRow &output,
				NodePropertyMaps &nodeProperties) {
			for (const auto &[alias, properties]: row.property_cache) {
				const auto binding = row.values.find(alias);
				if (binding ==
					row.values
							.end()) { // ZYX_COV_EXCL_LINE: evaluator writes cache entries only for aliases in this row.
					throw std::logic_error("logical Vector procedure Property cache has no matching binding");
				}
				const auto nodeId = procedureNodeIdentity(binding->second);
				if (!nodeId.has_value()) { // ZYX_COV_EXCL_LINE: only Node Property evaluation populates this cache.
					continue;
				}
				if (!std::ranges::any_of(output.values, [&](const auto &binding) {
					return procedureNodeIdentity(binding.second) == nodeId;
				})) continue;
				const auto [cached, inserted] = detail::procedureMapTryEmplace(nodeProperties, *nodeId);
				static_cast<void>(inserted);
				if (row.complete_entity_property_cache.contains(alias)) {
					// Selected reads represent missing fields with NULL. A complete map
					// must drop those sentinels instead of promoting them to real keys.
					for (const auto &[key, value]: cached->second.values) {
						const auto found = properties.find(key);
						if (found == properties.end() ? value != makeProcedurePropertyValue() : found->second != value) {
							throw std::runtime_error("logical Vector procedure observed inconsistent complete Node Properties");
						}
					}
					detail::ProcedurePayloadAdmissionScope::admit(detail::procedurePayloadHeapUpper<true>(properties));
                    cached->second = CachedNodeProperties{properties, true};
					continue;
				}
				for (const auto &[key, value]: properties) {
					if (cached->second.complete) {
						const auto found = cached->second.values.find(key);
						if (found == cached->second.values.end() ? value != makeProcedurePropertyValue() : found->second != value) {
							throw std::runtime_error("logical Vector procedure observed inconsistent selected Node Properties");
						}
						continue;
					}
					const auto [existing, valueInserted] = detail::procedureMapEmplace(cached->second.values, key, value);
					if (!valueInserted && existing->second != value) { // ZYX_COV_EXCL_LINE: one snapshot cannot diverge.
						throw std::runtime_error("logical Vector procedure observed inconsistent Node Properties");
					}
				}
			}
		}

		void attachCachedNodeProperties(LogicalPlanReadRow &row, const NodePropertyMaps &nodeProperties) {
			for (const auto &[alias, value]: row.values) {
				const auto nodeId = procedureNodeIdentity(value);
				if (!nodeId.has_value()) {
					continue;
				}
				if (const auto properties = nodeProperties.find(*nodeId); properties != nodeProperties.end()) {
					detail::procedureMapInsertOrAssign(row.property_cache, alias, properties->second.values);
					if (properties->second.complete) {
						detail::procedureSetInsert(row.complete_entity_property_cache, alias);
					}
				}
			}
		}

		void validateNode(const storage::LogicalGraphNode &node) {
			if (node.node_id == 0U) { // ZYX_COV_EXCL_LINE: both vector-hit paths reject zero IDs before Node hydration.
				throw std::runtime_error("logical Vector procedure backend returned an invalid Node");
			}
			std::unordered_set<std::string_view> labels;
			detail::procedureMapReserve(labels, node.labels.size());
			for (const std::string &label: node.labels) {
				if (label.empty() || !labels.insert(label).second) {
					throw std::runtime_error("logical Vector procedure backend returned an invalid Node label set");
				}
			}
		}

		void validateEdge(const storage::LogicalGraphEdgeTopology &edge) {
			if (edge.edge_id == 0U || edge.source_node_id == 0U || edge.target_node_id == 0U) {
				throw std::runtime_error("logical procedure backend returned an invalid relationship topology");
			}
		}

		[[nodiscard]] utils::OwnedStdString requireIndexName(const PropertyValue &value) {
			if (value.getType() != PropertyType::STRING) {
				throw std::invalid_argument("db.index.vector.queryNodes requires a String index name");
			}
			const std::string &name = std::get<std::string>(value.getVariant());
			if (name.empty()) {
				throw std::invalid_argument("db.index.vector.queryNodes requires a nonempty index name");
			}
			return name;
		}

		[[nodiscard]] size_t requireTopK(const PropertyValue &value) {
			if (value.getType() != PropertyType::INTEGER) {
				throw std::invalid_argument("db.index.vector.queryNodes requires a nonnegative integer topK");
			}
			const int64_t topK = std::get<int64_t>(value.getVariant());
			if (topK < 0 || static_cast<uint64_t>(topK) > std::numeric_limits<size_t>::max()) {
				throw std::invalid_argument("db.index.vector.queryNodes topK is outside the supported range");
			}
			return static_cast<size_t>(topK);
		}

		[[nodiscard]] utils::OwnedStdVector<float> requireQueryVector(const PropertyValue &value,
				size_t maximumMaterializedBytes, const QueryContext &context) {
			if (value.getType() != PropertyType::LIST) {
				throw std::invalid_argument("db.index.vector.queryNodes queryVector argument must be a List of floats");
			}
			if (value.getList().empty()) {
				throw std::invalid_argument("db.index.vector.queryNodes requires a nonempty numeric query vector");
			}
			if (value.getList().size() > std::numeric_limits<size_t>::max() / sizeof(float)) {
				throw std::overflow_error("db.index.vector.queryNodes query vector size overflowed");
			}
			const size_t queryBytes = value.getList().size() * sizeof(float);
			if (queryBytes > maximumMaterializedBytes) {
				throw std::runtime_error("db.index.vector.queryNodes query vector exceeded its materialized byte bound");
			}
			context.checkGuardNow();
			utils::OwnedStdVector<float> query;
			query.reserve(value.getList().size());
			if (checkedProcedureSizeMul(query.capacity(), sizeof(float)) > maximumMaterializedBytes) {
				throw std::runtime_error("db.index.vector.queryNodes query vector exceeded its materialized byte bound");
			}
			size_t coordinateIndex = 0U;
			for (const PropertyValue &coordinate: value.getList()) {
				if ((coordinateIndex++ & 1023U) == 1023U) context.checkGuard();
				float converted = 0.0F;
				if (coordinate.getType() == PropertyType::DOUBLE) {
					const double source = std::get<double>(coordinate.getVariant());
					if (!std::isfinite(source) || source > std::numeric_limits<float>::max() ||
						source < -std::numeric_limits<float>::max()) {
						throw std::invalid_argument("db.index.vector.queryNodes query has a non-finite or "
													"float-unrepresentable coordinate");
					}
					converted = static_cast<float>(source);
				} else if (coordinate.getType() == PropertyType::INTEGER) {
					converted = static_cast<float>(std::get<int64_t>(coordinate.getVariant()));
				} else {
					throw std::invalid_argument("db.index.vector.queryNodes requires numeric query coordinates");
				}
				if (!std::isfinite(converted)) { // ZYX_COV_EXCL_LINE: finite, float-representable inputs cannot convert
												 // to infinity.
					throw std::invalid_argument("db.index.vector.queryNodes query has a non-finite coordinate");
				}
				query.push_back(converted);
			}
			return query;
		}

		[[nodiscard]] size_t retainedVectorRequestBytes(
				const storage::LogicalGraphVectorSearchRequest &request) {
			size_t bytes = 0U;
			// The request object and string's inline storage are part of the stack
			// frame; charge only dynamically retained payloads.
			bytes = checkedProcedureSizeAdd(bytes, detail::dynamicStringBufferBytes(request.index_name));
			return checkedProcedureSizeAdd(bytes,
					checkedProcedureSizeMul(request.query_vector.capacity(), sizeof(float)));
		}

		[[nodiscard]] uint64_t requireShortestPathNodeId(const PropertyValue &value, const char *argumentName) {
			if (value.getType() == PropertyType::INTEGER) {
				const int64_t parsed = std::get<int64_t>(value.getVariant());
				if (parsed > 0) {
					return static_cast<uint64_t>(parsed);
				}
			} else if (value.getType() == PropertyType::STRING) {
				const std::string &text = std::get<std::string>(value.getVariant());
				uint64_t parsed = 0U;
				const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
				if (error == std::errc{} && end == text.data() + text.size() && parsed != 0U) {
					return parsed;
				}
			}
			throw std::invalid_argument(std::string("algo.shortestPath requires a positive integer ") + argumentName);
		}

		[[nodiscard]] size_t requireShortestPathDepth(const PropertyValue &value) {
			int64_t parsed = 0;
			if (value.getType() == PropertyType::INTEGER) {
				parsed = std::get<int64_t>(value.getVariant());
			} else if (value.getType() == PropertyType::STRING) {
				const std::string &text = std::get<std::string>(value.getVariant());
				const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
				if (error != std::errc{} || end != text.data() + text.size()) {
					parsed = 0;
				}
			}
			if (parsed <= 0) {
				throw std::invalid_argument("algo.shortestPath requires a positive integer maxDepth");
			}
			return static_cast<size_t>(parsed);
		}

		[[nodiscard]] utils::OwnedStdString requireProjectionName(const logical::LogicalCallProcedure::ArgumentView &arguments,
														const std::string_view procedureName,
														const size_t expectedArguments) {
			if (arguments.size() != expectedArguments || arguments.empty() ||
				arguments.front().getType() != PropertyType::STRING) {
				throw std::invalid_argument(std::string(procedureName) + " requires a nonempty projection name");
			}
			const std::string &name = std::get<std::string>(arguments.front().getVariant());
			if (name.empty()) {
				throw std::invalid_argument(std::string(procedureName) + " requires a nonempty projection name");
			}
			return name;
		}

		[[nodiscard]] int64_t requireGdsPositiveInteger(const PropertyValue &value, const std::string_view context) {
			if (value.getType() != PropertyType::INTEGER) {
				throw std::invalid_argument(std::string(context) + " must be a positive integer");
			}
			const int64_t parsed = std::get<int64_t>(value.getVariant());
			if (parsed <= 0) {
				throw std::invalid_argument(std::string(context) + " must be a positive integer");
			}
			return parsed;
		}

		[[nodiscard]] size_t requireGdsNonNegativeInteger(const PropertyValue &value, const std::string_view context) {
			if (value.getType() != PropertyType::INTEGER) {
				throw std::invalid_argument(std::string(context) + " must be a non-negative integer");
			}
			const int64_t parsed = std::get<int64_t>(value.getVariant());
			if (parsed < 0) {
				throw std::invalid_argument(std::string(context) + " must be a non-negative integer");
			}
			return static_cast<size_t>(parsed);
		}

		[[nodiscard]] double requireGdsFiniteNumber(const PropertyValue &value, const std::string_view context) {
			double parsed = 0.0;
			if (value.getType() == PropertyType::INTEGER) {
				parsed = static_cast<double>(std::get<int64_t>(value.getVariant()));
			} else if (value.getType() == PropertyType::DOUBLE) {
				parsed = std::get<double>(value.getVariant());
			} else {
				throw std::invalid_argument(std::string(context) + " must be numeric");
			}
			if (!std::isfinite(parsed)) {
				throw std::invalid_argument(std::string(context) + " must be finite");
			}
			return parsed;
		}

		[[nodiscard]] int64_t checkedOutputNodeId(const uint64_t nodeId) {
			if (nodeId == 0U || nodeId > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
				throw std::overflow_error("logical GDS result Node ID cannot be represented as a Cypher integer");
			}
			return static_cast<int64_t>(nodeId);
		}

		[[nodiscard]] storage::LogicalGraphNode validateProcedureNodeIdentity(storage::LogicalGraphReadSnapshot &snapshot,
																	  uint64_t nodeId) {
			const storage::LogicalGraphNode node = snapshot.readNodeShaped(nodeId, {});
			validateNode(node);
			if (node.node_id != nodeId) {
				throw std::runtime_error("logical procedure backend resolved a mismatched Node identity");
			}
			return node;
		}

		[[nodiscard]] utils::OwnedStdVector<uint64_t>
		findShortestPath(storage::LogicalGraphReadSnapshot &snapshot, uint64_t startNodeId, uint64_t endNodeId,
						 size_t maximumDepth, const LogicalPlanReadRuntimeBounds &bounds, const QueryContext &context) {
			if (bounds.maximum_node_records == 0U || bounds.maximum_edges_per_node == 0U ||
				bounds.maximum_adjacency_page_records == 0U || bounds.maximum_property_entries == 0U ||
				bounds.maximum_rows == 0U || bounds.maximum_materialized_bytes == 0U) {
				throw std::invalid_argument("logical shortest-path procedure requires positive graph read bounds");
			}
			std::array<uint64_t, 2U> endpointIds{startNodeId, endNodeId};
			std::ranges::sort(endpointIds);
			const std::span<const uint64_t> endpoints(endpointIds.data(), startNodeId == endNodeId ? 1U : 2U);
			if (snapshot.supportsNodeExistenceProbes()) {
				const std::vector<bool> present = snapshot.containsNodesSorted(endpoints);
				if (present.size() != endpoints.size()) {
					throw std::runtime_error("logical procedure backend changed shortest-path endpoint batch cardinality");
				}
				if (std::ranges::any_of(present, [](const bool value) { return !value; })) {
					return {};
				}
			} else if (!snapshot.containsNode(startNodeId) || !snapshot.containsNode(endNodeId)) {
				return {};
			}
			if (startNodeId == endNodeId) {
				if (!snapshot.supportsNodeExistenceProbes()) static_cast<void>(validateProcedureNodeIdentity(snapshot, startNodeId));
				return {startNodeId};
			}
			// A snapshot with a sorted existence probe can authenticate the start
			// endpoint without decoding its complete payload. Expression or output
			// requirements perform any subsequent shaped reads. Adapters without that
			// probe use an ID-only point read for endpoint validation.
			if (!snapshot.supportsNodeExistenceProbes()) {
				static_cast<void>(validateProcedureNodeIdentity(snapshot, startNodeId));
			}
			if (bounds.maximum_node_records > std::numeric_limits<size_t>::max() / bounds.maximum_edges_per_node) {
				throw std::overflow_error("logical shortest-path procedure Edge bound overflowed");
			}
			const size_t maximumExploredEdges = bounds.maximum_node_records * bounds.maximum_edges_per_node;

			struct Parent {
				uint64_t node_id = 0U;
			};
			const auto checkedStateAdd = [](const size_t left, const size_t right) {
				if (right > std::numeric_limits<size_t>::max() - left) {
					throw std::overflow_error("logical shortest-path procedure state size overflowed");
				}
				return left + right;
			};
			const auto checkedStateMul = [](const size_t left, const size_t right) {
				if (right != 0U && left > std::numeric_limits<size_t>::max() / right) {
					throw std::overflow_error("logical shortest-path procedure state size overflowed");
				}
				return left * right;
			};
			const auto stateBytesFor = [&](const size_t nodeCount, const size_t frontierHighWater,
					const size_t parentCount) {
				const size_t depthBytes = checkedStateMul(nodeCount,
						sizeof(std::pair<const uint64_t, size_t>) + 3U * sizeof(void *));
				const size_t parentBytes = checkedStateMul(parentCount,
						sizeof(std::pair<const uint64_t, Parent>) + 3U * sizeof(void *));
				const size_t frontierBytes = checkedStateMul(frontierHighWater,
						sizeof(uint64_t) + 2U * sizeof(void *));
				return checkedStateAdd(checkedStateAdd(depthBytes, parentBytes), frontierBytes);
			};
			// Charge allocator requests (including bucket arrays and deque blocks)
            // independently of the conservative local algorithm work limit.
            std::deque<uint64_t, utils::OwnedPmrAllocator<uint64_t>> frontier;
            using DepthEntry = std::pair<const uint64_t, size_t>;
            using ParentEntry = std::pair<const uint64_t, Parent>;
            std::unordered_map<uint64_t, size_t, std::hash<uint64_t>, std::equal_to<uint64_t>,
                utils::OwnedPmrAllocator<DepthEntry>> depths;
            std::unordered_map<uint64_t, Parent, std::hash<uint64_t>, std::equal_to<uint64_t>,
                utils::OwnedPmrAllocator<ParentEntry>> parents;
			MaterializedByteBudget procedureBudget(bounds.maximum_materialized_bytes);
			MaterializedByteReservation retainedStateBytes(&procedureBudget);
			size_t frontierHighWater = 1U;
			const auto admitState = [&](const size_t nodes, const size_t frontierCount, const size_t parentCount) {
				frontierHighWater = std::max(frontierHighWater, frontierCount);
				const size_t nextBytes = stateBytesFor(nodes, frontierHighWater, parentCount);
				if (nextBytes > bounds.maximum_materialized_bytes) {
					throw std::runtime_error("logical shortest-path procedure exceeded its materialized state byte bound");
				}
				retainedStateBytes.set(nextBytes);
			};
			admitState(1U, 1U, 0U);
			frontier.push_back(startNodeId);
			depths.emplace(startNodeId, 0U);
			size_t exploredEdges = 0U;
			bool found = false;

			while (!frontier.empty() && !found) {
				context.checkGuard();
				const uint64_t currentNodeId = frontier.front();
				frontier.pop_front();
				const size_t depth = depths.at(currentNodeId);
				if (depth == maximumDepth) {
					continue;
				}
				size_t sourceEdges = 0U;
				const auto visitDirection = [&](storage::LogicalGraphReadDirection direction) {
					std::optional<storage::LogicalGraphAdjacencyContinuation> continuation;
					uint64_t previousEdgeId = 0U;
					for (;;) {
						if (sourceEdges == bounds.maximum_edges_per_node || exploredEdges == maximumExploredEdges) {
							throw std::runtime_error("logical shortest-path procedure exceeded its Edge bound");
						}
						const size_t pageRecords = std::min(bounds.maximum_edges_per_node - sourceEdges,
															bounds.maximum_adjacency_page_records);
						MaterializedByteReservation adjacencyPageBytes(&procedureBudget);
						// Shortest-path expansion only needs Edge identity and endpoints.
						// Keep the topology cursor on the hot BFS path so relationship
						// Property roots and payloads are not decoded for edges that are
						// never returned.
						size_t requestedRecords = pageRecords;
						const auto page = [&] {
							for (;;) {
								context.checkGuard();
								struct TopologyBufferPressure {};
								storage::LogicalGraphAdjacencyTopologyPage candidate;
								try {
									candidate = snapshot.supportsAdjacencyTopologyBufferAdmission()
										? snapshot.scanAdjacentEdgeTopologiesWithBufferAdmission(currentNodeId, direction,
											continuation, requestedRecords, [&](size_t bytes) {
												context.checkGuard();
												if (adjacencyPageBytes.wouldExceed(bytes)) throw TopologyBufferPressure{};
												adjacencyPageBytes.set(bytes);
											})
										: snapshot.scanAdjacentEdgeTopologies(currentNodeId, direction, continuation, requestedRecords);
								} catch (const TopologyBufferPressure &) {
									adjacencyPageBytes.reset();
									if (requestedRecords == 1U) throw std::runtime_error("shortest-path topology source buffers exceeded their byte bound");
									requestedRecords = std::max<size_t>(1U, requestedRecords / 2U);
									continue;
								}
								if (candidate.edges.size() > requestedRecords ||
									(candidate.edges.empty() && candidate.continuation.has_value()) ||
									(continuation.has_value() && (candidate.edges.empty() ||
										candidate.edges.front().edge_id != continuation->next_edge_id))) {
									throw std::runtime_error("logical procedure backend returned an invalid shortest-path adjacency page");
								}
								uint64_t candidatePreviousId = previousEdgeId;
								for (const auto &edge: candidate.edges) {
									context.checkGuard();
									validateEdge(edge);
									if (candidatePreviousId != 0U && edge.edge_id <= candidatePreviousId) {
										throw std::runtime_error("logical procedure backend returned non-monotonic shortest-path relationship IDs");
									}
									candidatePreviousId = edge.edge_id;
									if ((direction == storage::LogicalGraphReadDirection::LGRD_OUTGOING && edge.source_node_id != currentNodeId) ||
										(direction == storage::LogicalGraphReadDirection::LGRD_INCOMING && edge.target_node_id != currentNodeId)) {
										throw std::runtime_error("logical procedure backend returned a mismatched shortest-path relationship");
									}
								}
								size_t bytes = checkedStateMul(candidate.edges.capacity(), sizeof(storage::LogicalGraphEdgeTopology));
								for (const auto &edge: candidate.edges) bytes = checkedStateAdd(bytes, edge.type.capacity());
								const size_t routingBytes = snapshot.supportsNodeExistenceProbes()
									? checkedStateMul(candidate.edges.size(), 2U * sizeof(uint64_t)) : 0U;
								const size_t admissionBytes = checkedStateAdd(bytes, routingBytes);
								if (!adjacencyPageBytes.wouldExceed(admissionBytes) || requestedRecords == 1U) {
									adjacencyPageBytes.set(admissionBytes);
									return candidate;
								}
								// No edge was processed: release this page and retry the same
								// continuation with a smaller prefix before probing endpoints.
								adjacencyPageBytes.reset();
								requestedRecords = std::max<size_t>(1U, requestedRecords / 2U);
							}
						}();
						size_t retainedPageBytes = checkedStateMul(page.edges.capacity(), sizeof(storage::LogicalGraphEdgeTopology));
						for (const auto &edge: page.edges) retainedPageBytes = checkedStateAdd(retainedPageBytes, edge.type.capacity());
						// Endpoint validation is independent of edge traversal order. Resolve
						// the distinct endpoint set once per topology page so a high-degree
						// frontier node does not repeat the same Entity Directory descent for
						// parallel edges or repeated targets.
						utils::OwnedStdVector<uint64_t> endpointIds;
						if (snapshot.supportsNodeExistenceProbes()) {
							const size_t plannedEndpointBytes = checkedStateMul(page.edges.size(), sizeof(uint64_t));
							const size_t plannedPresenceBytes = checkedStateMul(page.edges.size(), sizeof(uint64_t));
							adjacencyPageBytes.set(checkedStateAdd(retainedPageBytes,
									checkedStateAdd(plannedEndpointBytes, plannedPresenceBytes)));
							endpointIds.reserve(page.edges.size());
							const size_t reservedEndpointBytes = checkedStateMul(endpointIds.capacity(),
									sizeof(uint64_t));
							const size_t expectedPresenceBytes = plannedPresenceBytes;
							adjacencyPageBytes.set(checkedStateAdd(retainedPageBytes,
									checkedStateAdd(reservedEndpointBytes, expectedPresenceBytes)));
							for (const storage::LogicalGraphEdgeTopology &candidate: page.edges) {
								if (candidate.source_node_id == currentNodeId &&
										 direction == storage::LogicalGraphReadDirection::LGRD_OUTGOING) {
									endpointIds.push_back(candidate.target_node_id);
								} else if (candidate.target_node_id == currentNodeId &&
										 direction == storage::LogicalGraphReadDirection::LGRD_INCOMING) {
									endpointIds.push_back(candidate.source_node_id);
								}
							}
							std::ranges::sort(endpointIds);
							endpointIds.erase(std::unique(endpointIds.begin(), endpointIds.end()), endpointIds.end());
						}
						const std::vector<bool> endpointPresent = snapshot.supportsNodeExistenceProbes()
								? snapshot.containsNodesSorted(endpointIds)
								: std::vector<bool>{};
						if (snapshot.supportsNodeExistenceProbes()) {
							if (endpointPresent.size() != endpointIds.size()) {
								throw std::runtime_error("logical procedure backend changed shortest-path endpoint batch cardinality");
							}
							const size_t actualEndpointBytes = checkedStateMul(endpointIds.capacity(),
									sizeof(uint64_t));
							const size_t actualPresenceBytes = checkedStateAdd(endpointPresent.capacity(), 7U) / 8U;
							adjacencyPageBytes.set(checkedStateAdd(retainedPageBytes,
									checkedStateAdd(actualEndpointBytes, actualPresenceBytes)));
						}
						for (const storage::LogicalGraphEdgeTopology &edge: page.edges) {
							context.checkGuard();
							validateEdge(edge);
							if (previousEdgeId != 0U && edge.edge_id <= previousEdgeId) {
								throw std::runtime_error(
										"logical procedure backend returned non-monotonic shortest-path relationship IDs");
							}
							previousEdgeId = edge.edge_id;
							const bool endpointMatches = direction == storage::LogicalGraphReadDirection::LGRD_OUTGOING
																 ? edge.source_node_id == currentNodeId
																 : edge.target_node_id == currentNodeId;
							if (!endpointMatches) {
								throw std::runtime_error(
										"logical procedure backend returned a mismatched shortest-path relationship");
							}
							++exploredEdges;
							++sourceEdges;
							// OUT is visited first, so only an incoming self-loop repeats an
							// already processed logical relationship.
							if (direction == storage::LogicalGraphReadDirection::LGRD_INCOMING &&
								edge.source_node_id == currentNodeId) {
								continue;
							}
							const uint64_t nextNodeId = direction == storage::LogicalGraphReadDirection::LGRD_OUTGOING
																			? edge.target_node_id
																			: edge.source_node_id;
							if (snapshot.supportsNodeExistenceProbes()) {
								const auto endpoint = std::ranges::lower_bound(endpointIds, nextNodeId);
								if (endpoint == endpointIds.end() || *endpoint != nextNodeId ||
										endpointPresent[static_cast<size_t>(endpoint - endpointIds.begin())] == false) {
									throw std::runtime_error(
											"logical shortest-path procedure encountered an Edge with a missing endpoint");
								}
							} else {
								// Older logical adapters have no cheap existence probe; retain
								// an ID-only shaped read for endpoint authentication.
								static_cast<void>(validateProcedureNodeIdentity(snapshot, nextNodeId));
							}
							if (depths.contains(nextNodeId)) {
								continue;
							}
							if (depths.size() == bounds.maximum_node_records) {
								throw std::runtime_error("logical shortest-path procedure exceeded its Node bound");
							}
							const size_t nextNodeCount = checkedStateAdd(depths.size(), 1U);
							const size_t nextFrontierCount = nextNodeId == endNodeId ? frontier.size()
									: checkedStateAdd(frontier.size(), 1U);
							admitState(nextNodeCount, nextFrontierCount,
									checkedStateAdd(parents.size(), 1U));
							if (nextNodeCount > depths.bucket_count() * depths.max_load_factor()) {
								depths.reserve(nextNodeCount);
							}
							if (parents.size() + 1U > parents.bucket_count() * parents.max_load_factor()) {
								parents.reserve(parents.size() + 1U);
							}
							depths.emplace(nextNodeId, depth + 1U);
							parents.emplace(nextNodeId, Parent{currentNodeId});
							if (nextNodeId == endNodeId) {
								found = true;
								return;
							}
							frontier.push_back(nextNodeId);
						}
						if (!page.continuation.has_value()) {
							return;
						}
						if (sourceEdges == bounds.maximum_edges_per_node ||
							page.continuation->previous_edge_id != page.edges.back().edge_id ||
							page.continuation->next_edge_id == 0U ||
							page.continuation->previous_edge_id == page.continuation->next_edge_id) {
							throw std::runtime_error(
									"logical procedure backend returned an invalid shortest-path continuation");
						}
						continuation = page.continuation;
					}
				};
				visitDirection(storage::LogicalGraphReadDirection::LGRD_OUTGOING);
				if (!found) {
					visitDirection(storage::LogicalGraphReadDirection::LGRD_INCOMING);
				}
			}
			if (!found) {
				return {};
			}

			utils::OwnedStdVector<uint64_t> reversed;
			MaterializedByteReservation resultPathBytes(&procedureBudget);
			resultPathBytes.set(sizeof(uint64_t));
			reversed.reserve(1U);
			reversed.push_back(endNodeId);
			while (reversed.back() != startNodeId) {
				const auto parent = parents.find(reversed.back());
				if (parent == parents.end() || parent->second.node_id == 0U) {
					throw std::runtime_error("logical shortest-path procedure has an incomplete parent chain");
				}
				const size_t nextSize = checkedStateAdd(reversed.size(), 1U);
				if (nextSize > bounds.maximum_rows) {
					throw std::runtime_error("logical shortest-path procedure exceeded its result-row bound");
				}
				const size_t nextCapacity = nextSize > reversed.capacity()
						? std::max(nextSize, checkedStateMul(reversed.capacity(), 2U))
						: reversed.capacity();
				resultPathBytes.set(checkedStateMul(nextCapacity, sizeof(uint64_t)));
				if (nextCapacity > reversed.capacity()) reversed.reserve(nextCapacity);
				resultPathBytes.set(checkedStateMul(reversed.capacity(), sizeof(uint64_t)));
				reversed.push_back(parent->second.node_id);
			}
			std::ranges::reverse(reversed);
			return reversed;
		}

		[[nodiscard]] std::optional<LogicalPlanReadRow>
		applyProjection(const logical::LogicalProject &projection, storage::LogicalGraphReadSnapshot &snapshot,
						const LogicalPlanReadRuntimeBounds &readBounds, const QueryContext &context,
						LogicalPlanReadRow &source, NodePropertyMaps &nodeProperties,
						ProcedureDistinctRows *distinctRows, const size_t retainedOtherBytes = 0U,
						const bool accountRetainedProjectionBytes = false, const bool deferDistinct = false) {
			if ((projection.isDistinct() && distinctRows == nullptr && !deferDistinct) ||
				(!projection.isDistinct() && distinctRows != nullptr) || (deferDistinct && distinctRows != nullptr)) {
				throw std::logic_error("logical Vector procedure projection DISTINCT state is inconsistent");
			}
			if (projection.getItems().empty()) {
				throw std::invalid_argument("logical Vector procedure Project has no items");
			}

			detail::ProcedurePayloadAdmissionScope::admit(
                detail::procedureEmptyMapHeapUpper<decltype(LogicalPlanReadRow::values)>() +
                detail::procedureEmptyMapHeapUpper<decltype(LogicalPlanReadRow::property_cache)>() +
                detail::procedureEmptyMapHeapUpper<decltype(LogicalPlanReadRow::entity_metadata_cache)>() +
                detail::procedureEmptyMapHeapUpper<decltype(LogicalPlanReadRow::complete_entity_property_cache)>() +
                detail::procedureEmptyMapHeapUpper<decltype(LogicalPlanReadRow::path_cache)>());
            LogicalPlanReadRow result;
			detail::procedureMapReserve(result.values, projection.getItems().size());
			detail::procedureMapReserve(result.property_cache, projection.getItems().size());
			const auto addBytes = [](const size_t left, const size_t right) {
				if (right > std::numeric_limits<size_t>::max() - left) {
					throw std::overflow_error("logical procedure Project materialization size overflowed");
				}
				return left + right;
			};
			const auto mulBytes = [](const size_t left, const size_t right) {
				if (right != 0U && left > std::numeric_limits<size_t>::max() / right) {
					throw std::overflow_error("logical procedure Project materialization size overflowed");
				}
				return left * right;
			};
			const auto valueBytes = [&](const LogicalPlanReadValue &value) {
				size_t bytes = sizeof(value);
				if (const auto *scalar = std::get_if<PropertyValue>(&value)) {
					return addBytes(bytes, property_utils::getPropertyValueSize(*scalar));
				}
				if (const auto *reference = std::get_if<storage::EntityRef>(&value)) {
					static_cast<void>(reference);
					return addBytes(bytes, sizeof(storage::EntityRef));
				}
				if (const auto *node = std::get_if<storage::LogicalGraphNode>(&value)) {
					bytes = addBytes(bytes, mulBytes(node->labels.capacity(), sizeof(std::string)));
					for (const std::string &label: node->labels) bytes = addBytes(bytes, label.size());
					bytes = addBytes(bytes, mulBytes(node->properties.capacity(), sizeof(storage::LogicalGraphPropertyEntry)));
					for (const auto &entry: node->properties) {
						bytes = addBytes(bytes, entry.key.size());
						bytes = addBytes(bytes, property_utils::getPropertyValueSize(entry.value));
					}
					return bytes;
				}
				const auto &edge = std::get<storage::LogicalGraphEdge>(value);
				bytes = addBytes(bytes, edge.type.size());
				bytes = addBytes(bytes, mulBytes(edge.properties.capacity(), sizeof(storage::LogicalGraphPropertyEntry)));
				for (const auto &entry: edge.properties) {
					bytes = addBytes(bytes, entry.key.size());
					bytes = addBytes(bytes, property_utils::getPropertyValueSize(entry.value));
				}
				return bytes;
			};
			const auto rowPayloadBytes = [&](const LogicalPlanReadRow &row) {
				size_t bytes = sizeof(LogicalPlanReadRow);
				for (const auto &[name, value]: row.values) {
					bytes = addBytes(bytes, addBytes(name.size(), valueBytes(value)));
				}
				for (const auto &[alias, properties]: row.property_cache) {
					bytes = addBytes(bytes, alias.size());
					for (const auto &[key, value]: properties) {
						bytes = addBytes(bytes, addBytes(key.size(), property_utils::getPropertyValueSize(value)));
					}
				}
				for (const auto &[alias, metadata]: row.entity_metadata_cache) {
					bytes = addBytes(bytes, alias.size());
					for (const auto &[key, value]: metadata) {
						bytes = addBytes(bytes, addBytes(key.size(), property_utils::getPropertyValueSize(value)));
					}
				}
				return bytes;
			};
			const auto retainedProjectionBytes = [&] {
				if (!accountRetainedProjectionBytes) return size_t{0U};
				// Project outputs replace the source row after evaluation. Keep the
				// newly built output payload charged across sibling expressions, while
				// the procedure's batch and Property readers retain source-row ownership.
				return addBytes(retainedOtherBytes, rowPayloadBytes(result) - sizeof(LogicalPlanReadRow));
			};
			for (const logical::LogicalProjectItem &item: projection.getItems()) {
				context.checkGuard();
				if (item.alias.empty() || !item.expression) {
					throw std::logic_error("logical Vector procedure projection has an invalid output item");
				}
				// Evaluate against the original bindings while retaining new cache
				// entries in place. Output aliases never enter the source scope.
				const size_t retainedBytes = retainedProjectionBytes();
				if (retainedBytes >= readBounds.maximum_materialized_bytes) {
					throw std::runtime_error("logical procedure Project exceeded its shared materialized byte bound before expression");
				}
				LogicalPlanReadRuntimeBounds expressionBounds = readBounds;
				expressionBounds.maximum_materialized_bytes -= retainedBytes;
				LogicalPlanReadValue value = LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(
					snapshot, *item.expression, source, expressionBounds, context);
				if (!detail::procedureMapEmplace(result.values, item.alias, std::move(value)).second) {
					throw std::logic_error("logical Vector procedure projection has duplicate output aliases");
				}
				if (retainedProjectionBytes() > readBounds.maximum_materialized_bytes) {
					throw std::runtime_error("logical procedure Project exceeded its shared materialized byte bound after expression");
				}
			}
			// Scalar-only output cannot use the identity cache in a later layer.
			// Populate it once only for entities that remain in the output scope.
			std::erase_if(nodeProperties, [&](const auto &cached) {
				return !std::ranges::any_of(result.values, [&](const auto &binding) {
					return procedureNodeIdentity(binding.second) == cached.first;
				});
			});
			if (std::ranges::any_of(result.values, [](const auto &binding) {
				return procedureNodeIdentity(binding.second).has_value();
			})) {
				cacheResolvedNodeProperties(source, result, nodeProperties);
				attachCachedNodeProperties(result, nodeProperties);
			}
			// Preserve metadata across an Entity alias projection without fetching
			// it again in a later layer. Scalar outputs carry no entity metadata.
			for (const auto &[alias, value]: result.values) {
				const auto identity = procedureNodeIdentity(value);
				if (!identity) continue;
				for (const auto &[sourceAlias, sourceValue]: source.values) {
					if (procedureNodeIdentity(sourceValue) != identity) continue;
					const auto metadata = source.entity_metadata_cache.find(sourceAlias);
					if (metadata != source.entity_metadata_cache.end()) {
						detail::procedureMapInsertOrAssign(result.entity_metadata_cache, alias, metadata->second);
						break;
					}
				}
			}

			if (distinctRows != nullptr) {
				auto key =
						detail::makeOwnedProcedureDistinctRowKey(result, projection.getOutputVariables());
				if (distinctRows->contains(key)) {
					return std::nullopt;
				}
				if (distinctRows->size() == readBounds.maximum_distinct_rows) {
					throw std::runtime_error("logical Vector procedure DISTINCT exceeded its row bound");
				}
				static_cast<void>(distinctRows->insert(std::move(key)));
			}
			return result;
		}

		[[nodiscard]] bool applyFilter(const logical::LogicalFilter &filter,
									   storage::LogicalGraphReadSnapshot &snapshot,
									   const LogicalPlanReadRuntimeBounds &readBounds, const QueryContext &context,
									   LogicalPlanReadRow &row, NodePropertyMaps &nodeProperties) {
			if (!filter.getPredicate()) {
				throw std::invalid_argument("logical Vector procedure Filter has no predicate");
			}
			// The evaluator only enriches caches. Keep them on the surviving row
			// instead of copying the row and a second identity-indexed payload map.
			const LogicalPlanReadValue evaluated = LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(
					snapshot, *filter.getPredicate(), row, readBounds, context);
			const auto *predicate = std::get_if<PropertyValue>(&evaluated);
			if (predicate == nullptr) {
				throw std::runtime_error("logical Vector procedure Filter requires a scalar predicate");
			}
			if (predicate->getType() == PropertyType::NULL_TYPE) {
				return false;
			}
			if (predicate->getType() != PropertyType::BOOLEAN) {
				throw std::runtime_error("logical Vector procedure Filter requires a Boolean predicate");
			}
			return std::get<bool>(predicate->getVariant());
		}

		// Final procedure rows are consumed exactly once. Transfer their payloads
		// directly instead of rebuilding a reusable identity cache at the result
		// boundary. Each bare output alias already owns its complete Node.
		[[nodiscard]] ResultValue materializeResultValue(LogicalPlanReadValue &value,
				storage::LogicalGraphReadSnapshot &snapshot, const LogicalPlanReadRuntimeBounds &readBounds,
				const QueryContext &context, NodePropertyMaps &nodeProperties) {
			context.checkGuard();
			if (auto *property = std::get_if<PropertyValue>(&value)) return std::move(*property);
			if (auto *node = std::get_if<storage::LogicalGraphNode>(&value)) {
				validateNode(*node);
				nodeProperties.erase(node->node_id);
				auto properties = takeNodeProperties(*node, readBounds.maximum_property_entries, context);
				return LogicalNodeResult{node->node_id, std::move(node->labels), std::move(properties)};
			}
			if (const auto *reference = std::get_if<storage::EntityRef>(&value)) {
				if (reference->is_edge || reference->entity_id == 0U) throw std::logic_error("logical Vector procedure result contains an invalid Node reference");
				const uint64_t nodeId = reference->entity_id;
				auto node = snapshot.readNodeShaped(nodeId, completeNodeReadShape());
				validateNode(node);
				if (node.node_id != nodeId) throw std::runtime_error("logical Vector procedure result Node reference changed identity");
				value = std::move(node);
				return materializeResultValue(value, snapshot, readBounds, context, nodeProperties);
			}
			throw std::logic_error("logical Vector procedure result contains an unsupported Edge binding");
		}

