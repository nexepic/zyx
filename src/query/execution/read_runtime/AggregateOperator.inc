				case logical::LogicalOpType::LOP_AGGREGATE: {
					const auto &aggregate = dynamic_cast<const logical::LogicalAggregate &>(op);
					if (const std::optional<bool> direct = tryVisitDirectNodeCount(aggregate, state, visitor);
						direct.has_value()) {
						return *direct;
					}
					if (const std::optional<bool> variableLength =
								tryVisitBatchedVariableLengthCount(aggregate, state, visitor);
						variableLength.has_value()) {
						return *variableLength;
					}
					if (const std::optional<bool> twoHop =
								tryVisitAnchoredTwoHopRelationshipCount(aggregate, state, visitor);
						twoHop.has_value()) {
						return *twoHop;
					}
					if (const std::optional<bool> anchored =
								tryVisitAnchoredRelationshipCount(aggregate, state, visitor);
						anchored.has_value()) {
						return *anchored;
					}
					if (const std::optional<bool> direct = tryVisitDirectRelationshipCount(aggregate, state, visitor);
						direct.has_value()) {
						return *direct;
					}
					const auto &groupExpressions = aggregate.getGroupByExprs();
					const auto &groupAliases = aggregate.getGroupByAliases();
					const auto &aggregations = aggregate.getAggregations();
					std::optional<detail::BoundExpressionInputs> boundInputs;
					if (state.shared.bound_program) {
						detail::BudgetMemoryResource bindingMemory(state.shared.materialized_budget);
						std::pmr::vector<const expressions::Expression *> expressionInputs(&bindingMemory);
						for (const auto &expression : groupExpressions) expressionInputs.push_back(expression.get());
						for (const auto &item : aggregations) expressionInputs.push_back(item.argument.get());
						boundInputs.emplace(*state.shared.bound_program, state.shared.bound_program->id(op), expressionInputs,
							state.shared.snapshot, state.shared.materialized_budget, [&] { checkGuardNow(state); }, std::pmr::get_default_resource(), state.shared.context);
					}
					if (aggregations.empty() || groupAliases.size() > groupExpressions.size()) {
						throw std::invalid_argument("logical-plan Aggregate has an invalid shape");
					}
					std::vector<std::string> outputAliases;
					outputAliases.reserve(groupExpressions.size() + aggregations.size());
					for (size_t index = 0U; index < groupExpressions.size(); ++index) {
						if (!groupExpressions[index]) {
							throw std::invalid_argument("logical-plan Aggregate has an empty group expression");
						}
						const std::string alias = index < groupAliases.size() && !groupAliases[index].empty()
														  ? groupAliases[index]
														  : groupExpressions[index]->toString();
						if (alias.empty()) {
							throw std::invalid_argument("logical-plan Aggregate has an empty group output alias");
						}
						outputAliases.push_back(alias);
					}

					std::vector<AggregateFunctionType> functions;
					std::vector<double> percentiles;
					functions.reserve(aggregations.size());
					percentiles.reserve(aggregations.size());
					for (const logical::LogicalAggItem &item: aggregations) {
						const AggregateFunctionType function = aggregateFunctionType(item.functionName);
						if (item.alias.empty() || std::ranges::find(outputAliases, item.alias) != outputAliases.end() ||
							(function != AggregateFunctionType::AGG_COUNT && !item.argument) ||
							(function == AggregateFunctionType::AGG_COUNT && !item.argument && item.distinct)) {
							throw std::invalid_argument("logical-plan Aggregate has an invalid aggregate item");
						}
						outputAliases.push_back(item.alias);
						functions.push_back(function);
						percentiles.push_back(aggregatePercentile(item, function, state));
					}
					bool countOnlyAggregate = groupExpressions.empty();
					for (size_t index = 0U; countOnlyAggregate && index < aggregations.size(); ++index) {
						countOnlyAggregate =
								functions[index] == AggregateFunctionType::AGG_COUNT && !aggregations[index].distinct;
					}

					struct AggregateGroup {
						std::vector<PropertyValue> values;
						std::vector<std::unique_ptr<AggregateAccumulator>> accumulators;
						std::vector<std::pmr::unordered_set<std::pmr::string>> distinct_values;
						size_t accounted_bytes = 0U;
					};
					MaterializedByteReservation aggregateStateBytes(&state.shared.materialized_budget);
					MaterializedByteReservation groupStorageBytes(&state.shared.materialized_budget);
					detail::BudgetMemoryResource aggregateIndexMemory(state.shared.materialized_budget);
					std::vector<AggregateGroup> groups;
					std::pmr::unordered_map<std::pmr::string, size_t> groupIndexes{&aggregateIndexMemory};
					size_t aggregateMemoryBytes = 0U;
					const auto chargeAggregateBytes = [&](const size_t bytes) {
						if (bytes > state.shared.bounds.maximum_materialized_bytes -
											std::min(aggregateMemoryBytes, state.shared.bounds.maximum_materialized_bytes)) {
							throw std::runtime_error("logical-plan Aggregate exceeded its materialization byte bound");
						}
						const size_t nextBytes = checkedSizeAdd(
								aggregateMemoryBytes, bytes, "logical-plan Aggregate materialization size overflowed");
						aggregateStateBytes.set(nextBytes);
						aggregateMemoryBytes = nextBytes;
					};
					const auto createGroup = [&](std::vector<PropertyValue> values, MaterializedByteReservation *prepaid = nullptr) {
						if (groups.size() == state.shared.bounds.maximum_distinct_rows ||
							groups.size() == state.shared.bounds.maximum_rows) {
							throw std::runtime_error("logical-plan Aggregate exceeded its group bound");
						}
						size_t groupBytes = 0U;
						for (const PropertyValue &value: values) {
							groupBytes = checkedSizeAdd(groupBytes, detail::retainedPropertyValueBytes(value, [&] { checkGuardNow(state); }),
														"logical-plan Aggregate group size overflowed");
						}
						AggregateGroup group;
						group.values = std::move(values);
						groupBytes = checkedSizeAdd(groupBytes, group.values.capacity() * sizeof(PropertyValue),
													"logical-plan Aggregate group size overflowed");
						groupBytes =
								checkedSizeAdd(groupBytes,
											   aggregations.size() * (sizeof(std::unique_ptr<AggregateAccumulator>) +
																	  sizeof(std::pmr::unordered_set<std::pmr::string>)),
											   "logical-plan Aggregate group size overflowed");
						const size_t prepaidBytes = prepaid ? std::min(prepaid->bytes(), groupBytes) : 0U;
						if (prepaidBytes) {
							aggregateStateBytes.absorb(prepaid->splitOff(prepaidBytes));
							aggregateMemoryBytes = checkedSizeAdd(aggregateMemoryBytes, prepaidBytes, "group prepaid size overflowed");
						}
						chargeAggregateBytes(groupBytes - prepaidBytes);
						group.accumulators.reserve(aggregations.size());
						group.distinct_values.reserve(aggregations.size());
						for (size_t index = 0U; index < aggregations.size(); ++index)
							group.distinct_values.emplace_back(&aggregateIndexMemory);
						for (size_t index = 0U; index < aggregations.size(); ++index) {
							std::unique_ptr<AggregateAccumulator> accumulator =
									createAccumulator(functions[index], false, percentiles[index], &aggregateIndexMemory,
										[&](size_t objectBytes) {
											checkGuardNow(state);
											if (objectBytes > state.shared.materialized_budget.available())
												throw std::runtime_error("logical-plan Aggregate accumulator object exceeded its materialized byte bound");
											chargeAggregateBytes(objectBytes);
											groupBytes = checkedSizeAdd(groupBytes, objectBytes,
												"logical-plan Aggregate object size overflowed");
										});
							if (!accumulator) { // aggregateFunctionType accepts exactly the factory's supported enum
												// values.
								throw std::logic_error("logical-plan Aggregate could not create an accumulator");
							}
							group.accumulators.push_back(std::move(accumulator));
						}
						group.accounted_bytes = groupBytes;
						if (groups.size() == groups.capacity()) {
							const size_t minimumCapacity = checkedSizeAdd(
									groups.size(), 1U, "logical-plan Aggregate group capacity overflowed");
							const size_t requestedCapacity =
									groups.capacity() == 0U
											? minimumCapacity
											: std::max(minimumCapacity,
													   checkedSizeMul(
															   groups.capacity(), 2U,
															   "logical-plan Aggregate group capacity overflowed"));
							const size_t requestedStorageBytes =
									checkedSizeMul(requestedCapacity, sizeof(AggregateGroup),
												   "logical-plan Aggregate group vector size overflowed");
							if (requestedStorageBytes > state.shared.materialized_budget.available()) {
								throw std::runtime_error("logical-plan Aggregate group vector growth exceeded its "
														 "materialized byte bound");
							}
							MaterializedByteReservation groupStorageGrowthBytes(&state.shared.materialized_budget);
							groupStorageGrowthBytes.set(requestedStorageBytes);
							groups.reserve(requestedCapacity);
							const size_t actualCapacityBytes =
									checkedSizeMul(groups.capacity(), sizeof(AggregateGroup),
												   "logical-plan Aggregate group capacity overflowed");
							groupStorageGrowthBytes.set(actualCapacityBytes);
							groupStorageBytes = std::move(groupStorageGrowthBytes);
						}
						groups.push_back(std::move(group));
						return groups.size() - 1U;
					};
					if (groupExpressions.empty()) {
						static_cast<void>(createGroup({}));
					}
					const auto accountAccumulatorRetention = [&](AggregateGroup &group, size_t beforeBytes, size_t afterBytes, MaterializedByteReservation *prepaid = nullptr) {
						if (afterBytes >= beforeBytes) {
							const size_t delta = afterBytes - beforeBytes;
							const size_t transferred = prepaid ? std::min(delta, prepaid->bytes()) : 0U;
							if (transferred) {
								aggregateStateBytes.absorb(prepaid->splitOff(transferred));
								aggregateMemoryBytes = checkedSizeAdd(aggregateMemoryBytes, transferred, "aggregate prepaid size overflowed");
							}
							chargeAggregateBytes(delta - transferred);
							group.accounted_bytes = checkedSizeAdd(group.accounted_bytes, afterBytes - beforeBytes,
								"logical-plan Aggregate group size overflowed");
						} else {
							aggregateMemoryBytes -= beforeBytes - afterBytes;
							aggregateStateBytes.set(aggregateMemoryBytes);
							group.accounted_bytes -= beforeBytes - afterBytes;
						}
					};

					size_t inputRows = 0U;
					const logical::LogicalOperator &aggregateChild = requireOnlyChild(aggregate);
					std::pmr::vector<std::string_view> aggregateColumnKeys(&aggregateIndexMemory);
					std::pmr::vector<size_t> aggregateColumnBindings(&aggregateIndexMemory);
					std::optional<NodeColumnReceiver> aggregateColumnReceiver;
					bool aggregateColumnsBound = false;
					std::optional<detail::BoundColumnLayout> aggregateInputLayout;
					const size_t rowCountBinding = std::numeric_limits<size_t>::max();
					bool columnAggregateEligible = !state.shared.snapshot.supportsNodePropertyCellBatches() &&
						groupExpressions.empty() && !countOnlyAggregate &&
						state.shared.allow_deferred_entity_refs && state.shared.snapshot.supportsNativeNodeRefScans() &&
						aggregateChild.getType() == logical::LogicalOpType::LOP_NODE_SCAN;
					if (columnAggregateEligible) {
						const auto &scan = dynamic_cast<const logical::LogicalNodeScan &>(aggregateChild);
						for (size_t index = 0U; index < aggregations.size(); ++index) {
							const auto &item = aggregations[index];
							const auto *reference = dynamic_cast<const expressions::VariableReferenceExpression *>(item.argument.get());
							if (item.distinct ||
								(item.argument && (!reference || reference->getVariableName() != scan.getVariable())) ||
								(functions[index] != AggregateFunctionType::AGG_COUNT && (!reference || !reference->hasProperty()))) {
								columnAggregateEligible = false;
								break;
							}
							// Preserve payload-free COUNT probes for fields not already
							// required by a value-consuming sibling aggregate.
							if (functions[index] == AggregateFunctionType::AGG_COUNT && reference && reference->hasProperty()) {
								const bool sharedValueInput = std::ranges::any_of(aggregations, [&](const auto &sibling) {
									const auto *other = dynamic_cast<const expressions::VariableReferenceExpression *>(sibling.argument.get());
									return aggregateFunctionType(sibling.functionName) != AggregateFunctionType::AGG_COUNT && other &&
										other->hasProperty() && other->getVariableName() == scan.getVariable() &&
										other->getPropertyName() == reference->getPropertyName();
								});
								if (!sharedValueInput) { columnAggregateEligible = false; break; }
							}
						}
						if (columnAggregateEligible) {
							try {
								aggregateColumnKeys.reserve(aggregations.size());
								aggregateColumnBindings.reserve(aggregations.size());
								for (const auto &item : aggregations) {
									const auto *reference = dynamic_cast<const expressions::VariableReferenceExpression *>(item.argument.get());
									if (!reference || !reference->hasProperty()) {
										aggregateColumnBindings.push_back(rowCountBinding);
										continue;
									}
									const std::string_view key = reference->getPropertyName();
									auto found = std::ranges::find(aggregateColumnKeys, key);
									if (found == aggregateColumnKeys.end()) {
										aggregateColumnKeys.push_back(key);
										found = aggregateColumnKeys.end() - 1;
									}
									aggregateColumnBindings.push_back(static_cast<size_t>(found - aggregateColumnKeys.begin()));
								}
							} catch (const MaterializedByteLimitExceeded &) {
								columnAggregateEligible = false;
							}
						}
					}
					if (columnAggregateEligible) {
						aggregateColumnReceiver.emplace(NodeColumnReceiver{aggregateColumnKeys,
							[&](std::span<const std::string> keys, std::span<const std::vector<PropertyValue>> columns,
								const SelectionVector &selection, std::span<const size_t> originalOrdinals) {
								if (columns.size() != keys.size()) throw std::logic_error("aggregate column input changed its width");
								for (const auto &column : columns) {
									checkGuard(state);
									if (column.size() != selection.size()) throw std::logic_error("aggregate column input changed its row count");
								}
								for (size_t ordinal : originalOrdinals) {
									checkGuard(state);
									if (ordinal >= selection.size()) throw std::logic_error("aggregate column input has an invalid row ordinal");
								}
								if (!aggregateColumnsBound) {
									for (auto &binding : aggregateColumnBindings) {
										if (binding == rowCountBinding) continue;
										const auto found = std::ranges::find(keys, aggregateColumnKeys[binding]);
										if (found == keys.end()) throw std::logic_error("aggregate input column was not produced");
										binding = static_cast<size_t>(found - keys.begin());
									}
									try {
										aggregateInputLayout.emplace(keys, state.shared.materialized_budget,
											std::pmr::get_default_resource(), [&] { checkGuard(state); });
									} catch (const MaterializedByteLimitExceeded &) {
										// Optional name metadata declines before any aggregate effects.
									}
									aggregateColumnsBound = true;
								}
								if (aggregateInputLayout) aggregateInputLayout->validate(keys);
								else for (size_t index = 0U; index < aggregations.size(); ++index) {
									if (aggregateColumnBindings[index] == rowCountBinding) continue;
									const auto &reference = static_cast<const expressions::VariableReferenceExpression &>(*aggregations[index].argument);
									detail::validateColumnBinding(keys, aggregateColumnBindings[index], reference.getPropertyName());
								}
								detail::PrimitiveNumericColumns numericColumns(state.shared.materialized_budget);
								MaterializedByteReservation numericStateHeadroom(&state.shared.materialized_budget);
								size_t numericStateBytes = 0U;
								for (size_t index = 0U; index < functions.size(); ++index) {
									if (functions[index] == AggregateFunctionType::AGG_MIN || functions[index] == AggregateFunctionType::AGG_MAX) {
										const size_t retained = groups.front().accumulators[index]->retainedBytesForAdmission();
										numericStateBytes = checkedSizeAdd(numericStateBytes, sizeof(int64_t) - std::min(retained, sizeof(int64_t)),
											"logical-plan numeric aggregate state size overflowed");
									}
								}
								bool numericPrepared = false;
								// Growing collections own their allocation protocol; optional conversion
								// buffers must not compete with that state or trigger update replay.
								const bool primitiveOnly = std::ranges::all_of(functions, [](auto function) {
									return function == AggregateFunctionType::AGG_COUNT || function == AggregateFunctionType::AGG_SUM ||
										function == AggregateFunctionType::AGG_AVG || function == AggregateFunctionType::AGG_MIN ||
										function == AggregateFunctionType::AGG_MAX || function == AggregateFunctionType::AGG_STDEV ||
										function == AggregateFunctionType::AGG_STDEVP;
								});
								if (primitiveOnly && !numericStateHeadroom.wouldExceed(numericStateBytes)) {
									// Optional buffers cannot consume the scalar extremum's
									// first retained value allowance. Actual updates charge it below.
									numericStateHeadroom.set(numericStateBytes);
									numericPrepared = numericColumns.tryPrepare(columns, aggregateColumnBindings,
										[&] { checkGuard(state); });
									numericStateHeadroom.reset();
								}
								debug::PerfTrace::addValue("logical_plan.aggregate.typed_columns",
									static_cast<int64_t>(numericColumns.typedColumnCount()));
								if (numericPrepared && numericColumns.typedColumnCount() == 0U) {
									// Dynamic-only batches gain nothing from conversion metadata.
									numericColumns.release();
									numericPrepared = false;
								}
								std::optional<detail::BatchEnvelope> envelope;
								if (numericPrepared && aggregateInputLayout && state.shared.bound_program) {
									try {
										numericStateHeadroom.set(numericStateBytes);
										detail::BudgetMemoryResource metadataMemory(state.shared.materialized_budget);
										std::pmr::vector<detail::BatchColumnSource> sources(&metadataMemory);
										std::pmr::vector<uint32_t> selectedRows(&metadataMemory);
										std::pmr::vector<uint64_t> sourceOrdinals(&metadataMemory);
										for (size_t column = 0U; column < numericColumns.columnCount(); ++column) {
											checkGuard(state); sources.push_back(detail::BatchColumnSource::numeric(numericColumns, column));
										}
										for (size_t row = 0U; row < selection.size(); ++row) { checkGuard(state); sourceOrdinals.push_back(row); }
										for (size_t row : originalOrdinals) if (selection.includes(row)) {
											checkGuard(state); selectedRows.push_back(static_cast<uint32_t>(row));
										}
										envelope.emplace(state.shared.bound_program->root().program, *aggregateInputLayout, state.shared.snapshot,
											state.shared.bounds.maximum_rows, state.shared.materialized_budget);
										envelope->publish(keys, sources, selection.size(), selectedRows, sourceOrdinals, [&] { checkGuardNow(state); });
										debug::PerfTrace::addValue("logical_plan.batch_envelope.published", 1);
									} catch (const MaterializedByteLimitExceeded &) {
										envelope.reset(); debug::PerfTrace::addValue("logical_plan.batch_envelope.admission_fallback", 1);
									}
									numericStateHeadroom.reset();
								}
								const auto numericRow = [&](size_t column, size_t ordinal) {
									return envelope ? envelope->sourceRow(aggregateInputLayout->column(column), envelope->rowId(ordinal)) : numericColumns.rowId(ordinal);
								};
								for (const size_t ordinal : originalOrdinals) {
									checkGuard(state);
									if (!selection.includes(ordinal)) continue;
									if (inputRows == state.shared.bounds.maximum_rows)
										throw std::runtime_error("logical-plan Aggregate exceeded its input row bound");
									++inputRows;
									for (size_t index = 0U; index < aggregations.size(); ++index) {
										checkGuard(state);
										auto &accumulator = *groups.front().accumulators[index];
										if (functions[index] == AggregateFunctionType::AGG_COUNT || functions[index] == AggregateFunctionType::AGG_SUM ||
											functions[index] == AggregateFunctionType::AGG_AVG || functions[index] == AggregateFunctionType::AGG_STDEV ||
											functions[index] == AggregateFunctionType::AGG_STDEVP) {
											// Count and numeric sums retain no variable payload. Keep
											// their typed loop free of retention probes and copy guards.
											if (aggregateColumnBindings[index] == rowCountBinding) accumulator.updateRow();
											else if (numericPrepared) numericColumns.update(accumulator, functions[index], aggregateColumnBindings[index], numericRow(aggregateColumnBindings[index], ordinal));
											else accumulator.update(columns[aggregateColumnBindings[index]][ordinal]);
											continue;
										}
										const size_t beforeBytes = accumulator.retainedBytesForAdmission();
										MaterializedByteReservation updateScratch(&state.shared.materialized_budget);
										bool admissionRejected = false;
										const std::function<void(size_t)> admit = [&](size_t bytes) {
											checkGuardNow(state);
											try { updateScratch.set(bytes); }
											catch (const MaterializedByteLimitExceeded &) { admissionRejected = true; throw; }
										};
										try {
											if (numericPrepared) numericColumns.update(accumulator, functions[index], aggregateColumnBindings[index], numericRow(aggregateColumnBindings[index], ordinal), admit);
											else accumulator.updateAdmitted(columns[aggregateColumnBindings[index]][ordinal], admit);
										} catch (const MaterializedByteLimitExceeded &) {
											if (!admissionRejected || !numericPrepared) throw;
											// Extremum copying has not published state on admission failure.
											// Drop optional buffers and retry only this update, never rows
											// already counted or accumulated. Guard failures still escape.
											envelope.reset();
											numericColumns.release();
											numericPrepared = false;
											updateScratch.reset();
											accumulator.updateAdmitted(columns[aggregateColumnBindings[index]][ordinal], admit);
											debug::PerfTrace::addValue("logical_plan.aggregate.optional_buffers_released", 1);
										}
										updateScratch.reset();
										accountAccumulatorRetention(groups.front(), beforeBytes, accumulator.retainedBytesForAdmission());
									}
								}
								debug::PerfTrace::addValue("logical_plan.aggregate.column_input_rows",
									static_cast<int64_t>(selection.selectedCount()));
								return true;
							}});
					} else {
						std::pmr::vector<std::string_view>(&aggregateIndexMemory).swap(aggregateColumnKeys);
						std::pmr::vector<size_t>(&aggregateIndexMemory).swap(aggregateColumnBindings);
					}
					std::pmr::vector<std::string_view> directKeys(&aggregateIndexMemory);
					std::pmr::vector<size_t> directGroups(&aggregateIndexMemory), directArguments(&aggregateIndexMemory);
					struct DirectFilterTerm { size_t column; const expressions::LiteralExpression *literal; detail::ColumnPredicateOp operation; };
					std::pmr::vector<DirectFilterTerm> directTerms(&aggregateIndexMemory);
					const auto *directFilter = dynamic_cast<const logical::LogicalFilter *>(&aggregateChild);
					const auto *directInput = directFilter ? &requireOnlyChild(*directFilter) : &aggregateChild;
					bool directBound = false;
					bool directEligible = state.shared.allow_deferred_entity_refs && state.shared.snapshot.supportsNativeNodeRefScans() &&
						state.shared.snapshot.supportsNodePropertyCellBatches() && directInput->getType() == logical::LogicalOpType::LOP_NODE_SCAN;
					const auto *directScan = directEligible ? &static_cast<const logical::LogicalNodeScan &>(*directInput) : nullptr;
					const auto directReference = [&](const expressions::Expression *expression) {
						const auto *ref = dynamic_cast<const expressions::VariableReferenceExpression *>(expression);
						return ref && directScan && ref->getVariableName() == directScan->getVariable() ? ref : nullptr;
					};
					for (const auto &expression : groupExpressions) {
						const auto *ref = directReference(expression.get());
						if (!ref || !ref->hasProperty()) directEligible = false;
					}
					for (size_t index = 0; index < aggregations.size(); ++index) {
						const auto &item = aggregations[index]; const auto *ref = directReference(item.argument.get());
						if (item.argument && (!ref || (!ref->hasProperty() &&
							(functions[index] != AggregateFunctionType::AGG_COUNT || item.distinct)))) directEligible = false;
					}
					for (size_t index = 0; directEligible && index < aggregations.size(); ++index) {
						const auto &item = aggregations[index]; const auto *ref = directReference(item.argument.get());
						if (functions[index] != AggregateFunctionType::AGG_COUNT || item.distinct || !ref || !ref->hasProperty()) continue;
						const auto same = [&](const expressions::Expression *expression) {
							const auto *other = directReference(expression);
							return other && other->hasProperty() && other->getPropertyName() == ref->getPropertyName();
						};
						const bool needed = std::ranges::any_of(groupExpressions, [&](const auto &expr) { return same(expr.get()); }) ||
							std::ranges::any_of(aggregations, [&](const auto &sibling) {
								return (sibling.distinct || aggregateFunctionType(sibling.functionName) != AggregateFunctionType::AGG_COUNT) && same(sibling.argument.get());
							});
						if (!needed) directEligible = false;
					}
					// Payload-free global COUNT(property) probes remain preferable when
					// no grouping/value-consuming sibling already needs those fields.
					if (countOnlyAggregate && std::ranges::any_of(aggregations, [&](const auto &item) {
						const auto *ref = directReference(item.argument.get()); return ref && ref->hasProperty();
					})) directEligible = false;
					if (directEligible) {
						try {
							const auto bind = [&](const expressions::Expression *expression) {
								const auto *ref = directReference(expression);
								if (!ref || !ref->hasProperty()) return rowCountBinding;
								const std::string_view name = ref->getPropertyName();
								auto it = std::ranges::find(directKeys, name);
								if (it == directKeys.end()) { directKeys.push_back(name); it = directKeys.end() - 1; }
								return static_cast<size_t>(it - directKeys.begin());
							};
							if (directFilter) {
								const auto collect = [&](const auto &self, const expressions::Expression *expression) -> bool {
									checkGuard(state);
									const auto *binary = dynamic_cast<const expressions::BinaryOpExpression *>(expression);
									if (!binary || !binary->getLeft() || !binary->getRight()) return false;
									using Op = expressions::BinaryOperatorType;
									if (binary->getOperator() == Op::BOP_AND) return self(self, binary->getLeft()) && self(self, binary->getRight());
									const auto *ref = directReference(binary->getLeft());
									const auto *literal = dynamic_cast<const expressions::LiteralExpression *>(binary->getRight());
									if (!ref || !ref->hasProperty() || !literal) return false;
									detail::ColumnPredicateOp operation;
									switch (binary->getOperator()) {
										case Op::BOP_EQUAL: operation = detail::ColumnPredicateOp::CPO_EQUAL; break;
										case Op::BOP_NOT_EQUAL: operation = detail::ColumnPredicateOp::CPO_NOT_EQUAL; break;
										case Op::BOP_LESS: operation = detail::ColumnPredicateOp::CPO_LESS; break;
										case Op::BOP_LESS_EQUAL: operation = detail::ColumnPredicateOp::CPO_LESS_EQUAL; break;
										case Op::BOP_GREATER: operation = detail::ColumnPredicateOp::CPO_GREATER; break;
										case Op::BOP_GREATER_EQUAL: operation = detail::ColumnPredicateOp::CPO_GREATER_EQUAL; break;
										default: return false;
									}
									directTerms.push_back({bind(binary->getLeft()), literal, operation}); return true;
								};
								directEligible = collect(collect, directFilter->getPredicate().get());
							}
							for (const auto &expression : groupExpressions) directGroups.push_back(bind(expression.get()));
							for (const auto &item : aggregations) directArguments.push_back(bind(item.argument.get()));
						} catch (const MaterializedByteLimitExceeded &) { directEligible = false; }
					}
					if (!directEligible) {
						std::pmr::vector<std::string_view>(&aggregateIndexMemory).swap(directKeys);
						std::pmr::vector<size_t>(&aggregateIndexMemory).swap(directGroups);
						std::pmr::vector<size_t>(&aggregateIndexMemory).swap(directArguments);
						std::pmr::vector<DirectFilterTerm>(&aggregateIndexMemory).swap(directTerms);
					}
					if (directEligible) {
						aggregateColumnReceiver.emplace(NodeColumnReceiver{directKeys, {},
							[&](std::span<const std::string> keys, const detail::DirectPropertyColumns &columns,
								const SelectionVector &selection, std::span<const size_t> ordinals) {
								if (!directBound) {
									const auto resolve = [&](size_t &binding) {
										if (binding == rowCountBinding) return;
										const auto found = std::ranges::find(keys, directKeys.at(binding));
										if (found == keys.end()) throw std::logic_error("direct aggregate column missing");
										binding = static_cast<size_t>(found - keys.begin());
									};
									for (auto &binding : directGroups) resolve(binding);
									for (auto &binding : directArguments) resolve(binding);
									for (auto &term : directTerms) resolve(term.column);
									directBound = true;
								}
								const auto appendCellKey = [&](std::pmr::string &key, size_t column, detail::BatchRowId row) {
									const auto cell = columns.cell(column, row);
									std::visit([&](auto value) {
										using T = decltype(value);
										if constexpr (std::is_same_v<T, std::monostate>) appendDistinctPropertyValue(key, PropertyValue());
										else if constexpr (std::is_same_v<T, std::string_view>) {
											key += static_cast<char>(static_cast<uint8_t>(PropertyType::STRING)); appendDistinctString(key, value);
										} else if constexpr (std::is_same_v<T, const PropertyValue *>) appendDistinctPropertyValue(key, *value);
										else appendDistinctPropertyValue(key, PropertyValue(value));
									}, cell.data);
								};
                                // Partial COUNT over a bounded short-text column. Ordinary
                                // filters and low-cardinality groups merge per distinct value,
                                // rather than serializing keys/updating virtual accumulators per row.
                                bool batchCountEligible = directGroups.size() <= 1U &&
                                    std::ranges::all_of(functions, [](auto function) { return function == AggregateFunctionType::AGG_COUNT; });
                                for (size_t i = 0; i < aggregations.size(); ++i)
                                    batchCountEligible = batchCountEligible && !aggregations[i].distinct && directArguments[i] == rowCountBinding;
                                const size_t textColumn = !directGroups.empty() ? directGroups.front() :
                                    !directTerms.empty() ? directTerms.front().column : rowCountBinding;
                                for (const auto &term : directTerms)
                                    batchCountEligible = batchCountEligible && term.column == textColumn && term.literal->isString() &&
                                        term.operation == detail::ColumnPredicateOp::CPO_EQUAL;
                                if (batchCountEligible && directGroups.empty() && directTerms.empty()) {
                                    size_t accepted = 0U;
                                    for (const auto row : ordinals) {
                                        checkGuard(state);
                                        if (row >= columns.size()) throw std::logic_error("partial count ordinal outside batch");
                                        if (selection.includes(row)) ++accepted;
                                    }
                                    if (accepted > state.shared.bounds.maximum_rows - inputRows)
                                        throw std::runtime_error("logical-plan Aggregate exceeded its input row bound");
                                    inputRows += accepted;
                                    for (auto &accumulator : groups.front().accumulators)
                                        static_cast<CountAccumulator &>(*accumulator).addRows(accepted);
                                    debug::PerfTrace::addValue("logical_plan.aggregate.partial_count_batches", 1);
                                    debug::PerfTrace::addValue("logical_plan.aggregate.partial_count_updates", 1);
                                    debug::PerfTrace::addValue("logical_plan.aggregate.direct_input_rows", static_cast<int64_t>(accepted));
                                    return true;
                                }
                                if (batchCountEligible && textColumn != rowCountBinding) {
                                    struct PartialCount { size_t first, rows; };
                                    std::pmr::vector<PartialCount> partials(&aggregateIndexMemory);
                                    std::pmr::unordered_map<std::optional<std::string_view>, size_t> indexes(&aggregateIndexMemory);
                                    size_t accepted = 0U, rejected = 0U;
                                    bool prepared = false;
                                    try {
                                        prepared = columns.tryVisitShortTextColumn(textColumn, selection, ordinals,
                                            [&](size_t row, std::optional<std::string_view> text) {
                                                for (const auto &term : directTerms) {
                                                    if (!text || *text != term.literal->getStringValue()) { ++rejected; return; }
                                                }
                                                ++accepted;
                                                // Global COUNT needs no dictionary or per-row group key.
                                                const auto key = directGroups.empty() ? std::optional<std::string_view>{} : text;
                                                auto [found, inserted] = indexes.try_emplace(key, partials.size());
                                                if (inserted) partials.push_back({row, 0U});
                                                ++partials[found->second].rows;
                                            }, [&] { checkGuard(state); });
                                    } catch (const MaterializedByteLimitExceeded &) {
                                        // No aggregate state has changed. Optional partial buffers
                                        // are destroyed before the existing direct consumer runs.
                                        prepared = false;
                                    } catch (const detail::ResourceAdmissionDenied &) {
                                        prepared = false;
                                    }
                                    if (prepared) {
                                        if (accepted > state.shared.bounds.maximum_rows - inputRows)
                                            throw std::runtime_error("logical-plan Aggregate exceeded its input row bound");
                                        inputRows += accepted;
                                        for (const auto &partial : partials) {
                                            checkGuard(state); size_t groupIndex = 0U;
                                            if (!directGroups.empty()) {
                                                const auto row = columns.rowId(partial.first);
                                                std::pmr::string key{&aggregateIndexMemory}; appendCellKey(key, textColumn, row);
                                                const auto found = groupIndexes.find(key);
                                                if (found != groupIndexes.end()) groupIndex = found->second;
                                                else {
                                                    MaterializedByteReservation copies(&state.shared.materialized_budget);
                                                    copies.set(sizeof(PropertyValue));
                                                    std::vector<PropertyValue> values; values.reserve(1U);
                                                    const size_t slots = values.capacity() * sizeof(PropertyValue); copies.set(slots);
                                                    auto value = columns.copyValue(textColumn, row,
                                                        [&](size_t bytes) { copies.set(checkedSizeAdd(slots, bytes, "partial count key copy overflowed")); },
                                                        [&] { checkGuardNow(state); });
                                                    copies.set(checkedSizeAdd(slots, detail::propertyBufferEstimates(value, [&] { checkGuardNow(state); }).heapBytes,
                                                        "partial count key retention overflowed"));
                                                    values.push_back(std::move(value)); groupIndex = createGroup(std::move(values), &copies);
                                                    groupIndexes.emplace(std::move(key), groupIndex);
                                                }
                                            }
                                            for (auto &accumulator : groups.at(groupIndex).accumulators)
                                                static_cast<CountAccumulator &>(*accumulator).addRows(partial.rows);
                                        }
                                        debug::PerfTrace::addValue("logical_plan.aggregate.partial_count_batches", 1);
                                        debug::PerfTrace::addValue("logical_plan.aggregate.partial_count_updates", static_cast<int64_t>(partials.size()));
                                        debug::PerfTrace::addValue("logical_plan.aggregate.direct_input_rows", static_cast<int64_t>(accepted));
                                        debug::PerfTrace::addValue("logical_plan.aggregate.direct_filter_rejected", static_cast<int64_t>(rejected));
                                        return true;
                                    }
                                }
								size_t filterRejected = 0U;
                                for (const auto ordinal : ordinals) {
									checkGuard(state);
									if (ordinal >= columns.size()) throw std::logic_error("direct aggregate ordinal changed");
									if (!selection.includes(ordinal)) continue;
									bool passesFilter = true;
									for (const auto &term : directTerms) {
										checkGuard(state); const auto cell = columns.cell(term.column, columns.rowId(ordinal));
										if (cell.state != detail::ColumnCellState::CCS_VALUE) { passesFilter = false; break; }
										const auto &literal = *term.literal; PropertyValue scalar;
										std::optional<std::string_view> text; const PropertyValue *expected = &scalar;
										if (literal.isString()) { scalar = PropertyValue(std::string{}); text = literal.getStringValue(); }
										else if (literal.isBoolean()) scalar = PropertyValue(literal.getBooleanValue());
										else if (literal.isInteger()) scalar = PropertyValue(literal.getIntegerValue());
										else if (literal.isDouble()) scalar = PropertyValue(literal.getDoubleValue());
										else if (literal.isPropertyValue()) expected = &literal.getPropertyValue();
										const detail::ColumnPredicate predicate{term.column, term.operation, expected};
										const bool match = std::visit([&](auto value) { return detail::columnPredicateMatchesBorrowed(value, predicate, text); }, cell.data);
										if (!match) { passesFilter = false; break; }
									}
									if (!passesFilter) { ++filterRejected; continue; }
									if (inputRows == state.shared.bounds.maximum_rows) throw std::runtime_error("logical-plan Aggregate exceeded its input row bound");
									++inputRows; const auto row = columns.rowId(ordinal);
									size_t groupIndex = 0;
									if (!directGroups.empty()) {
										std::pmr::string key{&aggregateIndexMemory};
										for (const auto column : directGroups) appendCellKey(key, column, row);
										const auto found = groupIndexes.find(key);
										if (found != groupIndexes.end()) groupIndex = found->second;
										else {
											MaterializedByteReservation copies(&state.shared.materialized_budget);
											copies.set(checkedSizeMul(directGroups.size(), sizeof(PropertyValue), "direct group slots overflowed"));
											std::vector<PropertyValue> values; values.reserve(directGroups.size());
											copies.set(checkedSizeMul(values.capacity(), sizeof(PropertyValue), "direct group capacity overflowed"));
											for (const auto column : directGroups) {
												const size_t before = copies.bytes();
												auto value = columns.copyValue(column, row, [&](size_t bytes) {
													copies.set(checkedSizeAdd(before, bytes, "direct group copy overflowed"));
												}, [&] { checkGuardNow(state); });
												copies.set(checkedSizeAdd(before, detail::propertyBufferEstimates(value, [&] { checkGuardNow(state); }).heapBytes, "direct group retained bytes overflowed"));
												values.push_back(std::move(value));
											}
											groupIndex = createGroup(std::move(values), &copies);
											groupIndexes.emplace(std::move(key), groupIndex);
										}
									}
									auto &group = groups.at(groupIndex);
									for (size_t index = 0; index < aggregations.size(); ++index) {
										checkGuard(state); auto &accumulator = *group.accumulators[index];
										const size_t column = directArguments[index];
										if (column == rowCountBinding) { accumulator.updateRow(); continue; }
										const auto cell = columns.cell(column, row);
										if (aggregations[index].distinct) {
											std::pmr::string key{&aggregateIndexMemory}; appendCellKey(key, column, row);
											if (!group.distinct_values[index].insert(std::move(key)).second) continue;
										}
										if (functions[index] == AggregateFunctionType::AGG_COUNT) {
											if (cell.state == detail::ColumnCellState::CCS_VALUE) accumulator.updateRow();
											continue;
										}
										MaterializedByteReservation valueCopy(&state.shared.materialized_budget), update(&state.shared.materialized_budget);
										auto value = columns.copyValue(column, row, [&](size_t bytes) { valueCopy.set(bytes); }, [&] { checkGuardNow(state); });
										const size_t before = accumulator.retainedBytesForAdmission();
										accumulator.updateAdmitted(value, [&](size_t bytes) { checkGuardNow(state); update.set(bytes); });
										accountAccumulatorRetention(group, before, accumulator.retainedBytesForAdmission(), &update);
									}
									debug::PerfTrace::addValue("logical_plan.aggregate.direct_input_rows", 1);
								}
								debug::PerfTrace::addValue("logical_plan.aggregate.direct_filter_rejected", static_cast<int64_t>(filterRejected));
                                return true;
							}});
						aggregateColumnReceiver->filterOwner = directFilter;
					}
					const auto consumeAggregateRow = [&](LogicalPlanReadRow row,
														 MaterializedByteReservation inputRowBytes) {
						ScopedBoundInputs scopedInputs(state, boundInputs, row);
						if (countOnlyAggregate) {
							inputRowBytes.reset();
						} else if (inputRowBytes.bytes() == 0U) {
							admitReferenceResultBytes(
									inputRowBytes, logicalReadRowBytes(row, state.shared.context),
									"logical-plan Aggregate input row exceeded its shared byte bound");
						}
						checkGuard(state);
						if (inputRows == state.shared.bounds.maximum_rows) {
							throw std::runtime_error("logical-plan Aggregate exceeded its input row bound");
						}
						++inputRows;
						MaterializedByteReservation groupValueScratch(&state.shared.materialized_budget);
						size_t groupValueBytes = checkedSizeMul(groupExpressions.size(), sizeof(PropertyValue),
							"logical-plan Aggregate group value array overflowed");
						groupValueScratch.set(groupValueBytes);
						std::vector<PropertyValue> groupValues;
						groupValues.reserve(groupExpressions.size());
						std::pmr::string groupKey{&aggregateIndexMemory};
						for (const std::shared_ptr<expressions::Expression> &expression: groupExpressions) {
							PropertyValue value = scalarValue(evaluateExpression(*expression, row, state));
							groupValueBytes = checkedSizeAdd(groupValueBytes,
								detail::retainedPropertyValueBytes(value, [&] { checkGuardNow(state); }),
								"logical-plan Aggregate group value size overflowed");
							groupValueScratch.set(groupValueBytes);
							appendDistinctPropertyValue(groupKey, value);
							groupValues.push_back(std::move(value));
						}
						size_t groupIndex = 0U;
						if (!groupExpressions.empty()) {
							const auto found = groupIndexes.find(groupKey);
							if (found == groupIndexes.end()) {
								// Transfer the already-owned payload to permanent group accounting.
								groupValueScratch.reset();
								groupIndex = createGroup(std::move(groupValues));
								groupIndexes.emplace(std::move(groupKey), groupIndex);
							} else {
								groupIndex = found->second;
								std::vector<PropertyValue>().swap(groupValues);
								groupValueScratch.reset();
							}
						}
						AggregateGroup &group = groups[groupIndex];
						for (size_t index = 0U; index < aggregations.size(); ++index) {
							const logical::LogicalAggItem &item = aggregations[index];
							const size_t beforeBytes = group.accumulators[index]->retainedBytesForAdmission();
							if (!item.argument) {
								group.accumulators[index]->updateRow();
							} else {
								// A non-distinct COUNT over a direct Entity Property only needs
								// non-NULL presence. Native snapshots answer from the compact
								// token directory and value header without decoding payloads, keeping
								// large properties out of the transient row and session caches.
								std::optional<bool> presence;
								if (functions[index] == AggregateFunctionType::AGG_COUNT && !item.distinct &&
									item.argument->getExpressionType() ==
											expressions::ExpressionType::PROPERTY_ACCESS) {
									const auto *property =
											dynamic_cast<const expressions::VariableReferenceExpression *>(
													item.argument.get());
									if (property != nullptr && property->hasProperty() &&
										!row.values.contains(property->toString())) {
										const auto binding = row.values.find(property->getVariableName());
										if (binding != row.values.end()) {
											if (const auto *node =
														std::get_if<storage::LogicalGraphNode>(&binding->second)) {
												presence = state.shared.snapshot.nodePropertyIsNonNull(
														node->node_id, property->getPropertyName());
											} else if (const auto *edge = std::get_if<storage::LogicalGraphEdge>(
															   &binding->second)) {
												presence = state.shared.snapshot.edgePropertyIsNonNull(
														edge->edge_id, property->getPropertyName());
											} else if (const auto *reference =
															   std::get_if<storage::EntityRef>(&binding->second)) {
												validateEntityRef(*reference);
												presence = reference->is_edge ? state.shared.snapshot.edgePropertyIsNonNull(
																						reference->entity_id,
																						property->getPropertyName())
																			  : state.shared.snapshot.nodePropertyIsNonNull(
																						reference->entity_id,
																						property->getPropertyName());
											}
										}
									}
								}
								if (presence.has_value()) {
									if (*presence) {
										group.accumulators[index]->updateRow();
									}
									continue;
								}
								const PropertyValue value = scalarValue(evaluateExpression(*item.argument, row, state));
								if (item.distinct) {
									std::pmr::string key{&aggregateIndexMemory};
									appendDistinctPropertyValue(key, value);
									if (!group.distinct_values[index].insert(std::move(key)).second) continue;
								}
								MaterializedByteReservation updateScratch(&state.shared.materialized_budget);
								group.accumulators[index]->updateAdmitted(value, [&](size_t bytes) {
									checkGuardNow(state); updateScratch.set(bytes);
								});
								updateScratch.reset();
							}
							const size_t afterBytes = group.accumulators[index]->retainedBytesForAdmission();
							accountAccumulatorRetention(group, beforeBytes, afterBytes);
						}
						return true;
					};
					// Aggregate already consumes every input row. Its own all-selected batch
					// only delayed accumulation and duplicated the source's row storage.
					const auto consumeUnchargedAggregateRow = [&](LogicalPlanReadRow row) {
						return consumeAggregateRow(std::move(row),
												   MaterializedByteReservation(&state.shared.materialized_budget));
					};
					const ChargedRowVisitor chargedAggregateRow = consumeAggregateRow;
					std::vector<const expressions::Expression *> aggregateCandidates;
					aggregateCandidates.reserve(groupExpressions.size() + aggregations.size());
					for (const auto &expression: groupExpressions) {
						aggregateCandidates.push_back(expression.get());
					}
					const bool canProbeCountProperties = state.shared.snapshot.supportsPropertyNonNullProbes();
					for (size_t index = 0U; index < aggregations.size(); ++index) {
						const auto &item = aggregations[index];
						if (!item.argument)
							continue;
						// Grouping and sibling aggregates do not change COUNT's data needs.
						// Keep actual values when DISTINCT or another expression needs them;
						// unsupported snapshots retain their existing column-batch fallback.
						if (canProbeCountProperties && functions[index] == AggregateFunctionType::AGG_COUNT &&
							!item.distinct &&
							item.argument->getExpressionType() == expressions::ExpressionType::PROPERTY_ACCESS) {
							continue;
						}
						aggregateCandidates.push_back(item.argument.get());
					}
					const std::vector<const expressions::Expression *> aggregatePropertyExpressions =
							selectBatchableExpressions(aggregateCandidates);
					const std::vector<std::string> completeEntityVariables =
							selectBatchableCompleteEntityVariables(aggregateCandidates);
					std::optional<bool> batchedProperties;
					// COUNT is cardinality-only for selected scalar Properties: it must not
					// force a potentially large payload into the bounded prefetch batch.
					// Complete-Entity expressions are different: COUNT(properties(n)) still
					// needs the complete map, so that requirement is batched even for a
					// count-only aggregate.
					if (!aggregateColumnReceiver && ((!aggregatePropertyExpressions.empty() && !countOnlyAggregate) ||
						!completeEntityVariables.empty())) {
						batchedProperties = tryVisitBatchedPropertyExpressions(
								requireOnlyChild(aggregate), aggregatePropertyExpressions, state,
								consumeUnchargedAggregateRow, {}, completeEntityVariables, &chargedAggregateRow,
								!aggregateCandidates.empty() && std::ranges::all_of(aggregateCandidates, [](const auto *expression) {
									return expression && expression->getExpressionType() == expressions::ExpressionType::PROPERTY_ACCESS;
								}) && std::ranges::none_of(aggregations, [](const auto &item) { return item.distinct; }));
					}
					const bool completed = batchedProperties.has_value()
												   ? *batchedProperties
												   : visitOperator(aggregateChild, state, consumeUnchargedAggregateRow,
																   supportsChargedRowHandoff(aggregateChild.getType())
																		   ? &chargedAggregateRow
																		   : nullptr,
																   aggregateColumnReceiver ? &*aggregateColumnReceiver : nullptr);
					if (!completed) {
						throw std::runtime_error(
								"logical-plan Aggregate could not prove that its complete input was read");
					}
					// Release lookup nodes and buckets before downstream output is
					// admitted. Per-group DISTINCT keys live until their group is consumed.
					aggregateColumnReceiver.reset();
					std::pmr::vector<std::string_view>(&aggregateIndexMemory).swap(directKeys);
					std::pmr::vector<size_t>(&aggregateIndexMemory).swap(directGroups);
					std::pmr::vector<size_t>(&aggregateIndexMemory).swap(directArguments);
					std::pmr::vector<DirectFilterTerm>(&aggregateIndexMemory).swap(directTerms);
					aggregateInputLayout.reset();
					std::pmr::vector<std::string_view>(&aggregateIndexMemory).swap(aggregateColumnKeys);
					std::pmr::vector<size_t>(&aggregateIndexMemory).swap(aggregateColumnBindings);
					decltype(groupIndexes){&aggregateIndexMemory}.swap(groupIndexes);
					const size_t outputGroupCount = groups.size();
					for (size_t groupIndex = 0U; groupIndex < outputGroupCount; ++groupIndex) {
						AggregateGroup &group = groups[groupIndex];
						checkGuard(state);
						LogicalPlanReadRow row;
						for (size_t index = 0U; index < group.values.size(); ++index) {
							row.values.emplace(outputAliases[index], std::move(group.values[index]));
						}
						for (size_t index = 0U; index < aggregations.size(); ++index) {
							row.values.emplace(aggregations[index].alias, group.accumulators[index]->takeResult());
						}
						// Transfer payload ownership before downstream buffers acquire it.
						// The vector's now-empty group slot remains live until this scope ends.
						const size_t releasedBytes = group.accounted_bytes;
						group = AggregateGroup{};
						group.accounted_bytes = 0U;
						aggregateMemoryBytes -= releasedBytes;
						aggregateStateBytes.set(aggregateMemoryBytes);
						if (groupIndex + 1U == outputGroupCount) {
							// The last result no longer needs any group slots. Destroy
							// their retained capacity before refunding it and admitting
							// the output; even a scalar COUNT must have this headroom.
							std::vector<AggregateGroup>{}.swap(groups);
							groupStorageBytes.reset();
						}
						const size_t outputBytes = logicalReadRowBytes(row, state.shared.context) -
								(state.shared.resident_output_batch_bytes == 0U ? 0U : sizeof(LogicalPlanReadRow));
						// Buffered output already charges its row-slot capacity. Its
						// collector admits only payload; do not charge that slot twice.
						if (outputBytes > state.shared.materialized_budget.available()) {
							throw std::runtime_error(
									"logical-plan Aggregate output exceeded the shared materialized byte bound");
						}
						if (!visitor(std::move(row))) {
							return false;
						}
					}
					return true;
				}
