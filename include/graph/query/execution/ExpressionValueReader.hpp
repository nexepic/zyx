#pragma once

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

#include "graph/core/PropertyTypes.hpp"
#include "graph/query/execution/Record.hpp"
#include "graph/query/expressions/EvaluationContext.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/expressions/ExpressionEvaluationHelper.hpp"
#include "graph/query/expressions/ParameterExpression.hpp"

namespace graph::storage { class DataManager; }

namespace graph::query::execution {

	enum class ExpressionValueReaderKind {
		EVR_NULL,
		EVR_LITERAL,
		EVR_PARAMETER,
		EVR_VARIABLE,
		EVR_PROPERTY,
		EVR_GENERIC
	};

	class ExpressionValueReader {
	public:
		ExpressionValueReader() = default;

		static ExpressionValueReader compile(std::shared_ptr<expressions::Expression> expression) {
			ExpressionValueReader reader;
			reader.expression_ = std::move(expression);
			const auto *expr = reader.expression_.get();
			if (!expr) {
				reader.kind_ = ExpressionValueReaderKind::EVR_NULL;
				return reader;
			}

			switch (expr->getExpressionType()) {
				case expressions::ExpressionType::LITERAL: {
					const auto *literal = static_cast<const expressions::LiteralExpression *>(expr);
					reader.kind_ = ExpressionValueReaderKind::EVR_LITERAL;
					reader.literalValue_ = readLiteral(literal);
					break;
				}
				case expressions::ExpressionType::PARAMETER: {
					const auto *parameter = static_cast<const expressions::ParameterExpression *>(expr);
					reader.kind_ = ExpressionValueReaderKind::EVR_PARAMETER;
					reader.name_ = parameter->getParameterName();
					break;
				}
				case expressions::ExpressionType::VARIABLE_REFERENCE:
				case expressions::ExpressionType::PROPERTY_ACCESS: {
					const auto *variable = static_cast<const expressions::VariableReferenceExpression *>(expr);
					reader.kind_ = variable->hasProperty() ? ExpressionValueReaderKind::EVR_PROPERTY
					                                      : ExpressionValueReaderKind::EVR_VARIABLE;
					reader.name_ = variable->getVariableName();
					reader.propertyName_ = variable->hasProperty() ? variable->getPropertyName() : std::string{};
					break;
				}
				default:
					reader.kind_ = ExpressionValueReaderKind::EVR_GENERIC;
					break;
			}
			return reader;
		}

		[[nodiscard]] PropertyValue evaluate(
				const Record &record, storage::DataManager *dataManager = nullptr,
				const std::unordered_map<std::string, PropertyValue> *parameters = nullptr) const {
			switch (kind_) {
				case ExpressionValueReaderKind::EVR_NULL:
					return PropertyValue();
				case ExpressionValueReaderKind::EVR_LITERAL:
					return literalValue_;
				case ExpressionValueReaderKind::EVR_PARAMETER:
					return evaluateParameter(parameters);
				case ExpressionValueReaderKind::EVR_VARIABLE:
					return evaluateVariable(record);
				case ExpressionValueReaderKind::EVR_PROPERTY:
					return evaluateProperty(record);
				case ExpressionValueReaderKind::EVR_GENERIC:
					return expressions::ExpressionEvaluationHelper::evaluate(
						expression_.get(), record, dataManager, parameters);
			}
			return PropertyValue();
		}

		[[nodiscard]] ExpressionValueReaderKind kind() const { return kind_; }

	private:
		static PropertyValue readLiteral(const expressions::LiteralExpression *literal) {
			if (!literal || literal->isNull()) {
				return PropertyValue();
			}
			if (literal->isBoolean()) {
				return PropertyValue(literal->getBooleanValue());
			}
			if (literal->isInteger()) {
				return PropertyValue(literal->getIntegerValue());
			}
			if (literal->isDouble()) {
				return PropertyValue(literal->getDoubleValue());
			}
			if (literal->isString()) {
				return PropertyValue(literal->getStringValue());
			}
			return PropertyValue();
		}

		[[nodiscard]] PropertyValue evaluateParameter(
				const std::unordered_map<std::string, PropertyValue> *parameters) const {
			if (!parameters) {
				throw expressions::ExpressionEvaluationException("Missing query parameter: $" + name_);
			}
			auto it = parameters->find(name_);
			if (it == parameters->end()) {
				throw expressions::ExpressionEvaluationException("Missing query parameter: $" + name_);
			}
			return it->second;
		}

		[[nodiscard]] PropertyValue evaluateVariable(const Record &record) const {
			if (auto node = record.getNodeRef(name_)) {
				return PropertyValue(node->get().getId());
			}
			if (auto edge = record.getEdgeRef(name_)) {
				return PropertyValue(edge->get().getId());
			}
			if (auto value = record.getValueRef(name_)) {
				return value->get();
			}
			throw expressions::UndefinedVariableException(name_);
		}

		[[nodiscard]] PropertyValue evaluateProperty(const Record &record) const {
			if (auto node = record.getNodeRef(name_)) {
				const auto &properties = node->get().getProperties();
				auto it = properties.find(propertyName_);
				return it == properties.end() ? PropertyValue() : it->second;
			}
			if (auto edge = record.getEdgeRef(name_)) {
				const auto &properties = edge->get().getProperties();
				auto it = properties.find(propertyName_);
				return it == properties.end() ? PropertyValue() : it->second;
			}
			if (auto value = record.getValueRef(name_)) {
				const auto &propertyValue = value->get();
				if (propertyValue.getType() == PropertyType::MAP) {
					const auto &map = propertyValue.getMap();
					auto it = map.find(propertyName_);
					return it == map.end() ? PropertyValue() : it->second;
				}
				return extractTemporalComponent(propertyValue, propertyName_);
			}
			return PropertyValue();
		}

		static PropertyValue extractTemporalComponent(const PropertyValue &value, const std::string &component) {
			if (value.getType() == PropertyType::DATE) {
				const auto &date = std::get<TemporalDate>(value.getVariant());
				if (component == "year") return PropertyValue(static_cast<int64_t>(date.year()));
				if (component == "month") return PropertyValue(static_cast<int64_t>(date.month()));
				if (component == "day") return PropertyValue(static_cast<int64_t>(date.day()));
				if (component == "epochDays") return PropertyValue(static_cast<int64_t>(date.epochDays));
			} else if (value.getType() == PropertyType::DATETIME) {
				const auto &dateTime = std::get<TemporalDateTime>(value.getVariant());
				if (component == "year") return PropertyValue(static_cast<int64_t>(dateTime.year()));
				if (component == "month") return PropertyValue(static_cast<int64_t>(dateTime.month()));
				if (component == "day") return PropertyValue(static_cast<int64_t>(dateTime.day()));
				if (component == "hour") return PropertyValue(static_cast<int64_t>(dateTime.hour()));
				if (component == "minute") return PropertyValue(static_cast<int64_t>(dateTime.minute()));
				if (component == "second") return PropertyValue(static_cast<int64_t>(dateTime.second()));
				if (component == "epochMillis") return PropertyValue(dateTime.epochMillis);
			} else if (value.getType() == PropertyType::DURATION) {
				const auto &duration = std::get<TemporalDuration>(value.getVariant());
				if (component == "months") return PropertyValue(duration.months);
				if (component == "days") return PropertyValue(duration.days);
				if (component == "seconds") return PropertyValue(duration.nanos / 1000000000LL);
				if (component == "nanoseconds") return PropertyValue(duration.nanos);
			}
			return PropertyValue();
		}

		std::shared_ptr<expressions::Expression> expression_;
		ExpressionValueReaderKind kind_ = ExpressionValueReaderKind::EVR_NULL;
		std::string name_;
		std::string propertyName_;
		PropertyValue literalValue_;
	};

} // namespace graph::query::execution
