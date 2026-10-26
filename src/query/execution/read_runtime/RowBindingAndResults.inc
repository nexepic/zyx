		[[nodiscard]] std::optional<std::pair<bool, uint64_t>>
		logicalEntityIdentity(const LogicalPlanReadValue &candidate) {
			if (const auto *node = std::get_if<storage::LogicalGraphNode>(&candidate)) {
				return std::pair<bool, uint64_t>{false, node->node_id};
			}
			if (const auto *edge = std::get_if<storage::LogicalGraphEdge>(&candidate)) {
				return std::pair<bool, uint64_t>{true, edge->edge_id};
			}
			if (const auto *reference = std::get_if<storage::EntityRef>(&candidate)) {
				return std::pair<bool, uint64_t>{reference->is_edge, reference->entity_id};
			}
			return std::nullopt;
		}

		[[nodiscard]] bool logicalValuesEqual(const LogicalPlanReadValue &left, const LogicalPlanReadValue &right) {
			const auto leftIdentity = logicalEntityIdentity(left);
			const auto rightIdentity = logicalEntityIdentity(right);
			if (leftIdentity.has_value() && rightIdentity.has_value()) {
				return *leftIdentity == *rightIdentity;
			}
			return left == right;
		}

		[[nodiscard]] bool bindValue(LogicalPlanReadRow &row, const std::string &variable, LogicalPlanReadValue value) {
			if (variable.empty()) {
				return true;
			}
			const auto found = row.values.find(variable);
			if (found == row.values.end()) {
				row.values.emplace(variable, std::move(value));
				return true;
			}
			// A deferred EntityRef and a materialized entity represent the same
			// logical binding.  Compare their stable identity rather than their
			// storage representation so repeated variables (notably self-loops)
			// retain Cypher's identity semantics across lazy materialization.
			return logicalValuesEqual(found->second, value);
		}

		// A bare variable projection keeps the same logical Entity binding under a
		// new name. Transfer selected-field and metadata caches with that binding so
		// a later Project, Sort, or Aggregate does not reread fields already resolved
		// in the same snapshot.
		void propagateProjectedEntityCaches(const expressions::Expression &expression, const LogicalPlanReadRow &input,
											LogicalPlanReadRow &projected, const std::string &alias, ExecutionState &state) {
			const auto *reference = dynamic_cast<const expressions::VariableReferenceExpression *>(&expression);
			if (reference == nullptr || reference->hasProperty()) {
				return;
			}
			const std::string &sourceVariable = reference->getVariableName();
			const auto sourceValue = input.values.find(sourceVariable);
			const auto projectedValue = projected.values.find(alias);
			if (sourceValue == input.values.end() || projectedValue == projected.values.end() ||
				!logicalValuesEqual(sourceValue->second, projectedValue->second)) {
				return;
			}
			if (!logicalEntityIdentity(sourceValue->second).has_value()) {
				return;
			}
			if (const auto properties = input.property_cache.find(sourceVariable);
				properties != input.property_cache.end()) {
				projected.property_cache.emplace(alias, detail::copyPropertyMapGuarded(properties->second, [&] { checkGuardNow(state); }));
				if (input.complete_entity_property_cache.contains(sourceVariable)) {
					projected.complete_entity_property_cache.insert(alias);
				}
			}
			if (const auto metadata = input.entity_metadata_cache.find(sourceVariable);
				metadata != input.entity_metadata_cache.end()) {
				projected.entity_metadata_cache.emplace(alias, detail::copyPropertyMapGuarded(metadata->second, [&] { checkGuardNow(state); }));
			}
		}


		// DISTINCT needs a structural key, not a display string: PropertyValue::toString()
		// intentionally permits human-readable ambiguity in nested lists and maps.
		template<typename String>
		void appendDistinctUint64(String &output, uint64_t value) {
			for (size_t byte = 0U; byte < sizeof(value); ++byte) {
				output += static_cast<char>(value & 0xFFU);
				value >>= 8U;
			}
		}

		template<typename String>
		void appendDistinctString(String &output, std::string_view value) {
			appendDistinctUint64(output, value.size());
			output.append(value);
		}

		template<typename String>
		void appendDistinctPropertyValue(String &output, const PropertyValue &value) {
			const auto appendType = [&output](PropertyType type) {
				output += static_cast<char>(static_cast<uint8_t>(type));
			};
			switch (value.getType()) {
				case PropertyType::NULL_TYPE:
					appendType(PropertyType::NULL_TYPE);
					return;
				case PropertyType::BOOLEAN:
					appendType(PropertyType::BOOLEAN);
					output += std::get<bool>(value.getVariant()) ? '\x01' : '\x00';
					return;
				case PropertyType::INTEGER:
					appendType(PropertyType::INTEGER);
					appendDistinctUint64(output, static_cast<uint64_t>(std::get<int64_t>(value.getVariant())));
					return;
				case PropertyType::DOUBLE: {
					appendType(PropertyType::DOUBLE);
					double number = std::get<double>(value.getVariant());
					// PropertyValue treats -0.0 and +0.0 as equal, so DISTINCT must too.
					if (number == 0.0) {
						number = 0.0;
					}
					appendDistinctUint64(output, std::bit_cast<uint64_t>(number));
					return;
				}
				case PropertyType::STRING:
					appendType(PropertyType::STRING);
					appendDistinctString(output, std::get<std::string>(value.getVariant()));
					return;
				case PropertyType::LIST: {
					appendType(PropertyType::LIST);
					const auto &list = value.getList();
					appendDistinctUint64(output, list.size());
					for (const PropertyValue &element: list) {
						appendDistinctPropertyValue(output, element);
					}
					return;
				}
				case PropertyType::MAP: {
					appendType(PropertyType::MAP);
					const auto &map = value.getMap();
					const auto scratchResource = [&]() -> std::pmr::memory_resource * {
						if constexpr (std::is_same_v<String, std::pmr::string>) return output.get_allocator().resource();
						else if constexpr (std::is_same_v<String, utils::OwnedStdString>) {
                            const auto owner = utils::OwnedAllocationScope::owner();
                            return owner ? owner.get() : std::pmr::new_delete_resource();
                        } else return std::pmr::new_delete_resource();
					};
					std::pmr::vector<const PropertyValue::MapType::value_type *> entries{scratchResource()};
					entries.reserve(map.size());
					for (const auto &entry: map) {
						entries.push_back(&entry);
					}
					std::sort(entries.begin(), entries.end(),
							  [](const auto *left, const auto *right) { return left->first < right->first; });
					appendDistinctUint64(output, entries.size());
					for (const auto *entry: entries) {
						appendDistinctString(output, entry->first);
						appendDistinctPropertyValue(output, entry->second);
					}
					return;
				}
				case PropertyType::DATE:
				case PropertyType::DATETIME:
				case PropertyType::DURATION:
					appendType(value.getType());
					appendDistinctString(output, value.toString());
					return;
				case PropertyType::BYTES: {
					appendType(PropertyType::BYTES);
					const auto &bytes = value.getBytes();
					appendDistinctUint64(output, bytes.size());
					for (const std::byte byte: bytes) {
						output += static_cast<char>(std::to_integer<uint8_t>(byte));
					}
					return;
				}
				case PropertyType::UNKNOWN: // ZYX_COV_EXCL_LINE: PropertyValue has no UNKNOWN or COMPOSITE variant.
				case PropertyType::COMPOSITE: // ZYX_COV_EXCL_LINE: PropertyValue has no UNKNOWN or COMPOSITE variant.
					throw std::runtime_error("logical-plan DISTINCT received an unsupported Property value");
			}
			throw std::runtime_error(
					"logical-plan DISTINCT received an unknown Property value"); // ZYX_COV_EXCL_LINE: the PropertyType
																				 // switch is exhaustive for
																				 // PropertyValue.
		}

		template<typename String>
		void appendDistinctValueKey(String &output, const LogicalPlanReadValue &value) {
			if (const PropertyValue *property = std::get_if<PropertyValue>(&value)) {
				output += 'P';
				appendDistinctPropertyValue(output, *property);
				return;
			}
			if (const storage::LogicalGraphNode *node = std::get_if<storage::LogicalGraphNode>(&value)) {
				output += 'N';
				appendDistinctUint64(output, node->node_id);
				return;
			}
			if (const storage::LogicalGraphEdge *edge = std::get_if<storage::LogicalGraphEdge>(&value)) {
				output += 'E';
				appendDistinctUint64(output, edge->edge_id);
				return;
			}
			const storage::EntityRef &reference = std::get<storage::EntityRef>(value);
			validateEntityRef(reference);
			output += reference.is_edge ? 'E' : 'N';
			appendDistinctUint64(output, reference.entity_id);
		}

		[[nodiscard]] std::pmr::string distinctRowKey(const LogicalPlanReadRow &row,
												 const std::vector<logical::LogicalProjectItem> &items, std::pmr::memory_resource *memory) {
			std::pmr::string result{memory};
			for (const logical::LogicalProjectItem &item: items) {
				const auto found = row.values.find(item.alias);
				if (found == row.values.end()) { // ZYX_COV_EXCL_LINE: Project validates aliases and emplaces every
												 // requested value before DISTINCT keys are built.
					throw std::runtime_error("logical-plan Project failed to bind an output alias");
				}
				appendDistinctString(result, item.alias);
				appendDistinctValueKey(result, found->second);
			}
			return result;
		}

		[[nodiscard]] std::vector<std::string> boundOutputNames(const logical::LogicalOperator &op, ExecutionState &state,
			MaterializedByteReservation &charge) {
			if (!state.shared.bound_program) return op.getOutputVariables(); // explicit IR adapter
			const auto &program = *state.shared.bound_program; program.validateSnapshot(state.shared.snapshot);
			const auto outputs = program.view(program.id(op)).declared_outputs;
			size_t bytes = checkedSizeMul(outputs.size(), sizeof(std::string), "bound output metadata overflowed");
			const auto inlineCapacity = std::string{}.capacity();
			for (const auto slot : outputs) {
				checkGuardNow(state); const auto name = program.name(slot);
				if (name.size() > inlineCapacity) bytes = checkedSizeAdd(bytes, name.size() + 1U, "bound output name overflowed");
			}
			charge.set(bytes);
			std::vector<std::string> names; names.reserve(outputs.size());
			for (const auto slot : outputs) { checkGuardNow(state); names.emplace_back(program.name(slot)); }
			bytes = checkedSizeMul(names.capacity(), sizeof(std::string), "bound output capacity overflowed");
			for (const auto &name : names) bytes = checkedSizeAdd(bytes, detail::dynamicStringBufferBytes(name), "bound output buffers overflowed");
			charge.set(bytes); return names;
		}

		[[nodiscard]] LogicalPlanReadRow normalizeOutputRow(LogicalPlanReadRow row,
															const std::vector<std::string> &columns) {
			LogicalPlanReadRow normalized;
			normalized.values.reserve(columns.size());
			normalized.property_cache.reserve(columns.size());
			normalized.path_cache.reserve(columns.size());
			// The caller transfers an owned row. Move container nodes, including
			// caches, so normalization does not duplicate large Property/path payloads.
			for (const std::string &column: columns) {
				if (normalized.values.contains(column))
					continue;
				const auto value = row.values.find(column);
				if (value == row.values.end()) {
					throw std::runtime_error("logical-plan runtime child did not bind a declared output variable");
				}
				normalized.values.insert(row.values.extract(value));
				if (const auto properties = row.property_cache.find(column); properties != row.property_cache.end()) {
					normalized.property_cache.insert(row.property_cache.extract(properties));
					if (row.complete_entity_property_cache.contains(column)) {
						normalized.complete_entity_property_cache.insert(
								row.complete_entity_property_cache.extract(column));
					}
				}
				if (const auto metadata = row.entity_metadata_cache.find(column);
					metadata != row.entity_metadata_cache.end()) {
					normalized.entity_metadata_cache.insert(row.entity_metadata_cache.extract(metadata));
				}
				if (const auto path = row.path_cache.find(column); path != row.path_cache.end()) {
					normalized.path_cache.insert(row.path_cache.extract(path));
				}
			}
			return normalized;
		}

		void mergeCompatibleRowCaches(LogicalPlanReadRow &destination, const LogicalPlanReadRow &source, ExecutionState &state,
									  const std::pmr::unordered_set<std::string_view> *excludedAliases = nullptr) {
			const auto checkpoint = [&] { checkGuardNow(state); };
			const auto includeAlias = [excludedAliases](const std::string &alias) {
				return excludedAliases == nullptr || !excludedAliases->contains(alias);
			};
			for (const auto &[alias, properties]: source.property_cache) {
				if (!includeAlias(alias)) {
					continue;
				}
				const auto existing = destination.property_cache.find(alias);
				if (existing != destination.property_cache.end() && existing->second != properties) {
					throw std::runtime_error(
							"logical-plan row combination received incompatible Property cache entries");
				}
				if (existing == destination.property_cache.end()) {
					destination.property_cache.emplace(alias, detail::copyPropertyMapGuarded(properties, checkpoint));
				}
				if (source.complete_entity_property_cache.contains(alias)) {
					destination.complete_entity_property_cache.insert(alias);
				}
			}
			for (const auto &[alias, metadata]: source.entity_metadata_cache) {
				if (!includeAlias(alias)) {
					continue;
				}
				auto &target = destination.entity_metadata_cache[alias];
				for (const auto &[key, value]: metadata) {
					checkpoint();
					const auto entry = target.find(key);
					if (entry == target.end()) {
						target.emplace(key, detail::copyPropertyValueGuarded(value, checkpoint));
					} else if (entry->second != value) {
						throw std::runtime_error(
								"logical-plan row combination received incompatible Entity metadata cache entries");
					}
				}
			}
			for (const auto &[alias, path]: source.path_cache) {
				if (!includeAlias(alias)) {
					continue;
				}
				checkpoint();
				const auto entry = destination.path_cache.find(alias);
				if (entry == destination.path_cache.end()) {
					destination.path_cache.emplace(alias, detail::copyLogicalReadPathGuarded(path, checkpoint));
				} else if (entry->second != path) {
					throw std::runtime_error("logical-plan row combination received incompatible path cache entries");
				}
			}
		}

		// The source is consumed, unlike reusable JOIN/OPTIONAL MATCH rows.
		void mergeOwnedRowCaches(LogicalPlanReadRow &destination, LogicalPlanReadRow source) {
			for (const auto &[alias, properties]: source.property_cache) {
				const auto found = destination.property_cache.find(alias);
				if (found != destination.property_cache.end() && found->second != properties) {
					throw std::runtime_error(
							"logical-plan row combination received incompatible Property cache entries");
				}
			}
			for (const auto &[alias, metadata]: source.entity_metadata_cache) {
				const auto found = destination.entity_metadata_cache.find(alias);
				if (found == destination.entity_metadata_cache.end())
					continue;
				for (const auto &[key, value]: metadata) {
					const auto entry = found->second.find(key);
					if (entry != found->second.end() && entry->second != value) {
						throw std::runtime_error(
								"logical-plan row combination received incompatible Entity metadata cache entries");
					}
				}
			}
			for (const auto &[alias, path]: source.path_cache) {
				const auto found = destination.path_cache.find(alias);
				if (found != destination.path_cache.end() && found->second != path) {
					throw std::runtime_error("logical-plan row combination received incompatible path cache entries");
				}
			}
			destination.property_cache.merge(source.property_cache);
			destination.complete_entity_property_cache.merge(source.complete_entity_property_cache);
			destination.path_cache.merge(source.path_cache);
			destination.entity_metadata_cache.merge(source.entity_metadata_cache);
			for (auto &[alias, metadata]: source.entity_metadata_cache) {
				destination.entity_metadata_cache.at(alias).merge(metadata);
			}
		}

        [[nodiscard]] utils::OwnedStringVector visibleOutputColumns(const logical::LogicalOperator &root) {
            const auto all = root.getOwnedOutputVariables();
            utils::OwnedStringVector columns;
            columns.reserve(all.size());
            for (const auto &column : all) if (!isAnonymousVariable(column)) columns.push_back(column);
            return columns;
        }

        // Metadata and the final standard-vector copy overlap during handoff.
        // Both have admission before copying; detach transfers the final copy's
        // existing parent credit without a refund/re-admission window.
        [[nodiscard]] std::optional<detail::ResourceLease> populateResultColumns(QueryResult &result,
                const logical::LogicalOperator &root, size_t maximumBytes, const QueryContext *context) {
            const bool managed = context && context->resource_account;
            auto memory = managed
                ? std::make_shared<MaterializedByteBudget>(maximumBytes, context->resource_account, std::pmr::new_delete_resource())
                : std::make_shared<MaterializedByteBudget>(maximumBytes);
            utils::OwnedAllocationScope scope(memory);
            const auto columns = visibleOutputColumns(root);
            if (columns.size() > (std::numeric_limits<size_t>::max)()/sizeof(std::string))
                throw std::overflow_error("result column backing overflowed");
            size_t copyUpper = utils::detail::addStandardHeapBytes(3U * utils::detail::standardContainerProxyBytes(),
                utils::detail::standardAllocatorRequestBytes(columns.size()*sizeof(std::string), alignof(std::string)));
            for (const auto &column : columns)
                copyUpper = utils::detail::addStandardHeapBytes(copyUpper, utils::detail::standardFreshStringHeapUpper(column.size()));
            MaterializedByteReservation copied(memory.get());
            copied.set(copyUpper);
            result.setColumns(static_cast<const std::vector<std::string> &>(columns));
            const auto retained = queryResultColumnStorageBytes(result.getColumns());
            // The fresh-copy model is audited for the supported standard STLs.
            if (retained > copyUpper) std::terminate();
            copied.set(retained);
            if (managed) return copied.detachToParent();
            return std::nullopt;
        }

		template<typename String = std::string>
		[[nodiscard]] String distinctUnionRowKey(const LogicalPlanReadRow &row,
													  const std::vector<std::string> &columns,
			std::pmr::memory_resource *memory = std::pmr::new_delete_resource()) {
			String result = [&] {
				if constexpr (std::is_same_v<String, std::pmr::string>) return String{memory};
				else return String{};
			}();
			for (const std::string &column: columns) {
				const auto value = row.values.find(column);
				if (value ==
					row.values.end()) { // normalizeOutputRow validates every declared output before this key is built.
					throw std::runtime_error("logical-plan UNION lost a declared output variable");
				}
				appendDistinctString(result, column);
				appendDistinctValueKey(result, value->second);
			}
			return result;
		}

		[[nodiscard]] std::pmr::string bindingKey(const LogicalPlanReadRow &row,
			std::span<const std::string *const> columns, std::pmr::memory_resource *memory) {
			std::pmr::string result{memory};
			for (const auto *name: columns) {
				const auto &column = *name;
				const auto value = row.values.find(column);
				if (value == row.values.end()) {
					throw std::runtime_error("logical-plan runtime child did not bind a declared join variable");
				}
				appendDistinctString(result, column);
				appendDistinctValueKey(result, value->second);
			}
			return result;
		}

		[[nodiscard]] AggregateFunctionType aggregateFunctionType(std::string_view name) {
			std::string normalized;
			normalized.reserve(name.size());
			for (const char character: name) {
				normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
			}
			if (normalized == "count") {
				return AggregateFunctionType::AGG_COUNT;
			}
			if (normalized == "sum") {
				return AggregateFunctionType::AGG_SUM;
			}
			if (normalized == "avg") {
				return AggregateFunctionType::AGG_AVG;
			}
			if (normalized == "min") {
				return AggregateFunctionType::AGG_MIN;
			}
			if (normalized == "max") {
				return AggregateFunctionType::AGG_MAX;
			}
			if (normalized == "collect") {
				return AggregateFunctionType::AGG_COLLECT;
			}
			if (normalized == "stdev") {
				return AggregateFunctionType::AGG_STDEV;
			}
			if (normalized == "stdevp") {
				return AggregateFunctionType::AGG_STDEVP;
			}
			if (normalized == "percentiledisc") {
				return AggregateFunctionType::AGG_PERCENTILE_DISC;
			}
			if (normalized == "percentilecont") {
				return AggregateFunctionType::AGG_PERCENTILE_CONT;
			}
			throw std::invalid_argument("logical-plan Aggregate has an unsupported function");
		}

		[[nodiscard]] double aggregatePercentile(const logical::LogicalAggItem &item, AggregateFunctionType function,
												 ExecutionState &state) {
			const bool isPercentile = function == AggregateFunctionType::AGG_PERCENTILE_DISC ||
									  function == AggregateFunctionType::AGG_PERCENTILE_CONT;
			if (!isPercentile) {
				if (item.extraArg) {
					throw std::invalid_argument("logical-plan Aggregate has an unexpected extra argument");
				}
				return 0.5;
			}
			if (!item.extraArg) {
				throw std::invalid_argument("logical-plan Aggregate percentile requires a percentile argument");
			}
			LogicalPlanReadRow emptyRow;
			const PropertyValue value = scalarValue(evaluateExpression(*item.extraArg, emptyRow, state));
			const int64_t *integer = std::get_if<int64_t>(&value.getVariant());
			const double *number = std::get_if<double>(&value.getVariant());
			const double percentile = integer != nullptr  ? static_cast<double>(*integer)
									  : number != nullptr ? *number
														  : std::numeric_limits<double>::quiet_NaN();
			if (!std::isfinite(percentile) || percentile < 0.0 || percentile > 1.0) {
				throw std::invalid_argument(
						"logical-plan Aggregate percentile must be a finite value between zero and one");
			}
			return percentile;
		}

		struct MaterializedEntityKey {
			uint64_t id;
			bool is_edge;
			bool operator==(const MaterializedEntityKey &) const = default;
		};
		struct MaterializedEntityKeyHash {
			size_t operator()(const MaterializedEntityKey &key) const noexcept {
				return std::hash<uint64_t>{}(key.id) ^
					(key.is_edge ? std::numeric_limits<size_t>::max() : 0U);
			}
		};
		using MaterializedPropertyCache =
			std::pmr::unordered_map<MaterializedEntityKey, PropertyMap, MaterializedEntityKeyHash>;
		using FinalRowVisitor = std::function<bool(LogicalPlanReadRow &, ExecutionState &)>;
		using BatchSelectionPredicate = std::function<bool(LogicalPlanReadRow &, ExecutionState &)>;

		template<typename Entity>
		[[nodiscard]] const PropertyMap *findMatchingRowProperties(const LogicalPlanReadRow &row,
																   const Entity &entity) {
			for (const auto &[alias, candidate]: row.values) {
				const auto *matchingEntity = std::get_if<Entity>(&candidate);
				if (matchingEntity == nullptr || *matchingEntity != entity)
					continue;
				if (const auto cached = row.property_cache.find(alias); cached != row.property_cache.end()) {
					return &cached->second;
				}
			}
			return nullptr;
		}

		template<typename Entity>
		[[nodiscard]] const PropertyMap &
		materializeEntityProperties(const LogicalPlanReadRow &row, const Entity &entity, ExecutionState &state,
									MaterializedPropertyCache &materialized,
									MaterializedByteReservation *sourceBytes) {
			static_assert(std::is_same_v<Entity, storage::LogicalGraphNode> ||
						  std::is_same_v<Entity, storage::LogicalGraphEdge>);
			const MaterializedEntityKey cacheKey = [&entity]() {
				if constexpr (std::is_same_v<Entity, storage::LogicalGraphNode>)
					return MaterializedEntityKey{entity.node_id, false};
				else
					return MaterializedEntityKey{entity.edge_id, true};
			}();
			if (const auto cached = materialized.find(cacheKey); cached != materialized.end()) {
				return cached->second;
			}
			if (const PropertyMap *cached = findMatchingRowProperties(row, entity)) {
				return *cached;
			}
			size_t predictedMapBytes = 0U;
			if (sourceBytes != nullptr) {
				if (entity.properties.size() > state.shared.bounds.maximum_property_entries) {
					throw std::runtime_error("logical graph backend returned too many direct Property entries");
				}
				constexpr const char *message = "logical-plan NamedPath Property cache size overflowed";
				size_t bytes = directPropertyMapStorageBytes(entity.properties, message, state.shared.context);
				bytes -= sizeof(PropertyMap);
				predictedMapBytes = bytes;
				const size_t nextBytes = checkedSizeAdd(sourceBytes->bytes(), bytes, message);
				if (sourceBytes->wouldExceed(nextBytes)) {
					throw std::runtime_error("logical-plan NamedPath Property cache exceeded its shared byte bound");
				}
				sourceBytes->set(nextBytes);
			}
			PropertyMap properties = [&] {
				if constexpr (std::is_same_v<Entity, storage::LogicalGraphNode>) {
					validateNode(entity, state);
					return readNodeProperties(state.shared.snapshot, entity, state);
				} else {
					validateEdge(entity);
					return readEdgeProperties(state.shared.snapshot, entity, state);
				}
			}();
			if (sourceBytes != nullptr) {
				const size_t actualMapBytes = propertyMapBytes(properties,
					"logical-plan NamedPath Property cache size overflowed", state.shared.context) - sizeof(PropertyMap);
				sourceBytes->set(checkedSizeAdd(sourceBytes->bytes() - predictedMapBytes, actualMapBytes,
					"logical-plan NamedPath Property cache size overflowed"));
			}
			return materialized.emplace(cacheKey, std::move(properties)).first->second;
		}

		[[nodiscard]] PropertyMap copyResultPropertyMap(const PropertyMap &properties, ExecutionState &state) {
			return detail::copyPropertyMapGuarded(properties, [&] {
				if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
			});
		}

		template<typename Entity>
		[[nodiscard]] PropertyMap materializeUniqueEntityProperties(const LogicalPlanReadRow &row, const Entity &entity,
																	ExecutionState &state) {
			static_assert(std::is_same_v<Entity, storage::LogicalGraphNode> ||
						  std::is_same_v<Entity, storage::LogicalGraphEdge>);
			if (const PropertyMap *cached = findMatchingRowProperties(row, entity))
				return copyResultPropertyMap(*cached, state);
			if constexpr (std::is_same_v<Entity, storage::LogicalGraphNode>) {
				validateNode(entity, state);
				return readNodeProperties(state.shared.snapshot, entity, state);
			} else {
				validateEdge(entity);
				return readEdgeProperties(state.shared.snapshot, entity, state);
			}
		}

		template<typename Entity>
		[[nodiscard]] PropertyMap materializeCompleteEntityProperties(const Entity &entity, ExecutionState &state) {
			static_assert(std::is_same_v<Entity, storage::LogicalGraphNode> ||
						  std::is_same_v<Entity, storage::LogicalGraphEdge>);
			PropertyMap properties;
			properties.reserve(entity.properties.size());
			for (const storage::LogicalGraphPropertyEntry &entry: entity.properties) {
				checkGuard(state);
				if (!properties.emplace(entry.key, detail::copyPropertyValueGuarded(entry.value, [&] {
					if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
				})).second) {
					throw std::runtime_error("logical-plan result received duplicate complete Entity properties");
				}
			}
			return properties;
		}

		[[nodiscard]] size_t completeResultPropertyBytes(const std::vector<storage::LogicalGraphPropertyEntry> &entries,
														 ExecutionState &state) {
			detail::BudgetMemoryResource keyMemory(state.shared.materialized_budget);
			std::pmr::unordered_set<std::string_view> keys(&keyMemory);
			keys.reserve(entries.size());
			size_t bytes = sizeof(PropertyMap);
			for (const auto &entry: entries) {
				checkGuard(state);
				if (!keys.insert(entry.key).second) {
					throw std::runtime_error("logical-plan result received duplicate complete Entity properties");
				}
				bytes = checkedSizeAdd(bytes, entry.key.size(), "logical-plan result row size overflowed");
				bytes = checkedSizeAdd(bytes, sizeof(PropertyValue), "logical-plan result row size overflowed");
				bytes = checkedSizeAdd(bytes, retainedExpressionValueBytes(entry.value, state),
									   "logical-plan result row size overflowed");
			}
			return bytes;
		}

		template<typename Entity>
		[[nodiscard]] size_t completeEntityResultBytes(const Entity &entity, ExecutionState &state) {
			static_assert(std::is_same_v<Entity, storage::LogicalGraphNode> ||
						  std::is_same_v<Entity, storage::LogicalGraphEdge>);
			if constexpr (std::is_same_v<Entity, storage::LogicalGraphNode>) {
				size_t bytes = checkedSizeAdd(sizeof(LogicalNodeResult),
				checkedSizeMul(entity.labels.capacity(), sizeof(std::string),
					"logical-plan result row size overflowed"), "logical-plan result row size overflowed");
				size_t labelsUntilGuard = 256U;
				for (const std::string &label: entity.labels) {
					if (--labelsUntilGuard == 0U) {
						if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
						labelsUntilGuard = 256U;
					}
					bytes = checkedSizeAdd(bytes, std::max(label.size(), detail::dynamicStringBufferBytes(label)), "logical-plan result row size overflowed");
				}
				return checkedSizeAdd(bytes, completeResultPropertyBytes(entity.properties, state),
									  "logical-plan result row size overflowed");
			} else {
				const size_t bytes = checkedSizeAdd(sizeof(LogicalEdgeResult), std::max(entity.type.size(), detail::dynamicStringBufferBytes(entity.type)),
													"logical-plan result row size overflowed");
				return checkedSizeAdd(bytes, completeResultPropertyBytes(entity.properties, state),
									  "logical-plan result row size overflowed");
			}
		}

		void admitCompleteEntityResult(size_t entityBytes, size_t rowPrefixBytes, size_t columnNameBytes,
									   ExecutionState &state, MaterializedByteReservation &reservation) {
			size_t nextBytes =
					checkedSizeAdd(rowPrefixBytes, columnNameBytes, "logical-plan result row size overflowed");
			nextBytes = checkedSizeAdd(nextBytes, sizeof(ResultValue), "logical-plan result row size overflowed");
			nextBytes = checkedSizeAdd(nextBytes, entityBytes, "logical-plan result row size overflowed");
			if (nextBytes > state.shared.bounds.maximum_materialized_bytes) {
				throw std::runtime_error("logical-plan result row exceeded its materialized byte bound");
			}
			reservation.set(nextBytes);
		}

		[[nodiscard]] ResultValue materializeResultValue(const LogicalPlanReadRow &row,
														 const LogicalPlanReadValue &value, ExecutionState &state,
														 MaterializedPropertyCache &materialized, size_t rowPrefixBytes,
														 size_t columnNameBytes, bool rowPreflighted,
														 bool repeatedEntity, MaterializedByteReservation &outputReservation,
			MaterializedByteReservation &propertyCacheReservation) {
			const auto copyCheckpoint = [&] {
				if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
			};
			if (!rowPreflighted && !std::holds_alternative<storage::EntityRef>(value)) {
				size_t predictedBytes;
				if (const auto *property = std::get_if<PropertyValue>(&value))
					predictedBytes = retainedExpressionValueBytes(*property, state);
				else if (const auto *node = std::get_if<storage::LogicalGraphNode>(&value))
					predictedBytes = completeEntityResultBytes(*node, state);
				else
					predictedBytes = completeEntityResultBytes(std::get<storage::LogicalGraphEdge>(value), state);
				admitCompleteEntityResult(predictedBytes, rowPrefixBytes, columnNameBytes, state, outputReservation);
			}
			if (const auto *property = std::get_if<PropertyValue>(&value)) {
				return ResultValue(detail::copyPropertyValueGuarded(*property, [&] {
					if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
				}));
			}
			// Borrow a prefetched complete record while constructing the public
			// result. Only a cache miss needs a local shaped read.
			if (const auto *reference = std::get_if<storage::EntityRef>(&value)) {
				validateEntityRef(*reference);
				if (reference->is_edge) {
					MaterializedByteReservation loadedBytes(&state.shared.materialized_budget);
					std::optional<storage::LogicalGraphEdge> loaded;
					const auto cached = state.shared.edge_cache.find(reference->entity_id);
					const storage::LogicalGraphEdge *edge = nullptr;
					if (cached != state.shared.edge_cache.end()) {
						edge = &cached->second;
					} else {
						loaded.emplace(readEdgeCached(state, reference->entity_id));
						edge = &*loaded;
					}
					validateEdgeReferenceRecord(*reference, *edge, state, "logical graph Entity reference");
					const std::optional<size_t> entityBytes =
							rowPreflighted ? std::nullopt
										   : std::optional<size_t>(completeEntityResultBytes(*edge, state));
					if (loaded)
						loadedBytes.set(logicalEdgeCacheBytes(*edge, state.shared.context));
					if (entityBytes)
						admitCompleteEntityResult(*entityBytes, rowPrefixBytes, columnNameBytes, state, outputReservation);
					PropertyMap properties = materializeCompleteEntityProperties(*edge, state);
					return ResultValue(LogicalEdgeResult{edge->edge_id, edge->source_node_id, edge->target_node_id,
														 detail::copyPropertyStringGuarded(edge->type, copyCheckpoint), std::move(properties)});
				}
				MaterializedByteReservation loadedBytes(&state.shared.materialized_budget);
				std::optional<storage::LogicalGraphNode> loaded;
				const auto cached = state.shared.node_cache.find(reference->entity_id);
				const storage::LogicalGraphNode *node = nullptr;
				if (cached != state.shared.node_cache.end()) {
					node = &cached->second;
				} else {
					loaded.emplace(readNodeCached(state, reference->entity_id));
					node = &*loaded;
				}
				const std::optional<size_t> entityBytes =
						rowPreflighted ? std::nullopt : std::optional<size_t>(completeEntityResultBytes(*node, state));
				if (loaded)
					loadedBytes.set(logicalNodeCacheBytes(*node, state.shared.context));
				if (entityBytes)
					admitCompleteEntityResult(*entityBytes, rowPrefixBytes, columnNameBytes, state, outputReservation);
				PropertyMap properties = materializeCompleteEntityProperties(*node, state);
				return ResultValue(LogicalNodeResult{node->node_id, detail::copyPropertyStringsGuarded(node->labels, copyCheckpoint), std::move(properties)});
			}
			if (const auto *node = std::get_if<storage::LogicalGraphNode>(&value)) {
				if (!repeatedEntity) {
					PropertyMap properties = materializeUniqueEntityProperties(row, *node, state);
					return ResultValue(LogicalNodeResult{node->node_id, detail::copyPropertyStringsGuarded(node->labels, copyCheckpoint), std::move(properties)});
				}
				const PropertyMap &properties = materializeEntityProperties(row, *node, state, materialized, &propertyCacheReservation);
				return ResultValue(LogicalNodeResult{node->node_id, detail::copyPropertyStringsGuarded(node->labels, copyCheckpoint), copyResultPropertyMap(properties, state)});
			}
			const storage::LogicalGraphEdge &edge = std::get<storage::LogicalGraphEdge>(value);
			if (!repeatedEntity) {
				PropertyMap properties = materializeUniqueEntityProperties(row, edge, state);
				return ResultValue(LogicalEdgeResult{edge.edge_id, edge.source_node_id, edge.target_node_id, detail::copyPropertyStringGuarded(edge.type, copyCheckpoint),
													 std::move(properties)});
			}
			const PropertyMap &properties = materializeEntityProperties(row, edge, state, materialized, &propertyCacheReservation);
			return ResultValue(
					LogicalEdgeResult{edge.edge_id, edge.source_node_id, edge.target_node_id, detail::copyPropertyStringGuarded(edge.type, copyCheckpoint), copyResultPropertyMap(properties, state)});
		}

		[[nodiscard]] size_t inlineResultPropertyBytes(const std::vector<storage::LogicalGraphPropertyEntry> &entries,
													   ExecutionState &state) {
			if (entries.size() > state.shared.bounds.maximum_property_entries) {
				throw std::runtime_error("logical graph backend returned too many direct Property entries");
			}
			detail::BudgetMemoryResource keyMemory(state.shared.materialized_budget);
			std::pmr::unordered_set<std::string_view> keys(&keyMemory);
			keys.reserve(entries.size());
			size_t bytes = sizeof(PropertyMap);
			for (const auto &entry: entries) {
				checkGuard(state);
				if (entry.key.empty() || !keys.insert(entry.key).second) {
					throw std::runtime_error("logical graph backend returned an invalid direct Property entry set");
				}
				bytes = checkedSizeAdd(bytes, entry.key.size(), "logical-plan result row size overflowed");
				bytes = checkedSizeAdd(bytes, sizeof(PropertyValue), "logical-plan result row size overflowed");
				bytes = checkedSizeAdd(bytes, retainedExpressionValueBytes(entry.value, state),
									   "logical-plan result row size overflowed");
			}
			return bytes;
		}

		[[nodiscard]] size_t predictableEntityResultBytes(const LogicalPlanReadRow &row,
														  const storage::LogicalGraphNode &node,
														  ExecutionState &state) {
			const PropertyMap *cached = findMatchingRowProperties(row, node);
			if (cached == nullptr)
				validateNode(node, state);
			size_t bytes = checkedSizeAdd(sizeof(LogicalNodeResult),
				checkedSizeMul(node.labels.capacity(), sizeof(std::string),
					"logical-plan result row size overflowed"), "logical-plan result row size overflowed");
			size_t labelsUntilGuard = 256U;
			for (const std::string &label: node.labels) {
				if (--labelsUntilGuard == 0U) {
					if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
					labelsUntilGuard = 256U;
				}
				bytes = checkedSizeAdd(bytes, std::max(label.size(), detail::dynamicStringBufferBytes(label)), "logical-plan result row size overflowed");
			}
			return checkedSizeAdd(bytes,
								  cached != nullptr
										  ? propertyMapBytes(*cached, "logical-plan result row size overflowed", state.shared.context)
										  : inlineResultPropertyBytes(node.properties, state),
								  "logical-plan result row size overflowed");
		}

		[[nodiscard]] size_t predictableEntityResultBytes(const LogicalPlanReadRow &row,
														  const storage::LogicalGraphEdge &edge,
														  ExecutionState &state) {
			const PropertyMap *cached = findMatchingRowProperties(row, edge);
			if (cached == nullptr)
				validateEdge(edge);
			const size_t bytes = checkedSizeAdd(sizeof(LogicalEdgeResult), std::max(edge.type.size(), detail::dynamicStringBufferBytes(edge.type)),
												"logical-plan result row size overflowed");
			return checkedSizeAdd(bytes,
								  cached != nullptr
										  ? propertyMapBytes(*cached, "logical-plan result row size overflowed", state.shared.context)
										  : inlineResultPropertyBytes(edge.properties, state),
								  "logical-plan result row size overflowed");
		}

		[[nodiscard]] std::optional<size_t> predictableReferenceResultBytes(const storage::EntityRef &reference,
																			ExecutionState &state) {
			validateEntityRef(reference);
			if (reference.is_edge) {
				const auto cached = state.shared.edge_cache.find(reference.entity_id);
				if (cached == state.shared.edge_cache.end())
					return std::nullopt;
				const auto &edge = cached->second;
				validateEdgeReferenceRecord(reference, edge, state, "logical graph Entity reference");
				return completeEntityResultBytes(edge, state);
			}
			const auto cached = state.shared.node_cache.find(reference.entity_id);
			if (cached == state.shared.node_cache.end())
				return std::nullopt;
			const auto &node = cached->second;
			validateNode(node, state);
			return completeEntityResultBytes(node, state);
		}

		[[nodiscard]] std::optional<std::pair<bool, uint64_t>> resultEntityIdentity(const LogicalPlanReadValue &value) {
			if (const auto *reference = std::get_if<storage::EntityRef>(&value)) {
				return std::pair{reference->is_edge, reference->entity_id};
			}
			if (const auto *node = std::get_if<storage::LogicalGraphNode>(&value)) {
				return std::pair{false, node->node_id};
			}
			if (const auto *edge = std::get_if<storage::LogicalGraphEdge>(&value)) {
				return std::pair{true, edge->edge_id};
			}
			return std::nullopt;
		}

		void appendLogicalResultRow(QueryResult &result, LogicalPlanReadRow &row,
								const std::vector<std::string> &columns, ExecutionState &state) {
			// Scalars and unique Entities whose complete records are already in
			// memory have an exact logical result size before payloads are copied.
			size_t predictableBytes = sizeof(QueryResult::Row);
			bool predictable = true;
			for (size_t columnIndex = 0U; columnIndex < columns.size(); ++columnIndex) {
				const std::string &column = columns[columnIndex];
				const auto found = row.values.find(column);
				if (found == row.values.end()) {
					throw std::runtime_error("logical-plan runtime did not bind a declared output variable");
				}
				predictableBytes =
						checkedSizeAdd(predictableBytes, column.size(), "logical-plan result row size overflowed");
				predictableBytes = checkedSizeAdd(predictableBytes, sizeof(ResultValue),
												  "logical-plan result row size overflowed");
				if (const auto *property = std::get_if<PropertyValue>(&found->second)) {
					predictableBytes = checkedSizeAdd(predictableBytes, retainedExpressionValueBytes(*property, state),
													  "logical-plan result row size overflowed");
					continue;
				}
				const auto identity = resultEntityIdentity(found->second);
				for (size_t previous = 0U; previous < columnIndex; ++previous) {
					const auto prior = row.values.find(columns[previous]);
					if (prior == row.values.end())
						continue;
					if (identity == resultEntityIdentity(prior->second)) {
						predictable = false;
						break;
					}
				}
				if (!predictable)
					break;
				std::optional<size_t> entityBytes;
				if (const auto *reference = std::get_if<storage::EntityRef>(&found->second)) {
					entityBytes = predictableReferenceResultBytes(*reference, state);
				} else if (const auto *node = std::get_if<storage::LogicalGraphNode>(&found->second)) {
					entityBytes = predictableEntityResultBytes(row, *node, state);
				} else {
					entityBytes = predictableEntityResultBytes(row, std::get<storage::LogicalGraphEdge>(found->second),
															   state);
				}
				if (!entityBytes) {
					predictable = false;
					break;
				}
				predictableBytes =
						checkedSizeAdd(predictableBytes, *entityBytes, "logical-plan result row size overflowed");
			}
			MaterializedByteReservation outputReservation(&state.shared.materialized_budget);
			if (predictable) {
				if (predictableBytes > state.shared.bounds.maximum_materialized_bytes) {
					throw std::runtime_error("logical-plan result row exceeded its materialized byte bound");
				}
				if (predictableBytes > state.shared.materialized_budget.available()) {
					throw std::runtime_error("shared materialization exceeded its byte bound");
				}
				outputReservation.set(predictableBytes);
			}
			MaterializedByteReservation propertyCacheReservation(&state.shared.materialized_budget);
			QueryResult::Row output;
			output.reserve(columns.size());
			detail::BudgetMemoryResource propertyCacheMemory(state.shared.materialized_budget);
			MaterializedPropertyCache materialized(&propertyCacheMemory);
			size_t constructedBytes = sizeof(QueryResult::Row);
			for (const std::string &column: columns) {
				const auto found = row.values.find(column);
				if (found == row.values.end()) {
					throw std::runtime_error("logical-plan runtime did not bind a declared output variable");
				}
				bool repeatedEntity = false;
				if (const auto identity = resultEntityIdentity(found->second)) {
					for (const std::string &otherColumn: columns) {
						if (&otherColumn == &column)
							continue;
						const auto other = row.values.find(otherColumn);
						if (other != row.values.end() && resultEntityIdentity(other->second) == identity) {
							repeatedEntity = true;
							break;
						}
					}
				}
				ResultValue value = materializeResultValue(row, found->second, state, materialized, constructedBytes,
														   column.size(), predictable, repeatedEntity, outputReservation, propertyCacheReservation);
				const size_t valueBytes = resultValueBytes(value, state.shared.context);
				if (output.emplace(column, std::move(value)).second) {
					constructedBytes =
							checkedSizeAdd(constructedBytes, column.size(), "logical-plan result row size overflowed");
					constructedBytes = checkedSizeAdd(constructedBytes, sizeof(ResultValue),
													  "logical-plan result row size overflowed");
					constructedBytes =
							checkedSizeAdd(constructedBytes, valueBytes, "logical-plan result row size overflowed");
				}
				outputReservation.set(predictable ? std::max(predictableBytes, constructedBytes) : constructedBytes);
			}
			appendMaterializedResultRow(result, std::move(output), state, std::move(outputReservation));
		}

		void materializeRowEntityRefs(LogicalPlanReadRow &row, ExecutionState &state) {
			std::vector<storage::EntityRef> nodeReferences;
			std::vector<storage::EntityRef> edgeReferences;
			for (const auto &[_, value]: row.values) {
				if (const auto *reference = std::get_if<storage::EntityRef>(&value)) {
					validateEntityRef(*reference);
					(reference->is_edge ? edgeReferences : nodeReferences).push_back(*reference);
				}
			}
			const std::vector<storage::LogicalGraphNode> nodes = materializeNodeReferences(nodeReferences, state);
			const std::vector<storage::LogicalGraphEdge> edges = materializeEdgeReferences(edgeReferences, state);
			size_t nodeIndex = 0U;
			size_t edgeIndex = 0U;
			for (auto &[variable, value]: row.values) {
				if (const auto *reference = std::get_if<storage::EntityRef>(&value)) {
					if (reference->is_edge) {
						value = edges[edgeIndex++];
					} else {
						value = nodes[nodeIndex++];
					}
					row.property_cache.erase(variable);
					row.complete_entity_property_cache.erase(variable);
				}
			}
		}

		void appendUniquePrefetchReference(std::vector<storage::EntityRef> &references,
										   std::unordered_set<uint64_t> &ids, const storage::EntityRef &reference,
										   MaterializedByteReservation &referenceSlots,
										   MaterializedByteReservation &idStorage) {
			if (ids.contains(reference.entity_id))
				return;
			constexpr const char *message = "logical-plan reference prefetch scratch size overflowed";
			constexpr size_t idNodeBytes = sizeof(uint64_t) + 2U * sizeof(void *);
			const auto admitScratch = [](MaterializedByteReservation &reservation, size_t nextBytes) {
				if (reservation.wouldExceed(nextBytes)) {
					throw std::runtime_error("logical-plan reference prefetch scratch exceeded its shared byte bound");
				}
				reservation.set(nextBytes);
			};
			if (references.size() == references.capacity()) {
				const size_t nextCapacity =
						references.capacity() == 0U ? 1U : checkedSizeMul(references.capacity(), 2U, message);
				const size_t previousSlots = referenceSlots.bytes();
				const size_t nextSlots = checkedSizeMul(nextCapacity, sizeof(storage::EntityRef), message);
				admitScratch(referenceSlots, checkedSizeAdd(previousSlots, nextSlots, message));
				try {
					references.reserve(nextCapacity);
				} catch (...) {
					referenceSlots.set(previousSlots);
					throw;
				}
				admitScratch(referenceSlots,
							 checkedSizeMul(references.capacity(), sizeof(storage::EntityRef), message));
				const size_t previousIds = idStorage.bytes();
				const size_t plannedBuckets = checkedSizeMul(nextCapacity, 2U, message);
				admitScratch(
						idStorage,
						checkedSizeAdd(previousIds, checkedSizeMul(plannedBuckets, sizeof(void *), message), message));
				try {
					ids.reserve(nextCapacity);
				} catch (...) {
					idStorage.set(previousIds);
					throw;
				}
				admitScratch(idStorage,
							 checkedSizeAdd(checkedSizeMul(ids.size(), idNodeBytes, message),
											checkedSizeMul(ids.bucket_count(), sizeof(void *), message), message));
			}
			const size_t previousIds = idStorage.bytes();
			admitScratch(idStorage, checkedSizeAdd(previousIds, idNodeBytes, message));
			try {
				if (!ids.insert(reference.entity_id).second) {
					idStorage.set(previousIds);
					return;
				}
			} catch (...) {
				idStorage.set(previousIds);
				throw;
			}
			references.push_back(reference);
		}

		template<typename T>
		void reserveNamedPathSlots(std::vector<T> &values, size_t capacity, MaterializedByteReservation &slots) {
			if (capacity <= values.capacity())
				return;
			constexpr const char *message = "logical-plan NamedPath scratch capacity overflowed";
			const size_t previous = slots.bytes();
			const size_t peakBytes = checkedSizeAdd(previous, checkedSizeMul(capacity, sizeof(T), message), message);
			slots.set(peakBytes);
			try {
				values.reserve(capacity);
			} catch (...) {
				slots.set(previous);
				throw;
			}
			slots.set(checkedSizeMul(values.capacity(), sizeof(T), message));
		}

		void appendNamedPathReference(std::vector<storage::EntityRef> &references, const storage::EntityRef &reference,
									  size_t maximumCount, MaterializedByteReservation &slots) {
			if (references.size() >= maximumCount) {
				throw std::runtime_error("logical-plan NamedPath exceeded its deferred reference bound");
			}
			if (references.size() == references.capacity()) {
				const size_t nextCapacity = references.capacity() == 0U ? 1U
																		: (references.capacity() > maximumCount / 2U
																				   ? maximumCount
																				   : references.capacity() * 2U);
				reserveNamedPathSlots(references, nextCapacity, slots);
			}
			references.push_back(reference);
		}

		void prefetchEntityRefsInRows(std::span<const LogicalPlanReadRow> rows, ExecutionState &state,
									  const bool includePathReferences = false) {
			MaterializedByteReservation nodeReferenceSlots(&state.shared.materialized_budget);
			MaterializedByteReservation edgeReferenceSlots(&state.shared.materialized_budget);
			MaterializedByteReservation nodeIdStorage(&state.shared.materialized_budget);
			MaterializedByteReservation edgeIdStorage(&state.shared.materialized_budget);
			std::vector<storage::EntityRef> nodeReferences;
			std::vector<storage::EntityRef> edgeReferences;
			std::unordered_set<uint64_t> nodeIds;
			std::unordered_set<uint64_t> edgeIds;
			const auto collectReference = [&](const storage::EntityRef &reference, const bool allowZeroNode) {
				if (reference.entity_id == 0U && allowZeroNode) {
					return;
				}
				validateEntityRef(reference);
				if (reference.is_edge) {
					appendUniquePrefetchReference(edgeReferences, edgeIds, reference, edgeReferenceSlots,
												  edgeIdStorage);
				} else {
					appendUniquePrefetchReference(nodeReferences, nodeIds, reference, nodeReferenceSlots,
												  nodeIdStorage);
				}
			};
			for (const LogicalPlanReadRow &row: rows) {
				for (const auto &[_, value]: row.values) {
					const auto *reference = std::get_if<storage::EntityRef>(&value);
					if (reference == nullptr) {
						continue;
					}
					collectReference(*reference, false);
				}
				if (includePathReferences) {
					for (const auto &[_, path]: row.path_cache) {
						for (const storage::EntityRef &reference: path.node_refs) {
							// Variable-length paths use a zero Node reference as an explicit
							// marker for an already materialized endpoint.
							collectReference(reference, true);
						}
						for (const storage::EntityRef &reference: path.edge_refs) {
							collectReference(reference, false);
						}
					}
				}
			}
			std::ranges::sort(nodeReferences, {}, &storage::EntityRef::entity_id);
			std::ranges::sort(edgeReferences, {}, &storage::EntityRef::entity_id);
			static_cast<void>(materializeNodeReferences(nodeReferences, state, false));
			static_cast<void>(materializeEdgeReferences(edgeReferences, state, false));
		}

		void materializeEntityRefsInRows(std::span<LogicalPlanReadRow> rows, ExecutionState &state,
			MaterializedByteReservation &outputBytes) {
			prefetchEntityRefsInRows(rows, state);
			for (LogicalPlanReadRow &row: rows) {
				for (auto &[variable, value]: row.values) {
					const auto *referenceValue = std::get_if<storage::EntityRef>(&value);
					if (referenceValue == nullptr) continue;
					const storage::EntityRef reference = *referenceValue;
					// A declined optional cache entry is not an absent entity. Keep
					// fallback source ownership live, and admit the independently
					// owned output payload before copying it into the buffered row.
					const auto copyRecord = [&](const auto &record) {
						using Record = std::decay_t<decltype(record)>;
						const size_t bytes = [&] {
							if constexpr (std::is_same_v<Record, storage::LogicalGraphNode>)
								return logicalNodeCacheBytes(record, state.shared.context);
							else return logicalEdgeCacheBytes(record, state.shared.context);
						}();
						const size_t previous = logicalReadValueBytes(value, state.shared.context);
						if (previous > outputBytes.bytes())
							throw std::logic_error("logical-plan output reference charge is incomplete");
						outputBytes.set(checkedSizeAdd(outputBytes.bytes() - previous, bytes,
							"logical-plan owned output entity size overflowed"));
						value = detail::copyLogicalEntityGuarded(record, [&] { checkGuardNow(state); });
					};
					if (reference.is_edge) {
						const auto edge = state.shared.edge_cache.find(reference.entity_id);
						if (edge != state.shared.edge_cache.end()) {
							validateEdgeReferenceRecord(reference, edge->second, state,
								"logical graph batched Entity reference");
							copyRecord(edge->second);
						} else {
							MaterializedByteReservation sourceBytes(&state.shared.materialized_budget);
							const std::array<storage::EntityRef, 1U> references{reference};
							auto records = materializeEdgeReferences(references, state, true, &sourceBytes);
							validateEdgeReferenceRecord(reference, records.front(), state,
								"logical graph uncached batched Entity reference");
							copyRecord(records.front());
						}
					} else {
						MaterializedByteReservation sourceBytes(&state.shared.materialized_budget);
						std::optional<storage::LogicalGraphNode> uncached;
						const auto &node = readNodeCachedBorrowed(state, reference.entity_id, uncached, sourceBytes);
						validateNode(node, state);
						copyRecord(node);
					}
					row.property_cache.erase(variable);
					row.complete_entity_property_cache.erase(variable);
				}
			}
		}

		template<typename Invoke>
		[[nodiscard]] bool invokeOutputVisitorChecked(ExecutionState &state, Invoke &&invoke) {
			checkGuardNow(state);
			const auto fence = state.shared.snapshot.mutationFence();
			const bool keepGoing = invoke();
			// The last callback (including an early stop) may have no subsequent source checkpoint.
			checkGuardNow(state);
			const auto currentFence = state.shared.snapshot.mutationFence();
			if (fence.has_value() && currentFence != fence)
				throw std::runtime_error("logical-plan snapshot changed during output callback");
			return keepGoing;
		}

		template<typename Run, typename Emit>
		[[nodiscard]] LogicalPlanReadRuntimeStats
		executeWithBatchedOutputMaterialization(const LogicalPlanReadRuntimeBounds &bounds, Run &&run, Emit &&emit,
												const bool preserveVisitorEarlyStop = false,
												const bool replaceDeferredEntityRefs = true,
                                                const QueryContext *allocationContext = nullptr) {
			// A public row visitor is allowed to stop the producer after the first
			// emitted row. Keep that contract strict by flushing one row at a time;
			// QueryResult materialization has no such callback and can use larger
			// cross-row batches safely.
			// Keep reserved row slots to at most 1/16 of the byte allowance where
			// possible, leaving most of the budget for the rows and source payloads.
			constexpr size_t outputBatchSlotBudgetDivisor = 16U;
            const size_t allocationAllowance = allocationContext && allocationContext->resource_account
                ? std::min(bounds.maximum_materialized_bytes, allocationContext->resource_account->available())
                : bounds.maximum_materialized_bytes;
			const size_t slotBytes = checkedSizeMul(sizeof(LogicalPlanReadRow), outputBatchSlotBudgetDivisor,
													"logical-plan output batch slot bound overflowed");
			const size_t maximumOutputSlots =
					allocationAllowance < sizeof(LogicalPlanReadRow)
							? 0U
							: std::max<size_t>(1U, allocationAllowance / slotBytes);
			const size_t batchCapacity =
					preserveVisitorEarlyStop
							? std::min<size_t>(1U, maximumOutputSlots)
							: std::min({DEFAULT_PROPERTY_BATCH_ROWS, bounds.maximum_rows, maximumOutputSlots});
			if (batchCapacity == 0U) {
                // Private compiled nodes can legitimately hold part of the same
                // QUERY account. Report parent pressure as admission denial,
                // preserving the distinction from an invalid local row bound.
                if (bounds.maximum_rows != 0U && bounds.maximum_materialized_bytes >= sizeof(LogicalPlanReadRow) &&
                    allocationContext && allocationContext->resource_account && allocationAllowance < sizeof(LogicalPlanReadRow))
                    throw detail::ResourceAdmissionDenied();
				throw std::runtime_error("logical-plan output materialization exceeds its row or byte bound");
			}
			// The output batch is live while upstream sources (including LOAD CSV)
			// continue reading. Charge it to their shared execution budget.
            // Account actual allocator requests before allocating, including
            // unused output slots. The execution budget separately excludes this
            // resident capacity from its local allowance, without a second parent charge.
            std::optional<MaterializedByteBudget> outputStorage;
            if (allocationContext && allocationContext->resource_account)
                outputStorage.emplace(allocationAllowance, allocationContext->resource_account);
            MaterializedByteReservation bufferedReservation;
            bool budgetBound = false;
            std::pmr::vector<LogicalPlanReadRow> rows(outputStorage
                ? static_cast<std::pmr::memory_resource *>(&*outputStorage) : std::pmr::get_default_resource());
			rows.reserve(batchCapacity);
			const size_t residentOutputBatchBytes = checkedSizeMul(rows.capacity(), sizeof(LogicalPlanReadRow),
																   "logical-plan output batch storage size overflowed");
			if (residentOutputBatchBytes >= bounds.maximum_materialized_bytes) {
				throw std::invalid_argument("logical-plan output batch storage exhausts its materialized byte bound");
			}
			size_t bufferedBytes = 0U;
			bool stoppedByVisitor = false;
			bool finalized = false;
			// The vector's full row-slot capacity is already reserved above. Charge
			// only the row payload here to avoid counting each active object twice.
			const auto outputRowPayloadBytes = [](const LogicalPlanReadRow &row, const ExecutionState &state) {
				return logicalReadRowBytes(row, state.shared.context) - sizeof(LogicalPlanReadRow);
			};
			const auto flush = [&](ExecutionState &state) {
				if (rows.empty() || stoppedByVisitor) {
					return !stoppedByVisitor;
				}
				if (replaceDeferredEntityRefs) {
					materializeEntityRefsInRows(rows, state, bufferedReservation);
				} else {
					prefetchEntityRefsInRows(rows, state);
				}
				size_t materializedBytes = 0U;
				for (const LogicalPlanReadRow &row: rows) {
					materializedBytes = checkedSizeAdd(materializedBytes, outputRowPayloadBytes(row, state),
													   "logical-plan output batch materialization size overflowed");
				}
				bufferedReservation.set(materializedBytes);
				bufferedBytes = materializedBytes;
				for (LogicalPlanReadRow &row: rows) {
					enforceMaterializedRowBound(row, state);
					const bool keepGoing = preserveVisitorEarlyStop
						? invokeOutputVisitorChecked(state, [&] { return emit(row, state); })
						: emit(row, state);
					if (!keepGoing) {
						stoppedByVisitor = true;
						rows.clear();
						bufferedReservation.reset();
						bufferedBytes = 0U;
						return false;
					}
					const size_t releasedBytes = outputRowPayloadBytes(row, state);
					row = {};
					bufferedReservation.set(bufferedReservation.bytes() - releasedBytes);
				}
				rows.clear();
				bufferedReservation.reset();
				bufferedBytes = 0U;
				return true;
			};
			const FinalRowVisitor collect = [&](LogicalPlanReadRow &row, ExecutionState &state) {
				if (!budgetBound) {
					bufferedReservation = MaterializedByteReservation(&state.shared.materialized_budget);
					budgetBound = true;
				}
				const size_t rowBytes = outputRowPayloadBytes(row, state);
				if (!rows.empty() && (rows.size() == batchCapacity ||
									  rowBytes > bounds.maximum_materialized_bytes -
														 std::min(bufferedBytes, bounds.maximum_materialized_bytes) ||
									  rowBytes > state.shared.materialized_budget.available())) {
					if (!flush(state)) {
						return false;
					}
				}
				const size_t nextBytes = checkedSizeAdd(bufferedBytes, rowBytes,
														"logical-plan output batch materialization size overflowed");
				bufferedReservation.set(nextBytes);
				try {
					rows.push_back(std::move(row));
				} catch (...) {
					bufferedReservation.set(bufferedBytes);
					throw;
				}
				bufferedBytes = nextBytes;
				if (rows.size() == batchCapacity) {
					return flush(state);
				}
				return true;
			};
			const ExecutionFinalizer finalize = [&](ExecutionState &state, bool failed) {
				if (!finalized) {
					finalized = true;
					const auto release = [&] {
						std::pmr::vector<LogicalPlanReadRow>(rows.get_allocator().resource()).swap(rows);
						// Detach while the ExecutionState budget is still alive, including zero-charge owners.
						bufferedReservation = MaterializedByteReservation{};
					};
					try {
						if (!failed && !stoppedByVisitor) static_cast<void>(flush(state));
					} catch (...) {
						release();
						throw;
					}
					release();
				}
			};
			const LogicalPlanReadRuntimeStats stats = run(collect, finalize, residentOutputBatchBytes);
			if (!finalized) {
				throw std::logic_error("logical-plan output materialization run did not finalize its execution state");
			}
			return stats;
		}

