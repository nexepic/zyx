		[[nodiscard]] PropertyValue evaluateScalarExpression(const expressions::Expression &expression,
															 LogicalPlanReadRow &row, ExecutionState &state,
															 ShortestPathResultMode shortestMode) {
			using expressions::BinaryOperatorType;
			using expressions::ExpressionType;
			switch (expression.getExpressionType()) {
				case ExpressionType::LITERAL:
				case ExpressionType::VARIABLE_REFERENCE:
				case ExpressionType::PROPERTY_ACCESS:
				case ExpressionType::PARAMETER:
				case ExpressionType::EXPR_LIST_LITERAL:
					return scalarValue(evaluateExpression(expression, row, state));
				case ExpressionType::BINARY_OP: {
					const auto &binary = dynamic_cast<const expressions::BinaryOpExpression &>(expression);
					const PropertyValue left = scalarValue(evaluateExpression(*binary.getLeft(), row, state));
					MaterializedByteReservation operandBytes(&state.shared.materialized_budget);
					operandBytes.set(retainedExpressionValueBytes(left, state));
					const BinaryOperatorType operation = binary.getOperator();
					if (operation == BinaryOperatorType::BOP_AND || operation == BinaryOperatorType::BOP_OR) {
						const std::optional<bool> leftBoolean = booleanOrNull(left);
						if ((operation == BinaryOperatorType::BOP_AND && leftBoolean == false) ||
							(operation == BinaryOperatorType::BOP_OR && leftBoolean == true)) {
							return PropertyValue(*leftBoolean);
						}
					}
					const PropertyValue right = scalarValue(evaluateExpression(*binary.getRight(), row, state));
					operandBytes.set(checkedSizeAdd(operandBytes.bytes(), retainedExpressionValueBytes(right, state),
													"logical-plan binary operand storage size overflowed"));
					if (expressions::isArithmeticOperator(operation)) {
						return arithmetic(operation, left, right);
					}
					if (operation == BinaryOperatorType::BOP_STARTS_WITH ||
						operation == BinaryOperatorType::BOP_ENDS_WITH ||
						operation == BinaryOperatorType::BOP_CONTAINS ||
						operation == BinaryOperatorType::BOP_REGEX_MATCH) {
						return stringOperation(operation, left, right);
					}
					if (expressions::isComparisonOperator(operation)) {
						return comparison(operation, left, right);
					}
					if (expressions::isLogicalOperator(operation)) {
						return logical(operation, left, right);
					}
					throw std::logic_error("logical-plan runtime does not support this binary operation");
				}
				case ExpressionType::UNARY_OP: {
					const auto &unary = dynamic_cast<const expressions::UnaryOpExpression &>(expression);
					const PropertyValue operand = scalarValue(evaluateExpression(*unary.getOperand(), row, state));
					if (isNull(operand)) {
						return PropertyValue();
					}
					switch (unary.getOperator()) {
						case expressions::UnaryOperatorType::UOP_MINUS: {
							if (const int64_t *integer = std::get_if<int64_t>(&operand.getVariant())) {
								if (*integer == std::numeric_limits<int64_t>::min()) {
									throw std::overflow_error("logical-plan integer negation overflowed");
								}
								return PropertyValue(-*integer);
							}
							if (const double *number = std::get_if<double>(&operand.getVariant())) {
								return PropertyValue(-*number);
							}
							throw std::runtime_error("logical-plan unary minus requires a numeric operand");
						}
						case expressions::UnaryOperatorType::UOP_NOT: {
							const std::optional<bool> predicate = booleanOrNull(operand);
							return makeBoolean(predicate.has_value() ? std::optional<bool>(!*predicate) : std::nullopt);
						}
					}
					throw std::logic_error("logical-plan runtime received an unknown unary operation");
				}
				case ExpressionType::IN_LIST: {
					const auto &in = dynamic_cast<const expressions::InExpression &>(expression);
					const PropertyValue actual = scalarValue(evaluateExpression(*in.getValue(), row, state));
					if (isNull(actual)) {
						return PropertyValue();
					}
					MaterializedByteReservation operandBytes(&state.shared.materialized_budget);
					operandBytes.set(retainedExpressionValueBytes(actual, state));
					if (in.hasDynamicList()) {
						const PropertyValue candidates =
								scalarValue(evaluateExpression(*in.getListExpression(), row, state));
						if (isNull(candidates)) {
							return PropertyValue();
						}
						operandBytes.set(checkedSizeAdd(operandBytes.bytes(),
														retainedExpressionValueBytes(candidates, state),
														"logical-plan IN operand storage size overflowed"));
						bool sawNull = false;
						for (const PropertyValue &candidate: requireBoundedList(candidates, state, "IN")) {
							if (isNull(candidate)) {
								sawNull = true;
								continue;
							}
							if (actual == candidate) {
								return PropertyValue(true);
							}
						}
						return sawNull ? PropertyValue() : PropertyValue(false);
					}
					bool sawNull = false;
					for (const PropertyValue &candidate: in.getListValues()) {
						if (isNull(candidate)) {
							sawNull = true;
							continue;
						}
						if (actual == candidate) {
							return PropertyValue(true);
						}
					}
					return sawNull ? PropertyValue() : PropertyValue(false);
				}
				case ExpressionType::IS_NULL: {
					const auto &isNullExpression = dynamic_cast<const expressions::IsNullExpression &>(expression);
					const bool null =
							isNull(scalarValue(evaluateExpression(*isNullExpression.getExpression(), row, state)));
					return PropertyValue(isNullExpression.isNegated() ? !null : null);
				}
				case ExpressionType::FUNCTION_CALL: {
					return evaluateFunctionExpression(
							dynamic_cast<const expressions::FunctionCallExpression &>(expression), row, state);
				}
				case ExpressionType::CASE_EXPRESSION: {
					const auto &caseExpression = dynamic_cast<const expressions::CaseExpression &>(expression);
					if (caseExpression.isSimpleCase()) {
						const PropertyValue comparisonValue =
								scalarValue(evaluateExpression(*caseExpression.getComparisonExpression(), row, state));
						MaterializedByteReservation comparisonBytes(&state.shared.materialized_budget);
						comparisonBytes.set(retainedExpressionValueBytes(comparisonValue, state));
						for (const expressions::CaseBranch &branch: caseExpression.getBranches()) {
							if (!branch.whenExpression || !branch.thenExpression) {
								throw std::logic_error("logical-plan CASE expression has an incomplete branch");
							}
							const PropertyValue whenValue =
									scalarValue(evaluateExpression(*branch.whenExpression, row, state));
							// Only the current WHEN value coexists with the comparison.
							// Release its charge before evaluating the next branch.
							MaterializedByteReservation whenBytes(&state.shared.materialized_budget);
							whenBytes.set(retainedExpressionValueBytes(whenValue, state));
							if (!isNull(whenValue) && comparisonValue == whenValue) {
								return scalarValue(evaluateExpression(*branch.thenExpression, row, state));
							}
						}
					} else {
						for (const expressions::CaseBranch &branch: caseExpression.getBranches()) {
							if (!branch.whenExpression || !branch.thenExpression) {
								throw std::logic_error("logical-plan CASE expression has an incomplete branch");
							}
							const std::optional<bool> predicate =
									booleanOrNull(scalarValue(evaluateExpression(*branch.whenExpression, row, state)));
							if (predicate == true) {
								return scalarValue(evaluateExpression(*branch.thenExpression, row, state));
							}
						}
					}
					return caseExpression.getElseExpression() == nullptr
								   ? PropertyValue()
								   : scalarValue(evaluateExpression(*caseExpression.getElseExpression(), row, state));
				}
				case ExpressionType::LIST_SLICE: {
					const auto &slice = dynamic_cast<const expressions::ListSliceExpression &>(expression);
					if (slice.getList() == nullptr) {
						throw std::logic_error("logical-plan List slice has no source expression");
					}
					MaterializedByteReservation sourceListBytes(&state.shared.materialized_budget);
					PropertyValue listValue = scalarValue(evaluateExpression(*slice.getList(), row, state));
					if (isNull(listValue)) {
						return PropertyValue();
					}
					const std::vector<PropertyValue> &list = requireBoundedList(listValue, state, "List slice");
					sourceListBytes.set(materializedListStorageBytes(listValue, state));
					if (list.size() > static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
						throw std::overflow_error("logical-plan List slice length exceeds signed index capacity");
					}
					const int64_t length = static_cast<int64_t>(list.size());
					const auto evaluateIndex = [&](const expressions::Expression *indexExpression,
												   const char *description) -> int64_t {
						if (indexExpression == nullptr) {
							throw std::logic_error(std::string("logical-plan List slice has no ") + description);
						}
						const PropertyValue value = scalarValue(evaluateExpression(*indexExpression, row, state));
						if (const auto *integer = std::get_if<int64_t>(&value.getVariant())) {
							return *integer;
						}
						throw std::runtime_error("logical-plan List slice index must be an integer");
					};
					const auto normalize = [length](int64_t index) {
						if (index >= 0) {
							return index;
						}
						return index < -length ? std::numeric_limits<int64_t>::min() : length + index;
					};
					if (!slice.hasRange()) {
						const int64_t index = normalize(evaluateIndex(slice.getStart(), "index"));
						if (index < 0 || index >= length)
							return PropertyValue();
						MaterializedByteReservation selectedBytes(&state.shared.materialized_budget);
						selectedBytes.set(retainedExpressionValueBytes(list[static_cast<size_t>(index)], state));
						return list[static_cast<size_t>(index)];
					}
					int64_t start =
							slice.getStart() == nullptr ? 0 : normalize(evaluateIndex(slice.getStart(), "start"));
					int64_t end = slice.getEnd() == nullptr ? length : normalize(evaluateIndex(slice.getEnd(), "end"));
					start = std::clamp(start, int64_t{0}, length);
					end = std::clamp(end, int64_t{0}, length);
					if (start == 0 && end == length)
						return listValue;
					const size_t resultCount = static_cast<size_t>(std::max(int64_t{0}, end - start));
					size_t resultSize =
							checkedSizeAdd(sizeof(uint32_t),
										   checkedSizeMul(resultCount, sizeof(PropertyValue),
														  "logical-plan List slice result capacity overflowed"),
										   "logical-plan List slice result size overflowed");
					for (int64_t index = start; index < end; ++index) {
						resultSize = checkedSizeAdd(
								resultSize, retainedExpressionValueBytes(list[static_cast<size_t>(index)], state),
								"logical-plan List slice result size overflowed");
					}
					MaterializedByteReservation resultBytes(&state.shared.materialized_budget);
					resultBytes.set(resultSize);
					std::vector<PropertyValue> result;
					result.reserve(resultCount);
					resultBytes.set(
							checkedSizeAdd(resultSize,
										   checkedSizeMul(result.capacity() - resultCount, sizeof(PropertyValue),
														  "logical-plan List slice result capacity overflowed"),
										   "logical-plan List slice result size overflowed"));
					for (int64_t index = start; index < end; ++index) {
						result.push_back(list[static_cast<size_t>(index)]);
					}
					return PropertyValue(std::move(result));
				}
				case ExpressionType::LIST_COMPREHENSION: {
					const auto &comprehension =
							dynamic_cast<const expressions::ListComprehensionExpression &>(expression);
					if (comprehension.getListExpr() == nullptr || comprehension.getVariable().empty()) {
						throw std::logic_error("logical-plan List comprehension has no source or variable");
					}
					const PropertyValue listValue =
							scalarValue(evaluateExpression(*comprehension.getListExpr(), row, state));
					if (isNull(listValue)) {
						return PropertyValue();
					}
					const std::vector<PropertyValue> &list = requireBoundedList(listValue, state, "List comprehension");
					if (comprehension.getType() ==
						expressions::ListComprehensionExpression::ComprehensionType::COMP_REDUCE) {
						throw std::logic_error("logical-plan List comprehension cannot use a reduce mode");
					}
					MaterializedByteReservation sourceListBytes(&state.shared.materialized_budget);
					sourceListBytes.set(materializedListStorageBytes(listValue, state));
					MaterializedByteReservation resultBytes(&state.shared.materialized_budget);
					std::vector<PropertyValue> result;
					size_t resultPayloadBytes = sizeof(uint32_t);
					for (const PropertyValue &element: list) {
						ScopedRowBinding binding(row, comprehension.getVariable(), element, state);
						if (comprehension.getWhereExpr() != nullptr) {
							const PropertyValue predicate =
									scalarValue(evaluateExpression(*comprehension.getWhereExpr(), row, state));
							if (!isNull(predicate) && predicate.getType() != PropertyType::BOOLEAN) {
								throw std::runtime_error(
										"logical-plan List comprehension predicate must be Boolean or NULL");
							}
							if (isNull(predicate) || !std::get<bool>(predicate.getVariant())) {
								continue;
							}
						}
						PropertyValue mapped =
								comprehension.getMapExpr() != nullptr
										? scalarValue(evaluateExpression(*comprehension.getMapExpr(), row, state))
										: element;
						const size_t nextPayloadBytes =
								checkedSizeAdd(resultPayloadBytes, retainedExpressionValueBytes(mapped, state),
											   "logical-plan List comprehension result size overflowed");
						if (nextPayloadBytes > state.shared.bounds.maximum_materialized_bytes) {
							throw std::runtime_error(
									"logical-plan List comprehension exceeded its materialized byte bound");
						}
						const size_t nextCapacity =
								result.size() == result.capacity()
										? std::min(list.size(),
												   std::max(result.size() + 1U,
															checkedSizeMul(result.capacity(), 2U,
																		   "logical-plan List comprehension result "
																		   "capacity overflowed")))
										: result.capacity();
						resultBytes.set(checkedSizeAdd(
								nextPayloadBytes,
								checkedSizeMul(checkedSizeAdd(nextCapacity,
											 nextCapacity != result.capacity() ? result.capacity() : 0U,
											 "logical-plan List growth slot count overflowed"), sizeof(PropertyValue),
											   "logical-plan List comprehension result capacity overflowed"),
								"logical-plan List comprehension result size overflowed"));
						if (nextCapacity != result.capacity()) {
							result.reserve(nextCapacity);
							resultBytes.set(checkedSizeAdd(
									nextPayloadBytes,
									checkedSizeMul(result.capacity(), sizeof(PropertyValue),
												   "logical-plan List comprehension result capacity overflowed"),
									"logical-plan List comprehension result size overflowed"));
						}
						result.push_back(std::move(mapped));
						resultPayloadBytes = nextPayloadBytes;
					}
					return PropertyValue(std::move(result));
				}
				case ExpressionType::EXPR_REDUCE: {
					const auto &reduce = dynamic_cast<const expressions::ReduceExpression &>(expression);
					if (reduce.getInitialExpr() == nullptr || reduce.getListExpr() == nullptr ||
						reduce.getBodyExpr() == nullptr || reduce.getAccumulator().empty() ||
						reduce.getVariable().empty()) {
						throw std::logic_error("logical-plan REDUCE expression is incomplete");
					}
					MaterializedByteReservation accumulatorBytes(&state.shared.materialized_budget);
					PropertyValue accumulator = scalarValue(evaluateExpression(*reduce.getInitialExpr(), row, state));
					accumulatorBytes.set(retainedExpressionValueBytes(accumulator, state));
					MaterializedByteReservation sourceListBytes(&state.shared.materialized_budget);
					const PropertyValue listValue = scalarValue(evaluateExpression(*reduce.getListExpr(), row, state));
					if (isNull(listValue)) {
						return PropertyValue();
					}
					const std::vector<PropertyValue> &list = requireBoundedList(listValue, state, "REDUCE");
					sourceListBytes.set(materializedListStorageBytes(listValue, state));
					for (const PropertyValue &element: list) {
						MaterializedByteReservation nextBytes(&state.shared.materialized_budget);
						PropertyValue next;
						{
							ScopedRowBinding accumulatorBinding(row, reduce.getAccumulator(), accumulator, state);
							ScopedRowBinding elementBinding(row, reduce.getVariable(), element, state);
							next = scalarValue(evaluateExpression(*reduce.getBodyExpr(), row, state));
						}
						nextBytes.set(retainedExpressionValueBytes(next, state));
						accumulator = std::move(next);
						accumulatorBytes = std::move(nextBytes);
					}
					return accumulator;
				}
				case ExpressionType::EXPR_QUANTIFIER_FUNCTION: {
					const auto &quantifier =
							dynamic_cast<const expressions::QuantifierFunctionExpression &>(expression);
					if (quantifier.getListExpression() == nullptr || quantifier.getWhereExpression() == nullptr ||
						quantifier.getVariable().empty()) {
						throw std::logic_error("logical-plan quantifier expression is incomplete");
					}
					MaterializedByteReservation sourceListBytes(&state.shared.materialized_budget);
					const PropertyValue listValue =
							scalarValue(evaluateExpression(*quantifier.getListExpression(), row, state));
					if (isNull(listValue)) {
						return PropertyValue();
					}
					const std::vector<PropertyValue> &list = requireBoundedList(listValue, state, "quantifier");
					sourceListBytes.set(materializedListStorageBytes(listValue, state));
					const std::string function = lowerCase(quantifier.getFunctionName());
					if (function != "all" && function != "any" && function != "none" && function != "single") {
						throw std::runtime_error("logical-plan quantifier function is not supported");
					}
					size_t matches = 0U;
					bool sawNull = false;
					for (const PropertyValue &element: list) {
						ScopedRowBinding binding(row, quantifier.getVariable(), element, state);
						const PropertyValue predicate =
								scalarValue(evaluateExpression(*quantifier.getWhereExpression(), row, state));
						if (isNull(predicate)) {
							sawNull = true;
							continue;
						}
						if (predicate.getType() != PropertyType::BOOLEAN) {
							throw std::runtime_error("logical-plan quantifier predicate must be Boolean or NULL");
						}
						const bool matched = std::get<bool>(predicate.getVariant());
						if (function == "all" && !matched) {
							return PropertyValue(false);
						}
						if (function == "any" && matched) {
							return PropertyValue(true);
						}
						if (function == "none" && matched) {
							return PropertyValue(false);
						}
						if (function == "single" && matched && ++matches > 1U) {
							return PropertyValue(false);
						}
					}
					if (function == "all" || function == "none") {
						return sawNull ? PropertyValue() : PropertyValue(true);
					}
					if (function == "any") {
						return sawNull ? PropertyValue() : PropertyValue(false);
					}
					return matches == 1U ? PropertyValue(true) : (sawNull ? PropertyValue() : PropertyValue(false));
				}
				case ExpressionType::MAP_PROJECTION: {
					const auto &projection = dynamic_cast<const expressions::MapProjectionExpression &>(expression);
					if (!projection.isMapLiteral() && projection.getVariable().empty()) {
						throw std::logic_error("logical-plan Map projection has an empty variable");
					}
					if (projection.getItems().size() > state.shared.bounds.maximum_property_entries) {
						throw std::runtime_error("logical-plan Map projection exceeded the configured Property bound");
					}
					if (!projection.isMapLiteral()) {
						const LogicalPlanReadValue &binding = requireBinding(row, projection.getVariable());
						if (const PropertyValue *value = std::get_if<PropertyValue>(&binding)) {
							if (value->getType() == PropertyType::NULL_TYPE) {
								return PropertyValue();
							}
							if (value->getType() != PropertyType::MAP) {
								throw std::runtime_error("logical-plan Map projection requires a node, relationship, "
														 "map, or NULL variable");
							}
						}
					}
					MaterializedByteReservation resultBytes(&state.shared.materialized_budget);
					PropertyValue::MapType result;
					size_t resultPayloadBytes = sizeof(uint32_t);
					resultBytes.set(resultPayloadBytes);
					const auto insertResult = [&](const std::string &key, const PropertyValue &value) {
						const auto previous = result.find(key);
						const bool newKey = previous == result.end();
						if (newKey && result.size() == state.shared.bounds.maximum_property_entries) {
							throw std::runtime_error(
									"logical-plan Map projection exceeded the configured Property bound");
						}
						const size_t valueBytes = property_utils::getPropertyValueSize(value);
						size_t nextPayloadBytes = resultPayloadBytes;
						if (newKey) {
							nextPayloadBytes = checkedSizeAdd(
									nextPayloadBytes,
									checkedSizeAdd(checkedSizeAdd(key.size(), sizeof(size_t),
																  "logical-plan Map projection result size overflowed"),
												   valueBytes, "logical-plan Map projection result size overflowed"),
									"logical-plan Map projection result size overflowed");
						} else {
							const size_t previousBytes = property_utils::getPropertyValueSize(previous->second);
							nextPayloadBytes = checkedSizeAdd(nextPayloadBytes - previousBytes, valueBytes,
															  "logical-plan Map projection result size overflowed");
						}
						const size_t nextEntries = result.size() + static_cast<size_t>(newKey);
						const size_t entryBytes =
								checkedSizeMul(nextEntries, sizeof(PropertyValue::MapType::value_type) + sizeof(void *),
											   "logical-plan Map projection result capacity overflowed");
						const size_t nextResultBytes = checkedSizeAdd(
								nextPayloadBytes, entryBytes, "logical-plan Map projection result size overflowed");
						if (newKey) {
							resultBytes.set(nextResultBytes);
							result.insert_or_assign(key, value);
						} else {
							MaterializedByteReservation replacementBytes(&state.shared.materialized_budget);
							replacementBytes.set(valueBytes);
							result.insert_or_assign(key, value);
							replacementBytes.reset();
							resultBytes.set(nextResultBytes);
						}
						resultPayloadBytes = nextPayloadBytes;
					};
					for (const expressions::MapProjectionItem &item: projection.getItems()) {
						switch (item.type) {
							case expressions::MapProjectionItemType::MPROP_PROPERTY: {
								if (projection.isMapLiteral()) {
									throw std::logic_error("logical-plan Map literal has a Property selector");
								}
								if (item.key.empty()) {
									throw std::logic_error("logical-plan Map projection has an empty Property key");
								}
								const PropertyValue selected =
										readProperty(state.shared.snapshot, row, projection.getVariable(), item.key, state);
								MaterializedByteReservation selectedBytes(&state.shared.materialized_budget);
								selectedBytes.set(property_utils::getPropertyValueSize(selected));
								insertResult(item.key, selected);
								break;
							}
							case expressions::MapProjectionItemType::MPROP_LITERAL: {
								if (item.key.empty()) {
									throw std::logic_error("logical-plan Map projection has an empty literal key");
								}
								const PropertyValue literal =
										item.expression == nullptr
												? PropertyValue()
												: scalarValue(evaluateExpression(*item.expression, row, state));
								MaterializedByteReservation literalBytes(&state.shared.materialized_budget);
								literalBytes.set(property_utils::getPropertyValueSize(literal));
								insertResult(item.key, literal);
								break;
							}
							case expressions::MapProjectionItemType::MPROP_ALL_PROPERTIES: {
								if (projection.isMapLiteral()) {
									throw std::logic_error("logical-plan Map literal has an all-Properties selector");
								}
								MaterializedByteReservation sourceBytes(&state.shared.materialized_budget);
								const LogicalPlanReadValue &binding = requireBinding(row, projection.getVariable());
								const auto *mapValue = std::get_if<PropertyValue>(&binding);
								const auto cached = row.property_cache.find(projection.getVariable());
								const PropertyMap &properties =
										mapValue != nullptr && cached == row.property_cache.end()
												? mapValue->getMap()
												: readBindingProperties(state.shared.snapshot, row, projection.getVariable(),
																		state);
								sourceBytes.set(propertyMapBytes(properties,
																 "logical-plan Map projection source size overflowed"));
								for (const auto &[key, value]: properties) {
									insertResult(key, value);
								}
								break;
							}
						}
					}
					return PropertyValue(std::move(result));
				}
				case ExpressionType::EXPR_EXISTS: {
					const auto &exists = dynamic_cast<const expressions::ExistsExpression &>(expression);
					if (!exists.hasStructuredPattern() || exists.getSourceVar().empty()) {
						return PropertyValue(false);
					}
					const auto source = row.values.find(exists.getSourceVar());
					if (source == row.values.end()) {
						return PropertyValue(false);
					}
					const bool hasRelationship = exists.getPattern().find('-') != std::string::npos;
					if (hasRelationship && !exists.hasWhereClause() && exists.getTargetVar().empty() &&
						exists.getTargetLabel().empty()) {
						if (const std::optional<bool> referenceMatch = findPatternReferenceMatch(
									nodeBindingId(source->second, state), exists.getRelType(), exists.getDirection(), state);
							referenceMatch.has_value()) {
							return PropertyValue(*referenceMatch);
						}
					}
					const storage::LogicalGraphNode sourceNode = patternSourceIdentity(state, source->second);
					const auto matchesWhere = [&](const storage::LogicalGraphNode *target) {
						if (target != nullptr && !exists.getTargetVar().empty()) {
							if (const auto bound = row.values.find(exists.getTargetVar());
								bound != row.values.end() && nodeBindingId(bound->second, state) != target->node_id) {
								return false;
							}
							ScopedRowBinding binding(row, exists.getTargetVar(), *target, state);
							return !exists.hasWhereClause() ||
								   booleanOrNull(scalarValue(
										   evaluateExpression(*exists.getWhereExpression(), row, state))) == true;
						}
						return !exists.hasWhereClause() || booleanOrNull(scalarValue(evaluateExpression(
																   *exists.getWhereExpression(), row, state))) == true;
					};
					if (!hasRelationship) {
						return PropertyValue(matchesWhere(nullptr));
					}
					const std::optional<PatternBindingReadShape> patternShape = inspectPatternBindingReadShape(
							exists.getWhereExpression(), nullptr, exists.getTargetVar(), {}, state.shared.snapshot);
					bool matched = false;
					static_cast<void>(visitPatternTargets(
							sourceNode, exists.getRelType(), exists.getTargetLabel(), exists.getDirection(), state,
							[&](const storage::LogicalGraphEdge &, const storage::LogicalGraphNode &target) {
								if (!matchesWhere(&target)) {
									return true;
								}
								matched = true;
								return false;
							},
							patternShape.has_value() ? &*patternShape : nullptr, true));
					return PropertyValue(matched);
				}
				case ExpressionType::EXPR_PATTERN_COMPREHENSION: {
					const auto &comprehension =
							dynamic_cast<const expressions::PatternComprehensionExpression &>(expression);
					if (comprehension.getSourceVar().empty() || comprehension.getMapExpression() == nullptr) {
						throw std::logic_error(
								"logical-plan Pattern comprehension has an incomplete pattern or map expression");
					}
					const auto source = row.values.find(comprehension.getSourceVar());
					if (source == row.values.end()) {
						return PropertyValue(std::vector<PropertyValue>{});
					}
					const storage::LogicalGraphNode sourceNode = patternSourceIdentity(state, source->second);
					MaterializedByteReservation valuesReservation(&state.shared.materialized_budget);
					std::vector<PropertyValue> values;
					size_t valuesBytes = sizeof(uint32_t);
					const auto appendValue = [&](const storage::LogicalGraphEdge *edge,
												 const storage::LogicalGraphNode *target) {
						const auto appendBoundValue = [&]() {
							if (comprehension.hasWhereClause() &&
								booleanOrNull(scalarValue(
										evaluateExpression(*comprehension.getWhereExpression(), row, state))) != true) {
								return true;
							}
							if (values.size() == state.shared.bounds.maximum_rows) {
								throw std::runtime_error(
										"logical-plan Pattern comprehension exceeded its result bound");
							}
							PropertyValue mapped =
									scalarValue(evaluateExpression(*comprehension.getMapExpression(), row, state));
							const size_t mappedBytes = retainedExpressionValueBytes(mapped, state);
							const size_t nextValueBytes =
									checkedSizeAdd(valuesBytes, mappedBytes,
												   "logical-plan Pattern comprehension result size overflowed");
							if (nextValueBytes > state.shared.bounds.maximum_materialized_bytes) {
								throw std::runtime_error(
										"logical-plan Pattern comprehension exceeded its materialized byte bound");
							}
							const size_t nextCapacity =
									values.size() == values.capacity()
											? std::min(state.shared.bounds.maximum_rows,
													   std::max(values.size() + 1U,
																checkedSizeMul(values.capacity(), 2U,
																			   "logical-plan Pattern comprehension "
																			   "capacity overflowed")))
											: values.capacity();
							valuesReservation.set(checkedSizeAdd(
									nextValueBytes,
									checkedSizeMul(checkedSizeAdd(nextCapacity,
												 nextCapacity != values.capacity() ? values.capacity() : 0U,
												 "logical-plan Pattern growth slot count overflowed"), sizeof(PropertyValue),
												   "logical-plan Pattern comprehension capacity overflowed"),
									"logical-plan Pattern comprehension result size overflowed"));
							if (nextCapacity != values.capacity()) {
								values.reserve(nextCapacity);
								valuesReservation.set(checkedSizeAdd(nextValueBytes,
									checkedSizeMul(values.capacity(), sizeof(PropertyValue),
										"logical-plan Pattern comprehension capacity overflowed"),
									"logical-plan Pattern comprehension result size overflowed"));
							}
							values.push_back(std::move(mapped));
							valuesBytes = nextValueBytes;
							return true;
						};
						if (target != nullptr && !comprehension.getTargetVar().empty()) {
							if (const auto bound = row.values.find(comprehension.getTargetVar());
								bound != row.values.end() && nodeBindingId(bound->second, state) != target->node_id) {
								return true;
							}
							ScopedRowBinding targetBinding(row, comprehension.getTargetVar(), *target, state);
							if (edge != nullptr && !comprehension.getRelationshipVar().empty()) {
								ScopedRowBinding relationshipBinding(row, comprehension.getRelationshipVar(), *edge,
																	 state);
								return appendBoundValue();
							}
							return appendBoundValue();
						}
						if (edge != nullptr && !comprehension.getRelationshipVar().empty()) {
							ScopedRowBinding relationshipBinding(row, comprehension.getRelationshipVar(), *edge, state);
							return appendBoundValue();
						}
						return appendBoundValue();
					};
					const bool hasRelationship = comprehension.getPattern().find('-') != std::string::npos;
					const auto finishValues = [&]() {
						PropertyValue result(std::move(values));
						// Confirm that the incremental charge matches the returned value.
						if (property_utils::getPropertyValueSize(result) > state.shared.bounds.maximum_materialized_bytes) {
							throw std::runtime_error(
									"logical-plan Pattern comprehension exceeded its materialized byte bound");
						}
						return result;
					};
					if (!hasRelationship) {
						static_cast<void>(appendValue(nullptr, nullptr));
						return finishValues();
					}
					const std::optional<PatternBindingReadShape> patternShape = inspectPatternBindingReadShape(
							comprehension.getWhereExpression(), comprehension.getMapExpression(),
							comprehension.getTargetVar(), comprehension.getRelationshipVar(), state.shared.snapshot);
					static_cast<void>(visitPatternTargets(
							sourceNode, comprehension.getRelType(), comprehension.getTargetLabel(),
							comprehension.getDirection(), state,
							[&](const storage::LogicalGraphEdge &edge, const storage::LogicalGraphNode &target) {
								return appendValue(&edge, &target);
							},
							patternShape.has_value() ? &*patternShape : nullptr));
					return finishValues();
				}
				case ExpressionType::SHORTEST_PATH: {
					const auto &shortest = dynamic_cast<const expressions::ShortestPathExpression &>(expression);
					if (shortest.getStartVar().empty() || shortest.getEndVar().empty() || shortest.getMinHops() < 0 ||
						shortest.getMaxHops() < shortest.getMinHops()) {
						throw std::invalid_argument("logical-plan shortestPath has an invalid pattern or hop range");
					}
					if (shortest.getDirection() != expressions::PatternDirection::PAT_OUTGOING &&
						shortest.getDirection() != expressions::PatternDirection::PAT_INCOMING &&
						shortest.getDirection() != expressions::PatternDirection::PAT_BOTH) {
						throw std::invalid_argument("logical-plan shortestPath has an unknown direction");
					}
					const auto startBinding = row.values.find(shortest.getStartVar());
					const auto endBinding = row.values.find(shortest.getEndVar());
					if (startBinding == row.values.end() || endBinding == row.values.end()) {
						return PropertyValue();
					}
					// Shortest-path expansion is topology-only until a path is emitted. Resolve
					// stable logical IDs directly so a disconnected pair does not decode either
					// endpoint's labels or Properties merely to seed the BFS. Authenticate both
					// deferred endpoints with one sorted probe when the backend supports it;
					// readers without that capability retain the complete-read fallback.
					const uint64_t startNodeId = nodeBindingId(startBinding->second, state);
					const uint64_t endNodeId = nodeBindingId(endBinding->second, state);
					if (state.shared.snapshot.supportsNodeExistenceProbes()) {
						std::vector<uint64_t> endpointIds;
						if (std::holds_alternative<storage::EntityRef>(startBinding->second)) {
							endpointIds.push_back(startNodeId);
						}
						if (std::holds_alternative<storage::EntityRef>(endBinding->second)) {
							endpointIds.push_back(endNodeId);
						}
						std::ranges::sort(endpointIds);
						endpointIds.erase(std::unique(endpointIds.begin(), endpointIds.end()), endpointIds.end());
						if (!endpointIds.empty()) {
							const std::vector<bool> present = state.shared.snapshot.containsNodesSorted(endpointIds);
							if (present.size() != endpointIds.size()) {
								throw std::runtime_error(
										"logical graph shortestPath endpoint probe cardinality changed");
							}
							for (size_t index = 0U; index < endpointIds.size(); ++index) {
								if (!present[index]) {
									throw std::runtime_error("logical graph shortestPath endpoint Node does not exist");
								}
							}
						}
					} else {
						if (std::holds_alternative<storage::EntityRef>(startBinding->second)) {
							validateNodeIdentity(state, startNodeId);
						}
						if (std::holds_alternative<storage::EntityRef>(endBinding->second)) {
							validateNodeIdentity(state, endNodeId);
						}
					}
					int maximumHops = shortest.getMaxHops();
					if (state.shared.context != nullptr && state.shared.context->maxVarLengthDepth.has_value()) {
						if (*state.shared.context->maxVarLengthDepth < 0) {
							throw std::invalid_argument("logical-plan shortestPath has a negative query depth bound");
						}
						maximumHops = std::min(maximumHops, *state.shared.context->maxVarLengthDepth);
					}
					if (maximumHops < shortest.getMinHops()) {
						return PropertyValue();
					}
					if (static_cast<uint64_t>(maximumHops) > state.shared.bounds.maximum_node_records) {
						throw std::runtime_error("logical-plan shortestPath exceeds its Node-depth bound");
					}

					struct Parent {
						uint64_t node_id = 0U;
						storage::LogicalGraphEdgeTopology edge;
					};
					// The lease outlives the BFS containers it represents.
					MaterializedByteReservation shortestPathStateBytes(&state.shared.materialized_budget);
					std::unordered_map<uint64_t, int> distances;
					std::unordered_map<uint64_t, std::vector<Parent>> parents;
					std::deque<uint64_t> frontier;
					shortestPathStateBytes.set(sizeof(uint64_t) + sizeof(int) + sizeof(uint64_t));
					distances.emplace(startNodeId, 0);
					frontier.push_back(startNodeId);
					size_t parentEntries = 0U;
					std::optional<int> targetDistance = startNodeId == endNodeId ? std::optional<int>{0} : std::nullopt;
					size_t exploredEdges = 0U;
					const size_t maximumExploredEdges = [&]() {
						if (state.shared.bounds.maximum_node_records >
							std::numeric_limits<size_t>::max() / state.shared.bounds.maximum_edges_per_node) {
							throw std::overflow_error("logical-plan shortestPath Edge budget overflowed");
						}
						return state.shared.bounds.maximum_node_records * state.shared.bounds.maximum_edges_per_node;
					}();
					const auto chargeShortestPathState = [&](size_t bytes) {
						const size_t remaining =
								state.shared.bounds.maximum_materialized_bytes -
								std::min(shortestPathStateBytes.bytes(), state.shared.bounds.maximum_materialized_bytes);
						if (bytes > remaining) {
							throw std::runtime_error(
									"logical-plan shortestPath exceeded its materialized state byte bound");
						}
						shortestPathStateBytes.set(checkedSizeAdd(shortestPathStateBytes.bytes(), bytes,
																  "logical-plan shortestPath state size overflowed"));
					};
					const bool supportsExistenceProbes = state.shared.snapshot.supportsNodeExistenceProbes();
					std::optional<uint64_t> relationshipTypeToken;
					if (!shortest.getRelType().empty() &&
						state.shared.snapshot.supportsNativeRelationshipTypeTokens(shortest.getRelType())) {
						relationshipTypeToken = state.shared.snapshot.findRelationshipTypeToken(shortest.getRelType());
						if (!relationshipTypeToken.has_value()) {
							return PropertyValue();
						}
					}
					const bool useNativeAdjacencyRefs =
							state.shared.snapshot.supportsNativeAdjacentEdgeRefScans() &&
							(shortest.getRelType().empty() || relationshipTypeToken.has_value());
					const auto visitAdjacent = [&](uint64_t nodeId, const auto &onEdge) {
						size_t sourceEdges = 0U;
						const auto visitDirection = [&](storage::LogicalGraphReadDirection adjacencyDirection) {
							std::optional<storage::LogicalGraphAdjacencyContinuation> continuation;
							uint64_t previousEdgeId = 0U;
							for (;;) {
								if (exploredEdges == maximumExploredEdges ||
									sourceEdges == state.shared.bounds.maximum_edges_per_node) {
									throw std::runtime_error("logical-plan shortestPath exceeded its total Edge bound");
								}
								const size_t perNodeRemaining = state.shared.bounds.maximum_edges_per_node - sourceEdges;
								const size_t pageRecords =
										std::min(perNodeRemaining, state.shared.bounds.maximum_adjacency_page_records);
								MaterializedByteReservation adjacencyPageBytes(&state.shared.materialized_budget);
								std::vector<storage::LogicalGraphEdgeTopology> pageEdges;
								std::optional<storage::LogicalGraphAdjacencyContinuation> pageContinuation;
								if (useNativeAdjacencyRefs) {
									MaterializedByteReservation referenceBytes(&state.shared.materialized_budget);
									const auto page = readReferencePageBudgeted(state, nodeId, adjacencyDirection, continuation, pageRecords, referenceBytes);
									if (page.edges.size() > pageRecords ||
										(page.edges.empty() && page.continuation.has_value())) {
										throw std::runtime_error("logical graph backend returned an invalid "
																 "shortestPath adjacency reference page");
									}
									if (continuation.has_value() &&
										(page.edges.empty() ||
										 page.edges.front().entity_id != continuation->next_edge_id)) {
										throw std::runtime_error("logical graph backend returned a discontinuous "
																 "shortestPath adjacency reference page");
									}
									// Reference source buffers have their own reservation. Charge only
									// the projected topology here, including every retained type capacity.
									const size_t typeCapacity = relationshipTypeToken.has_value()
										? std::max(std::string{}.capacity(), shortest.getRelType().size()) : std::string{}.capacity();
									size_t typeBytes = 0U;
									for (const auto &reference: page.edges) {
										typeBytes = checkedSizeAdd(typeBytes,
											relationshipTypeToken && reference.type_token == *relationshipTypeToken
												? typeCapacity : std::string{}.capacity(),
											"logical-plan shortestPath projected type capacity overflowed");
									}
									const auto projectedBytes = [&](size_t capacity, size_t names) {
										return checkedSizeAdd(checkedSizeMul(capacity, sizeof(storage::LogicalGraphEdgeTopology),
											"logical-plan shortestPath projected capacity overflowed"), names,
											"logical-plan shortestPath projected names overflowed");
									};
									adjacencyPageBytes.set(projectedBytes(page.edges.size(), typeBytes));
									pageEdges.reserve(page.edges.size());
									adjacencyPageBytes.set(projectedBytes(pageEdges.capacity(), typeBytes));
									size_t actualTypeBytes = 0U;
									size_t remainingTypeBytes = typeBytes;
									for (const storage::EntityRef &reference: page.edges) {
										validateEdgeRef(reference, "logical graph backend returned an invalid or Node "
																   "reference from a shortestPath adjacency page");
										const bool typeMatches = !relationshipTypeToken.has_value() ||
																 reference.type_token == *relationshipTypeToken;
										pageEdges.push_back({reference.entity_id, reference.source_node_id,
															 reference.target_node_id,
															 typeMatches && relationshipTypeToken.has_value()
																	 ? shortest.getRelType()
																	 : std::string{}});
										actualTypeBytes = checkedSizeAdd(actualTypeBytes, pageEdges.back().type.capacity(),
											"logical-plan shortestPath actual type capacity overflowed");
										remainingTypeBytes -= typeMatches && relationshipTypeToken ? typeCapacity : std::string{}.capacity();
										adjacencyPageBytes.set(projectedBytes(pageEdges.capacity(), checkedSizeAdd(actualTypeBytes,
											remainingTypeBytes, "logical-plan shortestPath total type capacity overflowed")));
									}
									pageContinuation = page.continuation;
								} else {
									auto page = readTopologyPageBudgeted(state, nodeId, adjacencyDirection,
										continuation, pageRecords, adjacencyPageBytes);
									if (page.edges.size() > pageRecords ||
										(page.edges.empty() && page.continuation.has_value())) {
										throw std::runtime_error("logical graph backend returned an invalid "
																 "shortestPath adjacency page");
									}
									if (continuation.has_value() &&
										(page.edges.empty() ||
										 page.edges.front().edge_id != continuation->next_edge_id)) {
										throw std::runtime_error("logical graph backend returned a discontinuous "
																 "shortestPath adjacency page");
									}

									pageEdges = std::move(page.edges);
									pageContinuation = page.continuation;
								}
								if (pageEdges.size() > pageRecords ||
									(pageEdges.empty() && pageContinuation.has_value())) {
									throw std::runtime_error(
											"logical graph backend returned an invalid shortestPath adjacency page");
								}
								if (continuation.has_value() &&
									(pageEdges.empty() || pageEdges.front().edge_id != continuation->next_edge_id)) {
									throw std::runtime_error("logical graph backend returned a discontinuous "
															 "shortestPath adjacency page");
								}
								// Endpoint validation is independent of edge traversal order. Resolve
								// each distinct endpoint once per page so parallel edges and repeated
								// targets do not repeat an Entity Directory descent.
								std::vector<uint64_t> endpointIds;
								if (supportsExistenceProbes) {
									endpointIds.reserve(pageEdges.size());
									for (const storage::LogicalGraphEdgeTopology &candidate: pageEdges) {
										endpointIds.push_back(
												adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
														? candidate.target_node_id
														: candidate.source_node_id);
									}
									std::ranges::sort(endpointIds);
									endpointIds.erase(std::unique(endpointIds.begin(), endpointIds.end()),
													  endpointIds.end());
								}
								const size_t endpointIdBytes =
										checkedSizeMul(endpointIds.capacity(), sizeof(uint64_t),
													   "logical-plan shortestPath endpoint probe size overflowed");
								// Pre-admit one machine word per result bit. This upper estimate
								// covers packed-vector word rounding before the backend allocates it.
								const size_t expectedPresenceBytes =
										checkedSizeMul(endpointIds.size(), sizeof(uint64_t),
													   "logical-plan shortestPath endpoint probe size overflowed");
								const size_t endpointProbeBytes =
										checkedSizeAdd(endpointIdBytes, expectedPresenceBytes,
													   "logical-plan shortestPath endpoint probe size overflowed");
								adjacencyPageBytes.set(
										checkedSizeAdd(adjacencyPageBytes.bytes(), endpointProbeBytes,
													   "logical-plan shortestPath adjacency page size overflowed"));
								const std::vector<bool> endpointPresent =
										supportsExistenceProbes ? state.shared.snapshot.containsNodesSorted(endpointIds)
																: std::vector<bool>{};
								if (supportsExistenceProbes) {
									const size_t actualPresenceBytes =
											checkedSizeAdd(endpointPresent.capacity(), 7U,
														   "logical-plan shortestPath endpoint probe size overflowed") /
											8U;
									adjacencyPageBytes.set(checkedSizeAdd(
											adjacencyPageBytes.bytes() - expectedPresenceBytes, actualPresenceBytes,
											"logical-plan shortestPath adjacency page size overflowed"));
								}
								for (const storage::LogicalGraphEdgeTopology &edge: pageEdges) {
									checkGuard(state);
									validateEdge(edge);
									validateAdjacentEdgeOrder(edge.edge_id, previousEdgeId, "shortestPath");
									const bool endpointMatches =
											adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
													? edge.source_node_id == nodeId
													: edge.target_node_id == nodeId;
									if (!endpointMatches) {
										throw std::runtime_error(
												"logical graph backend returned an invalid shortestPath Edge");
									}
									incrementBounded(exploredEdges,
													 "logical-plan shortestPath Edge counter overflowed");
									incrementBounded(sourceEdges,
													 "logical-plan shortestPath per-Node Edge counter overflowed");
									incrementBounded(state.task.stats.edges_scanned,
													 "logical-plan shortestPath runtime Edge counter overflowed");
									if (shortest.getDirection() == expressions::PatternDirection::PAT_BOTH &&
										adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_INCOMING &&
										edge.source_node_id == nodeId) {
										continue;
									}
									if (!shortest.getRelType().empty() && edge.type != shortest.getRelType()) {
										continue;
									}
									const uint64_t next =
											adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
													? edge.target_node_id
													: edge.source_node_id;
									// A native probe authenticates the endpoint in the same pinned
									// snapshot without materializing labels or Properties for every
									// BFS frontier candidate. Readers without that stronger contract
									// authenticate identity with a shaped point read.
									if (supportsExistenceProbes) {
										const auto endpoint = std::ranges::lower_bound(endpointIds, next);
										if (endpoint == endpointIds.end() || *endpoint != next ||
											!endpointPresent[static_cast<size_t>(endpoint - endpointIds.begin())]) {
											throw std::runtime_error("logical graph backend returned a shortestPath "
																	 "Edge with a missing endpoint");
										}
									} else {
										validateNodeIdentity(state, next);
									}
									if (!onEdge(edge, next)) {
										return false;
									}
								}
								if (!pageContinuation.has_value()) {
									return true;
								}
								if (sourceEdges == state.shared.bounds.maximum_edges_per_node ||
									pageContinuation->previous_edge_id != pageEdges.back().edge_id ||
									pageContinuation->next_edge_id == 0U ||
									pageContinuation->previous_edge_id == pageContinuation->next_edge_id) {
									throw std::runtime_error(
											"logical graph backend returned an invalid shortestPath continuation");
								}
								continuation = pageContinuation;
							}
						};
						if ((shortest.getDirection() == expressions::PatternDirection::PAT_OUTGOING ||
							 shortest.getDirection() == expressions::PatternDirection::PAT_BOTH) &&
							!visitDirection(storage::LogicalGraphReadDirection::LGRD_OUTGOING)) {
							return false;
						}
						if (shortest.getDirection() == expressions::PatternDirection::PAT_INCOMING ||
							shortest.getDirection() == expressions::PatternDirection::PAT_BOTH) {
							return visitDirection(storage::LogicalGraphReadDirection::LGRD_INCOMING);
						}
						return true;
					};

					while (!frontier.empty()) {
						checkGuard(state);
						const uint64_t current = frontier.front();
						frontier.pop_front();
						const int currentDistance = distances.at(current);
						if (targetDistance.has_value() && currentDistance >= *targetDistance) {
							continue;
						}
						if (currentDistance == maximumHops) {
							continue;
						}
						static_cast<void>(visitAdjacent(current, [&](const storage::LogicalGraphEdgeTopology &edge,
																	 uint64_t next) {
							const int nextDistance = currentDistance + 1;
							if (nextDistance > maximumHops) {
								return true;
							}
							// A target reached before the requested lower hop bound cannot
							// contribute to a simple path. Do not let that too-short route
							// suppress a later valid route to the same target.
							if (next == endNodeId && nextDistance < shortest.getMinHops()) {
								return true;
							}
							const auto existing = distances.find(next);
							if (existing == distances.end()) {
								if (distances.size() == state.shared.bounds.maximum_node_records) {
									throw std::runtime_error(
											"logical-plan shortestPath exceeded its frontier Node bound");
								}
								chargeShortestPathState(sizeof(uint64_t) + sizeof(int) + sizeof(void *) +
														sizeof(uint64_t));
								distances.emplace(next, nextDistance);
								frontier.push_back(next);
								if (parentEntries == state.shared.bounds.maximum_rows) {
									throw std::runtime_error("logical-plan shortestPath exceeded its parent-DAG bound");
								}
								chargeShortestPathState(
										checkedSizeAdd(sizeof(Parent), edge.type.size(),
													   "logical-plan shortestPath parent-DAG size overflowed"));
								parents[next].push_back({current, edge});
								incrementBounded(parentEntries,
												 "logical-plan shortestPath parent-DAG counter overflowed");
							} else if (existing->second == nextDistance) {
								if (parentEntries == state.shared.bounds.maximum_rows) {
									throw std::runtime_error("logical-plan shortestPath exceeded its parent-DAG bound");
								}
								chargeShortestPathState(
										checkedSizeAdd(sizeof(Parent), edge.type.size(),
													   "logical-plan shortestPath parent-DAG size overflowed"));
								parents[next].push_back({current, edge});
								incrementBounded(parentEntries,
												 "logical-plan shortestPath parent-DAG counter overflowed");
							}
							if (next == endNodeId) {
								targetDistance = nextDistance;
							}
							return true;
						}));
					}
					if (!targetDistance.has_value() || *targetDistance < shortest.getMinHops()) {
						return PropertyValue();
					}
					for (auto &[_, entries]: parents) {
						std::ranges::sort(entries, {}, [](const Parent &parent) {
							return std::pair{parent.node_id, parent.edge.edge_id};
						});
					}

					if (shortestMode != ShortestPathResultMode::SPR_FULL) {
						if (!shortest.isAll()) {
							const size_t elements =
									checkedSizeAdd(checkedSizeMul(static_cast<size_t>(*targetDistance), size_t{2U},
																  "logical-plan shortestPath size overflowed"),
												   size_t{1U}, "logical-plan shortestPath size overflowed");
							if (elements > state.shared.bounds.maximum_rows) {
								throw std::runtime_error("logical-plan scalar function received a List exceeding the "
														 "configured List bound");
							}
							if (elements > static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
								throw std::overflow_error(
										"logical-plan shortestPath size exceeds integer result range");
							}
							if (shortestMode == ShortestPathResultMode::SPR_LENGTH) {
								const auto markerMatches = [&](uint64_t id, bool edge) {
									checkGuard(state);
									const auto present = edge ? state.shared.snapshot.edgePropertyExists(id, "_type")
															  : state.shared.snapshot.nodePropertyExists(id, "_type");
									if (present.has_value() && !*present)
										return true;
									PropertyValue marker;
									if (present.has_value()) {
										marker = readSelectedPropertyBudgeted(state.shared.snapshot, id, edge, "_type", state);
									} else {
										// Unsupported presence probes retain the complete reader
										// fallback and distinguish a missing key from explicit NULL.
										const auto entries = edge ? readEdgeCached(state, id).properties
																  : readNodeCached(state, id).properties;
										const auto field = std::ranges::find(entries, "_type",
																			 &storage::LogicalGraphPropertyEntry::key);
										if (field == entries.end())
											return true;
										marker = field->value;
									}
									if (property_utils::getPropertyValueSize(marker) >
										state.shared.bounds.maximum_materialized_bytes) {
										throw std::runtime_error(
												"logical-plan shortestPath marker exceeded its materialized bound");
									}
									return marker.getType() == PropertyType::STRING &&
										   std::get<std::string>(marker.getVariant()) ==
												   (edge ? "relationship" : "node");
								};
								uint64_t current = endNodeId;
								for (;;) {
									if (!markerMatches(current, false))
										return PropertyValue(static_cast<int64_t>(elements));
									if (current == startNodeId)
										break;
									const auto previous = parents.find(current);
									if (previous == parents.end() || previous->second.empty()) {
										throw std::runtime_error(
												"logical-plan shortestPath has an incomplete parent DAG");
									}
									const Parent &parent = previous->second.front();
									if (!markerMatches(parent.edge.edge_id, true))
										return PropertyValue(static_cast<int64_t>(elements));
									current = parent.node_id;
								}
								return PropertyValue(static_cast<int64_t>(*targetDistance));
							}
							return PropertyValue(static_cast<int64_t>(elements));
						}
						// Count paths in the parent DAG without constructing path lists or
						// entity maps. Parallel edges retain their path multiplicity.
						std::unordered_map<uint64_t, size_t> pathCounts;
						std::function<size_t(uint64_t)> countPaths = [&](uint64_t id) -> size_t {
							checkGuard(state);
							if (id == startNodeId)
								return 1U;
							if (const auto cached = pathCounts.find(id); cached != pathCounts.end())
								return cached->second;
							const auto previous = parents.find(id);
							if (previous == parents.end() || previous->second.empty()) {
								throw std::runtime_error("logical-plan shortestPath has an incomplete parent DAG");
							}
							size_t count = 0U;
							for (const Parent &parent: previous->second) {
								const size_t contribution = countPaths(parent.node_id);
								if (contribution > state.shared.bounds.maximum_rows - count) {
									throw std::runtime_error(
											"logical-plan shortestPath exceeded its result-path bound");
								}
								count += contribution;
							}
							chargeShortestPathState(sizeof(std::pair<const uint64_t, size_t>));
							pathCounts.emplace(id, count);
							return count;
						};
						const size_t count = countPaths(endNodeId);
						if (count > static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
							throw std::overflow_error("logical-plan shortestPath count exceeds integer result range");
						}
						return PropertyValue(static_cast<int64_t>(count));
					}

					const auto idValue = [](uint64_t id) {
						if (id > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
							throw std::overflow_error(
									"logical-plan shortestPath cannot represent an entity ID in a Property value");
						}
						return PropertyValue(static_cast<int64_t>(id));
					};
					const auto materializePath = [&](const std::vector<uint64_t> &nodeIds,
													 const std::vector<storage::LogicalGraphEdgeTopology> &edges,
													 MaterializedByteReservation &outputBytes) {
						const size_t pathElements = checkedSizeAdd(nodeIds.size(), edges.size(),
																   "logical-plan shortestPath result size overflowed");
						outputBytes.set(
								checkedSizeAdd(sizeof(uint32_t),
											   checkedSizeMul(pathElements, sizeof(PropertyValue),
															  "logical-plan shortestPath result size overflowed"),
											   "logical-plan shortestPath result size overflowed"));
						std::vector<PropertyValue> path;
						path.reserve(pathElements);
						const auto appendEntity = [&](const std::vector<storage::LogicalGraphPropertyEntry> &entries,
													  std::string_view type, uint64_t id) {
							if (entries.size() > state.shared.bounds.maximum_property_entries) {
								throw std::runtime_error(
										"logical graph backend returned too many direct Property entries");
							}
							incrementBounded(state.task.stats.property_sets_materialized,
											 "logical-plan Property materialization counter overflowed");
							MaterializedByteReservation keyBytes(&state.shared.materialized_budget);
							keyBytes.set(checkedSizeMul(entries.size(), sizeof(std::string_view),
														"logical-plan shortestPath Property key size overflowed"));
							std::vector<std::string_view> keys;
							keys.reserve(entries.size());
							size_t mapBytes = sizeof(uint32_t);
							bool hasType = false;
							bool hasId = false;
							for (const auto &entry: entries) {
								checkGuard(state);
								if (entry.key.empty()) {
									throw std::runtime_error(
											"logical graph backend returned an invalid direct Property entry set");
								}
								keys.push_back(entry.key);
								hasType |= entry.key == "_type";
								hasId |= entry.key == "_id";
								mapBytes = checkedSizeAdd(
										mapBytes,
										checkedSizeAdd(
												checkedSizeAdd(entry.key.size(), sizeof(size_t),
															   "logical-plan shortestPath result size overflowed"),
												property_utils::getPropertyValueSize(entry.value),
												"logical-plan shortestPath result size overflowed"),
										"logical-plan shortestPath result size overflowed");
							}
							std::ranges::sort(keys);
							if (std::adjacent_find(keys.begin(), keys.end()) != keys.end()) {
								throw std::runtime_error(
										"logical graph backend returned an invalid direct Property entry set");
							}
							std::vector<std::string_view>().swap(keys);
							keyBytes.reset();
							if (!hasType) {
								mapBytes = checkedSizeAdd(mapBytes, sizeof(size_t) + 5U + sizeof(size_t) + type.size(),
														  "logical-plan shortestPath result size overflowed");
							}
							if (!hasId) {
								mapBytes = checkedSizeAdd(mapBytes, sizeof(size_t) + 3U + sizeof(int64_t),
														  "logical-plan shortestPath result size overflowed");
							}
							outputBytes.set(checkedSizeAdd(outputBytes.bytes(), mapBytes,
														   "logical-plan shortestPath result size overflowed"));
							PropertyValue::MapType entityMap;
							entityMap.reserve(checkedSizeAdd(entries.size(), 2U,
															 "logical-plan shortestPath Property count overflowed"));
							entityMap.emplace("_type", PropertyValue(std::string(type)));
							entityMap.emplace("_id", idValue(id));
							for (const auto &entry: entries) {
								entityMap.insert_or_assign(entry.key, entry.value);
							}
							path.emplace_back(std::move(entityMap));
						};
						MaterializedByteReservation orderedNodeBytes(&state.shared.materialized_budget);
						orderedNodeBytes.set(
								checkedSizeMul(nodeIds.size(), sizeof(uint64_t),
											   "logical-plan shortestPath ordered Node ID size overflowed"));
						std::vector<uint64_t> orderedNodeIds = nodeIds;
						std::ranges::sort(orderedNodeIds);
						orderedNodeIds.erase(std::unique(orderedNodeIds.begin(), orderedNodeIds.end()),
											 orderedNodeIds.end());
						MaterializedByteReservation nodeRecordBytes(&state.shared.materialized_budget);
						nodeRecordBytes.set(checkedSizeMul(orderedNodeIds.size(), sizeof(storage::LogicalGraphNode),
														   "logical-plan shortestPath retained Node size overflowed"));
						std::vector<storage::LogicalGraphNode> nodeRecords;
						nodeRecords.reserve(orderedNodeIds.size());
						nodeRecordBytes.set(checkedSizeMul(nodeRecords.capacity(), sizeof(storage::LogicalGraphNode),
														   "logical-plan shortestPath retained Node size overflowed"));
						for (size_t offset = 0U; offset < orderedNodeIds.size();
							 offset += DEFAULT_PROPERTY_BATCH_ROWS) {
							const size_t count = std::min(DEFAULT_PROPERTY_BATCH_ROWS, orderedNodeIds.size() - offset);
							MaterializedByteReservation sourceBytes(&state.shared.materialized_budget);
							auto batch = readNodesShapedSortedBudgeted(
									state, std::span<const uint64_t>(orderedNodeIds).subspan(offset, count),
									completeNodeReadShape(), &sourceBytes);
							if (batch.size() != count) {
								throw std::runtime_error(
										"logical-plan shortestPath Node batch changed its cardinality");
							}
							for (size_t index = 0U; index < count; ++index) {
								validateNode(batch[index], state);
								if (batch[index].node_id != orderedNodeIds[offset + index]) {
									throw std::runtime_error(
											"logical-plan shortestPath Node batch changed logical ID order");
								}
							}
							retainShapedBatchBytes(batch, sourceBytes, state.shared.context);
							for (auto &node: batch) {
								const size_t heapBytes = logicalNodeCacheBytes(node, state.shared.context) - sizeof(node);
								sourceBytes.set(sourceBytes.bytes() - heapBytes);
								nodeRecordBytes.set(
										checkedSizeAdd(nodeRecordBytes.bytes(), heapBytes,
													   "logical-plan shortestPath retained Node size overflowed"));
								nodeRecords.push_back(std::move(node));
							}
						}
						std::vector<uint64_t>().swap(orderedNodeIds);
						orderedNodeBytes.reset();
						std::vector<uint64_t> orderedEdgeIds;
						MaterializedByteReservation orderedEdgeBytes(&state.shared.materialized_budget);
						orderedEdgeBytes.set(
								checkedSizeMul(edges.size(), sizeof(uint64_t),
											   "logical-plan shortestPath ordered Edge ID size overflowed"));
						orderedEdgeIds.reserve(edges.size());
						for (const auto &edge: edges) {
							orderedEdgeIds.push_back(edge.edge_id);
						}
						std::ranges::sort(orderedEdgeIds);
						orderedEdgeIds.erase(std::unique(orderedEdgeIds.begin(), orderedEdgeIds.end()),
											 orderedEdgeIds.end());
						MaterializedByteReservation edgeRecordBytes(&state.shared.materialized_budget);
						edgeRecordBytes.set(checkedSizeMul(orderedEdgeIds.size(), sizeof(storage::LogicalGraphEdge),
														   "logical-plan shortestPath retained Edge size overflowed"));
						std::vector<storage::LogicalGraphEdge> edgeRecords;
						edgeRecords.reserve(orderedEdgeIds.size());
						edgeRecordBytes.set(checkedSizeMul(edgeRecords.capacity(), sizeof(storage::LogicalGraphEdge),
														   "logical-plan shortestPath retained Edge size overflowed"));
						for (size_t offset = 0U; offset < orderedEdgeIds.size();
							 offset += DEFAULT_PROPERTY_BATCH_ROWS) {
							const size_t count = std::min(DEFAULT_PROPERTY_BATCH_ROWS, orderedEdgeIds.size() - offset);
							MaterializedByteReservation sourceBytes(&state.shared.materialized_budget);
							auto batch = readEdgesShapedSortedBudgeted(
									state, std::span<const uint64_t>(orderedEdgeIds).subspan(offset, count),
									completeEdgeReadShape(), &sourceBytes);
							if (batch.size() != count) {
								throw std::runtime_error(
										"logical-plan shortestPath Edge batch changed its cardinality");
							}
							for (size_t index = 0U; index < count; ++index) {
								validateEdge(batch[index]);
								if (batch[index].edge_id != orderedEdgeIds[offset + index]) {
									throw std::runtime_error(
											"logical-plan shortestPath Edge batch changed logical ID order");
								}
							}
							retainShapedBatchBytes(batch, sourceBytes, state.shared.context);
							for (auto &edge: batch) {
								const size_t heapBytes = logicalEdgeCacheBytes(edge, state.shared.context) - sizeof(edge);
								sourceBytes.set(sourceBytes.bytes() - heapBytes);
								edgeRecordBytes.set(
										checkedSizeAdd(edgeRecordBytes.bytes(), heapBytes,
													   "logical-plan shortestPath retained Edge size overflowed"));
								edgeRecords.push_back(std::move(edge));
							}
						}
						std::vector<uint64_t>().swap(orderedEdgeIds);
						orderedEdgeBytes.reset();
						for (size_t index = 0U; index < nodeIds.size(); ++index) {
							const auto nodeIt = std::ranges::lower_bound(nodeRecords, nodeIds[index], {},
																		 &storage::LogicalGraphNode::node_id);
							if (nodeIt == nodeRecords.end() || nodeIt->node_id != nodeIds[index]) {
								throw std::runtime_error("logical-plan shortestPath Node batch lost a path Node");
							}
							const storage::LogicalGraphNode &node = *nodeIt;
							validateNode(node, state);
							appendEntity(node.properties, "node", node.node_id);
							if (index == edges.size()) {
								continue;
							}
							const storage::LogicalGraphEdgeTopology &topology = edges[index];
							const auto edgeIt = std::ranges::lower_bound(edgeRecords, topology.edge_id, {},
																		 &storage::LogicalGraphEdge::edge_id);
							if (edgeIt == edgeRecords.end() || edgeIt->edge_id != topology.edge_id) {
								throw std::runtime_error("logical-plan shortestPath Edge batch lost a path Edge");
							}
							const storage::LogicalGraphEdge &edge = *edgeIt;
							validateEdge(edge);
							// A native untyped adjacency reference carries no relationship name.
							// Its empty topology type is therefore an unknown marker, not a claim
							// that the persisted Edge type is empty.
							if (edge.source_node_id != topology.source_node_id ||
								edge.target_node_id != topology.target_node_id ||
								(!topology.type.empty() && edge.type != topology.type)) {
								throw std::runtime_error("logical graph backend changed an Edge while materializing a "
														 "pinned shortestPath result");
							}
							appendEntity(edge.properties, "relationship", edge.edge_id);
						}
						return PropertyValue(std::move(path));
					};

					MaterializedByteReservation allPathReservation(&state.shared.materialized_budget);
					std::vector<PropertyValue> allPaths;
					size_t allPathBytes = 0U;
					MaterializedByteReservation reversedPathBytes(&state.shared.materialized_budget);
					reversedPathBytes.set(sizeof(uint64_t));
					std::vector<uint64_t> reversedNodes;
					reversedNodes.reserve(1U);
					reversedNodes.push_back(endNodeId);
					std::vector<storage::LogicalGraphEdgeTopology> reversedEdges;
					size_t reversedTypeBytes = 0U;
					const auto reversedStorageBytes = [&](size_t nodeCapacity, size_t edgeCapacity, size_t typeBytes) {
						return checkedSizeAdd(
								checkedSizeAdd(checkedSizeMul(nodeCapacity, sizeof(uint64_t),
															  "logical-plan shortestPath reverse Node size overflowed"),
											   checkedSizeMul(edgeCapacity, sizeof(storage::LogicalGraphEdgeTopology),
															  "logical-plan shortestPath reverse Edge size overflowed"),
											   "logical-plan shortestPath reverse path size overflowed"),
								typeBytes, "logical-plan shortestPath reverse path size overflowed");
					};
					std::function<void(uint64_t)> collectPaths = [&](uint64_t nodeId) {
						if (allPaths.size() == state.shared.bounds.maximum_rows) {
							throw std::runtime_error("logical-plan shortestPath exceeded its result-path bound");
						}
						if (nodeId == startNodeId) {
							MaterializedByteReservation forwardPathBytes(&state.shared.materialized_budget);
							size_t forwardBytes = checkedSizeAdd(
									checkedSizeMul(reversedNodes.size(), sizeof(uint64_t),
												   "logical-plan shortestPath forward Node size overflowed"),
									checkedSizeMul(reversedEdges.size(), sizeof(storage::LogicalGraphEdgeTopology),
												   "logical-plan shortestPath forward Edge size overflowed"),
									"logical-plan shortestPath forward path size overflowed");
							for (const auto &edge: reversedEdges) {
								forwardBytes = checkedSizeAdd(forwardBytes, edge.type.size(),
															  "logical-plan shortestPath forward path size overflowed");
							}
							forwardPathBytes.set(forwardBytes);
							std::vector<uint64_t> nodes(reversedNodes.rbegin(), reversedNodes.rend());
							std::vector<storage::LogicalGraphEdgeTopology> edges(reversedEdges.rbegin(),
																				 reversedEdges.rend());
							MaterializedByteReservation pendingPathBytes(&state.shared.materialized_budget);
							PropertyValue path = materializePath(nodes, edges, pendingPathBytes);
							const size_t pathBytes = property_utils::getPropertyValueSize(path);
							if (pathBytes > state.shared.bounds.maximum_materialized_bytes ||
								allPathBytes > state.shared.bounds.maximum_materialized_bytes - pathBytes) {
								throw std::runtime_error(
										"logical-plan shortestPath exceeded its materialized byte bound");
							}
							const size_t nextPathBytes = checkedSizeAdd(
									allPathBytes, pathBytes, "logical-plan shortestPath result size overflowed");
							const size_t nextCapacity =
									allPaths.size() == allPaths.capacity()
											? std::min(state.shared.bounds.maximum_rows,
													   std::max(allPaths.size() + 1U,
																checkedSizeMul(allPaths.capacity(), 2U,
																			   "logical-plan shortestPath result "
																			   "capacity overflowed")))
											: allPaths.capacity();
							const size_t retainedBytes = checkedSizeAdd(
									nextPathBytes,
									checkedSizeMul(nextCapacity, sizeof(PropertyValue),
												   "logical-plan shortestPath result capacity overflowed"),
									"logical-plan shortestPath result size overflowed");
							// The completed-path owner takes over from the construction lease.
							// No allocation occurs between releasing and acquiring these charges.
							pendingPathBytes.reset();
							allPathReservation.set(retainedBytes);
							if (nextCapacity != allPaths.capacity())
								allPaths.reserve(nextCapacity);
							allPaths.push_back(std::move(path));
							allPathBytes = nextPathBytes;
							return;
						}
						const auto previous = parents.find(nodeId);
						if (previous == parents.end() || previous->second.empty()) {
							throw std::runtime_error("logical-plan shortestPath has an incomplete parent DAG");
						}
						for (const Parent &parent: previous->second) {
							const size_t nodeCapacity =
									reversedNodes.size() == reversedNodes.capacity()
											? checkedSizeMul(
													  reversedNodes.capacity(), 2U,
													  "logical-plan shortestPath reverse Node capacity overflowed")
											: reversedNodes.capacity();
							const size_t edgeCapacity =
									reversedEdges.size() == reversedEdges.capacity()
											? std::max(size_t{1U}, checkedSizeMul(reversedEdges.capacity(), 2U,
																				  "logical-plan shortestPath reverse "
																				  "Edge capacity overflowed"))
											: reversedEdges.capacity();
							const size_t nextTypeBytes =
									checkedSizeAdd(reversedTypeBytes, parent.edge.type.size(),
												   "logical-plan shortestPath reverse Edge type size overflowed");
							reversedPathBytes.set(reversedStorageBytes(nodeCapacity, edgeCapacity, nextTypeBytes));
							if (nodeCapacity != reversedNodes.capacity())
								reversedNodes.reserve(nodeCapacity);
							if (edgeCapacity != reversedEdges.capacity())
								reversedEdges.reserve(edgeCapacity);
							reversedNodes.push_back(parent.node_id);
							reversedEdges.push_back(parent.edge);
							reversedTypeBytes = nextTypeBytes;
							collectPaths(parent.node_id);
							reversedEdges.pop_back();
							reversedTypeBytes -= parent.edge.type.size();
							reversedPathBytes.set(reversedStorageBytes(reversedNodes.capacity(),
																	   reversedEdges.capacity(), reversedTypeBytes));
							reversedNodes.pop_back();
							if (!shortest.isAll() && !allPaths.empty()) {
								return;
							}
						}
					};
					if (!shortest.isAll()) {
						uint64_t current = endNodeId;
						while (current != startNodeId) {
							const auto previous = parents.find(current);
							if (previous == parents.end() || previous->second.empty()) {
								throw std::runtime_error("logical-plan shortestPath has an incomplete parent DAG");
							}
							const Parent &parent = previous->second.front();
							const size_t nodeCapacity =
									reversedNodes.size() == reversedNodes.capacity()
											? checkedSizeMul(
													  reversedNodes.capacity(), 2U,
													  "logical-plan shortestPath reverse Node capacity overflowed")
											: reversedNodes.capacity();
							const size_t edgeCapacity =
									reversedEdges.size() == reversedEdges.capacity()
											? std::max(size_t{1U}, checkedSizeMul(reversedEdges.capacity(), 2U,
																				  "logical-plan shortestPath reverse "
																				  "Edge capacity overflowed"))
											: reversedEdges.capacity();
							const size_t nextTypeBytes =
									checkedSizeAdd(reversedTypeBytes, parent.edge.type.size(),
												   "logical-plan shortestPath reverse Edge type size overflowed");
							reversedPathBytes.set(reversedStorageBytes(nodeCapacity, edgeCapacity, nextTypeBytes));
							if (nodeCapacity != reversedNodes.capacity())
								reversedNodes.reserve(nodeCapacity);
							if (edgeCapacity != reversedEdges.capacity())
								reversedEdges.reserve(edgeCapacity);
							reversedNodes.push_back(parent.node_id);
							reversedEdges.push_back(parent.edge);
							reversedTypeBytes = nextTypeBytes;
							current = parent.node_id;
						}
						std::ranges::reverse(reversedNodes);
						std::ranges::reverse(reversedEdges);
						// The parent DAG and BFS frontier are no longer needed once the
						// deterministic first shortest route is captured. Release them before
						// reading complete Node/Edge payloads for the returned path.
						decltype(parents){}.swap(parents);
						decltype(distances){}.swap(distances);
						decltype(frontier){}.swap(frontier);
						shortestPathStateBytes.reset();
						MaterializedByteReservation outputBytes(&state.shared.materialized_budget);
						return materializePath(reversedNodes, reversedEdges, outputBytes);
					}
					collectPaths(endNodeId);
					if (allPaths.empty()) {
						throw std::runtime_error("logical-plan shortestPath failed to materialize a discovered path");
					}
					return shortest.isAll() ? PropertyValue(std::move(allPaths)) : std::move(allPaths.front());
				}
				default:
					throw std::logic_error("logical-plan runtime does not support this expression type");
			}
		}

