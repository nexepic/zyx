		struct ProcedureNodeOutputReference {
			uint64_t node_id = 0U;
			size_t row_index = 0U;
			size_t column_index = 0U;
		};

		// Keep procedure rows as logical references until a declared output column
		// actually needs a complete Node. This avoids opening Property roots for
		// scalar-only projections while preserving one sorted batch for bare Nodes.
		void materializeProcedureNodeOutputs(ProcedureRows &rows,
											 const std::vector<std::string> &columns,
											 storage::LogicalGraphReadSnapshot &snapshot,
											 const LogicalPlanReadRuntimeBounds &readBounds,
											 const QueryContext &context) {
			MaterializedByteBudget budget(readBounds.maximum_materialized_bytes);
			MaterializedByteReservation rowBytes(&budget);
			for (const auto &row: rows) admitProcedureBytes(rowBytes, checkedProcedureSizeAdd(rowBytes.bytes(), bufferedProcedureRowBytes(row)));
			MaterializedByteReservation referenceBytes(&budget);
			const auto visitOutputReferences = [&](auto &&visitor) {
				for (size_t rowIndex = 0U; rowIndex < rows.size(); ++rowIndex) {
					context.checkGuard();
					for (size_t columnIndex = 0U; columnIndex < columns.size(); ++columnIndex) {
						const auto found = rows[rowIndex].row.values.find(columns[columnIndex]);
						if (found == rows[rowIndex].row.values.end()) continue;
						const auto *reference = std::get_if<storage::EntityRef>(&found->second);
						if (reference == nullptr) continue;
						if (reference->is_edge || reference->entity_id == 0U) {
							throw std::logic_error("logical procedure output contains an invalid Node reference");
						}
						visitor(ProcedureNodeOutputReference{reference->entity_id, rowIndex, columnIndex});
					}
				}
			};
			size_t referenceCount = 0U;
			visitOutputReferences([&](const ProcedureNodeOutputReference &) {
				referenceCount = checkedProcedureSizeAdd(referenceCount, 1U);
			});
			referenceBytes.set(checkedProcedureSizeMul(referenceCount, sizeof(ProcedureNodeOutputReference)));
			std::vector<ProcedureNodeOutputReference, utils::OwnedPmrAllocator<ProcedureNodeOutputReference>> references;
			references.reserve(referenceCount);
			referenceBytes.set(checkedProcedureSizeMul(references.capacity(), sizeof(ProcedureNodeOutputReference)));
			visitOutputReferences([&](const ProcedureNodeOutputReference &reference) {
				references.push_back(reference);
			});
			if (references.empty()) return;
			std::ranges::sort(references, [](const ProcedureNodeOutputReference &left, const ProcedureNodeOutputReference &right) {
				if (left.node_id != right.node_id) return left.node_id < right.node_id;
				if (left.row_index != right.row_index) return left.row_index < right.row_index;
				return left.column_index < right.column_index;
			});
			MaterializedByteReservation idBytes(&budget);
			size_t uniqueIdCount = 0U;
			for (size_t index = 0U; index < references.size(); ++index) {
				if (index == 0U || references[index - 1U].node_id != references[index].node_id) {
					uniqueIdCount = checkedProcedureSizeAdd(uniqueIdCount, 1U);
				}
			}
			idBytes.set(checkedProcedureSizeMul(uniqueIdCount, sizeof(uint64_t)));
			utils::OwnedStdVector<uint64_t> sortedIds;
			sortedIds.reserve(uniqueIdCount);
			idBytes.set(checkedProcedureSizeMul(sortedIds.capacity(), sizeof(uint64_t)));
			for (const auto &reference: references) {
				if (sortedIds.empty() || sortedIds.back() != reference.node_id) {
					sortedIds.push_back(reference.node_id);
				}
			}
			const auto nodeBytes = [](const storage::LogicalGraphNode &node) {
				return bufferedProcedureNodeRowBytes(node, false) - sizeof(BufferedProcedureRow) - 4U -
					sizeof(LogicalPlanReadValue) + sizeof(storage::LogicalGraphNode);
			};
			struct NodeOutputPressure { const char *phase; size_t requested; };
			const auto processOnce = [&](size_t offset, size_t count) {
				context.checkGuard();
				const auto batch = std::span<const uint64_t>(sortedIds).subspan(offset, count);
				MaterializedByteReservation sourceBytes(&budget);
				const bool admitSource = snapshot.supportsShapedPropertyReadAdmission();
				if (admitSource) {
					const size_t frames = checkedProcedureSizeMul(count, sizeof(storage::LogicalGraphNode));
					if (sourceBytes.wouldExceed(frames)) throw NodeOutputPressure{"source frames", frames};
					sourceBytes.set(frames);
				}
				const std::function<void(size_t)> admission = [&](size_t encodedBytes) {
					context.checkGuard();
					const size_t next = checkedProcedureSizeAdd(sourceBytes.bytes(), encodedBytes);
					if (sourceBytes.wouldExceed(next)) throw NodeOutputPressure{"encoded Property", next};
					sourceBytes.set(next);
				};
				auto nodes = admitSource ? snapshot.readNodesShapedSortedWithAdmission(batch, completeNodeReadShape(), admission)
					: snapshot.readNodesShapedSorted(batch, completeNodeReadShape());
				context.checkGuardNow();
				if (nodes.size() != count) throw std::runtime_error("logical procedure Node batch changed its cardinality");
				size_t bytes = 0U;
				for (size_t index = 0U; index < count; ++index) {
					context.checkGuard();
					const auto &node = nodes[index];
					if (node.node_id != batch[index]) throw std::runtime_error("logical procedure Node batch changed logical ID order");
					validateNode(node);
					if (node.properties.size() > readBounds.maximum_property_entries) throw std::runtime_error("logical Vector procedure Node Properties exceeded its caller-supplied bound");
					std::unordered_set<std::string_view> keys;
					for (const auto &entry: node.properties) if (entry.key.empty() || !keys.insert(entry.key).second)
						throw std::runtime_error("logical Vector procedure read an invalid Node Property entry set");
					bytes = checkedProcedureSizeAdd(bytes, nodeBytes(node));
				}
				if (sourceBytes.wouldExceed(bytes)) throw NodeOutputPressure{"returned Node", bytes};
				sourceBytes.set(bytes);
				const auto firstReference = std::ranges::lower_bound(references, batch.front(), {}, &ProcedureNodeOutputReference::node_id);
				const size_t first = static_cast<size_t>(firstReference - references.begin());
				size_t referenceIndex = first;
				size_t plannedRows = rowBytes.bytes();
				size_t plannedSource = bytes;
				const size_t otherBytes = budget.used() - plannedRows - plannedSource;
				// Validate and admit the whole transition before touching any output row.
				for (const auto &node: nodes) {
					const size_t payload = nodeBytes(node) - sizeof(storage::LogicalGraphNode);
					while (referenceIndex < references.size() && references[referenceIndex].node_id == node.node_id) {
						context.checkGuard();
						++referenceIndex;
						const bool last = referenceIndex == references.size() || references[referenceIndex].node_id != node.node_id;
						if (last) plannedSource -= payload;
						plannedRows = checkedProcedureSizeAdd(plannedRows - sizeof(storage::EntityRef), payload);
						if (checkedProcedureSizeAdd(plannedRows, plannedSource) > readBounds.maximum_materialized_bytes - otherBytes) throw NodeOutputPressure{"publication", checkedProcedureSizeAdd(plannedRows, plannedSource)};
					}
				}
				referenceIndex = first;
				for (auto &node: nodes) {
					const size_t payload = nodeBytes(node) - sizeof(storage::LogicalGraphNode);
					while (referenceIndex < references.size() && references[referenceIndex].node_id == node.node_id) {
						context.checkGuard();
						const auto &reference = references[referenceIndex++];
						const bool last = referenceIndex == references.size() || references[referenceIndex].node_id != node.node_id;
						if (last) sourceBytes.set(sourceBytes.bytes() - payload);
						admitProcedureBytes(rowBytes, checkedProcedureSizeAdd(rowBytes.bytes() - sizeof(storage::EntityRef), payload));
						auto &value = rows[reference.row_index].row.values.at(columns[reference.column_index]);
						if (last) value = std::move(node);
						else value = node;
					}
				}
			};
			std::function<void(size_t, size_t)> processRange;
			processRange = [&](size_t offset, size_t count) {
				try { processOnce(offset, count); }
				catch (const NodeOutputPressure &pressure) {
					if (count == 1U) throw ProcedureByteBoundExceeded("logical procedure Node output exceeded its joint byte bound: phase=" + std::string(pressure.phase) +
						", requested=" + std::to_string(pressure.requested) + ", rows=" + std::to_string(rowBytes.bytes()) +
						", routing=" + std::to_string(referenceBytes.bytes() + idBytes.bytes()) + ", limit=" + std::to_string(readBounds.maximum_materialized_bytes));
					const size_t first = count / 2U;
					processRange(offset, first);
					processRange(offset + first, count - first);
				}
			};
			const size_t capacity = procedureNodeBatchCapacity(readBounds);
			for (size_t offset = 0U; offset < sortedIds.size(); offset += capacity) processRange(offset, std::min(capacity, sortedIds.size() - offset));
		}

		[[nodiscard]] ProcedureRows
		makeBufferedVectorProcedureRows(const std::span<const storage::LogicalGraphVectorSearchHit> hits,
										const QueryContext &context) {
			ProcedureRows rows;
			rows.reserve(hits.size());
			for (const auto &hit: hits) {
				context.checkGuard();
				BufferedProcedureRow buffered;
            detail::ProcedurePayloadAdmissionScope bufferedPayloadScope(buffered.allocation_credit);
				detail::procedureMapReserve(buffered.node_properties, 1U);
				detail::procedureMapEmplace(buffered.row.values, "node", storage::EntityRef{hit.node_id, false, 0U, 0U, 0U});
				detail::procedureMapEmplace(buffered.row.values, "score", makeProcedurePropertyValue(static_cast<double>(hit.score)));
				rows.push_back(std::move(buffered));
			}
			return rows;
		}

		[[nodiscard]] bool appendBufferedProcedureRows(ProcedureRows rows, storage::LogicalGraphReadSnapshot &snapshot,
								 const std::vector<const logical::LogicalOperator *> &operations,
								 const LogicalPlanReadRuntimeBounds &readBounds,
								 const QueryContext &context, QueryResult &result,
								 std::vector<ProcedureDistinctRows, utils::OwnedPmrAllocator<ProcedureDistinctRows>> *distinctRowsByOperation = nullptr,
								 std::vector<uint64_t> *operatorRowCounts = nullptr,
								 const size_t retainedSourceBytes = 0U,
								 const bool accountOutputTransition = false) {
			LogicalPlanReadRuntimeBounds batchBounds = readBounds;
			const size_t retainedBytes = checkedProcedureSizeAdd(
					retainedSourceBytes, procedureQueryResultBytes(result));
		if (retainedBytes > readBounds.maximum_materialized_bytes) {
			throw ProcedureByteBoundExceeded("logical procedure source and retained result exceeded their byte bound");
		}
		batchBounds.maximum_materialized_bytes -= retainedBytes;
			if (rows.size() > readBounds.maximum_rows) {
				throw std::runtime_error("logical procedure exceeded its configured result-row bound");
			}
			validateBufferedProcedureBytes(rows, batchBounds.maximum_materialized_bytes);
			utils::OwnedStdVector<uint64_t> localOperatorRowCounts(operations.size(), 0U);
			if (operatorRowCounts == nullptr) {
				operatorRowCounts = &localOperatorRowCounts;
			} else if (operatorRowCounts->size() != operations.size()) {
				throw std::logic_error("logical procedure operator state does not match its pipeline");
			}
		bool continueInput = true;

			for (size_t operationIndex = 0U; operationIndex < operations.size(); ++operationIndex) {
				const logical::LogicalOperator *operation = operations[operationIndex];
				context.checkGuard();
				if (rows.empty()) {
					break;
				}
				switch (operation->getType()) {
					case logical::LogicalOpType::LOP_PROJECT: {
						const auto &projection = dynamic_cast<const logical::LogicalProject &>(*operation);
						utils::OwnedStdVector<const expressions::Expression *> propertyExpressions;
						for (const auto &item: projection.getItems()) {
							propertyExpressions.push_back(item.expression.get());
						}
						prefetchProcedureProperties(rows, propertyExpressions, snapshot, batchBounds, context);
                        for (auto &row : rows) row.reconcilePayloadCredit();
						ProcedureDistinctRows localDistinctRows;
						ProcedureDistinctRows *distinctRows = nullptr;
						if (projection.isDistinct()) {
							if (distinctRowsByOperation != nullptr) {
								if (distinctRowsByOperation->size() != operations.size()) {
									throw std::logic_error("logical procedure DISTINCT state does not match its operator pipeline");
								}
								distinctRows = &distinctRowsByOperation->at(operationIndex);
							} else {
								distinctRows = &localDistinctRows;
							}
						}
						size_t outputCount = 0U;
						size_t pendingSourceBytes = 0U;
						for (const BufferedProcedureRow &buffered: rows) {
							pendingSourceBytes = checkedProcedureSizeAdd(pendingSourceBytes,
									bufferedProcedureRowBytes(buffered));
						}
						size_t retainedOutputBytes = 0U;
						for (size_t rowIndex = 0U; rowIndex < rows.size(); ++rowIndex) {
							BufferedProcedureRow &buffered = rows[rowIndex];
            detail::ProcedurePayloadAdmissionScope bufferedPayloadScope(buffered.allocation_credit);
							const size_t currentSourceBytes = bufferedProcedureRowBytes(buffered);
							if (currentSourceBytes > pendingSourceBytes) {
								throw std::logic_error("logical procedure pending source-byte accounting underflowed");
							}
							pendingSourceBytes -= currentSourceBytes;
							size_t retainedOtherBytes = checkedProcedureSizeAdd(retainedOutputBytes, pendingSourceBytes);
							// Rows not yet projected remain owned by the input batch while
							// completed outputs accumulate beside it. Include those live source
							// rows in the same allowance passed to this row's expressions.
							std::optional<LogicalPlanReadRow> projected = applyProjection(
								projection, snapshot, batchBounds, context, buffered.row, buffered.node_properties,
									distinctRows, retainedOtherBytes, true);
							if (projected.has_value()) {
								buffered.row = std::move(*projected);
								retainedOutputBytes = checkedProcedureSizeAdd(retainedOutputBytes,
										bufferedProcedureRowBytes(buffered));
								if (outputCount != rowIndex) rows[outputCount] = std::move(buffered);
								++outputCount;
							} else {
								// DISTINCT drops release payload before processing later rows.
								buffered = BufferedProcedureRow{};
							}
						}
						rows.resize(outputCount);
						validateBufferedProcedureBytes(rows, batchBounds.maximum_materialized_bytes);
						break;
					}
					case logical::LogicalOpType::LOP_FILTER: {
						const auto &filter = dynamic_cast<const logical::LogicalFilter &>(*operation);
						prefetchProcedureProperties(rows, {filter.getPredicate().get()}, snapshot, batchBounds, context);
						size_t outputCount = 0U;
						for (size_t rowIndex = 0U; rowIndex < rows.size(); ++rowIndex) {
							context.checkGuard();
							BufferedProcedureRow &buffered = rows[rowIndex];
            detail::ProcedurePayloadAdmissionScope bufferedPayloadScope(buffered.allocation_credit);
							if (applyFilter(filter, snapshot, batchBounds, context, buffered.row,
											buffered.node_properties)) {
								if (outputCount != rowIndex) rows[outputCount] = std::move(buffered);
								++outputCount;
							} else {
								buffered = BufferedProcedureRow{};
							}
						}
						rows.resize(outputCount);
						validateBufferedProcedureBytes(rows, batchBounds.maximum_materialized_bytes);
						break;
					}
					case logical::LogicalOpType::LOP_SKIP: {
						const int64_t offset = dynamic_cast<const logical::LogicalSkip &>(*operation).getOffset();
						if (offset < 0) {
							throw std::invalid_argument("logical Vector procedure SKIP cannot be negative");
						}
						uint64_t &skippedRows = operatorRowCounts->at(operationIndex);
						const uint64_t remainingSkip = static_cast<uint64_t>(offset) -
								std::min(static_cast<uint64_t>(offset), skippedRows);
						const size_t skip = static_cast<size_t>(std::min<uint64_t>(remainingSkip, rows.size()));
						skippedRows += static_cast<uint64_t>(skip);
						rows.erase(rows.begin(), rows.begin() + static_cast<std::ptrdiff_t>(skip));
						break;
					}
					case logical::LogicalOpType::LOP_LIMIT: {
						const int64_t limit = dynamic_cast<const logical::LogicalLimit &>(*operation).getLimit();
						if (limit < 0) {
							throw std::invalid_argument("logical Vector procedure LIMIT cannot be negative");
						}
						uint64_t &emittedRows = operatorRowCounts->at(operationIndex);
						const uint64_t limitValue = static_cast<uint64_t>(limit);
						const uint64_t remaining = limitValue - std::min(limitValue, emittedRows);
						const size_t maximum = static_cast<size_t>(std::min<uint64_t>(remaining, rows.size()));
						rows.resize(maximum);
						emittedRows += static_cast<uint64_t>(maximum);
						if (emittedRows >= limitValue) {
							continueInput = false;
						}
						break;
					}
					case logical::LogicalOpType::LOP_SORT: {
						const auto &sort = dynamic_cast<const logical::LogicalSort &>(*operation);
						const std::vector<logical::LogicalSortItem> &items = sort.getSortItems();
						if (items.empty()) {
							throw std::invalid_argument("logical Vector procedure Sort has no sort items");
						}
						utils::OwnedStdVector<const expressions::Expression *> propertyExpressions;
						for (const auto &item: items) {
							propertyExpressions.push_back(item.expression.get());
						}
						prefetchProcedureProperties(rows, propertyExpressions, snapshot, batchBounds, context);
                        for (auto &row : rows) row.reconcilePayloadCredit();
						struct DecoratedRow {
							std::vector<PropertyValue> keys;
							size_t sequence = 0U;
						};
						MaterializedByteBudget sortBudget(batchBounds.maximum_materialized_bytes);
						MaterializedByteReservation retainedRows(&sortBudget);
						for (const auto &buffered: rows) {
							retainedRows.set(checkedProcedureSizeAdd(retainedRows.bytes(), bufferedProcedureRowBytes(buffered)));
						}
						MaterializedByteReservation retainedKeys(&sortBudget);
						// Only indices and keys are copied; row payload stays in its buffer.
						MaterializedByteReservation rowSlack(&sortBudget);
						rowSlack.set(checkedProcedureSizeMul(rows.capacity() - rows.size(), sizeof(BufferedProcedureRow)));
						retainedKeys.set(checkedProcedureSizeMul(rows.size(), checkedProcedureSizeAdd(
							sizeof(DecoratedRow),
							checkedProcedureSizeMul(items.size(), sizeof(PropertyValue)))));
						std::vector<DecoratedRow, utils::OwnedPmrAllocator<DecoratedRow>> decoratedRows;
						decoratedRows.reserve(rows.size());
						retainedKeys.set(checkedProcedureSizeAdd(retainedKeys.bytes(),
							checkedProcedureSizeMul(decoratedRows.capacity() - rows.size(), sizeof(DecoratedRow))));
						for (BufferedProcedureRow &buffered: rows) {
                detail::ProcedurePayloadAdmissionScope bufferedPayloadScope(buffered.allocation_credit);
							DecoratedRow decorated{{}, decoratedRows.size()};
							detail::procedureVectorReserve(decorated.keys, items.size());
							retainedKeys.set(checkedProcedureSizeAdd(retainedKeys.bytes(),
								checkedProcedureSizeMul(decorated.keys.capacity() - items.size(), sizeof(PropertyValue))));
							for (const logical::LogicalSortItem &item: items) {
								if (!item.expression) {
									throw std::invalid_argument(
											"logical Vector procedure Sort has an empty sort expression");
								}
								const size_t previousRowBytes = bufferedProcedureRowBytes(buffered);
								LogicalPlanReadValue value =
										LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(
											 snapshot, *item.expression, buffered.row, batchBounds, context);
								auto *key = std::get_if<PropertyValue>(&value);
								if (key == nullptr) {
									throw std::runtime_error("logical Vector procedure Sort requires scalar sort keys");
								}
								retainedRows.set(checkedProcedureSizeAdd(retainedRows.bytes() - previousRowBytes,
									bufferedProcedureRowBytes(buffered)));
								retainedKeys.set(checkedProcedureSizeAdd(retainedKeys.bytes(), property_utils::getPropertyValueSize(*key)));
								decorated.keys.push_back(std::move(*key));
							}
							decoratedRows.push_back(std::move(decorated));
						}
						std::sort(decoratedRows.begin(), decoratedRows.end(),
										 [&context, &items](const DecoratedRow &left, const DecoratedRow &right) {
											 context.checkGuard();
											 for (size_t index = 0U; index < items.size(); ++index) {
												 if (left.keys[index] == right.keys[index]) {
													 continue;
												 }
												 return detail::orderByPrecedes(left.keys[index], right.keys[index], items[index].ascending);
											 }
											 // Explicit input sequence preserves stable ties while in-place
											 // sorting avoids a second buffered-row array.
											 return left.sequence < right.sequence;
										 });
						// Release keys before rotating permutation cycles. One moved row
						// frame suffices; no second row array or copied payload is needed.
						for (auto &decorated: decoratedRows) std::vector<PropertyValue>().swap(decorated.keys);
						retainedKeys.set(checkedProcedureSizeMul(decoratedRows.capacity(), sizeof(DecoratedRow)));
						MaterializedByteReservation rotationFrame(&sortBudget);
						for (size_t index = 0U; index < rows.size(); ++index) {
							context.checkGuard();
							if (decoratedRows[index].sequence == index) continue;
							rotationFrame.set(sizeof(BufferedProcedureRow));
							BufferedProcedureRow saved = std::move(rows[index]);
							size_t destination = index;
							while (decoratedRows[destination].sequence != index) {
								context.checkGuard();
								const size_t source = decoratedRows[destination].sequence;
								rows[destination] = std::move(rows[source]);
								decoratedRows[destination].sequence = destination;
								destination = source;
							}
							rows[destination] = std::move(saved);
							decoratedRows[destination].sequence = destination;
							rotationFrame.reset();
						}
						validateBufferedProcedureBytes(rows, batchBounds.maximum_materialized_bytes);
						break;
					}
					default: // ZYX_COV_EXCL_LINE: execute() admits only the five unary types dispatched above.
						throw std::logic_error("logical Vector procedure runtime received an unsupported operation");
				}
			}
			materializeProcedureNodeOutputs(rows, result.getColumns(), snapshot, batchBounds, context);
            for (auto &row : rows) row.reconcilePayloadCredit();
			validateBufferedProcedureBytes(rows, batchBounds.maximum_materialized_bytes);

			for (size_t rowIndex = 0U; rowIndex < rows.size(); ++rowIndex) {
				BufferedProcedureRow &buffered = rows[rowIndex];
            detail::ProcedurePayloadAdmissionScope bufferedPayloadScope(buffered.allocation_credit);
				context.checkGuard();
				const ProcedurePayloadReconcileGuard reconcile{buffered};
                detail::ProcedurePayloadAdmissionScope::admit(2U * detail::procedureEmptyMapHeapUpper<QueryResult::Row>());
                QueryResult::Row output;
                detail::procedureMapReserve(output, buffered.row.values.size());
				for (const std::string &column: result.getColumns()) {
					const auto value = buffered.row.values.find(column);
                    detail::procedureAdmitMapNode(output, column);
					if (value == buffered.row.values.end() || // ZYX_COV_EXCL_LINE: output variables are derived from
															  // the row-producing operators.
						!output.emplace(column, materializeResultValue(value->second, snapshot, batchBounds, context,
																	   buffered.node_properties))
								 .second) {
						throw std::logic_error(
								"logical Vector procedure projection did not bind a declared output column");
					}
				}
				if (accountOutputTransition) {
					appendProcedureResultRow(result, rows, rowIndex, std::move(output),
							readBounds.maximum_materialized_bytes, retainedSourceBytes);
				} else {
                    detail::ProcedureResultHandoff::append(result, std::move(output), buffered.allocation_credit);
				}
			}
			return continueInput;
		}

		[[nodiscard]] QueryResult
		executeBufferedProcedure(ProcedureRows rows, storage::LogicalGraphReadSnapshot &snapshot,
				const std::vector<const logical::LogicalOperator *> &operations,
				const logical::LogicalOperator &root, const LogicalPlanReadRuntimeBounds &readBounds,
				const QueryContext &context, const size_t retainedSourceBytes = 0U,
			const std::vector<std::string> *outputColumns = nullptr,
			const bool accountOutputTransition = false) {
			QueryResult result = makeAdmittedProcedureResult(context);
			result.setColumns(outputColumns == nullptr ? root.getOutputVariables() : *outputColumns);
			static_cast<void>(appendBufferedProcedureRows(
					std::move(rows), snapshot, operations, readBounds, context, result, nullptr, nullptr, retainedSourceBytes,
					accountOutputTransition));
			validateProcedureResultBudget(result, retainedSourceBytes, readBounds.maximum_materialized_bytes);
			return result;
		}

		[[nodiscard]] QueryResult
		executeBufferedVectorProcedure(std::vector<storage::LogicalGraphVectorSearchHit> &hits,
									   storage::LogicalGraphVectorSearchSnapshot &snapshot,
									   const std::vector<const logical::LogicalOperator *> &operations,
									   const logical::LogicalOperator &root,
									   const LogicalPlanReadRuntimeBounds &readBounds, const QueryContext &context,
									   const size_t retainedRequestBytes) {
			validateVectorSearchHits(hits, readBounds, context);
			const size_t retainedHitBytes = checkedProcedureSizeMul(
				hits.capacity(), sizeof(storage::LogicalGraphVectorSearchHit));
			const size_t retainedSourceBytes = checkedProcedureSizeAdd(retainedRequestBytes, retainedHitBytes);
			const size_t outputRowSlots = checkedProcedureSizeMul(hits.size(), sizeof(BufferedProcedureRow));
			if (retainedSourceBytes > readBounds.maximum_materialized_bytes ||
				outputRowSlots > readBounds.maximum_materialized_bytes - retainedSourceBytes) {
				throw ProcedureByteBoundExceeded("logical Vector procedure source and output slots exceeded their byte bound");
			}
			return executeBufferedProcedure(makeBufferedVectorProcedureRows(hits, context), snapshot,
													operations, root, readBounds, context, retainedSourceBytes, nullptr, true);
		}

		struct VectorProcedurePostSkipWindowFilter {
			size_t first_filter_index = 0U;
			size_t limit_index = 0U;
			size_t result_limit = 0U;
		};

		[[nodiscard]] std::optional<VectorProcedurePostSkipWindowFilter> vectorProcedurePostSkipFilterLimit(
				const std::vector<const logical::LogicalOperator *> &operations) {
			for (size_t index = 1U; index < operations.size(); ++index) {
				if (operations[index]->getType() != logical::LogicalOpType::LOP_SKIP) continue;
				size_t firstFilterIndex = operations.size();
				size_t limitIndex = index + 1U;
				for (; limitIndex < operations.size(); ++limitIndex) {
					const logical::LogicalOperator *operation = operations[limitIndex];
					if (operation->getType() == logical::LogicalOpType::LOP_FILTER) {
						if (firstFilterIndex == operations.size()) firstFilterIndex = limitIndex;
						continue;
					}
					if (operation->getType() == logical::LogicalOpType::LOP_PROJECT &&
							!dynamic_cast<const logical::LogicalProject &>(*operation).isDistinct()) continue;
					break;
				}
				if (firstFilterIndex == operations.size() || limitIndex == operations.size() ||
						operations[limitIndex]->getType() != logical::LogicalOpType::LOP_LIMIT) {
					continue;
				}
				const int64_t limit = dynamic_cast<const logical::LogicalLimit &>(*operations[limitIndex]).getLimit();
				if (limit < 0) throw std::invalid_argument("logical Vector procedure LIMIT cannot be negative");
				for (size_t suffix = limitIndex + 1U; suffix < operations.size(); ++suffix) {
					if (operations[suffix]->getType() != logical::LogicalOpType::LOP_PROJECT ||
							dynamic_cast<const logical::LogicalProject &>(*operations[suffix]).isDistinct()) {
						return std::nullopt;
					}
				}
				return VectorProcedurePostSkipWindowFilter{firstFilterIndex, limitIndex, static_cast<size_t>(limit)};
			}
			return std::nullopt;
		}

		// DISTINCT top-K may stop after the requested output prefix only when
		// every sort key is determined by a projected expression. Compare the
		// expression tree structurally for the pure, side-effect-free expression
		// forms supported here; unknown expression types keep the full-sort path.
		[[nodiscard]] bool sameDistinctTopKExpression(const expressions::Expression &left,
											 const expressions::Expression &right) {
			if (left.getExpressionType() != right.getExpressionType()) return false;
			switch (left.getExpressionType()) {
			case expressions::ExpressionType::VARIABLE_REFERENCE:
			case expressions::ExpressionType::PROPERTY_ACCESS: {
				const auto *leftReference = dynamic_cast<const expressions::VariableReferenceExpression *>(&left);
				const auto *rightReference = dynamic_cast<const expressions::VariableReferenceExpression *>(&right);
				return leftReference != nullptr && rightReference != nullptr &&
						leftReference->getVariableName() == rightReference->getVariableName() &&
						leftReference->hasProperty() == rightReference->hasProperty() &&
						(!leftReference->hasProperty() ||
						 leftReference->getPropertyName() == rightReference->getPropertyName());
			}
			case expressions::ExpressionType::BINARY_OP: {
				const auto *leftBinary = dynamic_cast<const expressions::BinaryOpExpression *>(&left);
				const auto *rightBinary = dynamic_cast<const expressions::BinaryOpExpression *>(&right);
				return leftBinary != nullptr && rightBinary != nullptr &&
						leftBinary->getOperator() == rightBinary->getOperator() &&
						leftBinary->getLeft() != nullptr && rightBinary->getLeft() != nullptr &&
						leftBinary->getRight() != nullptr && rightBinary->getRight() != nullptr &&
						sameDistinctTopKExpression(*leftBinary->getLeft(), *rightBinary->getLeft()) &&
						sameDistinctTopKExpression(*leftBinary->getRight(), *rightBinary->getRight());
			}
			case expressions::ExpressionType::UNARY_OP: {
				const auto *leftUnary = dynamic_cast<const expressions::UnaryOpExpression *>(&left);
				const auto *rightUnary = dynamic_cast<const expressions::UnaryOpExpression *>(&right);
				return leftUnary != nullptr && rightUnary != nullptr &&
						leftUnary->getOperator() == rightUnary->getOperator() &&
						leftUnary->getOperand() != nullptr && rightUnary->getOperand() != nullptr &&
						sameDistinctTopKExpression(*leftUnary->getOperand(), *rightUnary->getOperand());
			}
			case expressions::ExpressionType::LITERAL: {
				const auto *leftLiteral = dynamic_cast<const expressions::LiteralExpression *>(&left);
				const auto *rightLiteral = dynamic_cast<const expressions::LiteralExpression *>(&right);
				if (leftLiteral == nullptr || rightLiteral == nullptr) return false;
				if (leftLiteral->isNull()) return rightLiteral->isNull();
				if (leftLiteral->isBoolean()) {
					return rightLiteral->isBoolean() && leftLiteral->getBooleanValue() == rightLiteral->getBooleanValue();
				}
				if (leftLiteral->isInteger()) {
					return rightLiteral->isInteger() && leftLiteral->getIntegerValue() == rightLiteral->getIntegerValue();
				}
				if (leftLiteral->isDouble()) {
				if (!rightLiteral->isDouble()) return false;
				const double leftValue = leftLiteral->getDoubleValue();
				const double rightValue = rightLiteral->getDoubleValue();
				return leftValue == rightValue && std::signbit(leftValue) == std::signbit(rightValue);
				}
				if (leftLiteral->isString()) {
					return rightLiteral->isString() && leftLiteral->getStringValue() == rightLiteral->getStringValue();
				}
				return leftLiteral->isPropertyValue() && rightLiteral->isPropertyValue() &&
						leftLiteral->getPropertyValue() == rightLiteral->getPropertyValue();
			}
			default:
				return false;
			}
		}

		[[nodiscard]] const logical::LogicalProject *vectorProcedureDistinctTopKProject(
				const std::vector<const logical::LogicalOperator *> &operations) {
			if (operations.size() < 3U || operations[0]->getType() != logical::LogicalOpType::LOP_SORT ||
				operations[1]->getType() != logical::LogicalOpType::LOP_PROJECT) return nullptr;
			const auto *projection = &dynamic_cast<const logical::LogicalProject &>(*operations[1]);
			if (!projection->isDistinct() || projection->getItems().empty()) return nullptr;
			bool hasSkip = false;
			bool hasLimit = false;
			for (size_t index = 2U; index < operations.size(); ++index) {
				const logical::LogicalOpType type = operations[index]->getType();
				if (type == logical::LogicalOpType::LOP_SKIP && !hasLimit) {
					hasSkip = true;
					continue;
				}
				if (type == logical::LogicalOpType::LOP_FILTER && hasSkip && !hasLimit) {
					continue;
				}
				if (type == logical::LogicalOpType::LOP_LIMIT && !hasLimit) {
					hasLimit = true;
					continue;
				}
				if (type == logical::LogicalOpType::LOP_PROJECT &&
						!dynamic_cast<const logical::LogicalProject &>(*operations[index]).isDistinct()) continue;
				return nullptr;
			}
			if (!hasLimit) return nullptr;
			const auto &sort = dynamic_cast<const logical::LogicalSort &>(*operations[0]);
			if (sort.getSortItems().empty()) return nullptr;
			for (const logical::LogicalSortItem &sortItem: sort.getSortItems()) {
				if (!sortItem.expression) return nullptr;
				const bool represented = std::ranges::any_of(projection->getItems(), [&](const logical::LogicalProjectItem &item) {
					return item.expression != nullptr &&
							sameDistinctTopKExpression(*item.expression, *sortItem.expression);
				});
				if (!represented) return nullptr;
			}
			return projection;
		}

		[[nodiscard]] std::optional<size_t> vectorProcedureSortPrefix(
				const std::vector<const logical::LogicalOperator *> &operations, const size_t totalRows,
				const size_t maximumRows, const size_t maximumDistinctRows) {
			if (operations.empty() || operations.front()->getType() != logical::LogicalOpType::LOP_SORT) {
				return std::nullopt;
			}
			size_t remaining = totalRows;
			size_t requiredPrefix = 0U;
			size_t filtersBeforeWindow = 0U;
			bool firstOperationIsProject = false;
			bool firstOperationIsFilter = false;
			bool secondOperationIsProject = false;
			const logical::LogicalProject *distinctTopKProject = vectorProcedureDistinctTopKProject(operations);
			const std::optional<VectorProcedurePostSkipWindowFilter> postSkipWindowFilter =
					vectorProcedurePostSkipFilterLimit(operations);
			for (size_t index = 1U; index < operations.size(); ++index) {
				const logical::LogicalOperator *operation = operations[index];
				switch (operation->getType()) {
				case logical::LogicalOpType::LOP_FILTER:
					// Row-local Filters can run immediately after Sort, after one
					// Project, on both sides of that Project before the window, or
					// after SKIP when the top-K prefix expands to satisfy the later LIMIT.
					if (!((postSkipWindowFilter.has_value() && index >= postSkipWindowFilter->first_filter_index &&
							index < postSkipWindowFilter->limit_index) ||
						(index == 1U && filtersBeforeWindow == 0U) ||
						(index == 2U && firstOperationIsProject && filtersBeforeWindow == 0U) ||
						(index == 3U && firstOperationIsFilter && secondOperationIsProject &&
							filtersBeforeWindow == 1U))) {
						return std::nullopt;
					}
					++filtersBeforeWindow;
					if (index == 1U) firstOperationIsFilter = true;
					break;
				case logical::LogicalOpType::LOP_PROJECT:
					if (dynamic_cast<const logical::LogicalProject &>(*operation).isDistinct()) {
						if (distinctTopKProject == nullptr || index != 1U) return std::nullopt;
					}
					if (index == 1U) firstOperationIsProject = true;
					if (index == 2U) secondOperationIsProject = true;
					break;
				case logical::LogicalOpType::LOP_SKIP: {
					const int64_t offset = dynamic_cast<const logical::LogicalSkip &>(*operation).getOffset();
					if (offset < 0) {
						throw std::invalid_argument("logical Vector procedure SKIP cannot be negative");
					}
					const size_t skipped = std::min(remaining, static_cast<size_t>(offset));
					requiredPrefix = checkedProcedureSizeAdd(requiredPrefix, skipped);
					remaining -= skipped;
					break;
				}
				case logical::LogicalOpType::LOP_LIMIT: {
					const int64_t limit = dynamic_cast<const logical::LogicalLimit &>(*operation).getLimit();
					if (limit < 0) {
						throw std::invalid_argument("logical Vector procedure LIMIT cannot be negative");
					}
					remaining = std::min(remaining, static_cast<size_t>(limit));
					requiredPrefix = checkedProcedureSizeAdd(requiredPrefix, remaining);
					if (requiredPrefix == 0U || requiredPrefix > maximumRows ||
							(distinctTopKProject != nullptr && requiredPrefix > maximumDistinctRows) ||
							requiredPrefix >= totalRows) {
						return std::nullopt;
					}
					return requiredPrefix;
				}
				default:
					return std::nullopt;
				}
			}
			return std::nullopt;
		}

		[[nodiscard]] QueryResult executeTopKVectorProcedure(
				std::vector<storage::LogicalGraphVectorSearchHit> &hits,
				storage::LogicalGraphVectorSearchSnapshot &snapshot,
				const std::vector<const logical::LogicalOperator *> &operations,
				const logical::LogicalOperator &root, const size_t topK,
					const LogicalPlanReadRuntimeBounds &readBounds, const QueryContext &context,
					const size_t retainedRequestBytes) {
			const logical::LogicalProject *distinctProjection = vectorProcedureDistinctTopKProject(operations);
			utils::OwnedStringVector distinctOutputVariables;
			size_t distinctOutputStorageBytes = 0U;
			if (distinctProjection != nullptr) {
				distinctOutputVariables = distinctProjection->getOutputVariables();
				distinctOutputStorageBytes = checkedProcedureSizeMul(
						distinctOutputVariables.capacity(), sizeof(std::string));
				for (const std::string &variable: distinctOutputVariables) {
					distinctOutputStorageBytes = checkedProcedureSizeAdd(distinctOutputStorageBytes,
						detail::dynamicStringBufferBytes(variable));
				}
			}
			const size_t retainedHitBytes = checkedProcedureSizeMul(
				hits.capacity(), sizeof(storage::LogicalGraphVectorSearchHit));
			const size_t retainedSourceBytes = checkedProcedureSizeAdd(
					checkedProcedureSizeAdd(retainedRequestBytes, retainedHitBytes), distinctOutputStorageBytes);
			if (retainedSourceBytes >= readBounds.maximum_materialized_bytes && !hits.empty()) {
				throw ProcedureByteBoundExceeded("logical Vector procedure source leaves no top-K materialization budget");
			}
		LogicalPlanReadRuntimeBounds workingBounds = readBounds;
		workingBounds.maximum_materialized_bytes -= retainedSourceBytes;
			const auto &sort = dynamic_cast<const logical::LogicalSort &>(*operations.front());
			const auto &items = sort.getSortItems();
			if (items.empty()) {
				throw std::invalid_argument("logical Vector procedure Sort has no sort items");
			}
			utils::OwnedStdVector<const expressions::Expression *> expressions;
			expressions.reserve(items.size());
			for (const logical::LogicalSortItem &item: items) {
				if (!item.expression) {
					throw std::invalid_argument("logical Vector procedure Sort has an empty sort expression");
				}
				expressions.push_back(item.expression.get());
			}
			const logical::LogicalFilter *sourceFilter = nullptr;
			if (operations.size() > 1U && operations[1U]->getType() == logical::LogicalOpType::LOP_FILTER) {
				sourceFilter = &dynamic_cast<const logical::LogicalFilter &>(*operations[1U]);
				expressions.push_back(sourceFilter->getPredicate().get());
			}
			const size_t projectIndex = sourceFilter == nullptr ? 1U : 2U;
			const logical::LogicalProject *earlyProjection = nullptr;
			if (operations.size() > projectIndex + 1U &&
					operations[projectIndex]->getType() == logical::LogicalOpType::LOP_PROJECT &&
					operations[projectIndex + 1U]->getType() == logical::LogicalOpType::LOP_FILTER) {
				earlyProjection = &dynamic_cast<const logical::LogicalProject &>(*operations[projectIndex]);
				for (const logical::LogicalProjectItem &item: earlyProjection->getItems()) {
					expressions.push_back(item.expression.get());
				}
			}
			if (distinctProjection != nullptr) {
				for (const logical::LogicalProjectItem &item: distinctProjection->getItems()) {
					expressions.push_back(item.expression.get());
				}
			}
			const logical::LogicalFilter *earlyFilter = sourceFilter;
			std::vector<LogicalPlanBatchedPropertyRequest> projectedFilterRequests;
			if (earlyProjection != nullptr) {
				earlyFilter = &dynamic_cast<const logical::LogicalFilter &>(*operations[projectIndex + 1U]);
				std::vector<LogicalPlanBatchedPropertyRequest> filterRequests;
				static_cast<void>(LogicalPlanReadRuntime::collectBatchedPropertyRequests(
					{earlyFilter->getPredicate().get()}, filterRequests));
				for (const auto &request: filterRequests) {
					for (const logical::LogicalProjectItem &item: earlyProjection->getItems()) {
						if (item.alias != request.variable) continue;
						const auto *source = dynamic_cast<const expressions::VariableReferenceExpression *>(item.expression.get());
						if (source != nullptr && !source->hasProperty()) {
							projectedFilterRequests.push_back({source->getVariableName(), request.key});
						}
						break;
					}
				}
			}
			struct DecoratedRow {
				BufferedProcedureRow row;
				std::vector<PropertyValue> keys;
				size_t sequence = 0U;
			};
			const auto precedes = [&items, &context](const DecoratedRow &left, const DecoratedRow &right) {
				context.checkGuard();
				for (size_t index = 0U; index < items.size(); ++index) {
					if (left.keys[index] == right.keys[index]) {
						continue;
					}
					return detail::orderByPrecedes(left.keys[index], right.keys[index], items[index].ascending);
				}
				return left.sequence < right.sequence;
			};
			validateVectorSearchHits(hits, readBounds, context);
			// With `precedes` as the heap comparator, the heap front is the worst
			// retained row and can be replaced as better candidates arrive.
			MaterializedByteBudget topKBudget(workingBounds.maximum_materialized_bytes);
			MaterializedByteReservation heapBytes(&topKBudget);
			MaterializedByteReservation heapSlotSlackBytes(&topKBudget);
			ProcedureOrderedDistinctRows retainedDistinctKeys;
			struct RetainedDistinctInfo {
				ProcedureOrderedDistinctRows::iterator key;
				size_t storage_bytes = 0U;
			};
			std::map<size_t, RetainedDistinctInfo, std::less<>, utils::OwnedPmrAllocator<std::pair<const size_t, RetainedDistinctInfo>>> retainedDistinctRows;
			std::vector<DecoratedRow, utils::OwnedPmrAllocator<DecoratedRow>> retained;
			size_t retainedBytes = 0U;
			const auto decoratedBytes = [](const DecoratedRow &row) {
				size_t bytes = checkedProcedureSizeAdd(sizeof(DecoratedRow) - sizeof(BufferedProcedureRow), bufferedProcedureRowBytes(row.row));
				for (const PropertyValue &key: row.keys) {
					bytes = checkedProcedureSizeAdd(bytes,
							checkedProcedureSizeAdd(sizeof(PropertyValue), property_utils::getPropertyValueSize(key)));
				}
				return bytes;
			};
			const size_t capacity = procedureNodeBatchCapacity(workingBounds);
			const size_t keyStructure = checkedProcedureSizeAdd(sizeof(DecoratedRow) - sizeof(BufferedProcedureRow),
				checkedProcedureSizeMul(items.size(), sizeof(PropertyValue)));
			const auto ensureRetainedCapacity = [&](const auto &releaseReadAhead) {
				if (retained.size() < retained.capacity()) return;
				const size_t minimumCapacity = checkedProcedureSizeAdd(retained.size(), 1U);
				const size_t doubledCapacity = retained.capacity() == 0U
						? minimumCapacity
						: checkedProcedureSizeMul(retained.capacity(), 2U);
				const size_t requestedCapacity = std::min(topK, std::max(minimumCapacity, doubledCapacity));
				const size_t requestedBytes = checkedProcedureSizeMul(requestedCapacity, sizeof(DecoratedRow));
				// Existing heap row frames and slack account for the old allocation.
				// Reserve the new buffer as well while both may coexist during growth.
				const size_t growthBytes = checkedProcedureSizeAdd(heapSlotSlackBytes.bytes(), requestedBytes);
				if (heapSlotSlackBytes.wouldExceed(growthBytes)) releaseReadAhead();
				heapSlotSlackBytes.set(growthBytes);
				retained.reserve(requestedCapacity);
				const size_t actualBytes = checkedProcedureSizeMul(retained.capacity(), sizeof(DecoratedRow));
				const size_t occupiedBytes = checkedProcedureSizeMul(retained.size(), sizeof(DecoratedRow));
				heapSlotSlackBytes.set(actualBytes - occupiedBytes);
			};
			const auto retainCandidate = [&](DecoratedRow &&decorated, const size_t candidateBytes,
											 MaterializedByteReservation &candidateCharge, const auto &releaseReadAhead) {
				ensureRetainedCapacity(releaseReadAhead);
				// The reserved slack already covers the destination slot. Keep the
				// candidate reservation through the move, then transfer its charge to
				// the retained row and release the now-occupied slot from slack.
				retained.push_back(std::move(decorated));
				heapBytes.absorb(candidateCharge.splitOff(candidateBytes - sizeof(DecoratedRow)));
				heapBytes.absorb(heapSlotSlackBytes.splitOff(sizeof(DecoratedRow)));
				candidateCharge.reset();
				retainedBytes = checkedProcedureSizeAdd(retainedBytes, candidateBytes);
				std::push_heap(retained.begin(), retained.end(), precedes);
			};
			for (size_t offset = 0U; offset < hits.size();) {
				context.checkGuard();
				if (topKBudget.available() <= keyStructure) throw ProcedureByteBoundExceeded("logical procedure top-K has no candidate headroom");
				LogicalPlanReadRuntimeBounds sourceBounds = workingBounds;
				// Key slots are not live during source prefetch. Admit them after
				// temporary columns unwind, rather than double-reserving phases.
				sourceBounds.maximum_materialized_bytes = topKBudget.available();
				size_t count = std::min({capacity, procedureNodeBatchCapacity(sourceBounds), hits.size() - offset});
				ProcedureRows rows;
				for (;;) {
					try {
						auto prepared = makeBufferedVectorProcedureRows(std::span<const storage::LogicalGraphVectorSearchHit>(hits).subspan(offset, count), context);
						prefetchProcedureProperties(prepared, expressions, snapshot, sourceBounds, context,
							projectedFilterRequests);
						validateBufferedProcedureBytes(prepared, sourceBounds.maximum_materialized_bytes);
						rows = std::move(prepared);
						break;
					} catch (const ProcedureByteBoundExceeded &) {
						// No heap candidate has been processed. Drop the source batch
						// and retry a smaller prefix; malformed/guard errors propagate.
						if (count == 1U) throw;
						count = std::max<size_t>(1U, count / 2U);
					}
				}
				MaterializedByteReservation pendingRows(&topKBudget);
				for (const auto &row: rows) admitProcedureBytes(pendingRows, checkedProcedureSizeAdd(pendingRows.bytes(), bufferedProcedureRowBytes(row)));
				for (size_t rowIndex = 0U; rowIndex < rows.size(); ++rowIndex) {
					context.checkGuard();
					const size_t inputBytes = bufferedProcedureRowBytes(rows[rowIndex]);
					pendingRows.set(pendingRows.bytes() - inputBytes);
					MaterializedByteReservation candidateCharge(&topKBudget);
					const auto releaseReadAhead = [&] {
						rows.resize(rowIndex + 1U);
						pendingRows.reset();
					};
					const auto admitCandidate = [&](size_t bytes) {
						if (candidateCharge.wouldExceed(bytes) && pendingRows.bytes() != 0U) {
							// Release unprocessed read-ahead, never replay heap candidates.
							// Resume its hit suffix after the current candidate is handled.
							rows.resize(rowIndex + 1U);
							pendingRows.reset();
						}
						admitProcedureBytes(candidateCharge, bytes);
					};
					admitCandidate(checkedProcedureSizeAdd(inputBytes, keyStructure));
					DecoratedRow decorated{std::move(rows[rowIndex]), {}, offset + rowIndex};
                    detail::ProcedurePayloadAdmissionScope candidatePayloadScope(decorated.row.allocation_credit);
					utils::OwnedStdString candidateDistinctKey;
					size_t candidateDistinctStorageBytes = 0U;
					detail::procedureVectorReserve(decorated.keys, items.size());
					for (const logical::LogicalSortItem &item: items) {
						LogicalPlanReadValue value = LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(
													snapshot, *item.expression, decorated.row.row, workingBounds, context);
						auto *key = std::get_if<PropertyValue>(&value);
						if (key == nullptr) throw std::runtime_error("logical Vector procedure Sort requires scalar sort keys");
						// All key slots are already reserved; add only owned payload.
						size_t next = checkedProcedureSizeAdd(bufferedProcedureRowBytes(decorated.row), keyStructure);
						for (const auto &existing: decorated.keys) next = checkedProcedureSizeAdd(next, property_utils::getPropertyValueSize(existing));
						admitCandidate(checkedProcedureSizeAdd(next, property_utils::getPropertyValueSize(*key)));
						decorated.keys.push_back(std::move(*key));
					}
					if (distinctProjection != nullptr) {
						std::optional<LogicalPlanReadRow> projected = applyProjection(*distinctProjection,
								snapshot, workingBounds, context, decorated.row.row, decorated.row.node_properties,
								nullptr, candidateCharge.bytes(), true, true);
						if (!projected.has_value()) {
							throw std::logic_error("logical Vector procedure deferred DISTINCT Project dropped a row");
						}
						decorated.row.row = std::move(*projected);
						candidateDistinctKey = detail::makeOwnedProcedureDistinctRowKey(
								decorated.row.row, distinctOutputVariables);
						const size_t setNodeBytes = checkedProcedureSizeAdd(
								checkedProcedureSizeAdd(sizeof(std::string), 4U * sizeof(void *)),
								checkedProcedureSizeAdd(candidateDistinctKey.capacity(), 1U));
						const size_t mapNodeBytes = checkedProcedureSizeAdd(
								sizeof(std::pair<const size_t, RetainedDistinctInfo>), 4U * sizeof(void *));
						candidateDistinctStorageBytes = checkedProcedureSizeAdd(setNodeBytes, mapNodeBytes);
						admitCandidate(checkedProcedureSizeAdd(decoratedBytes(decorated), candidateDistinctStorageBytes));
						if (retainedDistinctKeys.contains(candidateDistinctKey)) continue;
					}
					if (sourceFilter != nullptr && earlyProjection != nullptr) {
						const bool keep = applyFilter(*sourceFilter, snapshot, workingBounds, context,
							decorated.row.row, decorated.row.node_properties);
						admitCandidate(decoratedBytes(decorated));
						if (!keep) continue;
					}
					if (earlyProjection != nullptr) {
						std::optional<LogicalPlanReadRow> projected = applyProjection(*earlyProjection,
								 snapshot, workingBounds, context, decorated.row.row, decorated.row.node_properties, nullptr);
						if (!projected.has_value()) {
							throw std::logic_error("logical Vector procedure non-DISTINCT top-K projection dropped a row");
						}
						decorated.row.row = std::move(*projected);
						admitCandidate(decoratedBytes(decorated));
					}
					if (earlyFilter != nullptr) {
						const bool keep = applyFilter(*earlyFilter, snapshot, workingBounds, context,
							decorated.row.row, decorated.row.node_properties);
						admitCandidate(decoratedBytes(decorated));
						if (!keep) continue;
					}
					const size_t candidateBytes = candidateCharge.bytes();
					if (retained.size() < topK) {
						if (distinctProjection != nullptr &&
								retainedDistinctKeys.size() == workingBounds.maximum_distinct_rows) {
							throw std::runtime_error("logical Vector procedure DISTINCT exceeded its row bound");
						}
						if (candidateBytes > workingBounds.maximum_materialized_bytes -
								std::min(retainedBytes, workingBounds.maximum_materialized_bytes)) {
							throw std::runtime_error("logical Vector procedure top-K rows exceeded their materialized byte bound");
						}
						if (distinctProjection != nullptr) {
							const auto [iterator, inserted] = retainedDistinctKeys.insert(std::move(candidateDistinctKey));
							if (!inserted) throw std::logic_error("logical Vector procedure retained a duplicate DISTINCT row");
							retainedDistinctRows.emplace(decorated.sequence,
									RetainedDistinctInfo{iterator, candidateDistinctStorageBytes});
						}
						retainCandidate(std::move(decorated), candidateBytes, candidateCharge, releaseReadAhead);
					} else if (precedes(decorated, retained.front())) {
						size_t replacedBytes = decoratedBytes(retained.front());
						if (distinctProjection != nullptr) {
							const auto oldDistinct = retainedDistinctRows.find(retained.front().sequence);
							if (oldDistinct == retainedDistinctRows.end()) throw std::logic_error("logical Vector top-K lost a DISTINCT key");
							replacedBytes = checkedProcedureSizeAdd(replacedBytes, oldDistinct->second.storage_bytes);
						}
						const size_t remainingBytes = retainedBytes - replacedBytes;
						if (candidateBytes > workingBounds.maximum_materialized_bytes -
								std::min(remainingBytes, workingBounds.maximum_materialized_bytes)) {
							throw std::runtime_error("logical Vector procedure top-K rows exceeded their materialized byte bound");
						}
						if (distinctProjection != nullptr) {
							const auto [iterator, inserted] = retainedDistinctKeys.insert(std::move(candidateDistinctKey));
							if (!inserted) throw std::logic_error("logical Vector procedure retained a duplicate DISTINCT row");
							retainedDistinctRows.emplace(decorated.sequence,
									RetainedDistinctInfo{iterator, candidateDistinctStorageBytes});
						}
						std::pop_heap(retained.begin(), retained.end(), precedes);
						if (distinctProjection != nullptr) {
							const auto oldDistinct = retainedDistinctRows.find(retained.back().sequence);
							if (oldDistinct == retainedDistinctRows.end()) throw std::logic_error("logical Vector top-K lost an evicted DISTINCT key");
							retainedDistinctKeys.erase(oldDistinct->second.key);
							retainedDistinctRows.erase(oldDistinct);
						}
						retained.pop_back();
						heapBytes.set(remainingBytes);
						heapSlotSlackBytes.set(checkedProcedureSizeMul(
								retained.capacity() - retained.size(), sizeof(DecoratedRow)));
						retainedBytes = remainingBytes;
						retainCandidate(std::move(decorated), candidateBytes, candidateCharge, releaseReadAhead);
					}
				}
				offset += rows.size();
			}
			std::sort_heap(retained.begin(), retained.end(), precedes);
			// Sort keys and DISTINCT membership no longer participate in output.
			// Release them before allocating the destination row buffer.
			retainedDistinctKeys.clear();
			retainedDistinctRows.clear();
			retainedBytes = 0U;
			for (DecoratedRow &decorated: retained) {
				std::vector<PropertyValue>().swap(decorated.keys);
				retainedBytes = checkedProcedureSizeAdd(retainedBytes, decoratedBytes(decorated));
			}
			heapBytes.set(retainedBytes);
			MaterializedByteReservation outputRowSlots(&topKBudget);
			const size_t requestedOutputRowBytes =
					checkedProcedureSizeMul(retained.size(), sizeof(BufferedProcedureRow));
			outputRowSlots.set(requestedOutputRowBytes);
			ProcedureRows rows;
			rows.reserve(retained.size());
			outputRowSlots.set(checkedProcedureSizeMul(rows.capacity(), sizeof(BufferedProcedureRow)));
			const size_t outputRowSlackBytes = checkedProcedureSizeMul(rows.capacity() - retained.size(),
																	 sizeof(BufferedProcedureRow));
			for (DecoratedRow &decorated: retained) {
				rows.push_back(std::move(decorated.row));
			}
			retained.clear();
			retainedDistinctKeys.clear();
			retainedDistinctRows.clear();
			heapBytes.reset();
			std::vector<DecoratedRow, utils::OwnedPmrAllocator<DecoratedRow>>().swap(retained);
			heapSlotSlackBytes.reset();
			outputRowSlots.reset();
			const size_t suffixOffset = earlyProjection != nullptr ? projectIndex + 2U
				: (distinctProjection != nullptr ? 2U : (sourceFilter == nullptr ? 1U : 2U));
			utils::OwnedStdVector<const logical::LogicalOperator *> suffix(operations.begin() +
													 static_cast<std::ptrdiff_t>(suffixOffset), operations.end());
			QueryResult result = makeAdmittedProcedureResult(context);
			setOwnedProcedureColumns(result, root);
			static_cast<void>(appendBufferedProcedureRows(
					std::move(rows), snapshot, suffix, workingBounds, context, result, nullptr, nullptr,
					outputRowSlackBytes, true));
			validateProcedureResultBudget(result, outputRowSlackBytes, workingBounds.maximum_materialized_bytes);
			return result;
		}

