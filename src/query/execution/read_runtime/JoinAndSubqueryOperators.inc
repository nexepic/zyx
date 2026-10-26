				case logical::LogicalOpType::LOP_JOIN: {
                    debug::PerfTrace::addValue("logical_plan.consumer_adapter.join.calls", 1);
					const auto &join = dynamic_cast<const logical::LogicalJoin &>(op);
					const logical::LogicalOperator *left = join.getLeft();
					const logical::LogicalOperator *right = join.getRight();
					if (left == nullptr || right == nullptr) {
						throw std::invalid_argument("logical-plan Join requires two valid children");
					}
					MaterializedByteReservation leftNameBytes(&state.shared.materialized_budget), rightNameBytes(&state.shared.materialized_budget);
					const auto leftColumns = boundOutputNames(*left, state, leftNameBytes);
					const auto rightColumns = boundOutputNames(*right, state, rightNameBytes);
					detail::BudgetMemoryResource nameMemory(state.shared.materialized_budget);
					auto *nameAllocator = state.shared.bound_program ? static_cast<std::pmr::memory_resource *>(&nameMemory) : std::pmr::get_default_resource();
					const auto validateColumns = [nameAllocator](const std::vector<std::string> &columns) {
						std::pmr::unordered_set<std::string_view> declaredColumns(nameAllocator);
						declaredColumns.reserve(columns.size());
						for (const std::string &column: columns) {
							if (column.empty() || !declaredColumns.insert(column).second) {
								throw std::invalid_argument("logical-plan Join has invalid output columns");
							}
						}
					};
					validateColumns(leftColumns);
					validateColumns(rightColumns);

					BoundedBatch<LogicalPlanReadRow> rightRows(
							state.shared.bounds.maximum_rows, state.shared.bounds.maximum_materialized_bytes,
							[&state](const LogicalPlanReadRow &row) { return logicalReadRowBytes(row, state.shared.context); },
							&state.shared.materialized_budget,
                            state.shared.context && state.shared.context->resource_account, sizeof(LogicalPlanReadRow));
					rightRows.reserve(state.shared.bounds.maximum_rows);
					std::pmr::vector<const std::string *> sharedColumns(nameAllocator);
					sharedColumns.reserve(std::min(leftColumns.size(), rightColumns.size()));
					std::pmr::unordered_set<std::string_view> rightColumnSet(nameAllocator);
					rightColumnSet.reserve(rightColumns.size());
					for (const std::string &column: rightColumns) {
						rightColumnSet.insert(column);
					}
					for (const std::string &column: leftColumns) {
						if (rightColumnSet.contains(column)) {
							sharedColumns.push_back(&column);
						}
					}
					// Build a bounded hash index over shared bindings. A join with no
					// shared columns intentionally retains Cartesian semantics by using
					// the same candidate list for every left row.
					detail::BudgetMemoryResource rightIndexMemory(state.shared.materialized_budget);
					std::pmr::unordered_map<std::pmr::string, std::pmr::vector<size_t>> rightRowsByKey{&rightIndexMemory};
					const bool rightCompleted = visitOperator(*right, state, [&](LogicalPlanReadRow row) {
						if (rightRows.size() == state.shared.bounds.maximum_rows) {
							throw std::runtime_error(
									"logical-plan Join exceeded its materialized right-side row bound");
						}
						LogicalPlanReadRow normalized = normalizeOutputRow(std::move(row), rightColumns);
						const auto key = bindingKey(normalized, sharedColumns, &rightIndexMemory);
						const size_t rowBytes = logicalReadRowBytes(normalized, state.shared.context);
						if (rowBytes > state.shared.bounds.maximum_materialized_bytes -
							std::min(rightRows.bytes(), state.shared.bounds.maximum_materialized_bytes))
							throw std::runtime_error("logical-plan Join exceeded its materialized byte bound");
						rightRows.append(std::move(normalized));
						rightRowsByKey[key].push_back(rightRows.size() - 1U);
						return true;
					});
					if (!rightCompleted) {
						throw std::runtime_error(
								"logical-plan Join could not prove that its materialized right input was exhausted");
					}
					if (rightRows.empty()) {
						return true;
					}

					bool stoppedByVisitor = false;
					const bool leftCompleted = visitOperator(*left, state, [&](LogicalPlanReadRow row) {
						MaterializedByteReservation leftReservation(&state.shared.materialized_budget);
						LogicalPlanReadRow normalizedLeft = normalizeOutputRow(std::move(row), leftColumns);
						const auto key = bindingKey(normalizedLeft, sharedColumns, &rightIndexMemory);
						leftReservation.set(logicalReadRowBytes(normalizedLeft, state.shared.context));
						const auto candidates = rightRowsByKey.find(key);
						if (candidates == rightRowsByKey.end()) {
							return true;
						}
						for (const size_t rightIndex: candidates->second) {
							checkGuard(state);
							const LogicalPlanReadRow &rightRow = rightRows.values()[rightIndex];
							const bool lastCandidate = rightIndex == candidates->second.back();
							MaterializedByteReservation outputReservation(&state.shared.materialized_budget);
							if (lastCandidate) {
								outputReservation.absorb(std::move(leftReservation));
								leftReservation = MaterializedByteReservation(&state.shared.materialized_budget);
							} else {
								outputReservation.set(logicalReadRowBytes(normalizedLeft, state.shared.context));
							}
							// Admit the right-side copy while its reusable source is still retained.
							outputReservation.set(checkedSizeAdd(outputReservation.bytes(),
								logicalReadRowBytes(rightRow, state.shared.context) - sizeof(LogicalPlanReadRow),
								"logical-plan combined row size overflowed"));
							LogicalPlanReadRow combined = lastCandidate ? std::move(normalizedLeft) : detail::copyLogicalReadRowGuarded(normalizedLeft, [&] { checkGuardNow(state); });
							if (lastCandidate) {
								normalizedLeft = {};
							}
							bool compatible = true;
							for (const auto &[column, value]: rightRow.values) {
								const auto existing = combined.values.find(column);
								if (existing != combined.values.end()) {
									if (!logicalValuesEqual(existing->second, value)) {
										compatible = false;
										break;
									}
									continue;
								}
								combined.values.emplace(column, detail::copyLogicalReadValueGuarded(value, [&] { checkGuardNow(state); }));
							}
							if (!compatible) {
								continue;
							}
							mergeCompatibleRowCaches(combined, rightRow, state);
							outputReservation.set(logicalReadRowBytes(combined, state.shared.context));
							bool keepGoing = true;
							if (chargedReceiver != nullptr) {
								keepGoing = (*chargedReceiver)(std::move(combined), std::move(outputReservation));
							} else {
								outputReservation.reset();
								keepGoing = visitor(std::move(combined));
							}
							if (!keepGoing) {
								stoppedByVisitor = true;
								return false;
							}
						}
						return true;
					});
					if (!leftCompleted && !stoppedByVisitor) {
						throw std::runtime_error(
								"logical-plan Join could not prove that its complete left input was read");
					}
					return leftCompleted;
				}
				case logical::LogicalOpType::LOP_OPTIONAL_MATCH: {
                    debug::PerfTrace::addValue("logical_plan.consumer_adapter.optional.calls", 1);
					const auto &optional = dynamic_cast<const logical::LogicalOptionalMatch &>(op);
					const logical::LogicalOperator *input = optional.getInput();
					const logical::LogicalOperator *pattern = optional.getOptionalPattern();
					if (input == nullptr || pattern == nullptr) {
						throw std::invalid_argument("logical-plan OptionalMatch requires two valid children");
					}
					MaterializedByteReservation inputNameBytes(&state.shared.materialized_budget), patternNameBytes(&state.shared.materialized_budget);
					const auto inputColumns = boundOutputNames(*input, state, inputNameBytes);
					const auto patternColumns = boundOutputNames(*pattern, state, patternNameBytes);
					detail::BudgetMemoryResource nameMemory(state.shared.materialized_budget);
					auto *nameAllocator = state.shared.bound_program ? static_cast<std::pmr::memory_resource *>(&nameMemory) : std::pmr::get_default_resource();
					std::pmr::unordered_set<std::string_view> inputColumnSet(nameAllocator);
					inputColumnSet.reserve(inputColumns.size());
					for (const std::string &column: inputColumns) {
						if (column.empty() || !inputColumnSet.insert(column).second) {
							throw std::invalid_argument("logical-plan OptionalMatch input has invalid output columns");
						}
					}
					std::pmr::unordered_set<std::string_view> patternColumnSet(nameAllocator);
					patternColumnSet.reserve(patternColumns.size());
					std::pmr::vector<const std::string *> sharedColumns(nameAllocator);
					sharedColumns.reserve(std::min(inputColumns.size(), patternColumns.size()));
					for (const std::string &column: patternColumns) {
						if (column.empty() || !patternColumnSet.insert(column).second) {
							throw std::invalid_argument(
									"logical-plan OptionalMatch pattern has invalid output columns");
						}
						if (inputColumnSet.contains(column)) {
							sharedColumns.push_back(&column);
						}
					}
					for (const std::string &variable: optional.getRequiredVariables()) {
						if (variable.empty() ||
							(!inputColumnSet.contains(variable) && !patternColumnSet.contains(variable))) {
							throw std::invalid_argument("logical-plan OptionalMatch has an invalid required variable");
						}
					}

					detail::BudgetMemoryResource patternIndexMemory(state.shared.materialized_budget);
					std::pmr::unordered_map<std::pmr::string, std::pmr::vector<size_t>> patternRowsByKey{&patternIndexMemory};
					BoundedBatch<LogicalPlanReadRow> patternRows(
							state.shared.bounds.maximum_rows, state.shared.bounds.maximum_materialized_bytes,
							[&state](const LogicalPlanReadRow &row) { return logicalReadRowBytes(row, state.shared.context); },
							&state.shared.materialized_budget,
                            state.shared.context && state.shared.context->resource_account, sizeof(LogicalPlanReadRow));
					patternRows.reserve(state.shared.bounds.maximum_rows);
					const bool patternCompleted = visitOperator(*pattern, state, [&](LogicalPlanReadRow row) {
						if (patternRows.size() == state.shared.bounds.maximum_rows) {
							throw std::runtime_error(
									"logical-plan OptionalMatch exceeded its materialized pattern row bound");
						}
						LogicalPlanReadRow normalized = normalizeOutputRow(std::move(row), patternColumns);
						const auto key = bindingKey(normalized, sharedColumns, &patternIndexMemory);
						const size_t rowBytes = logicalReadRowBytes(normalized, state.shared.context);
						if (rowBytes > state.shared.bounds.maximum_materialized_bytes -
							std::min(patternRows.bytes(), state.shared.bounds.maximum_materialized_bytes))
							throw std::runtime_error("logical-plan OptionalMatch exceeded its materialized byte bound");
						patternRows.append(std::move(normalized));
						patternRowsByKey[key].push_back(patternRows.size() - 1U);
						return true;
					});
					if (!patternCompleted) {
						throw std::runtime_error(
								"logical-plan OptionalMatch could not prove that its pattern input was exhausted");
					}

					bool stoppedByVisitor = false;
					const bool inputCompleted = visitOperator(*input, state, [&](LogicalPlanReadRow row) {
						MaterializedByteReservation inputReservation(&state.shared.materialized_budget);
						LogicalPlanReadRow normalizedInput = normalizeOutputRow(std::move(row), inputColumns);
						const auto key = bindingKey(normalizedInput, sharedColumns, &patternIndexMemory);
						inputReservation.set(logicalReadRowBytes(normalizedInput, state.shared.context));
						const auto matches = patternRowsByKey.find(key);
						if (matches == patternRowsByKey.end()) {
							LogicalPlanReadRow nullExtended = std::move(normalizedInput);
							normalizedInput = {};
							for (const std::string &column: patternColumns) {
								if (!inputColumnSet.contains(column)) {
									nullExtended.values.emplace(column, PropertyValue());
								}
							}
							MaterializedByteReservation outputReservation(&state.shared.materialized_budget);
							outputReservation.set(logicalReadRowBytes(nullExtended, state.shared.context));
							bool keepGoing = true;
							if (chargedReceiver != nullptr) {
								keepGoing = (*chargedReceiver)(std::move(nullExtended), std::move(outputReservation));
							} else {
								outputReservation.reset();
								keepGoing = visitor(std::move(nullExtended));
							}
							if (!keepGoing) {
								stoppedByVisitor = true;
								return false;
							}
							return true;
						}
						for (const size_t matchIndex: matches->second) {
							checkGuard(state);
							const LogicalPlanReadRow &match = patternRows.values()[matchIndex];
							const bool lastMatch = matchIndex == matches->second.back();
							MaterializedByteReservation outputReservation(&state.shared.materialized_budget);
							if (lastMatch) {
								outputReservation.absorb(std::move(inputReservation));
								inputReservation = MaterializedByteReservation(&state.shared.materialized_budget);
							} else {
								outputReservation.set(logicalReadRowBytes(normalizedInput, state.shared.context));
							}
							// Admit the right-side copy while its reusable source is still retained.
							outputReservation.set(checkedSizeAdd(outputReservation.bytes(),
								logicalReadRowBytesIncluding(match, [&](const std::string &alias) { return !inputColumnSet.contains(alias); }, state.shared.context) - sizeof(LogicalPlanReadRow),
								"logical-plan combined row size overflowed"));
							LogicalPlanReadRow combined = lastMatch ? std::move(normalizedInput) : detail::copyLogicalReadRowGuarded(normalizedInput, [&] { checkGuardNow(state); });
							if (lastMatch) {
								normalizedInput = {};
							}
							for (const auto &[column, value]: match.values) {
								if (!inputColumnSet.contains(column)) {
									combined.values.emplace(column, detail::copyLogicalReadValueGuarded(value, [&] { checkGuardNow(state); }));
								}
							}
							mergeCompatibleRowCaches(combined, match, state, &inputColumnSet);
							outputReservation.set(logicalReadRowBytes(combined, state.shared.context));
							bool keepGoing = true;
							if (chargedReceiver != nullptr) {
								keepGoing = (*chargedReceiver)(std::move(combined), std::move(outputReservation));
							} else {
								outputReservation.reset();
								keepGoing = visitor(std::move(combined));
							}
							if (!keepGoing) {
								stoppedByVisitor = true;
								return false;
							}
						}
						return true;
					});
					if (!inputCompleted && !stoppedByVisitor) {
						throw std::runtime_error(
								"logical-plan OptionalMatch could not prove that its complete input was read");
					}
					return inputCompleted;
				}
				case logical::LogicalOpType::LOP_CALL_SUBQUERY: {
                    debug::PerfTrace::addValue("logical_plan.consumer_adapter.subquery.calls", 1);
					const auto &subquery = dynamic_cast<const logical::LogicalCallSubquery &>(op);
					const logical::LogicalOperator *input = subquery.getInput();
					const logical::LogicalOperator *body = subquery.getSubquery();
					if (body == nullptr || subquery.isInTransactions() || subquery.getBatchSize() != 0) {
						throw std::invalid_argument("logical-plan CallSubquery has an unsupported or invalid shape");
					}
					const std::vector<std::string> returnedVariables = subquery.getReturnedVars();
					std::unordered_set<std::string> returnedSet;
					returnedSet.reserve(returnedVariables.size());
					for (const std::string &variable: returnedVariables) {
						if (variable.empty() || !returnedSet.insert(variable).second) {
							throw std::invalid_argument("logical-plan CallSubquery has invalid returned variables");
						}
					}
					const auto executeForInput = [&](LogicalPlanReadRow inputRow) {
						MaterializedByteReservation outerReservation(&state.shared.materialized_budget);
						MaterializedByteReservation importedReservation(&state.shared.materialized_budget);
						LogicalPlanReadRow retainedInput = std::move(inputRow);
						outerReservation.set(logicalReadRowBytes(retainedInput, state.shared.context));
						const std::vector<std::string> &importedVars = subquery.getImportedVars();
						for (size_t index = 0U; index < importedVars.size(); ++index) {
							const std::string &variable = importedVars[index];
							if (variable.empty()) {
								throw std::invalid_argument("logical-plan CallSubquery has an empty imported variable");
							}
							if (!retainedInput.values.contains(variable) ||
								std::ranges::find(importedVars.begin(), importedVars.begin() + index, variable) !=
										importedVars.begin() + index) {
								throw std::runtime_error(
										"logical-plan CallSubquery could not resolve a unique imported variable");
							}
						}
						const size_t importedBytes =
								logicalReadRowBytesIncluding(retainedInput, [&](const std::string &name) {
									return std::ranges::find(importedVars, name) != importedVars.end();
								}, state.shared.context);
						admitReferenceResultBytes(importedReservation, importedBytes,
												  "logical-plan CallSubquery imports exceeded its shared byte bound");
						LogicalPlanReadRow injected;
						injected.values.reserve(importedVars.size());
						injected.property_cache.reserve(importedVars.size());
						for (const std::string &variable: importedVars) {
							const auto value = retainedInput.values.find(variable);
							injected.values.emplace(variable, detail::copyLogicalReadValueGuarded(value->second, [&] { checkGuardNow(state); }));
							if (const auto properties = retainedInput.property_cache.find(variable);
								properties != retainedInput.property_cache.end()) {
								injected.property_cache.emplace(variable, detail::copyPropertyMapGuarded(properties->second, [&] { checkGuardNow(state); }));
								if (retainedInput.complete_entity_property_cache.contains(variable)) {
									injected.complete_entity_property_cache.insert(variable);
								}
							}
							if (const auto metadata = retainedInput.entity_metadata_cache.find(variable);
								metadata != retainedInput.entity_metadata_cache.end()) {
								injected.entity_metadata_cache.emplace(variable, detail::copyPropertyMapGuarded(metadata->second, [&] { checkGuardNow(state); }));
							}
							if (const auto path = retainedInput.path_cache.find(variable);
								path != retainedInput.path_cache.end()) {
								injected.path_cache.emplace(variable, detail::copyLogicalReadPathGuarded(path->second, [&] { checkGuardNow(state); }));
							}
						}
						struct InjectionScope final {
							ExecutionState &state;
							const LogicalPlanReadRow *previous;

							InjectionScope(ExecutionState &executionState, const LogicalPlanReadRow &row) :
								state(executionState), previous(executionState.task.injected_row) {
								state.task.injected_row = &row;
							}

							~InjectionScope() { state.task.injected_row = previous; }
						};
						admitReferenceResultBytes(importedReservation, logicalReadRowBytes(injected, state.shared.context),
								"logical-plan CallSubquery imports exceeded its shared byte bound");
						bool emitted = false;
						bool completed = false;
						{
							InjectionScope injection(state, injected);
							const auto acceptBodyRow = [&](LogicalPlanReadRow subqueryRow,
														   MaterializedByteReservation bodyReservation) {
								if (bodyReservation.bytes() == 0U) {
									admitReferenceResultBytes(
											bodyReservation, logicalReadRowBytes(subqueryRow, state.shared.context),
											"logical-plan CallSubquery body row exceeded its shared byte bound");
								}
								LogicalPlanReadRow normalized =
										normalizeOutputRow(std::move(subqueryRow), returnedVariables);
								admitReferenceResultBytes(
										bodyReservation, logicalReadRowBytes(normalized, state.shared.context),
										"logical-plan CallSubquery body row exceeded its shared byte bound");
								MaterializedByteReservation outputReservation(&state.shared.materialized_budget);
								admitReferenceResultBytes(
										outputReservation, logicalReadRowBytes(retainedInput, state.shared.context),
										"logical-plan CallSubquery output exceeded its shared byte bound");
								LogicalPlanReadRow combined = detail::copyLogicalReadRowGuarded(retainedInput, [&] { checkGuardNow(state); });
								for (const auto &[variable, value]: normalized.values) {
									if (combined.values.contains(variable)) {
										throw std::runtime_error(
												"logical-plan CallSubquery attempted to rebind an outer variable");
									}
								}
								combined.values.merge(normalized.values);
								mergeOwnedRowCaches(combined, std::move(normalized));
								outputReservation.absorb(std::move(bodyReservation));
								admitReferenceResultBytes(
										outputReservation, logicalReadRowBytes(combined, state.shared.context),
										"logical-plan CallSubquery output exceeded its shared byte bound");
								emitted = true;
								if (chargedReceiver != nullptr) {
									return (*chargedReceiver)(std::move(combined), std::move(outputReservation));
								}
								outputReservation.reset();
								return visitor(std::move(combined));
							};
							const ChargedRowVisitor chargedBodyRow = acceptBodyRow;
							completed = visitOperator(
									*body, state,
									[&](LogicalPlanReadRow row) {
										return acceptBodyRow(std::move(row),
															 MaterializedByteReservation(&state.shared.materialized_budget));
									},
									supportsChargedRowHandoff(body->getType()) ? &chargedBodyRow : nullptr);
						}
						injected = {};
						importedReservation.reset();
						if (!completed) {
							return false;
						}
						if (!emitted && (returnedVariables.empty() || subquery.isOptionalMatch())) {
							LogicalPlanReadRow output = std::move(retainedInput);
							retainedInput = {};
                            if (subquery.isOptionalMatch()) {
                                for (const auto &variable : returnedVariables) output.values.emplace(variable, PropertyValue());
                                admitReferenceResultBytes(outerReservation, logicalReadRowBytes(output, state.shared.context),
                                    "logical-plan correlated OptionalMatch NULL output exceeded its shared byte bound");
                            }
							if (chargedReceiver != nullptr) {
								return (*chargedReceiver)(std::move(output), std::move(outerReservation));
							}
							outerReservation.reset();
							return visitor(std::move(output));
						}
						return true;
					};
					if (input == nullptr) {
						return executeForInput({});
					}
					// Read-only correlated subqueries commonly consist of a unary
					// expression pipeline over the imported row. Prefetch both selected
					// Properties and complete-Entity requirements across outer rows before
					// invoking that pipeline; the body still evaluates each row normally,
					// so filtering, cardinality, and cancellation semantics do not change.
					const std::vector<const expressions::Expression *> subqueryExpressions =
							collectCorrelatedSubqueryExpressions(*body);
					const std::vector<const expressions::Expression *> batchableSubqueryExpressions =
							selectBatchableExpressions(subqueryExpressions);
					const std::vector<std::string> completeEntityVariables =
							selectBatchableCompleteEntityVariables(subqueryExpressions);
					if (!batchableSubqueryExpressions.empty() || !completeEntityVariables.empty()) {
						if (const std::optional<bool> batched =
									tryVisitBatchedPropertyExpressions(*input, batchableSubqueryExpressions, state,
																	   executeForInput, {}, completeEntityVariables);
							batched.has_value()) {
							return *batched;
						}
					}
					return visitOperator(*input, state, executeForInput);
				}
