/**
 * @file LogicalPlanMutationRuntime.cpp
 * @brief Implementation of the relocatable logical-plan mutation compiler.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#include "graph/query/execution/LogicalPlanMutationRuntime.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "graph/core/Types.hpp"
#include "graph/query/QueryPlan.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/expressions/ExpressionUtils.hpp"
#include "graph/query/logical/LogicalOperator.hpp"
#include "graph/query/logical/operators/LogicalAggregate.hpp"
#include "graph/query/logical/operators/LogicalCallSubquery.hpp"
#include "graph/query/logical/operators/LogicalCreateEdge.hpp"
#include "graph/query/logical/operators/LogicalCreateNode.hpp"
#include "graph/query/logical/operators/LogicalDelete.hpp"
#include "graph/query/logical/operators/LogicalForeach.hpp"
#include "graph/query/logical/operators/LogicalMergeEdge.hpp"
#include "graph/query/logical/operators/LogicalMergeNode.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/logical/operators/LogicalProject.hpp"
#include "graph/query/logical/operators/LogicalRemove.hpp"
#include "graph/query/logical/operators/LogicalSet.hpp"
#include "graph/query/logical/operators/LogicalSingleRow.hpp"
#include "graph/query/logical/operators/LogicalUnwind.hpp"

namespace graph::query::execution {

	namespace {

		using storage::LogicalGraphBatchEdgeCreateRequest;
		using storage::LogicalGraphMutationBatch;
		using storage::LogicalGraphMutationEntityKind;
		using storage::LogicalGraphMutationEntityReference;
		using storage::LogicalGraphMutationReferenceKind;
		using storage::LogicalGraphNodeCreateRequest;
		using storage::LogicalGraphPropertyMutationRequest;
		using storage::LogicalGraphWriteProperty;
		using PropertyMap = std::unordered_map<std::string, PropertyValue>;

		[[nodiscard]] LogicalGraphMutationEntityReference createdNodeReference(size_t ordinal) {
			return {LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE,
					static_cast<uint64_t>(ordinal)};
		}

		[[nodiscard]] std::vector<LogicalGraphWriteProperty>
		toLogicalProperties(const std::unordered_map<std::string, PropertyValue> &properties) {
			std::vector<std::pair<std::string, PropertyValue>> ordered;
			ordered.reserve(properties.size());
			for (const auto &[key, value]: properties) {
				ordered.emplace_back(key, value);
			}
			std::ranges::sort(ordered, {}, &std::pair<std::string, PropertyValue>::first);
			std::vector<LogicalGraphWriteProperty> result;
			result.reserve(ordered.size());
			for (const auto &[key, value]: ordered) {
				result.push_back({key, value});
			}
			return result;
		}

		[[nodiscard]] const logical::LogicalOperator *requireOptionalOnlyChild(const logical::LogicalOperator &op) {
			const std::vector<logical::LogicalOperator *> children = op.getChildren();
			if (children.empty()) {
				return nullptr;
			}
			if (children.size() != 1U || children.front() == nullptr) {
				throw std::runtime_error("logical mutation runtime requires unary CREATE operators");
			}
			return children.front();
		}

		struct CreateSetMutationChain {
			std::vector<const logical::LogicalOperator *> operations;
			const logical::LogicalOperator *read_root = nullptr;
		};

		struct ReadPropertyMutationChain {
			std::vector<const logical::LogicalOperator *> operations;
			const logical::LogicalOperator *read_root = nullptr;
		};

		[[nodiscard]] CreateSetMutationChain collectForeachMutationChain(const logical::LogicalForeach &foreach) {
			if (foreach.getIterVar().empty() || !foreach.getListExpr() || foreach.getBody() == nullptr) {
				throw std::logic_error(
						"logical FOREACH requires an iteration variable, list expression, and update body");
			}
			CreateSetMutationChain chain;
			const logical::LogicalOperator *tail = foreach.getBody();
			for (;;) {
				const logical::LogicalOpType type = tail->getType();
				if (type != logical::LogicalOpType::LOP_CREATE_NODE &&
					type != logical::LogicalOpType::LOP_CREATE_EDGE && type != logical::LogicalOpType::LOP_SET &&
					type != logical::LogicalOpType::LOP_REMOVE) {
					throw std::logic_error("logical FOREACH currently supports only CREATE Node/relationship, SET, and "
										   "REMOVE update chains");
				}
				if (type == logical::LogicalOpType::LOP_SET &&
					dynamic_cast<const logical::LogicalSet *>(tail)->getItems().empty()) {
					throw std::logic_error("logical FOREACH SET body cannot be empty");
				}
				if (type == logical::LogicalOpType::LOP_REMOVE &&
					dynamic_cast<const logical::LogicalRemove *>(tail)->getItems().empty()) {
					throw std::logic_error("logical FOREACH REMOVE body cannot be empty");
				}
				chain.operations.push_back(tail);
				const std::vector<logical::LogicalOperator *> children = tail->getChildren();
				if (children.empty()) {
					break;
				}
				if (children.size() != 1U || children.front() == nullptr) {
					throw std::logic_error("logical FOREACH update body must be a unary operator chain");
				}
				if (children.front()->getType() == logical::LogicalOpType::LOP_SINGLE_ROW) {
					if (!children.front()->getChildren().empty()) {
						throw std::logic_error("logical FOREACH update body has a malformed SingleRow seed");
					}
					break;
				}
				tail = children.front();
			}
			if (chain.operations.empty()) {
				throw std::logic_error("logical FOREACH update body cannot be empty");
			}
			std::ranges::reverse(chain.operations);
			chain.read_root = foreach.getInput();
			return chain;
		}

		[[nodiscard]] std::optional<CreateSetMutationChain>
		collectCreateSetMutationChain(const logical::LogicalOperator &root) {
			CreateSetMutationChain chain;
			const logical::LogicalOperator *current = &root;
			bool sawCreate = false;
			while (current != nullptr && (current->getType() == logical::LogicalOpType::LOP_CREATE_NODE ||
										  current->getType() == logical::LogicalOpType::LOP_CREATE_EDGE ||
										  current->getType() == logical::LogicalOpType::LOP_SET ||
										  current->getType() == logical::LogicalOpType::LOP_REMOVE ||
										  current->getType() == logical::LogicalOpType::LOP_DELETE)) {
				chain.operations.push_back(current);
				sawCreate = sawCreate || current->getType() == logical::LogicalOpType::LOP_CREATE_NODE ||
							current->getType() == logical::LogicalOpType::LOP_CREATE_EDGE;
				current = requireOptionalOnlyChild(*current);
			}
			if (!sawCreate || chain.operations.empty()) {
				return std::nullopt;
			}
			std::ranges::reverse(chain.operations);
			chain.read_root = current;
			return chain;
		}

		[[nodiscard]] std::optional<ReadPropertyMutationChain>
		collectReadPropertyMutationChain(const logical::LogicalOperator &root) {
			ReadPropertyMutationChain chain;
			const logical::LogicalOperator *current = &root;
			while (current != nullptr && (current->getType() == logical::LogicalOpType::LOP_SET ||
										  current->getType() == logical::LogicalOpType::LOP_REMOVE)) {
				chain.operations.push_back(current);
				current = requireOptionalOnlyChild(*current);
			}
			// SET and REMOVE have the same snapshot, staging, retry, and result
			// semantics whether a statement contains one update or a chain. Keeping
			// them on one path prevents the two implementations from drifting.
			if (chain.operations.empty() || current == nullptr) {
				return std::nullopt;
			}
			// MERGE owns its post-match SET/REMOVE chain so it can stage a single
			// atomic create-or-update batch. It is not a read-only chain input.
			if (current->getType() == logical::LogicalOpType::LOP_MERGE_NODE ||
				current->getType() == logical::LogicalOpType::LOP_MERGE_EDGE) {
				return std::nullopt;
			}
			std::ranges::reverse(chain.operations);
			chain.read_root = current;
			return chain;
		}

		// A write-only CALL body is planned with its own SingleRow seed. Rebind that
		// seed to the outer pipeline so the existing bounded CREATE runtime can stage
		// every outer row and publish one atomic batch. General CALL semantics stay
		// outside this deliberately narrow mutation path.
		[[nodiscard]] std::unique_ptr<logical::LogicalOperator>
		rebindWriteOnlyCallSubquery(const logical::LogicalOperator &root) {
			const logical::LogicalProject *outerProjection = nullptr;
			const logical::LogicalOperator *candidate = &root;
			if (root.getType() == logical::LogicalOpType::LOP_PROJECT) {
				outerProjection = dynamic_cast<const logical::LogicalProject *>(&root);
				if (outerProjection == nullptr) {
					throw std::logic_error("logical write-only CALL has an invalid Project operator");
				}
				candidate = requireOptionalOnlyChild(root);
				if (candidate == nullptr) {
					throw std::logic_error("logical write-only CALL projection requires a child");
				}
			}
			if (candidate->getType() != logical::LogicalOpType::LOP_CALL_SUBQUERY) {
				return nullptr;
			}

			const auto *call = dynamic_cast<const logical::LogicalCallSubquery *>(candidate);
			if (call == nullptr) {
				throw std::logic_error("logical write-only CALL has an invalid CallSubquery operator");
			}
			if (call->isInTransactions() || call->getBatchSize() != 0 || !call->getImportedVars().empty() ||
				!call->getReturnedVars().empty()) {
				throw std::runtime_error("logical mutation runtime does not support CALL subqueries with imports, "
										 "returned variables, or transaction batching");
			}
			if (call->getSubquery() == nullptr) {
				throw std::logic_error("logical write-only CALL requires an update body");
			}

			std::unique_ptr<logical::LogicalOperator> reboundBody = call->getSubquery()->clone();
			logical::LogicalOperator *current = reboundBody.get();
			bool sawCreate = false;
			for (;;) {
				if (current == nullptr) {
					throw std::logic_error("logical write-only CALL update body contains a null operator");
				}
				switch (current->getType()) {
					case logical::LogicalOpType::LOP_CREATE_NODE:
					case logical::LogicalOpType::LOP_CREATE_EDGE:
						sawCreate = true;
						break;
					case logical::LogicalOpType::LOP_SET:
					case logical::LogicalOpType::LOP_REMOVE:
						break;
					default:
						throw std::logic_error("logical write-only CALL supports only CREATE, SET, and REMOVE "
											   "update chains");
				}

				const std::vector<logical::LogicalOperator *> children = current->getChildren();
				if (children.size() != 1U || children.front() == nullptr) {
					throw std::logic_error(
							"logical write-only CALL update body must be a unary chain ending in SingleRow");
				}
				if (children.front()->getType() == logical::LogicalOpType::LOP_SINGLE_ROW) {
					if (!children.front()->getChildren().empty()) {
						throw std::logic_error("logical write-only CALL update body has a malformed SingleRow seed");
					}
					current->setChild(0U, call->getInput() != nullptr ? call->getInput()->clone()
																	  : std::make_unique<logical::LogicalSingleRow>());
					break;
				}
				current = children.front();
			}
			if (!sawCreate) {
				throw std::logic_error("logical write-only CALL update body requires a CREATE operation");
			}

			if (outerProjection == nullptr) {
				return reboundBody;
			}
			std::unique_ptr<logical::LogicalOperator> reboundRoot = outerProjection->clone();
			reboundRoot->setChild(0U, std::move(reboundBody));
			return reboundRoot;
		}

		[[nodiscard]] PropertyMap
		propertyMapFromLogicalProperties(const std::vector<LogicalGraphWriteProperty> &properties,
										 const char *operation) {
			PropertyMap result;
			result.reserve(properties.size());
			for (const LogicalGraphWriteProperty &property: properties) {
				if (property.key.empty() || !result.emplace(property.key, property.value).second) {
					throw std::logic_error(std::string(operation) + " has an invalid complete Property collection");
				}
			}
			return result;
		}

		[[nodiscard]] std::vector<LogicalGraphWriteProperty> sortedProperties(const PropertyMap &properties) {
			std::vector<LogicalGraphWriteProperty> result;
			result.reserve(properties.size());
			for (const auto &[key, value]: properties) {
				if (key.empty()) {
					throw std::logic_error("logical mutation has an empty Property key");
				}
				result.push_back({key, value});
			}
			std::ranges::sort(result, {}, &LogicalGraphWriteProperty::key);
			return result;
		}

		[[nodiscard]] PropertyMap readInlineProperties(const std::vector<storage::LogicalGraphPropertyEntry> &entries,
													   size_t maximumEntries) {
			if (entries.size() > maximumEntries) {
				throw std::runtime_error("logical mutation projection read exceeded its direct Property bound");
			}
			PropertyMap result;
			result.reserve(entries.size());
			for (const storage::LogicalGraphPropertyEntry &entry: entries) {
				if (entry.key.empty() || !result.emplace(entry.key, entry.value).second) {
					throw std::runtime_error("logical mutation projection read an invalid direct Property entry set");
				}
			}
			return result;
		}

		[[nodiscard]] PropertyMap readNodeProperties(const storage::LogicalGraphReadSnapshot &snapshot,
													 const storage::LogicalGraphNode &node, size_t maximumEntries) {
			static_cast<void>(snapshot);
			return readInlineProperties(node.properties, maximumEntries);
		}

		[[nodiscard]] PropertyMap readEdgeProperties(const storage::LogicalGraphReadSnapshot &snapshot,
													 const storage::LogicalGraphEdge &edge, size_t maximumEntries) {
			static_cast<void>(snapshot);
			return readInlineProperties(edge.properties, maximumEntries);
		}

		[[nodiscard]] PropertyValue
		evaluateCreatePropertyExpression(const expressions::Expression &expression, const QueryContext &context,
										 const LogicalPlanReadRow &row, storage::LogicalGraphReadSnapshot &snapshot,
										 const LogicalPlanReadRuntimeBounds &bounds,
										 std::unordered_map<std::string, PropertyMap> &propertyCache) {
			LogicalPlanReadRow evaluationRow = row;
			evaluationRow.property_cache = propertyCache;
			const LogicalPlanReadValue value = LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(
					snapshot, expression, evaluationRow, bounds, context);
			propertyCache = std::move(evaluationRow.property_cache);
			if (const auto *property = std::get_if<PropertyValue>(&value)) {
				return *property;
			}
			throw std::logic_error("logical mutation Property assignment must evaluate to a Property value");
		}

		void requireCompleteReadForMutation(const LogicalPlanReadRuntimeStats &stats,
											const LogicalPlanReadRuntimeBounds &bounds, const char *operation) {
			if (stats.rows_emitted == bounds.maximum_rows || stats.nodes_scanned == bounds.maximum_node_records ||
				stats.edges_scanned == bounds.maximum_edges_per_node) {
				throw std::runtime_error(std::string(operation) +
										 " reached a read bound before proving its input was exhausted");
			}
		}

		struct MutableGraphEntity {
			LogicalGraphMutationEntityKind kind = LogicalGraphMutationEntityKind::LGMK_NODE;
			uint64_t id = 0U;
			std::vector<storage::LogicalGraphPropertyEntry> direct_properties;
			std::optional<PropertyMap> properties;
			std::vector<std::string> labels;
			bool properties_changed = false;
			bool labels_changed = false;
		};

		using MutableGraphEntities = std::map<std::pair<uint8_t, uint64_t>, MutableGraphEntity>;

		[[nodiscard]] MutableGraphEntity &mutableEntityForValue(MutableGraphEntities &entities,
																const LogicalPlanReadValue &value) {
			if (const auto *node = std::get_if<storage::LogicalGraphNode>(&value)) {
				const auto key =
						std::pair{static_cast<uint8_t>(LogicalGraphMutationEntityKind::LGMK_NODE), node->node_id};
				auto [iterator, inserted] = entities.try_emplace(key);
				if (inserted) {
					iterator->second.kind = LogicalGraphMutationEntityKind::LGMK_NODE;
					iterator->second.id = node->node_id;
					iterator->second.direct_properties = node->properties;
					iterator->second.labels = node->labels;
				}
				return iterator->second;
			}
			if (const auto *edge = std::get_if<storage::LogicalGraphEdge>(&value)) {
				const auto key =
						std::pair{static_cast<uint8_t>(LogicalGraphMutationEntityKind::LGMK_EDGE), edge->edge_id};
				auto [iterator, inserted] = entities.try_emplace(key);
				if (inserted) {
					iterator->second.kind = LogicalGraphMutationEntityKind::LGMK_EDGE;
					iterator->second.id = edge->edge_id;
					iterator->second.direct_properties = edge->properties;
				}
				return iterator->second;
			}
			throw std::logic_error("logical mutation requires Node or Edge bindings");
		}

		[[nodiscard]] PropertyMap &mutableEntityProperties(MutableGraphEntity &entity,
														   const storage::LogicalGraphReadSnapshot &snapshot,
														   const LogicalPlanReadRuntimeBounds &bounds) {
			if (!entity.properties.has_value()) {
				static_cast<void>(snapshot);
				entity.properties = readInlineProperties(entity.direct_properties, bounds.maximum_property_entries);
			}
			return *entity.properties;
		}

		void synchronizeEntityPropertyCache(const LogicalPlanReadRow &row, const MutableGraphEntity &entity,
											const PropertyMap &properties,
											std::unordered_map<std::string, PropertyMap> &propertyCache) {
			for (const auto &[variable, value]: row.values) {
				const auto *node = std::get_if<storage::LogicalGraphNode>(&value);
				const auto *edge = std::get_if<storage::LogicalGraphEdge>(&value);
				const bool matchesNode = node != nullptr && entity.kind == LogicalGraphMutationEntityKind::LGMK_NODE &&
										 node->node_id == entity.id;
				const bool matchesEdge = edge != nullptr && entity.kind == LogicalGraphMutationEntityKind::LGMK_EDGE &&
										 edge->edge_id == entity.id;
				if (matchesNode || matchesEdge) {
					propertyCache.insert_or_assign(variable, properties);
				}
			}
		}

		void applySetOperation(const logical::LogicalSet &setOperator, const LogicalPlanReadRow &row,
							   storage::LogicalGraphReadSnapshot &snapshot, const LogicalPlanReadRuntimeBounds &bounds,
							   const QueryContext &context, MutableGraphEntities &entities,
							   std::unordered_map<std::string, PropertyMap> &propertyCache) {
			if (setOperator.getItems().empty()) {
				throw std::logic_error("logical SET requires at least one assignment");
			}
			for (const logical::LogicalSetItem &item: setOperator.getItems()) {
				const auto binding = row.values.find(item.variable);
				if (item.variable.empty() || binding == row.values.end()) {
					throw std::logic_error("logical SET could not resolve a requested entity variable");
				}
				MutableGraphEntity &entity = mutableEntityForValue(entities, binding->second);
				switch (item.type) {
					case logical::SetActionType::LSET_PROPERTY: {
						if (item.key.empty() || !item.expression) {
							throw std::logic_error("logical SET Property assignment is malformed");
						}
						PropertyMap &properties = mutableEntityProperties(entity, snapshot, bounds);
						synchronizeEntityPropertyCache(row, entity, properties, propertyCache);
						const PropertyValue value = evaluateCreatePropertyExpression(*item.expression, context, row,
																					 snapshot, bounds, propertyCache);
						if (value.getType() == PropertyType::NULL_TYPE) {
							if (properties.erase(item.key) != 0U) {
								entity.properties_changed = true;
							}
						} else {
							const auto existing = properties.find(item.key);
							if (existing == properties.end() || existing->second != value) {
								properties.insert_or_assign(item.key, value);
								entity.properties_changed = true;
							}
						}
						synchronizeEntityPropertyCache(row, entity, properties, propertyCache);
						break;
					}
					case logical::SetActionType::LSET_LABEL:
						if (entity.kind != LogicalGraphMutationEntityKind::LGMK_NODE || item.key.empty()) {
							throw std::logic_error("logical SET label assignment requires a Node and nonempty label");
						}
						if (std::ranges::find(entity.labels, item.key) == entity.labels.end()) {
							entity.labels.push_back(item.key);
							entity.labels_changed = true;
						}
						break;
					case logical::SetActionType::LSET_MAP_MERGE: {
						if (!item.expression) {
							throw std::logic_error("logical SET map-merge assignment is malformed");
						}
						PropertyMap &properties = mutableEntityProperties(entity, snapshot, bounds);
						synchronizeEntityPropertyCache(row, entity, properties, propertyCache);
						const PropertyValue value = evaluateCreatePropertyExpression(*item.expression, context, row,
																					 snapshot, bounds, propertyCache);
						if (value.getType() != PropertyType::MAP) {
							throw std::logic_error("logical SET map-merge assignment requires a map value");
						}
						for (const auto &[key, mergedValue]: value.getMap()) {
							if (key.empty()) {
								throw std::logic_error("logical SET map-merge assignment has an empty Property key");
							}
							if (mergedValue.getType() == PropertyType::NULL_TYPE) {
								if (properties.erase(key) != 0U) {
									entity.properties_changed = true;
								}
							} else {
								const auto existing = properties.find(key);
								if (existing == properties.end() || existing->second != mergedValue) {
									properties.insert_or_assign(key, mergedValue);
									entity.properties_changed = true;
								}
							}
						}
						synchronizeEntityPropertyCache(row, entity, properties, propertyCache);
						break;
					}
				}
			}
		}

		void applyRemoveOperation(const logical::LogicalRemove &removeOperator, const LogicalPlanReadRow &row,
								  storage::LogicalGraphReadSnapshot &snapshot,
								  const LogicalPlanReadRuntimeBounds &bounds, MutableGraphEntities &entities,
								  std::unordered_map<std::string, PropertyMap> &propertyCache) {
			if (removeOperator.getItems().empty()) {
				throw std::logic_error("logical REMOVE requires at least one item");
			}
			for (const logical::LogicalRemoveItem &item: removeOperator.getItems()) {
				const auto binding = row.values.find(item.variable);
				if (item.variable.empty() || item.key.empty() || binding == row.values.end()) {
					throw std::logic_error("logical REMOVE could not resolve a valid entity item");
				}
				MutableGraphEntity &entity = mutableEntityForValue(entities, binding->second);
				switch (item.type) {
					case logical::LogicalRemoveActionType::LREM_PROPERTY: {
						PropertyMap &properties = mutableEntityProperties(entity, snapshot, bounds);
						if (properties.erase(item.key) != 0U) {
							entity.properties_changed = true;
						}
						synchronizeEntityPropertyCache(row, entity, properties, propertyCache);
						break;
					}
					case logical::LogicalRemoveActionType::LREM_LABEL:
						if (entity.kind == LogicalGraphMutationEntityKind::LGMK_NODE) {
							const auto label = std::ranges::find(entity.labels, item.key);
							if (label != entity.labels.end()) {
								entity.labels.erase(label);
								entity.labels_changed = true;
							}
						}
						break;
				}
			}
		}

		struct CreateSetRuntimeRow {
			LogicalPlanReadRow source;
			std::unordered_map<std::string, LogicalGraphMutationEntityReference> entities;
			// A CREATE followed by DELETE cancels the provisional entity before
			// publication, so an implicit result cannot expose a durable binding.
			std::set<std::string> unavailable_implicit_entities;
		};

		using MutationProjectionResolver = std::function<ResultValue(const LogicalPlanReadRow &, const std::string &)>;

		[[nodiscard]] std::vector<std::string> visibleMutationOutputColumns(const logical::LogicalOperator &root) {
			std::vector<std::string> columns;
			std::set<std::string> seen;
			for (const std::string &column: root.getOutputVariables()) {
				// Match the physical executor: anonymous pattern bindings are never
				// exposed to callers, but named pass-through bindings are result rows.
				if (!column.empty() && !isAnonymousVariable(column) && seen.insert(column).second) {
					columns.push_back(column);
				}
			}
			return columns;
		}

		template<typename Row, typename Resolver>
		[[nodiscard]] QueryResult buildImplicitMutationResult(const std::vector<std::string> &columns,
															  const std::vector<Row> &rows, const QueryContext &context,
															  Resolver &&resolveBinding) {
			QueryResult result;
			result.setColumns(columns);
			// Mutation aggregates consume pipeline rows even when this statement has
			// no client-visible columns (for example, an anonymous CREATE).
			result.setPipelineRowCount(rows.size());
			if (columns.empty()) {
				return result;
			}
			for (const Row &source: rows) {
				QueryResult::Row output;
				output.reserve(columns.size());
				for (const std::string &column: columns) {
					context.checkGuard();
					if (!output.emplace(column, resolveBinding(source, column)).second) {
						throw std::logic_error("logical mutation produced duplicate output columns");
					}
				}
				result.addRow(std::move(output));
			}
			return result;
		}

		[[nodiscard]] ResultValue evaluateMutationProjectionExpression(
				const expressions::Expression &expression, const LogicalPlanReadRow &source,
				storage::LogicalGraphReadSnapshot &snapshot, const LogicalPlanReadRuntimeBounds &bounds,
				const QueryContext &context, const MutationProjectionResolver &resolveBinding);

		[[nodiscard]] QueryResult
		buildCreateSetResult(const logical::LogicalProject *projection, const std::vector<std::string> &implicitColumns,
							 const std::vector<CreateSetRuntimeRow> &rows, storage::LogicalGraphReadSnapshot &snapshot,
							 const LogicalGraphMutationBatch &batch,
							 const storage::LogicalGraphMutationBatchResult &result,
							 const MutableGraphEntities &persistedEntities, const LogicalPlanReadRuntimeBounds &bounds,
							 const QueryContext &context);

		[[nodiscard]] QueryResult executeCreateSetMutationChain(
				const CreateSetMutationChain &chain, const storage::LogicalGraphReadBackend &readBackend,
				storage::LogicalGraphWriteBackend &writeBackend, const LogicalPlanReadRuntimeBounds &bounds,
				const QueryContext &context, const logical::LogicalProject *projection,
				const std::vector<std::string> &implicitColumns, const logical::LogicalForeach *foreach = nullptr) {
			if (chain.operations.empty()) {
				throw std::logic_error("logical CREATE/SET mutation chain cannot be empty");
			}
			size_t createsPerInputRow = 0U;
			for (const logical::LogicalOperator *operation: chain.operations) {
				if (operation == nullptr) {
					throw std::logic_error("logical CREATE/SET mutation chain contains a null operator");
				}
				if (operation->getType() == logical::LogicalOpType::LOP_CREATE_NODE ||
					operation->getType() == logical::LogicalOpType::LOP_CREATE_EDGE) {
					++createsPerInputRow;
				}
			}
			if (createsPerInputRow > std::numeric_limits<size_t>::max() / bounds.maximum_rows) {
				throw std::overflow_error("logical CREATE/SET mutation chain exceeded its staged-create bound");
			}
			const size_t maximumStagedCreates = createsPerInputRow * bounds.maximum_rows;

			const auto appendExpressionProperties = [&](const auto &propertyExpressions,
														std::vector<LogicalGraphWriteProperty> &properties,
														const LogicalPlanReadRow &row,
														storage::LogicalGraphReadSnapshot &snapshot,
														std::unordered_map<std::string, PropertyMap> &propertyCache) {
				std::vector<std::pair<std::string, std::shared_ptr<expressions::Expression>>> ordered;
				ordered.reserve(propertyExpressions.size());
				for (const auto &[key, expression]: propertyExpressions) {
					ordered.emplace_back(key, expression);
				}
				std::ranges::sort(ordered, {},
								  &std::pair<std::string, std::shared_ptr<expressions::Expression>>::first);
				for (const auto &[key, expression]: ordered) {
					if (key.empty() || !expression ||
						std::ranges::any_of(properties, [&key](const LogicalGraphWriteProperty &property) {
							return property.key == key;
						})) {
						throw std::logic_error(
								"logical CREATE/SET has an invalid or duplicate Property expression key");
					}
					properties.push_back({key, evaluateCreatePropertyExpression(*expression, context, row, snapshot,
																				bounds, propertyCache)});
				}
			};

			const auto applyPropertySet = [&](PropertyMap &properties, const logical::LogicalSetItem &item,
											  const LogicalPlanReadRow &row,
											  storage::LogicalGraphReadSnapshot &snapshot,
											  std::unordered_map<std::string, PropertyMap> &propertyCache) {
				if (!item.expression) {
					throw std::logic_error("logical CREATE/SET has a Property assignment without an expression");
				}
				const PropertyValue value = evaluateCreatePropertyExpression(*item.expression, context, row, snapshot,
																			 bounds, propertyCache);
				switch (item.type) {
					case logical::SetActionType::LSET_PROPERTY: {
						if (item.key.empty()) {
							throw std::logic_error("logical CREATE/SET Property assignment has an empty key");
						}
						if (value.getType() == PropertyType::NULL_TYPE) {
							return properties.erase(item.key) != 0U;
						}
						const auto found = properties.find(item.key);
						if (found != properties.end() && found->second == value) {
							return false;
						}
						properties.insert_or_assign(item.key, value);
						return true;
					}
					case logical::SetActionType::LSET_MAP_MERGE: {
						if (value.getType() != PropertyType::MAP) {
							throw std::logic_error("logical CREATE/SET map merge requires a map value");
						}
						bool changed = false;
						for (const auto &[key, mergedValue]: value.getMap()) {
							if (key.empty()) {
								throw std::logic_error("logical CREATE/SET map merge has an empty Property key");
							}
							if (mergedValue.getType() == PropertyType::NULL_TYPE) {
								changed = properties.erase(key) != 0U || changed;
								continue;
							}
							const auto found = properties.find(key);
							if (found == properties.end() || found->second != mergedValue) {
								properties.insert_or_assign(key, mergedValue);
								changed = true;
							}
						}
						return changed;
					}
					case logical::SetActionType::LSET_LABEL:
						throw std::logic_error(
								"logical CREATE/SET label assignment must not evaluate a Property expression");
				}
				throw std::logic_error("logical CREATE/SET has an unsupported Property assignment");
			};

			constexpr size_t MAX_CONFLICT_RETRIES = 4U;
			for (size_t attempt = 0U; attempt < MAX_CONFLICT_RETRIES; ++attempt) {
				std::unique_ptr<storage::LogicalGraphReadSnapshot> snapshot =
						readBackend.openLogicalGraphReadSnapshot();
				if (!snapshot) {
					throw std::runtime_error("logical CREATE/SET could not open a read snapshot");
				}
				const std::optional<storage::LogicalGraphMutationFence> fence = snapshot->mutationFence();
				if (!fence.has_value() || !fence->isValid()) {
					throw std::logic_error(
							"logical CREATE/SET requires a backend that exposes an optimistic read fence");
				}

				LogicalGraphMutationBatch batch;
				MutableGraphEntities persistedEntities;
				std::vector<CreateSetRuntimeRow> projectionRows;
				std::set<size_t> cancelledNodeCreates;
				std::set<size_t> cancelledEdgeCreates;
				std::set<uint64_t> deletedNodeIds;
				std::set<uint64_t> deletedEdgeIds;
				std::map<uint64_t, bool> deletedNodeDetach;
				std::unique_ptr<logical::LogicalSingleRow> implicitSeed;
				const logical::LogicalOperator *readRoot = chain.read_root;
				if (readRoot == nullptr) {
					implicitSeed = std::make_unique<logical::LogicalSingleRow>();
					readRoot = implicitSeed.get();
				}

				LogicalPlanReadRuntime reader(readBackend);
				const auto entityReferencesForRow = [](const LogicalPlanReadRow &source) {
					std::unordered_map<std::string, LogicalGraphMutationEntityReference> entities;
					entities.reserve(source.values.size());
					for (const auto &[variable, value]: source.values) {
						if (const auto *node = std::get_if<storage::LogicalGraphNode>(&value)) {
							entities.emplace(variable,
											 LogicalGraphMutationEntityReference{
													 LogicalGraphMutationEntityKind::LGMK_NODE,
													 LogicalGraphMutationReferenceKind::LGRK_PERSISTED, node->node_id});
						} else if (const auto *edge = std::get_if<storage::LogicalGraphEdge>(&value)) {
							entities.emplace(variable,
											 LogicalGraphMutationEntityReference{
													 LogicalGraphMutationEntityKind::LGMK_EDGE,
													 LogicalGraphMutationReferenceKind::LGRK_PERSISTED, edge->edge_id});
						}
					}
					return entities;
				};
				const auto stageRow = [&](const LogicalPlanReadRow &source, bool retainOutputRow) {
					context.checkGuard();
					std::unordered_map<std::string, LogicalGraphMutationEntityReference> entities =
							entityReferencesForRow(source);
					entities.reserve(entities.size() + createsPerInputRow);
					std::unordered_map<std::string, PropertyMap> propertyCache = source.property_cache;

					const auto resolveNode = [&entities](const std::string &variable) {
						const auto found = entities.find(variable);
						if (variable.empty() || found == entities.end() ||
							found->second.entity_kind != LogicalGraphMutationEntityKind::LGMK_NODE) {
							throw std::logic_error("logical CREATE/SET cannot resolve a relationship Node endpoint");
						}
						return found->second;
					};

					for (const logical::LogicalOperator *operation: chain.operations) {
						context.checkGuard();
						if (const auto *node = dynamic_cast<const logical::LogicalCreateNode *>(operation);
							node != nullptr) {
							if (const auto existing = entities.find(node->getVariable()); existing != entities.end()) {
								if (node->getVariable().empty() ||
									existing->second.entity_kind != LogicalGraphMutationEntityKind::LGMK_NODE ||
									!node->getLabels().empty() || !node->getProperties().empty() ||
									!node->getPropertyExprs().empty()) {
									throw std::logic_error("logical CREATE/SET cannot redeclare a bound Node");
								}
								continue;
							}
							if (batch.node_creates.size() + batch.edge_creates.size() >= maximumStagedCreates) {
								throw std::runtime_error("logical CREATE/SET exceeded its staged mutation bound");
							}
							LogicalGraphNodeCreateRequest request;
							request.labels = node->getLabels();
							request.properties = toLogicalProperties(node->getProperties());
							appendExpressionProperties(node->getPropertyExprs(), request.properties, source, *snapshot,
													   propertyCache);
							const LogicalGraphMutationEntityReference reference =
									createdNodeReference(batch.node_creates.size());
							batch.node_creates.push_back(std::move(request));
							if (!node->getVariable().empty()) {
								entities.emplace(node->getVariable(), reference);
								propertyCache.insert_or_assign(
										node->getVariable(),
										propertyMapFromLogicalProperties(batch.node_creates.back().properties,
																		 "logical CREATE/SET Node creation"));
							}
							continue;
						}
						if (const auto *edge = dynamic_cast<const logical::LogicalCreateEdge *>(operation);
							edge != nullptr) {
							if (!edge->getVariable().empty() && entities.contains(edge->getVariable())) {
								throw std::logic_error("logical CREATE/SET has an invalid relationship declaration");
							}
							if (batch.node_creates.size() + batch.edge_creates.size() >= maximumStagedCreates) {
								throw std::runtime_error("logical CREATE/SET exceeded its staged mutation bound");
							}
							LogicalGraphBatchEdgeCreateRequest request;
							request.source = resolveNode(edge->getSourceVar());
							request.target = resolveNode(edge->getTargetVar());
							request.type = edge->getEdgeType();
							request.properties = toLogicalProperties(edge->getProperties());
							appendExpressionProperties(edge->getPropertyExprs(), request.properties, source, *snapshot,
													   propertyCache);
							const LogicalGraphMutationEntityReference reference{
									LogicalGraphMutationEntityKind::LGMK_EDGE,
									LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE,
									static_cast<uint64_t>(batch.edge_creates.size())};
							batch.edge_creates.push_back(std::move(request));
							if (!edge->getVariable().empty()) {
								entities.emplace(edge->getVariable(), reference);
								propertyCache.insert_or_assign(
										edge->getVariable(),
										propertyMapFromLogicalProperties(batch.edge_creates.back().properties,
																		 "logical CREATE/SET relationship creation"));
							}
							continue;
						}

						if (const auto *remove = dynamic_cast<const logical::LogicalRemove *>(operation);
							remove != nullptr) {
							if (remove->getItems().empty()) {
								throw std::logic_error("logical CREATE/SET chain has an empty REMOVE update");
							}
							for (const logical::LogicalRemoveItem &item: remove->getItems()) {
								const auto entity = entities.find(item.variable);
								if (item.variable.empty() || item.key.empty() || entity == entities.end()) {
									throw std::logic_error(
											"logical CREATE/SET cannot resolve a REMOVE entity variable");
								}
								const LogicalGraphMutationEntityReference reference = entity->second;
								if (item.type == logical::LogicalRemoveActionType::LREM_LABEL) {
									if (reference.entity_kind == LogicalGraphMutationEntityKind::LGMK_EDGE) {
										continue;
									}
									if (reference.reference_kind ==
												LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE &&
										reference.value < batch.node_creates.size()) {
										auto &labels = batch.node_creates[static_cast<size_t>(reference.value)].labels;
										if (const auto found = std::ranges::find(labels, item.key);
											found != labels.end()) {
											labels.erase(found);
										}
										continue;
									}
									if (reference.reference_kind != LogicalGraphMutationReferenceKind::LGRK_PERSISTED) {
										throw std::logic_error(
												"logical CREATE/SET has an invalid Node label removal target");
									}
									const auto value = source.values.find(item.variable);
									if (value == source.values.end()) {
										throw std::logic_error("logical CREATE/SET lost a persisted Node binding");
									}
									MutableGraphEntity &mutableEntity =
											mutableEntityForValue(persistedEntities, value->second);
									if (mutableEntity.kind != LogicalGraphMutationEntityKind::LGMK_NODE ||
										mutableEntity.id != reference.value) {
										throw std::logic_error("logical CREATE/SET Node label removal binding "
															   "is inconsistent");
									}
									if (const auto found = std::ranges::find(mutableEntity.labels, item.key);
										found != mutableEntity.labels.end()) {
										mutableEntity.labels.erase(found);
										mutableEntity.labels_changed = true;
									}
									continue;
								}

								if (reference.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE ||
									reference.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE) {
									std::vector<LogicalGraphWriteProperty> *properties = nullptr;
									if (reference.reference_kind ==
												LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE &&
										reference.entity_kind == LogicalGraphMutationEntityKind::LGMK_NODE &&
										reference.value < batch.node_creates.size()) {
										properties =
												&batch.node_creates[static_cast<size_t>(reference.value)].properties;
									} else if (reference.reference_kind ==
													   LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE &&
											   reference.entity_kind == LogicalGraphMutationEntityKind::LGMK_EDGE &&
											   reference.value < batch.edge_creates.size()) {
										properties =
												&batch.edge_creates[static_cast<size_t>(reference.value)].properties;
									}
									if (properties == nullptr) {
										throw std::logic_error("logical CREATE/SET has an invalid created "
															   "Property removal target");
									}
									PropertyMap current =
											propertyMapFromLogicalProperties(*properties, "logical CREATE/SET");
									static_cast<void>(current.erase(item.key));
									*properties = sortedProperties(current);
									propertyCache.insert_or_assign(item.variable, std::move(current));
									continue;
								}
								if (reference.reference_kind != LogicalGraphMutationReferenceKind::LGRK_PERSISTED) {
									throw std::logic_error("logical CREATE/SET has an invalid persisted "
														   "Property removal target");
								}
								const auto value = source.values.find(item.variable);
								if (value == source.values.end()) {
									throw std::logic_error("logical CREATE/SET lost a persisted entity binding");
								}
								MutableGraphEntity &mutableEntity =
										mutableEntityForValue(persistedEntities, value->second);
								if (mutableEntity.kind != reference.entity_kind ||
									mutableEntity.id != reference.value) {
									throw std::logic_error(
											"logical CREATE/SET persisted entity binding is inconsistent");
								}
								PropertyMap &properties = mutableEntityProperties(mutableEntity, *snapshot, bounds);
								mutableEntity.properties_changed =
										properties.erase(item.key) != 0U || mutableEntity.properties_changed;
								propertyCache.insert_or_assign(item.variable, properties);
							}
							continue;
						}

						if (const auto *deleteOperator = dynamic_cast<const logical::LogicalDelete *>(operation);
							deleteOperator != nullptr) {
							if (deleteOperator->getVariables().empty()) {
								throw std::logic_error("logical CREATE/SET chain has an empty DELETE target list");
							}
							for (const std::string &variable: deleteOperator->getVariables()) {
								const auto entity = entities.find(variable);
								if (variable.empty() || entity == entities.end()) {
									throw std::logic_error(
											"logical CREATE/SET cannot resolve a DELETE entity variable");
								}
								const LogicalGraphMutationEntityReference reference = entity->second;
								if (reference.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE) {
									cancelledNodeCreates.insert(static_cast<size_t>(reference.value));
									continue;
								}
								if (reference.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE) {
									cancelledEdgeCreates.insert(static_cast<size_t>(reference.value));
									continue;
								}
								const auto value = source.values.find(variable);
								if (value == source.values.end()) {
									throw std::logic_error("logical CREATE/SET lost a persisted DELETE binding");
								}
								if (reference.entity_kind == LogicalGraphMutationEntityKind::LGMK_NODE) {
									const auto *node = std::get_if<storage::LogicalGraphNode>(&value->second);
									if (node == nullptr || node->node_id != reference.value) {
										throw std::logic_error(
												"logical CREATE/SET persisted DELETE binding is inconsistent");
									}
									deletedNodeIds.insert(reference.value);
									deletedNodeDetach[reference.value] =
											deletedNodeDetach[reference.value] || deleteOperator->isDetach();
								} else {
									const auto *edge = std::get_if<storage::LogicalGraphEdge>(&value->second);
									if (edge == nullptr || edge->edge_id != reference.value) {
										throw std::logic_error(
												"logical CREATE/SET persisted DELETE binding is inconsistent");
									}
									deletedEdgeIds.insert(reference.value);
								}
							}
							continue;
						}

						const auto *set = dynamic_cast<const logical::LogicalSet *>(operation);
						if (set == nullptr || set->getItems().empty()) {
							throw std::logic_error("logical CREATE/SET chain has an invalid update operator");
						}
						for (const logical::LogicalSetItem &item: set->getItems()) {
							const auto entity = entities.find(item.variable);
							if (item.variable.empty() || entity == entities.end()) {
								throw std::logic_error("logical CREATE/SET cannot resolve a SET entity variable");
							}
							const LogicalGraphMutationEntityReference reference = entity->second;
							if (item.type == logical::SetActionType::LSET_LABEL) {
								if (item.key.empty() ||
									reference.entity_kind != LogicalGraphMutationEntityKind::LGMK_NODE) {
									throw std::logic_error("logical CREATE/SET label assignment requires a "
														   "Node and nonempty label");
								}
								if (reference.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE) {
									if (reference.value >= batch.node_creates.size()) {
										throw std::logic_error(
												"logical CREATE/SET has an invalid created Node reference");
									}
									auto &labels = batch.node_creates[static_cast<size_t>(reference.value)].labels;
									if (std::ranges::find(labels, item.key) == labels.end()) {
										labels.push_back(item.key);
									}
									continue;
								}
								if (reference.reference_kind != LogicalGraphMutationReferenceKind::LGRK_PERSISTED) {
									throw std::logic_error("logical CREATE/SET has an invalid Node label target");
								}
								const auto value = source.values.find(item.variable);
								if (value == source.values.end()) {
									throw std::logic_error("logical CREATE/SET lost a persisted Node binding");
								}
								MutableGraphEntity &mutableEntity =
										mutableEntityForValue(persistedEntities, value->second);
								if (mutableEntity.kind != LogicalGraphMutationEntityKind::LGMK_NODE ||
									mutableEntity.id != reference.value) {
									throw std::logic_error("logical CREATE/SET Node label binding is inconsistent");
								}
								if (std::ranges::find(mutableEntity.labels, item.key) == mutableEntity.labels.end()) {
									mutableEntity.labels.push_back(item.key);
									mutableEntity.labels_changed = true;
								}
								continue;
							}

							if (reference.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE ||
								reference.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE) {
								std::vector<LogicalGraphWriteProperty> *properties = nullptr;
								if (reference.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE &&
									reference.entity_kind == LogicalGraphMutationEntityKind::LGMK_NODE &&
									reference.value < batch.node_creates.size()) {
									properties = &batch.node_creates[static_cast<size_t>(reference.value)].properties;
								} else if (reference.reference_kind ==
												   LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE &&
										   reference.entity_kind == LogicalGraphMutationEntityKind::LGMK_EDGE &&
										   reference.value < batch.edge_creates.size()) {
									properties = &batch.edge_creates[static_cast<size_t>(reference.value)].properties;
								}
								if (properties == nullptr) {
									throw std::logic_error("logical CREATE/SET has an invalid created Property target");
								}
								PropertyMap current =
										propertyMapFromLogicalProperties(*properties, "logical CREATE/SET");
								static_cast<void>(applyPropertySet(current, item, source, *snapshot, propertyCache));
								*properties = sortedProperties(current);
								propertyCache.insert_or_assign(item.variable, std::move(current));
								continue;
							}

							if (reference.reference_kind != LogicalGraphMutationReferenceKind::LGRK_PERSISTED) {
								throw std::logic_error("logical CREATE/SET has an invalid persisted Property target");
							}
							const auto value = source.values.find(item.variable);
							if (value == source.values.end()) {
								throw std::logic_error("logical CREATE/SET lost a persisted entity binding");
							}
							MutableGraphEntity &mutableEntity = mutableEntityForValue(persistedEntities, value->second);
							if (mutableEntity.kind != reference.entity_kind || mutableEntity.id != reference.value) {
								throw std::logic_error("logical CREATE/SET persisted entity binding is inconsistent");
							}
							PropertyMap &properties = mutableEntityProperties(mutableEntity, *snapshot, bounds);
							mutableEntity.properties_changed =
									applyPropertySet(properties, item, source, *snapshot, propertyCache) ||
									mutableEntity.properties_changed;
							propertyCache.insert_or_assign(item.variable, properties);
						}
					}
					if (retainOutputRow) {
						projectionRows.push_back({source, std::move(entities), {}});
					}
				};

				LogicalPlanReadRuntimeStats stats;
				if (foreach == nullptr) {
					stats = reader.executeWithSnapshot(
							*snapshot, *readRoot, bounds,
							[&stageRow](const LogicalPlanReadRow &source) {
								stageRow(source, true);
								return true;
							},
							context);
				} else {
					size_t iterationCount = 0U;
					stats = reader.executeWithSnapshot(
							*snapshot, *readRoot, bounds,
							[&](const LogicalPlanReadRow &source) {
								context.checkGuard();
								projectionRows.push_back({source, entityReferencesForRow(source), {}});

								LogicalPlanReadRow evaluationRow = source;
								const LogicalPlanReadValue evaluated =
										LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(
												*snapshot, *foreach->getListExpr(), evaluationRow, bounds, context);
								const auto *listValue = std::get_if<PropertyValue>(&evaluated);
								if (listValue == nullptr) {
									throw std::logic_error(
											"logical FOREACH list expression must evaluate to a Property value");
								}
								if (listValue->getType() != PropertyType::LIST) {
									// Preserve the established FOREACH lowering contract: a scalar
									// source produces no body iteration and still passes its input row on.
									return true;
								}
								const std::vector<PropertyValue> &values = listValue->getList();
								if (values.size() > bounds.maximum_rows ||
									iterationCount > bounds.maximum_rows - values.size()) {
									throw std::runtime_error("logical FOREACH exceeded its configured iteration bound");
								}
								for (const PropertyValue &value: values) {
									context.checkGuard();
									LogicalPlanReadRow iteration = source;
									iteration.values.insert_or_assign(foreach->getIterVar(), value);
									iteration.property_cache.erase(foreach->getIterVar());
									stageRow(iteration, false);
									++iterationCount;
								}
								return true;
							},
							context);
				}
				requireCompleteReadForMutation(stats, bounds,
											   foreach == nullptr ? "logical CREATE/SET" : "logical FOREACH");

				for (const uint64_t edgeId: deletedEdgeIds) {
					batch.edge_deletes.push_back({edgeId});
				}
				for (const uint64_t nodeId: deletedNodeIds) {
					batch.node_deletes.push_back({nodeId, deletedNodeDetach[nodeId],
												  deletedNodeDetach[nodeId] ? bounds.maximum_edges_per_node : 0U});
				}

				// Cancel created entities before publication and remap surviving endpoints.
				if (!cancelledNodeCreates.empty() || !cancelledEdgeCreates.empty()) {
					const size_t invalidOrdinal = std::numeric_limits<size_t>::max();
					std::vector<size_t> nodeRemap(batch.node_creates.size(), invalidOrdinal);
					std::vector<LogicalGraphNodeCreateRequest> survivingNodes;
					survivingNodes.reserve(batch.node_creates.size() - cancelledNodeCreates.size());
					for (size_t index = 0U; index < batch.node_creates.size(); ++index) {
						if (cancelledNodeCreates.contains(index))
							continue;
						nodeRemap[index] = survivingNodes.size();
						survivingNodes.push_back(std::move(batch.node_creates[index]));
					}
					std::vector<size_t> edgeRemap(batch.edge_creates.size(), invalidOrdinal);
					std::vector<LogicalGraphBatchEdgeCreateRequest> survivingEdges;
					survivingEdges.reserve(batch.edge_creates.size() - cancelledEdgeCreates.size());
					const auto remapEndpoint = [&](LogicalGraphMutationEntityReference &reference) {
						if (reference.reference_kind != LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE)
							return;
						if (reference.value >= nodeRemap.size() ||
							nodeRemap[static_cast<size_t>(reference.value)] == invalidOrdinal) {
							throw std::logic_error(
									"logical CREATE/SET deleted a Node still referenced by a surviving relationship");
						}
						reference.value = nodeRemap[static_cast<size_t>(reference.value)];
					};
					for (size_t index = 0U; index < batch.edge_creates.size(); ++index) {
						if (cancelledEdgeCreates.contains(index))
							continue;
						LogicalGraphBatchEdgeCreateRequest edge = std::move(batch.edge_creates[index]);
						remapEndpoint(edge.source);
						remapEndpoint(edge.target);
						edgeRemap[index] = survivingEdges.size();
						survivingEdges.push_back(std::move(edge));
					}
					batch.node_creates = std::move(survivingNodes);
					batch.edge_creates = std::move(survivingEdges);
					for (CreateSetRuntimeRow &runtimeRow: projectionRows) {
						for (auto &[variable, reference]: runtimeRow.entities) {
							if (reference.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE &&
								reference.value < nodeRemap.size() &&
								nodeRemap[static_cast<size_t>(reference.value)] != invalidOrdinal) {
								reference.value = nodeRemap[static_cast<size_t>(reference.value)];
							} else if (reference.reference_kind ==
											   LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE &&
									   reference.value < edgeRemap.size() &&
									   edgeRemap[static_cast<size_t>(reference.value)] != invalidOrdinal) {
								reference.value = edgeRemap[static_cast<size_t>(reference.value)];
							} else if (reference.reference_kind ==
											   LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE ||
									   reference.reference_kind ==
											   LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE) {
								runtimeRow.unavailable_implicit_entities.insert(variable);
							}
						}
					}
				}

				for (const auto &[_, entity]: persistedEntities) {
					const LogicalGraphMutationEntityReference reference{
							entity.kind, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, entity.id};
					if ((entity.kind == LogicalGraphMutationEntityKind::LGMK_NODE &&
						 deletedNodeIds.contains(entity.id)) ||
						(entity.kind == LogicalGraphMutationEntityKind::LGMK_EDGE &&
						 deletedEdgeIds.contains(entity.id))) {
						continue;
					}
					if (entity.properties_changed) {
						batch.property_replacements.push_back({reference, sortedProperties(*entity.properties)});
					}
					if (entity.labels_changed) {
						batch.node_label_replacements.push_back({reference, entity.labels});
					}
				}
				try {
					const storage::LogicalGraphMutationBatchResult result =
							writeBackend.executeLogicalGraphMutationBatch(batch, fence);
					return buildCreateSetResult(projection, implicitColumns, projectionRows, *snapshot, batch, result,
												persistedEntities, bounds, context);
				} catch (const storage::LogicalGraphMutationConflict &) {
					// All created references and Property reads belong to this pinned view.
					// Replan the complete statement after a concurrent publication.
				}
			}
			throw std::runtime_error("logical CREATE/SET exceeded its optimistic conflict retry limit");
		}

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
							 const QueryContext &context) {
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

			std::vector<std::string> outputColumns =
					projection != nullptr ? projection->getOutputVariables() : implicitColumns;
			if (projection == nullptr) {
				std::erase_if(outputColumns, [&rows](const std::string &column) {
					return std::ranges::any_of(rows, [&column](const CreateSetRuntimeRow &row) {
						return row.unavailable_implicit_entities.contains(column);
					});
				});
			}
			QueryResult queryResult;
			queryResult.setColumns(outputColumns);
			queryResult.setPipelineRowCount(rows.size());
			for (const CreateSetRuntimeRow &runtimeRow: rows) {
				QueryResult::Row output;
				const auto resolve = [&](const std::string &variable) -> ResultValue {
					if (const auto entity = runtimeRow.entities.find(variable); entity != runtimeRow.entities.end()) {
						if (entity->second.reference_kind != LogicalGraphMutationReferenceKind::LGRK_PERSISTED) {
							return materializeCreated(entity->second);
						}
						return materializeMutableBinding(snapshot, runtimeRow.source, variable, persistedEntities,
														 bounds);
					}
					const auto scalar = runtimeRow.source.values.find(variable);
					if (scalar != runtimeRow.source.values.end() &&
						std::holds_alternative<PropertyValue>(scalar->second)) {
						return std::get<PropertyValue>(scalar->second);
					}
					throw std::logic_error("logical CREATE/SET RETURN could not resolve a variable");
				};
				if (projection == nullptr) {
					if (outputColumns.empty()) {
						continue;
					}
					output.reserve(outputColumns.size());
					for (const std::string &column: outputColumns) {
						context.checkGuard();
						if (!output.emplace(column, resolve(column)).second) {
							throw std::logic_error("logical CREATE/SET produced duplicate output columns");
						}
					}
					queryResult.addRow(std::move(output));
					continue;
				}
				for (const logical::LogicalProjectItem &item: projection->getItems()) {
					context.checkGuard();
					if (item.alias.empty() || !item.expression) {
						throw std::logic_error("logical CREATE/SET RETURN has an invalid output item");
					}
					ResultValue value = evaluateMutationProjectionExpression(
							*item.expression, runtimeRow.source, snapshot, bounds, context,
							[&resolve](const LogicalPlanReadRow &, const std::string &variable) {
								return resolve(variable);
							});
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
				const auto &reference = dynamic_cast<const expressions::VariableReferenceExpression &>(expression);
				if (!reference.hasProperty()) {
					return resolveBinding(source, reference.getVariableName());
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
		buildMutationProjection(const logical::LogicalProject &projection, const std::vector<LogicalPlanReadRow> &rows,
								storage::LogicalGraphReadSnapshot &snapshot, const LogicalPlanReadRuntimeBounds &bounds,
								const QueryContext &context, const MutationProjectionResolver &resolveBinding) {
			if (!resolveBinding) {
				throw std::logic_error("logical mutation RETURN requires a binding resolver");
			}
			QueryResult result;
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

		[[nodiscard]] PropertyMap mergeProperties(const std::unordered_map<std::string, PropertyValue> &properties) {
			PropertyMap result;
			result.reserve(properties.size());
			for (const auto &[key, value]: properties) {
				if (key.empty() || value.getType() == PropertyType::NULL_TYPE || !result.emplace(key, value).second) {
					throw std::logic_error("logical MERGE has an invalid match Property");
				}
			}
			return result;
		}

		void validateMergeLabels(const std::vector<std::string> &labels) {
			std::set<std::string> unique;
			for (const std::string &label: labels) {
				if (label.empty() || !unique.insert(label).second) {
					throw std::logic_error("logical MERGE has an empty or duplicate Node label");
				}
			}
		}

		[[nodiscard]] std::string normalizedMergeDirection(const std::string &direction) {
			std::string normalized;
			normalized.reserve(direction.size());
			for (const char value: direction) {
				normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(value))));
			}
			if (normalized != "out" && normalized != "in" && normalized != "both") {
				throw std::logic_error("logical MERGE has an unsupported relationship direction");
			}
			return normalized;
		}

		struct MergeRuntimeRow {
			LogicalPlanReadRow row;
			std::unordered_map<std::string, LogicalGraphMutationEntityReference> entities;
			std::vector<LogicalGraphMutationEntityReference> recent_nodes;
		};

		struct MergeNodeMatch {
			LogicalGraphMutationEntityReference reference;
			storage::LogicalGraphNode node;
			bool created = false;
		};

		struct MergeEdgeMatch {
			LogicalGraphMutationEntityReference reference;
			storage::LogicalGraphEdge edge;
			bool created = false;
		};

		struct MergeExecutionState {
			storage::LogicalGraphReadSnapshot &snapshot;
			const LogicalPlanReadRuntimeBounds &bounds;
			const QueryContext &context;
			const LogicalPlanReadRuntime &reader;
			LogicalGraphMutationBatch batch;
			MutableGraphEntities modified_entities;
			std::vector<PropertyMap> created_node_properties;
			std::vector<PropertyMap> created_edge_properties;
			std::unordered_map<const logical::LogicalMergeNode *, MergeNodeMatch> node_matches;
			std::unordered_map<std::string, MergeEdgeMatch> edge_matches;
			size_t operator_count = 0U;

			MergeExecutionState(storage::LogicalGraphReadSnapshot &readSnapshot,
								const LogicalPlanReadRuntimeBounds &readBounds, const QueryContext &queryContext,
								const LogicalPlanReadRuntime &readRuntime) :
				snapshot(readSnapshot), bounds(readBounds), context(queryContext), reader(readRuntime) {}
		};

		using MergeRowVisitor = std::function<bool(MergeRuntimeRow &&)>;

		[[nodiscard]] bool mergeNodePropertiesMatch(const PropertyMap &actual, const PropertyMap &expected) {
			for (const auto &[key, value]: expected) {
				const auto found = actual.find(key);
				if (found == actual.end() || found->second != value) {
					return false;
				}
			}
			return true;
		}

		[[nodiscard]] std::optional<storage::LogicalGraphNode> findMergeNode(const logical::LogicalMergeNode &operation,
																			 const PropertyMap &properties,
																			 MergeExecutionState &state) {
			std::vector<std::pair<std::string, PropertyValue>> predicates;
			predicates.reserve(properties.size());
			for (const auto &[key, value]: properties) {
				predicates.emplace_back(key, value);
			}
			std::ranges::sort(predicates, {}, &std::pair<std::string, PropertyValue>::first);
			logical::LogicalNodeScan scan("__logical_merge_node", operation.getLabels(), std::move(predicates));
			std::optional<storage::LogicalGraphNode> found;
			const LogicalPlanReadRuntimeStats stats = state.reader.executeWithSnapshot(
					state.snapshot, scan, state.bounds,
					[&](const LogicalPlanReadRow &row) {
						state.context.checkGuard();
						const auto binding = row.values.find(scan.getVariable());
						if (binding == row.values.end() ||
							!std::holds_alternative<storage::LogicalGraphNode>(binding->second)) {
							throw std::logic_error("logical MERGE Node scan produced an invalid binding");
						}
						found = std::get<storage::LogicalGraphNode>(binding->second);
						return false;
					},
					state.context);
			if (!found.has_value()) {
				requireCompleteReadForMutation(stats, state.bounds, "logical MERGE Node scan");
			}
			return found;
		}

		[[nodiscard]] bool applyMergeActions(PropertyMap &properties,
											 const std::vector<logical::MergeSetAction> &actions,
											 const std::string &targetVariable, MergeRuntimeRow &row,
											 MergeExecutionState &state) {
			if (actions.empty()) {
				return false;
			}
			if (targetVariable.empty()) {
				throw std::logic_error("logical MERGE cannot apply ON CREATE or ON MATCH to an anonymous entity");
			}
			bool changed = false;
			for (const logical::MergeSetAction &action: actions) {
				state.context.checkGuard();
				if (action.variable != targetVariable || action.key.empty() || !action.expression) {
					throw std::logic_error("logical MERGE has an unsupported ON CREATE or ON MATCH assignment");
				}
				const PropertyValue value =
						evaluateCreatePropertyExpression(*action.expression, state.context, row.row, state.snapshot,
														 state.bounds, row.row.property_cache);
				if (value.getType() == PropertyType::NULL_TYPE) {
					changed = properties.erase(action.key) != 0U || changed;
				} else {
					const auto existing = properties.find(action.key);
					if (existing == properties.end() || existing->second != value) {
						properties.insert_or_assign(action.key, value);
						changed = true;
					}
				}
				row.row.property_cache.insert_or_assign(targetVariable, properties);
			}
			return changed;
		}

		void bindMergeNode(MergeRuntimeRow &row, const std::string &variable,
						   const LogicalGraphMutationEntityReference &reference, const storage::LogicalGraphNode &node,
						   const std::optional<PropertyMap> &properties) {
			if (!variable.empty()) {
				const auto existing = row.entities.find(variable);
				if (existing != row.entities.end() && existing->second != reference) {
					throw std::logic_error("logical MERGE cannot rebind a Node variable");
				}
				const auto value = row.row.values.find(variable);
				if (value != row.row.values.end() &&
					!std::holds_alternative<storage::LogicalGraphNode>(value->second)) {
					throw std::logic_error("logical MERGE Node variable is bound to a non-Node value");
				}
				row.entities.insert_or_assign(variable, reference);
				row.row.values.insert_or_assign(variable, node);
				if (properties.has_value()) {
					row.row.property_cache.insert_or_assign(variable, *properties);
				}
			}
			row.recent_nodes.push_back(reference);
		}

		void bindMergeEdge(MergeRuntimeRow &row, const std::string &variable,
						   const LogicalGraphMutationEntityReference &reference, const storage::LogicalGraphEdge &edge,
						   const std::optional<PropertyMap> &properties) {
			if (variable.empty()) {
				return;
			}
			const auto existing = row.entities.find(variable);
			if (existing != row.entities.end() && existing->second != reference) {
				throw std::logic_error("logical MERGE cannot rebind a relationship variable");
			}
			const auto value = row.row.values.find(variable);
			if (value != row.row.values.end() && !std::holds_alternative<storage::LogicalGraphEdge>(value->second)) {
				throw std::logic_error("logical MERGE relationship variable is bound to a non-relationship value");
			}
			row.entities.insert_or_assign(variable, reference);
			row.row.values.insert_or_assign(variable, edge);
			if (properties.has_value()) {
				row.row.property_cache.insert_or_assign(variable, *properties);
			}
		}

		[[nodiscard]] LogicalGraphMutationEntityReference resolveMergeNode(const MergeRuntimeRow &row,
																		   const std::string &variable, bool source) {
			if (!variable.empty()) {
				const auto found = row.entities.find(variable);
				if (found == row.entities.end() ||
					found->second.entity_kind != LogicalGraphMutationEntityKind::LGMK_NODE) {
					throw std::logic_error("logical MERGE cannot resolve a relationship Node endpoint");
				}
				return found->second;
			}
			if (row.recent_nodes.size() < 2U) {
				throw std::logic_error("logical MERGE cannot infer an anonymous relationship Node endpoint");
			}
			return row.recent_nodes[row.recent_nodes.size() - (source ? 2U : 1U)];
		}

		[[nodiscard]] std::string mergeEdgeCacheKey(const logical::LogicalMergeEdge &operation,
													const LogicalGraphMutationEntityReference &source,
													const LogicalGraphMutationEntityReference &target) {
			return std::to_string(reinterpret_cast<uintptr_t>(&operation)) + ":" +
				   std::to_string(static_cast<uint8_t>(source.entity_kind)) + ":" +
				   std::to_string(static_cast<uint8_t>(source.reference_kind)) + ":" + std::to_string(source.value) +
				   ":" + std::to_string(static_cast<uint8_t>(target.entity_kind)) + ":" +
				   std::to_string(static_cast<uint8_t>(target.reference_kind)) + ":" + std::to_string(target.value);
		}

		[[nodiscard]] std::optional<storage::LogicalGraphEdge>
		findMergeEdge(const logical::LogicalMergeEdge &operation, uint64_t sourceNodeId, uint64_t targetNodeId,
					  const PropertyMap &properties, const std::string &direction, MergeExecutionState &state) {
			size_t remaining = state.bounds.maximum_edges_per_node;
			std::set<uint64_t> visited;
			const auto scanDirection = [&](storage::LogicalGraphReadDirection scanDirection,
										   const std::function<bool(const storage::LogicalGraphEdge &)> &matches)
					-> std::optional<storage::LogicalGraphEdge> {
				std::optional<storage::LogicalGraphAdjacencyContinuation> continuation;
				for (;;) {
					if (remaining == 0U) {
						throw std::runtime_error("logical MERGE relationship scan reached its configured bound");
					}
					const size_t pageLimit = std::min(remaining, state.bounds.maximum_adjacency_page_records);
					const storage::LogicalGraphAdjacencyPage page =
							state.snapshot.scanAdjacentEdges(sourceNodeId, scanDirection, continuation, pageLimit);
					if (page.edges.size() > pageLimit || (page.edges.empty() && page.continuation.has_value())) {
						throw std::runtime_error("logical MERGE relationship scan returned an invalid bounded page");
					}
					for (const storage::LogicalGraphEdge &edge: page.edges) {
						state.context.checkGuard();
						if (edge.edge_id == 0U || !visited.insert(edge.edge_id).second) {
							continue;
						}
						--remaining;
						if (matches(edge)) {
							if (edge.type != operation.getEdgeType()) {
								continue;
							}
							const PropertyMap actual =
									readEdgeProperties(state.snapshot, edge, state.bounds.maximum_property_entries);
							if (mergeNodePropertiesMatch(actual, properties)) {
								return edge;
							}
						}
						if (remaining == 0U && page.continuation.has_value()) {
							throw std::runtime_error("logical MERGE relationship scan reached its configured bound");
						}
					}
					if (!page.continuation.has_value()) {
						return std::nullopt;
					}
					continuation = page.continuation;
				}
			};

			if (direction == "out") {
				return scanDirection(
						storage::LogicalGraphReadDirection::LGRD_OUTGOING, [&](const storage::LogicalGraphEdge &edge) {
							return edge.source_node_id == sourceNodeId && edge.target_node_id == targetNodeId;
						});
			}
			if (direction == "in") {
				return scanDirection(
						storage::LogicalGraphReadDirection::LGRD_INCOMING, [&](const storage::LogicalGraphEdge &edge) {
							return edge.source_node_id == targetNodeId && edge.target_node_id == sourceNodeId;
						});
			}
			if (const auto outgoing = scanDirection(storage::LogicalGraphReadDirection::LGRD_OUTGOING,
													[&](const storage::LogicalGraphEdge &edge) {
														return edge.source_node_id == sourceNodeId &&
															   edge.target_node_id == targetNodeId;
													});
				outgoing.has_value()) {
				return outgoing;
			}
			return scanDirection(storage::LogicalGraphReadDirection::LGRD_INCOMING,
								 [&](const storage::LogicalGraphEdge &edge) {
									 return edge.source_node_id == targetNodeId && edge.target_node_id == sourceNodeId;
								 });
		}

		void applyMergeNode(const logical::LogicalMergeNode &operation, MergeRuntimeRow &row,
							MergeExecutionState &state) {
			validateMergeLabels(operation.getLabels());
			const PropertyMap matchProperties = mergeProperties(operation.getMatchProps());
			if (!operation.getVariable().empty()) {
				if (const auto existing = row.entities.find(operation.getVariable()); existing != row.entities.end()) {
					if (existing->second.entity_kind != LogicalGraphMutationEntityKind::LGMK_NODE) {
						throw std::logic_error("logical MERGE Node variable is bound to a relationship");
					}
					const auto value = row.row.values.find(operation.getVariable());
					if (value == row.row.values.end() ||
						!std::holds_alternative<storage::LogicalGraphNode>(value->second)) {
						throw std::logic_error("logical MERGE Node binding is malformed");
					}
					if (!operation.getOnMatchItems().empty()) {
						MutableGraphEntity &entity = mutableEntityForValue(state.modified_entities, value->second);
						PropertyMap &properties = mutableEntityProperties(entity, state.snapshot, state.bounds);
						entity.properties_changed = applyMergeActions(properties, operation.getOnMatchItems(),
																	  operation.getVariable(), row, state) ||
													entity.properties_changed;
					}
					row.recent_nodes.push_back(existing->second);
					return;
				}
			}

			MergeNodeMatch match;
			const auto cached = state.node_matches.find(&operation);
			if (cached != state.node_matches.end()) {
				match = cached->second;
			} else if (const auto existing = findMergeNode(operation, matchProperties, state); existing.has_value()) {
				match.reference = {LogicalGraphMutationEntityKind::LGMK_NODE,
								   LogicalGraphMutationReferenceKind::LGRK_PERSISTED, existing->node_id};
				match.node = *existing;
			} else {
				PropertyMap properties = matchProperties;
				const size_t ordinal = state.batch.node_creates.size();
				const LogicalGraphMutationEntityReference reference = createdNodeReference(ordinal);
				const storage::LogicalGraphNode actionNode{0U, operation.getLabels()};
				MergeRuntimeRow actionRow = row;
				if (!operation.getVariable().empty()) {
					actionRow.entities.insert_or_assign(operation.getVariable(), reference);
					actionRow.row.values.insert_or_assign(operation.getVariable(), actionNode);
					actionRow.row.property_cache.insert_or_assign(operation.getVariable(), properties);
				}
				static_cast<void>(applyMergeActions(properties, operation.getOnCreateItems(), operation.getVariable(),
													actionRow, state));
				LogicalGraphNodeCreateRequest request;
				request.labels = operation.getLabels();
				request.properties = sortedProperties(properties);
				state.batch.node_creates.push_back(std::move(request));
				state.created_node_properties.push_back(std::move(properties));
				match.reference = reference;
				match.node = actionNode;
				match.created = true;
			}
			state.node_matches.insert_or_assign(&operation, match);
			if (match.created) {
				bindMergeNode(row, operation.getVariable(), match.reference, match.node,
							  state.created_node_properties.at(static_cast<size_t>(match.reference.value)));
				return;
			}

			if (operation.getOnMatchItems().empty()) {
				bindMergeNode(row, operation.getVariable(), match.reference, match.node, std::nullopt);
				return;
			}
			MutableGraphEntity &entity =
					mutableEntityForValue(state.modified_entities, LogicalPlanReadValue(match.node));
			PropertyMap &properties = mutableEntityProperties(entity, state.snapshot, state.bounds);
			bindMergeNode(row, operation.getVariable(), match.reference, match.node, properties);
			entity.properties_changed =
					applyMergeActions(properties, operation.getOnMatchItems(), operation.getVariable(), row, state) ||
					entity.properties_changed;
		}

		void applyMergeEdge(const logical::LogicalMergeEdge &operation, MergeRuntimeRow &row,
							MergeExecutionState &state) {
			const std::string direction = normalizedMergeDirection(operation.getDirection());
			const PropertyMap matchProperties = mergeProperties(operation.getMatchProps());
			const LogicalGraphMutationEntityReference source = resolveMergeNode(row, operation.getSourceVar(), true);
			const LogicalGraphMutationEntityReference target = resolveMergeNode(row, operation.getTargetVar(), false);
			const std::string cacheKey = mergeEdgeCacheKey(operation, source, target);
			MergeEdgeMatch match;
			if (const auto cached = state.edge_matches.find(cacheKey); cached != state.edge_matches.end()) {
				match = cached->second;
			} else if (source.reference_kind == LogicalGraphMutationReferenceKind::LGRK_PERSISTED &&
					   target.reference_kind == LogicalGraphMutationReferenceKind::LGRK_PERSISTED) {
				if (const auto existing =
							findMergeEdge(operation, source.value, target.value, matchProperties, direction, state);
					existing.has_value()) {
					match.reference = {LogicalGraphMutationEntityKind::LGMK_EDGE,
									   LogicalGraphMutationReferenceKind::LGRK_PERSISTED, existing->edge_id};
					match.edge = *existing;
				}
			}
			if (match.reference.value == 0U) {
				PropertyMap properties = matchProperties;
				const size_t ordinal = state.batch.edge_creates.size();
				const LogicalGraphMutationEntityReference reference{
						LogicalGraphMutationEntityKind::LGMK_EDGE, LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE,
						ordinal};
				const storage::LogicalGraphEdge actionEdge{0U, 0U, 0U, operation.getEdgeType()};
				MergeRuntimeRow actionRow = row;
				if (!operation.getEdgeVar().empty()) {
					actionRow.entities.insert_or_assign(operation.getEdgeVar(), reference);
					actionRow.row.values.insert_or_assign(operation.getEdgeVar(), actionEdge);
					actionRow.row.property_cache.insert_or_assign(operation.getEdgeVar(), properties);
				}
				static_cast<void>(applyMergeActions(properties, operation.getOnCreateActions(), operation.getEdgeVar(),
													actionRow, state));
				LogicalGraphBatchEdgeCreateRequest request;
				request.source = direction == "in" ? target : source;
				request.target = direction == "in" ? source : target;
				request.type = operation.getEdgeType();
				request.properties = sortedProperties(properties);
				state.batch.edge_creates.push_back(std::move(request));
				state.created_edge_properties.push_back(std::move(properties));
				match.reference = reference;
				match.edge = actionEdge;
				match.created = true;
			}
			state.edge_matches.insert_or_assign(cacheKey, match);
			if (match.created) {
				bindMergeEdge(row, operation.getEdgeVar(), match.reference, match.edge,
							  state.created_edge_properties.at(static_cast<size_t>(match.reference.value)));
				return;
			}
			if (operation.getOnMatchActions().empty()) {
				bindMergeEdge(row, operation.getEdgeVar(), match.reference, match.edge, std::nullopt);
				return;
			}
			MutableGraphEntity &entity =
					mutableEntityForValue(state.modified_entities, LogicalPlanReadValue(match.edge));
			PropertyMap &properties = mutableEntityProperties(entity, state.snapshot, state.bounds);
			bindMergeEdge(row, operation.getEdgeVar(), match.reference, match.edge, properties);
			entity.properties_changed =
					applyMergeActions(properties, operation.getOnMatchActions(), operation.getEdgeVar(), row, state) ||
					entity.properties_changed;
		}

		[[nodiscard]] MergeRuntimeRow makeMergeRow(LogicalPlanReadRow row) {
			MergeRuntimeRow result;
			result.row = std::move(row);
			for (const auto &[variable, value]: result.row.values) {
				if (const auto *node = std::get_if<storage::LogicalGraphNode>(&value)) {
					result.entities.emplace(variable,
											LogicalGraphMutationEntityReference{
													LogicalGraphMutationEntityKind::LGMK_NODE,
													LogicalGraphMutationReferenceKind::LGRK_PERSISTED, node->node_id});
				} else if (const auto *edge = std::get_if<storage::LogicalGraphEdge>(&value)) {
					result.entities.emplace(variable,
											LogicalGraphMutationEntityReference{
													LogicalGraphMutationEntityKind::LGMK_EDGE,
													LogicalGraphMutationReferenceKind::LGRK_PERSISTED, edge->edge_id});
				}
			}
			return result;
		}

		bool visitMergeOperator(const logical::LogicalOperator &operation, MergeExecutionState &state,
								const MergeRowVisitor &visitor) {
			state.context.checkGuard();
			if (++state.operator_count > state.bounds.maximum_rows) {
				throw std::runtime_error("logical MERGE exceeded its mutation-operation bound");
			}
			const logical::LogicalOperator *child = requireOptionalOnlyChild(operation);
			const auto apply = [&](MergeRuntimeRow &&row) {
				if (operation.getType() == logical::LogicalOpType::LOP_MERGE_NODE) {
					applyMergeNode(dynamic_cast<const logical::LogicalMergeNode &>(operation), row, state);
				} else if (operation.getType() == logical::LogicalOpType::LOP_MERGE_EDGE) {
					applyMergeEdge(dynamic_cast<const logical::LogicalMergeEdge &>(operation), row, state);
				} else {
					throw std::logic_error("logical MERGE tree has an unsupported operator");
				}
				return visitor(std::move(row));
			};
			if (child == nullptr) {
				return apply({});
			}
			if (child->getType() == logical::LogicalOpType::LOP_MERGE_NODE ||
				child->getType() == logical::LogicalOpType::LOP_MERGE_EDGE) {
				return visitMergeOperator(*child, state, apply);
			}
			const LogicalPlanReadRuntimeStats stats = state.reader.executeWithSnapshot(
					state.snapshot, *child, state.bounds,
					[&](const LogicalPlanReadRow &row) { return apply(makeMergeRow(row)); }, state.context);
			requireCompleteReadForMutation(stats, state.bounds, "logical MATCH ... MERGE");
			return true;
		}

		void appendModifiedMergeEntities(MergeExecutionState &state) {
			for (const auto &[_, entity]: state.modified_entities) {
				if (!entity.properties_changed) {
					continue;
				}
				if (!entity.properties.has_value()) {
					throw std::logic_error("logical MERGE lost a staged Property collection");
				}
				LogicalGraphPropertyMutationRequest replacement;
				replacement.entity = {entity.kind, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, entity.id};
				replacement.properties = sortedProperties(*entity.properties);
				state.batch.property_replacements.push_back(std::move(replacement));
			}
		}

		void synchronizeCreatedMergeEntities(MergeExecutionState &state) {
			if (state.batch.node_creates.size() != state.created_node_properties.size() ||
				state.batch.edge_creates.size() != state.created_edge_properties.size()) {
				throw std::logic_error("logical MERGE lost its created-entity Property state");
			}
			for (size_t index = 0U; index < state.batch.node_creates.size(); ++index) {
				state.batch.node_creates[index].properties = sortedProperties(state.created_node_properties[index]);
			}
			for (size_t index = 0U; index < state.batch.edge_creates.size(); ++index) {
				state.batch.edge_creates[index].properties = sortedProperties(state.created_edge_properties[index]);
			}
		}

		[[nodiscard]] PropertyMap &mergePropertiesForPostMutation(const LogicalGraphMutationEntityReference &reference,
																  const std::string &variable, MergeRuntimeRow &row,
																  MergeExecutionState &state,
																  MutableGraphEntity **persistedEntity = nullptr) {
			if (reference.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE) {
				if (reference.entity_kind != LogicalGraphMutationEntityKind::LGMK_NODE ||
					reference.value >= state.created_node_properties.size()) {
					throw std::logic_error("logical MERGE SET has an invalid created Node reference");
				}
				return state.created_node_properties[static_cast<size_t>(reference.value)];
			}
			if (reference.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE) {
				if (reference.entity_kind != LogicalGraphMutationEntityKind::LGMK_EDGE ||
					reference.value >= state.created_edge_properties.size()) {
					throw std::logic_error("logical MERGE SET has an invalid created relationship reference");
				}
				return state.created_edge_properties[static_cast<size_t>(reference.value)];
			}
			if (reference.reference_kind != LogicalGraphMutationReferenceKind::LGRK_PERSISTED) {
				throw std::logic_error("logical MERGE SET has an invalid entity reference kind");
			}
			const auto value = row.row.values.find(variable);
			if (value == row.row.values.end()) {
				throw std::logic_error("logical MERGE SET could not resolve its entity binding");
			}
			MutableGraphEntity &entity = mutableEntityForValue(state.modified_entities, value->second);
			if (entity.kind != reference.entity_kind || entity.id != reference.value) {
				throw std::logic_error("logical MERGE SET entity binding does not match its stable reference");
			}
			if (persistedEntity != nullptr) {
				*persistedEntity = &entity;
			}
			return mutableEntityProperties(entity, state.snapshot, state.bounds);
		}

		void applyMergePostMutations(const std::vector<const logical::LogicalOperator *> &operations,
									 MergeRuntimeRow &row, MergeExecutionState &state) {
			for (const logical::LogicalOperator *operation: operations) {
				if (operation == nullptr) {
					throw std::logic_error("logical MERGE mutation chain contains a null operation");
				}
				if (operation->getType() == logical::LogicalOpType::LOP_SET) {
					const auto &set = dynamic_cast<const logical::LogicalSet &>(*operation);
					for (const logical::LogicalSetItem &item: set.getItems()) {
						state.context.checkGuard();
						if (item.variable.empty()) {
							throw std::logic_error("logical MERGE SET has an empty entity variable");
						}
						const auto reference = row.entities.find(item.variable);
						if (reference == row.entities.end()) {
							throw std::logic_error("logical MERGE SET could not resolve its entity variable");
						}
						if (item.type == logical::SetActionType::LSET_PROPERTY ||
							item.type == logical::SetActionType::LSET_MAP_MERGE) {
							if (!item.expression ||
								(item.type == logical::SetActionType::LSET_PROPERTY && item.key.empty())) {
								throw std::logic_error("logical MERGE SET has an incomplete Property assignment");
							}
							MutableGraphEntity *persisted = nullptr;
							PropertyMap &properties = mergePropertiesForPostMutation(reference->second, item.variable,
																					 row, state, &persisted);
							const PropertyValue value = evaluateCreatePropertyExpression(
									*item.expression, state.context, row.row, state.snapshot, state.bounds,
									row.row.property_cache);
							bool changed = false;
							if (item.type == logical::SetActionType::LSET_PROPERTY) {
								if (value.getType() == PropertyType::NULL_TYPE) {
									changed = properties.erase(item.key) != 0U;
								} else if (const auto existing = properties.find(item.key);
										   existing == properties.end() || existing->second != value) {
									properties.insert_or_assign(item.key, value);
									changed = true;
								}
							} else {
								if (value.getType() != PropertyType::MAP) {
									throw std::logic_error("logical MERGE SET map merge requires a Map value");
								}
								for (const auto &[key, replacement]: value.getMap()) {
									if (key.empty()) {
										throw std::logic_error("logical MERGE SET map merge has an empty Property key");
									}
									if (replacement.getType() == PropertyType::NULL_TYPE) {
										changed = properties.erase(key) != 0U || changed;
									} else if (const auto existing = properties.find(key);
											   existing == properties.end() || existing->second != replacement) {
										properties.insert_or_assign(key, replacement);
										changed = true;
									}
								}
							}
							if (persisted != nullptr) {
								persisted->properties_changed = persisted->properties_changed || changed;
							}
							row.row.property_cache.insert_or_assign(item.variable, properties);
							continue;
						}
						if (item.type != logical::SetActionType::LSET_LABEL || item.key.empty() ||
							reference->second.entity_kind != LogicalGraphMutationEntityKind::LGMK_NODE) {
							throw std::logic_error("logical MERGE SET has an unsupported label assignment");
						}
						const auto binding = row.row.values.find(item.variable);
						if (binding == row.row.values.end() ||
							!std::holds_alternative<storage::LogicalGraphNode>(binding->second)) {
							throw std::logic_error("logical MERGE SET label target is not a Node");
						}
						if (reference->second.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE) {
							const size_t ordinal = static_cast<size_t>(reference->second.value);
							if (ordinal >= state.batch.node_creates.size()) {
								throw std::logic_error("logical MERGE SET has an invalid created Node label reference");
							}
							auto &labels = state.batch.node_creates[ordinal].labels;
							if (std::ranges::find(labels, item.key) == labels.end()) {
								labels.push_back(item.key);
								std::get<storage::LogicalGraphNode>(binding->second).labels = labels;
							}
						} else {
							MutableGraphEntity &entity =
									mutableEntityForValue(state.modified_entities, binding->second);
							if (std::ranges::find(entity.labels, item.key) == entity.labels.end()) {
								entity.labels.push_back(item.key);
								entity.labels_changed = true;
								std::get<storage::LogicalGraphNode>(binding->second).labels = entity.labels;
							}
						}
					}
					continue;
				}
				if (operation->getType() != logical::LogicalOpType::LOP_REMOVE) {
					throw std::logic_error("logical MERGE mutation chain has an unsupported operation");
				}
				const auto &remove = dynamic_cast<const logical::LogicalRemove &>(*operation);
				for (const logical::LogicalRemoveItem &item: remove.getItems()) {
					state.context.checkGuard();
					const auto reference = row.entities.find(item.variable);
					if (item.variable.empty() || item.key.empty() || reference == row.entities.end()) {
						throw std::logic_error("logical MERGE REMOVE has an invalid entity target");
					}
					if (item.type == logical::LogicalRemoveActionType::LREM_PROPERTY) {
						MutableGraphEntity *persisted = nullptr;
						PropertyMap &properties = mergePropertiesForPostMutation(reference->second, item.variable, row,
																				 state, &persisted);
						const bool changed = properties.erase(item.key) != 0U;
						if (persisted != nullptr) {
							persisted->properties_changed = persisted->properties_changed || changed;
						}
						row.row.property_cache.insert_or_assign(item.variable, properties);
						continue;
					}
					if (item.type != logical::LogicalRemoveActionType::LREM_LABEL ||
						reference->second.entity_kind != LogicalGraphMutationEntityKind::LGMK_NODE) {
						throw std::logic_error("logical MERGE REMOVE has an unsupported label target");
					}
					const auto binding = row.row.values.find(item.variable);
					if (binding == row.row.values.end() ||
						!std::holds_alternative<storage::LogicalGraphNode>(binding->second)) {
						throw std::logic_error("logical MERGE REMOVE label target is not a Node");
					}
					if (reference->second.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE) {
						const size_t ordinal = static_cast<size_t>(reference->second.value);
						if (ordinal >= state.batch.node_creates.size()) {
							throw std::logic_error("logical MERGE REMOVE has an invalid created Node label reference");
						}
						auto &labels = state.batch.node_creates[ordinal].labels;
						labels.erase(std::remove(labels.begin(), labels.end(), item.key), labels.end());
						std::get<storage::LogicalGraphNode>(binding->second).labels = labels;
					} else {
						MutableGraphEntity &entity = mutableEntityForValue(state.modified_entities, binding->second);
						const size_t previous = entity.labels.size();
						entity.labels.erase(std::remove(entity.labels.begin(), entity.labels.end(), item.key),
											entity.labels.end());
						entity.labels_changed = entity.labels_changed || entity.labels.size() != previous;
						std::get<storage::LogicalGraphNode>(binding->second).labels = entity.labels;
					}
				}
			}
		}

		struct MergePostMutationChain {
			const logical::LogicalOperator *merge_root = nullptr;
			std::vector<const logical::LogicalOperator *> operations;
		};

		[[nodiscard]] std::optional<MergePostMutationChain>
		collectMergePostMutationChain(const logical::LogicalOperator &root) {
			MergePostMutationChain chain;
			const logical::LogicalOperator *current = &root;
			while (current != nullptr && (current->getType() == logical::LogicalOpType::LOP_SET ||
										  current->getType() == logical::LogicalOpType::LOP_REMOVE)) {
				chain.operations.push_back(current);
				current = requireOptionalOnlyChild(*current);
			}
			if (current == nullptr || (current->getType() != logical::LogicalOpType::LOP_MERGE_NODE &&
									   current->getType() != logical::LogicalOpType::LOP_MERGE_EDGE)) {
				return std::nullopt;
			}
			std::ranges::reverse(chain.operations);
			chain.merge_root = current;
			return chain;
		}

		[[nodiscard]] bool mergeBatchHasMutations(const LogicalGraphMutationBatch &batch) {
			return !batch.node_creates.empty() || !batch.edge_creates.empty() || !batch.property_replacements.empty() ||
				   !batch.node_label_replacements.empty() || !batch.edge_deletes.empty() || !batch.node_deletes.empty();
		}

		[[nodiscard]] uint64_t resolveMergeNodeId(const LogicalGraphMutationEntityReference &reference,
												  const storage::LogicalGraphMutationBatchResult &result) {
			if (reference.entity_kind != LogicalGraphMutationEntityKind::LGMK_NODE) {
				throw std::logic_error("logical MERGE relationship endpoint is not a Node");
			}
			if (reference.reference_kind == LogicalGraphMutationReferenceKind::LGRK_PERSISTED) {
				return reference.value;
			}
			if (reference.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE) {
				return result.created_node_ids.at(static_cast<size_t>(reference.value));
			}
			throw std::logic_error("logical MERGE relationship endpoint has an invalid reference kind");
		}

		[[nodiscard]] ResultValue materializeMergeReference(const LogicalGraphMutationEntityReference &reference,
															const MergeExecutionState &state,
															const storage::LogicalGraphMutationBatchResult &result) {
			if (reference.entity_kind == LogicalGraphMutationEntityKind::LGMK_NODE) {
				if (reference.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE) {
					const size_t ordinal = static_cast<size_t>(reference.value);
					return LogicalNodeResult{result.created_node_ids.at(ordinal),
											 state.batch.node_creates.at(ordinal).labels,
											 state.created_node_properties.at(ordinal)};
				}
				if (reference.reference_kind != LogicalGraphMutationReferenceKind::LGRK_PERSISTED) {
					throw std::logic_error("logical MERGE has an invalid Node projection reference");
				}
				const storage::LogicalGraphNode node = state.snapshot.readNode(reference.value);
				const auto changed = state.modified_entities.find(
						{static_cast<uint8_t>(LogicalGraphMutationEntityKind::LGMK_NODE), reference.value});
				const PropertyMap properties =
						changed != state.modified_entities.end() && changed->second.properties.has_value()
								? *changed->second.properties
								: readNodeProperties(state.snapshot, node, state.bounds.maximum_property_entries);
				return LogicalNodeResult{node.node_id, node.labels, properties};
			}
			if (reference.entity_kind != LogicalGraphMutationEntityKind::LGMK_EDGE) {
				throw std::logic_error("logical MERGE has an invalid projection entity kind");
			}
			if (reference.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE) {
				const size_t ordinal = static_cast<size_t>(reference.value);
				const LogicalGraphBatchEdgeCreateRequest &request = state.batch.edge_creates.at(ordinal);
				return LogicalEdgeResult{result.created_edge_ids.at(ordinal),
										 resolveMergeNodeId(request.source, result),
										 resolveMergeNodeId(request.target, result), request.type,
										 state.created_edge_properties.at(ordinal)};
			}
			if (reference.reference_kind != LogicalGraphMutationReferenceKind::LGRK_PERSISTED) {
				throw std::logic_error("logical MERGE has an invalid relationship projection reference");
			}
			const storage::LogicalGraphEdge edge = state.snapshot.readEdge(reference.value);
			const auto changed = state.modified_entities.find(
					{static_cast<uint8_t>(LogicalGraphMutationEntityKind::LGMK_EDGE), reference.value});
			const PropertyMap properties =
					changed != state.modified_entities.end() && changed->second.properties.has_value()
							? *changed->second.properties
							: readEdgeProperties(state.snapshot, edge, state.bounds.maximum_property_entries);
			return LogicalEdgeResult{edge.edge_id, edge.source_node_id, edge.target_node_id, edge.type, properties};
		}

		[[nodiscard]] QueryResult buildMergeResult(const logical::LogicalProject *projection,
												   const std::vector<std::string> &implicitColumns,
												   const std::vector<MergeRuntimeRow> &rows,
												   const MergeExecutionState &state,
												   const storage::LogicalGraphMutationBatchResult &result) {
			QueryResult queryResult;
			queryResult.setColumns(projection != nullptr ? projection->getOutputVariables() : implicitColumns);
			queryResult.setPipelineRowCount(rows.size());
			for (const MergeRuntimeRow &mergeRow: rows) {
				QueryResult::Row output;
				const auto resolve = [&](const std::string &variable) -> ResultValue {
					if (const auto entity = mergeRow.entities.find(variable); entity != mergeRow.entities.end()) {
						return materializeMergeReference(entity->second, state, result);
					}
					const auto scalar = mergeRow.row.values.find(variable);
					if (scalar != mergeRow.row.values.end() && std::holds_alternative<PropertyValue>(scalar->second)) {
						return std::get<PropertyValue>(scalar->second);
					}
					throw std::logic_error("logical MERGE RETURN could not resolve a variable");
				};
				if (projection == nullptr) {
					if (implicitColumns.empty()) {
						continue;
					}
					output.reserve(implicitColumns.size());
					for (const std::string &column: implicitColumns) {
						state.context.checkGuard();
						if (!output.emplace(column, resolve(column)).second) {
							throw std::logic_error("logical MERGE produced duplicate output columns");
						}
					}
					queryResult.addRow(std::move(output));
					continue;
				}
				for (const logical::LogicalProjectItem &item: projection->getItems()) {
					state.context.checkGuard();
					if (item.alias.empty() || !item.expression) {
						throw std::logic_error("logical MERGE RETURN has an invalid output item");
					}
					ResultValue value = evaluateMutationProjectionExpression(
							*item.expression, mergeRow.row, state.snapshot, state.bounds, state.context,
							[&resolve](const LogicalPlanReadRow &, const std::string &variable) {
								return resolve(variable);
							});
					if (!output.emplace(item.alias, std::move(value)).second) {
						throw std::logic_error("logical MERGE RETURN has duplicate output aliases");
					}
				}
				queryResult.addRow(std::move(output));
			}
			return queryResult;
		}

	} // namespace

	QueryResult LogicalPlanMutationRuntime::execute(const logical::LogicalOperator &root,
													const LogicalPlanReadRuntimeBounds &bounds,
													const QueryContext &context) const {
		if (bounds.maximum_rows == 0U) {
			throw std::invalid_argument("logical mutation runtime requires a positive mutation bound");
		}
		if (root.getType() == logical::LogicalOpType::LOP_AGGREGATE) {
			const auto &aggregate = dynamic_cast<const logical::LogicalAggregate &>(root);
			const std::vector<logical::LogicalOperator *> children = aggregate.getChildren();
			const auto &items = aggregate.getAggregations();
			if (children.size() != 1U || children.front() == nullptr || !aggregate.getGroupByExprs().empty() ||
				items.size() != 1U || items.front().functionName != "count" || items.front().argument ||
				items.front().distinct || items.front().extraArg || items.front().alias.empty()) {
				throw std::logic_error(
						"logical mutation runtime currently requires an ungrouped count(*) mutation aggregate");
			}

			const logical::LogicalOperator &mutationRoot = *children.front();
			size_t inputRows = 0U;
			const logical::LogicalOperator *chainRoot = &mutationRoot;
			if (chainRoot->getType() == logical::LogicalOpType::LOP_PROJECT) {
				if (dynamic_cast<const logical::LogicalProject *>(chainRoot) == nullptr) {
					throw std::logic_error("logical mutation aggregate received an invalid Project operator");
				}
				chainRoot = requireOptionalOnlyChild(*chainRoot);
				if (chainRoot == nullptr) {
					throw std::logic_error("logical mutation aggregate Project has no mutation child");
				}
			}
			if (const std::optional<CreateSetMutationChain> chain = collectCreateSetMutationChain(*chainRoot);
				chain.has_value()) {
				QueryResult mutationResult = executeCreateSetMutationChain(*chain, read_backend_, write_backend_,
																		   bounds, context, nullptr, {});
				inputRows = mutationResult.getPipelineRowCount();
			} else {
				QueryResult mutationResult = execute(mutationRoot, bounds, context);
				inputRows = mutationResult.getPipelineRowCount();
			}
			if (inputRows == 0U) {
				// A standalone anonymous CREATE intentionally exposes no columns, but it
				// still transforms the SingleRow seed and therefore contributes one row
				// to a following count(*).
				// Use the already-unwrapped mutation child. An aggregate may sit above a
				// Project that supplies no visible columns for an anonymous CREATE.
				const logical::LogicalOperator *tail = chainRoot;
				while (tail != nullptr && (tail->getType() == logical::LogicalOpType::LOP_CREATE_NODE ||
										   tail->getType() == logical::LogicalOpType::LOP_CREATE_EDGE)) {
					tail = requireOptionalOnlyChild(*tail);
				}
				if (tail != nullptr && tail->getType() == logical::LogicalOpType::LOP_SINGLE_ROW) {
					inputRows = 1U;
				}
			}
			if (inputRows > static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
				throw std::overflow_error("logical mutation count(*) exceeds the Property integer range");
			}
			QueryResult result;
			result.setColumns({items.front().alias});
			QueryResult::Row row;
			row.emplace(items.front().alias, PropertyValue(static_cast<int64_t>(inputRows)));
			result.addRow(std::move(row));
			return result;
		}
		if (std::unique_ptr<logical::LogicalOperator> reboundCall = rebindWriteOnlyCallSubquery(root)) {
			return execute(*reboundCall, bounds, context);
		}
		if (root.getType() == logical::LogicalOpType::LOP_FOREACH) {
			const auto &foreach = dynamic_cast<const logical::LogicalForeach &>(root);
			return executeCreateSetMutationChain(collectForeachMutationChain(foreach), read_backend_, write_backend_,
												 bounds, context, nullptr, visibleMutationOutputColumns(foreach),
												 &foreach);
		}
		const logical::LogicalProject *createSetProjection = nullptr;
		const logical::LogicalOperator *createSetRoot = &root;
		if (root.getType() == logical::LogicalOpType::LOP_PROJECT) {
			createSetProjection = dynamic_cast<const logical::LogicalProject *>(&root);
			if (createSetProjection == nullptr) {
				throw std::logic_error("logical CREATE/SET projection has an invalid Project operator");
			}
			createSetRoot = requireOptionalOnlyChild(root);
			if (createSetRoot == nullptr) {
				throw std::logic_error("logical CREATE/SET projection requires a mutation child");
			}
		}
		if (const std::optional<CreateSetMutationChain> chain = collectCreateSetMutationChain(*createSetRoot);
			chain.has_value()) {
			return executeCreateSetMutationChain(*chain, read_backend_, write_backend_, bounds, context,
												 createSetProjection, visibleMutationOutputColumns(*createSetRoot));
		}
		if (root.getType() == logical::LogicalOpType::LOP_PROJECT) {
			const logical::LogicalOperator *candidate = requireOptionalOnlyChild(root);
			if (candidate != nullptr && candidate->getType() == logical::LogicalOpType::LOP_FOREACH) {
				const auto &foreach = dynamic_cast<const logical::LogicalForeach &>(*candidate);
				return executeCreateSetMutationChain(collectForeachMutationChain(foreach), read_backend_,
													 write_backend_, bounds, context, createSetProjection,
													 visibleMutationOutputColumns(foreach), &foreach);
			}
		}
		const logical::LogicalProject *readMutationProjection = nullptr;
		const logical::LogicalOperator *readMutationRoot = &root;
		if (root.getType() == logical::LogicalOpType::LOP_PROJECT) {
			const logical::LogicalOperator *candidate = requireOptionalOnlyChild(root);
			if (candidate != nullptr && (candidate->getType() == logical::LogicalOpType::LOP_DELETE ||
										 candidate->getType() == logical::LogicalOpType::LOP_SET ||
										 candidate->getType() == logical::LogicalOpType::LOP_REMOVE)) {
				readMutationProjection = dynamic_cast<const logical::LogicalProject *>(&root);
				if (readMutationProjection == nullptr) {
					throw std::logic_error("logical mutation runtime received a malformed Project operator");
				}
				readMutationRoot = candidate;
			}
		}
		if (const std::optional<ReadPropertyMutationChain> chain = collectReadPropertyMutationChain(*readMutationRoot);
			chain.has_value()) {
			constexpr size_t MAX_CONFLICT_RETRIES = 4U;
			for (size_t attempt = 0U; attempt < MAX_CONFLICT_RETRIES; ++attempt) {
				std::unique_ptr<storage::LogicalGraphReadSnapshot> snapshot =
						read_backend_.openLogicalGraphReadSnapshot();
				if (!snapshot) {
					throw std::runtime_error("logical SET/REMOVE chain could not open a read snapshot");
				}
				const std::optional<storage::LogicalGraphMutationFence> fence = snapshot->mutationFence();
				if (!fence.has_value() || !fence->isValid()) {
					throw std::logic_error(
							"logical SET/REMOVE chain requires a backend that exposes an optimistic read fence");
				}

				MutableGraphEntities entities;
				std::vector<LogicalPlanReadRow> projectionRows;
				LogicalPlanReadRuntime reader(read_backend_);
				const LogicalPlanReadRuntimeStats stats = reader.executeWithSnapshot(
						*snapshot, *chain->read_root, bounds,
						[&](const LogicalPlanReadRow &row) {
							context.checkGuard();
							std::unordered_map<std::string, PropertyMap> propertyCache = row.property_cache;
							for (const logical::LogicalOperator *operation: chain->operations) {
								context.checkGuard();
								if (operation->getType() == logical::LogicalOpType::LOP_SET) {
									applySetOperation(dynamic_cast<const logical::LogicalSet &>(*operation), row,
													  *snapshot, bounds, context, entities, propertyCache);
								} else {
									applyRemoveOperation(dynamic_cast<const logical::LogicalRemove &>(*operation), row,
														 *snapshot, bounds, entities, propertyCache);
								}
							}
							LogicalPlanReadRow projected = row;
							projected.property_cache = std::move(propertyCache);
							projectionRows.push_back(std::move(projected));
							return true;
						},
						context);
				requireCompleteReadForMutation(stats, bounds, "logical SET/REMOVE chain");

				storage::LogicalGraphMutationBatch batch;
				for (const auto &[_, entity]: entities) {
					const LogicalGraphMutationEntityReference reference{
							entity.kind, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, entity.id};
					if (entity.properties_changed) {
						if (!entity.properties.has_value()) {
							throw std::logic_error(
									"logical SET/REMOVE chain lost its staged complete Property collection");
						}
						batch.property_replacements.push_back({reference, sortedProperties(*entity.properties)});
					}
					if (entity.labels_changed) {
						batch.node_label_replacements.push_back({reference, entity.labels});
					}
				}
				const auto resolveBinding = [&](const LogicalPlanReadRow &row, const std::string &variable) {
					return materializeMutableBinding(*snapshot, row, variable, entities, bounds);
				};
				const auto buildResult = [&]() {
					if (readMutationProjection != nullptr) {
						return buildMutationProjection(*readMutationProjection, projectionRows, *snapshot, bounds,
													   context, resolveBinding);
					}
					return buildImplicitMutationResult(visibleMutationOutputColumns(*readMutationRoot), projectionRows,
													   context, resolveBinding);
				};
				if (batch.property_replacements.empty() && batch.node_label_replacements.empty()) {
					return buildResult();
				}
				try {
					static_cast<void>(write_backend_.executeLogicalGraphMutationBatch(batch, fence));
					return buildResult();
				} catch (const storage::LogicalGraphMutationConflict &) {
					// Re-read and reapply the ordered chain against the new graph generation.
				}
			}
			throw std::runtime_error("logical SET/REMOVE chain exceeded its optimistic conflict retry limit");
		}
		if (readMutationRoot->getType() == logical::LogicalOpType::LOP_DELETE) {
			const auto &deleteOperator = dynamic_cast<const logical::LogicalDelete &>(*readMutationRoot);
			const logical::LogicalOperator *child = requireOptionalOnlyChild(deleteOperator);
			if (child == nullptr || deleteOperator.getVariables().empty()) {
				throw std::logic_error("logical DELETE requires one read child and at least one entity variable");
			}

			constexpr size_t MAX_CONFLICT_RETRIES = 4U;
			for (size_t attempt = 0U; attempt < MAX_CONFLICT_RETRIES; ++attempt) {
				std::unique_ptr<storage::LogicalGraphReadSnapshot> snapshot =
						read_backend_.openLogicalGraphReadSnapshot();
				if (!snapshot) {
					throw std::runtime_error("logical DELETE could not open a read snapshot");
				}
				const std::optional<storage::LogicalGraphMutationFence> fence = snapshot->mutationFence();
				if (!fence.has_value() || !fence->isValid()) {
					throw std::logic_error("logical DELETE requires a backend that exposes an optimistic read fence");
				}

				std::set<uint64_t> nodeIds;
				std::set<uint64_t> edgeIds;
				std::vector<LogicalPlanReadRow> projectionRows;
				LogicalPlanReadRuntime reader(read_backend_);
				const LogicalPlanReadRuntimeStats stats = reader.executeWithSnapshot(
						*snapshot, *child, bounds,
						[&](const LogicalPlanReadRow &row) {
							context.checkGuard();
							projectionRows.push_back(row);
							for (const std::string &variable: deleteOperator.getVariables()) {
								const auto found = row.values.find(variable);
								if (found == row.values.end()) {
									throw std::logic_error(
											"logical DELETE could not resolve a requested entity variable");
								}
								if (const auto *node = std::get_if<storage::LogicalGraphNode>(&found->second)) {
									nodeIds.insert(node->node_id);
								} else if (const auto *edge = std::get_if<storage::LogicalGraphEdge>(&found->second)) {
									edgeIds.insert(edge->edge_id);
								} else {
									throw std::logic_error("logical DELETE requires Node or Edge bindings");
								}
							}
							return true;
						},
						context);
				// A limited read cannot safely become a write: without a proof that
				// the producer was exhausted, deleting the prefix would be silent data
				// loss relative to the declarative Cypher statement.
				requireCompleteReadForMutation(stats, bounds, "logical DELETE");
				const auto buildDeleteResult = [&, snapshot = snapshot.get()]() -> QueryResult {
					const auto resolve = [&, snapshot](const LogicalPlanReadRow &row, const std::string &variable) {
						return materializeReadBinding(*snapshot, row, variable, bounds);
					};
					if (readMutationProjection != nullptr) {
						return buildMutationProjection(*readMutationProjection, projectionRows, *snapshot, bounds,
													   context, resolve);
					}
					return buildImplicitMutationResult(visibleMutationOutputColumns(*readMutationRoot), projectionRows,
													   context, resolve);
				};
				if (nodeIds.empty() && edgeIds.empty()) {
					return buildDeleteResult();
				}
				// DELETE makes the matched bindings invisible to the transaction overlay.
				// Materialize the statement result from the pre-delete snapshot before
				// publishing that visibility change.
				QueryResult deleteResult = buildDeleteResult();

				storage::LogicalGraphMutationBatch batch;
				batch.edge_deletes.reserve(edgeIds.size());
				for (const uint64_t edgeId: edgeIds) {
					batch.edge_deletes.push_back({edgeId});
				}
				batch.node_deletes.reserve(nodeIds.size());
				for (const uint64_t nodeId: nodeIds) {
					batch.node_deletes.push_back({nodeId, deleteOperator.isDetach(),
												  deleteOperator.isDetach() ? bounds.maximum_edges_per_node : 0U});
				}
				try {
					static_cast<void>(write_backend_.executeLogicalGraphMutationBatch(batch, fence));
					return deleteResult;
				} catch (const storage::LogicalGraphMutationConflict &) {
					// Re-open the snapshot and re-evaluate the full read plan. Never reuse
					// a stale entity set after a concurrent graph publication.
				}
			}
			throw std::runtime_error("logical DELETE exceeded its optimistic conflict retry limit");
		}
		const logical::LogicalProject *projection = nullptr;
		const logical::LogicalOperator *mutationRoot = &root;
		if (root.getType() == logical::LogicalOpType::LOP_PROJECT) {
			projection = dynamic_cast<const logical::LogicalProject *>(&root);
			if (projection == nullptr) {
				throw std::logic_error("logical mutation runtime received a malformed Project operator");
			}
			mutationRoot = requireOptionalOnlyChild(root);
			if (mutationRoot == nullptr) {
				throw std::logic_error("logical mutation projection requires a mutation child");
			}
		}
		if (const std::optional<MergePostMutationChain> mergeChain = collectMergePostMutationChain(*mutationRoot);
			mergeChain.has_value()) {
			constexpr size_t MAX_CONFLICT_RETRIES = 4U;
			for (size_t attempt = 0U; attempt < MAX_CONFLICT_RETRIES; ++attempt) {
				std::unique_ptr<storage::LogicalGraphReadSnapshot> snapshot =
						read_backend_.openLogicalGraphReadSnapshot();
				if (!snapshot) {
					throw std::runtime_error("logical MERGE could not open a read snapshot");
				}
				const std::optional<storage::LogicalGraphMutationFence> fence = snapshot->mutationFence();
				if (!fence.has_value() || !fence->isValid()) {
					throw std::logic_error("logical MERGE requires a backend that exposes an optimistic read fence");
				}
				LogicalPlanReadRuntime reader(read_backend_);
				MergeExecutionState state{*snapshot, bounds, context, reader};
				std::vector<MergeRuntimeRow> rows;
				const bool completed = visitMergeOperator(*mergeChain->merge_root, state, [&](MergeRuntimeRow &&row) {
					applyMergePostMutations(mergeChain->operations, row, state);
					if (rows.size() == bounds.maximum_rows) {
						throw std::runtime_error("logical MERGE exceeded its row bound");
					}
					rows.push_back(std::move(row));
					return true;
				});
				if (!completed) {
					throw std::runtime_error("logical MERGE did not complete its input evaluation");
				}
				appendModifiedMergeEntities(state);
				synchronizeCreatedMergeEntities(state);
				try {
					const storage::LogicalGraphMutationBatchResult result =
							mergeBatchHasMutations(state.batch)
									? write_backend_.executeLogicalGraphMutationBatch(state.batch, fence)
									: storage::LogicalGraphMutationBatchResult{};
					return buildMergeResult(projection, visibleMutationOutputColumns(*mutationRoot), rows, state,
											result);
				} catch (const storage::LogicalGraphMutationConflict &) {
					// A MERGE decision is snapshot-dependent. Re-evaluate all match and
					// creation choices rather than replaying a stale batch after a writer.
				}
			}
			throw std::runtime_error("logical MERGE exceeded its optimistic conflict retry limit");
		}

		throw std::logic_error("logical mutation runtime received an unsupported mutation plan");
	}
} // namespace graph::query::execution
