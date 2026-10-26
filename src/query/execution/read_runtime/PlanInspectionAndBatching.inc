		[[nodiscard]] bool visitOperator(const logical::LogicalOperator &op, ExecutionState &state,
										 const InternalVisitor &visitor,
										 const ChargedRowVisitor *chargedReceiver = nullptr,
										 const NodeColumnReceiver *columnReceiver = nullptr);

		struct BatchableEntityPropertyPredicate {
			std::string variable;
			std::string key;
		};

		struct BatchableEntityMetadataPredicate {
			std::string variable;
			std::string function;
		};

		void appendBatchableEntityPropertyPredicate(std::vector<BatchableEntityPropertyPredicate> &result,
													BatchableEntityPropertyPredicate candidate) {
			const bool duplicate = std::ranges::any_of(result, [&](const auto &existing) {
				return existing.variable == candidate.variable && existing.key == candidate.key;
			});
			if (!duplicate) {
				result.push_back(std::move(candidate));
			}
		}

		void appendBatchableEntityMetadataPredicate(std::vector<BatchableEntityMetadataPredicate> &result,
													BatchableEntityMetadataPredicate candidate) {
			const bool duplicate = std::ranges::any_of(result, [&](const auto &existing) {
				return existing.variable == candidate.variable && existing.function == candidate.function;
			});
			if (!duplicate) {
				result.push_back(std::move(candidate));
			}
		}

		void appendCompleteEntityVariable(std::vector<std::string> &result, std::string_view variable) {
			if (variable.empty() || std::ranges::find(result, variable) != result.end()) {
				return;
			}
			result.emplace_back(variable);
		}

		// Scoped list bindings only shadow outer entities inside their bodies;
		// source lists and REDUCE initializers still evaluate in the outer scope.
		// Track exclusions separately from allowed pattern bindings, since the
		// latter also enforce selected-only shape safety for properties()/keys().
		// Identify expression shapes that require the complete Entity payload.  This
		// is deliberately separate from selected-column discovery: a Sort or Project
		// may contain both a safe scalar Property and `properties(n)`/`n{.*}`.  The
		// caller can then materialize the complete bindings once per bounded batch
		// without disabling the selected-column work for sibling expressions.
		[[nodiscard]] bool
		collectCompleteEntityVariables(const expressions::Expression &expression, std::vector<std::string> &result,
									   const std::unordered_set<std::string> *allowedVariables = nullptr,
									   const std::unordered_set<std::string> *shadowedVariables = nullptr) {
			using ExpressionType = expressions::ExpressionType;
			const auto collectNested = [&](const expressions::Expression *nested) {
				return nested == nullptr ||
					   collectCompleteEntityVariables(*nested, result, allowedVariables, shadowedVariables);
			};
			switch (expression.getExpressionType()) {
				case ExpressionType::LITERAL:
				case ExpressionType::VARIABLE_REFERENCE:
				case ExpressionType::PROPERTY_ACCESS:
				case ExpressionType::PARAMETER:
				case ExpressionType::SHORTEST_PATH:
					return true;
				case ExpressionType::EXPR_LIST_LITERAL: {
					const auto &list = static_cast<const expressions::ListLiteralExpression &>(expression);
					if (list.isStatic()) {
						return true;
					}
					bool complete = true;
					for (const auto &element: list.getElements()) {
						complete = collectNested(element.get()) && complete;
					}
					return complete;
				}
				case ExpressionType::LIST_COMPREHENSION: {
					const auto &comprehension =
							static_cast<const expressions::ListComprehensionExpression &>(expression);
					const bool listComplete = collectNested(comprehension.getListExpr());
					auto scopedVariables =
							shadowedVariables == nullptr ? std::unordered_set<std::string>{} : *shadowedVariables;
					scopedVariables.insert(comprehension.getVariable());
					const auto collectScoped = [&](const expressions::Expression *nested) {
						return nested == nullptr ||
							   collectCompleteEntityVariables(*nested, result, allowedVariables, &scopedVariables);
					};
					const bool whereComplete = collectScoped(comprehension.getWhereExpr());
					const bool mapComplete = collectScoped(comprehension.getMapExpr());
					return listComplete && whereComplete && mapComplete;
				}
				case ExpressionType::EXPR_REDUCE: {
					const auto &reduce = static_cast<const expressions::ReduceExpression &>(expression);
					const bool initialComplete = collectNested(reduce.getInitialExpr());
					const bool listComplete = collectNested(reduce.getListExpr());
					auto scopedVariables =
							shadowedVariables == nullptr ? std::unordered_set<std::string>{} : *shadowedVariables;
					scopedVariables.insert(reduce.getVariable());
					scopedVariables.insert(reduce.getAccumulator());
					const bool bodyComplete = reduce.getBodyExpr() == nullptr ||
											  collectCompleteEntityVariables(*reduce.getBodyExpr(), result,
																			 allowedVariables, &scopedVariables);
					return initialComplete && listComplete && bodyComplete;
				}
				case ExpressionType::EXPR_QUANTIFIER_FUNCTION: {
					const auto &quantifier = static_cast<const expressions::QuantifierFunctionExpression &>(expression);
					const bool listComplete = collectNested(quantifier.getListExpression());
					auto scopedVariables =
							shadowedVariables == nullptr ? std::unordered_set<std::string>{} : *shadowedVariables;
					scopedVariables.insert(quantifier.getVariable());
					const bool whereComplete = quantifier.getWhereExpression() == nullptr ||
											   collectCompleteEntityVariables(*quantifier.getWhereExpression(), result,
																			  allowedVariables, &scopedVariables);
					return listComplete && whereComplete;
				}
				case ExpressionType::BINARY_OP: {
					const auto &binary = static_cast<const expressions::BinaryOpExpression &>(expression);
					if (binary.getLeft() == nullptr || binary.getRight() == nullptr) {
						return false;
					}
					const bool leftComplete = collectNested(binary.getLeft());
					const bool rightComplete = collectNested(binary.getRight());
					return leftComplete && rightComplete;
				}
				case ExpressionType::UNARY_OP: {
					const auto &unary = static_cast<const expressions::UnaryOpExpression &>(expression);
					return unary.getOperand() != nullptr && collectNested(unary.getOperand());
				}
				case ExpressionType::FUNCTION_CALL: {
					const auto &function = static_cast<const expressions::FunctionCallExpression &>(expression);
					const std::string functionName = lowerCase(function.getFunctionName());
					if (functionName == "properties" || functionName == "keys") {
						if (function.getArguments().size() != 1U) {
							return false;
						}
						const auto *reference = dynamic_cast<const expressions::VariableReferenceExpression *>(
								function.getArguments().front().get());
						if (reference == nullptr || reference->hasProperty() || reference->getVariableName().empty()) {
							return false;
						}
						if ((allowedVariables == nullptr || allowedVariables->contains(reference->getVariableName())) &&
							(shadowedVariables == nullptr ||
							 !shadowedVariables->contains(reference->getVariableName()))) {
							appendCompleteEntityVariable(result, reference->getVariableName());
						}
						return true;
					}
					bool complete = true;
					for (const auto &argument: function.getArguments()) {
						complete = collectNested(argument.get()) && complete;
					}
					return complete;
				}
				case ExpressionType::IN_LIST: {
					const auto &in = static_cast<const expressions::InExpression &>(expression);
					const bool valueComplete = collectNested(in.getValue());
					const bool listComplete = !in.hasDynamicList() || collectNested(in.getListExpression());
					return valueComplete && listComplete;
				}
				case ExpressionType::IS_NULL: {
					const auto &isNullExpression = static_cast<const expressions::IsNullExpression &>(expression);
					return collectNested(isNullExpression.getExpression());
				}
				case ExpressionType::MAP_PROJECTION: {
					const auto &projection = static_cast<const expressions::MapProjectionExpression &>(expression);
					if (projection.isMapLiteral()) {
						bool complete = true;
						for (const auto &item: projection.getItems()) {
							if (item.type == expressions::MapProjectionItemType::MPROP_PROPERTY ||
								item.type == expressions::MapProjectionItemType::MPROP_ALL_PROPERTIES) {
								complete = false;
								continue;
							}
							complete = collectNested(item.expression.get()) && complete;
						}
						return complete;
					}
					if (projection.getVariable().empty()) {
						return false;
					}
					const bool variableAllowed =
							(allowedVariables == nullptr || allowedVariables->contains(projection.getVariable())) &&
							(shadowedVariables == nullptr || !shadowedVariables->contains(projection.getVariable()));
					bool complete = true;
					for (const auto &item: projection.getItems()) {
						if (item.type == expressions::MapProjectionItemType::MPROP_ALL_PROPERTIES) {
							if (variableAllowed) {
								appendCompleteEntityVariable(result, projection.getVariable());
							}
							continue;
						}
						if (item.type == expressions::MapProjectionItemType::MPROP_LITERAL) {
							complete = collectNested(item.expression.get()) && complete;
						}
					}
					return complete;
				}
				case ExpressionType::CASE_EXPRESSION: {
					const auto &caseExpression = static_cast<const expressions::CaseExpression &>(expression);
					bool complete = collectNested(caseExpression.getComparisonExpression());
					for (const auto &branch: caseExpression.getBranches()) {
						const bool whenComplete = collectNested(branch.whenExpression.get());
						const bool thenComplete = collectNested(branch.thenExpression.get());
						complete = whenComplete && thenComplete && complete;
					}
					return collectNested(caseExpression.getElseExpression()) && complete;
				}
				case ExpressionType::LIST_SLICE: {
					const auto &slice = static_cast<const expressions::ListSliceExpression &>(expression);
					const bool listComplete = collectNested(slice.getList());
					const bool startComplete = collectNested(slice.getStart());
					const bool endComplete = collectNested(slice.getEnd());
					return listComplete && startComplete && endComplete;
				}
				case ExpressionType::EXPR_PATTERN_COMPREHENSION: {
					const auto &pattern = static_cast<const expressions::PatternComprehensionExpression &>(expression);
					if (pattern.getMapExpression() == nullptr)
						return false;
					auto scopedVariables =
							shadowedVariables == nullptr ? std::unordered_set<std::string>{} : *shadowedVariables;
					if (!pattern.getTargetVar().empty())
						scopedVariables.insert(pattern.getTargetVar());
					if (!pattern.getRelationshipVar().empty())
						scopedVariables.insert(pattern.getRelationshipVar());
					const bool whereComplete = pattern.getWhereExpression() == nullptr ||
											   collectCompleteEntityVariables(*pattern.getWhereExpression(), result,
																			  allowedVariables, &scopedVariables);
					const bool mapComplete = collectCompleteEntityVariables(*pattern.getMapExpression(), result,
																			allowedVariables, &scopedVariables);
					return whereComplete && mapComplete;
				}
				case ExpressionType::EXPR_EXISTS: {
					const auto &exists = static_cast<const expressions::ExistsExpression &>(expression);
					auto scopedVariables =
							shadowedVariables == nullptr ? std::unordered_set<std::string>{} : *shadowedVariables;
					if (!exists.getTargetVar().empty())
						scopedVariables.insert(exists.getTargetVar());
					return exists.getWhereExpression() == nullptr ||
						   collectCompleteEntityVariables(*exists.getWhereExpression(), result, allowedVariables,
														  &scopedVariables);
				}
			}
			return false;
		}

		// Keep complete-Entity prefetch for safe expression fragments when another
		// fragment owns a nested graph scope that this outer-row collector cannot
		// analyze. The recursive collector never enters those scopes, so partial
		// discoveries from supported siblings remain safe to batch.
		[[nodiscard]] std::vector<std::string>
		selectBatchableCompleteEntityVariables(const std::vector<const expressions::Expression *> &candidates,
											   const std::unordered_set<std::string> *allowedVariables = nullptr) {
			std::vector<std::string> result;
			for (const expressions::Expression *candidate: candidates) {
				if (candidate == nullptr) {
					return {};
				}
				std::vector<std::string> expressionVariables;
				static_cast<void>(collectCompleteEntityVariables(*candidate, expressionVariables, allowedVariables));
				for (const std::string &variable: expressionVariables) {
					appendCompleteEntityVariable(result, variable);
				}
			}
			return result;
		}

		[[nodiscard]] bool collectBatchableEntityPropertyPredicates(
				const expressions::Expression &expression, std::vector<BatchableEntityPropertyPredicate> &result,
				const std::unordered_set<std::string> *allowedVariables = nullptr,
				std::vector<BatchableEntityMetadataPredicate> *metadataResult = nullptr,
				const std::unordered_set<std::string> *shadowedVariables = nullptr) {
			using ExpressionType = expressions::ExpressionType;
			const auto collectNested = [&](const expressions::Expression *nested) {
				return nested == nullptr || collectBatchableEntityPropertyPredicates(*nested, result, allowedVariables,
																					 metadataResult, shadowedVariables);
			};
			if (expression.getExpressionType() == ExpressionType::PROPERTY_ACCESS) {
				const auto &reference = static_cast<const expressions::VariableReferenceExpression &>(expression);
				if (!reference.hasProperty() || reference.getVariableName().empty() ||
					reference.getPropertyName().empty()) {
					return false;
				}
				if ((allowedVariables != nullptr && !allowedVariables->contains(reference.getVariableName())) ||
					(shadowedVariables != nullptr && shadowedVariables->contains(reference.getVariableName()))) {
					// Pattern-local target/relationship bindings are populated only while
					// visiting a candidate. They remain on the semantic evaluator path;
					// outer-row properties can still be prefetched safely.
					return true;
				}
				appendBatchableEntityPropertyPredicate(result,
													   {reference.getVariableName(), reference.getPropertyName()});
				return true;
			}
			if (expression.getExpressionType() == ExpressionType::VARIABLE_REFERENCE) {
				// A scalar binding does not require a storage read. Treat it as a
				// batchable leaf so comprehensions can still prefetch outer entity
				// properties used alongside the scoped scalar.
				const auto &reference = static_cast<const expressions::VariableReferenceExpression &>(expression);
				return !reference.hasProperty();
			}
			if (expression.getExpressionType() == ExpressionType::SHORTEST_PATH)
				return true;
			if (expression.getExpressionType() == ExpressionType::LITERAL ||
				expression.getExpressionType() == ExpressionType::PARAMETER) {
				return true;
			}
			if (expression.getExpressionType() == ExpressionType::EXPR_LIST_LITERAL) {
				const auto &list = static_cast<const expressions::ListLiteralExpression &>(expression);
				if (list.isStatic()) {
					return true;
				}
				for (const auto &element: list.getElements()) {
					if (!collectNested(element.get())) {
						return false;
					}
				}
				return true;
			}
			if (expression.getExpressionType() == ExpressionType::LIST_COMPREHENSION) {
				const auto &comprehension = static_cast<const expressions::ListComprehensionExpression &>(expression);
				auto scopedVariables =
						shadowedVariables == nullptr ? std::unordered_set<std::string>{} : *shadowedVariables;
				scopedVariables.insert(comprehension.getVariable());
				const auto collectScoped = [&](const expressions::Expression *nested) {
					return nested == nullptr ||
						   collectBatchableEntityPropertyPredicates(*nested, result, allowedVariables, metadataResult,
																	&scopedVariables);
				};
				return collectNested(comprehension.getListExpr()) && collectScoped(comprehension.getWhereExpr()) &&
					   collectScoped(comprehension.getMapExpr());
			}
			if (expression.getExpressionType() == ExpressionType::EXPR_REDUCE) {
				const auto &reduce = static_cast<const expressions::ReduceExpression &>(expression);
				auto scopedVariables =
						shadowedVariables == nullptr ? std::unordered_set<std::string>{} : *shadowedVariables;
				scopedVariables.insert(reduce.getVariable());
				scopedVariables.insert(reduce.getAccumulator());
				return collectNested(reduce.getInitialExpr()) && collectNested(reduce.getListExpr()) &&
					   (reduce.getBodyExpr() == nullptr ||
						collectBatchableEntityPropertyPredicates(*reduce.getBodyExpr(), result, allowedVariables,
																 metadataResult, &scopedVariables));
			}
			if (expression.getExpressionType() == ExpressionType::EXPR_QUANTIFIER_FUNCTION) {
				const auto &quantifier = static_cast<const expressions::QuantifierFunctionExpression &>(expression);
				auto scopedVariables =
						shadowedVariables == nullptr ? std::unordered_set<std::string>{} : *shadowedVariables;
				scopedVariables.insert(quantifier.getVariable());
				return collectNested(quantifier.getListExpression()) &&
					   (quantifier.getWhereExpression() == nullptr ||
						collectBatchableEntityPropertyPredicates(*quantifier.getWhereExpression(), result,
																 allowedVariables, metadataResult, &scopedVariables));
			}
			if (expression.getExpressionType() == ExpressionType::EXPR_PATTERN_COMPREHENSION) {
				const auto &pattern = static_cast<const expressions::PatternComprehensionExpression &>(expression);
				if (pattern.getMapExpression() == nullptr)
					return false;
				auto scopedVariables =
						shadowedVariables == nullptr ? std::unordered_set<std::string>{} : *shadowedVariables;
				if (!pattern.getTargetVar().empty())
					scopedVariables.insert(pattern.getTargetVar());
				if (!pattern.getRelationshipVar().empty())
					scopedVariables.insert(pattern.getRelationshipVar());
				const auto collectScoped = [&](const expressions::Expression *nested) {
					return nested == nullptr ||
						   collectBatchableEntityPropertyPredicates(*nested, result, allowedVariables, metadataResult,
																	&scopedVariables);
				};
				return collectScoped(pattern.getWhereExpression()) && collectScoped(pattern.getMapExpression());
			}
			if (expression.getExpressionType() == ExpressionType::EXPR_EXISTS) {
				const auto &exists = static_cast<const expressions::ExistsExpression &>(expression);
				auto scopedVariables =
						shadowedVariables == nullptr ? std::unordered_set<std::string>{} : *shadowedVariables;
				if (!exists.getTargetVar().empty())
					scopedVariables.insert(exists.getTargetVar());
				return exists.getWhereExpression() == nullptr ||
					   collectBatchableEntityPropertyPredicates(*exists.getWhereExpression(), result, allowedVariables,
																metadataResult, &scopedVariables);
			}
			if (expression.getExpressionType() == ExpressionType::BINARY_OP) {
				const auto &binary = static_cast<const expressions::BinaryOpExpression &>(expression);
				return binary.getLeft() != nullptr && binary.getRight() != nullptr && collectNested(binary.getLeft()) &&
					   collectNested(binary.getRight());
			}
			if (expression.getExpressionType() == ExpressionType::UNARY_OP) {
				const auto &unary = static_cast<const expressions::UnaryOpExpression &>(expression);
				return unary.getOperand() != nullptr && collectNested(unary.getOperand());
			}
			if (expression.getExpressionType() == ExpressionType::FUNCTION_CALL) {
				const auto &function = static_cast<const expressions::FunctionCallExpression &>(expression);
				const std::string functionName = lowerCase(function.getFunctionName());
				if (metadataResult != nullptr && (functionName == "labels" || functionName == "type") &&
					function.getArguments().size() == 1U) {
					const auto *reference = dynamic_cast<const expressions::VariableReferenceExpression *>(
							function.getArguments().front().get());
					if (reference != nullptr && !reference->hasProperty() && !reference->getVariableName().empty() &&
						(allowedVariables == nullptr || allowedVariables->contains(reference->getVariableName())) &&
						(shadowedVariables == nullptr || !shadowedVariables->contains(reference->getVariableName()))) {
						appendBatchableEntityMetadataPredicate(*metadataResult,
															   {reference->getVariableName(), functionName});
					}
				}
				// `properties(entity)` and `keys(entity)` observe the complete map;
				// a selected-column pattern shape must not silently turn them into a
				// partial map.  Other scalar/entity metadata functions are safe because
				// pattern reads always retain labels and relationship type metadata.
				if (allowedVariables != nullptr && (functionName == "properties" || functionName == "keys") &&
					function.getArguments().size() == 1U) {
					const auto *reference = dynamic_cast<const expressions::VariableReferenceExpression *>(
							function.getArguments().front().get());
					if (reference != nullptr && !reference->hasProperty() &&
						allowedVariables->contains(reference->getVariableName()) &&
						(shadowedVariables == nullptr || !shadowedVariables->contains(reference->getVariableName()))) {
						return false;
					}
				}
				for (const auto &argument: function.getArguments()) {
					if (!collectNested(argument.get())) {
						return false;
					}
				}
				return true;
			}
			if (expression.getExpressionType() == ExpressionType::IN_LIST) {
				const auto &in = static_cast<const expressions::InExpression &>(expression);
				if (!collectNested(in.getValue())) {
					return false;
				}
				return !in.hasDynamicList() ||
					   (in.getListExpression() != nullptr && collectNested(in.getListExpression()));
			}
			if (expression.getExpressionType() == ExpressionType::IS_NULL) {
				const auto &isNullExpression = static_cast<const expressions::IsNullExpression &>(expression);
				return collectNested(isNullExpression.getExpression());
			}
			if (expression.getExpressionType() == ExpressionType::MAP_PROJECTION) {
				const auto &projection = static_cast<const expressions::MapProjectionExpression &>(expression);
				if (projection.isMapLiteral()) {
					for (const auto &item: projection.getItems()) {
						if (item.type == expressions::MapProjectionItemType::MPROP_PROPERTY ||
							item.type == expressions::MapProjectionItemType::MPROP_ALL_PROPERTIES) {
							// A map literal cannot select entity properties. Returning false
							// preserves the normal evaluator's validation and NULL semantics.
							return false;
						}
						if (!collectNested(item.expression.get())) {
							return false;
						}
					}
					return true;
				}
				if (projection.getVariable().empty()) {
					return false;
				}
				const bool variableIsBatchable =
						(allowedVariables == nullptr || allowedVariables->contains(projection.getVariable())) &&
						(shadowedVariables == nullptr || !shadowedVariables->contains(projection.getVariable()));
				for (const auto &item: projection.getItems()) {
					switch (item.type) {
						case expressions::MapProjectionItemType::MPROP_PROPERTY:
							if (item.key.empty()) {
								return false;
							}
							if (variableIsBatchable) {
								appendBatchableEntityPropertyPredicate(result, {projection.getVariable(), item.key});
							}
							break;
						case expressions::MapProjectionItemType::MPROP_LITERAL:
							if (!collectNested(item.expression.get())) {
								return false;
							}
							break;
						case expressions::MapProjectionItemType::MPROP_ALL_PROPERTIES:
							// `.*` requires the complete Property map and cannot use a
							// selected-column batch without changing its contract.
							if (variableIsBatchable) {
								return false;
							}
							break;
					}
				}
				return true;
			}
			if (expression.getExpressionType() == ExpressionType::CASE_EXPRESSION) {
				const auto &caseExpression = static_cast<const expressions::CaseExpression &>(expression);
				if (!collectNested(caseExpression.getComparisonExpression())) {
					return false;
				}
				for (const auto &branch: caseExpression.getBranches()) {
					if (!collectNested(branch.whenExpression.get()) || !collectNested(branch.thenExpression.get())) {
						return false;
					}
				}
				return collectNested(caseExpression.getElseExpression());
			}
			if (expression.getExpressionType() == ExpressionType::LIST_SLICE) {
				const auto &slice = static_cast<const expressions::ListSliceExpression &>(expression);
				return collectNested(slice.getList()) && collectNested(slice.getStart()) &&
					   collectNested(slice.getEnd());
			}
			return false;
		}

		// A logical operator may combine storage-backed expressions with shapes that
		// intentionally require complete entities or scoped path evaluation.  Keep
		// the safe subset batched instead of disabling batching for the whole operator.
		[[nodiscard]] std::vector<const expressions::Expression *>
		selectBatchableExpressions(const std::vector<const expressions::Expression *> &expressions) {
			std::vector<const expressions::Expression *> selected;
			selected.reserve(expressions.size());
			for (const expressions::Expression *expression: expressions) {
				if (expression == nullptr) {
					continue;
				}
				std::vector<BatchableEntityPropertyPredicate> properties;
				std::vector<BatchableEntityMetadataPredicate> metadata;
				if (collectBatchableEntityPropertyPredicates(*expression, properties, nullptr, &metadata) &&
					(!properties.empty() || !metadata.empty())) {
					selected.push_back(expression);
				}
			}
			return selected;
		}

		// A read-only correlated subquery executes once for each outer row, but a
		// simple unary body can still share its imported Entity Property reads.  Walk
		// only the expression-bearing unary operators here; anything more complex
		// keeps the normal per-row evaluator so nested scope and cardinality semantics
		// remain authoritative.
		[[nodiscard]] std::vector<const expressions::Expression *>
		collectCorrelatedSubqueryExpressions(const logical::LogicalOperator &body) {
			std::vector<const expressions::Expression *> expressions;
			const logical::LogicalOperator *current = &body;
			while (current != nullptr) {
				switch (current->getType()) {
					case logical::LogicalOpType::LOP_PROJECT: {
						const auto &project = dynamic_cast<const logical::LogicalProject &>(*current);
						for (const logical::LogicalProjectItem &item: project.getItems()) {
							expressions.push_back(item.expression.get());
						}
						break;
					}
					case logical::LogicalOpType::LOP_FILTER: {
						const auto &filter = dynamic_cast<const logical::LogicalFilter &>(*current);
						expressions.push_back(filter.getPredicate().get());
						break;
					}
					case logical::LogicalOpType::LOP_SORT: {
						const auto &sort = dynamic_cast<const logical::LogicalSort &>(*current);
						for (const logical::LogicalSortItem &item: sort.getSortItems()) {
							expressions.push_back(item.expression.get());
						}
						break;
					}
					case logical::LogicalOpType::LOP_AGGREGATE: {
						const auto &aggregate = dynamic_cast<const logical::LogicalAggregate &>(*current);
						for (const auto &expression: aggregate.getGroupByExprs()) {
							expressions.push_back(expression.get());
						}
						for (const logical::LogicalAggItem &item: aggregate.getAggregations()) {
							expressions.push_back(item.argument.get());
							expressions.push_back(item.extraArg.get());
						}
						break;
					}
					case logical::LogicalOpType::LOP_SKIP:
					case logical::LogicalOpType::LOP_LIMIT:
						break;
					case logical::LogicalOpType::LOP_UNWIND: {
						const auto &unwind = dynamic_cast<const logical::LogicalUnwind &>(*current);
						if (!unwind.hasLiteralList() && unwind.getListExpr() != nullptr) {
							expressions.push_back(unwind.getListExpr().get());
						}
						break;
					}
					case logical::LogicalOpType::LOP_SINGLE_ROW:
						return expressions;
					default:
						return {};
				}
				const std::vector<logical::LogicalOperator *> children = current->getChildren();
				if (children.size() != 1U || children.front() == nullptr) {
					return {};
				}
				current = children.front();
			}
			return {};
		}

		// Collect direct property accesses from side-effect-free expression shapes;
		// the original evaluator remains authoritative after the batch is filled.
		[[nodiscard]] std::optional<PatternBindingReadShape>
		inspectPatternBindingReadShape(const expressions::Expression *whereExpression,
									   const expressions::Expression *mapExpression, std::string_view targetVariable,
									   std::string_view relationshipVariable,
									   storage::LogicalGraphReadSnapshot &snapshot) {
			std::unordered_set<std::string> allowedVariables;
			if (!targetVariable.empty()) {
				allowedVariables.emplace(targetVariable);
			}
			if (!relationshipVariable.empty()) {
				allowedVariables.emplace(relationshipVariable);
			}
			std::vector<std::string> completeVariables;
			for (const auto *expression: {whereExpression, mapExpression}) {
				if (expression != nullptr &&
					!collectCompleteEntityVariables(*expression, completeVariables, &allowedVariables)) {
					return std::nullopt;
				}
			}
			auto selectedVariables = allowedVariables;
			for (const auto &variable: completeVariables)
				selectedVariables.erase(variable);
			std::vector<BatchableEntityPropertyPredicate> requests;
			for (const auto *expression: {whereExpression, mapExpression}) {
				if (expression != nullptr &&
					!collectBatchableEntityPropertyPredicates(*expression, requests, &selectedVariables)) {
					return std::nullopt;
				}
			}
			PatternBindingReadShape shape;
			shape.target.include_labels = !targetVariable.empty();
			shape.relationship.include_type = !relationshipVariable.empty();
			shape.target.all_properties =
					std::ranges::find(completeVariables, targetVariable) != completeVariables.end();
			shape.relationship.all_properties =
					std::ranges::find(completeVariables, relationshipVariable) != completeVariables.end();
			for (const BatchableEntityPropertyPredicate &request: requests) {
				const std::optional<uint32_t> token = snapshot.findPropertyToken(request.key);
				storage::EntityReadShape *destination = nullptr;
				std::vector<std::string> *namedProperties = nullptr;
				if (request.variable == targetVariable) {
					destination = &shape.target;
					namedProperties = &shape.target_named_properties;
				} else if (request.variable == relationshipVariable) {
					destination = &shape.relationship;
					namedProperties = &shape.relationship_named_properties;
				} else {
					throw std::logic_error("logical-plan pattern shape resolved an unknown scoped Entity variable");
				}
				if (token)
					destination->property_tokens.push_back(*token);
				else
					namedProperties->push_back(request.key);
			}
			const auto normalize = [](storage::EntityReadShape &entityShape) {
				std::ranges::sort(entityShape.property_tokens);
				entityShape.property_tokens.erase(
						std::unique(entityShape.property_tokens.begin(), entityShape.property_tokens.end()),
						entityShape.property_tokens.end());
				validateEntityReadShape(entityShape);
			};
			normalize(shape.target);
			normalize(shape.relationship);
			for (auto *keys: {&shape.target_named_properties, &shape.relationship_named_properties}) {
				std::ranges::sort(*keys);
				keys->erase(std::unique(keys->begin(), keys->end()), keys->end());
			}
			return shape;
		}

		// Prefetch selected Properties for any storage-neutral row producer.  Keeping
		// this boundary independent of a particular logical operator lets filters,
		// projections, and aggregates share the same bounded page-grouped read path.
		[[nodiscard]] std::optional<bool>
		tryVisitBatchedPropertyExpressions(const logical::LogicalOperator &child,
										   const std::vector<const expressions::Expression *> &expressions,
										   ExecutionState &state, const InternalVisitor &visitor,
										   const BatchSelectionPredicate &selectionPredicate = {},
										   const std::vector<std::string> &completeEntityVariables = {},
										   const ChargedRowVisitor *chargedReceiver = nullptr,
										   bool borrowRepeatedNodeColumns = false) {
			if (!state.shared.allow_deferred_entity_refs ||
				(!state.shared.snapshot.supportsNativeNodeRefScans() && !state.shared.snapshot.supportsNativeEdgeRefScans())) {
				return std::nullopt;
			}
			std::vector<BatchableEntityPropertyPredicate> requests;
			std::vector<BatchableEntityMetadataPredicate> metadataRequests;
			for (const expressions::Expression *expression: expressions) {
				if (expression == nullptr) {
					return std::nullopt;
				}
				std::vector<BatchableEntityPropertyPredicate> nestedRequests;
				if (!collectBatchableEntityPropertyPredicates(*expression, nestedRequests, nullptr,
															  &metadataRequests)) {
					return std::nullopt;
				}
				for (const auto &candidate: nestedRequests) {
					if (!std::ranges::any_of(requests, [&](const auto &existing) {
							return existing.variable == candidate.variable && existing.key == candidate.key;
						})) {
						requests.push_back(candidate);
					}
				}
			}
			if (requests.empty() && metadataRequests.empty() && completeEntityVariables.empty()) {
				return std::nullopt;
			}
			// A property expression can outlive the entity binding that produced it
			// (for example, an outer projection over `WITH DISTINCT n.value`).  In
			// that shape the child row intentionally contains only the projected
			// scalar, so this batch helper must defer to the normal evaluator rather
			// than treating the missing binding as a corrupt storage result.
			const std::vector<std::string> childOutputs = child.getOutputVariables();
			for (const BatchableEntityPropertyPredicate &request: requests) {
				if (std::ranges::find(childOutputs, request.variable) == childOutputs.end()) {
					return std::nullopt;
				}
			}
			for (const BatchableEntityMetadataPredicate &request: metadataRequests) {
				if (std::ranges::find(childOutputs, request.variable) == childOutputs.end()) {
					return std::nullopt;
				}
			}
			for (const std::string &variable: completeEntityVariables) {
				if (std::ranges::find(childOutputs, variable) == childOutputs.end()) {
					return std::nullopt;
				}
			}

			struct PropertyGroup {
				std::string variable;
				std::vector<std::string> keys;
			};
			std::vector<PropertyGroup> groups;
			for (const auto &request: requests) {
				auto group = std::ranges::find(groups, request.variable, &PropertyGroup::variable);
				if (group == groups.end()) {
					groups.push_back({request.variable, {request.key}});
				} else if (std::ranges::find(group->keys, request.key) == group->keys.end()) {
					group->keys.push_back(request.key);
				}
			}
			struct MetadataGroup {
				std::string variable;
				bool labels = false;
				bool type = false;
			};
			std::vector<MetadataGroup> metadataGroups;
			for (const BatchableEntityMetadataPredicate &request: metadataRequests) {
				auto group = std::ranges::find(metadataGroups, request.variable, &MetadataGroup::variable);
				if (group == metadataGroups.end()) {
					metadataGroups.push_back(
							{request.variable, request.function == "labels", request.function == "type"});
				} else if (request.function == "labels") {
					group->labels = true;
				} else if (request.function == "type") {
					group->type = true;
				}
			}

			// Reserve only a fraction of the working allowance for pending row
            // carriers. Split retries must retain their unprocessed siblings;
            // filling a tiny budget with carriers can starve even one payload.
            const size_t carrierCapacity = std::max<size_t>(1U,
                state.shared.materialized_budget.available() / 8U / sizeof(LogicalPlanReadRow));
            const size_t batchCapacity = std::min({DEFAULT_PROPERTY_BATCH_ROWS,
                state.shared.bounds.maximum_rows, carrierCapacity});
			if (batchCapacity == 0U) {
				return std::nullopt;
			}
			const bool coordinatedStorage = state.shared.context && state.shared.context->resource_account;
            MaterializedByteReservation legacyBufferedRowsBytes(&state.shared.materialized_budget);
            detail::BudgetMemoryResource selectionMemory(state.shared.materialized_budget);
            BoundedBatch<LogicalPlanReadRow> rows(
                batchCapacity, state.shared.bounds.maximum_materialized_bytes,
                [&state](const LogicalPlanReadRow &row) { return logicalReadRowBytes(row, state.shared.context); },
                coordinatedStorage ? &state.shared.materialized_budget : nullptr, true, sizeof(LogicalPlanReadRow));
            auto &bufferedRowsBytes = coordinatedStorage ? rows.enrichmentReservation() : legacyBufferedRowsBytes;
            rows.reserve(batchCapacity);
			bool stoppedByVisitor = false;
			size_t largestInputRowBytes = 0U;
			std::function<bool()> flush;
			struct PrefetchBytePressure {};
			const auto putCacheValue = [&](auto &cache, const std::string &variable, const std::string &key,
										   auto &&value) {
				const size_t previousBytes = bufferedRowsBytes.bytes();
				size_t nextBytes = previousBytes;
				const auto alias = cache.find(variable);
				if (alias == cache.end()) {
					nextBytes =
							checkedSizeAdd(nextBytes, variable.size(), "logical-plan prefetch cache size overflowed");
				}
				if (alias != cache.end() && alias->second.contains(key)) {
					nextBytes -= retainedExpressionValueBytes(alias->second.at(key), state);
				} else {
					nextBytes = checkedSizeAdd(nextBytes, key.size(), "logical-plan prefetch cache size overflowed");
				}
				const size_t requestedPayloadBytes = retainedExpressionValueBytes(value, state);
				nextBytes = checkedSizeAdd(nextBytes, requestedPayloadBytes,
										   "logical-plan prefetch cache size overflowed");
				const size_t withSelection =
						checkedSizeAdd(nextBytes, SelectionVector::requiredStorageBytes(rows.size()),
									   "logical-plan prefetch selection size overflowed");
				if (nextBytes > state.shared.bounds.maximum_materialized_bytes ||
					bufferedRowsBytes.wouldExceed(withSelection)) {
					throw PrefetchBytePressure{};
				}
				bufferedRowsBytes.set(nextBytes);
				try {
					if constexpr (std::is_lvalue_reference_v<decltype(value)>) {
						PropertyValue copy = detail::copyPropertyValueGuarded(value, [&] { checkGuardNow(state); });
						cache[variable].insert_or_assign(key, std::move(copy));
					} else {
						cache[variable].insert_or_assign(key, std::forward<decltype(value)>(value));
					}
				} catch (...) {
					bufferedRowsBytes.set(previousBytes);
					throw;
				}
				// Copying a container can change its capacity. Reconcile the owned
				// value before a pressure-driven split transfers charges to rows.
				const size_t retainedBytes = retainedExpressionValueBytes(cache.at(variable).at(key), state);
				const size_t actualBytes = checkedSizeAdd(nextBytes - requestedPayloadBytes, retainedBytes,
					"logical-plan copied cache size overflowed");
				// If a copy unexpectedly grows beyond admission, fail closed rather
				// than entering a split with an undercharged, already-mutated row.
				bufferedRowsBytes.set(actualBytes);
				if (debug::PerfTrace::areValuesEnabled()) {
					// Diagnose row-cache scatter separately from storage reads. A diagnostic
					// allocation failure must not reject an already admitted cache value.
					try {
						constexpr bool copied = std::is_lvalue_reference_v<decltype(value)>;
						debug::PerfTrace::addValue(copied ? "logical_plan.prefetch.copied_values" : "logical_plan.prefetch.moved_values", 1);
						debug::PerfTrace::addValue(copied ? "logical_plan.prefetch.copied_retained_bytes" : "logical_plan.prefetch.moved_retained_bytes",
							static_cast<int64_t>(std::min(retainedBytes,static_cast<size_t>((std::numeric_limits<int64_t>::max)()))));
					} catch (...) {}
				}
			};
			const auto hasMetadata = [](const LogicalPlanReadRow &row, const std::string &variable,
										const std::string &key) {
				const auto found = row.entity_metadata_cache.find(variable);
				return found != row.entity_metadata_cache.end() && found->second.contains(key);
			};
			const auto splitBufferedRowsAndFlush = [&]() {
				rows.recalculateBytes();
				MaterializedByteReservation pendingBytes(std::move(bufferedRowsBytes));
				bufferedRowsBytes = MaterializedByteReservation(&state.shared.materialized_budget);
				auto bufferedRows = std::move(rows.values());
				rows.clear();
				const size_t splitCapacity = std::max<size_t>(1U, bufferedRows.size() / 2U);
				std::reverse(bufferedRows.begin(), bufferedRows.end());
				while (!bufferedRows.empty()) {
					if (!rows.empty() && rows.wouldExceedBytes(bufferedRows.back()) && !flush())
						return false;
					const size_t rowBytes = logicalReadRowBytes(bufferedRows.back(), state.shared.context);
					LogicalPlanReadRow row = std::move(bufferedRows.back());
					bufferedRows.pop_back();
					MaterializedByteReservation rowCharge = coordinatedStorage
                        ? pendingBytes.splitOffAccounting(rowBytes, sizeof(LogicalPlanReadRow))
                        : pendingBytes.splitOff(rowBytes);
                    if (coordinatedStorage) rows.appendCharged(std::move(row), std::move(rowCharge));
                    else {
                        rows.append(std::move(row));
                        bufferedRowsBytes.absorb(std::move(rowCharge));
                    }
					if (rows.size() == splitCapacity && !flush())
						return false;
				}
				return rows.empty() || flush();
			};
			flush = [&]() {
				if (rows.empty()) {
					return true;
				}
				checkGuard(state);
				try {
					std::optional<ScopedPropertyColumns> ownedColumns;
					MaterializedByteReservation ownedColumnBytes(&state.shared.materialized_budget);
					if (!completeEntityVariables.empty()) {
						MaterializedByteReservation referenceWorkspaceBytes(&state.shared.materialized_budget);
						size_t maximumNodeReferences = 0U;
						size_t maximumEdgeReferences = 0U;
						for (const LogicalPlanReadRow &row: rows.values()) {
							for (const std::string &variable: completeEntityVariables) {
								const auto found = row.values.find(variable);
								if (found == row.values.end()) {
									throw std::runtime_error("logical-plan complete Entity batch lost its binding");
								}
								if (const auto *reference = std::get_if<storage::EntityRef>(&found->second)) {
									validateEntityRef(*reference);
									size_t &count = reference->is_edge ? maximumEdgeReferences : maximumNodeReferences;
									count = checkedSizeAdd(count, 1U,
												  "logical-plan complete Entity reference count overflowed");
								}
							}
						}
						const auto referenceBytes = [](size_t nodeCount, size_t edgeCount) {
							return checkedSizeAdd(
								checkedSizeMul(nodeCount, sizeof(storage::EntityRef),
											  "logical-plan complete Entity reference buffer size overflowed"),
								checkedSizeMul(edgeCount, sizeof(storage::EntityRef),
											  "logical-plan complete Entity reference buffer size overflowed"),
								"logical-plan complete Entity reference buffer size overflowed");
						};
						const size_t maximumReferenceBytes =
							referenceBytes(maximumNodeReferences, maximumEdgeReferences);
						if (referenceWorkspaceBytes.wouldExceed(maximumReferenceBytes))
							throw PrefetchBytePressure{};
						referenceWorkspaceBytes.set(maximumReferenceBytes);
						std::vector<storage::EntityRef> nodeReferences;
						std::vector<storage::EntityRef> edgeReferences;
						nodeReferences.reserve(maximumNodeReferences);
						edgeReferences.reserve(maximumEdgeReferences);
						const size_t actualReferenceBytes =
							referenceBytes(nodeReferences.capacity(), edgeReferences.capacity());
						if (referenceWorkspaceBytes.wouldExceed(actualReferenceBytes))
							throw PrefetchBytePressure{};
						referenceWorkspaceBytes.set(actualReferenceBytes);
						for (const LogicalPlanReadRow &row: rows.values()) {
							for (const std::string &variable: completeEntityVariables) {
								const auto found = row.values.find(variable);
								if (found == row.values.end()) {
									throw std::runtime_error("logical-plan complete Entity batch lost its binding");
								}
								if (const auto *reference = std::get_if<storage::EntityRef>(&found->second)) {
									validateEntityRef(*reference);
									(reference->is_edge ? edgeReferences : nodeReferences).push_back(*reference);
								}
							}
						}
						const auto materializeForPrefetch = [](auto &&materialize) {
							try {
								materialize();
							} catch (const std::runtime_error &error) {
								if (std::string_view(error.what()) != "shared materialization exceeded its byte bound")
									throw;
								throw PrefetchBytePressure{};
							}
						};
						if (!nodeReferences.empty()) {
							materializeForPrefetch([&] {
								static_cast<void>(materializeNodeReferences(nodeReferences, state, false));
							});
						}
						if (!edgeReferences.empty()) {
							materializeForPrefetch([&] {
								static_cast<void>(materializeEdgeReferences(edgeReferences, state, false));
							});
						}
					}
					if (!metadataGroups.empty()) {
						MaterializedByteReservation metadataWorkspaceBytes(&state.shared.materialized_budget);
						size_t maximumNodeIds = 0U;
						size_t maximumEdgeIds = 0U;
						for (const MetadataGroup &group: metadataGroups) {
							for (const LogicalPlanReadRow &row: rows.values()) {
								const auto found = row.values.find(group.variable);
								if (found == row.values.end()) {
									throw std::runtime_error("logical-plan metadata batch lost its Entity binding");
								}
								const auto *reference = std::get_if<storage::EntityRef>(&found->second);
								if (reference == nullptr)
									continue;
								validateEntityRef(*reference);
								const auto cached = row.entity_metadata_cache.find(group.variable);
								if (!reference->is_edge && group.labels &&
									(cached == row.entity_metadata_cache.end() || !cached->second.contains("labels")) &&
									!state.shared.node_cache.contains(reference->entity_id)) {
									maximumNodeIds = checkedSizeAdd(maximumNodeIds, 1U,
																	"logical-plan metadata Node ID count overflowed");
								}
								if (reference->is_edge && group.type &&
									(cached == row.entity_metadata_cache.end() || !cached->second.contains("type")) &&
									!state.shared.edge_cache.contains(reference->entity_id)) {
									maximumEdgeIds = checkedSizeAdd(maximumEdgeIds, 1U,
																	"logical-plan metadata Edge ID count overflowed");
								}
							}
						}
						const size_t maximumIdBytes =
								checkedSizeAdd(checkedSizeMul(maximumNodeIds, sizeof(uint64_t),
															  "logical-plan metadata ID scratch size overflowed"),
											   checkedSizeMul(maximumEdgeIds, sizeof(uint64_t),
															  "logical-plan metadata ID scratch size overflowed"),
											   "logical-plan metadata ID scratch size overflowed");
						metadataWorkspaceBytes.set(maximumIdBytes);
						std::vector<uint64_t> nodeIds;
						std::vector<uint64_t> edgeIds;
						nodeIds.reserve(maximumNodeIds);
						edgeIds.reserve(maximumEdgeIds);
						metadataWorkspaceBytes.set(
								checkedSizeAdd(checkedSizeMul(nodeIds.capacity(), sizeof(uint64_t),
															  "logical-plan metadata Node ID capacity overflowed"),
											   checkedSizeMul(edgeIds.capacity(), sizeof(uint64_t),
															  "logical-plan metadata Edge ID capacity overflowed"),
											   "logical-plan metadata ID capacity overflowed"));
						for (const MetadataGroup &group: metadataGroups) {
							for (const LogicalPlanReadRow &row: rows.values()) {
								const auto found = row.values.find(group.variable);
								if (found == row.values.end()) {
									throw std::runtime_error("logical-plan metadata batch lost its Entity binding");
								}
								const auto cached = row.entity_metadata_cache.find(group.variable);
								const auto *reference = std::get_if<storage::EntityRef>(&found->second);
								if (reference == nullptr) {
									continue;
								}
								validateEntityRef(*reference);
								if (!reference->is_edge && group.labels &&
									(cached == row.entity_metadata_cache.end() || !cached->second.contains("labels"))) {
									if (!state.shared.node_cache.contains(reference->entity_id)) {
										nodeIds.push_back(reference->entity_id);
									}
								}
								if (reference->is_edge && group.type &&
									(cached == row.entity_metadata_cache.end() || !cached->second.contains("type"))) {
									if (!state.shared.edge_cache.contains(reference->entity_id)) {
										edgeIds.push_back(reference->entity_id);
									}
								}
							}
						}
						std::ranges::sort(nodeIds);
						nodeIds.erase(std::unique(nodeIds.begin(), nodeIds.end()), nodeIds.end());
						std::ranges::sort(edgeIds);
						edgeIds.erase(std::unique(edgeIds.begin(), edgeIds.end()), edgeIds.end());
						if (nodeIds.size() > state.shared.bounds.maximum_node_records ||
							edgeIds.size() > state.shared.bounds.maximum_edge_records) {
							throw std::runtime_error("logical-plan metadata batch exceeded its Entity bound");
						}
						std::vector<storage::LogicalGraphNode> loadedNodes;
						std::vector<storage::LogicalGraphEdge> loadedEdges;
						const auto metadataFrameBytes = [&](const size_t nodeCapacity, const size_t edgeCapacity) {
							return checkedSizeAdd(
									checkedSizeAdd(checkedSizeMul(nodeIds.capacity(), sizeof(uint64_t),
																  "logical-plan metadata ID capacity overflowed"),
												   checkedSizeMul(edgeIds.capacity(), sizeof(uint64_t),
																  "logical-plan metadata ID capacity overflowed"),
												   "logical-plan metadata ID capacity overflowed"),
									checkedSizeAdd(checkedSizeMul(nodeCapacity, sizeof(storage::LogicalGraphNode),
																  "logical-plan metadata Node frame size overflowed"),
												   checkedSizeMul(edgeCapacity, sizeof(storage::LogicalGraphEdge),
																  "logical-plan metadata Edge frame size overflowed"),
												   "logical-plan metadata record frame size overflowed"),
									"logical-plan metadata workspace frame size overflowed");
						};
						metadataWorkspaceBytes.set(metadataFrameBytes(nodeIds.size(), edgeIds.size()));
						loadedNodes.reserve(nodeIds.size());
						loadedEdges.reserve(edgeIds.size());
						metadataWorkspaceBytes.set(metadataFrameBytes(loadedNodes.capacity(), loadedEdges.capacity()));
						for (size_t offset = 0U; offset < nodeIds.size();) {
							checkGuard(state);
							const size_t end = std::min(nodeIds.size(), offset + DEFAULT_PROPERTY_BATCH_ROWS);
							MaterializedByteReservation sourceBytes(&state.shared.materialized_budget);
							std::vector<storage::LogicalGraphNode> batch = readNodesShapedSortedBudgeted(
									state, std::span<const uint64_t>(nodeIds).subspan(offset, end - offset),
									{.include_labels = true}, &sourceBytes);
							if (batch.size() != end - offset) {
								throw std::runtime_error(
										"logical graph backend changed metadata Node batch cardinality");
							}
							for (size_t index = 0U; index < batch.size(); ++index) {
								validateNode(batch[index], state);
								if (batch[index].node_id != nodeIds[offset + index]) {
									throw std::runtime_error(
											"logical graph metadata Node batch changed logical ID order");
								}
							}
							retainShapedBatchBytes(batch, sourceBytes, state.shared.context);
							size_t batchPayloadBytes = 0U;
							for (const storage::LogicalGraphNode &node: batch) {
								batchPayloadBytes =
										checkedSizeAdd(batchPayloadBytes, logicalNodeCacheBytes(node, state.shared.context) - sizeof(node),
													   "logical-plan metadata Node payload size overflowed");
							}
							for (storage::LogicalGraphNode &node: batch)
								loadedNodes.push_back(std::move(node));
							metadataWorkspaceBytes.absorb(sourceBytes.splitOff(batchPayloadBytes));
							offset = end;
						}
						for (size_t offset = 0U; offset < edgeIds.size();) {
							checkGuard(state);
							const size_t end = std::min(edgeIds.size(), offset + DEFAULT_PROPERTY_BATCH_ROWS);
							MaterializedByteReservation sourceBytes(&state.shared.materialized_budget);
							std::vector<storage::LogicalGraphEdge> batch = readEdgesShapedSortedBudgeted(
									state, std::span<const uint64_t>(edgeIds).subspan(offset, end - offset),
									{.include_type = true}, &sourceBytes);
							if (batch.size() != end - offset) {
								throw std::runtime_error(
										"logical graph backend changed metadata Edge batch cardinality");
							}
							for (size_t index = 0U; index < batch.size(); ++index) {
								validateEdge(batch[index]);
								if (batch[index].edge_id != edgeIds[offset + index]) {
									throw std::runtime_error(
											"logical graph metadata Edge batch changed logical ID order");
								}
							}
							retainShapedBatchBytes(batch, sourceBytes, state.shared.context);
							size_t batchPayloadBytes = 0U;
							for (const storage::LogicalGraphEdge &edge: batch) {
								batchPayloadBytes =
										checkedSizeAdd(batchPayloadBytes, logicalEdgeCacheBytes(edge, state.shared.context) - sizeof(edge),
													   "logical-plan metadata Edge payload size overflowed");
							}
							for (storage::LogicalGraphEdge &edge: batch)
								loadedEdges.push_back(std::move(edge));
							metadataWorkspaceBytes.absorb(sourceBytes.splitOff(batchPayloadBytes));
							offset = end;
						}
						const auto putLabels = [&](LogicalPlanReadRow &row, const std::string &variable,
												   const std::vector<std::string> &labels) {
							std::vector<PropertyValue> values;
							values.reserve(labels.size());
							for (const std::string &label: labels) {
								values.emplace_back(label);
							}
							putCacheValue(row.entity_metadata_cache, variable, "labels",
										  PropertyValue(std::move(values)));
						};
						const auto putType = [&](LogicalPlanReadRow &row, const std::string &variable,
												 const std::string &type) {
							putCacheValue(row.entity_metadata_cache, variable, "type", PropertyValue(type));
						};
						for (LogicalPlanReadRow &row: rows.values()) {
							for (const MetadataGroup &group: metadataGroups) {
								const auto found = row.values.find(group.variable);
								if (found == row.values.end()) {
									throw std::runtime_error("logical-plan metadata batch lost its Entity binding");
								}
								if (const auto *node = std::get_if<storage::LogicalGraphNode>(&found->second)) {
									if (group.labels && !hasMetadata(row, group.variable, "labels")) {
										putLabels(row, group.variable, node->labels);
									}
									continue;
								}
								if (const auto *edge = std::get_if<storage::LogicalGraphEdge>(&found->second)) {
									if (group.type && !hasMetadata(row, group.variable, "type")) {
										putType(row, group.variable, edge->type);
									}
									continue;
								}
								const auto *reference = std::get_if<storage::EntityRef>(&found->second);
								if (reference == nullptr) {
									continue;
								}
								validateEntityRef(*reference);
								if (!reference->is_edge && group.labels &&
									!hasMetadata(row, group.variable, "labels")) {
									const auto position = std::ranges::lower_bound(
											loadedNodes, reference->entity_id, {}, &storage::LogicalGraphNode::node_id);
									if (position != loadedNodes.end() && position->node_id == reference->entity_id) {
										putLabels(row, group.variable, position->labels);
									} else {
										const auto cached = state.shared.node_cache.find(reference->entity_id);
										if (cached == state.shared.node_cache.end()) {
											throw std::runtime_error(
													"logical-plan metadata batch lost a requested Node");
										}
										putLabels(row, group.variable, cached->second.labels);
									}
								}
								if (reference->is_edge && group.type && !hasMetadata(row, group.variable, "type")) {
									const auto position = std::ranges::lower_bound(
											loadedEdges, reference->entity_id, {}, &storage::LogicalGraphEdge::edge_id);
									if (position != loadedEdges.end() && position->edge_id == reference->entity_id) {
										validateEdgeReferenceRecord(*reference, *position, state,
																	"logical-plan metadata batch relationship");
										putType(row, group.variable, position->type);
									} else {
										const auto cached = state.shared.edge_cache.find(reference->entity_id);
										if (cached == state.shared.edge_cache.end()) {
											throw std::runtime_error(
													"logical-plan metadata batch lost a requested relationship");
										}
										validateEdgeReferenceRecord(*reference, cached->second, state,
																	"logical-plan metadata batch relationship");
										putType(row, group.variable, cached->second.type);
									}
								}
							}
						}
					}
					for (const PropertyGroup &group: groups) {
						// IDs can contain at most one binding per buffered row. Reserve that
						// temporary gather space before either vector starts growing.
						MaterializedByteReservation propertyReadBytes(&state.shared.materialized_budget);
						const size_t maximumIdScratch = checkedSizeMul(
								checkedSizeMul(rows.size(), 2U, "logical-plan Property ID scratch size overflowed"),
								sizeof(uint64_t), "logical-plan Property ID scratch size overflowed");
						const size_t maximumKeyViewScratch =
								checkedSizeMul(group.keys.size(), sizeof(std::string_view),
											   "logical-plan Property key-view size overflowed");
						propertyReadBytes.set(checkedSizeAdd(maximumIdScratch, maximumKeyViewScratch,
															 "logical-plan Property gather scratch size overflowed"));
						std::vector<uint64_t> nodeIds;
						std::vector<uint64_t> edgeIds;
						for (LogicalPlanReadRow &row: rows.values()) {
							const auto found = row.values.find(group.variable);
							if (found == row.values.end()) {
								throw std::runtime_error("logical-plan Property batch lost its Entity binding");
							}
							if (const auto *reference = std::get_if<storage::EntityRef>(&found->second)) {
								validateEntityRef(*reference);
								if (reference->is_edge) {
									if (const auto cached = state.shared.edge_cache.find(reference->entity_id);
										cached != state.shared.edge_cache.end()) {
										for (const std::string &key: group.keys) {
											const auto property =
													std::ranges::find(cached->second.properties, key,
																	  &storage::LogicalGraphPropertyEntry::key);
											const PropertyValue *value = property == cached->second.properties.end()
																				 ? nullptr
																				 : &property->value;
											if (value == nullptr)
												putCacheValue(row.property_cache, group.variable, key, PropertyValue());
											else
												putCacheValue(row.property_cache, group.variable, key, *value);
										}
										continue;
									}
								} else if (const auto cached = state.shared.node_cache.find(reference->entity_id);
										   cached != state.shared.node_cache.end()) {
									for (const std::string &key: group.keys) {
										const auto property =
												std::ranges::find(cached->second.properties, key,
																  &storage::LogicalGraphPropertyEntry::key);
										const PropertyValue *value = property == cached->second.properties.end()
																			 ? nullptr
																			 : &property->value;
										if (value == nullptr)
											putCacheValue(row.property_cache, group.variable, key, PropertyValue());
										else
											putCacheValue(row.property_cache, group.variable, key, *value);
									}
									continue;
								}
								const auto cached = row.property_cache.find(group.variable);
								const bool complete = cached != row.property_cache.end() &&
													  std::ranges::all_of(group.keys, [&](const std::string &key) {
														  return cached->second.contains(key);
													  });
								if (!complete) {
									(reference->is_edge ? edgeIds : nodeIds).push_back(reference->entity_id);
								}
							}
						}
						std::ranges::sort(nodeIds);
						nodeIds.erase(std::unique(nodeIds.begin(), nodeIds.end()), nodeIds.end());
						std::ranges::sort(edgeIds);
						edgeIds.erase(std::unique(edgeIds.begin(), edgeIds.end()), edgeIds.end());
						std::vector<std::string_view> keys;
						keys.reserve(group.keys.size());
						for (const std::string &key: group.keys) {
							keys.push_back(key);
						}
						const auto columnFrameBytes = [&](const size_t idCount) {
							if (idCount == 0U)
								return size_t{0U};
							return checkedSizeAdd(
									checkedSizeMul(keys.size(), sizeof(std::vector<PropertyValue>),
												   "logical-plan Property column frame size overflowed"),
									checkedSizeMul(checkedSizeMul(keys.size(), idCount,
																  "logical-plan Property column frame size overflowed"),
												   sizeof(PropertyValue),
												   "logical-plan Property column frame size overflowed"),
									"logical-plan Property column frame size overflowed");
						};
						const size_t idStorageBytes =
								checkedSizeAdd(checkedSizeMul(nodeIds.capacity(), sizeof(uint64_t),
															  "logical-plan Property ID storage size overflowed"),
											   checkedSizeMul(edgeIds.capacity(), sizeof(uint64_t),
															  "logical-plan Property ID storage size overflowed"),
											   "logical-plan Property ID storage size overflowed");
						const size_t columnStructures = checkedSizeAdd(
								checkedSizeAdd(idStorageBytes,
											   checkedSizeMul(keys.capacity(), sizeof(std::string_view),
															  "logical-plan Property key-view size overflowed"),
											   "logical-plan Property gather structure size overflowed"),
								checkedSizeAdd(columnFrameBytes(nodeIds.size()), columnFrameBytes(edgeIds.size()),
											   "logical-plan Property column frame size overflowed"),
								"logical-plan Property gather structure size overflowed");
						// IDs and key views now have their actual capacities. Keep their
						// charge while replacing the conservative gather reservation with
						// the exact column-frame reservation before storage allocates results.
						propertyReadBytes.set(columnStructures);
						const std::function<void(size_t)> admitProperty = [&](size_t bytes) {
							const size_t next = checkedSizeAdd(propertyReadBytes.bytes(), bytes,
															   "logical-plan Property source size overflowed");
							if (propertyReadBytes.wouldExceed(next))
								throw PrefetchBytePressure{};
							propertyReadBytes.set(next);
							debug::PerfTrace::addValue(
									"logical_plan.property_source.admitted_bytes",
									static_cast<int64_t>(
											std::min(bytes, static_cast<size_t>(std::numeric_limits<int64_t>::max()))));
						};
						const bool sourceAdmission = state.shared.snapshot.supportsPropertyReadAdmission();
						auto nodeColumns = nodeIds.empty() ? std::vector<std::vector<PropertyValue>>{}
										   : sourceAdmission
												   ? state.shared.snapshot.readNodePropertyColumnsSortedWithAdmission(
															 nodeIds, keys, admitProperty)
												   : state.shared.snapshot.readNodePropertyColumnsSorted(nodeIds, keys);
						auto edgeColumns = edgeIds.empty() ? std::vector<std::vector<PropertyValue>>{}
										   : sourceAdmission
												   ? state.shared.snapshot.readEdgePropertyColumnsSortedWithAdmission(
															 edgeIds, keys, admitProperty)
												   : state.shared.snapshot.readEdgePropertyColumnsSorted(edgeIds, keys);
						// Invalid storage output must fail before budget retries. Otherwise
						// smaller reads could conceal corruption in the original batch.
						const auto validateColumns = [&](const std::vector<uint64_t> &ids,
														 const std::vector<std::vector<PropertyValue>> &columns) {
							const size_t expectedColumns = ids.empty() ? 0U : group.keys.size();
							if (columns.size() != expectedColumns ||
								std::ranges::any_of(columns,
													[&](const auto &column) { return column.size() != ids.size(); })) {
								throw std::runtime_error("logical graph backend changed Property batch cardinality");
							}
						};
						validateColumns(nodeIds, nodeColumns);
						validateColumns(edgeIds, edgeColumns);
						const auto columnPayloadBytes = [&](const auto &columns, const char *message) {
							size_t bytes = 0U;
							for (const auto &column: columns) {
								for (const PropertyValue &value: column) {
									bytes = checkedSizeAdd(bytes, property_utils::getPropertyValueSize(value), message);
								}
							}
							return bytes;
						};
						const size_t nodeColumnBytes =
								columnPayloadBytes(nodeColumns, "logical-plan Node Property batch size overflowed");
						const size_t edgeColumnBytes =
								columnPayloadBytes(edgeColumns, "logical-plan Edge Property batch size overflowed");
						const size_t retainedColumns = checkedSizeAdd(nodeColumnBytes, edgeColumnBytes,
																	  "logical-plan Property batch size overflowed");
						const size_t retainedWorkingSet = checkedSizeAdd(
								columnStructures, retainedColumns, "logical-plan Property working set size overflowed");
						if (retainedWorkingSet > state.shared.bounds.maximum_materialized_bytes) {
							if (rows.size() > 1U) {
								throw PrefetchBytePressure{};
							}
							throw std::runtime_error("logical-plan Property batch exceeded its byte bound");
						}
						// Rebase source wire admission to the existing retained-column
						// estimate once the complete shape has been authenticated. Readers
						// without source admission only gain this post-read reservation.
						if (propertyReadBytes.wouldExceed(retainedWorkingSet))
							throw PrefetchBytePressure{};
						propertyReadBytes.set(retainedWorkingSet);
						if (borrowRepeatedNodeColumns && groups.size() == 1U && metadataGroups.empty() &&
							completeEntityVariables.empty() && edgeIds.empty() && !nodeIds.empty() && nodeIds.size() < rows.size()) {
							ownedColumns.emplace(ScopedPropertyColumns{&group.variable, &group.keys, std::move(nodeIds), std::move(nodeColumns)});
							ownedColumnBytes.absorb(std::move(propertyReadBytes));
						} else {
							const auto fill = [&](const bool isEdge, const std::vector<uint64_t> &ids,
												  std::vector<std::vector<PropertyValue>> &columns) {
								if (ids.empty()) {
									return;
								}
								// Earlier duplicate bindings keep independent copies; the final
								// consumer takes ownership of the decoded column payload.
								std::vector<size_t> lastRow(ids.size(), rows.size());
								for (size_t rowIndex = 0U; rowIndex < rows.size(); ++rowIndex) {
									const auto found = rows.values()[rowIndex].values.find(group.variable);
									if (found == rows.values()[rowIndex].values.end())
										continue;
									const auto *reference = std::get_if<storage::EntityRef>(&found->second);
									if (reference == nullptr || reference->is_edge != isEdge)
										continue;
									const auto position = std::ranges::lower_bound(ids, reference->entity_id);
									if (position != ids.end() && *position == reference->entity_id) {
										lastRow[static_cast<size_t>(position - ids.begin())] = rowIndex;
									}
								}
								for (size_t rowIndex = 0U; rowIndex < rows.size(); ++rowIndex) {
									LogicalPlanReadRow &row = rows.values()[rowIndex];
									const auto found = row.values.find(group.variable);
									if (found == row.values.end()) {
										throw std::runtime_error("logical-plan Property batch lost its Entity binding");
									}
									const auto *reference = std::get_if<storage::EntityRef>(&found->second);
									if (reference == nullptr || reference->is_edge != isEdge) {
										continue;
									}
									const auto position = std::ranges::lower_bound(ids, reference->entity_id);
									if (position == ids.end() || *position != reference->entity_id) {
										continue;
									}
									const size_t ordinal = static_cast<size_t>(position - ids.begin());
									for (size_t column = 0U; column < group.keys.size(); ++column) {
										if (lastRow[ordinal] == rowIndex) {
											const size_t previous = propertyReadBytes.bytes();
											const size_t transferred =
													property_utils::getPropertyValueSize(columns[column][ordinal]);
											propertyReadBytes.set(previous - transferred);
											try {
												putCacheValue(row.property_cache, group.variable, group.keys[column],
															  std::move(columns[column][ordinal]));
											} catch (...) {
												propertyReadBytes.set(previous);
												throw;
											}
										} else {
											putCacheValue(row.property_cache, group.variable, group.keys[column],
														  std::as_const(columns[column][ordinal]));
										}
									}
								}
							};
							fill(false, nodeIds, nodeColumns);
							fill(true, edgeIds, edgeColumns);
							// The output rows now own the selected values. Drop the temporary
							// column matrices and gather vectors before charging downstream row
							// enrichment; retaining their empty/moved-from slots would inflate the
							// true peak and could reject a batch that fits after transfer.
							std::vector<std::vector<PropertyValue>>().swap(nodeColumns);
							std::vector<std::vector<PropertyValue>>().swap(edgeColumns);
							std::vector<uint64_t>().swap(nodeIds);
							std::vector<uint64_t>().swap(edgeIds);
							std::vector<std::string_view>().swap(keys);
							propertyReadBytes.set(0U);
						}
					}
					// Selected columns are attached to the buffered rows during the flush;
					// recompute the byte budget after that enrichment so LargeValue payloads
					// are covered by the same bound as the input rows.
					const size_t bitmapBytes = SelectionVector::requiredStorageBytes(rows.size());
					const size_t sharedAvailable =
							checkedSizeAdd(state.shared.materialized_budget.available(), bufferedRowsBytes.bytes(),
										   "logical-plan prefetch allowance overflowed");
					const size_t enrichedLimit = std::min(state.shared.bounds.maximum_materialized_bytes,
														  sharedAvailable - std::min(sharedAvailable, bitmapBytes));
					size_t enrichedBytes = 0U;
					size_t largestEnrichedRowBytes = 0U;
					for (const LogicalPlanReadRow &row: rows.values()) {
						const size_t rowBytes = logicalReadRowBytes(row, state.shared.context);
						if (rowBytes > enrichedLimit - std::min(enrichedBytes, enrichedLimit)) {
							if (rows.size() > 1U) {
								throw PrefetchBytePressure{};
							}
							throw std::runtime_error("bounded batch exceeded its byte limit");
						}
						enrichedBytes += rowBytes;
						largestEnrichedRowBytes = std::max(largestEnrichedRowBytes, rowBytes);
					}
					rows.recalculateBytes();
					debug::PerfTrace::addValue("logical_plan.batch.flushes", 1);
					debug::PerfTrace::addValue("logical_plan.batch.rows", static_cast<int64_t>(rows.size()));
					debug::PerfTrace::addValue("logical_plan.batch.materialized_bytes",
											   static_cast<int64_t>(rows.bytes()));
					bufferedRowsBytes.set(rows.bytes());
					// Borrowed columns remain resident during downstream callbacks. Keep
					// the same two-row headroom used by input admission after enrichment,
					// before publishing any row; split pressure must never replay effects.
					if (ownedColumns && rows.size() > 1U) {
						const size_t handoffHeadroom = checkedSizeAdd(bitmapBytes,
							checkedSizeMul(largestEnrichedRowBytes, 2U, "logical-plan prefetch handoff headroom overflowed"),
							"logical-plan prefetch handoff headroom overflowed");
						if (handoffHeadroom > state.shared.materialized_budget.available())
							throw PrefetchBytePressure{};
					}
					struct ColumnScope {
						ExecutionState &state; const ScopedPropertyColumns *previous;
						~ColumnScope() { state.task.property_columns = previous; }
					} columnScope{state, state.task.property_columns};
					if (ownedColumns) state.task.property_columns = &*ownedColumns;
					SelectionVector selection(rows.size(), &selectionMemory);
					if (selectionPredicate) {
						for (size_t index = 0U; index < rows.size(); ++index) {
							checkGuard(state);
							if (!selectionPredicate(rows.values()[index], state)) {
								selection.reject(index);
							}
						}
					}
					debug::PerfTrace::addValue("logical_plan.selection.selected_rows",
											   static_cast<int64_t>(selection.selectedCount()));
					debug::PerfTrace::addValue("logical_plan.selection.rejected_rows",
											   static_cast<int64_t>(rows.size() - selection.selectedCount()));
					if (ownedColumns && selectionPredicate) {
						// Filter rows may outlive this flush in downstream batching. Keep
						// independent caches for selected rows only; rejected rows never
						// receive the repeated property payload. Release columns before
						// handing any row to a downstream owner.
						auto &columns = *ownedColumns;
						const size_t scratchBytes = checkedSizeMul(columns.ids.size(), sizeof(size_t),
							"logical-plan selected scatter size overflowed");
						if (scratchBytes > state.shared.materialized_budget.available()) throw PrefetchBytePressure{};
						detail::BudgetMemoryResource scatterMemory(state.shared.materialized_budget);
						std::pmr::vector<size_t> lastSelected(columns.ids.size(), rows.size(), &scatterMemory);
						const auto ordinalFor = [&](const LogicalPlanReadRow &row) -> std::optional<size_t> {
							const auto *reference = std::get_if<storage::EntityRef>(&row.values.at(*columns.variable));
							if (reference == nullptr || reference->is_edge) return std::nullopt;
							const auto position = std::ranges::lower_bound(columns.ids, reference->entity_id);
							if (position == columns.ids.end() || *position != reference->entity_id)
								throw std::runtime_error("logical-plan selected column lost its Node binding");
							return static_cast<size_t>(position - columns.ids.begin());
						};
						for (size_t index = 0U; index < rows.size(); ++index) {
							if (!selection.includes(index)) continue;
							if (const auto ordinal = ordinalFor(rows.values()[index])) lastSelected[*ordinal] = index;
						}
						for (size_t index = 0U; index < rows.size(); ++index) {
							if (!selection.includes(index)) continue;
							auto &row = rows.values()[index];
							const auto selectedOrdinal = ordinalFor(row);
							if (!selectedOrdinal) continue;
							const size_t ordinal = *selectedOrdinal;
							for (size_t column = 0U; column < columns.keys->size(); ++column) {
								if (lastSelected[ordinal] == index) {
									const size_t previous = ownedColumnBytes.bytes();
									const size_t transferred = property_utils::getPropertyValueSize(columns.columns[column][ordinal]);
									ownedColumnBytes.set(previous - transferred);
									try {
										putCacheValue(row.property_cache, *columns.variable, (*columns.keys)[column],
											std::move(columns.columns[column][ordinal]));
									} catch (...) { ownedColumnBytes.set(previous); throw; }
								} else {
									putCacheValue(row.property_cache, *columns.variable, (*columns.keys)[column],
										std::as_const(columns.columns[column][ordinal]));
								}
							}
						}
						state.task.property_columns = columnScope.previous;
						ownedColumns.reset();
						ownedColumnBytes.reset();
					}
					// Predicates can populate additional caches. Reconcile those before
					// removing rows so each handoff refunds the current payload size.
					rows.recalculateBytes();
					bufferedRowsBytes.set(rows.bytes());
					std::reverse(rows.values().begin(), rows.values().end());
					bool keepGoing = true;
					const size_t rowCount = rows.size();
					for (size_t index = 0U; index < rowCount; ++index) {
						checkGuard(state);
                        if (coordinatedStorage) {
                            if (!selection.includes(index)) { rows.discardBack(); continue; }
                            auto charged = rows.popBackCharged();
                            const bool accepted = chargedReceiver
                                ? (*chargedReceiver)(std::move(charged.value), std::move(charged.reservation))
                                : visitor(std::move(charged.value));
                            if (!accepted) { keepGoing = false; break; }
                            continue;
                        }
						LogicalPlanReadRow row = rows.popBack();
						if (selection.includes(index)) {
							if (chargedReceiver != nullptr) {
								MaterializedByteReservation rowCharge =
										bufferedRowsBytes.splitOff(logicalReadRowBytes(row, state.shared.context));
								if (!(*chargedReceiver)(std::move(row), std::move(rowCharge))) {
									keepGoing = false;
									break;
								}
							} else {
								bufferedRowsBytes.set(rows.bytes());
								if (!visitor(std::move(row))) {
									keepGoing = false;
									break;
								}
							}
						} else {
							bufferedRowsBytes.set(rows.bytes());
						}
					}
					rows.clear();
					bufferedRowsBytes.reset();
					largestInputRowBytes = 0U;
					return keepGoing;
				} catch (const PrefetchBytePressure &) {
					if (rows.size() > 1U)
						return splitBufferedRowsAndFlush();
					throw std::runtime_error("bounded batch exceeded its byte limit");
				}
			};

			const auto acceptInput = [&](LogicalPlanReadRow row, MaterializedByteReservation charge) {
				// Leave room for cache enrichment rather than filling the entire
				// allowance with input references before their payload sizes are known.
				const size_t rowBytes = logicalReadRowBytes(row, state.shared.context);
				const size_t batchAllowance =
						checkedSizeAdd(checkedSizeAdd(state.shared.materialized_budget.available(), bufferedRowsBytes.bytes(),
													  "logical-plan prefetch input allowance overflowed"),
									   charge.bytes(), "logical-plan prefetch input allowance overflowed");
				const size_t headroom = checkedSizeMul(std::max(largestInputRowBytes, rowBytes), 2U,
													   "logical-plan prefetch input headroom overflowed");
				const size_t inputAllowance = batchAllowance - std::min(batchAllowance, headroom);
				if (!rows.empty() && (rows.wouldExceedBytes(row) ||
									  rowBytes > inputAllowance - std::min(rows.bytes(), inputAllowance))) {
					if (!flush()) {
						stoppedByVisitor = true;
						return false;
					}
				}
                if (coordinatedStorage) {
                    if (charge.bytes() == 0U) rows.append(std::move(row));
                    else rows.appendCharged(std::move(row), std::move(charge));
                } else {
                    rows.append(std::move(row));
                    if (charge.bytes() == 0U) bufferedRowsBytes.set(rows.bytes());
                    else bufferedRowsBytes.absorb(std::move(charge));
                }
				largestInputRowBytes = std::max(largestInputRowBytes, rowBytes);
				// A first cache-enriched row can already consume the headroom. Flush
				// it now so the upstream source can admit the next selected payload.
				if (rows.size() == batchCapacity || rows.bytes() > inputAllowance) {
					const bool keepGoing = flush();
					if (!keepGoing) {
						stoppedByVisitor = true;
					}
					return keepGoing;
				}
				return true;
			};
			const ChargedRowVisitor chargedInput = acceptInput;
			const bool childCompleted = visitOperator(
					child, state,
					[&](LogicalPlanReadRow row) {
						return acceptInput(std::move(row), MaterializedByteReservation(&state.shared.materialized_budget));
					},
					supportsChargedRowHandoff(child.getType()) ? &chargedInput : nullptr);
			if (stoppedByVisitor || !flush()) {
				return false;
			}
			return childCompleted;
		}

