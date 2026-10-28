#if ZYX_BUILD_STREAMING_COUNT_PATH
        [[nodiscard]] bool streamingCountEnabled() noexcept {
#if defined(ZYX_ENABLE_STORAGE_TEST_HOOKS)
            return (ZYX_EXPERIMENTAL_STREAMING_COUNT || test_enable_streaming_count) && !test_disable_streaming_count;
#else
            return ZYX_EXPERIMENTAL_STREAMING_COUNT;
#endif
        }
#endif
#if ZYX_BUILD_BORROWED_GROUP_KEY_PATH
		[[nodiscard]] bool borrowedGroupKeyEnabled() noexcept {
#if defined(ZYX_ENABLE_STORAGE_TEST_HOOKS)
            return ZYX_EXPERIMENTAL_BORROWED_GROUP_KEY || test_enable_borrowed_group_key;
#else
            return ZYX_EXPERIMENTAL_BORROWED_GROUP_KEY;
#endif
        }
#endif
		struct NodeColumnReceiver {
			std::span<const std::string_view> keys;
			std::function<bool(std::span<const std::string>,
				std::span<const std::vector<PropertyValue>>, const SelectionVector &,
				std::span<const size_t>)> consume;
			std::function<bool(std::span<const std::string>, const detail::DirectPropertyColumns &,
				const SelectionVector &, std::span<const size_t>)> consumeDirect{};
			const logical::LogicalOperator *filterOwner = nullptr;
#if ZYX_BUILD_STREAMING_COUNT_PATH
            std::function<bool(size_t)> consumeCount{};
            std::span<const detail::ColumnPredicate> countPredicates{};
#endif
		};
		[[nodiscard]] constexpr bool supportsChargedRowHandoff(logical::LogicalOpType type) {
			return type == logical::LogicalOpType::LOP_SORT || type == logical::LogicalOpType::LOP_FILTER ||
				   type == logical::LogicalOpType::LOP_PROJECT || type == logical::LogicalOpType::LOP_LIMIT ||
				   type == logical::LogicalOpType::LOP_SKIP || type == logical::LogicalOpType::LOP_UNWIND ||
				   type == logical::LogicalOpType::LOP_UNION || type == logical::LogicalOpType::LOP_JOIN ||
				   type == logical::LogicalOpType::LOP_OPTIONAL_MATCH ||
				   type == logical::LogicalOpType::LOP_CALL_SUBQUERY || type == logical::LogicalOpType::LOP_LOAD_CSV ||
				   type == logical::LogicalOpType::LOP_TRAVERSAL;
		}
		// Property batches are bounded independently of the result bound.  The
		// value is a memory-safety policy, not a semantic limit; callers can still
		// lower it through the existing maximum_rows bound.
		constexpr size_t DEFAULT_PROPERTY_BATCH_ROWS = 512U;

		enum class ShortestPathResultMode { SPR_FULL, SPR_SIZE, SPR_LENGTH };

		void validateTraversalTargetProperties(const logical::LogicalTraversal &traversal) {
			const auto &targetPredicates = traversal.getTargetProperties();
			for (size_t index = 0U; index < targetPredicates.size(); ++index) {
				const auto &key = targetPredicates[index].first;
				if (key.empty() || std::any_of(targetPredicates.begin(),
						targetPredicates.begin() + index,
						[&](const auto &predicate) { return predicate.first == key; })) {
					throw std::invalid_argument("logical-plan target Property predicate has an "
												"empty or duplicate key");
				}
			}
		}

		struct ScopedPropertyColumns {
			const std::string *variable = nullptr;
			const std::vector<std::string> *keys = nullptr;
			std::vector<uint64_t> ids;
			std::vector<std::vector<PropertyValue>> columns;
		};


		// Kept outside the traversal lambda so the callback has a stable type on
		// compilers that cannot capture a function over a local class correctly.
		struct VariableTraversalAdjacentEdge {
			storage::LogicalGraphEdgeTopology topology;
			std::optional<storage::EntityRef> reference;
		};

		// Query owners outlive every task and all borrowed batch views.
		// Shared caches and budgets are currently confined to one execution thread.
		struct ExecutionSharedState {
			storage::LogicalGraphReadSnapshot &snapshot;
			const LogicalPlanReadRuntimeBounds &bounds;
			const QueryContext *context = nullptr;
			// Public row callbacks and mutation planning retain fully materialized
			// entities. Only the result pipeline owns the deferred-row contract.
			bool allow_deferred_entity_refs = false;
			// QueryResult consumers may safely read ahead within a bounded batch. Public
			// row visitors keep this disabled so cancellation never materializes a later
			// path row that the caller did not request.
			bool allow_cross_row_entity_batches = false;
			size_t resident_output_batch_bytes = 0U;
			MaterializedByteBudget materialized_budget{bounds.maximum_materialized_bytes - resident_output_batch_bytes,
				context ? context->resource_account : nullptr, MaterializedByteBudget::OptionalAccountTag{}};
			detail::ProcedureBudgetHandoff procedure_handoff{materialized_budget};
			MaterializedByteReservation materialized_cache_reservation{&materialized_budget};
			MaterializedByteReservation final_result_reservation{&materialized_budget};
			size_t final_result_bytes = 0U;
			// Logical snapshots are immutable. A small per-query cache avoids repeated
			// point reads when a high-degree adjacency list shares targets. The byte
			// budget is independent of the entry bound: a single LargeValue-bearing
			// entity must not turn this convenience cache into an unbounded copy of
			// the Property store.
			detail::BudgetMemoryResource entity_cache_memory{materialized_budget};
			std::pmr::unordered_map<uint64_t, storage::LogicalGraphNode> node_cache{&entity_cache_memory};
			// Keep the same bounded reuse for Edge references that cross a result
			// boundary more than once (for example, a repeated path or projection).
			std::pmr::unordered_map<uint64_t, storage::LogicalGraphEdge> edge_cache{&entity_cache_memory};
			size_t materialized_cache_bytes = 0U;
			std::optional<detail::BoundExecutionProgram> bound_program{};
		};

		struct ExecutionTaskState {
			LogicalPlanReadRuntimeStats stats;
			const LogicalPlanReadRow *injected_row = nullptr;
			const ScopedPropertyColumns *property_columns = nullptr;
			detail::BoundExpressionInputs *bound_inputs = nullptr;
			const logical::LogicalSort *topk_sort = nullptr;
			size_t topk_rows = 0U;
		};

		// A task borrows query resources; scoped input/column views never become
		// query-global state. Parallel scheduling requires shared work admission
		// and explicit statistics merging before multiple tasks can run together.
		struct ExecutionState {
			ExecutionSharedState &shared;
			ExecutionTaskState task;
		};

		class ScopedBoundInputs final {
		public:
			ScopedBoundInputs(ExecutionState &state, std::optional<detail::BoundExpressionInputs> &inputs,
				const LogicalPlanReadRow &row) : state_(state), previous_(state.task.bound_inputs) {
				if (inputs) { binding_.emplace(*inputs, row); state_.task.bound_inputs = &*inputs; }
			}
			~ScopedBoundInputs() { state_.task.bound_inputs = previous_; }
			ScopedBoundInputs(const ScopedBoundInputs &) = delete;
			ScopedBoundInputs &operator=(const ScopedBoundInputs &) = delete;
		private:
			ExecutionState &state_;
			detail::BoundExpressionInputs *previous_;
			std::optional<detail::BoundExpressionInputs::RowBinding> binding_;
		};

		using ExecutionFinalizer = std::function<void(ExecutionState &, bool)>;

		constexpr size_t MAXIMUM_MATERIALIZED_CACHE_BYTES = 4U * 1024U * 1024U;

		// Complete objects are an explicit read shape as well.  Keeping this at the
		// runtime boundary ensures every entity materialization uses the same shape
		// contract as selected-column readers, while legacy snapshots can still use
		// the virtual fallback implementation.
		[[nodiscard]] storage::EntityReadShape completeNodeReadShape() {
			return {.include_labels = true, .include_type = false, .all_properties = true};
		}

		[[nodiscard]] storage::EntityReadShape completeEdgeReadShape() {
			return {.include_labels = false, .include_type = true, .all_properties = true};
		}

		// Pattern-local bindings are materialized only with the fields required by
		// their WHERE/map expressions, including explicit complete-map needs. A null
		// plan means an unsupported scope, preserving the complete-read fallback.
		// Unresolved tokens use the named-column boundary and its backend fallback. Complete-map EXISTS retains its
		// lazy visitor.
		struct PatternBindingReadShape {
			storage::EntityReadShape target;
			storage::EntityReadShape relationship;
			std::vector<std::string> target_named_properties;
			std::vector<std::string> relationship_named_properties;
		};

		[[nodiscard]] std::optional<PatternBindingReadShape>
		inspectPatternBindingReadShape(const expressions::Expression *whereExpression,
									   const expressions::Expression *mapExpression, std::string_view targetVariable,
									   std::string_view relationshipVariable,
									   storage::LogicalGraphReadSnapshot &snapshot);

		[[nodiscard]] size_t checkedSizeAdd(size_t left, size_t right, const char *message) {
			if (right > std::numeric_limits<size_t>::max() - left) {
				throw std::overflow_error(message);
			}
			return left + right;
		}

		[[nodiscard]] size_t checkedSizeMul(size_t left, size_t right, const char *message) {
			if (right != 0U && left > std::numeric_limits<size_t>::max() / right) {
				throw std::overflow_error(message);
			}
			return left * right;
		}

		[[nodiscard]] size_t csvStringCapacityBytes(const std::string &value) {
			return detail::dynamicStringBufferBytes(value);
		}

		[[nodiscard]] size_t logicalNodeCacheBytes(const storage::LogicalGraphNode &node, const QueryContext *context) {
			if (context != nullptr) context->checkGuardNow();
			size_t bytes = sizeof(node);
			bytes = checkedSizeAdd(
					bytes,
					checkedSizeMul(node.labels.capacity(), sizeof(std::string), "logical-plan Node cache size overflowed"),
					"logical-plan Node cache size overflowed");
			size_t labelsUntilGuard = 256U;
			for (const std::string &label: node.labels) {
				if (--labelsUntilGuard == 0U) {
					if (context != nullptr) context->checkGuardNow();
					labelsUntilGuard = 256U;
				}
				bytes = checkedSizeAdd(bytes, std::max(label.size(), detail::dynamicStringBufferBytes(label)), "logical-plan Node cache size overflowed");
			}
			bytes = checkedSizeAdd(bytes,
								   checkedSizeMul(node.properties.capacity(), sizeof(storage::LogicalGraphPropertyEntry),
												  "logical-plan Node cache size overflowed"),
								   "logical-plan Node cache size overflowed");
			for (const storage::LogicalGraphPropertyEntry &entry: node.properties) {
				bytes = checkedSizeAdd(bytes, std::max(entry.key.size(), detail::dynamicStringBufferBytes(entry.key)), "logical-plan Node cache size overflowed");
				bytes = checkedSizeAdd(bytes, detail::retainedPropertyValueBytes(entry.value, [&] { if (context != nullptr) context->checkGuardNow(); }),
									   "logical-plan Node cache size overflowed");
			}
			return bytes;
		}

		[[nodiscard]] size_t logicalEdgeCacheBytes(const storage::LogicalGraphEdge &edge, const QueryContext *context) {
			if (context != nullptr) context->checkGuardNow();
			size_t bytes = sizeof(edge);
			bytes = checkedSizeAdd(bytes, std::max(edge.type.size(), detail::dynamicStringBufferBytes(edge.type)), "logical-plan Edge cache size overflowed");
			bytes = checkedSizeAdd(bytes,
								   checkedSizeMul(edge.properties.capacity(), sizeof(storage::LogicalGraphPropertyEntry),
												  "logical-plan Edge cache size overflowed"),
								   "logical-plan Edge cache size overflowed");
			for (const storage::LogicalGraphPropertyEntry &entry: edge.properties) {
				bytes = checkedSizeAdd(bytes, std::max(entry.key.size(), detail::dynamicStringBufferBytes(entry.key)), "logical-plan Edge cache size overflowed");
				bytes = checkedSizeAdd(bytes, detail::retainedPropertyValueBytes(entry.value, [&] { if (context != nullptr) context->checkGuardNow(); }),
									   "logical-plan Edge cache size overflowed");
			}
			return bytes;
		}

		[[nodiscard]] size_t logicalReadValueBytes(const LogicalPlanReadValue &value, const QueryContext *context = nullptr) {
			if (const auto *property = std::get_if<PropertyValue>(&value)) {
				return detail::retainedPropertyValueBytes(*property, [&] { if (context != nullptr) context->checkGuardNow(); });
			}
			if (const auto *node = std::get_if<storage::LogicalGraphNode>(&value)) {
				return logicalNodeCacheBytes(*node, context);
			}
			if (const auto *edge = std::get_if<storage::LogicalGraphEdge>(&value)) {
				return logicalEdgeCacheBytes(*edge, context);
			}
			return sizeof(storage::EntityRef);
		}

		template<typename Include>
		[[nodiscard]] size_t logicalReadRowBytesIncluding(const LogicalPlanReadRow &row, Include includes, const QueryContext *context = nullptr, bool includeValues = true, bool excludeBoundEntityCaches = false) {
			if (context != nullptr) context->checkGuardNow();
			size_t bytes = sizeof(row);
			const auto invalidatedCache = [&](const std::string &alias) {
				if (!excludeBoundEntityCaches) return false;
				const auto binding = row.values.find(alias);
				return binding != row.values.end() &&
					(std::holds_alternative<storage::LogicalGraphNode>(binding->second) ||
					 std::holds_alternative<storage::LogicalGraphEdge>(binding->second));
			};
			size_t untilCheckpoint = 256U;
			const auto checkpoint = [&] {
				if (--untilCheckpoint == 0U) {
					if (context != nullptr) context->checkGuardNow();
					untilCheckpoint = 256U;
				}
			};
			for (const auto &[name, value]: row.values) {
				checkpoint();
				if (!includeValues || !includes(name))
					continue;
				bytes = checkedSizeAdd(bytes, name.size(), "logical-plan row materialization size overflowed");
				bytes = checkedSizeAdd(bytes, sizeof(value), "logical-plan row materialization size overflowed");
				bytes = checkedSizeAdd(bytes, logicalReadValueBytes(value, context),
									   "logical-plan row materialization size overflowed");
			}
			for (const auto &[name, properties]: row.property_cache) {
				checkpoint();
				if (!includes(name) || invalidatedCache(name))
					continue;
				bytes = checkedSizeAdd(bytes, name.size(), "logical-plan row materialization size overflowed");
				for (const auto &[key, value]: properties) {
					bytes = checkedSizeAdd(bytes, key.size(), "logical-plan row materialization size overflowed");
					bytes = checkedSizeAdd(bytes, detail::retainedPropertyValueBytes(value, [&] { if (context != nullptr) context->checkGuardNow(); }),
										   "logical-plan row materialization size overflowed");
				}
			}
			for (const auto &[name, metadata]: row.entity_metadata_cache) {
				checkpoint();
				if (!includes(name) || invalidatedCache(name))
					continue;
				bytes = checkedSizeAdd(bytes, name.size(), "logical-plan row metadata size overflowed");
				for (const auto &[key, value]: metadata) {
					bytes = checkedSizeAdd(bytes, key.size(), "logical-plan row metadata size overflowed");
					bytes = checkedSizeAdd(bytes, detail::retainedPropertyValueBytes(value, [&] { if (context != nullptr) context->checkGuardNow(); }),
										   "logical-plan row metadata size overflowed");
				}
			}
			for (const auto &[name, path]: row.path_cache) {
				checkpoint();
				if (!includes(name))
					continue;
				bytes = checkedSizeAdd(bytes, name.size(), "logical-plan row path materialization size overflowed");
				// Entity estimates include live record objects; charge only spare outer slots here.
				bytes = checkedSizeAdd(bytes, checkedSizeMul(path.nodes.capacity() - path.nodes.size(),
					sizeof(storage::LogicalGraphNode), "logical-plan row path materialization size overflowed"),
					"logical-plan row path materialization size overflowed");
				bytes = checkedSizeAdd(bytes, checkedSizeMul(path.edges.capacity() - path.edges.size(),
					sizeof(storage::LogicalGraphEdge), "logical-plan row path materialization size overflowed"),
					"logical-plan row path materialization size overflowed");
				for (const storage::LogicalGraphNode &node: path.nodes) {
					bytes = checkedSizeAdd(bytes, logicalNodeCacheBytes(node, context),
										   "logical-plan row path materialization size overflowed");
				}
				for (const storage::LogicalGraphEdge &edge: path.edges) {
					bytes = checkedSizeAdd(bytes, logicalEdgeCacheBytes(edge, context),
										   "logical-plan row path materialization size overflowed");
				}
				bytes = checkedSizeAdd(bytes,
									   checkedSizeMul(path.node_refs.capacity(), sizeof(storage::EntityRef),
													  "logical-plan row path materialization size overflowed"),
									   "logical-plan row path materialization size overflowed");
				bytes = checkedSizeAdd(bytes,
									   checkedSizeMul(path.edge_refs.capacity(), sizeof(storage::EntityRef),
													  "logical-plan row path materialization size overflowed"),
									   "logical-plan row path materialization size overflowed");
			}
			return bytes;
		}

		[[nodiscard]] size_t logicalReadRowBytes(const LogicalPlanReadRow &row, const QueryContext *context = nullptr) {
			return logicalReadRowBytesIncluding(row, [](const std::string &) { return true; }, context);
		}

		void enforceMaterializedRowBound(const LogicalPlanReadRow &row, const ExecutionState &state) {
			if (logicalReadRowBytes(row, state.shared.context) > state.shared.bounds.maximum_materialized_bytes) {
				throw std::runtime_error("logical-plan row exceeded its materialized byte bound");
			}
		}

		[[nodiscard]] size_t propertyMapBytes(const PropertyMap &properties, const char *message,
			const QueryContext *context = nullptr) {
			return detail::retainedPropertyMapBytes(properties, [&] {
				if (context != nullptr) context->checkGuardNow();
			});
		}

		[[nodiscard]] size_t
		directPropertyMapStorageBytes(const std::vector<storage::LogicalGraphPropertyEntry> &entries,
									  const char *message, const QueryContext *context) {
			size_t bytes = checkedSizeAdd(
					sizeof(PropertyMap),
					checkedSizeMul(entries.size(), sizeof(PropertyMap::value_type) + sizeof(void *), message), message);
			for (const storage::LogicalGraphPropertyEntry &entry: entries) {
				bytes = checkedSizeAdd(bytes, entry.key.size(), message);
				bytes = checkedSizeAdd(bytes, detail::retainedPropertyValueBytes(entry.value, [&] { if (context != nullptr) context->checkGuardNow(); }), message);
			}
			return bytes;
		}

		[[nodiscard]] size_t resultValueBytes(const ResultValue &value, const QueryContext *context) {
			if (context != nullptr) context->checkGuardNow();
			constexpr const char *message = "logical-plan result materialization size overflowed";
			if (value.isPrimitive()) {
				return detail::retainedPropertyValueBytes(value.asPrimitive(), [&] { if (context != nullptr) context->checkGuardNow(); });
			}
			if (value.isLogicalNode()) {
				const LogicalNodeResult &node = value.asLogicalNode();
				size_t bytes = checkedSizeAdd(sizeof(node),
					checkedSizeMul(node.labels.capacity(), sizeof(std::string), message), message);
				size_t labelsUntilGuard = 256U;
				for (const std::string &label: node.labels) {
					if (--labelsUntilGuard == 0U) {
						if (context != nullptr) context->checkGuardNow();
						labelsUntilGuard = 256U;
					}
					bytes = checkedSizeAdd(bytes, std::max(label.size(), detail::dynamicStringBufferBytes(label)), message);
				}
				return checkedSizeAdd(bytes, propertyMapBytes(node.properties, message, context), message);
			}
			if (value.isLogicalEdge()) {
				const LogicalEdgeResult &edge = value.asLogicalEdge();
				const size_t bytes = checkedSizeAdd(sizeof(edge), std::max(edge.type.size(), detail::dynamicStringBufferBytes(edge.type)), message);
				return checkedSizeAdd(bytes, propertyMapBytes(edge.properties, message, context), message);
			}
			throw std::logic_error("logical-plan runtime produced a physical Entity result");
		}

		[[nodiscard]] size_t materializedResultRowBytes(const QueryResult::Row &row, const ExecutionState &state) {
			size_t bytes = sizeof(row);
			for (const auto &[name, value]: row) {
				bytes = checkedSizeAdd(bytes, name.size(), "logical-plan result row size overflowed");
				bytes = checkedSizeAdd(bytes, sizeof(value), "logical-plan result row size overflowed");
				bytes = checkedSizeAdd(bytes, resultValueBytes(value, state.shared.context), "logical-plan result row size overflowed");
			}
			if (state.shared.context && state.shared.context->resource_account) {
				bytes = (std::max)(bytes, checkedSizeAdd(sizeof(row), graph::query::detail::resultRowHeapBytes(row),
					"logical-plan result heap storage overflowed"));
			}
			if (bytes > state.shared.bounds.maximum_materialized_bytes) {
				throw std::runtime_error("logical-plan result row exceeded its materialized byte bound");
			}
			return bytes;
		}

		[[nodiscard]] size_t queryResultColumnStorageBytes(const std::vector<std::string> &columns) {
			// Include the empty rows/notifications vector proxies in the retained
			// result metadata; their later capacity growth is accounted separately.
			return checkedSizeAdd(graph::query::detail::resultStringsHeapBytes(columns),
				2U * detail::standardContainerProxyBytes(), "logical-plan result column storage size overflowed");
		}

		void appendMaterializedResultRow(QueryResult &result, QueryResult::Row &&row, ExecutionState &state,
			MaterializedByteReservation &&rowReservation) {
			const size_t rowBytes = materializedResultRowBytes(row, state);
			const size_t previousBytes = state.shared.final_result_bytes;
			state.shared.final_result_reservation.absorb(std::move(rowReservation));
			size_t nextBytes = checkedSizeAdd(previousBytes, rowBytes, "logical-plan result retention size overflowed");
			const size_t currentCapacity = result.getRows().capacity();
			if (result.getRows().size() == currentCapacity) {
				const size_t minimumCapacity =
						checkedSizeAdd(result.getRows().size(), 1U, "logical-plan result row capacity overflowed");
				const size_t doubledCapacity =
						currentCapacity == 0U
								? minimumCapacity
								: checkedSizeMul(currentCapacity, 2U, "logical-plan result row capacity overflowed");
				const size_t requestedCapacity = std::max(minimumCapacity, doubledCapacity);
				const size_t requestedGrowthBytes = detail::standardAllocatorRequestBytes(
					checkedSizeMul(requestedCapacity, sizeof(QueryResult::Row),
						"logical-plan result row capacity overflowed"), alignof(QueryResult::Row));
                const size_t relocationBytes = graph::query::detail::resultRowsRelocationHeapUpper(result.getRows());
				const size_t transientBytes = checkedSizeAdd(nextBytes,
                    checkedSizeAdd(requestedGrowthBytes, relocationBytes, "logical-plan result relocation peak overflowed"),
                    "logical-plan result growth peak overflowed");
				state.shared.final_result_reservation.set(transientBytes);
				try {
					result.reserveRows(requestedCapacity);
				} catch (...) {
					state.shared.final_result_reservation.set(previousBytes);
					throw;
				}
				const size_t oldCapacityBytes = detail::standardAllocatorRequestBytes(
					checkedSizeMul(currentCapacity, sizeof(QueryResult::Row),
						"logical-plan result row capacity overflowed"), alignof(QueryResult::Row));
				const size_t actualCapacityBytes = detail::standardVectorHeapBufferBytes(result.getRows()) -
					detail::standardContainerProxyBytes();
				const size_t retainedPayloadBytes = previousBytes - oldCapacityBytes;
				nextBytes = checkedSizeAdd(
						checkedSizeAdd(retainedPayloadBytes, rowBytes, "logical-plan result retention size overflowed"),
						actualCapacityBytes, "logical-plan result retention size overflowed");
				state.shared.final_result_reservation.set(nextBytes);
			} else {
				state.shared.final_result_reservation.set(nextBytes);
			}
			try {
				result.addRow(std::move(row));
			} catch (...) {
				state.shared.final_result_reservation.set(previousBytes);
				throw;
			}
			state.shared.final_result_bytes = nextBytes;
		}

		[[nodiscard]] size_t propertyColumnBytes(const std::vector<std::vector<PropertyValue>> &columns,
												 std::span<const std::string_view> keys, const char *message, const QueryContext *context) {
			if (context != nullptr) context->checkGuardNow();
			size_t bytes = checkedSizeMul(columns.capacity(), sizeof(std::vector<PropertyValue>), message);
			for (size_t column = 0U; column < columns.size(); ++column) {
				bytes = checkedSizeAdd(bytes, checkedSizeMul(columns[column].capacity(), sizeof(PropertyValue), message), message);
				if (column < keys.size()) {
					bytes = checkedSizeAdd(bytes, keys[column].size(), message);
				}
				for (const PropertyValue &value: columns[column]) {
					bytes = checkedSizeAdd(bytes, detail::retainedPropertyValueBytes(value, [&] { if (context != nullptr) context->checkGuardNow(); }), message);
				}
			}
			return bytes;
		}

		[[nodiscard]] PropertyValue copyReservedPropertyValue(const PropertyValue &source,
				ExecutionState &state, MaterializedByteReservation &reservation) {
			const auto checkpoint = [&] { if (state.shared.context != nullptr) state.shared.context->checkGuardNow(); };
			const size_t previous = reservation.bytes();
			constexpr const char *message = "logical-plan copied Property payload size overflowed";
			reservation.set(checkedSizeAdd(previous, detail::retainedPropertyValueBytes(source, checkpoint), message));
			try {
				PropertyValue copy = state.shared.materialized_budget.transfersReservations()
                    ? graph::property_copy::copyPropertyValueGuarded(source, checkpoint)
                    : detail::copyPropertyValueGuarded(source, checkpoint);
				reservation.set(checkedSizeAdd(previous, detail::retainedPropertyValueBytes(copy, checkpoint), message));
				return copy;
			} catch (...) {
				reservation.set(previous);
				throw;
			}
		}

		[[nodiscard]] size_t logicalIndexCandidateBatchBytes(const storage::IndexCandidateBatch &batch, const QueryContext *context) {
			if (context != nullptr) context->checkGuardNow();
			constexpr const char *message = "logical-plan index candidate batch size overflowed";
			size_t bytes = checkedSizeMul(batch.candidates.capacity(), sizeof(storage::EntityRef), message);
			bytes = checkedSizeAdd(bytes, checkedSizeMul(batch.verified_property_columns.capacity(),
				sizeof(storage::IndexCandidateBatch::VerifiedPropertyColumn), message), message);
			for (const storage::IndexCandidateBatch::VerifiedPropertyColumn &column: batch.verified_property_columns) {
				bytes = checkedSizeAdd(bytes, std::max(column.key.capacity(), detail::dynamicStringBufferBytes(column.key)), message);
				bytes = checkedSizeAdd(bytes, checkedSizeMul(column.values.capacity(), sizeof(PropertyValue), message),
									   message);
				for (const PropertyValue &value: column.values) {
					bytes = checkedSizeAdd(bytes, detail::retainedPropertyValueBytes(value, [&] { if (context != nullptr) context->checkGuardNow(); }), message);
				}
			}
			return bytes;
		}

		void checkGuard(const ExecutionState &state) {
			if (state.shared.context != nullptr) {
				state.shared.context->checkGuard();
			}
		}

		// Copy/accounting helpers already bound work between checkpoints. Do not
		// apply the per-row guard sampling interval a second time at that boundary.
		void checkGuardNow(const ExecutionState &state) {
			if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
		}

		void validateNode(const storage::LogicalGraphNode &node, ExecutionState &state);
		void validateEdge(const storage::LogicalGraphEdge &edge);

		template<typename Record, typename Cache>
		void cacheEntityRecord(ExecutionState &state, Cache &cache, uint64_t id, Record &&record, size_t bytes,
							   size_t maximumRecords) {
			if (detail::ProcedurePayloadAdmissionScope::active()) return;
			if (cache.contains(id) || cache.size() >= maximumRecords ||
				bytes > MAXIMUM_MATERIALIZED_CACHE_BYTES -
								std::min(state.shared.materialized_cache_bytes, MAXIMUM_MATERIALIZED_CACHE_BYTES)) {
				return;
			}
			const size_t previousBytes = state.shared.materialized_cache_bytes;
			const size_t previousReservedBytes = state.shared.materialized_cache_reservation.bytes();
			using CachedRecord = typename Cache::mapped_type;
			state.shared.materialized_cache_reservation.set(checkedSizeAdd(previousReservedBytes, bytes - sizeof(CachedRecord),
				"logical-plan Entity cache payload size overflowed"));
			try {
				CachedRecord retained = [&]() -> CachedRecord {
					if constexpr (std::is_lvalue_reference_v<Record>) {
						return detail::copyLogicalEntityGuarded(record, [&] {
							if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
						});
					} else {
						return std::forward<Record>(record);
					}
				}();
				if constexpr (std::is_same_v<CachedRecord, storage::LogicalGraphNode>)
					bytes = logicalNodeCacheBytes(retained, state.shared.context);
				else
					bytes = logicalEdgeCacheBytes(retained, state.shared.context);
				const size_t retainedBytes = checkedSizeAdd(previousBytes, bytes,
					"logical-plan Entity cache size overflowed");
				if (retainedBytes > MAXIMUM_MATERIALIZED_CACHE_BYTES)
					throw std::runtime_error("logical-plan Entity cache exceeded its byte bound");
				state.shared.materialized_cache_reservation.set(checkedSizeAdd(previousReservedBytes, bytes - sizeof(CachedRecord),
					"logical-plan Entity cache payload size overflowed"));
				if (!cache.emplace(id, std::move(retained)).second) {
					state.shared.materialized_cache_reservation.set(previousReservedBytes);
					return;
				}
				state.shared.materialized_cache_bytes = retainedBytes;
			} catch (...) {
				state.shared.materialized_cache_reservation.set(previousReservedBytes);
				throw;
			}
		}

		void cacheNodeRecord(ExecutionState &state, const storage::LogicalGraphNode &node) {
			cacheEntityRecord(state, state.shared.node_cache, node.node_id, node, logicalNodeCacheBytes(node, state.shared.context),
							  state.shared.bounds.maximum_node_records);
		}
		void cacheNodeRecord(ExecutionState &state, storage::LogicalGraphNode &&node) {
			const uint64_t id = node.node_id;
			const size_t bytes = logicalNodeCacheBytes(node, state.shared.context);
			cacheEntityRecord(state, state.shared.node_cache, id, std::move(node), bytes, state.shared.bounds.maximum_node_records);
		}

		void cacheEdgeRecord(ExecutionState &state, const storage::LogicalGraphEdge &edge) {
			cacheEntityRecord(state, state.shared.edge_cache, edge.edge_id, edge, logicalEdgeCacheBytes(edge, state.shared.context),
							  state.shared.bounds.maximum_edge_records);
		}
		void cacheEdgeRecord(ExecutionState &state, storage::LogicalGraphEdge &&edge) {
			const uint64_t id = edge.edge_id;
			const size_t bytes = logicalEdgeCacheBytes(edge, state.shared.context);
			cacheEntityRecord(state, state.shared.edge_cache, id, std::move(edge), bytes, state.shared.bounds.maximum_edge_records);
		}

		// Borrow complete cache records while preparing an independently owned output.
		// unordered_map rehash preserves references; callers finish using this borrow
		// before invoking downstream visitors.
		[[nodiscard]] const storage::LogicalGraphNode &readNodeCachedBorrowed(ExecutionState &state, uint64_t nodeId,
				std::optional<storage::LogicalGraphNode> &uncached, MaterializedByteReservation &uncachedBytes) {
			if (const auto cached = state.shared.node_cache.find(nodeId); cached != state.shared.node_cache.end()) return cached->second;
			storage::LogicalGraphNode node;
			if (state.shared.snapshot.supportsShapedPropertyReadAdmission()) {
				uncachedBytes.set(sizeof(storage::LogicalGraphNode));
				const std::array<uint64_t, 1U> ids{nodeId};
				auto records = state.shared.snapshot.readNodesShapedSortedWithAdmission(ids, completeNodeReadShape(), [&](size_t bytes) {
					checkGuardNow(state);
					uncachedBytes.set(checkedSizeAdd(uncachedBytes.bytes(), bytes, "logical-plan target source size overflowed"));
				});
				if (records.size() != 1U) throw std::runtime_error("logical graph backend changed typed Node batch cardinality");
				node = std::move(records.front());
			} else {
				node = state.shared.snapshot.readNodeShaped(nodeId, completeNodeReadShape());
			}
			validateNode(node, state);
			if (node.node_id != nodeId) throw std::runtime_error("logical graph shaped Node read changed logical identity");
			// Cache admission owns the moved payload; no clone is constructed here.
			uncachedBytes.reset();
			cacheNodeRecord(state, std::move(node));
			if (const auto cached = state.shared.node_cache.find(nodeId); cached != state.shared.node_cache.end()) return cached->second;
			uncachedBytes.set(logicalNodeCacheBytes(node, state.shared.context));
			uncached.emplace(std::move(node));
			return *uncached;
		}

		[[nodiscard]] storage::LogicalGraphNode readNodeCached(ExecutionState &state, uint64_t nodeId) {
			if (const auto cached = state.shared.node_cache.find(nodeId); cached != state.shared.node_cache.end()) {
				return detail::copyLogicalEntityGuarded(cached->second, [&] {
					if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
				});
			}
			storage::LogicalGraphNode node = state.shared.snapshot.readNodeShaped(nodeId, completeNodeReadShape());
			validateNode(node, state);
			if (node.node_id != nodeId) {
				throw std::runtime_error("logical graph shaped Node read changed logical identity");
			}
			cacheNodeRecord(state, node);
			return node;
		}

		[[nodiscard]] storage::LogicalGraphEdge readEdgeCached(ExecutionState &state, uint64_t edgeId) {
			if (const auto cached = state.shared.edge_cache.find(edgeId); cached != state.shared.edge_cache.end()) {
				return detail::copyLogicalEntityGuarded(cached->second, [&] {
					if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
				});
			}
			storage::LogicalGraphEdge edge = state.shared.snapshot.readEdgeShaped(edgeId, completeEdgeReadShape());
			validateEdge(edge);
			if (edge.edge_id != edgeId) {
				throw std::runtime_error("logical graph shaped Edge read changed logical identity");
			}
			cacheEdgeRecord(state, edge);
			return edge;
		}

		void validateBounds(const LogicalPlanReadRuntimeBounds &bounds, bool hasVisitor) {
			if (!hasVisitor || bounds.maximum_node_records == 0U || bounds.maximum_node_page_records == 0U ||
				bounds.maximum_metadata_pages_per_node_page == 0U || bounds.maximum_edges_per_node == 0U ||
				bounds.maximum_edge_records == 0U ||
				bounds.maximum_edge_records == std::numeric_limits<size_t>::max() ||
				bounds.maximum_adjacency_page_records == 0U || bounds.maximum_property_entries == 0U ||
				bounds.maximum_rows == 0U || bounds.maximum_distinct_rows == 0U ||
				bounds.maximum_materialized_bytes == 0U || bounds.maximum_candidate_bytes == 0U ||
				bounds.maximum_node_page_records > bounds.maximum_node_records ||
				bounds.maximum_adjacency_page_records > bounds.maximum_edges_per_node) {
				throw std::invalid_argument(
						"logical-plan read runtime requires positive compatible bounds and a visitor");
			}
		}

		void incrementBounded(size_t &value, const char *message) {
			if (value == std::numeric_limits<size_t>::max()) { // ZYX_COV_EXCL_LINE: bounded query resources cannot
															   // reach SIZE_MAX.
				throw std::overflow_error(message);
			}
			++value;
		}

		void validateAdjacentEdgeOrder(uint64_t edgeId, uint64_t &previousEdgeId, const char *context) {
			if (edgeId == 0U || (previousEdgeId != 0U && edgeId <= previousEdgeId)) {
				throw std::runtime_error(std::string("logical graph backend returned non-monotonic ") + context +
										 " adjacency Edge IDs");
			}
			previousEdgeId = edgeId;
		}

