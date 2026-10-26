		[[nodiscard]] LogicalPlanReadValue evaluateExpression(const expressions::Expression &expression,
															  LogicalPlanReadRow &row, ExecutionState &state) {
			using expressions::ExpressionType;
			switch (expression.getExpressionType()) {
				case ExpressionType::LITERAL: {
					const auto &literal = dynamic_cast<const expressions::LiteralExpression &>(expression);
					if (literal.isNull()) {
						return PropertyValue();
					}
					if (literal.isBoolean()) {
						return PropertyValue(literal.getBooleanValue());
					}
					if (literal.isInteger()) {
						return PropertyValue(literal.getIntegerValue());
					}
					if (literal.isDouble()) {
						return PropertyValue(literal.getDoubleValue());
					}
					if (literal.isPropertyValue()) {
						detail::ProcedurePayloadAdmissionScope::admit(detail::procedurePayloadHeapUpper<true>(literal.getPropertyValue()));
						return literal.getPropertyValue();
					}
					detail::ProcedurePayloadAdmissionScope::admit(utils::detail::standardFreshStringHeapUpper(literal.getStringValue().size()));
					return PropertyValue(literal.getStringValue());
				}
				case ExpressionType::VARIABLE_REFERENCE:
				case ExpressionType::PROPERTY_ACCESS: {
					const auto &reference = dynamic_cast<const expressions::VariableReferenceExpression &>(expression);
					if (state.task.bound_inputs && state.task.bound_inputs->usesRow(row)) {
						const auto *instruction = state.task.bound_inputs->instruction(expression);
						const auto *value = instruction ? state.task.bound_inputs->value(*instruction) : nullptr;
						if (value && instruction->kind == detail::BoundInputKind::BIK_VALUE) {
							debug::PerfTrace::addValue("logical_plan.bound_expression.slot_reads", 1);
                            detail::ProcedurePayloadAdmissionScope::admit(detail::procedurePayloadHeapUpper<true>(*value)); return *value;
						}
						if (value && instruction->kind == detail::BoundInputKind::BIK_PROPERTY) {
							const auto property = state.task.bound_inputs->property(*instruction, state.shared.snapshot);
							if (property && (property->source != *instruction->source || property->key != reference.getPropertyName()))
								throw std::logic_error("bound property descriptor disagrees with its expression");
							debug::PerfTrace::addValue("logical_plan.bound_expression.property_reads", 1);
							return readProperty(state.shared.snapshot, row, reference.getVariableName(), reference.getPropertyName(), state, value);
						}
					}
					if (reference.hasProperty()) {
						// A WITH/RETURN projection without AS keeps the expression text as
						// its column name (for example `WITH n.value`). Resolve that
						// projected column before interpreting the same text as a property
						// access on a variable that is no longer in scope.
						if (const auto projected = row.values.find(reference.toString());
							projected != row.values.end()) {
							detail::ProcedurePayloadAdmissionScope::admit(detail::procedurePayloadHeapUpper<true>(projected->second));
							return projected->second;
						}
						return readProperty(state.shared.snapshot, row, reference.getVariableName(),
											reference.getPropertyName(), state);
					}
					const auto &binding = requireBinding(row, reference.getVariableName());
                    detail::ProcedurePayloadAdmissionScope::admit(detail::procedurePayloadHeapUpper<true>(binding));
                    return binding;
				}
				case ExpressionType::PARAMETER: {
					if (state.shared.context == nullptr) {
						throw std::runtime_error("logical-plan parameter evaluation requires a QueryContext");
					}
					const auto &parameter = dynamic_cast<const expressions::ParameterExpression &>(expression);
					const PropertyValue *value = nullptr;
					if (state.task.bound_inputs && state.task.bound_inputs->usesRow(row)) {
						const auto *input = state.task.bound_inputs->instruction(expression);
						if (input && input->kind == detail::BoundInputKind::BIK_PARAMETER)
							value = state.task.bound_inputs->parameter(*input, state.shared.context);
					}
					if (!value) {
						const auto found = state.shared.context->parameters.find(parameter.getParameterName());
						if (found != state.shared.context->parameters.end()) value = &found->second;
					}
					if (!value) {
						throw std::runtime_error("logical-plan read runtime could not resolve a query parameter");
					}
					if (value->getType() == PropertyType::STRING || value->getType() == PropertyType::LIST || value->getType() == PropertyType::MAP || value->getType() == PropertyType::BYTES) {
						MaterializedByteReservation copyBytes(&state.shared.materialized_budget);
						return copyReservedPropertyValue(*value, state, copyBytes);
					}
					return *value;
				}
				case ExpressionType::EXPR_LIST_LITERAL: {
					const auto &list = dynamic_cast<const expressions::ListLiteralExpression &>(expression);
					if (list.isStatic()) {
						if (list.getValue().getList().size() > state.shared.bounds.maximum_rows) {
							throw std::runtime_error("logical-plan List literal exceeded the configured List bound");
						}
						MaterializedByteReservation copyBytes(&state.shared.materialized_budget);
						copyBytes.set(materializedListStorageBytes(list.getValue(), state));
						return list.getValue();
					}
					if (list.getElements().size() > state.shared.bounds.maximum_rows) {
						throw std::runtime_error("logical-plan List literal exceeded the configured List bound");
					}
					MaterializedByteReservation valuesReservation(&state.shared.materialized_budget);
					valuesReservation.set(
							checkedSizeAdd(sizeof(uint32_t),
										   checkedSizeMul(list.getElements().size(), sizeof(PropertyValue),
														  "logical-plan List literal capacity overflowed"),
										   "logical-plan List literal size overflowed"));
					std::vector<PropertyValue> values;
					values.reserve(list.getElements().size());
					const size_t valuesCapacityBytes =
							checkedSizeAdd(sizeof(uint32_t),
										   checkedSizeMul(values.capacity(), sizeof(PropertyValue),
														  "logical-plan List literal capacity overflowed"),
										   "logical-plan List literal size overflowed");
					valuesReservation.set(valuesCapacityBytes);
					for (const auto &element: list.getElements()) {
						PropertyValue value = scalarValue(evaluateExpression(*element, row, state));
						valuesReservation.set(checkedSizeAdd(valuesReservation.bytes(),
															 retainedExpressionValueBytes(value, state),
															 "logical-plan List literal size overflowed"));
						values.push_back(std::move(value));
					}
					return PropertyValue(std::move(values));
				}
				default:
					return evaluateScalarExpression(expression, row, state);
			}
		}

		[[nodiscard]] PropertyValue arithmetic(expressions::BinaryOperatorType operation, const PropertyValue &left,
											   const PropertyValue &right) {
			if (isNull(left) || isNull(right)) {
				return PropertyValue();
			}
			const PropertyType leftType = left.getType();
			const PropertyType rightType = right.getType();
			if (operation == expressions::BinaryOperatorType::BOP_ADD && leftType == PropertyType::DATE &&
				rightType == PropertyType::DURATION) {
				return addDurationToDate(std::get<TemporalDate>(left.getVariant()),
										 std::get<TemporalDuration>(right.getVariant()));
			}
			if (operation == expressions::BinaryOperatorType::BOP_ADD && leftType == PropertyType::DURATION &&
				rightType == PropertyType::DATE) {
				return addDurationToDate(std::get<TemporalDate>(right.getVariant()),
										 std::get<TemporalDuration>(left.getVariant()));
			}
			if (operation == expressions::BinaryOperatorType::BOP_ADD && leftType == PropertyType::DATETIME &&
				rightType == PropertyType::DURATION) {
				return addDurationToDateTime(std::get<TemporalDateTime>(left.getVariant()),
											 std::get<TemporalDuration>(right.getVariant()));
			}
			if (operation == expressions::BinaryOperatorType::BOP_ADD && leftType == PropertyType::DURATION &&
				rightType == PropertyType::DATETIME) {
				return addDurationToDateTime(std::get<TemporalDateTime>(right.getVariant()),
											 std::get<TemporalDuration>(left.getVariant()));
			}
			if (leftType == PropertyType::DURATION && rightType == PropertyType::DURATION) {
				const TemporalDuration &leftDuration = std::get<TemporalDuration>(left.getVariant());
				const TemporalDuration &rightDuration = std::get<TemporalDuration>(right.getVariant());
				if (operation == expressions::BinaryOperatorType::BOP_ADD) {
					return PropertyValue(TemporalDuration{checkedAdd(leftDuration.months, rightDuration.months),
														  checkedAdd(leftDuration.days, rightDuration.days),
														  checkedAdd(leftDuration.nanos, rightDuration.nanos)});
				}
				if (operation == expressions::BinaryOperatorType::BOP_SUBTRACT) {
					return PropertyValue(TemporalDuration{checkedSubtract(leftDuration.months, rightDuration.months),
														  checkedSubtract(leftDuration.days, rightDuration.days),
														  checkedSubtract(leftDuration.nanos, rightDuration.nanos)});
				}
			}
			if (operation == expressions::BinaryOperatorType::BOP_SUBTRACT && leftType == PropertyType::DATE &&
				rightType == PropertyType::DURATION) {
				const TemporalDuration &duration = std::get<TemporalDuration>(right.getVariant());
				return addDurationToDate(std::get<TemporalDate>(left.getVariant()),
										 TemporalDuration{checkedSubtract(0LL, duration.months),
														  checkedSubtract(0LL, duration.days),
														  checkedSubtract(0LL, duration.nanos)});
			}
			if (operation == expressions::BinaryOperatorType::BOP_SUBTRACT && leftType == PropertyType::DATETIME &&
				rightType == PropertyType::DURATION) {
				const TemporalDuration &duration = std::get<TemporalDuration>(right.getVariant());
				return addDurationToDateTime(std::get<TemporalDateTime>(left.getVariant()),
											 TemporalDuration{checkedSubtract(0LL, duration.months),
															  checkedSubtract(0LL, duration.days),
															  checkedSubtract(0LL, duration.nanos)});
			}
			if (operation == expressions::BinaryOperatorType::BOP_SUBTRACT && leftType == PropertyType::DATE &&
				rightType == PropertyType::DATE) {
				const TemporalDate &leftDate = std::get<TemporalDate>(left.getVariant());
				const TemporalDate &rightDate = std::get<TemporalDate>(right.getVariant());
				return PropertyValue(
						TemporalDuration{0LL, static_cast<int64_t>(leftDate.epochDays) - rightDate.epochDays, 0LL});
			}
			if (operation == expressions::BinaryOperatorType::BOP_SUBTRACT && leftType == PropertyType::DATETIME &&
				rightType == PropertyType::DATETIME) {
				constexpr int64_t MILLIS_PER_DAY = 86'400LL * 1'000LL;
				const int64_t milliseconds =
						checkedSubtract(std::get<TemporalDateTime>(left.getVariant()).epochMillis,
										std::get<TemporalDateTime>(right.getVariant()).epochMillis);
				return PropertyValue(TemporalDuration{0LL, milliseconds / MILLIS_PER_DAY,
													  (milliseconds % MILLIS_PER_DAY) * 1'000'000LL});
			}
			if (operation == expressions::BinaryOperatorType::BOP_ADD &&
				(leftType == PropertyType::STRING || rightType == PropertyType::STRING)) {
				// Keep the logical path aligned with the established expression evaluator.
				return PropertyValue(expressions::EvaluationContext::toString(left) +
									 expressions::EvaluationContext::toString(right));
			}
			const int64_t *leftInteger = std::get_if<int64_t>(&left.getVariant());
			const int64_t *rightInteger = std::get_if<int64_t>(&right.getVariant());
			const double leftNumber = leftInteger ? static_cast<double>(*leftInteger)
									  : std::get_if<double>(&left.getVariant())
											  ? *std::get_if<double>(&left.getVariant())
											  : std::numeric_limits<double>::quiet_NaN();
			const double rightNumber = rightInteger ? static_cast<double>(*rightInteger)
									   : std::get_if<double>(&right.getVariant())
											   ? *std::get_if<double>(&right.getVariant())
											   : std::numeric_limits<double>::quiet_NaN();
			if (!std::isfinite(leftNumber) || !std::isfinite(rightNumber)) {
				throw std::runtime_error("logical-plan arithmetic requires numeric operands");
			}
			if (operation == expressions::BinaryOperatorType::BOP_MODULO && (!leftInteger || !rightInteger)) {
				throw std::runtime_error("logical-plan modulo requires integer operands");
			}
			if ((operation == expressions::BinaryOperatorType::BOP_DIVIDE ||
				 operation == expressions::BinaryOperatorType::BOP_MODULO) &&
				rightNumber == 0.0) {
				throw std::runtime_error("logical-plan arithmetic attempted division by zero");
			}
			if (leftInteger && rightInteger) {
				switch (operation) {
					case expressions::BinaryOperatorType::BOP_ADD:
						return PropertyValue(checkedAdd(*leftInteger, *rightInteger));
					case expressions::BinaryOperatorType::BOP_SUBTRACT:
						return PropertyValue(checkedSubtract(*leftInteger, *rightInteger));
					case expressions::BinaryOperatorType::BOP_MULTIPLY:
						return PropertyValue(checkedMultiply(*leftInteger, *rightInteger));
					case expressions::BinaryOperatorType::BOP_MODULO:
						if (*leftInteger == std::numeric_limits<int64_t>::min() && *rightInteger == -1) {
							throw std::overflow_error("logical-plan integer modulo overflowed");
						}
						return PropertyValue(*leftInteger % *rightInteger);
					default:
						break;
				}
			}
			PropertyValue result;
			switch (operation) {
				case expressions::BinaryOperatorType::BOP_ADD:
					result = PropertyValue(leftNumber + rightNumber);
					break;
				case expressions::BinaryOperatorType::BOP_SUBTRACT:
					result = PropertyValue(leftNumber - rightNumber);
					break;
				case expressions::BinaryOperatorType::BOP_MULTIPLY:
					result = PropertyValue(leftNumber * rightNumber);
					break;
				case expressions::BinaryOperatorType::BOP_DIVIDE:
					result = PropertyValue(leftNumber / rightNumber);
					break;
				case expressions::BinaryOperatorType::BOP_POWER:
					result = PropertyValue(std::pow(leftNumber, rightNumber));
					break;
				default: // ZYX_COV_EXCL_LINE: isArithmeticOperator admits every arithmetic enum value handled above.
					throw std::logic_error("logical-plan runtime received an unsupported arithmetic operation");
			}
			const double value = std::get<double>(result.getVariant());
			if (!std::isfinite(value)) {
				throw std::overflow_error("logical-plan floating-point arithmetic produced a non-finite result");
			}
			return result;
		}

		[[nodiscard]] PropertyValue comparison(expressions::BinaryOperatorType operation, const PropertyValue &left,
											   const PropertyValue &right) {
			if (isNull(left) || isNull(right)) {
				return PropertyValue();
			}
			switch (operation) {
				case expressions::BinaryOperatorType::BOP_EQUAL:
					return PropertyValue(left == right);
				case expressions::BinaryOperatorType::BOP_NOT_EQUAL:
					return PropertyValue(left != right);
				case expressions::BinaryOperatorType::BOP_LESS:
					return PropertyValue(left < right);
				case expressions::BinaryOperatorType::BOP_GREATER:
					return PropertyValue(left > right);
				case expressions::BinaryOperatorType::BOP_LESS_EQUAL:
					return PropertyValue(left <= right);
				case expressions::BinaryOperatorType::BOP_GREATER_EQUAL:
					return PropertyValue(left >= right);
				default:
					throw std::logic_error("logical-plan runtime received an unsupported comparison operation");
			}
		}

		[[nodiscard]] PropertyValue logical(expressions::BinaryOperatorType operation, const PropertyValue &left,
											const PropertyValue &right) {
			const std::optional<bool> leftBoolean = booleanOrNull(left);
			const std::optional<bool> rightBoolean = booleanOrNull(right);
			switch (operation) {
				case expressions::BinaryOperatorType::BOP_AND:
					if (leftBoolean == false || rightBoolean == false) {
						return PropertyValue(false);
					}
					return makeBoolean(leftBoolean.has_value() && rightBoolean.has_value() ? std::optional<bool>(true)
																						   : std::nullopt);
				case expressions::BinaryOperatorType::BOP_OR:
					if (leftBoolean == true || rightBoolean == true) {
						return PropertyValue(true);
					}
					return makeBoolean(leftBoolean.has_value() && rightBoolean.has_value() ? std::optional<bool>(false)
																						   : std::nullopt);
				case expressions::BinaryOperatorType::BOP_XOR:
					return !leftBoolean.has_value() || !rightBoolean.has_value()
								   ? PropertyValue()
								   : PropertyValue(*leftBoolean != *rightBoolean);
				default: // ZYX_COV_EXCL_LINE: isLogicalOperator admits every logical enum value handled above.
					throw std::logic_error("logical-plan runtime received an unsupported logical operation");
			}
		}

		[[nodiscard]] PropertyValue stringOperation(expressions::BinaryOperatorType operation,
													const PropertyValue &left, const PropertyValue &right) {
			if (isNull(left) || isNull(right)) {
				return PropertyValue();
			}
			const std::string leftString = expressions::EvaluationContext::toString(left);
			const std::string rightString = expressions::EvaluationContext::toString(right);
			switch (operation) {
				case expressions::BinaryOperatorType::BOP_STARTS_WITH:
					return PropertyValue(leftString.starts_with(rightString));
				case expressions::BinaryOperatorType::BOP_ENDS_WITH:
					return PropertyValue(leftString.ends_with(rightString));
				case expressions::BinaryOperatorType::BOP_CONTAINS:
					return PropertyValue(leftString.find(rightString) != std::string::npos);
				case expressions::BinaryOperatorType::BOP_REGEX_MATCH:
					try {
						return PropertyValue(std::regex_match(
								leftString, std::regex(rightString, std::regex_constants::ECMAScript)));
					} catch (const std::regex_error &) {
						throw std::runtime_error("logical-plan received an invalid regular expression");
					}
				default: // ZYX_COV_EXCL_LINE: callers dispatch only the three supported String operator enum values.
					throw std::logic_error("logical-plan runtime received an unsupported string operation");
			}
		}

		[[nodiscard]] std::string lowerCase(std::string_view value) {
			std::string result(value);
			std::ranges::transform(result, result.begin(),
								   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
			return result;
		}

		void requireScalarFunctionArgumentType(const PropertyValue &value, const PropertyType expected,
											   std::string_view functionName, const size_t argumentIndex) {
			if (isNull(value) || value.getType() == expected) {
				return;
			}
			throw std::runtime_error("logical-plan " + std::string(functionName) + "() argument " +
									 std::to_string(argumentIndex + 1U) + " has an invalid type");
		}

		void requireScalarFunctionArgumentOneOf(const PropertyValue &value, const PropertyType first,
												const PropertyType second, std::string_view functionName,
												const size_t argumentIndex) {
			if (isNull(value) || value.getType() == first || value.getType() == second) {
				return;
			}
			throw std::runtime_error("logical-plan " + std::string(functionName) + "() argument " +
									 std::to_string(argumentIndex + 1U) + " has an invalid type");
		}

		// The legacy FunctionRegistry remains a storage-neutral implementation
		// detail during retirement, but the native public path owns Cypher's typed
		// contract. Validate it here instead of inheriting permissive display
		// conversion behavior.
		void validateNativeScalarFunctionTypes(const std::string_view functionName,
											   const std::vector<PropertyValue> &arguments) {
			const bool unaryNumeric = functionName == "abs" || functionName == "ceil" || functionName == "floor" ||
									  functionName == "round" || functionName == "sqrt" || functionName == "sign" ||
									  functionName == "log" || functionName == "log10" || functionName == "exp" ||
									  functionName == "sin" || functionName == "cos" || functionName == "tan" ||
									  functionName == "asin" || functionName == "acos" || functionName == "atan";
			if (unaryNumeric) {
				requireScalarFunctionArgumentOneOf(arguments.front(), PropertyType::INTEGER, PropertyType::DOUBLE,
												   functionName, 0U);
				return;
			}
			if (functionName == "pow" || functionName == "atan2") {
				for (size_t index = 0U; index < arguments.size(); ++index) {
					requireScalarFunctionArgumentOneOf(arguments[index], PropertyType::INTEGER, PropertyType::DOUBLE,
													   functionName, index);
				}
				return;
			}
			const bool unaryString = functionName == "upper" || functionName == "lower" || functionName == "toupper" ||
									 functionName == "tolower" || functionName == "trim" || functionName == "ltrim" ||
									 functionName == "rtrim";
			if (unaryString) {
				requireScalarFunctionArgumentType(arguments.front(), PropertyType::STRING, functionName, 0U);
				return;
			}
			if (functionName == "substring") {
				requireScalarFunctionArgumentType(arguments[0], PropertyType::STRING, functionName, 0U);
				requireScalarFunctionArgumentType(arguments[1], PropertyType::INTEGER, functionName, 1U);
				if (arguments.size() == 3U) {
					requireScalarFunctionArgumentType(arguments[2], PropertyType::INTEGER, functionName, 2U);
				}
				return;
			}
			if (functionName == "startswith" || functionName == "endswith" || functionName == "contains" ||
				functionName == "split") {
				requireScalarFunctionArgumentType(arguments[0], PropertyType::STRING, functionName, 0U);
				requireScalarFunctionArgumentType(arguments[1], PropertyType::STRING, functionName, 1U);
				return;
			}
			if (functionName == "replace") {
				for (size_t index = 0U; index < arguments.size(); ++index) {
					requireScalarFunctionArgumentType(arguments[index], PropertyType::STRING, functionName, index);
				}
				return;
			}
			if (functionName == "left" || functionName == "right") {
				requireScalarFunctionArgumentType(arguments[0], PropertyType::STRING, functionName, 0U);
				requireScalarFunctionArgumentType(arguments[1], PropertyType::INTEGER, functionName, 1U);
				return;
			}
			if (functionName == "length" || functionName == "size" || functionName == "reverse") {
				requireScalarFunctionArgumentOneOf(arguments.front(), PropertyType::STRING, PropertyType::LIST,
												   functionName, 0U);
				return;
			}
			if (functionName == "head" || functionName == "tail" || functionName == "last" || functionName == "nodes" ||
				functionName == "relationships") {
				requireScalarFunctionArgumentType(arguments.front(), PropertyType::LIST, functionName, 0U);
			}
		}

		[[nodiscard]] PropertyValue evaluateNativeToBoolean(const PropertyValue &argument) {
			if (isNull(argument)) {
				return PropertyValue();
			}
			if (const bool *value = std::get_if<bool>(&argument.getVariant())) {
				return PropertyValue(*value);
			}
			if (const int64_t *value = std::get_if<int64_t>(&argument.getVariant())) {
				return PropertyValue(*value != 0);
			}
			if (const double *value = std::get_if<double>(&argument.getVariant())) {
				if (!std::isfinite(*value)) {
					throw std::runtime_error("logical-plan toBoolean() requires a finite numeric argument");
				}
				return PropertyValue(*value != 0.0);
			}
			if (const std::string *value = std::get_if<std::string>(&argument.getVariant())) {
				const std::string normalized = lowerCase(*value);
				if (normalized == "true") {
					return PropertyValue(true);
				}
				if (normalized == "false") {
					return PropertyValue(false);
				}
				throw std::runtime_error("logical-plan toBoolean() requires true or false text");
			}
			throw std::runtime_error("logical-plan toBoolean() requires a Boolean, numeric value, or true/false text");
		}

		[[nodiscard]] PropertyValue evaluateEntityFunction(const expressions::FunctionCallExpression &function,
														   LogicalPlanReadRow &row, ExecutionState &state) {
			const std::vector<std::unique_ptr<expressions::Expression>> &arguments = function.getArguments();
			if (arguments.size() != 1U || arguments.front() == nullptr) {
				throw std::runtime_error("logical-plan entity function requires exactly one variable argument");
			}
			const auto *reference =
					dynamic_cast<const expressions::VariableReferenceExpression *>(arguments.front().get());
			if (reference == nullptr || reference->hasProperty()) {
				throw std::runtime_error("logical-plan entity function requires a bare variable argument");
			}
			const LogicalPlanReadValue &binding = requireBinding(row, reference->getVariableName());
			const std::string name = lowerCase(function.getFunctionName());
			if (name == "id") {
				if (const auto *node = std::get_if<storage::LogicalGraphNode>(&binding)) {
					return PropertyValue(node->node_id);
				}
				if (const auto *edge = std::get_if<storage::LogicalGraphEdge>(&binding)) {
					return PropertyValue(edge->edge_id);
				}
				if (const auto *reference = std::get_if<storage::EntityRef>(&binding)) {
					validateEntityRef(*reference);
					return PropertyValue(reference->entity_id);
				}
				throw std::runtime_error("logical-plan id() requires a node or relationship variable");
			}
			if (name == "labels") {
				if (const auto metadata = row.entity_metadata_cache.find(reference->getVariableName());
					metadata != row.entity_metadata_cache.end()) {
					if (const auto cached = metadata->second.find(name); cached != metadata->second.end()) {
						detail::ProcedurePayloadAdmissionScope::admit(detail::procedurePayloadHeapUpper<true>(cached->second));
						return cached->second;
					}
				}
				// A deferred Node reference needs only its labels for labels(). Keep the
				// read shape narrow so the Property root is not opened merely to answer
				// an introspection function. Complete Node values already carry labels.
				const storage::LogicalGraphNode node = [&]() {
					if (const auto *referenceValue = std::get_if<storage::EntityRef>(&binding)) {
						validateNodeRef(*referenceValue, "logical-plan labels() requires a valid node variable");
						return state.shared.snapshot.readNodeShaped(referenceValue->entity_id, {.include_labels = true});
					}
					return materializeNodeValue(state, binding);
				}();
				if (node.labels.size() > state.shared.bounds.maximum_rows) {
					throw std::runtime_error("logical-plan labels() exceeded the configured List bound");
				}
				std::vector<PropertyValue> labels;
				labels.reserve(node.labels.size());
				for (const std::string &label: node.labels) {
					detail::ProcedurePayloadAdmissionScope::admit(utils::detail::standardFreshStringHeapUpper(label.size()));
                    labels.emplace_back(label);
				}
				PropertyValue result(std::move(labels));
				auto [metadataOwner, metadataInserted] = detail::procedureMapTryEmplace(row.entity_metadata_cache, reference->getVariableName());
                static_cast<void>(metadataInserted);
                detail::procedureMapInsertOrAssign(metadataOwner->second, name, result);
				return result;
			}
			if (name == "type") {
				if (const auto metadata = row.entity_metadata_cache.find(reference->getVariableName());
					metadata != row.entity_metadata_cache.end()) {
					if (const auto cached = metadata->second.find(name); cached != metadata->second.end()) {
						detail::ProcedurePayloadAdmissionScope::admit(detail::procedurePayloadHeapUpper<true>(cached->second));
						return cached->second;
					}
				}
				// type() is topology metadata. Resolve just the relationship type for a
				// deferred Edge reference; full Edge properties are unnecessary here.
				if (const auto *referenceValue = std::get_if<storage::EntityRef>(&binding)) {
					validateEdgeRef(*referenceValue, "logical-plan type() requires a valid relationship variable");
					PropertyValue result(
							state.shared.snapshot.readEdgeShaped(referenceValue->entity_id, {.include_type = true}).type);
					auto [metadataOwner, metadataInserted] = detail::procedureMapTryEmplace(row.entity_metadata_cache, reference->getVariableName());
                static_cast<void>(metadataInserted);
                detail::procedureMapInsertOrAssign(metadataOwner->second, name, result);
					return result;
				}
				PropertyValue result(materializeEdgeValue(state, binding).type);
				auto [metadataOwner, metadataInserted] = detail::procedureMapTryEmplace(row.entity_metadata_cache, reference->getVariableName());
                static_cast<void>(metadataInserted);
                detail::procedureMapInsertOrAssign(metadataOwner->second, name, result);
				return result;
			}
			if (name != "keys" && name != "properties") {
				throw std::logic_error("logical-plan received an unknown entity introspection function");
			}
			const auto *scalar = std::get_if<PropertyValue>(&binding);
			if (!std::holds_alternative<storage::LogicalGraphNode>(binding) &&
				!std::holds_alternative<storage::LogicalGraphEdge>(binding) &&
				!std::holds_alternative<storage::EntityRef>(binding) &&
				(scalar == nullptr || scalar->getType() != PropertyType::MAP)) {
				throw std::runtime_error(
						"logical-plan entity Property function requires a node, relationship, or Map variable");
			}
			const PropertyMap &properties =
					readBindingProperties(state.shared.snapshot, row, reference->getVariableName(), state);
			if (name == "properties") {
                detail::ProcedurePayloadAdmissionScope::admit(detail::procedurePayloadHeapUpper<true>(properties));
				return PropertyValue(properties);
			}
			if (properties.size() > state.shared.bounds.maximum_rows) {
				throw std::runtime_error("logical-plan keys() exceeded the configured List bound");
			}
			std::vector<std::string> orderedKeys;
			detail::procedureVectorReserve(orderedKeys, properties.size());
			for (const auto &[key, value]: properties) {
				static_cast<void>(value);
				detail::ProcedurePayloadAdmissionScope::admit(utils::detail::standardFreshStringHeapUpper(key.size()));
                orderedKeys.push_back(key);
			}
			std::ranges::sort(orderedKeys);
			std::vector<PropertyValue> keys;
			detail::procedureVectorReserve(keys, orderedKeys.size());
			for (const std::string &key: orderedKeys) {
				detail::ProcedurePayloadAdmissionScope::admit(utils::detail::standardFreshStringHeapUpper(key.size()));
                keys.emplace_back(key);
			}
			return PropertyValue(std::move(keys));
		}

		[[nodiscard]] PropertyValue evaluateRangeFunction(const std::vector<PropertyValue> &arguments,
														  ExecutionState &state) {
			if (arguments.size() < 2U || arguments.size() > 3U ||
				std::ranges::any_of(arguments, [](const PropertyValue &argument) { return isNull(argument); })) {
				return PropertyValue();
			}
			const auto toInteger = [](const PropertyValue &value) -> int64_t {
				if (const auto *integer = std::get_if<int64_t>(&value.getVariant())) {
					return *integer;
				}
				if (const auto *number = std::get_if<double>(&value.getVariant());
					number != nullptr && std::isfinite(*number) &&
					*number >= static_cast<double>(std::numeric_limits<int64_t>::min()) &&
					*number < static_cast<double>(std::numeric_limits<int64_t>::max())) {
					return static_cast<int64_t>(*number);
				}
				throw std::runtime_error("logical-plan range() requires numeric arguments");
			};
			const int64_t start = toInteger(arguments[0]);
			const int64_t end = toInteger(arguments[1]);
			const int64_t step = arguments.size() == 3U ? toInteger(arguments[2]) : 1;
			if (step == 0) {
				throw std::runtime_error("range() step cannot be zero");
			}
			MaterializedByteReservation listBytes(&state.shared.materialized_budget);
			std::vector<PropertyValue> result;
			size_t payloadBytes = 0U;
			const auto admitListBytes = [&](size_t capacity, size_t payload) {
				const size_t bytes =
						checkedSizeAdd(sizeof(uint32_t),
									   checkedSizeAdd(checkedSizeMul(capacity, sizeof(PropertyValue),
																	 "logical-plan range() capacity overflowed"),
													  payload, "logical-plan range() storage size overflowed"),
									   "logical-plan range() storage size overflowed");
				if (listBytes.wouldExceed(bytes)) {
					throw std::runtime_error("logical-plan range() exceeded its shared byte bound");
				}
				listBytes.set(bytes);
			};
			const bool hasValues = step > 0 ? start <= end : start >= end;
			if (hasValues && state.shared.bounds.maximum_rows != 0U) {
				const uint64_t distance = step > 0 ? static_cast<uint64_t>(end) - static_cast<uint64_t>(start)
												   : static_cast<uint64_t>(start) - static_cast<uint64_t>(end);
				const uint64_t magnitude =
						step > 0 ? static_cast<uint64_t>(step) : uint64_t{0} - static_cast<uint64_t>(step);
				const uint64_t additionalValues = distance / magnitude;
				const size_t initialCapacity =
						std::min(state.shared.bounds.maximum_rows,
								 additionalValues >= 15U ? size_t{16} : static_cast<size_t>(additionalValues + 1U));
				admitListBytes(initialCapacity, sizeof(int64_t));
				result.reserve(initialCapacity);
				admitListBytes(result.capacity(), sizeof(int64_t));
			}
			int64_t current = start;
			while ((step > 0 && current <= end) || (step < 0 && current >= end)) {
				checkGuard(state);
				if (result.size() == state.shared.bounds.maximum_rows) {
					throw std::runtime_error("logical-plan range() exceeded the configured List bound");
				}
				const size_t nextPayloadBytes =
						checkedSizeAdd(payloadBytes, sizeof(int64_t), "logical-plan range() payload size overflowed");
				if (result.size() == result.capacity()) {
					const size_t capacity = result.capacity();
					const size_t nextCapacity = capacity == 0U								? 1U
												: capacity > state.shared.bounds.maximum_rows / 2U ? state.shared.bounds.maximum_rows
																							: capacity * 2U;
					admitListBytes(nextCapacity, nextPayloadBytes);
					result.reserve(nextCapacity);
				}
				admitListBytes(result.capacity(), nextPayloadBytes);
				result.emplace_back(current);
				payloadBytes = nextPayloadBytes;
				if ((step > 0 && current > std::numeric_limits<int64_t>::max() - step) ||
					(step < 0 && current < std::numeric_limits<int64_t>::min() - step)) {
					break;
				}
				current += step;
			}
			return PropertyValue(std::move(result));
		}

		[[nodiscard]] PropertyValue evaluateFunctionExpression(const expressions::FunctionCallExpression &function,
															   LogicalPlanReadRow &row, ExecutionState &state) {
			const expressions::ScalarFunction *registered =
					expressions::FunctionRegistry::getInstance().lookupScalarFunction(function.getFunctionName());
			if (registered == nullptr) {
				throw std::runtime_error("logical-plan runtime could not resolve a scalar function");
			}
			if (!registered->validateArgCount(function.getArgumentCount())) {
				throw std::runtime_error("logical-plan scalar function has an invalid argument count");
			}
			if (function.getArgumentCount() > state.shared.bounds.maximum_rows) {
				throw std::runtime_error("logical-plan scalar function exceeded the configured argument bound");
			}
			if (dynamic_cast<const expressions::EntityIntrospectionFunction *>(registered) != nullptr) {
				return evaluateEntityFunction(function, row, state);
			}
			const std::string normalizedName = lowerCase(function.getFunctionName());
			if ((normalizedName == "size" || normalizedName == "length") && function.getArgumentCount() == 1U) {
				const auto *reference = dynamic_cast<const expressions::VariableReferenceExpression *>(
						function.getArguments().front().get());
				if (reference != nullptr && !reference->hasProperty()) {
					const auto cached = row.entity_metadata_cache.find(reference->getVariableName());
					if (cached != row.entity_metadata_cache.end()) {
						const auto scalar = cached->second.find("$shortest_path_" + normalizedName);
						if (scalar != cached->second.end())
							return scalar->second;
					}
				}
			}
			if ((normalizedName == "size" || normalizedName == "length") && function.getArgumentCount() == 1U &&
				function.getArguments().front() != nullptr &&
				function.getArguments().front()->getExpressionType() == expressions::ExpressionType::SHORTEST_PATH) {
				return evaluateScalarExpression(*function.getArguments().front(), row, state,
												normalizedName == "size" ? ShortestPathResultMode::SPR_SIZE
																		 : ShortestPathResultMode::SPR_LENGTH);
			}
			std::vector<PropertyValue> arguments;
			MaterializedByteReservation argumentBytes(&state.shared.materialized_budget);
			const size_t requestedArgumentSlotsBytes =
					checkedSizeMul(function.getArgumentCount(), sizeof(PropertyValue),
								   "logical-plan scalar function argument storage size overflowed");
			argumentBytes.set(requestedArgumentSlotsBytes);
			arguments.reserve(function.getArgumentCount());
			const size_t argumentSlotsBytes =
					checkedSizeMul(arguments.capacity(), sizeof(PropertyValue),
								   "logical-plan scalar function argument storage size overflowed");
			argumentBytes.set(argumentSlotsBytes);
			size_t argumentPayloadBytes = 0U;
			for (const std::unique_ptr<expressions::Expression> &argument: function.getArguments()) {
				if (argument == nullptr) {
					throw std::logic_error("logical-plan scalar function has a null argument expression");
				}
				PropertyValue value = scalarValue(evaluateExpression(*argument, row, state));
				if (value.getType() == PropertyType::LIST && value.getList().size() > state.shared.bounds.maximum_rows) {
					throw std::runtime_error(
							"logical-plan scalar function received a List exceeding the configured List bound");
				}
				const size_t nextPayloadBytes =
						checkedSizeAdd(argumentPayloadBytes, retainedExpressionValueBytes(value, state),
									   "logical-plan scalar function argument payload size overflowed");
				argumentBytes.set(checkedSizeAdd(argumentSlotsBytes, nextPayloadBytes,
												 "logical-plan scalar function argument storage size overflowed"));
				arguments.push_back(std::move(value));
				argumentPayloadBytes = nextPayloadBytes;
			}
			if (normalizedName == "range") {
				return evaluateRangeFunction(arguments, state);
			}
			if (normalizedName == "toboolean") {
				return evaluateNativeToBoolean(arguments.front());
			}
			validateNativeScalarFunctionTypes(normalizedName, arguments);
			if (normalizedName == "length" && arguments.size() == 1U &&
				arguments.front().getType() == PropertyType::LIST) {
				const std::vector<PropertyValue> &elements = arguments.front().getList();
				bool isPath = !elements.empty();
				size_t relationships = 0U;
				for (size_t index = 0U; index < elements.size(); ++index) {
					if (elements[index].getType() != PropertyType::MAP) {
						isPath = false;
						break;
					}
					const auto type = elements[index].getMap().find("_type");
					const std::string expected = index % 2U == 0U ? "node" : "relationship";
					if (type == elements[index].getMap().end() || type->second.getType() != PropertyType::STRING ||
						std::get<std::string>(type->second.getVariant()) != expected) {
						isPath = false;
						break;
					}
					relationships += index % 2U == 0U ? 0U : 1U;
				}
				if (isPath) {
					if (relationships > static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
						throw std::overflow_error("logical-plan path length exceeds the integer result range");
					}
					return PropertyValue(static_cast<int64_t>(relationships));
				}
			}
			Record emptyRecord;
			const std::unordered_map<std::string, PropertyValue> emptyParameters;
			const std::unordered_map<std::string, PropertyValue> &parameters =
					state.shared.context == nullptr ? emptyParameters : state.shared.context->parameters;
			const expressions::EvaluationContext evaluationContext(
					emptyRecord, parameters, {.maximum_collection_elements = state.shared.bounds.maximum_rows});
			PropertyValue result = registered->evaluate(arguments, evaluationContext);
			if (result.getType() == PropertyType::LIST && result.getList().size() > state.shared.bounds.maximum_rows) {
				throw std::runtime_error(
						"logical-plan scalar function returned a List exceeding the configured List bound");
			}
			return result;
		}

