/**
 * @file LogicalPlanOptimizer.cpp
 * @brief Storage-neutral logical-plan normalization rules.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#include "graph/query/optimization/LogicalPlanOptimizer.hpp"
#include "graph/query/optimization/ReadTraversalAnchorEnvelope.hpp"
#include "graph/query/optimization/ReadJoinReorderEnvelope.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <utility>

#include "graph/query/expressions/Expression.hpp"
#include "graph/query/logical/operators/LogicalFilter.hpp"
#include "graph/query/logical/operators/LogicalJoin.hpp"
#include "graph/query/logical/operators/LogicalLimit.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/logical/operators/LogicalProject.hpp"
#include "graph/query/logical/operators/LogicalSkip.hpp"
#include "graph/query/logical/operators/LogicalTraversal.hpp"

namespace graph::query::optimization {

	namespace {

		[[nodiscard]] std::optional<PropertyValue>
		literalValueForExactNodeLookup(const expressions::LiteralExpression &literal) {
			if (literal.isNull()) {
				// Cypher equality with NULL never selects a row. Keep the Filter as the
				// sole semantic authority instead of inventing a storage-specific miss.
				return std::nullopt;
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
			if (literal.isString()) {
				return PropertyValue(literal.getStringValue());
			}
			// LiteralExpression owns a closed representation set. After the scalar
			// variants above, the remaining representation is PropertyValue.
			return literal.getPropertyValue();
		}

		void collectConjuncts(const expressions::Expression &expression,
							  std::vector<const expressions::Expression *> &conjuncts) {
			const auto *binary = dynamic_cast<const expressions::BinaryOpExpression *>(&expression);
			if (binary != nullptr && binary->getOperator() == expressions::BinaryOperatorType::BOP_AND &&
				binary->getLeft() != nullptr && binary->getRight() != nullptr) {
				collectConjuncts(*binary->getLeft(), conjuncts);
				collectConjuncts(*binary->getRight(), conjuncts);
				return;
			}
			conjuncts.push_back(&expression);
		}

		[[nodiscard]] bool pushExactLiteralFiltersIntoNodeScan(logical::LogicalFilter &filter) {
			const auto &predicate = filter.getPredicate();
			if (!predicate) {
				return false;
			}
			const std::vector<logical::LogicalOperator *> children = filter.getChildren();
			if (children.size() != 1U || children.front() == nullptr ||
				children.front()->getType() != logical::LogicalOpType::LOP_NODE_SCAN) {
				return false;
			}
			auto *scan = dynamic_cast<logical::LogicalNodeScan *>(children.front());
			if (scan == nullptr || scan->getVariable().empty()) {
				return false;
			}

			std::vector<std::pair<std::string, PropertyValue>> predicates = scan->getPropertyPredicates();
			std::unordered_set<std::string> occupiedKeys;
			occupiedKeys.reserve(predicates.size() + scan->getParameterPropertyPredicates().size());
			for (const auto &[key, value]: predicates) {
				static_cast<void>(value);
				if (key.empty() || !occupiedKeys.insert(key).second) {
					// Leave an invalid pre-existing plan unchanged. The runtime remains the
					// semantic authority and will reject it at its normal validation point.
					return false;
				}
			}
			for (const logical::ParameterPropertyPredicate &parameter: scan->getParameterPropertyPredicates()) {
				if (parameter.key.empty() || !occupiedKeys.insert(parameter.key).second) {
					return false;
				}
			}

			std::vector<const expressions::Expression *> conjuncts;
			collectConjuncts(*predicate, conjuncts);
			bool changed = false;
			for (const expressions::Expression *conjunct: conjuncts) {
				const auto *comparison = dynamic_cast<const expressions::BinaryOpExpression *>(conjunct);
				if (comparison == nullptr || comparison->getOperator() != expressions::BinaryOperatorType::BOP_EQUAL ||
					comparison->getLeft() == nullptr || comparison->getRight() == nullptr) {
					continue;
				}
				const auto *property =
						dynamic_cast<const expressions::VariableReferenceExpression *>(comparison->getLeft());
				const auto *literal = dynamic_cast<const expressions::LiteralExpression *>(comparison->getRight());
				if (property == nullptr || !property->hasProperty() ||
					property->getVariableName() != scan->getVariable() || property->getPropertyName().empty() ||
					literal == nullptr || !occupiedKeys.insert(property->getPropertyName()).second) {
					continue;
				}
				const std::optional<PropertyValue> value = literalValueForExactNodeLookup(*literal);
				if (!value.has_value() || value->getType() == PropertyType::NULL_TYPE) {
					occupiedKeys.erase(property->getPropertyName());
					continue;
				}
				predicates.emplace_back(property->getPropertyName(), *value);
				changed = true;
			}
			if (!changed) {
				return false;
			}
			scan->setPropertyPredicates(std::move(predicates));
			scan->setPreferredScanHint(logical::LogicalScanHint::LSH_PROPERTY);
			return true;
		}

		[[nodiscard]] bool pushExactFiltersIntoIndependentJoinScans(logical::LogicalFilter &filter,
			const LogicalStatisticsProvider *statistics) {
			if (!filter.getPredicate()) return false;
			const auto children = filter.getChildren();
			if (children.size() != 1U) return false;
			auto *join = dynamic_cast<logical::LogicalJoin *>(children.front());
			if (!join) return false;
			const auto *left = dynamic_cast<const logical::LogicalNodeScan *>(join->getLeft());
			const auto *right = dynamic_cast<const logical::LogicalNodeScan *>(join->getRight());
			if (!left || !right || left->getVariable().empty() || right->getVariable().empty() ||
				left->getVariable() == right->getVariable()) return false;
			for (const auto *scan : {left, right}) {
				// Keep constrained/invalid input plans on their existing validation
				// path. Revisited rewritten scans also stop here, making this idempotent.
				if (!scan->getPropertyPredicates().empty() || !scan->getParameterPropertyPredicates().empty() ||
					!scan->getRangePredicates().empty() || scan->getCompositeEquality() ||
					(scan->getPreferredScanHint() != logical::LogicalScanHint::LSH_FULL &&
					 scan->getPreferredScanHint() != logical::LogicalScanHint::LSH_LABEL)) return false;
				for (const auto &label : scan->getLabels()) if (label.empty()) return false;
			}
			std::vector<const expressions::Expression *> conjuncts;
			collectConjuncts(*filter.getPredicate(), conjuncts);
			for (const auto *conjunct : conjuncts) {
				const auto *comparison = dynamic_cast<const expressions::BinaryOpExpression *>(conjunct);
				if (!comparison || comparison->getOperator() != expressions::BinaryOperatorType::BOP_EQUAL) return false;
				const auto *property = dynamic_cast<const expressions::VariableReferenceExpression *>(comparison->getLeft());
				const auto *literal = dynamic_cast<const expressions::LiteralExpression *>(comparison->getRight());
				if (!property || !property->hasProperty() || property->getPropertyName().empty() || !literal ||
					(property->getVariableName() != left->getVariable() && property->getVariableName() != right->getVariable())) return false;
				const auto value = literalValueForExactNodeLookup(*literal);
				if (!value || value->getType() == PropertyType::NULL_TYPE) return false;
			}
			// Every conjunct is a deterministic exact lookup on one independent
			// scan. Keep the original outer Filter as the semantic authority.
			bool changed = false;
			for (size_t side = 0U; side < 2U; ++side) {
				const auto *input = side == 0U ? join->getLeft() : join->getRight();
				if (statistics) {
					const auto *opposite = side == 0U ? join->getRight() : join->getLeft();
					const auto estimate = statistics->estimateJoinInput(*opposite);
					// With no estimated repetition, keep the existing batched Filter
					// rather than pay an additional scan-predicate evaluation path.
					if (estimate && estimate->isValid() && estimate->expected_rows <= 1U) continue;
				}
				logical::LogicalFilter candidate(input->clone(), filter.getPredicate());
				if (pushExactLiteralFiltersIntoNodeScan(candidate)) {
					join->setChild(side, candidate.detachChild(0U));
					changed = true;
				}
			}
			return changed;
		}

		[[nodiscard]] bool pushDirectLiteralRangeFilterIntoNodeScan(logical::LogicalFilter &filter) {
			const auto &predicate = filter.getPredicate();
			if (!predicate) {
				return false;
			}
			const std::vector<logical::LogicalOperator *> children = filter.getChildren();
			if (children.size() != 1U || children.front() == nullptr ||
				children.front()->getType() != logical::LogicalOpType::LOP_NODE_SCAN) {
				return false;
			}
			auto *scan = dynamic_cast<logical::LogicalNodeScan *>(children.front());
			if (scan == nullptr || scan->getVariable().empty() || !scan->getParameterPropertyPredicates().empty() ||
				!scan->getRangePredicates().empty() ||
				scan->getCompositeEquality().has_value()) {
				return false;
			}

			std::vector<const expressions::Expression *> conjuncts;
			collectConjuncts(*predicate, conjuncts);
			std::optional<logical::RangePredicate> range;
			for (const expressions::Expression *conjunct: conjuncts) {
				const auto *comparison = dynamic_cast<const expressions::BinaryOpExpression *>(conjunct);
				if (comparison == nullptr || comparison->getLeft() == nullptr || comparison->getRight() == nullptr) {
					continue;
				}
				const auto *property =
						dynamic_cast<const expressions::VariableReferenceExpression *>(comparison->getLeft());
				const auto *literal = dynamic_cast<const expressions::LiteralExpression *>(comparison->getRight());
				if (property == nullptr || !property->hasProperty() || property->getVariableName() != scan->getVariable() ||
					property->getPropertyName().empty() || literal == nullptr) {
					continue;
				}
				const std::optional<PropertyValue> value = literalValueForExactNodeLookup(*literal);
				if (!value.has_value() || value->getType() == PropertyType::NULL_TYPE) {
					continue;
				}

				const bool isLower = comparison->getOperator() == expressions::BinaryOperatorType::BOP_GREATER ||
									 comparison->getOperator() == expressions::BinaryOperatorType::BOP_GREATER_EQUAL;
				const bool isUpper = comparison->getOperator() == expressions::BinaryOperatorType::BOP_LESS ||
									 comparison->getOperator() == expressions::BinaryOperatorType::BOP_LESS_EQUAL;
				if (!isLower && !isUpper) {
					continue;
				}
				const bool inclusive = comparison->getOperator() == expressions::BinaryOperatorType::BOP_GREATER_EQUAL ||
									   comparison->getOperator() == expressions::BinaryOperatorType::BOP_LESS_EQUAL;
				if (!range.has_value()) {
					range = logical::RangePredicate{property->getPropertyName(), {}, {}, true, true};
				} else if (range->key != property->getPropertyName()) {
					// The native range boundary supports one property only. Keeping the
					// Filter is insufficient here because choosing one of two ranges would
					// make the resulting scan shape depend on conjunct ordering.
					return false;
				}
				if (std::ranges::any_of(scan->getPropertyPredicates(), [&property](const auto &predicate) {
					return predicate.first == property->getPropertyName();
				})) {
					// A property cannot simultaneously be an equality-prefix component
					// and the trailing range component of one composite index.
					return false;
				}
				if (isLower) {
					if (range->minValue.getType() == PropertyType::NULL_TYPE || *value > range->minValue ||
						(*value == range->minValue && !inclusive && range->minInclusive)) {
						range->minValue = *value;
						range->minInclusive = inclusive;
					}
				} else if (range->maxValue.getType() == PropertyType::NULL_TYPE || *value < range->maxValue ||
						   (*value == range->maxValue && !inclusive && range->maxInclusive)) {
					range->maxValue = *value;
					range->maxInclusive = inclusive;
				}
			}
			if (!range.has_value()) {
				return false;
			}
			scan->setRangePredicates({std::move(*range)});
			scan->setPreferredScanHint(logical::LogicalScanHint::LSH_RANGE);
			return true;
		}

	} // namespace

	LogicalPlanOptimizer::LogicalPlanOptimizer(const LogicalOptimizerOptions options) : options_(options) {
		if (options_.maximum_rewrite_passes == 0U || options_.maximum_plan_depth == 0U) {
			throw std::invalid_argument("logical optimizer limits must be positive");
		}
	}

	LogicalOptimizationResult LogicalPlanOptimizer::optimize(std::unique_ptr<logical::LogicalOperator> root,
															 const LogicalStatisticsProvider *statistics) const {
		LogicalOptimizationResult result;
		if (!root) {
			return result;
		}

		if (statistics != nullptr) {
			result.root_cardinality = statistics->estimate(*root);
			if (result.root_cardinality.has_value() && !result.root_cardinality->isValid()) {
				throw std::invalid_argument("logical statistics provider returned an invalid cardinality estimate");
			}
		}

		for (size_t pass = 0U; pass < options_.maximum_rewrite_passes; ++pass) {
			RewriteState state{.statistics = statistics};
			root = rewrite(std::move(root), 0U, state);
			result.rewrite_count += state.rewrite_count;
			if (!state.changed) {
				result.root = std::move(root);
				return result;
			}
		}

		throw std::logic_error("logical optimizer did not reach a fixed point within its rewrite bound");
	}

	std::unique_ptr<logical::LogicalOperator>
	LogicalPlanOptimizer::rewrite(std::unique_ptr<logical::LogicalOperator> root, const size_t depth,
									  RewriteState &state, const bool allowAnchorReordering, const bool allowJoinReordering) const {
		if (!root) {
			throw std::logic_error("logical optimizer received a null operator");
		}
		if (depth >= options_.maximum_plan_depth) {
			throw std::length_error("logical optimizer exceeded its plan-depth bound");
		}

		const std::vector<logical::LogicalOperator *> children = root->getChildren();
		// Expose safe scan predicates before descendants make cost decisions.
		// Postorder alone would choose JOIN sides using unfiltered label counts.
		if (auto *filter = dynamic_cast<logical::LogicalFilter *>(root.get());
			filter && children.size() == 1U && dynamic_cast<logical::LogicalJoin *>(children.front()) &&
			pushExactFiltersIntoIndependentJoinScans(*filter, state.statistics)) {
			++state.rewrite_count;
			state.changed = true;
		}

        const bool transparentAnchor = allowAnchorReordering && detail::readTraversalAnchorEnvelope(*root);
        const bool transparentJoin = allowJoinReordering && detail::readJoinReorderEnvelope(*root);
		for (size_t index = 0U; index < children.size(); ++index) {
			if (children[index] == nullptr) {
				throw std::logic_error("logical optimizer received an operator with a null child");
			}
			auto child = root->detachChild(index);
			root->setChild(index, rewrite(std::move(child), depth + 1U, state, transparentAnchor, transparentJoin));
		}

		if (root->getType() == logical::LogicalOpType::LOP_FILTER) {
			auto *filter = dynamic_cast<logical::LogicalFilter *>(root.get());
			if (filter == nullptr) {
				throw std::logic_error("logical FILTER type did not match its operator class");
			}
			if (pushExactLiteralFiltersIntoNodeScan(*filter) || pushDirectLiteralRangeFilterIntoNodeScan(*filter) ||
				pushExactFiltersIntoIndependentJoinScans(*filter, state.statistics)) {
				++state.rewrite_count;
				state.changed = true;
			}
			return root;
		}

		if (options_.allow_read_traversal_anchor && state.statistics && transparentAnchor) {
			// An explicit projection preserves the public column order when the
			// internal traversal bindings are produced from the opposite endpoint.
			auto *project = dynamic_cast<logical::LogicalProject *>(root.get());
			const auto projected = project ? project->getChildren() : std::vector<logical::LogicalOperator *>{};
			auto *traversal = projected.size() == 1U ? dynamic_cast<logical::LogicalTraversal *>(projected.front()) : nullptr;
			const auto inputs = traversal ? traversal->getChildren() : std::vector<logical::LogicalOperator *>{};
			const auto *scan = inputs.size() == 1U ? dynamic_cast<const logical::LogicalNodeScan *>(inputs.front()) : nullptr;
			if (scan && !scan->getVariable().empty() && scan->getVariable() == traversal->getSourceVar() &&
				scan->getPropertyPredicates().empty() && scan->getParameterPropertyPredicates().empty() &&
				scan->getRangePredicates().empty() && !scan->getCompositeEquality() && scan->getLabels().size() <= 1U &&
				(scan->getLabels().empty() || !scan->getLabels().front().empty()) &&
				(scan->getPreferredScanHint() == logical::LogicalScanHint::LSH_FULL ||
				 scan->getPreferredScanHint() == logical::LogicalScanHint::LSH_LABEL) && !traversal->getTargetVar().empty() &&
				scan->getVariable() != traversal->getTargetVar() &&
				(traversal->getEdgeVar().empty() || (traversal->getEdgeVar() != scan->getVariable() &&
				 traversal->getEdgeVar() != traversal->getTargetVar())) &&
				traversal->getTargetProperties().empty() && traversal->getTargetLabels().size() == 1U &&
				!traversal->getTargetLabels().front().empty() &&
				(traversal->getDirection() == "both" || (!scan->getLabels().empty() &&
				 (traversal->getDirection() == "out" || traversal->getDirection() == "in")))) {
				logical::LogicalNodeScan target(traversal->getTargetVar(), traversal->getTargetLabels());
				const auto sourceCount = state.statistics->estimateAnchorScan(*scan);
				const auto targetCount = state.statistics->estimateAnchorScan(target);
				logical::LogicalNodeScan universe(scan->getVariable());
				const auto universeCount = scan->getLabels().empty() ? sourceCount : state.statistics->estimateAnchorScan(universe);
				// Label counts alone cannot bound incident-edge work: a small
				// target label may contain hubs connected to unrelated source labels.
				// If the original source covers every node, the reversed adjacency
				// is a subset of the original edge work in all supported directions.
				if (sourceCount && targetCount && universeCount && *sourceCount == *universeCount &&
					*targetCount < *sourceCount) {
					const auto direction = traversal->getDirection() == "out" ? "in" :
						traversal->getDirection() == "in" ? "out" : "both";
					project->setChild(0U, std::make_unique<logical::LogicalTraversal>(target.clone(),
						traversal->getTargetVar(), traversal->getEdgeVar(), traversal->getSourceVar(),
						traversal->getEdgeType(), direction, scan->getLabels(), scan->getPropertyPredicates(),
						traversal->getEdgeProperties()));
					++state.rewrite_count;
					state.changed = true;
				}
			}
		}

		if (root->getType() == logical::LogicalOpType::LOP_JOIN) {
			auto *join = dynamic_cast<logical::LogicalJoin *>(root.get());
			if (join == nullptr) {
				throw std::logic_error("logical JOIN type did not match its operator class");
			}
			const logical::LogicalOperator *left = join->getLeft();
			const logical::LogicalOperator *right = join->getRight();
			if (left == nullptr || right == nullptr) {
				throw std::logic_error("logical JOIN has a null child");
			}
			if (state.statistics != nullptr && allowJoinReordering) {
				const std::optional<LogicalCardinalityEstimate> leftEstimate = state.statistics->estimateJoinInput(*left);
				const std::optional<LogicalCardinalityEstimate> rightEstimate = state.statistics->estimateJoinInput(*right);
				// The runtime materializes its right child. Reorder only on a strict,
				// validated estimate, preserving deterministic plans on ties or gaps.

                bool preferLeft = leftEstimate && rightEstimate && leftEstimate->isValid() && rightEstimate->isValid() &&
                    leftEstimate->expected_rows < rightEstimate->expected_rows;
                if (leftEstimate && rightEstimate && leftEstimate->isValid() && rightEstimate->isValid()) {
                    const auto leftBytes = state.statistics->estimateJoinMaterializedBytes(*left);
                    const auto rightBytes = state.statistics->estimateJoinMaterializedBytes(*right);
                    if (leftBytes && rightBytes) preferLeft = *leftBytes < *rightBytes;
                }
                if (preferLeft) {
					auto formerLeft = join->detachChild(0U);
					auto formerRight = join->detachChild(1U);
					++state.rewrite_count;
					state.changed = true;
					return std::make_unique<logical::LogicalJoin>(std::move(formerRight), std::move(formerLeft));
				}
			}
			return root;
		}

		if (root->getType() == logical::LogicalOpType::LOP_LIMIT) {
			auto *outer = dynamic_cast<logical::LogicalLimit *>(root.get());
			if (outer == nullptr) {
				throw std::logic_error("logical LIMIT type did not match its operator class");
			}
			auto child = outer->detachChild(0U);
			if (!child) {
				throw std::logic_error("logical LIMIT has no child");
			}
			if (child->getType() == logical::LogicalOpType::LOP_PROJECT) {
				auto *project = dynamic_cast<logical::LogicalProject *>(child.get());
				if (project == nullptr) {
					throw std::logic_error("logical PROJECT type did not match its operator class");
				}
				const bool hasValidItems =
						std::ranges::all_of(project->getItems(), [](const logical::LogicalProjectItem &item) {
							return item.expression != nullptr && !item.alias.empty();
						});
				if (outer->getLimit() >= 0 && !project->isDistinct() && hasValidItems) {
					auto input = project->detachChild(0U);
					if (!input) {
						throw std::logic_error("logical PROJECT has no child");
					}
					utils::OwnedStdVector<logical::LogicalProjectItem> items = project->getItems();
					++state.rewrite_count;
					state.changed = true;
					return std::make_unique<logical::LogicalProject>(
							std::make_unique<logical::LogicalLimit>(std::move(input), outer->getLimit()),
							std::move(items), false);
				}
			}
			if (child->getType() == logical::LogicalOpType::LOP_SKIP) {
				auto *skip = dynamic_cast<logical::LogicalSkip *>(child.get());
				if (skip == nullptr) {
					throw std::logic_error("logical SKIP type did not match its operator class");
				}
				const int64_t limit = outer->getLimit();
				const int64_t offset = skip->getOffset();
				// LIMIT 0 deliberately short-circuits without visiting its child. Negative
				// and overflowing windows remain the runtime's validation responsibility.
				if (limit > 0 && offset >= 0 && limit <= std::numeric_limits<int64_t>::max() - offset) {
					auto base = skip->detachChild(0U);
					if (!base) {
						throw std::logic_error("logical SKIP has no child");
					}
					++state.rewrite_count;
					state.changed = true;
					return std::make_unique<logical::LogicalSkip>(
							std::make_unique<logical::LogicalLimit>(std::move(base), limit + offset), offset);
				}
			}
			if (child->getType() == logical::LogicalOpType::LOP_LIMIT) {
				// Children are recursively validated before this rule observes them.
				auto *inner = static_cast<logical::LogicalLimit *>(child.get());
				if (outer->getLimit() >= 0 && inner->getLimit() >= 0) {
					auto base = inner->detachChild(0U);
					if (!base) {
						throw std::logic_error("nested logical LIMIT has no child");
					}
					++state.rewrite_count;
					state.changed = true;
					return std::make_unique<logical::LogicalLimit>(std::move(base),
																   std::min(outer->getLimit(), inner->getLimit()));
				}
			}
			outer->setChild(0U, std::move(child));
			return root;
		}

		if (root->getType() == logical::LogicalOpType::LOP_SKIP) {
			auto *outer = dynamic_cast<logical::LogicalSkip *>(root.get());
			if (outer == nullptr) {
				throw std::logic_error("logical SKIP type did not match its operator class");
			}
			auto child = outer->detachChild(0U);
			if (!child) {
				throw std::logic_error("logical SKIP has no child");
			}
			if (child->getType() == logical::LogicalOpType::LOP_SKIP) {
				// Children are recursively validated before this rule observes them.
				auto *inner = static_cast<logical::LogicalSkip *>(child.get());
				const int64_t outerOffset = outer->getOffset();
				const int64_t innerOffset = inner->getOffset();
				if (outerOffset >= 0 && innerOffset >= 0 &&
					outerOffset <= std::numeric_limits<int64_t>::max() - innerOffset) {
					auto base = inner->detachChild(0U);
					if (!base) {
						throw std::logic_error("nested logical SKIP has no child");
					}
					++state.rewrite_count;
					state.changed = true;
					return std::make_unique<logical::LogicalSkip>(std::move(base), outerOffset + innerOffset);
				}
			}
			outer->setChild(0U, std::move(child));
		}

		return root;
	}

} // namespace graph::query::optimization
