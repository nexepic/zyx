/**
 * @file PreparedQuery.cpp
 * @brief Storage-neutral implementation of the public typed query builder.
 *
 * Licensed under the Apache License, Version 2.0.
 */

#include "PreparedQueryInternal.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

#include "graph/core/PropertyTypes.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/expressions/ParameterExpression.hpp"
#include "graph/query/logical/operators/LogicalCallProcedure.hpp"
#include "graph/query/logical/operators/LogicalCreateEdge.hpp"
#include "graph/query/logical/operators/LogicalCreateIndex.hpp"
#include "graph/query/logical/operators/LogicalCreateNode.hpp"
#include "graph/query/logical/operators/LogicalDelete.hpp"
#include "graph/query/logical/operators/LogicalDropIndex.hpp"
#include "graph/query/logical/operators/LogicalFilter.hpp"
#include "graph/query/logical/operators/LogicalJoin.hpp"
#include "graph/query/logical/operators/LogicalLimit.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/logical/operators/LogicalProject.hpp"
#include "graph/query/logical/operators/LogicalRemove.hpp"
#include "graph/query/logical/operators/LogicalSet.hpp"
#include "graph/query/logical/operators/LogicalShowIndexes.hpp"
#include "graph/query/logical/operators/LogicalSingleRow.hpp"
#include "graph/query/logical/operators/LogicalSkip.hpp"
#include "graph/query/logical/operators/LogicalSort.hpp"
#include "graph/query/logical/operators/LogicalUnwind.hpp"

namespace zyx::detail {

	// PIK_ prefixes keep this internal enum safe on Windows headers too.
	enum class PreparedIdentifierKind {
		PIK_VARIABLE,
		PIK_LABEL,
		PIK_PROPERTY_KEY,
		PIK_RELATIONSHIP_TYPE,
		PIK_INDEX_NAME,
		PIK_PROCEDURE,
	};

	struct PreparedIdentifier final {
		PreparedIdentifierKind kind;
		std::string value;
	};

	class QueryBuilderImpl final {
	public:
		graph::query::QueryPlan plan;
		graph::query::QueryContext context;
		size_t next_parameter = 0U;
		std::optional<std::string> build_error;
		std::vector<PreparedIdentifier> identifiers;
	};

	namespace {

		using graph::PropertyValue;
		using graph::query::expressions::BinaryOpExpression;
		using graph::query::expressions::BinaryOperatorType;
		using graph::query::expressions::Expression;
		using graph::query::expressions::ParameterExpression;
		using graph::query::expressions::VariableReferenceExpression;
		using graph::query::logical::LogicalCallProcedure;
		using graph::query::logical::LogicalCreateEdge;
		using graph::query::logical::LogicalCreateIndex;
		using graph::query::logical::LogicalCreateNode;
		using graph::query::logical::LogicalDelete;
		using graph::query::logical::LogicalDropIndex;
		using graph::query::logical::LogicalFilter;
		using graph::query::logical::LogicalJoin;
		using graph::query::logical::LogicalLimit;
		using graph::query::logical::LogicalNodeScan;
		using graph::query::logical::LogicalOperator;
		using graph::query::logical::LogicalProject;
		using graph::query::logical::LogicalProjectItem;
		using graph::query::logical::LogicalRemove;
		using graph::query::logical::LogicalRemoveActionType;
		using graph::query::logical::LogicalRemoveItem;
		using graph::query::logical::LogicalSet;
		using graph::query::logical::LogicalSetItem;
		using graph::query::logical::LogicalShowIndexes;
		using graph::query::logical::LogicalSingleRow;
		using graph::query::logical::LogicalSkip;
		using graph::query::logical::LogicalSort;
		using graph::query::logical::LogicalSortItem;
		using graph::query::logical::LogicalUnwind;
		using graph::query::logical::ParameterPropertyPredicate;
		using graph::query::logical::SetActionType;

		constexpr size_t MAX_IDENTIFIER_LENGTH = 256U;
		constexpr size_t MAX_BUILDER_PARAMETERS = 65'536U;

		[[nodiscard]] bool isIdentifierStart(const unsigned char character) {
			return std::isalpha(character) != 0 || character == '_';
		}

		[[nodiscard]] bool isIdentifierContinue(const unsigned char character) {
			return std::isalnum(character) != 0 || character == '_';
		}

		void validateIdentifier(const std::string_view identifier, const std::string_view role) {
			if (identifier.empty() || identifier.size() > MAX_IDENTIFIER_LENGTH ||
				!isIdentifierStart(static_cast<unsigned char>(identifier.front()))) {
				throw std::invalid_argument("QueryBuilder requires a valid " + std::string(role) + " identifier");
			}
			for (const char character: identifier) {
				if (!isIdentifierContinue(static_cast<unsigned char>(character))) {
					throw std::invalid_argument("QueryBuilder requires a valid " + std::string(role) + " identifier");
				}
			}
		}

		void validateProcedureIdentifier(const std::string_view procedure) {
			if (procedure.empty() || procedure.size() > MAX_IDENTIFIER_LENGTH) {
				throw std::invalid_argument("QueryBuilder requires a valid procedure identifier");
			}
			size_t component_start = 0U;
			while (component_start < procedure.size()) {
				const size_t delimiter = procedure.find('.', component_start);
				const size_t component_size = (delimiter == std::string_view::npos ? procedure.size() : delimiter) - component_start;
				if (component_size == 0U) {
					throw std::invalid_argument("QueryBuilder requires a valid procedure identifier");
				}
				validateIdentifier(procedure.substr(component_start, component_size), "procedure component");
				if (delimiter == std::string_view::npos) {
					return;
				}
				component_start = delimiter + 1U;
			}
			throw std::invalid_argument("QueryBuilder requires a valid procedure identifier");
		}

		void recordIdentifier(QueryBuilderImpl &builder, const PreparedIdentifierKind kind, const std::string &value) {
			builder.identifiers.push_back({kind, value});
		}

		void recordIdentifiers(QueryBuilderImpl &builder, const PreparedIdentifierKind kind,
								const std::vector<std::string> &values) {
			for (const std::string &value: values) recordIdentifier(builder, kind, value);
		}

		void validateRecordedIdentifiers(const QueryBuilderImpl &builder) {
			for (const PreparedIdentifier &identifier: builder.identifiers) {
				switch (identifier.kind) {
					case PreparedIdentifierKind::PIK_VARIABLE:
						validateIdentifier(identifier.value, "variable");
						break;
					case PreparedIdentifierKind::PIK_LABEL:
						validateIdentifier(identifier.value, "label");
						break;
					case PreparedIdentifierKind::PIK_PROPERTY_KEY:
						validateIdentifier(identifier.value, "property key");
						break;
					case PreparedIdentifierKind::PIK_RELATIONSHIP_TYPE:
						validateIdentifier(identifier.value, "relationship type");
						break;
					case PreparedIdentifierKind::PIK_INDEX_NAME:
						if (!identifier.value.empty()) validateIdentifier(identifier.value, "index name");
						break;
					case PreparedIdentifierKind::PIK_PROCEDURE:
						validateProcedureIdentifier(identifier.value);
						break;
				}
			}
		}

		[[nodiscard]] PropertyValue toPropertyValue(const Value &value) {
			return std::visit(
					[]<typename T>(const T &item) -> PropertyValue {
						if constexpr (std::is_same_v<T, std::vector<float>>) {
							std::vector<PropertyValue> result;
							result.reserve(item.size());
							for (const float coordinate: item) result.emplace_back(static_cast<double>(coordinate));
							return PropertyValue(std::move(result));
						} else if constexpr (std::is_same_v<T, std::vector<std::string>>) {
							std::vector<PropertyValue> result;
							result.reserve(item.size());
							for (const std::string &element: item) result.emplace_back(element);
							return PropertyValue(std::move(result));
						} else if constexpr (std::is_same_v<T, std::shared_ptr<ValueList>>) {
							if (!item) return PropertyValue();
							std::vector<PropertyValue> result;
							result.reserve(item->elements.size());
							for (const Value &element: item->elements) result.push_back(toPropertyValue(element));
							return PropertyValue(std::move(result));
						} else if constexpr (std::is_same_v<T, std::shared_ptr<ValueMap>>) {
							if (!item) return PropertyValue();
							PropertyValue::MapType result;
							result.reserve(item->entries.size());
							for (const auto &[key, element]: item->entries) result.emplace(key, toPropertyValue(element));
							return PropertyValue(std::move(result));
						} else if constexpr (std::is_same_v<T, Bytes>) {
							PropertyValue::Bytes result;
							result.reserve(item.size());
							for (const std::uint8_t byte: item) result.push_back(static_cast<std::byte>(byte));
							return PropertyValue(std::move(result));
						} else if constexpr (std::is_same_v<T, std::shared_ptr<Node>> ||
									 std::is_same_v<T, std::shared_ptr<Edge>>) {
							throw std::invalid_argument("QueryBuilder values cannot contain graph entities");
						} else {
							return PropertyValue(item);
						}
					},
					value);
		}

		[[nodiscard]] std::string bindValue(QueryBuilderImpl &builder, const Value &value) {
			if (builder.next_parameter == MAX_BUILDER_PARAMETERS) {
				throw std::length_error("QueryBuilder exceeded its bound-value limit");
			}
			const std::string name = "__prepared_value_" + std::to_string(builder.next_parameter++);
			builder.context.parameters.emplace(name, toPropertyValue(value));
			return name;
		}

		[[nodiscard]] std::unordered_map<std::string, std::shared_ptr<Expression>>
		bindProperties(QueryBuilderImpl &builder, const std::unordered_map<std::string, Value> &properties) {
			std::unordered_map<std::string, std::shared_ptr<Expression>> expressions;
			expressions.reserve(properties.size());
			for (const auto &[key, value]: properties) {
				const std::string parameter = bindValue(builder, value);
				expressions.emplace(key, std::make_shared<ParameterExpression>(parameter));
			}
			return expressions;
		}

		void appendPipeline(QueryBuilderImpl &builder, std::unique_ptr<LogicalOperator> operation) {
			if (!operation) {
				throw std::logic_error("QueryBuilder cannot append an empty logical operation");
			}
			if (!builder.plan.root) {
				builder.plan.root = std::move(operation);
				return;
			}
			operation->setChild(0U, std::move(builder.plan.root));
			builder.plan.root = std::move(operation);
		}

		void recordBuilderError(QueryBuilderImpl &builder, const std::string_view message) {
			if (!builder.build_error.has_value()) builder.build_error = std::string(message);
		}

		void requirePipelineRoot(QueryBuilderImpl &builder, const std::string_view operation) {
			if (!builder.plan.root) {
				recordBuilderError(builder, "QueryBuilder " + std::string(operation) + " requires a preceding source clause");
				builder.plan.root = std::make_unique<LogicalSingleRow>();
			}
		}

		void validatePreparedPlan(const QueryBuilderImpl &builder) {
			validateRecordedIdentifiers(builder);
			if (!builder.plan.root) return;
			std::vector<const LogicalOperator *> pending{builder.plan.root.get()};
			std::unordered_set<const LogicalOperator *> visited;
			while (!pending.empty()) {
				const LogicalOperator *current = pending.back();
				pending.pop_back();
				if (current == nullptr || !visited.insert(current).second) {
					throw std::logic_error("QueryBuilder produced an invalid logical plan graph");
				}
				for (const LogicalOperator *child: current->getChildren()) {
					if (child == nullptr) throw std::logic_error("QueryBuilder produced a logical plan with a null child");
					pending.push_back(child);
				}
			}
		}

	} // namespace

	QueryBuilderImpl &requireQueryBuilderImpl(QueryBuilder &builder) {
		if (!builder.impl_) {
			throw std::logic_error("QueryBuilder cannot be used after it has been moved");
		}
		return *builder.impl_;
	}

	graph::query::QueryPlan clonePreparedQueryPlan(const PreparedQueryImpl &prepared) { return prepared.plan.clone(); }

	graph::query::QueryContext preparedQueryContext(const PreparedQueryImpl &prepared) { return prepared.context; }

} // namespace zyx::detail

namespace zyx {

	PreparedQuery::PreparedQuery(std::unique_ptr<detail::PreparedQueryImpl> impl) : impl_(std::move(impl)) {}
	PreparedQuery::~PreparedQuery() = default;
	PreparedQuery::PreparedQuery(PreparedQuery &&) noexcept = default;
	PreparedQuery &PreparedQuery::operator=(PreparedQuery &&) noexcept = default;
	bool PreparedQuery::empty() const noexcept { return !impl_ || !impl_->plan.root; }

	QueryBuilder::QueryBuilder() : impl_(std::make_unique<detail::QueryBuilderImpl>()) {}
	QueryBuilder::~QueryBuilder() = default;
	QueryBuilder::QueryBuilder(QueryBuilder &&) noexcept = default;
	QueryBuilder &QueryBuilder::operator=(QueryBuilder &&) noexcept = default;

	QueryBuilder &QueryBuilder::match(const std::string &variable, const std::string &label, const std::string &key,
									 const Value &value) {
		return match(variable, label.empty() ? std::vector<std::string>{} : std::vector<std::string>{label}, key, value);
	}

	QueryBuilder &QueryBuilder::match(const std::string &variable, const std::vector<std::string> &labels,
									 const std::string &key, const Value &value) {
		auto &builder = detail::requireQueryBuilderImpl(*this);
		detail::recordIdentifier(builder, detail::PreparedIdentifierKind::PIK_VARIABLE, variable);
		detail::recordIdentifiers(builder, detail::PreparedIdentifierKind::PIK_LABEL, labels);
		if (!key.empty()) detail::recordIdentifier(builder, detail::PreparedIdentifierKind::PIK_PROPERTY_KEY, key);
		auto scan = std::make_unique<graph::query::logical::LogicalNodeScan>(variable, labels);
		if (!key.empty()) {
			scan->setParameterPropertyPredicates({{key, detail::bindValue(builder, value)}});
		}
		if (builder.plan.root) {
			builder.plan.root = std::make_unique<graph::query::logical::LogicalJoin>(std::move(builder.plan.root), std::move(scan));
		} else {
			builder.plan.root = std::move(scan);
		}
		return *this;
	}

	QueryBuilder &QueryBuilder::where(const std::string &variable, const std::string &key, const Value &value) {
		auto &builder = detail::requireQueryBuilderImpl(*this);
		detail::requirePipelineRoot(builder, "WHERE");
		detail::recordIdentifier(builder, detail::PreparedIdentifierKind::PIK_VARIABLE, variable);
		detail::recordIdentifier(builder, detail::PreparedIdentifierKind::PIK_PROPERTY_KEY, key);
		const std::string parameter = detail::bindValue(builder, value);
		auto predicate = std::make_shared<graph::query::expressions::BinaryOpExpression>(
				std::make_unique<graph::query::expressions::VariableReferenceExpression>(variable, key),
				graph::query::expressions::BinaryOperatorType::BOP_EQUAL,
				std::make_unique<graph::query::expressions::ParameterExpression>(parameter));
		builder.plan.root = std::make_unique<graph::query::logical::LogicalFilter>(std::move(builder.plan.root), std::move(predicate));
		return *this;
	}

	QueryBuilder &QueryBuilder::createNode(const std::string &variable, const std::string &label,
										const std::unordered_map<std::string, Value> &properties) {
		return createNode(variable, label.empty() ? std::vector<std::string>{} : std::vector<std::string>{label}, properties);
	}

	QueryBuilder &QueryBuilder::createNode(const std::string &variable, const std::vector<std::string> &labels,
										const std::unordered_map<std::string, Value> &properties) {
		auto &builder = detail::requireQueryBuilderImpl(*this);
		detail::recordIdentifier(builder, detail::PreparedIdentifierKind::PIK_VARIABLE, variable);
		detail::recordIdentifiers(builder, detail::PreparedIdentifierKind::PIK_LABEL, labels);
		for (const auto &[key, ignored]: properties) {
			static_cast<void>(ignored);
			detail::recordIdentifier(builder, detail::PreparedIdentifierKind::PIK_PROPERTY_KEY, key);
		}
		auto operation = std::make_unique<graph::query::logical::LogicalCreateNode>(
				variable, labels, std::unordered_map<std::string, graph::PropertyValue>{}, detail::bindProperties(builder, properties));
		detail::appendPipeline(builder, std::move(operation));
		builder.plan.mutatesData = true;
		builder.plan.cacheable = false;
		return *this;
	}

	QueryBuilder &QueryBuilder::createEdge(const std::string &variable, const std::string &type,
										const std::string &sourceVariable, const std::string &targetVariable,
										const std::unordered_map<std::string, Value> &properties) {
		auto &builder = detail::requireQueryBuilderImpl(*this);
		detail::recordIdentifier(builder, detail::PreparedIdentifierKind::PIK_VARIABLE, variable);
		detail::recordIdentifier(builder, detail::PreparedIdentifierKind::PIK_RELATIONSHIP_TYPE, type);
		detail::recordIdentifier(builder, detail::PreparedIdentifierKind::PIK_VARIABLE, sourceVariable);
		detail::recordIdentifier(builder, detail::PreparedIdentifierKind::PIK_VARIABLE, targetVariable);
		for (const auto &[key, ignored]: properties) {
			static_cast<void>(ignored);
			detail::recordIdentifier(builder, detail::PreparedIdentifierKind::PIK_PROPERTY_KEY, key);
		}
		auto operation = std::make_unique<graph::query::logical::LogicalCreateEdge>(
				variable, type, std::unordered_map<std::string, graph::PropertyValue>{}, detail::bindProperties(builder, properties),
				sourceVariable, targetVariable);
		detail::appendPipeline(builder, std::move(operation));
		builder.plan.mutatesData = true;
		builder.plan.cacheable = false;
		return *this;
	}

	QueryBuilder &QueryBuilder::deleteEntities(const std::vector<std::string> &variables, const bool detach) {
		auto &builder = detail::requireQueryBuilderImpl(*this);
		detail::requirePipelineRoot(builder, "DELETE");
		if (variables.empty()) {
			detail::recordBuilderError(builder, "QueryBuilder DELETE requires at least one variable");
		}
		detail::recordIdentifiers(builder, detail::PreparedIdentifierKind::PIK_VARIABLE, variables);
		builder.plan.root = std::make_unique<graph::query::logical::LogicalDelete>(variables, detach, std::move(builder.plan.root));
		builder.plan.mutatesData = true;
		builder.plan.cacheable = false;
		return *this;
	}

	QueryBuilder &QueryBuilder::setProperty(const std::string &variable, const std::string &key, const Value &value) {
		auto &builder = detail::requireQueryBuilderImpl(*this);
		detail::requirePipelineRoot(builder, "SET");
		detail::recordIdentifier(builder, detail::PreparedIdentifierKind::PIK_VARIABLE, variable);
		detail::recordIdentifier(builder, detail::PreparedIdentifierKind::PIK_PROPERTY_KEY, key);
		const std::string parameter = detail::bindValue(builder, value);
		std::vector<graph::query::logical::LogicalSetItem> items;
		items.push_back({graph::query::logical::SetActionType::LSET_PROPERTY, variable, key,
						 std::make_shared<graph::query::expressions::ParameterExpression>(parameter)});
		builder.plan.root = std::make_unique<graph::query::logical::LogicalSet>(std::move(items), std::move(builder.plan.root));
		builder.plan.mutatesData = true;
		builder.plan.cacheable = false;
		return *this;
	}

	QueryBuilder &QueryBuilder::addLabel(const std::string &variable, const std::string &label) {
		auto &builder = detail::requireQueryBuilderImpl(*this);
		detail::requirePipelineRoot(builder, "SET");
		detail::recordIdentifier(builder, detail::PreparedIdentifierKind::PIK_VARIABLE, variable);
		detail::recordIdentifier(builder, detail::PreparedIdentifierKind::PIK_LABEL, label);
		std::vector<graph::query::logical::LogicalSetItem> items;
		items.push_back({graph::query::logical::SetActionType::LSET_LABEL, variable, label, nullptr});
		builder.plan.root = std::make_unique<graph::query::logical::LogicalSet>(std::move(items), std::move(builder.plan.root));
		builder.plan.mutatesData = true;
		builder.plan.cacheable = false;
		return *this;
	}

	QueryBuilder &QueryBuilder::remove(const std::string &variable, const std::string &key, const bool isLabel) {
		auto &builder = detail::requireQueryBuilderImpl(*this);
		detail::requirePipelineRoot(builder, "REMOVE");
		detail::recordIdentifier(builder, detail::PreparedIdentifierKind::PIK_VARIABLE, variable);
		detail::recordIdentifier(builder, isLabel ? detail::PreparedIdentifierKind::PIK_LABEL
													: detail::PreparedIdentifierKind::PIK_PROPERTY_KEY, key);
		std::vector<graph::query::logical::LogicalRemoveItem> items;
		items.push_back({isLabel ? graph::query::logical::LogicalRemoveActionType::LREM_LABEL
									 : graph::query::logical::LogicalRemoveActionType::LREM_PROPERTY,
						 variable, key});
		builder.plan.root = std::make_unique<graph::query::logical::LogicalRemove>(std::move(items), std::move(builder.plan.root));
		builder.plan.mutatesData = true;
		builder.plan.cacheable = false;
		return *this;
	}

	QueryBuilder &QueryBuilder::createIndex(const std::string &label, const std::string &property, const std::string &indexName) {
		auto &builder = detail::requireQueryBuilderImpl(*this);
		if (builder.plan.root) {
			detail::recordBuilderError(builder, "QueryBuilder schema operations must be standalone");
			return *this;
		}
		detail::recordIdentifier(builder, detail::PreparedIdentifierKind::PIK_LABEL, label);
		detail::recordIdentifier(builder, detail::PreparedIdentifierKind::PIK_PROPERTY_KEY, property);
		detail::recordIdentifier(builder, detail::PreparedIdentifierKind::PIK_INDEX_NAME, indexName);
		builder.plan.root = std::make_unique<graph::query::logical::LogicalCreateIndex>(indexName, label, property);
		builder.plan.mutatesSchema = true;
		builder.plan.cacheable = false;
		builder.plan.optimizable = false;
		return *this;
	}

	QueryBuilder &QueryBuilder::dropIndex(const std::string &indexName) {
		auto &builder = detail::requireQueryBuilderImpl(*this);
		if (builder.plan.root) {
			detail::recordBuilderError(builder, "QueryBuilder schema operations must be standalone");
			return *this;
		}
		if (indexName.empty()) {
			detail::recordBuilderError(builder, "QueryBuilder DROP INDEX requires a nonempty index name");
		}
		detail::recordIdentifier(builder, detail::PreparedIdentifierKind::PIK_INDEX_NAME, indexName);
		builder.plan.root = std::make_unique<graph::query::logical::LogicalDropIndex>(indexName, "", "");
		builder.plan.mutatesSchema = true;
		builder.plan.cacheable = false;
		builder.plan.optimizable = false;
		return *this;
	}

	QueryBuilder &QueryBuilder::dropIndex(const std::string &label, const std::string &property) {
		auto &builder = detail::requireQueryBuilderImpl(*this);
		if (builder.plan.root) {
			detail::recordBuilderError(builder, "QueryBuilder schema operations must be standalone");
			return *this;
		}
		detail::recordIdentifier(builder, detail::PreparedIdentifierKind::PIK_LABEL, label);
		detail::recordIdentifier(builder, detail::PreparedIdentifierKind::PIK_PROPERTY_KEY, property);
		builder.plan.root = std::make_unique<graph::query::logical::LogicalDropIndex>("", label, property);
		builder.plan.mutatesSchema = true;
		builder.plan.cacheable = false;
		builder.plan.optimizable = false;
		return *this;
	}

	QueryBuilder &QueryBuilder::showIndexes() {
		auto &builder = detail::requireQueryBuilderImpl(*this);
		if (builder.plan.root) {
			detail::recordBuilderError(builder, "QueryBuilder schema operations must be standalone");
			return *this;
		}
		builder.plan.root = std::make_unique<graph::query::logical::LogicalShowIndexes>();
		builder.plan.cacheable = false;
		builder.plan.optimizable = false;
		return *this;
	}

	QueryBuilder &QueryBuilder::call(const std::string &procedure, const std::vector<Value> &arguments) {
		auto &builder = detail::requireQueryBuilderImpl(*this);
		if (builder.plan.root) {
			detail::recordBuilderError(builder, "QueryBuilder CALL must be the first clause");
			return *this;
		}
		detail::recordIdentifier(builder, detail::PreparedIdentifierKind::PIK_PROCEDURE, procedure);
		std::vector<std::string> parameterNames;
		parameterNames.reserve(arguments.size());
		for (const Value &argument: arguments) parameterNames.push_back(detail::bindValue(builder, argument));
		builder.plan.root = graph::query::logical::LogicalCallProcedure::withBoundArguments(
				procedure, std::move(parameterNames));
		builder.plan.cacheable = false;
		builder.plan.optimizable = false;
		return *this;
	}

	QueryBuilder &QueryBuilder::returning(const std::vector<std::string> &variables) {
		auto &builder = detail::requireQueryBuilderImpl(*this);
		detail::requirePipelineRoot(builder, "RETURN");
		if (variables.empty()) {
			detail::recordBuilderError(builder, "QueryBuilder RETURN requires at least one variable");
		}
		detail::recordIdentifiers(builder, detail::PreparedIdentifierKind::PIK_VARIABLE, variables);
		std::vector<graph::query::logical::LogicalProjectItem> items;
		items.reserve(variables.size());
		for (const std::string &variable: variables) {
			items.emplace_back(std::make_shared<graph::query::expressions::VariableReferenceExpression>(variable), variable);
		}
		builder.plan.root = std::make_unique<graph::query::logical::LogicalProject>(std::move(builder.plan.root), std::move(items));
		return *this;
	}

	QueryBuilder &QueryBuilder::skip(const int64_t offset) {
		auto &builder = detail::requireQueryBuilderImpl(*this);
		detail::requirePipelineRoot(builder, "SKIP");
		if (offset < 0) {
			detail::recordBuilderError(builder, "QueryBuilder SKIP cannot be negative");
		}
		builder.plan.root = std::make_unique<graph::query::logical::LogicalSkip>(std::move(builder.plan.root), offset);
		return *this;
	}

	QueryBuilder &QueryBuilder::limit(const int64_t maximumRows) {
		auto &builder = detail::requireQueryBuilderImpl(*this);
		detail::requirePipelineRoot(builder, "LIMIT");
		if (maximumRows < 0) {
			detail::recordBuilderError(builder, "QueryBuilder LIMIT cannot be negative");
		}
		builder.plan.root = std::make_unique<graph::query::logical::LogicalLimit>(std::move(builder.plan.root), maximumRows);
		return *this;
	}

	QueryBuilder &QueryBuilder::orderBy(const std::vector<SortOrder> &items) {
		auto &builder = detail::requireQueryBuilderImpl(*this);
		detail::requirePipelineRoot(builder, "ORDER BY");
		if (items.empty()) {
			detail::recordBuilderError(builder, "QueryBuilder ORDER BY requires at least one sort item");
		}
		std::vector<graph::query::logical::LogicalSortItem> logicalItems;
		logicalItems.reserve(items.size());
		for (const SortOrder &item: items) {
			detail::recordIdentifier(builder, detail::PreparedIdentifierKind::PIK_VARIABLE, item.variable);
			detail::recordIdentifier(builder, detail::PreparedIdentifierKind::PIK_PROPERTY_KEY, item.property);
			logicalItems.emplace_back(std::make_shared<graph::query::expressions::VariableReferenceExpression>(item.variable, item.property),
								  item.ascending);
		}
		builder.plan.root = std::make_unique<graph::query::logical::LogicalSort>(std::move(builder.plan.root), std::move(logicalItems));
		return *this;
	}

	QueryBuilder &QueryBuilder::unwind(const std::vector<Value> &values, const std::string &alias) {
		auto &builder = detail::requireQueryBuilderImpl(*this);
		detail::recordIdentifier(builder, detail::PreparedIdentifierKind::PIK_VARIABLE, alias);
		std::vector<graph::PropertyValue> internalValues;
		internalValues.reserve(values.size());
		for (const Value &value: values) internalValues.push_back(detail::toPropertyValue(value));
		if (!builder.plan.root) builder.plan.root = std::make_unique<graph::query::logical::LogicalSingleRow>();
		builder.plan.root = std::make_unique<graph::query::logical::LogicalUnwind>(std::move(builder.plan.root), alias,
															std::move(internalValues));
		return *this;
	}

	PreparedQuery QueryBuilder::build() const {
		if (!impl_) throw std::logic_error("QueryBuilder cannot build after it has been moved");
		if (impl_->build_error.has_value()) throw std::logic_error(*impl_->build_error);
		detail::validatePreparedPlan(*impl_);
		auto prepared = std::make_unique<detail::PreparedQueryImpl>();
		prepared->plan = impl_->plan.clone();
		prepared->context = impl_->context;
		return PreparedQuery(std::move(prepared));
	}

} // namespace zyx
