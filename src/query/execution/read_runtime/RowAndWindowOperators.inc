				case logical::LogicalOpType::LOP_UNWIND: {
                    debug::PerfTrace::addValue("logical_plan.consumer_adapter.unwind.calls", 1);
					const auto &unwind = dynamic_cast<const logical::LogicalUnwind &>(op);
					if (unwind.getAlias().empty()) {
						throw std::invalid_argument("logical-plan UNWIND has an empty alias");
					}
					std::optional<detail::BoundExpressionInputs> boundInputs;
					if (state.shared.bound_program && unwind.getListExpr()) {
						const expressions::Expression *candidate = unwind.getListExpr().get();
						boundInputs.emplace(*state.shared.bound_program, state.shared.bound_program->id(op),
							std::span<const expressions::Expression *const>(&candidate, 1U), state.shared.snapshot,
							state.shared.materialized_budget, [&] { checkGuardNow(state); }, std::pmr::get_default_resource(), state.shared.context);
					}
					const auto expandRows = [&](LogicalPlanReadRow input, MaterializedByteReservation inputCharge) {
						// One input may produce many output rows. Keep its existing charge
						// until expansion ends; the receiving buffer admits each new row.
						if (inputCharge.bytes() == 0U) {
							const size_t inputBytes = logicalReadRowBytes(input, state.shared.context);
							if (inputBytes > sizeof(LogicalPlanReadRow)) {
								// The empty row frame is stack-owned. Charge retained bindings
								// when an upstream operator cannot transfer a row reservation.
								inputCharge.set(inputBytes - sizeof(LogicalPlanReadRow));
							}
						}
						// Literal lists live with the plan; expression results live through
						// this invocation. Iterate either directly instead of copying the list.
						MaterializedByteReservation evaluatedBytes(&state.shared.materialized_budget);
						LogicalPlanReadValue evaluated;
						std::span<const PropertyValue> values;
						if (unwind.hasLiteralList()) {
							values = unwind.getListValues();
						} else {
							if (!unwind.getListExpr()) {
								throw std::invalid_argument("logical-plan UNWIND has no list expression");
							}
							const PropertyValue *scalar = nullptr;
							bool ownsEvaluated = false;
							if (const auto *parameter = dynamic_cast<const expressions::ParameterExpression *>(
										unwind.getListExpr().get());
								parameter != nullptr) {
								if (state.shared.context == nullptr) {
									throw std::runtime_error(
											"logical-plan parameter evaluation requires a QueryContext");
								}
								const auto found = state.shared.context->parameters.find(parameter->getParameterName());
								if (found == state.shared.context->parameters.end()) {
									throw std::runtime_error(
											"logical-plan read runtime could not resolve a query parameter");
								}
								scalar = &found->second;
							} else if (const auto *list = dynamic_cast<const expressions::ListLiteralExpression *>(
											   unwind.getListExpr().get());
									   list != nullptr && list->isStatic()) {
								scalar = &list->getValue();
							} else {
								ownsEvaluated = true;
								{ ScopedBoundInputs scopedInputs(state, boundInputs, input);
								evaluated = evaluateExpression(*unwind.getListExpr(), input, state); }
								scalar = std::get_if<PropertyValue>(&evaluated);
								if (scalar == nullptr) {
									// Entity expressions retain their existing scalar-ID conversion.
									evaluated = scalarValue(evaluated);
									scalar = std::get_if<PropertyValue>(&evaluated);
								}
							}
							if (ownsEvaluated) {
								evaluatedBytes.set(scalar->getType() == PropertyType::LIST
														   ? materializedListStorageBytes(*scalar, state)
														   : property_utils::getPropertyValueSize(*scalar));
							}
							if (scalar->getType() == PropertyType::LIST) {
								values = scalar->getList();
							}
							if (scalar->getType() != PropertyType::LIST && unwind.scalarAsSingleRow()) {
								// Match the established pipeline UNWIND contract: a scalar
								// expression contributes one row for each input row.
								values = std::span<const PropertyValue>(scalar, 1U);
							}
						}
						if (values.size() > state.shared.bounds.maximum_rows) {
							throw std::runtime_error("logical-plan UNWIND exceeded its input list bound");
						}
						for (const PropertyValue &value: values) {
							checkGuard(state);
							MaterializedByteReservation outputBytes(&state.shared.materialized_budget);
							outputBytes.set(checkedSizeAdd(logicalReadRowBytes(input, state.shared.context),
								checkedSizeAdd(unwind.getAlias().size(), sizeof(LogicalPlanReadValue),
									"logical-plan UNWIND output size overflowed"), "logical-plan UNWIND output size overflowed"));
							LogicalPlanReadRow expanded = detail::copyLogicalReadRowGuarded(input, [&] { checkGuardNow(state); });
							expanded.values.insert_or_assign(unwind.getAlias(), copyReservedPropertyValue(value, state, outputBytes));
							expanded.property_cache.erase(unwind.getAlias());
							expanded.complete_entity_property_cache.erase(unwind.getAlias());
							expanded.entity_metadata_cache.erase(unwind.getAlias());
							expanded.path_cache.erase(unwind.getAlias());
							const size_t rowBytes = logicalReadRowBytes(expanded, state.shared.context);
							if (rowBytes > state.shared.bounds.maximum_materialized_bytes) {
								throw std::runtime_error("logical-plan UNWIND exceeded its materialized byte bound");
							}
							bool keepGoing = true;
							if (chargedReceiver != nullptr) {
								outputBytes.set(rowBytes);
								keepGoing = (*chargedReceiver)(std::move(expanded), std::move(outputBytes));
							} else {
								outputBytes.reset();
								keepGoing = visitor(std::move(expanded));
							}
							if (!keepGoing) {
								return false;
							}
						}
						return true;
					};
					const auto emitUnwindRows = [&](LogicalPlanReadRow input) {
						return expandRows(std::move(input), MaterializedByteReservation(&state.shared.materialized_budget));
					};
					const ChargedRowVisitor chargedInput = expandRows;
					const logical::LogicalOperator &unwindChild = requireOnlyChild(unwind);
					if (state.shared.allow_cross_row_entity_batches && !unwind.hasLiteralList() &&
						unwind.getListExpr() != nullptr) {
						const std::vector<const expressions::Expression *> listExpressions{unwind.getListExpr().get()};
						if (const std::optional<bool> batched = tryVisitBatchedPropertyExpressions(
									unwindChild, listExpressions, state, emitUnwindRows, {}, {},
									chargedReceiver != nullptr ? &chargedInput : nullptr);
							batched.has_value()) {
							return *batched;
						}
					}
					return visitOperator(unwindChild, state, emitUnwindRows,
										 chargedReceiver != nullptr && supportsChargedRowHandoff(unwindChild.getType())
												 ? &chargedInput
												 : nullptr);
				}
				case logical::LogicalOpType::LOP_LOAD_CSV: {
                    debug::PerfTrace::addValue("logical_plan.consumer_adapter.csv.calls", 1);
					const auto &loadCsv = dynamic_cast<const logical::LogicalLoadCsv &>(op);
					if (state.shared.context == nullptr) {
						throw std::runtime_error(
								"logical LOAD CSV requires an execution context with an import policy");
					}
					if (loadCsv.getRowVariable().empty() || !loadCsv.getUrlExpr()) {
						throw std::invalid_argument("logical LOAD CSV requires a URI expression and row variable");
					}
					std::optional<detail::BoundExpressionInputs> boundInputs;
					if (state.shared.bound_program) {
						const expressions::Expression *candidate = loadCsv.getUrlExpr().get();
						boundInputs.emplace(*state.shared.bound_program, state.shared.bound_program->id(op),
							std::span<const expressions::Expression *const>(&candidate, 1U), state.shared.snapshot,
							state.shared.materialized_budget, [&] { checkGuardNow(state); }, std::pmr::get_default_resource(), state.shared.context);
					}
					const std::string &delimiter = loadCsv.getFieldTerminator();
					if (delimiter.empty() || delimiter.size() > 8U ||
						delimiter.find_first_of("\r\n\"") != std::string::npos) {
						throw std::invalid_argument("logical LOAD CSV has an invalid field terminator");
					}
					const auto streamRows = [&](LogicalPlanReadRow input, MaterializedByteReservation inputCharge) {
						checkGuard(state);
						if (input.values.contains(loadCsv.getRowVariable())) {
							throw std::runtime_error("logical LOAD CSV attempted to rebind its row variable");
						}
						const size_t inputBytes = logicalReadRowBytes(input, state.shared.context);
						MaterializedByteReservation retainedSource(&state.shared.materialized_budget);
						if (inputCharge.bytes() == 0U && inputBytes >= state.shared.materialized_budget.available()) {
							throw std::runtime_error("logical LOAD CSV input exhausted its materialized byte bound");
						}
						if (inputCharge.bytes() != 0U) {
							retainedSource = std::move(inputCharge);
						} else {
							retainedSource.set(inputBytes);
						}
						PropertyValue uri;
						{ ScopedBoundInputs scopedInputs(state, boundInputs, input);
							uri = scalarValue(evaluateExpression(*loadCsv.getUrlExpr(), input, state)); }
						const size_t uriBytes = property_utils::getPropertyValueSize(uri);
						const size_t inputAndUriBytes = checkedSizeAdd(retainedSource.bytes(), uriBytes,
																	   "logical LOAD CSV retained size overflowed");
						retainedSource.set(inputAndUriBytes);
						size_t pathBytesEstimate = 0U;
						if (uri.getType() == PropertyType::STRING) {
							const size_t uriCharacters = checkedSizeAdd(std::get<std::string>(uri.getVariant()).size(),
																		1U, "logical LOAD CSV path size overflowed");
							pathBytesEstimate = checkedSizeMul(uriCharacters, sizeof(std::filesystem::path::value_type),
															   "logical LOAD CSV path size overflowed");
							retainedSource.set(checkedSizeAdd(inputAndUriBytes, pathBytesEstimate,
															  "logical LOAD CSV retained size overflowed"));
						}
						const std::filesystem::path csvPath = resolveCsvImportFile(uri, *state.shared.context);
						const auto &nativePath = csvPath.native();
						const size_t pathBytes =
								checkedSizeMul(nativePath.capacity(), sizeof(std::filesystem::path::value_type),
											   "logical LOAD CSV path storage size overflowed");
						retainedSource.set(checkedSizeAdd(checkedSizeAdd(inputAndUriBytes, pathBytes,
																		 "logical LOAD CSV retained size overflowed"),
														  pathBytesEstimate,
														  "logical LOAD CSV retained size overflowed"));
						const std::string sourcePath = csvPath.string();
						const size_t sourcePathBytes = csvStringCapacityBytes(sourcePath);
						const size_t retainedInputSourcePathBytes = checkedSizeAdd(
								inputAndUriBytes,
								checkedSizeAdd(pathBytes, sourcePathBytes, "logical LOAD CSV retained size overflowed"),
								"logical LOAD CSV retained size overflowed");
						retainedSource.set(retainedInputSourcePathBytes);
						CsvReader reader(sourcePath, delimiter,
										 CsvReaderLimits{state.shared.context->csv_import_policy.maximum_record_bytes,
														 state.shared.context->csv_import_policy.maximum_columns,
														 state.shared.context->csv_import_policy.maximum_field_bytes,
														 state.shared.materialized_budget.available()},
										 [&state] { state.shared.context->checkGuardNow(); });

						std::vector<std::string> headers;
						size_t headerBytes = 0U;
						if (loadCsv.isWithHeaders() && reader.hasNext()) {
							headers = reader.nextRow();
							headerBytes = checkedSizeMul(headers.capacity(), sizeof(std::string),
														 "logical LOAD CSV header capacity overflowed");
							for (const auto &header: headers) {
								headerBytes = checkedSizeAdd(headerBytes, csvStringCapacityBytes(header),
															 "logical LOAD CSV header capacity overflowed");
								if (header.empty()) {
									throw std::runtime_error("logical LOAD CSV has empty or duplicate header names");
								}
							}
							// Borrow names for duplicate detection rather than allocating a
							// second set of strings and unaccounted hash buckets/nodes.
							const size_t validationBytes =
									checkedSizeAdd(headerBytes,
												   checkedSizeMul(headers.size(), sizeof(std::string_view),
																  "logical LOAD CSV header validation size overflowed"),
												   "logical LOAD CSV header validation size overflowed");
							if (validationBytes > state.shared.materialized_budget.available()) {
								throw std::runtime_error(
										"logical LOAD CSV header validation exceeded its materialized byte bound");
							}
							state.shared.context->checkGuardNow();
							std::vector<std::string_view> sortedHeaders;
							sortedHeaders.reserve(headers.size());
							const size_t actualValidationBytes = checkedSizeAdd(
									headerBytes,
									checkedSizeMul(sortedHeaders.capacity(), sizeof(std::string_view),
												   "logical LOAD CSV header validation capacity overflowed"),
									"logical LOAD CSV header validation capacity overflowed");
							if (actualValidationBytes > state.shared.materialized_budget.available()) {
								throw std::runtime_error(
										"logical LOAD CSV header validation exceeded its materialized byte bound");
							}
							for (const auto &header: headers)
								sortedHeaders.emplace_back(header);
							std::sort(sortedHeaders.begin(), sortedHeaders.end());
							state.shared.context->checkGuardNow();
							if (std::adjacent_find(sortedHeaders.begin(), sortedHeaders.end()) != sortedHeaders.end()) {
								throw std::runtime_error("logical LOAD CSV has empty or duplicate header names");
							}
						}

						const size_t retainedBytes = checkedSizeAdd(retainedSource.bytes(), headerBytes,
																	"logical LOAD CSV retained size overflowed");
						if (headerBytes >= state.shared.materialized_budget.available()) {
							throw std::runtime_error("logical LOAD CSV headers exhausted its materialized byte bound");
						}
						retainedSource.set(retainedBytes);
						size_t loadedRows = 0U;
						while (true) {
							if (reader.atEnd())
								break;
							// The row limit only needs an EOF probe to determine whether
							// another row exists. Do not parse or allocate that forbidden row.
							if (loadedRows == state.shared.context->csv_import_policy.maximum_rows) {
								throw std::runtime_error("logical LOAD CSV exceeded its row bound");
							}
							reader.setMaximumBufferedBytes(state.shared.materialized_budget.available());
							if (!reader.hasNext())
								break;
							checkGuard(state);
							std::vector<std::string> fields = reader.nextRow();
							// Predict conversion storage before reserve/emplace. Field strings are
							// moved, but their vector slots coexist with the output container.
							size_t fieldBytes = checkedSizeMul(fields.capacity(), sizeof(std::string),
															   "logical LOAD CSV field capacity overflowed");
							for (const auto &field: fields) {
								fieldBytes = checkedSizeAdd(fieldBytes, csvStringCapacityBytes(field),
															"logical LOAD CSV field capacity overflowed");
							}
							MaterializedByteReservation rowReservation(&state.shared.materialized_budget);
							admitReferenceResultBytes(rowReservation, fieldBytes,
													  "logical LOAD CSV fields exceeded their materialized byte bound");
							size_t constructionBytes = checkedSizeAdd(sizeof(PropertyValue), fieldBytes,
																	  "logical LOAD CSV conversion size overflowed");
							const size_t valueCount =
									loadCsv.isWithHeaders() ? std::min(headers.size(), fields.size()) : fields.size();
							size_t mapNodeStorageBytes = 0U;
							if (loadCsv.isWithHeaders()) {
								// Include the link, a possible cached hash, and alignment slack in
								// addition to the key/value object. The bucket array is charged after
								// reserve using the implementation's reported bucket count.
								constexpr size_t mapNodeEstimate = sizeof(PropertyValue::MapType::value_type) +
																   2U * sizeof(void *) +
																   alignof(PropertyValue::MapType::value_type) - 1U;
								mapNodeStorageBytes = checkedSizeMul(valueCount, mapNodeEstimate,
																	 "logical LOAD CSV map node storage overflowed");
								constructionBytes = checkedSizeAdd(constructionBytes, mapNodeStorageBytes,
																   "logical LOAD CSV conversion size overflowed");
							} else {
								constructionBytes =
										checkedSizeAdd(constructionBytes,
													   checkedSizeMul(valueCount, sizeof(PropertyValue),
																	  "logical LOAD CSV list slot size overflowed"),
													   "logical LOAD CSV conversion size overflowed");
							}
							size_t valueBytes = sizeof(uint32_t);
							for (size_t index = 0U; index < valueCount; ++index) {
									if ((index & 255U) == 0U && state.shared.context != nullptr) state.shared.context->checkGuardNow();
								valueBytes = checkedSizeAdd(valueBytes, sizeof(size_t),
															"logical LOAD CSV value size overflowed");
								valueBytes = checkedSizeAdd(valueBytes, fields[index].size(),
															"logical LOAD CSV value size overflowed");
								if (loadCsv.isWithHeaders()) {
									constructionBytes =
											checkedSizeAdd(constructionBytes, csvStringCapacityBytes(headers[index]),
														   "logical LOAD CSV header-key capacity overflowed");
									valueBytes = checkedSizeAdd(valueBytes, sizeof(size_t),
																"logical LOAD CSV value size overflowed");
									valueBytes = checkedSizeAdd(valueBytes, headers[index].size(),
																"logical LOAD CSV value size overflowed");
								}
							}
							size_t outputContainerBytes = 0U;
							size_t outputBytes = checkedSizeAdd(inputBytes, loadCsv.getRowVariable().size(),
																"logical LOAD CSV output size overflowed");
							outputBytes = checkedSizeAdd(outputBytes, sizeof(LogicalPlanReadValue),
														 "logical LOAD CSV output size overflowed");
							outputBytes =
									checkedSizeAdd(outputBytes, valueBytes, "logical LOAD CSV output size overflowed");
							outputBytes = checkedSizeAdd(outputBytes, sizeof(PropertyValue),
														 "logical LOAD CSV handoff size overflowed");
							admitReferenceResultBytes(
									rowReservation, constructionBytes,
									"logical LOAD CSV conversion exceeded its materialized byte bound");
							PropertyValue rowValue;
							if (loadCsv.isWithHeaders()) {
								PropertyValue::MapType values;
								values.reserve(valueCount);
								const size_t requestedBucketBytes = checkedSizeMul(
										valueCount, sizeof(void *), "logical LOAD CSV map bucket estimate overflowed");
								const size_t actualBucketBytes =
										checkedSizeMul(values.bucket_count(), sizeof(void *),
													   "logical LOAD CSV map bucket capacity overflowed");
								if (actualBucketBytes > requestedBucketBytes) {
									constructionBytes =
											checkedSizeAdd(constructionBytes, actualBucketBytes - requestedBucketBytes,
														   "logical LOAD CSV map bucket capacity overflowed");
									admitReferenceResultBytes(
											rowReservation, constructionBytes,
											"logical LOAD CSV conversion exceeded its materialized byte bound");
								}
								outputContainerBytes = checkedSizeAdd(mapNodeStorageBytes, actualBucketBytes,
																	  "logical LOAD CSV map storage overflowed");
								for (size_t index = 0U; index < valueCount; ++index) {
									if ((index & 255U) == 0U && state.shared.context != nullptr) state.shared.context->checkGuardNow();
									const size_t headerCapacity = csvStringCapacityBytes(headers[index]);
									if (headerCapacity > headers[index].size()) {
										outputContainerBytes = checkedSizeAdd(
												outputContainerBytes, headerCapacity - headers[index].size(),
												"logical LOAD CSV output key capacity overflowed");
									}
									const size_t fieldCapacity = csvStringCapacityBytes(fields[index]);
									if (fieldCapacity > fields[index].size()) {
										outputContainerBytes = checkedSizeAdd(
												outputContainerBytes, fieldCapacity - fields[index].size(),
												"logical LOAD CSV output value capacity overflowed");
									}
								}
								for (size_t index = 0U; index < headers.size() && index < fields.size(); ++index) {
									if ((index & 255U) == 0U && state.shared.context != nullptr) state.shared.context->checkGuardNow();
									values.emplace(headers[index], PropertyValue(std::move(fields[index])));
								}
								rowValue = PropertyValue(std::move(values));
							} else {
								std::vector<PropertyValue> values;
								values.reserve(fields.size());
								const size_t requestedValueSlots =
										checkedSizeMul(valueCount, sizeof(PropertyValue),
													   "logical LOAD CSV value slot estimate overflowed");
								const size_t actualValueSlots =
										checkedSizeMul(values.capacity(), sizeof(PropertyValue),
													   "logical LOAD CSV value slot capacity overflowed");
								if (actualValueSlots > requestedValueSlots) {
									constructionBytes =
											checkedSizeAdd(constructionBytes, actualValueSlots - requestedValueSlots,
														   "logical LOAD CSV value slot capacity overflowed");
									admitReferenceResultBytes(
											rowReservation, constructionBytes,
											"logical LOAD CSV conversion exceeded its materialized byte bound");
								}
								outputContainerBytes = actualValueSlots;
								for (size_t index = 0U; index < valueCount; ++index) {
									if ((index & 255U) == 0U && state.shared.context != nullptr) state.shared.context->checkGuardNow();
									const size_t fieldCapacity = csvStringCapacityBytes(fields[index]);
									if (fieldCapacity > fields[index].size()) {
										outputContainerBytes = checkedSizeAdd(
												outputContainerBytes, fieldCapacity - fields[index].size(),
												"logical LOAD CSV output value capacity overflowed");
									}
								}
								size_t convertedFields = 0U;
								for (std::string &field: fields) {
									if ((convertedFields++ & 255U) == 0U && state.shared.context != nullptr) state.shared.context->checkGuardNow();
									values.emplace_back(std::move(field));
								}
								rowValue = PropertyValue(std::move(values));
							}
							outputBytes = checkedSizeAdd(outputBytes, outputContainerBytes,
														 "logical LOAD CSV output container capacity overflowed");
							// Drop moved-from slots and any extra columns before copying the input.
							std::vector<std::string>().swap(fields);
							admitReferenceResultBytes(
									rowReservation, outputBytes,
									"logical LOAD CSV conversion exceeded its materialized byte bound");
							++loadedRows;
							LogicalPlanReadRow expanded = detail::copyLogicalReadRowGuarded(input, [&] { checkGuardNow(state); });
							if (!bindValue(expanded, loadCsv.getRowVariable(), std::move(rowValue))) {
								throw std::runtime_error("logical LOAD CSV attempted to rebind its row variable");
							}
							expanded.property_cache.erase(loadCsv.getRowVariable());
							expanded.complete_entity_property_cache.erase(loadCsv.getRowVariable());
							expanded.entity_metadata_cache.erase(loadCsv.getRowVariable());
							const size_t rowBytes = logicalReadRowBytes(expanded, state.shared.context);
							admitReferenceResultBytes(rowReservation, std::max(rowBytes, outputBytes),
													  "logical LOAD CSV exceeded its materialized byte bound");
							// BoundedBatch requires its exact logical row charge. Preserve CSV's
							// additional capacity separately until the receiver returns.
							auto csvCapacityReservation = rowReservation.splitOff(
								outputBytes > rowBytes ? outputBytes - rowBytes : 0U);
							bool keepGoing = true;
							if (chargedReceiver != nullptr) {
								keepGoing = (*chargedReceiver)(std::move(expanded), std::move(rowReservation));
							} else {
								rowReservation.reset();
								keepGoing = visitor(std::move(expanded));
							}
							if (!keepGoing) {
								return false;
							}
						}
						return true;
					};

					const std::vector<logical::LogicalOperator *> children = loadCsv.getChildren();
					if (children.empty()) {
						return streamRows({}, MaterializedByteReservation(&state.shared.materialized_budget));
					}
					if (children.size() != 1U || children.front() == nullptr) {
						throw std::invalid_argument("logical LOAD CSV has an invalid input pipeline");
					}
					const auto streamUnchargedRow = [&](LogicalPlanReadRow input) {
						return streamRows(std::move(input), MaterializedByteReservation(&state.shared.materialized_budget));
					};
					const ChargedRowVisitor chargedStreamRows = streamRows;
					return visitOperator(*children.front(), state, streamUnchargedRow,
										 supportsChargedRowHandoff(children.front()->getType()) ? &chargedStreamRows
																								: nullptr);
				}
				case logical::LogicalOpType::LOP_UNION: {
                    debug::PerfTrace::addValue("logical_plan.consumer_adapter.union.calls", 1);
					const auto &unionOperator = dynamic_cast<const logical::LogicalUnion &>(op);
					const logical::LogicalOperator *left = unionOperator.getLeft();
					const logical::LogicalOperator *right = unionOperator.getRight();
					if (left == nullptr || right == nullptr) {
						throw std::invalid_argument("logical-plan UNION requires two valid children");
					}
					MaterializedByteReservation leftNameBytes(&state.shared.materialized_budget), rightNameBytes(&state.shared.materialized_budget);
					const auto columns = boundOutputNames(*left, state, leftNameBytes);
					const auto rightColumns = boundOutputNames(*right, state, rightNameBytes);
					if (columns.empty() || columns != rightColumns) {
						throw std::invalid_argument(
								"logical-plan UNION children must declare the same nonempty output columns");
					}
					detail::BudgetMemoryResource nameMemory(state.shared.materialized_budget);
					std::pmr::unordered_set<std::string_view> uniqueColumns(state.shared.bound_program
						? static_cast<std::pmr::memory_resource *>(&nameMemory) : std::pmr::get_default_resource());
					uniqueColumns.reserve(columns.size());
					for (const std::string &column: columns) {
						if (column.empty() || !uniqueColumns.insert(column).second) {
							throw std::invalid_argument("logical-plan UNION has invalid output columns");
						}
					}

					detail::BudgetMemoryResource distinctMemory(state.shared.materialized_budget);
					std::pmr::unordered_set<std::pmr::string> distinctRows{&distinctMemory};
					std::optional<detail::ExternalQueryKeySet> externalDistinct;
					if (!unionOperator.isAll() && state.shared.allow_cross_row_entity_batches &&
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
					const auto visitChild = [&](const logical::LogicalOperator &child) {
						const auto acceptRow = [&](LogicalPlanReadRow row, MaterializedByteReservation charge) {
							LogicalPlanReadRow output = normalizeOutputRow(std::move(row), columns);
							if (!unionOperator.isAll()) {
								auto key = distinctUnionRowKey<std::pmr::string>(output, columns, &distinctMemory);
								if (externalDistinct) {
									if (!externalDistinct->insert(std::move(key))) return true;
								} else {
									if (distinctRows.contains(key)) return true;
									if (distinctRows.size() == state.shared.bounds.maximum_distinct_rows)
										throw std::runtime_error("logical-plan UNION DISTINCT exceeded its row bound");
									distinctRows.insert(std::move(key));
								}
							}
							if (chargedReceiver != nullptr) {
								if (charge.bytes() != 0U)
									charge.set(logicalReadRowBytes(output, state.shared.context));
								return (*chargedReceiver)(std::move(output), std::move(charge));
							}
							charge.reset();
							return visitor(std::move(output));
						};
						const ChargedRowVisitor chargedInput = acceptRow;
						return visitOperator(
								child, state,
								[&](LogicalPlanReadRow row) {
									return acceptRow(std::move(row),
													 MaterializedByteReservation(&state.shared.materialized_budget));
								},
								chargedReceiver != nullptr && supportsChargedRowHandoff(child.getType()) ? &chargedInput
																										 : nullptr);
					};
					return finishDistinct(visitChild(*left) && visitChild(*right));
				}
				case logical::LogicalOpType::LOP_SORT: {
                    debug::PerfTrace::addValue("logical_plan.consumer_adapter.sort.calls", 1);
					const auto &sort = dynamic_cast<const logical::LogicalSort &>(op);
					const auto &items = sort.getSortItems();
					if (items.empty()) {
						throw std::invalid_argument("logical-plan Sort has no sort items");
					}
					for (const logical::LogicalSortItem &item: items) {
						if (!item.expression) {
							throw std::invalid_argument("logical-plan Sort has an empty sort expression");
						}
					}
					if (state.task.topk_sort == &sort && state.task.topk_rows != 0U)
						return visitBoundedTopKSort(sort, state, visitor, chargedReceiver, state.task.topk_rows);
					if(state.shared.allow_cross_row_entity_batches && state.shared.bounds.maximum_materialized_bytes>=detail::ExternalQuerySort::MINIMUM_QUERY_BYTES)
						if(auto *provider=dynamic_cast<storage::LogicalQueryScratchProvider *>(&state.shared.snapshot);provider && provider->queryScratchEnabled())
							return visitSpillableSort(sort,state,visitor,chargedReceiver,*provider);

					struct DecoratedRow {
						LogicalPlanReadRow row;
						std::vector<PropertyValue> keys;
					};
					const auto decoratedRowBytes = [&state](const DecoratedRow &decorated) {
						size_t bytes = logicalReadRowBytes(decorated.row, state.shared.context);
						for (const PropertyValue &key: decorated.keys) {
							bytes = checkedSizeAdd(bytes, retainedExpressionValueBytes(key, state),
												   "logical-plan Sort materialization size overflowed");
						}
						return bytes;
					};
					// Destroy buffered key vectors before their slot reservation on
					// cancellation or exception as well as on the normal output path.
					MaterializedByteReservation keySlots(&state.shared.materialized_budget);
					MaterializedByteReservation enrichmentPayload(&state.shared.materialized_budget);
					BoundedBatch<DecoratedRow> rows(state.shared.bounds.maximum_rows, state.shared.bounds.maximum_materialized_bytes,
													decoratedRowBytes, &state.shared.materialized_budget,
                            state.shared.context && state.shared.context->resource_account, sizeof(LogicalPlanReadRow));
					// Start with one bounded batch, not the complete cardinality/byte
					// allowance. Small grouped results must not allocate millions of
					// unused sort slots; the vector grows for actual input rows.
					rows.reserve(std::min({DEFAULT_PROPERTY_BATCH_ROWS, state.shared.bounds.maximum_rows,
										   state.shared.bounds.maximum_node_records}));
					const auto collectChargedRow = [&](LogicalPlanReadRow row, MaterializedByteReservation charge) {
						if (rows.size() == state.shared.bounds.maximum_rows) {
							throw std::runtime_error("logical-plan Sort exceeded its materialization row bound");
						}
						// Delay expression evaluation until we know ordering can affect the
						// result. This also preserves valid single-row aggregate results when
						// an otherwise unavailable input-only ORDER BY expression is present.
						if (charge.bytes() == 0U)
							rows.append({std::move(row), {}});
						else
							rows.appendCharged({std::move(row), {}}, std::move(charge));
						return true;
					};
					const auto collectRow = [&](LogicalPlanReadRow row) {
						return collectChargedRow(std::move(row),
												 MaterializedByteReservation(&state.shared.materialized_budget));
					};
					std::vector<const expressions::Expression *> sortCandidates;
					sortCandidates.reserve(items.size());
					for (const logical::LogicalSortItem &item: items) {
						sortCandidates.push_back(item.expression.get());
					}
					const std::vector<const expressions::Expression *> sortExpressions =
							selectBatchableExpressions(sortCandidates);
					const std::vector<std::string> completeEntityVariables =
							selectBatchableCompleteEntityVariables(sortCandidates);
					const logical::LogicalOperator &child = requireOnlyChild(sort);
					const ChargedRowVisitor chargedCollect = collectChargedRow;
					const std::optional<bool> batched = tryVisitBatchedPropertyExpressions(
							child, sortExpressions, state, collectRow, {}, completeEntityVariables, &chargedCollect);
					const bool completed =
							batched.has_value()
									? *batched
									: visitOperator(child, state, collectRow,
													supportsChargedRowHandoff(child.getType()) ? &chargedCollect
																							   : nullptr);
					if (!completed) {
						throw std::runtime_error(
								"logical-plan Sort could not prove that its materialized input was exhausted");
					}
					size_t slotsPerRow = 0U;
					if (rows.size() > 1U) {
						std::optional<detail::BoundExpressionInputs> boundInputs;
						if (state.shared.bound_program) boundInputs.emplace(*state.shared.bound_program,
							state.shared.bound_program->id(op), sortCandidates, state.shared.snapshot, state.shared.materialized_budget,
							[&] { checkGuardNow(state); }, std::pmr::get_default_resource(), state.shared.context);
						slotsPerRow = checkedSizeMul(items.size(), sizeof(PropertyValue),
													 "logical-plan Sort key-slot size overflowed");
						size_t keySlotBytes =
								checkedSizeMul(rows.size(), slotsPerRow, "logical-plan Sort key-slot size overflowed");
						if (keySlotBytes > state.shared.bounds.maximum_materialized_bytes -
												   std::min(rows.bytes(), state.shared.bounds.maximum_materialized_bytes) ||
							keySlotBytes > state.shared.materialized_budget.available()) {
							throw MaterializedByteLimitExceeded();
						}
						keySlots.set(keySlotBytes);
						size_t additionalEnrichmentBytes = 0U;
						for (DecoratedRow &row: rows.values()) {
							row.keys.reserve(items.size());
							const size_t actualSlots = checkedSizeMul(row.keys.capacity(), sizeof(PropertyValue),
									"logical-plan Sort key-slot size overflowed");
							const size_t extraSlots = actualSlots - slotsPerRow;
							const size_t retainedBytes = checkedSizeAdd(
								checkedSizeAdd(rows.bytes(), keySlotBytes,
									"logical-plan Sort materialization size overflowed"),
								additionalEnrichmentBytes, "logical-plan Sort materialization size overflowed");
							if (extraSlots > state.shared.bounds.maximum_materialized_bytes -
									std::min(retainedBytes, state.shared.bounds.maximum_materialized_bytes) ||
								extraSlots > state.shared.materialized_budget.available()) {
								throw MaterializedByteLimitExceeded();
							}
							keySlotBytes = checkedSizeAdd(keySlotBytes, extraSlots,
								"logical-plan Sort key-slot size overflowed");
							keySlots.set(keySlotBytes);
							size_t accountedRowBytes = logicalReadRowBytes(row.row, state.shared.context);
							for (const logical::LogicalSortItem &item: items) {
								checkGuard(state);
								PropertyValue key;
								{ ScopedBoundInputs scopedInputs(state, boundInputs, row.row);
									key = scalarValue(evaluateExpression(*item.expression, row.row, state)); }
								const size_t keyBytes = retainedExpressionValueBytes(key, state);
								const size_t currentRowBytes = logicalReadRowBytes(row.row, state.shared.context);
								const size_t rowGrowth = currentRowBytes > accountedRowBytes ?
									currentRowBytes - accountedRowBytes : 0U;
								const size_t enrichmentBytes = checkedSizeAdd(keyBytes, rowGrowth,
									"logical-plan Sort cache enrichment size overflowed");
								if (enrichmentBytes >
									state.shared.bounds.maximum_materialized_bytes -
											std::min(
													checkedSizeAdd(checkedSizeAdd(rows.bytes(), keySlotBytes,
																				  "logical-plan Sort materialization "
																				  "size overflowed"),
																   additionalEnrichmentBytes,
																   "logical-plan Sort materialization size overflowed"),
													state.shared.bounds.maximum_materialized_bytes)) {
									throw std::runtime_error(
											"logical-plan Sort exceeded its materialization byte bound");
								}
								if (enrichmentBytes >
									state.shared.materialized_budget.available()) {
									throw std::runtime_error(
											"logical-plan Sort keys exceeded the shared materialized byte bound");
								}
								additionalEnrichmentBytes =
										checkedSizeAdd(additionalEnrichmentBytes, enrichmentBytes,
													   "logical-plan Sort materialization size overflowed");
								enrichmentPayload.set(additionalEnrichmentBytes);
								accountedRowBytes = std::max(accountedRowBytes, currentRowBytes);
								row.keys.push_back(std::move(key));
							}
						}
						rows.recalculateBytes(std::move(enrichmentPayload));
						std::stable_sort(rows.values().begin(), rows.values().end(),
										 [&](const DecoratedRow &left, const DecoratedRow &right) {
											 checkGuard(state);
											 for (size_t index = 0U; index < items.size(); ++index) {
												 if (left.keys[index] == right.keys[index]) {
													 continue;
												 }
												 return detail::orderByPrecedes(left.keys[index], right.keys[index], items[index].ascending);
											 }
											 return false;
										 });
					}
					// Pop in sorted order so ownership leaves this batch before a
					// downstream buffer charges the same row. Free obsolete sort keys.
					std::reverse(rows.values().begin(), rows.values().end());
					while (!rows.empty()) {
						checkGuard(state);
						if (chargedReceiver != nullptr) {
							auto charged = rows.popBackCharged();
							const size_t releasedSlots = checkedSizeMul(charged.value.keys.capacity(), sizeof(PropertyValue),
								"logical-plan Sort key-slot size overflowed");
							std::vector<PropertyValue>().swap(charged.value.keys);
							keySlots.set(keySlots.bytes() - releasedSlots);
							charged.reservation.set(logicalReadRowBytes(charged.value.row, state.shared.context));
							if (!(*chargedReceiver)(std::move(charged.value.row), std::move(charged.reservation))) {
								return false;
							}
						} else {
							DecoratedRow row = rows.popBack();
							const size_t releasedSlots = checkedSizeMul(row.keys.capacity(), sizeof(PropertyValue),
								"logical-plan Sort key-slot size overflowed");
							std::vector<PropertyValue>().swap(row.keys);
							keySlots.set(keySlots.bytes() - releasedSlots);
							if (!visitor(std::move(row.row))) {
								return false;
							}
						}
					}
					return true;
				}
				case logical::LogicalOpType::LOP_SKIP: {
					const auto &skip = dynamic_cast<const logical::LogicalSkip &>(op);
					if (skip.getOffset() < 0) {
						throw std::invalid_argument("logical-plan SKIP cannot be negative");
					}
					const uint64_t offset = static_cast<uint64_t>(skip.getOffset());
					if (state.shared.bounds.maximum_rows == 0U) {
						throw std::runtime_error("logical-plan SKIP has no positive batch capacity");
					}
					size_t skipped = 0U;
					const auto acceptRow = [&](LogicalPlanReadRow row, MaterializedByteReservation charge) {
						checkGuard(state);
						if (static_cast<uint64_t>(skipped) < offset) {
							incrementBounded(skipped, "logical-plan SKIP counter overflowed");
							debug::PerfTrace::addValue("logical_plan.skip.discarded_rows", 1);
							return true;
						}
						enforceMaterializedRowBound(row, state);
						if (chargedReceiver != nullptr) {
							return (*chargedReceiver)(std::move(row), std::move(charge));
						}
						charge.reset();
						return visitor(std::move(row));
					};
					const logical::LogicalOperator &child = requireOnlyChild(skip);
					const ChargedRowVisitor chargedRow = acceptRow;
					return visitOperator(
							child, state,
							[&](LogicalPlanReadRow row) {
								return acceptRow(std::move(row),
												 MaterializedByteReservation(&state.shared.materialized_budget));
							},
							supportsChargedRowHandoff(child.getType()) ? &chargedRow : nullptr);
				}
				case logical::LogicalOpType::LOP_LIMIT: {
					const auto &limit = dynamic_cast<const logical::LogicalLimit &>(op);
					if (limit.getLimit() < 0) {
						throw std::invalid_argument("logical-plan LIMIT cannot be negative");
					}
					if (limit.getLimit() == 0) {
						return true;
					}
					const uint64_t maximum = static_cast<uint64_t>(limit.getLimit());
					if (state.shared.bounds.maximum_rows == 0U) {
						throw std::runtime_error("logical-plan LIMIT has no positive batch capacity");
					}
					size_t delivered = 0U;
					bool reachedLimit = false;
					bool cancelledByVisitor = false;
					const auto acceptRow = [&](LogicalPlanReadRow row, MaterializedByteReservation charge) {
						checkGuard(state);
						enforceMaterializedRowBound(row, state);
						incrementBounded(delivered, "logical-plan LIMIT counter overflowed");
						debug::PerfTrace::addValue("logical_plan.limit.delivered_rows", 1);
						bool keepGoing = true;
						if (chargedReceiver != nullptr) {
							keepGoing = (*chargedReceiver)(std::move(row), std::move(charge));
						} else {
							charge.reset();
							keepGoing = visitor(std::move(row));
						}
						if (!keepGoing)
							cancelledByVisitor = true;
						reachedLimit = static_cast<uint64_t>(delivered) >= maximum;
						if (!keepGoing || reachedLimit) {
							debug::PerfTrace::addValue("logical_plan.limit.early_stop", 1);
							return false;
						}
						return true;
					};
					const logical::LogicalOperator &child = requireOnlyChild(limit);
					struct RestoreTopK {
						ExecutionTaskState &task; const logical::LogicalSort *sort; size_t rows;
						~RestoreTopK() { task.topk_sort = sort; task.topk_rows = rows; }
					} restoreTopK{state.task, state.task.topk_sort, state.task.topk_rows};
					const logical::LogicalOperator *candidate = &child;
					while (candidate->getType() == logical::LogicalOpType::LOP_PROJECT &&
						!dynamic_cast<const logical::LogicalProject &>(*candidate).isDistinct())
						candidate = &requireOnlyChild(*candidate);
					if (candidate->getType() == logical::LogicalOpType::LOP_SORT &&
						maximum <= state.shared.bounds.maximum_rows) {
						state.task.topk_sort = &dynamic_cast<const logical::LogicalSort &>(*candidate);
						state.task.topk_rows = static_cast<size_t>(maximum);
					}

					const ChargedRowVisitor chargedRow = acceptRow;
					const bool childCompleted = visitOperator(
							child, state,
							[&](LogicalPlanReadRow row) {
								return acceptRow(std::move(row),
												 MaterializedByteReservation(&state.shared.materialized_budget));
							},
							supportsChargedRowHandoff(child.getType()) ? &chargedRow : nullptr);
					// Reaching the window exhausts its logical output even when the
					// physical child stopped; cancellation still propagates as incomplete.
					if (cancelledByVisitor)
						return false;
					return reachedLimit || childCompleted;
				}
