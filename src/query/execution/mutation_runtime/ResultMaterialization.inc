		[[nodiscard]] ResultValue materializeReadBinding(const storage::LogicalGraphReadSnapshot &snapshot,
														 const LogicalPlanReadRow &row, const std::string &variable,
														 const LogicalPlanReadRuntimeBounds &bounds) {
			const auto binding = row.values.find(variable);
			if (binding == row.values.end()) {
				throw std::logic_error("logical mutation RETURN could not resolve a source variable");
			}
			if (const auto *value = std::get_if<PropertyValue>(&binding->second)) {
				return *value;
			}
			if (const auto *node = std::get_if<storage::LogicalGraphNode>(&binding->second)) {
				return LogicalNodeResult{node->node_id, node->labels,
										 readNodeProperties(snapshot, *node, bounds.maximum_property_entries)};
			}
			const auto &edge = std::get<storage::LogicalGraphEdge>(binding->second);
			return LogicalEdgeResult{edge.edge_id, edge.source_node_id, edge.target_node_id, edge.type,
									 readEdgeProperties(snapshot, edge, bounds.maximum_property_entries)};
		}

		[[nodiscard]] ResultValue materializeMutableBinding(const storage::LogicalGraphReadSnapshot &snapshot,
															const LogicalPlanReadRow &row, const std::string &variable,
															const MutableGraphEntities &entities,
															const LogicalPlanReadRuntimeBounds &bounds) {
			const auto binding = row.values.find(variable);
			if (binding == row.values.end() || std::holds_alternative<PropertyValue>(binding->second)) {
				return materializeReadBinding(snapshot, row, variable, bounds);
			}
			if (const auto *node = std::get_if<storage::LogicalGraphNode>(&binding->second)) {
				const auto found =
						entities.find({static_cast<uint8_t>(LogicalGraphMutationEntityKind::LGMK_NODE), node->node_id});
				if (found == entities.end()) {
					return materializeReadBinding(snapshot, row, variable, bounds);
				}
				return LogicalNodeResult{
						node->node_id, found->second.labels,
						found->second.properties.has_value()
								? *found->second.properties
								: readNodeProperties(snapshot, *node, bounds.maximum_property_entries)};
			}
			const auto &edge = std::get<storage::LogicalGraphEdge>(binding->second);
			const auto found =
					entities.find({static_cast<uint8_t>(LogicalGraphMutationEntityKind::LGMK_EDGE), edge.edge_id});
			if (found == entities.end()) {
				return materializeReadBinding(snapshot, row, variable, bounds);
			}
			return LogicalEdgeResult{edge.edge_id, edge.source_node_id, edge.target_node_id, edge.type,
									 found->second.properties.has_value()
											 ? *found->second.properties
											 : readEdgeProperties(snapshot, edge, bounds.maximum_property_entries)};
		}

		[[nodiscard]] QueryResult
		buildCreateSetResult(const logical::LogicalProject *projection, const std::vector<std::string> &implicitColumns,
							 const std::vector<CreateSetRuntimeRow> &rows, storage::LogicalGraphReadSnapshot &snapshot,
							 const LogicalGraphMutationBatch &batch,
							 const storage::LogicalGraphMutationBatchResult &result,
							 const MutableGraphEntities &persistedEntities, const LogicalPlanReadRuntimeBounds &bounds,
							 const QueryContext &context, const logical::LogicalProject *exportedProjection,
							 CreateSetPrepublicationResultCache *prepublicationCache, bool cachePrepublicationValues,
							 const logical::LogicalSort *resultSort, const logical::LogicalSkip *resultSkip,
							 const logical::LogicalLimit *resultLimit) {
			if (result.created_node_ids.size() != batch.node_creates.size() ||
				result.created_edge_ids.size() != batch.edge_creates.size()) {
				throw std::runtime_error("logical CREATE/SET backend returned an inconsistent creation result");
			}
			const auto resolveNodeId = [&result](const LogicalGraphMutationEntityReference &reference) {
				if (reference.entity_kind != LogicalGraphMutationEntityKind::LGMK_NODE) {
					throw std::logic_error("logical CREATE/SET relationship endpoint is not a Node");
				}
				switch (reference.reference_kind) {
					case LogicalGraphMutationReferenceKind::LGRK_PERSISTED:
						return reference.value;
					case LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE:
						return result.created_node_ids.at(static_cast<size_t>(reference.value));
					case LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE:
						throw std::logic_error("logical CREATE/SET relationship endpoint cannot be a relationship");
				}
				throw std::logic_error("logical CREATE/SET relationship endpoint reference is invalid");
			};
			const auto materializeCreated = [&](const LogicalGraphMutationEntityReference &reference) -> ResultValue {
				switch (reference.reference_kind) {
					case LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE: {
						if (reference.entity_kind != LogicalGraphMutationEntityKind::LGMK_NODE ||
							reference.value >= batch.node_creates.size()) {
							throw std::logic_error(
									"logical CREATE/SET has an invalid created Node projection reference");
						}
						const size_t ordinal = static_cast<size_t>(reference.value);
						return LogicalNodeResult{
								result.created_node_ids.at(ordinal), batch.node_creates.at(ordinal).labels,
								propertyMapFromLogicalProperties(batch.node_creates.at(ordinal).properties,
																 "logical CREATE/SET Node projection")};
					}
					case LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE: {
						if (reference.entity_kind != LogicalGraphMutationEntityKind::LGMK_EDGE ||
							reference.value >= batch.edge_creates.size()) {
							throw std::logic_error(
									"logical CREATE/SET has an invalid created relationship projection reference");
						}
						const size_t ordinal = static_cast<size_t>(reference.value);
						const LogicalGraphBatchEdgeCreateRequest &request = batch.edge_creates.at(ordinal);
						return LogicalEdgeResult{
								result.created_edge_ids.at(ordinal), resolveNodeId(request.source),
								resolveNodeId(request.target), request.type,
								propertyMapFromLogicalProperties(request.properties,
																 "logical CREATE/SET relationship projection")};
					}
					case LogicalGraphMutationReferenceKind::LGRK_PERSISTED:
						throw std::logic_error("logical CREATE/SET expected a created projection reference");
				}
				throw std::logic_error("logical CREATE/SET projection reference is invalid");
			};
			std::unordered_map<std::string, const logical::LogicalProjectItem *> exportedItems;
			if (exportedProjection != nullptr) {
				exportedItems.reserve(exportedProjection->getItems().size());
				for (const logical::LogicalProjectItem &item: exportedProjection->getItems()) {
					if (item.alias.empty() || !item.expression || !exportedItems.emplace(item.alias, &item).second) {
						throw std::logic_error("logical write-only CALL has an invalid exported-variable projection");
					}
				}
			}

			std::vector<std::string> outputColumns =
					projection != nullptr ? projection->getOutputVariables() : implicitColumns;
			if (projection == nullptr) {
				std::erase_if(outputColumns, [&rows](const std::string &column) {
					return std::ranges::any_of(rows, [&column](const CreateSetRuntimeRow &row) {
						return row.unavailable_implicit_entities.contains(column);
					});
				});
			}
			std::function<ResultValue(const CreateSetRuntimeRow &, const std::string &)> resolveForRow;
			std::set<std::string> resolvingExportValues;
			resolveForRow = [&](const CreateSetRuntimeRow &runtimeRow, const std::string &variable) -> ResultValue {
				if (const auto entity = runtimeRow.entities.find(variable); entity != runtimeRow.entities.end()) {
					if (entity->second.reference_kind != LogicalGraphMutationReferenceKind::LGRK_PERSISTED) {
						return materializeCreated(entity->second);
					}
					return materializeMutableBinding(snapshot, runtimeRow.source, variable, persistedEntities, bounds);
				}
				const auto scalar = runtimeRow.source.values.find(variable);
				if (scalar != runtimeRow.source.values.end() && std::holds_alternative<PropertyValue>(scalar->second)) {
					return std::get<PropertyValue>(scalar->second);
				}
				if (const auto exported = exportedItems.find(variable); exported != exportedItems.end()) {
					if (!resolvingExportValues.insert(variable).second) {
						throw std::logic_error("logical write-only CALL has a cyclic exported-variable dependency");
					}
					try {
						ResultValue result = evaluateMutationProjectionExpression(
								*exported->second->expression, runtimeRow.source, snapshot, bounds, context,
								[&resolveForRow, &runtimeRow](const LogicalPlanReadRow &,
															 const std::string &nestedVariable) {
									return resolveForRow(runtimeRow, nestedVariable);
								});
						resolvingExportValues.erase(variable);
						return result;
					} catch (...) {
						resolvingExportValues.erase(variable);
						throw;
					}
				}
				throw std::logic_error("logical CREATE/SET RETURN could not resolve a variable");
			};
			std::function<bool(const CreateSetRuntimeRow &, const std::string &, std::set<std::string> &)>
					dependsOnCreatedEntity;
			dependsOnCreatedEntity = [&](const CreateSetRuntimeRow &runtimeRow, const std::string &variable,
										 std::set<std::string> &resolvingExports) {
				const auto entity = runtimeRow.entities.find(variable);
				if (entity != runtimeRow.entities.end()) {
					return entity->second.reference_kind != LogicalGraphMutationReferenceKind::LGRK_PERSISTED;
				}
				const auto exported = exportedItems.find(variable);
				if (exported == exportedItems.end() || !resolvingExports.insert(variable).second) {
					return false;
				}
				std::set<std::string> dependencies;
				expressions::ExpressionUtils::collectVariables(exported->second->expression.get(), dependencies);
				const bool depends = std::ranges::any_of(dependencies, [&](const std::string &dependency) {
					return dependsOnCreatedEntity(runtimeRow, dependency, resolvingExports);
				});
				resolvingExports.erase(variable);
				return depends;
			};
			std::vector<size_t> outputOrder;
			outputOrder.reserve(rows.size());
			for (size_t rowIndex = 0U; rowIndex < rows.size(); ++rowIndex) {
				outputOrder.push_back(rowIndex);
			}
			if (resultSort != nullptr) {
				const std::vector<logical::LogicalSortItem> &sortItems = resultSort->getSortItems();
				if (sortItems.empty()) {
					throw std::logic_error("logical write-only CALL ORDER BY has no sort items");
				}
				if (cachePrepublicationValues && prepublicationCache != nullptr) {
					prepublicationCache->sort_keys.assign(rows.size(),
														  std::vector<std::optional<PropertyValue>>(sortItems.size()));
				}
				for (size_t rowIndex = 0U; rowIndex < rows.size(); ++rowIndex) {
					for (size_t itemIndex = 0U; itemIndex < sortItems.size(); ++itemIndex) {
						const logical::LogicalSortItem &item = sortItems[itemIndex];
						context.checkGuard();
						if (!item.expression) {
							throw std::logic_error("logical write-only CALL ORDER BY has an empty sort expression");
						}
						if (!cachePrepublicationValues && prepublicationCache != nullptr &&
							rowIndex < prepublicationCache->sort_keys.size() &&
							itemIndex < prepublicationCache->sort_keys[rowIndex].size() &&
							prepublicationCache->sort_keys[rowIndex][itemIndex].has_value()) {
							continue;
						}
						std::set<std::string> dependencies;
						expressions::ExpressionUtils::collectVariables(item.expression.get(), dependencies);
						std::set<std::string> resolvingExports;
						const bool sortDependsOnCreatedEntity =
								std::ranges::any_of(dependencies, [&](const std::string &variable) {
									return dependsOnCreatedEntity(rows[rowIndex], variable, resolvingExports);
								});
						const ResultValue key = evaluateMutationProjectionExpression(
								*item.expression, rows[rowIndex].source, snapshot, bounds, context,
								[&resolveForRow, &row = rows[rowIndex]](const LogicalPlanReadRow &,
																		const std::string &variable) {
									return resolveForRow(row, variable);
								});
						if (!key.isPrimitive()) {
							throw std::logic_error(
									"logical write-only CALL ORDER BY expression must evaluate to a Property value");
						}
						if (cachePrepublicationValues && prepublicationCache != nullptr &&
							!sortDependsOnCreatedEntity) {
							prepublicationCache->sort_keys[rowIndex][itemIndex] = key.asPrimitive();
						}
					}
				}
				if (outputOrder.size() > 1U) {
					struct DecoratedRow {
						size_t index = 0U;
						std::vector<PropertyValue> keys;
					};
					std::vector<DecoratedRow> decoratedRows;
					decoratedRows.reserve(outputOrder.size());
					for (const size_t rowIndex: outputOrder) {
						DecoratedRow decorated{rowIndex, {}};
						decorated.keys.reserve(sortItems.size());
						for (size_t itemIndex = 0U; itemIndex < sortItems.size(); ++itemIndex) {
							const logical::LogicalSortItem &item = sortItems[itemIndex];
							std::optional<PropertyValue> cached;
							if (prepublicationCache != nullptr &&
								rowIndex < prepublicationCache->sort_keys.size() &&
								itemIndex < prepublicationCache->sort_keys[rowIndex].size()) {
								cached = prepublicationCache->sort_keys[rowIndex][itemIndex];
							}
							if (cached.has_value()) {
								decorated.keys.push_back(*cached);
								continue;
							}
							const ResultValue key = evaluateMutationProjectionExpression(
									*item.expression, rows[rowIndex].source, snapshot, bounds, context,
									[&resolveForRow, &row = rows[rowIndex]](const LogicalPlanReadRow &,
																			const std::string &variable) {
										return resolveForRow(row, variable);
									});
							if (!key.isPrimitive()) {
								throw std::logic_error("logical write-only CALL ORDER BY expression must evaluate to a "
													   "Property value");
							}
							decorated.keys.push_back(key.asPrimitive());
						}
						decoratedRows.push_back(std::move(decorated));
					}
					std::stable_sort(decoratedRows.begin(), decoratedRows.end(),
									 [&sortItems, &context](const DecoratedRow &left, const DecoratedRow &right) {
										 context.checkGuard();
										 for (size_t itemIndex = 0U; itemIndex < sortItems.size(); ++itemIndex) {
											 if (left.keys[itemIndex] == right.keys[itemIndex]) {
												 continue;
											 }
											 return sortItems[itemIndex].ascending
															? left.keys[itemIndex] < right.keys[itemIndex]
															: left.keys[itemIndex] > right.keys[itemIndex];
										 }
										 return false;
									 });
					outputOrder.clear();
					for (const DecoratedRow &row: decoratedRows) {
						outputOrder.push_back(row.index);
					}
				}
			}
			if (cachePrepublicationValues && projection != nullptr && prepublicationCache != nullptr) {
				prepublicationCache->projection_values.assign(
						rows.size(), std::vector<std::optional<ResultValue>>(projection->getItems().size()));
			}
			auto evaluateProjectionValue = [&](size_t rowIndex, size_t itemIndex) -> ResultValue {
				const logical::LogicalProjectItem &item = projection->getItems()[itemIndex];
				if (item.alias.empty() || !item.expression) {
					throw std::logic_error("logical CREATE/SET RETURN has an invalid output item");
				}
				std::set<std::string> variables;
				expressions::ExpressionUtils::collectVariables(item.expression.get(), variables);
				std::set<std::string> resolvingExports;
				const bool dependsOnCreated = std::ranges::any_of(variables, [&](const std::string &variable) {
					return dependsOnCreatedEntity(rows[rowIndex], variable, resolvingExports);
				});
				if (!cachePrepublicationValues && prepublicationCache != nullptr && !dependsOnCreated &&
						rowIndex < prepublicationCache->projection_values.size() &&
						itemIndex < prepublicationCache->projection_values[rowIndex].size() &&
						prepublicationCache->projection_values[rowIndex][itemIndex].has_value()) {
					return *prepublicationCache->projection_values[rowIndex][itemIndex];
				}
				ResultValue value = evaluateMutationProjectionExpression(
						*item.expression, rows[rowIndex].source, snapshot, bounds, context,
						[&resolveForRow, &row = rows[rowIndex]](const LogicalPlanReadRow &, const std::string &variable) {
							return resolveForRow(row, variable);
						});
				if (cachePrepublicationValues && prepublicationCache != nullptr && !dependsOnCreated) {
					prepublicationCache->projection_values[rowIndex][itemIndex] = value;
				}
				return value;
			};
			std::vector<std::vector<ResultValue>> distinctProjectionValues;
			if (projection != nullptr && projection->isDistinct()) {
				distinctProjectionValues.resize(rows.size());
				std::vector<size_t> distinctOrder;
				distinctOrder.reserve(outputOrder.size());
				const auto sameResultValue = [](const ResultValue &left, const ResultValue &right) {
					if (left.isPrimitive() || right.isPrimitive()) {
						return left.isPrimitive() && right.isPrimitive() && left.asPrimitive() == right.asPrimitive();
					}
					if (left.isNode() || right.isNode()) {
						return left.isNode() && right.isNode() && left.asNode().getId() == right.asNode().getId();
					}
					return left.isEdge() && right.isEdge() && left.asEdge().getId() == right.asEdge().getId();
				};
				for (const size_t rowIndex : outputOrder) {
					context.checkGuard();
					auto &values = distinctProjectionValues[rowIndex];
					values.reserve(projection->getItems().size());
					for (size_t itemIndex = 0U; itemIndex < projection->getItems().size(); ++itemIndex) {
						values.push_back(evaluateProjectionValue(rowIndex, itemIndex));
					}
					const bool duplicate = std::ranges::any_of(distinctOrder, [&](size_t previousIndex) {
						return std::ranges::equal(values, distinctProjectionValues[previousIndex], sameResultValue);
					});
					if (duplicate) {
						continue;
					}
					if (distinctOrder.size() == bounds.maximum_distinct_rows) {
						throw std::runtime_error("logical write-only CALL DISTINCT exceeded its row bound");
					}
					distinctOrder.push_back(rowIndex);
				}
				outputOrder = std::move(distinctOrder);
			}
			size_t first = 0U;
			if (resultSkip != nullptr) {
				if (resultSkip->getOffset() < 0) {
					throw std::logic_error("logical write-only CALL SKIP cannot be negative");
				}
				const uint64_t offset = static_cast<uint64_t>(resultSkip->getOffset());
				first = offset >= outputOrder.size() ? outputOrder.size() : static_cast<size_t>(offset);
			}
			size_t last = outputOrder.size();
			if (resultLimit != nullptr) {
				if (resultLimit->getLimit() < 0) {
					throw std::logic_error("logical write-only CALL LIMIT cannot be negative");
				}
				const uint64_t limit = static_cast<uint64_t>(resultLimit->getLimit());
				const size_t remaining = outputOrder.size() - first;
				if (limit < remaining) {
					last = first + static_cast<size_t>(limit);
				}
			}
			QueryResult queryResult;
			queryResult.setColumns(outputColumns);
			queryResult.setPipelineRowCount(last - first);
			for (size_t orderedIndex = first; orderedIndex < last; ++orderedIndex) {
				const CreateSetRuntimeRow &runtimeRow = rows[outputOrder[orderedIndex]];
				QueryResult::Row output;
				if (projection == nullptr) {
					if (outputColumns.empty()) {
						continue;
					}
					output.reserve(outputColumns.size());
					for (const std::string &column: outputColumns) {
						context.checkGuard();
						if (!output.emplace(column, resolveForRow(runtimeRow, column)).second) {
							throw std::logic_error("logical CREATE/SET produced duplicate output columns");
						}
					}
					queryResult.addRow(std::move(output));
					continue;
				}
				for (size_t itemIndex = 0U; itemIndex < projection->getItems().size(); ++itemIndex) {
					const logical::LogicalProjectItem &item = projection->getItems()[itemIndex];
					context.checkGuard();
					if (item.alias.empty() || !item.expression) {
						throw std::logic_error("logical CREATE/SET RETURN has an invalid output item");
					}
					const size_t rowIndex = outputOrder[orderedIndex];
					ResultValue value;
					if (projection->isDistinct() && rowIndex < distinctProjectionValues.size() &&
							itemIndex < distinctProjectionValues[rowIndex].size()) {
						value = distinctProjectionValues[rowIndex][itemIndex];
					} else {
						value = evaluateProjectionValue(rowIndex, itemIndex);
					}
					if (!output.emplace(item.alias, std::move(value)).second) {
						throw std::logic_error("logical CREATE/SET RETURN has duplicate output aliases");
					}
				}
				queryResult.addRow(std::move(output));
			}
			return queryResult;
		}

		void bindMaterializedProjectionValue(LogicalPlanReadRow &row, const std::string &variable,
											 const ResultValue &value) {
			if (variable.empty()) {
				throw std::logic_error("logical mutation RETURN has an empty source variable");
			}
			if (value.isPrimitive()) {
				row.values.insert_or_assign(variable, value.asPrimitive());
				return;
			}
			if (value.isLogicalNode()) {
				const LogicalNodeResult &node = value.asLogicalNode();
				row.values.insert_or_assign(variable, storage::LogicalGraphNode{node.node_id, node.labels});
				row.property_cache.insert_or_assign(variable, node.properties);
				return;
			}
			if (value.isLogicalEdge()) {
				const LogicalEdgeResult &edge = value.asLogicalEdge();
				row.values.insert_or_assign(variable, storage::LogicalGraphEdge{edge.edge_id, edge.source_node_id,
																				edge.target_node_id, edge.type});
				row.property_cache.insert_or_assign(variable, edge.properties);
				return;
			}
			throw std::logic_error("logical mutation RETURN cannot evaluate a legacy physical entity");
		}

		[[nodiscard]] ResultValue evaluateMutationProjectionExpression(
				const expressions::Expression &expression, const LogicalPlanReadRow &source,
				storage::LogicalGraphReadSnapshot &snapshot, const LogicalPlanReadRuntimeBounds &bounds,
				const QueryContext &context, const MutationProjectionResolver &resolveBinding) {
			if (expression.getExpressionType() == expressions::ExpressionType::VARIABLE_REFERENCE) {
				const auto *reference = dynamic_cast<const expressions::VariableReferenceExpression *>(&expression);
				if (reference == nullptr) {
					throw std::logic_error("logical mutation RETURN has an invalid VariableReference expression");
				}
				if (!reference->hasProperty()) {
					return resolveBinding(source, reference->getVariableName());
				}
			}
			LogicalPlanReadRow evaluationRow = source;
			std::set<std::string> referencedVariables;
			expressions::ExpressionUtils::collectVariables(&expression, referencedVariables);
			for (const std::string &variable: referencedVariables) {
				bindMaterializedProjectionValue(evaluationRow, variable, resolveBinding(source, variable));
			}
			const LogicalPlanReadValue evaluated = LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(
					snapshot, expression, evaluationRow, bounds, context);
			if (const auto *property = std::get_if<PropertyValue>(&evaluated)) {
				return *property;
			}
			throw std::logic_error("logical mutation RETURN expression must evaluate to a Property value");
		}

		[[nodiscard]] QueryResult
		buildMutationProjection(const logical::LogicalProject &projection, std::span<const LogicalPlanReadRow> rows,
								storage::LogicalGraphReadSnapshot &snapshot, const LogicalPlanReadRuntimeBounds &bounds,
								const QueryContext &context, const MutationProjectionResolver &resolveBinding) {
			if (!resolveBinding) {
				throw std::logic_error("logical mutation RETURN requires a binding resolver");
			}
			QueryResult result = context.resource_account ? QueryResult(detail::ResourceLease(context.resource_account)) : QueryResult{};
			result.setColumns(projection.getOutputVariables());
			result.setPipelineRowCount(rows.size());
			for (const LogicalPlanReadRow &source: rows) {
				QueryResult::Row row;
				for (const logical::LogicalProjectItem &item: projection.getItems()) {
					context.checkGuard();
					if (item.alias.empty() || !item.expression) {
						throw std::logic_error("logical mutation RETURN has an invalid output item");
					}
					ResultValue value = evaluateMutationProjectionExpression(*item.expression, source, snapshot, bounds,
																			 context, resolveBinding);
					if (!row.emplace(item.alias, std::move(value)).second) {
						throw std::logic_error("logical mutation RETURN has duplicate output aliases");
					}
				}
				result.addRow(std::move(row));
			}
			return result;
		}

