/**
 * @file LogicalNodeScan.hpp
 * @brief Logical operator for scanning nodes from the graph.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once
#include "graph/utils/OwnedStringVector.hpp"

#include "graph/query/logical/LogicalOperator.hpp"
#include "graph/query/logical/LogicalScanHint.hpp"
#include "graph/core/PropertyTypes.hpp"

#include "graph/core/OwnedPropertyVector.hpp"
#include "graph/core/OwnedPropertyPredicates.hpp"
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include "graph/utils/OwnedStdString.hpp"

namespace graph::query::logical {

/**
 * @brief Describes a range predicate on a single property (e.g. age > 18 AND age < 65).
 */
struct RangePredicate {
    std::string key;
    graph::PropertyValue minValue;     // monostate = no lower bound
    graph::PropertyValue maxValue;     // monostate = no upper bound
    bool minInclusive = true;
    bool maxInclusive = true;
};

/**
 * @brief Describes a composite equality predicate (e.g. n.name = 'Alice' AND n.age = 30).
 */
struct CompositeEqualityPredicate {
    utils::OwnedStringVector keys;
    graph::OwnedPropertyVector values;
};

/**
 * @brief Equality predicate whose value is resolved from QueryContext at execution time.
 */
struct ParameterPropertyPredicate {
    std::string key;
    std::string parameter_name;
};

struct ParameterPredicateCopyPolicy {
    static size_t copyBytes(const ParameterPropertyPredicate &value) {
        return utils::detail::addStandardHeapBytes(utils::detail::standardFreshStringHeapUpper(value.key.size()),
            utils::detail::standardFreshStringHeapUpper(value.parameter_name.size()));
    }
};
struct RangePredicateCopyPolicy {
    static size_t copyBytes(const RangePredicate &value) {
        return utils::detail::addStandardHeapBytes(utils::detail::standardFreshStringHeapUpper(value.key.size()),
            utils::detail::addStandardHeapBytes(graph::property_heap::copyRequestUpper(value.minValue),
                graph::property_heap::copyRequestUpper(value.maxValue)));
    }
};
using OwnedParameterPredicates = utils::OwnedCopyVector<ParameterPropertyPredicate, ParameterPredicateCopyPolicy>;
using OwnedRangePredicates = utils::OwnedCopyVector<RangePredicate, RangePredicateCopyPolicy>;

/**
 * @class LogicalNodeScan
 * @brief Leaf operator that scans nodes matching a variable, optional labels,
 *        and optional property predicates.
 *
 * Logical-plan transformations may enrich this operator with additional
 * property predicates before the storage-neutral runtime evaluates it.
 */
class LogicalNodeScan : public LogicalOperator {
public:
    explicit LogicalNodeScan(utils::OwnedStdString variable,
                             utils::OwnedStringVector labels = {},
                             OwnedPropertyPredicates propertyPredicates = {})
        : variable_(std::move(variable))
        , labels_(std::move(labels))
        , propertyPredicates_(std::move(propertyPredicates)) {}

    // -------------------------------------------------------------------------
    // LogicalOperator interface
    // -------------------------------------------------------------------------

    [[nodiscard]] LogicalOpType getType() const override {
        return LogicalOpType::LOP_NODE_SCAN;
    }

    [[nodiscard]] std::vector<LogicalOperator *> getChildren() const override {
        return {};
    }

    [[nodiscard]] std::vector<std::string> getOutputVariables() const override {
        return {variable_};
    }

    [[nodiscard]] utils::OwnedStringVector getOwnedOutputVariables() const override {
        utils::OwnedStringVector vars; vars.push_back(variable_); return vars;
    }

    [[nodiscard]] std::string toString() const override {
        std::string result = "NodeScan(" + variable_;
        if (!labels_.empty()) {
            result += ":";
            for (size_t i = 0; i < labels_.size(); ++i) {
                result += labels_[i];
                if (i + 1 < labels_.size()) result += ":";
            }
        }
        result += ")";
        return result;
    }

    [[nodiscard]] std::unique_ptr<LogicalOperator> clone() const override {
        auto cloned = std::make_unique<LogicalNodeScan>(variable_, labels_, propertyPredicates_);
        cloned->rangePredicates_ = rangePredicates_;
        cloned->compositeEquality_ = compositeEquality_;
        cloned->parameterPropertyPredicates_ = parameterPropertyPredicates_;
        cloned->preferred_scan_hint_ = preferred_scan_hint_;
        return cloned;
    }

    void setChild(size_t /*index*/, std::unique_ptr<LogicalOperator> /*child*/) override {
        throw std::logic_error("LogicalNodeScan is a leaf node and does not accept children");
    }

    std::unique_ptr<LogicalOperator> detachChild(size_t /*index*/) override {
        throw std::logic_error("LogicalNodeScan is a leaf node and has no children to detach");
    }

    // -------------------------------------------------------------------------
    // Accessors
    // -------------------------------------------------------------------------

    [[nodiscard]] const std::string &getVariable() const { return variable_; }
    [[nodiscard]] const std::vector<std::string> &getLabels() const { return labels_; }
    [[nodiscard]] const std::vector<std::pair<std::string, graph::PropertyValue>> &getPropertyPredicates() const {
        return propertyPredicates_;
    }
    [[nodiscard]] const std::vector<ParameterPropertyPredicate> &getParameterPropertyPredicates() const {
        return parameterPropertyPredicates_;
    }

    // -------------------------------------------------------------------------
    // Mutators for logical-plan transformations.
    // -------------------------------------------------------------------------

    void setPropertyPredicates(OwnedPropertyPredicates predicates) {
        propertyPredicates_ = std::move(predicates);
    }
    void setParameterPropertyPredicates(OwnedParameterPredicates predicates) {
        parameterPropertyPredicates_ = std::move(predicates);
    }

    // Range predicates
    [[nodiscard]] const std::vector<RangePredicate> &getRangePredicates() const {
        return rangePredicates_;
    }
    void setRangePredicates(OwnedRangePredicates predicates) {
        rangePredicates_ = std::move(predicates);
    }

    // Composite equality predicate
    [[nodiscard]] const std::optional<CompositeEqualityPredicate> &getCompositeEquality() const {
        return compositeEquality_;
    }
    void setCompositeEquality(CompositeEqualityPredicate pred) {
        if (compositeEquality_) {
            compositeEquality_->keys.swap(pred.keys);
            compositeEquality_->values.swap(pred.values);
        } else {
            compositeEquality_.emplace(std::move(pred));
        }
    }

    /** @brief Sets an optional storage-neutral scan preference. */
    void setPreferredScanHint(LogicalScanHint hint) {
        preferred_scan_hint_ = hint;
    }

    /** @brief Returns the optional scan preference (default: full scan). */
    [[nodiscard]] LogicalScanHint getPreferredScanHint() const {
        return preferred_scan_hint_;
    }

private:
    utils::OwnedStdString variable_;
    utils::OwnedStringVector labels_;
    OwnedPropertyPredicates propertyPredicates_;
    OwnedParameterPredicates parameterPropertyPredicates_;
    OwnedRangePredicates rangePredicates_;
    std::optional<CompositeEqualityPredicate> compositeEquality_;
    LogicalScanHint preferred_scan_hint_ = LogicalScanHint::LSH_FULL;
};

} // namespace graph::query::logical
